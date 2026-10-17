@echo off
REM Runs all modes and prints the exit codes. Build first with build.bat.
REM -1073740791 = 0xC0000409, the crash (also logged as Application Error 1000).
cd /d "%~dp0"
for %%m in (dll atexit) do (
  echo.
  echo ^> detach_crash.exe %%m
  .\detach_crash.exe %%m
  call echo exit code %%ERRORLEVEL%%
)
for %%m in (early late) do (
  echo.
  echo ^> detach_order.exe %%m
  .\detach_order.exe %%m
  call echo exit code %%ERRORLEVEL%%
)
pause
