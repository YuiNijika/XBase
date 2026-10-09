#include <XBase/Hooks.h>
#include <XBase/Log.h>
#include <XBase/Platform.h>
#include <XBase/Theme.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

#if defined(XBASE_WITH_KIERO)
#include <Windows.h>
#include <d3d9.h>
#include "kiero/kiero.h"
#include "kiero/minhook/MinHook.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx9.h"
#include "plugin.h"
#include "CPad.h"
#include "InputInternal.h"
#include "RenderFonts.h"
#include "../backends/TargetingBackend.h"

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using EndSceneFn = HRESULT(__stdcall*)(LPDIRECT3DDEVICE9);
using ResetFn = HRESULT(__stdcall*)(LPDIRECT3DDEVICE9, D3DPRESENT_PARAMETERS*);

struct DrawCallbackEntry {
    XBase::Hooks::DrawCallbackId id;
    std::function<void()> callback;
};

XBase::Hooks::RuntimeState g_state = XBase::Hooks::RuntimeState::Uninitialized;
std::mutex g_drawCallbacksMutex;
std::vector<DrawCallbackEntry> g_drawCallbacks;
std::uint64_t g_nextDrawCallbackId = 1;
bool g_menuVisible = false;
bool g_backgroundInputActive = false;
bool g_backgroundRenderActive = false;
float g_wheelDelta = 0.0f;
std::atomic<bool> g_wheelSuppressed{false};
std::atomic<bool> g_middleSuppressed{false};
const char* g_statusText = "not initialized";
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
EndSceneFn g_originalEndScene = nullptr;
ResetFn g_originalReset = nullptr;
#if defined(GTAVC)
using UpdateMouseFn = void(__cdecl*)();
#else
using UpdateMouseFn = void(__thiscall*)(CPad*);
#endif
UpdateMouseFn g_originalUpdateMouse = nullptr;
void* g_updateMouseTarget = nullptr;
bool g_gameInputBlocked = false;
std::array<bool, 256> g_keysHeldBeforeBlock{};
IDirect3DDevice9* g_device = nullptr;

// 显示模式表与场景相机地址，取自参考实现 III.VC.SA.WindowedMode
#if defined(GTASA)
constexpr std::uintptr_t kVideoModeListAddress = 0xC97C48;
constexpr std::uintptr_t kSceneCameraAddress = 0xC1703C;
constexpr std::uintptr_t kSetupBackBufferVertexAddress = 0x7043D0;
#elif defined(GTAVC)
constexpr std::uintptr_t kVideoModeListAddress = 0x7897D0;
constexpr std::uintptr_t kSceneCameraAddress = 0x8100BC;
constexpr std::uintptr_t kBlurOpenAddress = 0x55CE20;
#elif defined(GTA3)
constexpr std::uintptr_t kVideoModeListAddress = 0x662F18;
constexpr std::uintptr_t kSceneCameraAddress = 0x72676C;
constexpr std::uintptr_t kBlurOpenAddress = 0x50AE40;
#endif

// 平台显示模式表项，布局与参考实现的 DisplayMode 一致，
// 前端菜单会按这张表里的分辨率排版，窗口模式下必须同步成客户区大小
struct DisplayModeEntry {
    unsigned int width;
    unsigned int height;
    unsigned int refreshRate;
    unsigned int format;
    unsigned int flags;
};
std::vector<DisplayModeEntry> g_windowModeVideoModeBackup;
int g_windowModeVideoModeIndex = -1;

// 游戏自身的呈现参数地址，取自参考实现 III.VC.SA.WindowedMode
#if defined(GTASA)
constexpr std::uintptr_t kGamePresentParametersAddress = 0xC9C040;
#elif defined(GTAVC)
constexpr std::uintptr_t kGamePresentParametersAddress = 0xA0FD04;
#elif defined(GTA3)
constexpr std::uintptr_t kGamePresentParametersAddress = 0x943010;
#endif

// VC/III 是 D3D8 结构，比 D3D9 少了 MultiSampleQuality 字段
struct PresentParameters8 {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    DWORD BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType;
    DWORD SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};

#if defined(GTASA)
using GamePresentParameters = D3DPRESENT_PARAMETERS;
#else
using GamePresentParameters = PresentParameters8;
#endif

constexpr std::size_t kGamePresentParametersSize = sizeof(GamePresentParameters);
std::array<unsigned char, kGamePresentParametersSize> g_windowModeSavedPresentParameters{};
bool g_windowModeHasSavedPresentParameters = false;

// 窗口模式状态。切换到非全屏时保存原始的呈现参数、窗口样式与显示状态，
// 切回全屏时按保存值恢复
XBase::Hooks::WindowMode g_windowMode = XBase::Hooks::WindowMode::Fullscreen;
bool g_windowModePrepared = false;
bool g_windowModeHasSavedState = false;
D3DPRESENT_PARAMETERS g_windowModeSavedParameters{};
LONG_PTR g_windowModeSavedStyle = 0;
LONG_PTR g_windowModeSavedExStyle = 0;
RECT g_windowModeSavedWindowRect{};
int g_windowModeSavedScreenWidth = 0;
int g_windowModeSavedScreenHeight = 0;
int g_windowModeSavedMaximumWidth = 0;
int g_windowModeSavedMaximumHeight = 0;
bool g_windowModeSavedFullScreen = true;
unsigned long long g_windowModeSettleAt = 0;
unsigned long long g_windowModeStartupGraceUntil = 0;
int g_windowModeResizeAttempts = 0;
unsigned long long g_windowModeLastCheckAt = 0;
bool g_windowModeCursorClipped = false;

std::atomic<bool> g_shutdownRequested{false};
std::atomic<unsigned int> g_activeRenderCallbacks{0};
std::mutex g_renderCallbacksMutex;
std::condition_variable g_renderCallbacksIdle;

class RenderCallbackScope {
public:
    RenderCallbackScope() {
        g_activeRenderCallbacks.fetch_add(1, std::memory_order_acq_rel);
        renderAllowed_ = !g_shutdownRequested.load(std::memory_order_acquire);
    }

    ~RenderCallbackScope() {
        if (g_activeRenderCallbacks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            g_renderCallbacksIdle.notify_all();
        }
    }

    explicit operator bool() const { return renderAllowed_; }

private:
    bool renderAllowed_ = false;
};

void WaitForRenderCallbacks() {
    std::unique_lock<std::mutex> lock(g_renderCallbacksMutex);
    g_renderCallbacksIdle.wait(lock, []() {
        return g_activeRenderCallbacks.load(std::memory_order_acquire) == 0;
    });
}

std::vector<std::function<void()>> g_drawCallbacksSnapshot;
bool g_drawCallbacksSnapshotDirty = true;

const std::vector<std::function<void()>>& SnapshotDrawCallbacks() {
    std::lock_guard<std::mutex> lock(g_drawCallbacksMutex);
    if (!g_drawCallbacksSnapshotDirty) {
        return g_drawCallbacksSnapshot;
    }
    g_drawCallbacksSnapshot.clear();
    g_drawCallbacksSnapshot.reserve(g_drawCallbacks.size());
    for (const DrawCallbackEntry& entry : g_drawCallbacks) {
        g_drawCallbacksSnapshot.push_back(entry.callback);
    }
    g_drawCallbacksSnapshotDirty = false;
    return g_drawCallbacksSnapshot;
}

struct InputPatchSnapshot {
    uintptr_t address = 0;
    std::array<unsigned char, 5> bytes{};
    std::size_t size = 0;
    bool captured = false;
};

std::array<InputPatchSnapshot, 4> g_inputPatches;

void CaptureInputPatch(InputPatchSnapshot& patch) {
    if (patch.captured || patch.address == 0 || patch.size == 0) return;
    plugin::patch::GetRaw(patch.address, patch.bytes.data(), patch.size);
    patch.captured = true;
}

void RestoreInputPatch(InputPatchSnapshot& patch) {
    if (!patch.captured) return;
    plugin::patch::SetRaw(patch.address, patch.bytes.data(), patch.size);
    patch.captured = false;
}

void ConfigureInputPatches() {
#if defined(GTASA)
    g_inputPatches = {{{0x6194A0, {}, 1, false}, {0x541DD7, {}, 5, false}, {0x4EB731, {}, 1, false}, {0x4EB75A, {}, 1, false}}};
#elif defined(GTAVC)
    g_inputPatches = {{{0x6020A0, {}, 1, false}, {0x4AB6CA, {}, 5, false}, {}, {}}};
#elif defined(GTA3)
    g_inputPatches = {{{0x580D20, {}, 1, false}, {0x49272F, {}, 5, false}, {}, {}}};
#else
    g_inputPatches = {};
#endif
}

void ClearMouseState() {
    CPad* pad = CPad::GetPad(0);
    if (!pad) return;
    CPad::NewMouseControllerState = {};
    CPad::OldMouseControllerState = {};
    CPad::PCTempMouseControllerState = {};
    (void)pad;
}

void FilterMouseInput() {
    if (!g_middleSuppressed.load(std::memory_order_acquire)) return;
    CPad::NewMouseControllerState.mmb = 0;
    CPad::OldMouseControllerState.mmb = 0;
    CPad::PCTempMouseControllerState.mmb = 0;
    XBase::Detail::TargetingBackend::UpdatePointerInput();
}

#if defined(GTAVC)
void __cdecl UpdateMouse() {
    RenderCallbackScope callbackScope;
    g_originalUpdateMouse();
    if (callbackScope) FilterMouseInput();
}
#else
void __fastcall UpdateMouse(CPad* pad, void*) {
    RenderCallbackScope callbackScope;
    g_originalUpdateMouse(pad);
    if (callbackScope) FilterMouseInput();
}
#endif

bool InstallMouseInputHook() {
#if defined(GTAVC)
    g_updateMouseTarget = reinterpret_cast<void*>(gaddrof(CPad::UpdateMouse));
#elif defined(GTASA)
    g_updateMouseTarget = reinterpret_cast<void*>(0x53F3C0);
#elif defined(GTA3)
    g_updateMouseTarget = reinterpret_cast<void*>(0x491CA0);
#endif
    if (!g_updateMouseTarget) return false;
    if (MH_CreateHook(g_updateMouseTarget, reinterpret_cast<void*>(&UpdateMouse),
            reinterpret_cast<void**>(&g_originalUpdateMouse)) != MH_OK) {
        g_updateMouseTarget = nullptr;
        return false;
    }
    if (MH_EnableHook(g_updateMouseTarget) == MH_OK) return true;
    MH_RemoveHook(g_updateMouseTarget);
    g_updateMouseTarget = nullptr;
    g_originalUpdateMouse = nullptr;
    return false;
}

// 菜单期间按下的键若在关闭瞬间仍未松开，游戏恢复输入的第一帧会把它当作菜单操作
bool AnyGameKeyDown() {
    for (int key = 0x08; key < 0x100; ++key) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return true;
    }
    return false;
}

// 打开菜单之前就已经按住的键，玩家是在走路或开车时顺手开的菜单，
// 这类键不算菜单操作，否则关掉菜单后只要还按着移动键就会被一直锁住。
// 只有菜单打开之后才按下的键才值得等它松开
bool AnyMenuKeyDown() {
    for (int key = 0x08; key < 0x100; ++key) {
        if (g_keysHeldBeforeBlock[key]) continue;
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return true;
    }
    return false;
}

void RememberKeysHeldBeforeBlock() {
    for (int key = 0x08; key < 0x100; ++key) {
        g_keysHeldBeforeBlock[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }
}

bool WantsInput();

bool CapturePresentParameters(D3DPRESENT_PARAMETERS& parameters) {
    if (!g_device) return false;
    IDirect3DSwapChain9* chain = nullptr;
    if (FAILED(g_device->GetSwapChain(0, &chain)) || !chain) return false;
    const bool ok = SUCCEEDED(chain->GetPresentParameters(&parameters));
    chain->Release();
    return ok;
}

bool MonitorRect(HWND window, RECT& rect) {
    if (!window || !IsWindow(window)) return false;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) return false;
    rect = info.rcMonitor;
    return true;
}

LONG_PTR WindowStyleFor(XBase::Hooks::WindowMode mode) {
    if (mode == XBase::Hooks::WindowMode::Borderless) {
        return WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS;
    }
    return WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS;
}

// 不设 TOPMOST，否则游戏会一直压在别的程序之上，其他窗口无法覆盖
LONG_PTR WindowExStyleFor(XBase::Hooks::WindowMode) {
    return 0;
}

SIZE WindowSizeFor(XBase::Hooks::WindowMode mode, const SIZE& client) {
    RECT frame{0, 0, client.cx, client.cy};
    AdjustWindowRectEx(&frame, static_cast<DWORD>(WindowStyleFor(mode)), FALSE, static_cast<DWORD>(WindowExStyleFor(mode)));
    SIZE size{frame.right - frame.left, frame.bottom - frame.top};
    if (size.cx <= 0) size.cx = client.cx;
    if (size.cy <= 0) size.cy = client.cy;
    return size;
}

// 按模式摆放窗口，无边框铺满显示器，窗口模式居中并留出边框
void ApplyWindowModeGeometry(HWND window, XBase::Hooks::WindowMode mode, SIZE& clientSize, bool moveWindow) {
    RECT monitor{};
    if (!MonitorRect(window, monitor)) return;

    const LONG_PTR style = WindowStyleFor(mode);
    const LONG_PTR exStyle = WindowExStyleFor(mode);
    const int monitorWidth = monitor.right - monitor.left;
    const int monitorHeight = monitor.bottom - monitor.top;

    int windowWidth = 0;
    int windowHeight = 0;
    int x = 0;
    int y = 0;
    if (mode == XBase::Hooks::WindowMode::Borderless) {
        clientSize.cx = monitorWidth;
        clientSize.cy = monitorHeight;
        windowWidth = monitorWidth;
        windowHeight = monitorHeight;
        x = monitor.left;
        y = monitor.top;
        moveWindow = true;
    } else {
        RECT frame{0, 0, 0, 0};
        AdjustWindowRectEx(&frame, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle));
        const int frameWidth = frame.right - frame.left;
        const int frameHeight = frame.bottom - frame.top;
        if (clientSize.cx > monitorWidth - frameWidth) clientSize.cx = monitorWidth - frameWidth;
        if (clientSize.cy > monitorHeight - frameHeight) clientSize.cy = monitorHeight - frameHeight;
        const SIZE size = WindowSizeFor(mode, clientSize);
        windowWidth = size.cx;
        windowHeight = size.cy;
        x = monitor.left + (monitorWidth - windowWidth) / 2;
        y = monitor.top + (monitorHeight - windowHeight) / 2;
    }

    RECT current{};
    GetWindowRect(window, &current);
    const bool geometryMatches = current.left == x && current.top == y
        && current.right - current.left == windowWidth
        && current.bottom - current.top == windowHeight;
    const bool styleMatches = GetWindowLongPtrW(window, GWL_STYLE) == style
        && GetWindowLongPtrW(window, GWL_EXSTYLE) == exStyle;
    if (styleMatches && (!moveWindow || geometryMatches)) return;

    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, exStyle);
    SetWindowPos(
        window, HWND_NOTOPMOST,
        x, y, windowWidth, windowHeight,
        (moveWindow ? 0 : SWP_NOMOVE | SWP_NOSIZE) | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
}

void RestoreSavedWindowStyle(HWND window) {
    if (!window || !IsWindow(window)) return;
    if (g_windowModeSavedStyle != 0) {
        SetWindowLongPtrW(window, GWL_STYLE, g_windowModeSavedStyle);
    }
    SetWindowLongPtrW(window, GWL_EXSTYLE, g_windowModeSavedExStyle);
    if (g_windowModeSavedWindowRect.right > g_windowModeSavedWindowRect.left
        && g_windowModeSavedWindowRect.bottom > g_windowModeSavedWindowRect.top) {
        SetWindowPos(
            window, nullptr,
            g_windowModeSavedWindowRect.left, g_windowModeSavedWindowRect.top,
            g_windowModeSavedWindowRect.right - g_windowModeSavedWindowRect.left,
            g_windowModeSavedWindowRect.bottom - g_windowModeSavedWindowRect.top,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

// 记录显示状态细节，用于定位模式切换问题
void LogDisplayDiagnostics(const char* stage) {
    D3DPRESENT_PARAMETERS present{};
    const bool hasPresent = CapturePresentParameters(present);
    RECT windowRect{};
    RECT client{};
    if (g_window && IsWindow(g_window)) {
        GetWindowRect(g_window, &windowRect);
        GetClientRect(g_window, &client);
    }

    char message[320]{};
    std::snprintf(message, sizeof(message),
        "Hooks: [%s] present=%d windowed=%d bb=%ux%u fmt=%u refresh=%u fullScreen=%d style=0x%08X window=%d,%d %dx%d client=%dx%d",
        stage, hasPresent ? 1 : 0, hasPresent ? static_cast<int>(present.Windowed) : -1,
        hasPresent ? present.BackBufferWidth : 0, hasPresent ? present.BackBufferHeight : 0,
        hasPresent ? static_cast<unsigned int>(present.BackBufferFormat) : 0,
        hasPresent ? present.FullScreen_RefreshRateInHz : 0,
        RsGlobal.ps && RsGlobal.ps->fullScreen ? 1 : 0,
        g_window && IsWindow(g_window) ? static_cast<unsigned int>(GetWindowLongPtrW(g_window, GWL_STYLE)) : 0,
        static_cast<int>(windowRect.left), static_cast<int>(windowRect.top),
        static_cast<int>(windowRect.right - windowRect.left), static_cast<int>(windowRect.bottom - windowRect.top),
        static_cast<int>(client.right - client.left), static_cast<int>(client.bottom - client.top));
    XBase::Log::Info(message);
}

// 设备创建钩子需要提前声明，避免与后面的同步函数互相依赖
void SyncGameDisplayState();
void SyncGamePresentParameters(HWND window);

#if defined(GTASA)
using CreateDevice9Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDevice9Fn g_originalCreateDevice9 = nullptr;
#else
using CreateDevice8Fn = HRESULT(STDMETHODCALLTYPE*)(
    void*, UINT, D3DDEVTYPE, HWND, DWORD, PresentParameters8*, void**);
CreateDevice8Fn g_originalCreateDevice8 = nullptr;
#endif
bool g_startupWindowModePrepared = false;

// 设备创建钩子是参考实现的关键一步，游戏从一开始就建立窗口期交换链
#if defined(GTASA)
HRESULT STDMETHODCALLTYPE CreateDevice9Hook(
    IDirect3D9* self, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow,
    DWORD behaviorFlags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device) {
    if (parameters && focusWindow && g_windowMode != XBase::Hooks::WindowMode::Fullscreen) {
        if (!g_window) g_window = focusWindow;
        SIZE clientSize{};
        RECT monitor{};
        if (MonitorRect(focusWindow, monitor)) {
            clientSize.cx = monitor.right - monitor.left;
            clientSize.cy = monitor.bottom - monitor.top;
        }
        if (g_windowMode == XBase::Hooks::WindowMode::Windowed) {
            clientSize.cx = 1280;
            clientSize.cy = 720;
        }
        ApplyWindowModeGeometry(focusWindow, g_windowMode, clientSize, true);

        parameters->Windowed = TRUE;
        parameters->hDeviceWindow = focusWindow;
        parameters->BackBufferWidth = static_cast<UINT>(clientSize.cx);
        parameters->BackBufferHeight = static_cast<UINT>(clientSize.cy);
        parameters->SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters->FullScreen_RefreshRateInHz = 0;
        parameters->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        SyncGameDisplayState();
        SyncGamePresentParameters(focusWindow);
    }
    return g_originalCreateDevice9(self, adapter, deviceType, focusWindow, behaviorFlags, parameters, device);
}
#else
HRESULT STDMETHODCALLTYPE CreateDevice8Hook(
    void* self, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow,
    DWORD behaviorFlags, PresentParameters8* parameters, void** device) {
    if (parameters && focusWindow && g_windowMode != XBase::Hooks::WindowMode::Fullscreen) {
        if (!g_window) g_window = focusWindow;
        SIZE clientSize{};
        RECT monitor{};
        if (MonitorRect(focusWindow, monitor)) {
            clientSize.cx = monitor.right - monitor.left;
            clientSize.cy = monitor.bottom - monitor.top;
        }
        if (g_windowMode == XBase::Hooks::WindowMode::Windowed) {
            clientSize.cx = 1280;
            clientSize.cy = 720;
        }
        ApplyWindowModeGeometry(focusWindow, g_windowMode, clientSize, true);

        parameters->Windowed = TRUE;
        parameters->hDeviceWindow = focusWindow;
        parameters->BackBufferWidth = static_cast<UINT>(clientSize.cx);
        parameters->BackBufferHeight = static_cast<UINT>(clientSize.cy);
        parameters->SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters->FullScreen_RefreshRateInHz = 0;
        parameters->FullScreen_PresentationInterval = 0x80000000;  // D3DPRESENT_INTERVAL_DEFAULT
        SyncGameDisplayState();
        SyncGamePresentParameters(focusWindow);
    }
    return g_originalCreateDevice8(self, adapter, deviceType, focusWindow, behaviorFlags, parameters, device);
}
#endif

bool ReplaceVTableEntry(void** vtable, int index, void* replacement, void** original) {
    if (!vtable || index < 0) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    if (original) *original = vtable[index];
    vtable[index] = replacement;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// 在游戏创建设备之前调用，让设备直接以窗口模式创建。
// 独占全屏无法在运行中转为窗口期交换链，这就是模式需要重启生效的原因。
bool PrepareStartupWindowModeInternal(XBase::Hooks::WindowMode mode) {
#if defined(XBASE_WITH_KIERO)
    if (mode == XBase::Hooks::WindowMode::Fullscreen) return true;
    if (g_startupWindowModePrepared) return true;

    g_windowMode = mode;

    void* original = nullptr;
    bool replaced = false;
#if defined(GTASA)
    // 通过已加载的 d3d9 库取接口，兼容 d3d9 包装层，保证拿到与游戏相同的虚函数表
    HMODULE module9 = GetModuleHandleW(L"d3d9.dll");
    if (!module9) module9 = LoadLibraryW(L"d3d9.dll");
    if (!module9) return false;
    using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
    auto create9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(module9, "Direct3DCreate9"));
    if (!create9) return false;
    IDirect3D9* d3d = create9(D3D_SDK_VERSION);
    if (d3d) {
        void** vtable = *reinterpret_cast<void***>(d3d);
        replaced = ReplaceVTableEntry(vtable, 16, reinterpret_cast<void*>(&CreateDevice9Hook), &original);
        d3d->Release();
    }
    g_originalCreateDevice9 = reinterpret_cast<CreateDevice9Fn>(original);
#else
    HMODULE module = GetModuleHandleW(L"d3d8.dll");
    if (!module) module = LoadLibraryW(L"d3d8.dll");
    if (!module) return false;
    using Direct3DCreate8Fn = void*(WINAPI*)(UINT);
    auto create8 = reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(module, "Direct3DCreate8"));
    if (!create8) return false;
    void* d3d = create8(220);  // D3D_SDK_VERSION for DirectX 8.1
    if (d3d) {
        void** vtable = *reinterpret_cast<void***>(d3d);
        replaced = ReplaceVTableEntry(vtable, 15, reinterpret_cast<void*>(&CreateDevice8Hook), &original);
        if (replaced) {
            using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);
            reinterpret_cast<ReleaseFn>(vtable[2])(d3d);
        }
    }
    g_originalCreateDevice8 = reinterpret_cast<CreateDevice8Fn>(original);
#endif

    if (!replaced) {
        g_windowMode = XBase::Hooks::WindowMode::Fullscreen;
        XBase::Log::Warn("Hooks: 安装设备创建钩子失败，窗口模式未准备");
        return false;
    }

    g_startupWindowModePrepared = true;
    XBase::Log::Info(mode == XBase::Hooks::WindowMode::Borderless
        ? "Hooks: 已准备无边框窗口模式，游戏将以窗口期交换链创建设备"
        : "Hooks: 已准备窗口模式，游戏将以窗口期交换链创建设备");
    return true;
#else
    (void)mode;
    return false;
#endif
}

// 让游戏自身认为运行在窗口模式，改写 RsGlobal 里的窗口与分辨率状态
void SyncGameDisplayState() {
    if (!g_window || !IsWindow(g_window)) return;
    RECT client{};
    if (!GetClientRect(g_window, &client)) return;
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0) return;

    const bool windowed = g_windowMode != XBase::Hooks::WindowMode::Fullscreen;

    RsGlobal.ps->window = g_window;
    RsGlobal.ps->fullScreen = windowed ? FALSE : TRUE;
    RsGlobal.maximumWidth = windowed ? clientWidth : g_windowModeSavedMaximumWidth;
    RsGlobal.maximumHeight = windowed ? clientHeight : g_windowModeSavedMaximumHeight;
#if !defined(GTASA)
    RsGlobal.screenWidth = windowed ? clientWidth : g_windowModeSavedScreenWidth;
    RsGlobal.screenHeight = windowed ? clientHeight : g_windowModeSavedScreenHeight;
#endif

    // 平台显示模式表决定前端菜单的排版尺寸，窗口模式下同步成客户区大小
    auto** list = reinterpret_cast<DisplayModeEntry**>(kVideoModeListAddress);
    if (!list || !*list) return;
    const int count = static_cast<int>(RwEngineGetNumVideoModes());
    const int index = static_cast<int>(RwEngineGetCurrentVideoMode());
    if (count <= 0 || index < 0 || index >= count) return;

    if (g_windowModeVideoModeBackup.empty()) {
        g_windowModeVideoModeBackup.assign(*list, *list + count);
    } else if (g_windowModeVideoModeIndex >= 0 && g_windowModeVideoModeIndex < count
        && g_windowModeVideoModeIndex < static_cast<int>(g_windowModeVideoModeBackup.size())) {
        (*list)[g_windowModeVideoModeIndex] = g_windowModeVideoModeBackup[g_windowModeVideoModeIndex];
    }
    g_windowModeVideoModeIndex = index;

    DisplayModeEntry& mode = (*list)[index];
    if (windowed) {
        mode.width = static_cast<unsigned int>(clientWidth);
        mode.height = static_cast<unsigned int>(clientHeight);
        mode.refreshRate = 0;
#if defined(GTASA)
        mode.format = static_cast<unsigned int>(D3DFMT_A8R8G8B8);
#else
        mode.format = static_cast<unsigned int>(D3DFMT_X8R8G8B8);
#endif
        mode.flags &= ~1u;  // 清除 rwVIDEOMODEEXCLUSIVE
    } else if (index < static_cast<int>(g_windowModeVideoModeBackup.size())) {
        mode = g_windowModeVideoModeBackup[index];
    }
}

// 同步游戏自身的呈现参数，窗口模式下写成窗口化参数，
// 游戏或 d3d8to9 包装层按这些参数重建设备时也会得到窗口期交换链
void SyncGamePresentParameters(HWND window) {
    if (!window || !IsWindow(window)) return;
    auto* parameters = reinterpret_cast<GamePresentParameters*>(kGamePresentParametersAddress);

    if (g_windowMode == XBase::Hooks::WindowMode::Fullscreen) {
        if (g_windowModeHasSavedPresentParameters) {
            std::memcpy(parameters, g_windowModeSavedPresentParameters.data(), kGamePresentParametersSize);
        }
        return;
    }

    if (!g_windowModeHasSavedPresentParameters) {
        std::memcpy(g_windowModeSavedPresentParameters.data(), parameters, kGamePresentParametersSize);
        g_windowModeHasSavedPresentParameters = true;
    }

    RECT client{};
    if (GetClientRect(window, &client)) {
        parameters->BackBufferWidth = static_cast<UINT>(client.right - client.left);
        parameters->BackBufferHeight = static_cast<UINT>(client.bottom - client.top);
    }
    parameters->hDeviceWindow = window;
    parameters->Windowed = TRUE;
    parameters->SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters->FullScreen_RefreshRateInHz = 0;
#if defined(GTASA)
    parameters->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
#else
    parameters->FullScreen_PresentationInterval = 0x80000000;  // D3DPRESENT_INTERVAL_DEFAULT
#endif
}

// 制造一次尺寸变化，让游戏走自己的 WM_SIZE 重建设备路径，
// 我们不去直接释放或重置设备，避免默认池资源未释放导致的失败与黑屏
bool TriggerGameResize(HWND window, int width, int height) {
    if (!window || !IsWindow(window) || width <= 0 || height <= 0) return false;
    return SetWindowPos(
        window, nullptr, 0, 0, width, height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
}

// 缩小一像素再恢复，制造两次 WM_SIZE 触发游戏自身的设备重建
void NudgeGameResize() {
    if (!g_window || !IsWindow(g_window)) return;
    RECT windowRect{};
    if (!GetWindowRect(g_window, &windowRect)) return;
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    if (width <= 1 || height <= 1) return;
    TriggerGameResize(g_window, width - 1, height - 1);
    SetWindowPos(
        g_window, nullptr, windowRect.left, windowRect.top, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// 窗口尺寸变化后重建后处理顶点缓冲，并刷新宽屏修正
void RefreshPostEffects() {
    if (!g_window || !IsWindow(g_window)) return;
    RECT client{};
    if (!GetClientRect(g_window, &client)) return;

#if defined(GTASA)
    RwCamera* camera = *reinterpret_cast<RwCamera**>(kSceneCameraAddress);
    RwInt32 originalWidth = 0;
    RwInt32 originalHeight = 0;
    if (camera && camera->frameBuffer) {
        originalWidth = camera->frameBuffer->width;
        originalHeight = camera->frameBuffer->height;
        camera->frameBuffer->width = client.right - client.left;
        camera->frameBuffer->height = client.bottom - client.top;
    }
    plugin::CallDyn(kSetupBackBufferVertexAddress);
    if (camera && camera->frameBuffer) {
        camera->frameBuffer->width = originalWidth;
        camera->frameBuffer->height = originalHeight;
    }
#else
    RwCamera* camera = *reinterpret_cast<RwCamera**>(kSceneCameraAddress);
    if (camera) {
        plugin::CallDyn(kBlurOpenAddress, camera);
    }
#endif

    HMODULE widescreenFix = nullptr;
#if defined(GTASA)
    widescreenFix = GetModuleHandleW(L"GTASA.WidescreenFix.asi");
#elif defined(GTAVC)
    widescreenFix = GetModuleHandleW(L"GTAVC.WidescreenFix.asi");
#else
    widescreenFix = GetModuleHandleW(L"GTA3.WidescreenFix.asi");
#endif
    if (widescreenFix) {
        if (const auto update = reinterpret_cast<void(*)()>(GetProcAddress(widescreenFix, "UpdateVars"))) {
            update();
        }
    }
}

// 窗口模式下游离开客户区会丢失镜头控制，按需裁剪光标
void MaintainCursorClip() {
    if (g_windowMode == XBase::Hooks::WindowMode::Fullscreen || !g_window || !IsWindow(g_window)) {
        if (g_windowModeCursorClipped) {
            ClipCursor(nullptr);
            g_windowModeCursorClipped = false;
        }
        return;
    }

    const bool gameFocused = GetForegroundWindow() == g_window;
    if (!gameFocused || WantsInput()) {
        if (g_windowModeCursorClipped) {
            ClipCursor(nullptr);
            g_windowModeCursorClipped = false;
        }
        return;
    }

    RECT client{};
    if (!GetClientRect(g_window, &client)) return;
    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    if (!ClientToScreen(g_window, &topLeft) || !ClientToScreen(g_window, &bottomRight)) return;
    const RECT clip{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    if (ClipCursor(&clip) || g_windowModeCursorClipped) {
        g_windowModeCursorClipped = true;
    }
}

// 窗口或呈现参数被游戏改回时纠偏，设备重建只可能在启动阶段被游戏自己触发，
// 运行中修改显示模式需要重启游戏才生效。未启用窗口模式时不做任何干预
void MaintainWindowMode() {
    if (!g_window || !IsWindow(g_window)) return;
    if (g_windowMode == XBase::Hooks::WindowMode::Fullscreen && !g_windowModeHasSavedState) return;

    const unsigned long long now = static_cast<unsigned long long>(XBase::Platform::MonotonicMilliseconds());
    if (g_windowModeLastCheckAt != 0 && now - g_windowModeLastCheckAt < 1000) return;
    g_windowModeLastCheckAt = now;

    D3DPRESENT_PARAMETERS parameters{};
    if (!CapturePresentParameters(parameters)) return;

    const bool startupPhase = now < g_windowModeStartupGraceUntil;

    if (g_windowMode == XBase::Hooks::WindowMode::Fullscreen) {
        if (!parameters.Windowed) {
            g_windowModeResizeAttempts = 0;
            return;
        }
        if (!startupPhase) {
            XBase::Log::Warn("Hooks: 恢复独占全屏需要重启游戏才生效");
            g_windowModeStartupGraceUntil = now + 600000;
            return;
        }
        if (now < g_windowModeSettleAt) return;
        if (g_windowModeResizeAttempts >= 3) {
            g_windowModeResizeAttempts = 0;
            XBase::Log::Warn("Hooks: 恢复独占全屏未生效，将在重启游戏后生效");
            LogDisplayDiagnostics("restore-not-applied");
            g_windowModeStartupGraceUntil = now + 600000;
            return;
        }
        ++g_windowModeResizeAttempts;
        NudgeGameResize();
        g_windowModeSettleAt = now + 1500;
        return;
    }

    SIZE clientSize{};
    RECT client{};
    if (GetClientRect(g_window, &client)) {
        clientSize.cx = client.right - client.left;
        clientSize.cy = client.bottom - client.top;
    }
    // 维护阶段只纠正样式，无边框模式额外保证铺满显示器，同时保持游戏显示状态同步
    ApplyWindowModeGeometry(g_window, g_windowMode, clientSize, false);
    SyncGameDisplayState();

    if (parameters.Windowed && parameters.BackBufferWidth == static_cast<UINT>(clientSize.cx)
        && parameters.BackBufferHeight == static_cast<UINT>(clientSize.cy)) {
        g_windowModeResizeAttempts = 0;
        return;
    }

    if (!startupPhase) {
        XBase::Log::Warn("Hooks: 窗口模式需要重启游戏才生效");
        g_windowModeStartupGraceUntil = now + 600000;
        LogDisplayDiagnostics("restart-required");
        return;
    }

    // 请求刚发出时先给游戏自己的重建留时间
    if (now < g_windowModeSettleAt) return;

    if (g_windowModeResizeAttempts >= 3) {
        g_windowModeResizeAttempts = 0;
        XBase::Log::Warn("Hooks: 窗口模式未生效，将在重启游戏后生效");
        LogDisplayDiagnostics("not-applied");
        g_windowModeStartupGraceUntil = now + 600000;
        return;
    }
    ++g_windowModeResizeAttempts;

    // 启动阶段再制造一次尺寸变化，触发游戏自身的 WM_SIZE 重建
    NudgeGameResize();
    ApplyWindowModeGeometry(g_window, g_windowMode, clientSize, true);
    g_windowModeSettleAt = now + 1500;
}

void ApplyGameInputBlock(bool blocked, bool allowReleaseGate = true) {
    if (allowReleaseGate && !blocked && g_gameInputBlocked && AnyMenuKeyDown()) {
        blocked = true;
    }
    if (g_gameInputBlocked != blocked) {
        if (blocked) {
            RememberKeysHeldBeforeBlock();
        } else {
            g_keysHeldBeforeBlock.fill(false);
        }
        g_gameInputBlocked = blocked;
        ClearMouseState();
        if (blocked) {
            for (InputPatchSnapshot& patch : g_inputPatches) CaptureInputPatch(patch);
#if defined(GTASA)
            plugin::patch::SetUChar(0x6194A0, 0xC3);
            plugin::patch::Nop(0x541DD7, 5);
            plugin::patch::SetUChar(0x4EB731, 0xEB);
            plugin::patch::SetUChar(0x4EB75A, 0xEB);
#elif defined(GTAVC)
            plugin::patch::SetUChar(0x6020A0, 0xC3);
            plugin::patch::Nop(0x4AB6CA, 5);
#elif defined(GTA3)
            plugin::patch::SetUChar(0x580D20, 0xC3);
            plugin::patch::Nop(0x49272F, 5);
#endif
        } else {
            for (InputPatchSnapshot& patch : g_inputPatches) RestoreInputPatch(patch);
            // 菜单期间 DirectInput 鼠标增量被拦截后累积在缓冲里，
            // 恢复补丁后先跑一次 UpdatePads 把积压增量读入 CPad 状态，再清零丢弃，
            // 否则下一帧相机会按菜单期间的鼠标移动量转动。
            CPad::UpdatePads();
            ClearMouseState();
        }
    }

    CPad* pad = CPad::GetPad(0);
    if (pad) pad->DisablePlayerControls = blocked;
}

bool WantsInput() {
    return g_menuVisible || g_backgroundInputActive;
}

bool WantsRender() {
    return g_menuVisible || g_backgroundInputActive || g_backgroundRenderActive;
}

void ReleaseCursor() {
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().MouseDrawCursor = false;
    }
}

void SyncCursorPosition() {
    if (!g_window || !ImGui::GetCurrentContext()) return;

    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 1.0f || io.DisplaySize.y <= 1.0f) return;

    POINT cursor{};
    RECT client{};
    if (!GetCursorPos(&cursor) || !ScreenToClient(g_window, &cursor) || !GetClientRect(g_window, &client)) return;

    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    if (width <= 1.0f || height <= 1.0f) return;

    float x = static_cast<float>(cursor.x) * io.DisplaySize.x / width;
    float y = static_cast<float>(cursor.y) * io.DisplaySize.y / height;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x >= io.DisplaySize.x) x = io.DisplaySize.x - 1.0f;
    if (y >= io.DisplaySize.y) y = io.DisplaySize.y - 1.0f;
    io.AddMousePosEvent(x, y);
}

bool IsMouseMessage(UINT message) {
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_INPUT:
        return true;
    default:
        return false;
    }
}

bool IsKeyboardMessage(UINT message) {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
        return true;
    default:
        return false;
    }
}

bool CursorInsideClient(HWND window) {
    POINT cursor{};
    RECT client{};
    if (!GetCursorPos(&cursor) || !GetClientRect(window, &client)) return true;
    return PtInRect(&client, cursor) != FALSE;
}

// 窗口模式下抑制游戏的失焦处理与框外输入，避免自动暂停、最小化与误操作
bool HandleWindowModeMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            result = DefWindowProcW(window, message, wParam, lParam);
            return true;
        }
        return false;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETCURSOR:
        result = DefWindowProcW(window, message, wParam, lParam);
        return true;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) {
            result = 0;
            return true;
        }
        return false;
    case WM_STYLECHANGING:
        if (wParam == GWL_STYLE || wParam == GWL_EXSTYLE) {
            auto* styles = reinterpret_cast<STYLESTRUCT*>(lParam);
            if (styles) {
                if (wParam == GWL_STYLE) {
                    styles->styleNew = static_cast<DWORD>(WindowStyleFor(g_windowMode));
                } else {
                    styles->styleNew = static_cast<DWORD>(WindowExStyleFor(g_windowMode));
                }
            }
            result = 0;
            return true;
        }
        return false;
    case WM_SIZE:
        // 最小化时游戏会按 0 尺寸更新呈现参数，这里拦下
        if (wParam == SIZE_MINIMIZED) {
            result = DefWindowProcW(window, message, wParam, lParam);
            return true;
        }
        return false;
    default:
        return false;
    }
}

LRESULT __stdcall WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    RenderCallbackScope callbackScope;
    if (!callbackScope) {
        return g_originalWndProc
            ? CallWindowProc(g_originalWndProc, window, message, wParam, lParam)
            : DefWindowProc(window, message, wParam, lParam);
    }

    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        const bool repeat = (lParam & (1LL << 30)) != 0;
        XBase::Detail::Input::HandleVirtualKey(static_cast<std::uint32_t>(wParam), true, repeat);
    } else if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        XBase::Detail::Input::HandleVirtualKey(static_cast<std::uint32_t>(wParam), false, false);
    }

    if (g_middleSuppressed.load(std::memory_order_acquire)
        && (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP
            || message == WM_MBUTTONDBLCLK)) {
        return 1;
    }

    if (WantsInput() && ImGui::GetCurrentContext()) {
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
        const ImGuiIO& io = ImGui::GetIO();
        if ((IsMouseMessage(message) && io.WantCaptureMouse)
            || (IsKeyboardMessage(message) && (io.WantCaptureKeyboard || io.WantTextInput))) {
            return 1;
        }
    } else if (message == WM_MOUSEWHEEL) {
        g_wheelDelta += static_cast<float>(static_cast<short>(HIWORD(wParam))) / static_cast<float>(WHEEL_DELTA);
        if (g_wheelSuppressed.load(std::memory_order_acquire)) {
            return 0;
        }
#if defined(GTASA)
    } else if (message == WM_INPUT) {
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == sizeof(input)
            && input.header.dwType == RIM_TYPEMOUSE
            && (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
            g_wheelDelta += static_cast<float>(static_cast<short>(input.data.mouse.usButtonData)) / static_cast<float>(WHEEL_DELTA);
            if (g_wheelSuppressed.load(std::memory_order_acquire)) {
                return 0;
            }
        }
#endif
    }

    if (g_windowMode != XBase::Hooks::WindowMode::Fullscreen) {
        LRESULT handled = 0;
        if (HandleWindowModeMessage(window, message, wParam, lParam, handled)) {
            return handled;
        }
        if (IsKeyboardMessage(message) || IsMouseMessage(message)) {
            if (GetForegroundWindow() != window || !CursorInsideClient(window)) {
                return window == nullptr ? 0 : DefWindowProcW(window, message, wParam, lParam);
            }
        }
    }

    return g_originalWndProc
        ? CallWindowProc(g_originalWndProc, window, message, wParam, lParam)
        : DefWindowProc(window, message, wParam, lParam);
}

bool InitRenderRuntime(LPDIRECT3DDEVICE9 device) {
    D3DDEVICE_CREATION_PARAMETERS parameters{};
    if (FAILED(device->GetCreationParameters(&parameters)) || !parameters.hFocusWindow || !IsWindow(parameters.hFocusWindow)) {
        g_state = XBase::Hooks::RuntimeState::Failed;
        g_statusText = "D3D9 hook failed: invalid game window";
        XBase::Log::Error("Hooks: invalid D3D9 focus window");
        return false;
    }

    g_window = parameters.hFocusWindow;
    g_device = device;
    // 启动阶段允许用尺寸变化促成游戏自己的设备重建，之后不再尝试
    g_windowModeStartupGraceUntil = XBase::Platform::MonotonicMilliseconds() + 30000;
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    if (!g_originalWndProc) {
        g_state = XBase::Hooks::RuntimeState::Failed;
        g_statusText = "D3D9 hook failed: WndProc installation failed";
        XBase::Log::Error("Hooks: WndProc installation failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    const bool win32Initialized = ImGui_ImplWin32_Init(g_window);
    const bool dx9Initialized = win32Initialized && ImGui_ImplDX9_Init(device);
    if (!win32Initialized || !dx9Initialized) {
        if (dx9Initialized) ImGui_ImplDX9_Shutdown();
        if (win32Initialized) ImGui_ImplWin32_Shutdown();
        SetWindowLongPtr(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        g_originalWndProc = nullptr;
        g_window = nullptr;
        ImGui::DestroyContext();
        g_state = XBase::Hooks::RuntimeState::Failed;
        g_statusText = "D3D9 hook failed: ImGui backend initialization failed";
        XBase::Log::Error("Hooks: ImGui backend initialization failed");
        return false;
    }

    ImGui::StyleColorsDark();
    XBase::Theme::Init();
    XBase::Detail::RenderFonts::LoadDefault();
    g_state = XBase::Hooks::RuntimeState::RenderReady;
    g_statusText = "D3D9 render ready";
    XBase::Log::Info("Hooks: D3D9 and ImGui render runtime ready");
    return true;
}

HRESULT __stdcall EndScene(LPDIRECT3DDEVICE9 device) {
    RenderCallbackScope callbackScope;
    if (!callbackScope) {
        return g_originalEndScene ? g_originalEndScene(device) : S_OK;
    }
    if (g_state == XBase::Hooks::RuntimeState::Hooked && !InitRenderRuntime(device)) {
        return g_originalEndScene ? g_originalEndScene(device) : S_OK;
    }

    if (g_state == XBase::Hooks::RuntimeState::RenderReady && WantsRender()) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (WantsInput()) SyncCursorPosition();
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = WantsInput();

        for (const auto& drawCallback : SnapshotDrawCallbacks()) {
            if (drawCallback) drawCallback();
        }

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    return g_originalEndScene ? g_originalEndScene(device) : S_OK;
}

HRESULT __stdcall Reset(LPDIRECT3DDEVICE9 device, D3DPRESENT_PARAMETERS* parameters) {
    RenderCallbackScope callbackScope;
    if (!callbackScope) {
        return g_originalReset ? g_originalReset(device, parameters) : D3DERR_INVALIDCALL;
    }
    if (!g_originalReset) return D3DERR_INVALIDCALL;
    if (g_state != XBase::Hooks::RuntimeState::RenderReady) {
        return g_originalReset(device, parameters);
    }

    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (g_windowMode != XBase::Hooks::WindowMode::Fullscreen && parameters) {
        // 窗口模式要求交换链始终以窗口模式呈现，尺寸跟随当前客户区
        RECT client{};
        if (g_window && IsWindow(g_window) && GetClientRect(g_window, &client)) {
            parameters->BackBufferWidth = static_cast<UINT>(client.right - client.left);
            parameters->BackBufferHeight = static_cast<UINT>(client.bottom - client.top);
        }
        parameters->Windowed = TRUE;
        parameters->hDeviceWindow = g_window;
        parameters->SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters->FullScreen_RefreshRateInHz = 0;
    }
    const HRESULT result = g_originalReset(device, parameters);
    if (SUCCEEDED(result)) {
        g_device = device;
        ImGui_ImplDX9_CreateDeviceObjects();
        if (g_windowMode != XBase::Hooks::WindowMode::Fullscreen) {
            SyncGameDisplayState();
            SyncGamePresentParameters(g_window);
            RefreshPostEffects();
        }
    }
    return result;
}

} // namespace
#endif

namespace XBase::Hooks {

// 延迟的窗口模式请求在安全点落地，避免在渲染回调内 Reset 设备
bool Init() {
#if defined(XBASE_WITH_KIERO)
    if (g_state == RuntimeState::Hooked || g_state == RuntimeState::RenderReady) return true;
    if (g_state == RuntimeState::ShuttingDown) return false;

    g_shutdownRequested.store(false, std::memory_order_release);
    XBase::Detail::Input::Reset();
    ConfigureInputPatches();
    g_state = RuntimeState::Uninitialized;
    g_statusText = "initializing";
    if (kiero::init(kiero::RenderType::D3D9) != kiero::Status::Success) {
        g_state = RuntimeState::Failed;
        g_statusText = "D3D9 hook initialization failed";
        Log::Error("Hooks: kiero initialization failed");
        return false;
    }

    if (kiero::bind(42, reinterpret_cast<void**>(&g_originalEndScene), reinterpret_cast<void*>(EndScene)) != kiero::Status::Success) {
        kiero::shutdown();
        g_state = RuntimeState::Failed;
        g_statusText = "D3D9 hook failed: EndScene binding failed";
        Log::Error("Hooks: EndScene binding failed");
        return false;
    }

    if (kiero::bind(16, reinterpret_cast<void**>(&g_originalReset), reinterpret_cast<void*>(Reset)) != kiero::Status::Success) {
        kiero::shutdown();
        g_originalEndScene = nullptr;
        g_state = RuntimeState::Failed;
        g_statusText = "D3D9 hook failed: Reset binding failed";
        Log::Error("Hooks: Reset binding failed");
        return false;
    }

    if (!InstallMouseInputHook()) {
        kiero::shutdown();
        g_originalEndScene = nullptr;
        g_originalReset = nullptr;
        g_state = RuntimeState::Failed;
        g_statusText = "mouse input hook failed";
        Log::Error("Hooks: mouse input hook failed");
        return false;
    }

    g_state = RuntimeState::Hooked;
    g_statusText = "D3D9 hook installed";
    Log::Info("Hooks: D3D9 hooks installed");
    return true;
#else
    return false;
#endif
}

void Shutdown() {
#if defined(XBASE_WITH_KIERO)
    if (g_state == RuntimeState::Uninitialized || g_state == RuntimeState::ShuttingDown) return;

    if (g_windowModeHasSavedState && g_window) {
        // 恢复窗口样式，交换链在进程结束时随游戏一起释放
        RestoreSavedWindowStyle(g_window);
        g_windowMode = XBase::Hooks::WindowMode::Fullscreen;
    }
    ClipCursor(nullptr);
    g_windowModeCursorClipped = false;

    const RuntimeState previousState = g_state;
    g_state = RuntimeState::ShuttingDown;
    g_shutdownRequested.store(true, std::memory_order_release);
    g_menuVisible = false;
    g_backgroundInputActive = false;
    g_backgroundRenderActive = false;
    ApplyGameInputBlock(false, false);

    if (g_window && g_originalWndProc) {
        SetWindowLongPtr(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    }
    kiero::shutdown();
    WaitForRenderCallbacks();
    if (g_updateMouseTarget) {
        MH_RemoveHook(g_updateMouseTarget);
        g_updateMouseTarget = nullptr;
        g_originalUpdateMouse = nullptr;
    }

    if (previousState == RuntimeState::RenderReady) {
        ReleaseCursor();
        XBase::Theme::Shutdown();
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    {
        std::lock_guard<std::mutex> lock(g_drawCallbacksMutex);
        g_drawCallbacks.clear();
        g_drawCallbacksSnapshotDirty = true;
    }
    g_originalEndScene = nullptr;
    g_originalReset = nullptr;
    g_originalWndProc = nullptr;
    g_window = nullptr;
    g_device = nullptr;
    g_wheelDelta = 0.0f;
    g_wheelSuppressed.store(false, std::memory_order_release);
    g_middleSuppressed.store(false, std::memory_order_release);
    XBase::Detail::Input::Reset();
    g_statusText = "not initialized";
    g_state = RuntimeState::Uninitialized;
#endif
}

RuntimeState GetState() {
#if defined(XBASE_WITH_KIERO)
    return g_state;
#else
    return RuntimeState::Uninitialized;
#endif
}

bool IsInitialized() {
    const RuntimeState state = GetState();
    return state == RuntimeState::Hooked || state == RuntimeState::RenderReady;
}

bool IsReady() {
    return GetState() == RuntimeState::RenderReady;
}

bool HadInitFailure() {
    return GetState() == RuntimeState::Failed;
}

const char* GetStatusText() {
#if defined(XBASE_WITH_KIERO)
    return g_statusText;
#else
    return "hooks unavailable";
#endif
}

DrawCallbackId RegisterDrawCallback(std::function<void()> callback) {
#if defined(XBASE_WITH_KIERO)
    if (!callback) return {};

    std::lock_guard<std::mutex> lock(g_drawCallbacksMutex);
    const DrawCallbackId callbackId{g_nextDrawCallbackId++};
    g_drawCallbacks.push_back({callbackId, std::move(callback)});
    g_drawCallbacksSnapshotDirty = true;
    return callbackId;
#else
    (void)callback;
    return {};
#endif
}

bool UnregisterDrawCallback(DrawCallbackId callbackId) {
#if defined(XBASE_WITH_KIERO)
    if (!callbackId) return false;

    std::lock_guard<std::mutex> lock(g_drawCallbacksMutex);
    const auto callback = std::find_if(
        g_drawCallbacks.begin(),
        g_drawCallbacks.end(),
        [callbackId](const DrawCallbackEntry& entry) {
            return entry.id.value == callbackId.value;
        });
    if (callback == g_drawCallbacks.end()) return false;
    g_drawCallbacks.erase(callback);
    g_drawCallbacksSnapshotDirty = true;
    return true;
#else
    (void)callbackId;
    return false;
#endif
}

void SetMenuVisible(bool visible) {
#if defined(XBASE_WITH_KIERO)
    if (!IsInitialized()) visible = false;
    g_menuVisible = visible;
    if (WantsInput()) {
        ApplyGameInputBlock(true);
    } else {
        // 关闭菜单时必须立即恢复游戏输入。跨模块切换或网页宿主失焦时，
        // 关闭瞬间按键仍可能保持按下；释放保护不能把 DisablePlayerControls
        // 或 DirectInput 补丁留在锁定状态，否则游戏鼠标会一直失灵。
        ApplyGameInputBlock(false, false);
        ReleaseCursor();
    }
#else
    (void)visible;
#endif
}

bool IsMenuVisible() {
#if defined(XBASE_WITH_KIERO)
    return g_menuVisible;
#else
    return false;
#endif
}

void ToggleMenu() {
    SetMenuVisible(!IsMenuVisible());
}

void SetBackgroundInputActive(bool active) {
#if defined(XBASE_WITH_KIERO)
    g_backgroundInputActive = active;
    if (WantsInput()) {
        ApplyGameInputBlock(true);
    } else {
        ApplyGameInputBlock(false, false);
        ReleaseCursor();
    }
#else
    (void)active;
#endif
}

bool IsBackgroundInputActive() {
#if defined(XBASE_WITH_KIERO)
    return g_backgroundInputActive;
#else
    return false;
#endif
}

void SetBackgroundRenderActive(bool active) {
#if defined(XBASE_WITH_KIERO)
    g_backgroundRenderActive = active;
#else
    (void)active;
#endif
}

bool IsBackgroundRenderActive() {
#if defined(XBASE_WITH_KIERO)
    return g_backgroundRenderActive;
#else
    return false;
#endif
}

void MaintainInputState() {
#if defined(XBASE_WITH_KIERO)
    ApplyGameInputBlock(WantsInput());
    MaintainWindowMode();
    MaintainCursorClip();
    if (g_state != RuntimeState::RenderReady || !ImGui::GetCurrentContext()) return;
    ImGui::GetIO().MouseDrawCursor = WantsInput();
#endif
}

float GetFrameDeltaSeconds() {
#if defined(XBASE_WITH_KIERO)
    if (g_state == RuntimeState::RenderReady && ImGui::GetCurrentContext()) {
        return ImGui::GetIO().DeltaTime;
    }
#endif
    return 1.0f / 60.0f;
}

bool IsKeyboardCaptureActive() {
#if defined(XBASE_WITH_KIERO)
    if (g_state == RuntimeState::RenderReady && ImGui::GetCurrentContext()) {
        const ImGuiIO& io = ImGui::GetIO();
        return io.WantTextInput || io.WantCaptureKeyboard;
    }
#endif
    return false;
}

float ConsumeWheelDelta() {
#if defined(XBASE_WITH_KIERO)
    const float value = g_wheelDelta;
    g_wheelDelta = 0.0f;
    return value;
#else
    return 0.0f;
#endif
}

void SetWheelInputSuppressed(bool suppressed) {
    g_wheelSuppressed.store(suppressed, std::memory_order_release);
}

void SetMiddleInputSuppressed(bool suppressed) {
    g_middleSuppressed.store(suppressed, std::memory_order_release);
}

bool IsMiddleInputSuppressed() {
    return g_middleSuppressed.load(std::memory_order_acquire);
}

bool IsWheelInputSuppressed() {
    return g_wheelSuppressed.load(std::memory_order_acquire);
}

bool IsGameWindowFullscreen() {
#if defined(XBASE_WITH_KIERO)
    if (!g_window || !IsWindow(g_window)) return false;

    const LONG style = GetWindowLongW(g_window, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;

    RECT windowRect{};
    if (!GetWindowRect(g_window, &windowRect)) return false;
    HMONITOR monitor = MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;

    return windowRect.left <= info.rcMonitor.left
        && windowRect.top <= info.rcMonitor.top
        && windowRect.right >= info.rcMonitor.right
        && windowRect.bottom >= info.rcMonitor.bottom;
#else
    return false;
#endif
}

bool IsWindowModeSupported() {
#if defined(XBASE_WITH_KIERO)
    return g_state == RuntimeState::RenderReady && g_device != nullptr && g_window != nullptr;
#else
    return false;
#endif
}

XBase::Hooks::WindowMode GetWindowMode() {
#if defined(XBASE_WITH_KIERO)
    return g_windowMode;
#else
    return XBase::Hooks::WindowMode::Fullscreen;
#endif
}

// 显示模式只能在游戏创建设备时确定，运行中调用只记录请求，重启游戏后才生效
bool SetWindowMode(XBase::Hooks::WindowMode mode) {
#if defined(XBASE_WITH_KIERO)
    if (mode == GetWindowMode()) return true;
    XBase::Log::Info(mode == XBase::Hooks::WindowMode::Fullscreen
        ? "Hooks: 已保存独占全屏设置，重启游戏后生效"
        : "Hooks: 已保存窗口模式设置，重启游戏后生效");
    return true;
#else
    (void)mode;
    return false;
#endif
}

bool PrepareStartupWindowMode(XBase::Hooks::WindowMode mode) {
    return PrepareStartupWindowModeInternal(mode);
}

} // namespace XBase::Hooks

namespace XBase::Detail::Hooks {

HWND GetGameWindow() {
#if defined(XBASE_WITH_KIERO)
    return g_window;
#else
    return nullptr;
#endif
}

IDirect3DDevice9* GetD3D9Device() {
    return g_device;
}

void GetDisplaySize(float& width, float& height) {
    width = 0.0f;
    height = 0.0f;
#if defined(XBASE_WITH_KIERO)
    if (g_state == XBase::Hooks::RuntimeState::RenderReady && ImGui::GetCurrentContext()) {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        width = size.x;
        height = size.y;
    }
#endif
}

} // namespace XBase::Detail::Hooks
