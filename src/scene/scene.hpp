#pragma once

#include "runtime/runtime.hpp"
#include <array>

namespace atelier::scene {

using ObjectKey = std::uint64_t;
struct Vec3 {
    float x{}, y{}, z{};
    bool operator==(const Vec3&) const = default;
};
struct Transform { Vec3 position; };
struct SceneObject { Transform transform; };
struct Member {
    ObjectKey key{};
    ObjectRoot root;
    ObjectId id() const noexcept { return root.id(); }
};

// Runtime is the only owner of object values. Members and history own roots;
// UI/render snapshots never become a second mutable scene model.
class Scene final {
public:
    static constexpr std::size_t maximum_members = 256;
    static constexpr float position_limit = 10000.f;
    Scene();
    Result<Member> create(Vec3 position);
    Result<void> insert(const Member&, std::size_t index);
    Result<void> remove(ObjectKey);
    const Member* find(ObjectKey) const noexcept;
    std::span<const Member> members() const noexcept { return members_; }
    Result<Vec3> position(ObjectKey) const;
    Result<void> position(ObjectKey, Vec3);
    Result<void> collect_step();
    std::size_t live_objects() const noexcept { return runtime_.live_objects(); }
    static bool valid(Vec3) noexcept;
private:
    Runtime runtime_;
    TypeId object_type_;
    std::array<std::array<FieldId, 3>, 3> position_fields_;
    std::vector<Member> members_;
    ObjectKey next_key_{1};
    Result<Vec3> read_position(ObjectId) const;
    Result<void> write_position(ObjectId, Vec3);
};

}
