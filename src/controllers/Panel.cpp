#include <XBase/Panel.h>

#include "PanelAbi.h"

#include <XBase/Abi.h>
#include <XBase/Capabilities.h>
#include <XBase/Hooks.h>
#include <XBase/Json.h>
#include <XBase/Log.h>
#include <XBase/Platform.h>
#include <XBase/Runtime.h>
#include <XBase/UI.h>
#include <XBase/Version.h>
#include <XBase/WebBridge.h>
#include <XBase/WebView.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>

#if !defined(XBASE_RUNTIME_DLL)
// 只有 mod 侧编得到 Bootstrap，共享运行时自己就是函数表的持有者
#include "../Bootstrap.h"
#endif

namespace XBase::Panel {
namespace {

constexpr const char* kVirtualHost = "xbase.panel";
constexpr const char* kPageFile = "index.html";
constexpr const char* kPanelFolder = "Library\\panel\\";
constexpr Input::Hotkey kPanelHotkey{Input::Key::P, 0};

struct Binding {
    ValueRead read;
    ValueWrite write;
    TextRead textRead;
    TextWrite textWrite;
    ActionFn run;
};

struct ModEntry {
    ModSpec spec;
    std::unordered_map<std::string, Binding> bindings;
};

std::mutex g_mutex;
std::vector<ModEntry> g_mods;
bool g_initialized = false;
bool g_navigated = false;
WebView::WebViewId g_webViewId = WebView::DefaultInstance;
bool g_systemHotkeyDown = false;
std::string g_activeModId;
Input::Hotkey g_hotkey = kPanelHotkey;
Rect g_bounds{};
bool g_boundsReady = false;
Hooks::DrawCallbackId g_drawCallbackId{};

// 共享运行时比 mod 的头文件旧时没有这一段，此时退回本地注册表，
// 结果是每个模块各持一份面板，功能还在但不再聚合
bool HasPanelSection(const XBaseRuntime* table) {
    return table != nullptr
        && table->size >= XBASE_ABI_PANEL_OFFSET + sizeof(void*)
        && table->panelMount != nullptr;
}

const XBaseRuntime* RuntimeTable() {
#if defined(XBASE_RUNTIME_DLL)
    return nullptr;
#else
    const XBaseRuntime* table = Bootstrap::GetRuntimeTable();
    return HasPanelSection(table) ? table : nullptr;
#endif
}

std::string PanelDirectory() {
    return Platform::XBaseDirectory() + kPanelFolder;
}

Rect EnsureBounds();
void DrawCapturePanel();
void HidePanelInput();

std::string ModuleOwnerId() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleOwnerId), &module)) {
        return {};
    }

    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0) return {};
        if (size < path.size() - 1) {
            path.resize(size);
            break;
        }
        path.resize(path.size() * 2);
    }

    const std::size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    for (const wchar_t* suffix : {L"III", L"SA", L"VC"}) {
        const std::wstring token(suffix);
        if (name.size() > token.size()
            && name.compare(name.size() - token.size(), token.size(), token) == 0) {
            name.resize(name.size() - token.size());
            break;
        }
    }
    if (name.empty()) return {};
    const int utf8Size = WideCharToMultiByte(
        CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0) return {};
    std::string utf8(static_cast<std::size_t>(utf8Size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, name.data(), static_cast<int>(name.size()), utf8.data(), utf8Size, nullptr, nullptr);
    return utf8;
}

bool CapabilityOk(const std::optional<FeatureCapability>& capability) {
    if (!capability) return true;
    return HasCapability(*capability);
}

bool MatchesGame(const std::vector<std::string>& games) {
    if (games.empty()) return true;
    const std::string current = Runtime::GetGameKey();
    return std::find(games.begin(), games.end(), current) != games.end();
}

const char* KindName(ControlKind kind) {
    switch (kind) {
    case ControlKind::Toggle: return "toggle";
    case ControlKind::Float: return "float";
    case ControlKind::Int: return "int";
    case ControlKind::Action: return "action";
    case ControlKind::Select: return "select";
    case ControlKind::Text: return "text";
    case ControlKind::Textarea: return "textarea";
    case ControlKind::Color: return "color";
    case ControlKind::Progress: return "progress";
    case ControlKind::Custom: return "custom";
    case ControlKind::Radio: return "radio";
    case ControlKind::MultiSelect: return "multiselect";
    case ControlKind::Heading: return "heading";
    case ControlKind::Separator: return "separator";
    case ControlKind::Component: return "component";
    }
    return "toggle";
}

ControlKind KindFromName(const std::string& name) {
    if (name == "float") return ControlKind::Float;
    if (name == "int") return ControlKind::Int;
    if (name == "action") return ControlKind::Action;
    if (name == "select") return ControlKind::Select;
    if (name == "text") return ControlKind::Text;
    if (name == "textarea") return ControlKind::Textarea;
    if (name == "color") return ControlKind::Color;
    if (name == "progress") return ControlKind::Progress;
    if (name == "custom") return ControlKind::Custom;
    if (name == "radio") return ControlKind::Radio;
    if (name == "multiselect") return ControlKind::MultiSelect;
    if (name == "heading") return ControlKind::Heading;
    if (name == "separator") return ControlKind::Separator;
    if (name == "component") return ControlKind::Component;
    return ControlKind::Toggle;
}

Json::Value CapabilityJson(const std::optional<FeatureCapability>& capability) {
    return Json::Value(static_cast<double>(capability ? static_cast<int>(*capability) : -1));
}

std::optional<FeatureCapability> CapabilityFromJson(const Json::Value& value) {
    if (!value.IsNumber()) return std::nullopt;
    const int code = value.AsInt(-1);
    if (code < 0) return std::nullopt;
    return static_cast<FeatureCapability>(code);
}

Json::Value StringsJson(const std::vector<std::string>& values) {
    Json::Value result;
    for (const std::string& value : values) {
        result.Push(Json::Value(value));
    }
    return result;
}

std::vector<std::string> StringsFromJson(const Json::Value& value) {
    std::vector<std::string> result;
    if (!value.IsArray()) return result;
    for (std::size_t i = 0; i < value.Size(); ++i) {
        result.push_back(value[i].AsString());
    }
    return result;
}

bool IsSafeAssetPath(const std::string& value) {
    if (value.empty() || value.find(':') != std::string::npos
        || value.front() == '\\' || value.front() == '/') {
        return false;
    }

    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find_first_of("\\/", start);
        const std::string part = value.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool LoadCustomAsset(
    const std::string& modId,
    const std::string& file,
    const char* kind,
    std::string& content) {
    if (file.empty()) return false;
    if (!IsSafeAssetPath(file)) {
        Log::Warn(std::string("Panel: 拒绝不安全的 Custom ") + kind + " 文件路径: " + file);
        return false;
    }

    std::string path = Platform::ModDirectory(modId.c_str()) + file;
    std::replace(path.begin(), path.end(), '/', '\\');
    std::string loaded;
    if (!Platform::ReadTextFile(path, loaded)) {
        Log::Warn(std::string("Panel: Custom ") + kind + " 文件读取失败: " + path);
        return false;
    }
    content = std::move(loaded);
    return true;
}

void ResolveCustomAssets(ModSpec& spec) {
    for (Page& page : spec.pages) {
        for (Section& section : page.sections) {
            for (Control& control : section.controls) {
                if (control.kind != ControlKind::Custom) continue;
                // 文件字段优先；未配置文件时保留原有内联内容。
                LoadCustomAsset(spec.modId, control.htmlFile, "HTML", control.html);
                LoadCustomAsset(spec.modId, control.scriptFile, "JavaScript", control.script);
                LoadCustomAsset(spec.modId, control.styleFile, "CSS", control.style);
            }
        }
    }
}

Json::Value SerializeNode(const ComponentNode& node) {
    Json::Value value;
    value.Set("component", Json::Value(node.component));
    value.Set("text", Json::Value(node.text));
    value.Set("textPath", Json::Value(node.textPath));
    value.Set("props", node.props);
    Json::Value children;
    for (const ComponentNode& child : node.children) {
        children.Push(SerializeNode(child));
    }
    value.Set("children", children);
    Json::Value slots;
    for (const auto& [name, nodes] : node.slots) {
        Json::Value items;
        for (const ComponentNode& child : nodes) items.Push(SerializeNode(child));
        slots.Set(name, items);
    }
    value.Set("slots", slots);
    Json::Value templates;
    for (const auto& [name, child] : node.templates) {
        templates.Set(name, SerializeNode(child));
    }
    value.Set("templates", templates);
    Json::Value bindings;
    for (const ComponentBinding& binding : node.bindings) {
        Json::Value item;
        item.Set("controlId", Json::Value(binding.controlId));
        item.Set("property", Json::Value(binding.property));
        item.Set("event", Json::Value(binding.event));
        item.Set("kind", Json::Value(static_cast<int>(binding.kind)));
        bindings.Push(item);
    }
    value.Set("bindings", bindings);
    return value;
}

ComponentNode ParseNode(const Json::Value& value) {
    ComponentNode node;
    node.component = value["component"].AsString();
    node.text = value["text"].AsString();
    node.textPath = value["textPath"].AsString();
    node.props = value["props"];
    const Json::Value& children = value["children"];
    for (std::size_t i = 0; i < children.Size(); ++i) {
        node.children.push_back(ParseNode(children[i]));
    }
    const Json::Value& slots = value["slots"];
    for (const std::string& name : slots.Keys()) {
        for (std::size_t i = 0; i < slots[name].Size(); ++i) {
            node.slots[name].push_back(ParseNode(slots[name][i]));
        }
    }
    const Json::Value& bindings = value["bindings"];
    const Json::Value& templates = value["templates"];
    for (const std::string& name : templates.Keys()) {
        node.templates.emplace(name, ParseNode(templates[name]));
    }
    for (std::size_t i = 0; i < bindings.Size(); ++i) {
        ComponentBinding binding;
        binding.controlId = bindings[i]["controlId"].AsString();
        binding.property = bindings[i]["property"].AsString();
        binding.event = bindings[i]["event"].AsString();
        binding.kind = static_cast<ComponentBindingKind>(bindings[i]["kind"].AsInt());
        node.bindings.push_back(std::move(binding));
    }
    return node;
}

Json::Value SerializeControl(const Control& control) {
    Json::Value value;
    value.Set("id", Json::Value(control.id));
    value.Set("kind", Json::Value(std::string(KindName(control.kind))));
    value.Set("label", Json::Value(control.label));
    value.Set("hint", Json::Value(control.hint));
    value.Set("enabled", Json::Value(CapabilityOk(control.capability)));
    value.Set("bounded", Json::Value(control.bounded));
    value.Set("min", Json::Value(control.min));
    value.Set("max", Json::Value(control.max));
    value.Set("step", Json::Value(control.step));
    value.Set("format", Json::Value(control.format));
    value.Set("text", Json::Value(control.text));
    value.Set("placeholder", Json::Value(control.placeholder));
    value.Set("html", Json::Value(control.html));
    value.Set("script", Json::Value(control.script));
    value.Set("style", Json::Value(control.style));
    value.Set("htmlFile", Json::Value(control.htmlFile));
    value.Set("scriptFile", Json::Value(control.scriptFile));
    value.Set("styleFile", Json::Value(control.styleFile));
    value.Set("readOnly", Json::Value(control.readOnly));
    value.Set("visibleWhen", Json::Value(control.visibleWhen));
    value.Set("component", SerializeNode(control.component));

    if (!control.options.empty()) {
        Json::Value options;
        for (const Option& option : control.options) {
            Json::Value item;
            item.Set("value", Json::Value(option.value));
            item.Set("label", Json::Value(option.label));
            options.Push(item);
        }
        value.Set("options", options);
    }
    return value;
}

Control ParseControl(const Json::Value& value) {
    Control control;
    control.kind = KindFromName(value["kind"].AsString("toggle"));
    control.id = value["id"].AsString();
    control.label = value["label"].AsString();
    control.hint = value["hint"].AsString();
    control.capability = CapabilityFromJson(value["capability"]);
    control.bounded = value["bounded"].AsBool(false);
    control.min = value["min"].AsNumber(0.0);
    control.max = value["max"].AsNumber(0.0);
    control.step = value["step"].AsNumber(0.0);
    control.format = value["format"].AsString();
    control.text = value["text"].AsString();
    control.placeholder = value["placeholder"].AsString();
    control.html = value["html"].AsString();
    control.script = value["script"].AsString();
    control.style = value["style"].AsString();
    control.htmlFile = value["htmlFile"].AsString();
    control.scriptFile = value["scriptFile"].AsString();
    control.styleFile = value["styleFile"].AsString();
    control.readOnly = value["readOnly"].AsBool(false);
    control.visibleWhen = value["visibleWhen"].AsString();
    control.games = StringsFromJson(value["games"]);
    control.component = ParseNode(value["component"]);

    const Json::Value& options = value["options"];
    if (options.IsArray()) {
        for (std::size_t i = 0; i < options.Size(); ++i) {
            Option option;
            option.value = options[i]["value"].AsString();
            option.label = options[i]["label"].AsString();
            control.options.push_back(std::move(option));
        }
    }
    return control;
}

Json::Value SerializeSection(const Section& section) {
    Json::Value value;
    value.Set("id", Json::Value(section.id));
    value.Set("label", Json::Value(section.label));
    value.Set("hint", Json::Value(section.hint));
    value.Set("columns", Json::Value(static_cast<double>(section.columns)));
    value.Set("inline", Json::Value(section.inlineLayout));
    value.Set("enabled", Json::Value(CapabilityOk(section.capability)));

    Json::Value controls;
    for (const Control& control : section.controls) {
        if (!MatchesGame(control.games)) continue;
        controls.Push(SerializeControl(control));
    }
    value.Set("controls", controls);
    return value;
}

Section ParseSection(const Json::Value& value) {
    Section section;
    section.id = value["id"].AsString();
    section.label = value["label"].AsString();
    section.hint = value["hint"].AsString();
    section.columns = value["columns"].AsInt(1);
    section.inlineLayout = value["inline"].AsBool(false);
    section.capability = CapabilityFromJson(value["capability"]);

    const Json::Value& controls = value["controls"];
    if (controls.IsArray()) {
        for (std::size_t i = 0; i < controls.Size(); ++i) {
            section.controls.push_back(ParseControl(controls[i]));
        }
    }
    return section;
}

Json::Value SerializeMod(const ModEntry& entry) {
    Json::Value value;
    value.Set("id", Json::Value(entry.spec.modId));
    value.Set("title", Json::Value(entry.spec.title));
    value.Set("subtitle", Json::Value(entry.spec.subtitle));
    value.Set("version", Json::Value(entry.spec.version));

    Json::Value pages;
    for (const Page& page : entry.spec.pages) {
        Json::Value pageValue;
        pageValue.Set("id", Json::Value(page.id));
        pageValue.Set("label", Json::Value(page.label));

        Json::Value sections;
        for (const Section& section : page.sections) {
            const Json::Value sectionValue = SerializeSection(section);
            // 整块被版本筛空时不下发，网页端不必再判断一次
            if (sectionValue["controls"].Size() > 0) {
                sections.Push(sectionValue);
            }
        }
        pageValue.Set("sections", sections);
        pages.Push(pageValue);
    }
    value.Set("pages", pages);
    return value;
}

Json::Value SchemaJson() {
    Json::Value result;
    result.Set("game", Json::Value(std::string(Runtime::GetGameKey())));
    result.Set("gameName", Json::Value(std::string(Runtime::GetGameName())));
    result.Set("version", Json::Value(std::string(GetVersionString())));
    result.Set("activeModId", Json::Value(g_activeModId));

    Json::Value mods;
    for (const ModEntry& entry : g_mods) {
        mods.Push(SerializeMod(entry));
    }
    result.Set("mods", mods);
    return result;
}

Binding* FindBinding(const std::string& controlId) {
    for (ModEntry& entry : g_mods) {
        const auto found = entry.bindings.find(controlId);
        if (found != entry.bindings.end()) return &found->second;
    }
    return nullptr;
}

Json::Value OkValue() {
    Json::Value value;
    value.Set("ok", Json::Value(true));
    return value;
}

Json::Value FailedValue(const std::string& error) {
    Json::Value value;
    value.Set("ok", Json::Value(false));
    value.Set("error", Json::Value(error));
    return value;
}

Json::Value OnSchema(const Json::Value&) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return SchemaJson();
}

Json::Value OnGet(const Json::Value& params) {
    const std::string id = params["id"].AsString();
    std::lock_guard<std::mutex> lock(g_mutex);
    const Binding* binding = FindBinding(id);
    if (!binding || !binding->read) {
        return FailedValue("未绑定的控件: " + id);
    }

    Json::Value result;
    result.Set("ok", Json::Value(true));
    result.Set("value", Json::Value(binding->read()));
    return result;
}

Json::Value OnSet(const Json::Value& params) {
    const std::string id = params["id"].AsString();
    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(id);
    if (!binding || !binding->write) {
        return FailedValue("未绑定的控件: " + id);
    }

    const Json::Value& value = params["value"];
    const double next = value.IsBool() ? (value.AsBool() ? 1.0 : 0.0) : value.AsNumber(0.0);
    binding->write(next);
    return OkValue();
}

Json::Value OnGetText(const Json::Value& params) {
    const std::string id = params["id"].AsString();
    std::lock_guard<std::mutex> lock(g_mutex);
    const Binding* binding = FindBinding(id);
    if (!binding || !binding->textRead) {
        return FailedValue("未绑定的文本控件: " + id);
    }

    Json::Value result;
    result.Set("ok", Json::Value(true));
    result.Set("value", Json::Value(binding->textRead()));
    return result;
}

Json::Value OnSetText(const Json::Value& params) {
    const std::string id = params["id"].AsString();
    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(id);
    if (!binding || !binding->textWrite) {
        return FailedValue("未绑定的文本控件: " + id);
    }

    binding->textWrite(params["value"].AsString());
    return OkValue();
}

Json::Value OnRun(const Json::Value& params) {
    const std::string id = params["id"].AsString();
    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(id);
    if (!binding || !binding->run) {
        return FailedValue("未绑定的控件: " + id);
    }
    binding->run();
    return OkValue();
}

Json::Value OnHide(const Json::Value&) {
    Hide();
    return OkValue();
}

Rect EnsureBounds() {
    if (g_boundsReady && g_bounds.right > g_bounds.left && g_bounds.bottom > g_bounds.top) {
        return g_bounds;
    }

    Vec2 display = UI::GetDisplaySize();
    if (display.x <= 0.0f || display.y <= 0.0f) {
        HWND gameWindow = GetForegroundWindow();
        RECT client{};
        if (gameWindow && GetClientRect(gameWindow, &client)) {
            display = {
                static_cast<float>(client.right - client.left),
                static_cast<float>(client.bottom - client.top),
            };
        }
    }
    if (display.x <= 0.0f || display.y <= 0.0f) return {};

    const float width = display.x * 0.62f;
    const float height = display.y * 0.80f;
    g_bounds = Rect{(display.x - width) * 0.5f, (display.y - height) * 0.5f,
                    (display.x - width) * 0.5f + width, (display.y - height) * 0.5f + height};
    g_boundsReady = true;
    return g_bounds;
}

void DrawCapturePanel() {
    if (g_webViewId == WebView::DefaultInstance
        || !WebView::IsVisible(g_webViewId)
        || !WebView::UsesCaptureMode(g_webViewId)) {
        return;
    }

    const Rect bounds = EnsureBounds();
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    if (width <= 0.0f || height <= 0.0f) return;
    WebView::SetBounds(g_webViewId, bounds);

    UI::SetNextWindowPosition({bounds.left, bounds.top}, true);
    UI::SetNextWindowSize({width, height}, true);
    bool open = true;
    UI::Window(
        "XBasePanelWebView",
        "XBase Panel",
        [&] {
            WebView::DrawPanel(g_webViewId, bounds);
            const bool mouseDown = UI::IsMouseDown(UI::MouseButton::Left);
            if (UI::IsLastItemHovered() || mouseDown) {
                const float wheelDelta = Hooks::ConsumeWheelDelta();
                WebView::ForwardPanelInput(
                    g_webViewId,
                    bounds,
                    UI::GetMousePosition(),
                    mouseDown,
                    wheelDelta);
            }
        },
        &open,
        UI::Flag(UI::WindowFlag::NoTitleBar)
            | UI::Flag(UI::WindowFlag::NoResize)
            | UI::Flag(UI::WindowFlag::NoMove)
            | UI::Flag(UI::WindowFlag::NoScrollbar)
            | UI::Flag(UI::WindowFlag::NoBackground));
    if (!open) {
        HidePanelInput();
    }
}

void HidePanelInput() {
    Hooks::SetBackgroundInputActive(false);
    Hooks::SetBackgroundRenderActive(false);
    Hooks::SetMenuVisible(false);
    WebView::SetVisible(g_webViewId, false);
}

Json::Value OnSetSize(const Json::Value& params) {
    const Rect current = EnsureBounds();
    const float width = static_cast<float>(params["width"].AsNumber(current.right - current.left));
    const float height = static_cast<float>(params["height"].AsNumber(current.bottom - current.top));
    g_bounds = Rect{current.left, current.top, current.left + width, current.top + height};
    WebView::SetBounds(g_webViewId, g_bounds);
    return OkValue();
}

Json::Value OnSetPos(const Json::Value& params) {
    const Rect current = EnsureBounds();
    const float x = static_cast<float>(params["x"].AsNumber(current.left));
    const float y = static_cast<float>(params["y"].AsNumber(current.top));
    const float width = current.right - current.left;
    const float height = current.bottom - current.top;
    g_bounds = Rect{x, y, x + width, y + height};
    WebView::SetBounds(g_webViewId, g_bounds);
    return OkValue();
}

Json::Value OnRect(const Json::Value&) {
    const Rect current = EnsureBounds();
    Json::Value result;
    result.Set("x", Json::Value(static_cast<double>(current.left)));
    result.Set("y", Json::Value(static_cast<double>(current.top)));
    result.Set("width", Json::Value(static_cast<double>(current.right - current.left)));
    result.Set("height", Json::Value(static_cast<double>(current.bottom - current.top)));
    return result;
}

void InstallBridge() {
    WebBridge::Install(g_webViewId);
    WebBridge::RegisterMethod("panel.schema", OnSchema);
    WebBridge::RegisterMethod("panel.get", OnGet);
    WebBridge::RegisterMethod("panel.set", OnSet);
    WebBridge::RegisterMethod("panel.getText", OnGetText);
    WebBridge::RegisterMethod("panel.setText", OnSetText);
    WebBridge::RegisterMethod("panel.run", OnRun);
    WebBridge::RegisterMethod("panel.hide", OnHide);
    WebBridge::RegisterMethod("panel.setSize", OnSetSize);
    WebBridge::RegisterMethod("panel.setPos", OnSetPos);
    WebBridge::RegisterMethod("panel.rect", OnRect);
}

void UninstallBridge() {
    WebBridge::UnregisterMethod("panel.schema");
    WebBridge::UnregisterMethod("panel.get");
    WebBridge::UnregisterMethod("panel.set");
    WebBridge::UnregisterMethod("panel.getText");
    WebBridge::UnregisterMethod("panel.setText");
    WebBridge::UnregisterMethod("panel.run");
    WebBridge::UnregisterMethod("panel.hide");
    WebBridge::UnregisterMethod("panel.setSize");
    WebBridge::UnregisterMethod("panel.setPos");
    WebBridge::UnregisterMethod("panel.rect");
}

void RemoveMod(const std::string& modId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mods.erase(
        std::remove_if(g_mods.begin(), g_mods.end(),
                       [&modId](const ModEntry& entry) { return entry.spec.modId == modId; }),
        g_mods.end());
}

// --- mod 侧转发用的跳板 ---
// std::function 不能直接过 C 接口，这里把持有者留在 mod 自己的堆上，
// 函数表里只放一个函数指针加一个不透明的 userData

double TrampolineRead(void* userData) {
    const auto* binding = static_cast<const Binding*>(userData);
    return binding && binding->read ? binding->read() : 0.0;
}

void TrampolineWrite(double value, void* userData) {
    const auto* binding = static_cast<const Binding*>(userData);
    if (binding && binding->write) binding->write(value);
}

int TrampolineTextRead(char* buffer, std::uint32_t capacity, void* userData) {
    const auto* binding = static_cast<const Binding*>(userData);
    const std::string value = binding && binding->textRead ? binding->textRead() : std::string();
    const std::size_t needed = value.size() + 1;
    if (buffer && capacity >= needed) {
        std::memcpy(buffer, value.c_str(), needed);
    }
    return static_cast<int>(needed);
}

void TrampolineTextWrite(const char* value, void* userData) {
    const auto* binding = static_cast<const Binding*>(userData);
    if (binding && binding->textWrite) binding->textWrite(value ? value : "");
}

void TrampolineRun(void* userData) {
    const auto* binding = static_cast<const Binding*>(userData);
    if (binding && binding->run) binding->run();
}

std::mutex g_ownedMutex;
std::vector<std::pair<std::string, std::unique_ptr<Binding>>> g_owned;

Binding* OwnBinding(const std::string& controlId) {
    auto owned = std::make_unique<Binding>();
    Binding* raw = owned.get();
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    g_owned.emplace_back(controlId, std::move(owned));
    return raw;
}

void ReleaseOwned(const std::string& modId) {
    const std::string prefix = modId + ".";
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    g_owned.erase(
        std::remove_if(g_owned.begin(), g_owned.end(),
                       [&prefix](const std::pair<std::string, std::unique_ptr<Binding>>& item) {
                           return item.first.compare(0, prefix.size(), prefix) == 0;
                       }),
        g_owned.end());
}

Json::Value SpecJson(const ModSpec& spec) {
    Json::Value value;
    value.Set("modId", Json::Value(spec.modId));
    value.Set("ownerId", Json::Value(spec.ownerId));
    value.Set("title", Json::Value(spec.title));
    value.Set("subtitle", Json::Value(spec.subtitle));
    value.Set("version", Json::Value(spec.version));

    Json::Value pages;
    for (const Page& page : spec.pages) {
        Json::Value pageValue;
        pageValue.Set("id", Json::Value(page.id));
        pageValue.Set("label", Json::Value(page.label));

        Json::Value sections;
        for (const Section& section : page.sections) {
            Json::Value sectionValue;
            sectionValue.Set("id", Json::Value(section.id));
            sectionValue.Set("label", Json::Value(section.label));
            sectionValue.Set("hint", Json::Value(section.hint));
            sectionValue.Set("columns", Json::Value(static_cast<double>(section.columns)));
            sectionValue.Set("inline", Json::Value(section.inlineLayout));
            sectionValue.Set("capability", CapabilityJson(section.capability));

            Json::Value controls;
            for (const Control& control : section.controls) {
                Json::Value controlValue = SerializeControl(control);
                // 网页端要的是写好的 enabled，能力编码只是过桥用的
                controlValue.Set("capability", CapabilityJson(control.capability));
                controlValue.Set("games", StringsJson(control.games));
                controls.Push(controlValue);
            }
            sectionValue.Set("controls", controls);
            sections.Push(sectionValue);
        }
        pageValue.Set("sections", sections);
        pages.Push(pageValue);
    }
    value.Set("pages", pages);
    return value;
}

ModSpec SpecFromJson(const Json::Value& value) {
    ModSpec spec;
    spec.modId = value["modId"].AsString();
    spec.ownerId = value["ownerId"].AsString();
    spec.title = value["title"].AsString();
    spec.subtitle = value["subtitle"].AsString();
    spec.version = value["version"].AsString();

    const Json::Value& pages = value["pages"];
    if (!pages.IsArray()) return spec;

    for (std::size_t i = 0; i < pages.Size(); ++i) {
        Page page;
        page.id = pages[i]["id"].AsString();
        page.label = pages[i]["label"].AsString();
        const Json::Value& sections = pages[i]["sections"];
        if (sections.IsArray()) {
            for (std::size_t j = 0; j < sections.Size(); ++j) {
                Section section = ParseSection(sections[j]);
                page.sections.push_back(std::move(section));
            }
        }
        spec.pages.push_back(std::move(page));
    }
    return spec;
}

} // namespace

// 控件必须先登记进 entry.bindings，BindValue/BindAction 与网页端的 get/set/run
// 才能通过 FindBinding 命中；BindValue 只往已存在的槽位里填读写回调
void PopulateNodeBindings(ModEntry& entry, const ComponentNode& node) {
    for (const ComponentBinding& binding : node.bindings) {
        if (!binding.controlId.empty()) entry.bindings.try_emplace(binding.controlId);
    }
    for (const ComponentNode& child : node.children) PopulateNodeBindings(entry, child);
    for (const auto& [name, nodes] : node.slots) {
        for (const ComponentNode& child : nodes) PopulateNodeBindings(entry, child);
    }
    for (const auto& [name, child] : node.templates) PopulateNodeBindings(entry, child);
}

void PopulateBindings(ModEntry& entry) {
    for (const Page& page : entry.spec.pages) {
        for (const Section& section : page.sections) {
            for (const Control& control : section.controls) {
                entry.bindings[control.id] = Binding{};
                PopulateNodeBindings(entry, control.component);
            }
        }
    }
}

bool Mount(const ModSpec& spec) {
    ModSpec normalized = spec;
    if (normalized.ownerId.empty()) normalized.ownerId = ModuleOwnerId();
    if (normalized.modId.empty()) {
        Log::Warn("Panel: 挂载缺 modId，已忽略");
        return false;
    }
    ResolveCustomAssets(normalized);

    const XBaseRuntime* table = RuntimeTable();
    if (table) {
        return table->panelMount(SpecJson(normalized).Serialize(false).c_str()) != 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (ModEntry& entry : g_mods) {
        if (!normalized.ownerId.empty() && !entry.spec.ownerId.empty()
            && entry.spec.ownerId == normalized.ownerId
            && entry.spec.modId != normalized.modId) {
            Log::Warn("Panel: 同一个 ASI 只能挂载一个 Sidebar，已拒绝重复挂载");
            return false;
        }
        if (entry.spec.modId == normalized.modId) {
            entry.spec = normalized;
            entry.bindings.clear();
            PopulateBindings(entry);
            return true;
        }
    }
    ModEntry entry{normalized, {}};
    PopulateBindings(entry);
    g_mods.push_back(std::move(entry));
    return true;
}

void Unmount(const std::string& modId) {
    if (modId.empty()) return;

    const XBaseRuntime* table = RuntimeTable();
    if (table) {
        table->panelUnmount(modId.c_str());
        ReleaseOwned(modId);
        return;
    }
    RemoveMod(modId);
}

bool BindValue(const std::string& controlId, ValueRead read, ValueWrite write) {
    if (controlId.empty()) return false;

    const XBaseRuntime* table = RuntimeTable();
    if (table) {
        Binding* binding = OwnBinding(controlId);
        binding->read = std::move(read);
        binding->write = std::move(write);
        return table->panelBindValue(controlId.c_str(), &TrampolineRead, &TrampolineWrite, binding) != 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(controlId);
    if (!binding) {
        Log::Warn(std::string("Panel: 控件未登记就绑定 ") + controlId);
        return false;
    }
    binding->read = std::move(read);
    binding->write = std::move(write);
    return true;
}

bool BindText(const std::string& controlId, TextRead read, TextWrite write) {
    if (controlId.empty()) return false;

    const XBaseRuntime* table = RuntimeTable();
    if (table && table->panelBindText) {
        Binding* binding = OwnBinding(controlId);
        binding->textRead = std::move(read);
        binding->textWrite = std::move(write);
        return table->panelBindText(
            controlId.c_str(), &TrampolineTextRead, &TrampolineTextWrite, binding) != 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(controlId);
    if (!binding) {
        Log::Warn(std::string("Panel: 文本控件未登记就绑定 ") + controlId);
        return false;
    }
    binding->textRead = std::move(read);
    binding->textWrite = std::move(write);
    return true;
}

bool BindAction(const std::string& controlId, ActionFn run) {
    if (controlId.empty()) return false;

    const XBaseRuntime* table = RuntimeTable();
    if (table) {
        Binding* binding = OwnBinding(controlId);
        binding->run = std::move(run);
        return table->panelBindAction(controlId.c_str(), &TrampolineRun, binding) != 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(controlId);
    if (!binding) {
        Log::Warn(std::string("Panel: 控件未登记就绑定 ") + controlId);
        return false;
    }
    binding->run = std::move(run);
    return true;
}

void NotifyChanged(const std::string& controlId, double value) {
    if (controlId.empty()) return;

    const XBaseRuntime* table = RuntimeTable();
    if (table) {
        table->panelNotifyChanged(controlId.c_str(), value);
        return;
    }

    Json::Value payload;
    payload.Set("id", Json::Value(controlId));
    payload.Set("value", Json::Value(value));
    WebBridge::Emit(g_webViewId, "panel.changed", payload);
}

void NotifyTextChanged(const std::string& controlId, const std::string& value) {
    if (controlId.empty()) return;

    const XBaseRuntime* table = RuntimeTable();
    if (table && table->panelNotifyTextChanged) {
        table->panelNotifyTextChanged(controlId.c_str(), value.c_str());
        return;
    }

    Json::Value payload;
    payload.Set("id", Json::Value(controlId));
    payload.Set("value", Json::Value(value));
    WebBridge::Emit(g_webViewId, "panel.textChanged", payload);
}

bool IsAvailable() {
    const XBaseRuntime* table = RuntimeTable();
    if (table) return table->panelAvailable() != 0;
    return WebView::IsRuntimeAvailable() && Platform::FileExists(PanelDirectory() + kPageFile);
}

bool Show(const std::string& modId) {
    const XBaseRuntime* table = RuntimeTable();
    if (table) {
        Log::Info("Panel: 转发到共享运行时显示面板");
        table->panelShow(modId.c_str());
        return true;
    }

    if (!WebView::IsRuntimeAvailable()) {
        Log::Error("Panel: 本机没有可用的网页视图运行时");
        return false;
    }

    if (g_webViewId == WebView::DefaultInstance) {
        g_webViewId = WebView::Create();
    }

    if (!Hooks::IsInitialized() && !Hooks::Init()) {
        // XMenu 可能已经在另一个静态 XBase 副本里持有 D3D 钩子；
        // 此时继续创建 WebView，窗口模式仍可直接显示，不能阻止 Panel 打开。
        Log::Warn("Panel: 当前模块未持有渲染钩子，继续创建 WebView");
    }

    const std::string directory = PanelDirectory();
    if (!Platform::FileExists(directory + kPageFile)) {
        Log::Error(std::string("Panel: 面板资源缺失，把 panel 目录放到 ") + directory);
        return false;
    }

    if (!WebView::Init(g_webViewId)) {
        Log::Error("Panel: 网页视图初始化失败");
        return false;
    }

    InstallBridge();
    WebView::SetBounds(g_webViewId, EnsureBounds());

    if (!g_navigated) {
        WebView::MapFolder(g_webViewId, kVirtualHost, directory);
        WebView::Navigate(g_webViewId, std::string("https://") + kVirtualHost + "/" + kPageFile);
        g_navigated = true;
    }

    if (!modId.empty()) g_activeModId = modId;
    WebView::SetVisible(g_webViewId, true);
    Hooks::SetMenuVisible(true);
    return true;
}

void Hide() {
    const XBaseRuntime* table = RuntimeTable();
    if (table) {
        table->panelHide();
        return;
    }
    HidePanelInput();
}

void Toggle() {
    if (IsVisible()) {
        Hide();
        return;
    }
    Show();
}

bool IsVisible() {
    const XBaseRuntime* table = RuntimeTable();
    if (table) return table->panelIsVisible() != 0;
    return g_webViewId != WebView::DefaultInstance && WebView::IsVisible(g_webViewId);
}

void SetHotkey(const Input::Hotkey& hotkey) {
    // Panel 的全局入口由 XBase 统一拥有，模组传入的旧 F7 等配置不能覆盖它。
    (void)hotkey;
    g_hotkey = kPanelHotkey;

    const XBaseRuntime* table = RuntimeTable();
    if (table) {
        table->panelSetHotkey(static_cast<int>(kPanelHotkey.key), static_cast<unsigned int>(kPanelHotkey.modifiers));
    }
}

Input::Hotkey GetHotkey() {
    return kPanelHotkey;
}

void Init() {
    // 作为 mod 侧兼容层时，面板由 XBaseRuntime.dll 持有；
    // 不创建一个永远不会使用的本地 WebView 实例。
    if (RuntimeTable()) {
        g_initialized = true;
        return;
    }
    if (g_webViewId == WebView::DefaultInstance) {
        g_webViewId = WebView::Create();
    }
    if (!g_drawCallbackId) {
        g_drawCallbackId = Hooks::RegisterDrawCallback(DrawCapturePanel);
    }
    g_initialized = true;
}

bool IsInitialized() {
    return g_initialized;
}

void NotifyGameInit() {
    // 网页视图在游戏初始化时会自己收起来，导航状态留着复用
}

void Process() {
    // 共享运行时拥有唯一的面板状态和全局 P 热键。
    // 每个 ASI 仍会链接一份兼容版 Core，但不能再次处理同一个热键，
    // 否则同一帧会对共享面板连续 Toggle，表现为按键完全没有效果。
    if (RuntimeTable()) return;
    if (g_hotkey.key == Input::Key::None) return;
    if (g_webViewId != WebView::DefaultInstance && WebView::IsVisible(g_webViewId)) {
        WebView::SetBounds(g_webViewId, EnsureBounds());
    }
    // XMenu 与共享 Runtime 各自有一份输入状态；网页获得焦点后，P 的
    // AcceleratorKeyPressed 可能只抵达其中一份。Panel 固定使用 P，
    // 直接读取系统键态，避免跨模块边沿丢失。
    const bool down = (GetAsyncKeyState('P') & 0x8000) != 0;
    const bool pressed = down && !g_systemHotkeyDown;
    g_systemHotkeyDown = down;
    if (!pressed) return;
    Log::Info("Panel: 检测到 P 热键，切换面板");
    Toggle();
}

void Shutdown() {
    const XBaseRuntime* table = RuntimeTable();
    if (table) return;

    Hide();
    if (g_drawCallbackId) {
        Hooks::UnregisterDrawCallback(g_drawCallbackId);
        g_drawCallbackId = {};
    }
    UninstallBridge();
    if (g_webViewId != WebView::DefaultInstance) {
        WebBridge::Shutdown(g_webViewId);
        WebView::Destroy(g_webViewId);
        g_webViewId = WebView::DefaultInstance;
    }
    g_navigated = false;
    g_systemHotkeyDown = false;
    g_initialized = false;
}

namespace Abi {

int MountJson(const char* specJson) {
    if (!specJson) return 0;
    return Mount(SpecFromJson(Json::Value::Parse(specJson))) ? 1 : 0;
}

void UnmountName(const char* modId) {
    if (modId) Unmount(std::string(modId));
}

int BindValueRaw(const char* controlId, double (*read)(void*), void (*write)(double, void*), void* userData) {
    if (!controlId || !read || !write) return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(controlId);
    if (!binding) return 0;
    binding->read = [read, userData] { return read(userData); };
    binding->write = [write, userData](double value) { write(value, userData); };
    return 1;
}

int BindActionRaw(const char* controlId, void (*run)(void*), void* userData) {
    if (!controlId || !run) return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(controlId);
    if (!binding) return 0;
    binding->run = [run, userData] { run(userData); };
    return 1;
}

int BindTextRaw(
    const char* controlId,
    int (*read)(char*, std::uint32_t, void*),
    void (*write)(const char*, void*),
    void* userData) {
    if (!controlId || !read || !write) return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    Binding* binding = FindBinding(controlId);
    if (!binding) return 0;
    binding->textRead = [read, userData] {
        const int needed = read(nullptr, 0, userData);
        if (needed <= 1) return std::string();
        std::string value(static_cast<std::size_t>(needed - 1), '\0');
        read(value.data(), static_cast<std::uint32_t>(value.size() + 1), userData);
        return value;
    };
    binding->textWrite = [write, userData](const std::string& value) {
        write(value.c_str(), userData);
    };
    return 1;
}

void NotifyChangedName(const char* controlId, double value) {
    if (controlId) NotifyChanged(std::string(controlId), value);
}

void NotifyTextChangedName(const char* controlId, const char* value) {
    if (controlId) NotifyTextChanged(std::string(controlId), value ? std::string(value) : std::string());
}

int Available() {
    return IsAvailable() ? 1 : 0;
}

void ShowName(const char* modId) {
    Show(modId ? std::string(modId) : std::string());
}

void HideName() {
    Hide();
}

int IsVisibleRaw() {
    return IsVisible() ? 1 : 0;
}

void SetHotkeyRaw(int key, unsigned int modifiers) {
    // ABI 兼容旧版模组，但不允许旧配置把共享 Panel 改回 F7。
    (void)key;
    (void)modifiers;
    g_hotkey = kPanelHotkey;
}

} // namespace Abi

} // namespace XBase::Panel
