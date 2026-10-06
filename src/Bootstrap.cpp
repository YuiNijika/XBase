#include "Bootstrap.h"
#include <XBase/Package.h>
#include <XBase/Version.h>

#include <cstdio>
#include <string>
#include <string>
#include <windows.h>

namespace XBase::Bootstrap {
namespace {

HMODULE runtimeModule = nullptr;
const XBaseRuntime* runtimeTable = nullptr;

enum class DetectedGame {
    Unknown,
    SanAndreas,
    ViceCity,
    III,
};

// 共享运行时按游戏版本分成不同文件，和 payload 一样放在 XBase 目录下的 Library 子目录
const char* RuntimeFileName(DetectedGame game) {
    switch (game) {
    case DetectedGame::SanAndreas: return "XBaseSA.dll";
    case DetectedGame::ViceCity:   return "XBaseVC.dll";
    case DetectedGame::III:        return "XBaseIII.dll";
    case DetectedGame::Unknown:
    default:                       return nullptr;
    }
}

HMODULE payloadModule = nullptr;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

bool ReadUInt(std::uintptr_t address, unsigned int& value) {
    __try {
        value = *reinterpret_cast<const unsigned int*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

bool MatchUInt(std::uintptr_t address, unsigned int expected) {
    unsigned int value = 0;
    return ReadUInt(address, value) && value == expected;
}

DetectedGame DetectGame() {
    if (MatchUInt(0x401000, 0x53EC8B55) || MatchUInt(0x401000, 0x16197BE9)
        || MatchUInt(0x8245BC, 0x94BF) || MatchUInt(0x8252FC, 0x94BF)
        || MatchUInt(0x82533C, 0x94BF) || MatchUInt(0x858D51, 0x3539F633)
        || MatchUInt(0x858C61, 0x3539F633)) {
        return DetectedGame::SanAndreas;
    }

    if (MatchUInt(0x667BF0, 0x53E58955) || MatchUInt(0x667C40, 0x53E58955)
        || MatchUInt(0x666BA0, 0x53E58955)) {
        return DetectedGame::ViceCity;
    }

    if (MatchUInt(0x5C1E70, 0x53E58955) || MatchUInt(0x5C2130, 0x53E58955)
        || MatchUInt(0x5C6FD0, 0x53E58955)) {
        return DetectedGame::III;
    }

    return DetectedGame::Unknown;
}

// 载荷目录与文件名优先取宿主 ASI 导出的基名，未导出时按 ASI 文件名推导。
// XMenu 与 WebView2 都导出基名，三个游戏版本因此共用同一个目录与同一份清单，
// asi 文件名带的 SA / VC / III 后缀不会进到模组名里
using PayloadBaseNameFn = const char*(*)();

std::string ExportedHostName(HMODULE loaderModule) {
    const auto exported = reinterpret_cast<PayloadBaseNameFn>(
        GetProcAddress(loaderModule, "XBasePayloadBaseName"));
    if (!exported) return {};
    const char* name = exported();
    return name && name[0] ? std::string(name) : std::string();
}

bool EndsWithToken(const std::string& value, const char* suffix) {
    const std::string token(suffix);
    if (token.empty() || value.size() <= token.size()) return false;
    return value.compare(value.size() - token.size(), token.size(), token) == 0;
}

// 载荷文件名是基名 + 游戏后缀，按 asi 文件名推导基名时必须先把后缀剥掉，
// 否则 XMenuVC.asi 推成 XMenuVC，载荷会去找 XMenuVCVC.dll、数据目录也变成 XMenuVC。
// 剥完为空说明这个名字本来就只有后缀，保留原样交给后面的兜底处理。
std::string StripGameSuffix(const std::string& name) {
    static const char* const suffixes[] = { "III", "SA", "VC" };
    for (const char* suffix : suffixes) {
        if (EndsWithToken(name, suffix)) {
            const std::string stripped = name.substr(0, name.size() - std::string(suffix).size());
            return stripped.empty() ? name : stripped;
        }
    }
    return name;
}

std::string HostNameFromModule(HMODULE loaderModule) {
    const std::string exported = ExportedHostName(loaderModule);
    if (!exported.empty()) return exported;

    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(loaderModule, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0) return "XMenu";
        if (size < path.size() - 1) {
            path.resize(size);
            break;
        }
        path.resize(path.size() * 2);
    }

    const std::string utf8 = WideToUtf8(path);
    const std::size_t slash = utf8.find_last_of("\\/");
    std::string name = slash == std::string::npos ? utf8 : utf8.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name.resize(dot);
    }
    if (name.empty()) return std::string("XMenu");
    return StripGameSuffix(name);
}

std::string PayloadFileName(DetectedGame game, const std::string& hostName) {
    switch (game) {
    case DetectedGame::SanAndreas:
        return hostName + "SA.dll";
    case DetectedGame::ViceCity:
        return hostName + "VC.dll";
    case DetectedGame::III:
        return hostName + "III.dll";
    case DetectedGame::Unknown:
    default:
        return {};
    }
}

// 单文件 asi 通过导出 XBaseModTargetGame 声明自己只跑在哪个游戏版本上，
// 不匹配时 Bootstrap 静默跳过，避免多 asi 同目录时非本游戏的 asi 误初始化或弹错误框。
std::string ModTargetGame(HMODULE loaderModule) {
    using Fn = const char*(*)();
    const auto fn = reinterpret_cast<Fn>(GetProcAddress(loaderModule, "XBaseModTargetGame"));
    if (!fn) return {};
    const char* game = fn();
    return game ? std::string(game) : std::string();
}

std::string DirectoryFromModule(HMODULE module) {
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

    const std::string utf8 = WideToUtf8(path);
    const std::size_t slash = utf8.find_last_of("\\/");
    if (slash == std::string::npos) return {};
    return utf8.substr(0, slash + 1);
}

bool FileExists(const std::string& path) {
    const DWORD attributes = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// 载荷属于模组自己，和它的数据放在 XBase 下以模组名命名的子目录，公用库在 Library 所以不在这里找
// asi 同级目录仍作为兼容路径
std::string PayloadPath(HMODULE loaderModule, DetectedGame game) {
    const std::string hostName = HostNameFromModule(loaderModule);
    const std::string fileName = PayloadFileName(game, hostName);
    if (fileName.empty()) return {};

    const std::string gameRoot = DirectoryFromModule(nullptr);
    if (!gameRoot.empty()) {
        const std::string shared = gameRoot + "XBase\\Mods\\" + hostName + "\\" + fileName;
        if (FileExists(shared)) return shared;
    }

    const std::string directory = DirectoryFromModule(loaderModule);
    if (directory.empty()) return hostName + "\\" + fileName;
    return directory + hostName + "\\" + fileName;
}

void ShowError(const std::string& hostName, const char* message) {
    MessageBoxW(HWND_DESKTOP, Utf8ToWide(message).c_str(), Utf8ToWide(hostName).c_str(), MB_OK | MB_ICONERROR);
}

// 共享运行时必须先于任何业务代码就位，mod 拿到的函数表全部指向它
bool EnsureRuntime(HMODULE module, DetectedGame game, const std::string& hostName) {
    if (runtimeTable) return true;

    const char* fileName = RuntimeFileName(game);
    if (!fileName) return false;

    if (!runtimeModule) {
        // 进程里已有实例就直接复用，多个 mod 共用同一份
        runtimeModule = GetModuleHandleW(Utf8ToWide(fileName).c_str());
    }
    if (!runtimeModule) {
        const std::string gameRoot = DirectoryFromModule(nullptr);
        // 共享库统一放在 XBase 目录下的 Library 子目录，旧安装可能直接放在 XBase 根目录，所以再退回旧位置试一次
        const std::string libraryPath = gameRoot + "XBase\\Library\\" + fileName;
        if (FileExists(libraryPath)) {
            runtimeModule = LoadLibraryW(Utf8ToWide(libraryPath).c_str());
        } else {
            const std::string legacyPath = gameRoot + "XBase\\" + fileName;
            if (FileExists(legacyPath)) {
                runtimeModule = LoadLibraryW(Utf8ToWide(legacyPath).c_str());
            }
        }
    }

    if (!runtimeModule) {
        ShowError(hostName, (std::string(
            "Failed to load the XBase shared runtime.\n\nExpected file:\n<game folder>\\XBase\\Library\\")
            + fileName + "\n\nReinstall XBase to restore it.").c_str());
        return false;
    }

    const auto getRuntime = reinterpret_cast<XBaseGetRuntimeFn>(
        GetProcAddress(runtimeModule, XBASE_GET_RUNTIME_NAME));
    if (!getRuntime) {
        ShowError(hostName, "The XBase shared runtime does not export xbaseGetRuntime.\n\n"
                            "The file is probably from an older XBase version.");
        return false;
    }

    const XBaseRuntime* table = getRuntime(XBASE_ABI_VERSION);
    // 共享库只追加字段，拿到比自己头文件更长的表是正常情况，新字段用 size 判断；
    // 共享库的 ABI 比本 mod 的头文件更新也可以用，只有更旧才真的缺能力
    if (!table || table->size < sizeof(XBaseRuntime) || table->abiVersion < XBASE_ABI_VERSION) {
        const std::string message = std::string(
            "The XBase shared runtime was built for a different ABI version.\n\n"
            "This mod requires XBase ")
            + XBase::kVersionString + " or newer. Rebuild the mod and XBase together.";
        ShowError(hostName, message.c_str());
        return false;
    }

    runtimeTable = table;
    if (!runtimeTable->acquire || runtimeTable->acquire(hostName.c_str()) == 0) {
        runtimeTable = nullptr;
        ShowError(hostName, "Failed to initialize the XBase shared runtime.");
        return false;
    }
    return true;
}

} // namespace

bool Attach(ModuleHandle loaderModule) {
    if (payloadModule) return true;

    const HMODULE module = reinterpret_cast<HMODULE>(loaderModule);
    const std::string hostName = HostNameFromModule(module);
    const DetectedGame game = DetectGame();

    // 声明了目标游戏就只在匹配时初始化，否则静默跳过，避免同目录里其他游戏的 asi 弹错误框
    const std::string modTarget = ModTargetGame(module);
    if (!modTarget.empty()) {
        const bool match =
            (modTarget == "SA"  && game == DetectedGame::SanAndreas) ||
            (modTarget == "VC"  && game == DetectedGame::ViceCity)   ||
            (modTarget == "III" && game == DetectedGame::III);
        if (!match) return false;
    }

    if (game == DetectedGame::Unknown) {
        ShowError(
            hostName,
            "Failed to detect supported GTA runtime.\n\n"
            "Supported games: GTA SA, GTA Vice City, GTA III.");
        return false;
    }

    // mod 清单可以声明 engines.xbase 约束，不满足就拒绝挂载并弹出原因，
    // 校验放在运行库加载之前，约束不满足就不必把共享库拉起来
    std::string packageFailure;
    if (!XBase::Package::Validate(hostName, packageFailure)) {
        ShowError(hostName, packageFailure.c_str());
        return false;
    }

    if (!EnsureRuntime(module, game, hostName)) return false;

    // 入口就在本模块时不需要 payload，这是单文件 asi 形态
    if (GetProcAddress(module, "XBasePayloadAttach")) return true;

    const std::string payloadPath = PayloadPath(module, game);
    payloadModule = LoadLibraryW(Utf8ToWide(payloadPath).c_str());
    if (payloadModule) return true;

    const DWORD errorCode = GetLastError();
    char message[768]{};
    std::snprintf(
        message,
        sizeof(message),
        "Failed to load the plugin payload.\n\nExpected file:\n%s\n\nError code: %lu",
        payloadPath.c_str(),
        static_cast<unsigned long>(errorCode));
    ShowError(hostName, message);
    return false;
}

void Detach() {
    if (!payloadModule) return;
    FreeLibrary(payloadModule);
    payloadModule = nullptr;
}

bool AttachRuntime(ModuleHandle loaderModule) {
    const HMODULE module = reinterpret_cast<HMODULE>(loaderModule);
    const std::string hostName = HostNameFromModule(module);
    const DetectedGame game = DetectGame();
    if (game == DetectedGame::Unknown) {
        ShowError(
            hostName,
            "Failed to detect supported GTA runtime.\n\n"
            "Supported games: GTA SA, GTA Vice City, GTA III.");
        return false;
    }
    return EnsureRuntime(module, game, hostName);
}

const XBaseRuntime* GetRuntimeTable() {
    return runtimeTable;
}

bool IsAttached() {
    return payloadModule != nullptr;
}

} // namespace XBase::Bootstrap
