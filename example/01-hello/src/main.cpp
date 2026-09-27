// 最小可用骨架，同一个 asi 在三个游戏版本上都能跑
// 三个导出决定 Bootstrap 怎么认识这个模组，缺一个就会退回按文件名推导

#include <XBase/Host.h>
#include <XBase/Log.h>
#include <XBase/Package.h>
#include <XBase/Platform.h>

#include <string>

namespace {

// 改这一个常量就能换掉模组名，数据目录与载荷名都跟着变
constexpr const char* kModName = "Hello";

int gInitCount = 0;
int gFrameCount = 0;

void OnGameInit() {
    ++gInitCount;
    XBase::Log::Info("游戏初始化完成，模组开始运行");
    XBase::Host::ShowMessage("Hello XBase 已加载");
}

void OnProcess() {
    ++gFrameCount;
}

} // namespace

extern "C" __declspec(dllexport) const char* XBasePayloadBaseName() {
    return kModName;
}

// 声明目标游戏后，另外两个版本的 asi 静默跳过，不会弹错误框
extern "C" __declspec(dllexport) const char* XBaseModTargetGame() {
#if defined(GTASA)
    return "SA";
#elif defined(GTAVC)
    return "VC";
#elif defined(GTA3)
    return "III";
#else
    return "";
#endif
}

// 导出这个入口即单文件形态，Bootstrap 探测到它就不再加载外部 payload
extern "C" __declspec(dllexport) void XBasePayloadAttach() {
    XBase::Log::InitForMod(kModName);
    XBase::Log::Info(std::string("数据目录 ") + XBase::Platform::ModDirectory(kModName));

    // 清单是身份信息的单一真源，版本与约束都从它读，源码里不写死
    XBase::Package::Info packageInfo;
    if (XBase::Package::Load(kModName, packageInfo) && packageInfo.valid) {
        XBase::Log::Info(std::string("清单载入 ") + packageInfo.name + " " + packageInfo.version
                         + "，要求 XBase " + packageInfo.xbaseRequirement);
    } else {
        XBase::Log::Warn("未找到模组清单，Bootstrap 会按无约束放行");
    }

    if (!XBase::Host::Install({OnGameInit, OnProcess})) {
        XBase::Log::Error("注册生命周期失败");
        return;
    }

    XBase::Log::Info("生命周期注册完成");
}

extern "C" __declspec(dllexport) void XBasePayloadDetach() {
    XBase::Host::Shutdown();
    XBase::Log::Info(std::string("共初始化 ") + std::to_string(gInitCount)
                     + " 次，运行 " + std::to_string(gFrameCount) + " 帧");
    XBase::Log::Shutdown();
}
