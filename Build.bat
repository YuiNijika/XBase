@echo off
setlocal EnableDelayedExpansion
pushd "%~dp0"

set "CONFIG=Release"
set "NO_PAUSE="
if not defined PLATFORM_TOOLSET set "PLATFORM_TOOLSET=v145"

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
if /i "%~1"=="--toolset" goto parse_toolset
echo [Error] Unknown argument: %~1
echo Usage: Build.bat [Debug^|Release] [--toolset v145] [--no-pause]
goto fail

:parse_toolset
shift
if "%~1"=="" (
    echo [Error] --toolset requires a value, e.g. v145 / v143
    goto fail
)
set "TOOLSET_VALUE=%~1"
if /i "%TOOLSET_VALUE:~0,1%" NEQ "v" (
    echo [Error] Invalid platform toolset: %TOOLSET_VALUE%
    echo [Error] Expected a value such as v145 or v143.
    goto fail
)
for /f "delims=0123456789" %%C in ("%TOOLSET_VALUE:~1%") do (
    echo [Error] Invalid platform toolset: %TOOLSET_VALUE%
    echo [Error] Expected a value such as v145 or v143.
    goto fail
)
if "%TOOLSET_VALUE:~1%"=="" (
    echo [Error] Invalid platform toolset: %TOOLSET_VALUE%
    goto fail
)
set "PLATFORM_TOOLSET=%TOOLSET_VALUE%"
shift
goto parse_args

:args_done
if not defined PLATFORM_TOOLSET (
    echo [Error] PlatformToolset is empty.
    goto fail
)

echo ==========================================
echo    XBase Builder
echo ==========================================
echo Configuration: %CONFIG%
echo Platform: Win32
echo PlatformToolset: %PLATFORM_TOOLSET%
echo.

call :find_premake
if not defined PREMAKE_EXE (
    echo [Error] premake5 executable not found.
    goto fail
)

call :detect_plugin_sdk
if not defined PLUGIN_SDK_DIR (
    echo [Error] PLUGIN_SDK_DIR is required for all XBase targets.
    goto fail
)

call :find_msbuild
if not defined MSBUILD_EXE (
    echo [Error] MSBuild.exe not found.
    goto fail
)
call :build_plugin_sdk
if errorlevel 1 goto fail

if not exist "build" mkdir "build"

echo Removing stale generated project files...
del /Q "build\*.sln" "build\*.vcxproj" "build\*.vcxproj.filters" "build\*.vcxproj.user" >nul 2>nul

echo Generating Visual Studio 2022 project files...
"!PREMAKE_EXE!" vs2022
if errorlevel 1 (
    echo [Error] Project generation failed.
    goto fail
)

if not exist "build\XBase.sln" (
    echo [Error] build\XBase.sln not found.
    goto fail
)
for %%T in (XBaseBootstrap XBasePayloadEntry XBaseRuntimeEntry XBaseModEntry XBaseSA XBaseVC XBaseIII) do (
    if not exist "build\%%T.vcxproj" (
        echo [Error] build\%%T.vcxproj was not generated.
        goto fail
    )
)

call :find_msbuild
if not defined MSBUILD_EXE (
    echo [Error] MSBuild.exe not found.
    goto fail
)

echo.
echo Using premake: !PREMAKE_EXE!
echo Using MSBuild: !MSBUILD_EXE!

"!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseBootstrap /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
if errorlevel 1 (
    echo.
    echo [Error] XBaseBootstrap build failed.
    goto fail
)

"!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBasePayloadEntry /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
if errorlevel 1 (
    echo.
    echo [Error] XBasePayloadEntry build failed.
    goto fail
)

"!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseRuntimeEntry /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
if errorlevel 1 (
    echo.
    echo [Error] XBaseRuntimeEntry build failed.
    goto fail
)

"!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseModEntry /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
if errorlevel 1 (
    echo.
    echo [Error] XBaseModEntry build failed.
    goto fail
)

"!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseSA /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
if errorlevel 1 (
    echo.
    echo [Error] XBaseSA build failed.
    goto fail
)

"!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseVC /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
if errorlevel 1 (
    echo.
    echo [Error] XBaseVC build failed.
    goto fail
)

"!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseIII /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
if errorlevel 1 (
    echo.
    echo [Error] XBaseIII build failed.
    goto fail
)

for %%T in (XBaseBootstrap XBasePayloadEntry XBaseRuntimeEntry XBaseModEntry XBaseSA XBaseVC XBaseIII) do (
    if not exist "build\bin\%CONFIG%\%%T.lib" (
        echo [Error] build\bin\%CONFIG%\%%T.lib was not produced.
        goto fail
    )
)

if /i "%CONFIG%"=="Release" (
    rem 共享运行时只出 Release：plugin-sdk 的 output/lib 只有 Release 库，
    rem Debug 链接会因运行库与迭代器调试级别不匹配而失败
    "!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseRuntimeSA /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
    if errorlevel 1 (
        echo.
        echo [Error] XBaseRuntimeSA build failed.
        goto fail
    )

    "!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseRuntimeVC /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
    if errorlevel 1 (
        echo.
        echo [Error] XBaseRuntimeVC build failed.
        goto fail
    )

    "!MSBUILD_EXE!" "build\XBase.sln" /m /t:XBaseRuntimeIII /p:Configuration=%CONFIG% /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /verbosity:minimal
    if errorlevel 1 (
        echo.
        echo [Error] XBaseRuntimeIII build failed.
        goto fail
    )

    for %%T in (XBaseSA XBaseVC XBaseIII) do (
        if not exist "build\bin\%CONFIG%\%%T.dll" (
            echo [Error] build\bin\%CONFIG%\%%T.dll was not produced.
            goto fail
        )
    )


    call :stage_sdk "..\XMenu"
    call :stage_sdk "..\III.VC.SA.WebView2"
    rem example 是 XBase 自己的子目录，不是兄弟工程，路径不带上级
    call :stage_sdk "example"
)

rem 查看器与 SDK 一起构建，产物落在 viewer\build\bin\XBase.exe
call :build_viewer "%CONFIG%"

echo.
echo Build completed successfully.
echo Outputs: XBaseBootstrap.lib, XBasePayloadEntry.lib, XBaseSA.lib, XBaseVC.lib, XBaseIII.lib
echo           XBaseRuntimeEntry.lib, XBaseModEntry.lib
echo           XBaseSA.dll, XBaseVC.dll, XBaseIII.dll
goto success

rem ============================================================
rem 构建 XBase 日志与配置查看器
rem ============================================================
:build_viewer
if not exist "viewer\Build.bat" (
    echo [Warning] viewer\Build.bat not found; viewer was not built.
    exit /b 0
)
if not exist "viewer\src\main.cpp" (
    echo [Warning] viewer\src\main.cpp not found; viewer was not built.
    exit /b 0
)
echo Building XBase viewer...
call "viewer\Build.bat" "%~1" --no-pause
if errorlevel 1 (
    echo [Warning] XBase viewer build failed; continuing without it.
    exit /b 0
)
if exist "viewer\build\bin\XBase.exe" (
    echo [Info] XBase viewer built: viewer\build\bin\XBase.exe
) else (
    echo [Warning] XBase viewer executable was not produced.
)
exit /b 0

rem ============================================================
rem Stage the Release SDK for every sibling host that consumes it.
rem ============================================================
:stage_sdk
set "SDK_TARGET=%~1"
if not exist "%SDK_TARGET%\" exit /b 0
if not exist "%SDK_TARGET%\include\XBase" mkdir "%SDK_TARGET%\include\XBase"
if not exist "%SDK_TARGET%\lib" mkdir "%SDK_TARGET%\lib"
xcopy "include\XBase\*.h" "%SDK_TARGET%\include\XBase\" /Y /Q >nul
if errorlevel 1 (
    echo [Error] Failed to stage XBase public headers into %SDK_TARGET%.
    exit /b 1
)
for %%T in (XBaseBootstrap XBasePayloadEntry XBaseRuntimeEntry XBaseModEntry XBaseSA XBaseVC XBaseIII) do (
    copy /Y "build\bin\%CONFIG%\%%T.lib" "%SDK_TARGET%\lib\%%T.lib" >nul
    if errorlevel 1 (
        echo [Error] Failed to stage %%T.lib into %SDK_TARGET%.
        exit /b 1
    )
)
for %%P in (Plugin Plugin_VC Plugin_III) do (
    set "P_STAGE_NAME=%%P"
    set "P_SOURCE=%%P"
    if /i "%%P"=="Plugin" set "P_STAGE_NAME=PluginSA"
    if /i "%%P"=="Plugin_VC" set "P_STAGE_NAME=PluginVC"
    if /i "%%P"=="Plugin_III" set "P_STAGE_NAME=PluginIII"
    if not exist "%PLUGIN_SDK_DIR%\output\lib\!P_SOURCE!.lib" (
        echo [Error] Missing plugin-sdk game symbol library: !P_SOURCE!.lib
        exit /b 1
    )
    copy /Y "%PLUGIN_SDK_DIR%\output\lib\!P_SOURCE!.lib" "%SDK_TARGET%\lib\!P_STAGE_NAME!.lib" >nul
    if errorlevel 1 (
        echo [Error] Failed to stage !P_STAGE_NAME!.lib into %SDK_TARGET%.
        exit /b 1
    )
)
if exist "include\webview2\x86\WebView2Loader.dll" (
    copy /Y "include\webview2\x86\WebView2Loader.dll" "%SDK_TARGET%\lib\WebView2Loader.dll" >nul
    if errorlevel 1 (
        echo [Error] Failed to stage WebView2Loader.dll into %SDK_TARGET%.
        exit /b 1
    )
) else (
    echo [Warning] include\webview2\x86\WebView2Loader.dll not found; WebView feature will be unavailable.
)
echo [Info] Staged Release SDK to %SDK_TARGET%\include\XBase and %SDK_TARGET%\lib
exit /b 0

:find_premake
set "PREMAKE_EXE="
if exist "tools\premake5.exe" set "PREMAKE_EXE=%~dp0tools\premake5.exe" & goto :eof
if exist "..\XMenu\tools\premake5.exe" set "PREMAKE_EXE=%~dp0..\XMenu\tools\premake5.exe" & goto :eof
for /f "tokens=*" %%i in ('where premake5.exe 2^>nul') do if not defined PREMAKE_EXE set "PREMAKE_EXE=%%i"
if not defined PREMAKE_EXE (
    for /f "tokens=*" %%i in ('where premake5 2^>nul') do if not defined PREMAKE_EXE set "PREMAKE_EXE=%%i"
)
exit /b 0

:detect_plugin_sdk
if "%PLUGIN_SDK_DIR%"=="" (
    if exist "..\plugin-sdk\" (
        set "PLUGIN_SDK_DIR=%~dp0..\plugin-sdk"
        echo [Info] Auto-detected PLUGIN_SDK_DIR=!PLUGIN_SDK_DIR!
    ) else if exist "..\..\plugin-sdk\" (
        set "PLUGIN_SDK_DIR=%~dp0..\..\plugin-sdk"
        echo [Info] Auto-detected PLUGIN_SDK_DIR=!PLUGIN_SDK_DIR!
    ) else (
        echo [Error] plugin-sdk not found. Three-version backends require PLUGIN_SDK_DIR.
    )
)
if not "%PLUGIN_SDK_DIR%"=="" (
    if exist "%PLUGIN_SDK_DIR%" (
        echo [Info] Using plugin-sdk directory: %PLUGIN_SDK_DIR%
    ) else (
        echo [Warning] Ignoring invalid PLUGIN_SDK_DIR: %PLUGIN_SDK_DIR%
        set "PLUGIN_SDK_DIR="
        echo [Error] Three-version backends cannot be generated without plugin-sdk.
    )
)
exit /b 0

:build_plugin_sdk
rem XBase links plugin-sdk game symbols; build missing static libs before premake.
set "PSDK_LIB_DIR=!PLUGIN_SDK_DIR!\output\lib"
set "PSDK_NEED_BUILD="
if not exist "!PSDK_LIB_DIR!\Plugin.lib" set "PSDK_NEED_BUILD=1"
if not exist "!PSDK_LIB_DIR!\Plugin_VC.lib" set "PSDK_NEED_BUILD=1"
if not exist "!PSDK_LIB_DIR!\Plugin_III.lib" set "PSDK_NEED_BUILD=1"
if not defined PSDK_NEED_BUILD exit /b 0

echo Building plugin-sdk game symbol libraries...
if not exist "!PSDK_LIB_DIR!" mkdir "!PSDK_LIB_DIR!"
for %%P in ("plugin_sa\Plugin_SA" "plugin_vc\Plugin_VC" "plugin_III\Plugin_III") do (
    echo Building %%~P...
    "!MSBUILD_EXE!" "!PLUGIN_SDK_DIR!\%%~P.vcxproj" /m /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=!PLATFORM_TOOLSET! /p:PLUGIN_SDK_DIR=!PLUGIN_SDK_DIR! /verbosity:minimal
    if errorlevel 1 (
        echo [Error] plugin-sdk build failed: %%~P
        exit /b 1
    )
)
exit /b 0

:find_msbuild
if defined MSBUILD_EXE if exist "!MSBUILD_EXE!" exit /b 0
set "MSBUILD_EXE="
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD_EXE=%VSINSTALLDIR%MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD_EXE (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
            if not defined MSBUILD_EXE set "MSBUILD_EXE=%%i"
        )
    )
)
if not defined MSBUILD_EXE (
    for %%i in (
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    ) do if exist %%~si if not defined MSBUILD_EXE set "MSBUILD_EXE=%%~si"
)
if not defined MSBUILD_EXE (
    for /f "tokens=*" %%i in ('where MSBuild.exe 2^>nul') do if not defined MSBUILD_EXE set "MSBUILD_EXE=%%i"
)
exit /b 0

:success
popd
if not defined NO_PAUSE pause
exit /b 0

:fail
popd
if not defined NO_PAUSE pause
exit /b 1
