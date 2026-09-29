#pragma once

#include "world/world.hpp"
#include <unordered_map>

namespace atelier::world::detail {

struct WorldLifetime { bool open{true}; };
struct ActorData { World* world{}; bool enabled{}; bool transitioning{}; };
struct ComponentData { ObjectId owner; bool enabled{}; };
struct FlagScope {
    bool& flag;
    explicit FlagScope(bool& value) : flag(value) { flag = true; }
    ~FlagScope() { flag = false; }
};
struct DispatchScope {
    std::size_t& depth;
    explicit DispatchScope(std::size_t& value) : depth(value) { ++depth; }
    ~DispatchScope() { --depth; }
};

struct Access {
    using State = World::Impl;
    static State& state(World& world) noexcept { return *world.impl_; }
    static const State& state(const World& world) noexcept { return *world.impl_; }
    static ReferenceSequence& sequence(Actor& value) noexcept { return value.components_; }
    static ReferenceSlot& owner(Component& value) noexcept { return value.owner_; }
    static ReferenceSlot& transform(Cube& value) noexcept { return value.transform_; }
    static World*& member_world(Actor& value) noexcept { return value.member_world_; }
    static bool& enabled(Actor& value) noexcept { return value.enabled_; }
    static bool& enabled(Component& value) noexcept { return value.enabled_; }
    static bool& transitioning(Actor& value) noexcept { return value.transitioning_; }
};

inline std::unexpected<Error> error(ErrorCode code, std::string message) { return std::unexpected(Error{code, std::move(message)}); }
Result<ActorData> actor_data(Runtime&, ObjectId);
Result<ComponentData> component_data(Runtime&, ObjectId);
Result<ObjectId> cube_transform(Runtime&, ObjectId);
Result<bool> is(Runtime&, ObjectId, TypeId);
Result<void> validate_object(Runtime&, ObjectId, TypeId);

}

namespace atelier::world {

struct World::Impl {
    struct Position { std::size_t index{}, presentations{}; };
    struct Bucket { TypeId type; bool cube{}, counter{}, transform{}; std::vector<ObjectId> objects; };
    struct Task { ObjectId component, actor; Coroutine coroutine; };
    struct Pending { std::uint64_t serial; Request request; ObjectId owner; bool member_required{}; };

    Runtime& runtime;
    Types types;
    World& world;
    std::shared_ptr<detail::WorldLifetime> lifetime{std::make_shared<detail::WorldLifetime>()};
    bool running{}, closing{}, closed{}, transitioning{}, flushing{};
    std::size_t dispatch_depth{};
    std::vector<Member> members;
    std::unordered_map<ObjectId, Position> positions;
    std::vector<Bucket> buckets;
    std::vector<ObjectId> updates, presentation;
    std::vector<Task> tasks;
    std::vector<Pending> pending;
    std::uint64_t next_request{1};
    CounterUpdate counter_update;
    LifecycleObserver observer;

    Impl(Runtime& r, const Types& t, World& w) : runtime(r), types(t), world(w) {}
    Result<void> admit(bool mutation = true) const;
    Result<std::size_t> member(ObjectId) const;
    Result<ObjectId> component_owner(ObjectId) const;
    Result<std::size_t> prepare_bucket(TypeId, std::size_t additional);
    void prepare_activity(std::size_t additional = 0);
    Result<void> refresh();
    void cancel_tasks(ObjectId target = {}, bool actor = true) noexcept;
    void halt() noexcept;
    std::optional<Error> notify(Lifecycle, ObjectId);
    Commit finish(bool changed, std::optional<Error> failure = {});
    void erase_indexes(ObjectId component) noexcept;
};

}
