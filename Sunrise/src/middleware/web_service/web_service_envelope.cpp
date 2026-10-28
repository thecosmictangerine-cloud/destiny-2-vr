#include "web_service_envelope.h"

#include <algorithm>

#include "../encoding/byte_order.h"
#include "status_fields.h"

namespace sunrise::middleware::web_service {

/** Parses the fixed big-endian Web Service header without interpreting its payload. */
bool parse_request(std::span<const std::byte> input, Message& message) noexcept {
    message = {};
    if (input.size() < kEnvelopeHeaderSize) {
        return false;
    }
    message.opcode = encoding::read_u16_be(input.first<encoding::kU16Size>());
    message.transactionId =
        encoding::read_u32_be(input.subspan<encoding::kU16Size, encoding::kU32Size>());
    message.payload = input.subspan(kEnvelopeHeaderSize);
    return true;
}

/** Encodes the echoed header, the chosen status layout, and two absent trailer flags. */
bool encode_response(const Message& request,
                     ResponseShape shape,
                     const StatusResponse& status,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kEnvelopeHeaderSize) {
        return false;
    }
    encoding::write_u16_be(output.first<encoding::kU16Size>(), request.opcode);
    encoding::write_u32_be(output.subspan<encoding::kU16Size, encoding::kU32Size>(),
                           request.transactionId);

    encoding::bits::Writer writer(output.subspan(kEnvelopeHeaderSize));
    bool encoded = sunrise::middleware::web_service::status::write_fields(writer, shape, status);
    encoded = encoded && writer.write(0, kAbsentTrailerWidth);

    std::size_t payloadSize = 0;
    if (!encoded || !writer.finish(payloadSize)) {
        return false;
    }
    written = kEnvelopeHeaderSize + payloadSize;
    return true;
}

/** Writes the echoed header and hands back the writer for the payload behind it. */
encoding::bits::Writer begin_response(const Message& request,
                                      std::span<std::byte> staging) noexcept {
    encoding::write_u16_be(staging.first<encoding::kU16Size>(), request.opcode);
    encoding::write_u32_be(staging.subspan<encoding::kU16Size, encoding::kU32Size>(),
                           request.transactionId);
    return encoding::bits::Writer(staging.subspan(kEnvelopeHeaderSize));
}

/** Appends the absent trailer, then copies the staged response out in one step. */
bool finish_response(encoding::bits::Writer& writer,
                     bool encoded,
                     std::span<const std::byte> staging,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    std::size_t payloadSize = 0;
    if (!encoded || !writer.write(0U, kAbsentTrailerWidth) || !writer.finish(payloadSize)) {
        return false;
    }
    const std::size_t responseSize = kEnvelopeHeaderSize + payloadSize;
    if (responseSize > output.size()) {
        return false;
    }
    std::copy_n(staging.begin(), responseSize, output.begin());
    written = responseSize;
    return true;
}

/** Appends the first blob and the absent second one, then copies the staged response out. */
bool finish_response_with_blob(encoding::bits::Writer& writer,
                               bool encoded,
                               std::span<const std::byte> blob,
                               std::span<const std::byte> staging,
                               std::span<std::byte> output,
                               std::size_t& written) noexcept {
    written = 0;
    if (!encoded || blob.size() > kTrailerBlobCapacity || !writer.write(1U, kTrailerPresenceWidth)
        || !writer.write(static_cast<std::uint32_t>(blob.size()), kTrailerLengthWidth)) {
        return false;
    }
    for (const std::byte value : blob) {
        if (!writer.write(std::to_integer<std::uint32_t>(value), 8)) {
            return false;
        }
    }
    std::size_t payloadSize = 0;
    if (!writer.write(0U, kTrailerPresenceWidth) || !writer.finish(payloadSize)) {
        return false;
    }
    const std::size_t responseSize = kEnvelopeHeaderSize + payloadSize;
    if (responseSize > output.size()) {
        return false;
    }
    if (staging.data() != output.data()) {
        std::copy_n(staging.begin(), responseSize, output.begin());
    }
    written = responseSize;
    return true;
}

} // namespace sunrise::middleware::web_service
