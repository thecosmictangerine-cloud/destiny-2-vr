#pragma once

/**
 * A hang leaves its own evidence (first headset session, 2026-10-18: F9 back on froze the game, Windows
 * closed it, and nothing but a stack scan was left to read). A thread of its own watches the presents;
 * when none has come for 8 s after the game had been presenting, it writes ONE small dump of the process
 * (every thread's context and stack, MiniDumpNormal + thread info) to `SVR_Hang.dmp` beside the game, for
 * `scripts/diagnostics/hang_threads.py`. dbghelp is loaded when the watch starts, never during a hang.
 */
namespace sunrise::client::hooks::vr::watchdog {

/** Present thread, every present: starts the watch on the first, then only counts. */
void present() noexcept;

} // namespace sunrise::client::hooks::vr::watchdog
