/**
 * Owned client-message framing metadata and its bounded scalar decodes.
 * Helpers here need the Host runtime lock; the entry points take it themselves.
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../middleware/bap/activity_message/client_authoritative_data.h"
#include "../../state/activity/runtime.h"
#include "../../state/activity_sdk/runtime.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace {

using namespace detail;

std::array<ClientMessageRecord, kClientMessageHistoryCapacity> g_clientMessages{};
std::array<ClientMessageDetail, kClientMessageDetailCapacity> g_clientMessageDetails{};
std::size_t g_clientMessageStart{};
std::size_t g_clientMessageCount{};
std::size_t g_clientMessageDetailStart{};
std::size_t g_clientMessageDetailCount{};
std::uint64_t g_clientMessageSequence{};
std::uint64_t g_overwrittenClientMessages{};

/** Appends framing metadata without entering or draining the reducer queue. */
[[nodiscard]] std::uint64_t append_client_message(ClientMessageRecord record) noexcept {
    g_clientMessageSequence = next_nonzero(g_clientMessageSequence);
    record.sequence = g_clientMessageSequence;
    std::size_t index = (g_clientMessageStart + g_clientMessageCount) % g_clientMessages.size();
    if (g_clientMessageCount == g_clientMessages.size()) {
        index = g_clientMessageStart;
        g_clientMessageStart = (g_clientMessageStart + 1) % g_clientMessages.size();
        ++g_overwrittenClientMessages;
    } else {
        ++g_clientMessageCount;
    }
    g_clientMessages[index] = record;
    return record.sequence;
}

/** Appends one bounded decode in oldest-to-newest ring order. */
void append_client_message_detail(
    std::uint64_t sequence,
    std::uint32_t messageType,
    const middleware::bap::activity_message::sense_update::DecodedPacket* sense) noexcept {
    std::size_t index =
        (g_clientMessageDetailStart + g_clientMessageDetailCount) % g_clientMessageDetails.size();
    if (g_clientMessageDetailCount == g_clientMessageDetails.size()) {
        index = g_clientMessageDetailStart;
        g_clientMessageDetailStart =
            (g_clientMessageDetailStart + 1) % g_clientMessageDetails.size();
    } else {
        ++g_clientMessageDetailCount;
    }
    ClientMessageDetail& detail = g_clientMessageDetails[index];
    detail = {};
    detail.sequence = sequence;
    detail.messageType = messageType;
    if (sense != nullptr) {
        detail.sense = *sense;
        detail.hasSenseDecode = true;
    }
}

} // namespace

namespace detail {

/** The Host lock keeps the framing intake head ordered with a borrowed output. */
std::uint64_t latest_client_message_sequence() noexcept {
    return g_clientMessageSequence;
}

/** Publishes one safe generic client envelope into the ordered mission-input feed. */
void apply_client_message(const ClientMessageMissionInput& input, std::uint64_t now) noexcept {
    Instance* const instance = find_instance(input.binding);
    if (instance == nullptr || !instance->view.active) {
        ++g_droppedIngress;
        return;
    }
    touch(*instance);
    Event event{};
    event.attemptGeneration = input.attemptGeneration;
    event.binding = input.binding;
    event.tick = now;
    event.kind = EventKind::clientMessageReceived;
    event.stateRevision = instance->view.stateRevision;
    event.sourceGeneration = input.sourceGeneration;
    event.clientMessageSequence = input.clientMessageSequence;
    event.payloadBytes = input.payloadBytes;
    event.peerHeardMask = input.peerHeardMask;
    event.consumedBits = input.consumedBits;
    event.clientMessageType = input.messageType;
    event.clientMessageStatus = input.status;
    stamp_mission_sequence(event);
    ClientMessageSnapshot snapshot{};
    snapshot.messageType = input.messageType;
    snapshot.status = input.status;
    append_mission_input(event, nullptr, &snapshot);
}

/** Copies the retained framing history and its counter into the diagnostic view. */
void snapshot_client_messages(DiagnosticsSnapshot& output) noexcept {
    for (std::size_t index = 0; index < g_clientMessageCount; ++index) {
        output.clientMessages[index] =
            g_clientMessages[(g_clientMessageStart + index) % g_clientMessages.size()];
    }
    output.clientMessageCount = g_clientMessageCount;
    output.overwrittenClientMessages = g_overwrittenClientMessages;
}

/** Clears the framing history, the bounded decodes and their counters. */
void reset_client_messages() noexcept {
    for (ClientMessageRecord& message : g_clientMessages) {
        message = {};
    }
    SecureZeroMemory(g_clientMessageDetails.data(), sizeof(g_clientMessageDetails));
    g_clientMessageStart = 0;
    g_clientMessageCount = 0;
    g_clientMessageDetailStart = 0;
    g_clientMessageDetailCount = 0;
    g_clientMessageSequence = 0;
    g_overwrittenClientMessages = 0;
}

} // namespace detail

/** Retains one owned client message without entering the reducer queue. */
std::uint64_t record_client_message(
    const ClientMessageInput& input,
    const middleware::bap::activity_message::sense_update::DecodedPacket* sense) noexcept {
    if (!state::activity::binding_matches(input.binding)) {
        return 0;
    }
    ClientMessageRecord record{};
    record.binding.sessionId = input.binding.sessionId;
    record.binding.createdRevision = input.binding.createdRevision;
    record.authoritative = input.authoritative;
    record.tick = GetTickCount64();
    record.sourceGeneration = input.sourceGeneration;
    record.payloadFingerprint = input.payloadFingerprint;
    record.messageType = input.messageType;
    record.payloadBytes = input.payloadBytes;
    record.peerHeardMask = input.peerHeardMask;
    record.consumedBits = input.consumedBits;
    record.status = input.status;
    record.hasPayloadFingerprint = input.hasPayloadFingerprint;
    record.hasAuthoritative = input.hasAuthoritative;
    AcquireSRWLockExclusive(&g_lock);
    const std::uint64_t sequence = append_client_message(record);
    if (sense != nullptr) {
        append_client_message_detail(sequence, input.messageType, sense);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return sequence;
}

/** Queues one owned client envelope without a richer typed mission reducer. */
bool submit_client_message(const ClientMessageInput& input,
                           std::uint64_t clientMessageSequence) noexcept {
    namespace activity_message = middleware::bap::activity_message;
    namespace communication = activity_message::wire_schema::communication;
    communication::ActivityCommunicationRoute route{};
    const bool executableRoute =
        state::activity_sdk::executable_communication_route(input.messageType, route);
    if (!state::activity::binding_matches(input.binding) || input.sourceGeneration == 0
        || clientMessageSequence == 0 || !executableRoute
        || route.ingressDelivery != communication::IngressDeliveryPolicy::protocolHostInput
        || (route.ingressClass != communication::IngressClass::nativeMetadataOnly
            && route.ingressClass != communication::IngressClass::nativeParsed)
        || input.messageType == activity_message::sense_update::kMessageType
        || input.messageType == activity_message::incident::kMessageType
        || input.messageType == activity_message::client_authoritative_data::kMessageType) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    PendingInput pending{};
    pending.kind = PendingKind::clientMessage;
    pending.clientMessage.binding = input.binding;
    pending.clientMessage.sourceGeneration = input.sourceGeneration;
    pending.clientMessage.clientMessageSequence = clientMessageSequence;
    pending.clientMessage.messageType = input.messageType;
    pending.clientMessage.payloadBytes = input.payloadBytes;
    pending.clientMessage.peerHeardMask = input.peerHeardMask;
    pending.clientMessage.consumedBits = input.consumedBits;
    pending.clientMessage.status = input.status;
    if (!append_pending(pending)) {
        ++g_droppedIngress;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    ++g_queuedIngress;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Copies one retained scalar decode by its ingress sequence. */
bool client_message_detail(std::uint64_t sequence, ClientMessageDetail& output) noexcept {
    output = {};
    if (sequence == 0) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (std::size_t offset = g_clientMessageDetailCount; offset != 0; --offset) {
        const std::size_t index =
            (g_clientMessageDetailStart + offset - 1) % g_clientMessageDetails.size();
        if (g_clientMessageDetails[index].sequence == sequence) {
            output = g_clientMessageDetails[index];
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

} // namespace sunrise::server::activity::host
