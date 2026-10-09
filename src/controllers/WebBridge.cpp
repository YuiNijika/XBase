#include <XBase/WebBridge.h>

#include <XBase/BulletAssist.h>
#include <XBase/Camera.h>
#include <XBase/Capabilities.h>
#include <XBase/Cheats.h>
#include <XBase/Host.h>
#include <XBase/Log.h>
#include <XBase/Ped.h>
#include <XBase/Player.h>
#include <XBase/Teleport.h>
#include <XBase/Runtime.h>
#include <XBase/Scene.h>
#include <XBase/Types.h>
#include <XBase/Vehicle.h>
#include <XBase/VehicleEffects.h>
#include <XBase/Visual.h>
#include <XBase/WebView.h>
#include <XBase/Weapon.h>
#include <XBase/World.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace {

constexpr int ProtocolVersion = 1;

std::unordered_map<std::string, XBase::WebBridge::MethodHandler> s_customMethods;
std::unordered_set<XBase::WebView::WebViewId> s_installedInstances;

// 页面脚本注入，网页侧用 window.xbase.call 拿 Promise，用 on 订阅原生事件
const char* const ClientScript = R"JS(
(function () {
    if (!window.chrome || !window.chrome.webview) {
        return;
    }

    var pending = new Map();
    var listeners = new Map();
    var nextId = 1;

    window.chrome.webview.addEventListener('message', function (event) {
        var data = event.data;
        if (!data) {
            return;
        }
        if (typeof data.id === 'number' && pending.has(data.id)) {
            var entry = pending.get(data.id);
            pending.delete(data.id);
            if (data.ok) {
                entry.resolve(data.result);
            } else {
                entry.reject(new Error(data.error || 'xbase call failed'));
            }
            return;
        }
        if (typeof data.event === 'string' && listeners.has(data.event)) {
            listeners.get(data.event).forEach(function (fn) {
                fn(data.payload);
            });
        }
    });

    window.xbase = {
        call: function (method, params) {
            return new Promise(function (resolve, reject) {
                var id = nextId++;
                pending.set(id, { resolve: resolve, reject: reject });
                window.chrome.webview.postMessage({ id: id, method: method, params: params || {} });
            });
        },
        on: function (event, fn) {
            if (!listeners.has(event)) {
                listeners.set(event, []);
            }
            listeners.get(event).push(fn);
        },
        capabilities: function () {
            return window.xbase.call('bridge.capabilities');
        }
    };
})();
)JS";

const char* CapabilityName(XBase::CapabilitySupport support) {
    switch (support) {
    case XBase::CapabilitySupport::Supported: return "supported";
    case XBase::CapabilitySupport::Partial: return "partial";
    case XBase::CapabilitySupport::Unsupported:
    default: return "unsupported";
    }
}

void Reply(const XBase::Json::Value& id, const XBase::Json::Value& result) {
    XBase::Json::Value response;
    response.Set("id", id);
    response.Set("ok", true);
    response.Set("result", result);
    if (!XBase::WebView::PostJson(response.Serialize(false))) {
        XBase::Log::Warn("WebBridge: 应答回传失败");
    }
}

void Fail(const XBase::Json::Value& id, const std::string& error) {
    XBase::Json::Value response;
    response.Set("id", id);
    response.Set("ok", false);
    response.Set("error", error);
    XBase::WebView::PostJson(response.Serialize(false));
}

bool RequireCapability(
    const XBase::Json::Value& id,
    XBase::FeatureCapability capability,
    const char* method) {
    if (XBase::GetCapabilitySupport(capability) != XBase::CapabilitySupport::Unsupported) {
        return true;
    }
    Fail(id, std::string("unsupported on this game: ") + method);
    return false;
}

// 单一开关的接口集中在一张表里，能力查询与调用共用同一份定义
struct BoolToggle {
    const char* method;
    XBase::FeatureCapability capability;
    bool (*apply)(bool enable);
};

const BoolToggle kBoolToggles[] = {
    {"player.godMode", XBase::FeatureCapability::PlayerProofs,
     [](bool v) { XBase::Player::SetGodMode(v); return v; }},
    {"player.invisible", XBase::FeatureCapability::PlayerProofs,
     [](bool v) { XBase::Player::SetInvisible(v); return v; }},
    {"player.hardMode", XBase::FeatureCapability::PlayerProofs,
     [](bool v) { XBase::Player::SetHardMode(v); return v; }},
    {"player.freeFly", XBase::FeatureCapability::PlayerRuntimeEffects,
     [](bool v) { XBase::Player::SetFreeFly(v); return v; }},
    {"player.superJump", XBase::FeatureCapability::PlayerSuperJump,
     [](bool v) { return XBase::Player::SuperJump(v); }},
    {"player.superPunch", XBase::FeatureCapability::PlayerSuperPunch,
     [](bool v) { return XBase::Player::SuperPunch(v); }},
    {"player.underwaterBreathing", XBase::FeatureCapability::PlayerUnderwaterBreathing,
     [](bool v) { return XBase::Player::UnderwaterBreathing(v); }},
    {"player.cycleJump", XBase::FeatureCapability::PlayerCycleJump,
     [](bool v) { return XBase::Player::SetCycleJump(v); }},
    {"player.neverHungry", XBase::FeatureCapability::PlayerNeverHungry,
     [](bool v) { return XBase::Player::SetNeverHungry(v); }},
    {"player.fastSprint", XBase::FeatureCapability::PlayerFastSprint,
     [](bool v) { return XBase::Player::SetFastSprint(v); }},
    {"player.sprintEverywhere", XBase::FeatureCapability::PlayerSprintEverywhere,
     [](bool v) { return XBase::Player::SetSprintEverywhere(v); }},
    {"player.drunkEffect", XBase::FeatureCapability::PlayerDrunkEffect,
     [](bool v) { return XBase::Player::SetDrunkEffect(v); }},
    {"player.neverWanted", XBase::FeatureCapability::PlayerNeverWanted,
     [](bool v) { return XBase::Player::SetNeverWanted(v); }},
    {"player.keepStuff", XBase::FeatureCapability::PlayerKeepStuff,
     [](bool v) { return XBase::Player::SetKeepStuff(v); }},
    {"player.freeHealthcare", XBase::FeatureCapability::PlayerRuntimeEffects,
     [](bool v) { XBase::Player::SetFreeHealthcare(v); return v; }},
    {"player.freeJail", XBase::FeatureCapability::PlayerRuntimeEffects,
     [](bool v) { XBase::Player::SetFreeJail(v); return v; }},

    {"ped.bigHead", XBase::FeatureCapability::PedBigHead,
     [](bool v) { XBase::Ped::SetBigHead(v); return v; }},
    {"ped.thinBody", XBase::FeatureCapability::PedThinBody,
     [](bool v) { XBase::Ped::SetThinBody(v); return v; }},
    {"ped.flies", XBase::FeatureCapability::PedSmokeFlies,
     [](bool v) { XBase::Ped::SetFliesEffect(v); return v; }},
    {"ped.smoking", XBase::FeatureCapability::PedAttributes,
     [](bool v) { XBase::Ped::SetSmokingEffect(v); return v; }},
    {"ped.everyoneArmed", XBase::FeatureCapability::PedAttributes,
     [](bool v) { XBase::Ped::SetEveryoneArmed(v); return v; }},
    {"ped.mayhem", XBase::FeatureCapability::PedGlobalStrategies,
     [](bool v) { XBase::Ped::SetPedsMayhem(v); return v; }},
    {"ped.riot", XBase::FeatureCapability::PedGlobalStrategies,
     [](bool v) { XBase::Ped::SetPedsRiot(v); return v; }},
    {"ped.atkRocket", XBase::FeatureCapability::PedGlobalStrategies,
     [](bool v) { XBase::Ped::SetPedsAtkRocket(v); return v; }},
    {"ped.elvis", XBase::FeatureCapability::PedClassification,
     [](bool v) { XBase::Ped::SetElvisEverywhere(v); return v; }},
    {"ped.slutMagnet", XBase::FeatureCapability::PedAttributes,
     [](bool v) { XBase::Ped::SetSlutMagnet(v); return v; }},
    {"ped.nastyLimbs", XBase::FeatureCapability::PedAttributes,
     [](bool v) { XBase::Ped::SetNastyLimbs(v); return v; }},
    {"ped.noProstitutes", XBase::FeatureCapability::PedAttributes,
     [](bool v) { XBase::Ped::SetNoProstitutes(v); return v; }},
    {"ped.gangsEverywhere", XBase::FeatureCapability::PedClassification,
     [](bool v) { XBase::Ped::SetGangsEverywhere(v); return v; }},
    {"ped.gangsControl", XBase::FeatureCapability::PedClassification,
     [](bool v) { XBase::Ped::SetGangsControl(v); return v; }},
    {"ped.gangWars", XBase::FeatureCapability::PedGlobalStrategies,
     [](bool v) { XBase::Ped::SetGangWarsActive(v); return v; }},

    {"vehicle.heavy", XBase::FeatureCapability::VehicleTakeLessDamage,
     [](bool v) { XBase::Vehicle::SetHeavy(v); return v; }},
    {"vehicle.watertight", XBase::FeatureCapability::VehicleBasic,
     [](bool v) { XBase::Vehicle::SetWatertight(v); return v; }},
    {"vehicle.alwaysSkidMarks", XBase::FeatureCapability::VehicleAlwaysSkidMarks,
     [](bool v) { return XBase::Vehicle::SetAlwaysSkidMarks(v); }},
    {"vehicle.disableParticles", XBase::FeatureCapability::VehicleDisableParticles,
     [](bool v) { return XBase::Vehicle::SetDisableParticles(v); }},
    {"vehicle.driverTargetable", XBase::FeatureCapability::VehicleDriverTargetable,
     [](bool v) { return XBase::Vehicle::SetDriverTargetable(v); }},
    {"vehicle.heatSeekingTargetable", XBase::FeatureCapability::VehicleHeatSeekingTargetable,
     [](bool v) { return XBase::Vehicle::SetHeatSeekingTargetable(v); }},
    {"vehicle.visible", XBase::FeatureCapability::VehicleBasic,
     [](bool v) { XBase::Vehicle::SetVisible(v); return v; }},
    {"vehicle.engine", XBase::FeatureCapability::VehicleBasic,
     [](bool v) { XBase::Vehicle::SetEngine(v); return v; }},
    {"vehicle.autoDrive", XBase::FeatureCapability::VehicleAutoDrive,
     [](bool v) { return XBase::Vehicle::SetAutoDriveToWaypoint(v); }},

    {"world.fasterClock", XBase::FeatureCapability::WorldFasterClock,
     [](bool v) { XBase::World::SetFasterClock(v); return v; }},
    {"world.disableReplay", XBase::FeatureCapability::WorldDisableReplay,
     [](bool v) { XBase::World::SetDisableReplay(v); return v; }},
    {"world.disableCheats", XBase::FeatureCapability::WorldDisableCheats,
     [](bool v) { XBase::World::SetDisableCheats(v); return v; }},
    {"world.forbiddenAreaWanted", XBase::FeatureCapability::WorldForbiddenAreaWanted,
     [](bool v) { XBase::World::SetForbiddenAreaWanted(v); return v; }},
    {"world.freePayNSpray", XBase::FeatureCapability::WorldFreePayNSpray,
     [](bool v) { XBase::World::SetFreePayNSpray(v); return v; }},
    {"world.noWaterPhysics", XBase::FeatureCapability::WorldNoWaterPhysics,
     [](bool v) { XBase::World::SetNoWaterPhysics(v); return v; }},
    {"world.solidWater", XBase::FeatureCapability::WorldSolidWater,
     [](bool v) { XBase::World::SetSolidWater(v); return v; }},

    {"vehicle.noDamage", XBase::FeatureCapability::VehicleTakeLessDamage,
     [](bool v) { return XBase::Vehicle::SetTakeLessDamage(v); }},
    {"vehicle.invisible", XBase::FeatureCapability::VehicleBasic,
     [](bool v) { XBase::Vehicle::SetVisible(!v); return v; }},
    {"vehicle.petrolTankWeak", XBase::FeatureCapability::VehiclePetrolTankWeakPoint,
     [](bool v) { return XBase::Vehicle::SetPetrolTankWeakPoint(v); }},
    {"cheats.flyingCars", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::FlyingCars(v); return v; }},
    {"cheats.allCarsHaveNitro", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::AllCarsHaveNitro(v); return v; }},
    {"cheats.perfectHandling", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::PerfectHandling(v); return v; }},
    {"cheats.greenLights", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::GreenLights(v); return v; }},
    {"cheats.boatFly", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::BoatFly(v); return v; }},
    {"cheats.driveWater", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::DriveWater(v); return v; }},
    {"cheats.tankMode", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::TankMode(v); return v; }},
    {"cheats.aimDrive", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::AimDrive(v); return v; }},
    {"cheats.noDerail", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::NoDerail(v); return v; }},
    {"cheats.flipNoBurn", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::FlipNoBurn(v); return v; }},
    {"cheats.stayOnBike", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::StayOnBike(v); return v; }},
    {"cheats.bikeFly", XBase::FeatureCapability::VehicleCheats,
     [](bool v) { XBase::Cheats::BikeFly(v); return v; }},
    {"camera.freecam", XBase::FeatureCapability::CameraFreecam,
     [](bool v) { return XBase::Camera::SetMode(v ? XBase::Camera::Mode::Freecam : XBase::Camera::Mode::Disabled); }},
    {"camera.topDown", XBase::FeatureCapability::CameraTopDown,
     [](bool v) { return XBase::Camera::SetMode(v ? XBase::Camera::Mode::TopDown : XBase::Camera::Mode::Disabled); }},
};

// 后端里仍是空实现或常量返回的方法，按机型直接标为不可用，
// 否则入口可点但功能不会发生，观感比直接禁用更差
bool IsMethodStubbed(const char* method) {
#if defined(XBASE_BACKEND_VC)
    static const char* const kStubbed[] = {
        "ped.elvis",
        "ped.gangsControl",
        "ped.gangDensity",
        "ped.gangMemberModel",
        "ped.nastyLimbs",
        "teleport.marker",
        "cheats.perfectHandling",
    };
#elif defined(XBASE_BACKEND_III)
    static const char* const kStubbed[] = {
        "ped.elvis",
        "ped.gangDensity",
        "ped.gangMemberModel",
        "teleport.marker",
        "cheats.boatFly",
        "cheats.driveWater",
        "cheats.greenLights",
    };
#else
    static const char* const kStubbed[] = {""};
    (void)kStubbed;
    return false;
#endif
    for (const char* name : kStubbed) {
        if (std::strcmp(name, method) == 0) {
            return true;
        }
    }
    return false;
}

XBase::Json::Value CapabilityReport() {
    struct Entry {
        const char* method;
        XBase::FeatureCapability capability;
    };

    static const Entry entries[] = {
        {"player.snapshot", XBase::FeatureCapability::PlayerBasicState},
        {"player.heal", XBase::FeatureCapability::PlayerBasicState},
        {"player.armour", XBase::FeatureCapability::PlayerBasicState},
        {"player.money", XBase::FeatureCapability::PlayerBasicState},
        {"player.wanted", XBase::FeatureCapability::PlayerBasicState},
        {"player.kill", XBase::FeatureCapability::PlayerBasicState},
        {"player.moveRelative", XBase::FeatureCapability::PlayerMovement},
        {"vehicle.snapshot", XBase::FeatureCapability::VehicleBasic},
        {"vehicle.spawn", XBase::FeatureCapability::VehicleSpawn},
        {"vehicle.repair", XBase::FeatureCapability::VehicleBasic},
        {"vehicle.unflip", XBase::FeatureCapability::VehicleBasic},
        {"vehicle.colors", XBase::FeatureCapability::VehicleColors},
        {"vehicle.doors", XBase::FeatureCapability::VehicleDoors},
        {"world.getTime", XBase::FeatureCapability::WorldTime},
        {"world.setTime", XBase::FeatureCapability::WorldTime},
        {"world.weather", XBase::FeatureCapability::WorldWeather},
        {"world.gameSpeed", XBase::FeatureCapability::WorldGameSpeed},
        {"world.gravity", XBase::FeatureCapability::WorldGravity},
        {"world.freezeTime", XBase::FeatureCapability::WorldFreezeTime},
        {"teleport.to", XBase::FeatureCapability::TeleportBasic},
        {"teleport.forward", XBase::FeatureCapability::TeleportBasic},
        {"teleport.marker", XBase::FeatureCapability::TeleportBasic},
        {"weapon.give", XBase::FeatureCapability::WeaponGive},
        {"weapon.giveAll", XBase::FeatureCapability::WeaponGive},
        {"weapon.infiniteAmmo", XBase::FeatureCapability::WeaponRuntimeEffects},
        {"player.setHealth", XBase::FeatureCapability::PlayerBasicState},
        {"player.infiniteSprint", XBase::FeatureCapability::PlayerRuntimeEffects},
        {"player.skin", XBase::FeatureCapability::PlayerAppearance},
        {"ped.spawn", XBase::FeatureCapability::PedSpawn},
        {"ped.deleteLast", XBase::FeatureCapability::PedDelete},
        {"ped.noFire", XBase::FeatureCapability::BulletAssistFireSuppression},
        {"ped.noFireOptions", XBase::FeatureCapability::BulletAssistFireSuppression},
        {"vehicle.lights", XBase::FeatureCapability::VehicleBasic},
        {"vehicle.locked", XBase::FeatureCapability::VehicleBasic},
        {"vehicle.health", XBase::FeatureCapability::VehicleBasic},
        {"vehicle.paintjob", XBase::FeatureCapability::VehiclePaintjob},
        {"world.environment", XBase::FeatureCapability::WorldWeatherEffects},
        {"world.destroyVehicles", XBase::FeatureCapability::WorldGameSpeed},
        {"world.destroyPeds", XBase::FeatureCapability::WorldGameSpeed},
        {"weapon.clearAll", XBase::FeatureCapability::WeaponGive},
        {"weapon.drop", XBase::FeatureCapability::WeaponDrop},
        {"weapon.maxSkills", XBase::FeatureCapability::WeaponSkills},
        {"weapon.fastReload", XBase::FeatureCapability::WeaponRuntimeEffects},
        {"teleport.mapPosition", XBase::FeatureCapability::TeleportBasic},
        {"teleport.center", XBase::FeatureCapability::TeleportBasic},
        {"visual.hud", XBase::FeatureCapability::VisualHudRadar},
        {"visual.radar", XBase::FeatureCapability::VisualHudRadar},
        {"visual.filter", XBase::FeatureCapability::VisualFilter},
        {"visual.radarOptions", XBase::FeatureCapability::VisualRadarOptions},
        {"scene.animation", XBase::FeatureCapability::SceneAnimation},
        {"scene.particle", XBase::FeatureCapability::SceneParticle},
        {"scene.cutscene", XBase::FeatureCapability::SceneCutscene},
        {"scene.mission", XBase::FeatureCapability::SceneMission},
        {"scene.style", XBase::FeatureCapability::SceneAnimation},
        {"weapon.statOverrides", XBase::FeatureCapability::WeaponStatOverrides},
        {"weapon.resetStats", XBase::FeatureCapability::WeaponStatOverrides},
        {"player.maxVehicleSkills", XBase::FeatureCapability::PlayerStats},
        {"player.saveGame", XBase::FeatureCapability::PlayerSaveGame},
        {"world.fpsLimit", XBase::FeatureCapability::WorldFpsLimit},
        {"world.daysPassed", XBase::FeatureCapability::WorldDaysPassed},
        {"world.syncClock", XBase::FeatureCapability::WorldTime},
        {"ped.spawnLimits", XBase::FeatureCapability::PedGlobalStrategies},
        {"ped.gangWarStart", XBase::FeatureCapability::PedGlobalStrategies},
        {"ped.gangWarEnd", XBase::FeatureCapability::PedGlobalStrategies},
        {"player.proofs", XBase::FeatureCapability::PlayerProofs},
        {"player.runtimeOptions", XBase::FeatureCapability::PlayerRuntimeEffects},
        {"vehicle.trafficDensity", XBase::FeatureCapability::VehicleTrafficDensity},
        {"vehicle.colors4", XBase::FeatureCapability::VehicleColors},
        {"vehicle.blowUpAll", XBase::FeatureCapability::VehicleBasic},
        {"vehicle.popDoor", XBase::FeatureCapability::VehiclePopDoors},
        {"vehicle.siren", XBase::FeatureCapability::VehicleSirenOrAlarm},
        {"vehicle.neon", XBase::FeatureCapability::VehicleEffectsNeon},
        {"world.pickup", XBase::FeatureCapability::WorldPickups},
        {"world.removePickups", XBase::FeatureCapability::WorldPickups},
        {"bulletAssist.config", XBase::FeatureCapability::BulletAssistTracking},
        {"cheats.toggle", XBase::FeatureCapability::VehicleCheats},
        {"cheats.random", XBase::FeatureCapability::CheatsRandom},
        {"cheats.randomList", XBase::FeatureCapability::CheatsRandom},
        {"player.clothes", XBase::FeatureCapability::PlayerClothes},
        {"player.customSkin", XBase::FeatureCapability::PlayerAppearance},
        {"player.stat", XBase::FeatureCapability::PlayerStats},
        {"ped.gangDensity", XBase::FeatureCapability::PedClassification},
        {"ped.gangMemberModel", XBase::FeatureCapability::PedClassification},
        {"ped.gangWeapons", XBase::FeatureCapability::PedAttributes},
        {"ped.resetGangModels", XBase::FeatureCapability::PedClassification},
        {"vehicle.speedLock", XBase::FeatureCapability::VehicleCheats},
        {"vehicle.targetSpeed", XBase::FeatureCapability::VehicleCheats},
        {"vehicle.upgrade", XBase::FeatureCapability::VehicleUpgrades},
        {"vehicle.petrolTankWeak", XBase::FeatureCapability::VehiclePetrolTankWeakPoint},
        {"camera.settings", XBase::FeatureCapability::CameraFreecam},
        {"world.weatherRelease", XBase::FeatureCapability::WorldWeather},
        {"vehicle.seat", XBase::FeatureCapability::VehicleBasic},
        {"vehicle.resetColors", XBase::FeatureCapability::VehicleColors},
        {"weapon.removePickups", XBase::FeatureCapability::WeaponBasic},
        {"player.aimSkin", XBase::FeatureCapability::PlayerAimSkinChanger},
    };

    XBase::Json::Value methods;
    for (const Entry& entry : entries) {
        methods.Set(entry.method, XBase::Json::Value(CapabilityName(XBase::GetCapabilitySupport(entry.capability))));
    }
    for (const BoolToggle& toggle : kBoolToggles) {
        methods.Set(toggle.method, XBase::Json::Value(CapabilityName(XBase::GetCapabilitySupport(toggle.capability))));
    }

    for (const Entry& entry : entries) {
        if (IsMethodStubbed(entry.method)) {
            methods.Set(entry.method, XBase::Json::Value("unsupported"));
        }
    }
    for (const BoolToggle& toggle : kBoolToggles) {
        if (IsMethodStubbed(toggle.method)) {
            methods.Set(toggle.method, XBase::Json::Value("unsupported"));
        }
    }

    XBase::Json::Value result;
    result.Set("protocol", XBase::Json::Value(ProtocolVersion));
    result.Set("game", XBase::Json::Value(XBase::Runtime::GetGameKey()));
    result.Set("gameName", XBase::Json::Value(XBase::Runtime::GetGameName()));
    result.Set("methods", methods);
    return result;
}

XBase::Json::Value PlayerSnapshotJson() {
    const XBase::Player::PlayerSnapshot snapshot = XBase::Player::GetSnapshot();

    XBase::Json::Value position;
    position.Set("x", XBase::Json::Value(static_cast<double>(snapshot.position.x)));
    position.Set("y", XBase::Json::Value(static_cast<double>(snapshot.position.y)));
    position.Set("z", XBase::Json::Value(static_cast<double>(snapshot.position.z)));

    XBase::Json::Value result;
    result.Set("valid", XBase::Json::Value(snapshot.valid));
    result.Set("position", position);
    result.Set("health", XBase::Json::Value(static_cast<double>(snapshot.health)));
    result.Set("armour", XBase::Json::Value(static_cast<double>(snapshot.armour)));
    result.Set("money", XBase::Json::Value(snapshot.money));
    result.Set("wantedLevel", XBase::Json::Value(snapshot.wantedLevel));
    return result;
}

XBase::Json::Value VehicleSnapshotJson() {
    const XBase::Vehicle::VehicleSnapshot snapshot = XBase::Vehicle::GetSnapshot();

    XBase::Json::Value colors;
    colors.Set("primary", XBase::Json::Value(snapshot.colors.primary));
    colors.Set("secondary", XBase::Json::Value(snapshot.colors.secondary));

    XBase::Json::Value result;
    result.Set("valid", XBase::Json::Value(snapshot.modelId != 0));
    result.Set("modelId", XBase::Json::Value(static_cast<int>(snapshot.modelId)));
    result.Set("health", XBase::Json::Value(static_cast<double>(snapshot.health)));
    result.Set("colors", colors);
    result.Set("lights", XBase::Json::Value(snapshot.lights));
    result.Set("locked", XBase::Json::Value(snapshot.locked));
    return result;
}

void HandleMessage(const std::string& message) {
    const XBase::Json::Value request = XBase::Json::Value::Parse(message);
    if (!request.IsObject()) {
        return;
    }

    const XBase::Json::Value id = request["id"];
    const std::string method = request["method"].AsString();
    const XBase::Json::Value params = request["params"];
    if (method.empty()) {
        Fail(id, "missing method");
        return;
    }

    if (IsMethodStubbed(method.c_str())) {
        Fail(id, std::string("unsupported on this game: ") + method);
        return;
    }

    if (method == "bridge.capabilities") {
        Reply(id, CapabilityReport());
        return;
    }

    // 宿主注册的方法优先，便于覆盖或扩展默认表
    const auto custom = s_customMethods.find(method);
    if (custom != s_customMethods.end()) {
        if (!custom->second) {
            Fail(id, std::string("handler missing: ") + method);
            return;
        }
        Reply(id, custom->second(params));
        return;
    }

    if (method == "player.snapshot") {
        Reply(id, PlayerSnapshotJson());
        return;
    }
    if (method == "player.heal") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerBasicState, "player.heal")) return;
        XBase::Player::Heal();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.armour") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerBasicState, "player.armour")) return;
        XBase::Player::GiveArmour();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.money") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerBasicState, "player.money")) return;
        XBase::Player::SetMoney(params["amount"].AsInt());
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.wanted") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerBasicState, "player.wanted")) return;
        XBase::Player::SetWantedLevel(params["level"].AsInt());
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.kill") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerBasicState, "player.kill")) return;
        XBase::Player::Kill();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.moveRelative") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerMovement, "player.moveRelative")) return;
        const bool moved = XBase::Player::MoveRelative(
            static_cast<float>(params["forward"].AsNumber()),
            static_cast<float>(params["right"].AsNumber()),
            static_cast<float>(params["up"].AsNumber()));
        Reply(id, XBase::Json::Value(moved));
        return;
    }

    if (method == "vehicle.snapshot") {
        Reply(id, VehicleSnapshotJson());
        return;
    }
    if (method == "vehicle.spawn") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleSpawn, "vehicle.spawn")) return;
        XBase::Vehicle::SpawnOptions options;
        options.asDriver = params["asDriver"].AsBool(true);
        options.aircraftInAir = params["aircraftInAir"].AsBool(true);
        options.cleanupPrevious = params["cleanupPrevious"].AsBool(true);
        const XBase::Vehicle::SpawnResult result =
            XBase::Vehicle::SpawnEx(static_cast<unsigned int>(params["model"].AsInt()), options);
        Reply(id, XBase::Json::Value(result.success));
        return;
    }
    if (method == "vehicle.repair") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleBasic, "vehicle.repair")) return;
        XBase::Vehicle::Repair();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.unflip") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleBasic, "vehicle.unflip")) return;
        XBase::Vehicle::Unflip();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.colors") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleColors, "vehicle.colors")) return;
        XBase::Vehicle::Colors colors = XBase::Vehicle::GetColors();
        if (!params["primary"].IsNull()) colors.primary = params["primary"].AsInt();
        if (!params["secondary"].IsNull()) colors.secondary = params["secondary"].AsInt();
        XBase::Vehicle::SetColors(colors);
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.doors") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleDoors, "vehicle.doors")) return;
        XBase::Vehicle::OpenDoor(params["index"].AsInt());
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "world.getTime") {
        int hour = 0;
        int minute = 0;
        XBase::World::GetTime(hour, minute);
        XBase::Json::Value result;
        result.Set("hour", XBase::Json::Value(hour));
        result.Set("minute", XBase::Json::Value(minute));
        Reply(id, result);
        return;
    }
    if (method == "world.setTime") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldTime, "world.setTime")) return;
        XBase::World::SetTime(params["hour"].AsInt(), params["minute"].AsInt());
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "world.weather") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldWeather, "world.weather")) return;
        XBase::World::SetWeather(params["id"].AsInt(), params["lock"].AsBool(false));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "world.gameSpeed") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldGameSpeed, "world.gameSpeed")) return;
        XBase::World::SetGameSpeed(static_cast<float>(params["value"].AsNumber(1.0)));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "world.gravity") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldGravity, "world.gravity")) return;
        XBase::World::SetGravity(static_cast<float>(params["value"].AsNumber()));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "world.freezeTime") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldFreezeTime, "world.freezeTime")) return;
        XBase::World::SetFreezeTime(params["enable"].AsBool(true));
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "teleport.to") {
        if (!RequireCapability(id, XBase::FeatureCapability::TeleportBasic, "teleport.to")) return;
        const bool moved = XBase::Teleport::To(
            static_cast<float>(params["x"].AsNumber()),
            static_cast<float>(params["y"].AsNumber()),
            static_cast<float>(params["z"].AsNumber()),
            params["interior"].AsInt());
        Reply(id, XBase::Json::Value(moved));
        return;
    }
    if (method == "teleport.forward") {
        if (!RequireCapability(id, XBase::FeatureCapability::TeleportBasic, "teleport.forward")) return;
        const bool moved = XBase::Teleport::Forward(static_cast<float>(params["distance"].AsNumber(10.0)));
        Reply(id, XBase::Json::Value(moved));
        return;
    }
    if (method == "teleport.marker") {
        if (!RequireCapability(id, XBase::FeatureCapability::TeleportBasic, "teleport.marker")) return;
        const bool moved = XBase::Teleport::Marker(params["underwater"].AsBool(false));
        Reply(id, XBase::Json::Value(moved));
        return;
    }

    if (method == "weapon.give") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponGive, "weapon.give")) return;
        const int model = params["model"].AsInt(0);
        const bool given = model > 0
            ? XBase::Weapon::GiveModel(
                  static_cast<unsigned int>(model),
                  static_cast<unsigned int>(params["ammo"].AsInt(999)))
            : XBase::Weapon::Give(
                  static_cast<unsigned int>(params["type"].AsInt()),
                  static_cast<unsigned int>(params["ammo"].AsInt(999)));
        Reply(id, XBase::Json::Value(given));
        return;
    }
    if (method == "weapon.giveAll") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponGive, "weapon.giveAll")) return;
        Reply(id, XBase::Json::Value(XBase::Weapon::GiveAll()));
        return;
    }
    if (method == "weapon.infiniteAmmo") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponRuntimeEffects, "weapon.infiniteAmmo")) return;
        Reply(id, XBase::Json::Value(XBase::Weapon::SetInfiniteAmmo(params["enable"].AsBool(true))));
        return;
    }

    if (method == "player.setHealth") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerBasicState, "player.setHealth")) return;
        XBase::Player::SetHealth(static_cast<float>(params["value"].AsNumber(100.0)));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.infiniteSprint") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerRuntimeEffects, "player.infiniteSprint")) return;
        XBase::Player::SetInfiniteSprint(params["enable"].AsBool(true));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.skin") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerAppearance, "player.skin")) return;
        Reply(id, XBase::Json::Value(XBase::Player::SetSkin(static_cast<unsigned int>(params["model"].AsInt()))));
        return;
    }

    if (method == "ped.spawn") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedSpawn, "ped.spawn")) return;
        XBase::Types::PedSpawnOptions options;
        options.health = static_cast<float>(params["health"].AsNumber(100.0));
        options.armour = static_cast<float>(params["armour"].AsNumber(0.0));
        const bool atMarker = params["atMarker"].AsBool(false);
        const bool spawned = atMarker
            ? XBase::Ped::SpawnAtMarker(static_cast<unsigned int>(params["model"].AsInt()), options)
            : XBase::Ped::SpawnNearPlayer(static_cast<unsigned int>(params["model"].AsInt()), options);
        Reply(id, XBase::Json::Value(spawned));
        return;
    }
    if (method == "ped.deleteLast") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedDelete, "ped.deleteLast")) return;
        XBase::Ped::DeleteLastSpawned();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "ped.noFire") {
        if (!RequireCapability(id, XBase::FeatureCapability::BulletAssistFireSuppression, "ped.noFire")) return;
        XBase::Ped::SetNoFire(params["enable"].AsBool(true));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "ped.noFireOptions") {
        if (!RequireCapability(id, XBase::FeatureCapability::BulletAssistFireSuppression, "ped.noFireOptions")) return;
        XBase::Ped::NoFireOptions options;
        options.enable = params["enable"].AsBool(true);
        options.civilians = params["civilians"].AsBool(true);
        options.gangs = params["gangs"].AsBool(true);
        options.cops = params["cops"].AsBool(true);
        options.mission = params["mission"].AsBool(false);
        XBase::Ped::SetNoFire(options);
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "vehicle.lights") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleBasic, "vehicle.lights")) return;
        XBase::Vehicle::SetLights(params["enable"].AsBool(true));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.locked") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleBasic, "vehicle.locked")) return;
        XBase::Vehicle::SetLocked(params["enable"].AsBool(true));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.health") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleBasic, "vehicle.health")) return;
        XBase::Vehicle::SetHealth(static_cast<float>(params["value"].AsNumber(1000.0)));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.paintjob") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehiclePaintjob, "vehicle.paintjob")) return;
        Reply(id, XBase::Json::Value(XBase::Vehicle::SetPaintjob(params["index"].AsInt())));
        return;
    }

    if (method == "world.environment") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldWeatherEffects, "world.environment")) return;
        if (!params["rain"].IsNull()) XBase::World::SetRain(static_cast<float>(params["rain"].AsNumber()));
        if (!params["fog"].IsNull()) XBase::World::SetFoggyness(static_cast<float>(params["fog"].AsNumber()));
        if (!params["clouds"].IsNull()) XBase::World::SetCloudCoverage(static_cast<float>(params["clouds"].AsNumber()));
        if (!params["wind"].IsNull()) XBase::World::SetWind(static_cast<float>(params["wind"].AsNumber()));
        if (!params["sandstorm"].IsNull()) XBase::World::SetSandstorm(static_cast<float>(params["sandstorm"].AsNumber()));
        if (!params["extraSunny"].IsNull()) XBase::World::SetExtraSunnyness(static_cast<float>(params["extraSunny"].AsNumber()));
        if (!params["wetRoads"].IsNull()) XBase::World::SetWetRoads(static_cast<float>(params["wetRoads"].AsNumber()));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "world.destroyVehicles") {
        XBase::World::DestroyAllVehicles();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "world.destroyPeds") {
        XBase::World::DestroyAllPeds();
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "weapon.clearAll") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponGive, "weapon.clearAll")) return;
        Reply(id, XBase::Json::Value(XBase::Weapon::ClearAll()));
        return;
    }
    if (method == "weapon.drop") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponDrop, "weapon.drop")) return;
        Reply(id, XBase::Json::Value(XBase::Weapon::DropWeapon()));
        return;
    }
    if (method == "weapon.maxSkills") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponSkills, "weapon.maxSkills")) return;
        Reply(id, XBase::Json::Value(XBase::Weapon::MaxWeaponSkills()));
        return;
    }
    if (method == "weapon.fastReload") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponRuntimeEffects, "weapon.fastReload")) return;
        Reply(id, XBase::Json::Value(XBase::Weapon::SetFastReload(params["enable"].AsBool(true))));
        return;
    }

    if (method == "teleport.mapPosition") {
        if (!RequireCapability(id, XBase::FeatureCapability::TeleportBasic, "teleport.mapPosition")) return;
        const bool moved = XBase::Teleport::MapPosition(
            static_cast<float>(params["x"].AsNumber()),
            static_cast<float>(params["y"].AsNumber()),
            params["underwater"].AsBool(false));
        Reply(id, XBase::Json::Value(moved));
        return;
    }
    if (method == "teleport.center") {
        if (!RequireCapability(id, XBase::FeatureCapability::TeleportBasic, "teleport.center")) return;
        Reply(id, XBase::Json::Value(XBase::Teleport::Center()));
        return;
    }

    if (method == "visual.hud") {
        if (!RequireCapability(id, XBase::FeatureCapability::VisualHudRadar, "visual.hud")) return;
        Reply(id, XBase::Json::Value(XBase::Visual::DisplayHud(params["enable"].AsBool(true))));
        return;
    }
    if (method == "visual.radar") {
        if (!RequireCapability(id, XBase::FeatureCapability::VisualHudRadar, "visual.radar")) return;
        Reply(id, XBase::Json::Value(XBase::Visual::DisplayRadar(params["enable"].AsBool(true))));
        return;
    }
    if (method == "visual.filter") {
        if (!RequireCapability(id, XBase::FeatureCapability::VisualFilter, "visual.filter")) return;
        if (params["id"].IsNull()) {
            Reply(id, XBase::Json::Value(XBase::Visual::GetFilter()));
            return;
        }
        const bool applied = XBase::Visual::SetFilter(
            params["id"].AsInt(),
            static_cast<float>(params["strength"].AsNumber(1.0)));
        Reply(id, XBase::Json::Value(applied));
        return;
    }
    if (method == "visual.radarOptions") {
        if (!RequireCapability(id, XBase::FeatureCapability::VisualRadarOptions, "visual.radarOptions")) return;
        XBase::Visual::RadarOptions options;
        options.square = params["square"].AsBool(false);
        options.noRadarRot = params["noRadarRot"].AsBool(false);
        options.fullscreenMap = params["fullscreenMap"].AsBool(false);
        options.unfogMap = params["unfogMap"].AsBool(false);
        options.hideAreaNames = params["hideAreaNames"].AsBool(false);
        options.hideVehicleNames = params["hideVehicleNames"].AsBool(false);
        options.nightVision = params["nightVision"].AsBool(false);
        options.infrared = params["infrared"].AsBool(false);
        XBase::Visual::SetRadarOptions(options);
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "vehicle.autoDrive") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleAutoDrive, "vehicle.autoDrive")) return;
        const float speed = static_cast<float>(params["speed"].AsNumber(35.0));
        Reply(id, XBase::Json::Value(
            XBase::Vehicle::SetAutoDriveToWaypoint(params["enable"].AsBool(true), speed)));
        return;
    }

    for (const BoolToggle& toggle : kBoolToggles) {
        if (method == toggle.method) {
            if (!RequireCapability(id, toggle.capability, toggle.method)) return;
            Reply(id, XBase::Json::Value(toggle.apply(params["enable"].AsBool(true))));
            return;
        }
    }

    if (method == "ped.spawnLimits") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedGlobalStrategies, "ped.spawnLimits")) return;
        XBase::Ped::SetSpawnLimits(
            params["limitPolice"].AsBool(false),
            params["limitGangs"].AsBool(false),
            params["maxPolice"].AsInt(0),
            params["maxGangs"].AsInt(0));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "ped.gangWarStart") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedGlobalStrategies, "ped.gangWarStart")) return;
        XBase::Ped::StartGangWar(params["offensive"].AsBool(true));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "ped.gangWarEnd") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedGlobalStrategies, "ped.gangWarEnd")) return;
        XBase::Ped::EndGangWar();
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "weapon.statOverrides") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponStatOverrides, "weapon.statOverrides")) return;
        XBase::Weapon::StatOverrides overrides;
        overrides.hugeDamage = params["hugeDamage"].AsBool(false);
        overrides.longRange = params["longRange"].AsBool(false);
        overrides.rapidFire = params["rapidFire"].AsBool(false);
        overrides.dualWield = params["dualWield"].AsBool(false);
        overrides.moveAim = params["moveAim"].AsBool(false);
        overrides.moveFire = params["moveFire"].AsBool(false);
        overrides.noSpread = params["noSpread"].AsBool(false);
        overrides.customFireRate = params["customFireRate"].AsBool(false);
        overrides.fireRate = static_cast<float>(params["fireRate"].AsNumber(1.0));
        overrides.autoAim = params["autoAim"].AsBool(false);
        XBase::Weapon::SetStatOverrides(overrides);
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "weapon.resetStats") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponStatOverrides, "weapon.resetStats")) return;
        Reply(id, XBase::Json::Value(XBase::Weapon::ResetStats()));
        return;
    }

    if (method == "player.maxVehicleSkills") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerStats, "player.maxVehicleSkills")) return;
        Reply(id, XBase::Json::Value(XBase::Player::MaxVehicleSkills()));
        return;
    }
    if (method == "player.saveGame") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerSaveGame, "player.saveGame")) return;
        Reply(id, XBase::Json::Value(XBase::Player::RequestSaveGame()));
        return;
    }
    if (method == "player.copyCoordinates") {
        XBase::Player::CopyCoordinates();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.setArmour") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerBasicState, "player.setArmour")) return;
        XBase::Player::SetArmour(static_cast<float>(params["value"].AsNumber(100.0)));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.clearWanted") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerBasicState, "player.clearWanted")) return;
        XBase::Player::ClearWantedLevel();
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "world.fpsLimit") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldFpsLimit, "world.fpsLimit")) return;
        const int limit = params["value"].AsInt(0);
        XBase::World::SetFpsLimit(limit);
        Reply(id, XBase::Json::Value(XBase::World::GetFpsLimit()));
        return;
    }
    if (method == "world.daysPassed") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldDaysPassed, "world.daysPassed")) return;
        const int days = params["value"].AsInt(0);
        XBase::World::SetDaysPassed(days);
        Reply(id, XBase::Json::Value(XBase::World::GetDaysPassed()));
        return;
    }
    if (method == "world.syncClock") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldTime, "world.syncClock")) return;
        XBase::World::SyncTimeWithSystemClock();
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "scene.animation") {
        if (!RequireCapability(id, XBase::FeatureCapability::SceneAnimation, "scene.animation")) return;
        if (params["stop"].AsBool(false)) {
            Reply(id, XBase::Json::Value(XBase::Scene::StopAnimation()));
            return;
        }
        XBase::Scene::AnimationOptions options;
        options.loop = params["loop"].AsBool(false);
        options.secondary = params["secondary"].AsBool(false);
        options.onTargetPed = params["onTargetPed"].AsBool(false);
        const std::string group = params["group"].AsString();
        const std::string name = params["name"].AsString();
        Reply(id, XBase::Json::Value(XBase::Scene::PlayAnimation(group.c_str(), name.c_str(), options)));
        return;
    }
    if (method == "scene.particle") {
        if (!RequireCapability(id, XBase::FeatureCapability::SceneParticle, "scene.particle")) return;
        if (params["removeAll"].AsBool(false)) {
            Reply(id, XBase::Json::Value(XBase::Scene::RemoveAllParticles()));
            return;
        }
        if (params["removeLast"].AsBool(false)) {
            Reply(id, XBase::Json::Value(XBase::Scene::RemoveLatestParticle()));
            return;
        }
        const std::string name = params["name"].AsString();
        Reply(id, XBase::Json::Value(XBase::Scene::PlayParticle(name.c_str())));
        return;
    }
    if (method == "scene.cutscene") {
        if (!RequireCapability(id, XBase::FeatureCapability::SceneCutscene, "scene.cutscene")) return;
        if (params["stop"].AsBool(false)) {
            Reply(id, XBase::Json::Value(XBase::Scene::StopCutscene()));
            return;
        }
        if (params["status"].AsBool(false)) {
            Reply(id, XBase::Json::Value(XBase::Scene::IsCutsceneRunning()));
            return;
        }
        const std::string name = params["name"].AsString();
        const int interior = params["interior"].AsInt(-1);
        Reply(id, XBase::Json::Value(interior >= 0
            ? XBase::Scene::StartCutscene(name.c_str(), interior)
            : XBase::Scene::StartCutscene(name.c_str())));
        return;
    }
    if (method == "scene.mission") {
        if (!RequireCapability(id, XBase::FeatureCapability::SceneMission, "scene.mission")) return;
        if (params["fail"].AsBool(false)) {
            Reply(id, XBase::Json::Value(XBase::Scene::FailMission()));
            return;
        }
        if (params["status"].AsBool(false)) {
            const char* status = XBase::Scene::GetMissionStatus();
            Reply(id, XBase::Json::Value(std::string(status ? status : "")));
            return;
        }
        Reply(id, XBase::Json::Value(XBase::Scene::StartMission(params["id"].AsInt(0))));
        return;
    }
    if (method == "scene.style") {
        if (!RequireCapability(id, XBase::FeatureCapability::SceneAnimation, "scene.style")) return;
        bool applied = false;
        if (!params["fight"].IsNull()) {
            applied = XBase::Scene::SetFightingStyle(params["fight"].AsInt(0));
        }
        if (!params["walk"].IsNull()) {
            applied = XBase::Scene::SetWalkingStyle(params["walk"].AsInt(0)) || applied;
        }
        Reply(id, XBase::Json::Value(applied));
        return;
    }

    if (method == "player.proofs") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerProofs, "player.proofs")) return;
        XBase::Types::ProofState proofs = XBase::Player::GetProofState();
        if (!params["bullet"].IsNull()) proofs.bullet = params["bullet"].AsBool();
        if (!params["collision"].IsNull()) proofs.collision = params["collision"].AsBool();
        if (!params["explosion"].IsNull()) proofs.explosion = params["explosion"].AsBool();
        if (!params["fire"].IsNull()) proofs.fire = params["fire"].AsBool();
        if (!params["melee"].IsNull()) proofs.melee = params["melee"].AsBool();
        if (!params["nonPlayer"].IsNull()) proofs.nonPlayer = params["nonPlayer"].AsBool();
        XBase::Player::SetProofState(proofs);
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "player.runtimeOptions") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerRuntimeEffects, "player.runtimeOptions")) return;
        XBase::Player::RuntimeOptions options = XBase::Player::GetRuntimeOptions();
        if (!params["godMode"].IsNull()) options.godMode = params["godMode"].AsBool();
        if (!params["invisible"].IsNull()) options.invisible = params["invisible"].AsBool();
        if (!params["hardMode"].IsNull()) options.hardMode = params["hardMode"].AsBool();
        if (!params["autoHeal"].IsNull()) options.autoHeal = params["autoHeal"].AsBool();
        if (!params["respawnAtDeathPosition"].IsNull()) options.respawnAtDeathPosition = params["respawnAtDeathPosition"].AsBool();
        if (!params["freezeWantedLevel"].IsNull()) options.freezeWantedLevel = params["freezeWantedLevel"].AsBool();
        if (!params["freeFlyProtection"].IsNull()) options.freeFlyProtection = params["freeFlyProtection"].AsBool();
        if (!params["wantedLevel"].IsNull()) options.wantedLevel = params["wantedLevel"].AsInt();
        XBase::Player::SetRuntimeOptions(options);
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "vehicle.trafficDensity") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleTrafficDensity, "vehicle.trafficDensity")) return;
        if (params["value"].IsNull()) {
            float density = 1.0f;
            XBase::Vehicle::TryGetTrafficDensity(density);
            Reply(id, XBase::Json::Value(static_cast<double>(density)));
            return;
        }
        Reply(id, XBase::Json::Value(XBase::Vehicle::SetTrafficDensity(
            static_cast<float>(params["value"].AsNumber(1.0)))));
        return;
    }
    if (method == "vehicle.colors4") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleColors, "vehicle.colors4")) return;
        XBase::Vehicle::Colors colors = XBase::Vehicle::GetSnapshot().colors;
        if (!params["primary"].IsNull()) colors.primary = params["primary"].AsInt();
        if (!params["secondary"].IsNull()) colors.secondary = params["secondary"].AsInt();
        if (!params["tertiary"].IsNull()) colors.tertiary = params["tertiary"].AsInt();
        if (!params["quaternary"].IsNull()) colors.quaternary = params["quaternary"].AsInt();
        XBase::Vehicle::SetColors(colors);
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.blowUpAll") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleBasic, "vehicle.blowUpAll")) return;
        XBase::Vehicle::BlowUpAll();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.popDoor") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehiclePopDoors, "vehicle.popDoor")) return;
        XBase::Vehicle::PopDoor(params["index"].AsInt());
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.siren") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleSirenOrAlarm, "vehicle.siren")) return;
        Reply(id, XBase::Json::Value(XBase::Vehicle::SetSirenOrAlarm(params["enable"].AsBool(true))));
        return;
    }
    if (method == "vehicle.neon") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleEffectsNeon, "vehicle.neon")) return;
        XBase::VehicleEffects::NeonSettings neon;
        neon.enabled = params["enabled"].AsBool(true);
        neon.red = params["red"].AsInt(255);
        neon.green = params["green"].AsInt(0);
        neon.blue = params["blue"].AsInt(0);
        neon.pulsing = params["pulsing"].AsBool(false);
        Reply(id, XBase::Json::Value(XBase::VehicleEffects::ApplyCurrentNeon(neon)));
        return;
    }

    if (method == "world.pickup") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldPickups, "world.pickup")) return;
        XBase::Types::PickupOptions options;
        options.modelId = static_cast<unsigned int>(params["modelId"].AsInt(1240));
        options.type = static_cast<unsigned char>(params["type"].AsInt(3));
        options.quantity = static_cast<unsigned int>(params["quantity"].AsInt(1));
        options.moneyPerDay = static_cast<unsigned int>(params["moneyPerDay"].AsInt(0));
        options.empty = params["empty"].AsBool(false);
        Reply(id, XBase::Json::Value(XBase::World::SpawnPickup(options)));
        return;
    }
    if (method == "world.removePickups") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldPickups, "world.removePickups")) return;
        Reply(id, XBase::Json::Value(XBase::World::RemoveTrackedPickups()));
        return;
    }

    if (method == "bulletAssist.config") {
        if (!RequireCapability(id, XBase::FeatureCapability::BulletAssistTracking, "bulletAssist.config")) return;
        XBase::BulletAssist::Config config = XBase::BulletAssist::GetConfig();
        if (!params["tracking"].IsNull()) config.tracking = params["tracking"].AsBool();
        if (!params["throughWalls"].IsNull()) config.throughWalls = params["throughWalls"].AsBool();
        if (!params["hardLock"].IsNull()) config.hardLock = params["hardLock"].AsBool();
        if (!params["trackCivilian"].IsNull()) config.trackCivilian = params["trackCivilian"].AsBool();
        if (!params["trackFriend"].IsNull()) config.trackFriend = params["trackFriend"].AsBool();
        if (!params["trackHostile"].IsNull()) config.trackHostile = params["trackHostile"].AsBool();
        if (!params["trackNeutral"].IsNull()) config.trackNeutral = params["trackNeutral"].AsBool();
        if (!params["lockRange"].IsNull()) config.lockRange = static_cast<float>(params["lockRange"].AsNumber(100.0));
        if (!params["maxTargets"].IsNull()) config.maxTargets = params["maxTargets"].AsInt(4);
        if (!params["drawPedBounds"].IsNull()) config.drawPedBounds = params["drawPedBounds"].AsBool();
        if (!params["drawPedCollision"].IsNull()) config.drawPedCollision = params["drawPedCollision"].AsBool();
        if (!params["drawPedSkeleton"].IsNull()) config.drawPedSkeleton = params["drawPedSkeleton"].AsBool();
        if (!params["drawVehicleBounds"].IsNull()) config.drawVehicleBounds = params["drawVehicleBounds"].AsBool();
        if (!params["drawVehicleCollision"].IsNull()) config.drawVehicleCollision = params["drawVehicleCollision"].AsBool();
        if (!params["aimPart"].IsNull()) {
            switch (params["aimPart"].AsInt(1)) {
            case 0: config.aimPart = XBase::BulletAssist::AimPart::Head; break;
            case 2: config.aimPart = XBase::BulletAssist::AimPart::Abdomen; break;
            case 3: config.aimPart = XBase::BulletAssist::AimPart::Legs; break;
            default: config.aimPart = XBase::BulletAssist::AimPart::Chest; break;
            }
        }
        XBase::BulletAssist::SetConfig(config);
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "cheats.random") {
        if (!RequireCapability(id, XBase::FeatureCapability::CheatsRandom, "cheats.random")) return;
        XBase::Cheats::RandomSettings settings = XBase::Cheats::GetRandomSettings();
        if (!params["enabled"].IsNull()) settings.enabled = params["enabled"].AsBool();
        if (!params["showProgress"].IsNull()) settings.showProgress = params["showProgress"].AsBool();
        if (!params["intervalSeconds"].IsNull()) settings.intervalSeconds = params["intervalSeconds"].AsInt(5);
        XBase::Cheats::SetRandomSettings(settings);
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "cheats.randomList") {
        if (!RequireCapability(id, XBase::FeatureCapability::CheatsRandom, "cheats.randomList")) return;
        const std::size_t count = XBase::Cheats::GetRandomCheatCount();
        XBase::Json::Value items;
        for (std::size_t index = 0; index < count; ++index) {
            XBase::Json::Value item;
            const char* name = XBase::Cheats::GetRandomCheatName(index);
            item.Set("index", XBase::Json::Value(static_cast<int>(index)));
            item.Set("name", XBase::Json::Value(std::string(name ? name : "")));
            item.Set("enabled", XBase::Json::Value(XBase::Cheats::IsRandomCheatEnabled(index)));
            items.Push(item);
        }
        XBase::Json::Value result;
        result.Set("count", XBase::Json::Value(static_cast<int>(count)));
        result.Set("items", items);
        Reply(id, result);
        return;
    }
    if (method == "cheats.randomSet") {
        if (!RequireCapability(id, XBase::FeatureCapability::CheatsRandom, "cheats.randomSet")) return;
        Reply(id, XBase::Json::Value(XBase::Cheats::SetRandomCheatEnabled(
            static_cast<std::size_t>(params["index"].AsInt(0)),
            params["enabled"].AsBool(true))));
        return;
    }

    if (method == "player.clothes") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerClothes, "player.clothes")) return;
        Reply(id, XBase::Json::Value(XBase::Player::ApplyClothes(
            params["textureId"].AsInt(0),
            params["modelId"].AsInt(0),
            params["bodyPart"].AsInt(0))));
        return;
    }
    if (method == "player.customSkin") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerAppearance, "player.customSkin")) return;
        const std::string name = params["name"].AsString();
        Reply(id, XBase::Json::Value(XBase::Player::SetCustomSkin(name.c_str())));
        return;
    }
    if (method == "player.stat") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerStats, "player.stat")) return;
        Reply(id, XBase::Json::Value(XBase::Player::SetStat(
            params["id"].AsInt(0),
            static_cast<float>(params["value"].AsNumber(0.0)))));
        return;
    }

    if (method == "ped.gangDensity") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedClassification, "ped.gangDensity")) return;
        const int gangId = params["gangId"].AsInt(0);
        XBase::Ped::SetGangZoneDensity(gangId, params["density"].AsInt(0));
        Reply(id, XBase::Json::Value(XBase::Ped::GetGangZoneDensity(gangId)));
        return;
    }
    if (method == "ped.gangMemberModel") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedClassification, "ped.gangMemberModel")) return;
        XBase::Ped::SetGangMemberModel(
            static_cast<unsigned int>(params["gangId"].AsInt(0)),
            static_cast<unsigned int>(params["slot"].AsInt(0)),
            static_cast<unsigned int>(params["modelId"].AsInt(0)));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "ped.gangWeapons") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedAttributes, "ped.gangWeapons")) return;
        XBase::Ped::SetGangWeapons(
            static_cast<unsigned int>(params["gangId"].AsInt(0)),
            params["weapon1"].AsInt(0),
            params["weapon2"].AsInt(0),
            params["weapon3"].AsInt(0));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "ped.resetGangModels") {
        if (!RequireCapability(id, XBase::FeatureCapability::PedClassification, "ped.resetGangModels")) return;
        XBase::Ped::ResetGangModels();
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "vehicle.speedLock") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleCheats, "vehicle.speedLock")) return;
        XBase::Vehicle::ApplySpeedLock(static_cast<float>(params["speed"].AsNumber(60.0)));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.targetSpeed") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleCheats, "vehicle.targetSpeed")) return;
        XBase::Vehicle::ApplyTargetSpeed(static_cast<float>(params["speed"].AsNumber(60.0)));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.restoreSpeed") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleCheats, "vehicle.restoreSpeed")) return;
        XBase::Vehicle::RestoreTargetSpeed();
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.upgrade") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleUpgrades, "vehicle.upgrade")) return;
        const unsigned int modelId = static_cast<unsigned int>(params["modelId"].AsInt(1000));
        if (params["enable"].AsBool(true)) {
            XBase::Vehicle::AddUpgrade(modelId);
        } else {
            XBase::Vehicle::RemoveUpgrade(modelId);
        }
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "camera.settings") {
        if (!RequireCapability(id, XBase::FeatureCapability::CameraFreecam, "camera.settings")) return;
        XBase::Camera::Settings settings = XBase::Camera::GetSettings();
        if (!params["freecamFov"].IsNull()) settings.freecamFov = static_cast<float>(params["freecamFov"].AsNumber(70.0));
        if (!params["freecamSpeed"].IsNull()) settings.freecamSpeed = params["freecamSpeed"].AsInt(1);
        if (!params["topDownZoom"].IsNull()) settings.topDownZoom = params["topDownZoom"].AsInt(40);
        XBase::Camera::SetSettings(settings);
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "world.weatherRelease") {
        if (!RequireCapability(id, XBase::FeatureCapability::WorldWeather, "world.weatherRelease")) return;
        XBase::World::ReleaseWeather();
        Reply(id, XBase::Json::Value());
        return;
    }

    if (method == "vehicle.seat") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleBasic, "vehicle.seat")) return;
        XBase::Vehicle::WarpToSeat(params["index"].AsInt(0));
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "vehicle.resetColors") {
        if (!RequireCapability(id, XBase::FeatureCapability::VehicleColors, "vehicle.resetColors")) return;
        XBase::Vehicle::Colors colors;
        XBase::Vehicle::SetColors(colors);
        Reply(id, XBase::Json::Value());
        return;
    }
    if (method == "weapon.removePickups") {
        if (!RequireCapability(id, XBase::FeatureCapability::WeaponBasic, "weapon.removePickups")) return;
        Reply(id, XBase::Json::Value(XBase::Weapon::RemoveTrackedPickups()));
        return;
    }
    if (method == "player.aimSkin") {
        if (!RequireCapability(id, XBase::FeatureCapability::PlayerAimSkinChanger, "player.aimSkin")) return;
        Reply(id, XBase::Json::Value(XBase::Player::ApplyAimSkinChanger()));
        return;
    }

    if (method == "ui.notice") {
        const std::string text = params["text"].AsString();
        if (text.empty()) {
            Fail(id, "missing text");
            return;
        }
        XBase::Host::QueueMessage(text.c_str());
        Reply(id, XBase::Json::Value());
        return;
    }

    Fail(id, std::string("unknown method: ") + method);
}

bool s_installed = false;

} // namespace

namespace XBase::WebBridge {

bool RegisterMethod(const std::string& method, MethodHandler handler) {
    if (method.empty() || !handler) {
        return false;
    }
    s_customMethods[method] = std::move(handler);
    return true;
}

void UnregisterMethod(const std::string& method) {
    s_customMethods.erase(method);
}

void Install() {
    Install(XBase::WebView::CurrentInstance());
}

void Install(XBase::WebView::WebViewId id) {
    XBase::WebView::SetMessageHandler(id, HandleMessage);
    XBase::WebView::InjectScript(id, ClientScript);
    s_installedInstances.insert(id);
    s_installed = true;
    if (id == XBase::WebView::DefaultInstance) {
        XBase::Log::Info("WebBridge: 已注册网页调用通道");
    }
}

void Shutdown() {
    Shutdown(XBase::WebView::CurrentInstance());
}

void Shutdown(XBase::WebView::WebViewId id) {
    XBase::WebView::SetMessageHandler(id, nullptr);
    s_installedInstances.erase(id);
    s_installed = !s_installedInstances.empty();
}

bool IsInstalled() {
    return s_installed;
}

bool Emit(const std::string& event, const Json::Value& payload) {
    if (!s_installed || event.empty()) {
        return false;
    }

    Json::Value message;
    message.Set("event", Json::Value(event));
    message.Set("payload", payload);
    const std::string json = message.Serialize(false);
    const XBase::WebView::WebViewId current = XBase::WebView::CurrentInstance();
    if (current != XBase::WebView::DefaultInstance) {
        return XBase::WebView::PostJson(current, json);
    }

    bool sent = false;
    for (const XBase::WebView::WebViewId id : s_installedInstances) {
        sent = XBase::WebView::PostJson(id, json) || sent;
    }
    return sent;
}

bool Emit(XBase::WebView::WebViewId id, const std::string& event, const Json::Value& payload) {
    if (!s_installed || event.empty()) {
        return false;
    }

    Json::Value message;
    message.Set("event", Json::Value(event));
    message.Set("payload", payload);
    return XBase::WebView::PostJson(id, message.Serialize(false));
}

} // namespace XBase::WebBridge
