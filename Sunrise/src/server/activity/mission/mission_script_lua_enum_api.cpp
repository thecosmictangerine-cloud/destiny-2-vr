// Closed vocabularies. Each one is a set of named values inside a wider wire field, so the
// encodings the client cannot survive have no spelling a script can write.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../state/activity/membership/definition.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

using Channel = scriptable_auth::Type23Channel;

/** Lua index for one device channel: its name and wire value. */
[[nodiscard]] int device_channel_index(lua_State* state) {
    const auto* const handle =
        static_cast<const DeviceChannelHandle*>(luaL_checkudata(state, 1, kDeviceChannelMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "name") {
        const std::string_view name = device_channel_name(handle->channel);
        lua_pushlstring(state, name.data(), name.size());
    } else if (key == "value") {
        lua_pushinteger(state, handle->channel);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for the device channel collection: `count` or a channel name, else nil. */
[[nodiscard]] int device_channel_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kDeviceChannelCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    std::uint8_t channel = 0;
    if (key == "count") {
        lua_pushinteger(state, 3);
    } else if (device_channel_value(key, channel)) {
        push_device_channel(state, channel);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for one device transition: its name and wire value. */
[[nodiscard]] int device_transition_index(lua_State* state) {
    const auto* const handle = static_cast<const DeviceTransitionHandle*>(
        luaL_checkudata(state, 1, kDeviceTransitionMetatable));
    if (handle->row >= kDeviceTransitions.size()) {
        return luaL_error(state, "device transition row is outside the vocabulary");
    }
    const DeviceTransition& row = kDeviceTransitions[handle->row];
    const std::string_view key = lua_string_view(state, 2);
    if (key == "name") {
        lua_pushlstring(state, row.name.data(), row.name.size());
    } else if (key == "channel") {
        push_device_channel(state, static_cast<std::uint8_t>(row.channel));
    } else if (key == "value") {
        push_unit_scalar(state, row.value);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

[[nodiscard]] int device_transition_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kDeviceTransitionCollectionMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    if (row < 1 || static_cast<std::size_t>(row) > kDeviceTransitions.size()) {
        return luaL_error(state, "device transition row is outside the vocabulary");
    }
    push_device_transition(state, static_cast<std::uint8_t>(row - 1));
    return 1;
}

/** Lua index for the device transition collection: `count`, `at` or a transition name. */
[[nodiscard]] int device_transition_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kDeviceTransitionCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(state, static_cast<lua_Integer>(kDeviceTransitions.size()));
        return 1;
    }
    if (key == "at") {
        lua_pushcfunction(state, &device_transition_collection_at);
        return 1;
    }
    for (std::size_t row = 0; row < kDeviceTransitions.size(); ++row) {
        if (kDeviceTransitions[row].name == key) {
            push_device_transition(state, static_cast<std::uint8_t>(row));
            return 1;
        }
    }
    lua_pushnil(state);
    return 1;
}

/** Lua index for one lifetime state: `value`, else nil. */
[[nodiscard]] int lifetime_state_index(lua_State* state) {
    const auto* const handle =
        static_cast<const LifetimeStateHandle*>(luaL_checkudata(state, 1, kLifetimeStateMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "value") {
        lua_pushinteger(state, handle->state);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Selects one state inside the client jump table. There is no expression that reaches 11. */
[[nodiscard]] int lifetime_state_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kLifetimeStateCollectionMetatable));
    const lua_Integer requested = luaL_checkinteger(state, 2);
    if (requested < 0 || requested > host::kMaximumLifetimeState) {
        return luaL_error(state, "activity lifetime state is outside the client jump table");
    }
    push_lifetime_state(state, static_cast<std::uint8_t>(requested));
    return 1;
}

/** Lua index for the lifetime state collection: `count`, `at` and `default`, else nil. */
[[nodiscard]] int lifetime_state_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kLifetimeStateCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(state, kLifetimeStateCount);
    } else if (key == "at") {
        lua_pushcfunction(state, &lifetime_state_collection_at);
    } else if (key == "default") {
        push_lifetime_state(state, host::kDefaultLifetimeState);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

} // namespace

/** @return The script-facing name of one device channel, or "unknown". */
std::string_view device_channel_name(std::uint8_t channel) noexcept {
    switch (static_cast<Channel>(channel)) {
    case Channel::devicePosition:
        return "position";
    case Channel::devicePower:
        return "power";
    case Channel::deviceLock:
        return "lock";
    }
    return "unknown";
}

/** Maps a device channel name to its wire value. @return False when the name is unknown. */
bool device_channel_value(std::string_view name, std::uint8_t& output) noexcept {
    if (name == "position") {
        output = static_cast<std::uint8_t>(Channel::devicePosition);
        return true;
    }
    if (name == "power") {
        output = static_cast<std::uint8_t>(Channel::devicePower);
        return true;
    }
    if (name == "lock") {
        output = static_cast<std::uint8_t>(Channel::deviceLock);
        return true;
    }
    return false;
}

void push_device_channel(lua_State* state, std::uint8_t channel) {
    push_handle(state, kDeviceChannelMetatable, DeviceChannelHandle{channel});
}

void push_device_transition(lua_State* state, std::uint8_t row) {
    push_handle(state, kDeviceTransitionMetatable, DeviceTransitionHandle{row});
}

void push_lifetime_state(lua_State* state, std::uint8_t value) {
    push_handle(state, kLifetimeStateMetatable, LifetimeStateHandle{value});
}

void register_enum_metatables(lua_State* state) {
    register_metatable(state, kDeviceChannelMetatable, &device_channel_index);
    register_metatable(state, kDeviceChannelCollectionMetatable, &device_channel_collection_index);
    register_metatable(state, kDeviceTransitionMetatable, &device_transition_index);
    register_metatable(
        state, kDeviceTransitionCollectionMetatable, &device_transition_collection_index);
    register_metatable(state, kLifetimeStateMetatable, &lifetime_state_index);
    register_metatable(state, kLifetimeStateCollectionMetatable, &lifetime_state_collection_index);
}

/** Serves the activity table's enum collections. @return False for any other key. */
bool push_enum_activity_member(lua_State* state, std::string_view key) {
    if (key == "client_teleport_reset") {
        lua_pushinteger(state, state::activity::membership::kClientTeleportResetState);
        return true;
    }
    if (key == "device_channels") {
        push_handle(state, kDeviceChannelCollectionMetatable, DeviceChannelCollectionHandle{});
        return true;
    }
    if (key == "device_transitions") {
        push_handle(
            state, kDeviceTransitionCollectionMetatable, DeviceTransitionCollectionHandle{});
        return true;
    }
    if (key == "lifetime_states") {
        push_handle(state, kLifetimeStateCollectionMetatable, LifetimeStateCollectionHandle{});
        return true;
    }
    return false;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
