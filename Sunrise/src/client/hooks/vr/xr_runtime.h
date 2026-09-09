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
    /**
     * Head yaw relative to the direction the player was facing at the last recentre, in radians,
     * positive turning left (the game's +Y).
     *
     * Published separately from the folded vectors because the body servo needs the head's own
     * rotation on its own: the target the character's facing is driven towards is the room anchor
     * plus this, and folding it into the vectors would make it unrecoverable.
     */
    float roomYaw{};
    /** Symmetric horizontal FOV covering both eye frusta, in radians. */
    float horizontalFov{};
    float aspect{};
    /** Frame index the pose came from, so a consumer can tell a stale pose from a fresh one. */
    std::uint64_t frame{};
    bool valid{};
};

/**
 * One controller's aim pose, converted out of OpenXR's basis into the game's exactly as HeadPose
 * is, and measured from the same recentre origin.
 *
 * Sharing the origin is the whole point: the head and the hands then land in one world anchor, so
 * the geometry between them -- lean towards the gun and it grows, step back and it recedes -- is
 * preserved by the conversion instead of having to be reconstructed later.
 */
struct HandPose final {
    /** Where the controller points, in the game's basis, with the body yaw folded in. */
    Vector forward{};
    /** Controller up, same basis. */
    Vector up{};
    /** Controller right, same basis. Lets a calibration offset be expressed in the hand's basis,
     *  which is what makes one constant cancel the engine's viewmodel offset at any orientation. */
    Vector right{};
    /** Controller AIM translation away from the recentre origin, in game units. */
    Vector offset{};
    /**
     * The GRIP (palm) translation away from the same origin, in game units.
     *
     * OpenXR defines two poses per controller and they are not interchangeable. `aim` is the
     * origin of a pointing ray, and on Touch hardware it sits several centimetres in front of the
     * hand, in mid air. `grip` is the palm centroid -- the point a wrist actually rotates about
     * and the point a held object's grip has to coincide with. Anything placing a weapon uses
     * this one; anything pointing uses `forward`.
     *
     * Falls back to `offset` when the runtime refuses a grip pose, so a consumer never has to
     * check.
     */
    Vector palm{};
    /** Frame index the pose came from, so a consumer can tell a stale pose from a fresh one. */
    std::uint64_t frame{};
    /**
     * True when tracking was lost this frame and the pose is the last good one, re-folded by the
     * current room anchor.
     *
     * Held rather than dropped on purpose: dropping it makes a weapon placed by this pose snap
     * back to wherever the engine would have drawn it and then snap out again, which is what a
     * Quest controller resting against the body or leaving the cameras' view produces several
     * times a minute. The re-folding matters as much as the holding -- a snapshot kept in the
     * folded frame goes stale the moment the player turns, which is a bug this module has
     * already paid for once.
     */
    bool stale{};
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
 * @param roomAnchor World yaw, in radians, that the direction the player faced at the last
 *                   recentre maps to. The published pose is that anchor plus the head's own
 *                   rotation, which is what anchors the horizon to the room instead of to the
 *                   character: nothing but an explicit turn can move it, so the character may
 *                   then be driven to follow the head without the world sliding underneath.
 */
void begin_frame(float roomAnchor) noexcept;

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
 * @param rightHand True for the right controller, false for the left.
 * @return That controller's most recently located aim pose. Copied under a lock.
 */
[[nodiscard]] HandPose hand_pose(bool rightHand) noexcept;

/**
 * Copies the head and both controllers out under ONE lock acquisition.
 *
 * Not a convenience. Anything that computes the geometry BETWEEN the head and a hand -- which is
 * the whole basis of the weapon's placement -- must difference two poses from the same frame. Two
 * separate accessor calls can straddle a publish and difference two different instants, and the
 * error that produces is a jitter of exactly the size of one frame's movement.
 * @param head Receives the head pose.
 * @param left Receives the left controller.
 * @param right Receives the right controller.
 */
void frame_sample(HeadPose& head, HandPose& left, HandPose& right) noexcept;

/**
 * Records the horizontal FOV the camera hook actually left in the engine this frame, read back
 * from the pose block, so the layer declares the frustum the image was really rendered with.
 * @param horizontalFov Radians.
 */
void note_rendered_fov(float horizontalFov) noexcept;

/**
 * Records the aspect the camera hook actually left in the engine this frame, so the layer can
 * declare the vertical extent the image was really rendered with rather than the back buffer's
 * pixel ratio.
 * @param aspect tan(halfHorizontalFov) / tan(halfVerticalFov), as the engine stores it.
 */
void note_rendered_aspect(float aspect) noexcept;

/** Takes the next head position as the origin, so the player can re-seat themselves. */
void recentre() noexcept;

/** @return A short, stable name for the runtime in use, for the log. */
[[nodiscard]] const char* runtime_name() noexcept;

} // namespace sunrise::client::hooks::vr::xr
