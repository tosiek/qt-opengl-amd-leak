// AMD exit crash (atio6axx.dll 0xc0000409) without Qt.
//
// detach_crash.exe dll     interop device closed by holder.dll's static destructor (crashes)
// detach_crash.exe atexit  same device closed from the exe's atexit, before ExitProcess (clean)
//
// In both modes the OpenGL context is deleted before main returns, as Qt does.

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" __declspec(dllimport) void holder_init();
extern "C" __declspec(dllimport) void holder_close();

int main(int argc, char** argv)
{
  const bool viaAtexit = argc > 1 && !std::strcmp(argv[1], "atexit");
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  WNDCLASSA wc{};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "detach_crash";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("detach_crash", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
  HDC hdc = GetDC(hwnd);
  PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
  SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
  HGLRC glrc = wglCreateContext(hdc);
  wglMakeCurrent(hdc, glrc);

  holder_init();
  if (viaAtexit)
    std::atexit(holder_close);

  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(glrc);
  ReleaseDC(hwnd, hdc);
  DestroyWindow(hwnd);
  std::printf("main: returning\n");
  return 0;
}
