#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_lua_world_internal.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

/** Spawn rules are type-66 slots. */
constexpr std::uint32_t kSpawnRuleSlotType = 66;

/** Lua index for a squad handle: its named fields and collections. */
[[nodiscard]] int squad_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SquadHandle*>(luaL_checkudata(state, 1, kSquadMetatable));
    SquadDefinition definition{};
    if (!current_squad(state, *handle, definition)) {
        return luaL_error(state, "activity squad is stale");
    }
    const std::string_view key = lua_string_view(state, 2);
    if (key == "row") {
        lua_pushinteger(state, definition.localRow);
    } else if (key == "id") {
        lua_pushlstring(state, definition.id.data(), definition.id.size());
    } else if (key == "name") {
        lua_pushlstring(state, definition.name.data(), definition.name.size());
    } else if (key == "member_count") {
        lua_pushinteger(state, static_cast<lua_Integer>(definition.memberCount));
    } else if (key == "default_counts") {
        lua_createtable(state, static_cast<int>(definition.memberCount), 0);
        for (std::size_t index = 0; index < definition.memberCount; ++index) {
            lua_pushinteger(state, definition.defaultCounts[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
    } else if (key == "counts") {
        lua_pushcfunction(state, &squad_counts);
    } else if (world_api::push_squad_member(state, definition, key)) {
    } else if (key == "place") {
        lua_pushcfunction(state, &squad_place);
    } else if (key == "actor_command") {
        lua_pushcfunction(state, &squad_actor_command);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Stages one SDK-selected scalar actor command for every live member of a squad. */
[[nodiscard]] int squad_actor_command(lua_State* state) {
    const auto* const handle =
        static_cast<const SquadHandle*>(luaL_checkudata(state, 1, kSquadMetatable));
    SquadDefinition definition{};
    if (!current_squad(state, *handle, definition)) {
        return luaL_error(state, "activity squad is stale");
    }
    // Only these named arguments are accepted; any other key is refused.
    static constexpr std::array<std::string_view, 2> kDeclared{"command", "value"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer command = checked_integer_argument(state, "command");
    const lua_Integer value = checked_integer_argument(state, "value");
    if (command < 0
        || static_cast<std::uint64_t>(command) > (std::numeric_limits<std::uint32_t>::max)()
        || value < (std::numeric_limits<std::int32_t>::min)()
        || value > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "actor command scalar is outside its durable type");
    }
    Impl* const impl = impl_from_state(state);
    Intent intent{};
    if (impl == nullptr
        || !decode_sdk_build_sha256(std::string_view(impl->identity.sdkBuildId.data()),
                                    intent.sdkBuildSha256)) {
        return luaL_error(state, "loaded SDK generation identity is invalid");
    }
    intent.kind = IntentKind::actorCommand;
    intent.firstRow = definition.nativeRow;
    intent.actorCommandSelector = static_cast<std::uint32_t>(command);
    intent.actorCommandValue = static_cast<std::int32_t>(value);
    return queue_intent(state, active_frame(state), intent);
}

/**
 * Stages one squad placement: squad:place{counts, mode, retire_on_return, spawn_rule}.
 * Every parameter is optional; the counts and mode carry their own bounds. A spawn rule is a
 * type-66 slot of the squad's own object that replaces the package rule.
 */
[[nodiscard]] int squad_place(lua_State* state) {
    const auto* const handle =
        static_cast<const SquadHandle*>(luaL_checkudata(state, 1, kSquadMetatable));
    SquadDefinition definition{};
    if (!current_squad(state, *handle, definition) || definition.memberCount == 0
        || definition.memberCount > kSquadMemberCapacity) {
        return luaL_error(state, "activity squad is stale or invalid");
    }
    // Only these named arguments are accepted; any other key is refused.
    static constexpr std::array<std::string_view, 4> kDeclared{
        "counts", "mode", "retire_on_return", "spawn_rule"};
    refuse_unknown_arguments(state, kDeclared);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::placeSquad;
    intent.firstRow = definition.nativeRow;
    intent.squadCount = static_cast<std::uint8_t>(definition.memberCount);

    SquadCountVectorHandle counts{};
    if (optional_argument(state, "counts", kSquadCountVectorMetatable, counts)) {
        // A count vector is bound to one squad, and no type carries that binding.
        if (counts.squadRow != handle->localRow) {
            return luaL_error(state, "squad counts were minted by another squad");
        }
        std::copy_n(counts.counts.begin(), counts.count, intent.squadCounts.begin());
    } else {
        std::copy_n(
            definition.defaultCounts.begin(), definition.memberCount, intent.squadCounts.begin());
    }

    SquadModeHandle mode{};
    static_cast<void>(optional_argument(state, "mode", kSquadModeMetatable, mode));
    intent.squadMode = mode.mode;
    lua_getfield(state, 2, "spawn_rule");
    if (!lua_isnil(state, -1)) {
        const auto* const rule =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition ruleSlot{};
        if (!current_slot(state, *rule, ruleSlot) || ruleSlot.slotType != kSpawnRuleSlotType) {
            return luaL_error(state, "spawn_rule requires a current authored type-66 slot");
        }
        intent.squadSpawnRuleRow = static_cast<std::int32_t>(ruleSlot.nativeRow);
    }
    lua_pop(state, 1);
    intent.squadRetireOnReturn = optional_boolean_argument(state, "retire_on_return", false);
    return queue_intent(state, frame, intent);
}

void register_squad_metatables(lua_State* state) {
    register_metatable(state, kSquadMetatable, &squad_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
