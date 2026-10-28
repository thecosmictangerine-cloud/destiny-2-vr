#pragma once

/**
 * What the game is doing, as far as VR cares: a screen in the room or the world around the player.
 *
 * Every non-gameplay state -- title, character select, orbit and the Director, the travel and the
 * load, a menu opened in the world, Sunrise's own interface -- is shown on a virtual screen fixed in
 * the room (vr_screen.cpp); gameplay in the world is immersive, in first person and third person
 * alike (agreed 2026-10-17). The precedent is dishonored-vr's ScreenMode, validated on a Quest 3.
 * docs/PLAYABLE.md.
 */

#include <cstdint>

namespace sunrise::client::hooks::vr::state {

enum class View : unsigned char {
    screen,
    immersive,
};

/** Once per present, on the present thread: reads the signals and decides, with hysteresis. */
void tick() noexcept;

/** @return What this present shows. */
[[nodiscard]] View view() noexcept;

/** @return Why, in a word, for the log. */
[[nodiscard]] const char* reason() noexcept;

/** @return The game's boot-flow step (38 = in the world), or -1 when the camera tick is not running. */
[[nodiscard]] std::int32_t step() noexcept;

/**
 * The controllers just sent the key that opens a menu: the screen comes up this frame instead of
 * when the menu's own signal arrives (in dishonored-vr that was 0.36 s of a giant menu across the
 * eyes). If no menu follows within 0.7 s, back to the world.
 */
void expect_menu() noexcept;

/** The FOV the engine renders with while the screen is up (the camera hook, nothing of ours written). */
void note_engine_fov(float horizontalFov) noexcept;
[[nodiscard]] float engine_fov() noexcept;

/** @return Presents since the view last changed (0 on the frame it changed). */
[[nodiscard]] std::uint64_t frames_in_view() noexcept;

} // namespace sunrise::client::hooks::vr::state
