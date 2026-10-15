#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sunrise::client::hooks::vr::prehook {

/**
 * Runs a callback at the entry of any function, before it, with every argument it was given.
 *
 * Built for this executable, where whole subsystems are virtualised by VMProtect: their entry is a
 * five-byte `jmp` into the virtual machine, there is no x86 to detour behind it, and the argument
 * types are unknown. A pre-hook needs neither. Each one is a generated thunk that saves rcx, rdx,
 * r8, r9 and xmm0-3, calls the callback with a pointer to that save area, restores them and goes
 * on into the original -- through a Detours trampoline for an ordinary entry, or by retargeting the
 * entry `jmp` itself for a virtualised one. The callback can read the arguments, follow pointers,
 * and rewrite what they point at; the function then runs on the rewritten data.
 *
 * Nothing here raises an exception (the one thing VMProtect punishes): memory is only touched
 * through the callback's own reads, and the stack walk uses the unwind tables within the thread's
 * stack limits.
 */
using Callback = void (*)(std::uint64_t cookie, std::uint8_t* frame) noexcept;

/** Save-area layout the callback receives. */
inline constexpr std::size_t kXmm0 = 0x20;
inline constexpr std::size_t kR9 = 0x68;
inline constexpr std::size_t kR8 = 0x70;
inline constexpr std::size_t kRdx = 0x78;
inline constexpr std::size_t kRcx = 0x80;
inline constexpr std::size_t kReturn = 0x88;

/** @return Integer argument `index` (0 = rcx ... 3 = r9, 4 and up from the caller's stack). */
inline std::uint64_t arg(const std::uint8_t* frame, std::size_t index) noexcept {
    static constexpr std::size_t kReg[4] = {kRcx, kRdx, kR8, kR9};
    std::uint64_t v = 0;
    // Stack arguments: the caller's rsp at the call is frame + kReturn, the fifth argument sits
    // 0x28 above it (return address, then the 0x20 shadow space).
    std::memcpy(&v, frame + (index < 4 ? kReg[index] : kReturn + 0x28 + 8 * (index - 4)), sizeof v);
    return v;
}

/** @return The return address into the hooked function's caller. */
inline std::uint64_t return_address(const std::uint8_t* frame) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, frame + kReturn, sizeof v);
    return v;
}

/**
 * Hooks the function at `target`, which must be a function entry per the unwind tables.
 * @param failure Receives a short reason on failure.
 * @return True when the hook is live.
 */
[[nodiscard]] bool attach(std::uintptr_t target, Callback callback, std::uint64_t cookie,
                          const char*& failure) noexcept;

/** Runs when the hooked call returns. `context` is whatever the pre-hook passed along. */
using PostCallback = void (*)(std::uint64_t context) noexcept;

/**
 * From inside a pre-hook callback: run `post(context)` when THIS call returns, before its caller
 * sees anything -- the way to rewrite what a function wrote into its caller's buffers.
 *
 * Done by swapping the call's return address for a small trampoline that runs `post` and jumps on
 * to the real return address, kept in a per-thread stack. The return value (rax, xmm0) is preserved.
 * Nothing in the game may walk the stack through the call while it runs -- none does, and nothing
 * here raises an exception -- but it is the reason to request this only for the calls that need it.
 * @return False when the per-thread stack is full (deep recursion): the call then runs untouched.
 */
bool request_post(std::uint8_t* frame, PostCallback post, std::uint64_t context) noexcept;

/**
 * The hooked call's chain of return addresses, starting with the caller's. Must be called from
 * inside the callback (it unwinds through the callback's own frames to find the thunk).
 * @return Entries written.
 */
std::size_t walk(const std::uint8_t* frame, std::uintptr_t* out, std::size_t capacity) noexcept;

/** @return True when Windows copied all of it: a pointer read that cannot fault. */
[[nodiscard]] bool read(std::uintptr_t address, void* out, std::size_t size) noexcept;

/** @return True when Windows wrote all of it. */
[[nodiscard]] bool write(std::uintptr_t address, const void* in, std::size_t size) noexcept;

} // namespace sunrise::client::hooks::vr::prehook
