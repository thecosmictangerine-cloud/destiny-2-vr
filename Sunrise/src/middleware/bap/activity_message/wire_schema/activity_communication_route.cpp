#include "activity_communication_route.h"

#include <array>

namespace sunrise::middleware::bap::activity_message::wire_schema::communication {
namespace {

using IA = IngressAdapter;
using IC = IngressClass;
using EA = EgressAdapter;
using EC = EgressClass;
using OC = OutputCodec;
using SO = StateOwner;
using LE = LuaExposure;
using ID = IngressDeliveryPolicy;
using ED = EgressDeliveryPolicy;
using LP = LateJoinHandoffPolicy;
using RS = RouteStatus;

#include "activity_communication_route_data.inc"

/** @return The declaration row for one enum value, or null when the table has none. */
template <typename Enum, std::size_t Count>
[[nodiscard]] const EnumDeclaration<Enum>*
find_declaration(Enum value,
                 const std::array<EnumDeclaration<Enum>, Count>& declarations) noexcept {
    for (const EnumDeclaration<Enum>& declaration : declarations) {
        if (declaration.value == value) {
            return &declaration;
        }
    }
    return nullptr;
}

/** @return The stable SDK spelling generated for one enum value, or an empty view. */
template <typename Enum, std::size_t Count>
[[nodiscard]] std::string_view
declaration_name(Enum value,
                 const std::array<EnumDeclaration<Enum>, Count>& declarations) noexcept {
    const EnumDeclaration<Enum>* const declaration = find_declaration(value, declarations);
    return declaration != nullptr ? declaration->name : std::string_view{};
}

/** @return The checked source path generated for one enum value, or an empty view. */
template <typename Enum, std::size_t Count>
[[nodiscard]] std::string_view
declaration_path(Enum value,
                 const std::array<EnumDeclaration<Enum>, Count>& declarations) noexcept {
    const EnumDeclaration<Enum>* const declaration = find_declaration(value, declarations);
    return declaration != nullptr ? declaration->path : std::string_view{};
}

/** @return The one generated executable-domain declaration with this SDK name. */
template <typename Enum, std::size_t Count>
[[nodiscard]] const EnumDeclaration<Enum>*
find_named_declaration(std::string_view name,
                       const std::array<EnumDeclaration<Enum>, Count>& declarations) noexcept {
    for (const EnumDeclaration<Enum>& declaration : declarations) {
        if (declaration.name == name) {
            return &declaration;
        }
    }
    return nullptr;
}

/** @return True when one ingress class and its route fields agree. */
constexpr bool ingress_is_valid(const ActivityCommunicationRoute& route) noexcept {
    if (route.ingressClass == IC::notClientSent) {
        return route.ingressAdapter == IA::none && route.ingressDelivery == ID::none
               && route.ingressStatus == RS::notApplicable;
    }
    return route.ingressAdapter != IA::none && route.ingressDelivery != ID::none
           && route.ingressStatus == RS::resolved;
}

/** @return True when one egress class and its route fields agree. */
constexpr bool egress_is_valid(const ActivityCommunicationRoute& route) noexcept {
    switch (route.egressClass) {
    case EC::notServerOutputDirection:
        return route.egressAdapter == EA::none && route.egressDelivery == ED::none
               && route.egressStatus == RS::notApplicable;
    case EC::clientHandlesNoServerRoute:
        return route.egressAdapter == EA::none && route.egressDelivery == ED::none
               && route.egressStatus == RS::noServerRoute;
    case EC::routedNoLuaAction:
        return route.egressAdapter != EA::none && route.egressDelivery == ED::protocolNotification
               && route.egressStatus == RS::routedActionSemanticsUnresolved;
    case EC::routedProtocolOwned:
        return route.egressAdapter != EA::none && route.egressDelivery == ED::protocolNotification
               && route.egressStatus == RS::resolved;
    case EC::routedTypedLuaAction:
        return route.egressAdapter != EA::none && route.egressDelivery == ED::typedAuthStaging
               && route.egressStatus == RS::resolved;
    }
    return false;
}

/** Binds one generated SDK name through a finite executable enum domain. */
template <typename Enum, std::size_t Count>
[[nodiscard]] bool bind_name(std::string_view name,
                             const std::array<EnumDeclaration<Enum>, Count>& declarations,
                             Enum& output) noexcept {
    const EnumDeclaration<Enum>* const declaration = find_named_declaration(name, declarations);
    if (declaration == nullptr) {
        return false;
    }
    output = declaration->value;
    return true;
}

/** Binds one generated SDK name only when its audited source path also matches. */
template <typename Enum, std::size_t Count>
[[nodiscard]] bool bind_name_and_path(std::string_view name,
                                      std::string_view path,
                                      const std::array<EnumDeclaration<Enum>, Count>& declarations,
                                      Enum& output) noexcept {
    const EnumDeclaration<Enum>* const declaration = find_named_declaration(name, declarations);
    if (declaration == nullptr || declaration->path != path) {
        return false;
    }
    output = declaration->value;
    return true;
}

/** @return True when generated Lua exposure and count agree with this route class. */
[[nodiscard]] bool generated_lua_is_valid(const ActivityCommunicationRoute& route) noexcept {
    switch (route.ingressClass) {
    case IC::nativeMetadataOnly:
    case IC::nativeParsed:
        return route.luaExposure == LE::messageMetadataOnly && route.typedLuaSurfaceCount == 0;
    case IC::typedOnly:
    case IC::typedPostCommitOnly:
        return route.luaExposure == LE::typedEvent && route.typedLuaSurfaceCount != 0;
    case IC::acceptedJoinRecordOnly:
        return route.luaExposure == LE::sdkMetadataOnly && route.typedLuaSurfaceCount == 0;
    case IC::notClientSent:
        if (route.egressClass == EC::routedTypedLuaAction) {
            return route.luaExposure == LE::typedAction && route.typedLuaSurfaceCount != 0;
        }
        return route.luaExposure == LE::sdkMetadataOnly && route.typedLuaSurfaceCount == 0;
    }
    return false;
}

/** @return True when generated continuity agrees with the route's executable class. */
[[nodiscard]] bool generated_continuity_is_valid(const ActivityCommunicationRoute& route) noexcept {
    switch (route.egressClass) {
    case EC::clientHandlesNoServerRoute:
        return route.lateJoinHandoff == LP::noServerRoute;
    case EC::routedProtocolOwned:
        return route.lateJoinHandoff == LP::protocolLifecycleOwned;
    case EC::routedNoLuaAction:
    case EC::routedTypedLuaAction:
        return route.lateJoinHandoff == LP::missionStatePublicationUnresolved;
    case EC::notServerOutputDirection:
        return route.lateJoinHandoff
               == (route.luaExposure == LE::typedEvent ? LP::missionStatePublicationUnresolved
                                                       : LP::notDeclared);
    }
    return false;
}

/** @return True when one SDK-selected route agrees with the fixed wire direction. */
[[nodiscard]] bool generated_route_is_valid(const ActivityCommunicationRoute& route,
                                            const MessageDescriptor& message) noexcept {
    const bool clientSends = route.ingressClass != IC::notClientSent;
    const bool clientHandles = route.egressClass != EC::notServerOutputDirection;
    return ingress_is_valid(route) && egress_is_valid(route) && clientSends == message.clientSends
           && clientHandles == message.clientHandles
           && (route.outputCodec != OC::none) == message.clientHandles
           && generated_lua_is_valid(route) && generated_continuity_is_valid(route);
}

/** Converts one data-only SDK row into a checked executable enum route. */
[[nodiscard]] bool bind_generated_route(const RouteIdentity& source,
                                        const MessageDescriptor& message,
                                        ActivityCommunicationRoute& output) noexcept {
    output = {};
    if (source.messageIndex != message.id || source.messageId != message.id
        || source.typedLuaSurfaceCount > 0xFFU || source.flags != kRouteIdentityDataOnly) {
        return false;
    }
    ActivityCommunicationRoute pending{};
    pending.messageId = source.messageId;
    if (!bind_name_and_path(source.ingressAdapter,
                            source.ingressAdapterPath,
                            kIngressAdapterDeclarations,
                            pending.ingressAdapter)
        || !bind_name(source.ingressClass, kIngressClassDeclarations, pending.ingressClass)
        || !bind_name_and_path(source.egressAdapter,
                               source.egressAdapterPath,
                               kEgressAdapterDeclarations,
                               pending.egressAdapter)
        || !bind_name(source.egressClass, kEgressClassDeclarations, pending.egressClass)
        || !bind_name_and_path(source.outputCodec,
                               source.outputCodecPath,
                               kOutputCodecDeclarations,
                               pending.outputCodec)
        || !bind_name_and_path(
            source.stateOwner, source.stateOwnerPath, kStateOwnerDeclarations, pending.stateOwner)
        || !bind_name(source.luaExposure, kLuaExposureDeclarations, pending.luaExposure)
        || !bind_name(source.ingressDelivery, kIngressDeliveryDeclarations, pending.ingressDelivery)
        || !bind_name(source.egressDelivery, kEgressDeliveryDeclarations, pending.egressDelivery)
        || !bind_name(source.lateJoinHandoff, kLateJoinHandoffDeclarations, pending.lateJoinHandoff)
        || !bind_name(source.ingressStatus, kIngressStatusDeclarations, pending.ingressStatus)
        || !bind_name(source.egressStatus, kIngressStatusDeclarations, pending.egressStatus)) {
        return false;
    }
    pending.typedLuaSurfaceCount = static_cast<std::uint8_t>(source.typedLuaSurfaceCount);
    if (!generated_route_is_valid(pending, message)) {
        return false;
    }
    output = pending;
    return true;
}

/** The mission ABI declares eight actions and eleven typed event surfaces. */
constexpr std::size_t kTypedLuaSurfaceCount = 19;

/** @return True when every row is ordered and its bounded fields agree. */
consteval bool routes_are_valid() noexcept {
    std::size_t typedSurfaceCount = 0;
    for (std::size_t index = 0; index < kRoutes.size(); ++index) {
        const ActivityCommunicationRoute& route = kRoutes[index];
        if (route.messageId != index || !ingress_is_valid(route) || !egress_is_valid(route)) {
            return false;
        }
        const bool typed =
            route.luaExposure == LE::typedEvent || route.luaExposure == LE::typedAction;
        if (typed != (route.typedLuaSurfaceCount != 0)) {
            return false;
        }
        typedSurfaceCount += route.typedLuaSurfaceCount;
    }
    return typedSurfaceCount == kTypedLuaSurfaceCount;
}

/** @return Number of rows whose selected enum member equals the value. */
template <auto Member, typename Value> consteval std::size_t count_routes(Value value) noexcept {
    std::size_t count = 0;
    for (const ActivityCommunicationRoute& route : kRoutes) {
        count += (route.*Member == value) ? 1U : 0U;
    }
    return count;
}

/** Typed producers own their messages and cannot also enter generic Lua input. */
consteval bool typed_ingress_ids_are_valid() noexcept {
    for (const ActivityCommunicationRoute& route : kRoutes) {
        const bool expected = route.messageId == 6U || route.messageId == 19U
                              || route.messageId == 20U || route.messageId == 22U;
        if (message_lua_input_denied(route) != expected) {
            return false;
        }
    }
    return true;
}

static_assert(kRoutes.size() == kRouteCount);
static_assert(routes_are_valid());
static_assert(typed_ingress_ids_are_valid());
static_assert(count_routes<&ActivityCommunicationRoute::ingressClass>(IC::notClientSent) == 28U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressClass>(IC::acceptedJoinRecordOnly)
              == 1U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressClass>(IC::nativeMetadataOnly)
              == 5U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressClass>(IC::nativeParsed) == 21U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressClass>(IC::typedOnly) == 2U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressClass>(IC::typedPostCommitOnly)
              == 2U);
static_assert(count_routes<&ActivityCommunicationRoute::egressClass>(EC::notServerOutputDirection)
              == 38U);
static_assert(count_routes<&ActivityCommunicationRoute::egressClass>(EC::clientHandlesNoServerRoute)
              == 11U);
static_assert(count_routes<&ActivityCommunicationRoute::egressClass>(EC::routedNoLuaAction) == 1U);
static_assert(count_routes<&ActivityCommunicationRoute::egressClass>(EC::routedProtocolOwned)
              == 8U);
static_assert(count_routes<&ActivityCommunicationRoute::egressClass>(EC::routedTypedLuaAction)
              == 1U);
static_assert(count_routes<&ActivityCommunicationRoute::luaExposure>(LE::sdkMetadataOnly) == 28U);
static_assert(count_routes<&ActivityCommunicationRoute::luaExposure>(LE::messageMetadataOnly)
              == 26U);
static_assert(count_routes<&ActivityCommunicationRoute::luaExposure>(LE::typedEvent) == 4U);
static_assert(count_routes<&ActivityCommunicationRoute::luaExposure>(LE::typedAction) == 1U);
static_assert(count_routes<&ActivityCommunicationRoute::stateOwner>(SO::none) == 36U);
static_assert(count_routes<&ActivityCommunicationRoute::stateOwner>(SO::activityAuthorityConnection)
              == 3U);
static_assert(
    count_routes<&ActivityCommunicationRoute::stateOwner>(SO::activityAuthorityResetConnection)
    == 2U);
static_assert(count_routes<&ActivityCommunicationRoute::stateOwner>(SO::activityHost) == 3U);
static_assert(count_routes<&ActivityCommunicationRoute::stateOwner>(SO::activityMembershipState)
              == 6U);
static_assert(count_routes<&ActivityCommunicationRoute::stateOwner>(SO::activityEntitySlotsState)
              == 3U);
static_assert(count_routes<&ActivityCommunicationRoute::stateOwner>(SO::activityMessageTransaction)
              == 5U);
static_assert(
    count_routes<&ActivityCommunicationRoute::stateOwner>(SO::activityPatchEpochConnection) == 1U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressDelivery>(ID::none) == 28U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressDelivery>(ID::joinedRecordOnly)
              == 1U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressDelivery>(ID::protocolHostInput)
              == 26U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressDelivery>(ID::typedHostInput) == 2U);
static_assert(
    count_routes<&ActivityCommunicationRoute::ingressDelivery>(ID::postCommitTypedHostInput) == 2U);
static_assert(count_routes<&ActivityCommunicationRoute::egressDelivery>(ED::none) == 49U);
static_assert(count_routes<&ActivityCommunicationRoute::egressDelivery>(ED::protocolNotification)
              == 9U);
static_assert(count_routes<&ActivityCommunicationRoute::egressDelivery>(ED::typedAuthStaging)
              == 1U);
static_assert(count_routes<&ActivityCommunicationRoute::lateJoinHandoff>(LP::notDeclared) == 35U);
static_assert(count_routes<&ActivityCommunicationRoute::lateJoinHandoff>(LP::protocolLifecycleOwned)
              == 8U);
static_assert(count_routes<&ActivityCommunicationRoute::lateJoinHandoff>(
                  LP::missionStatePublicationUnresolved)
              == 5U);
static_assert(count_routes<&ActivityCommunicationRoute::lateJoinHandoff>(LP::noServerRoute) == 11U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressStatus>(RS::notApplicable) == 28U);
static_assert(count_routes<&ActivityCommunicationRoute::ingressStatus>(RS::resolved) == 31U);
static_assert(count_routes<&ActivityCommunicationRoute::egressStatus>(RS::notApplicable) == 38U);
static_assert(count_routes<&ActivityCommunicationRoute::egressStatus>(RS::resolved) == 9U);
static_assert(count_routes<&ActivityCommunicationRoute::egressStatus>(RS::noServerRoute) == 11U);
static_assert(
    count_routes<&ActivityCommunicationRoute::egressStatus>(RS::routedActionSemanticsUnresolved)
    == 1U);

} // namespace

std::span<const ActivityCommunicationRoute> routes() noexcept {
    return kRoutes;
}

const ActivityCommunicationRoute* find_route(std::uint32_t messageId) noexcept {
    return messageId < kRoutes.size() ? &kRoutes[messageId] : nullptr;
}

std::string_view sdk_projection_schema() noexcept {
    return kSdkProjectionSchema;
}

std::string_view stable_name(IngressAdapter value) noexcept {
    return declaration_name(value, kIngressAdapterDeclarations);
}

std::string_view stable_name(IngressClass value) noexcept {
    return declaration_name(value, kIngressClassDeclarations);
}

std::string_view stable_name(EgressAdapter value) noexcept {
    return declaration_name(value, kEgressAdapterDeclarations);
}

std::string_view stable_name(EgressClass value) noexcept {
    return declaration_name(value, kEgressClassDeclarations);
}

std::string_view stable_name(OutputCodec value) noexcept {
    return declaration_name(value, kOutputCodecDeclarations);
}

std::string_view stable_name(StateOwner value) noexcept {
    return declaration_name(value, kStateOwnerDeclarations);
}

std::string_view stable_name(LuaExposure value) noexcept {
    return declaration_name(value, kLuaExposureDeclarations);
}

std::string_view stable_name(IngressDeliveryPolicy value) noexcept {
    return declaration_name(value, kIngressDeliveryDeclarations);
}

std::string_view stable_name(EgressDeliveryPolicy value) noexcept {
    return declaration_name(value, kEgressDeliveryDeclarations);
}

std::string_view stable_name(LateJoinHandoffPolicy value) noexcept {
    return declaration_name(value, kLateJoinHandoffDeclarations);
}

/** Ingress and egress status share the one generated RouteStatus declaration table. */
std::string_view stable_name(RouteStatus value) noexcept {
    return declaration_name(value, kIngressStatusDeclarations);
}

std::string_view ingress_adapter_path(IngressAdapter adapter) noexcept {
    return declaration_path(adapter, kIngressAdapterDeclarations);
}

std::string_view egress_adapter_path(EgressAdapter adapter) noexcept {
    return declaration_path(adapter, kEgressAdapterDeclarations);
}

std::string_view output_codec_path(OutputCodec codec) noexcept {
    return declaration_path(codec, kOutputCodecDeclarations);
}

std::string_view state_owner_path(StateOwner owner) noexcept {
    return declaration_path(owner, kStateOwnerDeclarations);
}

std::span<const ExecutableRoute> ExecutableRegistry::entries() const noexcept {
    return ready() ? std::span(entries_) : std::span<const ExecutableRoute>{};
}

/** A ready registry holds one row per message id in id order, so the id is also the row index. */
const ActivityCommunicationRoute* ExecutableRegistry::find(std::uint32_t messageId) const noexcept {
    if (!ready() || messageId >= entries_.size()) {
        return nullptr;
    }
    return &entries_[messageId].route;
}

const ActivityCommunicationRoute*
ExecutableRegistry::find(std::uint32_t messageId, std::string_view messageName) const noexcept {
    if (!ready() || messageId >= entries_.size()) {
        return nullptr;
    }
    const ExecutableRoute& entry = entries_[messageId];
    return entry.messageName == messageName ? find(messageId) : nullptr;
}

std::size_t ExecutableRegistry::matched_count() const noexcept {
    return ready() ? matchedCount_ : 0;
}

bool ExecutableRegistry::ready() const noexcept {
    return status_ == ExecutableRegistryStatus::ready;
}

ExecutableRegistryStatus ExecutableRegistry::status() const noexcept {
    return status_;
}

std::uint32_t ExecutableRegistry::failed_message_id() const noexcept {
    return failedMessageId_;
}

/**
 * Binds one route row per message row after proving both identity tables agree.
 * @param messages One message identity per route row, in row order.
 * @param routeIdentities One route identity per route row, in row order.
 * @param output Cleared first. Receives the registry only when every row bound.
 * @return True when both extents and every message and route identity match.
 */
bool build_executable_registry(std::span<const MessageIdentity> messages,
                               std::span<const RouteIdentity> routeIdentities,
                               ExecutableRegistry& output) noexcept {
    output = {};
    if (messages.size() != kRouteCount || routeIdentities.size() != kRouteCount) {
        output.status_ = ExecutableRegistryStatus::wrongExtent;
        return false;
    }
    ExecutableRegistry pending{};
    for (std::size_t index = 0; index < kRouteCount; ++index) {
        ExecutableRoute& entry = pending.entries_[index];
        entry.messageId = static_cast<std::uint32_t>(index);
        // Row count equals the message count, so every index names a message row.
        const MessageDescriptor* const descriptor = find_message(entry.messageId);
        entry.messageName = descriptor->name;
        if (messages[index].messageId != entry.messageId
            || messages[index].name != descriptor->name) {
            output.status_ = ExecutableRegistryStatus::messageIdentityMismatch;
            output.failedMessageId_ = entry.messageId;
            return false;
        }
        if (!bind_generated_route(routeIdentities[index], *descriptor, entry.route)) {
            output.status_ = ExecutableRegistryStatus::routeIdentityMismatch;
            output.failedMessageId_ = entry.messageId;
            return false;
        }
        ++pending.matchedCount_;
    }
    pending.status_ = ExecutableRegistryStatus::ready;
    output = pending;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::wire_schema::communication
