#pragma once

#include <algorithm>
#include <span>

#include "../../middleware/bap/activity_message/ghost_link_auth.h"
#include "host_runtime_counter_auth.h"

// Type-65 Ghost-link scan model. The host owns the Auth `.0` generation for each slot: it must
// increase on every change, including an arm bit of false, or the client applies nothing.

namespace sunrise::server::activity::host::ghost_link {

namespace wire = middleware::bap::activity_message::ghost_link;

/**
 * Re-encodes one requested body under the next generation for its exact slot.
 * @param instance Live activity holding the transported estate.
 * @param request Requested body, whose arm bit is kept and whose generation is replaced.
 * @param guard Committed guard for this slot.
 * @param pending Receives the encoded body, its width and the chosen generation.
 * @param written Receives the encoded byte count.
 * @return False when the request is not an exact type-65 body or the counter is exhausted.
 */
[[nodiscard]] inline bool encode(const detail::Instance& instance,
                                 const detail::ScriptableRequest& request,
                                 const detail::ScriptableGuard& guard,
                                 PendingScriptableOverride& pending,
                                 std::size_t& written) noexcept {
    namespace fields = middleware::bap::activity_message::auth_fields;
    std::int32_t requested = 0;
    bool enabled = false;
    std::uint32_t transported = 0;
    if (request.kind != ScriptableOverrideKind::ghostLink
        || !counter_auth::compatible(request.kind, request.target)
        || request.authBitCount != wire::kBitCount || request.authByteCount != wire::kByteCount
        || !wire::decode(
            std::span(request.authBody).first(request.authByteCount), requested, enabled)
        || !counter_auth::previous_revision(instance, request.target, 0, transported)) {
        return false;
    }
    const std::uint32_t last = (std::max)(transported, guard.ghostLink.generation);
    if (last >= fields::kMaximumCounter) {
        return false;
    }
    const std::uint32_t next = last + 1;
    if (!wire::encode(static_cast<std::int32_t>(next), enabled, pending.body, written)) {
        return false;
    }
    pending.bitCount = static_cast<std::uint16_t>(wire::kBitCount);
    pending.generation = next;
    return true;
}

/** A newly transported generation re-arms the scan, so its finished bar starts over. */
inline void advance(detail::ScriptableGuard& guard,
                    const PendingScriptableOverride& pending) noexcept {
    std::int32_t generation = 0;
    bool enabled = false;
    if (pending.byteCount != wire::kByteCount
        || !wire::decode(std::span(pending.body).first(pending.byteCount), generation, enabled)) {
        return;
    }
    guard.ghostLink.generation = static_cast<std::uint32_t>(generation);
    guard.ghostLink.armed = enabled;
    guard.ghostLink.finished = false;
    guard.ghostLink.active = false;
    guard.ghostLink.fraction = 0.0F;
}

} // namespace sunrise::server::activity::host::ghost_link
