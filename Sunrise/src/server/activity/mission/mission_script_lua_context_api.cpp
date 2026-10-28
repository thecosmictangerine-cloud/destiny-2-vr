#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <tuple>

#include "../../../state/activity/membership/definition.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_peer_internal.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/**
 * Groups exact authored objects by native owner and reserves bounded output batches.
 * @param state Lua call holding the context and named object list.
 * @return One array of RequestKeys.
 */
[[nodiscard]] int context_activate_objects(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 2> kDeclared{"slots", "active"};
    refuse_unknown_arguments(state, kDeclared);
    const bool active = optional_boolean_argument(state, "active", true);
    lua_getfield(state, 2, "slots");
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    std::size_t entries = 0;
    lua_pushnil(state);
    while (lua_next(state, list) != 0) {
        if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1
            || static_cast<std::uint64_t>(lua_tointeger(state, -2)) > count) {
            return luaL_error(state, "object collection must be a dense list");
        }
        ++entries;
        lua_pop(state, 1);
    }
    if (entries != count) {
        return luaL_error(state, "object collection must be a dense list");
    }
    CallFrame& frame = active_frame(state);
    frame.candidate.objectBatch.clear();
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        const auto* handle =
            static_cast<const SlotHandle*>(luaL_testudata(state, -1, kSlotMetatable));
        SlotDefinition slot{};
        namespace format = ::sunrise::state::activity_sdk::format;
        const bool resolved =
            handle != nullptr ? current_slot(state, *handle, slot) : resolve_slot(state, -1, slot);
        if (!resolved || slot.slotType != format::kObjectSlotType
            || slot.componentClass != format::kObjectComponentClass
            || slot.senseSchema != format::kObjectSenseSchema
            || slot.authSchema != format::kObjectAuthSchema
            || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
            return luaL_error(state, "object collection requires exact authored type-4 slots");
        }
        lua_pop(state, 1);
        try {
            frame.candidate.objectBatch.push_back(slot);
        } catch (const std::bad_alloc&) {
            frame.intentAllocationFailed = true;
            return luaL_error(state, "object collection allocation failed");
        }
    }
    auto& slots = frame.candidate.objectBatch;
    std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) noexcept {
        return std::tie(a.objectTag, a.registryKey, a.nativeRow)
               < std::tie(b.objectTag, b.registryKey, b.nativeRow);
    });
    for (std::size_t index = 1; index < slots.size(); ++index) {
        if (slots[index - 1].nativeRow == slots[index].nativeRow) {
            return luaL_error(state, "object collection repeats a SlotView");
        }
    }
    lua_newtable(state);
    lua_Integer requestCount = 0;
    for (std::size_t first = 0; first < slots.size();) {
        Intent intent{};
        intent.kind = IntentKind::setObjectActive;
        intent.active = active;
        intent.firstRow = slots[first].nativeRow;
        std::size_t next = first + 1;
        while (next < slots.size() && slots[next].objectTag == slots[first].objectTag
               && slots[next].registryKey == slots[first].registryKey
               && intent.burstRowCount < intent.burstRows.size()) {
            intent.burstRows[intent.burstRowCount++] = slots[next++].nativeRow;
        }
        static_cast<void>(queue_intent(state, frame, intent));
        lua_rawseti(state, -2, ++requestCount);
        first = next;
    }
    return 1;
}

/**
 * Completes this attempt through the native lifetime owner.
 * @param state Lua call holding
 * the context and empty argument table.
 * @return One RequestKey.
 */
[[nodiscard]] int context_complete_mission(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    if (impl_from_state(state)->attempt.complete) {
        return luaL_error(state, "mission attempt is already complete");
    }
    Intent intent{};
    intent.kind = IntentKind::setLifetime;
    intent.lifetimeState = ::sunrise::state::activity::mission::kCompletedLifetimeState;
    return queue_intent(state, active_frame(state), intent);
}

[[nodiscard]] int context_squad(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SquadDefinition definition{};
    if (!resolve_squad(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous activity squad");
    }
    push_handle(state, kSquadMetatable, SquadHandle{definition.localRow});
    return 1;
}

/**
 * Arms a native hard wipe at an authored spawn set, or releases one with its request key.
 * The Lua caller passes `release_request` as the decimal string of the original key.
 */
[[nodiscard]] int context_restart_checkpoint(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 3> kDeclared{
        "region", "spawn_set_hash", "release_request"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer region = optional_integer_argument(state, "region", -1);
    const lua_Integer hash = optional_integer_argument(state, "spawn_set_hash", 0);
    if (region < 0 || region > ::sunrise::state::activity::membership::kMaximumSliceSetIndex
        || hash <= 0 || hash >= (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "checkpoint requires an authored region and spawn-set hash");
    }
    std::uint64_t release = 0;
    lua_getfield(state, 2, "release_request");
    if (!lua_isnil(state, -1)) {
        std::size_t length = 0;
        const char* const value = luaL_checklstring(state, -1, &length);
        const auto parsed = std::from_chars(value, value + length, release);
        if (parsed.ec != std::errc{} || parsed.ptr != value + length || release == 0) {
            return luaL_error(state, "checkpoint release requires the original RequestKey.value");
        }
    }
    lua_pop(state, 1);
    Intent intent{};
    intent.kind = IntentKind::restartCheckpoint;
    intent.checkpointReleaseRequest = release;
    intent.effectiveRegion = static_cast<std::int32_t>(region);
    intent.checkpointSpawnHash = static_cast<std::uint32_t>(hash);
    return queue_intent(state, active_frame(state), intent);
}

/**
 * Holds the player's spawn, or releases it.
 * A held spawn leaves the player without a body, so an opening cutscene runs before the arrival.
 * @param state Lua call holding the context and one `active` boolean.
 * @return One RequestKey.
 */
[[nodiscard]] int context_hold_spawn(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 1> kDeclared{"active"};
    refuse_unknown_arguments(state, kDeclared);
    lua_getfield(state, 2, "active");
    if (!lua_isboolean(state, -1)) {
        return luaL_argerror(state, 2, "hold_spawn requires an active boolean");
    }
    Intent intent{};
    intent.kind = IntentKind::holdSpawn;
    intent.active = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return queue_intent(state, active_frame(state), intent);
}

[[nodiscard]] int context_scene(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SceneDefinition definition{};
    if (!resolve_scene(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous authored scene");
    }
    push_handle(state, kSceneMetatable, SceneHandle{definition.localRow});
    return 1;
}

[[nodiscard]] int context_slot(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SlotDefinition definition{};
    if (!resolve_slot(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous activity slot");
    }
    push_handle(state, kSlotMetatable, SlotHandle{definition.localRow});
    return 1;
}

} // namespace

/** Resolves the squad one Lua argument names, by handle or by index. */
[[nodiscard]] bool resolve_squad(lua_State* state, int selector, SquadDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSquadRow != nullptr
               && impl->definitions.resolveSquadRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSquadId != nullptr
           && impl->definitions.resolveSquadId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

/** Resolves the scene one Lua argument names, by handle or by index. */
[[nodiscard]] bool resolve_scene(lua_State* state, int selector, SceneDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSceneRow != nullptr
               && impl->definitions.resolveSceneRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSceneId != nullptr
           && impl->definitions.resolveSceneId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

/** Resolves the slot one Lua argument names, by handle or by index. */
[[nodiscard]] bool resolve_slot(lua_State* state, int selector, SlotDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSlotRow != nullptr
               && impl->definitions.resolveSlotRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSlotId != nullptr
           && impl->definitions.resolveSlotId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

[[nodiscard]] bool
resolve_message_name(lua_State* state, std::string_view name, ActivityMessageDefinition& output) {
    Impl* const impl = impl_from_state(state);
    return impl != nullptr && impl->definitions.resolveActivityMessageName != nullptr
           && impl->definitions.resolveActivityMessageName(impl->definitions.context, name, output);
}

/**
 * Reads the optional omit list: generated slots whose owning object stays out of the seed.
 * @return False with the Lua error already raised.
 */
[[nodiscard]] bool parse_seed_omissions(lua_State* state, int index, Intent& intent) {
    if (lua_isnoneornil(state, index)) {
        return true;
    }
    luaL_checktype(state, index, LUA_TTABLE);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, index));
    if (count < 0
        || static_cast<std::size_t>(count)
               > ::sunrise::state::activity::mission::kMissionSeedOmitCapacity) {
        static_cast<void>(luaL_argerror(state, index, "mission seed omit list is too long"));
        return false;
    }
    for (lua_Integer entry = 1; entry <= count; ++entry) {
        lua_rawgeti(state, index, entry);
        SlotDefinition definition{};
        const bool resolved = resolve_slot(state, lua_gettop(state), definition);
        lua_pop(state, 1);
        if (!resolved) {
            static_cast<void>(luaL_argerror(state, index, "unknown or ambiguous activity slot"));
            return false;
        }
        intent.seedOmissions[static_cast<std::size_t>(entry - 1)] = {definition.objectTag,
                                                                     definition.registryKey};
    }
    intent.seedOmissionCount = static_cast<std::uint8_t>(count);
    return true;
}

/** Queues one generated mission state by its authored effective region. */
[[nodiscard]] int context_select_state(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    luaL_checktype(state, 2, LUA_TTABLE);
    lua_getfield(state, 2, "region_index");
    if (!lua_isinteger(state, -1)) {
        return luaL_argerror(state, 2, "generated mission state has no integer region_index");
    }
    const lua_Integer region = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (region < 0
        || static_cast<std::uint64_t>(region)
               > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        return luaL_argerror(state, 2, "generated mission state region_index is outside i32");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::selectMissionState;
    intent.effectiveRegion = static_cast<std::int32_t>(region);
    if (!parse_seed_omissions(state, 3, intent)) {
        return 0;
    }
    if (lua_istable(state, 3)) {
        lua_getfield(state, 3, "retire_placed_props");
        if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
            return luaL_argerror(state, 3, "retire_placed_props must be a boolean");
        }
        intent.retirePlacedProps = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
    }
    return queue_intent(state, frame, intent);
}

/** Lua index for the mission context: its collections, phase, variables and timers. */
[[nodiscard]] int context_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "sdk_build_id") {
        lua_pushstring(state, impl->identity.sdkBuildId.data());
    } else if (key == "activity_id") {
        lua_pushstring(state, impl->identity.activityId.data());
    } else if (key == "activity_row") {
        lua_pushinteger(state, impl->identity.activityRow);
    } else if (key == "definition_hash") {
        lua_pushinteger(state, impl->identity.definitionHash);
    } else if (key == "activity_role") {
        lua_pushstring(state, impl->identity.publicTarget ? "public" : "private");
    } else if (key == "player_key") {
        push_u64_string(state, impl->identity.playerKey);
    } else if (key == "sdk") {
        push_activity(state);
    } else if (key == "lifetime") {
        push_lifetime(state);
    } else if (key == "attempt_generation") {
        push_u64_string(state, impl->attempt.generation);
    } else if (key == "mission_complete") {
        lua_pushboolean(state, impl->attempt.complete);
    } else if (key == "complete_mission") {
        lua_pushcfunction(state, &context_complete_mission);
    } else if (key == "activate_objects") {
        lua_pushcfunction(state, &context_activate_objects);
    } else if (key == "cohort") {
        lua_pushcfunction(state, &context_cohort);
    } else if (key == "peers") {
        push_peers(state);
    } else if (key == "squad") {
        lua_pushcfunction(state, &context_squad);
    } else if (key == "scene") {
        lua_pushcfunction(state, &context_scene);
    } else if (key == "slot") {
        lua_pushcfunction(state, &context_slot);
    } else if (key == "hold_spawn") {
        lua_pushcfunction(state, &context_hold_spawn);
    } else if (key == "select_state") {
        lua_pushcfunction(state, &context_select_state);
    } else if (key == "restart_checkpoint") {
        lua_pushcfunction(state, &context_restart_checkpoint);
    } else if (key == "set_phase") {
        lua_pushcfunction(state, &context_set_phase);
    } else if (key == "set_variable") {
        lua_pushcfunction(state, &context_set_variable);
    } else if (key == "clear_variable") {
        lua_pushcfunction(state, &context_clear_variable);
    } else if (key == "start_timer") {
        lua_pushcfunction(state, &context_start_timer);
    } else if (key == "cancel_timer") {
        lua_pushcfunction(state, &context_cancel_timer);
    } else if (!push_key_context_member(state, key)) {
        lua_pushnil(state);
    }
    return 1;
}

void register_context_metatables(lua_State* state) {
    register_metatable(state, kContextMetatable, &context_index);
    register_population_metatables(state);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
