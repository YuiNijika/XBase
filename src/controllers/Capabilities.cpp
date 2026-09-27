#include <XBase/Capabilities.h>

namespace XBase {

CapabilitySupport GetCapabilitySupport(Capability capability) {
#if defined(XBASE_BACKEND_SA)
    switch (capability) {
    case Capability::Player:
    case Capability::Vehicle:
    case Capability::Weapon:
    case Capability::Visual:
    case Capability::Teleport:
    case Capability::Camera:
    case Capability::Cheats:
    case Capability::VehicleEffects:
    case Capability::Hooks:
    case Capability::Ui:
    case Capability::WebView:
    case Capability::Panel:
        return CapabilitySupport::Supported;
    case Capability::Ped:
    case Capability::World:
    case Capability::Scene:
    case Capability::Overlay:
    case Capability::BulletAssist:
        return CapabilitySupport::Partial;
    }
#elif defined(XBASE_BACKEND_VC)
    switch (capability) {
    case Capability::Player:
    case Capability::Ped:
    case Capability::Vehicle:
    case Capability::Weapon:
    case Capability::World:
    case Capability::Visual:
    case Capability::Teleport:
    case Capability::BulletAssist:
    case Capability::Cheats:
    // 任务相关的场景接口在这两个版本已有实现，粗粒度要与细粒度的 SceneMission 一致
    case Capability::Scene:
    // 覆盖层已改成走 ImGui 画布的实现，三个版本共用同一份代码
    case Capability::Overlay:
        return CapabilitySupport::Partial;
    case Capability::Hooks:
    case Capability::Ui:
    case Capability::WebView:
    case Capability::Panel:
        return CapabilitySupport::Supported;
    default:
        return CapabilitySupport::Unsupported;
    }
#elif defined(XBASE_BACKEND_III)
    switch (capability) {
    case Capability::Player:
    case Capability::Ped:
    case Capability::Vehicle:
    case Capability::Weapon:
    case Capability::World:
    case Capability::Visual:
    case Capability::Teleport:
    case Capability::Cheats:
    // 任务相关的场景接口在这个版本已有实现，粗粒度要与细粒度的 SceneMission 一致
    case Capability::Scene:
    // 覆盖层已改成走 ImGui 画布的实现，三个版本共用同一份代码
    case Capability::Overlay:
    // 边界框这类只读显示不需要挂钩开火函数，这个版本可用
    case Capability::BulletAssist:
        return CapabilitySupport::Partial;
    case Capability::Hooks:
    case Capability::Ui:
    case Capability::WebView:
    case Capability::Panel:
        return CapabilitySupport::Supported;
    default:
        return CapabilitySupport::Unsupported;
    }
#else
    (void)capability;
#endif
    return CapabilitySupport::Unsupported;
}

CapabilitySupport GetCapabilitySupport(FeatureCapability capability) {
#if defined(XBASE_BACKEND_SA)
    switch (capability) {
    case FeatureCapability::PlayerBasicState:
    case FeatureCapability::PlayerRuntimeEffects:
    case FeatureCapability::PlayerProofs:
    case FeatureCapability::PlayerMovement:
    case FeatureCapability::PlayerAppearance:
    case FeatureCapability::PlayerClothes:
    case FeatureCapability::PlayerStats:
    case FeatureCapability::PlayerSuperJump:
    case FeatureCapability::PlayerSuperPunch:
    case FeatureCapability::PlayerUnderwaterBreathing:
    case FeatureCapability::PlayerCycleJump:
    case FeatureCapability::PlayerNeverHungry:
    case FeatureCapability::PlayerFastSprint:
    case FeatureCapability::PlayerSprintEverywhere:
    case FeatureCapability::PlayerDrunkEffect:
    case FeatureCapability::PlayerNeverWanted:
    case FeatureCapability::PlayerAimSkinChanger:
    case FeatureCapability::PlayerKeepStuff:
    case FeatureCapability::PlayerSaveGame:
    case FeatureCapability::PedBasic:
    case FeatureCapability::PedSpawn:
    case FeatureCapability::PedDelete:
    case FeatureCapability::PedAttributes:
    case FeatureCapability::PedClassification:
    case FeatureCapability::PedBigHead:
    case FeatureCapability::PedThinBody:
    case FeatureCapability::PedSmokeFlies:
    case FeatureCapability::VehicleBasic:
    case FeatureCapability::VehicleColors:
    case FeatureCapability::VehicleDoors:
    case FeatureCapability::VehicleSpawn:
    case FeatureCapability::VehicleSpawnSession:
    case FeatureCapability::VehicleDelete:
    case FeatureCapability::VehicleEvents:
    case FeatureCapability::VehiclePopDoors:
    case FeatureCapability::VehicleAlwaysSkidMarks:
    case FeatureCapability::VehicleDisableParticles:
    case FeatureCapability::VehicleDriverTargetable:
    case FeatureCapability::VehicleHeatSeekingTargetable:
    case FeatureCapability::VehiclePetrolTankWeakPoint:
    case FeatureCapability::VehicleSirenOrAlarm:
    case FeatureCapability::VehicleTakeLessDamage:
    case FeatureCapability::VehicleTrafficDensity:
    case FeatureCapability::VehicleAutoDrive:
    case FeatureCapability::VehiclePaintjob:
    case FeatureCapability::VehicleUpgrades:
    case FeatureCapability::VehicleCheats:
    case FeatureCapability::WorldTime:
    case FeatureCapability::WorldWeather:
    case FeatureCapability::WorldWeatherEffects:
    case FeatureCapability::WorldGravity:
    case FeatureCapability::WorldGameSpeed:
    case FeatureCapability::WorldFpsLimit:
    case FeatureCapability::WorldDaysPassed:
    case FeatureCapability::WorldFreezeTime:
    case FeatureCapability::WorldFasterClock:
    case FeatureCapability::WorldDisableReplay:
    case FeatureCapability::WorldDisableCheats:
    case FeatureCapability::WorldForbiddenAreaWanted:
    case FeatureCapability::WorldFreePayNSpray:
    case FeatureCapability::WorldNoWaterPhysics:
    case FeatureCapability::WorldSolidWater:
    case FeatureCapability::WorldPickups:
    case FeatureCapability::WeaponBasic:
    case FeatureCapability::WeaponGive:
    case FeatureCapability::WeaponDrop:
    case FeatureCapability::WeaponSkills:
    case FeatureCapability::WeaponRuntimeEffects:
    case FeatureCapability::WeaponStatOverrides:
    case FeatureCapability::SceneAnimation:
    case FeatureCapability::SceneMission:
    case FeatureCapability::TeleportBasic:
    case FeatureCapability::VisualHudRadar:
    case FeatureCapability::VisualFilter:
    case FeatureCapability::VisualRadarOptions:
    case FeatureCapability::BulletAssistTracking:
    case FeatureCapability::BulletAssistThroughWalls:
    case FeatureCapability::BulletAssistHardLock:
    case FeatureCapability::BulletAssistPedBounds:
    case FeatureCapability::BulletAssistVehicleBounds:
        return CapabilitySupport::Supported;
    case FeatureCapability::PedMarkerSpawn:
    case FeatureCapability::PedGlobalStrategies:
    case FeatureCapability::SceneParticle:
    case FeatureCapability::SceneCutscene:
    case FeatureCapability::BulletAssistPedCollision:
    case FeatureCapability::BulletAssistPedSkeleton:
    case FeatureCapability::BulletAssistVehicleCollision:
    case FeatureCapability::BulletAssistFireSuppression:
        return CapabilitySupport::Partial;
    case FeatureCapability::CameraFreecam:
    case FeatureCapability::CameraTopDown:
    case FeatureCapability::CheatsRandom:
    case FeatureCapability::VehicleEffectsNeon:
        return CapabilitySupport::Supported;
    }
#elif defined(XBASE_BACKEND_VC)
    switch (capability) {
    case FeatureCapability::PlayerBasicState:
    case FeatureCapability::PlayerProofs:
    case FeatureCapability::PlayerMovement:
    case FeatureCapability::PlayerSaveGame:
    case FeatureCapability::PedBasic:
    case FeatureCapability::PedSpawn:
    case FeatureCapability::PedDelete:
    case FeatureCapability::PedAttributes:
    case FeatureCapability::PedClassification:
    case FeatureCapability::VehicleSpawn:
    case FeatureCapability::VehicleSpawnSession:
    case FeatureCapability::VehicleDelete:
    case FeatureCapability::VehicleEvents:
    case FeatureCapability::BulletAssistTracking:
    case FeatureCapability::BulletAssistThroughWalls:
    case FeatureCapability::BulletAssistHardLock:
    case FeatureCapability::BulletAssistPedBounds:
    case FeatureCapability::BulletAssistVehicleBounds:
    case FeatureCapability::WorldTime:
    case FeatureCapability::WorldWeather:
    case FeatureCapability::WorldGravity:
    case FeatureCapability::WorldGameSpeed:
    case FeatureCapability::WorldFpsLimit:
    case FeatureCapability::WorldDaysPassed:
    case FeatureCapability::WorldFreezeTime:
    case FeatureCapability::WorldFasterClock:
    case FeatureCapability::WorldDisableReplay:
    case FeatureCapability::WorldDisableCheats:
    case FeatureCapability::WeaponBasic:
    case FeatureCapability::WeaponGive:
    case FeatureCapability::WeaponDrop:
    case FeatureCapability::WeaponRuntimeEffects:
    case FeatureCapability::WeaponStatOverrides:
    case FeatureCapability::TeleportBasic:
    case FeatureCapability::VisualHudRadar:
    case FeatureCapability::VisualFilter:
        return CapabilitySupport::Supported;
    case FeatureCapability::PlayerRuntimeEffects:
    case FeatureCapability::VehicleBasic:
    case FeatureCapability::VehicleColors:
    case FeatureCapability::VehicleCheats:
    case FeatureCapability::BulletAssistPedCollision:
    case FeatureCapability::BulletAssistPedSkeleton:
    case FeatureCapability::BulletAssistVehicleCollision:
    case FeatureCapability::BulletAssistFireSuppression:
    case FeatureCapability::SceneMission:
    case FeatureCapability::WorldPickups:
        return CapabilitySupport::Partial;
    default:
        return CapabilitySupport::Unsupported;
    }
#elif defined(XBASE_BACKEND_III)
    switch (capability) {
    case FeatureCapability::PlayerBasicState:
    case FeatureCapability::PlayerProofs:
    case FeatureCapability::PlayerMovement:
    case FeatureCapability::PlayerSaveGame:
    case FeatureCapability::PedBasic:
    case FeatureCapability::PedSpawn:
    case FeatureCapability::PedDelete:
    case FeatureCapability::PedAttributes:
    case FeatureCapability::PedClassification:
    case FeatureCapability::PedBigHead:
    case FeatureCapability::VehicleSpawn:
    case FeatureCapability::VehicleSpawnSession:
    case FeatureCapability::VehicleDelete:
    case FeatureCapability::VehicleEvents:
    case FeatureCapability::WorldTime:
    case FeatureCapability::WorldWeather:
    case FeatureCapability::WorldGravity:
    case FeatureCapability::WorldGameSpeed:
    case FeatureCapability::WorldFpsLimit:
    case FeatureCapability::WorldDaysPassed:
    case FeatureCapability::WorldFreezeTime:
    case FeatureCapability::WorldFasterClock:
    case FeatureCapability::WorldDisableReplay:
    case FeatureCapability::WorldDisableCheats:
    case FeatureCapability::WeaponBasic:
    case FeatureCapability::WeaponGive:
    case FeatureCapability::WeaponDrop:
    case FeatureCapability::WeaponRuntimeEffects:
    case FeatureCapability::WeaponStatOverrides:
    case FeatureCapability::TeleportBasic:
    case FeatureCapability::VisualHudRadar:
    case FeatureCapability::VisualFilter:
    // 透视边界框只依赖对象池与世界转屏幕，不需要挂钩
    case FeatureCapability::BulletAssistPedBounds:
    case FeatureCapability::BulletAssistVehicleBounds:
        return CapabilitySupport::Supported;
    case FeatureCapability::PlayerRuntimeEffects:
    case FeatureCapability::VehicleBasic:
    case FeatureCapability::VehicleColors:
    case FeatureCapability::VehicleCheats:
    case FeatureCapability::SceneMission:
    case FeatureCapability::WorldPickups:
        return CapabilitySupport::Partial;
    default:
        return CapabilitySupport::Unsupported;
    }
#else
    (void)capability;
#endif
    return CapabilitySupport::Unsupported;
}

bool HasCapability(Capability capability) {
    return GetCapabilitySupport(capability) != CapabilitySupport::Unsupported;
}

bool HasCapability(FeatureCapability capability) {
    return GetCapabilitySupport(capability) != CapabilitySupport::Unsupported;
}

} // namespace XBase
