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
    /**
     * Each eye's own translation from the recentre origin, left at index 0, folded like `offset`.
     * `offset` is one of these (or their midpoint) -- whichever eye the image is rendered from.
     */
    std::array<Vector, 2> eyeOffsets{};
    /** Which position `offset` holds: 0 the midpoint, 1 the left eye, 2 the right. */
    int eye{};
    /** The yaw fold (room anchor minus the recentre facing) the vectors were turned by, radians. */
    float fold{};
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
    /**
     * The GRIP pose's own orientation (its -Z forward, +Y up), same basis. OpenXR defines its -Z as
     * the axis of the tube a closed hand makes: where a sword's hilt lies in the fist (vr_body.cpp).
     * Never the weapon's: that is `forward`, the pointing ray. Falls back to the aim's axes.
     */
    Vector gripForward{};
    Vector gripUp{};
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
 * Brings up the OpenXR runtime, instance and system on a thread of its own, then the session on the
 * game's D3D11 device on THIS (the present) thread, the only one on which the runtime may touch the
 * device safely; retries a failure (3 s without a headset or runtime, 10 s otherwise).
 * Call every present while VR is on: it never waits for the init thread; the session's creation is
 * a hitch of that one call's length.
 * @param device The device the game is already presenting with.
 * @return True once a session exists (it runs when the runtime says READY; see active()).
 */
[[nodiscard]] bool initialize(ID3D11Device* device) noexcept;

/**
 * Tears the session and instance down (VR switched off). Never blocks: while the init thread or the
 * frame thread is inside a runtime call it does nothing, so call it every present while off.
 * @return True on the call that took something down.
 */
bool shutdown() noexcept;

/**
 * Logs the modules the loader filter has seen since the last call (`ev=vr.xr dll`): which were the
 * runtime's and which were handed on to the game's own notification callback. Present thread.
 */
void drain_loader_log() noexcept;

/** @return True while the runtime gives the session input focus (the headset is on, no system menu). */
[[nodiscard]] bool session_focused() noexcept;

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

/**
 * @return The horizontal FOV (radians) last read back from the pose block, or 0 before the camera
 *         hook has run. The world is rendered with it while the module is on (the aspect is not).
 */
[[nodiscard]] float rendered_fov() noexcept;

/**
 * @return The room facing taken at the last recentre (radians, game yaw). A sample's `fold` is the
 *         room anchor it was folded with minus this.
 */
[[nodiscard]] float origin_yaw() noexcept;

/** Takes the next head position as the origin, so the player can re-seat themselves. */
void recentre() noexcept;

/**
 * Asks for one vibration pulse on a controller, played at the next frame on the present thread.
 * Safe from any thread; a request not yet played is replaced, not queued. Does nothing on a
 * runtime without haptics.
 * @param rightHand True for the right controller, false for the left.
 * @param amplitude 0..1.
 * @param seconds Duration.
 */
void haptic(bool rightHand, float amplitude, float seconds) noexcept;

/**
 * The eye the single (mono) image is rendered from: 0 the midpoint of the eyes, 1 the left eye,
 * 2 the right. One pose for both views either way -- the image is still declared identically to
 * both eyes, so this cannot reintroduce the double vision per-eye poses caused. What it changes is
 * where a sight lines up: in front of the nose, or in front of the shooting eye as with a real gun.
 * The orientation stays the average of both eyes'. Any thread; takes effect next frame.
 */
void set_render_eye(int eye) noexcept;

/** @return The eye the image is rendered from (see set_render_eye). */
[[nodiscard]] int render_eye() noexcept;

/**
 * The position of the eye the player AIMS with, from a head pose: the set_render_eye() eye (the
 * midpoint for 0). In mono it is `offset` itself; in alternating stereo `offset` flips between the
 * eyes every frame, and a sight must still line up with the one eye the player shoots with.
 */
[[nodiscard]] Vector aiming_eye_offset(const HeadPose& head) noexcept;

/** @return A short, stable name for the runtime in use, for the log. */
[[nodiscard]] const char* runtime_name() noexcept;

/**
 * What one mono image has to cover to fill both eyes, and at what density.
 *
 * The engine renders one symmetric frustum, so a mono image must hold the union of both eyes'
 * fields: `tanHalfWidth` and `tanHalfHeight` are the largest tangents of any eye edge. The density
 * is the runtime's recommended eye resolution over that eye's tangent extent -- pixels per tangent
 * unit at the centre of the panel, which is what "as sharp as the headset can show" means.
 */
struct EyeGeometry final {
    float tanHalfWidth{};
    float tanHalfHeight{};
    float pixelsPerTanX{};
    float pixelsPerTanY{};
    std::uint32_t recommendedWidth{};
    std::uint32_t recommendedHeight{};
    bool valid{};
};

/** @return The geometry of the current session's eyes; invalid before views have been located. */
[[nodiscard]] EyeGeometry eye_geometry() noexcept;

/**
 * Alternating-eye stereo (docs/DISPLAY.md): each frame is rendered from one eye, the other eye's
 * last image is kept, and each image is declared at the pose it was drawn from. False = mono.
 */
void set_stereo(bool alternate) noexcept;
[[nodiscard]] bool stereo() noexcept;

/**
 * Each projection view declares its own eye's share of the image (true, the default) or the whole
 * image with the symmetric FOV it was rendered with (false: the submission before 2026-10-16, which a
 * runtime that ignores declared FOVs shows doubled). `crop on|off` in SVR_Display.txt.
 */
void set_per_eye_crop(bool perEye) noexcept;
[[nodiscard]] bool per_eye_crop() noexcept;

/**
 * A deliberate error in the DECLARATION only, never the image: the declared pose turned by `yawDegrees`
 * about up, every declared FOV tangent scaled by `fovScale`. A runtime that honours declarations moves
 * or rescales the picture by exactly that; one that ignores them shows no change -- the experiment
 * that tells which kind a runtime is, read off its mirror. `probe <deg> <scale>`; `probe 0 1` is off.
 */
void set_declaration_probe(float yawDegrees, float fovScale) noexcept;

/**
 * How far ahead of the frame's predicted display time the head and hands are located: measured by
 * default (negative), so each image is drawn for the instant it is really shown; or fixed, in
 * milliseconds (0 = the frame's own prediction, the behaviour before 2026-10-16). `lead auto|off|<ms>`.
 */
void set_lead(float milliseconds) noexcept;
/** @return The lead in use, in milliseconds. */
[[nodiscard]] float lead_ms() noexcept;

/*
 * The frame ledger. Every consumer that turns the head pose into pixels takes its sample from one
 * record, so the camera, the gun, the arms and the layer agree about every frame -- a requirement
 * the moment consecutive frames are drawn from different eyes, where a gun placed against the other
 * eye's camera is off by the whole eye distance.
 */

/**
 * Camera thread, at the camera write: takes the latest pose and both hands, picks the eye this
 * frame is drawn from (the next one in alternating stereo, the configured one in mono -- `offset`
 * is set to it) and latches the result as this frame's sample.
 * @return False while no pose has been published.
 */
[[nodiscard]] bool camera_frame(HeadPose& head, HandPose& left, HandPose& right) noexcept;

/** Camera thread, after the block is written: what was written, for the GPU to recognise. */
void commit_camera(const Vector& position, const Vector& forward) noexcept;

/**
 * The sample the camera hook latched for the frame being built. For consumers later in the same
 * camera update (the weapon). Falls back to the latest pose before the first latch.
 * @return False when it fell back.
 */
bool camera_sample(HeadPose& head, HandPose& left, HandPose& right) noexcept;

/**
 * Render thread: the world's camera as the GPU received it (view constants row 7 and -row 6).
 * Names the record the frame being drawn was built from.
 */
void note_world_camera(const float* position, const float* forward) noexcept;

/**
 * The sample of the frame the GPU is drawing now, by the camera its world view carried. For
 * consumers on the render thread (the arms' rewrite). Falls back to the latest pose.
 * @return False when it fell back.
 */
bool drawn_sample(HeadPose& head, HandPose& left, HandPose& right) noexcept;

/** @return The ledger sequence of the frame the camera hook is building (0 before the first). */
[[nodiscard]] std::uint64_t camera_sequence() noexcept;

/** @return The ledger sequence of the frame the GPU is drawing (0 before the first match). */
[[nodiscard]] std::uint64_t drawn_sequence() noexcept;

} // namespace sunrise::client::hooks::vr::xr
