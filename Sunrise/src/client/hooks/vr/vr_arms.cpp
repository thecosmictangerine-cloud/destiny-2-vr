/**
 * The arms: each one reaches for its own controller, the weapon stays in the right hand.
 *
 * Destiny draws the first-person arms as three skinned meshes over one 72-bone skeleton (the
 * gloves, and two gauntlet layers that carry the upper arm and the shoulder). Their bone palette
 * reaches the GPU in constant buffer 11, camera-relative, and vr_gpu.cpp hands it to rewrite()
 * just before each arm draw. Nothing about the engine's own skeleton is touched: the arms are
 * re-posed on the way to the GPU, the way dishonored-vr did it (docs/ARMS.md).
 *
 * A skinning matrix is M = W * B^-1, so a rigid change D of a bone's world transform is simply
 * D * M -- no bind pose is ever needed to MOVE a bone. What IS needed is where the joints are, so
 * a segment can be turned about its own shoulder, elbow or wrist. Those come from two sources:
 *
 *   - the meshes: where the skin weights of two bones overlap is where the skin bends from one to
 *     the other, so the weight-product centroid of a bone pair sits on their joint. That gives
 *     the topology and a first estimate the moment the vertex buffers are read;
 *   - the animation: a joint J shared by parent p and child c satisfies (R_p - R_c) J = t_c - t_p
 *     in every frame, which a few frames of different poses pin down by least squares. Measured
 *     in Io across idle, ADS, sprint and reload: residual 0.1 mm at the shoulder, 0 at the elbow
 *     and wrist.
 *
 * The pose itself, per arm, every draw:
 *
 *   body      chest and clavicles in their bind pose, carried by a body frame hung from the neck
 *             (the neck is where the head pivots, so looking down does not drag the shoulders)
 *             and turned to where the head faces.
 *   hand      RIGHT: exactly where the game animated it, which is already on the gun, which the
 *             weapon module already put on the controller. LEFT: on the left controller, held
 *             the way the right hand holds the gun, mirrored -- a closed grip, aligned with the
 *             controller as a real hand around a Touch controller is.
 *   elbow     two-bone IK between the shoulder and the wrist, the elbow bent towards an
 *             anatomical pole (down, out, slightly back).
 *   segments  each upper arm and forearm placed from its BIND geometry onto the solved joints,
 *             with the forearm's roll split along its length (the radius turns about the ulna;
 *             the wrist end follows the hand, the elbow end hardly at all).
 *   fingers   the game's own finger pose, carried rigidly with the hand.
 *
 * Commands, from SVR_Arms.txt in the game directory (numbers with a dot):
 *   enable on|off      overlay on|off      dump
 *   neck <fwd> <up>              eye offset from the neck pivot, metres (default 0.08 0.10)
 *   shoulder <fwd> <up>          shoulder midpoint from the neck, metres (default -0.03 -0.12)
 *   pole <fwd> <out> <up>        elbow pole in the body frame (default -0.15 0.45 -0.88)
 *   stretch <max>                largest arm stretch before the shoulder slides (default 1.12)
 *   twist <forearm> <wrist>      share of the hand's roll each forearm bone takes (0.45 0.9)
 *   rig <L|R> <clav> <upperTwist> <upper> <fore> <foreTwist> <hand>     override the labels
 *   relearn                      forget the left hand's learned grip
 *   twohand on|off               the two-handed hold as a whole (off: the left grip is the grenade)
 *   twohand aim on|off           let a foregrip steer the barrel (off: every hold is visual only)
 *   twohand engage <take> <grenade>   radii, metres (default 0.10 0.25)
 *   twohand slide <back> <fwd>   the handguard around the learned foregrip (default 0.12 0.10)
 *   twohand pistol <m>           foregrip nearer the right palm than this is a support hand (0.18)
 *   twohand blend <s>            seconds to take hold or let go (default 0.12)
 *   twohand relearn | dump
 *
 * The two-handed hold (docs/TWO-HANDS.md). The game's own animated LEFT hand sits on the weapon's
 * foregrip for every weapon, as Bungie authored it, so that is where the left hand takes hold --
 * but learned relative to the right hand while steady, never followed live: reload, inspect and
 * sprint take the animated hand off the gun, and a held hand that flew off with it would be
 * absurd. Squeezing the left grip near the handguard takes hold; away from the gun it is the
 * grenade. While a foregrip is held the weapon module turns the gun about the right palm so the
 * handguard runs exactly through the left palm (vr_two_hand.h), and the hand slides along it. A
 * foregrip close to the right palm is a pistol's support hand: held, drawn, never steering.
 */

#include "vr_arms.h"

#include <Windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../core/logging/log.h"
#include "../graphics/renderer/world_lines.h"
#include "runtime.h"
#include "vr_ads.h"
#include "vr_dual.h"
#include "vr_frames.h"
#include "vr_gpu.h"
#include "vr_math.h"
#include "vr_two_hand.h"
#include "vr_weapon.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::arms {
namespace {

using namespace math;
namespace wl = graphics::renderer::world_lines;

constexpr std::size_t kMaxBones = 128;
constexpr wchar_t kCommandName[] = L"SVR_Arms.txt";
constexpr std::uint32_t kPollPeriod = 30;
constexpr std::uint32_t kReportPeriod = 240;
/** Pair overlap (in vertex-weight units) below which two bones are not considered joined. */
constexpr double kPairMin = 1.0;
/** Forgetting factor of the joint least squares, per observed frame. */
constexpr double kForget = 0.999;
/** Ridge weight pulling each joint towards the mesh-overlap estimate. */
constexpr double kPriorWeight = 0.02;
/** Furthest a learned joint may move from that estimate, metres. */
constexpr float kMaxJointShift = 0.18F;

enum Side : int { left = 0, right = 1 };
enum Seg : std::uint8_t { segNone, segBody, segUpper, segFore, segForeTwist, segHand };

struct SideRig final {
    int clav{-1};
    int upperTwist{-1};
    int upper{-1};
    int fore{-1};
    int foreTwist{-1};
    int hand{-1};
};

struct Labels final {
    int chest{11};
    /**
     * Destiny's first-person skeleton, read off the weight graph in Io (2026-10-15): 11 chest;
     * left 12 clavicle, 27 shoulder twist, 15 upper arm, 19 forearm, 34 wrist twist, 21 hand;
     * right 14, 28, 17, 20, 39, 22. The player skeleton is shared by every armour piece (all
     * meshes index the same palette), so this holds for any gauntlets; it is still validated
     * against the meshes before use, and `rig` overrides it.
     */
    std::array<SideRig, 2> side{SideRig{12, 27, 15, 19, 34, 21}, SideRig{14, 28, 17, 20, 39, 22}};
};

struct PairAcc final {
    double mass{};
    std::array<double, 3> p{};
};

/** Joint least squares: (sum D^T D) J = sum D^T e, with D = R_p - R_c and e = t_c - t_p. */
struct JointLs final {
    std::array<std::array<double, 3>, 3> ata{};
    std::array<double, 3> atb{};
    double frames{};
};

struct Rig final {
    bool valid{};
    std::array<std::uint8_t, kMaxBones> seg{};
    std::array<std::int8_t, kMaxBones> side{};
    std::array<float, kMaxBones> twist{};
    /** Bind joints: shoulder, elbow, wrist, per side. */
    std::array<V3, 2> s{};
    std::array<V3, 2> e{};
    std::array<V3, 2> w{};
    /** Palm centre: the hand bone's skin centroid, the point the controller's grip pose holds. */
    std::array<V3, 2> palm{};
    std::array<std::array<V3, 3>, 2> prior{};
    std::array<M3, 2> upperBind{};
    std::array<M3, 2> foreBind{};
    std::array<float, 2> l1{};
    std::array<float, 2> l2{};
    V3 shoulderMid{};
    std::size_t fingers{};
    /** Bind-space skin centroid of every bone: the pivot finger poses are blended about. */
    std::array<V3, kMaxBones> cen{};
    /** For each left finger bone, the right finger bone that mirrors it (-1 if none). */
    std::array<int, kMaxBones> mirrorOf{};
    /** One past the highest hand or finger bone: a palette this long carries every finger. */
    std::size_t fingerSpan{};
};

/** Tunables, published as one block so a draw never reads half an update. */
struct Params final {
    float neckFwd{0.08F};
    float neckUp{0.10F};
    float shoulderFwd{-0.03F};
    float shoulderUp{-0.12F};
    V3 pole{-0.15F, 0.45F, -0.88F};
    float stretchMax{1.12F};
    float twistFore{0.45F};
    float twistWrist{0.90F};
};

struct Overlay final {
    bool valid{};
    std::array<V3, 2> s{};
    std::array<V3, 2> e{};
    std::array<V3, 2> w{};
    std::array<Affine, 2> controller{};
    std::array<Affine, 2> hand{};
    std::array<float, 2> stretch{};
    std::array<float, 2> twist{};
    std::array<float, 2> wristError{};
    std::array<bool, 2> slid{};
    /** The hold as drawn: weight, the handguard on the drawn gun, and where the left palm went. */
    float holdW{};
    bool holdAim{};
    bool foreValid{};
    V3 guardA{};
    V3 guardB{};
    V3 snapPalm{};
    V3 ctrlPalmL{};
    /** Left controller palm to the drawn handguard line, and to the drawn left palm, metres. */
    float lineError{};
    float handError{};
    float slide{};
};

SRWLOCK g_lock = SRWLOCK_INIT;
Labels g_labels{};
Rig g_rig{};
Params g_params{};
std::array<std::array<JointLs, 3>, 2> g_ls{};
std::uint64_t g_observed = 0;

// Mesh accumulators (present thread, under g_lock).
std::array<double, kMaxBones> g_mass{};
std::array<std::array<double, 3>, kMaxBones> g_centroid{};
std::unordered_map<std::uint32_t, PairAcc> g_pairs;
std::unordered_set<std::uintptr_t> g_meshes;
std::size_t g_vertices = 0;

std::atomic_bool g_enabled{true};
std::atomic_bool g_overlay{false};
std::atomic<std::uint32_t> g_frame{0};
std::atomic<std::uint32_t> g_observedFrame{0xFFFFFFFFU};
std::atomic<std::uint64_t> g_rewrites{0};

/** The left hand's grip, learned from the right hand on the gun, in the controller's frame. */
struct Grip final {
    bool valid{};
    Affine rel{};
    std::uint32_t frame{0xFFFFFFFFU};
    /** When the measured grip started disagreeing with the learned one (ms), or 0. */
    std::uint64_t strikeSince{};
};
Grip g_grip{};
V3 g_lastCtrlR{};

/**
 * The right hand's hold on a weapon, kept per weapon for the LEFT hand (dual wield, docs/DUAL-WIELD.md):
 * the hand in the right controller's frame and every right finger in the hand's frame, taken while the
 * weapon was in the right hand and at rest. The left hand holds the left weapon as this, mirrored --
 * the same mirror that places the gun -- so hand and gun agree whatever weapon it is.
 */
struct GripSnapshot final {
    std::uintptr_t key{};
    Affine hand{};
    std::array<Affine, kMaxBones> finger{};
    std::array<bool, kMaxBones> fingerSet{};
};
std::array<GripSnapshot, 8> g_snapshots{};
std::atomic<std::uintptr_t> g_snapshotWanted{0};
float g_seatError = 0.0F;
/**
 * The canonical right-hand grip: the hand's rotation in the controller's (forward, left, up)
 * frame when holding the hand cannon at rest -- the pistol hold Bungie animated, with the barrel
 * along the controller's aim. Every weapon is turned (through the weapon module's rotational seat)
 * until the right hand holds it this way. Measured in Io on the mock (2026-10-15); `canon learn`
 * re-measures it from the weapon in hand.
 *
 * On its own that does NOT point every weapon where the controller points: each weapon's animation
 * holds it in the hand at its own angle, and the auto rifle came out 13 deg off (9 sideways, 7 up).
 * So once a weapon's sight is learned (vr_ads.cpp), the seat's target is swung by the smallest
 * rotation that lays the SIGHT along the controller's aim, as VR shooters align a weapon with the
 * pointing ray; the canonical hand only decides the roll about it, and the hand fits the gun.
 */
M3 g_canon = mat_of({0.08357F, -0.35772F, -0.02561F, 0.92973F});
bool g_canonSet = true;
std::uint64_t g_canonSteadySince = 0;
M3 g_seatQ{};
float g_seatAngle = 0.0F;
std::uint64_t g_seatBadSince = 0;
std::atomic_bool g_rotSeat{true};
/**
 * The sight's pitch relative to the controller's aim ray, radians, positive up: the "grip angle" VR
 * shooters let players set, because people disagree about how a Touch controller is held. 0 = the
 * runtime's own pointing ray. `canon aim <deg>`.
 */
std::atomic<float> g_aimPitch{0.0F};
Overlay g_overlayData{};
/** Body yaw, smoothed on the present thread and read by the draws. */
std::atomic<float> g_bodyYaw{0.0F};
std::atomic_bool g_bodyYawValid{false};
/** Whether the right controller was still in the last frame the grip block ran. */
bool g_ctrlSteady = false;

// ---------------------------------------------------------------------------------------------
// The two-handed hold: state
// ---------------------------------------------------------------------------------------------

/** Tunables of the hold, `twohand ...` in SVR_Arms.txt. Under g_lock. */
struct HoldParams final {
    bool on{true};
    /** Let a foregrip steer the barrel. Off, every hold is a support hand: drawn, never steering. */
    bool aim{true};
    /** Squeezing within this of the handguard takes hold, metres. */
    float engage{0.10F};
    /**
     * Squeezing beyond this throws a grenade. In between the press is the hold's and waits for the
     * hand to arrive -- a hand closes as it reaches, and that must not lob a grenade.
     */
    float grenade{0.25F};
    /** The handguard: how far the hand may slide back towards the right hand, and forward. */
    float slideBack{0.12F};
    float slideFwd{0.10F};
    /** A foregrip nearer the right palm than this is a support hand under a pistol, not a foregrip. */
    float pistol{0.18F};
    /** A support hand lets go when its controller strays this far from it, metres. */
    float releaseSupport{0.18F};
    /** A foregrip lets go when the hand slides this far past either end of the handguard... */
    float releaseSlide{0.12F};
    /** ... or when keeping it would turn the gun further than this from the right hand's aim (75 deg)... */
    float maxSwing{1.31F};
    /** ... or when the hands come closer than this. */
    float minLever{0.10F};
    /** Seconds to take hold, and to let go. */
    float blend{0.12F};
};

/**
 * Where the left hand holds THIS weapon, learned from the game's own animation. Under g_lock.
 *
 * `rel` is the animated left hand's frame in the animated right hand's: rigid on the gun while the
 * left hand is on it, so it follows the gun through recoil, bob and the seat loops with no thread
 * timing anywhere in it. It is only ever taken from a hand that has been STILL on the gun for a
 * while: a reload moves the left hand continuously, a weapon swap leaves a new, steady grip.
 */
struct Foregrip final {
    bool valid{};
    /** Confirmed since the last thing that may have changed the weapon (a swap, a melee, a super). */
    bool fresh{};
    /**
     * When (ms) that last thing happened. Nothing is confirmed for a while after it: a swap LOWERS
     * the old gun with both hands still rigidly on it, so for the first part of the animation the
     * old foregrip reads as perfectly still and would confirm itself straight back.
     */
    std::uint64_t doubtSince{};
    /** Counts foregrips adopted; a hold notices the weapon changed under it by this. */
    std::uint32_t generation{};
    Affine rel{};
    Affine prev{};
    bool prevValid{};
    std::uint64_t stillSince{};
    /** The foregrip's palm in the right controller's raw (forward, LEFT, up) basis, from its palm. */
    V3 fc{};
    bool fcValid{};
    /** The fingers around the foregrip, relative to the left hand frame, as the game animated them. */
    std::array<Affine, kMaxBones> fingers{};
    std::array<bool, kMaxBones> fingerSet{};
};

enum class Hold : std::uint8_t { free, pending, held, grenade, suspended };

const char* hold_name(Hold h) noexcept {
    switch (h) {
    case Hold::free: return "free";
    case Hold::pending: return "pending";
    case Hold::held: return "held";
    case Hold::grenade: return "grenade";
    default: return "suspended";
    }
}

/** The hold's state machine. Present thread only; what others need is published below. */
struct HoldMachine final {
    Hold state{Hold::free};
    bool down{};
    bool lastY{};
    bool lastMelee{};
    bool lastSuper{};
    /** Frozen when hold is taken: the mode, the foregrip and the handguard it steers by. */
    bool aim{};
    std::uint32_t generation{};
    V3 fc{};
    float sMin{};
    float sMax{};
    /** Linear 0..1 progress of taking hold; the published weight is it eased. */
    float ramp{};
    std::uint64_t suspendedAt{};
    LARGE_INTEGER last{};
    /** Left palm in the right controller's frame while tracked (see vr_weapon.cpp, same idea). */
    V3 leftInRight{};
    bool leftInRightValid{};
    float dist{};
    float swingAngle{};
    float slide{};
    float lever{};
    std::uint32_t grabs{};
    std::uint32_t releases{};
    std::uint32_t grenades{};
    const char* lastReason{"none"};
};

HoldParams g_hold{};
Foregrip g_fore{};
HoldMachine g_machine{};
/** Eased weight of the hold, its mode and handguard, read by the draws. */
std::atomic<float> g_holdW{0.0F};
std::atomic_bool g_holdAim{false};
std::atomic<float> g_holdSlideMin{0.0F};
std::atomic<float> g_holdSlideMax{0.0F};
std::atomic_bool g_grenadeHeld{false};
/** A squeeze away from the gun is a grenade (`ads mode button`) or just waits to take hold (default). */
std::atomic_bool g_gripGrenade{false};
/**
 * Until when (ms) the learning loops must treat the gun as two-handed: while the second hand turns
 * it, the right hand on the gun no longer sits the way the right controller alone would put it,
 * and a loop that learned from that would undo the hold or learn the hold as the grip.
 */
std::atomic<std::uint64_t> g_twoHandedUntil{0};
std::atomic<std::uint32_t> g_foreFrame{0xFFFFFFFFU};

/** The hold's line in the log; defined beside the state machine. Present thread. */
void report_hold() noexcept;

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

V3 to_v3(const std::array<float, 3>& a) noexcept {
    return V3{a[0], a[1], a[2]};
}

std::uint32_t pair_key(int a, int b) noexcept {
    if (a > b) {
        std::swap(a, b);
    }
    return (static_cast<std::uint32_t>(a) << 16) | static_cast<std::uint32_t>(b);
}

bool pair_joint(int a, int b, V3& out, double& mass) noexcept {
    const auto found = g_pairs.find(pair_key(a, b));
    if (found == g_pairs.end() || found->second.mass < kPairMin) {
        mass = found == g_pairs.end() ? 0.0 : found->second.mass;
        return false;
    }
    mass = found->second.mass;
    out = V3{static_cast<float>(found->second.p[0] / mass), static_cast<float>(found->second.p[1] / mass),
             static_cast<float>(found->second.p[2] / mass)};
    return true;
}

V3 centroid(int bone) noexcept {
    if (bone < 0 || static_cast<std::size_t>(bone) >= kMaxBones || g_mass[static_cast<std::size_t>(bone)] <= 0.0) {
        return {};
    }
    const auto& c = g_centroid[static_cast<std::size_t>(bone)];
    const double m = g_mass[static_cast<std::size_t>(bone)];
    return V3{static_cast<float>(c[0] / m), static_cast<float>(c[1] / m), static_cast<float>(c[2] / m)};
}

/** Joint j (0 shoulder, 1 elbow, 2 wrist) of a side as a (parent, child) bone pair. */
std::array<int, 2> joint_pair(const SideRig& s, int j) noexcept {
    switch (j) {
    case 0: return {s.clav, s.upper};
    case 1: return {s.upper, s.fore};
    default: return {s.fore, s.hand};
    }
}

/** Least-squares joint, pulled towards the overlap estimate and bounded around it. */
V3 solve_joint(const JointLs& ls, const V3& prior) noexcept {
    if (ls.frames < 1.0) {
        return prior;
    }
    auto a = ls.ata;
    std::array<double, 3> b = ls.atb;
    const std::array<double, 3> p{prior.x, prior.y, prior.z};
    for (std::size_t i = 0; i < 3; ++i) {
        a[i][i] += kPriorWeight;
        b[i] += kPriorWeight * p[i];
    }
    std::array<double, 3> x{};
    if (!solve3(a, b, x)) {
        return prior;
    }
    V3 j{static_cast<float>(x[0]), static_cast<float>(x[1]), static_cast<float>(x[2])};
    const V3 d = j - prior;
    const float n = length(d);
    if (!finite(j)) {
        return prior;
    }
    return n > kMaxJointShift ? prior + d * (kMaxJointShift / n) : j;
}

/**
 * Derives the rig from the accumulated meshes and the learned joints. Caller holds g_lock
 * exclusively. Leaves the rig invalid -- and the arms to the game -- unless every joint the
 * solver needs is backed by real skin on both sides.
 */
int g_lastFromSkeleton = 0;

void rebuild_rig() noexcept {
    Rig rig{};
    Labels labels = g_labels;
    // Which hand is which comes from the bind pose: the game's +Y is LEFT.
    if (centroid(labels.side[left].hand).y < centroid(labels.side[right].hand).y) {
        std::swap(labels.side[left], labels.side[right]);
    }
    bool ok = true;
    // The skeleton's joints in bind space (ARMS.md, solved on the Hunter by both methods to 0.1 mm; every
    // Guardian's arm pieces share this skeleton). A pair the meshes do not show -- the Warlock's gauntlets
    // stop at the elbow, the upper arm is the robe's sleeve -- starts from these, and the animation's
    // least squares (observe) then refines it to the character's own.
    static const std::array<std::array<V3, 3>, 2> kSkeletonJoints{{
        {{V3{-0.027F, 0.186F, 1.485F}, V3{0.077F, 0.324F, 1.274F}, V3{0.292F, 0.392F, 1.156F}}},
        {{V3{-0.027F, -0.186F, 1.485F}, V3{0.077F, -0.324F, 1.274F}, V3{0.292F, -0.392F, 1.156F}}},
    }};
    int fromSkeleton = 0;
    for (int s = 0; s < 2; ++s) {
        for (int j = 0; j < 3; ++j) {
            const auto pr = joint_pair(labels.side[static_cast<std::size_t>(s)], j);
            double mass = 0.0;
            V3 joint{};
            if (!pair_joint(pr[0], pr[1], joint, mass)) {
                // Which side is left comes from the hands' bind centroids, which a glove always gives.
                joint = kSkeletonJoints[static_cast<std::size_t>(s)][static_cast<std::size_t>(j)];
                ++fromSkeleton;
                if (g_mass[static_cast<std::size_t>(labels.side[static_cast<std::size_t>(s)].hand)] <= 0.0) {
                    ok = false;
                }
            }
            rig.prior[static_cast<std::size_t>(s)][static_cast<std::size_t>(j)] = joint;
        }
    }
    if (fromSkeleton > 0 && fromSkeleton != g_lastFromSkeleton) {
        reportf("ev=vr.arms rig joints_from_skeleton=%d (the meshes show no overlap there)", fromSkeleton);
    }
    g_lastFromSkeleton = fromSkeleton;
    if (!ok) {
        g_rig = rig;
        return;
    }
    for (std::size_t b = 0; b < kMaxBones; ++b) {
        rig.seg[b] = g_mass[b] > 0.0 ? segBody : segNone;
        rig.side[b] = -1;
    }
    const Params params = g_params;
    for (int s = 0; s < 2; ++s) {
        const SideRig& sr = labels.side[static_cast<std::size_t>(s)];
        auto mark = [&](int bone, Seg seg, float twist) {
            if (bone >= 0 && static_cast<std::size_t>(bone) < kMaxBones) {
                rig.seg[static_cast<std::size_t>(bone)] = seg;
                rig.side[static_cast<std::size_t>(bone)] = static_cast<std::int8_t>(s);
                rig.twist[static_cast<std::size_t>(bone)] = twist;
            }
        };
        mark(sr.upperTwist, segUpper, 0.0F);
        mark(sr.upper, segUpper, 0.0F);
        mark(sr.fore, segFore, params.twistFore);
        mark(sr.foreTwist, segForeTwist, params.twistWrist);
        mark(sr.hand, segHand, 0.0F);
        // The fingers: everything joined to the hand that is not on the way back up the arm.
        std::unordered_set<int> blocked{labels.chest, sr.clav, sr.upperTwist, sr.upper, sr.fore, sr.foreTwist};
        for (const SideRig& other : labels.side) {
            if (&other != &sr) {
                blocked.insert({other.clav, other.upperTwist, other.upper, other.fore, other.foreTwist, other.hand});
            }
        }
        std::vector<int> frontier{sr.hand};
        std::unordered_set<int> seen{sr.hand};
        while (!frontier.empty()) {
            const int bone = frontier.back();
            frontier.pop_back();
            for (const auto& [key, acc] : g_pairs) {
                if (acc.mass < kPairMin) {
                    continue;
                }
                const int a = static_cast<int>(key >> 16);
                const int b = static_cast<int>(key & 0xFFFF);
                const int next = a == bone ? b : (b == bone ? a : -1);
                if (next < 0 || blocked.count(next) != 0 || seen.count(next) != 0) {
                    continue;
                }
                seen.insert(next);
                frontier.push_back(next);
                mark(next, segHand, 0.0F);
                ++rig.fingers;
            }
        }
    }
    for (std::size_t s = 0; s < 2; ++s) {
        rig.s[s] = solve_joint(g_ls[s][0], rig.prior[s][0]);
        rig.e[s] = solve_joint(g_ls[s][1], rig.prior[s][1]);
        rig.w[s] = solve_joint(g_ls[s][2], rig.prior[s][2]);
        rig.palm[s] = centroid(labels.side[s].hand);
        rig.l1[s] = length(rig.e[s] - rig.s[s]);
        rig.l2[s] = length(rig.w[s] - rig.e[s]);
        const V3 n = normalize(cross(rig.e[s] - rig.s[s], rig.w[s] - rig.e[s]), V3{0, 0, 1});
        rig.upperBind[s] = frame_from(rig.e[s] - rig.s[s], n);
        rig.foreBind[s] = frame_from(rig.w[s] - rig.e[s], n);
        if (rig.l1[s] < 0.08F || rig.l1[s] > 0.6F || rig.l2[s] < 0.08F || rig.l2[s] > 0.6F) {
            ok = false;
        }
    }
    rig.shoulderMid = (rig.s[0] + rig.s[1]) * 0.5F;
    for (std::size_t b = 0; b < kMaxBones; ++b) {
        if (rig.seg[b] == segHand) {
            rig.fingerSpan = b + 1;
        }
    }
    // Left fingers to right fingers, by the bind pose's mirror symmetry: the bone whose centroid
    // sits closest to this one's reflection across the body's midplane (the game's Y = 0).
    for (std::size_t b = 0; b < kMaxBones; ++b) {
        rig.cen[b] = centroid(static_cast<int>(b));
        rig.mirrorOf[b] = -1;
    }
    for (std::size_t b = 0; b < kMaxBones; ++b) {
        if (rig.seg[b] != segHand || rig.side[b] != left || static_cast<int>(b) == labels.side[left].hand) {
            continue;
        }
        const V3 reflected{rig.cen[b].x, -rig.cen[b].y, rig.cen[b].z};
        float best = 0.02F;
        for (std::size_t o = 0; o < kMaxBones; ++o) {
            if (rig.seg[o] == segHand && rig.side[o] == right && static_cast<int>(o) != labels.side[right].hand) {
                const float d = length(rig.cen[o] - reflected);
                if (d < best) {
                    best = d;
                    rig.mirrorOf[b] = static_cast<int>(o);
                }
            }
        }
    }
    rig.valid = ok;
    const bool wasValid = g_rig.valid;
    g_rig = rig;
    g_labels = labels;
    if (rig.valid != wasValid) {
        reportf("ev=vr.arms rig valid=%d meshes=%zu vertices=%zu fingers=%zu "
                "L_s=%.3f,%.3f,%.3f L_e=%.3f,%.3f,%.3f L_w=%.3f,%.3f,%.3f l1=%.3f l2=%.3f "
                "R_s=%.3f,%.3f,%.3f l1=%.3f l2=%.3f",
                rig.valid ? 1 : 0, g_meshes.size(), g_vertices, rig.fingers, rig.s[0].x, rig.s[0].y, rig.s[0].z,
                rig.e[0].x, rig.e[0].y, rig.e[0].z, rig.w[0].x, rig.w[0].y, rig.w[0].z, rig.l1[0], rig.l2[0],
                rig.s[1].x, rig.s[1].y, rig.s[1].z, rig.l1[1], rig.l2[1]);
    }
}

// ---------------------------------------------------------------------------------------------
// The solve
// ---------------------------------------------------------------------------------------------

struct IkResult final {
    V3 s{};
    V3 e{};
    float k{1.0F};
    bool slid{};
};

/**
 * Two-bone IK. The shoulder stays put while the target is in reach; beyond it the arm may stretch
 * up to `kMax` (the game's arm is a few centimetres shorter than most players'), and past that the
 * shoulder slides towards the hand rather than the hand being left short of the controller -- the
 * hand on the controller is the one thing that must never be wrong.
 */
IkResult solve_two_bone(V3 s, const V3& w, float l1, float l2, const V3& pole, float kMax) noexcept {
    IkResult out{};
    V3 d = w - s;
    float dist = length(d);
    const float reach = l1 + l2;
    out.k = std::clamp(dist / (reach * 0.999F), 1.0F, kMax);
    const float a = l1 * out.k;
    const float b = l2 * out.k;
    if (dist > (a + b) * 0.999F) {
        s = w - normalize(d) * ((a + b) * 0.999F);
        d = w - s;
        dist = length(d);
        out.slid = true;
    }
    dist = std::max(dist, std::fabs(a - b) + 1e-3F);
    const V3 dh = normalize(d);
    const float ca = std::clamp((a * a + dist * dist - b * b) / (2.0F * a * dist), -1.0F, 1.0F);
    const float sa = std::sqrt(std::max(0.0F, 1.0F - ca * ca));
    V3 p = pole - dh * dot(pole, dh);
    p = normalize(p, normalize(cross(dh, V3{0, 0, 1}), V3{0, 1, 0}));
    out.s = s;
    out.e = s + dh * (a * ca) + p * (a * sa);
    return out;
}

Affine controller_frame(const xr::HandPose& hand, const xr::HeadPose& head) noexcept {
    // Columns (forward, LEFT, up): a proper rotation in the game's right-handed basis, which
    // (forward, right, up) is not.
    const V3 fwd = to_v3(hand.forward);
    const V3 up = to_v3(hand.up);
    const V3 lft = -to_v3(hand.right);
    return Affine{orthonormalize(M3{{fwd, lft, up}}), to_v3(hand.palm) - to_v3(head.offset)};
}

/** Mirror across the plane whose normal is the frame's second axis: diag(1, -1, 1). */
Affine mirror(const Affine& a) noexcept {
    auto flip = [](const V3& v) { return V3{v.x, -v.y, v.z}; };
    // S * M * S for the rotation: flip each column's y, then negate the middle column.
    M3 m{{flip(a.m.c[0]), -flip(a.m.c[1]), flip(a.m.c[2])}};
    return Affine{m, flip(a.t)};
}

/**
 * Blends two bone transforms about the bone's own bind centroid. Blending palette matrices about
 * the model origin -- the naive way -- swings a fingertip through the palm and tears the skin
 * (dishonored-vr paid for that once); about its own centroid each bone turns in place.
 */
Affine blend_about(const Affine& a, const Affine& b, float t, const V3& centroid) noexcept {
    const M3 r = blend(a.m, b.m, t);
    const V3 pa = a * centroid;
    const V3 pb = b * centroid;
    const V3 p = pa + (pb - pa) * t;
    return Affine{r, p - r * centroid};
}

/**
 * One left finger bone, relative to the left hand frame: open (the bind pose, the relaxed hand
 * the model was built in) blended towards closed (the right hand's grip on the gun, mirrored --
 * a hand around a handle, which is what a hand around a Touch controller is) by how hard the left
 * grip is squeezed.
 */
Affine left_finger(const Rig& rig, const float* bones, std::size_t boneCount, const Affine& rightHand, std::size_t b,
                   float grip) noexcept {
    // Open: bind pose relative to the hand frame [I | wrist].
    const Affine open{M3{}, -rig.w[left]};
    const int mirrored = rig.mirrorOf[b];
    if (mirrored < 0 || static_cast<std::size_t>(mirrored) >= boneCount || grip <= 0.001F) {
        return open;
    }
    const Affine rightRel = inverse_rigid(rightHand) * read_rows(bones + static_cast<std::size_t>(mirrored) * 12);
    const Affine closed = mirror(rightRel);
    return blend_about(open, closed, grip, rig.cen[b]);
}

/** Everything one rewrite needs from the outside world, sampled once. */
struct Inputs final {
    xr::HeadPose head{};
    xr::HandPose left{};
    xr::HandPose right{};
    float bodyYaw{};
};

bool sample(Inputs& in) noexcept {
    // The sample of the frame the GPU is drawing (the frame ledger, named by the world's view): the
    // palettes are camera-relative, so they must be posed against the very camera of their frame.
    (void)xr::drawn_sample(in.head, in.left, in.right);
    if (!in.head.valid) {
        return false;
    }
    if (frames::tracing()) {
        const float v[6] = {in.head.offset[0], in.head.offset[1], in.head.offset[2], 0.0F, 0.0F, 0.0F};
        frames::note(frames::Kind::arms, in.head.frame, v);
    }
    in.bodyYaw = g_bodyYaw.load(std::memory_order_relaxed);
    return true;
}

/** Horizontal heading of the head, robust to looking straight up or down. */
float heading_of(const xr::HeadPose& head) noexcept {
    const V3 f = to_v3(head.forward);
    const V3 u = to_v3(head.up);
    V3 h{f.x, f.y, 0.0F};
    if (length(h) < 0.3F) {
        // Looking down, the top of the head points forward; looking up, backward.
        h = f.z < 0.0F ? V3{u.x, u.y, 0.0F} : V3{-u.x, -u.y, 0.0F};
    }
    return std::atan2(h.y, h.x);
}

M3 yaw_matrix(float yaw) noexcept {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return M3{{V3{c, s, 0}, V3{-s, c, 0}, V3{0, 0, 1}}};
}

/** Angle of the rotation taking `b` to `a`, radians. */
float angle_between(const M3& a, const M3& b) noexcept {
    const M3 d = a * transpose(b);
    return std::acos(std::clamp((d.at(0, 0) + d.at(1, 1) + d.at(2, 2) - 1.0F) * 0.5F, -1.0F, 1.0F));
}

/** How long after a swap, melee or super nothing about the foregrip may be confirmed, ms. */
constexpr std::uint64_t kDoubtMs = 800;

bool two_handed(std::uint64_t nowMs) noexcept {
    return nowMs < g_twoHandedUntil.load(std::memory_order_relaxed);
}

/**
 * Learns where the left hand holds the weapon in hand. Once a frame, from a palette that carries
 * every finger, under the exclusive lock.
 *
 * Three ways a measurement is treated:
 *   - close to what is already learned: blended in slowly, the fingers recaptured, and -- once the
 *     hands have been still a quarter of a second with the weapon seated -- confirmed fresh again
 *     after anything that put it in doubt;
 *   - far from it, but still for a full second and where a foregrip can be: a different weapon,
 *     adopted outright. The first one only needs 0.3 s;
 *   - anything else (a reload, an inspect, sprinting): ignored.
 *
 * The foregrip in the right CONTROLLER's frame, which is what the hold and the weapon module steer
 * by, is measured separately on the drawn gun, and only while the controller is still and the gun
 * held one-handed: a moving controller puts the render latency into the measurement, and a gun
 * turned by the second hand no longer sits where the controller alone puts it.
 */
void learn_foregrip(const Rig& rig, int handL, const float* bones, std::size_t boneCount,
                    const std::array<Affine, 2>& animated, const Affine& ctrlR, bool rightValid,
                    std::uint64_t nowMs) noexcept {
    Foregrip& f = g_fore;
    const V3 palmOffsetL = rig.palm[left] - rig.w[left];
    const Affine meas = inverse_rigid(animated[right]) * animated[left];
    const bool still = f.prevValid && length(meas.t - f.prev.t) < 0.004F && angle_between(meas.m, f.prev.m) < 0.035F;
    f.prev = meas;
    f.prevValid = true;
    if (!still) {
        f.stillSince = 0;
    } else if (f.stillSince == 0) {
        f.stillSince = nowMs;
    }
    const std::uint64_t stillMs = f.stillSince == 0 ? 0 : nowMs - f.stillSince;
    const bool seated = g_seatError < 0.03F && (g_seatAngle < 0.09F || !g_rotSeat.load(std::memory_order_relaxed));
    // Where a foregrip can be, in the controller's frame: ahead of the right palm at most an arm's
    // length, near the barrel's vertical plane, neither over the head nor at the belt.
    const V3 fcCandidate = g_grip.valid ? g_grip.rel * (meas * palmOffsetL) : V3{};
    const bool plausible = g_grip.valid && fcCandidate.x > -0.15F && fcCandidate.x < 0.75F
                           && std::fabs(fcCandidate.y) < 0.25F && fcCandidate.z > -0.30F && fcCandidate.z < 0.25F;
    const bool matches = f.valid && length(meas.t - f.rel.t) < 0.02F && angle_between(meas.m, f.rel.m) < 0.17F;
    bool capture = false;
    if (matches) {
        f.rel.t = f.rel.t + (meas.t - f.rel.t) * 0.05F;
        f.rel.m = orthonormalize(blend(f.rel.m, meas.m, 0.05F));
        capture = true;
        if (!f.fresh && stillMs > 250 && seated && nowMs - f.doubtSince > kDoubtMs) {
            f.fresh = true;
            report("ev=vr.arms foregrip event=confirmed");
        }
    } else if (plausible && stillMs > (f.valid ? 1000U : 300U)) {
        f.rel = meas;
        f.valid = true;
        f.fresh = seated && nowMs - f.doubtSince > kDoubtMs;
        f.fcValid = false;
        ++f.generation;
        capture = true;
        reportf("ev=vr.arms foregrip event=learned rel_t=%.3f,%.3f,%.3f approx_fc=%.3f,%.3f,%.3f seated=%d", meas.t.x,
                meas.t.y, meas.t.z, fcCandidate.x, fcCandidate.y, fcCandidate.z, seated ? 1 : 0);
    }
    if (capture) {
        for (std::size_t b = 0; b < boneCount && b < kMaxBones; ++b) {
            if (rig.seg[b] == segHand && rig.side[b] == left && static_cast<int>(b) != handL) {
                f.fingers[b] = inverse_rigid(animated[left]) * read_rows(bones + b * 12);
                f.fingerSet[b] = true;
            }
        }
    }
    // Only from a gun that is confirmed and seated: during a swap the right hand carries the old gun
    // away from the controller, and a foregrip measured against the controller then is nonsense.
    if (f.valid && f.fresh && seated && rightValid && g_ctrlSteady && !two_handed(nowMs)) {
        const V3 palmOnGun = animated[right] * (f.rel * palmOffsetL);
        const V3 m = transpose(ctrlR.m) * (palmOnGun - ctrlR.t);
        if (!f.fcValid) {
            f.fc = m;
            f.fcValid = true;
            reportf("ev=vr.arms foregrip event=measured fc=%.3f,%.3f,%.3f lever=%.3f", m.x, m.y, m.z, length(m));
        } else {
            f.fc = f.fc + (m - f.fc) * 0.1F;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------------------------

void add_mesh(std::uintptr_t key, std::size_t count, const float* positions, const float* weights,
              const std::uint8_t* indices) noexcept {
    if (positions == nullptr || weights == nullptr || indices == nullptr || count == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (!g_meshes.insert(key).second) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    for (std::size_t v = 0; v < count; ++v) {
        const float* p = positions + v * 3;
        const float* w = weights + v * 4;
        const std::uint8_t* ix = indices + v * 4;
        for (int k = 0; k < 4; ++k) {
            if (w[k] <= 0.0F || ix[k] >= kMaxBones) {
                continue;
            }
            g_mass[ix[k]] += w[k];
            for (std::size_t a = 0; a < 3; ++a) {
                g_centroid[ix[k]][a] += static_cast<double>(w[k]) * p[a];
            }
            for (int l = k + 1; l < 4; ++l) {
                if (w[l] <= 0.0F || ix[l] >= kMaxBones || ix[l] == ix[k]) {
                    continue;
                }
                const double ww = static_cast<double>(w[k]) * w[l];
                PairAcc& acc = g_pairs[pair_key(ix[k], ix[l])];
                acc.mass += ww;
                for (std::size_t a = 0; a < 3; ++a) {
                    acc.p[a] += ww * p[a];
                }
            }
        }
    }
    g_vertices += count;
    rebuild_rig();
    ReleaseSRWLockExclusive(&g_lock);
    reportf("ev=vr.arms mesh key=%llX vertices=%zu total_meshes=%zu rig_valid=%d",
            static_cast<unsigned long long>(key), count, g_meshes.size(), g_rig.valid ? 1 : 0);
}

void observe(const float* bones, std::size_t boneCount) noexcept {
    const std::uint32_t frame = g_frame.load(std::memory_order_relaxed);
    if (bones == nullptr || g_observedFrame.exchange(frame, std::memory_order_relaxed) == frame) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_rig.prior[0][0].z == 0.0F) {
        // No meshes yet, so no idea which bones meet where: nothing to learn against.
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    for (std::size_t s = 0; s < 2; ++s) {
        for (int j = 0; j < 3; ++j) {
            const auto pr = joint_pair(g_labels.side[s], j);
            if (pr[0] < 0 || pr[1] < 0 || static_cast<std::size_t>(std::max(pr[0], pr[1])) >= boneCount) {
                continue;
            }
            const Affine p = read_rows(bones + static_cast<std::size_t>(pr[0]) * 12);
            const Affine c = read_rows(bones + static_cast<std::size_t>(pr[1]) * 12);
            JointLs& ls = g_ls[s][static_cast<std::size_t>(j)];
            for (auto& row : ls.ata) {
                for (double& v : row) {
                    v *= kForget;
                }
            }
            for (double& v : ls.atb) {
                v *= kForget;
            }
            ls.frames = ls.frames * kForget + 1.0;
            const V3 e = c.t - p.t;
            for (int r = 0; r < 3; ++r) {
                for (int k = 0; k < 3; ++k) {
                    double sum = 0.0;
                    for (int i = 0; i < 3; ++i) {
                        const double dik = p.m.at(i, r) - c.m.at(i, r);
                        const double djk = p.m.at(i, k) - c.m.at(i, k);
                        sum += dik * djk;
                    }
                    ls.ata[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)] += sum;
                }
                double sb = 0.0;
                for (int i = 0; i < 3; ++i) {
                    sb += (p.m.at(i, r) - c.m.at(i, r)) * e[i];
                }
                ls.atb[static_cast<std::size_t>(r)] += sb;
            }
        }
    }
    if ((++g_observed % 30) == 0) {
        rebuild_rig();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool ready() noexcept {
    return g_enabled.load(std::memory_order_relaxed) && g_rig.valid && enabled() && xr::active()
           && g_bodyYawValid.load(std::memory_order_relaxed);
}

namespace {

/**
 * The right hand's fingers round a gun's grip, relative to the hand frame: the closed fist a free hand
 * makes in third person (the game's own idle hand there is open). Under g_lock.
 */
std::array<Affine, kMaxBones> g_fist{};
std::array<bool, kMaxBones> g_fistSet{};
bool g_fistValid = false;

} // namespace

bool rewrite(const float* bones, std::size_t boneCount, float* out) noexcept {
    if (!ready() || bones == nullptr || out == nullptr) {
        return false;
    }
    Inputs in{};
    if (!sample(in)) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const Rig& rig = g_rig;
    const Labels& labels = g_labels;
    const Params params = g_params;
    const int handL = labels.side[left].hand;
    const int handR = labels.side[right].hand;
    if (!rig.valid || static_cast<std::size_t>(std::max(handL, handR)) >= boneCount) {
        ReleaseSRWLockShared(&g_lock);
        return false;
    }

    // The body: a frame hung from the neck and turned to the body's heading. The neck hangs from the
    // middle of the head, not from the camera: in alternating-eye stereo the camera is a different eye
    // every frame, and a body hung from it moved 3 cm sideways from one eye's image to the other's --
    // the hands stayed on the controllers, so the arms changed shape between the eyes (measured: a
    // 0.2-0.6 deg vertical disparity on the left forearm, impossible for any real object).
    const V3 f = to_v3(in.head.forward);
    const V3 u = to_v3(in.head.up);
    const V3 eyeL = to_v3(in.head.eyeOffsets[0]);
    const V3 eyeR = to_v3(in.head.eyeOffsets[1]);
    const bool eyesKnown = length(eyeR - eyeL) > 1e-4F;
    const V3 eyeToCentre = eyesKnown ? (eyeL + eyeR) * 0.5F - to_v3(in.head.offset) : V3{};
    const V3 neck = eyeToCentre - (f * params.neckFwd) - u * params.neckUp;
    const M3 yaw = yaw_matrix(in.bodyYaw);
    const V3 shoulderMid = neck + yaw * V3{params.shoulderFwd, 0.0F, params.shoulderUp};
    const Affine body{yaw, shoulderMid - yaw * rig.shoulderMid};

    // The hands as the game animated them: a frame at the bind wrist joint, axes of the bone.
    std::array<Affine, 2> animated{};
    std::array<V3, 2> palmDrawn{};
    for (std::size_t s = 0; s < 2; ++s) {
        const Affine m = read_rows(bones + static_cast<std::size_t>(labels.side[s].hand) * 12);
        animated[s] = Affine{orthonormalize(m.m), m * rig.w[s]};
        palmDrawn[s] = m * rig.palm[s];
    }
    const V3 palmOffsetL = rig.palm[left] - rig.w[left];

    // Targets. Right: where the game put it, on the gun. Left: the left controller, gripping the
    // way the right hand grips the gun, mirrored.
    std::array<Affine, 2> target{animated[0], animated[1]};
    const Affine ctrlL = controller_frame(in.left, in.head);
    const Affine ctrlR = controller_frame(in.right, in.head);
    const std::uint32_t frame = g_frame.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&g_lock);

    // The ADS learner reads the hand exactly as the game animated it, before anything here moves it.
    Affine gunRoot{};
    std::uint32_t gunBones = 0;
    const bool gunSeen = gpu::weapon_root(animated[right].t, gunRoot, &gunBones);
    ads::observe(animated[right], gunSeen ? &gunRoot : nullptr, gunBones, ctrlR);

    const std::uint64_t tick = GetTickCount64();
    // While the second hand turns the gun, the right hand on it is no longer where the right
    // controller alone would put it: nothing below may learn from it, or the loops would undo the
    // hold (the rotational seat) or learn the hold as the grip (the left hand's orientation).
    const bool twoHanded = two_handed(tick);
    // In ADS the game re-grips the hand on the gun (about 2 degrees on the hand cannon) while the ADS
    // compensation holds the GUN where the hip pose has it -- the gun is what the sight is on and what
    // the controller stands for. Nothing that learns from the drawn hand may learn from that: the
    // seat would turn the gun to chase the re-grip, the grip and the foregrip would learn it.
    // Only while the compensation holds the gun: a weapon not yet learned is left to the seat, which
    // drags it back into the hand as before.
    math::Affine unusedDelta{};
    const bool inAds = ads::compensation(unusedDelta);
    // And only while the game's animation is at rest in the hand. Everything below learns a STATIC
    // property of the weapon's hold; a recoil, a reload, a swap or a walk cycle is animation, and a
    // loop that chases it turns the gun in the hand on its own -- measured before this gate: the
    // hand cannon pointed 3.5-4 deg low after every shot (and the next round went there), and a
    // reload swung the sight up to 70 deg off the controller.
    const bool atRest = ads::animation_at_rest();
    if (in.right.valid && !inAds) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_grip.frame != frame) {
            g_grip.frame = frame;
            const Affine now = inverse_rigid(ctrlR) * animated[right];
            if (!g_grip.valid) {
                g_grip.rel = now;
                g_grip.valid = true;
            } else if (twoHanded) {
                g_grip.strikeSince = 0;
            } else if (atRest) {
                // Learned slowly and only from a steady grip: recoil and reload animations move
                // the right hand on the gun, and a left hand that echoed them would look wrong.
                const float jump = length(now.t - g_grip.rel.t);
                const M3 d = now.m * transpose(g_grip.rel.m);
                const float cosAngle = std::clamp((d.at(0, 0) + d.at(1, 1) + d.at(2, 2) - 1.0F) * 0.5F, -1.0F, 1.0F);
                if (jump < 0.06F && cosAngle > 0.866F) {
                    g_grip.rel.t = g_grip.rel.t + (now.t - g_grip.rel.t) * 0.05F;
                    g_grip.rel.m = blend(g_grip.rel.m, now.m, 0.05F);
                    g_grip.strikeSince = 0;
                } else if (g_grip.strikeSince == 0) {
                    g_grip.strikeSince = GetTickCount64();
                } else if (GetTickCount64() - g_grip.strikeSince > 800) {
                    // A different grip held for most of a second (800 ms) is not an animation flourish:
                    // the first sample was taken before the weapon sat on the controller, or the
                    // weapon changed. Re-seat rather than stay wrong for the rest of the session.
                    g_grip.rel = now;
                    g_grip.strikeSince = 0;
                }
            }
            // Self-seating: how far the drawn right palm is from the controller's grip pose, in
            // the controller's basis, back to the weapon module. Only while the controller is
            // steady: a moving hand puts its own latency into the measurement.
            // Measured in the room, not relative to the camera: in alternating-eye stereo the camera
            // is a different eye every frame, and a camera-relative controller jumps by the whole eye
            // distance each frame -- which read as a hand never still, and froze every seat.
            const V3 ctrlRoom = to_v3(in.right.palm);
            const float moved = length(ctrlRoom - g_lastCtrlR);
            g_lastCtrlR = ctrlRoom;
            g_ctrlSteady = moved < 0.004F;
            if (moved < 0.004F && atRest) {
                V3 e = transpose(ctrlR.m) * (palmDrawn[right] - ctrlR.t);
                if (twoHanded) {
                    // The weapon module applies this correction in the basis it hands the engine,
                    // which the second hand has turned. Measure it in that basis too -- the drawn
                    // right hand's, taken back through the grip it holds the gun with -- or the
                    // integrator would be steering through a rotated gain.
                    e = g_grip.rel.m * (transpose(animated[right].m) * (palmDrawn[right] - ctrlR.t));
                }
                weapon::seat_error(e.x, -e.y, e.z);
                g_seatError = length(e);
                V3 sightDir{};
                const ads::SightKnown sight = ads::sight_direction(sightDir);
                // The rotational seat. With the viewmodel turned by Q in the controller frame, the
                // hand's rotation there is Rrel = Q V (V: the weapon's own authored hold), so the Q
                // that makes it canonical is canon * Rrel^T * Q.
                const M3 relNow = transpose(ctrlR.m) * animated[right].m;
                if (twoHanded) {
                    g_canonSteadySince = 0;
                } else if (!g_canonSet) {
                    const std::uint64_t nowMs = GetTickCount64();
                    if (g_canonSteadySince == 0) {
                        g_canonSteadySince = nowMs;
                    } else if (nowMs - g_canonSteadySince > 2000) {
                        g_canon = transpose(g_seatQ) * relNow;
                        g_canonSet = true;
                        const auto q = quat_of(g_canon);
                        reportf("ev=vr.arms canon learned q=%.5f,%.5f,%.5f,%.5f", q[0], q[1], q[2], q[3]);
                    }
                } else if (g_rotSeat.load(std::memory_order_relaxed) && g_seatError < 0.15F
                           && sight != ads::SightKnown::pending) {
                    // Only while the palm is already near the controller: that is the weapon
                    // actually being held. Turning the gun on the strength of a hand that is
                    // somewhere else (a draw animation, the first frames) is how a seat loop
                    // talks itself into a wrong pose. And not while a learned weapon is about to be
                    // identified: its target is about to change, so hold rather than wander.
                    M3 qTarget = orthonormalize(g_canon * transpose(relNow) * g_seatQ);
                    if (sight == ads::SightKnown::known) {
                        // The sight along the aim: qTarget maps the gun frame to the controller's, so
                        // the swing that takes the sight there is applied on the left, in the
                        // controller's frame. Its fixed point is a constant per weapon (the hand's
                        // relation to the controller cancels out of g_canon * relNow^T * Q).
                        const float pitch = g_aimPitch.load(std::memory_order_relaxed);
                        const V3 want{std::cos(pitch), 0.0F, std::sin(pitch)};
                        const V3 v = normalize(qTarget * sightDir);
                        const V3 axis = cross(v, want);
                        const float sine = length(axis);
                        if (sine > 1e-6F) {
                            qTarget = orthonormalize(axis_angle(axis * (1.0F / sine), std::atan2(sine, dot(v, want))) * qTarget);
                        }
                    }
                    const M3 d = qTarget * transpose(g_seatQ);
                    const float c = std::clamp((d.at(0, 0) + d.at(1, 1) + d.at(2, 2) - 1.0F) * 0.5F, -1.0F, 1.0F);
                    g_seatAngle = std::acos(c);
                    // Big disagreements are transitions (a swap, an inspect); seat only on holds.
                    if (g_seatAngle < 1.05F) {
                        g_seatQ = orthonormalize(blend(g_seatQ, qTarget, 0.04F));
                        weapon::set_seat_rotation(quat_of(g_seatQ));
                    }
                }
            } else {
                g_canonSteadySince = 0;
            }
            // Escape hatch: a palm that stays far from the controller for two seconds while the
            // controller is still means the corrections themselves are wrong. Start over.
            if (moved < 0.004F && atRest && g_seatError > 0.25F && !twoHanded) {
                if (g_seatBadSince == 0) {
                    g_seatBadSince = GetTickCount64();
                } else if (GetTickCount64() - g_seatBadSince > 2000) {
                    g_seatQ = M3{};
                    weapon::seat_reset();
                    g_seatBadSince = 0;
                    report("ev=vr.arms seat reset reason=stuck");
                }
            } else {
                g_seatBadSince = 0;
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    // A snapshot of the right hand's hold for the left (dual wield), from a palette with every finger,
    // at rest, one-handed, with the grip learned.
    const std::uintptr_t snapKey = g_snapshotWanted.load(std::memory_order_relaxed);
    if (snapKey != 0 && !inAds && atRest && !twoHanded && boneCount >= rig.fingerSpan && g_grip.valid && in.right.valid) {
        AcquireSRWLockExclusive(&g_lock);
        GripSnapshot* slot = nullptr;
        for (GripSnapshot& s : g_snapshots) {
            if (s.key == snapKey || (slot == nullptr && s.key == 0)) {
                slot = &s;
            }
        }
        if (slot == nullptr) {
            slot = &g_snapshots[0];
        }
        *slot = GripSnapshot{};
        slot->key = snapKey;
        slot->hand = g_grip.rel;
        for (std::size_t b = 0; b < boneCount && b < kMaxBones; ++b) {
            if (rig.seg[b] == segHand && rig.side[b] == right && static_cast<int>(b) != handR) {
                slot->finger[b] = inverse_rigid(animated[right]) * read_rows(bones + b * 12);
                slot->fingerSet[b] = true;
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        g_snapshotWanted.store(0, std::memory_order_relaxed);
        reportf("ev=vr.arms grip_snapshot key=%llX hand_t=%.3f,%.3f,%.3f", static_cast<unsigned long long>(snapKey),
                slot->hand.t.x, slot->hand.t.y, slot->hand.t.z);
    }
    // The fist, for third person: the fingers round the gun's grip, at rest and one-handed.
    if (!inAds && atRest && !twoHanded && boneCount >= rig.fingerSpan && in.right.valid && (frame % 30) == 0) {
        AcquireSRWLockExclusive(&g_lock);
        for (std::size_t b = 0; b < boneCount && b < kMaxBones; ++b) {
            if (rig.seg[b] == segHand && rig.side[b] == right && static_cast<int>(b) != handR) {
                g_fist[b] = inverse_rigid(animated[right]) * read_rows(bones + b * 12);
                g_fistSet[b] = true;
            }
        }
        g_fistValid = true;
        ReleaseSRWLockExclusive(&g_lock);
    }
    // The foregrip, from the first palette of the frame that carries every finger (the gloves; the
    // gauntlet layers stop at bone 52).
    if (!inAds && boneCount >= rig.fingerSpan && g_foreFrame.exchange(frame, std::memory_order_relaxed) != frame) {
        AcquireSRWLockExclusive(&g_lock);
        learn_foregrip(g_rig, handL, bones, boneCount, animated, ctrlR, in.right.valid, tick);
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockShared(&g_lock);
    const bool leftDriven = in.left.valid && g_grip.valid;
    const float grip = std::clamp(xr::input_state().gripL, 0.0F, 1.0F);
    if (leftDriven) {
        // The grip's ORIENTATION is the right hand's, mirrored; its POSITION is the controller's
        // grip pose, which by definition is the palm's centre -- so the hand is placed to put its
        // palm there. Learning the position from the right hand as well would tie the left hand to
        // wherever a particular weapon happens to put the right one.
        const M3 r = ctrlL.m * mirror(g_grip.rel).m;
        target[left] = Affine{r, ctrlL.t - r * palmOffsetL};
    }

    // Dual wield: the left hand holds its own weapon, as the right hand held that weapon, mirrored, and
    // moves with the gun's kick and reload tilt.
    std::uintptr_t dualKey = 0;
    Affine dualMotion{};
    const GripSnapshot* dualGrip = nullptr;
    if (leftDriven && dual::left_hold(dualKey, dualMotion)) {
        for (const GripSnapshot& s : g_snapshots) {
            if (s.key == dualKey && dualKey != 0) {
                dualGrip = &s;
            }
        }
        if (dualGrip != nullptr) {
            target[left] = ctrlL * dualMotion * mirror(dualGrip->hand);
        }
    }

    // The hold: the left hand on the weapon's handguard, blended in from the controller by the
    // hold's weight. Everything about WHERE comes from this palette -- the learned foregrip carried
    // by the drawn right hand -- so the hand lands on the gun exactly as drawn, whatever frame the
    // weapon's orientation was computed in. Along the handguard it slides to the point nearest
    // the controller, which, with the barrel steered through the left palm, is the palm itself.
    const float holdW = leftDriven && g_fore.valid && dualGrip == nullptr ? g_holdW.load(std::memory_order_relaxed) : 0.0F;
    const bool holdAim = g_holdAim.load(std::memory_order_relaxed);
    V3 snapPalm{};
    V3 guardA{};
    V3 guardB{};
    float slide = 0.0F;
    float lineError = 0.0F;
    if (holdW > 0.0F) {
        const Affine foreFrame = animated[right] * g_fore.rel;
        const V3 palmOnGun = foreFrame * palmOffsetL;
        // The controller's forward as the drawn gun carries it: the hand's rotation taken back
        // through the grip it holds the gun with. The rotational seat makes that the barrel.
        const V3 axis = normalize(animated[right].m * (transpose(g_grip.rel.m) * V3{1.0F, 0.0F, 0.0F}));
        const float sMin = holdAim ? g_holdSlideMin.load(std::memory_order_relaxed) : 0.0F;
        const float sMax = holdAim ? g_holdSlideMax.load(std::memory_order_relaxed) : 0.0F;
        const V3 toCtrl = ctrlL.t - palmOnGun;
        slide = std::clamp(dot(toCtrl, axis), sMin, sMax);
        lineError = length(toCtrl - axis * dot(toCtrl, axis));
        const Affine snap{foreFrame.m, foreFrame.t + axis * slide};
        target[left] = blend_about(target[left], snap, holdW, palmOffsetL);
        snapPalm = palmOnGun + axis * slide;
        guardA = palmOnGun + axis * sMin;
        guardB = palmOnGun + axis * sMax;
    }

    // Solve both arms.
    std::array<Affine, 2> upperT{};
    std::array<Affine, 2> foreT{};
    std::array<Affine, 2> handT{};
    std::array<V3, 2> foreAxis{};
    std::array<V3, 2> elbowT{};
    std::array<float, 2> twist{};
    std::array<float, 2> stretch{};
    std::array<IkResult, 2> ik{};
    for (std::size_t s = 0; s < 2; ++s) {
        const V3 outward = V3{params.pole.x, s == left ? params.pole.y : -params.pole.y, params.pole.z};
        const V3 pole = yaw * outward;
        const V3 shoulder = body * rig.s[s];
        const V3 wrist = target[s].t;
        ik[s] = solve_two_bone(shoulder, wrist, rig.l1[s], rig.l2[s], pole, params.stretchMax);
        const V3 sT = ik[s].s;
        const V3 eT = ik[s].e;
        // The hinge normal, from the plane the IK bent the arm in: p x d, which is what
        // (E - S) x (W - E) gives for any bent arm, and stays defined when the arm is straight.
        const V3 dh = normalize(wrist - sT);
        V3 p = pole - dh * dot(pole, dh);
        p = normalize(p, normalize(cross(dh, V3{0, 0, 1}), V3{0, 1, 0}));
        const V3 n = normalize(cross(p, dh), V3{0, 0, 1});
        const M3 upperRot = frame_from(eT - sT, n) * transpose(rig.upperBind[s]);
        upperT[s] = Affine{upperRot, sT - upperRot * rig.s[s]};
        if (ik[s].k > 1.0001F) {
            upperT[s] = stretch_about(sT, normalize(eT - sT), ik[s].k) * upperT[s];
        }
        const M3 foreRot = frame_from(wrist - eT, n) * transpose(rig.foreBind[s]);
        foreT[s] = Affine{foreRot, eT - foreRot * rig.e[s]};
        foreAxis[s] = normalize(wrist - eT);
        elbowT[s] = eT;
        // The hand, as a bone transform: target * F_bind^-1 with F_bind = [I | wrist_bind].
        handT[s] = Affine{target[s].m, target[s].t - target[s].m * rig.w[s]};
        // How far the hand is rolled about the forearm relative to the untwisted forearm.
        twist[s] = wrap(twist_about(target[s].m * transpose(foreRot), foreAxis[s]));
        stretch[s] = ik[s].k;
    }

    // Write every bone.
    for (std::size_t b = 0; b < boneCount && b < kMaxBones; ++b) {
        const float* in4 = bones + b * 12;
        float* out4 = out + b * 12;
        const auto s = static_cast<std::size_t>(rig.side[b] < 0 ? 0 : rig.side[b]);
        Affine m{};
        switch (rig.seg[b]) {
        case segBody: m = body; break;
        case segUpper: m = upperT[s]; break;
        case segFore:
        case segForeTwist: {
            const M3 r = axis_angle(foreAxis[s], twist[s] * rig.twist[b]);
            m = rotate_about(r, elbowT[s]) * foreT[s];
            if (stretch[s] > 1.0001F) {
                m = stretch_about(elbowT[s], foreAxis[s], stretch[s]) * m;
            }
            break;
        }
        case segHand:
            if (s == left && leftDriven && static_cast<int>(b) != labels.side[left].hand) {
                const int mirrored = rig.mirrorOf[b];
                if (dualGrip != nullptr && mirrored >= 0 && mirrored < static_cast<int>(kMaxBones)
                    && dualGrip->fingerSet[static_cast<std::size_t>(mirrored)]) {
                    // Closed round the left weapon's grip: the right hand's fingers on it, mirrored.
                    m = target[left] * mirror(dualGrip->finger[static_cast<std::size_t>(mirrored)]);
                    break;
                }
                Affine rel = left_finger(rig, bones, boneCount, animated[right], b, grip);
                if (holdW > 0.0F && g_fore.fingerSet[b]) {
                    // Around the handguard: the fingers the game wrapped round this weapon's
                    // foregrip, captured with it, so a reload's fingers never leak in.
                    rel = blend_about(rel, g_fore.fingers[b], holdW, rig.cen[b]);
                }
                m = target[left] * rel;
                break;
            }
            // Carried rigidly with the hand: target * animated^-1 * game's own bone. Identity for
            // the right hand, which is why it stays exactly on the gun.
            m = target[s] * inverse_rigid(animated[s]) * read_rows(in4);
            break;
        default: std::memcpy(out4, in4, sizeof(float) * 12); continue;
        }
        write_rows(m, out4);
    }

    // What actually happened, for the overlay and the report: the wrist as the rewritten forearm
    // carries it, against the wrist the hand wants.
    if (g_overlay.load(std::memory_order_relaxed) || (frame % kReportPeriod) == 0) {
        Overlay o{};
        o.valid = true;
        for (std::size_t s = 0; s < 2; ++s) {
            o.s[s] = ik[s].s;
            o.e[s] = ik[s].e;
            o.w[s] = target[s].t;
            o.hand[s] = target[s];
            o.stretch[s] = ik[s].k;
            o.twist[s] = twist[s];
            o.slid[s] = ik[s].slid;
            Affine fore = rotate_about(axis_angle(foreAxis[s], twist[s] * params.twistFore), elbowT[s]) * foreT[s];
            if (stretch[s] > 1.0001F) {
                fore = stretch_about(elbowT[s], foreAxis[s], stretch[s]) * fore;
            }
            o.wristError[s] = length(fore * rig.w[s] - target[s].t);
        }
        o.controller[left] = ctrlL;
        o.controller[right] = ctrlR;
        o.holdW = holdW;
        o.holdAim = holdAim;
        o.foreValid = g_fore.valid;
        o.ctrlPalmL = ctrlL.t;
        if (holdW > 0.0F) {
            o.guardA = guardA;
            o.guardB = guardB;
            o.snapPalm = snapPalm;
            o.lineError = lineError;
            o.handError = length(snapPalm - ctrlL.t);
            o.slide = slide;
        } else if (g_fore.valid) {
            // Not held: where the handguard is, so the overlay shows what there is to take hold of.
            const V3 palmOnGun = (animated[right] * g_fore.rel) * palmOffsetL;
            const V3 axis = normalize(animated[right].m * (transpose(g_grip.rel.m) * V3{1.0F, 0.0F, 0.0F}));
            o.guardA = palmOnGun - axis * g_hold.slideBack;
            o.guardB = palmOnGun + axis * g_hold.slideFwd;
            o.snapPalm = palmOnGun;
        }
        ReleaseSRWLockShared(&g_lock);
        AcquireSRWLockExclusive(&g_lock);
        g_overlayData = o;
        ReleaseSRWLockExclusive(&g_lock);
    } else {
        ReleaseSRWLockShared(&g_lock);
    }
    g_rewrites.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Third person (docs/SWORD-SPARROW.md)
// ---------------------------------------------------------------------------------------------
//
// With a sword, in the Tower or on a sparrow the engine draws no first-person viewmodel: the arms are
// the Guardian's own gloves and gauntlets -- the very meshes the viewmodel uses, on the same 72-bone
// skeleton, in the same bind space (2026-10-18 captures) -- skinned by world-space palettes. vr_body.cpp
// hands them here made camera-relative, so the same rig, the same IK and the same hands pose them.

namespace {

std::atomic<std::uint64_t> g_freeRewrites{0};
/** The blade's trim about the controller's left axis (radians, positive raises the tip). */
std::atomic<float> g_swordPitch{0.0F};

/** The rotation taking unit `a` onto unit `b`, the shortest way. */
M3 rotation_between(const V3& a, const V3& b) noexcept {
    const V3 axis = cross(a, b);
    const float s = length(axis);
    const float c = dot(a, b);
    if (s < 1e-6F) {
        return c > 0.0F ? M3{} : axis_angle(normalize(cross(a, std::fabs(a.x) < 0.9F ? V3{1, 0, 0} : V3{0, 1, 0})), 3.14159265F);
    }
    return axis_angle(axis * (1.0F / s), std::atan2(s, c));
}

/** The right hand's hold in its controller's frame: the one learned on the guns, else the canonical one. */
M3 right_hold() noexcept {
    if (g_grip.valid) {
        return g_grip.rel.m;
    }
    // ARMS.md: the hand cannon's, at rest, in the controller's (forward, left, up) frame.
    return mat_of({0.0836F, -0.3577F, -0.0256F, 0.9297F});
}

/**
 * Where a free hand goes (the frame at the bind wrist, axes of the hand): its palm on the controller's
 * grip pose, turned the way the right hand holds a gun (mirrored for the left); the sword hand then
 * swung about the palm so the blade lies along the fist's axis. Deterministic in the sample, so the
 * sword palette (drawn apart, perhaps before the gloves) gets the very hand the gloves get.
 */
Affine free_target(const Rig& rig, std::size_t s, const FreeContext& ctx) noexcept {
    const xr::HandPose& hand = s == right ? ctx.right : ctx.left;
    const Affine ctrl = controller_frame(hand, ctx.head);
    const M3 hold = right_hold();
    M3 r = s == right ? ctrl.m * hold : ctrl.m * mirror(Affine{hold, V3{}}).m;
    const int other = 1 - static_cast<int>(s);
    const float twoHand = ctx.swordValid ? std::clamp(ctx.twoHand, 0.0F, 1.0F) : 0.0F;
    if (ctx.swordValid && ctx.swordHand == static_cast<int>(s)) {
        const Affine g = s == right ? ctx.sword : mirror(ctx.sword);
        const V3 bladeLocal = normalize(g.m * V3{0.0F, 0.0F, 1.0F});
        V3 want = normalize(to_v3(hand.gripForward), to_v3(hand.forward));
        const float pitch = g_swordPitch.load(std::memory_order_relaxed);
        if (pitch != 0.0F) {
            want = axis_angle(ctrl.m.c[1], -pitch) * want;
        }
        if (twoHand > 0.0F) {
            // Two hands on the hilt: the blade runs from the lower hand through this one.
            const xr::HandPose& support = other == 1 ? ctx.right : ctx.left;
            const V3 line = to_v3(hand.palm) - to_v3(support.palm);
            if (length(line) > 0.04F) {
                want = normalize(want * (1.0F - twoHand) + normalize(line) * twoHand, want);
            }
        }
        r = orthonormalize(rotation_between(normalize(r * bladeLocal), want) * r);
    } else if (twoHand > 0.0F && ctx.swordHand == other) {
        // The support hand: closed round the hilt below the sword hand, on the line the blade runs along,
        // turned like the sword hand's grip mirrored and laid along the same axis.
        const xr::HandPose& main = other == 1 ? ctx.right : ctx.left;
        const Affine mainCtrl = controller_frame(main, ctx.head);
        const V3 axis = normalize(to_v3(main.palm) - to_v3(hand.palm), normalize(to_v3(main.gripForward)));
        const float along = std::clamp(dot(mainCtrl.t - ctrl.t, axis), 0.08F, 0.22F);
        const V3 onHilt = mainCtrl.t - axis * along;
        const V3 tubeNow = normalize(to_v3(hand.gripForward), to_v3(hand.forward));
        const M3 held = orthonormalize(rotation_between(tubeNow, axis) * r);
        const Affine free{r, ctrl.t - r * (rig.palm[s] - rig.w[s])};
        const Affine onIt{held, onHilt - held * (rig.palm[s] - rig.w[s])};
        return blend_about(free, onIt, twoHand, rig.palm[s] - rig.w[s]);
    }
    return Affine{r, ctrl.t - r * (rig.palm[s] - rig.w[s])};
}

/** The closed hand's finger `b` relative to the hand frame: the fist from the guns, else the game's own. */
Affine closed_finger(const Rig& rig, const float* bones, std::size_t boneCount, const Affine& gameRight,
                     std::size_t b, std::size_t side) noexcept {
    const int rb = side == right ? static_cast<int>(b) : rig.mirrorOf[b];
    if (rb < 0 || static_cast<std::size_t>(rb) >= boneCount) {
        return Affine{M3{}, -rig.w[side]};
    }
    const auto r = static_cast<std::size_t>(rb);
    const Affine rel = g_fistValid && g_fistSet[r] ? g_fist[r] : inverse_rigid(gameRight) * read_rows(bones + r * 12);
    return side == right ? rel : mirror(rel);
}

} // namespace

bool free_ready() noexcept {
    return g_enabled.load(std::memory_order_relaxed) && g_rig.valid && enabled() && xr::active();
}

bool free_hand_bone(const FreeContext& ctx, int side, Affine& out) noexcept {
    if (!free_ready() || side < 0 || side > 1 || !ctx.head.valid) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const auto s = static_cast<std::size_t>(side);
    const Affine t = free_target(g_rig, s, ctx);
    out = Affine{t.m, t.t - t.m * g_rig.w[s]};
    ReleaseSRWLockShared(&g_lock);
    return true;
}

bool hand_bones(int& leftHand, int& rightHand) noexcept {
    AcquireSRWLockShared(&g_lock);
    leftHand = g_labels.side[left].hand;
    rightHand = g_labels.side[right].hand;
    const bool ok = g_rig.valid;
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

bool palm_points(const float* bones, std::size_t boneCount, V3& leftPalm, V3& rightPalm) noexcept {
    AcquireSRWLockShared(&g_lock);
    const int hl = g_labels.side[left].hand;
    const int hr = g_labels.side[right].hand;
    const bool ok = g_rig.valid && hl >= 0 && hr >= 0 && static_cast<std::size_t>(std::max(hl, hr)) < boneCount;
    if (ok) {
        leftPalm = read_rows(bones + static_cast<std::size_t>(hl) * 12) * g_rig.palm[left];
        rightPalm = read_rows(bones + static_cast<std::size_t>(hr) * 12) * g_rig.palm[right];
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

bool rewrite_free(const float* bones, std::size_t boneCount, float* out, const FreeContext& ctx) noexcept {
    if (!free_ready() || bones == nullptr || out == nullptr || !ctx.head.valid) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const Rig& rig = g_rig;
    const Labels& labels = g_labels;
    const Params params = g_params;
    const int handL = labels.side[left].hand;
    const int handR = labels.side[right].hand;
    if (!rig.valid || static_cast<std::size_t>(std::max(handL, handR)) >= boneCount) {
        ReleaseSRWLockShared(&g_lock);
        return false;
    }
    // The body, hung from the neck exactly as in first person, facing the context's heading.
    const V3 f = to_v3(ctx.head.forward);
    const V3 u = to_v3(ctx.head.up);
    const V3 eyeL = to_v3(ctx.head.eyeOffsets[0]);
    const V3 eyeR = to_v3(ctx.head.eyeOffsets[1]);
    const bool eyesKnown = length(eyeR - eyeL) > 1e-4F;
    const V3 eyeToCentre = eyesKnown ? (eyeL + eyeR) * 0.5F - to_v3(ctx.head.offset) : V3{};
    const V3 neck = eyeToCentre - (f * params.neckFwd) - u * params.neckUp;
    const M3 yaw = yaw_matrix(ctx.bodyYaw);
    const V3 shoulderMid = neck + yaw * V3{params.shoulderFwd, 0.0F, params.shoulderUp};
    const Affine body{yaw, shoulderMid - yaw * rig.shoulderMid};

    // The game's own hands (a sword in the right one, the handlebars, an idle pose).
    std::array<Affine, 2> animated{};
    for (std::size_t s = 0; s < 2; ++s) {
        const Affine m = read_rows(bones + static_cast<std::size_t>(labels.side[s].hand) * 12);
        animated[s] = Affine{orthonormalize(m.m), m * rig.w[s]};
    }
    std::array<Affine, 2> target{};
    for (std::size_t s = 0; s < 2; ++s) {
        target[s] = free_target(rig, s, ctx);
        const float w = std::clamp(ctx.bar[s], 0.0F, 1.0F);
        if (w > 0.0F) {
            // On the handlebar: the game's own hand there, blended in about the palm.
            target[s] = blend_about(target[s], animated[s], w, rig.palm[s] - rig.w[s]);
        }
    }
    const float gripL = std::clamp(ctx.left.valid ? xr::input_state().gripL : 0.0F, 0.0F, 1.0F);
    const float gripR = std::clamp(ctx.right.valid ? xr::input_state().gripR : 0.0F, 0.0F, 1.0F);

    // Solve both arms (as in first person).
    std::array<Affine, 2> upperT{};
    std::array<Affine, 2> foreT{};
    std::array<V3, 2> foreAxis{};
    std::array<V3, 2> elbowT{};
    std::array<float, 2> twist{};
    std::array<float, 2> stretch{};
    for (std::size_t s = 0; s < 2; ++s) {
        const V3 outward = V3{params.pole.x, s == left ? params.pole.y : -params.pole.y, params.pole.z};
        const V3 pole = yaw * outward;
        const V3 shoulder = body * rig.s[s];
        const V3 wrist = target[s].t;
        const IkResult ik = solve_two_bone(shoulder, wrist, rig.l1[s], rig.l2[s], pole, params.stretchMax);
        const V3 dh = normalize(wrist - ik.s);
        V3 p = pole - dh * dot(pole, dh);
        p = normalize(p, normalize(cross(dh, V3{0, 0, 1}), V3{0, 1, 0}));
        const V3 n = normalize(cross(p, dh), V3{0, 0, 1});
        const M3 upperRot = frame_from(ik.e - ik.s, n) * transpose(rig.upperBind[s]);
        upperT[s] = Affine{upperRot, ik.s - upperRot * rig.s[s]};
        if (ik.k > 1.0001F) {
            upperT[s] = stretch_about(ik.s, normalize(ik.e - ik.s), ik.k) * upperT[s];
        }
        const M3 foreRot = frame_from(wrist - ik.e, n) * transpose(rig.foreBind[s]);
        foreT[s] = Affine{foreRot, ik.e - foreRot * rig.e[s]};
        foreAxis[s] = normalize(wrist - ik.e);
        elbowT[s] = ik.e;
        twist[s] = wrap(twist_about(target[s].m * transpose(foreRot), foreAxis[s]));
        stretch[s] = ik.k;
    }

    for (std::size_t b = 0; b < boneCount && b < kMaxBones; ++b) {
        const float* in4 = bones + b * 12;
        float* out4 = out + b * 12;
        const auto s = static_cast<std::size_t>(rig.side[b] < 0 ? 0 : rig.side[b]);
        Affine m{};
        switch (rig.seg[b]) {
        case segBody: m = body; break;
        case segUpper: m = upperT[s]; break;
        case segFore:
        case segForeTwist: {
            const M3 r = axis_angle(foreAxis[s], twist[s] * rig.twist[b]);
            m = rotate_about(r, elbowT[s]) * foreT[s];
            if (stretch[s] > 1.0001F) {
                m = stretch_about(elbowT[s], foreAxis[s], stretch[s]) * m;
            }
            break;
        }
        case segHand: {
            const int handBone = labels.side[s].hand;
            if (static_cast<int>(b) == handBone) {
                m = Affine{target[s].m, target[s].t - target[s].m * rig.w[s]};
                break;
            }
            Affine rel{};
            if (ctx.swordValid && ctx.swordHand == static_cast<int>(s)) {
                // Round the hilt: the game's own right hand on it (mirrored for the left).
                const int rb = s == right ? static_cast<int>(b) : rig.mirrorOf[b];
                if (rb >= 0 && static_cast<std::size_t>(rb) < boneCount) {
                    rel = inverse_rigid(animated[right]) * read_rows(bones + static_cast<std::size_t>(rb) * 12);
                    rel = s == right ? rel : mirror(rel);
                } else {
                    rel = Affine{M3{}, -rig.w[s]};
                }
            } else {
                // Free: open to closed with the hand's own grip button; on the handlebar, the game's.
                const Affine open{M3{}, -rig.w[s]};
                const float curl = std::max(s == right ? gripR : gripL,
                                            ctx.swordValid && ctx.swordHand == 1 - static_cast<int>(s) ? ctx.twoHand : 0.0F);
                rel = blend_about(open, closed_finger(rig, bones, boneCount, animated[right], b, s), curl, rig.cen[b]);
                const float w = std::clamp(ctx.bar[s], 0.0F, 1.0F);
                if (w > 0.0F) {
                    rel = blend_about(rel, inverse_rigid(animated[s]) * read_rows(in4), w, rig.cen[b]);
                }
            }
            m = target[s] * rel;
            break;
        }
        default: std::memcpy(out4, in4, sizeof(float) * 12); continue;
        }
        write_rows(m, out4);
    }
    ReleaseSRWLockShared(&g_lock);
    g_freeRewrites.fetch_add(1, std::memory_order_relaxed);
    return true;
}

namespace {

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

void dump_rig() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Rig rig = g_rig;
    const Labels labels = g_labels;
    ReleaseSRWLockShared(&g_lock);
    for (std::size_t s = 0; s < 2; ++s) {
        const SideRig& sr = labels.side[s];
        reportf("ev=vr.arms rig side=%s clav=%d ut=%d upper=%d fore=%d ft=%d hand=%d "
                "shoulder=%.3f,%.3f,%.3f elbow=%.3f,%.3f,%.3f wrist=%.3f,%.3f,%.3f l1=%.3f l2=%.3f "
                "prior_s=%.3f,%.3f,%.3f ls_frames=%.0f",
                s == left ? "L" : "R", sr.clav, sr.upperTwist, sr.upper, sr.fore, sr.foreTwist, sr.hand, rig.s[s].x,
                rig.s[s].y, rig.s[s].z, rig.e[s].x, rig.e[s].y, rig.e[s].z, rig.w[s].x, rig.w[s].y, rig.w[s].z,
                rig.l1[s], rig.l2[s], rig.prior[s][0].x, rig.prior[s][0].y, rig.prior[s][0].z, g_ls[s][0].frames);
    }
    std::array<char, core::log::kLineCapacity> line{};
    int used = std::snprintf(line.data(), line.size(), "ev=vr.arms segs valid=%d", rig.valid ? 1 : 0);
    for (std::size_t b = 0; b < kMaxBones && used > 0 && static_cast<std::size_t>(used) < line.size() - 16; ++b) {
        if (rig.seg[b] != segNone) {
            used += std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used), " %zu:%c%d", b,
                                  "-BUFTH"[rig.seg[b]], rig.side[b]);
        }
    }
    report(line.data());
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
        char a[16] = {};
        const int n = sscanf_s(line.data(), "%15s %15s", verb, static_cast<unsigned>(sizeof verb), a,
                               static_cast<unsigned>(sizeof a));
        if (n < 1) {
            continue;
        }
        float v[3] = {};
        AcquireSRWLockExclusive(&g_lock);
        Params& p = g_params;
        if (std::strcmp(verb, "enable") == 0 && n >= 2) {
            g_enabled.store(std::strcmp(a, "off") != 0);
        } else if (std::strcmp(verb, "overlay") == 0 && n >= 2) {
            g_overlay.store(std::strcmp(a, "off") != 0);
        } else if (std::strcmp(verb, "neck") == 0 && sscanf_s(line.data(), "%*s %f %f", &v[0], &v[1]) == 2) {
            p.neckFwd = v[0];
            p.neckUp = v[1];
        } else if (std::strcmp(verb, "shoulder") == 0 && sscanf_s(line.data(), "%*s %f %f", &v[0], &v[1]) == 2) {
            p.shoulderFwd = v[0];
            p.shoulderUp = v[1];
        } else if (std::strcmp(verb, "pole") == 0
                   && sscanf_s(line.data(), "%*s %f %f %f", &v[0], &v[1], &v[2]) == 3) {
            p.pole = V3{v[0], v[1], v[2]};
        } else if (std::strcmp(verb, "stretch") == 0 && sscanf_s(line.data(), "%*s %f", &v[0]) == 1) {
            p.stretchMax = std::clamp(v[0], 1.0F, 1.5F);
        } else if (std::strcmp(verb, "twist") == 0 && sscanf_s(line.data(), "%*s %f %f", &v[0], &v[1]) == 2) {
            p.twistFore = v[0];
            p.twistWrist = v[1];
            rebuild_rig();
        } else if (std::strcmp(verb, "relearn") == 0) {
            g_grip = Grip{};
        } else if (std::strcmp(verb, "sword") == 0 && std::strcmp(a, "pitch") == 0
                   && sscanf_s(line.data(), "%*s %*s %f", &v[0]) == 1) {
            // sword pitch <deg>: the blade raised (+) or lowered from the fist's axis, live.
            g_swordPitch.store(std::clamp(v[0], -60.0F, 60.0F) * 0.0174533F, std::memory_order_relaxed);
            reportf("ev=vr.arms sword pitch_deg=%.1f", v[0]);
        } else if (std::strcmp(verb, "canon") == 0 && n >= 2) {
            // canon learn|on|off|reset|aim <deg>
            if (std::strcmp(a, "learn") == 0) {
                g_canonSet = false;
                g_canonSteadySince = 0;
            } else if (std::strcmp(a, "aim") == 0 && sscanf_s(line.data(), "%*s %*s %f", &v[0]) == 1) {
                g_aimPitch.store(std::clamp(v[0], -45.0F, 45.0F) * 0.0174533F, std::memory_order_relaxed);
            } else if (std::strcmp(a, "reset") == 0) {
                g_seatQ = M3{};
                weapon::set_seat_rotation(quat_of(g_seatQ));
            } else {
                g_rotSeat.store(std::strcmp(a, "off") != 0);
                if (std::strcmp(a, "off") == 0) {
                    g_seatQ = M3{};
                    weapon::set_seat_rotation(quat_of(g_seatQ));
                }
            }
        } else if (std::strcmp(verb, "twohand") == 0 && n >= 2) {
            HoldParams& h = g_hold;
            char b[16] = {};
            const bool hasB = sscanf_s(line.data(), "%*s %*s %15s", b, static_cast<unsigned>(sizeof b)) == 1;
            if (std::strcmp(a, "on") == 0 || std::strcmp(a, "off") == 0) {
                h.on = std::strcmp(a, "on") == 0;
            } else if (std::strcmp(a, "aim") == 0 && hasB) {
                h.aim = std::strcmp(b, "off") != 0;
            } else if (std::strcmp(a, "engage") == 0 && sscanf_s(line.data(), "%*s %*s %f %f", &v[0], &v[1]) == 2) {
                h.engage = std::clamp(v[0], 0.02F, 0.30F);
                h.grenade = std::clamp(v[1], h.engage, 1.0F);
            } else if (std::strcmp(a, "slide") == 0 && sscanf_s(line.data(), "%*s %*s %f %f", &v[0], &v[1]) == 2) {
                h.slideBack = std::clamp(v[0], 0.0F, 0.4F);
                h.slideFwd = std::clamp(v[1], 0.0F, 0.4F);
            } else if (std::strcmp(a, "pistol") == 0 && sscanf_s(line.data(), "%*s %*s %f", &v[0]) == 1) {
                h.pistol = std::clamp(v[0], 0.0F, 1.0F);
            } else if (std::strcmp(a, "blend") == 0 && sscanf_s(line.data(), "%*s %*s %f", &v[0]) == 1) {
                h.blend = std::clamp(v[0], 0.0F, 1.0F);
            } else if (std::strcmp(a, "relearn") == 0) {
                g_fore = Foregrip{};
            }
            reportf("ev=vr.arms twohand on=%d aim=%d engage=%.3f grenade=%.3f slide=%.3f,%.3f pistol=%.3f blend=%.3f",
                    h.on ? 1 : 0, h.aim ? 1 : 0, h.engage, h.grenade, h.slideBack, h.slideFwd, h.pistol, h.blend);
        } else if (std::strcmp(verb, "rig") == 0) {
            char sideName[4] = {};
            int b[6] = {};
            if (sscanf_s(line.data(), "%*s %3s %d %d %d %d %d %d", sideName, static_cast<unsigned>(sizeof sideName),
                         &b[0], &b[1], &b[2], &b[3], &b[4], &b[5])
                == 7) {
                SideRig& sr = g_labels.side[sideName[0] == 'R' ? right : left];
                sr = SideRig{b[0], b[1], b[2], b[3], b[4], b[5]};
                rebuild_rig();
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (std::strcmp(verb, "dump") == 0) {
            dump_rig();
        }
        if (std::strcmp(verb, "dump") == 0 || (std::strcmp(verb, "twohand") == 0 && std::strcmp(a, "dump") == 0)) {
            report_hold();
        }
        reportf("ev=vr.arms command %s", verb);
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

void add_frame(std::vector<wl::Edge>& edges, const Affine& frame, float size) {
    edges.push_back(wl::Edge{{frame.t.x, frame.t.y, frame.t.z},
                             {frame.t.x + frame.m.c[0].x * size, frame.t.y + frame.m.c[0].y * size,
                              frame.t.z + frame.m.c[0].z * size},
                             wl::Color{255, 40, 40, 255}});
    edges.push_back(wl::Edge{{frame.t.x, frame.t.y, frame.t.z},
                             {frame.t.x + frame.m.c[1].x * size, frame.t.y + frame.m.c[1].y * size,
                              frame.t.z + frame.m.c[1].z * size},
                             wl::Color{40, 255, 40, 255}});
    edges.push_back(wl::Edge{{frame.t.x, frame.t.y, frame.t.z},
                             {frame.t.x + frame.m.c[2].x * size, frame.t.y + frame.m.c[2].y * size,
                              frame.t.z + frame.m.c[2].z * size},
                             wl::Color{60, 120, 255, 255}});
}

/**
 * The verification overlay: each controller's frame (RGB = forward, left, up), the solved arm
 * (shoulder -> elbow -> wrist, yellow), and the hand target frame. Drawn with the frame's own
 * viewmodel camera, after the FOV fix, so it lands exactly where the arm geometry is drawn -- a
 * yellow line outside the sleeve is a bug, not an approximation.
 */
void draw_overlay(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) noexcept {
    gpu::ViewCamera camera{};
    if (!gpu::viewmodel_camera(camera)) {
        return;
    }
    AcquireSRWLockShared(&g_lock);
    const Overlay o = g_overlayData;
    ReleaseSRWLockShared(&g_lock);
    if (!o.valid) {
        return;
    }
    ID3D11RenderTargetView* const target = gpu::back_buffer_view(device, swapChain);
    if (target == nullptr) {
        return;
    }
    std::vector<wl::Edge> edges;
    std::vector<wl::Sphere> spheres;
    for (std::size_t s = 0; s < 2; ++s) {
        add_frame(edges, o.controller[s], 0.10F);
        add_frame(edges, o.hand[s], 0.05F);
        const wl::Color arm = o.slid[s] ? wl::Color{255, 80, 0, 255} : wl::Color{255, 230, 0, 255};
        edges.push_back(wl::Edge{{o.s[s].x, o.s[s].y, o.s[s].z}, {o.e[s].x, o.e[s].y, o.e[s].z}, arm});
        edges.push_back(wl::Edge{{o.e[s].x, o.e[s].y, o.e[s].z}, {o.w[s].x, o.w[s].y, o.w[s].z}, arm});
        spheres.push_back(wl::Sphere{{o.s[s].x, o.s[s].y, o.s[s].z}, 0.02F, 12, wl::Color{255, 230, 0, 255}});
        spheres.push_back(wl::Sphere{{o.e[s].x, o.e[s].y, o.e[s].z}, 0.015F, 12, wl::Color{255, 230, 0, 255}});
        spheres.push_back(wl::Sphere{{o.w[s].x, o.w[s].y, o.w[s].z}, 0.012F, 12, wl::Color{255, 255, 255, 255}});
    }
    // The handguard on the drawn gun: cyan while free, amber while taking hold, green when held,
    // with the palm on it; magenta from the left controller to that palm, which is the residue a
    // held foregrip should have none of.
    if (o.foreValid) {
        const wl::Color guard = o.holdW > 0.99F  ? wl::Color{40, 255, 120, 255}
                                : o.holdW > 0.0F ? wl::Color{255, 190, 0, 255}
                                                 : wl::Color{0, 200, 255, 255};
        edges.push_back(wl::Edge{{o.guardA.x, o.guardA.y, o.guardA.z}, {o.guardB.x, o.guardB.y, o.guardB.z}, guard});
        spheres.push_back(wl::Sphere{{o.snapPalm.x, o.snapPalm.y, o.snapPalm.z}, 0.018F, 12, guard});
        if (o.holdW > 0.0F) {
            edges.push_back(wl::Edge{{o.ctrlPalmL.x, o.ctrlPalmL.y, o.ctrlPalmL.z},
                                     {o.snapPalm.x, o.snapPalm.y, o.snapPalm.z},
                                     wl::Color{255, 0, 255, 255}});
        }
    }
    teleport::CameraPose pose{};
    pose.position = {0.0F, 0.0F, 0.0F};
    pose.forward = camera.forward;
    pose.up = camera.up;
    pose.horizontalFov = camera.horizontalFov;
    pose.aspect = camera.aspect;
    wl::Batch batch{};
    batch.edges = edges;
    batch.spheres = spheres;
    batch.lineWidthPixels = 2.0F;
    static_cast<void>(wl::draw(device, context, target, pose, batch));
}

void report_state() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Overlay o = g_overlayData;
    const bool valid = g_rig.valid;
    const bool grip = g_grip.valid;
    ReleaseSRWLockShared(&g_lock);
    const auto seat = weapon::seat_correction();
    const auto sq = quat_of(g_seatQ);
    reportf("ev=vr.arms seat err=%.4f correction=%.3f,%.3f,%.3f rot_err_deg=%.2f rot=%.4f,%.4f,%.4f,%.4f canon=%d",
            g_seatError, seat[0], seat[1], seat[2], g_seatAngle * 57.2958F, sq[0], sq[1], sq[2], sq[3],
            g_canonSet ? 1 : 0);
    reportf("ev=vr.arms state ready=%d rig=%d grip=%d rewrites=%llu "
            "L stretch=%.3f twist=%.1f wrist_err=%.4f slid=%d R stretch=%.3f twist=%.1f wrist_err=%.4f slid=%d",
            ready() ? 1 : 0, valid ? 1 : 0, grip ? 1 : 0,
            static_cast<unsigned long long>(g_rewrites.exchange(0, std::memory_order_relaxed)), o.stretch[0],
            o.twist[0] * 57.2958F, o.wristError[0], o.slid[0] ? 1 : 0, o.stretch[1], o.twist[1] * 57.2958F,
            o.wristError[1], o.slid[1] ? 1 : 0);
    // Where everything is, camera-relative (game axes: X forward, Y left, Z up), so a pose that
    // lands off screen can be located from the log alone.
    for (std::size_t s = 0; s < 2; ++s) {
        reportf("ev=vr.arms where side=%s shoulder=%.3f,%.3f,%.3f elbow=%.3f,%.3f,%.3f wrist=%.3f,%.3f,%.3f "
                "ctrl=%.3f,%.3f,%.3f ctrl_fwd=%.2f,%.2f,%.2f",
                s == 0 ? "L" : "R", o.s[s].x, o.s[s].y, o.s[s].z, o.e[s].x, o.e[s].y, o.e[s].z, o.w[s].x, o.w[s].y,
                o.w[s].z, o.controller[s].t.x, o.controller[s].t.y, o.controller[s].t.z, o.controller[s].m.c[0].x,
                o.controller[s].m.c[0].y, o.controller[s].m.c[0].z);
    }
}

} // namespace

void present(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) noexcept {
    const std::uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame % kPollPeriod == 0) {
        poll_commands();
    }
    // The body's heading follows the head with a little lag and never more than 35 degrees
    // behind: torsos turn with the gaze, but not with every flick of the eyes.
    xr::HeadPose head{};
    xr::HandPose l{};
    xr::HandPose r{};
    xr::frame_sample(head, l, r);
    if (head.valid) {
        const float target = heading_of(head);
        if (!g_bodyYawValid.load(std::memory_order_relaxed)) {
            g_bodyYaw.store(target, std::memory_order_relaxed);
            g_bodyYawValid.store(true, std::memory_order_relaxed);
        } else {
            const float current = g_bodyYaw.load(std::memory_order_relaxed);
            float delta = wrap(target - current) * 0.2F;
            const float lag = wrap(target - (current + delta));
            constexpr float kMaxLag = 0.61F;
            if (lag > kMaxLag) {
                delta += lag - kMaxLag;
            } else if (lag < -kMaxLag) {
                delta += lag + kMaxLag;
            }
            g_bodyYaw.store(wrap(current + delta), std::memory_order_relaxed);
        }
    }
    if (g_overlay.load(std::memory_order_relaxed) && device != nullptr && context != nullptr && swapChain != nullptr) {
        draw_overlay(device, context, swapChain);
    }
    if (frame % kReportPeriod == 0 && g_rig.valid) {
        report_state();
    }
}

// ---------------------------------------------------------------------------------------------
// The two-handed hold: the state machine
// ---------------------------------------------------------------------------------------------

namespace {

/** The gamepad's own thresholds for a grip as a button, so a press means the same in both places. */
constexpr float kGripPress = 0.55F;
constexpr float kGripRelease = 0.35F;

bool pressed(float value, bool held) noexcept {
    return held ? value > kGripRelease : value > kGripPress;
}

void take_hold(HoldMachine& h, bool aim, const V3& fc, float sMin, float sMax, std::uint32_t generation) noexcept {
    h.state = Hold::held;
    h.aim = aim;
    h.generation = generation;
    h.fc = fc;
    h.sMin = sMin;
    h.sMax = sMax;
    ++h.grabs;
    // A short, firm click in the left hand: the one confirmation a player needs that the gun is
    // now in both hands, and the convention of every shooter that has a foregrip.
    xr::haptic(false, 0.55F, 0.025F);
    reportf("ev=vr.arms hold event=grab mode=%s dist=%.3f fc=%.3f,%.3f,%.3f lever=%.3f slide=%.2f..%.2f",
            aim ? "aim" : "support", h.dist, fc.x, fc.y, fc.z, length(fc), sMin, sMax);
}

/**
 * @param pulse True when the game or the geometry took the hold away rather than the player
 *              opening their hand: a player letting go knows they did, one losing the gun should feel it.
 */
void let_go(HoldMachine& h, Hold next, const char* reason, bool pulse) noexcept {
    h.state = next;
    h.lastReason = reason;
    ++h.releases;
    if (pulse) {
        xr::haptic(false, 0.25F, 0.012F);
    }
    reportf("ev=vr.arms hold event=release reason=%s next=%s swing_deg=%.1f slide=%.3f lever=%.3f dist=%.3f", reason,
            hold_name(next), h.swingAngle * 57.2958F, h.slide, h.lever, h.dist);
}

void report_hold() noexcept {
    const HoldMachine& h = g_machine;
    AcquireSRWLockShared(&g_lock);
    const Foregrip& f = g_fore;
    const bool valid = f.valid;
    const bool fresh = f.fresh;
    const bool fcValid = f.fcValid;
    const V3 fc = f.fc;
    const Overlay o = g_overlayData;
    ReleaseSRWLockShared(&g_lock);
    reportf("ev=vr.arms hold state=%s mode=%s w=%.2f dist=%.3f swing_deg=%.2f slide=%.3f lever=%.3f fore=%d fresh=%d "
            "fc_ok=%d fc=%.3f,%.3f,%.3f fc_len=%.3f line_err=%.4f hand_err=%.4f drawn_slide=%.3f grabs=%u "
            "releases=%u grenades=%u last=%s",
            hold_name(h.state), h.aim ? "aim" : "support", g_holdW.load(std::memory_order_relaxed), h.dist,
            h.swingAngle * 57.2958F, h.slide, h.lever, valid ? 1 : 0, fresh ? 1 : 0, fcValid ? 1 : 0, fc.x, fc.y, fc.z,
            length(fc), o.lineError, o.handError, o.slide, h.grabs, h.releases, h.grenades, h.lastReason);
}

} // namespace

void update_hold() noexcept {
    HoldMachine& h = g_machine;
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float dt = 0.0F;
    if (h.last.QuadPart != 0 && frequency.QuadPart != 0) {
        dt = static_cast<float>(now.QuadPart - h.last.QuadPart) / static_cast<float>(frequency.QuadPart);
    }
    h.last = now;
    dt = std::clamp(dt, 0.0F, 0.05F);
    const std::uint64_t nowMs = GetTickCount64();

    xr::HeadPose head{};
    xr::HandPose l{};
    xr::HandPose r{};
    xr::frame_sample(head, l, r);
    const xr::InputState in = xr::input_state();

    AcquireSRWLockShared(&g_lock);
    const HoldParams hp = g_hold;
    // Known: there is a foregrip to measure against, if perhaps of the weapon being swapped away.
    // Ok: confirmed since, so it may be taken hold of.
    const bool foreKnown = g_fore.valid && g_fore.fcValid;
    const bool foreOk = foreKnown && g_fore.fresh;
    const V3 fcNow = g_fore.fc;
    const std::uint32_t foreGen = g_fore.generation;
    ReleaseSRWLockShared(&g_lock);

    // Nothing to hold (arms off, rig unknown, a controller missing): every squeeze is the grenade,
    // which is exactly what the left grip did before the hold existed.
    // In dual wield the left hand holds a weapon of its own: there is no second hand for this one.
    const bool live = hp.on && ready() && in.valid && l.valid && r.valid && !dual::dual();
    const bool zoneKnown = live && foreKnown;
    const bool zoneOk = live && foreOk;
    const bool support = length(fcNow) < hp.pistol;
    const bool aimNow = hp.aim && !support;
    const float sMinNow = aimNow ? -hp.slideBack : 0.0F;
    const float sMaxNow = aimNow ? hp.slideFwd : 0.0F;
    const M3 c = orthonormalize(M3{{to_v3(r.forward), -to_v3(r.right), to_v3(r.up)}});
    const V3 p = to_v3(r.palm);
    if (l.valid && !l.stale && r.valid) {
        h.leftInRight = transpose(c) * (to_v3(l.palm) - p);
        h.leftInRightValid = true;
    }
    // Tracking loss: the left hand held where it last was relative to the right, as the weapon
    // module does, so the two agree about where the hand is.
    const V3 lp = l.stale && h.leftInRightValid ? p + c * h.leftInRight : to_v3(l.palm);
    if (h.state != Hold::held) {
        h.dist = zoneKnown ? two_hand::zone_distance(c, p, lp, fcNow, sMinNow, sMaxNow) : 99.0F;
    }

    // The actions whose animation takes the gun out of the hands: a weapon swap (Y), a melee (right
    // grip), a super (right stick click). The hold is suspended -- the hand comes back by itself
    // if the grip is still squeezed when the gun is -- and the foregrip has to be confirmed again,
    // because after a swap it belongs to a different weapon.
    const bool y = (in.buttons & xr::kButtonY) != 0;
    const bool melee = pressed(in.gripR, h.lastMelee);
    const bool superPress = (in.buttons & xr::kButtonThumbR) != 0;
    const bool away = in.valid && ((y && !h.lastY) || (melee && !h.lastMelee) || (superPress && !h.lastSuper));
    const char* awayReason = y && !h.lastY ? "swap" : (melee && !h.lastMelee ? "melee" : "super");
    h.lastY = y;
    h.lastMelee = melee;
    h.lastSuper = superPress;
    if (away) {
        AcquireSRWLockExclusive(&g_lock);
        g_fore.fresh = false;
        g_fore.doubtSince = nowMs;
        g_fore.stillSince = 0;
        ReleaseSRWLockExclusive(&g_lock);
        if (h.state == Hold::held) {
            let_go(h, Hold::suspended, awayReason, false);
            h.suspendedAt = nowMs;
        }
    }

    const bool wasDown = h.down;
    h.down = in.valid && pressed(in.gripL, h.down);
    if (!h.down) {
        if (h.state == Hold::held) {
            let_go(h, Hold::free, "grip", false);
        }
        h.state = Hold::free;
    } else if (!wasDown) {
        // The squeeze is classified once, where it starts: on the handguard it takes hold, near it
        // it waits for the hand to arrive, anywhere else it is a grenade for as long as it lasts.
        // Near a foregrip still in doubt (the moments after a swap) it waits too: reaching for the
        // new gun as it comes up must not throw a grenade. With the grenade on the left trigger
        // (physical ADS, the default) there is no grenade here at all: a squeeze anywhere waits,
        // and takes hold when the hand reaches the handguard.
        if (g_gripGrenade.load(std::memory_order_relaxed) && (!zoneKnown || h.dist > hp.grenade)) {
            h.state = Hold::grenade;
            ++h.grenades;
            reportf("ev=vr.arms hold event=grenade dist=%.3f zone=%d", h.dist, zoneKnown ? 1 : 0);
        } else if (zoneOk && h.dist <= hp.engage) {
            take_hold(h, aimNow, fcNow, sMinNow, sMaxNow, foreGen);
        } else {
            h.state = Hold::pending;
        }
    } else {
        switch (h.state) {
        case Hold::pending:
            if (zoneOk && h.dist <= hp.engage) {
                take_hold(h, aimNow, fcNow, sMinNow, sMaxNow, foreGen);
            }
            break;
        case Hold::suspended:
            if (zoneOk && nowMs - h.suspendedAt > 400 && h.dist <= hp.engage) {
                take_hold(h, aimNow, fcNow, sMinNow, sMaxNow, foreGen);
            }
            break;
        case Hold::held:
            if (!live) {
                let_go(h, Hold::pending, "lost", true);
            } else if (foreGen != h.generation) {
                // The weapon changed under the hand without a button saying so (ammo ran out, a
                // pickup): the hold steers by a foregrip that is no longer there. Take it again
                // on the new one.
                let_go(h, Hold::suspended, "regrip", true);
                h.suspendedAt = nowMs;
            } else if (h.aim) {
                // The barrel follows the hand, so the hand is always on the line; what can break
                // the hold is how far the gun would have to turn, the hands meeting, or the hand
                // sliding off either end of the handguard.
                const two_hand::Swing sw = two_hand::swing(c, p, lp, h.fc, h.sMin, h.sMax);
                h.swingAngle = sw.angle;
                h.slide = sw.s;
                h.lever = sw.lever;
                h.dist = std::fabs(sw.sRaw - sw.s);
                if (sw.angle > hp.maxSwing) {
                    let_go(h, Hold::pending, "swing", true);
                } else if (sw.lever < hp.minLever) {
                    let_go(h, Hold::pending, "close", true);
                } else if (sw.sRaw > h.sMax + hp.releaseSlide || sw.sRaw < h.sMin - hp.releaseSlide) {
                    let_go(h, Hold::pending, "slide", true);
                }
            } else {
                h.dist = length(lp - (p + c * h.fc));
                h.swingAngle = 0.0F;
                h.lever = length(lp - p);
                if (h.dist > hp.releaseSupport) {
                    let_go(h, Hold::pending, "range", true);
                }
            }
            break;
        default: break;
        }
    }

    // The weight: real time, not frames (the mock runs near 190 fps, a headset at 72 to 120), eased
    // so the hand arrives on the gun and the barrel swings onto the line without a kink.
    const float target = h.state == Hold::held ? 1.0F : 0.0F;
    const float step = hp.blend > 1e-3F ? dt / hp.blend : 1.0F;
    h.ramp = target > h.ramp ? std::min(target, h.ramp + step) : std::max(target, h.ramp - step);
    const float w = h.ramp * h.ramp * (3.0F - 2.0F * h.ramp);
    g_holdW.store(w, std::memory_order_relaxed);
    g_holdAim.store(h.aim, std::memory_order_relaxed);
    g_holdSlideMin.store(h.sMin, std::memory_order_relaxed);
    g_holdSlideMax.store(h.sMax, std::memory_order_relaxed);
    if (w > 0.0F) {
        // A little past the end of the ramp: the palettes still in flight were posed with it.
        g_twoHandedUntil.store(nowMs + 200, std::memory_order_relaxed);
    }
    g_grenadeHeld.store(h.state == Hold::grenade, std::memory_order_relaxed);
    weapon::TwoHandGrip grip{};
    grip.weight = w;
    grip.aim = h.aim;
    grip.foregrip = {h.fc.x, h.fc.y, h.fc.z};
    grip.slideMin = h.sMin;
    grip.slideMax = h.sMax;
    weapon::set_two_hand(grip);
    if (g_frame.load(std::memory_order_relaxed) % kReportPeriod == 0 && g_rig.valid) {
        report_hold();
    }
}

bool grenade() noexcept {
    return g_grenadeHeld.load(std::memory_order_relaxed);
}

void set_grip_grenade(bool on) noexcept {
    g_gripGrenade.store(on, std::memory_order_relaxed);
}

bool holding() noexcept {
    return g_holdW.load(std::memory_order_relaxed) > 0.0F;
}

void snapshot_grip(std::uintptr_t key) noexcept {
    if (key != 0) {
        g_snapshotWanted.store(key, std::memory_order_relaxed);
    }
}

} // namespace sunrise::client::hooks::vr::arms
