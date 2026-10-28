#pragma once

/**
 * The virtual screen: every non-gameplay state (vr_state.h) shown on a flat screen fixed in the room,
 * in a dark room with the game frozen around it, and the controllers as a laser pointer on it.
 *
 * The precedent and its numbers are dishonored-vr's ScreenMode, which the user validated on a Quest 3
 * ("lo de tus menus esta perfecto"): 62 degrees wide at 2.4 m, placed in front of the head when it
 * opens (pitch kept within -30..+15 degrees), coming back in front by itself after the gaze has been
 * more than 40 degrees off it for 0.8 s (a 0.45 s glide) or at once on a stick click; a 0.22 s fade
 * in, the world it came from frozen and dimmed to 0.22 around it, and the world fading back in over
 * 0.28 s when play resumes. docs/PLAYABLE.md.
 *
 * Controls on the screen: point with either controller; trigger (or A) = click, hold = hold; B or the
 * menu button = Esc (back); right stick up/down = scroll; a stick click = the screen in front again.
 */

#include <cstdint>

namespace sunrise::client::hooks::vr::screen {

/** A pose in OpenXR's LOCAL space (metres; +X right, +Y up, -Z ahead). */
struct Pose final {
    float position[3]{};
    float orientation[4]{0, 0, 0, 1};
};

/** What this frame shows, for xr_runtime.cpp to draw and submit. */
struct Plan final {
    /** The screen this frame; false = the game, as before. */
    bool screen{};

    /** The panel: where it is, how big (m), the crop of the back buffer it shows. */
    Pose panel{};
    float width{};
    float height{};
    float alpha{};
    float crop[4]{};
    float corner{0.018F};
    std::uint32_t outWidth{};
    std::uint32_t outHeight{};
    bool pointer{};
    float pointerU{};
    float pointerV{};

    /** The pointer's beam: a thin quad from the controller to where it points. */
    bool beam{};
    Pose beamPose{};
    float beamLength{};
    float beamAlpha{};

    /** The room: freeze the newest picture of the game this frame (true once, out of play). */
    bool freeze{};
    float snapshot{};
    float desaturate{};
    float environment{1.0F};
    float fade{1.0F};
    float glowDirection[3]{0, 0, -1};
    float glow{};

    /** The game fading back in after a screen: 1 = as is, below 1 darkened toward black. */
    float worldFade{1.0F};
};

/**
 * Once per frame, on the present thread, from xr_runtime's end_frame.
 * @param head The head this frame (the eyes' midpoint, the cyclopean orientation).
 * @param aims The controllers' aim poses, left then right.
 * @param backBufferWidth,backBufferHeight The game's back buffer, whose UI band the panel shows.
 * @param snapshotAge Seconds since the newest picture of the game was taken (negative: none).
 * @param recentres The runtime's recentre count (the player held the Meta button): the screen follows.
 */
[[nodiscard]] Plan plan(const Pose& head, bool headValid, const Pose (&aims)[2], const bool (&aimValid)[2],
                        std::uint32_t backBufferWidth, std::uint32_t backBufferHeight, double snapshotAge,
                        std::uint32_t recentres) noexcept;

/** Brings the screen in front of the head (F7, a stick click). */
void request_recentre() noexcept;

/** The panel's last placement and pointer, for the log and the tests. */
void report_status() noexcept;

} // namespace sunrise::client::hooks::vr::screen
