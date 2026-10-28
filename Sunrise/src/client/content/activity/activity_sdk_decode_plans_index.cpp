#include <array>

#include "activity_sdk_decode_plans_internal.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal {
namespace {

/** Candidate offsets of the block chain marker inside a reflection record. */
constexpr std::array<std::uint32_t, 4> kMarkerOffsets{0x58, 0x60, 0x68, 0x70};
/** Codec block: bound handle at +0x10, serialized size at +0x18, field rows from +0x28. */
constexpr std::uint64_t kCodecHandleAt = 0x10;
constexpr std::uint64_t kCodecSerializedAt = 0x18;
constexpr std::uint64_t kCodecFieldsAt = 0x28;
/** Bit offset of one field inside its 40-byte row. */
constexpr std::uint64_t kFieldBitAt = 8;

/**
 * Sizes one reflection block from its class and row count.
 * @param size Receives header plus rows.
 * @return False for an unknown block class.
 */
[[nodiscard]] bool
block_size(std::uint32_t kind, std::uint32_t count, std::uint64_t& size) noexcept {
    std::uint64_t header = 0;
    std::uint64_t stride = 0;
    switch (kind) {
    case kCodecBlock:
        header = 0x28;
        stride = 0x28;
        break;
    case kPropertyBlock:
        header = 0x20;
        stride = 0x0C;
        break;
    case kMethodBlock:
        header = 0x10;
        stride = 0x08;
        break;
    case kTailBlock:
        header = 0x18;
        break;
    default:
        return false;
    }
    size = header + stride * count;
    return true;
}

} // namespace

/**
 * Walks every bound record's block chain and rows its codec block.
 * The chain must close exactly and a codec block must name its own bound handle.
 */
bool build_codec_index(const Image& image,
                       std::vector<SchemaRow>& rows,
                       std::vector<std::uint32_t>& fieldValues,
                       const char*& error) noexcept {
    try {
        rows.clear();
        fieldValues.clear();
        for (const auto& [handle, address] : image.slots()) {
            std::uint32_t length = 0;
            if (!image.u32(address, length)) {
                error = "record_read";
                return false;
            }
            std::uint32_t marker = 0;
            for (const std::uint32_t candidate : kMarkerOffsets) {
                std::uint32_t value = 0;
                if (candidate + 8 <= length && image.u32(address + candidate, value)
                    && value == kReflectionMarker) {
                    marker = candidate;
                    break;
                }
            }
            if (marker == 0) {
                continue;
            }
            std::uint64_t cursor = marker;
            bool codec = false;
            std::uint64_t codecAt = 0;
            std::uint32_t codecCount = 0;
            while (cursor < length) {
                std::uint32_t kind = 0;
                std::uint32_t count = 0;
                std::uint64_t size = 0;
                if (!image.u32(address + cursor + 4, kind)
                    || !image.u32(address + cursor + 8, count)) {
                    error = "record_read";
                    return false;
                }
                if (!block_size(kind, count, size)) {
                    error = "reflection_block_unknown";
                    return false;
                }
                if (cursor + size > length) {
                    error = "reflection_block_extent";
                    return false;
                }
                if (kind == kCodecBlock) {
                    std::uint32_t bound = 0;
                    if (!image.u32(address + cursor + kCodecHandleAt, bound)) {
                        error = "record_read";
                        return false;
                    }
                    if (bound != handle) {
                        error = "codec_handle_mismatch";
                        return false;
                    }
                    if (codec) {
                        error = "codec_block_repeated";
                        return false;
                    }
                    codec = true;
                    codecAt = cursor;
                    codecCount = count;
                }
                cursor += size;
            }
            if (cursor != length) {
                error = "reflection_chain_open";
                return false;
            }
            if (!codec) {
                continue;
            }
            SchemaRow row{handle, 0, static_cast<std::uint32_t>(fieldValues.size()), codecCount};
            if (!image.u32(address + codecAt + kCodecSerializedAt, row.size)) {
                error = "record_read";
                return false;
            }
            for (std::uint32_t index = 0; index < codecCount; ++index) {
                std::uint32_t bit = 0;
                if (!image.u32(address + codecAt + kCodecFieldsAt + kFieldRowSize * index
                                   + kFieldBitAt,
                               bit)) {
                    error = "record_read";
                    return false;
                }
                fieldValues.push_back(bit);
            }
            rows.push_back(row);
        }
        return true;
    } catch (...) {
        error = "allocation";
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal
