// Value types whose bound lives in the type. A script reads a value out of one of these and never
// writes the lane the client indexes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** Ordinary placement and native transport reservation; other encodings have no name. */
constexpr std::uint8_t kSquadModeReinforce = 0;
constexpr std::uint8_t kSquadModeReplace = 2;
constexpr std::uint8_t kSquadModeReserve = 3;

/** Lua index for a unit scalar: its value and the device lane bounds. */
[[nodiscard]] int unit_scalar_index(lua_State* state) {
    const auto* const handle =
        static_cast<const UnitScalarHandle*>(luaL_checkudata(state, 1, kUnitScalarMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "value") {
        lua_pushnumber(state, handle->value);
    } else if (key == "low") {
        lua_pushnumber(state, kUnitLaneLow);
    } else if (key == "high") {
        lua_pushnumber(state, kUnitLaneHigh);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Mints one channel value; the client owns the meaning of the float. */
[[nodiscard]] int unit_scalar_new(lua_State* state) {
    if (lua_type(state, 1) != LUA_TNUMBER) {
        return luaL_error(state, "channel value must be a number");
    }
    const lua_Number requested = lua_tonumber(state, 1);
    push_unit_scalar(state, static_cast<float>(requested));
    return 1;
}

/** Lua index for one squad mode: its name and wire value. */
[[nodiscard]] int squad_mode_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SquadModeHandle*>(luaL_checkudata(state, 1, kSquadModeMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "name") {
        const std::string_view name = squad_mode_name(handle->mode);
        lua_pushlstring(state, name.data(), name.size());
    } else if (key == "value") {
        lua_pushinteger(state, handle->mode);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for the squad mode collection: `count` or a mode name, else nil. */
[[nodiscard]] int squad_mode_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSquadModeCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "reinforce") {
        push_squad_mode(state, kSquadModeReinforce);
    } else if (key == "replace") {
        push_squad_mode(state, kSquadModeReplace);
    } else if (key == "reserve") {
        push_squad_mode(state, kSquadModeReserve);
    } else if (key == "count") {
        lua_pushinteger(state, 3);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Rereads the squad the vector was minted from so a stale generation refuses. */
[[nodiscard]] bool
vector_squad(lua_State* state, const SquadCountVectorHandle& handle, SquadDefinition& output) {
    return current_squad(state, SquadHandle{handle.squadRow}, output)
           && output.memberCount == handle.count;
}

[[nodiscard]] int squad_count_vector_at(lua_State* state) {
    const auto* const handle = static_cast<const SquadCountVectorHandle*>(
        luaL_checkudata(state, 1, kSquadCountVectorMetatable));
    const lua_Integer member = luaL_checkinteger(state, 2);
    if (member < 1 || member > handle->count) {
        return luaL_error(state, "squad member is outside the vector");
    }
    lua_pushinteger(state, handle->counts[static_cast<std::size_t>(member - 1)]);
    return 1;
}

/** Writes one non-negative member count in the squad Auth wire type. */
[[nodiscard]] int squad_count_vector_set(lua_State* state) {
    auto* const handle =
        static_cast<SquadCountVectorHandle*>(luaL_checkudata(state, 1, kSquadCountVectorMetatable));
    SquadDefinition definition{};
    if (!vector_squad(state, *handle, definition)) {
        return luaL_error(state, "activity squad is stale");
    }
    const lua_Integer member = luaL_checkinteger(state, 2);
    if (member < 1 || member > handle->count) {
        return luaL_error(state, "squad member is outside the vector");
    }
    const std::size_t row = static_cast<std::size_t>(member - 1);
    const lua_Integer requested = luaL_checkinteger(state, 3);
    if (requested < 0 || requested > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "squad member count is outside the wire integer range");
    }
    handle->counts[row] = static_cast<std::int32_t>(requested);
    return 0;
}

/** Lua index for a squad count vector: its used count, capacity and lanes. */
[[nodiscard]] int squad_count_vector_index(lua_State* state) {
    const auto* const handle = static_cast<const SquadCountVectorHandle*>(
        luaL_checkudata(state, 1, kSquadCountVectorMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(state, handle->count);
    } else if (key == "capacity") {
        lua_pushinteger(state, static_cast<lua_Integer>(kSquadCountCapacity));
    } else if (key == "at") {
        lua_pushcfunction(state, &squad_count_vector_at);
    } else if (key == "set") {
        lua_pushcfunction(state, &squad_count_vector_set);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

} // namespace

/** @return The stable Lua name of one squad mode. */
std::string_view squad_mode_name(std::uint8_t mode) noexcept {
    if (mode == kSquadModeReinforce) {
        return "reinforce";
    }
    if (mode == kSquadModeReplace) {
        return "replace";
    }
    if (mode == kSquadModeReserve) {
        return "reserve";
    }
    return "unknown";
}

void push_unit_scalar(lua_State* state, float value) {
    push_handle(state, kUnitScalarMetatable, UnitScalarHandle{value});
}

void push_squad_mode(lua_State* state, std::uint8_t mode) {
    push_handle(state, kSquadModeMetatable, SquadModeHandle{mode});
}

void push_squad_count_vector(lua_State* state, const SquadCountVectorHandle& value) {
    push_handle(state, kSquadCountVectorMetatable, value);
}

/** Lua `counts` on a squad: pushes its member counts. Errors when the squad is stale. */
int squad_counts(lua_State* state) {
    const auto* const handle =
        static_cast<const SquadHandle*>(luaL_checkudata(state, 1, kSquadMetatable));
    SquadDefinition definition{};
    if (!current_squad(state, *handle, definition)) {
        return luaL_error(state, "activity squad is stale");
    }
    if (definition.memberCount == 0 || definition.memberCount > kSquadCountCapacity) {
        return luaL_error(state,
                          "activity squad member count is outside the requested-count array");
    }
    SquadCountVectorHandle vector{};
    vector.squadRow = definition.localRow;
    vector.count = static_cast<std::uint8_t>(definition.memberCount);
    std::copy_n(definition.defaultCounts.begin(), definition.memberCount, vector.counts.begin());
    push_squad_count_vector(state, vector);
    return 1;
}

void register_value_metatables(lua_State* state) {
    register_metatable(state, kUnitScalarMetatable, &unit_scalar_index);
    register_metatable(state, kSquadModeMetatable, &squad_mode_index);
    register_metatable(state, kSquadModeCollectionMetatable, &squad_mode_collection_index);
    register_metatable(state, kSquadCountVectorMetatable, &squad_count_vector_index);
}

/** Pushes one value-type ActivityView member. @return False when the key is not ours. */
bool push_value_activity_member(lua_State* state, std::string_view key) {
    if (key == "squad_modes") {
        push_handle(state, kSquadModeCollectionMetatable, SquadModeCollectionHandle{});
        return true;
    }
    if (key == "unit") {
        lua_pushcfunction(state, &unit_scalar_new);
        return true;
    }
    return false;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
