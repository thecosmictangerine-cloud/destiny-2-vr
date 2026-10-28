#pragma once

#include "mission_auth_patch.h"
#include "squad_objective_auth.h"

namespace sunrise::middleware::bap::activity_message::squad_objective {

/** The transported objective target, evaluation revision and reserved placement mode. */
struct State final {
    std::uint32_t registryKey{};
    std::uint32_t revision{};
    std::uint16_t objectiveIndex{};
    std::int32_t taskGroup{kNoTaskGroup};
    bool reserved{};
    std::uint32_t spawnGeneration{};
};

/** Root field ordinals are fixed by the squad Auth schema. */
inline constexpr std::size_t kObjectiveField = 0;
inline constexpr std::size_t kSpawnGenerationField = 6;
inline constexpr std::size_t kRevisionField = 13;
inline constexpr std::size_t kTaskGroupField = 16;
inline constexpr std::size_t kModeField = 19;

/** Reads objective state from the complete transported squad body. */
[[nodiscard]] inline bool
read_state(std::span<const std::byte> body, std::size_t bits, State& output) noexcept {
    output = {};
    if (body.empty()) {
        return bits == 0;
    }
    mission_auth_patch::Layout layout{};
    if (!mission_auth_patch::parse(kSchema, body, bits, layout)) {
        return false;
    }
    std::uint64_t value = 0;
    encoding::bits::Reader mode(body);
    if (!mode.skip(layout.fields[kModeField].offset) || !mode.read(kModeWidth, value)) {
        return false;
    }
    output.reserved = value == static_cast<std::uint32_t>(squad_auth::Mode::reserve) + kModeBias;
    if (layout.fields[kTaskGroupField].present) {
        encoding::bits::Reader group(body);
        if (!group.skip(layout.fields[kTaskGroupField].offset + fields::kPresenceWidth)
            || !group.read(kTaskGroupWidth, value) || value > kTaskGroupCount) {
            return false;
        }
        output.taskGroup = static_cast<std::int32_t>(value) - kTaskGroupBias;
    }
    if (layout.fields[kSpawnGenerationField].present) {
        encoding::bits::Reader generation(body);
        if (!generation.skip(layout.fields[kSpawnGenerationField].offset + fields::kPresenceWidth)
            || !generation.read(fields::kCounterWidth, value)) {
            return false;
        }
        output.spawnGeneration = static_cast<std::uint32_t>(value);
    }
    if (layout.fields[kRevisionField].present) {
        encoding::bits::Reader revision(body);
        if (!revision.skip(layout.fields[kRevisionField].offset + fields::kPresenceWidth)
            || !revision.read(fields::kCounterWidth, value)) {
            return false;
        }
        output.revision = static_cast<std::uint32_t>(value);
    }
    if (!layout.fields[kObjectiveField].present) {
        return true;
    }
    encoding::bits::Reader reference(body);
    std::uint64_t type = 0;
    std::uint64_t index = 0;
    if (!reference.skip(layout.fields[kObjectiveField].offset + fields::kPresenceWidth)
        || !reference.read(fields::kClientRefKeyWidth, value)
        || !reference.read(fields::kClientRefTypeWidth, type)
        || !reference.read(fields::kClientRefIndexWidth, index)) {
        return false;
    }
    if (value != fields::kClientRefAbsentKey
        && type == scriptable_auth::kType3SlotType + fields::kClientRefTypeBias
        && index >= fields::kClientRefIndexBias) {
        output.registryKey = static_cast<std::uint32_t>(value);
        output.objectiveIndex = static_cast<std::uint16_t>(index - fields::kClientRefIndexBias);
    }
    return true;
}

/** Chooses an evaluation revision without allowing stale cost reports to change the assignment. */
[[nodiscard]] inline bool next_request(const State& previous,
                                       std::uint32_t expectedRevision,
                                       bool reconsider,
                                       bool preserveReservation,
                                       Request& request) noexcept {
    const bool same = previous.registryKey == request.registryKey
                      && previous.objectiveIndex == request.objectiveIndex;
    if (!reconsider && expectedRevision != 0 && (!same || previous.revision != expectedRevision)) {
        return false;
    }
    const bool advance = !same || reconsider || previous.revision == 0;
    if (advance && previous.revision == fields::kMaximumCounter) {
        return false;
    }
    request.revision = previous.revision + static_cast<std::uint32_t>(advance);
    if (preserveReservation) {
        request.reserved = previous.reserved;
    }
    return request.revision != 0;
}

} // namespace sunrise::middleware::bap::activity_message::squad_objective
