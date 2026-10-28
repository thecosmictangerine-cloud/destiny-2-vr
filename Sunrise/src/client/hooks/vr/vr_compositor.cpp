/**
 * The module's own passes (vr_compositor.h). Each is one full-target triangle, its own shaders,
 * sampler, rasterizer and constant buffer, run inside a device-context state of its own so the
 * game's pipeline state is swapped out and back in untouched -- the same discipline as the blit in
 * xr_runtime.cpp. Colours are computed in linear light and written through an sRGB view.
 */

#include "vr_compositor.h"

#include <Windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"

namespace sunrise::client::hooks::vr::compositor {
namespace {

void report(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, text);
}

template <typename T> void release_com(T*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

constexpr const char* kVertex = R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V main(uint id : SV_VertexID) {
    V o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// The panel: the crop resampled with four taps a quarter of an output pixel apart (text does not
// shimmer when the source is a little larger than the panel), rounded corners with an anti-aliased
// edge, the pointer's dot, premultiplied out. The source is read through an sRGB view: linear in.
constexpr const char* kPanelPixel = R"(
cbuffer C : register(b0) { float4 crop; float4 src; float4 shape; float4 ptr; float4 tone; };
Texture2D tx : register(t0); SamplerState sm : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 px = crop.xy + uv * crop.zw;
    float2 dx = ddx(px) * 0.25, dy = ddy(px) * 0.25;
    float3 c = (tx.Sample(sm, (px + dx + dy) / src.xy).rgb + tx.Sample(sm, (px + dx - dy) / src.xy).rgb
              + tx.Sample(sm, (px - dx + dy) / src.xy).rgb + tx.Sample(sm, (px - dx - dy) / src.xy).rgb) * 0.25;
    c = lerp(c, dot(c, float3(0.2126, 0.7152, 0.0722)), tone.y) * tone.x;
    float2 q = (uv - 0.5) * float2(shape.z, 1);
    float2 h = float2(shape.z, 1) * 0.5 - shape.x;
    float2 a = abs(q) - h;
    float d = length(max(a, 0)) + min(max(a.x, a.y), 0) - shape.x;
    float m = saturate(-d / max(shape.y, 1e-4));
    if (ptr.z > 0) {
        float2 e = (uv - ptr.xy) * float2(shape.z, 1);
        float r = length(e);
        float ring = saturate((ptr.w - r) / (ptr.w * 0.25));
        float core = saturate((ptr.w * 0.55 - r) / (ptr.w * 0.2));
        c = lerp(c, float3(0.02, 0.02, 0.02), ring * ptr.z);
        c = lerp(c, float3(0.95, 0.97, 1.0), core * ptr.z);
    }
    return float4(c, 1) * (m * src.z);
}
)";

// The room around the screen: each pixel is a direction (the eye's tangents turned by its pose
// now), a dark environment by that direction -- a faint horizon, the screen's light spilling round
// it -- and where the frozen game covers it, the game, dimmed, exactly where it was seen.
constexpr const char* kBackdropPixel = R"(
cbuffer C : register(b0) { float4 eyeQ; float4 tans; float4 snapQ; float4 snapT; float4 p0; float4 glow; };
Texture2D snap : register(t0); SamplerState sm : register(s0);
float3 qrot(float4 q, float3 v) { return v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 dl = normalize(float3(lerp(tans.x, tans.y, uv.x), lerp(tans.z, tans.w, uv.y), -1));
    float3 w = qrot(eyeQ, dl);
    float y = w.y;
    float3 sky = lerp(float3(0.0075, 0.0080, 0.0105), float3(0.0020, 0.0022, 0.0032), saturate(y * 1.4));
    float3 flo = lerp(float3(0.0055, 0.0052, 0.0056), float3(0.0012, 0.0012, 0.0014), saturate(-y * 2.0));
    float3 env = lerp(flo, sky, smoothstep(-0.03, 0.03, y));
    env += float3(0.0040, 0.0035, 0.0030) * exp(-y * y * 250);
    float g = saturate(dot(w, glow.xyz));
    env += float3(0.020, 0.022, 0.026) * pow(g, 14) * glow.w;
    float3 col = env * p0.y;
    if (p0.x > 0) {
        float3 s = qrot(float4(-snapQ.xyz, snapQ.w), w);
        if (s.z < -0.05) {
            float sx = s.x / -s.z, sy = s.y / -s.z;
            float2 st = float2((sx - snapT.x) / (snapT.y - snapT.x), (snapT.z - sy) / (snapT.z - snapT.w));
            float2 e = min(st, 1 - st);
            float edge = saturate(min(e.x, e.y) / 0.08);
            if (edge > 0) {
                float3 c = snap.SampleLevel(sm, saturate(st), 0).rgb;
                c = lerp(c, dot(c, float3(0.2126, 0.7152, 0.0722)), p0.w) * p0.x;
                col = lerp(col, c, edge);
            }
        }
    }
    col *= p0.z;
    float n = frac(sin(dot(pos.xy, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
    return float4(max(col + n * 0.0005, 0), 1);
}
)";

// A solid: uniform (a fade) or fading along U and soft across V (the beam).
constexpr const char* kSolidPixel = R"(
cbuffer C : register(b0) { float4 colour; float4 shape; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float along = shape.x > 0 ? saturate(1 - uv.x) : 1;
    along = shape.x > 0 ? along * along * saturate(uv.x * 20) : 1;
    float across = shape.y > 0 ? saturate((0.5 - abs(uv.y - 0.5)) / shape.y) : 1;
    float a = colour.a * along * across;
    return float4(colour.rgb * a, a);
}
)";

/** One pass: shaders, sampler, rasterizer, constant buffer, its own context state. */
struct Pass final {
    bool ready{};
    bool failed{};
    ID3D11VertexShader* vertex{};
    ID3D11PixelShader* pixel{};
    ID3D11SamplerState* sampler{};
    ID3D11RasterizerState* rasterizer{};
    ID3D11Buffer* constants{};
    ID3DDeviceContextState* state{};
    std::uint32_t constantBytes{};
    std::uint64_t draws{};
    std::uint32_t errors{};

    bool init(ID3D11Device* device, const char* pixelSource, const char* name, std::uint32_t bytes) noexcept {
        if (ready || failed) {
            return ready;
        }
        failed = true;
        ID3DBlob* vs = nullptr;
        ID3DBlob* ps = nullptr;
        ID3DBlob* errorsBlob = nullptr;
        HRESULT hr = D3DCompile(kVertex, std::strlen(kVertex), name, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, &errorsBlob);
        if (SUCCEEDED(hr)) {
            release_com(errorsBlob);
            hr = D3DCompile(pixelSource, std::strlen(pixelSource), name, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps,
                            &errorsBlob);
        }
        if (FAILED(hr)) {
            std::array<char, 400> line{};
            std::snprintf(line.data(), line.size(), "ev=vr.compositor pass=%s result=compile_fail %s", name,
                          errorsBlob != nullptr ? static_cast<const char*>(errorsBlob->GetBufferPointer()) : "");
            report(line.data());
            release_com(errorsBlob);
            release_com(vs);
            release_com(ps);
            return false;
        }
        device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex);
        device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel);
        release_com(vs);
        release_com(ps);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd, &sampler);
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rd, &rasterizer);
        constantBytes = (bytes + 15U) & ~15U;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = constantBytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        device->CreateBuffer(&bd, nullptr, &constants);
        ID3D11Device1* device1 = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1))) && device1 != nullptr) {
            const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
            device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state);
            device1->Release();
        }
        if (vertex == nullptr || pixel == nullptr || sampler == nullptr || rasterizer == nullptr || constants == nullptr
            || state == nullptr) {
            std::array<char, 160> line{};
            std::snprintf(line.data(), line.size(), "ev=vr.compositor pass=%s result=resource_fail", name);
            report(line.data());
            return false;
        }
        failed = false;
        ready = true;
        std::array<char, 160> line{};
        std::snprintf(line.data(), line.size(), "ev=vr.compositor pass=%s result=ready", name);
        report(line.data());
        return true;
    }

    /** One triangle into the target, reading `view` (may be null), with `k` as the constants. */
    bool run(ID3D11Device* device, ID3D11DeviceContext* context, const Target& target, const void* k,
             ID3D11ShaderResourceView* view, const char* name, bool blend) noexcept {
        ID3D11RenderTargetView* rtv = nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = static_cast<DXGI_FORMAT>(target.format);
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        const HRESULT hr = device->CreateRenderTargetView(target.texture, &rd, &rtv);
        if (FAILED(hr) || rtv == nullptr) {
            if (errors++ < 5) {
                std::array<char, 160> line{};
                std::snprintf(line.data(), line.size(), "ev=vr.compositor pass=%s rtv=fail hr=0x%08lX format=%u", name,
                              static_cast<unsigned long>(hr), target.format);
                report(line.data());
            }
            return false;
        }
        ID3D11DeviceContext1* context1 = nullptr;
        if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))
            || context1 == nullptr) {
            rtv->Release();
            return false;
        }
        ID3DDeviceContextState* previous = nullptr;
        context1->SwapDeviceContextState(state, &previous);
        context1->UpdateSubresource(constants, 0, nullptr, k, 0, 0);
        if (blend) {
            // Nothing blends here: the passes write whole targets. Kept for a pass that would.
        }
        const float clear[4] = {0, 0, 0, 0};
        context1->ClearRenderTargetView(rtv, clear);
        context1->OMSetRenderTargets(1, &rtv, nullptr);
        const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(target.width), static_cast<float>(target.height), 0.0F, 1.0F};
        context1->RSSetViewports(1, &viewport);
        context1->RSSetState(rasterizer);
        context1->IASetInputLayout(nullptr);
        context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context1->VSSetShader(vertex, nullptr, 0);
        context1->PSSetShader(pixel, nullptr, 0);
        context1->PSSetShaderResources(0, 1, &view);
        context1->PSSetSamplers(0, 1, &sampler);
        context1->PSSetConstantBuffers(0, 1, &constants);
        context1->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context1->PSSetShaderResources(0, 1, &none);
        context1->OMSetRenderTargets(0, nullptr, nullptr);
        context1->SwapDeviceContextState(previous, nullptr);
        release_com(previous);
        context1->Release();
        rtv->Release();
        ++draws;
        return true;
    }

    void release() noexcept {
        release_com(vertex);
        release_com(pixel);
        release_com(sampler);
        release_com(rasterizer);
        release_com(constants);
        release_com(state);
        ready = false;
        failed = false;
    }
};

Pass g_panel{};
Pass g_backdrop{};
Pass g_solid{};

/** A copy of a picture a shader can read, through an sRGB view. */
struct Copy final {
    ID3D11Texture2D* texture{};
    ID3D11ShaderResourceView* view{};
    std::uint32_t width{};
    std::uint32_t height{};
    DXGI_FORMAT family{DXGI_FORMAT_UNKNOWN};

    void release() noexcept {
        release_com(view);
        release_com(texture);
        width = 0;
        height = 0;
        family = DXGI_FORMAT_UNKNOWN;
    }
};
std::array<Copy, 4> g_copies{};
/** The panel's crop copy. */
Copy g_panelCopy{};

/** The typeless family of an 8-bit RGBA/BGRA format, or UNKNOWN for anything else. */
[[nodiscard]] DXGI_FORMAT typeless_of(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] DXGI_FORMAT srgb_of(DXGI_FORMAT typeless) noexcept {
    return typeless == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

/** Copies `box` of `source` into `copy` (rebuilt when the size or family changes). */
[[nodiscard]] bool copy_region(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
                               const D3D11_BOX& box, Copy& copy) noexcept {
    D3D11_TEXTURE2D_DESC sd{};
    source->GetDesc(&sd);
    const DXGI_FORMAT family = typeless_of(sd.Format);
    if (family == DXGI_FORMAT_UNKNOWN || sd.SampleDesc.Count != 1) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::array<char, 160> line{};
            std::snprintf(line.data(), line.size(), "ev=vr.compositor copy=unsupported format=%d samples=%u",
                          static_cast<int>(sd.Format), sd.SampleDesc.Count);
            report(line.data());
        }
        return false;
    }
    const std::uint32_t w = box.right - box.left;
    const std::uint32_t h = box.bottom - box.top;
    if (copy.texture == nullptr || copy.width != w || copy.height != h || copy.family != family) {
        copy.release();
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = w;
        cd.Height = h;
        cd.MipLevels = 1;
        cd.ArraySize = 1;
        cd.Format = family;
        cd.SampleDesc.Count = 1;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = srgb_of(family);
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateTexture2D(&cd, nullptr, &copy.texture))
            || FAILED(device->CreateShaderResourceView(copy.texture, &vd, &copy.view))) {
            copy.release();
            report("ev=vr.compositor copy=create_fail");
            return false;
        }
        copy.width = w;
        copy.height = h;
        copy.family = family;
    }
    context->CopySubresourceRegion(copy.texture, 0, 0, 0, 0, source, 0, &box);
    return true;
}

} // namespace

bool draw_panel(ID3D11Device* device, ID3D11DeviceContext* context, const PanelDraw& panel, const Target& target) noexcept {
    if (device == nullptr || context == nullptr || panel.source == nullptr || target.texture == nullptr
        || !g_panel.init(device, kPanelPixel, "panel", 80)) {
        return false;
    }
    D3D11_TEXTURE2D_DESC sd{};
    panel.source->GetDesc(&sd);
    const auto x0 = static_cast<UINT>((std::max)(0.0F, panel.crop[0]));
    const auto y0 = static_cast<UINT>((std::max)(0.0F, panel.crop[1]));
    const UINT x1 = (std::min)(sd.Width, static_cast<UINT>(panel.crop[0] + panel.crop[2]));
    const UINT y1 = (std::min)(sd.Height, static_cast<UINT>(panel.crop[1] + panel.crop[3]));
    if (x1 <= x0 + 8 || y1 <= y0 + 8) {
        return false;
    }
    const D3D11_BOX box{x0, y0, 0, x1, y1, 1};
    if (!copy_region(device, context, panel.source, box, g_panelCopy)) {
        return false;
    }
    const float w = static_cast<float>(x1 - x0);
    const float h = static_cast<float>(y1 - y0);
    const float soft = (std::max)(0.0025F, 1.5F / static_cast<float>(target.height));
    const float k[20] = {0.0F, 0.0F, w, h, w, h, panel.alpha, 0.0F, panel.corner, soft, panel.aspect, 0.0F,
                         panel.pointerU, panel.pointerV, panel.pointerAlpha, panel.pointerRadius,
                         panel.brightness, panel.desaturate, 0.0F, 0.0F};
    return g_panel.run(device, context, target, k, g_panelCopy.view, "panel", false);
}

bool draw_backdrop(ID3D11Device* device, ID3D11DeviceContext* context, const BackdropDraw& b, const Target& target) noexcept {
    if (device == nullptr || context == nullptr || target.texture == nullptr
        || !g_backdrop.init(device, kBackdropPixel, "backdrop", 96)) {
        return false;
    }
    const bool snap = b.snapshot != nullptr && b.snapshotBrightness > 0.0F;
    const float k[24] = {b.eyeOrientation[0], b.eyeOrientation[1], b.eyeOrientation[2], b.eyeOrientation[3],
                         b.eyeTangents[0], b.eyeTangents[1], b.eyeTangents[2], b.eyeTangents[3],
                         b.snapshotOrientation[0], b.snapshotOrientation[1], b.snapshotOrientation[2], b.snapshotOrientation[3],
                         b.snapshotTangents[0], b.snapshotTangents[1], b.snapshotTangents[2], b.snapshotTangents[3],
                         snap ? b.snapshotBrightness : 0.0F, b.environment, b.fade, b.desaturate,
                         b.glowDirection[0], b.glowDirection[1], b.glowDirection[2], b.glow};
    return g_backdrop.run(device, context, target, k, snap ? b.snapshot : nullptr, "backdrop", false);
}

bool draw_solid(ID3D11Device* device, ID3D11DeviceContext* context, const SolidDraw& solid, const Target& target) noexcept {
    if (device == nullptr || context == nullptr || target.texture == nullptr
        || !g_solid.init(device, kSolidPixel, "solid", 32)) {
        return false;
    }
    const float k[8] = {solid.colour[0], solid.colour[1], solid.colour[2], solid.colour[3], solid.gradient, solid.softEdge, 0.0F, 0.0F};
    return g_solid.run(device, context, target, k, nullptr, "solid", false);
}

ID3D11ShaderResourceView* snapshot(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
                                   unsigned slot) noexcept {
    if (device == nullptr || context == nullptr || source == nullptr || slot >= g_copies.size()) {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC sd{};
    source->GetDesc(&sd);
    const D3D11_BOX box{0, 0, 0, sd.Width, sd.Height, 1};
    return copy_region(device, context, source, box, g_copies[slot]) ? g_copies[slot].view : nullptr;
}

void release() noexcept {
    g_panel.release();
    g_backdrop.release();
    g_solid.release();
    for (Copy& copy : g_copies) {
        copy.release();
    }
    g_panelCopy.release();
}

} // namespace sunrise::client::hooks::vr::compositor
