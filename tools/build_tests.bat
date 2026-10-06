@echo off
rem =====================================================================================
rem  perfstat - build the offline self-test tool (32-bit console exe)
rem  Output: build\perfstat_tests.exe
rem  Note  : ASCII-only on purpose (cmd.exe mangles UTF-8 batch files)
rem
rem  Works both with an already-configured x86 environment (CI / Native Tools prompt)
rem  and from a plain cmd window (it will load vcvars32.bat itself).
rem =====================================================================================
setlocal

cd /d "%~dp0.."

rem ---- 1) already configured? ----
where cl.exe >nul 2>nul
if not errorlevel 1 goto :have_env

rem ---- 2) find vcvars32.bat ----
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
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat"

if not defined VCVARS (
    echo [ERROR] Cannot find a 32-bit MSVC environment ^(see build_win32.bat for details^)
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] failed to run vcvars32.bat
    exit /b 1
)

:have_env

if not exist "build" mkdir "build"
if not exist "build\tests" mkdir "build\tests"

echo compiling self-test...
cl /nologo /MT /O2 /Ob2 /Oi /GS- /W3 /EHsc /utf-8 /GR- /DNDEBUG /DWIN32 /D_WINDOWS ^
   /Fo:build\tests\ /Fd:build\tests\perfstat_tests.pdb ^
   tools\perfstat_tests.cpp src\core.cpp src\perf_platform_win32.cpp ^
   /link /OUT:build\perfstat_tests.exe /MACHINE:X86 /SUBSYSTEM:CONSOLE ^
   /INCREMENTAL:NO kernel32.lib user32.lib psapi.lib

if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
if not exist "build\perfstat_tests.exe" (
    echo [ERROR] build\perfstat_tests.exe was not produced
    exit /b 1
)
echo done: build\perfstat_tests.exe
exit /b 0
