#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../state/build_data/scriptables/definition.h"
#include "activity_sdk_mission_runtime.h"
#include "host_runtime.h"

namespace sunrise::server::activity::activity_sdk_mission {

/** One exact scene dependency has one authored actor control and one source squad. */
struct SceneSpawnPair final {
    std::uint32_t actorSlotRow{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t squadRow{state::activity_sdk::format::kAbsentIndex};
    host::ScriptableTarget actorTarget{};
    host::ScriptableTarget sourceTarget{};
    bool actorReady{};
    bool sourceReady{};
};

/** The scene wire format bounds the complete cast before any output is queued. */
struct SceneSpawnPlan final {
    std::array<
        SceneSpawnPair,
        middleware::bap::activity_message::sensor_auth_update::kAuthoredSceneMaximumDependencyCount>
        pairs{};
    std::size_t count{};
};

/** Resolves the complete cast from exact package edges without reading Host state. */
[[nodiscard]] SceneStatus
collect_scene_spawn_plan(const state::activity_sdk::Catalog& catalog,
                         const state::build_data::scriptables::Snapshot& world,
                         std::uint32_t occurrenceRow,
                         std::uint32_t sceneSlotRow,
                         SceneSpawnPlan& output) noexcept;

/** Resolves physical cast identities without a transport lease or Host ownership check. */
[[nodiscard]] SceneStatus resolve_scene_spawn_plan(const state::activity_sdk::BoundView& view,
                                                   std::uint32_t occurrenceRow,
                                                   std::uint32_t sceneSlotRow,
                                                   SceneSpawnPlan& output) noexcept;

/** Copies exact parent rows for a queued scene intent; no transport lease is required. */
[[nodiscard]] SceneStatus scene_spawn_sources(const state::activity_sdk::BoundView& view,
                                              std::uint32_t occurrenceRow,
                                              std::uint32_t sceneSlotRow,
                                              std::span<std::uint32_t> output,
                                              std::size_t& count) noexcept;

/** Checks every cast route and retained actor/source before any preparation is queued. */
[[nodiscard]] SceneStatus query_scene_spawn_plan(const state::activity_sdk::BoundView& view,
                                                 std::uint32_t occurrenceRow,
                                                 std::uint32_t sceneSlotRow,
                                                 SceneSpawnPlan& output) noexcept;

/** Queues one missing preparation or the scene through the same durable intent reservation. */
[[nodiscard]] SceneStatus activate_authored_scene_spawn_reserved(
    const state::activity_sdk::BoundView& view,
    std::uint32_t occurrenceRow,
    std::uint32_t sceneSlotRow,
    const host::ScriptableOutputReservation& reservation) noexcept;

/** Matches one staged preparation to an exact actor/source pair owned by this scene. */
[[nodiscard]] SceneStatus
validate_scene_preparation_output(const state::activity_sdk::BoundView& view,
                                  std::uint32_t occurrenceRow,
                                  std::uint32_t sceneSlotRow,
                                  const host::PendingScriptableOverride& staged,
                                  std::uint32_t& squadRow) noexcept;

} // namespace sunrise::server::activity::activity_sdk_mission
