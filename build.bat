@echo off
rem build.bat - build aint.exe with MSVC (VS 2026 Build Tools / Community)
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
  echo vcvars64.bat not found - install VS Build Tools with the C++ workload
  exit /b 1
)
call %VCVARS% >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /W3 /O2 /Ob2 /Oi /GF /GS- /Gy /GR- /GL /arch:AVX2 /volatileMetadata- /MD ^
  /Fo:build\ /Fe:aint.exe ^
  src\main.c src\codec.c src\archive.c src\garble.c src\thread.c src\fs.c ^
  /link /LTCG /nocoffgrpinfo /EMITVOLATILEMETADATA:NO /emittoolversioninfo:no ^
  /STUB:stub.bin /OPT:REF /OPT:ICF=32 /OPT:LBR /FIXED /DYNAMICBASE:NO
if errorlevel 1 exit /b 1
echo built aint.exe
