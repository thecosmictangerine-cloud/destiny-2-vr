#pragma once

#include "../../queuez/subscription.h"
#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode206 {

/** Web Service opcode for one queuez family subscription. */
inline constexpr std::uint16_t kOpcode = 206;

/**
 * Parses the bit-packed family type and root SOID.
 * Both fields are taken as sent. Only their width is required.
 * @param message Parsed Web Service envelope.
 * @param subscription Receives the logical family selector.
 * @return True when both whole fields are present.
 */
[[nodiscard]] bool parse_request(const Message& message,
                                 queuez::Subscription& subscription) noexcept;

/**
 * Encodes the success status with the family's first snapshot as the reply's first blob.
 * The client decodes that blob with its svc-123 codec and creates the family from it.
 * @param message Parsed Web Service envelope.
 * @param snapshot Complete svc-123 update-notification body for the subscribed family.
 * @param output Caller-owned svc-11 body storage, also used as staging.
 * @param written Receives encoded response body bytes.
 * @return True when the whole response fit the output.
 */
[[nodiscard]] bool encode_response(const Message& message,
                                   std::span<const std::byte> snapshot,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode206
