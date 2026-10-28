/**
 * OpenXR runtime, instance, session and frame loop for the VR module.
 *
 * The loader is ours. The OpenXR SDK ships a loader library, but linking it here is not possible:
 * the only build of it on this machine is compiled against the dynamic CRT while this DLL uses
 * the static one, and mixing those is a link error waiting to happen. The loader's job is small
 * and well specified -- find the active runtime's manifest, read the library out of it, and
 * negotiate an interface version -- so this file does it directly. That also means the mock
 * runtime and a real one are reached by exactly the same path.
 *
 * Everything here runs on the present thread, which is the only site that owns the render device.
 * The pose is published under a lock and the camera hook reads it from its own thread, so the
 * camera lags the headset by one frame. That is fine for a monoscopic proof of concept and is the
 * first thing to fix if the view ever feels swimmy.
 */

#include "xr_runtime.h"

#include <Windows.h>

#include <winternl.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#include "../../../core/logging/log.h"
#include "vr_compositor.h"
#include "vr_debug.h"
#include "vr_display.h"
#include "vr_frames.h"
#include "vr_gpu.h"
#include "vr_screen.h"
#include "vr_settings.h"

namespace sunrise::client::hooks::vr::xr {
namespace {

/**
 * The loader/runtime negotiation ABI. It lives in the loader's own sources rather than the public
 * SDK headers, so the three declarations it needs are reproduced here. Stable interface v1.
 */
constexpr std::uint32_t kLoaderRuntimeVersion = 1;
constexpr std::uint32_t kLoaderInfoStructVersion = 1;
constexpr std::uint32_t kRuntimeInfoStructVersion = 1;

enum XrLoaderInterfaceStructs : int {
    kStructUninitialized = 0,
    kStructLoaderInfo,
    kStructApiLayerRequest,
    kStructRuntimeRequest,
    kStructApiLayerCreateInfo,
    kStructApiLayerNextInfo,
};

struct XrNegotiateLoaderInfo {
    int structType;
    std::uint32_t structVersion;
    std::size_t structSize;
    std::uint32_t minInterfaceVersion;
    std::uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
};

struct XrNegotiateRuntimeRequest {
    int structType;
    std::uint32_t structVersion;
    std::size_t structSize;
    std::uint32_t runtimeInterfaceVersion;
    XrVersion runtimeApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
};

using PFN_xrNegotiateLoaderRuntimeInterface =
    XrResult(XRAPI_PTR*)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*);

/** Every entry point this module calls, resolved through the runtime's own proc address. */
struct Api final {
    PFN_xrGetInstanceProcAddr getInstanceProcAddr{};
    PFN_xrCreateInstance createInstance{};
    PFN_xrDestroyInstance destroyInstance{};
    PFN_xrGetInstanceProperties getInstanceProperties{};
    PFN_xrGetSystem getSystem{};
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews{};
    PFN_xrCreateSession createSession{};
    PFN_xrDestroySession destroySession{};
    PFN_xrBeginSession beginSession{};
    PFN_xrEndSession endSession{};
    PFN_xrCreateReferenceSpace createReferenceSpace{};
    PFN_xrDestroySpace destroySpace{};
    PFN_xrPollEvent pollEvent{};
    PFN_xrWaitFrame waitFrame{};
    PFN_xrBeginFrame beginFrame{};
    PFN_xrEndFrame endFrame{};
    PFN_xrLocateViews locateViews{};
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements{};
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats{};
    PFN_xrCreateSwapchain createSwapchain{};
    PFN_xrDestroySwapchain destroySwapchain{};
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages{};
    PFN_xrAcquireSwapchainImage acquireSwapchainImage{};
    PFN_xrWaitSwapchainImage waitSwapchainImage{};
    PFN_xrReleaseSwapchainImage releaseSwapchainImage{};
    PFN_xrStringToPath stringToPath{};
    PFN_xrCreateActionSet createActionSet{};
    PFN_xrDestroyActionSet destroyActionSet{};
    PFN_xrCreateAction createAction{};
    PFN_xrSuggestInteractionProfileBindings suggestInteractionProfileBindings{};
    PFN_xrAttachSessionActionSets attachSessionActionSets{};
    PFN_xrSyncActions syncActions{};
    PFN_xrGetActionStateFloat getActionStateFloat{};
    PFN_xrGetActionStateVector2f getActionStateVector2f{};
    PFN_xrGetActionStateBoolean getActionStateBoolean{};
    PFN_xrGetActionStatePose getActionStatePose{};
    PFN_xrCreateActionSpace createActionSpace{};
    PFN_xrLocateSpace locateSpace{};
    /** Optional: a runtime without haptics still gives head tracking and every input. */
    PFN_xrApplyHapticFeedback applyHapticFeedback{};
};

constexpr std::uint32_t kViewCount = 2;
constexpr std::uint32_t kMaxSwapchainImages = 8;
/** How long to wait for the compositor to hand an image back. Well over one frame. */
constexpr XrDuration kImageWaitNanoseconds = 100'000'000;
/** Frames between throttled submit reports. */
constexpr std::uint64_t kSubmitReportPeriod = 300;
/** Frames between controller pose reports. About a second, so a scripted step can read one. */
constexpr std::uint64_t kHandReportPeriod = 60;

/**
 * The runtime's swapchain the back buffer is copied into, one image for both eyes, and the blit
 * path used when the back buffer's format cannot simply be copied.
 */
struct Presentation final {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    /** Owned by the runtime; never released here. */
    std::array<ID3D11Texture2D*, kMaxSwapchainImages> images{};
    std::array<ID3D11RenderTargetView*, kMaxSwapchainImages> targets{};
    std::uint32_t imageCount{};
    std::uint32_t width{};
    std::uint32_t height{};
    /** Back-buffer format the swapchain was built for; a change rebuilds it. */
    DXGI_FORMAT sourceFormat{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    /** True when CopyResource is legal between the back buffer and the swapchain images. */
    bool copyDirect{};
    bool failed{};
    /** Blit path: an SRV-capable copy of the back buffer and a full-screen pass into the image. */
    ID3D11Texture2D* blitSource{};
    ID3D11ShaderResourceView* blitView{};
    ID3D11VertexShader* blitVertex{};
    ID3D11PixelShader* blitPixel{};
    ID3D11SamplerState* blitSampler{};
    ID3D11RasterizerState* blitRasterizer{};
    ID3DDeviceContextState* blitState{};
};

/** The action set the controllers are read through. Names match the mock runtime's lookup. */
struct Actions final {
    XrActionSet set{XR_NULL_HANDLE};
    XrAction move{XR_NULL_HANDLE};
    XrAction turn{XR_NULL_HANDLE};
    XrAction triggerL{XR_NULL_HANDLE};
    XrAction triggerR{XR_NULL_HANDLE};
    XrAction gripL{XR_NULL_HANDLE};
    XrAction gripR{XR_NULL_HANDLE};
    XrAction a{XR_NULL_HANDLE};
    XrAction b{XR_NULL_HANDLE};
    XrAction x{XR_NULL_HANDLE};
    XrAction y{XR_NULL_HANDLE};
    XrAction thumbL{XR_NULL_HANDLE};
    XrAction thumbR{XR_NULL_HANDLE};
    XrAction menu{XR_NULL_HANDLE};
    /**
     * Aim poses. One action per hand rather than a single action with subaction paths: the mock
     * runtime tells the hands apart by the last character of the action name, so `aim_l`/`aim_r`
     * are load bearing for every test in scripts	esting.
     */
    XrAction aimL{XR_NULL_HANDLE};
    XrAction aimR{XR_NULL_HANDLE};
    XrSpace spaceL{XR_NULL_HANDLE};
    XrSpace spaceR{XR_NULL_HANDLE};
    /**
     * Grip (palm) poses, bound to `/input/grip/pose`. Named `palm_*` rather than `grip_*` because
     * `grip_l`/`grip_r` are already the squeeze-value actions and the mock keys its input on the
     * action name; the trailing `_l`/`_r` is what tells it which hand a space belongs to, so that
     * part is as load bearing here as it is for the aim poses.
     */
    XrAction palmL{XR_NULL_HANDLE};
    XrAction palmR{XR_NULL_HANDLE};
    XrSpace palmSpaceL{XR_NULL_HANDLE};
    XrSpace palmSpaceR{XR_NULL_HANDLE};
    /** Vibration outputs, one per hand; null when the runtime refused them. */
    XrAction hapticL{XR_NULL_HANDLE};
    XrAction hapticR{XR_NULL_HANDLE};
    bool attached{};
};

/** Full-screen triangle; the pixel shader decodes sRGB when the target encodes it again. */
constexpr const char* kBlitVertexShader = R"(
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    Output output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

constexpr const char* kBlitPixelShaderDecode = R"(
Texture2D source : register(t0);
SamplerState linearSampler : register(s0);
float3 decode(float3 c) {
    float3 low = c / 12.92;
    float3 high = pow((c + 0.055) / 1.055, 2.4);
    return lerp(low, high, step(0.04045, c));
}
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = source.Sample(linearSampler, uv);
    return float4(decode(saturate(c.rgb)), 1.0);
}
)";

constexpr const char* kBlitPixelShaderCopy = R"(
Texture2D source : register(t0);
SamplerState linearSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(source.Sample(linearSampler, uv).rgb, 1.0);
}
)";

HMODULE g_runtimeModule{};
Api g_api{};
XrInstance g_instance{XR_NULL_HANDLE};
XrSystemId g_system{XR_NULL_SYSTEM_ID};
XrSession g_session{XR_NULL_HANDLE};
XrSpace g_space{XR_NULL_HANDLE};
/** The head's own space: the fade that brings the world back after a screen is drawn in it. */
XrSpace g_viewSpace{XR_NULL_HANDLE};
std::atomic<XrSessionState> g_sessionState{XR_SESSION_STATE_UNKNOWN};
/** Read on every thread; written on the frame thread (begin/end session) or the present thread (teardown). */
std::atomic_bool g_sessionRunning{false};
bool g_frameOpen{false};
std::atomic<Status> g_status{Status::off};

/**
 * Bringing the runtime up happens on a thread of its own: Meta's xrCreateInstance took 6.1 s over
 * Air Link in the sister project, and a missing headset has to be retried without stalling the game.
 * The present thread starts it, polls it, and picks the result up (initialize()).
 */
enum class InitPhase : int { idle, working, done };
std::atomic<int> g_initPhase{static_cast<int>(InitPhase::idle)};
std::atomic_bool g_initOk{false};
HANDLE g_initThread{};
ID3D11Device* g_initDevice{};
/** When the next attempt may start (GetTickCount64); 0 = at once. */
ULONGLONG g_nextAttemptMs{0};
std::uint32_t g_attempts{0};
/**
 * Whether the frame thread may feed the headset itself while the game is quiet. Its frames call
 * xrBeginFrame/xrEndFrame on another thread than the game's, with no image acquired or released --
 * Meta's runtime does its D3D11 work at release, on the game's thread, so that is safe there (and on
 * the mock); SteamVR uses the game's immediate context inside xrEndFrame (dishonored-vr), and this
 * game cannot be given D3D11's multithread protection: Destiny keeps its own copies of the context's
 * function pointers, so switched on early the protection blinds the GPU hooks (30 draws a period
 * instead of 40 000), switched on late it protects nothing. `keepalive on|off|auto` in SVR_Settings.txt.
 */
bool g_keepAliveAllowed{false};

/**
 * Raised where the runtime takes the session or the instance away (LOSS_PENDING, INSTANCE_LOSS_PENDING,
 * or a stopped session not offered back): the present thread tears everything down and starts over.
 */
std::atomic_bool g_restartRequested{false};
/** EXITING: the runtime asked the game to leave VR. It stays flat until F9 is switched off and on. */
std::atomic_bool g_exitHold{false};
/** The runtime moved LOCAL (the player held the Meta button): our origin follows, in the room and in yaw. */
std::atomic_bool g_runtimeRecentre{false};
std::atomic<std::uint32_t> g_runtimeRecentres{0};
/** Which runtime call the present thread is in, for the frame thread's report of a quiet game. */
std::atomic<const char*> g_presentCall{nullptr};

[[nodiscard]] double now_seconds() noexcept {
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

/** The keep-alive's copy of the last frame's layers: the swapchains they name keep their last released image. */
constexpr std::size_t kMaxQuads = 4;
struct SavedLayers final {
    bool haveProjection{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, kViewCount> views{};
    std::array<XrCompositionLayerQuad, kMaxQuads> quads{};
    std::size_t quadCount{};
};

/**
 * The OpenXR frame thread (dishonored-vr HANDOFF part 51, measured there on a Quest 3 over Air Link:
 * Meta held xrWaitFrame for 33 s on the render thread and froze the PC). xrWaitFrame, the event
 * queue and begin/end session live here; the present thread asks for a frame and waits for it with
 * a bound, so the game never waits on the runtime longer than that. When the game stops presenting
 * (a load, a hitch, a hang of its own) the thread keeps the headset fed with the last frame's
 * layers, held where they were in the room, instead of leaving it on an hourglass.
 */
struct Pump final {
    SRWLOCK lock{SRWLOCK_INIT};
    CONDITION_VARIABLE changed{CONDITION_VARIABLE_INIT};
    HANDLE thread{};
    bool quit{};
    bool request{};
    bool waited{};
    bool waiting{};
    bool mainOpen{};
    bool keepAliveOpen{};
    bool ending{};
    bool endPending{};
    bool lastTimedOut{};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    double waitedAt{};
    double lastMainBegin{};
    double lastMainEnd{};
    double lastKeepAlive{};
    double period{1.0 / 72.0};
    double timedOutSince{};
    SavedLayers layers{};
    bool inRun{};
    double runStart{};
    std::uint64_t runFrames{};
    std::uint64_t keepAliveRuns{};
    std::uint64_t keepAlives{};
    std::uint64_t waits{};
    std::uint64_t mainTimeouts{};
    std::uint64_t waitFails{};
    double longestWait{};
    double longestRun{};
    bool wasRunning{};
    double idleSince{};
    double nextKick{};
    std::uint64_t kicks{};
    double lastQuietReport{};
};
Pump g_pump{};
/** The present thread waits at most this long for the runtime to release a frame (2 ms once it has timed out). */
constexpr DWORD kWaitBoundMs = 50;
/** The game quiet this long and the frame thread feeds the headset itself. */
constexpr double kQuietSeconds = 0.20;
/** A running session stopped and not offered back this long: a new session (Meta only, measured there). */
constexpr double kKickSeconds = 5.0;
XrTime g_predictedDisplayTime{};
XrDuration g_displayPeriod{};
/**
 * The instant the head and hands are located for: the frame's predicted display time plus the LEAD,
 * the measured time between that prediction and the display time the image drawn from them is really
 * submitted for. The engine draws an image a frame or so after the camera hook takes its pose (the
 * ledger names which record each image was drawn from), so a pose predicted for "this frame" is
 * shown a frame late. A runtime that honours the declared pose reprojects that away; one that takes
 * every image as drawn for its display time (Meta's PC runtime is reported to ignore declared poses)
 * shows it as swim -- about a degree at 70 deg/s. Predicting for the instant the image will really be
 * shown is right for both: the declaration stays truthful, and there is nothing left to reproject.
 */
XrTime g_locateTime{};
std::atomic<std::int64_t> g_leadNs{0};
/** -1: measured (the default); otherwise a fixed lead in nanoseconds (`lead <ms>`, `lead off` = 0). */
std::atomic<std::int64_t> g_leadFixedNs{-1};
/** The last measured shortfall (submitted display time minus the time the image was drawn for). */
std::atomic<std::int64_t> g_leadErrorNs{0};
std::array<char, 64> g_runtimeName{};

/** Units the game moves for one metre of head travel. One is the starting guess. */
float g_unitsPerMetre{1.0F};

/** Head position taken as the origin, in OpenXR's own basis and metres. */
XrVector3f g_origin{};
/**
 * Head yaw at the last recentre, in the game's basis.
 *
 * Recentring used to take only the position, which left an arbitrary constant offset between where
 * the player looked and where the character faced -- whatever direction they happened to be sitting
 * in relative to the room's own forward axis. Invisible in the mock, because the mock's zero yaw is
 * the identity orientation, and a standing bug on a real headset.
 */
float g_originYaw{};
bool g_haveOrigin{false};
bool g_recentreRequested{true};

HeadPose g_pose{};
/** Aim poses of the two controllers, left at index 0, right at index 1. */
std::array<HandPose, 2> g_hands{};
InputState g_input{};
SRWLOCK g_poseLock{SRWLOCK_INIT};
std::uint64_t g_frameIndex{};

/** Index 0 is the mono image and the left eye's in stereo; index 1 the right eye's. */
std::array<Presentation, kViewCount> g_presentations{};
Actions g_actions{};

/** Views located this frame: the pose the NEXT rendered frame will be drawn from. */
std::array<XrView, kViewCount> g_pendingViews{};
bool g_havePending{false};
/** Views the frame being presented was drawn from, which is what the layer must declare. */
std::array<XrView, kViewCount> g_renderedViews{};
bool g_haveRendered{false};
bool g_shouldRender{false};
/** Horizontal FOV the camera hook left in the engine, read back from the pose block. */
std::atomic<float> g_renderedFov{0.0F};
/** Aspect the camera hook left in the engine, read back the same way. */
std::atomic<float> g_renderedAspect{0.0F};
/** The vertical half angle the layer last declared, for the log. */
std::atomic<float> g_declaredHalfVertical{0.0F};
/** The tangents of the last image submitted: what a snapshot of it spans (the screen's backdrop). */
float g_submittedTanX = 0.0F;
float g_submittedTanY = 0.0F;
/** The eye the image is rendered from (set_render_eye), and the one the views in flight used. */
std::atomic<int> g_renderEye{0};
int g_pendingEye{0};
int g_renderedEye{0};
/** Publication serials (HeadPose::frame) of the pending and the rendered views, for the ledger. */
std::uint64_t g_pendingSerial{0};
std::uint64_t g_renderedSerial{0};

/** The eyes' geometry for eye_geometry(): written on the present thread, read from any. */
SRWLOCK g_geometryLock{SRWLOCK_INIT};
EyeGeometry g_geometry{};

/**
 * The frame ledger (docs/DISPLAY.md). One record per camera write: which publication the camera
 * used, which eye it rendered from, the whole sample (head with that eye's offset, both hands, the
 * views) and the camera it wrote. Every consumer that turns the pose into pixels takes its sample
 * from here, so the camera, the gun, the arms and the layer can never disagree about a frame:
 *
 *  - the camera hook appends the record (camera_frame + commit_camera);
 *  - the weapon, later in the same update on the camera thread, reads the latched one;
 *  - the GPU names the record a frame was really drawn with, by the camera its world view carries
 *    (note_world_camera); the arms' rewrite and the layer read THAT one.
 *
 * Positions are kept as written, so the match is exact rather than nearest: the engine copies the
 * block's camera into the view constants.
 */
struct Publication final {
    std::uint64_t serial{};
    /** The instant the views and hands were located for (g_locateTime). */
    XrTime located{};
    std::array<XrView, kViewCount> views{};
    HeadPose head{};
    std::array<HandPose, 2> hands{};
    bool valid{};
};
struct DrawRecord final {
    std::uint64_t serial{};
    /** Which eye the camera rendered from: 0 the midpoint, 1 left, 2 right. */
    int eye{};
    /** True for a frame of alternating-eye stereo, false for a mono one. */
    bool stereo{};
    /** The instant its poses were located for. */
    XrTime located{};
    std::array<XrView, kViewCount> views{};
    HeadPose head{};
    std::array<HandPose, 2> hands{};
    std::array<float, 3> cameraPosition{};
    std::array<float, 3> cameraForward{};
    std::uint64_t sequence{};
    bool committed{};
};
constexpr std::size_t kPublicationRing = 8;
constexpr std::size_t kDrawRing = 16;
SRWLOCK g_ledgerLock{SRWLOCK_INIT};
std::array<Publication, kPublicationRing> g_publications{};
std::array<DrawRecord, kDrawRing> g_draws{};
std::uint64_t g_drawSequence{0};
/** The record the camera hook latched for the frame it is building (camera thread). */
DrawRecord g_cameraDraw{};
bool g_haveCameraDraw{false};
/** The record the GPU last saw drawn (render thread writes, present thread reads). */
DrawRecord g_drawn{};
bool g_haveDrawn{false};
std::uint64_t g_drawnMatches{0};
std::uint64_t g_drawnMisses{0};
/** The eye the last stereo camera frame used, so the next one takes the other. */
int g_lastStereoEye{2};
/** 0 mono, 1 alternating eyes. */
std::atomic<int> g_stereoMode{0};
/** Presented frames whose eye outside the UI canvas was filled from the scene (present thread). */
std::uint64_t g_filledFrames{0};

/** Emits one preformatted line on the client channel. */
void log_line(const char* text) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, text);
}

/** Emits one formatted line, truncated to the log's line capacity. */
#pragma warning(push)
// The format reaches snprintf as a parameter rather than a literal, which is the point of the
// helper; every caller in this file passes a literal.
#pragma warning(disable : 4774)
template <typename... Args> void log_fmt(const char* format, Args... args) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(), line.size(), format, args...);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}
#pragma warning(pop)

/**
 * Reads the path of the active runtime's manifest.
 * XR_RUNTIME_JSON wins when it is set, which is how the mock runtime is selected for a test.
 * @param output Receives the path.
 * @return True when a path was found.
 */
/**
 * Which runtime: `runtime` in SVR_Settings.txt (vr_settings.h). `auto`, the default: XR_RUNTIME_JSON
 * when set (the mock, a launcher), else Meta's runtime when it is installed -- the Quest over Link,
 * which on this PC is NOT the system's active runtime (SteamVR is), and a player should not have to
 * know that -- else the system's active runtime. `meta`, `steamvr`, `system` or a manifest's path force one.
 */
[[nodiscard]] bool manifest_path(std::wstring& output) noexcept {
    std::array<wchar_t, MAX_PATH> buffer{};
    const char* choice = settings::value("runtime", "auto");
    const bool automatic = std::strcmp(choice, "auto") == 0;
    const DWORD length =
        GetEnvironmentVariableW(L"XR_RUNTIME_JSON", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (automatic && length > 0 && length < buffer.size()) {
        output.assign(buffer.data(), length);
        log_fmt("ev=vr.xr runtime_choice=env path=%ls", output.c_str());
        return true;
    }
    const auto exists = [](const wchar_t* path) noexcept { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; };
    if (automatic || std::strcmp(choice, "meta") == 0) {
        for (const wchar_t* meta : {L"C:\\Program Files\\Meta Horizon\\Support\\oculus-runtime\\oculus_openxr_64.json",
                                    L"C:\\Program Files\\Oculus\\Support\\oculus-runtime\\oculus_openxr_64.json"}) {
            if (exists(meta)) {
                output = meta;
                log_fmt("ev=vr.xr runtime_choice=meta path=%ls", meta);
                return true;
            }
        }
    }
    if (std::strcmp(choice, "steamvr") == 0) {
        for (const wchar_t* steam : {L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win64.json"}) {
            if (exists(steam)) {
                output = steam;
                log_fmt("ev=vr.xr runtime_choice=steamvr path=%ls", steam);
                return true;
            }
        }
    }
    if (!automatic && std::strcmp(choice, "meta") != 0 && std::strcmp(choice, "steamvr") != 0
        && std::strcmp(choice, "system") != 0) {
        const int wide = MultiByteToWideChar(CP_UTF8, 0, choice, -1, buffer.data(), static_cast<int>(buffer.size()));
        if (wide > 1 && exists(buffer.data())) {
            output.assign(buffer.data());
            log_fmt("ev=vr.xr runtime_choice=path path=%ls", output.c_str());
            return true;
        }
        log_fmt("ev=vr.xr runtime_choice=invalid value=%s fallback=system", choice);
    }
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &key)
        != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LSTATUS status = RegQueryValueExW(
        key, L"ActiveRuntime", nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }
    output.assign(buffer.data());
    return !output.empty();
}

/**
 * Pulls `runtime.library_path` out of a manifest.
 * A full JSON parser is not warranted for one string: the key is unique in the file and its value
 * is a plain string whose only escape is the doubled backslash of a Windows path.
 * @param text Whole manifest.
 * @param output Receives the unescaped path.
 * @return True when the key was present and its value non-empty.
 */
[[nodiscard]] bool library_path_from(const std::string& text, std::string& output) noexcept {
    const std::size_t key = text.find("\"library_path\"");
    if (key == std::string::npos) {
        return false;
    }
    const std::size_t colon = text.find(':', key);
    if (colon == std::string::npos) {
        return false;
    }
    const std::size_t open = text.find('"', colon);
    if (open == std::string::npos) {
        return false;
    }
    output.clear();
    for (std::size_t index = open + 1; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '"') {
            return !output.empty();
        }
        if (character == '\\' && index + 1 < text.size()) {
            ++index;
            output.push_back(text[index]);
            continue;
        }
        output.push_back(character);
    }
    return false;
}

/** @return The whole contents of a file, or an empty string. */
[[nodiscard]] std::string read_file(const std::wstring& path) noexcept {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string text;
    std::array<char, 4096> chunk{};
    DWORD read = 0;
    while (ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)
           && read > 0) {
        text.append(chunk.data(), read);
    }
    CloseHandle(file);
    return text;
}

/** @return A UTF-8 string as UTF-16, or empty when it does not convert. */
[[nodiscard]] std::wstring widen_utf8(const std::string& text) noexcept {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

/**
 * Reads the first bytes of an export as they are in the module's file on disk.
 * The game hot-patches loader exports in memory; the image in System32 is the untouched original,
 * and the first bytes of an x64 function carry no absolute relocations, so file bytes equal the
 * bytes the loader mapped.
 * @param moduleName Base name of a System32 module.
 * @param exportRva Address of the export relative to the module base.
 * @param output Receives the bytes.
 * @return True when the file was parsed and the bytes read.
 */
[[nodiscard]] bool export_bytes_on_disk(const wchar_t* moduleName,
                                        std::uint32_t exportRva,
                                        std::span<std::byte> output) noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    const UINT systemLength = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (systemLength == 0 || systemLength >= path.size()) {
        return false;
    }
    std::wstring full(path.data(), systemLength);
    full += L'\\';
    full += moduleName;
    const std::string image = read_file(full);
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size()) {
        return false;
    }
    const auto* headers =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(headers);
    for (WORD index = 0; index < headers->FileHeader.NumberOfSections; ++index, ++section) {
        const std::uint32_t begin = section->VirtualAddress;
        const std::uint32_t end = begin + (std::max)(section->Misc.VirtualSize, section->SizeOfRawData);
        if (exportRva < begin || exportRva >= end) {
            continue;
        }
        const std::size_t offset = section->PointerToRawData + (exportRva - begin);
        if (offset + output.size() > image.size()) {
            return false;
        }
        std::memcpy(output.data(), image.data() + offset, output.size());
        return true;
    }
    return false;
}

/**
 * Puts the on-disk prologue back over one export the game has hot-patched in memory.
 * The game detours kernelbase!LoadLibraryExW and kernel32!GetProcAddress and refuses calls from
 * modules it does not know. Every CRT's DllMain calls both, so no foreign runtime can finish
 * initialising while the patches stand: that is the ERROR_DLL_INIT_FAILED every load ended in.
 * @return True when the export is now the original code, whether or not it had to be restored.
 */
[[nodiscard]] bool restore_export(const wchar_t* moduleName, const char* exportName) noexcept {
    const HMODULE module = GetModuleHandleW(moduleName);
    if (module == nullptr) {
        return false;
    }
    auto* const live = reinterpret_cast<std::byte*>(GetProcAddress(module, exportName));
    if (live == nullptr) {
        return false;
    }
    constexpr std::size_t kPrologue = 16;
    std::array<std::byte, kPrologue> original{};
    const auto rva = static_cast<std::uint32_t>(live - reinterpret_cast<std::byte*>(module));
    if (!export_bytes_on_disk(moduleName, rva, original)) {
        log_fmt("ev=vr.xr unhook module=%ls export=%s result=fail reason=disk", moduleName, exportName);
        return false;
    }
    std::size_t differing = 0;
    for (std::size_t index = 0; index < kPrologue; ++index) {
        if (live[index] != original[index]) {
            differing = index + 1;
        }
    }
    if (differing == 0) {
        return true;
    }
    DWORD previous = 0;
    if (VirtualProtect(live, differing, PAGE_EXECUTE_READWRITE, &previous) == FALSE) {
        log_fmt("ev=vr.xr unhook module=%ls export=%s result=fail reason=protect", moduleName, exportName);
        return false;
    }
    std::memcpy(live, original.data(), differing);
    DWORD ignored = 0;
    VirtualProtect(live, differing, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), live, differing);
    log_fmt("ev=vr.xr unhook module=%ls export=%s result=ok bytes=%zu", moduleName, exportName, differing);
    return true;
}

/**
 * Loads a module through ntdll's loader entry, bypassing kernelbase!LoadLibraryExW.
 * @param path Full path of the module.
 * @param status Receives the NTSTATUS the loader answered with.
 * @return The module, or null.
 */
[[nodiscard]] HMODULE load_via_ldr(const std::wstring& path, LONG& status) noexcept {
    using LdrLoadDllFn = LONG(NTAPI*)(PWSTR, PULONG, PUNICODE_STRING, PVOID*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto ldrLoadDll = ntdll != nullptr
                                ? reinterpret_cast<LdrLoadDllFn>(GetProcAddress(ntdll, "LdrLoadDll"))
                                : nullptr;
    if (ldrLoadDll == nullptr) {
        status = -1;
        return nullptr;
    }
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(path.c_str());
    name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    name.MaximumLength = static_cast<USHORT>(name.Length + sizeof(wchar_t));
    PVOID module = nullptr;
    status = ldrLoadDll(nullptr, nullptr, &name, &module);
    return status >= 0 ? static_cast<HMODULE>(module) : nullptr;
}

/** First exception raised while a watched load ran, for the diagnosis of a refused DllMain. */
struct WatchedException final {
    bool hit{};
    DWORD code{};
    void* address{};
};
WatchedException g_watched{};

LONG CALLBACK watch_exceptions(EXCEPTION_POINTERS* pointers) noexcept {
    if (!g_watched.hit && pointers != nullptr && pointers->ExceptionRecord != nullptr) {
        g_watched.hit = true;
        g_watched.code = pointers->ExceptionRecord->ExceptionCode;
        g_watched.address = pointers->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/** Names the module an address falls in, with its offset, or the raw address. */
void describe_address(void* address, char* output, std::size_t size) noexcept {
    HMODULE module = nullptr;
    if (address != nullptr
        && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address),
                              &module)
        && module != nullptr) {
        std::array<wchar_t, MAX_PATH> path{};
        GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        const wchar_t* base = path.data();
        for (const wchar_t* cursor = path.data(); *cursor != L'\0'; ++cursor) {
            if (*cursor == L'\\' || *cursor == L'/') {
                base = cursor + 1;
            }
        }
        std::snprintf(output,
                      size,
                      "%ls+0x%llX",
                      base,
                      static_cast<unsigned long long>(reinterpret_cast<std::byte*>(address)
                                                      - reinterpret_cast<std::byte*>(module)));
        return;
    }
    std::snprintf(output, size, "%p", address);
}

/**
 * LoadLibraryW with a vectored handler around it, so an exception thrown inside the DLL's
 * initialisation is caught in the log before the loader swallows it into STATUS_DLL_INIT_FAILED.
 */
[[nodiscard]] HMODULE load_watched(const wchar_t* path, const char* label) noexcept {
    g_watched = WatchedException{};
    void* const handler = AddVectoredExceptionHandler(1, &watch_exceptions);
    const HMODULE module = LoadLibraryW(path);
    const DWORD error = GetLastError();
    if (handler != nullptr) {
        RemoveVectoredExceptionHandler(handler);
    }
    if (g_watched.hit) {
        std::array<char, 128> where{};
        describe_address(g_watched.address, where.data(), where.size());
        log_fmt("ev=vr.xr load exception label=%s code=0x%08lX at=%s",
                label,
                static_cast<unsigned long>(g_watched.code),
                where.data());
    }
    SetLastError(error);
    return module;
}

/**
 * One node of ntdll's DLL notification list. Undocumented, unchanged since Vista: the cookie
 * LdrRegisterDllNotification hands back is a pointer to the caller's own node.
 */
struct LdrDllNotificationEntry final {
    LIST_ENTRY links;
    void* callback;
    void* context;
};

using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG, void*, void*, void**);
using LdrUnregisterDllNotificationFn = LONG(NTAPI*)(void*);
using LdrLockLoaderLockFn = LONG(NTAPI*)(ULONG, ULONG*, ULONG_PTR*);
using LdrUnlockLoaderLockFn = LONG(NTAPI*)(ULONG, ULONG_PTR);

/** What the loader hands a notification callback (LDR_DLL_NOTIFICATION_DATA; load and unload share it). */
struct LdrNotificationData final {
    ULONG flags;
    const UNICODE_STRING* fullName;
    const UNICODE_STRING* baseName;
    void* base;
    ULONG size;
};
using LdrNotificationFn = void(CALLBACK*)(ULONG, const LdrNotificationData*, void*);

/**
 * The game's callback is stood in front of, and by default still silenced: `noop` is what every
 * headset session so far ran with. `filter` (`ldr filter` in SVR_Debug.txt) hands the game every
 * module but the runtime's own -- tried on 2026-10-17 as the suspect for the destination-load hang,
 * and cleared (the hang is one of our engine detours, docs/PLAYABLE.md); it stays an option because
 * it cannot be tested on the mock whether Meta's runtime maps system modules from its own threads
 * that the game would then refuse. Either way every module mapped is logged (`ev=vr.xr dll`).
 */
enum class LoaderMode : int { filter, noop };
std::atomic<int> g_loaderMode{static_cast<int>(LoaderMode::noop)};
std::atomic<LdrNotificationFn> g_gameNotification{nullptr};
/** Loads on this thread while it is inside our own runtime calls: the runtime's, whatever they pull in. */
thread_local int t_ownLoads = 0;
/** The runtime library's directory, lower case: anything mapped from under it is the runtime's. */
std::array<wchar_t, MAX_PATH> g_runtimeDirectory{};
std::size_t g_runtimeDirectoryLength = 0;

/** What the filter saw, for the log; written under the loader lock, read on the present thread. */
struct LoaderRecord final {
    ULONG reason{};
    bool ours{};
    bool forwarded{};
    std::array<char, 120> name{};
};
constexpr std::uint32_t kLoaderRing = 64;
std::array<LoaderRecord, kLoaderRing> g_loaderRing{};
std::atomic<std::uint32_t> g_loaderWritten{0};
std::uint32_t g_loaderRead = 0;

struct OwnLoads final {
    OwnLoads() noexcept { ++t_ownLoads; }
    ~OwnLoads() { --t_ownLoads; }
    OwnLoads(const OwnLoads&) = delete;
    OwnLoads& operator=(const OwnLoads&) = delete;
};

[[nodiscard]] bool under_runtime_directory(const UNICODE_STRING* path) noexcept {
    if (path == nullptr || path->Buffer == nullptr || g_runtimeDirectoryLength == 0) {
        return false;
    }
    const std::size_t length = path->Length / sizeof(wchar_t);
    if (length < g_runtimeDirectoryLength) {
        return false;
    }
    for (std::size_t index = 0; index < g_runtimeDirectoryLength; ++index) {
        wchar_t c = path->Buffer[index];
        c = c == L'/' ? L'\\' : (c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
        if (c != g_runtimeDirectory[index]) {
            return false;
        }
    }
    return true;
}

/** Stands in front of the game's notification callback. Runs under the loader lock: no allocation, no loads. */
void CALLBACK filter_notification(ULONG reason, const LdrNotificationData* data, void* context) noexcept {
    const bool ours = t_ownLoads > 0 || (data != nullptr && under_runtime_directory(data->fullName));
    const LdrNotificationFn game = g_gameNotification.load(std::memory_order_acquire);
    const bool forward = !ours && game != nullptr
                         && g_loaderMode.load(std::memory_order_relaxed) == static_cast<int>(LoaderMode::filter);
    const std::uint32_t slot = g_loaderWritten.load(std::memory_order_relaxed);
    LoaderRecord& record = g_loaderRing[slot % kLoaderRing];
    record.reason = reason;
    record.ours = ours;
    record.forwarded = forward;
    std::size_t used = 0;
    const UNICODE_STRING* name = data != nullptr ? data->fullName : nullptr;
    if (name != nullptr && name->Buffer != nullptr) {
        const std::size_t length = name->Length / sizeof(wchar_t);
        // The tail of the path names the module; the head is mostly the same few directories.
        const std::size_t first = length > record.name.size() - 1 ? length - (record.name.size() - 1) : 0;
        for (std::size_t index = first; index < length; ++index) {
            const wchar_t c = name->Buffer[index];
            record.name[used++] = c > 32 && c < 127 ? static_cast<char>(c) : '_';
        }
    }
    record.name[used] = '\0';
    g_loaderWritten.store(slot + 1, std::memory_order_release);
    if (forward) {
        game(reason, data, context);
    }
}

/** Registered only to find the list; never expected to run. */
void CALLBACK placeholder_notification(ULONG, const void*, void*) noexcept {}

/** Reads `ldr noop|filter` from SVR_Debug.txt beside the game, once per runtime load. */
void read_loader_mode() noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    wchar_t* const slash = length != 0 && length < path.size() ? std::wcsrchr(path.data(), L'\\') : nullptr;
    if (slash == nullptr) {
        return;
    }
    const std::size_t dirLength = static_cast<std::size_t>(slash + 1 - path.data());
    if (wcscpy_s(slash + 1, path.size() - dirLength, L"SVR_Debug.txt") != 0) {
        return;
    }
    const std::string text = read_file(path.data());
    const std::size_t at = text.find("ldr ");
    if (at == std::string::npos) {
        return;
    }
    const bool noop = text.compare(at + 4, 6, "filter") != 0;
    g_loaderMode.store(static_cast<int>(noop ? LoaderMode::noop : LoaderMode::filter), std::memory_order_relaxed);
    log_fmt("ev=vr.xr ldr_mode=%s", noop ? "noop" : "filter");
}

/**
 * Remembers what the filter treats as the runtime's: its library's directory, lower case with
 * backslashes -- or, when that directory also holds the game (the mock is installed beside it), the
 * library's own path alone. The game maps modules of its own from there in the middle of a load
 * (dbghelp.dll), and a directory match took those from it.
 */
void note_runtime_directory(const std::wstring& library) noexcept {
    const auto lower = [](wchar_t c) noexcept {
        return c == L'/' ? L'\\' : (c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
    };
    g_runtimeDirectoryLength = 0;
    const std::size_t slash = library.find_last_of(L"\\/");
    std::size_t length = slash == std::wstring::npos ? 0 : slash + 1;
    std::array<wchar_t, MAX_PATH> exe{};
    const DWORD exeLength = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    const wchar_t* const exeSlash = exeLength != 0 ? std::wcsrchr(exe.data(), L'\\') : nullptr;
    const std::size_t exeDir = exeSlash != nullptr ? static_cast<std::size_t>(exeSlash + 1 - exe.data()) : 0;
    bool shared = exeDir != 0 && exeDir == length;
    for (std::size_t index = 0; shared && index < length; ++index) {
        shared = lower(exe[index]) == lower(library[index]);
    }
    if (shared) {
        length = library.size();
    }
    if (length == 0 || length >= g_runtimeDirectory.size()) {
        return;
    }
    for (std::size_t index = 0; index < length; ++index) {
        g_runtimeDirectory[index] = lower(library[index]);
    }
    g_runtimeDirectoryLength = length;
    log_fmt("ev=vr.xr ldr_filter runtime_match=%s", shared ? "file" : "directory");
}

/** @return True when an address lies inside a module's mapped image. */
[[nodiscard]] bool inside_module(const void* address, HMODULE module) noexcept {
    HMODULE owner = nullptr;
    return address != nullptr
           && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                     | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 static_cast<LPCWSTR>(address),
                                 &owner)
           && owner == module;
}

/**
 * Finds every DLL-load notification callback the process has registered and neutralises the
 * ones that are not Windows' own.
 *
 * The game registers one. The loader runs it for every new module before that module's DllMain,
 * and afterwards the DllMain never runs and the load ends in STATUS_DLL_INIT_FAILED -- for a
 * bare no-CRT DLL just as for a runtime -- while mapping the same file without initialisation
 * succeeds and no exception is raised. Nothing else in the process is in a position to do that:
 * ntdll's own code is byte-identical to the file. The callback is left in the list with its
 * function pointer pointing at a no-op, so the list stays consistent for the loader.
 * @return True when the list was walked.
 */
[[nodiscard]] bool neutralise_dll_notifications() noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&neutralise_dll_notifications),
                       &self);
    if (ntdll == nullptr || self == nullptr) {
        return false;
    }
    const auto registerFn = reinterpret_cast<LdrRegisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    const auto unregisterFn = reinterpret_cast<LdrUnregisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
    const auto lockFn =
        reinterpret_cast<LdrLockLoaderLockFn>(GetProcAddress(ntdll, "LdrLockLoaderLock"));
    const auto unlockFn =
        reinterpret_cast<LdrUnlockLoaderLockFn>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"));
    if (registerFn == nullptr || unregisterFn == nullptr || lockFn == nullptr
        || unlockFn == nullptr) {
        log_line("ev=vr.xr ldr_notify result=fail reason=exports");
        return false;
    }
    void* cookie = nullptr;
    if (registerFn(0, reinterpret_cast<void*>(&placeholder_notification), nullptr, &cookie) != 0
        || cookie == nullptr) {
        log_line("ev=vr.xr ldr_notify result=fail reason=register");
        return false;
    }
    ULONG lockState = 0;
    ULONG_PTR lockCookie = 0;
    const bool locked = lockFn(0, &lockState, &lockCookie) == 0;
    auto* const ours = static_cast<LdrDllNotificationEntry*>(cookie);
    std::size_t seen = 0;
    std::size_t neutralised = 0;
    for (LIST_ENTRY* entry = ours->links.Flink; entry != nullptr && entry != &ours->links;
         entry = entry->Flink) {
        // The list head is a global inside ntdll's image; every real node is heap.
        if (inside_module(entry, ntdll)) {
            continue;
        }
        auto* const node = reinterpret_cast<LdrDllNotificationEntry*>(entry);
        ++seen;
        std::array<char, 128> where{};
        describe_address(node->callback, where.data(), where.size());
        const bool foreign = !inside_module(node->callback, ntdll) && !inside_module(node->callback, self);
        log_fmt("ev=vr.xr ldr_notify callback=%s foreign=%d", where.data(), foreign ? 1 : 0);
        if (foreign) {
            // One foreign callback is known (the game's); a second would be forwarded to nobody.
            if (g_gameNotification.load(std::memory_order_relaxed) == nullptr) {
                g_gameNotification.store(reinterpret_cast<LdrNotificationFn>(node->callback), std::memory_order_release);
            }
            node->callback = reinterpret_cast<void*>(&filter_notification);
            ++neutralised;
        }
    }
    if (locked) {
        unlockFn(0, lockCookie);
    }
    unregisterFn(cookie);
    log_fmt("ev=vr.xr ldr_notify result=ok seen=%zu neutralised=%zu", seen, neutralised);
    return true;
}

/**
 * Runs one experiment per hypothesis after a refused load and logs each answer.
 * Mapping without initialisation separates the loader from DllMain; the two probe DLLs beside the
 * game separate the CRT from a bare entry point, and their marker files tell a DllMain that ran
 * from one the game skipped; FLS and TLS allocation rule out slot exhaustion.
 */
void diagnose_load_failure(const std::wstring& runtimePath) noexcept {
    const HMODULE mapped =
        LoadLibraryExW(runtimePath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    log_fmt("ev=vr.xr diag map_only=%d error=%lu",
            mapped != nullptr ? 1 : 0,
            mapped != nullptr ? 0UL : GetLastError());
    if (mapped != nullptr) {
        FreeLibrary(mapped);
    }
    const DWORD fls = FlsAlloc(nullptr);
    const DWORD tls = TlsAlloc();
    log_fmt("ev=vr.xr diag fls=%s tls=%s",
            fls != FLS_OUT_OF_INDEXES ? "ok" : "exhausted",
            tls != TLS_OUT_OF_INDEXES ? "ok" : "exhausted");
    if (fls != FLS_OUT_OF_INDEXES) {
        FlsFree(fls);
    }
    if (tls != TLS_OUT_OF_INDEXES) {
        TlsFree(tls);
    }
    const std::size_t slash = runtimePath.find_last_of(L"\\/");
    const std::wstring directory =
        slash != std::wstring::npos ? runtimePath.substr(0, slash + 1) : std::wstring{};
    constexpr std::array<const wchar_t*, 2> probes{L"SVR_Probe_NoCrt", L"SVR_Probe_Crt"};
    for (const wchar_t* probe : probes) {
        const std::wstring dll = directory + probe + L".dll";
        const std::wstring marker = directory + probe + L".txt";
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        DeleteFileW(marker.c_str());
        const HMODULE module = load_watched(dll.c_str(), "probe");
        const DWORD error = GetLastError();
        const bool entered = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
        log_fmt("ev=vr.xr diag probe=%ls loaded=%d error=%lu entered=%d",
                probe,
                module != nullptr ? 1 : 0,
                module != nullptr ? 0UL : error,
                entered ? 1 : 0);
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }
}

/**
 * Loads the active runtime and negotiates the loader interface with it.
 * @return True when the runtime handed back a usable proc address.
 */
[[nodiscard]] bool load_runtime() noexcept {
    std::wstring manifest;
    if (!manifest_path(manifest)) {
        log_line("ev=vr.xr load result=fail reason=no_active_runtime");
        g_status = Status::noRuntime;
        return false;
    }
    const std::string text = read_file(manifest);
    std::string library;
    if (text.empty() || !library_path_from(text, library)) {
        log_line("ev=vr.xr load result=fail reason=manifest_unreadable");
        g_status = Status::noRuntime;
        return false;
    }
    // A manifest may name the library relative to its own directory.
    std::wstring wide = widen_utf8(library);
    if (wide.find(L':') == std::wstring::npos) {
        const std::size_t slash = manifest.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            wide = manifest.substr(0, slash + 1) + wide;
        }
    }
    log_fmt("ev=vr.xr load path=%ls", wide.c_str());
    read_loader_mode();
    note_runtime_directory(wide);
    // The runtime's own DllMain, and every DLL it pulls in, must get the real loader.
    if (debug::has("exports keep")) {
        log_line("ev=vr.xr unhook skipped=exports reason=debug");
    } else {
        (void)restore_export(L"kernelbase.dll", "LoadLibraryExW");
        (void)restore_export(L"kernel32.dll", "GetProcAddress");
    }
    (void)neutralise_dll_notifications();
    g_runtimeModule = load_watched(wide.c_str(), "runtime");
    if (g_runtimeModule == nullptr) {
        const DWORD error = GetLastError();
        // The game patches kernelbase!LoadLibraryExW (a hot-patch jump into a trampoline of its
        // own) and answers ERROR_DLL_INIT_FAILED for any module it does not know. ntdll's loader
        // entry is untouched, so a foreign runtime has to go in underneath the patched export.
        LONG status = 0;
        g_runtimeModule = load_via_ldr(wide, status);
        if (g_runtimeModule == nullptr) {
            log_fmt("ev=vr.xr load result=fail reason=load_library error=%lu ldr_status=0x%08lX",
                    error,
                    static_cast<unsigned long>(status));
            diagnose_load_failure(wide);
            g_status = Status::noRuntime;
            return false;
        }
        log_fmt("ev=vr.xr load via=LdrLoadDll after_error=%lu", error);
    }
    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(g_runtimeModule, "xrNegotiateLoaderRuntimeInterface"));
    if (negotiate == nullptr) {
        log_line("ev=vr.xr load result=fail reason=no_negotiate_export");
        g_status = Status::negotiateFailed;
        return false;
    }
    XrNegotiateLoaderInfo info{};
    info.structType = kStructLoaderInfo;
    info.structVersion = kLoaderInfoStructVersion;
    info.structSize = sizeof info;
    info.minInterfaceVersion = 1;
    info.maxInterfaceVersion = kLoaderRuntimeVersion;
    // The runtime refuses when its own API version falls outside this range, and it reports its
    // version, not the lowest it supports: Meta's runtime is 1.1.x and turned down a 1.0.255 cap
    // (2026-09-08). Any 1.x is acceptable; the instance is still created as a 1.0 application.
    info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.maxApiVersion = XR_MAKE_VERSION(1, 0xFFFF, 0xFFFFFFFFu);
    XrNegotiateRuntimeRequest request{};
    request.structType = kStructRuntimeRequest;
    request.structVersion = kRuntimeInfoStructVersion;
    request.structSize = sizeof request;
    if (negotiate(&info, &request) != XR_SUCCESS || request.getInstanceProcAddr == nullptr) {
        log_line("ev=vr.xr load result=fail reason=negotiate_refused");
        g_status = Status::negotiateFailed;
        return false;
    }
    g_api.getInstanceProcAddr = request.getInstanceProcAddr;
    log_fmt("ev=vr.xr load result=ok interface=%u runtime_api=%u.%u.%u", request.runtimeInterfaceVersion,
            static_cast<unsigned>(XR_VERSION_MAJOR(request.runtimeApiVersion)),
            static_cast<unsigned>(XR_VERSION_MINOR(request.runtimeApiVersion)),
            static_cast<unsigned>(XR_VERSION_PATCH(request.runtimeApiVersion)));
    return true;
}

/**
 * Resolves one entry point through the runtime's proc address.
 * @return True when the runtime provided it.
 */
template <typename T>
[[nodiscard]] bool bind(T& slot, const char* name, XrInstance instance) noexcept {
    PFN_xrVoidFunction function = nullptr;
    if (g_api.getInstanceProcAddr(instance, name, &function) != XR_SUCCESS
        || function == nullptr) {
        log_fmt("ev=vr.xr bind result=fail name=%s", name);
        return false;
    }
    slot = reinterpret_cast<T>(function);
    return true;
}

/** Resolves the handful of entry points callable before an instance exists. */
[[nodiscard]] bool bind_global() noexcept {
    return bind(g_api.createInstance, "xrCreateInstance", XR_NULL_HANDLE);
}

/** Resolves every instance-level entry point. */
[[nodiscard]] bool bind_instance() noexcept {
    return bind(g_api.destroyInstance, "xrDestroyInstance", g_instance)
           && bind(g_api.getInstanceProperties, "xrGetInstanceProperties", g_instance)
           && bind(g_api.getSystem, "xrGetSystem", g_instance)
           && bind(g_api.enumerateViewConfigurationViews,
                   "xrEnumerateViewConfigurationViews",
                   g_instance)
           && bind(g_api.createSession, "xrCreateSession", g_instance)
           && bind(g_api.destroySession, "xrDestroySession", g_instance)
           && bind(g_api.beginSession, "xrBeginSession", g_instance)
           && bind(g_api.endSession, "xrEndSession", g_instance)
           && bind(g_api.createReferenceSpace, "xrCreateReferenceSpace", g_instance)
           && bind(g_api.destroySpace, "xrDestroySpace", g_instance)
           && bind(g_api.pollEvent, "xrPollEvent", g_instance)
           && bind(g_api.waitFrame, "xrWaitFrame", g_instance)
           && bind(g_api.beginFrame, "xrBeginFrame", g_instance)
           && bind(g_api.endFrame, "xrEndFrame", g_instance)
           && bind(g_api.locateViews, "xrLocateViews", g_instance)
           && bind(g_api.getD3D11Requirements,
                   "xrGetD3D11GraphicsRequirementsKHR",
                   g_instance)
           && bind(g_api.enumerateSwapchainFormats, "xrEnumerateSwapchainFormats", g_instance)
           && bind(g_api.createSwapchain, "xrCreateSwapchain", g_instance)
           && bind(g_api.destroySwapchain, "xrDestroySwapchain", g_instance)
           && bind(g_api.enumerateSwapchainImages, "xrEnumerateSwapchainImages", g_instance)
           && bind(g_api.acquireSwapchainImage, "xrAcquireSwapchainImage", g_instance)
           && bind(g_api.waitSwapchainImage, "xrWaitSwapchainImage", g_instance)
           && bind(g_api.releaseSwapchainImage, "xrReleaseSwapchainImage", g_instance)
           && bind(g_api.stringToPath, "xrStringToPath", g_instance)
           && bind(g_api.createActionSet, "xrCreateActionSet", g_instance)
           && bind(g_api.destroyActionSet, "xrDestroyActionSet", g_instance)
           && bind(g_api.createAction, "xrCreateAction", g_instance)
           && bind(g_api.suggestInteractionProfileBindings,
                   "xrSuggestInteractionProfileBindings",
                   g_instance)
           && bind(g_api.attachSessionActionSets, "xrAttachSessionActionSets", g_instance)
           && bind(g_api.syncActions, "xrSyncActions", g_instance)
           && bind(g_api.getActionStateFloat, "xrGetActionStateFloat", g_instance)
           && bind(g_api.getActionStateVector2f, "xrGetActionStateVector2f", g_instance)
           && bind(g_api.getActionStateBoolean, "xrGetActionStateBoolean", g_instance)
           && bind(g_api.getActionStatePose, "xrGetActionStatePose", g_instance)
           && bind(g_api.createActionSpace, "xrCreateActionSpace", g_instance)
           && bind(g_api.locateSpace, "xrLocateSpace", g_instance);
}

/** Defined below, beside the basis conversion; used by the layer submission above it. */
[[nodiscard]] XrPosef cyclopean(const std::array<XrView, kViewCount>& views) noexcept;
[[nodiscard]] XrPosef render_pose(const std::array<XrView, kViewCount>& views, int eye) noexcept;

/** Releases one COM object and clears the pointer. */
template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

/** @return An OpenXR path atom for a string, or XR_NULL_PATH. */
[[nodiscard]] XrPath path_of(const char* text) noexcept {
    XrPath path = XR_NULL_PATH;
    if (g_api.stringToPath(g_instance, text, &path) != XR_SUCCESS) {
        return XR_NULL_PATH;
    }
    return path;
}

/** Creates one action in the gamepad set. */
[[nodiscard]] bool make_action(XrAction& output, const char* name, XrActionType type) noexcept {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::snprintf(info.actionName, sizeof info.actionName, "%s", name);
    std::snprintf(info.localizedActionName, sizeof info.localizedActionName, "%s", name);
    return g_api.createAction(g_actions.set, &info, &output) == XR_SUCCESS
           && output != XR_NULL_HANDLE;
}

/** Creates the reference-space-relative space of one pose action. */
[[nodiscard]] bool make_action_space(XrAction action, XrSpace& output) noexcept {
    if (action == XR_NULL_HANDLE || g_api.createActionSpace == nullptr) {
        return false;
    }
    XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    info.action = action;
    info.subactionPath = XR_NULL_PATH;
    info.poseInActionSpace.orientation.w = 1.0F;
    return g_api.createActionSpace(g_session, &info, &output) == XR_SUCCESS
           && output != XR_NULL_HANDLE;
}

/** One action bound to one input path of a profile. */
struct Binding final {
    XrAction action{};
    const char* path{};
};

/** Suggests one profile's bindings. A runtime that lacks the profile refuses; that is not fatal. */
void suggest(const char* profile, std::span<const Binding> bindings) noexcept {
    // Room for every binding a profile declares. This was 16 while the Touch profile declared 17,
    // and the 17th -- the menu button -- was dropped without a word on every real runtime.
    std::array<XrActionSuggestedBinding, 32> suggested{};
    std::uint32_t count = 0;
    for (const Binding& binding : bindings) {
        const XrPath path = path_of(binding.path);
        if (binding.action == XR_NULL_HANDLE || path == XR_NULL_PATH || count >= suggested.size()) {
            if (count >= suggested.size()) {
                log_fmt("ev=vr.xr bindings profile=%s dropped=%s", profile, binding.path);
            }
            continue;
        }
        suggested[count++] = XrActionSuggestedBinding{binding.action, path};
    }
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    info.interactionProfile = path_of(profile);
    info.countSuggestedBindings = count;
    info.suggestedBindings = suggested.data();
    const XrResult result = g_api.suggestInteractionProfileBindings(g_instance, &info);
    log_fmt("ev=vr.xr bindings profile=%s count=%u result=%d", profile, count, static_cast<int>(result));
}

/**
 * Creates the action set, binds it for the Touch controllers and attaches it to the session.
 * The action names are what the mock runtime keys its synthetic input on, so they are load
 * bearing for the tests and must not change casually.
 */
[[nodiscard]] bool create_actions() noexcept {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(setInfo.actionSetName, sizeof setInfo.actionSetName, "gamepad");
    std::snprintf(setInfo.localizedActionSetName, sizeof setInfo.localizedActionSetName, "Gamepad");
    if (g_api.createActionSet(g_instance, &setInfo, &g_actions.set) != XR_SUCCESS) {
        log_line("ev=vr.xr actions result=fail reason=set");
        return false;
    }
    const bool created =
        make_action(g_actions.move, "move", XR_ACTION_TYPE_VECTOR2F_INPUT)
        && make_action(g_actions.turn, "turn", XR_ACTION_TYPE_VECTOR2F_INPUT)
        && make_action(g_actions.triggerL, "trigger_l", XR_ACTION_TYPE_FLOAT_INPUT)
        && make_action(g_actions.triggerR, "trigger_r", XR_ACTION_TYPE_FLOAT_INPUT)
        && make_action(g_actions.gripL, "grip_l", XR_ACTION_TYPE_FLOAT_INPUT)
        && make_action(g_actions.gripR, "grip_r", XR_ACTION_TYPE_FLOAT_INPUT)
        && make_action(g_actions.a, "btn_a", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.b, "btn_b", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.x, "btn_x", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.y, "btn_y", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.thumbL, "btn_thumb_l", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.thumbR, "btn_thumb_r", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.menu, "btn_menu", XR_ACTION_TYPE_BOOLEAN_INPUT)
        && make_action(g_actions.aimL, "aim_l", XR_ACTION_TYPE_POSE_INPUT)
        && make_action(g_actions.aimR, "aim_r", XR_ACTION_TYPE_POSE_INPUT)
        && make_action(g_actions.palmL, "palm_l", XR_ACTION_TYPE_POSE_INPUT)
        && make_action(g_actions.palmR, "palm_r", XR_ACTION_TYPE_POSE_INPUT);
    if (!created) {
        log_line("ev=vr.xr actions result=fail reason=action");
        return false;
    }
    // Vibration is optional, like the grip poses: a refusal costs the confirmation pulses only.
    if (!make_action(g_actions.hapticL, "haptic_l", XR_ACTION_TYPE_VIBRATION_OUTPUT)) {
        g_actions.hapticL = XR_NULL_HANDLE;
    }
    if (!make_action(g_actions.hapticR, "haptic_r", XR_ACTION_TYPE_VIBRATION_OUTPUT)) {
        g_actions.hapticR = XR_NULL_HANDLE;
    }
    const std::array<Binding, 19> touch{{
        {g_actions.aimL, "/user/hand/left/input/aim/pose"},
        {g_actions.aimR, "/user/hand/right/input/aim/pose"},
        {g_actions.palmL, "/user/hand/left/input/grip/pose"},
        {g_actions.palmR, "/user/hand/right/input/grip/pose"},
        {g_actions.move, "/user/hand/left/input/thumbstick"},
        {g_actions.turn, "/user/hand/right/input/thumbstick"},
        {g_actions.triggerL, "/user/hand/left/input/trigger/value"},
        {g_actions.triggerR, "/user/hand/right/input/trigger/value"},
        {g_actions.gripL, "/user/hand/left/input/squeeze/value"},
        {g_actions.gripR, "/user/hand/right/input/squeeze/value"},
        {g_actions.x, "/user/hand/left/input/x/click"},
        {g_actions.y, "/user/hand/left/input/y/click"},
        {g_actions.a, "/user/hand/right/input/a/click"},
        {g_actions.b, "/user/hand/right/input/b/click"},
        {g_actions.thumbL, "/user/hand/left/input/thumbstick/click"},
        {g_actions.thumbR, "/user/hand/right/input/thumbstick/click"},
        {g_actions.menu, "/user/hand/left/input/menu/click"},
        {g_actions.hapticL, "/user/hand/left/output/haptic"},
        {g_actions.hapticR, "/user/hand/right/output/haptic"},
    }};
    suggest("/interaction_profiles/oculus/touch_controller", touch);
    const std::array<Binding, 9> simple{{
        {g_actions.aimL, "/user/hand/left/input/aim/pose"},
        {g_actions.aimR, "/user/hand/right/input/aim/pose"},
        {g_actions.palmL, "/user/hand/left/input/grip/pose"},
        {g_actions.palmR, "/user/hand/right/input/grip/pose"},
        {g_actions.triggerR, "/user/hand/right/input/select/click"},
        {g_actions.triggerL, "/user/hand/left/input/select/click"},
        {g_actions.menu, "/user/hand/left/input/menu/click"},
        {g_actions.hapticL, "/user/hand/left/output/haptic"},
        {g_actions.hapticR, "/user/hand/right/output/haptic"},
    }};
    suggest("/interaction_profiles/khr/simple_controller", simple);
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_actions.set;
    if (g_api.attachSessionActionSets(g_session, &attach) != XR_SUCCESS) {
        log_line("ev=vr.xr actions result=fail reason=attach");
        return false;
    }
    g_actions.attached = true;
    // Action spaces come after the attach: an unattached set has no bound sources yet, and a
    // runtime is entitled to refuse. A refusal costs the hands and nothing else, so it is logged
    // and the rest of the session carries on -- head tracking must not depend on controllers.
    const bool spaces = make_action_space(g_actions.aimL, g_actions.spaceL)
                        && make_action_space(g_actions.aimR, g_actions.spaceR);
    // The grip spaces are allowed to fail on their own: a runtime that only offers aim poses
    // still gives head tracking and a pointable weapon, just with the pivot at the aim origin.
    // The fallback is silent at the point of use, so it is logged loudly here instead.
    const bool palmSpaces = make_action_space(g_actions.palmL, g_actions.palmSpaceL)
                            && make_action_space(g_actions.palmR, g_actions.palmSpaceR);
    log_fmt("ev=vr.xr actions result=ok spaces=%d palm_spaces=%d haptics=%d", spaces ? 1 : 0, palmSpaces ? 1 : 0,
            g_actions.hapticL != XR_NULL_HANDLE && g_api.applyHapticFeedback != nullptr ? 1 : 0);
    return true;
}

[[nodiscard]] float float_of(XrAction action) noexcept {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    if (g_api.getActionStateFloat(g_session, &get, &state) != XR_SUCCESS || !state.isActive) {
        return 0.0F;
    }
    return state.currentState;
}

[[nodiscard]] XrVector2f vector_of(XrAction action) noexcept {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (g_api.getActionStateVector2f(g_session, &get, &state) != XR_SUCCESS || !state.isActive) {
        return XrVector2f{};
    }
    return state.currentState;
}

[[nodiscard]] bool bool_of(XrAction action) noexcept {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (g_api.getActionStateBoolean(g_session, &get, &state) != XR_SUCCESS || !state.isActive) {
        return false;
    }
    return state.currentState == XR_TRUE;
}

/**
 * Vibration requests, one slot per hand (left, right), raised from any thread and played on the
 * present thread at the next frame. A newer request replaces an unplayed one: these are
 * confirmations, and two queued clicks would only blur into one longer one.
 */
std::array<std::atomic<float>, 2> g_hapticAmplitude{};
std::array<std::atomic<float>, 2> g_hapticSeconds{};

void flush_haptics() noexcept {
    for (std::size_t hand = 0; hand < 2; ++hand) {
        const float amplitude = g_hapticAmplitude[hand].exchange(0.0F, std::memory_order_acq_rel);
        if (amplitude <= 0.0F) {
            continue;
        }
        const XrAction action = hand == 0 ? g_actions.hapticL : g_actions.hapticR;
        if (action == XR_NULL_HANDLE || g_api.applyHapticFeedback == nullptr || !g_actions.attached) {
            continue;
        }
        const float seconds = g_hapticSeconds[hand].load(std::memory_order_relaxed);
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = action;
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.amplitude = std::clamp(amplitude, 0.0F, 1.0F);
        vibration.duration = static_cast<XrDuration>(std::clamp(seconds, 0.001F, 1.0F) * 1e9F);
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        const XrResult result =
            g_api.applyHapticFeedback(g_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        log_fmt("ev=vr.xr haptic hand=%s amplitude=%.2f ms=%.0f result=%d", hand == 0 ? "L" : "R", vibration.amplitude,
                seconds * 1000.0F, static_cast<int>(result));
    }
}

/** Syncs the action set and publishes the controllers. An unfocused session reads as idle. */
void sync_actions() noexcept {
    if (!g_actions.attached) {
        return;
    }
    XrActiveActionSet active{g_actions.set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    InputState input{};
    input.valid = true;
    if (g_api.syncActions(g_session, &sync) == XR_SUCCESS) {
        const XrVector2f move = vector_of(g_actions.move);
        const XrVector2f turn = vector_of(g_actions.turn);
        input.moveX = move.x;
        input.moveY = move.y;
        input.turnX = turn.x;
        input.turnY = turn.y;
        input.triggerL = float_of(g_actions.triggerL);
        input.triggerR = float_of(g_actions.triggerR);
        input.gripL = float_of(g_actions.gripL);
        input.gripR = float_of(g_actions.gripR);
        input.buttons = (bool_of(g_actions.a) ? kButtonA : 0U) | (bool_of(g_actions.b) ? kButtonB : 0U)
                        | (bool_of(g_actions.x) ? kButtonX : 0U) | (bool_of(g_actions.y) ? kButtonY : 0U)
                        | (bool_of(g_actions.thumbL) ? kButtonThumbL : 0U)
                        | (bool_of(g_actions.thumbR) ? kButtonThumbR : 0U)
                        | (bool_of(g_actions.menu) ? kButtonMenu : 0U);
    }
    AcquireSRWLockExclusive(&g_poseLock);
    g_input = input;
    ReleaseSRWLockExclusive(&g_poseLock);
}

/** Frees everything Presentation owns. The swapchain images belong to the runtime. */
void destroy_presentation(Presentation& p) noexcept {
    for (auto& target : p.targets) {
        release_com(target);
    }
    release_com(p.blitSource);
    release_com(p.blitView);
    release_com(p.blitVertex);
    release_com(p.blitPixel);
    release_com(p.blitSampler);
    release_com(p.blitRasterizer);
    release_com(p.blitState);
    if (p.swapchain != XR_NULL_HANDLE && g_api.destroySwapchain != nullptr) {
        g_api.destroySwapchain(p.swapchain);
    }
    p = Presentation{};
}

/** @return The typed view format for a back-buffer format, typeless ones included. */
[[nodiscard]] DXGI_FORMAT typed_view_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

/** @return True when a format is 8-bit RGBA in either channel order. */
[[nodiscard]] bool is_rgba8_family(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
           || format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

[[nodiscard]] bool is_bgra8_family(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
           || format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

[[nodiscard]] bool is_srgb(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

/** Builds the blit pass: shaders, sampler, a staging copy of the back buffer and one RTV per image. */
[[nodiscard]] bool prepare_blit(Presentation& p, ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source) noexcept {
    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT result = D3DCompile(kBlitVertexShader,
                                std::strlen(kBlitVertexShader),
                                nullptr,
                                nullptr,
                                nullptr,
                                "main",
                                "vs_4_0",
                                D3DCOMPILE_ENABLE_STRICTNESS,
                                0,
                                &vertexBlob,
                                &errors);
    release_com(errors);
    if (SUCCEEDED(result)) {
        const char* pixelSource = is_srgb(p.format) ? kBlitPixelShaderDecode : kBlitPixelShaderCopy;
        result = D3DCompile(pixelSource,
                            std::strlen(pixelSource),
                            nullptr,
                            nullptr,
                            nullptr,
                            "main",
                            "ps_4_0",
                            D3DCOMPILE_ENABLE_STRICTNESS,
                            0,
                            &pixelBlob,
                            &errors);
        release_com(errors);
    }
    if (SUCCEEDED(result)) {
        result = device->CreateVertexShader(
            vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &p.blitVertex);
    }
    if (SUCCEEDED(result)) {
        result = device->CreatePixelShader(
            pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &p.blitPixel);
    }
    release_com(vertexBlob);
    release_com(pixelBlob);
    if (SUCCEEDED(result)) {
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        result = device->CreateSamplerState(&sampler, &p.blitSampler);
    }
    if (SUCCEEDED(result)) {
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        result = device->CreateRasterizerState(&rasterizer, &p.blitRasterizer);
    }
    if (SUCCEEDED(result)) {
        D3D11_TEXTURE2D_DESC staging = source;
        staging.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        staging.Usage = D3D11_USAGE_DEFAULT;
        staging.CPUAccessFlags = 0;
        staging.MiscFlags = 0;
        staging.MipLevels = 1;
        result = device->CreateTexture2D(&staging, nullptr, &p.blitSource);
    }
    if (SUCCEEDED(result)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = typed_view_format(source.Format);
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        result = device->CreateShaderResourceView(p.blitSource, &view, &p.blitView);
    }
    for (std::uint32_t index = 0; SUCCEEDED(result) && index < p.imageCount; ++index) {
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = p.format;
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        result = device->CreateRenderTargetView(p.images[index], &view, &p.targets[index]);
    }
    if (SUCCEEDED(result)) {
        ID3D11Device1* device1 = nullptr;
        result = device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
        if (SUCCEEDED(result)) {
            const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
            D3D_FEATURE_LEVEL selected{};
            result = device1->CreateDeviceContextState(0,
                                                       &level,
                                                       1,
                                                       D3D11_SDK_VERSION,
                                                       __uuidof(ID3D11Device),
                                                       &selected,
                                                       &p.blitState);
            release_com(device1);
        }
    }
    if (FAILED(result)) {
        log_fmt("ev=vr.xr blit result=fail hr=0x%08lX", static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

/**
 * Creates the runtime swapchain for a back buffer, rebuilding it when the back buffer changes.
 * The image is the back buffer's size and, when the runtime allows it, a format CopyResource can
 * take the back buffer's bits into. An sRGB-typed target is preferred: the desktop image is
 * sRGB-encoded, and that type tells the compositor so.
 */
[[nodiscard]] bool ensure_presentation(Presentation& p, ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source) noexcept {
    if (p.failed) {
        return false;
    }
    if (p.swapchain != XR_NULL_HANDLE && p.width == source.Width && p.height == source.Height
        && p.sourceFormat == source.Format) {
        return true;
    }
    destroy_presentation(p);
    if (source.SampleDesc.Count > 1) {
        log_line("ev=vr.xr swapchain result=fail reason=msaa_back_buffer");
        p.failed = true;
        return false;
    }
    std::array<std::int64_t, 32> offered{};
    std::uint32_t offeredCount = 0;
    if (g_api.enumerateSwapchainFormats(
            g_session, static_cast<std::uint32_t>(offered.size()), &offeredCount, offered.data())
        != XR_SUCCESS) {
        offeredCount = 0;
    }
    const auto offers = [&](DXGI_FORMAT format) noexcept {
        for (std::uint32_t index = 0; index < offeredCount; ++index) {
            if (offered[index] == static_cast<std::int64_t>(format)) {
                return true;
            }
        }
        return false;
    };
    DXGI_FORMAT chosen = DXGI_FORMAT_UNKNOWN;
    bool direct = false;
    if (is_rgba8_family(source.Format) && offers(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
        chosen = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        direct = true;
    } else if (is_bgra8_family(source.Format) && offers(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
        chosen = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        direct = true;
    } else {
        constexpr std::array<DXGI_FORMAT, 4> candidates{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                                        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                                        DXGI_FORMAT_R8G8B8A8_UNORM,
                                                        DXGI_FORMAT_B8G8R8A8_UNORM};
        for (const DXGI_FORMAT candidate : candidates) {
            if (offers(candidate)) {
                chosen = candidate;
                break;
            }
        }
    }
    if (chosen == DXGI_FORMAT_UNKNOWN) {
        log_fmt("ev=vr.xr swapchain result=fail reason=no_format offered=%u", offeredCount);
        p.failed = true;
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = static_cast<std::int64_t>(chosen);
    info.sampleCount = 1;
    info.width = source.Width;
    info.height = source.Height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (g_api.createSwapchain(g_session, &info, &p.swapchain) != XR_SUCCESS
        || p.swapchain == XR_NULL_HANDLE) {
        log_line("ev=vr.xr swapchain result=fail reason=create");
        p.failed = true;
        return false;
    }
    std::array<XrSwapchainImageD3D11KHR, kMaxSwapchainImages> images{};
    for (auto& image : images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    std::uint32_t imageCount = 0;
    if (g_api.enumerateSwapchainImages(p.swapchain,
                                       kMaxSwapchainImages,
                                       &imageCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))
            != XR_SUCCESS
        || imageCount == 0 || imageCount > kMaxSwapchainImages) {
        log_line("ev=vr.xr swapchain result=fail reason=images");
        destroy_presentation(p);
        p.failed = true;
        return false;
    }
    for (std::uint32_t index = 0; index < imageCount; ++index) {
        p.images[index] = images[index].texture;
    }
    p.imageCount = imageCount;
    p.width = source.Width;
    p.height = source.Height;
    p.sourceFormat = source.Format;
    p.format = chosen;
    p.copyDirect = direct;
    if (!direct && !prepare_blit(p, device, source)) {
        destroy_presentation(p);
        p.failed = true;
        return false;
    }
    log_fmt("ev=vr.xr swapchain result=ok size=%ux%u source_format=%d format=%d direct=%d images=%u",
            p.width,
            p.height,
            static_cast<int>(source.Format),
            static_cast<int>(chosen),
            direct ? 1 : 0,
            imageCount);
    return true;
}

/** Draws the back buffer copy into one swapchain image inside a swapped-in context state. */
[[nodiscard]] bool blit(Presentation& p, ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer, std::uint32_t index) noexcept {
    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                       reinterpret_cast<void**>(&context1)))
        || context1 == nullptr) {
        return false;
    }
    context->CopyResource(p.blitSource, backBuffer);
    ID3DDeviceContextState* previous = nullptr;
    context1->SwapDeviceContextState(p.blitState, &previous);
    const D3D11_VIEWPORT viewport{
        0.0F, 0.0F, static_cast<float>(p.width), static_cast<float>(p.height), 0.0F, 1.0F};
    ID3D11RenderTargetView* target = p.targets[index];
    ID3D11ShaderResourceView* view = p.blitView;
    ID3D11SamplerState* sampler = p.blitSampler;
    context->RSSetViewports(1, &viewport);
    context->RSSetState(p.blitRasterizer);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(p.blitVertex, nullptr, 0);
    context->PSSetShader(p.blitPixel, nullptr, 0);
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, &sampler);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context1->SwapDeviceContextState(previous, nullptr);
    release_com(previous);
    release_com(context1);
    return true;
}

[[nodiscard]] XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) noexcept;

/** @return a * b. */
[[nodiscard]] XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b) noexcept {
    return XrQuaternionf{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                         a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

/**
 * A pose turned about the reference space's vertical by `radians`, positive to the left -- the
 * game's positive yaw, which OpenXR's +Y rotation also is. Position turns about the space's origin.
 */
[[nodiscard]] XrPosef turned_about_up(const XrPosef& pose, float radians) noexcept {
    if (radians == 0.0F || !std::isfinite(radians)) {
        return pose;
    }
    const XrQuaternionf turn{0.0F, std::sin(radians * 0.5F), 0.0F, std::cos(radians * 0.5F)};
    XrPosef out{};
    out.orientation = multiply(turn, pose.orientation);
    out.position = rotate(turn, pose.position);
    return out;
}

/** Where the image being presented came from. */
struct FrameSource final {
    std::array<XrView, kViewCount> views{};
    /** 0 the midpoint, 1 left, 2 right. */
    int eye{};
    float fold{};
    bool stereo{};
    /** False when no world view has been drawn since the last present (a menu, a load). */
    bool fresh{};
    std::uint64_t sequence{};
    /** The publication the image was drawn from. */
    std::uint64_t serial{};
    /** True when the GPU named the record; false for the old assumption (last present's views). */
    bool ledger{};
    /** The instant the record's poses were located for. */
    XrTime located{};
};
FrameSource g_lastSource{};

/** One eye's share of the rendered image: the pixels it covers and the FOV those pixels span. */
struct EyeCrop final {
    XrRect2Di rect{};
    XrFovf fov{};
};

/** What one eye's view declares while it holds an image, in alternating stereo. */
struct Shown final {
    bool valid{};
    XrPosef pose{};
    EyeCrop crop{};
    float fold{};
    std::uint32_t width{};
    std::uint32_t height{};
};
std::array<Shown, kViewCount> g_shown{};
/** An eye FOV the rendered image did not contain, reported once (the crop clamps it). */
bool g_cropClampReported = false;
std::atomic_bool g_perEyeCrop{true};
/** The declaration probe (set_declaration_probe): radians about up, and the tangent scale. */
std::atomic<float> g_probeYaw{0.0F};
std::atomic<float> g_probeFovScale{1.0F};

/**
 * @return The part of the rendered image that one eye's own FOV covers, and that FOV as the pixels
 *         really span it.
 *
 * The engine renders one SYMMETRIC frustum (tangents -tanX..tanX, -tanY..tanY) that contains both
 * eyes, while each eye's own FOV is asymmetric (a Quest 3's left eye is -54..+40 deg). Declaring the
 * whole image with the symmetric FOV is what the OpenXR spec allows, and what our mock and SteamVR
 * honour -- but Meta's PC runtime is reported to ignore a projection view's declared FOV and map the
 * sub-image rect onto the eye's OWN FOV (Khronos forum 110078; OpenXR-Layer-crop-fov #36). On a
 * Quest 3 that stretched the full image over -54..+40 in one eye and -40..+54 in the other: the
 * centre ~15 deg apart in opposite directions, double vision at every depth (2026-10-16 session).
 *
 * So each view gets exactly the rect its eye covers, declared with the FOV of that rect. A runtime
 * that honours the declaration and one that substitutes the eye's FOV then do the same thing, to
 * within the rounding of the rect to whole pixels (half a pixel per edge, recomputed into the
 * declared FOV so an honouring runtime is exact).
 */
[[nodiscard]] EyeCrop eye_crop(const XrFovf& eye, float tanX, float tanY, std::uint32_t width,
                               std::uint32_t height) noexcept {
    EyeCrop crop{};
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const bool usable = tanX > 0.0F && tanY > 0.0F && width != 0 && height != 0 && eye.angleRight > eye.angleLeft
                        && eye.angleUp > eye.angleDown;
    if (!usable) {
        // No eye FOV to crop to: the whole image, declared as rendered.
        crop.rect = XrRect2Di{XrOffset2Di{0, 0},
                              XrExtent2Di{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)}};
        crop.fov = XrFovf{-std::atan(tanX), std::atan(tanX), std::atan(tanY), -std::atan(tanY)};
        return crop;
    }
    // Image x grows with the tangent from -tanX at 0 to +tanX at w; image y grows DOWN, from +tanY.
    const auto column = [&](float tangent) { return (tangent + tanX) / (2.0F * tanX) * w; };
    const auto row = [&](float tangent) { return (tanY - tangent) / (2.0F * tanY) * h; };
    const float wantLeft = column(std::tan(eye.angleLeft));
    const float wantRight = column(std::tan(eye.angleRight));
    const float wantTop = row(std::tan(eye.angleUp));
    const float wantBottom = row(std::tan(eye.angleDown));
    const auto clampRound = [](float v, float limit) {
        return static_cast<std::int32_t>(std::lround((std::min)((std::max)(v, 0.0F), limit)));
    };
    const std::int32_t x0 = clampRound(wantLeft, w);
    const std::int32_t x1 = (std::max)(clampRound(wantRight, w), x0 + 1);
    const std::int32_t y0 = clampRound(wantTop, h);
    const std::int32_t y1 = (std::max)(clampRound(wantBottom, h), y0 + 1);
    if ((wantLeft < -0.5F || wantRight > w + 0.5F || wantTop < -0.5F || wantBottom > h + 0.5F) && !g_cropClampReported) {
        // The image does not hold the whole eye: that eye gets a narrower declared FOV (an honouring
        // runtime shows black beyond it, a substituting one stretches by the missing fraction).
        g_cropClampReported = true;
        log_fmt("ev=vr.xr crop clamped want=%.1f,%.1f,%.1f,%.1f image=%ux%u", wantLeft, wantRight, wantTop,
                wantBottom, width, height);
    }
    crop.rect = XrRect2Di{XrOffset2Di{x0, y0}, XrExtent2Di{x1 - x0, y1 - y0}};
    const auto tangentAtColumn = [&](std::int32_t x) { return -tanX + 2.0F * tanX * static_cast<float>(x) / w; };
    const auto tangentAtRow = [&](std::int32_t y) { return tanY - 2.0F * tanY * static_cast<float>(y) / h; };
    crop.fov = XrFovf{std::atan(tangentAtColumn(x0)), std::atan(tangentAtColumn(x1)), std::atan(tangentAtRow(y0)),
                      std::atan(tangentAtRow(y1))};
    return crop;
}

/**
 * The record the GPU last saw drawn -- on the single immediate context, the frame being presented.
 * Before the ledger has a match, the old assumption: the views located at the previous present.
 */
[[nodiscard]] FrameSource presented_source() noexcept {
    FrameSource source{};
    AcquireSRWLockShared(&g_ledgerLock);
    const bool have = g_haveDrawn && g_drawn.views[0].type == XR_TYPE_VIEW;
    if (have) {
        source.views = g_drawn.views;
        source.eye = g_drawn.eye;
        source.fold = g_drawn.head.fold;
        source.stereo = g_drawn.stereo;
        source.sequence = g_drawn.sequence;
        source.serial = g_drawn.serial;
        source.ledger = true;
        source.located = g_drawn.located;
    }
    ReleaseSRWLockShared(&g_ledgerLock);
    if (!have) {
        source.views = g_renderedViews;
        source.eye = g_renderedEye;
        source.serial = g_renderedSerial;
        source.fresh = true;
        return source;
    }
    source.fresh = source.sequence != g_lastSource.sequence;
    return source;
}

/**
 * Learns the lead (g_locateTime) from the image about to be submitted: it is shown at this frame's
 * predicted display time, and was drawn from poses located for `source.located`. The difference is
 * what the lead still lacks. Only images the GPU named and that are new count (a repeated image says
 * nothing about the pipeline); each correction is a fifth of the error and at most a display period,
 * so a single hitch cannot throw it; the lead stays within 0..3 periods.
 */
void update_lead(const FrameSource& source) noexcept {
    if (!source.ledger || !source.fresh || source.located == 0 || g_displayPeriod <= 0) {
        return;
    }
    const std::int64_t error = g_predictedDisplayTime - source.located;
    g_leadErrorNs.store(error, std::memory_order_relaxed);
    const std::int64_t period = g_displayPeriod;
    const std::int64_t step = (std::max)(-period, (std::min)(period, error / 5));
    const std::int64_t lead = (std::max)(std::int64_t{0}, (std::min)(3 * period, g_leadNs.load(std::memory_order_relaxed) + step));
    g_leadNs.store(lead, std::memory_order_relaxed);
}

/**
 * Copies the presented back buffer into the runtime swapchain and fills the projection views.
 * @return True when an image was acquired, filled and released, so a layer may reference it.
 */
[[nodiscard]] bool submit_back_buffer(IDXGISwapChain* swapChain,
                                      std::span<XrCompositionLayerProjectionView, kViewCount> views) noexcept {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))
        || backBuffer == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backBuffer->GetDevice(&device);
    if (device != nullptr) {
        device->GetImmediateContext(&context);
    }
    bool submitted = false;
    // Which frame this image is, and so which eye it belongs to and what pose it was drawn from.
    const FrameSource source = presented_source();
    const bool stereoFrame = source.stereo && source.fresh && (source.eye == 1 || source.eye == 2);
    const std::size_t target = stereoFrame ? static_cast<std::size_t>(source.eye - 1) : 0;
    if (device != nullptr && context != nullptr && ensure_presentation(g_presentations[target], device, description)) {
        Presentation& p = g_presentations[target];
        std::uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (g_api.acquireSwapchainImage(p.swapchain, &acquire, &index) == XR_SUCCESS
            && index < p.imageCount) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = kImageWaitNanoseconds;
            if (g_api.waitSwapchainImage(p.swapchain, &wait) == XR_SUCCESS) {
                if (p.copyDirect) {
                    // The UI keeps its canvas 16:10 at the narrowest, so an eye-shaped back buffer holds
                    // the world cropped to a centred band (with the HUD on it). The band's own input --
                    // the whole frame -- fills the eye; the back buffer's band goes on top, HUD and all.
                    unsigned top = 0;
                    unsigned bottom = 0;
                    ID3D11Texture2D* scene = display::canvas_band(description.Width, description.Height, top, bottom)
                                                 ? gpu::scene_texture()
                                                 : nullptr;
                    D3D11_TEXTURE2D_DESC sceneDesc{};
                    if (scene != nullptr) {
                        scene->GetDesc(&sceneDesc);
                    }
                    if (scene != nullptr && sceneDesc.Width == description.Width && sceneDesc.Height == description.Height
                        && sceneDesc.SampleDesc.Count == 1 && is_rgba8_family(sceneDesc.Format)) {
                        context->CopySubresourceRegion(p.images[index], 0, 0, 0, 0, scene, 0, nullptr);
                        const D3D11_BOX band{0, top, 0, description.Width, bottom, 1};
                        context->CopySubresourceRegion(p.images[index], 0, 0, top, 0, backBuffer, 0, &band);
                        ++g_filledFrames;
                    } else {
                        context->CopyResource(p.images[index], backBuffer);
                    }
                    if (scene != nullptr) {
                        scene->Release();
                    }
                    submitted = true;
                } else {
                    submitted = blit(p, context, backBuffer, index);
                }
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            g_api.releaseSwapchainImage(p.swapchain, &release);
        }
        if (submitted) {
            // The layer has to declare the frustum the engine really rendered, and the only ground
            // truth for that is the projection the GPU received for the world (`vr_gpu.cpp`). The
            // aspect written into the pose block is NOT it: the engine takes the block's FOV but
            // keeps the back buffer's aspect, so declaring the written 1.074 told the compositor
            // the image held +-50 deg vertically when it held +-35.7 -- the world stretched 1.65x
            // tall in the headset (measured with a GPU capture, 2026-10-15). It is what had
            // removed the black bands. Without a fresh measurement, the block's FOV and the back
            // buffer's pixel ratio, which is what the engine renders with.
            float halfHorizontal = 0.0F;
            float halfVertical = 0.0F;
            float p00 = 0.0F;
            float p11 = 0.0F;
            if (gpu::world_projection(p00, p11)) {
                halfHorizontal = std::atan(1.0F / p00);
                halfVertical = std::atan(1.0F / p11);
            } else {
                float horizontal = g_renderedFov.load(std::memory_order_relaxed);
                if (!(horizontal > 0.0F && horizontal < 3.1F)) {
                    horizontal = 1.5708F;
                }
                const float aspect = static_cast<float>(p.width) / static_cast<float>(p.height);
                halfHorizontal = horizontal * 0.5F;
                halfVertical = std::atan(std::tan(halfHorizontal) / aspect);
            }
            g_declaredHalfVertical.store(halfVertical, std::memory_order_relaxed);
            const float tanX = std::tan(halfHorizontal);
            const float tanY = std::tan(halfVertical);
            g_submittedTanX = tanX;
            g_submittedTanY = tanY;
            // Each eye's share of this image (eye_crop): its own FOV, as located for the frame the
            // image was drawn from.
            const bool perEye = g_perEyeCrop.load(std::memory_order_relaxed);
            const auto crop = [&](std::size_t eye) {
                return eye_crop(perEye ? source.views[eye].fov : XrFovf{}, tanX, tanY, p.width, p.height);
            };
            const float probeYaw = g_probeYaw.load(std::memory_order_relaxed);
            const float probeScale = g_probeFovScale.load(std::memory_order_relaxed);
            // The pose the image was really drawn from: the eye the camera took, the orientation
            // shared by both eyes. In mono that is one pose for both views, which is what keeps a
            // flat image fusable at infinity (see cyclopean()).
            const XrPosef drawn = render_pose(source.views, source.eye);
            const auto fill = [&](XrCompositionLayerProjectionView& view, const Presentation& from,
                                  const XrPosef& pose, const EyeCrop& share) {
                view = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose = probeYaw != 0.0F ? turned_about_up(pose, probeYaw) : pose;
                view.fov = share.fov;
                if (probeScale != 1.0F) {
                    const auto scaled = [&](float angle) { return std::atan(std::tan(angle) * probeScale); };
                    view.fov = XrFovf{scaled(view.fov.angleLeft), scaled(view.fov.angleRight), scaled(view.fov.angleUp),
                                      scaled(view.fov.angleDown)};
                }
                view.subImage.swapchain = from.swapchain;
                view.subImage.imageRect = share.rect;
                view.subImage.imageArrayIndex = 0;
            };
            if ((g_frameIndex % kSubmitReportPeriod) == 0) {
                const EyeCrop left = crop(0);
                const EyeCrop right = crop(1);
                log_fmt("ev=vr.xr crop image=%ux%u tan=%.4f,%.4f L=%d,%d,%d,%d fov=%.4f,%.4f,%.4f,%.4f "
                        "R=%d,%d,%d,%d fov=%.4f,%.4f,%.4f,%.4f",
                        p.width, p.height, tanX, tanY, left.rect.offset.x, left.rect.offset.y, left.rect.extent.width,
                        left.rect.extent.height, left.fov.angleLeft, left.fov.angleRight, left.fov.angleUp,
                        left.fov.angleDown, right.rect.offset.x, right.rect.offset.y, right.rect.extent.width,
                        right.rect.extent.height, right.fov.angleLeft, right.fov.angleRight, right.fov.angleUp,
                        right.fov.angleDown);
            }
            if (!stereoFrame) {
                for (std::uint32_t eye = 0; eye < kViewCount; ++eye) {
                    fill(views[eye], p, drawn, crop(eye));
                }
                // A mono frame in the middle of stereo (a menu, a load, the first frame) leaves no
                // held image worth showing beside the next fresh one.
                g_shown = {};
            } else {
                Shown& fresh = g_shown[target];
                fresh.valid = true;
                fresh.pose = drawn;
                fresh.crop = crop(target);
                fresh.fold = source.fold;
                fresh.width = p.width;
                fresh.height = p.height;
                const std::size_t other = 1 - target;
                fill(views[target], p, drawn, fresh.crop);
                const Presentation& held = g_presentations[other];
                if (g_shown[other].valid && held.swapchain != XR_NULL_HANDLE && held.width == p.width
                    && held.height == p.height) {
                    // The other eye keeps the image it was given a frame ago, declared where it was
                    // drawn -- turned by whatever artificial turn happened since, which moved the
                    // world against the room. Without that, a smooth turn of 120 deg/s would put a
                    // degree of false disparity between the eyes every frame.
                    const float turned = g_shown[other].fold - source.fold;
                    fill(views[other], held, turned_about_up(g_shown[other].pose, turned), g_shown[other].crop);
                } else {
                    // Nothing held yet: this eye's image for both, once, each eye's own share of it.
                    fill(views[other], p, drawn, crop(other));
                }
            }
            g_lastSource = source;
            update_lead(source);
        }
    }
    release_com(context);
    release_com(device);
    release_com(backBuffer);
    return submitted;
}

/** Turns a vector by a quaternion. */
[[nodiscard]] XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) noexcept {
    // v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f inner{u.y * v.z - u.z * v.y + q.w * v.x,
                           u.z * v.x - u.x * v.z + q.w * v.y,
                           u.x * v.y - u.y * v.x + q.w * v.z};
    return XrVector3f{v.x + 2.0F * (u.y * inner.z - u.z * inner.y),
                      v.y + 2.0F * (u.z * inner.x - u.x * inner.z),
                      v.z + 2.0F * (u.x * inner.y - u.y * inner.x)};
}

/**
 * @return The viewpoint the mono image is really rendered from: the midpoint of the eyes, with a
 *         single orientation for both.
 *
 * This is what fixes the double vision. One image was being submitted twice, but declared at each
 * eye's own pose, and a projection layer is a geometric promise the compositor keeps: it places
 * the content in the world according to the pose it is told, then lets the eye look at it from
 * where that eye really is. Two different declared poses therefore put the same pixels in two
 * different world directions, which is constant, wrong disparity across the whole frame -- a few
 * degrees is far past what fuses, so the brain suppresses one eye. Declaring the same cyclopean
 * pose for both views puts the content in identical world directions, disparity is zero, and it
 * fuses at infinity: exactly the flat-and-far mono look that was agreed.
 *
 * The orientations are averaged rather than one of them picked, because a headset with canted
 * panels reports a different orientation per eye and taking the left one yaws the whole view by
 * half the cant angle, permanently.
 */
[[nodiscard]] XrPosef cyclopean(const std::array<XrView, kViewCount>& views) noexcept;

/**
 * @return The pose the mono image is rendered from: the cyclopean orientation always, and the
 *         position of the chosen eye -- the midpoint (0), the left eye (1) or the right (2).
 *         Used both to place the camera and to declare the layer, so the two can never disagree.
 */
[[nodiscard]] XrPosef render_pose(const std::array<XrView, kViewCount>& views, int eye) noexcept {
    XrPosef pose = cyclopean(views);
    if (eye == 1 || eye == 2) {
        pose.position = views[static_cast<std::size_t>(eye - 1)].pose.position;
    }
    return pose;
}

[[nodiscard]] XrPosef cyclopean(const std::array<XrView, kViewCount>& views) noexcept {
    const XrQuaternionf& left = views[0].pose.orientation;
    const XrQuaternionf& right = views[1].pose.orientation;
    // A quaternion and its negation are the same rotation, so an unaligned sum can cancel out
    // instead of averaging.
    const float dot = left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
    const float sign = dot < 0.0F ? -1.0F : 1.0F;
    const XrQuaternionf mean{left.x + sign * right.x, left.y + sign * right.y,
                             left.z + sign * right.z, left.w + sign * right.w};
    const float length =
        std::sqrt(mean.x * mean.x + mean.y * mean.y + mean.z * mean.z + mean.w * mean.w);
    XrPosef pose{};
    pose.orientation =
        length > 1.0e-6F
            ? XrQuaternionf{mean.x / length, mean.y / length, mean.z / length, mean.w / length}
            : left;
    pose.position = XrVector3f{(views[0].pose.position.x + views[1].pose.position.x) * 0.5F,
                               (views[0].pose.position.y + views[1].pose.position.y) * 0.5F,
                               (views[0].pose.position.z + views[1].pose.position.z) * 0.5F};
    return pose;
}

/**
 * Maps a vector out of OpenXR's basis into the game's.
 * OpenXR: +X right, +Y up, -Z forward. Game: X forward, Z up, and therefore +Y left.
 */
[[nodiscard]] Vector to_game(const XrVector3f& v) noexcept {
    return Vector{-v.z, -v.x, v.y};
}

/** @return The angle wrapped into -pi..pi, so a difference of two yaws never takes the long way. */
[[nodiscard]] float wrap_angle(float radians) noexcept {
    return std::atan2(std::sin(radians), std::cos(radians));
}

/** Turns a vector about the game's up axis. */
void yaw_about_up(Vector& v, float sine, float cosine) noexcept {
    const float x = v[0];
    const float y = v[1];
    v[0] = x * cosine - y * sine;
    v[1] = x * sine + y * cosine;
}

/**
 * Converts one OpenXR pose into the game's basis, with its translation measured from the recentre
 * origin and the body yaw folded in -- the same treatment the head gets, so head and hand share
 * one anchor.
 * @param pose Pose in the reference space.
 * @param sine Sine of the body yaw.
 * @param cosine Cosine of the body yaw.
 * @param out Receives forward, up, right and offset.
 */
void to_game_pose(const XrPosef& pose, float sine, float cosine, HandPose& out) noexcept {
    out.forward = to_game(rotate(pose.orientation, XrVector3f{0.0F, 0.0F, -1.0F}));
    out.up = to_game(rotate(pose.orientation, XrVector3f{0.0F, 1.0F, 0.0F}));
    out.right = to_game(rotate(pose.orientation, XrVector3f{1.0F, 0.0F, 0.0F}));
    out.offset = to_game(XrVector3f{(pose.position.x - g_origin.x) * g_unitsPerMetre,
                                    (pose.position.y - g_origin.y) * g_unitsPerMetre,
                                    (pose.position.z - g_origin.z) * g_unitsPerMetre});
    yaw_about_up(out.forward, sine, cosine);
    yaw_about_up(out.up, sine, cosine);
    yaw_about_up(out.right, sine, cosine);
    yaw_about_up(out.offset, sine, cosine);
}

/**
 * Locates one action space against the same reference space the views were located in.
 * @param space The action space, or XR_NULL_HANDLE when the runtime refused one.
 * @param out Receives the pose, untouched on failure.
 * @return False when the space is missing or the runtime reports it untracked.
 */
[[nodiscard]] bool locate_raw(XrSpace space, XrPosef& out) noexcept {
    if (space == XR_NULL_HANDLE || g_api.locateSpace == nullptr || g_space == XR_NULL_HANDLE) {
        return false;
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    // The same instant as the head (begin_frame): the hands drawn in an image are where they will be
    // when that image is shown, like the head it is drawn from.
    if (g_api.locateSpace(space, g_space, g_locateTime, &location) != XR_SUCCESS) {
        return false;
    }
    // Both bits are required: an orientation-only location would put the weapon at the origin,
    // which reads as a plausible pose and would quietly ruin every position test.
    constexpr XrSpaceLocationFlags required =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((location.locationFlags & required) != required) {
        return false;
    }
    out = location.pose;
    return true;
}

/**
 * The last good RAW pose of one controller, in the reference space, before any yaw fold.
 *
 * Raw is the entire point. Holding a pose that has already been folded by the room anchor makes it
 * go out of date the instant the player turns, and turns a lost frame of tracking into a
 * translation that grows with every degree of artificial turn -- the same class of error that made
 * the weapon slide away from the hands. Kept unfolded, a held pose is re-folded by the current
 * anchor each frame and stays exactly where the hand really was.
 */
struct HandRaw final {
    XrPosef aim{};
    XrPosef palm{};
    bool havePalm{};
    bool have{};
};
std::array<HandRaw, 2> g_handRaw{};

/**
 * Locates one controller and converts it into the game's basis.
 *
 * Both of the hand's OpenXR poses are located: `aim` for where it points, `grip` for where the
 * palm is. When tracking drops, the last good raw pose is re-folded and served with `stale` set
 * rather than the hand simply disappearing -- see HandPose::stale for why that matters.
 * @param index 0 for the left hand, 1 for the right.
 * @param aimSpace The aim action space.
 * @param palmSpace The grip action space, or XR_NULL_HANDLE when the runtime refused one.
 * @param sine Sine of the fold angle.
 * @param cosine Cosine of the fold angle.
 * @return The pose, invalid only when this controller has never been tracked at all.
 */
[[nodiscard]] HandPose locate_hand(std::size_t index, XrSpace aimSpace, XrSpace palmSpace, float sine,
                                   float cosine) noexcept {
    HandPose hand{};
    HandRaw& raw = g_handRaw[index];
    XrPosef aimPose{};
    XrPosef palmPose{};
    if (locate_raw(aimSpace, aimPose)) {
        raw.aim = aimPose;
        raw.havePalm = locate_raw(palmSpace, palmPose);
        raw.palm = raw.havePalm ? palmPose : aimPose;
        raw.have = true;
    } else if (raw.have) {
        hand.stale = true;
    } else {
        return hand;
    }
    to_game_pose(raw.aim, sine, cosine, hand);
    // Only the translation is wanted from the grip pose. Its orientation is a different convention
    // from the aim pose's (thumb-up along its own axis rather than along the pointing ray), so
    // taking `forward` from it would silently re-aim the weapon by about ninety degrees.
    HandPose palmOnly{};
    to_game_pose(raw.palm, sine, cosine, palmOnly);
    hand.palm = palmOnly.offset;
    // ...but the grip's own axes are kept apart, for what is held in a fist rather than pointed.
    hand.gripForward = raw.havePalm ? palmOnly.forward : hand.forward;
    hand.gripUp = raw.havePalm ? palmOnly.up : hand.up;
    hand.frame = g_frameIndex;
    hand.valid = true;
    return hand;
}

[[nodiscard]] const char* state_name(XrSessionState state) noexcept {
    switch (state) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
    case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss_pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "unknown";
    }
}

/** RAII for the pump's lock. */
struct PumpLock final {
    PumpLock() noexcept { AcquireSRWLockExclusive(&g_pump.lock); }
    ~PumpLock() { ReleaseSRWLockExclusive(&g_pump.lock); }
    PumpLock(const PumpLock&) = delete;
    PumpLock& operator=(const PumpLock&) = delete;
};

/** Waits on the pump's condition for at most `ms`; the lock must be held (exclusive). */
void pump_wait(DWORD ms) noexcept {
    SleepConditionVariableSRW(&g_pump.changed, &g_pump.lock, ms, 0);
}

/**
 * STOPPING: the session is ended once no frame is open -- neither the game's nor a keep-alive's --
 * from whichever thread drains the events. Runs unlocked.
 */
void end_session_when_free() noexcept {
    {
        PumpLock held;
        if (g_pump.mainOpen || g_pump.keepAliveOpen) {
            g_pump.endPending = true;
            return;
        }
        g_pump.endPending = false;
        g_pump.ending = true;
        g_pump.waited = false;
        g_pump.request = false;
        g_sessionRunning = false;
        WakeAllConditionVariable(&g_pump.changed);
    }
    g_api.endSession(g_session);
    log_line("ev=vr.xr session state=stopped");
    PumpLock held;
    g_pump.ending = false;
    WakeAllConditionVariable(&g_pump.changed);
}

/** One session state change: the state machine, and what a change means for the rest of the module. */
void on_session_state(XrSessionState state) noexcept {
    g_sessionState = state;
    log_fmt("ev=vr.xr session_state=%s", state_name(state));
    {
        PumpLock held;
        if (state == XR_SESSION_STATE_FOCUSED || state == XR_SESSION_STATE_VISIBLE) {
            g_pump.wasRunning = true;
        } else if (state == XR_SESSION_STATE_IDLE && g_pump.wasRunning) {
            g_pump.wasRunning = false;
            g_pump.idleSince = now_seconds();
        } else if (state == XR_SESSION_STATE_READY) {
            g_pump.idleSince = 0.0;
        }
    }
    switch (state) {
    case XR_SESSION_STATE_READY:
        if (!g_sessionRunning) {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult result = g_api.beginSession(g_session, &begin);
            if (result == XR_SUCCESS) {
                PumpLock held;
                g_sessionRunning = true;
                g_pump.endPending = false;
                log_line("ev=vr.xr session state=running");
            } else {
                log_fmt("ev=vr.xr session begin result=%d", static_cast<int>(result));
            }
        }
        break;
    case XR_SESSION_STATE_STOPPING:
        // Normal: the headset came off, or the runtime's own menu took over. Begin again on READY.
        if (g_sessionRunning) {
            end_session_when_free();
        }
        break;
    case XR_SESSION_STATE_LOSS_PENDING:
        // The headset is gone (Link cable, Air Link, the runtime closed): start over when it is back.
        log_line("ev=vr.xr lost reason=session_loss action=restart");
        g_restartRequested = true;
        break;
    case XR_SESSION_STATE_EXITING:
        // Closed from the headset's own menu: leave VR, and do not reopen it by ourselves -- that is
        // what would put it straight back. F9 off and on again brings it back.
        log_line("ev=vr.xr exiting action=flat_until_f9");
        g_exitHold = true;
        g_restartRequested = true;
        break;
    default: break;
    }
}

/** Drains the event queue, driving the session through its state machine. On the frame thread. */
void pump_events() noexcept {
    for (;;) {
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        const XrResult result = g_api.pollEvent(g_instance, &event);
        if (result != XR_SUCCESS) {
            return;
        }
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            on_session_state(reinterpret_cast<const XrEventDataSessionStateChanged*>(&event)->state);
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto* change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
            if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                // The player recentred from the runtime's own menu: LOCAL now starts where the head
                // is. Ours follows, origin and yaw, so the view does not jump by the old offset.
                g_runtimeRecentre = true;
                g_runtimeRecentres.fetch_add(1, std::memory_order_relaxed);
            }
            log_fmt("ev=vr.xr reference_change space=%d", static_cast<int>(change->referenceSpaceType));
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            log_line("ev=vr.xr lost reason=instance_loss action=restart");
            g_restartRequested = true;
            break;
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            log_line("ev=vr.xr events_lost");
            break;
        default: break;
        }
    }
}

/** A Meta runtime (or the mock) gives a stopped session back late; a new one is READY at once. */
[[nodiscard]] bool kick_applies() noexcept {
    std::array<char, 64> name{};
    for (std::size_t i = 0; i + 1 < name.size() && g_runtimeName[i] != '\0'; ++i) {
        const char c = g_runtimeName[i];
        name[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return std::strstr(name.data(), "oculus") != nullptr || std::strstr(name.data(), "meta") != nullptr
           || std::strstr(name.data(), "mock") != nullptr;
}

/**
 * One frame for the headset with the last frame's layers, at the time this wait was for: the last
 * picture held where it was in the room (the compositor reprojects it for the head as it is now).
 * Frame thread, unlocked, with keepAliveOpen set. The device's lock is held around the frame calls:
 * a runtime may use the game's immediate context in them, and the game may be using it right now.
 */
void keep_alive_frame(const XrFrameState& state) noexcept {
    SavedLayers saved{};
    {
        PumpLock held;
        saved = g_pump.layers;
        if (!g_pump.inRun) {
            g_pump.inRun = true;
            g_pump.runStart = now_seconds();
            g_pump.runFrames = 0;
            ++g_pump.keepAliveRuns;
            if (g_pump.keepAliveRuns <= 30) {
                const double last = (std::max)(g_pump.lastMainEnd, g_pump.lastMainBegin);
                log_fmt("ev=vr.xr keepalive start quiet=%.2f layers=%d quads=%zu", now_seconds() - last,
                        saved.haveProjection ? 1 : 0, saved.quadCount);
            }
        }
        ++g_pump.runFrames;
    }
    saved.projection.views = saved.views.data();
    std::array<const XrCompositionLayerBaseHeader*, 1 + kMaxQuads> layers{};
    std::uint32_t count = 0;
    if (state.shouldRender != XR_FALSE) {
        if (saved.haveProjection) {
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&saved.projection);
        }
        for (std::size_t q = 0; q < saved.quadCount; ++q) {
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&saved.quads[q]);
        }
    }
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult result = g_api.beginFrame(g_session, &beginInfo);
    if (XR_SUCCEEDED(result)) {
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = state.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = count;
        endInfo.layers = count != 0 ? layers.data() : nullptr;
        result = g_api.endFrame(g_session, &endInfo);
    }
    PumpLock held;
    g_pump.lastKeepAlive = now_seconds();
    if (state.predictedDisplayPeriod > 1'000'000 && state.predictedDisplayPeriod < 100'000'000) {
        g_pump.period = static_cast<double>(state.predictedDisplayPeriod) * 1e-9;
    }
    if (XR_SUCCEEDED(result)) {
        ++g_pump.keepAlives;
    } else if (g_pump.waitFails++ < 8) {
        log_fmt("ev=vr.xr keepalive result=%d", static_cast<int>(result));
    }
}

DWORD WINAPI pump_thread(void*) noexcept {
    for (;;) {
        {
            PumpLock held;
            if (g_pump.quit) {
                break;
            }
        }
        pump_events();
        AcquireSRWLockExclusive(&g_pump.lock);
        if (g_pump.quit) {
            ReleaseSRWLockExclusive(&g_pump.lock);
            break;
        }
        if (g_pump.endPending && !g_pump.mainOpen && !g_pump.keepAliveOpen) {
            ReleaseSRWLockExclusive(&g_pump.lock);
            end_session_when_free();
            continue;
        }
        const double now = now_seconds();
        const double lastMain = (std::max)(g_pump.lastMainEnd, g_pump.lastMainBegin);
        if (lastMain > 0.0 && now - lastMain > 2.0 && now - g_pump.lastQuietReport > 10.0) {
            // The watchdog's line: the game has not presented for a while. What it was last doing
            // with the runtime tells a stall of ours from one of its own.
            g_pump.lastQuietReport = now;
            const char* call = g_presentCall.load(std::memory_order_relaxed);
            log_fmt("ev=vr.xr game_quiet s=%.1f main_open=%d present_call=%s session=%s keepalives=%llu", now - lastMain,
                    g_pump.mainOpen ? 1 : 0, call != nullptr ? call : "none", state_name(g_sessionState.load()),
                    static_cast<unsigned long long>(g_pump.keepAlives));
        }
        if (!g_sessionRunning) {
            g_pump.waited = false;
            g_pump.request = false;
            WakeAllConditionVariable(&g_pump.changed);
            if (g_pump.idleSince > 0.0 && !g_exitHold && now - g_pump.idleSince > kKickSeconds && now >= g_pump.nextKick) {
                g_pump.idleSince = 0.0;
                if (kick_applies()) {
                    // Measured on a Quest 3 over Air Link (dishonored-vr): a stopped session came back
                    // 23-169 s later, while a NEW session was READY at once.
                    g_pump.nextKick = now + 20.0;
                    ++g_pump.kicks;
                    log_line("ev=vr.xr session not_offered_back action=new_session");
                    g_restartRequested = true;
                } else {
                    log_fmt("ev=vr.xr session not_offered_back runtime=%s action=wait", g_runtimeName.data());
                }
            }
            pump_wait(20);
            ReleaseSRWLockExclusive(&g_pump.lock);
            continue;
        }
        const bool gameQuiet = g_keepAliveAllowed && !g_pump.mainOpen && lastMain > 0.0 && now - lastMain > kQuietSeconds;
        if (g_pump.waited) {
            // A frame the game asked for and has not taken (its wait gave up): to the headset after a while.
            if (gameQuiet && now - g_pump.waitedAt > kQuietSeconds) {
                const XrFrameState state = g_pump.state;
                g_pump.waited = false;
                g_pump.keepAliveOpen = true;
                ReleaseSRWLockExclusive(&g_pump.lock);
                keep_alive_frame(state);
                PumpLock held;
                g_pump.keepAliveOpen = false;
                WakeAllConditionVariable(&g_pump.changed);
                continue;
            }
            pump_wait(10);
            ReleaseSRWLockExclusive(&g_pump.lock);
            continue;
        }
        if (gameQuiet && !g_pump.request && now - g_pump.lastKeepAlive < g_pump.period * 0.9) {
            // At most one keep-alive frame per display period, whatever the runtime's wait does.
            const double left = g_pump.period * 0.9 - (now - g_pump.lastKeepAlive);
            pump_wait(static_cast<DWORD>(left * 1000.0) + 1);
            ReleaseSRWLockExclusive(&g_pump.lock);
            continue;
        }
        if (g_pump.request || gameQuiet) {
            const bool forGame = g_pump.request;
            g_pump.request = false;
            g_pump.waiting = true;
            ReleaseSRWLockExclusive(&g_pump.lock);
            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState state{XR_TYPE_FRAME_STATE};
            const double t0 = now_seconds();
            const XrResult result = g_api.waitFrame(g_session, &waitInfo, &state);
            const double t1 = now_seconds();
            AcquireSRWLockExclusive(&g_pump.lock);
            g_pump.waiting = false;
            ++g_pump.waits;
            g_pump.longestWait = (std::max)(g_pump.longestWait, t1 - t0);
            if (t1 - t0 > 1.0) {
                log_fmt("ev=vr.xr wait_frame s=%.2f note=runtime_held_frame", t1 - t0);
            }
            if (XR_FAILED(result) || result == XR_SESSION_LOSS_PENDING) {
                if (g_pump.waitFails++ < 8) {
                    log_fmt("ev=vr.xr wait_frame result=%d", static_cast<int>(result));
                }
                pump_wait(5);
                ReleaseSRWLockExclusive(&g_pump.lock);
                continue;
            }
            if (g_pump.quit || !g_sessionRunning || g_pump.ending) {
                ReleaseSRWLockExclusive(&g_pump.lock);
                continue;
            }
            const double lastMain2 = (std::max)(g_pump.lastMainEnd, g_pump.lastMainBegin);
            const bool stillQuiet = g_keepAliveAllowed && !g_pump.mainOpen && lastMain2 > 0.0 && t1 - lastMain2 > kQuietSeconds;
            if (forGame || g_pump.request || !stillQuiet) {
                g_pump.state = state;
                g_pump.waited = true;
                g_pump.waitedAt = t1;
                g_pump.request = false;
                WakeAllConditionVariable(&g_pump.changed);
                ReleaseSRWLockExclusive(&g_pump.lock);
            } else {
                g_pump.keepAliveOpen = true;
                ReleaseSRWLockExclusive(&g_pump.lock);
                keep_alive_frame(state);
                PumpLock held;
                g_pump.keepAliveOpen = false;
                WakeAllConditionVariable(&g_pump.changed);
            }
            continue;
        }
        pump_wait(10);
        ReleaseSRWLockExclusive(&g_pump.lock);
    }
    return 0;
}

/** Starts the frame thread; without it the old way (xrWaitFrame on the present thread) still works. */
void start_pump() noexcept {
    if (g_pump.thread != nullptr) {
        return;
    }
    // No thread is running, so nothing holds the lock: the state starts over, lock and all.
    const double period = g_pump.period;
    g_pump = Pump{};
    g_pump.period = period;
    DWORD id = 0;
    g_pump.thread = CreateThread(nullptr, 0, &pump_thread, nullptr, 0, &id);
    if (g_pump.thread == nullptr) {
        log_fmt("ev=vr.xr pump result=fail error=%lu fallback=present_thread", GetLastError());
        return;
    }
    // It paces the frames, as the render thread did.
    SetThreadPriority(g_pump.thread, THREAD_PRIORITY_ABOVE_NORMAL);
    log_fmt("ev=vr.xr pump result=ok thread=%lu bound_ms=%lu quiet_ms=%d", id, kWaitBoundMs,
            static_cast<int>(kQuietSeconds * 1000.0));
}

/** Asks the frame thread to stop and waits up to `ms`. False while it is still inside a runtime call. */
[[nodiscard]] bool stop_pump(DWORD ms) noexcept {
    if (g_pump.thread == nullptr) {
        return true;
    }
    {
        PumpLock held;
        g_pump.quit = true;
        WakeAllConditionVariable(&g_pump.changed);
    }
    if (WaitForSingleObject(g_pump.thread, ms) != WAIT_OBJECT_0) {
        return false;
    }
    CloseHandle(g_pump.thread);
    g_pump.thread = nullptr;
    return true;
}

/** The present thread's frame is over (or never began): the frame thread may feed the headset and end the session. */
void main_frame_ended() noexcept {
    PumpLock held;
    g_pump.mainOpen = false;
    g_pump.lastMainEnd = now_seconds();
    WakeAllConditionVariable(&g_pump.changed);
}

/** A copy of this frame's layers for the keep-alive. */
void save_layers(const XrCompositionLayerBaseHeader* const* layers, std::uint32_t count) noexcept {
    PumpLock held;
    g_pump.layers.haveProjection = false;
    g_pump.layers.quadCount = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const XrCompositionLayerBaseHeader* layer = layers[i];
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            if (projection->viewCount > kViewCount) {
                continue;
            }
            g_pump.layers.projection = *projection;
            for (std::uint32_t v = 0; v < projection->viewCount; ++v) {
                g_pump.layers.views[v] = projection->views[v];
            }
            g_pump.layers.haveProjection = true;
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD && g_pump.layers.quadCount < kMaxQuads) {
            g_pump.layers.quads[g_pump.layers.quadCount++] = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
        }
    }
}

} // namespace

/** @return True while a session is running and poses are being produced. */
bool active() noexcept {
    return g_sessionRunning;
}

/** @return Why the runtime is or is not running. */
Status status() noexcept {
    return g_status;
}

/** @return A short, stable name for the runtime in use. */
const char* runtime_name() noexcept {
    return g_runtimeName[0] != '\0' ? g_runtimeName.data() : "none";
}

void set_render_eye(int eye) noexcept {
    g_renderEye.store(eye == 1 || eye == 2 ? eye : 0, std::memory_order_relaxed);
}

int render_eye() noexcept {
    return g_renderEye.load(std::memory_order_relaxed);
}

Vector aiming_eye_offset(const HeadPose& head) noexcept {
    const int eye = g_renderEye.load(std::memory_order_relaxed);
    if (head.eye == eye) {
        return head.offset;
    }
    if (eye == 1 || eye == 2) {
        return head.eyeOffsets[static_cast<std::size_t>(eye - 1)];
    }
    return Vector{(head.eyeOffsets[0][0] + head.eyeOffsets[1][0]) * 0.5F,
                  (head.eyeOffsets[0][1] + head.eyeOffsets[1][1]) * 0.5F,
                  (head.eyeOffsets[0][2] + head.eyeOffsets[1][2]) * 0.5F};
}

/** Takes the next head position as the origin. */
void recentre() noexcept {
    g_recentreRequested = true;
}

namespace {

[[nodiscard]] bool finish_initialize(ID3D11Device* device) noexcept;

/**
 * Brings up the runtime, instance and system. On the init thread; the session follows on the present
 * thread (create_session), because it is the one call that works on the game's device.
 * An instance left from an earlier attempt that found no headset is reused: only xrGetSystem is
 * asked again, so a headset switched on later is picked up without reloading the runtime.
 */
[[nodiscard]] bool initialize_blocking(ID3D11Device* device) noexcept {
    // Everything the runtime maps while we are inside its setup is the runtime's, not the game's.
    const OwnLoads own;
    if (g_instance == XR_NULL_HANDLE) {
        if (!load_runtime() || !bind_global()) {
            return false;
        }
    }
    if (g_instance != XR_NULL_HANDLE) {
        return finish_initialize(device);
    }
    std::array<const char*, 1> extensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.enabledExtensionNames = extensions.data();
    create.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    std::snprintf(create.applicationInfo.applicationName,
                  sizeof create.applicationInfo.applicationName,
                  "Sunrise VR");
    std::snprintf(create.applicationInfo.engineName,
                  sizeof create.applicationInfo.engineName,
                  "Tiger");
    if (g_api.createInstance(&create, &g_instance) != XR_SUCCESS
        || g_instance == XR_NULL_HANDLE) {
        log_line("ev=vr.xr instance result=fail");
        g_status = Status::instanceFailed;
        return false;
    }
    if (!bind_instance()) {
        g_status = Status::instanceFailed;
        return false;
    }
    // Not part of the required set: haptics are confirmation, never a reason to refuse a session.
    if (!bind(g_api.applyHapticFeedback, "xrApplyHapticFeedback", g_instance)) {
        g_api.applyHapticFeedback = nullptr;
    }
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (g_api.getInstanceProperties(g_instance, &properties) == XR_SUCCESS) {
        std::snprintf(
            g_runtimeName.data(), g_runtimeName.size(), "%s", properties.runtimeName);
    }
    return finish_initialize(device);
}

/**
 * From the instance on, as far as the init thread may go: the system (the headset), its D3D11
 * requirements and the views. The session is the present thread's (create_session).
 */
[[nodiscard]] bool finish_initialize(ID3D11Device* device) noexcept {
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (g_api.getSystem(g_instance, &systemInfo, &g_system) != XR_SUCCESS) {
        log_line("ev=vr.xr system result=fail reason=no_hmd");
        g_status = Status::systemFailed;
        return false;
    }
    // Required before a D3D11 session, and it is where a real runtime reports the adapter it
    // wants. A mismatch against the game's device is the thing to suspect on a hybrid laptop.
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (g_api.getD3D11Requirements(g_instance, g_system, &requirements) != XR_SUCCESS) {
        log_line("ev=vr.xr requirements result=fail");
        g_status = Status::sessionFailed;
        return false;
    }
    std::array<XrViewConfigurationView, kViewCount> views{};
    for (auto& view : views) {
        view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    std::uint32_t viewCount = 0;
    if (g_api.enumerateViewConfigurationViews(g_instance,
                                              g_system,
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              kViewCount,
                                              &viewCount,
                                              views.data())
        != XR_SUCCESS) {
        log_line("ev=vr.xr views result=fail");
        g_status = Status::sessionFailed;
        return false;
    }
    log_fmt("ev=vr.xr views count=%u eye=%ux%u runtime=%s",
            viewCount,
            views[0].recommendedImageRectWidth,
            views[0].recommendedImageRectHeight,
            runtime_name());
    AcquireSRWLockExclusive(&g_geometryLock);
    g_geometry = EyeGeometry{};
    g_geometry.recommendedWidth = views[0].recommendedImageRectWidth;
    g_geometry.recommendedHeight = views[0].recommendedImageRectHeight;
    ReleaseSRWLockExclusive(&g_geometryLock);
    (void)device;
    return true;
}

/**
 * The session, its spaces and the controllers: everything that hands the runtime the game's device.
 * On the PRESENT thread, never the init thread. xrCreateSession on Meta's runtime works on the device
 * it is given, and this game has no D3D11 multithread protection: called from the init thread while
 * the game rendered, on 18 Oct (after the headset came off and on, Air Link), the game's main loop froze
 * for good inside the 1.5 s the call took (13:20:26-28 in Meta's log, `stall result=detected` 6 s later,
 * no line of ours ever again). On the present thread the game is waiting for us, so nothing else is
 * on the device; the price is a hitch of that call's length, once per session.
 */
[[nodiscard]] bool create_session(ID3D11Device* device) noexcept {
    const OwnLoads own;
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g_system;
    const ULONGLONG started = GetTickCount64();
    const XrResult created = g_api.createSession(g_instance, &sessionInfo, &g_session);
    const auto took = static_cast<unsigned long long>(GetTickCount64() - started);
    if (created != XR_SUCCESS || g_session == XR_NULL_HANDLE) {
        log_fmt("ev=vr.xr session result=fail code=%d ms=%llu", static_cast<int>(created), took);
        g_session = XR_NULL_HANDLE;
        g_status = Status::sessionFailed;
        return false;
    }
    log_fmt("ev=vr.xr session result=ok ms=%llu thread=present", took);
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0F;
    if (g_api.createReferenceSpace(g_session, &spaceInfo, &g_space) != XR_SUCCESS) {
        log_line("ev=vr.xr space result=fail");
        g_status = Status::sessionFailed;
        return false;
    }
    // VIEW: the head's own space, for the fade that brings the world back after a screen.
    XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace.orientation.w = 1.0F;
    if (g_api.createReferenceSpace(g_session, &viewInfo, &g_viewSpace) != XR_SUCCESS) {
        g_viewSpace = XR_NULL_HANDLE;
        log_line("ev=vr.xr view_space result=fail note=no_fade");
    }
    // The controllers are optional: head tracking runs with or without them.
    (void)create_actions();
    log_fmt("ev=vr.xr init result=ok runtime=%s", runtime_name());
    return true;
}

DWORD WINAPI init_thread(void*) noexcept {
    const bool ok = initialize_blocking(g_initDevice);
    g_initOk.store(ok, std::memory_order_release);
    g_initPhase.store(static_cast<int>(InitPhase::done), std::memory_order_release);
    return 0;
}

/** Destroys the session, its spaces and swapchains, and (unless `keepInstance`) the instance. Present thread. */
void teardown(bool keepInstance) noexcept;

} // namespace

/**
 * Brings the runtime up, without ever blocking the game: the work runs on the init thread, and this
 * picks the answer up on a later present. A failure is retried -- every 3 s while there is no
 * headset or no runtime (the instance is kept, so only the headset is asked for again), every 10 s
 * otherwise -- for as long as VR is switched on. Present thread.
 * @return True once a session exists (it runs when the runtime says READY).
 */
bool initialize(ID3D11Device* device) noexcept {
    const auto phase = static_cast<InitPhase>(g_initPhase.load(std::memory_order_acquire));
    if (phase == InitPhase::idle && g_status == Status::running) {
        return true;
    }
    if (g_exitHold) {
        return false;
    }
    if (phase == InitPhase::working) {
        return false;
    }
    if (phase == InitPhase::done) {
        if (g_initThread != nullptr) {
            // Never an unbounded wait on the game's thread: the init thread has published its answer,
            // but leaving takes the loader lock, which the runtime's own threads may be holding.
            if (WaitForSingleObject(g_initThread, 0) != WAIT_OBJECT_0) {
                return false;
            }
            CloseHandle(g_initThread);
            g_initThread = nullptr;
        }
        g_initPhase.store(static_cast<int>(InitPhase::idle), std::memory_order_release);
        if (g_initOk.load(std::memory_order_acquire)
            && !create_session(device != nullptr ? device : g_initDevice)) {
            g_initOk.store(false, std::memory_order_release);
        }
        if (g_initOk.load(std::memory_order_acquire)) {
            g_status = Status::running;
            g_attempts = 0;
            g_restartRequested = false;
            // Off unless asked for: on 17 Oct the first keep-alive frame of a run coincided with the game
            // AND the frame thread freezing together for 7.5 s (one keep-alive frame, an hourglass of
            // 7.53 s on the mock), and calling the runtime from a second thread while the game renders
            // cannot be proven safe here. When the game stalls, the runtime reprojects its last frame.
            const char* keepAlive = settings::value("keepalive", "off");
            g_keepAliveAllowed = std::strcmp(keepAlive, "on") == 0 || (std::strcmp(keepAlive, "auto") == 0 && kick_applies());
            log_fmt("ev=vr.xr keepalive allowed=%d setting=%s runtime=%s", g_keepAliveAllowed ? 1 : 0, keepAlive, runtime_name());
            start_pump();
            return true;
        }
        const Status failed = g_status;
        const bool noHeadset = failed == Status::systemFailed || failed == Status::noRuntime;
        // Without a headset the instance is kept: asking for the system again is all a retry needs.
        teardown(failed == Status::systemFailed);
        g_status = failed;
        g_nextAttemptMs = GetTickCount64() + (noHeadset ? 3000 : 10000);
        if (g_attempts <= 3 || (g_attempts % 20) == 0) {
            log_fmt("ev=vr.xr init result=retry status=%d attempt=%u next_ms=%d", static_cast<int>(failed), g_attempts,
                    noHeadset ? 3000 : 10000);
        }
        return false;
    }
    if (device == nullptr || GetTickCount64() < g_nextAttemptMs) {
        return false;
    }
    ++g_attempts;
    g_initDevice = device;
    g_initOk = false;
    g_initPhase.store(static_cast<int>(InitPhase::working), std::memory_order_release);
    DWORD id = 0;
    g_initThread = CreateThread(nullptr, 0, &init_thread, nullptr, 0, &id);
    if (g_initThread == nullptr) {
        // No thread: the old way, on this one.
        const bool ok = initialize_blocking(device);
        g_initOk = ok;
        g_initPhase.store(static_cast<int>(InitPhase::done), std::memory_order_release);
    }
    return false;
}

/** Runs one OpenXR frame and publishes the pose. */
void begin_frame(float roomAnchor) noexcept {
    if (g_status != Status::running) {
        return;
    }
    if (g_restartRequested) {
        // The runtime took the session (or the instance) away: everything goes, and initialize()
        // brings it back when the runtime has it again. A frame thread still inside a runtime call
        // keeps the teardown waiting until it returns (this runs again next present).
        if (!stop_pump(50)) {
            return;
        }
        log_line("ev=vr.xr restart action=teardown");
        teardown(false);
        g_nextAttemptMs = GetTickCount64() + 3000;
        g_restartRequested = false;
        return;
    }
    if (g_pump.thread == nullptr) {
        pump_events();
    }
    if (g_runtimeRecentre.exchange(false)) {
        // LOCAL moved under us: take the origin again, where the head is now.
        g_recentreRequested = true;
        log_line("ev=vr.xr recentre reason=runtime");
    }
    if (!g_sessionRunning || g_frameOpen) {
        return;
    }
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (g_pump.thread != nullptr) {
        // The frame thread waits; this asks for a frame and waits for it with a bound. A runtime that
        // holds frames back costs this present the headset, never the game its frame.
        AcquireSRWLockExclusive(&g_pump.lock);
        if (!g_pump.waited) {
            g_pump.request = true;
            WakeAllConditionVariable(&g_pump.changed);
        }
        const DWORD bound = g_pump.lastTimedOut ? 2 : kWaitBoundMs;
        const double deadline = now_seconds() + bound / 1000.0;
        while (!g_pump.waited && !g_pump.quit && !g_pump.ending && g_sessionRunning) {
            const double left = deadline - now_seconds();
            if (left <= 0.0) {
                break;
            }
            pump_wait(static_cast<DWORD>(left * 1000.0) + 1);
        }
        const double now = now_seconds();
        if (!g_pump.waited || g_pump.ending || !g_sessionRunning) {
            ++g_pump.mainTimeouts;
            if (!g_pump.lastTimedOut) {
                g_pump.lastTimedOut = true;
                g_pump.timedOutSince = now;
                if (g_pump.mainTimeouts <= 20) {
                    log_fmt("ev=vr.xr wait_bound ms=%lu waiting=%d note=game_goes_on", bound, g_pump.waiting ? 1 : 0);
                }
            }
            ReleaseSRWLockExclusive(&g_pump.lock);
            return;
        }
        if (g_pump.lastTimedOut) {
            g_pump.lastTimedOut = false;
            if (g_pump.mainTimeouts <= 20) {
                log_fmt("ev=vr.xr wait_bound over s=%.2f", now - g_pump.timedOutSince);
            }
        }
        if (g_pump.inRun) {
            const double run = now - g_pump.runStart;
            g_pump.longestRun = (std::max)(g_pump.longestRun, run);
            if (g_pump.keepAliveRuns <= 30) {
                log_fmt("ev=vr.xr keepalive over frames=%llu s=%.2f", static_cast<unsigned long long>(g_pump.runFrames), run);
            }
            g_pump.inRun = false;
        }
        frameState = g_pump.state;
        g_pump.waited = false;
        g_pump.request = false;
        g_pump.mainOpen = true;
        g_pump.lastMainBegin = now;
        ReleaseSRWLockExclusive(&g_pump.lock);
    } else {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        g_presentCall = "xrWaitFrame";
        const XrResult waited = g_api.waitFrame(g_session, &waitInfo, &frameState);
        g_presentCall = nullptr;
        if (waited != XR_SUCCESS) {
            return;
        }
    }
    g_predictedDisplayTime = frameState.predictedDisplayTime;
    g_displayPeriod = frameState.predictedDisplayPeriod;
    const std::int64_t fixedLead = g_leadFixedNs.load(std::memory_order_relaxed);
    g_locateTime = g_predictedDisplayTime + (fixedLead >= 0 ? fixedLead : g_leadNs.load(std::memory_order_relaxed));
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    g_presentCall = "xrBeginFrame";
    const XrResult begun = g_api.beginFrame(g_session, &beginInfo);
    g_presentCall = nullptr;
    if (XR_FAILED(begun)) {
        main_frame_ended();
        return;
    }
    g_frameOpen = true;
    g_shouldRender = frameState.shouldRender != XR_FALSE;
    sync_actions();
    flush_haptics();
    if (!g_shouldRender) {
        return;
    }
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = g_locateTime;
    locate.space = g_space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    std::array<XrView, kViewCount> views{};
    for (auto& view : views) {
        view.type = XR_TYPE_VIEW;
    }
    std::uint32_t count = 0;
    if (g_api.locateViews(g_session, &locate, &viewState, kViewCount, &count, views.data())
            != XR_SUCCESS
        || count < kViewCount) {
        return;
    }
    const bool tracked = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0
                         && (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    if (!tracked) {
        return;
    }
    // What the camera hook renders next is drawn from these; the layer for that image needs them.
    g_pendingViews = views;
    g_pendingEye = g_renderEye.load(std::memory_order_relaxed);
    g_havePending = true;
    // Both eyes' geometry, once per session. The mock reports identical orientations and so cannot
    // reproduce a real headset's panel cant; logging it means the next headset session leaves the
    // evidence behind instead of the question having to be asked twice.
    static bool loggedEyes = false;
    if (!loggedEyes) {
        loggedEyes = true;
        for (std::uint32_t eye = 0; eye < kViewCount; ++eye) {
            log_fmt("ev=vr.xr eyes index=%u pos=%.4f,%.4f,%.4f quat=%.5f,%.5f,%.5f,%.5f "
                    "fov_lrud=%.4f,%.4f,%.4f,%.4f",
                    eye, views[eye].pose.position.x, views[eye].pose.position.y,
                    views[eye].pose.position.z, views[eye].pose.orientation.x,
                    views[eye].pose.orientation.y, views[eye].pose.orientation.z,
                    views[eye].pose.orientation.w, views[eye].fov.angleLeft, views[eye].fov.angleRight,
                    views[eye].fov.angleUp, views[eye].fov.angleDown);
        }
    }
    // The origin is the midpoint of the eyes whichever eye renders, so switching eyes moves the
    // camera by half the eye distance and nothing else: the hands, measured from the same origin,
    // stay where they are.
    const XrPosef& left = views[0].pose;
    const XrPosef& right = views[1].pose;
    const XrVector3f midpoint{(left.position.x + right.position.x) * 0.5F,
                              (left.position.y + right.position.y) * 0.5F,
                              (left.position.z + right.position.z) * 0.5F};
    const XrVector3f centre = render_pose(views, g_pendingEye).position;
    const bool recentring = g_recentreRequested || !g_haveOrigin;
    if (recentring) {
        g_origin = midpoint;
        g_haveOrigin = true;
        g_recentreRequested = false;
        log_fmt("ev=vr.xr recentre x=%.3f y=%.3f z=%.3f eye=%d", midpoint.x, midpoint.y, midpoint.z, g_pendingEye);
    }
    // The cyclopean orientation, not the left eye's: with canted panels the left eye is off by
    // half the cant angle, and that would yaw the whole view for the entire session.
    const XrQuaternionf centreOrientation = cyclopean(views).orientation;
    const XrVector3f forwardXr = rotate(centreOrientation, XrVector3f{0.0F, 0.0F, -1.0F});
    const XrVector3f upXr = rotate(centreOrientation, XrVector3f{0.0F, 1.0F, 0.0F});
    const XrVector3f delta{(centre.x - g_origin.x) * g_unitsPerMetre,
                           (centre.y - g_origin.y) * g_unitsPerMetre,
                           (centre.z - g_origin.z) * g_unitsPerMetre};

    HeadPose pose{};
    pose.forward = to_game(forwardXr);
    pose.up = to_game(upXr);
    pose.offset = to_game(delta);
    // The head's yaw in the room, before anything is folded in, and the same value measured from
    // the recentre facing. The second is what the body servo steers towards.
    const float roomYaw = std::atan2(pose.forward[1], pose.forward[0]);
    if (recentring) {
        g_originYaw = roomYaw;
        log_fmt("ev=vr.xr recentre_yaw room=%.4f", roomYaw);
    }
    pose.roomYaw = wrap_angle(roomYaw - g_originYaw);
    // Fold by the room anchor MINUS the recentre facing: the result is the anchor plus the head's
    // own rotation, so looking straight ahead points exactly along the anchor no matter which way
    // the player happens to be sitting in the room.
    const float fold = wrap_angle(roomAnchor - g_originYaw);
    const float sine = std::sin(fold);
    const float cosine = std::cos(fold);
    yaw_about_up(pose.forward, sine, cosine);
    yaw_about_up(pose.up, sine, cosine);
    yaw_about_up(pose.offset, sine, cosine);
    for (std::size_t index = 0; index < kViewCount; ++index) {
        const XrVector3f& p = views[index].pose.position;
        pose.eyeOffsets[index] = to_game(XrVector3f{(p.x - g_origin.x) * g_unitsPerMetre,
                                                    (p.y - g_origin.y) * g_unitsPerMetre,
                                                    (p.z - g_origin.z) * g_unitsPerMetre});
        yaw_about_up(pose.eyeOffsets[index], sine, cosine);
    }
    pose.eye = g_pendingEye;
    pose.fold = fold;

    // The engine's frustum field is a single symmetric horizontal FOV, so the eyes' asymmetric
    // frusta have to be covered by the smallest symmetric one that contains both.
    float halfHorizontal = 0.0F;
    float halfVertical = 0.0F;
    for (std::uint32_t index = 0; index < kViewCount; ++index) {
        const XrFovf& fov = views[index].fov;
        halfHorizontal = (std::max)(halfHorizontal,
                                    (std::max)(std::fabs(fov.angleLeft), std::fabs(fov.angleRight)));
        halfVertical = (std::max)(halfVertical,
                                  (std::max)(std::fabs(fov.angleUp), std::fabs(fov.angleDown)));
    }
    pose.horizontalFov = halfHorizontal * 2.0F;
    {
        // The union both eyes need, and the density of the panel at its centre: the recommended
        // size spread over one eye's own tangent extent.
        float densityX = 0.0F;
        float densityY = 0.0F;
        for (std::uint32_t index = 0; index < kViewCount; ++index) {
            const XrFovf& fov = views[index].fov;
            const float spanX = std::tan(fov.angleRight) - std::tan(fov.angleLeft);
            const float spanY = std::tan(fov.angleUp) - std::tan(fov.angleDown);
            AcquireSRWLockShared(&g_geometryLock);
            const float recW = static_cast<float>(g_geometry.recommendedWidth);
            const float recH = static_cast<float>(g_geometry.recommendedHeight);
            ReleaseSRWLockShared(&g_geometryLock);
            if (spanX > 0.01F && spanY > 0.01F) {
                densityX = (std::max)(densityX, recW / spanX);
                densityY = (std::max)(densityY, recH / spanY);
            }
        }
        AcquireSRWLockExclusive(&g_geometryLock);
        g_geometry.tanHalfWidth = std::tan(halfHorizontal);
        g_geometry.tanHalfHeight = std::tan(halfVertical);
        g_geometry.pixelsPerTanX = densityX;
        g_geometry.pixelsPerTanY = densityY;
        g_geometry.valid = densityX > 0.0F && densityY > 0.0F;
        ReleaseSRWLockExclusive(&g_geometryLock);
    }
    const float tangentVertical = std::tan(halfVertical);
    pose.aspect = tangentVertical > 0.0F ? std::tan(halfHorizontal) / tangentVertical : 1.0F;
    pose.frame = ++g_frameIndex;
    pose.valid = true;

    // The hands ride on the head's frame: same reference space, same origin, same yaw fold, so a
    // consumer can subtract one from the other and get the real geometry of the room.
    const std::array<HandPose, 2> hands{
        locate_hand(0, g_actions.spaceL, g_actions.palmSpaceL, sine, cosine),
        locate_hand(1, g_actions.spaceR, g_actions.palmSpaceR, sine, cosine)};

    AcquireSRWLockExclusive(&g_poseLock);
    g_pose = pose;
    g_hands = hands;
    ReleaseSRWLockExclusive(&g_poseLock);
    g_pendingSerial = pose.frame;
    frames::note(frames::Kind::publish, pose.frame, nullptr, g_pendingEye);
    AcquireSRWLockExclusive(&g_ledgerLock);
    Publication& publication = g_publications[pose.frame % kPublicationRing];
    publication.serial = pose.frame;
    publication.located = g_locateTime;
    publication.views = views;
    publication.head = pose;
    publication.hands = hands;
    publication.valid = true;
    ReleaseSRWLockExclusive(&g_ledgerLock);

    if ((g_frameIndex % kHandReportPeriod) == 0) {
        // The fold angle goes out with the poses, and the head's offset beside the hands', so a
        // test can undo the body yaw exactly and compare head against hand within one frame
        // rather than pairing up two throttled log lines that were never adjacent.
        log_fmt("ev=vr.xr hand yaw=%.4f r_valid=%d r_stale=%d r_fwd=%.3f,%.3f,%.3f r_up=%.3f,%.3f,%.3f "
                "r_off=%.3f,%.3f,%.3f r_palm=%.3f,%.3f,%.3f l_valid=%d l_fwd=%.3f,%.3f,%.3f "
                "l_off=%.3f,%.3f,%.3f h_off=%.3f,%.3f,%.3f",
                fold,
                hands[1].valid ? 1 : 0, hands[1].stale ? 1 : 0,
                hands[1].forward[0], hands[1].forward[1], hands[1].forward[2],
                hands[1].up[0], hands[1].up[1], hands[1].up[2],
                hands[1].offset[0], hands[1].offset[1], hands[1].offset[2],
                hands[1].palm[0], hands[1].palm[1], hands[1].palm[2],
                hands[0].valid ? 1 : 0,
                hands[0].forward[0], hands[0].forward[1], hands[0].forward[2],
                hands[0].offset[0], hands[0].offset[1], hands[0].offset[2],
                pose.offset[0], pose.offset[1], pose.offset[2]);
    }

    if ((g_frameIndex % 180) == 0) {
        log_fmt("ev=vr.xr pose fwd=%.3f,%.3f,%.3f off=%.3f,%.3f,%.3f fov=%.4f aspect=%.3f",
                pose.forward[0],
                pose.forward[1],
                pose.forward[2],
                pose.offset[0],
                pose.offset[1],
                pose.offset[2],
                pose.horizontalFov,
                pose.aspect);
    }
}

namespace {

/**
 * A swapchain of the module's own drawing (the screen's panel, its room, the beam, a fade): images a
 * pass renders into, sRGB so the passes work in linear light. One image is acquired, drawn, released
 * per frame; an image whose wait timed out stays acquired and is waited for again next frame, as the
 * spec requires (releasing it unwaited is a call-order error).
 */
struct Surface final {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::array<ID3D11Texture2D*, kMaxSwapchainImages> images{};
    std::uint32_t imageCount{};
    std::uint32_t width{};
    std::uint32_t height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    bool failed{};
    bool acquired{};
    std::uint32_t index{};
};
Surface g_panelSurface{};
Surface g_beamSurface{};
Surface g_fadeSurface{};
std::array<Surface, kViewCount> g_backdropSurfaces{};

void destroy_surface(Surface& s) noexcept {
    if (s.swapchain != XR_NULL_HANDLE && g_api.destroySwapchain != nullptr) {
        g_api.destroySwapchain(s.swapchain);
    }
    s = Surface{};
}

[[nodiscard]] bool ensure_surface(Surface& s, std::uint32_t width, std::uint32_t height, const char* name) noexcept {
    if (s.swapchain != XR_NULL_HANDLE && s.width == width && s.height == height) {
        return true;
    }
    if (s.failed && s.width == width && s.height == height) {
        return false;
    }
    destroy_surface(s);
    s.width = width;
    s.height = height;
    std::array<std::int64_t, 32> offered{};
    std::uint32_t count = 0;
    if (g_api.enumerateSwapchainFormats(g_session, static_cast<std::uint32_t>(offered.size()), &count, offered.data()) != XR_SUCCESS) {
        count = 0;
    }
    for (const DXGI_FORMAT candidate : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        for (std::uint32_t i = 0; i < count && s.format == DXGI_FORMAT_UNKNOWN; ++i) {
            if (offered[i] == static_cast<std::int64_t>(candidate)) {
                s.format = candidate;
            }
        }
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = static_cast<std::int64_t>(s.format);
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    std::array<XrSwapchainImageD3D11KHR, kMaxSwapchainImages> images{};
    for (auto& image : images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    if (s.format == DXGI_FORMAT_UNKNOWN || g_api.createSwapchain(g_session, &info, &s.swapchain) != XR_SUCCESS
        || g_api.enumerateSwapchainImages(s.swapchain, kMaxSwapchainImages, &s.imageCount,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))
               != XR_SUCCESS
        || s.imageCount == 0 || s.imageCount > kMaxSwapchainImages) {
        log_fmt("ev=vr.xr surface=%s result=fail size=%ux%u format=%d", name, width, height, static_cast<int>(s.format));
        destroy_surface(s);
        s.width = width;
        s.height = height;
        s.failed = true;
        return false;
    }
    for (std::uint32_t i = 0; i < s.imageCount; ++i) {
        s.images[i] = images[i].texture;
    }
    log_fmt("ev=vr.xr surface=%s result=ok size=%ux%u images=%u", name, width, height, s.imageCount);
    return true;
}

/** Acquires (or keeps) an image, draws it with `draw(target)`, releases it. */
template <typename Draw> [[nodiscard]] bool render_surface(Surface& s, Draw&& draw) noexcept {
    if (s.swapchain == XR_NULL_HANDLE) {
        return false;
    }
    if (!s.acquired) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (g_api.acquireSwapchainImage(s.swapchain, &acquire, &s.index) != XR_SUCCESS || s.index >= s.imageCount) {
            return false;
        }
        s.acquired = true;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = kImageWaitNanoseconds;
    if (g_api.waitSwapchainImage(s.swapchain, &wait) != XR_SUCCESS) {
        return false;
    }
    const compositor::Target target{s.images[s.index], s.width, s.height, static_cast<std::uint32_t>(s.format)};
    const bool drawn = draw(target);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    g_api.releaseSwapchainImage(s.swapchain, &release);
    s.acquired = false;
    return drawn;
}

/** The game, a moment ago, for the screen's room: a ring of three, one frozen while a screen shows. */
struct Snapshot final {
    bool valid{};
    double at{};
    XrQuaternionf orientation{0, 0, 0, 1};
    float tangents[4]{};
    ID3D11ShaderResourceView* view{};
};
std::array<Snapshot, 3> g_snapshots{};
std::size_t g_snapshotNext = 0;
int g_frozen = -1;
double g_lastSnapshot = 0.0;

void offer_snapshot(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) noexcept {
    const double now = now_seconds();
    if (now - g_lastSnapshot < 0.2 || g_submittedTanX <= 0.0F || g_submittedTanY <= 0.0F) {
        return;
    }
    std::size_t slot = g_snapshotNext;
    if (static_cast<int>(slot) == g_frozen) {
        slot = (slot + 1) % g_snapshots.size();
    }
    // The whole eye-shaped world when the scene composite ran (no HUD), the back buffer otherwise.
    ID3D11Texture2D* source = gpu::scene_texture();
    ID3D11Texture2D* backBuffer = nullptr;
    if (source == nullptr) {
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
            return;
        }
    }
    ID3D11ShaderResourceView* view =
        compositor::snapshot(device, context, source != nullptr ? source : backBuffer, static_cast<unsigned>(slot));
    if (source != nullptr) {
        source->Release();
    }
    release_com(backBuffer);
    g_lastSnapshot = now;
    if (view == nullptr) {
        return;
    }
    Snapshot& snap = g_snapshots[slot];
    snap.valid = true;
    snap.at = now;
    snap.view = view;
    snap.orientation = render_pose(g_lastSource.views, g_lastSource.eye).orientation;
    snap.tangents[0] = -g_submittedTanX;
    snap.tangents[1] = g_submittedTanX;
    snap.tangents[2] = g_submittedTanY;
    snap.tangents[3] = -g_submittedTanY;
    g_snapshotNext = (slot + 1) % g_snapshots.size();
}

/** @return Seconds since the newest snapshot, or -1. */
[[nodiscard]] double newest_snapshot_age() noexcept {
    double newest = -1.0;
    for (const Snapshot& s : g_snapshots) {
        if (s.valid) {
            newest = (std::max)(newest, s.at);
        }
    }
    return newest < 0.0 ? -1.0 : now_seconds() - newest;
}

/** Freezes the newest snapshot at least 0.15 s old (a menu draws itself a frame or two before it is known), else the newest. */
void freeze_snapshot() noexcept {
    const double now = now_seconds();
    int best = -1;
    int newest = -1;
    for (int i = 0; i < static_cast<int>(g_snapshots.size()); ++i) {
        const Snapshot& s = g_snapshots[static_cast<std::size_t>(i)];
        if (!s.valid) {
            continue;
        }
        if (newest < 0 || s.at > g_snapshots[static_cast<std::size_t>(newest)].at) {
            newest = i;
        }
        if (now - s.at >= 0.15 && (best < 0 || s.at > g_snapshots[static_cast<std::size_t>(best)].at)) {
            best = i;
        }
    }
    g_frozen = best >= 0 ? best : newest;
}

void forget_snapshots() noexcept {
    g_snapshots = {};
    g_frozen = -1;
    g_snapshotNext = 0;
}

/** The screen's frame: the room drawn for each eye's pose now, the panel and the beam over it. */
[[nodiscard]] std::uint32_t compose_screen(const screen::Plan& plan, IDXGISwapChain* swapChain,
                                           XrCompositionLayerProjection& projection,
                                           std::array<XrCompositionLayerProjectionView, kViewCount>& views,
                                           std::array<XrCompositionLayerQuad, kMaxQuads>& quads, std::uint32_t& quadCount) noexcept {
    ID3D11Texture2D* backBuffer = nullptr;
    if (swapChain == nullptr
        || FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))
        || backBuffer == nullptr) {
        return 0;
    }
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backBuffer->GetDevice(&device);
    if (device != nullptr) {
        device->GetImmediateContext(&context);
    }
    std::uint32_t layers = 0;
    quadCount = 0;
    if (device != nullptr && context != nullptr && g_havePending) {
        // The room, per eye, at the pose that eye has this frame: it holds still in the room with
        // nothing to reproject.
        AcquireSRWLockShared(&g_geometryLock);
        const std::uint32_t rw = (std::max)(256U, g_geometry.recommendedWidth / 2);
        const std::uint32_t rh = (std::max)(256U, g_geometry.recommendedHeight / 2);
        ReleaseSRWLockShared(&g_geometryLock);
        const Snapshot* frozen = g_frozen >= 0 && g_snapshots[static_cast<std::size_t>(g_frozen)].valid
                                     ? &g_snapshots[static_cast<std::size_t>(g_frozen)]
                                     : nullptr;
        bool eyesDrawn = true;
        for (std::uint32_t eye = 0; eye < kViewCount; ++eye) {
            Surface& s = g_backdropSurfaces[eye];
            const XrView& view = g_pendingViews[eye];
            const bool drawn = ensure_surface(s, rw, rh, eye == 0 ? "room_l" : "room_r")
                               && render_surface(s, [&](const compositor::Target& target) {
                                      compositor::BackdropDraw b{};
                                      b.eyeOrientation[0] = view.pose.orientation.x;
                                      b.eyeOrientation[1] = view.pose.orientation.y;
                                      b.eyeOrientation[2] = view.pose.orientation.z;
                                      b.eyeOrientation[3] = view.pose.orientation.w;
                                      b.eyeTangents[0] = std::tan(view.fov.angleLeft);
                                      b.eyeTangents[1] = std::tan(view.fov.angleRight);
                                      b.eyeTangents[2] = std::tan(view.fov.angleUp);
                                      b.eyeTangents[3] = std::tan(view.fov.angleDown);
                                      if (frozen != nullptr) {
                                          b.snapshot = frozen->view;
                                          b.snapshotOrientation[0] = frozen->orientation.x;
                                          b.snapshotOrientation[1] = frozen->orientation.y;
                                          b.snapshotOrientation[2] = frozen->orientation.z;
                                          b.snapshotOrientation[3] = frozen->orientation.w;
                                          for (int i = 0; i < 4; ++i) {
                                              b.snapshotTangents[i] = frozen->tangents[i];
                                          }
                                          b.snapshotBrightness = plan.snapshot;
                                      }
                                      b.desaturate = plan.desaturate;
                                      b.environment = plan.environment;
                                      b.fade = plan.fade;
                                      for (int i = 0; i < 3; ++i) {
                                          b.glowDirection[i] = plan.glowDirection[i];
                                      }
                                      b.glow = plan.glow;
                                      return compositor::draw_backdrop(device, context, b, target);
                                  });
            eyesDrawn = eyesDrawn && drawn;
            views[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[eye].pose = view.pose;
            views[eye].fov = view.fov;
            views[eye].subImage.swapchain = s.swapchain;
            views[eye].subImage.imageRect = XrRect2Di{XrOffset2Di{0, 0},
                                                      XrExtent2Di{static_cast<std::int32_t>(s.width), static_cast<std::int32_t>(s.height)}};
        }
        if (eyesDrawn) {
            projection.space = g_space;
            projection.viewCount = kViewCount;
            projection.views = views.data();
            layers = 1;
        }
        if (plan.alpha > 0.0F && plan.outWidth >= 16 && plan.outHeight >= 16
            && ensure_surface(g_panelSurface, plan.outWidth, plan.outHeight, "panel")
            && render_surface(g_panelSurface, [&](const compositor::Target& target) {
                   compositor::PanelDraw p{};
                   p.source = backBuffer;
                   for (int i = 0; i < 4; ++i) {
                       p.crop[i] = plan.crop[i];
                   }
                   p.alpha = plan.alpha;
                   p.corner = plan.corner;
                   p.aspect = plan.height > 0.0F ? plan.width / plan.height : 1.6F;
                   p.pointerU = plan.pointerU;
                   p.pointerV = plan.pointerV;
                   p.pointerAlpha = plan.pointer ? 1.0F : 0.0F;
                   return compositor::draw_panel(device, context, p, target);
               })) {
            XrCompositionLayerQuad& quad = quads[quadCount++];
            quad = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
            quad.space = g_space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_panelSurface.swapchain;
            quad.subImage.imageRect = XrRect2Di{XrOffset2Di{0, 0}, XrExtent2Di{static_cast<std::int32_t>(g_panelSurface.width),
                                                                               static_cast<std::int32_t>(g_panelSurface.height)}};
            quad.pose.position = XrVector3f{plan.panel.position[0], plan.panel.position[1], plan.panel.position[2]};
            quad.pose.orientation = XrQuaternionf{plan.panel.orientation[0], plan.panel.orientation[1], plan.panel.orientation[2],
                                                  plan.panel.orientation[3]};
            quad.size = XrExtent2Df{plan.width, plan.height};
        }
        if (plan.beam && plan.beamLength > 0.01F && ensure_surface(g_beamSurface, 128, 16, "beam")
            && render_surface(g_beamSurface, [&](const compositor::Target& target) {
                   compositor::SolidDraw b{};
                   b.colour[0] = 0.55F;
                   b.colour[1] = 0.75F;
                   b.colour[2] = 1.0F;
                   b.colour[3] = plan.beamAlpha;
                   b.gradient = 1.0F;
                   b.softEdge = 0.45F;
                   return compositor::draw_solid(device, context, b, target);
               })) {
            XrCompositionLayerQuad& quad = quads[quadCount++];
            quad = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space = g_space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_beamSurface.swapchain;
            quad.subImage.imageRect = XrRect2Di{XrOffset2Di{0, 0}, XrExtent2Di{128, 16}};
            quad.pose.position = XrVector3f{plan.beamPose.position[0], plan.beamPose.position[1], plan.beamPose.position[2]};
            quad.pose.orientation = XrQuaternionf{plan.beamPose.orientation[0], plan.beamPose.orientation[1],
                                                  plan.beamPose.orientation[2], plan.beamPose.orientation[3]};
            quad.size = XrExtent2Df{plan.beamLength, 0.006F};
        }
    }
    release_com(context);
    release_com(device);
    release_com(backBuffer);
    return layers;
}

/** The world fading back in after a screen: a black quad over the view, `1 - fade` opaque. */
[[nodiscard]] bool compose_fade(float fade, IDXGISwapChain* swapChain, XrCompositionLayerQuad& quad) noexcept {
    if (fade >= 0.999F || g_viewSpace == XR_NULL_HANDLE || swapChain == nullptr) {
        return false;
    }
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || device == nullptr) {
        return false;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const bool drawn = ensure_surface(g_fadeSurface, 16, 16, "fade")
                       && render_surface(g_fadeSurface, [&](const compositor::Target& target) {
                              compositor::SolidDraw s{};
                              s.colour[3] = std::clamp(1.0F - fade, 0.0F, 1.0F);
                              return compositor::draw_solid(device, context, s, target);
                          });
    release_com(context);
    release_com(device);
    if (!drawn) {
        return false;
    }
    quad = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g_viewSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_fadeSurface.swapchain;
    quad.subImage.imageRect = XrRect2Di{XrOffset2Di{0, 0}, XrExtent2Di{16, 16}};
    quad.pose.orientation.w = 1.0F;
    quad.pose.position = XrVector3f{0.0F, 0.0F, -0.3F};
    quad.size = XrExtent2Df{3.0F, 3.0F};
    return true;
}

} // namespace

/** Ends the OpenXR frame opened by begin_frame, submitting the presented image when it can. */
void end_frame(IDXGISwapChain* swapChain) noexcept {
    if (g_status != Status::running || !g_frameOpen) {
        return;
    }
    // What this frame shows: the game, or the screen (vr_state.cpp decides, vr_screen.cpp places).
    screen::Pose head{};
    bool headValid = false;
    if (g_havePending) {
        const XrPosef centre = cyclopean(g_pendingViews);
        head.position[0] = centre.position.x;
        head.position[1] = centre.position.y;
        head.position[2] = centre.position.z;
        head.orientation[0] = centre.orientation.x;
        head.orientation[1] = centre.orientation.y;
        head.orientation[2] = centre.orientation.z;
        head.orientation[3] = centre.orientation.w;
        headValid = true;
    }
    screen::Pose aims[2]{};
    bool aimValid[2]{};
    for (std::size_t h = 0; h < 2; ++h) {
        if (g_handRaw[h].have) {
            const XrPosef& a = g_handRaw[h].aim;
            aims[h].position[0] = a.position.x;
            aims[h].position[1] = a.position.y;
            aims[h].position[2] = a.position.z;
            aims[h].orientation[0] = a.orientation.x;
            aims[h].orientation[1] = a.orientation.y;
            aims[h].orientation[2] = a.orientation.z;
            aims[h].orientation[3] = a.orientation.w;
            aimValid[h] = true;
        }
    }
    std::uint32_t bbW = 0;
    std::uint32_t bbH = 0;
    if (swapChain != nullptr) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(swapChain->GetDesc(&desc))) {
            bbW = desc.BufferDesc.Width;
            bbH = desc.BufferDesc.Height;
        }
    }
    const screen::Plan plan = screen::plan(head, headValid, aims, aimValid, bbW, bbH, newest_snapshot_age(),
                                           g_runtimeRecentres.load(std::memory_order_relaxed));
    if (plan.freeze) {
        freeze_snapshot();
    } else if (!plan.screen) {
        g_frozen = -1;
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, kViewCount> projectionViews{};
    std::array<XrCompositionLayerQuad, kMaxQuads> quads{};
    std::uint32_t quadCount = 0;
    const XrCompositionLayerBaseHeader* layers[1 + kMaxQuads]{};
    std::uint32_t layerCount = 0;
    if (g_shouldRender && plan.screen) {
        if (compose_screen(plan, swapChain, layer, projectionViews, quads, quadCount) != 0) {
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        }
        for (std::uint32_t q = 0; q < quadCount; ++q) {
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[q]);
        }
    } else if (g_shouldRender && swapChain != nullptr && g_haveRendered
        && submit_back_buffer(swapChain, projectionViews)) {
        layer.space = g_space;
        layer.viewCount = kViewCount;
        layer.views = projectionViews.data();
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device != nullptr) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            offer_snapshot(device, context, swapChain);
            release_com(context);
            release_com(device);
        }
        if (compose_fade(plan.worldFade, swapChain, quads[0])) {
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[0]);
        }
    }
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = g_predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount != 0 ? layers : nullptr;
    g_presentCall = "xrEndFrame";
    const XrResult result = g_api.endFrame(g_session, &endInfo);
    g_presentCall = nullptr;
    g_frameOpen = false;
    if (layerCount != 0) {
        save_layers(layers, layerCount);
    }
    main_frame_ended();
    if (XR_FAILED(result)) {
        // A refused frame is a black headset with no other trace.
        static std::uint32_t refused = 0;
        if (refused++ < 10) {
            log_fmt("ev=vr.xr end_frame result=%d layers=%u", static_cast<int>(result), layerCount);
        }
    }
    if (frames::tracing()) {
        // v0 = the ledger's draw sequence, v1 = 1 when the GPU named it, v2 = 1 when fresh.
        const float v[6] = {static_cast<float>(g_lastSource.sequence), g_lastSource.ledger ? 1.0F : 0.0F,
                            g_lastSource.fresh ? 1.0F : 0.0F, g_lastSource.stereo ? 1.0F : 0.0F, 0.0F, 0.0F};
        frames::note(frames::Kind::present, layerCount != 0 ? g_lastSource.serial : 0, v, g_lastSource.eye);
    }
    frames::present_tick();
    // The frame the game draws next uses the views located this frame.
    if (g_havePending) {
        g_renderedViews = g_pendingViews;
        g_renderedEye = g_pendingEye;
        g_renderedSerial = g_pendingSerial;
        g_haveRendered = true;
    }
    if ((g_frameIndex % kSubmitReportPeriod) == 0 || result != XR_SUCCESS) {
        // The written aspect goes out beside the FOV as proof the write landed (the engine does
        // not render with it, see the layer), and the vertical the layer really declared beside it.
        log_fmt("ev=vr.xr submit layers=%u result=%d vfov_declared=%.4f fov=%.4f aspect=%.4f",
                layerCount,
                static_cast<int>(result),
                2.0F * g_declaredHalfVertical.load(std::memory_order_relaxed),
                g_renderedFov.load(std::memory_order_relaxed),
                g_renderedAspect.load(std::memory_order_relaxed));
        AcquireSRWLockShared(&g_ledgerLock);
        const unsigned long long matches = g_drawnMatches;
        const unsigned long long misses = g_drawnMisses;
        ReleaseSRWLockShared(&g_ledgerLock);
        log_fmt("ev=vr.xr ledger stereo=%d ledger=%d eye=%d fresh=%d world_matches=%llu world_misses=%llu filled=%llu",
                g_lastSource.stereo ? 1 : 0, g_lastSource.ledger ? 1 : 0, g_lastSource.eye,
                g_lastSource.fresh ? 1 : 0, matches, misses,
                static_cast<unsigned long long>(g_filledFrames));
        const std::int64_t fixedLead = g_leadFixedNs.load(std::memory_order_relaxed);
        log_fmt("ev=vr.xr lead mode=%s lead_ms=%.2f shortfall_ms=%.2f period_ms=%.2f",
                fixedLead >= 0 ? "fixed" : "measured",
                static_cast<double>(fixedLead >= 0 ? fixedLead : g_leadNs.load()) * 1e-6,
                static_cast<double>(g_leadErrorNs.load()) * 1e-6, static_cast<double>(g_displayPeriod) * 1e-6);
    }
}

/** @return The most recently published pose. */
HeadPose head_pose() noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const HeadPose pose = g_pose;
    ReleaseSRWLockShared(&g_poseLock);
    return pose;
}

/** @return One controller's most recently located aim pose. */
HandPose hand_pose(bool rightHand) noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const HandPose hand = g_hands[rightHand ? 1 : 0];
    ReleaseSRWLockShared(&g_poseLock);
    return hand;
}

void frame_sample(HeadPose& head, HandPose& left, HandPose& right) noexcept {
    AcquireSRWLockShared(&g_poseLock);
    head = g_pose;
    left = g_hands[0];
    right = g_hands[1];
    ReleaseSRWLockShared(&g_poseLock);
}

/** @return The controllers as of the last sync. */
InputState input_state() noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const InputState input = g_input;
    ReleaseSRWLockShared(&g_poseLock);
    return input;
}

/** Records the horizontal FOV the engine actually rendered with this frame. */
void note_rendered_aspect(float aspect) noexcept {
    if (aspect > 0.1F && aspect < 10.0F) {
        g_renderedAspect.store(aspect, std::memory_order_relaxed);
    }
}

void note_rendered_fov(float horizontalFov) noexcept {
    g_renderedFov.store(horizontalFov, std::memory_order_relaxed);
}

float rendered_fov() noexcept {
    return g_renderedFov.load(std::memory_order_relaxed);
}

float origin_yaw() noexcept {
    return g_originYaw;
}

EyeGeometry eye_geometry() noexcept {
    AcquireSRWLockShared(&g_geometryLock);
    const EyeGeometry geometry = g_geometry;
    ReleaseSRWLockShared(&g_geometryLock);
    return geometry;
}

void set_stereo(bool alternate) noexcept {
    g_stereoMode.store(alternate ? 1 : 0, std::memory_order_relaxed);
}

void set_per_eye_crop(bool perEye) noexcept {
    g_perEyeCrop.store(perEye, std::memory_order_relaxed);
}

bool per_eye_crop() noexcept {
    return g_perEyeCrop.load(std::memory_order_relaxed);
}

void set_lead(float milliseconds) noexcept {
    g_leadFixedNs.store(milliseconds < 0.0F ? -1 : static_cast<std::int64_t>(milliseconds * 1.0e6F),
                        std::memory_order_relaxed);
}

float lead_ms() noexcept {
    const std::int64_t fixedLead = g_leadFixedNs.load(std::memory_order_relaxed);
    return static_cast<float>(fixedLead >= 0 ? fixedLead : g_leadNs.load(std::memory_order_relaxed)) * 1.0e-6F;
}

void set_declaration_probe(float yawDegrees, float fovScale) noexcept {
    g_probeYaw.store(yawDegrees * 0.0174532925F, std::memory_order_relaxed);
    g_probeFovScale.store(fovScale > 0.1F && fovScale < 10.0F ? fovScale : 1.0F, std::memory_order_relaxed);
}

bool stereo() noexcept {
    return g_stereoMode.load(std::memory_order_relaxed) != 0;
}

bool camera_frame(HeadPose& head, HandPose& left, HandPose& right) noexcept {
    AcquireSRWLockExclusive(&g_ledgerLock);
    AcquireSRWLockShared(&g_poseLock);
    head = g_pose;
    left = g_hands[0];
    right = g_hands[1];
    ReleaseSRWLockShared(&g_poseLock);
    if (!head.valid) {
        g_haveCameraDraw = false;
        ReleaseSRWLockExclusive(&g_ledgerLock);
        return false;
    }
    DrawRecord record{};
    record.serial = head.frame;
    const Publication& publication = g_publications[head.frame % kPublicationRing];
    if (publication.valid && publication.serial == head.frame) {
        record.views = publication.views;
        record.located = publication.located;
    }
    record.stereo = g_stereoMode.load(std::memory_order_relaxed) != 0;
    if (record.stereo) {
        // Every camera frame takes the other eye, whatever the publications did in between: what
        // alternates is what gets DRAWN, and the GPU says which one each image was (note_world_camera).
        record.eye = g_lastStereoEye == 1 ? 2 : 1;
        g_lastStereoEye = record.eye;
        head.offset = head.eyeOffsets[static_cast<std::size_t>(record.eye - 1)];
        head.eye = record.eye;
    } else {
        record.eye = head.eye;
    }
    record.head = head;
    record.hands = {left, right};
    g_cameraDraw = record;
    g_haveCameraDraw = true;
    ReleaseSRWLockExclusive(&g_ledgerLock);
    if (frames::tracing()) {
        // The instant the camera takes its sample, and the newest publication at that instant.
        const float v[6] = {static_cast<float>(g_frameIndex), 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
        frames::note(frames::Kind::mark, head.frame, v, record.eye);
    }
    return true;
}

void commit_camera(const Vector& position, const Vector& forward) noexcept {
    AcquireSRWLockExclusive(&g_ledgerLock);
    if (g_haveCameraDraw) {
        g_cameraDraw.cameraPosition = position;
        g_cameraDraw.cameraForward = forward;
        g_cameraDraw.sequence = ++g_drawSequence;
        g_cameraDraw.committed = true;
        g_draws[g_cameraDraw.sequence % kDrawRing] = g_cameraDraw;
    }
    ReleaseSRWLockExclusive(&g_ledgerLock);
}

bool camera_sample(HeadPose& head, HandPose& left, HandPose& right) noexcept {
    AcquireSRWLockShared(&g_ledgerLock);
    const bool have = g_haveCameraDraw;
    if (have) {
        head = g_cameraDraw.head;
        left = g_cameraDraw.hands[0];
        right = g_cameraDraw.hands[1];
    }
    ReleaseSRWLockShared(&g_ledgerLock);
    if (!have) {
        frame_sample(head, left, right);
    }
    return have;
}

void note_world_camera(const float* position, const float* forward) noexcept {
    // The newest record whose written camera this is. Exact up to float noise: the engine copies
    // the block into the view constants.
    constexpr float kPositionSlack = 1.0e-3F;
    constexpr float kForwardSlack = 1.0e-4F;
    // Several records can carry the same camera -- a head held still: the camera hook works a frame
    // ahead of the renderer, so the newest match is often the NEXT frame. Frames are drawn in order, so
    // the record drawn now is the OLDEST match not drawn yet; the newest only when nothing newer than
    // the last drawn matches (a repeat), or to resync if the oldest has fallen behind.
    constexpr std::uint64_t kResyncFrames = 3;
    AcquireSRWLockExclusive(&g_ledgerLock);
    const std::uint64_t lastDrawn = g_haveDrawn ? g_drawn.sequence : 0;
    const DrawRecord* newest = nullptr;
    const DrawRecord* oldestNew = nullptr;
    for (const DrawRecord& record : g_draws) {
        if (!record.committed) {
            continue;
        }
        bool same = true;
        for (std::size_t lane = 0; lane < 3 && same; ++lane) {
            same = std::fabs(record.cameraPosition[lane] - position[lane]) < kPositionSlack
                   && std::fabs(record.cameraForward[lane] - forward[lane]) < kForwardSlack;
        }
        if (!same) {
            continue;
        }
        if (newest == nullptr || record.sequence > newest->sequence) {
            newest = &record;
        }
        if (record.sequence > lastDrawn && (oldestNew == nullptr || record.sequence < oldestNew->sequence)) {
            oldestNew = &record;
        }
    }
    const DrawRecord* best = newest;
    if (oldestNew != nullptr && newest != nullptr && newest->sequence - oldestNew->sequence <= kResyncFrames) {
        best = oldestNew;
    }
    if (best != nullptr) {
        g_drawn = *best;
        g_haveDrawn = true;
        ++g_drawnMatches;
    } else {
        ++g_drawnMisses;
    }
    ReleaseSRWLockExclusive(&g_ledgerLock);
}

std::uint64_t camera_sequence() noexcept {
    AcquireSRWLockShared(&g_ledgerLock);
    const std::uint64_t sequence = g_haveCameraDraw && g_cameraDraw.committed ? g_cameraDraw.sequence : 0;
    ReleaseSRWLockShared(&g_ledgerLock);
    return sequence;
}

std::uint64_t drawn_sequence() noexcept {
    AcquireSRWLockShared(&g_ledgerLock);
    const std::uint64_t sequence = g_haveDrawn ? g_drawn.sequence : 0;
    ReleaseSRWLockShared(&g_ledgerLock);
    return sequence;
}

bool drawn_sample(HeadPose& head, HandPose& left, HandPose& right) noexcept {
    AcquireSRWLockShared(&g_ledgerLock);
    const bool have = g_haveDrawn;
    if (have) {
        head = g_drawn.head;
        left = g_drawn.hands[0];
        right = g_drawn.hands[1];
    }
    ReleaseSRWLockShared(&g_ledgerLock);
    if (!have) {
        frame_sample(head, left, right);
    }
    return have;
}

void haptic(bool rightHand, float amplitude, float seconds) noexcept {
    const std::size_t hand = rightHand ? 1 : 0;
    g_hapticSeconds[hand].store(seconds, std::memory_order_relaxed);
    g_hapticAmplitude[hand].store(amplitude, std::memory_order_release);
}

/** Tears the session and instance down and unloads the runtime. */
bool session_focused() noexcept {
    return g_sessionRunning && g_sessionState.load(std::memory_order_relaxed) == XR_SESSION_STATE_FOCUSED;
}

void drain_loader_log() noexcept {
    const std::uint32_t written = g_loaderWritten.load(std::memory_order_acquire);
    if (written - g_loaderRead > kLoaderRing) {
        log_fmt("ev=vr.xr dll dropped=%u", written - g_loaderRead - kLoaderRing);
        g_loaderRead = written - kLoaderRing;
    }
    for (; g_loaderRead != written; ++g_loaderRead) {
        const LoaderRecord& record = g_loaderRing[g_loaderRead % kLoaderRing];
        log_fmt("ev=vr.xr dll %s ours=%d forwarded=%d name=%s", record.reason == 1 ? "load" : "unload",
                record.ours ? 1 : 0, record.forwarded ? 1 : 0, record.name.data());
    }
}

/**
 * Switching VR off. Waits for nothing: with the init thread still working, or the frame thread
 * still inside a runtime call, it does nothing and says so -- the caller runs it every present while
 * off, so the teardown happens on the first present after they are free.
 * @return True once everything is down.
 */
bool shutdown() noexcept {
    if (static_cast<InitPhase>(g_initPhase.load(std::memory_order_acquire)) == InitPhase::working) {
        return false;
    }
    if (static_cast<InitPhase>(g_initPhase.load(std::memory_order_acquire)) == InitPhase::done) {
        if (g_initThread != nullptr) {
            WaitForSingleObject(g_initThread, INFINITE);
            CloseHandle(g_initThread);
            g_initThread = nullptr;
        }
        g_initPhase.store(static_cast<int>(InitPhase::idle), std::memory_order_release);
    }
    if (!stop_pump(50)) {
        return false;
    }
    const bool wasUp = g_status != Status::off || g_instance != XR_NULL_HANDLE;
    teardown(false);
    // A switch off and on is the player asking for VR again, after an EXITING too.
    g_exitHold = false;
    g_restartRequested = false;
    g_nextAttemptMs = 0;
    g_attempts = 0;
    return wasUp;
}

namespace {

void teardown(bool keepInstance) noexcept {
    const OwnLoads own;
    for (Surface* surface : {&g_panelSurface, &g_beamSurface, &g_fadeSurface, &g_backdropSurfaces[0], &g_backdropSurfaces[1]}) {
        destroy_surface(*surface);
    }
    if (g_viewSpace != XR_NULL_HANDLE && g_api.destroySpace != nullptr) {
        g_api.destroySpace(g_viewSpace);
    }
    g_viewSpace = XR_NULL_HANDLE;
    forget_snapshots();
    compositor::release();
    for (HandRaw& raw : g_handRaw) {
        raw = HandRaw{};
    }
    for (Presentation& presentation : g_presentations) {
        destroy_presentation(presentation);
    }
    g_shown = {};
    g_lastSource = FrameSource{};
    AcquireSRWLockExclusive(&g_ledgerLock);
    g_publications = {};
    g_draws = {};
    g_haveCameraDraw = false;
    g_haveDrawn = false;
    ReleaseSRWLockExclusive(&g_ledgerLock);
    if (g_api.destroySpace != nullptr) {
        for (XrSpace* space : {&g_actions.spaceL, &g_actions.spaceR, &g_actions.palmSpaceL,
                               &g_actions.palmSpaceR}) {
            if (*space != XR_NULL_HANDLE) {
                g_api.destroySpace(*space);
                *space = XR_NULL_HANDLE;
            }
        }
    }
    if (g_actions.set != XR_NULL_HANDLE && g_api.destroyActionSet != nullptr) {
        g_api.destroyActionSet(g_actions.set);
    }
    g_actions = Actions{};
    g_havePending = false;
    g_haveRendered = false;
    g_shouldRender = false;
    AcquireSRWLockExclusive(&g_poseLock);
    g_input = InputState{};
    g_pose = HeadPose{};
    g_hands = std::array<HandPose, 2>{};
    ReleaseSRWLockExclusive(&g_poseLock);
    if (g_session != XR_NULL_HANDLE) {
        // Destroyed directly, whatever its state: xrDestroySession is valid in any of them, while
        // xrEndSession is only valid once the runtime has said STOPPING.
        if (g_space != XR_NULL_HANDLE && g_api.destroySpace != nullptr) {
            g_api.destroySpace(g_space);
        }
        if (g_api.destroySession != nullptr) {
            g_api.destroySession(g_session);
        }
    }
    g_session = XR_NULL_HANDLE;
    g_space = XR_NULL_HANDLE;
    g_sessionRunning = false;
    g_sessionState = XR_SESSION_STATE_UNKNOWN;
    g_frameOpen = false;
    g_status = Status::off;
    g_haveOrigin = false;
    g_recentreRequested = true;
    g_runtimeRecentre = false;
    if (keepInstance) {
        return;
    }
    if (g_instance != XR_NULL_HANDLE && g_api.destroyInstance != nullptr) {
        g_api.destroyInstance(g_instance);
    }
    // The runtime's library stays mapped: its own threads may outlive the instance, and the next
    // attempt maps the same module again (the loader only counts a reference).
    g_api = Api{};
    g_instance = XR_NULL_HANDLE;
    g_system = XR_NULL_SYSTEM_ID;
}

} // namespace

} // namespace sunrise::client::hooks::vr::xr
