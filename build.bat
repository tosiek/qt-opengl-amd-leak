@echo off
REM Run from a "x64 Native Tools Command Prompt for VS 2022"
cl /nologo /EHsc /O2 /MD /std:c++17 interop_leak.cpp /link d3d11.lib opengl32.lib user32.lib gdi32.lib psapi.lib
