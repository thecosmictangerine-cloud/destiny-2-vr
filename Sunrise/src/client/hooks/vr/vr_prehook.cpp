/**
 * Pre-hooks: see vr_prehook.h for what they are. This file is the machinery.
 *
 * The thunk, per hook:
 *
 *   push rcx / rdx / r8 / r9          ; the integer arguments
 *   sub rsp, 0x68                     ; shadow space + xmm0-3 + alignment (rsp is 16-aligned here)
 *   movdqu [rsp+0x20..0x50], xmm0-3   ; the float arguments
 *   mov rcx, cookie / mov rdx, rsp / mov rax, callback / call rax
 *   movdqu xmm0-3, [...] / add rsp, 0x68 / pop r9 / r8 / rdx / rcx
 *   spin: mov rax, [slot] / test rax, rax / jz +2 / jmp rax / pause / jmp spin
 *
 * For an ordinary entry the slot is the Detours trampoline, which only exists once the transaction
 * has committed -- by which time the thunk can already be running -- so the thunk spins while the
 * slot is still null, for the microseconds between the commit and the store. For a virtualised
 * entry the slot is the old `jmp` destination, set before the `jmp` is retargeted at the thunk.
 *
 * Thunks are carved from one executable pool near the image (a retargeted `jmp rel32` must reach
 * them) and never freed: a thread may be inside one at any time.
 */

#include "vr_prehook.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "../../hooking/detour.h"

namespace sunrise::client::hooks::vr::prehook {
namespace {

constexpr std::size_t kPoolBytes = 0x40000;
constexpr std::size_t kMaxHooks = 256;

SRWLOCK g_lock = SRWLOCK_INIT;
std::uint8_t* g_pool = nullptr;
std::size_t g_poolUsed = 0;
std::array<hooking::detour::Handle, kMaxHooks> g_handles{};
std::size_t g_handleCount = 0;

std::uintptr_t image_base() noexcept {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

std::uint8_t* pool_alloc(std::size_t size) noexcept {
    if (g_pool == nullptr) {
        // Walk down from just below the image in 64 KB steps; the first free spot is well within a
        // 32-bit displacement of any code in the image (145 MB).
        const std::uintptr_t base = image_base();
        for (std::uintptr_t probe = (base & ~static_cast<std::uintptr_t>(0xFFFF)) - kPoolBytes;
             probe > 0x10000 && base - probe < 0x60000000; probe -= 0x10000) {
            g_pool = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(probe), kPoolBytes,
                                                             MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (g_pool != nullptr) {
                break;
            }
        }
        if (g_pool == nullptr) {
            return nullptr;
        }
    }
    const std::size_t aligned = (size + 15) & ~static_cast<std::size_t>(15);
    if (g_poolUsed + aligned > kPoolBytes) {
        return nullptr;
    }
    std::uint8_t* const p = g_pool + g_poolUsed;
    g_poolUsed += aligned;
    return p;
}

/** Emits a thunk. @return Its entry, or null; `slot` receives the address of its jump slot. */
std::uint8_t* make_thunk(Callback callback, std::uint64_t cookie, std::atomic<std::uint64_t>*& slot) noexcept {
    std::vector<std::uint8_t> c;
    auto bytes = [&](std::initializer_list<std::uint8_t> b) { c.insert(c.end(), b); };
    auto imm64 = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            c.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    bytes({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});  // push rcx, rdx, r8, r9
    bytes({0x48, 0x83, 0xEC, 0x68});              // sub rsp, 0x68
    bytes({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20});  // movdqu [rsp+0x20], xmm0
    bytes({0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30});  // movdqu [rsp+0x30], xmm1
    bytes({0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40});  // movdqu [rsp+0x40], xmm2
    bytes({0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50});  // movdqu [rsp+0x50], xmm3
    bytes({0x48, 0xB9});                          // mov rcx, cookie
    imm64(cookie);
    bytes({0x48, 0x89, 0xE2});                    // mov rdx, rsp
    bytes({0x48, 0xB8});                          // mov rax, callback
    imm64(reinterpret_cast<std::uint64_t>(callback));
    bytes({0xFF, 0xD0});                          // call rax
    bytes({0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20});  // movdqu xmm0, [rsp+0x20]
    bytes({0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30});
    bytes({0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40});
    bytes({0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50});
    bytes({0x48, 0x83, 0xC4, 0x68});              // add rsp, 0x68
    bytes({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});  // pop r9, r8, rdx, rcx
    const std::size_t spin = c.size();
    bytes({0x48, 0x8B, 0x05, 0, 0, 0, 0});        // mov rax, [rip+slot]
    const std::size_t relAt = c.size() - 4;
    bytes({0x48, 0x85, 0xC0});                    // test rax, rax
    bytes({0x74, 0x02});                          // jz pause
    bytes({0xFF, 0xE0});                          // jmp rax
    bytes({0xF3, 0x90});                          // pause
    bytes({0xEB, 0x00});                          // jmp spin
    c.back() = static_cast<std::uint8_t>(
        static_cast<std::int8_t>(static_cast<std::ptrdiff_t>(spin) - static_cast<std::ptrdiff_t>(c.size())));
    while (c.size() % 8 != 0) {
        c.push_back(0xCC);
    }
    const std::size_t slotAt = c.size();
    imm64(0);
    const auto rel = static_cast<std::int32_t>(slotAt - (relAt + 4));
    std::memcpy(c.data() + relAt, &rel, 4);
    std::uint8_t* const mem = pool_alloc(c.size());
    if (mem == nullptr) {
        return nullptr;
    }
    std::memcpy(mem, c.data(), c.size());
    FlushInstructionCache(GetCurrentProcess(), mem, c.size());
    slot = reinterpret_cast<std::atomic<std::uint64_t>*>(mem + slotAt);
    return mem;
}

/**
 * Retargets the `jmp rel32` at `target` to `thunk`. Only the displacement changes, written with one
 * interlocked exchange of the aligned block containing it, so a thread executing the jump sees the
 * old destination or the new one, never half of each.
 */
bool retarget_jump(std::uintptr_t target, const std::uint8_t* thunk) noexcept {
    const std::int64_t rel64 = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(thunk))
                               - static_cast<std::int64_t>(target + 5);
    if (rel64 > INT32_MAX || rel64 < INT32_MIN) {
        return false;
    }
    const auto rel = static_cast<std::int32_t>(rel64);
    const std::uintptr_t field = target + 1;
    const auto page = reinterpret_cast<void*>(target & ~static_cast<std::uintptr_t>(0xFFF));
    DWORD old = 0;
    if (VirtualProtect(page, 0x2000, PAGE_EXECUTE_READWRITE, &old) == FALSE) {
        return false;
    }
    bool ok = false;
    if ((field & 7) <= 4) {
        auto* block = reinterpret_cast<volatile LONG64*>(field & ~static_cast<std::uintptr_t>(7));
        const LONG64 before = *block;
        LONG64 after = before;
        std::memcpy(reinterpret_cast<std::uint8_t*>(&after) + (field & 7), &rel, 4);
        ok = InterlockedCompareExchange64(block, after, before) == before;
    } else if ((field & 15) <= 12) {
        auto* block = reinterpret_cast<LONG64*>(field & ~static_cast<std::uintptr_t>(15));
        alignas(16) LONG64 expected[2] = {block[0], block[1]};
        alignas(16) LONG64 desired[2] = {expected[0], expected[1]};
        std::memcpy(reinterpret_cast<std::uint8_t*>(desired) + (field & 15), &rel, 4);
        ok = InterlockedCompareExchange128(block, desired[1], desired[0], expected) != 0;
    }
    DWORD ignored = 0;
    VirtualProtect(page, 0x2000, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 5);
    return ok;
}

/** A requested post-call: where the call really returns to, and what to run first. */
struct PostEntry final {
    std::uint64_t ret{};
    PostCallback post{};
    std::uint64_t context{};
};
thread_local std::array<PostEntry, 16> t_post{};
thread_local std::size_t t_postCount = 0;
std::uint8_t* g_returnTrampoline = nullptr;

/**
 * Called by the return trampoline: runs the newest post-call of this thread and hands back the
 * address the call was really returning to. Calls return in the reverse order they were made, so a
 * per-thread stack pairs them up without any bookkeeping beyond the count.
 */
std::uint64_t post_dispatch() noexcept {
    const PostEntry e = t_post[--t_postCount];
    e.post(e.context);
    return e.ret;
}

/**
 *   push rax / sub rsp, 0x38 / movdqu [rsp+0x20], xmm0      ; the return value, rsp 16-aligned
 *   mov rax, post_dispatch / call rax / mov r11, rax          ; r11: the real return address
 *   movdqu xmm0, [rsp+0x20] / add rsp, 0x38 / pop rax / jmp r11
 *
 * r11 is volatile at a return point, so the caller cannot be relying on it.
 */
std::uint8_t* make_return_trampoline() noexcept {
    std::vector<std::uint8_t> c;
    auto bytes = [&](std::initializer_list<std::uint8_t> b) { c.insert(c.end(), b); };
    bytes({0x50});                                // push rax
    bytes({0x48, 0x83, 0xEC, 0x38});              // sub rsp, 0x38
    bytes({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20});  // movdqu [rsp+0x20], xmm0
    bytes({0x48, 0xB8});                          // mov rax, post_dispatch
    const auto fn = reinterpret_cast<std::uint64_t>(&post_dispatch);
    for (int i = 0; i < 8; ++i) {
        c.push_back(static_cast<std::uint8_t>(fn >> (8 * i)));
    }
    bytes({0xFF, 0xD0});                          // call rax
    bytes({0x49, 0x89, 0xC3});                    // mov r11, rax
    bytes({0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20});  // movdqu xmm0, [rsp+0x20]
    bytes({0x48, 0x83, 0xC4, 0x38});              // add rsp, 0x38
    bytes({0x58});                                // pop rax
    bytes({0x41, 0xFF, 0xE3});                    // jmp r11
    std::uint8_t* const mem = pool_alloc(c.size());
    if (mem != nullptr) {
        std::memcpy(mem, c.data(), c.size());
        FlushInstructionCache(GetCurrentProcess(), mem, c.size());
    }
    return mem;
}

} // namespace

bool request_post(std::uint8_t* frame, PostCallback post, std::uint64_t context) noexcept {
    if (g_returnTrampoline == nullptr || t_postCount >= t_post.size()) {
        return false;
    }
    t_post[t_postCount++] = PostEntry{return_address(frame), post, context};
    const auto trampoline = reinterpret_cast<std::uint64_t>(g_returnTrampoline);
    std::memcpy(frame + kReturn, &trampoline, sizeof trampoline);
    return true;
}

bool read(std::uintptr_t address, void* out, std::size_t size) noexcept {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &got) != FALSE
           && got == size;
}

bool write(std::uintptr_t address, const void* in, std::size_t size) noexcept {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), in, size, &put) != FALSE
           && put == size;
}

bool attach(std::uintptr_t target, Callback callback, std::uint64_t cookie, const char*& failure) noexcept {
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(target, &imageBase, nullptr);
    if (fe == nullptr || imageBase + fe->BeginAddress != target) {
        failure = "not_a_function_start";
        return false;
    }
    std::array<std::uint8_t, 5> first{};
    if (!read(target, first.data(), first.size()) || first[0] == 0xEB || (first[0] == 0xFF && first[1] == 0x25)) {
        failure = "entry_is_short_or_indirect_jump";
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_returnTrampoline == nullptr) {
        g_returnTrampoline = make_return_trampoline();
    }
    std::atomic<std::uint64_t>* slot = nullptr;
    std::uint8_t* const thunk = make_thunk(callback, cookie, slot);
    if (thunk == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        failure = "pool";
        return false;
    }
    bool ok = false;
    if (first[0] == 0xE9) {
        // A virtualised entry (or another module's detour): the five-byte jump IS the hook point.
        std::int32_t rel = 0;
        std::memcpy(&rel, first.data() + 1, 4);
        slot->store(static_cast<std::uint64_t>(target + 5 + static_cast<std::intptr_t>(rel)), std::memory_order_release);
        ok = retarget_jump(target, thunk);
        failure = ok ? "" : "retarget";
    } else if (g_handleCount < g_handles.size()) {
        hooking::detour::Handle& handle = g_handles[g_handleCount];
        const hooking::detour::Spec spec{reinterpret_cast<void*>(target), thunk};
        ok = hooking::detour::install(spec, handle);
        if (ok) {
            ++g_handleCount;
            slot->store(reinterpret_cast<std::uint64_t>(handle.original), std::memory_order_release);
        }
        failure = ok ? "" : "detour";
    } else {
        failure = "full";
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

std::size_t walk(const std::uint8_t* frame, std::uintptr_t* out, std::size_t capacity) noexcept {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fe = nullptr;
    PVOID handlerData = nullptr;
    DWORD64 establisher = 0;
    // Unwind out of this module's own frames -- however many the inliner left between here and the
    // thunk -- until the next frame has no unwind data: that is the thunk, which must sit exactly at
    // `frame`, or the layout assumed below would be wrong and the walk stops.
    for (int guard = 0; guard < 12; ++guard) {
        fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fe == nullptr) {
            break;
        }
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
    }
    if (fe != nullptr || ctx.Rsp != reinterpret_cast<DWORD64>(frame)) {
        return 0;
    }
    ctx.Rip = return_address(frame);
    ctx.Rsp = reinterpret_cast<DWORD64>(frame) + kReturn + 8;
    std::size_t n = 0;
    while (n < capacity) {
        if (ctx.Rip == 0 || ctx.Rsp < low || ctx.Rsp >= high) {
            break;
        }
        out[n++] = static_cast<std::uintptr_t>(ctx.Rip);
        fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fe == nullptr) {
            break;  // virtualised code: no unwind data, and guessing would read wild memory
        }
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
    }
    return n;
}

} // namespace sunrise::client::hooks::vr::prehook
