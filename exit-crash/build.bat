@echo off
REM Run from a "x64 Native Tools Command Prompt for VS 2022"
cl /nologo /EHsc /O2 /MD /LD holder.cpp /link d3d11.lib opengl32.lib || exit /b 1
cl /nologo /EHsc /O2 /MD detach_crash.cpp /link holder.lib user32.lib gdi32.lib opengl32.lib || exit /b 1
cl /nologo /EHsc /O2 /MD detach_order.cpp /link d3d11.lib user32.lib gdi32.lib opengl32.lib
