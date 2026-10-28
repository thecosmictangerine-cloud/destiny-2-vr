#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace sunrise::state::account::inventory {

/** The profile and character writebacks carry 704 and 352 new-item bits. */
using ProfileNewItems = std::array<std::uint32_t, 22>;
using CharacterNewItems = std::array<std::uint32_t, 11>;

/** A delivered acquisition can temporarily place an item outside its usual row. */
struct PresentedItemRow {
    std::uint64_t instanceSoid{};
    std::uint16_t inventoryRow{};
};

[[nodiscard]] bool record_profile_seen(const ProfileNewItems& newItems) noexcept;
[[nodiscard]] bool
record_character_seen(const CharacterNewItems& newItems,
                      std::span<const PresentedItemRow> presentation = {}) noexcept;

/** A clear bit means the item at that published row has been seen. */
template <std::size_t N>
[[nodiscard]] bool seen_at(const std::array<std::uint32_t, N>& bits, std::size_t row) noexcept {
    // Each published seen-state word holds 32 item bits.
    constexpr std::size_t kWordBits = 32;
    return row < N * kWordBits && (bits[row / kWordBits] & (1U << (row % kWordBits))) == 0;
}

} // namespace sunrise::state::account::inventory
