#pragma once

#include <span>

#include "../definition.h"

namespace sunrise::state::activity::mission {

/** Stable result from one exact mission-state operation. */
enum class Status : std::uint8_t {
    ready,
    invalidBinding,
    invalidProgram,
    programMismatch,
    revisionMismatch,
    inputSequenceMismatch,
    intentMismatch,
    outOfMemory,
    intentSequenceExhausted,
    variableCapacity,
    timerCapacity,
    invalidVariable,
    invalidTimer,
    hostRevisionMismatch,
    revisionExhausted,
    invalidTransition,
};

/** Value-owned mission state copied from one exact activity-session generation. */
struct Snapshot final {
    MissionState state{};
    std::uint64_t activityStateRevision{};
};

/** Durable accepted-input cursors used to prove whether a retained feed interval is complete. */
struct InputSequenceSnapshot final {
    std::uint64_t attemptGeneration{kFirstAttemptGeneration};
    std::uint64_t committed{};
    std::uint64_t issued{};
    bool faulted{};
};

/** Complete value candidates published together by one successful VM callback. */
struct CommitCandidate final {
    std::span<const ScriptVariable> variables{};
    std::span<const MissionTimer> timers{};
    std::span<const TypedIntent> pendingIntents{};
    std::uint64_t nextRevision{};
    std::uint64_t nextInputSequence{};
    std::uint64_t nextTimerSequence{kFirstTimerSequence};
    std::uint64_t nextIntentKey{kFirstIntentKey};
    std::uint32_t phase{};
    bool started{};
};

/** Binds a program once, or reads the existing exact program state. */
[[nodiscard]] Status
bind(const SessionBinding& binding, const ProgramKey& program, Snapshot& output) noexcept;

/**
 * Replaces the bound program identity while keeping the mission record.
 * Refuses unless the record is unfaulted, holds no pending intent, and its input cursors agree.
 * It changes nothing else, so a replacement can never mint a request key the old program spent.
 */
[[nodiscard]] Status rebind_program(const SessionBinding& binding,
                                    const ProgramKey& expected,
                                    const ProgramKey& replacement,
                                    Snapshot& output) noexcept;

/** Atomically issues the next durable per-binding Host mission-input sequence. */
[[nodiscard]] bool issue_input_sequence(const SessionBinding& binding,
                                        std::uint64_t& output,
                                        std::uint64_t* attemptGeneration = nullptr) noexcept;

/** Copies one exact mission record for a read-only view. */
[[nodiscard]] bool state_snapshot(const SessionBinding& binding, Snapshot& output) noexcept;

/** Reads one exact binding's durable committed/issued input cursors without binding a program. */
[[nodiscard]] bool input_sequence_snapshot(const SessionBinding& binding,
                                           InputSequenceSnapshot& output) noexcept;

/** Durably faults one exact binding after the retained Host input feed reports data loss. */
[[nodiscard]] Status fault_input_feed(const SessionBinding& binding, Snapshot& output) noexcept;

/**
 * Retires every issued input of a binding no program is bound to.
 * Inputs accepted before a program binds are the baseline, not callbacks, and `bind` retires them
 * the same way. A binding with no script never binds, so its Host feed would retain them for good.
 * @return `ready` when the cursors agree afterwards, `invalidTransition` when a program is bound.
 */
[[nodiscard]] Status retire_unbound_inputs(const SessionBinding& binding) noexcept;

/** Commits one VM transaction against its exact prior mission revision. */
[[nodiscard]] Status commit(const SessionBinding& binding,
                            const ProgramKey& program,
                            std::uint64_t expectedRevision,
                            std::uint64_t expectedInputSequence,
                            const CommitCandidate& transaction,
                            Snapshot& output) noexcept;

/** Assigns the exact next Host output revision to the durable head intent. */
[[nodiscard]] Status assign_intent_output(const SessionBinding& binding,
                                          const ProgramKey& program,
                                          std::uint64_t expectedMissionRevision,
                                          std::uint64_t expectedIntentSequence,
                                          std::uint64_t hostOutputRevision,
                                          Snapshot& output) noexcept;

/** Reads whether the exact durable head still owns one queued Host output revision. */
[[nodiscard]] bool intent_output_assigned(const SessionBinding& binding,
                                          std::uint64_t expectedIntentSequence,
                                          std::uint64_t expectedHostOutputRevision) noexcept;

/** Releases one output assignment while retaining the durable intent and optional parent reset. */
[[nodiscard]] Status
release_intent_output(const SessionBinding& binding,
                      const ProgramKey& program,
                      std::uint64_t expectedMissionRevision,
                      std::uint64_t expectedIntentSequence,
                      std::uint64_t expectedHostOutputRevision,
                      Snapshot& output,
                      const SquadPopulation* preparedParent = nullptr) noexcept;

/** Removes the durable head only after its exact Host output revision was staged. */
[[nodiscard]] Status
acknowledge_intent_output(const SessionBinding& binding,
                          const ProgramKey& program,
                          std::uint64_t expectedMissionRevision,
                          std::uint64_t expectedIntentSequence,
                          std::uint64_t expectedHostOutputRevision,
                          Snapshot& output,
                          const DeviceRequestReport* device = nullptr,
                          const SquadPopulation* population = nullptr,
                          std::span<const SquadPopulation> scenePopulations = {}) noexcept;

/** Retains count evidence only for the exact transported spawn generation. */
[[nodiscard]] Status observe_squad_population(const SessionBinding& binding,
                                              const ProgramKey& program,
                                              const SquadPopulationReport& report,
                                              Snapshot& output) noexcept;

/** Joins accepted device deltas to the exact staged requests they satisfy. */
[[nodiscard]] Status
observe_device_report(const SessionBinding& binding,
                      const ProgramKey& program,
                      const DeviceReport& report,
                      std::array<std::uint64_t, kDeviceChannelCount>& appliedRequests,
                      Snapshot& output) noexcept;

/** Re-arms matching device desires only after their Auth reaches a newer publication owner. */
[[nodiscard]] Status
renew_device_publications(const SessionBinding& binding,
                          const DevicePublicationBoundary& boundary,
                          std::span<const DevicePublication> publications) noexcept;

/** Removes one successfully applied local effect whose durable head owns no Host output. */
[[nodiscard]] Status acknowledge_intent(const SessionBinding& binding,
                                        const ProgramKey& program,
                                        std::uint64_t expectedMissionRevision,
                                        std::uint64_t expectedIntentSequence,
                                        Snapshot& output) noexcept;

/**
 * Drops the durable head after a request was refused.
 * The head must own no Host output revision. Use this instead of a fault when the request was
 * impossible but the runtime is still consistent.
 */
[[nodiscard]] Status discard_intent(const SessionBinding& binding,
                                    const ProgramKey& program,
                                    std::uint64_t expectedMissionRevision,
                                    std::uint64_t expectedIntentSequence,
                                    Snapshot& output) noexcept;
/** Marks an exact mission revision faulted without changing its phase. */
[[nodiscard]] Status fault(const SessionBinding& binding,
                           const ProgramKey& program,
                           std::uint64_t expectedRevision,
                           Snapshot& output) noexcept;

/** Clears one exact program fault and drops its uncommitted input cursor. */
[[nodiscard]] Status recover(const SessionBinding& binding,
                             const ProgramKey& program,
                             std::uint64_t expectedRevision,
                             Snapshot& output) noexcept;

/**
 * Value-owned publishable projection of one mission record, with no wire form.
 * It carries no pending intent and no request key. A pending intent names a Host output revision
 * owned by the publishing connection, and a request key never touches the wire.
 */
struct PublishedMissionState final {
    std::array<ScriptVariable, kVariableCapacity> variables{};
    std::array<MissionTimer, kTimerCapacity> timers{};
    /** Remaining milliseconds per timer, so a recipient rebuilds its own absolute deadline. */
    std::array<std::uint32_t, kTimerCapacity> timerRemainingMs{};
    std::uint64_t revision{};
    std::size_t variableCount{};
    std::size_t timerCount{};
    std::uint32_t phase{};
    bool started{};
};

/**
 * Projects one committed record into the value-owned form a publication would carry.
 * There is no encoder and no transport behind this. It exists so whatever publication is proved
 * reuses one shape.
 * @param now Service tick the timer remainders are measured from.
 * @return True when the record is bound, unfaulted and within every capacity.
 */
[[nodiscard]] bool project_for_publication(const Snapshot& snapshot,
                                           std::uint64_t now,
                                           PublishedMissionState& output) noexcept;

[[nodiscard]] const char* status_name(Status status) noexcept;

} // namespace sunrise::state::activity::mission
