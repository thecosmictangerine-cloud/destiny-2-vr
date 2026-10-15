/**
 * The GPU side of the arms: D3D11 detours, shader reflection, and a whole-frame capture.
 *
 * The first-person arms are skinned on the GPU, so whatever poses them reaches the device as a
 * buffer of bone matrices bound to a draw. Rewriting that buffer is how the sister project gave
 * Dishonored 2 arms that follow the controllers (docs/ARMS.md), and nothing about it depends on the
 * engine: d3d11.dll is not VMProtect's business, and a detour there raises no exception.
 *
 * This file is the reconnaissance half. It answers, from the live game, the questions the rewrite
 * depends on:
 *
 *   - which draws are the arms, and on which context they are recorded (deferred contexts have a
 *     vtable of their own, so both are detoured);
 *   - which vertex shader skins them, and where its bones come from -- read out of the shader's own
 *     reflection, captured when the shader is CREATED, because a shader object cannot give its
 *     bytecode back afterwards;
 *   - what the palette looks like in memory: the buffer bound at that slot, its contents, its stride;
 *   - and, decisively, which draws disappear when a rule hides them, so an identification is
 *     confirmed by a screenshot instead of argued from numbers.
 *
 * Commands arrive in `SVR_Gpu.txt` in the game directory, one per line, read every 30 presents:
 *
 *   capture                one frame: every draw, its state and the bytes of every buffer it reads,
 *                          into SVR_GpuCapture.txt / .bin; analysed by scripts/testing/gpu_capture.py
 *   hide vs|ps|srv|cb|vb|ib <hex pointer>   skip every draw that binds it (up to 8 rules per kind)
 *   hide chk <8 hex>       skip every draw whose vertex shader's DXBC checksum starts with this
 *   hide none              clear every rule
 *   stats                  draw counts per context type, into the log
 */

#include "vr_gpu.h"

#include <Windows.h>

#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "runtime.h"
#include "vr_arms.h"
#include "vr_d3d_slots.h"

namespace sunrise::client::hooks::vr::gpu {
namespace {

constexpr wchar_t kCommandName[] = L"SVR_Gpu.txt";
constexpr wchar_t kCaptureText[] = L"SVR_GpuCapture.txt";
constexpr wchar_t kCaptureBlob[] = L"SVR_GpuCapture.bin";
constexpr std::uint32_t kPollPeriod = 30;
constexpr std::uint32_t kStatsPeriod = 600;
/** Buffers up to this size are shadowed whole on every Unmap while a capture runs. */
constexpr UINT kShadowMax = 256U * 1024U;
/** Buffers up to this size are read back through a staging copy at the end of a capture. */
constexpr UINT kReadbackMax = 4U * 1024U * 1024U;
constexpr std::size_t kMaxHide = 8;
constexpr UINT kVsCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
/** VS resource slots looked at; the full 128 would make every record enormous for nothing. */
constexpr UINT kVsSrvSlots = 32;

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

/** Formats into a std::string, for capture records that outgrow a log line. */
template <typename... Args> void appendf(std::string& out, const char* format, Args... args) {
    std::array<char, 512> chunk{};
    const int written = std::snprintf(chunk.data(), chunk.size(), format, args...);
    if (written > 0) {
        out.append(chunk.data(), static_cast<std::size_t>(written) < chunk.size() ? static_cast<std::size_t>(written)
                                                                                : chunk.size() - 1);
    }
}

bool game_path(const wchar_t* name, std::array<wchar_t, MAX_PATH>& out) noexcept {
    const DWORD length = GetModuleFileNameW(nullptr, out.data(), static_cast<DWORD>(out.size()));
    if (length == 0 || length >= out.size()) {
        return false;
    }
    wchar_t* const slash = std::wcsrchr(out.data(), L'\\');
    if (slash == nullptr) {
        return false;
    }
    const std::size_t dirLength = static_cast<std::size_t>(slash + 1 - out.data());
    return wcscpy_s(slash + 1, out.size() - dirLength, name) == 0;
}

// ---------------------------------------------------------------------------------------------
// Shader and input layout registries
// ---------------------------------------------------------------------------------------------

/** What the reflection of one shader says, kept compact. */
struct ShaderInfo final {
    /** First 32 bits of the DXBC checksum: stable across launches, unlike the object pointer. */
    std::uint32_t checksum{};
    std::string summary;
};

/** Where the skinning attributes sit in vertex stream 0, resolved from the input layout. */
struct LayoutInfo final {
    int position{-1};
    DXGI_FORMAT positionFormat{DXGI_FORMAT_UNKNOWN};
    int weights{-1};
    DXGI_FORMAT weightsFormat{DXGI_FORMAT_UNKNOWN};
    int indices{-1};
    DXGI_FORMAT indicesFormat{DXGI_FORMAT_UNKNOWN};
};

SRWLOCK g_registryLock = SRWLOCK_INIT;
std::unordered_map<const void*, ShaderInfo> g_shaders;
std::unordered_map<const void*, std::string> g_layouts;
std::unordered_map<const void*, LayoutInfo> g_layoutInfo;
std::atomic<std::uint64_t> g_shadersSeen{0};
std::atomic<std::uint64_t> g_shadersNamed{0};

const char* input_type_name(D3D_SHADER_INPUT_TYPE type) noexcept {
    switch (type) {
    case D3D_SIT_CBUFFER: return "cb";
    case D3D_SIT_TBUFFER: return "tb";
    case D3D_SIT_TEXTURE: return "tex";
    case D3D_SIT_SAMPLER: return "smp";
    case D3D_SIT_STRUCTURED: return "sbuf";
    case D3D_SIT_BYTEADDRESS: return "raw";
    case D3D_SIT_UAV_RWTYPED: return "uav";
    case D3D_SIT_UAV_RWSTRUCTURED: return "uavs";
    case D3D_SIT_UAV_RWBYTEADDRESS: return "uavr";
    default: return "other";
    }
}

const char* srv_dimension_name(D3D_SRV_DIMENSION dimension) noexcept {
    switch (dimension) {
    case D3D_SRV_DIMENSION_BUFFER: return "buf";
    case D3D_SRV_DIMENSION_BUFFEREX: return "bufex";
    case D3D_SRV_DIMENSION_TEXTURE1D: return "t1d";
    case D3D_SRV_DIMENSION_TEXTURE2D: return "t2d";
    case D3D_SRV_DIMENSION_TEXTURE2DARRAY: return "t2da";
    case D3D_SRV_DIMENSION_TEXTURE3D: return "t3d";
    case D3D_SRV_DIMENSION_TEXTURECUBE: return "cube";
    default: return "-";
    }
}

/**
 * Reflects a shader's bytecode into one line: inputs, then every bound resource with its slot,
 * type and dimension, then each constant buffer's size and the first few variable names.
 *
 * Names are recorded because they may not be stripped -- in Dishonored they were not, and
 * `ro_matrices` found the palette in one step. If they are stripped, the structure alone still
 * marks a skinning shader: a vertex input carrying bone indices and a buffer resource to index.
 */
ShaderInfo reflect(const void* bytecode, SIZE_T length) {
    ShaderInfo info{};
    if (bytecode == nullptr || length < 20) {
        return info;
    }
    std::memcpy(&info.checksum, static_cast<const std::uint8_t*>(bytecode) + 4, sizeof info.checksum);
    ID3D11ShaderReflection* reflection = nullptr;
    if (FAILED(D3DReflect(bytecode, length, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&reflection)))
        || reflection == nullptr) {
        info.summary = "reflect=fail";
        return info;
    }
    D3D11_SHADER_DESC desc{};
    if (SUCCEEDED(reflection->GetDesc(&desc))) {
        appendf(info.summary, "ins=%u", desc.InstructionCount);
        info.summary += " in=";
        for (UINT i = 0; i < desc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC parameter{};
            if (SUCCEEDED(reflection->GetInputParameterDesc(i, &parameter))) {
                appendf(info.summary, "%s%s%u:%X", i == 0 ? "" : ",",
                        parameter.SemanticName != nullptr ? parameter.SemanticName : "?", parameter.SemanticIndex,
                        static_cast<unsigned>(parameter.Mask));
            }
        }
        info.summary += " res=";
        bool named = false;
        for (UINT i = 0; i < desc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bind{};
            if (FAILED(reflection->GetResourceBindingDesc(i, &bind))) {
                continue;
            }
            appendf(info.summary, "%s%s%u", i == 0 ? "" : ",", input_type_name(bind.Type), bind.BindPoint);
            if (bind.BindCount > 1) {
                appendf(info.summary, "x%u", bind.BindCount);
            }
            if (bind.Type == D3D_SIT_TEXTURE || bind.Type == D3D_SIT_STRUCTURED || bind.Type == D3D_SIT_BYTEADDRESS) {
                appendf(info.summary, ":%s", srv_dimension_name(bind.Dimension));
                if (bind.Type == D3D_SIT_STRUCTURED) {
                    appendf(info.summary, ":stride%u", bind.NumSamples);
                }
            }
            if (bind.Name != nullptr && bind.Name[0] != '\0') {
                appendf(info.summary, "'%s'", bind.Name);
                named = true;
            }
            if (bind.Type == D3D_SIT_CBUFFER) {
                ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByName(bind.Name);
                D3D11_SHADER_BUFFER_DESC cbDesc{};
                if (cb != nullptr && SUCCEEDED(cb->GetDesc(&cbDesc))) {
                    appendf(info.summary, ":sz%u:v%u", cbDesc.Size, cbDesc.Variables);
                    for (UINT v = 0; v < cbDesc.Variables && v < 6; ++v) {
                        ID3D11ShaderReflectionVariable* variable = cb->GetVariableByIndex(v);
                        D3D11_SHADER_VARIABLE_DESC variableDesc{};
                        if (variable != nullptr && SUCCEEDED(variable->GetDesc(&variableDesc))
                            && variableDesc.Name != nullptr) {
                            appendf(info.summary, "[%s@%u+%u]", variableDesc.Name, variableDesc.StartOffset,
                                    variableDesc.Size);
                        }
                    }
                }
            }
        }
        if (named) {
            g_shadersNamed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    reflection->Release();
    return info;
}

void remember_shader(const void* shader, const void* bytecode, SIZE_T length, const char* stage) {
    if (shader == nullptr) {
        return;
    }
    ShaderInfo info = reflect(bytecode, length);
    info.summary.insert(0, stage);
    g_shadersSeen.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_registryLock);
    g_shaders[shader] = std::move(info);
    ReleaseSRWLockExclusive(&g_registryLock);
}

bool shader_info(const void* shader, ShaderInfo& out) {
    AcquireSRWLockShared(&g_registryLock);
    const auto found = g_shaders.find(shader);
    const bool ok = found != g_shaders.end();
    if (ok) {
        out = found->second;
    }
    ReleaseSRWLockShared(&g_registryLock);
    return ok;
}

std::uint32_t shader_checksum(const void* shader) {
    AcquireSRWLockShared(&g_registryLock);
    const auto found = g_shaders.find(shader);
    const std::uint32_t checksum = found != g_shaders.end() ? found->second.checksum : 0;
    ReleaseSRWLockShared(&g_registryLock);
    return checksum;
}

// ---------------------------------------------------------------------------------------------
// Capture state
// ---------------------------------------------------------------------------------------------

ID3D11DeviceContext* g_immediate = nullptr;

/** Idle, then one frame of shadowing only (so the frame's buffer writes are seen), then recording. */
enum class Phase : int { idle = 0, armed = 1, recording = 2 };
std::atomic<int> g_phase{static_cast<int>(Phase::idle)};

struct Shadow final {
    std::uint64_t generation{};
    std::vector<std::uint8_t> bytes;
};

SRWLOCK g_captureLock = SRWLOCK_INIT;
std::unordered_map<const void*, Shadow> g_shadows;
std::uint64_t g_shadowGeneration = 0;
std::vector<std::string> g_records;
struct BlobKey final {
    const void* resource;
    std::uint64_t generation;
    UINT offset;
    UINT length;
    bool operator==(const BlobKey& other) const noexcept {
        return resource == other.resource && generation == other.generation && offset == other.offset
               && length == other.length;
    }
};
struct BlobKeyHash final {
    std::size_t operator()(const BlobKey& key) const noexcept {
        return std::hash<const void*>{}(key.resource) ^ (key.generation * 0x9E3779B97F4A7C15ULL)
               ^ (static_cast<std::size_t>(key.offset) << 20) ^ key.length;
    }
};
std::unordered_map<BlobKey, std::uint32_t, BlobKeyHash> g_blobIds;
std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> g_blobs;
/** Buffers read by a recorded draw with no shadow: read back at the end through staging. */
std::unordered_map<ID3D11Buffer*, std::vector<std::uint32_t>> g_pendingReadback;
std::unordered_set<const void*> g_referencedShaders;
std::uint32_t g_drawIndex = 0;

/** Per-thread record of the pointer a Map handed out, so Unmap can read the bytes before they go. */
struct MapSlot final {
    ID3D11Resource* resource{};
    UINT subresource{};
    void* data{};
};
thread_local std::array<MapSlot, 16> t_maps{};
/** Set while this module itself calls into the context, so its own traffic is not recorded. */
thread_local bool t_inside = false;

// ---------------------------------------------------------------------------------------------
// Hide rules
// ---------------------------------------------------------------------------------------------

enum HideKind : std::size_t { hideVs, hidePs, hideSrv, hideCb, hideVb, hideIb, hideChk, hideKinds };
std::array<std::array<std::atomic<std::uintptr_t>, kMaxHide>, hideKinds> g_hide{};
std::array<std::atomic<std::size_t>, hideKinds> g_hideCount{};
std::atomic_bool g_anyHide{false};
std::atomic<std::uint64_t> g_hidden{0};

bool hide_matches(HideKind kind, std::uintptr_t value) noexcept {
    const std::size_t count = g_hideCount[kind].load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count && i < kMaxHide; ++i) {
        if (g_hide[kind][i].load(std::memory_order_relaxed) == value) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------------------------

std::atomic<std::uint64_t> g_drawsImmediate{0};
std::atomic<std::uint64_t> g_drawsDeferred{0};
std::atomic<std::uint64_t> g_dispatches{0};
std::atomic<std::uint64_t> g_maps{0};
std::atomic<std::uint64_t> g_updates{0};
std::uint32_t g_presents = 0;

// ---------------------------------------------------------------------------------------------
// Recording one draw
// ---------------------------------------------------------------------------------------------

UINT format_bytes(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT: return 12;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: return 8;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UINT: return 2;
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_UNORM: return 1;
    default: return 0;
    }
}

/**
 * Takes the bytes of `resource` in [offset, offset+length) into the capture, from its shadow when
 * the frame wrote it, or queues a staging readback for the end of the capture when it did not.
 * Caller holds g_captureLock exclusively.
 * @return The blob id, which the record line refers to.
 */
std::uint32_t take_blob(ID3D11Buffer* buffer, UINT byteWidth, UINT offset, UINT length) {
    if (offset >= byteWidth) {
        return 0;
    }
    if (length > byteWidth - offset) {
        length = byteWidth - offset;
    }
    const auto shadow = g_shadows.find(buffer);
    const std::uint64_t generation = shadow != g_shadows.end() ? shadow->second.generation : 0;
    const BlobKey key{buffer, generation, offset, length};
    const auto known = g_blobIds.find(key);
    if (known != g_blobIds.end()) {
        return known->second;
    }
    const auto id = static_cast<std::uint32_t>(g_blobs.size() + 1);
    g_blobIds.emplace(key, id);
    if (shadow != g_shadows.end() && shadow->second.bytes.size() >= static_cast<std::size_t>(offset) + length) {
        g_blobs.emplace_back(id, std::vector<std::uint8_t>(shadow->second.bytes.begin() + offset,
                                                           shadow->second.bytes.begin() + offset + length));
    } else {
        // Encoded as (offset, length) in the first 8 bytes until the readback fills it in.
        std::vector<std::uint8_t> placeholder(8);
        std::memcpy(placeholder.data(), &offset, 4);
        std::memcpy(placeholder.data() + 4, &length, 4);
        g_blobs.emplace_back(id, std::move(placeholder));
        if (byteWidth <= kReadbackMax) {
            auto& pending = g_pendingReadback[buffer];
            if (pending.empty()) {
                buffer->AddRef();
            }
            pending.push_back(id);
        }
    }
    return id;
}

const char* draw_kind_name(int kind) noexcept {
    switch (kind) {
    case 0: return "di";
    case 1: return "d";
    case 2: return "dii";
    case 3: return "dn";
    case 4: return "diii";
    case 5: return "dni";
    default: return "?";
    }
}

/**
 * Builds one capture line for the draw about to happen on `context`. Everything is read back from
 * the context itself (Get* calls), so no state has to be tracked through the Set* methods.
 */
void record_draw(ID3D11DeviceContext* context, int kind, UINT count, UINT instances, UINT start, INT base) {
    std::string line;
    line.reserve(1024);
    const bool deferred = context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
    ID3D11VertexShader* vs = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    ID3D11PixelShader* ps = nullptr;
    context->PSGetShader(&ps, nullptr, nullptr);
    ID3D11InputLayout* layout = nullptr;
    context->IAGetInputLayout(&layout);
    ID3D11Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    context->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
    std::array<ID3D11Buffer*, 4> vertexBuffers{};
    std::array<UINT, 4> strides{};
    std::array<UINT, 4> offsets{};
    context->IAGetVertexBuffers(0, 4, vertexBuffers.data(), strides.data(), offsets.data());
    std::array<ID3D11Buffer*, kVsCbSlots> cbs{};
    std::array<UINT, kVsCbSlots> firsts{};
    std::array<UINT, kVsCbSlots> nums{};
    ID3D11DeviceContext1* context1 = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))
        && context1 != nullptr) {
        context1->VSGetConstantBuffers1(0, kVsCbSlots, cbs.data(), firsts.data(), nums.data());
        context1->Release();
    } else {
        context->VSGetConstantBuffers(0, kVsCbSlots, cbs.data());
    }
    std::array<ID3D11ShaderResourceView*, kVsSrvSlots> srvs{};
    context->VSGetShaderResources(0, kVsSrvSlots, srvs.data());
    std::array<ID3D11RenderTargetView*, 2> rtvs{};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(2, rtvs.data(), &dsv);
    UINT viewportCount = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&viewportCount, &viewport);

    AcquireSRWLockExclusive(&g_captureLock);
    appendf(line, "d i=%u c=%p %s t=%lu k=%s n=%u inst=%u si=%u bv=%d vs=%p ps=%p il=%p", g_drawIndex++,
            static_cast<void*>(context), deferred ? "def" : "imm", GetCurrentThreadId(), draw_kind_name(kind), count,
            instances, start, base, static_cast<void*>(vs), static_cast<void*>(ps), static_cast<void*>(layout));
    if (vs != nullptr) {
        g_referencedShaders.insert(vs);
        appendf(line, " vsh=%08X", shader_checksum(vs));
    }
    if (layout != nullptr) {
        g_referencedShaders.insert(layout);
    }
    if (indexBuffer != nullptr) {
        appendf(line, " ib=%p:%d:%u", static_cast<void*>(indexBuffer), static_cast<int>(indexFormat), indexOffset);
    }
    for (std::size_t i = 0; i < vertexBuffers.size(); ++i) {
        if (vertexBuffers[i] != nullptr) {
            D3D11_BUFFER_DESC desc{};
            vertexBuffers[i]->GetDesc(&desc);
            appendf(line, " vb%zu=%p:%u:%u:%u:u%d", i, static_cast<void*>(vertexBuffers[i]), strides[i], offsets[i],
                    desc.ByteWidth, static_cast<int>(desc.Usage));
        }
    }
    for (UINT slot = 0; slot < kVsCbSlots; ++slot) {
        if (cbs[slot] == nullptr) {
            continue;
        }
        D3D11_BUFFER_DESC desc{};
        cbs[slot]->GetDesc(&desc);
        // VSSetConstantBuffers (no offsets) reports first 0 and num 4096: clip to the buffer.
        const UINT offset = firsts[slot] * 16U;
        const UINT length = nums[slot] != 0 ? nums[slot] * 16U : desc.ByteWidth;
        const std::uint32_t blob = take_blob(cbs[slot], desc.ByteWidth, offset, length);
        appendf(line, " cb%u=%p:%u:%u:%u:u%d:b%u", slot, static_cast<void*>(cbs[slot]), firsts[slot], nums[slot],
                desc.ByteWidth, static_cast<int>(desc.Usage), blob);
    }
    for (UINT slot = 0; slot < kVsSrvSlots; ++slot) {
        if (srvs[slot] == nullptr) {
            continue;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        srvs[slot]->GetDesc(&view);
        ID3D11Resource* resource = nullptr;
        srvs[slot]->GetResource(&resource);
        appendf(line, " t%u=%p:%s:f%d", slot, static_cast<void*>(resource),
                srv_dimension_name(static_cast<D3D_SRV_DIMENSION>(view.ViewDimension)), static_cast<int>(view.Format));
        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (resource != nullptr) {
            resource->GetType(&dimension);
        }
        if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
            auto* buffer = static_cast<ID3D11Buffer*>(resource);
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            UINT first = 0;
            UINT elements = 0;
            if (view.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
                first = view.Buffer.FirstElement;
                elements = view.Buffer.NumElements;
            } else if (view.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
                first = view.BufferEx.FirstElement;
                elements = view.BufferEx.NumElements;
            }
            UINT element = desc.StructureByteStride != 0 ? desc.StructureByteStride : format_bytes(view.Format);
            if (element == 0) {
                element = 4;
            }
            const std::uint32_t blob = take_blob(buffer, desc.ByteWidth, first * element, elements * element);
            appendf(line, ":%u:%u:e%u:%u:u%d:m%X:b%u", first, elements, element, desc.ByteWidth,
                    static_cast<int>(desc.Usage), desc.MiscFlags, blob);
        } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC desc{};
            static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
            appendf(line, ":%ux%u:a%u", desc.Width, desc.Height, desc.ArraySize);
        }
        if (resource != nullptr) {
            resource->Release();
        }
    }
    if (rtvs[0] != nullptr) {
        ID3D11Resource* target = nullptr;
        rtvs[0]->GetResource(&target);
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        rtvs[0]->GetDesc(&view);
        appendf(line, " rt=%p:f%d", static_cast<void*>(target), static_cast<int>(view.Format));
        if (target != nullptr) {
            target->Release();
        }
    }
    if (rtvs[1] != nullptr) {
        appendf(line, " rt1=1");
    }
    if (dsv != nullptr) {
        ID3D11Resource* depth = nullptr;
        dsv->GetResource(&depth);
        appendf(line, " ds=%p", static_cast<void*>(depth));
        if (depth != nullptr) {
            depth->Release();
        }
    }
    if (viewportCount > 0) {
        appendf(line, " vp=%.0f,%.0f,%.0f,%.0f,%.3f,%.3f", viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                viewport.Height, viewport.MinDepth, viewport.MaxDepth);
    }
    g_records.push_back(std::move(line));
    ReleaseSRWLockExclusive(&g_captureLock);

    auto release = [](auto* object) {
        if (object != nullptr) {
            object->Release();
        }
    };
    release(vs);
    release(ps);
    release(layout);
    release(indexBuffer);
    for (auto* b : vertexBuffers) {
        release(b);
    }
    for (auto* b : cbs) {
        release(b);
    }
    for (auto* v : srvs) {
        release(v);
    }
    for (auto* v : rtvs) {
        release(v);
    }
    release(dsv);
}

void record_dispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z) {
    ID3D11ComputeShader* cs = nullptr;
    context->CSGetShader(&cs, nullptr, nullptr);
    std::array<ID3D11ShaderResourceView*, 16> srvs{};
    context->CSGetShaderResources(0, 16, srvs.data());
    std::array<ID3D11UnorderedAccessView*, 8> uavs{};
    context->CSGetUnorderedAccessViews(0, 8, uavs.data());
    std::string line;
    AcquireSRWLockExclusive(&g_captureLock);
    appendf(line, "x i=%u c=%p t=%lu g=%u,%u,%u cs=%p", g_drawIndex++, static_cast<void*>(context),
            GetCurrentThreadId(), x, y, z, static_cast<void*>(cs));
    if (cs != nullptr) {
        g_referencedShaders.insert(cs);
        appendf(line, " csh=%08X", shader_checksum(cs));
    }
    for (std::size_t i = 0; i < srvs.size(); ++i) {
        if (srvs[i] != nullptr) {
            ID3D11Resource* resource = nullptr;
            srvs[i]->GetResource(&resource);
            appendf(line, " t%zu=%p", i, static_cast<void*>(resource));
            if (resource != nullptr) {
                resource->Release();
            }
        }
    }
    for (std::size_t i = 0; i < uavs.size(); ++i) {
        if (uavs[i] != nullptr) {
            ID3D11Resource* resource = nullptr;
            uavs[i]->GetResource(&resource);
            appendf(line, " u%zu=%p", i, static_cast<void*>(resource));
            if (resource != nullptr) {
                resource->Release();
            }
        }
    }
    g_records.push_back(std::move(line));
    ReleaseSRWLockExclusive(&g_captureLock);
    if (cs != nullptr) {
        cs->Release();
    }
    for (auto* v : srvs) {
        if (v != nullptr) {
            v->Release();
        }
    }
    for (auto* v : uavs) {
        if (v != nullptr) {
            v->Release();
        }
    }
}

/**
 * Evaluates the hide rules against the state bound for this draw. Reads back only what the
 * active rules need, so a session with one `hide vs` rule pays for one VSGetShader per draw.
 */
bool should_hide(ID3D11DeviceContext* context) noexcept {
    if (g_hideCount[hideVs].load(std::memory_order_relaxed) != 0
        || g_hideCount[hideChk].load(std::memory_order_relaxed) != 0) {
        ID3D11VertexShader* vs = nullptr;
        context->VSGetShader(&vs, nullptr, nullptr);
        bool hit = false;
        if (vs != nullptr) {
            hit = hide_matches(hideVs, reinterpret_cast<std::uintptr_t>(vs));
            if (!hit && g_hideCount[hideChk].load(std::memory_order_relaxed) != 0) {
                hit = hide_matches(hideChk, shader_checksum(vs));
            }
            vs->Release();
        }
        if (hit) {
            return true;
        }
    }
    if (g_hideCount[hidePs].load(std::memory_order_relaxed) != 0) {
        ID3D11PixelShader* ps = nullptr;
        context->PSGetShader(&ps, nullptr, nullptr);
        const bool hit = ps != nullptr && hide_matches(hidePs, reinterpret_cast<std::uintptr_t>(ps));
        if (ps != nullptr) {
            ps->Release();
        }
        if (hit) {
            return true;
        }
    }
    if (g_hideCount[hideSrv].load(std::memory_order_relaxed) != 0) {
        std::array<ID3D11ShaderResourceView*, kVsSrvSlots> srvs{};
        context->VSGetShaderResources(0, kVsSrvSlots, srvs.data());
        bool hit = false;
        for (auto* view : srvs) {
            if (view == nullptr) {
                continue;
            }
            if (!hit) {
                ID3D11Resource* resource = nullptr;
                view->GetResource(&resource);
                hit = resource != nullptr && hide_matches(hideSrv, reinterpret_cast<std::uintptr_t>(resource));
                if (resource != nullptr) {
                    resource->Release();
                }
            }
            view->Release();
        }
        if (hit) {
            return true;
        }
    }
    if (g_hideCount[hideCb].load(std::memory_order_relaxed) != 0) {
        std::array<ID3D11Buffer*, kVsCbSlots> cbs{};
        context->VSGetConstantBuffers(0, kVsCbSlots, cbs.data());
        bool hit = false;
        for (auto* cb : cbs) {
            if (cb != nullptr) {
                hit = hit || hide_matches(hideCb, reinterpret_cast<std::uintptr_t>(cb));
                cb->Release();
            }
        }
        if (hit) {
            return true;
        }
    }
    if (g_hideCount[hideVb].load(std::memory_order_relaxed) != 0) {
        std::array<ID3D11Buffer*, 4> vbs{};
        std::array<UINT, 4> strides{};
        std::array<UINT, 4> offsets{};
        context->IAGetVertexBuffers(0, 4, vbs.data(), strides.data(), offsets.data());
        bool hit = false;
        for (auto* vb : vbs) {
            if (vb != nullptr) {
                hit = hit || hide_matches(hideVb, reinterpret_cast<std::uintptr_t>(vb));
                vb->Release();
            }
        }
        if (hit) {
            return true;
        }
    }
    if (g_hideCount[hideIb].load(std::memory_order_relaxed) != 0) {
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT format{};
        UINT offset = 0;
        context->IAGetIndexBuffer(&ib, &format, &offset);
        const bool hit = ib != nullptr && hide_matches(hideIb, reinterpret_cast<std::uintptr_t>(ib));
        if (ib != nullptr) {
            ib->Release();
        }
        if (hit) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// The rewrite: arm palettes (cb11) and the viewmodel's projection (cb12)
// ---------------------------------------------------------------------------------------------
//
// Both constant buffers are DYNAMIC and re-Mapped per object (cb11) or per view (cb12): one
// resource, new contents each time. So the contents are classified as they are Unmapped, and a
// draw that binds the resource while its latest contents are an arm palette or a viewmodel view
// gets our rewritten copy bound in its place, then the game's buffer back.

constexpr std::size_t kHeaderRows = 8;
constexpr std::size_t kMaxPaletteBones = 128;
/** The arm skeleton's meshes carry 52 or 72 bones; the weapon's palette has 8. */
constexpr std::size_t kMinArmBones = 40;
constexpr UINT kPaletteBytes = 65536;
constexpr UINT kViewBytes = 224;
constexpr std::size_t kViewFloats = kViewBytes / 4;

/** What the slot-11 resource last received, when that was an arm palette. */
struct PaletteShadow final {
    ID3D11Resource* resource{};
    bool valid{};
    std::size_t bones{};
    std::array<float, (kHeaderRows + 3 * kMaxPaletteBones) * 4> rows{};
};

/** What the slot-12 resource last received, when that was the viewmodel's view. */
struct ViewShadow final {
    ID3D11Resource* resource{};
    bool valid{};
    std::array<float, kViewFloats> rows{};
};

/** One arm mesh waiting for its vertex buffer to be read back on the present thread. */
struct MeshRequest final {
    ID3D11Buffer* buffer{};
    UINT stride{};
    UINT offset{};
    LayoutInfo layout{};
    float scale{};
    std::array<float, 3> origin{};
};

enum class BufferKind : std::uint8_t { other, palette, view };

std::atomic_bool g_rewriteOn{true};
std::atomic_bool g_fovFix{true};
PaletteShadow g_palette{};
ViewShadow g_vmView{};
/** The main world camera's projection scales, the target of the viewmodel FOV fix. */
std::atomic<float> g_worldP00{0.0F};
std::atomic<float> g_worldP11{0.0F};
/** Last viewmodel camera, for the overlay; written on the render thread. */
SRWLOCK g_cameraLock = SRWLOCK_INIT;
ViewCamera g_vmCamera{};
bool g_vmCameraValid = false;
/** Our replacement constant buffers, created on the present thread. */
ID3D11Buffer* g_ourPalette = nullptr;
ID3D11Buffer* g_ourView = nullptr;
/** Arm meshes by vertex buffer: decoded (true) or requested (false). */
std::unordered_map<const void*, bool> g_armMeshes;
std::vector<MeshRequest> g_meshQueue;
SRWLOCK g_meshLock = SRWLOCK_INIT;
std::atomic<std::uint64_t> g_paletteSwaps{0};
std::atomic<std::uint64_t> g_viewSwaps{0};
std::atomic<std::uint64_t> g_armPalettesSeen{0};
/** Per-thread classification cache: resource -> kind, so Map/Unmap need not GetDesc every time. */
thread_local std::unordered_map<const void*, BufferKind> t_kinds;
/**
 * Mapped pointers of the buffers being watched, shared by every thread: the game may Map on one
 * thread and Unmap on another, which a per-thread table silently misses.
 */
SRWLOCK g_rewriteMapLock = SRWLOCK_INIT;
std::array<MapSlot, 16> g_rewriteMaps{};
std::atomic<std::uint64_t> g_viewsExamined{0};
std::atomic<std::uint64_t> g_vmViewsSeen{0};
std::atomic<std::uint64_t> g_palettesExamined{0};

bool rewrite_wanted() noexcept {
    return g_rewriteOn.load(std::memory_order_relaxed) && enabled();
}

BufferKind classify(ID3D11Resource* resource) noexcept {
    if (resource == nullptr) {
        return BufferKind::other;
    }
    const auto found = t_kinds.find(resource);
    if (found != t_kinds.end()) {
        return found->second;
    }
    BufferKind kind = BufferKind::other;
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC desc{};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 && desc.Usage == D3D11_USAGE_DYNAMIC) {
            kind = desc.ByteWidth == kPaletteBytes ? BufferKind::palette
                   : desc.ByteWidth == kViewBytes  ? BufferKind::view
                                                   : BufferKind::other;
        }
    }
    if (t_kinds.size() > 4096) {
        t_kinds.clear(); // resources come and go; a bounded cache never holds a stale pointer for long
    }
    t_kinds.emplace(resource, kind);
    return kind;
}

/** @return True when the three float4 rows starting at `r` hold an orthonormal 3x3. */
bool rigid_rows(const float* r) noexcept {
    const float c0[3] = {r[0], r[4], r[8]};
    const float c1[3] = {r[1], r[5], r[9]};
    const float c2[3] = {r[2], r[6], r[10]};
    auto d = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    return std::fabs(d(c0, c0) - 1.0F) < 0.05F && std::fabs(d(c1, c1) - 1.0F) < 0.05F
           && std::fabs(d(c2, c2) - 1.0F) < 0.05F && std::fabs(d(c0, c1)) < 0.05F && std::fabs(d(c0, c2)) < 0.05F
           && std::fabs(d(c1, c2)) < 0.05F;
}

bool patched_view(std::array<float, kViewFloats>& out) noexcept;
void publish_camera(const std::array<float, kViewFloats>& rows) noexcept;

/**
 * Looks at a palette as it is Unmapped. An arm palette is recognised by its root: camera-relative
 * and straight below the eye, at the feet (the viewmodel skeleton hangs from the camera; anything
 * in the world sits hundreds of metres from it, and the weapon's root is beside the hand).
 */
void examine_palette(ID3D11Resource* resource, const float* data) noexcept {
    g_palettesExamined.fetch_add(1, std::memory_order_relaxed);
    const float scale = data[16];
    const bool header = scale > 0.0F && std::fabs(data[17] - scale) < 1e-4F && std::fabs(data[18] - scale) < 1e-4F
                        && data[19] == 0.0F;
    const float* bone0 = data + kHeaderRows * 4;
    // The root may be anywhere near the camera: once the weapon module places the viewmodel on the
    // controller it carries the whole skeleton with it. What stays true is camera-relative
    // (metres, not the hundreds a world object sits at) and a skeleton far bigger than a gun's.
    const bool nearCamera = header && std::fabs(bone0[3]) < 3.0F && std::fabs(bone0[7]) < 3.0F && std::fabs(bone0[11]) < 3.0F
                      && rigid_rows(bone0);
    std::size_t bones = 0;
    if (nearCamera) {
        while (bones < kMaxPaletteBones && rigid_rows(bone0 + bones * 12)) {
            ++bones;
        }
    }
    const bool arm = nearCamera && bones >= kMinArmBones;
    if (!arm) {
        if (resource == g_palette.resource) {
            g_palette.valid = false;
        }
        return;
    }
    const std::size_t floats = (kHeaderRows + 3 * bones) * 4;
    std::memcpy(g_palette.rows.data(), data, floats * sizeof(float));
    g_palette.resource = resource;
    g_palette.bones = bones;
    g_palette.valid = true;
    g_armPalettesSeen.fetch_add(1, std::memory_order_relaxed);
    arms::observe(g_palette.rows.data() + kHeaderRows * 4, bones);
    // Rewritten IN PLACE, in the memory the game just filled and is about to hand to the GPU.
    // Whatever draw then binds it -- G-buffer, depth prepass, shadow, any pass -- gets the arms as
    // posed here, with no binding to track and nothing to restore.
    if (arms::ready()) {
        static thread_local std::array<float, 3 * 4 * kMaxPaletteBones> s_out{};
        if (arms::rewrite(g_palette.rows.data() + kHeaderRows * 4, bones, s_out.data())) {
            std::memcpy(const_cast<float*>(bone0), s_out.data(), 3 * 4 * bones * sizeof(float));
            g_paletteSwaps.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void examine_view(ID3D11Resource* resource, const float* data) noexcept {
    g_viewsExamined.fetch_add(1, std::memory_order_relaxed);
    std::array<float, kViewFloats> rows{};
    std::memcpy(rows.data(), data, sizeof rows);
    const bool cameraAtOrigin = std::fabs(rows[28]) < 1e-3F && std::fabs(rows[29]) < 1e-3F && std::fabs(rows[30]) < 1e-3F;
    const float p00 = rows[40];
    const float p11 = rows[45];
    // The viewmodel compresses its depth range: P22 is non-zero, where the world's reversed
    // infinite projection has 0.
    const bool viewmodel = cameraAtOrigin && rows[50] != 0.0F && p00 > 0.2F && p00 < 5.0F && p11 > 0.2F;
    // The main camera: perspective (row 12 is 0 0 0 -1), at a real position, with the back
    // buffer's aspect -- which rules out the shadow cascades (orthographic) and the square probes.
    const bool world = !cameraAtOrigin && std::fabs(rows[51] + 1.0F) < 1e-3F && std::fabs(rows[48]) < 1e-3F
                       && std::fabs(rows[49]) < 1e-3F && std::fabs(rows[50]) < 1e-4F && p00 > 0.2F && p00 < 5.0F
                       && p11 / p00 > 1.2F && p11 / p00 < 2.6F;
    if (world) {
        g_worldP00.store(p00, std::memory_order_relaxed);
        g_worldP11.store(p11, std::memory_order_relaxed);
    }
    if (viewmodel) {
        g_vmViewsSeen.fetch_add(1, std::memory_order_relaxed);
        g_vmView.resource = resource;
        g_vmView.rows = rows;
        g_vmView.valid = true;
        // The FOV fix, in place: the viewmodel's projection widened to the world's in the very
        // memory the game is about to Unmap.
        std::array<float, kViewFloats> patched{};
        if (g_fovFix.load(std::memory_order_relaxed) && patched_view(patched)) {
            std::memcpy(const_cast<float*>(data), patched.data(), sizeof patched);
            publish_camera(patched);
            g_viewSwaps.fetch_add(1, std::memory_order_relaxed);
        } else {
            publish_camera(rows);
        }
    } else if (resource == g_vmView.resource) {
        g_vmView.valid = false;
    }
}

/** Fills `out` with the viewmodel view, its projection widened to the world's. */
bool patched_view(std::array<float, kViewFloats>& out) noexcept {
    const float wx = g_worldP00.load(std::memory_order_relaxed);
    const float wy = g_worldP11.load(std::memory_order_relaxed);
    out = g_vmView.rows;
    const float vx = out[40];
    const float vy = out[45];
    if (wx <= 0.0F || wy <= 0.0F || vx <= 0.0F || vy <= 0.0F) {
        return false;
    }
    const float sx = wx / vx;
    const float sy = wy / vy;
    for (std::size_t r = 0; r < 4; ++r) {
        out[r * 4 + 0] *= sx; // rows 0-3 are the transposed view-projection: column 0 is clip x
        out[r * 4 + 1] *= sy;
    }
    out[40] *= sx;
    out[45] *= sy;
    return true;
}

void publish_camera(const std::array<float, kViewFloats>& rows) noexcept {
    ViewCamera camera{};
    // Rows 4-6 are the view matrix's rows: right, up, and BACK (a right-handed view looks down -Z).
    camera.forward = {-rows[24], -rows[25], -rows[26]};
    camera.up = {rows[20], rows[21], rows[22]};
    camera.horizontalFov = 2.0F * std::atan(1.0F / rows[40]);
    camera.aspect = rows[45] / rows[40];
    AcquireSRWLockExclusive(&g_cameraLock);
    g_vmCamera = camera;
    g_vmCameraValid = true;
    ReleaseSRWLockExclusive(&g_cameraLock);
}

/** Writes `bytes` into one of our dynamic buffers. Runs inside a draw hook, on the game's context. */
bool upload(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, std::size_t bytes) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (buffer == nullptr || FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, data, bytes);
    context->Unmap(buffer, 0);
    return true;
}

/** What a draw changed, so the hook can put the game's own state back after it. */
struct DrawAction final {
    bool skip{};
    ID3D11Buffer* vs11{};
    ID3D11Buffer* vs12{};
    ID3D11Buffer* ps11{};
    ID3D11Buffer* ps12{};
    bool restore11{};
    bool restore12{};
    bool restorePs11{};
    bool restorePs12{};
};

/** Queues an unknown arm mesh for readback, once. */
void request_mesh(ID3D11DeviceContext* context, ID3D11Buffer* vb, UINT stride, UINT offset) noexcept {
    AcquireSRWLockExclusive(&g_meshLock);
    const bool fresh = g_armMeshes.emplace(vb, false).second;
    ReleaseSRWLockExclusive(&g_meshLock);
    if (!fresh) {
        return;
    }
    MeshRequest request{};
    request.buffer = vb;
    vb->AddRef();
    request.stride = stride;
    request.offset = offset;
    request.scale = g_palette.rows[16];
    request.origin = {g_palette.rows[20], g_palette.rows[21], g_palette.rows[22]};
    ID3D11InputLayout* layout = nullptr;
    context->IAGetInputLayout(&layout);
    if (layout != nullptr) {
        AcquireSRWLockShared(&g_registryLock);
        const auto found = g_layoutInfo.find(layout);
        if (found != g_layoutInfo.end()) {
            request.layout = found->second;
        }
        ReleaseSRWLockShared(&g_registryLock);
        layout->Release();
    }
    AcquireSRWLockExclusive(&g_meshLock);
    g_meshQueue.push_back(request);
    ReleaseSRWLockExclusive(&g_meshLock);
}

/**
 * The draw side of the rewrite, which is now only reconnaissance: the palettes and the view are
 * rewritten in place at Unmap, so all a draw still has to say is WHICH mesh an arm palette is
 * skinning, so its vertex buffer can be read into the rig once.
 */
void prepare_rewrite(ID3D11DeviceContext* context, DrawAction& action) noexcept {
    static_cast<void>(action);
    if (!g_palette.valid || !rewrite_wanted()) {
        return;
    }
    ID3D11Buffer* bound = nullptr;
    context->VSGetConstantBuffers(11, 1, &bound);
    if (bound == nullptr) {
        return;
    }
    const bool paletteHere = bound == g_palette.resource;
    bound->Release();
    if (!paletteHere) {
        return;
    }
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0;
    UINT offset = 0;
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (vb == nullptr) {
        return;
    }
    AcquireSRWLockShared(&g_meshLock);
    const bool seen = g_armMeshes.find(vb) != g_armMeshes.end();
    ReleaseSRWLockShared(&g_meshLock);
    if (!seen) {
        request_mesh(context, vb, stride, offset);
    }
    vb->Release();
}

/** Puts the game's buffers back after a rewritten draw. Holds and drops the references taken. */
void after_draw(ID3D11DeviceContext* context, DrawAction& action) noexcept {
    if (action.restore11) {
        context->VSSetConstantBuffers(11, 1, &action.vs11);
    }
    if (action.restore12) {
        context->VSSetConstantBuffers(12, 1, &action.vs12);
    }
    if (action.restorePs11) {
        context->PSSetConstantBuffers(11, 1, &action.ps11);
    }
    if (action.restorePs12) {
        context->PSSetConstantBuffers(12, 1, &action.ps12);
    }
    for (auto* b : {action.vs11, action.vs12, action.ps11, action.ps12}) {
        if (b != nullptr) {
            b->Release();
        }
    }
}

/**
 * A vertex buffer asked for by `dumpvb`. The pointer from the command is never dereferenced: the
 * buffer is taken (with a reference) from a draw that actually binds it, so a stale pointer can at
 * worst match nothing.
 */
std::atomic<std::uintptr_t> g_dumpWanted{0};
std::atomic<ID3D11Buffer*> g_dumpTaken{nullptr};

void take_dump_buffer(ID3D11DeviceContext* context) noexcept {
    const std::uintptr_t wanted = g_dumpWanted.load(std::memory_order_acquire);
    if (wanted == 0) {
        return;
    }
    std::array<ID3D11Buffer*, 4> vbs{};
    std::array<UINT, 4> strides{};
    std::array<UINT, 4> offsets{};
    context->IAGetVertexBuffers(0, 4, vbs.data(), strides.data(), offsets.data());
    for (auto* vb : vbs) {
        if (vb == nullptr) {
            continue;
        }
        ID3D11Buffer* expected = nullptr;
        if (reinterpret_cast<std::uintptr_t>(vb) == wanted
            && g_dumpTaken.compare_exchange_strong(expected, vb, std::memory_order_acq_rel)) {
            g_dumpWanted.store(0, std::memory_order_release);
            continue; // keeps the reference IAGetVertexBuffers added
        }
        vb->Release();
    }
}

/** Decides what happens to a draw: skipped, recorded, and/or given our rewritten buffers. */
DrawAction on_draw(ID3D11DeviceContext* context, int kind, UINT count, UINT instances, UINT start,
                   INT base) noexcept {
    DrawAction action{};
    if (t_inside || context == nullptr) {
        return action;
    }
    take_dump_buffer(context);
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
        g_drawsDeferred.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_drawsImmediate.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_anyHide.load(std::memory_order_relaxed) && should_hide(context)) {
        g_hidden.fetch_add(1, std::memory_order_relaxed);
        action.skip = true;
        return action;
    }
    t_inside = true;
    if (g_phase.load(std::memory_order_acquire) == static_cast<int>(Phase::recording)) {
        record_draw(context, kind, count, instances, start, base);
    }
    prepare_rewrite(context, action);
    t_inside = false;
    return action;
}

void finish_draw(ID3D11DeviceContext* context, DrawAction& action) noexcept {
    if (action.restore11 || action.restore12 || action.restorePs11 || action.restorePs12) {
        t_inside = true;
        after_draw(context, action);
        t_inside = false;
    }
}

void on_map(ID3D11Resource* resource, UINT subresource, const D3D11_MAPPED_SUBRESOURCE* mapped) noexcept {
    g_maps.fetch_add(1, std::memory_order_relaxed);
    if (t_inside || mapped == nullptr) {
        return;
    }
    if (rewrite_wanted() && subresource == 0 && classify(resource) != BufferKind::other) {
        AcquireSRWLockExclusive(&g_rewriteMapLock);
        MapSlot* free = nullptr;
        bool stored = false;
        for (MapSlot& slot : g_rewriteMaps) {
            if (slot.resource == resource) {
                slot.data = mapped->pData;
                stored = true;
                break;
            }
            if (free == nullptr && slot.resource == nullptr) {
                free = &slot;
            }
        }
        if (!stored && free != nullptr) {
            *free = MapSlot{resource, 0, mapped->pData};
        }
        ReleaseSRWLockExclusive(&g_rewriteMapLock);
    }
    if (g_phase.load(std::memory_order_acquire) == static_cast<int>(Phase::idle)) {
        return;
    }
    for (MapSlot& slot : t_maps) {
        if (slot.resource == nullptr || (slot.resource == resource && slot.subresource == subresource)) {
            slot = MapSlot{resource, subresource, mapped->pData};
            return;
        }
    }
}

void store_shadow(ID3D11Resource* resource, const void* data, UINT bytes) {
    AcquireSRWLockExclusive(&g_captureLock);
    Shadow& shadow = g_shadows[resource];
    shadow.generation = ++g_shadowGeneration;
    shadow.bytes.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + bytes);
    ReleaseSRWLockExclusive(&g_captureLock);
}

/** @return The buffer's width when it is a buffer small enough to shadow, else 0. */
UINT shadowable(ID3D11Resource* resource) noexcept {
    if (resource == nullptr) {
        return 0;
    }
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
        return 0;
    }
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    return desc.ByteWidth <= kShadowMax ? desc.ByteWidth : 0;
}

void on_unmap(ID3D11Resource* resource, UINT subresource) noexcept {
    if (t_inside) {
        return;
    }
    if (subresource == 0) {
        const void* data = nullptr;
        AcquireSRWLockExclusive(&g_rewriteMapLock);
        for (MapSlot& slot : g_rewriteMaps) {
            if (slot.resource == resource) {
                data = slot.data;
                slot = MapSlot{};
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_rewriteMapLock);
        if (data != nullptr) {
            t_inside = true;
            const BufferKind kind = classify(resource);
            if (kind == BufferKind::palette) {
                examine_palette(resource, static_cast<const float*>(data));
            } else if (kind == BufferKind::view) {
                examine_view(resource, static_cast<const float*>(data));
            }
            t_inside = false;
        }
    }
    if (g_phase.load(std::memory_order_acquire) == static_cast<int>(Phase::idle)) {
        return;
    }
    for (MapSlot& slot : t_maps) {
        if (slot.resource == resource && slot.subresource == subresource) {
            const UINT bytes = shadowable(resource);
            if (bytes != 0 && slot.data != nullptr) {
                store_shadow(resource, slot.data, bytes);
            }
            slot = MapSlot{};
            return;
        }
    }
}

void on_update(ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box, const void* data) noexcept {
    g_updates.fetch_add(1, std::memory_order_relaxed);
    if (t_inside || g_phase.load(std::memory_order_acquire) == static_cast<int>(Phase::idle) || data == nullptr
        || subresource != 0) {
        return;
    }
    const UINT bytes = shadowable(resource);
    if (bytes == 0) {
        return;
    }
    if (box == nullptr) {
        store_shadow(resource, data, bytes);
        return;
    }
    // A partial update lands on top of the previous shadow, which is what the GPU will hold too.
    AcquireSRWLockExclusive(&g_captureLock);
    Shadow& shadow = g_shadows[resource];
    if (shadow.bytes.size() < bytes) {
        shadow.bytes.resize(bytes);
    }
    if (box->right > box->left && box->right <= bytes) {
        std::memcpy(shadow.bytes.data() + box->left, data, box->right - box->left);
    }
    shadow.generation = ++g_shadowGeneration;
    ReleaseSRWLockExclusive(&g_captureLock);
}

// ---------------------------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------------------------

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                          UINT, UINT);
using Update1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                           const void*, UINT, UINT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using CreateVsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11VertexShader**);
using CreateCsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11ComputeShader**);
using CreateIlFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*,
                                               SIZE_T, ID3D11InputLayout**);

enum CtxHook : std::size_t {
    hDrawIndexed,
    hDraw,
    hDrawIndexedInstanced,
    hDrawInstanced,
    hDrawIndexedIndirect,
    hDrawIndirect,
    hMap,
    hUnmap,
    hUpdate,
    hUpdate1,
    hDispatch,
    kCtxHooks
};

/**
 * One set of context detours per vtable. Deferred contexts are a different class in d3d11.dll
 * with their own vtable, and Dishonored's engine recorded every gameplay draw on them -- zero
 * immediate draws per frame while the game was visibly drawing. Templated on the vtable so each
 * keeps its own trampolines.
 */
template <int K> struct ContextHooks final {
    static inline std::array<hooking::detour::Handle, kCtxHooks> handles{};

    template <typename F> static F real(CtxHook hook) noexcept {
        return reinterpret_cast<F>(handles[hook].original);
    }

    static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
        DrawAction action = on_draw(c, 0, count, 1, start, base);
        if (!action.skip) {
            real<DrawIndexedFn>(hDrawIndexed)(c, count, start, base);
            finish_draw(c, action);
        }
    }
    static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c, UINT count, UINT start) {
        DrawAction action = on_draw(c, 1, count, 1, start, 0);
        if (!action.skip) {
            real<DrawFn>(hDraw)(c, count, start);
            finish_draw(c, action);
        }
    }
    static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start,
                                                       INT base, UINT startInstance) {
        DrawAction action = on_draw(c, 2, count, instances, start, base);
        if (!action.skip) {
            real<DrawIndexedInstancedFn>(hDrawIndexedInstanced)(c, count, instances, start, base, startInstance);
            finish_draw(c, action);
        }
    }
    static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start,
                                                UINT startInstance) {
        DrawAction action = on_draw(c, 3, count, instances, start, 0);
        if (!action.skip) {
            real<DrawInstancedFn>(hDrawInstanced)(c, count, instances, start, startInstance);
            finish_draw(c, action);
        }
    }
    static void STDMETHODCALLTYPE DrawIndexedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset) {
        DrawAction action = on_draw(c, 4, 0, 0, offset, 0);
        if (!action.skip) {
            real<DrawIndirectFn>(hDrawIndexedIndirect)(c, args, offset);
            finish_draw(c, action);
        }
    }
    static void STDMETHODCALLTYPE DrawIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset) {
        DrawAction action = on_draw(c, 5, 0, 0, offset, 0);
        if (!action.skip) {
            real<DrawIndirectFn>(hDrawIndirect)(c, args, offset);
            finish_draw(c, action);
        }
    }
    static HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type,
                                         UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
        const HRESULT result = real<MapFn>(hMap)(c, r, sub, type, flags, out);
        if (SUCCEEDED(result) && type != D3D11_MAP_READ) {
            on_map(r, sub, out);
        }
        return result;
    }
    static void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
        // Read BEFORE the real Unmap: afterwards the pointer may not be ours to read.
        on_unmap(r, sub);
        real<UnmapFn>(hUnmap)(c, r, sub);
    }
    static void STDMETHODCALLTYPE Update(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                         const void* data, UINT rowPitch, UINT depthPitch) {
        on_update(r, sub, box, data);
        real<UpdateFn>(hUpdate)(c, r, sub, box, data, rowPitch, depthPitch);
    }
    static void STDMETHODCALLTYPE Update1(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                          const void* data, UINT rowPitch, UINT depthPitch, UINT flags) {
        on_update(r, sub, box, data);
        real<Update1Fn>(hUpdate1)(c, r, sub, box, data, rowPitch, depthPitch, flags);
    }
    static void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
        g_dispatches.fetch_add(1, std::memory_order_relaxed);
        if (!t_inside && g_phase.load(std::memory_order_acquire) == static_cast<int>(Phase::recording)) {
            t_inside = true;
            record_dispatch(c, x, y, z);
            t_inside = false;
        }
        real<DispatchFn>(hDispatch)(c, x, y, z);
    }

    static std::array<void*, kCtxHooks> replacements() noexcept {
        return {reinterpret_cast<void*>(&DrawIndexed),         reinterpret_cast<void*>(&Draw),
                reinterpret_cast<void*>(&DrawIndexedInstanced), reinterpret_cast<void*>(&DrawInstanced),
                reinterpret_cast<void*>(&DrawIndexedIndirect),  reinterpret_cast<void*>(&DrawIndirect),
                reinterpret_cast<void*>(&Map),                  reinterpret_cast<void*>(&Unmap),
                reinterpret_cast<void*>(&Update),               reinterpret_cast<void*>(&Update1),
                reinterpret_cast<void*>(&Dispatch)};
    }
};

using Immediate = ContextHooks<0>;
using Deferred = ContextHooks<1>;

std::array<hooking::detour::Handle, 3> g_deviceHandles{};

HRESULT STDMETHODCALLTYPE create_vertex_shader(ID3D11Device* device, const void* bytecode, SIZE_T length,
                                               ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
    const HRESULT result = reinterpret_cast<CreateVsFn>(g_deviceHandles[0].original)(device, bytecode, length,
                                                                                      linkage, out);
    if (SUCCEEDED(result) && out != nullptr && *out != nullptr) {
        remember_shader(*out, bytecode, length, "vs ");
    }
    return result;
}

HRESULT STDMETHODCALLTYPE create_compute_shader(ID3D11Device* device, const void* bytecode, SIZE_T length,
                                                ID3D11ClassLinkage* linkage, ID3D11ComputeShader** out) {
    const HRESULT result = reinterpret_cast<CreateCsFn>(g_deviceHandles[1].original)(device, bytecode, length,
                                                                                      linkage, out);
    if (SUCCEEDED(result) && out != nullptr && *out != nullptr) {
        remember_shader(*out, bytecode, length, "cs ");
    }
    return result;
}

HRESULT STDMETHODCALLTYPE create_input_layout(ID3D11Device* device, const D3D11_INPUT_ELEMENT_DESC* elements,
                                              UINT count, const void* bytecode, SIZE_T length,
                                              ID3D11InputLayout** out) {
    const HRESULT result = reinterpret_cast<CreateIlFn>(g_deviceHandles[2].original)(device, elements, count,
                                                                                      bytecode, length, out);
    if (SUCCEEDED(result) && out != nullptr && *out != nullptr && elements != nullptr) {
        std::string text = "il";
        // Resolve APPEND_ALIGNED offsets for stream 0, the stream that carries the skinning data.
        LayoutInfo info{};
        UINT cursor = 0;
        for (UINT i = 0; i < count; ++i) {
            if (elements[i].InputSlot != 0) {
                continue;
            }
            const UINT offset =
                elements[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? cursor : elements[i].AlignedByteOffset;
            cursor = offset + format_bytes(elements[i].Format);
            const char* name = elements[i].SemanticName != nullptr ? elements[i].SemanticName : "";
            if (elements[i].SemanticIndex == 0 && _stricmp(name, "POSITION") == 0) {
                info.position = static_cast<int>(offset);
                info.positionFormat = elements[i].Format;
            } else if (elements[i].SemanticIndex == 0 && _stricmp(name, "BLENDWEIGHT") == 0) {
                info.weights = static_cast<int>(offset);
                info.weightsFormat = elements[i].Format;
            } else if (elements[i].SemanticIndex == 0 && _stricmp(name, "BLENDINDICES") == 0) {
                info.indices = static_cast<int>(offset);
                info.indicesFormat = elements[i].Format;
            }
        }
        for (UINT i = 0; i < count; ++i) {
            appendf(text, " %s%u:f%d:s%u:o%u%s", elements[i].SemanticName != nullptr ? elements[i].SemanticName : "?",
                    elements[i].SemanticIndex, static_cast<int>(elements[i].Format), elements[i].InputSlot,
                    elements[i].AlignedByteOffset,
                    elements[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? ":inst" : "");
        }
        AcquireSRWLockExclusive(&g_registryLock);
        g_layouts[*out] = std::move(text);
        g_layoutInfo[*out] = info;
        ReleaseSRWLockExclusive(&g_registryLock);
    }
    return result;
}

std::atomic_bool g_installTried{false};
std::atomic_bool g_installed{false};
/** Kept alive for the life of the process: its vtable is what the deferred detours point into. */
ID3D11DeviceContext* g_deferredProbe = nullptr;

/** @return True when the entry is plain code, not a jump some other overlay already planted. */
bool hookable(const void* entry) noexcept {
    std::array<std::uint8_t, 2> bytes{};
    SIZE_T read = 0;
    if (entry == nullptr || ReadProcessMemory(GetCurrentProcess(), entry, bytes.data(), bytes.size(), &read) == FALSE
        || read != bytes.size()) {
        return false;
    }
    return bytes[0] != 0xE9 && bytes[0] != 0xEB && !(bytes[0] == 0xFF && bytes[1] == 0x25);
}

void install(ID3D11Device* device) noexcept {
    if (g_installTried.exchange(true, std::memory_order_acq_rel) || device == nullptr) {
        return;
    }
    std::array<wchar_t, MAX_PATH> off{};
    if (game_path(L"SVR_GpuOff.txt", off) && GetFileAttributesW(off.data()) != INVALID_FILE_ATTRIBUTES) {
        report("ev=vr.gpu install result=skipped reason=SVR_GpuOff.txt");
        return;
    }
    const d3d::Slots slots = d3d::slots();
    device->GetImmediateContext(&g_immediate);
    if (FAILED(device->CreateDeferredContext(0, &g_deferredProbe))) {
        g_deferredProbe = nullptr;
    }
    void** const deviceTable = *reinterpret_cast<void***>(device);
    void** const immediateTable = g_immediate != nullptr ? *reinterpret_cast<void***>(g_immediate) : nullptr;
    void** const deferredTable = g_deferredProbe != nullptr ? *reinterpret_cast<void***>(g_deferredProbe) : nullptr;
    const std::array<std::size_t, kCtxHooks> contextSlots{
        slots.drawIndexed, slots.draw,  slots.drawIndexedInstanced, slots.drawInstanced,
        slots.drawIndexedInstancedIndirect, slots.drawInstancedIndirect, slots.map, slots.unmap,
        slots.updateSubresource, slots.updateSubresource1, slots.dispatch};

    std::vector<hooking::detour::Spec> specs;
    std::vector<hooking::detour::Handle*> outputs;
    std::vector<void*> taken;
    auto add = [&](void* target, void* replacement, hooking::detour::Handle* output) {
        if (target == nullptr || !hookable(target)) {
            return;
        }
        for (void* existing : taken) {
            if (existing == target) {
                return;
            }
        }
        taken.push_back(target);
        specs.push_back(hooking::detour::Spec{target, replacement});
        outputs.push_back(output);
    };
    add(deviceTable[slots.createVertexShader], reinterpret_cast<void*>(&create_vertex_shader), &g_deviceHandles[0]);
    add(deviceTable[slots.createComputeShader], reinterpret_cast<void*>(&create_compute_shader),
        &g_deviceHandles[1]);
    add(deviceTable[slots.createInputLayout], reinterpret_cast<void*>(&create_input_layout), &g_deviceHandles[2]);
    std::size_t immediateCount = 0;
    std::size_t deferredCount = 0;
    if (immediateTable != nullptr) {
        const auto replacements = Immediate::replacements();
        for (std::size_t i = 0; i < kCtxHooks; ++i) {
            const std::size_t before = specs.size();
            add(immediateTable[contextSlots[i]], replacements[i], &Immediate::handles[i]);
            immediateCount += specs.size() - before;
        }
    }
    if (deferredTable != nullptr) {
        // An entry shared with the immediate vtable is already covered; `add` skips it.
        const auto replacements = Deferred::replacements();
        for (std::size_t i = 0; i < kCtxHooks; ++i) {
            const std::size_t before = specs.size();
            add(deferredTable[contextSlots[i]], replacements[i], &Deferred::handles[i]);
            deferredCount += specs.size() - before;
        }
    }
    // One transaction for everything: either the module sees the whole pipeline or none of it.
    std::vector<hooking::detour::Handle> handles(specs.size());
    if (specs.empty() || !hooking::detour::install(specs, handles)) {
        reportf("ev=vr.gpu install result=fail specs=%zu", specs.size());
        return;
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        *outputs[i] = handles[i];
    }
    g_installed.store(true, std::memory_order_release);
    reportf("ev=vr.gpu install result=ok hooks=%zu immediate=%zu deferred=%zu imm_vt=%p def_vt=%p shared=%d "
            "slot_draw_indexed=%zu slot_map=%zu slot_vs_cb1=%zu",
            specs.size(), immediateCount, deferredCount, static_cast<void*>(immediateTable),
            static_cast<void*>(deferredTable), immediateTable == deferredTable ? 1 : 0, slots.drawIndexed, slots.map,
            slots.vsSetConstantBuffers1);
}

// ---------------------------------------------------------------------------------------------
// Ending a capture
// ---------------------------------------------------------------------------------------------

/** Copies every buffer a draw read without a shadow through a staging copy and fills its blobs. */
void read_back(ID3D11Device* device) {
    if (g_immediate == nullptr) {
        return;
    }
    std::unordered_map<std::uint32_t, std::size_t> blobIndex;
    for (std::size_t i = 0; i < g_blobs.size(); ++i) {
        blobIndex[g_blobs[i].first] = i;
    }
    t_inside = true;
    for (auto& [buffer, ids] : g_pendingReadback) {
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = desc.ByteWidth;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* staging = nullptr;
        if (SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &staging)) && staging != nullptr) {
            g_immediate->CopyResource(staging, buffer);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(g_immediate->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
                for (const std::uint32_t id : ids) {
                    auto& blob = g_blobs[blobIndex[id]].second;
                    UINT offset = 0;
                    UINT length = 0;
                    std::memcpy(&offset, blob.data(), 4);
                    std::memcpy(&length, blob.data() + 4, 4);
                    if (static_cast<std::size_t>(offset) + length <= desc.ByteWidth) {
                        blob.assign(bytes + offset, bytes + offset + length);
                    }
                }
                g_immediate->Unmap(staging, 0);
            }
            staging->Release();
        }
        buffer->Release();
    }
    t_inside = false;
    g_pendingReadback.clear();
}

/** Creates our replacement constant buffers. On the present thread, never inside a draw. */
void ensure_buffers(ID3D11Device* device) noexcept {
    auto make = [device](UINT bytes, ID3D11Buffer*& out) {
        if (out != nullptr) {
            return;
        }
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = bytes;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &out))) {
            out = nullptr;
        }
    };
    make(kPaletteBytes, g_ourPalette);
    make(kViewBytes, g_ourView);
}

float snorm16(const std::uint8_t* p) noexcept {
    std::int16_t v = 0;
    std::memcpy(&v, p, 2);
    return std::max(-1.0F, static_cast<float>(v) / 32767.0F);
}

/**
 * Reads the queued arm meshes' vertex buffers back and hands them, decoded into bind space, to
 * the arm rig. Position is R16G16B16A16_SNORM scaled by the palette header's dequantisation,
 * weights R8G8B8A8_UNORM, indices R8G8B8A8_UINT -- the formats measured in Io; anything else is
 * reported and left alone rather than guessed at.
 */
void read_meshes(ID3D11Device* device) noexcept {
    std::vector<MeshRequest> queue;
    AcquireSRWLockExclusive(&g_meshLock);
    queue.swap(g_meshQueue);
    ReleaseSRWLockExclusive(&g_meshLock);
    for (MeshRequest& request : queue) {
        bool decoded = false;
        if (request.layout.position < 0) {
            // The input layout predates our hooks (created before the first present). The arm
            // meshes come in two packings, both measured in Io; anything else stays undecoded.
            if (request.stride == 16) {
                request.layout = LayoutInfo{0, DXGI_FORMAT_R16G16B16A16_SNORM, 8, DXGI_FORMAT_R8G8B8A8_UNORM,
                                            12, DXGI_FORMAT_R8G8B8A8_UINT};
            } else if (request.stride == 12) {
                request.layout = LayoutInfo{0, DXGI_FORMAT_R16G16B16A16_SNORM, -1, DXGI_FORMAT_UNKNOWN,
                                            8, DXGI_FORMAT_R8G8B8A8_UINT};
            }
        }
        const LayoutInfo& layout = request.layout;
        const bool formats = layout.position >= 0 && layout.positionFormat == DXGI_FORMAT_R16G16B16A16_SNORM
                             && layout.indices >= 0 && layout.indicesFormat == DXGI_FORMAT_R8G8B8A8_UINT
                             && (layout.weights < 0 || layout.weightsFormat == DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D11_BUFFER_DESC desc{};
        request.buffer->GetDesc(&desc);
        if (formats && request.stride != 0 && g_immediate != nullptr) {
            D3D11_BUFFER_DESC stagingDesc{};
            stagingDesc.ByteWidth = desc.ByteWidth;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Buffer* staging = nullptr;
            t_inside = true;
            if (SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &staging)) && staging != nullptr) {
                g_immediate->CopyResource(staging, request.buffer);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(g_immediate->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData) + request.offset;
                    const std::size_t count = (desc.ByteWidth - request.offset) / request.stride;
                    std::vector<float> positions(count * 3);
                    std::vector<float> weights(count * 4);
                    std::vector<std::uint8_t> indices(count * 4);
                    for (std::size_t v = 0; v < count; ++v) {
                        const std::uint8_t* vertex = bytes + v * request.stride;
                        for (std::size_t a = 0; a < 3; ++a) {
                            positions[v * 3 + a] = snorm16(vertex + layout.position + a * 2) * request.scale
                                                   + request.origin[a];
                        }
                        for (std::size_t k = 0; k < 4; ++k) {
                            indices[v * 4 + k] = vertex[layout.indices + static_cast<int>(k)];
                            weights[v * 4 + k] = layout.weights >= 0
                                                     ? vertex[layout.weights + static_cast<int>(k)] / 255.0F
                                                     : (k == 0 ? 1.0F : 0.0F);
                        }
                    }
                    g_immediate->Unmap(staging, 0);
                    arms::add_mesh(reinterpret_cast<std::uintptr_t>(request.buffer), count, positions.data(),
                                   weights.data(), indices.data());
                    decoded = true;
                }
                staging->Release();
            }
            t_inside = false;
        }
        if (!decoded) {
            reportf("ev=vr.gpu mesh result=skip buffer=%p stride=%u pos=%d:%d w=%d:%d i=%d:%d", static_cast<void*>(request.buffer),
                    request.stride, layout.position, static_cast<int>(layout.positionFormat), layout.weights,
                    static_cast<int>(layout.weightsFormat), layout.indices, static_cast<int>(layout.indicesFormat));
        }
        AcquireSRWLockExclusive(&g_meshLock);
        // Kept as known either way: an undecodable mesh is simply drawn with the game's palette.
        g_armMeshes[request.buffer] = decoded;
        ReleaseSRWLockExclusive(&g_meshLock);
        request.buffer->Release();
    }
}

/** Writes the buffer `dumpvb` caught to SVR_Vb_<pointer>.bin through a staging copy. */
void write_dump(ID3D11Device* device) {
    ID3D11Buffer* const buffer = g_dumpTaken.exchange(nullptr, std::memory_order_acq_rel);
    if (buffer == nullptr || g_immediate == nullptr) {
        return;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    D3D11_BUFFER_DESC stagingDesc{};
    stagingDesc.ByteWidth = desc.ByteWidth;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = nullptr;
    bool written = false;
    t_inside = true;
    if (SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &staging)) && staging != nullptr) {
        g_immediate->CopyResource(staging, buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_immediate->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::array<wchar_t, 64> name{};
            swprintf_s(name.data(), name.size(), L"SVR_Vb_%p.bin", static_cast<void*>(buffer));
            std::array<wchar_t, MAX_PATH> path{};
            FILE* file = nullptr;
            if (game_path(name.data(), path) && _wfopen_s(&file, path.data(), L"wb") == 0 && file != nullptr) {
                written = std::fwrite(mapped.pData, 1, desc.ByteWidth, file) == desc.ByteWidth;
                std::fclose(file);
            }
            g_immediate->Unmap(staging, 0);
        }
        staging->Release();
    }
    t_inside = false;
    reportf("ev=vr.gpu dumpvb buffer=%p bytes=%u written=%d", static_cast<void*>(buffer), desc.ByteWidth,
            written ? 1 : 0);
    buffer->Release();
}

void finish_capture(ID3D11Device* device) {
    AcquireSRWLockExclusive(&g_captureLock);
    read_back(device);
    std::array<wchar_t, MAX_PATH> textPath{};
    std::array<wchar_t, MAX_PATH> blobPath{};
    FILE* text = nullptr;
    FILE* blob = nullptr;
    std::size_t blobBytes = 0;
    if (game_path(kCaptureText, textPath) && game_path(kCaptureBlob, blobPath)
        && _wfopen_s(&text, textPath.data(), L"wb") == 0 && text != nullptr
        && _wfopen_s(&blob, blobPath.data(), L"wb") == 0 && blob != nullptr) {
        std::fprintf(text, "# draws/dispatches %zu, blobs %zu\n", g_records.size(), g_blobs.size());
        for (const std::string& record : g_records) {
            std::fwrite(record.data(), 1, record.size(), text);
            std::fputc('\n', text);
        }
        AcquireSRWLockShared(&g_registryLock);
        for (const void* object : g_referencedShaders) {
            const auto shader = g_shaders.find(object);
            if (shader != g_shaders.end()) {
                std::fprintf(text, "s %p chk=%08X %s\n", object, shader->second.checksum,
                             shader->second.summary.c_str());
                continue;
            }
            const auto layout = g_layouts.find(object);
            if (layout != g_layouts.end()) {
                std::fprintf(text, "l %p %s\n", object, layout->second.c_str());
            }
        }
        ReleaseSRWLockShared(&g_registryLock);
        for (const auto& [id, bytes] : g_blobs) {
            const auto size = static_cast<std::uint32_t>(bytes.size());
            std::fwrite(&id, sizeof id, 1, blob);
            std::fwrite(&size, sizeof size, 1, blob);
            std::fwrite(bytes.data(), 1, bytes.size(), blob);
            blobBytes += bytes.size();
        }
    }
    if (text != nullptr) {
        std::fclose(text);
    }
    if (blob != nullptr) {
        std::fclose(blob);
    }
    reportf("ev=vr.gpu capture done records=%zu blobs=%zu blob_bytes=%zu shadows=%zu", g_records.size(),
            g_blobs.size(), blobBytes, g_shadows.size());
    g_records.clear();
    g_blobs.clear();
    g_blobIds.clear();
    g_shadows.clear();
    g_referencedShaders.clear();
    g_drawIndex = 0;
    ReleaseSRWLockExclusive(&g_captureLock);
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

void set_hide(const char* kindName, const char* value) {
    if (std::strcmp(kindName, "none") == 0) {
        for (auto& count : g_hideCount) {
            count.store(0, std::memory_order_release);
        }
        g_anyHide.store(false, std::memory_order_release);
        report("ev=vr.gpu hide cleared=1");
        return;
    }
    static constexpr std::array<const char*, hideKinds> kNames{"vs", "ps", "srv", "cb", "vb", "ib", "chk"};
    for (std::size_t kind = 0; kind < hideKinds; ++kind) {
        if (std::strcmp(kindName, kNames[kind]) != 0) {
            continue;
        }
        const std::uintptr_t parsed = static_cast<std::uintptr_t>(std::strtoull(value, nullptr, 16));
        const std::size_t count = g_hideCount[kind].load(std::memory_order_acquire);
        if (count < kMaxHide) {
            g_hide[kind][count].store(parsed, std::memory_order_relaxed);
            g_hideCount[kind].store(count + 1, std::memory_order_release);
        }
        g_anyHide.store(true, std::memory_order_release);
        reportf("ev=vr.gpu hide kind=%s value=%llX rules=%zu", kindName, static_cast<unsigned long long>(parsed),
                count + 1);
        return;
    }
    reportf("ev=vr.gpu command result=fail verb=hide kind=%s", kindName);
}

void report_stats() {
    reportf("ev=vr.gpu stats installed=%d draws_imm=%llu draws_def=%llu dispatches=%llu maps=%llu updates=%llu "
            "hidden=%llu shaders=%llu named=%llu presents=%u",
            g_installed.load() ? 1 : 0, static_cast<unsigned long long>(g_drawsImmediate.exchange(0)),
            static_cast<unsigned long long>(g_drawsDeferred.exchange(0)),
            static_cast<unsigned long long>(g_dispatches.exchange(0)),
            static_cast<unsigned long long>(g_maps.exchange(0)), static_cast<unsigned long long>(g_updates.exchange(0)),
            static_cast<unsigned long long>(g_hidden.exchange(0)),
            static_cast<unsigned long long>(g_shadersSeen.load()),
            static_cast<unsigned long long>(g_shadersNamed.load()), g_presents);
}

void poll_commands() {
    std::array<wchar_t, MAX_PATH> path{};
    if (!game_path(kCommandName, path)) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"r") != 0 || file == nullptr) {
        return;
    }
    std::array<char, 256> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        char verb[16] = {};
        char a[32] = {};
        char b[32] = {};
        const int n = sscanf_s(line.data(), "%15s %31s %31s", verb, static_cast<unsigned>(sizeof verb), a,
                               static_cast<unsigned>(sizeof a), b, static_cast<unsigned>(sizeof b));
        if (n < 1) {
            continue;
        }
        if (std::strcmp(verb, "capture") == 0) {
            if (!g_installed.load(std::memory_order_acquire)) {
                report("ev=vr.gpu capture result=fail reason=not_installed");
            } else {
                g_phase.store(static_cast<int>(Phase::armed), std::memory_order_release);
                report("ev=vr.gpu capture armed=1");
            }
        } else if (std::strcmp(verb, "hide") == 0 && n >= 2) {
            set_hide(a, n >= 3 ? b : "0");
        } else if (std::strcmp(verb, "fov") == 0 && n >= 2) {
            g_fovFix.store(std::strcmp(a, "off") != 0, std::memory_order_release);
            reportf("ev=vr.gpu fov_fix=%s", a);
        } else if (std::strcmp(verb, "rewrite") == 0 && n >= 2) {
            g_rewriteOn.store(std::strcmp(a, "off") != 0, std::memory_order_release);
            reportf("ev=vr.gpu rewrite=%s", a);
        } else if (std::strcmp(verb, "dumpvb") == 0 && n >= 2) {
            g_dumpWanted.store(static_cast<std::uintptr_t>(std::strtoull(a, nullptr, 16)), std::memory_order_release);
            reportf("ev=vr.gpu dumpvb armed=%s", a);
        } else if (std::strcmp(verb, "stats") == 0) {
            report_stats();
        } else {
            reportf("ev=vr.gpu command result=fail verb=%s", verb);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

} // namespace

void present(ID3D11Device* device, IDXGISwapChain* swapChain) noexcept {
    static_cast<void>(swapChain);
    ++g_presents;
    install(device);
    // Advance the capture at the frame boundary: armed (shadowing) -> recording -> written out.
    const int phase = g_phase.load(std::memory_order_acquire);
    if (phase == static_cast<int>(Phase::armed)) {
        g_phase.store(static_cast<int>(Phase::recording), std::memory_order_release);
    } else if (phase == static_cast<int>(Phase::recording)) {
        g_phase.store(static_cast<int>(Phase::idle), std::memory_order_release);
        finish_capture(device);
    }
    write_dump(device);
    if (g_installed.load(std::memory_order_acquire)) {
        ensure_buffers(device);
        read_meshes(device);
        arms::present(device, g_immediate, swapChain);
    }
    if (g_presents % kPollPeriod == 0) {
        poll_commands();
    }
    if (g_installed.load(std::memory_order_acquire) && g_presents % kStatsPeriod == 0) {
        report_stats();
        reportf("ev=vr.gpu rewrite on=%d views_examined=%llu vm_views=%llu palettes_examined=%llu palettes_seen=%llu "
                "palette_swaps=%llu view_swaps=%llu world_p=%.4f,%.4f",
                rewrite_wanted() ? 1 : 0, static_cast<unsigned long long>(g_viewsExamined.exchange(0)),
                static_cast<unsigned long long>(g_vmViewsSeen.exchange(0)),
                static_cast<unsigned long long>(g_palettesExamined.exchange(0)),
                static_cast<unsigned long long>(g_armPalettesSeen.exchange(0)),
                static_cast<unsigned long long>(g_paletteSwaps.exchange(0)),
                static_cast<unsigned long long>(g_viewSwaps.exchange(0)), g_worldP00.load(), g_worldP11.load());
    }
}

bool viewmodel_camera(ViewCamera& out) noexcept {
    AcquireSRWLockShared(&g_cameraLock);
    const bool valid = g_vmCameraValid;
    if (valid) {
        out = g_vmCamera;
    }
    ReleaseSRWLockShared(&g_cameraLock);
    return valid;
}

ID3D11DeviceContext* immediate_context() noexcept {
    return g_immediate;
}

} // namespace sunrise::client::hooks::vr::gpu
