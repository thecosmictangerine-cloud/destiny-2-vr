#pragma once

namespace sunrise::client::hooks::vr::destination {

/**
 * Persistent forced destination for the test loop.
 *
 * Sunrise's Activity override panel forces a destination through
 * `state::activity::forced::publish`, but keeps nothing between runs. This reads the same
 * selection from `SVR_Destination.txt` beside the executable and publishes it, so a scripted
 * session lands in a known first-person area from any Director launch. One line:
 *
 *   <package_name> <bubble> <slice_set> [<activity_index> [<spawn_set_hash>]]
 *
 * e.g. `eden_freeroam 20 160 7`. The activity index is the definition the package binds to (the
 * `activity=N` of `ev=bap svc=42`); without it the launch keeps the Director node's definition,
 * which hangs the load when that node was not a patrol. Hex is accepted for the hash
 * (`0x2EA8FB98`). The file is read on the
 * first camera frame and re-read whenever its contents change; it is never deleted. No file means
 * nothing is published and the panel's own selection stands. An `off` line clears the override.
 */
void tick() noexcept;

} // namespace sunrise::client::hooks::vr::destination
