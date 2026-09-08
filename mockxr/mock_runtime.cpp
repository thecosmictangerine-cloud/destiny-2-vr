// Minimal mock OpenXR runtime for headless development of a VR mod. Carried over
// from an earlier project, which is why it still names itself DishonoredVR below.
// It reports a fake stereo HMD, accepts a D3D11 session, hands out real D3D11
// swapchain textures, and no-ops the compositor. Lets the mod's entire OpenXR
// lifecycle (instance -> session -> swapchains -> frame loop -> submit) be
// exercised in-game with no headset and no real runtime. TEST TOOL ONLY.
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "mock_loader_interfaces.h"

namespace
{
constexpr uint32_t kEyeW = 1024, kEyeH = 1024;
constexpr uint32_t kSwapImages = 2;

// Optional HMD emulation: SVR_MockHmd.txt (cwd) = "w h fovL fovR fovU fovD"
// (fovs in degrees for the LEFT eye; the right eye is mirrored, as real HMDs
// are). Lets the mock reproduce hardware geometry headless — e.g. Quest 3:
// "1824 1968 -52 42 48 -50". Missing file = legacy 1024x1024 symmetric 90.
uint32_t gEyeW = kEyeW, gEyeH = kEyeH;
float gFovDeg[4] = { -45.0f, 45.0f, 45.0f, -45.0f }; // L R U D, left eye
bool gHmdCfgRead = false;
void ReadHmdConfig()
{
    if (gHmdCfgRead) return;
    gHmdCfgRead = true;
    FILE* f = nullptr;
    fopen_s(&f, "SVR_MockHmd.txt", "r");
    if (!f) return;
    unsigned w = 0, h = 0;
    float l = 0, r = 0, u = 0, d = 0;
    if (fscanf_s(f, "%u %u %f %f %f %f", &w, &h, &l, &r, &u, &d) == 6 && w && h)
    {
        gEyeW = w; gEyeH = h;
        gFovDeg[0] = l; gFovDeg[1] = r; gFovDeg[2] = u; gFovDeg[3] = d;
    }
    fclose(f);
}
XrFovf EyeFov(int eye)
{
    const float k = 3.14159265f / 180.0f;
    if (eye == 0)
        return { gFovDeg[0] * k, gFovDeg[1] * k, gFovDeg[2] * k, gFovDeg[3] * k };
    // Right eye: horizontal mirror of the left.
    return { -gFovDeg[1] * k, -gFovDeg[0] * k, gFovDeg[2] * k, gFovDeg[3] * k };
}

struct MockSwapchain
{
    uint32_t w = kEyeW, h = kEyeH;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::vector<ID3D11Texture2D*> tex;
    uint32_t acquired = 0;
};

// Single instance of everything.
struct XrInstance_T { int _; } gInstance;
struct XrSession_T { int _; } gSession;
struct XrSpace_T { int _; } gSpace;

ID3D11Device* gDevice = nullptr;

// Session state events delivered one per xrPollEvent call.
XrSessionState gStateSeq[] = {
    XR_SESSION_STATE_IDLE, XR_SESSION_STATE_READY,
    XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED
};
int gStateIdx = 0;
int64_t gFrameTime = 1;

FILE* gLog = nullptr;
void L(const char* fmt, ...)
{
    if (!gLog) { fopen_s(&gLog, "SVR_MockXR.log", "w"); }
    if (!gLog) return;
    va_list a; va_start(a, fmt); vfprintf(gLog, fmt, a); va_end(a);
    fprintf(gLog, "\n"); fflush(gLog);
}
} // namespace

#define MOCK_FN extern "C" XRAPI_ATTR XrResult XRAPI_CALL

MOCK_FN Mock_xrEnumerateInstanceExtensionProperties(const char*, uint32_t cap, uint32_t* count, XrExtensionProperties* props)
{
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    uint32_t n = 1;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    strcpy_s(props[0].extensionName, XR_MAX_EXTENSION_NAME_SIZE, exts[0]);
    props[0].extensionVersion = 1;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrCreateInstance(const XrInstanceCreateInfo*, XrInstance* out)
{
    L("xrCreateInstance"); *out = (XrInstance)&gInstance; return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroyInstance(XrInstance) { L("xrDestroyInstance"); return XR_SUCCESS; }

MOCK_FN Mock_xrGetInstanceProperties(XrInstance, XrInstanceProperties* p)
{
    strcpy_s(p->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "DishonoredVR Mock Runtime");
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    return XR_SUCCESS;
}

MOCK_FN Mock_xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* id) { *id = (XrSystemId)1; return XR_SUCCESS; }

MOCK_FN Mock_xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p)
{
    strcpy_s(p->systemName, XR_MAX_SYSTEM_NAME_SIZE, "Mock HMD");
    p->graphicsProperties.maxSwapchainImageWidth = 4096;
    p->graphicsProperties.maxSwapchainImageHeight = 4096;
    p->graphicsProperties.maxLayerCount = 16;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
    uint32_t cap, uint32_t* count, XrViewConfigurationView* views)
{
    ReadHmdConfig();
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; i++)
    {
        views[i].recommendedImageRectWidth = gEyeW;
        views[i].maxImageRectWidth = 4096;
        views[i].recommendedImageRectHeight = gEyeH;
        views[i].maxImageRectHeight = 4096;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

MOCK_FN Mock_xrGetD3D11GraphicsRequirementsKHR(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* req)
{
    req->adapterLuid = LUID{ 0, 0 };
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrCreateSession(XrInstance, const XrSessionCreateInfo* ci, XrSession* out)
{
    const XrBaseInStructure* s = (const XrBaseInStructure*)ci->next;
    while (s)
    {
        if (s->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
        {
            gDevice = ((const XrGraphicsBindingD3D11KHR*)s)->device;
            break;
        }
        s = s->next;
    }
    gStateIdx = 0;
    L("xrCreateSession device=%p", (void*)gDevice);
    *out = (XrSession)&gSession;
    return gDevice ? XR_SUCCESS : XR_ERROR_GRAPHICS_DEVICE_INVALID;
}
MOCK_FN Mock_xrDestroySession(XrSession) { L("xrDestroySession"); return XR_SUCCESS; }

MOCK_FN Mock_xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* out)
{
    *out = (XrSpace)&gSpace; return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroySpace(XrSpace) { return XR_SUCCESS; }

MOCK_FN Mock_xrEnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* count, int64_t* formats)
{
    int64_t f[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    formats[0] = f[0]; formats[1] = f[1];
    return XR_SUCCESS;
}

MOCK_FN Mock_xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* ci, XrSwapchain* out)
{
    MockSwapchain* sc = new MockSwapchain();
    sc->w = ci->width; sc->h = ci->height; sc->fmt = (DXGI_FORMAT)ci->format;
    for (uint32_t i = 0; i < kSwapImages; i++)
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = sc->w; d.Height = sc->h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = sc->fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* t = nullptr;
        HRESULT hr = gDevice ? gDevice->CreateTexture2D(&d, nullptr, &t) : E_FAIL;
        if (FAILED(hr)) { L("CreateTexture2D failed hr=0x%08x", (unsigned)hr); }
        sc->tex.push_back(t);
    }
    L("xrCreateSwapchain %ux%u fmt=%d images=%zu", sc->w, sc->h, (int)sc->fmt, sc->tex.size());
    *out = (XrSwapchain)sc;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroySwapchain(XrSwapchain h)
{
    MockSwapchain* sc = (MockSwapchain*)h;
    for (auto* t : sc->tex) if (t) t->Release();
    delete sc; return XR_SUCCESS;
}

MOCK_FN Mock_xrEnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* imgs)
{
    MockSwapchain* sc = (MockSwapchain*)h;
    *count = (uint32_t)sc->tex.size();
    if (cap == 0) return XR_SUCCESS;
    if (cap < sc->tex.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    XrSwapchainImageD3D11KHR* d3d = (XrSwapchainImageD3D11KHR*)imgs;
    for (size_t i = 0; i < sc->tex.size(); i++) d3d[i].texture = sc->tex[i];
    return XR_SUCCESS;
}

MOCK_FN Mock_xrPollEvent(XrInstance, XrEventDataBuffer* ev)
{
    if (gStateIdx < (int)(sizeof(gStateSeq) / sizeof(gStateSeq[0])))
    {
        auto* e = (XrEventDataSessionStateChanged*)ev;
        e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
        e->next = nullptr;
        e->session = (XrSession)&gSession;
        e->state = gStateSeq[gStateIdx++];
        e->time = gFrameTime;
        return XR_SUCCESS;
    }
    return XR_EVENT_UNAVAILABLE;
}

MOCK_FN Mock_xrBeginSession(XrSession, const XrSessionBeginInfo*) { L("xrBeginSession"); return XR_SUCCESS; }
MOCK_FN Mock_xrEndSession(XrSession) { L("xrEndSession"); return XR_SUCCESS; }

void ReadMockInput(); // defined below, beside the fields it fills

MOCK_FN Mock_xrWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* fs)
{
    // Also read here, not only in xrSyncActions: a mod that drives the head but has not attached
    // an action set yet never syncs, and would be stuck with the procedural sweep.
    ReadMockInput();
    fs->predictedDisplayTime = ++gFrameTime * 1000000;
    fs->predictedDisplayPeriod = 11111111;
    fs->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrBeginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
MOCK_FN Mock_xrEndFrame(XrSession, const XrFrameEndInfo* fei)
{
    static uint64_t n = 0;
    if ((n++ % 300) == 0) L("xrEndFrame #%llu layers=%u", (unsigned long long)n, fei->layerCount);
    return XR_SUCCESS;
}

// Input file, all fields optional (missing = keep previous):
//   turnX trigR handYaw handPitch headYaw headPitch headX headY headZ
//   moveX moveY handX handY handZ btnA btnB btnX btnY
//   turnY trigL gripL gripR btnThumbL btnThumbR btnMenu
//   lhandYaw lhandPitch lhandX lhandY lhandZ
// Angles in degrees; positions in meters (offsets from the defaults);
// buttons are 0/1. New fields appended at the end so old step lists keep working.
// hand* fields drive the RIGHT hand; lhand* the LEFT (psi aim / VR hands).
float gInTurnX = 0, gInTrigR = 0, gInHandYaw = 0, gInHandPitch = 0;
float gInHeadYaw = 0, gInHeadPitch = 0, gInHeadX = 0, gInHeadY = 0, gInHeadZ = 0;
float gInMoveX = 0, gInMoveY = 0, gInHandX = 0, gInHandY = 0, gInHandZ = 0;
float gInBtnA = 0, gInBtnB = 0, gInBtnX = 0, gInBtnY = 0;
float gInTurnY = 0, gInTrigL = 0, gInGripL = 0, gInGripR = 0;
float gInBtnThumbL = 0, gInBtnThumbR = 0, gInBtnMenu = 0;
float gInLHandYaw = 0, gInLHandPitch = 0, gInLHandX = 0, gInLHandY = 0, gInLHandZ = 0;
bool gHaveInput = false; //!< once a file was read, head pose is file-driven

MOCK_FN Mock_xrLocateViews(XrSession, const XrViewLocateInfo*, XrViewState* vs,
    uint32_t cap, uint32_t* count, XrView* views)
{
    *count = 2;
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (cap < 2) return XR_SUCCESS;

    // Head pose: file-driven once SVR_MockInput.txt exists (deterministic
    // tests); otherwise a slow sine yaw sweep so the pipeline carries changing
    // data. q = yaw(Y) * pitch(X).
    float yaw, pitch = 0;
    if (gHaveInput)
    {
        yaw = gInHeadYaw * 3.14159265f / 180.0f;
        pitch = gInHeadPitch * 3.14159265f / 180.0f;
    }
    else
    {
        yaw = 0.6f * sinf((float)gFrameTime * 0.02f);
    }
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    for (int i = 0; i < 2; i++)
    {
        views[i].pose.orientation = { cy * sp, sy * cp, -sy * sp, cy * cp };
        views[i].pose.position = { (i == 0 ? -0.032f : 0.032f) + gInHeadX,
                                   1.6f + gInHeadY, gInHeadZ }; // ~IPD, standing height + offsets
        views[i].fov = EyeFov(i);
    }
    return XR_SUCCESS;
}

// ---- Input (controller actions) ----
struct MockActionSet_T { int _; } gActionSet;
struct MockAction { std::string name; XrActionType type; };
struct MockActionSpace { int hand; };

void ReadMockInput()
{
    // Only re-read when the file changed: opening it every frame races with the test
    // harness writer (sharing violations) and made short input pulses unreliable.
    static FILETIME lastWrite = {};
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA("SVR_MockInput.txt", GetFileExInfoStandard, &fad)) return;
    if (fad.ftLastWriteTime.dwLowDateTime == lastWrite.dwLowDateTime && fad.ftLastWriteTime.dwHighDateTime == lastWrite.dwHighDateTime) return;
    lastWrite = fad.ftLastWriteTime;
    FILE* f = nullptr;
    fopen_s(&f, "SVR_MockInput.txt", "r");
    if (f)
    {
        int n = fscanf_s(f, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
            &gInTurnX, &gInTrigR, &gInHandYaw, &gInHandPitch,
            &gInHeadYaw, &gInHeadPitch, &gInHeadX, &gInHeadY, &gInHeadZ,
            &gInMoveX, &gInMoveY, &gInHandX, &gInHandY, &gInHandZ,
            &gInBtnA, &gInBtnB, &gInBtnX, &gInBtnY,
            &gInTurnY, &gInTrigL, &gInGripL, &gInGripR,
            &gInBtnThumbL, &gInBtnThumbR, &gInBtnMenu,
            &gInLHandYaw, &gInLHandPitch, &gInLHandX, &gInLHandY, &gInLHandZ);
        fclose(f);
        if (n >= 1)
            gHaveInput = true;
    }
}

MOCK_FN Mock_xrStringToPath(XrInstance, const char* str, XrPath* path)
{
    // FNV-1a hash -> stable non-zero path atom.
    uint64_t h = 1469598103934665603ULL;
    for (const char* p = str; *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    *path = (XrPath)(h ? h : 1);
    return XR_SUCCESS;
}
MOCK_FN Mock_xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* out)
{
    *out = (XrActionSet)&gActionSet; return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroyActionSet(XrActionSet) { return XR_SUCCESS; }
MOCK_FN Mock_xrCreateAction(XrActionSet, const XrActionCreateInfo* ci, XrAction* out)
{
    MockAction* a = new MockAction();
    a->name = ci->actionName;
    a->type = ci->actionType;
    *out = (XrAction)a;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrSuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding*) { return XR_SUCCESS; }
MOCK_FN Mock_xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*) { return XR_SUCCESS; }
MOCK_FN Mock_xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo* ci, XrSpace* out)
{
    MockAction* a = (MockAction*)ci->action;
    MockActionSpace* s = new MockActionSpace();
    s->hand = (a && a->name.size() && a->name.back() == 'r') ? 1 : 0;
    *out = (XrSpace)s;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrSyncActions(XrSession, const XrActionsSyncInfo*) { ReadMockInput(); return XR_SUCCESS; }
MOCK_FN Mock_xrGetActionStateFloat(XrSession, const XrActionStateGetInfo* gi, XrActionStateFloat* st)
{
    MockAction* a = (MockAction*)gi->action;
    st->isActive = XR_TRUE;
    st->currentState = 0.0f;
    if (a)
    {
        if (a->name == "turn") st->currentState = gInTurnX;
        else if (a->name == "turn_y") st->currentState = gInTurnY;
        else if (a->name == "trigger_r") st->currentState = gInTrigR;
        else if (a->name == "trigger_l") st->currentState = gInTrigL;
        else if (a->name == "grip_l") st->currentState = gInGripL;
        else if (a->name == "grip_r") st->currentState = gInGripR;
    }
    return XR_SUCCESS;
}
MOCK_FN Mock_xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo* gi, XrActionStateVector2f* st)
{
    MockAction* a = (MockAction*)gi->action;
    st->isActive = XR_TRUE;
    st->currentState = { 0.0f, 0.0f };
    if (a && a->name == "move")
        st->currentState = { gInMoveX, gInMoveY };
    else if (a && a->name == "turn")   // right thumbstick as one vector, as a real Touch reports it
        st->currentState = { gInTurnX, gInTurnY };
    return XR_SUCCESS;
}
MOCK_FN Mock_xrGetActionStatePose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* st)
{
    st->isActive = XR_TRUE; return XR_SUCCESS;
}
MOCK_FN Mock_xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo* gi, XrActionStateBoolean* st)
{
    MockAction* a = (MockAction*)gi->action;
    st->isActive = XR_TRUE;
    st->currentState = XR_FALSE;
    if (a)
    {
        float v = 0;
        if (a->name == "btn_a") v = gInBtnA;
        else if (a->name == "btn_b") v = gInBtnB;
        else if (a->name == "btn_x") v = gInBtnX;
        else if (a->name == "btn_y") v = gInBtnY;
        else if (a->name == "btn_thumb_l") v = gInBtnThumbL;
        else if (a->name == "btn_thumb_r") v = gInBtnThumbR;
        else if (a->name == "btn_menu") v = gInBtnMenu;
        st->currentState = (v > 0.5f) ? XR_TRUE : XR_FALSE;
    }
    return XR_SUCCESS;
}
MOCK_FN Mock_xrLocateSpace(XrSpace space, XrSpace, XrTime, XrSpaceLocation* loc)
{
    MockActionSpace* s = (MockActionSpace*)space;
    // Simulated hand aim poses: in front of the head, aiming along -Z. The
    // right hand is driven by hand*, the left by lhand* (independent, so
    // left-hand psi aim and VR-hand tests can steer them separately).
    const bool right = (s && s->hand == 1);
    float yaw = (right ? gInHandYaw : gInLHandYaw) * 3.14159265f / 180.0f;
    float pitch = (right ? gInHandPitch : gInLHandPitch) * 3.14159265f / 180.0f;
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    // q = yaw(Y) * pitch(X)
    loc->pose.orientation.w = cy * cp;
    loc->pose.orientation.x = cy * sp;
    loc->pose.orientation.y = sy * cp;
    loc->pose.orientation.z = -sy * sp;
    if (right)
        loc->pose.position = { 0.2f + gInHandX, 1.4f + gInHandY, -0.3f + gInHandZ };
    else
        loc->pose.position = { -0.2f + gInLHandX, 1.4f + gInLHandY, -0.3f + gInLHandZ };
    loc->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrAcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{
    MockSwapchain* sc = (MockSwapchain*)h;
    *index = sc->acquired;
    sc->acquired = (sc->acquired + 1) % (uint32_t)sc->tex.size();
    return XR_SUCCESS;
}
MOCK_FN Mock_xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
MOCK_FN Mock_xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }

// ---- Dispatch ----
extern "C" XRAPI_ATTR XrResult XRAPI_CALL Mock_xrGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* fn)
{
    *fn = nullptr;
    auto set = [&](void* p) { *fn = (PFN_xrVoidFunction)p; return XR_SUCCESS; };
#define BIND(n) if (strcmp(name, #n) == 0) return set((void*)Mock_##n)
    BIND(xrEnumerateInstanceExtensionProperties);
    BIND(xrCreateInstance);
    BIND(xrDestroyInstance);
    BIND(xrGetInstanceProperties);
    BIND(xrGetSystem);
    BIND(xrGetSystemProperties);
    BIND(xrEnumerateViewConfigurationViews);
    BIND(xrGetD3D11GraphicsRequirementsKHR);
    BIND(xrCreateSession);
    BIND(xrDestroySession);
    BIND(xrCreateReferenceSpace);
    BIND(xrDestroySpace);
    BIND(xrEnumerateSwapchainFormats);
    BIND(xrCreateSwapchain);
    BIND(xrDestroySwapchain);
    BIND(xrEnumerateSwapchainImages);
    BIND(xrPollEvent);
    BIND(xrBeginSession);
    BIND(xrEndSession);
    BIND(xrWaitFrame);
    BIND(xrBeginFrame);
    BIND(xrEndFrame);
    BIND(xrLocateViews);
    BIND(xrAcquireSwapchainImage);
    BIND(xrWaitSwapchainImage);
    BIND(xrReleaseSwapchainImage);
    BIND(xrStringToPath);
    BIND(xrCreateActionSet);
    BIND(xrDestroyActionSet);
    BIND(xrCreateAction);
    BIND(xrSuggestInteractionProfileBindings);
    BIND(xrAttachSessionActionSets);
    BIND(xrCreateActionSpace);
    BIND(xrSyncActions);
    BIND(xrGetActionStateFloat);
    BIND(xrGetActionStateVector2f);
    BIND(xrGetActionStatePose);
    BIND(xrGetActionStateBoolean);
    BIND(xrLocateSpace);
#undef BIND
    if (strcmp(name, "xrGetInstanceProcAddr") == 0)
        return set((void*)Mock_xrGetInstanceProcAddr);
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---- Loader negotiation entry point ----
extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* req)
{
    L("xrNegotiateLoaderRuntimeInterface");
    if (!loaderInfo || !req) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;

    req->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    req->runtimeApiVersion = XR_CURRENT_API_VERSION;
    req->getInstanceProcAddr = Mock_xrGetInstanceProcAddr;
    return XR_SUCCESS;
}
