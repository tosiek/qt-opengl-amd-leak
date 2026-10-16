@echo off
REM Runs all modes and saves the output to results.txt. Build first with build.bat.
REM Usage: test.bat [frames]
cd /d "%~dp0"
set FRAMES=%1
if "%FRAMES%"=="" set FRAMES=75
echo Running, about 15 seconds...
(
  ver
  for %%m in (register reuse nointerop) do (
    echo.
    .\interop_leak.exe %%m %FRAMES%
  )
) > results.txt 2>&1
type results.txt
echo.
echo Saved to %CD%\results.txt
pause
