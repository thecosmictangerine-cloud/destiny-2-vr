#pragma once

/**
 * The mod is built for one executable: the Season of Arrivals destiny2.exe that Project Sunrise
 * installs (PE timestamp 2020-08-24, the depot manifest the Sunrise guide names). The same DLL
 * dropped into a current Steam install of Destiny 2 would patch a live, online game protected by
 * BattlEye, which is how an account gets banned. So before Sunrise or the VR module touches
 * anything, the main image's headers are compared with that build's, and any other executable is
 * told why in a message box and closed. Nothing has been hooked or written by then.
 */
namespace sunrise::client::hooks::vr::game_check {

/**
 * Closes the process unless it is the Sunrise build of destiny2.exe. Call it first in every entry
 * point that installs hooks; it costs two header reads and does nothing on the right game.
 */
void stop_unless_sunrise_build() noexcept;

} // namespace sunrise::client::hooks::vr::game_check
