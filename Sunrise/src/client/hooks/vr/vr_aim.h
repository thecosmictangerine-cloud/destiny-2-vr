#pragma once

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

} // namespace sunrise::client::hooks::vr::aim
