#include <XBase/WebView.h>

#include <XBase/Hooks.h>
#include <XBase/Log.h>
#include <XBase/Platform.h>

#include "HooksInternal.h"
#include "InputInternal.h"
#include "webview2/WebView2.h"

#include <Windows.h>
#include <objbase.h>
#include <objidl.h>

#include "imgui.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace {

using XBase::Rect;

using CreateEnvironmentWithOptionsFn = HRESULT(STDAPICALLTYPE*)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler);
using GetBrowserVersionStringFn = HRESULT(STDAPICALLTYPE*)(
    PCWSTR browserExecutableFolder,
    LPWSTR* versionInfo);

constexpr const wchar_t* kHostWindowClass = L"XBaseWebViewHost";
constexpr const wchar_t* kHostWindowMarker = L"XBase.WebView.Host";

// 抓帧模式参数，包括空闲与交互时的抓帧间隔、全屏状态检测间隔和交互判定阈值。
// 交互期 33 毫秒约等于 30 帧，再往上加帧率编码解码的开销就会吃满一个核
constexpr unsigned long long kCaptureIdleIntervalMs = 500;
constexpr unsigned long long kCaptureActiveIntervalMs = 33;
constexpr unsigned long long kCaptureActiveWindowMs = 1500;
constexpr unsigned long long kCaptureModeCheckMs = 250;
constexpr unsigned long long kMoveForwardIntervalMs = 60;
constexpr float kInteractionMoveThreshold = 8.0f;

struct Runtime {
    HMODULE loader = nullptr;
    CreateEnvironmentWithOptionsFn createEnvironment = nullptr;
    GetBrowserVersionStringFn getVersion = nullptr;
};

struct WebViewState {
    std::mutex mutex;
    HWND gameWindow = nullptr;
    HWND hostWindow = nullptr;
    ICoreWebView2Environment* environment = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* webview = nullptr;
    EventRegistrationToken navigationStartingToken{};
    EventRegistrationToken navigationCompletedToken{};
    EventRegistrationToken documentTitleToken{};
    EventRegistrationToken newWindowToken{};
    EventRegistrationToken acceleratorKeyToken{};
    bool tokensRegistered = false;
    bool acceleratorKeyRegistered = false;

    bool initRequested = false;
    bool createRequested = false;
    bool createInFlight = false;
    bool shutdownPending = false;
    bool initialized = false;
    bool visible = false;
    bool loading = false;
    bool canGoBack = false;
    bool canGoForward = false;
    int lastError = 0;
    int runtimeState = -1;  // -1 未检测，0 不可用，1 可用
    std::string url;
    std::string title;
    std::string pendingUrl;
    std::string pendingHtml;
    // 关闭控制器后仍要保留最后一次导航请求，下一次显示时才能重建同一页面。
    std::string sourceUrl;
    std::string sourceHtml;
    Rect bounds{};
    bool boundsApplied = false;
    float zoom = 1.0f;
    XBase::WebView::StateCallback stateCallback = nullptr;
    XBase::WebView::MessageHandler messageHandler = nullptr;
    // 文档级脚本登记一次就要在之后每个新建的控制器上重新注入，
    // 点叉关闭会销毁控制器再重建，这里不能跟着控制器一起清掉
    std::vector<std::string> documentScripts;

    // 本地页面用虚拟主机映射成 https 源，file 协议下子资源会被当作跨源拦下
    std::vector<std::pair<std::string, std::string>> virtualHosts;
    EventRegistrationToken webMessageToken{};
    bool webMessageRegistered = false;

    // 独占全屏下改用抓帧贴图呈现
    bool previewReady = false;
    bool captureMode = false;
    bool menuWasVisible = false;
    bool captureActive = false;
    IDirect3DTexture9* texture = nullptr;
    int textureWidth = 0;
    int textureHeight = 0;
    bool textureDynamic = false;
    IStream* captureStream = nullptr;
    bool captureInFlight = false;
unsigned long long lastCaptureAt = 0;
unsigned long long lastInteractionAt = 0;
unsigned long long lastModeCheckAt = 0;
unsigned long long lastForwardedMoveAt = 0;
float lastForwardedMoveX = 0.0f;
float lastForwardedMoveY = 0.0f;
bool forwardedMoveValid = false;
bool previousMouseDown = false;
int cursorShows = 0;
};

Runtime s_runtime;
WebViewState s_defaultState;
std::unordered_map<XBase::WebView::WebViewId, std::unique_ptr<WebViewState>> s_instances;
XBase::WebView::WebViewId s_nextInstanceId = 1;
thread_local WebViewState* t_activeState = nullptr;
thread_local XBase::WebView::WebViewId t_activeInstance = XBase::WebView::DefaultInstance;
std::unordered_map<HWND, WebViewState*> s_hostStates;

WebViewState& ActiveState() {
    return t_activeState ? *t_activeState : s_defaultState;
}

XBase::WebView::WebViewId ActiveInstance() {
    return t_activeState ? t_activeInstance : XBase::WebView::DefaultInstance;
}

class ScopedState {
public:
    explicit ScopedState(WebViewState* state, XBase::WebView::WebViewId id)
        : previousState_(t_activeState), previousId_(t_activeInstance) {
        t_activeState = state;
        t_activeInstance = id;
    }
    ~ScopedState() {
        t_activeState = previousState_;
        t_activeInstance = previousId_;
    }
private:
    WebViewState* previousState_;
    XBase::WebView::WebViewId previousId_;
};

WebViewState* FindState(XBase::WebView::WebViewId id) {
    if (id == XBase::WebView::DefaultInstance) return &s_defaultState;
    const auto found = s_instances.find(id);
    return found == s_instances.end() ? nullptr : found->second.get();
}

XBase::WebView::WebViewId FindInstanceId(WebViewState* state) {
    if (state == &s_defaultState) return XBase::WebView::DefaultInstance;
    for (const auto& entry : s_instances) {
        if (entry.second.get() == state) return entry.first;
    }
    return XBase::WebView::DefaultInstance;
}

WebViewState* StateForActive() {
    return &ActiveState();
}

LRESULT CALLBACK HostWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
void DestroyHostWindow();
void ApplyVirtualHostsLocked(ICoreWebView2* webview);

std::wstring HostWindowClassName() {
    static const std::wstring name = [] {
        wchar_t buffer[96]{};
        std::swprintf(
            buffer,
            sizeof(buffer) / sizeof(buffer[0]),
            L"%ls_%p",
            kHostWindowClass,
            reinterpret_cast<const void*>(&HostWindowProc));
        return std::wstring(buffer);
    }();
    return name;
}

UINT HostCloseMessage() {
    static const UINT message = RegisterWindowMessageW(L"XBase.WebView.Close");
    return message;
}

std::string ModuleFilePath(const char* fileName) {
    return XBase::Platform::CurrentModuleDirectory() + fileName;
}

// 加载器属于公用二进制，统一放在 XBase 目录下的 Library 子目录
// 旧安装可能放在 XBase 根目录或跟着 asi 一起分发，所以先搬过去再加载
std::string RuntimeLoaderPath() {
    return XBase::Platform::XBaseDirectory() + "Library\\WebView2Loader.dll";
}

void MigrateLegacyRuntimeLoader() {
    const std::string target = RuntimeLoaderPath();
    if (XBase::Platform::FileExists(target)) return;

    const std::string candidates[] = {
        XBase::Platform::XBaseDirectory() + "WebView2Loader.dll",  // 旧版直接放 XBase 根目录
        ModuleFilePath("WebView2Loader.dll"),                      // 更早的版本随 asi 附带
    };

    for (const std::string& legacy : candidates) {
        if (!XBase::Platform::FileExists(legacy)) continue;

        std::string content;
        if (!XBase::Platform::ReadBinaryFile(legacy, content)) continue;

        XBase::Platform::EnsureDirectory(XBase::Platform::XBaseDirectory() + "Library\\");
        if (XBase::Platform::WriteBinaryFile(target, content)) {
            XBase::Log::Info("WebView: WebView2Loader.dll 已迁移到 XBase\\Library 目录");
            return;
        }
    }
}

bool LoadRuntime() {
    if (s_runtime.loader) return true;

    MigrateLegacyRuntimeLoader();

    const std::wstring sharedPath = XBase::Platform::Utf8ToWide(RuntimeLoaderPath());
    HMODULE loader = LoadLibraryW(sharedPath.c_str());
    if (!loader) {
        // 兼容仍有自带加载器的旧安装
        loader = LoadLibraryW(XBase::Platform::Utf8ToWide(ModuleFilePath("WebView2Loader.dll")).c_str());
    }
    if (!loader) {
        loader = LoadLibraryW(L"WebView2Loader.dll");
    }
    if (!loader) {
        XBase::Log::Warn("WebView: WebView2Loader.dll 未找到，网页视图不可用");
        return false;
    }

    s_runtime.loader = loader;
    s_runtime.createEnvironment = reinterpret_cast<CreateEnvironmentWithOptionsFn>(
        GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
    s_runtime.getVersion = reinterpret_cast<GetBrowserVersionStringFn>(
        GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString"));
    if (!s_runtime.createEnvironment || !s_runtime.getVersion) {
        XBase::Log::Warn("WebView: WebView2Loader 缺少所需导出函数");
        return false;
    }
    return true;
}

std::wstring WideFrom(const std::string& value) {
    return XBase::Platform::Utf8ToWide(value);
}

void ApplyVirtualHostsLocked(ICoreWebView2* webview) {
    if (!webview || ActiveState().virtualHosts.empty()) return;
    ICoreWebView2_3* webview3 = nullptr;
    if (FAILED(webview->QueryInterface(IID_PPV_ARGS(&webview3))) || !webview3) return;
    for (const auto& entry : ActiveState().virtualHosts) {
        webview3->SetVirtualHostNameToFolderMapping(
            WideFrom(entry.first).c_str(),
            WideFrom(entry.second).c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }
    webview3->Release();
}


std::string Utf8FromCoTaskMem(LPWSTR value) {
    if (!value) return {};
    std::string result = XBase::Platform::WideToUtf8(value);
    CoTaskMemFree(value);
    return result;
}

std::string SourceOf(ICoreWebView2* webview) {
    if (!webview) return {};
    LPWSTR source = nullptr;
    if (FAILED(webview->get_Source(&source))) return {};
    return Utf8FromCoTaskMem(source);
}

std::string TitleOf(ICoreWebView2* webview) {
    if (!webview) return {};
    LPWSTR title = nullptr;
    if (FAILED(webview->get_DocumentTitle(&title))) return {};
    return Utf8FromCoTaskMem(title);
}

void RefreshHistoryFlags(ICoreWebView2* webview, bool& canGoBack, bool& canGoForward) {
    canGoBack = false;
    canGoForward = false;
    if (!webview) return;
    BOOL value = FALSE;
    canGoBack = SUCCEEDED(webview->get_CanGoBack(&value)) && value != FALSE;
    value = FALSE;
    canGoForward = SUCCEEDED(webview->get_CanGoForward(&value)) && value != FALSE;
}

void NotifyStateChanged() {
    XBase::WebView::StateCallback callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        callback = ActiveState().stateCallback;
    }
    if (callback) {
        callback();
    }
}

void ApplyBoundsLocked() {
    if (!ActiveState().hostWindow || !ActiveState().gameWindow) return;

    RECT client{};
    if (!GetClientRect(ActiveState().gameWindow, &client)) return;

    float displayWidth = 0.0f;
    float displayHeight = 0.0f;
    XBase::Detail::Hooks::GetDisplaySize(displayWidth, displayHeight);

    // 宿主使用 ImGui 显示坐标传入矩形，按客户区比例换算成子窗口坐标
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (displayWidth > 1.0f && displayHeight > 1.0f && client.right > 0 && client.bottom > 0) {
        scaleX = static_cast<float>(client.right) / displayWidth;
        scaleY = static_cast<float>(client.bottom) / displayHeight;
    }

    int left = static_cast<int>(ActiveState().bounds.left * scaleX);
    int top = static_cast<int>(ActiveState().bounds.top * scaleY);
    int width = static_cast<int>((ActiveState().bounds.right - ActiveState().bounds.left) * scaleX);
    int height = static_cast<int>((ActiveState().bounds.bottom - ActiveState().bounds.top) * scaleY);
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    if (left + width > client.right) width = client.right - left;
    if (top + height > client.bottom) height = client.bottom - top;
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;

    SetWindowPos(
        ActiveState().hostWindow, nullptr,
        left, top, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    if (ActiveState().controller) {
        RECT bounds{0, 0, width, height};
        ActiveState().controller->put_Bounds(bounds);
    }
    ActiveState().boundsApplied = true;

    static RECT lastLogged{};
    if (client.right != lastLogged.right || client.bottom != lastLogged.bottom
        || left != lastLogged.left || top != lastLogged.top
        || width != (lastLogged.right - lastLogged.left)
        || height != (lastLogged.bottom - lastLogged.top)) {
        lastLogged.left = left;
        lastLogged.top = top;
        lastLogged.right = left + width;
        lastLogged.bottom = top + height;
        char message[192]{};
        std::snprintf(message, sizeof(message),
            "WebView bounds client=%dx%d display=%.0fx%.0f rect=%d,%d %dx%d",
            static_cast<int>(client.right), static_cast<int>(client.bottom),
            displayWidth, displayHeight, left, top, width, height);
        XBase::Log::Info(message);
    }
}

// 独占全屏下 DWM 不合成子窗口，HWND 覆盖层永远不可见
bool IsExclusiveFullscreen() {
    IDirect3DDevice9* device = XBase::Detail::Hooks::GetD3D9Device();
    if (!device) return false;
    IDirect3DSwapChain9* chain = nullptr;
    if (FAILED(device->GetSwapChain(0, &chain)) || !chain) return false;
    D3DPRESENT_PARAMETERS parameters{};
    const bool exclusive = SUCCEEDED(chain->GetPresentParameters(&parameters)) && !parameters.Windowed;
    chain->Release();
    return exclusive;
}

void ApplyVisibleLocked() {
    if (!ActiveState().hostWindow) return;
    // 独占全屏下抓帧模式不接受焦点，否则游戏会失去键盘输入
    const bool captureMode = IsExclusiveFullscreen();
    if (ActiveState().controller) {
        ActiveState().controller->put_IsVisible(ActiveState().visible ? TRUE : FALSE);
    }
    ShowWindow(ActiveState().hostWindow, ActiveState().visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (ActiveState().visible) {
        if (!captureMode) {
            SetFocus(ActiveState().hostWindow);
            if (ActiveState().controller) {
                ActiveState().controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
        } else if (ActiveState().gameWindow) {
            SetFocus(ActiveState().gameWindow);
        }
    } else if (ActiveState().gameWindow) {
        SetFocus(ActiveState().gameWindow);
    }
}

void ApplyZoomLocked() {
    if (ActiveState().controller) {
        ActiveState().controller->put_ZoomFactor(static_cast<double>(ActiveState().zoom));
    }
}

// 调用方需持有状态锁，异步创建结束后才允许销毁宿主窗口
void ReleaseControllerLocked() {
    if (ActiveState().webview && ActiveState().tokensRegistered) {
        ActiveState().webview->remove_NavigationStarting(ActiveState().navigationStartingToken);
        ActiveState().webview->remove_NavigationCompleted(ActiveState().navigationCompletedToken);
        ActiveState().webview->remove_DocumentTitleChanged(ActiveState().documentTitleToken);
        ActiveState().webview->remove_NewWindowRequested(ActiveState().newWindowToken);
        if (ActiveState().webMessageRegistered) {
            ActiveState().webview->remove_WebMessageReceived(ActiveState().webMessageToken);
            ActiveState().webMessageRegistered = false;
        }
        ActiveState().tokensRegistered = false;
    }
    if (ActiveState().controller) {
        if (ActiveState().acceleratorKeyRegistered) {
            ActiveState().controller->remove_AcceleratorKeyPressed(ActiveState().acceleratorKeyToken);
            ActiveState().acceleratorKeyRegistered = false;
        }
        ActiveState().controller->Close();
        ActiveState().controller->Release();
        ActiveState().controller = nullptr;
    }
    if (ActiveState().webview) {
        ActiveState().webview->Release();
        ActiveState().webview = nullptr;
    }
    if (ActiveState().environment) {
        ActiveState().environment->Release();
        ActiveState().environment = nullptr;
    }
    if (ActiveState().captureStream) {
        ActiveState().captureStream->Release();
        ActiveState().captureStream = nullptr;
    }
    if (ActiveState().texture) {
        ActiveState().texture->Release();
        ActiveState().texture = nullptr;
    }
    ActiveState().captureInFlight = false;
    ActiveState().previewReady = false;
    ActiveState().textureWidth = 0;
    ActiveState().textureHeight = 0;
    ActiveState().previousMouseDown = false;
    ActiveState().lastForwardedMoveAt = 0;
    ActiveState().lastForwardedMoveX = 0.0f;
    ActiveState().lastForwardedMoveY = 0.0f;
    ActiveState().forwardedMoveValid = false;

    // 销毁异步宿主时，焦点/鼠标捕获不一定会随子窗口同步回到游戏窗口。
    // 清掉 WebView 注入的输入状态与 ShowCursor 计数，避免下一次回到游戏鼠标失效。
    if (GetCapture() == ActiveState().hostWindow) {
        ReleaseCapture();
    }
    while (ActiveState().cursorShows > 0) {
        ShowCursor(FALSE);
        --ActiveState().cursorShows;
    }
    DestroyHostWindow();
    if (ActiveState().gameWindow && IsWindow(ActiveState().gameWindow)
        && GetFocus() != ActiveState().gameWindow) {
        SetFocus(ActiveState().gameWindow);
    }
    XBase::Detail::Input::Reset();
    ActiveState().initialized = false;
    ActiveState().loading = false;
    ActiveState().canGoBack = false;
    ActiveState().canGoForward = false;
    ActiveState().boundsApplied = false;
}

// 同一个 XBase WebView 运行时只允许一个可见/创建中的视图。
// 切换页面时释放旧控制器，避免多个 HWND 与 WebView2 实例争抢输入和焦点。
void CloseOtherInstances(XBase::WebView::WebViewId keepId) {
    HWND keepWindow = nullptr;
    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        keepWindow = ActiveState().hostWindow;
    }

    // XMenu 与 XBaseRuntime.dll 可能各自带有一份静态 WebView 代码，
    // 通过进程内窗口消息把另一份实现持有的宿主也关闭掉。
    HWND gameWindow = ActiveState().gameWindow;
    if (!gameWindow) {
        gameWindow = XBase::Detail::Hooks::GetGameWindow();
    }
    if (gameWindow) {
        struct CloseContext {
            HWND keep;
        } context{keepWindow};
        EnumChildWindows(
            gameWindow,
            [](HWND window, LPARAM parameter) -> BOOL {
                auto* context = reinterpret_cast<CloseContext*>(parameter);
                if (window == context->keep || !GetPropW(window, kHostWindowMarker)) {
                    return TRUE;
                }
                SendMessageW(window, HostCloseMessage(), 0, 0);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));
    }

    std::vector<XBase::WebView::WebViewId> ids;
    if (keepId != XBase::WebView::DefaultInstance) {
        ids.push_back(XBase::WebView::DefaultInstance);
    }
    for (const auto& entry : s_instances) {
        if (entry.first != keepId) {
            ids.push_back(entry.first);
        }
    }

    for (const XBase::WebView::WebViewId id : ids) {
        WebViewState* state = FindState(id);
        if (!state) continue;

        ScopedState scope(state, id);
        bool shouldClose = false;
        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            shouldClose = ActiveState().visible
                || ActiveState().initialized
                || ActiveState().createRequested
                || ActiveState().createInFlight;
        }
        if (!shouldClose) continue;

        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            ActiveState().visible = false;
            ActiveState().createRequested = false;
            if (ActiveState().createInFlight) {
                ActiveState().shutdownPending = true;
            } else {
                ReleaseControllerLocked();
            }
        }
        XBase::Log::Info(
            "WebView: 新视图显示前已关闭旧 WebView 实例 " + std::to_string(id));
    }
}

void ExecuteScript(const std::string& script) {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (!ActiveState().webview) return;
    ActiveState().webview->ExecuteScript(WideFrom(script).c_str(), nullptr);
}

bool DecodeImageToRgba(const std::vector<unsigned char>& data, unsigned char*& pixels, int& width, int& height) {
    if (data.empty() || data.size() > static_cast<std::size_t>(INT_MAX)) return false;

    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(
        data.data(), static_cast<int>(data.size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0) {
        if (decoded) stbi_image_free(decoded);
        width = 0;
        height = 0;
        return false;
    }

    // 图像库输出 RGBA，通道交换放到上传时顺手做，省一次全图遍历
    pixels = decoded;
    return true;
}

bool UploadTextureLocked(const unsigned char* pixels, int width, int height) {
    IDirect3DDevice9* device = XBase::Detail::Hooks::GetD3D9Device();
    if (!device || !pixels || width <= 0 || height <= 0) return false;

    if (!ActiveState().texture || ActiveState().textureWidth != width || ActiveState().textureHeight != height) {
        if (ActiveState().texture) {
            ActiveState().texture->Release();
            ActiveState().texture = nullptr;
        }
        IDirect3DTexture9* texture = nullptr;
        // 动态纹理配合 DISCARD 整块上传，一次到位；设备不支持再退回托管池
        if (SUCCEEDED(device->CreateTexture(
                static_cast<UINT>(width), static_cast<UINT>(height), 1,
                D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr)) && texture) {
            ActiveState().textureDynamic = true;
        } else {
            texture = nullptr;
            if (FAILED(device->CreateTexture(
                    static_cast<UINT>(width), static_cast<UINT>(height), 1,
                    0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) || !texture) {
                return false;
            }
            ActiveState().textureDynamic = false;
        }
        ActiveState().texture = texture;
        ActiveState().textureWidth = width;
        ActiveState().textureHeight = height;
    }

    D3DLOCKED_RECT locked{};
    if (FAILED(ActiveState().texture->LockRect(0, &locked, nullptr, ActiveState().textureDynamic ? D3DLOCK_DISCARD : 0))) {
        ActiveState().texture->Release();
        ActiveState().texture = nullptr;
        ActiveState().textureWidth = 0;
        ActiveState().textureHeight = 0;
        ActiveState().previewReady = false;
        return false;
    }
    // 解出来是 RGBA，纹理要 BGRA，逐行边换边拷，读写各过一遍就够
    const std::size_t sourceStride = static_cast<std::size_t>(width) * 4;
    for (int row = 0; row < height; ++row) {
        const unsigned char* source = pixels + static_cast<std::size_t>(row) * sourceStride;
        auto* target = static_cast<unsigned char*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch;
        for (int column = 0; column < width; ++column) {
            const std::size_t offset = static_cast<std::size_t>(column) * 4;
            target[offset + 0] = source[offset + 2];
            target[offset + 1] = source[offset + 1];
            target[offset + 2] = source[offset + 0];
            target[offset + 3] = 255;
        }
    }
    ActiveState().texture->UnlockRect(0);
    ActiveState().previewReady = true;
    return true;
}

void StartCapture();

#define XBASE_WEBVIEW_HANDLER_BODY(InterfaceName)                                      \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {    \
        if (!object) return E_POINTER;                                                 \
        *object = nullptr;                                                             \
        if (riid == IID_IUnknown || riid == __uuidof(InterfaceName)) {                 \
            *object = static_cast<InterfaceName*>(this);                               \
            AddRef();                                                                  \
            return S_OK;                                                               \
        }                                                                              \
        return E_NOINTERFACE;                                                          \
    }                                                                                  \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }                  \
    ULONG STDMETHODCALLTYPE Release() override {                                       \
        const ULONG remaining = --refCount_;                                           \
        if (remaining == 0) delete this;                                               \
        return remaining;                                                              \
    }                                                                                  \
    std::atomic<ULONG> refCount_{1};

class CaptureCompletedHandler final : public ICoreWebView2CapturePreviewCompletedHandler {
public:
    explicit CaptureCompletedHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2CapturePreviewCompletedHandler)

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode) override {
        ScopedState scope(state_, FindInstanceId(state_));
        std::vector<unsigned char> data;
        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            IStream* stream = ActiveState().captureStream;
            ActiveState().captureStream = nullptr;
            ActiveState().captureInFlight = false;
            ActiveState().lastCaptureAt = static_cast<unsigned long long>(XBase::Platform::MonotonicMilliseconds());

            if (stream && SUCCEEDED(errorCode)) {
                STATSTG stat{};
                if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) {
                    const ULONGLONG size = stat.cbSize.QuadPart;
                    if (size > 0 && size < 64ull * 1024ull * 1024ull) {
                        data.resize(static_cast<std::size_t>(size));
                        LARGE_INTEGER origin{};
                        stream->Seek(origin, STREAM_SEEK_SET, nullptr);
                        ULONG read = 0;
                        if (FAILED(stream->Read(data.data(), static_cast<ULONG>(size), &read)) || read != size) {
                            data.clear();
                        }
                    }
                }
            }
            if (stream) stream->Release();
        }

        if (!data.empty()) {
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            if (DecodeImageToRgba(data, pixels, width, height)) {
                std::lock_guard<std::mutex> lock(ActiveState().mutex);
                UploadTextureLocked(pixels, width, height);
                stbi_image_free(pixels);
            }
        }
        return S_OK;
    }
private:
    WebViewState* state_;
};

void StartCapture() {
    ICoreWebView2* webview = nullptr;
    bool captureActive = false;
    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        if (ActiveState().captureInFlight || !ActiveState().webview || !ActiveState().visible) return;
        webview = ActiveState().webview;
        webview->AddRef();
        ActiveState().captureInFlight = true;
        captureActive = ActiveState().captureActive;
    }

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        ActiveState().captureInFlight = false;
        webview->Release();
        return;
    }

    auto* handler = new CaptureCompletedHandler(&ActiveState());
    // 静止时用 PNG 保证清晰度，交互期用 JPEG 压低编码与解码开销
    const COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT format =
        captureActive ? COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG
                      : COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG;
    const HRESULT hr = webview->CapturePreview(format, stream, handler);
    handler->Release();
    webview->Release();

    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (FAILED(hr)) {
        stream->Release();
        ActiveState().captureInFlight = false;
        return;
    }
    ActiveState().captureStream = stream;
}

// 网页子窗口持有焦点时按键不会到达游戏窗口，转发给 XBase 输入系统以便菜单热键继续工作
class AcceleratorKeyPressedHandler final : public ICoreWebView2AcceleratorKeyPressedEventHandler {
public:
    explicit AcceleratorKeyPressedHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2AcceleratorKeyPressedEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller* sender, ICoreWebView2AcceleratorKeyPressedEventArgs* args) override {
        ScopedState scope(state_, FindInstanceId(state_));
        if (!args) return S_OK;
        COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
        if (FAILED(args->get_KeyEventKind(&kind))) return S_OK;
        UINT virtualKey = 0;
        if (FAILED(args->get_VirtualKey(&virtualKey))) return S_OK;

        const bool down = kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN
            || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
        XBase::Detail::Input::HandleVirtualKey(static_cast<std::uint32_t>(virtualKey), down, false);
        return S_OK;
    }
private:
    WebViewState* state_;
};

class NavigationStartingHandler final : public ICoreWebView2NavigationStartingEventHandler {
public:
    explicit NavigationStartingHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2NavigationStartingEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) override {
        ScopedState scope(state_, FindInstanceId(state_));
        std::string url;
        if (args) {
            LPWSTR uri = nullptr;
            if (SUCCEEDED(args->get_Uri(&uri))) {
                url = Utf8FromCoTaskMem(uri);
            }
        }
        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            ActiveState().loading = true;
            ActiveState().lastError = 0;
            if (!url.empty()) ActiveState().url = url;
        }
        NotifyStateChanged();
        return S_OK;
    }
private:
    WebViewState* state_;
};

class NavigationCompletedHandler final : public ICoreWebView2NavigationCompletedEventHandler {
public:
    explicit NavigationCompletedHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2NavigationCompletedEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) override {
        ScopedState scope(state_, FindInstanceId(state_));
        bool canGoBack = false;
        bool canGoForward = false;
        RefreshHistoryFlags(sender, canGoBack, canGoForward);
        const std::string url = SourceOf(sender);

        int error = 0;
        if (args) {
            COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
            if (SUCCEEDED(args->get_WebErrorStatus(&status))) {
                error = static_cast<int>(status);
            }
        }

        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            ActiveState().loading = false;
            ActiveState().canGoBack = canGoBack;
            ActiveState().canGoForward = canGoForward;
            ActiveState().lastError = error;
            if (!url.empty()) ActiveState().url = url;
        }
        NotifyStateChanged();
        return S_OK;
    }
private:
    WebViewState* state_;
};

class DocumentTitleChangedHandler final : public ICoreWebView2DocumentTitleChangedEventHandler {
public:
    explicit DocumentTitleChangedHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2DocumentTitleChangedEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, IUnknown*) override {
        ScopedState scope(state_, FindInstanceId(state_));
        const std::string title = TitleOf(sender);
        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            ActiveState().title = title;
        }
        NotifyStateChanged();
        return S_OK;
    }
private:
    WebViewState* state_;
};

// 网页用 postMessage 发来的 JSON 原样转给宿主注册的处理函数
class WebMessageReceivedHandler final : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    explicit WebMessageReceivedHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2WebMessageReceivedEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        ScopedState scope(state_, FindInstanceId(state_));
        if (!args) return S_OK;

        std::string message;
        LPWSTR json = nullptr;
        if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
            message = Utf8FromCoTaskMem(json);
        }
        if (message.empty()) return S_OK;

        XBase::WebView::MessageHandler handler = nullptr;
        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            handler = ActiveState().messageHandler;
        }
        if (handler) {
            handler(message);
        }
        return S_OK;
    }
private:
    WebViewState* state_;
};

class NewWindowRequestedHandler final : public ICoreWebView2NewWindowRequestedEventHandler {
public:
    explicit NewWindowRequestedHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2NewWindowRequestedEventHandler)

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) override {
        ScopedState scope(state_, FindInstanceId(state_));
        if (!sender || !args) return S_OK;
        args->put_Handled(TRUE);
        LPWSTR uri = nullptr;
        if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
            sender->Navigate(uri);
            CoTaskMemFree(uri);
        }
        return S_OK;
    }
private:
    WebViewState* state_;
};

class ControllerCompletedHandler final : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    explicit ControllerCompletedHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller* controller) override {
        ScopedState scope(state_, FindInstanceId(state_));
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            ActiveState().createInFlight = false;

            if (ActiveState().shutdownPending || !ActiveState().hostWindow || !IsWindow(ActiveState().hostWindow)) {
                ActiveState().shutdownPending = false;
                if (controller) {
                    controller->Close();
                }
                ReleaseControllerLocked();
            } else if (FAILED(errorCode) || !controller) {
                XBase::Log::Error("WebView: WebView2 控制器创建失败");
            } else {
                ActiveState().controller = controller;
                ActiveState().controller->AddRef();
                ActiveState().controller->put_IsVisible(FALSE);
                ApplyBoundsLocked();
                ApplyZoomLocked();

                auto* acceleratorKey = new AcceleratorKeyPressedHandler(state_);
                if (SUCCEEDED(ActiveState().controller->add_AcceleratorKeyPressed(
                        acceleratorKey, &ActiveState().acceleratorKeyToken))) {
                    ActiveState().acceleratorKeyRegistered = true;
                }
                acceleratorKey->Release();

                ICoreWebView2* webview = nullptr;
                if (SUCCEEDED(ActiveState().controller->get_CoreWebView2(&webview)) && webview) {
                    ActiveState().webview = webview;
                    ICoreWebView2Settings* settings = nullptr;
                    if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
                        settings->put_IsStatusBarEnabled(FALSE);
                        settings->put_AreDevToolsEnabled(TRUE);
                        settings->Release();
                    }

                    auto* navigationStarting = new NavigationStartingHandler(state_);
                    auto* navigationCompleted = new NavigationCompletedHandler(state_);
                    auto* titleChanged = new DocumentTitleChangedHandler(state_);
                    auto* newWindow = new NewWindowRequestedHandler(state_);
                    if (SUCCEEDED(webview->add_NavigationStarting(navigationStarting, &ActiveState().navigationStartingToken))
                        && SUCCEEDED(webview->add_NavigationCompleted(navigationCompleted, &ActiveState().navigationCompletedToken))
                        && SUCCEEDED(webview->add_DocumentTitleChanged(titleChanged, &ActiveState().documentTitleToken))
                        && SUCCEEDED(webview->add_NewWindowRequested(newWindow, &ActiveState().newWindowToken))) {
                        ActiveState().tokensRegistered = true;
                    }
                    navigationStarting->Release();
                    navigationCompleted->Release();
                    titleChanged->Release();
                    newWindow->Release();

                    auto* webMessage = new WebMessageReceivedHandler(state_);
                    if (SUCCEEDED(webview->add_WebMessageReceived(webMessage, &ActiveState().webMessageToken))) {
                        ActiveState().webMessageRegistered = true;
                    }
                    webMessage->Release();

                    // 页面脚本注入要等控制器就绪，这里把登记过的全部补上，不清空，
                    // 之后再重建控制器时还要用同一份清单
                    for (const std::string& script : ActiveState().documentScripts) {
                        webview->AddScriptToExecuteOnDocumentCreated(WideFrom(script).c_str(), nullptr);
                    }

                    ApplyVirtualHostsLocked(webview);

                    ActiveState().canGoBack = false;
                    ActiveState().canGoForward = false;
                    ActiveState().url = SourceOf(webview);
                    ActiveState().title = TitleOf(webview);

                    const std::string html = !ActiveState().pendingHtml.empty()
                        ? ActiveState().pendingHtml
                        : ActiveState().sourceHtml;
                    const std::string url = !ActiveState().pendingUrl.empty()
                        ? ActiveState().pendingUrl
                        : ActiveState().sourceUrl;
                    if (!html.empty()) {
                        webview->NavigateToString(WideFrom(html).c_str());
                        ActiveState().pendingHtml.clear();
                    } else if (!url.empty()) {
                        webview->Navigate(WideFrom(url).c_str());
                        ActiveState().pendingUrl.clear();
                    }
                }
                ActiveState().initialized = true;
                if (ActiveState().visible) {
                    ApplyVisibleLocked();
                }
                ready = true;
            }
        }

        if (ready) {
            XBase::Log::Info("WebView: WebView2 控制器就绪");
        }
        NotifyStateChanged();
        return S_OK;
    }
private:
    WebViewState* state_;
};

class EnvironmentCompletedHandler final : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    explicit EnvironmentCompletedHandler(WebViewState* state) : state_(state) {}
    XBASE_WEBVIEW_HANDLER_BODY(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment* environment) override {
        ScopedState scope(state_, FindInstanceId(state_));
        if (FAILED(errorCode) || !environment) {
            {
                std::lock_guard<std::mutex> lock(ActiveState().mutex);
                ActiveState().createInFlight = false;
                ActiveState().createRequested = false;
            }
            XBase::Log::Error("WebView: WebView2 环境创建失败");
            NotifyStateChanged();
            return S_OK;
        }

        ICoreWebView2Environment* storedEnvironment = nullptr;
        HWND hostWindow = nullptr;
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            cancelled = ActiveState().shutdownPending || !ActiveState().createRequested;
            if (!cancelled) {
                ActiveState().environment = environment;
                ActiveState().environment->AddRef();
                storedEnvironment = ActiveState().environment;
                hostWindow = ActiveState().hostWindow;
            }
        }
        if (cancelled) {
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            ActiveState().createInFlight = false;
            ActiveState().createRequested = false;
            ActiveState().shutdownPending = false;
            return S_OK;
        }
        if (!hostWindow || !IsWindow(hostWindow)) {
            XBase::Log::Error("WebView: 宿主窗口缺失，无法创建控制器");
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            ActiveState().createInFlight = false;
            ReleaseControllerLocked();
            return S_OK;
        }
        auto* handler = new ControllerCompletedHandler(state_);
        const HRESULT hr = storedEnvironment->CreateCoreWebView2Controller(hostWindow, handler);
        handler->Release();
        if (FAILED(hr)) {
            XBase::Log::Error("WebView: 控制器创建请求失败");
        }
        return S_OK;
    }
private:
    WebViewState* state_;
};

bool EnsureHostWindow() {
    if (ActiveState().hostWindow) return true;
    if (!ActiveState().gameWindow) return false;

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &HostWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        const std::wstring className = HostWindowClassName();
        windowClass.lpszClassName = className.c_str();
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            XBase::Log::Error("WebView: 宿主窗口类注册失败");
            return false;
        }
        classRegistered = true;
    }

    ActiveState().hostWindow = CreateWindowExW(
        0, HostWindowClassName().c_str(), L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 1, 1,
        ActiveState().gameWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!ActiveState().hostWindow) {
        XBase::Log::Error("WebView: 宿主窗口创建失败");
        return false;
    }
    SetPropW(ActiveState().hostWindow, kHostWindowMarker, reinterpret_cast<HANDLE>(1));
    s_hostStates[ActiveState().hostWindow] = &ActiveState();
    return true;
}

void DestroyHostWindow() {
    if (!ActiveState().hostWindow) return;
    RemovePropW(ActiveState().hostWindow, kHostWindowMarker);
    s_hostStates.erase(ActiveState().hostWindow);
    DestroyWindow(ActiveState().hostWindow);
    ActiveState().hostWindow = nullptr;
}

LRESULT CALLBACK HostWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    WebViewState* windowState = nullptr;
    const auto stateIt = s_hostStates.find(window);
    if (stateIt != s_hostStates.end()) {
        windowState = stateIt->second;
    }
    ScopedState scope(windowState, windowState ? FindInstanceId(windowState) : XBase::WebView::DefaultInstance);
    if (message == HostCloseMessage()) {
        if (windowState) {
            XBase::Log::Info("WebView: 收到新视图请求，关闭当前跨模块宿主");
            // 旧模块的 ReactUi 会在下一帧按菜单状态自动重显；
            // 关闭宿主时同步收起它，避免与新面板互相抢占。
            XBase::Hooks::SetBackgroundInputActive(false);
            XBase::Hooks::SetBackgroundRenderActive(false);
            XBase::Hooks::SetMenuVisible(false);
            std::lock_guard<std::mutex> lock(ActiveState().mutex);
            ActiveState().visible = false;
            ActiveState().createRequested = false;
            if (ActiveState().createInFlight) {
                ActiveState().shutdownPending = true;
                ApplyVisibleLocked();
            } else {
                ReleaseControllerLocked();
            }
        }
        return 0;
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        XBase::Detail::Input::HandleVirtualKey(
            static_cast<std::uint32_t>(wParam), true, (lParam & (1LL << 30)) != 0);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        XBase::Detail::Input::HandleVirtualKey(static_cast<std::uint32_t>(wParam), false, false);
        break;
    case WM_SIZE:
        if (ActiveState().controller) {
            RECT bounds{0, 0, LOWORD(lParam), HIWORD(lParam)};
            ActiveState().controller->put_Bounds(bounds);
        }
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
    case WM_SETFOCUS:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

namespace XBase::WebView {

WebViewId Create() {
    const WebViewId id = s_nextInstanceId++;
    s_instances.emplace(id, std::make_unique<WebViewState>());
    return id;
}

void Destroy(WebViewId id) {
    if (id == DefaultInstance) {
        Shutdown(id);
        return;
    }
    auto found = s_instances.find(id);
    if (found == s_instances.end()) return;
    {
        ScopedState scope(found->second.get(), id);
        Shutdown(id);
    }
    // 异步环境/控制器回调可能仍然持有状态指针，保留槽位直到进程退出，
    // 避免销毁实例后回调访问悬空内存。后续创建会使用新的 ID。
}

WebViewId CurrentInstance() {
    return ActiveInstance();
}

bool IsRuntimeAvailable(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (ActiveState().runtimeState >= 0) {
        return ActiveState().runtimeState == 1;
    }
    if (!LoadRuntime()) {
        ActiveState().runtimeState = 0;
        return false;
    }
    LPWSTR version = nullptr;
    const HRESULT hr = s_runtime.getVersion(nullptr, &version);
    if (version) {
        CoTaskMemFree(version);
    }
    ActiveState().runtimeState = SUCCEEDED(hr) ? 1 : 0;
    return ActiveState().runtimeState == 1;
}

bool IsRuntimeAvailable() {
    return IsRuntimeAvailable(ActiveInstance());
}

bool Init(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    // 只登记请求。真正的创建延迟到宿主请求页面后的 Process 安全点执行，
    // 避免在渲染钩子里重入消息循环，也避免未使用网页时启动源码进程。
    if (!IsRuntimeAvailable()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    ActiveState().initRequested = true;
    return true;
}

bool Init() {
    return Init(ActiveInstance());
}

bool IsInitialized(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    return ActiveState().initRequested || ActiveState().initialized;
}

bool IsInitialized() {
    return IsInitialized(ActiveInstance());
}

void NotifyGameInit() {
    SetVisible(DefaultInstance, false);
    std::vector<WebViewId> ids;
    for (const auto& entry : s_instances) ids.push_back(entry.first);
    for (const WebViewId id : ids) {
        SetVisible(id, false);
    }
}

void ProcessCreation();

// 菜单关闭后网页面板失去宿主，自动隐藏，避免覆盖层残留并持续抢占焦点
void ProcessMenuTransition() {
    const bool menuVisible = XBase::Hooks::IsMenuVisible();

    bool shouldHide = false;
    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        if (menuVisible) {
            ActiveState().menuWasVisible = true;
        } else if (ActiveState().menuWasVisible) {
            ActiveState().menuWasVisible = false;
            shouldHide = ActiveState().visible;
        }
    }
    if (shouldHide) {
        SetVisible(false);
    }
}

// 独占全屏下 HWND 覆盖层不可见，改为定时抓帧供宿主以贴图绘制
void ProcessCapture() {
    const unsigned long long now = static_cast<unsigned long long>(Platform::MonotonicMilliseconds());

    const bool menuVisible = XBase::Hooks::IsMenuVisible();
    bool wantCapture = false;
    bool modeChanged = false;
    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        if (ActiveState().lastModeCheckAt == 0 || now - ActiveState().lastModeCheckAt >= kCaptureModeCheckMs) {
            ActiveState().lastModeCheckAt = now;
            const bool exclusive = IsExclusiveFullscreen();
            if (exclusive != ActiveState().captureMode) {
                ActiveState().captureMode = exclusive;
                modeChanged = true;
            }
        }
        if (ActiveState().captureMode && ActiveState().initialized && ActiveState().visible && !ActiveState().captureInFlight) {
            // 菜单开着就按交互期算，否则用户停下来看面板，画面就停在几秒前的样子
            const bool active = menuVisible
                || now - ActiveState().lastInteractionAt < kCaptureActiveWindowMs;
            ActiveState().captureActive = active;
            const unsigned long long interval = active ? kCaptureActiveIntervalMs : kCaptureIdleIntervalMs;
            wantCapture = ActiveState().lastCaptureAt == 0 || now - ActiveState().lastCaptureAt >= interval;
        }
        if (modeChanged && ActiveState().hostWindow && ActiveState().gameWindow) {
            // 抓帧模式改变渲染表面大小，重新应用一次边界
            ActiveState().boundsApplied = false;
            ApplyBoundsLocked();
        }
    }
    if (wantCapture) {
        StartCapture();
    }
}

// 面板获得焦点期间按键只到达网页子窗口，用系统状态补齐输入，保证菜单热键仍可关闭菜单
void ProcessKeyboardFallback() {
    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        if (!ActiveState().initialized || !ActiveState().visible) return;
    }
    XBase::Detail::Input::PollFromSystem();
}

// 游戏在游玩状态会隐藏系统光标，面板以原生窗口显示时需要把光标重新显示出来，
// 独占全屏抓帧预览不占屏幕，保持游戏自己的光标状态
void ProcessCursorVisibility() {
    bool panelShown = false;
    HWND hostWindow = nullptr;
    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        panelShown = ActiveState().initialized && ActiveState().visible && !ActiveState().captureMode;
        hostWindow = ActiveState().hostWindow;
    }

    if (panelShown) {
        CURSORINFO info{};
        info.cbSize = sizeof(info);
        if (GetCursorInfo(&info) && info.flags == 0 && ShowCursor(TRUE) >= 0) {
            ++ActiveState().cursorShows;
        }

        // 游戏在游玩状态会把光标形状设成空，落在面板上时补回箭头形状，
        // 否则只有点击让网页子窗口拿到焦点后光标才可见
        if (hostWindow && IsWindow(hostWindow)) {
            POINT cursor{};
            RECT client{};
            if (GetCursorPos(&cursor) && GetClientRect(hostWindow, &client)) {
                POINT topLeft{client.left, client.top};
                POINT bottomRight{client.right, client.bottom};
                if (ClientToScreen(hostWindow, &topLeft) && ClientToScreen(hostWindow, &bottomRight)) {
                    const RECT screenRect{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
                    if (PtInRect(&screenRect, cursor)) {
                        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
                    }
                }
            }
        }
        return;
    }

    if (ActiveState().cursorShows > 0) {
        for (int index = 0; index < ActiveState().cursorShows; ++index) {
            ShowCursor(FALSE);
        }
        ActiveState().cursorShows = 0;
    }
}

void Process() {
    ProcessMenuTransition();
    ProcessCreation();
    ProcessCapture();
    ProcessKeyboardFallback();
    ProcessCursorVisibility();

    std::vector<WebViewId> ids;
    ids.reserve(s_instances.size());
    for (const auto& entry : s_instances) ids.push_back(entry.first);
    for (const WebViewId id : ids) {
        WebViewState* state = FindState(id);
        if (!state) continue;
        ScopedState scope(state, id);
        ProcessMenuTransition();
        ProcessCreation();
        ProcessCapture();
        ProcessKeyboardFallback();
        ProcessCursorVisibility();
    }
}

void ProcessCreation() {
    if (!ActiveState().createRequested || ActiveState().initialized || ActiveState().createInFlight
        || ActiveState().shutdownPending) {
        return;
    }

    const HWND gameWindow = Detail::Hooks::GetGameWindow();
    const HWND fallbackWindow = gameWindow ? gameWindow : GetForegroundWindow();
    if (!fallbackWindow) {
        return;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        Log::Error("WebView: COM 初始化失败");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        if (!ActiveState().createRequested || ActiveState().initialized || ActiveState().createInFlight) {
            return;
        }
        ActiveState().gameWindow = fallbackWindow;
        if (!EnsureHostWindow()) {
            ActiveState().createRequested = false;
            Log::Error("WebView: 宿主窗口创建失败");
            return;
        }
    }

    // 网页视图用户数据放用户应用数据下 com.yuinijika.xbase 的 webview2 目录，不落在游戏目录，
    // 游戏目录常被整体打包分享，EBWebView 里的缓存与登录态不该跟着一起走
    std::string dataFolder = XBase::Platform::AppDataDirectory();
    if (!dataFolder.empty()) {
        // EnsureDirectory 只建最后一级，先把 com.yuinijika.xbase 建出来
        Platform::EnsureDirectory(dataFolder);
        const std::string sharedFolder = dataFolder + "webview2\\";
        Platform::EnsureDirectory(sharedFolder);
        dataFolder = sharedFolder + "instance_" + std::to_string(ActiveInstance());
    }
    if (dataFolder.empty() || !Platform::EnsureDirectory(dataFolder)) {
        // AppData 不可用时退回游戏目录，至少保证网页视图可用
        const std::string sharedFolder = XBase::Platform::XBaseDirectory() + "webview2\\";
        Platform::EnsureDirectory(sharedFolder);
        dataFolder = sharedFolder + "instance_" + std::to_string(ActiveInstance());
        Platform::EnsureDirectory(dataFolder);
    }

    static bool legacyDataFolderWarned = false;
    if (!legacyDataFolderWarned) {
        legacyDataFolderWarned = true;
        const std::string legacy = XBase::Platform::XBaseDirectory() + "webview2";
        if (Platform::DirectoryExists(legacy)) {
            Log::Warn("WebView: 检测到旧用户数据目录 <游戏目录>\\XBase\\webview2，已改用 AppData，旧目录可手动删除");
        }
    }

    auto* handler = new EnvironmentCompletedHandler(&ActiveState());
    const HRESULT hr = s_runtime.createEnvironment(
        nullptr,
        WideFrom(dataFolder).c_str(),
        nullptr,
        handler);
    handler->Release();

    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (FAILED(hr)) {
        ActiveState().createRequested = false;
        Log::Error("WebView: WebView2 环境创建请求失败");
        return;
    }
    ActiveState().createInFlight = true;
}

// 关闭面板会释放浏览器与宿主窗口，之后再次设为可见会重新创建
bool Close() {
    if (!IsRuntimeAvailable()) return false;

    SetVisible(false);

    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    ActiveState().initRequested = true;
    ActiveState().createRequested = false;
    if (ActiveState().createInFlight) {
        ActiveState().shutdownPending = true;
        return true;
    }
    ReleaseControllerLocked();
    return true;
}

void Shutdown() {
    if (!t_activeState) {
        Shutdown(DefaultInstance);
        std::vector<WebViewId> ids;
        for (const auto& entry : s_instances) ids.push_back(entry.first);
        for (const WebViewId id : ids) {
            Shutdown(id);
        }
        return;
    }
    SetVisible(false);

    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    ActiveState().initRequested = false;
    ActiveState().createRequested = false;
    if (ActiveState().createInFlight) {
        // 创建中的控制器到达后自行释放并销毁窗口，避免父窗口句柄被复用
        ActiveState().shutdownPending = true;
        return;
    }
    ReleaseControllerLocked();
}

bool Navigate(const std::string& url) {
    if (url.empty()) return false;
    if (!IsRuntimeAvailable()) return false;
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    ActiveState().initRequested = true;
    ActiveState().createRequested = true;
    ActiveState().sourceUrl = url;
    ActiveState().sourceHtml.clear();
    if (!ActiveState().webview) {
        ActiveState().pendingUrl = url;
        ActiveState().pendingHtml.clear();
        return true;
    }
    return SUCCEEDED(ActiveState().webview->Navigate(WideFrom(url).c_str()));
}

bool SetHtml(const std::string& html) {
    if (html.empty()) return false;
    if (!IsRuntimeAvailable()) return false;
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    ActiveState().initRequested = true;
    ActiveState().createRequested = true;
    ActiveState().sourceHtml = html;
    ActiveState().sourceUrl.clear();
    if (!ActiveState().webview) {
        ActiveState().pendingHtml = html;
        ActiveState().pendingUrl.clear();
        return true;
    }
    return SUCCEEDED(ActiveState().webview->NavigateToString(WideFrom(html).c_str()));
}

void Reload() {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (ActiveState().webview) {
        ActiveState().webview->Reload();
    }
}

bool GoBack() {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (!ActiveState().webview || !ActiveState().canGoBack) return false;
    return SUCCEEDED(ActiveState().webview->GoBack());
}

bool GoForward() {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (!ActiveState().webview || !ActiveState().canGoForward) return false;
    return SUCCEEDED(ActiveState().webview->GoForward());
}

void SetZoom(float factor) {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (factor < 0.25f) factor = 0.25f;
    if (factor > 4.0f) factor = 4.0f;
    ActiveState().zoom = factor;
    ApplyZoomLocked();
}

void SetVisible(bool visible) {
    const XBase::WebView::WebViewId currentId = ActiveInstance();
    if (visible) {
        CloseOtherInstances(currentId);
    }

    // 显示面板即视为请求懒创建，创建在 Process 的安全点执行
    const bool runtimeAvailable = visible ? IsRuntimeAvailable() : false;
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (ActiveState().visible == visible) return;
    ActiveState().visible = visible;
    if (visible) {
        if (!runtimeAvailable) {
            ActiveState().visible = false;
            return;
        }
        ActiveState().initRequested = true;
        ActiveState().createRequested = true;
        if (!ActiveState().boundsApplied) {
            ApplyBoundsLocked();
        }
    }
    ApplyVisibleLocked();
}

bool IsVisible() {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    return ActiveState().visible;
}

void SetBounds(const Rect& bounds) {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (ActiveState().boundsApplied
        && ActiveState().bounds.left == bounds.left
        && ActiveState().bounds.top == bounds.top
        && ActiveState().bounds.right == bounds.right
        && ActiveState().bounds.bottom == bounds.bottom) {
        return;
    }
    ActiveState().bounds = bounds;
    ApplyBoundsLocked();
}

State GetState() {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    State state;
    state.initialized = ActiveState().initialized;
    state.visible = ActiveState().visible;
    state.loading = ActiveState().loading;
    state.canGoBack = ActiveState().canGoBack;
    state.canGoForward = ActiveState().canGoForward;
    state.lastError = ActiveState().lastError;
    state.url = ActiveState().url;
    state.title = ActiveState().title;
    return state;
}

void SetStateCallback(StateCallback callback) {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    ActiveState().stateCallback = callback;
}

void SetMessageHandler(MessageHandler handler) {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    ActiveState().messageHandler = std::move(handler);
}

bool MapFolder(const std::string& hostName, const std::string& folderPath) {
    if (hostName.empty() || folderPath.empty()) return false;
    std::lock_guard<std::mutex> lock(ActiveState().mutex);

    bool replaced = false;
    for (auto& entry : ActiveState().virtualHosts) {
        if (entry.first == hostName) {
            entry.second = folderPath;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        ActiveState().virtualHosts.emplace_back(hostName, folderPath);
    }

    if (ActiveState().webview) {
        ApplyVirtualHostsLocked(ActiveState().webview);
    }
    return true;
}

bool PostJson(const std::string& json) {
    if (json.empty()) return false;
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    if (!ActiveState().webview) return false;
    return SUCCEEDED(ActiveState().webview->PostWebMessageAsJson(WideFrom(json).c_str()));
}

bool InjectScript(const std::string& script) {
    if (script.empty()) return false;
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    // 先进持久清单，之后无论控制器重建多少次都会重新注入
    ActiveState().documentScripts.push_back(script);
    if (!ActiveState().webview) {
        return true;
    }
    return SUCCEEDED(ActiveState().webview->AddScriptToExecuteOnDocumentCreated(WideFrom(script).c_str(), nullptr));
}

bool UsesCaptureMode() {
    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    return ActiveState().initialized && ActiveState().captureMode;
}

void DrawPanel(const Rect& bounds) {
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    if (width <= 0.0f || height <= 0.0f) return;

    std::lock_guard<std::mutex> lock(ActiveState().mutex);
    ImGui::SetCursorScreenPos(ImVec2(bounds.left, bounds.top));
    if (ActiveState().texture && ActiveState().previewReady) {
        ImGui::Image(reinterpret_cast<ImTextureID>(ActiveState().texture), ImVec2(width, height));
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 minimum(bounds.left, bounds.top);
    const ImVec2 maximum(bounds.left + width, bounds.top + height);
    drawList->AddRectFilled(minimum, maximum, IM_COL32(18, 20, 26, 235));
    drawList->AddText(ImVec2(minimum.x + 14.0f, minimum.y + 14.0f), IM_COL32(190, 196, 210, 255), "...");
    ImGui::Dummy(ImVec2(width, height));
}

void ForwardPanelInput(const Rect& bounds, Vec2 mouse, bool mouseDown, float wheelDelta) {
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    if (width <= 0.0f || height <= 0.0f) return;

    int pageWidth = 0;
    int pageHeight = 0;
    {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        pageWidth = ActiveState().textureWidth;
        pageHeight = ActiveState().textureHeight;
    }
    if (pageWidth <= 0 || pageHeight <= 0) return;

    const float x = std::clamp((mouse.x - bounds.left) * pageWidth / width, 0.0f, pageWidth - 1.0f);
    const float y = std::clamp((mouse.y - bounds.top) * pageHeight / height, 0.0f, pageHeight - 1.0f);

    const bool clickEdge = mouseDown && !ActiveState().previousMouseDown;
    ActiveState().previousMouseDown = mouseDown;

    const unsigned long long now = static_cast<unsigned long long>(Platform::MonotonicMilliseconds());
    const bool movedEnough = !ActiveState().forwardedMoveValid
        || std::abs(x - ActiveState().lastForwardedMoveX) >= kInteractionMoveThreshold
        || std::abs(y - ActiveState().lastForwardedMoveY) >= kInteractionMoveThreshold;
    const bool moved = movedEnough && now - ActiveState().lastForwardedMoveAt >= kMoveForwardIntervalMs;
    if (moved) {
        ActiveState().lastForwardedMoveX = x;
        ActiveState().lastForwardedMoveY = y;
        ActiveState().lastForwardedMoveAt = now;
        ActiveState().forwardedMoveValid = true;
        char script[512]{};
        std::snprintf(script, sizeof(script),
            "(function(){var e=document.elementFromPoint(%.1f,%.1f);if(!e)return;"
            "e.dispatchEvent(new MouseEvent('mousemove',{bubbles:true,cancelable:true,clientX:%.1f,clientY:%.1f,view:window}));})()",
            x, y, x, y);
        ExecuteScript(script);
    }

    if (clickEdge) {
        char script[512]{};
        std::snprintf(script, sizeof(script),
            "(function(){var e=document.elementFromPoint(%.1f,%.1f);if(!e)return;"
            "var o={bubbles:true,cancelable:true,clientX:%.1f,clientY:%.1f,view:window};"
            "e.focus&&e.focus();"
            "e.dispatchEvent(new MouseEvent('mousedown',o));"
            "e.dispatchEvent(new MouseEvent('mouseup',o));"
            "e.dispatchEvent(new MouseEvent('click',o));})()",
            x, y, x, y);
        ExecuteScript(script);
    }
    if (wheelDelta != 0.0f) {
        char script[128]{};
        std::snprintf(script, sizeof(script), "window.scrollBy(0,%.0f);", static_cast<double>(-wheelDelta * 120.0f));
        ExecuteScript(script);
    }

    if (moved || clickEdge || wheelDelta != 0.0f) {
        std::lock_guard<std::mutex> lock(ActiveState().mutex);
        ActiveState().lastInteractionAt = now;
        if (clickEdge || wheelDelta != 0.0f) {
            ActiveState().lastCaptureAt = 0;
        }
    }
}

bool Close(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return Close();
}

void Shutdown(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    Shutdown();
}

bool Navigate(WebViewId id, const std::string& url) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return Navigate(url);
}

bool SetHtml(WebViewId id, const std::string& html) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return SetHtml(html);
}

void Reload(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    Reload();
}

bool GoBack(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return GoBack();
}

bool GoForward(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return GoForward();
}

void SetZoom(WebViewId id, float factor) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    SetZoom(factor);
}

void SetVisible(WebViewId id, bool visible) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    SetVisible(visible);
}

bool IsVisible(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return IsVisible();
}

void SetBounds(WebViewId id, const Rect& bounds) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    SetBounds(bounds);
}

State GetState(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return {};
    ScopedState scope(state, id);
    return GetState();
}

void SetStateCallback(WebViewId id, StateCallback callback) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    SetStateCallback(callback);
}

void SetMessageHandler(WebViewId id, MessageHandler handler) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    SetMessageHandler(std::move(handler));
}

bool PostJson(WebViewId id, const std::string& json) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return PostJson(json);
}

bool InjectScript(WebViewId id, const std::string& script) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return InjectScript(script);
}

bool MapFolder(WebViewId id, const std::string& hostName, const std::string& folderPath) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return MapFolder(hostName, folderPath);
}

bool UsesCaptureMode(WebViewId id) {
    WebViewState* state = FindState(id);
    if (!state) return false;
    ScopedState scope(state, id);
    return UsesCaptureMode();
}

void DrawPanel(WebViewId id, const Rect& bounds) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    DrawPanel(bounds);
}

void ForwardPanelInput(WebViewId id, const Rect& bounds, Vec2 mouse, bool mouseDown, float wheelDelta) {
    WebViewState* state = FindState(id);
    if (!state) return;
    ScopedState scope(state, id);
    ForwardPanelInput(bounds, mouse, mouseDown, wheelDelta);
}

} // namespace XBase::WebView
