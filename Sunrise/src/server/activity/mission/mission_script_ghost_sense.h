#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/bap/activity_message/sense_update.h"

namespace sunrise::server::activity::mission {

/** Ghost links one mission watches at once. */
inline constexpr std::size_t kGhostLinkCapacity = 8;

/** Type-65 Sense root ordinals: active, elapsed over required duration, accepted generation. */
inline constexpr std::uint16_t kGhostActiveOrdinal = 0;
inline constexpr std::uint16_t kGhostProgressOrdinal = 1;
inline constexpr std::uint16_t kGhostGenerationOrdinal = 2;
inline constexpr std::uint8_t kGhostSeenActive = 0x01;
inline constexpr std::uint8_t kGhostSeenProgress = 0x02;
inline constexpr std::uint8_t kGhostSeenGeneration = 0x04;
inline constexpr std::uint8_t kGhostSeenAll =
    kGhostSeenActive | kGhostSeenProgress | kGhostSeenGeneration;

/** Last Ghost-link level seen for one slot. */
struct GhostLevel final {
    std::int32_t generation{};
    float progress{};
    std::uint8_t seen{};
    bool active{};
    bool operator==(const GhostLevel&) const = default;
};

/** One retained Ghost-link level with the slot it was reported for. */
struct GhostLinkRow final {
    GhostLevel level{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
};

/**
 * Merges one decoded type-65 body into the retained level.
 * @return True when the level changed and all three fields are known.
 */
[[nodiscard]] inline bool update_ghost_level(
    GhostLevel& level,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::uint32_t root) noexcept {
    GhostLevel next = level;
    for (const auto& value : values) {
        if (!value.present || value.schemaRow != root) {
            continue;
        }
        switch (value.fieldOrdinal) {
        case kGhostActiveOrdinal:
            next.active = value.unsignedValue != 0;
            next.seen |= kGhostSeenActive;
            break;
        case kGhostProgressOrdinal:
            if (!std::isfinite(value.realValue) || value.realValue < 0) {
                return false;
            }
            next.progress = value.realValue;
            next.seen |= kGhostSeenProgress;
            break;
        case kGhostGenerationOrdinal:
            next.generation = static_cast<std::int32_t>(value.signedValue);
            next.seen |= kGhostSeenGeneration;
            break;
        default:
            break;
        }
    }
    const bool changed = !(next == level);
    level = next;
    return changed && next.seen == kGhostSeenAll;
}

} // namespace sunrise::server::activity::mission
