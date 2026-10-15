/**
 * Vtable slots of the D3D11 interfaces the GPU probe detours, taken from the SDK's own C
 * declarations rather than counted by hand.
 *
 * Isolated in its own translation unit because it has to see the headers with CINTERFACE defined,
 * which turns every interface into a plain struct whose `lpVtbl` points at a struct of named
 * function pointers -- and `offsetof` into that struct is the slot, exactly, for whatever SDK this
 * is compiled against. A wrong slot detours the wrong method and corrupts the call silently, so a
 * table recited from memory is not good enough here.
 */

#define CINTERFACE
#define COBJMACROS
// The CD3D11_* helpers call methods, which a C-style interface does not have.
#define D3D11_NO_HELPERS
#include <d3d11_1.h>

#include <cstddef>

#include "vr_d3d_slots.h"

namespace sunrise::client::hooks::vr::d3d {
namespace {

#define SVR_SLOT(vtbl, method) (offsetof(vtbl, method) / sizeof(void*))

} // namespace

Slots slots() noexcept {
    Slots out{};
    out.createBuffer = SVR_SLOT(ID3D11DeviceVtbl, CreateBuffer);
    out.createInputLayout = SVR_SLOT(ID3D11DeviceVtbl, CreateInputLayout);
    out.createVertexShader = SVR_SLOT(ID3D11DeviceVtbl, CreateVertexShader);
    out.createPixelShader = SVR_SLOT(ID3D11DeviceVtbl, CreatePixelShader);
    out.createComputeShader = SVR_SLOT(ID3D11DeviceVtbl, CreateComputeShader);
    out.createDeferredContext = SVR_SLOT(ID3D11DeviceVtbl, CreateDeferredContext);

    out.vsSetConstantBuffers = SVR_SLOT(ID3D11DeviceContext1Vtbl, VSSetConstantBuffers);
    out.vsSetShader = SVR_SLOT(ID3D11DeviceContext1Vtbl, VSSetShader);
    out.vsSetShaderResources = SVR_SLOT(ID3D11DeviceContext1Vtbl, VSSetShaderResources);
    out.psSetShader = SVR_SLOT(ID3D11DeviceContext1Vtbl, PSSetShader);
    out.drawIndexed = SVR_SLOT(ID3D11DeviceContext1Vtbl, DrawIndexed);
    out.draw = SVR_SLOT(ID3D11DeviceContext1Vtbl, Draw);
    out.drawIndexedInstanced = SVR_SLOT(ID3D11DeviceContext1Vtbl, DrawIndexedInstanced);
    out.drawInstanced = SVR_SLOT(ID3D11DeviceContext1Vtbl, DrawInstanced);
    out.map = SVR_SLOT(ID3D11DeviceContext1Vtbl, Map);
    out.unmap = SVR_SLOT(ID3D11DeviceContext1Vtbl, Unmap);
    out.updateSubresource = SVR_SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource);
    out.iaSetInputLayout = SVR_SLOT(ID3D11DeviceContext1Vtbl, IASetInputLayout);
    out.iaSetVertexBuffers = SVR_SLOT(ID3D11DeviceContext1Vtbl, IASetVertexBuffers);
    out.iaSetIndexBuffer = SVR_SLOT(ID3D11DeviceContext1Vtbl, IASetIndexBuffer);
    out.omSetRenderTargets = SVR_SLOT(ID3D11DeviceContext1Vtbl, OMSetRenderTargets);
    out.rsSetViewports = SVR_SLOT(ID3D11DeviceContext1Vtbl, RSSetViewports);
    out.csSetShader = SVR_SLOT(ID3D11DeviceContext1Vtbl, CSSetShader);
    out.csSetShaderResources = SVR_SLOT(ID3D11DeviceContext1Vtbl, CSSetShaderResources);
    out.csSetConstantBuffers = SVR_SLOT(ID3D11DeviceContext1Vtbl, CSSetConstantBuffers);
    out.csSetUnorderedAccessViews = SVR_SLOT(ID3D11DeviceContext1Vtbl, CSSetUnorderedAccessViews);
    out.dispatch = SVR_SLOT(ID3D11DeviceContext1Vtbl, Dispatch);
    out.executeCommandList = SVR_SLOT(ID3D11DeviceContext1Vtbl, ExecuteCommandList);
    out.finishCommandList = SVR_SLOT(ID3D11DeviceContext1Vtbl, FinishCommandList);
    out.drawIndexedInstancedIndirect = SVR_SLOT(ID3D11DeviceContext1Vtbl, DrawIndexedInstancedIndirect);
    out.drawInstancedIndirect = SVR_SLOT(ID3D11DeviceContext1Vtbl, DrawInstancedIndirect);
    out.vsSetConstantBuffers1 = SVR_SLOT(ID3D11DeviceContext1Vtbl, VSSetConstantBuffers1);
    out.csSetConstantBuffers1 = SVR_SLOT(ID3D11DeviceContext1Vtbl, CSSetConstantBuffers1);
    out.updateSubresource1 = SVR_SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource1);
    return out;
}

#undef SVR_SLOT

} // namespace sunrise::client::hooks::vr::d3d
