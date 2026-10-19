#pragma once

#include "world/world.hpp"
#include "foundation/map.hpp"
#include "foundation/set.hpp"
#include <cstring>
#include <deque>
#include <optional>
#include <type_traits>
#include <unordered_map>

namespace atelier::world::detail {

struct WorldLifetime { bool open{true}; };
struct EntityData { World* world{}; bool enabled{}; bool transitioning{}; };
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
    static ReferenceSequence& sequence(Entity& value) noexcept { return value.components_; }
    static ReferenceSlot& owner(Component& value) noexcept { return value.owner_; }
    static ReferenceSequence& children(Entity& value) noexcept { return value.children_; }
    static ReferenceSlot& parent(Entity& value) noexcept { return value.parent_; }
    static ReferenceSlot& origin(Entity& value) noexcept { return value.origin_; }
    static bool& spatial(Entity& value) noexcept { return value.spatial_; }
    static bool& explicit_origin(Entity& value) noexcept { return value.explicit_origin_; }
    static bool& world_origin(Entity& value) noexcept { return value.world_origin_; }
    static geometry::MeshRef& geometry(StaticMeshComponent& value) noexcept { return value.geometry_; }
    static const geometry::MeshRef& geometry(const StaticMeshComponent& value) noexcept { return value.geometry_; }
    static String& mesh_source(StaticMeshComponent& value) noexcept { return value.mesh_source_; }
    static const String& mesh_source(const StaticMeshComponent& value) noexcept { return value.mesh_source_; }
    static ReferenceSequence& materials(StaticMeshComponent& value) noexcept {return value.materials_;}
    static std::vector<std::string>& material_slots(StaticMeshComponent& value) noexcept {return value.material_slots_;}
    static std::vector<bool>& material_overrides(StaticMeshComponent& value) noexcept {return value.material_overrides_;}
    static auto& surfaces(StaticMeshComponent& value) noexcept {return value.surfaces_;}
    static auto& surface_geometry(StaticMeshComponent& value) noexcept {return value.surface_geometry_;}
    static auto& surface_parameters(StaticMeshComponent& value) noexcept {return value.surface_parameters_;}
    static World*& member_world(Entity& value) noexcept { return value.member_world_; }
    static bool& enabled(Entity& value) noexcept { return value.enabled_; }
    static bool& enabled(Component& value) noexcept { return value.enabled_; }
    static bool& transitioning(Entity& value) noexcept { return value.transitioning_; }
};

inline std::unexpected<Error> error(ErrorCode code, std::string message) { return std::unexpected(Error{code, std::move(message)}); }
Result<EntityData> entity_data(Runtime&, ObjectId);
Result<ComponentData> component_data(Runtime&, ObjectId);
Result<void> validate_tree(Runtime&, ObjectId entity, std::span<const ObjectRoot>);
Result<std::vector<ObjectRoot>> subtree(Runtime&, ObjectId entity);
Result<void> clear_component(Runtime&, ObjectId entity, ObjectId component, std::size_t index);
Result<void> unlink_parent(Runtime&, ObjectId entity);
Result<void> requires_space(Runtime&, ObjectId entity, std::span<const ObjectRoot>);
Result<bool> is(Runtime&, ObjectId, TypeId);
Result<void> validate_object(Runtime&, ObjectId, TypeId);
// Validators registered with LightSettings and BodySettings.
Result<void> light_rules(const LightSettings&);
Result<void> body_rules(const BodySettings&);
// Checks a native value against its reflected type's hard bounds and validators.
template<class T> Result<void> check_value(Runtime& runtime, const T& value) {
    auto type = runtime.native_type<T>();
    if (!type) return std::unexpected(type.error());
    auto failure = runtime.validate_value(*type, std::as_bytes(std::span{&value, 1}));
    if (!failure) return std::unexpected(failure.error());
    if (*failure) return std::unexpected((*failure)->error);
    return {};
}
// Values that passed validation, by bucket position. Validators are pure checks
// of a value's bytes and published types never change, so the same bytes pass
// again; any other value at that position is validated again.
template<class T> class PassedValues {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<std::optional<T>> values_;
public:
    void resize(std::size_t size) { values_.resize(size); }
    [[nodiscard]] bool contains(std::size_t at, const T& value) const noexcept {
        return values_[at] && std::memcmp(&*values_[at], &value, sizeof(T)) == 0;
    }
    void insert(std::size_t at, const T& value) noexcept { std::memcpy(&values_[at].emplace(), &value, sizeof(T)); }
};
// world::material_surfaces inside the caller's borrow of the mesh component.
Result<std::shared_ptr<const std::vector<material::SurfaceRef>>> material_surfaces(Runtime&, ObjectId, StaticMeshComponent&);
// One Sky Light per World, disabled components included.
World::Admission unique_sky_light(CapabilityId lights);
// A light other than Sky needs a Transform on its member owner.
World::Admission spatial_lights();
Result<void> queue_translation(Runtime&, ObjectId component, math::Vector3);
Result<void> queue_request(Runtime&, ObjectId object, RequestKind, bool enabled);

// A short-lived evaluation snapshot for candidate graphs and Detached objects.
// No cached TRS is an authoring source.
struct SpaceNode {
    ObjectId parent{};
    std::optional<ObjectId> origin;
    bool spatial{};
    Trs local;
    World* world{};
};
struct SpaceGraph {
    Runtime& runtime;
    std::unordered_map<ObjectId, SpaceNode> nodes;
    std::unordered_map<ObjectId, math::Matrix4> matrices;
    // Reused only inside this evaluation phase; no author data survives it.
    std::vector<ObjectId> matrix_path;
    Set<ObjectId> matrix_seen, parent_seen;
    explicit SpaceGraph(Runtime& r) : runtime(r) {}
    Result<SpaceNode*> node(ObjectId);
    Result<ObjectId> parent(ObjectId);
    Result<math::Matrix4> matrix(ObjectId);
    void invalidate() { matrices.clear(); }
};

// One World's member spatial evaluation, shared by every consumer. Relations
// and Transform presence change only through World operations, which clear the
// cache. TRS values may also change through Runtime field writes or generated
// code, so each pass re-reads the TRS of every Entity it visits and recomposes
// only a chain whose own TRS or parent matrix changed.
struct SpaceCache {
    struct Entry {
        Trs local;                 // As read in pass `checked`.
        ObjectId parent;           // Resolved spatial parent; null is world space.
        std::uint64_t checked{}, version{}, parent_version{};
        math::Matrix4 world{};
        bool spatial{}, composed{};
        // A zero scale axis on this node or an ancestor; FP32 rounding can leave
        // the composed matrix of such a space with a tiny nonzero determinant.
        bool flat{};
        // Pass in which this chain failed to compose at `cause`, whose error is in `failures`.
        std::uint64_t failed{};
        ObjectId cause;
    };
    Map<ObjectId, Entry> entries;
    Map<ObjectId, Error> failures;
    SpaceGraph relations;
    std::vector<ObjectId> path;
    std::uint64_t pass{1};         // Above every fresh entry's `checked` and `failed`.
    ObjectId failed;               // The Entity whose relation or TRS failed the latest matrix().
    explicit SpaceCache(Runtime& runtime) : relations(runtime) {}
    void clear() noexcept;
    void begin_pass() noexcept { ++pass; failures.clear(); }
    Result<math::Matrix4> matrix(ObjectId);
    Result<ObjectId> parent(ObjectId);
    // The visited chain of the latest matrix() call in this pass.
    [[nodiscard]] const Entry* entry(ObjectId id) const noexcept { return entries.find(id); }
    // The topmost node with a zero scale axis on that chain, else `object`.
    [[nodiscard]] ObjectId degenerate_root(ObjectId object) const noexcept;
private:
    Result<Entry*> resolve(ObjectId);
};

}

namespace atelier::world {

struct World::Impl {
    struct Task { ObjectId object, entity; Coroutine coroutine; };
    struct Pending { std::uint64_t serial; Request request; ObjectId owner; bool member_required{}; };
    struct Scheduled { ObjectId object, entity; const Behavior* behavior{}; };
    // An isolated behavior failure. Begin-faulted objects never began; Tick-
    // faulted objects began and still receive End. Cleared when the object
    // leaves running participation or by clear_faults().
    enum class Fault : std::uint8_t { none, begin, tick };
    struct ComponentState {
        ObjectId id;
        TypeId type;
        const Behavior* behavior{};
        std::uint32_t capabilities{}; // Bit i: in the bucket of registrations[i].
        bool enabled{}, begun{};
        Fault fault{};
    };
    // Execution and registration state of one member, parallel to `members`.
    // Derived from Runtime when the Entity enters or its own composition changes.
    struct MemberState {
        TypeId type;
        const Behavior* behavior{};
        ObjectId parent;
        ObjectId origin;            // Explicit non-world spatial origin, if any.
        std::size_t size{1};        // Members in its composition subtree, itself included.
        bool enabled{}, eligible{}, begun{};
        Fault fault{};
        std::vector<ComponentState> components;
    };
    struct Bucket { TypeId type; std::vector<ObjectId> objects; };

    rendering::SceneLightingSettings lighting;
    Runtime& runtime;
    Types types;
    World& world;
    std::shared_ptr<detail::WorldLifetime> lifetime{std::make_shared<detail::WorldLifetime>()};
    bool running{}, closing{}, closed{}, transitioning{}, flushing{}, updating{}, relocating{};
    std::size_t dispatch_depth{};
    // Composition preorder: every member's subtree is one contiguous range.
    std::vector<Member> members;
    std::vector<MemberState> states;
    Map<ObjectId, std::size_t> positions;
    // Derived lazily from `states`.
    bool schedule_dirty{true}, buckets_dirty{true};
    std::array<std::vector<Scheduled>, 3> updates; // Indexed by TickPhase.
    std::vector<Bucket> buckets;
    struct Registration { TypeId type; CapabilityPredicate predicate; };
    std::vector<Registration> registrations;
    std::vector<std::vector<CapabilityMember>> capable; // Rebuilt with `buckets`.
    // The world library's own registrations; owned by presentation code.
    CapabilityId meshes, lights;
    // Aligned with the meshes and lights buckets at each extraction call.
    detail::PassedValues<Material> valid_materials;
    detail::PassedValues<LightSettings> valid_lights;
    static constexpr std::uint32_t bit(CapabilityId id) noexcept { return 1u << id.value; }
    std::uint32_t presented() const noexcept { return bit(meshes) | bit(lights); }
    Result<std::uint32_t> capabilities_of(ObjectId component, TypeId type) const;
    // Tick prerequisite edges, both directions; only between current participants.
    std::unordered_map<ObjectId, std::vector<ObjectId>> prerequisites, dependents;
    void forget_prerequisites(ObjectId) noexcept;
    struct SimulationEntry { std::uint64_t id; std::shared_ptr<const Simulation> simulation; };
    std::vector<SimulationEntry> simulations;
    std::uint64_t next_simulation{1};
    detail::SpaceCache space;
    std::vector<Task> tasks;
    struct Translation { ObjectId component, entity; math::Vector3 delta; };
    std::vector<Translation> translations;
    struct TranslationChange { ObjectId entity; Trs value; };
    std::vector<TranslationChange> translation_changes;
    Result<void> apply_translations();
    Result<void> apply_poses(std::span<const SimulationPose>, DiagnosticPhase);
    std::vector<Pending> pending;
    std::uint64_t next_request{1};
    struct Binding { TypeId type; Behavior behavior; };
    std::deque<Binding> behaviors;
    Result<const Behavior*> behavior(TypeId) const;
    LifecycleObserver observer;
    struct Subscription { std::uint64_t id; std::shared_ptr<LifecycleObserver> observer; };
    std::vector<Subscription> subscriptions;
    std::uint64_t next_subscription{1};

    FailurePolicy policy{FailurePolicy::isolate};
    static constexpr std::size_t diagnostic_capacity = 1024;
    std::vector<Diagnostic> diagnostics;
    std::uint64_t next_diagnostic{1}, dropped_diagnostics{};
    // A consumer failure reported again in the next pass of its phase extends
    // one entry; a pass without it resolves it. Presentation and lighting
    // passes are extraction calls; physics and command passes are updates.
    struct DiagnosticKey {
        ObjectId object; DiagnosticKind kind; DiagnosticPhase phase; std::string field;
        bool operator==(const DiagnosticKey&) const = default;
    };
    struct DiagnosticKeyHash { std::size_t operator()(const DiagnosticKey&) const noexcept; };
    struct Recent { std::uint64_t serial, pass; };
    std::unordered_map<DiagnosticKey, Recent, DiagnosticKeyHash> recent;
    std::array<std::uint64_t, std::size_t(DiagnosticPhase::lifecycle) + 1> passes{};
    void begin_pass(DiagnosticPhase) noexcept;
    void report(Diagnostic);
    // Reports an object failure. True when it stays with that object; false
    // means the World must stop and surface `error` (halt policy or a World-
    // level failure, which is not reported as an object diagnostic).
    bool contain(const Error&, Diagnostic);
    bool contain(const Error&, DiagnosticKind, DiagnosticPhase, ObjectId object, ObjectId entity,
        std::vector<std::string> field = {}, Severity = Severity::error);
    // A broken World index is a World-level failure; once seen, nothing is isolated.
    mutable bool invariant_failed{};
    Error inconsistent(const char* message) const {
        invariant_failed = true;
        return Error{ErrorCode::invalid_member, message};
    }
    Fault* fault_of(ObjectId object, ObjectId entity) noexcept;
    // Stops a begun object's Tick: cancels its tasks and drops its pending translations.
    void fault_tick(ObjectId object, ObjectId entity) noexcept;

    Impl(Runtime& r, const Types& t, World& w) : runtime(r), types(t), world(w), space(r) {}
    Result<void> admit(bool mutation = true) const;
    Result<void> admit_edit(ObjectId) const;
    // Tells simulations that a transform edit of a running World moved their Entities.
    Result<void> relocate(ObjectId edited);
    Result<std::size_t> member(ObjectId) const;
    Result<ObjectId> component_owner(ObjectId) const;
    // Admits same-context detached composition as well as this World's members.
    Result<detail::EntityData> composition_entity(ObjectId) const;
    Result<detail::ComponentData> composition_component(ObjectId) const;

    // Member index maintenance. Callers keep `members`, `states` and
    // `positions` consistent before any callback can observe them.
    // Invalidates derived schedules/directories; `spatial` also drops cached
    // relations and matrices.
    void changed(bool spatial = true) noexcept;
    Result<std::vector<ComponentState>> derive_components(ObjectId entity) const;
    Result<MemberState> derive(ObjectId entity, ObjectId parent, ObjectId origin) const;
    Result<void> rederive(ObjectId entity);
    void reindex(std::size_t first, std::size_t last) noexcept;
    void resize_ancestors(ObjectId parent, std::ptrdiff_t delta) noexcept;
    void refresh_eligibility(std::size_t first, std::size_t count) noexcept;
    MemberState* state_of(ObjectId) noexcept;
    const MemberState* state_of(ObjectId) const noexcept;
    ComponentState* component_state(ObjectId entity, ObjectId component) noexcept;
    void rebuild_schedule();
    void rebuild_buckets();

    struct AdmissionEntry { TypeId type; Admission admission; };
    std::vector<AdmissionEntry> admissions;
    // Runs each admission on the candidates of its type; empty selections are skipped.
    Result<void> check_admissions(std::span<const ObjectId> candidates);
    Result<void> check_admissions(std::span<const ObjectRoot> candidates);
    // Whether an edit of this member component runs admissions; detached composition does not.
    Result<bool> admitted(ObjectId component) const;
    // Shared by World::write: write, then type rules and admissions; `restore` undoes a rejected edit.
    template<class Write, class Restore> Result<void> checked_write(ObjectId, bool entity, Write&&, Restore&&);
    Result<void> check_edit(ObjectId component);
    void cancel_tasks(ObjectId target = {}, bool entity = true) noexcept;
    void halt() noexcept;
    // Begin visits [first,last) members; End reverses it. A non-null component
    // limits the visit to that component of the range.
    std::optional<Error> begin(std::size_t first, std::size_t last, ObjectId component = {});
    std::optional<Error> end(std::size_t first, std::size_t last, ObjectId component = {});
    std::optional<Error> begin() { return begin(0, members.size()); }
    std::optional<Error> end() { return end(0, members.size()); }
    std::optional<Error> invoke(const Scheduled&, bool beginning);
    // One phase of update: Tick, tasks (pre_physics only), commands, requests.
    std::optional<Error> run_phase(TickPhase, float amount);
    // Applies queued requests; object failures are contained, others returned.
    std::optional<Error> flush_requests();
    Result<Commit> apply_request(const Pending&);
    std::optional<Error> shutdown();
    std::optional<Error> notify(Lifecycle, ObjectId);
    Commit finish(bool changed, std::optional<Error> failure = {});
};

}
