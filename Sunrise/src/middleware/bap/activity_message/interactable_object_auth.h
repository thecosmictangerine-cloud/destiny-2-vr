#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "auth_fields.h"

// Type-4 object Auth that spawns authored entry zero and subscribes to accepted player use.
// The client answers the subscription on its Sense reply list with the interaction latch.

namespace sunrise::middleware::bap::activity_message::interactable_object {

namespace fields = auth_fields;

/** Object Auth schema; the SDK format table carries the same value as kObjectAuthSchema. */
inline constexpr std::uint32_t kSchema = 0x8080992FU;
/** Registered reply the client emits for the interaction subscription below. */
inline constexpr std::uint32_t kInteractionReply = 0x80804FB7U;
/** Subscription schema that makes the client emit kInteractionReply. */
inline constexpr std::uint32_t kInteractionSubscription = 0x80804FB8U;
/** Optional ownership subscription; the client answers it with the owner key reply. */
inline constexpr std::uint32_t kOwnershipSubscription = 0x80809ACDU;
inline constexpr std::uint32_t kOwnershipReply = 0x80809ACCU;
inline constexpr std::size_t kBits = 375;
inline constexpr std::size_t kBytes = 47;
/** The ownership subscription adds its presence bit and schema. */
inline constexpr std::size_t kOwnerBits = kBits + fields::kPresenceWidth + 32;
inline constexpr std::size_t kOwnerBytes = (kOwnerBits + 7) / 8;
/** Reply subscription count: one, or two with ownership. */
inline constexpr std::uint8_t kSubscriptionCountWidth = 2;
/** Use-visibility mode. Wire zero is native -1, which lets use through with no player filter. */
inline constexpr std::uint8_t kUseVisibilityWidth = 2;
inline constexpr std::uint32_t kUseVisibilityDefault = 0;

/**
 * Encodes the object body.
 * @param generation Positive object generation; a new one respawns the entry.
 * @param trackOwner Adds the ownership subscription.
 * @param active Field .2, whether the entry is active.
 * @param used Sends the row already latched at revision one, the state after a first use.
 * @param written Receives kBytes or kOwnerBytes.
 */
[[nodiscard]] inline bool encode(std::int32_t generation,
                                 std::span<std::byte> output,
                                 std::size_t& written,
                                 bool trackOwner = false,
                                 bool active = true,
                                 bool used = false) noexcept {
    written = 0;
    const std::size_t bytes = trackOwner ? kOwnerBytes : kBytes;
    const std::size_t bits = trackOwner ? kOwnerBits : kBits;
    if (generation <= 0 || output.size() < bytes) {
        return false;
    }
    encoding::bits::Writer writer(output.first(bytes));
    const std::array<fields::Field, 5> head{{
        {static_cast<std::uint32_t>(generation) + fields::kSigned32Bias, 32},
        {fields::kSigned32Bias, 32}, // signed zero
        {active ? 1U : 0U, fields::kBoolWidth},
        {0, fields::kBoolWidth},
        {fields::kSigned32Bias, 32}, // signed zero
    }};
    const std::array<fields::Field, 7> transform{{
        {0, 32},
        {0, 32},
        {0, 32},
        {0, fields::kBoolWidth},
        {trackOwner ? 2U : 1U, kSubscriptionCountWidth},
        {1, fields::kPresenceWidth},
        {kInteractionSubscription, 32},
    }};
    const std::array<fields::Field, 1> visibility{{{kUseVisibilityDefault, kUseVisibilityWidth}}};
    // The row's revision and latch; the client takes them only when the revision is newer.
    const std::array<fields::Field, 2> tail{{
        {fields::kSigned32Bias + (used ? 1U : 0U), 32},
        {used ? 1U : 0U, fields::kBoolWidth},
    }};
    return fields::write_fields(writer, head) && fields::write_absent_client_ref(writer)
           && fields::write_fields(writer, transform) && fields::write_fields(writer, visibility)
           && fields::write_absent_client_ref(writer) && fields::write_fields(writer, tail)
           && (!trackOwner
               || (writer.write(1, fields::kPresenceWidth)
                   && writer.write(kOwnershipSubscription, 32)))
           && fields::finish_exact(writer, bits, bytes, written);
}

} // namespace sunrise::middleware::bap::activity_message::interactable_object
