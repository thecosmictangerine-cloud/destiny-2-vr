#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/map_generator_auth.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace generator = middleware::bap::activity_message::map_generator_auth;
namespace format = state::activity_sdk::format;

namespace {

/** A record selects a field only when its own mask bit is set; the rest stay authored. */
constexpr std::uint8_t kSeedMask = static_cast<std::uint8_t>(generator::Override::seed);
constexpr std::uint8_t kModeMask = static_cast<std::uint8_t>(generator::Override::mode);
constexpr std::uint8_t kAnchorMask = static_cast<std::uint8_t>(generator::Override::anchors);
constexpr std::uint8_t kEnabledMask = static_cast<std::uint8_t>(generator::Override::enabled);
/** Integer inputs three, four and five carry the last three mask bits, in record order. */
constexpr std::array<std::uint8_t, 3> kIntegerMasks{
    static_cast<std::uint8_t>(generator::Override::integer2),
    static_cast<std::uint8_t>(generator::Override::integer3),
    static_cast<std::uint8_t>(generator::Override::integer4)};
/** Integer inputs one and two have no mask bit; they apply on any value other than -1. */
constexpr std::size_t kUnmaskedIntegerCount = 2;

/** @return True when one live Slot row is an exact type-37 map generator. */
[[nodiscard]] bool exact_generator_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == generator::kSlotType
           && definition.componentClass == generator::kComponentClass
           && definition.authSchema == generator::kSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return False with the Lua error already raised when the field is outside the native lane. */
[[nodiscard]] bool
signed_byte(lua_State* state, lua_Integer value, const char* name, std::int8_t& output) {
    if (value < (std::numeric_limits<std::int8_t>::min)()
        || value > (std::numeric_limits<std::int8_t>::max)()) {
        return luaL_argerror(state, 2, name) == 0;
    }
    output = static_cast<std::int8_t>(value);
    return true;
}

/** Reads one anchor row: two authored grid selectors, a float input and an enable flag. */
[[nodiscard]] bool read_anchor(lua_State* state, int row, generator::Anchor& anchor) {
    if (!lua_istable(state, row)) {
        return luaL_argerror(state, 2, "each anchor must be a table") == 0;
    }
    lua_getfield(state, row, "first");
    lua_getfield(state, row, "second");
    lua_getfield(state, row, "value");
    lua_getfield(state, row, "enabled");
    const bool typed = lua_isinteger(state, -4) && lua_isinteger(state, -3)
                       && lua_isnumber(state, -2) && lua_isboolean(state, -1);
    if (!typed) {
        lua_pop(state, 4);
        return luaL_argerror(state, 2, "an anchor needs first, second, value and enabled") == 0;
    }
    const lua_Integer first = lua_tointeger(state, -4);
    const lua_Integer second = lua_tointeger(state, -3);
    anchor.value = static_cast<float>(lua_tonumber(state, -2));
    anchor.enabled = lua_toboolean(state, -1) != 0;
    lua_pop(state, 4);
    return signed_byte(state, first, "anchor first", anchor.first)
           && signed_byte(state, second, "anchor second", anchor.second);
}

/** Reads the four anchors and sets their mask bit. */
[[nodiscard]] bool read_anchors(lua_State* state, generator::Record& record) {
    if (push_argument(state, "anchors") == LUA_TNIL) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1)
        || lua_rawlen(state, -1) != static_cast<std::size_t>(generator::kAnchorCount)) {
        lua_pop(state, 1);
        return luaL_argerror(state, 2, "anchors must be a list of four") == 0;
    }
    const int list = lua_gettop(state);
    for (std::size_t index = 0; index < generator::kAnchorCount; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        const bool read = read_anchor(state, lua_gettop(state), record.anchors[index]);
        lua_pop(state, 1);
        if (!read) {
            lua_pop(state, 1);
            return false;
        }
    }
    lua_pop(state, 1);
    record.overrides |= kAnchorMask;
    return true;
}

/** Reads the five integer worker inputs in record order, keeping -1 where the caller is silent. */
[[nodiscard]] bool read_integers(lua_State* state, generator::Record& record) {
    if (push_argument(state, "values") == LUA_TNIL) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1)
        || lua_rawlen(state, -1) > static_cast<std::size_t>(generator::kIntegerCount)) {
        lua_pop(state, 1);
        return luaL_argerror(state, 2, "values must be a list of at most five integers") == 0;
    }
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        if (!lua_isinteger(state, -1)) {
            lua_pop(state, 2);
            return luaL_argerror(state, 2, "values must hold integers") == 0;
        }
        const lua_Integer value = lua_tointeger(state, -1);
        lua_pop(state, 1);
        if (value < (std::numeric_limits<std::int32_t>::min)()
            || value > (std::numeric_limits<std::int32_t>::max)()) {
            lua_pop(state, 1);
            return luaL_argerror(state, 2, "values must be 32-bit signed integers") == 0;
        }
        record.integerInputs[index] = static_cast<std::int32_t>(value);
        if (index >= kUnmaskedIntegerCount) {
            record.overrides |= kIntegerMasks[index - kUnmaskedIntegerCount];
        }
    }
    lua_pop(state, 1);
    return true;
}

/** Reads the two float worker inputs; each keeps -1 until the caller names it. */
[[nodiscard]] bool read_reals(lua_State* state, generator::Record& record) {
    if (push_argument(state, "reals") == LUA_TNIL) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1)
        || lua_rawlen(state, -1) > static_cast<std::size_t>(generator::kRealCount)) {
        lua_pop(state, 1);
        return luaL_argerror(state, 2, "reals must be a list of at most two numbers") == 0;
    }
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        if (!lua_isnumber(state, -1)) {
            lua_pop(state, 2);
            return luaL_argerror(state, 2, "reals must hold numbers") == 0;
        }
        record.realInputs[index] = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

} // namespace

/**
 * Writes one record of an authored map generator and leaves the other authored.
 * A field the caller omits keeps the worker's authored value: the masked fields through their
 * mask bit, the two floats and the first two integers through their -1 sentinel.
 */
[[nodiscard]] int slot_generate_map(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 8> kDeclared{
        "record", "seed", "mode", "anchors", "enabled", "values", "reals", "state_key"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot)) {
        return luaL_error(state, "activity slot is stale or invalid");
    }
    if (!exact_generator_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-37 map generator");
    }
    const lua_Integer record = optional_integer_argument(state, "record", 1);
    if (record < 1 || record > static_cast<lua_Integer>(generator::kRecordCount)) {
        return luaL_error(state, "record must be 1 or 2");
    }
    generator::Body body{};
    generator::Record& target = body.records[static_cast<std::size_t>(record - 1)];
    const lua_Integer seed = optional_integer_argument(state, "seed", -1);
    if (seed >= 0) {
        if (seed > static_cast<lua_Integer>((std::numeric_limits<std::uint32_t>::max)())) {
            return luaL_error(state, "seed must be a 32-bit unsigned integer");
        }
        target.seed = static_cast<std::uint32_t>(seed);
        target.overrides |= kSeedMask;
    }
    if (push_argument(state, "mode") != LUA_TNIL) {
        if (!lua_isinteger(state, -1)) {
            return luaL_error(state, "mode must be an integer");
        }
        const lua_Integer mode = lua_tointeger(state, -1);
        lua_pop(state, 1);
        if (!signed_byte(state, mode, "mode", target.mode)) {
            return 0;
        }
        target.overrides |= kModeMask;
    } else {
        lua_pop(state, 1);
    }
    if (push_argument(state, "enabled") != LUA_TNIL) {
        if (!lua_isboolean(state, -1)) {
            return luaL_error(state, "enabled must be a boolean");
        }
        target.enabled = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        target.overrides |= kEnabledMask;
    } else {
        lua_pop(state, 1);
    }
    if (!read_anchors(state, target) || !read_integers(state, target)
        || !read_reals(state, target)) {
        return 0;
    }
    const lua_Integer stateKey = optional_integer_argument(state, "state_key", 0);
    if (stateKey < 0
        || stateKey > static_cast<lua_Integer>((std::numeric_limits<std::uint32_t>::max)())) {
        return luaL_error(state, "state_key must be a 32-bit unsigned integer");
    }
    body.stateKey = static_cast<std::uint32_t>(stateKey);
    std::array<std::byte, generator::kByteCount> bytes{};
    std::size_t written = 0;
    if (!generator::encode(body, bytes, written)) {
        return luaL_error(state, "map generator encoder failed");
    }
    return queue_slot_auth(
        state, slot, generator::kSchema, generator::kBitCount, std::span(bytes).first(written));
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
