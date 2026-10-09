// 通用面板示例：覆盖全部内置控件，以及 Custom 控件里的桥接 API 调用。
// 界面挂到共享运行时里，一个 ASI 对应一个 Sidebar，页面内可以有多个 Tab。

#include <XBase/Config.h>
#include <XBase/Core.h>
#include <XBase/Host.h>
#include <XBase/Input.h>
#include <XBase/Log.h>
#include <XBase/Panel.h>
#include <XBase/Player.h>

#include "components.h"

#include <atomic>
#include <string>

namespace {

constexpr const char* kModName = "PanelSample";
constexpr const char* kKeyGodMode = "godMode";
constexpr const char* kKeyScale = "scale";
constexpr const char* kKeyMode = "mode";
bool g_godMode = false;
double g_scale = 1.0;
double g_mode = 1.0;
double g_count = 10.0;
double g_color = 0x3366FF;
double g_progress = 62.5;
double g_radio = 1.0;
double g_flags = 5.0;
std::string g_title = "PanelSample";
std::string g_notes = "这里演示 Textarea 与 panel.textChanged。";
std::string g_status;
bool g_mounted = false;

enum class PanelCommand {
    None,
    Show,
    Hide,
    Toggle,
    Remount,
    Unmount,
    Status,
};

std::atomic<PanelCommand> g_pendingCommand{PanelCommand::None};

void RefreshStatus() {
    const XBase::Input::Hotkey hotkey = XBase::Panel::GetHotkey();
    g_status = std::string("Available: ") + (XBase::Panel::IsAvailable() ? "true" : "false")
        + "\nInitialized in this module: " + (XBase::Panel::IsInitialized() ? "true" : "false")
        + "\nVisible: " + (XBase::Panel::IsVisible() ? "true" : "false")
        + "\nHotkey: " + XBase::Input::FormatHotkey(hotkey);
    XBase::Panel::NotifyTextChanged("panelsample.status", g_status);
    XBase::Log::Info(g_status);
}

XBase::Panel::ModSpec BuildSpec() {
    XBase::Panel::ModSpec spec;
    spec.modId = kModName;
    spec.title = "面板示例";
    spec.subtitle = "全部控件与桥接方法";
    spec.version = "v0.2.0";

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

    XBase::Panel::Control count;
    count.kind = XBase::Panel::ControlKind::Int;
    count.id = "panelsample.count";
    count.label = "整数";
    count.bounded = true;
    count.min = 0.0;
    count.max = 100.0;
    count.step = 1.0;
    tuning.controls.push_back(count);

    XBase::Panel::Control color;
    color.kind = XBase::Panel::ControlKind::Color;
    color.id = "panelsample.color";
    color.label = "颜色";
    color.hint = "按 0xRRGGBB 数值绑定";
    tuning.controls.push_back(color);

    XBase::Panel::Control progress;
    progress.kind = XBase::Panel::ControlKind::Progress;
    progress.id = "panelsample.progress";
    progress.label = "进度";
    progress.min = 0.0;
    progress.max = 100.0;
    progress.format = "%.1f%%";
    progress.readOnly = true;
    tuning.controls.push_back(progress);

    XBase::Panel::Page controlsPage;
    controlsPage.id = "controls";
    controlsPage.label = "全部控件";

    XBase::Panel::Section choice;
    choice.id = "choice";
    choice.label = "选项控件";
    choice.columns = 2;

    XBase::Panel::Control radio;
    radio.kind = XBase::Panel::ControlKind::Radio;
    radio.id = "panelsample.radio";
    radio.label = "单选";
    radio.options.push_back(XBase::Panel::Option{"one", "选项一"});
    radio.options.push_back(XBase::Panel::Option{"two", "选项二"});
    radio.options.push_back(XBase::Panel::Option{"three", "选项三"});
    choice.controls.push_back(radio);

    XBase::Panel::Control flags;
    flags.kind = XBase::Panel::ControlKind::MultiSelect;
    flags.id = "panelsample.flags";
    flags.label = "多选";
    flags.options.push_back(XBase::Panel::Option{"a", "功能 A"});
    flags.options.push_back(XBase::Panel::Option{"b", "功能 B"});
    flags.options.push_back(XBase::Panel::Option{"c", "功能 C"});
    flags.options.push_back(XBase::Panel::Option{"d", "功能 D"});
    choice.controls.push_back(flags);

    XBase::Panel::Section text;
    text.id = "text";
    text.label = "文本控件";

    XBase::Panel::Control title;
    title.kind = XBase::Panel::ControlKind::Text;
    title.id = "panelsample.title";
    title.label = "标题";
    title.placeholder = "输入标题";
    text.controls.push_back(title);

    XBase::Panel::Control notes;
    notes.kind = XBase::Panel::ControlKind::Textarea;
    notes.id = "panelsample.notes";
    notes.label = "备注";
    notes.placeholder = "输入多行备注";
    text.controls.push_back(notes);

    XBase::Panel::Control heading;
    heading.kind = XBase::Panel::ControlKind::Heading;
    heading.id = "panelsample.heading";
    heading.label = "只读布局标题";
    text.controls.push_back(heading);

    XBase::Panel::Control separator;
    separator.kind = XBase::Panel::ControlKind::Separator;
    separator.id = "panelsample.separator";
    text.controls.push_back(separator);

    controlsPage.sections.push_back(choice);
    controlsPage.sections.push_back(text);
    spec.pages.push_back(controlsPage);

    XBase::Panel::Page bridgePage;
    bridgePage.id = "bridge";
    bridgePage.label = "桥接 API";

    XBase::Panel::Section bridge;
    bridge.id = "bridge";
    bridge.label = "前端方法";
    bridge.hint = "Custom 控件中的脚本直接调用 xbase.call/get/set/getText/setText/run/on";

    XBase::Panel::Control bridgeDemo;
    bridgeDemo.kind = XBase::Panel::ControlKind::Custom;
    bridgeDemo.id = "panelsample.bridge";
    bridgeDemo.label = "WebBridge";
    // Custom 同时支持内联 html/script/style 与独立资源文件。
    // 这里用文件引用演示推荐的项目组织方式，路径相对于 Mods\PanelSample\。
    bridgeDemo.htmlFile = "ui/bridge.html";
    bridgeDemo.styleFile = "ui/bridge.css";
    bridgeDemo.scriptFile = "ui/bridge.js";
    bridge.controls.push_back(bridgeDemo);
    bridgePage.sections.push_back(bridge);
    spec.pages.push_back(bridgePage);

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
    spec.pages.push_back(PanelSample::Components::BuildPage());

    XBase::Panel::Page management;
    management.id = "management";
    management.label = "面板管理";
    XBase::Panel::Section statusSection;
    statusSection.id = "status";
    statusSection.label = "状态";
    XBase::Panel::Control status;
    status.kind = XBase::Panel::ControlKind::Textarea;
    status.id = "panelsample.status";
    status.label = "运行状态";
    status.readOnly = true;
    statusSection.controls.push_back(status);
    management.sections.push_back(statusSection);

    XBase::Panel::Section commands;
    commands.id = "commands";
    commands.label = "操作";
    commands.columns = 3;
    commands.inlineLayout = true;
    for (const XBase::Panel::Option& option : {
             XBase::Panel::Option{"show", "显示面板"},
             XBase::Panel::Option{"hide", "隐藏面板"},
             XBase::Panel::Option{"toggle", "切换显示"},
             XBase::Panel::Option{"remount", "重新挂载"},
             XBase::Panel::Option{"unmount", "卸载示例"},
             XBase::Panel::Option{"status", "刷新状态"}}) {
        XBase::Panel::Control command;
        command.kind = XBase::Panel::ControlKind::Action;
        command.id = "panelsample." + option.value;
        command.label = option.label;
        commands.controls.push_back(command);
    }
    management.sections.push_back(commands);
    spec.pages.push_back(management);
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

    ok = XBase::Panel::BindValue(
             "panelsample.count",
             [] { return g_count; },
             [](double value) { g_count = value; })
        && ok;

    ok = XBase::Panel::BindValue(
             "panelsample.color",
             [] { return g_color; },
             [](double value) { g_color = value; })
        && ok;

    ok = XBase::Panel::BindValue(
             "panelsample.progress",
             [] { return g_progress; },
             [](double value) { g_progress = value; })
        && ok;

    ok = XBase::Panel::BindValue(
             "panelsample.radio",
             [] { return g_radio; },
             [](double value) { g_radio = value; })
        && ok;

    ok = XBase::Panel::BindValue(
             "panelsample.flags",
             [] { return g_flags; },
             [](double value) { g_flags = value; })
        && ok;

    ok = XBase::Panel::BindText(
             "panelsample.title",
             [] { return g_title; },
             [](const std::string& value) { g_title = value; })
        && ok;

    ok = XBase::Panel::BindText(
             "panelsample.notes",
             [] { return g_notes; },
             [](const std::string& value) { g_notes = value; })
        && ok;

    ok = XBase::Panel::BindAction("panelsample.printLog", [] {
             XBase::Log::Info(std::string("面板动作：倍率 ") + std::to_string(g_scale)
                              + "，模式下标 " + std::to_string(static_cast<int>(g_mode)));
         })
        && ok;

    ok = XBase::Panel::BindAction("panelsample.reset", [] {
             g_scale = 1.0;
             g_mode = 1.0;
             g_count = 10.0;
             g_color = 0x3366FF;
             g_progress = 62.5;
             g_radio = 1.0;
             g_flags = 5.0;
             g_title = "PanelSample";
             g_notes = "这里演示 Textarea 与 panel.textChanged。";
             XBase::Config::SetString(kKeyScale, std::to_string(g_scale));
             XBase::Config::SetString(kKeyMode, std::to_string(g_mode));
             XBase::Config::Save();
             // 模组自己改的状态要推回界面，否则网页还停在旧值上
             XBase::Panel::NotifyChanged("panelsample.scale", g_scale);
             XBase::Panel::NotifyChanged("panelsample.mode", g_mode);
             XBase::Panel::NotifyChanged("panelsample.count", g_count);
             XBase::Panel::NotifyChanged("panelsample.color", g_color);
             XBase::Panel::NotifyChanged("panelsample.progress", g_progress);
             XBase::Panel::NotifyChanged("panelsample.radio", g_radio);
             XBase::Panel::NotifyChanged("panelsample.flags", g_flags);
             XBase::Panel::NotifyTextChanged("panelsample.title", g_title);
             XBase::Panel::NotifyTextChanged("panelsample.notes", g_notes);
             XBase::Host::QueueMessage("已恢复默认参数");
         })
        && ok;

    ok = XBase::Panel::BindText(
        "panelsample.status",
        [] { return g_status; },
        [](const std::string&) {}) && ok;
    for (const auto& [id, command] : {
             std::pair<const char*, PanelCommand>{"show", PanelCommand::Show},
             {"hide", PanelCommand::Hide},
             {"toggle", PanelCommand::Toggle},
             {"remount", PanelCommand::Remount},
             {"unmount", PanelCommand::Unmount},
             {"status", PanelCommand::Status}}) {
        ok = XBase::Panel::BindAction(
            std::string("panelsample.") + id,
            [command] { g_pendingCommand.store(command); }) && ok;
    }
    return PanelSample::Components::Bind() && ok;
}

bool MountPanel() {
    if (!XBase::Panel::IsAvailable()) {
        XBase::Log::Warn("面板不可用，检查 XBase\\Library\\panel 与网页视图运行时");
        return false;
    }
    if (!XBase::Panel::Mount(BuildSpec())) return false;
    g_mounted = true;
    if (!BindHooks()) {
        XBase::Panel::Unmount(kModName);
        g_mounted = false;
        return false;
    }
    XBase::Panel::SetHotkey({XBase::Input::Key::F7, 0});
    RefreshStatus();
    return true;
}

void ExecuteCommand() {
    const PanelCommand command = g_pendingCommand.exchange(PanelCommand::None);
    if (command == PanelCommand::None) return;
    switch (command) {
    case PanelCommand::Show:
        if (!XBase::Panel::Show(kModName)) XBase::Log::Warn("显示面板请求失败");
        break;
    case PanelCommand::Hide:
        XBase::Panel::Hide();
        break;
    case PanelCommand::Toggle:
        XBase::Panel::Toggle();
        break;
    case PanelCommand::Remount:
        if (!MountPanel()) XBase::Log::Error("重新挂载失败");
        break;
    case PanelCommand::Unmount:
        XBase::Panel::Unmount(kModName);
        g_mounted = false;
        break;
    case PanelCommand::Status:
    case PanelCommand::None:
        break;
    }
    if (g_mounted) RefreshStatus();
}

void OnGameInit() {
    g_godMode = XBase::Config::GetBool(kKeyGodMode, false);
    g_scale = XBase::Config::GetString(kKeyScale, "1.0").empty()
        ? 1.0
        : std::stod(XBase::Config::GetString(kKeyScale, "1.0"));
    g_mode = std::stod(XBase::Config::GetString(kKeyMode, "1"));

    // 先挂载结构再绑钩子，绑定到的控件必须已经在注册表里
    if (!MountPanel()) {
        XBase::Log::Error("面板挂载失败");
        return;
    }

    XBase::Log::Info("面板已挂载，按 P 开关");
}

void OnProcess() {
    // 注册表操作不能在绑定回调持有锁时执行
    if (XBase::Input::WasPressed(XBase::Input::Key::F6)) {
        g_pendingCommand.store(PanelCommand::Remount);
    }
    ExecuteCommand();
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
    XBase::Host::Shutdown();
    XBase::Panel::Unmount(kModName);
    g_mounted = false;
    g_pendingCommand.store(PanelCommand::None);
    XBase::Log::Shutdown();
}
