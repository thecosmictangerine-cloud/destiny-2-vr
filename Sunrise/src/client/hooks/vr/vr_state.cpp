/**
 * The game's state, as far as VR cares (vr_state.h).
 *
 * Signals, all read on the present thread, none of them from a frame of ours:
 *  - the game's own boot-flow step (Sunrise's world-step poll, `bootflow::current_step`): 38 is
 *    `activity:in_world`; orbit is 29, the launch and the load 30-37, character select 27. The poll
 *    runs in the camera hook, which does not run on the title screen or at character select, so
 *    there the step reads -1: a screen, which is right.
 *  - a menu opened in the world: the game confines and recentres the pointer every frame while the
 *    mouse looks around, and lets it go over a menu. `menu` combines the cursor's visibility and its
 *    confinement (measured with `stateprobe`, docs/PLAYABLE.md).
 *  - Sunrise's own interface (Insert): always on the screen, where it can be read and clicked.
 *
 * The world is immersive in first and third person alike (the Tower, a sparrow, a cut scene): the
 * user's decision, 2026-10-17.
 */

#include "vr_state.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../bootflow/bootflow_hook_lifecycle.h"
#include "vr_debug.h"
#include "vr_display.h"
#include "vr_gpu.h"
#include "vr_weapon.h"

namespace sunrise::client::hooks::vr::state {
namespace {

constexpr std::int32_t kInWorld = 38;
/** A screen comes up after this long of its signal; the world comes back after this long of play. */
constexpr double kEnterSeconds = 0.03;
constexpr double kExitSeconds = 0.25;
/** Out of a menu opened in the world: longer, because the Director's opening animation draws the
 *  world again for up to ~0.7 s right after the menu has taken the screen (measured twice on
 *  17 Oct). Just after opening it takes a second of world to close; once the menu has been up a
 *  while, 0.4 s -- closing a menu is not made to wait for an animation that only opening has. */
constexpr double kMenuExitEarlySeconds = 1.0;
constexpr double kMenuExitSeconds = 0.4;
constexpr double kMenuSettleSeconds = 1.5;
double g_viewSince = 0.0;
/** A menu asked for by the controllers is expected this long. */
constexpr double kExpectSeconds = 0.7;

std::atomic<View> g_view{View::screen};
std::atomic<const char*> g_reason{"start"};
std::atomic<std::int32_t> g_step{-1};
std::atomic<std::uint64_t> g_framesInView{0};
std::atomic<double> g_expectUntil{0.0};
std::atomic<float> g_engineFov{0.0F};
/** The world camera's views seen by the GPU module, at the last present, and since when none. */
std::uint64_t g_lastWorldViews = 0;
double g_noWorldSince = -1.0;
/** A screen opened because the world stopped being drawn (a full-screen menu) must not be shut by
 *  the same frame's count: the world comes back only after this long of being drawn again. */
constexpr double kNoWorldSeconds = 0.15;

View g_pending{View::screen};
double g_pendingSince{0.0};
std::uint64_t g_presents{0};

[[nodiscard]] double now_seconds() noexcept {
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

void report(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, text);
}

/** What the pointer says about the game: shown, and confined to a small rectangle or not. */
struct Pointer final {
    bool showing{};
    bool confined{};
    POINT position{};
    RECT clip{};
    RECT client{};
};

[[nodiscard]] Pointer read_pointer() noexcept {
    Pointer p{};
    CURSORINFO info{};
    info.cbSize = sizeof info;
    if (GetCursorInfo(&info) != FALSE) {
        p.showing = (info.flags & CURSOR_SHOWING) != 0;
        p.position = info.ptScreenPos;
    }
    GetClipCursor(&p.clip);
    const int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    p.confined = (p.clip.right - p.clip.left) < screenW - 2 || (p.clip.bottom - p.clip.top) < screenH - 2;
    if (const auto window = static_cast<HWND>(display::window()); window != nullptr) {
        RECT client{};
        GetClientRect(window, &client);
        POINT origin{0, 0};
        ClientToScreen(window, &origin);
        p.client = RECT{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    }
    return p;
}

} // namespace

void tick() noexcept {
    ++g_presents;
    const double now = now_seconds();
    const std::int32_t step = bootflow::current_step();
    g_step.store(step, std::memory_order_relaxed);
    const Pointer pointer = read_pointer();
    const bool sunriseUi = core::ui::runtime::snapshot().visible;
    // A menu that covers the screen (the character screen, the inventory, the Director, ...) stops the
    // world from being drawn: the GPU module sees no world camera at all (measured 2026-10-17: ~356
    // world views a frame in play, 0 under F1, I and M). The overlays drawn OVER the world -- Esc's
    // game options, the Ghost's nav mode -- keep it drawn and stay in the world; the OS pointer says
    // nothing either way (Destiny draws its own and never shows or confines the system one).
    const std::uint64_t worldViews = gpu::world_views();
    const bool worldDrawn = worldViews != g_lastWorldViews;
    g_lastWorldViews = worldViews;
    if (worldDrawn) {
        g_noWorldSince = -1.0;
    } else if (g_noWorldSince < 0.0) {
        g_noWorldSince = now;
    }
    const bool menu = step == kInWorld && g_noWorldSince >= 0.0 && now - g_noWorldSince >= kNoWorldSeconds;

    View want = View::immersive;
    const char* why = "world";
    bool immediate = false;
    if (step != kInWorld) {
        want = View::screen;
        why = step < 0 ? "no_world" : step == 29 ? "orbit" : step < 29 ? "boot" : "travel";
    } else if (sunriseUi) {
        want = View::screen;
        why = "sunrise_ui";
    } else if (menu) {
        want = View::screen;
        why = "menu";
    }
    const double expect = g_expectUntil.load(std::memory_order_relaxed);
    if (expect > 0.0) {
        if (want == View::screen) {
            g_expectUntil.store(0.0, std::memory_order_relaxed);
        } else if (now < expect) {
            want = View::screen;
            why = "menu_expected";
            immediate = true;
        } else {
            g_expectUntil.store(0.0, std::memory_order_relaxed);
            report("ev=vr.state expect=missed");
        }
    }

    const View current = g_view.load(std::memory_order_relaxed);
    if (want != g_pending) {
        g_pending = want;
        g_pendingSince = now;
    }
    const bool fromMenu = current == View::screen && std::strcmp(g_reason.load(std::memory_order_relaxed), "menu") == 0;
    const double menuExit = now - g_viewSince < kMenuSettleSeconds ? kMenuExitEarlySeconds : kMenuExitSeconds;
    const double need = want == View::screen ? kEnterSeconds : fromMenu ? menuExit : kExitSeconds;
    if (want != current && (immediate || now - g_pendingSince >= need)) {
        g_view.store(want, std::memory_order_release);
        g_framesInView.store(0, std::memory_order_relaxed);
        g_viewSince = now;
        std::array<char, 160> line{};
        std::snprintf(line.data(), line.size(), "ev=vr.state view=%s reason=%s step=%d pointer=%d,%d",
                      want == View::screen ? "screen" : "immersive", why, step, pointer.showing ? 1 : 0,
                      pointer.confined ? 1 : 0);
        report(line.data());
    } else {
        g_framesInView.fetch_add(1, std::memory_order_relaxed);
    }
    g_reason.store(g_view.load(std::memory_order_relaxed) == want ? why : g_reason.load(), std::memory_order_relaxed);

    if (debug::has("stateprobe") && (g_presents % 30) == 0) {
        static std::uint64_t lastScene = 0;
        static std::uint64_t lastWeapon = 0;
        static std::uint64_t lastWorld = 0;
        static std::uint64_t lastVm = 0;
        const std::uint64_t scene = gpu::scene_composites();
        const std::uint64_t weaponFrames = weapon::weapon_frames();
        const std::uint64_t worldNow = gpu::world_views();
        const std::uint64_t vmViews = gpu::viewmodel_views();
        std::array<char, 360> line{};
        std::snprintf(line.data(), line.size(),
                      "ev=vr.state probe step=%d scene=%llu weapon=%llu world=%llu vm=%llu showing=%d confined=%d pos=%ld,%ld clip=%ld,%ld,%ld,%ld client=%ld,%ld,%ld,%ld "
                      "sunrise_ui=%d focus=%d view=%s",
                      step, static_cast<unsigned long long>(scene - lastScene), static_cast<unsigned long long>(weaponFrames - lastWeapon),
                      static_cast<unsigned long long>(worldNow - lastWorld), static_cast<unsigned long long>(vmViews - lastVm),
                      pointer.showing ? 1 : 0, pointer.confined ? 1 : 0, pointer.position.x, pointer.position.y,
                      pointer.clip.left, pointer.clip.top, pointer.clip.right, pointer.clip.bottom, pointer.client.left,
                      pointer.client.top, pointer.client.right, pointer.client.bottom, sunriseUi ? 1 : 0,
                      GetForegroundWindow() == static_cast<HWND>(display::window()) ? 1 : 0,
                      g_view.load() == View::screen ? "screen" : "immersive");
        report(line.data());
        lastScene = scene;
        lastWeapon = weaponFrames;
        lastWorld = worldNow;
        lastVm = vmViews;
    }
}

View view() noexcept {
    return g_view.load(std::memory_order_acquire);
}

const char* reason() noexcept {
    return g_reason.load(std::memory_order_relaxed);
}

std::int32_t step() noexcept {
    return g_step.load(std::memory_order_relaxed);
}

void expect_menu() noexcept {
    g_expectUntil.store(now_seconds() + kExpectSeconds, std::memory_order_relaxed);
}

void note_engine_fov(float horizontalFov) noexcept {
    g_engineFov.store(horizontalFov, std::memory_order_relaxed);
}

float engine_fov() noexcept {
    return g_engineFov.load(std::memory_order_relaxed);
}

std::uint64_t frames_in_view() noexcept {
    return g_framesInView.load(std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::vr::state
