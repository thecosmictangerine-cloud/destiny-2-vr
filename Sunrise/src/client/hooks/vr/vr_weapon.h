#pragma once

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

} // namespace sunrise::client::hooks::vr::weapon
