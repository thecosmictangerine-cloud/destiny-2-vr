#pragma once

#include "../hooks/teleport/runtime.h"

namespace sunrise::client::player::position {

/** The local player's world position, or nothing when they have not been seen. */
struct Snapshot {
    hooks::teleport::Vector position{};
    bool present{};
};

/** Publishes the position of the component the physics sync is running for. */
void observe(void* component) noexcept;

/** Refreshes the position for a player at rest. Call it per frame, on a game thread. */
void poll() noexcept;

/**
 * @return The player's physics component from the last sync or poll, or null. Any thread may read
 * it. It can be stale, so read through it only with faulting-safe reads.
 */
[[nodiscard]] void* component() noexcept;

/** Drops the published position. */
void reset() noexcept;

/** @return The last published position, which any thread may read. */
[[nodiscard]] Snapshot snapshot() noexcept;

} // namespace sunrise::client::player::position
