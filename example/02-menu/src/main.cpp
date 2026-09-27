// 带界面的骨架，展示菜单绘制、配置持久化、热键与每帧状态推送
// 开关类状态必须由宿主持有并每帧推给领域，只在点击时调一次会被下一帧覆盖

#include <XBase/Capabilities.h>
#include <XBase/Config.h>
#include <XBase/Core.h>
#include <XBase/Hooks.h>
#include <XBase/Host.h>
#include <XBase/Input.h>
#include <XBase/Log.h>
#include <XBase/Player.h>
#include <XBase/UI.h>

#include <string>

namespace {

constexpr const char* kModName = "Menu";
constexpr const char* kWindowId = "menu.main";
constexpr const char* kWindowTitle = "Menu 示例";
constexpr const char* kKeyGodMode = "godMode";
constexpr XBase::Input::Key kMenuHotkey = XBase::Input::Key::F7;

bool gGodMode = false;
bool gHooksReady = false;
XBase::Hooks::DrawCallbackId gDrawCallbackId;

void DrawMenu() {
    if (!XBase::Hooks::IsMenuVisible()) return;

    XBase::UI::Window(kWindowId, kWindowTitle, []() {
        if (XBase::UI::Checkbox("无敌", gGodMode)) {
            XBase::Config::SetBool(kKeyGodMode, gGodMode);
            XBase::Config::Save();
        }

        XBase::UI::TextDisabled("按 F7 开关本窗口");
    });
}

void OnGameInit() {
    if (gHooksReady) return;

    gDrawCallbackId = XBase::Hooks::RegisterDrawCallback(DrawMenu);
    gHooksReady = static_cast<bool>(gDrawCallbackId) && XBase::Hooks::Init();

    if (!gHooksReady) {
        XBase::Log::Error("渲染后端初始化失败，菜单不可用");
        return;
    }

    XBase::Hooks::SetMenuVisible(true);
    XBase::Host::ShowMessage("Menu 示例已加载，按 F7 开关菜单");
}

void OnProcess() {
    if (XBase::Input::WasPressed(kMenuHotkey)) {
        XBase::Hooks::ToggleMenu();
    }

    if (!XBase::Core::IsWorldReady()) return;

    // 能力不足时这个领域在当前游戏版本没有真实实现，入口该禁用而不是照常调用
    if (!XBase::HasCapability(XBase::FeatureCapability::PlayerBasicState)) return;

    XBase::Player::SetGodMode(gGodMode);
}

} // namespace

extern "C" __declspec(dllexport) const char* XBasePayloadBaseName() {
    return kModName;
}

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

extern "C" __declspec(dllexport) void XBasePayloadAttach() {
    XBase::Log::InitForMod(kModName);
    XBase::Config::InitForMod(kModName);

    gGodMode = XBase::Config::GetBool(kKeyGodMode, false);
    XBase::Log::Info(std::string("配置已载入，无敌状态 ") + (gGodMode ? "开" : "关"));

    if (!XBase::Host::Install({OnGameInit, OnProcess})) {
        XBase::Log::Error("注册生命周期失败");
        return;
    }

    XBase::Log::Info("生命周期注册完成");
}

extern "C" __declspec(dllexport) void XBasePayloadDetach() {
    XBase::Host::Shutdown();

    if (gDrawCallbackId) {
        XBase::Hooks::UnregisterDrawCallback(gDrawCallbackId);
        gDrawCallbackId = {};
    }

    XBase::Hooks::Shutdown();
    XBase::Log::Shutdown();
}
