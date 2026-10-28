#pragma once

/**
 * The module's own drawing for what the headset shows besides the game's frame: the virtual
 * screen's panel, the room around it, the pointer's beam, a fade. Plain D3D11 passes on the game's
 * immediate context, each inside a device-context state of its own (the game's is swapped out and
 * back in untouched). Nothing here knows OpenXR: the targets are whatever textures it is handed.
 * Ported from dishonored-vr's XRSession passes (panel, backdrop), which a Quest 3 validated.
 */

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace sunrise::client::hooks::vr::compositor {

/** A target texture and the format to render into it with (an sRGB format encodes on write). */
struct Target final {
    ID3D11Texture2D* texture{};
    std::uint32_t width{};
    std::uint32_t height{};
    /** DXGI_FORMAT of the view to render through. */
    std::uint32_t format{};
};

/** One panel: a crop of a source picture, shaped and faded. */
struct PanelDraw final {
    /** The picture: a texture a shader may not read (a back buffer) is copied first. */
    ID3D11Texture2D* source{};
    /** x, y, width, height in source pixels. */
    float crop[4]{};
    float alpha{1.0F};
    /** Corner radius as a fraction of the panel's height. */
    float corner{0.018F};
    /** The panel's width over its height, for the corners. */
    float aspect{1.6F};
    /** The pointer's dot, in panel UV; alpha 0 = none. */
    float pointerU{};
    float pointerV{};
    float pointerAlpha{};
    /** Radius as a fraction of the panel's height. */
    float pointerRadius{0.008F};
    /** Darkens the picture (a dimmed snapshot): 1 = as is. */
    float brightness{1.0F};
    float desaturate{};
};

/** The room around the screen: dark, room-locked, the frozen game where it was seen. */
struct BackdropDraw final {
    /** The eye this image is for, its orientation in the room and its FOV tangents (l r u d). */
    float eyeOrientation[4]{0, 0, 0, 1};
    float eyeTangents[4]{-1, 1, 1, -1};
    /** The frozen picture (null: none), its orientation and tangents when it was seen. */
    ID3D11ShaderResourceView* snapshot{};
    float snapshotOrientation[4]{0, 0, 0, 1};
    float snapshotTangents[4]{-1, 1, 1, -1};
    float snapshotBrightness{};
    float desaturate{};
    float environment{1.0F};
    float fade{1.0F};
    /** Direction to the screen (unit, room) and how much it lights its surroundings. */
    float glowDirection[3]{0, 0, -1};
    float glow{};
};

/** A soft-edged solid: the beam (a gradient along its length) or a fade (uniform). */
struct SolidDraw final {
    float colour[4]{0, 0, 0, 1};
    /** 1 = fade out along U (a beam that thins into the room), 0 = uniform. */
    float gradient{};
    /** Soft edge across V, fraction of the height (0 = hard). */
    float softEdge{};
};

[[nodiscard]] bool draw_panel(ID3D11Device* device, ID3D11DeviceContext* context, const PanelDraw& panel,
                              const Target& target) noexcept;
[[nodiscard]] bool draw_backdrop(ID3D11Device* device, ID3D11DeviceContext* context, const BackdropDraw& backdrop,
                                 const Target& target) noexcept;
[[nodiscard]] bool draw_solid(ID3D11Device* device, ID3D11DeviceContext* context, const SolidDraw& solid,
                              const Target& target) noexcept;

/**
 * A copy of `source` (any RGBA8/BGRA8 texture, a back buffer included) a shader can read as sRGB,
 * held in `slot` (0-3) and rebuilt when the size changes.
 * @return Its view, or null.
 */
[[nodiscard]] ID3D11ShaderResourceView* snapshot(ID3D11Device* device, ID3D11DeviceContext* context,
                                                 ID3D11Texture2D* source, unsigned slot) noexcept;

/** Releases everything (VR off, device lost). */
void release() noexcept;

} // namespace sunrise::client::hooks::vr::compositor
