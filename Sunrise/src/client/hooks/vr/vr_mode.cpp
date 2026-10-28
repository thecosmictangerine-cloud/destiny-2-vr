/**
 * The play mode and the eye (vr_mode.h): the research trace, the mode, the vehicle, the frame records.
 */

#include "vr_mode.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../../../core/logging/log.h"
#include "../../player/player_position.h"
#include "../teleport/internal.h"
#include "../teleport/runtime.h"
#include "runtime.h"
#include "vr_debug.h"
#include "vr_dual.h"
#include "vr_gamepad.h"
#include "vr_gpu.h"
#include "vr_prehook.h"
#include "vr_state.h"
#include "vr_weapon.h"

namespace sunrise::client::hooks::vr::mode {
namespace {

namespace tp = teleport;

/** The body words the trace keeps: the motion state around the position (+448) and velocity (+560). */
constexpr std::size_t kBodyFirst = 352;
constexpr std::size_t kBodyFloats = 64;
/** Movement-state flags on the physics component (sword_skate.cpp). */
constexpr std::size_t kMovementStateOffset = 15492;
/** The body's transform: rotation columns at +368, +384, +400 and the translation (the feet) at +416. */
constexpr std::size_t kBodyRotation = 368;
constexpr std::size_t kBodyTranslation = 416;
/** The body's linear velocity (Sunrise's teleport/internal.h kBodyVelocityX). */
constexpr std::size_t kBodyVelocity = 560;

struct Row final {
    double ms{};
    std::int32_t step{};
    std::uint8_t view{};
    std::array<float, 3> camPos{};
    std::array<float, 3> camFwd{};
    std::uintptr_t component{};
    std::uint16_t handle{};
    std::uintptr_t body{};
    std::array<float, kBodyFloats> bodyWords{};
    std::uint32_t movement{};
    std::uint64_t weaponFrames{};
    std::uint64_t vmViews{};
    std::uint64_t worldViews{};
    std::uintptr_t active{};
    std::uintptr_t vcomp{};
    std::uintptr_t vbody{};
    std::array<float, kBodyFloats> vbodyWords{};
};

constexpr std::size_t kMaxRows = 4096;
std::array<Row, kMaxRows> g_rows{};
std::atomic<std::uint32_t> g_rowCount{0};
std::atomic<std::uint32_t> g_rowsWanted{0};
std::atomic_bool g_traceDone{false};

/**
 * The vehicle: mounting one takes the player's own physics body away (its body index goes to -1), and
 * the vehicle is a physics object of its own, synced like every other. It is the component whose body
 * is nearest to where the player last stood when the body went, followed from there.
 */
std::atomic_bool g_wantVehicle{false};
SRWLOCK g_vehicleLock = SRWLOCK_INIT;
std::array<float, 3> g_vehicleNear{};
std::uintptr_t g_vehicleBest = 0;
float g_vehicleBestDist = 1e9F;
std::uintptr_t g_vehicle = 0;
std::array<float, 3> g_lastFeet{};
bool g_haveFeet = false;

/**
 * The player's own component when Sunrise's is stale. Getting off a sparrow on the headset (2026-10-18,
 * EDZ) gave the Guardian a NEW physics component while Sunrise's tracker kept the old one, which still
 * passes its ownership test but has no body: from then on every chase camera (a sword, a super) found no
 * body, the player's new component was "reacquired" as the vehicle, and a sword was ridden like a
 * sparrow. So while the viewmodel is drawn -- on foot, certainly not riding -- and Sunrise's component
 * has no body, the component under the first-person camera (feet = camera - eye height) that the local
 * player owns is looked for among those synced; it stands in until it loses its body or Sunrise's has one
 * again. (The physics tick skips a body at rest: a player who has not moved since getting off is found
 * once they do.) Under g_vehicleLock.
 */
std::atomic_bool g_wantOwn{false};
std::array<float, 3> g_ownPoint{};
std::uintptr_t g_ownBest = 0;
float g_ownBestDist = 1e9F;
bool g_ownBestOwner = false;
std::uintptr_t g_ownPlayer = 0;
/** Viewmodel frames in a row with Sunrise's component bodiless: a mount's first frames never search. */
int g_ownSearchFrames = 0;
constexpr int kOwnSearchAfterFrames = 30;
constexpr float kOwnReachH = 0.6F;
constexpr float kOwnReachV = 0.4F;

/**
 * Full lock turns the steered heading this fast (rad/s). In a cockpit the room turns with the vehicle,
 * and 2.2 rad/s made the sparrow turn 90 deg/s on the mock -- vection a headset should not have to
 * carry. 1.3 (75 deg/s) still carves; `steer <deg/s>` in SVR_Mode.txt tunes it.
 */
std::atomic<float> g_steerRate{1.3F};

/** A dump asked for by the command file, taken on the camera thread. */
std::atomic<int> g_dumpWhat{0};
std::atomic<std::uint32_t> g_dumpBytes{0};

void report(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, {text, std::strlen(text)});
}

template <typename... Args> void reportf(const char* format, Args... args) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(), line.size(), format, args...);
    if (written > 0) {
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
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

double now_ms() noexcept {
    LARGE_INTEGER c{};
    LARGE_INTEGER f{};
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return f.QuadPart != 0 ? 1000.0 * static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart) : 0.0;
}

template <typename T> bool read_at(std::uintptr_t address, T& out) noexcept {
    return address != 0 && prehook::read(address, &out, sizeof out);
}

std::uintptr_t body_of(std::uintptr_t component) noexcept {
    std::uintptr_t array = 0;
    std::int32_t index = 0;
    if (!read_at(component + tp::kPhysicsComponentBodyArray, array) || !read_at(component + tp::kPhysicsComponentBodyIndex, index)
        || array == 0 || index < 0) {
        return 0;
    }
    std::uintptr_t body = 0;
    return read_at(array + tp::kBodyEntryStride * static_cast<std::size_t>(index) + tp::kBodyPointer, body) ? body : 0;
}

void dump(int what, std::uint32_t bytes) noexcept {
    const auto player = reinterpret_cast<std::uintptr_t>(player::position::component());
    AcquireSRWLockShared(&g_vehicleLock);
    const std::uintptr_t vehicle = g_vehicle;
    ReleaseSRWLockShared(&g_vehicleLock);
    const std::uintptr_t component = what >= 3 ? vehicle : player;
    const std::uintptr_t base = (what == 1 || what == 3) ? component : body_of(component);
    std::array<wchar_t, MAX_PATH> path{};
    static const wchar_t* const kNames[] = {L"", L"SVR_ModeDump_comp.txt", L"SVR_ModeDump_body.txt", L"SVR_ModeDump_vcomp.txt",
                                            L"SVR_ModeDump_vbody.txt"};
    if (base == 0 || what < 1 || what > 4 || !game_path(kNames[what], path)) {
        reportf("ev=vr.mode dump result=fail what=%d base=%p", what, reinterpret_cast<void*>(base));
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"w") != 0 || file == nullptr) {
        return;
    }
    for (std::uint32_t off = 0; off < bytes; off += 4) {
        std::uint32_t word = 0;
        if (!read_at(base + off, word)) {
            break;
        }
        float f = 0.0F;
        std::memcpy(&f, &word, sizeof f);
        std::fprintf(file, "%u %08X %.6g\n", off, word, static_cast<double>(f));
    }
    std::fclose(file);
    reportf("ev=vr.mode dump result=ok what=%d base=%p bytes=%u", what, reinterpret_cast<void*>(base), bytes);
}

void write_trace(std::uint32_t count) noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    if (!game_path(L"SVR_ModeTrace.csv", path)) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"w") != 0 || file == nullptr) {
        return;
    }
    std::fprintf(file, "ms,step,view,cx,cy,cz,fx,fy,fz,comp,handle,body,movement,wframes,vmviews,worldviews,active");
    for (std::size_t i = 0; i < kBodyFloats; ++i) {
        std::fprintf(file, ",b%zu", kBodyFirst + 4 * i);
    }
    std::fprintf(file, ",vcomp,vbody");
    for (std::size_t i = 0; i < kBodyFloats; ++i) {
        std::fprintf(file, ",v%zu", kBodyFirst + 4 * i);
    }
    std::fprintf(file, "\n");
    for (std::uint32_t r = 0; r < count; ++r) {
        const Row& row = g_rows[r];
        std::fprintf(file, "%.3f,%d,%u,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%llX,%u,%llX,%08X,%llu,%llu,%llu,%llX", row.ms, row.step,
                     row.view, row.camPos[0], row.camPos[1], row.camPos[2], row.camFwd[0], row.camFwd[1], row.camFwd[2],
                     static_cast<unsigned long long>(row.component), row.handle, static_cast<unsigned long long>(row.body),
                     row.movement, static_cast<unsigned long long>(row.weaponFrames),
                     static_cast<unsigned long long>(row.vmViews), static_cast<unsigned long long>(row.worldViews),
                     static_cast<unsigned long long>(row.active));
        for (const float f : row.bodyWords) {
            std::fprintf(file, ",%.6g", static_cast<double>(f));
        }
        std::fprintf(file, ",%llX,%llX", static_cast<unsigned long long>(row.vcomp), static_cast<unsigned long long>(row.vbody));
        for (const float f : row.vbodyWords) {
            std::fprintf(file, ",%.6g", static_cast<double>(f));
        }
        std::fprintf(file, "\n");
    }
    std::fclose(file);
    reportf("ev=vr.mode trace written rows=%u", count);
}

void poll_commands() noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    if (!game_path(L"SVR_Mode.txt", path)) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"r") != 0 || file == nullptr) {
        return;
    }
    std::array<char, 128> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        char verb[16] = {};
        char a[16] = {};
        unsigned b = 0;
        const int n = sscanf_s(line.data(), "%15s %15s %u", verb, static_cast<unsigned>(sizeof verb), a,
                               static_cast<unsigned>(sizeof a), &b);
        if (n >= 2 && std::strcmp(verb, "trace") == 0) {
            const unsigned frames = static_cast<unsigned>(std::atoi(a));
            g_rowCount.store(0, std::memory_order_relaxed);
            g_traceDone.store(false, std::memory_order_relaxed);
            g_rowsWanted.store(frames < kMaxRows ? frames : static_cast<unsigned>(kMaxRows), std::memory_order_release);
            reportf("ev=vr.mode trace start frames=%u", frames);
        } else if (n >= 2 && std::strcmp(verb, "steer") == 0) {
            const float degrees = static_cast<float>(std::atof(a));
            if (degrees > 10.0F && degrees < 400.0F) {
                g_steerRate.store(degrees * 0.0174533F, std::memory_order_relaxed);
                reportf("ev=vr.mode steer rate_deg=%.1f", degrees);
            }
        } else if (n >= 3 && std::strcmp(verb, "dump") == 0) {
            g_dumpBytes.store(b, std::memory_order_relaxed);
            const int what = std::strcmp(a, "comp") == 0 ? 1 : std::strcmp(a, "body") == 0 ? 2 : std::strcmp(a, "vcomp") == 0 ? 3 : 4;
            g_dumpWhat.store(what, std::memory_order_release);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

// ---------------------------------------------------------------------------------------------
// The mode, the eye and the frame records
// ---------------------------------------------------------------------------------------------

using math::Affine;
using math::M3;
using math::V3;

constexpr std::int32_t kWorldStep = 38;
/** Frames without a first-person viewmodel before the camera is taken for a third-person one. */
constexpr int kThirdPersonFrames = 6;
/** The engine's camera this far from where the eye would be is a chase camera, not an eye. */
constexpr float kChaseDistance = 0.6F;
/** The eye above the feet, as first person measured it (2026-10-18: 1.7086 m, Hunter). */
constexpr float kDefaultEyeHeight = 1.7086F;
/** Stairs and steps pop the body up and down; the eye rides them over this time constant. */
constexpr float kStepSeconds = 0.07F;

std::atomic<Mode> g_mode{Mode::first};
std::uint64_t g_lastWeaponFrames = 0;
int g_noViewmodelFrames = 0;
float g_eyeHeight = kDefaultEyeHeight;
float g_smoothZ = 0.0F;
bool g_smoothValid = false;
double g_lastTickMs = 0.0;

/** Shapes the renderer measures (vr_body.cpp), in the vehicle's frame. Any thread, under the lock. */
SRWLOCK g_shapeLock = SRWLOCK_INIT;
/** The rider's root in the sparrow's frame, as measured on the 2026-10-18 capture: the prior. */
const V3 kRiderOffsetPrior{-0.035F, 0.026F, 0.510F};
V3 g_riderOffset = kRiderOffsetPrior;
/**
 * The rider's eye above their own root (bone 18 applied to the bind eye, 2026-10-18 sparrow capture: the
 * rider hunched forward). The seat is the rider's root as the renderer shows it plus this, so an offset
 * between the vehicle's physics origin and its drawn root corrects itself.
 */
const V3 kRiderEye{0.294F, 0.027F, 0.359F};
bool g_barsValid = false;
V3 g_barL{};
V3 g_barR{};

/** Steering: the input, and the heading the vehicle is being steered towards. */
std::atomic<float> g_steer{0.0F};
float g_steerTarget = 0.0F;
bool g_steerValid = false;
/** The steering servo's own bookkeeping (camera thread): what it asked, and whether the camera answered. */
constexpr int kSteerStuckFrames = 20;
int g_steerLastCounts = 0;
float g_steerPrevYaw = 0.0F;
bool g_steerHavePrev = false;
int g_steerNoResponse = 0;
std::uint32_t g_steerProbe = 0;
/** How far the steered heading may lead the vehicle's own: let go and it straightens out at once. */
constexpr float kSteerLead = 0.45F;

/** The records, a ring written by the camera thread. */
constexpr std::size_t kRecords = 16;
SRWLOCK g_ringLock = SRWLOCK_INIT;
std::array<Record, kRecords> g_ring{};
std::uint64_t g_serial = 0;
Record g_pending{};
bool g_pendingValid = false;

std::atomic<std::uint64_t> g_modeChanges{0};

V3 v3(const float* p) noexcept {
    return V3{p[0], p[1], p[2]};
}

void rotate_z(float* v, float c, float s) noexcept {
    const float x = v[0];
    const float y = v[1];
    v[0] = c * x - s * y;
    v[1] = s * x + c * y;
}

/** Turns a sample about the vertical through the room origin, as a different room anchor would have. */
void turn_sample(float angle, xr::HeadPose& head, xr::HandPose& left, xr::HandPose& right) noexcept {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    rotate_z(head.forward.data(), c, s);
    rotate_z(head.up.data(), c, s);
    rotate_z(head.offset.data(), c, s);
    rotate_z(head.eyeOffsets[0].data(), c, s);
    rotate_z(head.eyeOffsets[1].data(), c, s);
    head.fold = math::wrap(head.fold + angle);
    for (xr::HandPose* h : {&left, &right}) {
        rotate_z(h->forward.data(), c, s);
        rotate_z(h->up.data(), c, s);
        rotate_z(h->right.data(), c, s);
        rotate_z(h->offset.data(), c, s);
        rotate_z(h->palm.data(), c, s);
        rotate_z(h->gripForward.data(), c, s);
        rotate_z(h->gripUp.data(), c, s);
    }
}

bool read_transform(std::uintptr_t body, Affine& out) noexcept {
    std::array<float, 16> words{};
    if (body == 0 || !prehook::read(body + kBodyRotation, words.data(), sizeof words)) {
        return false;
    }
    out.m = math::orthonormalize(M3{{V3{words[0], words[1], words[2]}, V3{words[4], words[5], words[6]},
                                     V3{words[8], words[9], words[10]}}});
    out.t = V3{words[12], words[13], words[14]};
    return std::isfinite(out.t.x) && std::isfinite(out.t.y) && std::isfinite(out.t.z);
}

/**
 * Where the engine's chase camera sits relative to the vehicle it follows (sparrow frame, 2026-10-18
 * capture: 4.07 m behind, 1.76 m up). A summon leaves the previous sparrow standing a moment beside the
 * new one, and the nearest-at-mount search took it once (the eye stayed on the old one while the player
 * rode the new): after a second of riding the tracked vehicle must sit where the camera says, or the
 * component nearest that point replaces it.
 */
const V3 kChaseInVehicle{-4.07F, 0.0F, 1.76F};
constexpr float kChaseMismatch = 3.0F;
std::atomic_bool g_wantNear{false};
std::array<float, 3> g_nearPoint{};
std::uintptr_t g_nearBest = 0;
float g_nearBestDist = 1e9F;
int g_rideFrames = 0;

/** How far from where it should be a vehicle is looked for (under g_vehicleLock). */
float g_searchRadius = 1.0F;

/** Frames a vehicle whose body went is held (its last transform) while its replacement is looked for. */
constexpr int kVehicleGraceFrames = 36;
int g_vehicleLost = 0;
Affine g_lastVehicleT{};
bool g_lastVehicleValid = false;

/** A different vehicle: what was learned on the last one (its rider, its handlebars) starts over. */
void vehicle_changed() noexcept {
    AcquireSRWLockExclusive(&g_shapeLock);
    g_riderOffset = kRiderOffsetPrior;
    g_barsValid = false;
    ReleaseSRWLockExclusive(&g_shapeLock);
}

/** The vehicle's component, kept from the moment it is found until the player's body comes back. */
std::uintptr_t track_vehicle(std::uintptr_t playerBody) noexcept {
    if (playerBody != 0) {
        std::array<float, 3> feet{};
        if (prehook::read(playerBody + kBodyTranslation, feet.data(), sizeof feet)) {
            g_lastFeet = feet;
            g_haveFeet = true;
        }
        g_wantVehicle.store(false, std::memory_order_relaxed);
        AcquireSRWLockExclusive(&g_vehicleLock);
        g_vehicle = 0;
        g_vehicleBest = 0;
        g_vehicleBestDist = 1e9F;
        ReleaseSRWLockExclusive(&g_vehicleLock);
        return 0;
    }
    if (!g_haveFeet || state::step() != kWorldStep) {
        // Out of the world nothing is searched: the physics hook goes back to costing nothing.
        g_wantVehicle.store(false, std::memory_order_relaxed);
        g_wantNear.store(false, std::memory_order_relaxed);
        return 0;
    }
    AcquireSRWLockExclusive(&g_vehicleLock);
    const std::uintptr_t body = body_of(g_vehicle);
    if (g_vehicle != 0 && body != 0) {
        // Followed: where it is now is where to look for it should its body ever go.
        std::array<float, 3> at{};
        if (prehook::read(body + kBodyTranslation, at.data(), sizeof at)) {
            g_vehicleNear = at;
        }
        g_vehicleLost = 0;
    } else if (g_vehicle != 0) {
        // Its body went (measured: riding into a building). The component nearest to where it last was,
        // synced since, is its replacement; held for a grace period, then let go.
        if (g_vehicleBest != 0 && g_vehicleBest != g_vehicle) {
            vehicle_changed();
            reportf("ev=vr.mode vehicle reacquired comp=%p was=%p dist=%.2f lost_frames=%d", reinterpret_cast<void*>(g_vehicleBest),
                    reinterpret_cast<void*>(g_vehicle), g_vehicleBestDist, g_vehicleLost);
            g_vehicle = g_vehicleBest;
            g_vehicleLost = 0;
        } else if (++g_vehicleLost > kVehicleGraceFrames) {
            reportf("ev=vr.mode vehicle lost comp=%p", reinterpret_cast<void*>(g_vehicle));
            g_vehicle = 0;
        }
    } else {
        // Searching from the mount: the component nearest to where the player last stood.
        g_vehicleNear = g_lastFeet;
        if (g_vehicleBest != 0) {
            g_vehicle = g_vehicleBest;
            g_vehicleLost = 0;
            vehicle_changed();
            reportf("ev=vr.mode vehicle found comp=%p dist=%.2f", reinterpret_cast<void*>(g_vehicle), g_vehicleBestDist);
        }
    }
    g_vehicleBest = 0;
    g_vehicleBestDist = 1e9F;
    if (g_vehicle != 0 && g_vehicleLost == 0 && g_nearBest != 0 && g_nearBest != g_vehicle) {
        vehicle_changed();
            reportf("ev=vr.mode vehicle corrected comp=%p was=%p dist_to_expected=%.2f", reinterpret_cast<void*>(g_nearBest),
                reinterpret_cast<void*>(g_vehicle), g_nearBestDist);
        g_vehicle = g_nearBest;
        g_rideFrames = 0;
    }
    g_nearBest = 0;
    g_nearBestDist = 1e9F;
    const std::uintptr_t vehicle = g_vehicle;
    const bool searching = vehicle == 0 || g_vehicleLost > 0;
    // At the mount the sparrow appears under the player (0.00-0.50 m measured): anything further is not
    // what they mounted. Found again after its body went, it has moved with the ride: 4 m.
    g_searchRadius = vehicle == 0 ? 1.0F : 4.0F;
    ReleaseSRWLockExclusive(&g_vehicleLock);
    g_wantVehicle.store(searching, std::memory_order_relaxed);
    return vehicle;
}

/**
 * The player's body this frame: Sunrise's component's, or the stand-in's (g_ownPlayer). `viewmodel`: the
 * first-person weapon was drawn this frame. Camera thread.
 */
std::uintptr_t player_body(std::uintptr_t sunrise, bool viewmodel, bool inWorld, const V3& engineCam) noexcept {
    // `stale player` in SVR_Debug.txt: Sunrise's component taken as bodiless, as the headset met it after a
    // ride (the mock's gets its body back), so the stand-in is exercised.
    const std::uintptr_t body = debug::has("stale player") ? 0 : body_of(sunrise);
    g_ownSearchFrames = (body == 0 && viewmodel && inWorld) ? std::min(g_ownSearchFrames + 1, 100000) : 0;
    const bool search = g_ownSearchFrames >= kOwnSearchAfterFrames;
    AcquireSRWLockExclusive(&g_vehicleLock);
    if (body != 0) {
        if (g_ownPlayer != 0) {
            reportf("ev=vr.mode own_player released comp=%p reason=sunrise_body", reinterpret_cast<void*>(g_ownPlayer));
        }
        g_ownPlayer = 0;
    } else if (search && g_ownBest != 0 && g_ownBest != g_ownPlayer) {
        reportf("ev=vr.mode own_player found comp=%p was=%p sunrise=%p dist=%.2f owner=%d", reinterpret_cast<void*>(g_ownBest),
                reinterpret_cast<void*>(g_ownPlayer), reinterpret_cast<void*>(sunrise), g_ownBestDist, g_ownBestOwner ? 1 : 0);
        g_ownPlayer = g_ownBest;
        // On foot for sure: whatever was taken for a vehicle is not one.
        g_vehicle = 0;
        g_vehicleLost = 0;
    }
    g_ownBest = 0;
    g_ownBestDist = 1e9F;
    g_ownBestOwner = false;
    if (search) {
        g_ownPoint = {engineCam.x, engineCam.y, engineCam.z - g_eyeHeight};
    }
    std::uintptr_t own = body != 0 ? 0 : g_ownPlayer;
    std::uintptr_t ownBody = body_of(own);
    if (own != 0 && ownBody == 0) {
        // Its body went too (a mount): the vehicle search takes over from where the feet were.
        reportf("ev=vr.mode own_player released comp=%p reason=no_body", reinterpret_cast<void*>(own));
        g_ownPlayer = 0;
    }
    ReleaseSRWLockExclusive(&g_vehicleLock);
    g_wantOwn.store(search, std::memory_order_relaxed);
    return body != 0 ? body : ownBody;
}

void push_record(const Record& r) noexcept {
    AcquireSRWLockExclusive(&g_ringLock);
    g_ring[r.serial % kRecords] = r;
    ReleaseSRWLockExclusive(&g_ringLock);
}

template <typename Key> bool nearest_record(Key key, const V3& point, float tolerance, Record& out) noexcept {
    AcquireSRWLockShared(&g_ringLock);
    float best = tolerance;
    const Record* found = nullptr;
    for (const Record& r : g_ring) {
        if (r.serial == 0 || r.mode == Mode::first || r.mode == Mode::passive) {
            continue;
        }
        const float d = math::length(key(r) - point);
        // Equal distances (a body at rest): the newest record, which is what the renderer is closest to.
        if (d < best || (found != nullptr && d == best && r.serial > found->serial)) {
            best = d;
            found = &r;
        }
    }
    if (found != nullptr) {
        out = *found;
    }
    ReleaseSRWLockShared(&g_ringLock);
    return found != nullptr;
}

/** The steered heading, and the mouse that turns the engine's camera (which the vehicle follows) onto it. */
void steer(float vehicleYaw, float engineYaw, double dt) noexcept {
    if (!g_steerValid) {
        g_steerTarget = vehicleYaw;
        g_steerValid = true;
    }
    const float input = std::clamp(g_steer.load(std::memory_order_relaxed), -1.0F, 1.0F);
    // Right is negative yaw (the game's +Y is left).
    g_steerTarget = math::wrap(g_steerTarget - input * g_steerRate.load(std::memory_order_relaxed) * static_cast<float>(dt));
    const float lead = math::wrap(g_steerTarget - vehicleYaw);
    if (std::fabs(lead) > kSteerLead) {
        g_steerTarget = math::wrap(vehicleYaw + std::copysign(kSteerLead, lead));
    }
    if (std::fabs(input) < 0.02F) {
        // Hands off: the target relaxes onto the vehicle's heading, so it runs straight from there.
        g_steerTarget = math::wrap(g_steerTarget - math::wrap(g_steerTarget - vehicleYaw) * std::min(1.0F, static_cast<float>(dt) * 6.0F));
    }
    // The body servo's lesson (docs/BODY-SERVO-HANG.md): credit what was asked, and stop pushing at a
    // camera that does not answer -- a mount or dismount animation, the grace hold of a vehicle whose body
    // went -- rather than inject thousands of counts a second into it. Probe once in a while; the first
    // real answer resumes.
    const float cpr = counts_per_radian();
    if (g_steerLastCounts != 0 && g_steerHavePrev) {
        const float achieved = math::wrap(engineYaw - g_steerPrevYaw);
        const float expected = -static_cast<float>(g_steerLastCounts) / cpr;
        const bool answered = std::fabs(achieved) > 0.25F * std::fabs(expected) && (achieved > 0.0F) == (expected > 0.0F);
        if (answered) {
            g_steerNoResponse = 0;
        } else if (g_steerNoResponse < kSteerStuckFrames) {
            if (++g_steerNoResponse == kSteerStuckFrames) {
                report("ev=vr.mode steer=stuck reason=no_response action=stop_injecting");
            }
        }
    }
    g_steerPrevYaw = engineYaw;
    g_steerHavePrev = true;
    g_steerLastCounts = 0;
    const float error = math::wrap(g_steerTarget - engineYaw);
    if (std::fabs(error) < 0.004F) {
        return;
    }
    if (g_steerNoResponse >= kSteerStuckFrames && (++g_steerProbe % static_cast<std::uint32_t>(kSteerStuckFrames)) != 0) {
        return;
    }
    const int counts = static_cast<int>(std::lround(std::clamp(-error * cpr * 0.5F, -220.0F, 220.0F)));
    g_steerLastCounts = gamepad::turn_body(counts) ? counts : 0;
}

} // namespace

Eye camera_tick(const float* enginePosition, const float* engineForward, xr::HeadPose& head, xr::HandPose& left,
                xr::HandPose& right) noexcept {
    const int what = g_dumpWhat.exchange(0, std::memory_order_acq_rel);
    if (what != 0) {
        dump(what, g_dumpBytes.load(std::memory_order_relaxed));
    }
    const double nowMs = now_ms();
    const double dt = g_lastTickMs > 0.0 ? std::clamp((nowMs - g_lastTickMs) / 1000.0, 0.001, 0.1) : 0.016;
    g_lastTickMs = nowMs;

    const std::uint64_t wf = weapon::weapon_frames();
    if (wf != g_lastWeaponFrames) {
        g_lastWeaponFrames = wf;
        g_noViewmodelFrames = 0;
    } else if (g_noViewmodelFrames < 1000) {
        ++g_noViewmodelFrames;
    }

    const V3 engineCam = v3(enginePosition);
    const bool inWorld = state::step() == kWorldStep && state::view() == state::View::immersive;
    const auto playerComponent = reinterpret_cast<std::uintptr_t>(player::position::component());
    const std::uintptr_t playerBody = player_body(playerComponent, g_noViewmodelFrames < 3, inWorld, engineCam);
    const std::uintptr_t vehicle = track_vehicle(playerBody);
    const std::uintptr_t vehicleBody = body_of(vehicle);

    Affine feet{};
    const bool haveFeet = read_transform(playerBody, feet);
    Affine vehicleT{};
    bool haveVehicle = read_transform(vehicleBody, vehicleT);
    if (haveVehicle) {
        g_lastVehicleT = vehicleT;
        g_lastVehicleValid = true;
    } else if (vehicle != 0 && g_lastVehicleValid) {
        // Its body is between two components: the cockpit holds still rather than dropping the ride.
        vehicleT = g_lastVehicleT;
        haveVehicle = true;
    } else if (vehicle == 0) {
        g_lastVehicleValid = false;
    }

    Mode mode = Mode::first;
    // `thirdperson off` in SVR_Debug.txt: the engine's chase cameras are left as they were (bisection).
    if (inWorld && g_noViewmodelFrames >= kThirdPersonFrames && !debug::has("thirdperson off")) {
        if (haveFeet) {
            const V3 eye = feet.t + V3{0.0F, 0.0F, g_eyeHeight};
            // A camera at the eye without a viewmodel is still first person (an interaction, a
            // finisher's first frames): only a camera away from the head is a chase camera.
            mode = math::length(engineCam - eye) > kChaseDistance ? Mode::onFoot : Mode::first;
        } else if (haveVehicle && math::length(engineCam - vehicleT.t) > 1.5F && math::length(engineCam - vehicleT.t) < 8.0F) {
            // ...and the engine's camera chasing something at a vehicle's distance (4.4 m on the sparrow):
            // a body gone for another reason (death, a cut scene) is not a ride.
            mode = Mode::vehicle;
        } else {
            mode = Mode::passive;
        }
    }
    // The eye height, learned while the engine's own camera is the eye: standing, not crouched (a
    // crouch is half a metre lower and is refused by the window).
    // Only at rest: the first-person camera smooths the body's steps and trails it on slopes, so a
    // moving body's samples walked the height down 11 cm in one session (2026-10-18).
    std::array<float, 3> velocity{};
    const bool still = playerBody != 0 && prehook::read(playerBody + kBodyVelocity, velocity.data(), sizeof velocity)
                       && velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2] < 0.05F * 0.05F;
    if (mode == Mode::first && inWorld && haveFeet && g_noViewmodelFrames == 0 && still) {
        const float h = engineCam.z - feet.t.z;
        if (h > 1.2F && h < 2.1F && std::fabs(h - g_eyeHeight) < 0.08F) {
            g_eyeHeight += (h - g_eyeHeight) * 0.01F;
        }
    }
    const Mode was = g_mode.exchange(mode, std::memory_order_relaxed);
    if (was != mode) {
        g_rideFrames = 0;
        g_wantNear.store(false, std::memory_order_relaxed);
        g_modeChanges.fetch_add(1, std::memory_order_relaxed);
        reportf("ev=vr.mode mode=%s was=%s eye_height=%.3f vehicle=%p", name(mode), name(was), g_eyeHeight,
                reinterpret_cast<void*>(vehicle));
        g_smoothValid = false;
        g_steerValid = false;
        g_steerHavePrev = false;
        g_steerLastCounts = 0;
        g_steerNoResponse = 0;
        g_steer.store(0.0F, std::memory_order_relaxed);
    }

    Eye eye{};
    eye.mode = mode;
    Record rec{};
    rec.mode = mode;
    if (mode == Mode::onFoot) {
        // The body's feet, and the eye height above them; the vertical rides steps smoothly.
        V3 base = feet.t + V3{0.0F, 0.0F, g_eyeHeight};
        if (!g_smoothValid || std::fabs(base.z - g_smoothZ) > 0.6F) {
            g_smoothZ = base.z;
            g_smoothValid = true;
        } else {
            g_smoothZ += (base.z - g_smoothZ) * (1.0F - std::exp(-static_cast<float>(dt) / kStepSeconds));
        }
        base.z = g_smoothZ;
        eye.replace = true;
        eye.base = base;
        rec.rider = feet.t;
    } else if (mode == Mode::vehicle) {
        AcquireSRWLockShared(&g_shapeLock);
        const V3 riderOffset = g_riderOffset;
        const V3 seat = riderOffset + kRiderEye;
        ReleaseSRWLockShared(&g_shapeLock);
        const V3 x = vehicleT.m.c[0];
        const float yaw = std::atan2(x.y, x.x);
        eye.replace = true;
        eye.base = vehicleT * seat;
        eye.cockpit = true;
        eye.anchor = yaw;
        // The sample was folded with the anchor of the present that took it; turned to this frame's
        // heading, the eye and the hands ride the vehicle without a frame of lag.
        const float delta = math::wrap(math::wrap(yaw - xr::origin_yaw()) - head.fold);
        if (head.valid && std::fabs(delta) > 1e-6F) {
            turn_sample(delta, head, left, right);
        }
        rec.rider = vehicleT * riderOffset;
        rec.vehicle = vehicleT;
        rec.vehicleYaw = yaw;
        {
            const float ey = std::atan2(engineForward[1], engineForward[0]);
            const float c = std::cos(ey);
            const float sn = std::sin(ey);
            const V3 off{c * kChaseInVehicle.x - sn * kChaseInVehicle.y, sn * kChaseInVehicle.x + c * kChaseInVehicle.y,
                         kChaseInVehicle.z};
            const V3 expected = engineCam - off;
            const bool settled = ++g_rideFrames > 72;
            const bool wrong = settled && math::length(V3{expected.x - vehicleT.t.x, expected.y - vehicleT.t.y, 0.0F}) > kChaseMismatch;
            AcquireSRWLockExclusive(&g_vehicleLock);
            g_nearPoint = {expected.x, expected.y, expected.z};
            ReleaseSRWLockExclusive(&g_vehicleLock);
            g_wantNear.store(wrong, std::memory_order_relaxed);
        }
        const float engineYaw = std::atan2(engineForward[1], engineForward[0]);
        steer(yaw, engineYaw, dt);
        static std::uint32_t s_rideFrames = 0;
        static V3 s_lastSeat{};
        const float speed = math::length(eye.base - s_lastSeat) / static_cast<float>(dt);
        s_lastSeat = eye.base;
        if ((++s_rideFrames % 30) == 0) {
            reportf("ev=vr.mode ride yaw=%.4f target=%.4f engine_yaw=%.4f steer=%.2f speed=%.2f pitch=%.3f seat=%.3f,%.3f,%.3f",
                    yaw, g_steerTarget, engineYaw, g_steer.load(std::memory_order_relaxed), speed,
                    std::asin(std::clamp(x.z, -1.0F, 1.0F)), eye.base.x, eye.base.y, eye.base.z);
        }
    }
    g_pending = rec;
    g_pendingValid = mode == Mode::onFoot || mode == Mode::vehicle;

    // The research trace.
    const std::uint32_t wanted = g_rowsWanted.load(std::memory_order_acquire);
    const std::uint32_t at = g_rowCount.load(std::memory_order_relaxed);
    if (wanted != 0 && at < wanted) {
        Row& row = g_rows[at];
        row = Row{};
        row.ms = nowMs;
        row.step = state::step();
        row.view = state::view() == state::View::immersive ? 1 : 0;
        for (std::size_t i = 0; i < 3; ++i) {
            row.camPos[i] = enginePosition[i];
            row.camFwd[i] = engineForward[i];
        }
        row.component = playerComponent;
        (void)read_at(row.component + tp::kPhysicsComponentObjectHandle, row.handle);
        (void)read_at(row.component + kMovementStateOffset, row.movement);
        row.body = playerBody;
        if (row.body != 0) {
            (void)prehook::read(row.body + kBodyFirst, row.bodyWords.data(), sizeof row.bodyWords);
        }
        row.weaponFrames = wf;
        row.vmViews = gpu::viewmodel_views();
        row.worldViews = gpu::world_views();
        row.active = static_cast<std::uintptr_t>(mode);
        row.vcomp = vehicle;
        row.vbody = vehicleBody;
        if (row.vbody != 0) {
            (void)prehook::read(row.vbody + kBodyFirst, row.vbodyWords.data(), sizeof row.vbodyWords);
        }
        g_rowCount.store(at + 1, std::memory_order_release);
        if (at + 1 == wanted) {
            g_traceDone.store(true, std::memory_order_release);
        }
    }
    return eye;
}

void commit(const float* cameraPosition, const xr::HeadPose& head, const xr::HandPose& left,
            const xr::HandPose& right) noexcept {
    if (!g_pendingValid) {
        return;
    }
    g_pendingValid = false;
    g_pending.serial = ++g_serial;
    g_pending.camera = v3(cameraPosition);
    g_pending.head = head;
    g_pending.left = left;
    g_pending.right = right;
    push_record(g_pending);
}

void present_tick() noexcept {
    static std::uint32_t s_presents = 0;
    if ((++s_presents % 30) == 0) {
        poll_commands();
    }
    if (g_traceDone.exchange(false, std::memory_order_acq_rel)) {
        const std::uint32_t count = g_rowCount.load(std::memory_order_acquire);
        g_rowsWanted.store(0, std::memory_order_release);
        write_trace(count);
    }
    if ((s_presents % 600) == 0) {
        AcquireSRWLockShared(&g_shapeLock);
        const V3 r = g_riderOffset;
        const bool bars = g_barsValid;
        const V3 bl = g_barL;
        const V3 br = g_barR;
        ReleaseSRWLockShared(&g_shapeLock);
        reportf("ev=vr.mode state mode=%s eye_height=%.3f changes=%llu rider=%.3f,%.3f,%.3f bars=%d l=%.3f,%.3f,%.3f "
                "r=%.3f,%.3f,%.3f",
                name(g_mode.load(std::memory_order_relaxed)), g_eyeHeight,
                static_cast<unsigned long long>(g_modeChanges.load(std::memory_order_relaxed)), r.x, r.y, r.z, bars ? 1 : 0,
                bl.x, bl.y, bl.z, br.x, br.y, br.z);
    }
}

Mode current() noexcept {
    return g_mode.load(std::memory_order_relaxed);
}

const char* name(Mode mode) noexcept {
    switch (mode) {
    case Mode::first: return "first";
    case Mode::onFoot: return "on_foot";
    case Mode::vehicle: return "vehicle";
    case Mode::passive: return "passive";
    }
    return "?";
}

namespace {
SRWLOCK g_renderEyeLock = SRWLOCK_INIT;
std::array<float, 3> g_renderEye{};
bool g_renderEyeValid = false;
/** When it was set: the render side may run a frame or two behind, never 100 ms. */
ULONGLONG g_renderEyeTick = 0;
constexpr ULONGLONG kRenderEyeMaxAgeMs = 100;
} // namespace

void set_render_eye(bool valid, const float* position) noexcept {
    AcquireSRWLockExclusive(&g_renderEyeLock);
    g_renderEyeValid = valid && position != nullptr;
    if (g_renderEyeValid) {
        g_renderEye = {position[0], position[1], position[2]};
        g_renderEyeTick = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&g_renderEyeLock);
}

bool render_eye(float* position) noexcept {
    AcquireSRWLockShared(&g_renderEyeLock);
    const bool valid = g_renderEyeValid && GetTickCount64() - g_renderEyeTick < kRenderEyeMaxAgeMs;
    if (valid && position != nullptr) {
        position[0] = g_renderEye[0];
        position[1] = g_renderEye[1];
        position[2] = g_renderEye[2];
    }
    ReleaseSRWLockShared(&g_renderEyeLock);
    return valid;
}

bool latest_record(Record& out) noexcept {
    AcquireSRWLockShared(&g_ringLock);
    const Record* best = nullptr;
    for (const Record& r : g_ring) {
        if (r.serial != 0 && (best == nullptr || r.serial > best->serial)) {
            best = &r;
        }
    }
    if (best != nullptr) {
        out = *best;
    }
    ReleaseSRWLockShared(&g_ringLock);
    return best != nullptr;
}

bool record_by_serial(std::uint64_t serial, Record& out) noexcept {
    AcquireSRWLockShared(&g_ringLock);
    const Record& r = g_ring[serial % kRecords];
    const bool found = serial != 0 && r.serial == serial;
    if (found) {
        out = r;
    }
    ReleaseSRWLockShared(&g_ringLock);
    return found;
}

bool record_by_rider(const V3& root, float tolerance, Record& out) noexcept {
    return nearest_record([](const Record& r) { return r.rider; }, root, tolerance, out);
}

bool record_by_vehicle(const V3& origin, float tolerance, Record& out) noexcept {
    return nearest_record([](const Record& r) { return r.mode == Mode::vehicle ? r.vehicle.t : V3{1e9F, 1e9F, 1e9F}; },
                          origin, tolerance, out);
}

void note_rider_offset(const V3& inVehicle) noexcept {
    // A rider half a metre from where riders sit is not riding this vehicle (a mount animation, the
    // wrong vehicle): never learned, or the matching that feeds this would lock onto it.
    if (math::length(inVehicle - kRiderOffsetPrior) > 0.5F) {
        return;
    }
    AcquireSRWLockExclusive(&g_shapeLock);
    g_riderOffset = g_riderOffset + (inVehicle - g_riderOffset) * 0.1F;
    ReleaseSRWLockExclusive(&g_shapeLock);
}

void note_handlebars(const V3& leftInVehicle, const V3& rightInVehicle) noexcept {
    AcquireSRWLockExclusive(&g_shapeLock);
    if (!g_barsValid) {
        g_barL = leftInVehicle;
        g_barR = rightInVehicle;
        g_barsValid = true;
    } else {
        g_barL = g_barL + (leftInVehicle - g_barL) * 0.05F;
        g_barR = g_barR + (rightInVehicle - g_barR) * 0.05F;
    }
    ReleaseSRWLockExclusive(&g_shapeLock);
}

bool handlebars(V3& leftInVehicle, V3& rightInVehicle) noexcept {
    AcquireSRWLockShared(&g_shapeLock);
    leftInVehicle = g_barL;
    rightInVehicle = g_barR;
    const bool valid = g_barsValid;
    ReleaseSRWLockShared(&g_shapeLock);
    return valid;
}

void set_steering(float steer) noexcept {
    g_steer.store(steer, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::vr::mode

namespace sunrise::client::hooks::vr {

void observe_physics(void* component) noexcept {
    using namespace mode;
    const bool wantVehicle = g_wantVehicle.load(std::memory_order_relaxed);
    const bool wantNear = g_wantNear.load(std::memory_order_relaxed);
    const bool wantOwn = g_wantOwn.load(std::memory_order_relaxed);
    if ((!wantVehicle && !wantNear && !wantOwn) || component == nullptr) {
        return;
    }
    // Sunrise's own component is the player's: never a vehicle (it may be the stand-in's find, with
    // `stale player`; a really stale one has no body and is skipped below anyway).
    const bool sunrisePlayer = component == player::position::component();
    if (sunrisePlayer && !wantOwn) {
        return;
    }
    const std::uintptr_t body = body_of(reinterpret_cast<std::uintptr_t>(component));
    std::array<float, 3> at{};
    if (body == 0 || !prehook::read(body + kBodyTranslation, at.data(), sizeof at)) {
        return;
    }
    // Asked outside the lock: it calls into the engine.
    const bool owner = wantOwn && teleport::owns_local_player(component);
    const auto address = reinterpret_cast<std::uintptr_t>(component);
    AcquireSRWLockExclusive(&g_vehicleLock);
    if (wantOwn) {
        const float dx = at[0] - g_ownPoint[0];
        const float dy = at[1] - g_ownPoint[1];
        const float h = std::sqrt(dx * dx + dy * dy);
        const float v = std::fabs(at[2] - g_ownPoint[2]);
        // Only the local player's own: a sparrow being summoned appears at the feet while the viewmodel is
        // still drawn, and a nearest-only rule took it for the player (mock, 2026-10-18). Among owners, the
        // nearest.
        if (owner && h < kOwnReachH && v < kOwnReachV && h + v < g_ownBestDist) {
            g_ownBestDist = h + v;
            g_ownBest = address;
            g_ownBestOwner = owner;
        }
    }
    // The player's own stand-in is never a vehicle.
    if (address == g_ownPlayer || sunrisePlayer) {
        ReleaseSRWLockExclusive(&g_vehicleLock);
        return;
    }
    if (wantVehicle) {
        const float dx = at[0] - g_vehicleNear[0];
        const float dy = at[1] - g_vehicleNear[1];
        const float dz = at[2] - g_vehicleNear[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < g_searchRadius && d < g_vehicleBestDist) {
            g_vehicleBestDist = d;
            g_vehicleBest = reinterpret_cast<std::uintptr_t>(component);
        }
    }
    if (wantNear) {
        const float dx = at[0] - g_nearPoint[0];
        const float dy = at[1] - g_nearPoint[1];
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < 2.0F && d < g_nearBestDist) {
            g_nearBestDist = d;
            g_nearBest = reinterpret_cast<std::uintptr_t>(component);
        }
    }
    ReleaseSRWLockExclusive(&g_vehicleLock);
}

} // namespace sunrise::client::hooks::vr
