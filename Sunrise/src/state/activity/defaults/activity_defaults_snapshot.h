#pragma once

#include "definition.h"

namespace sunrise::state::activity::defaults {

/** Copies the immutable activity defaults published with the root State. */
void snapshot(ActivityDefaults& output) noexcept;

} // namespace sunrise::state::activity::defaults
