#pragma once

#include <array>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

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

/** @return The immediate context the game draws on, without a reference, or null. */
[[nodiscard]] ID3D11DeviceContext* immediate_context() noexcept;

} // namespace sunrise::client::hooks::vr::gpu
