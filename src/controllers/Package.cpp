#include <XBase/Package.h>

#include <XBase/Json.h>
#include <XBase/Platform.h>
#include <XBase/Version.h>

#include <cctype>

namespace XBase::Package {
namespace {

std::string Trim(const std::string& value) {
    std::size_t start = 0;
    std::size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

// 与 Version.h 的 kVersionNumber 同一编码
std::uint32_t EncodeNumber(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) {
    return (major << 16) | (minor << 8) | patch;
}

constexpr std::uint32_t kNumberInfinity = 0xFFFFFFFFu;

// 半版本号：1 | 1.2 | 1.2.3 | 1.x | 1.2.x | * ，前缀 v 忽略，预发布后缀不影响比较。
// 通配位记录在 minorWildcard / patchWildcard 上，比较时折成区间边界
struct PartialVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    bool majorWildcard = false;
    bool minorWildcard = false;
    bool patchWildcard = false;
    bool any = false;  // 单个 * 或空，任意版本
};

bool ParseComponent(const std::string& text, std::size_t& position, std::uint32_t& number, bool& wildcard) {
    wildcard = false;
    if (position >= text.size()) return false;
    if (text[position] == '*') {
        wildcard = true;
        ++position;
        number = 0;
        return true;
    }
    // x 与 X 也是通配写法
    if (text[position] == 'x' || text[position] == 'X') {
        wildcard = true;
        ++position;
        number = 0;
        return true;
    }
    if (!std::isdigit(static_cast<unsigned char>(text[position]))) return false;
    std::uint32_t parsed = 0;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
        parsed = parsed * 10 + static_cast<std::uint32_t>(text[position] - '0');
        ++position;
    }
    number = parsed;
    return true;
}

bool ParsePartial(const std::string& text, PartialVersion& out) {
    std::string value = Trim(text);
    if (!value.empty() && (value[0] == 'v' || value[0] == 'V')) {
        value.erase(0, 1);
    }
    const std::size_t dash = value.find('-');
    if (dash != std::string::npos) {
        value = value.substr(0, dash);
    }
    if (value.empty()) return false;

    if (value == "*") {
        out = PartialVersion{};
        out.any = true;
        return true;
    }

    PartialVersion parsed;
    std::size_t position = 0;

    bool majorWildcard = false;
    if (!ParseComponent(value, position, parsed.major, majorWildcard)) return false;
    if (majorWildcard) {
        parsed.majorWildcard = true;
        out = parsed;
        return true;
    }

    if (position < value.size()) {
        if (value[position] != '.') return false;
        ++position;
        if (!ParseComponent(value, position, parsed.minor, parsed.minorWildcard)) return false;
    } else {
        parsed.minorWildcard = true;
    }

    if (position < value.size()) {
        if (value[position] != '.') return false;
        ++position;
        if (!ParseComponent(value, position, parsed.patch, parsed.patchWildcard)) return false;
    } else {
        parsed.patchWildcard = true;
    }

    if (position != value.size()) return false;
    out = parsed;
    return true;
}

// 区间统一表达成 [lower, upper)，lower 是否含端点单独记录
struct VersionRange {
    std::uint32_t lower = 0;
    std::uint32_t upper = kNumberInfinity;
    bool includeLower = true;
};

// 把半版本号折成该比较符对应的区间边界
bool BuildRange(const std::string& op, const PartialVersion& version, VersionRange& out) {
    if (version.any) {
        out = VersionRange{};
        return true;
    }

    // 下界：通配位补零
    const std::uint32_t lower = EncodeNumber(version.major, version.minor, version.patch);

    // 上界：最后一个非通配分量加一；全通配视为无上界
    std::uint32_t upper = kNumberInfinity;
    if (!version.majorWildcard) {
        if (version.minorWildcard) {
            upper = EncodeNumber(version.major + 1u, 0, 0);              // ^1.x
        } else if (version.patchWildcard) {
            upper = EncodeNumber(version.major, version.minor + 1u, 0);  // ^1.2.x
        } else if (version.major != 0) {
            upper = EncodeNumber(version.major + 1u, 0, 0);              // ^1.2.3
        } else if (version.minor != 0) {
            upper = EncodeNumber(0, version.minor + 1u, 0);              // ^0.2.3
        } else {
            upper = EncodeNumber(0, 0, version.patch + 1u);              // ^0.0.3
        }
    }

    if (op.empty() || op == "=" || op == "==") {
        // 精确匹配：通配位折成区间，否则退化为等值
        if (upper == kNumberInfinity) {
            out.lower = lower;
            out.upper = lower + 1;
            out.includeLower = true;
            return true;
        }
        out.lower = lower;
        out.upper = upper;
        out.includeLower = true;
        return true;
    }
    if (op == ">=") {
        out.lower = lower;
        out.includeLower = true;
        return true;
    }
    if (op == ">") {
        out.lower = lower;
        out.includeLower = false;
        return true;
    }
    if (op == "<=") {
        out.upper = upper == kNumberInfinity ? kNumberInfinity : lower;
        return true;
    }
    if (op == "<") {
        out.upper = lower;
        return true;
    }
    if (op == "^") {
        // 与 npm 一致，不改变最左非零分量
        out.lower = lower;
        out.upper = upper;
        return true;
    }
    if (op == "~") {
        // 只允许补丁位变化
        out.lower = lower;
        out.upper = version.majorWildcard ? kNumberInfinity
            : version.minorWildcard ? EncodeNumber(version.major + 1u, 0, 0)
            : EncodeNumber(version.major, version.minor + 1u, 0);
        return true;
    }
    return false;
}

bool RangeContains(const VersionRange& range, std::uint32_t number) {
    if (number < range.lower) return false;
    if (!range.includeLower && number == range.lower) return false;
    return number < range.upper;
}

std::string ToLower(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool ClauseSatisfied(const std::string& clause, std::uint32_t runtimeNumber) {
    // latest 是第三方运行库的专用写法，跟随最新发行版，不做版本锁定
    if (ToLower(Trim(clause)) == "latest") {
        return true;
    }
    static const char* const operators[] = {">=", "<=", "==", ">", "<", "^", "~", "="};

    // 连字符区间 1.2.3 - 2.0.0
    const std::size_t hyphen = clause.find(" - ");
    if (hyphen != std::string::npos) {
        PartialVersion low;
        PartialVersion high;
        if (!ParsePartial(clause.substr(0, hyphen), low)) return false;
        if (!ParsePartial(clause.substr(hyphen + 3), high)) return false;
        VersionRange lowerRange;
        VersionRange upperRange;
        if (!BuildRange(">=", low, lowerRange)) return false;
        if (!BuildRange("<", high, upperRange)) return false;
        return RangeContains(lowerRange, runtimeNumber) && RangeContains(upperRange, runtimeNumber);
    }

    std::string op;
    std::string version = clause;
    for (const char* candidate : operators) {
        const std::string prefix = candidate;
        if (version.rfind(prefix, 0) == 0) {
            op = prefix;
            version = version.substr(prefix.size());
            break;
        }
    }
    version = Trim(version);

    PartialVersion partial;
    if (!ParsePartial(version, partial)) {
        return false;
    }
    VersionRange range;
    if (!BuildRange(op, partial, range)) {
        return false;
    }
    return RangeContains(range, runtimeNumber);
}

bool GroupSatisfied(const std::string& group, std::uint32_t runtimeNumber) {
    std::size_t start = 0;
    while (start <= group.size()) {
        std::size_t end = group.size();
        for (std::size_t index = start; index < group.size(); ++index) {
            if (std::isspace(static_cast<unsigned char>(group[index]))) {
                end = index;
                break;
            }
        }
        const std::string clause = Trim(group.substr(start, end - start));
        if (!clause.empty() && !ClauseSatisfied(clause, runtimeNumber)) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

} // namespace

// 发布通道标记。latest 跟随最新发行版，其余按通道筛选，
// 都不做数字比较，走 SatisfiesText 的版本字符串匹配
bool IsChannelToken(const std::string& value) {
    static const char* const channels[] = {"latest", "alpha", "beta", "rc", "stable", "nightly", "dev"};
    const std::string lower = ToLower(Trim(value));
    for (const char* channel : channels) {
        if (lower == channel) return true;
    }
    return false;
}

bool VersionHasChannel(const std::string& versionText, const std::string& channel) {
    return ToLower(versionText).find(ToLower(Trim(channel))) != std::string::npos;
}

// 三段完整版本号的解析，预发布后缀不影响比较；依赖与约束比较都用它
bool ParseVersionNumber(const std::string& text, std::uint32_t& number) {
    PartialVersion partial;
    if (!ParsePartial(text, partial) || partial.any || partial.majorWildcard
        || partial.minorWildcard || partial.patchWildcard) {
        return false;
    }
    number = EncodeNumber(partial.major, partial.minor, partial.patch);
    return true;
}

bool ParseVersion(const std::string& text, std::uint32_t& number) {
    return ParseVersionNumber(text, number);
}

// 清单路径自己拼而不走 ModDirectory：后者会顺手建目录，
// 只查一次清单却建出一个空的模组目录是纯粹的副作用（清单一律只读）
std::string ManifestPath(const std::string& modName) {
    return Platform::XBaseDirectory() + "Mods\\" + modName + "\\package.json";
}

bool Load(const std::string& modName, Info& out) {
    out = Info{};
    out.name = modName;

    const std::string path = ManifestPath(modName);
    if (!Platform::FileExists(path)) {
        return false;
    }

    std::string text;
    if (!Platform::ReadTextFile(path, text)) {
        return false;
    }

    const Json::Value root = Json::Value::Parse(text);
    if (!root.IsObject()) {
        return false;
    }

    if (root["name"].IsString()) out.name = root["name"].AsString();
    if (root["version"].IsString()) out.version = root["version"].AsString();
    if (root["author"].IsString()) out.author = root["author"].AsString();
    if (root["description"].IsString()) out.description = root["description"].AsString();
    if (root["license"].IsString()) out.license = root["license"].AsString();
    if (root["homepage"].IsString()) out.homepage = root["homepage"].AsString();

    const Json::Value& engines = root["engines"];
    if (engines.IsObject() && engines["xbase"].IsString()) {
        out.xbaseRequirement = engines["xbase"].AsString();
    }

    const Json::Value& dependencies = root["dependencies"];
    if (dependencies.IsObject()) {
        for (const std::string& dependencyName : dependencies.Keys()) {
            const Json::Value& requirement = dependencies[dependencyName];
            if (requirement.IsString()) {
                out.dependencies.push_back({dependencyName, requirement.AsString()});
            }
        }
    }

    out.valid = true;
    return true;
}

bool SatisfiesText(const std::string& requirement, const std::string& runtimeVersion) {
    const std::string trimmed = Trim(requirement);
    // 通道标记按版本字符串匹配，数字区间按数字比较，两条路互不干扰
    if (IsChannelToken(trimmed)) {
        if (ToLower(trimmed) == "latest") {
            return !runtimeVersion.empty();
        }
        return !runtimeVersion.empty() && VersionHasChannel(runtimeVersion, trimmed);
    }
    std::uint32_t number = 0;
    if (!ParseVersion(runtimeVersion, number)) {
        return false;
    }
    return Satisfies(requirement, number);
}

bool Satisfies(const std::string& requirement, std::uint32_t runtimeNumber) {
    const std::string trimmed = Trim(requirement);
    if (trimmed.empty() || trimmed == "*" || IsChannelToken(trimmed)) {
        return true;
    }
    // || 分隔的组任一满足即可
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t separator = trimmed.find("||", start);
        const std::string group = Trim(trimmed.substr(start, separator - start));
        if (!group.empty() && GroupSatisfied(group, runtimeNumber)) {
            return true;
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 2;
    }
    return false;
}

bool Validate(
    const std::string& modName,
    std::string& failureReason,
    std::string* warningReason) {
    failureReason.clear();
    if (warningReason) warningReason->clear();

    Info info;
    const bool exists = Load(modName, info);
    if (!exists) {
        // 没有清单视为无约束，旧的 mod 目录照常挂载
        return true;
    }
    if (!info.valid) {
        failureReason = "The mod package manifest is malformed.\n\n"
                        "File:\n" + ManifestPath(modName);
        return false;
    }

    std::string problems;
    if (!SatisfiesText(info.xbaseRequirement, kVersionString)) {
        problems += "XBase " + (info.xbaseRequirement.empty() ? std::string("*") : info.xbaseRequirement)
            + " is required, installed is " + kVersionString;
    }

    // 依赖分两类：名字对应其它 mod 的按对方清单版本校验；
    // 对应不到 mod 的是第三方运行库（asi loader、补丁、图形转换层这类），只声明不自动校验，
    // 版本是否匹配由 mod 随包分发或安装说明保证
    for (const auto& dependency : info.dependencies) {
        Info dependencyInfo;
        if (!Load(dependency.first, dependencyInfo) || !dependencyInfo.valid) {
            continue;
        }
        if (!SatisfiesText(dependency.second, dependencyInfo.version)) {
            if (!problems.empty()) problems += "\n";
            problems += "Dependency " + dependency.first + " is at " + dependencyInfo.version
                + ", required " + dependency.second;
        }
    }

    if (problems.empty()) {
        return true;
    }
    if (warningReason) {
        *warningReason = "XBase warning: package requirements are not satisfied; the mod will continue to load.\n\n"
            "Mod: " + info.name + "\nInstalled XBase: " + kVersionString + "\n\n" + problems;
    }
    return true;
}

} // namespace XBase::Package
