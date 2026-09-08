/**
 * Camera probe for the VR module.
 *
 * The engine recomputes the camera pose every frame and the camera-frame hook is the only site
 * that reaches the pose block, so the probe writes there, after the engine is done. Everything it
 * applies is a delta on top of what the engine produced: the probe never needs to know where the
 * player is, and switching it off restores the stock view with no state to unwind.
 *
 * This is the F0.b gate. It answers one question -- does the renderer honour a pose written at
 * this site, or does something later in the frame recompute it -- and it answers it with offsets
 * big enough to be unmistakable in a screenshot.
 */

#include "runtime.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../input/window_focus.h"
#include "../teleport/runtime.h"
#include "vr_destination.h"
#include "vr_gamepad.h"
#include "vr_watch.h"
#include "vr_weapon.h"
#include "xr_runtime.h"

struct IDXGISwapChain;

namespace sunrise::client::hooks::vr {
namespace {

/**
 * Field offsets inside one player camera block. Duplicated from the teleport module's private
 * internal.h on purpose: the target build is pinned to a fixed depot manifest, so these cannot
 * drift, and copying five numbers keeps this module from reaching into another module's privates.
 */
constexpr std::size_t kCameraPositionX = 0x594;
constexpr std::size_t kCameraForwardX = kCameraPositionX + 0x28;
constexpr std::size_t kCameraUpX = kCameraPositionX + 0x34;
constexpr std::size_t kCameraHorizontalFov = kCameraPositionX + 0x40;
constexpr std::size_t kCameraAspect = kCameraPositionX + 0xB8;

/** The vertical lane. The basis is X forward, Z up, so height is the third lane. */
constexpr std::size_t kVerticalLane = 2;

/** The high bit of a polled key state marks it held. */
constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);

constexpr int kToggleKey = VK_F9;
constexpr int kYawKey = VK_F10;
constexpr int kHeightKey = VK_F11;
constexpr int kResetKey = VK_F8;
constexpr int kRecentreKey = VK_F7;

/** One press of the yaw key, in degrees. Four presses make a full turn. */
constexpr float kYawStep = 45.0F;
/** One press of the height key, in game units. */
constexpr float kHeightStep = 1.0F;
/** Height wraps rather than growing without bound, so the probe cannot be lost above the map. */
constexpr float kHeightLimit = 8.0F;

constexpr float kPi = 3.14159265358979F;
constexpr float kDegreesToRadians = kPi / 180.0F;

/** Frames between throttled pose reports. About three seconds at sixty frames. */
constexpr std::uint32_t kReportPeriod = 180;

std::atomic_bool g_enabled{false};
std::atomic<float> g_yawDegrees{0.0F};
std::atomic<float> g_height{0.0F};
std::atomic<float> g_fovDegrees{0.0F};

/** Held state of each probe key on the previous frame, so a switch flips only on the press. */
std::array<bool, 5> g_keyDown{};

/** The camera thread owns both of these; they are published for the report only. */
Pose g_lastRead{};
Pose g_lastWritten{};
/** This frame's engine pose and the head pose composed from it, for the consumers the F3 probe redirects. */
Pose g_enginePose{};
Pose g_headPose{};
SRWLOCK g_poseLock{SRWLOCK_INIT};

std::uint32_t g_frame{0};
/** Presents seen, for the throttled pad report. Present thread only. */
std::uint32_t g_presentCount{0};

/**
 * Yaw of the engine's own camera, published by the camera hook for the present thread to fold
 * into the head pose. Without it the head would point wherever the room does and mouse look
 * would stop turning the view.
 */
std::atomic<float> g_bodyYaw{0.0F};

/** Set while a valid head pose is what the camera is being driven from. */
std::atomic_bool g_headTracking{false};

/**
 * Reads one value out of game memory without faulting on a torn pointer.
 * @param address Source address.
 * @param value Receives the value.
 * @return True when Windows copied the whole value.
 */
template <typename T> [[nodiscard]] bool read_at(const std::byte* address, T& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE
           && read == sizeof value;
}

/**
 * Writes one value into game memory without faulting on a torn pointer.
 * @param address Destination address.
 * @param value Value to store.
 * @return True when Windows copied the whole value.
 */
template <typename T> [[nodiscard]] bool write_at(std::byte* address, const T& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &written) != FALSE
           && written == sizeof value;
}

/** @return True when a probe key went down this frame, and latches its held state. */
[[nodiscard]] bool pressed(std::size_t slot, int virtualKey) noexcept {
    const bool down = (GetAsyncKeyState(virtualKey) & kKeyHeldBit) != 0;
    const bool edge = down && !g_keyDown[slot];
    g_keyDown[slot] = down;
    return edge;
}

/**
 * Turns one vector about the up axis.
 * @param vector Vector to turn, in the game's basis.
 * @param sine Sine of the angle.
 * @param cosine Cosine of the angle.
 */
void rotate_about_up(Vector& vector, float sine, float cosine) noexcept {
    const float x = vector[0];
    const float y = vector[1];
    vector[0] = x * cosine - y * sine;
    vector[1] = x * sine + y * cosine;
}

/** @return True when every lane of a pose is a finite number. */
[[nodiscard]] bool finite(const Pose& pose) noexcept {
    for (std::size_t lane = 0; lane < pose.position.size(); ++lane) {
        if (!std::isfinite(pose.position[lane]) || !std::isfinite(pose.forward[lane])
            || !std::isfinite(pose.up[lane])) {
            return false;
        }
    }
    return std::isfinite(pose.horizontalFov) && std::isfinite(pose.aspect);
}

/** Emits one line on the client channel at a level the stock thresholds admit. */
void report(const char* event) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, event);
}

/** Emits the probe's switch and deltas. */
void report_state() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=vr.probe state on=%d yaw=%.1f height=%.2f fov=%.1f",
                                      g_enabled.load(std::memory_order_relaxed) ? 1 : 0,
                                      g_yawDegrees.load(std::memory_order_relaxed),
                                      g_height.load(std::memory_order_relaxed),
                                      g_fovDegrees.load(std::memory_order_relaxed));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Emits the pose the engine produced and the pose read back out of memory after the write.
 * The read-back is the proof that the store landed; the screenshot is the proof that the
 * renderer honoured it.
 */
void report_pose(const Pose& before, const Pose& after) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=vr.probe pose in_pos=%.2f,%.2f,%.2f in_fwd=%.3f,%.3f,%.3f "
                      "in_fov=%.4f out_pos=%.2f,%.2f,%.2f out_fwd=%.3f,%.3f,%.3f out_fov=%.4f",
                      before.position[0],
                      before.position[1],
                      before.position[2],
                      before.forward[0],
                      before.forward[1],
                      before.forward[2],
                      before.horizontalFov,
                      after.position[0],
                      after.position[1],
                      after.position[2],
                      after.forward[0],
                      after.forward[1],
                      after.forward[2],
                      after.horizontalFov);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** @return True while the probe is overwriting the camera pose. */
bool enabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}

/** @return The deltas currently applied. */
Offsets offsets() noexcept {
    return Offsets{g_yawDegrees.load(std::memory_order_relaxed),
                   g_height.load(std::memory_order_relaxed),
                   g_fovDegrees.load(std::memory_order_relaxed)};
}

/** @return The last pose read before the probe wrote over it. */
Pose last_read() noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const Pose pose = g_lastRead;
    ReleaseSRWLockShared(&g_poseLock);
    return pose;
}

/** @return The last pose read back from game memory after the probe wrote it. */
Pose last_written() noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const Pose pose = g_lastWritten;
    ReleaseSRWLockShared(&g_poseLock);
    return pose;
}

Pose engine_pose() noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const Pose pose = g_enginePose;
    ReleaseSRWLockShared(&g_poseLock);
    return pose;
}

Pose head_pose() noexcept {
    AcquireSRWLockShared(&g_poseLock);
    const Pose pose = g_headPose;
    ReleaseSRWLockShared(&g_poseLock);
    return pose;
}

/** Clears the switch and every delta. */
void reset() noexcept {
    g_enabled.store(false, std::memory_order_relaxed);
    g_yawDegrees.store(0.0F, std::memory_order_relaxed);
    g_height.store(0.0F, std::memory_order_relaxed);
    g_fovDegrees.store(0.0F, std::memory_order_relaxed);
}

/** Reads the probe's keys once a frame and updates the switch and the deltas. */
void poll_keys() noexcept {
    // The access watch and the F3 probe are driven by files, so they run whatever the focus is.
    watch::tick();
    weapon::tick();
    destination::tick();
    // An open interface owns the keyboard, and a key pressed at another window is not ours.
    if (!client::input::game_focused() || core::ui::runtime::snapshot().visible) {
        g_keyDown.fill(false);
        return;
    }
    bool changed = false;
    if (pressed(0, kToggleKey)) {
        const bool next = !g_enabled.load(std::memory_order_relaxed);
        g_enabled.store(next, std::memory_order_relaxed);
        changed = true;
    }
    if (pressed(1, kYawKey)) {
        float yaw = g_yawDegrees.load(std::memory_order_relaxed) + kYawStep;
        if (yaw >= 360.0F) {
            yaw -= 360.0F;
        }
        g_yawDegrees.store(yaw, std::memory_order_relaxed);
        changed = true;
    }
    if (pressed(2, kHeightKey)) {
        float height = g_height.load(std::memory_order_relaxed) + kHeightStep;
        if (height > kHeightLimit) {
            height = 0.0F;
        }
        g_height.store(height, std::memory_order_relaxed);
        changed = true;
    }
    if (pressed(3, kResetKey)) {
        reset();
        changed = true;
    }
    if (pressed(4, kRecentreKey)) {
        xr::recentre();
        report("ev=vr.probe recentre requested");
    }
    if (changed) {
        report_state();
    }
}

/** @return True while the head pose, rather than the manual deltas, is driving the camera. */
bool head_tracking() noexcept {
    return g_headTracking.load(std::memory_order_relaxed);
}

/** Drives the OpenXR frame from the present hook. */
void present_frame(void* device, void* swapChain) noexcept {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        gamepad::release_all();
        return;
    }
    if (!xr::initialize(static_cast<ID3D11Device*>(device))) {
        gamepad::release_all();
        return;
    }
    // The engine's yaw comes from the camera hook, a frame behind. Turning the body is slow
    // enough relative to a frame that folding in last frame's yaw is not visible.
    xr::begin_frame(g_bodyYaw.load(std::memory_order_relaxed));
    xr::end_frame(static_cast<IDXGISwapChain*>(swapChain));
    // The controllers drive the keyboard and mouse only while the session produces input.
    gamepad::pump(xr::active());
    if ((++g_presentCount % kReportPeriod) == 0) {
        const gamepad::Stats stats = gamepad::stats();
        const xr::InputState input = xr::input_state();
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=vr.pad stats injected=%llu held=%u mouse=%lld,%lld "
                                          "move=%.2f,%.2f turn=%.2f,%.2f trig=%.2f,%.2f buttons=0x%X",
                                          static_cast<unsigned long long>(stats.injected),
                                          stats.heldKeys,
                                          static_cast<long long>(stats.mouseCountsX),
                                          static_cast<long long>(stats.mouseCountsY),
                                          input.moveX,
                                          input.moveY,
                                          input.turnX,
                                          input.turnY,
                                          input.triggerL,
                                          input.triggerR,
                                          input.buttons);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Lays the deltas over the camera pose of one player. */
void apply_camera(std::uint32_t playerIndex) noexcept {
    ++g_frame;
    const bool due = (g_frame % kReportPeriod) == 0;
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    std::byte* const block = teleport::camera_block(playerIndex);
    // The block's address is what every out-of-process diagnostic starts from, so publish it once
    // per change instead of leaving the harness to scan for it.
    static std::byte* lastBlock = nullptr;
    if (block != lastBlock) {
        lastBlock = block;
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(), line.size(), "ev=vr.probe block player=%u addr=0x%llX",
                                          static_cast<unsigned>(playerIndex),
                                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(block)));
        if (written > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    if (block == nullptr) {
        if (due) {
            report("ev=vr.probe pose result=fail reason=no_block");
        }
        return;
    }
    Pose pose{};
    if (!read_at(block + kCameraPositionX, pose.position)
        || !read_at(block + kCameraForwardX, pose.forward)
        || !read_at(block + kCameraUpX, pose.up)
        || !read_at(block + kCameraHorizontalFov, pose.horizontalFov)
        || !read_at(block + kCameraAspect, pose.aspect)) {
        if (due) {
            report("ev=vr.probe pose result=fail reason=read");
        }
        return;
    }
    if (!finite(pose)) {
        if (due) {
            report("ev=vr.probe pose result=fail reason=not_finite");
        }
        return;
    }
    // Publish the engine's own yaw for the present thread, which needs it before it can fold the
    // head pose into the body's facing.
    g_bodyYaw.store(std::atan2(pose.forward[1], pose.forward[0]), std::memory_order_relaxed);

    Pose next = pose;
    const xr::HeadPose head = xr::head_pose();
    const bool tracking = head.valid && xr::active();
    g_headTracking.store(tracking, std::memory_order_relaxed);
    if (tracking) {
        // The head replaces the engine's orientation outright: the pose already carries the body
        // yaw, so what is left here is the head's own rotation and its travel from the origin.
        next.forward = head.forward;
        next.up = head.up;
        for (std::size_t lane = 0; lane < next.position.size(); ++lane) {
            next.position[lane] += head.offset[lane];
        }
        if (head.horizontalFov > 0.0F && head.horizontalFov < kPi) {
            next.horizontalFov = head.horizontalFov;
        }
        if (head.aspect > 0.0F) {
            next.aspect = head.aspect;
        }
    }

    // The manual deltas stay live on top, as trim while tracking and as the whole story without it.
    const float yaw = g_yawDegrees.load(std::memory_order_relaxed) * kDegreesToRadians;
    if (yaw != 0.0F) {
        const float sine = std::sin(yaw);
        const float cosine = std::cos(yaw);
        rotate_about_up(next.forward, sine, cosine);
        rotate_about_up(next.up, sine, cosine);
    }
    next.position[kVerticalLane] += g_height.load(std::memory_order_relaxed);
    const float fovDelta = g_fovDegrees.load(std::memory_order_relaxed) * kDegreesToRadians;
    if (fovDelta != 0.0F) {
        const float fov = next.horizontalFov + fovDelta;
        // Keep the frustum a frustum: a non-positive or reflex angle would poison every matrix
        // the engine builds from it for the rest of the frame.
        if (fov > 0.0F && fov < kPi) {
            next.horizontalFov = fov;
        }
    }
    AcquireSRWLockExclusive(&g_poseLock);
    g_enginePose = pose;
    g_headPose = next;
    ReleaseSRWLockExclusive(&g_poseLock);
    // With the block's orientation left to the engine, the head reaches the render view through
    // the F3 probe's detours instead, and whatever else reads the block keeps the body's facing.
    const bool orientation = weapon::block_orientation_enabled();
    const bool stored = write_at(block + kCameraPositionX, orientation ? next.position : pose.position)
                        && write_at(block + kCameraForwardX, orientation ? next.forward : pose.forward)
                        && write_at(block + kCameraUpX, orientation ? next.up : pose.up)
                        && write_at(block + kCameraHorizontalFov, next.horizontalFov);
    if (!stored) {
        if (due) {
            report("ev=vr.probe pose result=fail reason=write");
        }
        return;
    }
    // The layer must declare the frustum the engine really renders with, so the FOV is read back
    // every frame: if the engine clamps the value, the readback carries the clamped one.
    float renderedFov = 0.0F;
    if (read_at(block + kCameraHorizontalFov, renderedFov) && std::isfinite(renderedFov)) {
        xr::note_rendered_fov(renderedFov);
    }
    if (!due) {
        return;
    }
    // Read the block back rather than reporting what we meant to store: this is the only way to
    // tell a store that landed from one the engine refused.
    Pose readBack{};
    if (!read_at(block + kCameraPositionX, readBack.position)
        || !read_at(block + kCameraForwardX, readBack.forward)
        || !read_at(block + kCameraUpX, readBack.up)
        || !read_at(block + kCameraHorizontalFov, readBack.horizontalFov)) {
        report("ev=vr.probe pose result=fail reason=readback");
        return;
    }
    AcquireSRWLockExclusive(&g_poseLock);
    g_lastRead = pose;
    g_lastWritten = readBack;
    ReleaseSRWLockExclusive(&g_poseLock);
    report_pose(pose, readBack);
}

} // namespace sunrise::client::hooks::vr
