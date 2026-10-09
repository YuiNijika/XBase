#include <XBase/Targeting.h>
#include <XBase/Hooks.h>
#include <XBase/Capabilities.h>
#include <XBase/Runtime.h>

#include "../backends/TargetingBackend.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

namespace XBase::Targeting {
namespace {
Config s_config;
std::vector<Target> s_targets;
std::mutex s_targetsMutex;
Kind s_selectedKind = Kind::Ped;
EntityId s_selectedId{};
Target s_selectedTarget{};
bool s_hasSelectedTarget = false;
Labels s_labels;

void ClearSelectionUnlocked() {
    s_selectedId = {};
    s_selectedTarget = {};
    s_hasSelectedTarget = false;
    for (Target& target : s_targets) target.selected = false;
}
}

std::vector<ActionInfo> GetActions(Kind kind) {
    const bool vehicle = kind == Kind::Vehicle;
    const bool basic = HasCapability(vehicle ? FeatureCapability::VehicleBasic : FeatureCapability::PedBasic);
    const bool sa = Runtime::GetGameTarget() == Runtime::GameTarget::SanAndreas;
    std::vector<ActionInfo> actions;
    const auto add = [&](Action action, const char* key, Parameter parameter = Parameter::None,
                         float min = 0.0f, float max = 0.0f, float value = 0.0f, bool supported = true) {
        actions.push_back({action, key, parameter, min, max, value, basic && supported});
    };
    add(Action::None, "targeting.action.none");
    add(Action::Restore, vehicle ? "targeting.restore" : "targeting.heal");
    add(vehicle ? Action::Ignite : Action::Kill, vehicle ? "targeting.action.ignite" : "targeting.action.kill");
    add(Action::Health, "targeting.action.health", Parameter::Number, 0, 100000, vehicle ? 1000.0f : 100.0f);
    add(Action::MaxHealth, "targeting.action.maxHealth", Parameter::Number, 1, 100000, vehicle ? 1000.0f : 100.0f);
    add(Action::Bring, "targeting.bring");
    add(Action::Teleport, "targeting.teleport");
    add(Action::Stop, "targeting.action.stop");
    add(Action::Freeze, "targeting.action.freeze", Parameter::Toggle);
    add(Action::Delete, "targeting.action.delete", Parameter::None, 0, 0, 0,
        HasCapability(vehicle ? FeatureCapability::VehicleDelete : FeatureCapability::PedDelete));
    add(Action::Visible, "targeting.action.visible", Parameter::Toggle);
    add(Action::Proofs, "targeting.action.proofs", Parameter::Toggle);
    add(Action::BulletProof, "targeting.action.bulletProof", Parameter::Toggle);
    add(Action::FireProof, "targeting.action.fireProof", Parameter::Toggle);
    add(Action::ExplosionProof, "targeting.action.explosionProof", Parameter::Toggle);
    add(Action::CollisionProof, "targeting.action.collisionProof", Parameter::Toggle);
    add(Action::MeleeProof, "targeting.action.meleeProof", Parameter::Toggle);
    if (!vehicle) {
        add(Action::Armour, "targeting.armour", Parameter::Number, 0, 100000, 100);
        add(Action::Disarm, "targeting.disarm");
        const int maxWeapon = sa ? 43 : Runtime::GetGameTarget() == Runtime::GameTarget::ViceCity ? 33 : 12;
        add(Action::Weapon, "targeting.action.weapon", Parameter::Weapon, 1, static_cast<float>(maxWeapon), 1,
            HasCapability(FeatureCapability::WeaponGive));
        return actions;
    }
    add(Action::Upright, "targeting.upright");
    add(Action::Unlock, "targeting.unlock");
    add(Action::Lock, "targeting.action.lock");
    add(Action::Colors, "targeting.action.colors", Parameter::Colors, 0, 255, 0,
        HasCapability(FeatureCapability::VehicleColors));
    add(Action::Explode, "targeting.action.explode");
    add(Action::Engine, "targeting.action.engine", Parameter::Toggle);
    add(Action::Lights, "targeting.action.lights", Parameter::Toggle);
    add(Action::Speed, "targeting.action.speed", Parameter::Number, 0, 300, 60);
    add(Action::OpenDoor, "targeting.action.openDoor", Parameter::Door, 0, 5, 0,
        HasCapability(FeatureCapability::VehicleDoors));
    add(Action::WarpToSeat, "targeting.action.warpToSeat", Parameter::Seat, 0, 7);
    add(Action::EnterVehicle, "targeting.action.enterVehicle");
    add(Action::Heavy, "targeting.action.heavy", Parameter::Toggle);
    add(Action::Watertight, "targeting.action.watertight", Parameter::Toggle);
    add(Action::PopDoor, "targeting.action.popDoor", Parameter::Door, 0, 5, 0, sa);
    add(Action::SkidMarks, "targeting.action.skidMarks", Parameter::Toggle, 0, 0, 0, sa);
    add(Action::Particles, "targeting.action.particles", Parameter::Toggle, 0, 0, 0, sa);
    add(Action::DriverTargetable, "targeting.action.driverTargetable", Parameter::Toggle, 0, 0, 0, sa);
    add(Action::HeatSeekingTargetable, "targeting.action.heatSeekingTargetable", Parameter::Toggle, 0, 0, 0, sa);
    add(Action::PetrolTankWeakPoint, "targeting.action.petrolTankWeakPoint", Parameter::Toggle, 0, 0, 0, sa);
    add(Action::Siren, "targeting.action.siren", Parameter::Toggle, 0, 0, 0, sa);
    add(Action::TakeLessDamage, "targeting.action.takeLessDamage", Parameter::Toggle, 0, 0, 0, sa);
    add(Action::Paintjob, "targeting.action.paintjob", Parameter::Number, -1, 2, -1,
        HasCapability(FeatureCapability::VehiclePaintjob));
    add(Action::AddUpgrade, "targeting.action.addUpgrade", Parameter::Number, 1000, 1193, 1000,
        HasCapability(FeatureCapability::VehicleUpgrades));
    add(Action::RemoveUpgrade, "targeting.action.removeUpgrade", Parameter::Number, 1000, 1193, 1000,
        HasCapability(FeatureCapability::VehicleUpgrades));
    add(Action::RemoveAllUpgrades, "targeting.action.removeAllUpgrades", Parameter::None, 0, 0, 0,
        HasCapability(FeatureCapability::VehicleUpgrades));
    return actions;
}

void SetConfig(const Config& config) {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    s_config = config;
    if (!std::isfinite(s_config.radius)) s_config.radius = 80.0f;
    if (!std::isfinite(s_config.hitRadius)) s_config.hitRadius = 160.0f;
    s_config.radius = std::clamp(s_config.radius, 5.0f, 250.0f);
    s_config.hitRadius = std::clamp(s_config.hitRadius, 40.0f, 400.0f);
    s_config.maxTargets = std::clamp(s_config.maxTargets, 1, 64);
    for (Kind kind : {Kind::Ped, Kind::Vehicle}) {
        Menu& menu = kind == Kind::Ped ? s_config.pedMenu : s_config.vehicleMenu;
        const auto actions = GetActions(kind);
        for (Binding& binding : menu) {
            const auto found = std::find_if(actions.begin(), actions.end(), [&](const ActionInfo& info) {
                return info.action == binding.action && info.supported;
            });
            if (found == actions.end()) {
                binding = {};
                continue;
            }
            binding.value = std::isfinite(binding.value)
                ? std::clamp(binding.value, found->minimum, found->maximum) : found->defaultValue;
            binding.secondary = std::clamp(binding.secondary, 0, binding.action == Action::Weapon ? 99999 : 255);
            binding.tertiary = std::clamp(binding.tertiary, 0, 255);
            binding.quaternary = std::clamp(binding.quaternary, 0, 255);
        }
    }
    Detail::TargetingBackend::ResetPointerState();
    ClearSelectionUnlocked();
    if (!s_config.enabled) {
        Hooks::SetMiddleInputSuppressed(false);
        s_targets.clear();
        Detail::TargetingBackend::ResetPointerState();
        ClearSelectionUnlocked();
    }
}

Config GetConfig() {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    return s_config;
}

void SetLabels(const Labels& labels) {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    s_labels = labels;
    Detail::TargetingBackend::SetLabels(s_labels);
}

void Process() {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    Detail::TargetingBackend::ProcessEffects();
    if (!s_config.enabled) {
        s_targets.clear();
        Hooks::SetMiddleInputSuppressed(false);
        return;
    }
    Hooks::SetMiddleInputSuppressed(
        s_config.mouseSelect && !Hooks::IsMenuVisible() && !Hooks::IsKeyboardCaptureActive());
    if (s_hasSelectedTarget) {
        const unsigned int lockedModel = s_selectedTarget.modelId;
        if (!Detail::TargetingBackend::Refresh(s_selectedTarget) || s_selectedTarget.modelId != lockedModel) {
            ClearSelectionUnlocked();
        } else if (std::none_of(s_targets.begin(), s_targets.end(), [](const Target& target) {
                       return target.selected;
                   })) {
            s_selectedTarget.selected = true;
            s_targets.push_back(s_selectedTarget);
        }
    }
    Detail::TargetingBackend::Collect(s_config, s_targets);
    for (Target& target : s_targets) {
        target.selected = target.kind == s_selectedKind && target.id.value == s_selectedId.value;
        if (target.selected) {
            s_selectedTarget = target;
            s_hasSelectedTarget = true;
        }
    }
    if (Hooks::IsMenuVisible() || Hooks::IsKeyboardCaptureActive()) {
        Detail::TargetingBackend::ResetPointerState();
        ClearSelectionUnlocked();
        return;
    }
    Target commandTarget;
    const int action = Detail::TargetingBackend::ConsumeAction(commandTarget);
    const bool commandMatchesLock =
        s_hasSelectedTarget
        && commandTarget.kind == s_selectedKind
        && commandTarget.id.value == s_selectedId.value
        && commandTarget.modelId == s_selectedTarget.modelId;
    if (action == 3 && commandMatchesLock) {
        ClearSelectionUnlocked();
        return;
    }
    // 看门狗式操作链路只允许作用于当前锁定实体；输入边沿跨帧或实体重用
    // 时，丢弃过期命令，避免把动作落到另一个恰好复用相同句柄的目标上。
    if (!Hooks::IsMenuVisible() && !Hooks::IsKeyboardCaptureActive()
        && action != 0 && s_config.mouseSelect && commandMatchesLock
        && Detail::TargetingBackend::Refresh(commandTarget)
        && commandTarget.modelId == s_selectedTarget.modelId) {
        constexpr int slots[] = {1, 4, 5, 2, 6, 7};
        const auto slot = std::find(std::begin(slots), std::end(slots), action);
        if (slot != std::end(slots)) {
            const Menu& menu = commandTarget.kind == Kind::Vehicle ? s_config.vehicleMenu : s_config.pedMenu;
            Detail::TargetingBackend::ExecuteBinding(commandTarget, menu[slot - std::begin(slots)]);
        }
        ClearSelectionUnlocked();
    }
}

void Draw() {
    Config config;
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(s_targetsMutex);
        config = s_config;
        targets = s_targets;
    }
    if (!config.enabled) return;

    // 看门狗式交互不接管全局鼠标：游戏继续负责镜头移动，
    // backend 用准星位置和鼠标中键边沿完成目标锁定。
    if (config.mouseSelect && !Hooks::IsMenuVisible() && !Hooks::IsKeyboardCaptureActive()) {
        Target picked;
        if (Detail::TargetingBackend::PickAtCursor(config, targets, picked)) {
            Select(picked.kind, picked.id);
            for (Target& target : targets) {
                target.selected = target.kind == picked.kind && target.id.value == picked.id.value;
            }
        }
    } else {
        Detail::TargetingBackend::ResetPointerState();
    }

    Detail::TargetingBackend::Draw(config, targets);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    s_config = {};
    Hooks::SetMiddleInputSuppressed(false);
    s_targets.clear();
    Detail::TargetingBackend::ResetPointerState();
    ClearSelectionUnlocked();
    Detail::TargetingBackend::ClearEffects();
}

void NotifyGameInit() {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    s_targets.clear();
    Detail::TargetingBackend::ResetPointerState();
    Detail::TargetingBackend::ClearEffects();
    ClearSelectionUnlocked();
}

std::vector<Target> GetTargets() {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    return s_targets;
}

bool Select(Kind kind, EntityId id) {
    if (!id) return false;
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    const auto found = std::find_if(s_targets.begin(), s_targets.end(), [kind, id](const Target& target) {
        return target.kind == kind && target.id.value == id.value;
    });
    if (found == s_targets.end()) return false;
    s_selectedKind = kind;
    s_selectedId = id;
    s_selectedTarget = *found;
    s_hasSelectedTarget = true;
    for (Target& target : s_targets) {
        target.selected = target.kind == kind && target.id.value == id.value;
    }
    return true;
}

void ClearSelection() {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    ClearSelectionUnlocked();
}

bool GetSelected(Target& target) {
    std::lock_guard<std::mutex> lock(s_targetsMutex);
    const auto found = std::find_if(s_targets.begin(), s_targets.end(), [](const Target& value) {
        return value.selected;
    });
    if (found != s_targets.end()) {
        target = *found;
        return true;
    }
    if (!s_hasSelectedTarget) return false;
    target = s_selectedTarget;
    return true;
}

bool SetSelectedHealth(float health) {
    Target selected;
    if (!GetSelected(selected)) return false;
    return Detail::TargetingBackend::SetHealth(selected.kind, selected.id, std::max(0.0f, health));
}

bool DeleteSelected() {
    Target selected;
    if (!GetSelected(selected)) return false;
    const bool deleted = Detail::TargetingBackend::Delete(selected.kind, selected.id);
    if (deleted) ClearSelection();
    return deleted;
}

} // namespace XBase::Targeting
