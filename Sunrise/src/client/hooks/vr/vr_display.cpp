/**
 * The engine renders at the headset's shape and density while VR is on. See vr_display.h and
 * docs/DISPLAY.md.
 */

#include "vr_display.h"

#include <Windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../graphics/renderer/world_lines.h"
#include "vr_gpu.h"
#include "vr_settings.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::display {
namespace {

/**
 * The engine's aspect-ratio setting is a singleton (getter +0xDD3740, object +0x2808578). Its views
 * read `[this + 0x20]` in "auto" mode -- `GetAspect()` (+0x121F580), a variant (+0x12215D0) and the
 * letterbox rectangles (+0x1221670) -- and the projection and the letterbox are built from it:
 *
 *     mode = [this + 8]  (0 counts as 3)
 *     mode 1 : 16/9          mode 2 : 2.4          mode 3 (auto) : [this + 0x20]
 *
 * `Update(size)` (+0x12218F0) rewrites `[this + 0x20]` every frame through a wrapper (+0x3C4360)
 * round `SnapAspect(width, height, float* screen, float* render)` (+0x3C41C0), which does not compute
 * width / height: it looks it up in a two-entry table (+0x1BD1DB0) of {screen range -> render aspect}:
 *
 *     screen 1.6 .. 7.17 (16:10 .. 129:18)  ->  render = screen
 *     screen 1.25 .. 1.33 (5:4 .. 4:3)      ->  render 1.6
 *
 * outside both ranges it takes the nearest endpoint. So a 4:3 monitor gets a 16:10 render letterboxed
 * on it, and a 0.96 buffer snaps to "5:4 screen, 16:10 render". The wrapper keeps only `render` (for
 * the projection); two more callers use `screen` to size the final composite's letterbox (+0x3BDCE0,
 * +0x3C4A30, behind a flag at +0x1F89E06). Replacing only the wrapper's answer gave the right
 * projection squeezed into a 5:4-derived band; replacing only `screen` gave a 16:10 projection in a
 * band. While the render is eye-shaped, both answers are the true ratio. Read from the decrypted image
 * and confirmed live, 2026-10-15/16.
 */
constexpr std::uintptr_t kSnapAspectRva = 0x3C41C0;
constexpr std::array<std::uint8_t, 10> kSnapAspectPrologue{0x40, 0x53, 0x48, 0x83, 0xEC,
                                                           0x60, 0xF3, 0x0F, 0x10, 0x25};

/** IDXGISwapChain::ResizeBuffers is slot 13: 3 IUnknown, 4 IDXGIObject, 1 IDXGIDeviceSubObject, then 5 more. */
constexpr std::size_t kResizeBuffersSlot = 13;

constexpr wchar_t kCommandName[] = L"SVR_Display.txt";
constexpr std::uint32_t kPollPeriod = 30;
/** Render sizes are kept to multiples of this: the engine's own targets are tiled. */
constexpr std::uint32_t kSizeAlign = 8;
/** Below this the shape is not worth changing: a rounding difference, not a different image. */
constexpr std::uint32_t kSizeSlack = 8;
/**
 * More resizes than this inside kStormWindowMs means the engine is fighting the size it is handed
 * (comparing its back buffer against the window every frame, say). Giving up is the safe answer: a
 * resize every frame recreates every render target the engine owns.
 */
constexpr int kStormResizes = 6;
constexpr ULONGLONG kStormWindowMs = 3000;
/** Presents to wait for the engine to answer a posted WM_SIZE before nudging the window instead. */
constexpr std::uint32_t kAnswerPresents = 45;

using SnapFn = void (*)(unsigned, unsigned, float*, float*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

std::array<hooking::detour::Handle, 2> g_handles{};
std::atomic_bool g_installed{false};
bool g_installTried{false};

/**
 * The UI's canvas is never narrower than this (16:10): anything taller gets the canvas as a centred
 * band, with the composited world cropped to it (vr_gpu.cpp, "the UI's scene composite").
 */
constexpr float kUiMinAspect = 1.6F;

/** What the user asked for. */
std::atomic_bool g_wantEye{true};
/** Fill the eye outside the UI's canvas from the composite's input (canvas_band). */
std::atomic_bool g_fill{true};
/**
 * Density relative to the headset's recommended size. 0.8 by default, measured on this machine (RTX 3060
 * Laptop, mock at a Quest 3's geometry, 2026-10-16): at 1.0 (2264x2352) the GPU is at 100 % and the game
 * runs 53-56 fps; at 0.8 (1816x1880) it reaches the CPU's own limit, 71-72 fps, with the GPU at 68 % --
 * a Quest 3 at 72 Hz held, and room left for Link's encoder. Frame rate matters more than sharpness in
 * a headset; `scale 1.0` in SVR_Display.txt brings the full density back.
 */
std::atomic<float> g_scale{0.8F};
std::atomic<std::uint32_t> g_fixedWidth{0};
std::atomic<std::uint32_t> g_fixedHeight{0};
std::atomic_bool g_infoNow{false};

/** What the resize hook substitutes, published by the present thread. 0 = leave the call alone. */
std::atomic<std::uint32_t> g_targetWidth{0};
std::atomic<std::uint32_t> g_targetHeight{0};
std::atomic_bool g_unclamp{false};
std::atomic<IDXGISwapChain*> g_mainSwapChain{nullptr};
std::atomic<std::uint32_t> g_resizes{0};
std::atomic<std::uint32_t> g_aspectCalls{0};

/** Present thread state. */
std::uint32_t g_presents{0};
/** The window's size before a nudge, put back a few presents later (restore_nudge); 0 = none pending. */
int g_restoreWidth{0};
int g_restoreHeight{0};
std::uint32_t g_restoreAt{0};
std::uint32_t g_appliedWidth{0};
std::uint32_t g_appliedHeight{0};
bool g_requested{false};
std::uint32_t g_requestedAt{0};
std::uint32_t g_resizesAtRequest{0};
bool g_nudged{false};
bool g_gaveUp{false};
std::array<ULONGLONG, kStormResizes> g_resizeTimes{};
std::size_t g_resizeTimeNext{0};
std::uint32_t g_resizesSeen{0};

template <typename... Args> void reportf(const char* format, Args... args) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(), line.size(), format, args...);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size() ? static_cast<std::size_t>(written)
                                                                            : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::warn, {line.data(), length});
    }
}

/** The last snapped and true aspects, for the report. */
std::atomic<float> g_snapped{0.0F};
std::atomic<float> g_trueAspect{0.0F};

/**
 * Who asks SnapAspect what, for the report: the distinct (caller, width, height) seen, with counts.
 * The aspect is only right if every caller hands in the size of the image it is about to shape.
 */
struct SnapCaller final {
    std::atomic<std::uintptr_t> rva{0};
    std::atomic<std::uint32_t> width{0};
    std::atomic<std::uint32_t> height{0};
    std::atomic<std::uint32_t> count{0};
};
std::array<SnapCaller, 8> g_snapCallers{};

void note_snap_caller(std::uintptr_t rva, unsigned width, unsigned height) noexcept {
    for (SnapCaller& c : g_snapCallers) {
        if (c.rva.load(std::memory_order_relaxed) == rva && c.width.load(std::memory_order_relaxed) == width
            && c.height.load(std::memory_order_relaxed) == height) {
            c.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    for (SnapCaller& c : g_snapCallers) {
        std::uintptr_t expected = 0;
        if (c.rva.compare_exchange_strong(expected, rva, std::memory_order_relaxed)) {
            c.width.store(width, std::memory_order_relaxed);
            c.height.store(height, std::memory_order_relaxed);
            c.count.store(1, std::memory_order_relaxed);
            return;
        }
    }
}

void snap_replacement(unsigned width, unsigned height, float* out, float* second) noexcept {
    g_aspectCalls.fetch_add(1, std::memory_order_relaxed);
    note_snap_caller(reinterpret_cast<std::uintptr_t>(_ReturnAddress())
                         - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),
                     width, height);
    const auto original = reinterpret_cast<SnapFn>(g_handles[0].original);
    if (original != nullptr) {
        original(width, height, out, second);
    }
    if (out == nullptr || height == 0) {
        return;
    }
    g_snapped.store(second != nullptr ? *second : *out, std::memory_order_relaxed);
    // The size handed in is the render's own (the back buffer the resize hook shaped), so its ratio
    // is exactly the frustum the layer will declare. BOTH answers are replaced: the screen aspect
    // (`out`, what the letterbox callers size against) and the render aspect (`second`, what the
    // wrapper hands Update() for the projection). Replacing only one is either a 16:10 projection or
    // a 16:10 band.
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    g_trueAspect.store(aspect, std::memory_order_relaxed);
    if (g_unclamp.load(std::memory_order_acquire) && aspect > 0.2F && aspect < 5.0F) {
        *out = aspect;
        if (second != nullptr) {
            *second = aspect;
        }
    }
}

HRESULT STDMETHODCALLTYPE resize_replacement(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height,
                                             DXGI_FORMAT format, UINT flags) noexcept {
    const auto original = reinterpret_cast<ResizeFn>(g_handles[1].original);
    // DXGI refuses to resize while anything still references the back buffer (INVALID_CALL, which
    // the engine then retries every frame). Two things of ours do once a debug overlay has been drawn:
    // the cached view of it, and world_lines' private device-context state, which keeps the last
    // render target it drew into BOUND after swapping back. Both are dropped here; both are rebuilt on
    // the next overlay draw.
    gpu::release_back_buffer_view();
    graphics::renderer::world_lines::release();
    UINT w = width;
    UINT h = height;
    if (swapChain != nullptr && swapChain == g_mainSwapChain.load(std::memory_order_acquire)) {
        const std::uint32_t tw = g_targetWidth.load(std::memory_order_acquire);
        const std::uint32_t th = g_targetHeight.load(std::memory_order_acquire);
        if (tw != 0 && th != 0) {
            w = tw;
            h = th;
        }
        g_resizes.fetch_add(1, std::memory_order_acq_rel);
    }
    const HRESULT result = original != nullptr ? original(swapChain, count, w, h, format, flags) : E_FAIL;
    if (swapChain != nullptr && swapChain == g_mainSwapChain.load(std::memory_order_acquire)) {
        reportf("ev=vr.display resize asked=%ux%u given=%ux%u result=0x%08lX", width, height, w, h,
                static_cast<unsigned long>(result));
    }
    return result;
}

template <std::size_t N>
bool prologue_matches(std::uintptr_t address, const std::array<std::uint8_t, N>& expected) noexcept {
    std::array<std::uint8_t, N> actual{};
    SIZE_T read = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), actual.data(), N, &read) == FALSE
        || read != N) {
        return false;
    }
    return actual == expected;
}

void install(IDXGISwapChain* swapChain) noexcept {
    if (g_installTried || swapChain == nullptr) {
        return;
    }
    g_installTried = true;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const std::uintptr_t aspect = base + kSnapAspectRva;
    if (base == 0 || !prologue_matches(aspect, kSnapAspectPrologue)) {
        reportf("ev=vr.display install result=fail reason=aspect_prologue");
        return;
    }
    void** const table = *reinterpret_cast<void***>(swapChain);
    void* const resize = table[kResizeBuffersSlot];
    const std::array<hooking::detour::Spec, 2> specs{
        hooking::detour::Spec{reinterpret_cast<void*>(aspect), reinterpret_cast<void*>(&snap_replacement)},
        hooking::detour::Spec{resize, reinterpret_cast<void*>(&resize_replacement)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        reportf("ev=vr.display install result=fail reason=detour");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    reportf("ev=vr.display install result=ok snap_aspect=0x%llX", static_cast<unsigned long long>(kSnapAspectRva));
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

/**
 * The player's choice of shape (`display_shape` in SVR_Settings.txt), read once at the first present.
 * `desktop` keeps the desktop window undistorted, for recording, at the cost of an image that no longer
 * fills the eye; SVR_Display.txt's `shape` still overrides it live. SVR_DISPLAY_SHAPE in the environment
 * is the test harness's (Enable-MockXr): it reaches only a game that shell launches, so the batteries keep
 * the shape they were written for whatever the player's file says.
 */
void apply_settings() noexcept {
    static bool applied = false;
    if (applied) {
        return;
    }
    applied = true;
    std::array<char, 16> env{};
    const DWORD envLength = GetEnvironmentVariableA("SVR_DISPLAY_SHAPE", env.data(), static_cast<DWORD>(env.size()));
    const bool fromEnv = envLength != 0 && envLength < env.size();
    const char* shape = fromEnv ? env.data() : settings::value("display_shape", "eye");
    g_wantEye.store(std::strcmp(shape, "desktop") != 0, std::memory_order_relaxed);
    // The render's density (`scale` in SVR_Display.txt, live); the harness keeps the batteries' 0.8.
    if (!fromEnv) {
        g_scale.store(settings::number("display_scale", 0.8F, 0.3F, 2.0F), std::memory_order_relaxed);
    }
    reportf("ev=vr.display shape=%s scale=%.2f source=%s", g_wantEye.load() ? "eye" : "desktop", g_scale.load(),
            fromEnv ? "env" : "settings");
}

void poll_commands() noexcept {
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
        const int n = sscanf_s(line.data(), "%15s %31s", verb, static_cast<unsigned>(sizeof verb), a,
                               static_cast<unsigned>(sizeof a));
        if (n < 1) {
            continue;
        }
        if (std::strcmp(verb, "shape") == 0 && n >= 2) {
            g_wantEye.store(std::strcmp(a, "desktop") != 0, std::memory_order_relaxed);
            g_gaveUp = false;
            reportf("ev=vr.display shape=%s", g_wantEye.load() ? "eye" : "desktop");
        } else if (std::strcmp(verb, "scale") == 0) {
            float v = 0.0F;
            if (sscanf_s(line.data(), "%*s %f", &v) == 1 && v >= 0.25F && v <= 2.0F) {
                g_scale.store(v, std::memory_order_relaxed);
                reportf("ev=vr.display scale=%.3f", v);
            }
        } else if (std::strcmp(verb, "size") == 0) {
            unsigned w = 0;
            unsigned h = 0;
            if (sscanf_s(line.data(), "%*s %u %u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 8192 && h <= 8192) {
                g_fixedWidth.store(w, std::memory_order_relaxed);
                g_fixedHeight.store(h, std::memory_order_relaxed);
            } else {
                g_fixedWidth.store(0, std::memory_order_relaxed);
                g_fixedHeight.store(0, std::memory_order_relaxed);
            }
            reportf("ev=vr.display size=%ux%u", g_fixedWidth.load(), g_fixedHeight.load());
        } else if (std::strcmp(verb, "stereo") == 0 && n >= 2) {
            xr::set_stereo(std::strcmp(a, "on") == 0 || std::strcmp(a, "alternate") == 0);
            reportf("ev=vr.display stereo=%s", xr::stereo() ? "alternate" : "mono");
        } else if (std::strcmp(verb, "fill") == 0 && n >= 2) {
            g_fill.store(std::strcmp(a, "off") != 0, std::memory_order_relaxed);
            reportf("ev=vr.display fill=%s", g_fill.load() ? "on" : "off");
        } else if (std::strcmp(verb, "crop") == 0 && n >= 2) {
            xr::set_per_eye_crop(std::strcmp(a, "off") != 0);
            reportf("ev=vr.display crop=%s", xr::per_eye_crop() ? "per_eye" : "whole");
        } else if (std::strcmp(verb, "lead") == 0 && n >= 2) {
            float ms = -1.0F;
            if (std::strcmp(a, "off") == 0) {
                ms = 0.0F;
            } else if (std::strcmp(a, "auto") != 0 && sscanf_s(a, "%f", &ms) != 1) {
                ms = -1.0F;
            }
            xr::set_lead(ms);
            reportf("ev=vr.display lead=%s ms=%.2f", ms < 0.0F ? "measured" : "fixed", ms);
        } else if (std::strcmp(verb, "probe") == 0) {
            float yaw = 0.0F;
            float scale = 1.0F;
            const int got = sscanf_s(line.data(), "%*s %f %f", &yaw, &scale);
            xr::set_declaration_probe(got >= 1 ? yaw : 0.0F, got >= 2 ? scale : 1.0F);
            reportf("ev=vr.display probe yaw_deg=%.2f fov_scale=%.3f", got >= 1 ? yaw : 0.0F, got >= 2 ? scale : 1.0F);
        } else if (std::strcmp(verb, "info") == 0) {
            g_infoNow.store(true, std::memory_order_relaxed);
        } else {
            reportf("ev=vr.display command result=fail verb=%s", verb);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

std::uint32_t align(float value) noexcept {
    const auto v = static_cast<std::uint32_t>(std::lround(value));
    return (std::max)(kSizeAlign, (v + kSizeAlign / 2) / kSizeAlign * kSizeAlign);
}

/** The size the engine should render at now: 0x0 for "leave it alone". */
void desired(std::uint32_t& width, std::uint32_t& height) noexcept {
    width = 0;
    height = 0;
    // Keyed on the session EXISTING, not on it running: a headset taken off and put back stops and
    // restarts the session, and each of those used to resize the game twice (24 resizes in the
    // 16 Oct session) -- a resize in the middle of a load is exactly what must never happen.
    if (!g_wantEye.load(std::memory_order_relaxed) || g_gaveUp || xr::status() != xr::Status::running) {
        return;
    }
    const std::uint32_t fw = g_fixedWidth.load(std::memory_order_relaxed);
    const std::uint32_t fh = g_fixedHeight.load(std::memory_order_relaxed);
    if (fw != 0 && fh != 0) {
        width = fw;
        height = fh;
        return;
    }
    const xr::EyeGeometry eye = xr::eye_geometry();
    if (!eye.valid) {
        return;
    }
    // One density for both axes -- the larger -- so pixels stay square: the engine's projection has
    // one aspect, and a non-square pixel would be a stretch the layer could not undo.
    const float density = (std::max)(eye.pixelsPerTanX, eye.pixelsPerTanY) * g_scale.load(std::memory_order_relaxed);
    width = (std::min)(align(2.0F * eye.tanHalfWidth * density), 8192U);
    height = (std::min)(align(2.0F * eye.tanHalfHeight * density), 8192U);
}

HWND window_of(IDXGISwapChain* swapChain) noexcept {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (swapChain == nullptr || FAILED(swapChain->GetDesc(&desc))) {
        return nullptr;
    }
    return desc.OutputWindow;
}

bool back_buffer_size(IDXGISwapChain* swapChain, std::uint32_t& width, std::uint32_t& height) noexcept {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (swapChain == nullptr || FAILED(swapChain->GetDesc(&desc))) {
        return false;
    }
    width = desc.BufferDesc.Width;
    height = desc.BufferDesc.Height;
    return true;
}

/**
 * Makes the engine run its resize path, which is what calls ResizeBuffers and recomputes the
 * aspect scales. A WM_SIZE with the window's own client size first; if that is not answered, a
 * one-pixel change of the window and back, ASYNCHRONOUSLY: the window belongs to another thread,
 * and a synchronous SetWindowPos from the render thread can wait on a thread that waits on it.
 */
void request_resize(IDXGISwapChain* swapChain, bool nudge) noexcept {
    const HWND window = window_of(swapChain);
    if (window == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(window, &client);
    if (!nudge) {
        PostMessageW(window, WM_SIZE, SIZE_RESTORED,
                     MAKELPARAM(client.right - client.left, client.bottom - client.top));
        return;
    }
    RECT frame{};
    GetWindowRect(window, &frame);
    const int w = frame.right - frame.left;
    const int h = frame.bottom - frame.top;
    constexpr UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    // One pixel taller now, and the original size put back on a later present (restore_nudge): two
    // asynchronous calls in a row could be coalesced, and the window grew a pixel with every
    // handover (1085 -> 1104 rows over the 16 Oct session).
    if (g_restoreHeight == 0) {
        g_restoreWidth = w;
        g_restoreHeight = h;
    }
    SetWindowPos(window, nullptr, 0, 0, w, h + 1, flags);
    g_restoreAt = g_presents + 3;
}

/** Puts the window back to its size before a nudge, until it is. */
void restore_nudge(IDXGISwapChain* swapChain) noexcept {
    if (g_restoreHeight == 0 || g_presents < g_restoreAt) {
        return;
    }
    const HWND window = window_of(swapChain);
    if (window == nullptr) {
        return;
    }
    RECT frame{};
    GetWindowRect(window, &frame);
    if (frame.right - frame.left == g_restoreWidth && frame.bottom - frame.top == g_restoreHeight) {
        g_restoreHeight = 0;
        return;
    }
    constexpr UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(window, nullptr, 0, 0, g_restoreWidth, g_restoreHeight, flags);
    g_restoreAt = g_presents + 10;
}

void note_resize_storm() noexcept {
    const std::uint32_t total = g_resizes.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    while (g_resizesSeen < total) {
        ++g_resizesSeen;
        g_resizeTimes[g_resizeTimeNext] = now;
        g_resizeTimeNext = (g_resizeTimeNext + 1) % g_resizeTimes.size();
    }
    const ULONGLONG oldest = g_resizeTimes[g_resizeTimeNext];
    if (!g_gaveUp && oldest != 0 && now - oldest < kStormWindowMs && g_targetWidth.load() != 0) {
        g_gaveUp = true;
        reportf("ev=vr.display shape=desktop reason=resize_storm resizes=%d window_ms=%llu", kStormResizes,
                static_cast<unsigned long long>(kStormWindowMs));
    }
}

} // namespace

void present(ID3D11Device* device, IDXGISwapChain* swapChain) noexcept {
    (void)device;
    if (swapChain == nullptr) {
        return;
    }
    ++g_presents;
    g_mainSwapChain.store(swapChain, std::memory_order_release);
    install(swapChain);
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    apply_settings();
    if (g_presents % kPollPeriod == 0) {
        poll_commands();
    }
    note_resize_storm();
    restore_nudge(swapChain);

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    desired(width, height);
    std::uint32_t bw = 0;
    std::uint32_t bh = 0;
    (void)back_buffer_size(swapChain, bw, bh);

    const bool changed = width != g_appliedWidth || height != g_appliedHeight;
    if (changed) {
        // Publish first, then make the engine resize: the hook substitutes whatever is published at
        // the moment the engine calls it. 0x0 restores the stock path (the window's own size).
        g_targetWidth.store(width, std::memory_order_release);
        g_targetHeight.store(height, std::memory_order_release);
        g_unclamp.store(width != 0, std::memory_order_release);
        g_appliedWidth = width;
        g_appliedHeight = height;
        g_requested = true;
        g_nudged = false;
        g_requestedAt = g_presents;
        g_resizesAtRequest = g_resizes.load(std::memory_order_acquire);
        request_resize(swapChain, false);
        reportf("ev=vr.display request target=%ux%u back_buffer=%ux%u", width, height, bw, bh);
    } else if (g_requested) {
        const bool answered = g_resizes.load(std::memory_order_acquire) != g_resizesAtRequest;
        if (answered) {
            g_requested = false;
        } else if (!g_nudged && g_presents - g_requestedAt > kAnswerPresents) {
            g_nudged = true;
            g_requestedAt = g_presents;
            request_resize(swapChain, true);
            reportf("ev=vr.display request=nudge target=%ux%u", width, height);
        } else if (g_nudged && g_presents - g_requestedAt > kAnswerPresents * 4) {
            g_requested = false;
            reportf("ev=vr.display request result=unanswered target=%ux%u back_buffer=%ux%u", width, height, bw, bh);
        }
    }
    if (g_infoNow.exchange(false, std::memory_order_relaxed) || (g_presents % 1800) == 0) {
        const xr::EyeGeometry eye = xr::eye_geometry();
        reportf("ev=vr.display info want=%s target=%ux%u back_buffer=%ux%u unclamp=%d resizes=%u snaps=%u snapped=%.4f true=%.4f "
                "gave_up=%d eye_tan=%.4f,%.4f density=%.1f,%.1f rec=%ux%u scale=%.3f",
                g_wantEye.load() ? "eye" : "desktop", g_targetWidth.load(), g_targetHeight.load(), bw, bh,
                g_unclamp.load() ? 1 : 0, g_resizes.load(), g_aspectCalls.load(), g_snapped.load(), g_trueAspect.load(), g_gaveUp ? 1 : 0,
                eye.tanHalfWidth, eye.tanHalfHeight, eye.pixelsPerTanX, eye.pixelsPerTanY, eye.recommendedWidth,
                eye.recommendedHeight, g_scale.load());
        for (const SnapCaller& c : g_snapCallers) {
            const std::uintptr_t rva = c.rva.load(std::memory_order_relaxed);
            if (rva != 0) {
                reportf("ev=vr.display snap_caller ret=+0x%llX size=%ux%u calls=%u",
                        static_cast<unsigned long long>(rva), c.width.load(), c.height.load(), c.count.load());
            }
        }
    }
}

bool eye_shaped() noexcept {
    return g_unclamp.load(std::memory_order_acquire);
}

bool canvas_band(unsigned width, unsigned height, unsigned& top, unsigned& bottom) noexcept {
    if (!g_fill.load(std::memory_order_relaxed) || !eye_shaped() || width == 0 || height == 0) {
        return false;
    }
    const float canvas = static_cast<float>(width) / kUiMinAspect;
    if (canvas >= static_cast<float>(height) - 1.0F) {
        return false;
    }
    // Inside the band the back buffer is an exact copy of the scene (measured: zero difference), so the
    // band taken from it is pulled in by a couple of rows: whatever way the engine rounds its edges,
    // no row of its black surround can reach the eye.
    constexpr unsigned kMargin = 2;
    const float first = (static_cast<float>(height) - canvas) * 0.5F;
    top = static_cast<unsigned>(std::ceil(first)) + kMargin;
    bottom = static_cast<unsigned>(std::floor(first + canvas)) - kMargin;
    return bottom > top && bottom <= height;
}

void ui_band(unsigned width, unsigned height, unsigned& top, unsigned& bottom) noexcept {
    top = 0;
    bottom = height;
    if (width == 0 || height == 0) {
        return;
    }
    // The canvas is 16:10 at the narrowest: a taller buffer holds it in a centred band, a wider one
    // fills the whole height (and lays the menus out across its width).
    const float canvas = static_cast<float>(width) / kUiMinAspect;
    if (canvas >= static_cast<float>(height) - 1.0F) {
        return;
    }
    const float first = (static_cast<float>(height) - canvas) * 0.5F;
    top = static_cast<unsigned>(std::lround(first));
    bottom = (std::min)(height, static_cast<unsigned>(std::lround(first + canvas)));
}

void* window() noexcept {
    IDXGISwapChain* const swapChain = g_mainSwapChain.load(std::memory_order_acquire);
    return swapChain != nullptr ? static_cast<void*>(window_of(swapChain)) : nullptr;
}

} // namespace sunrise::client::hooks::vr::display
