#pragma once

/**
 * The Guardian's own body in third person (docs/SWORD-SPARROW.md): what of it is drawn, and where.
 *
 * With a sword, in the Tower or on a sparrow the engine draws the character with world-space bone
 * palettes, one long-lived constant buffer per armour piece (5120 bytes) or object (1024), the same
 * layout as the viewmodel's (header rows 4-5, a 3x4 per bone from row 8). Every piece of the Guardian is
 * a prefix of one 72-bone skeleton rooted at its feet -- the skeleton the first-person arms use -- so:
 *
 *  - the gloves and gauntlets are posed by the arm IK (vr_arms.cpp rewrite_free), made camera-relative
 *    against the very camera of their frame (vr_mode.cpp records, found by the palette's own root);
 *  - every other piece (helmet, chest, cloak, legs, class item) is collapsed to nothing: no body;
 *  - the sword (two rigid one-bone palettes beside the game's hand) is moved into the VR hand, keeping
 *    the game's own hold on its hilt; the weapon stowed on the back is collapsed;
 *  - on a sparrow nothing of the vehicle is touched; the rider's hands on it give the handlebars.
 *
 * Everything is rewritten in place at Unmap, so every pass (G-buffer, shadows, glow) agrees.
 */

#include <cstddef>
#include <cstdint>

struct ID3D11Resource;

namespace sunrise::client::hooks::vr::body {

/** @return True while world palettes should be looked at (a third-person mode in the world). */
[[nodiscard]] bool wanted() noexcept;

/** Render thread, at the Unmap of a dynamic constant buffer of `bytes` that may be a world palette. */
void examine(ID3D11Resource* resource, float* data, std::size_t bytes) noexcept;

/** Any thread: a Map of a buffer the size of a world palette (research counter). */
void note_map(unsigned rowPitch) noexcept;

/**
 * Whether a 128-byte object constant buffer (identity world matrix in rows 0-3, a mesh header in rows
 * 4-5) belongs to one of the Guardian's pieces hidden this frame: the cloth a cloak simulates on the CPU
 * and writes in world space (a dynamic vertex buffer, no bones to collapse) carries its cloak's header
 * there (2026-10-18 capture). Its draw is then skipped. Any thread.
 */
[[nodiscard]] bool hidden_cloth(const float* rows) noexcept;

/**
 * Whether `palette` holds an arm piece posed this frame. The arm pieces also carry RIGID sub-meshes (no
 * blend weights) riding body bones the IK leaves where the hidden body has them -- the undersuit's neckline
 * just under the eye, drawn as two dark arcs on the ground (2026-10-18) -- which the first-person viewmodel
 * never draws: vr_gpu.cpp skips those draws. Any thread.
 */
[[nodiscard]] bool posed_arm_buffer(const void* palette) noexcept;

/** Render thread, at a draw: the palette buffer bound at slot 11 and whether the mesh is an arm mesh. */
void note_draw(ID3D11Resource* palette, bool armMesh) noexcept;

/** Present thread, once per present: handlebars, haptics, commands, the report. */
void present() noexcept;

/** Which hand holds the sword: 0 left, 1 right. Any thread. */
[[nodiscard]] int sword_hand() noexcept;
void set_sword_hand(int hand) noexcept;

/** @return True while the free hand holds the sword's hilt too: its grip is the hold, nothing else. */
[[nodiscard]] bool two_handed() noexcept;

/** @return True while a sword is in hand (seen in the last half second). */
[[nodiscard]] bool sword_held() noexcept;

/** The handlebar steering this frame (-1 left .. 1 right), and whether any hand is on a handlebar. */
[[nodiscard]] float handlebar_steer(bool& held) noexcept;

} // namespace sunrise::client::hooks::vr::body
