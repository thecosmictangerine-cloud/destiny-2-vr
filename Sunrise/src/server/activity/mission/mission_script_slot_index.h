#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../state/activity_sdk/runtime.h"

// Slot lookups for the bound scenario, built once per catalog and scenario.
namespace sunrise::server::activity::mission::slot_index {

/** Slot fields a Sense row names. A zero senseSchema matches any schema. */
struct SenseKey final {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint32_t senseSchema{};
    std::uint32_t slotIndex{};
    std::uint32_t slotType{};
};

/** One slot of the bound scenario: its one-based local row and its catalog row. */
struct Found final {
    std::uint32_t localRow{};
    std::uint32_t nativeRow{};
};

/** @return False when the row is outside the scenario or the scenario is invalid. */
[[nodiscard]] bool
by_row(const state::activity_sdk::BoundView& view, std::uint32_t localRow, Found& output) noexcept;

/** @return False unless exactly one slot has this id, name or alias. */
[[nodiscard]] bool
by_id(const state::activity_sdk::BoundView& view, std::string_view id, Found& output) noexcept;

/** @return False unless exactly one slot matches every field of the key. */
[[nodiscard]] bool
by_sense(const state::activity_sdk::BoundView& view, const SenseKey& key, Found& output) noexcept;

/** @return Slot count of the scenario, each object counted once; zero when invalid. */
[[nodiscard]] std::size_t count(const state::activity_sdk::BoundView& view) noexcept;

} // namespace sunrise::server::activity::mission::slot_index
