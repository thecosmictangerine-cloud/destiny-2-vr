/**
 * The virtual screen (vr_screen.h): placement, fades, the pointer and its clicks.
 *
 * Geometry is all in OpenXR's LOCAL space, the space the layers are submitted in, so the screen is
 * fixed in the room by the compositor itself and holds still whatever the game's frame rate does.
 * A screen placement is "the eye it was placed for, a yaw, a pitch and a distance"; its centre is
 * eye + direction(yaw, pitch) * distance and the quad faces back along that direction.
 */

#include "vr_screen.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../input/window_focus.h"
#include "vr_display.h"
#include "vr_settings.h"
#include "vr_state.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::screen {
namespace {

constexpr float kPi = 3.14159265F;
constexpr float kDegrees = kPi / 180.0F;

struct Config final {
    float size = 62.0F;         // degrees: the screen's width seen from where it is placed
    float distance = 2.4F;      // m
    float pitchMin = -30.0F;    // degrees: placed at the head's pitch within these
    float pitchMax = 15.0F;
    float follow = 40.0F;       // degrees: the gaze this far from the centre...
    float followDelay = 0.8F;   // s: ...this long, and it comes back in front
    float glide = 0.45F;        // s
    float fadeIn = 0.22F;       // s
    float fadeOut = 0.28F;      // s: the world back in from the dimmed picture
    float backdrop = 0.22F;     // brightness of the frozen picture around the screen
    float density = 28.0F;      // px per degree of the panel's swapchain
    float corner = 0.018F;
};
[[nodiscard]] Config load_config() noexcept {
    Config c{};
    c.size = settings::number("screen_size", c.size, 20.0F, 120.0F);
    c.distance = settings::number("screen_distance", c.distance, 0.8F, 20.0F);
    return c;
}
const Config g_config = load_config();

struct Place final {
    float eye[3]{};
    float yaw{};
    float pitch{};
    float distance{2.4F};
    bool valid{};
};

/** Everything below lives on the present thread. */
Place g_current{};
Place g_from{};
Place g_to{};
float g_glideT = -1.0F;
double g_awaySince = -1.0;
float g_alpha = 0.0F;
float g_snapFade = 1.0F;
bool g_fromPlay = false;
bool g_wasScreen = false;
/** When the world came back (its fade-in), and the last frame of play (a picture "a moment ago"). */
double g_gameSince = -100.0;
double g_lastPlay = -100.0;
float g_exitFrom = 0.0F;
float g_lastBackdrop = 0.0F;
double g_last = 0.0;
std::int32_t g_stepAtEntry = -1;
std::uint32_t g_recentresSeen = 0;
std::atomic_bool g_recentreRequested{false};

/** The pointer and what it holds down. */
int g_hand = 1;
bool g_leftDown = false;
bool g_escDown = false;
std::uint32_t g_lastButtons = 0;
float g_lastTriggers[2]{};
int g_lastX = -100000;
int g_lastY = -100000;
double g_lastWheel = 0.0;
int g_tabHeld = 0;
double g_lastTab = 0.0;
std::uint64_t g_clicks = 0;
std::uint64_t g_moves = 0;
float g_lastU = 0.0F;
float g_lastV = 0.0F;
bool g_lastHit = false;
std::uint64_t g_frames = 0;
float g_lastWidth = 0.0F;
float g_lastHeight = 0.0F;

[[nodiscard]] double now_seconds() noexcept {
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

void reportf(const char* format, ...) noexcept {
    std::array<char, 320> line{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, args);
    va_end(args);
    if (written > 0) {
        core::log::write(core::log::Channel::client, core::log::Level::warn, line.data());
    }
}

[[nodiscard]] float ease(float t) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float wrap(float a) noexcept {
    while (a > kPi) {
        a -= 2.0F * kPi;
    }
    while (a < -kPi) {
        a += 2.0F * kPi;
    }
    return a;
}

void rotate(const float q[4], const float v[3], float out[3]) noexcept {
    const float c1[3] = {q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2], q[0] * v[1] - q[1] * v[0]};
    const float t[3] = {c1[0] + q[3] * v[0], c1[1] + q[3] * v[1], c1[2] + q[3] * v[2]};
    const float c2[3] = {q[1] * t[2] - q[2] * t[1], q[2] * t[0] - q[0] * t[2], q[0] * t[1] - q[1] * t[0]};
    for (int i = 0; i < 3; ++i) {
        out[i] = v[i] + 2.0F * c2[i];
    }
}

/** The OpenXR direction of a yaw (about +Y, positive to the left) and a pitch (positive up). */
void direction(float yaw, float pitch, float out[3]) noexcept {
    out[0] = -std::sin(yaw) * std::cos(pitch);
    out[1] = std::sin(pitch);
    out[2] = -std::cos(yaw) * std::cos(pitch);
}

/** Yaw, then pitch about the quad's own X: its +Z faces back along the direction it is in. */
void orientation(float yaw, float pitch, float q[4]) noexcept {
    const float cy = std::cos(yaw * 0.5F);
    const float sy = std::sin(yaw * 0.5F);
    const float cp = std::cos(pitch * 0.5F);
    const float sp = std::sin(pitch * 0.5F);
    q[0] = cy * sp;
    q[1] = sy * cp;
    q[2] = -sy * sp;
    q[3] = cy * cp;
}

[[nodiscard]] Place ahead(const Pose& head) noexcept {
    const float forwardLocal[3] = {0.0F, 0.0F, -1.0F};
    float forward[3]{};
    rotate(head.orientation, forwardLocal, forward);
    Place place{};
    for (int i = 0; i < 3; ++i) {
        place.eye[i] = head.position[i];
    }
    place.yaw = std::atan2(-forward[0], -forward[2]);
    place.pitch = std::clamp(std::asin(std::clamp(forward[1], -1.0F, 1.0F)), g_config.pitchMin * kDegrees,
                             g_config.pitchMax * kDegrees);
    place.distance = g_config.distance;
    place.valid = true;
    return place;
}

void centre(const Place& place, float out[3]) noexcept {
    float d[3]{};
    direction(place.yaw, place.pitch, d);
    for (int i = 0; i < 3; ++i) {
        out[i] = place.eye[i] + d[i] * place.distance;
    }
}

[[nodiscard]] Place lerp(const Place& a, const Place& b, float t) noexcept {
    Place out{};
    out.valid = true;
    for (int i = 0; i < 3; ++i) {
        out.eye[i] = a.eye[i] + (b.eye[i] - a.eye[i]) * t;
    }
    out.yaw = a.yaw + wrap(b.yaw - a.yaw) * t;
    out.pitch = a.pitch + (b.pitch - a.pitch) * t;
    out.distance = a.distance + (b.distance - a.distance) * t;
    return out;
}

void glide_to(const Place& to, const char* why) noexcept {
    g_from = g_current;
    g_to = to;
    g_glideT = 0.0F;
    g_awaySince = -1.0;
    reportf("ev=vr.screen move reason=%s yaw=%.0f->%.0f pitch=%.0f->%.0f", why, g_current.yaw / kDegrees, to.yaw / kDegrees,
            g_current.pitch / kDegrees, to.pitch / kDegrees);
}

// ---- input --------------------------------------------------------------------------------------

void send_mouse(DWORD flags, LONG dx, LONG dy, DWORD data) noexcept {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = flags;
    input.mi.mouseData = data;
    SendInput(1, &input, sizeof input);
}

void send_key(WORD scancode, bool down) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scancode;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0U : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof input);
}

void release_held() noexcept {
    if (g_leftDown) {
        send_mouse(MOUSEEVENTF_LEFTUP, 0, 0, 0);
        g_leftDown = false;
    }
    if (g_escDown) {
        send_key(0x01, false);
        g_escDown = false;
    }
}

/**
 * Moves the pointer to a point of the screen (physical pixels) with an absolute SendInput move: the
 * game sees real pointer motion -- what its hover needs (a SetCursorPos alone only pans the
 * Director) -- and Sunrise's guard on SetCursorPos, which swallows the call while its own interface
 * is open, is not in the way.
 */
void move_pointer(int x, int y) noexcept {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int height = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    const LONG nx = static_cast<LONG>(std::lround(static_cast<double>(x - left) * 65535.0 / width));
    const LONG ny = static_cast<LONG>(std::lround(static_cast<double>(y - top) * 65535.0 / height));
    send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, nx, ny, 0);
    ++g_moves;
}

/** Where a controller's ray meets the panel, in panel UV (0..1, V down). */
[[nodiscard]] bool hit(const Pose& aim, const float panelCentre[3], const float panelQ[4], float width, float height,
                       float& u, float& v, float& distance) noexcept {
    const float forwardLocal[3] = {0.0F, 0.0F, -1.0F};
    float d[3]{};
    rotate(aim.orientation, forwardLocal, d);
    const float normalLocal[3] = {0.0F, 0.0F, 1.0F};
    float n[3]{};
    rotate(panelQ, normalLocal, n);
    const float facing = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    if (facing > -0.05F) {
        return false;
    }
    const float toPanel[3] = {panelCentre[0] - aim.position[0], panelCentre[1] - aim.position[1], panelCentre[2] - aim.position[2]};
    const float t = (toPanel[0] * n[0] + toPanel[1] * n[1] + toPanel[2] * n[2]) / facing;
    if (t <= 0.0F || t > 20.0F) {
        return false;
    }
    const float point[3] = {aim.position[0] + d[0] * t - panelCentre[0], aim.position[1] + d[1] * t - panelCentre[1],
                            aim.position[2] + d[2] * t - panelCentre[2]};
    const float inverse[4] = {-panelQ[0], -panelQ[1], -panelQ[2], panelQ[3]};
    float local[3]{};
    rotate(inverse, point, local);
    u = local[0] / width + 0.5F;
    v = 0.5F - local[1] / height;
    distance = t;
    return u > -0.03F && u < 1.03F && v > -0.03F && v < 1.03F;
}

/** A quad along a ray, turned about it to face the head: the beam. */
void beam_pose(const Pose& aim, float length, const float head[3], Pose& out) noexcept {
    const float forwardLocal[3] = {0.0F, 0.0F, -1.0F};
    float x[3]{};
    rotate(aim.orientation, forwardLocal, x);
    float toHead[3] = {head[0] - aim.position[0], head[1] - aim.position[1], head[2] - aim.position[2]};
    const float along = toHead[0] * x[0] + toHead[1] * x[1] + toHead[2] * x[2];
    float z[3] = {toHead[0] - along * x[0], toHead[1] - along * x[1], toHead[2] - along * x[2]};
    float n = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (n < 1e-4F) {
        z[0] = 0.0F;
        z[1] = 1.0F;
        z[2] = 0.0F;
        n = 1.0F;
    }
    for (float& c : z) {
        c /= n;
    }
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    // Rotation matrix with columns x, y, z -> quaternion.
    const float m00 = x[0], m01 = y[0], m02 = z[0];
    const float m10 = x[1], m11 = y[1], m12 = z[1];
    const float m20 = x[2], m21 = y[2], m22 = z[2];
    const float trace = m00 + m11 + m22;
    float q[4]{};
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q[3] = 0.25F * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0F + m00 - m11 - m22) * 2.0F;
        q[3] = (m21 - m12) / s;
        q[0] = 0.25F * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0F + m11 - m00 - m22) * 2.0F;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25F * s;
        q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0F + m22 - m00 - m11) * 2.0F;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25F * s;
    }
    for (int i = 0; i < 3; ++i) {
        out.position[i] = aim.position[i] + x[i] * length * 0.5F;
    }
    for (int i = 0; i < 4; ++i) {
        out.orientation[i] = q[i];
    }
}

/**
 * The controllers on the screen. Either hand points; the one whose trigger was pulled last is the
 * one that clicks. Nothing is sent while the game is not the window in front.
 */
void pointer_input(Plan& plan, const Pose (&aims)[2], const bool (&aimValid)[2], const float panelCentre[3],
                   const float panelQ[4], const float head[3], std::uint32_t bbW, std::uint32_t bbH, double now) noexcept {
    const xr::InputState in = xr::input_state();
    const bool focused = client::input::game_focused();
    if (!in.valid || !focused) {
        release_held();
        g_lastButtons = in.buttons;
        return;
    }
    const float triggers[2] = {in.triggerL, in.triggerR};
    for (int h = 0; h < 2; ++h) {
        if (triggers[h] > 0.6F && g_lastTriggers[h] <= 0.6F) {
            g_hand = h;
        }
    }
    float u = 0.0F;
    float v = 0.0F;
    float distance = 0.0F;
    bool onPanel = aimValid[g_hand] && hit(aims[g_hand], panelCentre, panelQ, plan.width, plan.height, u, v, distance);
    if (!onPanel) {
        // The other hand may be the one pointing at it.
        const int other = 1 - g_hand;
        float u2 = 0.0F;
        float v2 = 0.0F;
        float d2 = 0.0F;
        if (aimValid[other] && hit(aims[other], panelCentre, panelQ, plan.width, plan.height, u2, v2, d2)) {
            g_hand = other;
            u = u2;
            v = v2;
            distance = d2;
            onPanel = true;
        }
    }
    if (aimValid[g_hand]) {
        plan.beam = true;
        plan.beamLength = onPanel ? distance : 0.6F;
        plan.beamAlpha = onPanel ? 0.75F : 0.35F;
        beam_pose(aims[g_hand], plan.beamLength, head, plan.beamPose);
    }
    if (onPanel) {
        u = std::clamp(u, 0.0F, 1.0F);
        v = std::clamp(v, 0.0F, 1.0F);
        plan.pointer = true;
        plan.pointerU = u;
        plan.pointerV = v;
        // Panel UV -> back buffer pixel -> the screen. The game takes the pointer's position in its
        // window's client area AS a back-buffer pixel, one to one, whatever the two sizes are
        // (Probe-Cursor.ps1, 17 Oct: a 1248x1296 eye-shaped buffer in a 1920x1112 window, the system
        // pointer at client (302,251), (302,851), (962,541) drawn at buffer (308,259), (303,865),
        // (966,555)). Scaling by client/buffer -- right only when they are equal, as on the desktop --
        // put every click of the eye-shaped render somewhere else.
        const auto window = static_cast<HWND>(display::window());
        if (window != nullptr && bbW != 0 && bbH != 0) {
            POINT origin{0, 0};
            ClientToScreen(window, &origin);
            const float bx = plan.crop[0] + u * plan.crop[2];
            const float by = plan.crop[1] + v * plan.crop[3];
            const int x = origin.x + static_cast<int>(std::lround(bx));
            const int y = origin.y + static_cast<int>(std::lround(by));
            const int screenBottom = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            const int screenRight = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            static bool reportedUnreachable = false;
            if ((y >= screenBottom || x >= screenRight) && !reportedUnreachable) {
                reportedUnreachable = true;
                reportf("ev=vr.screen pointer=unreachable x=%d y=%d screen=%d,%d note=buffer_larger_than_desktop", x, y,
                        screenRight, screenBottom);
            }
            if (x != g_lastX || y != g_lastY) {
                move_pointer(x, y);
                g_lastX = x;
                g_lastY = y;
            }
        }
    }
    g_lastHit = onPanel;
    g_lastU = u;
    g_lastV = v;

    // Click: the pointing hand's trigger, or A. Held for as long as it is held (drags, hold-to-confirm).
    const bool a = (in.buttons & xr::kButtonA) != 0;
    const bool wantDown = (onPanel || g_leftDown) && (triggers[g_hand] > 0.6F || (g_leftDown && triggers[g_hand] > 0.35F) || a);
    if (wantDown != g_leftDown) {
        send_mouse(wantDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0, 0, 0);
        g_leftDown = wantDown;
        if (wantDown) {
            ++g_clicks;
            xr::haptic(g_hand == 1, 0.25F, 0.015F);
        }
    }
    // Back: B or the menu button is Esc.
    const bool back = (in.buttons & (xr::kButtonB | xr::kButtonMenu)) != 0;
    if (back != g_escDown) {
        send_key(0x01, back);
        g_escDown = back;
    }
    // Scroll: the right stick, faster the further it is pushed.
    const float scroll = in.turnY;
    if (std::fabs(scroll) > 0.35F) {
        const double interval = 0.16 - 0.11 * (std::fabs(scroll) - 0.35) / 0.65;
        if (now - g_lastWheel > interval) {
            send_mouse(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(scroll > 0.0F ? WHEEL_DELTA : -WHEEL_DELTA));
            g_lastWheel = now;
        }
    }
    // Tabs: the left stick left or right is A / D, which is how Destiny's menus page on a PC
    // (the character screen shows [A] CLAN ... INVENTORY [D]). Repeats while held.
    const int tab = in.moveX > 0.7F ? 1 : in.moveX < -0.7F ? -1 : 0;
    if (tab == 0) {
        g_tabHeld = 0;
    } else if (tab != g_tabHeld || now - g_lastTab > 0.45) {
        const WORD scancode = tab > 0 ? 0x20 : 0x1E;  // D : A
        send_key(scancode, true);
        send_key(scancode, false);
        g_tabHeld = tab;
        g_lastTab = now;
    }
    // A stick click: the screen in front again.
    const std::uint32_t pressed = in.buttons & ~g_lastButtons;
    if ((pressed & (xr::kButtonThumbL | xr::kButtonThumbR)) != 0) {
        g_recentreRequested = true;
    }
    g_lastButtons = in.buttons;
    g_lastTriggers[0] = triggers[0];
    g_lastTriggers[1] = triggers[1];
}

} // namespace

Plan plan(const Pose& head, bool headValid, const Pose (&aims)[2], const bool (&aimValid)[2], std::uint32_t bbW,
          std::uint32_t bbH, double snapshotAge, std::uint32_t recentres) noexcept {
    Plan out{};
    ++g_frames;
    const double now = now_seconds();
    const float dt = g_last > 0.0 ? static_cast<float>(std::clamp(now - g_last, 0.0, 0.1)) : 0.0F;
    g_last = now;
    const bool screen = state::view() == state::View::screen;

    if (screen && !g_wasScreen) {
        // Into the screen: placed in front of the head, faded in; the game a moment ago frozen
        // around it when it came straight out of play.
        g_alpha = 0.0F;
        g_snapFade = 1.0F;
        g_glideT = -1.0F;
        g_awaySince = -1.0;
        g_current = headValid ? ahead(head) : Place{};
        g_fromPlay = now - g_lastPlay < 1.0 && snapshotAge >= 0.0 && snapshotAge < 2.5;
        out.freeze = g_fromPlay;
        g_stepAtEntry = state::step();
        g_recentresSeen = recentres;
        // It opens in front already: a recentre asked for in the world is spent.
        g_recentreRequested = false;
        reportf("ev=vr.screen open reason=%s yaw=%.0f pitch=%.0f backdrop=%s", state::reason(), g_current.yaw / kDegrees,
                g_current.pitch / kDegrees, g_fromPlay ? "frozen_game" : "room");
    }
    if (!screen) {
        if (g_wasScreen) {
            // Back to the world: it fades in from the dimmed picture, or from black.
            g_gameSince = now;
            g_exitFrom = std::clamp(g_lastBackdrop, 0.0F, 1.0F);
            release_held();
            reportf("ev=vr.screen close clicks=%llu moves=%llu", static_cast<unsigned long long>(g_clicks),
                    static_cast<unsigned long long>(g_moves));
        }
        g_wasScreen = false;
        if (now - g_gameSince < g_config.fadeOut) {
            out.worldFade = g_exitFrom + (1.0F - g_exitFrom) * ease(static_cast<float>((now - g_gameSince) / g_config.fadeOut));
        }
        g_lastPlay = now;
        return out;
    }
    g_wasScreen = true;
    out.screen = true;
    if (!headValid) {
        out.screen = true;
        out.alpha = 0.0F;
        return out;
    }

    if (!g_current.valid || recentres != g_recentresSeen) {
        g_current = ahead(head);
        g_glideT = -1.0F;
        g_recentresSeen = recentres;
    }
    if (g_recentreRequested.exchange(false)) {
        glide_to(ahead(head), "request");
        xr::haptic(false, 0.2F, 0.02F);
        xr::haptic(true, 0.2F, 0.02F);
    }
    float c[3]{};
    centre(g_current, c);
    {
        // Looked away long enough: back in front.
        const float forwardLocal[3] = {0.0F, 0.0F, -1.0F};
        float f[3]{};
        rotate(head.orientation, forwardLocal, f);
        const float v[3] = {c[0] - head.position[0], c[1] - head.position[1], c[2] - head.position[2]};
        const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const float off = n > 1e-4F ? std::acos(std::clamp((v[0] * f[0] + v[1] * f[1] + v[2] * f[2]) / n, -1.0F, 1.0F)) / kDegrees : 0.0F;
        if (g_glideT < 0.0F && g_config.followDelay > 0.0F && off > g_config.follow) {
            if (g_awaySince < 0.0) {
                g_awaySince = now;
            } else if (now - g_awaySince > g_config.followDelay) {
                glide_to(ahead(head), "out_of_sight");
            }
        } else {
            g_awaySince = -1.0;
        }
    }
    if (g_glideT >= 0.0F) {
        g_glideT += g_config.glide > 0.0F ? dt / g_config.glide : 1.0F;
        g_current = g_glideT >= 1.0F ? g_to : lerp(g_from, g_to, ease(g_glideT));
        if (g_glideT >= 1.0F) {
            g_glideT = -1.0F;
        }
        centre(g_current, c);
    }
    g_alpha = g_config.fadeIn > 0.0F ? (std::min)(1.0F, g_alpha + dt / g_config.fadeIn) : 1.0F;
    const float a = ease(g_alpha);
    // A load replaces the world: the frozen picture is of the place left behind. Let it go.
    if (g_fromPlay && state::step() != g_stepAtEntry && g_snapFade > 0.0F) {
        if (g_snapFade >= 1.0F) {
            reportf("ev=vr.screen backdrop=fading reason=step step=%d", state::step());
        }
        g_snapFade = (std::max)(0.0F, g_snapFade - dt / 0.5F);
    }

    // The panel: the back buffer's UI band, where every menu is laid out.
    unsigned top = 0;
    unsigned bottom = bbH;
    display::ui_band(bbW, bbH, top, bottom);
    out.crop[0] = 0.0F;
    out.crop[1] = static_cast<float>(top);
    out.crop[2] = static_cast<float>(bbW);
    out.crop[3] = static_cast<float>(bottom - top);
    const float aspect = out.crop[3] > 0.0F ? out.crop[2] / out.crop[3] : 1.6F;
    const float grow = 0.965F + 0.035F * a;  // it opens out a little as it fades in
    out.width = 2.0F * g_config.distance * std::tan(g_config.size * 0.5F * kDegrees) * grow;
    out.height = out.width / aspect;
    out.alpha = a;
    out.corner = g_config.corner;
    orientation(g_current.yaw, g_current.pitch, out.panel.orientation);
    for (int i = 0; i < 3; ++i) {
        out.panel.position[i] = c[i];
    }
    out.outWidth = static_cast<std::uint32_t>(std::clamp((std::min)(out.crop[2], g_config.size * g_config.density), 256.0F, 4096.0F));
    out.outHeight = static_cast<std::uint32_t>(std::clamp(static_cast<float>(out.outWidth) / aspect, 144.0F, 4096.0F));

    // The room.
    if (g_fromPlay) {
        // Out of play: the frozen picture IS the game a moment ago, so it starts at full brightness
        // and dims while the screen comes up -- the world stops and darkens, no cut to black.
        out.snapshot = (1.0F + (g_config.backdrop - 1.0F) * a) * g_snapFade;
        out.desaturate = 0.35F * a;
        out.environment = a;
        out.fade = 1.0F;
    } else {
        out.snapshot = 0.0F;
        out.environment = 1.0F;
        out.fade = a;
    }
    g_lastBackdrop = out.snapshot;
    float glow[3] = {c[0] - head.position[0], c[1] - head.position[1], c[2] - head.position[2]};
    const float gn = std::sqrt(glow[0] * glow[0] + glow[1] * glow[1] + glow[2] * glow[2]);
    for (int i = 0; i < 3; ++i) {
        out.glowDirection[i] = gn > 1e-4F ? glow[i] / gn : 0.0F;
    }
    out.glow = a;

    pointer_input(out, aims, aimValid, c, out.panel.orientation, head.position, bbW, bbH, now);
    g_lastWidth = out.width;
    g_lastHeight = out.height;
    if ((g_frames % 90) == 0) {
        report_status();
    }
    return out;
}

void request_recentre() noexcept {
    g_recentreRequested = true;
}

void report_status() noexcept {
    float c[3]{};
    if (g_current.valid) {
        centre(g_current, c);
    }
    reportf("ev=vr.screen status open=%d yaw=%.3f pitch=%.3f centre=%.4f,%.4f,%.4f size=%.4f,%.4f alpha=%.2f pointer=%d u=%.3f v=%.3f hand=%d "
            "clicks=%llu moves=%llu",
            g_wasScreen ? 1 : 0, g_current.yaw / kDegrees, g_current.pitch / kDegrees, c[0], c[1], c[2], g_lastWidth, g_lastHeight, g_alpha,
            g_lastHit ? 1 : 0, g_lastU, g_lastV, g_hand, static_cast<unsigned long long>(g_clicks),
            static_cast<unsigned long long>(g_moves));
}

} // namespace sunrise::client::hooks::vr::screen
