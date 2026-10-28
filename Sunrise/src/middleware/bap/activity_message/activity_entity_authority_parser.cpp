/**
 * Exact parsers for client-to-host entity-authority msgs 26, 27, 29, 31, 32 and 33.
 * Fixed u32 masks decode into canonical slot bytes.
 */

#include <bit>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "entity_authority.h"

namespace sunrise::middleware::bap::activity_message::entity_authority {
namespace {

/** @return True when only zero tail padding remains. */
[[nodiscard]] bool finish_padding(encoding::bits::Reader& reader) noexcept {
    const std::size_t paddingBits = reader.remaining_bits();
    std::uint64_t padding = 0;
    return paddingBits < entity_slots::kBitsPerMaskByte
           && reader.read(static_cast<std::uint8_t>(paddingBits), padding) && padding == 0
           && reader.remaining_bits() == 0;
}

/** Removes the signed midpoint bias from one correlation field. */
[[nodiscard]] constexpr std::int32_t decode_correlation(std::uint32_t wire) noexcept {
    // The correlation field uses a signed 32-bit midpoint bias.
    constexpr std::uint32_t kSignedBias = 0x80000000U;
    return std::bit_cast<std::int32_t>(wire - kSignedBias);
}

/** Reads the exact selector and aligned mask shared by msgs 26 and 33. */
[[nodiscard]] bool read_selector_and_mask(std::span<const std::byte> payload,
                                          std::size_t expectedSize,
                                          Release& release) noexcept {
    if (payload.size() != expectedSize) {
        return false;
    }
    release.selector = std::to_integer<std::uint8_t>(payload.front());
    if (release.selector > kMaximumSelector) {
        return false;
    }
    return entity_slots::decode_entity_slots(payload.subspan(kSelectorSize), release.mask);
}

} // namespace

/** Parses msg 26, whose mask starts on a byte boundary after the selector. */
bool parse_abandon(std::span<const std::byte> payload, Release& release) noexcept {
    Release parsed{};
    if (!read_selector_and_mask(payload, kAbandonByteCount, parsed)) {
        return false;
    }
    encoding::bits::Reader reader(payload);
    std::uint64_t stored = 0;
    if (!reader.skip(kSelectorWidth + entity_slots::kSlotCount)
        || !reader.read(kReasonWidth, stored) || !finish_padding(reader)) {
        return false;
    }
    parsed.reason = static_cast<std::int32_t>(stored) - kReasonBias;
    parsed.hasReason = true;
    release = parsed;
    return true;
}

/** Parses msg 33, which is the selector and the mask with no reason. */
bool parse_abdicate(std::span<const std::byte> payload, Release& release) noexcept {
    Release parsed{};
    if (!read_selector_and_mask(payload, kAbdicateByteCount, parsed)) {
        return false;
    }
    release = parsed;
    return true;
}

/** Parses the reason followed immediately by the unaligned 8,192-bit mask of msg 27. */
bool parse_request_purge(std::span<const std::byte> payload, PurgeRequest& request) noexcept {
    if (payload.size() != kRequestPurgeByteCount) {
        return false;
    }
    PurgeRequest parsed{};
    encoding::bits::Reader reader(payload);
    std::uint64_t stored = 0;
    if (!reader.read(kReasonWidth, stored)) {
        return false;
    }
    parsed.reason = static_cast<std::int32_t>(stored) - kReasonBias;
    if (!entity_slots::read_mask(reader, parsed.mask)) {
        return false;
    }
    if (!finish_padding(reader)) {
        return false;
    }
    request = parsed;
    return true;
}

/** Parses msg 29, 31 or 32. */
bool parse_query_answer(std::uint32_t messageType,
                        std::span<const std::byte> payload,
                        QueryAnswer& answer) noexcept {
    std::size_t expectedSize = 0;
    if (messageType == kResetAcknowledgementMessageType) {
        expectedSize = kResetAcknowledgementByteCount;
    } else if (messageType == kQueryPerBubbleMessageType) {
        expectedSize = kQueryPerBubbleByteCount;
    } else if (messageType == kQueryResponseMessageType) {
        expectedSize = kQueryResponseByteCount;
    } else {
        return false;
    }

    if (payload.size() != expectedSize) {
        return false;
    }

    QueryAnswer parsed{};
    parsed.correlation =
        decode_correlation(encoding::read_u32_be(payload.first<kCorrelationSize>()));
    if (messageType == kResetAcknowledgementMessageType) {
        answer = parsed;
        return true;
    }

    std::size_t offset = kCorrelationSize;
    if (messageType == kQueryPerBubbleMessageType) {
        parsed.selector = std::to_integer<std::uint8_t>(payload[offset]);
        if (parsed.selector > kMaximumSelector) {
            return false;
        }
        parsed.hasSelector = true;
        offset += kSelectorSize;
    }

    if (!entity_slots::decode_entity_slots(payload.subspan(offset), parsed.mask)) {
        return false;
    }
    parsed.hasMask = true;
    answer = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::entity_authority
