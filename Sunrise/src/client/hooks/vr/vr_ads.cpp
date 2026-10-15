/**
 * Aiming down the sights (docs/ADS.md). See vr_ads.h for the three jobs and the frames.
 *
 * Why the model is two POSES rather than a sight table: Destiny's ADS animation is authored to put
 * each weapon's sight exactly on the flat camera's axis, so the pose the game animates the hand to in
 * ADS, relative to the pose it holds at the hip, carries everything this module needs -- what to undo
 * so the gun stays in the hand, and where the sight is on the gun -- for any weapon, iron sight or
 * optic, without a single hand-measured number. It is the same move that found the foregrip
 * (vr_arms.cpp): let Bungie's animation tell us where things are.
 *
 * The ADS PROGRESS is the engine's own: ADS narrows the engine's FOV in step with the animation, and
 * the camera hook sees the engine's value before writing the VR one over it (the zoom never reaches
 * the image). `zoom = tan(hip/2) / tan(fov/2)`, and the fraction is how far that is towards the
 * weapon's full ADS zoom, which is learned with its poses.
 *
 * `SVR_Ads.txt`, one command per line, consumed on read:
 *
 *   mode auto|button|off       physical ADS (default) | left trigger | none
 *   enter <cm> <deg>           sight line within this of the eye, and within this of the gaze (4 12)
 *   exit <cm> <deg>            leaves beyond these (6 20)
 *   dwell <ms_in> <ms_out>     how long a condition must hold before it switches (60 150)
 *   reach <m>                  the gun must be at least this far ahead of the eye (0.08)
 *   zero <m>                   the shot meets the sight line at this distance (25)
 *   comp on|off                cancel the ADS animation (default on)
 *   learn on|off               learn poses (default on)
 *   haptic on|off              the tick on entering ADS (default on)
 *   force on|off               hold ADS regardless of the sight (tests, and teaching a new weapon)
 *   proxy auto|hand|gun        which drawn bone stands for the gun (default auto: the weapon's root)
 *   eye center|left|right      the eye the mono image is rendered from (xr_runtime)
 *   forget                     drop the profile in hand; `forget all` drops every one
 *   trace <seconds>|stop       per-frame log of the ADS progress and the drawn pose, to SVR_AdsTrace.csv
 *   dump                       everything, now
 */

#include "vr_ads.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "../../../core/logging/log.h"
#include "runtime.h"
#include "vr_arms.h"
#include "vr_gamepad.h"
#include "vr_gpu.h"
#include "vr_weapon.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::ads {
namespace {

using namespace math;

constexpr wchar_t kCommandName[] = L"SVR_Ads.txt";
constexpr wchar_t kProfilesName[] = L"SVR_AdsProfiles.txt";
constexpr wchar_t kTraceName[] = L"SVR_AdsTrace.csv";
constexpr std::uint32_t kPollPeriod = 30;
constexpr std::uint64_t kReportMs = 3000;
constexpr float kDeg = 3.14159265F / 180.0F;
constexpr std::size_t kMaxProfiles = 32;

// ---------------------------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------------------------

std::atomic<int> g_mode{static_cast<int>(Mode::automatic)};
/**
 * Enter and exit, as distance from the eye to the sight line and angle between the line and the
 * gaze. A hysteresis band, because a held gun wobbles by a centimetre and a degree or two, and a
 * threshold without one would chatter the game in and out of ADS. Live-tunable: whether these
 * trigger when the player MEANS to aim is a judgement only the headset can make.
 */
std::atomic<float> g_enterRadius{0.04F};
std::atomic<float> g_enterAngle{12.0F * kDeg};
std::atomic<float> g_exitRadius{0.06F};
std::atomic<float> g_exitAngle{20.0F * kDeg};
std::atomic<int> g_dwellInMs{60};
std::atomic<int> g_dwellOutMs{150};
/** The sight in front of the eye, not behind it or level with it: the gun at least this far ahead. */
std::atomic<float> g_minReach{0.08F};
/**
 * Where the round meets the sight line. A sight sits a few centimetres above the bore; parallel,
 * every round would land that far below the dot at every range. Zeroed like a real sight (Pavlov's
 * red dots are zeroed at 25 m, by its players' measurements), the error is under a degree from a
 * couple of metres out and vanishes at the zero -- invisible against this game's own spread.
 */
std::atomic<float> g_zero{25.0F};
std::atomic_bool g_compOn{true};
std::atomic_bool g_learnOn{true};
std::atomic_bool g_hapticOn{true};
std::atomic_bool g_force{false};
/**
 * What stands for the gun. 0: the drawn right hand. 1: the weapon palette's root. 2 (default): the
 * weapon's root whenever it is seen, the hand only for a weapon that has none. The gun is the better
 * stand-in, measured: through ADS the hand cannon's root moved 106 mm and turned 1.9 deg in the local
 * frame while the hand turned 4.1-4.4 -- the hand re-grips by about 2 deg, and a sight learned from
 * the hand would carry that error.
 */
std::atomic<int> g_proxy{2};
/** When the weapon's palette was last seen (ms), so a frame without it does not flip the stand-in. */
std::uint64_t g_gunSeenAt = 0;

// ---------------------------------------------------------------------------------------------
// The engine's ADS progress (camera thread writes, everyone reads)
// ---------------------------------------------------------------------------------------------

std::atomic<float> g_engineFov{0.0F};
std::atomic<float> g_hipFov{0.0F};
std::atomic<float> g_zoom{1.0F};
/** When the engine's FOV last changed by more than a whisker (ms): a plateau is where it stopped. */
std::atomic<std::uint64_t> g_fovMovedAt{0};
/** Camera thread only: the hip-FOV tracker. */
float t_candidateFov = 0.0F;
std::uint64_t t_candidateSince = 0;
float t_lastFov = 0.0F;

/** @return The zoom the engine is applying now, 1 at the hip. */
float zoom_of(float fov, float hip) noexcept {
    if (!(fov > 0.05F && fov < 3.0F && hip > 0.05F && hip < 3.0F)) {
        return 1.0F;
    }
    return std::tan(hip * 0.5F) / std::tan(fov * 0.5F);
}

// ---------------------------------------------------------------------------------------------
// Profiles: one per weapon, identified by the hip pose its animation holds the hand in
// ---------------------------------------------------------------------------------------------

/**
 * Bins of the ADS curve: the animation's own progress as a function of the FOV's. The two are not
 * eased alike -- measured on the hand cannon, the animation is 77% of the way when the FOV is 54% --
 * but the relation is the same going in and coming out, so it is learned once per weapon.
 */
constexpr std::size_t kCurveBins = 16;

struct Profile final {
    std::uint32_t id{};
    /** The proxy bone in the local frame at the hip, and at full ADS. */
    Affine hip{};
    Affine ads{};
    bool hipSet{};
    bool adsSet{};
    std::uint32_t hipSamples{};
    std::uint32_t adsSamples{};
    /** The engine's zoom at full ADS. */
    float zoomAds{1.0F};
    /** The flat camera in the local frame at full ADS (where the sight was brought to). */
    V3 eyeLocal{};
    int proxy{};
    /**
     * Bones in the weapon's palette (0 for the hand stand-in): part of the weapon's identity. The hip
     * pose alone is not enough -- the auto rifle and the grenade launcher hold the gun 1.6 cm and
     * 4.7 deg apart, and a looser match merged them into one profile.
     */
    std::uint32_t bones{};
    /** The animation's progress at FOV fraction i / kCurveBins; the identity until learned. */
    std::array<float, kCurveBins + 1> curve{};
    std::uint32_t curveSamples{};
    /** Derived, valid when complete: the sight line in the gun frame. */
    V3 sightO{};
    V3 sightD{1.0F, 0.0F, 0.0F};
    bool complete{};
    bool dirty{};
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::array<Profile, kMaxProfiles> g_profiles{};
std::size_t g_profileCount = 0;
std::uint32_t g_nextId = 1;
/** Index of the profile in hand, or -1. Trusted only while `g_currentOk`. */
int g_current = -1;
/**
 * False from the moment the weapon in hand might have changed (a swap press, a steady hip pose
 * that matches no profile) until a steady hip pose matches one again. Nothing is compensated and no
 * learned sight is used while it is false: an ADS undone with another weapon's animation would
 * throw the gun across the screen.
 */
bool g_currentOk = false;
bool g_loaded = false;

/** What the camera and firing threads need, copied out under the lock. */
struct Snapshot final {
    bool ok{};
    Profile p{};
};

/** Bones of the weapon palette seen last (render thread writes), for the detector's fallback. */
std::atomic<std::uint32_t> g_lastGunBones{0};

Snapshot snapshot() noexcept;

/**
 * The profile the detector may use while the one in hand is not trusted: the only complete profile
 * for a weapon whose palette has the bones last seen. None when that is ambiguous or unknown.
 */
bool fallback_profile(Profile& out) noexcept {
    const std::uint32_t bones = g_lastGunBones.load(std::memory_order_relaxed);
    if (bones == 0) {
        return false;
    }
    int found = -1;
    int matches = 0;
    AcquireSRWLockShared(&g_lock);
    for (std::size_t i = 0; i < g_profileCount; ++i) {
        if (g_profiles[i].complete && g_profiles[i].bones == bones) {
            found = static_cast<int>(i);
            ++matches;
        }
    }
    if (matches == 1) {
        out = g_profiles[static_cast<std::size_t>(found)];
    }
    ReleaseSRWLockShared(&g_lock);
    return matches == 1;
}

Snapshot snapshot() noexcept {
    Snapshot s{};
    AcquireSRWLockShared(&g_lock);
    if (g_currentOk && g_current >= 0) {
        s.ok = true;
        s.p = g_profiles[static_cast<std::size_t>(g_current)];
    }
    ReleaseSRWLockShared(&g_lock);
    return s;
}

/** @return Angle of the rotation between two frames, radians. */
float angle_between(const M3& a, const M3& b) noexcept {
    const M3 d = a * transpose(b);
    return std::acos(std::clamp((d.at(0, 0) + d.at(1, 1) + d.at(2, 2) - 1.0F) * 0.5F, -1.0F, 1.0F));
}

void identity_curve(Profile& p) noexcept {
    for (std::size_t i = 0; i <= kCurveBins; ++i) {
        p.curve[i] = static_cast<float>(i) / static_cast<float>(kCurveBins);
    }
}

/** @return The animation's progress at FOV fraction f, from the learned curve. */
float curve_at(const Profile& p, float f) noexcept {
    const float x = std::clamp(f, 0.0F, 1.0F) * static_cast<float>(kCurveBins);
    const auto i = std::min(static_cast<std::size_t>(x), kCurveBins - 1);
    const float t = x - static_cast<float>(i);
    return p.curve[i] + (p.curve[i + 1] - p.curve[i]) * t;
}

/**
 * One observation of the curve, spread over the two bins around f by their interpolation weights,
 * then kept monotonic and pinned at both ends: the animation starts at the hip and ends at full ADS.
 */
void learn_curve(Profile& p, float f, float alpha) noexcept {
    const float x = std::clamp(f, 0.0F, 1.0F) * static_cast<float>(kCurveBins);
    const auto i = std::min(static_cast<std::size_t>(x), kCurveBins - 1);
    const float t = x - static_cast<float>(i);
    constexpr float kRate = 0.35F;
    p.curve[i] += (alpha - p.curve[i]) * kRate * (1.0F - t);
    p.curve[i + 1] += (alpha - p.curve[i + 1]) * kRate * t;
    p.curve[0] = 0.0F;
    p.curve[kCurveBins] = 1.0F;
    for (std::size_t k = 1; k < kCurveBins; ++k) {
        p.curve[k] = std::clamp(std::max(p.curve[k], p.curve[k - 1]), 0.0F, 1.0F);
    }
    ++p.curveSamples;
}

/** Interpolates two poses of one bone, the way an animation blend does: lerp the position, slerp the rotation. */
Affine pose_lerp(const Affine& a, const Affine& b, float t) noexcept {
    return Affine{blend(a.m, b.m, t), a.t + (b.t - a.t) * t};
}

/**
 * Recomputes what a profile implies, once both poses are known.
 *
 * In ADS the sight is on the flat camera's axis: the line through `eyeLocal` along local +X. In the
 * proxy's own frame that line is `ads^-1 * line`; carried to where the proxy sits at the hip, it is
 * `hip * ads^-1 * line` -- the sight line in the gun frame, because the gun frame is by definition
 * the one in which the hand holds its hip pose.
 */
void derive(Profile& p) noexcept {
    p.complete = p.hipSet && p.adsSet && p.zoomAds > 1.01F;
    if (!p.complete) {
        return;
    }
    const Affine back = p.hip * inverse_rigid(p.ads);
    p.sightO = back * p.eyeLocal;
    p.sightD = normalize(back.m * V3{1.0F, 0.0F, 0.0F});
}

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

void write_affine(FILE* f, const Affine& a) noexcept {
    const auto q = quat_of(a.m);
    std::fprintf(f, " %.6f %.6f %.6f %.7f %.7f %.7f %.7f", a.t.x, a.t.y, a.t.z, q[0], q[1], q[2], q[3]);
}

/** Saves every complete profile. Caller holds the lock (shared is enough). */
void save_profiles() noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    if (!game_path(kProfilesName, path)) {
        return;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.data(), L"w") != 0 || f == nullptr) {
        return;
    }
    std::fprintf(f, "# ADS profiles, learned from the game's own animation (vr_ads.cpp). One weapon per line:\n"
                    "# p2 id proxy bones zoomAds  hip(t q)  ads(t q)  eyeLocal  curve(17)\n");
    for (std::size_t i = 0; i < g_profileCount; ++i) {
        const Profile& p = g_profiles[i];
        if (!p.complete) {
            continue;
        }
        std::fprintf(f, "p2 %u %d %u %.5f", p.id, p.proxy, p.bones, p.zoomAds);
        write_affine(f, p.hip);
        write_affine(f, p.ads);
        std::fprintf(f, " %.6f %.6f %.6f", p.eyeLocal.x, p.eyeLocal.y, p.eyeLocal.z);
        for (const float c : p.curve) {
            std::fprintf(f, " %.4f", c);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

void load_profiles() noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    if (!game_path(kProfilesName, path)) {
        return;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.data(), L"r") != 0 || f == nullptr) {
        return;
    }
    std::array<char, 768> line{};
    AcquireSRWLockExclusive(&g_lock);
    while (std::fgets(line.data(), static_cast<int>(line.size()), f) != nullptr && g_profileCount < kMaxProfiles) {
        // "p2 id proxy bones zoomAds hip(7) ads(7) eye(3) curve(17)"
        if (std::strncmp(line.data(), "p2 ", 3) != 0) {
            continue;
        }
        std::array<float, 21 + kCurveBins + 1> v{};
        std::size_t got = 0;
        const char* cursor = line.data() + 3;
        while (got < v.size()) {
            char* next = nullptr;
            const float x = std::strtof(cursor, &next);
            if (next == cursor) {
                break;
            }
            v[got++] = x;
            cursor = next;
        }
        if (got != v.size()) {
            continue;
        }
        Profile p{};
        p.id = static_cast<std::uint32_t>(v[0]);
        p.proxy = static_cast<int>(v[1]);
        p.bones = static_cast<std::uint32_t>(v[2]);
        p.zoomAds = v[3];
        p.hip = Affine{mat_of({v[7], v[8], v[9], v[10]}), V3{v[4], v[5], v[6]}};
        p.ads = Affine{mat_of({v[14], v[15], v[16], v[17]}), V3{v[11], v[12], v[13]}};
        p.eyeLocal = V3{v[18], v[19], v[20]};
        for (std::size_t i = 0; i <= kCurveBins; ++i) {
            p.curve[i] = v[21 + i];
        }
        p.curveSamples = 100;
        p.hipSet = p.adsSet = true;
        p.hipSamples = p.adsSamples = 100;
        derive(p);
        g_profiles[g_profileCount++] = p;
        g_nextId = std::max(g_nextId, p.id + 1);
    }
    const std::size_t count = g_profileCount;
    ReleaseSRWLockExclusive(&g_lock);
    std::fclose(f);
    reportf("ev=vr.ads profiles loaded=%zu", count);
}

// ---------------------------------------------------------------------------------------------
// Learning (render thread)
// ---------------------------------------------------------------------------------------------

/** Render-thread state of the learner. */
struct Learner final {
    std::uint64_t lastSerial{};
    Affine prev{};
    bool prevValid{};
    std::uint64_t stillSince{};
    /** A steady hip pose matching no profile, since (ms). */
    std::uint64_t unknownSince{};
};
Learner g_learner{};

/**
 * The animation's rest detector (render thread), on the drawn hand in the local frame. Separate from
 * the learner's own stillness, which only runs while learning is on and on the gun's stand-in.
 * A recoil turns round at its peak, so a frame or two of stillness mean nothing: rest is declared
 * only after kRestMs of it.
 */
constexpr std::uint64_t kRestMs = 150;
Affine g_restPrev{};
bool g_restPrevValid = false;
std::uint64_t g_restSince = 0;
std::atomic_bool g_atRest{false};

/** Per-frame trace, recorded on the render thread and written out on the present thread. */
struct TraceRow final {
    std::uint64_t ms{};
    float fov{};
    float zoom{};
    float fraction{};
    Affine hand{};
    Affine gun{};
    bool gunValid{};
    std::array<float, 3> lanesIn{};
    int comp{};
    /** The drawn hand and gun relative to the controller: constant if nothing visibly slips. */
    Affine seen{};
    Affine seenGun{};
    /** How far the character lags the gaze (rad): the body servo's step response, in ADS or not. */
    float body{};
};
SRWLOCK g_traceLock = SRWLOCK_INIT;
std::vector<TraceRow> g_trace;
std::uint64_t g_traceUntil = 0;
bool g_traceWanted = false;

float fraction_for(const Profile& p, float zoom) noexcept {
    if (!(p.zoomAds > 1.01F)) {
        return 0.0F;
    }
    return std::clamp((zoom - 1.0F) / (p.zoomAds - 1.0F), 0.0F, 1.0F);
}

/** @return Index of the profile whose hip pose matches, or -1. Caller holds the lock. */
constexpr float kMatchDistance = 0.012F;
constexpr float kMatchAngle = 3.0F * kDeg;

int match(const Affine& pose, int proxy, std::uint32_t bones) noexcept {
    int best = -1;
    float bestCost = 1e9F;
    for (std::size_t i = 0; i < g_profileCount; ++i) {
        const Profile& p = g_profiles[i];
        if (!p.hipSet || p.proxy != proxy || p.bones != bones) {
            continue;
        }
        const float d = length(pose.t - p.hip.t);
        const float a = angle_between(pose.m, p.hip.m);
        if (d < kMatchDistance && a < kMatchAngle) {
            const float cost = d / kMatchDistance + a / kMatchAngle;
            if (cost < bestCost) {
                bestCost = cost;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

/**
 * The curve, from a frame in mid-transition: how far the pose is along its own hip->ADS path
 * (projection of its position onto the path), against the FOV's fraction this frame.
 */
void learn_transition(const Affine& pose, int proxy) noexcept {
    const float zoom = g_zoom.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_lock);
    if (g_currentOk && g_current >= 0) {
        Profile& p = g_profiles[static_cast<std::size_t>(g_current)];
        const float f = fraction_for(p, zoom);
        const V3 path = p.ads.t - p.hip.t;
        const float len2 = dot(path, path);
        if (p.complete && p.proxy == proxy && f > 0.02F && f < 0.98F && len2 > 0.02F * 0.02F) {
            const float alpha = dot(pose.t - p.hip.t, path) / len2;
            const float offPath = length(pose.t - p.hip.t - path * alpha);
            // A pose off its own path (a reload, a swap, recoil in mid-ADS) teaches nothing.
            if (alpha > -0.1F && alpha < 1.1F && offPath < 0.01F) {
                learn_curve(p, f, std::clamp(alpha, 0.0F, 1.0F));
                if (p.curveSamples % 60 == 0) {
                    p.dirty = true;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void learn(const Affine& pose, int proxy, std::uint32_t bones, const weapon::DrawnFrame& frame,
           std::uint64_t nowMs) noexcept {
    Learner& l = g_learner;
    const bool still = l.prevValid && length(pose.t - l.prev.t) < 0.0015F && angle_between(pose.m, l.prev.m) < 0.4F * kDeg;
    l.prev = pose;
    l.prevValid = true;
    if (!still) {
        l.stillSince = 0;
        return;
    }
    if (l.stillSince == 0) {
        l.stillSince = nowMs;
    }
    const std::uint64_t stillMs = nowMs - l.stillSince;
    const float fov = g_engineFov.load(std::memory_order_relaxed);
    const float hip = g_hipFov.load(std::memory_order_relaxed);
    const float zoom = g_zoom.load(std::memory_order_relaxed);
    const std::uint64_t fovStillMs = nowMs - g_fovMovedAt.load(std::memory_order_relaxed);
    if (!(hip > 0.05F) || !(fov > 0.05F)) {
        return;
    }
    const bool atHip = std::fabs(fov - hip) < 0.002F && fovStillMs > 400 && stillMs > 250;
    const bool atAds = zoom > 1.05F && fovStillMs > 250 && stillMs > 250;
    if (!atHip && !atAds) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (atHip) {
        const int found = match(pose, proxy, bones);
        if (found >= 0) {
            l.unknownSince = 0;
            Profile& p = g_profiles[static_cast<std::size_t>(found)];
            if (found != g_current || !g_currentOk) {
                g_current = found;
                g_currentOk = true;
                reportf("ev=vr.ads weapon event=matched id=%u complete=%d", p.id, p.complete ? 1 : 0);
            }
            // The idle pose drifts a little with breathing; follow it slowly -- but only from close by,
            // so a neighbouring weapon's pose can never walk a profile over to itself.
            if (length(pose.t - p.hip.t) < 0.004F && angle_between(pose.m, p.hip.m) < 1.0F * kDeg) {
                p.hip = pose_lerp(p.hip, pose, 0.02F);
                ++p.hipSamples;
                derive(p);
            }
        } else {
            // A steady hip pose nobody knows: another weapon, or a pose that only looks like a hip
            // (sprint holds one for as long as it lasts). Either way the profile in hand is no
            // longer trusted; a new one is started after a second, and only kept on disk once it
            // has been seen in ADS, so sprint poses never reach the file.
            //
            // Not at once, though: a steady pose that is briefly off -- the hand cannon settling after
            // a shot, the end of an ADS exit -- would drop the profile for a few frames and, worse,
            // hand the detector its guessed sight for them. A real change of weapon persists (and a
            // swap press has already distrusted the profile anyway).
            if (l.unknownSince == 0) {
                l.unknownSince = nowMs;
            }
            if (g_currentOk && nowMs - l.unknownSince > 600) {
                g_currentOk = false;
                report("ev=vr.ads weapon event=lost reason=hip_mismatch");
            }
            if (nowMs - l.unknownSince > 1000 && g_profileCount < kMaxProfiles) {
                // Recycle a provisional (never completed) profile before growing the table.
                std::size_t slot = g_profileCount;
                for (std::size_t i = 0; i < g_profileCount; ++i) {
                    if (!g_profiles[i].complete) {
                        slot = i;
                        break;
                    }
                }
                Profile p{};
                p.id = g_nextId++;
                p.proxy = proxy;
                p.bones = bones;
                identity_curve(p);
                p.hip = pose;
                p.hipSet = true;
                p.hipSamples = 1;
                g_profiles[slot] = p;
                if (slot == g_profileCount) {
                    ++g_profileCount;
                }
                g_current = static_cast<int>(slot);
                g_currentOk = true;
                l.unknownSince = 0;
                reportf("ev=vr.ads weapon event=new id=%u bones=%u hip_t=%.3f,%.3f,%.3f", p.id, bones, pose.t.x,
                        pose.t.y, pose.t.z);
            }
        }
    } else if (g_currentOk && g_current >= 0) {
        // Full ADS: the animation has settled with the sight on the flat camera's axis.
        Profile& p = g_profiles[static_cast<std::size_t>(g_current)];
        const V3 eye = transpose(frame.local.m) * (-V3{frame.lanesIn[0], frame.lanesIn[1], frame.lanesIn[2]});
        if (!p.adsSet) {
            p.ads = pose;
            p.zoomAds = zoom;
            p.eyeLocal = eye;
            p.adsSet = true;
            p.adsSamples = 1;
            p.dirty = true;
            derive(p);
            reportf("ev=vr.ads weapon event=ads_learned id=%u zoom=%.4f ads_t=%.3f,%.3f,%.3f move=%.3f turn_deg=%.2f "
                    "sight_o=%.3f,%.3f,%.3f sight_d=%.4f,%.4f,%.4f",
                    p.id, zoom, pose.t.x, pose.t.y, pose.t.z, length(pose.t - p.hip.t),
                    angle_between(pose.m, p.hip.m) / kDeg, p.sightO.x, p.sightO.y, p.sightO.z, p.sightD.x, p.sightD.y,
                    p.sightD.z);
        } else if (std::fabs(zoom - p.zoomAds) > 0.03F * p.zoomAds) {
            // Every weapon has its own ADS zoom: a different one means the profile in hand is not this
            // weapon's, whatever its hip pose looked like. Nothing is compensated until it is sorted out.
            g_currentOk = false;
            reportf("ev=vr.ads weapon event=lost reason=zoom id=%u zoom=%.4f expected=%.4f", p.id, zoom, p.zoomAds);
        } else if (length(pose.t - p.ads.t) < 0.03F && angle_between(pose.m, p.ads.m) < 6.0F * kDeg) {
            p.ads = pose_lerp(p.ads, pose, 0.05F);
            p.zoomAds += (zoom - p.zoomAds) * 0.05F;
            p.eyeLocal = p.eyeLocal + (eye - p.eyeLocal) * 0.05F;
            ++p.adsSamples;
            if (p.adsSamples % 120 == 0) {
                p.dirty = true;
            }
            derive(p);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// ---------------------------------------------------------------------------------------------
// The physical detector (present thread)
// ---------------------------------------------------------------------------------------------

enum class State : int { idle, entering, held, leaving };

struct Detector final {
    State state{State::idle};
    std::uint64_t since{};
    float radius{};
    float gaze{};
    float reach{};
    bool guessed{};
    bool valid{};
    std::uint32_t entries{};
    std::uint32_t exits{};
    bool lastY{};
};
Detector g_det{};
std::atomic_bool g_want{false};
std::uint64_t g_lastReport = 0;
std::uint32_t g_frame = 0;

const char* state_name(State s) noexcept {
    switch (s) {
    case State::idle: return "idle";
    case State::entering: return "entering";
    case State::held: return "held";
    case State::leaving: return "leaving";
    }
    return "?";
}

/**
 * The sight line in camera-relative coordinates -- the frame the image is rendered in, whose origin
 * IS the eye the image is rendered from. With no learned sight, a guess: parallel to the barrel,
 * 9 cm above the palm, which is close enough for the first ADS to happen and teach the real one.
 */
bool sight_in_eye(V3& o, V3& d, V3& palm, xr::HeadPose& head, bool& guessed) noexcept {
    Affine gun{};
    if (!weapon::gun_frame(gun, head)) {
        return false;
    }
    const V3 headOffset{head.offset[0], head.offset[1], head.offset[2]};
    xr::HeadPose unused{};
    xr::HandPose hand{};
    if (!weapon::weapon_sample(unused, hand)) {
        return false;
    }
    palm = V3{hand.palm[0], hand.palm[1], hand.palm[2]} - headOffset;
    Snapshot s = snapshot();
    // Borrow a learned profile only while the weapon in hand is not identified. Identified but never
    // held in ADS is a NEW weapon, and a sight borrowed for it because another weapon has as many
    // palette bones would be wrong; the guess below is honest about being a guess.
    if (!s.ok) {
        s.ok = fallback_profile(s.p);
    }
    if (s.ok && s.p.complete) {
        o = gun * s.p.sightO - headOffset;
        d = normalize(gun.m * s.p.sightD);
        guessed = false;
    } else {
        o = palm + gun.m * V3{0.0F, 0.0F, 0.09F};
        d = gun.m.c[0];
        guessed = true;
    }
    return finite(o) && finite(d);
}

void haptic_tick() noexcept {
    if (g_hapticOn.load(std::memory_order_relaxed)) {
        // Light and short: an acknowledgement, not an event. The grab's click is 0.55 for 25 ms.
        xr::haptic(true, 0.22F, 0.010F);
    }
}

/** World projection against the FOV written: does any zoom reach the image? */
const char* zoom_check(float& p00, float& expected) noexcept {
    float p11 = 0.0F;
    expected = 0.0F;
    p00 = 0.0F;
    const float fov = xr::rendered_fov();
    if (!(fov > 0.1F && fov < 3.1F)) {
        return "nofov";
    }
    expected = 1.0F / std::tan(fov * 0.5F);
    if (!gpu::world_projection(p00, p11)) {
        return "stale";
    }
    return std::fabs(p00 / expected - 1.0F) < 0.01F ? "ok" : "LEAK";
}

void detect(std::uint64_t nowMs) noexcept {
    Detector& d = g_det;
    const auto mode = static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
    V3 o{};
    V3 dir{};
    V3 palm{};
    xr::HeadPose head{};
    bool guessed = false;
    d.valid = enabled() && xr::active() && sight_in_eye(o, dir, palm, head, guessed);
    if (d.valid) {
        // Origin = the eye. Distance from it to the line, the angle from the gaze, and how far ahead
        // the hand is along the line.
        d.radius = length(o - dir * dot(o, dir));
        const V3 gaze{head.forward[0], head.forward[1], head.forward[2]};
        d.gaze = std::acos(std::clamp(dot(dir, normalize(gaze)), -1.0F, 1.0F));
        d.reach = dot(palm, dir);
        d.guessed = guessed;
    }
    const float reach = g_minReach.load(std::memory_order_relaxed);
    const bool in = d.valid && d.radius < g_enterRadius.load(std::memory_order_relaxed)
                    && d.gaze < g_enterAngle.load(std::memory_order_relaxed) && d.reach > reach;
    const bool keep = d.valid && d.radius < g_exitRadius.load(std::memory_order_relaxed)
                      && d.gaze < g_exitAngle.load(std::memory_order_relaxed) && d.reach > reach * 0.5F;
    const auto dwellIn = static_cast<std::uint64_t>(std::max(0, g_dwellInMs.load(std::memory_order_relaxed)));
    const auto dwellOut = static_cast<std::uint64_t>(std::max(0, g_dwellOutMs.load(std::memory_order_relaxed)));
    const State before = d.state;
    switch (d.state) {
    case State::idle:
        if (in) {
            d.state = State::entering;
            d.since = nowMs;
        }
        break;
    case State::entering:
        if (!in) {
            d.state = State::idle;
        } else if (nowMs - d.since >= dwellIn) {
            d.state = State::held;
        }
        break;
    case State::held:
        if (!keep) {
            d.state = State::leaving;
            d.since = nowMs;
        }
        break;
    case State::leaving:
        if (keep) {
            d.state = State::held;
        } else if (nowMs - d.since >= dwellOut) {
            d.state = State::idle;
        }
        break;
    }
    const bool held = d.state == State::held || d.state == State::leaving;
    const bool want = mode == Mode::automatic && held;
    const bool force = g_force.load(std::memory_order_relaxed);
    const bool wasWant = g_want.load(std::memory_order_relaxed);
    g_want.store(want || force, std::memory_order_relaxed);
    if (before != d.state && (d.state == State::held || d.state == State::idle)
        && (before == State::entering || before == State::leaving)) {
        float p00 = 0.0F;
        float expected = 0.0F;
        const char* check = zoom_check(p00, expected);
        if (d.state == State::held) {
            ++d.entries;
            if (want && !wasWant) {
                haptic_tick();
            }
        } else {
            ++d.exits;
        }
        reportf("ev=vr.ads %s r=%.4f gaze_deg=%.2f reach=%.3f guessed=%d zoom_check=%s p00=%.4f expected=%.4f",
                d.state == State::held ? "enter" : "exit", d.radius, d.gaze / kDeg, d.reach, d.guessed ? 1 : 0, check,
                p00, expected);
    }
}

void write_trace() noexcept {
    std::vector<TraceRow> rows;
    AcquireSRWLockExclusive(&g_traceLock);
    rows.swap(g_trace);
    g_traceWanted = false;
    ReleaseSRWLockExclusive(&g_traceLock);
    std::array<wchar_t, MAX_PATH> path{};
    FILE* f = nullptr;
    if (!game_path(kTraceName, path) || _wfopen_s(&f, path.data(), L"w") != 0 || f == nullptr) {
        return;
    }
    std::fprintf(f, "ms,fov,zoom,fraction,comp,hx,hy,hz,hqx,hqy,hqz,hqw,gun,gx,gy,gz,gqx,gqy,gqz,gqw,lx,ly,lz,"
                    "sx,sy,sz,sqx,sqy,sqz,sqw,vx,vy,vz,vqx,vqy,vqz,vqw,body\n");
    for (const TraceRow& r : rows) {
        const auto qh = quat_of(r.hand.m);
        const auto qg = quat_of(r.gun.m);
        const auto qs = quat_of(r.seen.m);
        const auto qv = quat_of(r.seenGun.m);
        std::fprintf(f,
                     "%llu,%.6f,%.6f,%.5f,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
                     "%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.5f\n",
                     static_cast<unsigned long long>(r.ms), r.fov, r.zoom, r.fraction, r.comp, r.hand.t.x, r.hand.t.y,
                     r.hand.t.z, qh[0], qh[1], qh[2], qh[3], r.gunValid ? 1 : 0, r.gun.t.x, r.gun.t.y, r.gun.t.z, qg[0],
                     qg[1], qg[2], qg[3], r.lanesIn[0], r.lanesIn[1], r.lanesIn[2], r.seen.t.x, r.seen.t.y, r.seen.t.z,
                     qs[0], qs[1], qs[2], qs[3], r.seenGun.t.x, r.seenGun.t.y, r.seenGun.t.z, qv[0], qv[1], qv[2],
                     qv[3], r.body);
    }
    std::fclose(f);
    reportf("ev=vr.ads trace written rows=%zu", rows.size());
}

void dump() noexcept {
    AcquireSRWLockShared(&g_lock);
    reportf("ev=vr.ads profiles count=%zu current=%d ok=%d", g_profileCount, g_current, g_currentOk ? 1 : 0);
    for (std::size_t i = 0; i < g_profileCount; ++i) {
        const Profile& p = g_profiles[i];
        reportf("ev=vr.ads profile id=%u proxy=%d bones=%u complete=%d hip_n=%u ads_n=%u zoom=%.4f hip_t=%.4f,%.4f,%.4f "
                "ads_t=%.4f,%.4f,%.4f move=%.4f turn_deg=%.2f eye=%.4f,%.4f,%.4f sight_o=%.4f,%.4f,%.4f "
                "sight_d=%.5f,%.5f,%.5f",
                p.id, p.proxy, p.bones, p.complete ? 1 : 0, p.hipSamples, p.adsSamples, p.zoomAds, p.hip.t.x, p.hip.t.y,
                p.hip.t.z, p.ads.t.x, p.ads.t.y, p.ads.t.z, length(p.ads.t - p.hip.t),
                angle_between(p.ads.m, p.hip.m) / kDeg, p.eyeLocal.x, p.eyeLocal.y, p.eyeLocal.z, p.sightO.x,
                p.sightO.y, p.sightO.z, p.sightD.x, p.sightD.y, p.sightD.z);
        reportf("ev=vr.ads curve id=%u samples=%u at_f=0,.25,.5,.75,1 alpha=%.3f,%.3f,%.3f,%.3f,%.3f", p.id,
                p.curveSamples, curve_at(p, 0.0F), curve_at(p, 0.25F), curve_at(p, 0.5F), curve_at(p, 0.75F),
                curve_at(p, 1.0F));
    }
    ReleaseSRWLockShared(&g_lock);
}

void report_state(std::uint64_t nowMs) noexcept {
    const Detector& d = g_det;
    float p00 = 0.0F;
    float expected = 0.0F;
    const char* check = zoom_check(p00, expected);
    const Snapshot s = snapshot();
    reportf("ev=vr.ads state=%s mode=%d want=%d valid=%d r=%.4f gaze_deg=%.2f reach=%.3f guessed=%d fov=%.4f "
            "hip_fov=%.4f zoom=%.4f fraction=%.3f profile=%u ok=%d complete=%d entries=%u exits=%u zoom_check=%s "
            "eye=%d rest=%d",
            state_name(d.state), g_mode.load(), g_want.load() ? 1 : 0, d.valid ? 1 : 0, d.radius, d.gaze / kDeg, d.reach,
            d.guessed ? 1 : 0, g_engineFov.load(), g_hipFov.load(), g_zoom.load(), engine_fraction(),
            s.ok ? s.p.id : 0U, s.ok ? 1 : 0, s.ok && s.p.complete ? 1 : 0, d.entries, d.exits, check,
            xr::render_eye(), g_atRest.load() ? 1 : 0);
    static_cast<void>(nowMs);
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
        float x = 0.0F;
        float y = 0.0F;
        const bool two = sscanf_s(line.data(), "%*s %f %f", &x, &y) == 2;
        const bool one = sscanf_s(line.data(), "%*s %f", &x) == 1;
        const bool on = n >= 2 && std::strcmp(a, "off") != 0;
        if (std::strcmp(verb, "mode") == 0 && n >= 2) {
            const Mode m = std::strcmp(a, "off") == 0 ? Mode::off
                           : std::strcmp(a, "button") == 0 ? Mode::button
                                                           : Mode::automatic;
            g_mode.store(static_cast<int>(m), std::memory_order_relaxed);
            arms::set_grip_grenade(m == Mode::button);
            reportf("ev=vr.ads mode=%s", a);
        } else if (std::strcmp(verb, "enter") == 0 && two) {
            g_enterRadius.store(x * 0.01F);
            g_enterAngle.store(y * kDeg);
            reportf("ev=vr.ads enter cm=%.2f deg=%.2f", x, y);
        } else if (std::strcmp(verb, "exit") == 0 && two) {
            g_exitRadius.store(x * 0.01F);
            g_exitAngle.store(y * kDeg);
            reportf("ev=vr.ads exit cm=%.2f deg=%.2f", x, y);
        } else if (std::strcmp(verb, "dwell") == 0 && two) {
            g_dwellInMs.store(static_cast<int>(x));
            g_dwellOutMs.store(static_cast<int>(y));
            reportf("ev=vr.ads dwell in_ms=%.0f out_ms=%.0f", x, y);
        } else if (std::strcmp(verb, "reach") == 0 && one) {
            g_minReach.store(x);
            reportf("ev=vr.ads reach m=%.3f", x);
        } else if (std::strcmp(verb, "zero") == 0 && one && x > 1.0F) {
            g_zero.store(x);
            reportf("ev=vr.ads zero m=%.1f", x);
        } else if (std::strcmp(verb, "comp") == 0 && n >= 2) {
            g_compOn.store(on);
            reportf("ev=vr.ads comp=%s", a);
        } else if (std::strcmp(verb, "learn") == 0 && n >= 2) {
            g_learnOn.store(on);
            reportf("ev=vr.ads learn=%s", a);
        } else if (std::strcmp(verb, "haptic") == 0 && n >= 2) {
            g_hapticOn.store(on);
            reportf("ev=vr.ads haptic=%s", a);
        } else if (std::strcmp(verb, "force") == 0 && n >= 2) {
            g_force.store(on);
            reportf("ev=vr.ads force=%s", a);
        } else if (std::strcmp(verb, "proxy") == 0 && n >= 2) {
            g_proxy.store(std::strcmp(a, "gun") == 0 ? 1 : std::strcmp(a, "hand") == 0 ? 0 : 2);
            AcquireSRWLockExclusive(&g_lock);
            g_currentOk = false;
            ReleaseSRWLockExclusive(&g_lock);
            reportf("ev=vr.ads proxy=%s", a);
        } else if (std::strcmp(verb, "eye") == 0 && n >= 2) {
            const int eye = std::strcmp(a, "left") == 0 ? 1 : std::strcmp(a, "right") == 0 ? 2 : 0;
            xr::set_render_eye(eye);
            reportf("ev=vr.ads eye=%s", a);
        } else if (std::strcmp(verb, "forget") == 0) {
            AcquireSRWLockExclusive(&g_lock);
            if (n >= 2 && std::strcmp(a, "all") == 0) {
                g_profileCount = 0;
                g_current = -1;
            } else if (g_current >= 0) {
                g_profiles[static_cast<std::size_t>(g_current)] = g_profiles[g_profileCount - 1];
                --g_profileCount;
                g_current = -1;
            }
            g_currentOk = false;
            save_profiles();
            ReleaseSRWLockExclusive(&g_lock);
            reportf("ev=vr.ads forget=%s", n >= 2 ? a : "current");
        } else if (std::strcmp(verb, "trace") == 0 && ((one && !(x > 0.0F)) || std::strcmp(a, "stop") == 0)) {
            // Ends a running trace now and writes what it holds (a test that does not know its own
            // length in advance starts a long one and stops it).
            AcquireSRWLockExclusive(&g_traceLock);
            g_traceUntil = GetTickCount64();
            ReleaseSRWLockExclusive(&g_traceLock);
            report("ev=vr.ads trace stop");
        } else if (std::strcmp(verb, "trace") == 0 && one) {
            AcquireSRWLockExclusive(&g_traceLock);
            g_trace.clear();
            g_trace.reserve(4096);
            g_traceUntil = GetTickCount64() + static_cast<std::uint64_t>(x * 1000.0F);
            g_traceWanted = true;
            ReleaseSRWLockExclusive(&g_traceLock);
            reportf("ev=vr.ads trace seconds=%.1f", x);
        } else if (std::strcmp(verb, "dump") == 0) {
            dump();
            report_state(GetTickCount64());
        } else {
            reportf("ev=vr.ads command result=fail verb=%s", verb);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------------------------

void note_engine_fov(float radians) noexcept {
    if (!(radians > 0.05F && radians < 3.0F)) {
        return;
    }
    const std::uint64_t nowMs = GetTickCount64();
    if (std::fabs(radians - t_lastFov) > 0.0005F) {
        g_fovMovedAt.store(nowMs, std::memory_order_relaxed);
    }
    t_lastFov = radians;
    g_engineFov.store(radians, std::memory_order_relaxed);
    // The hip FOV is whatever the engine settles on without ADS -- the player's FOV setting. A
    // steady value that is SMALLER than the current estimate is ADS settling or the true hip,
    // so it is taken only when nothing holds ADS; sprint widens the FOV, so a larger steady value
    // is taken only after it has persisted a long while (a changed setting), never from a sprint.
    const bool adsHeld = g_want.load(std::memory_order_relaxed) || gamepad::aim_held();
    if (std::fabs(radians - t_candidateFov) > 0.0005F) {
        t_candidateFov = radians;
        t_candidateSince = nowMs;
    }
    const float hip = g_hipFov.load(std::memory_order_relaxed);
    const std::uint64_t steady = nowMs - t_candidateSince;
    if (hip <= 0.0F) {
        if (!adsHeld && steady > 1000) {
            g_hipFov.store(t_candidateFov, std::memory_order_relaxed);
            reportf("ev=vr.ads hip_fov=%.4f", t_candidateFov);
        }
    } else if (!adsHeld && std::fabs(t_candidateFov - hip) > 0.0005F) {
        const bool narrower = t_candidateFov < hip;
        if ((narrower && steady > 1500 && zoom_of(t_candidateFov, hip) < 1.05F) || (!narrower && steady > 10000)) {
            g_hipFov.store(t_candidateFov, std::memory_order_relaxed);
            reportf("ev=vr.ads hip_fov=%.4f was=%.4f", t_candidateFov, hip);
        }
    }
    g_zoom.store(zoom_of(radians, g_hipFov.load(std::memory_order_relaxed)), std::memory_order_relaxed);
}

float engine_fraction() noexcept {
    const float zoom = g_zoom.load(std::memory_order_relaxed);
    const Snapshot s = snapshot();
    if (s.ok && s.p.complete) {
        return fraction_for(s.p, zoom);
    }
    return zoom > 1.02F ? 1.0F : 0.0F;
}

bool compensation(Affine& delta) noexcept {
    if (!g_compOn.load(std::memory_order_relaxed)) {
        return false;
    }
    const Snapshot s = snapshot();
    if (!s.ok || !s.p.complete) {
        return false;
    }
    const float f = fraction_for(s.p, g_zoom.load(std::memory_order_relaxed));
    if (f < 1e-3F) {
        return false;
    }
    // The proxy where the animation has it now -- its progress from the learned curve, blended the
    // way an animation blends -- and delta, the rigid move from the hip pose to it.
    const Affine now = pose_lerp(s.p.hip, s.p.ads, curve_at(s.p, f));
    delta = now * inverse_rigid(s.p.hip);
    return finite(delta.t) && finite(delta.m.c[0]);
}

void observe(const Affine& drawnHand, const Affine* gunRoot, std::uint32_t gunBones, const Affine& controller) noexcept {
    const weapon::DrawnFrame frame = weapon::drawn_frame();
    if (!frame.valid || !enabled()) {
        g_atRest.store(false, std::memory_order_relaxed);
        g_restPrevValid = false;
        return;
    }
    // Once per placed frame: an arm palette arrives once per arm mesh, three times a frame.
    if (frame.serial == g_learner.lastSerial) {
        return;
    }
    g_learner.lastSerial = frame.serial;
    const Affine toLocal = inverse_rigid(frame.local);
    const Affine hand = toLocal * drawnHand;
    const bool gunValid = gunRoot != nullptr;
    const Affine gun = gunValid ? toLocal * *gunRoot : Affine{};
    const std::uint64_t nowMs = GetTickCount64();
    // Rest: the same per-frame stillness the learner uses (idle sway is far below it; a recoil, a
    // reload or a walk cycle is far above), held for kRestMs.
    const bool stillNow = g_restPrevValid && length(hand.t - g_restPrev.t) < 0.0015F
                          && angle_between(hand.m, g_restPrev.m) < 0.4F * kDeg;
    g_restPrev = hand;
    g_restPrevValid = true;
    if (!stillNow) {
        g_restSince = 0;
    } else if (g_restSince == 0) {
        g_restSince = nowMs;
    }
    g_atRest.store(g_restSince != 0 && nowMs - g_restSince >= kRestMs, std::memory_order_relaxed);
    if (gunValid) {
        g_gunSeenAt = nowMs;
        g_lastGunBones.store(gunBones, std::memory_order_relaxed);
    }
    // The stand-in: in auto, the gun whenever it is seen; a frame that misses it is skipped rather than
    // learned from the hand, and only a weapon unseen for two seconds falls back to the hand.
    const int wanted = g_proxy.load(std::memory_order_relaxed);
    int proxy = 0;
    bool usable = true;
    if (wanted == 1 || (wanted == 2 && nowMs - g_gunSeenAt < 2000)) {
        proxy = 1;
        usable = gunValid;
    }
    AcquireSRWLockExclusive(&g_traceLock);
    if (g_traceWanted && nowMs <= g_traceUntil && g_trace.size() < 20000) {
        TraceRow r{};
        r.ms = nowMs;
        r.fov = g_engineFov.load(std::memory_order_relaxed);
        r.zoom = g_zoom.load(std::memory_order_relaxed);
        r.fraction = engine_fraction();
        r.hand = hand;
        r.gun = gun;
        r.gunValid = gunValid;
        r.lanesIn = frame.lanesIn;
        Affine unused{};
        r.comp = compensation(unused) ? 1 : 0;
        r.seen = inverse_rigid(controller) * drawnHand;
        r.seenGun = gunValid ? inverse_rigid(controller) * *gunRoot : Affine{};
        r.body = body_yaw_error();
        g_trace.push_back(r);
    }
    ReleaseSRWLockExclusive(&g_traceLock);
    if (g_learnOn.load(std::memory_order_relaxed) && usable) {
        const Affine& pose = proxy == 1 ? gun : hand;
        learn(pose, proxy, proxy == 1 ? gunBones : 0U, frame, nowMs);
        learn_transition(pose, proxy);
    }
}

SightKnown sight_direction(V3& direction) noexcept {
    const Snapshot s = snapshot();
    if (s.ok) {
        if (!s.p.complete) {
            return SightKnown::unknown;
        }
        direction = s.p.sightD;
        return SightKnown::known;
    }
    // Not identified yet -- the moments after a swap, before the hip pose is matched. If a learned
    // profile has this palette, the weapon is most likely about to be matched: holding the seat beats
    // turning the gun towards the canonical hand for a quarter of a second and back.
    const std::uint32_t bones = g_lastGunBones.load(std::memory_order_relaxed);
    bool any = false;
    AcquireSRWLockShared(&g_lock);
    for (std::size_t i = 0; i < g_profileCount && bones != 0; ++i) {
        any = any || (g_profiles[i].complete && g_profiles[i].bones == bones);
    }
    ReleaseSRWLockShared(&g_lock);
    return any ? SightKnown::pending : SightKnown::unknown;
}

bool animation_at_rest() noexcept {
    return g_atRest.load(std::memory_order_relaxed);
}

bool sight(V3& origin, V3& direction) noexcept {
    const Snapshot s = snapshot();
    if (!s.ok || !s.p.complete) {
        return false;
    }
    origin = s.p.sightO;
    direction = s.p.sightD;
    return true;
}

float zero_distance() noexcept {
    return g_zero.load(std::memory_order_relaxed);
}

Mode mode() noexcept {
    return static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
}

bool want_ads() noexcept {
    return g_want.load(std::memory_order_relaxed);
}

void present() noexcept {
    ++g_frame;
    if (!g_loaded) {
        g_loaded = true;
        load_profiles();
    }
    if (g_frame % kPollPeriod == 0) {
        poll_commands();
    }
    const std::uint64_t nowMs = GetTickCount64();
    // A weapon swap: whatever is in hand is not trusted until its hip pose is matched again.
    const xr::InputState in = xr::input_state();
    const bool y = in.valid && (in.buttons & xr::kButtonY) != 0;
    if (y && !g_det.lastY) {
        AcquireSRWLockExclusive(&g_lock);
        g_currentOk = false;
        ReleaseSRWLockExclusive(&g_lock);
    }
    g_det.lastY = y;
    detect(nowMs);
    bool save = false;
    AcquireSRWLockExclusive(&g_lock);
    for (std::size_t i = 0; i < g_profileCount; ++i) {
        if (g_profiles[i].dirty) {
            g_profiles[i].dirty = false;
            save = true;
        }
    }
    if (save) {
        save_profiles();
    }
    ReleaseSRWLockExclusive(&g_lock);
    bool traceDone = false;
    AcquireSRWLockShared(&g_traceLock);
    traceDone = g_traceWanted && nowMs > g_traceUntil;
    ReleaseSRWLockShared(&g_traceLock);
    if (traceDone) {
        write_trace();
    }
    if (nowMs - g_lastReport > kReportMs && enabled() && xr::active()) {
        g_lastReport = nowMs;
        report_state(nowMs);
    }
}

} // namespace sunrise::client::hooks::vr::ads
