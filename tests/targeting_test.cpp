#include <XBase/Targeting.h>
#include <XBase/Capabilities.h>
#include <XBase/Hooks.h>
#include <XBase/Runtime.h>

#include "../src/backends/TargetingBackend.h"
#include "../src/backends/TargetingInteraction.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace XBase::Targeting;
XBase::Runtime::GameTarget game = XBase::Runtime::GameTarget::ViceCity;
Target target{Kind::Vehicle, XBase::EntityId{42}, 130, {}, 5, 1000, false};
Target command;
Binding executed;
int pending = 0;
int executions = 0;
int resets = 0;
int clears = 0;
bool menuVisible = false;
bool modelReused = false;
bool suppressed = false;
}

namespace XBase {
bool HasCapability(FeatureCapability feature) {
    if (feature == FeatureCapability::VehicleDoors || feature == FeatureCapability::VehicleUpgrades
        || feature == FeatureCapability::VehiclePaintjob) {
        return game == Runtime::GameTarget::SanAndreas;
    }
    return true;
}
namespace Runtime {
GameTarget GetGameTarget() { return game; }
}
namespace Hooks {
bool IsMenuVisible() { return menuVisible; }
bool IsKeyboardCaptureActive() { return false; }
void SetMiddleInputSuppressed(bool value) { suppressed = value; }
}
}

namespace XBase::Detail::TargetingBackend {
void Collect(const Config&, std::vector<Target>& targets) { targets = {target}; }
void Draw(const Config&, const std::vector<Target>&) {}
bool PickAtCursor(const Config&, const std::vector<Target>&, Target&) { return false; }
void ResetPointerState() { ++resets; pending = 0; }
void SetLabels(const Labels&) {}
bool Refresh(Target& value) {
    if (modelReused) ++value.modelId;
    return true;
}
int ConsumeAction(Target& value) { value = command; const int result = pending; pending = 0; return result; }
bool SetHealth(Kind, EntityId, float) { return true; }
bool Delete(Kind, EntityId) { return true; }
bool ExecuteAction(const Target&, int) { return true; }
bool ExecuteBinding(const Target&, const Binding& binding) { ++executions; executed = binding; return true; }
void ProcessEffects() {}
void ClearEffects() { ++clears; }
}

int main() {
    Config config;
    assert(config.pedMenu[3].action == Action::Kill);
    assert(config.vehicleMenu[3].action == Action::Ignite);
    assert(config.pedMenu.size() == SlotCount);
    const auto vehicleActions = GetActions(Kind::Vehicle);
    assert(std::any_of(vehicleActions.begin(), vehicleActions.end(), [](const ActionInfo& info) {
        return info.action == Action::EnterVehicle && info.supported && info.parameter == Parameter::None;
    }));
    config.enabled = true;
    config.radius = std::numeric_limits<float>::quiet_NaN();
    config.hitRadius = std::numeric_limits<float>::infinity();
    config.maxTargets = 500;
    config.vehicleMenu[0] = {Action::Colors, 999, -1, 999, -1};
    config.vehicleMenu[1] = {static_cast<Action>(999)};
    config.vehicleMenu[2] = {Action::Paintjob};
    SetConfig(config);
    config = GetConfig();
    assert(config.radius == 80 && config.hitRadius == 160 && config.maxTargets == 64);
    assert(config.vehicleMenu[0].value == 255 && config.vehicleMenu[0].secondary == 0);
    assert(config.vehicleMenu[0].tertiary == 255 && config.vehicleMenu[0].quaternary == 0);
    assert(config.vehicleMenu[1].action == Action::None && config.vehicleMenu[2].action == Action::None);

    Process();
    assert(suppressed && Select(target.kind, target.id));
    command = target;
    pending = 1;
    Process();
    assert(executions == 1 && executed.action == Action::Colors && executed.value == 255);
    assert(!GetSelected(command));

    target = {Kind::Ped, XBase::EntityId{43}, 7, {}, 5, 100, false};
    Process();
    assert(Select(target.kind, target.id));
    command = target;
    pending = 5;
    Process();
    assert(executions == 2 && executed.action == Action::Disarm);
    target = {Kind::Vehicle, XBase::EntityId{42}, 130, {}, 5, 1000, false};
    executions = 1;
    Process();

    assert(Select(target.kind, target.id));
    command = target;
    ++command.id.value;
    pending = 2;
    Process();
    assert(executions == 1);

    command = target;
    pending = 2;
    modelReused = true;
    Process();
    assert(executions == 1);
    modelReused = false;

    SetConfig(config);
    Process();
    assert(Select(target.kind, target.id));
    command = target;
    pending = 2;
    menuVisible = true;
    Process();
    assert(executions == 1 && !GetSelected(command) && !suppressed);
    menuVisible = false;
    const int previousResets = resets;
    SetConfig(config);
    assert(resets > previousResets);
    NotifyGameInit();
    assert(GetTargets().empty() && clears == 1);
    Shutdown();
    assert(!GetConfig().enabled && clears == 2);

    using XBase::Detail::TargetingBackend::Interaction;
    constexpr float pi = 3.14159265359f;
    constexpr int slots[] = {1, 4, 5, 2, 6, 7};
    for (int slot = 0; slot < 6; ++slot) {
        Interaction interaction;
        interaction.Update(&target, false, false, 0, 0, 0);
        assert(interaction.Update(&target, true, false, 0, 0, 1).selected);
        const float angle = -pi / 2 + slot * pi / 3;
        interaction.Update(&target, true, false, std::cos(angle) * 90, std::sin(angle) * 90, 1.3);
        assert(interaction.Action() == slots[slot]);
        assert(interaction.Update(&target, false, false, 0, 0, 1.4).action == slots[slot]);
    }
    Interaction interaction;
    interaction.Update(&target, false, false, 0, 0, 0);
    interaction.Update(&target, true, false, 0, 0, 1);
    assert(interaction.Update(&target, false, false, 0, 0, 1.1).action == 3);
    interaction.Update(&target, true, false, 0, 0, 2);
    assert(interaction.Update(&target, true, true, 0, 0, 2.2).action == 3);
    assert(!interaction.Update(&target, true, false, 0, 0, 2.3).selected);
    std::cout << "Targeting config, capabilities, command guards, lifecycle and six radial sectors passed\n";
}
