// Shows that the exit crash depends on DLL detach order.
//
// detach_order.exe early   holder.dll initialized before atidxx64.dll (like Qt6WebEngineCore.dll):
//                          at exit atidxx64.dll is detached first, then holder.dll's static
//                          destructor calls wglDXCloseDeviceNV -> crash expected
// detach_order.exe late    a D3D11 device is created first, so atidxx64.dll is initialized before
//                          holder.dll; at exit holder.dll is detached first -> no crash expected
//
// Same holder.dll, same call from the same static destructor in both modes.

#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>

typedef void (*PFN_holder_init)();

int main(int argc, char** argv)
{
  const bool late = argc > 1 && !std::strcmp(argv[1], "late");
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  ID3D11Device* pin = nullptr;
  if (late)
  {
    // Loads and initializes atidxx64.dll now; the device is kept so the driver stays loaded.
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pin, nullptr,
                      nullptr);
    std::printf("main: D3D11 device created first (atidxx64.dll %s)\n",
                GetModuleHandleA("atidxx64.dll") ? "loaded" : "NOT loaded");
  }

  HMODULE holder = LoadLibraryA("holder.dll");
  auto holder_init = (PFN_holder_init)GetProcAddress(holder, "holder_init");
  if (!holder_init)
  {
    std::printf("holder.dll not found\n");
    return 2;
  }

  WNDCLASSA wc{};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "detach_order";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("detach_order", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
  HDC hdc = GetDC(hwnd);
  PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
  SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
  HGLRC glrc = wglCreateContext(hdc);
  wglMakeCurrent(hdc, glrc);

  holder_init();

  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(glrc);
  ReleaseDC(hwnd, hdc);
  DestroyWindow(hwnd);
  std::printf("main: returning (%s)\n", late ? "late" : "early");
  return 0;
}
