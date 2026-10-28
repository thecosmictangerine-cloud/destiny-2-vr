/**
 * The gun's aim line, drawn and reported.
 *
 * The line is the shot as it will be fired: from the muzzle, a point fixed relative to the palm in
 * the gun's basis (controller, rotational seat and two-handed swing all included), along the
 * learned SIGHT line zeroed at `ads::zero_distance()` -- or along the bare barrel while the weapon in
 * hand has no learned sight (vr_ads.cpp). The sight line itself is drawn beside it. Everything is
 * camera-relative in the game's axes (X forward, Y left, Z up), the frame the viewmodel camera is
 * expressed in, so it lands where the gun is drawn without knowing where the player is.
 *
 * Why a drawn line is the instrument: the world-line pass projects with the camera the frame was
 * rendered with, so an impact that is on the shot's line is on the drawn line in the image whatever
 * its distance -- no depth, no parallax arithmetic, nothing to estimate. The harness measures the
 * perpendicular distance in pixels and converts it to an angle.
 *
 * `SVR_Aim.txt`, one command per line, consumed on read:
 *
 *   ray on|off              draw the aim line into the frame (desktop and headset)
 *   muzzle <fwd> <right> <up>  muzzle point from the palm, controller basis, metres
 *   length <metres>         how far the drawn line reaches
 *   sight on|off            zero the shot onto the learned sight (default) or fire along the barrel
 *   dump                    log the line now, including its projection
 */

#include "vr_aim.h"

#include <Windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <span>
#include <vector>

#include "../../../core/logging/log.h"
#include "../graphics/renderer/world_lines.h"
#include "../teleport/runtime.h"
#include "runtime.h"
#include "vr_ads.h"
#include "vr_debug.h"
#include "vr_dual.h"
#include "vr_gpu.h"
#include "vr_math.h"
#include "vr_prehook.h"
#include "vr_weapon.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::aim {
namespace {

namespace wl = graphics::renderer::world_lines;
using math::M3;
using math::V3;

constexpr std::uint32_t kPollPeriod = 30;
constexpr std::uint32_t kReportPeriod = 180;
constexpr wchar_t kCommandName[] = L"SVR_Aim.txt";

std::atomic_bool g_ray{false};
std::atomic_bool g_dumpNow{false};
/**
 * Muzzle from the palm in the controller's basis (forward, right, up). Only the components across
 * the bore matter for where a shot goes -- moving the origin along its own line changes nothing --
 * so the forward one is cosmetic: it decides where the drawn line starts.
 *
 * Measured from side views with the line drawn (2026-10-15): at 0.07 up the bore ran 2.3 cm above
 * the line on the hand cannon and 3.7 cm on the auto rifle; 0.10 puts it through the middle of the
 * hand cannon's barrel and the lower edge of the rifle's. Forward stays INSIDE the gun (the mouth is
 * ~0.33 m further on the hand cannon): the gun hides where the round starts, so the tracer reads as
 * leaving the barrel on any length of gun, and the game's eye-to-start wall check stays conservative.
 */
std::array<std::atomic<float>, 3> g_muzzle{{{0.20F}, {0.0F}, {0.10F}}};
std::atomic<float> g_length{300.0F};
std::uint32_t g_frame = 0;

void report_text(const char* text) noexcept {
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

V3 v3(const xr::Vector& v) noexcept {
    return V3{v[0], v[1], v[2]};
}

/** Use the learned sight for the shot's direction (`sight on|off`, default on). */
std::atomic_bool g_sightOn{true};

/**
 * Where the shot goes, relative to the character's eye (the engine's camera position): from the
 * muzzle, along `basis` column 0. With a learned sight, that direction is zeroed onto the sight line
 * -- the round meets the line the player sees through the sights at the zero distance -- and the
 * rest of the basis is the gun's, turned by the smallest rotation that does it, so the fire rig keeps
 * its roll. Without one, the barrel: the gun basis' forward.
 */
struct Shot final {
    V3 muzzle{};
    M3 basis{};
    V3 sightO{};
    V3 sightD{};
    bool sighted{};
    xr::HeadPose head{};
    bool valid{};
};

Shot shot_geometry() noexcept {
    Shot s{};
    math::Affine gun{};
    xr::HandPose hand{};
    if (!weapon::gun_frame(gun, s.head, &hand)) {
        return s;
    }
    const V3 f = v3(hand.forward);
    const V3 r = v3(hand.right);
    const V3 u = v3(hand.up);
    s.basis = math::orthonormalize(M3{{f, -r, u}});
    s.muzzle = v3(hand.palm) + f * g_muzzle[0].load(std::memory_order_relaxed)
               + r * g_muzzle[1].load(std::memory_order_relaxed) + u * g_muzzle[2].load(std::memory_order_relaxed);
    V3 so{};
    V3 sd{};
    if (g_sightOn.load(std::memory_order_relaxed) && ads::sight(so, sd)) {
        s.sightO = gun * so;
        s.sightD = math::normalize(gun.m * sd);
        const V3 target = s.sightO + s.sightD * ads::zero_distance();
        const V3 dir = math::normalize(target - s.muzzle);
        const V3 axis = math::cross(s.basis.c[0], dir);
        const float sine = math::length(axis);
        const float cosine = math::dot(s.basis.c[0], dir);
        if (sine > 1e-6F) {
            s.basis = math::orthonormalize(math::axis_angle(axis * (1.0F / sine), std::atan2(sine, cosine)) * s.basis);
        }
        s.sighted = true;
    }
    s.valid = math::finite(s.muzzle) && math::finite(s.basis.c[0]);
    return s;
}

/** The aim line, camera-relative: origin at the muzzle, unit direction along the shot. */
struct Line final {
    V3 origin{};
    V3 direction{};
    /** The sight line, when known, camera-relative. */
    V3 sightO{};
    V3 sightD{};
    bool sighted{};
    bool valid{};
};

Line current_line() noexcept {
    Line line{};
    const Shot s = shot_geometry();
    if (!s.valid) {
        return line;
    }
    const V3 head = v3(s.head.offset);
    line.origin = s.muzzle - head;
    line.direction = s.basis.c[0];
    line.sighted = s.sighted;
    line.sightO = s.sightO - head;
    line.sightD = s.sightD;
    line.valid = math::finite(line.origin) && math::finite(line.direction);
    return line;
}

/**
 * Projects a camera-relative point exactly as the world-line pass does, into normalised device
 * coordinates (x right, y up, both -1..1 across the frame).
 * @return False behind the camera.
 */
bool project(const gpu::ViewCamera& camera, const V3& p, float& x, float& y) noexcept {
    const V3 forward = math::normalize(V3{camera.forward[0], camera.forward[1], camera.forward[2]});
    const V3 up = math::normalize(V3{camera.up[0], camera.up[1], camera.up[2]});
    const V3 right = math::normalize(math::cross(forward, up));
    const float depth = math::dot(p, forward);
    const float tangent = std::tan(camera.horizontalFov * 0.5F);
    if (depth <= 1e-4F || tangent <= 0.0F) {
        return false;
    }
    x = math::dot(p, right) / (tangent * depth);
    y = math::dot(p, up) * camera.aspect / (tangent * depth);
    return std::isfinite(x) && std::isfinite(y);
}

/**
 * One line for the harness: the aim line in camera space and in NDC (its origin, and its vanishing
 * point, which is where an infinitely distant impact on it would appear), and the camera it was
 * projected with.
 */
void report_line(const Line& line, const gpu::ViewCamera& camera) noexcept {
    float ox = 0.0F;
    float oy = 0.0F;
    float vx = 0.0F;
    float vy = 0.0F;
    const bool originOk = project(camera, line.origin, ox, oy);
    const bool vanishOk = project(camera, line.direction * 1.0e4F, vx, vy);
    // The sight line's own vanishing point and where its zero point lands: the round should arrive
    // at the zero point, and far beyond it it converges on the sight's vanishing point.
    float sx = 0.0F;
    float sy = 0.0F;
    const bool sightOk = line.sighted && project(camera, line.sightD * 1.0e4F, sx, sy);
    // Distance from the eye (the camera-relative origin) to the sight line.
    const float sightR = line.sighted ? math::length(line.sightO - line.sightD * math::dot(line.sightO, line.sightD)) : 0.0F;
    reportf("ev=vr.aim line origin=%.4f,%.4f,%.4f dir=%.5f,%.5f,%.5f ndc_origin=%d,%.5f,%.5f "
            "ndc_vanish=%d,%.5f,%.5f cam_fwd=%.5f,%.5f,%.5f cam_up=%.5f,%.5f,%.5f hfov=%.5f aspect=%.5f "
            "sighted=%d sight_o=%.4f,%.4f,%.4f sight_d=%.5f,%.5f,%.5f ndc_sight=%d,%.5f,%.5f sight_r=%.4f",
            line.origin.x, line.origin.y, line.origin.z, line.direction.x, line.direction.y, line.direction.z,
            originOk ? 1 : 0, ox, oy, vanishOk ? 1 : 0, vx, vy, camera.forward[0], camera.forward[1],
            camera.forward[2], camera.up[0], camera.up[1], camera.up[2], camera.horizontalFov, camera.aspect,
            line.sighted ? 1 : 0, line.sightO.x, line.sightO.y, line.sightO.z, line.sightD.x, line.sightD.y,
            line.sightD.z, sightOk ? 1 : 0, sx, sy, sightR);
}

// ---------------------------------------------------------------------------------------------
// The shot.
//
// Found by tracing the fire path live (docs/AIM.md): the weapon's fire function `+0xCB7680` queues
// a fire event; `+0xCCF720` plays queued events out; `+0xCB48C0` (virtualised) resolves one into
// fire points and hands them to `+0x181E930`, the launcher, which fires one projectile
// per point through `+0x181C8B0` (virtualised). The launcher is where the whole shot is in plain
// data, and its entry is native:
//
//   launch(weapon, flags, FirePoint* points, Vec4* muzzles, int count, ...)
//
//   FirePoint, 0x50 bytes: +0x10 orientation quaternion (x, y, z, w), +0x20 position (x, y, z, 1)
//   muzzles, 0x10 bytes each: a world point per fire point
//
// Everything is world space, game axes. The orientation is the character's aim -- which is why the
// shot followed the character and nothing else. Rewriting the fire points here, before the
// launcher reads them, makes the shot leave the VR gun's muzzle along its barrel: no lag (the
// weapon pose of this very frame), the character's facing untouched, spread and recoil still the
// game's own (applied downstream, relative to the orientation it is given).
// ---------------------------------------------------------------------------------------------

constexpr std::uintptr_t kLaunchRva = 0x181E930;
constexpr std::array<std::uint8_t, 19> kLaunchPrologue{0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
                                                       0x57, 0x48, 0x8D, 0xAC, 0x24, 0x10, 0xFF, 0xFF, 0xFF};
constexpr std::size_t kFirePointStride = 0x50;
constexpr std::size_t kFirePointQuat = 0x10;
constexpr std::size_t kFirePointPos = 0x20;
constexpr std::size_t kMuzzleStride = 0x10;
constexpr std::int64_t kMaxFirePoints = 64;

enum class ShotMode : int { off = 0, observe = 1, aim = 2 };
std::atomic<int> g_shotMode{static_cast<int>(ShotMode::aim)};
std::atomic_bool g_launchTried{false};
std::atomic_bool g_launchHooked{false};
std::atomic<std::uint64_t> g_shots{0};
std::atomic<std::uint64_t> g_shotsRewritten{0};
std::atomic<std::uint64_t> g_shotsForeign{0};

/** The last shot, for the log (written on the firing thread, reported on the present thread). */
struct ShotRecord final {
    std::uint64_t serial{};
    /** The weapon object the launch was for (the launcher's first argument). */
    std::uintptr_t weapon{};
    int count{};
    bool rewritten{};
    V3 origPos{};
    V3 origFwd{};
    V3 origMuzzle{};
    V3 newPos{};
    V3 newFwd{};
    V3 eye{};
    V3 engineFwd{};
    /** The direction came from the learned sight (zeroed), not the bare barrel. */
    bool sighted{};
};
SRWLOCK g_shotLock = SRWLOCK_INIT;

/**
 * Every fire point this thread's launch rewrite last wrote, so the round's trace can recognise its
 * own rounds: a multi-point pattern (pellets, twin barrels) gives each round its own start and
 * direction, and a trace that matched only the first would hand rounds 2..N back to the camera.
 */
constexpr std::size_t kRememberedPoints = 16;
struct LastBarrel final {
    std::array<V3, kRememberedPoints> start{};
    std::array<V3, kRememberedPoints> dir{};
    std::size_t count{};
    ULONGLONG tick{};
    bool valid{};
};
thread_local LastBarrel t_lastBarrel{};
ShotRecord g_lastShot{};
std::uint64_t g_reportedShot = 0;

V3 load3(const std::uint8_t* p) noexcept {
    float f[3];
    std::memcpy(f, p, sizeof f);
    return V3{f[0], f[1], f[2]};
}

/**
 * The barrel in the world, this frame: the orientation the weapon module handed the engine (seat
 * and second hand included) and the muzzle, both relative to the character's eye -- the same
 * composition the gun is drawn with: `eye_engine + palm + R * muzzle`.
 */
bool barrel_world(V3& muzzle, M3& basis, bool* sighted = nullptr) noexcept {
    const Shot s = shot_geometry();
    if (!s.valid) {
        return false;
    }
    const Pose engine = engine_pose();
    const V3 eye{engine.position[0], engine.position[1], engine.position[2]};
    muzzle = eye + s.muzzle;
    basis = s.basis;
    if (sighted != nullptr) {
        *sighted = s.sighted;
    }
    return math::finite(muzzle) && math::finite(basis.c[0]);
}

/**
 * The LEFT gun's barrel in the world, this frame: the dual-wield module's left shot (the right gun's
 * shot as measured on that weapon, mirrored onto the left controller), carried from the room into
 * the world exactly as the right gun's is.
 */
bool left_barrel_world(V3& muzzle, M3& basis, bool* sighted = nullptr) noexcept {
    math::Affine shot{};
    bool isSighted = false;
    if (!dual::left_shot(shot, &isSighted)) {
        return false;
    }
    const Pose engine = engine_pose();
    const V3 eye{engine.position[0], engine.position[1], engine.position[2]};
    muzzle = eye + shot.t;
    basis = math::orthonormalize(shot.m);
    if (sighted != nullptr) {
        *sighted = isSighted;
    }
    return math::finite(muzzle) && math::finite(basis.c[0]);
}

/** Set by the launch rewrite for the round being launched on this thread: which gun it leaves from. */
thread_local bool t_launchLeft = false;

void on_launch(std::uint64_t, std::uint8_t* frame) noexcept {
    const auto mode = static_cast<ShotMode>(g_shotMode.load(std::memory_order_relaxed));
    if (mode == ShotMode::off) {
        return;
    }
    const auto points = static_cast<std::uintptr_t>(prehook::arg(frame, 2));
    const auto muzzles = static_cast<std::uintptr_t>(prehook::arg(frame, 3));
    const auto count = static_cast<std::int64_t>(static_cast<std::int32_t>(prehook::arg(frame, 4)));
    if (points == 0 || muzzles == 0 || count < 1 || count > kMaxFirePoints) {
        return;
    }
    g_shots.fetch_add(1, std::memory_order_relaxed);
    std::array<std::uint8_t, kFirePointStride> first{};
    std::array<float, 4> firstMuzzle{};
    if (!prehook::read(points, first.data(), first.size()) || !prehook::read(muzzles, firstMuzzle.data(), 16)) {
        return;
    }
    std::array<float, 4> q0{};
    std::memcpy(q0.data(), first.data() + kFirePointQuat, sizeof q0);
    const V3 p0 = load3(first.data() + kFirePointPos);
    const V3 m0{firstMuzzle[0], firstMuzzle[1], firstMuzzle[2]};
    const M3 r0 = math::mat_of(q0);
    const Pose engine = engine_pose();
    const V3 eye{engine.position[0], engine.position[1], engine.position[2]};
    const V3 engineFwd{engine.forward[0], engine.forward[1], engine.forward[2]};

    ShotRecord rec{};
    rec.weapon = static_cast<std::uintptr_t>(prehook::arg(frame, 0));
    rec.count = static_cast<int>(count);
    rec.origPos = p0;
    rec.origFwd = r0.c[0];
    rec.origMuzzle = m0;
    rec.eye = eye;
    rec.engineFwd = engineFwd;

    // Only the local player's own shot: it starts within reach of this player's eye and leaves
    // along this player's aim. Anything else that launches -- another player, an AI, a turret --
    // is left exactly as the game made it.
    // The left gun's rounds are recognised by the weapon they are launched for (or, for a twin of the
    // weapon in the right hand, by the dual-wield module's own record of who pulled which trigger):
    // they leave from wherever the game keeps that weapon -- its holster -- which no position test
    // would accept.
    const bool left = enabled() && dual::launch_hand(rec.weapon) == 1;
    t_launchLeft = left;
    const bool mine = left || (math::length(p0 - eye) < 2.0F && math::dot(r0.c[0], engineFwd) > 0.9F && enabled());
    V3 muzzle{};
    M3 barrel{};
    const bool placed = left ? left_barrel_world(muzzle, barrel, &rec.sighted) : barrel_world(muzzle, barrel, &rec.sighted);
    if (mode == ShotMode::aim && mine && placed) {
        // D carries the game's fire rig onto the barrel: a rotation from the character's aim to the
        // gun's, applied to every fire point and every offset between them, so a multi-barrel or
        // multi-pellet pattern keeps its shape and simply leaves from the gun.
        const M3 d = barrel * math::transpose(r0);
        LastBarrel remembered{};
        for (std::int64_t i = 0; i < count; ++i) {
            const std::uintptr_t entry = points + static_cast<std::uintptr_t>(i) * kFirePointStride;
            const std::uintptr_t mz = muzzles + static_cast<std::uintptr_t>(i) * kMuzzleStride;
            std::array<float, 4> q{};
            std::array<float, 4> pos{};
            std::array<float, 4> m{};
            if (!prehook::read(entry + kFirePointQuat, q.data(), 16) || !prehook::read(entry + kFirePointPos, pos.data(), 16)
                || !prehook::read(mz, m.data(), 16)) {
                break;
            }
            const std::array<float, 4> nq = math::quat_of(d * math::mat_of(q));
            const V3 np = muzzle + d * (V3{pos[0], pos[1], pos[2]} - p0);
            const V3 nm = muzzle + d * (V3{m[0], m[1], m[2]} - m0);
            const std::array<float, 4> outPos{np.x, np.y, np.z, pos[3]};
            const std::array<float, 4> outMuzzle{nm.x, nm.y, nm.z, m[3]};
            static_cast<void>(prehook::write(entry + kFirePointQuat, nq.data(), 16));
            static_cast<void>(prehook::write(entry + kFirePointPos, outPos.data(), 16));
            static_cast<void>(prehook::write(mz, outMuzzle.data(), 16));
            if (remembered.count < kRememberedPoints) {
                remembered.start[remembered.count] = np;
                remembered.dir[remembered.count] = math::mat_of(nq).c[0];
                ++remembered.count;
            }
        }
        remembered.tick = GetTickCount64();
        remembered.valid = remembered.count != 0;
        t_lastBarrel = remembered;
        rec.rewritten = true;
        rec.newPos = muzzle;
        rec.newFwd = barrel.c[0];
        g_shotsRewritten.fetch_add(1, std::memory_order_relaxed);
    } else if (!mine) {
        g_shotsForeign.fetch_add(1, std::memory_order_relaxed);
    }
    AcquireSRWLockExclusive(&g_shotLock);
    rec.serial = g_lastShot.serial + 1;
    g_lastShot = rec;
    ReleaseSRWLockExclusive(&g_shotLock);
}

// ---------------------------------------------------------------------------------------------
// The hit.
//
// Moving the fire points moves the bullet's start and the visuals -- muzzle flash, tracer -- but not
// where it lands, because the game aims every round the way shooters do: it traces the CAMERA's ray
// to find what is under the crosshair and sends the round from the muzzle to that point. The round
// (`+0x181C8B0`, virtualised) gets that ray from `+0x12B0390`, which asks `+0x12B0950`:
//
//   aim_ray(&player, flags, ..., Vec3* origin, Vec3* direction, Vec3* eye, int* result)
//
// whose origin, direction and eye come from the camera block or from a protected view-ray cache
// (`+0x1285110` -> `+0x1284CC0`, an obfuscated singleton). The same function also runs once a frame
// for other consumers (`+0x12B0C28`); only the call the round makes, returning to `+0x12B06EA`, is
// rewritten, after it returns, with the barrel: origin and eye at the muzzle, direction along the
// barrel. The round then traces the barrel's line and flies there -- from the muzzle the launcher
// rewrite already gave it.
// ---------------------------------------------------------------------------------------------

constexpr std::uintptr_t kAimRayRva = 0x12B0950;
constexpr std::uintptr_t kAimRayFromRoundRva = 0x12B06EA;
constexpr std::array<std::uint8_t, 13> kAimRayPrologue{0x4C, 0x8B, 0xDC, 0x56, 0x41, 0x54, 0x48,
                                                       0x81, 0xEC, 0x98, 0x00, 0x00, 0x00};
std::atomic_bool g_aimRayHooked{false};
std::atomic<std::uint64_t> g_raysRewritten{0};
std::uintptr_t g_base = 0;

/** The three output buffers of the round's aim-ray call, between its entry and its return. */
struct AimRayCall final {
    std::uintptr_t origin{};
    std::uintptr_t direction{};
    std::uintptr_t eye{};
};
thread_local AimRayCall t_aimRay{};

/** What the round was really sent along, for the shot record. */
struct RayRecord final {
    std::uint64_t serial{};
    V3 origOrigin{};
    V3 origDir{};
    V3 newOrigin{};
    V3 newDir{};
};
RayRecord g_lastRay{};

void after_aim_ray(std::uint64_t) noexcept {
    const AimRayCall call = t_aimRay;
    std::array<float, 3> o{};
    std::array<float, 3> d{};
    std::array<float, 3> e{};
    if (!prehook::read(call.origin, o.data(), 12) || !prehook::read(call.direction, d.data(), 12)
        || !prehook::read(call.eye, e.data(), 12)) {
        return;
    }
    const Pose engine = engine_pose();
    const V3 eye{engine.position[0], engine.position[1], engine.position[2]};
    const V3 engineFwd{engine.forward[0], engine.forward[1], engine.forward[2]};
    const V3 gotEye{e[0], e[1], e[2]};
    const V3 gotDir{d[0], d[1], d[2]};
    // The local player's ray only: from this player's eye, along this player's aim.
    if (math::length(gotEye - eye) > 1.0F || math::dot(math::normalize(gotDir), engineFwd) < 0.9F || !enabled()) {
        return;
    }
    V3 muzzle{};
    M3 barrel{};
    if (!(t_launchLeft ? left_barrel_world(muzzle, barrel) : barrel_world(muzzle, barrel))) {
        return;
    }
    const V3 dir = barrel.c[0];
    const std::array<float, 3> m{muzzle.x, muzzle.y, muzzle.z};
    const std::array<float, 3> f{dir.x, dir.y, dir.z};
    static_cast<void>(prehook::write(call.origin, m.data(), 12));
    static_cast<void>(prehook::write(call.direction, f.data(), 12));
    static_cast<void>(prehook::write(call.eye, m.data(), 12));
    g_raysRewritten.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_shotLock);
    g_lastRay = RayRecord{g_lastRay.serial + 1, V3{o[0], o[1], o[2]}, gotDir, muzzle, dir};
    ReleaseSRWLockExclusive(&g_shotLock);
}

void on_aim_ray(std::uint64_t, std::uint8_t* frame) noexcept {
    if (static_cast<ShotMode>(g_shotMode.load(std::memory_order_relaxed)) != ShotMode::aim
        || prehook::return_address(frame) != g_base + kAimRayFromRoundRva) {
        return;
    }
    t_aimRay = AimRayCall{static_cast<std::uintptr_t>(prehook::arg(frame, 4)),
                          static_cast<std::uintptr_t>(prehook::arg(frame, 5)),
                          static_cast<std::uintptr_t>(prehook::arg(frame, 6))};
    if (t_aimRay.origin == 0 || t_aimRay.direction == 0 || t_aimRay.eye == 0) {
        return;
    }
    static_cast<void>(prehook::request_post(frame, &after_aim_ray, 0));
}

void install_aim_ray() noexcept {
    std::array<std::uint8_t, kAimRayPrologue.size()> bytes{};
    if (!prehook::read(g_base + kAimRayRva, bytes.data(), bytes.size()) || bytes != kAimRayPrologue) {
        report_text("ev=vr.aim aimray hook result=fail reason=prologue_mismatch");
        return;
    }
    const char* failure = "";
    if (!prehook::attach(g_base + kAimRayRva, &on_aim_ray, 0, failure)) {
        reportf("ev=vr.aim aimray hook result=fail reason=%s", failure);
        return;
    }
    g_aimRayHooked.store(true, std::memory_order_release);
    report_text("ev=vr.aim aimray hook result=ok rva=12B0950");
}

// ---------------------------------------------------------------------------------------------
// The round's own trace.
//
// The round is traced by `+0x181F680(&player, Vec4 ray[2] {start, direction}, ..., stack flags)`.
// It arrives carrying the barrel (the two rewrites above), but two of its flags would undo that
// before the trace:
//
//   arg 7  replace the direction with the aim stored on the character (actor +0xB20 / +0xB14)
//   arg 6  move the start onto the eye's line (eye + dir * depth), so the round "leaves the eye"
//
// Both are the flat game's way of making the round go where the crosshair is. For this round they
// are cleared, and nothing else is touched: the spread the function adds next stays the game's, and
// so does the eye it passes on to the trace, which is what stops a muzzle pushed through a wall from
// shooting through it.
//
// Recognised by content, not by caller (the caller is virtualised): the start and direction must be
// exactly one of the fire points this thread's launch rewrite just wrote. Any other trace -- another
// actor's, a different weapon system's -- cannot match.
// ---------------------------------------------------------------------------------------------

constexpr std::uintptr_t kRoundTraceRva = 0x181F680;
constexpr std::array<std::uint8_t, 19> kRoundTracePrologue{0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x57, 0x41, 0x55, 0x49, 0x8D,
                                                           0x6B, 0xD1, 0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00};
constexpr std::size_t kArgProjectStart = 6;
constexpr std::size_t kArgUseActorAim = 7;
std::atomic<std::uint64_t> g_tracesKept{0};


void on_round_trace(std::uint64_t, std::uint8_t* frame) noexcept {
    if (static_cast<ShotMode>(g_shotMode.load(std::memory_order_relaxed)) != ShotMode::aim || !t_lastBarrel.valid
        || GetTickCount64() - t_lastBarrel.tick > 500) {
        return;
    }
    const auto ray = static_cast<std::uintptr_t>(prehook::arg(frame, 1));
    std::array<float, 8> v{};
    if (ray == 0 || !prehook::read(ray, v.data(), sizeof v)) {
        return;
    }
    const V3 start{v[0], v[1], v[2]};
    const V3 dir{v[4], v[5], v[6]};
    bool ours = false;
    for (std::size_t i = 0; i < t_lastBarrel.count && !ours; ++i) {
        ours = math::length(start - t_lastBarrel.start[i]) < 0.02F && math::dot(dir, t_lastBarrel.dir[i]) > 0.9999F;
    }
    if (!ours) {
        return;
    }
    // Stack arguments are bytes in 8-byte slots; the callee reads the low byte.
    const std::uint8_t zero = 0;
    const std::uintptr_t slot = reinterpret_cast<std::uintptr_t>(frame) + prehook::kReturn + 0x28;
    static_cast<void>(prehook::write(slot + 8 * (kArgProjectStart - 4), &zero, 1));
    static_cast<void>(prehook::write(slot + 8 * (kArgUseActorAim - 4), &zero, 1));
    g_tracesKept.fetch_add(1, std::memory_order_relaxed);
}

void install_round_trace() noexcept {
    std::array<std::uint8_t, kRoundTracePrologue.size()> bytes{};
    if (!prehook::read(g_base + kRoundTraceRva, bytes.data(), bytes.size()) || bytes != kRoundTracePrologue) {
        report_text("ev=vr.aim trace hook result=fail reason=prologue_mismatch");
        return;
    }
    const char* failure = "";
    if (!prehook::attach(g_base + kRoundTraceRva, &on_round_trace, 0, failure)) {
        reportf("ev=vr.aim trace hook result=fail reason=%s", failure);
        return;
    }
    report_text("ev=vr.aim trace hook result=ok rva=181F680");
}

void install_launch() noexcept {
    if (g_launchTried.exchange(true)) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    g_base = base;
    install_aim_ray();
    install_round_trace();
    std::array<std::uint8_t, kLaunchPrologue.size()> bytes{};
    if (!prehook::read(base + kLaunchRva, bytes.data(), bytes.size()) || bytes != kLaunchPrologue) {
        report_text("ev=vr.aim launch hook result=fail reason=prologue_mismatch");
        return;
    }
    const char* failure = "";
    if (!prehook::attach(base + kLaunchRva, &on_launch, 0, failure)) {
        reportf("ev=vr.aim launch hook result=fail reason=%s", failure);
        return;
    }
    g_launchHooked.store(true, std::memory_order_release);
    report_text("ev=vr.aim launch hook result=ok rva=181E930");
}

/** One line per shot, after the fact: what the game aimed, and what was fired. */
void report_shot() noexcept {
    AcquireSRWLockShared(&g_shotLock);
    const ShotRecord s = g_lastShot;
    const RayRecord ray = g_lastRay;
    ReleaseSRWLockShared(&g_shotLock);
    if (s.serial == g_reportedShot) {
        return;
    }
    g_reportedShot = s.serial;
    const float turn = std::acos(std::clamp(math::dot(s.origFwd, s.newFwd), -1.0F, 1.0F)) * 57.29578F;
    reportf("ev=vr.aim shot serial=%llu weapon=%p points=%d rewritten=%d orig_pos=%.3f,%.3f,%.3f orig_fwd=%.4f,%.4f,%.4f "
            "orig_muzzle=%.3f,%.3f,%.3f new_pos=%.3f,%.3f,%.3f new_fwd=%.4f,%.4f,%.4f eye=%.3f,%.3f,%.3f "
            "engine_fwd=%.4f,%.4f,%.4f turned_deg=%.2f sighted=%d total=%llu rewritten_total=%llu foreign=%llu",
            static_cast<unsigned long long>(s.serial), reinterpret_cast<void*>(s.weapon), s.count, s.rewritten ? 1 : 0, s.origPos.x, s.origPos.y,
            s.origPos.z, s.origFwd.x, s.origFwd.y, s.origFwd.z, s.origMuzzle.x, s.origMuzzle.y, s.origMuzzle.z,
            s.newPos.x, s.newPos.y, s.newPos.z, s.newFwd.x, s.newFwd.y, s.newFwd.z, s.eye.x, s.eye.y, s.eye.z,
            s.engineFwd.x, s.engineFwd.y, s.engineFwd.z, s.rewritten ? turn : 0.0F, s.sighted ? 1 : 0,
            static_cast<unsigned long long>(g_shots.load()), static_cast<unsigned long long>(g_shotsRewritten.load()),
            static_cast<unsigned long long>(g_shotsForeign.load()));
    reportf("ev=vr.aim ray serial=%llu orig_origin=%.3f,%.3f,%.3f orig_dir=%.4f,%.4f,%.4f new_origin=%.3f,%.3f,%.3f "
            "new_dir=%.4f,%.4f,%.4f rays_rewritten=%llu traces_kept=%llu",
            static_cast<unsigned long long>(ray.serial), ray.origOrigin.x, ray.origOrigin.y, ray.origOrigin.z,
            ray.origDir.x, ray.origDir.y, ray.origDir.z, ray.newOrigin.x, ray.newOrigin.y, ray.newOrigin.z, ray.newDir.x,
            ray.newDir.y, ray.newDir.z, static_cast<unsigned long long>(g_raysRewritten.load()),
            static_cast<unsigned long long>(g_tracesKept.load()));
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
        if (std::strcmp(verb, "ray") == 0 && n >= 2) {
            g_ray.store(std::strcmp(a, "off") != 0, std::memory_order_release);
            reportf("ev=vr.aim ray=%s", g_ray.load() ? "on" : "off");
        } else if (std::strcmp(verb, "muzzle") == 0) {
            float v[3] = {};
            if (sscanf_s(line.data(), "%*s %f %f %f", &v[0], &v[1], &v[2]) == 3) {
                for (std::size_t i = 0; i < 3; ++i) {
                    g_muzzle[i].store(v[i], std::memory_order_relaxed);
                }
                reportf("ev=vr.aim muzzle fwd=%.3f right=%.3f up=%.3f", v[0], v[1], v[2]);
            }
        } else if (std::strcmp(verb, "length") == 0) {
            float v = 0.0F;
            if (sscanf_s(line.data(), "%*s %f", &v) == 1 && v > 0.0F) {
                g_length.store(v, std::memory_order_relaxed);
            }
        } else if (std::strcmp(verb, "shot") == 0 && n >= 2) {
            // `shot off|observe|aim`: leave the game's shot alone, only record it, or fire it from
            // the barrel (the default).
            const ShotMode mode = std::strcmp(a, "off") == 0       ? ShotMode::off
                                  : std::strcmp(a, "observe") == 0 ? ShotMode::observe
                                                                   : ShotMode::aim;
            g_shotMode.store(static_cast<int>(mode), std::memory_order_relaxed);
            reportf("ev=vr.aim shot_mode=%s", a);
        } else if (std::strcmp(verb, "sight") == 0 && n >= 2) {
            // `sight on|off`: zero the shot onto the learned sight line (default), or fire along the
            // bare barrel as before the sight was known.
            g_sightOn.store(std::strcmp(a, "off") != 0, std::memory_order_relaxed);
            reportf("ev=vr.aim sight=%s", a);
        } else if (std::strcmp(verb, "dump") == 0) {
            g_dumpNow.store(true, std::memory_order_release);
        } else {
            reportf("ev=vr.aim command result=fail verb=%s", verb);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

void draw(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain, const Line& line,
          const gpu::ViewCamera& camera) noexcept {
    ID3D11RenderTargetView* const target = gpu::back_buffer_view(device, swapChain);
    if (target == nullptr) {
        return;
    }
    const float length = g_length.load(std::memory_order_relaxed);
    const V3 end = line.origin + line.direction * length;
    // Pure magenta: a colour Io never shows, so the harness can find the line by colour alone. The
    // sight line, when known, in pure cyan beside it: the two meet at the zero distance.
    const wl::Color color{255, 0, 255, 255};
    const wl::Color sightColor{0, 255, 255, 255};
    const V3 sightStart = line.sightO + line.sightD * std::max(0.0F, math::dot(line.origin - line.sightO, line.sightD));
    const V3 sightEnd = line.sightO + line.sightD * length;
    const std::array<wl::Edge, 2> edges{
        wl::Edge{{line.origin.x, line.origin.y, line.origin.z}, {end.x, end.y, end.z}, color},
        wl::Edge{{sightStart.x, sightStart.y, sightStart.z}, {sightEnd.x, sightEnd.y, sightEnd.z}, sightColor}};
    const std::array<wl::Sphere, 1> spheres{
        wl::Sphere{{line.origin.x, line.origin.y, line.origin.z}, 0.006F, 10, color}};
    teleport::CameraPose pose{};
    pose.position = {0.0F, 0.0F, 0.0F};
    pose.forward = camera.forward;
    pose.up = camera.up;
    pose.horizontalFov = camera.horizontalFov;
    pose.aspect = camera.aspect;
    wl::Batch batch{};
    batch.edges = std::span<const wl::Edge>(edges.data(), line.sighted ? 2U : 1U);
    batch.spheres = spheres;
    batch.lineWidthPixels = 2.0F;
    static_cast<void>(wl::draw(device, context, target, pose, batch));
}

} // namespace

bool right_shot_in_controller(math::Affine& shot, bool* sighted) noexcept {
    const Shot s = shot_geometry();
    if (!s.valid) {
        return false;
    }
    // Against the RAW controller (before the rotational seat): the frame the dual-wield module places
    // the left gun in, and the one the recorded gun was measured against.
    xr::HeadPose h{};
    xr::HandPose l{};
    xr::HandPose r{};
    (void)xr::camera_sample(h, l, r);
    if (!r.valid) {
        return false;
    }
    const M3 raw = math::orthonormalize(M3{{v3(r.forward), -v3(r.right), v3(r.up)}});
    const V3 palm = v3(r.palm);
    shot.m = math::transpose(raw) * s.basis;
    shot.t = math::transpose(raw) * (s.muzzle - palm);
    if (sighted != nullptr) {
        *sighted = s.sighted;
    }
    return math::finite(shot.t);
}

void present(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) noexcept {
    ++g_frame;
    if (g_frame % kPollPeriod == 0) {
        poll_commands();
    }
    // The shot is redirected from the first frame VR is up, like the weapon's own configuration:
    // before F9 nothing here touches the engine.
    if (!g_launchTried.load(std::memory_order_acquire) && enabled() && xr::active() && !debug::has("nodetour aim")) {
        install_launch();
    }
    report_shot();
    const bool ray = g_ray.load(std::memory_order_acquire);
    const bool dump = g_dumpNow.exchange(false, std::memory_order_acq_rel);
    if (!ray && !dump) {
        return;
    }
    gpu::ViewCamera camera{};
    if (!gpu::viewmodel_camera(camera)) {
        return;
    }
    const Line line = current_line();
    if (!line.valid) {
        return;
    }
    if (ray && device != nullptr && context != nullptr && swapChain != nullptr) {
        draw(device, context, swapChain, line, camera);
    }
    if (dump || (ray && g_frame % kReportPeriod == 0)) {
        report_line(line, camera);
    }
}

} // namespace sunrise::client::hooks::vr::aim
