#pragma once

#include <cstdint>

#include "vr_math.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace sunrise::client::hooks::vr::dual {

/**
 * Dual wielding: a weapon in each hand, any pair from the loadout (docs/DUAL-WIELD.md).
 *
 * Once a camera frame, on the camera thread: commands from `SVR_Dual.txt`, the engine hooks the first
 * time VR is up, the weapons' recordings and calibration, and what the left hand shows.
 */
void tick() noexcept;

/** @return The engine object of the weapon in the right hand (the game's active weapon), or 0. */
[[nodiscard]] std::uintptr_t active_weapon() noexcept;

/**
 * Which hand a launch of `weapon` belongs to: 0 the right (the game's own shot), 1 the left. Game
 * thread, from inside the launcher, once per launch (a twin's are told apart by the order the
 * triggers fired them, so this consumes).
 */
[[nodiscard]] int launch_hand(std::uintptr_t weapon) noexcept;

/**
 * The left gun's shot for the frame being built, in the room (relative to the recentre origin, game
 * axes, the frame the controllers are in): `t` the muzzle, `m` the fire basis. The right gun's shot,
 * measured on the same weapon while it was in the right hand, mirrored onto the left controller.
 * Any thread.
 * @return False when the left hand holds nothing.
 */
[[nodiscard]] bool left_shot(math::Affine& shot, bool* sighted) noexcept;

/** @return True while the left hand holds a weapon. Any thread. */
[[nodiscard]] bool dual() noexcept;

/** The left hand takes a weapon (the selected slot's), or puts it away. */
void set_dual(bool on) noexcept;

/** Which loadout slot the left hand holds: 0 kinetic, 1 energy, 2 power. */
void set_left_slot(int slot) noexcept;

/** The next slot round, with a tick in the left hand. */
void cycle_left() noexcept;

[[nodiscard]] int left_slot() noexcept;

/** The reload button, in dual: the left weapon reloads too when it is not full. */
void request_reload() noexcept;

/**
 * A weapon slot the module wants the right hand to switch to (calibration shows a weapon there to
 * record it), or -1. The gamepad taps the slot's key. Consumes.
 */
[[nodiscard]] int take_slot_request() noexcept;

/**
 * The gamepad just tapped the key of loadout slot `slot` (0-2): the weapon that comes up next is that
 * slot's, which is what anchors the loadout's order (the HUD reads it rotated, in-hand weapon first).
 */
void note_slot_key(int slot) noexcept;

/**
 * The weapon in the left hand and its motion this frame -- recoil kick and reload tilt -- as a rigid
 * transform in the left controller's frame (forward, LEFT, up; origin at the grip). vr_arms.cpp
 * moves the left hand with it. Any thread.
 * @return False when the left hand holds nothing drawable.
 */
[[nodiscard]] bool left_hold(std::uintptr_t& key, math::Affine& motion) noexcept;

/** Which grips this frame are a hand drawing from or putting into a holster, not their usual action. */
struct HolsterUse final {
    bool left{};
    bool right{};
};

/**
 * The holsters, once per present from the gamepad pump, before the grips become keys: kinetic at the
 * right hip, energy at the left hip, power over the shoulder. Either hand squeezes its grip there to
 * draw that weapon (the right hand's is the game's own swap; the left hand's is dual wield), or, the
 * left hand holding it already, to put it away.
 */
[[nodiscard]] HolsterUse holster_pump() noexcept;

/** Once per present: the magazines drawn on the guns while dual wielding. Present thread. */
void present(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) noexcept;

/** @return Rounds in the left weapon's magazine (-1 when none), and whether it is reloading. */
[[nodiscard]] int left_magazine() noexcept;
[[nodiscard]] bool left_reloading() noexcept;

} // namespace sunrise::client::hooks::vr::dual
