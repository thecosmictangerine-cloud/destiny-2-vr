#include <algorithm>
#include <array>
#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode702.h"

namespace sunrise::middleware::web_service::messages::opcode702 {
namespace {
using encoding::bits::Reader;

/** Primitive widths in the character writeback schema. */
constexpr std::uint8_t kPresenceBits = 1;
constexpr std::uint8_t kByteBits = 8;
constexpr std::uint8_t kShortBits = 16;
constexpr std::uint8_t kWordBits = 32;
constexpr std::uint8_t kLongBits = 64;
constexpr std::uint8_t kWorldBits = 5;
constexpr std::uint8_t kSelectorBits = 3;
/** The activity block's bytes ride at bias 128. */
constexpr int kByteBias = 128;
/** Fixed array lengths from the character writeback schema. */
constexpr std::size_t kHeaderFloats = 3;
constexpr std::size_t kSeenWords = 4;
constexpr std::size_t kRosterItems = 20;
constexpr std::size_t kRosterPlugs = 64;
constexpr std::size_t kOpaqueBytes = 128;
constexpr std::size_t kInventoryRows = 350;
constexpr std::size_t kUnlockFlags = 768;
constexpr std::size_t kTailBooleans = 3;

/** An absent field has no payload. */
template <typename Read> bool optional(Reader& reader, Read read) noexcept {
    std::uint64_t present = 0;
    return reader.read(kPresenceBits, present) && (present == 0 || read(reader));
}

bool skip_optional(Reader& reader, std::size_t width) noexcept {
    return optional(reader, [width](Reader& field) noexcept { return field.skip(width); });
}

/** Header floats carry their own presence bits; hash-array words do not. */
bool read_header(Reader& reader) noexcept {
    const auto floats = [](Reader& array) noexcept {
        for (std::size_t index = 0; index < kHeaderFloats; ++index) {
            if (!skip_optional(array, kWordBits)) {
                return false;
            }
        }
        return true;
    };
    return optional(reader, floats) && skip_optional(reader, kSeenWords * kWordBits)
           && skip_optional(reader, kWordBits);
}

/** The five-byte activity block carries the world-state field without an inner presence bit. */
bool read_activity(Reader& reader, Request& output) noexcept {
    const auto block = [&output](Reader& fields) noexcept {
        // Three bytes at bias 128 and one three-bit selector at bias 1 precede the world state.
        std::uint64_t value = 0;
        for (std::int8_t& byte : output.activityBytes) {
            if (!fields.read(kByteBits, value)) {
                return false;
            }
            byte = static_cast<std::int8_t>(static_cast<int>(value) - kByteBias);
        }
        if (!fields.read(kSelectorBits, value)) {
            return false;
        }
        output.activitySelector = static_cast<std::int8_t>(static_cast<int>(value) - 1);
        if (!fields.read(kWorldBits, value)) {
            return false;
        }
        output.worldState = static_cast<std::uint8_t>(value);
        output.hasWorldState = true;
        return true;
    };
    return optional(reader, block) && skip_optional(reader, kWordBits)
           && skip_optional(reader, kByteBits) && skip_optional(reader, kLongBits);
}

/** The roster mirror has twenty records and seven trailing optional scalars. */
bool read_roster(Reader& reader) noexcept {
    const auto items = [](Reader& array) noexcept {
        for (std::size_t index = 0; index < kRosterItems; ++index) {
            if (!skip_optional(array, kLongBits) || !skip_optional(array, kShortBits)
                || !skip_optional(array, kShortBits)
                || !skip_optional(array, kRosterPlugs * kShortBits) || !skip_optional(array, 4)) {
                return false;
            }
        }
        return true;
    };
    // The roster tail carries these seven optional wire fields in order.
    constexpr std::array<std::uint8_t, 7> kTailWidths{
        kLongBits, kLongBits, kWordBits, kWordBits, kByteBits, kByteBits, 4};
    if (!optional(reader, items)) {
        return false;
    }
    for (const auto width : kTailWidths) {
        if (!skip_optional(reader, width)) {
            return false;
        }
    }
    return true;
}

/** The leading byte counts the payload bytes, up to the buffer's capacity. */
bool read_opaque_state(Reader& reader) noexcept {
    const auto buffer = [](Reader& fields) noexcept {
        std::uint64_t count = 0;
        return fields.read(kByteBits, count) && count <= kOpaqueBytes
               && fields.skip(static_cast<std::size_t>(count) * kByteBits);
    };
    return optional(reader, buffer) && skip_optional(reader, kLongBits);
}

/** New-item bits and instance watermarks share a group but have independent presence bits. */
bool read_inventory(Reader& reader, Request& output) noexcept {
    const auto bitmap = [&output](Reader& array) noexcept {
        state::account::inventory::CharacterNewItems bits{};
        for (auto& word : bits) {
            std::uint64_t value = 0;
            if (!array.read(kWordBits, value)) {
                return false;
            }
            word = static_cast<std::uint32_t>(value);
        }
        output.newItems = bits;
        return true;
    };
    return optional(reader, bitmap) && skip_optional(reader, kInventoryRows * kWordBits);
}

/** Each boolean in the final array has its own presence bit. */
bool read_tail_booleans(Reader& reader) noexcept {
    for (std::size_t index = 0; index < kTailBooleans; ++index) {
        if (!skip_optional(reader, 1)) {
            return false;
        }
    }
    return true;
}

/** Child order follows the 5328-byte character bank. */
bool read_body(Reader& reader, Request& output) noexcept {
    // The next record contains six shorts, one word, and one long.
    constexpr std::size_t kFixedRecordBits = 6 * kShortBits + kWordBits + kLongBits;
    constexpr std::size_t kUnlockFlagBits = 3;
    return optional(reader,
                    [&output](Reader& group) noexcept { return read_activity(group, output); })
           && optional(reader, read_roster) && optional(reader, read_opaque_state)
           && skip_optional(reader, kWordBits)
           && optional(reader,
                       [&output](Reader& group) noexcept { return read_inventory(group, output); })
           && skip_optional(reader, kFixedRecordBits)
           && skip_optional(reader, kUnlockFlags * kUnlockFlagBits)
           && optional(reader, read_tail_booleans);
}

/** Unused bytes in the fixed-capacity request buffer must be zero. */
bool zero_padding(Reader& reader) noexcept {
    while (reader.remaining_bits() != 0) {
        std::uint64_t value = 0;
        const auto width =
            static_cast<std::uint8_t>((std::min)(reader.remaining_bits(), std::size_t{kLongBits}));
        if (!reader.read(width, value) || value != 0) {
            return false;
        }
    }
    return true;
}
} // namespace

/** Reads activity state and new-item flags without applying a partial writeback. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.empty()
        || message.payload.size() > kPayloadSize) {
        return false;
    }
    Reader reader(message.payload);
    Request candidate;
    if (!optional(reader, read_header)
        || !optional(reader,
                     [&candidate](Reader& group) noexcept { return read_body(group, candidate); })
        || !zero_padding(reader)) {
        return false;
    }
    request = candidate;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode702
