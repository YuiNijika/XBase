#include "TargetingBackend.h"
#include "TargetingInteraction.h"
#include "RuntimeGuard.h"
#include "VehicleBackend.h"

#include "plugin.h"
#include "extensions/ScriptCommands.h"
#include "common.h"
#include "CEntity.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CCamera.h"
#include "CPools.h"
#include "CPad.h"
#include "CSprite.h"
#include "CVehicle.h"
#include "CAutomobile.h"
#include "CModelInfo.h"
#include "CStreaming.h"
#include "CWeaponInfo.h"
#if defined(GTASA)
#include "CMenuManager.h"
#else
#include "CMousePointerStateHelper.h"
#endif
#include "CVector.h"
#include "RenderWare.h"
#include "imgui.h"
#include <XBase/Hooks.h>
#include <XBase/Camera.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace XBase::Detail::TargetingBackend {
Targeting::Labels s_labels;
namespace {
Interaction s_interaction;
std::atomic<bool> s_capturePointer{false};
float s_pointerX = 0.0f;
float s_pointerY = 0.0f;
std::mutex s_commandMutex;
Targeting::Target s_commandTarget{};
int s_command = 0;
struct HealthLimit {
    Targeting::Target target;
    float maximum;
};
std::vector<HealthLimit> s_healthLimits;
struct VehicleTransfer {
    Targeting::Target target;
    Targeting::Binding binding;
    int remaining = 0;
};
VehicleTransfer s_vehicleTransfer;
constexpr int kVehicleTransferFrames = 60;
constexpr float kPi = 3.14159265359f;

bool SameTarget(const Targeting::Target& left, const Targeting::Target& right) {
    return left.kind == right.kind && left.id.value == right.id.value
        && left.modelId == right.modelId;
}

bool Project(const Targeting::Target& target, ImVec2& screen);

void QueueAction(int action, const Targeting::Target& target) {
    std::lock_guard<std::mutex> lock(s_commandMutex);
    if (s_command != 0) return;
    s_command = action;
    s_commandTarget = target;
}

void Diamond(ImDrawList* draw, ImVec2 point, float radius, ImU32 color, float width) {
    const ImVec2 points[] = {
        {point.x, point.y - radius}, {point.x + radius, point.y},
        {point.x, point.y + radius}, {point.x - radius, point.y},
    };
    draw->AddPolyline(points, 4, color, ImDrawFlags_Closed, width);
}

bool WorldToScreen(const CVector& world, ImVec2& screen) {
    if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)) return false;
    RwV3d input{world.x, world.y, world.z};
    RwV3d output{};
    float width = 0.0f;
    float height = 0.0f;
    if (!CSprite::CalcScreenCoors(input, &output, &width, &height, true)) return false;
    if (!std::isfinite(output.x) || !std::isfinite(output.y) || output.z <= 0.01f) return false;
    screen = {output.x, output.y};
    return true;
}

bool Project(const Targeting::Target& target, ImVec2& screen) {
    if (!WorldToScreen({target.position.x, target.position.y, target.position.z + 0.6f}, screen)) return false;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    return screen.x >= 0.0f && screen.y >= 0.0f && screen.x <= size.x && screen.y <= size.y;
}

bool IsValidPed(CPed* ped, CPlayerPed* player) {
    return ped && ped != player && ped->m_fHealth > 0.0f;
}

bool IsInVehicle(CPed* ped) {
#if defined(GTASA)
    return ped && ped->bInVehicle && ped->m_pVehicle;
#else
    return ped && ped->m_bInVehicle && ped->m_pVehicle;
#endif
}

void DisarmPed(CPed* ped) {
    // Switch away from the equipped weapon before clearing inventory so its model is removed.
#if defined(GTA3)
    ped->SetCurrentWeapon(0);
#else
    ped->SetCurrentWeapon(static_cast<eWeaponType>(0));
#endif
    ped->ClearWeapons();
#if defined(GTA3)
    ped->SetCurrentWeapon(0);
#else
    ped->SetCurrentWeapon(static_cast<eWeaponType>(0));
#endif
}

void RestoreVehicleCamera() {
    if (Camera::IsActive()) return;
    TheCamera.UpdateTargetEntity();
    plugin::Command<plugin::Commands::RESTORE_CAMERA_JUMPCUT>();
}

bool IsValidVehicle(CVehicle* vehicle, CPlayerPed* player) {
    return vehicle && vehicle->m_fHealth > 0.0f
        && (!IsInVehicle(player) || vehicle != player->m_pVehicle);
}

bool RefreshTarget(Targeting::Target& target, CPlayerPed* player, const CVector& origin) {
    if (!target.id) return false;
    const int ref = static_cast<int>(target.id.value - 1u);

    if (target.kind == Targeting::Kind::Ped) {
        CPed* ped = CPools::GetPed(ref);
        if (!IsValidPed(ped, player)) return false;
        const CVector position = ped->GetPosition();
        target.modelId = static_cast<unsigned int>(ped->m_nModelIndex);
        target.position = {position.x, position.y, position.z};
        target.distance = (position - origin).Magnitude();
        target.health = ped->m_fHealth;
        return true;
    }

    CVehicle* vehicle = CPools::GetVehicle(ref);
    if (!IsValidVehicle(vehicle, player)) return false;
    const CVector position = vehicle->GetPosition();
    target.modelId = static_cast<unsigned int>(vehicle->m_nModelIndex);
    target.position = {position.x, position.y, position.z};
    target.distance = (position - origin).Magnitude();
    target.health = vehicle->m_fHealth;
    return true;
}

} // namespace

bool Refresh(Targeting::Target& target) {
    if (!RuntimeGuard::IsRuntimeSafe()) return false;
    CPlayerPed* player = FindPlayerPed();
    return player && RefreshTarget(target, player, player->GetPosition());
}

int ConsumeAction(Targeting::Target& target) {
    std::lock_guard<std::mutex> lock(s_commandMutex);
    const int action = s_command;
    target = s_commandTarget;
    s_command = 0;
    s_commandTarget = {};
    return action;
}

void Collect(const Targeting::Config& config, std::vector<Targeting::Target>& targets) {
    Targeting::Target lockedTarget{};
    bool hasLockedTarget = false;
    if (const auto selected = std::find_if(targets.begin(), targets.end(), [](const Targeting::Target& target) {
            return target.selected;
        }); selected != targets.end()) {
        lockedTarget = *selected;
        hasLockedTarget = true;
    }

    targets.clear();
    if (!RuntimeGuard::IsRuntimeSafe()) return;
    CPlayerPed* player = FindPlayerPed();
    if (!player) return;
    const CVector origin = player->GetPosition();
    const float radiusSquared = config.radius * config.radius;

    if (config.includePeds && CPools::ms_pPedPool) {
        for (int index = 0; index < CPools::ms_pPedPool->m_nSize; ++index) {
            CPed* ped = CPools::ms_pPedPool->GetAt(index);
            if (!IsValidPed(ped, player)) continue;
            const CVector position = ped->GetPosition();
            const CVector delta = position - origin;
            const float distanceSquared = delta.MagnitudeSqr();
            if (distanceSquared > radiusSquared) continue;
            targets.push_back({Targeting::Kind::Ped,
                EntityId{static_cast<std::uint32_t>(CPools::GetPedRef(ped)) + 1u},
                static_cast<unsigned int>(ped->m_nModelIndex),
                {position.x, position.y, position.z},
                std::sqrt(distanceSquared), ped->m_fHealth, false});
        }
    }
    if (config.includeVehicles && CPools::ms_pVehiclePool) {
        for (int index = 0; index < CPools::ms_pVehiclePool->m_nSize; ++index) {
            CVehicle* vehicle = CPools::ms_pVehiclePool->GetAt(index);
            if (!IsValidVehicle(vehicle, player)) continue;
            const CVector position = vehicle->GetPosition();
            const CVector delta = position - origin;
            const float distanceSquared = delta.MagnitudeSqr();
            if (distanceSquared > radiusSquared) continue;
            targets.push_back({Targeting::Kind::Vehicle,
                EntityId{static_cast<std::uint32_t>(CPools::GetVehicleRef(vehicle)) + 1u},
                static_cast<unsigned int>(vehicle->m_nModelIndex),
                {position.x, position.y, position.z},
                std::sqrt(distanceSquared), vehicle->m_fHealth, false});
        }
    }

    // 保证已经锁定的目标不会因为排序或 maxTargets 截断而消失。
    // 如果目标仍然存在，即使它暂时离开扫描半径，也用实体池刷新位置和血量，
    // 让链路继续跟随真实目标，而不是停在旧的绘制快照上。
    if (hasLockedTarget) {
        const auto current = std::find_if(targets.begin(), targets.end(), [&lockedTarget](const Targeting::Target& target) {
            return target.kind == lockedTarget.kind && target.id.value == lockedTarget.id.value;
        });
        if (current != targets.end()) {
            current->selected = true;
        } else if (RefreshTarget(lockedTarget, player, origin)) {
            lockedTarget.selected = true;
            targets.push_back(lockedTarget);
        }
    }

    std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
        return left.distance < right.distance;
    });

    while (static_cast<int>(targets.size()) > config.maxTargets) {
        const auto remove = std::find_if(targets.rbegin(), targets.rend(), [](const Targeting::Target& target) {
            return !target.selected;
        });
        if (remove == targets.rend()) break;
        targets.erase(std::prev(remove.base()));
    }
}

void Draw(const Targeting::Config& config, const std::vector<Targeting::Target>& targets) {
    if (!ImGui::GetCurrentContext() || !RuntimeGuard::IsRuntimeSafe()
        || Hooks::IsMenuVisible() || Hooks::IsKeyboardCaptureActive()) return;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (size.x <= 1.0f || size.y <= 1.0f) return;
    const ImVec2 center{size.x * 0.5f, size.y * 0.5f};
    const bool interactive = config.mouseSelect && !Hooks::IsMenuVisible() && !Hooks::IsKeyboardCaptureActive();
    const ImU32 white = IM_COL32(242, 246, 247, 240);
    const ImU32 cyan = IM_COL32(90, 226, 232, 255);

    struct ScreenNode {
        const Targeting::Target* target = nullptr;
        ImVec2 screen{};
        bool focused = false;
        bool active = false;
    };
    std::vector<ScreenNode> nodes;
    nodes.reserve(targets.size());
    for (const auto& target : targets) {
        ImVec2 screen{};
        if (!Project(target, screen)) continue;
        const bool focused = interactive && s_interaction.HasFocus()
            && SameTarget(target, s_interaction.Focus());
        const bool active = focused || target.selected;
        nodes.push_back({&target, screen, focused, active});
    }

    if (config.drawLinks) {
        const float phase = static_cast<float>(std::fmod(ImGui::GetTime() * 0.65, 1.0));
        for (const ScreenNode& node : nodes) {
            const ImU32 color = node.active ? cyan : IM_COL32(230, 236, 238, 65);
            drawList->AddLine(center, node.screen, IM_COL32(0, 0, 0, 110), node.active ? 3.0f : 2.0f);
            drawList->AddLine(center, node.screen, color, node.active ? 1.6f : 0.8f);
            if (!node.active) continue;
            drawList->AddCircleFilled(
                {center.x + (node.screen.x - center.x) * phase,
                 center.y + (node.screen.y - center.y) * phase}, 2.5f,
                color);
        }
    }

    for (const ScreenNode& node : nodes) {
        const Targeting::Target& target = *node.target;
        const bool focused = node.focused;
        const bool active = node.active;
        const ImU32 color = focused ? white : target.selected ? cyan : IM_COL32(230, 236, 238, 130);
        Diamond(drawList, node.screen, active ? 10.0f : 5.0f, color, active ? 1.7f : 1.0f);
        if (active) {
            drawList->AddCircleFilled(node.screen, 2.0f, color);
            Diamond(drawList, node.screen, 15.0f, IM_COL32(240, 245, 247, 80), 1.0f);
        }
        if (focused) {
            const float width = 236.0f;
            const float height = 52.0f;
            const ImVec2 top{
                std::clamp(node.screen.x + 26.0f, 8.0f, std::max(8.0f, size.x - width - 8.0f)),
                std::clamp(node.screen.y - 26.0f, 8.0f, std::max(8.0f, size.y - height - 8.0f)),
            };
            drawList->AddRectFilled(top, {top.x + width, top.y + height}, IM_COL32(10, 12, 15, 225));
            drawList->AddRectFilled(top, {top.x + 3.0f, top.y + height}, white);
            char title[96]{};
            std::snprintf(title, sizeof(title), "%s / %u",
                target.kind == Targeting::Kind::Vehicle ? s_labels.vehicle.c_str() : s_labels.ped.c_str(),
                target.modelId);
            drawList->AddText({top.x + 12.0f, top.y + 8.0f}, white, title);
            char detail[96]{};
            std::snprintf(detail, sizeof(detail), "%.0fm  /  %.0f HP", target.distance, target.health);
            drawList->AddText({top.x + 12.0f, top.y + 29.0f}, IM_COL32(170, 181, 187, 255), detail);
        }
    }
    if (interactive) {
        drawList->AddCircle(center, s_interaction.HasFocus() ? 6.0f : 3.0f, white, 20, 1.0f);
        if (s_interaction.GetState() == Interaction::State::Operating) {
            const float radius = 96.0f;
            const bool vehicle = s_interaction.Focus().kind == Targeting::Kind::Vehicle;
            drawList->AddCircle(center, radius, IM_COL32(0, 0, 0, 200), 48, 18.0f);
            const Targeting::Menu& menu = vehicle ? config.vehicleMenu : config.pedMenu;
            constexpr int actions[] = {1, 4, 5, 2, 6, 7};
            const float labelRadius = radius + 32.0f;
            for (int sector = 0; sector < 6; ++sector) {
                const auto action = menu[sector].action;
                const char* label = action == Targeting::Action::Restore
                    ? (vehicle ? s_labels.restore.c_str() : s_labels.heal.c_str())
                    : s_labels.actions[static_cast<std::size_t>(action)].c_str();
                const float angle = -kPi * 0.5f + sector * kPi / 3.0f;
                const bool chosen = s_interaction.Action() == actions[sector];
                const ImU32 color = chosen
                    ? (action == Targeting::Action::Kill || action == Targeting::Action::Ignite
                        || action == Targeting::Action::Explode || action == Targeting::Action::Delete
                        ? IM_COL32(255, 126, 122, 255) : cyan)
                    : IM_COL32(240, 245, 247, 180);
                drawList->PathArcTo(center, radius, angle - kPi / 6.0f + 0.05f,
                    angle + kPi / 6.0f - 0.05f, 16);
                drawList->PathStroke(color, 0, chosen ? 8.0f : 4.0f);
                const ImVec2 textSize = ImGui::CalcTextSize(label);
                drawList->AddText({center.x + std::cos(angle) * labelRadius - textSize.x * 0.5f,
                    center.y + std::sin(angle) * labelRadius - textSize.y * 0.5f}, color, label);
            }
            const char* kind = vehicle ? s_labels.vehicle.c_str() : s_labels.ped.c_str();
            const ImVec2 kindSize = ImGui::CalcTextSize(kind);
            drawList->AddRectFilled({center.x - kindSize.x * 0.5f - 12.0f, center.y - 22.0f},
                {center.x + kindSize.x * 0.5f + 12.0f, center.y + 22.0f}, IM_COL32(10, 12, 15, 220), 4.0f);
            drawList->AddText({center.x - kindSize.x * 0.5f, center.y - kindSize.y * 0.5f}, white, kind);
            const float px = s_interaction.PointerX();
            const float py = s_interaction.PointerY();
            const float length = std::sqrt(px * px + py * py);
            const float scale = length > radius ? radius / length : 1.0f;
            drawList->AddCircleFilled({center.x + px * scale, center.y + py * scale}, 4.0f, white);
        }
    }
}

bool PickAtCursor(
    const Targeting::Config& config,
    const std::vector<Targeting::Target>& targets,
    Targeting::Target& target) {
    if (!config.mouseSelect || !ImGui::GetCurrentContext() || !RuntimeGuard::IsRuntimeSafe()) {
        ResetPointerState();
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &processId);
    if (processId != GetCurrentProcessId()) {
        ResetPointerState();
        return false;
    }
    const bool middleButtonDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    const bool cancel = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0
        || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 1.0f || displaySize.y <= 1.0f) return false;
    const ImVec2 cursor{displaySize.x * 0.5f, displaySize.y * 0.5f};
    const float maxDistance = std::max(config.hitRadius, 40.0f) * displaySize.y / 1080.0f;
    const float maxDistanceSquared = maxDistance * maxDistance;
    float nearestDistanceSquared = maxDistanceSquared;
    const Targeting::Target* candidate = nullptr;

    if (s_interaction.IsLocked()) {
        const auto current = std::find_if(targets.begin(), targets.end(), [](const Targeting::Target& item) {
            return SameTarget(item, s_interaction.Focus());
        });
        if (current != targets.end()) candidate = &*current;
    } else {
        for (const Targeting::Target& item : targets) {
            ImVec2 screen{};
            if (!Project(item, screen)) continue;
            const float dx = screen.x - cursor.x;
            const float dy = screen.y - cursor.y;
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > nearestDistanceSquared) continue;
            nearestDistanceSquared = distanceSquared;
            candidate = &item;
        }
    }
    float dx = 0.0f;
    float dy = 0.0f;
    {
        std::lock_guard<std::mutex> lock(s_commandMutex);
        dx = s_pointerX;
        dy = s_pointerY;
        s_pointerX = s_pointerY = 0.0f;
    }
    const Interaction::Result result = s_interaction.Update(
        candidate, middleButtonDown, cancel, dx, dy, ImGui::GetTime());
    s_capturePointer.store(s_interaction.IsLocked(), std::memory_order_release);
    if (result.action) QueueAction(result.action, result.target);
    if (!result.selected) return false;
    target = result.target;
    return true;
}

void ResetPointerState() {
    s_interaction.Reset();
    s_capturePointer.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(s_commandMutex);
    s_pointerX = s_pointerY = 0.0f;
    s_command = 0;
    s_commandTarget = {};
}

void UpdatePointerInput() {
    if (!RuntimeGuard::IsRuntimeSafe()) return;
    if (!s_capturePointer.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(s_commandMutex);
    float dx = CPad::NewMouseControllerState.x;
    float dy = CPad::NewMouseControllerState.y;
#if defined(GTAVC)
    // The game applies these helper fields to the opposite named axes
    if (MousePointerStateHelper.invertV) dx = -dx;
    if (MousePointerStateHelper.invertH) dy = -dy;
#elif defined(GTA3)
    if (MousePointerStateHelper.m_bInvertHorizontally) dx = -dx;
    if (MousePointerStateHelper.m_bInvertVertically) dy = -dy;
#elif defined(GTASA)
    if (CMenuManager::bInvertMouseX) dx = -dx;
    if (CMenuManager::bInvertMouseY) dy = -dy;
#endif
    s_pointerX += dx;
    s_pointerY += dy;
    CPad::NewMouseControllerState.x = 0.0f;
    CPad::NewMouseControllerState.y = 0.0f;
}

bool SetHealth(Targeting::Kind kind, EntityId id, float health) {
    if (!RuntimeGuard::IsRuntimeSafe() || !id || !std::isfinite(health)) return false;
    const bool repair = health >= 1000.0f;
    const int ref = static_cast<int>(id.value - 1u);
    const auto limit = std::find_if(s_healthLimits.begin(), s_healthLimits.end(), [&](const HealthLimit& item) {
        return item.target.kind == kind && item.target.id.value == id.value;
    });
    if (limit != s_healthLimits.end()) health = std::min(health, limit->maximum);
    if (kind == Targeting::Kind::Ped) {
        CPed* ped = CPools::GetPed(ref);
        if (!ped || ped == FindPlayerPed()) return false;
        ped->m_fHealth = health;
        return true;
    }
    CVehicle* vehicle = CPools::GetVehicle(ref);
    if (!vehicle) return false;
    if (repair) {
#if defined(GTASA)
        vehicle->Fix();
#else
        if (CModelInfo::IsCarModel(vehicle->m_nModelIndex)) {
            static_cast<CAutomobile*>(vehicle)->Fix();
        }
#endif
    }
    vehicle->m_fHealth = health;
    return true;
}

void ClearEffects() {
    s_healthLimits.clear();
    s_vehicleTransfer = {};
}

void ProcessEffects() {
    if (!RuntimeGuard::IsRuntimeSafe()) {
        s_vehicleTransfer = {};
        return;
    }
    if (s_vehicleTransfer.remaining > 0) {
        const VehicleTransfer transfer = s_vehicleTransfer;
        Targeting::Target current = transfer.target;
        CPlayerPed* player = FindPlayerPed();
        CVehicle* vehicle = CPools::GetVehicle(static_cast<int>(current.id.value - 1u));
        if (IsInVehicle(player) && player->m_pVehicle == vehicle
            && vehicle->m_nModelIndex == current.modelId) {
            RestoreVehicleCamera();
            s_vehicleTransfer = {};
        } else if (!Refresh(current) || !SameTarget(current, transfer.target)) {
            s_vehicleTransfer = {};
        } else {
            ExecuteBinding(current, transfer.binding);
        }
        if (s_vehicleTransfer.remaining > 0 && --s_vehicleTransfer.remaining == 0) {
            s_vehicleTransfer = {};
        }
    }
    s_healthLimits.erase(std::remove_if(s_healthLimits.begin(), s_healthLimits.end(), [](HealthLimit& limit) {
        Targeting::Target current = limit.target;
        if (!Refresh(current) || !SameTarget(current, limit.target)) return true;
        if (current.health <= limit.maximum) return false;
        const int ref = static_cast<int>(current.id.value - 1u);
        if (current.kind == Targeting::Kind::Ped) CPools::GetPed(ref)->m_fHealth = limit.maximum;
        else CPools::GetVehicle(ref)->m_fHealth = limit.maximum;
        return false;
    }), s_healthLimits.end());
}

bool ExecuteBinding(const Targeting::Target& target, const Targeting::Binding& binding) {
    using Targeting::Action;
    if (!RuntimeGuard::IsRuntimeSafe() || !target.id || !std::isfinite(binding.value)) return false;
    const auto actions = Targeting::GetActions(target.kind);
    const auto info = std::find_if(actions.begin(), actions.end(), [&](const Targeting::ActionInfo& item) {
        return item.action == binding.action && item.supported;
    });
    if (info == actions.end() || binding.action == Action::None) return false;
    CPlayerPed* player = FindPlayerPed();
    if (!player) return false;
    const int ref = static_cast<int>(target.id.value - 1u);
    CPed* ped = target.kind == Targeting::Kind::Ped ? CPools::GetPed(ref) : nullptr;
    CVehicle* vehicle = target.kind == Targeting::Kind::Vehicle ? CPools::GetVehicle(ref) : nullptr;
    if (ped ? !IsValidPed(ped, player) : !IsValidVehicle(vehicle, player)) return false;
    CPhysical* entity = ped ? static_cast<CPhysical*>(ped) : static_cast<CPhysical*>(vehicle);
    if (entity->m_nModelIndex != target.modelId) return false;
    const float value = std::clamp(binding.value, info->minimum, info->maximum);
    const bool enabled = binding.enabled;
    switch (binding.action) {
    case Action::Restore: {
        const auto limit = std::find_if(s_healthLimits.begin(), s_healthLimits.end(), [&](const HealthLimit& item) {
            return SameTarget(item.target, target);
        });
        if (vehicle) {
            if (!SetHealth(target.kind, target.id, 1000.0f)) return false;
            if (limit != s_healthLimits.end()) vehicle->m_fHealth = limit->maximum;
            return true;
        }
        ped->m_fHealth = limit != s_healthLimits.end() ? limit->maximum : 100.0f;
        return true;
    }
    case Action::Kill:
        ped->m_fHealth = 0.0f;
        return true;
    case Action::Ignite:
        vehicle->bFireProof = false;
        vehicle->m_fHealth = 249.0f;
        return true;
    case Action::Health:
    case Action::MaxHealth: {
        const auto old = std::find_if(s_healthLimits.begin(), s_healthLimits.end(), [&](const HealthLimit& limit) {
            return SameTarget(limit.target, target);
        });
        const float health = binding.action == Action::Health && old != s_healthLimits.end()
            ? std::min(value, old->maximum) : value;
        if (binding.action == Action::MaxHealth) {
            if (old != s_healthLimits.end()) old->maximum = value;
            else s_healthLimits.push_back({target, value});
#if defined(GTASA)
            if (ped) ped->m_fMaxHealth = value;
#endif
        }
        if (ped) ped->m_fHealth = health;
        else vehicle->m_fHealth = health;
        return true;
    }
    case Action::Armour: ped->m_fArmour = value; return true;
    case Action::Disarm: DisarmPed(ped); return true;
    case Action::Upright: return ExecuteAction(target, 4);
    case Action::Unlock: return ExecuteAction(target, 5);
    case Action::Bring: return ExecuteAction(target, 6);
    case Action::Teleport: return ExecuteAction(target, 7);
    case Action::Stop:
        entity->m_vecMoveSpeed = {};
        entity->m_vecTurnSpeed = {};
        return true;
    case Action::Freeze:
        if (ped) plugin::Command<plugin::Commands::FREEZE_CHAR_POSITION>(ref, enabled);
        else plugin::Command<plugin::Commands::FREEZE_CAR_POSITION>(ref, enabled);
        return true;
    case Action::Delete:
        if (ped) plugin::Command<plugin::Commands::DELETE_CHAR>(ref);
        else plugin::Command<plugin::Commands::DELETE_CAR>(ref);
        return true;
    case Action::Visible: entity->bIsVisible = enabled; return true;
    case Action::Proofs:
        entity->bBulletProof = enabled;
        entity->bFireProof = enabled;
        entity->bExplosionProof = enabled;
        entity->bCollisionProof = enabled;
        entity->bMeleeProof = enabled;
        return true;
    case Action::BulletProof: entity->bBulletProof = enabled; return true;
    case Action::FireProof: entity->bFireProof = enabled; return true;
    case Action::ExplosionProof: entity->bExplosionProof = enabled; return true;
    case Action::CollisionProof: entity->bCollisionProof = enabled; return true;
    case Action::MeleeProof: entity->bMeleeProof = enabled; return true;
    case Action::Colors:
        vehicle->m_nPrimaryColor = static_cast<unsigned char>(value);
        vehicle->m_nSecondaryColor = static_cast<unsigned char>(binding.secondary);
#if defined(GTASA)
        vehicle->m_nTertiaryColor = static_cast<unsigned char>(binding.tertiary);
        vehicle->m_nQuaternaryColor = static_cast<unsigned char>(binding.quaternary);
#endif
        return true;
    case Action::Explode:
#if defined(GTASA)
        vehicle->BlowUpCar(player, false);
#else
        vehicle->BlowUpCar(player);
#endif
        return true;
    case Action::Lock:
#if defined(GTA3)
        vehicle->m_eDoorLock = CARLOCK_LOCKED_PLAYER_INSIDE;
#else
        vehicle->m_eDoorLock = DOORLOCK_LOCKED_PLAYER_INSIDE;
#endif
        return true;
    case Action::Engine:
        vehicle->bEngineOn = enabled;
#if defined(GTASA)
        vehicle->bEngineBroken = !enabled;
#endif
        return true;
    case Action::Lights: vehicle->bLightsOn = enabled; return true;
    case Action::Speed:
        vehicle->m_vecMoveSpeed = vehicle->GetForward() * (value / 180.0f);
        return true;
    case Action::Weapon: {
        const auto type = static_cast<eWeaponType>(static_cast<int>(value));
#if defined(GTASA)
        CWeaponInfo* weapon = CWeaponInfo::GetWeaponInfo(type, 1);
        const int model = weapon ? weapon->m_nModelId : -1;
#else
        CWeaponInfo* weapon = CWeaponInfo::GetWeaponInfo(type);
        const int model = weapon ? weapon->m_nModelId : -1;
#endif
        if (model <= 0) return false;
        CStreaming::RequestModel(model, PRIORITY_REQUEST);
        CStreaming::LoadAllRequestedModels(false);
        if (!CStreaming::HasModelLoaded(model)) return false;
        const unsigned int ammo = static_cast<unsigned int>(std::clamp(binding.secondary, 0, 99999));
#if defined(GTA3)
        ped->GiveWeapon(type, ammo);
#else
        ped->GiveWeapon(type, ammo, false);
#endif
        plugin::Command<plugin::Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(model);
        return true;
    }
    case Action::EnterVehicle:
    case Action::WarpToSeat: {
        const bool replaceDriver = binding.action == Action::EnterVehicle;
        const int seat = replaceDriver ? 0 : static_cast<int>(value);
        if (seat > vehicle->m_nMaxPassengers) return false;
        if (seat == 0 && vehicle->m_pDriver && !replaceDriver) return false;
#if defined(GTAVC)
        if (seat > 0 && vehicle->m_passengers[seat - 1]) return false;
#else
        if (seat > 0 && vehicle->m_apPassengers[seat - 1]) return false;
#endif
        const int playerRef = CPools::GetPedRef(player);
        const auto waitForTransfer = [&] {
            if (s_vehicleTransfer.remaining == 0) {
                s_vehicleTransfer = {target, binding, kVehicleTransferFrames};
            }
            return true;
        };
        if (IsInVehicle(player)) {
            const CVector position = vehicle->TransformFromObjectSpace(CVector(-3.0f, 0.0f, 0.5f));
            plugin::Command<plugin::Commands::WARP_CHAR_FROM_CAR_TO_COORD>(
                playerRef, position.x, position.y, position.z);
            return waitForTransfer();
        }
        if (replaceDriver && vehicle->m_pDriver) {
            CPed* driver = vehicle->m_pDriver;
            const CVector position = vehicle->TransformFromObjectSpace(CVector(3.0f, 0.0f, 0.5f));
            plugin::Command<plugin::Commands::WARP_CHAR_FROM_CAR_TO_COORD>(
                CPools::GetPedRef(driver), position.x, position.y, position.z);
            return waitForTransfer();
        }
        if (replaceDriver) {
#if defined(GTA3)
            vehicle->m_eDoorLock = CARLOCK_UNLOCKED;
#else
            vehicle->m_eDoorLock = DOORLOCK_UNLOCKED;
#endif
        }
        if (seat == 0) plugin::Command<plugin::Commands::WARP_CHAR_INTO_CAR>(playerRef, ref);
        else plugin::Command<plugin::Commands::WARP_CHAR_INTO_CAR_AS_PASSENGER>(playerRef, ref, seat - 1);
        const bool entered = IsInVehicle(player) && player->m_pVehicle == vehicle
            && (seat != 0 || vehicle->m_pDriver == player);
        if (entered) {
            RestoreVehicleCamera();
            s_vehicleTransfer = {};
        }
        return entered;
    }
    case Action::Heavy:
        plugin::Command<plugin::Commands::SET_CAR_HEAVY>(ref, enabled);
        return true;
    case Action::Watertight:
        plugin::Command<plugin::Commands::SET_CAR_WATERTIGHT>(ref, enabled);
        return true;
#if defined(GTASA)
    case Action::OpenDoor:
        plugin::Command<plugin::Commands::OPEN_CAR_DOOR>(ref, static_cast<int>(value));
        return true;
    case Action::PopDoor:
        plugin::Command<plugin::Commands::POP_CAR_DOOR>(ref, static_cast<int>(value), false);
        return true;
    case Action::SkidMarks: vehicle->bAlwaysSkidMarks = enabled; return true;
    case Action::Particles: vehicle->bDisableParticles = !enabled; return true;
    case Action::DriverTargetable: vehicle->bVehicleCanBeTargetted = enabled; return true;
    case Action::HeatSeekingTargetable: vehicle->bVehicleCanBeTargettedByHS = enabled; return true;
    case Action::PetrolTankWeakPoint: vehicle->bPetrolTankIsWeakPoint = enabled; return true;
    case Action::Siren: vehicle->bSirenOrAlarm = enabled; return true;
    case Action::TakeLessDamage: vehicle->bTakeLessDamage = enabled; return true;
    case Action::Paintjob: vehicle->SetRemap(static_cast<int>(value)); return true;
    case Action::AddUpgrade: {
        const int model = static_cast<int>(value);
        if (!CModelInfo::GetModelInfo(model)) return false;
        CStreaming::RequestModel(model, PRIORITY_REQUEST);
        CStreaming::LoadAllRequestedModels(false);
        if (!CStreaming::HasModelLoaded(model)) return false;
        vehicle->AddVehicleUpgrade(model);
        plugin::Command<plugin::Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(model);
        return true;
    }
    case Action::RemoveUpgrade: vehicle->RemoveVehicleUpgrade(static_cast<int>(value)); return true;
    case Action::RemoveAllUpgrades: vehicle->RemoveAllUpgrades(); return true;
#else
    case Action::OpenDoor:
        Detail::VehicleBackend::OpenDoor(vehicle, static_cast<int>(value));
        return true;
#endif
    default: return false;
    }
}

bool ExecuteAction(const Targeting::Target& target, int action) {
    if (!RuntimeGuard::IsRuntimeSafe() || !target.id) return false;
    CPlayerPed* player = FindPlayerPed();
    if (!player) return false;
    const int ref = static_cast<int>(target.id.value - 1u);
    CPed* ped = target.kind == Targeting::Kind::Ped ? CPools::GetPed(ref) : nullptr;
    CVehicle* vehicle = target.kind == Targeting::Kind::Vehicle ? CPools::GetVehicle(ref) : nullptr;
    if (ped ? !IsValidPed(ped, player) : !IsValidVehicle(vehicle, player)) return false;
    CPhysical* entity = ped ? static_cast<CPhysical*>(ped) : static_cast<CPhysical*>(vehicle);
    if (entity->m_nModelIndex != target.modelId) return false;
    if (action == 4) {
        if (ped) ped->m_fArmour = 100.0f;
        else {
            const float heading = vehicle->GetHeading();
            vehicle->SetOrientation(0.0f, 0.0f, heading);
            vehicle->m_vecMoveSpeed = {};
            vehicle->m_vecTurnSpeed = {};
        }
    } else if (action == 5) {
        if (ped) DisarmPed(ped);
#if defined(GTA3)
        else vehicle->m_eDoorLock = CARLOCK_UNLOCKED;
#else
        else vehicle->m_eDoorLock = DOORLOCK_UNLOCKED;
#endif
    } else if (action == 6 || action == 7) {
        CPhysical* moving = action == 6
            ? ped && ped->m_pVehicle ? static_cast<CPhysical*>(ped->m_pVehicle) : entity
            : static_cast<CPhysical*>(player);
        if (action == 7 && player->m_pVehicle) return false;
        const CVector destination = action == 6
            ? player->TransformFromObjectSpace(CVector(3.0f, 4.0f, 0.5f))
            : entity->TransformFromObjectSpace(CVector(3.0f, 0.0f, 0.5f));
        if (action == 6 && (vehicle || (ped && ped->m_pVehicle))) {
            CVehicle* car = vehicle ? vehicle : ped->m_pVehicle;
            plugin::Command<plugin::Commands::SET_CAR_COORDINATES>(
                CPools::GetVehicleRef(car), destination.x, destination.y, destination.z);
        } else {
            CPed* character = action == 6 ? ped : player;
            plugin::Command<plugin::Commands::SET_CHAR_COORDINATES>(
                CPools::GetPedRef(character), destination.x, destination.y, destination.z);
        }
        moving->m_vecMoveSpeed = {};
        moving->m_vecTurnSpeed = {};
    } else return false;
    return true;
}

bool Delete(Targeting::Kind kind, EntityId id) {
    if (!id) return false;
    const int ref = static_cast<int>(id.value - 1u);
    if (kind == Targeting::Kind::Ped) {
        CPed* ped = CPools::GetPed(ref);
        if (!ped || ped == FindPlayerPed()) return false;
        ped->m_fHealth = 0.0f;
        return true;
    }
    CVehicle* vehicle = CPools::GetVehicle(ref);
    CPlayerPed* player = FindPlayerPed();
    if (!vehicle || (player && vehicle == player->m_pVehicle)) return false;
    vehicle->m_fHealth = 0.0f;
    return true;
}

void SetLabels(const XBase::Targeting::Labels& labels) {
    s_labels = labels;
}

} // namespace XBase::Detail::TargetingBackend
