#include "editor/camera.hpp"
#include "presentation/presentation.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace atelier::editor {
bool Camera::set_state(const CameraState& value) noexcept {
    if (value.view > CameraView::left || !std::isfinite(value.target.x) || !std::isfinite(value.target.y) ||
        !std::isfinite(value.target.z) || !std::isfinite(value.yaw) || !std::isfinite(value.pitch) ||
        !std::isfinite(value.distance) || value.distance < .5f || value.distance > 2000.f ||
        std::abs(value.pitch) > DirectX::XM_PIDIV2 || !std::isfinite(value.roll) ||
        !std::isfinite(value.vertical_fov) || value.vertical_fov < 1 || value.vertical_fov > 179 ||
        !std::isfinite(value.near_clip) || !std::isfinite(value.far_clip) || value.near_clip < .001f ||
        value.near_clip > 100000 || value.far_clip <= value.near_clip || value.far_clip > 1000000)
        return false;
    auto candidate = *this;
    candidate.view_ = value.view;
    candidate.target_ = value.target;
    candidate.yaw_ = value.yaw;
    candidate.pitch_ = value.pitch;
    candidate.distance_ = value.distance;
    candidate.roll_ = value.roll;
    candidate.vertical_fov_ = value.vertical_fov;
    candidate.near_clip_ = value.near_clip;
    candidate.far_clip_ = value.far_clip;
    const auto forward = math::try_normalize(candidate.target_ - candidate.eye());
    const auto up = candidate.up_hint();
    if (!forward || !math::try_normalize(math::cross(up, *forward)))
        return false;
    *this = candidate;
    return true;
}
void Camera::reset() noexcept {
    view_ = CameraView::perspective;
    target_ = {};
    distance_ = std::sqrt(125.f);
    yaw_ = std::atan2(6.f, -8.f);
    pitch_ = std::asin(5.f / distance_);
    roll_ = 0;
    vertical_fov_ = 45;
    near_clip_ = .05f;
    far_clip_ = 30000;
}
void Camera::set_view(CameraView view) noexcept {
    view_ = view;
    roll_ = 0;
    if (view == CameraView::perspective) {
        yaw_ = std::atan2(6.f, -8.f);
        pitch_ = std::asin(5.f / std::sqrt(125.f));
    } else {
        yaw_ = view == CameraView::right  ? DirectX::XM_PIDIV2
               : view == CameraView::left ? -DirectX::XM_PIDIV2
               : view == CameraView::back ? 0
                                          : DirectX::XM_PI;
        pitch_ = view == CameraView::top ? DirectX::XM_PIDIV2 : view == CameraView::bottom ? -DirectX::XM_PIDIV2 : 0;
    }
}
math::Vector3 Camera::up_hint() const noexcept {
    if (view_ != CameraView::perspective)
        return view_ == CameraView::top      ? math::Vector3{0, 0, 1}
               : view_ == CameraView::bottom ? math::Vector3{0, 0, -1}
                                             : math::Vector3{0, 1, 0};
    const math::Vector3 right{-std::cos(yaw_), 0, std::sin(yaw_)};
    const math::Vector3 up{-std::sin(pitch_) * std::sin(yaw_), std::cos(pitch_), -std::sin(pitch_) * std::cos(yaw_)};
    return up * std::cos(roll_) + right * std::sin(roll_);
}
math::Vector3 Camera::construction_normal() const noexcept {
    return view_ == CameraView::front || view_ == CameraView::back   ? math::Vector3{0, 0, 1}
           : view_ == CameraView::right || view_ == CameraView::left ? math::Vector3{1, 0, 0}
                                                                     : math::Vector3{0, 1, 0};
}
void Camera::focus(math::Vector3 target) noexcept {
    target_ = target;
    distance_ = 5.f;
}
bool Camera::look_at(math::Vector3 eye, math::Vector3 target) noexcept {
    const double dx = double(eye.x) - target.x;
    const double dy = double(eye.y) - target.y;
    const double dz = double(eye.z) - target.z;
    const auto distance = std::hypot(dx, dy, dz);
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z) || !std::isfinite(distance) ||
        distance < .5 || distance > 2000 || std::hypot(dx, dz) < 1e-6)
        return false;
    return set_state({CameraView::perspective, target, static_cast<float>(std::atan2(dx, dz)),
                      static_cast<float>(std::asin(dy / distance)), static_cast<float>(distance)});
}
bool Camera::view_from(const presentation::CameraSnapshot& snapshot) noexcept {
    const auto rotation = math::try_normalize(snapshot.rotation);
    if (!rotation)
        return false;
    const auto forward = math::rotate(*rotation, math::Vector3{0, 0, 1});
    const auto up = math::rotate(*rotation, math::Vector3{0, 1, 0});
    const float yaw = std::atan2(-forward.x, -forward.z);
    const float pitch = std::atan2(-forward.y, std::hypot(forward.x, forward.z));
    const math::Vector3 right{-std::cos(yaw), 0, std::sin(yaw)};
    const math::Vector3 upright{-std::sin(pitch) * std::sin(yaw), std::cos(pitch), -std::sin(pitch) * std::cos(yaw)};
    const float roll = std::atan2(math::dot(up, right), math::dot(up, upright));
    return set_state({CameraView::perspective, snapshot.position + forward * distance_, yaw, pitch, distance_, roll,
                      snapshot.vertical_fov, snapshot.near_clip, snapshot.far_clip});
}
void Camera::frame(math::Vector3 center, float radius) noexcept {
    target_ = center;
    // Fit the sphere to the vertical field with a small margin; orthographic views share the distance.
    distance_ = std::clamp(std::max(radius, .05f) * 1.15f / std::sin(DirectX::XMConvertToRadians(vertical_fov_) * .5f),
                           .5f, 2000.f);
}
void Camera::orbit(float dx, float dy) noexcept {
    view_ = CameraView::perspective;
    yaw_ += dx * .006f;
    pitch_ = std::clamp(pitch_ + dy * .006f, -1.45f, 1.45f);
}
void Camera::look(float dx, float dy) noexcept {
    const auto position = eye();
    view_ = CameraView::perspective;
    yaw_ += dx * .003f;
    pitch_ = std::clamp(pitch_ + dy * .003f, -1.55f, 1.55f);
    target_ += position - eye();
}
void Camera::fly(math::Vector3 local) noexcept {
    const auto forward = math::try_normalize(target_ - eye());
    if (!forward)
        return;
    const auto right = math::try_normalize(math::cross(math::Vector3{0, 1, 0}, *forward));
    if (!right)
        return;
    target_ += *right * local.x + math::Vector3{0, local.y, 0} + *forward * local.z;
}
void Camera::zoom(float steps) noexcept {
    distance_ = std::clamp(distance_ * std::exp(-steps * .12f), .5f, 2000.f);
}
math::Vector3 Camera::eye() const noexcept {
    return {target_.x + distance_ * std::cos(pitch_) * std::sin(yaw_), target_.y + distance_ * std::sin(pitch_),
            target_.z + distance_ * std::cos(pitch_) * std::cos(yaw_)};
}
void Camera::pan(float dx, float dy, float height) noexcept {
    if (!(height > 0))
        return;
    const auto forward = math::try_normalize(target_ - eye());
    if (!forward)
        return;
    const auto right = math::try_normalize(math::cross(up_hint(), *forward));
    if (!right)
        return;
    const auto up = math::cross(*forward, *right);
    // One pixel spans this many metres on the target plane.
    const float metres = 2 * distance_ * std::tan(DirectX::XMConvertToRadians(vertical_fov_) * .5f) / height;
    target_ -= *right * (dx * metres);
    target_ += up * (dy * metres);
}
CameraMatrices Camera::matrices(float aspect) const noexcept {
    using namespace DirectX;
    const auto position = eye();
    const auto eye_vector = XMVectorSet(position.x, position.y, position.z, 1.f);
    const auto target = XMVectorSet(target_.x, target_.y, target_.z, 1.f);
    XMFLOAT4X4 view;
    const auto up = up_hint();
    XMStoreFloat4x4(&view, XMMatrixLookAtLH(eye_vector, target, XMVectorSet(up.x, up.y, up.z, 0)));
    const bool perspective = view_ == CameraView::perspective;
    const double scale = 1 / std::tan(double(vertical_fov_) * std::numbers::pi / 360);
    const double y = perspective ? scale : scale / distance_;
    const double depth = double(far_clip_) - near_clip_;
    // Perspective depth is near / view Z; depth zero is an infinite direction.
    // Orthographic depth remains linear over its finite clip interval.
    const double z = perspective ? 0 : -1 / depth;
    const double offset = perspective ? double(near_clip_) : double(far_clip_) / depth;
    CameraMatrices result;
    static_assert(sizeof(view) == sizeof(result.view));
    std::memcpy(result.view.data(), &view, sizeof(view));
    result.projection = {{static_cast<float>(y / std::max(aspect, .01f)), 0, 0, 0, 0, static_cast<float>(y), 0, 0, 0, 0,
                          static_cast<float>(z), perspective ? 1.f : 0.f, 0, 0, static_cast<float>(offset),
                          perspective ? 0.f : 1.f}};
    result.far_depth = perspective ? near_clip_ / far_clip_ : 0;
    return result;
}
}
