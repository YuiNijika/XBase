@echo off
setlocal EnableDelayedExpansion
pushd "%~dp0"

echo [Info] XBase viewer has been deprecated and is no longer built.
echo [Info] Use XBase\Mods\<mod>\debug.log and the in-game diagnostics instead.
popd
exit /b 0

rem Legacy build script retained for source archival only.

set "CONFIG=Release"
set "NO_PAUSE="

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="Release" (
    set "CONFIG=Release"
    shift
    goto parse_args
)
if /i "%~1"=="Debug" (
    set "CONFIG=Debug"
    shift
    goto parse_args
)
if /i "%~1"=="--no-pause" (
    set "NO_PAUSE=1"
    shift
    goto parse_args
)
echo [Error] Unknown argument: %~1
echo Usage: Build.bat [Debug^|Release] [--no-pause]
goto fail

:args_done

echo ==========================================
echo    XBase Viewer Builder
echo ==========================================
echo Configuration: %CONFIG%

rem 用 vswhere 找已安装的 Visual Studio，再取出 32 位编译环境
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALLDIR="
if exist "!VSWHERE!" (
    for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        set "VSINSTALLDIR=%%i\"
    )
)
if not defined VSINSTALLDIR (
    echo [Error] Visual Studio with C++ build tools was not found.
    goto fail
)

echo Using Visual Studio: !VSINSTALLDIR!
call "!VSINSTALLDIR!VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo [Error] Failed to initialize the compiler environment.
    goto fail
)

if not exist "build\bin" mkdir "build\bin"
if not exist "build\obj" mkdir "build\obj"

if /i "%CONFIG%"=="Debug" (
    set "CL_FLAGS=/nologo /Od /Zi /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DDEBUG"
) else (
    set "CL_FLAGS=/nologo /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG"
)

echo Compiling src\main.cpp ...
cl !CL_FLAGS! /Fo:build\obj\ /Fe:build\bin\XBase.exe src\main.cpp ^
    /link user32.lib gdi32.lib shell32.lib comctl32.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [Error] Build failed.
    goto fail
)

if not exist "build\bin\XBase.exe" (
    echo [Error] build\bin\XBase.exe was not produced.
    goto fail
)

echo.
echo [Info] Build completed successfully.
echo [Info] Output: build\bin\XBase.exe
echo [Info] Copy it to ^<game root^>\XBase\XBase.exe and run it there.
goto success

:success
popd
if not defined NO_PAUSE pause
exit /b 0

:fail
popd
if not defined NO_PAUSE pause
exit /b 1
