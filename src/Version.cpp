#include <XBase/Version.h>

namespace XBase {
namespace {
constexpr char kVersionString[] = "v0.1.0-rc";
constexpr VersionTriple kVersion{0, 1, 0};

std::uint32_t EncodeVersion(VersionTriple version) {
    return (static_cast<std::uint32_t>(version.major) << 16)
        | (static_cast<std::uint32_t>(version.minor) << 8)
        | static_cast<std::uint32_t>(version.patch);
}
}

const char* GetVersionString() {
    return kVersionString;
}

VersionTriple GetVersion() {
    return kVersion;
}

std::uint32_t GetVersionNumber() {
    return EncodeVersion(kVersion);
}

bool AtLeast(int major, int minor, int patch) {
    return GetVersionNumber() >= EncodeVersion({major, minor, patch});
}

GameVersion GetGameVersion() {
#if defined(GTASA)
    return GameVersion::SA;
#elif defined(GTAVC)
    return GameVersion::VC;
#else
    return GameVersion::III;
#endif
}

bool IsSA() { return GetGameVersion() == GameVersion::SA; }
bool IsVC() { return GetGameVersion() == GameVersion::VC; }
bool IsIII() { return GetGameVersion() == GameVersion::III; }

std::string GetVersionName() {
    switch (GetGameVersion()) {
    case GameVersion::SA: return "SA";
    case GameVersion::VC: return "VC";
    default: return "III";
    }
}
} // namespace XBase
