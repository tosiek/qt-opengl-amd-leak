@echo off
REM Runs all modes. Build first with build.bat. Usage: test.bat [frames]
cd /d "%~dp0"
set FRAMES=%1
if "%FRAMES%"=="" set FRAMES=75
for %%m in (register reuse nointerop) do (
  echo.
  .\interop_leak.exe %%m %FRAMES%
)
