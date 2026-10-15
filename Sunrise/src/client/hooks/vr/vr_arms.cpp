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
#include "vr_gpu.h"
#include "vr_math.h"
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
float g_seatError = 0.0F;
/**
 * The canonical right-hand grip: the hand's rotation in the controller's (forward, left, up)
 * frame when holding the hand cannon at rest -- the pistol hold Bungie animated, with the barrel
 * along the controller's aim. Every weapon is turned (through the weapon module's rotational seat)
 * until the right hand holds it this way, which points every weapon where the controller points.
 * Measured in Io on the mock (2026-10-15); `canon learn` re-measures it from the weapon in hand.
 */
M3 g_canon = mat_of({0.08357F, -0.35772F, -0.02561F, 0.92973F});
bool g_canonSet = true;
std::uint64_t g_canonSteadySince = 0;
M3 g_seatQ{};
float g_seatAngle = 0.0F;
std::uint64_t g_seatBadSince = 0;
std::atomic_bool g_rotSeat{true};
Overlay g_overlayData{};
/** Body yaw, smoothed on the present thread and read by the draws. */
std::atomic<float> g_bodyYaw{0.0F};
std::atomic_bool g_bodyYawValid{false};

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
void rebuild_rig() noexcept {
    Rig rig{};
    Labels labels = g_labels;
    // Which hand is which comes from the bind pose: the game's +Y is LEFT.
    if (centroid(labels.side[left].hand).y < centroid(labels.side[right].hand).y) {
        std::swap(labels.side[left], labels.side[right]);
    }
    bool ok = true;
    for (int s = 0; s < 2; ++s) {
        for (int j = 0; j < 3; ++j) {
            const auto pr = joint_pair(labels.side[static_cast<std::size_t>(s)], j);
            double mass = 0.0;
            V3 joint{};
            if (!pair_joint(pr[0], pr[1], joint, mass)) {
                ok = false;
                continue;
            }
            rig.prior[static_cast<std::size_t>(s)][static_cast<std::size_t>(j)] = joint;
        }
    }
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
    xr::frame_sample(in.head, in.left, in.right);
    if (!in.head.valid) {
        return false;
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

    // The body: a frame hung from the neck and turned to the body's heading.
    const V3 f = to_v3(in.head.forward);
    const V3 u = to_v3(in.head.up);
    const V3 neck = -(f * params.neckFwd) - u * params.neckUp;
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

    if (in.right.valid) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_grip.frame != frame) {
            g_grip.frame = frame;
            const Affine now = inverse_rigid(ctrlR) * animated[right];
            if (!g_grip.valid) {
                g_grip.rel = now;
                g_grip.valid = true;
            } else {
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
            const float moved = length(ctrlR.t - g_lastCtrlR);
            g_lastCtrlR = ctrlR.t;
            if (moved < 0.004F) {
                const V3 e = transpose(ctrlR.m) * (palmDrawn[right] - ctrlR.t);
                weapon::seat_error(e.x, -e.y, e.z);
                g_seatError = length(e);
                // The rotational seat. With the viewmodel turned by Q in the controller frame, the
                // hand's rotation there is Rrel = Q V (V: the weapon's own authored hold), so the Q
                // that makes it canonical is canon * Rrel^T * Q.
                const M3 relNow = transpose(ctrlR.m) * animated[right].m;
                if (!g_canonSet) {
                    const std::uint64_t nowMs = GetTickCount64();
                    if (g_canonSteadySince == 0) {
                        g_canonSteadySince = nowMs;
                    } else if (nowMs - g_canonSteadySince > 2000) {
                        g_canon = transpose(g_seatQ) * relNow;
                        g_canonSet = true;
                        const auto q = quat_of(g_canon);
                        reportf("ev=vr.arms canon learned q=%.5f,%.5f,%.5f,%.5f", q[0], q[1], q[2], q[3]);
                    }
                } else if (g_rotSeat.load(std::memory_order_relaxed) && g_seatError < 0.15F) {
                    // Only while the palm is already near the controller: that is the weapon
                    // actually being held. Turning the gun on the strength of a hand that is
                    // somewhere else (a draw animation, the first frames) is how a seat loop
                    // talks itself into a wrong pose.
                    const M3 qTarget = orthonormalize(g_canon * transpose(relNow) * g_seatQ);
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
            if (moved < 0.004F && g_seatError > 0.25F) {
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
                m = target[left] * left_finger(rig, bones, boneCount, animated[right], b, grip);
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
        } else if (std::strcmp(verb, "canon") == 0 && n >= 2) {
            // canon learn|on|off|reset
            if (std::strcmp(a, "learn") == 0) {
                g_canonSet = false;
                g_canonSteadySince = 0;
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
        reportf("ev=vr.arms command %s", verb);
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

/** The back buffer's render target view, cached against the texture it was made for. */
ID3D11RenderTargetView* back_buffer_view(ID3D11Device* device, IDXGISwapChain* swapChain) noexcept {
    static ID3D11Texture2D* s_texture = nullptr;
    static ID3D11RenderTargetView* s_view = nullptr;
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))
        || texture == nullptr) {
        return nullptr;
    }
    if (texture != s_texture) {
        if (s_view != nullptr) {
            s_view->Release();
            s_view = nullptr;
        }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
        viewDesc.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                          : desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                         : desc.Format;
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateRenderTargetView(texture, &viewDesc, &s_view))) {
            s_view = nullptr;
        }
        s_texture = texture;
    }
    texture->Release();
    return s_view;
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
    ID3D11RenderTargetView* const target = back_buffer_view(device, swapChain);
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

} // namespace sunrise::client::hooks::vr::arms
