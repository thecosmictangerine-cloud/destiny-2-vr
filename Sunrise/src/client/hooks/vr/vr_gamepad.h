#pragma once

#include <cstdint>

#include "runtime.h"

namespace sunrise::client::hooks::vr::gamepad {

/**
 * Injects a relative horizontal mouse movement, for the body servo.
 *
 * Goes through the same gate as every other injection -- nothing is sent unless the game has focus
 * and the mod's own interface is closed -- so the servo cannot type into the overlay or steer a
 * window the player has alt-tabbed away from.
 * @param counts Mouse counts; positive turns the character the way a rightward mouse move does.
 * @return True when the counts were actually sent.
 */
bool turn_body(int counts) noexcept;

/** Counters for the log. */
struct Stats final {
    /** Pumps that injected at least one key transition or mouse event. */
    std::uint64_t injected{};
    /** Keys currently held down by the injector. */
    std::uint32_t heldKeys{};
    /** Mouse counts injected so far, for calibrating the look rate. */
    std::int64_t mouseCountsX{};
    std::int64_t mouseCountsY{};
};

/**
 * Turns the controllers into the keyboard and mouse the game already listens to.
 *
 * The game reads pads through HID, not XInput (its image holds no XInput symbol at all), so
 * standing in for a pad would need a driver. It does read injected keyboard scancodes and mouse
 * motion, which is how the test harness drives it, so the controllers are mapped onto those:
 * the left stick onto WASD, the right stick onto mouse motion, triggers onto the mouse buttons,
 * and the buttons onto the default PC bindings.
 *
 * Runs once per present. Injects only while the game window has the focus and the mod's own
 * interface is closed; loses all held keys the moment either stops being true.
 * @param enabled False releases everything and injects nothing.
 */
void pump(bool enabled) noexcept;

/** Releases every key and button the injector holds. */
void release_all() noexcept;

/** @return Counters since start. */
[[nodiscard]] Stats stats() noexcept;

} // namespace sunrise::client::hooks::vr::gamepad
