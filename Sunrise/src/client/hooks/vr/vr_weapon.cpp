/**
 * F3 probe: who gets which camera pose.
 *
 * Found with one in-process access watch (RESEARCH.md, "VMProtect"): two code paths read the pose
 * out of the player camera block after Sunrise's hook has written it.
 *
 *   destiny2.exe+0x12D22C0  get_camera_pose(Vec3* pos, Vec3* fwd, Vec3* up)
 *       Copies +0x594/+0x5BC/+0x5C8 of the local player's block into the caller's vectors. Called
 *       four times a frame from six threads: the render side.
 *   destiny2.exe+0x12D50F0  camera_pose_ptr(int playerIndex) -> float*
 *       Returns block(playerIndex)+0x594; the caller reads fwd at +0x28 and up at +0x34 directly
 *       and builds a basis from them, once a frame on the camera thread.
 *
 * Both are detoured. Each call's return address is counted, and rules keyed on that address decide
 * whether the caller receives the pose as written (head), or the engine's own (body). The pointer
 * path hands out a private copy of the pose region with the orientation swapped.
 *
 * The RVAs are pinned like every other offset in this module: the target build is a fixed depot
 * manifest, and VMProtect decrypts the same image every launch.
 */

#include "vr_weapon.h"

#include <Windows.h>

#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "runtime.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::weapon {
namespace {

constexpr std::uintptr_t kGetterRva = 0x12D22C0;
constexpr std::uintptr_t kPosePtrRva = 0x12D50F0;
/**
 * The weapon's own transform builder, found by disassembling backwards from the getter caller
 * `D5D832` in the live process (VMProtect encrypts the image on disk, so this is the only way).
 *
 * It takes ONE argument, an output buffer in rcx, calls the pose getter, builds
 * `right = fwd x up`, assembles the 4x4 `[fwd, right, up, (0,0,0,1)]`, converts it through
 * `+0x465100`, and copies 32 bytes of the result into its caller's buffer. The position the getter
 * hands it, at `rbp-0x59`, is never read -- which is exactly why moving the pose's position moved
 * nothing on screen.
 *
 * Those 32 bytes are therefore the real lever: whatever the caller places the weapon with, it
 * comes out of here.
 */
constexpr std::uintptr_t kXformRva = 0xD5D7F0;
constexpr std::array<std::uint8_t, 18> kGetterPrologue{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
                                                       0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF8};
constexpr std::array<std::uint8_t, 9> kPosePtrPrologue{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xD9};
constexpr std::array<std::uint8_t, 20> kXformPrologue{0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70,
                                                      0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x48, 0x8D, 0x68, 0xA1};
/** Floats in the transform builder's output buffer. */
constexpr std::size_t kXformFloats = 8;

constexpr std::uint32_t kPollPeriod = 30;
constexpr std::uint32_t kReportPeriod = 180;
constexpr wchar_t kCommandName[] = L"SVR_Weapon.txt";
constexpr std::size_t kMaxCallers = 24;
constexpr std::size_t kMaxRuleRvas = 8;
/**
 * Rules held per path at once. More than one is essential, not a convenience: the decisive
 * configuration for the weapon's position hands the head to the render view's caller and the hand
 * to the weapon's caller in the same frame, which a single rule cannot express.
 */
constexpr std::size_t kMaxRules = 4;
/** Bytes of the pose region handed out through the pointer path; callers index it by small offsets. */
constexpr std::size_t kFakeRegionBytes = 0x100;
constexpr std::size_t kOffsetForward = 0x28;
constexpr std::size_t kOffsetUp = 0x34;

using Getter = void(__fastcall*)(Vector*, Vector*, Vector*);
using PosePtr = float*(__fastcall*)(int);
using Xform = void*(__fastcall*)(void*);

enum class Source : std::uint8_t { none, body, head, hand };

struct Rule final {
    bool all{false};
    std::array<std::uintptr_t, kMaxRuleRvas> rvas{};
    std::size_t count{0};
    Source source{Source::none};
};

/** The rules of one path, published as a whole so a reader never sees a half-written set. */
struct RuleSet final {
    std::array<Rule, kMaxRules> rules{};
    std::size_t count{0};
};

struct Caller final {
    std::atomic<std::uintptr_t> rva{0};
    std::atomic<std::uint64_t> count{0};
};

struct Path final {
    std::array<Caller, kMaxCallers> callers{};
    std::atomic<std::uint64_t> overflow{0};
    /** Rule sets are published whole from the camera thread; readers copy the pointer first. */
    std::atomic<const RuleSet*> rules{nullptr};
    /** Double buffered, so the set being replaced is never the one a detour is reading. */
    std::array<RuleSet, 2> sets{};
    std::size_t nextSet{0};
};

Path g_getter{};
Path g_ptr{};
Path g_xform{};
std::array<hooking::detour::Handle, 3> g_handles{};

/** Log the transform builder's output floats, to work out what they mean. */
std::atomic_bool g_xformDump{false};
/**
 * Add the hand's travel relative to the head to the buffer's position lanes.
 *
 * A delta rather than an absolute: `hand.offset - head.offset` is the vector by which the weapon
 * has to move away from wherever the engine just put it, and adding it works without knowing the
 * absolute frame or the sign convention of whatever those lanes are.
 */
std::atomic_bool g_xformDelta{false};
/**
 * Which of the eight floats the delta is added to. Measured, not guessed: forcing each float in
 * turn and matching the weapon's template between captures gave 166 px of horizontal travel for
 * 0.08 on lane 4, 172 px of vertical travel on lane 6, and a depth change on lane 5. Projecting
 * those onto the camera's axes identifies them as the game's own world axes -- X forward, Y left,
 * Z up -- which is the basis the hand and head offsets are already in, so no rotation is needed.
 * 0.08 metres subtending 13.5 degrees also puts the weapon 0.33 m from the eye, which confirms
 * that a game unit is a metre.
 */
std::array<std::atomic<int>, 3> g_xformLanes{{{4}, {5}, {6}}};
/**
 * The weapon's pivot correction, in the CONTROLLER's own basis: (forward, right, up), metres.
 *
 * This is the one constant the placement needs, and cancelling it is what puts the centre of
 * rotation on the gun instead of on the eyeball.
 *
 * The engine draws the weapon at `camera + R(q) * d + lanes`, where `q` is the quaternion it built
 * from the (forward, up) pair this module hands to the weapon's getter caller and `d` is its own
 * fixed viewmodel offset, measured at about 0.33 m. Nothing cancelled `R(q) * d`, so handing the
 * engine a different orientation swung the weapon along a 0.33 m arc centred on the eye -- a 90
 * degree wrist roll moved it about 0.47 m. That is the "the centre of rotation is neither in the
 * arms nor in the gun" symptom, exactly.
 *
 * Because `R(q)`'s columns ARE the basis handed over, the correction needs no matrix:
 * `R(q) * v == v.x * forward + v.y * right + v.z * up`, and HandPose carries all three vectors.
 * So one three-float constant in the hand's basis covers it at any orientation.
 *
 * It is one knob, not two, and that is worth knowing: the engine's `d` and the offset from the
 * palm to the weapon model's own origin are both fixed vectors in this same basis, so their sum is
 * the only thing that can ever be measured. Tuning it until pure rotation stops translating the
 * gun is therefore a complete calibration, and the residual is the instrument's noise floor.
 */
std::array<std::atomic<float>, 3> g_pivot{};
/**
 * Which of the controller's two OpenXR poses anchors the weapon's position.
 *
 * `true` (the default) uses the grip pose -- the palm centroid, which is the point a wrist really
 * rotates about and the point a held object's grip must coincide with. `false` uses the aim pose,
 * whose origin sits several centimetres in front of the hand in mid air; a weapon anchored there
 * pivots about a point outside the player's fist no matter how well `g_pivot` is tuned. Kept
 * switchable only so the difference can be measured rather than argued about.
 */
std::atomic_bool g_palmAnchor{true};
/** Set once when the transform hook had to sample the pose itself, so the log says so. */
std::atomic_bool g_latchMissed{false};
/**
 * Which callers of the transform builder get the weapon's offset written into their buffer.
 *
 * `+0xD5D7F0` is detoured wholesale, so without a gate every one of its callers is offset --
 * including any that turns out to place something other than the first-person weapon. The default
 * is still all of them, because that is the configuration measured to work; the counters logged by
 * `report` say whether there is more than one, and this is how it gets narrowed when there is.
 */
std::array<std::atomic<std::uintptr_t>, kMaxRuleRvas> g_xformTargets{};
std::atomic<std::size_t> g_xformTargetCount{0};

/** @return True when this caller of the transform builder should have the offset applied. */
bool xform_gated(std::uintptr_t rva) noexcept {
    const std::size_t count = g_xformTargetCount.load(std::memory_order_acquire);
    if (count == 0) {
        return true;
    }
    for (std::size_t i = 0; i < count && i < g_xformTargets.size(); ++i) {
        if (g_xformTargets[i].load(std::memory_order_relaxed) == rva) {
            return true;
        }
    }
    return false;
}
/** Forced values, for probing one lane at a time. NaN means leave it alone. */
std::array<std::atomic<float>, kXformFloats> g_xformForce{};
std::atomic_bool g_installed{false};
std::atomic_bool g_installTried{false};
std::atomic_bool g_blockOrientation{true};
std::atomic_bool g_blockPosition{true};
/**
 * Calibration nudge added to the weapon's world-space offset, in the game's own axes: X forward,
 * Y left, Z up, in metres. Live-tunable so the gun's rest position can be seated by eye.
 *
 * Deliberately NOT in the hand's basis: this one is for comfort trims that should stay put in the
 * world, where `g_pivot` is for the part that has to rotate with the gun. Anything captured at
 * runtime belongs in neither -- a snapshot is what made turning with the stick slide the weapon
 * away from the hands.
 */
std::array<std::atomic<float>, 3> g_handOffset{};
/**
 * One consistent sample of the head and the right hand, latched by the getter detour for the
 * transform detour to reuse.
 *
 * Thread local, and that is the mechanism rather than an implementation detail. The weapon's
 * transform builder `+0xD5D7F0` calls the pose getter itself, so on the thread that is building
 * the weapon's transform the getter runs first and this detour runs immediately after -- one
 * writer, one reader, same thread, no lock and no chance of pairing up two different frames. And
 * pairing matters: `R(q)` comes from the pose the getter handed over, so the `- R(q) * d`
 * correction has to be computed from that same sample or the cancellation leaves a residue the
 * size of one frame's hand movement, which is felt as jitter.
 */
struct Sample final {
    xr::HeadPose head{};
    xr::HandPose hand{};
    bool valid{};
};
thread_local Sample t_sample{};
std::uintptr_t g_base = 0;
std::uint32_t g_frame = 0;
/** Fake pose regions for the pointer path, one per player index the accessor is asked about. */
alignas(16) std::array<std::array<std::uint8_t, kFakeRegionBytes>, 4> g_fake{};

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

void count_caller(Path& path, std::uintptr_t rva) noexcept {
    for (Caller& caller : path.callers) {
        std::uintptr_t current = caller.rva.load(std::memory_order_acquire);
        if (current == 0) {
            std::uintptr_t expected = 0;
            current = caller.rva.compare_exchange_strong(expected, rva, std::memory_order_acq_rel) ? rva : expected;
        }
        if (current == rva) {
            caller.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    path.overflow.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @return The source this caller should be handed.
 * A rule naming the caller explicitly beats a catch-all, so `getter all body` can set the
 * background and `getter D5D832 hand` carve one caller out of it.
 */
Source decide(const Path& path, std::uintptr_t rva) noexcept {
    const RuleSet* set = path.rules.load(std::memory_order_acquire);
    if (set == nullptr) {
        return Source::none;
    }
    for (std::size_t i = 0; i < set->count; ++i) {
        const Rule& rule = set->rules[i];
        for (std::size_t j = 0; j < rule.count; ++j) {
            if (rule.rvas[j] == rva) {
                return rule.source;
            }
        }
    }
    for (std::size_t i = 0; i < set->count; ++i) {
        if (set->rules[i].all) {
            return set->rules[i].source;
        }
    }
    return Source::none;
}

/**
 * Builds the weapon pose from the right controller.
 *
 * The position is the engine's own camera position plus the hand's offset from the recentre
 * origin. Using the engine's position rather than the head's is what decouples them: the room
 * origin maps to the same world point for both, so leaning moves the head while the hand stays,
 * and the weapon can be approached.
 * @param out Receives the pose.
 * @return False when no controller pose has been located yet.
 */
bool hand_pose_for(Pose& out) noexcept {
    xr::HeadPose head{};
    xr::HandPose left{};
    xr::HandPose hand{};
    // One lock acquisition for both, so the head and the hand cannot come from different frames.
    xr::frame_sample(head, left, hand);
    if (!hand.valid) {
        return false;
    }
    // Latched for the transform detour, which runs later on this same thread and must correct for
    // the very orientation being handed over here.
    t_sample.head = head;
    t_sample.hand = hand;
    t_sample.valid = true;
    const Pose engine = engine_pose();
    // The position is filled in for completeness only: the weapon's draw ignores the position half
    // of this pose entirely (measured -- see the note on kXformRva), and its position is set through
    // the transform buffer's world-space offset lanes instead. What this pose really carries to the
    // weapon is the orientation.
    for (std::size_t lane = 0; lane < out.position.size(); ++lane) {
        out.position[lane] = engine.position[lane] + hand.offset[lane];
    }
    out.forward = hand.forward;
    out.up = hand.up;
    out.horizontalFov = engine.horizontalFov;
    out.aspect = engine.aspect;
    return true;
}

/** @return The pose a rule asks for, and whether it is usable this frame. */
bool pose_for(Source source, Pose& out) noexcept {
    switch (source) {
    case Source::body: out = engine_pose(); break;
    case Source::head: out = head_pose(); break;
    case Source::hand:
        if (!hand_pose_for(out)) {
            return false;
        }
        break;
    default: return false;
    }
    const float n = out.forward[0] * out.forward[0] + out.forward[1] * out.forward[1] + out.forward[2] * out.forward[2];
    return n > 0.5F && n < 1.5F;
}

void __fastcall getter_replacement(Vector* pos, Vector* fwd, Vector* up) noexcept {
    const auto rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base;
    const auto original = reinterpret_cast<Getter>(g_handles[0].original);
    if (original != nullptr) {
        original(pos, fwd, up);
    }
    count_caller(g_getter, rva);
    Pose pose{};
    if (!pose_for(decide(g_getter, rva), pose)) {
        return;
    }
    if (pos != nullptr) {
        *pos = pose.position;
    }
    if (fwd != nullptr) {
        *fwd = pose.forward;
    }
    if (up != nullptr) {
        *up = pose.up;
    }
}

float* __fastcall pose_ptr_replacement(int playerIndex) noexcept {
    const auto rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base;
    const auto original = reinterpret_cast<PosePtr>(g_handles[1].original);
    float* const real = original != nullptr ? original(playerIndex) : nullptr;
    count_caller(g_ptr, rva);
    Pose pose{};
    if (real == nullptr || !pose_for(decide(g_ptr, rva), pose)) {
        return real;
    }
    auto& fake = g_fake[static_cast<std::size_t>(playerIndex) & 3U];
    std::memcpy(fake.data(), real, fake.size());
    std::memcpy(fake.data(), pose.position.data(), sizeof(Vector));
    std::memcpy(fake.data() + kOffsetForward, pose.forward.data(), sizeof(Vector));
    std::memcpy(fake.data() + kOffsetUp, pose.up.data(), sizeof(Vector));
    return reinterpret_cast<float*>(fake.data());
}

/**
 * Overwrites, and optionally reports, the 32 bytes the weapon's transform builder produced.
 * Runs on the camera thread after the original, so the caller reads whatever is left here.
 */
void __fastcall xform_replacement(void* out) noexcept {
    const auto rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base;
    const auto original = reinterpret_cast<Xform>(g_handles[2].original);
    void* const result = original != nullptr ? original(out) : nullptr;
    count_caller(g_xform, rva);
    if (out == nullptr) {
        return;
    }
    auto* const lanes = static_cast<float*>(out);
    std::array<float, kXformFloats> before{};
    std::memcpy(before.data(), lanes, sizeof before);
    for (std::size_t lane = 0; lane < kXformFloats; ++lane) {
        const float forced = g_xformForce[lane].load(std::memory_order_relaxed);
        if (std::isfinite(forced)) {
            lanes[lane] = forced;
        }
    }
    if (g_xformDelta.load(std::memory_order_acquire) && xform_gated(rva)) {
        // The sample the getter handed to the weapon this frame, on this thread. Falling back to a
        // fresh read keeps a misconfigured session working (`getter ... body`, say), but the pivot
        // correction is then computed against an orientation the engine was not given, so it is
        // worth knowing about.
        xr::HeadPose head{};
        xr::HandPose hand{};
        if (t_sample.valid) {
            head = t_sample.head;
            hand = t_sample.hand;
            t_sample.valid = false;
        } else {
            xr::HandPose left{};
            xr::frame_sample(head, left, hand);
            g_latchMissed.store(true, std::memory_order_relaxed);
        }
        if (hand.valid && head.valid) {
            // Where the weapon must end up, stated once:
            //
            //     weapon_world = eye_world + (hand_room - head_room)
            //
            // i.e. the controller's true position relative to the head, carried into the world.
            // The engine is already going to draw it at `camera + R(q) * d + lanes` and its camera
            // is the eye this module wrote, so what is left to write is
            //
            //     lanes = (palm - head) + R(q) * pivot
            //
            // with `pivot` absorbing both the engine's own viewmodel offset and the palm-to-model
            // origin offset (see g_pivot).
            //
            // Everything in it is read from THIS frame. There is no captured reference and nothing
            // to go stale, which is the property that matters: the room anchor's yaw fold is
            // already inside both `palm` and `head.offset`, so it cancels in the difference and an
            // artificial turn cannot move the weapon relative to the player. The previous version
            // subtracted a snapshot of the folded hand offset, which did not rotate with the
            // anchor and grew into over a metre of phantom translation at 180 degrees of turn.
            const bool palmAnchor = g_palmAnchor.load(std::memory_order_relaxed);
            const xr::Vector& anchor = palmAnchor ? hand.palm : hand.offset;
            const float pivotForward = g_pivot[0].load(std::memory_order_relaxed);
            const float pivotRight = g_pivot[1].load(std::memory_order_relaxed);
            const float pivotUp = g_pivot[2].load(std::memory_order_relaxed);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const int lane = g_xformLanes[axis].load(std::memory_order_relaxed);
                if (lane < 0 || lane >= static_cast<int>(kXformFloats)) {
                    continue;
                }
                // R(q) * pivot, without a matrix: the basis handed to the getter IS R(q)'s
                // columns, so the correction rotates rigidly with the gun at any orientation.
                const float correction = pivotForward * hand.forward[axis] + pivotRight * hand.right[axis]
                                         + pivotUp * hand.up[axis];
                lanes[static_cast<std::size_t>(lane)] += anchor[axis] - head.offset[axis] + correction
                                                         + g_handOffset[axis].load(std::memory_order_relaxed);
            }
        }
    }
    if (g_xformDump.load(std::memory_order_acquire) && (g_frame % kPollPeriod) == 0) {
        reportf("ev=vr.weapon xform caller=%llX in=%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f "
                "out=%.4f,%.4f,%.4f latch_missed=%d",
                static_cast<unsigned long long>(rva), before[0], before[1], before[2], before[3], before[4],
                before[5], before[6], before[7], lanes[4], lanes[5], lanes[6],
                g_latchMissed.exchange(false, std::memory_order_relaxed) ? 1 : 0);
    }
    static_cast<void>(result);
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

void install() noexcept {
    if (g_installTried.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    for (auto& forced : g_xformForce) {
        forced.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_release);
    }
    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_base == 0) {
        report("ev=vr.weapon install result=fail reason=no_base");
        return;
    }
    const std::uintptr_t getter = g_base + kGetterRva;
    const std::uintptr_t posePtr = g_base + kPosePtrRva;
    const std::uintptr_t xform = g_base + kXformRva;
    if (!prologue_matches(getter, kGetterPrologue) || !prologue_matches(posePtr, kPosePtrPrologue)
        || !prologue_matches(xform, kXformPrologue)) {
        report("ev=vr.weapon install result=fail reason=prologue_mismatch");
        return;
    }
    const std::array<hooking::detour::Spec, 3> specs{
        hooking::detour::Spec{reinterpret_cast<void*>(getter), reinterpret_cast<void*>(&getter_replacement)},
        hooking::detour::Spec{reinterpret_cast<void*>(posePtr), reinterpret_cast<void*>(&pose_ptr_replacement)},
        hooking::detour::Spec{reinterpret_cast<void*>(xform), reinterpret_cast<void*>(&xform_replacement)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        report("ev=vr.weapon install result=fail reason=detour");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    reportf("ev=vr.weapon install result=ok getter=0x%llX ptr=0x%llX xform=0x%llX",
            static_cast<unsigned long long>(kGetterRva), static_cast<unsigned long long>(kPosePtrRva),
            static_cast<unsigned long long>(kXformRva));
}

void report_callers(const char* name, Path& path) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int used = std::snprintf(line.data(), line.size(), "ev=vr.weapon callers path=%s", name);
    for (Caller& caller : path.callers) {
        const std::uintptr_t rva = caller.rva.load(std::memory_order_acquire);
        if (rva == 0 || used < 0 || static_cast<std::size_t>(used) >= line.size() - 24) {
            break;
        }
        const std::uint64_t count = caller.count.exchange(0, std::memory_order_relaxed);
        const int n = std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used), " %llX:%llu",
                                    static_cast<unsigned long long>(rva), static_cast<unsigned long long>(count));
        if (n <= 0) {
            break;
        }
        used += n;
    }
    const std::uint64_t overflow = path.overflow.exchange(0, std::memory_order_relaxed);
    if (overflow != 0 && used > 0 && static_cast<std::size_t>(used) < line.size() - 24) {
        used += std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used), " overflow=%llu",
                              static_cast<unsigned long long>(overflow));
    }
    if (used > 0) {
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(used)});
    }
}

/** Directory of the executable plus the given file name. */
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
 * Parses `all|none|<rva>[,<rva>...]` and `body|head|hand` and merges it into the path's rule set.
 *
 * `none` clears every rule. Otherwise the rule replaces the one with the same target list if there
 * is one, and is appended if not, so rules for different callers accumulate instead of evicting
 * each other.
 */
void apply_rule(Path& path, const char* targets, const char* source, const char* name) noexcept {
    Rule rule{};
    if (std::strcmp(source, "body") == 0) {
        rule.source = Source::body;
    } else if (std::strcmp(source, "head") == 0) {
        rule.source = Source::head;
    } else if (std::strcmp(source, "hand") == 0) {
        rule.source = Source::hand;
    } else {
        rule.source = Source::none;
    }
    if (std::strcmp(targets, "all") == 0) {
        rule.all = true;
    } else if (std::strcmp(targets, "none") != 0) {
        const char* cursor = targets;
        while (*cursor != '\0' && rule.count < rule.rvas.size()) {
            char* end = nullptr;
            const unsigned long long value = std::strtoull(cursor, &end, 16);
            if (end == cursor) {
                break;
            }
            rule.rvas[rule.count++] = static_cast<std::uintptr_t>(value);
            cursor = (*end == ',') ? end + 1 : end;
        }
    }
    RuleSet& set = path.sets[path.nextSet];
    path.nextSet = (path.nextSet + 1) % path.sets.size();
    const RuleSet* current = path.rules.load(std::memory_order_acquire);
    set = current != nullptr ? *current : RuleSet{};
    if (std::strcmp(targets, "none") == 0 || rule.source == Source::none) {
        set.count = 0;
    } else {
        std::size_t slot = set.count;
        for (std::size_t i = 0; i < set.count; ++i) {
            const Rule& existing = set.rules[i];
            if (existing.all == rule.all && existing.count == rule.count
                && std::memcmp(existing.rvas.data(), rule.rvas.data(), rule.count * sizeof(std::uintptr_t)) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < set.rules.size()) {
            set.rules[slot] = rule;
            if (slot == set.count) {
                ++set.count;
            }
        }
    }
    path.rules.store(&set, std::memory_order_release);
    reportf("ev=vr.weapon rule path=%s targets=%s source=%s rvas=%zu all=%d rules=%zu", name, targets, source,
            rule.count, rule.all ? 1 : 0, set.count);
}

/**
 * The configuration proven in Io on 2026-09-09: the head drives the camera block, the controller's
 * orientation reaches the weapon through its own getter caller, and the controller's travel reaches
 * it through the transform buffer's world-space offset lanes. A command file can still override any
 * of it afterwards.
 */
void apply_defaults() noexcept {
    g_blockPosition.store(true, std::memory_order_release);
    g_blockOrientation.store(true, std::memory_order_release);
    apply_rule(g_getter, "D5D832", "hand", "getter");
    for (std::size_t axis = 0; axis < g_xformLanes.size(); ++axis) {
        g_xformLanes[axis].store(static_cast<int>(4 + axis), std::memory_order_release);
    }
    g_palmAnchor.store(true, std::memory_order_release);
    // Measured in Io on 2026-09-09, not guessed. `Solve-PivotRoll.ps1` drove the translation caused
    // by a +/-20 degree wrist ROLL to zero: 72.0 px at the start, 0.0 px at the solution, with both
    // signs of the roll reading exactly zero. Roll is the axis to solve on because it turns the
    // weapon in the image plane and preserves its silhouette -- a yaw or pitch foreshortens the
    // gun, and correlation then answers with the offset that best fits a changed shape, worth about
    // 100 px with no translation behind it.
    //
    // Roll is blind to the component along its own axis, so the forward one is taken from the
    // independent measurement in RESEARCH.md: 0.08 m of lane travel subtending 13.5 degrees puts
    // the engine's viewmodel offset at 0.33 m. The two routes agree -- |pivot| comes out at 0.359 m
    // against that 0.33 m -- and that agreement is the check that this is the engine's real offset
    // rather than a fudge that happens to cancel one test.
    //
    // The final seating, whether the grip falls where the hand actually is, is a perceptual
    // judgement that needs the headset; `pivot x y z` in the command file tunes it live.
    constexpr std::array<float, 3> kMeasuredPivot{-0.3300F, -0.0915F, 0.1090F};
    for (std::size_t axis = 0; axis < g_pivot.size(); ++axis) {
        g_pivot[axis].store(kMeasuredPivot[axis], std::memory_order_release);
    }
    g_xformTargetCount.store(0, std::memory_order_release);
    g_xformDelta.store(true, std::memory_order_release);
    reportf("ev=vr.weapon defaults applied=1 anchor=palm pivot=%.4f,%.4f,%.4f", kMeasuredPivot[0],
            kMeasuredPivot[1], kMeasuredPivot[2]);
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
        char a[128] = {};
        char b[16] = {};
        const int n = sscanf_s(line.data(), "%15s %127s %15s", verb, static_cast<unsigned>(sizeof verb), a,
                               static_cast<unsigned>(sizeof a), b, static_cast<unsigned>(sizeof b));
        if (n < 1) {
            continue;
        }
        if (std::strcmp(verb, "getter") == 0 && n >= 3) {
            apply_rule(g_getter, a, b, "getter");
        } else if (std::strcmp(verb, "ptr") == 0 && n >= 3) {
            apply_rule(g_ptr, a, b, "ptr");
        } else if (std::strcmp(verb, "block") == 0 && n >= 2) {
            // `block pos|orient on|off` addresses one field; bare `block on|off` sets both, which
            // keeps every existing command file and every note in the docs working unchanged.
            const bool split = std::strcmp(a, "pos") == 0 || std::strcmp(a, "orient") == 0;
            const bool on = split ? (n >= 3 && std::strcmp(b, "on") == 0) : (std::strcmp(a, "on") == 0);
            if (!split || std::strcmp(a, "orient") == 0) {
                g_blockOrientation.store(on, std::memory_order_release);
            }
            if (!split || std::strcmp(a, "pos") == 0) {
                g_blockPosition.store(on, std::memory_order_release);
            }
            reportf("ev=vr.weapon block position=%s orientation=%s",
                    g_blockPosition.load(std::memory_order_acquire) ? "on" : "off",
                    g_blockOrientation.load(std::memory_order_acquire) ? "on" : "off");
        } else if (std::strcmp(verb, "hand_offset") == 0) {
            // Parsed off the raw line: the three values are floats and can be negative, which the
            // three-token scan above cannot express.
            float values[3] = {};
            if (sscanf_s(line.data(), "%*s %f %f %f", &values[0], &values[1], &values[2]) == 3) {
                for (std::size_t lane = 0; lane < g_handOffset.size(); ++lane) {
                    g_handOffset[lane].store(values[lane], std::memory_order_release);
                }
                // World axes, not the hand's: X forward, Y LEFT, Z up. The old labels said
                // fwd/right/up, which is the basis `pivot` uses and this one does not.
                reportf("ev=vr.weapon hand_offset x=%.3f y=%.3f z=%.3f", values[0], values[1], values[2]);
            } else {
                report("ev=vr.weapon command result=fail verb=hand_offset reason=parse");
            }
        } else if (std::strcmp(verb, "pivot") == 0) {
            // The one constant the placement needs, in the controller's own basis: forward, right,
            // up, in metres. Live-tunable because the way it is measured is a search: sweep the
            // controller's rotation with its position held and drive the weapon's screen travel to
            // zero. See g_pivot.
            float values[3] = {};
            if (sscanf_s(line.data(), "%*s %f %f %f", &values[0], &values[1], &values[2]) == 3) {
                for (std::size_t axis = 0; axis < g_pivot.size(); ++axis) {
                    g_pivot[axis].store(values[axis], std::memory_order_release);
                }
                reportf("ev=vr.weapon pivot fwd=%.3f right=%.3f up=%.3f mag=%.3f", values[0], values[1],
                        values[2],
                        std::sqrt(values[0] * values[0] + values[1] * values[1] + values[2] * values[2]));
            } else {
                report("ev=vr.weapon command result=fail verb=pivot reason=parse");
            }
        } else if (std::strcmp(verb, "turn") == 0) {
            // Turns the room anchor by an exact angle, which is what artificial turning does.
            // Here so the decisive regression -- "turning with the stick must not move the weapon
            // relative to the player" -- can be driven to a known angle instead of holding a
            // synthetic stick for a guessed length of time. It is the test lever for the bug that
            // reached over a metre of phantom translation at 180 degrees.
            float degrees = 0.0F;
            if (sscanf_s(line.data(), "%*s %f", &degrees) == 1) {
                turn_room(degrees * 3.14159265F / 180.0F);
                reportf("ev=vr.weapon turn degrees=%.2f", degrees);
            } else {
                report("ev=vr.weapon command result=fail verb=turn reason=parse");
            }
        } else if (std::strcmp(verb, "anchor") == 0 && n >= 2) {
            const bool palm = std::strcmp(a, "aim") != 0;
            g_palmAnchor.store(palm, std::memory_order_release);
            reportf("ev=vr.weapon anchor=%s", palm ? "palm" : "aim");
        } else if (std::strcmp(verb, "xform") == 0 && n >= 2) {
            // Everything here is live-tunable on purpose: one rebuild plus a six-minute reload is
            // the cost of a wrong guess about those eight floats, so the guessing happens through
            // the command file instead.
            if (std::strcmp(a, "dump") == 0) {
                const bool on = n < 3 || std::strcmp(b, "off") != 0;
                g_xformDump.store(on, std::memory_order_release);
                reportf("ev=vr.weapon xform dump=%s", on ? "on" : "off");
            } else if (std::strcmp(a, "delta") == 0) {
                // `abs` and `rel` are still accepted so existing command files and scripts keep
                // parsing, but there is only one mode now: the relative one WAS the bug.
                const bool off = n >= 3 && std::strcmp(b, "off") == 0;
                g_xformDelta.store(!off, std::memory_order_release);
                reportf("ev=vr.weapon xform delta=%s mode=absolute", off ? "off" : "on");
            } else if (std::strcmp(a, "ref") == 0) {
                report("ev=vr.weapon xform ref result=noop reason=no_reference_exists");
            } else if (std::strcmp(a, "caller") == 0 && n >= 3) {
                std::size_t count = 0;
                if (std::strcmp(b, "all") != 0) {
                    const char* cursor = b;
                    while (*cursor != '\0' && count < g_xformTargets.size()) {
                        char* end = nullptr;
                        const unsigned long long value = std::strtoull(cursor, &end, 16);
                        if (end == cursor) {
                            break;
                        }
                        g_xformTargets[count++].store(static_cast<std::uintptr_t>(value),
                                                      std::memory_order_relaxed);
                        cursor = (*end == ',') ? end + 1 : end;
                    }
                }
                g_xformTargetCount.store(count, std::memory_order_release);
                reportf("ev=vr.weapon xform caller targets=%s count=%zu", b, count);
            } else if (std::strcmp(a, "lanes") == 0) {
                int lanes[3] = {};
                if (sscanf_s(line.data(), "%*s %*s %d %d %d", &lanes[0], &lanes[1], &lanes[2]) == 3) {
                    for (std::size_t axis = 0; axis < g_xformLanes.size(); ++axis) {
                        g_xformLanes[axis].store(lanes[axis], std::memory_order_release);
                    }
                    reportf("ev=vr.weapon xform lanes=%d,%d,%d", lanes[0], lanes[1], lanes[2]);
                }
            } else if (std::strcmp(a, "force") == 0) {
                int lane = -1;
                float value = 0.0F;
                if (sscanf_s(line.data(), "%*s %*s %d %f", &lane, &value) == 2 && lane >= 0
                    && lane < static_cast<int>(kXformFloats)) {
                    g_xformForce[static_cast<std::size_t>(lane)].store(value, std::memory_order_release);
                    reportf("ev=vr.weapon xform force lane=%d value=%.4f", lane, value);
                }
            } else if (std::strcmp(a, "clear") == 0) {
                for (auto& forced : g_xformForce) {
                    forced.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_release);
                }
                g_xformDelta.store(false, std::memory_order_release);
                report("ev=vr.weapon xform cleared");
            } else {
                reportf("ev=vr.weapon command result=fail verb=xform arg=%s", a);
            }
        } else if (std::strcmp(verb, "install") == 0) {
            install();
        } else if (std::strcmp(verb, "report") == 0) {
            report_callers("getter", g_getter);
            report_callers("ptr", g_ptr);
            report_callers("xform", g_xform);
        } else {
            reportf("ev=vr.weapon command result=fail verb=%s", verb);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

} // namespace

void tick() noexcept {
    ++g_frame;
    if (g_frame % kPollPeriod == 0) {
        poll_commands();
    }
    // F9 brings the whole weapon configuration up by itself, so a headset session needs no command
    // file at all. Before F9 nothing here has touched the engine, which keeps a plain session --
    // and destination loading in particular -- byte-for-byte the pre-F3 behaviour.
    if (!g_installTried.load(std::memory_order_acquire) && enabled() && xr::active()) {
        install();
        if (g_installed.load(std::memory_order_acquire)) {
            apply_defaults();
        }
    }
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    if (g_frame % kReportPeriod == 0) {
        report_callers("getter", g_getter);
        report_callers("ptr", g_ptr);
        report_callers("xform", g_xform);
    }
}

bool block_orientation_enabled() noexcept {
    return g_blockOrientation.load(std::memory_order_acquire);
}

bool block_position_enabled() noexcept {
    return g_blockPosition.load(std::memory_order_acquire);
}

void retake_reference() noexcept {
    // Nothing to retake any more, and that is the fix rather than an omission. The weapon's
    // placement is a pure function of this frame's head and hand poses, so a recentre moves the
    // shared origin and both move with it; there is no stored reference left that could disagree
    // with the new one. Kept as a symbol so F7's handler reads the same as it always did.
    report("ev=vr.weapon recentre result=noop reason=stateless_placement");
}

} // namespace sunrise::client::hooks::vr::weapon
