#pragma once

#include <string_view>

#include "../../../state/activity_sdk/runtime.h"

namespace sunrise::server::ui::activity_host::sdk_squad_view {

/** Draws generated scenario squads and the guarded server-side place action. */
void draw(const state::activity_sdk::BoundView& view,
          const state::activity_sdk::format::Scenario& scenario) noexcept;

/** @return The name the squad table shows: source slot name, then slot id, then squad id. */
[[nodiscard]] std::string_view
squad_display_name(const state::activity_sdk::Catalog& catalog,
                   const state::activity_sdk::format::Squad& squad) noexcept;

} // namespace sunrise::server::ui::activity_host::sdk_squad_view
