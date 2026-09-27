#include "runtime/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace atelier {
namespace {

constexpr auto no_slot = std::numeric_limits<std::uint32_t>::max();
std::atomic<std::uint64_t> next_context{1};

auto failure(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

struct CallbackScope {
    std::size_t& depth;
    explicit CallbackScope(std::size_t& count) : depth(count) { ++depth; }
    ~CallbackScope() { --depth; }
};

Result<std::size_t> aligned_size(std::size_t value, std::size_t alignment) {
    if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return failure(ErrorCode::invalid_definition, "Type layout exceeds addressable storage.");
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

Result<void> validate_storage(const TypeInfo& type, std::span<const std::byte> storage) {
    if (!storage.data() || storage.size() < type.size ||
        reinterpret_cast<std::uintptr_t>(storage.data()) % type.alignment != 0) {
        return failure(ErrorCode::invalid_storage, "Value storage is null, too small or misaligned.");
    }
    return {};
}

bool overlaps(const void* destination, const void* source, std::size_t size) noexcept {
    const auto a = reinterpret_cast<std::uintptr_t>(destination);
    const auto b = reinterpret_cast<std::uintptr_t>(source);
    return (a > b ? a - b : b - a) < size;
}

bool overlaps(const void* a_storage, std::size_t a_size, const void* b_storage, std::size_t b_size) noexcept {
    const auto a = reinterpret_cast<std::uintptr_t>(a_storage);
    const auto b = reinterpret_cast<std::uintptr_t>(b_storage);
    return a <= b ? b - a < a_size : a - b < b_size;
}

}

namespace detail {

struct ValueRootStorage {
    TypeId type;
    std::span<const std::byte> storage;
};

struct RootState {
    using Value = std::variant<ObjectReference, ValueRootStorage>;
    struct Entry {
        Value value;
        std::uint32_t next_free{no_slot};
        bool active{true};
        std::uint32_t trace_next{no_slot};
        std::uint32_t trace_previous{no_slot};
        bool trace_queued{};
        std::uint32_t value_index{no_slot};
    };
    std::vector<Entry> entries;
    // Dense live value roots only: object roots and retired entries cannot own this storage.
    std::vector<std::uint32_t> value_entries;
    std::uint32_t free_head{no_slot};
    std::size_t count{};
    std::size_t traversals{};
    bool alive{true};
    void* observer{};
    void (*added)(void*, std::uint32_t) noexcept{};
    void (*removing)(void*, std::uint32_t) noexcept{};

    bool has_capacity() const noexcept { return free_head != no_slot || entries.size() < no_slot; }

    std::uint32_t add(Value value) {
        if (traversals != 0) throw std::logic_error("Cannot add roots during reference traversal.");
        if (!has_capacity()) throw std::length_error("Root identity space exhausted.");
        const bool is_value = std::holds_alternative<ValueRootStorage>(value);
        if (is_value && value_entries.size() == value_entries.capacity()) {
            value_entries.reserve(std::min<std::size_t>(no_slot,
                std::max<std::size_t>(8, value_entries.capacity() * 2)));
        }
        auto index = free_head;
        if (index == no_slot) {
            index = static_cast<std::uint32_t>(entries.size());
            entries.push_back({std::move(value)});
        } else {
            auto& entry = entries[index];
            free_head = entry.next_free;
            entry.value = std::move(value);
            entry.next_free = no_slot;
            entry.active = true;
        }
        if (is_value) {
            entries[index].value_index = static_cast<std::uint32_t>(value_entries.size());
            value_entries.push_back(index);
        }
        ++count;
        if (added) added(observer, index);
        return index;
    }

    void remove(std::uint32_t index) noexcept {
        if (removing) removing(observer, index);
        auto& entry = entries[index];
        if (entry.value_index != no_slot) {
            const auto moved = value_entries.back();
            value_entries[entry.value_index] = moved;
            entries[moved].value_index = entry.value_index;
            value_entries.pop_back();
            entry.value_index = no_slot;
        }
        entry.value = ObjectReference{};
        entry.active = false;
        entry.next_free = free_head;
        entry.trace_next = no_slot;
        entry.trace_previous = no_slot;
        entry.trace_queued = false;
        free_head = index;
        --count;
    }

    bool overlaps_storage(const void* storage, std::size_t size) const noexcept {
        for (const auto index : value_entries) {
            const auto& value = std::get<ValueRootStorage>(entries[index].value);
            if (overlaps(storage, size, value.storage.data(), value.storage.size())) return true;
        }
        return false;
    }
};

}

ObjectRoot::ObjectRoot(std::shared_ptr<detail::RootState> state, std::uint32_t index) noexcept
    : state_(std::move(state)), index_(index) {}

ObjectRoot::~ObjectRoot() { reset(); }

ObjectRoot::ObjectRoot(const ObjectRoot& other) {
    if (other.state_) {
        index_ = other.state_->add(other.reference());
        state_ = other.state_;
    }
}

ObjectRoot& ObjectRoot::operator=(const ObjectRoot& other) {
    if (this != &other) {
        ObjectRoot copy{other};
        *this = std::move(copy);
    }
    return *this;
}

ObjectRoot::ObjectRoot(ObjectRoot&& other) noexcept
    : state_(std::move(other.state_)), index_(std::exchange(other.index_, 0)) {}

ObjectRoot& ObjectRoot::operator=(ObjectRoot&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        index_ = std::exchange(other.index_, 0);
    }
    return *this;
}

void ObjectRoot::reset() noexcept {
    if (state_) {
        state_->remove(index_);
        state_.reset();
        index_ = 0;
    }
}

bool ObjectRoot::registered() const noexcept { return state_ && state_->alive; }

ObjectReference ObjectRoot::reference() const noexcept {
    return state_ ? std::get<ObjectReference>(state_->entries[index_].value) : ObjectReference{};
}

ValueRoot::ValueRoot(std::shared_ptr<detail::RootState> state, std::uint32_t index) noexcept
    : state_(std::move(state)), index_(index) {}

ValueRoot::~ValueRoot() { reset(); }

ValueRoot::ValueRoot(ValueRoot&& other) noexcept
    : state_(std::move(other.state_)), index_(std::exchange(other.index_, 0)) {}

ValueRoot& ValueRoot::operator=(ValueRoot&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        index_ = std::exchange(other.index_, 0);
    }
    return *this;
}

void ValueRoot::reset() noexcept {
    if (state_) {
        state_->remove(index_);
        state_.reset();
        index_ = 0;
    }
}

bool ValueRoot::registered() const noexcept { return state_ && state_->alive; }

struct Runtime::Impl {
    struct Type;
    struct FieldStorage {
        std::size_t offset{};
        void* (*address)(void*) noexcept{};
        ValueDescription value;
        const Type* type{};
    };
    struct Type {
        TypeInfo info;
        NativeLayout native;
        std::vector<FieldStorage> fields;
        std::vector<Invoker> methods;
        std::vector<std::uint32_t> trace_fields;
        std::size_t trace_depth{1};
        std::vector<std::uint32_t> destroy_fields;
        std::size_t walk_depth{1};
        bool needs_destroy{};
        // Only built-in scalars and padding-free homogeneous described layouts qualify.
        const Type* scalar_leaf{};
    };
    struct Slot {
        TypeId type;
        void* storage{};
        std::uint64_t generation{1};
        std::uint32_t next_free{no_slot};
        std::uint64_t mark_epoch{};
        std::uint32_t gray_next{no_slot};
        std::uint32_t gray_previous{no_slot};
        bool gray_queued{};
    };

    std::uint64_t context{next_context.fetch_add(1, std::memory_order_relaxed)};
    std::vector<std::unique_ptr<Type>> types;
    std::vector<Slot> slots;
    // Handles keep only this registry alive, never the Runtime or its objects.
    std::shared_ptr<detail::RootState> roots{std::make_shared<detail::RootState>()};
    std::uint32_t free_head{no_slot};
    std::size_t live{};
    std::size_t callbacks{};
    std::size_t active_executions{};
    bool sealed{};
    TypeId f32;
    TypeId u32;
    CollectionPhase phase{CollectionPhase::idle};
    std::uint64_t epoch{};
    std::size_t marked{};
    std::size_t pending{};
    std::size_t root_cursor{}, root_limit{}, sweep_cursor{}, sweep_limit{};
    std::uint32_t gray_head{no_slot}, gray_tail{no_slot};
    std::uint32_t root_head{no_slot}, root_tail{no_slot};
    struct TraceFrame {
        const Type* type;
        const void* storage;
        std::size_t next_field{};
    };
    std::vector<TraceFrame> trace_stack;
    std::size_t maximum_trace_depth{};
    enum class TraceOwner { none, object, value_root };
    TraceOwner trace_owner{TraceOwner::none};
    std::uint32_t trace_owner_index{no_slot};
    struct WalkFrame {
        const Type* type{};
        std::byte* storage{};
        const std::byte* source{};
        std::size_t next_field{};
    };
    // Synchronous walks use disjoint suffixes; callbacks may grow this vector.
    // No reference into it survives a callback. GC's suspended cursor stays separate.
    std::vector<WalkFrame> walk_stack;
    struct WalkScope {
        std::vector<WalkFrame>& stack;
        const std::size_t base{stack.size()};
        ~WalkScope() { stack.resize(base); }
    };

    Impl() {
        walk_stack.reserve(1);
        roots->observer = this;
        roots->added = [](void* context, std::uint32_t index) noexcept {
            static_cast<Impl*>(context)->root_added(index);
        };
        roots->removing = [](void* context, std::uint32_t index) noexcept {
            static_cast<Impl*>(context)->root_removing(index);
        };
    }

    void prepare_walk(const Type& type) {
        if (type.scalar_leaf) return;
        // Reserve before touching values, including space for failure cleanup/barriers.
        walk_stack.reserve(walk_stack.size() + type.walk_depth);
    }

    bool marking() const noexcept { return phase == CollectionPhase::roots || phase == CollectionPhase::mark; }

    bool is_live(const Slot& slot) const noexcept {
        return slot.storage && (phase != CollectionPhase::sweep || slot.mark_epoch == epoch);
    }

    void cancel_cursor() noexcept {
        trace_stack.clear();
        trace_owner = TraceOwner::none;
        trace_owner_index = no_slot;
    }

    void remove_gray(std::uint32_t index) noexcept {
        auto& slot = slots[index];
        if (!slot.gray_queued) return;
        if (slot.gray_previous != no_slot) slots[slot.gray_previous].gray_next = slot.gray_next;
        else gray_head = slot.gray_next;
        if (slot.gray_next != no_slot) slots[slot.gray_next].gray_previous = slot.gray_previous;
        else gray_tail = slot.gray_previous;
        slot.gray_next = no_slot;
        slot.gray_previous = no_slot;
        slot.gray_queued = false;
    }

    void shade(ObjectId object) noexcept {
        if (!marking() || !lookup(object)) return;
        auto& slot = slots[object.index_];
        if (slot.mark_epoch == epoch) return;
        slot.mark_epoch = epoch;
        ++marked;
        slot.gray_previous = gray_tail;
        slot.gray_next = no_slot;
        slot.gray_queued = true;
        if (gray_tail != no_slot) slots[gray_tail].gray_next = object.index_;
        else gray_head = object.index_;
        gray_tail = object.index_;
    }

    void remove_root_queue(std::uint32_t index) noexcept {
        auto& entry = roots->entries[index];
        if (!entry.trace_queued) return;
        if (entry.trace_previous != no_slot) roots->entries[entry.trace_previous].trace_next = entry.trace_next;
        else root_head = entry.trace_next;
        if (entry.trace_next != no_slot) roots->entries[entry.trace_next].trace_previous = entry.trace_previous;
        else root_tail = entry.trace_previous;
        entry.trace_next = no_slot;
        entry.trace_previous = no_slot;
        entry.trace_queued = false;
    }

    void root_added(std::uint32_t index) noexcept {
        if (!marking()) return;
        auto& entry = roots->entries[index];
        if (const auto* reference = std::get_if<ObjectReference>(&entry.value)) {
            shade(reference->id());
            return;
        }
        const auto& value = std::get<detail::ValueRootStorage>(entry.value);
        if (entry.trace_queued || lookup(value.type)->trace_fields.empty()) return;
        entry.trace_previous = root_tail;
        entry.trace_next = no_slot;
        entry.trace_queued = true;
        if (root_tail != no_slot) roots->entries[root_tail].trace_next = index;
        else root_head = index;
        root_tail = index;
    }

    void root_removing(std::uint32_t index) noexcept {
        remove_root_queue(index);
        if (trace_owner == TraceOwner::value_root && trace_owner_index == index) cancel_cursor();
    }

    void shade_value(const Type& type, const void* storage) noexcept {
        if (!marking() || type.trace_fields.empty()) return;
        WalkScope scope{walk_stack};
        walk_stack.push_back({&type, static_cast<std::byte*>(const_cast<void*>(storage))});
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == frame.type->trace_fields.size()) {
                walk_stack.pop_back();
                continue;
            }
            ++walk_stack[index].next_field;
            const auto& field = frame.type->fields[frame.type->trace_fields[frame.next_field]];
            auto* address = field.address ? field.address(frame.storage) : frame.storage + field.offset;
            if (field.value.form == ValueForm::strong_reference) {
                shade(static_cast<const ReferenceSlot*>(address)->object_);
            } else {
                walk_stack.push_back({field.type, static_cast<std::byte*>(address)});
            }
        }
    }

    void start_trace(const Type& type, const void* storage, TraceOwner owner, std::uint32_t index) noexcept {
        if (type.trace_fields.empty()) return;
        trace_owner = owner;
        trace_owner_index = index;
        // begin_collection reserved the maximum depth of every published layout.
        trace_stack.push_back({&type, storage});
    }

    void trace_one() noexcept {
        auto& frame = trace_stack.back();
        if (frame.next_field == frame.type->trace_fields.size()) {
            trace_stack.pop_back();
            if (trace_stack.empty()) cancel_cursor();
            return;
        }
        const auto& field = frame.type->fields[frame.type->trace_fields[frame.next_field++]];
        const auto* address = field.address ? field.address(const_cast<void*>(frame.storage))
            : static_cast<const std::byte*>(frame.storage) + field.offset;
        // A locator may revoke its ValueRoot. Never resurrect the cancelled borrow.
        if (trace_owner == TraceOwner::none) return;
        if (field.value.form == ValueForm::strong_reference) {
            shade(static_cast<const ReferenceSlot*>(address)->object_);
        } else {
            trace_stack.push_back({field.type, address});
        }
    }

    void recycle(std::uint32_t index) noexcept {
        auto& slot = slots[index];
        // Retire exhausted generations rather than rebinding an old identity.
        if (slot.generation != std::numeric_limits<std::uint64_t>::max()) {
            ++slot.generation;
            slot.next_free = free_head;
            free_head = index;
        }
    }

    bool collect_one() noexcept {
        if (phase == CollectionPhase::roots) {
            if (root_cursor != root_limit) {
                const auto index = static_cast<std::uint32_t>(root_cursor++);
                if (roots->entries[index].active) root_added(index);
            } else {
                phase = CollectionPhase::mark;
            }
            return false;
        }
        if (phase == CollectionPhase::mark) {
            if (!trace_stack.empty()) {
                CallbackScope traversal_scope{roots->traversals};
                trace_one();
            } else if (root_head != no_slot) {
                const auto index = root_head;
                const auto value = std::get<detail::ValueRootStorage>(roots->entries[index].value);
                remove_root_queue(index);
                start_trace(*lookup(value.type), value.storage.data(), TraceOwner::value_root, index);
            } else if (gray_head != no_slot) {
                const auto index = gray_head;
                remove_gray(index);
                const auto& slot = slots[index];
                start_trace(*lookup(slot.type), slot.storage, TraceOwner::object, index);
            } else {
                // One atomic logical-death commit. No heap-wide invalidation pass.
                pending = live - marked;
                live = marked;
                sweep_cursor = 0;
                sweep_limit = slots.size();
                phase = CollectionPhase::sweep;
            }
            return false;
        }
        if (sweep_cursor != sweep_limit) {
            const auto index = static_cast<std::uint32_t>(sweep_cursor++);
            auto& slot = slots[index];
            if (slot.storage && slot.mark_epoch != epoch) {
                release(slot);
                recycle(index);
                return true;
            }
        } else {
            phase = CollectionPhase::idle;
        }
        return false;
    }

    Type* lookup(TypeId id) const noexcept {
        if (id.context_ != context || id.index_ >= types.size()) return nullptr;
        return types[id.index_].get();
    }

    const Type* lookup(const TypeReference& reference,
        std::span<const std::unique_ptr<Type>> staged = {}) const {
        // Even inside a transaction, explicit IDs name only published types.
        if (const auto* id = std::get_if<TypeId>(&reference.target_)) return lookup(*id);
        const auto matches = [&](const auto& type) {
            if (const auto* name = std::get_if<std::string>(&reference.target_)) return type->info.name == *name;
            return type->native.representation == std::get<const void*>(reference.target_);
        };
        for (const auto& type : staged) if (matches(type)) return type.get();
        for (const auto& type : types) if (matches(type)) return type.get();
        return nullptr;
    }

    Result<FunctionSignature> resolve_signature(const SignatureDefinition& definition,
        std::optional<TypeId> owner = std::nullopt,
        std::span<const std::unique_ptr<Type>> staged = {}) const {
        const auto resolve_value = [&](const ValueDefinition& value) -> Result<ValueDescription> {
            const auto* type = lookup(value.type, staged);
            if (!type) return failure(ErrorCode::invalid_definition, "Signature contains an unresolved value type.");
            if (type->info.kind != TypeKind::scalar || value.form != ValueForm::inline_value)
                return failure(ErrorCode::invalid_definition, "Signature parameters and result must be inline scalars.");
            return ValueDescription{type->info.id, value.form};
        };
        FunctionSignature signature;
        signature.parameters.reserve(definition.parameters.size());
        for (const auto& parameter : definition.parameters) {
            auto value = resolve_value(parameter);
            if (!value) return std::unexpected(std::move(value.error()));
            signature.parameters.push_back(*value);
        }
        if (definition.result) {
            auto value = resolve_value(*definition.result);
            if (!value) return std::unexpected(std::move(value.error()));
            signature.result = *value;
        }
        if (definition.receiver) {
            const auto* type = lookup(definition.receiver->type, staged);
            if (!type || type->info.kind != TypeKind::object)
                return failure(ErrorCode::invalid_definition, "Signature receiver must name an object type.");
            if (owner && type->info.id != *owner)
                return failure(ErrorCode::invalid_definition, "Method receiver must match its owning object type.");
            signature.receiver = ReceiverDescription{type->info.id, definition.receiver->read_only};
        } else if (owner) {
            signature.receiver = ReceiverDescription{*owner, false};
        }
        return signature;
    }

    const Slot* lookup(ObjectId id) const noexcept {
        if (id.context_ != context || id.index_ >= slots.size()) return nullptr;
        const auto& slot = slots[id.index_];
        return is_live(slot) && slot.generation == id.generation_ ? &slot : nullptr;
    }

    Result<const Type*> value_type(TypeId id, std::span<const std::byte> storage) const {
        const auto* type = lookup(id);
        if (!type || type->info.kind == TypeKind::object) {
            return failure(ErrorCode::invalid_type, "Value operations require a registered scalar or record type.");
        }
        const auto valid = validate_storage(type->info, storage);
        if (!valid) return std::unexpected(valid.error());
        return type;
    }

    template<class T>
    TypeId add_scalar(std::string name) {
        const TypeId id{context, static_cast<std::uint32_t>(types.size())};
        auto type = std::make_unique<Type>();
        type->info = {id, std::move(name), TypeKind::scalar, sizeof(T), alignof(T), {}, {}, true, true};
        type->scalar_leaf = type.get();
        type->native = {sizeof(T), alignof(T), detail::representation<T>(),
            [](void* address) { std::construct_at(static_cast<T*>(address)); },
            [](void* address) noexcept { std::destroy_at(static_cast<T*>(address)); },
            [](void* destination, const void* source) {
                std::construct_at(static_cast<T*>(destination), *static_cast<const T*>(source));
            },
            [](void* destination, const void* source) {
                *static_cast<T*>(destination) = *static_cast<const T*>(source);
            }};
        types.push_back(std::move(type));
        return id;
    }

    TypeId scalar_type(const Scalar& scalar) const noexcept {
        return std::holds_alternative<float>(scalar) ? f32 : u32;
    }

    template<class T>
    static void initialize_scalars(void* storage, std::size_t size) noexcept {
        auto* bytes = static_cast<std::byte*>(storage);
        for (std::size_t offset = 0; offset != size; offset += sizeof(T)) {
            std::construct_at(reinterpret_cast<T*>(bytes + offset));
        }
    }

    void construct_scalar_block(const Type& type, void* storage, const void* source) noexcept {
        if (source) std::memcpy(storage, source, type.info.size);
        else if (type.scalar_leaf->info.id == f32) initialize_scalars<float>(storage, type.info.size);
        else initialize_scalars<std::uint32_t>(storage, type.info.size);
    }

    // Native constructors own their subobjects. Described frames track only completed
    // fields, so a failed child is never destroyed twice during reverse unwinding.
    void construct(const Type& type, void* storage, const void* source = nullptr) {
        if (type.native.construct) {
            if (source) type.native.copy_construct(storage, source);
            else type.native.construct(storage);
            return;
        }
        if (type.scalar_leaf) {
            construct_scalar_block(type, storage, source);
            return;
        }
        WalkScope scope{walk_stack};
        walk_stack.push_back({&type, static_cast<std::byte*>(storage), static_cast<const std::byte*>(source)});
        try {
            while (walk_stack.size() != scope.base) {
                const auto index = walk_stack.size() - 1;
                const auto frame = walk_stack[index];
                if (frame.next_field == frame.type->fields.size()) {
                    walk_stack.pop_back();
                    if (walk_stack.size() != scope.base) ++walk_stack.back().next_field;
                    continue;
                }
                const auto& field = frame.type->fields[frame.next_field];
                auto* destination = frame.storage + field.offset;
                const auto* origin = frame.source ? frame.source + field.offset : nullptr;
                if (field.value.form == ValueForm::inline_value) {
                    if (field.type->native.construct) {
                        if (origin) field.type->native.copy_construct(destination, origin);
                        else field.type->native.construct(destination);
                    } else if (field.type->scalar_leaf) {
                        construct_scalar_block(*field.type, destination, origin);
                    } else {
                        walk_stack.push_back({field.type, destination, origin});
                        continue;
                    }
                } else if (origin) {
                    std::construct_at(reinterpret_cast<ReferenceSlot*>(destination),
                        *reinterpret_cast<const ReferenceSlot*>(origin));
                } else {
                    std::construct_at(reinterpret_cast<ReferenceSlot*>(destination));
                }
                ++walk_stack[index].next_field;
            }
        } catch (...) {
            while (walk_stack.size() != scope.base) {
                const auto index = walk_stack.size() - 1;
                const auto frame = walk_stack[index];
                if (frame.next_field == 0) {
                    walk_stack.pop_back();
                    continue;
                }
                --walk_stack[index].next_field;
                const auto& field = frame.type->fields[frame.next_field - 1];
                if (field.value.form == ValueForm::inline_value) {
                    destroy(*field.type, frame.storage + field.offset);
                }
            }
            throw;
        }
    }

    Result<void> try_construct(const Type& type, void* storage, const void* source = nullptr) {
        prepare_walk(type);
        CallbackScope scope{callbacks};
        try {
            construct(type, storage, source);
            shade_value(type, storage);
            return {};
        } catch (const std::exception& error) {
            return failure(ErrorCode::construction_failed, error.what());
        } catch (...) {
            return failure(ErrorCode::construction_failed, "Native value construction raised an unknown exception.");
        }
    }

    void assign(const Type& type, void* storage, const void* source) {
        if (type.native.construct) {
            type.native.copy_assign(storage, source);
            return;
        }
        if (type.scalar_leaf) {
            std::memcpy(storage, source, type.info.size);
            return;
        }
        WalkScope scope{walk_stack};
        walk_stack.push_back({&type, static_cast<std::byte*>(storage), static_cast<const std::byte*>(source)});
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == frame.type->fields.size()) {
                walk_stack.pop_back();
                continue;
            }
            ++walk_stack[index].next_field;
            const auto& field = frame.type->fields[frame.next_field];
            auto* destination = frame.storage + field.offset;
            const auto* origin = frame.source + field.offset;
            if (field.value.form == ValueForm::inline_value) {
                if (field.type->native.construct) field.type->native.copy_assign(destination, origin);
                else if (field.type->scalar_leaf) std::memcpy(destination, origin, field.type->info.size);
                else walk_stack.push_back({field.type, destination, origin});
            } else {
                *reinterpret_cast<ReferenceSlot*>(destination) = *reinterpret_cast<const ReferenceSlot*>(origin);
            }
        }
    }

    void destroy(const Type& type, void* storage) noexcept {
        if (!type.needs_destroy) return;
        if (type.native.destroy) {
            type.native.destroy(storage);
            return;
        }
        WalkScope scope{walk_stack};
        walk_stack.push_back({&type, static_cast<std::byte*>(storage), nullptr, type.destroy_fields.size()});
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == 0) {
                walk_stack.pop_back();
                continue;
            }
            --walk_stack[index].next_field;
            const auto& field = frame.type->fields[frame.type->destroy_fields[frame.next_field - 1]];
            auto* address = frame.storage + field.offset;
            if (field.type->native.destroy) field.type->native.destroy(address);
            else walk_stack.push_back({field.type, address, nullptr, field.type->destroy_fields.size()});
        }
    }

    Result<void> trace(const Type& type, const void* storage, ReferenceVisitor visitor) {
        if (!type.info.contains_references) return {};
        prepare_walk(type);
        WalkScope scope{walk_stack};
        walk_stack.push_back({&type, static_cast<std::byte*>(const_cast<void*>(storage))});
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == frame.type->fields.size()) {
                walk_stack.pop_back();
                continue;
            }
            ++walk_stack[index].next_field;
            const auto& field = frame.type->fields[frame.next_field];
            // Preserve locator order, including scalar fields in reference-bearing native types.
            auto* address = field.address ? field.address(frame.storage) : frame.storage + field.offset;
            if (field.value.form == ValueForm::inline_value) {
                if (field.type->info.contains_references) {
                    walk_stack.push_back({field.type, static_cast<std::byte*>(address)});
                }
            } else {
                const auto object = static_cast<const ReferenceSlot*>(address)->object_;
                if (object == ObjectId{}) continue;
                const auto result = visitor.visit(visitor.context, ObjectReference{field.value, object});
                if (!result) return result;
            }
        }
        return {};
    }

    template<class F>
    Result<void> visit_guarded(ReferenceVisitor visitor, F&& enumerate) {
        if (!visitor.visit) return failure(ErrorCode::argument_mismatch, "Reference traversal requires a visitor.");
        CallbackScope callback_scope{callbacks};
        CallbackScope traversal_scope{roots->traversals};
        try {
            return enumerate();
        } catch (const std::exception& error) {
            return failure(ErrorCode::invocation_failed, error.what());
        } catch (...) {
            return failure(ErrorCode::invocation_failed, "Reference visitor raised an unknown exception.");
        }
    }

    void release(Slot& slot) noexcept {
        const auto index = static_cast<std::uint32_t>(&slot - slots.data());
        remove_gray(index);
        if (trace_owner == TraceOwner::object && trace_owner_index == index) cancel_cursor();
        if (is_live(slot)) {
            --live;
            if (phase != CollectionPhase::idle && slot.mark_epoch == epoch) --marked;
        } else {
            --pending;
        }
        auto* type = lookup(slot.type);
        void* storage = std::exchange(slot.storage, nullptr);
        destroy(*type, storage);
        ::operator delete(storage, std::align_val_t{type->info.alignment});
    }
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {
    if (impl_->context == 0) throw std::overflow_error("Runtime identity space exhausted.");
    impl_->f32 = impl_->add_scalar<float>("core.f32");
    impl_->u32 = impl_->add_scalar<std::uint32_t>("core.u32");
}

Result<void> Runtime::check_execution_entry() const {
    if (!impl_->roots->alive || impl_->callbacks != 0 || impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Generated execution cannot enter during a native borrow, traversal or shutdown.");
    return {};
}

Result<void> Runtime::enter_execution() {
    auto ready = check_execution_entry();
    if (!ready) return ready;
    if (impl_->active_executions == std::numeric_limits<std::size_t>::max())
        return failure(ErrorCode::capacity_exceeded, "Execution nesting space exhausted.");
    ++impl_->active_executions;
    return {};
}

void Runtime::leave_execution() noexcept { --impl_->active_executions; }

Result<void> Runtime::check_code_publication() const {
    auto ready = check_execution_entry();
    if (!ready) return ready;
    if (impl_->active_executions != 0)
        return failure(ErrorCode::busy, "Code publication requires all generated invocations to return.");
    return {};
}

Result<void> Runtime::begin_collection() {
    if (impl_->callbacks != 0 || !impl_->roots->alive || impl_->phase != CollectionPhase::idle) {
        return failure(ErrorCode::busy, "Collection can begin only at an idle host safe point.");
    }
    if (impl_->epoch == std::numeric_limits<std::uint64_t>::max()) {
        return failure(ErrorCode::capacity_exceeded, "Collection epoch space exhausted.");
    }
    impl_->trace_stack.reserve(impl_->maximum_trace_depth);
    impl_->cancel_cursor();
    ++impl_->epoch;
    impl_->marked = 0;
    impl_->pending = 0;
    impl_->root_cursor = 0;
    impl_->root_limit = impl_->roots->entries.size();
    impl_->phase = CollectionPhase::roots;
    return {};
}

Result<CollectionProgress> Runtime::collect_step(CollectionBudget budget) {
    if (impl_->callbacks != 0 || !impl_->roots->alive) {
        return failure(ErrorCode::busy, "Collection steps require a host safe point.");
    }
    if (budget.max_work == 0 || budget.max_time < std::chrono::nanoseconds::zero()) {
        return failure(ErrorCode::argument_mismatch, "Collection requires positive work and nonnegative time limits.");
    }
    const auto started = std::chrono::steady_clock::now();
    CollectionProgress progress;
    CallbackScope scope{impl_->callbacks};
    while (progress.work_done != budget.max_work && impl_->phase != CollectionPhase::idle) {
        if (progress.work_done != 0 && budget.max_time != std::chrono::nanoseconds::zero() &&
            std::chrono::steady_clock::now() - started >= budget.max_time) break;
        progress.reclaimed += impl_->collect_one();
        ++progress.work_done;
    }
    progress.phase = impl_->phase;
    progress.pending_reclamation = impl_->pending;
    progress.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
    return progress;
}

CollectionPhase Runtime::collection_phase() const noexcept { return impl_->phase; }
std::size_t Runtime::pending_reclamation() const noexcept { return impl_->pending; }

Runtime::~Runtime() {
    impl_->roots->alive = false;
    impl_->roots->observer = nullptr;
    impl_->roots->added = nullptr;
    impl_->roots->removing = nullptr;
    CallbackScope scope{impl_->callbacks};
    for (auto& slot : impl_->slots) {
        if (slot.storage) impl_->release(slot);
    }
}

Result<TypeId> Runtime::register_type(TypeDefinition definition) {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(std::move(definition));
    auto published = register_types(std::move(definitions));
    if (!published) return std::unexpected(std::move(published.error()));
    return published->front();
}

Result<FunctionSignature> Runtime::resolve_signature(const SignatureDefinition& definition) const {
    return impl_->resolve_signature(definition);
}

Result<std::vector<TypeId>> Runtime::register_types(std::vector<TypeDefinition> definitions) {
    if (impl_->phase != CollectionPhase::idle) return failure(ErrorCode::busy, "Cannot publish types during collection.");
    if (impl_->sealed || impl_->callbacks != 0) {
        return failure(ErrorCode::busy, "Type registration closes when object execution begins.");
    }
    if (definitions.empty()) {
        return failure(ErrorCode::invalid_definition, "Type publication requires a non-empty batch.");
    }
    if (definitions.size() > no_slot - impl_->types.size()) {
        return failure(ErrorCode::capacity_exceeded, "Type identity space exhausted.");
    }

    const auto first_index = impl_->types.size();
    std::vector<std::unique_ptr<Impl::Type>> staged;
    staged.reserve(definitions.size());
    for (auto& definition : definitions) {
        if (definition.name.empty() ||
            (definition.kind != TypeKind::record && definition.kind != TypeKind::object)) {
            return failure(ErrorCode::invalid_definition, "A named record or object type is required.");
        }
        const auto same_name = [&](const auto& type) { return type->info.name == definition.name; };
        if (std::ranges::any_of(impl_->types, same_name) || std::ranges::any_of(staged, same_name)) {
            return failure(ErrorCode::duplicate_name, "Type already defined: " + definition.name);
        }
        const auto& native = definition.native;
        if (native.representation) {
            const auto same_representation = [&](const auto& type) {
                return type->native.representation == native.representation;
            };
            if (std::ranges::any_of(impl_->types, same_representation) ||
                std::ranges::any_of(staged, same_representation)) {
                return failure(ErrorCode::invalid_definition, "C++ representation already has a type definition.");
            }
        }
        if (definition.fields.size() >= no_slot || definition.methods.size() >= no_slot) {
            return failure(ErrorCode::capacity_exceeded, "Member identity space exhausted.");
        }
        const bool is_native = native.size || native.alignment || native.representation || native.construct ||
                               native.destroy || native.copy_construct || native.copy_assign;
        if (is_native && (!native.size || !std::has_single_bit(native.alignment) ||
            !native.representation || !native.construct || !native.destroy || native.size % native.alignment != 0)) {
            return failure(ErrorCode::invalid_definition, "Native layout requires valid size, alignment and lifecycle operations.");
        }
        if (definition.kind == TypeKind::record && !definition.methods.empty()) {
            return failure(ErrorCode::invalid_definition, "This slice supports methods on object types only.");
        }
        const TypeId id{impl_->context, static_cast<std::uint32_t>(first_index + staged.size())};
        auto type = std::make_unique<Impl::Type>();
        type->info = {id, std::move(definition.name), definition.kind, native.size, is_native ? native.alignment : 1, {}, {}};
        if (definition.kind == TypeKind::record) {
            type->info.copy_constructible = !is_native || native.copy_construct;
            type->info.copy_assignable = !is_native || native.copy_assign;
        }
        type->native = native;
        staged.push_back(std::move(type));
    }

    // Only this transaction sees staged identities. Published TypeId inputs cannot name them.
    const auto resolve = [&](const TypeReference& reference) { return impl_->lookup(reference, staged); };
    const auto resolved_type = [&](TypeId id) -> const Impl::Type* {
        return id.index_ < first_index ? impl_->types[id.index_].get() : staged[id.index_ - first_index].get();
    };

    for (std::size_t i = 0; i != definitions.size(); ++i) {
        auto& definition = definitions[i];
        auto& type = *staged[i];
        const auto id = type.info.id;
        const bool is_native = type.native.construct != nullptr;
        type.info.fields.reserve(definition.fields.size());
        type.fields.reserve(definition.fields.size());
        // Validate names before moving any strings, including non-exposed fields.
        for (std::size_t field_index = 0; field_index != definition.fields.size(); ++field_index) {
            const auto& name = definition.fields[field_index].name;
            if (name.empty()) return failure(ErrorCode::invalid_definition, "Field name must not be empty.");
            for (std::size_t previous = 0; previous != field_index; ++previous) {
                if (definition.fields[previous].name == name) return failure(ErrorCode::duplicate_name, "Duplicate field: " + name);
            }
        }
        for (std::size_t method_index = 0; method_index != definition.methods.size(); ++method_index) {
            const auto& name = definition.methods[method_index].name;
            for (const auto& field : definition.fields) {
                if (field.name == name) return failure(ErrorCode::duplicate_name, "Duplicate member: " + name);
            }
            for (std::size_t previous = 0; previous != method_index; ++previous) {
                if (definition.methods[previous].name == name) return failure(ErrorCode::duplicate_name, "Duplicate method: " + name);
            }
        }
        for (auto& field : definition.fields) {
            const auto* target = resolve(field.value.type);
            if (!target) {
                return failure(ErrorCode::invalid_definition, "Unresolved field type: " + type.info.name + "." + field.name);
            }
            const auto form = field.value.form;
            if (form == ValueForm::inline_value) {
                if (target->info.kind == TypeKind::object) {
                    return failure(ErrorCode::invalid_definition, "Objects cannot be stored as inline values.");
                }
            } else if (form == ValueForm::strong_reference || form == ValueForm::weak_reference) {
                if (target->info.kind != TypeKind::object) {
                    return failure(ErrorCode::invalid_definition, "Object references require an object target type.");
                }
            } else {
                return failure(ErrorCode::invalid_definition, "Unknown value form.");
            }
            if (is_native) {
                const auto representation = form == ValueForm::inline_value
                    ? target->native.representation : detail::representation<ReferenceSlot>();
                if (!field.native_address || !field.native_representation || field.native_representation != representation) {
                    return failure(ErrorCode::invalid_definition, "Native member representation does not match its value description.");
                }
            } else if (field.native_address || field.native_representation) {
                return failure(ErrorCode::invalid_definition, "Described fields cannot supply native member accessors.");
            }
            const auto index = static_cast<std::uint32_t>(type.fields.size());
            const ValueDescription value{target->info.id, form};
            type.fields.push_back({0, field.native_address, value});
            if (field.exposed) type.info.fields.push_back({FieldId{id, index}, std::move(field.name), value});
        }
        type.info.methods.reserve(definition.methods.size());
        type.methods.reserve(definition.methods.size());
        for (auto& method : definition.methods) {
            if (method.name.empty() || !method.invoke) {
                return failure(ErrorCode::invalid_definition, "Methods require a name and implementation.");
            }
            auto signature = impl_->resolve_signature(method.signature, id, staged);
            if (!signature) return std::unexpected(std::move(signature.error()));
            type.info.methods.push_back({MethodId{id, static_cast<std::uint32_t>(type.info.methods.size())},
                std::move(method.name), std::move(*signature)});
            type.methods.push_back(method.invoke);
        }
    }

    // Only inline edges constrain layout. An explicit stack rejects cycles without
    // consuming the C++ call stack for deeply nested, but valid, definition batches.
    enum class LayoutState { unseen, active, complete };
    struct LayoutFrame { std::size_t type; std::size_t next_field; };
    std::vector<LayoutState> states(staged.size(), LayoutState::unseen);
    std::vector<LayoutFrame> stack;
    for (std::size_t start = 0; start != staged.size(); ++start) {
        if (states[start] == LayoutState::complete) continue;
        states[start] = LayoutState::active;
        stack.push_back({start, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            auto& type = *staged[frame.type];
            bool descended = false;
            while (frame.next_field != type.fields.size()) {
                const auto value = type.fields[frame.next_field++].value;
                if (value.form != ValueForm::inline_value || value.type.index_ < first_index) continue;
                const auto dependency = value.type.index_ - first_index;
                if (states[dependency] == LayoutState::active) {
                    return failure(ErrorCode::invalid_definition, "Inline layout cycle at type: " + staged[dependency]->info.name);
                }
                if (states[dependency] == LayoutState::unseen) {
                    states[dependency] = LayoutState::active;
                    stack.push_back({dependency, 0});
                    descended = true;
                    break;
                }
            }
            if (descended) continue;
            const bool is_native = type.native.construct != nullptr;
            type.needs_destroy = is_native;
            for (std::size_t field_index = 0; field_index != type.fields.size(); ++field_index) {
                const auto value = type.fields[field_index].value;
                const auto* target = resolved_type(value.type);
                type.fields[field_index].type = target;
                const bool reference = value.form != ValueForm::inline_value;
                type.info.contains_references |= reference || target->info.contains_references;
                if (!reference) {
                    type.walk_depth = std::max(type.walk_depth, target->walk_depth + 1);
                    if (!is_native && target->needs_destroy) {
                        type.destroy_fields.push_back(static_cast<std::uint32_t>(field_index));
                        type.needs_destroy = true;
                    }
                }
                if (value.form == ValueForm::strong_reference || (!reference && !target->trace_fields.empty())) {
                    type.trace_fields.push_back(static_cast<std::uint32_t>(field_index));
                    if (!reference) type.trace_depth = std::max(type.trace_depth, target->trace_depth + 1);
                }
                if (!is_native) {
                    const auto alignment = reference ? alignof(ReferenceSlot) : target->info.alignment;
                    const auto size = reference ? sizeof(ReferenceSlot) : target->info.size;
                    const auto offset = aligned_size(type.info.size, alignment);
                    if (!offset) return std::unexpected(offset.error());
                    if (size > std::numeric_limits<std::size_t>::max() - *offset) {
                        return failure(ErrorCode::invalid_definition, "Field exceeds addressable storage.");
                    }
                    type.fields[field_index].offset = *offset;
                    type.info.size = *offset + size;
                    type.info.alignment = std::max(type.info.alignment, alignment);
                    if (!reference) {
                        type.info.copy_constructible &= target->info.copy_constructible;
                        type.info.copy_assignable &= target->info.copy_assignable;
                    }
                }
            }
            if (!is_native) {
                const auto size = aligned_size(std::max(std::size_t{1}, type.info.size), type.info.alignment);
                if (!size) return std::unexpected(size.error());
                type.info.size = *size;
                if (!type.fields.empty()) {
                    const auto* scalar = type.fields.front().type->scalar_leaf;
                    std::size_t end = 0;
                    for (const auto& field : type.fields) {
                        if (!scalar || field.value.form != ValueForm::inline_value ||
                            field.type->scalar_leaf != scalar || field.offset != end) {
                            scalar = nullptr;
                            break;
                        }
                        end += field.type->info.size;
                    }
                    if (end == type.info.size) type.scalar_leaf = scalar;
                    if (type.scalar_leaf) type.walk_depth = 1;
                }
            }
            states[frame.type] = LayoutState::complete;
            stack.pop_back();
        }
    }

    std::vector<TypeId> result;
    result.reserve(staged.size());
    for (const auto& type : staged) result.push_back(type->info.id);
    // All potentially allocating work precedes commit. Moving unique_ptrs cannot fail.
    std::size_t walk_depth = 1;
    for (const auto& type : staged) walk_depth = std::max(walk_depth, type->walk_depth);
    impl_->walk_stack.reserve(walk_depth);
    impl_->types.reserve(first_index + staged.size());
    for (auto& type : staged) {
        if (!type->trace_fields.empty()) impl_->maximum_trace_depth = std::max(impl_->maximum_trace_depth, type->trace_depth);
        impl_->types.push_back(std::move(type));
    }
    return result;
}

Result<TypeId> Runtime::find_type(std::string_view name) const {
    for (const auto& type : impl_->types) {
        if (type->info.name == name) return type->info.id;
    }
    return failure(ErrorCode::invalid_type, "Unknown type: " + std::string{name});
}

Result<TypeId> Runtime::find_representation(const void* representation) const {
    for (const auto& type : impl_->types) {
        if (type->native.representation == representation) return type->info.id;
    }
    return failure(ErrorCode::invalid_type, "Register the native value type before exposing its members.");
}

Result<const TypeInfo*> Runtime::describe(TypeId type) const {
    const auto* found = impl_->lookup(type);
    if (!found) return failure(ErrorCode::invalid_type, "Type does not belong to this runtime.");
    return &found->info;
}

Result<FieldId> Runtime::find_field(TypeId type, std::string_view name) const {
    const auto info = describe(type);
    if (!info) return std::unexpected(info.error());
    for (const auto& field : (*info)->fields) {
        if (field.name == name) return field.id;
    }
    return failure(ErrorCode::invalid_member, "Unknown field: " + std::string{name});
}

Result<MethodId> Runtime::find_method(TypeId type, std::string_view name) const {
    const auto info = describe(type);
    if (!info) return std::unexpected(info.error());
    for (const auto& method : (*info)->methods) {
        if (method.name == name) return method.id;
    }
    return failure(ErrorCode::invalid_member, "Unknown method: " + std::string{name});
}

Result<void> Runtime::initialize_value(TypeId type_id, std::span<std::byte> destination) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Value mutation is not allowed during reference traversal.");
    const auto type = impl_->value_type(type_id, destination);
    if (!type) return std::unexpected(type.error());
    if (impl_->roots->overlaps_storage(destination.data(), (*type)->info.size)) {
        return failure(ErrorCode::busy, "End value-root registrations before starting a new value lifetime.");
    }
    return impl_->try_construct(**type, destination.data());
}

Result<void> Runtime::copy_construct_value(TypeId type_id, std::span<std::byte> destination,
                                         std::span<const std::byte> source) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Value mutation is not allowed during reference traversal.");
    const auto type = impl_->value_type(type_id, destination);
    if (!type) return std::unexpected(type.error());
    const auto valid = validate_storage((*type)->info, source);
    if (!valid) return std::unexpected(valid.error());
    if (impl_->roots->overlaps_storage(destination.data(), (*type)->info.size)) {
        return failure(ErrorCode::busy, "End value-root registrations before starting a new value lifetime.");
    }
    if (!(*type)->info.copy_constructible) {
        return failure(ErrorCode::unsupported_operation, "Type does not support copy construction.");
    }
    if (overlaps(destination.data(), source.data(), (*type)->info.size)) {
        return failure(ErrorCode::invalid_storage, "Copy construction requires non-overlapping values.");
    }
    return impl_->try_construct(**type, destination.data(), source.data());
}

Result<void> Runtime::assign_value(TypeId type_id, std::span<std::byte> destination,
                                 std::span<const std::byte> source) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Value mutation is not allowed during reference traversal.");
    const auto type = impl_->value_type(type_id, destination);
    if (!type) return std::unexpected(type.error());
    const auto valid = validate_storage((*type)->info, source);
    if (!valid) return std::unexpected(valid.error());
    if (!(*type)->info.copy_assignable) {
        return failure(ErrorCode::unsupported_operation, "Type does not support copy assignment.");
    }
    if (destination.data() == source.data()) return {};
    if (overlaps(destination.data(), source.data(), (*type)->info.size)) {
        return failure(ErrorCode::invalid_storage, "Assignment does not support partially overlapping values.");
    }
    impl_->prepare_walk(**type);
    CallbackScope scope{impl_->callbacks};
    try {
        impl_->assign(**type, destination.data(), source.data());
        impl_->shade_value(**type, destination.data());
        return {};
    } catch (const std::exception& error) {
        impl_->shade_value(**type, destination.data());
        return failure(ErrorCode::assignment_failed, error.what());
    } catch (...) {
        impl_->shade_value(**type, destination.data());
        return failure(ErrorCode::assignment_failed, "Native value assignment raised an unknown exception.");
    }
}

Result<void> Runtime::destroy_value(TypeId type_id, std::span<std::byte> storage) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Value mutation is not allowed during reference traversal.");
    const auto type = impl_->value_type(type_id, storage);
    if (!type) return std::unexpected(type.error());
    if (impl_->roots->overlaps_storage(storage.data(), (*type)->info.size)) {
        return failure(ErrorCode::busy, "End value-root registrations before destroying their storage.");
    }
    impl_->prepare_walk(**type);
    CallbackScope scope{impl_->callbacks};
    impl_->destroy(**type, storage.data());
    return {};
}

Result<std::uint32_t> Runtime::resolve_object(ObjectId object) const {
    if (object.context_ != impl_->context || object.index_ >= impl_->slots.size()) {
        return failure(ErrorCode::invalid_object, "Object does not belong to this runtime.");
    }
    const auto& slot = impl_->slots[object.index_];
    if (!impl_->is_live(slot) || slot.generation != object.generation_) {
        return failure(ErrorCode::invalid_object, "Object has been destroyed or committed for collection.");
    }
    return object.index_;
}

Result<ObjectRoot> Runtime::create(TypeId type_id) {
    if (impl_->callbacks != 0) return failure(ErrorCode::busy, "Object creation is not allowed during a callback.");
    const auto* type = impl_->lookup(type_id);
    if (!type || type->info.kind != TypeKind::object) {
        return failure(ErrorCode::invalid_type, "Only registered object types have independent identity.");
    }
    if ((impl_->free_head == no_slot && impl_->slots.size() >= no_slot) || !impl_->roots->has_capacity()) {
        return failure(ErrorCode::capacity_exceeded, "Object or root identity space exhausted.");
    }
    // No user callback may resize slots, so commit will not allocate after construction.
    if (impl_->free_head == no_slot && impl_->slots.size() == impl_->slots.capacity()) {
        impl_->slots.reserve(std::min<std::size_t>(no_slot,
            std::max(impl_->slots.size() + 1, impl_->slots.capacity() * 2)));
    }
    impl_->prepare_walk(*type);
    CallbackScope scope{impl_->callbacks};
    void* storage = ::operator new(type->info.size, std::align_val_t{type->info.alignment});
    try {
        impl_->construct(*type, storage);
    } catch (const std::exception& error) {
        ::operator delete(storage, std::align_val_t{type->info.alignment});
        return failure(ErrorCode::construction_failed, error.what());
    } catch (...) {
        ::operator delete(storage, std::align_val_t{type->info.alignment});
        return failure(ErrorCode::construction_failed, "Native constructor raised an unknown exception.");
    }
    const auto index = impl_->free_head == no_slot ? static_cast<std::uint32_t>(impl_->slots.size()) : impl_->free_head;
    const auto generation = index == impl_->slots.size() ? std::uint64_t{1} : impl_->slots[index].generation;
    const ObjectId object{impl_->context, index, generation};
    std::uint32_t root_index;
    try {
        root_index = impl_->roots->add(ObjectReference{{type_id, ValueForm::strong_reference}, object});
    } catch (...) {
        impl_->destroy(*type, storage);
        ::operator delete(storage, std::align_val_t{type->info.alignment});
        throw;
    }
    ObjectRoot root{impl_->roots, root_index};
    if (index == impl_->slots.size()) impl_->slots.emplace_back();
    else impl_->free_head = impl_->slots[index].next_free;
    auto& slot = impl_->slots[index];
    slot.type = type_id;
    slot.storage = storage;
    slot.next_free = no_slot;
    slot.mark_epoch = 0;
    ++impl_->live;
    if (impl_->phase == CollectionPhase::sweep) {
        slot.mark_epoch = impl_->epoch;
        ++impl_->marked;
    } else {
        impl_->shade(object);
    }
    impl_->sealed = true;
    return root;
}

Result<void> Runtime::destroy(ObjectId object) {
    if (impl_->callbacks != 0) return failure(ErrorCode::busy, "Object destruction is not allowed during a callback.");
    const auto index = resolve_object(object);
    if (!index) return std::unexpected(index.error());
    auto& slot = impl_->slots[*index];
    {
        CallbackScope scope{impl_->callbacks};
        impl_->release(slot);
    }
    impl_->recycle(*index);
    return {};
}

Result<TypeId> Runtime::type_of(ObjectId object) const {
    const auto index = resolve_object(object);
    if (!index) return std::unexpected(index.error());
    return impl_->slots[*index].type;
}

Result<void> Runtime::validate_reference(const ObjectReference& reference) const {
    const auto description = reference.description();
    const auto* type = impl_->lookup(description.type);
    if (!type || type->info.kind != TypeKind::object) {
        return failure(ErrorCode::invalid_type, "Reference target type does not belong to this runtime.");
    }
    if (description.form != ValueForm::strong_reference && description.form != ValueForm::weak_reference) {
        return failure(ErrorCode::type_mismatch, "An object reference requires a reference value form.");
    }
    if (!reference.is_null()) {
        if (reference.id().context_ != impl_->context) return failure(ErrorCode::invalid_object, "Reference identity belongs to another runtime.");
        const auto* live = impl_->lookup(reference.id());
        if (live && live->type != description.type) {
            return failure(ErrorCode::type_mismatch, "Live object does not match the reference target type.");
        }
    }
    return {};
}

Result<ObjectRoot> Runtime::hold(ObjectId object) {
    const auto type = type_of(object);
    if (!type) return std::unexpected(type.error());
    return hold(ObjectReference{{*type, ValueForm::strong_reference}, object});
}

Result<ObjectRoot> Runtime::hold(const ObjectReference& reference) {
    if (!impl_->roots->alive || impl_->roots->traversals != 0) {
        return failure(ErrorCode::busy, "Cannot add roots during shutdown or reference traversal.");
    }
    const auto valid = validate_reference(reference);
    if (!valid) return std::unexpected(valid.error());
    if (reference.description().form != ValueForm::strong_reference) {
        return failure(ErrorCode::type_mismatch, "Use try_acquire to obtain a strong root from a weak reference.");
    }
    if (!impl_->roots->has_capacity()) return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    return ObjectRoot{impl_->roots, impl_->roots->add(reference)};
}

Result<ObjectReference> Runtime::observe(const ObjectReference& reference) const {
    const auto valid = validate_reference(reference);
    if (!valid) return std::unexpected(valid.error());
    return ObjectReference{{reference.description().type, ValueForm::weak_reference}, reference.id()};
}

Result<ObjectReference> Runtime::null_reference(TypeId type_id, ValueForm form) const {
    ObjectReference reference{{type_id, form}, ObjectId{}};
    const auto valid = validate_reference(reference);
    if (!valid) return std::unexpected(valid.error());
    return reference;
}

Result<std::optional<ObjectRoot>> Runtime::try_acquire(const ObjectReference& reference) {
    if (!impl_->roots->alive || impl_->roots->traversals != 0) {
        return failure(ErrorCode::busy, "Cannot acquire references during shutdown or reference traversal.");
    }
    const auto valid = validate_reference(reference);
    if (!valid) return std::unexpected(valid.error());
    if (reference.description().form != ValueForm::weak_reference) {
        return failure(ErrorCode::type_mismatch, "try_acquire requires a weak reference.");
    }
    if (reference.is_null() || !impl_->lookup(reference.id())) return std::optional<ObjectRoot>{};
    if (!impl_->roots->has_capacity()) return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    ObjectReference strong{{reference.description().type, ValueForm::strong_reference}, reference.id()};
    return std::optional<ObjectRoot>{ObjectRoot{impl_->roots, impl_->roots->add(strong)}};
}

Result<ValueRoot> Runtime::root_value(TypeId type_id, std::span<const std::byte> storage) {
    if (!impl_->roots->alive || impl_->roots->traversals != 0) {
        return failure(ErrorCode::busy, "Cannot register value roots during shutdown or reference traversal.");
    }
    const auto type = impl_->value_type(type_id, storage);
    if (!type) return std::unexpected(type.error());
    if (!impl_->roots->has_capacity()) return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    return ValueRoot{impl_->roots, impl_->roots->add(detail::ValueRootStorage{type_id, storage.first((*type)->info.size)})};
}

Result<void> Runtime::visit_roots(ReferenceVisitor visitor) const {
    if (!impl_->roots->alive) return failure(ErrorCode::busy, "Runtime is shutting down.");
    return impl_->visit_guarded(visitor, [&]() -> Result<void> {
        const auto limit = impl_->roots->entries.size();
        for (std::size_t i = 0; i != limit; ++i) {
            if (!impl_->roots->entries[i].active) continue;
            // A visitor may release a root; keep the current descriptor independent.
            const auto value = impl_->roots->entries[i].value;
            if (const auto* reference = std::get_if<ObjectReference>(&value)) {
                if (!reference->is_null()) {
                    const auto result = visitor.visit(visitor.context, *reference);
                    if (!result) return result;
                }
            } else {
                const auto& rooted = std::get<detail::ValueRootStorage>(value);
                const auto result = impl_->trace(*impl_->lookup(rooted.type), rooted.storage.data(), visitor);
                if (!result) return result;
            }
        }
        return {};
    });
}

Result<void> Runtime::visit_references(ObjectId object, ReferenceVisitor visitor) const {
    const auto index = resolve_object(object);
    if (!index) return std::unexpected(index.error());
    const auto& slot = impl_->slots[*index];
    return impl_->visit_guarded(visitor, [&] { return impl_->trace(*impl_->lookup(slot.type), slot.storage, visitor); });
}

Result<void> Runtime::visit_references(TypeId type_id, std::span<const std::byte> storage, ReferenceVisitor visitor) const {
    const auto type = impl_->value_type(type_id, storage);
    if (!type) return std::unexpected(type.error());
    return impl_->visit_guarded(visitor, [&] { return impl_->trace(**type, storage.data(), visitor); });
}

std::size_t Runtime::root_count() const noexcept { return impl_->roots->count; }

Result<Runtime::Address> Runtime::resolve_field(ObjectId object, std::span<const FieldId> path) const {
    const auto index = resolve_object(object);
    if (!index) return std::unexpected(index.error());
    const auto& slot = impl_->slots[*index];
    return resolve_path(slot.type, slot.storage, path);
}

Result<Runtime::Address> Runtime::resolve_path(TypeId type, void* storage, std::span<const FieldId> path) const {
    if (path.empty()) return failure(ErrorCode::invalid_member, "Field path must not be empty.");
    CallbackScope scope{impl_->callbacks};
    Address result{storage, {type}};
    for (const auto field : path) {
        if (result.value.form != ValueForm::inline_value) {
            return failure(ErrorCode::invalid_member, "Field paths cannot traverse object references.");
        }
        const auto* owner = impl_->lookup(result.value.type);
        if (field.owner_ != result.value.type || field.index_ >= owner->fields.size()) {
            return failure(ErrorCode::invalid_member, "Field does not belong to its containing type.");
        }
        const auto& access = owner->fields[field.index_];
        result.data = access.address ? access.address(result.data) : static_cast<std::byte*>(result.data) + access.offset;
        result.value = access.value;
    }
    return result;
}

Result<ObjectReference> Runtime::read_reference_at(Address address) const {
    if (address.value.form == ValueForm::inline_value) return failure(ErrorCode::type_mismatch, "Read requires an object reference field.");
    ObjectReference reference{address.value, static_cast<const ReferenceSlot*>(address.data)->object_};
    const auto valid = validate_reference(reference);
    if (!valid) return std::unexpected(valid.error());
    return reference;
}

Result<void> Runtime::write_reference_at(Address address, const ObjectReference& reference) {
    const auto valid = validate_reference(reference);
    if (!valid) return std::unexpected(valid.error());
    if ((address.value.form != ValueForm::strong_reference && address.value.form != ValueForm::weak_reference) ||
        address.value.type != reference.description().type ||
        (address.value.form == ValueForm::strong_reference && reference.description().form != ValueForm::strong_reference)) {
        return failure(ErrorCode::type_mismatch, "Reference target or holding form does not match the field.");
    }
    static_cast<ReferenceSlot*>(address.data)->object_ = reference.id();
    if (address.value.form == ValueForm::strong_reference) impl_->shade(reference.id());
    return {};
}

Result<ObjectReference> Runtime::read_reference(ValueDescription value, const ReferenceSlot& storage) const {
    return read_reference_at({const_cast<ReferenceSlot*>(&storage), value});
}

Result<void> Runtime::write_reference(ValueDescription value, ReferenceSlot& storage, const ObjectReference& reference) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Reference mutation is not allowed during traversal.");
    return write_reference_at({&storage, value}, reference);
}

Result<ObjectReference> Runtime::read_reference(ObjectId object, std::span<const FieldId> path) const {
    const auto address = resolve_field(object, path);
    if (!address) return std::unexpected(address.error());
    return read_reference_at(*address);
}

Result<void> Runtime::write_reference(ObjectId object, std::span<const FieldId> path, const ObjectReference& reference) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Reference mutation is not allowed during traversal.");
    const auto address = resolve_field(object, path);
    if (!address) return std::unexpected(address.error());
    return write_reference_at(*address, reference);
}

Result<ObjectReference> Runtime::read_reference(TypeId type_id, std::span<const std::byte> storage,
                                               std::span<const FieldId> path) const {
    const auto type = impl_->value_type(type_id, storage);
    if (!type) return std::unexpected(type.error());
    const auto address = resolve_path(type_id, const_cast<std::byte*>(storage.data()), path);
    if (!address) return std::unexpected(address.error());
    return read_reference_at(*address);
}

Result<void> Runtime::write_reference(TypeId type_id, std::span<std::byte> storage,
                                     std::span<const FieldId> path, const ObjectReference& reference) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Reference mutation is not allowed during traversal.");
    const auto type = impl_->value_type(type_id, storage);
    if (!type) return std::unexpected(type.error());
    const auto address = resolve_path(type_id, storage.data(), path);
    if (!address) return std::unexpected(address.error());
    return write_reference_at(*address, reference);
}

Result<Scalar> Runtime::read(ObjectId object, std::span<const FieldId> path) const {
    const auto address = resolve_field(object, path);
    if (!address) return std::unexpected(address.error());
    if (address->value.type == impl_->f32) return Scalar{*static_cast<const float*>(address->data)};
    if (address->value.type == impl_->u32) return Scalar{*static_cast<const std::uint32_t*>(address->data)};
    return failure(ErrorCode::type_mismatch, "Read requires a scalar field; select a field inside the record.");
}

Result<void> Runtime::write(ObjectId object, std::span<const FieldId> path, Scalar value) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Field mutation is not allowed during reference traversal.");
    const auto address = resolve_field(object, path);
    if (!address) return std::unexpected(address.error());
    if (address->value.form != ValueForm::inline_value || address->value.type != impl_->scalar_type(value)) {
        return failure(ErrorCode::type_mismatch, "Value type does not match the field.");
    }
    if (address->value.type == impl_->f32) *static_cast<float*>(address->data) = std::get<float>(value);
    else *static_cast<std::uint32_t*>(address->data) = std::get<std::uint32_t>(value);
    return {};
}

Result<void> Runtime::call(ObjectId object, MethodId method, std::span<const Scalar> arguments, Scalar* output) {
    if (impl_->roots->traversals != 0) return failure(ErrorCode::busy, "Method execution is not allowed during reference traversal.");
    const auto index = resolve_object(object);
    if (!index) return std::unexpected(index.error());
    const auto& slot = impl_->slots[*index];
    const auto* type = impl_->lookup(slot.type);
    if (method.owner_ != slot.type || method.index_ >= type->methods.size()) {
        return failure(ErrorCode::invalid_member, "Method does not belong to the receiver type.");
    }
    const auto& signature = type->info.methods[method.index_].signature;
    if (arguments.size() != signature.parameters.size()) {
        return failure(ErrorCode::argument_mismatch, "Incorrect argument count.");
    }
    for (std::size_t i = 0; i != arguments.size(); ++i) {
        if (impl_->scalar_type(arguments[i]) != signature.parameters[i].type) {
            return failure(ErrorCode::argument_mismatch, "Argument type does not match parameter " + std::to_string(i));
        }
    }
    if (signature.result && !output)
        return failure(ErrorCode::invalid_storage, "Non-void method requires result storage.");
    CallbackScope scope{impl_->callbacks};
    CallContext context{*this, object, slot.storage, arguments};
    try {
        auto invoked = type->methods[method.index_](context);
        if (!invoked) return invoked;
        if (signature.result) {
            if (!context.result || impl_->scalar_type(*context.result) != signature.result->type)
                return failure(ErrorCode::invocation_failed, "Method did not produce its declared result type.");
            *output = *context.result;
        } else if (context.result) {
            return failure(ErrorCode::invocation_failed, "Void method produced an undeclared result.");
        }
        return {};
    } catch (const std::exception& error) {
        return failure(ErrorCode::invocation_failed, error.what());
    } catch (...) {
        return failure(ErrorCode::invocation_failed, "Native method raised an unknown exception.");
    }
}

std::size_t Runtime::live_objects() const noexcept { return impl_->live; }

}
