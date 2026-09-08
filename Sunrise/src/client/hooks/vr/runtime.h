#pragma once

#include <array>
#include <cstdint>

namespace sunrise::client::hooks::vr {

/** Three lanes of a camera vector in the game's own basis: X forward, Z up. */
using Vector = std::array<float, 3>;

/** One camera pose, laid out as the game stores it. */
struct Pose final {
    Vector position{};
    Vector forward{};
    Vector up{};
    /** Horizontal field of view, in radians, as the game stores it. */
    float horizontalFov{};
    float aspect{};
};

/**
 * Deltas the probe lays on top of the pose the engine computed for the frame.
 * Kept as offsets, not absolutes, so the probe never has to know where the player is.
 */
struct Offsets final {
    float yawDegrees{};
    float height{};
    float fovDegrees{};
};

/** @return True while the probe is overwriting the camera pose. */
[[nodiscard]] bool enabled() noexcept;

/** @return The deltas currently applied. */
[[nodiscard]] Offsets offsets() noexcept;

/** @return The last pose read before the probe wrote over it. */
[[nodiscard]] Pose last_read() noexcept;

/** @return The last pose read back from game memory after the probe wrote it. */
[[nodiscard]] Pose last_written() noexcept;

/** @return This frame's pose as the engine produced it, before anything of ours (the body). */
[[nodiscard]] Pose engine_pose() noexcept;

/** @return This frame's pose with the head and the trims laid over it, whether or not it was stored. */
[[nodiscard]] Pose head_pose() noexcept;

/**
 * Reads the probe's keys once a frame and updates the switch and the deltas.
 * Runs on the camera thread, beside the other frame polls.
 */
void poll_keys() noexcept;

/**
 * Lays the deltas over the camera pose of one player.
 *
 * Runs inside the camera-frame hook after the engine has computed the pose, which is the only
 * site that reaches the pose block. A write here is what the rest of the frame reads.
 * @param playerIndex Player whose camera block to write.
 */
void apply_camera(std::uint32_t playerIndex) noexcept;

/** Clears the switch and every delta. The probe stops writing. */
void reset() noexcept;

/**
 * Drives the OpenXR frame from the present hook, which is the only site that owns the render
 * device. Brings the runtime up on the first call and does nothing once it has failed.
 * @param device The device the game is presenting with.
 * @param swapChain The swap chain the game just presented; its back buffer goes to the headset.
 */
void present_frame(void* device, void* swapChain) noexcept;

/** @return True while the head pose, rather than the manual deltas, is driving the camera. */
[[nodiscard]] bool head_tracking() noexcept;

} // namespace sunrise::client::hooks::vr
