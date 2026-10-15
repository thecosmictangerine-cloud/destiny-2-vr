#pragma once

/**
 * The little linear algebra the arm solver needs: 3-vectors, 3x3 rotations (column vectors), rigid
 * and affine transforms, and the swing-twist split. Header only, no dependencies, all inline.
 *
 * Conventions: a Mat3's columns are the images of the basis vectors, so `m * v` rotates `v`.
 * An Affine is `x -> m * x + t`; composing `a * b` applies `b` first.
 */

#include <array>
#include <cmath>

namespace sunrise::client::hooks::vr::math {

struct V3 final {
    float x{};
    float y{};
    float z{};

    constexpr V3 operator+(const V3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    constexpr V3 operator-(const V3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    constexpr V3 operator-() const noexcept { return {-x, -y, -z}; }
    constexpr V3 operator*(float s) const noexcept { return {x * s, y * s, z * s}; }
    constexpr V3& operator+=(const V3& o) noexcept {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    constexpr float operator[](int i) const noexcept { return i == 0 ? x : (i == 1 ? y : z); }
};

constexpr float dot(const V3& a, const V3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr V3 cross(const V3& a, const V3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const V3& v) noexcept { return std::sqrt(dot(v, v)); }
inline V3 normalize(const V3& v, const V3& fallback = {1.0F, 0.0F, 0.0F}) noexcept {
    const float n = length(v);
    return n > 1e-8F ? v * (1.0F / n) : fallback;
}
inline bool finite(const V3& v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct M3 final {
    /** Columns. */
    std::array<V3, 3> c{V3{1, 0, 0}, V3{0, 1, 0}, V3{0, 0, 1}};

    constexpr V3 operator*(const V3& v) const noexcept { return c[0] * v.x + c[1] * v.y + c[2] * v.z; }
    constexpr M3 operator*(const M3& o) const noexcept { return M3{{*this * o.c[0], *this * o.c[1], *this * o.c[2]}}; }
    /** Row r, column k. */
    constexpr float at(int r, int k) const noexcept { return c[static_cast<std::size_t>(k)][r]; }
};

constexpr M3 transpose(const M3& m) noexcept {
    return M3{{V3{m.c[0].x, m.c[1].x, m.c[2].x}, V3{m.c[0].y, m.c[1].y, m.c[2].y},
               V3{m.c[0].z, m.c[1].z, m.c[2].z}}};
}

/** Rotation by `angle` radians about the unit axis `a` (Rodrigues). */
inline M3 axis_angle(const V3& a, float angle) noexcept {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.0F - c;
    return M3{{V3{t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y},
               V3{t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x},
               V3{t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c}}};
}

/**
 * An orthonormal frame whose X runs along `axis` and whose Z is `normal` made perpendicular to it.
 * This is how a limb segment's orientation is pinned down by two vectors: its own direction and
 * the normal of the plane its joint bends in.
 */
inline M3 frame_from(const V3& axis, const V3& normal) noexcept {
    const V3 x = normalize(axis);
    V3 z = normal - x * dot(normal, x);
    z = normalize(z, normalize(cross(x, V3{0, 0, 1}), V3{0, 1, 0}));
    const V3 y = cross(z, x);
    return M3{{x, y, z}};
}

/** Re-orthonormalises a nearly orthonormal matrix (Gram-Schmidt on X then Y). */
inline M3 orthonormalize(const M3& m) noexcept {
    const V3 x = normalize(m.c[0]);
    V3 y = m.c[1] - x * dot(m.c[1], x);
    y = normalize(y, normalize(cross(V3{0, 0, 1}, x), V3{0, 1, 0}));
    return M3{{x, y, cross(x, y)}};
}

struct Affine final {
    M3 m{};
    V3 t{};

    constexpr V3 operator*(const V3& p) const noexcept { return m * p + t; }
    constexpr Affine operator*(const Affine& o) const noexcept { return Affine{m * o.m, m * o.t + t}; }
};

/** Inverse of a RIGID transform (orthonormal m). */
constexpr Affine inverse_rigid(const Affine& a) noexcept {
    const M3 r = transpose(a.m);
    return Affine{r, -(r * a.t)};
}

/** The transform that rotates by `r` about the point `pivot`. */
constexpr Affine rotate_about(const M3& r, const V3& pivot) noexcept {
    return Affine{r, pivot - r * pivot};
}

/** The transform that scales by `k` along the unit `axis` through `pivot` (identity across it). */
constexpr Affine stretch_about(const V3& pivot, const V3& axis, float k) noexcept {
    // m = I + (k - 1) * a a^T
    const float e = k - 1.0F;
    const M3 m{{V3{1 + e * axis.x * axis.x, e * axis.x * axis.y, e * axis.x * axis.z},
                V3{e * axis.y * axis.x, 1 + e * axis.y * axis.y, e * axis.y * axis.z},
                V3{e * axis.z * axis.x, e * axis.z * axis.y, 1 + e * axis.z * axis.z}}};
    return Affine{m, pivot - m * pivot};
}

/** Reads one 3x4 row-major palette matrix (three float4 rows: m00 m01 m02 tx | ...). */
inline Affine read_rows(const float* rows) noexcept {
    Affine a{};
    a.m = M3{{V3{rows[0], rows[4], rows[8]}, V3{rows[1], rows[5], rows[9]}, V3{rows[2], rows[6], rows[10]}}};
    a.t = V3{rows[3], rows[7], rows[11]};
    return a;
}

inline void write_rows(const Affine& a, float* rows) noexcept {
    for (int r = 0; r < 3; ++r) {
        rows[r * 4 + 0] = a.m.at(r, 0);
        rows[r * 4 + 1] = a.m.at(r, 1);
        rows[r * 4 + 2] = a.m.at(r, 2);
        rows[r * 4 + 3] = a.t[r];
    }
}

/** Unit quaternion (x, y, z, w) of a rotation matrix. */
inline std::array<float, 4> quat_of(const M3& m) noexcept {
    const float trace = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
    std::array<float, 4> q{};
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q = {(m.at(2, 1) - m.at(1, 2)) / s, (m.at(0, 2) - m.at(2, 0)) / s, (m.at(1, 0) - m.at(0, 1)) / s, 0.25F * s};
    } else if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
        const float s = std::sqrt(1.0F + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2.0F;
        q = {0.25F * s, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s, (m.at(2, 1) - m.at(1, 2)) / s};
    } else if (m.at(1, 1) > m.at(2, 2)) {
        const float s = std::sqrt(1.0F + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2.0F;
        q = {(m.at(0, 1) + m.at(1, 0)) / s, 0.25F * s, (m.at(1, 2) + m.at(2, 1)) / s, (m.at(0, 2) - m.at(2, 0)) / s};
    } else {
        const float s = std::sqrt(1.0F + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2.0F;
        q = {(m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, 0.25F * s, (m.at(1, 0) - m.at(0, 1)) / s};
    }
    return q;
}

inline M3 mat_of(const std::array<float, 4>& q) noexcept {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return M3{{V3{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
               V3{2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
               V3{2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}}};
}

/** Normalised linear blend of two rotations, short way round; good enough below ~90 degrees apart. */
inline M3 blend(const M3& a, const M3& b, float t) noexcept {
    auto qa = quat_of(a);
    auto qb = quat_of(b);
    const float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    const float sign = d < 0.0F ? -1.0F : 1.0F;
    std::array<float, 4> q{};
    float n = 0.0F;
    for (std::size_t i = 0; i < 4; ++i) {
        q[i] = qa[i] * (1.0F - t) + qb[i] * sign * t;
        n += q[i] * q[i];
    }
    n = std::sqrt(n);
    for (auto& v : q) {
        v /= n > 1e-8F ? n : 1.0F;
    }
    return mat_of(q);
}

/** Twist angle of rotation `r` about the unit axis `a` (swing-twist decomposition), in radians. */
inline float twist_about(const M3& r, const V3& a) noexcept {
    const auto q = quat_of(r);
    const float p = q[0] * a.x + q[1] * a.y + q[2] * a.z;
    return 2.0F * std::atan2(p, q[3]);
}

/** Wraps an angle to (-pi, pi]. */
inline float wrap(float angle) noexcept {
    constexpr float kPi = 3.14159265358979F;
    while (angle > kPi) {
        angle -= 2.0F * kPi;
    }
    while (angle <= -kPi) {
        angle += 2.0F * kPi;
    }
    return angle;
}

/** Solves the symmetric positive 3x3 system `a x = b` (Cramer). Returns false when singular. */
inline bool solve3(const std::array<std::array<double, 3>, 3>& a, const std::array<double, 3>& b,
                   std::array<double, 3>& x) noexcept {
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                       - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                       + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::fabs(det) < 1e-12) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        auto m = a;
        for (int r = 0; r < 3; ++r) {
            m[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)] = b[static_cast<std::size_t>(r)];
        }
        x[static_cast<std::size_t>(k)] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                                          - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                                          + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
                                         / det;
    }
    return true;
}

} // namespace sunrise::client::hooks::vr::math
