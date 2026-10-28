#include "activity_host_sdk_squad_view.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "../../../client/ui/activity/authored_placement_marker.h"
#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../bap/runtime.h"
#include "activity_host_anchor_render_controls.h"
#include "activity_host_sdk_squad_actions.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::sdk_squad_view {
namespace {

namespace format = state::activity_sdk::format;
namespace marker = client::ui::activity::authored_placement_marker;
namespace render_controls = server::ui::activity_host::anchor_render_controls;
namespace scaling = core::ui::scaling::dpi;
namespace section = core::ui::components::section;
namespace sdk = state::activity_sdk;
namespace tables = middleware::content::packages::tables;
/** Filter storage per squad row: the whole identity document, and one formatted fragment. */
constexpr std::size_t kSearchDocumentCapacity = 8'192;
constexpr std::size_t kSearchFragmentCapacity = 512;

std::weak_ptr<const sdk::Catalog> g_selectionCatalog{};
state::activity::SessionBinding g_selectionBinding{};
std::uint64_t g_selectionClientGeneration{};
std::uint32_t g_selectedSquad{format::kAbsentIndex};
ImGuiTextFilter g_squadFilter{};
bool g_currentStateOnly{};

/** One filtered global squad row and whether its occurrence belongs to the live region. */
struct BrowserRow final {
    std::uint32_t squadRow{format::kAbsentIndex};
    bool currentState{};
};

std::vector<BrowserRow> g_listedSquads{};
/** Every anchor this scenario can draw. The search must never change it. */
std::vector<marker::Anchor> g_scenarioAnchors{};
/** Anchors owned by the rows the search currently lists, for the bulk-select action only. */
std::vector<marker::Anchor> g_listedAnchors{};
/** The scenario anchor set is fixed for one binding, so it is rebuilt only when that changes. */
bool g_scenarioAnchorsValid{};

/** @return The global row index of one borrowed squad row. */
[[nodiscard]] std::uint32_t global_row(const sdk::Catalog& catalog,
                                       const format::Squad& squad) noexcept {
    const auto squads = catalog.squads();
    const std::size_t row = static_cast<std::size_t>(&squad - squads.data());
    return row < squads.size() ? static_cast<std::uint32_t>(row) : format::kAbsentIndex;
}

/** @return True when one global row belongs to the bound scenario subset. */
[[nodiscard]] bool contains(std::span<const format::Squad> squads,
                            const sdk::Catalog& catalog,
                            std::uint32_t row) noexcept {
    return std::any_of(squads.begin(), squads.end(), [&catalog, row](const auto& squad) noexcept {
        return global_row(catalog, squad) == row;
    });
}

/** Resets selection and action inputs whenever the exact SDK binding changes. */
void sync_selection(const sdk::BoundView& view, std::span<const format::Squad> squads) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    const std::uint32_t first =
        squads.empty() ? format::kAbsentIndex : global_row(catalog, squads.front());
    if (g_selectionCatalog.lock() != view.catalog || !same_binding(g_selectionBinding, view.binding)
        || g_selectionClientGeneration != view.activityClientGeneration) {
        g_selectionCatalog = view.catalog;
        g_selectionBinding = view.binding;
        g_selectionClientGeneration = view.activityClientGeneration;
        g_selectedSquad = first;
        reset_squad_action_inputs();
        g_scenarioAnchorsValid = false;
    }
    if (!contains(squads, catalog, g_selectedSquad)) {
        g_selectedSquad = first;
        reset_squad_action_inputs();
    }
}

/** @return One source slot retained by a generated squad row. */
[[nodiscard]] const format::Slot* source_slot(const sdk::Catalog& catalog,
                                              const format::Squad& squad) noexcept {
    const auto slots = catalog.slots();
    return squad.slotIndex < slots.size() ? &slots[squad.slotIndex] : nullptr;
}

/** @return The source slot's strongest generated display label. */
[[nodiscard]] std::string_view source_label(const sdk::Catalog& catalog,
                                            const format::Squad& squad) noexcept {
    const format::Slot* const slot = source_slot(catalog, squad);
    if (slot != nullptr) {
        const std::string_view name = catalog.string(slot->name);
        if (!name.empty()) {
            return name;
        }
        const std::string_view id = catalog.string(slot->id);
        if (!id.empty()) {
            return id;
        }
    }
    return catalog.string(squad.id);
}

/** One squad row's whole identity text, held in fixed storage the filter reads once. */
struct SearchDocument final {
    std::array<char, kSearchDocumentCapacity> text{};
    std::size_t length{};
};

/** Appends one nonempty generated string to a row-wide search document. */
void append_search_text(SearchDocument& document, std::string_view value) noexcept {
    if (value.empty() || document.length + value.size() + 2 > document.text.size()) {
        return;
    }
    document.text[document.length++] = ' ';
    std::copy_n(value.data(), value.size(), document.text.data() + document.length);
    document.length += value.size();
    document.text[document.length] = '\0';
}

/** Appends one bounded formatted identity fragment to a row-wide search document. */
template <typename... Arguments>
void append_search_format(SearchDocument& document, const char* pattern, Arguments... arguments) {
    std::array<char, kSearchFragmentCapacity> value{};
    const int length = std::snprintf(value.data(), value.size(), pattern, arguments...);
    if (length <= 0) {
        return;
    }
    append_search_text(
        document, {value.data(), (std::min)(static_cast<std::size_t>(length), value.size() - 1)});
}

/** Builds the complete generated identity document consumed by ImGuiTextFilter. */
[[nodiscard]] bool squad_matches_filter(const sdk::Catalog& catalog,
                                        const format::Squad& squad,
                                        std::uint32_t squadRow) {
    if (!g_squadFilter.IsActive()) {
        return true;
    }
    SearchDocument search{};
    append_search_text(search, catalog.string(squad.id));
    append_search_format(search,
                         "squad %u row %u spawner 0x%08X spawn-rule 0x%08X flags 0x%08X",
                         static_cast<unsigned>(squadRow),
                         static_cast<unsigned>(squadRow),
                         static_cast<unsigned>(squad.spawnerConfigTag),
                         static_cast<unsigned>(squad.spawnRuleConfigTag),
                         static_cast<unsigned>(squad.flags));

    const auto slots = catalog.slots();
    if (squad.slotIndex < slots.size()) {
        const format::Slot& slot = slots[squad.slotIndex];
        append_search_text(search, catalog.string(slot.name));
        append_search_text(search, catalog.string(slot.id));
        for (const format::Text& alias : sdk::slot_aliases(catalog, slot)) {
            append_search_text(search, catalog.string(alias.value));
        }
        append_search_format(search,
                             "slot-row %u slot %u type %u component 0x%08X sense 0x%08X auth "
                             "0x%08X",
                             static_cast<unsigned>(squad.slotIndex),
                             static_cast<unsigned>(slot.slotIndex),
                             static_cast<unsigned>(slot.slotType),
                             static_cast<unsigned>(slot.componentClass),
                             static_cast<unsigned>(slot.senseSchema),
                             static_cast<unsigned>(slot.authSchema));
    }

    const auto objects = catalog.objects();
    if (squad.objectIndex < objects.size()) {
        const format::Object& object = objects[squad.objectIndex];
        append_search_text(search, catalog.string(object.id));
        append_search_format(search,
                             "object-row %u object-tag 0x%08X object-key 0x%08X",
                             static_cast<unsigned>(squad.objectIndex),
                             static_cast<unsigned>(object.objectTag),
                             static_cast<unsigned>(object.objectKey));
    }

    const auto occurrences = catalog.occurrences();
    if (squad.occurrenceIndex < occurrences.size()) {
        const format::Occurrence& occurrence = occurrences[squad.occurrenceIndex];
        append_search_text(search, catalog.string(occurrence.id));
        append_search_text(search, catalog.string(occurrence.contextRegistryKey));
        append_search_text(search, catalog.string(occurrence.registryId));
        append_search_text(search, catalog.string(occurrence.entryId));
        append_search_format(search,
                             "occurrence-row %u registry-field 0x%08X object-ordinal %u bubble-row "
                             "%u state-row %u",
                             static_cast<unsigned>(squad.occurrenceIndex),
                             static_cast<unsigned>(occurrence.registryField),
                             static_cast<unsigned>(occurrence.objectOrdinal),
                             static_cast<unsigned>(occurrence.bubbleIndex),
                             static_cast<unsigned>(occurrence.stateIndex));
    }

    const auto actorClasses = catalog.actor_classes();
    for (const format::SquadMember& member : sdk::squad_members(catalog, squad)) {
        append_search_text(search, catalog.string(member.id));
        append_search_format(search,
                             "member %u key 0x%08X actor-row %u",
                             static_cast<unsigned>(member.memberOrdinal),
                             static_cast<unsigned>(member.memberKey),
                             static_cast<unsigned>(member.actorClassIndex));
        if (member.actorClassIndex < actorClasses.size()) {
            const format::ActorClass& actor = actorClasses[member.actorClassIndex];
            append_search_text(search, catalog.string(actor.id));
            append_search_format(search,
                                 "actor 0x%08X name-hash 0x%08X rsat 0x%08X",
                                 static_cast<unsigned>(actor.definitionTag),
                                 static_cast<unsigned>(actor.nameHash),
                                 static_cast<unsigned>(actor.rsatTag));
        }
    }

    for (const format::SquadAnchor& anchor : sdk::squad_anchors(catalog, squad)) {
        append_search_text(search, catalog.string(anchor.id));
        const float x = std::bit_cast<float>(anchor.positionBits[0]);
        const float y = std::bit_cast<float>(anchor.positionBits[1]);
        const float z = std::bit_cast<float>(anchor.positionBits[2]);
        append_search_format(search,
                             "point %u 0x%08X[%u] identity 0x%016llX position %.3f %.3f %.3f "
                             "%.9g %.9g %.9g",
                             static_cast<unsigned>(anchor.pointOrdinal),
                             static_cast<unsigned>(anchor.objectListTag),
                             static_cast<unsigned>(anchor.placementOrdinal),
                             static_cast<unsigned long long>(anchor.placedEntryIdentity),
                             static_cast<double>(x),
                             static_cast<double>(y),
                             static_cast<double>(z),
                             static_cast<double>(x),
                             static_cast<double>(y),
                             static_cast<double>(z));
    }
    return g_squadFilter.PassFilter(search.text.data(), search.text.data() + search.length);
}

/** Resolves the generated bubble/state ordinals owned by one squad occurrence. */
[[nodiscard]] bool squad_region(const sdk::Catalog& catalog,
                                const format::Squad& squad,
                                std::uint32_t& bubbleOrdinal,
                                std::uint32_t& stateOrdinal) noexcept {
    bubbleOrdinal = 0;
    stateOrdinal = 0;
    const auto occurrences = catalog.occurrences();
    const auto bubbles = catalog.bubbles();
    const auto states = catalog.states();
    if (squad.occurrenceIndex >= occurrences.size()) {
        return false;
    }
    const format::Occurrence& occurrence = occurrences[squad.occurrenceIndex];
    if (occurrence.scenarioIndex != squad.scenarioIndex
        || occurrence.objectIndex != squad.objectIndex || occurrence.bubbleIndex >= bubbles.size()
        || occurrence.stateIndex >= states.size()) {
        return false;
    }
    const format::Bubble& bubble = bubbles[occurrence.bubbleIndex];
    const format::State& state = states[occurrence.stateIndex];
    if (bubble.scenarioIndex != squad.scenarioIndex || state.scenarioIndex != squad.scenarioIndex
        || state.bubbleIndex != occurrence.bubbleIndex) {
        return false;
    }
    bubbleOrdinal = bubble.bubbleOrdinal;
    stateOrdinal = state.stateOrdinal;
    return true;
}

/** @return True when one squad occurrence belongs to the live ActivityClient region. */
[[nodiscard]] bool squad_is_current(const sdk::Catalog& catalog,
                                    const format::Squad& squad,
                                    std::int32_t effectiveRegion) noexcept {
    if (effectiveRegion < 0) {
        return false;
    }
    std::uint32_t bubbleOrdinal = 0;
    std::uint32_t stateOrdinal = 0;
    const std::uint32_t region = static_cast<std::uint32_t>(effectiveRegion);
    return squad_region(catalog, squad, bubbleOrdinal, stateOrdinal)
           && bubbleOrdinal == region / tables::kSliceSetIndexFactor
           && stateOrdinal == region % tables::kSliceSetIndexFactor;
}

/** Appends every exact anchor child of one squad row. */
void append_squad_anchors(const sdk::BoundView& view,
                          std::uint32_t squadRow,
                          const format::Squad& squad,
                          std::vector<marker::Anchor>& output) {
    for (std::uint32_t ordinal = 0; ordinal < squad.anchors.count; ++ordinal) {
        marker::Anchor anchor{};
        if (marker::sdk_squad_anchor(view, squadRow, squad.anchors.first + ordinal, anchor)) {
            output.push_back(anchor);
        }
    }
}

/** Builds the listed rows, and separately the full scenario row set the world may draw. */
[[nodiscard]] bool materialize_rows(const sdk::BoundView& view,
                                    std::span<const format::Squad> squads,
                                    std::int32_t effectiveRegion,
                                    std::size_t& currentCount) noexcept {
    currentCount = 0;
    try {
        const sdk::Catalog& catalog = *view.catalog;
        for (const format::Squad& squad : squads) {
            currentCount += squad_is_current(catalog, squad, effectiveRegion) ? 1U : 0U;
        }
        std::vector<BrowserRow> listed;
        listed.reserve(squads.size());
        std::vector<marker::Anchor> scenarioAnchors;
        std::vector<marker::Anchor> listedAnchors;
        for (const bool currentPass : {true, false}) {
            for (const format::Squad& squad : squads) {
                const bool current = squad_is_current(catalog, squad, effectiveRegion);
                const std::uint32_t row = global_row(catalog, squad);
                if (current != currentPass || row == format::kAbsentIndex) {
                    continue;
                }
                if (!g_scenarioAnchorsValid) {
                    append_squad_anchors(view, row, squad, scenarioAnchors);
                }
                if ((current || !g_currentStateOnly) && squad_matches_filter(catalog, squad, row)) {
                    listed.push_back({row, current});
                    append_squad_anchors(view, row, squad, listedAnchors);
                }
            }
        }
        g_listedSquads.swap(listed);
        if (!g_scenarioAnchorsValid) {
            g_scenarioAnchors.swap(scenarioAnchors);
            g_scenarioAnchorsValid = true;
        }
        g_listedAnchors.swap(listedAnchors);
        return true;
    } catch (...) {
        return false;
    }
}

/** Counts exact anchor children from one squad that are in the copied marker set. */
[[nodiscard]] std::size_t selected_anchor_count(const sdk::BoundView& view,
                                                std::uint32_t squadRow,
                                                const marker::Context& context,
                                                const marker::State& selected) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    if (squadRow >= catalog.squads().size()) {
        return 0;
    }
    const format::Squad& squad = catalog.squads()[squadRow];
    std::size_t count = 0;
    for (std::uint32_t ordinal = 0; ordinal < squad.anchors.count; ++ordinal) {
        marker::Anchor anchor{};
        const std::uint32_t anchorRow = squad.anchors.first + ordinal;
        if (marker::sdk_squad_anchor(view, squadRow, anchorRow, anchor)
            && marker::contains(selected,
                                context,
                                marker::AnchorSource::sdkSquadAnchor,
                                anchor.sourceRow,
                                anchor.ownerRow)) {
            ++count;
        }
    }
    return count;
}

/** Ticks or clears every exact anchor child of one squad. It never changes what Show draws. */
void set_squad_rendering(const sdk::BoundView& view,
                         std::uint32_t squadRow,
                         const marker::Context& context,
                         bool enabled,
                         marker::State& selected) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    if (squadRow >= catalog.squads().size()) {
        return;
    }
    const format::Squad& squad = catalog.squads()[squadRow];
    for (std::uint32_t ordinal = 0; ordinal < squad.anchors.count; ++ordinal) {
        marker::Anchor anchor{};
        const std::uint32_t anchorRow = squad.anchors.first + ordinal;
        if (!marker::sdk_squad_anchor(view, squadRow, anchorRow, anchor)) {
            continue;
        }
        const marker::State current = marker::snapshot();
        const bool present = marker::contains(current,
                                              context,
                                              marker::AnchorSource::sdkSquadAnchor,
                                              anchor.sourceRow,
                                              anchor.ownerRow);
        if (present != enabled) {
            marker::toggle({context, anchor});
        }
    }
    selected = marker::snapshot();
}

/** Publishes every scenario point and draws the shared render controls. */
void draw_world_render_controls(const marker::Context& context, marker::State& selected) noexcept {
    ImGui::PushID("sdk_squad_world_render");
    render_controls::draw_options(selected);
    const bool published =
        marker::publish_rows(context, g_scenarioAnchors, marker::PublishedSource::explicitRows);
    if (ImGui::Button("Tick listed")) {
        (void)marker::select_many(
            context, g_listedAnchors, g_listedAnchors.size() > marker::kSelectionCapacity);
        selected = marker::snapshot();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ticks every point owned by a listed squad.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Untick all")) {
        marker::clear();
        selected = marker::snapshot();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu point%s in this scenario",
                        g_scenarioAnchors.size(),
                        g_scenarioAnchors.size() == 1 ? "" : "s");
    if (!published) {
        ImGui::TextDisabled("Point source unavailable");
    }
    render_controls::draw_status(context, selected);
    ImGui::PopID();
}

/** Draws the list filters. They choose what the table shows and nothing else. */
void draw_squad_filters() noexcept {
    ImGui::SetNextItemWidth(scaling::pixels(360.0F));
    g_squadFilter.Draw("Search##sdk_squads");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Filters the list. The world still draws the same points.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Current state only##sdk_squads", &g_currentStateOnly);
}

/** Draws the listed squads, their world-point ticks, and the row highlight. */
void draw_squad_table(const sdk::BoundView& view,
                      std::span<const format::Squad> squads,
                      const marker::Context& context,
                      std::int32_t effectiveRegion,
                      std::size_t currentCount,
                      marker::State& selected) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    if (effectiveRegion >= 0) {
        ImGui::TextDisabled("%zu listed of %zu; %zu in the current region %d",
                            g_listedSquads.size(),
                            squads.size(),
                            currentCount,
                            effectiveRegion);
    } else {
        ImGui::TextDisabled(
            "%zu listed of %zu; current region unavailable", g_listedSquads.size(), squads.size());
    }
    if (squads.empty()) {
        ImGui::TextDisabled("This scenario has no squads.");
        return;
    }
    if (!ImGui::BeginTable(
            "##sdk_squads", 8, kWideTableFlags, table_layout::size(g_listedSquads.size()))) {
        return;
    }
    ImGui::TableSetupColumn("Draw");
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("region");
    ImGui::TableSetupColumn("slot");
    ImGui::TableSetupColumn("spawner");
    ImGui::TableSetupColumn("spawn rule");
    ImGui::TableSetupColumn("members");
    ImGui::TableSetupColumn("points");
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_listedSquads.size()));
    while (clipper.Step()) {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
            const BrowserRow browserRow = g_listedSquads[static_cast<std::size_t>(visible)];
            const std::uint32_t row = browserRow.squadRow;
            if (row >= catalog.squads().size()) {
                continue;
            }
            const format::Squad& squad = catalog.squads()[row];
            const format::Slot* const slot = source_slot(catalog, squad);
            const std::string_view label = source_label(catalog, squad);
            const std::string_view visibleLabel = label.empty() ? "unnamed squad" : label;
            std::array<char, 192> selectable{};
            (void)std::snprintf(selectable.data(),
                                selectable.size(),
                                "%.*s##squad_%u",
                                static_cast<int>(visibleLabel.size()),
                                visibleLabel.data(),
                                static_cast<unsigned>(row));

            std::size_t exactAnchors = 0;
            if (context.catalogKind == marker::CatalogKind::activitySdk) {
                for (std::uint32_t ordinal = 0; ordinal < squad.anchors.count; ++ordinal) {
                    marker::Anchor anchor{};
                    exactAnchors +=
                        marker::sdk_squad_anchor(view, row, squad.anchors.first + ordinal, anchor)
                            ? 1U
                            : 0U;
                }
            }
            const std::size_t selectedCount = selected_anchor_count(view, row, context, selected);
            bool rendered = selectedCount != 0;

            ImGui::PushID(static_cast<int>(row));
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(exactAnchors == 0);
            if (ImGui::Checkbox("##render", &rendered)) {
                set_squad_rendering(view, row, context, rendered, selected);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (exactAnchors == 0) {
                    ImGui::SetTooltip("This squad has no points.");
                } else {
                    ImGui::SetTooltip("%zu of %zu points ticked", selectedCount, exactAnchors);
                }
            }
            ImGui::TableNextColumn();
            if (table_layout::selectable(selectable.data(), g_selectedSquad == row)) {
                g_selectedSquad = row;
                reset_squad_action_inputs();
            }
            ImGui::TableNextColumn();
            std::uint32_t bubbleOrdinal = 0;
            std::uint32_t stateOrdinal = 0;
            if (!squad_region(catalog, squad, bubbleOrdinal, stateOrdinal)) {
                ImGui::TextDisabled("invalid");
            } else if (browserRow.currentState) {
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark),
                                   "current %u/%u",
                                   static_cast<unsigned>(bubbleOrdinal),
                                   static_cast<unsigned>(stateOrdinal));
            } else {
                ImGui::Text("%u/%u",
                            static_cast<unsigned>(bubbleOrdinal),
                            static_cast<unsigned>(stateOrdinal));
            }
            ImGui::TableNextColumn();
            if (slot == nullptr) {
                ImGui::TextDisabled("invalid row %u", static_cast<unsigned>(squad.slotIndex));
            } else {
                ImGui::Text("%u / type %u",
                            static_cast<unsigned>(slot->slotIndex),
                            static_cast<unsigned>(slot->slotType));
            }
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(squad.spawnerConfigTag));
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(squad.spawnRuleConfigTag));
            ImGui::TableNextColumn();
            ImGui::Text("%zu", sdk::squad_members(catalog, squad).size());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", sdk::squad_anchors(catalog, squad).size());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

} // namespace

std::string_view squad_display_name(const sdk::Catalog& catalog,
                                    const format::Squad& squad) noexcept {
    return source_label(catalog, squad);
}

/** Draws generated scenario squads and the guarded server-side place action. */
void draw(const sdk::BoundView& view, const format::Scenario& scenario) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    const auto squads = sdk::scenario_squads(catalog, scenario);
    sync_selection(view, squads);
    server::bap::ActivityLinkView link{};
    (void)server::bap::activity_link_view(view.binding, link);
    draw_squad_filters();
    std::size_t currentCount = 0;
    if (!materialize_rows(view, squads, link.effectiveRegion, currentCount)) {
        ImGui::TextDisabled("Squad row storage did not fit");
        marker::publish_no_rows();
        return;
    }
    marker::Context context{};
    marker::State selected = marker::snapshot();
    if (marker::sdk_context(view, context)) {
        draw_world_render_controls(context, selected);
    } else {
        ImGui::TextDisabled("The world cannot draw this activity");
        marker::publish_no_rows();
    }
    draw_squad_table(view, squads, context, link.effectiveRegion, currentCount, selected);
    if (g_selectedSquad >= catalog.squads().size()) {
        return;
    }
    const format::Squad& squad = catalog.squads()[g_selectedSquad];
    initialize_inputs(catalog, squad, g_selectedSquad);
    const std::string_view label = source_label(catalog, squad);
    const std::string_view visibleLabel = label.empty() ? "unnamed squad" : label;
    section::header("Place this squad", nullptr);
    ImGui::Text("%.*s", static_cast<int>(visibleLabel.size()), visibleLabel.data());
    draw_anchors(catalog, squad);
    draw_place_action(view, squad, g_selectedSquad);
    draw_authored_behavior_scenes(view, squad);
    if (ImGui::TreeNodeEx("Technical details##squad", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Text("squad row %u  spawner 0x%08X  spawn rule 0x%08X  flags 0x%08X",
                    static_cast<unsigned>(g_selectedSquad),
                    static_cast<unsigned>(squad.spawnerConfigTag),
                    static_cast<unsigned>(squad.spawnRuleConfigTag),
                    static_cast<unsigned>(squad.flags));
        ImGui::TextUnformatted("wire mode 0");
        ImGui::TreePop();
    }
}

} // namespace sunrise::server::ui::activity_host::sdk_squad_view
