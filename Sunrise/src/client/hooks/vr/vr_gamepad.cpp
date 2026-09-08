/**
 * Controllers as keyboard and mouse.
 *
 * See vr_gamepad.h for why this is not an XInput pad. Everything goes through SendInput with
 * scancodes, which is the path the game's raw-input reader accepts; WM_-level fakes it ignores.
 *
 * Default PC bindings of the target build, as mapped here:
 *   left stick      W A S D           right stick       mouse motion (look, turns the body)
 *   right trigger   left mouse (fire) left trigger      right mouse (aim)
 *   A               Space (jump)      B                 Ctrl (crouch)
 *   X               R (reload)        Y                 1 / 2 alternating (weapon slot)
 *   left grip       Q (grenade)       right grip        C (melee)
 *   left stick click  Shift (sprint)  right stick click F (super)
 *   menu            F1 (menu)
 * Interact (E, hold) is deliberately unmapped until there is something to interact with.
 */

#include "vr_gamepad.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../input/window_focus.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::gamepad {
namespace {

/** Scancodes (set 1) of every key the injector can hold, indexed by Key. */
enum class Key : std::uint8_t {
    w,
    a,
    s,
    d,
    space,
    ctrl,
    shift,
    r,
    q,
    c,
    f,
    one,
    two,
    f1,
    count,
};

constexpr std::array<WORD, static_cast<std::size_t>(Key::count)> kScancodes{
    0x11, // W
    0x1E, // A
    0x1F, // S
    0x20, // D
    0x39, // Space
    0x1D, // Left Ctrl
    0x2A, // Left Shift
    0x13, // R
    0x10, // Q
    0x2E, // C
    0x21, // F
    0x02, // 1
    0x03, // 2
    0x3B, // F1
};

/** Stick deflection that starts a movement key, and the lower one that releases it. */
constexpr float kMovePress = 0.4F;
constexpr float kMoveRelease = 0.25F;
/** Below this the look stick is treated as centred. */
constexpr float kLookDeadZone = 0.12F;
/** Mouse counts per second at full deflection. Tuned on the headset, not here. */
constexpr float kLookCountsPerSecond = 1400.0F;
/**
 * Artificial turn rate at full stick. Expressed in radians rather than mouse counts because the
 * stick now moves the room anchor directly, which needs no calibration against the game's own
 * sensitivity.
 */
constexpr float kTurnRadiansPerSecond = 2.0F;
/** Pressed above, released below: triggers, grips and the analogue-to-digital buttons. */
constexpr float kTriggerPress = 0.55F;
constexpr float kTriggerRelease = 0.35F;
/** Longest interval a pump will integrate; a stall must not fling the view. */
constexpr float kMaxStepSeconds = 0.05F;

std::array<bool, static_cast<std::size_t>(Key::count)> g_keyDown{};
bool g_leftMouseDown{false};
bool g_rightMouseDown{false};
bool g_nextWeaponSlotTwo{true};
bool g_lastY{false};
float g_lookRemainderX{0.0F};
float g_lookRemainderY{0.0F};
LARGE_INTEGER g_lastPump{};
std::atomic<std::uint64_t> g_injected{0};
std::atomic<std::int64_t> g_mouseX{0};
std::atomic<std::int64_t> g_mouseY{0};

/** Sends one keyboard transition by scancode. */
void send_key(Key key, bool down) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = kScancodes[static_cast<std::size_t>(key)];
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0U : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof input);
}

/** Holds or releases one key, sending only on a change. @return True when a transition went out. */
bool hold_key(Key key, bool down) noexcept {
    bool& state = g_keyDown[static_cast<std::size_t>(key)];
    if (state == down) {
        return false;
    }
    state = down;
    send_key(key, down);
    return true;
}

/** Holds or releases one mouse button, sending only on a change. */
bool hold_mouse(bool& state, bool down, DWORD downFlag, DWORD upFlag) noexcept {
    if (state == down) {
        return false;
    }
    state = down;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? downFlag : upFlag;
    SendInput(1, &input, sizeof input);
    return true;
}

/** Sends relative mouse motion. */
void move_mouse(LONG dx, LONG dy) noexcept {
    if (dx == 0 && dy == 0) {
        return;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    SendInput(1, &input, sizeof input);
    g_mouseX.fetch_add(dx, std::memory_order_relaxed);
    g_mouseY.fetch_add(dy, std::memory_order_relaxed);
}

/** Digital read of an axis with hysteresis against its current key state. */
[[nodiscard]] bool digital(float value, bool held, float press, float release) noexcept {
    return held ? value > release : value > press;
}

/** @return Seconds since the previous pump, bounded. */
[[nodiscard]] float step_seconds() noexcept {
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float seconds = 0.0F;
    if (g_lastPump.QuadPart != 0 && frequency.QuadPart != 0) {
        seconds = static_cast<float>(now.QuadPart - g_lastPump.QuadPart)
                  / static_cast<float>(frequency.QuadPart);
    }
    g_lastPump = now;
    return std::clamp(seconds, 0.0F, kMaxStepSeconds);
}

/** Squared response keeps small deflections fine and the ends fast. */
[[nodiscard]] float look_curve(float value) noexcept {
    const float magnitude = std::fabs(value);
    if (magnitude < kLookDeadZone) {
        return 0.0F;
    }
    const float scaled = (magnitude - kLookDeadZone) / (1.0F - kLookDeadZone);
    return std::copysign(scaled * scaled, value);
}

/**
 * @return True while injected input may be sent at all.
 * Injected input lands on the foreground window, so anything but the game, with the mod's own
 * interface closed, would receive it instead.
 */
[[nodiscard]] bool gate_open() noexcept {
    return client::input::game_focused() && !core::ui::runtime::snapshot().visible;
}

} // namespace

bool turn_body(int counts) noexcept {
    if (counts == 0 || !gate_open()) {
        return false;
    }
    move_mouse(static_cast<LONG>(counts), 0);
    return true;
}

void release_all() noexcept {
    for (std::size_t index = 0; index < g_keyDown.size(); ++index) {
        if (g_keyDown[index]) {
            g_keyDown[index] = false;
            send_key(static_cast<Key>(index), false);
        }
    }
    (void)hold_mouse(g_leftMouseDown, false, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
    (void)hold_mouse(g_rightMouseDown, false, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
    g_lookRemainderX = 0.0F;
    g_lookRemainderY = 0.0F;
    g_lastPump = LARGE_INTEGER{};
}

void pump(bool enabled) noexcept {
    // Nothing goes out while the gate is shut, and nothing stays held either.
    if (!enabled || !gate_open()) {
        release_all();
        return;
    }
    const xr::InputState in = xr::input_state();
    if (!in.valid) {
        release_all();
        return;
    }
    const float dt = step_seconds();
    bool sent = false;

    // Left stick: four keys with hysteresis, opposite directions exclusive.
    const auto held = [](Key key) noexcept { return g_keyDown[static_cast<std::size_t>(key)]; };
    // Locomotion is expressed in the direction the player is LOOKING, then rotated into the
    // character's frame, because the engine moves relative to the character. While the servo is
    // caught up the rotation is nothing; while it lags -- or cannot inject at all -- this is what
    // keeps "forward" meaning forward. Eight-way granularity is imposed by the keyboard the game
    // reads, and is of no consequence for walking.
    const float lag = body_yaw_error();
    const float lagSine = std::sin(lag);
    const float lagCosine = std::cos(lag);
    // The stick is +Y forward and +X right; the character's forward is X and its LEFT is +Y, so a
    // leftward turn of the movement vector is a rotation of (forward, right) by -lag.
    const float moveForward = in.moveY * lagCosine + in.moveX * lagSine;
    const float moveRight = in.moveX * lagCosine - in.moveY * lagSine;
    sent |= hold_key(Key::w, digital(moveForward, held(Key::w), kMovePress, kMoveRelease));
    sent |= hold_key(Key::s, digital(-moveForward, held(Key::s), kMovePress, kMoveRelease));
    sent |= hold_key(Key::d, digital(moveRight, held(Key::d), kMovePress, kMoveRelease));
    sent |= hold_key(Key::a, digital(-moveRight, held(Key::a), kMovePress, kMoveRelease));

    // Right stick: artificial turning. It moves the room anchor by an exact number of radians
    // instead of injecting mouse counts, because a rotation expressed in radians needs no
    // calibration and cannot drift with the game's sensitivity setting. The character is brought
    // round to match by the body servo, so horizon and character still turn together, which is
    // what artificial turning has always done.
    //
    // Vertical look is dropped on purpose: in VR the pitch comes from the player's own neck, and
    // injecting mouse Y would only fight it.
    if (dt > 0.0F) {
        const float turn = look_curve(in.turnX);
        if (turn != 0.0F) {
            turn_room(-turn * kTurnRadiansPerSecond * dt);
        }
    }

    // Triggers onto the mouse buttons.
    sent |= hold_mouse(g_leftMouseDown,
                       digital(in.triggerR, g_leftMouseDown, kTriggerPress, kTriggerRelease),
                       MOUSEEVENTF_LEFTDOWN,
                       MOUSEEVENTF_LEFTUP);
    sent |= hold_mouse(g_rightMouseDown,
                       digital(in.triggerL, g_rightMouseDown, kTriggerPress, kTriggerRelease),
                       MOUSEEVENTF_RIGHTDOWN,
                       MOUSEEVENTF_RIGHTUP);

    // Buttons and grips onto the default bindings.
    sent |= hold_key(Key::space, (in.buttons & xr::kButtonA) != 0);
    sent |= hold_key(Key::ctrl, (in.buttons & xr::kButtonB) != 0);
    sent |= hold_key(Key::r, (in.buttons & xr::kButtonX) != 0);
    sent |= hold_key(Key::q, digital(in.gripL, held(Key::q), kTriggerPress, kTriggerRelease));
    sent |= hold_key(Key::c, digital(in.gripR, held(Key::c), kTriggerPress, kTriggerRelease));
    sent |= hold_key(Key::shift, (in.buttons & xr::kButtonThumbL) != 0);
    sent |= hold_key(Key::f, (in.buttons & xr::kButtonThumbR) != 0);
    sent |= hold_key(Key::f1, (in.buttons & xr::kButtonMenu) != 0);

    // Y alternates the two primary weapon slots: one tap per press.
    const bool yDown = (in.buttons & xr::kButtonY) != 0;
    const Key slot = g_nextWeaponSlotTwo ? Key::two : Key::one;
    if (yDown && !g_lastY) {
        sent |= hold_key(slot, true);
    } else if (!yDown && g_lastY) {
        sent |= hold_key(Key::one, false);
        sent |= hold_key(Key::two, false);
        g_nextWeaponSlotTwo = !g_nextWeaponSlotTwo;
    }
    g_lastY = yDown;

    if (sent) {
        g_injected.fetch_add(1, std::memory_order_relaxed);
    }
}

Stats stats() noexcept {
    Stats result{};
    result.injected = g_injected.load(std::memory_order_relaxed);
    for (const bool down : g_keyDown) {
        result.heldKeys += down ? 1U : 0U;
    }
    result.mouseCountsX = g_mouseX.load(std::memory_order_relaxed);
    result.mouseCountsY = g_mouseY.load(std::memory_order_relaxed);
    return result;
}

} // namespace sunrise::client::hooks::vr::gamepad
