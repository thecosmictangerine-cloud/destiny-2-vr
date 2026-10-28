#pragma once

#include <cstdint>

#include "definition.h"

namespace sunrise::state::activity::destination {

/**
 * Finds the free-roam activity that runs the public regions of one destination.
 * @param activityIndex Activity whose destination is looked up.
 * @param output Receives the free-roam activity index; unchanged on failure.
 * @return False when the catalog is not published or the destination has no free-roam row.
 */
[[nodiscard]] bool free_roam_activity(std::int16_t activityIndex, std::uint16_t& output) noexcept;

/**
 * Builds the destination a public session runs: the source's free-roam activity, same world.
 * @param source Destination of the private session the public one serves.
 * @param output Receives the public destination; unchanged on failure.
 * @return False when the source has no free-roam activity.
 */
[[nodiscard]] bool public_destination(const DestinationSelection& source,
                                      DestinationSelection& output) noexcept;

} // namespace sunrise::state::activity::destination
