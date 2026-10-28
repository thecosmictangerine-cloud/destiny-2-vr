#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "../../../middleware/bap/activity_message/squad_auth_body.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

// Metatable names that lock the value userdata shapes a program may hold.
inline constexpr char kUnitScalarMetatable[] = "sunrise.sdk.unit_scalar";
inline constexpr char kSquadModeMetatable[] = "sunrise.sdk.squad_mode";
inline constexpr char kSquadModeCollectionMetatable[] = "sunrise.sdk.squad_modes";
inline constexpr char kSquadCountVectorMetatable[] = "sunrise.sdk.squad_counts";

/** Every device lane clamps to this closed range, so a named transition picks one endpoint. */
inline constexpr float kUnitLaneLow = 0.0F;
inline constexpr float kUnitLaneHigh = 1.0F;

/** The requested-count array the client indexes holds this many elements. */
inline constexpr std::size_t kSquadCountCapacity =
    middleware::bap::activity_message::squad_auth::kMaximumRequestedCountLength;

/** One device lane value. The mint does not check its range. */
struct UnitScalarHandle final {
    float value{};
};

/** One squad placement mode. The wire lane is wider than the two named values. */
struct SquadModeHandle final {
    std::uint8_t mode{};
};

struct SquadModeCollectionHandle final {
    std::uint8_t marker{};
};

/**
 * One squad's requested counts, bound to the squad that minted it.
 * The length is the squad's member count, so no caller ever writes a count lane.
 */
struct SquadCountVectorHandle final {
    std::array<std::int32_t, kSquadCountCapacity> counts{};
    std::uint32_t squadRow{};
    std::uint8_t count{};
};

/** @return The Lua spelling of one named squad mode, or "unknown". */
[[nodiscard]] std::string_view squad_mode_name(std::uint8_t mode) noexcept;

void push_unit_scalar(lua_State* state, float value);
void push_squad_mode(lua_State* state, std::uint8_t mode);
void push_squad_count_vector(lua_State* state, const SquadCountVectorHandle& value);

/** Mints one count vector prefilled from the squad's authored defaults. */
[[nodiscard]] int squad_counts(lua_State* state);

/** Registers every locked value userdata shape. */
void register_value_metatables(lua_State* state);

/** Pushes one recognized ActivityView value member and returns whether the key was owned. */
[[nodiscard]] bool push_value_activity_member(lua_State* state, std::string_view key);

} // namespace sunrise::server::activity::mission::lua_vm::detail
