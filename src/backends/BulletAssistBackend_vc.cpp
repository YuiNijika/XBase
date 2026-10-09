#include "BulletAssistBackend.h"
#include "PedBackend.h"
#include "RuntimeGuard.h"
#include "common.h"

#include <XBase/Core.h>
#include <XBase/Ped.h>
#include <XBase/Hooks.h>
#include "CCamera.h"
#include "CColModel.h"
#include "CColPoint.h"
#include "CEntity.h"
#include "CModelInfo.h"
#include "CPad.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CSprite.h"
#include "CTimer.h"
#include "CVector.h"
#include "CVehicle.h"
#include "CWeapon.h"
#include "eObjective.h"
#include "ePedStates.h"
#include "ePedType.h"
#include "eVehicleType.h"
#include "imgui.h"
#include "kiero/minhook/MinHook.h"
#include "RenderWare.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace XBase::Detail::BulletAssistBackend {
namespace {
using ProcessLineOfSightFn = bool(__cdecl*)(
    const CVector&, const CVector&, CColPoint&, CEntity*&,
    bool, bool, bool, bool, bool, bool, bool, bool);
using FireFn = bool(__thiscall*)(CWeapon*, CEntity*, CVector*);
using FireInstantHitFn = bool(__thiscall*)(CWeapon*, CEntity*, CVector*);
using FireInstantHitFromCarFn = bool(__thiscall*)(CWeapon*, CVehicle*, bool, bool);

constexpr std::uintptr_t kFireAddress = 0x5D45E0;
constexpr std::uintptr_t kFireInstantHitAddress = 0x5D1140;
constexpr std::uintptr_t kFireInstantHitFromCarAddress = 0x5CB0A0;
constexpr std::uintptr_t kProcessLineOfSightAddress = 0x4D92D0;
constexpr float kPi = 3.14159265f;
constexpr float kMaxPitchUp = 1.05f;
constexpr float kMaxPitchDown = 1.49f;

ProcessLineOfSightFn s_originalProcessLineOfSight = nullptr;
FireFn s_originalFire = nullptr;
FireInstantHitFn s_originalFireInstantHit = nullptr;
FireInstantHitFromCarFn s_originalFireInstantHitFromCar = nullptr;
BulletAssist::Config s_config;
std::atomic<unsigned int> s_inFlight{0};
std::atomic<bool> s_stopping{false};
bool s_ownsFire = false;
bool s_ownsFireInstantHit = false;
bool s_ownsFireInstantHitFromCar = false;
bool s_ownsProcessLineOfSight = false;
int s_fireDepth = 0;

struct Candidate {
    CPed* ped = nullptr;
    CVehicle* vehicle = nullptr;
    CVector position{};
    float score = 0.0f;
    int reference = -1;
};

std::vector<Candidate> s_candidates;
CVector s_shotTarget{};
bool s_hasShotTarget = false;
CPed* s_hardLockPed = nullptr;

struct CallbackScope {
    bool active = false;
    CallbackScope() {
        if (s_stopping.load(std::memory_order_acquire)) return;
        s_inFlight.fetch_add(1, std::memory_order_acq_rel);
        if (s_stopping.load(std::memory_order_acquire)) {
            s_inFlight.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }
        active = true;
    }
    ~CallbackScope() {
        if (active) s_inFlight.fetch_sub(1, std::memory_order_acq_rel);
    }
};

enum class Relation { Civilian, Friend, Hostile, Neutral };

bool IsValidPed(CPed* ped, CPed* player) {
    return ped && ped != player && ped->m_fHealth > 0.0f
        && ped->m_ePedState != PEDSTATE_DEAD
        && ped->m_ePedState != PEDSTATE_DIE;
}

Relation Classify(CPed* ped, CPed* player) {
    if (!ped || ped->m_nPedStatus != 2) return Relation::Civilian;
    if (ped->bScriptPedIsPlayerAlly || ped->m_nPedType == PED_TYPE_GANG_VERCETTI) return Relation::Friend;
    if (ped->m_pThreatEntity == static_cast<CEntity*>(player) || ped->m_nPedType == PED_TYPE_COP) return Relation::Hostile;
    const eObjective objective = ped->m_nObjective;
    if (objective == OBJECTIVE_KILL_CHAR_ON_FOOT
        || objective == OBJECTIVE_KILL_CHAR_ANY_MEANS
        || objective == OBJECTIVE_KILL_CHAR_ON_BOAT
        || objective == OBJECTIVE_GUARD_ATTACK
        || objective == OBJECTIVE_AIM_GUN_AT) return Relation::Hostile;
    return Relation::Neutral;
}

bool IsRelationEnabled(Relation relation) {
    switch (relation) {
    case Relation::Civilian: return s_config.trackCivilian;
    case Relation::Friend: return s_config.trackFriend;
    case Relation::Hostile: return s_config.trackHostile;
    case Relation::Neutral: return s_config.trackNeutral;
    }
    return false;
}

RpAtomic* FindSkinHierarchy(RpAtomic* atomic, void* data) {
    auto** hierarchy = static_cast<RpHAnimHierarchy**>(data);
    *hierarchy = RpSkinAtomicGetHAnimHierarchy(atomic);
    return *hierarchy ? nullptr : atomic;
}

RpHAnimHierarchy* PedHierarchy(CPed* ped) {
    if (!ped || !ped->m_pRwClump) return nullptr;
    RpHAnimHierarchy* hierarchy = nullptr;
    RpClumpForAllAtomics(ped->m_pRwClump, FindSkinHierarchy, &hierarchy);
    return hierarchy;
}

bool BonePosition(CPed* ped, RpHAnimHierarchy* hierarchy, int tag, CVector& position) {
    if (!hierarchy || hierarchy->numNodes <= 0 || hierarchy->numNodes > 128) return false;
    const int index = RpHAnimIDGetIndex(hierarchy, tag);
    if (index < 0 || index >= hierarchy->numNodes) return false;
    const RwMatrix* matrices = RpHAnimHierarchyGetMatrixArray(hierarchy);
    if (!matrices) return false;
    const RwV3d& point = matrices[index].pos;
    position = {point.x, point.y, point.z};
    if (hierarchy->flags & rpHANIMHIERARCHYLOCALSPACEMATRICES) {
        position = ped->TransformFromObjectSpace(position);
    }
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)
        && (position - ped->GetPosition()).MagnitudeSqr() < 64.0f;
}

CVector PedAimPosition(CPed* ped) {
    CVector position = ped->GetPosition();
    RpHAnimHierarchy* hierarchy = PedHierarchy(ped);
    constexpr int headTag = 8;
    constexpr int chestTag = 3;
    constexpr int pelvisTag = 2;
    constexpr int leftKneeTag = 42;
    constexpr int rightKneeTag = 52;
    CVector bone{};
    if (s_config.aimPart == BulletAssist::AimPart::Legs) {
        CVector other{};
        if (BonePosition(ped, hierarchy, leftKneeTag, bone)
            && BonePosition(ped, hierarchy, rightKneeTag, other)) return (bone + other) * 0.5f;
    } else {
        const int tag = s_config.aimPart == BulletAssist::AimPart::Head ? headTag
            : s_config.aimPart == BulletAssist::AimPart::Abdomen ? pelvisTag : chestTag;
        if (BonePosition(ped, hierarchy, tag, bone)) return bone;
    }
    switch (s_config.aimPart) {
    case BulletAssist::AimPart::Head: position.z += 0.82f; break;
    case BulletAssist::AimPart::Abdomen: position.z += 0.42f; break;
    case BulletAssist::AimPart::Legs: position.z += 0.24f; break;
    case BulletAssist::AimPart::Chest:
    default: position.z += 0.58f; break;
    }
    return position;
}

CVector VehicleAimPosition(CVehicle* vehicle) {
    CVector position = vehicle->GetPosition();
    position.z += 0.6f;
    return position;
}

bool CameraDirection(CVector& origin, CVector& direction) {
    const int index = TheCamera.m_nActiveCam;
    if (index < 0 || index > 2) return false;
    const CCam& camera = TheCamera.m_asCams[index];
    origin = camera.m_vecSource;
    direction = camera.m_vecFront;
    const float length = direction.Magnitude();
    if (length < 0.0001f || !std::isfinite(length)) return false;
    direction *= 1.0f / length;
    return true;
}

float Score(const CVector& cameraOrigin, const CVector& cameraDirection,
            const CVector& playerPosition, const CVector& target) {
    CVector delta = target - cameraOrigin;
    const float cameraDistance = delta.Magnitude();
    if (cameraDistance < 0.05f || !std::isfinite(cameraDistance)) return -1.0f;
    delta *= 1.0f / cameraDistance;
    const float affinity = delta.x * cameraDirection.x
        + delta.y * cameraDirection.y + delta.z * cameraDirection.z;
    if (affinity < 0.25f) return -1.0f;
    const float distance = (target - playerPosition).Magnitude();
    if (distance > s_config.lockRange) return -1.0f;
    return affinity * 3.0f - distance / s_config.lockRange * 0.35f;
}

void CollectCandidates() {
    s_candidates.clear();
    CPlayerPed* player = FindPlayerPed();
    if (!player) return;
    CVector cameraOrigin{};
    CVector cameraDirection{};
    if (!CameraDirection(cameraOrigin, cameraDirection)) return;
    const CVector playerPosition = player->GetPosition();

    if (CPools::ms_pPedPool) {
        for (int index = 0; index < CPools::ms_pPedPool->m_nSize; ++index) {
            CPed* ped = CPools::ms_pPedPool->GetAt(index);
            if (!IsValidPed(ped, player) || !IsRelationEnabled(Classify(ped, player))) continue;
            if (!ped->m_pRwClump || ped->m_bInVehicle
                || (ped->GetPosition() - playerPosition).MagnitudeSqr() > s_config.lockRange * s_config.lockRange) continue;
            const CVector target = PedAimPosition(ped);
            const float score = Score(cameraOrigin, cameraDirection, playerPosition, target);
            if (score >= 0.0f) s_candidates.push_back({ped, nullptr, target, score, CPools::GetPedRef(ped)});
        }
    }
    if (CPools::ms_pVehiclePool) {
        for (int index = 0; index < CPools::ms_pVehiclePool->m_nSize; ++index) {
            CVehicle* vehicle = CPools::ms_pVehiclePool->GetAt(index);
            if (!vehicle || !vehicle->m_pRwObject || vehicle->m_fHealth <= 0.0f
                || vehicle == player->m_pVehicle) continue;
            const Relation relation = vehicle->m_pDriver && vehicle->m_pDriver != player
                ? Classify(vehicle->m_pDriver, player) : Relation::Neutral;
            if (!IsRelationEnabled(relation)) continue;
            const CVector target = VehicleAimPosition(vehicle);
            const float score = Score(cameraOrigin, cameraDirection, playerPosition, target);
            if (score >= 0.0f) s_candidates.push_back({nullptr, vehicle, target, score, CPools::GetVehicleRef(vehicle)});
        }
    }

    const std::size_t count = std::min(s_candidates.size(), static_cast<std::size_t>(s_config.maxTargets));
    std::partial_sort(s_candidates.begin(), s_candidates.begin() + count, s_candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.score > right.score;
    });
    s_candidates.resize(count);
}

bool RefreshCandidate(Candidate& candidate) {
    CPlayerPed* player = FindPlayerPed();
    if (!player || candidate.reference < 0) return false;
    if (candidate.ped) {
        if (CPools::GetPed(candidate.reference) != candidate.ped || !IsValidPed(candidate.ped, player)) return false;
        candidate.position = PedAimPosition(candidate.ped);
    } else {
        if (CPools::GetVehicle(candidate.reference) != candidate.vehicle
            || !candidate.vehicle || candidate.vehicle->m_fHealth <= 0.0f
            || candidate.vehicle == player->m_pVehicle) return false;
        candidate.position = VehicleAimPosition(candidate.vehicle);
    }
    return (candidate.position - player->GetPosition()).MagnitudeSqr() <= s_config.lockRange * s_config.lockRange;
}

bool ShotCanReach(const CVector& origin, const Candidate& candidate) {
    if (s_config.throughWalls) return true;
    if (!s_originalProcessLineOfSight) return false;
    CColPoint point{};
    CEntity* entity = nullptr;
    const bool hit = s_originalProcessLineOfSight(origin, candidate.position, point, entity,
        true, true, true, true, true, false, false, false);
    return !hit || entity == candidate.ped || entity == candidate.vehicle;
}

void BeginShot() {
    ++s_fireDepth;
    if (s_fireDepth != 1) return;
    s_hasShotTarget = false;
    if (!s_config.tracking || !RuntimeGuard::IsRuntimeSafe()
        || Hooks::IsMenuVisible() || Hooks::IsKeyboardCaptureActive()) return;
    CVector origin{};
    CVector direction{};
    if (!CameraDirection(origin, direction)) return;
    const auto choose = [&origin](Candidate& candidate) {
        if (!RefreshCandidate(candidate) || !ShotCanReach(origin, candidate)) return false;
        s_shotTarget = candidate.position;
        s_hasShotTarget = true;
        return true;
    };
    for (Candidate& candidate : s_candidates) {
        if (candidate.ped == s_hardLockPed && candidate.ped && choose(candidate)) return;
    }
    for (Candidate& candidate : s_candidates) {
        if (choose(candidate)) return;
    }
}

void EndShot() {
    if (s_fireDepth <= 0) return;
    --s_fireDepth;
    if (s_fireDepth == 0) s_hasShotTarget = false;
}

struct ShotScope {
    bool active = false;
    explicit ShotScope(bool enabled) : active(enabled) { if (active) BeginShot(); }
    ~ShotScope() { if (active) EndShot(); }
};

bool IsLocalPlayer(CEntity* entity) {
    CPlayerPed* player = FindPlayerPed();
    if (!player || !entity) return false;
    return entity == static_cast<CEntity*>(player)
        || (player->m_pVehicle && entity == static_cast<CEntity*>(player->m_pVehicle));
}

CVector ExtendPast(const CVector& origin, const CVector& target) {
    CVector delta = target - origin;
    const float length = delta.Magnitude();
    if (length < 0.05f) return target;
    return origin + delta * ((length + 0.45f) / length);
}

bool __cdecl HookProcessLineOfSight(
    const CVector& origin, const CVector& target, CColPoint& point, CEntity*& entity,
    bool buildings, bool vehicles, bool peds, bool objects, bool dummies,
    bool seeThrough, bool cameraIgnore, bool shootThrough) {
    CallbackScope callback;
    if (!s_originalProcessLineOfSight) return false;
    if (!callback.active || s_fireDepth <= 0 || cameraIgnore || !RuntimeGuard::IsRuntimeSafe()) {
        return s_originalProcessLineOfSight(origin, target, point, entity, buildings, vehicles,
            peds, objects, dummies, seeThrough, cameraIgnore, shootThrough);
    }
    const bool tracked = s_config.tracking && s_hasShotTarget;
    const CVector redirected = tracked ? ExtendPast(origin, s_shotTarget) : target;
    if (tracked) {
        peds = true;
        vehicles = true;
    }
    if (s_config.throughWalls) {
        buildings = false;
        objects = false;
        dummies = false;
    }
    const bool hit = s_originalProcessLineOfSight(origin, redirected, point, entity, buildings, vehicles,
        peds, objects, dummies, seeThrough, cameraIgnore, shootThrough);
    if (hit && (tracked || s_config.throughWalls) && entity
        && entity->m_nType == ENTITY_TYPE_VEHICLE) {
        CVehicle* vehicle = static_cast<CVehicle*>(entity);
        if (vehicle->m_fHealth <= 0.0f) {
            entity = nullptr;
            return false;
        }
    }
    return hit;
}

bool __fastcall HookFire(CWeapon* weapon, void*, CEntity* firingEntity, CVector* source) {
    CallbackScope callback;
    if (!s_originalFire) return false;
    ShotScope shot(callback.active && IsLocalPlayer(firingEntity));
    return s_originalFire(weapon, firingEntity, source);
}

bool __fastcall HookFireInstantHit(CWeapon* weapon, void*, CEntity* firingEntity, CVector* source) {
    CallbackScope callback;
    if (!s_originalFireInstantHit) return false;
    if (callback.active && firingEntity && !IsLocalPlayer(firingEntity)) {
        CPed* ped = static_cast<CPed*>(firingEntity);
        const int reference = CPools::GetPedRef(ped);
        if (reference >= 0 && ShouldSuppressPedFire(
                PedId{static_cast<std::uint32_t>(reference) + 1u}, Ped::GetNoFireOptions())) return false;
    }
    ShotScope shot(callback.active && IsLocalPlayer(firingEntity));
    return s_originalFireInstantHit(weapon, firingEntity, source);
}

bool __fastcall HookFireInstantHitFromCar(
    CWeapon* weapon, void*, CVehicle* vehicle, bool left, bool right) {
    CallbackScope callback;
    if (!s_originalFireInstantHitFromCar) return false;
    CPlayerPed* player = FindPlayerPed();
    if (callback.active && vehicle && vehicle->m_pDriver && vehicle->m_pDriver != player) {
        const int reference = CPools::GetPedRef(vehicle->m_pDriver);
        if (reference >= 0 && ShouldSuppressPedFire(
                PedId{static_cast<std::uint32_t>(reference) + 1u}, Ped::GetNoFireOptions())) return false;
    }
    ShotScope shot(callback.active && player && player->m_pVehicle == vehicle);
    return s_originalFireInstantHitFromCar(weapon, vehicle, left, right);
}

bool InstallHook(std::uintptr_t address, void* detour, void** original, bool& owned) {
    void* target = reinterpret_cast<void*>(address);
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    const MH_STATUS enabled = MH_EnableHook(target);
    owned = enabled == MH_OK || enabled == MH_ERROR_ENABLED;
    if (!owned) MH_RemoveHook(target);
    return owned;
}

void RemoveHook(std::uintptr_t address, bool& owned) {
    if (!owned) return;
    void* target = reinterpret_cast<void*>(address);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    owned = false;
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

float NormalizeAngle(float angle) {
    while (angle > kPi) angle -= 2.0f * kPi;
    while (angle < -kPi) angle += 2.0f * kPi;
    return angle;
}

float LerpAngle(float from, float to, float factor) {
    return from + NormalizeAngle(to - from) * factor;
}

void SetCamFrontFromAngles(CCam& camera) {
    const float cosVertical = std::cos(camera.m_fVerticalAngle);
    const float sinVertical = std::sin(camera.m_fVerticalAngle);
    const float cosHorizontal = std::cos(camera.m_fHorizontalAngle);
    const float sinHorizontal = std::sin(camera.m_fHorizontalAngle);
    camera.m_vecFront = CVector(
        -cosHorizontal * cosVertical, -sinHorizontal * cosVertical, sinVertical);
    camera.m_fAlphaSpeed = 0.0f;
    camera.m_fBetaSpeed = 0.0f;
}

void ApplyHardLockAim(const CVector& worldTarget) {
    const int index = TheCamera.m_nActiveCam;
    if (index < 0 || index > 2) return;
    CCam& camera = TheCamera.m_asCams[index];

    float blend = 0.55f;
    if (CTimer::ms_fTimeStep > 0.0f) {
        blend = std::min(1.0f, 0.35f * CTimer::ms_fTimeStep);
    }
    if (blend < 0.22f) blend = 0.22f;

    ImVec2 screen{};
    if (WorldToScreen(worldTarget, screen)) {
        const float halfWidth = std::max(1.0f, static_cast<float>(RsGlobal.maximumWidth) * 0.5f);
        const float halfHeight = std::max(1.0f, static_cast<float>(RsGlobal.maximumHeight) * 0.5f);
        const float errorX = screen.x - halfWidth;
        const float errorY = screen.y - halfHeight;

        float fovDegrees = camera.m_fFOV;
        if (fovDegrees < 5.0f || fovDegrees > 170.0f || !std::isfinite(fovDegrees)) {
            fovDegrees = 70.0f;
        }
        const float halfFovY = fovDegrees * 0.5f * (kPi / 180.0f);
        const float aspect = halfWidth / halfHeight;
        const float halfFovX = std::atan(std::tan(halfFovY) * aspect);

        const float horizontal = camera.m_fHorizontalAngle - (errorX / halfWidth) * halfFovX;
        const float vertical = std::clamp(
            camera.m_fVerticalAngle - (errorY / halfHeight) * halfFovY,
            -kMaxPitchDown, kMaxPitchUp);

        camera.m_fHorizontalAngle = LerpAngle(camera.m_fHorizontalAngle, horizontal, blend);
        camera.m_fVerticalAngle = LerpAngle(camera.m_fVerticalAngle, vertical, blend);
        SetCamFrontFromAngles(camera);
        return;
    }

    const float deltaX = worldTarget.x - camera.m_vecSource.x;
    const float deltaY = worldTarget.y - camera.m_vecSource.y;
    const float deltaZ = worldTarget.z - camera.m_vecSource.z;
    const float length = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
    if (length < 0.05f || !std::isfinite(length)) return;

    const float inverse = 1.0f / length;
    const float vertical = std::clamp(
        std::asin(std::clamp(deltaZ * inverse, -1.0f, 1.0f)), -kMaxPitchDown, kMaxPitchUp);
    const float horizontal = std::atan2(-deltaY * inverse, -deltaX * inverse);

    camera.m_fHorizontalAngle = LerpAngle(camera.m_fHorizontalAngle, horizontal, blend);
    camera.m_fVerticalAngle = LerpAngle(camera.m_fVerticalAngle, vertical, blend);
    SetCamFrontFromAngles(camera);
}

void ClearHardLock() {
    s_hardLockPed = nullptr;
}

CPed* ResolveHardLockPed() {
    if (s_candidates.empty()) {
        ClearHardLock();
        return nullptr;
    }
    if (s_hardLockPed) {
        for (const Candidate& candidate : s_candidates) {
            if (candidate.ped == s_hardLockPed) return s_hardLockPed;
        }
    }
    for (const Candidate& candidate : s_candidates) {
        if (candidate.ped) {
            s_hardLockPed = candidate.ped;
            return s_hardLockPed;
        }
    }
    ClearHardLock();
    return nullptr;
}

bool PlayerWantsHardLockInput() {
    CPad* pad = CPad::GetPad(0);
    if (!pad) return false;
    if (pad->GetTarget()) return true;
    if (pad->GetWeapon() != 0) return true;
    if (pad->WeaponJustDown()) return true;
    if (CPad::NewMouseControllerState.lmb) return true;
    return false;
}

void ApplyHardLock(const BulletAssist::Config& config) {
    if (Hooks::IsMenuVisible() || Hooks::IsKeyboardCaptureActive()
        || (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0) return;
    if (!config.hardLock || !config.tracking || !PlayerWantsHardLockInput()) {
        if (!config.hardLock || !config.tracking) ClearHardLock();
        return;
    }
    CPed* ped = ResolveHardLockPed();
    if (!ped) return;
    for (const Candidate& candidate : s_candidates) {
        if (candidate.ped == ped) {
            ApplyHardLockAim(candidate.position);
            return;
        }
    }
    ApplyHardLockAim(PedAimPosition(ped));
}

void DrawLine(ImDrawList* drawList, const CVector& from, const CVector& to, ImU32 color) {
    ImVec2 screenFrom{};
    ImVec2 screenTo{};
    if (WorldToScreen(from, screenFrom) && WorldToScreen(to, screenTo)) {
        drawList->AddLine(screenFrom, screenTo, color, 1.4f);
    }
}

void DrawLocalBoxWire(ImDrawList* drawList, CEntity* entity, const CVector& minimum, const CVector& maximum, ImU32 color) {
    if (!entity) return;
    CVector corners[8] = {
        {minimum.x, minimum.y, minimum.z}, {maximum.x, minimum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z}, {minimum.x, maximum.y, minimum.z},
        {minimum.x, minimum.y, maximum.z}, {maximum.x, minimum.y, maximum.z},
        {maximum.x, maximum.y, maximum.z}, {minimum.x, maximum.y, maximum.z},
    };
    for (CVector& corner : corners) corner = entity->TransformFromObjectSpace(corner);
    ImVec2 projected[8]{};
    bool valid[8]{};
    for (int index = 0; index < 8; ++index) valid[index] = WorldToScreen(corners[index], projected[index]);
    constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };
    for (const auto& edge : edges) {
        if (valid[edge[0]] && valid[edge[1]]) drawList->AddLine(projected[edge[0]], projected[edge[1]], color, 1.2f);
    }
}

void DrawBounds(ImDrawList* drawList, CEntity* entity, ImU32 color) {
    CColModel* collision = entity ? entity->GetColModel() : nullptr;
    if (!collision) return;
    DrawLocalBoxWire(drawList, entity, collision->m_boundBox.m_vecMin, collision->m_boundBox.m_vecMax, color);
}

void DrawLocalSphereWire(ImDrawList* drawList, CEntity* entity, const CVector& center, float radius, ImU32 color) {
    if (!entity || radius < 0.01f) return;
    constexpr int segments = 12;
    CVector previousXY{};
    CVector previousXZ{};
    CVector previousYZ{};
    for (int index = 0; index <= segments; ++index) {
        const float angle = kPi * 2.0f * static_cast<float>(index) / static_cast<float>(segments);
        const float cosAngle = std::cos(angle);
        const float sinAngle = std::sin(angle);
        const CVector pointXY = entity->TransformFromObjectSpace(
            CVector(center.x + radius * cosAngle, center.y + radius * sinAngle, center.z));
        const CVector pointXZ = entity->TransformFromObjectSpace(
            CVector(center.x + radius * cosAngle, center.y, center.z + radius * sinAngle));
        const CVector pointYZ = entity->TransformFromObjectSpace(
            CVector(center.x, center.y + radius * cosAngle, center.z + radius * sinAngle));
        if (index > 0) {
            DrawLine(drawList, previousXY, pointXY, color);
            DrawLine(drawList, previousXZ, pointXZ, color);
            DrawLine(drawList, previousYZ, pointYZ, color);
        }
        previousXY = pointXY;
        previousXZ = pointXZ;
        previousYZ = pointYZ;
    }
}

void DrawCollision(ImDrawList* drawList, CEntity* entity, ImU32 boxColor, ImU32 sphereColor) {
    CColModel* collision = entity ? entity->GetColModel() : nullptr;
    if (!collision) return;
    if (collision->m_pBoxes) {
        for (unsigned short index = 0; index < collision->m_nNumBoxes; ++index) {
            DrawLocalBoxWire(drawList, entity, collision->m_pBoxes[index].m_vecMin, collision->m_pBoxes[index].m_vecMax, boxColor);
        }
    }
    if (collision->m_pSpheres) {
        for (unsigned short index = 0; index < collision->m_nNumSpheres; ++index) {
            DrawLocalSphereWire(drawList, entity, collision->m_pSpheres[index].m_vecCenter, collision->m_pSpheres[index].m_fRadius, sphereColor);
        }
    }
}

void DrawSkeleton(ImDrawList* drawList, CPed* ped, ImU32 color) {
    RpHAnimHierarchy* hierarchy = PedHierarchy(ped);
    if (!hierarchy) return;
    // Resolve model bone tags through RenderWare instead of interpreting frame pointers as indices
    constexpr int links[][2] = {
        {2, 3}, {3, 4}, {4, 5}, {5, 8},
        {4, 22}, {22, 23}, {23, 24}, {24, 25},
        {4, 32}, {32, 33}, {33, 34}, {34, 35},
        {2, 41}, {41, 42}, {42, 43}, {43, 44},
        {2, 51}, {51, 52}, {52, 53}, {53, 54},
    };
    for (const auto& link : links) {
        CVector from{};
        CVector to{};
        if (BonePosition(ped, hierarchy, link[0], from) && BonePosition(ped, hierarchy, link[1], to)) {
            DrawLine(drawList, from, to, color);
        }
    }
}

bool ShouldDraw(CEntity* entity, CPed* player, float range) {
    if (!entity || !entity->m_pRwObject) return false;
    if ((entity->GetPosition() - player->GetPosition()).MagnitudeSqr() > range * range) return false;
    ImVec2 screen{};
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    constexpr float margin = 120.0f;
    return WorldToScreen(entity->GetPosition(), screen)
        && screen.x >= -margin && screen.y >= -margin
        && screen.x <= size.x + margin && screen.y <= size.y + margin;
}
}

bool Init() {
    s_stopping.store(false, std::memory_order_release);
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    const bool fire = InstallHook(kFireAddress, reinterpret_cast<void*>(&HookFire),
        reinterpret_cast<void**>(&s_originalFire), s_ownsFire);
    const bool instant = InstallHook(kFireInstantHitAddress, reinterpret_cast<void*>(&HookFireInstantHit),
        reinterpret_cast<void**>(&s_originalFireInstantHit), s_ownsFireInstantHit);
    const bool car = InstallHook(kFireInstantHitFromCarAddress, reinterpret_cast<void*>(&HookFireInstantHitFromCar),
        reinterpret_cast<void**>(&s_originalFireInstantHitFromCar), s_ownsFireInstantHitFromCar);
    const bool line = InstallHook(kProcessLineOfSightAddress, reinterpret_cast<void*>(&HookProcessLineOfSight),
        reinterpret_cast<void**>(&s_originalProcessLineOfSight), s_ownsProcessLineOfSight);
    if (fire && instant && car && line) return true;
    Shutdown();
    return false;
}

void Process(const BulletAssist::Config& config) {
    s_config = config;
    if (!RuntimeGuard::IsRuntimeSafe()) {
        s_candidates.clear();
        s_hasShotTarget = false;
        ClearHardLock();
        return;
    }
    if (s_config.tracking) CollectCandidates();
    else s_candidates.clear();
    ApplyHardLock(s_config);
}

void Shutdown() {
    s_stopping.store(true, std::memory_order_release);
    RemoveHook(kProcessLineOfSightAddress, s_ownsProcessLineOfSight);
    RemoveHook(kFireInstantHitFromCarAddress, s_ownsFireInstantHitFromCar);
    RemoveHook(kFireInstantHitAddress, s_ownsFireInstantHit);
    RemoveHook(kFireAddress, s_ownsFire);
    while (s_inFlight.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    s_originalProcessLineOfSight = nullptr;
    s_originalFire = nullptr;
    s_originalFireInstantHit = nullptr;
    s_originalFireInstantHitFromCar = nullptr;
    s_candidates.clear();
    s_fireDepth = 0;
    s_hasShotTarget = false;
    s_hardLockPed = nullptr;
    s_config = {};
}

void Draw(const BulletAssist::Config& config) {
    if (!Core::IsWorldReady() || !RuntimeGuard::IsRuntimeSafe() || !ImGui::GetCurrentContext()) return;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;
    CPlayerPed* player = FindPlayerPed();
    if (!player) return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    drawList->PushClipRect({0.0f, 0.0f}, size, true);

    if (config.tracking) {
        for (const Candidate& candidate : s_candidates) {
            ImVec2 screen{};
            if (WorldToScreen(candidate.position, screen)) {
                drawList->AddCircle(screen, 8.0f, IM_COL32(255, 80, 80, 230), 16, 1.5f);
            }
        }
    }
    if (CPools::ms_pPedPool && (config.drawPedBounds || config.drawPedCollision || config.drawPedSkeleton)) {
        for (int index = 0; index < CPools::ms_pPedPool->m_nSize; ++index) {
            CPed* ped = CPools::ms_pPedPool->GetAt(index);
            if (!IsValidPed(ped, player) || !ShouldDraw(ped, player, config.lockRange)) continue;
            if (config.drawPedBounds) DrawBounds(drawList, ped, IM_COL32(80, 220, 120, 230));
            if (config.drawPedCollision) DrawCollision(drawList, ped, IM_COL32(60, 180, 255, 220), IM_COL32(120, 200, 255, 200));
            if (config.drawPedSkeleton) DrawSkeleton(drawList, ped, IM_COL32(255, 200, 60, 230));
        }
    }
    if (CPools::ms_pVehiclePool && (config.drawVehicleBounds || config.drawVehicleCollision)) {
        for (int index = 0; index < CPools::ms_pVehiclePool->m_nSize; ++index) {
            CVehicle* vehicle = CPools::ms_pVehiclePool->GetAt(index);
            if (!vehicle || vehicle == player->m_pVehicle || vehicle->m_fHealth <= 0.0f
                || !ShouldDraw(vehicle, player, config.lockRange)) continue;
            if (config.drawVehicleBounds) DrawBounds(drawList, vehicle, IM_COL32(255, 140, 60, 230));
            if (config.drawVehicleCollision) DrawCollision(drawList, vehicle, IM_COL32(255, 90, 90, 220), IM_COL32(255, 160, 120, 200));
        }
    }
    drawList->PopClipRect();
}

namespace {

CPed* ResolvePed(PedId ped) {
    if (!ped || !CPools::ms_pPedPool) return nullptr;
    const int index = static_cast<int>(ped.value - 1u);
    if (index < 0 || index >= CPools::ms_pPedPool->m_nSize) return nullptr;
    CPed* candidate = CPools::ms_pPedPool->GetAt(index);
    return candidate && CPools::GetPedRef(candidate) == index ? candidate : nullptr;
}

} // namespace

bool ShouldSuppressPedFire(PedId ped, const Ped::NoFireOptions& options) {
    CPed* target = ResolvePed(ped);
    if (!target) return false;
    return ShouldSuppressNoFire(
        options, PedBackend::IsMission(target), PedBackend::IsCop(target), PedBackend::IsGang(target));
}

} // namespace XBase::Detail::BulletAssistBackend
