@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if not exist "Russia1.exe" (
  echo First run Build.cmd in this folder to create Russia1.exe.
  pause
  exit /b 1
)
"%~dp0Russia1.exe"
if errorlevel 1 (
  echo The player exited with an error.
  pause
)
