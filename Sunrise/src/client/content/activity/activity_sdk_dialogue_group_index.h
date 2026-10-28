#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::client::content::activity::sdk_generation::dialogue_group_index {

/** One content range keyed by its authored dialogue definition hash. */
struct Span final {
    std::uint32_t definitionHash{};
    std::size_t begin{};
    std::size_t end{};
};

/** One cue definition's delayed-request expiration window, in seconds. */
struct Definition final {
    std::uint32_t hash{};
    float authoredWindowSeconds{};

    bool operator==(const Definition&) const = default;
};

/** Reads every authored cue definition in bank order, including empty windows. */
[[nodiscard]] bool definitions(std::span<const std::byte> bytes,
                               std::vector<Definition>& output) noexcept;

/** Builds a hash-sorted index from the native 16-byte dialogue group rows. */
[[nodiscard]] bool build(std::span<const std::byte> bytes,
                         std::size_t groupRows,
                         std::size_t groupCount,
                         std::vector<Span>& output) noexcept;

/** Finds the content range for one cue definition hash. */
[[nodiscard]] bool
find(std::span<const Span> groups, std::uint32_t definitionHash, Span& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::dialogue_group_index
