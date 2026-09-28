#include "scene/scene.hpp"
#include "runtime/native.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace atelier::scene {
namespace {
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
auto invalid(const char* message) { return std::unexpected(Error{ErrorCode::invalid_definition, message}); }
}

Scene::Scene() {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(NativeRegistration<Vec3>{runtime_, "scene.Vec3", TypeKind::record}
        .field<&Vec3::x>("x").field<&Vec3::y>("y").field<&Vec3::z>("z").build());
    definitions.push_back(NativeRegistration<Transform>{runtime_, "scene.Transform", TypeKind::record}
        .field<&Transform::position>("position").build());
    definitions.push_back(NativeRegistration<SceneObject>{runtime_, "scene.Object", TypeKind::object}
        .field<&SceneObject::transform>("transform").build());
    const auto types = take(runtime_.register_types(std::move(definitions)));
    object_type_ = types[2];
    const auto transform = take(runtime_.find_field(types[2], "transform"));
    const auto position = take(runtime_.find_field(types[1], "position"));
    constexpr std::array names{"x", "y", "z"};
    for (std::size_t axis = 0; axis < names.size(); ++axis)
        position_fields_[axis] = {transform, position, take(runtime_.find_field(types[0], names[axis]))};
    members_.reserve(maximum_members);
}

bool Scene::valid(Vec3 position) noexcept {
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
        std::abs(position.x) <= position_limit && std::abs(position.y) <= position_limit &&
        std::abs(position.z) <= position_limit;
}
Result<Member> Scene::create(Vec3 position) {
    if (!valid(position)) return invalid("Position must be finite and within +/-10000 metres.");
    if (members_.size() >= maximum_members || next_key_ == std::numeric_limits<ObjectKey>::max())
        return invalid("Scene object limit reached.");
    auto object = runtime_.create(object_type_);
    if (!object) return std::unexpected(object.error());
    auto written = write_position(object->id(), position);
    if (!written) return std::unexpected(written.error());
    return Member{next_key_++, std::move(*object)};
}
const Member* Scene::find(ObjectKey key) const noexcept {
    const auto found = std::find_if(members_.begin(), members_.end(), [key](const auto& item) { return item.key == key; });
    return found == members_.end() ? nullptr : &*found;
}
Result<void> Scene::insert(const Member& member, std::size_t index) {
    if (!member.key || !member.root.registered() || find(member.key) || index > members_.size() || members_.size() >= maximum_members)
        return invalid("Cannot insert this scene member.");
    auto type = runtime_.type_of(member.id());
    if (!type || *type != object_type_) return invalid("Scene member belongs to another Runtime or type.");
    if (std::any_of(members_.begin(), members_.end(), [&](const auto& item) { return item.id() == member.id(); }))
        return invalid("Object is already present under another scene key.");
    // Acquire the root before changing the member directory. Allocation failure
    // leaves both the directory and the caller's history root intact.
    Member held = member;
    members_.insert(members_.begin() + static_cast<std::ptrdiff_t>(index), std::move(held));
    return {};
}
Result<void> Scene::remove(ObjectKey key) {
    const auto found = std::find_if(members_.begin(), members_.end(), [key](const auto& item) { return item.key == key; });
    if (found == members_.end()) return invalid("Object is not a scene member.");
    members_.erase(found);
    return {};
}
Result<Vec3> Scene::read_position(ObjectId id) const {
    std::array<float, 3> values{};
    for (std::size_t axis = 0; axis < values.size(); ++axis) {
        auto value = runtime_.read(id, position_fields_[axis]);
        if (!value) return std::unexpected(value.error());
        values[axis] = std::get<float>(*value);
    }
    return Vec3{values[0], values[1], values[2]};
}
Result<void> Scene::write_position(ObjectId id, Vec3 position) {
    if (!valid(position)) return invalid("Position must be finite and within +/-10000 metres.");
    const auto old = read_position(id);
    if (!old) return std::unexpected(old.error());
    const std::array before{old->x, old->y, old->z};
    const std::array after{position.x, position.y, position.z};
    for (std::size_t axis = 0; axis < after.size(); ++axis) {
        auto result = runtime_.write(id, position_fields_[axis], Scalar{after[axis]});
        if (!result) {
            for (std::size_t restore = 0; restore < axis; ++restore) {
                const auto restored = runtime_.write(id, position_fields_[restore], Scalar{before[restore]});
                if (!restored) throw std::runtime_error("Scene position rollback failed: " + restored.error().message);
            }
            return result;
        }
    }
    return {};
}
Result<Vec3> Scene::position(ObjectKey key) const {
    const auto* member = find(key);
    if (!member) return invalid("Object is not a scene member.");
    return read_position(member->id());
}
Result<void> Scene::position(ObjectKey key, Vec3 position) {
    const auto* member = find(key);
    if (!member) return invalid("Object is not a scene member.");
    return write_position(member->id(), position);
}
Result<void> Scene::collect_step() {
    if (runtime_.collection_phase() == CollectionPhase::idle) {
        auto started = runtime_.begin_collection();
        if (!started) return started;
    }
    auto progress = runtime_.collect_step({32});
    if (!progress) return std::unexpected(progress.error());
    return {};
}
}
