#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/auth_fields.h"
#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;

namespace {

/** Reads one unsigned 32-bit field from an atom declaration. */
[[nodiscard]] std::uint32_t atom_u32(lua_State* state, int table, const char* field) {
    const lua_Integer value = directive_integer(state, table, field);
    if (value < 0 || value > (std::numeric_limits<std::uint32_t>::max)()) {
        static_cast<void>(luaL_error(state, "atom field '%s' is outside a 32-bit range", field));
    }
    return static_cast<std::uint32_t>(value);
}

/** Reads one finite real field from an atom declaration. */
[[nodiscard]] float atom_real(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    const auto value = static_cast<float>(luaL_checknumber(state, -1));
    lua_pop(state, 1);
    if (!std::isfinite(value)) {
        static_cast<void>(luaL_error(state, "atom field '%s' is not finite", field));
    }
    return value;
}

/** Reads one optional small unsigned field from an atom declaration. */
[[nodiscard]] std::uint8_t
atom_u8(lua_State* state, int table, const char* field, std::uint8_t bound) {
    lua_getfield(state, table, field);
    const lua_Integer value = lua_isnil(state, -1) ? 0 : luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    if (value < 0 || value > bound) {
        static_cast<void>(luaL_error(state, "atom field '%s' is outside its native width", field));
    }
    return static_cast<std::uint8_t>(value);
}

/** Reads one optional boolean field from an atom declaration. */
[[nodiscard]] bool atom_flag(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    const bool value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return value;
}

/** Resolves the live Slot handle an atom points at into its exact 55-bit client reference. */
[[nodiscard]] scriptable_auth::Type2LaneClientRef atom_target(lua_State* state, int table) {
    scriptable_auth::Type2LaneClientRef reference{};
    lua_getfield(state, table, "target");
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
    SlotDefinition target{};
    if (!current_slot(state, *handle, target)) {
        static_cast<void>(luaL_error(state, "atom target slot is stale"));
    }
    // A client reference carries the slot index as a signed 16-bit lane.
    constexpr auto kHighestReferenceIndex =
        static_cast<std::uint32_t>((std::numeric_limits<std::int16_t>::max)());
    if (target.slotType > 126
        || static_cast<std::uint32_t>(target.slotIndex) > kHighestReferenceIndex) {
        static_cast<void>(luaL_error(state, "atom target slot is outside a client reference"));
    }
    reference.registryKey = target.registryKey;
    reference.slotType = static_cast<std::int8_t>(target.slotType);
    reference.slotIndex = static_cast<std::int16_t>(target.slotIndex);
    lua_pop(state, 1);
    return reference;
}

/** The point reader has no bounds check, so every parameter must belong to the SDK target. */
[[nodiscard]] std::uint8_t atom_point_parameter(lua_State* state, int table) {
    const auto parameter =
        atom_u8(state, table, "value", (std::numeric_limits<std::uint8_t>::max)());
    lua_getfield(state, table, "target");
    const auto* handle = static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
    SlotDefinition target{};
    const bool current = current_slot(state, *handle, target);
    lua_pop(state, 1);
    const Impl* impl = impl_from_state(state);
    if (!current || impl->definitions.resolveActorAbilityTarget == nullptr
        || !impl->definitions.resolveActorAbilityTarget(
            impl->definitions.context, target.nativeRow, parameter)) {
        static_cast<void>(luaL_error(state, "atom target has no such authored point parameter"));
    }
    return parameter;
}

scriptable_auth::Type2LanePrimary atom_face(lua_State* state, int table) {
    return scriptable_auth::Type2LaneAlternateRefByte{atom_target(state, table),
                                                      atom_point_parameter(state, table)};
}

scriptable_auth::Type2LanePrimary atom_snap_to(lua_State* state, int table) {
    return scriptable_auth::Type2LaneRefByte{atom_target(state, table),
                                             atom_point_parameter(state, table)};
}

scriptable_auth::Type2LanePrimary atom_sequence(lua_State* state, int table) {
    return scriptable_auth::Type2LaneU32{atom_u32(state, table, "value")};
}

scriptable_auth::Type2LanePrimary atom_sleep(lua_State* state, int table) {
    return scriptable_auth::Type2LaneReal32{atom_real(state, table, "seconds")};
}

scriptable_auth::Type2LanePrimary atom_move_to(lua_State* state, int table) {
    return scriptable_auth::Type2LaneRefByteBool{atom_target(state, table),
                                                 atom_point_parameter(state, table),
                                                 atom_flag(state, table, "enabled")};
}

scriptable_auth::Type2LanePrimary atom_trivial(lua_State* /*state*/, int /*table*/) {
    return scriptable_auth::Type2LaneEmpty{};
}

scriptable_auth::Type2LanePrimary atom_control_flag(lua_State* state, int table) {
    return scriptable_auth::Type2LaneU6{atom_u8(state, table, "value", 0x3FU)};
}

scriptable_auth::Type2LanePrimary atom_set_temperament(lua_State* state, int table) {
    return scriptable_auth::Type2LaneU32Bool{atom_u32(state, table, "value"),
                                             atom_flag(state, table, "enabled")};
}

scriptable_auth::Type2LanePrimary atom_set_channel(lua_State* state, int table) {
    return scriptable_auth::Type2LaneU32Real32{atom_u32(state, table, "channel"),
                                               atom_real(state, table, "value")};
}

/** Uses one exact actor-owned ability and the first parameter of an optional authored point. */
scriptable_auth::Type2LanePrimary
atom_ability(lua_State* state, int table, const SlotDefinition& actor) {
    table = lua_absindex(state, table);
    // Actor-target modes and raw hash tuples have no script-level meaning on this path.
    static constexpr std::array<std::string_view, 4> kFields{
        "kind", "ability", "target", "quantized"};
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        const auto key = lua_string_view(state, -2);
        if (std::find(kFields.begin(), kFields.end(), key) == kFields.end()) {
            static_cast<void>(luaL_error(state, "unknown ability atom field"));
        }
        lua_pop(state, 1);
    }
    lua_getfield(state, table, "ability");
    luaL_checktype(state, -1, LUA_TTABLE);
    const std::uint32_t owner = atom_u32(state, -1, "slot_row");
    const std::uint32_t group = atom_u32(state, -1, "group_hash");
    const std::uint32_t request = atom_u32(state, -1, "request_hash");
    lua_pop(state, 1);
    Impl* const impl = impl_from_state(state);
    if (owner != actor.nativeRow || impl == nullptr
        || impl->definitions.resolveActorAbility == nullptr
        || !impl->definitions.resolveActorAbility(
            impl->definitions.context, owner, group, request)) {
        static_cast<void>(luaL_error(state, "ability does not belong to this SDK actor"));
    }
    scriptable_auth::Type2LaneTripleRef ability{};
    ability.values = {
        group, request, middleware::bap::activity_message::auth_fields::kClientRefAbsentKey};
    ability.reference.slotType = -1;
    ability.mode = -1;
    ability.value = -1;
    lua_getfield(state, table, "target");
    const bool targeted = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (targeted) {
        lua_getfield(state, table, "target");
        const auto* const handle =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition target{};
        const bool current = current_slot(state, *handle, target);
        lua_pop(state, 1);
        // Selector zero is the first authored point parameter, not an actor-target mode.
        constexpr std::uint32_t kFirstPointParameter = 0;
        if (!current || impl->definitions.resolveActorAbilityTarget == nullptr
            || !impl->definitions.resolveActorAbilityTarget(
                impl->definitions.context, target.nativeRow, kFirstPointParameter)) {
            static_cast<void>(luaL_error(state, "ability target has no authored point parameter"));
        }
        ability.reference = atom_target(state, table);
        ability.value = static_cast<std::int8_t>(kFirstPointParameter);
    }
    return ability;
}
/** One script-facing atom name and the reader that fills its native lane child. */
struct AtomKind final {
    std::string_view name;
    scriptable_auth::Type2LanePrimary (*read)(lua_State*, int);
};

/** The ten primary lane schemas the client-atom runner selects between. */
constexpr std::array<AtomKind, 10> kAtomKinds{{
    {"face", &atom_face},
    {"sequence", &atom_sequence},
    {"sleep", &atom_sleep},
    {"move_to", &atom_move_to},
    {"trivial", &atom_trivial},
    {"control_flag", &atom_control_flag},
    {"set_temperament", &atom_set_temperament},
    {"set_channel", &atom_set_channel},
    {"snap_to", &atom_snap_to},
    {"ability", nullptr},
}};

/** Builds one atom lane from its declaration table at stack index `table`. */
void parse_atom(lua_State* state,
                int table,
                const SlotDefinition& actor,
                scriptable_auth::Type2KeyedLane& lane) {
    lua_getfield(state, table, "kind");
    const std::string_view kind = lua_string_view(state, -1);
    const auto match =
        std::find_if(kAtomKinds.begin(), kAtomKinds.end(), [kind](const AtomKind& row) noexcept {
            return row.name == kind;
        });
    if (match == kAtomKinds.end()) {
        static_cast<void>(luaL_error(state, "unknown atom kind"));
        return;
    }
    lane.primary =
        match->read != nullptr ? match->read(state, table) : atom_ability(state, table, actor);
    lua_pop(state, 1);
    lua_getfield(state, table, "quantized");
    if (!lua_isnil(state, -1)) {
        const lua_Integer quantized = luaL_checkinteger(state, -1);
        if (quantized < 0 || quantized > 0x7FF) {
            static_cast<void>(luaL_error(state, "atom quantized value is wider than 11 bits"));
        }
        lane.secondary =
            scriptable_auth::Type2LaneQuantized11{static_cast<std::uint16_t>(quantized)};
    }
    lua_pop(state, 1);
}
} // namespace

/** Exposes the same closed operation names the native atom parser accepts. */
void push_atom_kinds(lua_State* state) {
    lua_createtable(state, 0, static_cast<int>(kAtomKinds.size()));
    for (const auto& kind : kAtomKinds) {
        lua_pushlstring(state, kind.name.data(), kind.name.size());
        lua_setfield(state, -2, kind.name.data());
    }
}

/** Stages an actor program while native state owns its creation and program revisions. */
[[nodiscard]] int slot_run_atoms(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_combatant_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-2 combatant");
    }
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 2> kDeclared{"spawn", "atoms"};
    refuse_unknown_arguments(state, kDeclared);
    const bool spawn = optional_boolean_argument(state, "spawn", false);
    // The encoder needs a positive template value; only the native reducer publishes counters.
    constexpr std::uint32_t kUncommittedRevision = 1;
    scriptable_auth::Type2Body body{};
    body.channels.revision = kUncommittedRevision;
    body.atoms.generation = kUncommittedRevision;
    lua_getfield(state, 2, "atoms");
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const auto count = static_cast<std::size_t>(lua_rawlen(state, list));
    if (count == 0 || count > body.atoms.lanes.size()) {
        return luaL_error(state, "an atom program holds one to 32 lanes");
    }
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index) + 1);
        luaL_checktype(state, -1, LUA_TTABLE);
        parse_atom(state, lua_gettop(state), slot, body.atoms.lanes[index]);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    body.atoms.count = static_cast<std::uint8_t>(count);

    std::array<std::byte, scriptable_auth::kType2MaximumBodyByteCount> encoded{};
    std::size_t written = 0;
    std::size_t writtenBits = 0;
    if (!scriptable_auth::encode_type2_body(body, encoded, written, writtenBits)) {
        return luaL_error(state, "atom program native encoder failed");
    }
    return queue_slot_auth(state,
                           slot,
                           scriptable_auth::kType2Schema,
                           writtenBits,
                           std::span<const std::byte>(encoded.data(), written),
                           IntentKind::runActorProgram,
                           spawn);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
