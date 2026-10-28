// Placed-object lifetime registry and the install pass for every world-object hook.
// g_lock guards the tables below; the exported helpers say which callers must already hold it.

#include "world_object_registry.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../executable/image.h"
#include "../../hooking/detour.h"
#include "../../memory/current_process_memory.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/registry.h"
#include "../../patterns/signature_text.h"
#include "internal.h"

namespace sunrise::client::hooks::world_objects {
namespace {

using patterns::signature;
using patterns::signature_length;

// Identity-bearing placements per registry clear that get an observed line. This is the positive
// control for the duplicate count, so it matches the registry's own capacity. A smaller budget is
// spent before the object lists load, and a missing duplicate then says nothing.
constexpr std::size_t kIdentityReportBudget = 16384;

/** Placement ledger is a power-of-two ring, so the mask indexes it without a division. */
constexpr std::size_t kRegistryCapacity = 16384;
constexpr std::size_t kRegistryMask = kRegistryCapacity - 1;
static_assert((kRegistryCapacity & kRegistryMask) == 0);

/** Placement and entity signatures share one image scan. */
constexpr std::size_t kTargetCount = 12;

/** Native entry that instantiates a placed world object. */
constexpr std::string_view kInstantiateSignatureText =
    "40 55 53 56 41 56 41 57 48 8D 6C 24 ? 48 81 EC 60 01 00 00";
/** Compiled form of that pattern; the scan requires one match. */
constexpr auto kInstantiateSignature =
    signature<signature_length(kInstantiateSignatureText)>(kInstantiateSignatureText);

/** Native entry that destroys a placed world object. */
constexpr std::string_view kDestroySignatureText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 50 8B D9 8B F9";
/** Compiled form of that pattern; the scan requires one match. */
constexpr auto kDestroySignature =
    signature<signature_length(kDestroySignatureText)>(kDestroySignatureText);

// The datum allocator every creation path shares. The instantiate hook above is one of its
// three callers, so a build reported here and not there was made by one of the other two.
constexpr std::string_view kAllocateSignatureText =
    "48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 56 48 81 EC 40 01 00 00 48 8B 05 ? ? ? ? 48 33 "
    "C4 48 89 84 24 ? ? ? ? C7 01 FF FF FF FF";
/** Compiled form of that pattern; the scan requires one match. */
constexpr auto kAllocateSignature =
    signature<signature_length(kAllocateSignatureText)>(kAllocateSignatureText);

// The logical destroy. It is the only path that releases an object's simulation entity, so a
// teardown that frees the object without passing here leaves the entity to rebuild it.
constexpr std::string_view kLogicalDestroySignatureText =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 40 48 8B 3D ? ? ? ? 8B D9";
/** Compiled form of that pattern; the scan requires one match. */
constexpr auto kLogicalDestroySignature =
    signature<signature_length(kLogicalDestroySignatureText)>(kLogicalDestroySignatureText);

/** Native entry that resolves a handle pair to its datum. */
constexpr std::string_view kResolvePairSignatureText =
    "4C 8B D1 83 FA FF 74 ? 4C 8B 0D ? ? ? ? 44 8B C2 41 C1 F8 1F 8B C2 C1 E8 0D 41 81 E0 "
    "00 3C 00 00 0F B7 C8 41 81 C8 FF 03 00 00 49 8B 01 44 23 C1 45 0F AF 41 ? 0F B7 CA "
    "81 E1 FF 1F 00 00 4D 8B 44 00";
/** Compiled form of that pattern; the scan requires one match. */
constexpr auto kResolvePairSignature =
    signature<signature_length(kResolvePairSignatureText)>(kResolvePairSignatureText);

/** Native entry that validates a handle pair, and the compiled form of its pattern. */
constexpr std::string_view kValidatePairSignatureText = "48 83 EC 08 44 8B 51";
constexpr auto kValidatePairSignature =
    signature<signature_length(kValidatePairSignatureText)>(kValidatePairSignatureText);

// The longer suffix fixes the two RIP-relative operands used below at stable byte offsets.
constexpr std::string_view kDatumLayoutSignatureText =
    "48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 55 41 56 41 57 48 83 EC 30 44 8B F1 8B F9 41 81 "
    "E6 FF 1F 00 00 48 8B CA 44 0F AF 35 ? ? ? ? 41 8B E9 41 8B D8 4C 8B FA 4C 03 35 ? ? "
    "? ?";
/** Compiled form of that pattern; the scan requires one match. */
constexpr auto kDatumLayoutSignature =
    signature<signature_length(kDatumLayoutSignatureText)>(kDatumLayoutSignatureText);

/** The seven placement signatures, in the order the target list expects them. */
constexpr std::array kPlacementSignatures{
    patterns::Pattern{"placed_object_instantiate", kInstantiateSignature},
    patterns::Pattern{"placed_object_destroy", kDestroySignature},
    patterns::Pattern{"object_datum_allocate", kAllocateSignature},
    patterns::Pattern{"object_logical_destroy", kLogicalDestroySignature},
    patterns::Pattern{"object_handle_pair", kResolvePairSignature},
    patterns::Pattern{"object_handle_validate", kValidatePairSignature},
    patterns::Pattern{"object_datum_layout", kDatumLayoutSignature},
};

enum class EntryState : std::uint8_t {
    empty,
    live,
    tombstone,
};

struct RegistryEntry final {
    Instance instance{};
    EntryState state{EntryState::empty};
};

/** One fixed placement identity count, separate from the handle-keyed lifetime table. */
struct IdentityEntry final {
    std::uint64_t key{};
    std::uint32_t count{};
    EntryState state{EntryState::empty};
};

/** Datum bytes through the object-list tuple, and through the retained placement identity. */
constexpr std::uint32_t kDatumTupleBytes = offsetof(DatumIdentity, placementIdentity);
constexpr std::uint32_t kDatumIdentityBytes = sizeof(DatumIdentity);

std::array<RegistryEntry, kRegistryCapacity> g_entries{};
std::array<IdentityEntry, kRegistryCapacity> g_identities{};
std::uint64_t g_overflowCount{};
std::array<hooking::detour::Handle, 7> g_handles{};

/** @return A stable open-address bucket for a native handle. */
[[nodiscard]] constexpr std::size_t bucket(std::uint32_t handle) noexcept {
    return (static_cast<std::size_t>(handle) * 2654435761U) & kRegistryMask;
}

/** Clears every retained identity. Caller owns g_lock exclusively. */
void clear_registry() noexcept {
    g_entries = {};
    g_identities = {};
    g_liveCount = 0;
    g_identityReportBudget = kIdentityReportBudget;
    g_dynamicReportBudget = kIdentityReportBudget;
    g_allocateReportBudget = kIdentityReportBudget;
    g_logicalDestroyReportBudget = kIdentityReportBudget;
    g_dynamicCount = 0;
}

/**
 * Combines one placement into a nonzero registry key.
 * A real placed-entry identity wins: a mirrored map variant repeats it under a different
 * object-list tag, which the tuple alone would count twice. No identity means the tuple key.
 */
[[nodiscard]] constexpr std::uint64_t identity_key(const Instance& instance) noexcept {
    if (has_placement_identity(instance)) {
        return instance.placementIdentity;
    }
    return (static_cast<std::uint64_t>(instance.objectListTag) << 32U) | instance.entryIndex;
}

/** Adjusts one placement count and reports the first simultaneous duplicate. */
void adjust_identity(const Instance& instance, bool increment) noexcept {
    const std::uint64_t key = identity_key(instance);
    const std::size_t first =
        static_cast<std::size_t>((key ^ (key >> 32U)) * 2654435761U) & kRegistryMask;
    std::size_t tombstone = kRegistryCapacity;
    for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
        const std::size_t index = (first + probe) & kRegistryMask;
        IdentityEntry& entry = g_identities[index];
        if (entry.state == EntryState::live && entry.key == key) {
            if (increment) {
                ++entry.count;
                // Every repeat is reported with its caller, so the pass that rebuilt it is named.
                if (entry.count >= 2) {
                    std::array<char, 224> line{};
                    const std::uintptr_t caller = g_instantiateCaller >= g_moduleBase
                                                      ? g_instantiateCaller - g_moduleBase
                                                      : g_instantiateCaller;
                    const int written = std::snprintf(
                        line.data(),
                        line.size(),
                        "ev=world_object stage=placement result=duplicate list=0x%08X "
                        "entry=%u identity=0x%016llX count=%u caller=+0x%llX",
                        instance.objectListTag,
                        instance.entryIndex,
                        static_cast<unsigned long long>(instance.placementIdentity),
                        entry.count,
                        static_cast<unsigned long long>(caller));
                    if (written > 0) {
                        core::log::write(core::log::Channel::client,
                                         core::log::Level::debug,
                                         {line.data(), static_cast<std::size_t>(written)});
                    }
                }
            } else if (entry.count > 1) {
                --entry.count;
            } else {
                entry.state = EntryState::tombstone;
                entry.count = 0;
            }
            return;
        }
        if (entry.state == EntryState::tombstone && tombstone == kRegistryCapacity) {
            tombstone = index;
        }
        if (entry.state != EntryState::empty) {
            continue;
        }
        if (!increment) {
            return;
        }
        IdentityEntry& destination =
            g_identities[tombstone == kRegistryCapacity ? index : tombstone];
        destination = {key, 1, EntryState::live};
        return;
    }
    if (increment && tombstone < kRegistryCapacity) {
        g_identities[tombstone] = {key, 1, EntryState::live};
    }
}

/**
 * Returns a tombstone run to empty when the slot after it is already empty.
 * A probe stops only on empty, so a run that never becomes empty makes every later miss walk the
 * whole table. Nothing live can sit past an empty slot, so the run behind one is free to reclaim.
 * @param index Slot just tombstoned. Caller owns g_lock exclusively.
 */
void reclaim_tombstones(std::size_t index) noexcept {
    if (g_entries[(index + 1) & kRegistryMask].state != EntryState::empty) {
        return;
    }
    std::size_t slot = index;
    while (g_entries[slot].state == EntryState::tombstone) {
        g_entries[slot] = {};
        slot = (slot - 1) & kRegistryMask;
    }
}

/** Checks the native generation pair and the three datum identity fields. */
[[nodiscard]] bool validate(const Instance& instance) noexcept {
    if (g_validatePair == nullptr) {
        return false;
    }
    const HandlePair pair{instance.generation, instance.handle};
    std::int32_t resolved = -1;
    __try {
        g_validatePair(&pair, &resolved);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (static_cast<std::uint32_t>(resolved) != instance.handle) {
        return false;
    }

    DatumIdentity identity{};
    if (!read_datum_identity(instance.handle, identity)) {
        return false;
    }
    return identity.selfHandle == instance.handle
           && identity.objectListTag == instance.objectListTag
           && identity.entryIndex == instance.entryIndex;
}

/** @return True when no replacement still owns a trampoline call. */
[[nodiscard]] bool calls_idle() noexcept {
    return g_activeCalls.load(std::memory_order_acquire) == 0;
}

struct Targets final {
    std::byte* instantiate{};
    std::byte* destroy{};
    std::byte* allocate{};
    std::byte* logicalDestroy{};
    std::byte* resolvePair{};
    std::byte* validatePair{};
    std::byte* datumLayout{};
    std::byte* createEntity{};
    std::byte* purgeEntities{};
    std::byte* glueMapping{};
    std::byte* entityPool{};
    std::byte* entityPolicy{};
};

/** @return The main module's base, or zero when it cannot be read. */
[[nodiscard]] std::uintptr_t main_module_base() noexcept {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

/**
 * Resolves every target in one image pass.
 * @param output Receives one address per target, in placement and entity order.
 * @return False unless both signature groups fill the list and every one matched once.
 */
[[nodiscard]] bool resolve_targets(Targets& output) noexcept {
    output = {};
    executable::ExecutableImage main{};
    if (!executable::inspect_main_module(main)) {
        return false;
    }
    std::array<patterns::ImageRange, executable::kPeSectionLimit> ranges{};
    for (std::size_t index = 0; index < main.count; ++index) {
        ranges[index] = patterns::ImageRange{main.sections[index]};
    }
    const std::array<std::span<const patterns::Pattern>, 2> groups{kPlacementSignatures,
                                                                   entity_patterns()};
    std::array<patterns::Pattern, kTargetCount> definitions{};
    std::size_t next = 0;
    for (const std::span<const patterns::Pattern>& group : groups) {
        for (const patterns::Pattern& pattern : group) {
            if (next == definitions.size()) {
                return false;
            }
            definitions[next] = pattern;
            ++next;
        }
    }
    if (next != definitions.size()) {
        return false;
    }
    std::array<patterns::Match, kTargetCount> matches{};
    if (!patterns::resolve_all(std::span(ranges.data(), main.count), definitions, matches)) {
        return false;
    }
    for (const patterns::Match& match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }
    output.instantiate = matches[0].address;
    output.destroy = matches[1].address;
    output.allocate = matches[2].address;
    output.logicalDestroy = matches[3].address;
    output.resolvePair = matches[4].address;
    output.validatePair = matches[5].address;
    output.datumLayout = matches[6].address;
    output.createEntity = matches[7].address;
    output.purgeEntities = matches[8].address;
    output.glueMapping = matches[9].address;
    output.entityPool = matches[10].address;
    output.entityPolicy = matches[11].address;
    return true;
}

/** Binds the two datum globals encoded at fixed operands in the checked layout signature. */
[[nodiscard]] bool bind_datum_layout(std::byte* target) noexcept {
    if (target == nullptr) {
        return false;
    }
    g_datumStrideStorage = reinterpret_cast<const std::uint32_t*>(
        patterns::resolve_relative(target + 41, target + 45));
    g_datumBaseStorage = reinterpret_cast<const std::uintptr_t*>(
        patterns::resolve_relative(target + 57, target + 61));
    return g_datumStrideStorage != nullptr && g_datumBaseStorage != nullptr;
}

} // namespace

SRWLOCK g_lock{SRWLOCK_INIT};
std::atomic_uint32_t g_activeCalls{};
std::atomic_bool g_accepting{};
std::uintptr_t g_moduleBase{};
std::size_t g_liveCount{};
std::size_t g_identityReportBudget{kIdentityReportBudget};
std::size_t g_dynamicReportBudget{kIdentityReportBudget};
std::size_t g_allocateReportBudget{kIdentityReportBudget};
std::size_t g_logicalDestroyReportBudget{kIdentityReportBudget};
std::uint64_t g_dynamicCount{};
std::uintptr_t g_instantiateCaller{};
/**
 * Handles the dynamic path built, so the destroy detour can say which of them the client tears
 * down. A dynamic build names no placed entry, so the identity registry never holds one, and
 * whether a bubble crossing removes it is not known.
 */
std::array<std::uint32_t, kDynamicHandleCapacity> g_dynamicHandles{};
std::array<std::uint64_t, kDynamicHandleCapacity> g_dynamicOrdinals{};
ResolvePair g_resolvePair{};
ValidatePair g_validatePair{};
const std::uintptr_t* g_datumBaseStorage{};
const std::uint32_t* g_datumStrideStorage{};

/** Inserts or replaces one handle without allocating. Caller owns g_lock exclusively. */
void retain(const Instance& instance) noexcept {
    std::size_t firstTombstone = kRegistryCapacity;
    for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
        const std::size_t index = (bucket(instance.handle) + probe) & kRegistryMask;
        RegistryEntry& entry = g_entries[index];
        if (entry.state == EntryState::live && entry.instance.handle == instance.handle) {
            if (identity_key(entry.instance) != identity_key(instance)) {
                adjust_identity(entry.instance, false);
                adjust_identity(instance, true);
            }
            entry.instance = instance;
            return;
        }
        if (entry.state == EntryState::tombstone && firstTombstone == kRegistryCapacity) {
            firstTombstone = index;
        }
        if (entry.state != EntryState::empty) {
            continue;
        }
        RegistryEntry& destination =
            g_entries[firstTombstone == kRegistryCapacity ? index : firstTombstone];
        destination.instance = instance;
        destination.state = EntryState::live;
        adjust_identity(instance, true);
        ++g_liveCount;
        return;
    }
    if (firstTombstone != kRegistryCapacity) {
        RegistryEntry& destination = g_entries[firstTombstone];
        destination.instance = instance;
        destination.state = EntryState::live;
        adjust_identity(instance, true);
        ++g_liveCount;
        return;
    }
    ++g_overflowCount;
}

/** Erases one handle before native teardown can recycle its generation. */
void erase(std::uint32_t handle) noexcept {
    for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
        const std::size_t index = (bucket(handle) + probe) & kRegistryMask;
        RegistryEntry& entry = g_entries[index];
        if (entry.state == EntryState::empty) {
            return;
        }
        if (entry.state == EntryState::live && entry.instance.handle == handle) {
            adjust_identity(entry.instance, false);
            entry.state = EntryState::tombstone;
            --g_liveCount;
            if (g_liveCount == 0) {
                clear_registry();
                return;
            }
            reclaim_tombstones(index);
            return;
        }
    }
}

/**
 * Reads one object datum's identity block.
 * A stride that stops before the placement identity still yields the tuple, and leaves the
 * identity zero, so a layout surprise degrades to tuple-keyed counting instead of silence.
 */
bool read_datum_identity(std::uint32_t handle, DatumIdentity& output) noexcept {
    if (g_datumBaseStorage == nullptr || g_datumStrideStorage == nullptr) {
        return false;
    }
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!read_value(g_datumBaseStorage, base) || !read_value(g_datumStrideStorage, stride)
        || base == 0 || stride < kDatumTupleBytes) {
        return false;
    }
    const std::size_t wanted =
        stride >= kDatumIdentityBytes ? kDatumIdentityBytes : kDatumTupleBytes;
    output = {};
    return memory::read_current_process(
        nullptr,
        base + static_cast<std::uintptr_t>(stride) * (handle & 0x1FFFU),
        std::span(reinterpret_cast<std::byte*>(&output), wanted));
}

/** Installs the generation-checked placed-object lifetime capture. */
bool install() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const auto attached = [](const auto& handle) { return handle.attached; };
    if (std::all_of(g_handles.begin(), g_handles.end(), attached)) {
        const bool accepting = g_accepting.load(std::memory_order_acquire);
        ReleaseSRWLockExclusive(&g_lock);
        return accepting;
    }
    if (std::any_of(g_handles.begin(), g_handles.end(), attached)) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_moduleBase = main_module_base();
    Targets targets{};
    if (!resolve_targets(targets) || !bind_datum_layout(targets.datumLayout)) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=world_objects stage=install result=fail reason=targets");
        return false;
    }
    g_resolvePair = reinterpret_cast<ResolvePair>(targets.resolvePair);
    g_glueStrideStorage = reinterpret_cast<const std::uint32_t*>(
        patterns::resolve_relative(targets.glueMapping + 9, targets.glueMapping + 13));
    g_glueBaseStorage = reinterpret_cast<const std::uintptr_t*>(
        patterns::resolve_relative(targets.glueMapping + 18, targets.glueMapping + 22));
    g_entityRecordBase = reinterpret_cast<std::uintptr_t>(
        patterns::resolve_relative(targets.entityPool + 20, targets.entityPool + 24));
    g_validatePair = reinterpret_cast<ValidatePair>(targets.validatePair);
    const std::array specs{
        hooking::detour::Spec{targets.instantiate, reinterpret_cast<void*>(&instantiate)},
        hooking::detour::Spec{targets.destroy, reinterpret_cast<void*>(&destroy)},
        hooking::detour::Spec{targets.allocate, reinterpret_cast<void*>(&allocate)},
        hooking::detour::Spec{targets.logicalDestroy, reinterpret_cast<void*>(&logical_destroy)},
        hooking::detour::Spec{targets.createEntity, reinterpret_cast<void*>(&create_entity)},
        hooking::detour::Spec{targets.purgeEntities, reinterpret_cast<void*>(&purge_entities)},
        hooking::detour::Spec{targets.entityPolicy, reinterpret_cast<void*>(&entity_policy)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        g_resolvePair = nullptr;
        g_validatePair = nullptr;
        g_datumBaseStorage = nullptr;
        g_datumStrideStorage = nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=world_objects stage=install result=fail reason=detour");
        return false;
    }
    g_instantiateOriginal.store(reinterpret_cast<Instantiate>(g_handles[0].original),
                                std::memory_order_release);
    g_destroyOriginal.store(reinterpret_cast<Destroy>(g_handles[1].original),
                            std::memory_order_release);
    g_allocateOriginal.store(reinterpret_cast<Allocate>(g_handles[2].original),
                             std::memory_order_release);
    g_logicalDestroyOriginal.store(reinterpret_cast<LogicalDestroy>(g_handles[3].original),
                                   std::memory_order_release);
    g_createEntityOriginal.store(reinterpret_cast<CreateEntity>(g_handles[4].original),
                                 std::memory_order_release);
    g_purgeEntitiesOriginal.store(reinterpret_cast<PurgeEntities>(g_handles[5].original),
                                  std::memory_order_release);
    g_entityPolicyOriginal.store(reinterpret_cast<EntityPolicy>(g_handles[6].original),
                                 std::memory_order_release);

    g_accepting.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=world_objects stage=install result=ok");
    return true;
}

/** Removes the world hooks after native calls have left their trampolines. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (std::none_of(g_handles.begin(), g_handles.end(), [](const auto& handle) {
            return handle.attached;
        })) {
        clear_registry();
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    g_accepting.store(false, std::memory_order_release);
    const std::array<hooking::detour::ProtectedCodeEntry, 7> protectedEntries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&instantiate)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&destroy)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&allocate)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&logical_destroy)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&create_entity)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&purge_entities)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&entity_policy)},

    };
    const hooking::detour::UninstallResult result =
        hooking::detour::uninstall(g_handles, protectedEntries, &calls_idle);
    if (result != hooking::detour::UninstallResult::removed) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_instantiateOriginal.store(nullptr, std::memory_order_release);
    g_destroyOriginal.store(nullptr, std::memory_order_release);
    g_allocateOriginal.store(nullptr, std::memory_order_release);
    g_logicalDestroyOriginal.store(nullptr, std::memory_order_release);
    g_createEntityOriginal.store(nullptr, std::memory_order_release);
    g_purgeEntitiesOriginal.store(nullptr, std::memory_order_release);
    g_entityPolicyOriginal.store(nullptr, std::memory_order_release);
    g_entityRecordBase = 0;
    g_policyTrace = {};
    g_glueBaseStorage = nullptr;
    g_glueStrideStorage = nullptr;
    g_resolvePair = nullptr;
    g_validatePair = nullptr;
    g_datumBaseStorage = nullptr;
    g_datumStrideStorage = nullptr;
    clear_registry();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** @return True while all world hooks accept observations. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool installed =
        g_handles[0].attached && g_handles[1].attached && g_handles[2].attached
        && g_handles[3].attached && g_handles[4].attached && g_handles[5].attached
        && std::all_of(
            g_handles.begin() + 6, g_handles.end(), [](const auto& h) { return h.attached; })
        && g_accepting.load(std::memory_order_acquire);
    ReleaseSRWLockShared(&g_lock);
    return installed;
}

/** Finds exact live instances, pruning any generation or datum mismatch. */
std::size_t
find(std::uint32_t objectListTag, std::uint32_t entryIndex, std::span<Instance> output) noexcept {
    std::size_t found = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (RegistryEntry& entry : g_entries) {
        if (entry.state != EntryState::live || entry.instance.objectListTag != objectListTag
            || entry.instance.entryIndex != entryIndex) {
            continue;
        }
        if (!validate(entry.instance)) {
            entry.state = EntryState::tombstone;
            --g_liveCount;
            continue;
        }
        if (found < output.size()) {
            output[found] = entry.instance;
        }
        ++found;
    }
    if (g_liveCount == 0) {
        clear_registry();
    }
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

/** @return Current bounded-registry counters. */
Diagnostics diagnostics() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Diagnostics result{g_liveCount,
                             g_overflowCount,
                             g_handles[0].attached && g_handles[1].attached
                                 && g_accepting.load(std::memory_order_acquire)};
    ReleaseSRWLockShared(&g_lock);
    return result;
}

} // namespace sunrise::client::hooks::world_objects
