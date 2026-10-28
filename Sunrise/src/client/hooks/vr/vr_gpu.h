#pragma once

#include <array>
#include <cstdint>

#include "vr_math.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;

namespace sunrise::client::hooks::vr::gpu {

/**
 * Once per present, on the present thread. Installs the D3D11 detours the first time, polls the
 * command file, and closes a frame capture that has run its course.
 *
 * Runs whether or not VR is enabled: the vertex shaders have to be seen as they are CREATED, which
 * is long before anyone presses F9, or their reflection is lost for the session.
 */
void present(ID3D11Device* device, IDXGISwapChain* swapChain) noexcept;

/** The camera the viewmodel was last drawn with, after the FOV fix: camera-relative, game basis. */
struct ViewCamera final {
    std::array<float, 3> forward{};
    std::array<float, 3> up{};
    float horizontalFov{};
    /** Projection y scale over x scale, i.e. width / height of the frustum. */
    float aspect{};
};

/** @return False until a viewmodel view has been seen. */
[[nodiscard]] bool viewmodel_camera(ViewCamera& out) noexcept;

/**
 * The projection scales the world was really rendered with (1/tan of the half angles), as the GPU
 * received them -- the ground truth for what the image contains, whatever was written upstream.
 * @return False until the world camera has been seen, or when it was last seen over 0.5 s ago.
 */
[[nodiscard]] bool world_projection(float& p00, float& p11) noexcept;

/**
 * The back buffer's render target view, cached against the texture it was made for, so the debug
 * overlays can draw into the frame the desktop and the headset both receive. Present thread only.
 * @return The view, owned here, or null.
 */
[[nodiscard]] ID3D11RenderTargetView* back_buffer_view(ID3D11Device* device, IDXGISwapChain* swapChain) noexcept;

/**
 * Drops the cached view, and with it the reference it holds on the back buffer. Called right before
 * ResizeBuffers, which DXGI refuses while any reference to the buffers is outstanding.
 */
void release_back_buffer_view() noexcept;

/**
 * The whole frame as the UI composited it this frame: the input of the UI's scene composite, before
 * the UI crops it to its canvas (never narrower than 16:10). An eye-shaped render is complete in it,
 * while the back buffer holds it cropped to a band with the HUD on top (docs/DISPLAY.md).
 * @return The texture with a reference the caller releases, or null when the composite did not run
 *         since the last present (menus, loads) or has not been seen at all.
 */
[[nodiscard]] ID3D11Texture2D* scene_texture() noexcept;

/**
 * Whether `scale` is the vertex dequantisation scale (palette header row 4) of one of the arm meshes the
 * first-person viewmodel draws: the gloves' and gauntlets' own. In third person the same meshes are
 * skinned by world palettes carrying the same header, so this names an arm piece whatever buffer the
 * engine's pool put it in this frame (vr_body.cpp). Any thread.
 */
[[nodiscard]] bool is_arm_scale(float scale) noexcept;

/** @return How many times the UI's scene composite (the world drawn into the canvas) has run. */
[[nodiscard]] std::uint64_t scene_composites() noexcept;

/** @return World and viewmodel view constants examined so far (only while the rewrite runs: immersive). */
[[nodiscard]] std::uint64_t world_views() noexcept;
[[nodiscard]] std::uint64_t viewmodel_views() noexcept;

/** @return The immediate context the game draws on, without a reference, or null. */
[[nodiscard]] ID3D11DeviceContext* immediate_context() noexcept;

/**
 * The root bone of the small skinned palette (the weapon's: a handful of bones, camera-relative)
 * seen most recently nearest to `around`, within half a metre and the last 100 ms.
 * @return False when none qualifies.
 */
[[nodiscard]] bool weapon_root(const math::V3& around, math::Affine& out, std::uint32_t* bones = nullptr) noexcept;

} // namespace sunrise::client::hooks::vr::gpu
