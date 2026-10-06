@echo off
rem =====================================================================================
rem  perfstat - build the "plugin load" self-test (loads build\perfstat.dll like the engine)
rem  Output: build\perfstat_loader_test.exe
rem  Note  : ASCII-only on purpose
rem =====================================================================================
setlocal

cd /d "%~dp0.."

set "VCVARS="
if exist "D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

if not defined VCVARS (
    echo [ERROR] vcvars32.bat not found
    exit /b 1
)

call "%VCVARS%" >nul
if not exist "build" mkdir "build"
if not exist "build\tests" mkdir "build\tests"

echo compiling loader test...
cl /nologo /MT /O2 /W3 /EHsc /utf-8 /GR- /DNDEBUG /DWIN32 /D_WINDOWS ^
   /Fo:build\tests\ /Fd:build\tests\perfstat_loader_test.pdb ^
   tools\perfstat_loader_test.cpp ^
   /link /OUT:build\perfstat_loader_test.exe /MACHINE:X86 /SUBSYSTEM:CONSOLE ^
   /INCREMENTAL:NO kernel32.lib

if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo done: build\perfstat_loader_test.exe
exit /b 0
