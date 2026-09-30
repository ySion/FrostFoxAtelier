#pragma once

#include "runtime/parameter.hpp"
#include <array>
#include <ranges>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <utility>
#include <vector>

namespace atelier {
template<class T> class Span;
class StringView;


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
    friend struct std::hash<TypeId>;
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
    friend struct std::hash<ObjectId>;
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

enum class TypeKind { scalar, record, object, enumeration, string, array };
enum class ValueForm { inline_value, strong_reference, weak_reference, strong_sequence };

struct ValueDescription {
    TypeId type;
    ValueForm form{ValueForm::inline_value};
    bool operator==(const ValueDescription&) const = default;
};

struct ParameterDescription : ValueDescription {
    ParameterPassing passing{ParameterPassing::value};
    ParameterDescription() = default;
    ParameterDescription(TypeId type, ValueForm form = ValueForm::inline_value, ParameterPassing mode = ParameterPassing::value)
        : ValueDescription{type, form}, passing(mode) {}
    ParameterDescription(ValueDescription value, ParameterPassing mode = ParameterPassing::value)
        : ValueDescription(value), passing(mode) {}
    bool operator==(const ParameterDescription&) const = default;
};

// Receiver access is independent of object-reference ownership and parameter passing.
struct ReceiverDescription {
    TypeId type;
    bool read_only{};
    bool operator==(const ReceiverDescription&) const = default;
};

// Owns its descriptions, not the Runtime, callable identity or executable code.
// An absent result means void; an absent receiver describes a free function.
enum class FunctionExecution { synchronous, coroutine };
struct FunctionSignature {
    std::vector<ParameterDescription> parameters;
    std::optional<ValueDescription> result;
    std::optional<ReceiverDescription> receiver;
    FunctionExecution execution{FunctionExecution::synchronous};
    bool operator==(const FunctionSignature&) const = default;
};

// Compact scalar/enum values; no boxed object/value universe.
struct EnumValue {
    TypeId type;
    std::uint32_t bits{};
    bool operator==(const EnumValue&) const = default;
};
using Scalar = std::variant<float, std::uint32_t, std::int32_t, EnumValue>;

// Validated, call-scoped storage. Invokers must not retain these addresses.
struct ArgumentView {
    TypeId type;
    const void* data{};
    void* writable{};
    template<class T> decltype(auto) get() const {
        using Value = std::remove_cvref_t<T>;
        if constexpr (std::is_lvalue_reference_v<T>) {
            if constexpr (std::is_const_v<std::remove_reference_t<T>>)
                return *static_cast<const Value*>(data);
            else return *static_cast<Value*>(writable);
        } else return Value(*static_cast<const Value*>(data));
    }
};

class Runtime;

struct CallContext {
    Runtime& runtime;
    ObjectId self;
    // Borrowed for this synchronous invocation only. Native bindings are trusted code.
    void* storage;
    std::span<const ArgumentView> arguments;
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
class TypePublication;
template<class T> inline constexpr bool borrowed_result = std::is_pointer_v<T> || std::is_reference_v<T>;
template<class T, std::size_t N> inline constexpr bool borrowed_result<std::span<T, N>> = true;
template<class C, class Traits> inline constexpr bool borrowed_result<std::basic_string_view<C, Traits>> = true;
template<class T> inline constexpr bool borrowed_result<std::reference_wrapper<T>> = true;
template<class T> inline constexpr bool borrowed_result<Span<T>> = true;
template<> inline constexpr bool borrowed_result<StringView> = true;
template<class T> struct NativeCallbackResult { static constexpr bool supported = false; };
template<class T> struct NativeCallbackResult<Result<T>> {
    static constexpr bool supported = !borrowed_result<T>;
};
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

// Inline storage for an object field, never an external root. All mutation and
// access use Runtime's registered field metadata. Records cannot contain it.
class ReferenceSequence final {
public:
    ReferenceSequence() = default;
    ~ReferenceSequence() = default;
    ReferenceSequence(const ReferenceSequence&) = delete;
    ReferenceSequence& operator=(const ReferenceSequence&) = delete;
    ReferenceSequence(ReferenceSequence&&) = delete;
    ReferenceSequence& operator=(ReferenceSequence&&) = delete;
private:
    friend class Runtime;
    std::vector<ObjectId> objects_;
};

// Owns the lookup path, not the object. Every access revalidates its identity.
// Native-slot locators require an active with_native borrow for each access.
class ReferenceField final {
public:
    ReferenceField() = default;
private:
    friend class Runtime;
    ObjectId object_;
    std::vector<FieldId> path_;
    bool native_only_{};
};

struct SequenceSplice {
    ReferenceField field;
    std::size_t first{};
    std::size_t erase_count{};
    std::span<const ObjectReference> inserted;
};
struct ReferenceWrite {
    ReferenceField field;
    ObjectReference value;
};
using ReferenceChange = std::variant<SequenceSplice, ReferenceWrite>;

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

struct ParameterDefinition : ValueDefinition {
    ParameterPassing passing{ParameterPassing::value};
    ParameterDefinition() = default;
    ParameterDefinition(TypeReference type, ValueForm form = ValueForm::inline_value, ParameterPassing mode = ParameterPassing::value)
        : ValueDefinition{std::move(type), form}, passing(mode) {}
    ParameterDefinition(ValueDefinition value, ParameterPassing mode = ParameterPassing::value)
        : ValueDefinition(std::move(value)), passing(mode) {}
};

// Value operands own their scalar. References borrow live caller-owned storage until
// return. A field operand borrows its path and resolves inside the native call.
class Argument {
    friend class Runtime;
    Scalar value_{};
    TypeReference type_;
    bool borrowed_{};
    const void* data_{};
    void* writable_{};
    ObjectId object_;
    std::span<const FieldId> path_;
public:
    Argument() = default;
    Argument(Scalar value) : value_(std::move(value)) {}
    template<class T> requires (!std::is_volatile_v<T>)
    static Argument borrow(T& value) {
        Argument result;
        result.borrowed_ = true;
        result.type_ = TypeReference::native<std::remove_const_t<T>>();
        result.data_ = std::addressof(value);
        if constexpr (!std::is_const_v<T>) result.writable_ = std::addressof(value);
        return result;
    }
    // Trusted typed storage, also used by generated code. Must be a live,
    // aligned value of the exact Runtime type; writable is null or equals data.
    static Argument borrow(TypeId type, const void* data, void* writable = nullptr) {
        Argument result;
        result.borrowed_ = true;
        result.type_ = type;
        result.data_ = data;
        result.writable_ = writable;
        return result;
    }
    static Argument field(ObjectId object, std::span<const FieldId> path) {
        Argument result;
        result.object_ = object;
        result.path_ = path;
        return result;
    }
};

// Typed scalar scratch and validated views share the invocation lifetime.
// Small calls need no allocation. Addresses cannot survive this owner.
class PreparedArguments {
    friend class Runtime;
    struct alignas(4) Slot { std::byte bytes[4]; };
    std::array<Slot, 8> inline_slots_{};
    std::array<ArgumentView, 8> inline_views_{};
    std::vector<Slot> slots_;
    std::vector<ArgumentView> views_;
    std::size_t size_{};
public:
    PreparedArguments() = default;
    PreparedArguments(const PreparedArguments&) = delete;
    PreparedArguments& operator=(const PreparedArguments&) = delete;
    [[nodiscard]] std::span<const ArgumentView> values() const noexcept {
        return {size_ > inline_views_.size() ? views_.data() : inline_views_.data(), size_};
    }
};

struct ReceiverDefinition {
    TypeReference type;
    bool read_only{};
};

struct SignatureDefinition {
    std::vector<ParameterDefinition> parameters;
    std::optional<ValueDefinition> result{};
    std::optional<ReceiverDefinition> receiver{};
    FunctionExecution execution{FunctionExecution::synchronous};
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
    // Complete native representation to its explicitly declared native base.
    void* (*base_address)(void*) noexcept{};
    // Array element representation, checked against the same registered value type.
    const void* element_representation{};
    // Trusted read-only, non-reentrant projection of a live Array/String.
    // Does not allocate, mutate, invoke user code or extend the buffer lifetime.
    void (*sequence_view)(const void*, const void**, std::size_t*) noexcept{};
    void (*sequence_mutate)(SequenceMutation, void*, const void*, std::size_t){};
    // Trusted promise: value operations may change their destination, but do
    // not invalidate other borrowed storage, retain input addresses or reenter
    // source execution. Array publication also requires this of its element.
    bool value_operations_preserve_borrows{};
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
    // Optional direct access to standard-layout native fields, supplied by
    // generated offsetof expressions. Native bindings are trusted producers.
    std::optional<std::size_t> native_offset{};
};

struct MethodDefinition {
    std::string name;
    SignatureDefinition signature;
    Invoker invoke{};
    // Includes transitive calls: no borrowed-address escape or invalidation.
    // Runtime's existing callback scope also rejects GC/lifecycle reentry.
    bool preserves_borrows{};
};

struct TypeDefinition {
    std::string name;
    TypeKind kind{TypeKind::object};
    std::vector<FieldDefinition> fields;
    std::vector<MethodDefinition> methods;
    NativeLayout native;
    // Objects default to core.Object. Records cannot declare a base.
    std::optional<TypeReference> base{};
    struct Enumeration {
        TypeReference underlying;
        std::vector<std::pair<std::string, std::uint32_t>> values;
    };
    std::optional<Enumeration> enumeration{};
    std::optional<TypeReference> element{};
};

struct FieldInfo {
    FieldId id;
    std::string name;
    ValueDescription value;
    std::optional<std::size_t> offset{};
};

struct MethodInfo {
    MethodId id;
    std::string name;
    FunctionSignature signature;
    bool preserves_borrows{};
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
    // Public member lists include ancestors, preserving declaration IDs/signatures.
    std::optional<TypeId> base{};
    struct Enumeration {
        TypeId underlying;
        std::vector<std::pair<std::string, std::uint32_t>> values;
        bool operator==(const Enumeration&) const = default;
    };
    std::optional<Enumeration> enumeration{};
    std::optional<TypeId> element{};
    bool native_storage{};
    void (*sequence_view)(const void*, const void**, std::size_t*) noexcept{};
    void (*sequence_mutate)(SequenceMutation, void*, const void*, std::size_t){};
    bool value_operations_preserve_borrows{};
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
    [[nodiscard]] std::vector<TypeId> types() const;
    [[nodiscard]] TypeId object_type() const noexcept;
    [[nodiscard]] Result<bool> is_a(TypeId actual, TypeId expected) const;
    // Preserves identity and strength. Upcasts preserve stale identities; a
    // non-null downcast requires a live compatible object. Does not create a root.
    [[nodiscard]] Result<ObjectReference> cast_reference(const ObjectReference&, TypeId target) const;
    // Resolves published types without changing registration or code publication state.
    // Scalar/enum parameters carry independent passing modes; results are values.
    // Invalid/unsupported definitions fail with invalid_definition. The returned value
    // owns its arrays; its TypeIds remain scoped to this Runtime, which it does not own.
    [[nodiscard]] Result<FunctionSignature> resolve_signature(const SignatureDefinition& definition) const;
    [[nodiscard]] Result<TypeId> find_type(std::string_view name) const;
    [[nodiscard]] Result<const TypeInfo*> describe(TypeId type) const;
    [[nodiscard]] Result<FieldId> find_field(TypeId type, std::string_view name) const;
    [[nodiscard]] Result<MethodId> find_method(TypeId type, std::string_view name) const;

    // Trusted synchronous native storage access. Callback returns Result<R>;
    // R must own its data. Capturing/returning hidden borrowed addresses is also
    // forbidden by contract. The scope blocks object lifecycle, GC and JIT entry.
    template<class T, class F>
        requires std::invocable<F, T&> && detail::NativeCallbackResult<std::invoke_result_t<F, T&>>::supported
    [[nodiscard]] std::invoke_result_t<F, T&> with_native(ObjectId object, F&& callback) {
        return with_native<T>(object, {}, std::forward<F>(callback));
    }

    // The same protected borrow can target an inline field in native or described objects.
    template<class T, class F>
        requires std::invocable<F, T&> && detail::NativeCallbackResult<std::invoke_result_t<F, T&>>::supported
    [[nodiscard]] std::invoke_result_t<F, T&> with_native(ObjectId object, std::span<const FieldId> path, F&& callback) {
        auto borrowed = borrow_native(object, detail::representation<std::remove_cv_t<T>>(), path);
        if (!borrowed) return std::unexpected(borrowed.error());
        try {
            return std::invoke(std::forward<F>(callback), *static_cast<T*>(borrowed->storage));
        } catch (const std::exception& error) {
            return std::unexpected(Error{ErrorCode::invocation_failed, error.what()});
        } catch (...) {
            return std::unexpected(Error{ErrorCode::invocation_failed, "Native borrow callback raised an unknown exception."});
        }
    }

    [[nodiscard]] Result<ReferenceField> reference_field(ObjectId, std::span<const FieldId>) const;
    [[nodiscard]] Result<ReferenceField> reference_field(ObjectId object, FieldId field) const {
        return reference_field(object, {&field, 1});
    }
    // Match the protected storage to its registered metadata, including hidden
    // fields and native base segments. Cannot relabel a slot's strong/weak form.
    [[nodiscard]] Result<ReferenceField> reference_field(ObjectId, const ReferenceSlot&) const;
    [[nodiscard]] Result<ReferenceField> reference_field(ObjectId, const ReferenceSequence&) const;
    [[nodiscard]] Result<ObjectReference> read_reference(const ReferenceField&) const;
    [[nodiscard]] Result<std::size_t> sequence_size(const ReferenceField&) const;
    [[nodiscard]] Result<ObjectReference> sequence_read(const ReferenceField&, std::size_t index) const;
    [[nodiscard]] Result<void> visit_sequence(const ReferenceField&, ReferenceVisitor) const;
    // One splice per sequence and one write per slot. All validation, input
    // copying and capacity preparation precede any logical change. Commit has
    // no allocations or callbacks; preparation may propagate allocation errors.
    [[nodiscard]] Result<void> apply_reference_changes(std::span<const ReferenceChange>);

    template<class T>
    [[nodiscard]] Result<TypeId> native_type() const {
        return find_representation(detail::representation<T>());
    }

    // Trusted caller-owned value storage; only the first TypeInfo::size bytes are used.
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
    // Mutates caller-owned live foundation storage under the same callback
    // protection as other value operations. Input is an element, same sequence,
    // or UTF-8 bytes according to operation; count is used by capacity operations.
    [[nodiscard]] Result<void> mutate_sequence(TypeId type, std::span<std::byte> storage,
        SequenceMutation operation, TypeId input_type = {}, std::span<const std::byte> input = {}, std::size_t count = 0);

    // Creation publishes an external root before returning. Roots do not prevent
    // explicit destruction or Runtime shutdown, and releasing one never destroys an object.
    [[nodiscard]] Result<ObjectRoot> create(TypeId type);
    [[nodiscard]] Result<ObjectRoot> hold(ObjectId object);
    [[nodiscard]] Result<ObjectRoot> hold(ObjectId object, TypeId declared_type);
    [[nodiscard]] Result<ObjectRoot> hold(const ObjectReference& reference);
    [[nodiscard]] Result<ObjectReference> observe(const ObjectReference& reference) const;
    [[nodiscard]] Result<ObjectReference> null_reference(TypeId type, ValueForm form = ValueForm::strong_reference) const;
    [[nodiscard]] Result<std::optional<ObjectRoot>> try_acquire(const ObjectReference& reference);
    [[nodiscard]] Result<ValueRoot> root_value(TypeId type, std::span<const std::byte> storage);
    [[nodiscard]] Result<void> destroy(ObjectId object);
    // Context provenance only: true for this Runtime's live or stale identities.
    // Null and foreign identities return false. Does not validate or root an object.
    [[nodiscard]] bool belongs_to_context(ObjectId object) const noexcept;
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
    template<std::ranges::contiguous_range R>
        requires std::same_as<std::ranges::range_value_t<R>, Argument>
    [[nodiscard]] Result<void> call(ObjectId object, MethodId method, const R& arguments, Scalar* output = nullptr) {
        return call_arguments(object, method, std::span<const Argument>{arguments}, output);
    }
    // Source adapters use this before entry. Field operands are rejected here:
    // their addresses may only be borrowed inside a protected native call.
    [[nodiscard]] Result<void> prepare_arguments(const FunctionSignature&, std::span<const Argument>, PreparedArguments&) const;
    [[nodiscard]] std::size_t live_objects() const noexcept;
    // Explicit host safe points only. Starting a cycle reserves its traversal
    // cursor before changing state. Step budgets require positive max_work.
    [[nodiscard]] Result<void> begin_collection();
    [[nodiscard]] Result<CollectionProgress> collect_step(CollectionBudget budget);
    [[nodiscard]] CollectionPhase collection_phase() const noexcept;
    // Committed-dead objects are no longer live, even before physical destruction.
    [[nodiscard]] std::size_t pending_reclamation() const noexcept;

private:
    friend class detail::TypePublication;
    Result<std::shared_ptr<detail::TypePublication>> prepare_types(std::vector<TypeDefinition>);
    [[nodiscard]] Result<void> call_arguments(ObjectId, MethodId, std::span<const Argument>, Scalar*);
    [[nodiscard]] Result<void> prepare_arguments(const FunctionSignature&, std::span<const Argument>, PreparedArguments&, bool fields) const;
    friend class ExecutionScope;
    [[nodiscard]] Result<void> enter_execution();
    void leave_execution() noexcept;
    struct Impl;
    struct Address { void* data; ValueDescription value; };
    struct NativeBorrow {
        Runtime* runtime;
        void* storage;
        NativeBorrow(Runtime&, void*) noexcept;
        NativeBorrow(NativeBorrow&&) noexcept;
        ~NativeBorrow();
        NativeBorrow(const NativeBorrow&) = delete;
    };
    std::unique_ptr<Impl> impl_;
    [[nodiscard]] Result<NativeBorrow> borrow_native(ObjectId, const void* representation, std::span<const FieldId>);
    [[nodiscard]] Result<ReferenceField> native_reference_field(ObjectId, const void*, bool sequence) const;
    [[nodiscard]] Result<Address> resolve_reference_field(const ReferenceField&) const;
    [[nodiscard]] Result<TypeId> find_representation(const void* representation) const;
    [[nodiscard]] Result<std::uint32_t> resolve_object(ObjectId object) const;
    [[nodiscard]] Result<Address> resolve_field(ObjectId object, std::span<const FieldId> path) const;
    [[nodiscard]] Result<Address> resolve_path(TypeId type, void* storage, std::span<const FieldId> path) const;
    [[nodiscard]] Result<void> validate_reference(const ObjectReference& reference) const;
    [[nodiscard]] Result<ObjectReference> read_reference_at(Address address) const;
    [[nodiscard]] Result<void> write_reference_at(Address address, const ObjectReference& reference);
    [[nodiscard]] Result<void> validate_reference_assignment(ValueDescription, const ObjectReference&) const;
    void commit_reference_write(Address, ObjectId) noexcept;
};

}

template<> struct std::hash<atelier::TypeId> {
    std::size_t operator()(atelier::TypeId id) const noexcept {
        const auto seed = std::hash<std::uint64_t>{}(id.context_);
        return seed ^ (std::hash<std::uint32_t>{}(id.index_) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    }
};
template<> struct std::hash<atelier::ObjectId> {
    std::size_t operator()(atelier::ObjectId id) const noexcept {
        auto seed = std::hash<std::uint64_t>{}(id.context_);
        seed ^= std::hash<std::uint32_t>{}(id.index_) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        return seed ^ (std::hash<std::uint64_t>{}(id.generation_) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    }
};
