#include <string_view>

#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"

// The four mission-input event views. Each carries the client ingress row and the decoded body of
// one client message, so only these four expose client_message_sequence and payload_bytes.

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** The client ingress row and framed size every mission input carries. */
[[nodiscard]] bool
push_client_input_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "client_message_sequence") {
        push_u64_string(state, event.clientMessageSequence);
    } else if (key == "payload_bytes") {
        lua_pushinteger(state, event.payloadBytes);
    } else {
        return false;
    }
    return true;
}

/** Peers the client heard this input from. Sense updates and client messages both report it. */
[[nodiscard]] bool
push_peer_heard_mask_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key != "peer_heard_mask") {
        return false;
    }
    lua_pushinteger(state, event.peerHeardMask);
    return true;
}

/** Counts the Sense decoder retained from one update. */
[[nodiscard]] bool
push_sense_decode_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "objects_decoded") {
        lua_pushinteger(state, event.objectsDecoded);
    } else if (key == "groups_decoded") {
        lua_pushinteger(state, event.groupsDecoded);
    } else {
        return false;
    }
    return true;
}

/** Identity and status of one client activity message, with its catalog row. */
[[nodiscard]] bool
push_client_message_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "message_type") {
        lua_pushinteger(state, event.clientMessageType);
        return true;
    }
    if (key == "message_status") {
        lua_pushstring(state, lua_client_message_status_name(event.clientMessageStatus));
        return true;
    }
    if (key != "message_name" && key != "message") {
        return false;
    }
    ActivityMessageDefinition definition{};
    if (!resolve_message_id(state, event.clientMessageType, definition)) {
        lua_pushnil(state);
    } else if (key == "message") {
        push_message(state, definition.localRow);
    } else {
        lua_pushlstring(state, definition.name.data(), definition.name.size());
    }
    return true;
}

/** Reads one member of a decoded client Sense update. */
[[nodiscard]] int sense_update_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)
        || push_client_input_member(state, event, key)) {
        return 1;
    }
    // The update names one slot only: the first decoded ClientRef of the packet.
    if (event_surface_visible(state, event.kind)
        && (push_state_revision_member(state, event, key)
            || push_peer_heard_mask_member(state, event, key)
            || push_sense_decode_member(state, event, key)
            || (event.hasFirstObject && push_slot_identity_member(state, event, key)))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a committed client-state after-image. Absent fields read nil. */
[[nodiscard]] int client_state_changed_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)
        || push_client_input_member(state, event, key)) {
        return 1;
    }
    if (!event_surface_visible(state, event.kind)) {
        lua_pushnil(state);
        return 1;
    }
    if (key == "activity_state_revision") {
        push_u64_string(state, event.activityStateRevision);
    } else if (key == "membership_revision") {
        lua_pushinteger(state, event.membershipRevision);
    } else if (key == "region_index" && event.clientStateHasRegion) {
        lua_pushinteger(state, event.regionIndex);
    } else if (key == "current_region_index" && event.clientStateHasCurrentRegion) {
        lua_pushinteger(state, event.currentRegionIndex);
    } else if (key == "held_region_index" && event.heldRegionIndex >= 0) {
        lua_pushinteger(state, event.heldRegionIndex);
    } else if (key == "region_slice_set_hash" && event.clientStateHasRegion) {
        lua_pushinteger(state, event.regionSliceSetHash);
    } else if (key == "spawn_state" && event.clientStateHasSpawn) {
        lua_pushinteger(state, event.spawnState);
    } else if (key == "teleport_state" && event.clientStateHasTeleport) {
        lua_pushinteger(state, event.teleportState);
    } else if (key == "teleport_slice_set_index" && event.clientStateHasTeleport) {
        lua_pushinteger(state, event.teleportSliceSetIndex);
    } else if (key == "teleport_slice_set_hash" && event.clientStateHasTeleport) {
        lua_pushinteger(state, event.teleportSliceSetHash);
    } else if (key == "entered" && event.clientEntered) {
        lua_pushboolean(state, 1);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/**
 * Reads one member of an inbound incident.
 * There is no incident_revision member: the mission-sequence prologue writes that union arm.
 */
[[nodiscard]] int incident_received_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)
        || push_client_input_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind) && push_incident_body_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a received client activity message. */
[[nodiscard]] int client_message_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)
        || push_client_input_member(state, event, key)
        || push_peer_heard_mask_member(state, event, key)
        || push_state_revision_member(state, event, key)
        || push_client_message_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

} // namespace

void register_input_event_metatables(lua_State* state) {
    register_metatable(state, kSenseUpdateEventMetatable, &sense_update_index);
    register_metatable(state, kClientStateChangedEventMetatable, &client_state_changed_index);
    register_metatable(state, kIncidentReceivedEventMetatable, &incident_received_index);
    register_metatable(state, kClientMessageEventMetatable, &client_message_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
