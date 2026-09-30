#pragma once

#include "world/math.hpp"

namespace atelier::editor {
struct CameraMatrices {
    std::array<float, 16> view;
    std::array<float, 16> projection;
};
// Left-handed, metres, +Y up; row-major matrices and row vectors.
class Camera final {
public:
    Camera() { reset(); }
    void reset() noexcept;
    void focus(world::Vec3) noexcept;
    void orbit(float logical_dx, float logical_dy) noexcept;
    void zoom(float wheel_steps) noexcept;
    CameraMatrices matrices(float aspect) const noexcept;
private:
    world::Vec3 target_;
    float yaw_{}, pitch_{}, distance_{};
};
}
