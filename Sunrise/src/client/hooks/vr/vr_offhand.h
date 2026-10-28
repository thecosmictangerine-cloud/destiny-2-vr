#pragma once

#include <cstddef>
#include <cstdint>

#include "vr_math.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace sunrise::client::hooks::vr::offhand {

/**
 * The off-hand weapon's image (docs/DUAL-WIELD.md).
 *
 * The game draws one weapon: the one in hand. A second one is drawn here, by the GPU, from a
 * RECORDING of that weapon's own draws, taken while it was the game's weapon in the right hand and at
 * rest: every draw that skins the weapon's bone palette, in every pass it appears in (the depth
 * prepass and the G-buffer), with its shaders, states, textures, vertex and index data and per-object
 * constants -- the resources copied into ours, so nothing the engine later frees or reuses can change
 * it. Replayed each frame right after the live weapon's (or the arms') first draw of the same pass,
 * with the pass's own render targets and per-view constants and the bone palette moved onto the left
 * controller: the gun as Bungie draws it, lit by the game's own lighting, in the other hand.
 *
 * Everything here runs on the render thread, called from vr_gpu.cpp's D3D11 detours, except the
 * public calls documented otherwise.
 */

/**
 * vr_gpu.cpp, at every Unmap of the bone-palette buffer (constant buffer 11's resource): what it now
 * holds. `bones` points at bone 0 (12 floats per bone), `header` at row 0 (the 8 header rows).
 * `weapon` is true for a small camera-relative palette, the weapon's; arm palettes are not passed.
 */
void note_palette(ID3D11Resource* resource, const float* header, const float* bones, std::size_t boneCount,
                  bool weapon) noexcept;

/** vr_gpu.cpp, before every draw on the immediate context: records it when a recording is due. */
void before_draw(ID3D11DeviceContext* context, int kind, unsigned count, unsigned instances, unsigned start,
                 int base) noexcept;

/** vr_gpu.cpp, after every draw on the immediate context: replays the off-hand weapon when this is its place. */
void after_draw(ID3D11DeviceContext* context) noexcept;

/** vr_gpu.cpp, at present: frame bookkeeping, deferred resource work, the report. */
void present(ID3D11Device* device) noexcept;

/** @return True while an arm palette (the arms' draws) is what constant buffer 11 holds. vr_gpu.cpp. */
void note_arm_palette(ID3D11Resource* resource) noexcept;

// --- Driven by the dual-wield module (any thread) --------------------------------------------

/**
 * Asks for the weapon in the right hand to be recorded as `key` (the engine weapon object), at the
 * next frame it is drawn at rest. A recording already held for `key` is kept unless `force`.
 */
void request_recording(std::uintptr_t key, bool force) noexcept;

/** @return True when a complete recording of `key` is held. */
[[nodiscard]] bool recorded(std::uintptr_t key) noexcept;

/**
 * Forgets every recording: a world was entered or left, and its weapons are other objects (an old
 * key may even come back as a new weapon). `recorded` answers false from now on; the recordings are
 * freed at the next present, on the render thread that owns them.
 */
void forget_all() noexcept;

/**
 * Forgets the recording of `key` alone: that object left the loadout (a weapon swapped in the
 * inventory is replaced by a new object, and the old address may be reused by anything). `recorded`
 * answers false for it at once; its resources are freed at the next present.
 */
void forget(std::uintptr_t key) noexcept;

/** What the left hand shows, each frame: which recording, and where. */
struct Placement final {
    /** The recording to draw (0: nothing in the left hand). */
    std::uintptr_t key{};
    /**
     * The gun's root, camera-relative in the game's axes, for the frame being drawn, is computed here
     * from the left controller of that frame; this is an extra rigid motion on top, in the left
     * controller's frame (forward, LEFT, up; origin at the grip): recoil kick and reload tilt.
     */
    math::Affine kick{};
    /** False hides the off-hand weapon (dual wield off, or no recording yet). */
    bool visible{};
};
void set_placement(const Placement& placement) noexcept;

/**
 * Where the recorded weapon `key` sits relative to the RIGHT controller (controller frame: forward,
 * LEFT, up, origin at the grip pose), as recorded: its root bone, and the right hand on it. The
 * left-hand placement is the mirror of these (vr_arms.cpp places the left hand the same way).
 * @return False when `key` is not recorded.
 */
[[nodiscard]] bool recorded_hold(std::uintptr_t key, math::Affine& gunInController, math::Affine& handInController) noexcept;

/**
 * The left gun's frame for the frame being drawn now, camera-relative (game axes): its root bone,
 * mirrored onto the left controller. vr_aim.cpp fires the left weapon from it. Any thread.
 * @return False when nothing is in the left hand.
 */
[[nodiscard]] bool left_gun(math::Affine& root, std::uintptr_t* key = nullptr) noexcept;

} // namespace sunrise::client::hooks::vr::offhand
