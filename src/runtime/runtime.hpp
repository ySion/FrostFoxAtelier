#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <utility>
#include <vector>

namespace atelier {

enum class ErrorCode {
    invalid_type, invalid_definition, duplicate_name, invalid_object,
    invalid_member, type_mismatch, argument_mismatch, construction_failed,
    invocation_failed, busy, capacity_exceeded, invalid_storage,
    unsupported_operation, assignment_failed
};

struct Error {
    ErrorCode code;
    std::string message;
};

template<class T>
using Result = std::expected<T, Error>;

// These identities belong to one Runtime. They are not serialization IDs.
class TypeId {
public:
    TypeId() = default;
    bool operator==(const TypeId&) const = default;
private:
    friend class Runtime;
    TypeId(std::uint64_t context, std::uint32_t index) : context_(context), index_(index) {}
    std::uint64_t context_{};
    std::uint32_t index_{};
};

class ObjectId {
public:
    ObjectId() = default;
    bool operator==(const ObjectId&) const = default;
private:
    friend class Runtime;
    ObjectId(std::uint64_t context, std::uint32_t index, std::uint64_t generation)
        : context_(context), index_(index), generation_(generation) {}
    std::uint64_t context_{};
    std::uint32_t index_{};
    std::uint64_t generation_{};
};

class FieldId {
public:
    FieldId() = default;
    bool operator==(const FieldId&) const = default;
private:
    friend class Runtime;
    FieldId(TypeId owner, std::uint32_t index) : owner_(owner), index_(index) {}
    TypeId owner_;
    std::uint32_t index_{};
};

class MethodId {
public:
    MethodId() = default;
    bool operator==(const MethodId&) const = default;
private:
    friend class Runtime;
    MethodId(TypeId owner, std::uint32_t index) : owner_(owner), index_(index) {}
    TypeId owner_;
    std::uint32_t index_{};
};

enum class TypeKind { scalar, record, object };
enum class ValueForm { inline_value, strong_reference, weak_reference };

struct ValueDescription {
    TypeId type;
    ValueForm form{ValueForm::inline_value};
    bool operator==(const ValueDescription&) const = default;
};

// Receiver access is independent of object-reference ownership and parameter passing.
struct ReceiverDescription {
    TypeId type;
    bool read_only{};
    bool operator==(const ReceiverDescription&) const = default;
};

// Owns its descriptions, not the Runtime, callable identity or executable code.
// An absent result means void; an absent receiver describes a free function.
struct FunctionSignature {
    std::vector<ValueDescription> parameters;
    std::optional<ValueDescription> result;
    std::optional<ReceiverDescription> receiver;
    bool operator==(const FunctionSignature&) const = default;
};

// Only the scalar values needed by the first slice; no boxed object/value universe.
using Scalar = std::variant<float, std::uint32_t>;

class Runtime;

struct CallContext {
    Runtime& runtime;
    ObjectId self;
    // Borrowed for this synchronous invocation only. Native bindings are trusted code.
    void* storage;
    std::span<const Scalar> arguments;
    // Staged business value. Runtime commits it only after successful validation.
    std::optional<Scalar> result{};
};

using Invoker = Result<void> (*)(CallContext&);

namespace detail {
// Local C++ representation check, never a persistent or language type identity.
template<class T>
inline const char representation_token{};
template<class T>
constexpr const void* representation() noexcept { return &representation_token<T>; }
struct RootState;
}

// A typed reference value, not an external root. Copies preserve stale identities.
class ObjectReference {
public:
    ObjectReference() = default;
    [[nodiscard]] ValueDescription description() const noexcept { return description_; }
    [[nodiscard]] ObjectId id() const noexcept { return object_; }
    [[nodiscard]] bool is_null() const noexcept { return object_ == ObjectId{}; }
    bool operator==(const ObjectReference&) const = default;
private:
    friend class Runtime;
    ObjectReference(ValueDescription description, ObjectId object) : description_(description), object_(object) {}
    ValueDescription description_{TypeId{}, ValueForm::strong_reference};
    ObjectId object_;
};

// Compact field storage. Native reference mutations go through Runtime; whole
// native record copies go through its value operations, not ad-hoc slot assignment.
class ReferenceSlot {
public:
    ReferenceSlot() = default;
    [[nodiscard]] ObjectId id() const noexcept { return object_; }
private:
    friend class Runtime;
    ObjectId object_;
};

class ObjectRoot {
public:
    ObjectRoot() = default;
    ~ObjectRoot();
    ObjectRoot(const ObjectRoot& other);
    ObjectRoot& operator=(const ObjectRoot& other);
    ObjectRoot(ObjectRoot&& other) noexcept;
    ObjectRoot& operator=(ObjectRoot&& other) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool registered() const noexcept;
    [[nodiscard]] ObjectReference reference() const noexcept;
    [[nodiscard]] ObjectId id() const noexcept { return reference().id(); }
private:
    friend class Runtime;
    ObjectRoot(std::shared_ptr<detail::RootState> state, std::uint32_t index) noexcept;
    std::shared_ptr<detail::RootState> state_;
    std::uint32_t index_{};
};

// Registers a live caller-owned value, not its storage ownership. Reset this
// registration before destroying or freeing the value. Moving the handle is safe.
class ValueRoot {
public:
    ValueRoot() = default;
    ~ValueRoot();
    ValueRoot(const ValueRoot&) = delete;
    ValueRoot& operator=(const ValueRoot&) = delete;
    ValueRoot(ValueRoot&& other) noexcept;
    ValueRoot& operator=(ValueRoot&& other) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool registered() const noexcept;
private:
    friend class Runtime;
    ValueRoot(std::shared_ptr<detail::RootState> state, std::uint32_t index) noexcept;
    std::shared_ptr<detail::RootState> state_;
    std::uint32_t index_{};
};

struct ReferenceVisitor {
    void* context{};
    Result<void> (*visit)(void*, const ObjectReference&){};
};

// Definition-time lookup only. Names are exact, fully qualified declaration names;
// publication resolves these selectors to the Runtime's ordinary TypeId identities.
class TypeReference {
public:
    TypeReference() = default;
    TypeReference(TypeId type) : target_(type) {}
    TypeReference(std::string name) : target_(std::move(name)) {}
    template<class T>
    static TypeReference native() { return TypeReference{detail::representation<T>(), std::in_place}; }
private:
    friend class Runtime;
    TypeReference(const void* representation, std::in_place_t) : target_(representation) {}
    std::variant<TypeId, std::string, const void*> target_;
};

struct ValueDefinition {
    TypeReference type;
    ValueForm form{ValueForm::inline_value};
};

struct ReceiverDefinition {
    TypeReference type;
    bool read_only{};
};

struct SignatureDefinition {
    std::vector<ValueDefinition> parameters;
    std::optional<ValueDefinition> result{};
    std::optional<ReceiverDefinition> receiver{};
};

struct NativeLayout {
    std::size_t size{};
    std::size_t alignment{};
    const void* representation{};
    void (*construct)(void*){};
    void (*destroy)(void*) noexcept{};
    // Optional for records. Construction unwinds on failure; assignment preserves
    // a destructible value and its basic invariants, but need not roll back.
    void (*copy_construct)(void*, const void*){};
    void (*copy_assign)(void*, const void*){};
};

struct FieldDefinition {
    std::string name;
    ValueDefinition value;
    // Empty for runtime-computed storage; generated from member pointers for C++.
    // Runs under callback lifetime protection. Revoking a root is allowed, but
    // borrowed storage must remain live until the current synchronous call returns.
    void* (*native_address)(void*) noexcept{};
    const void* native_representation{};
    // Hidden fields participate in layout and tracing, but have no public FieldId.
    bool exposed{true};
};

struct MethodDefinition {
    std::string name;
    SignatureDefinition signature;
    Invoker invoke{};
};

struct TypeDefinition {
    std::string name;
    TypeKind kind{TypeKind::object};
    std::vector<FieldDefinition> fields;
    std::vector<MethodDefinition> methods;
    NativeLayout native;
};

struct FieldInfo {
    FieldId id;
    std::string name;
    ValueDescription value;
};

struct MethodInfo {
    MethodId id;
    std::string name;
    FunctionSignature signature;
};

struct TypeInfo {
    TypeId id;
    std::string name;
    TypeKind kind;
    std::size_t size;
    std::size_t alignment;
    std::vector<FieldInfo> fields;
    std::vector<MethodInfo> methods;
    bool copy_constructible{};
    bool copy_assignable{};
    // Includes declared hidden fields and references inside inline records.
    bool contains_references{};
};

enum class CollectionPhase { idle, roots, mark, sweep };

struct CollectionBudget {
    std::size_t max_work;
    // Zero disables the time limit. A positive limit is soft: one work unit,
    // including a native destructor, always finishes before the next check.
    std::chrono::nanoseconds max_time{};
};

struct CollectionProgress {
    CollectionPhase phase{CollectionPhase::idle};
    std::size_t work_done{};
    std::size_t reclaimed{};
    std::size_t pending_reclamation{};
    std::chrono::nanoseconds elapsed{};
};

// Single-threaded, explicitly owned context. Definitions are immutable after publication.
// Registration closes on first successful object creation. Callbacks cannot create/destroy objects.
class Runtime final {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] Result<TypeId> register_type(TypeDefinition definition);
    // Resolve the entire batch, including forward references, before publishing anything.
    // Results follow input order. Empty batches are invalid. No provisional IDs escape.
    [[nodiscard]] Result<std::vector<TypeId>> register_types(std::vector<TypeDefinition> definitions);
    // Resolves published types without changing registration or code publication state.
    // This slice accepts inline scalar parameters/results and an optional object receiver.
    // Invalid/unsupported definitions fail with invalid_definition. The returned value
    // owns its arrays; its TypeIds remain scoped to this Runtime, which it does not own.
    [[nodiscard]] Result<FunctionSignature> resolve_signature(const SignatureDefinition& definition) const;
    [[nodiscard]] Result<TypeId> find_type(std::string_view name) const;
    [[nodiscard]] Result<const TypeInfo*> describe(TypeId type) const;
    [[nodiscard]] Result<FieldId> find_field(TypeId type, std::string_view name) const;
    [[nodiscard]] Result<MethodId> find_method(TypeId type, std::string_view name) const;

    template<class T>
    [[nodiscard]] Result<TypeId> native_type() const {
        return find_representation(detail::representation<T>());
    }

    // Trusted caller-owned scalar/record storage; only the first TypeInfo::size bytes are used.
    // Sources must be live values of this type. Initialize/copy destinations must be unconstructed;
    // assign/destroy destinations must be live. The caller owns storage and lifetime tracking,
    // and must destroy values before this Runtime ends. These calls do not validate object handles.
    // Failed construction leaves no live destination; failed assignment may partially update it.
    // Copy construction forbids overlap. Supported assignment permits exact self-assignment
    // as a no-op, but rejects partial overlap. Invalid requests do not enter value operations.
    [[nodiscard]] Result<void> initialize_value(TypeId type, std::span<std::byte> destination);
    [[nodiscard]] Result<void> copy_construct_value(TypeId type, std::span<std::byte> destination,
                                                  std::span<const std::byte> source);
    [[nodiscard]] Result<void> assign_value(TypeId type, std::span<std::byte> destination,
                                          std::span<const std::byte> source);
    [[nodiscard]] Result<void> destroy_value(TypeId type, std::span<std::byte> storage);

    // Creation publishes an external root before returning. Roots do not prevent
    // explicit destruction or Runtime shutdown, and releasing one never destroys an object.
    [[nodiscard]] Result<ObjectRoot> create(TypeId type);
    [[nodiscard]] Result<ObjectRoot> hold(ObjectId object);
    [[nodiscard]] Result<ObjectRoot> hold(const ObjectReference& reference);
    [[nodiscard]] Result<ObjectReference> observe(const ObjectReference& reference) const;
    [[nodiscard]] Result<ObjectReference> null_reference(TypeId type, ValueForm form = ValueForm::strong_reference) const;
    [[nodiscard]] Result<std::optional<ObjectRoot>> try_acquire(const ObjectReference& reference);
    [[nodiscard]] Result<ValueRoot> root_value(TypeId type, std::span<const std::byte> storage);
    [[nodiscard]] Result<void> destroy(ObjectId object);
    [[nodiscard]] Result<TypeId> type_of(ObjectId object) const;

    // A field path traverses inline records. Its final field must be a scalar.
    [[nodiscard]] Result<Scalar> read(ObjectId object, std::span<const FieldId> path) const;
    [[nodiscard]] Result<void> write(ObjectId object, std::span<const FieldId> path, Scalar value);
    [[nodiscard]] Result<Scalar> read(ObjectId object, FieldId field) const {
        return read(object, {&field, 1});
    }
    [[nodiscard]] Result<void> write(ObjectId object, FieldId field, Scalar value) {
        return write(object, {&field, 1}, value);
    }

    [[nodiscard]] Result<ObjectReference> read_reference(ObjectId object, std::span<const FieldId> path) const;
    [[nodiscard]] Result<void> write_reference(ObjectId object, std::span<const FieldId> path, const ObjectReference& reference);
    [[nodiscard]] Result<ObjectReference> read_reference(ObjectId object, FieldId field) const {
        return read_reference(object, {&field, 1});
    }
    [[nodiscard]] Result<void> write_reference(ObjectId object, FieldId field, const ObjectReference& reference) {
        return write_reference(object, {&field, 1}, reference);
    }
    [[nodiscard]] Result<ObjectReference> read_reference(TypeId type, std::span<const std::byte> storage,
                                                        std::span<const FieldId> path) const;
    [[nodiscard]] Result<void> write_reference(TypeId type, std::span<std::byte> storage,
                                              std::span<const FieldId> path, const ObjectReference& reference);
    // Trusted native slots: the caller supplies the slot's declared description
    // and keeps its containing value alive, including non-exposed native fields.
    [[nodiscard]] Result<ObjectReference> read_reference(ValueDescription value, const ReferenceSlot& storage) const;
    [[nodiscard]] Result<void> write_reference(ValueDescription value, ReferenceSlot& storage, const ObjectReference& reference);
    // Enumerates non-null edges, including weak and stale references. Inline
    // records are traversed; object references are not. Visitors must keep borrowed value
    // storage live. Runtime mutations and new roots are rejected during traversal.
    [[nodiscard]] Result<void> visit_roots(ReferenceVisitor visitor) const;
    [[nodiscard]] Result<void> visit_references(ObjectId object, ReferenceVisitor visitor) const;
    [[nodiscard]] Result<void> visit_references(TypeId type, std::span<const std::byte> storage,
                                              ReferenceVisitor visitor) const;
    [[nodiscard]] std::size_t root_count() const noexcept;
    // Read-only admission for generated execution, before roots or side effects.
    // Rejects active native borrows/traversal, not a paused GC cycle.
    [[nodiscard]] Result<void> check_execution_entry() const;
    // Publication needs a quiescent Runtime, not completion of a paused GC cycle.
    [[nodiscard]] Result<void> check_code_publication() const;
    // Checks every parameter and required result storage before entering user code.
    // Non-void methods require output; void permits null and never writes output.
    // Failure preserves output, including a callback's missing/wrong/fabricated result.
    // User-code side effects are not rolled back. Native borrows still forbid source entry.
    [[nodiscard]] Result<void> call(ObjectId object, MethodId method, std::span<const Scalar> arguments,
                                    Scalar* output = nullptr);
    [[nodiscard]] std::size_t live_objects() const noexcept;
    // Explicit host safe points only. Starting a cycle reserves its traversal
    // cursor before changing state. Step budgets require positive max_work.
    [[nodiscard]] Result<void> begin_collection();
    [[nodiscard]] Result<CollectionProgress> collect_step(CollectionBudget budget);
    [[nodiscard]] CollectionPhase collection_phase() const noexcept;
    // Committed-dead objects are no longer live, even before physical destruction.
    [[nodiscard]] std::size_t pending_reclamation() const noexcept;

private:
    friend class ExecutionScope;
    [[nodiscard]] Result<void> enter_execution();
    void leave_execution() noexcept;
    struct Impl;
    struct Address { void* data; ValueDescription value; };
    std::unique_ptr<Impl> impl_;
    [[nodiscard]] Result<TypeId> find_representation(const void* representation) const;
    [[nodiscard]] Result<std::uint32_t> resolve_object(ObjectId object) const;
    [[nodiscard]] Result<Address> resolve_field(ObjectId object, std::span<const FieldId> path) const;
    [[nodiscard]] Result<Address> resolve_path(TypeId type, void* storage, std::span<const FieldId> path) const;
    [[nodiscard]] Result<void> validate_reference(const ObjectReference& reference) const;
    [[nodiscard]] Result<ObjectReference> read_reference_at(Address address) const;
    [[nodiscard]] Result<void> write_reference_at(Address address, const ObjectReference& reference);
};

}
