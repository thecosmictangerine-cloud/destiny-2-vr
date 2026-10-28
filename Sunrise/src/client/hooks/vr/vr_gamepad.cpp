/**
 * Controllers as keyboard and mouse.
 *
 * See vr_gamepad.h for why this is not an XInput pad. Everything goes through SendInput with
 * scancodes, which is the path the game's raw-input reader accepts; WM_-level fakes it ignores.
 *
 * Default PC bindings of the target build, as mapped here:
 *   left stick      W A S D           right stick       mouse motion (look, turns the body)
 *   right trigger   left mouse (fire) left trigger      Q (grenade)
 *   A               Space (jump)      B                 Ctrl (crouch)
 *   X               R (reload)        Y                 1 / 2 alternating (weapon slot); held: dual wield
 *   left grip       hold the weapon                     right grip        C (melee)
 *   left stick click  Shift (sprint)  right stick click F (super)
 *   menu            F1 (menu)
 *   right mouse (aim) is held by the physical ADS detector while the sight is at the eye (vr_ads.cpp)
 * Interact (E, hold) is deliberately unmapped until there is something to interact with.
 *
 * The grenade is the left trigger, except while the left hand holds the gun: the index finger rests
 * on that trigger round a foregrip, and a firm grip squeezes it, so a press that STARTS while holding
 * is ignored until it is let go. The left grip is then only the hold.
 *
 * Dual wield (vr_dual.cpp, docs/DUAL-WIELD.md): holding Y gives the left hand a weapon of its own, or puts
 * it away. With a weapon in the left hand, the left trigger fires it, the left grip is the grenade, a tap
 * of Y cycles which loadout weapon the left hand holds, and X reloads the left weapon too.
 *
 * `ads mode button` (SVR_Ads.txt) restores the earlier mapping: left trigger = right mouse (aim), and
 * the left grip contextual -- on the handguard it takes hold, away from it it is the grenade.
 */

#include "vr_gamepad.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../input/window_focus.h"
#include "vr_ads.h"
#include "vr_arms.h"
#include "vr_body.h"
#include "vr_mode.h"
#include "vr_dual.h"
#include "vr_state.h"
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
    three,
    f1,
    m,
    o,
    p,
    tab,
    e,
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
    0x04, // 3
    0x3B, // F1
    0x32, // M
    0x18, // O (sparrow dodge left)
    0x19, // P (sparrow dodge right)
    0x0F, // Tab (the Ghost; with E held after it, the sparrow)
    0x12, // E
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
std::atomic_bool g_aimHeld{false};
/** The left trigger as a button, and whether the press in progress began while holding the gun. */
bool g_leftTriggerDown{false};
bool g_leftTriggerBlocked{false};
/** The menu button: when it went down, whether its hold already fired, and a key held for one pump. */
bool g_menuWasDown{false};
bool g_menuFired{false};
std::uint64_t g_menuSince{0};
int g_tapKey{-1};
bool g_nextWeaponSlotTwo{true};
bool g_lastY{false};
/** When Y went down (ms), and whether this press has already been used as a hold. */
std::uint64_t g_yDownAt{0};
bool g_yHoldDone{false};
/** Holding Y this long toggles dual wield instead of swapping weapons. */
constexpr std::uint64_t kYHoldMs = 600;
/** A weapon slot key being tapped for the dual-wield module (calibration): which, and pumps left. */
int g_tapSlot{-1};
int g_tapPumps{0};
bool g_lastX{false};
float g_lookRemainderX{0.0F};
float g_lookRemainderY{0.0F};
LARGE_INTEGER g_lastPump{};
std::atomic<std::uint64_t> g_injected{0};
std::atomic<std::int64_t> g_mouseX{0};
std::atomic<std::int64_t> g_mouseY{0};
/** The play mode of the previous pump: a change lets go of everything, so no key outlives its mode. */
mode::Mode g_lastMode{mode::Mode::first};
/**
 * The sparrow from the controllers: X held 0.8 s plays the game's own summon -- Tab tapped (the Ghost),
 * then E held -- which has no binding of its own on PC. A tap of X is still the reload.
 */
std::uint64_t g_xDownAt{0};
bool g_xHoldDone{false};
int g_summonStep{0};
/** Pumps left of a tapped reload (R). */
int g_reloadPumps{0};
std::uint64_t g_summonAt{0};

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
    g_aimHeld.store(false, std::memory_order_relaxed);
    g_leftTriggerDown = false;
    g_leftTriggerBlocked = false;
    g_tapKey = -1;
    g_summonStep = 0;
    g_lookRemainderX = 0.0F;
    g_lookRemainderY = 0.0F;
    g_lastPump = LARGE_INTEGER{};
}

namespace {

/**
 * The menu button: a tap is the character screen (F1), held 0.6 s the Director (M) -- the Director has
 * no other way in from the world on the controllers. Either way the virtual screen comes up with it,
 * this frame, instead of a frame or two of a giant menu across the eyes before the menu is recognised.
 * A tapped key is held for one pump, then let go.
 */
void menu_button(const xr::InputState& in, bool& sent) noexcept {
    if (g_tapKey >= 0) {
        sent |= hold_key(static_cast<Key>(g_tapKey), false);
        g_tapKey = -1;
    }
    const std::uint64_t menuNow = GetTickCount64();
    const bool menuDown = (in.buttons & xr::kButtonMenu) != 0;
    if (menuDown && !g_menuWasDown) {
        g_menuSince = menuNow;
        g_menuFired = false;
    }
    if (menuDown && !g_menuFired && menuNow - g_menuSince >= 600) {
        g_menuFired = true;
        state::expect_menu();
        sent |= hold_key(Key::m, true);
        g_tapKey = static_cast<int>(Key::m);
    }
    if (!menuDown && g_menuWasDown && !g_menuFired) {
        state::expect_menu();
        sent |= hold_key(Key::f1, true);
        g_tapKey = static_cast<int>(Key::f1);
    }
    g_menuWasDown = menuDown;
}

/**
 * The sparrow (docs/SWORD-SPARROW.md), No Man's Sky VR's exocraft for a model: throttle on the right
 * trigger, brake and reverse on the left, steering on the handlebars (grip them where the rider's hands
 * are; push one forward to turn) or on either stick; boost on the left stick click, dismount on A, the
 * dodges on X and Y. Steering never comes from the head: vr_mode.cpp turns it into the heading the
 * vehicle is driven towards.
 */
void pump_vehicle(const xr::InputState& in, bool& sent) noexcept {
    const auto held = [](Key key) noexcept { return g_keyDown[static_cast<std::size_t>(key)]; };
    const bool forward = digital(in.triggerR, held(Key::w), kTriggerPress, kTriggerRelease)
                         || digital(in.moveY, held(Key::w), kMovePress, kMoveRelease);
    const bool back = digital(in.triggerL, held(Key::s), kTriggerPress, kTriggerRelease)
                      || digital(-in.moveY, held(Key::s), kMovePress, kMoveRelease);
    sent |= hold_key(Key::w, forward && !back);
    sent |= hold_key(Key::s, back);
    sent |= hold_key(Key::a, false);
    sent |= hold_key(Key::d, false);
    bool onBars = false;
    const float bars = body::handlebar_steer(onBars);
    const float stick = std::clamp(look_curve(in.moveX) + look_curve(in.turnX), -1.0F, 1.0F);
    mode::set_steering(onBars && std::fabs(stick) < 0.05F ? bars : stick);
    // Boost is the vehicle's right mouse.
    sent |= hold_mouse(g_rightMouseDown, (in.buttons & xr::kButtonThumbL) != 0, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
    sent |= hold_mouse(g_leftMouseDown, false, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
    sent |= hold_key(Key::space, (in.buttons & xr::kButtonA) != 0);
    sent |= hold_key(Key::o, (in.buttons & xr::kButtonX) != 0);
    sent |= hold_key(Key::p, (in.buttons & xr::kButtonY) != 0);
    for (const Key k : {Key::ctrl, Key::shift, Key::r, Key::q, Key::c, Key::f}) {
        sent |= hold_key(k, false);
    }
    menu_button(in, sent);
}

} // namespace

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
    const mode::Mode playMode = mode::current();
    const bool firstPerson = playMode == mode::Mode::first;
    const bool swordInHand = playMode == mode::Mode::onFoot && body::sword_held();
    if (playMode != g_lastMode) {
        release_all();
        g_lastMode = playMode;
        mode::set_steering(0.0F);
    }
    if (playMode == mode::Mode::vehicle) {
        bool sentVehicle = false;
        (void)step_seconds();
        pump_vehicle(in, sentVehicle);
        if (sentVehicle) {
            g_injected.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    // A grip squeezed at a holster draws or puts away a weapon (vr_dual.cpp), and is nothing else.
    const dual::HolsterUse holster = dual::holster_pump();
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

    // Right trigger: fire.
    sent |= hold_mouse(g_leftMouseDown,
                       digital(in.triggerR, g_leftMouseDown, kTriggerPress, kTriggerRelease),
                       MOUSEEVENTF_LEFTDOWN,
                       MOUSEEVENTF_LEFTUP);
    // Aim and grenade, by mode. Physical ADS (the default) holds right mouse while the sight is at
    // the eye and frees the left trigger for the grenade.
    const ads::Mode mode = ads::mode();
    const bool leftTrigger = digital(in.triggerL, g_leftTriggerDown, kTriggerPress, kTriggerRelease);
    // With a weapon in the left hand, the left trigger is that weapon's (vr_dual.cpp reads it itself).
    const bool dualWield = dual::dual();
    if (leftTrigger && !g_leftTriggerDown && !dualWield) {
        // Classified once, where the press starts: a squeeze that begins with the gun in both hands
        // is the grip tightening round the foregrip, not a throw.
        g_leftTriggerBlocked = arms::holding();
        if (mode != ads::Mode::button) {
            core::log::write(core::log::Channel::client, core::log::Level::warn,
                             g_leftTriggerBlocked ? "ev=vr.pad left_trigger action=ignored reason=holding"
                                                  : "ev=vr.pad left_trigger action=grenade");
        }
    }
    g_leftTriggerDown = leftTrigger;
    // Only in first person: with a sword the right mouse is its heavy attack, and no sight is learned.
    const bool aim = !firstPerson ? false : mode == ads::Mode::button ? leftTrigger : ads::want_ads();
    // Dual wield: the grenade moves to the left grip, which has no gun to hold there.
    const bool grenade = dualWield ? !holster.left && digital(in.gripL, held(Key::q), kTriggerPress, kTriggerRelease)
                         : mode == ads::Mode::button ? arms::grenade()
                                                     : (leftTrigger && !g_leftTriggerBlocked);
    sent |= hold_mouse(g_rightMouseDown, aim, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
    g_aimHeld.store(g_rightMouseDown, std::memory_order_relaxed);

    // Buttons and grips onto the default bindings.
    sent |= hold_key(Key::space, (in.buttons & xr::kButtonA) != 0);
    sent |= hold_key(Key::ctrl, (in.buttons & xr::kButtonB) != 0);
    const bool xDown = (in.buttons & xr::kButtonX) != 0;
    // A tap of X reloads, on its release (a hold is the sparrow's summon, and must not reload too).
    if (!xDown && g_lastX && !g_xHoldDone) {
        g_reloadPumps = 4;
    }
    sent |= hold_key(Key::r, g_reloadPumps > 0);
    if (g_reloadPumps > 0) {
        --g_reloadPumps;
    }
    if (xDown && !g_lastX && dualWield) {
        dual::request_reload();
    }
    {
        const std::uint64_t now = GetTickCount64();
        if (xDown && !g_lastX) {
            g_xDownAt = now;
            g_xHoldDone = false;
        } else if (xDown && !g_xHoldDone && now - g_xDownAt >= 800 && g_summonStep == 0) {
            g_xHoldDone = true;
            g_summonStep = 1;
            g_summonAt = now;
            core::log::write(core::log::Channel::client, core::log::Level::warn, "ev=vr.pad summon=sparrow");
        }
        // Tab down 120 ms, up, a breath, then E held 1.8 s (the game's hold), then up.
        const std::uint64_t t = now - g_summonAt;
        switch (g_summonStep) {
        case 1: sent |= hold_key(Key::tab, true); if (t > 120) { g_summonStep = 2; } break;
        case 2: sent |= hold_key(Key::tab, false); if (t > 650) { g_summonStep = 3; } break;
        case 3: sent |= hold_key(Key::e, true); if (t > 2450) { g_summonStep = 4; } break;
        case 4: sent |= hold_key(Key::e, false); g_summonStep = 0; break;
        default: break;
        }
    }
    g_lastX = xDown;
    sent |= hold_key(Key::q, grenade);
    // The right grip is melee (C) -- except on a sword's hilt, where it is the second hand's hold.
    sent |= hold_key(Key::c, !holster.right && !(swordInHand && body::two_handed())
                                 && digital(in.gripR, held(Key::c), kTriggerPress, kTriggerRelease));
    sent |= hold_key(Key::shift, (in.buttons & xr::kButtonThumbL) != 0);
    sent |= hold_key(Key::f, (in.buttons & xr::kButtonThumbR) != 0);
    menu_button(in, sent);

    // Y: a tap alternates the two primary weapon slots (in dual wield: cycles the LEFT hand's weapon);
    // held, it gives the left hand a weapon or puts it away. A tap acts on release, so it can be told
    // from a hold.
    const bool yDown = (in.buttons & xr::kButtonY) != 0;
    const std::uint64_t nowMs = GetTickCount64();
    if (yDown && !g_lastY) {
        g_yDownAt = nowMs;
        g_yHoldDone = false;
    } else if (yDown && !g_yHoldDone && nowMs - g_yDownAt >= kYHoldMs) {
        g_yHoldDone = true;
        dual::set_dual(!dual::dual());
    } else if (!yDown && g_lastY && !g_yHoldDone) {
        if (swordInHand) {
            // A sword: Y passes it to the other hand.
            body::set_sword_hand(1 - body::sword_hand());
        } else if (dualWield) {
            dual::cycle_left();
        } else {
            g_tapSlot = g_nextWeaponSlotTwo ? 1 : 0;
            g_tapPumps = 0;
            g_nextWeaponSlotTwo = !g_nextWeaponSlotTwo;
        }
    }
    g_lastY = yDown;
    // A slot the dual-wield module wants shown in the right hand (to record it for the left).
    const int wanted = dual::take_slot_request();
    if (wanted >= 0 && wanted <= 2) {
        g_tapSlot = wanted;
        g_tapPumps = 0;
        // Keep the Y alternation in step with whatever is in hand now.
        g_nextWeaponSlotTwo = wanted != 1;
    }
    // A weapon slot key, tapped: down for a few pumps, then up.
    if (g_tapSlot >= 0) {
        const Key key = g_tapSlot == 0 ? Key::one : g_tapSlot == 1 ? Key::two : Key::three;
        if (g_tapPumps == 0) {
            dual::note_slot_key(g_tapSlot);
        }
        sent |= hold_key(key, g_tapPumps < 4);
        if (++g_tapPumps > 5) {
            sent |= hold_key(Key::one, false);
            sent |= hold_key(Key::two, false);
            sent |= hold_key(Key::three, false);
            g_tapSlot = -1;
        }
    }

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

bool aim_held() noexcept {
    return g_aimHeld.load(std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::vr::gamepad
