#include "physics/physics.hpp"
#include "runtime/native.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <sstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace atelier::physics {
namespace {
using JPH::uint64;
std::mutex registration_mutex;
std::size_t scene_count{};
std::atomic<std::uint64_t> next_scene_id{1};

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
    if (value.shape != ShapeKind::box && value.shape != ShapeKind::sphere &&
        value.shape != ShapeKind::triangle_mesh && value.shape != ShapeKind::capsule)
        return failure(ErrorCode::argument_mismatch, "Unknown collision shape.");
    if (value.motion != MotionKind::stationary && value.motion != MotionKind::kinematic && value.motion != MotionKind::dynamic)
        return failure(ErrorCode::argument_mismatch, "Unknown body motion type.");
    if (!math::is_finite(value.half_extents) || value.half_extents.x <= 0 || value.half_extents.y <= 0 ||
        value.half_extents.z <= 0 || !std::isfinite(value.radius) || value.radius <= 0 ||
        !std::isfinite(value.capsule_half_height) || value.capsule_half_height <= 0)
        return failure(ErrorCode::argument_mismatch, "Collision dimensions must be finite and positive.");
    if (value.category >= value.responses.size() ||
        !std::isfinite(value.material.friction) || value.material.friction < 0 ||
        !std::isfinite(value.material.restitution) || value.material.restitution < 0 ||
        value.material.restitution > 1)
        return failure(ErrorCode::argument_mismatch, "Invalid collision category or physical material.");
    for (auto response : value.responses)
        if (response != Response::ignore && response != Response::overlap && response != Response::block)
            return failure(ErrorCode::argument_mismatch, "Invalid collision response.");
    if (value.shape == ShapeKind::triangle_mesh && value.motion != MotionKind::stationary)
        return failure(ErrorCode::argument_mismatch, "Triangle mesh collision requires a stationary body.");
    if (value.cooked_shape && value.shape != ShapeKind::triangle_mesh)
        return failure(ErrorCode::argument_mismatch, "Cooked collision asset requires a triangle mesh body.");
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
constexpr std::uint64_t hash_basis = 14695981039346656037ull;
std::uint64_t hash_bytes(std::string_view bytes, std::uint64_t hash = hash_basis) {
    for (const unsigned char byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}
void write_number(std::string& out, std::uint64_t value, unsigned count) {
    for (unsigned index = 0; index < count; ++index) out.push_back(char(value >> (index * 8)));
}
bool read_number(std::string_view bytes, std::size_t& cursor, unsigned count, std::uint64_t& value) {
    if (count > bytes.size() - cursor) return false;
    value = 0;
    for (unsigned index = 0; index < count; ++index)
        value |= std::uint64_t(static_cast<unsigned char>(bytes[cursor++])) << (index * 8);
    return true;
}
std::uint64_t mesh_hash(const geometry::Mesh& mesh, math::Vector3 scale) {
    std::uint64_t hash = hash_basis;
    const auto add = [&](float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned index = 0; index < 4; ++index)
            hash = (hash ^ ((bits >> (index * 8)) & 0xffu)) * 1099511628211ull;
    };
    add(scale.x); add(scale.y); add(scale.z);
    for (const auto& vertex : mesh.vertices()) {
        for (float value : vertex.position) add(value);
        for (float value : vertex.normal) add(value);
    }
    return hash;
}
Result<JPH::ShapeRefC> mesh_shape(const geometry::Mesh& mesh, math::Vector3 scale) {
    JPH::TriangleList triangles;
    triangles.reserve(mesh.vertices().size() / 3);
    const auto vertices = mesh.vertices();
    for (std::size_t index = 0; index < vertices.size(); index += 3) {
        const auto point = [&](std::size_t offset) {
            const auto& value = vertices[index + offset].position;
            return JPH::Vec3(value[0] * scale.x, value[1] * scale.y, value[2] * scale.z);
        };
        const auto a = point(0), b = point(1), c = point(2);
        const auto& normal = vertices[index].normal;
        if ((b - a).Cross(c - a).Dot(JPH::Vec3(normal[0], normal[1], normal[2])) >= 0)
            triangles.emplace_back(a, b, c);
        else triangles.emplace_back(a, c, b);
    }
    auto built = JPH::MeshShapeSettings(triangles).Create();
    if (built.HasError()) return failure(ErrorCode::argument_mismatch, "Mesh cannot form a Jolt collision shape.");
    return JPH::ShapeRefC(built.Get());
}
Result<JPH::ShapeRefC> restore_shape(std::string_view bytes, const geometry::Mesh& mesh,
    math::Vector3 scale, std::string_view source) {
    constexpr std::string_view magic = "ATJSHAPE";
    if (!bytes.starts_with(magic)) return failure(ErrorCode::invalid_storage, "Collision asset magic is invalid.");
    std::size_t cursor = magic.size();
    std::uint64_t version{}, jolt{}, source_size{}, source_hash{}, geometry_hash{}, payload_size{}, payload_hash{};
    if (!read_number(bytes, cursor, 4, version) || !read_number(bytes, cursor, 8, jolt) ||
        !read_number(bytes, cursor, 4, source_size) || !read_number(bytes, cursor, 8, source_hash) ||
        !read_number(bytes, cursor, 8, geometry_hash) || !read_number(bytes, cursor, 8, payload_size) ||
        !read_number(bytes, cursor, 8, payload_hash))
        return failure(ErrorCode::invalid_storage, "Collision asset header is truncated.");
    if (version != 1 || jolt != JPH_VERSION_ID)
        return failure(ErrorCode::invalid_storage, "Collision asset format or Jolt version requires recook.");
    if (source_size > 4096 || payload_size > 128ull * 1024 * 1024 ||
        source_size + payload_size != bytes.size() - cursor)
        return failure(ErrorCode::invalid_storage, "Collision asset length is invalid.");
    const auto stored_source = bytes.substr(cursor, static_cast<std::size_t>(source_size));
    cursor += static_cast<std::size_t>(source_size);
    const auto payload = bytes.substr(cursor);
    if (hash_bytes(stored_source) != source_hash || hash_bytes(payload) != payload_hash)
        return failure(ErrorCode::invalid_storage, "Collision asset content digest is invalid.");
    if (stored_source != source || geometry_hash != mesh_hash(mesh, scale))
        return failure(ErrorCode::invalid_storage, "Collision asset source or scale changed; recook required.");
    std::istringstream input(std::string(payload), std::ios::binary);
    JPH::StreamInWrapper stream(input);
    JPH::Shape::IDToShapeMap shapes;
    JPH::Shape::IDToMaterialMap materials;
    auto restored = JPH::Shape::sRestoreWithChildren(stream, shapes, materials);
    if (restored.HasError() || stream.IsFailed() || input.rdbuf()->in_avail() != 0)
        return failure(ErrorCode::invalid_storage, "Collision asset Jolt payload is invalid.");
    return JPH::ShapeRefC(restored.Get());
}
}

Result<std::string> cook_triangle_mesh(const geometry::MeshRef& mesh,
    math::Vector3 scale, std::string_view source_key) {
    if (!mesh || source_key.empty() || source_key.size() > 4096 ||
        !math::is_finite(scale) || scale.x <= 0 || scale.y <= 0 || scale.z <= 0)
        return failure(ErrorCode::argument_mismatch, "Cook needs a mesh, stable source key and positive scale.");
    if (auto registered = acquire_jolt(); !registered) return std::unexpected(registered.error());
    struct Registration { ~Registration() { release_jolt(); } } registration;
    auto shape = mesh_shape(*mesh, scale);
    if (!shape) return std::unexpected(shape.error());
    std::ostringstream output(std::ios::binary);
    JPH::StreamOutWrapper stream(output);
    JPH::Shape::ShapeToIDMap shapes;
    JPH::Shape::MaterialToIDMap materials;
    (*shape)->SaveWithChildren(stream, shapes, materials);
    if (stream.IsFailed()) return failure(ErrorCode::invalid_storage, "Could not serialize Jolt collision shape.");
    const auto payload = output.str();
    if (payload.size() > 128ull * 1024 * 1024)
        return failure(ErrorCode::capacity_exceeded, "Cooked collision shape is too large.");
    std::string bytes("ATJSHAPE", 8);
    write_number(bytes, 1, 4);
    write_number(bytes, JPH_VERSION_ID, 8);
    write_number(bytes, source_key.size(), 4);
    write_number(bytes, hash_bytes(source_key), 8);
    write_number(bytes, mesh_hash(*mesh, scale), 8);
    write_number(bytes, payload.size(), 8);
    write_number(bytes, hash_bytes(payload), 8);
    bytes.append(source_key);
    bytes.append(payload);
    return bytes;
}
Result<void> validate_cooked_triangle_mesh(std::string_view bytes,
    const geometry::MeshRef& mesh, math::Vector3 scale, std::string_view source_key) {
    if (!mesh || source_key.empty() || !math::is_finite(scale) ||
        scale.x <= 0 || scale.y <= 0 || scale.z <= 0)
        return failure(ErrorCode::argument_mismatch, "Collision validation needs a mesh, source and positive scale.");
    if (auto registered = acquire_jolt(); !registered) return std::unexpected(registered.error());
    struct Registration { ~Registration() { release_jolt(); } } registration;
    auto shape = restore_shape(bytes, *mesh, scale, source_key);
    if (!shape) return std::unexpected(shape.error());
    return {};
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
        Impl* owner{};
        std::mutex mutex;
        std::vector<RawContact> pending;
        std::uint32_t dropped{};
        JPH::ValidateResult OnContactValidate(const JPH::Body& first, const JPH::Body& second,
            JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
            return owner->response(first.GetID(), second.GetID()) == Response::ignore ?
                JPH::ValidateResult::RejectAllContactsForThisBodyPair : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        void push(ContactPhase phase, JPH::BodyID first, JPH::BodyID second) {
            std::lock_guard lock(mutex);
            if (pending.size() < 16384) pending.push_back({phase, first, second});
            else ++dropped;
        }
        void OnContactAdded(const JPH::Body& first, const JPH::Body& second,
            const JPH::ContactManifold&, JPH::ContactSettings& settings) override {
            settings.mIsSensor = owner->response(first.GetID(), second.GetID()) != Response::block;
            push(ContactPhase::began, first.GetID(), second.GetID());
        }
        void OnContactPersisted(const JPH::Body& first, const JPH::Body& second,
            const JPH::ContactManifold&, JPH::ContactSettings& settings) override {
            settings.mIsSensor = owner->response(first.GetID(), second.GetID()) != Response::block;
            push(ContactPhase::persisted, first.GetID(), second.GetID());
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
    struct Body {
        JPH::BodyID id;
        ObjectId component, root;
        MotionKind motion;
        BodySettings settings;
        geometry::MeshRef mesh;
        std::uint64_t revision;
    };
    enum class CommandKind { force, impulse, kinematic_target, teleport };
    struct Command {
        CommandKind kind;
        BodyHandle handle;
        math::Vector3 value;
        math::Quaternion rotation{0, 0, 0, 1};
        bool preserve_velocity{};
    };
    std::vector<Command> commands;
    std::unordered_map<ObjectId, Body> bodies;
    std::unordered_map<JPH::BodyID, ObjectId> actors;
    struct Distance {
        BodyHandle first, second;
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
    };
    std::map<std::uint64_t, Distance> distances;
    struct Character {
        ObjectId root;
        std::uint64_t revision;
        CharacterSettings settings;
        JPH::Ref<JPH::CharacterVirtual> character;
        math::Vector3 input{};
        bool jump{};
    };
    std::unordered_map<ObjectId, Character> characters;
    struct Ragdoll {
        ObjectId root;
        std::uint64_t revision;
        JPH::Ref<JPH::Ragdoll> ragdoll;
    };
    std::unordered_map<ObjectId, Ragdoll> ragdolls;
    std::unordered_set<JPH::BodyID> ragdoll_ids;
    struct Vehicle {
        BodyHandle chassis;
        std::uint64_t revision;
        JPH::Ref<JPH::VehicleConstraint> constraint;
        JPH::Ref<JPH::VehicleCollisionTesterRay> tester;
    };
    std::unordered_map<ObjectId, Vehicle> vehicles;
    struct PairState { std::uint32_t contacts{}; ContactEvent event; };
    std::map<std::uint64_t, PairState> active_pairs;
    std::vector<ContactEvent> ready_events;
    static std::uint64_t pair_key(JPH::BodyID first, JPH::BodyID second) {
        const auto a = first.GetIndexAndSequenceNumber();
        const auto b = second.GetIndexAndSequenceNumber();
        return (std::uint64_t{std::min(a, b)} << 32) | std::max(a, b);
    }
    std::optional<ContactEvent> make_event(JPH::BodyID first, JPH::BodyID second,
        std::uint64_t step) const {
        auto a = actors.find(first), b = actors.find(second);
        if (a == actors.end() || b == actors.end()) return std::nullopt;
        auto x = bodies.find(a->second), y = bodies.find(b->second);
        if (x == bodies.end() || y == bodies.end()) return std::nullopt;
        if (first.GetIndexAndSequenceNumber() > second.GetIndexAndSequenceNumber()) {
            std::swap(a, b);
            std::swap(x, y);
        }
        return ContactEvent{ContactPhase::began, a->second, b->second,
            x->second.component, y->second.component, step,
            response(first, second) != Response::block};
    }
    void collect_contacts(std::uint64_t step, StepResult& result) {
        std::vector<RawContact> raw;
        {
            std::lock_guard lock(contacts.mutex);
            raw.swap(contacts.pending);
            result.dropped_contact_notifications += contacts.dropped;
            contacts.dropped = 0;
        }
        result.contacts_incomplete = result.dropped_contact_notifications != 0;
        std::map<std::uint64_t, bool> previous;
        for (const auto& [key, state] : active_pairs) previous.emplace(key, state.contacts != 0);
        for (const auto& notice : raw) {
            const auto key = pair_key(notice.first, notice.second);
            if (notice.phase == ContactPhase::began) {
                auto event = make_event(notice.first, notice.second, step);
                if (!event) continue;
                auto& state = active_pairs[key];
                if (state.contacts == 0) state.event = *event;
                ++state.contacts;
            } else if (notice.phase == ContactPhase::ended) {
                auto found = active_pairs.find(key);
                if (found != active_pairs.end() && found->second.contacts != 0) --found->second.contacts;
            }
        }
        for (auto it = active_pairs.begin(); it != active_pairs.end();) {
            auto& state = it->second;
            if (state.contacts == 0) {
                if (previous.contains(it->first)) {
                    auto event = state.event;
                    event.phase = ContactPhase::ended;
                    event.step_id = step;
                    ready_events.push_back(event);
                }
                it = active_pairs.erase(it);
                continue;
            }
            auto event = state.event;
            event.phase = previous.contains(it->first) ? ContactPhase::persisted : ContactPhase::began;
            event.step_id = step;
            ready_events.push_back(event);
            ++it;
        }
    }
    void remove_pairs(JPH::BodyID id, std::uint64_t step) {
        const auto value = id.GetIndexAndSequenceNumber();
        for (auto it = active_pairs.begin(); it != active_pairs.end();) {
            if (static_cast<std::uint32_t>(it->first >> 32) == value ||
                static_cast<std::uint32_t>(it->first) == value) {
                auto event = it->second.event;
                event.phase = ContactPhase::ended;
                event.exit_reason = ContactExitReason::body_removed;
                event.step_id = step;
                ready_events.push_back(event);
                it = active_pairs.erase(it);
            } else ++it;
        }
    }
    Response response(JPH::BodyID first, JPH::BodyID second) const {
        auto a = actors.find(first), b = actors.find(second);
        if (a == actors.end() || b == actors.end()) {
            const bool left_ragdoll = ragdoll_ids.contains(first);
            const bool right_ragdoll = ragdoll_ids.contains(second);
            if ((left_ragdoll && (right_ragdoll || b != actors.end())) ||
                (right_ragdoll && a != actors.end())) return Response::block;
            return Response::ignore;
        }
        auto left = bodies.find(a->second), right = bodies.find(b->second);
        if (left == bodies.end() || right == bodies.end()) return Response::ignore;
        const auto& x = left->second.settings;
        const auto& y = right->second.settings;
        const auto xy = x.responses[y.category], yx = y.responses[x.category];
        if (xy == Response::ignore || yx == Response::ignore) return Response::ignore;
        if (x.sensor || y.sensor || xy == Response::overlap || yx == Response::overlap)
            return Response::overlap;
        return Response::block;
    }
    bool query_accept(JPH::BodyID id, const QueryFilter& filter) const {
        auto owner = actors.find(id);
        if (owner == actors.end() || owner->second == filter.ignore_actor) return false;
        auto body = bodies.find(owner->second);
        return body != bodies.end() &&
            body->second.component != filter.ignore_component &&
            ((filter.categories >> body->second.settings.category) & 1u) != 0 &&
            filter.responses[body->second.settings.category] != Response::ignore &&
            body->second.settings.responses[filter.category] != Response::ignore;
    }
    QueryHit query_hit(JPH::BodyID id, std::uint32_t sub_shape, float fraction,
        math::Vector3 point, math::Vector3 normal, bool initial,
        std::uint64_t scene, const QueryFilter& filter) const {
        const auto actor = actors.at(id);
        const auto& body = bodies.at(actor);
        const bool blocking = !body.settings.sensor &&
            body.settings.responses[filter.category] == Response::block &&
            filter.responses[body.settings.category] == Response::block;
        return {{actor, body.component, scene, body.revision}, fraction, point, normal,
            sub_shape, body.settings.sensor, initial, blocking, body.settings.material};
    }

    Impl() {
        contacts.owner = this;
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
    : world_(world), types_(types), impl_(std::make_unique<Impl>()), scene_id_(next_scene_id++) {}
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
    while (!impl_->characters.empty()) remove_special(impl_->characters.begin()->first);
    while (!impl_->ragdolls.empty()) remove_special(impl_->ragdolls.begin()->first);
    while (!impl_->vehicles.empty()) remove_special(impl_->vehicles.begin()->first);
    while (!impl_->distances.empty()) {
        auto found = impl_->distances.begin();
        impl_->system.RemoveConstraint(found->second.constraint);
        impl_->distances.erase(found);
    }
    while (!impl_->bodies.empty()) remove(impl_->bodies.begin()->first);
    impl_.reset();
    release_jolt();
}
void PhysicsScene::remove(ObjectId actor) noexcept {
    auto found = impl_->bodies.find(actor);
    if (found == impl_->bodies.end()) return;
    remove_special(actor);
    for (auto it = impl_->distances.begin(); it != impl_->distances.end();) {
        if (it->second.first.actor == actor || it->second.second.actor == actor) {
            impl_->system.RemoveConstraint(it->second.constraint);
            it = impl_->distances.erase(it);
        } else ++it;
    }
    auto id = found->second.id;
    impl_->remove_pairs(id, completed_step_);
    auto& bodies = impl_->system.GetBodyInterface();
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
    impl_->actors.erase(id);
    impl_->bodies.erase(found);
}
void PhysicsScene::remove_special(ObjectId actor) noexcept {
    if (auto found = impl_->vehicles.find(actor); found != impl_->vehicles.end()) {
        impl_->system.RemoveStepListener(found->second.constraint);
        impl_->system.RemoveConstraint(found->second.constraint);
        impl_->vehicles.erase(found);
    }
    impl_->characters.erase(actor);
    if (auto found = impl_->ragdolls.find(actor); found != impl_->ragdolls.end()) {
        for (const auto id : found->second.ragdoll->GetBodyIDs()) impl_->ragdoll_ids.erase(id);
        found->second.ragdoll->RemoveFromPhysicsSystem();
        impl_->ragdolls.erase(found);
    }
}
Result<void> PhysicsScene::reconcile(ObjectId actor) {
    auto status = world::inspect_actor(world_.runtime(), actor);
    if (!status) return std::unexpected(status.error());
    if (status->state != world::ActorState::active) { remove_special(actor); remove(actor); return {}; }
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
        current->second.root == **root && current->second.mesh == mesh &&
        current->second.settings == *settings) return {};
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
    else if (settings->shape == ShapeKind::capsule)
        shape = new JPH::CapsuleShape(settings->capsule_half_height, settings->radius);
    else {
        std::string source;
        if (settings->cooked_shape) {
            auto binding = world::read_static_mesh(world_.runtime(), **root);
            if (!binding) return std::unexpected(binding.error());
            source = binding->mesh.source;
        }
        auto built = settings->cooked_shape ?
            restore_shape(*settings->cooked_shape, *mesh, canonical->scale, source) :
            mesh_shape(*mesh, canonical->scale);
        if (!built) return std::unexpected(built.error());
        shape = *built;
    }
    remove(actor);
    auto& bodies = impl_->system.GetBodyInterface();
    JPH::BodyCreationSettings creation(shape,
        position(canonical->position), rotation(canonical->rotation), motion(settings->motion),
        settings->motion == MotionKind::stationary ? Impl::stationary_layer : Impl::moving_layer);
    creation.mIsSensor = settings->sensor;
    creation.mFriction = settings->material.friction;
    creation.mRestitution = settings->material.restitution;
    const auto id = bodies.CreateAndAddBody(creation,
        settings->motion == MotionKind::dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (id.IsInvalid()) return failure(ErrorCode::capacity_exceeded, "Jolt could not create a physical body.");
    remove_special(actor);
    impl_->bodies.emplace(actor, Impl::Body{id, component, **root, settings->motion, *settings,
        std::move(mesh), next_revision_++});
    impl_->actors.emplace(id, actor);
    return {};
}
Result<void> PhysicsScene::lifecycle(world::Lifecycle phase, ObjectId object) {
    auto type = world_.runtime().type_of(object);
    if (!type) return std::unexpected(type.error());
    auto actor = world_.runtime().is_a(*type, world_.types().actor);
    if (!actor) return std::unexpected(actor.error());
    if (phase == world::Lifecycle::deactivated || phase == world::Lifecycle::detached) {
        if (*actor) { remove_special(object); remove(object); }
        else for (auto it = impl_->bodies.begin(); it != impl_->bodies.end();) {
            if (it->second.component == object || it->second.root == object) {
                auto owner = it->first; ++it; remove(owner);
            } else ++it;
        }
        if (!*actor) for (auto it = impl_->characters.begin(); it != impl_->characters.end();) {
            if (it->second.root == object) { auto owner = it->first; ++it; remove_special(owner); }
            else ++it;
        }
        if (!*actor) for (auto it = impl_->ragdolls.begin(); it != impl_->ragdolls.end();) {
            if (it->second.root == object) { auto owner = it->first; ++it; remove_special(owner); }
            else ++it;
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
    if (updating_) return failure(ErrorCode::busy, "Physics update cannot be nested.");
    updating_ = true;
    struct ResetUpdate { bool& flag; ~ResetUpdate() { flag = false; } } reset{updating_};
    if (auto ticked = world_.update(elapsed); !ticked) return std::unexpected(ticked.error());
    if (!world_.running()) {
        while (!impl_->characters.empty()) remove_special(impl_->characters.begin()->first);
        while (!impl_->ragdolls.empty()) remove_special(impl_->ragdolls.begin()->first);
        while (!impl_->bodies.empty()) remove(impl_->bodies.begin()->first);
        impl_->commands.clear();
        remaining_ = 0;
        single_step_ = false;
        return StepResult{.completed_step = completed_step_};
    }
    auto members = world_.members();
    if (!members) return std::unexpected(members.error());
    for (const auto& member : *members)
        if (auto found = reconcile(member.id()); !found) return std::unexpected(found.error());
    constexpr float fixed = 1.f / 60.f;
    constexpr std::uint32_t maximum_steps = 8;
    StepResult result{.completed_step = completed_step_};
    if (!paused_) {
        const float accumulated = remaining_ + elapsed;
        remaining_ = std::min(accumulated, fixed * maximum_steps);
        result.dropped = accumulated - remaining_;
    }
    auto& bodies = impl_->system.GetBodyInterface();
    const bool stepping_once = paused_ && single_step_;
    single_step_ = false;
    while (((stepping_once && result.steps == 0) ||
        (!paused_ && remaining_ + 1e-7f >= fixed)) && result.steps < maximum_steps) {
        if (before_step_) {
            try {
                auto called = before_step_(completed_step_ + 1, fixed);
                if (!called) return std::unexpected(called.error());
            } catch (const std::exception& error) {
                return failure(ErrorCode::invocation_failed, error.what());
            } catch (...) {
                return failure(ErrorCode::invocation_failed, "Before-step handler threw an unknown exception.");
            }
        }
        std::vector<Impl::Command> commands;
        commands.swap(impl_->commands);
        for (const auto& command : commands) {
            auto found = impl_->bodies.find(command.handle.actor);
            if (found == impl_->bodies.end() || found->second.component != command.handle.component ||
                command.handle.scene != scene_id_ || found->second.revision != command.handle.revision) {
                ++result.rejected_commands;
                continue;
            }
            const auto& body = found->second;
            switch (command.kind) {
            case Impl::CommandKind::force:
                bodies.AddForce(body.id, vector(command.value));
                break;
            case Impl::CommandKind::impulse:
                bodies.AddImpulse(body.id, vector(command.value));
                break;
            case Impl::CommandKind::kinematic_target:
                bodies.MoveKinematic(body.id, position(command.value), rotation(command.rotation), fixed);
                break;
            case Impl::CommandKind::teleport:
                bodies.SetPositionAndRotation(body.id, position(command.value), rotation(command.rotation),
                    body.motion == MotionKind::dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
                if (!command.preserve_velocity)
                    bodies.SetLinearAndAngularVelocity(body.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                break;
            }
        }
        for (auto& [actor, controlled] : impl_->characters) {
            (void)actor;
            auto& character = *controlled.character;
            character.UpdateGroundVelocity();
            auto velocity = character.GetLinearVelocity();
            const bool grounded = character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
            const auto ground = grounded ? character.GetGroundVelocity() : JPH::Vec3::sZero();
            const auto gravity = impl_->system.GetGravity();
            velocity = JPH::Vec3(controlled.input.x + ground.GetX(),
                grounded ? ground.GetY() : velocity.GetY() + gravity.GetY() * fixed,
                controlled.input.z + ground.GetZ());
            if (controlled.jump && grounded) velocity.SetY(controlled.settings.jump_speed);
            controlled.jump = false;
            character.SetLinearVelocity(velocity);
            JPH::CharacterVirtual::ExtendedUpdateSettings step_settings;
            step_settings.mWalkStairsStepUp = JPH::Vec3(0, controlled.settings.step_height, 0);
            step_settings.mStickToFloorStepDown = JPH::Vec3(0, -controlled.settings.step_height, 0);
            character.ExtendedUpdate(fixed, gravity, step_settings,
                impl_->system.GetDefaultBroadPhaseLayerFilter(Impl::moving_layer),
                impl_->system.GetDefaultLayerFilter(Impl::moving_layer), {}, {}, impl_->temporary);
        }
        if (impl_->system.Update(fixed, 1, &impl_->temporary, &impl_->jobs) != JPH::EPhysicsUpdateError::None)
            return failure(ErrorCode::invocation_failed, "Jolt physics step failed.");
        for (const auto& [actor, body] : impl_->bodies) {
            (void)actor;
            if (body.motion == MotionKind::stationary) continue;
            JPH::RVec3 where;
            JPH::Quat orientation;
            bodies.GetPositionAndRotation(body.id, where, orientation);
            auto written = world_.apply_simulation_transform(body.root, vector(where), rotation(orientation));
            if (!written) return std::unexpected(written.error());
        }
        for (const auto& [actor, controlled] : impl_->characters) {
            (void)actor;
            auto written = world_.apply_simulation_transform(controlled.root,
                vector(controlled.character->GetPosition()), rotation(controlled.character->GetRotation()));
            if (!written) return std::unexpected(written.error());
        }
        if (!stepping_once) remaining_ = std::max(0.f, remaining_ - fixed);
        ++result.steps;
        result.completed_step = ++completed_step_;
        impl_->collect_contacts(completed_step_, result);
        if (after_step_) {
            try {
                auto called = after_step_(completed_step_, fixed);
                if (!called) return std::unexpected(called.error());
            } catch (const std::exception& error) {
                return failure(ErrorCode::invocation_failed, error.what());
            } catch (...) {
                return failure(ErrorCode::invocation_failed, "After-step handler threw an unknown exception.");
            }
        }
    }
    result.remaining = remaining_;
    std::vector<ContactEvent> events;
    events.swap(impl_->ready_events);
    for (const auto& event : events) {
        if (!handler_) break;
        if (!world_.runtime().type_of(event.first_actor) ||
            !world_.runtime().type_of(event.second_actor)) continue;
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
    auto handle = body_handle(actor);
    if (!handle) return std::unexpected(handle.error());
    if (impl_->bodies.at(actor).motion != MotionKind::dynamic)
        return failure(ErrorCode::invalid_member, "Force requires an active dynamic body.");
    impl_->commands.push_back({Impl::CommandKind::force, *handle, force});
    return {};
}
Result<void> PhysicsScene::add_impulse(ObjectId actor, math::Vector3 impulse) {
    if (!math::is_finite(impulse)) return failure(ErrorCode::argument_mismatch, "Impulse must be finite.");
    auto handle = body_handle(actor);
    if (!handle) return std::unexpected(handle.error());
    if (impl_->bodies.at(actor).motion != MotionKind::dynamic)
        return failure(ErrorCode::invalid_member, "Impulse requires an active dynamic body.");
    impl_->commands.push_back({Impl::CommandKind::impulse, *handle, impulse});
    return {};
}
Result<BodyHandle> PhysicsScene::body_handle(ObjectId actor) const {
    auto found = impl_->bodies.find(actor);
    if (found == impl_->bodies.end())
        return failure(ErrorCode::invalid_member, "Actor has no active physical body in this scene.");
    return BodyHandle{actor, found->second.component, scene_id_, found->second.revision};
}
Result<BodyState> PhysicsScene::read_body_state(BodyHandle handle) const {
    auto found = impl_->bodies.find(handle.actor);
    if (found == impl_->bodies.end() || found->second.component != handle.component ||
        handle.scene != scene_id_ || found->second.revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Physical body handle is stale or belongs to another scene.");
    auto& bodies = impl_->system.GetBodyInterface();
    JPH::RVec3 where;
    JPH::Quat orientation;
    bodies.GetPositionAndRotation(found->second.id, where, orientation);
    const auto linear = bodies.GetLinearVelocity(found->second.id);
    const auto angular = bodies.GetAngularVelocity(found->second.id);
    return BodyState{handle, completed_step_, vector(where),
        {linear.GetX(), linear.GetY(), linear.GetZ()},
        {angular.GetX(), angular.GetY(), angular.GetZ()}, rotation(orientation),
        !bodies.IsActive(found->second.id)};
}
Result<void> PhysicsScene::set_kinematic_target(BodyHandle handle, math::Vector3 where,
    math::Quaternion orientation) {
    auto state = read_body_state(handle);
    if (!state) return std::unexpected(state.error());
    if (impl_->bodies.at(handle.actor).motion != MotionKind::kinematic)
        return failure(ErrorCode::invalid_member, "Kinematic target requires a kinematic body.");
    auto valid = world::canonical_transform({where, orientation, {1, 1, 1}});
    if (!valid) return std::unexpected(valid.error());
    impl_->commands.push_back({Impl::CommandKind::kinematic_target, handle,
        valid->position, valid->rotation});
    return {};
}
Result<void> PhysicsScene::teleport(BodyHandle handle, math::Vector3 where,
    math::Quaternion orientation, bool preserve_velocity) {
    auto state = read_body_state(handle);
    if (!state) return std::unexpected(state.error());
    auto valid = world::canonical_transform({where, orientation, {1, 1, 1}});
    if (!valid) return std::unexpected(valid.error());
    impl_->commands.push_back({Impl::CommandKind::teleport, handle,
        valid->position, valid->rotation, preserve_velocity});
    return {};
}
Result<ConstraintHandle> PhysicsScene::create_distance_constraint(BodyHandle first,
    BodyHandle second, float minimum, float maximum) {
    if (first.actor == second.actor || !std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum < 0 || maximum < minimum)
        return failure(ErrorCode::argument_mismatch, "Distance constraint requires two bodies and a valid range.");
    if (auto state = read_body_state(first); !state) return std::unexpected(state.error());
    if (auto state = read_body_state(second); !state) return std::unexpected(state.error());
    const auto left = impl_->bodies.at(first.actor).id;
    const auto right = impl_->bodies.at(second.actor).id;
    const std::array<JPH::BodyID, 2> ids{left, right};
    JPH::BodyLockMultiWrite locks(impl_->system.GetBodyLockInterface(), ids.data(), ids.size());
    if (!locks.GetBody(0) || !locks.GetBody(1))
        return failure(ErrorCode::busy, "Could not lock distance constraint bodies.");
    JPH::DistanceConstraintSettings settings;
    settings.mPoint1 = locks.GetBody(0)->GetPosition();
    settings.mPoint2 = locks.GetBody(1)->GetPosition();
    settings.mMinDistance = minimum;
    settings.mMaxDistance = maximum;
    JPH::Ref<JPH::TwoBodyConstraint> constraint = settings.Create(*locks.GetBody(0), *locks.GetBody(1));
    impl_->system.AddConstraint(constraint);
    const auto revision = next_revision_++;
    impl_->distances.emplace(revision, Impl::Distance{first, second, constraint});
    return ConstraintHandle{scene_id_, revision};
}
Result<void> PhysicsScene::destroy_constraint(ConstraintHandle handle) {
    auto found = impl_->distances.find(handle.revision);
    if (handle.scene != scene_id_ || found == impl_->distances.end())
        return failure(ErrorCode::invalid_member, "Distance constraint handle is stale.");
    impl_->system.RemoveConstraint(found->second.constraint);
    impl_->distances.erase(found);
    return {};
}
Result<SpecialHandle> PhysicsScene::create_character(ObjectId actor, CharacterSettings settings) {
    if (!std::isfinite(settings.radius) || settings.radius <= 0 ||
        !std::isfinite(settings.capsule_half_height) || settings.capsule_half_height <= 0 ||
        !std::isfinite(settings.max_slope_radians) || settings.max_slope_radians < 0 ||
        settings.max_slope_radians >= 1.5707963f || !std::isfinite(settings.jump_speed) ||
        settings.jump_speed < 0 || !std::isfinite(settings.step_height) || settings.step_height < 0)
        return failure(ErrorCode::argument_mismatch, "Invalid character dimensions or movement settings.");
    if (!world_.running() || impl_->characters.contains(actor) || impl_->bodies.contains(actor) ||
        impl_->ragdolls.contains(actor))
        return failure(ErrorCode::invalid_member, "Character requires an active Actor without another physics body.");
    auto state = world::inspect_actor(world_.runtime(), actor);
    if (!state) return std::unexpected(state.error());
    if (state->state != world::ActorState::active)
        return failure(ErrorCode::invalid_member, "Character Actor must be active in this World.");
    auto root = world::root_component(world_.runtime(), actor);
    if (!root) return std::unexpected(root.error());
    if (!*root) return failure(ErrorCode::invalid_member, "Character Actor needs a root SceneComponent.");
    auto root_status = world::inspect_component(world_.runtime(), **root);
    if (!root_status) return std::unexpected(root_status.error());
    if (!root_status->enabled)
        return failure(ErrorCode::invalid_member, "Character root SceneComponent must be enabled.");
    auto trs = world::read_transform(world_.runtime(), **root);
    if (!trs) return std::unexpected(trs.error());
    if (trs->scale != math::Vector3{1, 1, 1})
        return failure(ErrorCode::argument_mismatch, "Character root requires unit scale.");
    JPH::Ref<JPH::CharacterVirtualSettings> jolt = new JPH::CharacterVirtualSettings();
    jolt->mShape = new JPH::CapsuleShape(settings.capsule_half_height, settings.radius);
    jolt->mMaxSlopeAngle = settings.max_slope_radians;
    JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(jolt,
        position(trs->position), rotation(trs->rotation), 0, &impl_->system);
    const auto revision = next_revision_++;
    impl_->characters.emplace(actor, Impl::Character{**root, revision, settings, character});
    return SpecialHandle{actor, scene_id_, revision};
}
Result<void> PhysicsScene::set_character_input(SpecialHandle handle,
    math::Vector3 planar_velocity, bool jump) {
    auto found = impl_->characters.find(handle.actor);
    if (handle.scene != scene_id_ || found == impl_->characters.end() ||
        found->second.revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Character handle is stale.");
    if (!math::is_finite(planar_velocity) || planar_velocity.y != 0)
        return failure(ErrorCode::argument_mismatch, "Character input needs a finite horizontal velocity.");
    found->second.input = planar_velocity;
    found->second.jump |= jump;
    return {};
}
Result<CharacterState> PhysicsScene::read_character_state(SpecialHandle handle) const {
    auto found = impl_->characters.find(handle.actor);
    if (handle.scene != scene_id_ || found == impl_->characters.end() ||
        found->second.revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Character handle is stale.");
    const auto& character = *found->second.character;
    const auto velocity = character.GetLinearVelocity();
    return CharacterState{handle, completed_step_, vector(character.GetPosition()),
        {velocity.GetX(), velocity.GetY(), velocity.GetZ()},
        character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround};
}
Result<void> PhysicsScene::destroy_character(SpecialHandle handle) {
    if (auto state = read_character_state(handle); !state) return std::unexpected(state.error());
    impl_->characters.erase(handle.actor);
    return {};
}
Result<SpecialHandle> PhysicsScene::create_ragdoll(ObjectId actor,
    std::span<const RagdollPart> parts) {
    if (parts.empty() || parts.size() > 128 || !world_.running() ||
        impl_->bodies.contains(actor) || impl_->characters.contains(actor) || impl_->ragdolls.contains(actor))
        return failure(ErrorCode::argument_mismatch, "Ragdoll requires 1-128 parts and an unused active Actor.");
    auto state = world::inspect_actor(world_.runtime(), actor);
    if (!state) return std::unexpected(state.error());
    if (state->state != world::ActorState::active)
        return failure(ErrorCode::invalid_member, "Ragdoll Actor must be active in this World.");
    auto root = world::root_component(world_.runtime(), actor);
    if (!root) return std::unexpected(root.error());
    if (!*root) return failure(ErrorCode::invalid_member, "Ragdoll Actor needs a root SceneComponent.");
    auto root_status = world::inspect_component(world_.runtime(), **root);
    if (!root_status) return std::unexpected(root_status.error());
    if (!root_status->enabled)
        return failure(ErrorCode::invalid_member, "Ragdoll root SceneComponent must be enabled.");
    auto trs = world::read_transform(world_.runtime(), **root);
    if (!trs) return std::unexpected(trs.error());
    if (trs->scale != math::Vector3{1, 1, 1})
        return failure(ErrorCode::argument_mismatch, "Ragdoll root requires unit scale.");
    const auto world_part = [&](math::Vector3 local) {
        const auto rotated = math::rotate(trs->rotation, local);
        return position({trs->position.x + rotated.x, trs->position.y + rotated.y,
            trs->position.z + rotated.z});
    };
    JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();
    settings->mSkeleton = new JPH::Skeleton();
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& source = parts[index];
        if (source.name.empty() || source.parent < -1 || source.parent >= static_cast<int>(index) ||
            !math::is_finite(source.local_position) || !std::isfinite(source.radius) || source.radius <= 0 ||
            !std::isfinite(source.capsule_half_height) || source.capsule_half_height <= 0)
            return failure(ErrorCode::argument_mismatch, "Ragdoll part has invalid hierarchy or dimensions.");
        settings->mSkeleton->AddJoint(source.name, source.parent);
        JPH::RagdollSettings::Part part;
        part.SetShape(new JPH::CapsuleShape(source.capsule_half_height, source.radius));
        part.mPosition = world_part(source.local_position);
        part.mRotation = rotation(trs->rotation);
        part.mMotionType = JPH::EMotionType::Dynamic;
        part.mObjectLayer = Impl::moving_layer;
        if (source.parent >= 0) {
            JPH::Ref<JPH::PointConstraintSettings> joint = new JPH::PointConstraintSettings();
            const auto& parent = parts[source.parent];
            joint->mPoint1 = joint->mPoint2 = world_part({
                (parent.local_position.x + source.local_position.x) * .5f,
                (parent.local_position.y + source.local_position.y) * .5f,
                (parent.local_position.z + source.local_position.z) * .5f});
            part.mToParent = joint;
        }
        settings->mParts.push_back(part);
    }
    settings->mSkeleton->CalculateParentJointIndices();
    settings->DisableParentChildCollisions();
    JPH::Ref<JPH::Ragdoll> ragdoll = settings->CreateRagdoll(
        static_cast<JPH::CollisionGroup::GroupID>(next_revision_), 0, &impl_->system);
    if (!ragdoll) return failure(ErrorCode::capacity_exceeded, "Jolt could not create the ragdoll.");
    ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
    for (const auto id : ragdoll->GetBodyIDs()) impl_->ragdoll_ids.insert(id);
    const auto revision = next_revision_++;
    impl_->ragdolls.emplace(actor, Impl::Ragdoll{**root, revision, ragdoll});
    return SpecialHandle{actor, scene_id_, revision};
}
Result<RagdollPose> PhysicsScene::read_ragdoll_pose(SpecialHandle handle) const {
    auto found = impl_->ragdolls.find(handle.actor);
    if (handle.scene != scene_id_ || found == impl_->ragdolls.end() ||
        found->second.revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Ragdoll handle is stale.");
    RagdollPose pose{.handle = handle, .step_id = completed_step_, .positions = {}, .rotations = {}};
    auto& bodies = impl_->system.GetBodyInterface();
    for (const auto id : found->second.ragdoll->GetBodyIDs()) {
        JPH::RVec3 where;
        JPH::Quat orientation;
        bodies.GetPositionAndRotation(id, where, orientation);
        pose.positions.push_back(vector(where));
        pose.rotations.push_back(rotation(orientation));
    }
    return pose;
}
Result<void> PhysicsScene::destroy_ragdoll(SpecialHandle handle) {
    if (auto pose = read_ragdoll_pose(handle); !pose) return std::unexpected(pose.error());
    remove_special(handle.actor);
    return {};
}
Result<SpecialHandle> PhysicsScene::create_vehicle(BodyHandle chassis, VehicleSettings settings) {
    if (auto state = read_body_state(chassis); !state) return std::unexpected(state.error());
    if (impl_->bodies.at(chassis.actor).motion != MotionKind::dynamic ||
        impl_->vehicles.contains(chassis.actor) || !std::isfinite(settings.half_width) ||
        settings.half_width <= 0 || !std::isfinite(settings.half_length) ||
        settings.half_length <= settings.wheel_radius || !std::isfinite(settings.wheel_radius) ||
        settings.wheel_radius <= 0 || !std::isfinite(settings.suspension_length) ||
        settings.suspension_length <= 0)
        return failure(ErrorCode::argument_mismatch, "Vehicle requires a dynamic chassis and positive wheel dimensions.");
    const auto id = impl_->bodies.at(chassis.actor).id;
    JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return failure(ErrorCode::busy, "Could not lock vehicle chassis.");
    JPH::VehicleConstraintSettings vehicle;
    for (int wheel = 0; wheel < 4; ++wheel) {
        JPH::Ref<JPH::WheelSettingsWV> item = new JPH::WheelSettingsWV();
        item->mPosition = JPH::Vec3(wheel % 2 == 0 ? settings.half_width : -settings.half_width,
            -.25f, wheel < 2 ? settings.half_length : -settings.half_length);
        item->mRadius = settings.wheel_radius;
        item->mWidth = settings.wheel_radius * .4f;
        item->mSuspensionMinLength = 0;
        item->mSuspensionMaxLength = settings.suspension_length;
        item->mMaxSteerAngle = wheel < 2 ? .6f : 0.f;
        vehicle.mWheels.push_back(JPH::Ref<JPH::WheelSettings>(item.GetPtr()));
    }
    JPH::Ref<JPH::WheeledVehicleControllerSettings> controller = new JPH::WheeledVehicleControllerSettings();
    controller->mDifferentials.resize(1);
    controller->mDifferentials[0].mLeftWheel = 2;
    controller->mDifferentials[0].mRightWheel = 3;
    vehicle.mController = controller;
    JPH::Ref<JPH::VehicleConstraint> constraint = new JPH::VehicleConstraint(lock.GetBody(), vehicle);
    JPH::Ref<JPH::VehicleCollisionTesterRay> tester =
        new JPH::VehicleCollisionTesterRay(Impl::moving_layer);
    constraint->SetVehicleCollisionTester(tester);
    impl_->system.AddConstraint(constraint);
    impl_->system.AddStepListener(constraint);
    const auto revision = next_revision_++;
    impl_->vehicles.emplace(chassis.actor, Impl::Vehicle{chassis, revision, constraint, tester});
    return SpecialHandle{chassis.actor, scene_id_, revision};
}
Result<void> PhysicsScene::set_vehicle_input(SpecialHandle handle,
    float throttle, float steering, float brake) {
    auto found = impl_->vehicles.find(handle.actor);
    if (handle.scene != scene_id_ || found == impl_->vehicles.end() ||
        found->second.revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Vehicle handle is stale.");
    if (!std::isfinite(throttle) || std::abs(throttle) > 1 || !std::isfinite(steering) ||
        std::abs(steering) > 1 || !std::isfinite(brake) || brake < 0 || brake > 1)
        return failure(ErrorCode::argument_mismatch, "Vehicle controls are out of range.");
    auto* controller = static_cast<JPH::WheeledVehicleController*>(found->second.constraint->GetController());
    controller->SetDriverInput(throttle, steering, brake, 0);
    impl_->system.GetBodyInterface().ActivateBody(impl_->bodies.at(handle.actor).id);
    return {};
}
Result<VehicleState> PhysicsScene::read_vehicle_state(SpecialHandle handle) const {
    auto found = impl_->vehicles.find(handle.actor);
    if (handle.scene != scene_id_ || found == impl_->vehicles.end() ||
        found->second.revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Vehicle handle is stale.");
    auto chassis = read_body_state(found->second.chassis);
    if (!chassis) return std::unexpected(chassis.error());
    VehicleState state{handle, completed_step_, chassis->position, chassis->linear_velocity};
    for (int i = 0; i < 4; ++i)
        state.wheel_contact[i] = found->second.constraint->GetWheel(i)->HasContact();
    return state;
}
Result<void> PhysicsScene::destroy_vehicle(SpecialHandle handle) {
    if (auto state = read_vehicle_state(handle); !state) return std::unexpected(state.error());
    remove_special(handle.actor);
    return {};
}
namespace {
Result<void> validate_query(const QueryFilter& filter) {
    if (filter.capacity == 0 || filter.capacity > 4096 ||
        filter.category >= filter.responses.size() ||
        (filter.mode != QueryMode::any && filter.mode != QueryMode::closest && filter.mode != QueryMode::all))
        return failure(ErrorCode::argument_mismatch, "Invalid physics query mode or capacity.");
    for (auto response : filter.responses)
        if (response != Response::ignore && response != Response::overlap && response != Response::block)
            return failure(ErrorCode::argument_mismatch, "Invalid physics query response.");
    return {};
}
void finish_query(QueryResult& result, const QueryFilter& filter) {
    std::sort(result.hits.begin(), result.hits.end(), [](const QueryHit& a, const QueryHit& b) {
        if (a.fraction != b.fraction) return a.fraction < b.fraction;
        if (a.body.revision != b.body.revision) return a.body.revision < b.body.revision;
        return a.sub_shape < b.sub_shape;
    });
    const auto limit = filter.mode == QueryMode::all ? filter.capacity : std::size_t{1};
    if (result.hits.size() > limit) {
        result.overflow = filter.mode == QueryMode::all;
        result.hits.resize(limit);
    }
    result.has_block = std::ranges::any_of(result.hits, [](const QueryHit& hit) { return hit.blocking; });
}
}
Result<QueryResult> PhysicsScene::raycast(math::Vector3 start, math::Vector3 end,
    QueryFilter filter) const {
    if (!math::is_finite(start) || !math::is_finite(end))
        return failure(ErrorCode::argument_mismatch, "Ray endpoints must be finite.");
    if (auto valid = validate_query(filter); !valid) return std::unexpected(valid.error());
    QueryResult result;
    result.step_id = completed_step_;
    const JPH::RRayCast ray(position(start), vector(math::Vector3{end.x - start.x, end.y - start.y, end.z - start.z}));
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, collector);
    for (const auto& hit : collector.mHits) {
        if (!impl_->query_accept(hit.mBodyID, filter)) continue;
        const auto point = ray.GetPointOnRay(hit.mFraction);
        math::Vector3 normal{};
        if (hit.mFraction > 0) {
            JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), hit.mBodyID);
            if (lock.Succeeded()) {
                const auto value = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
                normal = {value.GetX(), value.GetY(), value.GetZ()};
            }
        }
        result.hits.push_back(impl_->query_hit(hit.mBodyID, hit.mSubShapeID2.GetValue(),
            hit.mFraction, vector(point), normal, hit.mFraction == 0, scene_id_, filter));
    }
    finish_query(result, filter);
    return result;
}
Result<QueryResult> PhysicsScene::sweep_sphere(math::Vector3 start, math::Vector3 end,
    float radius, QueryFilter filter) const {
    if (!math::is_finite(start) || !math::is_finite(end) || !std::isfinite(radius) || radius <= 0)
        return failure(ErrorCode::argument_mismatch, "Sphere sweep requires finite endpoints and positive radius.");
    if (auto valid = validate_query(filter); !valid) return std::unexpected(valid.error());
    QueryResult result;
    result.step_id = completed_step_;
    const JPH::ShapeRefC shape = new JPH::SphereShape(radius);
    const JPH::RShapeCast cast(shape.GetPtr(), JPH::Vec3::sReplicate(1.f),
        JPH::RMat44::sTranslation(position(start)),
        vector(math::Vector3{end.x - start.x, end.y - start.y, end.z - start.z}));
    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings{},
        JPH::RVec3::sZero(), collector);
    for (const auto& hit : collector.mHits) {
        if (!impl_->query_accept(hit.mBodyID2, filter)) continue;
        auto normal = hit.mPenetrationAxis;
        if (normal.LengthSq() > 0) normal = -normal.Normalized();
        result.hits.push_back(impl_->query_hit(hit.mBodyID2, hit.mSubShapeID2.GetValue(),
            hit.mFraction, {hit.mContactPointOn2.GetX(), hit.mContactPointOn2.GetY(), hit.mContactPointOn2.GetZ()},
            {normal.GetX(), normal.GetY(), normal.GetZ()}, hit.mFraction == 0, scene_id_, filter));
    }
    finish_query(result, filter);
    return result;
}
Result<QueryResult> PhysicsScene::overlap_sphere(math::Vector3 center, float radius,
    QueryFilter filter) const {
    if (!math::is_finite(center) || !std::isfinite(radius) || radius <= 0)
        return failure(ErrorCode::argument_mismatch, "Sphere overlap requires finite center and positive radius.");
    if (auto valid = validate_query(filter); !valid) return std::unexpected(valid.error());
    QueryResult result;
    result.step_id = completed_step_;
    const JPH::ShapeRefC shape = new JPH::SphereShape(radius);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CollideShape(shape.GetPtr(), JPH::Vec3::sReplicate(1.f),
        JPH::RMat44::sTranslation(position(center)), JPH::CollideShapeSettings{},
        JPH::RVec3::sZero(), collector);
    for (const auto& hit : collector.mHits) {
        if (!impl_->query_accept(hit.mBodyID2, filter)) continue;
        auto normal = hit.mPenetrationAxis;
        if (normal.LengthSq() > 0) normal = -normal.Normalized();
        result.hits.push_back(impl_->query_hit(hit.mBodyID2, hit.mSubShapeID2.GetValue(),
            0, {hit.mContactPointOn2.GetX(), hit.mContactPointOn2.GetY(), hit.mContactPointOn2.GetZ()},
            {normal.GetX(), normal.GetY(), normal.GetZ()}, true, scene_id_, filter));
    }
    finish_query(result, filter);
    return result;
}
std::size_t PhysicsScene::body_count() const noexcept { return impl_->bodies.size(); }

}
