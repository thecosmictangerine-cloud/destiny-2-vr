/**
 * OpenXR runtime, instance, session and frame loop for the VR module.
 *
 * The loader is ours. The OpenXR SDK ships a loader library, but linking it here is not possible:
 * the only build of it on this machine is compiled against the dynamic CRT while this DLL uses
 * the static one, and mixing those is a link error waiting to happen. The loader's job is small
 * and well specified -- find the active runtime's manifest, read the library out of it, and
 * negotiate an interface version -- so this file does it directly. That also means the mock
 * runtime and a real one are reached by exactly the same path.
 *
 * Everything here runs on the present thread, which is the only site that owns the render device.
 * The pose is published under a lock and the camera hook reads it from its own thread, so the
 * camera lags the headset by one frame. That is fine for a monoscopic proof of concept and is the
 * first thing to fix if the view ever feels swimmy.
 */

#include "xr_runtime.h"

#include <Windows.h>

#include <winternl.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#include "../../../core/logging/log.h"

namespace sunrise::client::hooks::vr::xr {
namespace {

/**
 * The loader/runtime negotiation ABI. It lives in the loader's own sources rather than the public
 * SDK headers, so the three declarations it needs are reproduced here. Stable interface v1.
 */
constexpr std::uint32_t kLoaderRuntimeVersion = 1;
constexpr std::uint32_t kLoaderInfoStructVersion = 1;
constexpr std::uint32_t kRuntimeInfoStructVersion = 1;

enum XrLoaderInterfaceStructs : int {
    kStructUninitialized = 0,
    kStructLoaderInfo,
    kStructApiLayerRequest,
    kStructRuntimeRequest,
    kStructApiLayerCreateInfo,
    kStructApiLayerNextInfo,
};

struct XrNegotiateLoaderInfo {
    int structType;
    std::uint32_t structVersion;
    std::size_t structSize;
    std::uint32_t minInterfaceVersion;
    std::uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
};

struct XrNegotiateRuntimeRequest {
    int structType;
    std::uint32_t structVersion;
    std::size_t structSize;
    std::uint32_t runtimeInterfaceVersion;
    XrVersion runtimeApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
};

using PFN_xrNegotiateLoaderRuntimeInterface =
    XrResult(XRAPI_PTR*)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*);

/** Every entry point this module calls, resolved through the runtime's own proc address. */
struct Api final {
    PFN_xrGetInstanceProcAddr getInstanceProcAddr{};
    PFN_xrCreateInstance createInstance{};
    PFN_xrDestroyInstance destroyInstance{};
    PFN_xrGetInstanceProperties getInstanceProperties{};
    PFN_xrGetSystem getSystem{};
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews{};
    PFN_xrCreateSession createSession{};
    PFN_xrDestroySession destroySession{};
    PFN_xrBeginSession beginSession{};
    PFN_xrEndSession endSession{};
    PFN_xrCreateReferenceSpace createReferenceSpace{};
    PFN_xrDestroySpace destroySpace{};
    PFN_xrPollEvent pollEvent{};
    PFN_xrWaitFrame waitFrame{};
    PFN_xrBeginFrame beginFrame{};
    PFN_xrEndFrame endFrame{};
    PFN_xrLocateViews locateViews{};
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements{};
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats{};
    PFN_xrCreateSwapchain createSwapchain{};
    PFN_xrDestroySwapchain destroySwapchain{};
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages{};
    PFN_xrAcquireSwapchainImage acquireSwapchainImage{};
    PFN_xrWaitSwapchainImage waitSwapchainImage{};
    PFN_xrReleaseSwapchainImage releaseSwapchainImage{};
    PFN_xrStringToPath stringToPath{};
    PFN_xrCreateActionSet createActionSet{};
    PFN_xrDestroyActionSet destroyActionSet{};
    PFN_xrCreateAction createAction{};
    PFN_xrSuggestInteractionProfileBindings suggestInteractionProfileBindings{};
    PFN_xrAttachSessionActionSets attachSessionActionSets{};
    PFN_xrSyncActions syncActions{};
    PFN_xrGetActionStateFloat getActionStateFloat{};
    PFN_xrGetActionStateVector2f getActionStateVector2f{};
    PFN_xrGetActionStateBoolean getActionStateBoolean{};
};

constexpr std::uint32_t kViewCount = 2;
constexpr std::uint32_t kMaxSwapchainImages = 8;
/** How long to wait for the compositor to hand an image back. Well over one frame. */
constexpr XrDuration kImageWaitNanoseconds = 100'000'000;
/** Frames between throttled submit reports. */
constexpr std::uint64_t kSubmitReportPeriod = 300;

/**
 * The runtime's swapchain the back buffer is copied into, one image for both eyes, and the blit
 * path used when the back buffer's format cannot simply be copied.
 */
struct Presentation final {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    /** Owned by the runtime; never released here. */
    std::array<ID3D11Texture2D*, kMaxSwapchainImages> images{};
    std::array<ID3D11RenderTargetView*, kMaxSwapchainImages> targets{};
    std::uint32_t imageCount{};
    std::uint32_t width{};
    std::uint32_t height{};
    /** Back-buffer format the swapchain was built for; a change rebuilds it. */
    DXGI_FORMAT sourceFormat{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    /** True when CopyResource is legal between the back buffer and the swapchain images. */
    bool copyDirect{};
    bool failed{};
    /** Blit path: an SRV-capable copy of the back buffer and a full-screen pass into the image. */
    ID3D11Texture2D* blitSource{};
    ID3D11ShaderResourceView* blitView{};
    ID3D11VertexShader* blitVertex{};
    ID3D11PixelShader* blitPixel{};
    ID3D11SamplerState* blitSampler{};
    ID3D11RasterizerState* blitRasterizer{};
    ID3DDeviceContextState* blitState{};
};

/** The action set the controllers are read through. Names match the mock runtime's lookup. */
struct Actions final {
    XrActionSet set{XR_NULL_HANDLE};
    XrAction move{XR_NULL_HANDLE};
    XrAction turn{XR_NULL_HANDLE};
    XrAction triggerL{XR_NULL_HANDLE};
    XrAction triggerR{XR_NULL_HANDLE};
    XrAction gripL{XR_NULL_HANDLE};
    XrAction gripR{XR_NULL_HANDLE};
    XrAction a{XR_NULL_HANDLE};
    XrAction b{XR_NULL_HANDLE};
    XrAction x{XR_NULL_HANDLE};
    XrAction y{XR_NULL_HANDLE};
    XrAction thumbL{XR_NULL_HANDLE};
    XrAction thumbR{XR_NULL_HANDLE};
    XrAction menu{XR_NULL_HANDLE};
    bool attached{};
};

/** Full-screen triangle; the pixel shader decodes sRGB when the target encodes it again. */
constexpr const char* kBlitVertexShader = R"(
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    Output output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

constexpr const char* kBlitPixelShaderDecode = R"(
Texture2D source : register(t0);
SamplerState linearSampler : register(s0);
float3 decode(float3 c) {
    float3 low = c / 12.92;
    float3 high = pow((c + 0.055) / 1.055, 2.4);
    return lerp(low, high, step(0.04045, c));
}
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = source.Sample(linearSampler, uv);
    return float4(decode(saturate(c.rgb)), 1.0);
}
)";

constexpr const char* kBlitPixelShaderCopy = R"(
Texture2D source : register(t0);
SamplerState linearSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(source.Sample(linearSampler, uv).rgb, 1.0);
}
)";

HMODULE g_runtimeModule{};
Api g_api{};
XrInstance g_instance{XR_NULL_HANDLE};
XrSystemId g_system{XR_NULL_SYSTEM_ID};
XrSession g_session{XR_NULL_HANDLE};
XrSpace g_space{XR_NULL_HANDLE};
XrSessionState g_sessionState{XR_SESSION_STATE_UNKNOWN};
bool g_sessionRunning{false};
bool g_frameOpen{false};
bool g_attempted{false};
Status g_status{Status::off};
XrTime g_predictedDisplayTime{};
std::array<char, 64> g_runtimeName{};

/** Units the game moves for one metre of head travel. One is the starting guess. */
float g_unitsPerMetre{1.0F};

/** Head position taken as the origin, in OpenXR's own basis and metres. */
XrVector3f g_origin{};
bool g_haveOrigin{false};
bool g_recentreRequested{true};

HeadPose g_pose{};
InputState g_input{};
SRWLOCK g_poseLock{SRWLOCK_INIT};
std::uint64_t g_frameIndex{};

Presentation g_presentation{};
Actions g_actions{};

/** Views located this frame: the pose the NEXT rendered frame will be drawn from. */
std::array<XrView, kViewCount> g_pendingViews{};
bool g_havePending{false};
/** Views the frame being presented was drawn from, which is what the layer must declare. */
std::array<XrView, kViewCount> g_renderedViews{};
bool g_haveRendered{false};
bool g_shouldRender{false};
/** Horizontal FOV the camera hook left in the engine, read back from the pose block. */
std::atomic<float> g_renderedFov{0.0F};

/** Emits one preformatted line on the client channel. */
void log_line(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, text);
}

/** Emits one formatted line, truncated to the log's line capacity. */
#pragma warning(push)
// The format reaches snprintf as a parameter rather than a literal, which is the point of the
// helper; every caller in this file passes a literal.
#pragma warning(disable : 4774)
template <typename... Args> void log_fmt(const char* format, Args... args) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(), line.size(), format, args...);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}
#pragma warning(pop)

/**
 * Reads the path of the active runtime's manifest.
 * XR_RUNTIME_JSON wins when it is set, which is how the mock runtime is selected for a test.
 * @param output Receives the path.
 * @return True when a path was found.
 */
[[nodiscard]] bool manifest_path(std::wstring& output) noexcept {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length =
        GetEnvironmentVariableW(L"XR_RUNTIME_JSON", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        output.assign(buffer.data(), length);
        return true;
    }
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &key)
        != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LSTATUS status = RegQueryValueExW(
        key, L"ActiveRuntime", nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }
    output.assign(buffer.data());
    return !output.empty();
}

/**
 * Pulls `runtime.library_path` out of a manifest.
 * A full JSON parser is not warranted for one string: the key is unique in the file and its value
 * is a plain string whose only escape is the doubled backslash of a Windows path.
 * @param text Whole manifest.
 * @param output Receives the unescaped path.
 * @return True when the key was present and its value non-empty.
 */
[[nodiscard]] bool library_path_from(const std::string& text, std::string& output) noexcept {
    const std::size_t key = text.find("\"library_path\"");
    if (key == std::string::npos) {
        return false;
    }
    const std::size_t colon = text.find(':', key);
    if (colon == std::string::npos) {
        return false;
    }
    const std::size_t open = text.find('"', colon);
    if (open == std::string::npos) {
        return false;
    }
    output.clear();
    for (std::size_t index = open + 1; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '"') {
            return !output.empty();
        }
        if (character == '\\' && index + 1 < text.size()) {
            ++index;
            output.push_back(text[index]);
            continue;
        }
        output.push_back(character);
    }
    return false;
}

/** @return The whole contents of a file, or an empty string. */
[[nodiscard]] std::string read_file(const std::wstring& path) noexcept {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string text;
    std::array<char, 4096> chunk{};
    DWORD read = 0;
    while (ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)
           && read > 0) {
        text.append(chunk.data(), read);
    }
    CloseHandle(file);
    return text;
}

/** @return A UTF-8 string as UTF-16, or empty when it does not convert. */
[[nodiscard]] std::wstring widen_utf8(const std::string& text) noexcept {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

/**
 * Reads the first bytes of an export as they are in the module's file on disk.
 * The game hot-patches loader exports in memory; the image in System32 is the untouched original,
 * and the first bytes of an x64 function carry no absolute relocations, so file bytes equal the
 * bytes the loader mapped.
 * @param moduleName Base name of a System32 module.
 * @param exportRva Address of the export relative to the module base.
 * @param output Receives the bytes.
 * @return True when the file was parsed and the bytes read.
 */
[[nodiscard]] bool export_bytes_on_disk(const wchar_t* moduleName,
                                        std::uint32_t exportRva,
                                        std::span<std::byte> output) noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    const UINT systemLength = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (systemLength == 0 || systemLength >= path.size()) {
        return false;
    }
    std::wstring full(path.data(), systemLength);
    full += L'\\';
    full += moduleName;
    const std::string image = read_file(full);
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size()) {
        return false;
    }
    const auto* headers =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(headers);
    for (WORD index = 0; index < headers->FileHeader.NumberOfSections; ++index, ++section) {
        const std::uint32_t begin = section->VirtualAddress;
        const std::uint32_t end = begin + (std::max)(section->Misc.VirtualSize, section->SizeOfRawData);
        if (exportRva < begin || exportRva >= end) {
            continue;
        }
        const std::size_t offset = section->PointerToRawData + (exportRva - begin);
        if (offset + output.size() > image.size()) {
            return false;
        }
        std::memcpy(output.data(), image.data() + offset, output.size());
        return true;
    }
    return false;
}

/**
 * Puts the on-disk prologue back over one export the game has hot-patched in memory.
 * The game detours kernelbase!LoadLibraryExW and kernel32!GetProcAddress and refuses calls from
 * modules it does not know. Every CRT's DllMain calls both, so no foreign runtime can finish
 * initialising while the patches stand: that is the ERROR_DLL_INIT_FAILED every load ended in.
 * @return True when the export is now the original code, whether or not it had to be restored.
 */
[[nodiscard]] bool restore_export(const wchar_t* moduleName, const char* exportName) noexcept {
    const HMODULE module = GetModuleHandleW(moduleName);
    if (module == nullptr) {
        return false;
    }
    auto* const live = reinterpret_cast<std::byte*>(GetProcAddress(module, exportName));
    if (live == nullptr) {
        return false;
    }
    constexpr std::size_t kPrologue = 16;
    std::array<std::byte, kPrologue> original{};
    const auto rva = static_cast<std::uint32_t>(live - reinterpret_cast<std::byte*>(module));
    if (!export_bytes_on_disk(moduleName, rva, original)) {
        log_fmt("ev=vr.xr unhook module=%ls export=%s result=fail reason=disk", moduleName, exportName);
        return false;
    }
    std::size_t differing = 0;
    for (std::size_t index = 0; index < kPrologue; ++index) {
        if (live[index] != original[index]) {
            differing = index + 1;
        }
    }
    if (differing == 0) {
        return true;
    }
    DWORD previous = 0;
    if (VirtualProtect(live, differing, PAGE_EXECUTE_READWRITE, &previous) == FALSE) {
        log_fmt("ev=vr.xr unhook module=%ls export=%s result=fail reason=protect", moduleName, exportName);
        return false;
    }
    std::memcpy(live, original.data(), differing);
    DWORD ignored = 0;
    VirtualProtect(live, differing, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), live, differing);
    log_fmt("ev=vr.xr unhook module=%ls export=%s result=ok bytes=%zu", moduleName, exportName, differing);
    return true;
}

/**
 * Loads a module through ntdll's loader entry, bypassing kernelbase!LoadLibraryExW.
 * @param path Full path of the module.
 * @param status Receives the NTSTATUS the loader answered with.
 * @return The module, or null.
 */
[[nodiscard]] HMODULE load_via_ldr(const std::wstring& path, LONG& status) noexcept {
    using LdrLoadDllFn = LONG(NTAPI*)(PWSTR, PULONG, PUNICODE_STRING, PVOID*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto ldrLoadDll = ntdll != nullptr
                                ? reinterpret_cast<LdrLoadDllFn>(GetProcAddress(ntdll, "LdrLoadDll"))
                                : nullptr;
    if (ldrLoadDll == nullptr) {
        status = -1;
        return nullptr;
    }
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(path.c_str());
    name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    name.MaximumLength = static_cast<USHORT>(name.Length + sizeof(wchar_t));
    PVOID module = nullptr;
    status = ldrLoadDll(nullptr, nullptr, &name, &module);
    return status >= 0 ? static_cast<HMODULE>(module) : nullptr;
}

/** First exception raised while a watched load ran, for the diagnosis of a refused DllMain. */
struct WatchedException final {
    bool hit{};
    DWORD code{};
    void* address{};
};
WatchedException g_watched{};

LONG CALLBACK watch_exceptions(EXCEPTION_POINTERS* pointers) noexcept {
    if (!g_watched.hit && pointers != nullptr && pointers->ExceptionRecord != nullptr) {
        g_watched.hit = true;
        g_watched.code = pointers->ExceptionRecord->ExceptionCode;
        g_watched.address = pointers->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/** Names the module an address falls in, with its offset, or the raw address. */
void describe_address(void* address, char* output, std::size_t size) noexcept {
    HMODULE module = nullptr;
    if (address != nullptr
        && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address),
                              &module)
        && module != nullptr) {
        std::array<wchar_t, MAX_PATH> path{};
        GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        const wchar_t* base = path.data();
        for (const wchar_t* cursor = path.data(); *cursor != L'\0'; ++cursor) {
            if (*cursor == L'\\' || *cursor == L'/') {
                base = cursor + 1;
            }
        }
        std::snprintf(output,
                      size,
                      "%ls+0x%llX",
                      base,
                      static_cast<unsigned long long>(reinterpret_cast<std::byte*>(address)
                                                      - reinterpret_cast<std::byte*>(module)));
        return;
    }
    std::snprintf(output, size, "%p", address);
}

/**
 * LoadLibraryW with a vectored handler around it, so an exception thrown inside the DLL's
 * initialisation is caught in the log before the loader swallows it into STATUS_DLL_INIT_FAILED.
 */
[[nodiscard]] HMODULE load_watched(const wchar_t* path, const char* label) noexcept {
    g_watched = WatchedException{};
    void* const handler = AddVectoredExceptionHandler(1, &watch_exceptions);
    const HMODULE module = LoadLibraryW(path);
    const DWORD error = GetLastError();
    if (handler != nullptr) {
        RemoveVectoredExceptionHandler(handler);
    }
    if (g_watched.hit) {
        std::array<char, 128> where{};
        describe_address(g_watched.address, where.data(), where.size());
        log_fmt("ev=vr.xr load exception label=%s code=0x%08lX at=%s",
                label,
                static_cast<unsigned long>(g_watched.code),
                where.data());
    }
    SetLastError(error);
    return module;
}

/**
 * One node of ntdll's DLL notification list. Undocumented, unchanged since Vista: the cookie
 * LdrRegisterDllNotification hands back is a pointer to the caller's own node.
 */
struct LdrDllNotificationEntry final {
    LIST_ENTRY links;
    void* callback;
    void* context;
};

using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG, void*, void*, void**);
using LdrUnregisterDllNotificationFn = LONG(NTAPI*)(void*);
using LdrLockLoaderLockFn = LONG(NTAPI*)(ULONG, ULONG*, ULONG_PTR*);
using LdrUnlockLoaderLockFn = LONG(NTAPI*)(ULONG, ULONG_PTR);

/** Stands in for a neutralised notification callback. */
void CALLBACK ignore_notification(ULONG, const void*, void*) noexcept {}

/** Registered only to find the list; never expected to run. */
void CALLBACK placeholder_notification(ULONG, const void*, void*) noexcept {}

/** @return True when an address lies inside a module's mapped image. */
[[nodiscard]] bool inside_module(const void* address, HMODULE module) noexcept {
    HMODULE owner = nullptr;
    return address != nullptr
           && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                     | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 static_cast<LPCWSTR>(address),
                                 &owner)
           && owner == module;
}

/**
 * Finds every DLL-load notification callback the process has registered and neutralises the
 * ones that are not Windows' own.
 *
 * The game registers one. The loader runs it for every new module before that module's DllMain,
 * and afterwards the DllMain never runs and the load ends in STATUS_DLL_INIT_FAILED -- for a
 * bare no-CRT DLL just as for a runtime -- while mapping the same file without initialisation
 * succeeds and no exception is raised. Nothing else in the process is in a position to do that:
 * ntdll's own code is byte-identical to the file. The callback is left in the list with its
 * function pointer pointing at a no-op, so the list stays consistent for the loader.
 * @return True when the list was walked.
 */
[[nodiscard]] bool neutralise_dll_notifications() noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&neutralise_dll_notifications),
                       &self);
    if (ntdll == nullptr || self == nullptr) {
        return false;
    }
    const auto registerFn = reinterpret_cast<LdrRegisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    const auto unregisterFn = reinterpret_cast<LdrUnregisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
    const auto lockFn =
        reinterpret_cast<LdrLockLoaderLockFn>(GetProcAddress(ntdll, "LdrLockLoaderLock"));
    const auto unlockFn =
        reinterpret_cast<LdrUnlockLoaderLockFn>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"));
    if (registerFn == nullptr || unregisterFn == nullptr || lockFn == nullptr
        || unlockFn == nullptr) {
        log_line("ev=vr.xr ldr_notify result=fail reason=exports");
        return false;
    }
    void* cookie = nullptr;
    if (registerFn(0, reinterpret_cast<void*>(&placeholder_notification), nullptr, &cookie) != 0
        || cookie == nullptr) {
        log_line("ev=vr.xr ldr_notify result=fail reason=register");
        return false;
    }
    ULONG lockState = 0;
    ULONG_PTR lockCookie = 0;
    const bool locked = lockFn(0, &lockState, &lockCookie) == 0;
    auto* const ours = static_cast<LdrDllNotificationEntry*>(cookie);
    std::size_t seen = 0;
    std::size_t neutralised = 0;
    for (LIST_ENTRY* entry = ours->links.Flink; entry != nullptr && entry != &ours->links;
         entry = entry->Flink) {
        // The list head is a global inside ntdll's image; every real node is heap.
        if (inside_module(entry, ntdll)) {
            continue;
        }
        auto* const node = reinterpret_cast<LdrDllNotificationEntry*>(entry);
        ++seen;
        std::array<char, 128> where{};
        describe_address(node->callback, where.data(), where.size());
        const bool foreign = !inside_module(node->callback, ntdll) && !inside_module(node->callback, self)
                             && node->callback != reinterpret_cast<void*>(&ignore_notification);
        log_fmt("ev=vr.xr ldr_notify callback=%s foreign=%d", where.data(), foreign ? 1 : 0);
        if (foreign) {
            node->callback = reinterpret_cast<void*>(&ignore_notification);
            ++neutralised;
        }
    }
    if (locked) {
        unlockFn(0, lockCookie);
    }
    unregisterFn(cookie);
    log_fmt("ev=vr.xr ldr_notify result=ok seen=%zu neutralised=%zu", seen, neutralised);
    return true;
}

/**
 * Runs one experiment per hypothesis after a refused load and logs each answer.
 * Mapping without initialisation separates the loader from DllMain; the two probe DLLs beside the
 * game separate the CRT from a bare entry point, and their marker files tell a DllMain that ran
 * from one the game skipped; FLS and TLS allocation rule out slot exhaustion.
 */
void diagnose_load_failure(const std::wstring& runtimePath) noexcept {
    const HMODULE mapped =
        LoadLibraryExW(runtimePath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    log_fmt("ev=vr.xr diag map_only=%d error=%lu",
            mapped != nullptr ? 1 : 0,
            mapped != nullptr ? 0UL : GetLastError());
    if (mapped != nullptr) {
        FreeLibrary(mapped);
    }
    const DWORD fls = FlsAlloc(nullptr);
    const DWORD tls = TlsAlloc();
    log_fmt("ev=vr.xr diag fls=%s tls=%s",
            fls != FLS_OUT_OF_INDEXES ? "ok" : "exhausted",
            tls != TLS_OUT_OF_INDEXES ? "ok" : "exhausted");
    if (fls != FLS_OUT_OF_INDEXES) {
        FlsFree(fls);
    }
    if (tls != TLS_OUT_OF_INDEXES) {
        TlsFree(tls);
    }
    const std::size_t slash = runtimePath.find_last_of(L"\\/");
    const std::wstring directory =
        slash != std::wstring::npos ? runtimePath.substr(0, slash + 1) : std::wstring{};
    constexpr std::array<const wchar_t*, 2> probes{L"SVR_Probe_NoCrt", L"SVR_Probe_Crt"};
    for (const wchar_t* probe : probes) {
        const std::wstring dll = directory + probe + L".dll";
        const std::wstring marker = directory + probe + L".txt";
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        DeleteFileW(marker.c_str());
        const HMODULE module = load_watched(dll.c_str(), "probe");
        const DWORD error = GetLastError();
        const bool entered = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
        log_fmt("ev=vr.xr diag probe=%ls loaded=%d error=%lu entered=%d",
                probe,
                module != nullptr ? 1 : 0,
                module != nullptr ? 0UL : error,
                entered ? 1 : 0);
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }
}

/**
 * Loads the active runtime and negotiates the loader interface with it.
 * @return True when the runtime handed back a usable proc address.
 */
[[nodiscard]] bool load_runtime() noexcept {
    std::wstring manifest;
    if (!manifest_path(manifest)) {
        log_line("ev=vr.xr load result=fail reason=no_active_runtime");
        g_status = Status::noRuntime;
        return false;
    }
    const std::string text = read_file(manifest);
    std::string library;
    if (text.empty() || !library_path_from(text, library)) {
        log_line("ev=vr.xr load result=fail reason=manifest_unreadable");
        g_status = Status::noRuntime;
        return false;
    }
    // A manifest may name the library relative to its own directory.
    std::wstring wide = widen_utf8(library);
    if (wide.find(L':') == std::wstring::npos) {
        const std::size_t slash = manifest.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            wide = manifest.substr(0, slash + 1) + wide;
        }
    }
    log_fmt("ev=vr.xr load path=%ls", wide.c_str());
    // The runtime's own DllMain, and every DLL it pulls in, must get the real loader.
    (void)restore_export(L"kernelbase.dll", "LoadLibraryExW");
    (void)restore_export(L"kernel32.dll", "GetProcAddress");
    (void)neutralise_dll_notifications();
    g_runtimeModule = load_watched(wide.c_str(), "runtime");
    if (g_runtimeModule == nullptr) {
        const DWORD error = GetLastError();
        // The game patches kernelbase!LoadLibraryExW (a hot-patch jump into a trampoline of its
        // own) and answers ERROR_DLL_INIT_FAILED for any module it does not know. ntdll's loader
        // entry is untouched, so a foreign runtime has to go in underneath the patched export.
        LONG status = 0;
        g_runtimeModule = load_via_ldr(wide, status);
        if (g_runtimeModule == nullptr) {
            log_fmt("ev=vr.xr load result=fail reason=load_library error=%lu ldr_status=0x%08lX",
                    error,
                    static_cast<unsigned long>(status));
            diagnose_load_failure(wide);
            g_status = Status::noRuntime;
            return false;
        }
        log_fmt("ev=vr.xr load via=LdrLoadDll after_error=%lu", error);
    }
    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(g_runtimeModule, "xrNegotiateLoaderRuntimeInterface"));
    if (negotiate == nullptr) {
        log_line("ev=vr.xr load result=fail reason=no_negotiate_export");
        g_status = Status::negotiateFailed;
        return false;
    }
    XrNegotiateLoaderInfo info{};
    info.structType = kStructLoaderInfo;
    info.structVersion = kLoaderInfoStructVersion;
    info.structSize = sizeof info;
    info.minInterfaceVersion = 1;
    info.maxInterfaceVersion = kLoaderRuntimeVersion;
    // The runtime refuses when its own API version falls outside this range, and it reports its
    // version, not the lowest it supports: Meta's runtime is 1.1.x and turned down a 1.0.255 cap
    // (2026-09-08). Any 1.x is acceptable; the instance is still created as a 1.0 application.
    info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.maxApiVersion = XR_MAKE_VERSION(1, 0xFFFF, 0xFFFFFFFFu);
    XrNegotiateRuntimeRequest request{};
    request.structType = kStructRuntimeRequest;
    request.structVersion = kRuntimeInfoStructVersion;
    request.structSize = sizeof request;
    if (negotiate(&info, &request) != XR_SUCCESS || request.getInstanceProcAddr == nullptr) {
        log_line("ev=vr.xr load result=fail reason=negotiate_refused");
        g_status = Status::negotiateFailed;
        return false;
    }
    g_api.getInstanceProcAddr = request.getInstanceProcAddr;
    log_fmt("ev=vr.xr load result=ok interface=%u runtime_api=%u.%u.%u", request.runtimeInterfaceVersion,
            static_cast<unsigned>(XR_VERSION_MAJOR(request.runtimeApiVersion)),
            static_cast<unsigned>(XR_VERSION_MINOR(request.runtimeApiVersion)),
            static_cast<unsigned>(XR_VERSION_PATCH(request.runtimeApiVersion)));
    return true;
}

/**
 * Resolves one entry point through the runtime's proc address.
 * @return True when the runtime provided it.
 */
template <typename T>
[[nodiscard]] bool bind(T& slot, const char* name, XrInstance instance) noexcept {
    PFN_xrVoidFunction function = nullptr;
    if (g_api.getInstanceProcAddr(instance, name, &function) != XR_SUCCESS
        || function == nullptr) {
        log_fmt("ev=vr.xr bind result=fail name=%s", name);
        return false;
    }
    slot = reinterpret_cast<T>(function);
    return true;
}

/** Resolves the handful of entry points callable before an instance exists. */
[[nodiscard]] bool bind_global() noexcept {
    return bind(g_api.createInstance, "xrCreateInstance", XR_NULL_HANDLE);
}

/** Resolves every instance-level entry point. */
[[nodiscard]] bool bind_instance() noexcept {
    return bind(g_api.destroyInstance, "xrDestroyInstance", g_instance)
           && bind(g_api.getInstanceProperties, "xrGetInstanceProperties", g_instance)
           && bind(g_api.getSystem, "xrGetSystem", g_instance)
           && bind(g_api.enumerateViewConfigurationViews,
                   "xrEnumerateViewConfigurationViews",
                   g_instance)
           && bind(g_api.createSession, "xrCreateSession", g_instance)
           && bind(g_api.destroySession, "xrDestroySession", g_instance)
           && bind(g_api.beginSession, "xrBeginSession", g_instance)
           && bind(g_api.endSession, "xrEndSession", g_instance)
           && bind(g_api.createReferenceSpace, "xrCreateReferenceSpace", g_instance)
           && bind(g_api.destroySpace, "xrDestroySpace", g_instance)
           && bind(g_api.pollEvent, "xrPollEvent", g_instance)
           && bind(g_api.waitFrame, "xrWaitFrame", g_instance)
           && bind(g_api.beginFrame, "xrBeginFrame", g_instance)
           && bind(g_api.endFrame, "xrEndFrame", g_instance)
           && bind(g_api.locateViews, "xrLocateViews", g_instance)
           && bind(g_api.getD3D11Requirements,
                   "xrGetD3D11GraphicsRequirementsKHR",
                   g_instance)
           && bind(g_api.enumerateSwapchainFormats, "xrEnumerateSwapchainFormats", g_instance)
           && bind(g_api.createSwapchain, "xrCreateSwapchain", g_instance)
           && bind(g_api.destroySwapchain, "xrDestroySwapchain", g_instance)
           && bind(g_api.enumerateSwapchainImages, "xrEnumerateSwapchainImages", g_instance)
           && bind(g_api.acquireSwapchainImage, "xrAcquireSwapchainImage", g_instance)
           && bind(g_api.waitSwapchainImage, "xrWaitSwapchainImage", g_instance)
           && bind(g_api.releaseSwapchainImage, "xrReleaseSwapchainImage", g_instance)
           && bind(g_api.stringToPath, "xrStringToPath", g_instance)
           && bind(g_api.createActionSet, "xrCreateActionSet", g_instance)
           && bind(g_api.destroyActionSet, "xrDestroyActionSet", g_instance)
           && bind(g_api.createAction, "xrCreateAction", g_instance)
           && bind(g_api.suggestInteractionProfileBindings,
                   "xrSuggestInteractionProfileBindings",
                   g_instance)
           && bind(g_api.attachSessionActionSets, "xrAttachSessionActionSets", g_instance)
           && bind(g_api.syncActions, "xrSyncActions", g_instance)
           && bind(g_api.getActionStateFloat, "xrGetActionStateFloat", g_instance)
           && bind(g_api.getActionStateVector2f, "xrGetActionStateVector2f", g_instance)
           && bind(g_api.getActionStateBoolean, "xrGetActionStateBoolean", g_instance);
}

/** Releases one COM object and clears the pointer. */
template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

/** @return An OpenXR path atom for a string, or XR_NULL_PATH. */
[[nodiscard]] XrPath path_of(const char* text) noexcept {
    XrPath path = XR_NULL_PATH;
    if (g_api.stringToPath(g_instance, text, &path) != XR_SUCCESS) {
        return XR_NULL_PATH;
    }
    return path;
}

/** Creates one action in the gamepad set. */
[[nodiscard]] bool make_action(XrAction& output, const char* name, XrActionType type) noexcept {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::snprintf(info.actionName, sizeof info.actionName, "%s", name);
    std::snprintf(info.localizedActionName, sizeof info.localizedActionName, "%s", name);
    return g_api.createAction(g_actions.set, &info, &output) == XR_SUCCESS
           && output != XR_NULL_HANDLE;
}

/** One action bound to one input path of a profile. */
struct Binding final {
    XrAction action{};
    const char* path{};
};

/** Suggests one profile's bindings. A runtime that lacks the profile refuses; that is not fatal. */
void suggest(const char* profile, std::span<const Binding> bindings) noexcept {
    std::array<XrActionSuggestedBinding, 16> suggested{};
    std::uint32_t count = 0;
    for (const Binding& binding : bindings) {
        const XrPath path = path_of(binding.path);
        if (path == XR_NULL_PATH || count >= suggested.size()) {
            continue;
        }
        suggested[count++] = XrActionSuggestedBinding{binding.action, path};
    }
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    info.interactionProfile = path_of(profile);
    info.countSuggestedBindings = count;
    info.suggestedBindings = suggested.data();
    const XrResult result = g_api.suggestInteractionProfileBindings(g_instance, &info);
    log_fmt("ev=vr.xr bindings profile=%s count=%u result=%d", profile, count, static_cast<int>(result));
}

/**
 * Creates the action set, binds it for the Touch controllers and attaches it to the session.
 * The action names are what the mock runtime keys its synthetic input on, so they are load
 * bearing for the tests and must not change casually.
 */
[[nodiscard]] bool create_actions() noexcept {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(setInfo.actionSetName, sizeof setInfo.actionSetName, "gamepad");
    std::snprintf(setInfo.localizedActionSetName, sizeof setInfo.localizedActionSetName, "Gamepad");
    if (g_api.createActionSet(g_instance, &setInfo, &g_actions.set) != XR_SUCCESS) {
        log_line("ev=vr.xr actions result=fail reason=set");
        return false;
    }
    const bool created =
        make_action(g_actions.move, "move", XR_ACTION_TYPE_VECTOR2F_INPUT)
        && make_action(g_actions.turn, "turn", XR_ACTION_TYPE_VECTOR2F_INPUT)
        && make_action(g_actions.triggerL, "trigger_l", XR_ACTION_TYPE_FLOAT_INPUT)
        && make_action(g_actions.triggerR, "trigger_r", XR_ACTION_TYPE_FLOAT_INPUT)
        && make_action(g_actions.gripL, "grip_l", XR_ACTION_TYPE_FLOAT_INPUT)
        && make_action(g_actions.gripR, "grip_r", XR_ACTION_TYPE_FLOAT_INPUT)
        && make_action(g_actions.a, "btn_a", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.b, "btn_b", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.x, "btn_x", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.y, "btn_y", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.thumbL, "btn_thumb_l", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.thumbR, "btn_thumb_r", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.menu, "btn_menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    if (!created) {
        log_line("ev=vr.xr actions result=fail reason=action");
        return false;
    }
    const std::array<Binding, 13> touch{{
        {g_actions.move, "/user/hand/left/input/thumbstick"},
        {g_actions.turn, "/user/hand/right/input/thumbstick"},
        {g_actions.triggerL, "/user/hand/left/input/trigger/value"},
        {g_actions.triggerR, "/user/hand/right/input/trigger/value"},
        {g_actions.gripL, "/user/hand/left/input/squeeze/value"},
        {g_actions.gripR, "/user/hand/right/input/squeeze/value"},
        {g_actions.x, "/user/hand/left/input/x/click"},
        {g_actions.y, "/user/hand/left/input/y/click"},
        {g_actions.a, "/user/hand/right/input/a/click"},
        {g_actions.b, "/user/hand/right/input/b/click"},
        {g_actions.thumbL, "/user/hand/left/input/thumbstick/click"},
        {g_actions.thumbR, "/user/hand/right/input/thumbstick/click"},
        {g_actions.menu, "/user/hand/left/input/menu/click"},
    }};
    suggest("/interaction_profiles/oculus/touch_controller", touch);
    const std::array<Binding, 3> simple{{
        {g_actions.triggerR, "/user/hand/right/input/select/click"},
        {g_actions.triggerL, "/user/hand/left/input/select/click"},
        {g_actions.menu, "/user/hand/left/input/menu/click"},
    }};
    suggest("/interaction_profiles/khr/simple_controller", simple);
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_actions.set;
    if (g_api.attachSessionActionSets(g_session, &attach) != XR_SUCCESS) {
        log_line("ev=vr.xr actions result=fail reason=attach");
        return false;
    }
    g_actions.attached = true;
    log_line("ev=vr.xr actions result=ok");
    return true;
}

[[nodiscard]] float float_of(XrAction action) noexcept {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    if (g_api.getActionStateFloat(g_session, &get, &state) != XR_SUCCESS || !state.isActive) {
        return 0.0F;
    }
    return state.currentState;
}

[[nodiscard]] XrVector2f vector_of(XrAction action) noexcept {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (g_api.getActionStateVector2f(g_session, &get, &state) != XR_SUCCESS || !state.isActive) {
        return XrVector2f{};
    }
    return state.currentState;
}

[[nodiscard]] bool bool_of(XrAction action) noexcept {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (g_api.getActionStateBoolean(g_session, &get, &state) != XR_SUCCESS || !state.isActive) {
        return false;
    }
    return state.currentState == XR_TRUE;
}

/** Syncs the action set and publishes the controllers. An unfocused session reads as idle. */
void sync_actions() noexcept {
    if (!g_actions.attached) {
        return;
    }
    XrActiveActionSet active{g_actions.set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    InputState input{};
    input.valid = true;
    if (g_api.syncActions(g_session, &sync) == XR_SUCCESS) {
        const XrVector2f move = vector_of(g_actions.move);
        const XrVector2f turn = vector_of(g_actions.turn);
        input.moveX = move.x;
        input.moveY = move.y;
        input.turnX = turn.x;
        input.turnY = turn.y;
        input.triggerL = float_of(g_actions.triggerL);
        input.triggerR = float_of(g_actions.triggerR);
        input.gripL = float_of(g_actions.gripL);
        input.gripR = float_of(g_actions.gripR);
        input.buttons = (bool_of(g_actions.a) ? kButtonA : 0U) | (bool_of(g_actions.b) ? kButtonB : 0U)
                        | (bool_of(g_actions.x) ? kButtonX : 0U) | (bool_of(g_actions.y) ? kButtonY : 0U)
                        | (bool_of(g_actions.thumbL) ? kButtonThumbL : 0U)
                        | (bool_of(g_actions.thumbR) ? kButtonThumbR : 0U)
                        | (bool_of(g_actions.menu) ? kButtonMenu : 0U);
    }
    AcquireSRWLockExclusive(&g_poseLock);
    g_input = input;
    ReleaseSRWLockExclusive(&g_poseLock);
}

/** Frees everything Presentation owns. The swapchain images belong to the runtime. */
void destroy_presentation() noexcept {
    Presentation& p = g_presentation;
    for (auto& target : p.targets) {
        release_com(target);
    }
    release_com(p.blitSource);
    release_com(p.blitView);
    release_com(p.blitVertex);
    release_com(p.blitPixel);
    release_com(p.blitSampler);
    release_com(p.blitRasterizer);
    release_com(p.blitState);
    if (p.swapchain != XR_NULL_HANDLE && g_api.destroySwapchain != nullptr) {
        g_api.destroySwapchain(p.swapchain);
    }
    p = Presentation{};
}

/** @return The typed view format for a back-buffer format, typeless ones included. */
[[nodiscard]] DXGI_FORMAT typed_view_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

/** @return True when a format is 8-bit RGBA in either channel order. */
[[nodiscard]] bool is_rgba8_family(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
           || format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

[[nodiscard]] bool is_bgra8_family(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
           || format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

[[nodiscard]] bool is_srgb(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

/** Builds the blit pass: shaders, sampler, a staging copy of the back buffer and one RTV per image. */
[[nodiscard]] bool prepare_blit(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source) noexcept {
    Presentation& p = g_presentation;
    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT result = D3DCompile(kBlitVertexShader,
                                std::strlen(kBlitVertexShader),
                                nullptr,
                                nullptr,
                                nullptr,
                                "main",
                                "vs_4_0",
                                D3DCOMPILE_ENABLE_STRICTNESS,
                                0,
                                &vertexBlob,
                                &errors);
    release_com(errors);
    if (SUCCEEDED(result)) {
        const char* pixelSource = is_srgb(p.format) ? kBlitPixelShaderDecode : kBlitPixelShaderCopy;
        result = D3DCompile(pixelSource,
                            std::strlen(pixelSource),
                            nullptr,
                            nullptr,
                            nullptr,
                            "main",
                            "ps_4_0",
                            D3DCOMPILE_ENABLE_STRICTNESS,
                            0,
                            &pixelBlob,
                            &errors);
        release_com(errors);
    }
    if (SUCCEEDED(result)) {
        result = device->CreateVertexShader(
            vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &p.blitVertex);
    }
    if (SUCCEEDED(result)) {
        result = device->CreatePixelShader(
            pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &p.blitPixel);
    }
    release_com(vertexBlob);
    release_com(pixelBlob);
    if (SUCCEEDED(result)) {
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        result = device->CreateSamplerState(&sampler, &p.blitSampler);
    }
    if (SUCCEEDED(result)) {
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        result = device->CreateRasterizerState(&rasterizer, &p.blitRasterizer);
    }
    if (SUCCEEDED(result)) {
        D3D11_TEXTURE2D_DESC staging = source;
        staging.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        staging.Usage = D3D11_USAGE_DEFAULT;
        staging.CPUAccessFlags = 0;
        staging.MiscFlags = 0;
        staging.MipLevels = 1;
        result = device->CreateTexture2D(&staging, nullptr, &p.blitSource);
    }
    if (SUCCEEDED(result)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = typed_view_format(source.Format);
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        result = device->CreateShaderResourceView(p.blitSource, &view, &p.blitView);
    }
    for (std::uint32_t index = 0; SUCCEEDED(result) && index < p.imageCount; ++index) {
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = p.format;
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        result = device->CreateRenderTargetView(p.images[index], &view, &p.targets[index]);
    }
    if (SUCCEEDED(result)) {
        ID3D11Device1* device1 = nullptr;
        result = device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
        if (SUCCEEDED(result)) {
            const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
            D3D_FEATURE_LEVEL selected{};
            result = device1->CreateDeviceContextState(0,
                                                       &level,
                                                       1,
                                                       D3D11_SDK_VERSION,
                                                       __uuidof(ID3D11Device),
                                                       &selected,
                                                       &p.blitState);
            release_com(device1);
        }
    }
    if (FAILED(result)) {
        log_fmt("ev=vr.xr blit result=fail hr=0x%08lX", static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

/**
 * Creates the runtime swapchain for a back buffer, rebuilding it when the back buffer changes.
 * The image is the back buffer's size and, when the runtime allows it, a format CopyResource can
 * take the back buffer's bits into. An sRGB-typed target is preferred: the desktop image is
 * sRGB-encoded, and that type tells the compositor so.
 */
[[nodiscard]] bool ensure_presentation(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source) noexcept {
    Presentation& p = g_presentation;
    if (p.failed) {
        return false;
    }
    if (p.swapchain != XR_NULL_HANDLE && p.width == source.Width && p.height == source.Height
        && p.sourceFormat == source.Format) {
        return true;
    }
    destroy_presentation();
    if (source.SampleDesc.Count > 1) {
        log_line("ev=vr.xr swapchain result=fail reason=msaa_back_buffer");
        p.failed = true;
        return false;
    }
    std::array<std::int64_t, 32> offered{};
    std::uint32_t offeredCount = 0;
    if (g_api.enumerateSwapchainFormats(
            g_session, static_cast<std::uint32_t>(offered.size()), &offeredCount, offered.data())
        != XR_SUCCESS) {
        offeredCount = 0;
    }
    const auto offers = [&](DXGI_FORMAT format) noexcept {
        for (std::uint32_t index = 0; index < offeredCount; ++index) {
            if (offered[index] == static_cast<std::int64_t>(format)) {
                return true;
            }
        }
        return false;
    };
    DXGI_FORMAT chosen = DXGI_FORMAT_UNKNOWN;
    bool direct = false;
    if (is_rgba8_family(source.Format) && offers(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
        chosen = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        direct = true;
    } else if (is_bgra8_family(source.Format) && offers(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
        chosen = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        direct = true;
    } else {
        constexpr std::array<DXGI_FORMAT, 4> candidates{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                                        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                                        DXGI_FORMAT_R8G8B8A8_UNORM,
                                                        DXGI_FORMAT_B8G8R8A8_UNORM};
        for (const DXGI_FORMAT candidate : candidates) {
            if (offers(candidate)) {
                chosen = candidate;
                break;
            }
        }
    }
    if (chosen == DXGI_FORMAT_UNKNOWN) {
        log_fmt("ev=vr.xr swapchain result=fail reason=no_format offered=%u", offeredCount);
        p.failed = true;
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = static_cast<std::int64_t>(chosen);
    info.sampleCount = 1;
    info.width = source.Width;
    info.height = source.Height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (g_api.createSwapchain(g_session, &info, &p.swapchain) != XR_SUCCESS
        || p.swapchain == XR_NULL_HANDLE) {
        log_line("ev=vr.xr swapchain result=fail reason=create");
        p.failed = true;
        return false;
    }
    std::array<XrSwapchainImageD3D11KHR, kMaxSwapchainImages> images{};
    for (auto& image : images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    std::uint32_t imageCount = 0;
    if (g_api.enumerateSwapchainImages(p.swapchain,
                                       kMaxSwapchainImages,
                                       &imageCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))
            != XR_SUCCESS
        || imageCount == 0 || imageCount > kMaxSwapchainImages) {
        log_line("ev=vr.xr swapchain result=fail reason=images");
        destroy_presentation();
        p.failed = true;
        return false;
    }
    for (std::uint32_t index = 0; index < imageCount; ++index) {
        p.images[index] = images[index].texture;
    }
    p.imageCount = imageCount;
    p.width = source.Width;
    p.height = source.Height;
    p.sourceFormat = source.Format;
    p.format = chosen;
    p.copyDirect = direct;
    if (!direct && !prepare_blit(device, source)) {
        destroy_presentation();
        p.failed = true;
        return false;
    }
    log_fmt("ev=vr.xr swapchain result=ok size=%ux%u source_format=%d format=%d direct=%d images=%u",
            p.width,
            p.height,
            static_cast<int>(source.Format),
            static_cast<int>(chosen),
            direct ? 1 : 0,
            imageCount);
    return true;
}

/** Draws the back buffer copy into one swapchain image inside a swapped-in context state. */
[[nodiscard]] bool blit(ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer, std::uint32_t index) noexcept {
    Presentation& p = g_presentation;
    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                       reinterpret_cast<void**>(&context1)))
        || context1 == nullptr) {
        return false;
    }
    context->CopyResource(p.blitSource, backBuffer);
    ID3DDeviceContextState* previous = nullptr;
    context1->SwapDeviceContextState(p.blitState, &previous);
    const D3D11_VIEWPORT viewport{
        0.0F, 0.0F, static_cast<float>(p.width), static_cast<float>(p.height), 0.0F, 1.0F};
    ID3D11RenderTargetView* target = p.targets[index];
    ID3D11ShaderResourceView* view = p.blitView;
    ID3D11SamplerState* sampler = p.blitSampler;
    context->RSSetViewports(1, &viewport);
    context->RSSetState(p.blitRasterizer);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(p.blitVertex, nullptr, 0);
    context->PSSetShader(p.blitPixel, nullptr, 0);
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, &sampler);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context1->SwapDeviceContextState(previous, nullptr);
    release_com(previous);
    release_com(context1);
    return true;
}

/**
 * Copies the presented back buffer into the runtime swapchain and fills the projection views.
 * @return True when an image was acquired, filled and released, so a layer may reference it.
 */
[[nodiscard]] bool submit_back_buffer(IDXGISwapChain* swapChain,
                                      std::span<XrCompositionLayerProjectionView, kViewCount> views) noexcept {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))
        || backBuffer == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backBuffer->GetDevice(&device);
    if (device != nullptr) {
        device->GetImmediateContext(&context);
    }
    bool submitted = false;
    if (device != nullptr && context != nullptr && ensure_presentation(device, description)) {
        Presentation& p = g_presentation;
        std::uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (g_api.acquireSwapchainImage(p.swapchain, &acquire, &index) == XR_SUCCESS
            && index < p.imageCount) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = kImageWaitNanoseconds;
            if (g_api.waitSwapchainImage(p.swapchain, &wait) == XR_SUCCESS) {
                if (p.copyDirect) {
                    context->CopyResource(p.images[index], backBuffer);
                    submitted = true;
                } else {
                    submitted = blit(context, backBuffer, index);
                }
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            g_api.releaseSwapchainImage(p.swapchain, &release);
        }
        if (submitted) {
            // The engine's frustum is symmetric: this horizontal FOV at the back buffer's aspect.
            float horizontal = g_renderedFov.load(std::memory_order_relaxed);
            if (!(horizontal > 0.0F && horizontal < 3.1F)) {
                horizontal = 1.5708F;
            }
            const float halfHorizontal = horizontal * 0.5F;
            const float halfVertical = std::atan(
                std::tan(halfHorizontal) * static_cast<float>(p.height) / static_cast<float>(p.width));
            for (std::uint32_t eye = 0; eye < kViewCount; ++eye) {
                XrCompositionLayerProjectionView& view = views[eye];
                view = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose = g_renderedViews[eye].pose;
                view.fov = XrFovf{-halfHorizontal, halfHorizontal, halfVertical, -halfVertical};
                view.subImage.swapchain = p.swapchain;
                view.subImage.imageRect.offset = XrOffset2Di{0, 0};
                view.subImage.imageRect.extent =
                    XrExtent2Di{static_cast<std::int32_t>(p.width), static_cast<std::int32_t>(p.height)};
                view.subImage.imageArrayIndex = 0;
            }
        }
    }
    release_com(context);
    release_com(device);
    release_com(backBuffer);
    return submitted;
}

/** Turns a vector by a quaternion. */
[[nodiscard]] XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) noexcept {
    // v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f inner{u.y * v.z - u.z * v.y + q.w * v.x,
                           u.z * v.x - u.x * v.z + q.w * v.y,
                           u.x * v.y - u.y * v.x + q.w * v.z};
    return XrVector3f{v.x + 2.0F * (u.y * inner.z - u.z * inner.y),
                      v.y + 2.0F * (u.z * inner.x - u.x * inner.z),
                      v.z + 2.0F * (u.x * inner.y - u.y * inner.x)};
}

/**
 * Maps a vector out of OpenXR's basis into the game's.
 * OpenXR: +X right, +Y up, -Z forward. Game: X forward, Z up, and therefore +Y left.
 */
[[nodiscard]] Vector to_game(const XrVector3f& v) noexcept {
    return Vector{-v.z, -v.x, v.y};
}

/** Turns a vector about the game's up axis. */
void yaw_about_up(Vector& v, float sine, float cosine) noexcept {
    const float x = v[0];
    const float y = v[1];
    v[0] = x * cosine - y * sine;
    v[1] = x * sine + y * cosine;
}

/** Drains the event queue, driving the session through its state machine. */
void pump_events() noexcept {
    for (;;) {
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        const XrResult result = g_api.pollEvent(g_instance, &event);
        if (result != XR_SUCCESS) {
            return;
        }
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            continue;
        }
        const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
        g_sessionState = changed->state;
        if (g_sessionState == XR_SESSION_STATE_READY && !g_sessionRunning) {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (g_api.beginSession(g_session, &begin) == XR_SUCCESS) {
                g_sessionRunning = true;
                log_line("ev=vr.xr session state=running");
            }
        } else if (g_sessionState == XR_SESSION_STATE_STOPPING && g_sessionRunning) {
            // Normal: the game window lost focus, or the headset came off. Begin again on READY.
            g_api.endSession(g_session);
            g_sessionRunning = false;
            log_line("ev=vr.xr session state=stopped");
        }
    }
}

} // namespace

/** @return True while a session is running and poses are being produced. */
bool active() noexcept {
    return g_sessionRunning;
}

/** @return Why the runtime is or is not running. */
Status status() noexcept {
    return g_status;
}

/** @return A short, stable name for the runtime in use. */
const char* runtime_name() noexcept {
    return g_runtimeName[0] != '\0' ? g_runtimeName.data() : "none";
}

/** Takes the next head position as the origin. */
void recentre() noexcept {
    g_recentreRequested = true;
}

/** Brings up the runtime, instance, system and session on the game's device. */
bool initialize(ID3D11Device* device) noexcept {
    if (g_status == Status::running) {
        return true;
    }
    if (g_attempted || device == nullptr) {
        return false;
    }
    g_attempted = true;
    if (!load_runtime() || !bind_global()) {
        return false;
    }
    std::array<const char*, 1> extensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.enabledExtensionNames = extensions.data();
    create.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    std::snprintf(create.applicationInfo.applicationName,
                  sizeof create.applicationInfo.applicationName,
                  "Sunrise VR");
    std::snprintf(create.applicationInfo.engineName,
                  sizeof create.applicationInfo.engineName,
                  "Tiger");
    if (g_api.createInstance(&create, &g_instance) != XR_SUCCESS
        || g_instance == XR_NULL_HANDLE) {
        log_line("ev=vr.xr instance result=fail");
        g_status = Status::instanceFailed;
        return false;
    }
    if (!bind_instance()) {
        g_status = Status::instanceFailed;
        return false;
    }
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (g_api.getInstanceProperties(g_instance, &properties) == XR_SUCCESS) {
        std::snprintf(
            g_runtimeName.data(), g_runtimeName.size(), "%s", properties.runtimeName);
    }
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (g_api.getSystem(g_instance, &systemInfo, &g_system) != XR_SUCCESS) {
        log_line("ev=vr.xr system result=fail reason=no_hmd");
        g_status = Status::systemFailed;
        return false;
    }
    // Required before a D3D11 session, and it is where a real runtime reports the adapter it
    // wants. A mismatch against the game's device is the thing to suspect on a hybrid laptop.
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (g_api.getD3D11Requirements(g_instance, g_system, &requirements) != XR_SUCCESS) {
        log_line("ev=vr.xr requirements result=fail");
        g_status = Status::sessionFailed;
        return false;
    }
    std::array<XrViewConfigurationView, kViewCount> views{};
    for (auto& view : views) {
        view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    std::uint32_t viewCount = 0;
    if (g_api.enumerateViewConfigurationViews(g_instance,
                                              g_system,
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              kViewCount,
                                              &viewCount,
                                              views.data())
        != XR_SUCCESS) {
        log_line("ev=vr.xr views result=fail");
        g_status = Status::sessionFailed;
        return false;
    }
    log_fmt("ev=vr.xr views count=%u eye=%ux%u runtime=%s",
            viewCount,
            views[0].recommendedImageRectWidth,
            views[0].recommendedImageRectHeight,
            runtime_name());
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g_system;
    if (g_api.createSession(g_instance, &sessionInfo, &g_session) != XR_SUCCESS
        || g_session == XR_NULL_HANDLE) {
        log_line("ev=vr.xr session result=fail");
        g_status = Status::sessionFailed;
        return false;
    }
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0F;
    if (g_api.createReferenceSpace(g_session, &spaceInfo, &g_space) != XR_SUCCESS) {
        log_line("ev=vr.xr space result=fail");
        g_status = Status::sessionFailed;
        return false;
    }
    // The controllers are optional: head tracking runs with or without them.
    (void)create_actions();
    g_status = Status::running;
    log_fmt("ev=vr.xr init result=ok runtime=%s", runtime_name());
    return true;
}

/** Runs one OpenXR frame and publishes the pose. */
void begin_frame(float bodyYaw) noexcept {
    if (g_status != Status::running) {
        return;
    }
    pump_events();
    if (!g_sessionRunning || g_frameOpen) {
        return;
    }
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (g_api.waitFrame(g_session, &waitInfo, &frameState) != XR_SUCCESS) {
        return;
    }
    g_predictedDisplayTime = frameState.predictedDisplayTime;
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (g_api.beginFrame(g_session, &beginInfo) != XR_SUCCESS) {
        return;
    }
    g_frameOpen = true;
    g_shouldRender = frameState.shouldRender != XR_FALSE;
    sync_actions();
    if (!g_shouldRender) {
        return;
    }
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = g_predictedDisplayTime;
    locate.space = g_space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    std::array<XrView, kViewCount> views{};
    for (auto& view : views) {
        view.type = XR_TYPE_VIEW;
    }
    std::uint32_t count = 0;
    if (g_api.locateViews(g_session, &locate, &viewState, kViewCount, &count, views.data())
            != XR_SUCCESS
        || count < kViewCount) {
        return;
    }
    const bool tracked = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0
                         && (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    if (!tracked) {
        return;
    }
    // What the camera hook renders next is drawn from these; the layer for that image needs them.
    g_pendingViews = views;
    g_havePending = true;
    // The head sits midway between the eyes.
    const XrPosef& left = views[0].pose;
    const XrPosef& right = views[1].pose;
    const XrVector3f centre{(left.position.x + right.position.x) * 0.5F,
                            (left.position.y + right.position.y) * 0.5F,
                            (left.position.z + right.position.z) * 0.5F};
    if (g_recentreRequested || !g_haveOrigin) {
        g_origin = centre;
        g_haveOrigin = true;
        g_recentreRequested = false;
        log_fmt("ev=vr.xr recentre x=%.3f y=%.3f z=%.3f", centre.x, centre.y, centre.z);
    }
    const XrVector3f forwardXr = rotate(left.orientation, XrVector3f{0.0F, 0.0F, -1.0F});
    const XrVector3f upXr = rotate(left.orientation, XrVector3f{0.0F, 1.0F, 0.0F});
    const XrVector3f delta{(centre.x - g_origin.x) * g_unitsPerMetre,
                           (centre.y - g_origin.y) * g_unitsPerMetre,
                           (centre.z - g_origin.z) * g_unitsPerMetre};

    HeadPose pose{};
    pose.forward = to_game(forwardXr);
    pose.up = to_game(upXr);
    pose.offset = to_game(delta);
    // Fold in the body yaw so mouse look still turns the body and the head adds on top.
    const float sine = std::sin(bodyYaw);
    const float cosine = std::cos(bodyYaw);
    yaw_about_up(pose.forward, sine, cosine);
    yaw_about_up(pose.up, sine, cosine);
    yaw_about_up(pose.offset, sine, cosine);

    // The engine's frustum field is a single symmetric horizontal FOV, so the eyes' asymmetric
    // frusta have to be covered by the smallest symmetric one that contains both.
    float halfHorizontal = 0.0F;
    float halfVertical = 0.0F;
    for (std::uint32_t index = 0; index < kViewCount; ++index) {
        const XrFovf& fov = views[index].fov;
        halfHorizontal = (std::max)(halfHorizontal,
                                    (std::max)(std::fabs(fov.angleLeft), std::fabs(fov.angleRight)));
        halfVertical = (std::max)(halfVertical,
                                  (std::max)(std::fabs(fov.angleUp), std::fabs(fov.angleDown)));
    }
    pose.horizontalFov = halfHorizontal * 2.0F;
    const float tangentVertical = std::tan(halfVertical);
    pose.aspect = tangentVertical > 0.0F ? std::tan(halfHorizontal) / tangentVertical : 1.0F;
    pose.frame = ++g_frameIndex;
    pose.valid = true;

    AcquireSRWLockExclusive(&g_poseLock);
    g_pose = pose;
    ReleaseSRWLockExclusive(&g_poseLock);

    if ((g_frameIndex % 180) == 0) {
        log_fmt("ev=vr.xr pose fwd=%.3f,%.3f,%.3f off=%.3f,%.3f,%.3f fov=%.4f aspect=%.3f",
                pose.forward[0],
                pose.forward[1],
                pose.forward[2],
                pose.offset[0],
                pose.offset[1],
                pose.offset[2],
                pose.horizontalFov,
                pose.aspect);
    }
}

/** Ends the OpenXR frame opened by begin_frame, submitting the presented image when it can. */
void end_frame(IDXGISwapChain* swapChain) noexcept {
    if (g_status != Status::running || !g_frameOpen) {
        return;
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, kViewCount> projectionViews{};
    const XrCompositionLayerBaseHeader* layers[1]{};
    std::uint32_t layerCount = 0;
    if (g_shouldRender && swapChain != nullptr && g_haveRendered
        && submit_back_buffer(swapChain, projectionViews)) {
        layer.space = g_space;
        layer.viewCount = kViewCount;
        layer.views = projectionViews.data();
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        layerCount = 1;
    }
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = g_predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount != 0 ? layers : nullptr;
    const XrResult result = g_api.endFrame(g_session, &endInfo);
    g_frameOpen = false;
    // The frame the game draws next uses the views located this frame.
    if (g_havePending) {
        g_renderedViews = g_pendingViews;
        g_haveRendered = true;
    }
    if ((g_frameIndex % kSubmitReportPeriod) == 0 || result != XR_SUCCESS) {
        log_fmt("ev=vr.xr submit layers=%u result=%d fov=%.4f",
                layerCount,
                static_cast<int>(result),
                g_renderedFov.load(std::memory_order_relaxed));
    }
}

/** @return The most recently published pose. */
HeadPose head_pose() noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const HeadPose pose = g_pose;
    ReleaseSRWLockShared(&g_poseLock);
    return pose;
}

/** @return The controllers as of the last sync. */
InputState input_state() noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const InputState input = g_input;
    ReleaseSRWLockShared(&g_poseLock);
    return input;
}

/** Records the horizontal FOV the engine actually rendered with this frame. */
void note_rendered_fov(float horizontalFov) noexcept {
    g_renderedFov.store(horizontalFov, std::memory_order_relaxed);
}

/** Tears the session and instance down and unloads the runtime. */
void shutdown() noexcept {
    destroy_presentation();
    if (g_actions.set != XR_NULL_HANDLE && g_api.destroyActionSet != nullptr) {
        g_api.destroyActionSet(g_actions.set);
    }
    g_actions = Actions{};
    g_havePending = false;
    g_haveRendered = false;
    g_shouldRender = false;
    AcquireSRWLockExclusive(&g_poseLock);
    g_input = InputState{};
    g_pose = HeadPose{};
    ReleaseSRWLockExclusive(&g_poseLock);
    if (g_session != XR_NULL_HANDLE) {
        if (g_sessionRunning && g_api.endSession != nullptr) {
            g_api.endSession(g_session);
        }
        if (g_space != XR_NULL_HANDLE && g_api.destroySpace != nullptr) {
            g_api.destroySpace(g_space);
        }
        if (g_api.destroySession != nullptr) {
            g_api.destroySession(g_session);
        }
    }
    if (g_instance != XR_NULL_HANDLE && g_api.destroyInstance != nullptr) {
        g_api.destroyInstance(g_instance);
    }
    if (g_runtimeModule != nullptr) {
        FreeLibrary(g_runtimeModule);
    }
    g_runtimeModule = nullptr;
    g_api = Api{};
    g_instance = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_space = XR_NULL_HANDLE;
    g_system = XR_NULL_SYSTEM_ID;
    g_sessionRunning = false;
    g_frameOpen = false;
    g_status = Status::off;
    g_attempted = false;
    g_haveOrigin = false;
    g_recentreRequested = true;
}

} // namespace sunrise::client::hooks::vr::xr
