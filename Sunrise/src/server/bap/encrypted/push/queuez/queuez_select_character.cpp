#include <array>
#include <cstdio>
#include <optional>

#include "../../../../../core/logging/log.h"
#include "../../queuez/queuez_state_validation.h"
#include "../snapshot/internal.h"
#include "queuez_update_frame.h"

namespace sunrise::server::bap::encrypted::push {

/** Appends the opcode-504 Family-4 character-selection update. */
bool append_select_character_notification(Scratch& scratch,
                                          const queuez::SelectCharacter& select,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::span<const std::byte, state::kBapNonceSize> nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_selection_move(scratch, select, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends the opcode-403 character upsert as one increment above the current peer version. */
bool append_equipment_swap_notification(
    Scratch& scratch,
    const queuez::EquipmentSwap& swap,
    const state::PendingEquipmentSwap& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_equipment_swap(
               scratch, swap, mutation, acquisitionPresentationRows, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends one opcode-406 selected-character item-state upsert. */
bool append_item_state_notification(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    const state::PendingItemState& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_item_state(
               scratch, update, mutation, acquisitionPresentationRows, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends one opcode-901 selected-character artifact ownership upsert. */
bool append_artifact_purchase_notification(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    const state::PendingArtifactPurchase& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_artifact_purchase(
               scratch, update, mutation, acquisitionPresentationRows, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends a reset increment without the full-snapshot acquisition semantics. */
bool append_artifact_reset_notification(Scratch& scratch,
                                        const queuez::EquipmentSwap& update,
                                        std::span<const std::byte, state::kAesKeySize> key,
                                        std::span<const std::byte, state::kBapNonceSize> nonce,
                                        std::span<std::byte> response,
                                        std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_artifact_reset(scratch, update, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends one exact resident upsert after artifact reset. */
bool append_artifact_item_refresh_notification(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    std::uint64_t instanceSoid,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_artifact_item_refresh(scratch, update, instanceSoid, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends one socket item upsert and its charged account balances when required. */
bool append_socket_plug_notification(Scratch& scratch,
                                     const queuez::SocketPlug& socketPlug,
                                     const state::PendingSocketPlug& mutation,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::span<const std::byte, state::kBapNonceSize> nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_socket_plug(scratch, socketPlug, mutation, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends one subclass item-instance upsert after an opcode-801 selection. */
bool append_subclass_selection_notification(Scratch& scratch,
                                            const queuez::SubclassSelection& selection,
                                            const state::PendingSubclassSelection& mutation,
                                            std::span<const std::byte, state::kAesKeySize> key,
                                            std::span<const std::byte, state::kBapNonceSize> nonce,
                                            std::span<std::byte> response,
                                            std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_subclass_selection(scratch, selection, mutation, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends one atomic new-instance-before-character Family-4 acquisition update. */
bool append_item_acquisition_notification(
    Scratch& scratch,
    const queuez::ItemAcquisition& acquisition,
    const state::PendingItemAcquisition& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_item_acquisition(
               scratch, acquisition, mutation, acquisitionPresentationRows, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends an optional new profile resident followed by the full account after-image. */
bool append_profile_item_acquisition_notification(
    Scratch& scratch,
    const queuez::ProfileItemAcquisition& acquisition,
    const state::PendingProfileItemAcquisition& mutation,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_profile_item_acquisition(scratch, acquisition, mutation, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Publishes the transient XP row and its matching account progression in one increment. */
bool append_seasonal_experience_notification(
    Scratch& scratch,
    const queuez::SessionState& before,
    std::int32_t amount,
    std::int32_t mutationSerial,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written,
    queuez::SessionState& after) noexcept {
    after = before;
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_seasonal_experience_presentation(
            scratch, before, amount, mutationSerial, acquisitionPresentationRows, prepared)
        || !queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written)) {
        return false;
    }
    after.family4Version = prepared.family.version;
    if (!queuez::valid(after)) {
        after = before;
        return false;
    }
    return true;
}

/** Publishes every item in one rank-one class package as an ordinary acquisition. */
bool append_season_pass_package_notification(
    Scratch& scratch,
    const queuez::SessionState& before,
    const state::PendingDirectItemBundle& mutation,
    std::uint16_t rewardIndex,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written,
    queuez::SessionState& after) noexcept {
    after = before;
    snapshot::Prepared prepared{};
    const std::size_t itemCount = mutation.itemCount;
    // The item objects are indexed below, and every new resident must fit the peer's manifest.
    if (!snapshot::prepare_season_pass_package(
            scratch, before, mutation, rewardIndex, acquisitionPresentationRows, prepared)
        || itemCount == 0 || prepared.family.objects.size() != itemCount + 2U
        || before.family4ResidentCount + itemCount > before.family4Residents.size()) {
        return false;
    }
    for (std::size_t index = 0; index < itemCount; ++index) {
        const middleware::queuez::Object& object = prepared.family.objects[index];
        for (std::size_t residentIndex = 0; residentIndex < before.family4ResidentCount;
             ++residentIndex) {
            if (before.family4Residents[residentIndex].objectSoid == object.version) {
                return false;
            }
        }
        after.family4Residents[after.family4ResidentCount++] =
            queuez::ResidentObject{object.version, object.id};
    }
    const middleware::queuez::Object& characterObject = prepared.family.objects[itemCount];
    std::size_t characterMatches = 0;
    for (std::size_t index = 0; index < before.family4ResidentCount; ++index) {
        const queuez::ResidentObject& resident = before.family4Residents[index];
        characterMatches +=
            static_cast<std::size_t>(resident.objectSoid == characterObject.version
                                     && resident.definitionId == characterObject.id);
    }
    if (characterMatches != 1) {
        return false;
    }
    after.family4Version = prepared.family.version;
    if (!queuez::valid(after)
        || !queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written)) {
        after = before;
        return false;
    }
    return true;
}

/** Publishes one prepared record-reward batch. */
bool append_record_reward_notification(
    Scratch& scratch,
    const queuez::SessionState& before,
    const queuez::RecordRewardGrant& update,
    const state::PendingRecordRewardGrant& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_record_reward_grant(
               scratch, before, update, mutation, acquisitionPresentationRows, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

/** Appends one atomic dismantle update, including any account-wide material payout. */
bool append_item_dismantle_notification(Scratch& scratch,
                                        const queuez::ItemDismantle& dismantle,
                                        const state::PendingItemDismantle& mutation,
                                        std::span<const std::byte, state::kAesKeySize> key,
                                        std::span<const std::byte, state::kBapNonceSize> nonce,
                                        std::span<std::byte> response,
                                        std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    return snapshot::prepare_item_dismantle(scratch, dismantle, mutation, prepared)
           && queuez_frame::append_prepared(scratch, prepared, key, nonce, response, written);
}

} // namespace sunrise::server::bap::encrypted::push
