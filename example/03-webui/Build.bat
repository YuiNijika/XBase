@echo off
setlocal EnableDelayedExpansion
pushd "%~dp0"

set "CONFIG=Release"
set "NO_PAUSE="
set "PLATFORM_TOOLSET=%PLATFORM_TOOLSET%"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="Debug" (
    set "CONFIG=Debug"
    shift
    goto parse_args
)
if /i "%~1"=="Release" (
    set "CONFIG=Release"
    shift
    goto parse_args
)
if /i "%~1"=="--no-pause" (
    set "NO_PAUSE=1"
    shift
    goto parse_args
)
if /i "%~1"=="--toolset" (
    shift
    if "%~1"=="" (
        echo [Error] --toolset requires a value such as v145 or v143
        goto fail
    )
    set "PLATFORM_TOOLSET=%~1"
    shift
    goto parse_args
)
echo [Error] Unknown argument: %~1
echo Usage: Build.bat [Debug^|Release] [--toolset v145] [--no-pause]
goto fail

:args_done

echo ==========================================
echo    XBase Example Builder
echo ==========================================
echo Configuration: %CONFIG%
echo Platform: Win32
echo.

rem XBase 只 stage Release 的库，要用 Debug 得先单独构建并 stage 一份
if /i not "%CONFIG%"=="Release" (
    echo [Error] The staged XBase SDK contains Release libraries only.
    goto fail
)

set "PREMAKE_EXE=..\..\tools\premake5.exe"
if not exist "%PREMAKE_EXE%" (
    echo [Error] premake5.exe not found at %PREMAKE_EXE%
    echo [Error] Run XBase Build.bat first, or copy tools\premake5.exe here.
    goto fail
)

for %%T in (XBaseModEntry XBaseSA XBaseVC XBaseIII PluginSA PluginVC PluginIII) do (
    if not exist "..\lib\%%T.lib" (
        echo [Error] Missing staged library: ..\lib\%%T.lib
        echo [Error] Run XBase Build.bat Release to stage the XBase SDK.
        goto fail
    )
)
if not exist "..\include\XBase\XBase.h" (
    echo [Error] Missing staged headers: ..\include\XBase\XBase.h
    echo [Error] Run XBase Build.bat Release to stage the XBase SDK.
    goto fail
)

call :resolve_msbuild
if errorlevel 1 goto fail

rem VS18 的真实 C++ 工具集是 v145，老版本用 --toolset v143 覆盖
if not defined PLATFORM_TOOLSET set "PLATFORM_TOOLSET=v145"
echo [Info] PlatformToolset: %PLATFORM_TOOLSET%

if not exist "build" mkdir "build"

echo Removing stale generated project files...
del /Q "build\*.sln" "build\*.vcxproj" "build\*.vcxproj.filters" "build\*.vcxproj.user" >nul 2>nul

echo Generating Visual Studio project files...
"%PREMAKE_EXE%" vs2022
if errorlevel 1 (
    echo [Error] Project generation failed.
    goto fail
)

rem 工程名从生成的 sln 推导，把本脚本复制到别的示例目录不用改任何一行
set "PROJECT_NAME="
for %%S in ("build\*.sln") do set "PROJECT_NAME=%%~nS"
if not defined PROJECT_NAME (
    echo [Error] No solution found under build\.
    goto fail
)
echo [Info] Building project: %PROJECT_NAME%

set "MSBUILD_PROPS=/p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal"

for %%G in (SA VC III) do (
    echo Building !PROJECT_NAME!%%G.asi...
    "!MSBUILD_EXE!" "build\!PROJECT_NAME!.sln" /m /t:!PROJECT_NAME!%%G !MSBUILD_PROPS!
    if errorlevel 1 (
        echo [Error] !PROJECT_NAME!%%G build failed.
        goto fail
    )
    if not exist "build\bin\!PROJECT_NAME!%%G.asi" (
        echo [Error] build\bin\!PROJECT_NAME!%%G.asi was not produced.
        goto fail
    )
)

rem 模组清单必须落到数据目录，Bootstrap 挂载前会读它校验 engines 与依赖
if exist "data\package.json" (
    if not exist "build\bin\XBase\Mods\!PROJECT_NAME!" mkdir "build\bin\XBase\Mods\!PROJECT_NAME!"
    copy /Y "data\package.json" "build\bin\XBase\Mods\!PROJECT_NAME!\package.json" >nul
    echo [Info] Staged package.json to build\bin\XBase\Mods\!PROJECT_NAME!
)

rem 示例自带的网页资源随 asi 落到产物目录，方便整包拷进游戏目录
if exist "ui\" (
    if not exist "build\bin\XBase\Mods\!PROJECT_NAME!\ui" mkdir "build\bin\XBase\Mods\!PROJECT_NAME!\ui"
    xcopy "ui\*" "build\bin\XBase\Mods\!PROJECT_NAME!\ui\" /E /I /Y >nul
    echo [Info] Staged ui to build\bin\XBase\Mods\!PROJECT_NAME!\ui
)

echo.
echo Build completed successfully.
echo Output files:
for %%G in (SA VC III) do echo   build\bin\!PROJECT_NAME!%%G.asi
echo.
echo Copy the asi matching your game into the game plugins folder.
goto success

rem ============================================================
rem 纯 cmd 定位 MSBuild，不依赖 PowerShell
rem ============================================================
:resolve_msbuild
set "MSBUILD_EXE="

if defined VSINSTALLDIR (
    if exist "%VSINSTALLDIR%MSBuild\Current\Bin\MSBuild.exe" (
        set "MSBUILD_EXE=%VSINSTALLDIR%MSBuild\Current\Bin\MSBuild.exe"
    )
)

if not defined MSBUILD_EXE (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2^>nul`) do (
            if not defined MSBUILD_EXE set "MSBUILD_EXE=%%i"
        )
    )
)

if not defined MSBUILD_EXE (
    for %%R in (
        "D:\SoftWare\Microsoft Visual Studio"
        "%ProgramFiles%\Microsoft Visual Studio"
        "%ProgramFiles(x86)%\Microsoft Visual Studio"
    ) do (
        if not defined MSBUILD_EXE if exist "%%~R\" (
            for %%Y in (18 2026 2022 2019) do (
                if not defined MSBUILD_EXE (
                    for %%E in (Community Professional Enterprise BuildTools Preview) do (
                        if not defined MSBUILD_EXE if exist "%%~R\%%Y\%%E\MSBuild\Current\Bin\MSBuild.exe" (
                            set "MSBUILD_EXE=%%~R\%%Y\%%E\MSBuild\Current\Bin\MSBuild.exe"
                        )
                    )
                )
            )
        )
    )
)

if not defined MSBUILD_EXE (
    for /f "delims=" %%i in ('where MSBuild.exe 2^>nul') do (
        if not defined MSBUILD_EXE set "MSBUILD_EXE=%%i"
    )
)

if not defined MSBUILD_EXE (
    echo [Error] MSBuild.exe not found.
    echo [Error] Install Visual Studio with the Desktop development with C++ workload.
    exit /b 1
)

echo [Info] Using MSBuild: !MSBUILD_EXE!
exit /b 0

:success
popd
if not defined NO_PAUSE pause
exit /b 0

:fail
popd
if not defined NO_PAUSE pause
exit /b 1
