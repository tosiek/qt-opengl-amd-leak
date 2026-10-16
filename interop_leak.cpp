// AMD WGL_NV_DX_interop leak: minimal repro (D3D11 + OpenGL, no other libraries).
//
// Per frame: create a D3D11 texture, register it with wglDXRegisterObjectNV, lock,
// unlock, unregister, release it and Flush() the D3D11 context. Every resource is
// freed, yet on AMD Radeon the process grows by one texture (~3.5 MB at 1280x720)
// and one Event handle per frame. The same loop without the interop calls stays flat.
//
// interop_leak.exe [mode] [frames]
//   register   new texture registered/unregistered every frame (default; leaks on AMD)
//   reuse      one texture registered once, locked/unlocked every frame (flat)
//   nointerop  new texture every frame, no WGL interop (flat)

#include <windows.h>
#include <d3d11.h>
#include <psapi.h>
#include <wrl/client.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

typedef HANDLE(WINAPI* PFN_wglDXOpenDeviceNV)(void*);
typedef BOOL(WINAPI* PFN_wglDXCloseDeviceNV)(HANDLE);
typedef HANDLE(WINAPI* PFN_wglDXRegisterObjectNV)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* PFN_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL(WINAPI* PFN_wglDXLockObjectsNV)(HANDLE, GLint, HANDLE*);
typedef BOOL(WINAPI* PFN_wglDXUnlockObjectsNV)(HANDLE, GLint, HANDLE*);

static const GLenum WGL_ACCESS_READ_ONLY_NV = 0x0000;
static const UINT W = 1280, H = 720;

static void check(bool ok, const char* what)
{
  if (ok)
    return;
  std::printf("ERROR: %s (GetLastError %lu)\n", what, GetLastError());
  std::exit(1);
}

static void report(const char* label, SIZE_T baseMem, DWORD baseHandles)
{
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
  DWORD handles = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles);
  std::printf("%-10s private %+6lld MB   handles %+5ld\n", label,
              (long long)(pmc.PrivateUsage >> 20) - (long long)(baseMem >> 20), (long)handles - (long)baseHandles);
}

int main(int argc, char** argv)
{
  const char* mode = argc > 1 ? argv[1] : "register";
  const int frames = argc > 2 ? std::atoi(argv[2]) : 100;
  const bool reuse = !std::strcmp(mode, "reuse");
  const bool interop = std::strcmp(mode, "nointerop") != 0;

  // Hidden window + OpenGL context
  WNDCLASSA wc{};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "interop_leak";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("interop_leak", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
  HDC hdc = GetDC(hwnd);
  PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
  check(SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd), "SetPixelFormat");
  HGLRC glrc = wglCreateContext(hdc);
  check(glrc && wglMakeCurrent(hdc, glrc), "wglCreateContext");
  std::printf("GL_RENDERER: %s\nGL_VERSION:  %s\nmode: %s, %d frames\n", glGetString(GL_RENDERER),
              glGetString(GL_VERSION), mode, frames);

  auto wglDXOpenDeviceNV = (PFN_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
  auto wglDXCloseDeviceNV = (PFN_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
  auto wglDXRegisterObjectNV = (PFN_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
  auto wglDXUnregisterObjectNV = (PFN_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
  auto wglDXLockObjectsNV = (PFN_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
  auto wglDXUnlockObjectsNV = (PFN_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");
  check(wglDXOpenDeviceNV && wglDXRegisterObjectNV && wglDXLockObjectsNV, "WGL_NV_DX_interop not available");

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)),
        "D3D11CreateDevice");
  HANDLE interopDevice = wglDXOpenDeviceNV(device.Get());
  check(interopDevice != nullptr, "wglDXOpenDeviceNV");

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = W;
  desc.Height = H;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  ComPtr<ID3D11Texture2D> texture;
  GLuint glTexture = 0;
  HANDLE glHandle = nullptr;
  glGenTextures(1, &glTexture);

  SIZE_T baseMem = 0;
  DWORD baseHandles = 0;
  const int warmup = 10;
  for (int f = 0; f < warmup + frames; ++f)
  {
    if (!reuse || !texture)
    {
      check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "CreateTexture2D");
      if (interop)
      {
        glHandle = wglDXRegisterObjectNV(interopDevice, texture.Get(), glTexture, GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        check(glHandle != nullptr, "wglDXRegisterObjectNV");
      }
    }

    if (interop)
    {
      check(wglDXLockObjectsNV(interopDevice, 1, &glHandle), "wglDXLockObjectsNV");
      wglDXUnlockObjectsNV(interopDevice, 1, &glHandle);
    }

    if (!reuse)
    {
      if (interop)
        check(wglDXUnregisterObjectNV(interopDevice, glHandle), "wglDXUnregisterObjectNV");
      texture.Reset(); // last reference: the texture should be destroyed
    }
    context->Flush();
    Sleep(50); // give the driver time for deferred destruction (~20 fps)

    if (f == warmup - 1)
    {
      PROCESS_MEMORY_COUNTERS_EX pmc{};
      GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
      baseMem = pmc.PrivateUsage;
      GetProcessHandleCount(GetCurrentProcess(), &baseHandles);
    }
    if (f >= warmup && (f - warmup) % 25 == 0)
    {
      char label[32];
      std::snprintf(label, sizeof(label), "frame %d", f - warmup);
      report(label, baseMem, baseHandles);
    }
  }
  report("end", baseMem, baseHandles);

  if (reuse && interop)
    wglDXUnregisterObjectNV(interopDevice, glHandle);
  texture.Reset();
  glDeleteTextures(1, &glTexture);
  wglDXCloseDeviceNV(interopDevice);
  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(glrc);
  ReleaseDC(hwnd, hdc);
  DestroyWindow(hwnd);
  return 0;
}
