// Standalone test of the mock OpenXR runtime (mock_runtime.cpp): loads the DLL the way a loader
// does (xrNegotiateLoaderRuntimeInterface), drives it with a WARP D3D11 device, and checks
//   - projection + quad layers composed into each eye (LOCAL, VIEW and action-space quads, sRGB and
//     UNORM, premultiplied / unpremultiplied / opaque, per-eye visibility), a quad-only frame, and a
//     compose sequence;
//   - xrEndFrame's validation (dead handles, a never-released swapchain, a bad rect, too many layers)
//     and the frame call order, single-threaded and with xrWaitFrame on its own thread;
//   - every SVR_MockEvent.txt word: nohmd, stall (HOURGLASS), recentre (LOCAL really moves), stop
//     (READY again later), exit, loss (and a new session after it), instanceloss (and a new instance).
// Run it in a scratch directory (Run-MockHarness.ps1 does): the mock reads and writes its files in
// the working directory. Prints PASS/FAIL per check and exits with the number of failures.
#include <windows.h>
#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "../mock_loader_interfaces.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>

namespace
{
int gFails = 0, gPasses = 0;
void Check(bool ok, const char* what, ...)
{
    char b[512];
    va_list a; va_start(a, what); vsnprintf(b, sizeof b, what, a); va_end(a);
    printf("%s  %s\n", ok ? "PASS" : "FAIL", b);
    fflush(stdout);
    (ok ? gPasses : gFails)++;
}

PFN_xrGetInstanceProcAddr gGpa = nullptr;
XrInstance gInst = XR_NULL_HANDLE;
template <typename T> T Fn(const char* name)
{
    PFN_xrVoidFunction f = nullptr;
    gGpa(gInst, name, &f);
    if (!f) { printf("missing %s\n", name); exit(100); }
    return (T)f;
}
#define FN(n) static auto n = Fn<PFN_##n>(#n)

double NowSec()
{
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}
void WriteText(const char* path, const char* text)
{
    FILE* f = nullptr; fopen_s(&f, path, "wb"); if (f) { fputs(text, f); fclose(f); }
}
std::string ReadText(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string s; char buf[65536]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) s.append(buf, got);
    CloseHandle(h); return s;
}
size_t Count(const std::string& hay, const char* needle)
{
    size_t n = 0; for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++; return n;
}

ID3D11Device* gDev = nullptr;
ID3D11DeviceContext* gCtx = nullptr;

uint8_t Enc(float v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); return (uint8_t)(255.f * (v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - 0.055f) + 0.5f); }

struct Swap
{
    XrSwapchain h = XR_NULL_HANDLE;
    uint32_t w = 0, h_ = 0;
    std::vector<ID3D11Texture2D*> img;
};
Swap MakeSwap(XrSession s, uint32_t w, uint32_t h, int64_t fmt, const std::function<void(int, int, uint8_t*)>& px)
{
    FN(xrCreateSwapchain); FN(xrEnumerateSwapchainImages);
    Swap sw; sw.w = w; sw.h_ = h;
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    const XrResult r = xrCreateSwapchain(s, &ci, &sw.h);
    if (r != XR_SUCCESS) { Check(false, "xrCreateSwapchain %ux%u -> %d", w, h, (int)r); return sw; }
    uint32_t n = 0; xrEnumerateSwapchainImages(sw.h, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> im(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xrEnumerateSwapchainImages(sw.h, n, &n, (XrSwapchainImageBaseHeader*)im.data());
    std::vector<uint8_t> data((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) px((int)x, (int)y, &data[((size_t)y * w + x) * 4]);
    for (auto& i : im) { sw.img.push_back(i.texture); gCtx->UpdateSubresource(i.texture, 0, nullptr, data.data(), w * 4, 0); }
    return sw;
}
void Cycle(const Swap& s)
{
    FN(xrAcquireSwapchainImage); FN(xrWaitSwapchainImage); FN(xrReleaseSwapchainImage);
    uint32_t idx = 0;
    xrAcquireSwapchainImage(s.h, nullptr, &idx);
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s.h, &wi);
    xrReleaseSwapchainImage(s.h, nullptr);
}

struct Ev { XrStructureType type; XrSessionState state; XrEventDataBuffer raw; };
std::vector<Ev> PollAll(XrResult* last = nullptr)
{
    FN(xrPollEvent);
    std::vector<Ev> out;
    for (;;)
    {
        XrEventDataBuffer b{ XR_TYPE_EVENT_DATA_BUFFER };
        const XrResult r = xrPollEvent(gInst, &b);
        if (r != XR_SUCCESS) { if (last) *last = r; break; }
        Ev e{ b.type, XR_SESSION_STATE_UNKNOWN, b };
        if (b.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) e.state = ((XrEventDataSessionStateChanged*)&b)->state;
        out.push_back(e);
    }
    return out;
}
std::string States(const std::vector<Ev>& ev)
{
    std::string s;
    for (auto& e : ev)
    {
        if (!s.empty()) s += ",";
        if (e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) s += std::to_string((int)e.state);
        else s += "ev" + std::to_string((int)e.type);
    }
    return s;
}
std::string StatesOf(std::initializer_list<XrSessionState> l)
{
    std::string s; for (auto x : l) { if (!s.empty()) s += ","; s += std::to_string((int)x); } return s;
}

// Everything that belongs to one session (recreated with it).
struct World
{
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE, aimR = XR_NULL_HANDLE;
    Swap eye[2], panel, reticle, backL, backR, hand, never;
};

XrSession MakeSession()
{
    FN(xrCreateSession);
    XrGraphicsBindingD3D11KHR gb{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; gb.device = gDev;
    XrSessionCreateInfo ci{ XR_TYPE_SESSION_CREATE_INFO }; ci.next = &gb; ci.systemId = 1;
    XrSession s = XR_NULL_HANDLE;
    const XrResult r = xrCreateSession(gInst, &ci, &s);
    return r == XR_SUCCESS ? s : XR_NULL_HANDLE;
}

XrAction gAimAction = XR_NULL_HANDLE;
void MakeWorld(World& w)
{
    FN(xrCreateReferenceSpace); FN(xrCreateActionSet); FN(xrCreateAction); FN(xrCreateActionSpace);
    XrReferenceSpaceCreateInfo rc{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; rc.poseInReferenceSpace.orientation.w = 1;
    rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    Check(xrCreateReferenceSpace(w.session, &rc, &w.local) == XR_SUCCESS, "create LOCAL space");
    rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    Check(xrCreateReferenceSpace(w.session, &rc, &w.view) == XR_SUCCESS, "create VIEW space");
    if (!gAimAction)
    {
        XrActionSetCreateInfo sci{ XR_TYPE_ACTION_SET_CREATE_INFO }; strcpy_s(sci.actionSetName, "test"); strcpy_s(sci.localizedActionSetName, "test");
        XrActionSet set; xrCreateActionSet(gInst, &sci, &set);
        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO }; aci.actionType = XR_ACTION_TYPE_POSE_INPUT; strcpy_s(aci.actionName, "aim_r"); strcpy_s(aci.localizedActionName, "aim_r");
        xrCreateAction(set, &aci, &gAimAction);
    }
    XrActionSpaceCreateInfo asi{ XR_TYPE_ACTION_SPACE_CREATE_INFO }; asi.action = gAimAction; asi.poseInActionSpace.orientation.w = 1;
    Check(xrCreateActionSpace(w.session, &asi, &w.aimR) == XR_SUCCESS, "create aim_r action space");

    const int64_t SRGB = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, UNORM = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (int e = 0; e < 2; e++)
        w.eye[e] = MakeSwap(w.session, 1232, 1360, SRGB, [e](int x, int y, uint8_t* p) {
            // A grid on grey; left eye tinted red, right eye green; a white cross at the image centre.
            const bool grid = (x % 64) < 2 || (y % 64) < 2;
            const bool cross = (abs(x - 616) < 3 && abs(y - 680) < 60) || (abs(y - 680) < 3 && abs(x - 616) < 60);
            uint8_t g = grid ? 200 : 90;
            p[0] = cross ? 255 : (uint8_t)(e == 0 ? g + 30 > 255 ? 255 : g + 30 : g);
            p[1] = cross ? 255 : (uint8_t)(e == 1 ? g + 30 > 255 ? 255 : g + 30 : g);
            p[2] = cross ? 255 : g; p[3] = 255; });
    // A UI panel: premultiplied, an opaque white border, a translucent blue body (alpha 0.5), an opaque yellow bar.
    w.panel = MakeSwap(w.session, 1700, 1060, SRGB, [](int x, int y, uint8_t* p) {
        const bool border = x < 20 || y < 20 || x >= 1680 || y >= 1040;
        const bool bar = y > 480 && y < 580 && x > 200 && x < 1500;
        if (border) { p[0] = p[1] = p[2] = 255; p[3] = 255; return; }
        if (bar) { p[0] = 255; p[1] = 220; p[2] = 0; p[3] = 255; return; }
        const float a = 0.5f;
        p[0] = Enc(0.1f * a); p[1] = Enc(0.3f * a); p[2] = Enc(0.8f * a); p[3] = (uint8_t)(a * 255 + 0.5f); });
    // A reticle: 64x64 UNORM, unpremultiplied orange disc, transparent outside.
    w.reticle = MakeSwap(w.session, 64, 64, UNORM, [](int x, int y, uint8_t* p) {
        const float dx = x - 31.5f, dy = y - 31.5f; const bool in = dx * dx + dy * dy < 28 * 28;
        p[0] = 255; p[1] = 128; p[2] = 0; p[3] = in ? 255 : 0; });
    // Per-eye backdrops: opaque magenta (left eye only) and cyan (right eye only).
    w.backL = MakeSwap(w.session, 256, 256, SRGB, [](int, int, uint8_t* p) { p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255; });
    w.backR = MakeSwap(w.session, 256, 256, SRGB, [](int, int, uint8_t* p) { p[0] = 0; p[1] = 255; p[2] = 255; p[3] = 255; });
    // A hand-held card: opaque green with a dark stripe.
    w.hand = MakeSwap(w.session, 128, 64, SRGB, [](int x, int, uint8_t* p) { const bool s = x > 56 && x < 72; p[0] = 0; p[1] = s ? 60 : 200; p[2] = 0; p[3] = 255; });
    // Never acquired nor released: a layer showing it is invalid.
    w.never = MakeSwap(w.session, 64, 64, SRGB, [](int, int, uint8_t* p) { p[0] = p[1] = p[2] = p[3] = 255; });
    for (const Swap* s : { &w.eye[0], &w.eye[1], &w.panel, &w.reticle, &w.backL, &w.backR, &w.hand }) Cycle(*s);
}

XrCompositionLayerQuad Quad(XrSpace space, const Swap& s, XrPosef pose, float wm, float hm, XrCompositionLayerFlags flags, XrEyeVisibility vis)
{
    XrCompositionLayerQuad q{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = flags; q.space = space; q.eyeVisibility = vis;
    q.subImage.swapchain = s.h; q.subImage.imageRect = { { 0, 0 }, { (int32_t)s.w, (int32_t)s.h_ } };
    q.pose = pose; q.size = { wm, hm };
    return q;
}
XrPosef P(float x, float y, float z) { return { { 0, 0, 0, 1 }, { x, y, z } }; }

struct FrameOut { XrResult wait = XR_SUCCESS, begin = XR_SUCCESS, end = XR_SUCCESS; XrFrameState fs{ XR_TYPE_FRAME_STATE }; double waitSec = 0; };

/** One whole frame. `layers` picks what is submitted: 'p' projection, 'q' the standard quads, 'Q' quads only. */
FrameOut Frame(World& w, const char* layers = "pq", const std::function<void(std::vector<XrCompositionLayerBaseHeader*>&)>& extra = nullptr)
{
    FN(xrWaitFrame); FN(xrBeginFrame); FN(xrEndFrame); FN(xrLocateViews);
    FrameOut o;
    XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
    const double t0 = NowSec();
    o.wait = xrWaitFrame(w.session, &wi, &o.fs);
    o.waitSec = NowSec() - t0;
    if (o.wait != XR_SUCCESS) return o;
    o.begin = xrBeginFrame(w.session, nullptr);
    if (o.begin != XR_SUCCESS) return o;
    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO }; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = o.fs.predictedDisplayTime; li.space = w.local;
    XrViewState vs{ XR_TYPE_VIEW_STATE }; XrView v[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }; uint32_t n = 0;
    xrLocateViews(w.session, &li, &vs, 2, &n, v);
    XrCompositionLayerProjectionView pv[2];
    for (int e = 0; e < 2; e++)
    {
        Cycle(w.eye[e]);
        pv[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        pv[e].pose = v[e].pose; pv[e].fov = v[e].fov;
        pv[e].subImage.swapchain = w.eye[e].h; pv[e].subImage.imageRect = { { 0, 0 }, { 1232, 1360 } };
    }
    XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    proj.space = w.local; proj.viewCount = 2; proj.views = pv;
    static XrCompositionLayerQuad q[5];
    q[0] = Quad(w.local, w.panel, P(0.0f, 1.6f, -2.0f), 1.7f, 1.06f, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, XR_EYE_VISIBILITY_BOTH);
    q[1] = Quad(w.view, w.reticle, P(0.0f, 0.0f, -1.0f), 0.04f, 0.04f,
                XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT, XR_EYE_VISIBILITY_BOTH);
    q[2] = Quad(w.local, w.backL, P(-0.9f, 1.3f, -1.5f), 0.4f, 0.4f, 0, XR_EYE_VISIBILITY_LEFT);
    q[3] = Quad(w.local, w.backR, P(-0.9f, 1.3f, -1.5f), 0.4f, 0.4f, 0, XR_EYE_VISIBILITY_RIGHT);
    q[4] = Quad(w.aimR, w.hand, P(0.0f, 0.04f, 0.0f), 0.12f, 0.06f, 0, XR_EYE_VISIBILITY_BOTH);
    std::vector<XrCompositionLayerBaseHeader*> L;
    if (strchr(layers, 'p')) L.push_back((XrCompositionLayerBaseHeader*)&proj);
    if (strchr(layers, 'q') || strchr(layers, 'Q')) for (auto& x : q) L.push_back((XrCompositionLayerBaseHeader*)&x);
    if (extra) extra(L);
    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = o.fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = (uint32_t)L.size(); ei.layers = L.data();
    o.end = xrEndFrame(w.session, &ei);
    return o;
}

bool WaitFile(const char* path, double sec)
{
    const double t0 = NowSec();
    while (NowSec() - t0 < sec) { if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true; Sleep(20); }
    return false;
}

/** Runs frames until a file appears (or n frames). */
void FramesUntil(World& w, const char* path, int n, const char* layers = "pq")
{
    for (int i = 0; i < n && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES; i++) Frame(w, layers);
}

/** Polls (running frames meanwhile when `frames`) until the given states have arrived, in order. */
bool ExpectStates(World* w, std::initializer_list<XrSessionState> want, double sec, std::string* got)
{
    std::vector<Ev> all;
    const double t0 = NowSec();
    const std::string target = StatesOf(want);
    while (NowSec() - t0 < sec)
    {
        auto ev = PollAll();
        for (auto& e : ev) all.push_back(e);
        if (States(all).find(target) != std::string::npos) break;
        if (w) Frame(*w, "pq"); else Sleep(10);
    }
    *got = States(all);
    return got->find(target) != std::string::npos;
}

float YawDeg(const XrQuaternionf& q) { return atan2f(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.y * q.y + q.x * q.x)) * 57.2957795f; }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: mock_harness <SunriseVR_MockXR.dll>\n"); return 100; }
    DeleteFileA("SVR_MockEvent.txt"); DeleteFileA("SVR_MockInput.txt"); DeleteFileA("SVR_MockCompose.txt"); DeleteFileA("SVR_MockMotion.txt");
    // Quest 3 through Link, paced at 72 Hz.
    WriteText("SVR_MockHmd.txt", "1232 1360 -54 40 44 -55 72 0.062\n");
    // Startup without a headset: the event file is read at xrCreateInstance / xrGetSystem.
    WriteText("SVR_MockEvent.txt", "nohmd 1.0   # the headset is off at launch\n");

    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 100; }
    typedef XrResult(XRAPI_PTR * PFN_neg)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*);
    PFN_neg neg = (PFN_neg)GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface");
    XrNegotiateLoaderInfo li{ XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof li, 1, 1, XR_MAKE_VERSION(1, 0, 0), XR_CURRENT_API_VERSION };
    XrNegotiateRuntimeRequest rq{ XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof rq };
    Check(neg && neg(&li, &rq) == XR_SUCCESS && rq.getInstanceProcAddr, "negotiate");
    gGpa = rq.getInstanceProcAddr;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &gDev, nullptr, &gCtx)), "WARP device");

    auto xrCreateInstance = Fn<PFN_xrCreateInstance>("xrCreateInstance");
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO }; strcpy_s(ici.applicationInfo.applicationName, "harness"); ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    const char* ext[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME }; ici.enabledExtensionCount = 1; ici.enabledExtensionNames = ext;
    Check(xrCreateInstance(&ici, &gInst) == XR_SUCCESS, "xrCreateInstance");
    FN(xrGetSystem); FN(xrDestroySession); FN(xrBeginSession); FN(xrEndSession); FN(xrRequestExitSession);
    FN(xrWaitFrame); FN(xrBeginFrame); FN(xrEndFrame); FN(xrLocateSpace); FN(xrDestroyInstance);

    // ---- (b) no headset at startup ----
    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = 0;
    Check(xrGetSystem(gInst, &sgi, &sys) == XR_ERROR_FORM_FACTOR_UNAVAILABLE, "nohmd: xrGetSystem -> FORM_FACTOR_UNAVAILABLE at startup");
    Sleep(1100);
    Check(xrGetSystem(gInst, &sgi, &sys) == XR_SUCCESS && sys == 1, "nohmd: xrGetSystem succeeds once the time is up");

    World w;
    w.session = MakeSession();
    Check(w.session != XR_NULL_HANDLE, "xrCreateSession");
    std::string got;
    Check(ExpectStates(nullptr, { XR_SESSION_STATE_IDLE, XR_SESSION_STATE_READY }, 1.0, &got), "new session: IDLE, READY (%s)", got.c_str());
    MakeWorld(w);
    {
        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        Check(xrWaitFrame(w.session, nullptr, &fs) == XR_ERROR_SESSION_NOT_RUNNING, "xrWaitFrame before xrBeginSession -> SESSION_NOT_RUNNING");
        Check(xrEndSession(w.session) == XR_ERROR_SESSION_NOT_RUNNING, "xrEndSession before xrBeginSession -> SESSION_NOT_RUNNING");
    }
    XrSessionBeginInfo sbi{ XR_TYPE_SESSION_BEGIN_INFO }; sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    Check(xrBeginSession(w.session, &sbi) == XR_SUCCESS, "xrBeginSession in READY");
    Check(xrBeginSession(w.session, &sbi) == XR_ERROR_SESSION_RUNNING, "second xrBeginSession -> SESSION_RUNNING");
    Check(ExpectStates(nullptr, { XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED }, 1.0, &got), "begun: SYNCHRONIZED, VISIBLE, FOCUSED (%s)", got.c_str());
    Check(xrEndSession(w.session) == XR_ERROR_SESSION_NOT_STOPPING, "xrEndSession while FOCUSED -> SESSION_NOT_STOPPING");

    // ---- call order ----
    Check(xrBeginFrame(w.session, nullptr) == XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame without xrWaitFrame -> CALL_ORDER_INVALID");
    {
        XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO }; ei.displayTime = 1; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        Check(xrEndFrame(w.session, &ei) == XR_ERROR_CALL_ORDER_INVALID, "xrEndFrame without xrBeginFrame -> CALL_ORDER_INVALID");
    }
    for (int i = 0; i < 5; i++) { FrameOut o = Frame(w); if (i == 4) Check(o.wait == XR_SUCCESS && o.begin == XR_SUCCESS && o.end == XR_SUCCESS && o.fs.shouldRender, "frame: projection + 5 quads accepted, shouldRender"); }

    // ---- 1. quads in the compositor ----
    WriteText("SVR_MockCompose.txt", "hq\n");
    FramesUntil(w, "SVR_seen_hq.txt", 10);
    std::string hq = ReadText("SVR_seen_hq.txt");
    Check(!hq.empty(), "compose hq written");
    Check(Count(hq, "quad ") >= 10, "hq: one quad line per quad and eye (%zu)", Count(hq, "quad "));
    Check(hq.find("quads 5 drawn 4 4") != std::string::npos, "hq: 4 of 5 quads drawn per eye (each backdrop in its own eye)");
    Check(hq.find("hidden from this eye") != std::string::npos, "hq: eyeVisibility honoured");
    WriteText("SVR_MockCompose.txt", "qonly\n");
    FramesUntil(w, "SVR_seen_qonly.txt", 10, "Q");
    std::string qo = ReadText("SVR_seen_qonly.txt");
    Check(qo.find("coverage 0 0.00000") != std::string::npos && qo.find("quads 5 drawn") != std::string::npos, "qonly: no projection, quads drawn");
    WriteText("SVR_MockCompose.txt", "seqq seq 3\n");
    FramesUntil(w, "SVR_seen_seqq_f02.txt", 12);
    Check(Count(ReadText("SVR_seen_seqq_f00.txt"), "quad ") >= 10 && Count(ReadText("SVR_seen_seqq_f02.txt"), "quad ") >= 10, "seq: quads composed in every frame of a sequence");

    // ---- xrEndFrame validation ----
    {
        static XrCompositionLayerQuad bad;
        FrameOut o = Frame(w, "p", [&](std::vector<XrCompositionLayerBaseHeader*>& L) {
            bad = Quad(w.local, w.panel, P(0, 1.6f, -2), 1, 1, 0, XR_EYE_VISIBILITY_BOTH); bad.subImage.swapchain = (XrSwapchain)(uintptr_t)0x1234; L.push_back((XrCompositionLayerBaseHeader*)&bad); });
        Check(o.end == XR_ERROR_HANDLE_INVALID, "quad with a dead swapchain handle -> HANDLE_INVALID (%d)", (int)o.end);
        o = Frame(w, "p", [&](std::vector<XrCompositionLayerBaseHeader*>& L) {
            bad = Quad(w.local, w.never, P(0, 1.6f, -2), 1, 1, 0, XR_EYE_VISIBILITY_BOTH); L.push_back((XrCompositionLayerBaseHeader*)&bad); });
        Check(o.end == XR_ERROR_LAYER_INVALID, "quad on a never-released swapchain -> LAYER_INVALID (%d)", (int)o.end);
        o = Frame(w, "p", [&](std::vector<XrCompositionLayerBaseHeader*>& L) {
            bad = Quad(w.local, w.reticle, P(0, 1.6f, -2), 1, 1, 0, XR_EYE_VISIBILITY_BOTH); bad.subImage.imageRect.extent.width = 65; L.push_back((XrCompositionLayerBaseHeader*)&bad); });
        Check(o.end == XR_ERROR_SWAPCHAIN_RECT_INVALID, "quad rect outside its swapchain -> SWAPCHAIN_RECT_INVALID (%d)", (int)o.end);
        o = Frame(w, "p", [&](std::vector<XrCompositionLayerBaseHeader*>& L) {
            bad = Quad((XrSpace)(uintptr_t)0x4321, w.reticle, P(0, 1.6f, -2), 1, 1, 0, XR_EYE_VISIBILITY_BOTH); L.push_back((XrCompositionLayerBaseHeader*)&bad); });
        Check(o.end == XR_ERROR_HANDLE_INVALID, "quad with a dead space handle -> HANDLE_INVALID (%d)", (int)o.end);
        o = Frame(w, "pq", [&](std::vector<XrCompositionLayerBaseHeader*>& L) { while (L.size() < 17) L.push_back(L.back()); });
        Check(o.end == XR_ERROR_LAYER_LIMIT_EXCEEDED, "17 layers -> LAYER_LIMIT_EXCEEDED (%d)", (int)o.end);
        o = Frame(w);
        Check(o.end == XR_SUCCESS, "a good frame after the rejected ones");
    }

    // ---- xrWaitFrame on its own thread, begin/end on this one ----
    {
        std::mutex mx; std::condition_variable cv; std::deque<XrFrameState> q; bool done = false; int waitErr = 0;
        std::thread waiter([&] {
            for (int i = 0; i < 60; i++)
            {
                XrFrameState fs{ XR_TYPE_FRAME_STATE };
                if (xrWaitFrame(w.session, nullptr, &fs) != XR_SUCCESS) waitErr++;
                std::lock_guard<std::mutex> lk(mx); q.push_back(fs); cv.notify_all();
            }
            std::lock_guard<std::mutex> lk(mx); done = true; cv.notify_all();
        });
        int ok = 0, frames = 0;
        const double t0 = NowSec();
        for (;;)
        {
            XrFrameState fs;
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [&] { return !q.empty() || done; });
                if (q.empty()) break;
                fs = q.front(); q.pop_front();
            }
            frames++;
            if (xrBeginFrame(w.session, nullptr) != XR_SUCCESS) continue;
            for (int e = 0; e < 2; e++) Cycle(w.eye[e]);
            static XrCompositionLayerQuad rq;
            rq = Quad(w.view, w.reticle, P(0, 0, -1), 0.04f, 0.04f, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, XR_EYE_VISIBILITY_BOTH);
            const XrCompositionLayerBaseHeader* L[] = { (const XrCompositionLayerBaseHeader*)&rq };
            XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO }; ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            ei.layerCount = 1; ei.layers = L;
            if (xrEndFrame(w.session, &ei) == XR_SUCCESS) ok++;
        }
        waiter.join();
        const double el = NowSec() - t0;
        Check(waitErr == 0 && ok == 60 && frames == 60, "threaded frame loop: 60 waits on one thread, 60 begin/end on another (%d ok, %d wait errors)", ok, waitErr);
        Check(el > 60.0 / 72.0 * 0.8 && el < 3.0, "threaded loop paced at ~72 Hz (%.2f s for 60 frames)", el);
    }

    // ---- (c) stall: HOURGLASS ----
    WriteText("SVR_MockEvent.txt", "stall 1.5\n");
    {
        double longest = 0;
        for (int i = 0; i < 4; i++) { FrameOut o = Frame(w); if (o.waitSec > longest) longest = o.waitSec; }
        Check(longest > 1.4 && longest < 2.5, "stall: one xrWaitFrame blocked %.2f s", longest);
        const std::string log = ReadText("SVR_MockXR.log");
        Check(log.find("ev=mock event=stall") != std::string::npos, "stall: logged as ev=mock event=stall");
        Check(log.find("HOURGLASS #1: no frame for 1.") != std::string::npos, "stall: HOURGLASS logged for the gap");
        Check(log.find("HOURGLASS ongoing") != std::string::npos, "stall: the watchdog saw the open gap");
    }

    // ---- (f) recentre: LOCAL moves to the head ----
    {
        // turnX trigR handYaw handPitch headYaw headPitch headX headY headZ: head turned 30 deg, 0.3 m to the right
        WriteText("SVR_MockInput.txt", "0 0 0 0 30 0 0.3 0 0\n");
        FrameOut o = Frame(w);
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(w.view, w.local, o.fs.predictedDisplayTime, &loc);
        Check(fabsf(loc.pose.position.x - 0.3f) < 1e-3f && fabsf(loc.pose.position.y - 1.6f) < 1e-3f && fabsf(YawDeg(loc.pose.orientation) - 30.f) < 0.1f,
              "before recentre: VIEW in LOCAL at (%.3f,%.3f,%.3f) yaw %.1f", loc.pose.position.x, loc.pose.position.y, loc.pose.position.z, YawDeg(loc.pose.orientation));
        WriteText("SVR_MockEvent.txt", "recentre\n");
        Frame(w);
        auto ev = PollAll();
        const XrEventDataReferenceSpaceChangePending* rc = nullptr;
        for (auto& e : ev) if (e.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) rc = (const XrEventDataReferenceSpaceChangePending*)&e.raw;
        Check(rc != nullptr, "recentre: REFERENCE_SPACE_CHANGE_PENDING delivered");
        if (rc)
        {
            const XrTime tc = rc->changeTime;
            Check(rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL && rc->poseValid && fabsf(rc->poseInPreviousSpace.position.x - 0.3f) < 1e-3f &&
                  fabsf(YawDeg(rc->poseInPreviousSpace.orientation) - 30.f) < 0.1f,
                  "recentre: poseInPreviousSpace = the old head (%.3f,%.3f,%.3f) yaw %.1f", rc->poseInPreviousSpace.position.x, rc->poseInPreviousSpace.position.y,
                  rc->poseInPreviousSpace.position.z, YawDeg(rc->poseInPreviousSpace.orientation));
            XrSpaceLocation a{ XR_TYPE_SPACE_LOCATION }, b{ XR_TYPE_SPACE_LOCATION }, h{ XR_TYPE_SPACE_LOCATION };
            xrLocateSpace(w.view, w.local, tc - 1, &a);
            xrLocateSpace(w.view, w.local, tc + 1, &b);
            xrLocateSpace(w.aimR, w.local, tc + 1, &h);
            Check(fabsf(a.pose.position.x - 0.3f) < 1e-3f, "recentre: before changeTime the old LOCAL (x %.3f)", a.pose.position.x);
            Check(fabsf(b.pose.position.x) < 1e-3f && fabsf(b.pose.position.y) < 1e-3f && fabsf(b.pose.position.z) < 1e-3f && fabsf(YawDeg(b.pose.orientation)) < 0.1f,
                  "recentre: from changeTime the head is LOCAL's origin (%.3f,%.3f,%.3f) yaw %.2f", b.pose.position.x, b.pose.position.y, b.pose.position.z, YawDeg(b.pose.orientation));
            Check(fabsf(h.pose.position.x - 0.0634f) < 2e-3f && fabsf(h.pose.position.y + 0.2f) < 2e-3f && fabsf(h.pose.position.z + 0.3098f) < 2e-3f,
                  "recentre: the hand moves with LOCAL (%.4f,%.4f,%.4f), expected (0.0634,-0.2,-0.3098)", h.pose.position.x, h.pose.position.y, h.pose.position.z);
        }
        Check(ReadText("SVR_MockXR.log").find("ev=mock event=recentre") != std::string::npos, "recentre: logged");
        // Compose after it: the panel at LOCAL (0,1.6,-2) is now 1.6 m above the eye -> visible check of the jump.
        WriteText("SVR_MockCompose.txt", "recentred\n");
        FramesUntil(w, "SVR_seen_recentred.txt", 10);
    }

    // ---- (d) stop and offered back ----
    {
        WriteText("SVR_MockEvent.txt", "stop 1.0\n");
        Check(ExpectStates(&w, { XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING }, 2.0, &got),
              "stop: VISIBLE, SYNCHRONIZED, STOPPING (%s)", got.c_str());
        FrameOut o = Frame(w);
        Check(o.wait == XR_SUCCESS && !o.fs.shouldRender, "stop: frames still run in STOPPING, shouldRender false");
        Check(xrEndSession(w.session) == XR_SUCCESS, "stop: xrEndSession in STOPPING");
        Check(Frame(w).wait == XR_ERROR_SESSION_NOT_RUNNING, "stop: xrWaitFrame after xrEndSession -> SESSION_NOT_RUNNING");
        Sleep(400);
        Check(States(PollAll()) == StatesOf({ XR_SESSION_STATE_IDLE }), "stop: IDLE, and no READY yet after 0.4 s");
        Check(ExpectStates(nullptr, { XR_SESSION_STATE_READY }, 2.0, &got), "stop: READY again ~1 s after xrEndSession (%s)", got.c_str());
        Check(xrBeginSession(w.session, &sbi) == XR_SUCCESS, "stop: xrBeginSession again");
        Check(ExpectStates(nullptr, { XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED }, 1.0, &got), "stop: running again (%s)", got.c_str());
        Check(Frame(w).end == XR_SUCCESS, "stop: frames again");
    }

    // ---- xrRequestExitSession -> EXITING; then a NEW session ----
    {
        Check(xrRequestExitSession(w.session) == XR_SUCCESS, "xrRequestExitSession");
        Check(ExpectStates(&w, { XR_SESSION_STATE_STOPPING }, 2.0, &got), "request exit: STOPPING (%s)", got.c_str());
        Check(xrEndSession(w.session) == XR_SUCCESS, "request exit: xrEndSession");
        Check(ExpectStates(nullptr, { XR_SESSION_STATE_IDLE, XR_SESSION_STATE_EXITING }, 1.0, &got), "request exit: IDLE, EXITING (%s)", got.c_str());
        Check(xrDestroySession(w.session) == XR_SUCCESS, "destroy session");
        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        Check(xrWaitFrame(w.session, nullptr, &fs) == XR_ERROR_HANDLE_INVALID, "destroyed session handle -> HANDLE_INVALID");
        w = World{}; w.session = MakeSession();
        Check(w.session != XR_NULL_HANDLE && ExpectStates(nullptr, { XR_SESSION_STATE_IDLE, XR_SESSION_STATE_READY }, 1.0, &got), "second session: IDLE, READY (%s)", got.c_str());
        MakeWorld(w);
        Check(xrBeginSession(w.session, &sbi) == XR_SUCCESS && ExpectStates(nullptr, { XR_SESSION_STATE_FOCUSED }, 1.0, &got), "second session runs");
        Check(Frame(w).end == XR_SUCCESS, "second session: frames");
    }

    // ---- (e) exit from the runtime ----
    {
        WriteText("SVR_MockEvent.txt", "exit\n");
        Check(ExpectStates(&w, { XR_SESSION_STATE_STOPPING }, 2.0, &got), "exit: STOPPING (%s)", got.c_str());
        Check(xrEndSession(w.session) == XR_SUCCESS && ExpectStates(nullptr, { XR_SESSION_STATE_IDLE, XR_SESSION_STATE_EXITING }, 1.0, &got),
              "exit: IDLE, EXITING after xrEndSession (%s)", got.c_str());
        xrDestroySession(w.session);
        w = World{}; w.session = MakeSession(); ExpectStates(nullptr, { XR_SESSION_STATE_READY }, 1.0, &got); MakeWorld(w);
        Check(xrBeginSession(w.session, &sbi) == XR_SUCCESS && ExpectStates(nullptr, { XR_SESSION_STATE_FOCUSED }, 1.0, &got), "third session runs");
        Frame(w);
    }

    // ---- (a) loss, with the headset gone for a second ----
    {
        WriteText("SVR_MockEvent.txt", "loss\nnohmd 1.0\n");
        FrameOut o = Frame(w);
        Check(o.wait == XR_SESSION_LOSS_PENDING, "loss: xrWaitFrame -> XR_SESSION_LOSS_PENDING before the event is polled (%d)", (int)o.wait);
        auto ev = PollAll();
        Check(States(ev).find(std::to_string((int)XR_SESSION_STATE_LOSS_PENDING)) != std::string::npos, "loss: LOSS_PENDING delivered (%s)", States(ev).c_str());
        o = Frame(w);
        Check(o.wait == XR_ERROR_SESSION_LOST, "loss: then xrWaitFrame -> SESSION_LOST (%d)", (int)o.wait);
        Check(xrBeginFrame(w.session, nullptr) == XR_ERROR_SESSION_LOST, "loss: xrBeginFrame -> SESSION_LOST");
        Check(xrDestroySession(w.session) == XR_SUCCESS, "loss: destroy the lost session");
        Check(MakeSession() == XR_NULL_HANDLE, "loss: no new session while the headset is gone");
        Check(xrGetSystem(gInst, &sgi, &sys) == XR_ERROR_FORM_FACTOR_UNAVAILABLE, "loss: xrGetSystem -> FORM_FACTOR_UNAVAILABLE");
        Sleep(1100);
        Check(xrGetSystem(gInst, &sgi, &sys) == XR_SUCCESS, "loss: the headset is back");
        w = World{}; w.session = MakeSession();
        Check(w.session != XR_NULL_HANDLE && ExpectStates(nullptr, { XR_SESSION_STATE_IDLE, XR_SESSION_STATE_READY }, 1.0, &got), "loss: a new session is READY at once (%s)", got.c_str());
        MakeWorld(w);
        Check(xrBeginSession(w.session, &sbi) == XR_SUCCESS && ExpectStates(nullptr, { XR_SESSION_STATE_FOCUSED }, 1.0, &got), "loss: the new session runs");
        Check(Frame(w).end == XR_SUCCESS, "loss: frames in the new session");
    }

    // ---- (g) instance loss ----
    {
        WriteText("SVR_MockEvent.txt", "instanceloss\n");
        Frame(w);
        XrResult last = XR_SUCCESS;
        auto ev = PollAll(&last);
        bool sawLoss = false; for (auto& e : ev) if (e.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) sawLoss = true;
        Check(sawLoss && last == XR_ERROR_INSTANCE_LOST, "instanceloss: INSTANCE_LOSS_PENDING, then xrPollEvent -> INSTANCE_LOST (%d)", (int)last);
        Check(Frame(w).wait == XR_ERROR_INSTANCE_LOST, "instanceloss: xrWaitFrame -> INSTANCE_LOST");
        Check(xrGetSystem(gInst, &sgi, &sys) == XR_ERROR_INSTANCE_LOST, "instanceloss: xrGetSystem -> INSTANCE_LOST");
        xrDestroySession(w.session);
        Check(xrDestroyInstance(gInst) == XR_SUCCESS, "instanceloss: destroy the instance");
        gInst = XR_NULL_HANDLE;
        Check(xrCreateInstance(&ici, &gInst) == XR_SUCCESS && xrGetSystem(gInst, &sgi, &sys) == XR_SUCCESS, "instanceloss: a new instance works");
        w = World{}; w.session = MakeSession();
        Check(w.session != XR_NULL_HANDLE && ExpectStates(nullptr, { XR_SESSION_STATE_READY }, 1.0, &got), "instanceloss: a new session in the new instance");
        MakeWorld(w);
        Check(xrBeginSession(w.session, &sbi) == XR_SUCCESS && ExpectStates(nullptr, { XR_SESSION_STATE_FOCUSED }, 1.0, &got) && Frame(w).end == XR_SUCCESS,
              "instanceloss: frames again");
    }

    {
        const std::string log = ReadText("SVR_MockXR.log");
        for (const char* word : { "event=nohmd", "event=stall", "event=recentre", "event=stop", "event=exit", "event=loss", "event=instanceloss" })
            Check(log.find(std::string("ev=mock ") + word) != std::string::npos, "log has ev=mock %s", word);
        Check(Count(log, "HOURGLASS #") == 1, "exactly one HOURGLASS (the stall's) in the whole run (%zu)", Count(log, "HOURGLASS #"));
        Check(GetFileAttributesA("SVR_MockEvent.txt") == INVALID_FILE_ATTRIBUTES, "the event file is consumed");
    }
    xrDestroySession(w.session);
    xrDestroyInstance(gInst);
    printf("\n%d passed, %d failed\n", gPasses, gFails);
    return gFails;
}
