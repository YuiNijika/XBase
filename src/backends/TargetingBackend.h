#pragma once

#include <vector>

#include <XBase\Targeting.h>

namespace XBase::Detail::TargetingBackend {

void Collect(const Targeting::Config& config, std::vector<Targeting::Target>& targets);
void Draw(const Targeting::Config& config, const std::vector<Targeting::Target>& targets);
bool PickAtCursor(const Targeting::Config& config,
    const std::vector<Targeting::Target>& targets,
    Targeting::Target& target);
void ResetPointerState();
void UpdatePointerInput();
void SetLabels(const Targeting::Labels& labels);
bool Refresh(Targeting::Target& target);
// Draw records commands; only the game-thread Process consumes entity mutations.
int ConsumeAction(Targeting::Target& target);
bool SetHealth(Targeting::Kind kind, EntityId id, float health);
bool Delete(Targeting::Kind kind, EntityId id);
bool ExecuteAction(const Targeting::Target& target, int action);
bool ExecuteBinding(const Targeting::Target& target, const Targeting::Binding& binding);
void ProcessEffects();
void ClearEffects();

} // namespace XBase::Detail::TargetingBackend
