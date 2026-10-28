#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "vr_math.h"
#include "xr_runtime.h"

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

/**
 * Keeps the right hand's hold on the weapon in hand -- the hand in the controller's frame and its
 * fingers -- as the hold the LEFT hand will have on that weapon in dual wield (mirrored), at the next
 * palette drawn at rest. Any thread.
 */
void snapshot_grip(std::uintptr_t key) noexcept;

/**
 * Everything a third-person pose needs (docs/SWORD-SPARROW.md): the sample of the frame the palette
 * belongs to, camera-relative like the palette handed in, and what each hand holds.
 */
struct FreeContext final {
    xr::HeadPose head{};
    xr::HandPose left{};
    xr::HandPose right{};
    /** The heading the shoulders face (the head's on foot, the vehicle's on a vehicle). */
    float bodyYaw{};
    /** Which hand holds the sword: -1 none, 0 left, 1 right. */
    int swordHand{-1};
    /** The sword relative to the game's RIGHT hand bone, as the game holds it (palette space). */
    bool swordValid{};
    math::Affine sword{};
    /** Per hand, how far it is on the handlebar the game's own hand holds (0..1). */
    std::array<float, 2> bar{};
    /**
     * The other hand on the sword's hilt, below the sword hand (0..1, blended): the blade then lies along
     * the line from it to the sword hand, and it closes round the hilt there.
     */
    float twoHand{};
};

/** @return True when rewrite_free() can pose anything: the rig learned (in first person), VR running. */
[[nodiscard]] bool free_ready() noexcept;

/**
 * Poses a third-person arm palette (gloves or gauntlets of the Guardian's own body, made camera-
 * relative by the caller): each hand on its controller -- the sword hand with the blade along the
 * fist's axis, a hand on the handlebar where the game put it -- elbows solved, nothing learned.
 */
[[nodiscard]] bool rewrite_free(const float* bones, std::size_t boneCount, float* out, const FreeContext& ctx) noexcept;

/** The hand bone (palette matrix, camera-relative) rewrite_free() gives `side` (0 left, 1 right). */
[[nodiscard]] bool free_hand_bone(const FreeContext& ctx, int side, math::Affine& out) noexcept;

/** The palm centres of a third-person arm palette, as posed (palette space). */
[[nodiscard]] bool palm_points(const float* bones, std::size_t boneCount, math::V3& left, math::V3& right) noexcept;

/** The bind-space wrist joints and the hand bone indices (for vr_body.cpp). */
[[nodiscard]] bool hand_bones(int& left, int& right) noexcept;

} // namespace sunrise::client::hooks::vr::arms
