#pragma once

#include <cstdint>

namespace sunrise::client::hooks::vr::frames {

/**
 * The frame ledger's trace: who used which head pose, on which thread, when.
 *
 * Several consumers turn the head pose into what the engine draws -- the camera block, the weapon's
 * transform, the arms' palettes -- on different threads and at different moments of a frame, and
 * the layer declaring the image to the headset runs on yet another. When they disagree about which
 * pose a frame was drawn with, the error is invisible on the desktop and shows in the headset as
 * swim (mono) or as a wrong-eye or IPD-sized jump (alternating eyes). The trace measures it instead
 * of assuming it: every consumer notes the publication serial it used, and the GPU notes the camera
 * the world was really rendered with.
 *
 * `SVR_FrameCmd.txt`: `trace <presents>` records that many presents' worth of events and writes
 * `SVR_FrameTrace.csv` beside the game. Costs one branch per note while not tracing.
 */
enum class Kind : std::uint8_t {
    /** begin_frame published a pose. serial = its frame index. */
    publish = 0,
    /** The camera hook wrote the block. serial = pose used; v = written position, forward. */
    camera = 1,
    /** The weapon's getter sampled the head. serial = pose used; v = head offset. */
    weapon = 2,
    /** The weapon's transform detour placed the gun. serial = pose used. */
    xform = 3,
    /** The arms' rewrite sampled the head. serial = pose used. */
    arms = 4,
    /** The world's view constants reached the GPU. v = camera position (row 7), forward (-row 6). */
    worldView = 5,
    /** end_frame submitted a layer. serial = pose declared; eye = which eye(s). */
    present = 6,
    /** The viewmodel's view constants reached the GPU. */
    vmView = 7,
    /** Free-form marker. */
    mark = 8,
};

/** Records one event while a trace is running. Any thread. */
void note(Kind kind, std::uint64_t serial, const float* v6 = nullptr, int eye = -1) noexcept;

/** @return True while a trace is recording. */
[[nodiscard]] bool tracing() noexcept;

/** Present thread, once per present: counts the trace window down and writes the file at its end. */
void present_tick() noexcept;

/** Starts a trace of `presents` presents. Any thread. */
void start_trace(std::uint32_t presents) noexcept;

} // namespace sunrise::client::hooks::vr::frames
