#pragma once

struct ID3D11Device;
struct IDXGISwapChain;

namespace sunrise::client::hooks::vr::display {

/**
 * The shape and size the engine renders at, made the headset's (docs/DISPLAY.md).
 *
 * A mono image has to cover the union of both eyes' fields -- on a Quest 3 about 108 x 110
 * degrees, an aspect of 0.96 -- and the engine was rendering the desktop window's 16:9, which left
 * a third of each eye black: the floating screen of the first sessions. Two things stood in the way,
 * and this module removes both while VR is on, and puts them back when it is off:
 *
 *  - the render size follows the swap chain, which follows the window. `ResizeBuffers` is
 *    intercepted for the game's swap chain and handed the eye's shape at the headset's density,
 *    whatever the window on the desktop is;
 *  - the engine's aspect-ratio setting does not use the back buffer's ratio: it SNAPS it to a
 *    table of monitor ratios (+0x3C41C0), the narrowest 16:10, for the projection. While the render
 *    is eye-shaped the snapped values are replaced by the true one, at their source;
 *  - the UI composites the finished world into its canvas, which is never narrower than 16:10, so
 *    the back buffer holds the eye-shaped world cropped to a 16:10 band. The presentation fills the
 *    rest of the eye from the composite's own input, which holds the whole frame (`canvas_band`,
 *    gpu::scene_texture, xr_runtime.cpp).
 *
 * `SVR_Display.txt`: `shape eye|desktop`, `scale <f>` (density relative to the headset's
 * recommended size), `size <w> <h>` / `size auto`, `fill on|off`, `stereo on|off`, `info`.
 */
void present(ID3D11Device* device, IDXGISwapChain* swapChain) noexcept;

/** @return True while the engine is rendering at the eye's shape. */
[[nodiscard]] bool eye_shaped() noexcept;

/**
 * The rows of a `width` x `height` back buffer the UI's canvas covers: all of them for 16:10 or
 * wider, a centred 16:10 band for anything taller.
 * @return False when the canvas covers the whole buffer (nothing to fill), or filling is off.
 */
[[nodiscard]] bool canvas_band(unsigned width, unsigned height, unsigned& top, unsigned& bottom) noexcept;

/**
 * The rows of a `width` x `height` back buffer the UI's canvas covers -- every menu is laid out in
 * them -- whatever `fill` says: all of them for 16:10 or wider, a centred 16:10 band for taller.
 */
void ui_band(unsigned width, unsigned height, unsigned& top, unsigned& bottom) noexcept;

/** @return The game's window (an HWND), or null before the first present. */
[[nodiscard]] void* window() noexcept;

} // namespace sunrise::client::hooks::vr::display
