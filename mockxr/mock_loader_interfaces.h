#pragma once
// Minimal subset of OpenXR's loader<->runtime negotiation ABI (normally in the
// loader source's loader_interfaces.h, not the public SDK). Stable interface v1.
#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_CURRENT_LOADER_RUNTIME_VERSION 1
#define XR_LOADER_INFO_STRUCT_VERSION 1
#define XR_RUNTIME_INFO_STRUCT_VERSION 1

typedef enum XrLoaderInterfaceStructs {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO,
} XrLoaderInterfaceStructs;

typedef struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateRuntimeRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t runtimeInterfaceVersion;
    XrVersion runtimeApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

#ifdef __cplusplus
}
#endif
