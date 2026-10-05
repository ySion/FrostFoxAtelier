#include "physics/physics.hpp"
#include "runtime/native.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace atelier::physics {
namespace {
std::mutex registration_mutex;
std::size_t scene_count{};

std::unexpected<Error> failure(ErrorCode code, const char* message) {
    return std::unexpected(Error{code, message});
}
Result<void> acquire_jolt() {
    std::lock_guard lock(registration_mutex);
    if (scene_count == 0) {
        if (JPH::Factory::sInstance) return failure(ErrorCode::busy, "Jolt has another process owner.");
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ++scene_count;
    return {};
}
void release_jolt() noexcept {
    std::lock_guard lock(registration_mutex);
    if (--scene_count == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}
Result<void> validate(BodySettings value) {
    if (value.shape != ShapeKind::box && value.shape != ShapeKind::sphere && value.shape != ShapeKind::triangle_mesh)
        return failure(ErrorCode::argument_mismatch, "Unknown collision shape.");
    if (value.motion != MotionKind::stationary && value.motion != MotionKind::kinematic && value.motion != MotionKind::dynamic)
        return failure(ErrorCode::argument_mismatch, "Unknown body motion type.");
    if (!math::is_finite(value.half_extents) || value.half_extents.x <= 0 || value.half_extents.y <= 0 ||
        value.half_extents.z <= 0 || !std::isfinite(value.radius) || value.radius <= 0)
        return failure(ErrorCode::argument_mismatch, "Collision dimensions must be finite and positive.");
    if (value.shape == ShapeKind::triangle_mesh && value.motion != MotionKind::stationary)
        return failure(ErrorCode::argument_mismatch, "Triangle mesh collision requires a stationary body.");
    return {};
}
JPH::RVec3 position(math::Vector3 value) { return {value.x, value.y, value.z}; }
JPH::Vec3 vector(math::Vector3 value) { return {value.x, value.y, value.z}; }
JPH::Quat rotation(math::Quaternion value) { return {value.x, value.y, value.z, value.w}; }
math::Vector3 vector(JPH::RVec3Arg value) { return {value.GetX(), value.GetY(), value.GetZ()}; }
math::Quaternion rotation(JPH::QuatArg value) { return {value.GetX(), value.GetY(), value.GetZ(), value.GetW()}; }
JPH::EMotionType motion(MotionKind value) {
    switch (value) {
    case MotionKind::stationary: return JPH::EMotionType::Static;
    case MotionKind::kinematic: return JPH::EMotionType::Kinematic;
    case MotionKind::dynamic: return JPH::EMotionType::Dynamic;
    }
    std::unreachable();
}
}

Result<Types> register_types(Runtime& runtime) {
    auto id = NativeRegistration<BodyComponent>{runtime, "physics.BodyComponent", TypeKind::object}
        .base<world::Component>().publish();
    if (!id) return std::unexpected(id.error());
    return Types{*id};
}
Result<void> configure(Runtime& runtime, ObjectId component, BodySettings settings) {
    if (auto valid = validate(settings); !valid) return valid;
    auto state = world::inspect_component(runtime, component);
    if (!state) return std::unexpected(state.error());
    if (state->active) return failure(ErrorCode::busy, "Stop the World before changing a physical body.");
    return runtime.with_native<BodyComponent>(component, [&](BodyComponent& body) -> Result<void> {
        body.settings = settings;
        return {};
    });
}
Result<BodySettings> read_settings(Runtime& runtime, ObjectId component) {
    return runtime.with_native<const BodyComponent>(component, [](const BodyComponent& body) -> Result<BodySettings> {
        return body.settings;
    });
}

struct PhysicsScene::Impl {
    static constexpr JPH::ObjectLayer stationary_layer = 0;
    static constexpr JPH::ObjectLayer moving_layer = 1;
    struct RawContact { ContactPhase phase; JPH::BodyID first, second; };
    struct Contacts final : JPH::ContactListener {
        std::mutex mutex;
        std::vector<RawContact> pending;
        void push(ContactPhase phase, JPH::BodyID first, JPH::BodyID second) {
            std::lock_guard lock(mutex);
            pending.push_back({phase, first, second});
        }
        void OnContactAdded(const JPH::Body& first, const JPH::Body& second,
            const JPH::ContactManifold&, JPH::ContactSettings&) override {
            push(ContactPhase::began, first.GetID(), second.GetID());
        }
        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            push(ContactPhase::ended, pair.GetBody1ID(), pair.GetBody2ID());
        }
    } contacts;
    JPH::BroadPhaseLayerInterfaceTable broad_phase{2, 2};
    JPH::ObjectLayerPairFilterTable pairs{2};
    std::optional<JPH::ObjectVsBroadPhaseLayerFilterTable> broad_phase_filter;
    JPH::TempAllocatorImpl temporary{4 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem system;
    struct Body { JPH::BodyID id; ObjectId component, root; MotionKind motion; geometry::MeshRef mesh; };
    std::unordered_map<ObjectId, Body> bodies;
    std::unordered_map<JPH::BodyID, ObjectId> actors;

    Impl() {
        broad_phase.MapObjectToBroadPhaseLayer(stationary_layer, JPH::BroadPhaseLayer(0));
        broad_phase.MapObjectToBroadPhaseLayer(moving_layer, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(stationary_layer, moving_layer);
        pairs.EnableCollision(moving_layer, moving_layer);
        broad_phase_filter.emplace(broad_phase, 2, pairs, 2);
        system.Init(4096, 0, 4096, 4096, broad_phase, *broad_phase_filter, pairs);
        system.SetContactListener(&contacts);
        system.SetGravity(JPH::Vec3(0, -9.81f, 0));
    }
};

PhysicsScene::PhysicsScene(world::World& world, Types types)
    : world_(world), types_(types), impl_(std::make_unique<Impl>()) {}
Result<std::unique_ptr<PhysicsScene>> PhysicsScene::create(world::World& world, Types types) {
    if (world.closed()) return failure(ErrorCode::unsupported_operation, "Cannot attach physics to a closed World.");
    auto actual = world.runtime().native_type<BodyComponent>();
    if (!actual) return std::unexpected(actual.error());
    if (*actual != types.body_component) return failure(ErrorCode::type_mismatch, "Physics types belong to another Runtime.");
    if (auto acquired = acquire_jolt(); !acquired) return std::unexpected(acquired.error());
    std::unique_ptr<PhysicsScene> result;
    try {
        result.reset(new PhysicsScene(world, types));
    } catch (...) {
        release_jolt();
        throw;
    }
    auto subscribed = world.subscribe_lifecycle([scene = result.get()](world::World&, world::Lifecycle phase, ObjectId object) {
        return scene->lifecycle(phase, object);
    });
    if (!subscribed) return std::unexpected(subscribed.error());
    result->subscription_ = *subscribed;
    auto members = world.members();
    if (!members) return std::unexpected(members.error());
    for (const auto& member : *members) {
        if (auto added = result->reconcile(member.id()); !added) return std::unexpected(added.error());
    }
    return result;
}
PhysicsScene::~PhysicsScene() {
    if (subscription_) world_.unsubscribe_lifecycle(subscription_);
    while (!impl_->bodies.empty()) remove(impl_->bodies.begin()->first);
    impl_.reset();
    release_jolt();
}
void PhysicsScene::remove(ObjectId actor) noexcept {
    auto found = impl_->bodies.find(actor);
    if (found == impl_->bodies.end()) return;
    auto id = found->second.id;
    auto& bodies = impl_->system.GetBodyInterface();
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
    impl_->actors.erase(id);
    impl_->bodies.erase(found);
}
Result<void> PhysicsScene::reconcile(ObjectId actor) {
    auto status = world::inspect_actor(world_.runtime(), actor);
    if (!status) return std::unexpected(status.error());
    if (status->state != world::ActorState::active) { remove(actor); return {}; }
    auto group = world::components(world_.runtime(), actor);
    if (!group) return std::unexpected(group.error());
    ObjectId component;
    for (const auto& item : *group) {
        auto type = world_.runtime().type_of(item.id());
        if (!type) return std::unexpected(type.error());
        auto body = world_.runtime().is_a(*type, types_.body_component);
        if (!body) return std::unexpected(body.error());
        if (*body) {
            auto enabled = world::inspect_component(world_.runtime(), item.id());
            if (!enabled) return std::unexpected(enabled.error());
            if (enabled->enabled) {
                if (component != ObjectId{}) return failure(ErrorCode::invalid_member, "An Actor can have only one enabled physical body.");
                component = item.id();
            }
        }
    }
    if (component == ObjectId{}) { remove(actor); return {}; }
    auto root = world::root_component(world_.runtime(), actor);
    if (!root) return std::unexpected(root.error());
    if (!*root) return failure(ErrorCode::invalid_member, "A physical body requires an Actor root SceneComponent.");
    auto root_status = world::inspect_component(world_.runtime(), **root);
    if (!root_status) return std::unexpected(root_status.error());
    if (!root_status->enabled) { remove(actor); return {}; }
    auto settings = read_settings(world_.runtime(), component);
    if (!settings) return std::unexpected(settings.error());
    if (auto valid = validate(*settings); !valid) return valid;
    geometry::MeshRef mesh;
    if (settings->shape == ShapeKind::triangle_mesh) {
        auto binding = world::read_static_mesh(world_.runtime(), **root);
        if (!binding) return std::unexpected(binding.error());
        mesh = std::move(binding->mesh.resolved);
        if (!mesh) return failure(ErrorCode::invalid_member, "Triangle mesh collision requires a root mesh resource.");
    }
    auto current = impl_->bodies.find(actor);
    if (current != impl_->bodies.end() && current->second.component == component &&
        current->second.root == **root && current->second.mesh == mesh) return {};
    auto trs = world::read_transform(world_.runtime(), **root);
    if (!trs) return std::unexpected(trs.error());
    auto canonical = world::canonical_transform(*trs);
    if (!canonical) return std::unexpected(canonical.error());
    if (settings->shape != ShapeKind::triangle_mesh && canonical->scale != math::Vector3{1, 1, 1})
        return failure(ErrorCode::argument_mismatch, "Primitive physical Actor roots require unit scale.");
    JPH::ShapeRefC shape;
    if (settings->shape == ShapeKind::box)
        shape = new JPH::BoxShape(vector(settings->half_extents));
    else if (settings->shape == ShapeKind::sphere)
        shape = new JPH::SphereShape(settings->radius);
    else {
        JPH::TriangleList triangles;
        triangles.reserve(mesh->vertices().size() / 3);
        const auto vertices = mesh->vertices();
        for (std::size_t i = 0; i < vertices.size(); i += 3) {
            const auto vertex = [&](std::size_t index) {
                const auto& p = vertices[index].position;
                return JPH::Vec3(p[0] * canonical->scale.x, p[1] * canonical->scale.y,
                    p[2] * canonical->scale.z);
            };
            const auto a = vertex(i), b = vertex(i + 1), c = vertex(i + 2);
            const auto& normal = vertices[i].normal;
            if ((b - a).Cross(c - a).Dot(JPH::Vec3(normal[0], normal[1], normal[2])) >= 0)
                triangles.emplace_back(a, b, c);
            else triangles.emplace_back(a, c, b);
        }
        auto built = JPH::MeshShapeSettings(triangles).Create();
        if (built.HasError()) return failure(ErrorCode::argument_mismatch, "Mesh cannot form a Jolt collision shape.");
        shape = built.Get();
    }
    remove(actor);
    auto& bodies = impl_->system.GetBodyInterface();
    const auto id = bodies.CreateAndAddBody(JPH::BodyCreationSettings(shape,
        position(canonical->position), rotation(canonical->rotation), motion(settings->motion),
        settings->motion == MotionKind::stationary ? Impl::stationary_layer : Impl::moving_layer),
        settings->motion == MotionKind::dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (id.IsInvalid()) return failure(ErrorCode::capacity_exceeded, "Jolt could not create a physical body.");
    impl_->bodies.emplace(actor, Impl::Body{id, component, **root, settings->motion, std::move(mesh)});
    impl_->actors.emplace(id, actor);
    return {};
}
Result<void> PhysicsScene::lifecycle(world::Lifecycle phase, ObjectId object) {
    auto type = world_.runtime().type_of(object);
    if (!type) return std::unexpected(type.error());
    auto actor = world_.runtime().is_a(*type, world_.types().actor);
    if (!actor) return std::unexpected(actor.error());
    if (phase == world::Lifecycle::deactivated || phase == world::Lifecycle::detached) {
        if (*actor) remove(object);
        else for (auto it = impl_->bodies.begin(); it != impl_->bodies.end();) {
            if (it->second.component == object || it->second.root == object) {
                auto owner = it->first; ++it; remove(owner);
            } else ++it;
        }
        return {};
    }
    if (*actor) return reconcile(object);
    auto state = world::inspect_component(world_.runtime(), object);
    if (!state) return std::unexpected(state.error());
    return state->owner == ObjectId{} ? Result<void>{} : reconcile(state->owner);
}
Result<StepResult> PhysicsScene::update(float elapsed) {
    if (!std::isfinite(elapsed) || elapsed < 0 || elapsed > 1.f)
        return failure(ErrorCode::argument_mismatch, "Physics frame duration must be within [0, 1] seconds.");
    if (auto ticked = world_.update(elapsed); !ticked) return std::unexpected(ticked.error());
    if (!world_.running()) {
        while (!impl_->bodies.empty()) remove(impl_->bodies.begin()->first);
        remaining_ = 0;
        return StepResult{};
    }
    auto members = world_.members();
    if (!members) return std::unexpected(members.error());
    for (const auto& member : *members)
        if (auto found = reconcile(member.id()); !found) return std::unexpected(found.error());
    constexpr float fixed = 1.f / 60.f;
    constexpr std::uint32_t maximum_steps = 8;
    remaining_ = std::min(remaining_ + elapsed, fixed * maximum_steps);
    StepResult result;
    auto& bodies = impl_->system.GetBodyInterface();
    while (remaining_ >= fixed && result.steps < maximum_steps) {
        for (const auto& [actor, body] : impl_->bodies) {
            (void)actor;
            if (body.motion != MotionKind::kinematic) continue;
            auto trs = world::read_transform(world_.runtime(), body.root);
            if (!trs) return std::unexpected(trs.error());
            bodies.MoveKinematic(body.id, position(trs->position), rotation(trs->rotation), fixed);
        }
        if (impl_->system.Update(fixed, 1, &impl_->temporary, &impl_->jobs) != JPH::EPhysicsUpdateError::None)
            return failure(ErrorCode::invocation_failed, "Jolt physics step failed.");
        for (const auto& [actor, body] : impl_->bodies) {
            (void)actor;
            if (body.motion != MotionKind::dynamic) continue;
            JPH::RVec3 where;
            JPH::Quat orientation;
            bodies.GetPositionAndRotation(body.id, where, orientation);
            auto written = world_.apply_simulation_transform(body.root, vector(where), rotation(orientation));
            if (!written) return std::unexpected(written.error());
        }
        remaining_ -= fixed;
        ++result.steps;
    }
    result.remaining = remaining_;
    std::vector<Impl::RawContact> events;
    {
        std::lock_guard lock(impl_->contacts.mutex);
        events.swap(impl_->contacts.pending);
    }
    for (const auto& raw : events) {
        if (!handler_) break;
        auto first = impl_->actors.find(raw.first), second = impl_->actors.find(raw.second);
        if (first == impl_->actors.end() || second == impl_->actors.end()) continue;
        auto first_body = impl_->bodies.find(first->second), second_body = impl_->bodies.find(second->second);
        if (first_body == impl_->bodies.end() || second_body == impl_->bodies.end()) continue;
        ContactEvent event{raw.phase, first->second, second->second,
            first_body->second.component, second_body->second.component};
        try {
            auto delivered = handler_(event);
            if (!delivered) return std::unexpected(delivered.error());
        } catch (const std::exception& error) {
            return failure(ErrorCode::invocation_failed, error.what());
        } catch (...) {
            return failure(ErrorCode::invocation_failed, "Contact handler threw an unknown exception.");
        }
    }
    return result;
}
Result<void> PhysicsScene::set_gravity(math::Vector3 gravity) {
    if (!math::is_finite(gravity)) return failure(ErrorCode::argument_mismatch, "Gravity must be finite.");
    impl_->system.SetGravity(vector(gravity));
    return {};
}
Result<void> PhysicsScene::add_force(ObjectId actor, math::Vector3 force) {
    if (!math::is_finite(force)) return failure(ErrorCode::argument_mismatch, "Force must be finite.");
    auto found = impl_->bodies.find(actor);
    if (found == impl_->bodies.end() || found->second.motion != MotionKind::dynamic)
        return failure(ErrorCode::invalid_member, "Force requires an active dynamic body.");
    impl_->system.GetBodyInterface().AddForce(found->second.id, vector(force));
    return {};
}
Result<void> PhysicsScene::add_impulse(ObjectId actor, math::Vector3 impulse) {
    if (!math::is_finite(impulse)) return failure(ErrorCode::argument_mismatch, "Impulse must be finite.");
    auto found = impl_->bodies.find(actor);
    if (found == impl_->bodies.end() || found->second.motion != MotionKind::dynamic)
        return failure(ErrorCode::invalid_member, "Impulse requires an active dynamic body.");
    impl_->system.GetBodyInterface().AddImpulse(found->second.id, vector(impulse));
    return {};
}
std::size_t PhysicsScene::body_count() const noexcept { return impl_->bodies.size(); }

}
