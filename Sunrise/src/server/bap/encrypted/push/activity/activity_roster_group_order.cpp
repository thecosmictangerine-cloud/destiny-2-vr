// Group order, bubble retention and state sequences for the msg-5 roster body.
// Every helper here runs under the connection lock, and Scratch owns the storage its spans name.

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <utility>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity_sdk/runtime.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

namespace layouts = state::build_data::scenarios;

namespace {
/** The state byte is stored biased into one signed byte, so the sequence stays inside this. */
constexpr std::uint8_t kStateSequenceWrap = 128;
/** Standard 32-bit FNV-1a basis and prime fold the group set into one comparable value. */
constexpr std::uint32_t kFoldBasis = 2166136261U;
constexpr std::uint32_t kFoldPrime = 16777619U;

/** @return FNV-1a over one group's registration identity: its key, tag, slot set and epoch. */
[[nodiscard]] std::uint32_t group_identity_fold(const message::Group& group,
                                                std::uint8_t regionEpoch) noexcept {
    std::uint32_t folded = kFoldBasis;
    const auto mix = [&folded](std::uint32_t value) noexcept {
        folded = (folded ^ value) * kFoldPrime;
    };
    // A bubble change moves no group: each key stays registered under every bubble it was
    // seen in, and a moved byte tears the group's content down and rebuilds it.
    (void)regionEpoch;
    mix(group.key);
    mix(group.objectTag);
    mix(static_cast<std::uint32_t>(group.slotTypes.size()));
    for (const std::uint8_t slotType : group.slotTypes) {
        mix(slotType);
    }
    for (const std::uint8_t slotFlag : group.slotFlags) {
        mix(slotFlag);
    }
    for (const std::uint16_t slotIndex : group.slotIndices) {
        mix(slotIndex);
    }
    // The seed-only flag selects phase-2 content, not what the client registers. A moved byte
    // rebuilds the group and stops a cutscene playing in it, so content never moves the byte.
    return folded;
}
} // namespace

/**
 * Tracks the bubble the client holds; a change moves no group and only logs the crossing.
 * @param refresh Refresh being answered, which stands in for its uncommitted bubble, or null.
 */
void advance_region_epoch(Session& session, const RefreshReport* refresh) noexcept {
    const std::int32_t held =
        state::activity::membership::instantiated_region(client_placement(session, refresh));
    if (held < 0) {
        return;
    }
    const std::int32_t bubble =
        held
        / static_cast<std::int32_t>(middleware::content::packages::tables::kSliceSetIndexFactor);
    if (bubble == session.activityRosterRegionBubble) {
        return;
    }
    const std::int32_t previous = session.activityRosterRegionBubble;
    // The epoch no longer advances: a bubble change is the client's business, and the roster
    // keeps every registration. The log line still names the crossing.
    session.activityRosterRegionBubble = bubble;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=roster_state result=region_moved "
                                      "bubble=%d from=%d region=%d epoch=%u",
                                      bubble,
                                      previous,
                                      held,
                                      static_cast<unsigned>(session.activityRosterRegionEpoch));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

namespace {
/** @return Index of the used lease holding this key, or the lease count when none does. */
[[nodiscard]] std::size_t lease_index(const Session& session, std::uint32_t key) noexcept {
    for (std::size_t slot = 0; slot < session.activityRosterGroupLeases.size(); ++slot) {
        const RosterGroupLease& lease = session.activityRosterGroupLeases[slot];
        if (lease.used && lease.key == key) {
            return slot;
        }
    }
    return session.activityRosterGroupLeases.size();
}

/** @return One bit per sub-block bubble that lists this key. */
[[nodiscard]] std::uint64_t group_bubbles(const message::Roster& roster,
                                          std::uint32_t key) noexcept {
    std::uint64_t bubbles = 0;
    for (const message::BubbleSubBlock& block : roster.bubbleSubBlocks) {
        if (block.bubble >= layouts::kBubbleCapacity) {
            continue;
        }
        for (const std::uint32_t listed : block.keys) {
            if (listed == key) {
                bubbles |= std::uint64_t{1} << block.bubble;
                break;
            }
        }
    }
    return bubbles;
}

/** Re-points one group's slot spans at the scratch row it now occupies. */
void point_group_at_row(Scratch& scratch, std::size_t row, message::Group& group) noexcept {
    const layouts::RosterGroup& stored = scratch.rosterGroups[row];
    group.objectTag = stored.objectTag;
    group.key = stored.registryKey;
    group.slotTypes = std::span<const std::uint8_t>(stored.slotTypes.data(), stored.slotCount);
    group.slotFlags = std::span<const std::uint8_t>(stored.slotFlags.data(), stored.slotCount);
    group.slotIndices = std::span<const std::uint16_t>(stored.slotIndices.data(), stored.slotCount);
}

/** Drops one key from every bubble sub-block, and drops a block left with no key. */
[[nodiscard]] bool
remove_sub_block_key(Scratch& scratch, message::Roster& roster, std::uint32_t key) noexcept {
    const std::size_t blocks = roster.bubbleSubBlocks.size();
    if (blocks != 0 && roster.bubbleSubBlocks.data() != scratch.rosterSubBlocks.data()) {
        return false;
    }
    std::size_t kept = 0;
    for (std::size_t block = 0; block < blocks; ++block) {
        auto& keys = scratch.rosterSubBlockKeys[block];
        const std::size_t size = scratch.rosterSubBlocks[block].keys.size();
        std::size_t count = 0;
        for (std::size_t index = 0; index < size; ++index) {
            if (keys[index] != key) {
                keys[count++] = keys[index];
            }
        }
        if (count == 0) {
            continue;
        }
        if (kept != block) {
            scratch.rosterSubBlocks[kept] = scratch.rosterSubBlocks[block];
            scratch.rosterSubBlockKeys[kept] = keys;
        }
        scratch.rosterSubBlocks[kept].keys =
            std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[kept].data(), count);
        ++kept;
    }
    roster.bubbleSubBlocks = std::span(scratch.rosterSubBlocks).first(kept);
    return true;
}

/** Moves one group to the end of the top-level list; the groups it passes shift up by one. */
void rotate_into_top_level(Scratch& scratch,
                           message::Roster& roster,
                           std::size_t position) noexcept {
    const std::size_t top = roster.topLevelGroupCount;
    scratch.rosterGroupSpare = scratch.rosterGroups[position];
    const message::Group moved = roster.groups[position];
    for (std::size_t row = position; row > top; --row) {
        scratch.rosterGroups[row] = scratch.rosterGroups[row - 1];
        roster.groups[row] = roster.groups[row - 1];
        point_group_at_row(scratch, row, roster.groups[row]);
    }
    scratch.rosterGroups[top] = scratch.rosterGroupSpare;
    roster.groups[top] = moved;
    point_group_at_row(scratch, top, roster.groups[top]);
    ++roster.topLevelGroupCount;
}
} // namespace

/**
 * Registers every group of an object present in all scenario states in the top-level list.
 * A bubble sub-block key is rebuilt when the player enters that bubble, which resets its sensors.
 */
bool promote_scenario_wide_groups(const Session& session,
                                  Scratch& scratch,
                                  message::Roster& roster,
                                  std::size_t& firstAppended) noexcept {
    namespace sdk = state::activity_sdk;
    // Only a mission program selects states, so an activity without one keeps its layout.
    if (state::activity::membership::declared_initial_region(session.activity.session.sessionId)
        < 0) {
        return true;
    }
    const sdk::Snapshot catalog = sdk::snapshot();
    sdk::BoundView view{};
    const sdk::Selection selection{session.activity.session, 1, session.activity.bindingGeneration};
    if (catalog == nullptr || sdk::resolve(catalog, selection, view) != sdk::Status::ready) {
        return true;
    }
    const MissionSeedLease& seed = session.activityMissionSeed;
    const auto omissions =
        std::span(seed.plan.omissions).first(seed.configured ? seed.plan.omissionCount : 0);
    std::size_t wideCount = 0;
    if (!sdk::scenario_wide_groups(view, omissions, scratch.rosterWideGroups, wideCount)) {
        return false;
    }
    // Top-level entries are keyed by position, so the groups take their first-seen order.
    const std::size_t unleased = session.activityRosterGroupLeases.size();
    std::array<std::size_t, kScenarioWideGroupCapacity> order{};
    for (std::size_t index = 0; index < wideCount; ++index) {
        order[index] = index;
    }
    const auto rank = [&](std::size_t wide) noexcept {
        const std::size_t slot = lease_index(session, scratch.rosterWideGroups[wide].registryKey);
        return slot < unleased ? slot : unleased + wide;
    };
    std::stable_sort(order.begin(),
                     order.begin() + static_cast<std::ptrdiff_t>(wideCount),
                     [&rank](std::size_t first, std::size_t second) noexcept {
                         return rank(first) < rank(second);
                     });
    const bool ownsTopLevel = publishes_top_level_groups(session);
    for (std::size_t step = 0; step < wideCount; ++step) {
        const layouts::RosterGroup& wide = scratch.rosterWideGroups[order[step]];
        std::size_t position = roster.groups.size();
        const ExistingGroup existing = find_existing_group(wide, scratch, roster, position);
        if (existing == ExistingGroup::conflict) {
            return false;
        }
        if (existing == ExistingGroup::exact && position < roster.topLevelGroupCount) {
            continue;
        }
        if (existing == ExistingGroup::missing) {
            // Only the link that owns the top-level list registers a group nobody asked for yet.
            if (!ownsTopLevel) {
                continue;
            }
            position = roster.groupCount;
            if (position >= roster.groups.size()
                || !fill_generated_group(wide, scratch, position, roster)) {
                return false;
            }
            roster.groups[position] = message::Group{
                .objectTag = wide.objectTag, .key = wide.registryKey, .missionSeedOnly = true};
            point_group_at_row(scratch, position, roster.groups[position]);
            ++roster.groupCount;
        }
        if (!remove_sub_block_key(scratch, roster, wide.registryKey)) {
            return false;
        }
        if (position >= firstAppended) {
            ++firstAppended;
        }
        rotate_into_top_level(scratch, roster, position);
        roster.groups[roster.topLevelGroupCount - 1].retired = !ownsTopLevel;
    }
    return true;
}

/** Lease slots are handed out in first-seen order, so the slot index is that order. */
void order_appended_groups(const Session& session,
                           Scratch& scratch,
                           message::Roster& roster,
                           std::size_t firstAppended) noexcept {
    const std::size_t count = roster.groupCount;
    if (firstAppended >= count || count > roster.groups.size()
        || count > scratch.rosterGroups.size()) {
        return;
    }
    // A key with no lease yet keeps its append position, after every leased key.
    const std::size_t unleased = session.activityRosterGroupLeases.size();
    std::array<std::size_t, message::kPublishedGroupCapacity> source{};
    std::array<std::size_t, message::kPublishedGroupCapacity> rank{};
    for (std::size_t index = firstAppended; index < count; ++index) {
        source[index] = index;
        const std::size_t slot = lease_index(session, roster.groups[index].key);
        rank[index] = slot < unleased ? slot : unleased + index;
    }
    std::stable_sort(source.begin() + static_cast<std::ptrdiff_t>(firstAppended),
                     source.begin() + static_cast<std::ptrdiff_t>(count),
                     [&rank](std::size_t first, std::size_t second) noexcept {
                         return rank[first] < rank[second];
                     });
    std::array<message::Group, message::kPublishedGroupCapacity> previous{};
    std::array<bool, message::kPublishedGroupCapacity> placed{};
    for (std::size_t index = firstAppended; index < count; ++index) {
        previous[index] = roster.groups[index];
    }
    // Each cycle of the permutation moves one row at a time through the spare.
    for (std::size_t start = firstAppended; start < count; ++start) {
        if (placed[start] || source[start] == start) {
            placed[start] = true;
            continue;
        }
        scratch.rosterGroupSpare = scratch.rosterGroups[start];
        std::size_t target = start;
        while (source[target] != start) {
            scratch.rosterGroups[target] = scratch.rosterGroups[source[target]];
            placed[target] = true;
            target = source[target];
        }
        scratch.rosterGroups[target] = scratch.rosterGroupSpare;
        placed[target] = true;
    }
    for (std::size_t index = firstAppended; index < count; ++index) {
        roster.groups[index] = previous[source[index]];
        point_group_at_row(scratch, index, roster.groups[index]);
    }
}

/** A key leaves a bubble only with its lease, so a push never drops a registration on its own. */
bool retain_group_bubbles(const Session& session,
                          Scratch& scratch,
                          message::Roster& roster) noexcept {
    for (std::size_t index = roster.topLevelGroupCount; index < roster.groupCount; ++index) {
        const std::uint32_t key = roster.groups[index].key;
        const std::size_t slot = lease_index(session, key);
        if (slot >= session.activityRosterGroupLeases.size()) {
            continue;
        }
        // Every bubble the client holds this key under stays, whichever link hosts it now: a
        // dropped bubble re-registers the key on the next visit and rebuilds its content.
        const std::uint64_t wanted = session.activityRosterGroupLeases[slot].bubbles;
        for (std::uint32_t bubble = 0; bubble < layouts::kBubbleCapacity; ++bubble) {
            if ((wanted & (std::uint64_t{1} << bubble)) == 0) {
                continue;
            }
            if (!activate_existing_group(index, bubble, scratch, roster)) {
                return false;
            }
        }
    }
    return true;
}

namespace {
/** @return Rank of this bubble in the session's first-seen order, recording it when new. */
[[nodiscard]] std::size_t bubble_rank(Session& session, std::uint32_t bubble) noexcept {
    for (std::size_t rank = 0; rank < session.activityRosterBubbleOrderCount; ++rank) {
        if (session.activityRosterBubbleOrder[rank] == bubble) {
            return rank;
        }
    }
    const std::size_t rank = session.activityRosterBubbleOrderCount;
    if (rank < session.activityRosterBubbleOrder.size()) {
        session.activityRosterBubbleOrder[rank] = static_cast<std::uint8_t>(bubble);
        ++session.activityRosterBubbleOrderCount;
    }
    return rank;
}
} // namespace

/** @return Rank of this key in the bubble's first-seen key order, recording it when new. */
[[nodiscard]] std::size_t
bubble_key_rank(Session& session, std::uint32_t bubble, std::uint32_t key) noexcept {
    auto& order = session.activityRosterBubbleKeyOrder[bubble];
    std::size_t& count = session.activityRosterBubbleKeyOrderCount[bubble];
    for (std::size_t rank = 0; rank < count && rank < order.size(); ++rank) {
        if (order[rank] == key) {
            return rank;
        }
    }
    const std::size_t rank = count;
    if (rank < order.size()) {
        order[rank] = key;
        ++count;
    }
    return rank;
}

/** Puts one sub-block's keys into the order the client first saw them under that bubble. */
void order_sub_block_keys(Session& session,
                          std::uint32_t bubble,
                          std::span<std::uint32_t> keys) noexcept {
    // Record first, in the block's own order: a comparison must not be what names a new key's
    // rank, and a one-key block compares nothing.
    for (const std::uint32_t key : keys) {
        (void)bubble_key_rank(session, bubble, key);
    }
    for (std::size_t index = 0; index < keys.size(); ++index) {
        std::size_t lowest = index;
        for (std::size_t candidate = index + 1; candidate < keys.size(); ++candidate) {
            if (bubble_key_rank(session, bubble, keys[candidate])
                < bubble_key_rank(session, bubble, keys[lowest])) {
                lowest = candidate;
            }
        }
        std::swap(keys[index], keys[lowest]);
    }
}

/**
 * Sorts the bubble sub-blocks and their keys into the order the client first saw them.
 * @param session Owns the recorded first-seen order.
 * @param scratch Rows permuted with the sub-blocks.
 * @param roster Roster whose sub-blocks are reordered in place.
 */
void order_sub_blocks(Session& session, Scratch& scratch, message::Roster& roster) noexcept {
    const std::size_t count = roster.bubbleSubBlocks.size();
    std::array<std::size_t, layouts::kBubbleCapacity> ranks{};
    for (std::size_t index = 0; index < count && index < ranks.size(); ++index) {
        ranks[index] = bubble_rank(session, scratch.rosterSubBlocks[index].bubble);
    }
    // Each block's key span points into its own row, so the rows move with the blocks.
    for (std::size_t index = 0; index < count && index < ranks.size(); ++index) {
        std::size_t lowest = index;
        for (std::size_t candidate = index + 1; candidate < count; ++candidate) {
            if (ranks[candidate] < ranks[lowest]) {
                lowest = candidate;
            }
        }
        if (lowest == index) {
            continue;
        }
        std::swap(ranks[index], ranks[lowest]);
        std::swap(scratch.rosterSubBlocks[index], scratch.rosterSubBlocks[lowest]);
        std::swap(scratch.rosterSubBlockKeys[index], scratch.rosterSubBlockKeys[lowest]);
        const auto repoint = [&scratch](std::size_t moved) noexcept {
            scratch.rosterSubBlocks[moved].keys =
                std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[moved].data(),
                                               scratch.rosterSubBlocks[moved].keys.size());
        };
        repoint(index);
        repoint(lowest);
    }
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t bubble = scratch.rosterSubBlocks[index].bubble;
        if (bubble >= layouts::kBubbleCapacity) {
            continue;
        }
        order_sub_block_keys(session,
                             bubble,
                             std::span<std::uint32_t>(scratch.rosterSubBlockKeys[index].data(),
                                                      scratch.rosterSubBlocks[index].keys.size()));
    }
}

/**
 * Stamps every group's revision from its lease: stable while the group's identity holds,
 * advanced only for a new key or a changed identity. The wire carries one byte per key, and the
 * client rebuilds only the groups whose byte moved, so an unrelated change replays nothing.
 */
void stamp_group_sequences(Session& session, message::Roster& roster) noexcept {
    static_assert(kRosterGroupLeaseCapacity >= message::kPublishedGroupCapacity);
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        message::Group& group = roster.groups[index];
        const std::uint32_t folded = group_identity_fold(group, session.activityRosterRegionEpoch);
        RosterGroupLease* lease = nullptr;
        RosterGroupLease* freeSlot = nullptr;
        for (RosterGroupLease& candidate : session.activityRosterGroupLeases) {
            if (candidate.used && candidate.key == group.key) {
                lease = &candidate;
                break;
            }
            if (!candidate.used && freeSlot == nullptr) {
                freeSlot = &candidate;
            }
        }
        const bool newKey = lease == nullptr;
        if (newKey || lease->identityFold != folded) {
            session.activityRosterState =
                static_cast<std::uint8_t>((session.activityRosterState + 1) % kStateSequenceWrap);
            if (lease == nullptr) {
                lease = freeSlot;
            }
            if (lease != nullptr) {
                lease->key = group.key;
                lease->identityFold = folded;
                lease->sequence = session.activityRosterState;
                if (newKey) {
                    lease->bubbles = 0;
                }
                lease->used = true;
            }
            std::array<char, core::log::kLineCapacity> line{};
            const int written =
                std::snprintf(line.data(),
                              line.size(),
                              "ev=activity stage=roster_state result=group_moved key=0x%08X "
                              "seq=%u new=%d",
                              group.key,
                              session.activityRosterState,
                              newKey ? 1 : 0);
            if (written > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::debug,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        if (lease != nullptr && index >= roster.topLevelGroupCount) {
            lease->bubbles |= group_bubbles(roster, group.key);
        }
        group.stateSequence = lease != nullptr ? lease->sequence : session.activityRosterState;
        group.hasStateSequence = true;
    }
}
} // namespace sunrise::server::bap::encrypted::push::activity
