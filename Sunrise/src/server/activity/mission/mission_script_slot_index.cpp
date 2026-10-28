#include "mission_script_slot_index.h"

#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <unordered_map>
#include <vector>

#include "../../../core/threading/srw_lock.h"

namespace sunrise::server::activity::mission::slot_index {
namespace {

namespace sdk = state::activity_sdk;
namespace format = state::activity_sdk::format;

// Marks an id that more than one slot carries. Local rows start at one.
constexpr std::uint32_t kAmbiguousRow = 0;
// Private and public instances of one activity share a scenario; a few entries cover them.
constexpr std::size_t kIndexCapacity = 4;

/** One object of the scenario, at its first occurrence. */
struct ObjectEntry final {
    std::uint32_t objectIndex{};
    std::uint32_t firstLocalRow{};
};

/** Slot view of one scenario. Its string keys point into the catalog it was built from. */
struct Index final {
    std::weak_ptr<const sdk::Catalog> catalog{};
    const sdk::Catalog* address{};
    std::uint32_t scenarioRow{format::kAbsentIndex};
    bool valid{};
    std::vector<std::uint32_t> nativeRows{};
    std::unordered_map<std::string_view, std::uint32_t> ids{};
    std::unordered_map<std::uint64_t, std::vector<ObjectEntry>> objects{};
};

core::threading::SrwLock g_lock{};
std::array<Index, kIndexCapacity> g_indexes{};
std::size_t g_nextIndex{};

[[nodiscard]] std::uint64_t object_key(std::uint32_t registryKey,
                                       std::uint32_t objectTag) noexcept {
    return (static_cast<std::uint64_t>(registryKey) << 32U) | objectTag;
}

void add_id(Index& index, std::string_view id, std::uint32_t localRow) {
    if (id.empty()) {
        return;
    }
    const auto [entry, inserted] = index.ids.try_emplace(id, localRow);
    if (!inserted && entry->second != localRow) {
        entry->second = kAmbiguousRow;
    }
}

/**
 * Walks the scenario once in local-row order. Objects repeat across occurrences; the first counts.
 * @return False when any occurrence or slot row is malformed, as the linear lookup refused.
 */
[[nodiscard]] bool build(const sdk::BoundView& view, Index& index) {
    const sdk::Catalog& catalog = *view.catalog;
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    if (scenario == nullptr) {
        return false;
    }
    const auto objects = catalog.objects();
    const auto allSlots = catalog.slots();
    std::vector<bool> seen(objects.size(), false);
    std::uint32_t localRow = 0;
    for (const format::Occurrence& occurrence : sdk::scenario_occurrences(catalog, *scenario)) {
        if (occurrence.objectIndex >= objects.size()) {
            return false;
        }
        if (seen[occurrence.objectIndex]) {
            continue;
        }
        seen[occurrence.objectIndex] = true;
        const format::Object& object = objects[occurrence.objectIndex];
        const auto slots = sdk::object_slots(catalog, object);
        if (slots.size() > (std::numeric_limits<std::uint32_t>::max)() - localRow
            || catalog.string(object.id).empty()) {
            return false;
        }
        index.objects[object_key(object.objectKey, object.objectTag)].push_back(
            {occurrence.objectIndex, localRow + 1});
        for (const format::Slot& slot : slots) {
            if (&slot < allSlots.data() || &slot >= allSlots.data() + allSlots.size()
                || slot.objectIndex != occurrence.objectIndex || catalog.string(slot.id).empty()) {
                return false;
            }
            ++localRow;
            index.nativeRows.push_back(static_cast<std::uint32_t>(&slot - allSlots.data()));
            add_id(index, catalog.string(slot.id), localRow);
            add_id(index, catalog.string(slot.name), localRow);
            for (const format::Text& alias : sdk::slot_aliases(catalog, slot)) {
                add_id(index, catalog.string(alias.value), localRow);
            }
        }
    }
    return true;
}

/**
 * Finds or builds the index for this view. The caller holds g_lock exclusively.
 * @return Null when the view names no scenario or the build ran out of memory.
 */
[[nodiscard]] const Index* index_for(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr) {
        return nullptr;
    }
    for (const Index& index : g_indexes) {
        // A live weak reference at the same address is the same catalog.
        if (index.address == view.catalog.get() && index.scenarioRow == view.scenarioRow
            && !index.catalog.expired()) {
            return &index;
        }
    }
    Index& index = g_indexes[g_nextIndex];
    g_nextIndex = (g_nextIndex + 1) % g_indexes.size();
    index = {};
    try {
        index.valid = build(view, index);
    } catch (const std::bad_alloc&) {
        index = {};
        return nullptr;
    }
    index.catalog = view.catalog;
    index.address = view.catalog.get();
    index.scenarioRow = view.scenarioRow;
    return &index;
}

[[nodiscard]] bool found_row(const Index& index, std::uint32_t localRow, Found& output) noexcept {
    if (localRow == 0 || localRow > index.nativeRows.size()) {
        return false;
    }
    output = {.localRow = localRow, .nativeRow = index.nativeRows[localRow - 1]};
    return true;
}

} // namespace

bool by_row(const sdk::BoundView& view, std::uint32_t localRow, Found& output) noexcept {
    output = {};
    const std::lock_guard guard(g_lock);
    const Index* const index = index_for(view);
    return index != nullptr && index->valid && found_row(*index, localRow, output);
}

bool by_id(const sdk::BoundView& view, std::string_view id, Found& output) noexcept {
    output = {};
    const std::lock_guard guard(g_lock);
    const Index* const index = index_for(view);
    if (index == nullptr || !index->valid || id.empty()) {
        return false;
    }
    const auto entry = index->ids.find(id);
    return entry != index->ids.end() && found_row(*index, entry->second, output);
}

/**
 * Finds the one scenario slot a Sense report names.
 * @param key Reported object, slot and optional Sense schema.
 * @param output Cleared first. Receives the slot row.
 * @return False unless exactly one slot matches.
 */
bool by_sense(const sdk::BoundView& view, const SenseKey& key, Found& output) noexcept {
    output = {};
    const std::lock_guard guard(g_lock);
    const Index* const index = index_for(view);
    if (index == nullptr || !index->valid) {
        return false;
    }
    const auto entry = index->objects.find(object_key(key.registryKey, key.objectTag));
    if (entry == index->objects.end()) {
        return false;
    }
    const sdk::Catalog& catalog = *view.catalog;
    std::size_t matches = 0;
    for (const ObjectEntry& object : entry->second) {
        const auto slots = sdk::object_slots(catalog, catalog.objects()[object.objectIndex]);
        for (std::size_t offset = 0; offset < slots.size(); ++offset) {
            const format::Slot& slot = slots[offset];
            if (slot.slotIndex != key.slotIndex || slot.slotType != key.slotType
                || (key.senseSchema != 0 && slot.senseSchema != key.senseSchema)) {
                continue;
            }
            ++matches;
            if (!found_row(
                    *index, object.firstLocalRow + static_cast<std::uint32_t>(offset), output)) {
                return false;
            }
        }
    }
    if (matches != 1) {
        output = {};
        return false;
    }
    return true;
}

std::size_t count(const sdk::BoundView& view) noexcept {
    const std::lock_guard guard(g_lock);
    const Index* const index = index_for(view);
    return index != nullptr && index->valid ? index->nativeRows.size() : 0;
}

} // namespace sunrise::server::activity::mission::slot_index
