@echo off
rem =====================================================================================
rem  perfstat - build the "plugin load" self-test
rem             (loads build\perfstat.dll exactly like the engine would)
rem  Output: build\perfstat_loader_test.exe
rem  Note  : ASCII-only on purpose
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
if not exist "build\perfstat_loader_test.exe" (
    echo [ERROR] build\perfstat_loader_test.exe was not produced
    exit /b 1
)
echo done: build\perfstat_loader_test.exe
exit /b 0
