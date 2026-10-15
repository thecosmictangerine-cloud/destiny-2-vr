#pragma once

/**
 * The geometry of the two-handed hold, in one place because two modules must agree on it to the
 * millimetre: vr_arms.cpp decides when the left hand takes hold and draws it on the weapon, and
 * vr_weapon.cpp turns the weapon so its foregrip runs through the left hand. Header only, pure.
 *
 * Frames. `c` is the right controller's RAW basis -- columns (forward, LEFT, up) in the game's
 * axes, before the weapon module's rotational seat. `p` and `l` are the right and left controllers'
 * grip (palm) points, in any common frame. `fc` is the foregrip -- the left palm where the game's
 * own animation puts it on this weapon -- expressed in `c`, measured from the right palm.
 *
 * Everything drawn on the gun pivots about the right palm: the weapon module's positional seat puts
 * the drawn right palm exactly there, at any orientation. So turning the basis handed to the engine
 * by a world rotation D moves the foregrip from `p + c fc` to `p + D c fc`, and nothing else is
 * needed to know where it lands.
 *
 * The left hand may hold anywhere along the handguard, so the foregrip is a LINE, `fc + s * x`,
 * with `x` the controller's forward -- which the rotational seat makes the barrel's -- and `s` in
 * [sMin, sMax] metres. A support grip under a pistol is the degenerate case sMin = sMax = 0.
 */

#include <algorithm>
#include <cmath>

#include "vr_math.h"

namespace sunrise::client::hooks::vr::two_hand {

/** The rotation that brings the foregrip onto the left hand, and what it took to find it. */
struct Swing final {
    /** World-space rotation axis (unit) and angle, radians, smallest rotation that does it. */
    math::V3 axis{0.0F, 0.0F, 1.0F};
    float angle{};
    /** Where along the handguard the left hand holds, clamped to the handguard. */
    float s{};
    /** The same before clamping: how far past either end of the handguard the hand has slid. */
    float sRaw{};
    /** Distance between the two palms. */
    float lever{};
};

/**
 * Turns the weapon so the barrel runs through the left hand, exactly.
 *
 * A rotation about the right palm preserves every distance from it, so of all the points on the
 * foregrip line only those at the left palm's own distance can ever be brought onto it: the line
 * meets that sphere at `s = -fc.x + sqrt(fc.x^2 - |fc|^2 + |l - p|^2)` (the forward root, the one
 * on the handguard). Rotating the shortest way from where that point is to where the hand is then
 * puts the foregrip line through the palm with no residue, at any lever and any slide. The
 * shortest arc is also what keeps the roll the right wrist's: pitching or yawing the gun with the
 * left hand turns it about an axis perpendicular to the barrel, never about the barrel itself.
 */
inline Swing swing(const math::M3& c, const math::V3& p, const math::V3& l, const math::V3& fc, float sMin,
                   float sMax) noexcept {
    using namespace math;
    Swing out{};
    const V3 d = l - p;
    out.lever = length(d);
    const float disc = fc.x * fc.x - dot(fc, fc) + out.lever * out.lever;
    out.sRaw = disc >= 0.0F ? -fc.x + std::sqrt(disc) : -fc.x;
    out.s = std::clamp(out.sRaw, sMin, sMax);
    const V3 u = normalize(c * (fc + V3{out.s, 0.0F, 0.0F}));
    const V3 v = normalize(d, u);
    const V3 x = cross(u, v);
    const float sine = length(x);
    out.angle = std::atan2(sine, dot(u, v));
    out.axis = sine > 1e-7F ? x * (1.0F / sine) : V3{0.0F, 0.0F, 1.0F};
    return out;
}

/**
 * How far the left palm is from the handguard of the weapon as it is held one-handed: the distance
 * that decides whether squeezing the grip takes hold of the gun or throws a grenade.
 * @param s Receives where along the handguard the closest point is.
 */
inline float zone_distance(const math::M3& c, const math::V3& p, const math::V3& l, const math::V3& fc, float sMin,
                           float sMax, float* s = nullptr) noexcept {
    using namespace math;
    const V3 base = p + c * fc;
    const V3 axis = c.c[0];
    const float t = std::clamp(dot(l - base, axis), sMin, sMax);
    if (s != nullptr) {
        *s = t;
    }
    return length(l - (base + axis * t));
}

} // namespace sunrise::client::hooks::vr::two_hand
