#pragma once

/**
 * Where the module's own CPU time goes, per frame: QPC around each hook body, summed per slot and
 * reported by the GPU module's stats line (`ev=vr.profile`). Off unless `profile on` is written to
 * SVR_Gpu.txt -- one relaxed load per call when off.
 */

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace sunrise::client::hooks::vr::profile {

enum Slot : unsigned {
    draw,
    map,
    unmap,
    update,
    present,
    camera,
    weapon,
    camMode,
    camBody,
    camLedger,
    camBlock,
    camRead,
    camWrite,
    kSlots
};

inline std::atomic_bool g_on{false};
inline std::atomic<std::int64_t> g_ticks[kSlots]{};
inline std::atomic<std::uint64_t> g_calls[kSlots]{};

class Scope final {
public:
    explicit Scope(Slot slot) noexcept : m_slot(slot) {
        if (g_on.load(std::memory_order_relaxed)) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            m_start = now.QuadPart;
        }
    }
    ~Scope() {
        if (m_start != 0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            g_ticks[m_slot].fetch_add(now.QuadPart - m_start, std::memory_order_relaxed);
            g_calls[m_slot].fetch_add(1, std::memory_order_relaxed);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Slot m_slot;
    std::int64_t m_start{0};
};

} // namespace sunrise::client::hooks::vr::profile
