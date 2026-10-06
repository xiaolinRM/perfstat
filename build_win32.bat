@echo off
rem =====================================================================================
rem  perfstat - Windows 32-bit build script (VS2022, plain cl.exe)
rem  Usage : run build_win32.bat from a normal cmd window
rem  Output: build\perfstat.dll
rem  Note  : this file is intentionally ASCII-only; Chinese docs are in README.md
rem =====================================================================================
setlocal

cd /d "%~dp0"

set "VCVARS="
if exist "D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat"

if not defined VCVARS (
    echo [ERROR] vcvars32.bat not found. Please open a "x86 Native Tools Command Prompt" and run cl manually.
    exit /b 1
)

echo [1/3] loading x86 build environment...
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] failed to run vcvars32.bat
    exit /b 1
)

if not exist "build" mkdir "build"

echo [2/3] compiling...
if exist "build\perfstat.dll" del /q "build\perfstat.dll"
cl /nologo /LD /MT /O2 /Ob2 /Oi /GS- /W3 /EHsc /utf-8 /GR- /DNDEBUG /DWIN32 /D_WINDOWS ^
   /Fo:build\ /Fd:build\perfstat.pdb ^
   src\perfstat.cpp src\core.cpp src\perf_platform_win32.cpp ^
   /link /OUT:build\perfstat.dll /MACHINE:X86 ^
   /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /OPT:REF /OPT:ICF kernel32.lib user32.lib psapi.lib

if errorlevel 1 (
    echo [ERROR] compile failed
    exit /b 1
)

echo [3/3] done: build\perfstat.dll
echo.
echo Install: copy build\perfstat.dll into the server's left4dead2\addons\ folder, then:
echo     plugin_load perfstat
echo     perf_help
echo.
exit /b 0
