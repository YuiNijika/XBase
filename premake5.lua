workspace "XBase"
    configurations { "Debug", "Release" }
    platforms { "Win32" }
    architecture "x86"
    language "C++"
    cppdialect "C++20"
    characterset "MBCS"
    staticruntime "On"
    toolset "msc"
    buildoptions { "/utf-8", "/FS" }
    location "build"
    targetdir "build/bin/%{cfg.buildcfg}"
    objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

    local pluginSdkDir = os.getenv("PLUGIN_SDK_DIR")
    if pluginSdkDir == nil or pluginSdkDir == "" then
        if os.isdir("../plugin-sdk") then
            pluginSdkDir = "../plugin-sdk"
        elseif os.isdir("../../plugin-sdk") then
            pluginSdkDir = "../../plugin-sdk"
        end
    end

    local hasPluginSdk = false
    if pluginSdkDir ~= nil and pluginSdkDir ~= "" and os.isdir(pluginSdkDir) then
        hasPluginSdk = true
    end

    if not hasPluginSdk then
        error("plugin-sdk is required to build XBaseSA, XBaseVC, and XBaseIII")
    end

    local hasKiero = os.isdir("include/kiero")

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_CRT_NON_CONFORMING_SWPRINTFS",
    }

-- 入口库会被 WHOLEARCHIVE 整包拉进 asi，且有的宿主只链入口库（例如 WebView2 的 loader），
-- 所以 Bootstrap.cpp 用到的东西必须一起进来：挂载前要读 mod 清单做约束校验，
-- 清单链路是 Package + Json + Platform，三者都只依赖 Win32 与标准库，不碰 plugin-sdk
local ENTRY_SUPPORT_SOURCES = {
    "src/controllers/Package.cpp",
    "src/controllers/Json.cpp",
    "src/controllers/Platform.cpp",
}

local function add_entry_target(name, sources)
    project(name)
        kind "StaticLib"
        targetname(name)
        files(sources)
        files(ENTRY_SUPPORT_SOURCES)
        -- 入口库要能拿到 XBase/Abi.h 才能把函数表交给适配层
        includedirs { "include" }

        filter "configurations:Debug"
            defines { "DEBUG" }
            optimize "Off"
            symbols "On"
        filter "configurations:Release"
            defines { "NDEBUG" }
            optimize "Speed"
            symbols "Off"
        filter {}
end

add_entry_target("XBaseBootstrap", {
    "src/Bootstrap.h",
    "src/Bootstrap.cpp",
    "src/BootstrapEntry.cpp"
})

add_entry_target("XBasePayloadEntry", {
    "src/PayloadEntry.cpp"
})

-- XBase.asi 用：只引导共享运行时，不找 payload
add_entry_target("XBaseRuntimeEntry", {
    "src/Bootstrap.h",
    "src/Bootstrap.cpp",
    "src/RuntimeEntry.cpp"
})

-- 单文件 mod 的 asi 用：引导共享运行时后直接跑本模块的业务入口
add_entry_target("XBaseModEntry", {
    "src/Bootstrap.h",
    "src/Bootstrap.cpp",
    "src/ModEntry.cpp"
})

local function add_sa_settings()
    if hasPluginSdk then
        defines { "XBASE_WITH_PLUGIN_SDK", "XBASE_BACKEND_SA", "GTASA", "_GTA_", "RW", "IS_PLATFORM_WIN" }
        includedirs {
            "include",
            "src/backends",
            path.join(pluginSdkDir, "plugin_sa"),
            path.join(pluginSdkDir, "plugin_sa", "game_sa"),
            path.join(pluginSdkDir, "plugin_sa", "game_sa", "enums"),
            path.join(pluginSdkDir, "plugin_sa", "game_sa", "rw"),
            path.join(pluginSdkDir, "shared"),
            path.join(pluginSdkDir, "shared", "game"),
            path.join(pluginSdkDir, "shared", "dxsdk"),
            path.join(pluginSdkDir, "stb")
        }
    end
    if hasKiero then
        defines { "XBASE_WITH_KIERO" }
        includedirs { "include/imgui", "include/kiero" }
    end
end

local function add_portable_player_target(name, sdkName, gameName, gameDefine, backendDefine)
    project(name)
        kind "StaticLib"
        targetname(name)
        files {
            "include/XBase/**.h",
            "src/backends/BulletAssistBackend.h",
            "src/backends/BulletAssistBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/RuntimeGuard_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/PlayerBackend.h",
            "src/backends/PlayerBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/PedBackend.h",
            "src/backends/PedBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/SceneBackend.h",
            "src/backends/SceneBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/VehicleBackend.h",
            "src/backends/VehicleBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/WeaponBackend.h",
            "src/backends/WeaponBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/WorldBackend.h",
            "src/backends/WorldBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/VisualBackend.h",
            "src/backends/VisualBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/TeleportBackend.h",
            "src/backends/TeleportBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/backends/CheatsBackend.h",
            "src/backends/CheatsBackend_" .. string.lower(sdkName) .. ".cpp",
            "src/controllers/Capabilities.cpp",
            "src/controllers/BulletAssist.cpp",
            "src/controllers/CheatsPortable.cpp",
            "src/controllers/Config.cpp",
            "src/controllers/CoreStub.cpp",
            "src/controllers/Panel.cpp",
            "src/controllers/I18n.cpp",
            "src/controllers/Json.cpp",
            "src/controllers/Log.cpp",
            "src/controllers/Hooks.cpp",
            "src/controllers/Input.cpp",
            "src/controllers/Overlay.cpp",
            "src/controllers/Hotkey.cpp",
            "src/controllers/Host.cpp",
            "src/controllers/Platform.cpp",
            "src/controllers/Package.cpp",
            "src/controllers/Runtime.cpp",
            "src/controllers/RenderFonts.cpp",
            "src/controllers/RenderFonts.h",
            "src/controllers/Theme.cpp",
            "src/controllers/UI.cpp",
            "src/controllers/WebView.cpp",
            "src/controllers/WebBridge.cpp",
            "src/controllers/PlayerPortable.cpp",
            "src/controllers/PedPortable.cpp",
            "src/controllers/ScenePortable.cpp",
            "src/controllers/PortableStubs.cpp",
            "src/controllers/VehiclePortable.cpp",
            "src/controllers/WeaponPortable.cpp",
            "src/controllers/WorldPortable.cpp",
            "src/controllers/VisualPortable.cpp",
            "src/controllers/TeleportPortable.cpp",
            "include/imgui/imgui.cpp",
            "include/imgui/imgui_draw.cpp",
            "include/imgui/imgui_tables.cpp",
            "include/imgui/imgui_widgets.cpp",
            "include/imgui/imgui_impl_win32.cpp",
            "include/imgui/imgui_impl_dx9.cpp",
            "include/kiero/kiero.cpp",
            "include/kiero/minhook/buffer.c",
            "include/kiero/minhook/hook.c",
            "include/kiero/minhook/trampoline.c",
            "include/kiero/minhook/hde/hde32.c"
        }
        includedirs {
            "include",
            "src/backends",
            "include/imgui",
            "include/kiero",
            path.join(pluginSdkDir, "plugin_" .. sdkName),
            path.join(pluginSdkDir, "plugin_" .. sdkName, "game_" .. gameName),
            path.join(pluginSdkDir, "plugin_" .. sdkName, "game_" .. gameName, "enums"),
            path.join(pluginSdkDir, "plugin_" .. sdkName, "game_" .. gameName, "rw"),
            path.join(pluginSdkDir, "shared"),
            path.join(pluginSdkDir, "shared", "game"),
            path.join(pluginSdkDir, "shared", "dxsdk"),
            path.join(pluginSdkDir, "stb")
        }
        defines {
            "XBASE_WITH_PLUGIN_SDK",
            "XBASE_WITH_KIERO",
            backendDefine,
            gameDefine,
            "_GTA_",
            "RW",
            "IS_PLATFORM_WIN"
        }

        filter "configurations:Debug"
            defines { "DEBUG" }
            optimize "Off"
            symbols "On"
        filter "configurations:Release"
            defines { "NDEBUG" }
            optimize "Speed"
            symbols "Off"
        filter {}
end

project "XBaseSA"
    kind "StaticLib"
    targetname "XBaseSA"
    files {
        "include/XBase/**.h",
        "src/**.h",
        "src/controllers/*.cpp",
        "src/controllers/Camera.cpp",
        "src/controllers/Cheats.cpp",
        "src/controllers/VehicleEffects.cpp",
        "src/backends/BulletAssistBackend_sa.cpp",
        "src/backends/RuntimeGuard_sa.cpp",
        "include/imgui/imgui.cpp",
        "include/imgui/imgui_draw.cpp",
        "include/imgui/imgui_tables.cpp",
        "include/imgui/imgui_widgets.cpp",
        "include/imgui/imgui_impl_win32.cpp",
        "include/imgui/imgui_impl_dx9.cpp",
        "include/kiero/kiero.cpp",
        "include/kiero/minhook/buffer.c",
        "include/kiero/minhook/hook.c",
        "include/kiero/minhook/trampoline.c",
        "include/kiero/minhook/hde/hde32.c"
    }
    removefiles {
        "src/main.cpp",
        "src/controllers/CoreStub.cpp",
        -- 导出入口只属于共享运行时，静态库里编进去会出现重复定义
        "src/controllers/RuntimeExport.cpp",
        "src/controllers/PlayerPortable.cpp",
        "src/controllers/PedPortable.cpp",
        "src/controllers/ScenePortable.cpp",
        "src/controllers/PortableStubs.cpp",
        "src/controllers/CheatsPortable.cpp",
        "src/controllers/VehiclePortable.cpp",
        "src/controllers/WeaponPortable.cpp",
        "src/controllers/WorldPortable.cpp",
        "src/controllers/VisualPortable.cpp",
        "src/controllers/TeleportPortable.cpp",
        "src/backends/PlayerBackend_vc.cpp",
        "src/backends/PlayerBackend_iii.cpp",
        "src/backends/VehicleBackend_vc.cpp",
        "src/backends/VehicleBackend_iii.cpp",
        "src/backends/WeaponBackend_vc.cpp",
        "src/backends/WeaponBackend_iii.cpp",
        "src/backends/WorldBackend_vc.cpp",
        "src/backends/WorldBackend_iii.cpp",
        "src/backends/VisualBackend_vc.cpp",
        "src/backends/VisualBackend_iii.cpp",
        "src/backends/TeleportBackend_vc.cpp",
        "src/backends/TeleportBackend_iii.cpp"
    }
    add_sa_settings()

    filter "configurations:Debug"
        defines { "DEBUG" }
        optimize "Off"
        symbols "On"
    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "Speed"
        symbols "Off"
    filter {}

add_portable_player_target("XBaseVC", "vc", "vc", "GTAVC", "XBASE_BACKEND_VC")
add_portable_player_target("XBaseIII", "III", "III", "GTA3", "XBASE_BACKEND_III")

-- 共享运行时：进程内只加载一份，mod 通过函数表接入。
-- 产物名与静态库同名（XBaseSA.dll 对 XBaseSA.lib），所以导入库必须另起名字，
-- 否则链接器生成的 XBaseSA.lib 会覆盖掉静态库。
local function add_runtime_target(projectName, targetName, sdkName, gameName, gameDefine, backendDefine, portable, sdkLib)
    project(projectName)
        kind "SharedLib"
        targetname(targetName)
        targetextension ".dll"
        implibname(projectName)

        -- 静态库不需要解析符号，动态库必须把 plugin-sdk 链进来，
        -- 也只有这里链一份，mod 侧就不再各自带一份 plugin-sdk 全局
        libdirs { path.join(pluginSdkDir, "output", "lib") }
        links { sdkLib }

        if portable then
            files {
                "include/XBase/**.h",
                "src/backends/BulletAssistBackend.h",
                "src/backends/BulletAssistBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/RuntimeGuard_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/PlayerBackend.h",
                "src/backends/PlayerBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/PedBackend.h",
                "src/backends/PedBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/SceneBackend.h",
                "src/backends/SceneBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/VehicleBackend.h",
                "src/backends/VehicleBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/WeaponBackend.h",
                "src/backends/WeaponBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/WorldBackend.h",
                "src/backends/WorldBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/VisualBackend.h",
                "src/backends/VisualBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/TeleportBackend.h",
                "src/backends/TeleportBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/backends/CheatsBackend.h",
                "src/backends/CheatsBackend_" .. string.lower(sdkName) .. ".cpp",
                "src/controllers/Capabilities.cpp",
                "src/controllers/BulletAssist.cpp",
                "src/controllers/CheatsPortable.cpp",
                "src/controllers/Config.cpp",
                "src/controllers/CoreStub.cpp",
                "src/controllers/Panel.cpp",
                "src/controllers/I18n.cpp",
                "src/controllers/Json.cpp",
                "src/controllers/Log.cpp",
                "src/controllers/Hooks.cpp",
                "src/controllers/Input.cpp",
                "src/controllers/Overlay.cpp",
                "src/controllers/Hotkey.cpp",
                "src/controllers/Host.cpp",
                "src/controllers/Platform.cpp",
                "src/controllers/Runtime.cpp",
                "src/controllers/RenderFonts.cpp",
                "src/controllers/RenderFonts.h",
                "src/controllers/Theme.cpp",
                "src/controllers/UI.cpp",
                "src/controllers/WebView.cpp",
                "src/controllers/WebBridge.cpp",
                "src/controllers/PlayerPortable.cpp",
                "src/controllers/PedPortable.cpp",
                "src/controllers/ScenePortable.cpp",
                "src/controllers/PortableStubs.cpp",
                "src/controllers/VehiclePortable.cpp",
                "src/controllers/WeaponPortable.cpp",
                "src/controllers/WorldPortable.cpp",
                "src/controllers/VisualPortable.cpp",
                "src/controllers/TeleportPortable.cpp",
                "src/controllers/RuntimeExport.cpp",
                "include/imgui/imgui.cpp",
                "include/imgui/imgui_draw.cpp",
                "include/imgui/imgui_tables.cpp",
                "include/imgui/imgui_widgets.cpp",
                "include/imgui/imgui_impl_win32.cpp",
                "include/imgui/imgui_impl_dx9.cpp",
                "include/kiero/kiero.cpp",
                "include/kiero/minhook/buffer.c",
                "include/kiero/minhook/hook.c",
                "include/kiero/minhook/trampoline.c",
                "include/kiero/minhook/hde/hde32.c"
            }
        else
            files {
                "include/XBase/**.h",
                "src/**.h",
                "src/controllers/*.cpp",
                "src/backends/BulletAssistBackend_sa.cpp",
                "src/backends/RuntimeGuard_sa.cpp",
                "src/controllers/RuntimeExport.cpp",
                "include/imgui/imgui.cpp",
                "include/imgui/imgui_draw.cpp",
                "include/imgui/imgui_tables.cpp",
                "include/imgui/imgui_widgets.cpp",
                "include/imgui/imgui_impl_win32.cpp",
                "include/imgui/imgui_impl_dx9.cpp",
                "include/kiero/kiero.cpp",
                "include/kiero/minhook/buffer.c",
                "include/kiero/minhook/hook.c",
                "include/kiero/minhook/trampoline.c",
                "include/kiero/minhook/hde/hde32.c"
            }
            removefiles {
                "src/main.cpp",
                "src/controllers/CoreStub.cpp",
                "src/controllers/PlayerPortable.cpp",
                "src/controllers/PedPortable.cpp",
                "src/controllers/ScenePortable.cpp",
                "src/controllers/PortableStubs.cpp",
                "src/controllers/CheatsPortable.cpp",
                "src/controllers/VehiclePortable.cpp",
                "src/controllers/WeaponPortable.cpp",
                "src/controllers/WorldPortable.cpp",
                "src/controllers/VisualPortable.cpp",
                "src/controllers/TeleportPortable.cpp",
                "src/backends/PlayerBackend_vc.cpp",
                "src/backends/PlayerBackend_iii.cpp",
                "src/backends/VehicleBackend_vc.cpp",
                "src/backends/VehicleBackend_iii.cpp",
                "src/backends/WeaponBackend_vc.cpp",
                "src/backends/WeaponBackend_iii.cpp",
                "src/backends/WorldBackend_vc.cpp",
                "src/backends/WorldBackend_iii.cpp",
                "src/backends/VisualBackend_vc.cpp",
                "src/backends/VisualBackend_iii.cpp",
                "src/backends/TeleportBackend_vc.cpp",
                "src/backends/TeleportBackend_iii.cpp"
            }
        end

        includedirs {
            "include",
            "src/backends",
            "include/imgui",
            "include/kiero",
            path.join(pluginSdkDir, "plugin_" .. sdkName),
            path.join(pluginSdkDir, "plugin_" .. sdkName, "game_" .. gameName),
            path.join(pluginSdkDir, "plugin_" .. sdkName, "game_" .. gameName, "enums"),
            path.join(pluginSdkDir, "plugin_" .. sdkName, "game_" .. gameName, "rw"),
            path.join(pluginSdkDir, "shared"),
            path.join(pluginSdkDir, "shared", "game"),
            path.join(pluginSdkDir, "shared", "dxsdk"),
            path.join(pluginSdkDir, "stb")
        }
        defines {
            "XBASE_WITH_PLUGIN_SDK",
            "XBASE_WITH_KIERO",
            "XBASE_RUNTIME_DLL",
            backendDefine,
            gameDefine,
            "_GTA_",
            "RW",
            "IS_PLATFORM_WIN"
        }

        filter "configurations:Debug"
            defines { "DEBUG" }
            optimize "Off"
            symbols "On"
        filter "configurations:Release"
            defines { "NDEBUG" }
            optimize "Speed"
            symbols "Off"
        filter {}
end

add_runtime_target("XBaseRuntimeSA", "XBaseSA", "sa", "sa", "GTASA", "XBASE_BACKEND_SA", false, "Plugin")
add_runtime_target("XBaseRuntimeVC", "XBaseVC", "vc", "vc", "GTAVC", "XBASE_BACKEND_VC", true, "Plugin_VC")
add_runtime_target("XBaseRuntimeIII", "XBaseIII", "III", "III", "GTA3", "XBASE_BACKEND_III", true, "Plugin_III")
