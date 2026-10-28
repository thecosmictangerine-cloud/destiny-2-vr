#pragma once

#include <algorithm>

#include "mission_script_sdk_bridge.h"

namespace sunrise::server::activity::mission::sdk_bridge::actor_ability {

/** Accepts only an extracted group/request pair owned by this exact actor slot. */
[[nodiscard]] inline bool resolve(const void* context,
                                  std::uint32_t slotRow,
                                  std::uint32_t groupHash,
                                  std::uint32_t requestHash) noexcept {
    namespace sdk = state::activity_sdk;
    const auto* const view = static_cast<const sdk::BoundView*>(context);
    if (view == nullptr || view->catalog == nullptr || sdk::bound_activity(*view) == nullptr
        || sdk::bound_scenario(*view) == nullptr) {
        return false;
    }
    const auto abilities = view->catalog->actor_abilities();
    const auto first = std::lower_bound(
        abilities.begin(), abilities.end(), slotRow, [](const auto& row, std::uint32_t slot) {
            return row.slotIndex < slot;
        });
    for (auto row = first; row != abilities.end() && row->slotIndex == slotRow; ++row) {
        if (row->groupHash == groupHash && row->requestHash == requestHash) {
            return true;
        }
    }
    return false;
}

/** The client indexes point parameters without a bound, so the SDK must prove the selected row. */
[[nodiscard]] inline bool
target(const void* context, std::uint32_t slotRow, std::uint32_t selectorIndex) noexcept {
    namespace sdk = state::activity_sdk;
    const auto* const view = static_cast<const sdk::BoundView*>(context);
    if (view == nullptr || view->catalog == nullptr || sdk::bound_activity(*view) == nullptr
        || sdk::bound_scenario(*view) == nullptr) {
        return false;
    }
    const auto targets = view->catalog->actor_ability_targets();
    const auto found = std::lower_bound(
        targets.begin(), targets.end(), slotRow, [](const auto& row, std::uint32_t slot) {
            return row.slotIndex < slot;
        });
    return found != targets.end() && found->slotIndex == slotRow
           && selectorIndex < found->selectorCount;
}

} // namespace sunrise::server::activity::mission::sdk_bridge::actor_ability
