// Bounded lists for the count and object-reference lanes that walk past a fixed client array.
// A list cannot grow past its lane's array, so the count it produces is always in range.
// TODO: the typed Auth draft consumes these; until it lands they have no caller.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** @return The lane a list handle names, or null when the row is outside the table. */
[[nodiscard]] const BoundedLane* bounded_lane(std::uint8_t row) noexcept {
    return row < kBoundedLanes.size() ? &kBoundedLanes[row] : nullptr;
}

[[nodiscard]] int count_list_at(lua_State* state) {
    const auto* const handle =
        static_cast<const CountListHandle*>(luaL_checkudata(state, 1, kCountListMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    if (row < 1 || row > handle->count) {
        return luaL_error(state, "count list row is outside the list");
    }
    lua_pushinteger(state, handle->values[static_cast<std::size_t>(row - 1)]);
    return 1;
}

/** Appends one element. The lane's array size is the ceiling, so the count cannot overrun. */
[[nodiscard]] int count_list_append(lua_State* state) {
    auto* const handle =
        static_cast<CountListHandle*>(luaL_checkudata(state, 1, kCountListMetatable));
    const BoundedLane* const lane = bounded_lane(handle->lane);
    if (lane == nullptr) {
        return luaL_error(state, "count list lane is unknown");
    }
    if (handle->count >= lane->capacity) {
        return luaL_error(state, "count list is full");
    }
    const lua_Integer value = luaL_checkinteger(state, 2);
    if (value < (std::numeric_limits<std::int32_t>::min)()
        || value > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "count list value is outside int32");
    }
    handle->values[handle->count] = static_cast<std::int32_t>(value);
    ++handle->count;
    return 0;
}

/** Lua index for a count list: the lane's counts and bounds. Errors on an unknown lane. */
[[nodiscard]] int count_list_index(lua_State* state) {
    const auto* const handle =
        static_cast<const CountListHandle*>(luaL_checkudata(state, 1, kCountListMetatable));
    const BoundedLane* const lane = bounded_lane(handle->lane);
    if (lane == nullptr) {
        return luaL_error(state, "count list lane is unknown");
    }
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(state, handle->count);
    } else if (key == "capacity") {
        lua_pushinteger(state, lane->capacity);
    } else if (key == "lane") {
        lua_pushlstring(state, lane->name.data(), lane->name.size());
    } else if (key == "at") {
        lua_pushcfunction(state, &count_list_at);
    } else if (key == "append") {
        lua_pushcfunction(state, &count_list_append);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Pushes the slot at one 1-based list row. Raises a Lua error when the row is out of range. */
[[nodiscard]] int object_ref_list_at(lua_State* state) {
    const auto* const handle =
        static_cast<const ObjectRefListHandle*>(luaL_checkudata(state, 1, kObjectRefListMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    if (row < 1 || row > handle->count) {
        return luaL_error(state, "object reference row is outside the list");
    }
    push_handle(
        state, kSlotMetatable, SlotHandle{handle->slotRows[static_cast<std::size_t>(row - 1)]});
    return 1;
}

/** Appends one installed object. The element is a resolved slot, never an index a script wrote. */
[[nodiscard]] int object_ref_list_append(lua_State* state) {
    auto* const handle =
        static_cast<ObjectRefListHandle*>(luaL_checkudata(state, 1, kObjectRefListMetatable));
    const auto* const slotHandle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 2, kSlotMetatable));
    const BoundedLane* const lane = bounded_lane(handle->lane);
    if (lane == nullptr) {
        return luaL_error(state, "object reference lane is unknown");
    }
    if (handle->count >= lane->capacity) {
        return luaL_error(state, "object reference list is full");
    }
    SlotDefinition slot{};
    if (!current_slot(state, *slotHandle, slot)) {
        return luaL_error(state, "activity slot is stale");
    }
    handle->slotRows[handle->count] = slotHandle->localRow;
    ++handle->count;
    return 0;
}

/** Lua index for an object reference list: the lane's refs and bounds. */
[[nodiscard]] int object_ref_list_index(lua_State* state) {
    const auto* const handle =
        static_cast<const ObjectRefListHandle*>(luaL_checkudata(state, 1, kObjectRefListMetatable));
    const BoundedLane* const lane = bounded_lane(handle->lane);
    if (lane == nullptr) {
        return luaL_error(state, "object reference lane is unknown");
    }
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(state, handle->count);
    } else if (key == "capacity") {
        lua_pushinteger(state, lane->capacity);
    } else if (key == "lane") {
        lua_pushlstring(state, lane->name.data(), lane->name.size());
    } else if (key == "at") {
        lua_pushcfunction(state, &object_ref_list_at);
    } else if (key == "append") {
        lua_pushcfunction(state, &object_ref_list_append);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Mints the empty list of the lane at stack index one. */
[[nodiscard]] int bounded_lane_list(lua_State* state) {
    const auto* const handle =
        static_cast<const BoundedLaneHandle*>(luaL_checkudata(state, 1, kBoundedLaneMetatable));
    const BoundedLane* const lane = bounded_lane(handle->row);
    if (lane == nullptr) {
        return luaL_error(state, "bounded lane is unknown");
    }
    if (lane->kind == BoundedLaneKind::count) {
        CountListHandle list{};
        list.lane = handle->row;
        push_handle(state, kCountListMetatable, list);
    } else {
        ObjectRefListHandle list{};
        list.lane = handle->row;
        push_handle(state, kObjectRefListMetatable, list);
    }
    return 1;
}

/** Lua index for one bounded lane: its name, bound and current use. */
[[nodiscard]] int bounded_lane_index(lua_State* state) {
    const auto* const handle =
        static_cast<const BoundedLaneHandle*>(luaL_checkudata(state, 1, kBoundedLaneMetatable));
    const BoundedLane* const lane = bounded_lane(handle->row);
    if (lane == nullptr) {
        return luaL_error(state, "bounded lane is unknown");
    }
    const std::string_view key = lua_string_view(state, 2);
    if (key == "name") {
        lua_pushlstring(state, lane->name.data(), lane->name.size());
    } else if (key == "capacity") {
        lua_pushinteger(state, lane->capacity);
    } else if (key == "kind") {
        lua_pushstring(state, lane->kind == BoundedLaneKind::count ? "count" : "object_ref");
    } else if (key == "list") {
        lua_pushcfunction(state, &bounded_lane_list);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for the bounded lane collection: `count` or a lane name, else nil. */
[[nodiscard]] int bounded_lane_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kBoundedLaneCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(state, static_cast<lua_Integer>(kBoundedLanes.size()));
        return 1;
    }
    for (std::size_t row = 0; row < kBoundedLanes.size(); ++row) {
        if (kBoundedLanes[row].name == key) {
            push_bounded_lane(state, static_cast<std::uint8_t>(row));
            return 1;
        }
    }
    lua_pushnil(state);
    return 1;
}

/** Lua index for a world position: `x`, `y` and `z` select a lane, any other key is nil. */
[[nodiscard]] int world_position_index(lua_State* state) {
    const auto* const handle =
        static_cast<const WorldPositionHandle*>(luaL_checkudata(state, 1, kWorldPositionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "x") {
        lua_pushnumber(state, handle->value[0]);
    } else if (key == "y") {
        lua_pushnumber(state, handle->value[1]);
    } else if (key == "z") {
        lua_pushnumber(state, handle->value[2]);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Mints one finite world position. The client applies no magnitude bound, so none is here. */
[[nodiscard]] int world_position_new(lua_State* state) {
    std::array<float, 3> value{};
    for (int lane = 0; lane < 3; ++lane) {
        if (lua_type(state, lane + 1) != LUA_TNUMBER) {
            return luaL_error(state, "world position takes three finite numbers");
        }
        const lua_Number requested = lua_tonumber(state, lane + 1);
        value[static_cast<std::size_t>(lane)] = static_cast<float>(requested);
        if (!std::isfinite(requested) || !std::isfinite(value[static_cast<std::size_t>(lane)])) {
            return luaL_error(state, "world position takes three finite numbers");
        }
    }
    push_world_position(state, value);
    return 1;
}

} // namespace

void push_bounded_lane(lua_State* state, std::uint8_t row) {
    push_handle(state, kBoundedLaneMetatable, BoundedLaneHandle{row});
}

void push_world_position(lua_State* state, const std::array<float, 3>& value) {
    push_handle(state, kWorldPositionMetatable, WorldPositionHandle{value});
}

void register_bounded_metatables(lua_State* state) {
    register_metatable(state, kBoundedLaneCollectionMetatable, &bounded_lane_collection_index);
    register_metatable(state, kBoundedLaneMetatable, &bounded_lane_index);
    register_metatable(state, kCountListMetatable, &count_list_index);
    register_metatable(state, kObjectRefListMetatable, &object_ref_list_index);
    register_metatable(state, kWorldPositionMetatable, &world_position_index);
}

/** Pushes one bounded-lane ActivityView member. @return False when the key is not ours. */
bool push_bounded_activity_member(lua_State* state, std::string_view key) {
    if (key == "bounded_lanes") {
        push_handle(state, kBoundedLaneCollectionMetatable, BoundedLaneCollectionHandle{});
        return true;
    }
    if (key == "position") {
        lua_pushcfunction(state, &world_position_new);
        return true;
    }
    return false;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
