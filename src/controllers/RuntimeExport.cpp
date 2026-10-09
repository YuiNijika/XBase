#include <XBase/Abi.h>

#include <XBase/Config.h>
#include <XBase/Core.h>
#include <XBase/Hooks.h>
#include <XBase/Host.h>
#include <XBase/Input.h>
#include <XBase/Log.h>
#include <XBase/Platform.h>
#include <XBase/UI.h>
#include <XBase/Version.h>

#include "PanelAbi.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct HostSink {
    void (*onGameInit)(void*) = nullptr;
    void (*onProcess)(void*) = nullptr;
    void* userData = nullptr;
};

std::mutex g_mutex;
std::vector<HostSink> g_sinks;
unsigned int g_refCount = 0;
bool g_started = false;

// 返回值统一是含结尾空字符的所需长度，容量不足时只报长度不写入
int CopyText(const std::string& value, char* buffer, std::uint32_t capacity) {
    const std::size_t needed = value.size() + 1;
    if (buffer && capacity >= needed) {
        std::memcpy(buffer, value.c_str(), needed);
    }
    return static_cast<int>(needed);
}

XBase::Color UnpackColor(std::uint32_t packed) {
    return XBase::Color{
        static_cast<std::uint8_t>((packed >> 16) & 0xFFu),
        static_cast<std::uint8_t>((packed >> 8) & 0xFFu),
        static_cast<std::uint8_t>(packed & 0xFFu),
        static_cast<std::uint8_t>((packed >> 24) & 0xFFu),
    };
}

std::vector<HostSink> SnapshotSinks() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sinks;
}

void DispatchGameInit() {
    XBase::Core::NotifyGameInit();
    for (const HostSink& sink : SnapshotSinks()) {
        if (sink.onGameInit) sink.onGameInit(sink.userData);
    }
}

void DispatchProcess() {
    // 领域分发只在共享库里跑一次，mod 不要再各调一遍
    XBase::Core::Process();
    for (const HostSink& sink : SnapshotSinks()) {
        if (sink.onProcess) sink.onProcess(sink.userData);
    }
}

int Acquire(const char*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_refCount;
    if (g_started) return 1;
    g_started = true;
    XBase::Core::Init(XBase::Core::AllDomains);
    XBase::Host::Install({DispatchGameInit, DispatchProcess});
    return 1;
}

int Release(const char*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_refCount > 0) --g_refCount;
    // 归零也不拆钩子，避免 mod 反复启停时重建 D3D 钩子
    return 1;
}

int GameDirectory(char* buffer, std::uint32_t capacity) {
    return CopyText(XBase::Platform::GameDirectory(), buffer, capacity);
}

int XBaseDirectory(char* buffer, std::uint32_t capacity) {
    return CopyText(XBase::Platform::XBaseDirectory(), buffer, capacity);
}

int ModDirectory(const char* modName, char* buffer, std::uint32_t capacity) {
    return CopyText(XBase::Platform::ModDirectory(modName ? modName : "Common"), buffer, capacity);
}

int AppDataDirectory(char* buffer, std::uint32_t capacity) {
    return CopyText(XBase::Platform::AppDataDirectory(), buffer, capacity);
}

void LogInitForMod(const char* modName) {
    XBase::Log::InitForMod(modName ? modName : "Common");
}

void LogWrite(int level, const char* message) {
    if (!message) return;
    switch (level) {
    case 1: XBase::Log::Write(XBase::Log::Level::Warn, message); break;
    case 2: XBase::Log::Write(XBase::Log::Level::Error, message); break;
    default: XBase::Log::Write(XBase::Log::Level::Info, message); break;
    }
}

void LogShutdown() {
    XBase::Log::Shutdown();
}

void ConfigInitForMod(const char* modName) {
    XBase::Config::InitForMod(modName ? modName : "Common");
}

int ConfigGetString(const char* key, char* buffer, std::uint32_t capacity) {
    if (!key) return 0;
    return CopyText(XBase::Config::GetString(key), buffer, capacity);
}

void ConfigSetString(const char* key, const char* value) {
    if (!key || !value) return;
    XBase::Config::SetString(key, value);
}

int ConfigGetInt(const char* key, int defaultValue) {
    return key ? XBase::Config::GetInt(key, defaultValue) : defaultValue;
}

void ConfigSetInt(const char* key, int value) {
    if (key) XBase::Config::SetInt(key, value);
}

int ConfigGetBool(const char* key, int defaultValue) {
    return (key && XBase::Config::GetBool(key, defaultValue != 0)) ? 1 : 0;
}

void ConfigSetBool(const char* key, int value) {
    if (key) XBase::Config::SetBool(key, value != 0);
}

void ConfigSave() {
    XBase::Config::Save();
}

int HostInstall(void (*onGameInit)(void*), void (*onProcess)(void*), void* userData) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started) return 0;
    g_sinks.push_back(HostSink{onGameInit, onProcess, userData});
    return 1;
}

void HostShutdown() {
    // 只摘掉全部订阅，宿主事件本身随共享库一起存活
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sinks.clear();
}

void HostShowMessage(const char* message) {
    if (message) XBase::Host::ShowMessage(message);
}

void HostQueueMessage(const char* message) {
    if (message) XBase::Host::QueueMessage(message);
}

int IsWorldReady() {
    return XBase::Core::IsWorldReady() ? 1 : 0;
}

int HooksInit() {
    return XBase::Hooks::Init() ? 1 : 0;
}

void HooksShutdown() {
    XBase::Hooks::Shutdown();
}

std::uint64_t RegisterDrawCallback(void (*draw)(void*), void* userData) {
    if (!draw) return 0;
    const XBase::Hooks::DrawCallbackId id =
        XBase::Hooks::RegisterDrawCallback([draw, userData] { draw(userData); });
    return id.value;
}

void UnregisterDrawCallback(std::uint64_t callbackId) {
    XBase::Hooks::UnregisterDrawCallback(XBase::Hooks::DrawCallbackId{callbackId});
}

void SetMenuVisible(int visible) {
    XBase::Hooks::SetMenuVisible(visible != 0);
}

int IsMenuVisible() {
    return XBase::Hooks::IsMenuVisible() ? 1 : 0;
}

void MaintainInputState() {
    XBase::Hooks::MaintainInputState();
}

float GetFrameDeltaSeconds() {
    return XBase::Hooks::GetFrameDeltaSeconds();
}

int IsKeyDown(int key) {
    return XBase::Input::IsDown(static_cast<XBase::Input::Key>(key)) ? 1 : 0;
}

int WasKeyPressed(int key) {
    return XBase::Input::WasPressed(static_cast<XBase::Input::Key>(key)) ? 1 : 0;
}

void PollSystemKeys() {
    XBase::Input::PollSystemKeys();
}

void SetNextWindowPosition(float x, float y) {
    XBase::UI::SetNextWindowPosition(XBase::Vec2{x, y});
}

void GetDisplaySize(float* width, float* height) {
    const XBase::Vec2 size = XBase::UI::GetDisplaySize();
    if (width) *width = size.x;
    if (height) *height = size.y;
}

void Text(const char* value) {
    if (value) XBase::UI::Text(value);
}

void TextFormatted(const char* format, float value) {
    if (format) XBase::UI::Text(format, value);
}

void TextDisabled(const char* value) {
    if (value) XBase::UI::TextDisabled(value);
}

void Separator() {
    XBase::UI::Separator();
}

void SameLine() {
    XBase::UI::SameLine();
}

void DrawLine(float x1, float y1, float x2, float y2, std::uint32_t color, float thickness) {
    XBase::UI::Canvas::Line(XBase::Vec2{x1, y1}, XBase::Vec2{x2, y2}, UnpackColor(color), thickness);
}

void DrawRect(float x1, float y1, float x2, float y2, std::uint32_t color, float thickness) {
    XBase::UI::Canvas::Rect(XBase::Vec2{x1, y1}, XBase::Vec2{x2, y2}, UnpackColor(color), thickness);
}

void DrawRectFilled(float x1, float y1, float x2, float y2, std::uint32_t color) {
    XBase::UI::Canvas::RectFilled(XBase::Vec2{x1, y1}, XBase::Vec2{x2, y2}, UnpackColor(color));
}

void DrawText(float x, float y, std::uint32_t color, const char* value) {
    if (!value) return;
    XBase::UI::Canvas::Text(XBase::Vec2{x, y}, UnpackColor(color), value);
}

int VersionString(char* buffer, std::uint32_t capacity) {
    return CopyText(XBase::GetVersionString(), buffer, capacity);
}

std::uint32_t VersionNumber() {
    return XBase::GetVersionNumber();
}

int PanelMount(const char* specJson) {
    return XBase::Panel::Abi::MountJson(specJson);
}

void PanelUnmount(const char* modId) {
    XBase::Panel::Abi::UnmountName(modId);
}

int PanelBindValue(const char* controlId, double (*read)(void*), void (*write)(double, void*), void* userData) {
    return XBase::Panel::Abi::BindValueRaw(controlId, read, write, userData);
}

int PanelBindText(
    const char* controlId,
    int (*read)(char*, std::uint32_t, void*),
    void (*write)(const char*, void*),
    void* userData) {
    return XBase::Panel::Abi::BindTextRaw(controlId, read, write, userData);
}

int PanelBindAction(const char* controlId, void (*run)(void*), void* userData) {
    return XBase::Panel::Abi::BindActionRaw(controlId, run, userData);
}

void PanelNotifyChanged(const char* controlId, double value) {
    XBase::Panel::Abi::NotifyChangedName(controlId, value);
}

void PanelNotifyTextChanged(const char* controlId, const char* value) {
    XBase::Panel::Abi::NotifyTextChangedName(controlId, value);
}

int PanelAvailable() {
    return XBase::Panel::Abi::Available();
}

void PanelShow(const char* modId) {
    XBase::Panel::Abi::ShowName(modId);
}

void PanelHide() {
    XBase::Panel::Abi::HideName();
}

int PanelIsVisible() {
    return XBase::Panel::Abi::IsVisibleRaw();
}

void PanelSetHotkey(int key, unsigned int modifiers) {
    XBase::Panel::Abi::SetHotkeyRaw(key, modifiers);
}

XBaseRuntime BuildTable() {
    XBaseRuntime table{};
    table.size = sizeof(XBaseRuntime);
    table.abiVersion = XBASE_ABI_VERSION;
    table.acquire = &Acquire;
    table.release = &Release;
    table.gameDirectory = &GameDirectory;
    table.xbaseDirectory = &XBaseDirectory;
    table.modDirectory = &ModDirectory;
    table.appDataDirectory = &AppDataDirectory;
    table.logInitForMod = &LogInitForMod;
    table.logWrite = &LogWrite;
    table.logShutdown = &LogShutdown;
    table.configInitForMod = &ConfigInitForMod;
    table.configGetString = &ConfigGetString;
    table.configSetString = &ConfigSetString;
    table.configGetInt = &ConfigGetInt;
    table.configSetInt = &ConfigSetInt;
    table.configGetBool = &ConfigGetBool;
    table.configSetBool = &ConfigSetBool;
    table.configSave = &ConfigSave;
    table.hostInstall = &HostInstall;
    table.hostShutdown = &HostShutdown;
    table.hostShowMessage = &HostShowMessage;
    table.hostQueueMessage = &HostQueueMessage;
    table.isWorldReady = &IsWorldReady;
    table.hooksInit = &HooksInit;
    table.hooksShutdown = &HooksShutdown;
    table.registerDrawCallback = &RegisterDrawCallback;
    table.unregisterDrawCallback = &UnregisterDrawCallback;
    table.setMenuVisible = &SetMenuVisible;
    table.isMenuVisible = &IsMenuVisible;
    table.maintainInputState = &MaintainInputState;
    table.getFrameDeltaSeconds = &GetFrameDeltaSeconds;
    table.isKeyDown = &IsKeyDown;
    table.wasKeyPressed = &WasKeyPressed;
    table.pollSystemKeys = &PollSystemKeys;
    table.setNextWindowPosition = &SetNextWindowPosition;
    table.getDisplaySize = &GetDisplaySize;
    table.text = &Text;
    table.textFormatted = &TextFormatted;
    table.textDisabled = &TextDisabled;
    table.separator = &Separator;
    table.sameLine = &SameLine;
    table.drawLine = &DrawLine;
    table.drawRect = &DrawRect;
    table.drawRectFilled = &DrawRectFilled;
    table.drawText = &DrawText;
    table.versionString = &VersionString;
    table.versionNumber = &VersionNumber;
    table.panelMount = &PanelMount;
    table.panelUnmount = &PanelUnmount;
    table.panelBindValue = &PanelBindValue;
    table.panelBindText = &PanelBindText;
    table.panelBindAction = &PanelBindAction;
    table.panelNotifyChanged = &PanelNotifyChanged;
    table.panelNotifyTextChanged = &PanelNotifyTextChanged;
    table.panelAvailable = &PanelAvailable;
    table.panelShow = &PanelShow;
    table.panelHide = &PanelHide;
    table.panelIsVisible = &PanelIsVisible;
    table.panelSetHotkey = &PanelSetHotkey;
    return table;
}

} // namespace

// 导出属性由 Abi.h 的声明带出，这里不要再写一遍，否则链接规范会冲突
extern "C" const XBaseRuntime* xbaseGetRuntime(std::uint32_t abiVersion) {
    // 字段只追加不改，旧头文件编译的 mod 拿到更长的表照样可用，
    // 新字段是否存在由调用方拿 size 判断；只拒绝比共享库更新的 ABI
    if (abiVersion == 0 || abiVersion > XBASE_ABI_VERSION) return nullptr;
    // 表只构建一次，之后所有 mod 拿到的都是同一份
    static const XBaseRuntime table = BuildTable();
    return &table;
}
