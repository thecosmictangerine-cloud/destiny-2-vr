#include "activity_destination_public.h"

#include <cstddef>
#include <string_view>

#include "../../build_data/activities/activity_catalog.h"

namespace sunrise::state::activity::destination {
namespace {

/** Record +0xDC holds the activity template hash; every free-roam row carries this one. */
constexpr std::uint32_t kFreeRoamTemplateHash = 0x0109ED6BU;

} // namespace

/** Finds the lowest named free-roam row on the activity's destination. */
bool free_roam_activity(std::int16_t activityIndex, std::uint16_t& output) noexcept {
    const auto rows = build_data::activities::entries();
    if (activityIndex < 0 || static_cast<std::size_t>(activityIndex) >= rows.size()) {
        return false;
    }
    const std::uint8_t destination = rows[static_cast<std::size_t>(activityIndex)].destination;
    for (const build_data::activities::Definition& row : rows) {
        if (row.gameplaySettingsHash == kFreeRoamTemplateHash && row.destination == destination
            && !row.name().empty()) {
            output = row.index;
            return true;
        }
    }
    return false;
}

/** Builds the public destination from the source's free-roam row. */
bool public_destination(const DestinationSelection& source, DestinationSelection& output) noexcept {
    std::uint16_t index = 0;
    if (!free_roam_activity(source.activityIndex, index)) {
        return false;
    }
    if (static_cast<std::int16_t>(index) == source.activityIndex) {
        output = source;
        return true;
    }
    const std::string_view name = build_data::activities::entries()[index].name();
    if (name.empty() || name.size() > kPackageNameCapacity) {
        return false;
    }
    // Same world, so the arrival fields and the nonce stay; the peer matches only the nonce.
    DestinationSelection result = source;
    result.packageName = {};
    for (std::size_t position = 0; position < name.size(); ++position) {
        result.packageName[position] = static_cast<std::int8_t>(name[position]);
    }
    result.packageNameLength = static_cast<std::uint8_t>(name.size());
    result.activityIndex = static_cast<std::int16_t>(index);
    result.sourceActivityIndex = source.activityIndex;
    // The captured bits describe the mission pick, so the minimal descriptor is built instead.
    result.descriptorBits = {};
    result.descriptorBitLength = 0;
    result.descriptorNameBit = 0;
    result.hasDescriptorName = false;
    output = result;
    return true;
}

} // namespace sunrise::state::activity::destination
