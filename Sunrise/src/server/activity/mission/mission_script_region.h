#pragma once

#include "../host_runtime.h"

namespace sunrise::server::activity::mission {

/**
 * Pending-region changes never establish entry into a held region.
 * @param source Exact accepted client-state after-image.
 * @param output Receives the region transition with the original input ownership.
 * @return True only when the native current-region change flag names a new held region.
 */
[[nodiscard]] inline bool make_region_changed(const host::Event& source,
                                              host::Event& output) noexcept {
    output = {};
    if (source.kind != host::EventKind::clientStateChanged || !source.clientStateHasCurrentRegion
        || source.currentRegionIndex < 0 || source.currentRegionIndex != source.heldRegionIndex
        || source.currentRegionIndex == source.previousRegionIndex) {
        return false;
    }
    output.kind = host::EventKind::regionChanged;
    output.binding = source.binding;
    output.sequence = source.sequence;
    output.tick = source.tick;
    output.attemptGeneration = source.attemptGeneration;
    output.sourceGeneration = source.sourceGeneration;
    output.missionSequence = source.missionSequence;
    output.regionIndex = source.currentRegionIndex;
    output.previousRegionIndex = source.previousRegionIndex;
    return true;
}

} // namespace sunrise::server::activity::mission
