#pragma once

#include <array>

#include "runtime.h"

namespace sunrise::client::hooks::vr::weapon {

/**
 * Detours the two engine paths that hand the camera pose to its consumers and lets one chosen
 * consumer receive a different pose. This is the F3 probe: the weapon viewmodel is drawn from the
 * camera pose, so whichever path feeds it can be made to hand out the body's pose (or, later, a
 * controller's) while the world view keeps the head's.
 *
 * Runs once per camera frame. Installs the detours on the first call, after verifying the code
 * bytes at the pinned addresses. Driven by `SVR_Weapon.txt` beside the executable, one rule per
 * line, consumed on read:
 *
 *   install                                         attach both detours (nothing is hooked until then)
 *   getter all|none|<rva>[,<rva>...] body|head|hand  which getter callers get which pose
 *   ptr    all|none|<rva>[,<rva>...] body|head|hand  same for the pose-pointer accessor
 *   block  on|off                                    both of the two below at once
 *   block  pos|orient on|off                         write the head's position, or orientation,
 *                                                    into the camera block, or leave the engine's
 *   hand_offset <fwd> <right> <up>                   calibration offset in the hand's own basis,
 *                                                    game units, so one constant cancels the
 *                                                    engine's viewmodel offset at any orientation
 *   report                                           dump caller counters now
 *
 * Caller counters (`ev=vr.weapon callers`) go to the log every few seconds while installed.
 */
void tick() noexcept;

/** @return False while the pose written to the camera block should keep the engine's orientation. */
[[nodiscard]] bool block_orientation_enabled() noexcept;

/**
 * Takes the controller's current pose as the weapon's rest reference again, so the gun's resting
 * place follows the player re-seating themselves. Called from the recentre hotkey.
 */
void retake_reference() noexcept;

/**
 * Split from the orientation because it is the decisive lever for the weapon's position: if the
 * engine draws the weapon at the block's position rather than at the pose it is handed, the head's
 * translation has to stay out of the block and reach the render view through a getter rule instead.
 * @return False while the pose written to the camera block should keep the engine's position.
 */
[[nodiscard]] bool block_position_enabled() noexcept;

/**
 * Feeds back where the right hand really is on the controller, so the weapon seats itself.
 *
 * The arm module reads the drawn right palm out of the GPU palette every frame and reports how
 * far it sits from the controller's grip pose, in the controller's basis (forward, right, up,
 * metres). A slow integrator moves the weapon by the opposite amount. The `pivot` constant was
 * measured by hand for one weapon; this closes the same loop for any weapon, and for any slow
 * animation (sprint) that carries the gun away from the hand -- fast ones, like recoil, still show.
 */
void seat_error(float forward, float right, float up) noexcept;

/** @return The seat correction currently applied, controller basis (forward, right, up). */
[[nodiscard]] std::array<float, 3> seat_correction() noexcept;

/**
 * The rotational half of self-seating: a rotation in the controller's own proper basis
 * (forward, LEFT, up), as a unit quaternion (x, y, z, w), applied to the orientation handed to the
 * weapon before anything else. The arm module drives it so the right hand holds every weapon the
 * way it holds the hand cannon at rest -- which points every weapon where the controller points.
 */
void set_seat_rotation(const std::array<float, 4>& quaternion) noexcept;

/** Drops both seat corrections, positional and rotational, back to none. */
void seat_reset() noexcept;

} // namespace sunrise::client::hooks::vr::weapon
