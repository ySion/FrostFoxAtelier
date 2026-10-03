#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace atelier::math {

// An execution classification of the Runtime's canonical record types. It does
// not create a second type identity or change their compact memory layout.
enum class Shape : std::uint32_t { none, vector2, vector3, vector4, quaternion, matrix4 };
enum class Precision : std::uint32_t { f32, f64 };
struct Type {
    Shape shape{Shape::none};
    Precision precision{Precision::f32};
    constexpr explicit operator bool() const noexcept { return shape != Shape::none; }
    constexpr bool vector() const noexcept {
        return shape == Shape::vector2 || shape == Shape::vector3 || shape == Shape::vector4;
    }
    constexpr std::size_t components() const noexcept {
        switch (shape) {
        case Shape::vector2: return 2;
        case Shape::vector3: return 3;
        case Shape::vector4: case Shape::quaternion: return 4;
        case Shape::matrix4: return 16;
        default: return 0;
        }
    }
    constexpr std::size_t scalar_size() const noexcept { return precision == Precision::f64 ? 8 : 4; }
    constexpr std::size_t size() const noexcept { return components() * scalar_size(); }
    bool operator==(const Type&) const = default;
};

constexpr std::string_view name(Type type) noexcept {
    const bool wide = type.precision == Precision::f64;
    switch (type.shape) {
    case Shape::vector2: return wide ? "math.vector2d" : "math.vector2";
    case Shape::vector3: return wide ? "math.vector3d" : "math.vector3";
    case Shape::vector4: return wide ? "math.vector4d" : "math.vector4";
    case Shape::quaternion: return wide ? "math.quaterniond" : "math.quaternion";
    case Shape::matrix4: return wide ? "math.matrix4d" : "math.matrix4";
    default: return {};
    }
}
constexpr std::string_view short_name(Type type) noexcept {
    const auto qualified = name(type);
    return qualified.empty() ? qualified : qualified.substr(5);
}

}
