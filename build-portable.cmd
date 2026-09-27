@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo === Genia Unlocker VS Portable build ===

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found. Install Visual Studio Desktop development with C++.
  exit /b 2
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe`) do (
  set "MSBUILD=%%I"
  goto :found
)

:found
if not defined MSBUILD (
  echo [ERROR] MSBuild.exe with C++ tools was not found.
  exit /b 3
)

echo MSBuild: %MSBUILD%
"%MSBUILD%" "%~dp0GeniaUnlocker.sln" /m /restore /t:Rebuild /p:Configuration=Portable /p:Platform=x64 /v:minimal
if errorlevel 1 exit /b %errorlevel%

echo.
echo [OK] dist\GeniaUnlocker.exe
exit /b 0
