// holder.dll: plays the role of Qt6WebEngineCore.dll's WGLHelper.
// A function-local static owns a D3D11 device and a WGL_NV_DX_interop device. Its destructor
// calls wglDXCloseDeviceNV. Function-local statics of a DLL are destroyed in the DLL's
// DLL_PROCESS_DETACH, i.e. inside LdrShutdownProcess, after atidxx64.dll (loaded later, by
// D3D11CreateDevice) has already been detached.

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <GL/gl.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

typedef HANDLE(WINAPI* PFN_wglDXOpenDeviceNV)(void*);
typedef BOOL(WINAPI* PFN_wglDXCloseDeviceNV)(HANDLE);
typedef HANDLE(WINAPI* PFN_wglDXRegisterObjectNV)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* PFN_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL(WINAPI* PFN_wglDXLockObjectsNV)(HANDLE, GLint, HANDLE*);
typedef BOOL(WINAPI* PFN_wglDXUnlockObjectsNV)(HANDLE, GLint, HANDLE*);

struct Holder
{
  PFN_wglDXCloseDeviceNV close = nullptr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  HANDLE interopDevice = nullptr;

  Holder()
  {
    // Like WGLHelper: needs a current GL context for wglGetProcAddress.
    auto open = (PFN_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
    close = (PFN_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
    auto reg = (PFN_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
    auto unreg = (PFN_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
    auto lock = (PFN_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
    auto unlock = (PFN_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");
    if (!open || !close || !reg || !unreg || !lock || !unlock)
    {
      std::printf("WGL_NV_DX_interop not available\n");
      ExitProcess(2);
    }
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                      &context);
    interopDevice = open(device.Get());

    // Use the interop once, like one imported WebEngine frame.
    D3D11_TEXTURE2D_DESC desc{1280, 720, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> texture;
    device->CreateTexture2D(&desc, nullptr, &texture);
    GLuint glTexture = 0;
    glGenTextures(1, &glTexture);
    HANDLE h = reg(interopDevice, texture.Get(), glTexture, GL_TEXTURE_2D, 0);
    lock(interopDevice, 1, &h);
    unlock(interopDevice, 1, &h);
    unreg(interopDevice, h);
    glDeleteTextures(1, &glTexture);
    std::printf("holder: interop device %p open\n", interopDevice);
  }

  ~Holder() { closeDevice("static destructor (DLL_PROCESS_DETACH)"); }

  void closeDevice(const char* where)
  {
    if (!interopDevice)
      return;
    std::printf("holder: wglDXCloseDeviceNV from %s\n", where);
    std::fflush(stdout);
    close(interopDevice); // <- crashes in atio6axx/atidxx64 when called during loader shutdown
    interopDevice = nullptr;
    std::printf("holder: closed\n");
  }
};

static Holder* instance()
{
  static Holder holder; // same pattern as WGLHelper::instance()
  return &holder;
}

extern "C" __declspec(dllexport) void holder_init() { instance(); }
extern "C" __declspec(dllexport) void holder_close() { instance()->closeDevice("exe atexit (before ExitProcess)"); }
