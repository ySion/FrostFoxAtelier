#pragma once

#include <array>
#include "runtime/runtime.hpp"

namespace atelier::world {

struct Vec3 {
    float x{}, y{}, z{};
    bool operator==(const Vec3&) const = default;
};
struct Quaternion {
    float x{}, y{}, z{}, w{1.f};
    bool operator==(const Quaternion&) const = default;
};
struct Trs {
    Vec3 position;
    Quaternion rotation;
    Vec3 scale{1.f, 1.f, 1.f};
    bool operator==(const Trs&) const = default;
};
// Row-major, row vectors: local = S * R * T; translation is [12..14].
using Matrix4 = std::array<float, 16>;
[[nodiscard]] Result<Trs> canonical_transform(Trs);
[[nodiscard]] Matrix4 transform_matrix(const Trs&) noexcept;

}
