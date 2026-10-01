#pragma once

#include <array>
#include "runtime/reflection.hpp"
#include "runtime/runtime.hpp"
#include "world/math.gen.hpp"

namespace atelier::world {

ASTRUCT(Name="world.Vec3")
struct Vec3 {
    AGENERATED_BODY(Vec3)
public:
    AFIELD()
    float x{};
    AFIELD()
    float y{};
    AFIELD()
    float z{};
    bool operator==(const Vec3&) const = default;
};
ASTRUCT(Name="world.Quaternion")
struct Quaternion {
    AGENERATED_BODY(Quaternion)
public:
    AFIELD()
    float x{};
    AFIELD()
    float y{};
    AFIELD()
    float z{};
    AFIELD()
    float w{1.f};
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
[[nodiscard]] Result<Matrix4> multiply(const Matrix4&, const Matrix4&);
// Fails on singular parents, overflow, reflection or shear that local TRS cannot represent.
[[nodiscard]] Result<Trs> relative_transform(const Matrix4& world, const Matrix4& parent);

}
