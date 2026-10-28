#pragma once

/**
 * What kind of play the world is in, for VR, and where the eye goes in each. docs/SWORD-SPARROW.md.
 *
 * Destiny draws a sword, the sparrow, the Tower's social space, emotes and most supers with a third-
 * person chase camera. VR keeps the player in first person through all of them: the eye goes to the
 * Guardian's own head, the body is not drawn (arms, the sword and the sparrow are), and on a vehicle the
 * room rides the vehicle's heading -- a cockpit -- while the head looks freely.
 *
 *  - first:   the engine builds the first-person viewmodel (the weapon's transform builder runs). The
 *             engine's camera is the eye, as it always was.
 *  - onFoot:  no viewmodel, the player's own physics body present (a sword, the Tower, an emote). The
 *             eye is the body's feet (its transform, +416) plus the eye height learned in first person.
 *  - vehicle: the player's physics body gone (mounting takes it away) and a vehicle found: the physics
 *             component nearest to where the player last stood. The eye is the vehicle's seat.
 *  - passive: neither (a death camera, a cut scene): the engine's camera, untouched as before.
 *
 * The physics body updates once per rendered frame and its transform IS what the renderer draws: the
 * root bone of the third-person palettes equals it to the millimetre (2026-10-18 captures).
 *
 * Research half: `SVR_Mode.txt` -- `trace <frames>` writes one CSV line per camera frame
 * (`SVR_ModeTrace.csv`), `dump comp|body|vcomp|vbody <bytes>` that object's memory once.
 */

#include <cstdint>

#include "vr_math.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::mode {

enum class Mode : std::uint8_t {
    first,
    onFoot,
    vehicle,
    passive,
};

/** What the camera hook needs from this module, once per camera frame. */
struct Eye final {
    Mode mode{Mode::first};
    /** onFoot / vehicle: the eye's base in the world (the head's travel is added by the caller). */
    bool replace{};
    math::V3 base{};
    /** vehicle: the room anchor this frame (the vehicle's heading), and the turn applied to the sample. */
    bool cockpit{};
    float anchor{};
};

/**
 * Camera thread, once per frame, before the camera is written: decides the mode and the eye. In a
 * vehicle the head and hands sampled this frame are turned to the vehicle's current heading (they were
 * folded with the anchor of the present that sampled them), so the eye and the hands ride it exactly.
 */
[[nodiscard]] Eye camera_tick(const float* enginePosition, const float* engineForward, xr::HeadPose& head,
                              xr::HandPose& left, xr::HandPose& right) noexcept;

/**
 * The render view's eye in third person. The engine stops drawing the Guardian when the camera comes
 * near them (measured: not one draw of their armour with the camera block at the head), so in third
 * person the block keeps the chase camera's position -- the logic camera, which the engine culls with
 * -- and only the render view (the camera getter's caller B363FA, vr_weapon.cpp) is handed the eye.
 * Camera thread sets; any thread reads. False outside third person.
 */
void set_render_eye(bool valid, const float* position) noexcept;
[[nodiscard]] bool render_eye(float* position) noexcept;

/** Camera thread, after the write: the camera position the engine renders this frame with. */
void commit(const float* cameraPosition, const xr::HeadPose& head, const xr::HandPose& left,
            const xr::HandPose& right) noexcept;

/** Present thread, once per present. */
void present_tick() noexcept;

/** @return The current mode. Any thread. */
[[nodiscard]] Mode current() noexcept;

/** @return The mode's name, for logs. */
[[nodiscard]] const char* name(Mode mode) noexcept;

/**
 * One camera frame, as recorded at the camera hook: what a world-space palette drawn later must be
 * posed against. Found again by its anchor -- a palette's root is the body (or the vehicle) of its own
 * frame, so the record whose anchor matches it is that frame's, whichever order the GPU sees things in.
 */
struct Record final {
    std::uint64_t serial{};
    Mode mode{Mode::first};
    /** onFoot: the feet (the body's transform). vehicle: where the rider's root is expected. */
    math::V3 rider{};
    /** vehicle: its transform. */
    math::Affine vehicle{};
    float vehicleYaw{};
    /** The camera position written for the frame, and the sample it was composed from. */
    math::V3 camera{};
    xr::HeadPose head{};
    xr::HandPose left{};
    xr::HandPose right{};
};

/** The newest record. Any thread. */
[[nodiscard]] bool latest_record(Record& out) noexcept;

/** The record of serial `serial`, while the ring still holds it. Any thread. */
[[nodiscard]] bool record_by_serial(std::uint64_t serial, Record& out) noexcept;

/** The record whose rider point is nearest `root`, within `tolerance` metres. Any thread. */
[[nodiscard]] bool record_by_rider(const math::V3& root, float tolerance, Record& out) noexcept;

/** The record whose vehicle origin is nearest `origin`, within `tolerance` metres. Any thread. */
[[nodiscard]] bool record_by_vehicle(const math::V3& origin, float tolerance, Record& out) noexcept;

/** The rider's root in the vehicle's frame, as the renderer showed it (vr_body.cpp refines it). */
void note_rider_offset(const math::V3& inVehicle) noexcept;

/** The handlebar grips in the vehicle's frame, from the rider's own hands (vr_body.cpp). */
void note_handlebars(const math::V3& leftInVehicle, const math::V3& rightInVehicle) noexcept;
[[nodiscard]] bool handlebars(math::V3& leftInVehicle, math::V3& rightInVehicle) noexcept;

/** The steering input of this frame, -1 (left) .. 1 (right), from the controllers (vr_gamepad.cpp). */
void set_steering(float steer) noexcept;

} // namespace sunrise::client::hooks::vr::mode
