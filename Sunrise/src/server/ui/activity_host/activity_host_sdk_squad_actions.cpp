// The selected-squad action panel: member counts, authored anchors and the place action.
// Drawn from the render thread only; the input state here takes no lock.

#include "activity_host_sdk_squad_actions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <span>
#include <string_view>

#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../activity/activity_sdk_mission_runtime.h"
#include "../../activity/activity_sdk_squad_runtime.h"
#include "activity_host_sdk_squad_view.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::sdk_squad_view {

namespace format = state::activity_sdk::format;
namespace mission = server::activity::activity_sdk_mission;
namespace runtime = server::activity::activity_sdk_squads;
namespace scaling = core::ui::scaling::dpi;
namespace sdk = state::activity_sdk;
namespace section = core::ui::components::section;
namespace squad_auth = middleware::bap::activity_message::squad_auth;

namespace {
mission::SceneStatus g_behaviorSceneResult{mission::SceneStatus::ready};
bool g_hasBehaviorSceneResult{};
std::uint32_t g_initializedSquad{format::kAbsentIndex};
std::array<int, squad_auth::kMaximumRequestedCountLength> g_requestedCounts{};
int g_mode{};
bool g_useNameHash{};
std::uint32_t g_nameHash{};
/** Catalog row of the actor-spawn destination squad, or absent for none. */
std::uint32_t g_destinationRow{format::kAbsentIndex};
/** Catalog slot row of the type-66 spawn rule override, or absent for the package rule. */
std::uint32_t g_spawnRuleRow{format::kAbsentIndex};
/** Package slot type of a spawn rule; the squad Auth override must name one. */
constexpr std::uint32_t kSpawnRuleSlotType = 66;
runtime::Status g_lastResult{runtime::Status::ready};
bool g_hasResult{};

/** @return The candidate count shared by every lane; a pool to pick from, not a spawn count. */
[[nodiscard]] std::uint16_t authored_maximum(const format::SquadMember& member) noexcept {
    return *std::min_element(member.candidateCounts.begin(), member.candidateCounts.end());
}

// The count per slot is host-supplied and unauthored. Sense reports alive members in six bits.
constexpr int kRequestedCountLimit = 63;

/** Draws bounded member-count inputs and copies their current wire vector. */
[[nodiscard]] std::size_t draw_members(const sdk::Catalog& catalog,
                                       const format::Squad& squad,
                                       std::span<std::int32_t> output,
                                       bool& hasPositive) noexcept {
    const auto members = sdk::squad_members(catalog, squad);
    const auto actorClasses = catalog.actor_classes();
    const std::size_t count = (std::min)(members.size(), output.size());
    hasPositive = false;
    for (std::size_t index = 0; index < count; ++index) {
        g_requestedCounts[index] = std::clamp(g_requestedCounts[index], 0, kRequestedCountLimit);
        output[index] = static_cast<std::int32_t>(g_requestedCounts[index]);
        hasPositive = hasPositive || output[index] > 0;
    }
    if (!ImGui::BeginTable("##sdk_squad_members", 4, kWideTableFlags, table_layout::size(count))) {
        return count;
    }
    ImGui::TableSetupColumn("member", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("actor class", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("candidates");
    ImGui::TableSetupColumn("requested", ImGuiTableColumnFlags_WidthStretch);
    table_layout::frozen_headers();
    for (std::size_t index = 0; index < count; ++index) {
        const format::SquadMember& member = members[index];
        table_layout::next_row();
        ImGui::TableNextColumn();
        const std::string_view memberId = catalog.string(member.id);
        if (memberId.empty()) {
            ImGui::Text("member %u", static_cast<unsigned>(member.memberOrdinal));
        } else {
            ImGui::TextUnformatted(memberId.data(), memberId.data() + memberId.size());
        }
        ImGui::TableNextColumn();
        if (member.actorClassIndex >= actorClasses.size()) {
            ImGui::TextDisabled(member.actorClassIndex == format::kAbsentIndex
                                    ? "unresolved"
                                    : "invalid actor row");
        } else {
            const std::string_view actorId =
                catalog.string(actorClasses[member.actorClassIndex].id);
            if (actorId.empty()) {
                ImGui::TextDisabled("actor row %u", static_cast<unsigned>(member.actorClassIndex));
            } else {
                ImGui::TextUnformatted(actorId.data(), actorId.data() + actorId.size());
            }
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", member.defaultCount);
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::SliderInt("##requested", &g_requestedCounts[index], 0, kRequestedCountLimit)) {
            output[index] = static_cast<std::int32_t>(g_requestedCounts[index]);
            g_hasResult = false;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    hasPositive = std::any_of(output.begin(),
                              output.begin() + static_cast<std::ptrdiff_t>(count),
                              [](auto value) noexcept { return value > 0; });
    return count;
}

} // namespace

/** Initializes member requests from the exact positive generated defaults. */
void initialize_inputs(const sdk::Catalog& catalog,
                       const format::Squad& squad,
                       std::uint32_t squadRow) noexcept {
    if (g_initializedSquad == squadRow) {
        return;
    }
    g_requestedCounts.fill(0);
    const auto members = sdk::squad_members(catalog, squad);
    for (std::size_t index = 0; index < members.size() && index < g_requestedCounts.size();
         ++index) {
        g_requestedCounts[index] = (std::max)(members[index].defaultCount, 0);
    }
    g_mode = 0;
    g_useNameHash = false;
    g_nameHash = 0;
    g_destinationRow = format::kAbsentIndex;
    g_spawnRuleRow = format::kAbsentIndex;
    g_initializedSquad = squadRow;
    g_hasResult = false;
}

/** Catalog names are not NUL-terminated; ImGui labels need a terminated copy. */
using SquadLabel = std::array<char, 96>;

/**
 * Copies one squad's display name into a terminated label.
 * @param row Squad row; an out-of-range row takes the fallback.
 * @param fallback Label used when the squad has no name.
 * @return The terminated label, truncated to its capacity.
 */
[[nodiscard]] SquadLabel squad_label(const sdk::Catalog& catalog,
                                     std::span<const format::Squad> squads,
                                     std::uint32_t row,
                                     const char* fallback) noexcept {
    SquadLabel label{};
    const std::string_view name =
        row < squads.size() ? squad_display_name(catalog, squads[row]) : std::string_view{};
    const std::string_view text = name.empty() ? std::string_view{fallback} : name;
    const std::size_t length = (std::min)(text.size(), label.size() - 1);
    std::copy_n(text.data(), length, label.data());
    return label;
}

/** Copies a slot's name, then its id, into a terminated label. */
[[nodiscard]] SquadLabel slot_label(const sdk::Catalog& catalog,
                                    std::span<const format::Slot> slots,
                                    std::uint32_t row,
                                    const char* fallback) noexcept {
    SquadLabel label{};
    std::string_view name{};
    if (row < slots.size()) {
        name = catalog.string(slots[row].name);
        if (name.empty()) {
            name = catalog.string(slots[row].id);
        }
    }
    const std::string_view text = name.empty() ? std::string_view{fallback} : name;
    const std::size_t length = (std::min)(text.size(), label.size() - 1);
    std::copy_n(text.data(), length, label.data());
    return label;
}

/** Offers every type-66 spawn rule of the same object in place of the package rule. */
void draw_spawn_rule(const sdk::Catalog& catalog, const format::Squad& squad) noexcept {
    const auto slots = catalog.slots();
    const SquadLabel current = slot_label(catalog, slots, g_spawnRuleRow, "package rule");
    ImGui::SetNextItemWidth(scaling::pixels(360.0F));
    const bool open = ImGui::BeginCombo("Spawn rule override", current.data());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Type-1 field 12. The squad spawns at this rule's points instead.");
    }
    if (!open) {
        return;
    }
    if (ImGui::Selectable("package rule", g_spawnRuleRow == format::kAbsentIndex)) {
        g_spawnRuleRow = format::kAbsentIndex;
        g_hasResult = false;
    }
    for (std::uint32_t row = 0; row < slots.size(); ++row) {
        if (slots[row].objectIndex != squad.objectIndex
            || slots[row].slotType != kSpawnRuleSlotType) {
            continue;
        }
        const SquadLabel name = slot_label(catalog, slots, row, "rule");
        ImGui::PushID(static_cast<int>(row));
        if (ImGui::Selectable(name.data(), g_spawnRuleRow == row)) {
            g_spawnRuleRow = row;
            g_hasResult = false;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

/** Offers every other type-1 squad of the same object as the actor-spawn destination. */
void draw_destination(const sdk::Catalog& catalog,
                      const format::Squad& squad,
                      std::uint32_t squadRow) noexcept {
    const auto squads = catalog.squads();
    const SquadLabel current = squad_label(catalog, squads, g_destinationRow, "none");
    ImGui::SetNextItemWidth(scaling::pixels(360.0F));
    const bool open = ImGui::BeginCombo("Actor-spawn destination", current.data());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Type-1 field 10. Member actor-spawn actions fill this squad.");
    }
    if (!open) {
        return;
    }
    if (ImGui::Selectable("none", g_destinationRow == format::kAbsentIndex)) {
        g_destinationRow = format::kAbsentIndex;
        g_hasResult = false;
    }
    for (std::uint32_t row = 0; row < squads.size(); ++row) {
        if (row == squadRow || squads[row].objectIndex != squad.objectIndex) {
            continue;
        }
        const SquadLabel name = squad_label(catalog, squads, row, "squad");
        ImGui::PushID(static_cast<int>(row));
        if (ImGui::Selectable(name.data(), g_destinationRow == row)) {
            g_destinationRow = row;
            g_hasResult = false;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

/** Draws exact authored anchors without assigning gameplay semantics to their positions. */
void draw_anchors(const sdk::Catalog& catalog, const format::Squad& squad) noexcept {
    const auto anchors = sdk::squad_anchors(catalog, squad);
    ImGui::Text("%zu authored anchor%s", anchors.size(), anchors.size() == 1 ? "" : "s");
    for (const format::SquadAnchor& anchor : anchors) {
        const float x = std::bit_cast<float>(anchor.positionBits[0]);
        const float y = std::bit_cast<float>(anchor.positionBits[1]);
        const float z = std::bit_cast<float>(anchor.positionBits[2]);
        ImGui::BulletText("point %u  0x%08X[%u]  (%.3f, %.3f, %.3f)",
                          static_cast<unsigned>(anchor.pointOrdinal),
                          static_cast<unsigned>(anchor.objectListTag),
                          static_cast<unsigned>(anchor.placementOrdinal),
                          static_cast<double>(x),
                          static_cast<double>(y),
                          static_cast<double>(z));
    }
}

/** Draws every bounded option carried by the native type-1 encoder. */
void draw_place_action(const sdk::BoundView& view,
                       const format::Squad& squad,
                       std::uint32_t squadRow) noexcept {
    const auto members = sdk::squad_members(*view.catalog, squad);
    if (ImGui::Button("Authored defaults")) {
        for (std::size_t index = 0; index < members.size() && index < g_requestedCounts.size();
             ++index) {
            g_requestedCounts[index] = (std::max)(members[index].defaultCount, 0);
        }
        g_hasResult = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Zero all")) {
        g_requestedCounts.fill(0);
        g_hasResult = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Candidate counts")) {
        for (std::size_t index = 0; index < members.size() && index < g_requestedCounts.size();
             ++index) {
            g_requestedCounts[index] = static_cast<int>(authored_maximum(members[index]));
        }
        g_hasResult = false;
    }
    std::array<std::int32_t, squad_auth::kMaximumRequestedCountLength> requested{};
    bool hasPositive = false;
    const std::size_t count = draw_members(*view.catalog, squad, requested, hasPositive);
    const std::span<const std::int32_t> counts(requested.data(), count);
    // Mode 3 keeps the requests as capacity for another squad's actor-spawn actions.
    constexpr std::array<const char*, 3> kModes{"Mode 0", "Mode 2", "Mode 3 (reserve)"};
    g_mode = std::clamp(g_mode, 0, static_cast<int>(kModes.size()) - 1);
    ImGui::SetNextItemWidth(scaling::pixels(180.0F));
    if (ImGui::Combo("Placement mode", &g_mode, kModes.data(), static_cast<int>(kModes.size()))) {
        g_hasResult = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Modes 0 and 2 spawn now; mode 3 reserves the counts for a destination fill.");
    }
    if (ImGui::Checkbox("Include definition name hash", &g_useNameHash)) {
        g_hasResult = false;
    }
    if (g_useNameHash) {
        ImGui::SetNextItemWidth(scaling::pixels(180.0F));
        if (ImGui::InputScalar("Name hash",
                               ImGuiDataType_U32,
                               &g_nameHash,
                               nullptr,
                               nullptr,
                               "%08X",
                               ImGuiInputTextFlags_CharsHexadecimal)) {
            g_hasResult = false;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Optional type-1 definition hash. It is not the slot name.");
        }
    }
    draw_destination(*view.catalog, squad, squadRow);
    draw_spawn_rule(*view.catalog, squad);
    const squad_auth::Mode mode = g_mode == 0   ? squad_auth::Mode::mode0
                                  : g_mode == 1 ? squad_auth::Mode::mode2
                                                : squad_auth::Mode::reserve;
    const std::optional<std::uint32_t> nameHash =
        g_useNameHash ? std::optional<std::uint32_t>{g_nameHash} : std::nullopt;
    const std::optional<std::uint32_t> destination =
        g_destinationRow == format::kAbsentIndex ? std::nullopt
                                                 : std::optional<std::uint32_t>{g_destinationRow};
    const std::optional<std::uint32_t> spawnRule =
        g_spawnRuleRow == format::kAbsentIndex ? std::nullopt
                                               : std::optional<std::uint32_t>{g_spawnRuleRow};
    const runtime::Status available = runtime::availability(
        view, squadRow, counts, mode, nameHash, false, destination, spawnRule);
    const bool enabled = available == runtime::Status::ready && hasPositive;
    ImGui::BeginDisabled(!enabled);
    if (ImGui::Button("Place squad")) {
        g_lastResult =
            runtime::place(view, squadRow, counts, mode, nameHash, false, destination, spawnRule);
        g_hasResult = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!hasPositive && available == runtime::Status::ready) {
        ImGui::TextDisabled("set at least one count above zero");
    } else {
        ImGui::TextDisabled("%s", runtime::status_name(available));
    }
    if (g_hasResult) {
        ImGui::Text("Last place  %s", runtime::status_name(g_lastResult));
    }
}

/** Offers every exact type-43 scene which authors behavior for the selected type-1 squad. */
void draw_authored_behavior_scenes(const sdk::BoundView& view,
                                   const format::Squad& squad) noexcept {
    mission::Snapshot snapshot{};
    if (mission::query(view, snapshot) != mission::Status::ready || view.catalog == nullptr) {
        return;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const auto slots = catalog.slots();
    const auto occurrences = catalog.occurrences();
    std::uint32_t squadSlotRow = format::kAbsentIndex;
    for (std::uint32_t row = 0; row < slots.size(); ++row) {
        const format::Slot& slot = slots[row];
        if (slot.objectIndex == squad.objectIndex && slot.slotType == format::kSquadSlotType
            && slot.slotIndex == squad.slotIndex) {
            squadSlotRow = row;
            break;
        }
    }
    if (squadSlotRow == format::kAbsentIndex) {
        return;
    }
    section::header("Package-linked scenes", nullptr);
    ImGui::TextWrapped("Companion scenes, for an A/B test. Their effect on AI is unverified.");
    std::size_t sceneCount = 0;
    for (std::uint32_t sceneSlotRow = 0; sceneSlotRow < slots.size(); ++sceneSlotRow) {
        const format::Slot& sceneSlot = slots[sceneSlotRow];
        if (sceneSlot.slotType != format::kAuthoredSceneSlotType) {
            continue;
        }
        bool linked = false;
        for (const format::AuthoredSceneSquadEdge& edge :
             sdk::slot_authored_scene_squad_edges(catalog, sceneSlot)) {
            linked = linked || edge.squadSlotIndex == squadSlotRow;
        }
        if (!linked) {
            continue;
        }
        for (std::uint32_t occurrenceRow = 0; occurrenceRow < occurrences.size(); ++occurrenceRow) {
            const format::Occurrence& occurrence = occurrences[occurrenceRow];
            if (occurrence.scenarioIndex != view.scenarioRow
                || occurrence.stateIndex != snapshot.plan.stateRow
                || occurrence.objectIndex != sceneSlot.objectIndex) {
                continue;
            }
            ++sceneCount;
            ImGui::PushID(static_cast<int>(sceneSlotRow));
            ImGui::PushID(static_cast<int>(occurrenceRow));
            const mission::SceneStatus available =
                mission::authored_scene_availability(view, occurrenceRow, sceneSlotRow);
            ImGui::BeginDisabled(available != mission::SceneStatus::ready);
            if (ImGui::Button("Run authored behavior")) {
                g_behaviorSceneResult =
                    mission::activate_authored_scene(view, occurrenceRow, sceneSlotRow);
                g_hasBehaviorSceneResult = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            const std::string_view name = catalog.string(sceneSlot.name);
            ImGui::Text("%.*s  %s",
                        static_cast<int>(name.size()),
                        name.data(),
                        mission::status_name(available));
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    if (sceneCount != 0) {
        ImGui::TextDisabled(
            "Type-1 placement has no movement goal field. Run a linked scene to see "
            "what the package pairs with it.");
        if (g_hasBehaviorSceneResult) {
            ImGui::Text("Last behavior scene  %s", mission::status_name(g_behaviorSceneResult));
        }
    }
}

/** Forgets which squad the inputs were built for, and its last place result. */
void reset_squad_action_inputs() noexcept {
    g_initializedSquad = format::kAbsentIndex;
    g_hasResult = false;
}

} // namespace sunrise::server::ui::activity_host::sdk_squad_view
