#include "internal.h"

namespace sunrise::state::activity::membership::transactions {

/** Merges one sparse client report into host-owned State; it grants no mission authority. */
bool commit_authoritative(ActivityState& state,
                          SessionRecord& record,
                          const PendingMutation& prepared,
                          CommittedClientState& clientState) noexcept {
    clientState = {};
    if (!equal(prepared.authoritativeInput, prepared.authoritativeGuard)) {
        return false;
    }

    // The merge decides the outcome. The prepared plan is not compared against it. State moves
    // between prepare and commit, and refusing on that difference dropped the whole delta: no
    // revision advanced and the region never moved.
    const MembershipState before = record.membership;
    clientState.previousRegion = before.currentRegion.index;
    MembershipState merged = merge(before, prepared.authoritativeInput);
    const bool changed = !equal_authoritative(record.membership, merged);
    const bool movesRegion = moves_region(record.membership, merged);
    const bool revisionExhausted = state.stateRevision == activity::kMaximumRevision
                                   || (record.membership.hasIdentity
                                       && record.membership.revision == kMaximumMembershipRevision);
    if ((changed || movesRegion) && revisionExhausted) {
        return false;
    }
    if (!changed && !movesRegion) {
        clientState.activityStateRevision = state.stateRevision;
        clientState.membershipRevision = record.membership.revision;
        clientState.heldRegion = record.membership.currentRegion.index;
        clientState.committed = true;
        return true;
    }

    // Only a changed published field earns a revision. A bare region move changes nothing the
    // membership body carries; a public region's new advertisement is republished by the keepalive
    // when the region it advertises changes.
    if (changed && record.membership.hasIdentity) {
        ++merged.revision;
        merged.acknowledgedRevision = kAbsentRevision;
    }
    // A report that the client holds no slice set is the teardown before a load, so the write-back
    // it sent describes a world it has left. The next load reports its own.
    if (merged.currentRegion.index < 0) {
        merged.clientInWorld = false;
    }
    // The host teleport's spawn step and its retirement are part of the merge, so the staged
    // answer to the report already carries them.
    record.membership = merged;
    publish_change(state, record);
    clientState.region = record.membership.region;
    clientState.currentRegion = record.membership.currentRegion;
    clientState.heldRegion = record.membership.currentRegion.index;
    clientState.activityStateRevision = state.stateRevision;
    clientState.membershipRevision = record.membership.revision;
    clientState.teleportSliceSetIndex = record.membership.teleport.sliceSetIndex;
    clientState.teleportSliceSetHash = record.membership.teleport.spawnSetHash;
    clientState.spawnState = record.membership.spawn.state;
    clientState.teleportState = record.membership.teleport.state;
    clientState.hasRegion = movesRegion && record.membership.region.index >= 0;
    clientState.hasCurrentRegion =
        before.currentRegion.index != record.membership.currentRegion.index
        && record.membership.currentRegion.index >= 0;
    clientState.hasSpawn = before.spawn.state != record.membership.spawn.state;
    clientState.hasTeleport =
        before.teleport.state != record.membership.teleport.state
        || before.teleport.sliceSetIndex != record.membership.teleport.sliceSetIndex
        || before.teleport.spawnSetHash != record.membership.teleport.spawnSetHash;
    clientState.changed = clientState.hasRegion || clientState.hasCurrentRegion
                          || clientState.hasSpawn || clientState.hasTeleport;
    clientState.committed = true;
    return true;
}

} // namespace sunrise::state::activity::membership::transactions
