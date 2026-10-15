/**
 * Live function tracer. See vr_probe.h for what it is for; the hooking itself is vr_prehook.
 */

#include "vr_probe.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "../../../core/logging/log.h"
#include "vr_prehook.h"

namespace sunrise::client::hooks::vr::probe {
namespace {

constexpr std::size_t kMaxHooks = 160;
constexpr std::size_t kMaxCallers = 12;
constexpr std::size_t kChainDepth = 14;
constexpr std::uint32_t kPollPeriod = 15;
constexpr std::uint32_t kReportPeriod = 180;
constexpr wchar_t kCommandName[] = L"SVR_Probe.txt";
/** Aim matches logged per hook until `reset`: enough to see the pattern, not enough to flood. */
constexpr int kHitsPerHook = 12;
/** Calls per hook per camera frame that get the (comparatively expensive) aim scan. */
constexpr int kScanBudgetPerFrame = 400;
/** The fire window stays open this long after the button is released: a shot can lag the press. */
constexpr ULONGLONG kFireTailMs = 400;

struct Hook final {
    std::uintptr_t rva{};
    std::array<char, 24> name{};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> periodCalls{0};
    std::array<std::atomic<std::uintptr_t>, kMaxCallers> callers{};
    std::atomic<int> argsLeft{0};
    std::atomic<int> hitsLogged{0};
    std::atomic<int> scanBudget{kScanBudgetPerFrame};
    std::atomic<std::uint64_t> scanSkipped{0};
};

std::array<Hook, kMaxHooks> g_hooks{};
std::atomic<std::size_t> g_hookCount{0};
std::uintptr_t g_base = 0;
std::uintptr_t g_imageEnd = 0;
std::uint32_t g_frame = 0;

std::atomic_bool g_scan{true};
std::atomic_bool g_fireWindow{false};
ULONGLONG g_fireUntil = 0;
std::array<std::atomic<float>, 3> g_eye{};
std::array<std::atomic<float>, 3> g_forward{};

thread_local bool t_inRecord = false;

/** Lines produced on arbitrary game threads, flushed to the log on the camera thread. */
using Line = std::array<char, 960>;
SRWLOCK g_queueLock = SRWLOCK_INIT;
std::array<Line, 256> g_queue{};
std::size_t g_queueCount = 0;
std::atomic<std::uint64_t> g_dropped{0};

template <typename... Args> void reportf(const char* format, Args... args) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(), line.size(), format, args...);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size() ? static_cast<std::size_t>(written)
                                                                            : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::warn, {line.data(), length});
    }
}

void report(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, text);
}

void enqueue(const Line& line) noexcept {
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_queueCount < g_queue.size()) {
        g_queue[g_queueCount++] = line;
    } else {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_queueLock);
}

/** Appends to a line, never past its end. */
struct Writer final {
    Line line{};
    std::size_t used{0};
    template <typename... Args> void add(const char* format, Args... args) noexcept {
        if (used >= line.size() - 1) {
            return;
        }
        const int n = std::snprintf(line.data() + used, line.size() - used, format, args...);
        if (n > 0) {
            used = std::min(line.size() - 1, used + static_cast<std::size_t>(n));
        }
    }
};

bool read_mem(std::uintptr_t address, void* out, std::size_t size) noexcept {
    return prehook::read(address, out, size);
}

bool plausible_pointer(std::uint64_t v) noexcept {
    return v > 0x10000ULL && v < 0x7FFFFFFFFFFFULL && (v & 3U) == 0;
}

/** Prints an address as `+RVA` inside the executable, `-` outside it. */
void add_address(Writer& w, std::uintptr_t a) noexcept {
    if (a >= g_base && a < g_imageEnd) {
        w.add("%llX,", static_cast<unsigned long long>(a - g_base));
    } else {
        w.add("-,");
    }
}

void log_chain(Hook& hook, const std::uint8_t* frame, const char* why) noexcept {
    std::array<std::uintptr_t, kChainDepth> chain{};
    const std::size_t n = prehook::walk(frame, chain.data(), chain.size());
    Writer w;
    w.add("ev=vr.trace %s hook=%s rva=%llX thread=%lu chain=", why, hook.name.data(),
          static_cast<unsigned long long>(hook.rva), GetCurrentThreadId());
    for (std::size_t i = 0; i < n; ++i) {
        add_address(w, chain[i]);
    }
    enqueue(w.line);
}

struct Regs final {
    std::array<std::uint64_t, 4> gp{};  // rcx, rdx, r8, r9
    std::array<std::array<float, 4>, 4> xmm{};
    std::uint64_t ret{};
};

Regs regs_of(const std::uint8_t* frame) noexcept {
    Regs r{};
    std::memcpy(&r.gp[0], frame + 0x80, 8);
    std::memcpy(&r.gp[1], frame + 0x78, 8);
    std::memcpy(&r.gp[2], frame + 0x70, 8);
    std::memcpy(&r.gp[3], frame + 0x68, 8);
    for (std::size_t i = 0; i < 4; ++i) {
        std::memcpy(r.xmm[i].data(), frame + 0x20 + 0x10 * i, 16);
    }
    std::memcpy(&r.ret, frame + 0x88, 8);
    return r;
}

constexpr const char* kRegName[4] = {"rcx", "rdx", "r8", "r9"};

void log_args(Hook& hook, const Regs& r) noexcept {
    Writer w;
    w.add("ev=vr.trace args hook=%s from=", hook.name.data());
    add_address(w, r.ret);
    for (std::size_t i = 0; i < 4; ++i) {
        w.add(" %s=%llX", kRegName[i], static_cast<unsigned long long>(r.gp[i]));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        w.add(" xmm%zu=%.4f,%.4f,%.4f,%.4f", i, r.xmm[i][0], r.xmm[i][1], r.xmm[i][2], r.xmm[i][3]);
    }
    enqueue(w.line);
    // What each pointer argument points at, as floats: vectors and transforms read at a glance.
    for (std::size_t i = 0; i < 4; ++i) {
        if (!plausible_pointer(r.gp[i])) {
            continue;
        }
        std::array<float, 24> f{};
        if (!read_mem(r.gp[i], f.data(), sizeof f)) {
            continue;
        }
        Writer m;
        m.add("ev=vr.trace mem hook=%s %s:", hook.name.data(), kRegName[i]);
        for (const float v : f) {
            m.add(" %.4g", v);
        }
        enqueue(m.line);
    }
}

/** What a float triple at some place in the arguments looks like, compared with the aim. */
enum class Kind { none, forward, nearForward, eye };

Kind classify(const float* t, const std::array<float, 3>& eye, const std::array<float, 3>& fwd) noexcept {
    if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) {
        return Kind::none;
    }
    if (std::fabs(t[0] - fwd[0]) < 2e-3F && std::fabs(t[1] - fwd[1]) < 2e-3F && std::fabs(t[2] - fwd[2]) < 2e-3F) {
        return Kind::forward;
    }
    // A unit vector within a few degrees of the aim: the same aim after spread or recoil was added.
    const float len2 = t[0] * t[0] + t[1] * t[1] + t[2] * t[2];
    const float dot = t[0] * fwd[0] + t[1] * fwd[1] + t[2] * fwd[2];
    if (std::fabs(len2 - 1.0F) < 2e-3F && dot > 0.996F) {
        return Kind::nearForward;
    }
    // The eye, or anything within reach of it -- a muzzle, the character's own origin.
    if (std::fabs(t[0] - eye[0]) < 1.5F && std::fabs(t[1] - eye[1]) < 1.5F && std::fabs(t[2] - eye[2]) < 2.5F
        && (std::fabs(eye[0]) + std::fabs(eye[1])) > 1.0F) {
        return Kind::eye;
    }
    return Kind::none;
}

/** Scans `bytes` read from `where` for aim-like triples, appending what it finds. @return Forward hits. */
int scan_block(Writer& w, const char* label, std::size_t level, const std::uint8_t* bytes, std::size_t size,
               const std::array<float, 3>& eye, const std::array<float, 3>& fwd) noexcept {
    int forwards = 0;
    for (std::size_t off = 0; off + 12 <= size; off += 4) {
        float t[3];
        std::memcpy(t, bytes + off, sizeof t);
        const Kind k = classify(t, eye, fwd);
        if (k == Kind::none) {
            continue;
        }
        // A ray given as two points, Havok's way (`m_from`, `m_to`): a point at the eye followed,
        // a vector or two later, by a far point along the aim.
        if (k == Kind::eye) {
            for (const std::size_t gap : {std::size_t{12}, std::size_t{16}, std::size_t{32}}) {
                if (off + gap + 12 > size) {
                    continue;
                }
                float u[3];
                std::memcpy(u, bytes + off + gap, sizeof u);
                const float d[3] = {u[0] - t[0], u[1] - t[1], u[2] - t[2]};
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (std::isfinite(len) && len > 1.0F
                    && (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / len > 0.996F) {
                    ++forwards;
                    w.add(" %s%s+%zX:RAY(from %.3f,%.3f,%.3f to +%zX len %.1f dir %.4f,%.4f,%.4f)", label,
                          level == 2 ? "*" : "", off, t[0], t[1], t[2], gap, len, d[0] / len, d[1] / len, d[2] / len);
                }
            }
        }
        if (k == Kind::forward || k == Kind::nearForward) {
            ++forwards;
        }
        w.add(" %s%s+%zX:%s(%.4f,%.4f,%.4f)", label, level == 2 ? "*" : "", off,
              k == Kind::forward ? "FWD" : k == Kind::nearForward ? "FWD~" : "EYE",
              t[0], t[1], t[2]);
    }
    return forwards;
}

/**
 * Looks for the character's aim in this call's arguments: in xmm0-3 by value, and behind each
 * pointer register one and two levels deep. Only an aim FORWARD makes it a hit worth logging -- a
 * point near the eye alone is everywhere (the character, its weapon, its sounds).
 */
void scan_aim(Hook& hook, const std::uint8_t* frame, const Regs& r) noexcept {
    const std::array<float, 3> eye{g_eye[0].load(std::memory_order_relaxed), g_eye[1].load(std::memory_order_relaxed),
                                   g_eye[2].load(std::memory_order_relaxed)};
    const std::array<float, 3> fwd{g_forward[0].load(std::memory_order_relaxed),
                                   g_forward[1].load(std::memory_order_relaxed),
                                   g_forward[2].load(std::memory_order_relaxed)};
    Writer w;
    w.add("ev=vr.trace aim hook=%s from=", hook.name.data());
    add_address(w, r.ret);
    int forwards = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        forwards += scan_block(w, i == 0 ? "xmm0" : i == 1 ? "xmm1" : i == 2 ? "xmm2" : "xmm3", 1,
                               reinterpret_cast<const std::uint8_t*>(r.xmm[i].data()), 16, eye, fwd);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (!plausible_pointer(r.gp[i])) {
            continue;
        }
        std::array<std::uint8_t, 192> level1{};
        if (!read_mem(r.gp[i], level1.data(), level1.size())) {
            continue;
        }
        forwards += scan_block(w, kRegName[i], 1, level1.data(), level1.size(), eye, fwd);
        int followed = 0;
        for (std::size_t off = 0; off + 8 <= 96 && followed < 8; off += 8) {
            std::uint64_t p = 0;
            std::memcpy(&p, level1.data() + off, 8);
            if (!plausible_pointer(p) || (p >= g_base && p < g_imageEnd)) {
                continue;
            }
            std::array<std::uint8_t, 96> level2{};
            if (!read_mem(p, level2.data(), level2.size())) {
                continue;
            }
            ++followed;
            char label[24];
            std::snprintf(label, sizeof label, "%s+%zX", kRegName[i], off);
            forwards += scan_block(w, label, 2, level2.data(), level2.size(), eye, fwd);
        }
    }
    if (forwards > 0 && hook.hitsLogged.fetch_add(1, std::memory_order_relaxed) < kHitsPerHook) {
        w.add(" ref_fwd=%.3f,%.3f,%.3f", fwd[0], fwd[1], fwd[2]);
        enqueue(w.line);
        log_chain(hook, frame, "aimchain");
    }
}

void record(std::uint64_t id, std::uint8_t* frame) noexcept {
    if (t_inRecord || id >= kMaxHooks) {
        return;
    }
    t_inRecord = true;
    Hook& hook = g_hooks[id];
    hook.calls.fetch_add(1, std::memory_order_relaxed);
    hook.periodCalls.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t ret = 0;
    std::memcpy(&ret, frame + 0x88, sizeof ret);
    // First sighting of a caller: claim a slot and log its chain once.
    for (auto& slot : hook.callers) {
        std::uintptr_t current = slot.load(std::memory_order_acquire);
        if (current == ret) {
            break;
        }
        if (current == 0) {
            std::uintptr_t expected = 0;
            if (slot.compare_exchange_strong(expected, ret, std::memory_order_acq_rel)) {
                log_chain(hook, frame, "caller");
                break;
            }
            if (expected == ret) {
                break;
            }
        }
    }
    const bool wantArgs = hook.argsLeft.load(std::memory_order_relaxed) > 0
                          && hook.argsLeft.fetch_sub(1, std::memory_order_relaxed) > 0;
    const bool wantScan = g_scan.load(std::memory_order_relaxed) && g_fireWindow.load(std::memory_order_relaxed);
    if (wantArgs || wantScan) {
        const Regs r = regs_of(frame);
        if (wantArgs) {
            log_args(hook, r);
        }
        if (wantScan) {
            if (hook.scanBudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
                scan_aim(hook, frame, r);
            } else {
                hook.scanSkipped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    t_inRecord = false;
}


bool install_hook(std::uintptr_t rva, const char* name) noexcept {
    const std::size_t count = g_hookCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (g_hooks[i].rva == rva) {
            reportf("ev=vr.trace hook rva=%llX result=already", static_cast<unsigned long long>(rva));
            return true;
        }
    }
    if (count >= kMaxHooks) {
        reportf("ev=vr.trace hook rva=%llX result=fail reason=full", static_cast<unsigned long long>(rva));
        return false;
    }
    const std::uintptr_t target = g_base + rva;
    Hook& hook = g_hooks[count];
    hook.rva = rva;
    std::snprintf(hook.name.data(), hook.name.size(), "%s", name != nullptr && name[0] != '\0' ? name : "");
    if (hook.name[0] == '\0') {
        std::snprintf(hook.name.data(), hook.name.size(), "%llX", static_cast<unsigned long long>(rva));
    }
    // Published before the hook can fire, so record() never sees an index past the count; a failed
    // attach leaves an entry that simply never gets called.
    g_hookCount.store(count + 1, std::memory_order_release);
    const char* failure = "";
    if (!prehook::attach(target, &record, count, failure)) {
        reportf("ev=vr.trace hook rva=%llX result=fail reason=%s", static_cast<unsigned long long>(rva), failure);
        return false;
    }
    reportf("ev=vr.trace hook rva=%llX name=%s result=ok", static_cast<unsigned long long>(rva), hook.name.data());
    return true;
}

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
    return wcscpy_s(slash + 1, out.size() - dirLength, name) == 0;
}

void report_counts() noexcept {
    Writer w;
    w.add("ev=vr.trace counts");
    const std::size_t count = g_hookCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t n = g_hooks[i].periodCalls.exchange(0, std::memory_order_relaxed);
        const std::uint64_t skipped = g_hooks[i].scanSkipped.exchange(0, std::memory_order_relaxed);
        if (n != 0) {
            w.add(" %s:%llu", g_hooks[i].name.data(), static_cast<unsigned long long>(n));
            if (skipped != 0) {
                w.add("(skip%llu)", static_cast<unsigned long long>(skipped));
            }
        }
        if (w.used > w.line.size() - 40) {
            enqueue(w.line);
            w = Writer{};
            w.add("ev=vr.trace counts+");
        }
    }
    enqueue(w.line);
}

void poll_commands() noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    if (!game_path(kCommandName, path)) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.data(), L"r") != 0 || file == nullptr) {
        return;
    }
    std::array<char, 4096> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        char verb[16] = {};
        char a[3000] = {};
        char b[32] = {};
        const int n = sscanf_s(line.data(), "%15s %2999s %31s", verb, static_cast<unsigned>(sizeof verb), a,
                               static_cast<unsigned>(sizeof a), b, static_cast<unsigned>(sizeof b));
        if (n < 1) {
            continue;
        }
        if (std::strcmp(verb, "hook") == 0 && n >= 2) {
            const char* cursor = a;
            int index = 0;
            while (*cursor != '\0') {
                char* end = nullptr;
                const unsigned long long value = std::strtoull(cursor, &end, 16);
                if (end == cursor) {
                    break;
                }
                char name[24] = {};
                if (n >= 3) {
                    std::snprintf(name, sizeof name, index == 0 ? "%s" : "%s%d", b, index);
                }
                install_hook(static_cast<std::uintptr_t>(value), name);
                ++index;
                cursor = (*end == ',') ? end + 1 : end;
            }
        } else if (std::strcmp(verb, "args") == 0 && n >= 3) {
            const unsigned long long rva = std::strtoull(a, nullptr, 16);
            const int calls = std::atoi(b);
            const std::size_t count = g_hookCount.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < count; ++i) {
                if (g_hooks[i].rva == rva) {
                    g_hooks[i].argsLeft.store(calls, std::memory_order_relaxed);
                }
            }
            reportf("ev=vr.trace args rva=%llX calls=%d", rva, calls);
        } else if (std::strcmp(verb, "scan") == 0 && n >= 2) {
            g_scan.store(std::strcmp(a, "off") != 0, std::memory_order_relaxed);
            reportf("ev=vr.trace scan=%s", g_scan.load() ? "on" : "off");
        } else if (std::strcmp(verb, "reset") == 0) {
            const std::size_t count = g_hookCount.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < count; ++i) {
                g_hooks[i].hitsLogged.store(0, std::memory_order_relaxed);
                for (auto& slot : g_hooks[i].callers) {
                    slot.store(0, std::memory_order_relaxed);
                }
            }
            report("ev=vr.trace reset");
        } else if (std::strcmp(verb, "report") == 0) {
            report_counts();
        } else {
            reportf("ev=vr.trace command result=fail verb=%s", verb);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

void flush() noexcept {
    std::array<Line, 64> local{};
    std::size_t n = 0;
    AcquireSRWLockExclusive(&g_queueLock);
    n = std::min(g_queueCount, local.size());
    for (std::size_t i = 0; i < n; ++i) {
        local[i] = g_queue[i];
    }
    // Keep whatever did not fit for the next frame, in order.
    for (std::size_t i = n; i < g_queueCount; ++i) {
        g_queue[i - n] = g_queue[i];
    }
    g_queueCount -= n;
    ReleaseSRWLockExclusive(&g_queueLock);
    for (std::size_t i = 0; i < n; ++i) {
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         {local[i].data(), std::strlen(local[i].data())});
    }
    const std::uint64_t dropped = g_dropped.exchange(0, std::memory_order_relaxed);
    if (dropped != 0) {
        reportf("ev=vr.trace dropped=%llu", static_cast<unsigned long long>(dropped));
    }
}

} // namespace

void set_reference(const std::array<float, 3>& eye, const std::array<float, 3>& forward) noexcept {
    for (std::size_t i = 0; i < 3; ++i) {
        g_eye[i].store(eye[i], std::memory_order_relaxed);
        g_forward[i].store(forward[i], std::memory_order_relaxed);
    }
}

void tick() noexcept {
    ++g_frame;
    if (g_base == 0) {
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + static_cast<std::uintptr_t>(dos->e_lfanew));
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
    }
    // The fire window: the left button as Windows sees it, which is what both the harness's clicks
    // and the controller's trigger injection set.
    const ULONGLONG now = GetTickCount64();
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
        g_fireUntil = now + kFireTailMs;
    }
    g_fireWindow.store(now < g_fireUntil, std::memory_order_relaxed);
    const std::size_t count = g_hookCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        g_hooks[i].scanBudget.store(kScanBudgetPerFrame, std::memory_order_relaxed);
    }
    if (g_frame % kPollPeriod == 0) {
        poll_commands();
    }
    if (count != 0 && g_frame % kReportPeriod == 0) {
        report_counts();
    }
    flush();
}

} // namespace sunrise::client::hooks::vr::probe
