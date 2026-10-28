#include "mission_script_sdk_bridge.h"

namespace sunrise::server::activity::mission::sdk_bridge {
namespace sdk = state::activity_sdk;
namespace format = state::activity_sdk::format;

/** Copies the bound activity's binding definition out of the catalog. */
bool activity_binding_definition(const sdk::Catalog& catalog,
                                 const format::Activity& activity,
                                 lua_vm::ActivityBindingDefinition& output) noexcept {
    output = {};
    const auto activities = catalog.activities();
    if (activities.empty()) {
        return false;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(activities.data());
    const auto selected = reinterpret_cast<std::uintptr_t>(&activity);
    const std::size_t bytes = activities.size_bytes();
    if (selected < first || selected - first >= bytes
        || (selected - first) % sizeof(format::Activity) != 0) {
        return false;
    }
    output.internalName = catalog.string(activity.internalName);
    output.displayName = catalog.string(activity.displayName);
    output.selectedActivityRootTag = activity.selectedActivityRootTag;
    output.selectedScenarioTag = activity.selectedScenarioTag;
    output.matchmakingConfigTag = activity.matchmakingConfigTag;
    output.joinStatus = activity.joinStatus;
    output.bindingDisposition = activity.bindingDisposition;
    output.bindingReason = activity.bindingReason;
    output.bindingEvidenceBasis = activity.bindingEvidenceBasis;
    output.runnableStatus = activity.runnableStatus;
    output.fullSdkAcceptable =
        (activity.bindingFlags & format::kActivityBindingFullSdkAcceptable) != 0;
    output.hasInternalName = (activity.bindingFlags & format::kActivityBindingHasInternalName) != 0;
    output.hasMatchmakingConfig =
        (activity.bindingFlags & format::kActivityBindingHasMatchmakingConfig) != 0;
    return true;
}

/** @return The binding tag span one kind names, or empty when the kind is unknown. */
std::span<const format::ActivityBindingTag>
activity_binding_tags(const sdk::Catalog& catalog,
                      const format::Activity& activity,
                      lua_vm::ActivityBindingTagKind kind) noexcept {
    switch (kind) {
    case lua_vm::ActivityBindingTagKind::activityRootCandidates:
        return sdk::activity_root_candidate_tags(catalog, activity);
    case lua_vm::ActivityBindingTagKind::scenarioNameCandidates:
        return sdk::activity_scenario_name_candidate_tags(catalog, activity);
    case lua_vm::ActivityBindingTagKind::evidenceRoots:
        return sdk::activity_evidence_root_tags(catalog, activity);
    }
    return {};
}

} // namespace sunrise::server::activity::mission::sdk_bridge
