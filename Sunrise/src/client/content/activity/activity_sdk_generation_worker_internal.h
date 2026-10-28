#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/activity_sdk/generated_world/catalog_manifest.h"
#include "../../../state/activity_sdk/generated_world/format.h"
#include "../../../state/activity_sdk/generation/definition.h"
#include "../../../state/build_data/scriptables/definition.h"
#include "activity_sdk_activity_inventory.h"
#include "activity_sdk_external_placements.h"
#include "activity_sdk_generation_worker.h"
#include "activity_sdk_lua_artifacts.h"
#include "activity_sdk_topology_inventory.h"
#include "scriptable_catalog_builder.h"

namespace sunrise::client::content::activity::sdk_generation::worker_internal {

namespace generated = state::activity_sdk::generated_world;
namespace manifest = state::activity_sdk::generated_world::manifest;
namespace package_reader = middleware::content::packages::reader;
namespace catalog = state::build_data::scriptables;
namespace builder = ::sunrise::client::content::activity::scriptables::internal;
namespace inventory = activity_inventory;
namespace topology = topology_inventory;

/** Holds one step token plus the builder's own 96-byte refusal text. */
inline constexpr std::size_t kDetailCapacity = 160;

/** One stable scenario identity copied out of the package inventory. */
struct Scenario final {
    std::uint32_t tag{};
    std::array<char, catalog::kScenarioNameCapacity> name{};
    std::uint8_t nameLength{};
};

/** Inputs and owned state kept for one live or offline package pass. */
struct Work final {
    void* module{};
    package_reader::BlockKeys keys{};
    generated::Digest sourceFingerprint{};
    std::wstring packageDirectory{};
    std::wstring sdkDirectory{};
    std::wstring scenarioDirectory{};
    std::wstring catalogPath{};
    std::wstring packPath{};
    std::wstring cacheScenarioDirectory{};
    std::wstring cacheCatalogPath{};
    /** Owned client executable whose reflection metadata compiles the decoder cache. */
    std::wstring executablePath{};
    /** Base of the running client module; null in the offline pass, which reads the file. */
    const void* executableModule{};
    std::vector<Scenario> scenarios{};
    std::vector<manifest::ActivityRootRecord> activityRoots{};
    std::vector<manifest::ActivityVariantRecord> activityVariants{};
    inventory::Snapshot activityInventory{};
    topology::Snapshot canonicalTopology{};
    std::vector<lua_artifacts::ScenarioWorldSource> scenarioWorldSources{};
    /** Placements the scenarios load from container lists and type-4 descriptors. */
    external_placements::Index externalPlacements{};
    generated::Digest payloadSha256{};
    std::uint64_t packBytes{};
    std::uint32_t luaFiles{};
    OfflineBuildResult* offlineResult{};
    OfflineBuildStatus* offlineStatus{};
    bool offline{};
    /** Writes the sdk/lua declaration tree. Live passes take it from the boot policy. */
    bool luaDeclarations{};
};

/** One indexed result keeps parallel work deterministic when threads finish out of order. */
struct ScenarioBuildResult final {
    manifest::Record record{};
    std::shared_ptr<const catalog::Snapshot> snapshot{};
    std::array<char, kDetailCapacity> detail{};
    bool attempted{};
    bool ready{};
    bool reused{};
};

/** @return True when the selected live or offline caller asked the pass to stop. */
[[nodiscard]] bool cancel_requested() noexcept;

/** Sends one worker event to the selected live or offline observer. */
void publish_progress(state::activity_sdk::generation::Status status,
                      std::uint32_t current,
                      std::uint32_t total,
                      std::uint32_t scenarioTag,
                      std::string_view detail,
                      bool determinate = true) noexcept;

/** Builds all scenario snapshots in parallel while retaining scenario-order output. */
[[nodiscard]] bool build_scenarios(Work& work,
                                   const package_reader::Source& source,
                                   const builder::ContainerIndex& containers,
                                   const manifest::Catalog& prior,
                                   bool priorReady,
                                   std::size_t firstScenario,
                                   std::size_t scenarioCount,
                                   std::vector<ScenarioBuildResult>& results);

/** Converts and checks the package-backed activity inventory used by every later stage. */
[[nodiscard]] bool build_inventory(Work& work, const package_reader::Source& source) noexcept;

/** Emits transparent positional trigger declarations for one mission module. */
[[nodiscard]] bool build_world_source(const catalog::Snapshot& snapshot,
                                      lua_artifacts::ScenarioWorldSource& result);

/** @return True when the native binding partition is safe to publish. */
[[nodiscard]] bool binding_ready(const inventory::BindingCompleteness& completeness,
                                 std::size_t activityCount) noexcept;

/** @return True when the persisted binding partition is safe to consume. */
[[nodiscard]] bool binding_ready(const manifest::BindingCompleteness& completeness,
                                 std::size_t activityCount) noexcept;

/** Converts the checked native binding partition to its persistent form. */
[[nodiscard]] bool convert_binding_completeness(const inventory::BindingCompleteness& input,
                                                manifest::BindingCompleteness& output) noexcept;

/** Formats the content-addressed path for one exact scenario payload. */
[[nodiscard]] bool shard_path(const std::wstring& directory,
                              std::uint32_t scenarioTag,
                              const generated::Digest& digest,
                              std::wstring& output) noexcept;

/** Reopens one full shard into caller-owned snapshot storage. */
[[nodiscard]] bool load_record(const std::wstring& scenarioDirectory,
                               const generated::Digest& sourceFingerprint,
                               const Scenario& scenario,
                               const manifest::Record& record,
                               catalog::Snapshot& output) noexcept;

/** Reopens one full shard under shared immutable ownership. */
[[nodiscard]] bool load_full_record(const std::wstring& scenarioDirectory,
                                    const generated::Digest& sourceFingerprint,
                                    const Scenario& scenario,
                                    const manifest::Record& record,
                                    std::shared_ptr<const catalog::Snapshot>& output) noexcept;

/** Finds one tag in the sorted scenario catalog. */
[[nodiscard]] const manifest::Record* find_record(const manifest::Catalog& catalog,
                                                  std::uint32_t scenarioTag) noexcept;

/** Accepts one exact name or the native unnamed-scenario fallback. */
[[nodiscard]] bool record_name_matches(const manifest::Record& record,
                                       std::uint32_t scenarioTag,
                                       std::string_view requestedName) noexcept;

/**
 * Builds the pack, Lua, and catalog in the selected isolated output. Live work then commits only
 * its owned Lua subtree, pack, and catalog with catalog last and rollback.
 */
[[nodiscard]] bool publish_estate(Work& work,
                                  const package_reader::Source& source,
                                  std::span<const manifest::Record> records,
                                  const char*& failureDetail) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::worker_internal
