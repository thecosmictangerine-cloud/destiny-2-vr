#include <cstdint>
#include <limits>
#include <string_view>

#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

[[nodiscard]] int squad_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSquadCollectionMetatable));
    SquadDefinition definition{};
    if (!resolve_squad(state, 2, definition)) {
        return luaL_error(state, "activity squad row is unavailable");
    }
    push_handle(state, kSquadMetatable, SquadHandle{definition.localRow});
    return 1;
}

[[nodiscard]] int squad_collection_resolve(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSquadCollectionMetatable));
    SquadDefinition definition{};
    if (!resolve_squad(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous activity squad");
    }
    push_handle(state, kSquadMetatable, SquadHandle{definition.localRow});
    return 1;
}

[[nodiscard]] int scene_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSceneCollectionMetatable));
    SceneDefinition definition{};
    if (!resolve_scene(state, 2, definition)) {
        return luaL_error(state, "authored-scene row is unavailable");
    }
    push_handle(state, kSceneMetatable, SceneHandle{definition.localRow});
    return 1;
}

[[nodiscard]] int scene_collection_resolve(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSceneCollectionMetatable));
    SceneDefinition definition{};
    if (!resolve_scene(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous authored scene");
    }
    push_handle(state, kSceneMetatable, SceneHandle{definition.localRow});
    return 1;
}

[[nodiscard]] int slot_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSlotCollectionMetatable));
    SlotDefinition definition{};
    if (!resolve_slot(state, 2, definition)) {
        return luaL_error(state, "activity slot row is unavailable");
    }
    push_handle(state, kSlotMetatable, SlotHandle{definition.localRow});
    return 1;
}

[[nodiscard]] int slot_collection_resolve(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSlotCollectionMetatable));
    SlotDefinition definition{};
    if (!resolve_slot(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous activity slot");
    }
    push_handle(state, kSlotMetatable, SlotHandle{definition.localRow});
    return 1;
}

/** Lua `at` for the activity binding tag collection: resolves one 1-based tag. */
[[nodiscard]] int activity_binding_tag_collection_at(lua_State* state) {
    const auto* const collection = static_cast<const ActivityBindingTagCollectionHandle*>(
        luaL_checkudata(state, 1, kActivityBindingTagCollectionMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    Impl* const impl = impl_from_state(state);
    std::uint32_t tag = 0;
    if (impl == nullptr || row <= 0
        || static_cast<std::uint64_t>(row) > (std::numeric_limits<std::uint32_t>::max)()
        || impl->definitions.resolveActivityBindingTag == nullptr
        || !impl->definitions.resolveActivityBindingTag(
            impl->definitions.context, collection->kind, static_cast<std::uint32_t>(row), tag)) {
        return luaL_error(state, "activity-binding tag row is unavailable");
    }
    lua_pushinteger(state, tag);
    return 1;
}

/** Lua `at` for the activity binding locator collection: resolves one 1-based locator. */
[[nodiscard]] int activity_binding_locator_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kActivityBindingLocatorCollectionMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    Impl* const impl = impl_from_state(state);
    ActivityBindingLocatorDefinition definition{};
    if (impl == nullptr || row <= 0
        || static_cast<std::uint64_t>(row) > (std::numeric_limits<std::uint32_t>::max)()
        || impl->definitions.resolveActivityBindingLocator == nullptr
        || !impl->definitions.resolveActivityBindingLocator(
            impl->definitions.context, static_cast<std::uint32_t>(row), definition)) {
        return luaL_error(state, "activity-binding locator row is unavailable");
    }
    push_activity_binding_locator(state, definition.localRow);
    return 1;
}

/** Pushes the message at one 1-based row. Raises a Lua error when the row is unavailable. */
[[nodiscard]] int message_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kMessageCollectionMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    ActivityMessageDefinition definition{};
    if (row <= 0 || static_cast<std::uint64_t>(row) > (std::numeric_limits<std::uint32_t>::max)()
        || !resolve_message_row(state, static_cast<std::uint32_t>(row), definition)) {
        return luaL_error(state, "activity-message row is unavailable");
    }
    push_message(state, definition.localRow);
    return 1;
}

/** Pushes the message carrying one message id. Raises a Lua error when the id is unavailable. */
[[nodiscard]] int message_collection_by_id(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kMessageCollectionMetatable));
    const lua_Integer id = luaL_checkinteger(state, 2);
    ActivityMessageDefinition definition{};
    if (id < 0 || static_cast<std::uint64_t>(id) > (std::numeric_limits<std::uint32_t>::max)()
        || !resolve_message_id(state, static_cast<std::uint32_t>(id), definition)) {
        return luaL_error(state, "activity-message id is unavailable");
    }
    push_message(state, definition.localRow);
    return 1;
}

[[nodiscard]] int message_collection_resolve(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kMessageCollectionMetatable));
    ActivityMessageDefinition definition{};
    if (!resolve_message_name(state, lua_string_view(state, 2), definition)) {
        return luaL_error(state, "unknown or ambiguous activity message");
    }
    push_message(state, definition.localRow);
    return 1;
}

/** Lua `at` for an activity-message field collection: resolves one 1-based field. */
[[nodiscard]] int message_field_collection_at(lua_State* state) {
    const auto* const collection = static_cast<const MessageFieldCollectionHandle*>(
        luaL_checkudata(state, 1, kMessageFieldCollectionMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    ActivityMessageFieldDefinition definition{};
    if (row <= 0 || static_cast<std::uint64_t>(row) > (std::numeric_limits<std::uint32_t>::max)()
        || !resolve_message_field_row(
            state, collection->messageRow, static_cast<std::uint32_t>(row), definition)) {
        return luaL_error(state, "activity-message field row is unavailable");
    }
    push_message_field(state, definition.messageRow, definition.localRow);
    return 1;
}

} // namespace

/** Lua index for the activity binding tag collection: `count` and `at`, else nil. */
[[nodiscard]] int activity_binding_tag_collection_index(lua_State* state) {
    const auto* const collection = static_cast<const ActivityBindingTagCollectionHandle*>(
        luaL_checkudata(state, 1, kActivityBindingTagCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        ActivityBindingDefinition definition{};
        Impl* const impl = impl_from_state(state);
        if (!current_activity_binding(state, definition) || impl == nullptr
            || impl->definitions.activityBindingTagCount == nullptr) {
            return luaL_error(state, "activity binding is stale");
        }
        lua_pushinteger(state,
                        static_cast<lua_Integer>(impl->definitions.activityBindingTagCount(
                            impl->definitions.context, collection->kind)));
    } else if (key == "at") {
        lua_pushcfunction(state, &activity_binding_tag_collection_at);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for the activity binding locator collection: `count` and `at`, else nil. */
[[nodiscard]] int activity_binding_locator_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kActivityBindingLocatorCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        ActivityBindingDefinition definition{};
        Impl* const impl = impl_from_state(state);
        if (!current_activity_binding(state, definition) || impl == nullptr
            || impl->definitions.activityBindingLocatorCount == nullptr) {
            return luaL_error(state, "activity binding is stale");
        }
        lua_pushinteger(state,
                        static_cast<lua_Integer>(impl->definitions.activityBindingLocatorCount(
                            impl->definitions.context)));
    } else if (key == "at") {
        lua_pushcfunction(state, &activity_binding_locator_collection_at);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for the squad collection: `count`, `at` and `resolve`, else nil. */
[[nodiscard]] int squad_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSquadCollectionMetatable));
    Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(
            state,
            static_cast<lua_Integer>(impl->definitions.squadCount(impl->definitions.context)));
    } else if (key == "at") {
        lua_pushcfunction(state, &squad_collection_at);
    } else if (key == "resolve") {
        lua_pushcfunction(state, &squad_collection_resolve);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for the scene collection: `count`, `at` and `resolve`, else nil. */
[[nodiscard]] int scene_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSceneCollectionMetatable));
    Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(
            state,
            static_cast<lua_Integer>(impl->definitions.sceneCount(impl->definitions.context)));
    } else if (key == "at") {
        lua_pushcfunction(state, &scene_collection_at);
    } else if (key == "resolve") {
        lua_pushcfunction(state, &scene_collection_resolve);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for the slot collection: `count`, `at` and `resolve`, else nil. */
[[nodiscard]] int slot_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kSlotCollectionMetatable));
    Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(
            state,
            static_cast<lua_Integer>(impl->definitions.slotCount(impl->definitions.context)));
    } else if (key == "at") {
        lua_pushcfunction(state, &slot_collection_at);
    } else if (key == "resolve") {
        lua_pushcfunction(state, &slot_collection_resolve);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for the activity-message collection: `count`, `at`, `by_id` and `resolve`. */
[[nodiscard]] int message_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kMessageCollectionMetatable));
    Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        lua_pushinteger(state,
                        static_cast<lua_Integer>(
                            impl->definitions.activityMessageCount(impl->definitions.context)));
    } else if (key == "at") {
        lua_pushcfunction(state, &message_collection_at);
    } else if (key == "by_id") {
        lua_pushcfunction(state, &message_collection_by_id);
    } else if (key == "resolve") {
        lua_pushcfunction(state, &message_collection_resolve);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for an activity-message field collection: `count` and `at`, else nil. */
[[nodiscard]] int message_field_collection_index(lua_State* state) {
    const auto* const collection = static_cast<const MessageFieldCollectionHandle*>(
        luaL_checkudata(state, 1, kMessageFieldCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "count") {
        ActivityMessageDefinition message{};
        if (!resolve_message_row(state, collection->messageRow, message)) {
            return luaL_error(state, "activity message is stale");
        }
        lua_pushinteger(state, message.fieldCount);
    } else if (key == "at") {
        lua_pushcfunction(state, &message_field_collection_at);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Registers every collection metatable against its index function. */
void register_collection_metatables(lua_State* state) {
    register_metatable(
        state, kActivityBindingTagCollectionMetatable, &activity_binding_tag_collection_index);
    register_metatable(state,
                       kActivityBindingLocatorCollectionMetatable,
                       &activity_binding_locator_collection_index);
    register_metatable(state, kSquadCollectionMetatable, &squad_collection_index);
    register_metatable(state, kSceneCollectionMetatable, &scene_collection_index);
    register_metatable(state, kSlotCollectionMetatable, &slot_collection_index);
    register_metatable(state, kMessageCollectionMetatable, &message_collection_index);
    register_metatable(state, kMessageFieldCollectionMetatable, &message_field_collection_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
