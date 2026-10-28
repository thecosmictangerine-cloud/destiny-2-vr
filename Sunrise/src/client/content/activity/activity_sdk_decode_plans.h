#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/activity_sdk/format.h"

// Builds the SObject decode-plan cache pair that the entity transport loads beside the pack.
// The plans compile package RSAT selections against the executable's reflection metadata.

namespace sunrise::client::content::activity::sdk_generation::decode_plans {

/** Why one cache build stopped. */
enum class Status : std::uint8_t {
    ready,
    cancelled,
    invalidInput,
    executableUnavailable,
    executableInvalid,
    reflectionInvalid,
    packageRead,
    hashFailed,
    writeFailed,
    allocation,
};

/** Optional cancellation probe polled between plans. */
using CancelProbe = bool (*)(void* context) noexcept;

/** Everything one build reads. The descriptors name the component and selection pairs. */
struct Request final {
    /** Executable file, hashed into the evidence; read for reflection when no module is given. */
    std::wstring_view executablePath{};
    /** Base of the running client module; the packed file carries no slot table on disk. */
    const void* executableModule{};
    const middleware::content::packages::reader::Source* source{};
    std::span<const state::activity_sdk::format::SobjectRsatDescriptor> descriptors{};
    std::span<const std::byte> sdkBuildSha256{};
    std::wstring_view cachePath{};
    CancelProbe cancel{};
    void* cancelContext{};
};

/** Counts reported after one complete build. */
struct Result final {
    std::uint32_t slots{};
    std::uint32_t pairs{};
    std::uint32_t plans{};
    std::uint32_t active{};
    std::uint32_t errors{};
    std::uint32_t entries{};
    std::uint32_t schemas{};
    std::uint32_t fields{};
};

/**
 * Compiles every descriptor pair and writes the cache and its manifest beside the pack.
 * @param request Executable, package source, descriptor pairs, SDK identity and output path.
 * @param output Receives the counts of the written cache.
 * @return Status::ready when both files are complete on disk.
 */
[[nodiscard]] Status build(const Request& request, Result& output) noexcept;

/** @return The stable log name of one build status. */
[[nodiscard]] const char* status_name(Status status) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans
