#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "internal.h"
#include "spawn/slice_set_sample.h"

namespace sunrise::client::hooks::bootflow {
namespace {

using core::log::kLineCapacity;

/** The type-17 lifetime block's byte 2 reader; 1 when no type-17 reference resolves. */
constexpr std::string_view kByte2SignatureText =
    "48 83 EC 28 E8 ? ? ? ? 48 85 C0 74 ? 0F B6 40 ? 48 83 C4 28 C3 B0 01 48 83 C4 28 C3 CC 4C "
    "40 53";
constexpr auto kByte2Signature =
    signature<signature_length(kByte2SignatureText)>(kByte2SignatureText);
/** The type-17 lifetime block resolver, null when no reference resolves. The 0x11 is the type. */
constexpr std::string_view kBlockSignatureText =
    "48 83 EC 38 33 D2 4C 8D 44 24 ? 8D 4A 11 E8 ? ? ? ? 84 C0 74 ? 8B 54 24 ? 83 FA FF 74 ? 8B C2 "
    "81 E2 FF 1F 00 00 C1 F8 0D";
constexpr auto kBlockSignature =
    signature<signature_length(kBlockSignatureText)>(kBlockSignatureText);
/** The lifetime state reader, -1 when no reference resolves. */
constexpr std::string_view kStateSignatureText = "48 83 EC 28 E8 ? ? ? ? 48 85 C0 74 ? 0F B6 00";
constexpr auto kStateSignature =
    signature<signature_length(kStateSignatureText)>(kStateSignatureText);
/** The group-session host test, by group index. */
constexpr std::string_view kHostSignatureText =
    "40 53 48 83 EC 20 48 63 D9 E8 ? ? ? ? 48 85 C0 74 ? 83 FB FF 74 ? 4C 69 C3 48 91 03 00 4C 03 "
    "C0 49 63 40 ? 48 69 C8 A0 C8 01 00 49 8D 40 ? 48 03 C1 74 ? 8B 80 ? ? ? ? 83 C0 FA";
constexpr auto kHostSignature = signature<signature_length(kHostSignatureText)>(kHostSignatureText);

/** The msg-5 roster apply. It skips its whole diff while no bubble is current. */
constexpr std::string_view kApplySignatureText = "40 55 56 48 83 EC 48 48 8B EA";
constexpr auto kApplySignature =
    signature<signature_length(kApplySignatureText)>(kApplySignatureText);

/** Lines allowed per run; a line goes out only when a sampled value changes. */
constexpr unsigned kMaxReports = 64;
/** Roster applies logged per run; every call is one line. */
constexpr unsigned kMaxApplyReports = 48;
/** The fireteam is group 0. */
constexpr int kFireteamGroup = 0;

using Byte2 = std::uint8_t(__fastcall*)();
using Block = std::byte*(__fastcall*)();
using State = std::int32_t(__fastcall*)();
using HostTest = bool(__fastcall*)(int);
using ApplyDelta = void*(__fastcall*)(void*, void*, void*, void*);

/** One sample of the step-38 joinability gate's inputs. */
struct Sample {
    std::uint8_t byte2{};
    std::uint8_t hasBlock{};
    std::int32_t state{};
    std::uint8_t host{};

    bool operator==(const Sample&) const noexcept = default;
};

hooking::detour::Handle g_handle{};
hooking::detour::Handle g_applyHandle{};
std::atomic<Byte2> g_original{nullptr};
std::atomic<ApplyDelta> g_applyOriginal{nullptr};
std::atomic<unsigned> g_applyReported{0};
std::atomic<Block> g_block{nullptr};
std::atomic<State> g_state{nullptr};
std::atomic<HostTest> g_host{nullptr};
std::atomic<unsigned> g_reported{0};
Sample g_last{};
std::atomic_bool g_hasLast{false};

/** Writes one line when the sample changed, within the per-run budget. */
void report(const Sample& value) noexcept {
    if (g_hasLast.load(std::memory_order_acquire) && value == g_last) {
        return;
    }
    g_last = value;
    g_hasLast.store(true, std::memory_order_release);
    if (g_reported.fetch_add(1, std::memory_order_relaxed) >= kMaxReports) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=probe stage=lifetime byte2=%u block=%u state=%d host=%u",
                                      static_cast<unsigned>(value.byte2),
                                      static_cast<unsigned>(value.hasBlock),
                                      value.state,
                                      static_cast<unsigned>(value.host));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Runs the native reader, then samples the gate's other inputs. Reads only.
 * @return The native byte, or 1 when the trampoline is gone.
 */
__declspec(noinline) std::uint8_t __fastcall byte2() noexcept {
    const Byte2 original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return 1;
    }
    Sample value{};
    value.byte2 = original();
    const Block block = g_block.load(std::memory_order_acquire);
    value.hasBlock = block != nullptr && block() != nullptr ? 1U : 0U;
    const State state = g_state.load(std::memory_order_acquire);
    value.state = state != nullptr ? state() : -2;
    const HostTest host = g_host.load(std::memory_order_acquire);
    value.host = host != nullptr && host(kFireteamGroup) ? 1U : 0U;
    report(value);
    return value.byte2;
}

/**
 * Runs the roster apply, then logs the slice set that was current and whether a type-17 block
 * resolves afterwards. Reads only.
 * @return The apply's own result, or null when the trampoline is gone.
 */
__declspec(noinline) void* __fastcall apply_delta(void* roster,
                                                  void* wire,
                                                  void* a3,
                                                  void* a4) noexcept {
    const ApplyDelta original = g_applyOriginal.load(std::memory_order_acquire);
    if (original == nullptr) {
        return nullptr;
    }
    const std::int32_t sliceSet = spawn::sample_current_slice_set();
    void* const result = original(roster, wire, a3, a4);
    if (g_applyReported.fetch_add(1, std::memory_order_relaxed) < kMaxApplyReports) {
        const Block block = g_block.load(std::memory_order_acquire);
        const State state = g_state.load(std::memory_order_acquire);
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=probe stage=roster_apply slice=%d block=%u state=%d",
                                          sliceSet,
                                          block != nullptr && block() != nullptr ? 1U : 0U,
                                          state != nullptr ? state() : -2);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return result;
}

/** @param reason Key naming the target that failed. */
void report_failure(const char* reason) noexcept {
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=probe stage=lifetime result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/**
 * Attaches the lifetime gate probe. It reads only and forwards every call.
 * @return True when the reader and its three helpers were found and the detour attached.
 */
bool install_lifetime_gate_probe() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kByte2Signature, "lifetime_block_byte2");
    if (target == nullptr) {
        report_failure("byte2");
        return false;
    }
    std::byte* const block = scan_main_image_unique(kBlockSignature, "lifetime_block");
    std::byte* const state = scan_main_image_unique(kStateSignature, "lifetime_state");
    std::byte* const host = scan_main_image_unique(kHostSignature, "group_session_is_host");
    if (block == nullptr || state == nullptr || host == nullptr) {
        report_failure("helpers");
        return false;
    }
    g_block.store(reinterpret_cast<Block>(block), std::memory_order_release);
    g_state.store(reinterpret_cast<State>(state), std::memory_order_release);
    g_host.store(reinterpret_cast<HostTest>(host), std::memory_order_release);
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&byte2)};
    if (!hooking::detour::install(spec, g_handle)) {
        report_failure("attach");
        return false;
    }
    g_original.store(reinterpret_cast<Byte2>(g_handle.original), std::memory_order_release);
    std::byte* const apply = scan_main_image_unique(kApplySignature, "client_roster_apply_delta");
    const hooking::detour::Spec applySpec{apply, reinterpret_cast<void*>(&apply_delta)};
    if (apply == nullptr || !hooking::detour::install(applySpec, g_applyHandle)) {
        report_failure("apply");
    } else {
        g_applyOriginal.store(reinterpret_cast<ApplyDelta>(g_applyHandle.original),
                              std::memory_order_release);
    }
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=probe stage=lifetime result=ok");
    return true;
}

/** Detaches the probe. */
void uninstall_lifetime_gate_probe() noexcept {
    if (g_applyHandle.attached) {
        (void)hooking::detour::uninstall(g_applyHandle);
    }
    g_applyOriginal.store(nullptr, std::memory_order_release);
    g_applyReported.store(0, std::memory_order_release);
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_hasLast.store(false, std::memory_order_release);
    g_reported.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
