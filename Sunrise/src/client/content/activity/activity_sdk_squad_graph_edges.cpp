#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "../../../middleware/crypto/sha256.h"
#include "activity_sdk_squad_graph_internal.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory::detail {
namespace {

namespace crypto = middleware::crypto::sha256;
namespace tables = middleware::content::packages::tables;
namespace topology = topology_inventory;

struct ObjectReference final {
    std::uint32_t objectKey{};
    std::uint16_t slotIndex{};
    std::uint8_t slotType{};
    bool valid{};
};

struct TargetKey final {
    std::uint32_t objectKey{};
    std::uint32_t slotType{};
    std::uint32_t slotIndex{};

    bool operator==(const TargetKey&) const = default;
};

struct TargetKeyHash final {
    [[nodiscard]] std::size_t operator()(const TargetKey& value) const noexcept {
        std::size_t hash = value.objectKey;
        hash ^= static_cast<std::size_t>(value.slotType) << 1U;
        hash ^= static_cast<std::size_t>(value.slotIndex) << 17U;
        return hash;
    }
};

struct RuleObjectKey final {
    std::uint32_t ruleRow{};
    std::uint32_t objectRow{};

    bool operator==(const RuleObjectKey&) const = default;
};

struct RuleObjectKeyHash final {
    [[nodiscard]] std::size_t operator()(const RuleObjectKey& value) const noexcept {
        return static_cast<std::size_t>(value.ruleRow)
               ^ (static_cast<std::size_t>(value.objectRow) << 17U);
    }
};

using TargetGroup = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;

struct ExactTarget final {
    std::vector<std::uint32_t> descriptors{};
    std::uint32_t referenceMask{};
};

/** One edge held until both of its descriptors resolve to catalog rows. */
struct PendingEdge final {
    std::string id{};
    std::uint32_t spawnerRow{format::kAbsentIndex};
    std::uint32_t ruleRow{format::kAbsentIndex};
    std::uint32_t sourceDescriptorRow{format::kAbsentIndex};
    std::uint32_t targetObjectRow{format::kAbsentIndex};
    std::uint32_t targetSlotRow{format::kAbsentIndex};
    std::uint32_t referenceMask{};
    std::vector<std::uint32_t> targetDescriptors{};
    std::vector<std::uint32_t> scenarios{};
    bool associationExact{};
};

/** @return True when one topology text owns exactly its declared bytes. */
[[nodiscard]] bool text_view(const topology::Text& text, std::string_view& output) noexcept {
    output = {};
    if (text.length >= text.value.size()) {
        return false;
    }
    output = std::string_view(text.value.data(), text.length);
    return text.value[text.length] == '\0';
}

template <typename... Values>
[[nodiscard]] bool format_text(std::string& output, const char* pattern, Values... values) {
    std::array<char, 192> bytes{};
    const int count = std::snprintf(bytes.data(), bytes.size(), pattern, values...);
    if (count < 0 || static_cast<std::size_t>(count) >= bytes.size()) {
        return false;
    }
    output.assign(bytes.data(), static_cast<std::size_t>(count));
    return true;
}

/**
 * Appends one unsigned value in little-endian order.
 * @tparam Value Unsigned type whose whole width is appended.
 * @return False when the buffer cannot grow.
 */
template <typename Value>
[[nodiscard]] bool append_little(std::vector<std::byte>& output, Value value) {
    static_assert(std::is_unsigned_v<Value>);
    if (output.size() > (std::numeric_limits<std::size_t>::max)() - sizeof(Value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & Value{0xFFU}));
    }
    return true;
}

/** Appends one length-prefixed hash input part. @return False when the buffer cannot grow. */
[[nodiscard]] bool append_hash_part(std::vector<std::byte>& output, std::string_view value) {
    if (!append_little(output, static_cast<std::uint64_t>(value.size()))
        || value.size() > (std::numeric_limits<std::size_t>::max)() - output.size()) {
        return false;
    }
    output.reserve(output.size() + value.size());
    for (const unsigned char byte : value) {
        output.push_back(static_cast<std::byte>(byte));
    }
    return true;
}

/** Hashes one domain and its parts to lowercase hex. @return False when hashing fails. */
[[nodiscard]] bool
domain_hash(std::string_view domain, std::span<const std::string_view> parts, std::string& output) {
    output.clear();
    std::vector<std::byte> input{};
    if (!append_little(input, static_cast<std::uint32_t>(domain.size()))) {
        return false;
    }
    input.reserve(input.size() + domain.size());
    for (const unsigned char byte : domain) {
        input.push_back(static_cast<std::byte>(byte));
    }
    for (const std::string_view part : parts) {
        if (!append_hash_part(input, part)) {
            return false;
        }
    }
    crypto::Digest digest{};
    if (!crypto::hash(input, digest)) {
        return false;
    }
    // Edge ids are published in lowercase hex.
    static constexpr std::array<char, 16> kHex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    output.resize(digest.size() * 2U);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = static_cast<unsigned char>(digest[index]);
        output[index * 2U] = kHex[value >> 4U];
        output[index * 2U + 1U] = kHex[value & 0xFU];
    }
    return true;
}

/** Decodes one packed object reference; the result is invalid when any field is out of range. */
[[nodiscard]] ObjectReference decode_reference(std::uint64_t raw) noexcept {
    ObjectReference output{};
    output.objectKey = static_cast<std::uint32_t>(raw);
    output.slotType = static_cast<std::uint8_t>(raw >> 32U);
    const auto padding = static_cast<std::uint8_t>(raw >> 40U);
    output.slotIndex = static_cast<std::uint16_t>(raw >> 48U);
    output.valid = output.objectKey != 0 && output.objectKey != format::kAbsentIndex
                   && output.slotType >= 1 && output.slotType <= 72 && padding == 0
                   && output.slotIndex != 0xFFFFU;
    return output;
}

/** Builds the join key of one edge target. @return False when the descriptor is out of range. */
[[nodiscard]] bool target_key(const topology::Snapshot& topology,
                              const GraphDescriptor& descriptor,
                              TargetKey& output) noexcept {
    output = {};
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    if (slot.objectIndex != descriptor.objectIndex) {
        return false;
    }
    output = {topology.objects[descriptor.objectIndex].objectKey, slot.slotType, slot.slotIndex};
    return true;
}

/** Builds the stable text id of one spawner edge. */
[[nodiscard]] bool edge_identity(const topology::Snapshot& topology,
                                 const GraphSpawner& spawner,
                                 const GraphRule& rule,
                                 const GraphDescriptor& source,
                                 const GraphDescriptor& target,
                                 std::string& output) {
    std::string objectId{};
    std::string_view slotId{};
    if (target.objectIndex >= topology.objects.size() || target.slotIndex >= topology.slots.size()
        || !format_text(objectId,
                        "object/%08x",
                        static_cast<unsigned>(topology.objects[target.objectIndex].objectTag))
        || !text_view(topology.slots[target.slotIndex].id, slotId)) {
        return false;
    }
    const std::array<std::string_view, 5> parts{spawner.id, source.id, rule.id, objectId, slotId};
    std::string digest{};
    if (!domain_hash("sunrise-authored-spawner-rule-edge-v1", parts, digest)) {
        return false;
    }
    output = "authored-spawner-rule-edge/" + digest;
    return true;
}

/** Sorts and deduplicates every scenario list used by the edge join. */
template <typename Index> void canonicalize_scenarios(Index& index) {
    for (auto& [key, scenarios] : index) {
        (void)key;
        std::sort(scenarios.begin(), scenarios.end());
        scenarios.erase(std::unique(scenarios.begin(), scenarios.end()), scenarios.end());
    }
}

/**
 * Indexes the exact rule contexts once. The former edge loop rescanned every config context and,
 * for every candidate, every rule point and point context.
 */
[[nodiscard]] bool build_exact_rule_scenarios(
    const GraphSnapshot& graph,
    std::unordered_map<RuleObjectKey, std::vector<std::uint32_t>, RuleObjectKeyHash>& output) {
    std::vector<std::uint32_t> contextPointCounts(graph.configContexts.size());
    std::vector<std::uint32_t> contextExactPointCounts(graph.configContexts.size());
    for (std::uint32_t pointRow = 0; pointRow < graph.points.size(); ++pointRow) {
        const GraphPoint& point = graph.points[pointRow];
        if (point.contexts.first > graph.pointContexts.size()
            || point.contexts.count > graph.pointContexts.size() - point.contexts.first) {
            return false;
        }
        for (std::uint32_t contextRow = point.contexts.first;
             contextRow < point.contexts.first + point.contexts.count;
             ++contextRow) {
            const GraphPointContext& context = graph.pointContexts[contextRow];
            if (context.pointRow != pointRow
                || context.configContextRow >= graph.configContexts.size()) {
                return false;
            }
            ++contextPointCounts[context.configContextRow];
            if (context.status == PointContextStatus::exact && context.matches.count == 1) {
                ++contextExactPointCounts[context.configContextRow];
            }
        }
    }
    for (std::uint32_t contextRow = 0; contextRow < graph.configContexts.size(); ++contextRow) {
        const GraphConfigContext& context = graph.configContexts[contextRow];
        if (context.ruleRow == format::kAbsentIndex) {
            continue;
        }
        if (context.ruleRow >= graph.rules.size()) {
            return false;
        }
        const GraphRule& rule = graph.rules[context.ruleRow];
        // An inline rule carries its own placement, so no object-list match is needed.
        const bool pointsExact = rule.inlineForm
                                 || (contextPointCounts[contextRow] == rule.points.count
                                     && contextExactPointCounts[contextRow] == rule.points.count);
        if (rule.points.count != 0 && pointsExact) {
            output[{context.ruleRow, context.objectIndex}].push_back(context.scenarioIndex);
        }
    }
    canonicalize_scenarios(output);
    return true;
}

/** Materializes one reference's candidates and exact logical target, if any. */
[[nodiscard]] bool resolve_reference(
    const topology::Snapshot& topology,
    GraphSnapshot& graph,
    std::uint32_t spawnerRow,
    std::uint32_t referenceOrdinal,
    std::uint64_t rawReference,
    const std::unordered_map<TargetKey, std::vector<std::uint32_t>, TargetKeyHash>& byTarget,
    const std::unordered_map<std::uint32_t, std::uint32_t>& rulesByConfig,
    const std::vector<std::vector<std::uint32_t>>& scenariosByObject,
    std::map<TargetGroup, ExactTarget>& exactTargets,
    ReferenceResolutionStatus& status,
    std::uint32_t scopedSource = format::kAbsentIndex) {
    const ObjectReference decoded = decode_reference(rawReference);
    GraphReference reference{};
    reference.spawnerRow = spawnerRow;
    reference.sourceDescriptorRow = scopedSource;
    reference.referenceOrdinal = referenceOrdinal;
    reference.rawReference = rawReference;
    reference.targetObjectKey = decoded.objectKey;
    reference.targetSlotType = decoded.slotType;
    reference.targetSlotIndex = decoded.slotIndex;
    reference.encodingValid = decoded.valid;
    reference.candidateDescriptors.first =
        static_cast<std::uint32_t>(graph.referenceDescriptors.size());
    const std::uint32_t referenceRow = static_cast<std::uint32_t>(graph.references.size());
    if (!decoded.valid) {
        status = ReferenceResolutionStatus::invalidEncoding;
        reference.status = status;
        graph.references.push_back(reference);
        return true;
    }

    const TargetKey key{decoded.objectKey, decoded.slotType, decoded.slotIndex};
    const auto found = byTarget.find(key);
    const std::vector<std::uint32_t> empty{};
    const std::vector<std::uint32_t>& candidates = found == byTarget.end() ? empty : found->second;
    std::map<TargetGroup, std::vector<std::uint32_t>> groups{};
    std::size_t authoritativeCount = 0;
    const GraphSpawner& spawner = graph.spawners[spawnerRow];
    const auto sourceStatus = scopedSource != format::kAbsentIndex ? SourceDescriptorStatus::exact
                                                                   : spawner.sourceDescriptorStatus;
    const auto sourceRow =
        scopedSource != format::kAbsentIndex ? scopedSource : spawner.sourceDescriptorRow;
    for (const std::uint32_t descriptorRow : candidates) {
        const GraphDescriptor& descriptor = graph.descriptors[descriptorRow];
        // Object keys are reused by campaign and arcade scenarios. A reference can only
        // name a target that occurs in a scenario containing its source object.
        if (sourceStatus == SourceDescriptorStatus::exact) {
            const GraphDescriptor& source = graph.descriptors[sourceRow];
            const auto& sourceScenarios = scenariosByObject[source.objectIndex];
            const auto& targetScenarios = scenariosByObject[descriptor.objectIndex];
            const bool sharesScenario =
                std::any_of(sourceScenarios.begin(), sourceScenarios.end(), [&](auto scenario) {
                    return std::binary_search(
                        targetScenarios.begin(), targetScenarios.end(), scenario);
                });
            if (!sharesScenario) {
                continue;
            }
        }
        const topology::Slot& slot = topology.slots[descriptor.slotIndex];
        const auto rule = rulesByConfig.find(descriptor.configTag);
        const bool authoritative =
            descriptor.componentClass == tables::kAuthoredSquadRulePrimaryClass
            && slot.slotType == tables::kAuthoredSquadRuleSlotType && rule != rulesByConfig.end();
        if (authoritative) {
            groups[{rule->second, descriptor.objectIndex, descriptor.slotIndex}].push_back(
                descriptorRow);
            ++authoritativeCount;
        }
        graph.referenceDescriptors.push_back({referenceRow, descriptorRow, authoritative, false});
    }
    reference.candidateDescriptors.count =
        static_cast<std::uint32_t>(graph.referenceDescriptors.size())
        - reference.candidateDescriptors.first;

    if (sourceStatus == SourceDescriptorStatus::missing) {
        status = ReferenceResolutionStatus::sourceDescriptorMissing;
    } else if (sourceStatus == SourceDescriptorStatus::ambiguous) {
        status = ReferenceResolutionStatus::sourceDescriptorAmbiguous;
    } else if (reference.candidateDescriptors.count == 0) {
        status = ReferenceResolutionStatus::targetMissing;
    } else if (authoritativeCount != reference.candidateDescriptors.count) {
        status = ReferenceResolutionStatus::targetDescriptorMismatch;
    } else if (groups.size() != 1) {
        status = ReferenceResolutionStatus::targetAmbiguous;
    } else {
        status = ReferenceResolutionStatus::exact;
        const auto& [group, resolved] = *groups.begin();
        reference.resolvedRuleRow = std::get<0>(group);
        reference.resolvedObjectRow = std::get<1>(group);
        reference.resolvedSlotRow = std::get<2>(group);
        ExactTarget& target = exactTargets[group];
        if (!target.descriptors.empty() && target.descriptors != resolved) {
            return false;
        }
        target.descriptors = resolved;
        target.referenceMask |= 1U << referenceOrdinal;
        const std::size_t begin = reference.candidateDescriptors.first;
        const std::size_t end = begin + reference.candidateDescriptors.count;
        for (std::size_t row = begin; row < end; ++row) {
            graph.referenceDescriptors[row].resolvedTarget =
                std::find(
                    resolved.begin(), resolved.end(), graph.referenceDescriptors[row].descriptorRow)
                != resolved.end();
        }
    }
    reference.status = status;
    graph.references.push_back(reference);
    return true;
}

/** A reused config is unambiguous in disjoint scenario contexts. Never resolve two
 * different source slots that coexist in the same scenario by arbitrary row order. */
[[nodiscard]] std::vector<std::uint32_t>
scoped_sources(const GraphSnapshot& graph,
               const GraphSpawner& spawner,
               const std::vector<std::vector<std::uint32_t>>& scenarios) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> logical;
    for (std::uint32_t i = 0; i < spawner.sourceDescriptorCandidates.count; ++i) {
        const auto row =
            graph.sourceDescriptorCandidates[spawner.sourceDescriptorCandidates.first + i]
                .descriptorRow;
        const auto& d = graph.descriptors[row];
        logical.try_emplace({d.objectIndex, d.slotIndex}, row);
    }
    std::vector<std::uint32_t> result;
    for (const auto& entry : logical) {
        const auto identity = entry.first;
        const auto row = entry.second;
        const auto& owned = scenarios[identity.first];
        const bool overlap = std::any_of(
            logical.begin(), logical.end(), [&scenarios, &owned, identity](const auto& other) {
                if (other.first == identity) {
                    return false;
                }
                const auto& peers = scenarios[other.first.first];
                return std::any_of(owned.begin(), owned.end(), [&peers](auto scenario) {
                    return std::binary_search(peers.begin(), peers.end(), scenario);
                });
            });
        if (!overlap) {
            result.push_back(row);
        }
    }
    return result;
}

/** Resolves every spawner raw reference into sorted pending authored edges. */
[[nodiscard]] bool build_pending_edges(const topology::Snapshot& topology,
                                       GraphSnapshot& graph,
                                       std::vector<PendingEdge>& output) {
    std::unordered_map<std::uint32_t, std::uint32_t> rulesByConfig{};
    for (std::uint32_t row = 0; row < graph.rules.size(); ++row) {
        rulesByConfig.emplace(graph.rules[row].configTag, row);
    }
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> descriptorsByConfig{};
    std::unordered_map<TargetKey, std::vector<std::uint32_t>, TargetKeyHash> descriptorsByTarget{};
    for (std::uint32_t row = 0; row < graph.descriptors.size(); ++row) {
        const GraphDescriptor& descriptor = graph.descriptors[row];
        descriptorsByConfig[descriptor.configTag].push_back(row);
        TargetKey key{};
        if (!target_key(topology, descriptor, key)) {
            return false;
        }
        descriptorsByTarget[key].push_back(row);
    }
    std::vector<std::vector<std::uint32_t>> scenariosByObject(topology.objects.size());
    for (std::uint32_t row = 0; row < topology.occurrences.size(); ++row) {
        const topology::Occurrence& occurrence = topology.occurrences[row];
        if (occurrence.objectIndex >= scenariosByObject.size()) {
            return false;
        }
        scenariosByObject[occurrence.objectIndex].push_back(occurrence.scenarioIndex);
    }
    for (std::vector<std::uint32_t>& scenarios : scenariosByObject) {
        std::sort(scenarios.begin(), scenarios.end());
        scenarios.erase(std::unique(scenarios.begin(), scenarios.end()), scenarios.end());
    }
    std::unordered_map<RuleObjectKey, std::vector<std::uint32_t>, RuleObjectKeyHash>
        exactRuleScenarios{};
    if (!build_exact_rule_scenarios(graph, exactRuleScenarios)) {
        return false;
    }

    for (std::uint32_t spawnerRow = 0; spawnerRow < graph.spawners.size(); ++spawnerRow) {
        GraphSpawner& spawner = graph.spawners[spawnerRow];
        spawner.sourceDescriptorCandidates.first =
            static_cast<std::uint32_t>(graph.sourceDescriptorCandidates.size());
        for (const std::uint32_t descriptorRow : descriptorsByConfig[spawner.configTag]) {
            const GraphDescriptor& descriptor = graph.descriptors[descriptorRow];
            const topology::Slot& slot = topology.slots[descriptor.slotIndex];
            if (descriptor.componentClass == tables::kAuthoredSquadSpawnerPrimaryClass
                && slot.slotType == tables::kAuthoredSquadSpawnerSlotType) {
                graph.sourceDescriptorCandidates.push_back({spawnerRow, descriptorRow});
            }
        }
        spawner.sourceDescriptorCandidates.count =
            static_cast<std::uint32_t>(graph.sourceDescriptorCandidates.size())
            - spawner.sourceDescriptorCandidates.first;
        if (spawner.sourceDescriptorCandidates.count == 1) {
            spawner.sourceDescriptorStatus = SourceDescriptorStatus::exact;
            spawner.sourceDescriptorRow =
                graph.sourceDescriptorCandidates[spawner.sourceDescriptorCandidates.first]
                    .descriptorRow;
        } else if (spawner.sourceDescriptorCandidates.count == 0) {
            spawner.sourceDescriptorStatus = SourceDescriptorStatus::missing;
        } else {
            spawner.sourceDescriptorStatus = SourceDescriptorStatus::ambiguous;
        }

        spawner.references.first = static_cast<std::uint32_t>(graph.references.size());
        auto sources = scoped_sources(graph, spawner, scenariosByObject);
        if (sources.empty()) {
            sources.push_back(format::kAbsentIndex);
        }
        for (const auto sourceRow : sources) {
            std::map<TargetGroup, ExactTarget> exactTargets{};
            std::array<ReferenceResolutionStatus, 2> statuses{};
            const std::array<std::uint64_t, 2> raw{spawner.rawReference98, spawner.rawReferenceA0};
            for (std::uint32_t ordinal = 0; ordinal < raw.size(); ++ordinal) {
                if (!resolve_reference(topology,
                                       graph,
                                       spawnerRow,
                                       ordinal,
                                       raw[ordinal],
                                       descriptorsByTarget,
                                       rulesByConfig,
                                       scenariosByObject,
                                       exactTargets,
                                       statuses[ordinal],
                                       sourceRow)) {
                    return false;
                }
            }
            const bool hasAmbiguous = std::find(statuses.begin(),
                                                statuses.end(),
                                                ReferenceResolutionStatus::targetAmbiguous)
                                      != statuses.end();
            const bool hasOther = std::any_of(statuses.begin(), statuses.end(), [](auto status) {
                return status != ReferenceResolutionStatus::exact
                       && status != ReferenceResolutionStatus::invalidEncoding;
            });
            // With both refs absent, the spawner's own point set is the target, on its own slot.
            const bool bothAbsent = std::all_of(statuses.begin(), statuses.end(), [](auto status) {
                return status == ReferenceResolutionStatus::invalidEncoding;
            });
            if (bothAbsent && spawner.hasInlinePointSet
                && spawner.inlineRuleRow != format::kAbsentIndex
                && sourceRow != format::kAbsentIndex && exactTargets.empty()) {
                const GraphDescriptor& source = graph.descriptors[sourceRow];
                ExactTarget& target =
                    exactTargets[{spawner.inlineRuleRow, source.objectIndex, source.slotIndex}];
                target.descriptors = {sourceRow};
            }
            const bool associationExact = sourceRow != format::kAbsentIndex && !hasAmbiguous
                                          && !hasOther && !exactTargets.empty();
            if (sourceRow == format::kAbsentIndex) {
                continue;
            }
            for (const auto& [group, target] : exactTargets) {
                if (target.descriptors.empty()) {
                    return false;
                }
                PendingEdge edge{};
                edge.spawnerRow = spawnerRow;
                edge.ruleRow = std::get<0>(group);
                edge.sourceDescriptorRow = sourceRow;
                edge.targetObjectRow = std::get<1>(group);
                edge.targetSlotRow = std::get<2>(group);
                edge.referenceMask = target.referenceMask;
                edge.targetDescriptors = target.descriptors;
                std::sort(edge.targetDescriptors.begin(),
                          edge.targetDescriptors.end(),
                          [&graph](std::uint32_t first, std::uint32_t second) {
                              return graph.descriptors[first].id < graph.descriptors[second].id;
                          });
                edge.associationExact = associationExact;
                if (!edge_identity(topology,
                                   spawner,
                                   graph.rules[edge.ruleRow],
                                   graph.descriptors[edge.sourceDescriptorRow],
                                   graph.descriptors[edge.targetDescriptors.front()],
                                   edge.id)) {
                    return false;
                }

                const GraphDescriptor& source = graph.descriptors[edge.sourceDescriptorRow];
                if (source.objectIndex >= scenariosByObject.size()) {
                    return false;
                }
                const std::vector<std::uint32_t>& sourceScenarios =
                    scenariosByObject[source.objectIndex];
                const auto targetScenariosFound =
                    exactRuleScenarios.find({edge.ruleRow, edge.targetObjectRow});
                const std::vector<std::uint32_t> empty{};
                const std::vector<std::uint32_t>& targetScenarios =
                    targetScenariosFound == exactRuleScenarios.end() ? empty
                                                                     : targetScenariosFound->second;
                std::set_intersection(sourceScenarios.begin(),
                                      sourceScenarios.end(),
                                      targetScenarios.begin(),
                                      targetScenarios.end(),
                                      std::back_inserter(edge.scenarios));
                output.push_back(std::move(edge));
            }
        }
        spawner.references.count =
            static_cast<std::uint32_t>(graph.references.size()) - spawner.references.first;
    }
    std::sort(
        output.begin(), output.end(), [](const PendingEdge& first, const PendingEdge& second) {
            return first.id < second.id;
        });
    return std::adjacent_find(output.begin(),
                              output.end(),
                              [](const PendingEdge& first, const PendingEdge& second) {
                                  return first.id == second.id;
                              })
           == output.end();
}

} // namespace

/** Appends descriptor/reference/edge joins after all definition and context rows exist. */
bool build_graph_edges(const topology_inventory::Snapshot& topology,
                       const Facts& facts,
                       GraphSnapshot& output) {
    if (output.spawners.size() != facts.spawners.size() || output.rules.size() != facts.rules.size()
        || output.descriptors.size() != facts.descriptors.size()) {
        return false;
    }
    std::vector<PendingEdge> pending{};
    if (!build_pending_edges(topology, output, pending)) {
        return false;
    }
    output.edges.reserve(pending.size());
    for (PendingEdge& source : pending) {
        const std::uint32_t edgeRow = static_cast<std::uint32_t>(output.edges.size());
        GraphEdge edge{};
        edge.id = std::move(source.id);
        edge.spawnerRow = source.spawnerRow;
        edge.ruleRow = source.ruleRow;
        edge.sourceDescriptorRow = source.sourceDescriptorRow;
        edge.targetDescriptorRow = source.targetDescriptors.front();
        edge.targetObjectRow = source.targetObjectRow;
        edge.targetSlotRow = source.targetSlotRow;
        edge.referenceMask = source.referenceMask;
        edge.targetDescriptors.first =
            static_cast<std::uint32_t>(output.edgeTargetDescriptors.size());
        edge.targetDescriptors.count = static_cast<std::uint32_t>(source.targetDescriptors.size());
        for (const std::uint32_t descriptorRow : source.targetDescriptors) {
            output.edgeTargetDescriptors.push_back({edgeRow, descriptorRow});
        }
        edge.scenarioContexts.first = static_cast<std::uint32_t>(output.edgeContexts.size());
        edge.scenarioContexts.count = static_cast<std::uint32_t>(source.scenarios.size());
        for (const std::uint32_t scenarioIndex : source.scenarios) {
            output.edgeContexts.push_back(
                {static_cast<std::uint32_t>(output.edgeContexts.size()), scenarioIndex, edgeRow});
        }
        edge.associationExact = source.associationExact;
        edge.sameObject = output.descriptors[edge.sourceDescriptorRow].objectIndex
                          == output.descriptors[edge.targetDescriptorRow].objectIndex;
        output.edges.push_back(std::move(edge));
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory::detail
