@echo off
setlocal
call "%~dp0build.bat" x86
if errorlevel 1 exit /b 1
call "%~dp0build.bat" x64
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0package.ps1"
if errorlevel 1 exit /b 1
exit /b 0
