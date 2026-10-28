#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "ghost_link_auth.h"
#include "sense_update.h"

// Type-65 Ghost-link Sense. The host publishes this root inside a msg-5 object state block; the
// client's Auth consumer reads it and only a fraction strictly above 1.0 finishes the scene.

namespace sunrise::middleware::bap::activity_message::ghost_link_sense {

namespace fields = auth_fields;

/** Sense identity and fixed body size: delta root bit, bool, real32, biased int32. */
inline constexpr std::uint32_t kSchema = ghost_link::kSenseSchema;
inline constexpr std::size_t kBitCount = 66;
inline constexpr std::size_t kByteCount = 9;
/** Root ordinals in wire order: active, elapsed over duration, accepted Auth generation. */
inline constexpr std::uint16_t kActiveOrdinal = 0;
inline constexpr std::uint16_t kFractionOrdinal = 1;
inline constexpr std::uint16_t kGenerationOrdinal = 2;
/** A fraction of exactly 1.0 finishes nothing, so a finish publishes one above it. */
inline constexpr float kFinishedFraction = 1.25F;

/**
 * Encodes one complete Sense body with its delta root bit.
 * @param active Scene active flag.
 * @param fraction Elapsed over duration, carried as raw IEEE bits.
 * @param generation Auth generation this state answers.
 * @param output At least kByteCount.
 * @param bytes Receives kByteCount, or zero on failure.
 * @param bits Receives kBitCount, or zero on failure.
 * @return True when the whole body fits.
 */
[[nodiscard]] inline bool encode(bool active,
                                 float fraction,
                                 std::int32_t generation,
                                 std::span<std::byte> output,
                                 std::size_t& bytes,
                                 std::size_t& bits) noexcept {
    bytes = 0;
    bits = 0;
    if (output.size() < kByteCount || !std::isfinite(fraction) || fraction < 0.0F) {
        return false;
    }
    encoding::bits::Writer writer(output.first(kByteCount));
    const bool encoded =
        writer.write(1, fields::kPresenceWidth)
        && writer.write(active ? 1U : 0U, fields::kBoolWidth)
        && writer.write(std::bit_cast<std::uint32_t>(fraction), 32)
        && writer.write(static_cast<std::uint32_t>(generation) + fields::kSigned32Bias, 32)
        && fields::finish_exact(writer, kBitCount, kByteCount, bytes);
    if (!encoded) {
        bytes = 0;
        return false;
    }
    bits = kBitCount;
    return true;
}

/** One client-reported Ghost-link level. */
struct Level final {
    std::int32_t generation{};
    float fraction{};
    bool active{};
};

/**
 * Reads one decoded Sense body. All three fields are mandatory, so a body missing one is refused.
 * @param values Decoded values owned by the type-65 object.
 * @param output Receives the reported level, unchanged on failure.
 * @return True when every field was present and in range.
 */
[[nodiscard]] inline bool read(std::span<const sense_update::DecodedValue> values,
                               Level& output) noexcept {
    // All three mandatory fields must be present for the body to read as complete.
    constexpr std::uint8_t seenAll = 0x07;
    Level level{};
    std::uint8_t seen = 0;
    for (const sense_update::DecodedValue& value : values) {
        if (value.schemaRow != kSchema || !value.present) {
            continue;
        }
        switch (value.fieldOrdinal) {
        case kActiveOrdinal:
            level.active = value.unsignedValue != 0;
            seen |= 0x01;
            break;
        case kFractionOrdinal:
            if (!std::isfinite(value.realValue) || value.realValue < 0.0F) {
                return false;
            }
            level.fraction = value.realValue;
            seen |= 0x02;
            break;
        case kGenerationOrdinal:
            level.generation = static_cast<std::int32_t>(value.signedValue);
            seen |= 0x04;
            break;
        default:
            break;
        }
    }
    if (seen != seenAll) {
        return false;
    }
    output = level;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::ghost_link_sense
