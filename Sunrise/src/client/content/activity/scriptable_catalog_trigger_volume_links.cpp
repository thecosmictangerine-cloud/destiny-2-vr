#include "scriptable_catalog_trigger_volume_links.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

namespace sunrise::client::content::activity::scriptables::internal {
namespace {

namespace catalog = state::build_data::scriptables;

// Package slot types that watch a volume: the player trigger and the player monitor.
constexpr std::uint16_t kIncomingTriggerSlotType = 31;
constexpr std::uint16_t kIncomingMonitorSlotType = 30;
constexpr std::uint8_t kTargetTriggerSlotType = 60;
// Fixed capacity for one pass's incoming trigger references.
constexpr std::size_t kIncomingReferenceCapacity = 262'144;

struct Candidate final {
    std::uint32_t targetObjectRow{};
    std::uint32_t targetKey{};
    std::uint32_t referenceRow{};
    std::uint16_t targetSlotIndex{};
    std::uint16_t targetSlotType{};
};

[[nodiscard]] auto identity(const Candidate& candidate) noexcept {
    return std::tie(candidate.targetObjectRow,
                    candidate.targetKey,
                    candidate.targetSlotType,
                    candidate.targetSlotIndex);
}

[[nodiscard]] auto identity(const catalog::TriggerVolumeTable& table,
                            std::uint32_t objectRow) noexcept {
    return std::tuple{
        objectRow, table.registryKey, static_cast<std::uint16_t>(table.slotType), table.slotIndex};
}

} // namespace

/** Retains every type-31 and type-30 reference compatible with a type-60 trigger owner. */
bool append_trigger_volume_incoming_references(catalog::Snapshot& output,
                                               TriggerVolumeLinkCancelCheck cancel) noexcept {
    output.triggerVolumeIncomingReferences.clear();
    for (catalog::TriggerVolumeOwner& owner : output.triggerVolumeOwners) {
        owner.firstIncomingReference = 0;
        owner.incomingReferenceCount = 0;
        owner.incomingReferenceMatchCount = 0;
    }
    try {
        std::vector<Candidate> candidates{};
        candidates.reserve(output.references.size());
        for (std::size_t referenceRow = 0; referenceRow < output.references.size();
             ++referenceRow) {
            if (cancel != nullptr && cancel()) {
                return false;
            }
            const catalog::TypedReference& reference = output.references[referenceRow];
            if (reference.sourceObjectRow >= output.objects.size()
                || reference.sourceSlotRow >= output.slots.size()
                || reference.targetObjectRow >= output.objects.size()
                || reference.join != catalog::ReferenceJoin::exact) {
                continue;
            }
            const catalog::Slot& sourceSlot = output.slots[reference.sourceSlotRow];
            if (sourceSlot.objectRow != reference.sourceObjectRow
                || (sourceSlot.slotType != kIncomingTriggerSlotType
                    && sourceSlot.slotType != kIncomingMonitorSlotType)) {
                continue;
            }
            candidates.push_back({reference.targetObjectRow,
                                  reference.targetKey,
                                  static_cast<std::uint32_t>(referenceRow),
                                  reference.targetSlotIndex,
                                  reference.targetSlotType});
        }
        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const Candidate& first, const Candidate& second) noexcept {
                      return identity(first) < identity(second);
                  });
        output.triggerVolumeIncomingReferences.reserve(
            (std::min)(candidates.size(), kIncomingReferenceCapacity));
        for (std::size_t ownerRow = 0; ownerRow < output.triggerVolumeOwners.size(); ++ownerRow) {
            if (cancel != nullptr && cancel()) {
                return false;
            }
            catalog::TriggerVolumeOwner& owner = output.triggerVolumeOwners[ownerRow];
            owner.firstIncomingReference =
                static_cast<std::uint32_t>(output.triggerVolumeIncomingReferences.size());
            if (owner.tableRow >= output.triggerVolumeTables.size()) {
                output.triggerVolumeDiagnostics.complete = false;
                continue;
            }
            const catalog::TriggerVolumeTable& table = output.triggerVolumeTables[owner.tableRow];
            if (table.slotType != kTargetTriggerSlotType
                || owner.objectRow >= output.objects.size()) {
                continue;
            }
            const auto key = identity(table, owner.objectRow);
            const auto first =
                std::lower_bound(candidates.begin(),
                                 candidates.end(),
                                 key,
                                 [](const Candidate& candidate, const auto& value) noexcept {
                                     return identity(candidate) < value;
                                 });
            const auto last =
                std::upper_bound(first,
                                 candidates.end(),
                                 key,
                                 [](const auto& value, const Candidate& candidate) noexcept {
                                     return value < identity(candidate);
                                 });
            const std::size_t matchCount = static_cast<std::size_t>(last - first);
            owner.incomingReferenceMatchCount = static_cast<std::uint32_t>(matchCount);
            const std::size_t available =
                kIncomingReferenceCapacity - output.triggerVolumeIncomingReferences.size();
            const std::size_t appendCount = (std::min)(matchCount, available);
            for (std::size_t offset = 0; offset < appendCount; ++offset) {
                const auto candidate = first + static_cast<std::ptrdiff_t>(offset);
                const catalog::TypedReference& reference =
                    output.references[candidate->referenceRow];
                output.triggerVolumeIncomingReferences.push_back(
                    {static_cast<std::uint32_t>(ownerRow),
                     candidate->referenceRow,
                     reference.sourceObjectRow,
                     reference.sourceSlotRow});
                ++owner.incomingReferenceCount;
            }
            const std::size_t dropped = matchCount - appendCount;
            if (dropped != 0) {
                output.triggerVolumeDiagnostics.droppedIncomingReferences += dropped;
                output.triggerVolumeDiagnostics.complete = false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::scriptables::internal
