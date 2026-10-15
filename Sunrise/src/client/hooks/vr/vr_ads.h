#pragma once

#include <cstdint>

#include "vr_math.h"

namespace sunrise::client::hooks::vr::ads {

/**
 * Aiming down the sights, the way a VR shooter does it (docs/ADS.md).
 *
 * Three jobs, one module, because they share one model of the weapon:
 *
 *   1. The ADS ANIMATION is cancelled. The game's ADS moves the gun to the flat camera; in VR the
 *      gun stays in the hand. The animation is learned per weapon as a rigid transform of the
 *      viewmodel's local pose (hip -> ADS), and the weapon module applies its inverse, scaled by the
 *      engine's own ADS progress, to the frame it hands the engine -- the same frame, no lag.
 *   2. The SIGHT is learned from the same two poses. In ADS the game puts the sight exactly on the
 *      flat camera's axis; carried back to the hip pose, that axis is the weapon's sight line in
 *      the hand. The shot leaves the muzzle zeroed onto it (`aim_ray`), so the round lands where
 *      the sight is, at any range.
 *   3. ADS is PHYSICAL. When the sight line passes through the eye the image is rendered from, with
 *      the gun ahead of it and roughly along the gaze, the game is put into ADS (right mouse) for
 *      its accuracy and handling; it leaves with hysteresis. No zoom ever reaches the image: the
 *      camera hook writes the FOV after the engine has zoomed.
 *
 * Frames: "local" is the viewmodel's own frame, the one the engine poses the arms and the gun in
 * (rotation = the basis the weapon module hands the engine, origin = the transform buffer's
 * offset). "gun" is that frame as the weapon module INTENDS it, before the ADS compensation: the
 * frame in which the hand sits at its hip pose. Game axes throughout: X forward, Y left, Z up.
 */

/**
 * The engine's horizontal FOV for this frame, as it produced it, before the camera hook writes
 * over it. Its narrowing IS the ADS progress the engine animates everything else with. Camera
 * thread, once a frame, before the weapon is placed.
 */
void note_engine_fov(float radians) noexcept;

/**
 * How far into ADS the engine is, 0 at the hip and 1 at full ADS, as far as it is known: exact for
 * a learned weapon, all-or-nothing on any zoom before that. Any thread.
 */
[[nodiscard]] float engine_fraction() noexcept;

/**
 * The ADS compensation for the frame being placed now: the rigid transform, in the local frame,
 * by which the game's ADS animation has moved the hand so far. The weapon module turns and shifts
 * the frame it hands the engine by its inverse. Camera thread.
 * @return False when there is nothing to compensate (no learned weapon, compensation off, hip).
 */
[[nodiscard]] bool compensation(math::Affine& delta) noexcept;

/**
 * The drawn right hand as the GPU received it this frame (camera-relative, game axes) and the
 * weapon palette's root bone when one was seen near it, and the right controller's grip frame (same
 * space) -- the hand relative to it is what the player SEES slip. The learning half: hip and ADS
 * poses, weapon changes. Render thread, once per arm palette; cheap.
 */
void observe(const math::Affine& drawnHand, const math::Affine* gunRoot, std::uint32_t gunBones,
             const math::Affine& controller) noexcept;

/**
 * True while the game's animation holds the drawn hand still in the viewmodel's local frame -- no
 * recoil, reload, swap, inspect or walk bob playing -- and has for a little while. The local pose is
 * pure animation (the controller and every correction live in the frame, not in the pose), so this is
 * the clean signal for "the weapon is at rest in the hand": the only time the arm module's seats may
 * learn the weapon's hold. Chasing an animation instead turns the gun in the hand after every shot.
 * Render thread, after `observe`.
 */
[[nodiscard]] bool animation_at_rest() noexcept;

/**
 * The learned sight line of the weapon in hand, in the GUN frame: a point on it and its unit
 * direction. Any thread.
 * @return False until the weapon in hand has been seen in ADS.
 */
[[nodiscard]] bool sight(math::V3& origin, math::V3& direction) noexcept;

/** What is known about the sight of the weapon in hand, for the arm module's rotational seat. */
enum class SightKnown : int {
    /** No learned sight: a weapon never held in ADS. The seat falls back to the canonical hand. */
    unknown = 0,
    /** The weapon in hand is not identified yet, but a learned profile has its palette: hold still. */
    pending = 1,
    /** The weapon in hand is identified and its sight learned. */
    known = 2,
};
/**
 * The learned sight DIRECTION of the weapon in hand, in the gun frame, as far as it is known. The arm
 * module turns the gun so this runs along the controller's aim -- every weapon points where the
 * controller points, which a canonical HAND grip does not give (the auto rifle's hold put it 13 deg
 * off). Render thread.
 */
[[nodiscard]] SightKnown sight_direction(math::V3& direction) noexcept;

/** Distance along the sight line at which the shot is zeroed, metres. */
[[nodiscard]] float zero_distance() noexcept;

/** How ADS is entered. */
enum class Mode : int {
    /** No ADS at all; the left trigger is the grenade. */
    off = 0,
    /** Physical: the sight brought to the eye. The left trigger is the grenade. The default. */
    automatic = 1,
    /** The left trigger is ADS, as before this module; the left grip stays contextual. */
    button = 2,
};
[[nodiscard]] Mode mode() noexcept;

/** @return True while the game should be held in ADS (right mouse). Present thread. */
[[nodiscard]] bool want_ads() noexcept;

/**
 * Once per present: commands, the physical detector, the haptic tick, the periodic report.
 * Runs after the OpenXR frame is located and before the controllers become keys.
 */
void present() noexcept;

} // namespace sunrise::client::hooks::vr::ads
