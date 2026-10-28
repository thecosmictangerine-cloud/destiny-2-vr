#pragma once

#include "vr_math.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace sunrise::client::hooks::vr::aim {

/**
 * The gun's aim line: where a shot from the barrel goes.
 *
 * Drawn into the frame on request (`SVR_Aim.txt`: `ray on|off`), which makes it both the
 * verification instrument -- an impact either sits on the drawn line or it does not, at any depth,
 * because the line is projected by the very camera the world was rendered with -- and the optional
 * laser for the player. `dump` logs the line in normalised device coordinates for the harness.
 *
 * Once per present, on the present thread.
 */
void present(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) noexcept;

/**
 * The shot of the gun in the right hand as it would leave now, in the right controller's own frame
 * (columns forward, LEFT, up; origin at the grip pose): `t` the muzzle, `m` the fire basis (column 0
 * the direction, zeroed onto the learned sight when there is one). Rigid with the controller while
 * the gun sits still in the hand -- which is what lets the dual-wield module carry it, mirrored, to
 * the left hand with the weapon it was measured on. Any thread.
 * @return False when the gun has not been placed yet.
 */
[[nodiscard]] bool right_shot_in_controller(math::Affine& shot, bool* sighted) noexcept;

} // namespace sunrise::client::hooks::vr::aim
