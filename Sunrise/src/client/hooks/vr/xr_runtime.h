#pragma once

#include <array>
#include <cstdint>

struct ID3D11Device;
struct IDXGISwapChain;

namespace sunrise::client::hooks::vr::xr {

/** Three lanes in the game's basis: X forward, Z up. */
using Vector = std::array<float, 3>;

/**
 * One head pose, already converted out of OpenXR's basis into the game's and ready to write.
 *
 * OpenXR is +Y up, -Z forward, +X right, in metres. The game is X forward, Z up, right handed,
 * which makes its +Y axis point LEFT. The conversion is therefore
 * `(gx, gy, gz) = (-xz, -xx, xy)`, not a sign flip.
 */
struct HeadPose final {
    /** Head forward, in the game's basis, with the body yaw already folded in. */
    Vector forward{};
    /** Head up, same basis. */
    Vector up{};
    /** Head translation away from the recentre origin, in game units. */
    Vector offset{};
    /** Symmetric horizontal FOV covering both eye frusta, in radians. */
    float horizontalFov{};
    float aspect{};
    /** Frame index the pose came from, so a consumer can tell a stale pose from a fresh one. */
    std::uint64_t frame{};
    bool valid{};
};

/** Button bits of InputState::buttons. */
constexpr std::uint32_t kButtonA = 1U << 0;
constexpr std::uint32_t kButtonB = 1U << 1;
constexpr std::uint32_t kButtonX = 1U << 2;
constexpr std::uint32_t kButtonY = 1U << 3;
constexpr std::uint32_t kButtonThumbL = 1U << 4;
constexpr std::uint32_t kButtonThumbR = 1U << 5;
constexpr std::uint32_t kButtonMenu = 1U << 6;

/** The controllers, read once per frame through the action set. Sticks are -1..1, +Y forward. */
struct InputState final {
    float moveX{};
    float moveY{};
    float turnX{};
    float turnY{};
    float triggerL{};
    float triggerR{};
    float gripL{};
    float gripR{};
    std::uint32_t buttons{};
    /** False until the action set has been attached and synced at least once. */
    bool valid{};
};

/** Why the runtime is not running, for the log and the UI. */
enum class Status : unsigned char {
    off,
    noRuntime,
    negotiateFailed,
    instanceFailed,
    systemFailed,
    sessionFailed,
    running,
};

/**
 * Brings up the OpenXR runtime, instance, system and session on the game's D3D11 device.
 * Safe to call every frame: it does the work once and reports the same answer after that.
 * @param device The device the game is already presenting with.
 * @return True once a session is running.
 */
[[nodiscard]] bool initialize(ID3D11Device* device) noexcept;

/** Tears the session and instance down and unloads the runtime. */
void shutdown() noexcept;

/** @return True while a session is running and poses are being produced. */
[[nodiscard]] bool active() noexcept;

/** @return Why the runtime is or is not running. */
[[nodiscard]] Status status() noexcept;

/**
 * Runs one OpenXR frame: drains events, waits, begins, syncs the actions, locates the views and
 * publishes the pose. Called from the present hook, which is the only site that owns the render
 * device.
 * @param bodyYaw Yaw of the game's own camera, in radians, folded into the published pose so
 *                mouse look still turns the body and the head adds on top of it.
 */
void begin_frame(float bodyYaw) noexcept;

/**
 * Ends the OpenXR frame opened by begin_frame.
 * With a swap chain, the back buffer just presented is copied into the runtime's swapchain and
 * submitted as one projection layer, the same image for both eyes, at the pose and FOV it was
 * rendered with. Without one, the frame ends with no layers.
 * @param swapChain The swap chain the game just presented, or null.
 */
void end_frame(IDXGISwapChain* swapChain) noexcept;

/** @return The most recently published pose. Copied under a lock; safe from any thread. */
[[nodiscard]] HeadPose head_pose() noexcept;

/** @return The controllers as of the last sync. Copied under a lock; safe from any thread. */
[[nodiscard]] InputState input_state() noexcept;

/**
 * Records the horizontal FOV the camera hook actually left in the engine this frame, read back
 * from the pose block, so the layer declares the frustum the image was really rendered with.
 * @param horizontalFov Radians.
 */
void note_rendered_fov(float horizontalFov) noexcept;

/** Takes the next head position as the origin, so the player can re-seat themselves. */
void recentre() noexcept;

/** @return A short, stable name for the runtime in use, for the log. */
[[nodiscard]] const char* runtime_name() noexcept;

} // namespace sunrise::client::hooks::vr::xr
