#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../state/activity_sdk/format.h"
#include "mission_script_lua_bap_internal.h"
#include "mission_script_lua_catalog_internal.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_manifest_internal.h"
#include "mission_script_lua_world_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace format = state::activity_sdk::format;

namespace {
/** Maps one message direction to its Lua spelling. */
[[nodiscard]] const char* direction_name(std::uint32_t value) noexcept {
    using Direction = state::activity_sdk::format::ActivityMessageDirection;
    switch (static_cast<Direction>(value)) {
    case Direction::remoteToClient:
        return "remote_to_client";
    case Direction::clientToRemote:
        return "client_to_remote";
    case Direction::bidirectional:
        return "bidirectional";
    case Direction::clientToRemoteSpecial:
        return "client_to_remote_special";
    case Direction::nameOnly:
        return "name_only";
    }
    return nullptr;
}

/** Maps one wire coverage grade to its Lua spelling. */
[[nodiscard]] const char* coverage_name(std::uint32_t value) noexcept {
    using Coverage = state::activity_sdk::format::ActivityMessageCoverage;
    switch (static_cast<Coverage>(value)) {
    case Coverage::fixedWireExact:
        return "fixed_wire_exact";
    case Coverage::customWireExact:
        return "custom_wire_exact";
    case Coverage::partialDynamicBody:
        return "partial_dynamic_body";
    case Coverage::variableWire:
        return "variable_wire";
    case Coverage::serviceConversion:
        return "service_conversion";
    case Coverage::nameOnly:
        return "name_only";
    }
    return nullptr;
}

[[nodiscard]] const char* call_form_name(std::uint32_t value) noexcept {
    using CallForm = state::activity_sdk::format::ActivityMessageCallForm;
    switch (static_cast<CallForm>(value)) {
    case CallForm::direct:
        return "direct";
    case CallForm::deltaRootBit:
        return "delta_root_bit";
    }
    return nullptr;
}

/** Maps one definition provenance to its Lua spelling. */
[[nodiscard]] const char* definition_state_name(std::uint32_t value) noexcept {
    using DefinitionState = state::activity_sdk::format::ActivityMessageDefinitionState;
    switch (static_cast<DefinitionState>(value)) {
    case DefinitionState::none:
        return "none";
    case DefinitionState::authored:
        return "authored";
    case DefinitionState::graph:
        return "graph";
    }
    return nullptr;
}

[[nodiscard]] const char* field_source_name(std::uint32_t value) noexcept {
    using Source = state::activity_sdk::format::ActivityMessageFieldSource;
    switch (static_cast<Source>(value)) {
    case Source::graph:
        return "graph";
    case Source::authored:
        return "authored";
    }
    return nullptr;
}

/** Maps one field name evidence grade to its Lua spelling. */
[[nodiscard]] const char* field_confidence_name(std::uint32_t value) noexcept {
    using Confidence = state::activity_sdk::format::ActivityMessageFieldConfidence;
    switch (static_cast<Confidence>(value)) {
    case Confidence::verified:
        return "verified";
    case Confidence::assumed:
        return "assumed";
    case Confidence::unnamed:
        return "unnamed";
    }
    return nullptr;
}

/** Maps one operator exposure grade to its Lua spelling. */
[[nodiscard]] const char* field_exposure_name(std::uint32_t value) noexcept {
    using Exposure = state::activity_sdk::format::ActivityMessageFieldExposure;
    switch (static_cast<Exposure>(value)) {
    case Exposure::redacted:
        return "redacted";
    case Exposure::operatorValue:
        return "operator_value";
    case Exposure::provisionalValue:
        return "provisional_value";
    }
    return nullptr;
}

/** @return The numeric exposure grade the field row's flag bits grant. */
[[nodiscard]] std::uint32_t field_exposure_code(std::uint32_t flags) noexcept {
    return static_cast<std::uint32_t>(state::activity_sdk::format::field_exposure(flags));
}

void push_optional_u32(lua_State* state, std::uint32_t value) {
    if (value == state::activity_sdk::format::kAbsentIndex) {
        lua_pushnil(state);
    } else {
        lua_pushinteger(state, value);
    }
}

void push_optional_i64(lua_State* state, std::int64_t value) {
    if (value == state::activity_sdk::format::kAbsentSignedValue) {
        lua_pushnil(state);
    } else {
        lua_pushinteger(state, static_cast<lua_Integer>(value));
    }
}

void push_optional_name(lua_State* state, const char* value) {
    if (value == nullptr) {
        lua_pushnil(state);
    } else {
        lua_pushstring(state, value);
    }
}

[[nodiscard]] std::size_t
ingress_surface_count(const ActivityMessageDefinition& definition) noexcept {
    return definition.ingressSurfaceCount <= definition.ingressSurfaces.size()
               ? definition.ingressSurfaceCount
               : 0;
}

[[nodiscard]] std::size_t
egress_surface_count(const ActivityMessageDefinition& definition) noexcept {
    return definition.egressSurfaceCount <= definition.egressSurfaces.size()
               ? definition.egressSurfaceCount
               : 0;
}

void push_message_surfaces(lua_State* state,
                           std::span<const ActivityMessageSurfaceDefinition> surfaces) {
    lua_createtable(state, static_cast<int>(surfaces.size()), 0);
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        lua_createtable(state, 0, 2);
        set_string(state, "id", surfaces[index].id);
        set_string(state, "lua_name", surfaces[index].luaName);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
}

/** Pushes one route plus only its catalog-authorized executable Lua surface. */
void push_message_communication(lua_State* state, const ActivityMessageDefinition& definition) {
    namespace format = state::activity_sdk::format;
    namespace communication = middleware::bap::activity_message::wire_schema::communication;
    communication::ActivityCommunicationRoute route{};
    const bool executable = executable_message_route(definition, route);
    const std::string_view receive = executable ? receive_api_name(route) : "none";
    const bool send = executable && send_api_available(route);
    lua_createtable(state, 0, 28);
    set_boolean(state, "can_receive", receive != "none");
    set_boolean(state, "can_send", send);
    set_boolean(state,
                "data_only",
                (definition.communicationFlags & format::kActivityCommunicationDataOnly) != 0);
    set_string(state, "egress_adapter", definition.egressAdapter);
    set_string(state, "egress_adapter_path", definition.egressAdapterPath);
    set_string(state, "egress_class", definition.egressClass);
    set_string(state, "egress_delivery", definition.egressDelivery);
    set_string(state, "egress_status", definition.egressStatus);
    set_boolean(state, "executable", executable);
    set_string(state, "ingress_adapter", definition.ingressAdapter);
    set_string(state, "ingress_adapter_path", definition.ingressAdapterPath);
    set_string(state, "ingress_class", definition.ingressClass);
    set_string(state, "ingress_delivery", definition.ingressDelivery);
    set_string(state, "ingress_status", definition.ingressStatus);
    set_string(state, "late_join_handoff", definition.lateJoinHandoff);
    set_string(state, "lua_exposure", definition.luaExposure);
    set_integer(state, "message_id", definition.messageId);
    set_integer(state, "message_index", definition.localRow - 1);
    set_string(state, "output_codec", definition.outputCodec);
    set_string(state, "output_codec_path", definition.outputCodecPath);
    set_string(state, "receive_api", receive);
    push_message_surfaces(
        state, std::span(definition.ingressSurfaces).first(ingress_surface_count(definition)));
    lua_setfield(state, -2, "receive_surfaces");
    set_string(state, "send_api", send ? "typed_actions" : "none");
    push_message_surfaces(
        state, std::span(definition.egressSurfaces).first(egress_surface_count(definition)));
    lua_setfield(state, -2, "send_surfaces");
    set_string(state, "state_owner", definition.stateOwner);
    set_string(state, "state_owner_path", definition.stateOwnerPath);
    set_integer(state, "typed_lua_surface_count", definition.typedLuaSurfaceCount);
}

/** Maps one activity join status to its Lua spelling. */
[[nodiscard]] const char* activity_join_status_name(std::uint32_t value) noexcept {
    switch (static_cast<format::ActivityJoinStatus>(value)) {
    case format::ActivityJoinStatus::exact:
        return "exact";
    case format::ActivityJoinStatus::liveNameMissing:
        return "live_name_missing";
    case format::ActivityJoinStatus::sourceNameMissing:
        return "source_name_missing";
    case format::ActivityJoinStatus::liveNameAmbiguous:
        return "live_name_ambiguous";
    }
    return nullptr;
}

/** Maps one binding disposition to its Lua spelling. */
[[nodiscard]] const char* activity_binding_disposition_name(std::uint32_t value) noexcept {
    switch (static_cast<format::ActivityBindingDisposition>(value)) {
    case format::ActivityBindingDisposition::fixedScenario:
        return "fixed_scenario";
    case format::ActivityBindingDisposition::namedDefinitionUnavailable:
        return "named_definition_unavailable";
    case format::ActivityBindingDisposition::noDirectFixedActivityName:
        return "no_direct_fixed_activity_name";
    case format::ActivityBindingDisposition::unresolvedRunnable:
        return "unresolved_runnable";
    }
    return nullptr;
}

/** Maps one binding reason to its Lua spelling. */
[[nodiscard]] const char* activity_binding_reason_name(std::uint32_t value) noexcept {
    switch (static_cast<format::ActivityBindingReason>(value)) {
    case format::ActivityBindingReason::exactActivityRootScenarioEdge:
        return "exact_activity_root_scenario_edge";
    case format::ActivityBindingReason::installedRouteAbsent:
        return "installed_route_absent";
    case format::ActivityBindingReason::noDirectFixedActivityName:
        return "no_direct_fixed_activity_name";
    case format::ActivityBindingReason::activityRootNameAmbiguous:
        return "activity_root_name_ambiguous";
    case format::ActivityBindingReason::activityRootEdgeMissing:
        return "activity_root_edge_missing";
    }
    return nullptr;
}

/** Maps one binding evidence basis to its Lua spelling. */
[[nodiscard]] const char* activity_binding_evidence_basis_name(std::uint32_t value) noexcept {
    switch (static_cast<format::ActivityBindingEvidenceBasis>(value)) {
    case format::ActivityBindingEvidenceBasis::effectiveActivityRootNamePlusPayloadScenarioEdge:
        return "effective_activity_root_name_plus_payload_scenario_edge";
    case format::ActivityBindingEvidenceBasis::effectiveActivityAndScenarioRootNameCensus:
        return "effective_activity_and_scenario_root_name_census";
    case format::ActivityBindingEvidenceBasis::activityRecordInternalNameEmpty:
        return "activity_record_internal_name_empty";
    case format::ActivityBindingEvidenceBasis::effectiveActivityRootNameCensus:
        return "effective_activity_root_name_census";
    }
    return nullptr;
}

/** Maps one runnable status to its Lua spelling. */
[[nodiscard]] const char* activity_runnable_status_name(std::uint32_t value) noexcept {
    switch (static_cast<format::ActivityRunnableStatus>(value)) {
    case format::ActivityRunnableStatus::fixedScenarioBound:
        return "fixed_scenario_bound";
    case format::ActivityRunnableStatus::unavailableInInstalledEstate:
        return "unavailable_in_installed_estate";
    case format::ActivityRunnableStatus::fixedScenarioNotApplicable:
        return "fixed_scenario_not_applicable";
    case format::ActivityRunnableStatus::unresolved:
        return "unresolved";
    }
    return nullptr;
}

enum class ActivityBindingMemberResult : std::uint8_t {
    notOwned,
    pushed,
    stale,
};

/**
 * Pushes one activity-binding member.
 * @return Whether the key was owned here, and whether the binding was still resolvable.
 */
[[nodiscard]] ActivityBindingMemberResult push_activity_binding_member(lua_State* state,
                                                                       std::string_view key) {
    const bool owned = key == "activity_root_candidate_tags"
                       || key == "scenario_name_candidate_tags" || key == "evidence_root_tags"
                       || key == "binding_locators" || key == "internal_name"
                       || key == "display_name" || key == "join_status" || key == "join_status_code"
                       || key == "binding_disposition" || key == "binding_disposition_code"
                       || key == "binding_reason" || key == "binding_reason_code"
                       || key == "binding_evidence_basis" || key == "binding_evidence_basis_code"
                       || key == "runnable_status" || key == "runnable_status_code"
                       || key == "binding_full_sdk_acceptable" || key == "has_internal_name"
                       || key == "has_matchmaking_config" || key == "selected_activity_root_tag"
                       || key == "selected_scenario_tag" || key == "matchmaking_config_tag";
    if (!owned) {
        return ActivityBindingMemberResult::notOwned;
    }
    ActivityBindingDefinition definition{};
    if (!current_activity_binding(state, definition)) {
        return ActivityBindingMemberResult::stale;
    }
    if (key == "activity_root_candidate_tags") {
        push_activity_binding_tag_collection(state, ActivityBindingTagKind::activityRootCandidates);
    } else if (key == "scenario_name_candidate_tags") {
        push_activity_binding_tag_collection(state, ActivityBindingTagKind::scenarioNameCandidates);
    } else if (key == "evidence_root_tags") {
        push_activity_binding_tag_collection(state, ActivityBindingTagKind::evidenceRoots);
    } else if (key == "binding_locators") {
        push_activity_binding_locator_collection(state);
    } else if (key == "internal_name") {
        if (definition.internalName.empty()) {
            lua_pushnil(state);
        } else {
            lua_pushlstring(state, definition.internalName.data(), definition.internalName.size());
        }
    } else if (key == "display_name") {
        if (definition.displayName.empty()) {
            lua_pushnil(state);
        } else {
            lua_pushlstring(state, definition.displayName.data(), definition.displayName.size());
        }
    } else if (key == "join_status") {
        push_optional_name(state, activity_join_status_name(definition.joinStatus));
    } else if (key == "join_status_code") {
        lua_pushinteger(state, definition.joinStatus);
    } else if (key == "binding_disposition") {
        push_optional_name(state, activity_binding_disposition_name(definition.bindingDisposition));
    } else if (key == "binding_disposition_code") {
        lua_pushinteger(state, definition.bindingDisposition);
    } else if (key == "binding_reason") {
        push_optional_name(state, activity_binding_reason_name(definition.bindingReason));
    } else if (key == "binding_reason_code") {
        lua_pushinteger(state, definition.bindingReason);
    } else if (key == "binding_evidence_basis") {
        push_optional_name(state,
                           activity_binding_evidence_basis_name(definition.bindingEvidenceBasis));
    } else if (key == "binding_evidence_basis_code") {
        lua_pushinteger(state, definition.bindingEvidenceBasis);
    } else if (key == "runnable_status") {
        push_optional_name(state, activity_runnable_status_name(definition.runnableStatus));
    } else if (key == "runnable_status_code") {
        lua_pushinteger(state, definition.runnableStatus);
    } else if (key == "binding_full_sdk_acceptable") {
        lua_pushboolean(state, definition.fullSdkAcceptable ? 1 : 0);
    } else if (key == "has_internal_name") {
        lua_pushboolean(state, definition.hasInternalName ? 1 : 0);
    } else if (key == "has_matchmaking_config") {
        lua_pushboolean(state, definition.hasMatchmakingConfig ? 1 : 0);
    } else if (key == "selected_activity_root_tag") {
        push_optional_u32(state, definition.selectedActivityRootTag);
    } else if (key == "selected_scenario_tag") {
        push_optional_u32(state, definition.selectedScenarioTag);
    } else {
        push_optional_u32(state, definition.matchmakingConfigTag);
    }
    return ActivityBindingMemberResult::pushed;
}

} // namespace

/** Reads one activity-message row: identity, wire shape, fields, and communication. */
[[nodiscard]] int message_index(lua_State* state) {
    const auto* const handle =
        static_cast<const MessageHandle*>(luaL_checkudata(state, 1, kMessageMetatable));
    ActivityMessageDefinition definition{};
    if (!current_message(state, *handle, definition)) {
        return luaL_error(state, "activity message is stale");
    }
    namespace communication = middleware::bap::activity_message::wire_schema::communication;
    communication::ActivityCommunicationRoute route{};
    const bool executable = executable_message_route(definition, route);
    const std::string_view receive = executable ? receive_api_name(route) : "none";
    const bool send = executable && send_api_available(route);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "row") {
        lua_pushinteger(state, definition.localRow);
    } else if (key == "id") {
        lua_pushinteger(state, definition.messageId);
    } else if (key == "name") {
        lua_pushlstring(state, definition.name.data(), definition.name.size());
    } else if (key == "direction") {
        push_optional_name(state, direction_name(definition.direction));
    } else if (key == "direction_code") {
        lua_pushinteger(state, definition.direction);
    } else if (key == "coverage") {
        push_optional_name(state, coverage_name(definition.coverage));
    } else if (key == "coverage_code") {
        lua_pushinteger(state, definition.coverage);
    } else if (key == "definition_handle") {
        push_optional_u32(state, definition.definitionHandle);
    } else if (key == "call_form") {
        push_optional_name(state, call_form_name(definition.callForm));
    } else if (key == "call_form_code") {
        lua_pushinteger(state, definition.callForm);
    } else if (key == "definition_state") {
        push_optional_name(state, definition_state_name(definition.definitionState));
    } else if (key == "definition_state_code") {
        lua_pushinteger(state, definition.definitionState);
    } else if (key == "definition_struct_size") {
        push_optional_u32(state, definition.definitionStructSize);
    } else if (key == "wire_min_bits") {
        push_optional_u32(state, definition.wireMinBits);
    } else if (key == "wire_max_bits") {
        push_optional_u32(state, definition.wireMaxBits);
    } else if (key == "field_count") {
        lua_pushinteger(state, definition.fieldCount);
    } else if (key == "named_field_count") {
        lua_pushinteger(state, definition.namedFieldCount);
    } else if (key == "graph_field_count") {
        lua_pushinteger(state, definition.graphFieldCount);
    } else if (key == "authored_field_count") {
        lua_pushinteger(state, definition.authoredFieldCount);
    } else if (key == "flags") {
        lua_pushinteger(state, definition.flags);
    } else if (key == "can_receive") {
        lua_pushboolean(state, receive != "none" ? 1 : 0);
    } else if (key == "can_send") {
        lua_pushboolean(state, send ? 1 : 0);
    } else if (key == "receive_api") {
        lua_pushlstring(state, receive.data(), receive.size());
    } else if (key == "send_api") {
        lua_pushstring(state, send ? "typed_actions" : "none");
    } else if (key == "receive_surfaces") {
        push_message_surfaces(
            state, std::span(definition.ingressSurfaces).first(ingress_surface_count(definition)));
    } else if (key == "send_surfaces") {
        push_message_surfaces(
            state, std::span(definition.egressSurfaces).first(egress_surface_count(definition)));
    } else if (key == "matches" && receive != "none") {
        lua_pushcfunction(state, &message_matches);
    } else if (key == "communication" || key == "route") {
        push_message_communication(state, definition);
    } else if (key == "fields") {
        push_message_field_collection(state, definition.localRow);
    } else {
        lua_pushnil(state);
    }
    return 1;
}
/** Reads one message-field row: path, type, widths, and owner handle. */
[[nodiscard]] int message_field_index(lua_State* state) {
    const auto* const handle =
        static_cast<const MessageFieldHandle*>(luaL_checkudata(state, 1, kMessageFieldMetatable));
    ActivityMessageFieldDefinition definition{};
    if (!current_message_field(state, *handle, definition)) {
        return luaL_error(state, "activity-message field is stale");
    }
    namespace format = state::activity_sdk::format;
    const std::string_view key = lua_string_view(state, 2);
    if (key == "row") {
        lua_pushinteger(state, definition.localRow);
    } else if (key == "global_row") {
        lua_pushinteger(state, definition.globalRow);
    } else if (key == "message_row") {
        lua_pushinteger(state, definition.messageRow);
    } else if (key == "ordinal") {
        lua_pushinteger(state, definition.ordinal);
    } else if (key == "path") {
        lua_pushlstring(state, definition.path.data(), definition.path.size());
    } else if (key == "name") {
        if (definition.name.empty()) {
            lua_pushnil(state);
        } else {
            lua_pushlstring(state, definition.name.data(), definition.name.size());
        }
    } else if (key == "type") {
        if (definition.type.empty()) {
            lua_pushnil(state);
        } else {
            lua_pushlstring(state, definition.type.data(), definition.type.size());
        }
    } else if (key == "source") {
        push_optional_name(state, field_source_name(definition.source));
    } else if (key == "source_code") {
        lua_pushinteger(state, definition.source);
    } else if (key == "struct_offset") {
        push_optional_u32(state, definition.structOffset);
    } else if (key == "struct_offset_abs") {
        push_optional_u32(state, definition.structOffsetAbs);
    } else if (key == "type_code") {
        push_optional_u32(state, definition.typeCode);
    } else if (key == "bias") {
        push_optional_i64(state, definition.bias);
    } else if (key == "bits") {
        push_optional_u32(state, definition.bits);
    } else if (key == "bits_min") {
        push_optional_u32(state, definition.bitsMin);
    } else if (key == "bits_max") {
        push_optional_u32(state, definition.bitsMax);
    } else if (key == "width_or_count_offset") {
        push_optional_u32(state, definition.widthOrCountOffset);
    } else if (key == "repeat" || key == "repeat_count") {
        lua_pushinteger(state, definition.repeat);
    } else if (key == "nested_handle") {
        push_optional_u32(state, definition.nestedHandle);
    } else if (key == "owner_handle") {
        push_optional_u32(state, definition.ownerHandle);
    } else if (key == "depth") {
        lua_pushinteger(state, definition.depth);
    } else if (key == "flags") {
        lua_pushinteger(state, definition.flags);
    } else if (key == "presence_bit") {
        lua_pushboolean(state,
                        (definition.flags & format::kActivityMessageFieldPresenceBit) != 0 ? 1 : 0);
    } else if (key == "coined_name") {
        lua_pushboolean(state,
                        (definition.flags & format::kActivityMessageFieldCoinedName) != 0 ? 1 : 0);
    } else if (key == "documented_row") {
        lua_pushboolean(
            state, (definition.flags & format::kActivityMessageFieldDocumentedRow) != 0 ? 1 : 0);
    } else if (key == "repeated_block") {
        lua_pushboolean(
            state, (definition.flags & format::kActivityMessageFieldRepeatedBlock) != 0 ? 1 : 0);
    } else if (key == "data_only") {
        lua_pushboolean(state,
                        (definition.flags & format::kActivityMessageFieldDataOnly) != 0 ? 1 : 0);
    } else if (key == "confidence") {
        push_optional_name(state, field_confidence_name(definition.confidence));
    } else if (key == "confidence_code") {
        lua_pushinteger(state, definition.confidence);
    } else if (key == "exposure") {
        push_optional_name(state, field_exposure_name(field_exposure_code(definition.flags)));
    } else if (key == "exposure_code") {
        lua_pushinteger(state, field_exposure_code(definition.flags));
    } else {
        lua_pushnil(state);
    }
    return 1;
}
/** Reads one activity binding locator row. */
[[nodiscard]] int activity_binding_locator_index(lua_State* state) {
    const auto* const handle = static_cast<const ActivityBindingLocatorHandle*>(
        luaL_checkudata(state, 1, kActivityBindingLocatorMetatable));
    ActivityBindingLocatorDefinition definition{};
    if (!current_activity_binding_locator(state, *handle, definition)) {
        return luaL_error(state, "activity-binding locator is stale");
    }
    const std::string_view key = lua_string_view(state, 2);
    if (key == "tag") {
        lua_pushinteger(state, definition.tag);
    } else if (key == "offset") {
        push_u64_string(state, definition.offset);
    } else {
        lua_pushnil(state);
    }
    return 1;
}
/** Reads one ActivityView member, deferring unowned keys to the catalog, manifest and world. */
[[nodiscard]] int activity_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kActivityMetatable));
    Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "build_id") {
        lua_pushstring(state, impl->identity.sdkBuildId.data());
    } else if (key == "activity_id") {
        lua_pushstring(state, impl->identity.activityId.data());
    } else if (key == "activity_row") {
        lua_pushinteger(state, impl->identity.activityRow);
    } else if (key == "definition_hash") {
        lua_pushinteger(state, impl->identity.definitionHash);
    } else if (key == "squads") {
        push_squad_collection(state);
    } else if (key == "authored_scenes") {
        push_scene_collection(state);
    } else if (key == "slots") {
        push_slot_collection(state);
    } else if (key == "atom_kinds") {
        push_atom_kinds(state);
    } else if (key == "activity_messages") {
        push_message_collection(state);
    } else if (key == "bap_services") {
        push_bap_service_collection(state);
    } else {
        switch (push_activity_binding_member(state, key)) {
        case ActivityBindingMemberResult::pushed:
            break;
        case ActivityBindingMemberResult::stale:
            return luaL_error(state, "activity binding is stale");
        case ActivityBindingMemberResult::notOwned:
            if (!catalog_api::push_activity_member(state, key)
                && !manifest_api::push_activity_member(state, key)
                && !world_api::push_activity_member(state, key)
                && !push_value_activity_member(state, key) && !push_enum_activity_member(state, key)
                && !push_bounded_activity_member(state, key)) {
                lua_pushnil(state);
            }
            break;
        }
    }
    return 1;
}

void register_definition_metatables(lua_State* state) {
    register_metatable(state, kActivityMetatable, &activity_index);
    register_metatable(state, kActivityBindingLocatorMetatable, &activity_binding_locator_index);
    register_metatable(state, kMessageMetatable, &message_index);
    register_metatable(state, kMessageFieldMetatable, &message_field_index);
}
} // namespace sunrise::server::activity::mission::lua_vm::detail
