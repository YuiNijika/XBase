// 网页面板骨架，展示本地页面映射、原生方法注册与页面反向调用
// 页面必须先映射成虚拟 https 主机再加载，file 协议下模块脚本会被浏览器拦下

#include <XBase/Config.h>
#include <XBase/Host.h>
#include <XBase/Input.h>
#include <XBase/Json.h>
#include <XBase/Log.h>
#include <XBase/Platform.h>
#include <XBase/WebBridge.h>
#include <XBase/WebView.h>

#include <string>

namespace {

constexpr const char* kModName = "WebUi";
constexpr const char* kVirtualHost = "webui.local";
constexpr const char* kPageFile = "index.html";
constexpr const char* kKeyGodMode = "godMode";
constexpr XBase::Input::Key kPanelHotkey = XBase::Input::Key::F7;

// 页面用 window.xbase.call 调进来，参数与返回值都是 JSON
XBase::Json::Value OnSetGodMode(const XBase::Json::Value& params) {
    const bool enabled = params["enabled"].AsBool(false);

    XBase::Config::SetBool(kKeyGodMode, enabled);
    XBase::Config::Save();
    XBase::Log::Info(std::string("网页把无敌设为 ") + (enabled ? "开" : "关"));

    XBase::Json::Value result;
    result.Set("enabled", XBase::Json::Value(enabled));
    return result;
}

bool OpenPanel() {
    if (!XBase::WebView::IsRuntimeAvailable()) {
        XBase::Log::Error("本机没有可用的网页视图运行时");
        return false;
    }

    const std::string uiDir = XBase::Platform::ModDirectory(kModName) + "ui\\";
    if (!XBase::Platform::FileExists(uiDir + kPageFile)) {
        XBase::Log::Error(std::string("页面缺失，把 ui 目录拷到 ") + uiDir);
        return false;
    }

    if (!XBase::WebView::Init()) {
        XBase::Log::Error("网页视图初始化失败");
        return false;
    }

    XBase::WebBridge::Install();
    XBase::WebBridge::RegisterMethod("mod.setGodMode", OnSetGodMode);

    XBase::WebView::MapFolder(kVirtualHost, uiDir);
    XBase::WebView::Navigate(std::string("https://") + kVirtualHost + "/" + kPageFile);
    XBase::WebView::SetVisible(true);
    return true;
}

void OnGameInit() {
    if (OpenPanel()) {
        XBase::Host::ShowMessage("WebUi 示例已加载，按 F7 开关面板");
    }
}

void OnProcess() {
    if (!XBase::Input::WasPressed(kPanelHotkey)) return;

    XBase::WebView::SetVisible(!XBase::WebView::IsVisible());
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
    XBase::Log::Info(std::string("无敌当前状态 ") + (XBase::Config::GetBool(kKeyGodMode, false) ? "开" : "关"));

    if (!XBase::Host::Install({OnGameInit, OnProcess})) {
        XBase::Log::Error("注册生命周期失败");
        return;
    }

    XBase::Log::Info("生命周期注册完成");
}

extern "C" __declspec(dllexport) void XBasePayloadDetach() {
    XBase::Host::Shutdown();
    XBase::WebView::SetVisible(false);
    XBase::WebView::Shutdown();
    XBase::WebBridge::Shutdown();
    XBase::Log::Shutdown();
}
