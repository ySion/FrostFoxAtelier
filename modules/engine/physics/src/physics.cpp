#include "physics/physics.hpp"
#include "detail/components.hpp"
#include "foundation/map.hpp"
#include "foundation/scope_exit.hpp"
#include "foundation/set.hpp"
#include "physics/timing.hpp"
#include "runtime/native.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
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
#include <Jolt/Physics/Collision/CastResult.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace atelier::physics {
namespace {
using JPH::uint64;

struct BodyIdHash {
    using is_avalanching = void;

    std::size_t operator()(JPH::BodyID id) const noexcept {
        return Hash<std::uint32_t>{}(id.GetIndexAndSequenceNumber());
    }
};

struct BodyIdEqual {
    bool operator()(JPH::BodyID left, JPH::BodyID right) const noexcept {
        return left.GetIndexAndSequenceNumber() == right.GetIndexAndSequenceNumber();
    }
};

std::mutex registration_mutex;

std::size_t scene_count{};

// One solver pool serves every scene in the process and lives while any scene does.
// The host thread of a step also runs jobs; each concurrent step uses one barrier.
JPH::JobSystemThreadPool* solver_jobs{};

std::atomic<std::uint64_t> next_scene_id{1};

// Derived Capacity buffers per body slot, the ratios of Jolt's Samples (10240 bodies,
// 65536 body pairs, 20480 contact constraints).
constexpr std::size_t body_pairs_per_body = 8;

constexpr std::size_t contacts_per_body = 2;

constexpr float fixed_step = 1.f / 60.f;

std::unexpected<Error> failure(ErrorCode code, Utf8Text message) {
    return std::unexpected(Error{code, std::move(message)});
}
}

namespace detail {
bool contain(world::World& world, const Error& error, ObjectId object, ObjectId entity) {
    world::Diagnostic diagnostic;
    diagnostic.kind = world::DiagnosticKind::invalid_value;
    diagnostic.phase = world::DiagnosticPhase::physics;
    diagnostic.object = object;
    diagnostic.entity = entity;
    return world.contain(error, std::move(diagnostic));
}
}

namespace {
Result<void> validate_active_entity(world::World& world, ObjectId entity) {
    ATELIER_TRY_BIND(state, world::inspect_entity(world.runtime(), entity));
    if (state->state != world::EntityState::active || !world.contains(entity))
        return failure(ErrorCode::invalid_member, "Physics Entity must be active in this World.");
    return {};
}

Result<void> acquire_jolt() {
    std::lock_guard lock(registration_mutex);
    if (scene_count == 0) {
        if (JPH::Factory::sInstance)
            return failure(ErrorCode::busy, "Jolt has another process owner.");
        JPH::RegisterDefaultAllocator();
#ifdef JPH_ENABLE_ASSERTS
        // Jolt PhysicsSystem.cpp:679 asserts on every Update error; recheck this match when Jolt is upgraded.
        // step() reports those errors as warnings; every other assert still breaks.
        JPH::AssertFailed = [](const char* expression, const char*, const char* file, JPH::uint) {
            return !(StringView{expression} == "errors == EPhysicsUpdateError::None" &&
                     StringView{file}.ends_with("PhysicsSystem.cpp"));
        };
#endif
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        const auto cores = std::thread::hardware_concurrency();
        // On 500 active boxes 3 workers solved faster than 1, 7 or 15.
        solver_jobs = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                   int(std::clamp(cores > 1 ? cores - 1 : 1u, 1u, 3u)));
    }
    ++scene_count;
    return {};
}

void release_jolt() noexcept {
    std::lock_guard lock(registration_mutex);
    if (--scene_count == 0) {
        delete solver_jobs;
        solver_jobs = nullptr;
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

JPH::RVec3 position(math::Vector3 value) {
    return {value.x, value.y, value.z};
}

JPH::Vec3 vector(math::Vector3 value) {
    return {value.x, value.y, value.z};
}

JPH::Quat rotation(math::Quaternion value) {
    return {value.x, value.y, value.z, value.w};
}

math::Vector3 vector(JPH::RVec3Arg value) {
    return {value.GetX(), value.GetY(), value.GetZ()};
}

math::Quaternion rotation(JPH::QuatArg value) {
    return {value.GetX(), value.GetY(), value.GetZ(), value.GetW()};
}

JPH::EMotionType motion(MotionKind value) {
    switch (value) {
    case MotionKind::stationary:
        return JPH::EMotionType::Static;
    case MotionKind::kinematic:
        return JPH::EMotionType::Kinematic;
    case MotionKind::dynamic:
        return JPH::EMotionType::Dynamic;
    }
    std::unreachable();
}

constexpr std::uint64_t hash_basis = 14695981039346656037ull;

std::uint64_t fnv1a_bytes(Span<const std::byte> bytes, std::uint64_t hash = hash_basis) {
    for (const std::byte byte : bytes)
        hash = (hash ^ std::to_integer<std::uint8_t>(byte)) * 1099511628211ull;
    return hash;
}

void write_number(Array<std::byte>& out, std::uint64_t value, unsigned count) {
    for (unsigned index = 0; index < count; ++index)
        out.add(static_cast<std::byte>(value >> (index * 8)));
}

bool read_number(Span<const std::byte> bytes, std::size_t& cursor, unsigned count, std::uint64_t& value) {
    if (count > bytes.size() - cursor)
        return false;
    value = 0;
    for (unsigned index = 0; index < count; ++index)
        value |= std::uint64_t(std::to_integer<std::uint8_t>(bytes[cursor++])) << (index * 8);
    return true;
}

class ByteSpanStreamIn final : public JPH::StreamIn {
public:
    explicit ByteSpanStreamIn(Span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    void ReadBytes(void* out_data, std::size_t num_bytes) override {
        if (num_bytes == 0)
            return;
        if (eof_ || num_bytes > bytes_.size() - cursor_) {
            eof_ = true;
            return;
        }
        std::memcpy(out_data, bytes_.data() + cursor_, num_bytes);
        cursor_ += num_bytes;
    }

    bool IsEOF() const override { return eof_; }

    bool IsFailed() const override { return false; }

    bool Exhausted() const noexcept { return !eof_ && cursor_ == bytes_.size(); }

private:
    Span<const std::byte> bytes_;

    std::size_t cursor_{};

    bool eof_{};
};

class ByteArrayStreamOut final : public JPH::StreamOut {
public:
    void WriteBytes(const void* in_data, std::size_t num_bytes) override {
        if (num_bytes == 0)
            return;
        const auto* begin = static_cast<const std::byte*>(in_data);
        bytes_.append(Span<const std::byte>{begin, num_bytes});
    }

    bool IsFailed() const override { return false; }

    Span<const std::byte> view() const noexcept { return bytes_.view(); }

private:
    Array<std::byte> bytes_;
};

std::uint64_t mesh_hash(const geometry::Mesh& mesh, math::Vector3 scale) {
    std::uint64_t hash = hash_basis;
    const auto add = [&](float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned index = 0; index < 4; ++index)
            hash = (hash ^ ((bits >> (index * 8)) & 0xffu)) * 1099511628211ull;
    };
    add(scale.x);
    add(scale.y);
    add(scale.z);
    for (const auto& vertex : mesh.vertices()) {
        for (float value : vertex.position)
            add(value);
        for (float value : vertex.normal)
            add(value);
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
        const auto a = point(0);
        const auto b = point(1);
        const auto c = point(2);
        // The inverse-transpose keeps the authored front under a mirrored scale.
        const auto& normal = vertices[index].normal;
        if ((b - a).Cross(c - a).Dot(JPH::Vec3(normal[0] / scale.x, normal[1] / scale.y, normal[2] / scale.z)) >= 0)
            triangles.emplace_back(a, b, c);
        else
            triangles.emplace_back(a, c, b);
    }
    auto built = JPH::MeshShapeSettings(triangles).Create();
    if (built.HasError())
        return failure(ErrorCode::argument_mismatch, "Mesh cannot form a Jolt collision shape.");
    return JPH::ShapeRefC(built.Get());
}

Result<JPH::ShapeRefC> restore_shape(Span<const std::byte> bytes, const geometry::Mesh& mesh, math::Vector3 scale,
                                     StringView source) {
    constexpr StringView magic = "ATJSHAPE";
    if (!bytes.starts_with(as_bytes(magic)))
        return failure(ErrorCode::invalid_storage, "Collision asset magic is invalid.");
    std::size_t cursor = magic.size();
    std::uint64_t version{};
    std::uint64_t jolt{};
    std::uint64_t source_size{};
    std::uint64_t source_hash{};
    std::uint64_t geometry_hash{};
    std::uint64_t payload_size{};
    std::uint64_t payload_hash{};
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
    const auto stored_source = bytes.slice(cursor, static_cast<std::size_t>(source_size));
    cursor += static_cast<std::size_t>(source_size);
    const auto payload = bytes.slice(cursor);
    if (fnv1a_bytes(stored_source) != source_hash || fnv1a_bytes(payload) != payload_hash)
        return failure(ErrorCode::invalid_storage, "Collision asset content digest is invalid.");
    if (stored_source != as_bytes(source) || geometry_hash != mesh_hash(mesh, scale))
        return failure(ErrorCode::rebuild_required, "Collision asset source or scale changed; recook required.");
    ByteSpanStreamIn stream(payload);
    JPH::Shape::IDToShapeMap shapes;
    JPH::Shape::IDToMaterialMap materials;
    auto restored = JPH::Shape::sRestoreWithChildren(stream, shapes, materials);
    if (restored.HasError() || !stream.Exhausted())
        return failure(ErrorCode::invalid_storage, "Collision asset Jolt payload is invalid.");
    return JPH::ShapeRefC(restored.Get());
}
}

Result<Array<std::byte>> cook_triangle_mesh(const geometry::MeshRef& mesh, math::Vector3 scale,
                                            StringView source_key) {
    if (!mesh || source_key.empty() || source_key.size() > 4096 || !math::is_finite(scale) || scale.x == 0 ||
        scale.y == 0 || scale.z == 0)
        return failure(ErrorCode::argument_mismatch, "Cook needs a mesh, stable source key and nonzero scale.");
    ATELIER_TRY(acquire_jolt());
    ScopeExit registration{[]() noexcept { release_jolt(); }};
    ATELIER_TRY_BIND(shape, mesh_shape(*mesh, scale));
    ByteArrayStreamOut stream;
    JPH::Shape::ShapeToIDMap shapes;
    JPH::Shape::MaterialToIDMap materials;
    (*shape)->SaveWithChildren(stream, shapes, materials);
    const auto payload = stream.view();
    if (payload.size() > 128ull * 1024 * 1024)
        return failure(ErrorCode::capacity_exceeded, "Cooked collision shape is too large.");
    constexpr StringView magic = "ATJSHAPE";

    const auto source_bytes = as_bytes(source_key);

    Array<std::byte> bytes;
    bytes.reserve(magic.size() + 48 + source_bytes.size() + payload.size());
    bytes.append(as_bytes(magic));
    write_number(bytes, 1, 4);
    write_number(bytes, JPH_VERSION_ID, 8);
    write_number(bytes, source_bytes.size(), 4);
    write_number(bytes, fnv1a_bytes(source_bytes), 8);
    write_number(bytes, mesh_hash(*mesh, scale), 8);
    write_number(bytes, payload.size(), 8);
    write_number(bytes, fnv1a_bytes(payload), 8);
    bytes.append(source_bytes);
    bytes.append(payload);
    return bytes;
}

Result<void> validate_cooked_triangle_mesh(Span<const std::byte> bytes, const geometry::MeshRef& mesh,
                                           math::Vector3 scale, StringView source_key) {
    if (!mesh || source_key.empty() || !math::is_finite(scale) || scale.x == 0 || scale.y == 0 || scale.z == 0)
        return failure(ErrorCode::argument_mismatch, "Collision validation needs a mesh, source and nonzero scale.");
    ATELIER_TRY(acquire_jolt());
    ScopeExit registration{[]() noexcept { release_jolt(); }};
    ATELIER_TRY_BIND(shape, restore_shape(bytes, *mesh, scale, source_key));
    return {};
}

Result<void> configure(Runtime& runtime, ObjectId component, BodySettings settings) {
    ATELIER_TRY(world::validate_body_settings(runtime, settings));
    ATELIER_TRY_BIND(state, world::inspect_component(runtime, component));
    if (state->active)
        return failure(ErrorCode::busy, "Stop the World before changing a physical body.");
    return runtime.with_native<world::PrimitiveComponent>(component,
                                                          [&](world::PrimitiveComponent& body) -> Result<void> {
                                                              body.body = settings;
                                                              return {};
                                                          });
}

Result<BodySettings> read_settings(Runtime& runtime, ObjectId component) {
    return runtime.with_native<const world::PrimitiveComponent>(
        component, [](const world::PrimitiveComponent& body) -> Result<BodySettings> { return body.body; });
}

struct PhysicsScene::Impl {
    static constexpr JPH::ObjectLayer stationary_layer = 0;

    static constexpr JPH::ObjectLayer moving_layer = 1;

    struct RawContact {
        ContactPhase phase;

        JPH::BodyID first;

        JPH::BodyID second;
    };

    struct Contacts final : JPH::ContactListener {
        Impl* owner{};

        std::mutex mutex;

        Array<RawContact> pending;

        std::uint32_t dropped{};

        JPH::ValidateResult OnContactValidate(const JPH::Body& first, const JPH::Body& second, JPH::RVec3Arg,
                                              const JPH::CollideShapeResult&) override {
            return owner->response(first.GetID(), second.GetID()) == Response::ignore
                       ? JPH::ValidateResult::RejectAllContactsForThisBodyPair
                       : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }

        void push(ContactPhase phase, JPH::BodyID first, JPH::BodyID second) {
            std::lock_guard lock(mutex);
            if (pending.size() < 16384)
                pending.add({phase, first, second});
            else
                ++dropped;
        }

        void OnContactAdded(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold&,
                            JPH::ContactSettings& settings) override {
            settings.mIsSensor = owner->response(first.GetID(), second.GetID()) != Response::block;
            push(ContactPhase::began, first.GetID(), second.GetID());
        }

        void OnContactPersisted(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold&,
                                JPH::ContactSettings& settings) override {
            settings.mIsSensor = owner->response(first.GetID(), second.GetID()) != Response::block;
            push(ContactPhase::persisted, first.GetID(), second.GetID());
        }

        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            push(ContactPhase::ended, pair.GetBody1ID(), pair.GetBody2ID());
        }
    } contacts;

    JPH::BroadPhaseLayerInterfaceTable broad_phase{2, 2};

    JPH::ObjectLayerPairFilterTable pairs{2};

    Optional<JPH::ObjectVsBroadPhaseLayerFilterTable> broad_phase_filter;

    // Bodies Jolt puts to sleep during a step; solver threads report them.
    struct Activation final : JPH::BodyActivationListener {
        std::mutex mutex;

        Array<JPH::BodyID> deactivated;

        void OnBodyActivated(const JPH::BodyID&, JPH::uint64) override {}

        void OnBodyDeactivated(const JPH::BodyID& id, JPH::uint64) override {
            std::lock_guard lock(mutex);
            deactivated.add(id);
        }
    } activation;

    JPH::TempAllocatorImplWithMallocFallback temporary{4 * 1024 * 1024};

    JPH::PhysicsSystem system;

    struct Body {
        JPH::BodyID id;

        ObjectId entity;

        ObjectId component;

        MotionKind motion;

        BodySettings settings;

        geometry::MeshRef mesh;

        std::uint64_t revision;

        math::Vector3 shape_scale;

        world::Trs target;

        // The owner's local TRS as this scene last created, teleported or published it;
        // any other value was written past physics and relocates the body.
        world::Trs local;

        Array<ObjectId> ancestors;

        // The owner's world pose as this scene last created, teleported or published it; a
        // different one with the same local TRS means an ancestor moved, which relocates the body.
        world::Trs held;
    };

    // Whether two world poses agree within the float error of a TRS round trip.
    static bool near(const world::Trs& a, const world::Trs& b) noexcept {
        for (std::size_t i = 0; i < 3; ++i)
            if (std::abs(a.position[i] - b.position[i]) > 1e-5f * std::max(1.f, std::abs(b.position[i])))
                return false;
        const auto& [ax, ay, az, aw] = a.rotation;
        const auto& [bx, by, bz, bw] = b.rotation;
        const float sign = ax * bx + ay * by + az * bz + aw * bw < 0 ? -1.f : 1.f;
        return std::abs(ax - sign * bx) <= 1e-5f && std::abs(ay - sign * by) <= 1e-5f &&
               std::abs(az - sign * bz) <= 1e-5f && std::abs(aw - sign * bw) <= 1e-5f;
    }

    enum class CommandKind { force, impulse, kinematic_target, teleport };

    struct Command {
        CommandKind kind;

        BodyHandle handle;

        math::Vector3 value;

        math::Quaternion rotation{0, 0, 0, 1};

        bool preserve_velocity{};
    };

    Array<Command> commands;

    Array<world::SimulationPose> poses;

    // The body each pose of `poses` publishes, null for controllers and bound parts.
    Array<Body*> posed;

    Array<std::pair<ObjectId, world::Trs>> written;

    struct Pose {
        math::Vector3 position;

        math::Quaternion rotation;
    };

    // Where each simulated Entity was last published, created or teleported; its next
    // published step is drawn moving from there.
    Map<ObjectId, Pose> history;

    // The poses the latest step published and the World accepted, with their history.
    struct Interpolated {
        ObjectId entity;

        Pose from;

        Pose to;
    };

    Array<Interpolated> interpolated;

    // Bodies whose pose this step publishes: moved by Jolt or teleported.
    JPH::BodyIDVector moved;

    JPH::BodyIDVector active;

    // A spatial ancestor as one reconcile pass reads it; each part is read on first use.
    struct Ancestor {
        Optional<bool> member;

        Optional<bool> active;

        Optional<bool> moving_body;

        Optional<world::Trs> local;

        Optional<ObjectId> parent;
    };

    // Shared by every body checked in the current step's reconcile pass, then discarded:
    // a later write can change an ancestor.
    Map<ObjectId, Ancestor> ancestry;

    bool ancestry_pass{};

    // The body components at or below each Entity, a body's own Entity included; rebuilt when stale.
    Map<ObjectId, Array<ObjectId>> dependents;

    bool dependents_stale{true};

    // Bucket component -> owner Entity as of the latest full reconcile.
    Map<ObjectId, ObjectId> component_owners;

    // Entities a failure or a degenerate space left without a body; every step tries them again.
    Array<ObjectId> retry;

    // Written objects to reconcile: since the cursor, and those step handlers wrote.
    Array<ObjectId> changed;

    Array<ObjectId> pending;

    Array<ObjectId> handler_writes;

    Map<ObjectId, Array<ObjectId>>& below() {
        if (dependents_stale) {
            dependents.clear();
            for (const auto component : body_order.get(bodies))
                for (const auto node : bodies.at(component).ancestors)
                    dependents.try_emplace(node).value->add(component);
            dependents_stale = false;
        }
        return dependents;
    }

    Map<ObjectId, Body> bodies;

    detail::InsertionOrder body_order;

    Map<ObjectId, ObjectId> entity_bodies;

    Body* resolve(BodyHandle handle, std::uint64_t scene) {
        auto* found = bodies.find(handle.component);
        return found && handle.scene == scene && found->entity == handle.entity && found->revision == handle.revision
                   ? found
                   : nullptr;
    }

    Map<JPH::BodyID, ObjectId, BodyIdHash, BodyIdEqual> body_components;

    struct Distance {
        BodyHandle first;

        BodyHandle second;

        JPH::Ref<JPH::TwoBodyConstraint> constraint;
    };

    Map<std::uint64_t, Distance> distances;

    struct Character {
        std::uint64_t revision;

        CharacterSettings settings;

        JPH::Ref<JPH::CharacterVirtual> character;

        math::Vector3 input{};

        bool jump{};
    };

    Map<ObjectId, Character> characters;

    detail::InsertionOrder character_order;

    struct Ragdoll {
        std::uint64_t revision;

        JPH::Ref<JPH::Ragdoll> ragdoll;
    };

    Map<ObjectId, Ragdoll> ragdolls;

    detail::InsertionOrder ragdoll_order;

    Set<JPH::BodyID, BodyIdHash, BodyIdEqual> ragdoll_ids;

    struct PairState {
        std::uint32_t contacts{};

        ContactEvent event;
    };

    // Sorted pair keys give multithreaded Jolt contact callbacks deterministic delivery order.
    std::map<std::uint64_t, PairState> active_pairs;

    Array<ContactEvent> ready_events;

    static std::uint64_t pair_key(JPH::BodyID first, JPH::BodyID second) {
        const auto a = first.GetIndexAndSequenceNumber();
        const auto b = second.GetIndexAndSequenceNumber();
        return (std::uint64_t{std::min(a, b)} << 32) | std::max(a, b);
    }

    Optional<ContactEvent> make_event(JPH::BodyID first, JPH::BodyID second, std::uint64_t step,
                                      std::uint64_t scene) const {
        const auto* a = body_components.find(first);
        const auto* b = body_components.find(second);
        if (!a || !b)
            return {};
        const auto* x = bodies.find(*a);
        const auto* y = bodies.find(*b);
        if (!x || !y)
            return {};
        if (first.GetIndexAndSequenceNumber() > second.GetIndexAndSequenceNumber())
            std::swap(x, y);
        return ContactEvent{ContactPhase::began,
                            {x->entity, x->component, scene, x->revision},
                            {y->entity, y->component, scene, y->revision},
                            step,
                            response(first, second) != Response::block};
    }

    void collect_contacts(std::uint64_t step, std::uint64_t scene) {
        Array<RawContact> raw;
        {
            std::lock_guard lock(contacts.mutex);
            raw.swap(contacts.pending);
        }
        Set<std::uint64_t> previous;
        previous.reserve(active_pairs.size());
        for (const auto& [key, state] : active_pairs) {
            (void)state;
            previous.add(key);
        }
        for (const auto& notice : raw) {
            const auto key = pair_key(notice.first, notice.second);
            if (notice.phase == ContactPhase::began) {
                auto event = make_event(notice.first, notice.second, step, scene);
                if (!event)
                    continue;
                auto& state = active_pairs[key];
                if (state.contacts == 0)
                    state.event = *event;
                ++state.contacts;
            } else if (notice.phase == ContactPhase::ended) {
                auto found = active_pairs.find(key);
                if (found != active_pairs.end() && found->second.contacts != 0)
                    --found->second.contacts;
            }
        }
        for (auto it = active_pairs.begin(); it != active_pairs.end();) {
            auto& state = it->second;
            // When the contact buffer is full Jolt reports a cached manifold removed but keeps it,
            // and then only persists it, so a count that reaches zero is checked against Jolt.
            if (state.contacts == 0 && system.WereBodiesInContact(JPH::BodyID(std::uint32_t(it->first >> 32)),
                                                                  JPH::BodyID(std::uint32_t(it->first))))
                state.contacts = 1;
            if (state.contacts == 0) {
                if (previous.contains(it->first)) {
                    auto event = state.event;
                    event.phase = ContactPhase::ended;
                    event.step_id = step;
                    ready_events.add(event);
                }
                it = active_pairs.erase(it);
                continue;
            }
            auto event = state.event;
            event.phase = previous.contains(it->first) ? ContactPhase::persisted : ContactPhase::began;
            event.step_id = step;
            ready_events.add(event);
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
                ready_events.add(event);
                it = active_pairs.erase(it);
            } else
                ++it;
        }
    }

    Response response(JPH::BodyID first, JPH::BodyID second) const {
        const auto* a = body_components.find(first);
        const auto* b = body_components.find(second);
        if (!a || !b) {
            const bool left_ragdoll = ragdoll_ids.contains(first);
            const bool right_ragdoll = ragdoll_ids.contains(second);
            if ((left_ragdoll && (right_ragdoll || b != nullptr)) || (right_ragdoll && a != nullptr))
                return Response::block;
            return Response::ignore;
        }
        const auto* left = bodies.find(*a);
        const auto* right = bodies.find(*b);
        if (!left || !right)
            return Response::ignore;
        const auto& x = left->settings;
        const auto& y = right->settings;
        const auto xy = x.response(y.category);
        const auto yx = y.response(x.category);
        if (xy == Response::ignore || yx == Response::ignore)
            return Response::ignore;
        if (x.sensor || y.sensor || xy == Response::overlap || yx == Response::overlap)
            return Response::overlap;
        return Response::block;
    }

    Response query_response(const BodySettings& settings, const QueryFilter& filter) const {
        Response response = Response::block;
        if (filter.policy != QueryPolicy::object_type)
            response = settings.response(filter.category);
        if (filter.policy == QueryPolicy::interaction) {
            const auto other = filter.responses[settings.category];
            if (other == Response::ignore)
                response = Response::ignore;
            else if (other == Response::overlap && response == Response::block)
                response = Response::overlap;
        }
        if (settings.sensor && response == Response::block)
            response = Response::overlap;
        return response;
    }

    bool query_accept(JPH::BodyID id, const QueryFilter& filter) const {
        const auto* owner = body_components.find(id);
        if (!owner)
            return false;
        const auto* body = bodies.find(*owner);
        if (!body || body->entity == filter.ignore_entity ||
            std::ranges::find(filter.ignored_entities, body->entity) != filter.ignored_entities.end())
            return false;
        const auto response = query_response(body->settings, filter);
        return body->component != filter.ignore_component &&
               std::ranges::find(filter.ignored_components, body->component) == filter.ignored_components.end() &&
               (filter.include_sensors || !body->settings.sensor) &&
               ((filter.categories >> body->settings.category) & 1u) != 0 && response != Response::ignore &&
               (!filter.ignore_touches || response == Response::block);
    }

    QueryHit query_hit(JPH::BodyID id, std::uint32_t sub_shape, float fraction, math::Vector3 point,
                       math::Vector3 normal, bool initial, std::uint64_t scene, const QueryFilter& filter) const {
        const auto& body = bodies.at(body_components.at(id));
        const bool blocking = query_response(body.settings, filter) == Response::block;
        return {{body.entity, body.component, scene, body.revision},
                fraction,
                point,
                normal,
                sub_shape,
                body.settings.sensor,
                initial,
                blocking,
                body.settings.material};
    }

    explicit Impl(Capacity capacity) {
        contacts.owner = this;
        broad_phase.MapObjectToBroadPhaseLayer(stationary_layer, JPH::BroadPhaseLayer(0));
        broad_phase.MapObjectToBroadPhaseLayer(moving_layer, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(stationary_layer, moving_layer);
        pairs.EnableCollision(moving_layer, moving_layer);
        broad_phase_filter.emplace(broad_phase, 2, pairs, 2);
        system.Init(capacity.bodies, 0, capacity.body_pairs, capacity.contact_constraints, broad_phase,
                    *broad_phase_filter, pairs);
        system.SetContactListener(&contacts);
        system.SetBodyActivationListener(&activation);
        system.SetGravity(JPH::Vec3(0, -9.81f, 0));
    }
};

PhysicsScene::PhysicsScene(world::World& world, Types types, MeshReader mesh_reader, Capacity capacity)
    : world_(world), types_(types), mesh_reader_(mesh_reader), impl_(std::make_unique<Impl>(capacity)),
      components_(std::make_unique<detail::ComponentSimulation>(world, *this)), scene_id_(next_scene_id++) {}

Result<std::unique_ptr<PhysicsScene>> PhysicsScene::create(world::World& world, Types types, MeshReader mesh_reader,
                                                           Capacity capacity) {
    if (world.closed())
        return failure(ErrorCode::unsupported_operation, "Cannot attach physics to a closed World.");
    ATELIER_TRY_BIND(actual, world.runtime().native_type<CharacterComponent>());
    if (*actual != types.character_component)
        return failure(ErrorCode::type_mismatch, "Physics types belong to another Runtime.");
    if (capacity.bodies > JPH::PhysicsSystem::cMaxBodiesLimit ||
        capacity.body_pairs > JPH::PhysicsSystem::cMaxBodyPairsLimit ||
        capacity.contact_constraints > JPH::PhysicsSystem::cMaxContactConstraintsLimit)
        return failure(ErrorCode::argument_mismatch, "Physics capacity exceeds Jolt's limits.");
    {
        ATELIER_TRY_BIND(registered, world.register_capability(world.types().primitive_component));
        ATELIER_TRY_BIND(primitives, world.members(*registered));
        // Room for as many bodies again as the World holds now; Jolt cannot grow these later.
        const auto bodies = std::clamp<std::size_t>(std::bit_ceil(std::max<std::size_t>(2 * primitives->size(), 4096)),
                                                    4096, JPH::PhysicsSystem::cMaxBodiesLimit);
        if (!capacity.bodies)
            capacity.bodies = std::uint32_t(bodies);
        if (!capacity.body_pairs)
            capacity.body_pairs = std::uint32_t(
                std::min<std::size_t>(body_pairs_per_body * bodies, JPH::PhysicsSystem::cMaxBodyPairsLimit));
        if (!capacity.contact_constraints)
            capacity.contact_constraints = std::uint32_t(
                std::min<std::size_t>(contacts_per_body * bodies, JPH::PhysicsSystem::cMaxContactConstraintsLimit));
        // Jolt raises both to at least 4 so that its hash maps get 4 buckets.
        capacity.body_pairs = std::max(capacity.body_pairs, 4u);
        capacity.contact_constraints = std::max(capacity.contact_constraints, 4u);
    }
    ATELIER_TRY(acquire_jolt());
    std::unique_ptr<PhysicsScene> result{new PhysicsScene(world, types, mesh_reader, capacity)};
    auto subscribed =
        world.subscribe_lifecycle([scene = result.get()](world::World&, world::Lifecycle phase, ObjectId object) {
            return scene->lifecycle(phase, object);
        });
    if (!subscribed)
        return std::unexpected(subscribed.error());
    result->subscription_ = *subscribed;
    auto simulation = world.add_simulation(
        {[scene = result.get()](float elapsed) { return scene->step(elapsed); },
         [scene = result.get()](ObjectId entity) { return scene->writes_pose(entity); },
         [scene = result.get()](Span<const ObjectId> entities) { return scene->relocate(entities); }});
    if (!simulation)
        return std::unexpected(simulation.error());
    result->simulation_ = *simulation;
    for (auto [id, type] : {std::pair{&result->primitives_, world.types().primitive_component},
                            std::pair{&result->characters_, types.character_component},
                            std::pair{&result->ragdolls_, types.ragdoll_component}}) {
        ATELIER_TRY_BIND(registered, world.register_capability(type));
        *id = *registered;
    }
    ATELIER_TRY_BIND(members, world.members());
    for (const auto& member : *members) {
        ATELIER_TRY(result->reconcile(member.id()));
    }
    return result;
}

PhysicsScene::~PhysicsScene() {
    if (simulation_)
        world_.remove_simulation(simulation_);
    if (subscription_)
        world_.unsubscribe_lifecycle(subscription_);
    components_.reset();
    while (!impl_->characters.empty())
        remove_special(impl_->characters.begin()->key);
    while (!impl_->ragdolls.empty())
        remove_special(impl_->ragdolls.begin()->key);
    for (const auto [revision, distance] : impl_->distances) {
        (void)revision;
        impl_->system.RemoveConstraint(distance.constraint);
    }
    impl_->distances.clear();
    while (!impl_->bodies.empty())
        remove(impl_->bodies.begin()->key);
    impl_.reset();
    release_jolt();
}

void PhysicsScene::remove(ObjectId component) noexcept {
    const auto* found = impl_->bodies.find(component);
    if (!found)
        return;
    const auto entity = found->entity;
    const auto id = found->id;
    auto& bodies = impl_->system.GetBodyInterface();
    impl_->distances.remove_if([&](std::uint64_t, const Impl::Distance& distance) {
        if (distance.first.component != component && distance.second.component != component)
            return false;
        impl_->system.RemoveConstraint(distance.constraint);
        bodies.ActivateBody(impl_->bodies.at(distance.first.component).id);
        bodies.ActivateBody(impl_->bodies.at(distance.second.component).id);
        return true;
    });
    impl_->remove_pairs(id, completed_step_);
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
    impl_->body_components.remove(id);
    impl_->entity_bodies.remove(entity);
    impl_->bodies.remove(component);
    impl_->body_order.invalidate();
    unbuilt_ |= impl_->system.GetNumBodies() == 0;
    ++edits_;
    impl_->dependents_stale = true;
    forget_pose(entity);
}

void PhysicsScene::forget_pose(ObjectId entity) noexcept {
    // Every interpolated and presented Entity has a history entry.
    if (!impl_->history.remove(entity))
        return;
    impl_->interpolated.remove_if([&](const Impl::Interpolated& item) { return item.entity == entity; });
    presented_.remove_if([&](const world::PresentedPose& item) { return item.entity == entity; });
}

void PhysicsScene::hold_poses(Span<const world::SimulationPose> poses) {
    // One compaction for the whole batch; every interpolated and presented Entity has a history entry.
    Set<ObjectId> held;
    for (const auto& pose : poses)
        if (impl_->history.remove(pose.entity))
            held.add(pose.entity);
    if (!held.empty()) {
        impl_->interpolated.remove_if([&](const Impl::Interpolated& item) { return held.contains(item.entity); });
        presented_.remove_if([&](const world::PresentedPose& item) { return held.contains(item.entity); });
    }
    for (const auto& [entity, where, orientation] : poses) {
        const Impl::Pose pose{where, orientation};
        impl_->history.insert_or_assign(entity, pose);
        // An equal pair keeps the Entity's descendants from following a moving ancestor.
        impl_->interpolated.add({entity, pose, pose});
        if (!paused_)
            presented_.add({entity, where, orientation, where, orientation});
    }
}

void PhysicsScene::set_paused(bool paused) noexcept {
    paused_ = paused;
    if (paused)
        present();
}

void PhysicsScene::present() {
    presented_.clear();
    if (paused_) {
        // Resuming starts from the World pose shown while paused, not from an older one.
        for (auto& item : impl_->interpolated)
            item.from = item.to;
        return;
    }
    const float alpha = std::clamp(remaining_ / fixed_step, 0.f, 1.f);
    presented_.reserve(impl_->interpolated.size());
    for (const auto& [entity, from, to] : impl_->interpolated) {
        auto where = to.position;
        auto orientation = to.rotation;
        if (std::memcmp(&from, &to, sizeof from) != 0) {
            where = math::lerp(from.position, to.position, alpha);
            // Normalized lerp along the shorter arc.
            const auto& [ax, ay, az, aw] = from.rotation;
            const auto& [bx, by, bz, bw] = to.rotation;
            const float side = ax * bx + ay * by + az * bz + aw * bw < 0 ? -1.f : 1.f;
            const auto blend = [&](float a, float b) { return std::lerp(side * a, b, alpha); };
            if (const auto unit =
                    math::try_normalize(math::Quaternion{blend(ax, bx), blend(ay, by), blend(az, bz), blend(aw, bw)}))
                orientation = *unit;
        }
        presented_.add({entity, where, orientation, to.position, to.rotation});
    }
}

Result<void> PhysicsScene::follow_handler(std::uint64_t opened) {
    auto& runtime = world_.runtime();
    auto& written = impl_->handler_writes;
    runtime.changed_objects(opened, written);
    impl_->pending.append(written);
    Array<ObjectId> moved;
    if (world_.revision() == reconciled_) {
        auto& below = impl_->below();
        for (const auto object : written)
            if (const auto* found = below.find(object))
                moved.append(*found);
    } else {
        // A structural edit since this step's reconcile leaves Body::ancestors behind; walk the current chains.
        const Set<ObjectId> touched(written.begin(), written.end());
        for (const auto component : impl_->body_order.get(impl_->bodies)) {
            const auto& body = impl_->bodies.at(component);
            if (!world_.contains(body.entity))
                continue;
            for (auto node = body.entity; node != ObjectId{};) {
                if (touched.contains(node)) {
                    moved.add(component);
                    break;
                }
                auto parent = world::spatial_parent(runtime, node);
                if (!parent)
                    break;
                node = *parent;
            }
        }
    }
    // Sort only the affected subset; an idle handler must not scan every body.
    std::ranges::sort(
        moved, [&](ObjectId a, ObjectId b) { return impl_->bodies.at(a).revision < impl_->bodies.at(b).revision; });
    moved.deduplicate();
    for (const auto component : moved) {
        const auto& body = impl_->bodies.at(component);
        const auto entity = body.entity;
        auto relocated = [&]() -> Result<void> {
            ATELIER_TRY_BIND(local, world::read_transform(runtime, entity));
            ATELIER_TRY_BIND(matrix, world::world_matrix(runtime, entity));
            auto pose = world::relative_transform(*matrix, world::transform_matrix({}));
            // A degenerate space keeps its body until the next step's reconcile drops it.
            if (!pose)
                return {};
            if (std::memcmp(&*local, &body.local, sizeof body.local) == 0 && Impl::near(*pose, body.held))
                return {};
            return relocate(Span<const ObjectId>{&entity, 1});
        }();
        if (!relocated && !detail::contain(world_, relocated.error(), entity, entity))
            return relocated;
    }
    return {};
}

void PhysicsScene::remove_special(ObjectId entity) noexcept {
    if (impl_->characters.contains(entity) || impl_->ragdolls.contains(entity)) {
        forget_pose(entity);
        ++edits_;
    }
    if (impl_->characters.remove(entity))
        impl_->character_order.invalidate();
    if (auto removed = impl_->ragdolls.take(entity)) {
        for (const auto id : removed->ragdoll->GetBodyIDs())
            impl_->ragdoll_ids.remove(id);
        removed->ragdoll->RemoveFromPhysicsSystem();
        impl_->ragdoll_order.invalidate();
        unbuilt_ |= impl_->system.GetNumBodies() == 0;
    }
}

Result<void> PhysicsScene::reconcile(ObjectId entity) {
    auto& runtime = world_.runtime();
    ATELIER_TRY_BIND(status, world::inspect_entity(runtime, entity));
    ObjectId candidate;
    unsigned drivers = 0;
    if (status->state == world::EntityState::active) {
        ATELIER_TRY_BIND(group, world::components(runtime, entity));
        for (const auto& item : *group) {
            ATELIER_TRY_BIND(state, world::inspect_component(runtime, item.id()));
            if (!state->active)
                continue;
            auto character = runtime.is_a(state->type, types_.character_component);
            auto ragdoll = runtime.is_a(state->type, types_.ragdoll_component);
            if (!character)
                return std::unexpected(character.error());
            if (!ragdoll)
                return std::unexpected(ragdoll.error());
            drivers += *character || *ragdoll;
            ATELIER_TRY_BIND(primitive, runtime.is_a(state->type, world_.types().primitive_component));
            if (!*primitive)
                continue;
            ATELIER_TRY_BIND(settings, read_settings(runtime, item.id()));
            if (!settings->enabled) {
                remove(item.id());
                continue;
            }
            if (candidate != ObjectId{})
                return failure(
                    ErrorCode::invalid_member,
                    "An Entity may have only one active Primitive pose writer; use a child Entity for another body.");
            candidate = item.id();
        }
        if (drivers == 0 && (impl_->characters.contains(entity) || impl_->ragdolls.contains(entity)))
            drivers = 1;
        if (drivers + unsigned(candidate != ObjectId{}) > 1)
            return failure(ErrorCode::invalid_member, "An Entity may have only one active physics pose writer.");
    } else
        remove_special(entity);
    if (const auto* current = impl_->entity_bodies.find(entity); current && *current != candidate)
        remove(*current);
    return candidate == ObjectId{} ? Result<void>{} : reconcile_component(entity, candidate);
}

Result<void> PhysicsScene::reconcile_component(ObjectId entity, ObjectId component) {
    auto& runtime = world_.runtime();
    ATELIER_TRY_BIND(settings, read_settings(runtime, component));
    ATELIER_TRY(world::validate_body_settings(runtime, *settings));
    if (!settings->enabled) {
        remove(component);
        return {};
    }
    ATELIER_TRY_BIND(spatial, world::has_transform(runtime, entity));
    if (!*spatial)
        return failure(ErrorCode::invalid_member, "A Primitive requires a spatial owner Entity.");
    Array<ObjectId> ancestors;
    // A zero scale axis anywhere in the chain is reported once, on the topmost such node.
    ObjectId degenerate;
    Optional<Error> hierarchy;
    world::Trs owner_local;
    for (auto node = entity; node != ObjectId{};) {
        // Within a step's reconcile pass each node is read once; otherwise every call reads it.
        Impl::Ancestor scratch;
        auto& checked = impl_->ancestry_pass ? *impl_->ancestry.try_emplace(node).value : scratch;
        if (!checked.member)
            checked.member = world_.contains(node);
        if (!*checked.member)
            return failure(ErrorCode::invalid_member,
                           "Physics spatial origin belongs to another World or is detached.");
        ancestors.add(node);
        if (!checked.active) {
            ATELIER_TRY_BIND(status, world::inspect_entity(runtime, node));
            checked.active = status->state == world::EntityState::active;
        }
        if (node == entity && !*checked.active) {
            remove(component);
            return {};
        }
        if (!checked.local) {
            ATELIER_TRY_BIND(local, world::read_transform(runtime, node));
            checked.local = *local;
        }
        const auto& scale = checked.local->scale;
        if (node == entity)
            owner_local = *checked.local;
        if (scale.x == 0 || scale.y == 0 || scale.z == 0)
            degenerate = node;
        if (!hierarchy && node != entity && settings->motion != MotionKind::stationary &&
            (std::abs(std::abs(scale.x) - std::abs(scale.y)) > 1e-5f ||
             std::abs(std::abs(scale.x) - std::abs(scale.z)) > 1e-5f))
            hierarchy = Error{ErrorCode::argument_mismatch,
                              "Moving Primitive ancestors require uniform scale for lossless pose writeback."};
        if (!hierarchy && node != entity && settings->motion != MotionKind::dynamic) {
            if (!checked.moving_body) {
                ATELIER_TRY_BIND(group, world::components(runtime, node));
                bool moving = false;
                for (const auto& ancestor : *group) {
                    ATELIER_TRY_BIND(state, world::inspect_component(runtime, ancestor.id()));
                    ATELIER_TRY_BIND(primitive, runtime.is_a(state->type, world_.types().primitive_component));
                    if (!*primitive || !state->active)
                        continue;
                    ATELIER_TRY_BIND(body, read_settings(runtime, ancestor.id()));
                    moving |= body->enabled && body->motion != MotionKind::stationary;
                }
                checked.moving_body = moving;
            }
            if (*checked.moving_body)
                hierarchy = Error{
                    ErrorCode::unsupported_operation,
                    "Independent static or kinematic bodies cannot follow a moving physical ancestor; use an independent spatial branch."};
        }
        if (!checked.parent) {
            ATELIER_TRY_BIND(parent, world::spatial_parent(runtime, node));
            checked.parent = *parent;
        }
        node = *checked.parent;
    }
    ATELIER_TRY_BIND(matrix, world::world_matrix(runtime, entity));
    if (degenerate != ObjectId{} || world::singular(*matrix)) {
        remove(component);
        world::Diagnostic diagnostic;
        diagnostic.severity = world::Severity::warning;
        diagnostic.kind = world::DiagnosticKind::degenerate_value;
        diagnostic.phase = world::DiagnosticPhase::physics;
        diagnostic.object = diagnostic.entity = degenerate != ObjectId{} ? degenerate : entity;
        diagnostic.field = {"scale"_s};
        diagnostic.message = "A zero scale axis makes this space degenerate; Primitives in it have no physical body."_s;
        world_.report(std::move(diagnostic));
        impl_->retry.add(entity);
        return {};
    }
    if (hierarchy)
        return std::unexpected(std::move(*hierarchy));
    ATELIER_TRY_BIND(canonical, world::relative_transform(*matrix, world::transform_matrix({})));
    // Box, sphere and capsule have no local offset and are symmetric about every
    // local axis, so a mirrored world scale only changes sign: their dimensions
    // use |scale| and the reflection stays in the decomposition. Offsets or
    // asymmetric shapes would need the reflection applied to the shape itself.
    const auto scale = canonical->scale;
    const auto size = math::cwise_abs(scale);
    if ((settings->shape == ShapeKind::sphere || settings->shape == ShapeKind::capsule) &&
        (std::abs(size.x - size.y) > 1e-5f || std::abs(size.x - size.z) > 1e-5f))
        return failure(ErrorCode::argument_mismatch, "Sphere and capsule collision require uniform world scale.");
    geometry::MeshRef mesh;
    String source;
    if (settings->shape == ShapeKind::triangle_mesh) {
        ATELIER_TRY_BIND(binding, mesh_reader_(runtime, component));
        mesh = std::move(binding->resolved);
        source = std::move(binding->source);
        if (!mesh)
            return failure(ErrorCode::invalid_member, "Triangle collision requires this Primitive's mesh resource.");
    }
    auto* current = impl_->bodies.find(component);
    const auto same_scale = [&](math::Vector3 a, math::Vector3 b) {
        for (std::size_t i = 0; i < 3; ++i)
            if (std::abs(a[i] - b[i]) > 1e-5f * std::max(1.f, std::abs(b[i])))
                return false;
        return true;
    };
    if (current && current->entity == entity && current->mesh == mesh && current->settings == *settings &&
        same_scale(current->shape_scale, scale)) {
        if (current->ancestors == ancestors) {
            // A TRS written past physics (Runtime, Source), on the owner or an ancestor, moves
            // the body as set_transform does.
            static_assert(sizeof(world::Trs) == 10 * sizeof(float));
            if (std::memcmp(&current->local, &owner_local, sizeof owner_local) == 0 &&
                Impl::near(*canonical, current->held))
                return {};
            current->local = owner_local;
            return relocate(Span<const ObjectId>{&entity, 1});
        }
        // A keep-world hierarchy edit changes dependency metadata, not the body or its constraints.
        JPH::RVec3 where;
        JPH::Quat orientation;
        impl_->system.GetBodyInterface().GetPositionAndRotation(current->id, where, orientation);
        const auto actual = vector(where);
        bool same_position = true;
        for (std::size_t i = 0; i < 3; ++i)
            same_position &=
                std::abs(actual[i] - canonical->position[i]) <= 1e-5f * std::max(1.f, std::abs(canonical->position[i]));
        const auto expected = rotation(canonical->rotation);
        const auto sign = orientation.Dot(expected) < 0 ? -1.f : 1.f;
        const bool same_rotation = std::abs(orientation.GetX() - sign * expected.GetX()) <= 1e-5f &&
                                   std::abs(orientation.GetY() - sign * expected.GetY()) <= 1e-5f &&
                                   std::abs(orientation.GetZ() - sign * expected.GetZ()) <= 1e-5f &&
                                   std::abs(orientation.GetW() - sign * expected.GetW()) <= 1e-5f;
        if (same_position && same_rotation) {
            current->ancestors = std::move(ancestors);
            current->local = owner_local;
            current->held = *canonical;
            impl_->dependents_stale = true;
            // Until a step publishes it, its listing keeps it off a moving new ancestor's interpolation.
            const world::SimulationPose held_pose{entity, canonical->position, canonical->rotation};
            hold_poses({&held_pose, 1});
            return {};
        }
    }
    const auto dimensions = math::cwise_mul(settings->half_extents, size);
    if (!math::is_finite(dimensions) || math::min_component(dimensions) <= 0 ||
        !std::isfinite(settings->radius * size.x) || settings->radius * size.x <= 0 ||
        !std::isfinite(settings->capsule_half_height * size.x) || settings->capsule_half_height * size.x <= 0)
        return failure(ErrorCode::argument_mismatch, "Scaled collision dimensions exceed finite range.");
    JPH::ShapeRefC shape;
    if (settings->shape == ShapeKind::box)
        shape = new JPH::BoxShape(vector(dimensions));
    else if (settings->shape == ShapeKind::sphere)
        shape = new JPH::SphereShape(settings->radius * size.x);
    else if (settings->shape == ShapeKind::capsule)
        shape = new JPH::CapsuleShape(settings->capsule_half_height * size.x, settings->radius * size.x);
    else {
        auto built = settings->cooked_shape ? restore_shape(*settings->cooked_shape, *mesh, scale, source)
                                            : mesh_shape(*mesh, scale);
        // A body rescaled at runtime outgrows its cooked shape; cook here rather than write the shared cache.
        if (!built && built.error().code == ErrorCode::rebuild_required) {
            built = mesh_shape(*mesh, scale);
            ++runtime_cooks_;
        }
        if (!built)
            return std::unexpected(built.error());
        shape = *built;
    }
    auto& bodies = impl_->system.GetBodyInterface();
    JPH::BodyCreationSettings creation(
        shape, position(canonical->position), rotation(canonical->rotation), motion(settings->motion),
        settings->motion == MotionKind::stationary ? Impl::stationary_layer : Impl::moving_layer);
    creation.mIsSensor = settings->sensor;
    creation.mCollideKinematicVsNonDynamic = settings->sensor && settings->motion == MotionKind::kinematic;
    creation.mFriction = settings->material.friction;
    creation.mRestitution = settings->material.restitution;
    const auto id =
        bodies.CreateAndAddBody(creation, settings->motion == MotionKind::stationary ? JPH::EActivation::DontActivate
                                                                                     : JPH::EActivation::Activate);
    if (id.IsInvalid())
        return failure(ErrorCode::capacity_exceeded, "Jolt could not create a physical body.");
    remove(component);
    remove_special(entity);
    impl_->bodies.try_emplace(component, Impl::Body{id, entity, component, settings->motion, *settings, std::move(mesh),
                                                    next_revision_++, scale, *canonical, owner_local,
                                                    std::move(ancestors), *canonical});
    impl_->body_order.invalidate();
    impl_->dependents_stale = true;
    impl_->entity_bodies.try_emplace(entity, component);
    impl_->body_components.try_emplace(id, component);
    const world::SimulationPose held_pose{entity, canonical->position, canonical->rotation};
    hold_poses({&held_pose, 1});
    return {};
}

Result<void> PhysicsScene::lifecycle(world::Lifecycle phase, ObjectId object) {
    ATELIER_TRY_BIND(type, world_.runtime().type_of(object));
    ATELIER_TRY_BIND(entity, world_.runtime().is_a(*type, world_.types().entity));
    if (phase == world::Lifecycle::deactivated || phase == world::Lifecycle::detached) {
        if (!world_.running()) {
            // A stopped World steps nothing; the next start begins from an empty accumulator
            // and reconciles everything.
            tracked_ = false;
            impl_->pending.clear();
            impl_->commands.clear();
            impl_->history.clear();
            impl_->interpolated.clear();
            presented_.clear();
            remaining_ = 0;
            single_step_ = false;
        }
        ATELIER_TRY(components_->removed(object, phase == world::Lifecycle::detached));
        if (*entity)
            remove_special(object);
        if (!*entity)
            remove(object);
        else {
            if (const auto* own = impl_->entity_bodies.find(object))
                remove(*own);
            // Deactivation follows composition ownership. A disabled independent
            // origin still supplies its authored frame to active owners.
            if (phase == world::Lifecycle::detached && world_.running())
                for (const auto component : impl_->body_order.get(impl_->bodies)) {
                    const auto& body = impl_->bodies.at(component);
                    if (std::ranges::find(body.ancestors, object) != body.ancestors.end())
                        remove(component);
                }
        }
        return {};
    }
    if (*entity)
        return reconcile(object);
    ATELIER_TRY_BIND(state, world::inspect_component(world_.runtime(), object));
    return state->owner == ObjectId{} ? Result<void>{} : reconcile(state->owner);
}

Result<void> PhysicsScene::step(float elapsed) {
    timing::Scope timed{phases::step};
    Optional<timing::Scope> reconciling{std::in_place, phases::reconcile};
    auto& runtime = world_.runtime();
    // Runtime write stamps name what changed since the previous step's write-back, and
    // pending holds what its step handlers wrote; only those Entities, the owners of those
    // components and the bodies below those Entities are reconciled. A structural edit
    // (World::revision) or a stopped World reconciles every body owner and controller.
    // Other members hold no body or pose driver, so reconcile is a no-op for them.
    const bool full = !tracked_ || reconciled_ != world_.revision();
    reconciled_ = world_.revision();
    auto& changed = impl_->changed;
    changed.clear();
    if (!full)
        runtime.changed_objects(cursor_, changed);
    changed.append(impl_->pending);
    impl_->pending.clear();
    Set<ObjectId> candidates(impl_->retry.begin(), impl_->retry.end());
    impl_->retry.clear();
    if (full) {
        for (const auto [entity, component] : impl_->entity_bodies) {
            (void)component;
            candidates.add(entity);
        }
        for (const auto [entity, character] : impl_->characters) {
            (void)character;
            candidates.add(entity);
        }
        for (const auto [entity, ragdoll] : impl_->ragdolls) {
            (void)ragdoll;
            candidates.add(entity);
        }
        impl_->component_owners.clear();
        // World keeps these buckets with composition and enablement.
        // Existing bodies and controllers stay candidates so that leaving removes them.
        const auto add_owners = [&](world::CapabilityId id, bool body) -> Result<void> {
            ATELIER_TRY_BIND(found, world_.members(id));
            for (const auto& item : *found) {
                impl_->component_owners.insert_or_assign(item.component, item.entity);
                if (!item.active)
                    continue;
                if (body) {
                    auto enabled = runtime.with_native<const world::PrimitiveComponent>(
                        item.component,
                        [](const world::PrimitiveComponent& value) -> Result<bool> { return value.body.enabled; });
                    if (!enabled)
                        return std::unexpected(enabled.error());
                    if (!*enabled)
                        continue;
                }
                candidates.add(item.entity);
            }
            return {};
        };
        ATELIER_TRY(add_owners(primitives_, true));
        ATELIER_TRY(add_owners(characters_, false));
        ATELIER_TRY(add_owners(ragdolls_, false));
    } else {
        auto& below = impl_->below();
        for (const auto object : changed) {
            // A written component stands for its owner; bodies below an owner depend on its settings too.
            const auto* owner = impl_->component_owners.find(object);
            const auto entity = owner ? *owner : object;
            if (owner || impl_->entity_bodies.contains(entity) || impl_->characters.contains(entity) ||
                impl_->ragdolls.contains(entity))
                candidates.add(entity);
            if (const auto* found = below.find(entity))
                for (const auto component : *found)
                    candidates.add(impl_->bodies.at(component).entity);
        }
    }
    if (!candidates.empty()) {
        ATELIER_TRY_BIND(members, world_.members());
        impl_->ancestry.clear();
        impl_->ancestry_pass = true;
        ScopeExit pass_end{[this]() noexcept { impl_->ancestry_pass = false; }};
        for (const auto& member : *members) {
            if (!candidates.contains(member.id()))
                continue;
            auto found = reconcile(member.id());
            if (found)
                continue;
            // An isolated Entity keeps no ordinary body until its configuration is valid again.
            if (!detail::contain(world_, found.error(), member.id(), member.id()))
                return std::unexpected(found.error());
            if (const auto* body = impl_->entity_bodies.find(member.id()))
                remove(*body);
            impl_->retry.add(member.id());
        }
    }
    // Controllers bind Entities, components and bodies: any write, or a body or controller
    // created or removed since, reconciles them again, as does a failure they contained.
    if (full || !changed.empty() || controlled_ != next_revision_ + edits_ || components_->contained()) {
        ATELIER_TRY(components_->reconcile());
        controlled_ = next_revision_ + edits_;
    }
    reconciling.reset();
    // Bodies added to an empty broad phase, as every run start does, would otherwise be
    // rebalanced by Jolt during the first solves.
    if (unbuilt_ && impl_->system.GetNumBodies() != 0) {
        impl_->system.OptimizeBroadPhase();
        unbuilt_ = false;
    }
    constexpr float fixed = fixed_step;
    constexpr std::uint32_t maximum_steps = 8;
    StepResult result{.runtime_cooks = std::exchange(runtime_cooks_, 0), .completed_step = completed_step_};
    const auto finish_stopped = [&]() -> Result<void> {
        // Stop/close removed participation. Discard pending time and events,
        // including ended events queued by body removal, but keep completed solves.
        remaining_ = 0;
        single_step_ = false;
        tracked_ = false;
        impl_->ready_events.clear();
        result.remaining = 0;
        last_step_ = result;
        return {};
    };
    if (!paused_) {
        const float accumulated = remaining_ + elapsed;
        remaining_ = std::min(accumulated, fixed * maximum_steps);
    }
    auto& bodies = impl_->system.GetBodyInterface();
    const bool stepping_once = paused_ && single_step_;
    if (!paused_)
        single_step_ = false;
    while (((stepping_once && result.steps == 0) || (!paused_ && remaining_ + 1e-7f >= fixed)) &&
           result.steps < maximum_steps) {
        timing::Scope substep{phases::substep};
        if (before_step_) {
            ATELIER_TRY_BIND(opened, runtime.change_cursor());
            ATELIER_TRY(before_step_(completed_step_ + 1, fixed));
            if (!world_.running())
                return finish_stopped();
            ATELIER_TRY(follow_handler(*opened));
        }
        ATELIER_TRY(components_->before_step(fixed));
        auto& moved = impl_->moved;
        moved.clear();
        Array<Impl::Command> commands;
        commands.swap(impl_->commands);
        for (const auto& command : commands) {
            auto* target = impl_->resolve(command.handle, scene_id_);
            if (!target)
                continue;
            auto& body = *target;
            switch (command.kind) {
            case Impl::CommandKind::force:
                bodies.AddForce(body.id, vector(command.value));
                break;
            case Impl::CommandKind::impulse:
                bodies.AddImpulse(body.id, vector(command.value));
                break;
            case Impl::CommandKind::kinematic_target:
                body.target.position = command.value;
                body.target.rotation = command.rotation;
                break;
            case Impl::CommandKind::teleport:
                body.target.position = command.value;
                body.target.rotation = command.rotation;
                // The step starts from the teleported pose, so it draws no trail from the old one.
                impl_->history.insert_or_assign(body.entity, Impl::Pose{command.value, command.rotation});
                bodies.SetPositionAndRotation(body.id, position(command.value), rotation(command.rotation),
                                              body.motion == MotionKind::dynamic ? JPH::EActivation::Activate
                                                                                 : JPH::EActivation::DontActivate);
                moved.push_back(body.id);
                if (!command.preserve_velocity)
                    bodies.SetLinearAndAngularVelocity(body.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                break;
            }
        }
        for (const auto component : impl_->body_order.get(impl_->bodies)) {
            const auto& body = impl_->bodies.at(component);
            if (body.motion == MotionKind::kinematic)
                bodies.MoveKinematic(body.id, position(body.target.position), rotation(body.target.rotation), fixed);
        }
        for (const auto entity : impl_->character_order.get(impl_->characters)) {
            auto& controlled = impl_->characters.at(entity);
            auto& character = *controlled.character;
            character.UpdateGroundVelocity();
            auto velocity = character.GetLinearVelocity();
            const bool grounded = character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
            const auto ground = grounded ? character.GetGroundVelocity() : JPH::Vec3::sZero();
            const auto gravity = impl_->system.GetGravity();
            velocity = JPH::Vec3(controlled.input.x + ground.GetX(),
                                 grounded ? ground.GetY() : velocity.GetY() + gravity.GetY() * fixed,
                                 controlled.input.z + ground.GetZ());
            if (controlled.jump && grounded)
                velocity.SetY(controlled.settings.jump_speed);
            controlled.jump = false;
            character.SetLinearVelocity(velocity);
            JPH::CharacterVirtual::ExtendedUpdateSettings step_settings;
            step_settings.mWalkStairsStepUp = JPH::Vec3(0, controlled.settings.step_height, 0);
            step_settings.mStickToFloorStepDown = JPH::Vec3(0, -controlled.settings.step_height, 0);
            character.ExtendedUpdate(fixed, gravity, step_settings,
                                     impl_->system.GetDefaultBroadPhaseLayerFilter(Impl::moving_layer),
                                     impl_->system.GetDefaultLayerFilter(Impl::moving_layer), {}, {}, impl_->temporary);
        }
        {
            timing::Scope solving{phases::solve};
            impl_->activation.deactivated.clear();
            // Full body pair or contact buffers only drop contacts; Jolt keeps simulating.
            const auto updated = impl_->system.Update(fixed, 1, &impl_->temporary, solver_jobs);
            if (updated != JPH::EPhysicsUpdateError::None) {
                ++result.contact_overflows;
                const auto report = [&](JPH::EPhysicsUpdateError bits, Utf8Text field, Utf8Text message) {
                    if ((updated & bits) == JPH::EPhysicsUpdateError::None)
                        return;
                    world::Diagnostic diagnostic;
                    diagnostic.severity = world::Severity::warning;
                    diagnostic.kind = world::DiagnosticKind::capacity_exceeded;
                    diagnostic.phase = world::DiagnosticPhase::physics;
                    diagnostic.field = {std::move(field).into_string()};
                    diagnostic.message = std::move(message).into_string();
                    world_.report(std::move(diagnostic));
                };
                report(JPH::EPhysicsUpdateError::BodyPairCacheFull, "body_pairs",
                       "The physics body pair buffer is full; some contacts were ignored. Raise Capacity::body_pairs.");
                report(
                    JPH::EPhysicsUpdateError::ManifoldCacheFull | JPH::EPhysicsUpdateError::ContactConstraintsFull,
                    "contact_constraints",
                    "The physics contact buffer is full; some contacts were ignored. Raise Capacity::contact_constraints.");
            }
            std::lock_guard lock(impl_->contacts.mutex);
            if (const auto dropped = std::exchange(impl_->contacts.dropped, 0)) {
                world::Diagnostic diagnostic;
                diagnostic.severity = world::Severity::warning;
                diagnostic.kind = world::DiagnosticKind::capacity_exceeded;
                diagnostic.phase = world::DiagnosticPhase::physics;
                diagnostic.field = {"contact_notifications"_s};
                diagnostic.value = String::concat("dropped=", dropped, "; capacity=16384");
                diagnostic.message =
                    "The physics contact notification queue is full; some notifications were dropped."_s;
                world_.report(std::move(diagnostic));
            }
        }
        Optional<timing::Scope> writing{std::in_place, phases::write_back};
        // Only bodies this step moved publish a pose: active ones, those that fell asleep
        // during it and teleported ones. Sleeping and static bodies keep the World pose.
        impl_->system.GetActiveBodies(JPH::EBodyType::RigidBody, impl_->active);
        moved.insert(moved.end(), impl_->active.begin(), impl_->active.end());
        moved.insert(moved.end(), impl_->activation.deactivated.begin(), impl_->activation.deactivated.end());
        // Solver threads list bodies in any order; the batch must not depend on it.
        std::sort(moved.begin(), moved.end());
        moved.erase(std::unique(moved.begin(), moved.end()), moved.end());
        // Update has returned and only this thread uses the system, so reads need no body lock.
        auto& reader = impl_->system.GetBodyInterfaceNoLock();
        auto& poses = impl_->poses;
        auto& posed = impl_->posed;
        poses.clear();
        posed.clear();
        const auto publish = [&](Impl::Body& body) {
            JPH::RVec3 where;
            JPH::Quat orientation;
            reader.GetPositionAndRotation(body.id, where, orientation);
            poses.add({body.entity, vector(where), rotation(orientation)});
            posed.add(&body);
        };
        for (const auto id : moved)
            if (const auto* owner = impl_->body_components.find(id))
                publish(impl_->bodies.at(*owner));
        for (const auto entity : impl_->character_order.get(impl_->characters)) {
            const auto& controlled = impl_->characters.at(entity);
            poses.add(
                {entity, vector(controlled.character->GetPosition()), rotation(controlled.character->GetRotation())});
        }
        // Manual native ragdolls have no authoring Entry but still publish their owner pose.
        for (const auto entity : impl_->ragdoll_order.get(impl_->ragdolls)) {
            const auto& controlled = impl_->ragdolls.at(entity);
            JPH::RVec3 where;
            JPH::Quat orientation;
            reader.GetPositionAndRotation(controlled.ragdoll->GetBodyIDs().front(), where, orientation);
            poses.add({entity, vector(where), rotation(orientation)});
        }
        ATELIER_TRY(components_->append_poses(poses));
        posed.resize(poses.size());
        // A body below a published Entity is published too, so that its local pose keeps
        // the world pose Jolt holds for it.
        if (!poses.empty() &&
            std::ranges::any_of(impl_->bodies, [](const auto& item) { return item.value.ancestors.size() > 1; })) {
            Set<ObjectId> published;
            published.reserve(poses.size());
            for (const auto& pose : poses)
                published.add(pose.entity);
            for (const auto component : impl_->body_order.get(impl_->bodies)) {
                auto& body = impl_->bodies.at(component);
                if (body.ancestors.size() > 1 && !published.contains(body.entity) &&
                    std::any_of(body.ancestors.begin() + 1, body.ancestors.end(),
                                [&](ObjectId node) { return published.contains(node); }))
                    publish(body);
            }
        }
        ATELIER_TRY(world_.apply_simulation_transforms(poses, &impl_->written));
        // `written` keeps the order of `poses` and omits rejected ones.
        auto& interpolated = impl_->interpolated;
        interpolated.clear();
        for (std::size_t i = 0, j = 0; i < poses.size() && j < impl_->written.size(); ++i) {
            if (poses[i].entity != impl_->written[j].first)
                continue;
            const auto& local = impl_->written[j++].second;
            // A bound part can also own a body until the next reconcile rejects one of them.
            auto* body = posed[i];
            if (!body)
                if (const auto* owner = impl_->entity_bodies.find(poses[i].entity))
                    body = &impl_->bodies.at(*owner);
            if (body) {
                auto& published = *body;
                published.local = local;
                published.held = {poses[i].position, poses[i].rotation, {1, 1, 1}};
                // Below an ancestor the local TRS rounds the pose; reconcile compares against that rounding.
                // A structural edit since this step's reconcile can have given a root body an ancestor.
                if (published.ancestors.size() > 1 || world_.revision() != reconciled_)
                    if (auto matrix = world::world_matrix(runtime, poses[i].entity))
                        if (auto authored = world::relative_transform(*matrix, world::transform_matrix({})))
                            published.held = *authored;
            }
            const Impl::Pose to{poses[i].position, poses[i].rotation};
            auto& from = *impl_->history.try_emplace(poses[i].entity, to).value;
            interpolated.add({poses[i].entity, from, to});
            from = to;
        }
        writing.reset();
        if (!stepping_once)
            remaining_ = std::max(0.f, remaining_ - fixed);
        else
            single_step_ = false;
        ++result.steps;
        result.completed_step = ++completed_step_;
        ATELIER_TRY(components_->after_step());
        impl_->collect_contacts(completed_step_, scene_id_);
        if (after_step_) {
            ATELIER_TRY_BIND(opened, runtime.change_cursor());
            ATELIER_TRY(after_step_(completed_step_, fixed));
            if (!world_.running())
                return finish_stopped();
            ATELIER_TRY(follow_handler(*opened));
        }
    }
    result.remaining = remaining_;
    present();
    // Every write since this step's reconcile was physics' own or a step handler's (pending);
    // later ones, contact handlers' included, are found by the next step.
    ATELIER_TRY_BIND(cursor, runtime.change_cursor());
    cursor_ = *cursor;
    tracked_ = true;
    timing::Scope dispatching{phases::events};
    Array<ContactEvent> events;
    events.swap(impl_->ready_events);
    for (const auto& event : events) {
        if (!handler_)
            break;
        if (!world_.runtime().type_of(event.first.entity) || !world_.runtime().type_of(event.second.entity))
            continue;
        ATELIER_TRY(handler_(event));
        if (!world_.running())
            return finish_stopped();
    }
    last_step_ = result;
    return {};
}

Result<void> PhysicsScene::set_gravity(math::Vector3 gravity) {
    if (!math::is_finite(gravity))
        return failure(ErrorCode::argument_mismatch, "Gravity must be finite.");
    impl_->system.SetGravity(vector(gravity));
    return {};
}

Result<void> PhysicsScene::add_force(BodyHandle handle, math::Vector3 force) {
    if (!math::is_finite(force))
        return failure(ErrorCode::argument_mismatch, "Force must be finite.");
    const auto* body = impl_->resolve(handle, scene_id_);
    if (!body)
        return failure(ErrorCode::invalid_member, "Physical body handle is stale or belongs to another scene.");
    if (body->motion != MotionKind::dynamic)
        return failure(ErrorCode::invalid_member, "Force requires an active dynamic body.");
    impl_->commands.add({Impl::CommandKind::force, handle, force});
    return {};
}

Result<void> PhysicsScene::add_impulse(BodyHandle handle, math::Vector3 impulse) {
    if (!math::is_finite(impulse))
        return failure(ErrorCode::argument_mismatch, "Impulse must be finite.");
    const auto* body = impl_->resolve(handle, scene_id_);
    if (!body)
        return failure(ErrorCode::invalid_member, "Physical body handle is stale or belongs to another scene.");
    if (body->motion != MotionKind::dynamic)
        return failure(ErrorCode::invalid_member, "Impulse requires an active dynamic body.");
    impl_->commands.add({Impl::CommandKind::impulse, handle, impulse});
    return {};
}

bool PhysicsScene::writes_pose(ObjectId entity) const {
    if (pose_writer(entity) != ObjectId{} || components_->bound_pose_writer(entity) != ObjectId{})
        return true;
    // An active Character or Ragdoll claims its owner before the next step creates the controller.
    auto& runtime = world_.runtime();
    auto group = world::components(runtime, entity);
    if (!group)
        return true;
    for (const auto& component : *group) {
        auto type = runtime.type_of(component.id());
        if (!type)
            return true;
        for (const auto controller : {types_.character_component, types_.ragdoll_component}) {
            auto is = runtime.is_a(*type, controller);
            if (!is)
                return true;
            if (!*is)
                continue;
            auto status = world::inspect_component(runtime, component.id());
            if (!status || status->active)
                return true;
        }
    }
    return false;
}

// An authored pose replaces the simulated one at the next step; velocities are kept.
// Everything is resolved first, so a refusal leaves the simulation untouched.
Result<void> PhysicsScene::relocate(Span<const ObjectId> entities) {
    struct Target {
        ObjectId entity;

        Optional<BodyHandle> body;

        world::Trs pose;

        world::Trs local;
    };
    Array<Target> targets;
    targets.reserve(entities.size());
    for (const auto entity : entities) {
        if (impl_->ragdolls.contains(entity) || components_->bound_pose_writer(entity) != ObjectId{})
            return failure(
                ErrorCode::unsupported_operation,
                "A Ragdoll and its parts follow their controller; an authored transform applies after a restart.");
        ATELIER_TRY_BIND(matrix, world::world_matrix(world_.runtime(), entity));
        // A degenerate space has no body; the next reconcile drops it.
        auto relative = world::relative_transform(*matrix, world::transform_matrix({}));
        if (!relative)
            continue;
        ATELIER_TRY_BIND(pose, world::canonical_transform({relative->position, relative->rotation, {1, 1, 1}}));
        Optional<BodyHandle> body;
        if (!impl_->characters.contains(entity)) {
            auto handle = body_handle(entity);
            // A controller claimed before its first step has nothing to move yet.
            if (!handle)
                continue;
            body = *handle;
        }
        world::Trs local{};
        if (body) {
            ATELIER_TRY_BIND(authored, world::read_transform(world_.runtime(), entity));
            local = *authored;
        }
        targets.add({entity, body, *pose, local});
    }
    // Relocated Entities are drawn at their new World pose; their next step moves from there.
    Array<world::SimulationPose> held;
    held.reserve(targets.size());
    for (const auto& target : targets)
        held.add({target.entity, target.pose.position, target.pose.rotation});
    hold_poses(held);
    for (const auto& target : targets) {
        if (target.body) {
            impl_->commands.add(
                {Impl::CommandKind::teleport, *target.body, target.pose.position, target.pose.rotation, true});
            auto& body = impl_->bodies.at(target.body->component);
            body.local = target.local;
            body.held = target.pose;
            continue;
        }
        auto& character = *impl_->characters.at(target.entity).character;
        character.SetPosition(position(target.pose.position));
        character.SetRotation(rotation(target.pose.rotation));
    }
    return {};
}

ObjectId PhysicsScene::pose_writer(ObjectId entity) const noexcept {
    if (const auto* body = impl_->entity_bodies.find(entity))
        return *body;
    return impl_->characters.contains(entity) || impl_->ragdolls.contains(entity) ? entity : ObjectId{};
}

Result<bool> PhysicsScene::controller_transform_matches(ObjectId entity, math::Vector3 position,
                                                        math::Quaternion orientation) const {
    ATELIER_TRY_BIND(matrix, world::world_matrix(world_.runtime(), entity));
    ATELIER_TRY_BIND(authored, world::relative_transform(*matrix, world::transform_matrix({})));
    if (std::abs(authored->scale.x - 1) > 1e-5f || std::abs(authored->scale.y - 1) > 1e-5f ||
        std::abs(authored->scale.z - 1) > 1e-5f)
        return false;
    for (std::size_t i = 0; i < 3; ++i)
        if (std::abs(position[i] - authored->position[i]) > 1e-5f * std::max(1.f, std::abs(authored->position[i])))
            return false;
    const auto expected = rotation(authored->rotation);
    const auto actual = rotation(orientation);
    const auto sign = actual.Dot(expected) < 0 ? -1.f : 1.f;
    return std::abs(actual.GetX() - sign * expected.GetX()) <= 1e-5f &&
           std::abs(actual.GetY() - sign * expected.GetY()) <= 1e-5f &&
           std::abs(actual.GetZ() - sign * expected.GetZ()) <= 1e-5f &&
           std::abs(actual.GetW() - sign * expected.GetW()) <= 1e-5f;
}

Result<BodyHandle> PhysicsScene::body_handle(ObjectId object) const {
    ObjectId component = object;
    const auto* found = impl_->bodies.find(component);
    if (!found)
        if (const auto* owner = impl_->entity_bodies.find(object)) {
            component = *owner;
            found = impl_->bodies.find(component);
        }
    if (!found)
        return failure(ErrorCode::invalid_member, "Primitive or Entity has no active body in this scene.");
    return BodyHandle{found->entity, component, scene_id_, found->revision};
}

Result<BodyState> PhysicsScene::read_body_state(BodyHandle handle) const {
    const auto* body = impl_->resolve(handle, scene_id_);
    if (!body)
        return failure(ErrorCode::invalid_member, "Physical body handle is stale or belongs to another scene.");
    auto& bodies = impl_->system.GetBodyInterface();
    JPH::RVec3 where;
    JPH::Quat orientation;
    bodies.GetPositionAndRotation(body->id, where, orientation);
    const auto linear = bodies.GetLinearVelocity(body->id);
    const auto angular = bodies.GetAngularVelocity(body->id);
    return BodyState{handle,
                     completed_step_,
                     vector(where),
                     {linear.GetX(), linear.GetY(), linear.GetZ()},
                     {angular.GetX(), angular.GetY(), angular.GetZ()},
                     rotation(orientation),
                     !bodies.IsActive(body->id)};
}

Result<void> PhysicsScene::set_kinematic_target(BodyHandle handle, math::Vector3 where, math::Quaternion orientation) {
    const auto* body = impl_->resolve(handle, scene_id_);
    if (!body)
        return failure(ErrorCode::invalid_member, "Physical body handle is stale or belongs to another scene.");
    if (body->motion != MotionKind::kinematic)
        return failure(ErrorCode::invalid_member, "Kinematic target requires a kinematic body.");
    ATELIER_TRY_BIND(valid, world::canonical_transform({where, orientation, {1, 1, 1}}));
    impl_->commands.add({Impl::CommandKind::kinematic_target, handle, valid->position, valid->rotation});
    return {};
}

Result<void> PhysicsScene::teleport(BodyHandle handle, math::Vector3 where, math::Quaternion orientation,
                                    bool preserve_velocity) {
    const auto* body = impl_->resolve(handle, scene_id_);
    if (!body)
        return failure(ErrorCode::invalid_member, "Physical body handle is stale or belongs to another scene.");
    ATELIER_TRY_BIND(valid, world::canonical_transform({where, orientation, {1, 1, 1}}));
    impl_->commands.add(
        {Impl::CommandKind::teleport, handle, valid->position, valid->rotation, preserve_velocity});
    return {};
}

Result<ConstraintHandle> PhysicsScene::create_distance_constraint(BodyHandle first, BodyHandle second, float minimum,
                                                                  float maximum) {
    if (first.component == second.component || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum < 0 ||
        maximum < minimum)
        return failure(ErrorCode::argument_mismatch, "Distance constraint requires two bodies and a valid range.");
    ATELIER_TRY(read_body_state(first));
    ATELIER_TRY(read_body_state(second));
    const auto left = impl_->bodies.at(first.component).id;
    const auto right = impl_->bodies.at(second.component).id;
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
    impl_->distances.try_emplace(revision, Impl::Distance{first, second, constraint});
    return ConstraintHandle{scene_id_, revision};
}

Result<void> PhysicsScene::destroy_constraint(ConstraintHandle handle) {
    if (handle.scene != scene_id_)
        return failure(ErrorCode::invalid_member, "Distance constraint handle is stale.");
    auto removed = impl_->distances.take(handle.revision);
    if (!removed)
        return failure(ErrorCode::invalid_member, "Distance constraint handle is stale.");
    impl_->system.RemoveConstraint(removed->constraint);
    // Removing support must resume sleeping endpoints as well as active ones.
    // Jolt removes the constraint without waking its bodies.
    auto& bodies = impl_->system.GetBodyInterface();
    bodies.ActivateBody(impl_->bodies.at(removed->first.component).id);
    bodies.ActivateBody(impl_->bodies.at(removed->second.component).id);
    return {};
}

Result<SpecialHandle> PhysicsScene::create_character(ObjectId entity, CharacterSettings settings) {
    if (!std::isfinite(settings.radius) || settings.radius <= 0 || !std::isfinite(settings.capsule_half_height) ||
        settings.capsule_half_height <= 0 || !std::isfinite(settings.max_slope_radians) ||
        settings.max_slope_radians < 0 || settings.max_slope_radians >= 1.5707963f ||
        !std::isfinite(settings.jump_speed) || settings.jump_speed < 0 || !std::isfinite(settings.step_height) ||
        settings.step_height < 0)
        return failure(ErrorCode::argument_mismatch, "Invalid character dimensions or movement settings.");
    if (!world_.running() || impl_->characters.contains(entity) || bool(body_handle(entity)) ||
        impl_->ragdolls.contains(entity) || components_->bound_pose_writer(entity) != ObjectId{})
        return failure(ErrorCode::invalid_member, "Character requires an active Entity without another physics body.");
    ATELIER_TRY(validate_active_entity(world_, entity));
    ATELIER_TRY_BIND(spatial, world::has_transform(world_.runtime(), entity));
    if (!*spatial)
        return failure(ErrorCode::invalid_member, "Physics controller requires a spatial Entity.");
    ATELIER_TRY_BIND(matrix, world::world_matrix(world_.runtime(), entity));
    ATELIER_TRY_BIND(trs, world::relative_transform(*matrix, world::transform_matrix({})));
    if (std::abs(trs->scale.x - 1) > 1e-5f || std::abs(trs->scale.y - 1) > 1e-5f || std::abs(trs->scale.z - 1) > 1e-5f)
        return failure(ErrorCode::argument_mismatch, "Character Entity requires unit, unmirrored world scale.");
    JPH::Ref<JPH::CharacterVirtualSettings> jolt = new JPH::CharacterVirtualSettings();
    jolt->mShape = new JPH::CapsuleShape(settings.capsule_half_height, settings.radius);
    jolt->mMaxSlopeAngle = settings.max_slope_radians;
    JPH::Ref<JPH::CharacterVirtual> character =
        new JPH::CharacterVirtual(jolt, position(trs->position), rotation(trs->rotation), 0, &impl_->system);
    const auto revision = next_revision_++;
    impl_->characters.try_emplace(entity, Impl::Character{revision, settings, character});
    impl_->character_order.invalidate();
    const world::SimulationPose held_pose{entity, trs->position, trs->rotation};
    hold_poses({&held_pose, 1});
    return SpecialHandle{entity, scene_id_, revision};
}

Result<void> PhysicsScene::set_character_input(SpecialHandle handle, math::Vector3 planar_velocity, bool jump) {
    auto* found = impl_->characters.find(handle.entity);
    if (handle.scene != scene_id_ || !found || found->revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Character handle is stale.");
    if (!math::is_finite(planar_velocity) || planar_velocity.y != 0)
        return failure(ErrorCode::argument_mismatch, "Character input needs a finite horizontal velocity.");
    found->input = planar_velocity;
    found->jump |= jump;
    return {};
}

Result<SpecialHandle> PhysicsScene::character_handle(ObjectId entity) const {
    const auto* found = impl_->characters.find(entity);
    if (!found)
        return failure(ErrorCode::invalid_member, "Entity has no active character.");
    return SpecialHandle{entity, scene_id_, found->revision};
}

Result<SpecialHandle> PhysicsScene::ragdoll_handle(ObjectId entity) const {
    const auto* found = impl_->ragdolls.find(entity);
    if (!found)
        return failure(ErrorCode::invalid_member, "Entity has no active ragdoll.");
    return SpecialHandle{entity, scene_id_, found->revision};
}

Result<CharacterState> PhysicsScene::read_character_state(SpecialHandle handle) const {
    const auto* found = impl_->characters.find(handle.entity);
    if (handle.scene != scene_id_ || !found || found->revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Character handle is stale.");
    const auto& character = *found->character;
    const auto velocity = character.GetLinearVelocity();
    return CharacterState{handle,
                          completed_step_,
                          vector(character.GetPosition()),
                          {velocity.GetX(), velocity.GetY(), velocity.GetZ()},
                          character.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround};
}

Result<void> PhysicsScene::destroy_character(SpecialHandle handle) {
    ATELIER_TRY(read_character_state(handle));
    forget_pose(handle.entity);
    impl_->characters.remove(handle.entity);
    impl_->character_order.invalidate();
    ++edits_;
    return {};
}

Result<SpecialHandle> PhysicsScene::create_ragdoll(ObjectId entity, Span<const RagdollPart> parts) {
    if (parts.empty() || parts.size() > 128 || !world_.running() || bool(body_handle(entity)) ||
        impl_->characters.contains(entity) || impl_->ragdolls.contains(entity) ||
        components_->bound_pose_writer(entity) != ObjectId{})
        return failure(ErrorCode::argument_mismatch, "Ragdoll requires 1-128 parts and an unused active Entity.");
    ATELIER_TRY(validate_active_entity(world_, entity));
    ATELIER_TRY_BIND(spatial, world::has_transform(world_.runtime(), entity));
    if (!*spatial)
        return failure(ErrorCode::invalid_member, "Physics controller requires a spatial Entity.");
    ATELIER_TRY_BIND(matrix, world::world_matrix(world_.runtime(), entity));
    ATELIER_TRY_BIND(trs, world::relative_transform(*matrix, world::transform_matrix({})));
    if (std::abs(trs->scale.x - 1) > 1e-5f || std::abs(trs->scale.y - 1) > 1e-5f || std::abs(trs->scale.z - 1) > 1e-5f)
        return failure(ErrorCode::argument_mismatch, "Ragdoll Entity requires unit, unmirrored world scale.");
    const auto world_part = [&](math::Vector3 local) {
        const auto rotated = math::rotate(trs->rotation, local);
        return position({trs->position.x + rotated.x, trs->position.y + rotated.y, trs->position.z + rotated.z});
    };
    JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();
    settings->mSkeleton = new JPH::Skeleton();
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& source = parts[index];
        if (source.name.empty() || source.parent < -1 || source.parent >= static_cast<int>(index) ||
            !math::is_finite(source.local_position) || !std::isfinite(source.radius) || source.radius <= 0 ||
            !std::isfinite(source.capsule_half_height) || source.capsule_half_height <= 0)
            return failure(ErrorCode::argument_mismatch, "Ragdoll part has invalid hierarchy or dimensions.");
        settings->mSkeleton->AddJoint(source.name.view().std_view(), source.parent);
        JPH::RagdollSettings::Part part;
        part.SetShape(new JPH::CapsuleShape(source.capsule_half_height, source.radius));
        part.mPosition = world_part(source.local_position);
        part.mRotation = rotation(trs->rotation);
        part.mMotionType = JPH::EMotionType::Dynamic;
        part.mObjectLayer = Impl::moving_layer;
        if (source.parent >= 0) {
            JPH::Ref<JPH::PointConstraintSettings> joint = new JPH::PointConstraintSettings();
            const auto& parent = parts[source.parent];
            joint->mPoint1 = joint->mPoint2 = world_part({(parent.local_position.x + source.local_position.x) * .5f,
                                                          (parent.local_position.y + source.local_position.y) * .5f,
                                                          (parent.local_position.z + source.local_position.z) * .5f});
            part.mToParent = joint;
        }
        settings->mParts.push_back(part);
    }
    settings->mSkeleton->CalculateParentJointIndices();
    settings->DisableParentChildCollisions();
    JPH::Ref<JPH::Ragdoll> ragdoll =
        settings->CreateRagdoll(static_cast<JPH::CollisionGroup::GroupID>(next_revision_), 0, &impl_->system);
    if (!ragdoll)
        return failure(ErrorCode::capacity_exceeded, "Jolt could not create the ragdoll.");
    ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
    for (const auto id : ragdoll->GetBodyIDs())
        impl_->ragdoll_ids.add(id);
    const auto revision = next_revision_++;
    impl_->ragdolls.try_emplace(entity, Impl::Ragdoll{revision, ragdoll});
    impl_->ragdoll_order.invalidate();
    const world::SimulationPose held_pose{entity, trs->position, trs->rotation};
    hold_poses({&held_pose, 1});
    return SpecialHandle{entity, scene_id_, revision};
}

Result<RagdollPose> PhysicsScene::read_ragdoll_pose(SpecialHandle handle) const {
    const auto* found = impl_->ragdolls.find(handle.entity);
    if (handle.scene != scene_id_ || !found || found->revision != handle.revision)
        return failure(ErrorCode::invalid_member, "Ragdoll handle is stale.");
    RagdollPose pose{.handle = handle, .step_id = completed_step_, .positions = {}, .rotations = {}};
    auto& bodies = impl_->system.GetBodyInterface();
    for (const auto id : found->ragdoll->GetBodyIDs()) {
        JPH::RVec3 where;
        JPH::Quat orientation;
        bodies.GetPositionAndRotation(id, where, orientation);
        pose.positions.add(vector(where));
        pose.rotations.add(rotation(orientation));
    }
    return pose;
}

Result<void> PhysicsScene::destroy_ragdoll(SpecialHandle handle) {
    ATELIER_TRY(read_ragdoll_pose(handle));
    remove_special(handle.entity);
    return {};
}

namespace {
Result<void> validate_query(const QueryFilter& filter) {
    if (filter.capacity == 0 || filter.capacity > 4096 || filter.category >= filter.responses.size() ||
        (filter.policy != QueryPolicy::interaction && filter.policy != QueryPolicy::channel &&
         filter.policy != QueryPolicy::object_type) ||
        (filter.mode != QueryMode::any && filter.mode != QueryMode::closest && filter.mode != QueryMode::all))
        return failure(ErrorCode::argument_mismatch, "Invalid physics query mode or capacity.");
    for (auto response : filter.responses)
        if (response != Response::ignore && response != Response::overlap && response != Response::block)
            return failure(ErrorCode::argument_mismatch, "Invalid physics query response.");
    return {};
}

void finish_query(QueryResult& result, const QueryFilter& filter) {
    std::sort(result.hits.begin(), result.hits.end(), [](const QueryHit& a, const QueryHit& b) {
        if (a.fraction != b.fraction)
            return a.fraction < b.fraction;
        if (a.body.revision != b.body.revision)
            return a.body.revision < b.body.revision;
        return a.sub_shape < b.sub_shape;
    });
    if (filter.stop_at_block) {
        const auto first = std::ranges::find_if(result.hits, [](const auto& hit) { return hit.blocking; });
        if (first != result.hits.end())
            result.hits.truncate(static_cast<std::size_t>(first - result.hits.begin()) + 1);
    }
    if (filter.mode != QueryMode::all && result.hits.size() > 1)
        result.hits.truncate(1);
    result.has_hit = !result.hits.empty();
    result.has_block = std::ranges::any_of(result.hits, [](const QueryHit& hit) { return hit.blocking; });
    const auto limit = filter.mode == QueryMode::all ? filter.capacity : std::size_t{1};
    if (result.hits.size() > limit) {
        result.overflow = filter.mode == QueryMode::all;
        result.hits.truncate(limit);
    }
    const auto delta = result.end - result.start;
    const auto length = std::hypot(delta.x, delta.y, delta.z);
    for (auto& hit : result.hits) {
        hit.distance = length * hit.fraction;
        hit.location = result.start + delta * hit.fraction;
    }
}

bool finite_segment(math::Vector3 start, math::Vector3 end) {
    const auto delta = end - start;
    return math::is_finite(start) && math::is_finite(end) && math::is_finite(delta) &&
           std::isfinite(std::hypot(delta.x, delta.y, delta.z));
}
}

Result<QueryResult> PhysicsScene::raycast(math::Vector3 start, math::Vector3 end, QueryFilter filter) const {
    if (!finite_segment(start, end))
        return failure(ErrorCode::argument_mismatch, "Ray endpoints must be finite.");
    ATELIER_TRY(validate_query(filter));
    QueryResult result;
    result.step_id = completed_step_;
    result.start = start;
    result.end = end;
    const JPH::RRayCast ray(position(start), vector(math::Vector3{end.x - start.x, end.y - start.y, end.z - start.z}));
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, collector);
    for (const auto& hit : collector.mHits) {
        if (!impl_->query_accept(hit.mBodyID, filter))
            continue;
        const auto point = ray.GetPointOnRay(hit.mFraction);
        math::Vector3 normal{};
        if (hit.mFraction > 0) {
            JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), hit.mBodyID);
            if (lock.Succeeded()) {
                const auto value = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
                normal = {value.GetX(), value.GetY(), value.GetZ()};
            }
        }
        result.hits.add(impl_->query_hit(hit.mBodyID, hit.mSubShapeID2.GetValue(), hit.mFraction, vector(point),
                                         normal, hit.mFraction == 0, scene_id_, filter));
    }
    finish_query(result, filter);
    return result;
}

Result<QueryResult> PhysicsScene::sweep_sphere(math::Vector3 start, math::Vector3 end, float radius,
                                               QueryFilter filter) const {
    if (!finite_segment(start, end) || !std::isfinite(radius) || radius <= 0)
        return failure(ErrorCode::argument_mismatch, "Sphere sweep requires finite endpoints and positive radius.");
    ATELIER_TRY(validate_query(filter));
    QueryResult result;
    result.step_id = completed_step_;
    result.start = start;
    result.end = end;
    const JPH::ShapeRefC shape = new JPH::SphereShape(radius);
    const JPH::RShapeCast cast(shape.GetPtr(), JPH::Vec3::sReplicate(1.f), JPH::RMat44::sTranslation(position(start)),
                               vector(math::Vector3{end.x - start.x, end.y - start.y, end.z - start.z}));
    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    JPH::ShapeCastSettings cast_settings;
    cast_settings.mReturnDeepestPoint = true;
    cast_settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    impl_->system.GetNarrowPhaseQuery().CastShape(cast, cast_settings, JPH::RVec3::sZero(), collector);
    for (const auto& hit : collector.mHits) {
        if (!impl_->query_accept(hit.mBodyID2, filter))
            continue;
        auto normal = hit.mPenetrationAxis;
        if (normal.LengthSq() > 0)
            normal = -normal.Normalized();
        result.hits.add(
            impl_->query_hit(hit.mBodyID2, hit.mSubShapeID2.GetValue(), hit.mFraction,
                             {hit.mContactPointOn2.GetX(), hit.mContactPointOn2.GetY(), hit.mContactPointOn2.GetZ()},
                             {normal.GetX(), normal.GetY(), normal.GetZ()}, hit.mFraction == 0, scene_id_, filter));
        result.hits.back().penetration_depth = std::max(0.f, hit.mPenetrationDepth);
    }
    finish_query(result, filter);
    return result;
}

Result<QueryResult> PhysicsScene::overlap_sphere(math::Vector3 center, float radius, QueryFilter filter) const {
    if (!math::is_finite(center) || !std::isfinite(radius) || radius <= 0)
        return failure(ErrorCode::argument_mismatch, "Sphere overlap requires finite center and positive radius.");
    ATELIER_TRY(validate_query(filter));
    QueryResult result;
    result.step_id = completed_step_;
    result.start = center;
    result.end = center;
    const JPH::ShapeRefC shape = new JPH::SphereShape(radius);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CollideShape(shape.GetPtr(), JPH::Vec3::sReplicate(1.f),
                                                     JPH::RMat44::sTranslation(position(center)),
                                                     JPH::CollideShapeSettings{}, JPH::RVec3::sZero(), collector);
    for (const auto& hit : collector.mHits) {
        if (!impl_->query_accept(hit.mBodyID2, filter))
            continue;
        auto normal = hit.mPenetrationAxis;
        if (normal.LengthSq() > 0)
            normal = -normal.Normalized();
        result.hits.add(
            impl_->query_hit(hit.mBodyID2, hit.mSubShapeID2.GetValue(), 0,
                             {hit.mContactPointOn2.GetX(), hit.mContactPointOn2.GetY(), hit.mContactPointOn2.GetZ()},
                             {normal.GetX(), normal.GetY(), normal.GetZ()}, true, scene_id_, filter));
        result.hits.back().penetration_depth = std::max(0.f, hit.mPenetrationDepth);
    }
    finish_query(result, filter);
    return result;
}

std::size_t PhysicsScene::body_count() const noexcept {
    return impl_->bodies.size();
}

std::size_t PhysicsScene::active_body_count() const noexcept {
    return impl_->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

Result<QueryResult> PhysicsScene::trace(math::Vector3 start, math::Vector3 end, const TraceSettings& settings,
                                        QueryIgnore ignore) const {
    if (settings.shape > TraceShape::sphere_overlap || settings.selection > TraceSelection::object_type ||
        settings.mode > TraceMode::multi || settings.channel >= 16 || settings.object_types > 0xffff ||
        !settings.capacity || settings.capacity > 4096)
        return failure(ErrorCode::argument_mismatch,
                       "Invalid trace shape, selection, mode, channel, mask or capacity.");
    if (settings.shape == TraceShape::sphere_overlap)
        end = start;
    if (!finite_segment(start, end) || (settings.shape != TraceShape::sphere_overlap && start == end))
        return failure(ErrorCode::argument_mismatch,
                       "Trace requires a finite nonzero segment; use sphere overlap for a stationary volume.");
    QueryFilter filter;
    filter.policy = settings.selection == TraceSelection::channel ? QueryPolicy::channel : QueryPolicy::object_type;
    filter.category = static_cast<std::uint8_t>(settings.channel);
    filter.categories =
        settings.selection == TraceSelection::channel ? 0xffff : static_cast<std::uint16_t>(settings.object_types);
    filter.mode = settings.mode == TraceMode::multi ? QueryMode::all : QueryMode::closest;
    filter.capacity = settings.capacity;
    filter.include_sensors = settings.include_sensors;
    filter.ignored_entities = ignore.entities;
    filter.ignored_components = ignore.components;
    filter.ignore_touches = settings.selection == TraceSelection::channel && settings.mode != TraceMode::multi;
    filter.stop_at_block = settings.selection == TraceSelection::channel && settings.mode == TraceMode::multi &&
                           settings.shape != TraceShape::sphere_overlap;
    auto result = settings.shape == TraceShape::line           ? raycast(start, end, filter)
                  : settings.shape == TraceShape::sphere_sweep ? sweep_sphere(start, end, settings.radius, filter)
                                                               : overlap_sphere(start, settings.radius, filter);
    if (result && settings.mode == TraceMode::test)
        result->hits.clear();
    return result;
}

Result<QueryResult> PhysicsScene::execute_query(ObjectId component, QueryIgnore ignore) {
    return components_->execute_query(component, ignore);
}

Result<QueryResult> PhysicsScene::read_query_result(ObjectId component) const {
    return components_->read_query_result(component);
}

}
