#pragma once

#include <cstddef>
#include <span>

#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../state/activity/mission/definition.h"

namespace sunrise::server::bap {
struct Session;
struct Scratch;
} // namespace sunrise::server::bap

namespace sunrise::server::bap::encrypted::push::activity {

/** Decodes only the Type 23 bodies included in a successfully encoded roster. */
[[nodiscard]] bool collect_roster_device_publications(
    const middleware::bap::activity_message::sensor_auth_update::Snapshot& snapshot,
    std::span<state::activity::mission::DevicePublication> output,
    std::size_t& count) noexcept;

/** Captures device proofs and input boundaries before the frame reaches the caller. */
[[nodiscard]] bool stage_roster_device_publications(
    Session& session,
    Scratch& scratch,
    const middleware::bap::activity_message::sensor_auth_update::Snapshot& snapshot) noexcept;

/** Renews device report ownership only after the roster frame reaches the caller. */
void commit_roster_device_publications(const Session& session) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
