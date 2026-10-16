# AMD `WGL_NV_DX_interop` leak: unregistered D3D11 textures are never freed

On AMD Radeon graphics (Windows), a Direct3D 11 texture that was registered
with `wglDXRegisterObjectNV` is **never freed** after
`wglDXUnregisterObjectNV` and its last `Release()`, even after
`ID3D11DeviceContext::Flush()`. One kernel Event handle leaks with it.

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
test.bat                         REM runs the three modes below, 75 frames each
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

## Related

- Jellyfin Desktop [#1091](https://github.com/jellyfin/jellyfin-desktop/issues/1091),
  [#1255](https://github.com/jellyfin/jellyfin-desktop/issues/1255)
- Similar OpenGL-only leaks with Qt WebEngine on NVIDIA:
  [QTBUG-142026](https://qt-project.atlassian.net/browse/QTBUG-142026),
  [QTBUG-145376](https://qt-project.atlassian.net/browse/QTBUG-145376)
