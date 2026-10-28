/**
 * The off-hand weapon's image. See vr_offhand.h for what it is; docs/DUAL-WIELD.md for why it is
 * built this way.
 *
 * What a weapon draw is, measured on GPU captures in Io (2026-10-16): Destiny skins the weapon with
 * its own small bone palettes (1, 8 and 1 bones on the hand cannon), uploaded into the same 64 KB
 * dynamic constant buffer the arms use (slot 11), camera-relative in world axes, every palette's
 * bone 0 the weapon's root. The weapon is drawn in the two passes the arms are drawn in -- the
 * depth prepass (no render target, the main depth buffer) and the G-buffer (MRT) -- right after the
 * gloves. Its other constant buffers split cleanly in two: slot 0 per object (a different range or
 * buffer per draw), slots 2, 9, 10, 12 and 13 per view or per frame, the very same bindings the arm
 * draws of the pass have. That is the rule used here: a slot bound exactly as the pass's arm draw
 * binds it is live (the replay leaves the pass's own binding in place); anything else is the
 * weapon's, and copied.
 *
 * A recording therefore holds, per weapon draw: shaders, input layout, topology, vertex and index
 * buffers, every bound shader resource and sampler, the blend, depth-stencil and rasterizer states,
 * the draw's arguments, a GPU copy of each per-object constant buffer taken at the very point in the
 * command stream the draw read it, and the CPU copy of its bone palette. Immutable objects are held
 * by reference; everything the engine writes per frame is copied.
 */

#include "vr_offhand.h"

#include <Windows.h>

#include <d3d11_1.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../../core/logging/log.h"
#include "runtime.h"
#include "vr_frames.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::offhand {
namespace {

using math::Affine;
using math::M3;
using math::V3;

constexpr UINT kCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
constexpr UINT kSrvSlots = 32;
constexpr UINT kSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
constexpr UINT kVbSlots = 4;
constexpr std::size_t kHeaderFloats = 8 * 4;
constexpr std::size_t kMaxBones = 128;
constexpr std::size_t kMaxDraws = 96;
constexpr std::size_t kMaxRecordings = 8;
constexpr std::size_t kMaxPasses = 6;
constexpr UINT kPaletteBytes = 65536;
constexpr UINT kPaletteSlot = 11;

void report(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, text);
}

template <typename... Args> void reportf(const char* format, Args... args) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(), line.size(), format, args...);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size() ? static_cast<std::size_t>(written)
                                                                            : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::warn, {line.data(), length});
    }
}

template <typename T> void release(T*& p) noexcept {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

/**
 * A pass, by what it renders into: the format of each colour target (up to 8) and of the depth
 * buffer. By FORMAT, not by resource: the targets are recreated whenever the render changes size
 * (vr_display.cpp reshapes it to the eye, `shape desktop` back), and a recording keyed on resource
 * pointers silently stopped matching any pass after that -- the gun simply vanished. The passes the
 * weapon is drawn in (depth prepass: no colour target; G-buffer: four) differ in their formats.
 */
struct PassSig final {
    std::array<DXGI_FORMAT, 8> rt{};
    DXGI_FORMAT ds{DXGI_FORMAT_UNKNOWN};
    bool depth{};
    bool operator==(const PassSig& o) const noexcept { return rt == o.rt && ds == o.ds && depth == o.depth; }
};

PassSig pass_of(ID3D11DeviceContext* context) noexcept {
    PassSig sig{};
    std::array<ID3D11RenderTargetView*, 8> rtvs{};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(8, rtvs.data(), &dsv);
    for (std::size_t i = 0; i < rtvs.size(); ++i) {
        if (rtvs[i] != nullptr) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            rtvs[i]->GetDesc(&desc);
            sig.rt[i] = desc.Format;
            rtvs[i]->Release();
        }
    }
    if (dsv != nullptr) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        dsv->GetDesc(&desc);
        sig.ds = desc.Format;
        sig.depth = true;
        dsv->Release();
    }
    return sig;
}

/** One constant buffer binding: what the engine had bound, and our copy when it is the object's own. */
struct CbBinding final {
    ID3D11Buffer* original{};
    UINT first{};
    UINT num{};
    /** A copy of the whole buffer, taken in the command stream right where the draw read it. */
    ID3D11Buffer* copy{};
    /** Bound exactly as the pass's arm draw binds it: per view or per frame, left as the pass has it. */
    bool live{};
};

/** The state of one stage that a replay has to set: shader, constants, resources, samplers. */
template <typename Shader> struct StageState final {
    Shader* shader{};
    std::array<CbBinding, kCbSlots> cb{};
    std::array<ID3D11ShaderResourceView*, kSrvSlots> srv{};
    std::array<ID3D11SamplerState*, kSamplerSlots> sampler{};
};

struct RecordedDraw final {
    int pass{-1};
    int kind{};
    UINT count{};
    UINT instances{};
    UINT start{};
    INT base{};
    ID3D11InputLayout* layout{};
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    std::array<ID3D11Buffer*, kVbSlots> vb{};
    std::array<UINT, kVbSlots> stride{};
    std::array<UINT, kVbSlots> offset{};
    ID3D11Buffer* ib{};
    DXGI_FORMAT ibFormat{};
    UINT ibOffset{};
    StageState<ID3D11VertexShader> vs{};
    StageState<ID3D11PixelShader> ps{};
    ID3D11BlendState* blend{};
    std::array<float, 4> blendFactor{};
    UINT sampleMask{};
    ID3D11DepthStencilState* depth{};
    UINT stencilRef{};
    ID3D11RasterizerState* raster{};
    /** Index into the recording's palettes. */
    int palette{-1};
};

struct RecordedPalette final {
    std::uint64_t serial{};
    std::array<float, kHeaderFloats> header{};
    std::array<float, 12 * kMaxBones> bones{};
    std::size_t count{};
    /** Ours: re-uploaded with the bones moved onto the left controller every frame. */
    ID3D11Buffer* buffer{};
};

struct Recording final {
    std::uintptr_t key{};
    bool complete{};
    std::vector<RecordedDraw> draws;
    std::vector<RecordedPalette> palettes;
    std::array<PassSig, kMaxPasses> passes{};
    std::size_t passCount{};
    /** The weapon's root bone (palette bone 0), camera-relative, and the right controller, same frame. */
    Affine root{};
    Affine controller{};
    std::uint64_t recordedAt{};
};

// --- Shared between the render thread and the callers -------------------------------------------

SRWLOCK g_lock = SRWLOCK_INIT;
std::array<Recording, kMaxRecordings> g_recordings{};
Placement g_placement{};
/** The left gun's root as last drawn, camera-relative; for the shot. */
Affine g_leftRoot{};
std::uintptr_t g_leftKey = 0;
bool g_leftValid = false;
ULONGLONG g_leftTick = 0;

std::atomic<std::uintptr_t> g_wantKey{0};
/** Whether anything is in the left hand: with nothing to draw and nothing to record, every draw hook returns at once. */
std::atomic_bool g_visible{false};
std::atomic_bool g_wantForce{false};
/** `forget_all` asked; the next present frees every recording. */
std::atomic_bool g_forgetAll{false};
/** `forget` marked recordings incomplete; the next present frees them. */
std::atomic_bool g_forgetSome{false};

// --- Render thread only -------------------------------------------------------------------------

ID3D11Device* g_device = nullptr;

/** The bone-palette buffer and what it holds right now. */
struct PaletteNow final {
    ID3D11Resource* resource{};
    bool weapon{};
    bool arm{};
    std::uint64_t serial{};
    std::array<float, kHeaderFloats> header{};
    std::array<float, 12 * kMaxBones> bones{};
    std::size_t count{};
};
PaletteNow g_now{};

/** A recording in progress: one whole frame of weapon draws, and the arm draws' bindings per pass. */
struct Capture final {
    bool active{};
    std::uintptr_t key{};
    Recording rec{};
    /** The arm draw's constant buffer bindings, per pass: what "live" means there. */
    std::array<std::array<std::pair<ID3D11Buffer*, UINT>, kCbSlots>, kMaxPasses> armVs{};
    std::array<std::array<std::pair<ID3D11Buffer*, UINT>, kCbSlots>, kMaxPasses> armPs{};
    std::array<bool, kMaxPasses> armSeen{};
    std::uint32_t frames{};
};
Capture g_capture{};

/** Per frame: which passes have had their replay. */
std::array<PassSig, kMaxPasses> g_replayedPasses{};
std::size_t g_replayedCount = 0;
std::uint32_t g_frame = 0;
std::atomic<std::uint64_t> g_replays{0};
std::atomic<std::uint64_t> g_replayDraws{0};
std::uint64_t g_paletteSerial = 0;

int find_pass(const std::array<PassSig, kMaxPasses>& passes, std::size_t count, const PassSig& sig) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (passes[i] == sig) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void free_draw(RecordedDraw& d) noexcept {
    release(d.layout);
    for (auto& b : d.vb) {
        release(b);
    }
    release(d.ib);
    auto free_stage = [](auto& s) {
        release(s.shader);
        for (auto& c : s.cb) {
            release(c.original);
            release(c.copy);
        }
        for (auto& v : s.srv) {
            release(v);
        }
        for (auto& v : s.sampler) {
            release(v);
        }
    };
    free_stage(d.vs);
    free_stage(d.ps);
    release(d.blend);
    release(d.depth);
    release(d.raster);
}

void free_recording(Recording& r) noexcept {
    for (auto& d : r.draws) {
        free_draw(d);
    }
    for (auto& p : r.palettes) {
        release(p.buffer);
    }
    r = Recording{};
}

/** A GPU copy of a whole buffer, at this point of the command stream. Null on failure. */
ID3D11Buffer* copy_buffer(ID3D11DeviceContext* context, ID3D11Buffer* source) noexcept {
    if (source == nullptr || g_device == nullptr) {
        return nullptr;
    }
    D3D11_BUFFER_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    ID3D11Buffer* copy = nullptr;
    if (FAILED(g_device->CreateBuffer(&desc, nullptr, &copy)) || copy == nullptr) {
        return nullptr;
    }
    context->CopyResource(copy, source);
    return copy;
}

template <typename Shader, typename GetShader, typename GetCbs, typename GetSrvs, typename GetSamplers>
void take_stage(ID3D11DeviceContext* context, StageState<Shader>& s, GetShader getShader, GetCbs getCbs,
                GetSrvs getSrvs, GetSamplers getSamplers) noexcept {
    getShader(&s.shader);
    std::array<ID3D11Buffer*, kCbSlots> cbs{};
    std::array<UINT, kCbSlots> firsts{};
    std::array<UINT, kCbSlots> nums{};
    getCbs(cbs.data(), firsts.data(), nums.data());
    for (UINT i = 0; i < kCbSlots; ++i) {
        s.cb[i].original = cbs[i];
        s.cb[i].first = firsts[i];
        s.cb[i].num = nums[i];
        if (cbs[i] != nullptr && i != kPaletteSlot) {
            s.cb[i].copy = copy_buffer(context, cbs[i]);
        }
    }
    getSrvs(s.srv.data());
    getSamplers(s.sampler.data());
}

/** Records the draw about to happen into the capture. */
void record_draw(ID3D11DeviceContext* context, int kind, UINT count, UINT instances, UINT start, INT base,
                 int pass) noexcept {
    Recording& rec = g_capture.rec;
    if (rec.draws.size() >= kMaxDraws) {
        return;
    }
    // A geometry, hull or domain shader would need recording too; none has been seen on a weapon.
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    context->GSGetShader(&gs, nullptr, nullptr);
    context->HSGetShader(&hs, nullptr, nullptr);
    context->DSGetShader(&ds, nullptr, nullptr);
    const bool extra = gs != nullptr || hs != nullptr || ds != nullptr;
    release(gs);
    release(hs);
    release(ds);
    if (extra) {
        return;
    }
    // The palette this draw skins with, deduplicated by the Unmap it came from.
    int paletteIndex = -1;
    for (std::size_t i = 0; i < rec.palettes.size(); ++i) {
        if (rec.palettes[i].serial == g_now.serial) {
            paletteIndex = static_cast<int>(i);
        }
    }
    if (paletteIndex < 0) {
        RecordedPalette p{};
        p.serial = g_now.serial;
        p.header = g_now.header;
        p.bones = g_now.bones;
        p.count = g_now.count;
        rec.palettes.push_back(p);
        paletteIndex = static_cast<int>(rec.palettes.size() - 1);
    }
    RecordedDraw d{};
    d.pass = pass;
    d.kind = kind;
    d.count = count;
    d.instances = instances;
    d.start = start;
    d.base = base;
    d.palette = paletteIndex;
    context->IAGetInputLayout(&d.layout);
    context->IAGetPrimitiveTopology(&d.topology);
    context->IAGetVertexBuffers(0, kVbSlots, d.vb.data(), d.stride.data(), d.offset.data());
    context->IAGetIndexBuffer(&d.ib, &d.ibFormat, &d.ibOffset);
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) || c1 == nullptr) {
        free_draw(d);
        return;
    }
    take_stage(
        context, d.vs, [&](ID3D11VertexShader** s) { context->VSGetShader(s, nullptr, nullptr); },
        [&](ID3D11Buffer** b, UINT* f, UINT* n) { c1->VSGetConstantBuffers1(0, kCbSlots, b, f, n); },
        [&](ID3D11ShaderResourceView** v) { context->VSGetShaderResources(0, kSrvSlots, v); },
        [&](ID3D11SamplerState** v) { context->VSGetSamplers(0, kSamplerSlots, v); });
    take_stage(
        context, d.ps, [&](ID3D11PixelShader** s) { context->PSGetShader(s, nullptr, nullptr); },
        [&](ID3D11Buffer** b, UINT* f, UINT* n) { c1->PSGetConstantBuffers1(0, kCbSlots, b, f, n); },
        [&](ID3D11ShaderResourceView** v) { context->PSGetShaderResources(0, kSrvSlots, v); },
        [&](ID3D11SamplerState** v) { context->PSGetSamplers(0, kSamplerSlots, v); });
    c1->Release();
    context->OMGetBlendState(&d.blend, d.blendFactor.data(), &d.sampleMask);
    context->OMGetDepthStencilState(&d.depth, &d.stencilRef);
    context->RSGetState(&d.raster);
    rec.draws.push_back(d);
}

/** The arm draw's constant buffer bindings in this pass: the reference for "live". */
void note_arm_bindings(ID3D11DeviceContext* context, int pass) noexcept {
    if (pass < 0 || g_capture.armSeen[static_cast<std::size_t>(pass)]) {
        return;
    }
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) || c1 == nullptr) {
        return;
    }
    auto take = [&](auto get, std::array<std::pair<ID3D11Buffer*, UINT>, kCbSlots>& out) {
        std::array<ID3D11Buffer*, kCbSlots> cbs{};
        std::array<UINT, kCbSlots> firsts{};
        std::array<UINT, kCbSlots> nums{};
        get(cbs.data(), firsts.data(), nums.data());
        for (UINT i = 0; i < kCbSlots; ++i) {
            out[i] = {cbs[i], firsts[i]};
            release(cbs[i]); // only compared by identity
        }
    };
    take([&](ID3D11Buffer** b, UINT* f, UINT* n) { c1->VSGetConstantBuffers1(0, kCbSlots, b, f, n); },
         g_capture.armVs[static_cast<std::size_t>(pass)]);
    take([&](ID3D11Buffer** b, UINT* f, UINT* n) { c1->PSGetConstantBuffers1(0, kCbSlots, b, f, n); },
         g_capture.armPs[static_cast<std::size_t>(pass)]);
    c1->Release();
    g_capture.armSeen[static_cast<std::size_t>(pass)] = true;
}

/** Turns a frame of captured draws into a recording: only the arms' passes, live slots marked. */
bool finish_capture() noexcept {
    Recording& rec = g_capture.rec;
    std::vector<RecordedDraw> kept;
    for (RecordedDraw& d : rec.draws) {
        const auto pass = static_cast<std::size_t>(d.pass);
        if (d.pass < 0 || !g_capture.armSeen[pass]) {
            free_draw(d);
            continue;
        }
        auto mark = [&](auto& stage, const std::array<std::pair<ID3D11Buffer*, UINT>, kCbSlots>& arm) {
            for (UINT i = 0; i < kCbSlots; ++i) {
                CbBinding& c = stage.cb[i];
                c.live = c.original != nullptr && i != kPaletteSlot && arm[i].first == c.original && arm[i].second == c.first;
                if (c.live) {
                    release(c.copy);
                }
            }
        };
        mark(d.vs, g_capture.armVs[pass]);
        mark(d.ps, g_capture.armPs[pass]);
        kept.push_back(d);
    }
    rec.draws.swap(kept);
    if (rec.draws.empty() || rec.palettes.empty()) {
        return false;
    }
    // Our palette buffers, one per recorded palette.
    for (RecordedPalette& p : rec.palettes) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = kPaletteBytes;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_device->CreateBuffer(&desc, nullptr, &p.buffer))) {
            p.buffer = nullptr;
            return false;
        }
    }
    // The root: bone 0 of the palette with the most bones (every weapon palette shares bone 0).
    const RecordedPalette* main = &rec.palettes[0];
    for (const RecordedPalette& p : rec.palettes) {
        if (p.count > main->count) {
            main = &p;
        }
    }
    rec.root = math::read_rows(main->bones.data());
    rec.root.m = math::orthonormalize(rec.root.m);
    return true;
}

/** The right controller as a frame, camera-relative: columns (forward, LEFT, up), origin at the palm. */
Affine controller_frame(const xr::HandPose& hand, const xr::HeadPose& head) noexcept {
    const V3 f{hand.forward[0], hand.forward[1], hand.forward[2]};
    const V3 u{hand.up[0], hand.up[1], hand.up[2]};
    const V3 l{-hand.right[0], -hand.right[1], -hand.right[2]};
    return Affine{math::orthonormalize(M3{{f, l, u}}),
                  V3{hand.palm[0] - head.offset[0], hand.palm[1] - head.offset[1], hand.palm[2] - head.offset[2]}};
}

/** Mirror across the controller's own midplane: S * a * S, S = diag(1, -1, 1). A proper rigid motion. */
Affine mirror(const Affine& a) noexcept {
    auto flip = [](const V3& v) { return V3{v.x, -v.y, v.z}; };
    M3 m{{flip(a.m.c[0]), -flip(a.m.c[1]), flip(a.m.c[2])}};
    return Affine{m, flip(a.t)};
}

// --- Replay -------------------------------------------------------------------------------------

/** Everything a replay changes, read back before it and put back after it. */
struct Saved final {
    ID3D11InputLayout* layout{};
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    std::array<ID3D11Buffer*, kVbSlots> vb{};
    std::array<UINT, kVbSlots> stride{};
    std::array<UINT, kVbSlots> offset{};
    ID3D11Buffer* ib{};
    DXGI_FORMAT ibFormat{};
    UINT ibOffset{};
    ID3D11VertexShader* vs{};
    ID3D11PixelShader* ps{};
    std::array<ID3D11Buffer*, kCbSlots> vsCb{};
    std::array<UINT, kCbSlots> vsFirst{};
    std::array<UINT, kCbSlots> vsNum{};
    std::array<ID3D11Buffer*, kCbSlots> psCb{};
    std::array<UINT, kCbSlots> psFirst{};
    std::array<UINT, kCbSlots> psNum{};
    std::array<ID3D11ShaderResourceView*, kSrvSlots> vsSrv{};
    std::array<ID3D11ShaderResourceView*, kSrvSlots> psSrv{};
    std::array<ID3D11SamplerState*, kSamplerSlots> vsSampler{};
    std::array<ID3D11SamplerState*, kSamplerSlots> psSampler{};
    ID3D11BlendState* blend{};
    std::array<float, 4> blendFactor{};
    UINT sampleMask{};
    ID3D11DepthStencilState* depth{};
    UINT stencilRef{};
    ID3D11RasterizerState* raster{};
};

void save(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, Saved& s) noexcept {
    c->IAGetInputLayout(&s.layout);
    c->IAGetPrimitiveTopology(&s.topology);
    c->IAGetVertexBuffers(0, kVbSlots, s.vb.data(), s.stride.data(), s.offset.data());
    c->IAGetIndexBuffer(&s.ib, &s.ibFormat, &s.ibOffset);
    c->VSGetShader(&s.vs, nullptr, nullptr);
    c->PSGetShader(&s.ps, nullptr, nullptr);
    c1->VSGetConstantBuffers1(0, kCbSlots, s.vsCb.data(), s.vsFirst.data(), s.vsNum.data());
    c1->PSGetConstantBuffers1(0, kCbSlots, s.psCb.data(), s.psFirst.data(), s.psNum.data());
    c->VSGetShaderResources(0, kSrvSlots, s.vsSrv.data());
    c->PSGetShaderResources(0, kSrvSlots, s.psSrv.data());
    c->VSGetSamplers(0, kSamplerSlots, s.vsSampler.data());
    c->PSGetSamplers(0, kSamplerSlots, s.psSampler.data());
    c->OMGetBlendState(&s.blend, s.blendFactor.data(), &s.sampleMask);
    c->OMGetDepthStencilState(&s.depth, &s.stencilRef);
    c->RSGetState(&s.raster);
}

void restore(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, Saved& s) noexcept {
    c->IASetInputLayout(s.layout);
    c->IASetPrimitiveTopology(s.topology);
    c->IASetVertexBuffers(0, kVbSlots, s.vb.data(), s.stride.data(), s.offset.data());
    c->IASetIndexBuffer(s.ib, s.ibFormat, s.ibOffset);
    c->VSSetShader(s.vs, nullptr, 0);
    c->PSSetShader(s.ps, nullptr, 0);
    c1->VSSetConstantBuffers1(0, kCbSlots, s.vsCb.data(), s.vsFirst.data(), s.vsNum.data());
    c1->PSSetConstantBuffers1(0, kCbSlots, s.psCb.data(), s.psFirst.data(), s.psNum.data());
    c->VSSetShaderResources(0, kSrvSlots, s.vsSrv.data());
    c->PSSetShaderResources(0, kSrvSlots, s.psSrv.data());
    c->VSSetSamplers(0, kSamplerSlots, s.vsSampler.data());
    c->PSSetSamplers(0, kSamplerSlots, s.psSampler.data());
    c->OMSetBlendState(s.blend, s.blendFactor.data(), s.sampleMask);
    c->OMSetDepthStencilState(s.depth, s.stencilRef);
    c->RSSetState(s.raster);
    release(s.layout);
    for (auto& b : s.vb) {
        release(b);
    }
    release(s.ib);
    release(s.vs);
    release(s.ps);
    for (auto& b : s.vsCb) {
        release(b);
    }
    for (auto& b : s.psCb) {
        release(b);
    }
    for (auto& v : s.vsSrv) {
        release(v);
    }
    for (auto& v : s.psSrv) {
        release(v);
    }
    for (auto& v : s.vsSampler) {
        release(v);
    }
    for (auto& v : s.psSampler) {
        release(v);
    }
    release(s.blend);
    release(s.depth);
    release(s.raster);
}

template <typename Shader, typename SetCbs>
void bind_constants(const StageState<Shader>& stage, const Saved& saved, bool vertex, SetCbs setCbs,
                    ID3D11Buffer* palette) noexcept {
    std::array<ID3D11Buffer*, kCbSlots> cbs{};
    std::array<UINT, kCbSlots> firsts{};
    std::array<UINT, kCbSlots> nums{};
    for (UINT i = 0; i < kCbSlots; ++i) {
        const CbBinding& c = stage.cb[i];
        if (i == kPaletteSlot && c.original != nullptr) {
            cbs[i] = palette;
            firsts[i] = c.first;
            nums[i] = c.num;
        } else if (c.live) {
            cbs[i] = vertex ? saved.vsCb[i] : saved.psCb[i];
            firsts[i] = vertex ? saved.vsFirst[i] : saved.psFirst[i];
            nums[i] = vertex ? saved.vsNum[i] : saved.psNum[i];
        } else {
            cbs[i] = c.copy;
            firsts[i] = c.first;
            nums[i] = c.num;
        }
    }
    setCbs(cbs.data(), firsts.data(), nums.data());
}

/** The left gun's palettes for this frame, uploaded. @return False when there is nothing to draw. */
bool upload_palettes(ID3D11DeviceContext* context, Recording& rec, const Affine& move) noexcept {
    for (RecordedPalette& p : rec.palettes) {
        if (p.buffer == nullptr) {
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(p.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        auto* out = static_cast<float*>(mapped.pData);
        std::memcpy(out, p.header.data(), sizeof p.header);
        for (std::size_t b = 0; b < p.count; ++b) {
            const Affine m = move * math::read_rows(p.bones.data() + b * 12);
            math::write_rows(m, out + kHeaderFloats + b * 12);
        }
        context->Unmap(p.buffer, 0);
    }
    return true;
}

/**
 * Where the left gun goes this frame: the recorded gun, carried from the right controller it was
 * recorded on to the left controller of the frame being drawn, mirrored across the controller's
 * midplane (as the left hand's grip is the right one's mirrored), then the kick in the gun's frame.
 * As one rigid transform applied to every recorded bone: move = L * S(C^-1 * root)S * kick * root^-1.
 */
bool placement_for(const Recording& rec, const Placement& placement, Affine& move, Affine& rootNow) noexcept {
    xr::HeadPose head{};
    xr::HandPose left{};
    xr::HandPose right{};
    (void)xr::drawn_sample(head, left, right);
    if (!head.valid || !left.valid) {
        return false;
    }
    const Affine ctrlL = controller_frame(left, head);
    const Affine gunInCtrl = mirror(math::inverse_rigid(rec.controller) * rec.root);
    rootNow = ctrlL * placement.kick * gunInCtrl;
    move = rootNow * math::inverse_rigid(rec.root);
    return math::finite(move.t);
}

void replay(ID3D11DeviceContext* context, int pass) noexcept {
    AcquireSRWLockShared(&g_lock);
    const Placement placement = g_placement;
    Recording* rec = nullptr;
    for (Recording& r : g_recordings) {
        if (r.complete && r.key == placement.key) {
            rec = &r;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (rec == nullptr || !placement.visible) {
        return;
    }
    // The recording's draws for this pass, matched by pass signature (recorded passes are indices
    // into the recording's own list).
    const PassSig now = g_replayedPasses[static_cast<std::size_t>(pass)];
    const int recPass = find_pass(rec->passes, rec->passCount, now);
    if (recPass < 0) {
        return;
    }
    Affine move{};
    Affine rootNow{};
    if (!placement_for(*rec, placement, move, rootNow)) {
        return;
    }
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) || c1 == nullptr) {
        return;
    }
    if (!upload_palettes(context, *rec, move)) {
        c1->Release();
        return;
    }
    Saved saved{};
    save(context, c1, saved);
    std::uint64_t drawn = 0;
    for (const RecordedDraw& d : rec->draws) {
        if (d.pass != recPass) {
            continue;
        }
        context->IASetInputLayout(d.layout);
        context->IASetPrimitiveTopology(d.topology);
        context->IASetVertexBuffers(0, kVbSlots, d.vb.data(), d.stride.data(), d.offset.data());
        context->IASetIndexBuffer(d.ib, d.ibFormat, d.ibOffset);
        context->VSSetShader(d.vs.shader, nullptr, 0);
        context->PSSetShader(d.ps.shader, nullptr, 0);
        ID3D11Buffer* palette = d.palette >= 0 ? rec->palettes[static_cast<std::size_t>(d.palette)].buffer : nullptr;
        bind_constants(d.vs, saved, true,
                       [&](ID3D11Buffer* const* b, const UINT* f, const UINT* n) { c1->VSSetConstantBuffers1(0, kCbSlots, b, f, n); },
                       palette);
        bind_constants(d.ps, saved, false,
                       [&](ID3D11Buffer* const* b, const UINT* f, const UINT* n) { c1->PSSetConstantBuffers1(0, kCbSlots, b, f, n); },
                       palette);
        context->VSSetShaderResources(0, kSrvSlots, d.vs.srv.data());
        context->PSSetShaderResources(0, kSrvSlots, d.ps.srv.data());
        context->VSSetSamplers(0, kSamplerSlots, d.vs.sampler.data());
        context->PSSetSamplers(0, kSamplerSlots, d.ps.sampler.data());
        context->OMSetBlendState(d.blend, d.blendFactor.data(), d.sampleMask);
        context->OMSetDepthStencilState(d.depth, d.stencilRef);
        context->RSSetState(d.raster);
        switch (d.kind) {
        case 0: context->DrawIndexed(d.count, d.start, d.base); break;
        case 1: context->Draw(d.count, d.start); break;
        case 2: context->DrawIndexedInstanced(d.count, d.instances, d.start, d.base, 0); break;
        case 3: context->DrawInstanced(d.count, d.instances, d.start, 0); break;
        default: continue;
        }
        ++drawn;
    }
    restore(context, c1, saved);
    c1->Release();
    g_replays.fetch_add(1, std::memory_order_relaxed);
    g_replayDraws.fetch_add(drawn, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_lock);
    g_leftRoot = rootNow;
    g_leftKey = placement.key;
    g_leftValid = true;
    g_leftTick = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
}

/** The weapon or arm draw about to happen, and the pass it is in; -1 when it is neither. */
struct Landmark final {
    bool weapon{};
    bool arm{};
    PassSig pass{};
};
Landmark g_pending{};

bool palette_bound(ID3D11DeviceContext* context) noexcept {
    if (g_now.resource == nullptr) {
        return false;
    }
    ID3D11Buffer* bound = nullptr;
    context->VSGetConstantBuffers(kPaletteSlot, 1, &bound);
    const bool same = bound != nullptr && bound == g_now.resource;
    release(bound);
    return same;
}

} // namespace

void note_palette(ID3D11Resource* resource, const float* header, const float* bones, std::size_t boneCount,
                  bool weapon) noexcept {
    g_now.resource = resource;
    g_now.weapon = weapon;
    g_now.arm = false;
    g_now.serial = ++g_paletteSerial;
    if (weapon && header != nullptr && bones != nullptr) {
        g_now.count = std::min(boneCount, kMaxBones);
        std::memcpy(g_now.header.data(), header, sizeof(float) * kHeaderFloats);
        std::memcpy(g_now.bones.data(), bones, sizeof(float) * 12 * g_now.count);
    } else {
        g_now.count = 0;
    }
}

void note_arm_palette(ID3D11Resource* resource) noexcept {
    g_now.resource = resource;
    g_now.weapon = false;
    g_now.arm = true;
    g_now.serial = ++g_paletteSerial;
    g_now.count = 0;
}

void before_draw(ID3D11DeviceContext* context, int kind, unsigned count, unsigned instances, unsigned start,
                 int base) noexcept {
    g_pending = Landmark{};
    // Single-handed play costs nothing here: no left weapon on show and no recording due, no work.
    if (context == nullptr || (!g_capture.active && !g_visible.load(std::memory_order_relaxed))
        || (!g_now.weapon && !g_now.arm) || !palette_bound(context)) {
        return;
    }
    g_pending.weapon = g_now.weapon;
    g_pending.arm = g_now.arm;
    g_pending.pass = pass_of(context);
    if (!g_capture.active) {
        return;
    }
    Recording& rec = g_capture.rec;
    int pass = find_pass(rec.passes, rec.passCount, g_pending.pass);
    if (pass < 0 && rec.passCount < kMaxPasses) {
        rec.passes[rec.passCount] = g_pending.pass;
        pass = static_cast<int>(rec.passCount++);
    }
    if (pass < 0) {
        return;
    }
    if (g_pending.arm) {
        note_arm_bindings(context, pass);
    } else if (g_now.count > 0) {
        record_draw(context, kind, count, instances, start, base, pass);
    }
}

void after_draw(ID3D11DeviceContext* context) noexcept {
    if (context == nullptr || (!g_pending.weapon && !g_pending.arm)) {
        return;
    }
    const PassSig sig = g_pending.pass;
    g_pending = Landmark{};
    if (find_pass(g_replayedPasses, g_replayedCount, sig) >= 0 || g_replayedCount >= kMaxPasses) {
        return;
    }
    g_replayedPasses[g_replayedCount] = sig;
    const int pass = static_cast<int>(g_replayedCount++);
    replay(context, pass);
}

void present(ID3D11Device* device) noexcept {
    ++g_frame;
    g_device = device;
    g_replayedCount = 0;
    if (g_forgetAll.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&g_lock);
        for (Recording& r : g_recordings) {
            free_recording(r);
        }
        ReleaseSRWLockExclusive(&g_lock);
        free_recording(g_capture.rec);
        g_capture = Capture{};
        g_wantKey.store(0, std::memory_order_relaxed);
        g_forgetAll.store(false, std::memory_order_release);
        report("ev=vr.offhand forget_all");
    }
    if (g_forgetSome.exchange(false, std::memory_order_acq_rel)) {
        AcquireSRWLockExclusive(&g_lock);
        for (Recording& r : g_recordings) {
            if (!r.complete && r.key != 0) {
                free_recording(r);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (g_capture.active) {
        // One whole frame recorded: keep it if it holds the weapon in an arm pass.
        g_capture.active = false;
        if (finish_capture()) {
            g_capture.rec.key = g_capture.key;
            g_capture.rec.complete = true;
            g_capture.rec.recordedAt = GetTickCount64();
            std::size_t draws = g_capture.rec.draws.size();
            std::size_t palettes = g_capture.rec.palettes.size();
            AcquireSRWLockExclusive(&g_lock);
            Recording* slot = nullptr;
            for (Recording& r : g_recordings) {
                if (r.key == g_capture.key) {
                    slot = &r;
                }
            }
            if (slot == nullptr) {
                for (Recording& r : g_recordings) {
                    if (r.key == 0) {
                        slot = &r;
                        break;
                    }
                }
            }
            if (slot == nullptr) {
                slot = &g_recordings[0];
            }
            free_recording(*slot);
            *slot = std::move(g_capture.rec);
            ReleaseSRWLockExclusive(&g_lock);
            reportf("ev=vr.offhand recorded key=%llX draws=%zu palettes=%zu passes=%zu",
                    static_cast<unsigned long long>(g_capture.key), draws, palettes, slot->passCount);
        } else {
            reportf("ev=vr.offhand record result=fail key=%llX draws=%zu", static_cast<unsigned long long>(g_capture.key),
                    g_capture.rec.draws.size());
            free_recording(g_capture.rec);
        }
        g_capture = Capture{};
    }
    const std::uintptr_t want = g_wantKey.exchange(0, std::memory_order_acq_rel);
    if (want != 0 && device != nullptr) {
        // The controller the coming frame will be drawn with is read at the first weapon draw; the
        // recording keeps the one of the frame it was taken in.
        xr::HeadPose head{};
        xr::HandPose left{};
        xr::HandPose right{};
        (void)xr::drawn_sample(head, left, right);
        if (head.valid && right.valid) {
            g_capture = Capture{};
            g_capture.active = true;
            g_capture.key = want;
            g_capture.rec.controller = controller_frame(right, head);
        }
    }
    if (g_frame % 600 == 0) {
        reportf("ev=vr.offhand stats replays=%llu draws=%llu", static_cast<unsigned long long>(g_replays.exchange(0)),
                static_cast<unsigned long long>(g_replayDraws.exchange(0)));
    }
}

void request_recording(std::uintptr_t key, bool force) noexcept {
    if (key == 0) {
        return;
    }
    if (!force && recorded(key)) {
        return;
    }
    g_wantForce.store(force, std::memory_order_relaxed);
    g_wantKey.store(key, std::memory_order_release);
}

void forget_all() noexcept {
    g_forgetAll.store(true, std::memory_order_release);
}

void forget(std::uintptr_t key) noexcept {
    if (key == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    bool found = false;
    for (Recording& r : g_recordings) {
        if (r.complete && r.key == key) {
            // Incomplete with a key: no longer drawn nor reported, freed by the next present.
            r.complete = false;
            found = true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (found) {
        g_forgetSome.store(true, std::memory_order_release);
        reportf("ev=vr.offhand forget key=%llX", static_cast<unsigned long long>(key));
    }
}

bool recorded(std::uintptr_t key) noexcept {
    if (g_forgetAll.load(std::memory_order_acquire)) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (const Recording& r : g_recordings) {
        found = found || (r.complete && r.key == key);
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void set_placement(const Placement& placement) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_placement = placement;
    ReleaseSRWLockExclusive(&g_lock);
    g_visible.store(placement.visible && placement.key != 0, std::memory_order_relaxed);
}

bool recorded_hold(std::uintptr_t key, Affine& gunInController, Affine& handInController) noexcept {
    static_cast<void>(handInController);
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (const Recording& r : g_recordings) {
        if (r.complete && r.key == key) {
            gunInController = math::inverse_rigid(r.controller) * r.root;
            found = true;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

bool left_gun(Affine& root, std::uintptr_t* key) noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool ok = g_leftValid && GetTickCount64() - g_leftTick < 250;
    root = g_leftRoot;
    if (key != nullptr) {
        *key = g_leftKey;
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

} // namespace sunrise::client::hooks::vr::offhand
