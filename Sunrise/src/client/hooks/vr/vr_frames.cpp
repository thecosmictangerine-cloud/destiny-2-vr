/**
 * The frame ledger's trace. See vr_frames.h.
 */

#include "vr_frames.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>

#include "../../../core/logging/log.h"

namespace sunrise::client::hooks::vr::frames {
namespace {

struct Event final {
    std::int64_t qpc{};
    std::uint64_t serial{};
    std::array<float, 6> v{};
    std::uint32_t tid{};
    std::uint8_t kind{};
    std::int8_t eye{};
};

constexpr std::size_t kCapacity = 32768;
std::array<Event, kCapacity> g_events{};
std::atomic<std::uint32_t> g_next{0};
std::atomic_bool g_on{false};
/** Presents left in the window; the present thread owns the countdown. */
std::atomic<std::uint32_t> g_left{0};

void reportf(const char* format, std::uint32_t a, std::uint32_t b) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(), line.size(), format, a, b);
    if (written > 0) {
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void write_file() noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return;
    }
    wchar_t* const slash = std::wcsrchr(path.data(), L'\\');
    if (slash == nullptr) {
        return;
    }
    const std::size_t dirLength = static_cast<std::size_t>(slash + 1 - path.data());
    if (wcscpy_s(slash + 1, path.size() - dirLength, L"SVR_FrameTrace.csv") != 0) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"w") != 0 || file == nullptr) {
        return;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const std::uint32_t count = (std::min)(g_next.load(std::memory_order_acquire), static_cast<std::uint32_t>(kCapacity));
    std::fprintf(file, "us,tid,kind,serial,eye,v0,v1,v2,v3,v4,v5\n");
    const std::int64_t origin = count > 0 ? g_events[0].qpc : 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Event& e = g_events[i];
        std::fprintf(file, "%.1f,%u,%u,%llu,%d,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f\n",
                     static_cast<double>(e.qpc - origin) * 1.0e6 / static_cast<double>(frequency.QuadPart),
                     e.tid, static_cast<unsigned>(e.kind), static_cast<unsigned long long>(e.serial),
                     static_cast<int>(e.eye), e.v[0], e.v[1], e.v[2], e.v[3], e.v[4], e.v[5]);
    }
    std::fclose(file);
    reportf("ev=vr.frames trace=written events=%u capacity=%u", count, static_cast<std::uint32_t>(kCapacity));
}

void start_trace_locked(std::uint32_t presents) noexcept;

/** `SVR_FrameCmd.txt`: `trace <presents>`. */
void poll_commands() noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return;
    }
    wchar_t* const slash = std::wcsrchr(path.data(), L'\\');
    if (slash == nullptr) {
        return;
    }
    const std::size_t dirLength = static_cast<std::size_t>(slash + 1 - path.data());
    if (wcscpy_s(slash + 1, path.size() - dirLength, L"SVR_FrameCmd.txt") != 0) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"r") != 0 || file == nullptr) {
        return;
    }
    std::array<char, 128> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        unsigned presents = 0;
        if (sscanf_s(line.data(), "trace %u", &presents) == 1) {
            start_trace_locked(presents);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

} // namespace

void note(Kind kind, std::uint64_t serial, const float* v6, int eye) noexcept {
    if (!g_on.load(std::memory_order_relaxed)) {
        return;
    }
    const std::uint32_t index = g_next.fetch_add(1, std::memory_order_acq_rel);
    if (index >= kCapacity) {
        return;
    }
    Event& e = g_events[index];
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    e.qpc = now.QuadPart;
    e.serial = serial;
    e.tid = GetCurrentThreadId();
    e.kind = static_cast<std::uint8_t>(kind);
    e.eye = static_cast<std::int8_t>(eye);
    if (v6 != nullptr) {
        for (std::size_t i = 0; i < 6; ++i) {
            e.v[i] = v6[i];
        }
    } else {
        e.v = {};
    }
}

bool tracing() noexcept {
    return g_on.load(std::memory_order_relaxed);
}

void start_trace(std::uint32_t presents) noexcept {
    start_trace_locked(presents);
}

namespace {
void start_trace_locked(std::uint32_t presents) noexcept {
    if (g_on.load(std::memory_order_relaxed) || presents == 0) {
        return;
    }
    g_next.store(0, std::memory_order_release);
    g_left.store(presents, std::memory_order_release);
    g_on.store(true, std::memory_order_release);
    reportf("ev=vr.frames trace=start presents=%u capacity=%u", presents, static_cast<std::uint32_t>(kCapacity));
}
} // namespace

void present_tick() noexcept {
    static std::uint32_t polls = 0;
    if ((++polls % 30) == 0) {
        poll_commands();
    }
    if (!g_on.load(std::memory_order_relaxed)) {
        return;
    }
    const std::uint32_t left = g_left.load(std::memory_order_relaxed);
    if (left > 1 && g_next.load(std::memory_order_relaxed) < kCapacity) {
        g_left.store(left - 1, std::memory_order_relaxed);
        return;
    }
    g_on.store(false, std::memory_order_release);
    // Writers that passed the switch just before it flipped finish within microseconds; the file is
    // written a moment later so none of them is caught half-written.
    Sleep(2);
    write_file();
}

} // namespace sunrise::client::hooks::vr::frames
