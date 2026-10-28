#pragma once

#include "activity_sdk_actor_rsat_inventory.h"
#include "activity_sdk_squad_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::actor_ability_inventory {

/** Requires validated squad-link inputs; extracts actor keys and bounded Type 58 targets. */
[[nodiscard]] bool build(const topology_inventory::Snapshot& topology,
                         const squad_inventory::Facts& facts,
                         const squad_inventory::Snapshot& squads,
                         squad_inventory::TagReader reader,
                         void* context,
                         actor_rsat_inventory::Snapshot& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::actor_ability_inventory
