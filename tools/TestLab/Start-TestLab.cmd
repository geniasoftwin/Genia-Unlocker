@echo off
setlocal
cd /d "%~dp0"
echo Genia Unlocker Test Lab
echo.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0GeniaUnlocker-TestLab.ps1"
if errorlevel 1 (
  echo.
  echo [ERROR] Test Lab exited with code %ERRORLEVEL%.
  pause
)
