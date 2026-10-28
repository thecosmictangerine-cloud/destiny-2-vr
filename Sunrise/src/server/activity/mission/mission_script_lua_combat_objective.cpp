#include <array>
#include <cmath>
#include <limits>

#include "../../../middleware/bap/activity_message/squad_objective_auth.h"
#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"
#include "mission_script_squad_sense.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

namespace objective = middleware::bap::activity_message::squad_objective;
namespace format = state::activity_sdk::format;
/** Returned task-group values compare by their two SDK identity fields. */
constexpr const char* kTaskGroupMetatable = "sunrise.mission.task_group";

/** Resolves one generated task-group reference through the pinned native SDK. */
[[nodiscard]] CombatObjectiveGroupDefinition task_group(lua_State* state, int table) {
    luaL_checktype(state, table, LUA_TTABLE);
    const lua_Integer slotRow = directive_integer(state, table, "slot_row");
    const lua_Integer group = directive_integer(state, table, "group_index");
    Impl* const impl = impl_from_state(state);
    CombatObjectiveGroupDefinition output{};
    if (slotRow < 0 || slotRow > (std::numeric_limits<std::uint32_t>::max)() || group < 0
        || group >= objective::kTaskGroupCount || impl == nullptr
        || impl->definitions.resolveCombatObjectiveGroup == nullptr
        || !impl->definitions.resolveCombatObjectiveGroup(impl->definitions.context,
                                                          static_cast<std::uint32_t>(slotRow),
                                                          static_cast<std::uint32_t>(group),
                                                          output)) {
        static_cast<void>(luaL_error(state, "task group does not belong to this SDK objective"));
    }
    return output;
}

/** Compares a returned task-group value with an extracted task-group constant. */
int task_group_equal(lua_State* state) {
    const auto left = task_group(state, 1);
    const auto right = task_group(state, 2);
    lua_pushboolean(state, left.slotRow == right.slotRow && left.groupIndex == right.groupIndex);
    return 1;
}

/** Keeps equality semantic while preventing scripts from replacing the native metatable. */
void set_task_group_metatable(lua_State* state) {
    if (luaL_newmetatable(state, kTaskGroupMetatable) != 0) {
        lua_pushcfunction(state, &task_group_equal);
        lua_setfield(state, -2, "__eq");
        lua_pushboolean(state, false);
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
}

} // namespace

/** Assigns a generated combat objective while native state owns evaluation revisions. */
int slot_assign_combat_objective(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 5> kDeclared{
        "objective", "task_group", "reconsider", "reserved", "refresh_player_awareness"};
    refuse_unknown_arguments(state, kDeclared);
    const auto reference = checked_argument<SlotHandle>(state, "objective", kSlotMetatable);
    SlotDefinition squad{};
    SlotDefinition target{};
    if (!current_slot(state, *handle, squad) || !current_slot(state, reference, target)
        || squad.slotType != format::kSquadSlotType
        || squad.componentClass != format::kSquadComponentClass
        || squad.authSchema != objective::kSchema || target.slotType != format::kObjectiveSlotType
        || target.componentClass != format::kObjectiveComponentClass
        || target.authSchema != middleware::bap::activity_message::scriptable_auth::kType3Schema
        || (squad.flags & format::kSlotSchemaJoinExact) == 0
        || (target.flags & format::kSlotSchemaJoinExact) == 0
        || squad.registryKey != target.registryKey) {
        return luaL_error(state, "combat objective requires exact slots in the same registry");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::assignCombatObjective;
    intent.firstRow = squad.nativeRow;
    intent.secondRow = target.nativeRow;
    intent.entryIndex = objective::kNoTaskGroup;
    lua_getfield(state, 2, "task_group");
    if (!lua_isnil(state, -1)) {
        const auto group = task_group(state, -1);
        if (group.slotRow != target.nativeRow) {
            return luaL_error(state, "task group belongs to another objective");
        }
        intent.entryIndex = static_cast<std::int32_t>(group.groupIndex);
    }
    lua_pop(state, 1);
    intent.objectiveReconsider = optional_boolean_argument(state, "reconsider", false);
    intent.objectiveRefreshAwareness =
        optional_boolean_argument(state, "refresh_player_awareness", false);
    lua_getfield(state, 2, "reserved");
    intent.objectivePreserveReservation = lua_isnil(state, -1);
    lua_pop(state, 1);
    intent.objectiveReserved = optional_boolean_argument(state, "reserved", false);
    if (!intent.objectiveReconsider && frame.event != nullptr
        && frame.event->kind == host::EventKind::squadState
        && frame.event->firstRegistryKey == squad.registryKey
        && frame.event->slotObjectTag == squad.objectTag
        && frame.event->firstSlotIndex == squad.slotIndex) {
        intent.expectedObjectiveRevision = frame.event->squadObjectiveRevision;
    }
    return queue_intent(state, frame, intent);
}

/** Returns a reachable cost and whether this report supplies a qualified value for the group. */
int event_task_cost(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 1> kDeclared{"group"};
    refuse_unknown_arguments(state, kDeclared);
    lua_getfield(state, 2, "group");
    const auto group = task_group(state, -1);
    lua_pop(state, 1);
    const bool qualified = event.kind == host::EventKind::squadState
                           && event.squadObjectiveCostQualified
                           && group.registryKey == event.squadObjectiveRegistryKey
                           && group.slotIndex == event.squadObjectiveSlotIndex
                           && (event.squadObjectiveCostMask & (1U << group.groupIndex)) != 0;
    const float cost = event.squadObjectiveCosts[group.groupIndex];
    const bool known =
        qualified && std::isfinite(cost) && cost >= 0 && cost <= kMaximumObjectiveCost;
    if (known && cost < kMaximumObjectiveCost) {
        lua_pushnumber(state, cost);
    } else {
        lua_pushnil(state);
    }
    lua_pushboolean(state, known);
    return 2;
}

/** Returns the selected authored group and whether the squad is assigned to this objective. */
int event_task_group(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 1> kDeclared{"objective"};
    refuse_unknown_arguments(state, kDeclared);
    const auto handle = checked_argument<SlotHandle>(state, "objective", kSlotMetatable);
    SlotDefinition target{};
    if (!current_slot(state, handle, target) || target.slotType != format::kObjectiveSlotType) {
        return luaL_error(state, "task group needs an SDK objective");
    }
    const bool assigned = event.kind == host::EventKind::squadState
                          && event.squadObjectiveRegistryKey == target.registryKey
                          && event.squadObjectiveSlotIndex == target.slotIndex;
    if (assigned && event.squadObjectiveTaskGroup >= 0) {
        lua_createtable(state, 0, 2);
        lua_pushinteger(state, target.nativeRow);
        lua_setfield(state, -2, "slot_row");
        lua_pushinteger(state, event.squadObjectiveTaskGroup);
        lua_setfield(state, -2, "group_index");
        set_task_group_metatable(state);
    } else {
        lua_pushnil(state);
    }
    lua_pushboolean(state, assigned);
    return 2;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
