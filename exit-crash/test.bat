@echo off
REM Runs both modes and prints the exit codes. Build first with build.bat.
REM -1073740791 = 0xC0000409, the crash (also logged as Application Error 1000).
cd /d "%~dp0"
.\detach_crash.exe dll
echo exit code %ERRORLEVEL%
echo.
.\detach_crash.exe atexit
echo exit code %ERRORLEVEL%
pause
