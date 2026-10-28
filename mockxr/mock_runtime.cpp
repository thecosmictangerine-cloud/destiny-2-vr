// Mock OpenXR runtime for headless development of a VR mod. TEST TOOL ONLY. Carried over from an
// earlier project, which is why it still names itself DishonoredVR below.
//
// It reports a fake stereo HMD, accepts a D3D11 session, hands out real D3D11 swapchain textures,
// runs the session through the states a real runtime does (in the legal order, queued), and, on
// request, emulates the compositor: projection and quad layers composed into what each EYE sees.
// Lets the mod's entire OpenXR lifecycle (instance -> session -> swapchains -> frame loop ->
// submit -> loss and recovery) be exercised in-game with no headset and no real runtime.
//
// Files, all in the working directory (the GAME's, C:\Games\Sunrise):
//   SVR_MockHmd.txt      read once: "w h fovL fovR fovU fovD [hz] [ipd]"            (ReadHmdConfig)
//   SVR_MockInput.txt    poses and buttons, re-read whenever it changes            (ReadMockInput)
//   SVR_MockMotion.txt   head motion as a function of time, one term per line, summed:
//                        "yaw|pitch|roll <amplitude deg> <rate rad/s>" (a sine), or "shake <deg>": a
//                        standing person's head tremor of that size (incommensurate sines on all three
//                        axes, 0.2 to 6 Hz -- breathing, sway, the neck's micro-corrections)
//   SVR_MockCompose.txt  "<tag> [world] [seq n] [full] [fovlock] [poselock]": compose what each eye
//                        sees into SVR_seen_<tag>_eye<i>.bmp + SVR_seen_<tag>.txt (the compositor below)
//   SVR_MockEvent.txt    failure emulation, ONE-SHOT: read about once a frame (and from xrPollEvent,
//                        xrGetSystem, xrCreateInstance), every line executed once, then the file is
//                        deleted (if it cannot be, its write time is remembered so it never runs twice).
//                        Write it with a trailing newline: a file without one is taken as half written.
//                        One command per line, '#' starts a comment:
//       loss            the headset's connection is lost: XR_SESSION_STATE_LOSS_PENDING is queued; until
//                       it is polled the frame calls return XR_SESSION_LOSS_PENDING, after it every
//                       session call returns XR_ERROR_SESSION_LOST until the app destroys the session.
//       nohmd [s]       no headset: xrGetSystem returns XR_ERROR_FORM_FACTOR_UNAVAILABLE (and
//                       xrCreateSession XR_ERROR_SYSTEM_INVALID) for s seconds; s omitted or < 0 = until
//                       `hmd`. Written before the game starts, it emulates a headset that is off at launch.
//       hmd             the headset is back: ends a `nohmd` at once (`nohmd 0` does the same).
//       stall <s>       the next xrWaitFrame of a running session blocks for s seconds (one-shot): the
//                       compositor not releasing the app. Shows up as a HOURGLASS line.
//       stop [s]        the runtime takes the session away, as a headset taken off does: FOCUSED ->
//                       VISIBLE -> SYNCHRONIZED -> STOPPING; after the app's xrEndSession, IDLE; then READY
//                       again s seconds after that xrEndSession. s omitted or < 0 = never offered back.
//       exit            the runtime asks the app to quit: ... -> STOPPING, then after xrEndSession IDLE and
//                       EXITING (from IDLE/READY straight to EXITING).
//       recentre        (or recenter) the user's recentre: XrEventDataReferenceSpaceChangePending for
//                       LOCAL, and LOCAL's origin REALLY moves, at changeTime, to the head's position and
//                       yaw at that time: every pose (head, views, hands, layer spaces) located for a time
//                       at or after changeTime is expressed in the new LOCAL, so poses jump as on a Quest.
//       instanceloss    XrEventDataInstanceLossPending; once polled, XR_ERROR_INSTANCE_LOST from every
//                       instance-level call until the app destroys the instance and creates a new one.
//                       Each executed command is logged as "ev=mock event=<name> ...".
//   SVR_MockXR.log       the log, shared-readable. Appended (not truncated) when the runtime is reloaded
//                        within the same process. Every gap of more than 1 s between two xrEndFrame of a
//                        running, not stopping session is logged as "HOURGLASS #n: no frame for X s"; a
//                        watchdog also logs "HOURGLASS ongoing" while such a gap is still open, so a test
//                        can prove the headset was never left without frames (and see it if it was).
//
// The session state machine (spec order): xrCreateSession queues IDLE, READY. xrBeginSession (only in
// READY) queues SYNCHRONIZED, VISIBLE, FOCUSED. xrEndSession only in STOPPING (else
// XR_ERROR_SESSION_NOT_STOPPING), queues IDLE. xrRequestExitSession queues the way down to STOPPING and
// EXITING after xrEndSession. xrWaitFrame/xrBeginFrame/xrEndFrame return XR_ERROR_SESSION_NOT_RUNNING
// outside xrBeginSession..xrEndSession and check the call order (xrBeginFrame needs a waited frame,
// xrEndFrame a begun one; a second xrWaitFrame waits, up to 1 s, for the previous frame's xrBeginFrame).
// They may be called from different threads (xrWaitFrame on one, begin/end on another): the shared
// state is under one mutex. shouldRender is false outside VISIBLE/FOCUSED; outside FOCUSED, xrSyncActions
// returns XR_SESSION_NOT_FOCUSED and actions are inactive. A new session or instance can be created
// after the old one is destroyed (the app's recovery after a loss); a session's spaces and swapchains
// are destroyed with it, so a stale handle used afterwards is XR_ERROR_HANDLE_INVALID. xrEndFrame
// validates what it is given (live swapchain/space handles, a released image, the rect inside the
// swapchain, at most 16 layers, projection and quad types only) and logs "xrEndFrame REJECTED <why>".
// Spaces: LOCAL, STAGE (the room, which a recentre does not move) and VIEW (head-locked) reference
// spaces and the controllers' action spaces; any of them can be the base of xrLocateSpace and
// xrLocateViews, and any can carry a quad layer.
//
// Standalone test, no game needed: mockxr\tests\Run-MockHarness.ps1 (mock_harness.cpp).
#include <vector>
#include <string>
#include <algorithm>
#include <deque>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <share.h>
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
constexpr uint32_t kMaxLayers = 16;   // what xrGetSystemProperties reports

// Optional HMD emulation: SVR_MockHmd.txt (cwd) = "w h fovL fovR fovU fovD [hz] [ipd]"
// (fovs in degrees for the LEFT eye; the right eye is mirrored, as real HMDs
// are). Lets the mock reproduce hardware geometry headless. Quest 3 as measured on
// the user's headset through Link (2026-09-09): "1824 1968 -54 40 44 -55".
// Missing file = legacy 1024x1024 symmetric 90.
//   hz:  > 0 makes xrWaitFrame pace the app like a compositor does, releasing it once per display
//        period; 0 (default) returns at once. Frame pacing decides which frame carries which eye
//        in alternating-eye stereo, so a test of that has to run paced.
//   ipd: the distance between the eyes in metres (default 0.064).
uint32_t gEyeW = kEyeW, gEyeH = kEyeH;
float gFovDeg[4] = { -45.0f, 45.0f, 45.0f, -45.0f }; // L R U D, left eye
float gPaceHz = 0.0f;
float gIpd = 0.064f;
bool gHmdCfgRead = false;
void ReadHmdConfig()
{
    if (gHmdCfgRead) return;
    gHmdCfgRead = true;
    FILE* f = nullptr;
    fopen_s(&f, "SVR_MockHmd.txt", "r");
    if (!f) return;
    unsigned w = 0, h = 0;
    float l = 0, r = 0, u = 0, d = 0, hz = 0, ipd = 0;
    const int n = fscanf_s(f, "%u %u %f %f %f %f %f %f", &w, &h, &l, &r, &u, &d, &hz, &ipd);
    if (n >= 6 && w && h)
    {
        gEyeW = w; gEyeH = h;
        gFovDeg[0] = l; gFovDeg[1] = r; gFovDeg[2] = u; gFovDeg[3] = d;
    }
    if (n >= 7 && hz >= 0.0f && hz < 1000.0f) gPaceHz = hz;
    if (n >= 8 && ipd > 0.0f && ipd < 0.2f) gIpd = ipd;
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
    uint32_t arraySize = 1;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::vector<ID3D11Texture2D*> tex;
    uint32_t acquired = 0;
    /** The image handed out by the last acquire, and the last one released: what a layer shows. */
    int32_t current = -1;
    int32_t released = -1;
    /** Releases so far, so the compositor can tell a fresh image from one held over. */
    uint64_t releases = 0;
};

bool IsSrgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }

/**
 * A space: a reference space (LOCAL, STAGE, VIEW) or an action space (a controller pose). Both are
 * located in LOCAL first and then related to each other, so any space can be the base of any other.
 * `palm` marks a space created from a `palm_*` action, i.e. one bound to OpenXR's grip/pose rather
 * than aim/pose. Real hardware puts those two several centimetres apart, so the mock does too --
 * otherwise confusing one for the other is undetectable here and only shows up in the headset.
 */
struct MockSpace
{
    bool action = false;
    XrReferenceSpaceType ref = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrPosef offset = { { 0, 0, 0, 1 }, { 0, 0, 0 } };   // poseInReferenceSpace / poseInActionSpace
    int hand = 0;
    bool palm = false;
    std::string name;
};

// Single instance and session (handles are their addresses; spaces and swapchains are heap objects).
struct XrInstance_T { int _; } gInstance;
struct XrSession_T { int _; } gSession;

ID3D11Device* gDevice = nullptr;

/** Monotonic nanoseconds. */
int64_t NowNs()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
double Sec(int64_t ns) { return (double)ns * 1e-9; }

FILE* gLog = nullptr;
std::mutex gLogM;   // the app calls in from more than one thread
void L(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(gLogM);
    // _fsopen with _SH_DENYNO, not fopen_s: fopen_s opens the file exclusively, and then no test
    // can read the log while the game holds it. Truncated once per PROCESS: an app that unloads and
    // reloads the runtime (a new instance after a loss) would otherwise lose everything before it.
    if (!gLog)
    {
        const bool first = GetEnvironmentVariableA("SVR_MOCKXR_LOG_OPEN", nullptr, 0) == 0;
        SetEnvironmentVariableA("SVR_MOCKXR_LOG_OPEN", "1");
        gLog = _fsopen("SVR_MockXR.log", first ? "w" : "a", _SH_DENYNO);
    }
    if (!gLog) return;
    va_list a; va_start(a, fmt); vfprintf(gLog, fmt, a); va_end(a);
    fprintf(gLog, "\n"); fflush(gLog);
}

/** Blocks until t (QPC ns), with a high-resolution timer (Sleep's granularity can be 15.6 ms). */
void SleepUntilNs(int64_t t)
{
    static thread_local HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    for (;;)
    {
        const int64_t now = NowNs();
        if (now >= t) return;
        if (tm)
        {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((t - now) / 100);
            if (SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(tm, 1000); continue; }
        }
        Sleep(t - now > 2000000 ? 1 : 0);
    }
}

// ---- Small pose math ----
/** q = a * b, both (x, y, z, w). */
XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
XrQuaternionf QConj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }

/** Rotates v by q. */
XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v)
{
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return { v.x + q.w * tx + (q.y * tz - q.z * ty),
             v.y + q.w * ty + (q.z * tx - q.x * tz),
             v.z + q.w * tz + (q.x * ty - q.y * tx) };
}
void QRotV(const XrQuaternionf& q, const float v[3], float o[3])
{   // o = q v q*
    const float u[3] = { q.x, q.y, q.z }, w = q.w;
    const float c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    const float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
    for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * (w * c1[i] + c2[i]);
}
void QRotVInv(const XrQuaternionf& q, const float v[3], float o[3]) { const XrQuaternionf c = { -q.x, -q.y, -q.z, q.w }; QRotV(c, v, o); }

/** a * b: b expressed in a's frame, taken out to a's parent. */
XrPosef PoseMul(const XrPosef& a, const XrPosef& b)
{
    const XrVector3f p = QRot(a.orientation, b.position);
    return { QMul(a.orientation, b.orientation), { a.position.x + p.x, a.position.y + p.y, a.position.z + p.z } };
}
XrPosef PoseInv(const XrPosef& a)
{
    const XrQuaternionf c = QConj(a.orientation);
    const XrVector3f p = QRot(c, a.position);
    return { c, { -p.x, -p.y, -p.z } };
}
XrQuaternionf QYaw(float yaw) { return { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) }; }

// ---- Session state machine and failure emulation ----
// Events are QUEUED and delivered one per xrPollEvent, in order. The runtime's own notion of the
// session state is the last state it queued (gState): that is what xrBeginSession/xrEndSession are
// checked against, and what shouldRender and focus follow.
enum class EvKind { State, RefSpace, InstanceLoss };
struct MockEvent
{
    EvKind kind = EvKind::State;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    XrTime time = 0;
    XrPosef pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };   // RefSpace: the new origin in the previous space
};
std::mutex gSm;
std::condition_variable gFrameCv;   // xrBeginFrame -> a second xrWaitFrame waiting for it
std::deque<MockEvent> gEvq;
bool gInstanceAlive = false, gInstanceLossQueued = false, gInstanceLost = false;
bool gSessionExists = false, gRunning = false, gExitPending = false, gLossQueued = false, gLost = false;
XrSessionState gState = XR_SESSION_STATE_UNKNOWN;
std::atomic<bool> gFocused{ false };    // gState == FOCUSED, readable without the lock
double gReadyAfterSec = -1.0;           // the pending `stop`: READY again this long after xrEndSession
int64_t gReadyAtNs = -1;                // when a stopped session is offered back; -1 = not scheduled
uint64_t gWaited = 0, gBegun = 0, gEnded = 0;   // frame call order
int64_t gNoHmdUntilNs = 0;              // 0 = a headset is there; INT64_MAX = until `hmd`
double gStallArmedSec = 0.0;            // the next xrWaitFrame blocks this long
std::atomic<int64_t> gLastEndNs{ 0 };   // the last xrEndFrame of a running session (or its xrBeginSession)
std::atomic<int64_t> gLastPredicted{ 0 };
unsigned gHourglasses = 0; double gHourglassMax = 0.0; bool gHourglassOngoing = false;
int64_t gFrameTime = 1;                 // frame COUNTER (the procedural head sweep); not a time

const char* StateName(XrSessionState s)
{
    switch (s)
    {
    case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "?";
    }
}

/** Under gSm. */
void QueueStateLocked(XrSessionState s)
{
    MockEvent e; e.kind = EvKind::State; e.state = s; e.time = NowNs();
    gEvq.push_back(e);
    gState = s;
    gFocused = (s == XR_SESSION_STATE_FOCUSED);
}
/** Under gSm: the legal way down from wherever the session is to STOPPING. */
void QueueStopLocked()
{
    for (;;)
    {
        if (gState == XR_SESSION_STATE_FOCUSED) QueueStateLocked(XR_SESSION_STATE_VISIBLE);
        else if (gState == XR_SESSION_STATE_VISIBLE) QueueStateLocked(XR_SESSION_STATE_SYNCHRONIZED);
        else if (gState == XR_SESSION_STATE_SYNCHRONIZED) QueueStateLocked(XR_SESSION_STATE_STOPPING);
        else return;
    }
}
/** Under gSm: no headset right now? (ends an expired `nohmd`, and logs it) */
bool NoHmdLocked()
{
    if (gNoHmdUntilNs == 0) return false;
    if (NowNs() >= gNoHmdUntilNs)
    {
        gNoHmdUntilNs = 0;
        L("ev=mock event=nohmd_end t=%.3f (the headset is back)", Sec(NowNs()));
        return false;
    }
    return true;
}
/** Under gSm: a session gap that ends because the session goes away while it was supposed to run. */
void HourglassAtEndLocked(const char* why)
{
    if (!gRunning || gState == XR_SESSION_STATE_STOPPING) return;
    const int64_t last = gLastEndNs.load();
    const double gap = last ? Sec(NowNs() - last) : 0.0;
    if (gap > 1.0)
    {
        gHourglasses++; if (gap > gHourglassMax) gHourglassMax = gap;
        L("HOURGLASS #%u: no frame for %.2f s, then %s (longest %.2f s)", gHourglasses, gap, why, gHourglassMax);
    }
}
/** Under gSm: the checks every session-level call makes first. */
XrResult SessionCheckLocked(XrSession s)
{
    if (gInstanceLost) return XR_ERROR_INSTANCE_LOST;
    if (!gSessionExists || s != (XrSession)&gSession) return XR_ERROR_HANDLE_INVALID;
    if (gLost) return XR_ERROR_SESSION_LOST;
    return XR_SUCCESS;
}

// The watchdog: a gap still open is reported while it is open, so a test sees an app that stopped
// submitting for good (a hang), not only gaps that ended.
HANDLE gWdThread = nullptr, gWdStop = nullptr;
DWORD WINAPI WatchdogMain(void*)
{
    while (WaitForSingleObject(gWdStop, 100) == WAIT_TIMEOUT)
    {
        std::lock_guard<std::mutex> lk(gSm);
        if (!gRunning || gLost || gState == XR_SESSION_STATE_STOPPING || gHourglassOngoing) continue;
        const int64_t last = gLastEndNs.load();
        if (!last) continue;
        const double gap = Sec(NowNs() - last);
        if (gap > 1.0)
        {
            gHourglassOngoing = true;
            L("HOURGLASS ongoing: no frame for %.2f s so far (t=%.3f)", gap, Sec(NowNs()));
        }
    }
    return 0;
}
void StartWatchdog()
{
    if (gWdThread) return;
    gWdStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gWdThread = gWdStop ? CreateThread(nullptr, 0, WatchdogMain, nullptr, 0, nullptr) : nullptr;
}
void StopWatchdog()
{
    if (!gWdThread) return;
    SetEvent(gWdStop);
    WaitForSingleObject(gWdThread, 2000);
    CloseHandle(gWdThread); CloseHandle(gWdStop);
    gWdThread = nullptr; gWdStop = nullptr;
}

// ---- The LOCAL space's origin (moved by a recentre) ----
// Poses are produced in the ROOM's frame (the input file's numbers) and expressed in LOCAL on the way
// out. Until a recentre LOCAL is the room; a recentre moves LOCAL's origin to the head's position and
// yaw (LOCAL stays gravity-aligned) from changeTime on, so a pose located for an earlier time still
// comes out in the previous LOCAL, as the spec has it.
struct LocalOrigin { bool moved = false; float yaw = 0.0f; float o[3] = { 0, 0, 0 }; };
std::mutex gOriginM;
LocalOrigin gOrPrev, gOrCur;
XrTime gOrChangeTime = 0;
LocalOrigin OriginAt(XrTime t)
{
    std::lock_guard<std::mutex> lk(gOriginM);
    return t >= gOrChangeTime ? gOrCur : gOrPrev;
}
/** Room pose -> the LOCAL of origin o: p' = Ry(-yaw) (p - o), q' = qy(-yaw) q. */
XrPosef ApplyOrigin(const LocalOrigin& o, XrPosef pose)
{
    if (!o.moved) return pose;
    const float c = cosf(-o.yaw), s = sinf(-o.yaw);
    const float dx = pose.position.x - o.o[0], dy = pose.position.y - o.o[1], dz = pose.position.z - o.o[2];
    pose.position = { c * dx + s * dz, dy, -s * dx + c * dz };
    pose.orientation = QMul(QYaw(-o.yaw), pose.orientation);
    return pose;
}
XrPosef ToLocalAt(const XrPosef& room, XrTime t) { return ApplyOrigin(OriginAt(t), room); }
} // namespace

#define MOCK_FN extern "C" XRAPI_ATTR XrResult XRAPI_CALL

void ReadMockInput();    // defined below, beside the fields it fills
void ReadMockMotion();
void ReadMockEvents();   // the failure-emulation command file

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
    ReadMockEvents();
    {
        std::lock_guard<std::mutex> lk(gSm);
        if (gInstanceAlive) L("xrCreateInstance: note, the previous instance was never destroyed");
        gInstanceAlive = true; gInstanceLost = false; gInstanceLossQueued = false;
        for (auto it = gEvq.begin(); it != gEvq.end();) it = it->kind == EvKind::InstanceLoss ? gEvq.erase(it) : it + 1;
    }
    L("xrCreateInstance"); *out = (XrInstance)&gInstance; return XR_SUCCESS;
}

void DestroySessionInternal();
MOCK_FN Mock_xrDestroyInstance(XrInstance)
{
    bool sessionAlive;
    { std::lock_guard<std::mutex> lk(gSm); sessionAlive = gSessionExists; }
    if (sessionAlive) { L("xrDestroyInstance: destroys the session it still owns"); DestroySessionInternal(); }
    {
        std::lock_guard<std::mutex> lk(gSm);
        gInstanceAlive = false; gInstanceLost = false; gInstanceLossQueued = false;
        gEvq.clear();
    }
    L("xrDestroyInstance"); return XR_SUCCESS;
}

MOCK_FN Mock_xrGetInstanceProperties(XrInstance, XrInstanceProperties* p)
{
    strcpy_s(p->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "DishonoredVR Mock Runtime");
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    return XR_SUCCESS;
}

MOCK_FN Mock_xrGetSystem(XrInstance, const XrSystemGetInfo* gi, XrSystemId* id)
{
    ReadMockEvents();   // also here: before a session exists nothing else reads the event file
    std::lock_guard<std::mutex> lk(gSm);
    if (gInstanceLost) return XR_ERROR_INSTANCE_LOST;
    if (gi && gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    if (NoHmdLocked())
    {
        static unsigned n = 0;
        if (n++ % 100 == 0) L("xrGetSystem: no headset (nohmd), XR_ERROR_FORM_FACTOR_UNAVAILABLE (#%u) t=%.3f", n, Sec(NowNs()));
        return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    }
    *id = (XrSystemId)1; return XR_SUCCESS;
}

MOCK_FN Mock_xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p)
{
    strcpy_s(p->systemName, XR_MAX_SYSTEM_NAME_SIZE, "Mock HMD");
    p->graphicsProperties.maxSwapchainImageWidth = 4096;
    p->graphicsProperties.maxSwapchainImageHeight = 4096;
    p->graphicsProperties.maxLayerCount = kMaxLayers;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrEnumerateViewConfigurations(XrInstance, XrSystemId, uint32_t cap, uint32_t* count, XrViewConfigurationType* types)
{
    *count = 1;
    if (cap == 0) return XR_SUCCESS;
    types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrEnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* modes)
{
    *count = 1;
    if (cap == 0) return XR_SUCCESS;
    modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
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
    ReadMockEvents();
    ID3D11Device* device = nullptr;
    const XrBaseInStructure* s = (const XrBaseInStructure*)ci->next;
    while (s)
    {
        if (s->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
        {
            device = ((const XrGraphicsBindingD3D11KHR*)s)->device;
            break;
        }
        s = s->next;
    }
    {
        std::lock_guard<std::mutex> lk(gSm);
        if (gInstanceLost) return XR_ERROR_INSTANCE_LOST;
        if (NoHmdLocked()) { L("xrCreateSession: no headset (nohmd), XR_ERROR_SYSTEM_INVALID"); return XR_ERROR_SYSTEM_INVALID; }
        if (gSessionExists) { L("xrCreateSession: a session already exists, XR_ERROR_LIMIT_REACHED"); return XR_ERROR_LIMIT_REACHED; }
        if (device)
        {
            gDevice = device;
            gEvq.erase(std::remove_if(gEvq.begin(), gEvq.end(), [](const MockEvent& e) { return e.kind != EvKind::InstanceLoss; }), gEvq.end());
            gSessionExists = true; gRunning = false; gExitPending = false; gLossQueued = false; gLost = false;
            gReadyAfterSec = -1.0; gReadyAtNs = -1; gWaited = gBegun = gEnded = 0; gHourglassOngoing = false;
            gState = XR_SESSION_STATE_UNKNOWN;
            QueueStateLocked(XR_SESSION_STATE_IDLE);   // a NEW session: READY at once
            QueueStateLocked(XR_SESSION_STATE_READY);
        }
    }
    L("xrCreateSession device=%p", (void*)device);
    if (!device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    StartWatchdog();
    *out = (XrSession)&gSession;
    return XR_SUCCESS;
}

/** The frames of a compose sequence (see the compositor): flushed when the session goes away. */
void FlushSequence();

void DestroyChildren();   // with the registries, below

void DestroySessionInternal()
{
    FlushSequence();
    StopWatchdog();
    DestroyChildren();
    std::lock_guard<std::mutex> lk(gSm);
    HourglassAtEndLocked("the session was destroyed");
    for (auto it = gEvq.begin(); it != gEvq.end();) it = it->kind != EvKind::InstanceLoss ? gEvq.erase(it) : it + 1;
    gSessionExists = false; gRunning = false; gExitPending = false; gLossQueued = false; gLost = false;
    gReadyAfterSec = -1.0; gReadyAtNs = -1; gWaited = gBegun = gEnded = 0;
    gState = XR_SESSION_STATE_UNKNOWN; gFocused = false;
    gFrameCv.notify_all();
}
MOCK_FN Mock_xrDestroySession(XrSession s)
{
    {
        std::lock_guard<std::mutex> lk(gSm);
        if (!gSessionExists || s != (XrSession)&gSession) return XR_ERROR_HANDLE_INVALID;
    }
    L("xrDestroySession");
    DestroySessionInternal();
    return XR_SUCCESS;
}

// ---- Spaces and swapchains: live-handle registries ----
std::mutex gObjM;
std::vector<MockSpace*> gSpaces;
std::vector<MockSwapchain*> gSwapchains;
bool SpaceLive(XrSpace h)
{
    std::lock_guard<std::mutex> lk(gObjM);
    for (MockSpace* s : gSpaces) if ((XrSpace)s == h) return true;
    return false;
}
bool SwapchainLive(XrSwapchain h)
{
    std::lock_guard<std::mutex> lk(gObjM);
    for (MockSwapchain* s : gSwapchains) if ((XrSwapchain)s == h) return true;
    return false;
}
/** A session's spaces and swapchains die with it (spec): any later use of them is HANDLE_INVALID. */
void DestroyChildren()
{
    std::vector<MockSpace*> sp;
    std::vector<MockSwapchain*> sw;
    { std::lock_guard<std::mutex> lk(gObjM); sp.swap(gSpaces); sw.swap(gSwapchains); }
    for (MockSpace* s : sp) delete s;
    for (MockSwapchain* s : sw) { for (auto* t : s->tex) if (t) t->Release(); delete s; }
    if (!sp.empty() || !sw.empty()) L("xrDestroySession: %zu spaces and %zu swapchains still alive are destroyed with it", sp.size(), sw.size());
}

MOCK_FN Mock_xrEnumerateReferenceSpaces(XrSession, uint32_t cap, uint32_t* count, XrReferenceSpaceType* spaces)
{
    const XrReferenceSpaceType t[] = { XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE };
    *count = 3;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 3; i++) spaces[i] = t[i];
    return XR_SUCCESS;
}

const char* RefName(XrReferenceSpaceType t)
{
    return t == XR_REFERENCE_SPACE_TYPE_VIEW ? "VIEW" : t == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL" : t == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "?";
}

MOCK_FN Mock_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* ci, XrSpace* out)
{
    {
        std::lock_guard<std::mutex> lk(gSm);
        const XrResult r = SessionCheckLocked(session);
        if (r != XR_SUCCESS) return r;
    }
    if (!ci) return XR_ERROR_VALIDATION_FAILURE;
    if (ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW && ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE)
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    MockSpace* s = new MockSpace();
    s->action = false;
    s->ref = ci->referenceSpaceType;
    s->offset = ci->poseInReferenceSpace;
    s->name = RefName(s->ref);
    { std::lock_guard<std::mutex> lk(gObjM); gSpaces.push_back(s); }
    L("xrCreateReferenceSpace type=%s", s->name.c_str());
    *out = (XrSpace)s; return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroySpace(XrSpace h)
{
    std::lock_guard<std::mutex> lk(gObjM);
    for (size_t i = 0; i < gSpaces.size(); i++)
        if ((XrSpace)gSpaces[i] == h) { delete gSpaces[i]; gSpaces.erase(gSpaces.begin() + (ptrdiff_t)i); return XR_SUCCESS; }
    return XR_ERROR_HANDLE_INVALID;
}

MOCK_FN Mock_xrEnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* count, int64_t* formats)
{
    // Exactly these two, in this order: the mod chooses its copy path from what is offered.
    int64_t f[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    formats[0] = f[0]; formats[1] = f[1];
    return XR_SUCCESS;
}

MOCK_FN Mock_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* out)
{
    {
        std::lock_guard<std::mutex> lk(gSm);
        const XrResult r = SessionCheckLocked(session);
        if (r != XR_SUCCESS) return r;
    }
    if (!ci || ci->width == 0 || ci->height == 0) return XR_ERROR_VALIDATION_FAILURE;
    if (ci->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && ci->format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        L("xrCreateSwapchain fmt=%lld: XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED", (long long)ci->format);
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }
    if (ci->faceCount > 1 || ci->sampleCount > 1) return XR_ERROR_FEATURE_UNSUPPORTED;
    MockSwapchain* sc = new MockSwapchain();
    sc->w = ci->width; sc->h = ci->height; sc->fmt = (DXGI_FORMAT)ci->format;
    sc->arraySize = ci->arraySize ? ci->arraySize : 1;
    for (uint32_t i = 0; i < kSwapImages; i++)
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = sc->w; d.Height = sc->h; d.MipLevels = ci->mipCount ? ci->mipCount : 1; d.ArraySize = sc->arraySize;
        d.Format = sc->fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if ((ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) && !IsSrgb(sc->fmt)) d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        ID3D11Texture2D* t = nullptr;
        HRESULT hr = gDevice ? gDevice->CreateTexture2D(&d, nullptr, &t) : E_FAIL;
        if (FAILED(hr))
        {
            L("CreateTexture2D failed hr=0x%08x", (unsigned)hr);
            for (auto* x : sc->tex) if (x) x->Release();
            delete sc;
            return XR_ERROR_RUNTIME_FAILURE;
        }
        sc->tex.push_back(t);
    }
    { std::lock_guard<std::mutex> lk(gObjM); gSwapchains.push_back(sc); }
    L("xrCreateSwapchain %ux%u fmt=%d images=%zu array=%u usage=0x%llx", sc->w, sc->h, (int)sc->fmt, sc->tex.size(), sc->arraySize,
      (unsigned long long)ci->usageFlags);
    *out = (XrSwapchain)sc;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroySwapchain(XrSwapchain h)
{
    MockSwapchain* sc = nullptr;
    {
        std::lock_guard<std::mutex> lk(gObjM);
        for (size_t i = 0; i < gSwapchains.size(); i++)
            if ((XrSwapchain)gSwapchains[i] == h) { sc = gSwapchains[i]; gSwapchains.erase(gSwapchains.begin() + (ptrdiff_t)i); break; }
    }
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    for (auto* t : sc->tex) if (t) t->Release();
    delete sc; return XR_SUCCESS;
}

MOCK_FN Mock_xrEnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* imgs)
{
    if (!SwapchainLive(h)) return XR_ERROR_HANDLE_INVALID;
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
    ReadMockEvents();
    std::lock_guard<std::mutex> lk(gSm);
    if (gInstanceLost) return XR_ERROR_INSTANCE_LOST;
    const int64_t now = NowNs();
    if (gReadyAtNs >= 0 && now >= gReadyAtNs)
    {
        gReadyAtNs = -1;
        if (gSessionExists && !gRunning && !gLost && !gLossQueued && gState == XR_SESSION_STATE_IDLE)
        {
            QueueStateLocked(XR_SESSION_STATE_READY);
            L("ev=mock session offered back: READY t=%.3f", Sec(now));
        }
    }
    if (gEvq.empty()) return XR_EVENT_UNAVAILABLE;
    const MockEvent e = gEvq.front(); gEvq.pop_front();
    if (e.kind == EvKind::State)
    {
        auto* d = (XrEventDataSessionStateChanged*)ev;
        memset(d, 0, sizeof *d);
        d->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
        d->session = (XrSession)&gSession;
        d->state = e.state;
        d->time = e.time;
        if (e.state == XR_SESSION_STATE_LOSS_PENDING)
        {   // delivered: the session is gone now
            gLost = true; gRunning = false; gFocused = false;
            gFrameCv.notify_all();
        }
        L("ev=mock poll state=%s t=%.3f", StateName(e.state), Sec(now));
    }
    else if (e.kind == EvKind::RefSpace)
    {
        auto* d = (XrEventDataReferenceSpaceChangePending*)ev;
        memset(d, 0, sizeof *d);
        d->type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
        d->session = (XrSession)&gSession;
        d->referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        d->changeTime = e.time;
        d->poseValid = XR_TRUE;
        d->poseInPreviousSpace = e.pose;
        L("ev=mock poll event=REFERENCE_SPACE_CHANGE_PENDING changeTime=%lld t=%.3f", (long long)e.time, Sec(now));
    }
    else
    {
        auto* d = (XrEventDataInstanceLossPending*)ev;
        memset(d, 0, sizeof *d);
        d->type = XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;
        d->lossTime = e.time;
        gInstanceLost = true; gRunning = false; gFocused = false;
        gFrameCv.notify_all();
        L("ev=mock poll event=INSTANCE_LOSS_PENDING t=%.3f", Sec(now));
    }
    return XR_SUCCESS;
}

MOCK_FN Mock_xrBeginSession(XrSession session, const XrSessionBeginInfo* bi)
{
    std::lock_guard<std::mutex> lk(gSm);
    XrResult r = SessionCheckLocked(session);
    if (r == XR_SUCCESS && gLossQueued) r = XR_ERROR_SESSION_LOST;
    if (r == XR_SUCCESS && gRunning) r = XR_ERROR_SESSION_RUNNING;
    if (r == XR_SUCCESS && gState != XR_SESSION_STATE_READY) r = XR_ERROR_SESSION_NOT_READY;
    if (r == XR_SUCCESS && bi && bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) r = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (r != XR_SUCCESS) { L("xrBeginSession refused: %d (state %s)", (int)r, StateName(gState)); return r; }
    L("xrBeginSession");
    gRunning = true; gExitPending = false; gReadyAfterSec = -1.0; gReadyAtNs = -1;
    gWaited = gBegun = gEnded = 0; gHourglassOngoing = false;
    gLastEndNs = NowNs();   // the hourglass clock starts now: the headset waits for the first frame
    QueueStateLocked(XR_SESSION_STATE_SYNCHRONIZED);
    QueueStateLocked(XR_SESSION_STATE_VISIBLE);
    QueueStateLocked(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}
MOCK_FN Mock_xrEndSession(XrSession session)
{
    std::lock_guard<std::mutex> lk(gSm);
    XrResult r = SessionCheckLocked(session);
    if (r == XR_SUCCESS && !gRunning) r = XR_ERROR_SESSION_NOT_RUNNING;
    if (r == XR_SUCCESS && gState != XR_SESSION_STATE_STOPPING) r = XR_ERROR_SESSION_NOT_STOPPING;
    if (r != XR_SUCCESS) { L("xrEndSession refused: %d (state %s)", (int)r, StateName(gState)); return r; }
    gRunning = false;
    gFrameCv.notify_all();
    QueueStateLocked(XR_SESSION_STATE_IDLE);
    if (gExitPending)
    {
        gExitPending = false;
        QueueStateLocked(XR_SESSION_STATE_EXITING);
        L("xrEndSession (exit requested: IDLE, EXITING)");
    }
    else if (gReadyAfterSec >= 0.0)
    {
        gReadyAtNs = NowNs() + (int64_t)(gReadyAfterSec * 1e9);
        L("xrEndSession (the stopped session will be offered back in %.2f s)", gReadyAfterSec);
    }
    else
    {
        gReadyAtNs = -1;
        L("xrEndSession (the stopped session is NOT offered back: only a new session gets READY)");
    }
    gReadyAfterSec = -1.0;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrRequestExitSession(XrSession session)
{
    std::lock_guard<std::mutex> lk(gSm);
    XrResult r = SessionCheckLocked(session);
    if (r == XR_SUCCESS && !gRunning) r = XR_ERROR_SESSION_NOT_RUNNING;
    if (r != XR_SUCCESS) { L("xrRequestExitSession refused: %d", (int)r); return r; }
    gExitPending = true;
    QueueStopLocked();
    L("xrRequestExitSession");
    return XR_SUCCESS;
}

/**
 * A compositor releases the app once per display period, at its vsync. Without it the game runs
 * as fast as it can, which is a different pipeline from the one a headset sees: with alternating
 * eyes, which frame carries which eye depends on it.
 */
void PaceFrame()
{
    ReadHmdConfig();
    if (gPaceHz <= 0.0f) return;
    static int64_t next = 0;
    const int64_t period = (int64_t)(1e9 / gPaceHz);
    const int64_t now = NowNs();
    if (next == 0 || now - next > period * 4) next = now;   // first frame, or a long stall: resync
    while (NowNs() < next)
    {
        const int64_t left = next - NowNs();
        if (left > 2000000) Sleep((DWORD)((left - 1000000) / 1000000));
        else YieldProcessor();
    }
    next += period;
}

MOCK_FN Mock_xrWaitFrame(XrSession session, const XrFrameWaitInfo*, XrFrameState* fs)
{
    // Also read here, not only in xrSyncActions: a mod that drives the head but has not attached
    // an action set yet never syncs, and would be stuck with the procedural sweep.
    ReadMockEvents();
    ReadMockInput();
    ReadMockMotion();
    ReadHmdConfig();
    const XrDuration period = gPaceHz > 0.0f ? (XrDuration)(1e9 / gPaceHz) : 11111111;
    double stall = 0.0;
    {
        std::unique_lock<std::mutex> lk(gSm);
        const XrResult r = SessionCheckLocked(session);
        if (r != XR_SUCCESS) return r;
        if (gLossQueued)
        {
            fs->predictedDisplayTime = NowNs() + 2 * period; fs->predictedDisplayPeriod = period; fs->shouldRender = XR_FALSE;
            return XR_SESSION_LOSS_PENDING;
        }
        if (!gRunning) return XR_ERROR_SESSION_NOT_RUNNING;
        if (gWaited > gBegun)
        {   // the previous frame was waited and never begun: the spec blocks until it is
            if (!gFrameCv.wait_for(lk, std::chrono::seconds(1), [] { return gBegun >= gWaited || !gRunning || gLost || gInstanceLost; }))
            {
                L("xrWaitFrame CALL ORDER: the previous frame was never begun (waited 1 s, dropping it)");
                gWaited = gBegun;
            }
            if (gInstanceLost) return XR_ERROR_INSTANCE_LOST;
            if (gLost) return XR_ERROR_SESSION_LOST;
            if (!gRunning) return XR_ERROR_SESSION_NOT_RUNNING;
        }
        stall = gStallArmedSec; gStallArmedSec = 0.0;
    }
    if (stall > 0.0)
    {   // the compositor does not release the app (lock not held: other threads keep calling in)
        const int64_t t0 = NowNs();
        L("ev=mock stall begin seconds=%.2f t=%.3f (xrWaitFrame blocks)", stall, Sec(t0));
        SleepUntilNs(t0 + (int64_t)(stall * 1e9));
        L("ev=mock stall end waited=%.2f t=%.3f", Sec(NowNs() - t0), Sec(NowNs()));
    }
    PaceFrame();
    ++gFrameTime;
    // Real time, as a runtime gives it: released at a vsync, the frame begun now is displayed two
    // periods later (one to render, one to compose and scan out). Poses are a function of time
    // (EyePoseAt), so an app that predicts for the wrong time is measurably wrong here too.
    XrTime t = NowNs() + 2 * period;
    bool render = true;
    {
        std::lock_guard<std::mutex> lk(gSm);
        if (t <= gLastPredicted.load()) t = gLastPredicted.load() + 1;   // strictly increasing
        gLastPredicted = t;
        ++gWaited;
        render = gState == XR_SESSION_STATE_VISIBLE || gState == XR_SESSION_STATE_FOCUSED;
    }
    fs->predictedDisplayTime = t;
    fs->predictedDisplayPeriod = period;
    fs->shouldRender = render ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrBeginFrame(XrSession session, const XrFrameBeginInfo*)
{
    std::lock_guard<std::mutex> lk(gSm);
    const XrResult r = SessionCheckLocked(session);
    if (r != XR_SUCCESS) return r;
    if (gLossQueued) return XR_SESSION_LOSS_PENDING;
    if (!gRunning) return XR_ERROR_SESSION_NOT_RUNNING;
    if (gWaited <= gBegun)
    {
        static unsigned n = 0;
        if (n++ < 20) L("xrBeginFrame CALL ORDER: no waited frame to begin (XR_ERROR_CALL_ORDER_INVALID)");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    XrResult res = XR_SUCCESS;
    if (gBegun > gEnded) { gEnded = gBegun; res = XR_FRAME_DISCARDED; }   // the previous frame was never ended
    ++gBegun;
    gFrameCv.notify_all();
    return res;
}

// ---- The compositor, emulated: what each EYE would see ----
//
// Ported from the sister project's mock and extended. A projection layer is a geometric promise:
// each view says which pose its image was rendered from and which FOV it covers, and the
// compositor resamples it into the eye's own FOV with a rotation-only reprojection from that pose
// to where the eye is NOW -- what the Quest's timewarp does. Everything that decides whether the
// headset image is right (bands, stretch, double vision, swim, which eye got which picture) lives
// in that mapping, and none of it is visible in the desktop capture.
//
// Quad layers are drawn over it in submission order: a ray from the eye's POSITION (a quad is a
// surface in the room, so where the eye is matters, unlike the projection's rotation-only resample)
// meets the quad's plane, at its space's pose (located for the frame's display time) times the
// layer's pose, size in metres, front face only (+Z towards the viewer, as the spec has it); the
// layer's subImage rect is sampled bilinearly; eyeVisibility picks the eyes. Blending follows the
// flags: without XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT the quad is opaque; with it,
// premultiplied (src + dst (1 - a)) unless XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT (src a +
// dst (1 - a)). An sRGB swapchain is decoded and blended in LINEAR light, as a real compositor does
// (blending the encoded bytes makes every translucent layer lighter than on the headset); a UNORM
// one is blended as stored, the same convention the projection's bytes are shown with. A frame with
// no projection layer composes over BLACK (what the headset shows), not the "no image" blue.
//
// Writing SVR_MockCompose.txt makes xrEndFrame compose. Its words:
//   <tag> [world] [seq <n>] [full]
//   world   the eye does NOT turn (identity orientation, same position): with truthful layer poses
//           the static world comes out the same whatever the head does -- the swim test.
//   seq n   compose this frame and the n-1 after it, as <tag>_f00 .. (alternating eyes, timing).
//   full    the eye's full panel resolution (default: half per axis).
//   fovlock compose as Meta's PC runtime is reported to (Khronos forum 110078, OpenXR-Layer-crop-fov
//           #36): the declared FOV is ignored and the sub-image rect is mapped onto the EYE's own FOV.
//   poselock the declared pose is ignored too: the image is taken as drawn from the eye's own pose.
//           Together they are the most pessimistic reading of that runtime; a submission that comes
//           out the same with and without them does not depend on which reading is true.
//           (fovlock/poselock apply to the projection layer only.)
// Output, in the working directory: SVR_seen_<tag>_eye<i>.bmp (linear in tan, like a real eye
// buffer; pixels no layer reaches are painted dark blue, which a real headset shows as black --
// blue tells "no image" from a black image) and SVR_seen_<tag>.txt with the numbers (one "quad"
// line per quad and eye: its pose in LOCAL, size, rect, flags and the pixels it covered).
XrPosef gLastViewPose[2] = { { { 0, 0, 0, 1 }, { 0, 0, 0 } }, { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
/** One eye's pose at a given time (defined with the input, below), in LOCAL: what xrLocateViews
 *  returns for that time, and where the compositor takes the eye to be when a frame is displayed. */
XrPosef EyePoseAt(int eye, XrTime t);
/** Where a space is at time t, in LOCAL (defined with the input). False for a dead handle. */
bool LocateInLocal(XrSpace space, XrTime t, XrPosef& out);
uint64_t gEndFrames = 0;

/**
 * Copies one slice of a texture into a new staging texture -- a GPU copy, queued and cheap, so a
 * sequence can capture consecutive frames without stalling the app (reading back and resampling
 * every frame took ~60 ms and spread an 8-frame "sequence" over half a second of head motion).
 * @return The staging texture (the caller releases it), or null when the format is not readable.
 */
ID3D11Texture2D* CopyToStaging(ID3D11Texture2D* t, UINT slice, bool& bgra)
{
    if (!t || !gDevice) return nullptr;
    D3D11_TEXTURE2D_DESC d = {}; t->GetDesc(&d);
    const DXGI_FORMAT f = d.Format;
    bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!(bgra || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS)) return nullptr;
    if (slice >= d.ArraySize) return nullptr;
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
    s.ArraySize = 1; s.MipLevels = 1;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(gDevice->CreateTexture2D(&s, nullptr, &st))) return nullptr;
    ID3D11DeviceContext* ctx = nullptr; gDevice->GetImmediateContext(&ctx);
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, D3D11CalcSubresource(0, slice, d.MipLevels), nullptr);
    ctx->Release();
    return st;
}

/** Reads a staging texture back (this is the part that waits for the GPU). */
bool ReadStaging(ID3D11Texture2D* st, std::vector<uint8_t>& px, UINT& w, UINT& h)
{
    if (!st || !gDevice) return false;
    D3D11_TEXTURE2D_DESC d = {}; st->GetDesc(&d);
    ID3D11DeviceContext* ctx = nullptr; gDevice->GetImmediateContext(&ctx);
    D3D11_MAPPED_SUBRESOURCE m = {};
    const bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok)
    {
        w = d.Width; h = d.Height; px.resize((size_t)w * h * 4);
        for (UINT y = 0; y < h; y++) memcpy(&px[(size_t)y * w * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        ctx->Unmap(st, 0);
    }
    ctx->Release();
    return ok;
}

bool WriteBmp(const char* path, const std::vector<uint8_t>& rgb, int w, int h)
{
    FILE* f = nullptr; if (fopen_s(&f, path, "wb") || !f) return false;
    const int row = (w * 3 + 3) & ~3, size = 54 + row * h;
    uint8_t hd[54] = { 'B', 'M' };
    auto put32 = [&](int o, int v) { hd[o] = (uint8_t)v; hd[o + 1] = (uint8_t)(v >> 8); hd[o + 2] = (uint8_t)(v >> 16); hd[o + 3] = (uint8_t)(v >> 24); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h); hd[26] = 1; hd[28] = 24; put32(34, row * h);
    fwrite(hd, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = h - 1; y >= 0; y--)
    {
        for (int x = 0; x < w; x++) { const uint8_t* p = &rgb[((size_t)y * w + x) * 3]; line[x * 3] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0]; }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f); return true;
}

/** sRGB <-> linear, for blending quads in linear light. */
float SrgbToLin(uint8_t v)
{
    static float lut[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; i++) { const float c = (float)i / 255.0f; lut[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
        init = true;
    }
    return lut[v];
}
uint8_t LinToSrgb(float v)
{
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    const float e = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(e * 255.0f + 0.5f);
}

/** One frame as the compositor received it, captured to be composed now or after a sequence. */
struct Captured
{
    std::string tag;
    bool worldFixed = false, full = false, fovLock = false, poseLock = false;
    bool hasProjection = false;
    uint32_t layerCount = 0;
    std::string header;                     // the frame's lines of SVR_seen_<tag>.txt
    struct Eye
    {
        std::string lines;                  // its "eye" and "view" lines
        XrPosef atDisplay{};                // where the eye is when the frame is displayed
        bool haveView = false;
        XrCompositionLayerProjectionView view{};
        ID3D11Texture2D* staging = nullptr; // the image the view showed, copied at capture
        bool bgra = false;
    } eyes[2];
    struct Quad
    {
        uint32_t layer = 0;
        XrEyeVisibility vis = XR_EYE_VISIBILITY_BOTH;
        XrCompositionLayerFlags flags = 0;
        bool located = false;
        XrPosef pose{};                     // in LOCAL, at the frame's display time
        XrExtent2Df size{};
        XrRect2Di rect{};
        std::string space;
        uint32_t sw = 0, sh = 0;
        int32_t image = -1;
        bool srgb = false, bgra = false;
        ID3D11Texture2D* staging = nullptr;
    };
    std::vector<Quad> quads;
};

Captured Capture(const XrFrameEndInfo* fei, const char* tag, bool worldFixed, bool full, bool fovLock, bool poseLock)
{
    ReadHmdConfig();
    Captured c;
    c.tag = tag; c.worldFixed = worldFixed; c.full = full; c.fovLock = fovLock; c.poseLock = poseLock;
    c.layerCount = fei->layerCount;
    char b[640];
    snprintf(b, sizeof b, "frame %llu layers %u display_time %lld\n", (unsigned long long)gEndFrames, fei->layerCount, (long long)fei->displayTime);
    c.header += b;
    snprintf(b, sizeof b, "compositor fovlock %d poselock %d\n", fovLock ? 1 : 0, poseLock ? 1 : 0);
    c.header += b;
    snprintf(b, sizeof b, "panel %u %u ipd %.4f\n", gEyeW, gEyeH, gIpd);
    c.header += b;
    const XrCompositionLayerProjection* proj = nullptr;
    for (uint32_t li = 0; li < fei->layerCount; li++)
    {
        snprintf(b, sizeof b, "layer %u type %d\n", li, (int)fei->layers[li]->type); c.header += b;
        if (!proj && fei->layers[li]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) proj = (const XrCompositionLayerProjection*)fei->layers[li];
    }
    c.hasProjection = proj != nullptr;
    const float r2d = 57.2957795f;
    for (int eye = 0; eye < 2; eye++)
    {
        Captured::Eye& e = c.eyes[eye];
        const XrFovf ef = EyeFov(eye);
        // Where the eye is when this frame is displayed: the time the app submitted it for.
        e.atDisplay = EyePoseAt(eye, fei->displayTime);
        XrPosef ep = e.atDisplay;
        if (worldFixed) ep.orientation = { 0, 0, 0, 1 };
        snprintf(b, sizeof b, "eye %d fov %.4f %.4f %.4f %.4f pose q %.6f %.6f %.6f %.6f p %.5f %.5f %.5f\n", eye,
                 ef.angleLeft * r2d, ef.angleRight * r2d, ef.angleUp * r2d, ef.angleDown * r2d,
                 ep.orientation.x, ep.orientation.y, ep.orientation.z, ep.orientation.w, ep.position.x, ep.position.y, ep.position.z);
        e.lines += b;
        if (proj && (uint32_t)eye < proj->viewCount)
        {
            const XrCompositionLayerProjectionView& v = proj->views[eye];
            MockSwapchain* sc = (MockSwapchain*)v.subImage.swapchain;
            snprintf(b, sizeof b, "view %d fov %.4f %.4f %.4f %.4f pose q %.6f %.6f %.6f %.6f p %.5f %.5f %.5f rect %d %d %d %d slice %u swap %p %ux%u image %d releases %llu\n", eye,
                     v.fov.angleLeft * r2d, v.fov.angleRight * r2d, v.fov.angleUp * r2d, v.fov.angleDown * r2d,
                     v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w,
                     v.pose.position.x, v.pose.position.y, v.pose.position.z,
                     v.subImage.imageRect.offset.x, v.subImage.imageRect.offset.y, v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height,
                     v.subImage.imageArrayIndex, (void*)sc, sc ? sc->w : 0, sc ? sc->h : 0, sc ? sc->released : -1,
                     sc ? (unsigned long long)sc->releases : 0ULL);
            e.lines += b;
            e.haveView = true;
            e.view = v;
            ID3D11Texture2D* tex = (sc && sc->released >= 0 && (size_t)sc->released < sc->tex.size()) ? sc->tex[sc->released] : nullptr;
            e.staging = CopyToStaging(tex, v.subImage.imageArrayIndex, e.bgra);
        }
    }
    // The quads: located NOW, for the frame's display time (a hand-held or head-locked quad is where
    // its space is when the frame is shown), and their images copied, as the projection's are.
    for (uint32_t li = 0; li < fei->layerCount; li++)
    {
        if (fei->layers[li]->type != XR_TYPE_COMPOSITION_LAYER_QUAD) continue;
        const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)fei->layers[li];
        Captured::Quad cq;
        cq.layer = li; cq.vis = q->eyeVisibility; cq.flags = q->layerFlags; cq.size = q->size; cq.rect = q->subImage.imageRect;
        XrPosef sp;
        cq.located = LocateInLocal(q->space, fei->displayTime, sp);
        if (cq.located) cq.pose = PoseMul(sp, q->pose);
        {
            const MockSpace* ms = (const MockSpace*)q->space;
            cq.space = ms ? ms->name : "?";
        }
        MockSwapchain* sc = (MockSwapchain*)q->subImage.swapchain;
        cq.sw = sc->w; cq.sh = sc->h; cq.image = sc->released; cq.srgb = IsSrgb(sc->fmt);
        ID3D11Texture2D* tex = (sc->released >= 0 && (size_t)sc->released < sc->tex.size()) ? sc->tex[sc->released] : nullptr;
        cq.staging = CopyToStaging(tex, q->subImage.imageArrayIndex, cq.bgra);
        c.quads.push_back(cq);
    }
    return c;
}

/** Composes a captured frame into each eye and writes SVR_seen_<tag>_eye<i>.bmp and .txt; frees it. */
void Render(Captured& c)
{
    std::string info = c.header;
    char b[640];
    const bool full = c.full, fovLock = c.fovLock, poseLock = c.poseLock;
    // Each quad's image, read back once for both eyes.
    struct QuadPx { std::vector<uint8_t> px; UINT w = 0, h = 0; bool ok = false; };
    std::vector<QuadPx> qpx(c.quads.size());
    for (size_t i = 0; i < c.quads.size(); i++)
    {
        qpx[i].ok = ReadStaging(c.quads[i].staging, qpx[i].px, qpx[i].w, qpx[i].h);
        if (c.quads[i].staging) { c.quads[i].staging->Release(); c.quads[i].staging = nullptr; }
    }
    unsigned drawn[2] = { 0, 0 };
    for (int eye = 0; eye < 2; eye++)
    {
        Captured::Eye& e = c.eyes[eye];
        info += e.lines;
        const XrFovf ef = EyeFov(eye);
        const XrPosef atDisplay = e.atDisplay;
        XrPosef ep = atDisplay;
        if (c.worldFixed) ep.orientation = { 0, 0, 0, 1 };
        const float tL = tanf(ef.angleLeft), tR = tanf(ef.angleRight), tU = tanf(ef.angleUp), tD = tanf(ef.angleDown);
        const int W = (int)(full ? gEyeW : gEyeW / 2), H = (int)(full ? gEyeH : gEyeH / 2);
        std::vector<uint8_t> out((size_t)W * H * 3, 0);
        // No projection layer at all: the headset shows black behind the quads. Otherwise the pixels
        // the projection does not reach are the "no image" blue.
        if (c.hasProjection)
            for (size_t i = 0; i < out.size(); i += 3) { out[i] = 0; out[i + 1] = 0; out[i + 2] = 70; }
        unsigned long long covered = 0;
        if (e.haveView)
        {
            const XrCompositionLayerProjectionView& v = e.view;
            // What the compositor TAKES the image to be: the declaration, or (fovlock/poselock) the
            // eye's own FOV and pose whatever was declared. The log line keeps the declaration.
            const XrFovf lf = fovLock ? ef : v.fov;
            const XrQuaternionf layerQ = poseLock ? atDisplay.orientation : v.pose.orientation;
            std::vector<uint8_t> px; UINT tw = 0, th = 0; const bool bgra = e.bgra;
            if (ReadStaging(e.staging, px, tw, th))
            {
                const float lL = tanf(lf.angleLeft), lR = tanf(lf.angleRight), lU = tanf(lf.angleUp), lD = tanf(lf.angleDown);
                const float rx = (float)v.subImage.imageRect.offset.x, ry = (float)v.subImage.imageRect.offset.y;
                const float rw = (float)v.subImage.imageRect.extent.width, rh = (float)v.subImage.imageRect.extent.height;
                // Bilinear, inside the image rect only: a compositor clamps to the sub-image, and
                // nearest sampling would quantise every disparity measured on these to whole pixels.
                auto texel = [&](int x, int y, int c) -> float {
                    const int cx = x < (int)rx ? (int)rx : (x > (int)(rx + rw) - 1 ? (int)(rx + rw) - 1 : x);
                    const int cy = y < (int)ry ? (int)ry : (y > (int)(ry + rh) - 1 ? (int)(ry + rh) - 1 : y);
                    const uint8_t* p = &px[((size_t)cy * tw + cx) * 4];
                    return (float)(bgra ? p[2 - c] : p[c]);
                };
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                    {
                        const float d[3] = { tL + (x + 0.5f) / W * (tR - tL), tU - (y + 0.5f) / H * (tU - tD), -1.f };
                        float wd[3], ld[3]; QRotV(ep.orientation, d, wd); QRotVInv(layerQ, wd, ld);
                        if (ld[2] >= -1e-6f) continue;
                        const float lx = ld[0] / -ld[2], ly = ld[1] / -ld[2];
                        const float s = (lx - lL) / (lR - lL), t = (lU - ly) / (lU - lD);
                        if (s < 0 || s >= 1 || t < 0 || t >= 1) continue;
                        const float fx = rx + s * rw - 0.5f, fy = ry + t * rh - 0.5f;
                        const int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                        const float ax = fx - x0, ay = fy - y0;
                        if (x0 + 1 < 0 || y0 + 1 < 0 || x0 >= (int)tw || y0 >= (int)th) continue;
                        uint8_t* o = &out[((size_t)y * W + x) * 3];
                        for (int c = 0; c < 3; c++)
                        {
                            const float top = texel(x0, y0, c) * (1 - ax) + texel(x0 + 1, y0, c) * ax;
                            const float bot = texel(x0, y0 + 1, c) * (1 - ax) + texel(x0 + 1, y0 + 1, c) * ax;
                            o[c] = (uint8_t)(top * (1 - ay) + bot * ay + 0.5f);
                        }
                        covered++;
                    }
            }
            else info += "  (texture not readable)\n";
        }
        if (e.staging) { e.staging->Release(); e.staging = nullptr; }
        snprintf(b, sizeof b, "coverage %d %.5f\n", eye, (double)covered / ((double)W * H)); info += b;

        // Quads over it, in submission order.
        const XrEyeVisibility mine = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
        for (size_t qi = 0; qi < c.quads.size(); qi++)
        {
            const Captured::Quad& q = c.quads[qi];
            const QuadPx& qp = qpx[qi];
            const XrVector3f& P = q.pose.position;
            const XrQuaternionf& Q = q.pose.orientation;
            const char* why = nullptr;
            if (q.vis != XR_EYE_VISIBILITY_BOTH && q.vis != mine) why = "hidden from this eye (eyeVisibility)";
            else if (!q.located) why = "space not located";
            else if (!qp.ok) why = "texture not readable";
            unsigned long long hit = 0;
            float lo[3] = { 0, 0, 0 };
            if (!why)
            {
                const float r[3] = { ep.position.x - P.x, ep.position.y - P.y, ep.position.z - P.z };
                QRotVInv(Q, r, lo);
                if (lo[2] <= 0.0f) why = "seen from behind (back face is not drawn)";
            }
            if (!why)
            {
                const bool alpha = (q.flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
                const bool unpre = (q.flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0;
                const bool srgb = q.srgb, bgra = q.bgra;
                const float rx = (float)q.rect.offset.x, ry = (float)q.rect.offset.y;
                const float rw = (float)q.rect.extent.width, rh = (float)q.rect.extent.height;
                const int rx0 = q.rect.offset.x, ry0 = q.rect.offset.y;
                const int rx1 = q.rect.offset.x + q.rect.extent.width - 1, ry1 = q.rect.offset.y + q.rect.extent.height - 1;
                const UINT tw = qp.w;
                // A texel in the working space: linear light for sRGB, the stored value otherwise.
                auto texel = [&](int x, int y, int ch) -> float {
                    const int cx = x < rx0 ? rx0 : (x > rx1 ? rx1 : x);
                    const int cy = y < ry0 ? ry0 : (y > ry1 ? ry1 : y);
                    const uint8_t* p = &qp.px[((size_t)cy * tw + cx) * 4];
                    const uint8_t v = ch == 3 ? p[3] : (bgra ? p[2 - ch] : p[ch]);
                    return (srgb && ch < 3) ? SrgbToLin(v) : (float)v / 255.0f;
                };
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                    {
                        const float d[3] = { tL + (x + 0.5f) / W * (tR - tL), tU - (y + 0.5f) / H * (tU - tD), -1.f };
                        float wd[3], ld[3]; QRotV(ep.orientation, d, wd); QRotVInv(Q, wd, ld);
                        if (ld[2] >= -1e-9f) continue;          // the ray never comes down to the plane
                        const float t = -lo[2] / ld[2];
                        const float hx = lo[0] + ld[0] * t, hy = lo[1] + ld[1] * t;
                        const float u = hx / q.size.width + 0.5f, v = 0.5f - hy / q.size.height;
                        if (!(u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f)) continue;
                        const float fx = rx + u * rw - 0.5f, fy = ry + v * rh - 0.5f;
                        const int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                        const float ax = fx - x0, ay = fy - y0;
                        float s[4];
                        for (int ch = 0; ch < 4; ch++)
                        {
                            const float top = texel(x0, y0, ch) * (1 - ax) + texel(x0 + 1, y0, ch) * ax;
                            const float bot = texel(x0, y0 + 1, ch) * (1 - ax) + texel(x0 + 1, y0 + 1, ch) * ax;
                            s[ch] = top * (1 - ay) + bot * ay;
                        }
                        const float a = alpha ? s[3] : 1.0f;
                        uint8_t* o = &out[((size_t)y * W + x) * 3];
                        for (int ch = 0; ch < 3; ch++)
                        {
                            const float dst = srgb ? SrgbToLin(o[ch]) : (float)o[ch] / 255.0f;
                            const float src = (alpha && unpre) ? s[ch] * a : s[ch];
                            const float col = alpha ? src + dst * (1.0f - a) : src;
                            if (srgb) o[ch] = LinToSrgb(col);
                            else { const float cc = col < 0.0f ? 0.0f : (col > 1.0f ? 1.0f : col); o[ch] = (uint8_t)(cc * 255.0f + 0.5f); }
                        }
                        if (a > 0.02f) hit++;
                    }
                if (hit) drawn[eye]++;
            }
            snprintf(b, sizeof b, "quad %u eye %d space %s pos %.4f %.4f %.4f q %.5f %.5f %.5f %.5f size %.4f %.4f rect %d %d %d %d swap %ux%u %s image %d flags %llu vis %d covered %llu%s%s\n",
                     q.layer, eye, q.space.c_str(), P.x, P.y, P.z, Q.x, Q.y, Q.z, Q.w, q.size.width, q.size.height,
                     q.rect.offset.x, q.rect.offset.y, q.rect.extent.width, q.rect.extent.height, q.sw, q.sh, q.srgb ? "srgb" : "unorm",
                     q.image, (unsigned long long)q.flags, (int)q.vis, hit, why ? " -- " : "", why ? why : "");
            info += b;
        }
        char path[260]; snprintf(path, sizeof path, "SVR_seen_%s_eye%d.bmp", c.tag.c_str(), eye);
        snprintf(b, sizeof b, "out %d %dx%d %s\n", eye, W, H, WriteBmp(path, out, W, H) ? path : "WRITE FAILED"); info += b;
    }
    snprintf(b, sizeof b, "quads %zu drawn %u %u\n", c.quads.size(), drawn[0], drawn[1]); info += b;
    // The .txt last: the harness waits for it, so every image is complete by the time it appears.
    char path[260]; snprintf(path, sizeof path, "SVR_seen_%s.txt", c.tag.c_str());
    FILE* f = nullptr;
    if (!fopen_s(&f, path, "w") && f) { fputs(info.c_str(), f); fclose(f); }
    L("compose '%s' layers %u quads %zu drawn L%u R%u", c.tag.c_str(), c.layerCount, c.quads.size(), drawn[0], drawn[1]);
}

void Compose(const XrFrameEndInfo* fei, const char* tag, bool worldFixed, bool full, bool fovLock, bool poseLock)
{
    Captured c = Capture(fei, tag, worldFixed, full, fovLock, poseLock);
    Render(c);
}

// A sequence in progress. Its frames are CAPTURED as they pass (a GPU copy each) and composed once
// the last one is in, so the frames are consecutive at the app's own pace, as a headset shows them.
struct Sequence
{
    char tag[64] = "";
    int left = 0, index = 0;
    bool world = false, full = false, fovLock = false, poseLock = false;
    std::vector<Captured> frames;
} gSeq;
void FlushSequence()
{
    if (gSeq.frames.empty()) { gSeq.left = 0; return; }
    L("compose sequence '%s' cut short after %zu frames (the session went away)", gSeq.tag, gSeq.frames.size());
    for (Captured& c : gSeq.frames) Render(c);
    gSeq.frames.clear();
    gSeq.left = 0;
}

/**
 * What a real runtime checks before accepting a frame. A layer the compositor cannot show (a dead
 * swapchain or space handle, a swapchain never released, a rect outside its image) is refused, as
 * a runtime does, instead of being drawn from garbage. @return XR_SUCCESS or the error, and why.
 */
XrResult ValidateFrame(const XrFrameEndInfo* fei, char* why, size_t whyLen)
{
    if (!fei) { snprintf(why, whyLen, "no XrFrameEndInfo"); return XR_ERROR_VALIDATION_FAILURE; }
    if (fei->displayTime <= 0) { snprintf(why, whyLen, "displayTime %lld", (long long)fei->displayTime); return XR_ERROR_TIME_INVALID; }
    if (fei->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
    {
        snprintf(why, whyLen, "environmentBlendMode %d", (int)fei->environmentBlendMode); return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    }
    if (fei->layerCount > kMaxLayers) { snprintf(why, whyLen, "%u layers > %u", fei->layerCount, kMaxLayers); return XR_ERROR_LAYER_LIMIT_EXCEEDED; }
    if (fei->layerCount && !fei->layers) { snprintf(why, whyLen, "layers is null"); return XR_ERROR_LAYER_INVALID; }
    auto subImage = [&](uint32_t li, const XrSwapchainSubImage& si) -> XrResult {
        if (!SwapchainLive(si.swapchain)) { snprintf(why, whyLen, "layer %u: swapchain %p is not a live handle", li, (void*)si.swapchain); return XR_ERROR_HANDLE_INVALID; }
        const MockSwapchain* sc = (const MockSwapchain*)si.swapchain;
        if (sc->released < 0) { snprintf(why, whyLen, "layer %u: swapchain %p never released an image", li, (void*)sc); return XR_ERROR_LAYER_INVALID; }
        const XrRect2Di& r = si.imageRect;
        if (r.offset.x < 0 || r.offset.y < 0 || r.extent.width <= 0 || r.extent.height <= 0 ||
            (int64_t)r.offset.x + r.extent.width > (int64_t)sc->w || (int64_t)r.offset.y + r.extent.height > (int64_t)sc->h)
        {
            snprintf(why, whyLen, "layer %u: rect %d %d %d %d outside the %ux%u swapchain", li, r.offset.x, r.offset.y, r.extent.width, r.extent.height, sc->w, sc->h);
            return XR_ERROR_SWAPCHAIN_RECT_INVALID;
        }
        if (si.imageArrayIndex >= sc->arraySize) { snprintf(why, whyLen, "layer %u: array index %u >= %u", li, si.imageArrayIndex, sc->arraySize); return XR_ERROR_VALIDATION_FAILURE; }
        return XR_SUCCESS;
    };
    for (uint32_t li = 0; li < fei->layerCount; li++)
    {
        const XrCompositionLayerBaseHeader* lh = fei->layers[li];
        if (!lh) { snprintf(why, whyLen, "layer %u is null", li); return XR_ERROR_LAYER_INVALID; }
        if (!SpaceLive(lh->space)) { snprintf(why, whyLen, "layer %u: space %p is not a live handle", li, (void*)lh->space); return XR_ERROR_HANDLE_INVALID; }
        if (lh->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
        {
            const XrCompositionLayerProjection* p = (const XrCompositionLayerProjection*)lh;
            if (p->viewCount != 2 || !p->views) { snprintf(why, whyLen, "layer %u: projection with %u views", li, p->viewCount); return XR_ERROR_VALIDATION_FAILURE; }
            for (uint32_t v = 0; v < 2; v++) { const XrResult r = subImage(li, p->views[v].subImage); if (r != XR_SUCCESS) return r; }
        }
        else if (lh->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
        {
            const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)lh;
            if ((int)q->eyeVisibility < 0 || (int)q->eyeVisibility > 2) { snprintf(why, whyLen, "layer %u: eyeVisibility %d", li, (int)q->eyeVisibility); return XR_ERROR_VALIDATION_FAILURE; }
            const XrResult r = subImage(li, q->subImage); if (r != XR_SUCCESS) return r;
        }
        else { snprintf(why, whyLen, "layer %u: type %d is not supported by the mock", li, (int)lh->type); return XR_ERROR_LAYER_INVALID; }
    }
    return XR_SUCCESS;
}

MOCK_FN Mock_xrEndFrame(XrSession session, const XrFrameEndInfo* fei)
{
    {
        std::lock_guard<std::mutex> lk(gSm);
        const XrResult r = SessionCheckLocked(session);
        if (r != XR_SUCCESS) return r;
        if (gLossQueued) { if (gBegun > gEnded) gEnded = gBegun; return XR_SESSION_LOSS_PENDING; }
        if (!gRunning) return XR_ERROR_SESSION_NOT_RUNNING;
        if (gBegun <= gEnded)
        {
            static unsigned n = 0;
            if (n++ < 20) L("xrEndFrame CALL ORDER: no begun frame to end (XR_ERROR_CALL_ORDER_INVALID)");
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        ++gEnded;
        // The hourglass: what the headset shows when a running session's frames stop coming.
        const int64_t now = NowNs(), last = gLastEndNs.exchange(now);
        const double gap = last ? Sec(now - last) : 0.0;
        if (gState != XR_SESSION_STATE_STOPPING && gap > 1.0)
        {
            gHourglasses++; if (gap > gHourglassMax) gHourglassMax = gap;
            L("HOURGLASS #%u: no frame for %.2f s (longest %.2f s) t=%.3f", gHourglasses, gap, gHourglassMax, Sec(now));
        }
        gHourglassOngoing = false;
    }
    {
        char why[256] = "";
        const XrResult v = ValidateFrame(fei, why, sizeof why);
        if (v != XR_SUCCESS)
        {
            static unsigned n = 0;
            if (n < 20 || n % 500 == 0) L("xrEndFrame REJECTED %d: %s (#%u)", (int)v, why, n + 1);
            ++n;
            return v;
        }
    }
    static uint64_t n = 0;
    if ((n++ % 300) == 0) L("xrEndFrame #%llu layers=%u", (unsigned long long)n, fei->layerCount);
    ++gEndFrames;
    // A sequence in progress: one more frame of it.
    if (gSeq.left > 0)
    {
        char tag[96]; snprintf(tag, sizeof tag, "%s_f%02d", gSeq.tag, gSeq.index++);
        gSeq.frames.push_back(Capture(fei, tag, gSeq.world, gSeq.full, gSeq.fovLock, gSeq.poseLock));
        if (--gSeq.left == 0)
        {
            for (Captured& c : gSeq.frames) Render(c);
            gSeq.frames.clear();
            gLastEndNs = NowNs();   // composing is the mock's time, not a gap in the app's frames
        }
        return XR_SUCCESS;
    }
    if (GetFileAttributesA("SVR_MockCompose.txt") != INVALID_FILE_ATTRIBUTES)
    {
        char text[256] = {};
        FILE* f = _fsopen("SVR_MockCompose.txt", "r", _SH_DENYNO);
        size_t got = 0;
        if (f) { got = fread(text, 1, sizeof(text) - 1, f); fclose(f); }
        if (got == 0 || text[got - 1] != '\n') return XR_SUCCESS;   // still being written
        DeleteFileA("SVR_MockCompose.txt");
        char tag[64] = "seen";
        bool world = false, full = false, fovLock = false, poseLock = false;
        int seq = 0;
        char* context = nullptr;
        int word = 0;
        for (char* tok = strtok_s(text, " \t\r\n", &context); tok; tok = strtok_s(nullptr, " \t\r\n", &context), ++word)
        {
            if (word == 0) { strncpy_s(tag, tok, _TRUNCATE); continue; }
            if (strcmp(tok, "world") == 0) world = true;
            else if (strcmp(tok, "full") == 0) full = true;
            else if (strcmp(tok, "fovlock") == 0) fovLock = true;
            else if (strcmp(tok, "poselock") == 0) poseLock = true;
            else if (strcmp(tok, "seq") == 0)
            {
                char* nTok = strtok_s(nullptr, " \t\r\n", &context);
                if (nTok) seq = atoi(nTok);
            }
        }
        if (seq > 1)
        {
            strncpy_s(gSeq.tag, tag, _TRUNCATE);
            gSeq.index = 0; gSeq.world = world; gSeq.full = full; gSeq.fovLock = fovLock; gSeq.poseLock = poseLock;
            char first[96]; snprintf(first, sizeof first, "%s_f%02d", gSeq.tag, gSeq.index++);
            gSeq.frames.clear();
            gSeq.frames.push_back(Capture(fei, first, world, full, fovLock, poseLock));
            gSeq.left = seq - 1;
        }
        else
        {
            Compose(fei, tag, world, full, fovLock, poseLock);
            gLastEndNs = NowNs();   // composing is the mock's time, not a gap in the app's frames
        }
    }
    return XR_SUCCESS;
}

// Input file, all fields optional (missing = keep previous):
//   turnX trigR handYaw handPitch headYaw headPitch headX headY headZ
//   moveX moveY handX handY handZ btnA btnB btnX btnY
//   turnY trigL gripL gripR btnThumbL btnThumbR btnMenu
//   lhandYaw lhandPitch lhandX lhandY lhandZ
//   handRoll lhandRoll
// Angles in degrees; positions in meters (offsets from the defaults);
// buttons are 0/1. New fields appended at the end so old step lists keep working.
// hand* fields drive the RIGHT hand; lhand* the LEFT (psi aim / VR hands).
//
// Roll is the axis a wrist actually turns about and the one the weapon's pivot bug shows up in,
// so it is not optional for testing: without it "pure rotation must not translate" cannot be
// exercised at all. Appended last, so every existing step list keeps working unchanged.
float gInTurnX = 0, gInTrigR = 0, gInHandYaw = 0, gInHandPitch = 0;
float gInHeadYaw = 0, gInHeadPitch = 0, gInHeadX = 0, gInHeadY = 0, gInHeadZ = 0;
float gInMoveX = 0, gInMoveY = 0, gInHandX = 0, gInHandY = 0, gInHandZ = 0;
float gInBtnA = 0, gInBtnB = 0, gInBtnX = 0, gInBtnY = 0;
float gInTurnY = 0, gInTrigL = 0, gInGripL = 0, gInGripR = 0;
float gInBtnThumbL = 0, gInBtnThumbR = 0, gInBtnMenu = 0;
float gInLHandYaw = 0, gInLHandPitch = 0, gInLHandX = 0, gInLHandY = 0, gInLHandZ = 0;
float gInHandRoll = 0, gInLHandRoll = 0;
bool gHaveInput = false; //!< once a file was read, head pose is file-driven

// Head motion as a function of TIME (SVR_MockMotion.txt = "yaw <amplitude deg> <rate rad/s>"): the
// head yaws by amplitude * sin(rate * t) on top of the input file's yaw, evaluated at whatever time is
// asked. xrLocateViews(t) is then a perfect predictor and the compositor knows where the eye really is
// when a frame is displayed, so an app that renders for one time and submits for another shows the
// difference as swim -- as on a headset. Without the file the head is still between input writes.
bool gMotionOn = false;
struct MotionTerm { int axis; float ampRad; float rate; float phase; };
MotionTerm gMotion[24] = {};
int gMotionCount = 0;
int64_t gMotionT0 = 0;
/** The timed motion's yaw, pitch and roll (radians) at time t. */
void MotionAt(XrTime t, float& yaw, float& pitch, float& roll)
{
    yaw = pitch = roll = 0.0f;
    if (!gMotionOn) return;
    const float s = (float)((double)(t - gMotionT0) * 1e-9);
    for (int i = 0; i < gMotionCount; ++i)
    {
        const float v = gMotion[i].ampRad * sinf(gMotion[i].rate * s + gMotion[i].phase);
        (gMotion[i].axis == 0 ? yaw : gMotion[i].axis == 1 ? pitch : roll) += v;
    }
}

void ReadMockMotion()
{
    static std::mutex m; std::lock_guard<std::mutex> lk(m);   // xrWaitFrame and xrSyncActions may be on different threads
    static FILETIME lastWrite = {};
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA("SVR_MockMotion.txt", GetFileExInfoStandard, &fad))
    {
        if (gMotionOn) L("motion off");
        gMotionOn = false;
        lastWrite = {};
        return;
    }
    if (fad.ftLastWriteTime.dwLowDateTime == lastWrite.dwLowDateTime && fad.ftLastWriteTime.dwHighDateTime == lastWrite.dwHighDateTime) return;
    char text[1024] = {};
    FILE* f = _fsopen("SVR_MockMotion.txt", "r", _SH_DENYNO);
    if (!f) return;
    const size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    if (got == 0 || text[got - 1] != '\n') return;
    lastWrite = fad.ftLastWriteTime;
    const float d2r = 3.14159265f / 180.0f;
    const float tau = 6.2831853f;
    gMotionCount = 0;
    char* ctx = nullptr;
    for (char* line = strtok_s(text, "\r\n", &ctx); line; line = strtok_s(nullptr, "\r\n", &ctx))
    {
        char kind[16] = {};
        float amp = 0, rate = 0;
        const int n = sscanf_s(line, "%15s %f %f", kind, (unsigned)sizeof kind, &amp, &rate);
        const int axis = strcmp(kind, "yaw") == 0 ? 0 : strcmp(kind, "pitch") == 0 ? 1 : strcmp(kind, "roll") == 0 ? 2 : -1;
        if (n == 3 && axis >= 0 && gMotionCount < 24)
        {
            gMotion[gMotionCount++] = { axis, amp * d2r, rate, 0.0f };
        }
        else if (n >= 2 && strcmp(kind, "shake") == 0)
        {
            // Relative amplitudes and frequencies (Hz) of a standing person's head tremor.
            static const float k[][3] = {
                {0, 0.55f, 0.31f}, {0, 0.45f, 1.30f}, {0, 0.30f, 2.70f}, {0, 0.18f, 4.30f}, {0, 0.10f, 6.10f},
                {1, 0.45f, 0.27f}, {1, 0.35f, 1.70f}, {1, 0.22f, 3.30f}, {1, 0.12f, 5.30f},
                {2, 0.60f, 0.23f}, {2, 0.35f, 1.10f}, {2, 0.20f, 2.90f} };
            for (const auto& term : k)
            {
                if (gMotionCount >= 24) break;
                gMotion[gMotionCount] = { (int)term[0], amp * term[1] * d2r, term[2] * tau, 0.7f * (float)gMotionCount };
                ++gMotionCount;
            }
        }
    }
    gMotionT0 = NowNs();
    gMotionOn = gMotionCount > 0;
    L("motion terms=%d", gMotionCount);
}

/** The head's yaw and pitch (radians) at time t, in the ROOM. */
void HeadAnglesAt(XrTime t, float& yaw, float& pitch, float& roll)
{
    roll = 0;
    // File-driven once SVR_MockInput.txt exists (deterministic tests), plus the timed motion;
    // otherwise a slow sine yaw sweep so the pipeline carries changing data.
    pitch = 0;
    if (gHaveInput)
    {
        yaw = gInHeadYaw * 3.14159265f / 180.0f;
        pitch = gInHeadPitch * 3.14159265f / 180.0f;
        float my, mp, mr; MotionAt(t, my, mp, mr);
        yaw += my; pitch += mp; roll += mr;
    }
    else
    {
        yaw = 0.6f * sinf((float)gFrameTime * 0.02f);
    }
}
/** q = yaw(Y) * pitch(X) * roll(Z). */
XrQuaternionf HeadQuat(float yaw, float pitch, float roll = 0.0f)
{
    const float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    const float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    const XrQuaternionf a{ cy * sp, sy * cp, -sy * sp, cy * cp };
    if (roll == 0.0f) return a;
    const float cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
    // a * (0, 0, sr, cr)
    return { a.x * cr + a.y * sr, -a.x * sr + a.y * cr, a.w * sr + a.z * cr, a.w * cr - a.z * sr };
}
/** The head's centre (between the eyes) at time t, in the ROOM. */
XrPosef HeadPoseRoomAt(XrTime t)
{
    float yaw, pitch, roll; HeadAnglesAt(t, yaw, pitch, roll);
    XrPosef pose;
    pose.orientation = HeadQuat(yaw, pitch, roll);
    pose.position = { gInHeadX, 1.6f + gInHeadY, gInHeadZ };
    return pose;
}

XrPosef EyePoseAt(int eye, XrTime t)
{
    float yaw, pitch, roll; HeadAnglesAt(t, yaw, pitch, roll);
    XrPosef pose;
    pose.orientation = HeadQuat(yaw, pitch, roll);
    // The eyes sit +-32 mm along the head's own right axis (~IPD), so they turn with the head as
    // a real headset's do: rendering from one eye must move that eye when the head yaws. A pitch
    // about X leaves an X offset unchanged; the yaw about Y turns (e, 0, 0) into (e cos, 0, -e sin).
    ReadHmdConfig();
    const float e = (eye == 0 ? -0.5f : 0.5f) * gIpd;
    pose.position = { e * cosf(yaw) + gInHeadX, 1.6f + gInHeadY, -e * sinf(yaw) + gInHeadZ }; // standing height + offsets
    return ToLocalAt(pose, t);   // the room until a recentre moves LOCAL
}

MOCK_FN Mock_xrLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* vs,
    uint32_t cap, uint32_t* count, XrView* views)
{
    {
        std::lock_guard<std::mutex> lk(gSm);
        const XrResult r = SessionCheckLocked(session);
        if (r != XR_SUCCESS) return r;
    }
    *count = 2;
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (cap < 2) return XR_SUCCESS;
    const XrTime t = info ? info->displayTime : NowNs();
    // Relative to the space asked for (LOCAL, the usual one, is the identity).
    XrPosef base = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
    if (info && !LocateInLocal(info->space, t, base)) return XR_ERROR_HANDLE_INVALID;
    const XrPosef inv = PoseInv(base);
    for (int i = 0; i < 2; i++)
    {
        views[i].pose = PoseMul(inv, EyePoseAt(i, t));
        views[i].fov = EyeFov(i);
        gLastViewPose[i] = views[i].pose;
    }
    return XR_SUCCESS;
}

// ---- Input (controller actions) ----
struct MockActionSet_T { int _; } gActionSet;
struct MockAction { std::string name; XrActionType type; };

void ReadMockInput()
{
    static std::mutex m; std::lock_guard<std::mutex> lk(m);   // xrWaitFrame and xrSyncActions may be on different threads
    // Only re-read when the file changed: opening it every frame races with the test
    // harness writer (sharing violations) and made short input pulses unreliable.
    static FILETIME lastWrite = {};
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA("SVR_MockInput.txt", GetFileExInfoStandard, &fad)) return;
    if (fad.ftLastWriteTime.dwLowDateTime == lastWrite.dwLowDateTime && fad.ftLastWriteTime.dwHighDateTime == lastWrite.dwHighDateTime) return;
    // The write time is only marked as consumed once the WHOLE file has been read. Marking it first
    // lost any pose whose read collided with the harness still writing it (a sharing violation, or
    // half a line): that pose was never retried, and the test silently ran with the previous one.
    char text[1024] = {};
    FILE* f = _fsopen("SVR_MockInput.txt", "r", _SH_DENYNO);
    if (!f) return;
    const size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    if (got == 0 || text[got - 1] != '\n') return;   // empty or still being written: next frame
    lastWrite = fad.ftLastWriteTime;
    {
        int n = sscanf_s(text, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f "
                        "%f %f",
            &gInTurnX, &gInTrigR, &gInHandYaw, &gInHandPitch,
            &gInHeadYaw, &gInHeadPitch, &gInHeadX, &gInHeadY, &gInHeadZ,
            &gInMoveX, &gInMoveY, &gInHandX, &gInHandY, &gInHandZ,
            &gInBtnA, &gInBtnB, &gInBtnX, &gInBtnY,
            &gInTurnY, &gInTrigL, &gInGripL, &gInGripR,
            &gInBtnThumbL, &gInBtnThumbR, &gInBtnMenu,
            &gInLHandYaw, &gInLHandPitch, &gInLHandX, &gInLHandY, &gInLHandZ,
            &gInHandRoll, &gInLHandRoll);
        if (n >= 1)
            gHaveInput = true;
    }
}

// ---- Failure emulation: SVR_MockEvent.txt (the words are listed at the top of the file) ----
void ExecEvent(const char* word, const char* arg)
{
    const int64_t now = NowNs();
    const bool hasArg = arg && *arg;
    const double val = hasArg ? strtod(arg, nullptr) : 0.0;   // the DLL's CRT stays in the "C" locale: '.' decimals
    std::lock_guard<std::mutex> lk(gSm);
    if (strcmp(word, "loss") == 0)
    {
        if (!gSessionExists || gLost || gLossQueued) { L("ev=mock event=loss ignored (no live session) t=%.3f", Sec(now)); return; }
        gLossQueued = true;
        gEvq.erase(std::remove_if(gEvq.begin(), gEvq.end(), [](const MockEvent& e) { return e.kind == EvKind::State; }), gEvq.end());
        QueueStateLocked(XR_SESSION_STATE_LOSS_PENDING);
        gFocused = false;
        L("ev=mock event=loss state=LOSS_PENDING t=%.3f (frame calls return XR_SESSION_LOSS_PENDING until it is polled, then XR_ERROR_SESSION_LOST)", Sec(now));
    }
    else if (strcmp(word, "nohmd") == 0)
    {
        if (hasArg && val == 0.0) { gNoHmdUntilNs = 0; L("ev=mock event=nohmd seconds=0 (headset back) t=%.3f", Sec(now)); return; }
        gNoHmdUntilNs = (hasArg && val > 0.0) ? now + (int64_t)(val * 1e9) : INT64_MAX;
        if (gNoHmdUntilNs == INT64_MAX) L("ev=mock event=nohmd seconds=forever t=%.3f (until `hmd`)", Sec(now));
        else L("ev=mock event=nohmd seconds=%.2f t=%.3f", val, Sec(now));
    }
    else if (strcmp(word, "hmd") == 0)
    {
        gNoHmdUntilNs = 0;
        L("ev=mock event=hmd t=%.3f (headset back)", Sec(now));
    }
    else if (strcmp(word, "stall") == 0)
    {
        gStallArmedSec = val > 0.0 ? val : 0.0;
        L("ev=mock event=stall seconds=%.2f t=%.3f (armed for the next xrWaitFrame of a running session)", gStallArmedSec, Sec(now));
    }
    else if (strcmp(word, "stop") == 0)
    {
        if (!gRunning || gLost || gLossQueued || gState == XR_SESSION_STATE_STOPPING) { L("ev=mock event=stop ignored (session not running, state %s) t=%.3f", StateName(gState), Sec(now)); return; }
        gReadyAfterSec = hasArg ? val : -1.0;
        QueueStopLocked();
        if (gReadyAfterSec >= 0.0) L("ev=mock event=stop ready_after=%.2f t=%.3f (-> STOPPING)", gReadyAfterSec, Sec(now));
        else L("ev=mock event=stop ready_after=never t=%.3f (-> STOPPING)", Sec(now));
    }
    else if (strcmp(word, "exit") == 0)
    {
        if (!gSessionExists || gLost || gLossQueued) { L("ev=mock event=exit ignored (no live session) t=%.3f", Sec(now)); return; }
        if (gRunning)
        {
            gExitPending = true;
            QueueStopLocked();
            L("ev=mock event=exit t=%.3f (-> STOPPING, EXITING after xrEndSession)", Sec(now));
        }
        else
        {
            if (gState == XR_SESSION_STATE_READY) QueueStateLocked(XR_SESSION_STATE_IDLE);
            gReadyAtNs = -1;
            QueueStateLocked(XR_SESSION_STATE_EXITING);
            L("ev=mock event=exit t=%.3f (not running: -> EXITING)", Sec(now));
        }
    }
    else if (strcmp(word, "recentre") == 0 || strcmp(word, "recenter") == 0)
    {
        if (!gSessionExists || gLost) { L("ev=mock event=recentre ignored (no live session) t=%.3f", Sec(now)); return; }
        // In effect for every time after the frames already handed out.
        const XrTime tc = (gLastPredicted.load() > now ? gLastPredicted.load() : now) + 1;
        float yaw, pitch, roll; HeadAnglesAt(tc, yaw, pitch, roll);
        LocalOrigin nu; nu.moved = true; nu.yaw = yaw;
        nu.o[0] = gInHeadX; nu.o[1] = 1.6f + gInHeadY; nu.o[2] = gInHeadZ;
        LocalOrigin prev;
        {
            std::lock_guard<std::mutex> lo(gOriginM);
            gOrPrev = gOrCur; prev = gOrCur;
            gOrCur = nu; gOrChangeTime = tc;
        }
        // The new origin, in the previous LOCAL.
        XrPosef inPrev; inPrev.orientation = QYaw(nu.yaw); inPrev.position = { nu.o[0], nu.o[1], nu.o[2] };
        inPrev = ApplyOrigin(prev, inPrev);
        MockEvent e; e.kind = EvKind::RefSpace; e.time = tc; e.pose = inPrev;
        gEvq.push_back(e);
        L("ev=mock event=recentre changeTime=%lld t=%.3f LOCAL origin -> room (%.3f,%.3f,%.3f) yaw %.2f deg; in previous LOCAL p (%.3f,%.3f,%.3f) q (%.4f,%.4f,%.4f,%.4f)",
          (long long)tc, Sec(now), nu.o[0], nu.o[1], nu.o[2], nu.yaw * 57.2957795f,
          inPrev.position.x, inPrev.position.y, inPrev.position.z, inPrev.orientation.x, inPrev.orientation.y, inPrev.orientation.z, inPrev.orientation.w);
    }
    else if (strcmp(word, "instanceloss") == 0)
    {
        if (!gInstanceAlive || gInstanceLossQueued || gInstanceLost) { L("ev=mock event=instanceloss ignored (no live instance) t=%.3f", Sec(now)); return; }
        gInstanceLossQueued = true;
        MockEvent e; e.kind = EvKind::InstanceLoss; e.time = now;
        gEvq.push_back(e);
        L("ev=mock event=instanceloss t=%.3f (XR_ERROR_INSTANCE_LOST once polled)", Sec(now));
    }
    else
    {
        L("ev=mock event=unknown word=%s", word);
    }
}

void ReadMockEvents()
{
    static std::mutex m; std::lock_guard<std::mutex> lk(m);
    static FILETIME lastWrite = {};   // a file that could not be deleted: executed once, never again
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA("SVR_MockEvent.txt", GetFileExInfoStandard, &fad)) return;
    if (fad.ftLastWriteTime.dwLowDateTime == lastWrite.dwLowDateTime && fad.ftLastWriteTime.dwHighDateTime == lastWrite.dwHighDateTime) return;
    char text[2048] = {};
    FILE* f = _fsopen("SVR_MockEvent.txt", "r", _SH_DENYNO);
    if (!f) return;
    const size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    if (got == 0 || text[got - 1] != '\n') return;   // empty or still being written: next time
    if (DeleteFileA("SVR_MockEvent.txt")) lastWrite = {};
    else { lastWrite = fad.ftLastWriteTime; L("ev=mock event file could not be deleted (err %lu): its write time is remembered", GetLastError()); }
    char* lineCtx = nullptr;
    for (char* line = strtok_s(text, "\r\n", &lineCtx); line; line = strtok_s(nullptr, "\r\n", &lineCtx))
    {
        if (char* hash = strchr(line, '#')) *hash = 0;
        char* wordCtx = nullptr;
        char* word = strtok_s(line, " \t", &wordCtx);
        if (!word) continue;
        char* arg = strtok_s(nullptr, " \t", &wordCtx);
        ExecEvent(word, arg);
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
    MockSpace* s = new MockSpace();
    s->action = true;
    s->hand = (a && a->name.size() && a->name.back() == 'r') ? 1 : 0;
    s->palm = (a && a->name.compare(0, 4, "palm") == 0);
    s->offset = ci->poseInActionSpace;
    s->name = a ? a->name : "?";
    { std::lock_guard<std::mutex> lk(gObjM); gSpaces.push_back(s); }
    L("xrCreateActionSpace name=%s hand=%d palm=%d", a ? a->name.c_str() : "?", s->hand, s->palm ? 1 : 0);
    *out = (XrSpace)s;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrSyncActions(XrSession, const XrActionsSyncInfo*)
{
    ReadMockEvents(); ReadMockInput(); ReadMockMotion();
    // Outside FOCUSED (a headset taken off, the runtime's own menu) the app gets no input.
    return gFocused.load() ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}
MOCK_FN Mock_xrGetActionStateFloat(XrSession, const XrActionStateGetInfo* gi, XrActionStateFloat* st)
{
    MockAction* a = (MockAction*)gi->action;
    st->isActive = gFocused.load() ? XR_TRUE : XR_FALSE;
    st->currentState = 0.0f;
    if (a && st->isActive)
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
    st->isActive = gFocused.load() ? XR_TRUE : XR_FALSE;
    st->currentState = { 0.0f, 0.0f };
    if (!st->isActive) return XR_SUCCESS;
    if (a && a->name == "move")
        st->currentState = { gInMoveX, gInMoveY };
    else if (a && a->name == "turn")   // right thumbstick as one vector, as a real Touch reports it
        st->currentState = { gInTurnX, gInTurnY };
    return XR_SUCCESS;
}
MOCK_FN Mock_xrGetActionStatePose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* st)
{
    st->isActive = gFocused.load() ? XR_TRUE : XR_FALSE; return XR_SUCCESS;
}
MOCK_FN Mock_xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo* gi, XrActionStateBoolean* st)
{
    MockAction* a = (MockAction*)gi->action;
    st->isActive = gFocused.load() ? XR_TRUE : XR_FALSE;
    st->currentState = XR_FALSE;
    if (a && st->isActive)
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

/** A controller's pose in the ROOM (aim, or grip for a `palm_*` space). */
XrPosef HandPoseRoom(const MockSpace* s)
{
    // Simulated hand aim poses: in front of the head, aiming along -Z. The
    // right hand is driven by hand*, the left by lhand* (independent, so
    // left-hand psi aim and VR-hand tests can steer them separately).
    const bool right = (s && s->hand == 1);
    float yaw = (right ? gInHandYaw : gInLHandYaw) * 3.14159265f / 180.0f;
    float pitch = (right ? gInHandPitch : gInLHandPitch) * 3.14159265f / 180.0f;
    float roll = (right ? gInHandRoll : gInLHandRoll) * 3.14159265f / 180.0f;
    // q = yaw(Y) * pitch(X) * roll(Z), composed properly rather than by hand: roll is the third
    // axis and the shortcut that produced the two-axis form does not extend to it.
    const XrQuaternionf qy = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
    const XrQuaternionf qp = { sinf(pitch * 0.5f), 0.0f, 0.0f, cosf(pitch * 0.5f) };
    const XrQuaternionf qr = { 0.0f, 0.0f, sinf(roll * 0.5f), cosf(roll * 0.5f) };
    const XrQuaternionf q = QMul(QMul(qy, qp), qr);
    XrPosef pose;
    pose.orientation = q;
    if (right)
        pose.position = { 0.2f + gInHandX, 1.4f + gInHandY, -0.3f + gInHandZ };
    else
        pose.position = { -0.2f + gInLHandX, 1.4f + gInLHandY, -0.3f + gInLHandZ };
    // The grip (palm) pose sits behind and below the aim origin, in the controller's own frame,
    // roughly where a Touch controller puts it: 7 cm back along +Z and 3 cm down. Fixed offset,
    // so it rotates with the controller -- which is exactly the property a consumer that confuses
    // the two poses gets wrong.
    if (s && s->palm)
    {
        const XrVector3f d = QRot(q, XrVector3f{ 0.0f, -0.03f, 0.07f });
        pose.position.x += d.x;
        pose.position.y += d.y;
        pose.position.z += d.z;
    }
    return pose;
}

bool LocateInLocal(XrSpace h, XrTime t, XrPosef& out)
{
    if (!SpaceLive(h)) return false;
    const MockSpace* s = (const MockSpace*)h;
    XrPosef base;
    if (s->action) base = ToLocalAt(HandPoseRoom(s), t);
    else if (s->ref == XR_REFERENCE_SPACE_TYPE_VIEW) base = ToLocalAt(HeadPoseRoomAt(t), t);
    else if (s->ref == XR_REFERENCE_SPACE_TYPE_STAGE) base = ToLocalAt(XrPosef{ { 0, 0, 0, 1 }, { 0, 0, 0 } }, t);   // the room; LOCAL until a recentre
    else base = { { 0, 0, 0, 1 }, { 0, 0, 0 } };   // LOCAL
    out = PoseMul(base, s->offset);
    return true;
}

MOCK_FN Mock_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* loc)
{
    XrPosef a, b;
    if (!LocateInLocal(space, time, a) || !LocateInLocal(baseSpace, time, b)) return XR_ERROR_HANDLE_INVALID;
    loc->pose = PoseMul(PoseInv(b), a);
    loc->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    return XR_SUCCESS;
}

/**
 * Haptics: logged, so a test can see the pulse that confirms taking hold of the weapon -- the one
 * piece of feedback a player gets in the hand rather than on screen.
 */
MOCK_FN Mock_xrApplyHapticFeedback(XrSession, const XrHapticActionInfo* info, const XrHapticBaseHeader* header)
{
    const MockAction* a = info ? (const MockAction*)info->action : nullptr;
    const XrHapticVibration* v = (header && header->type == XR_TYPE_HAPTIC_VIBRATION) ? (const XrHapticVibration*)header : nullptr;
    L("haptic action=%s amplitude=%.2f ms=%.1f", a ? a->name.c_str() : "?", v ? v->amplitude : -1.0f,
      v ? (double)v->duration / 1e6 : -1.0);
    return XR_SUCCESS;
}
MOCK_FN Mock_xrStopHapticFeedback(XrSession, const XrHapticActionInfo*) { return XR_SUCCESS; }

MOCK_FN Mock_xrAcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{
    if (!SwapchainLive(h)) return XR_ERROR_HANDLE_INVALID;
    MockSwapchain* sc = (MockSwapchain*)h;
    *index = sc->acquired;
    sc->current = (int32_t)sc->acquired;
    sc->acquired = (sc->acquired + 1) % (uint32_t)sc->tex.size();
    return XR_SUCCESS;
}
MOCK_FN Mock_xrWaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo*)
{
    return SwapchainLive(h) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
MOCK_FN Mock_xrReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo*)
{
    if (!SwapchainLive(h)) return XR_ERROR_HANDLE_INVALID;
    MockSwapchain* sc = (MockSwapchain*)h;
    if (sc->current >= 0) { sc->released = sc->current; sc->current = -1; ++sc->releases; }
    return XR_SUCCESS;
}

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
    BIND(xrEnumerateViewConfigurations);
    BIND(xrEnumerateEnvironmentBlendModes);
    BIND(xrEnumerateViewConfigurationViews);
    BIND(xrGetD3D11GraphicsRequirementsKHR);
    BIND(xrCreateSession);
    BIND(xrDestroySession);
    BIND(xrEnumerateReferenceSpaces);
    BIND(xrCreateReferenceSpace);
    BIND(xrDestroySpace);
    BIND(xrEnumerateSwapchainFormats);
    BIND(xrCreateSwapchain);
    BIND(xrDestroySwapchain);
    BIND(xrEnumerateSwapchainImages);
    BIND(xrPollEvent);
    BIND(xrBeginSession);
    BIND(xrEndSession);
    BIND(xrRequestExitSession);
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
    BIND(xrApplyHapticFeedback);
    BIND(xrStopHapticFeedback);
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
