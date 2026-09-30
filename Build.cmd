@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if errorlevel 1 goto folder_error

echo Building Russia1.exe...
where cl.exe >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_tools
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto no_tools
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" goto no_tools
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl.exe >nul 2>nul
if errorlevel 1 goto no_tools

:build
rc /nologo /fo pin.res pin.rc >build.log 2>&1
if errorlevel 1 goto build_error
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE /Fe:Russia1.exe player.cpp pin.res mfplat.lib mfuuid.lib mf.lib ole32.lib oleaut32.lib user32.lib gdi32.lib wininet.lib shlwapi.lib dwmapi.lib comctl32.lib gdiplus.lib /link /SUBSYSTEM:WINDOWS >>build.log 2>&1
if errorlevel 1 goto build_error
if not exist "Russia1.exe" goto build_error
echo Build complete: Russia1.exe
echo Starting the live stream...
start /wait "" "%~dp0Russia1.exe"
if errorlevel 1 (
  echo The player exited with an error.
  pause
  exit /b 1
)
exit /b 0

:folder_error
echo Cannot open the folder containing Build.cmd.
pause
exit /b 1

:no_tools
echo Visual Studio Build Tools with Desktop development with C++ and Windows SDK are required.
echo Install these components and run Build.cmd again.
pause
exit /b 1

:build_error
echo Build failed. Compiler output follows:
if exist build.log type build.log
pause
exit /b 1
