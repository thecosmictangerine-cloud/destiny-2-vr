#include "activity_membership_query.h"

#include <Windows.h>

#include <limits>

#include "../../runtime/storage/internal.h"
#include "../transactions/internal.h"

namespace sunrise::state::activity::membership {

/** Tests whether the client has applied the current membership revision. */
bool acknowledged(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool applied = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const MembershipState& membership = state.sessions[target].membership;
        applied = membership.revision != kAbsentRevision
                  && membership.acknowledgedRevision == membership.revision;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return applied;
}

/** Arms or clears the host-named teleport region for one joined session. */
bool arm_host_teleport(std::uint64_t sessionId,
                       std::int32_t sliceSetIndex,
                       std::uint32_t spawnSetHash) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool changed = false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        MembershipState& membership = state.sessions[target].membership;
        if (sliceSetIndex == kAbsentSliceSetIndex) {
            changed = membership.hasHostTeleport;
            membership.hasHostTeleport = false;
            membership.hostTeleport = {};
        } else if (!membership.hasHostTeleport
                   || membership.hostTeleport.sliceSetIndex != sliceSetIndex
                   || membership.hostTeleport.spawnSetHash != spawnSetHash) {
            // Step 0 latches the token, it does not compare it, so the increment is bookkeeping
            // for the client's own arm. The state is what gates the step, and zero is idle.
            const std::uint8_t token =
                static_cast<std::uint8_t>(membership.hostTeleport.token + 1U);
            membership.hostTeleport.sliceSetIndex = sliceSetIndex;
            membership.hostTeleport.spawnSetHash = spawnSetHash;
            membership.hostTeleport.token = token;
            membership.hostTeleport.state = kHostTeleportArmedState;
            membership.hasHostTeleport = true;
            // The client refuses a region record whose per-member token does not equal its own
            // transition count. The initial slice-set load is count 1 and each host teleport adds
            // one, so the published token must advance or the target region never precaches.
            const std::uint8_t current = membership.hasTransitionToken ? membership.transitionToken
                                                                       : kInitialTransitionToken;
            auto advanced = static_cast<std::uint8_t>(current + 1U);
            if (advanced == 0) {
                advanced = kInitialTransitionToken;
            }
            membership.transitionToken = advanced;
            membership.hasTransitionToken = true;
            changed = true;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return changed;
}

/** All-one bits is not a spawn set hash. */
constexpr std::uint32_t kInvalidSpawnSetHash = (std::numeric_limits<std::uint32_t>::max)();

/** Arms one hard wipe; the same request key is accepted again while it is armed. */
bool arm_hard_wipe(const SessionBinding& binding,
                   std::uint64_t requestKey,
                   std::int32_t region,
                   std::uint32_t spawnSetHash) noexcept {
    if (requestKey == 0 || region < 0 || spawnSetHash == 0
        || spawnSetHash == kInvalidSpawnSetHash) {
        return false;
    }
    bool accepted = false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, binding.sessionId);
    if (target != kInvalidSessionSlot
        && state.sessions[target].createdRevision == binding.createdRevision) {
        MembershipState& membership = state.sessions[target].membership;
        HardWipeState& wipe = membership.hardWipe;
        if (wipe.requestKey == requestKey) {
            accepted = true;
        } else if (!wipe.active && !membership.hasHostTeleport) {
            wipe.requestKey = requestKey;
            wipe.region = region;
            wipe.spawnSetHash = spawnSetHash;
            wipe.host = {};
            wipe.host.state = kHardWipeStartState;
            // The token must differ from the client's current block, or the client ignores it.
            wipe.host.opaqueByte = static_cast<std::uint8_t>(membership.spawn.opaqueByte + 1U);
            wipe.active = true;
            wipe.resetReady = false;
            wipe.clientWaiting = false;
            accepted = true;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return accepted;
}

/** @return True while the client has not yet mirrored the armed wipe's spawn block. */
bool hard_wipe_needs_publish(std::uint64_t sessionId) noexcept {
    bool pending = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const MembershipState& member = state.sessions[target].membership;
        const HardWipeState& wipe = member.hardWipe;
        pending = wipe.active
                  && (wipe.host.state == kHardWipeReleaseState
                      || member.spawn.opaqueByte != wipe.host.opaqueByte
                      || member.spawn.state < kHardWipeStartState);
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return pending;
}

/** @return True while the client has not yet mirrored an armed wipe's start block. */
bool hard_wipe_start_unseen(std::uint64_t sessionId) noexcept {
    bool unseen = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const MembershipState& member = state.sessions[target].membership;
        const HardWipeState& wipe = member.hardWipe;
        unseen = wipe.active && wipe.host.state != kHardWipeReleaseState
                 && (member.spawn.opaqueByte != wipe.host.opaqueByte
                     || member.spawn.state < kHardWipeStartState);
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return unseen;
}

/** Releases the armed wipe named by its request key. */
bool release_hard_wipe(const SessionBinding& binding, std::uint64_t requestKey) noexcept {
    bool accepted = false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, binding.sessionId);
    if (target != kInvalidSessionSlot
        && state.sessions[target].createdRevision == binding.createdRevision) {
        HardWipeState& wipe = state.sessions[target].membership.hardWipe;
        if (wipe.active && wipe.requestKey == requestKey) {
            wipe.release();
            accepted = true;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return accepted;
}

/** @return The checkpoint spawn set a wipe named for this region, or zero. */
std::uint32_t checkpoint_spawn_hash(std::uint64_t sessionId, std::int32_t region) noexcept {
    std::uint32_t result = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const HardWipeState& wipe = state.sessions[target].membership.hardWipe;
        if (wipe.requestKey != 0 && wipe.region == region) {
            result = wipe.spawnSetHash;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return result;
}

/** Holds or releases the spawn the mission program owns. */
void set_program_spawn_hold(std::uint64_t sessionId, bool held) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].membership.programSpawnHold = held;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** @return True while the mission program holds this session's spawn. */
bool program_spawn_hold(std::uint64_t sessionId) noexcept {
    bool held = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        held = state.sessions[target].membership.programSpawnHold;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return held;
}

/** Records the declared initial-state region on the joined session that owns the program. */
void note_declared_initial_region(std::uint64_t sessionId, std::int32_t region) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].membership.declaredInitialRegion = region;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Records the spawn set the attached program declared on the joined session that owns it. */
void note_declared_spawn_set(std::uint64_t sessionId, std::uint32_t spawnSetHash) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].membership.declaredSpawnSetHash = spawnSetHash;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** @return The declared spawn set, or zero while no program declared one. */
std::uint32_t declared_spawn_set(std::uint64_t sessionId) noexcept {
    std::uint32_t spawnSetHash = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        spawnSetHash = state.sessions[target].membership.declaredSpawnSetHash;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return spawnSetHash;
}

/** @return The declared initial-state region, or -1 while no program declared one. */
std::int32_t declared_initial_region(std::uint64_t sessionId) noexcept {
    std::int32_t region = kAbsentRegionIndex;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        region = state.sessions[target].membership.declaredInitialRegion;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return region;
}

/** @return The current activity State revision. */
std::uint64_t state_revision() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const std::uint64_t revision = runtime::storage::g_state.activity.stateRevision;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return revision;
}

/** Reports whether a host-named teleport is still waiting for the client to move. */
bool host_teleport_armed(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool armed = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const MembershipState& membership = state.sessions[target].membership;
        // The spawn state is the arm after the client has moved. It still rides every body, but it
        // is no longer waiting on anything, so it must not keep forcing revisions.
        armed =
            membership.hasHostTeleport && membership.hostTeleport.state != kHostTeleportSpawnState;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return armed;
}

/** Reads the pending region the client last reported. */
std::int32_t reported_region(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return kAbsentRegionIndex;
    }
    std::int32_t region = kAbsentRegionIndex;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        region = state.sessions[target].membership.region.index;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return region;
}

/** Reads the region the client is in: the current leg, else the pending one. */
std::int32_t player_region(std::uint64_t sessionId) noexcept {
    const ClientPlacement placement = reported_placement(sessionId);
    return placement.currentRegion >= 0 ? placement.currentRegion : placement.region;
}

/** Reads the slice set from the client's newest D6 teleport state. */
std::int32_t reported_slice_set(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return kAbsentSliceSetIndex;
    }
    std::int32_t sliceSet = kAbsentSliceSetIndex;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        sliceSet = state.sessions[target].membership.teleport.sliceSetIndex;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return sliceSet;
}

/** Reads the client's region legs and current bubble together. */
ClientPlacement reported_placement(std::uint64_t sessionId) noexcept {
    ClientPlacement placement{};
    if (sessionId == kAbsentSessionId) {
        return placement;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const MembershipState& membership = state.sessions[target].membership;
        placement.region = membership.region.index;
        placement.currentRegion = membership.currentRegion.index;
        placement.bubble = membership.bubble;
        placement.bubbleRevision = membership.bubbleRevision;
        placement.clientInWorld = membership.clientInWorld;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return placement;
}

/** Records the world state the client's character write-back reports, on every joined session. */
void note_client_writeback(bool inWorld) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    for (SessionRecord& record : state.sessions) {
        if (record.occupied && record.joined) {
            // Store the report alone. Folding in the region held at this instant drops an arrival
            // that lands before the region leg, and only the next write-back can restore it.
            record.membership.clientInWorld = inWorld;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Names the region the client has instantiated. */
std::int32_t instantiated_region(const ClientPlacement& placement) noexcept {
    return placement.currentRegion >= 0 ? placement.currentRegion : kAbsentRegionIndex;
}

/** Reads the newest session the client has reported a region on. */
std::uint64_t live_region_session(std::uint64_t fallback) noexcept {
    std::uint64_t newest = kAbsentSessionId;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    for (const SessionRecord& record : runtime::storage::g_state.activity.sessions) {
        if (record.occupied && record.sessionId > newest
            && (record.membership.region.index > kAbsentRegionIndex
                || record.membership.currentRegion.index > kAbsentRegionIndex)) {
            newest = record.sessionId;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return newest == kAbsentSessionId ? fallback : newest;
}

/** Reads the member key the client joined this activity session under. */
std::uint64_t member_key(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return 0;
    }
    std::uint64_t key = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && state.sessions[target].membership.hasIdentity) {
        key = state.sessions[target].membership.identity.memberKey;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return key;
}

/** Reads the identity value message 12 publishes at member record `+16`. */
std::uint64_t join_identity(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return 0;
    }
    std::uint64_t identity = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && state.sessions[target].membership.hasIdentity) {
        identity = state.sessions[target].membership.identity.joinIdentity;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return identity;
}

} // namespace sunrise::state::activity::membership
