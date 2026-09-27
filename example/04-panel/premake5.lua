-- 单文件 asi 骨架，asi 自带业务代码，不需要额外的 payload dll
-- 三个游戏版本共用同一份源码，版本差异由 Bootstrap 加载的共享运行时承担

workspace "PanelSample"
    configurations { "Debug", "Release" }
    architecture "x86"
    platforms "Win32"
    language "C++"
    cppdialect "C++20"
    characterset "MBCS"
    staticruntime "On"
    location "build"
    targetdir "build/bin"

    toolset "msc"
    buildoptions { "/utf-8", "/FS" }

    defines {
        "IS_PLATFORM_WIN",
        "_CRT_SECURE_NO_WARNINGS",
        "_CRT_NON_CONFORMING_SWPRINTFS"
    }

-- 公共头与库由 XBase 构建统一 stage 到 example 目录，所有示例共用同一份
-- 把示例拷出去单独使用时，把这两行改成自己的 SDK 位置
local XBASE_INCLUDE_DIR = "../include"
local XBASE_LIB_DIR = "../lib"

function configureBuildMode()
    filter "configurations:Debug"
        symbols "On"
        defines { "DEBUG" }

    filter "configurations:Release"
        optimize "On"
        defines { "NDEBUG" }

    filter {}
end

-- 每个游戏版本产出一个 asi，三个 asi 互不依赖，装哪个跑哪个
function createModProject(projectID)
    local upperID = string.upper(projectID)
    local gameDefine = upperID == "III" and "GTA3" or "GTA" .. upperID

    project ("PanelSample" .. upperID)
        kind "SharedLib"
        targetname ("PanelSample" .. upperID)
        targetextension ".asi"
        targetdir "build/bin"

        includedirs {
            XBASE_INCLUDE_DIR,
            "src"
        }

        files {
            "src/**.h",
            "src/**.cpp",
            "../include/XBase/**.h"
        }

        defines { gameDefine }
        libdirs { XBASE_LIB_DIR }

        -- 入口库必须整包拉入，否则链接器会丢掉带 DllMain 的对象
        links { "XBaseModEntry", "XBase" .. upperID, "Plugin" .. upperID }
        linkoptions { "/WHOLEARCHIVE:XBaseModEntry.lib" }

        configureBuildMode()
end

createModProject("sa")
createModProject("vc")
createModProject("iii")
