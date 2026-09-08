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
 *   getter all|none|<rva>[,<rva>...] body|head     which getter callers get which pose
 *   ptr    all|none|<rva>[,<rva>...] body|head     same for the pose-pointer accessor
 *   block  on|off                                   write head orientation into the block or not
 *   report                                          dump caller counters now
 *
 * Caller counters (`ev=vr.weapon callers`) go to the log every few seconds while installed.
 */
void tick() noexcept;

/** @return False while the pose written to the camera block should keep the engine's orientation. */
[[nodiscard]] bool block_orientation_enabled() noexcept;

} // namespace sunrise::client::hooks::vr::weapon
