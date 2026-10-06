@echo off
rem =====================================================================================
rem  perfstat - Windows 32-bit build script (VS2019 / VS2022 / VS2026, plain cl.exe)
rem  Usage : run build_win32.bat from a normal cmd window
rem  Output: build\perfstat.dll
rem  Note  : this file is intentionally ASCII-only; Chinese docs are in README.md
rem =====================================================================================
setlocal

cd /d "%~dp0"

rem -----------------------------------------------------------------------------------
rem 1) If a 32-bit MSVC environment is ALREADY configured (CI uses ilammy/msvc-dev-cmd,
rem    or the user opened a "x86 Native Tools Command Prompt"), just use it.
rem    Detect by checking whether cl.exe is on PATH -- a native-tools prompt and the
rem    msvc-dev-cmd action both prepend the compiler directory to PATH.
rem -----------------------------------------------------------------------------------
where cl.exe >nul 2>nul
if not errorlevel 1 (
    echo [1/3] cl.exe already on PATH, using the current environment
    goto :have_env
)

rem -----------------------------------------------------------------------------------
rem 2) Otherwise look for vcvars32.bat. Try vswhere first (works for any VS version and
rem    any install drive), then fall back to a list of well-known locations.
rem -----------------------------------------------------------------------------------
set "VCVARS="

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat"
    )
)

if not defined VCVARS if defined VCINSTALLDIR if exist "%VCINSTALLDIR%Auxiliary\Build\vcvars32.bat" set "VCVARS=%VCINSTALLDIR%Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if defined VSINSTALLDIR if exist "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%VSINSTALLDIR%VC\Auxiliary\Build\vcvars32.bat"

if not defined VCVARS if exist "D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=D:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

if not defined VCVARS (
    echo [ERROR] Cannot find a 32-bit MSVC environment.
    echo         Please either:
    echo           1. open "x86 Native Tools Command Prompt for VS" and run build_win32.bat, or
    echo           2. install "Desktop development with C++" workload with the x86 toolset.
    exit /b 1
)

echo [1/3] loading x86 build environment: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] failed to run vcvars32.bat
    exit /b 1
)

:have_env

if not exist "build" mkdir "build"

echo [2/3] compiling...
if exist "build\perfstat.dll" del /q "build\perfstat.dll"
cl /nologo /LD /MT /O2 /Ob2 /Oi /GS- /W3 /EHsc /utf-8 /GR- /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS ^
   /Fo:build\ /Fd:build\perfstat.pdb ^
   src\perfstat.cpp src\core.cpp src\perf_platform_win32.cpp ^
   /link /OUT:build\perfstat.dll /IMPLIB:build\perfstat.lib /MACHINE:X86 ^
   /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /OPT:REF /OPT:ICF kernel32.lib user32.lib psapi.lib

if errorlevel 1 (
    echo [ERROR] compile failed
    exit /b 1
)

if not exist "build\perfstat.dll" (
    echo [ERROR] build\perfstat.dll was not produced
    exit /b 1
)

echo [3/3] done: build\perfstat.dll
echo.
echo Install: copy build\perfstat.dll into the server's left4dead2\addons\ folder, then:
echo     plugin_load perfstat
echo     perf_help
echo.
exit /b 0
