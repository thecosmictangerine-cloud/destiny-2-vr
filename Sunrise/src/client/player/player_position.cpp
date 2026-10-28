/**
 * The local player's published world position.
 * The game threads write it and the interface reads it, so a seqlock guards the vector.
 */

#include "player_position.h"

#include <atomic>
#include <cstdint>

namespace sunrise::client::player::position {
namespace {

namespace teleport = hooks::teleport;

/** Odd while a write is in progress, so a reader that sees one retries. */
std::atomic_uint32_t g_sequence{0};
/** Written between two sequence bumps, and read between two equal even reads. */
teleport::Vector g_position{};
std::atomic_bool g_present{false};
/**
 * The player's physics component, found on the sync tick.
 * It is kept here rather than taken from the teleport hook, because that hook only caches one
 * while the teleport feature is switched on.
 */
std::atomic<void*> g_component{nullptr};

/**
 * Reads one component's body position and publishes it.
 * @param component Component already proved to be the player's.
 * @return True when the body was read. A failed read leaves the last position published.
 */
[[nodiscard]] bool publish_from(void* component) noexcept {
    teleport::Vector position{};
    // The body is gone at rest and during a load, so the last position stands until a new one
    // reads back. A player who cannot be read has not moved.
    if (!teleport::read_position(component, position)) {
        return false;
    }
    g_sequence.fetch_add(1, std::memory_order_acq_rel);
    g_position = position;
    g_sequence.fetch_add(1, std::memory_order_release);
    g_present.store(true, std::memory_order_release);
    return true;
}

} // namespace

/** Publishes the position of the component the physics sync is running for. */
void observe(void* component) noexcept {
    if (component == nullptr) {
        return;
    }
    void* const known = g_component.load(std::memory_order_relaxed);
    if (known == component) {
        (void)publish_from(component);
        return;
    }
    // The ownership test is paid only until the player's component is known. The frame poll drops
    // a stale one, which is what lets a new destination's component be found.
    if (known != nullptr || !teleport::owns_local_player(component)) {
        return;
    }
    g_component.store(component, std::memory_order_relaxed);
    (void)publish_from(component);
}

/** Refreshes the position for a player at rest, and drops a component that is no longer theirs. */
void poll() noexcept {
    void* component = g_component.load(std::memory_order_relaxed);
    if (component == nullptr) {
        // The teleport hook keeps one too whenever its own feature is on.
        component = teleport::local_player_component();
    }
    if (component == nullptr) {
        return;
    }
    if (!teleport::owns_local_player(component)) {
        g_component.store(nullptr, std::memory_order_relaxed);
        g_present.store(false, std::memory_order_release);
        return;
    }
    g_component.store(component, std::memory_order_relaxed);
    (void)publish_from(component);
}

/** Reports the player's physics component. */
void* component() noexcept {
    return g_component.load(std::memory_order_relaxed);
}

/** Drops the published position. */
void reset() noexcept {
    g_component.store(nullptr, std::memory_order_relaxed);
    g_present.store(false, std::memory_order_release);
}

/** @return The last published position. */
Snapshot snapshot() noexcept {
    Snapshot value{};
    if (!g_present.load(std::memory_order_acquire)) {
        return value;
    }
    for (;;) {
        const std::uint32_t before = g_sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        value.position = g_position;
        if (g_sequence.load(std::memory_order_acquire) == before) {
            break;
        }
    }
    value.present = true;
    return value;
}

} // namespace sunrise::client::player::position
