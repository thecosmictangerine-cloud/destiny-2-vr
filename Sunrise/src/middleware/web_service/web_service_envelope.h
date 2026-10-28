#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../encoding/bit_writer.h"

namespace sunrise::middleware::web_service {

/** The 6-byte Web Service header holds a big-endian opcode and transaction id. */
inline constexpr std::size_t kEnvelopeHeaderSize = sizeof(std::uint16_t) + sizeof(std::uint32_t);
/** Every response ends with two optional byte blobs, each behind one presence bit. */
inline constexpr std::uint8_t kAbsentTrailerWidth = 2;
inline constexpr std::uint8_t kTrailerPresenceWidth = 1;
/** A present blob carries a 16-bit byte length, then its bytes, not byte aligned. */
inline constexpr std::uint8_t kTrailerLengthWidth = 16;
inline constexpr std::size_t kTrailerBlobCapacity = 0xFFFF;
/**
 * Status value of a reply that publishes no Family-4 revision.
 * The Client's version wait skips its object-store compare on this value and completes at once.
 * Logical INT32_MIN, the descriptor's own zero, is read as a revision instead.
 */
inline constexpr std::int32_t kNoFamily4Publication = -1;
/**
 * Status code of a refused operation.
 * The descriptor biases logical zero to the wire success the Client expects, so any other logical
 * value refuses. The five bits hold no error taxonomy, so one code covers every reason.
 */
inline constexpr std::int32_t kRefusedStatusCode = 1;

/** Parsed request header and borrowed bit-packed payload. */
struct Message {
    std::uint16_t opcode{};
    std::uint32_t transactionId{};
    std::span<const std::byte> payload{};
};

/** Reusable response payload layouts proven for stateless Web Service opcodes. */
enum class ResponseShape : std::uint8_t {
    statusOnly,
    statusPair,
    statusPairWithBool,
    // Header echo with an empty payload, for opcodes with no contract of their own.
    // Keeps the correlated task from hanging. The client decoder may under-run.
    generic,
};

/** Logical status values encoded with the protocol descriptor biases. */
struct StatusResponse {
    std::int32_t code{};
    /** Descriptor zero. A reply feeding the Family-4 wait must set a revision or the constant. */
    std::int32_t value{(std::numeric_limits<std::int32_t>::min)()};
    bool trailingBool{};
};

/**
 * Parses the byte-aligned Web Service header and keeps the rest as payload.
 * @param input Whole decrypted svc-10 body.
 * @param message Receives the opcode, transaction id, and borrowed payload.
 * @return True when the 6-byte header is present.
 */
[[nodiscard]] bool parse_request(std::span<const std::byte> input, Message& message) noexcept;

/**
 * Encodes one supported bit-packed response body with cleared optional trailers.
 * @param request Parsed request whose opcode and transaction id are echoed.
 * @param shape Descriptor layout to encode.
 * @param status Logical status values before descriptor biases.
 * @param output Caller-owned svc-11 body storage.
 * @param written Receives encoded response body bytes.
 * @return True when the output buffer holds the whole response.
 */
[[nodiscard]] bool encode_response(const Message& request,
                                   ResponseShape shape,
                                   const StatusResponse& status,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

/**
 * Writes the echoed header at the front of staging storage.
 * @param request Parsed request whose opcode and transaction id are echoed.
 * @param staging Response storage sized for the largest body this opcode can produce.
 * @return Bit writer positioned on the payload that follows the header.
 */
[[nodiscard]] encoding::bits::Writer begin_response(const Message& request,
                                                    std::span<std::byte> staging) noexcept;

/**
 * Closes the payload with the absent trailer fields and publishes the staged response.
 * Nothing reaches the output unless the whole response fits it.
 * @param writer Payload writer returned by begin_response.
 * @param encoded False when an earlier payload field did not fit.
 * @param staging Storage begin_response wrote the header into.
 * @param output Caller-owned svc-11 body storage.
 * @param written Receives encoded response body bytes, or zero when the response is refused.
 * @return True when the payload closed and the whole response fit the output.
 */
[[nodiscard]] bool finish_response(encoding::bits::Writer& writer,
                                   bool encoded,
                                   std::span<const std::byte> staging,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

/**
 * Closes the payload with the first blob present and the second absent, then publishes it.
 * @param writer Payload writer returned by begin_response.
 * @param encoded False when an earlier payload field did not fit.
 * @param blob Bytes of the first trailer blob, at most kTrailerBlobCapacity.
 * @param staging Storage begin_response wrote the header into.
 * @param output Caller-owned svc-11 body storage.
 * @param written Receives encoded response body bytes, or zero when the response is refused.
 * @return True when the payload closed and the whole response fit the output.
 */
[[nodiscard]] bool finish_response_with_blob(encoding::bits::Writer& writer,
                                             bool encoded,
                                             std::span<const std::byte> blob,
                                             std::span<const std::byte> staging,
                                             std::span<std::byte> output,
                                             std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service
