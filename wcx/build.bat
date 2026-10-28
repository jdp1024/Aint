@echo off
setlocal
cd /d "%~dp0"
set ARCH=%~1
if "%ARCH%"=="" set ARCH=x64
if not "%ARCH%"=="x64" if not "%ARCH%"=="x86" (
  echo Usage: build.bat [x64^|x86]
  exit /b 1
)
set VS=C:\Program Files\Microsoft Visual Studio\18\BuildTools
if not exist "%VS%\VC\Auxiliary\Build\vcvarsall.bat" set VS=C:\Program Files\Microsoft Visual Studio\18\Community
if not exist "%VS%\VC\Auxiliary\Build\vcvarsall.bat" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
)
if not exist "%VS%\VC\Auxiliary\Build\vcvarsall.bat" (
  echo Visual Studio C++ Build Tools not found.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul
if errorlevel 1 exit /b 1
set OUT=build\%ARCH%
set EXT=wcx64
set DEF=ain_wcx.def
if "%ARCH%"=="x86" set EXT=wcx
if "%ARCH%"=="x86" set DEF=ain_wcx_x86.def
if not exist "%OUT%" mkdir "%OUT%"
rc /nologo /fo "%OUT%\ain_wcx.res" ain_wcx.rc
if errorlevel 1 exit /b 1
cl /nologo /W3 /O2 /arch:AVX2 /Ob2 /Oi /GF /GS- /Gy /GR- /GL /arch:AVX2 /volatileMetadata- /MT /LD /D_WIN32_WINNT=0x0601 /DWIN32_LEAN_AND_MEAN ^
  /Fo"%OUT%\\" /Fe"%OUT%\ain.%EXT%" ^
  plugin.c settings.c ..\src\archive.c ..\src\codec.c ..\src\garble.c ..\src\thread.c ..\src\fs.c ^
  /link /LTCG /nocoffgrpinfo /EMITVOLATILEMETADATA:NO /emittoolversioninfo:no /OPT:REF /OPT:ICF=32 /OPT:LBR /DEF:%DEF% /IMPLIB:"%OUT%\ain.lib" "%OUT%\ain_wcx.res" user32.lib
if errorlevel 1 exit /b 1
cl /nologo /W3 /O2 /MT /D_WIN32_WINNT=0x0601 /Fo"%OUT%\wcx_test.obj" ^
  /Fe"%OUT%\wcx_test.exe" tests\wcx_test.c "%OUT%\ain.lib" user32.lib
if errorlevel 1 exit /b 1
echo Built %OUT%\ain.%EXT% and WCX integration tests.
