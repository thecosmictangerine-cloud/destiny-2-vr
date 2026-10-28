#pragma once

#include "../../../middleware/bap/activity_message/squad_objective_state.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {

/** Qualifies reported costs against the exact transported objective and evaluation revision. */
inline void qualify_objective_costs(const RuntimeInstance& instance, host::Event& event) noexcept {
    namespace objective = middleware::bap::activity_message::squad_objective;
    std::vector<host::PendingScriptableOverride> estate{};
    if (!host::scriptable_auth_estate(
            instance.view.binding, instance.view.activityClientGeneration, estate)) {
        return;
    }
    for (const auto& retained : estate) {
        const auto& target = retained.target;
        if (target.registryKey != event.firstRegistryKey || target.objectTag != event.slotObjectTag
            || target.slotIndex != event.firstSlotIndex || target.slotType != event.firstSlotType) {
            continue;
        }
        objective::State assignment{};
        if (objective::read_state(std::span(retained.body).first(retained.byteCount),
                                  retained.bitCount,
                                  assignment)) {
            event.squadObjectiveRegistryKey = assignment.registryKey;
            event.squadObjectiveSlotIndex = assignment.objectiveIndex;
            event.squadObjectiveTaskGroup = assignment.taskGroup;
            event.squadObjectiveCostQualified =
                event.squadObjectiveRevision != 0
                && assignment.revision == event.squadObjectiveRevision;
        }
        return;
    }
}

} // namespace sunrise::server::activity::mission
