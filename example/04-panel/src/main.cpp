// 通用面板骨架：只描述界面并挂钩子，前端由 XBase 自带，本示例一行网页代码都没有
// 界面挂到共享运行时里，多个模组会聚合到同一个面板的侧栏上

#include <XBase/Config.h>
#include <XBase/Core.h>
#include <XBase/Host.h>
#include <XBase/Input.h>
#include <XBase/Log.h>
#include <XBase/Panel.h>
#include <XBase/Player.h>

#include <string>

namespace {

constexpr const char* kModName = "PanelSample";
constexpr const char* kKeyGodMode = "godMode";
constexpr const char* kKeyScale = "scale";
constexpr const char* kKeyMode = "mode";
constexpr XBase::Input::Key kPanelHotkey = XBase::Input::Key::F7;

bool g_godMode = false;
double g_scale = 1.0;
double g_mode = 1.0;

XBase::Panel::ModSpec BuildSpec() {
    XBase::Panel::ModSpec spec;
    spec.modId = kModName;
    spec.title = "面板示例";
    spec.subtitle = "只挂钩子，不写前端";
    spec.version = "v0.1.0";

    XBase::Panel::Page page;
    page.id = "main";
    page.label = "主页面";

    XBase::Panel::Section player;
    player.id = "player";
    player.label = "玩家";
    player.hint = "整块跟着能力走，能力不支持时分区置灰";
    player.capability = XBase::FeatureCapability::PlayerProofs;

    XBase::Panel::Control godMode;
    godMode.kind = XBase::Panel::ControlKind::Toggle;
    godMode.id = "panelsample.godMode";
    godMode.label = "无敌";
    godMode.hint = "每帧下发，游戏里改了也会同步回界面";
    player.controls.push_back(godMode);

    XBase::Panel::Section tuning;
    tuning.id = "tuning";
    tuning.label = "参数";
    tuning.columns = 2;

    XBase::Panel::Control scale;
    scale.kind = XBase::Panel::ControlKind::Float;
    scale.id = "panelsample.scale";
    scale.label = "倍率";
    scale.bounded = true;
    scale.min = 0.0;
    scale.max = 5.0;
    scale.step = 0.1;
    scale.format = "%.1fx";
    tuning.controls.push_back(scale);

    XBase::Panel::Control mode;
    mode.kind = XBase::Panel::ControlKind::Select;
    mode.id = "panelsample.mode";
    mode.label = "模式";
    // 读写的是下标，value 只给模组自己看
    mode.options.push_back(XBase::Panel::Option{"slow", "慢"});
    mode.options.push_back(XBase::Panel::Option{"normal", "正常"});
    mode.options.push_back(XBase::Panel::Option{"fast", "快"});
    tuning.controls.push_back(mode);

    XBase::Panel::Section actions;
    actions.id = "actions";
    actions.label = "动作";
    actions.inlineLayout = true;
    actions.columns = 3;

    XBase::Panel::Control printLog;
    printLog.kind = XBase::Panel::ControlKind::Action;
    printLog.id = "panelsample.printLog";
    printLog.label = "写一行日志";
    actions.controls.push_back(printLog);

    XBase::Panel::Control reset;
    reset.kind = XBase::Panel::ControlKind::Action;
    reset.id = "panelsample.reset";
    reset.label = "恢复默认";
    actions.controls.push_back(reset);

    page.sections.push_back(player);
    page.sections.push_back(tuning);
    page.sections.push_back(actions);
    spec.pages.push_back(page);
    return spec;
}

bool BindHooks() {
    bool ok = true;

    ok = XBase::Panel::BindValue(
             "panelsample.godMode",
             [] { return g_godMode ? 1.0 : 0.0; },
             [](double value) {
                 g_godMode = value != 0.0;
                 XBase::Config::SetBool(kKeyGodMode, g_godMode);
                 XBase::Config::Save();
             })
        && ok;

    ok = XBase::Panel::BindValue(
             "panelsample.scale",
             [] { return g_scale; },
             [](double value) {
                 g_scale = value;
                 XBase::Config::SetString(kKeyScale, std::to_string(g_scale));
                 XBase::Config::Save();
             })
        && ok;

    ok = XBase::Panel::BindValue(
             "panelsample.mode",
             [] { return g_mode; },
             [](double value) {
                 g_mode = value;
                 XBase::Config::SetString(kKeyMode, std::to_string(g_mode));
                 XBase::Config::Save();
             })
        && ok;

    ok = XBase::Panel::BindAction("panelsample.printLog", [] {
             XBase::Log::Info(std::string("面板动作：倍率 ") + std::to_string(g_scale)
                              + "，模式下标 " + std::to_string(static_cast<int>(g_mode)));
         })
        && ok;

    ok = XBase::Panel::BindAction("panelsample.reset", [] {
             g_scale = 1.0;
             g_mode = 1.0;
             XBase::Config::SetString(kKeyScale, std::to_string(g_scale));
             XBase::Config::SetString(kKeyMode, std::to_string(g_mode));
             XBase::Config::Save();
             // 模组自己改的状态要推回界面，否则网页还停在旧值上
             XBase::Panel::NotifyChanged("panelsample.scale", g_scale);
             XBase::Panel::NotifyChanged("panelsample.mode", g_mode);
             XBase::Host::QueueMessage("已恢复默认参数");
         })
        && ok;

    return ok;
}

void OnGameInit() {
    g_godMode = XBase::Config::GetBool(kKeyGodMode, false);
    g_scale = XBase::Config::GetString(kKeyScale, "1.0").empty()
        ? 1.0
        : std::stod(XBase::Config::GetString(kKeyScale, "1.0"));
    g_mode = std::stod(XBase::Config::GetString(kKeyMode, "1"));

    if (!XBase::Panel::IsAvailable()) {
        XBase::Log::Warn("面板不可用，检查 XBase\\Library\\panel 与网页视图运行时");
        return;
    }

    // 先挂载结构再绑钩子，绑定到的控件必须已经在注册表里
    if (!XBase::Panel::Mount(BuildSpec()) || !BindHooks()) {
        XBase::Log::Error("面板挂载失败");
        return;
    }

    XBase::Panel::SetHotkey(XBase::Input::Hotkey{kPanelHotkey, 0});
    XBase::Log::Info("面板已挂载，按 F7 开关");
}

void OnProcess() {
    if (!XBase::Core::IsWorldReady()) return;
    if (!XBase::HasCapability(XBase::FeatureCapability::PlayerProofs)) return;

    // 每帧下发，游戏里被别的东西改掉时界面也能跟上
    XBase::Player::SetGodMode(g_godMode);
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

    if (!XBase::Host::Install({OnGameInit, OnProcess})) {
        XBase::Log::Error("注册生命周期失败");
        return;
    }

    XBase::Log::Info("生命周期注册完成");
}

extern "C" __declspec(dllexport) void XBasePayloadDetach() {
    XBase::Panel::Unmount(kModName);
    XBase::Host::Shutdown();
    XBase::Log::Shutdown();
}
