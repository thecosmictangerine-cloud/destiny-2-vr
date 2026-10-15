#pragma once

#include <array>
#include <cstdint>

#include "runtime.h"
#include "vr_math.h"
#include "xr_runtime.h"

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

/**
 * The head and the FINAL weapon orientation of the last frame the weapon was handed one: the
 * controller after the rotational seat and the two-handed swing, i.e. exactly what the engine drew
 * the gun with. Any thread.
 * @return False until the weapon has been driven at least once.
 */
[[nodiscard]] bool weapon_sample(xr::HeadPose& head, xr::HandPose& hand) noexcept;

/** @return The seat correction currently applied, controller basis (forward, right, up). */
[[nodiscard]] std::array<float, 3> seat_correction() noexcept;

/**
 * The gun frame of the last frame the weapon was placed: the frame in which the drawn hand sits at
 * its HIP pose, i.e. where the weapon module means the gun to be, before the ADS compensation turns
 * what it hands the engine. Rotation: columns (forward, LEFT, up), the final weapon basis. Origin:
 * relative to the character's eye (the engine's camera position, before the head offset), so
 * `engine eye + t` is the world point and `t - head.offset` the camera-relative one. Any thread.
 * @return False until the weapon has been driven at least once.
 */
[[nodiscard]] bool gun_frame(math::Affine& frame, xr::HeadPose& head, xr::HandPose* hand = nullptr) noexcept;

/** The viewmodel frame the engine was really handed, camera-relative, as the transform detour left it. */
struct DrawnFrame final {
    /** Rotation: the basis handed to the weapon's getter. Origin: the transform buffer's offset lanes. */
    math::Affine local{};
    /** The engine's own offset lanes before this module added to them. */
    std::array<float, 3> lanesIn{};
    /** Counts transform detours; equal serials mean the same frame. */
    std::uint64_t serial{};
    bool valid{};
};

/** @return The last frame the engine placed the viewmodel with. Any thread. */
[[nodiscard]] DrawnFrame drawn_frame() noexcept;

/**
 * The rotational half of self-seating: a rotation in the controller's own proper basis
 * (forward, LEFT, up), as a unit quaternion (x, y, z, w), applied to the orientation handed to the
 * weapon before anything else. The arm module drives it so the right hand holds every weapon the
 * way it holds the hand cannon at rest -- which points every weapon where the controller points.
 */
void set_seat_rotation(const std::array<float, 4>& quaternion) noexcept;

/** Drops both seat corrections, positional and rotational, back to none. */
void seat_reset() noexcept;

/** The left hand's hold on the weapon, as the arm module decided it this frame. */
struct TwoHandGrip final {
    /** 0 one-handed, 1 fully held; in between while taking hold or letting go. */
    float weight{};
    /** True when the hold steers the barrel (a foregrip); false for a support hand under a pistol. */
    bool aim{};
    /** The foregrip in the right controller's raw (forward, LEFT, up) basis, from its palm, metres. */
    std::array<float, 3> foregrip{};
    /** The handguard, metres along the controller's forward from the foregrip. */
    float slideMin{};
    float slideMax{};
};

/**
 * Two-handed aim. While the hold steers the barrel, the orientation handed to the weapon is turned
 * about the right palm until the foregrip line runs through the left palm (vr_two_hand.h), blended
 * in by `weight`. Roll stays the right wrist's. Published by the arm module once a frame; a grip
 * that stops being published lets go by itself after a quarter of a second.
 */
void set_two_hand(const TwoHandGrip& grip) noexcept;

} // namespace sunrise::client::hooks::vr::weapon
