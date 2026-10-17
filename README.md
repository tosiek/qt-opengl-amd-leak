# AMD `WGL_NV_DX_interop` leak: unregistered D3D11 textures are never freed

On AMD Radeon graphics (Windows), a Direct3D 11 texture that was registered
with `wglDXRegisterObjectNV` is **never freed** after
`wglDXUnregisterObjectNV` and its last `Release()`, even after
`ID3D11DeviceContext::Flush()`. One kernel Event handle leaks with it.

## Quick test (no build)

Download [amd-interop-leak-test.zip](https://github.com/tosiek/qt-opengl-amd-leak/releases/download/v1/amd-interop-leak-test.zip)
(Windows 10/11 x64), unzip it and double-click `test.bat`. It takes about 15
seconds and saves the output to `results.txt`. `register` growing by about 3.5 MB and
1 handle per frame, while `reuse` and `nointerop` stay flat, means your driver
is affected.

The exe is built with MSVC 14.44 (Visual Studio 2022 17.14) and needs the
Visual C++ 2015-2022 Redistributable (x64) 14.44 or newer. The two runtime
DLLs (`vcruntime140.dll`, `vcruntime140_1.dll`, 14.44.35211.0) are in the zip,
so nothing needs to be installed.

## Repro

[`interop_leak.cpp`](interop_leak.cpp) (~150 lines, D3D11 + OpenGL only).
Each frame it does the following:

1. creates a 1280x720 BGRA D3D11 texture
2. calls `wglDXRegisterObjectNV`, `wglDXLockObjectsNV` and `wglDXUnlockObjectsNV`
3. calls `wglDXUnregisterObjectNV`
4. calls `Release()` and `Flush()`

Build in an "x64 Native Tools Command Prompt for VS 2022":

```
build.bat
test.bat                         REM runs the three modes below (75 frames each), saves results.txt
```

Or run one mode with `interop_leak.exe <mode> [frames]`:

- `register`: leaks
- `reuse`: the same texture registered once; flat
- `nointerop`: the same create/release with no WGL interop; flat

## Results

AMD Radeon Vega (Ryzen 7 PRO 5850U, `VEN_1002&DEV_1638`), driver
31.0.21925.1001, OpenGL `4.6.0 Compatibility Profile Context 26.5.2.260413`,
Windows 11 24H2. Private bytes and handle count after 75 frames:

| Mode | Per frame | Memory | Handles |
|---|---|---|---|
| `register` | new texture, register, lock/unlock, unregister, release, flush | **+288 MB** | **+79** |
| `reuse` | one texture registered once; lock/unlock | 0 MB | +3 |
| `nointerop` | new texture, release, flush; no interop | +1 MB | +3 |

Each frame leaks about 3.8 MB (one 1280x720 frame plus overhead) and 1 handle.
Driver 31.0.21924.61 (HP OEM) behaves the same.

Other observations:

- The leaked memory is private `PAGE_READWRITE | PAGE_WRITECOMBINE` memory,
  which the driver allocates. The C++ heap does not grow.
- The leaked handles are Event objects, created in kernel mode.
- After exit, a process that leaked stays behind with 0 threads and about
  280 MB of private memory. It cannot be terminated and disappears on its own
  after 15-75 minutes.

## Impact

Qt WebEngine uses this exact sequence for every web frame when Qt Quick
renders with OpenGL on Windows (`wgl_helper.cpp`,
`native_skia_output_device_opengl.cpp`). Apps such as
[Jellyfin Desktop](https://github.com/jellyfin/jellyfin-desktop/issues/1091)
grow by about 1 GB per minute until the system runs out of memory. An apitrace
of the Qt app shows balanced register/unregister and lock/unlock calls.

Workarounds:

- **App side:** register a small pool of textures once and reuse them; the
  `reuse` mode does not leak.
- **Qt WebEngine users:** set `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`.
  Chromium then passes frames as images and does not use the interop.

## Qt WebEngine tester

[`qt-tester/`](qt-tester/) is a small Qt app that shows the same web page (rebuilt
once per second) for 60 s with OpenGL and 60 s with Direct3D 11, and writes
`report.txt` with the memory and handle growth of each run. It checks a whole
Qt WebEngine setup, not only the driver call.

Prebuilt with Qt 6.12.0, no install needed:
[qt-webengine-leak-tester-6.12.0.zip](https://github.com/tosiek/qt-opengl-amd-leak/releases/download/v2/qt-webengine-leak-tester-6.12.0.zip)
(126 MB). Unzip and double-click `run-test.bat`; it takes about 3 minutes.

| Qt | OpenGL (60 s) | Direct3D 11 (60 s) |
|---|---|---|
| 6.12.0 | +215 MB, +60 handles | +5 MB |
| 6.11.3 | +229 MB, +60 handles | 0 MB |

Build: `cmake -S qt-tester -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
-DCMAKE_PREFIX_PATH=<Qt>/msvc2022_64` and `cmake --build build` (Qt modules:
qtwebengine, qtwebchannel, qtpositioning).

## Qt WebEngine fix (tested)

[`qt-patch/0001-wgl-interop-texture-pool.patch`](qt-patch/0001-wgl-interop-texture-pool.patch)
changes only `src/core/compositor/wgl_helper.{h,cpp}`: instead of registering a new D3D11
texture with `wglDXRegisterObjectNV` for every web frame, Qt WebEngine keeps a small pool of
registered textures per OpenGL context and reuses them.

Tested with Qt 6.12.0 on the affected AMD driver (Qt Quick OpenGL, 60 s page re-rendering):
original +215 MB / +60 handles, patched +5 MB / -2 handles. Jellyfin Desktop with the
patched build stays flat during browsing and video playback.

Prebuilt for Qt 6.12.0 msvc2022_64 (drop-in replacement for `Qt6WebEngineCore.dll` and
`QtWebEngineProcess.exe`, needs the release Qt WebEngine 6.12.0 package):
[qtwebengine-6.12.0-wgl-pool-win64.zip](https://github.com/tosiek/qt-opengl-amd-leak/releases/download/v3/qtwebengine-6.12.0-wgl-pool-win64.zip).
Build details and licence notes are in the zip's `README.txt`.

## Crash on exit (atio6axx.dll, 0xc0000409)

A Qt WebEngine app that showed at least one web frame with OpenGL crashes when it exits on
the same AMD driver. The Windows event log shows Application Error 1000: faulting module
`atio6axx.dll` 31.0.21925.1001, exception code `0xc0000409`, fault offset `0x21a2b29`. It
happens with Qt 6.10.3, 6.11.3 and 6.12.0, with and without the texture pool patch. It does
not happen with `--disable-gpu`, with the Direct3D 11 scene graph, or when the window stays
minimized (no web frame imported).

Crash dump stack, from the bottom up:

```
main returns -> ExitProcess -> LdrShutdownProcess
  Qt6WebEngineCore.dll DLL_PROCESS_DETACH -> static destructors
    WGLHelper::~WGLHelper() -> wglDXCloseDeviceNV
      atio6axx.dll -> d3d11.dll -> atidxx64.dll: access violation reading freed memory
        atio6axx.dll exception handler -> __fastfail(7)   (FAST_FAIL_FATAL_APP_EXIT)
```

`WGLHelper` is a function-local static, so it is destroyed during `DLL_PROCESS_DETACH`.
`atidxx64.dll` was loaded later (by `D3D11CreateDevice`), so it has already been detached
and freed its state. Only the main thread is left at that point, so it is not a race.

### Repro without Qt

[`exit-crash/`](exit-crash/): `holder.dll` keeps a D3D11 device and a `WGL_NV_DX_interop`
device in a function-local static, like `WGLHelper`, and closes it in the destructor.
`build.bat`, then `test.bat`:

| Mode | `wglDXCloseDeviceNV` called from | Exit code |
|---|---|---|
| `dll` | `holder.dll` static destructor (`DLL_PROCESS_DETACH`) | **`0xC0000409`**, same event log signature |
| `atexit` | the exe's `atexit` handler, before `ExitProcess` | 0 |

### Qt WebEngine fix (tested)

[`qt-patch/0002-wgl-helper-no-static-destructor.patch`](qt-patch/0002-wgl-helper-no-static-destructor.patch)
creates the `WGLHelper` singleton with `new` and never destroys it; the OS releases
everything at process exit. It applies to stock Qt WebEngine and on top of 0001.

Tested with Qt 6.12.0 (0001 + 0002), Qt Quick OpenGL, window visible: 5 of 5 runs exit
cleanly. With 0001 only, every visible run crashed on exit.

## Related

- Jellyfin Desktop [#1091](https://github.com/jellyfin/jellyfin-desktop/issues/1091),
  [#1255](https://github.com/jellyfin/jellyfin-desktop/issues/1255)
- Similar OpenGL-only leaks with Qt WebEngine on NVIDIA:
  [QTBUG-142026](https://qt-project.atlassian.net/browse/QTBUG-142026),
  [QTBUG-145376](https://qt-project.atlassian.net/browse/QTBUG-145376)
