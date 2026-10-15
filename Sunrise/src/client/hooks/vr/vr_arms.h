#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace sunrise::client::hooks::vr::arms {

/**
 * One arm mesh, decoded from its vertex buffer into bind space. Every arm mesh shares the one
 * skeleton, so their bone indices agree and their vertices add up to one picture of the rig.
 * Called on the present thread.
 * @param key The vertex buffer, only to tell meshes apart.
 * @param count Vertices.
 * @param positions Bind-space positions, three floats per vertex.
 * @param weights Four skin weights per vertex.
 * @param indices Four bone indices per vertex.
 */
void add_mesh(std::uintptr_t key, std::size_t count, const float* positions, const float* weights,
              const std::uint8_t* indices) noexcept;

/**
 * The game's own arm palette for this frame, as it Unmapped it: the material the joints are
 * learned from. Called on the render thread; cheap, and throttled inside.
 * @param bones Bone 0's first row (12 floats per bone).
 * @param boneCount Rigid bones in the palette.
 */
void observe(const float* bones, std::size_t boneCount) noexcept;

/** @return True when rewrite() would change anything: rig known, VR running, arms enabled. */
[[nodiscard]] bool ready() noexcept;

/**
 * Rewrites one arm palette so the arms reach for the controllers.
 * @param bones The game's palette, bone 0's first row.
 * @param boneCount Bones in it.
 * @param out Receives the rewritten bones, same layout.
 * @return False when nothing was written; the draw then keeps the game's palette.
 */
[[nodiscard]] bool rewrite(const float* bones, std::size_t boneCount, float* out) noexcept;

/** Once per present: commands, the periodic report, and the verification overlay. */
void present(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) noexcept;

/**
 * The two-handed hold, once per OpenXR frame, after the actions were synced and before the
 * controllers are turned into keys: decides what the left grip means this frame (take hold of the
 * weapon, keep holding it, or throw a grenade), ramps the hold in and out, publishes it to the
 * weapon module and pulses the left controller on taking hold. Present thread.
 */
void update_hold() noexcept;

/**
 * @return True while the left grip is being held as a GRENADE press: squeezed away from the
 *         weapon, or with nothing to hold. A squeeze on (or on its way to) the handguard is the
 *         hold's, and never reaches the game.
 */
[[nodiscard]] bool grenade() noexcept;

/**
 * Whether a left-grip squeeze away from the weapon throws a grenade (true, the `ads mode button`
 * mapping) or waits for the hand to reach the handguard and takes hold there (false, the default:
 * the grenade is on the left trigger, and the grip is only the hold).
 */
void set_grip_grenade(bool on) noexcept;

/** @return True while the left hand holds the weapon, or is still letting go of it. Any thread. */
[[nodiscard]] bool holding() noexcept;

} // namespace sunrise::client::hooks::vr::arms
