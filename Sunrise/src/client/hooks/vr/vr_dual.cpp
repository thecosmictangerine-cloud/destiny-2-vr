/**
 * Dual wielding: a weapon in each hand, any pair from the loadout (docs/DUAL-WIELD.md).
 *
 * The game has one weapon in hand -- the right hand's, which the rest of the module already puts on
 * the right controller. The LEFT hand's weapon is any weapon of the loadout, the one in the right hand
 * included, and it is a real one: fired through the engine's own shot function, with its own
 * magazine, its own rounds and projectiles, its own muzzle flash, tracer and impacts.
 *
 * What the engine offers, measured live (2026-10-16, section 1-2 of the doc):
 *
 *   - Every weapon of the loadout is an object of its own, alive whether or not it is in hand, with
 *     its own magazine sub-object at weapon + 0x1760. Only the weapon in hand is ticked.
 *   - `+0xCB7680(weapon, 0, block*, 0, float 0, result*)` is a shot. Called for a weapon NOT in hand
 *     it consumes that weapon's round and queues its fire event; its fire-event player
 *     `+0xCCF720(weapon, frameTime)`, which the game runs only for the weapon in hand, plays it out
 *     into the launcher. With the magazine empty it refuses (`rax = 0`, flag 4): the ammunition stays
 *     the engine's business. It has no rate limit of its own: that is this module's (the per-weapon
 *     interval, below).
 *   - `+0xCAEB50(magazine, rounds, 0)` moves rounds from the reserve into the magazine: a reload.
 *
 * Everything that touches a weapon runs on the game thread, inside the fire-event player call the
 * game makes for the weapon in hand -- exactly where the game itself plays shots out.
 *
 * The left weapon's IMAGE is vr_offhand.cpp's (a recording of the weapon's own draws, replayed in the
 * left hand); where its rounds LEAVE is vr_aim.cpp's (the launch rewrite, fed by `left_shot`); its
 * HAND is vr_arms.cpp's (the right hand's grip on that weapon, mirrored). This file decides which
 * weapon, when it fires, and how it kicks.
 *
 * `SVR_Dual.txt`, one command per line:
 *
 *   dual on|off             the left hand takes a weapon, or puts it away
 *   left <slot>             which loadout slot the left hand holds (0 kinetic, 1 energy, 2 power)
 *   interval <slot> <ms>    override a weapon's rate of fire
 *   reloadtime <ms>         how long the left hand's reload takes (default 1500)
 *   kick on|off             the left gun's recoil
 *   calibrate on|off        record a weapon never held in the right hand by showing it there briefly
 *   dump                    the state, every weapon's ammunition and what is known about it
 *   -- research and tests --
 *   record                  record the weapon in the right hand now
 *   fire <slot> [n]         n engine shots of that weapon, one per frame
 *   play <slot> <frames>    run that weapon's fire-event player each frame
 *   reload <slot>           the engine's reload transfer on that weapon
 *   stats <slot>            decode every slot around that weapon as a stat, to SVR_DualStats<slot>.txt
 *   relearn                 forget the loadout and learn it again
 */

#include "vr_dual.h"
#include "vr_mode.h"

#include <Windows.h>
#include <share.h>

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
#include "vr_aim.h"
#include "vr_arms.h"
#include "vr_gpu.h"
#include "vr_offhand.h"
#include "vr_prehook.h"
#include "vr_state.h"
#include "xr_runtime.h"

namespace sunrise::client::hooks::vr::dual {
namespace {

using math::Affine;
using math::M3;
using math::V3;

constexpr wchar_t kCommandName[] = L"SVR_Dual.txt";
constexpr std::uint32_t kPollPeriod = 15;
constexpr std::uint32_t kReportPeriod = 300;

// --- Engine ------------------------------------------------------------------------------------

constexpr std::uintptr_t kFireRva = 0xCB7680;
constexpr std::uintptr_t kPlayEventsRva = 0xCCF720;
constexpr std::uintptr_t kAmmoGetterRva = 0xCBCD00;
/** Magazine sub-object getters: rounds in the magazine, rounds in reserve. */
constexpr std::uintptr_t kMagazineRva = 0xCBCF30;
constexpr std::uintptr_t kReserveRva = 0xCBCD00;
/** transfer(magazine, rounds, unlimited): reserve into magazine, as a reload does. Returns rounds moved. */
constexpr std::uintptr_t kTransferRva = 0xCAEB50;
/**
 * The weapon stat reader: every stat is stored obfuscated in a small block, `[block + 4]`, and this
 * decodes it to a float (the magazine's capacity is `stat(magazine + 0xE0)`). 273 call sites.
 */
constexpr std::uintptr_t kStatRva = 0xB8B7F0;
constexpr std::array<std::uint8_t, 5> kPlayEventsPrologue{0x48, 0x89, 0x54, 0x24, 0x10};
constexpr std::array<std::uint8_t, 4> kAmmoGetterPrologue{0x40, 0x53, 0x48, 0x83};
constexpr std::array<std::uint8_t, 4> kMagazinePrologue{0x40, 0x53, 0x48, 0x83};
constexpr std::array<std::uint8_t, 5> kTransferPrologue{0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::array<std::uint8_t, 4> kStatPrologue{0x48, 0x83, 0xEC, 0x28};
/** The fire function is virtualised: its entry is a jmp into the VM, and that is all there is to check. */
constexpr std::uint8_t kFireEntryByte = 0xE9;
/** The magazine sub-object the HUD's ammo getters are handed. */
constexpr std::uintptr_t kMagazineOffset = 0x1760;
/** The magazine's capacity stat block, inside the magazine sub-object. */
constexpr std::uintptr_t kCapacityOffset = 0xE0;
/**
 * The weapon's rate of fire, as the seconds between rounds: a stat block of the weapon object itself.
 * Found by decoding every slot of the three loadout weapons with the engine's own stat reader
 * (`stats`): hand cannon 0.5333, auto rifle 0.1000 (600 RPM), grenade launcher 0.4000 -- each the
 * exact inverse of the rounds-per-second block at +0xDC (1.875, 10, 2.5).
 */
constexpr std::uintptr_t kIntervalStat = 0x19C;
constexpr std::uintptr_t kRoundsPerSecondStat = 0xDC;

// --- Feel --------------------------------------------------------------------------------------

constexpr float kTriggerPress = 0.55F;
constexpr float kTriggerRelease = 0.35F;
/** The recoil kick's rise time: the gun peaks this long after the shot and settles in about 4x. */
constexpr float kKickTau = 0.045F;
/** How long a reload's tilt takes to come in and to go out, seconds. */
constexpr float kReloadEase = 0.22F;
/** A weapon that has been in the right hand, still, this long is recorded for the left. */
constexpr std::uint64_t kRecordAfterMs = 1200;
/** How long a calibration waits for a weapon to come up and be recorded before giving up. */
constexpr std::uint64_t kCalibrateTimeoutMs = 5000;

/**
 * Log lines are QUEUED and written by `flush_log` on the camera or present thread, never on the
 * game's own threads: most of this module runs inside the engine's weapon calls, on its job-system
 * threads, and file I/O there is time stolen from a fiber the frame is waiting on (vr_probe.cpp does
 * the same, for the same reason).
 */
using LogLine = std::array<char, core::log::kLineCapacity>;
SRWLOCK g_logLock = SRWLOCK_INIT;
std::array<LogLine, 64> g_logQueue{};
std::size_t g_logCount = 0;
std::atomic<std::uint64_t> g_logDropped{0};

void enqueue_log(const char* text, std::size_t length) noexcept {
    AcquireSRWLockExclusive(&g_logLock);
    if (g_logCount < g_logQueue.size()) {
        LogLine& line = g_logQueue[g_logCount++];
        const std::size_t n = std::min(length, line.size() - 1);
        std::memcpy(line.data(), text, n);
        line[n] = '\0';
    } else {
        g_logDropped.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

void report(const char* text) noexcept {
    enqueue_log(text, std::strlen(text));
}

template <typename... Args> void reportf(const char* format, Args... args) noexcept {
    LogLine line{};
    const int written = std::snprintf(line.data(), line.size(), format, args...);
    if (written > 0) {
        enqueue_log(line.data(), static_cast<std::size_t>(written) < line.size() ? static_cast<std::size_t>(written)
                                                                                  : line.size() - 1);
    }
}

/**
 * A trace that survives the process (SVR_DualTrace.txt in the game's directory, flushed line by
 * line): the log's queue dies with a crash, and the crashes of dual wield left nothing in it.
 */
SRWLOCK g_traceLock = SRWLOCK_INIT;
FILE* g_trace = nullptr;
bool g_traceTried = false;

template <typename... Args> void tracef(const char* format, Args... args) noexcept {
    AcquireSRWLockExclusive(&g_traceLock);
    if (!g_traceTried) {
        g_traceTried = true;
        g_trace = _wfsopen(L"SVR_DualTrace.txt", L"w", _SH_DENYNO);
    }
    if (g_trace != nullptr) {
        std::fprintf(g_trace, "%llu tid=%lu ", static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId());
        std::fprintf(g_trace, format, args...);
        std::fputc('\n', g_trace);
        std::fflush(g_trace);
    }
    ReleaseSRWLockExclusive(&g_traceLock);
}

/** Every engine call, traced the first time it is made with a given argument. */
std::array<std::atomic<std::uint64_t>, 128> g_tracedCalls{};

void trace_call(std::uintptr_t rva, std::uint64_t arg) noexcept {
    const std::uint64_t key = (static_cast<std::uint64_t>(rva) << 44) ^ arg;
    for (auto& slot : g_tracedCalls) {
        std::uint64_t seen = slot.load(std::memory_order_relaxed);
        if (seen == key) {
            return;
        }
        if (seen == 0 && slot.compare_exchange_strong(seen, key, std::memory_order_relaxed)) {
            tracef("call rva=%llX arg=%p", static_cast<unsigned long long>(rva), reinterpret_cast<void*>(arg));
            return;
        }
    }
}

/** Writes the queued lines. Camera or present thread only. */
void flush_log() noexcept {
    std::array<LogLine, 64> local{};
    std::size_t n = 0;
    AcquireSRWLockExclusive(&g_logLock);
    n = g_logCount;
    for (std::size_t i = 0; i < n; ++i) {
        local[i] = g_logQueue[i];
    }
    g_logCount = 0;
    ReleaseSRWLockExclusive(&g_logLock);
    for (std::size_t i = 0; i < n; ++i) {
        core::log::write(core::log::Channel::client, core::log::Level::warn, {local[i].data(), std::strlen(local[i].data())});
    }
    const std::uint64_t dropped = g_logDropped.exchange(0, std::memory_order_relaxed);
    if (dropped != 0) {
        std::array<char, 64> line{};
        const int w = std::snprintf(line.data(), line.size(), "ev=vr.dual log_dropped=%llu", static_cast<unsigned long long>(dropped));
        if (w > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::warn, {line.data(), static_cast<std::size_t>(w)});
        }
    }
}

std::uint64_t now_ms() noexcept {
    return GetTickCount64();
}

double now_seconds() noexcept {
    LARGE_INTEGER c{};
    LARGE_INTEGER f{};
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return f.QuadPart != 0 ? static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart) : 0.0;
}

std::uintptr_t g_base = 0;
std::atomic_bool g_installTried{false};
std::atomic_bool g_installed{false};
thread_local bool t_inside = false;

// ---------------------------------------------------------------------------------------------
// The loadout
// ---------------------------------------------------------------------------------------------

/** The loadout, in the order the HUD reads it each frame (kinetic, energy, power). */
std::array<std::atomic<std::uintptr_t>, 3> g_slots{};
std::atomic<std::uintptr_t> g_active{0};
std::atomic<std::uint64_t> g_activeSince{0};

int slot_of(std::uintptr_t weapon) noexcept {
    for (std::size_t i = 0; i < g_slots.size(); ++i) {
        if (weapon != 0 && g_slots[i].load(std::memory_order_relaxed) == weapon) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::uintptr_t weapon_in(int slot) noexcept {
    return slot >= 0 && slot < static_cast<int>(g_slots.size()) ? g_slots[static_cast<std::size_t>(slot)].load() : 0;
}

/**
 * Whether the left hand may use `weapon` in the engine: only once it has been in the right hand, at
 * rest, in this world -- which a recording (vr_offhand.cpp) proves, and calibration is what makes one.
 * A weapon never drawn is not complete: its event player or its shot called before it first comes up
 * reads through a null pointer and kills the game (`destiny2.exe+0xCB4949` / `+0xCBD9B1`, 2026-10-17,
 * Y held in a fresh session). The engine's own event-player calls are no proof: right after a load
 * it runs it once for every weapon of the loadout.
 */
bool usable(std::uintptr_t weapon) noexcept {
    return weapon != 0 && offhand::recorded(weapon);
}

/** Sunrise's boot-flow step in the world (vr_state.cpp). */
constexpr std::int32_t kWorldStep = 38;

/** Bumped each time the world is entered or left: whatever the camera thread had under way is void. */
std::atomic<std::uint32_t> g_worldEpoch{0};

/**
 * The HUD reads the loadout's magazines every frame, each weapon several times (A A B B C C | A A ...),
 * in a CYCLIC order that starts with the weapon in hand: slot 0 in hand reads 0 1 2, slot 1 reads
 * 1 2 0, slot 2 reads 2 0 1 (measured 2026-10-18; the first version took it for slot order, true only
 * with slot 0 in hand). So a frame gives the slots up to a rotation, and the rotation is the one that
 * agrees best with the slots already known; with none known (a world just entered), slot 0 is taken
 * to be in hand, and a slot key tapped by the gamepad corrects it (note_slot_key).
 * The slots are followed, not learned once: a weapon never drawn can be a provisional object that
 * the engine replaces with the real one when it first comes up (2026-10-17: ...2600 until drawn,
 * ...C780 after), and the left hand must hold the one the engine draws. A new set is taken once it
 * has been seen in kSlotStableFrames frames, so a stray read by other code cannot reorder them.
 */
constexpr int kSlotStableFrames = 30;
SRWLOCK g_seqLock = SRWLOCK_INIT;
std::array<std::uintptr_t, 3> g_seq{};
std::size_t g_seqCount = 0;
bool g_seqOverflow = false;
std::array<std::uintptr_t, 3> g_candidate{};
int g_candidateFrames = 0;

/**
 * Proof that a weapon object is still the engine's. A menu can replace any weapon of the loadout (the
 * inventory swaps the power weapon for another: a NEW object, the old one freed), and while it is open
 * the HUD may not read the loadout at all. Calling into the old object killed the game
 * (`destiny2.exe+0xCB4949`, [null+8], the old launcher in rcx, 2026-10-18 on the Quest). So the epoch
 * moves on every frame something other than play is on (a menu, a screen, a load), and an object may
 * be used by the left hand only once a complete HUD frame read in play, in the current epoch, has
 * named it. Under g_seqLock.
 */
std::uint32_t g_playEpoch = 1;
/**
 * Complete HUD frames committed so far: a weapon is confirmed only while one of the last few named it, so
 * an object the engine replaced while play went on is refused at once, not 30 frames later.
 */
std::uint64_t g_commitSerial = 0;
constexpr std::uint64_t kConfirmFrames = 4;
struct Seen final {
    std::uintptr_t weapon{};
    std::uint32_t epoch{};
    std::uint64_t serial{};
};
std::array<Seen, 6> g_seen{};

void note_seen(std::uintptr_t weapon) noexcept {
    Seen* slot = &g_seen[0];
    for (Seen& s : g_seen) {
        if (s.weapon == weapon) {
            slot = &s;
            break;
        }
        if (s.epoch < slot->epoch) {
            slot = &s;
        }
    }
    slot->weapon = weapon;
    slot->epoch = g_playEpoch;
    slot->serial = g_commitSerial;
}

/** Whether `weapon` has been seen in play since anything else was last on. Any thread. */
bool confirmed(std::uintptr_t weapon) noexcept {
    if (weapon == 0) {
        return false;
    }
    AcquireSRWLockShared(&g_seqLock);
    bool ok = false;
    for (const Seen& s : g_seen) {
        ok = ok || (s.weapon == weapon && s.epoch == g_playEpoch && g_commitSerial - s.serial <= kConfirmFrames);
    }
    ReleaseSRWLockShared(&g_seqLock);
    return ok;
}

/**
 * Whether the game is in play this frame: the world, nothing over it, and in first person -- with a sword
 * or on a sparrow there is no left-hand weapon to draw (vr_mode.cpp), and none of its engine calls.
 */
bool in_play() noexcept {
    return state::step() == kWorldStep && state::view() == state::View::immersive && mode::current() == mode::Mode::first;
}

/** Once a frame on the game thread: anything but play voids every proof. */
void sync_play() noexcept {
    if (in_play()) {
        return;
    }
    AcquireSRWLockExclusive(&g_seqLock);
    ++g_playEpoch;
    ReleaseSRWLockExclusive(&g_seqLock);
}

/** Under g_seqLock. */
void reset_sequence() noexcept {
    g_seq = {};
    g_seqCount = 0;
    g_seqOverflow = false;
    g_candidate = {};
    g_candidateFrames = 0;
}

/**
 * Frames since the HUD last read three weapons. A SWORD in the power slot is not read through this getter
 * (its ammunition goes another way): the HUD then reads two, the kinetic and the energy weapon, and a
 * loadout that waited for three was never learned -- the left hand had no weapon at all with a sword
 * equipped (2026-10-18). Two-weapon frames are taken once no three-weapon frame has come for a while, so a
 * stray short frame never disturbs a three-weapon loadout.
 */
std::uint32_t g_framesSinceThree = 1000;
constexpr std::uint32_t kTwoWeaponAfterFrames = 60;

/** The order of the frame that just ended. Under g_seqLock. */
void commit_sequence() noexcept {
    const bool three = g_seqCount == g_seq.size() && !g_seqOverflow;
    g_framesSinceThree = three ? 0 : std::min<std::uint32_t>(g_framesSinceThree + 1, 1000);
    const bool two = g_seqCount == 2 && !g_seqOverflow && g_framesSinceThree > kTwoWeaponAfterFrames;
    if (three || two) {
        ++g_commitSerial;
        if (in_play()) {
            for (std::size_t i = 0; i < g_seqCount; ++i) {
                note_seen(g_seq[i]);
            }
        }
        // Slot i is seq[(i + shift) % n], for the shift that keeps the most known slots where they are.
        const std::size_t n = g_seqCount;
        std::size_t shift = 0;
        int best = -1;
        for (std::size_t s = 0; s < n; ++s) {
            int matches = 0;
            for (std::size_t i = 0; i < n; ++i) {
                matches += g_slots[i].load(std::memory_order_relaxed) == g_seq[(i + s) % n] ? 1 : 0;
            }
            if (matches > best) {
                best = matches;
                shift = s;
            }
        }
        // Two weapons are the kinetic and the energy one; the power slot (a sword) stays unknown.
        std::array<std::uintptr_t, 3> slots{};
        for (std::size_t i = 0; i < n; ++i) {
            slots[i] = g_seq[(i + shift) % n];
        }
        if (slots == g_candidate) {
            ++g_candidateFrames;
        } else {
            g_candidate = slots;
            g_candidateFrames = 1;
        }
        if (g_candidateFrames == kSlotStableFrames) {
            std::array<std::uintptr_t, 3> gone{};
            for (std::size_t i = 0; i < g_slots.size(); ++i) {
                const std::uintptr_t was = g_slots[i].exchange(g_candidate[i], std::memory_order_relaxed);
                if (was != g_candidate[i]) {
                    gone[i] = was;
                    reportf("ev=vr.dual slot=%zu weapon=%p was=%p", i, reinterpret_cast<void*>(g_candidate[i]),
                            reinterpret_cast<void*>(was));
                    tracef("slot=%zu weapon=%p was=%p", i, reinterpret_cast<void*>(g_candidate[i]), reinterpret_cast<void*>(was));
                }
            }
            // An object that left the loadout altogether is no weapon of ours any more: its recording
            // goes (the address may come back as anything), and so does its proof.
            for (const std::uintptr_t was : gone) {
                if (was != 0 && std::find(g_candidate.begin(), g_candidate.end(), was) == g_candidate.end()) {
                    offhand::forget(was);
                    for (Seen& s : g_seen) {
                        if (s.weapon == was) {
                            s = Seen{};
                        }
                    }
                }
            }
        }
    }
    g_seqCount = 0;
    g_seqOverflow = false;
}

/**
 * The loadout belongs to one world. Orbit and character select read the HUD's ammo of weapon objects
 * of their own, and every load makes the world's anew: a loadout learned in orbit -- which is where
 * it was learned once VR came up at launch -- named, in the world, objects that are not the weapons
 * in hand, and the left hand's first use of one killed the game (2026-10-17). So it is learned only
 * in the world and forgotten on the way in and on the way out.
 */
void sync_world() noexcept {
    static std::atomic_bool s_inWorld{false};
    const bool inWorld = state::step() == kWorldStep;
    if (s_inWorld.exchange(inWorld, std::memory_order_relaxed) == inWorld) {
        return;
    }
    AcquireSRWLockExclusive(&g_seqLock);
    for (auto& slot : g_slots) {
        slot.store(0, std::memory_order_relaxed);
    }
    reset_sequence();
    ReleaseSRWLockExclusive(&g_seqLock);
    offhand::forget_all();
    g_worldEpoch.fetch_add(1, std::memory_order_relaxed);
    reportf("ev=vr.dual loadout reset reason=%s", inWorld ? "world_entered" : "world_left");
    tracef("loadout reset reason=%s", inWorld ? "world_entered" : "world_left");
}

/** The HUD reads every weapon's magazine each frame, in slot order: that is the loadout. */
void on_ammo_getter(std::uint64_t, std::uint8_t* frame) noexcept {
    const auto magazine = static_cast<std::uintptr_t>(prehook::arg(frame, 0));
    if (t_inside || magazine < kMagazineOffset) {
        return;
    }
    sync_world();
    if (state::step() != kWorldStep) {
        return;
    }
    const std::uintptr_t weapon = magazine - kMagazineOffset;
    // The frame's order (see g_seq): a read of the weapon last read changes nothing, a new weapon
    // extends the order, and one already in it, not last, starts the next frame.
    AcquireSRWLockExclusive(&g_seqLock);
    if (g_seqCount == 0 || g_seq[g_seqCount - 1] != weapon) {
        bool seen = false;
        for (std::size_t i = 0; i < g_seqCount; ++i) {
            seen = seen || g_seq[i] == weapon;
        }
        if (!seen) {
            if (g_seqCount < g_seq.size()) {
                g_seq[g_seqCount++] = weapon;
            } else {
                g_seqOverflow = true;
            }
        } else {
            commit_sequence();
            g_seq[0] = weapon;
            g_seqCount = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_seqLock);
}

/**
 * The slot key the gamepad last tapped, when, and what was in hand then: the next weapon to come up
 * is that slot's.
 */
std::atomic<int> g_keySlot{-1};
std::atomic<std::uint64_t> g_keyAt{0};
std::atomic<std::uintptr_t> g_keyFrom{0};
constexpr std::uint64_t kKeyAnchorMs = 2500;

/**
 * The weapon in the right hand, each frame. Once a weapon other than the one in hand at a slot key
 * has come up and is among the slots, it is that key's slot: when the slots say otherwise, they are
 * rotated so that it is (see g_seq). Game thread.
 */
void anchor_slots(std::uintptr_t weapon) noexcept {
    const int keySlot = g_keySlot.load(std::memory_order_relaxed);
    if (keySlot < 0) {
        return;
    }
    if (GetTickCount64() - g_keyAt.load(std::memory_order_relaxed) > kKeyAnchorMs) {
        g_keySlot.store(-1, std::memory_order_relaxed);
        return;
    }
    if (weapon == g_keyFrom.load(std::memory_order_relaxed)) {
        return;
    }
    AcquireSRWLockExclusive(&g_seqLock);
    const int at = slot_of(weapon);
    if (at >= 0) {
        g_keySlot.store(-1, std::memory_order_relaxed);
    }
    if (at >= 0 && at != keySlot) {
        // Only among the slots the HUD reads: with a sword the power slot is not one of them.
        const std::size_t n = g_slots[2].load(std::memory_order_relaxed) != 0 ? g_slots.size() : 2;
        if (static_cast<std::size_t>(at) >= n || static_cast<std::size_t>(keySlot) >= n) {
            ReleaseSRWLockExclusive(&g_seqLock);
            return;
        }
        std::array<std::uintptr_t, 3> old{};
        for (std::size_t i = 0; i < g_slots.size(); ++i) {
            old[i] = g_slots[i].load(std::memory_order_relaxed);
        }
        // New slot i holds what old slot (i + at - keySlot) held.
        const std::size_t shift = (static_cast<std::size_t>(at) + n - static_cast<std::size_t>(keySlot)) % n;
        for (std::size_t i = 0; i < n; ++i) {
            g_slots[i].store(old[(i + shift) % n], std::memory_order_relaxed);
        }
        g_candidate = {};
        g_candidateFrames = 0;
        reportf("ev=vr.dual slots anchored weapon=%p key_slot=%d was_slot=%d", reinterpret_cast<void*>(weapon), keySlot, at);
        tracef("slots anchored weapon=%p key_slot=%d was_slot=%d", reinterpret_cast<void*>(weapon), keySlot, at);
    }
    ReleaseSRWLockExclusive(&g_seqLock);
}

// ---------------------------------------------------------------------------------------------
// Engine calls
// ---------------------------------------------------------------------------------------------

std::int64_t call_int(std::uintptr_t rva, std::uint64_t rcx, std::uint64_t rdx = 0, std::uint64_t r8 = 0) noexcept {
    trace_call(rva, rcx);
    prehook::CallRegs regs{};
    regs.target = g_base + rva;
    regs.rcx = rcx;
    regs.rdx = rdx;
    regs.r8 = r8;
    if (!prehook::invoke(regs)) {
        return -1;
    }
    return static_cast<std::int32_t>(regs.result & 0xFFFFFFFFULL);
}

float call_stat(std::uintptr_t block) noexcept {
    trace_call(kStatRva, block);
    prehook::CallRegs regs{};
    regs.target = g_base + kStatRva;
    regs.rcx = block;
    return prehook::invoke(regs) ? regs.resultFloat : 0.0F;
}

int magazine_of(std::uintptr_t weapon) noexcept {
    return weapon == 0 ? -1 : static_cast<int>(call_int(kMagazineRva, weapon + kMagazineOffset));
}

int reserve_of(std::uintptr_t weapon) noexcept {
    return weapon == 0 ? -1 : static_cast<int>(call_int(kReserveRva, weapon + kMagazineOffset));
}

/** @return The weapon's own seconds between rounds, or 0 when the stat does not read sensibly. */
float interval_of(std::uintptr_t weapon) noexcept {
    if (weapon == 0) {
        return 0.0F;
    }
    const float interval = call_stat(weapon + kIntervalStat);
    const float perSecond = call_stat(weapon + kRoundsPerSecondStat);
    // The two blocks must agree: one is the other's inverse on every weapon measured.
    if (!(interval > 0.02F && interval < 5.0F) || !(perSecond > 0.0F) || std::fabs(interval * perSecond - 1.0F) > 0.05F) {
        return 0.0F;
    }
    return interval;
}

int capacity_of(std::uintptr_t weapon) noexcept {
    return weapon == 0 ? -1 : static_cast<int>(std::lround(call_stat(weapon + kMagazineOffset + kCapacityOffset)));
}

/** A reload's transfer: as many rounds as fit, from the reserve. @return Rounds moved. */
int reload_weapon(std::uintptr_t weapon) noexcept {
    return weapon == 0 ? 0 : static_cast<int>(call_int(kTransferRva, weapon + kMagazineOffset, 9999, 0));
}

/**
 * One shot of `weapon` through the engine's own fire function, made the way the engine makes it:
 * `fire(weapon, 0, block*, 0, float 0, result*)`. The block (r8) is a structure on the caller's stack
 * whose pointers are only valid inside that caller, so it is not copied: a zeroed block is what the
 * function needs (measured: with one it runs to the end and queues the shot). The sixth argument is a
 * small result block it ORs flags into (+4: 1 or 2 fired, 4 refused for want of ammunition).
 * @return True when a round was fired.
 */
bool call_fire(std::uintptr_t weapon) noexcept {
    alignas(16) std::array<std::uint8_t, 0x400> block{};
    alignas(16) std::array<std::uint8_t, 0x100> result{};
    trace_call(kFireRva, weapon);
    prehook::CallRegs regs{};
    regs.target = g_base + kFireRva;
    regs.rcx = weapon;
    regs.r8 = reinterpret_cast<std::uint64_t>(block.data());
    regs.stack[1] = reinterpret_cast<std::uint64_t>(result.data());
    return prehook::invoke(regs) && (regs.result & 0xFF) != 0 && (result[4] & 4) == 0;
}

/** The engine's own arguments of this frame's event-player call: replayed for the other weapon. */
struct FrameArgs final {
    std::uint64_t rdx{};
    std::uint64_t r8{};
    std::uint64_t r9{};
    std::array<float, 4> xmm1{};
};

void call_play_events(std::uintptr_t weapon, const FrameArgs& args) noexcept {
    trace_call(kPlayEventsRva, weapon);
    prehook::CallRegs regs{};
    regs.target = g_base + kPlayEventsRva;
    regs.rcx = weapon;
    regs.rdx = args.rdx;
    regs.r8 = args.r8;
    regs.r9 = args.r9;
    regs.xmm[1] = args.xmm1;
    static_cast<void>(prehook::invoke(regs));
}

// ---------------------------------------------------------------------------------------------
// What is known about each weapon
// ---------------------------------------------------------------------------------------------

/**
 * Per weapon (the engine object; recordings, like the objects, live for the session): its shot as it
 * leaves the right hand, its rate of fire, its magazine.
 */
struct Profile final {
    std::uintptr_t key{};
    /** The right gun's shot in the right controller's frame (muzzle, fire basis), measured at rest. */
    Affine shot{};
    bool shotKnown{};
    bool sighted{};
    /** Seconds between rounds while the trigger is held. */
    float interval{};
    /** Where the interval came from: 0 a guess from the magazine, 1 measured off the right hand, 2 set. */
    int intervalSource{};
    /** The last few intervals measured off the right hand, for the floor of them. */
    std::array<float, 6> samples{};
    std::size_t sampleCount{};
    int capacity{};
};
SRWLOCK g_profileLock = SRWLOCK_INIT;
std::array<Profile, 8> g_profiles{};

Profile* profile_for(std::uintptr_t key, bool create) noexcept {
    Profile* empty = nullptr;
    for (Profile& p : g_profiles) {
        if (p.key == key) {
            return &p;
        }
        if (empty == nullptr && p.key == 0) {
            empty = &p;
        }
    }
    if (!create || key == 0) {
        return nullptr;
    }
    if (empty == nullptr) {
        empty = &g_profiles[0];
    }
    *empty = Profile{};
    empty->key = key;
    return empty;
}

/**
 * A rate of fire from the magazine alone, until the weapon has been fired in the right hand: small
 * magazines are slow weapons (launchers, hand cannons, snipers), big ones fast (SMGs, rifles). Only
 * ever a placeholder -- the first burst from the right hand replaces it.
 */
float guess_interval(int capacity) noexcept {
    if (capacity <= 0) {
        return 0.30F;
    }
    if (capacity <= 6) {
        return 0.67F;
    }
    if (capacity <= 13) {
        return 0.43F;
    }
    if (capacity <= 24) {
        return 0.23F;
    }
    if (capacity <= 40) {
        return 0.12F;
    }
    return 0.10F;
}

/** The left weapon's profile, copied (any thread). */
bool profile_copy(std::uintptr_t key, Profile& out) noexcept {
    AcquireSRWLockShared(&g_profileLock);
    const Profile* p = nullptr;
    for (const Profile& q : g_profiles) {
        if (q.key == key && key != 0) {
            p = &q;
        }
    }
    if (p != nullptr) {
        out = *p;
    }
    ReleaseSRWLockShared(&g_profileLock);
    return p != nullptr;
}

/** The right gun's shot, now, kept for its weapon. Camera thread. */
void remember_shot(std::uintptr_t key) noexcept {
    Affine shot{};
    bool sighted = false;
    if (key == 0 || !aim::right_shot_in_controller(shot, &sighted)) {
        return;
    }
    AcquireSRWLockExclusive(&g_profileLock);
    Profile* p = profile_for(key, true);
    const bool first = !p->shotKnown;
    p->shot = shot;
    p->shotKnown = true;
    p->sighted = sighted;
    ReleaseSRWLockExclusive(&g_profileLock);
    if (first) {
        reportf("ev=vr.dual shot_learned key=%p muzzle=%.3f,%.3f,%.3f dir=%.4f,%.4f,%.4f sighted=%d",
                reinterpret_cast<void*>(key), shot.t.x, shot.t.y, shot.t.z, shot.m.c[0].x, shot.m.c[0].y,
                shot.m.c[0].z, sighted ? 1 : 0);
    }
}

/** Mirror across the controller's own midplane: S * a * S, S = diag(1, -1, 1). */
Affine mirror(const Affine& a) noexcept {
    auto flip = [](const V3& v) { return V3{v.x, -v.y, v.z}; };
    M3 m{{flip(a.m.c[0]), -flip(a.m.c[1]), flip(a.m.c[2])}};
    return Affine{m, flip(a.t)};
}

Affine controller_frame(const xr::HandPose& hand) noexcept {
    const M3 c = math::orthonormalize(M3{{V3{hand.forward[0], hand.forward[1], hand.forward[2]},
                                          V3{-hand.right[0], -hand.right[1], -hand.right[2]},
                                          V3{hand.up[0], hand.up[1], hand.up[2]}}});
    return Affine{c, V3{hand.palm[0], hand.palm[1], hand.palm[2]}};
}

// ---------------------------------------------------------------------------------------------
// The left hand's state
// ---------------------------------------------------------------------------------------------

std::atomic_bool g_dual{false};
std::atomic<int> g_leftSlot{1};
std::atomic_bool g_kickOn{true};
std::atomic_bool g_calibrateOn{true};
std::atomic<float> g_reloadSeconds{1.5F};

/** Written on the game thread when a left round goes, read by the placement. QPC seconds. */
std::atomic<double> g_lastLeftShot{-10.0};
std::atomic<float> g_lastKickDeg{4.0F};
/** The left reload in progress: when it started and when it ends (QPC seconds), 0 when none. */
std::atomic<double> g_reloadStart{0.0};
std::atomic<double> g_reloadEnd{0.0};
std::atomic<int> g_leftMagazine{-1};
std::atomic<int> g_leftCapacity{-1};
std::atomic<float> g_leftInterval{0.0F};
std::atomic_bool g_counterOn{true};
std::atomic_bool g_holstersOn{true};
std::atomic<int> g_rightMagazine{-1};
std::atomic<int> g_rightCapacity{-1};
std::atomic<std::uint64_t> g_leftShots{0};
std::atomic<std::uint64_t> g_leftRefused{0};
std::atomic<std::uint64_t> g_leftReloads{0};
/** A reload asked for from outside the game thread (the reload button). */
std::atomic_bool g_reloadWanted{false};

/** For a twin (the same weapon in both hands): who pulled which trigger, in launch order. */
SRWLOCK g_fifoLock = SRWLOCK_INIT;
std::array<std::uint8_t, 64> g_fifo{};
std::size_t g_fifoHead = 0;
std::size_t g_fifoCount = 0;

void fifo_push(std::uint8_t hand) noexcept {
    AcquireSRWLockExclusive(&g_fifoLock);
    if (g_fifoCount == g_fifo.size()) {
        g_fifoHead = (g_fifoHead + 1) % g_fifo.size();
        --g_fifoCount;
    }
    g_fifo[(g_fifoHead + g_fifoCount) % g_fifo.size()] = hand;
    ++g_fifoCount;
    ReleaseSRWLockExclusive(&g_fifoLock);
}

int fifo_pop() noexcept {
    AcquireSRWLockExclusive(&g_fifoLock);
    int hand = 0;
    if (g_fifoCount != 0) {
        hand = g_fifo[g_fifoHead];
        g_fifoHead = (g_fifoHead + 1) % g_fifo.size();
        --g_fifoCount;
    }
    ReleaseSRWLockExclusive(&g_fifoLock);
    return hand;
}

std::uintptr_t left_key() noexcept {
    return g_dual.load(std::memory_order_relaxed) ? weapon_in(g_leftSlot.load(std::memory_order_relaxed)) : 0;
}

/**
 * Engine shots of the weapon in hand, observed: the rate of fire of the right hand's weapon, measured
 * as the game paces it while the trigger stays held. Game thread.
 */
std::uintptr_t g_lastRightWeapon = 0;
double g_lastRightShot = 0.0;

void on_fire(std::uint64_t, std::uint8_t* frame) noexcept {
    if (t_inside) {
        return;
    }
    const auto weapon = static_cast<std::uintptr_t>(prehook::arg(frame, 0));
    trace_call(0xF1, weapon);
    const double now = now_seconds();
    // A twin's engine shot is the right hand's: recorded so the launch after it is routed right.
    if (weapon == left_key() && weapon == g_active.load(std::memory_order_relaxed)) {
        fifo_push(0);
    }
    const xr::InputState in = xr::input_state();
    const bool held = in.valid && in.triggerR > kTriggerRelease;
    if (weapon == g_lastRightWeapon && held) {
        const auto gap = static_cast<float>(now - g_lastRightShot);
        if (gap > 0.03F && gap < 1.5F) {
            AcquireSRWLockExclusive(&g_profileLock);
            Profile* p = profile_for(weapon, true);
            if (p->intervalSource < 2) {
                p->samples[p->sampleCount % p->samples.size()] = gap;
                ++p->sampleCount;
                float floor = 10.0F;
                const std::size_t n = std::min(p->sampleCount, p->samples.size());
                for (std::size_t i = 0; i < n; ++i) {
                    floor = std::min(floor, p->samples[i]);
                }
                if (p->intervalSource == 0 || std::fabs(floor - p->interval) > 0.004F) {
                    reportf("ev=vr.dual interval_learned key=%p interval=%.3f samples=%zu", reinterpret_cast<void*>(weapon),
                            floor, n);
                }
                p->interval = floor;
                p->intervalSource = 1;
            }
            ReleaseSRWLockExclusive(&g_profileLock);
        }
    }
    g_lastRightWeapon = weapon;
    g_lastRightShot = now;
}

// ---------------------------------------------------------------------------------------------
// The left trigger, on the game thread
// ---------------------------------------------------------------------------------------------

bool g_leftTriggerDown = false;
double g_nextLeftShot = 0.0;
/** The burst in progress, for its summary in the log: first and last round, and how many. */
double g_burstFirst = 0.0;
double g_burstLast = 0.0;
int g_burstShots = 0;
std::uintptr_t g_lastLeftWeapon = 0;

/** Research commands for the game thread. */
std::atomic<int> g_fireSlot{-1};
std::atomic<int> g_fireLeft{0};
std::atomic<int> g_playSlot{-1};
std::atomic<int> g_playLeft{0};
std::atomic<int> g_reloadSlot{-1};
std::atomic<int> g_statSlot{-1};
std::atomic_bool g_dumpWanted{false};

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
 * Research: decodes every 4-byte slot around a weapon object as if it were a stat block, into
 * SVR_DualStats<slot>.txt (offset from the weapon object, decoded value, raw word). Game thread.
 */
void dump_stats(int slot) noexcept {
    const std::uintptr_t weapon = weapon_in(slot);
    if (weapon == 0 || !confirmed(weapon)) {
        return;
    }
    std::array<wchar_t, 64> name{};
    swprintf_s(name.data(), name.size(), L"SVR_DualStats%d.txt", slot);
    std::array<wchar_t, MAX_PATH> path{};
    FILE* file = nullptr;
    if (!game_path(name.data(), path) || _wfopen_s(&file, path.data(), L"w") != 0 || file == nullptr) {
        return;
    }
    for (std::ptrdiff_t off = -0x1000; off < 0x3000; off += 4) {
        const std::uintptr_t block = static_cast<std::uintptr_t>(static_cast<std::ptrdiff_t>(weapon) + off);
        std::uint32_t raw = 0;
        if (!prehook::read(block + 4, &raw, sizeof raw)) {
            continue;
        }
        std::fprintf(file, "%lld %.6g %08X\n", static_cast<long long>(off), call_stat(block), raw);
    }
    std::fclose(file);
    reportf("ev=vr.dual stats slot=%d weapon=%p capacity=%d", slot, reinterpret_cast<void*>(weapon), capacity_of(weapon));
}

void dump_state() noexcept {
    for (int s = 0; s < 3; ++s) {
        // An object not named by the HUD lately may be gone: not one engine call into it, even for a dump.
        const std::uintptr_t w = confirmed(weapon_in(s)) ? weapon_in(s) : 0;
        Profile p{};
        const bool known = profile_copy(w, p);
        reportf("ev=vr.dual weapon slot=%d key=%p active=%d left=%d magazine=%d reserve=%d capacity=%d recorded=%d "
                "shot=%d sighted=%d interval_stat=%.4f interval_learned=%.3f source=%d",
                s, reinterpret_cast<void*>(w), w != 0 && w == g_active.load() ? 1 : 0, w != 0 && w == left_key() ? 1 : 0,
                magazine_of(w), reserve_of(w), capacity_of(w), offhand::recorded(w) ? 1 : 0,
                known && p.shotKnown ? 1 : 0, known && p.sighted ? 1 : 0, interval_of(w), known ? p.interval : 0.0F,
                known ? p.intervalSource : -1);
    }
    reportf("ev=vr.dual state dual=%d left_slot=%d shots=%llu refused=%llu reloads=%llu left_magazine=%d",
            g_dual.load() ? 1 : 0, g_leftSlot.load(), static_cast<unsigned long long>(g_leftShots.load()),
            static_cast<unsigned long long>(g_leftRefused.load()), static_cast<unsigned long long>(g_leftReloads.load()),
            g_leftMagazine.load());
}

/** The left hand's weapon, once a frame, on the game thread. */
void left_hand(std::uintptr_t active, const FrameArgs& args) noexcept {
    const std::uintptr_t left = left_key();
    if (g_dual.load(std::memory_order_relaxed)) {
        g_rightMagazine.store(magazine_of(active), std::memory_order_relaxed);
        static std::uintptr_t s_capacityOf = 0;
        if (active != s_capacityOf) {
            s_capacityOf = active;
            g_rightCapacity.store(capacity_of(active), std::memory_order_relaxed);
        }
    }
    const double now = now_seconds();
    const xr::InputState in = xr::input_state();
    const bool down = in.valid && (g_leftTriggerDown ? in.triggerL > kTriggerRelease : in.triggerL > kTriggerPress);
    const bool pressed = down && !g_leftTriggerDown;
    g_leftTriggerDown = down;
    if (left == 0 || !enabled()) {
        g_leftMagazine.store(-1, std::memory_order_relaxed);
        return;
    }
    if (!in_play() || !confirmed(left)) {
        // A menu is on, or one was and the HUD has not yet named this object again: it may be gone
        // (see g_playEpoch). Not one engine call into it; the trigger state is still followed above.
        static std::uintptr_t s_unconfirmedLogged = 0;
        if (s_unconfirmedLogged != left && in_play()) {
            s_unconfirmedLogged = left;
            reportf("ev=vr.dual left_wait key=%p reason=unconfirmed", reinterpret_cast<void*>(left));
            tracef("left_wait key=%p reason=unconfirmed", reinterpret_cast<void*>(left));
        }
        return;
    }
    if (!usable(left)) {
        // Perhaps not yet complete in the engine (see `usable`): wait for calibration to bring it up.
        static std::uintptr_t s_waitLogged = 0;
        if (s_waitLogged != left) {
            s_waitLogged = left;
            reportf("ev=vr.dual left_wait key=%p reason=never_drawn", reinterpret_cast<void*>(left));
            tracef("left_wait key=%p", reinterpret_cast<void*>(left));
        }
        g_leftMagazine.store(-1, std::memory_order_relaxed);
        return;
    }
    if (left != g_lastLeftWeapon) {
        // A different weapon in the left hand: its own reload state, nothing carried over.
        g_lastLeftWeapon = left;
        g_reloadEnd.store(0.0, std::memory_order_relaxed);
        g_nextLeftShot = 0.0;
        g_leftCapacity.store(capacity_of(left), std::memory_order_relaxed);
        g_leftInterval.store(interval_of(left), std::memory_order_relaxed);
        reportf("ev=vr.dual left_weapon key=%p capacity=%d interval=%.4f", reinterpret_cast<void*>(left),
                g_leftCapacity.load(), g_leftInterval.load());
    }
    const bool twin = left == active;
    // A reload in progress finishes with the engine's own transfer, reserve into magazine.
    const double reloadEnd = g_reloadEnd.load(std::memory_order_relaxed);
    if (reloadEnd > 0.0 && now >= reloadEnd) {
        const int moved = reload_weapon(left);
        g_reloadEnd.store(0.0, std::memory_order_relaxed);
        g_leftReloads.fetch_add(1, std::memory_order_relaxed);
        reportf("ev=vr.dual left_reload done moved=%d magazine=%d", moved, magazine_of(left));
    }
    const bool reloading = g_reloadEnd.load(std::memory_order_relaxed) > 0.0;
    int magazine = magazine_of(left);
    const int capacity = g_leftCapacity.load(std::memory_order_relaxed);
    const bool wantReload = g_reloadWanted.exchange(false, std::memory_order_relaxed);
    if (!reloading && magazine >= 0 && reserve_of(left) > 0
        && ((magazine == 0 && (pressed || now - g_lastLeftShot.load() > 0.25)) || (wantReload && magazine < capacity))) {
        // Empty (or asked): reload. An empty magazine reloads by itself a moment after the last round,
        // as most VR shooters do with a weapon that has no magazine to swap by hand.
        g_reloadStart.store(now, std::memory_order_relaxed);
        g_reloadEnd.store(now + g_reloadSeconds.load(std::memory_order_relaxed), std::memory_order_relaxed);
        xr::haptic(false, 0.25F, 0.02F);
        reportf("ev=vr.dual left_reload start magazine=%d capacity=%d", magazine, capacity);
    }
    // The trigger: a round every interval while held, the first one the moment it is pulled.
    // The weapon's own rate of fire; failing that, one measured off the right hand, or a guess.
    Profile p{};
    const bool known = profile_copy(left, p);
    float interval = g_leftInterval.load(std::memory_order_relaxed);
    if (interval <= 0.0F) {
        interval = known && p.interval > 0.0F ? p.interval : guess_interval(capacity);
    }
    if (known && p.intervalSource == 2) {
        interval = p.interval;
    }
    if (!reloading && down && now >= g_nextLeftShot) {
        if (magazine > 0) {
            t_inside = true;
            const bool fired = call_fire(left);
            t_inside = false;
            if (fired) {
                if (twin) {
                    fifo_push(1);
                }
                g_leftShots.fetch_add(1, std::memory_order_relaxed);
                g_lastLeftShot.store(now, std::memory_order_relaxed);
                if (g_burstShots == 0) {
                    g_burstFirst = now;
                }
                g_burstLast = now;
                ++g_burstShots;
                g_lastKickDeg.store(std::clamp(interval * 14.0F, 2.0F, 9.0F), std::memory_order_relaxed);
                xr::haptic(false, std::clamp(0.35F + interval, 0.35F, 0.9F), 0.03F);
                --magazine;
            } else {
                g_leftRefused.fetch_add(1, std::memory_order_relaxed);
            }
            // Paced from when it was due, not from now: a frame's lateness does not slow the weapon. A
            // round more than half an interval late (the first of a burst, or after a hitch) paces from
            // now instead, so nothing ever fires faster than the weapon's own rate to catch up.
            const double due = g_nextLeftShot;
            g_nextLeftShot = now - due > interval * 0.5 ? now + interval : due + interval;
        } else if (pressed) {
            // Dry: a click in the hand.
            xr::haptic(false, 0.15F, 0.01F);
        }
    }
    if (!down && g_burstShots > 0) {
        // With where the left controller points and where its shot goes, in the room: the rounds the
        // launcher logs must leave along `shot_fwd`, and that must lie along `ctrl_fwd` up to the sight's
        // zeroing.
        Affine shot{};
        V3 ctrlFwd{};
        xr::HeadPose head{};
        xr::HandPose lh{};
        xr::HandPose rh{};
        (void)xr::camera_sample(head, lh, rh);
        if (lh.valid) {
            ctrlFwd = V3{lh.forward[0], lh.forward[1], lh.forward[2]};
        }
        static_cast<void>(left_shot(shot, nullptr));
        reportf("ev=vr.dual left_burst key=%p shots=%d seconds=%.3f interval=%.4f twin=%d magazine=%d "
                "ctrl_fwd=%.5f,%.5f,%.5f shot_fwd=%.5f,%.5f,%.5f",
                reinterpret_cast<void*>(left), g_burstShots, g_burstLast - g_burstFirst,
                g_burstShots > 1 ? (g_burstLast - g_burstFirst) / (g_burstShots - 1) : 0.0, twin ? 1 : 0, magazine,
                ctrlFwd.x, ctrlFwd.y, ctrlFwd.z, shot.m.c[0].x, shot.m.c[0].y, shot.m.c[0].z);
        g_burstShots = 0;
    }
    if (!down) {
        // A released trigger may fire again as soon as the weapon's own rate allows.
        g_nextLeftShot = std::min(g_nextLeftShot, g_lastLeftShot.load() + interval);
    }
    g_leftMagazine.store(magazine, std::memory_order_relaxed);
    // The left weapon's fire events, played out the way the game plays the weapon in hand's. A twin's
    // are the weapon in hand's own and the game plays them.
    if (!twin) {
        t_inside = true;
        call_play_events(left, args);
        t_inside = false;
    }
}

/** The weapon in hand's fire-event player, once a frame on the game thread. */
void on_play_events(std::uint64_t, std::uint8_t* frame) noexcept {
    if (t_inside) {
        return;
    }
    sync_world();
    sync_play();
    const auto weapon = static_cast<std::uintptr_t>(prehook::arg(frame, 0));
    trace_call(0xE1, weapon);
    if (weapon != g_active.load(std::memory_order_relaxed)) {
        g_active.store(weapon, std::memory_order_relaxed);
        g_activeSince.store(now_ms(), std::memory_order_relaxed);
    }
    anchor_slots(weapon);
    FrameArgs args{};
    args.rdx = prehook::arg(frame, 1);
    args.r8 = prehook::arg(frame, 2);
    args.r9 = prehook::arg(frame, 3);
    std::memcpy(args.xmm1.data(), frame + prehook::kXmm0 + 0x10, 16);
    left_hand(weapon, args);
    t_inside = true;
    // Research and test commands.
    const int fireSlot = g_fireSlot.load(std::memory_order_relaxed);
    if (fireSlot >= 0 && g_fireLeft.load(std::memory_order_relaxed) > 0) {
        const std::uintptr_t target = weapon_in(fireSlot);
        const bool fired = target != 0 && confirmed(target) && call_fire(target);
        reportf("ev=vr.dual fire slot=%d weapon=%p fired=%d", fireSlot, reinterpret_cast<void*>(target), fired ? 1 : 0);
        if (g_fireLeft.fetch_sub(1, std::memory_order_relaxed) <= 1) {
            g_fireSlot.store(-1, std::memory_order_relaxed);
        }
    }
    const int playSlot = g_playSlot.load(std::memory_order_relaxed);
    if (playSlot >= 0 && g_playLeft.load(std::memory_order_relaxed) > 0) {
        const std::uintptr_t target = weapon_in(playSlot);
        if (target != 0 && confirmed(target) && target != weapon && target != left_key()) {
            call_play_events(target, args);
        }
        if (g_playLeft.fetch_sub(1, std::memory_order_relaxed) <= 1) {
            g_playSlot.store(-1, std::memory_order_relaxed);
        }
    }
    const int reloadSlot = g_reloadSlot.exchange(-1, std::memory_order_relaxed);
    if (reloadSlot >= 0) {
        const std::uintptr_t w = confirmed(weapon_in(reloadSlot)) ? weapon_in(reloadSlot) : 0;
        const int before = magazine_of(w);
        const int moved = reload_weapon(w);
        reportf("ev=vr.dual reload slot=%d before=%d moved=%d after=%d reserve=%d", reloadSlot, before, moved,
                magazine_of(w), reserve_of(w));
    }
    const int statSlot = g_statSlot.exchange(-1, std::memory_order_relaxed);
    if (statSlot >= 0) {
        dump_stats(statSlot);
    }
    if (g_dumpWanted.exchange(false, std::memory_order_relaxed)) {
        dump_state();
    }
    t_inside = false;
}

template <std::size_t N> bool prologue_is(std::uintptr_t address, const std::array<std::uint8_t, N>& expected) noexcept {
    std::array<std::uint8_t, N> bytes{};
    return prehook::read(address, bytes.data(), bytes.size()) && bytes == expected;
}

void install() noexcept {
    if (g_installTried.exchange(true)) {
        return;
    }
    // The kill switch: with SVR_DualOff.txt beside the game, dual wield never touches the engine.
    std::array<wchar_t, MAX_PATH> off{};
    if (game_path(L"SVR_DualOff.txt", off) && GetFileAttributesW(off.data()) != INVALID_FILE_ATTRIBUTES) {
        report("ev=vr.dual install result=skipped reason=SVR_DualOff.txt");
        return;
    }
    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uint8_t fireEntry = 0;
    if (!prehook::read(g_base + kFireRva, &fireEntry, 1) || fireEntry != kFireEntryByte
        || !prologue_is(g_base + kPlayEventsRva, kPlayEventsPrologue)
        || !prologue_is(g_base + kAmmoGetterRva, kAmmoGetterPrologue)
        || !prologue_is(g_base + kMagazineRva, kMagazinePrologue)
        || !prologue_is(g_base + kTransferRva, kTransferPrologue) || !prologue_is(g_base + kStatRva, kStatPrologue)) {
        report("ev=vr.dual install result=fail reason=prologue_mismatch");
        return;
    }
    const char* failure = "";
    if (!prehook::attach(g_base + kFireRva, &on_fire, 0, failure)
        || !prehook::attach(g_base + kPlayEventsRva, &on_play_events, 0, failure)
        || !prehook::attach(g_base + kAmmoGetterRva, &on_ammo_getter, 0, failure)) {
        reportf("ev=vr.dual install result=fail reason=%s", failure);
        return;
    }
    g_installed.store(true, std::memory_order_release);
    report("ev=vr.dual install result=ok");
}

// ---------------------------------------------------------------------------------------------
// The camera thread: recordings, calibration, placement
// ---------------------------------------------------------------------------------------------

/**
 * Showing a weapon in the right hand that the left hand wants but that has never been recorded: the
 * game swaps to it, the recording is taken the moment it is at rest, and the game swaps back.
 */
enum class Calib : int { idle, swapping, back };
Calib g_calib = Calib::idle;
int g_calibReturnSlot = -1;
std::uintptr_t g_calibKey = 0;
std::uint64_t g_calibSince = 0;
std::atomic<int> g_slotRequest{-1};

/**
 * The kick, and the reload's tilt, as one rigid motion in the left controller's frame (forward, LEFT,
 * up; origin at the grip): the muzzle rises and the gun comes back into the hand after a shot,
 * Bungie-like; during a reload it rolls and dips out of the line of sight. Applied to the gun and the
 * hand together, never to where the round goes.
 */
Affine left_motion() noexcept {
    const double now = now_seconds();
    float pitch = 0.0F;
    float roll = 0.0F;
    float back = 0.0F;
    float drop = 0.0F;
    if (g_kickOn.load(std::memory_order_relaxed)) {
        const auto t = static_cast<float>(now - g_lastLeftShot.load(std::memory_order_relaxed));
        if (t >= 0.0F && t < 12.0F * kKickTau) {
            const float k = (t / kKickTau) * std::exp(1.0F - t / kKickTau);
            const float deg = g_lastKickDeg.load(std::memory_order_relaxed);
            pitch += deg * k * 0.0174533F;
            back += 0.006F * deg * k / 4.0F;
        }
    }
    const double start = g_reloadStart.load(std::memory_order_relaxed);
    const double end = g_reloadEnd.load(std::memory_order_relaxed);
    if (end > 0.0 && now >= start && now <= end) {
        const auto in = static_cast<float>(std::clamp((now - start) / kReloadEase, 0.0, 1.0));
        const auto out = static_cast<float>(std::clamp((end - now) / kReloadEase, 0.0, 1.0));
        float s = std::min(in, out);
        s = s * s * (3.0F - 2.0F * s);
        roll += 0.75F * s;
        pitch -= 0.45F * s;
        drop += 0.05F * s;
    }
    // Muzzle up is a turn about the controller's LEFT axis by -pitch; roll is about forward.
    const M3 rp = math::axis_angle(V3{0.0F, 1.0F, 0.0F}, -pitch);
    const M3 rr = math::axis_angle(V3{1.0F, 0.0F, 0.0F}, -roll);
    return Affine{rp * rr, V3{-back, 0.0F, -drop}};
}

/** Once a camera frame. */
void camera_update() noexcept {
    const std::uintptr_t active = g_active.load(std::memory_order_relaxed);
    const std::uint64_t now = now_ms();
    const bool running = enabled() && xr::active() && g_installed.load(std::memory_order_relaxed);
    // The weapon in the right hand, at rest and settled: its shot (always, it is cheap and tracks a
    // sight learned since) and, once, its image.
    const bool settled = running && active != 0 && now - g_activeSince.load(std::memory_order_relaxed) > kRecordAfterMs
                         && ads::animation_at_rest() && !arms::holding() && ads::engine_fraction() < 0.01F;
    static std::uint32_t s_frame = 0;
    ++s_frame;
    if (settled && s_frame % 30 == 0) {
        remember_shot(active);
        if (!offhand::recorded(active)) {
            offhand::request_recording(active, false);
            arms::snapshot_grip(active);
        }
    }
    // Calibration: the left hand wants a weapon nobody has recorded. A world entered or left since
    // voids one under way: its weapons are gone.
    sync_world();
    static std::uint32_t s_epoch = 0;
    const std::uint32_t epoch = g_worldEpoch.load(std::memory_order_relaxed);
    if (epoch != s_epoch) {
        s_epoch = epoch;
        g_calib = Calib::idle;
        g_slotRequest.store(-1, std::memory_order_relaxed);
    }
    const std::uintptr_t left = left_key();
    switch (g_calib) {
    case Calib::idle:
        if (running && left != 0 && !offhand::recorded(left) && g_calibrateOn.load(std::memory_order_relaxed)
            && left != active) {
            g_calibReturnSlot = slot_of(active);
            g_calibKey = left;
            g_calibSince = now;
            g_slotRequest.store(slot_of(left), std::memory_order_relaxed);
            g_calib = Calib::swapping;
            reportf("ev=vr.dual calibrate start key=%p return_slot=%d", reinterpret_cast<void*>(left), g_calibReturnSlot);
            tracef("calibrate start key=%p return_slot=%d", reinterpret_cast<void*>(left), g_calibReturnSlot);
        }
        break;
    case Calib::swapping: {
        // The weapon drawn is the real object that replaces the provisional one asked for (see g_seq):
        // the left hand's own weapon recorded is as good.
        const bool done = offhand::recorded(g_calibKey) || usable(left);
        if (done || now - g_calibSince > kCalibrateTimeoutMs) {
            reportf("ev=vr.dual calibrate %s key=%p left=%p", done ? "recorded" : "timeout",
                    reinterpret_cast<void*>(g_calibKey), reinterpret_cast<void*>(left));
            tracef("calibrate %s key=%p left=%p", done ? "recorded" : "timeout", reinterpret_cast<void*>(g_calibKey),
                   reinterpret_cast<void*>(left));
            if (g_calibReturnSlot >= 0) {
                g_slotRequest.store(g_calibReturnSlot, std::memory_order_relaxed);
            }
            g_calib = Calib::back;
            g_calibSince = now;
        }
        break;
    }
    case Calib::back:
        if (now - g_calibSince > 800) {
            g_calib = Calib::idle;
        }
        break;
    }
    // What the left hand shows this frame.
    offhand::Placement placement{};
    placement.key = left;
    placement.visible = left != 0 && running && offhand::recorded(left);
    placement.kick = left_motion();
    offhand::set_placement(placement);
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
        int b = 0;
        const int n = sscanf_s(line.data(), "%15s %31s %d", verb, static_cast<unsigned>(sizeof verb), a,
                               static_cast<unsigned>(sizeof a), &b);
        if (n < 1) {
            continue;
        }
        const int ai = std::atoi(a);
        if (std::strcmp(verb, "dual") == 0 && n >= 2) {
            set_dual(std::strcmp(a, "off") != 0);
        } else if (std::strcmp(verb, "left") == 0 && n >= 2) {
            set_left_slot(ai);
        } else if (std::strcmp(verb, "interval") == 0 && n >= 3) {
            AcquireSRWLockExclusive(&g_profileLock);
            Profile* p = profile_for(weapon_in(ai), true);
            if (p != nullptr) {
                p->interval = static_cast<float>(b) / 1000.0F;
                p->intervalSource = 2;
            }
            ReleaseSRWLockExclusive(&g_profileLock);
            reportf("ev=vr.dual interval slot=%d ms=%d", ai, b);
        } else if (std::strcmp(verb, "reloadtime") == 0 && n >= 2) {
            g_reloadSeconds.store(static_cast<float>(ai) / 1000.0F, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "kick") == 0 && n >= 2) {
            g_kickOn.store(std::strcmp(a, "off") != 0, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "counter") == 0 && n >= 2) {
            g_counterOn.store(std::strcmp(a, "off") != 0, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "holsters") == 0 && n >= 2) {
            g_holstersOn.store(std::strcmp(a, "off") != 0, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "calibrate") == 0 && n >= 2) {
            g_calibrateOn.store(std::strcmp(a, "off") != 0, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "dump") == 0) {
            g_dumpWanted.store(true, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "record") == 0) {
            const std::uintptr_t active = g_active.load(std::memory_order_relaxed);
            offhand::request_recording(active, true);
            arms::snapshot_grip(active);
            remember_shot(active);
            reportf("ev=vr.dual command record key=%p", reinterpret_cast<void*>(active));
        } else if (std::strcmp(verb, "fire") == 0 && n >= 2) {
            g_fireLeft.store(n >= 3 ? std::max(1, b) : 1, std::memory_order_relaxed);
            g_fireSlot.store(ai, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "play") == 0 && n >= 3) {
            g_playLeft.store(b, std::memory_order_relaxed);
            g_playSlot.store(ai, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "reload") == 0 && n >= 2) {
            g_reloadSlot.store(ai, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "stats") == 0 && n >= 2) {
            g_statSlot.store(ai, std::memory_order_relaxed);
        } else if (std::strcmp(verb, "relearn") == 0) {
            AcquireSRWLockExclusive(&g_seqLock);
            for (auto& slot : g_slots) {
                slot.store(0, std::memory_order_relaxed);
            }
            reset_sequence();
            ReleaseSRWLockExclusive(&g_seqLock);
        } else {
            reportf("ev=vr.dual command result=fail verb=%s", verb);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}


// ---------------------------------------------------------------------------------------------
// Holsters: each hand draws any weapon from where it is worn
// ---------------------------------------------------------------------------------------------
//
// Kinetic at the right hip, energy at the left hip, power over the shoulder: reach there with either
// hand and squeeze the grip. The right hand's weapon is the game's, so drawing there swaps it; the
// left hand takes that weapon as its own, or puts it back when it already holds it. Positions are in
// the body's frame (the head's heading, origin at the head), generous enough for a seated player.

struct HolsterZone final {
    V3 centre{};
    float radius{};
    int slot{};
};
constexpr std::array<HolsterZone, 3> kHolsters{{
    HolsterZone{V3{0.05F, -0.24F, -0.62F}, 0.22F, 0},
    HolsterZone{V3{0.05F, 0.24F, -0.62F}, 0.22F, 1},
    HolsterZone{V3{-0.22F, 0.0F, -0.02F}, 0.26F, 2},
}};

struct HandHolster final {
    int zone{-1};
    bool gripDown{};
    bool consumed{};
};
std::array<HandHolster, 2> g_handHolster{};

/** Which holster the palm is in, -1 for none. */
int holster_at(const xr::HeadPose& head, const xr::HandPose& hand) noexcept {
    if (!head.valid || !hand.valid) {
        return -1;
    }
    V3 f{head.forward[0], head.forward[1], 0.0F};
    if (math::length(f) < 0.2F) {
        f = V3{-head.up[0], -head.up[1], 0.0F};
        if (head.forward[2] < 0.0F) {
            f = V3{head.up[0], head.up[1], 0.0F};
        }
    }
    f = math::normalize(f);
    const V3 l{-f.y, f.x, 0.0F};
    const V3 d{hand.palm[0] - head.offset[0], hand.palm[1] - head.offset[1], hand.palm[2] - head.offset[2]};
    const V3 body{math::dot(d, f), math::dot(d, l), d.z};
    int best = -1;
    float bestDist = 1e9F;
    for (std::size_t i = 0; i < kHolsters.size(); ++i) {
        const float dist = math::length(body - kHolsters[i].centre);
        if (dist < kHolsters[i].radius && dist < bestDist) {
            best = static_cast<int>(i);
            bestDist = dist;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------
// The magazines, drawn on the guns
// ---------------------------------------------------------------------------------------------

namespace wl = graphics::renderer::world_lines;


/** Seven segments per digit (a top, b top-right, c bottom-right, d bottom, e bottom-left, f top-left, g middle). */
constexpr std::array<std::uint8_t, 10> kDigitSegments{0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

/** Appends a number as seven-segment strokes, centred at `centre`, in the plane of `right`/`up`. */
void add_number(std::vector<wl::Edge>& edges, int value, const V3& centre, const V3& right, const V3& up, float height,
                wl::Color color) noexcept {
    if (value < 0) {
        return;
    }
    std::array<int, 4> digits{};
    int count = 0;
    do {
        digits[static_cast<std::size_t>(count++)] = value % 10;
        value /= 10;
    } while (value > 0 && count < 4);
    const float w = height * 0.5F;
    const float h = height * 0.5F;
    const float pitch = w * 1.6F;
    const float total = pitch * static_cast<float>(count - 1);
    for (int i = 0; i < count; ++i) {
        const int digit = digits[static_cast<std::size_t>(count - 1 - i)];
        const V3 c = centre + right * (static_cast<float>(i) * pitch - total * 0.5F);
        auto p = [&](float x, float y) {
            const V3 v = c + right * (x * w * 0.5F) + up * (y * h);
            return wl::Vector3{v.x, v.y, v.z};
        };
        const std::uint8_t s = kDigitSegments[static_cast<std::size_t>(digit)];
        if ((s & 0x01) != 0) edges.push_back({p(-1, 1), p(1, 1), color});
        if ((s & 0x02) != 0) edges.push_back({p(1, 1), p(1, 0), color});
        if ((s & 0x04) != 0) edges.push_back({p(1, 0), p(1, -1), color});
        if ((s & 0x08) != 0) edges.push_back({p(-1, -1), p(1, -1), color});
        if ((s & 0x10) != 0) edges.push_back({p(-1, -1), p(-1, 0), color});
        if ((s & 0x20) != 0) edges.push_back({p(-1, 0), p(-1, 1), color});
        if ((s & 0x40) != 0) edges.push_back({p(-1, 0), p(1, 0), color});
    }
}

wl::Color ammo_color(int magazine, int capacity) noexcept {
    if (magazine <= 0) {
        return wl::Color{255, 70, 60, 255};
    }
    if (capacity > 0 && magazine * 4 <= capacity) {
        return wl::Color{255, 180, 40, 255};
    }
    return wl::Color{235, 240, 245, 255};
}

/**
 * One counter on a gun: just above its muzzle point, facing the eye. `shot` is the gun's shot in the
 * room; the counter rides with it, so it follows the hand.
 */
void add_counter(std::vector<wl::Edge>& edges, const Affine& shot, const xr::HeadPose& head, const V3& camRight,
                 const V3& camUp, int magazine, int capacity, float reload) noexcept {
    const V3 headOffset{head.offset[0], head.offset[1], head.offset[2]};
    const V3 fwd = shot.m.c[0];
    const V3 up = shot.m.c[2];
    // Just above the muzzle point (20 cm ahead of the palm): over the gun, where the eye already is when
    // aiming. Further back it sits by the wrist, which from 30 cm away projects far out of the view.
    const V3 anchor = shot.t + fwd * 0.02F + up * 0.065F - headOffset;
    if (reload >= 0.0F) {
        // Reloading: a bar that fills.
        const float half = 0.025F;
        const V3 a = anchor - camRight * half;
        const V3 b = a + camRight * (2.0F * half * std::clamp(reload, 0.0F, 1.0F));
        const V3 e = anchor + camRight * half;
        edges.push_back({{a.x, a.y, a.z}, {e.x, e.y, e.z}, wl::Color{60, 70, 80, 255}});
        edges.push_back({{a.x, a.y, a.z}, {b.x, b.y, b.z}, wl::Color{90, 200, 255, 255}});
        return;
    }
    add_number(edges, magazine, anchor, camRight, camUp, 0.022F, ammo_color(magazine, capacity));
}

std::uint32_t g_frame = 0;

} // namespace

void tick() noexcept {
    ++g_frame;
    flush_log();
    if (!g_installTried.load(std::memory_order_acquire) && enabled() && xr::active()) {
        install();
    }
    if (g_frame % kPollPeriod == 0) {
        poll_commands();
    }
    camera_update();
    if (g_frame % kReportPeriod == 0 && g_dual.load(std::memory_order_relaxed)) {
        reportf("ev=vr.dual report left=%p slot=%d magazine=%d shots=%llu refused=%llu reloads=%llu calib=%d",
                reinterpret_cast<void*>(left_key()), g_leftSlot.load(), g_leftMagazine.load(),
                static_cast<unsigned long long>(g_leftShots.load()), static_cast<unsigned long long>(g_leftRefused.load()),
                static_cast<unsigned long long>(g_leftReloads.load()), static_cast<int>(g_calib));
    }
}

std::uintptr_t active_weapon() noexcept {
    return g_active.load(std::memory_order_relaxed);
}

int launch_hand(std::uintptr_t weapon) noexcept {
    const std::uintptr_t left = left_key();
    if (weapon == 0 || weapon != left) {
        return 0;
    }
    if (weapon != g_active.load(std::memory_order_relaxed)) {
        return 1;
    }
    // A twin: in the order the triggers fired it.
    return fifo_pop();
}

bool left_shot(Affine& shot, bool* sighted) noexcept {
    const std::uintptr_t key = left_key();
    Profile p{};
    if (key == 0 || !profile_copy(key, p) || !p.shotKnown) {
        return false;
    }
    xr::HeadPose head{};
    xr::HandPose left{};
    xr::HandPose right{};
    (void)xr::camera_sample(head, left, right);
    if (!left.valid) {
        return false;
    }
    shot = controller_frame(left) * mirror(p.shot);
    if (sighted != nullptr) {
        *sighted = p.sighted;
    }
    return math::finite(shot.t);
}

bool dual() noexcept {
    return g_dual.load(std::memory_order_relaxed) && left_key() != 0;
}

void set_dual(bool on) noexcept {
    if (g_dual.exchange(on) != on) {
        reportf("ev=vr.dual dual=%s left_slot=%d", on ? "on" : "off", g_leftSlot.load());
        tracef("dual=%s left_slot=%d left=%p drawn=%d", on ? "on" : "off", g_leftSlot.load(),
               reinterpret_cast<void*>(weapon_in(g_leftSlot.load())), usable(weapon_in(g_leftSlot.load())) ? 1 : 0);
        xr::haptic(false, on ? 0.5F : 0.3F, 0.04F);
    }
}

void set_left_slot(int slot) noexcept {
    if (slot < 0 || slot > 2) {
        return;
    }
    g_leftSlot.store(slot, std::memory_order_relaxed);
    reportf("ev=vr.dual left_slot=%d key=%p recorded=%d", slot, reinterpret_cast<void*>(weapon_in(slot)),
            offhand::recorded(weapon_in(slot)) ? 1 : 0);
}

void cycle_left() noexcept {
    set_left_slot((g_leftSlot.load(std::memory_order_relaxed) + 1) % 3);
    xr::haptic(false, 0.3F, 0.02F);
}

int left_slot() noexcept {
    return g_leftSlot.load(std::memory_order_relaxed);
}

void request_reload() noexcept {
    g_reloadWanted.store(true, std::memory_order_relaxed);
}

int take_slot_request() noexcept {
    return g_slotRequest.exchange(-1, std::memory_order_relaxed);
}

void note_slot_key(int slot) noexcept {
    if (slot < 0 || slot > 2) {
        return;
    }
    g_keyFrom.store(g_active.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_keyAt.store(GetTickCount64(), std::memory_order_relaxed);
    g_keySlot.store(slot, std::memory_order_relaxed);
}

bool left_hold(std::uintptr_t& key, Affine& motion) noexcept {
    key = left_key();
    if (key == 0 || !offhand::recorded(key)) {
        return false;
    }
    motion = left_motion();
    return true;
}

int left_magazine() noexcept {
    return g_leftMagazine.load(std::memory_order_relaxed);
}

bool left_reloading() noexcept {
    return g_reloadEnd.load(std::memory_order_relaxed) > 0.0;
}

HolsterUse holster_pump() noexcept {
    HolsterUse use{};
    const xr::InputState in = xr::input_state();
    xr::HeadPose head{};
    xr::HandPose left{};
    xr::HandPose right{};
    xr::frame_sample(head, left, right);
    const bool on = g_holstersOn.load(std::memory_order_relaxed) && enabled() && xr::active() && in.valid
                    && g_installed.load(std::memory_order_relaxed);
    for (int side = 0; side < 2; ++side) {
        HandHolster& h = g_handHolster[static_cast<std::size_t>(side)];
        const xr::HandPose& hand = side == 0 ? left : right;
        const float grip = side == 0 ? in.gripL : in.gripR;
        const int zone = on ? holster_at(head, hand) : -1;
        if (zone >= 0 && zone != h.zone) {
            // Entering a holster: a faint tick, so the hand knows where it is without looking.
            xr::haptic(side == 1, 0.12F, 0.006F);
        }
        h.zone = zone;
        const bool down = h.gripDown ? grip > kTriggerRelease : grip > kTriggerPress;
        if (down && !h.gripDown && zone >= 0) {
            h.consumed = true;
            const int slot = kHolsters[static_cast<std::size_t>(zone)].slot;
            if (side == 0) {
                if (g_dual.load(std::memory_order_relaxed) && g_leftSlot.load(std::memory_order_relaxed) == slot) {
                    set_dual(false);
                    reportf("ev=vr.dual holster hand=left action=put_away slot=%d", slot);
                } else {
                    set_left_slot(slot);
                    set_dual(true);
                    reportf("ev=vr.dual holster hand=left action=draw slot=%d", slot);
                }
            } else {
                if (slot != slot_of(g_active.load(std::memory_order_relaxed))) {
                    g_slotRequest.store(slot, std::memory_order_relaxed);
                }
                reportf("ev=vr.dual holster hand=right action=draw slot=%d", slot);
            }
            xr::haptic(side == 1, 0.5F, 0.03F);
        } else if (!down) {
            h.consumed = false;
        }
        h.gripDown = down;
        if (side == 0) {
            use.left = h.consumed;
        } else {
            use.right = h.consumed;
        }
    }
    return use;
}

void present(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) noexcept {
    flush_log();
    // Out of play, every proof is void -- from here too, not only from the in-hand weapon's event player,
    // which may not run at all during a menu or a swap.
    sync_play();
    if (!g_counterOn.load(std::memory_order_relaxed) || !dual() || device == nullptr || context == nullptr
        || swapChain == nullptr) {
        return;
    }
    gpu::ViewCamera camera{};
    if (!gpu::viewmodel_camera(camera)) {
        return;
    }
    xr::HeadPose head{};
    xr::HandPose left{};
    xr::HandPose right{};
    (void)xr::camera_sample(head, left, right);
    if (!head.valid) {
        return;
    }
    const V3 fwd = math::normalize(V3{camera.forward[0], camera.forward[1], camera.forward[2]});
    const V3 up0 = math::normalize(V3{camera.up[0], camera.up[1], camera.up[2]});
    // The counters face the eye: the image's own right and up.
    const V3 camRight = math::normalize(math::cross(fwd, up0));
    const V3 camUp = math::normalize(math::cross(camRight, fwd));
    std::vector<wl::Edge> edges;
    edges.reserve(64);
    const std::uintptr_t key = left_key();
    Profile p{};
    if (left.valid && profile_copy(key, p) && p.shotKnown) {
        const Affine shot = controller_frame(left) * left_motion() * mirror(p.shot);
        float reload = -1.0F;
        const double end = g_reloadEnd.load(std::memory_order_relaxed);
        if (end > 0.0) {
            const double start = g_reloadStart.load(std::memory_order_relaxed);
            reload = static_cast<float>((now_seconds() - start) / std::max(0.05, end - start));
        }
        add_counter(edges, shot, head, camRight, camUp, g_leftMagazine.load(), g_leftCapacity.load(), reload);
    }
    Affine rightShot{};
    if (right.valid && aim::right_shot_in_controller(rightShot, nullptr)) {
        add_counter(edges, controller_frame(right) * rightShot, head, camRight, camUp, g_rightMagazine.load(),
                    g_rightCapacity.load(), -1.0F);
    }
    if (edges.empty()) {
        return;
    }
    ID3D11RenderTargetView* const target = gpu::back_buffer_view(device, swapChain);
    if (target == nullptr) {
        return;
    }
    teleport::CameraPose pose{};
    pose.position = {0.0F, 0.0F, 0.0F};
    pose.forward = camera.forward;
    pose.up = camera.up;
    pose.horizontalFov = camera.horizontalFov;
    pose.aspect = camera.aspect;
    // The projection the world was really rendered with, when known: the counters must sit on the guns
    // in whatever shape the render has.
    float p00 = 0.0F;
    float p11 = 0.0F;
    if (gpu::world_projection(p00, p11) && p00 > 0.0F && p11 > 0.0F) {
        pose.horizontalFov = 2.0F * std::atan(1.0F / p00);
        pose.aspect = p11 / p00;
    }
    wl::Batch batch{};
    batch.edges = std::span<const wl::Edge>(edges.data(), edges.size());
    batch.lineWidthPixels = 2.5F;
    static_cast<void>(wl::draw(device, context, target, pose, batch));
}


} // namespace sunrise::client::hooks::vr::dual
