#pragma once

#include <array>

namespace sunrise::client::hooks::vr::probe {

/**
 * A live function tracer for reverse engineering inside a VMProtect'd process.
 *
 * Breakpoints, single-stepping and guard pages get this game killed (RESEARCH.md, "VMProtect"), so
 * the only way to watch code run is a detour. This module detours ANY function on demand, from a
 * command file, without a rebuild: each hook is a small generated thunk that saves every argument
 * register (rcx, rdx, r8, r9, xmm0-3), calls a recorder, restores them and continues into the
 * original. The recorder never raises an exception -- memory is read with ReadProcessMemory and the
 * stack is walked with the unwind tables, bounded by the thread's own stack limits.
 *
 * What it records, per hook:
 *   - calls, per report period;
 *   - each distinct caller, the first time it is seen, with its whole call chain;
 *   - on request, the next N calls' argument registers and the floats they point at;
 *   - while the fire button is down, every call whose arguments carry the character's aim -- a unit
 *     vector equal to the engine's camera forward, or a point at the eye -- directly or one pointer
 *     deep. That is the search for the function a shot is aimed with.
 *
 * `SVR_Probe.txt`, one command per line, consumed on read:
 *   hook <rva>[,<rva>...] [name]   detour these function entries (checked against .pdata)
 *   args <rva> <n>                 log the next n calls' registers and pointed-at floats
 *   scan on|off                    aim matching while the left button is down (default on)
 *   report                         counts now
 */
void tick() noexcept;

/**
 * The character's aim this frame, as the engine produced it before anything of ours: eye position
 * and forward, game basis. The recorder matches arguments against it. Camera thread.
 */
void set_reference(const std::array<float, 3>& eye, const std::array<float, 3>& forward) noexcept;

} // namespace sunrise::client::hooks::vr::probe
