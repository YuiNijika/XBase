#pragma once

#include <XBase/Core.h>
#include <XBase/Capabilities.h>
#include <XBase/Player.h>
#include <XBase/Ped.h>
#include <XBase/Vehicle.h>
#include <XBase/World.h>
#include <XBase/Weapon.h>
#include <XBase/Teleport.h>
#include <XBase/Scene.h>
#include <XBase/Visual.h>
#include <XBase/BulletAssist.h>
#include <XBase/Overlay.h>
#include <XBase/Camera.h>
#include <XBase/Cheats.h>
#include <XBase/VehicleEffects.h>
#include <XBase/WebView.h>
#include <XBase/Panel.h>

#include <cstddef>
#include <iterator>

namespace XBase::Core::Detail {

using LifecycleFn = void (*)();
using InitializationCheck = bool (*)();

inline void Noop() {}
inline bool Initialized() { return true; }
inline void WebViewDomainInit() { WebView::Init(); }

struct DomainLifecycle {
    Domain domain;
    LifecycleFn init;
    InitializationCheck isInitialized;
    LifecycleFn notifyGameInit;
    LifecycleFn process;
    LifecycleFn shutdown;
};

inline const DomainLifecycle kDomains[] = {
    {Domain::Player,       Noop,               Initialized,                  Player::NotifyGameInit,  Player::Process,       Player::Shutdown},
    {Domain::Ped,          Ped::Init,          Initialized,                  Ped::NotifyGameInit,     Ped::Process,          Ped::Shutdown},
    {Domain::Vehicle,      Noop,               Initialized,                  Vehicle::NotifyGameInit, Vehicle::Process,      Vehicle::Shutdown},
    {Domain::World,        Noop,               Initialized,                  World::NotifyGameInit,   World::Process,        World::Shutdown},
    {Domain::Weapon,       Noop,               Initialized,                  Weapon::NotifyGameInit,  Weapon::Process,       Weapon::Shutdown},
    {Domain::Teleport,     Noop,               Initialized,                  Noop,                    Teleport::Process,      Noop},
    {Domain::Scene,        Noop,               Initialized,                  Scene::NotifyGameInit,   Scene::Process,         Scene::Shutdown},
    {Domain::Visual,       Noop,               Initialized,                  Visual::NotifyGameInit,  Visual::Process,        Visual::Shutdown},
    {Domain::BulletAssist, BulletAssist::Init, BulletAssist::IsInitialized, Noop,                    BulletAssist::Process, BulletAssist::Shutdown},
    {Domain::Overlay,      Overlay::Init,      Initialized,                  Noop,                    Overlay::Process,      Overlay::Shutdown},
    {Domain::Camera,       Noop,               Initialized,                  Camera::NotifyGameInit,  Camera::Process,       Camera::Shutdown},
    {Domain::Cheats,       Cheats::Init,       Initialized,                  Cheats::NotifyGameInit,  Cheats::Process,       Cheats::Shutdown},
    {Domain::VehicleEffects, VehicleEffects::Init, Initialized,              VehicleEffects::NotifyGameInit, VehicleEffects::Process, VehicleEffects::Shutdown},
    {Domain::WebView,      WebViewDomainInit, WebView::IsInitialized,       WebView::NotifyGameInit, WebView::Process,     WebView::Shutdown},
    // 面板只在打开时才建网页视图，域本身只负责标记就绪与轮询热键
    {Domain::Panel,        Panel::Init,       Panel::IsInitialized,         Panel::NotifyGameInit,   Panel::Process,        Panel::Shutdown},
};

inline bool IsEnabled(DomainMask mask, Domain domain) {
    return (mask & DomainBit(domain)) != 0;
}

inline Capability DomainCapability(Domain domain) {
    switch (domain) {
    case Domain::Player: return Capability::Player;
    case Domain::Ped: return Capability::Ped;
    case Domain::Vehicle: return Capability::Vehicle;
    case Domain::World: return Capability::World;
    case Domain::Weapon: return Capability::Weapon;
    case Domain::Teleport: return Capability::Teleport;
    case Domain::Scene: return Capability::Scene;
    case Domain::Visual: return Capability::Visual;
    case Domain::BulletAssist: return Capability::BulletAssist;
    case Domain::Overlay: return Capability::Overlay;
    case Domain::Camera: return Capability::Camera;
    case Domain::Cheats: return Capability::Cheats;
    case Domain::VehicleEffects: return Capability::VehicleEffects;
    case Domain::WebView: return Capability::WebView;
    case Domain::Panel: return Capability::Panel;
    }
    return Capability::Player;
}

inline DomainMask SupportedDomains(DomainMask requested) {
    DomainMask supported = 0;
    for (const DomainLifecycle& entry : kDomains) {
        if (IsEnabled(requested, entry.domain) && HasCapability(DomainCapability(entry.domain))) {
            supported |= DomainBit(entry.domain);
        }
    }
    return supported;
}

inline void SetEnabledDomains(
    DomainMask& requested,
    DomainMask& active,
    bool gameInitialized,
    DomainMask nextRequested) {
    requested = nextRequested & AllDomains;
    const DomainMask candidates = SupportedDomains(requested);

    for (std::size_t i = std::size(kDomains); i > 0; --i) {
        const DomainLifecycle& entry = kDomains[i - 1];
        if (IsEnabled(active, entry.domain) && !IsEnabled(candidates, entry.domain)) {
            entry.shutdown();
            active &= ~DomainBit(entry.domain);
        }
    }

    for (const DomainLifecycle& entry : kDomains) {
        if (IsEnabled(active, entry.domain) || !IsEnabled(candidates, entry.domain)) continue;
        entry.init();
        if (!entry.isInitialized()) {
            entry.shutdown();
            continue;
        }
        active |= DomainBit(entry.domain);
        if (gameInitialized) entry.notifyGameInit();
    }
}

inline void NotifyGameInit(DomainMask active, bool& gameInitialized) {
    if (gameInitialized) return;
    gameInitialized = true;
    for (const DomainLifecycle& entry : kDomains) {
        if (IsEnabled(active, entry.domain)) entry.notifyGameInit();
    }
}

inline void Process(DomainMask enabled, bool gameInitialized, bool worldReady) {
    if (!gameInitialized || !worldReady) return;
    for (const DomainLifecycle& entry : kDomains) {
        if (IsEnabled(enabled, entry.domain)) entry.process();
    }
}

inline void Shutdown(DomainMask& requested, DomainMask& active, bool& gameInitialized) {
    for (std::size_t i = std::size(kDomains); i > 0; --i) {
        const DomainLifecycle& entry = kDomains[i - 1];
        if (IsEnabled(active, entry.domain)) entry.shutdown();
    }
    requested = 0;
    active = 0;
    gameInitialized = false;
}

} // namespace XBase::Core::Detail