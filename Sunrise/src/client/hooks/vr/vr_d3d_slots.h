#pragma once

#include <cstddef>

namespace sunrise::client::hooks::vr::d3d {

/** Vtable indices of the D3D11 methods the GPU probe detours. See vr_d3d_slots.cpp. */
struct Slots final {
    std::size_t createBuffer{};
    std::size_t createInputLayout{};
    std::size_t createVertexShader{};
    std::size_t createPixelShader{};
    std::size_t createComputeShader{};
    std::size_t createDeferredContext{};

    std::size_t vsSetConstantBuffers{};
    std::size_t vsSetShader{};
    std::size_t vsSetShaderResources{};
    std::size_t psSetShader{};
    std::size_t drawIndexed{};
    std::size_t draw{};
    std::size_t drawIndexedInstanced{};
    std::size_t drawInstanced{};
    std::size_t map{};
    std::size_t unmap{};
    std::size_t updateSubresource{};
    std::size_t iaSetInputLayout{};
    std::size_t iaSetVertexBuffers{};
    std::size_t iaSetIndexBuffer{};
    std::size_t omSetRenderTargets{};
    std::size_t rsSetViewports{};
    std::size_t csSetShader{};
    std::size_t csSetShaderResources{};
    std::size_t csSetConstantBuffers{};
    std::size_t csSetUnorderedAccessViews{};
    std::size_t dispatch{};
    std::size_t executeCommandList{};
    std::size_t finishCommandList{};
    std::size_t drawIndexedInstancedIndirect{};
    std::size_t drawInstancedIndirect{};
    std::size_t vsSetConstantBuffers1{};
    std::size_t csSetConstantBuffers1{};
    std::size_t updateSubresource1{};
};

/** @return The slots, computed from the SDK headers this module was compiled against. */
[[nodiscard]] Slots slots() noexcept;

} // namespace sunrise::client::hooks::vr::d3d
