/**
 * The Guardian's own body in third person. See vr_body.h; docs/SWORD-SPARROW.md for the evidence.
 */

#include "vr_body.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <unordered_map>

#include "../../../core/logging/log.h"
#include "runtime.h"
#include "vr_arms.h"
#include "vr_gpu.h"
#include "vr_math.h"
#include "vr_mode.h"
#include "vr_state.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::body {
namespace {

using math::Affine;
using math::M3;
using math::V3;

constexpr std::size_t kHeaderFloats = 8 * 4;
constexpr std::size_t kMaxBones = 104;
/** The Guardian's pieces are prefixes of one skeleton; anything under this is an object, not armour. */
constexpr std::size_t kMinPieceBones = 12;
/** The gloves carry every finger: the whole skeleton. */
constexpr std::size_t kGloveBones = 60;
/** Small rigid objects (a weapon's parts): a sword is one bone per mesh. */
constexpr std::size_t kMaxObjectBones = 8;
/** How near the game's hand a sword's root sits (7.8 cm from the wrist on Worldline Zero). */
constexpr float kSwordReach = 0.35F;

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

V3 v3(const float* p) noexcept {
    return V3{p[0], p[1], p[2]};
}

bool rigid(const float* r) noexcept {
    const V3 c0{r[0], r[4], r[8]};
    const V3 c1{r[1], r[5], r[9]};
    const V3 c2{r[2], r[6], r[10]};
    return std::fabs(math::dot(c0, c0) - 1.0F) < 0.05F && std::fabs(math::dot(c1, c1) - 1.0F) < 0.05F
           && std::fabs(math::dot(c2, c2) - 1.0F) < 0.05F && std::fabs(math::dot(c0, c1)) < 0.05F
           && std::fabs(math::dot(c0, c2)) < 0.05F && std::fabs(math::dot(c1, c2)) < 0.05F && std::isfinite(r[3])
           && std::isfinite(r[7]) && std::isfinite(r[11]);
}

// --- What a buffer is, learned from the draws that bind it -----------------------------------------

enum class Role : std::uint8_t { unknown, arm, hide };
SRWLOCK g_roleLock = SRWLOCK_INIT;
std::unordered_map<const void*, Role> g_roles;
/** The Guardian's pieces seen this frame, for the draws to teach their role. */
std::array<const void*, 16> g_family{};
std::atomic<std::uint32_t> g_familyCount{0};

Role role_of(const void* resource) noexcept {
    AcquireSRWLockShared(&g_roleLock);
    const auto found = g_roles.find(resource);
    const Role role = found != g_roles.end() ? found->second : Role::unknown;
    ReleaseSRWLockShared(&g_roleLock);
    return role;
}

// --- The game's hands, from the frame's gloves -----------------------------------------------------

struct GameHands final {
    std::uint64_t serial{};
    std::uint32_t present{};
    Affine root{};
    std::array<Affine, 2> hand{};
    std::array<V3, 2> palm{};
    bool valid{};
};
SRWLOCK g_handsLock = SRWLOCK_INIT;
GameHands g_hands{};

/** The sword in the game's right hand (palette space), learned from the frames that show both. */
SRWLOCK g_swordLock = SRWLOCK_INIT;
Affine g_swordG{};
bool g_swordGValid = false;
std::atomic<ULONGLONG> g_swordSeen{0};
std::atomic<int> g_swordHand{1};

/** The handlebar weights (present thread writes, render thread reads), and the steering they make. */
std::array<std::atomic<float>, 2> g_bar{};
std::atomic<float> g_barSteer{0.0F};
/** The other hand on the sword's hilt (0..1, blended in and out). */
std::atomic<float> g_twoHand{0.0F};
std::atomic_bool g_twoHandHeld{false};
std::atomic_bool g_barHeld{false};

std::atomic<std::uint32_t> g_presents{0};
/** Headers (scale, origin) of the pieces hidden recently, for their CPU-simulated cloth (hidden_cloth). */
struct HiddenHeader final {
    std::array<float, 4> h{};
    std::uint32_t present{};
};
SRWLOCK g_hiddenLock = SRWLOCK_INIT;
std::array<HiddenHeader, 12> g_hiddenHeaders{};
std::atomic<std::uint64_t> g_clothSkips{0};
/** The buffers holding an arm piece posed this frame (pooled buffers: dropped the moment they hold else). */
SRWLOCK g_armBufLock = SRWLOCK_INIT;
std::array<const void*, 8> g_armBufs{};

void set_arm_buffer(const void* resource, bool arm) noexcept {
    AcquireSRWLockExclusive(&g_armBufLock);
    const void** free = nullptr;
    bool done = false;
    for (const void*& b : g_armBufs) {
        if (b == resource) {
            if (!arm) {
                b = nullptr;
            }
            done = true;
            break;
        }
        if (b == nullptr && free == nullptr) {
            free = &b;
        }
    }
    if (!done && arm && free != nullptr) {
        *free = resource;
    }
    ReleaseSRWLockExclusive(&g_armBufLock);
}

void note_hidden(const float* header) noexcept {
    const std::uint32_t now = g_presents.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_hiddenLock);
    HiddenHeader* slot = &g_hiddenHeaders[0];
    for (HiddenHeader& e : g_hiddenHeaders) {
        if (std::fabs(e.h[0] - header[16]) < 1e-5F && std::fabs(e.h[1] - header[20]) < 1e-5F && std::fabs(e.h[2] - header[21]) < 1e-5F
            && std::fabs(e.h[3] - header[22]) < 1e-5F) {
            slot = &e;
            break;
        }
        if (e.present < slot->present) {
            slot = &e;
        }
    }
    slot->h = {header[16], header[20], header[21], header[22]};
    slot->present = now;
    ReleaseSRWLockExclusive(&g_hiddenLock);
}
/** `trace <n>` in SVR_Body.txt: the next n world palettes examined, one log line each (research). */
std::atomic<int> g_traceLeft{0};
std::atomic<std::uint64_t> g_examined{0};
std::atomic<std::uint64_t> g_unmaps{0};
std::atomic<std::uint64_t> g_maps{0};
struct Stats final {
    std::atomic<std::uint64_t> armed{0};
    std::atomic<std::uint64_t> hidden{0};
    std::atomic<std::uint64_t> swords{0};
    std::atomic<std::uint64_t> stowed{0};
    std::atomic<std::uint64_t> unmatched{0};
    std::atomic<std::uint64_t> swordBeforeGloves{0};
    std::atomic<std::uint64_t> noRig{0};
};
Stats g_stats{};

void collapse(float* data, std::size_t capacity) noexcept {
    std::memset(data + kHeaderFloats, 0, capacity * 12 * sizeof(float));
}

/** The context the arms are posed with, for one record. */
arms::FreeContext context_for(const mode::Record& rec) noexcept {
    arms::FreeContext ctx{};
    ctx.head = rec.head;
    ctx.left = rec.left;
    ctx.right = rec.right;
    if (rec.mode == mode::Mode::vehicle) {
        ctx.bodyYaw = rec.vehicleYaw;
    } else {
        // The head's heading: the shoulders face where the player faces.
        const V3 f = v3(rec.head.forward.data());
        const V3 u = v3(rec.head.up.data());
        V3 h{f.x, f.y, 0.0F};
        if (math::length(h) < 0.3F) {
            h = f.z < 0.0F ? V3{u.x, u.y, 0.0F} : V3{-u.x, -u.y, 0.0F};
        }
        ctx.bodyYaw = std::atan2(h.y, h.x);
    }
    const bool sword = rec.mode == mode::Mode::onFoot && GetTickCount64() - g_swordSeen.load(std::memory_order_relaxed) < 500;
    AcquireSRWLockShared(&g_swordLock);
    ctx.swordValid = sword && g_swordGValid;
    ctx.sword = g_swordG;
    ReleaseSRWLockShared(&g_swordLock);
    ctx.swordHand = sword ? g_swordHand.load(std::memory_order_relaxed) : -1;
    ctx.twoHand = sword ? g_twoHand.load(std::memory_order_relaxed) : 0.0F;
    if (rec.mode == mode::Mode::vehicle) {
        ctx.bar[0] = g_bar[0].load(std::memory_order_relaxed);
        ctx.bar[1] = g_bar[1].load(std::memory_order_relaxed);
    }
    return ctx;
}

/** The camera-relative copy of `n` bones, translations moved by -camera. */
void to_relative(const float* bones, std::size_t n, const V3& camera, float* out) noexcept {
    std::memcpy(out, bones, n * 12 * sizeof(float));
    for (std::size_t b = 0; b < n; ++b) {
        out[b * 12 + 3] -= camera.x;
        out[b * 12 + 7] -= camera.y;
        out[b * 12 + 11] -= camera.z;
    }
}

void to_world(float* bones, std::size_t n, const V3& camera) noexcept {
    for (std::size_t b = 0; b < n; ++b) {
        bones[b * 12 + 3] += camera.x;
        bones[b * 12 + 7] += camera.y;
        bones[b * 12 + 11] += camera.z;
    }
}

/** One of the Guardian's pieces. */
void examine_piece(ID3D11Resource* resource, float* data, float* local, std::size_t n, std::size_t capacity,
                   const mode::Record& rec) noexcept {
    float* const bones = local + kHeaderFloats;
    const Affine root = math::read_rows(bones);
    if (rec.mode == mode::Mode::vehicle) {
        mode::note_rider_offset(math::inverse_rigid(rec.vehicle) * root.t);
    }
    const bool gloves = n >= kGloveBones;
    if (gloves) {
        // The game's own hands this frame: the sword's reference, and on a sparrow the handlebars.
        int hl = -1;
        int hr = -1;
        V3 pl{};
        V3 pr{};
        if (arms::hand_bones(hl, hr) && arms::palm_points(bones, n, pl, pr)) {
            GameHands h{};
            h.serial = rec.serial;
            h.present = g_presents.load(std::memory_order_relaxed);
            h.root = root;
            h.hand[0] = math::read_rows(bones + static_cast<std::size_t>(hl) * 12);
            h.hand[1] = math::read_rows(bones + static_cast<std::size_t>(hr) * 12);
            h.palm = {pl, pr};
            h.valid = true;
            AcquireSRWLockExclusive(&g_handsLock);
            g_hands = h;
            ReleaseSRWLockExclusive(&g_handsLock);
            if (rec.mode == mode::Mode::vehicle) {
                const Affine inv = math::inverse_rigid(rec.vehicle);
                mode::note_handlebars(inv * pl, inv * pr);
            }
        }
    }
    // The draws tell which pieces carry the arms (the meshes the first-person viewmodel draws).
    const std::uint32_t at = g_familyCount.load(std::memory_order_relaxed);
    if (at < g_family.size()) {
        bool known = false;
        for (std::uint32_t i = 0; i < at; ++i) {
            known = known || g_family[i] == resource;
        }
        if (!known) {
            g_family[at] = resource;
            g_familyCount.store(at + 1, std::memory_order_relaxed);
        }
    }
    // An arm piece is one whose mesh the first-person viewmodel draws as an arm: the same dequantisation
    // scale in the header. Never by buffer: the engine pools these and hands a piece's buffer to another
    // object the next frame (measured: an "arm" buffer holding a 24-bone object 200 m away).
    const float scale = local[16];
    const Role role = gloves || gpu::is_arm_scale(scale) ? Role::arm : Role::hide;
    static_cast<void>(resource);
    if (role == Role::arm && arms::free_ready()) {
        static thread_local std::array<float, 12 * kMaxBones> s_rel{};
        static thread_local std::array<float, 12 * kMaxBones> s_out{};
        to_relative(bones, n, rec.camera, s_rel.data());
        const arms::FreeContext ctx = context_for(rec);
        if (arms::rewrite_free(s_rel.data(), n, s_out.data(), ctx)) {
            to_world(s_out.data(), n, rec.camera);
            std::memcpy(data + kHeaderFloats, s_out.data(), n * 12 * sizeof(float));
            set_arm_buffer(resource, true);
            g_stats.armed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (role == Role::arm) {
        // The rig is learned in first person: until then the arms are not drawn rather than drawn wrong.
        g_stats.noRig.fetch_add(1, std::memory_order_relaxed);
    }
    collapse(data, capacity);
    note_hidden(local);
    g_stats.hidden.fetch_add(1, std::memory_order_relaxed);
}

/**
 * The weapon in hand, one entry per mesh (a sword is a hilt and a blade; a super's weapon may be more),
 * told apart by the mesh's own dequantisation scale. A mesh's hold in the game's right hand is learned once
 * it has stayed put for kHoldStableFrames -- a sword is fixed in the fist, but the Arc Staff is spun in it
 * by the super's animations, and a hold relearned whenever the game spun it made the staff jump (measured
 * 2026-10-18) -- and then kept for as long as the weapon is out. Until then that mesh is not drawn rather
 * than drawn where the hidden body's hand is.
 */
constexpr int kHoldStableFrames = 15;
struct HeldPart final {
    float scale{};
    Affine hold{};
    Affine candidate{};
    int stable{};
    bool frozen{};
};
std::array<HeldPart, 6> g_parts{};

bool close_holds(const Affine& a, const Affine& b) noexcept {
    return math::length(a.t - b.t) < 0.03F && math::dot(a.m.c[0], b.m.c[0]) > 0.996F && math::dot(a.m.c[1], b.m.c[1]) > 0.996F
           && math::dot(a.m.c[2], b.m.c[2]) > 0.996F;
}

/** A small rigid object beside the Guardian: the weapon in hand, something carried on the body, or neither. */
void examine_object(float* data, float* local, std::size_t n, std::size_t capacity) noexcept {
    float* const bones = local + kHeaderFloats;
    const Affine m0 = math::read_rows(bones);
    const float scale = local[16];
    AcquireSRWLockShared(&g_handsLock);
    const GameHands hands = g_hands;
    ReleaseSRWLockShared(&g_handsLock);
    if (!hands.valid) {
        return;
    }
    const float toRight = math::length(m0.t - hands.palm[1]);
    const float toLeft = math::length(m0.t - hands.palm[0]);
    if (std::min(toRight, toLeft) < kSwordReach) {
        const bool sameFrame = hands.present == g_presents.load(std::memory_order_relaxed);
        if (!sameFrame) {
            g_stats.swordBeforeGloves.fetch_add(1, std::memory_order_relaxed);
        }
        if (GetTickCount64() - g_swordSeen.exchange(GetTickCount64(), std::memory_order_relaxed) > 1500) {
            // Drawn (again): a pulse in the hand that holds it, and every hold learned afresh.
            AcquireSRWLockExclusive(&g_swordLock);
            g_parts = {};
            g_swordGValid = false;
            ReleaseSRWLockExclusive(&g_swordLock);
            xr::haptic(g_swordHand.load(std::memory_order_relaxed) == 1, 0.6F, 0.05F);
            report("ev=vr.body sword drawn");
        }
        const Affine g = math::inverse_rigid(Affine{math::orthonormalize(hands.hand[1].m), hands.hand[1].t})
                         * Affine{math::orthonormalize(m0.m), m0.t};
        AcquireSRWLockExclusive(&g_swordLock);
        HeldPart* part = nullptr;
        for (HeldPart& h : g_parts) {
            if (h.scale != 0.0F && std::fabs(h.scale - scale) < 1e-4F) {
                part = &h;
                break;
            }
        }
        if (part == nullptr) {
            for (HeldPart& h : g_parts) {
                if (h.scale == 0.0F) {
                    part = &h;
                    part->scale = scale;
                    part->candidate = g;
                    part->stable = 0;
                    break;
                }
            }
        }
        if (part != nullptr && !part->frozen && sameFrame) {
            if (close_holds(g, part->candidate)) {
                part->candidate.t = part->candidate.t + (g.t - part->candidate.t) * 0.2F;
                part->candidate.m = math::orthonormalize(math::blend(part->candidate.m, g.m, 0.2F));
                ++part->stable;
            } else {
                part->candidate = g;
                part->stable = 1;
            }
            if (part->stable >= kHoldStableFrames) {
                part->hold = part->candidate;
                part->frozen = true;
                if (!g_swordGValid) {
                    // The first mesh held still is the weapon's reference: its +Z is the blade.
                    g_swordG = part->hold;
                    g_swordGValid = true;
                }
                reportf("ev=vr.body sword hold scale=%.4f t=%.3f,%.3f,%.3f blade=%.3f,%.3f,%.3f", scale, part->hold.t.x,
                        part->hold.t.y, part->hold.t.z, part->hold.m.c[2].x, part->hold.m.c[2].y, part->hold.m.c[2].z);
            }
        }
        const bool frozen = part != nullptr && part->frozen;
        const Affine hold = frozen ? part->hold : Affine{};
        ReleaseSRWLockExclusive(&g_swordLock);
        mode::Record rec{};
        const bool haveRecord = mode::record_by_serial(hands.serial, rec);
        const arms::FreeContext ctx = haveRecord ? context_for(rec) : arms::FreeContext{};
        Affine handBone{};
        if (!frozen || !haveRecord || !ctx.swordValid || !arms::free_hand_bone(ctx, ctx.swordHand, handBone)) {
            collapse(data, capacity);
            return;
        }
        Affine g2 = hold;
        if (ctx.swordHand == 0) {
            auto flip = [](const V3& v) { return V3{v.x, -v.y, v.z}; };
            g2 = Affine{M3{{flip(g2.m.c[0]), -flip(g2.m.c[1]), flip(g2.m.c[2])}}, flip(g2.t)};
        }
        Affine target = handBone * g2;
        target.t = target.t + rec.camera;
        // Measured, not assumed: the blade against the fist's axis, the hilt against the palm.
        static std::uint32_t s_placed = 0;
        if ((++s_placed % 120) == 1) {
            const xr::HandPose& hand = ctx.swordHand == 1 ? rec.right : rec.left;
            const V3 want = math::normalize(v3(hand.gripForward.data()));
            const V3 blade = math::normalize(target.m.c[2]);
            const V3 palm = rec.camera + v3(hand.palm.data()) - v3(rec.head.offset.data());
            reportf("ev=vr.body sword_pose hand=%s blade_deg=%.2f hilt_to_palm_mm=%.1f same_frame=%d",
                    ctx.swordHand == 1 ? "right" : "left",
                    std::acos(std::clamp(math::dot(want, blade), -1.0F, 1.0F)) * 57.29578F,
                    math::length(target.t - palm) * 1000.0F, sameFrame ? 1 : 0);
        }
        const Affine move = target * math::inverse_rigid(Affine{math::orthonormalize(m0.m), m0.t});
        for (std::size_t b = 0; b < n; ++b) {
            const Affine moved = move * math::read_rows(bones + b * 12);
            math::write_rows(moved, data + kHeaderFloats + b * 12);
        }
        g_stats.swords.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Anything else carried on the body (the weapon stowed on the back, one at the hip during a super):
    // part of the body that is not drawn. The body's volume, in the root's frame; what lies on the ground
    // (an engram, ammunition) is below it.
    // At the mesh's centre (the header's origin, carried by bone 0): a skinned piece's bone 0 is the
    // feet, below the volume, while its mesh is not.
    const V3 centre = m0 * V3{local[20], local[21], local[22]};
    const V3 inRoot = math::inverse_rigid(Affine{math::orthonormalize(hands.root.m), hands.root.t}) * centre;
    if (std::sqrt(inRoot.x * inRoot.x + inRoot.y * inRoot.y) < 0.7F && inRoot.z > 0.3F && inRoot.z < 2.1F) {
        collapse(data, capacity);
        note_hidden(local);
        g_stats.stowed.fetch_add(1, std::memory_order_relaxed);
    }
}

void poll_commands() noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return;
    }
    wchar_t* const slash = std::wcsrchr(path.data(), L'\\');
    if (slash == nullptr || wcscpy_s(slash + 1, path.size() - static_cast<std::size_t>(slash + 1 - path.data()), L"SVR_Body.txt") != 0) {
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
        const int n = sscanf_s(line.data(), "%15s %15s", verb, static_cast<unsigned>(sizeof verb), a, static_cast<unsigned>(sizeof a));
        if (n >= 2 && std::strcmp(verb, "hand") == 0) {
            set_sword_hand(std::strcmp(a, "left") == 0 ? 0 : 1);
        } else if (n >= 1 && std::strcmp(verb, "dump") == 0) {
            reportf("ev=vr.body dump sword_valid=%d hand=%d roles=%zu", g_swordGValid ? 1 : 0, sword_hand(), g_roles.size());
        } else if (n >= 2 && std::strcmp(verb, "trace") == 0) {
            g_traceLeft.store(std::atoi(a), std::memory_order_relaxed);
        } else if (n >= 1 && std::strcmp(verb, "forget") == 0) {
            AcquireSRWLockExclusive(&g_roleLock);
            g_roles.clear();
            ReleaseSRWLockExclusive(&g_roleLock);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

} // namespace

bool wanted() noexcept {
    const mode::Mode m = mode::current();
    return (m == mode::Mode::onFoot || m == mode::Mode::vehicle) && enabled() && state::view() == state::View::immersive;
}

void examine(ID3D11Resource* resource, float* data, std::size_t bytes) noexcept {
    g_unmaps.fetch_add(1, std::memory_order_relaxed);
    // Whatever this buffer held, it holds something new now: an arm piece again only if posed below.
    set_arm_buffer(resource, false);
    if (data == nullptr || bytes < (kHeaderFloats + 12) * sizeof(float)) {
        return;
    }
    const std::size_t capacity = std::min(kMaxBones, (bytes / sizeof(float) - kHeaderFloats) / 12);
    // The mapped memory may be write-combined (slow to read): the header first, and the bones only for
    // what is a palette at all -- these buffers are every object's, not just the Guardian's.
    static thread_local std::array<float, kHeaderFloats + 12 * kMaxBones> s_local{};
    std::memcpy(s_local.data(), data, kHeaderFloats * sizeof(float));
    const float* h = s_local.data();
    const float scale = h[16];
    if (!(scale > 0.0F) || std::fabs(h[17] - scale) > 1e-4F || std::fabs(h[18] - scale) > 1e-4F || h[19] != 0.0F) {
        return;
    }
    std::memcpy(s_local.data() + kHeaderFloats, data + kHeaderFloats, 12 * capacity * sizeof(float));
    const float* bones = s_local.data() + kHeaderFloats;
    std::size_t n = 0;
    while (n < capacity && rigid(bones + n * 12)) {
        ++n;
    }
    if (n == 0) {
        return;
    }
    const V3 root{bones[3], bones[7], bones[11]};
    const mode::Mode m = mode::current();
    g_examined.fetch_add(1, std::memory_order_relaxed);
    if (g_traceLeft.load(std::memory_order_relaxed) > 0) {
        g_traceLeft.fetch_sub(1, std::memory_order_relaxed);
        mode::Record nearest{};
        const bool found = mode::record_by_rider(root, 1000.0F, nearest);
        reportf("ev=vr.body trace buf=%p bytes=%zu n=%zu root=%.3f,%.3f,%.3f rec=%llu rider=%.3f,%.3f,%.3f d=%.3f present=%u",
                static_cast<void*>(resource), bytes, n, root.x, root.y, root.z,
                static_cast<unsigned long long>(found ? nearest.serial : 0), nearest.rider.x, nearest.rider.y, nearest.rider.z,
                found ? math::length(nearest.rider - root) : -1.0F, g_presents.load(std::memory_order_relaxed));
    }
    if (n >= 2 && n < kMinPieceBones) {
        // A small piece of the Guardian's own: the Titan's mark is six bones, the skeleton's first six,
        // rooted at the feet (2026-10-18 capture) -- under the size of an armour piece, and its cloth
        // escaped with it. Its bone 0 IS the frame's root: to the centimetre, and turned as the root is.
        mode::Record small{};
        if (mode::record_by_rider(root, 0.02F, small)) {
            AcquireSRWLockShared(&g_handsLock);
            const GameHands hands = g_hands;
            ReleaseSRWLockShared(&g_handsLock);
            const Affine m0 = math::read_rows(bones);
            const bool turned = !hands.valid
                                || (math::dot(math::normalize(m0.m.c[0]), math::normalize(hands.root.m.c[0])) > 0.9986F
                                    && math::dot(math::normalize(m0.m.c[2]), math::normalize(hands.root.m.c[2])) > 0.9986F);
            if (turned) {
                examine_piece(resource, data, s_local.data(), n, capacity, small);
                return;
            }
        }
    }
    if (n >= kMinPieceBones) {
        mode::Record rec{};
        const float tolerance = m == mode::Mode::vehicle ? 0.6F : 0.35F;
        if (mode::record_by_rider(root, tolerance, rec)) {
            examine_piece(resource, data, s_local.data(), n, capacity, rec);
        } else {
            g_stats.unmatched.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (m == mode::Mode::onFoot && n <= kMaxObjectBones) {
        examine_object(data, s_local.data(), n, capacity);
    }
}

bool posed_arm_buffer(const void* palette) noexcept {
    if (palette == nullptr) {
        return false;
    }
    AcquireSRWLockShared(&g_armBufLock);
    bool found = false;
    for (const void* b : g_armBufs) {
        found = found || b == palette;
    }
    ReleaseSRWLockShared(&g_armBufLock);
    return found;
}

bool hidden_cloth(const float* rows) noexcept {
    if (rows == nullptr) {
        return false;
    }
    // Rows 4-5 first (the header): almost every object's differs from the few hidden pieces'.
    std::array<float, 8> head{};
    std::memcpy(head.data(), rows + 16, sizeof head);
    if (!(head[0] > 0.0F) || head[1] != head[0] || head[2] != head[0]) {
        return false;
    }
    const std::uint32_t now = g_presents.load(std::memory_order_relaxed);
    bool match = false;
    AcquireSRWLockShared(&g_hiddenLock);
    for (const HiddenHeader& e : g_hiddenHeaders) {
        match = match
                || (e.present + 2 >= now && e.h[0] > 0.0F && std::fabs(e.h[0] - head[0]) < 1e-5F && std::fabs(e.h[1] - head[4]) < 1e-5F
                    && std::fabs(e.h[2] - head[5]) < 1e-5F && std::fabs(e.h[3] - head[6]) < 1e-5F);
    }
    ReleaseSRWLockShared(&g_hiddenLock);
    if (!match) {
        return false;
    }
    // Then the world matrix: identity, the cloth being already in world space.
    std::array<float, 16> m{};
    std::memcpy(m.data(), rows, sizeof m);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(m[static_cast<std::size_t>(r * 4 + c)] - (r == c ? 1.0F : 0.0F)) > 1e-5F) {
                return false;
            }
        }
    }
    g_clothSkips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void note_map(unsigned rowPitch) noexcept {
    static_cast<void>(rowPitch);
    g_maps.fetch_add(1, std::memory_order_relaxed);
}

void note_draw(ID3D11Resource* palette, bool armMesh) noexcept {
    if (palette == nullptr) {
        return;
    }
    const std::uint32_t count = g_familyCount.load(std::memory_order_relaxed);
    bool family = false;
    for (std::uint32_t i = 0; i < count && i < g_family.size(); ++i) {
        family = family || g_family[i] == palette;
    }
    if (!family) {
        return;
    }
    // A piece is an arm piece once any of its meshes is one of the viewmodel's arm meshes.
    const Role now = role_of(palette);
    if (now == Role::arm || (now == Role::hide && !armMesh)) {
        return;
    }
    AcquireSRWLockExclusive(&g_roleLock);
    g_roles[palette] = armMesh ? Role::arm : Role::hide;
    ReleaseSRWLockExclusive(&g_roleLock);
    reportf("ev=vr.body piece buffer=%p role=%s", static_cast<void*>(palette), armMesh ? "arm" : "hide");
}

void present() noexcept {
    const std::uint32_t frame = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    g_familyCount.store(0, std::memory_order_relaxed);
    if ((frame % 30) == 0) {
        poll_commands();
    }
    // The handlebars: a hand gripping near the one the game's rider holds takes it, and turning the
    // bars (the line between the hands) or pushing one forward steers.
    mode::Record rec{};
    V3 barL{};
    V3 barR{};
    const xr::InputState in = xr::input_state();
    const bool riding = mode::current() == mode::Mode::vehicle && mode::latest_record(rec) && rec.mode == mode::Mode::vehicle
                        && mode::handlebars(barL, barR);
    static std::array<V3, 2> s_grab{};
    static std::array<bool, 2> s_held{};
    float steer = 0.0F;
    int held = 0;
    for (std::size_t s = 0; s < 2; ++s) {
        float w = g_bar[s].load(std::memory_order_relaxed);
        bool want = false;
        if (riding) {
            const xr::HandPose& hand = s == 0 ? rec.left : rec.right;
            const V3 palmWorld = rec.camera + v3(hand.palm.data()) - v3(rec.head.offset.data());
            const V3 inVehicle = math::inverse_rigid(rec.vehicle) * palmWorld;
            const V3 bar = s == 0 ? barL : barR;
            const float grip = s == 0 ? in.gripL : in.gripR;
            const bool wasHeld = s_held[s];
            const float reach = wasHeld ? 0.45F : 0.22F;
            want = hand.valid && grip > (wasHeld ? 0.35F : 0.55F) && math::length(inVehicle - bar) < reach;
            if (want && !wasHeld) {
                s_grab[s] = inVehicle;
                xr::haptic(s == 1, 0.35F, 0.03F);
                reportf("ev=vr.body bar grab hand=%s dist=%.3f", s == 1 ? "right" : "left", math::length(inVehicle - bar));
            } else if (!want && wasHeld) {
                reportf("ev=vr.body bar release hand=%s", s == 1 ? "right" : "left");
            }
            if (want) {
                // Pushing the right bar forward (or pulling the left back) turns left.
                const float push = (inVehicle.x - s_grab[s].x) / 0.12F;
                steer += s == 1 ? -push : push;
                ++held;
            }
        }
        if (!riding && s_held[s]) {
            reportf("ev=vr.body bar release hand=%s", s == 1 ? "right" : "left");
        }
        s_held[s] = want;
        w = std::clamp(w + (want ? 0.12F : -0.12F), 0.0F, 1.0F);
        g_bar[s].store(w, std::memory_order_relaxed);
    }
    g_barSteer.store(held > 0 ? std::clamp(steer / static_cast<float>(held), -1.0F, 1.0F) : 0.0F, std::memory_order_relaxed);
    g_barHeld.store(held > 0, std::memory_order_relaxed);

    // Two hands on the hilt: the free hand's grip closed on the line below the sword hand (where a
    // longsword's hilt runs, back from the fist along its axis) takes it, and holds while the grip does.
    {
        const bool sword = sword_held() && mode::current() == mode::Mode::onFoot;
        const int main = g_swordHand.load(std::memory_order_relaxed);
        bool want = false;
        if (sword) {
            const xr::HandPose mh = xr::hand_pose(main == 1);
            const xr::HandPose sh = xr::hand_pose(main != 1);
            const float grip = main == 1 ? in.gripL : in.gripR;
            const bool wasHolding = g_twoHandHeld.load(std::memory_order_relaxed);
            if (mh.valid && sh.valid) {
                const V3 axis = math::normalize(v3(mh.gripForward.data()), v3(mh.forward.data()));
                const V3 d = v3(sh.palm.data()) - v3(mh.palm.data());
                const float along = -math::dot(d, axis);
                const float off = math::length(d + axis * along);
                const bool onHilt = along > 0.03F && along < (wasHolding ? 0.40F : 0.30F) && off < (wasHolding ? 0.25F : 0.12F);
                want = onHilt && grip > (wasHolding ? 0.35F : 0.55F);
                if (want && !wasHolding) {
                    xr::haptic(main != 1, 0.4F, 0.03F);
                    reportf("ev=vr.body two_hand take along=%.3f off=%.3f", along, off);
                } else if (!want && wasHolding) {
                    reportf("ev=vr.body two_hand release");
                }
            }
        }
        g_twoHandHeld.store(want, std::memory_order_relaxed);
        const float w = g_twoHand.load(std::memory_order_relaxed);
        g_twoHand.store(std::clamp(w + (want ? 0.15F : -0.15F), 0.0F, 1.0F), std::memory_order_relaxed);
    }

    // The sword's weight, suggested in the hand: a hum that rises with the speed of the swing.
    if (sword_held() && mode::current() == mode::Mode::onFoot) {
        const int handIndex = g_swordHand.load(std::memory_order_relaxed);
        const xr::HandPose hand = xr::hand_pose(handIndex == 1);
        static V3 s_last{};
        static bool s_lastValid = false;
        if (hand.valid) {
            const V3 p = v3(hand.palm.data()) + v3(hand.gripForward.data()) * 0.9F;
            if (s_lastValid) {
                const float speed = math::length(p - s_last) * 72.0F;
                if (speed > 2.0F) {
                    xr::haptic(handIndex == 1, std::clamp((speed - 2.0F) / 6.0F, 0.08F, 0.7F), 0.025F);
                }
            }
            s_last = p;
            s_lastValid = true;
        } else {
            s_lastValid = false;
        }
    }
    if ((frame % 600) == 0 && wanted()) {
        reportf("ev=vr.body stats maps=%llu unmaps=%llu examined=%llu mode=%s armed=%llu hidden=%llu swords=%llu stowed=%llu unmatched=%llu sword_before_gloves=%llu "
                "no_rig=%llu sword_hold=%d hand=%d bars=%.2f,%.2f cloth_skips=%llu",
                static_cast<unsigned long long>(g_maps.load()), static_cast<unsigned long long>(g_unmaps.load()),
                static_cast<unsigned long long>(g_examined.load()),
                mode::name(mode::current()),
                static_cast<unsigned long long>(g_stats.armed.load()),
                static_cast<unsigned long long>(g_stats.hidden.load()), static_cast<unsigned long long>(g_stats.swords.load()),
                static_cast<unsigned long long>(g_stats.stowed.load()), static_cast<unsigned long long>(g_stats.unmatched.load()),
                static_cast<unsigned long long>(g_stats.swordBeforeGloves.load()),
                static_cast<unsigned long long>(g_stats.noRig.load()), g_swordGValid ? 1 : 0, sword_hand(),
                g_bar[0].load(), g_bar[1].load(), static_cast<unsigned long long>(g_clothSkips.load()));
    }
}

int sword_hand() noexcept {
    return g_swordHand.load(std::memory_order_relaxed);
}

void set_sword_hand(int hand) noexcept {
    const int was = g_swordHand.exchange(hand == 0 ? 0 : 1, std::memory_order_relaxed);
    if (was != hand) {
        xr::haptic(hand == 1, 0.5F, 0.04F);
        reportf("ev=vr.body sword hand=%s", hand == 0 ? "left" : "right");
    }
}

bool two_handed() noexcept {
    return g_twoHandHeld.load(std::memory_order_relaxed);
}

bool sword_held() noexcept {
    return GetTickCount64() - g_swordSeen.load(std::memory_order_relaxed) < 500;
}

float handlebar_steer(bool& held) noexcept {
    held = g_barHeld.load(std::memory_order_relaxed);
    return g_barSteer.load(std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::vr::body
