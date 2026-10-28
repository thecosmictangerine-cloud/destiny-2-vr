#include "vr_watchdog.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../../../core/logging/log.h"
#include "vr_debug.h"

namespace sunrise::client::hooks::vr::watchdog {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

std::atomic<std::uint64_t> g_presents{0};
std::atomic_bool g_started{false};
MiniDumpWriteDumpFn g_writeDump = nullptr;

/** Presents seen before a silence counts: a boot still loading is not a hang. */
constexpr std::uint64_t kArmedAfterPresents = 600;
constexpr ULONGLONG kSilenceMs = 8000;

void report(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, {text, std::strlen(text)});
}

bool dump_path(std::array<wchar_t, MAX_PATH>& out) noexcept {
    const DWORD length = GetModuleFileNameW(nullptr, out.data(), static_cast<DWORD>(out.size()));
    wchar_t* const slash = length != 0 && length < out.size() ? std::wcsrchr(out.data(), L'\\') : nullptr;
    return slash != nullptr
           && wcscpy_s(slash + 1, out.size() - static_cast<std::size_t>(slash + 1 - out.data()), L"SVR_Hang.dmp") == 0;
}

void write_dump(ULONGLONG silentMs) noexcept {
    std::array<char, 160> line{};
    std::snprintf(line.data(), line.size(), "ev=vr.watchdog hang=detected silent_ms=%llu presents=%llu action=dump",
                  static_cast<unsigned long long>(silentMs), static_cast<unsigned long long>(g_presents.load()));
    report(line.data());
    std::array<wchar_t, MAX_PATH> path{};
    if (g_writeDump == nullptr || !dump_path(path)) {
        report("ev=vr.watchdog dump=fail reason=no_dbghelp");
        return;
    }
    const HANDLE file = CreateFileW(path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("ev=vr.watchdog dump=fail reason=file");
        return;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
    CloseHandle(file);
    report(ok ? "ev=vr.watchdog dump=ok file=SVR_Hang.dmp" : "ev=vr.watchdog dump=fail reason=write");
}

DWORD WINAPI watch(void*) noexcept {
    std::uint64_t last = g_presents.load(std::memory_order_relaxed);
    ULONGLONG lastChange = GetTickCount64();
    bool dumped = false;
    for (;;) {
        Sleep(500);
        const std::uint64_t now = g_presents.load(std::memory_order_relaxed);
        const ULONGLONG tick = GetTickCount64();
        if (now != last) {
            last = now;
            lastChange = tick;
            continue;
        }
        if (!dumped && now >= kArmedAfterPresents && tick - lastChange >= kSilenceMs) {
            // Once a session: the file is evidence, never overwritten by a second, later silence.
            dumped = true;
            write_dump(tick - lastChange);
        }
    }
}

} // namespace

void present() noexcept {
    const std::uint64_t count = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    // `hang test` in SVR_Debug.txt: the present thread stops for 10 s once, to prove the dump.
    if (count == 4 * kArmedAfterPresents && debug::has("hang test")) {
        report("ev=vr.watchdog hang_test=sleeping ms=10000");
        Sleep(10000);
    }
    if (g_started.load(std::memory_order_relaxed) || g_presents.load(std::memory_order_relaxed) < kArmedAfterPresents) {
        return;
    }
    if (g_started.exchange(true)) {
        return;
    }
    // Loaded now, from a healthy game, so a hang never waits on the loader.
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    g_writeDump = dbghelp != nullptr ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    const HANDLE thread = CreateThread(nullptr, 0, &watch, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
    report(g_writeDump != nullptr ? "ev=vr.watchdog start dbghelp=1" : "ev=vr.watchdog start dbghelp=0");
}

} // namespace sunrise::client::hooks::vr::watchdog
