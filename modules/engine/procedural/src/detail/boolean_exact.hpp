#pragma once

#include "foundation/hash.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// The common input lattice uses coordinates in [0, 2^25]. A face normal needs
// at most 52 signed bits and its offset 79. Rebased planes admit magnitudes
// below 2^52 and 2^81 respectively.
// Triple-plane Cramer's determinants fit 192 bits. No division or normalization
// occurs in a topology predicate. The pinned clang-cl supports C23 bit integers.
namespace atelier::procedural::detail::boolean_exact {
using Wide = _BitInt(256);

using Product = _BitInt(384);

using Triple = _BitInt(640);

using Offset = _BitInt(128);

using GridPoint = std::array<std::int64_t, 3>;

using Point = std::array<Wide, 4>; // Homogeneous, positive w.

template <class T>
int sign(T x) {
    return (x > 0) - (x < 0);
}

struct Plane {
    GridPoint n{};

    Offset d{};

    bool operator==(const Plane&) const = default;

    Wide evaluate(const Point& p) const { return n[0] * p[0] + n[1] * p[1] + n[2] * p[2] + d * p[3]; }

    int side(const Point& p) const {
        if (p[3] == 1)
            return sign(Offset(n[0]) * std::int64_t(p[0]) + Offset(n[1]) * std::int64_t(p[1]) +
                        Offset(n[2]) * std::int64_t(p[2]) + d);
        return sign(evaluate(p));
    }

    Plane flipped() const { return {{-n[0], -n[1], -n[2]}, -d}; }

    int axis() const {
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(n[k]) > std::abs(n[axis]))
                axis = k;
        return axis;
    }
};

inline Plane through(const GridPoint& a, const GridPoint& b, const GridPoint& c) {
    Plane p;
    for (int k = 0; k < 3; ++k) {
        const int i = (k + 1) % 3;
        const int j = (k + 2) % 3;
        p.n[k] = (b[i] - a[i]) * (c[j] - a[j]) - (b[j] - a[j]) * (c[i] - a[i]);
        p.d -= Offset(p.n[k]) * a[k];
    }
    return p;
}

// Contains edge a->b and the projection direction. Positive side is left of
// the directed edge when viewing the cyclic (i,j) projection.
inline Plane edge_plane(const GridPoint& a, const GridPoint& b, int axis) {
    const int i = (axis + 1) % 3;
    const int j = (axis + 2) % 3;
    Plane p;
    p.n[i] = a[j] - b[j];
    p.n[j] = b[i] - a[i];
    p.d = -(Offset(p.n[i]) * a[i] + Offset(p.n[j]) * a[j]);
    return p;
}

inline Point intersection(const Plane& a, const Plane& b, const Plane& c) {
    Point p{};
    for (int k = 0; k < 3; ++k) {
        const int i = (k + 1) % 3;
        const int j = (k + 2) % 3;
        const Offset bc = Offset(b.n[i]) * c.n[j] - Offset(b.n[j]) * c.n[i];
        const Offset ca = Offset(c.n[i]) * a.n[j] - Offset(c.n[j]) * a.n[i];
        const Offset ab = Offset(a.n[i]) * b.n[j] - Offset(a.n[j]) * b.n[i];
        p[k] = -(Wide(a.d) * bc + Wide(b.d) * ca + Wide(c.d) * ab);
        p[3] += Wide(a.n[k]) * bc;
    }
    if (p[3] < 0)
        for (auto& v : p)
            v = -v;
    return p;
}

inline int compare(const Point& a, const Point& b, int k) {
    if (a[3] == b[3])
        return sign(a[k] - b[k]);
    return sign(Product(a[k]) * b[3] - Product(b[k]) * a[3]);
}

struct PointLess {
    bool operator()(const Point& a, const Point& b) const {
        for (int k = 0; k < 3; ++k)
            if (const int order = compare(a, b, k))
                return order < 0;
        return false;
    }
};

// Strip the common power of two, then divide by the first odd component
// modulo 2^64. Any two integer representatives of a projective tuple
// differ from the same primitive tuple by an integer scale. Its odd part
// cancels here. A collision still requires exact projective equality.
template <class... T>
std::size_t projective_hash(Wide first, T... rest) noexcept {
    if (first == 1)
        return hash_fields(std::uint64_t{1}, static_cast<std::uint64_t>(rest)...);
    const auto bits = static_cast<unsigned _BitInt(256)>((first | ... | Wide(rest)));
    const int shift = __builtin_ctzg(bits); // The tuple is nonzero.
    auto reduced = [shift](Wide v) { return static_cast<std::uint64_t>(v >> shift); };
    const std::array words{reduced(first), reduced(rest)...};
    const auto pivot = *std::find_if(words.begin(), words.end(), [](auto w) { return w & 1; });
    std::uint64_t inverse = 1;
    // Newton's iteration doubles the number of correct inverse bits each time.
    for (int i = 0; i < 6; ++i)
        inverse *= 2 - pivot * inverse;
    return std::apply([inverse](auto... w) { return hash_fields((w * inverse)...); }, words);
}

struct PointRefHash {
    using is_avalanching = void;

    std::size_t operator()(const Point* p) const noexcept {
        const auto& [x, y, z, w] = *p;
        return projective_hash(w, x, y, z);
    }
};

inline bool same_point(const Point& a, const Point& b) noexcept {
    if (a[3] == b[3])
        return a == b;
    return !compare(a, b, 0) && !compare(a, b, 1) && !compare(a, b, 2);
}

struct PointRefEqual {
    bool operator()(const Point* a, const Point* b) const noexcept { return same_point(*a, *b); }
};

inline Triple determinant(const Point& a, const Point& b, const Point& c, int axis) {
    const int i = (axis + 1) % 3;
    const int j = (axis + 2) % 3;
    // Accumulate the full 3x3 homogeneous determinant only where a boundary
    // retains collinear corners after sewing.
    return Triple(a[i]) * (Product(b[j]) * c[3] - Product(c[j]) * b[3]) +
           Triple(a[j]) * (Product(b[3]) * c[i] - Product(c[3]) * b[i]) +
           Triple(a[3]) * (Product(b[i]) * c[j] - Product(c[i]) * b[j]);
}

// Plücker coordinates of the line shared by two support planes. Keeping these
// projective avoids a wide-integer GCD for every edge of every retained cell.
struct Line {
    std::array<Offset, 3> direction;

    std::array<Wide, 3> moment;

    int axis;

    Line(const Plane& a, const Plane& b) : axis(0) {
        for (int k = 0; k < 3; ++k) {
            const int i = (k + 1) % 3;
            const int j = (k + 2) % 3;
            direction[k] = Offset(a.n[i]) * b.n[j] - Offset(a.n[j]) * b.n[i];
            moment[k] = Wide(a.n[k]) * b.d - Wide(b.n[k]) * a.d;
        }
        while (!direction[axis])
            ++axis;
    }

    bool operator==(const Line& b) const noexcept {
        if (axis != b.axis)
            return false;
        if (direction[axis] == b.direction[axis])
            return direction == b.direction && moment == b.moment;
        for (int k = axis + 1; k < 3; ++k)
            if (Wide(direction[k]) * b.direction[axis] != Wide(b.direction[k]) * direction[axis])
                return false;
        for (int k = 0; k < 3; ++k)
            if (Product(moment[k]) * b.direction[axis] != Product(b.moment[k]) * direction[axis])
                return false;
        return true;
    }

    std::size_t hash() const noexcept {
        return projective_hash(direction[0], direction[1], direction[2], moment[0], moment[1], moment[2]);
    }
};
}
