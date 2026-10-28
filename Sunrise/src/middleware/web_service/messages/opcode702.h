#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../../state/account/inventory/seen_state.h"
#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode702 {

/** Web Service opcode of the character object B write-back. */
inline constexpr std::uint16_t kOpcode = 702;
/** The 5360-byte character mirror packs into at most 4800 bytes; shorter bodies are valid. */
inline constexpr std::size_t kPayloadSize = 4800;
/**
 * Join-policy bit 3. The client raises it only in step 38, and only while the type-17
 * lifetime body resolves with field `.2` at 0, so it marks world entry.
 */
inline constexpr std::uint8_t kInWorld = 8;

/** Supported fields from the character writeback. */
struct Request {
    /**
     * Five-bit field at objB `+12068`, schema path `.0.11.1.0.0.4`: the join-policy flags of the
     * client's current group session, copied from parameter 1 `active-join-controls`.
     */
    std::uint8_t worldState{};
    bool hasWorldState{};
    /** User, party and limited join-slot counts of that group session, bias 128. */
    std::array<std::int8_t, 3> activityBytes{};
    /** The local join mode derived from parameter 9, bias 1. */
    std::int8_t activitySelector{};
    std::optional<state::account::inventory::CharacterNewItems> newItems;
};

/**
 * Reads the complete character writeback with optional groups and bounded zero padding.
 * @param message Parsed Web Service envelope.
 * @param request Receives only fields present in a valid body.
 * @return False for malformed or truncated fields.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode702
