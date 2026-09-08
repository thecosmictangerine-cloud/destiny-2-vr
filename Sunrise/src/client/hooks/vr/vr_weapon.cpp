/**
 * F3 probe: who gets which camera pose.
 *
 * Found with one in-process access watch (RESEARCH.md, "VMProtect"): two code paths read the pose
 * out of the player camera block after Sunrise's hook has written it.
 *
 *   destiny2.exe+0x12D22C0  get_camera_pose(Vec3* pos, Vec3* fwd, Vec3* up)
 *       Copies +0x594/+0x5BC/+0x5C8 of the local player's block into the caller's vectors. Called
 *       four times a frame from six threads: the render side.
 *   destiny2.exe+0x12D50F0  camera_pose_ptr(int playerIndex) -> float*
 *       Returns block(playerIndex)+0x594; the caller reads fwd at +0x28 and up at +0x34 directly
 *       and builds a basis from them, once a frame on the camera thread.
 *
 * Both are detoured. Each call's return address is counted, and rules keyed on that address decide
 * whether the caller receives the pose as written (head), or the engine's own (body). The pointer
 * path hands out a private copy of the pose region with the orientation swapped.
 *
 * The RVAs are pinned like every other offset in this module: the target build is a fixed depot
 * manifest, and VMProtect decrypts the same image every launch.
 */

#include "vr_weapon.h"

#include <Windows.h>

#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"

namespace sunrise::client::hooks::vr::weapon {
namespace {

constexpr std::uintptr_t kGetterRva = 0x12D22C0;
constexpr std::uintptr_t kPosePtrRva = 0x12D50F0;
constexpr std::array<std::uint8_t, 18> kGetterPrologue{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
                                                       0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF8};
constexpr std::array<std::uint8_t, 9> kPosePtrPrologue{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xD9};

constexpr std::uint32_t kPollPeriod = 30;
constexpr std::uint32_t kReportPeriod = 180;
constexpr wchar_t kCommandName[] = L"SVR_Weapon.txt";
constexpr std::size_t kMaxCallers = 24;
constexpr std::size_t kMaxRuleRvas = 8;
/** Bytes of the pose region handed out through the pointer path; callers index it by small offsets. */
constexpr std::size_t kFakeRegionBytes = 0x100;
constexpr std::size_t kOffsetForward = 0x28;
constexpr std::size_t kOffsetUp = 0x34;

using Getter = void(__fastcall*)(Vector*, Vector*, Vector*);
using PosePtr = float*(__fastcall*)(int);

enum class Source : std::uint8_t { none, body, head };

struct Rule final {
    bool all{false};
    std::array<std::uintptr_t, kMaxRuleRvas> rvas{};
    std::size_t count{0};
    Source source{Source::none};
};

struct Caller final {
    std::atomic<std::uintptr_t> rva{0};
    std::atomic<std::uint64_t> count{0};
};

struct Path final {
    std::array<Caller, kMaxCallers> callers{};
    std::atomic<std::uint64_t> overflow{0};
    /** Rules are replaced whole from the camera thread; readers copy the pointer first. */
    std::atomic<const Rule*> rule{nullptr};
    std::array<Rule, 2> ruleSlots{};
    std::size_t nextSlot{0};
};

Path g_getter{};
Path g_ptr{};
std::array<hooking::detour::Handle, 2> g_handles{};
std::atomic_bool g_installed{false};
std::atomic_bool g_installTried{false};
std::atomic_bool g_blockOrientation{true};
std::uintptr_t g_base = 0;
std::uint32_t g_frame = 0;
/** Fake pose regions for the pointer path, one per player index the accessor is asked about. */
alignas(16) std::array<std::array<std::uint8_t, kFakeRegionBytes>, 4> g_fake{};

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

void count_caller(Path& path, std::uintptr_t rva) noexcept {
    for (Caller& caller : path.callers) {
        std::uintptr_t current = caller.rva.load(std::memory_order_acquire);
        if (current == 0) {
            std::uintptr_t expected = 0;
            current = caller.rva.compare_exchange_strong(expected, rva, std::memory_order_acq_rel) ? rva : expected;
        }
        if (current == rva) {
            caller.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    path.overflow.fetch_add(1, std::memory_order_relaxed);
}

Source decide(const Path& path, std::uintptr_t rva) noexcept {
    const Rule* rule = path.rule.load(std::memory_order_acquire);
    if (rule == nullptr || rule->source == Source::none) {
        return Source::none;
    }
    if (rule->all) {
        return rule->source;
    }
    for (std::size_t i = 0; i < rule->count; ++i) {
        if (rule->rvas[i] == rva) {
            return rule->source;
        }
    }
    return Source::none;
}

/** @return The pose a rule asks for, and whether it is usable this frame. */
bool pose_for(Source source, Pose& out) noexcept {
    switch (source) {
    case Source::body: out = engine_pose(); break;
    case Source::head: out = head_pose(); break;
    default: return false;
    }
    const float n = out.forward[0] * out.forward[0] + out.forward[1] * out.forward[1] + out.forward[2] * out.forward[2];
    return n > 0.5F && n < 1.5F;
}

void __fastcall getter_replacement(Vector* pos, Vector* fwd, Vector* up) noexcept {
    const auto rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base;
    const auto original = reinterpret_cast<Getter>(g_handles[0].original);
    if (original != nullptr) {
        original(pos, fwd, up);
    }
    count_caller(g_getter, rva);
    Pose pose{};
    if (!pose_for(decide(g_getter, rva), pose)) {
        return;
    }
    if (pos != nullptr) {
        *pos = pose.position;
    }
    if (fwd != nullptr) {
        *fwd = pose.forward;
    }
    if (up != nullptr) {
        *up = pose.up;
    }
}

float* __fastcall pose_ptr_replacement(int playerIndex) noexcept {
    const auto rva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base;
    const auto original = reinterpret_cast<PosePtr>(g_handles[1].original);
    float* const real = original != nullptr ? original(playerIndex) : nullptr;
    count_caller(g_ptr, rva);
    Pose pose{};
    if (real == nullptr || !pose_for(decide(g_ptr, rva), pose)) {
        return real;
    }
    auto& fake = g_fake[static_cast<std::size_t>(playerIndex) & 3U];
    std::memcpy(fake.data(), real, fake.size());
    std::memcpy(fake.data(), pose.position.data(), sizeof(Vector));
    std::memcpy(fake.data() + kOffsetForward, pose.forward.data(), sizeof(Vector));
    std::memcpy(fake.data() + kOffsetUp, pose.up.data(), sizeof(Vector));
    return reinterpret_cast<float*>(fake.data());
}

template <std::size_t N>
bool prologue_matches(std::uintptr_t address, const std::array<std::uint8_t, N>& expected) noexcept {
    std::array<std::uint8_t, N> actual{};
    SIZE_T read = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), actual.data(), N, &read) == FALSE
        || read != N) {
        return false;
    }
    return actual == expected;
}

void install() noexcept {
    if (g_installTried.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_base == 0) {
        report("ev=vr.weapon install result=fail reason=no_base");
        return;
    }
    const std::uintptr_t getter = g_base + kGetterRva;
    const std::uintptr_t posePtr = g_base + kPosePtrRva;
    if (!prologue_matches(getter, kGetterPrologue) || !prologue_matches(posePtr, kPosePtrPrologue)) {
        report("ev=vr.weapon install result=fail reason=prologue_mismatch");
        return;
    }
    const std::array<hooking::detour::Spec, 2> specs{
        hooking::detour::Spec{reinterpret_cast<void*>(getter), reinterpret_cast<void*>(&getter_replacement)},
        hooking::detour::Spec{reinterpret_cast<void*>(posePtr), reinterpret_cast<void*>(&pose_ptr_replacement)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        report("ev=vr.weapon install result=fail reason=detour");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    reportf("ev=vr.weapon install result=ok getter=0x%llX ptr=0x%llX", static_cast<unsigned long long>(kGetterRva),
            static_cast<unsigned long long>(kPosePtrRva));
}

void report_callers(const char* name, Path& path) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int used = std::snprintf(line.data(), line.size(), "ev=vr.weapon callers path=%s", name);
    for (Caller& caller : path.callers) {
        const std::uintptr_t rva = caller.rva.load(std::memory_order_acquire);
        if (rva == 0 || used < 0 || static_cast<std::size_t>(used) >= line.size() - 24) {
            break;
        }
        const std::uint64_t count = caller.count.exchange(0, std::memory_order_relaxed);
        const int n = std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used), " %llX:%llu",
                                    static_cast<unsigned long long>(rva), static_cast<unsigned long long>(count));
        if (n <= 0) {
            break;
        }
        used += n;
    }
    const std::uint64_t overflow = path.overflow.exchange(0, std::memory_order_relaxed);
    if (overflow != 0 && used > 0 && static_cast<std::size_t>(used) < line.size() - 24) {
        used += std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used), " overflow=%llu",
                              static_cast<unsigned long long>(overflow));
    }
    if (used > 0) {
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(used)});
    }
}

/** Directory of the executable plus the given file name. */
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

/** Parses `all|none|<rva>[,<rva>...]` and `body|head` into a rule and publishes it on the path. */
void apply_rule(Path& path, const char* targets, const char* source, const char* name) noexcept {
    Rule& rule = path.ruleSlots[path.nextSlot];
    path.nextSlot = (path.nextSlot + 1) % path.ruleSlots.size();
    rule = Rule{};
    if (std::strcmp(source, "body") == 0) {
        rule.source = Source::body;
    } else if (std::strcmp(source, "head") == 0) {
        rule.source = Source::head;
    } else {
        rule.source = Source::none;
    }
    if (std::strcmp(targets, "all") == 0) {
        rule.all = true;
    } else if (std::strcmp(targets, "none") != 0) {
        const char* cursor = targets;
        while (*cursor != '\0' && rule.count < rule.rvas.size()) {
            char* end = nullptr;
            const unsigned long long value = std::strtoull(cursor, &end, 16);
            if (end == cursor) {
                break;
            }
            rule.rvas[rule.count++] = static_cast<std::uintptr_t>(value);
            cursor = (*end == ',') ? end + 1 : end;
        }
    }
    if (rule.source == Source::none) {
        rule.all = false;
        rule.count = 0;
    }
    path.rule.store(&rule, std::memory_order_release);
    reportf("ev=vr.weapon rule path=%s targets=%s source=%s rvas=%zu all=%d", name, targets, source, rule.count,
            rule.all ? 1 : 0);
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
    std::array<char, 256> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        char verb[16] = {};
        char a[128] = {};
        char b[16] = {};
        const int n = sscanf_s(line.data(), "%15s %127s %15s", verb, static_cast<unsigned>(sizeof verb), a,
                               static_cast<unsigned>(sizeof a), b, static_cast<unsigned>(sizeof b));
        if (n < 1) {
            continue;
        }
        if (std::strcmp(verb, "getter") == 0 && n >= 3) {
            apply_rule(g_getter, a, b, "getter");
        } else if (std::strcmp(verb, "ptr") == 0 && n >= 3) {
            apply_rule(g_ptr, a, b, "ptr");
        } else if (std::strcmp(verb, "block") == 0 && n >= 2) {
            const bool on = std::strcmp(a, "on") == 0;
            g_blockOrientation.store(on, std::memory_order_release);
            reportf("ev=vr.weapon block orientation=%s", on ? "on" : "off");
        } else if (std::strcmp(verb, "install") == 0) {
            install();
        } else if (std::strcmp(verb, "report") == 0) {
            report_callers("getter", g_getter);
            report_callers("ptr", g_ptr);
        } else {
            reportf("ev=vr.weapon command result=fail verb=%s", verb);
        }
    }
    std::fclose(file);
    DeleteFileW(path.data());
}

} // namespace

void tick() noexcept {
    ++g_frame;
    // The detours are opt-in: nothing touches the engine until an `install` command arrives, so
    // a session with the file absent is byte-for-byte the pre-F3 behaviour (loading included).
    if (g_frame % kPollPeriod == 0) {
        poll_commands();
    }
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    if (g_frame % kReportPeriod == 0) {
        report_callers("getter", g_getter);
        report_callers("ptr", g_ptr);
    }
}

bool block_orientation_enabled() noexcept {
    return g_blockOrientation.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::vr::weapon
