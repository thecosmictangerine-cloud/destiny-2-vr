/**
 * Access watch: which code touches one address?
 *
 * The game marks every one of its threads ThreadHideFromDebugger, so an attached debugger with a
 * data breakpoint never receives the trap (measured: 70 threads hidden, 0 events). This does the
 * same job from inside the process, where hiding does not apply: a helper thread arms hardware
 * breakpoint DR0 on every other thread, a vectored exception handler records each instruction
 * pointer that trips it, and once the window closes the sites are reported.
 *
 * Diagnostic only. It exists to find the code that builds the first-person weapon view from the
 * camera pose (F3), and it is driven from outside so the game never has to be rebuilt per query.
 */

#include "vr_watch.h"

#include <Windows.h>

#include <TlHelp32.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../../../core/logging/log.h"

namespace sunrise::client::hooks::vr::watch {
namespace {

constexpr std::size_t kMaxSites = 96;
constexpr std::size_t kMaxThreadsPerSite = 6;
constexpr std::uint32_t kPollPeriod = 30;
constexpr wchar_t kCommandName[] = L"SVR_Watch.txt";
constexpr wchar_t kReportName[] = L"SVR_Watch.log";
constexpr DWORD kMaxWindowMs = 20000;
constexpr std::size_t kMaxThreads = 512;
constexpr std::size_t kMaxForeign = 8;

/** What a thread's debug registers held before the watch, so the end restores them exactly. */
struct Saved final {
    DWORD thread{0};
    std::array<std::uint64_t, 4> dr{};
    std::uint64_t dr7{0};
};
std::array<Saved, kMaxThreads> g_saved{};
std::size_t g_savedCount = 0;
std::uint32_t g_dr7Busy = 0;

/** Single-steps the handler declined, kept to tell a wrong slot decode from a silent breakpoint. */
struct Foreign final {
    std::uint64_t rip{0};
    std::uint64_t dr6{0};
    std::uint64_t dr7{0};
};
std::array<Foreign, kMaxForeign> g_foreign{};
std::atomic<std::size_t> g_foreignCount{0};
/** Debug register slot the current command uses, 0-3. */
std::atomic<unsigned> g_slot{0};

struct Site final {
    std::atomic<std::uintptr_t> rip{0};
    std::atomic<std::uint64_t> count{0};
    std::array<std::atomic<std::uint32_t>, kMaxThreadsPerSite> threads{};
    std::atomic_bool regsTaken{false};
    /** Registers and the top of the stack at the first hit, for finding the struct pointer. */
    std::array<std::uint64_t, 16> regs{};
    std::uint64_t stackTop{0};
    /** The first words of the stack at the first hit: return addresses among them make a rough backtrace. */
    std::array<std::uint64_t, 24> stack{};
};

struct Command final {
    std::uintptr_t address{0};
    std::uint32_t length{4};
    bool writeOnly{false};
    DWORD windowMs{3000};
    unsigned slot{0};
};

std::array<Site, kMaxSites> g_sites;
std::atomic_bool g_armed{false};
std::atomic<std::uint64_t> g_hits{0};
std::atomic<std::uint64_t> g_overflow{0};
std::atomic<std::uint64_t> g_foreignSteps{0};
void* g_handler = nullptr;
HANDLE g_helper = nullptr;
Command g_command{};
std::uint32_t g_frame = 0;

/** Emits one line on the client channel at a level the stock thresholds admit. */
void report(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, text);
}

template <typename... Args> void reportf(const char* format, Args... args) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(), line.size(), format, args...);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size() ? static_cast<std::size_t>(written)
                                                                            : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::warn, {line.data(), length});
    }
}

/** Directory of the executable, with a trailing separator, plus the given file name. */
bool game_path(const wchar_t* name, std::array<wchar_t, MAX_PATH>& out) noexcept {
    const DWORD length = GetModuleFileNameW(nullptr, out.data(), static_cast<DWORD>(out.size()));
    if (length == 0 || length >= out.size()) {
        return false;
    }
    wchar_t* const slash = std::wcsrchr(out.data(), L'\\');
    if (slash == nullptr) {
        return false;
    }
    const std::size_t dirLength = static_cast<std::size_t>(slash + 1 - out.data());
    if (dirLength + std::wcslen(name) + 1 > out.size()) {
        return false;
    }
    return wcscpy_s(slash + 1, out.size() - dirLength, name) == 0;
}

/** Records one trap. Runs on the trapping thread inside the exception dispatcher: no locks. */
void record(const CONTEXT& ctx) noexcept {
    const std::uintptr_t rip = static_cast<std::uintptr_t>(ctx.Rip);
    g_hits.fetch_add(1, std::memory_order_relaxed);
    for (Site& site : g_sites) {
        std::uintptr_t current = site.rip.load(std::memory_order_acquire);
        if (current == 0) {
            std::uintptr_t expected = 0;
            if (site.rip.compare_exchange_strong(expected, rip, std::memory_order_acq_rel)) {
                current = rip;
            } else {
                current = expected;
            }
        }
        if (current != rip) {
            continue;
        }
        site.count.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t thread = GetCurrentThreadId();
        for (auto& slot : site.threads) {
            std::uint32_t empty = 0;
            const std::uint32_t seen = slot.load(std::memory_order_relaxed);
            if (seen == thread) {
                break;
            }
            if (seen == 0 && slot.compare_exchange_strong(empty, thread, std::memory_order_relaxed)) {
                break;
            }
        }
        if (!site.regsTaken.exchange(true, std::memory_order_acq_rel)) {
            site.regs = {ctx.Rax, ctx.Rcx, ctx.Rdx, ctx.Rbx, ctx.Rsp, ctx.Rbp, ctx.Rsi, ctx.Rdi,
                         ctx.R8,  ctx.R9,  ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15};
            std::uint64_t top = 0;
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Rsp), &top, sizeof top, &got)
                && got == sizeof top) {
                site.stackTop = top;
            }
            std::array<std::uint64_t, 24> words{};
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Rsp), words.data(),
                                  sizeof words, &got)
                && got == sizeof words) {
                site.stack = words;
            }
        }
        return;
    }
    g_overflow.fetch_add(1, std::memory_order_relaxed);
}

LONG CALLBACK handler(EXCEPTION_POINTERS* info) noexcept {
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT& ctx = *info->ContextRecord;
    // Ours means: our slot reports the hit and that slot holds our address. Anything else -- the
    // game's own trap flag or a breakpoint of its own -- must reach its handler untouched. A trap
    // of ours must be eaten even outside the window (a thread armed before the window opened, or
    // not yet disarmed after it closed): handing the game a stray single-step made it disable the
    // debug registers everywhere and the watch went silent.
    const unsigned slot = g_slot.load(std::memory_order_relaxed);
    const std::uint64_t dr6Bit = 1ULL << slot;
    std::uint64_t slotAddress = 0;
    switch (slot) {
    case 0: slotAddress = ctx.Dr0; break;
    case 1: slotAddress = ctx.Dr1; break;
    case 2: slotAddress = ctx.Dr2; break;
    default: slotAddress = ctx.Dr3; break;
    }
    const bool ours = (ctx.Dr6 & dr6Bit) != 0 && slotAddress == static_cast<std::uint64_t>(g_command.address);
    if (!ours) {
        const std::size_t n = static_cast<std::size_t>(g_foreignSteps.fetch_add(1, std::memory_order_relaxed));
        if (n < kMaxForeign) {
            g_foreign[n] = Foreign{ctx.Rip, ctx.Dr6, ctx.Dr7};
            g_foreignCount.store(n + 1, std::memory_order_release);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ctx.Dr6 &= ~dr6Bit;
    if (g_armed.load(std::memory_order_acquire)) {
        record(ctx);
    }
    // A data breakpoint traps after the access completed, so nothing has to be re-executed.
    return EXCEPTION_CONTINUE_EXECUTION;
}

/**
 * Arms DR3 on one thread, remembering what the thread held, or puts the remembered values back.
 * The thread is suspended for the duration of the edit.
 */
bool set_breakpoint(HANDLE thread, DWORD threadId, const Command& command, bool clear) noexcept {
    alignas(16) CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        return false;
    }
    bool ok = false;
    if (GetThreadContext(thread, &ctx) != FALSE) {
        const unsigned slot = command.slot & 3U;
        const std::uint64_t enableBits = 0x3ULL << (slot * 2U);
        const std::uint64_t controlMask = 0xFULL << (16U + slot * 4U);
        if (clear) {
            // Restore exactly; a thread we never armed keeps whatever it has.
            for (std::size_t i = 0; i < g_savedCount; ++i) {
                if (g_saved[i].thread == threadId) {
                    ctx.Dr0 = g_saved[i].dr[0];
                    ctx.Dr1 = g_saved[i].dr[1];
                    ctx.Dr2 = g_saved[i].dr[2];
                    ctx.Dr3 = g_saved[i].dr[3];
                    ctx.Dr7 = g_saved[i].dr7;
                    break;
                }
            }
        } else {
            if (g_savedCount < g_saved.size()) {
                g_saved[g_savedCount++] = Saved{threadId, {ctx.Dr0, ctx.Dr1, ctx.Dr2, ctx.Dr3}, ctx.Dr7};
            }
            if ((ctx.Dr7 & 0xFFULL) != 0) {
                ++g_dr7Busy;
            }
            const std::uint64_t rw = command.writeOnly ? 0x1ULL : 0x3ULL;
            std::uint64_t len = 0x3ULL;
            switch (command.length) {
            case 1: len = 0x0ULL; break;
            case 2: len = 0x1ULL; break;
            case 8: len = 0x2ULL; break;
            default: len = 0x3ULL; break;
            }
            switch (slot) {
            case 0: ctx.Dr0 = command.address; break;
            case 1: ctx.Dr1 = command.address; break;
            case 2: ctx.Dr2 = command.address; break;
            default: ctx.Dr3 = command.address; break;
            }
            const std::uint64_t local = 0x1ULL << (slot * 2U);
            ctx.Dr7 = (ctx.Dr7 & ~(controlMask | enableBits)) | local | (rw << (16U + slot * 4U))
                      | (len << (18U + slot * 4U));
        }
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ok = SetThreadContext(thread, &ctx) != FALSE;
    }
    ResumeThread(thread);
    return ok;
}

struct ArmResult final {
    std::uint32_t ok{0};
    std::uint32_t failed{0};
};

/** Applies set_breakpoint to every thread of the process except the caller. */
ArmResult for_all_threads(const Command& command, bool clear) noexcept {
    ArmResult result{};
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }
    const DWORD self = GetCurrentThreadId();
    const DWORD process = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Thread32First(snapshot, &entry); more != FALSE; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME
                                             | THREAD_QUERY_INFORMATION,
                                         FALSE,
                                         entry.th32ThreadID);
        if (thread == nullptr) {
            ++result.failed;
            continue;
        }
        if (set_breakpoint(thread, entry.th32ThreadID, command, clear)) {
            ++result.ok;
        } else {
            ++result.failed;
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return result;
}

/** Formats an address as module+offset when it lies inside a module, else as a raw hex. */
void describe(std::uintptr_t address, char* out, std::size_t size) noexcept {
    HMODULE module = nullptr;
    if (address != 0
        && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address),
                              &module)
               != FALSE
        && module != nullptr) {
        std::array<wchar_t, MAX_PATH> path{};
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        const wchar_t* name = path.data();
        if (length != 0) {
            const wchar_t* const slash = std::wcsrchr(path.data(), L'\\');
            if (slash != nullptr) {
                name = slash + 1;
            }
        }
        std::snprintf(out, size, "%ls+0x%llX", name,
                      static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
        return;
    }
    std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
}

/** Writes the sites to the report file and the structured log. */
void publish(const Command& command, ArmResult armed, ArmResult cleared) noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    FILE* file = nullptr;
    if (game_path(kReportName, path) && _wfopen_s(&file, path.data(), L"a") != 0) {
        file = nullptr;
    }
    std::size_t sites = 0;
    for (const Site& site : g_sites) {
        if (site.rip.load(std::memory_order_acquire) != 0) {
            ++sites;
        }
    }
    reportf("ev=vr.watch done addr=0x%llX len=%u mode=%s ms=%lu slot=%u hits=%llu sites=%zu armed=%u/%u "
            "cleared=%u/%u overflow=%llu foreign_steps=%llu dr7_busy_before=%u",
            static_cast<unsigned long long>(command.address), command.length, command.writeOnly ? "w" : "rw",
            static_cast<unsigned long>(command.windowMs), command.slot,
            static_cast<unsigned long long>(g_hits.load()), sites, armed.ok, armed.ok + armed.failed, cleared.ok,
            cleared.ok + cleared.failed, static_cast<unsigned long long>(g_overflow.load()),
            static_cast<unsigned long long>(g_foreignSteps.load()), g_dr7Busy);
    const std::size_t foreign = g_foreignCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < foreign && i < kMaxForeign; ++i) {
        std::array<char, 96> where{};
        describe(static_cast<std::uintptr_t>(g_foreign[i].rip), where.data(), where.size());
        reportf("ev=vr.watch foreign rip=%s dr6=0x%llX dr7=0x%llX", where.data(),
                static_cast<unsigned long long>(g_foreign[i].dr6),
                static_cast<unsigned long long>(g_foreign[i].dr7));
    }
    if (file != nullptr) {
        std::fprintf(file, "watch addr=0x%llX len=%u mode=%s ms=%lu hits=%llu sites=%zu armed=%u/%u\n",
                     static_cast<unsigned long long>(command.address), command.length,
                     command.writeOnly ? "w" : "rw", static_cast<unsigned long>(command.windowMs),
                     static_cast<unsigned long long>(g_hits.load()), sites, armed.ok, armed.ok + armed.failed);
    }
    static constexpr const char* kRegNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                                  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    for (const Site& site : g_sites) {
        const std::uintptr_t rip = site.rip.load(std::memory_order_acquire);
        if (rip == 0) {
            continue;
        }
        std::array<char, 96> where{};
        std::array<char, 96> caller{};
        describe(rip, where.data(), where.size());
        describe(static_cast<std::uintptr_t>(site.stackTop), caller.data(), caller.size());
        std::array<char, 64> threads{};
        std::size_t used = 0;
        for (const auto& slot : site.threads) {
            const std::uint32_t id = slot.load(std::memory_order_relaxed);
            if (id == 0 || used >= threads.size() - 1) {
                break;
            }
            const int n = std::snprintf(threads.data() + used, threads.size() - used, "%s%lu", used ? "," : "",
                                        static_cast<unsigned long>(id));
            if (n <= 0) {
                break;
            }
            used += static_cast<std::size_t>(n);
        }
        reportf("ev=vr.watch site rip=%s abs=0x%llX top=%s count=%llu threads=%s", where.data(),
                static_cast<unsigned long long>(rip), caller.data(),
                static_cast<unsigned long long>(site.count.load()), threads.data());
        if (file != nullptr) {
            std::fprintf(file, "site rip=%s abs=0x%llX top=%s count=%llu threads=%s\n  regs", where.data(),
                         static_cast<unsigned long long>(rip), caller.data(),
                         static_cast<unsigned long long>(site.count.load()), threads.data());
            for (std::size_t i = 0; i < site.regs.size(); ++i) {
                std::fprintf(file, " %s=0x%llX", kRegNames[i], static_cast<unsigned long long>(site.regs[i]));
            }
            std::fprintf(file, "\n  stack");
            for (std::size_t i = 0; i < site.stack.size(); ++i) {
                HMODULE module = nullptr;
                const auto word = static_cast<std::uintptr_t>(site.stack[i]);
                if (word != 0
                    && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                              | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                          reinterpret_cast<LPCWSTR>(word), &module)
                           != FALSE) {
                    std::array<char, 96> text{};
                    describe(word, text.data(), text.size());
                    std::fprintf(file, " [+%zX]=%s", i * sizeof(std::uint64_t), text.data());
                }
            }
            std::fprintf(file, "\n");
        }
    }
    if (file != nullptr) {
        std::fprintf(file, "end\n");
        std::fclose(file);
    }
}

DWORD WINAPI helper(void*) noexcept {
    const Command command = g_command;
    for (Site& site : g_sites) {
        site.rip.store(0);
        site.count.store(0);
        for (auto& slot : site.threads) {
            slot.store(0);
        }
        site.regsTaken.store(false);
        site.regs.fill(0);
        site.stackTop = 0;
    }
    g_hits.store(0);
    g_overflow.store(0);
    g_foreignSteps.store(0);
    g_savedCount = 0;
    g_dr7Busy = 0;
    g_foreignCount.store(0);
    g_slot.store(command.slot & 3U);
    // The handler lives only as long as the window: outside it every exception is the game's.
    g_handler = AddVectoredExceptionHandler(1, handler);
    reportf("ev=vr.watch handler result=%s", g_handler != nullptr ? "ok" : "fail");
    // Open the window before the first thread is armed: a trap must never find it closed.
    g_armed.store(true, std::memory_order_release);
    const ArmResult armed = for_all_threads(command, false);
    reportf("ev=vr.watch armed addr=0x%llX ok=%u failed=%u dr7_busy_before=%u",
            static_cast<unsigned long long>(command.address), armed.ok, armed.failed, g_dr7Busy);
    Sleep(command.windowMs);
    g_armed.store(false, std::memory_order_release);
    const ArmResult cleared = for_all_threads(command, true);
    // Give any trap already in flight a moment to reach the handler before it goes away.
    Sleep(50);
    if (g_handler != nullptr) {
        RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
    publish(command, armed, cleared);
    return 0;
}

/** Parses `<hex address> <len> <w|rw> <ms> [slot]`. */
bool parse(const char* text, Command& out) noexcept {
    char mode[8] = {};
    unsigned long long address = 0;
    unsigned int length = 4;
    unsigned long ms = 3000;
    unsigned int slot = 0;
    if (sscanf_s(text, "%llx %u %7s %lu %u", &address, &length, mode, static_cast<unsigned>(sizeof mode), &ms,
                 &slot)
            < 1
        || address == 0) {
        return false;
    }
    out.slot = slot & 3U;
    out.address = static_cast<std::uintptr_t>(address);
    out.length = (length == 1 || length == 2 || length == 4 || length == 8) ? length : 4;
    out.writeOnly = std::strcmp(mode, "w") == 0;
    out.windowMs = static_cast<DWORD>(ms == 0 ? 3000UL : (ms > kMaxWindowMs ? kMaxWindowMs : ms));
    return true;
}

} // namespace

void tick() noexcept {
    if (++g_frame % kPollPeriod != 0) {
        return;
    }
    if (g_helper != nullptr) {
        if (WaitForSingleObject(g_helper, 0) != WAIT_OBJECT_0) {
            return;
        }
        CloseHandle(g_helper);
        g_helper = nullptr;
    }
    std::array<wchar_t, MAX_PATH> path{};
    if (!game_path(kCommandName, path)) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"r") != 0 || file == nullptr) {
        return;
    }
    std::array<char, 256> line{};
    const bool read = std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr;
    std::fclose(file);
    DeleteFileW(path.data());
    Command command{};
    if (!read || !parse(line.data(), command)) {
        report("ev=vr.watch command result=fail reason=parse");
        return;
    }
    g_command = command;
    reportf("ev=vr.watch start addr=0x%llX len=%u mode=%s ms=%lu", static_cast<unsigned long long>(command.address),
            command.length, command.writeOnly ? "w" : "rw", static_cast<unsigned long>(command.windowMs));
    g_helper = CreateThread(nullptr, 0, helper, nullptr, 0, nullptr);
    if (g_helper == nullptr) {
        report("ev=vr.watch command result=fail reason=thread");
    }
}

} // namespace sunrise::client::hooks::vr::watch
