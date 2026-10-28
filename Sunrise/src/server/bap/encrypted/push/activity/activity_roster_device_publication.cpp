#include "activity_roster_device_publication.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../../../state/activity/mission/runtime.h"
#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

namespace message = middleware::bap::activity_message::sensor_auth_update;
namespace scriptable = middleware::bap::activity_message::scriptable_auth;

/** Requires an encoded snapshot; retired slots and unknown channels yield no proof. */
bool collect_roster_device_publications(
    const message::Snapshot& snapshot,
    std::span<state::activity::mission::DevicePublication> output,
    std::size_t& count) noexcept {
    count = 0;
    if (snapshot.roster.groupCount > snapshot.roster.groups.size()) {
        return false;
    }
    for (const auto& value : snapshot.authOverrides) {
        if (!value.present || value.slotType != scriptable::kType23SlotType
            || value.authSchema != scriptable::kType23Schema
            || value.bitCount != scriptable::kType23BitCount
            || value.byteCount > value.body.size()) {
            continue;
        }
        bool included = false;
        for (const auto& group :
             std::span(snapshot.roster.groups).first(snapshot.roster.groupCount)) {
            if (group.retired || group.objectTag != value.objectTag || group.key != value.key
                || group.slotTypes.size() != group.slotIndices.size()
                || group.slotTypes.size() != group.slotFlags.size()) {
                continue;
            }
            for (std::size_t index = 0; index < group.slotTypes.size(); ++index) {
                included = included
                           || (group.slotTypes[index] == value.slotType
                               && group.slotIndices[index] == value.slotIndex
                               && (group.slotFlags[index] & message::kSlotAuthFlag) != 0);
            }
        }
        scriptable::Type23Body body{};
        if (!included
            || !scriptable::decode_type23_body(std::span(value.body).first(value.byteCount),
                                               body)) {
            continue;
        }
        state::activity::mission::DevicePublication publication{};
        publication.objectTag = value.objectTag;
        publication.registryKey = value.key;
        publication.slotIndex = value.slotIndex;
        publication.originatingHostRevision = value.originatingHostRevision;
        bool known = false;
        for (std::size_t index = 0; index < body.channels.size(); ++index) {
            const auto& channel = body.channels[index];
            if (channel.sequence <= 0 || !std::isfinite(channel.desiredValue)
                || channel.desiredValue < 0.0F || channel.desiredValue > 1.0F) {
                continue;
            }
            publication.channels[index] = {channel.desiredValue, channel.sequence, true, true};
            known = true;
        }
        if (!known) {
            continue;
        }
        if (count == output.size()) {
            return false;
        }
        output[count++] = publication;
    }
    return true;
}

/** Captured boundaries and body proofs share the roster's commit or discard outcome. */
bool stage_roster_device_publications(Session& session,
                                      Scratch& scratch,
                                      const message::Snapshot& snapshot) noexcept {
    std::size_t count = 0;
    state::activity::mission::DevicePublicationBoundary boundary{};
    if (!collect_roster_device_publications(snapshot, scratch.rosterDevicePublications, count)) {
        return false;
    }
    if (count != 0
        && server::activity::host::publication_input_boundary(session.activity.session,
                                                              boundary.attemptGeneration,
                                                              boundary.inputSequence,
                                                              boundary.clientMessageSequence)) {
        boundary.sourceGeneration = session.activity.bindingGeneration;
    }
    auto& staged = session.activityRosterStaged;
    std::copy_n(scratch.rosterDevicePublications.begin(), count, staged.devicePublications.begin());
    staged.devicePublicationCount = static_cast<std::uint16_t>(count);
    staged.devicePublicationBoundary = boundary;
    staged.devicePublicationBinding = session.activity.session;
    return true;
}

/** Publication failures fault the mission instead of retaining stale report ownership. */
void commit_roster_device_publications(const Session& session) noexcept {
    const auto& staged = session.activityRosterStaged;
    if (staged.devicePublicationCount != 0
        && staged.devicePublicationCount <= staged.devicePublications.size()
        && staged.devicePublicationBoundary.sourceGeneration == staged.bindingGeneration
        && state::activity::same_binding(staged.devicePublicationBinding,
                                         session.activity.session)) {
        const auto status = state::activity::mission::renew_device_publications(
            staged.devicePublicationBinding,
            staged.devicePublicationBoundary,
            std::span(staged.devicePublications).first(staged.devicePublicationCount));
        if (status != state::activity::mission::Status::ready) {
            state::activity::mission::Snapshot fault{};
            const auto faultStatus =
                state::activity::mission::fault_input_feed(staged.devicePublicationBinding, fault);
            std::array<char, core::log::kLineCapacity> line{};
            const int length =
                std::snprintf(line.data(),
                              line.size(),
                              "ev=activity stage=device_publication result=%s fault=%s",
                              state::activity::mission::status_name(status),
                              state::activity::mission::status_name(faultStatus));
            if (length > 0 && static_cast<std::size_t>(length) < line.size()) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::error,
                                 {line.data(), static_cast<std::size_t>(length)});
            }
        }
    }
}

} // namespace sunrise::server::bap::encrypted::push::activity
