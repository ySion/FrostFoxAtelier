#pragma once

#include "runtime/execution.hpp"
#include "world/math.hpp"
#include <limits>

namespace atelier::world {

class World;
namespace detail { struct Access; struct WorldLifetime; }

// Native payloads participate in the same Runtime type hierarchy and GC graph.
// Their relation fields are private and registered as hidden reference fields.
class Actor {
public:
    Actor() = default;
    Actor(const Actor&) = delete;
    Actor& operator=(const Actor&) = delete;
private:
    friend struct detail::Access;
    ReferenceSequence components_;
    World* member_world_{};
    bool enabled_{true};
    bool transitioning_{};
};

class Component {
public:
    Component() = default;
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
private:
    friend struct detail::Access;
    ReferenceSlot owner_;
    bool enabled_{true};
};

struct Transform : Component {
    Vec3 position;
    Quaternion rotation;
    Vec3 scale{1.f, 1.f, 1.f};
};
class Cube : public Component {
public:
    Cube() = default;
private:
    friend struct detail::Access;
    ReferenceSlot transform_;
};
struct Counter : Component {
    float value{};
    std::uint32_t steps{};
    void advance(float amount) noexcept { value += amount; ++steps; }
};

using FieldPath = std::vector<FieldId>;
struct Types {
    TypeId actor, component, transform, cube, counter;
    TypeId vec3, quaternion;
    std::array<std::array<FieldId, 2>, 3> position_fields;
    std::array<std::array<FieldId, 2>, 4> rotation_fields;
    std::array<std::array<FieldId, 2>, 3> scale_fields;
    FieldId counter_value, counter_steps;
    MethodId counter_advance;
};
// Call once per Runtime, before object creation; additional derived definitions
// may be registered afterwards, before the Runtime closes registration.
[[nodiscard]] Result<Types> register_types(Runtime&);

enum class ActorState { detached, attached_inactive, active, invalid };
struct ActorStatus {
    ActorState state{ActorState::invalid};
    TypeId type;
    bool enabled{};
    bool can_edit{};
    bool can_present{};
};
struct ComponentStatus {
    bool valid{};
    TypeId type;
    ObjectId owner;
    bool enabled{};
    bool active{};
    bool can_present{};
};

[[nodiscard]] Result<ObjectRoot> create_actor(Runtime&, TypeId actual = {});
[[nodiscard]] Result<ObjectRoot> create_component(Runtime&, TypeId actual);
[[nodiscard]] Result<ActorStatus> inspect_actor(Runtime&, ObjectId);
[[nodiscard]] Result<ComponentStatus> inspect_component(Runtime&, ObjectId);
// These owning reference snapshots are ordinary roots, not a second relation graph.
[[nodiscard]] Result<std::vector<ObjectRoot>> components(Runtime&, ObjectId actor);
[[nodiscard]] Result<Trs> read_transform(Runtime&, ObjectId transform);
[[nodiscard]] Result<Matrix4> world_matrix(Runtime&, ObjectId transform);
[[nodiscard]] Result<std::optional<ObjectId>> root_transform(Runtime&, ObjectId actor);

struct Commit {
    bool changed{};
    // A completed relation change is never reported as if it had not happened.
    // Consumers record that change, then surface any lifecycle notification error.
    std::optional<Error> notification_error;
};
// The only game-object destruction entry. A detached object does not depend on
// its previous World. Attached objects first use their current World's exit chain.
[[nodiscard]] Result<Commit> destroy_actor(Runtime&, ObjectId);
[[nodiscard]] Result<Commit> destroy_component(Runtime&, ObjectId);

struct Member {
    ObjectRoot root;
    [[nodiscard]] ObjectId id() const noexcept { return root.id(); }
};
struct ActorRemoval {
    ObjectRoot root;
    std::size_t index{};
    Commit commit;
    [[nodiscard]] ObjectId id() const noexcept { return root.id(); }
private:
    friend class World;
    friend struct detail::Access;
    std::weak_ptr<detail::WorldLifetime> origin_;
};
struct ComponentRemoval {
    ObjectRoot root;
    ObjectId owner;
    std::size_t index{};
    Commit commit;
    [[nodiscard]] ObjectId id() const noexcept { return root.id(); }
private:
    friend class World;
    friend struct detail::Access;
    std::weak_ptr<detail::WorldLifetime> origin_;
};
struct Attachment {
    ObjectId component;
    // Required for a new Cube. An existing explicit binding is retained when
    // this is null; no component is selected implicitly by type or list order.
    ObjectId transform;
};
struct RenderItem {
    ObjectId actor, component, transform;
    Matrix4 world;
};

enum class Lifecycle { attached, activated, deactivated, detached };
enum class RequestKind {
    attach_actor, detach_actor, attach_component, remove_component,
    destroy_actor, destroy_component, actor_enabled, component_enabled,
    start, stop, close
};
inline constexpr std::size_t append = std::numeric_limits<std::size_t>::max();
struct Request {
    RequestKind kind;
    ObjectId target;
    ObjectId actor;       // attach_component destination
    ObjectId transform;   // explicit Cube binding
    std::size_t index{append};
    bool enabled{true};
};
struct RequestCompletion {
    std::uint64_t request;
    Result<Commit> result;
};
struct Activity {
    std::size_t update_registrations{};
    std::size_t presentation_registrations{};
    std::size_t tasks{};
};

// Same Runtime thread. Runtime outlives World, its detached objects and tasks.
// The World owns only member roots and execution/registration state. Actor's
// private member_world relation is authoritative; directories are indexes.
class World final {
public:
    using CounterUpdate = std::function<Result<void>(Runtime&, ObjectId, float)>;
    using LifecycleObserver = std::function<Result<void>(World&, Lifecycle, ObjectId)>;
    using TaskStart = std::function<Result<Coroutine>(Runtime&, ObjectId)>;

    World(Runtime&, const Types&);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    [[nodiscard]] Runtime& runtime() const noexcept;
    [[nodiscard]] const Types& types() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] Activity activity() const noexcept;
    // Borrow ends at the next structural change. Queries during a transition fail.
    [[nodiscard]] Result<std::span<const Member>> members() const;
    [[nodiscard]] Result<std::vector<ObjectId>> query(TypeId, bool include_derived = true) const;
    [[nodiscard]] Result<Commit> attach(ObjectId actor, std::size_t index = append);
    [[nodiscard]] Result<ActorRemoval> detach(ObjectId actor);
    [[nodiscard]] Result<Commit> restore(const ActorRemoval&);
    [[nodiscard]] Result<Commit> attach_components(ObjectId actor,
        std::span<const Attachment>, std::size_t index = append);
    [[nodiscard]] Result<ComponentRemoval> remove_component(ObjectId component);
    [[nodiscard]] Result<Commit> restore(const ComponentRemoval&);
    [[nodiscard]] Result<Commit> set_enabled(ObjectId actor_or_component, bool);
    [[nodiscard]] Result<Commit> start();
    [[nodiscard]] Result<Commit> stop();
    [[nodiscard]] Result<Commit> close();

    // Editing requires an open, stopped World and a current component member.
    // Transform scalar writes use the same finite/positive-scale TRS validation.
    [[nodiscard]] Result<Scalar> read(ObjectId component, std::span<const FieldId>) const;
    [[nodiscard]] Result<void> write(ObjectId component, std::span<const FieldId>, Scalar);
    [[nodiscard]] Result<void> set_transform(ObjectId transform, Trs);
    [[nodiscard]] Result<std::vector<RenderItem>> render_items() const;

    // Counter has a native default. An optional source/JIT adapter runs outside
    // all native borrows with the same Runtime and protected member identities.
    [[nodiscard]] Result<void> counter_update(CounterUpdate);
    [[nodiscard]] Result<void> observe_lifecycle(LifecycleObserver);
    [[nodiscard]] Result<void> update(float amount);
    // The trusted starter uses the provided Runtime/receiver. World owns the
    // resulting task; deactivate/detach/stop/close cancels it before exit completes.
    [[nodiscard]] Result<void> start_task(ObjectId component, const TaskStart&);

    // Request acceptance is not completion. Replay checks the original World's
    // membership/owner conditions; it never follows a target into another World.
    [[nodiscard]] Result<std::uint64_t> request(Request);
    [[nodiscard]] Result<std::vector<RequestCompletion>> flush();

private:
    friend struct detail::Access;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
