#include "foundation/map.hpp"
#include "foundation/string.hpp"
#include "runtime/runtime.hpp"
#include "runtime/detail/type_publication.hpp"
#include "math/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
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

bool normalize_description(std::string& text) {
    if (!StringView{std::string_view{text}}.is_valid_utf8())
        return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127 ||
            (c == 0xc2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
             static_cast<unsigned char>(text[i + 1]) <= 0x9f))
            return false;
    }
    // Normalize in place only when needed, without allocating another copy.
    auto output = text.find('\r');
    if (output == std::string::npos)
        return true;
    for (auto input = output; input < text.size(); ++input) {
        const char c = text[input];
        text[output++] = c == '\r' ? '\n' : c;
        if (c == '\r' && input + 1 < text.size() && text[input + 1] == '\n')
            ++input;
    }
    text.resize(output);
    return true;
}

struct CallbackScope {
    std::size_t& depth;
    explicit CallbackScope(std::size_t& count) : depth(count) { ++depth; }
    ~CallbackScope() { --depth; }
};

// Exact for every range leaf type: f32, i32 and u32 all widen losslessly.
double range_value(const Scalar& value) noexcept {
    return std::visit(
        [](const auto& v) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(v)>, EnumValue>)
                return 0.0;
            else
                return static_cast<double>(v);
        },
        value);
}

template <class T>
concept RangeLeaf = std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, std::int32_t> ||
                    std::same_as<T, std::uint32_t>;

template <RangeLeaf T>
bool constrain_leaf(const FieldRange& range, T& value) noexcept {
    if constexpr (std::floating_point<T>)
        if (!std::isfinite(value))
            return false;
    if (range.hard_min && value < std::get<T>(*range.hard_min))
        value = std::get<T>(*range.hard_min);
    if (range.hard_max && value > std::get<T>(*range.hard_max))
        value = std::get<T>(*range.hard_max);
    return true;
}

// Publication guarantees the value and both hard bounds share the field's leaf type.
bool constrain_scalar(const FieldRange& range, Scalar& value) noexcept {
    return std::visit(
        [&](auto& v) {
            if constexpr (RangeLeaf<std::remove_cvref_t<decltype(v)>>)
                return constrain_leaf(range, v);
            else
                return true;
        },
        value);
}

// Constrains one leaf in place through memcpy, since storage need not be typed.
bool constrain_storage(const FieldRange& range, void* data, bool write) noexcept {
    return std::visit(
        [&](const auto& bound) {
            using T = std::remove_cvref_t<decltype(bound)>;
            if constexpr (RangeLeaf<T>) {
                T value;
                std::memcpy(&value, data, sizeof value);
                if (!constrain_leaf(range, value))
                    return false;
                if (write)
                    std::memcpy(data, &value, sizeof value);
            }
            return true;
        },
        range.hard_min ? *range.hard_min : *range.hard_max);
}

std::string scalar_text(const Scalar& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::remove_cvref_t<decltype(v)>;
            if constexpr (std::same_as<T, EnumValue>)
                return std::to_string(v.bits);
            else if constexpr (std::same_as<T, bool>)
                return v ? "true" : "false";
            else {
                char digits[64];
                return {digits, std::to_chars(digits, digits + sizeof digits, v).ptr};
            }
        },
        value);
}

// Why a stored leaf breaks its hard bounds, or empty; `value` receives the leaf either way.
std::string bound_violation(const FieldRange& range, const void* data, Scalar& value) {
    return std::visit(
        [&](const auto& bound) -> std::string {
            using T = std::remove_cvref_t<decltype(bound)>;
            if constexpr (RangeLeaf<T>) {
                T leaf;
                std::memcpy(&leaf, data, sizeof leaf);
                value = leaf;
                if constexpr (std::floating_point<T>)
                    if (!std::isfinite(leaf))
                        return scalar_text(value) + " is not finite";
                if (range.hard_min && leaf < std::get<T>(*range.hard_min))
                    return scalar_text(value) + " is below the hard minimum " + scalar_text(*range.hard_min);
                if (range.hard_max && leaf > std::get<T>(*range.hard_max))
                    return scalar_text(value) + " is above the hard maximum " + scalar_text(*range.hard_max);
            }
            return {};
        },
        range.hard_min ? *range.hard_min : *range.hard_max);
}

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
    void (*added)(void*, std::uint32_t) noexcept {};
    void (*removing)(void*, std::uint32_t) noexcept {};

    bool has_capacity() const noexcept { return free_head != no_slot || entries.size() < no_slot; }

    std::uint32_t add(Value value) {
        if (traversals != 0)
            throw std::logic_error("Cannot add roots during reference traversal.");
        if (!has_capacity())
            throw std::length_error("Root identity space exhausted.");
        const bool is_value = std::holds_alternative<ValueRootStorage>(value);
        if (is_value && value_entries.size() == value_entries.capacity()) {
            value_entries.reserve(
                std::min<std::size_t>(no_slot, std::max<std::size_t>(8, value_entries.capacity() * 2)));
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
        if (added)
            added(observer, index);
        return index;
    }

    void remove(std::uint32_t index) noexcept {
        if (removing)
            removing(observer, index);
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
            if (overlaps(storage, size, value.storage.data(), value.storage.size()))
                return true;
        }
        return false;
    }
};

}

ObjectRoot::ObjectRoot(std::shared_ptr<detail::RootState> state, std::uint32_t index) noexcept
    : state_(std::move(state)), index_(index) {}

ObjectRoot::~ObjectRoot() {
    reset();
}

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

bool ObjectRoot::registered() const noexcept {
    return state_ && state_->alive;
}

ObjectReference ObjectRoot::reference() const noexcept {
    return state_ ? std::get<ObjectReference>(state_->entries[index_].value) : ObjectReference{};
}

ValueRoot::ValueRoot(std::shared_ptr<detail::RootState> state, std::uint32_t index) noexcept
    : state_(std::move(state)), index_(index) {}

ValueRoot::~ValueRoot() {
    reset();
}

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

bool ValueRoot::registered() const noexcept {
    return state_ && state_->alive;
}

struct Runtime::Impl {
    struct Type;
    struct FieldStorage {
        std::size_t offset{};
        void* (*address)(void*) noexcept {};
        ValueDescription value;
        const Type* type{};
        std::vector<Scalar> default_leaves{};
        // Only fields with hard bounds allocate; the stable address backs Address and leaf pointers.
        std::unique_ptr<const FieldRange> hard{};
    };
    // A hard-bounded scalar inside a record or object, reached through storage indices
    // (base segments included); `field` is the reported path, without base segments.
    // With `array` set, `path` reaches an Array: `range` then bounds each scalar or vector
    // element, otherwise every element applies its own type's leaves.
    struct ConstrainedLeaf {
        std::vector<std::uint32_t> path;
        const FieldRange* range;
        std::vector<FieldId> field;
        const Type* array{};
    };
    // A type validator for the value at `path`: an inline record, a base segment or the type
    // itself. With `array` set, every element runs its own type's validators instead.
    struct ValidatorStep {
        std::vector<std::uint32_t> path;
        std::vector<FieldId> field;
        Result<void> (*validate)(const void*);
        const Type* array{};
    };
    struct Type {
        TypeInfo info;
        NativeLayout native;
        const Type* base{};
        // Own declarations, including hidden names, for inherited conflict checks.
        std::vector<std::string> member_names;
        // An internal leading base segment reuses layout/lifetime/trace walkers.
        // Native constructors still own their complete C++ object themselves.
        std::vector<FieldStorage> fields;
        std::vector<Invoker> methods;
        std::vector<std::uint32_t> trace_fields;
        std::size_t trace_depth{1};
        std::vector<std::uint32_t> destroy_fields;
        std::size_t walk_depth{1};
        bool needs_destroy{};
        // Only built-in scalars and padding-free homogeneous described layouts qualify.
        const Type* scalar_leaf{};
        // Every hard-bounded leaf (and Array of them), so whole-value writes and validate() can enforce them.
        std::vector<ConstrainedLeaf> constrained_leaves;
        Result<void> (*validate)(const void*){};
        // Post-order: base segment, inline records and Arrays innermost first, the type's own validator last.
        std::vector<ValidatorStep> validators;
        // Non-trivial values only: byte ranges (null swap) and native swaps that exchange two
        // values without throwing. Empty when the type cannot, so writes fall back to assignment.
        struct SwapPart {
            std::size_t offset;
            std::size_t size;
            void (*swap)(void*, void*) noexcept;
        };
        std::vector<SwapPart> swap_parts;
    };
    // Ordered to stay at 64 bytes.
    struct Slot {
        TypeId type;
        void* storage{};
        std::uint64_t generation{1};
        // write_epoch of the latest write; see stamp().
        std::uint64_t written{};
        std::uint64_t mark_epoch{};
        std::uint32_t next_free{no_slot};
        std::uint32_t gray_next{no_slot};
        std::uint32_t gray_previous{no_slot};
        bool gray_queued{};
    };
    static_assert(sizeof(Slot) == 64);

    std::uint64_t context{next_context.fetch_add(1, std::memory_order_relaxed)};
    std::vector<std::unique_ptr<Type>> types;
    // Published C++ representation -> types index; registration keeps representations unique.
    Map<const void*, std::uint32_t> representations;
    // Published name -> types index. Keys view the immutable name inside each heap Type.
    Map<StringView, std::uint32_t> names;
    std::vector<Slot> slots;
    // Write stamps. write_pages[i] is the newest stamp among slots [64i, 64i + 64), so
    // changed_objects() skips unchanged pages. Sized with slots before creation commits.
    static constexpr unsigned write_page_shift = 6;
    std::uint64_t write_epoch{1};
    std::vector<std::uint64_t> write_pages;
    void stamp(std::uint32_t index) noexcept {
        slots[index].written = write_epoch;
        write_pages[index >> write_page_shift] = write_epoch;
    }
    // Handles keep only this registry alive, never the Runtime or its objects.
    std::shared_ptr<detail::RootState> roots{std::make_shared<detail::RootState>()};
    std::uint32_t free_head{no_slot};
    std::size_t live{};
    std::size_t callbacks{};
    std::size_t native_borrows{};
    bool changing_references{};
    std::size_t active_executions{};
    TypeId f32;
    TypeId f64;
    TypeId u32;
    TypeId i32;
    TypeId boolean;
    TypeId object;
    CollectionPhase phase{CollectionPhase::idle};
    std::uint64_t epoch{};
    std::size_t marked{};
    std::size_t pending{};
    std::size_t root_cursor{};
    std::size_t root_limit{};
    std::size_t sweep_cursor{};
    std::size_t sweep_limit{};
    std::uint32_t gray_head{no_slot};
    std::uint32_t gray_tail{no_slot};
    std::uint32_t root_head{no_slot};
    std::uint32_t root_tail{no_slot};
    struct TraceFrame {
        const Type* type;
        const void* storage;
        std::size_t next_field{};
        const ReferenceSequence* sequence{};
        std::size_t next_element{};
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
        if (type.scalar_leaf)
            return;
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
        if (!slot.gray_queued)
            return;
        if (slot.gray_previous != no_slot)
            slots[slot.gray_previous].gray_next = slot.gray_next;
        else
            gray_head = slot.gray_next;
        if (slot.gray_next != no_slot)
            slots[slot.gray_next].gray_previous = slot.gray_previous;
        else
            gray_tail = slot.gray_previous;
        slot.gray_next = no_slot;
        slot.gray_previous = no_slot;
        slot.gray_queued = false;
    }

    void queue_gray(std::uint32_t index) noexcept {
        auto& slot = slots[index];
        if (slot.gray_queued)
            return;
        slot.gray_previous = gray_tail;
        slot.gray_next = no_slot;
        slot.gray_queued = true;
        if (gray_tail != no_slot)
            slots[gray_tail].gray_next = index;
        else
            gray_head = index;
        gray_tail = index;
    }

    void shade(ObjectId object) noexcept {
        if (!marking() || !lookup(object))
            return;
        auto& slot = slots[object.index_];
        if (slot.mark_epoch == epoch)
            return;
        slot.mark_epoch = epoch;
        ++marked;
        queue_gray(object.index_);
    }

    void sequence_changed(ObjectId owner) noexcept {
        if (!marking())
            return;
        // shade alone cannot reschedule an already marked, partially scanned object.
        if (trace_owner == TraceOwner::object && trace_owner_index == owner.index_)
            cancel_cursor();
        if (slots[owner.index_].mark_epoch == epoch)
            queue_gray(owner.index_);
    }

    void remove_root_queue(std::uint32_t index) noexcept {
        auto& entry = roots->entries[index];
        if (!entry.trace_queued)
            return;
        if (entry.trace_previous != no_slot)
            roots->entries[entry.trace_previous].trace_next = entry.trace_next;
        else
            root_head = entry.trace_next;
        if (entry.trace_next != no_slot)
            roots->entries[entry.trace_next].trace_previous = entry.trace_previous;
        else
            root_tail = entry.trace_previous;
        entry.trace_next = no_slot;
        entry.trace_previous = no_slot;
        entry.trace_queued = false;
    }

    void root_added(std::uint32_t index) noexcept {
        if (!marking())
            return;
        auto& entry = roots->entries[index];
        if (const auto* reference = std::get_if<ObjectReference>(&entry.value)) {
            shade(reference->id());
            return;
        }
        const auto& value = std::get<detail::ValueRootStorage>(entry.value);
        if (entry.trace_queued || lookup(value.type)->trace_fields.empty())
            return;
        entry.trace_previous = root_tail;
        entry.trace_next = no_slot;
        entry.trace_queued = true;
        if (root_tail != no_slot)
            roots->entries[root_tail].trace_next = index;
        else
            root_head = index;
        root_tail = index;
    }

    void root_removing(std::uint32_t index) noexcept {
        remove_root_queue(index);
        if (trace_owner == TraceOwner::value_root && trace_owner_index == index)
            cancel_cursor();
    }

    void shade_value(const Type& type, const void* storage) noexcept {
        if (!marking() || type.trace_fields.empty())
            return;
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
            } else if (field.value.form == ValueForm::strong_sequence) {
                for (const auto object : static_cast<const ReferenceSequence*>(address)->objects_)
                    shade(object);
            } else {
                walk_stack.push_back({field.type, static_cast<std::byte*>(address)});
            }
        }
    }

    void start_trace(const Type& type, const void* storage, TraceOwner owner, std::uint32_t index) noexcept {
        if (type.trace_fields.empty())
            return;
        trace_owner = owner;
        trace_owner_index = index;
        // begin_collection reserved the maximum depth of every published layout.
        trace_stack.push_back({&type, storage});
    }

    void trace_one() noexcept {
        auto& frame = trace_stack.back();
        if (frame.sequence) {
            if (frame.next_element != frame.sequence->objects_.size()) {
                shade(frame.sequence->objects_[frame.next_element++]);
            } else {
                frame.sequence = nullptr;
                frame.next_element = 0;
            }
            return;
        }
        if (frame.next_field == frame.type->trace_fields.size()) {
            trace_stack.pop_back();
            if (trace_stack.empty())
                cancel_cursor();
            return;
        }
        const auto& field = frame.type->fields[frame.type->trace_fields[frame.next_field++]];
        const auto* address = field.address ? field.address(const_cast<void*>(frame.storage))
                                            : static_cast<const std::byte*>(frame.storage) + field.offset;
        // A locator may revoke its ValueRoot. Never resurrect the cancelled borrow.
        if (trace_owner == TraceOwner::none)
            return;
        if (field.value.form == ValueForm::strong_reference) {
            shade(static_cast<const ReferenceSlot*>(address)->object_);
        } else if (field.value.form == ValueForm::strong_sequence) {
            frame.sequence = static_cast<const ReferenceSequence*>(address);
            frame.next_element = 0;
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
                if (roots->entries[index].active)
                    root_added(index);
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
        if (id.context_ != context || id.index_ >= types.size())
            return nullptr;
        return types[id.index_].get();
    }

    static bool derives_from(const Type& actual, const Type& expected) noexcept {
        for (const auto* type = &actual; type; type = type->base) {
            if (type == &expected)
                return true;
        }
        return false;
    }

    // Caller validates the ancestry and protects native address callbacks.
    static void* base_storage(const Type& actual, const Type& expected, void* storage) noexcept {
        for (const auto* type = &actual; type != &expected; type = type->base) {
            if (type->native.base_address)
                storage = type->native.base_address(storage);
        }
        return storage;
    }

    static void* leaf_storage(const Type& record, void* storage, std::span<const std::uint32_t> path) noexcept {
        for (const auto* type = &record; const auto index : path) {
            const auto& access = type->fields[index];
            storage = access.address ? access.address(storage) : static_cast<std::byte*>(storage) + access.offset;
            type = access.type;
        }
        return storage;
    }

    // Hidden fields have no FieldInfo, so names come from the declaration order.
    std::string member_name(FieldId member) const {
        const auto& owner = *lookup(member.owner_);
        if (owner.info.kind == TypeKind::array)
            return "[" + std::to_string(member.index_) + "]";
        return owner.member_names[member.index_ - (owner.base && owner.base->info.id != object ? 1 : 0)];
    }

    void append_path(std::string& text, std::span<const FieldId> path) const {
        for (const auto id : path) {
            auto name = member_name(id);
            if (name.front() != '[')
                text += '.';
            text += name;
        }
    }

    // Names the value as validation failures do: "Type.field[i].leaf: reason".
    auto located(ErrorCode code, TypeId root, std::span<const FieldId> path, std::span<const FieldId> below,
                 std::string_view reason) const {
        std::string text = lookup(root)->info.name;
        append_path(text, path);
        append_path(text, below);
        text += ": ";
        text += reason;
        return failure(code, std::move(text));
    }

    static std::pair<std::byte*, std::size_t> elements(const Type& array, void* storage) noexcept {
        const void* data{};
        std::size_t count{};
        array.native.sequence_view(storage, &data, &count);
        return {static_cast<std::byte*>(const_cast<void*>(data)), count};
    }

    // Element selectors hold 32-bit indices; walks name elements of a larger Array by the Array.
    static constexpr std::size_t selectable_elements = std::size_t{std::numeric_limits<std::uint32_t>::max()} + 1;

    // Visits every hard-bounded scalar leaf with its range, expanding Array entries per
    // element, until `visit` returns false. A non-null `field` then holds that leaf's path.
    template <class Visit>
    bool each_bounded(const Type& type, void* storage, std::vector<FieldId>* field, Visit& visit) const {
        for (const auto& leaf : type.constrained_leaves) {
            auto* data = static_cast<std::byte*>(leaf_storage(type, storage, leaf.path));
            const auto depth = field ? field->size() : 0;
            if (field)
                field->insert(field->end(), leaf.field.begin(), leaf.field.end());
            bool proceed = true;
            if (!leaf.array)
                proceed = visit(*leaf.range, static_cast<void*>(data));
            else {
                const auto& element = *lookup(*leaf.array->info.element);
                const auto [items, count] = elements(*leaf.array, data);
                auto* named = count > selectable_elements ? nullptr : field;
                for (std::size_t i = 0; proceed && i != count; ++i) {
                    auto* item = items + i * element.info.size;
                    if (named)
                        named->push_back(FieldId{leaf.array->info.id, static_cast<std::uint32_t>(i)});
                    if (!leaf.range)
                        proceed = each_bounded(element, item, named, visit);
                    else if (element.info.kind == TypeKind::scalar)
                        proceed = visit(*leaf.range, static_cast<void*>(item));
                    else
                        for (std::uint32_t component = 0; proceed && component != element.info.math.components();
                             ++component) {
                            if (named)
                                named->push_back(FieldId{element.info.id, component});
                            proceed = visit(*leaf.range,
                                            static_cast<void*>(item + component * element.info.math.scalar_size()));
                            if (proceed && named)
                                named->pop_back();
                        }
                    if (proceed && named)
                        named->pop_back();
                }
            }
            if (!proceed)
                return false;
            if (field)
                field->resize(depth);
        }
        return true;
    }

    // Runs validators in post-order, expanding Array entries per element, until `visit`
    // returns false; a non-null `field` then holds the failing value's path.
    template <class Visit>
    bool each_validator(const Type& type, void* storage, std::vector<FieldId>* field, Visit& visit) const {
        for (const auto& step : type.validators) {
            auto* data = leaf_storage(type, storage, step.path);
            const auto depth = field ? field->size() : 0;
            if (field)
                field->insert(field->end(), step.field.begin(), step.field.end());
            bool proceed = true;
            if (!step.array)
                proceed = visit(step.validate, data);
            else {
                const auto& element = *lookup(*step.array->info.element);
                const auto [items, count] = elements(*step.array, data);
                auto* named = count > selectable_elements ? nullptr : field;
                for (std::size_t i = 0; proceed && i != count; ++i) {
                    if (named)
                        named->push_back(FieldId{step.array->info.id, static_cast<std::uint32_t>(i)});
                    proceed = each_validator(element, items + i * element.info.size, named, visit);
                    if (proceed && named)
                        named->pop_back();
                }
            }
            if (!proceed)
                return false;
            if (field)
                field->resize(depth);
        }
        return true;
    }

    // Checks (write == false) or clamps a value; false on NaN/Inf. `range` bounds a scalar
    // or math vector as a whole, otherwise the type's own hard-bounded leaves apply.
    bool constrain_value(const Type& type, const FieldRange* range, void* storage, bool write) const {
        if (range && type.info.kind == TypeKind::array) {
            const auto& element = *lookup(*type.info.element);
            const auto [items, count] = elements(type, storage);
            for (std::size_t i = 0; i != count; ++i)
                if (!constrain_value(element, range, items + i * element.info.size, write))
                    return false;
            return true;
        }
        if (range) {
            const bool scalar = type.info.kind == TypeKind::scalar;
            const auto stride = scalar ? type.info.size : type.info.math.scalar_size();
            for (std::size_t i = 0, count = scalar ? 1 : type.info.math.components(); i != count; ++i)
                if (!constrain_storage(*range, static_cast<std::byte*>(storage) + i * stride, write))
                    return false;
            return true;
        }
        auto leaf = [write](const FieldRange& bounds, void* data) { return constrain_storage(bounds, data, write); };
        return each_bounded(type, storage, nullptr, leaf);
    }

    // After constrain_value rejected a value: the selectors from `type` down to its first NaN
    // or infinite leaf. Only failures pay for the path.
    std::vector<FieldId> rejected_leaf(const Type& type, const FieldRange* range, void* storage) const {
        std::vector<FieldId> field;
        if (range && type.info.kind == TypeKind::array) {
            const auto& element = *lookup(*type.info.element);
            const auto [items, count] = elements(type, storage);
            for (std::size_t i = 0; i != count; ++i) {
                auto* item = items + i * element.info.size;
                if (constrain_value(element, range, item, false))
                    continue;
                if (i < selectable_elements) {
                    field.push_back(FieldId{type.info.id, static_cast<std::uint32_t>(i)});
                    const auto below = rejected_leaf(element, range, item);
                    field.insert(field.end(), below.begin(), below.end());
                }
                break;
            }
        } else if (range) {
            if (type.info.kind != TypeKind::scalar)
                for (std::uint32_t component = 0; component != type.info.math.components(); ++component)
                    if (!constrain_storage(*range,
                                           static_cast<std::byte*>(storage) + component * type.info.math.scalar_size(),
                                           false)) {
                        field.push_back(FieldId{type.info.id, component});
                        break;
                    }
        } else {
            auto leaf = [](const FieldRange& bounds, void* data) { return constrain_storage(bounds, data, false); };
            each_bounded(type, storage, &field, leaf);
        }
        return field;
    }

    static void swap_value(const Type& type, void* first, void* second) noexcept {
        for (const auto& part : type.swap_parts) {
            auto* a = static_cast<std::byte*>(first) + part.offset;
            auto* b = static_cast<std::byte*>(second) + part.offset;
            if (part.swap)
                part.swap(a, b);
            else
                std::swap_ranges(a, a + part.size, b);
        }
    }

    // Copies `source` over the live value at `destination`. With a `bounded` field address, hard
    // bounds reject NaN/Inf before any change and clamp afterwards. Non-trivial values are copied
    // and clamped in a temporary, then swapped in, so a failure leaves `destination` unchanged;
    // types without a noexcept swap fall back to copy assignment (basic guarantee).
    Result<void> replace_value(const Type& type, const Address* bounded, void* destination, const void* source) {
        CallbackScope scope{callbacks};
        const auto* range = bounded ? bounded->range : nullptr;
        const bool constrained = bounded;
        if (constrained && !constrain_value(type, range, const_cast<void*>(source), false))
            return located(ErrorCode::argument_mismatch, bounded->root, bounded->path,
                           rejected_leaf(type, range, const_cast<void*>(source)),
                           "Hard-bounded fields reject NaN and infinity.");
        if (type.info.plain_value || type.info.kind == TypeKind::scalar) {
            std::memmove(destination, source, type.info.size);
            if (constrained)
                constrain_value(type, range, destination, true);
            return {};
        }
        if (type.swap_parts.empty()) {
            try {
                assign(type, destination, source);
            } catch (const std::exception& error) {
                return failure(ErrorCode::assignment_failed, error.what());
            } catch (...) {
                return failure(ErrorCode::assignment_failed, "Native value assignment raised an unknown exception.");
            }
            if (constrained)
                constrain_value(type, range, destination, true);
            return {};
        }
        detail::ValueStorage temporary;
        std::byte* copy{};
        try {
            copy = temporary.prepare(type.info.size, type.info.alignment);
        } catch (const std::exception& error) {
            return failure(ErrorCode::assignment_failed, error.what());
        }
        if (auto made = try_construct(type, copy, source); !made)
            return failure(ErrorCode::assignment_failed, made.error().message);
        if (constrained)
            constrain_value(type, range, copy, true);
        swap_value(type, destination, copy);
        destroy(type, copy);
        return {};
    }

    // Valid values, the common case, pass once without paths or allocation; a failure reruns
    // with paths to report it, so its validators run twice.
    std::optional<ValidationFailure> check_rules(const Type& root, void* storage) {
        CallbackScope scope{callbacks};
        auto within = [](const FieldRange& range, void* data) {
            Scalar value;
            return bound_violation(range, data, value).empty();
        };
        auto passes = [](Result<void> (*validate)(const void*), const void* data) {
            try {
                return validate(data).has_value();
            } catch (...) {
                return false;
            }
        };
        if (each_bounded(root, storage, nullptr, within) && each_validator(root, storage, nullptr, passes))
            return std::nullopt;
        std::vector<FieldId> field;
        std::optional<ValidationFailure> found;
        const auto fail = [&](std::optional<Scalar> value, ErrorCode code, std::string reason) {
            found = ValidationFailure{field, {}, std::move(value), {code, root.info.name}};
            for (const auto id : field)
                found->names.push_back(member_name(id));
            append_path(found->error.message, field);
            found->error.message += ": " + reason;
            return false;
        };
        auto bound = [&](const FieldRange& range, void* data) {
            Scalar value;
            auto reason = bound_violation(range, data, value);
            return reason.empty() || fail(value, ErrorCode::argument_mismatch, std::move(reason));
        };
        auto rule = [&](Result<void> (*validate)(const void*), const void* data) {
            Result<void> checked;
            try {
                checked = validate(data);
            } catch (const std::exception& error) {
                return fail({}, ErrorCode::invocation_failed, std::string("validator threw: ") + error.what());
            } catch (...) {
                return fail({}, ErrorCode::invocation_failed, "validator threw an unknown exception");
            }
            return checked || fail({}, ErrorCode::argument_mismatch, std::move(checked.error().message));
        };
        if (each_bounded(root, storage, &field, bound))
            each_validator(root, storage, &field, rule);
        return found;
    }

    const Type* lookup(const TypeReference& reference, std::span<const std::unique_ptr<Type>> staged = {}) const {
        // Even inside a transaction, explicit IDs name only published types.
        if (const auto* id = std::get_if<TypeId>(&reference.target_))
            return lookup(*id);
        const auto* name = std::get_if<std::string>(&reference.target_);
        const auto matches = [&](const auto& type) {
            if (name)
                return type->info.name == *name;
            return type->native.representation == std::get<const void*>(reference.target_);
        };
        for (const auto& type : staged)
            if (matches(type))
                return type.get();
        if (name)
            return named(*name);
        return native(std::get<const void*>(reference.target_));
    }

    const Type* named(std::string_view name) const noexcept {
        const auto* index = names.find(StringView{name});
        return index ? types[*index].get() : nullptr;
    }

    const Type* native(const void* representation) const noexcept {
        const auto* index = representations.find(representation);
        return index ? types[*index].get() : nullptr;
    }

    Result<FunctionSignature> resolve_signature(const SignatureDefinition& definition,
                                                std::optional<TypeId> owner = std::nullopt,
                                                std::span<const std::unique_ptr<Type>> staged = {}) const {
        const auto resolve_value = [&](const ValueDefinition& value, bool borrow = false) -> Result<ValueDescription> {
            const auto* type = lookup(value.type, staged);
            if (!type)
                return failure(ErrorCode::invalid_definition, "Signature contains an unresolved value type.");
            const bool scalar = type->info.kind == TypeKind::scalar || type->info.kind == TypeKind::enumeration;
            const bool record_value = type->info.kind == TypeKind::record && type->info.plain_value;
            const bool value_borrow = borrow && !type->info.contains_references &&
                                      (type->info.kind == TypeKind::record || type->info.kind == TypeKind::array ||
                                       type->info.kind == TypeKind::string);
            if ((!scalar && !record_value && !value_borrow) || value.form != ValueForm::inline_value)
                return failure(
                    ErrorCode::invalid_definition,
                    "Signature requires scalar/enum/plain record values or synchronous value borrows without GC references.");
            if (definition.execution == FunctionExecution::coroutine && record_value)
                return failure(ErrorCode::invalid_definition,
                               "Coroutine boundaries do not yet carry record value parameters or results.");
            return ValueDescription{type->info.id, value.form};
        };
        FunctionSignature signature;
        if (definition.execution != FunctionExecution::synchronous &&
            definition.execution != FunctionExecution::coroutine)
            return failure(ErrorCode::invalid_definition, "Signature has an invalid execution kind.");
        if (owner && definition.execution != FunctionExecution::synchronous)
            return failure(ErrorCode::invalid_definition, "Native method invokers cannot suspend.");
        signature.execution = definition.execution;
        signature.parameters.reserve(definition.parameters.size());
        for (const auto& parameter : definition.parameters) {
            auto value = resolve_value(parameter, parameter.passing != ParameterPassing::value);
            if (!value)
                return std::unexpected(std::move(value.error()));
            if (parameter.passing != ParameterPassing::value && parameter.passing != ParameterPassing::read_only &&
                parameter.passing != ParameterPassing::reference)
                return failure(ErrorCode::invalid_definition, "Unknown parameter passing mode.");
            if (definition.execution == FunctionExecution::coroutine && parameter.passing != ParameterPassing::value)
                return failure(ErrorCode::invalid_definition, "Coroutine parameters cannot borrow caller storage.");
            signature.parameters.push_back({*value, parameter.passing});
        }
        if (definition.result) {
            auto value = resolve_value(*definition.result);
            if (!value)
                return std::unexpected(std::move(value.error()));
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
        if (id.context_ != context || id.index_ >= slots.size())
            return nullptr;
        const auto& slot = slots[id.index_];
        return is_live(slot) && slot.generation == id.generation_ ? &slot : nullptr;
    }

    Result<const Type*> value_type(TypeId id, std::span<const std::byte> storage) const {
        const auto* type = lookup(id);
        if (!type || type->info.kind == TypeKind::object) {
            return failure(ErrorCode::invalid_type, "Value operations require a registered value type.");
        }
        const auto valid = validate_storage(type->info, storage);
        if (!valid)
            return std::unexpected(valid.error());
        return type;
    }

    template <class T>
    TypeId add_scalar(std::string name) {
        const TypeId id{context, static_cast<std::uint32_t>(types.size())};
        auto type = std::make_unique<Type>();
        type->info = {id, std::move(name), TypeKind::scalar, sizeof(T), alignof(T), {}, {}, true, true};
        type->info.plain_value = true;
        type->scalar_leaf = type.get();
        type->native = {sizeof(T),
                        alignof(T),
                        detail::representation<T>(),
                        [](void* address) { std::construct_at(static_cast<T*>(address)); },
                        [](void* address) noexcept { std::destroy_at(static_cast<T*>(address)); },
                        [](void* destination, const void* source) {
                            std::construct_at(static_cast<T*>(destination), *static_cast<const T*>(source));
                        },
                        [](void* destination, const void* source) {
                            *static_cast<T*>(destination) = *static_cast<const T*>(source);
                        }};
        representations.try_emplace(type->native.representation, id.index_);
        names.try_emplace(StringView{std::string_view{type->info.name}}, id.index_);
        types.push_back(std::move(type));
        return id;
    }

    TypeId scalar_type(const Scalar& scalar) const noexcept {
        if (std::holds_alternative<float>(scalar))
            return f32;
        if (std::holds_alternative<double>(scalar))
            return f64;
        if (std::holds_alternative<std::uint32_t>(scalar))
            return u32;
        if (std::holds_alternative<std::int32_t>(scalar))
            return i32;
        if (std::holds_alternative<bool>(scalar))
            return boolean;
        return std::get<EnumValue>(scalar).type;
    }

    template <class T>
    static void initialize_scalars(void* storage, std::size_t size) noexcept {
        auto* bytes = static_cast<std::byte*>(storage);
        for (std::size_t offset = 0; offset != size; offset += sizeof(T)) {
            std::construct_at(reinterpret_cast<T*>(bytes + offset));
        }
    }

    void construct_scalar_block(const Type& type, void* storage, const void* source) noexcept {
        if (source)
            std::memcpy(storage, source, type.info.size);
        else if (type.scalar_leaf->info.id == f32)
            initialize_scalars<float>(storage, type.info.size);
        else if (type.scalar_leaf->info.id == f64)
            initialize_scalars<double>(storage, type.info.size);
        else if (type.scalar_leaf->info.id == i32)
            initialize_scalars<std::int32_t>(storage, type.info.size);
        else if (type.scalar_leaf->info.id == boolean)
            initialize_scalars<bool>(storage, type.info.size);
        else
            initialize_scalars<std::uint32_t>(storage, type.info.size);
    }

    // Native constructors own their subobjects. Described frames track only completed
    // fields, so a failed child is never destroyed twice during reverse unwinding.
    void construct(const Type& type, void* storage, const void* source = nullptr) {
        if (type.native.construct) {
            if (source)
                type.native.copy_construct(storage, source);
            else
                type.native.construct(storage);
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
                    if (walk_stack.size() != scope.base)
                        ++walk_stack.back().next_field;
                    continue;
                }
                const auto& field = frame.type->fields[frame.next_field];
                auto* destination = frame.storage + field.offset;
                const auto* origin = frame.source ? frame.source + field.offset : nullptr;
                if (field.value.form == ValueForm::inline_value) {
                    if (field.type->native.construct) {
                        if (origin)
                            field.type->native.copy_construct(destination, origin);
                        else
                            field.type->native.construct(destination);
                    } else if (field.type->scalar_leaf) {
                        construct_scalar_block(*field.type, destination, origin);
                    } else {
                        walk_stack.push_back({field.type, destination, origin});
                        continue;
                    }
                    if (!origin) {
                        auto* leaf = destination;
                        for (const auto& value : field.default_leaves)
                            std::visit(
                                [&](const auto& scalar) {
                                    using T = std::remove_cvref_t<decltype(scalar)>;
                                    if constexpr (!std::same_as<T, EnumValue>) {
                                        std::memcpy(leaf, &scalar, sizeof(T));
                                        leaf += sizeof(T);
                                    }
                                },
                                value);
                    }
                } else if (field.value.form == ValueForm::strong_sequence) {
                    std::construct_at(reinterpret_cast<ReferenceSequence*>(destination));
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
                } else if (field.value.form == ValueForm::strong_sequence) {
                    std::destroy_at(reinterpret_cast<ReferenceSequence*>(frame.storage + field.offset));
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
                if (field.type->native.construct)
                    field.type->native.copy_assign(destination, origin);
                else if (field.type->scalar_leaf)
                    std::memcpy(destination, origin, field.type->info.size);
                else
                    walk_stack.push_back({field.type, destination, origin});
            } else {
                *reinterpret_cast<ReferenceSlot*>(destination) = *reinterpret_cast<const ReferenceSlot*>(origin);
            }
        }
    }

    void destroy(const Type& type, void* storage) noexcept {
        if (!type.needs_destroy)
            return;
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
            if (field.value.form == ValueForm::strong_sequence)
                std::destroy_at(reinterpret_cast<ReferenceSequence*>(address));
            else if (field.type->native.destroy)
                field.type->native.destroy(address);
            else
                walk_stack.push_back({field.type, address, nullptr, field.type->destroy_fields.size()});
        }
    }

    Result<void> trace(const Type& type, const void* storage, ReferenceVisitor visitor) {
        if (!type.info.contains_references)
            return {};
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
                if (field.value.form == ValueForm::strong_sequence) {
                    for (const auto object : static_cast<const ReferenceSequence*>(address)->objects_) {
                        if (object == ObjectId{})
                            continue;
                        const auto result = visitor.visit(
                            visitor.context, ObjectReference{{field.value.type, ValueForm::strong_reference}, object});
                        if (!result)
                            return result;
                    }
                    continue;
                }
                const auto object = static_cast<const ReferenceSlot*>(address)->object_;
                if (object == ObjectId{})
                    continue;
                const auto result = visitor.visit(visitor.context, ObjectReference{field.value, object});
                if (!result)
                    return result;
            }
        }
        return {};
    }

    template <class F>
    Result<void> visit_guarded(ReferenceVisitor visitor, F&& enumerate) {
        if (!visitor.visit)
            return failure(ErrorCode::argument_mismatch, "Reference traversal requires a visitor.");
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
        if (trace_owner == TraceOwner::object && trace_owner_index == index)
            cancel_cursor();
        if (is_live(slot)) {
            --live;
            if (phase != CollectionPhase::idle && slot.mark_epoch == epoch)
                --marked;
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
    if (impl_->context == 0)
        throw std::overflow_error("Runtime identity space exhausted.");
    impl_->f32 = impl_->add_scalar<float>("core.f32");
    impl_->u32 = impl_->add_scalar<std::uint32_t>("core.u32");
    impl_->i32 = impl_->add_scalar<std::int32_t>("core.i32");
    impl_->f64 = impl_->add_scalar<double>("core.f64");
    static_assert(sizeof(bool) == 1 && alignof(bool) == 1);
    impl_->boolean = impl_->add_scalar<bool>("core.bool");
    impl_->object = TypeId{impl_->context, static_cast<std::uint32_t>(impl_->types.size())};
    auto root = std::make_unique<Impl::Type>();
    root->info = {impl_->object, "core.Object", TypeKind::object, 1, 1, {}, {}};
    impl_->names.try_emplace(StringView{std::string_view{root->info.name}}, impl_->object.index_);
    impl_->types.push_back(std::move(root));
    const auto registered = register_types(math::type_definitions(*this));
    if (!registered)
        throw std::logic_error(registered.error().message);
    const auto classify = [&]<class T>() {
        static_assert(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>);
        constexpr auto descriptor = math::type_of<T>;
        const auto identity = native_type<T>();
        if (!identity)
            throw std::logic_error("Builtin math type has no native identity.");
        auto& info = impl_->lookup(*identity)->info;
        if (info.kind != TypeKind::record || info.contains_references || !info.copy_constructible ||
            !info.copy_assignable || info.size != descriptor.size() || info.alignment != descriptor.scalar_size() ||
            info.fields.size() != descriptor.components())
            throw std::logic_error("Builtin math layout does not match its execution description.");
        const auto scalar = descriptor.precision == math::Precision::f64 ? impl_->f64 : impl_->f32;
        for (std::size_t index = 0; index < info.fields.size(); ++index) {
            const auto& field = info.fields[index];
            if (field.value != ValueDescription{scalar} || field.offset != index * descriptor.scalar_size())
                throw std::logic_error("Builtin math components do not match their native layout.");
        }
        info.math = descriptor;
    };
    classify.template operator()<math::Vector2>();
    classify.template operator()<math::Vector3>();
    classify.template operator()<math::Vector4>();
    classify.template operator()<math::Quaternion>();
    classify.template operator()<math::Matrix4>();
    classify.template operator()<math::Vector2d>();
    classify.template operator()<math::Vector3d>();
    classify.template operator()<math::Vector4d>();
    classify.template operator()<math::Quaterniond>();
    classify.template operator()<math::Matrix4d>();
}

Result<void> Runtime::check_execution_entry() const {
    if (!impl_->roots->alive || impl_->callbacks != 0 || impl_->roots->traversals != 0)
        return failure(ErrorCode::busy,
                       "Generated execution cannot enter during a native borrow, traversal or shutdown.");
    return {};
}

Result<void> Runtime::enter_execution() {
    auto ready = check_execution_entry();
    if (!ready)
        return ready;
    if (impl_->active_executions == std::numeric_limits<std::size_t>::max())
        return failure(ErrorCode::capacity_exceeded, "Execution nesting space exhausted.");
    ++impl_->active_executions;
    return {};
}

void Runtime::leave_execution() noexcept {
    --impl_->active_executions;
}

Result<void> Runtime::enter_execution_inspection() {
    auto ready = check_execution_entry();
    if (!ready)
        return ready;
    if (impl_->active_executions == 0)
        return failure(ErrorCode::busy, "Execution inspection requires an active generated invocation.");
    // Share reference traversal's read-only admission, including root copying
    // through RootState. This is not a new execution or a native storage borrow.
    ++impl_->callbacks;
    ++impl_->roots->traversals;
    return {};
}

void Runtime::leave_execution_inspection() noexcept {
    --impl_->roots->traversals;
    --impl_->callbacks;
}

Result<void> Runtime::check_code_publication() const {
    auto ready = check_execution_entry();
    if (!ready)
        return ready;
    if (impl_->active_executions != 0)
        return failure(ErrorCode::busy, "Code publication requires all generated invocations to return.");
    return {};
}

Result<void> Runtime::begin_collection() {
    if (impl_->callbacks != 0 || impl_->active_executions != 0 || !impl_->roots->alive ||
        impl_->phase != CollectionPhase::idle) {
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
    if (impl_->callbacks != 0 || impl_->active_executions != 0 || !impl_->roots->alive) {
        return failure(ErrorCode::busy, "Collection steps require a host safe point.");
    }
    if (budget.max_work == 0 || budget.max_time < std::chrono::nanoseconds::zero()) {
        return failure(ErrorCode::argument_mismatch, "Collection requires positive work and nonnegative time limits.");
    }
    const bool timed = budget.max_time != std::chrono::nanoseconds::zero();
    const auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    CollectionProgress progress;
    CallbackScope scope{impl_->callbacks};
    while (progress.work_done != budget.max_work && impl_->phase != CollectionPhase::idle) {
        if (progress.work_done != 0 && timed && std::chrono::steady_clock::now() - started >= budget.max_time)
            break;
        progress.reclaimed += impl_->collect_one();
        ++progress.work_done;
    }
    progress.phase = impl_->phase;
    progress.pending_reclamation = impl_->pending;
    return progress;
}

CollectionPhase Runtime::collection_phase() const noexcept {
    return impl_->phase;
}
std::size_t Runtime::pending_reclamation() const noexcept {
    return impl_->pending;
}

Runtime::~Runtime() {
    impl_->roots->alive = false;
    impl_->roots->observer = nullptr;
    impl_->roots->added = nullptr;
    impl_->roots->removing = nullptr;
    CallbackScope scope{impl_->callbacks};
    for (auto& slot : impl_->slots) {
        if (slot.storage)
            impl_->release(slot);
    }
}

Result<TypeId> Runtime::register_type(TypeDefinition definition) {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(std::move(definition));
    auto published = register_types(std::move(definitions));
    if (!published)
        return std::unexpected(std::move(published.error()));
    return published->front();
}

Result<FunctionSignature> Runtime::resolve_signature(const SignatureDefinition& definition) const {
    return impl_->resolve_signature(definition);
}

struct detail::TypePublication::State {
    std::uint64_t context{};
    std::size_t first_index{};
    std::vector<std::unique_ptr<Runtime::Impl::Type>> staged;
    std::vector<const TypeInfo*> catalog;
    bool reserved{};
    bool committed{};
};
detail::TypePublication::TypePublication() : state_(std::make_unique<State>()) {}
detail::TypePublication::~TypePublication() = default;
Result<std::shared_ptr<detail::TypePublication>>
detail::TypePublication::prepare(Runtime& runtime, std::vector<TypeDefinition> definitions) {
    return runtime.prepare_types(std::move(definitions));
}
std::span<const TypeInfo* const> detail::TypePublication::catalog() const noexcept {
    return state_->catalog;
}
Result<FunctionSignature> detail::TypePublication::resolve_signature(Runtime& runtime,
                                                                     const SignatureDefinition& definition) const {
    if (state_->context != runtime.impl_->context || state_->committed ||
        state_->first_index != runtime.impl_->types.size())
        return failure(ErrorCode::invalid_definition, "Type candidate no longer matches its Runtime catalog.");
    return runtime.impl_->resolve_signature(definition, std::nullopt, state_->staged);
}
Result<void> detail::TypePublication::reserve_commit(Runtime& runtime) {
    auto& impl = *runtime.impl_;
    if (state_->context != impl.context || state_->committed || state_->first_index != impl.types.size())
        return failure(ErrorCode::invalid_definition, "Type candidate no longer matches its Runtime catalog.");
    // Type safe point: code-publication quiescence plus an idle collector, whose
    // trace stack was reserved for the depths known when its cycle began.
    if (impl.phase != CollectionPhase::idle || !impl.roots->alive || impl.callbacks != 0 ||
        impl.roots->traversals != 0 || impl.active_executions != 0)
        return failure(
            ErrorCode::busy,
            "Type publication requires a host safe point without collection, callbacks or generated execution.");
    std::size_t walk_depth = 1;
    std::size_t representations = impl.representations.size();
    for (const auto& type : state_->staged) {
        walk_depth = std::max(walk_depth, type->walk_depth);
        if (type->native.representation)
            ++representations;
    }
    impl.walk_stack.reserve(walk_depth);
    impl.types.reserve(state_->first_index + state_->staged.size());
    impl.representations.reserve(representations);
    impl.names.reserve(state_->first_index + state_->staged.size());
    state_->reserved = true;
    return {};
}
void detail::TypePublication::commit(Runtime& runtime) noexcept {
    auto& impl = *runtime.impl_;
    if (!state_->reserved || state_->committed || state_->context != impl.context ||
        state_->first_index != impl.types.size())
        std::terminate();
    for (auto& type : state_->staged) {
        const auto index = static_cast<std::uint32_t>(impl.types.size());
        if (!type->trace_fields.empty())
            impl.maximum_trace_depth = std::max(impl.maximum_trace_depth, type->trace_depth);
        if (type->native.representation)
            impl.representations.try_emplace(type->native.representation, index);
        impl.names.try_emplace(StringView{std::string_view{type->info.name}}, index);
        impl.types.push_back(std::move(type));
    }
    state_->staged.clear();
    state_->committed = true;
}
Result<std::vector<TypeId>> Runtime::register_types(std::vector<TypeDefinition> definitions) {
    auto publication = prepare_types(std::move(definitions));
    if (!publication)
        return std::unexpected(std::move(publication.error()));
    std::vector<TypeId> result;
    for (const auto& type : (*publication)->state_->staged)
        result.push_back(type->info.id);
    auto ready = (*publication)->reserve_commit(*this);
    if (!ready)
        return std::unexpected(std::move(ready.error()));
    (*publication)->commit(*this);
    return result;
}

Result<std::shared_ptr<detail::TypePublication>> Runtime::prepare_types(std::vector<TypeDefinition> definitions) {
    // Preparation only reads the published catalog; reserve_commit checks the safe point.
    if (definitions.empty()) {
        return failure(ErrorCode::invalid_definition, "Type publication requires a non-empty batch.");
    }
    if (definitions.size() > no_slot - impl_->types.size()) {
        return failure(ErrorCode::capacity_exceeded, "Type identity space exhausted.");
    }

    const auto first_index = impl_->types.size();
    auto publication = std::shared_ptr<detail::TypePublication>(new detail::TypePublication);
    publication->state_->context = impl_->context;
    publication->state_->first_index = first_index;
    auto& staged = publication->state_->staged;
    staged.reserve(definitions.size());
    for (auto& definition : definitions) {
        if (definition.name.empty() || (definition.kind != TypeKind::record && definition.kind != TypeKind::object &&
                                        definition.kind != TypeKind::enumeration &&
                                        definition.kind != TypeKind::string && definition.kind != TypeKind::array)) {
            return failure(ErrorCode::invalid_definition,
                           "A named record, object, enum, string or array type is required.");
        }
        const auto invalid_description = [](const std::string& owner) {
            return failure(ErrorCode::invalid_definition,
                           "Description requires valid UTF-8 without controls other than line breaks and tabs: " +
                               owner);
        };
        if (!normalize_description(definition.description))
            return invalid_description(definition.name);
        for (auto& field : definition.fields)
            if (!normalize_description(field.description))
                return invalid_description(definition.name + "." + field.name);
        for (auto& method : definition.methods)
            if (!normalize_description(method.description))
                return invalid_description(definition.name + "." + method.name);
        const auto same_name = [&](const auto& type) { return type->info.name == definition.name; };
        if (impl_->named(definition.name) || std::ranges::any_of(staged, same_name)) {
            return failure(ErrorCode::duplicate_name, "Type already defined: " + definition.name);
        }
        const auto& native = definition.native;
        if (native.representation) {
            const auto same_representation = [&](const auto& type) {
                return type->native.representation == native.representation;
            };
            if (impl_->representations.contains(native.representation) ||
                std::ranges::any_of(staged, same_representation)) {
                return failure(ErrorCode::invalid_definition, "C++ representation already has a type definition.");
            }
        }
        if (definition.fields.size() >= no_slot || definition.methods.size() >= no_slot) {
            return failure(ErrorCode::capacity_exceeded, "Member identity space exhausted.");
        }
        const bool is_native = native.size || native.alignment || native.representation || native.construct ||
                               native.destroy || native.copy_construct || native.copy_assign || native.swap ||
                               native.base_address || native.element_representation || native.sequence_view ||
                               native.sequence_mutate || native.value_operations_preserve_borrows || native.plain_value;
        if (is_native && (!native.size || !std::has_single_bit(native.alignment) || !native.representation ||
                          !native.construct || !native.destroy || native.size % native.alignment != 0)) {
            return failure(ErrorCode::invalid_definition,
                           "Native layout requires valid size, alignment and lifecycle operations.");
        }
        if (definition.kind != TypeKind::object && !definition.methods.empty()) {
            return failure(ErrorCode::invalid_definition, "This slice supports methods on object types only.");
        }
        if (definition.kind != TypeKind::object && (definition.base || native.base_address)) {
            return failure(ErrorCode::invalid_definition, "Value types cannot declare an object base.");
        }
        if ((definition.kind == TypeKind::enumeration) != definition.enumeration.has_value())
            return failure(ErrorCode::invalid_definition, "Enumeration metadata must match its type kind.");
        if (definition.kind == TypeKind::enumeration &&
            (!definition.fields.empty() || !definition.methods.empty() || definition.base || native.base_address ||
             (is_native &&
              (native.size != 4 || native.alignment != 4 || !native.copy_construct || !native.copy_assign))))
            return failure(ErrorCode::invalid_definition,
                           "Enums require a 32-bit value layout and cannot declare members or bases.");
        const bool array = definition.kind == TypeKind::array;
        const bool string = definition.kind == TypeKind::string;
        if (array != definition.element.has_value() || array != (native.element_representation != nullptr))
            return failure(ErrorCode::invalid_definition,
                           "Array types require their exact native element representation.");
        if (array && !native.sequence_view)
            return failure(ErrorCode::invalid_definition,
                           "Array types require an element projection for element paths.");
        if ((native.sequence_view || native.sequence_mutate) && !array && !string)
            return failure(ErrorCode::invalid_definition, "Only Array/String types can project sequence storage.");
        if ((array || string) &&
            (!is_native || !native.copy_construct || !native.copy_assign || !definition.fields.empty()))
            return failure(ErrorCode::invalid_definition,
                           "Foundation values require native ownership operations and no inline fields.");
        if (string && native.representation != detail::representation<String>())
            return failure(ErrorCode::invalid_definition,
                           "String types require the engine UTF-8 String representation.");
        if (definition.validate && definition.kind != TypeKind::record && definition.kind != TypeKind::object)
            return failure(ErrorCode::invalid_definition, "Type validators require a record or object type.");
        const TypeId id{impl_->context, static_cast<std::uint32_t>(first_index + staged.size())};
        auto type = std::make_unique<Impl::Type>();
        type->info = {
            id, std::move(definition.name), definition.kind, native.size, is_native ? native.alignment : 1, {}, {}};
        type->info.description = std::move(definition.description);
        type->validate = definition.validate;
        if (definition.kind != TypeKind::object) {
            type->info.copy_constructible = !is_native || native.copy_construct;
            type->info.copy_assignable = !is_native || native.copy_assign;
        }
        type->info.plain_value = definition.kind == TypeKind::enumeration ||
                                 (definition.kind == TypeKind::record &&
                                  (!is_native || (native.plain_value && native.copy_construct && native.copy_assign)));
        type->native = native;
        type->info.native_storage = is_native;
        type->info.sequence_view = native.sequence_view;
        type->info.sequence_mutate = native.sequence_mutate;
        type->info.value_operations_preserve_borrows = native.value_operations_preserve_borrows;
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
        if (definition.enumeration) {
            const auto* underlying = resolve(definition.enumeration->underlying);
            if (!underlying || (underlying->info.id != impl_->i32 && underlying->info.id != impl_->u32))
                return failure(ErrorCode::invalid_definition, "Enum underlying type must be core.i32 or core.u32.");
            const auto& values = definition.enumeration->values;
            for (std::size_t e = 0; e < values.size(); ++e) {
                if (values[e].first.empty())
                    return failure(ErrorCode::invalid_definition, "Enum names cannot be empty.");
                for (std::size_t j = 0; j < e; ++j)
                    if (values[j].first == values[e].first)
                        return failure(ErrorCode::duplicate_name, "Duplicate enum name: " + values[e].first);
            }
            type.info.enumeration =
                TypeInfo::Enumeration{underlying->info.id, std::move(definition.enumeration->values)};
            type.info.size = 4;
            type.info.alignment = 4;
            if (!type.native.construct) {
                type.native = underlying->native;
                type.native.representation = nullptr;
            }
        }
        if (definition.element) {
            const auto* element = resolve(*definition.element);
            if (!element || element->info.kind == TypeKind::object ||
                element->native.representation != type.native.element_representation)
                return failure(ErrorCode::invalid_definition,
                               "Array elements must resolve to their registered native value representation.");
            type.info.element = element->info.id;
        }
        const bool is_native = type.native.construct != nullptr;
        if (type.info.kind == TypeKind::object) {
            type.base = definition.base ? resolve(*definition.base) : impl_->lookup(impl_->object);
            if (!type.base || type.base->info.kind != TypeKind::object)
                return failure(ErrorCode::invalid_definition, "Object base must resolve to an object type.");
            type.info.base = type.base->info.id;
            if (is_native && type.base->info.id != impl_->object &&
                (!type.base->native.construct || !type.native.base_address)) {
                return failure(ErrorCode::invalid_definition,
                               "Native inheritance requires a native base and its address adapter.");
            }
            if (type.base->info.id == impl_->object && type.native.base_address)
                return failure(ErrorCode::invalid_definition,
                               "The storage-free Object root has no native base adapter.");
            if (type.base->info.id != impl_->object)
                type.fields.push_back({0, type.native.base_address, {*type.info.base}});
        }
        type.info.fields.reserve(definition.fields.size());
        type.fields.reserve(type.fields.size() + definition.fields.size());
        // Validate names before moving any strings, including non-exposed fields.
        for (std::size_t field_index = 0; field_index != definition.fields.size(); ++field_index) {
            const auto& name = definition.fields[field_index].name;
            if (name.empty())
                return failure(ErrorCode::invalid_definition, "Field name must not be empty.");
            for (std::size_t previous = 0; previous != field_index; ++previous) {
                if (definition.fields[previous].name == name)
                    return failure(ErrorCode::duplicate_name, "Duplicate field: " + name);
            }
        }
        for (std::size_t method_index = 0; method_index != definition.methods.size(); ++method_index) {
            const auto& name = definition.methods[method_index].name;
            for (const auto& field : definition.fields) {
                if (field.name == name)
                    return failure(ErrorCode::duplicate_name, "Duplicate member: " + name);
            }
            for (std::size_t previous = 0; previous != method_index; ++previous) {
                if (definition.methods[previous].name == name)
                    return failure(ErrorCode::duplicate_name, "Duplicate method: " + name);
            }
        }
        for (auto& field : definition.fields) {
            type.member_names.push_back(field.name);
            const auto* target = resolve(field.value.type);
            if (!target) {
                const auto* named = std::get_if<std::string>(&field.value.type.target_);
                return failure(ErrorCode::invalid_definition, "Unresolved field type" +
                                                                  (named ? " '" + *named + "'" : std::string{}) + ": " +
                                                                  type.info.name + "." + field.name);
            }
            const auto form = field.value.form;
            if (form == ValueForm::inline_value) {
                if (target->info.kind == TypeKind::object) {
                    return failure(ErrorCode::invalid_definition, "Objects cannot be stored as inline values.");
                }
            } else if (form == ValueForm::strong_reference || form == ValueForm::weak_reference ||
                       form == ValueForm::strong_sequence) {
                if (target->info.kind != TypeKind::object) {
                    return failure(ErrorCode::invalid_definition, "Object references require an object target type.");
                }
                if (form == ValueForm::strong_sequence && type.info.kind != TypeKind::object)
                    return failure(ErrorCode::invalid_definition,
                                   "Reference sequences require an object field, not a record value.");
            } else {
                return failure(ErrorCode::invalid_definition, "Unknown value form.");
            }
            if (is_native) {
                const auto representation = form == ValueForm::inline_value ? target->native.representation
                                            : form == ValueForm::strong_sequence
                                                ? detail::representation<ReferenceSequence>()
                                                : detail::representation<ReferenceSlot>();
                if (!field.native_address || !field.native_representation ||
                    field.native_representation != representation) {
                    return failure(ErrorCode::invalid_definition,
                                   "Native member representation does not match its value description.");
                }
            } else if (field.native_address || field.native_representation || field.native_offset) {
                return failure(ErrorCode::invalid_definition,
                               "Described fields cannot supply native member accessors.");
            }
            const auto index = static_cast<std::uint32_t>(type.fields.size());
            const ValueDescription value{target->info.id, form};
            auto published_leaves = field.default_leaves;
            if (!field.default_leaves.empty()) {
                TypeId leaf{};
                std::size_t components = 1;
                if (target->info.kind == TypeKind::scalar)
                    leaf = target->info.id;
                else if (target->info.kind == TypeKind::enumeration) {
                    // A later batch entry has not resolved its underlying type yet.
                    const auto* underlying =
                        target->info.enumeration
                            ? resolved_type(target->info.enumeration->underlying)
                            : resolve(definitions[target->info.id.index_ - first_index].enumeration->underlying);
                    if (underlying && (underlying->info.id == impl_->i32 || underlying->info.id == impl_->u32))
                        leaf = underlying->info.id;
                } else if (target->info.math.vector()) {
                    leaf = target->info.math.precision == math::Precision::f64 ? impl_->f64 : impl_->f32;
                    components = target->info.math.components();
                }
                if (is_native || type.info.kind != TypeKind::object || form != ValueForm::inline_value ||
                    leaf == TypeId{} || field.default_leaves.size() != components ||
                    std::ranges::any_of(field.default_leaves, [&](const Scalar& item) {
                        return std::holds_alternative<EnumValue>(item) || impl_->scalar_type(item) != leaf;
                    }))
                    return failure(ErrorCode::invalid_definition,
                                   "Default leaves require a described object's scalar, enum or math vector "
                                   "field, with one leaf of the exact scalar or underlying type per component: " +
                                       type.info.name + "." + field.name);
                if (std::ranges::any_of(field.default_leaves,
                                        [](const Scalar& item) { return !std::isfinite(range_value(item)); }))
                    return failure(ErrorCode::invalid_definition,
                                   "Default leaves must be finite: " + type.info.name + "." + field.name);
                if (target->info.kind == TypeKind::enumeration) {
                    const auto& bits = field.default_leaves.front();
                    published_leaves = {
                        EnumValue{target->info.id, std::holds_alternative<std::int32_t>(bits)
                                                       ? std::bit_cast<std::uint32_t>(std::get<std::int32_t>(bits))
                                                       : std::get<std::uint32_t>(bits)}};
                }
            }
            if (const auto& range = field.range; range.ui_min || range.ui_max || range.hard_min || range.hard_max) {
                const auto invalid = [&](std::string_view reason) {
                    return failure(ErrorCode::invalid_definition,
                                   "Field range " + std::string(reason) + ": " + type.info.name + "." + field.name);
                };
                // An Array range bounds each element; later batch entries have not resolved their element yet.
                const auto* bounded = target;
                if (form == ValueForm::inline_value && target->info.kind == TypeKind::array)
                    bounded = target->info.element
                                  ? resolved_type(*target->info.element)
                                  : resolve(*definitions[target->info.id.index_ - first_index].element);
                if (!bounded)
                    return invalid("requires an f32, f64, i32, u32 or math vector field, or an Array of them");
                const auto& vector = bounded->info.math;
                const auto leaf = form != ValueForm::inline_value          ? TypeId{}
                                  : bounded->info.kind == TypeKind::scalar ? bounded->info.id
                                  : vector.vector()
                                      ? (vector.precision == math::Precision::f64 ? impl_->f64 : impl_->f32)
                                      : TypeId{};
                if (leaf != impl_->f32 && leaf != impl_->f64 && leaf != impl_->i32 && leaf != impl_->u32)
                    return invalid("requires an f32, f64, i32, u32 or math vector field, or an Array of them");
                for (const auto* bound : {&range.ui_min, &range.ui_max, &range.hard_min, &range.hard_max})
                    if (*bound && (impl_->scalar_type(**bound) != leaf || !std::isfinite(range_value(**bound))))
                        return invalid("bounds must be finite values of the field's leaf scalar type");
                const auto above = [](const std::optional<Scalar>& low, const std::optional<Scalar>& high) {
                    return low && high && range_value(*high) < range_value(*low);
                };
                if (above(range.ui_min, range.ui_max) || above(range.hard_min, range.hard_max))
                    return invalid("minimum exceeds maximum");
                for (const auto* ui : {&range.ui_min, &range.ui_max})
                    if (above(range.hard_min, *ui) || above(*ui, range.hard_max))
                        return invalid("UI bounds must lie within hard bounds");
                if (range.constrains())
                    for (const auto& leaf : field.default_leaves) {
                        const std::optional<Scalar> item{leaf};
                        if (above(range.hard_min, item) || above(item, range.hard_max))
                            return invalid("default lies outside hard bounds");
                    }
            }
            type.fields.push_back(
                {0, field.native_address, value, nullptr, std::move(field.default_leaves),
                 field.range.constrains() ? std::make_unique<const FieldRange>(field.range) : nullptr});
            if (field.exposed)
                type.info.fields.push_back({FieldId{id, index}, std::move(field.name), value, field.native_offset,
                                            std::move(published_leaves), std::move(field.label),
                                            std::move(field.category), std::move(field.description), field.range});
        }
        type.info.methods.reserve(definition.methods.size());
        type.methods.reserve(definition.methods.size());
        for (auto& method : definition.methods) {
            type.member_names.push_back(method.name);
            if (method.name.empty() || !method.invoke) {
                return failure(ErrorCode::invalid_definition, "Methods require a name and implementation.");
            }
            auto signature = impl_->resolve_signature(method.signature, id, staged);
            if (!signature)
                return std::unexpected(std::move(signature.error()));
            auto& names = method.parameter_names;
            if (!names.empty() && names.size() != signature->parameters.size())
                return failure(ErrorCode::invalid_definition,
                               "Parameter names must match the parameters of method: " + method.name);
            for (std::size_t i = 0; i < names.size(); ++i)
                if (!names[i].empty() && std::find(names.begin(), names.begin() + i, names[i]) != names.begin() + i)
                    return failure(ErrorCode::duplicate_name,
                                   "Duplicate parameter " + names[i] + " in method: " + method.name);
            names.resize(signature->parameters.size());
            type.info.methods.push_back({MethodId{id, static_cast<std::uint32_t>(type.info.methods.size())},
                                         std::move(method.name), std::move(*signature), method.preserves_borrows,
                                         std::move(method.description), std::move(names)});
            type.methods.push_back(method.invoke);
        }
    }

    // Inline values and the private base segment constrain layout. Reject cycles without
    // consuming the C++ call stack for deeply nested, but valid, definition batches.
    enum class LayoutState { unseen, active, complete };
    struct LayoutFrame {
        std::size_t type;
        std::size_t next_field;
    };
    std::vector<LayoutState> states(staged.size(), LayoutState::unseen);
    std::vector<LayoutFrame> stack;
    for (std::size_t start = 0; start != staged.size(); ++start) {
        if (states[start] == LayoutState::complete)
            continue;
        states[start] = LayoutState::active;
        stack.push_back({start, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            auto& type = *staged[frame.type];
            bool descended = false;
            while (frame.next_field != type.fields.size() + (type.info.element ? 1u : 0u)) {
                const auto edge = frame.next_field++;
                const auto value =
                    edge < type.fields.size() ? type.fields[edge].value : ValueDescription{*type.info.element};
                if (value.form != ValueForm::inline_value || value.type.index_ < first_index)
                    continue;
                const auto dependency = value.type.index_ - first_index;
                if (states[dependency] == LayoutState::active) {
                    return failure(ErrorCode::invalid_definition,
                                   "Inline layout cycle at type: " + staged[dependency]->info.name);
                }
                if (states[dependency] == LayoutState::unseen) {
                    states[dependency] = LayoutState::active;
                    stack.push_back({dependency, 0});
                    descended = true;
                    break;
                }
            }
            if (descended)
                continue;
            const bool is_native = type.native.construct != nullptr;
            if (type.base) {
                for (const auto* ancestor = type.base; ancestor; ancestor = ancestor->base) {
                    for (const auto& name : type.member_names) {
                        if (std::ranges::find(ancestor->member_names, name) != ancestor->member_names.end())
                            return failure(ErrorCode::invalid_definition,
                                           "Member hides an inherited declaration: " + type.info.name + "." + name +
                                               " hides " + ancestor->info.name + "." + name);
                    }
                }
                type.info.fields.insert(type.info.fields.begin(), type.base->info.fields.begin(),
                                        type.base->info.fields.end());
                type.info.methods.insert(type.info.methods.begin(), type.base->info.methods.begin(),
                                         type.base->info.methods.end());
            }
            if (type.info.element) {
                const auto* element_type = resolved_type(*type.info.element);
                const auto& element = element_type->info;
                type.info.value_operations_preserve_borrows &= element.kind == TypeKind::scalar ||
                                                               element.kind == TypeKind::enumeration ||
                                                               element.value_operations_preserve_borrows;
                if (element.contains_references || !element.copy_constructible)
                    return failure(
                        ErrorCode::invalid_definition,
                        "Reflected Array elements must be copyable values without GC references; use ReferenceSequence for object references.");
                // The element layout is complete; its rules apply to every element.
                if (!element_type->constrained_leaves.empty())
                    type.constrained_leaves.push_back({{}, nullptr, {}, &type});
                if (!element_type->validators.empty())
                    type.validators.push_back({{}, {}, nullptr, &type});
            }
            type.needs_destroy = is_native;
            if (!is_native && type.info.kind == TypeKind::record)
                type.info.value_operations_preserve_borrows = true;
            for (std::size_t field_index = 0; field_index != type.fields.size(); ++field_index) {
                const auto value = type.fields[field_index].value;
                const auto* target = resolved_type(value.type);
                type.fields[field_index].type = target;
                const bool reference = value.form != ValueForm::inline_value;
                if (!reference && (type.info.kind == TypeKind::record || type.info.kind == TypeKind::object)) {
                    const auto index = static_cast<std::uint32_t>(field_index);
                    // The base segment carries inherited fields under their declaring FieldIds.
                    const bool base_segment = field_index == 0 && type.base && type.base->info.id != impl_->object;
                    const auto reported = [&](const std::vector<FieldId>& nested) {
                        std::vector<FieldId> field;
                        if (!base_segment)
                            field.push_back(FieldId{type.info.id, index});
                        field.insert(field.end(), nested.begin(), nested.end());
                        return field;
                    };
                    const auto stored = [&](const std::vector<std::uint32_t>& nested) {
                        std::vector<std::uint32_t> path{index};
                        path.insert(path.end(), nested.begin(), nested.end());
                        return path;
                    };
                    if (const auto* range = type.fields[field_index].hard.get()) {
                        if (target->info.kind == TypeKind::scalar)
                            type.constrained_leaves.push_back({{index}, range, reported({})});
                        else if (target->info.kind == TypeKind::array)
                            type.constrained_leaves.push_back({{index}, range, reported({}), target});
                        else
                            for (std::uint32_t component = 0; component != target->info.math.components(); ++component)
                                type.constrained_leaves.push_back(
                                    {{index, component}, range, reported({FieldId{target->info.id, component}})});
                    } else
                        for (const auto& leaf : target->constrained_leaves)
                            type.constrained_leaves.push_back(
                                {stored(leaf.path), leaf.range, reported(leaf.field), leaf.array});
                    for (const auto& step : target->validators)
                        type.validators.push_back({stored(step.path), reported(step.field), step.validate, step.array});
                }
                if (!is_native && type.info.kind == TypeKind::record)
                    type.info.value_operations_preserve_borrows &=
                        !reference &&
                        (target->info.kind == TypeKind::scalar || target->info.kind == TypeKind::enumeration ||
                         target->info.value_operations_preserve_borrows);
                type.info.contains_references |= reference || target->info.contains_references;
                type.info.plain_value &=
                    !reference && (target->info.kind == TypeKind::scalar ||
                                   target->info.kind == TypeKind::enumeration || target->info.plain_value);
                if (!reference) {
                    type.walk_depth = std::max(type.walk_depth, target->walk_depth + 1);
                    if (!is_native && target->needs_destroy) {
                        type.destroy_fields.push_back(static_cast<std::uint32_t>(field_index));
                        type.needs_destroy = true;
                    }
                }
                if (!is_native && value.form == ValueForm::strong_sequence) {
                    type.destroy_fields.push_back(static_cast<std::uint32_t>(field_index));
                    type.needs_destroy = true;
                }
                if (value.form == ValueForm::strong_reference || value.form == ValueForm::strong_sequence ||
                    (!reference && !target->trace_fields.empty())) {
                    type.trace_fields.push_back(static_cast<std::uint32_t>(field_index));
                    if (!reference)
                        type.trace_depth = std::max(type.trace_depth, target->trace_depth + 1);
                }
                if (!is_native) {
                    const auto alignment = value.form == ValueForm::strong_sequence ? alignof(ReferenceSequence)
                                           : reference                              ? alignof(ReferenceSlot)
                                                                                    : target->info.alignment;
                    const auto size = value.form == ValueForm::strong_sequence ? sizeof(ReferenceSequence)
                                      : reference                              ? sizeof(ReferenceSlot)
                                                                               : target->info.size;
                    const auto offset = aligned_size(type.info.size, alignment);
                    if (!offset)
                        return std::unexpected(offset.error());
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
            if (type.validate)
                type.validators.push_back({{}, {}, type.validate});
            // Direct offsets are published only after every dependent layout is complete.
            for (auto& field : type.info.fields) {
                if (field.id.owner_ != type.info.id)
                    continue;
                const auto& storage = type.fields[field.id.index_];
                if (!is_native)
                    field.offset = storage.offset;
                if (!field.offset)
                    continue;
                const bool inline_value = field.value.form == ValueForm::inline_value;
                const auto size = inline_value                                     ? storage.type->info.size
                                  : field.value.form == ValueForm::strong_sequence ? sizeof(ReferenceSequence)
                                                                                   : sizeof(ReferenceSlot);
                const auto alignment = inline_value                                     ? storage.type->info.alignment
                                       : field.value.form == ValueForm::strong_sequence ? alignof(ReferenceSequence)
                                                                                        : alignof(ReferenceSlot);
                if (*field.offset > type.info.size || size > type.info.size - *field.offset ||
                    *field.offset % alignment != 0 || type.info.alignment < alignment)
                    return failure(ErrorCode::invalid_definition,
                                   "Direct field offset exceeds its type layout or alignment.");
            }
            if (!is_native) {
                const auto size = aligned_size(std::max(std::size_t{1}, type.info.size), type.info.alignment);
                if (!size)
                    return std::unexpected(size.error());
                type.info.size = *size;
                if (!type.fields.empty()) {
                    const auto* scalar = type.fields.front().type->scalar_leaf;
                    std::size_t end = 0;
                    for (const auto& field : type.fields) {
                        if (!scalar || !field.default_leaves.empty() || field.value.form != ValueForm::inline_value ||
                            field.type->scalar_leaf != scalar || field.offset != end) {
                            scalar = nullptr;
                            break;
                        }
                        end += field.type->info.size;
                    }
                    if (end == type.info.size)
                        type.scalar_leaf = scalar;
                    if (type.scalar_leaf)
                        type.walk_depth = 1;
                }
            }
            // Trivial values swap their bytes directly; others compose field swaps or use the native one.
            if (type.info.kind != TypeKind::object && !type.info.plain_value && !type.info.contains_references) {
                if (is_native) {
                    if (type.native.swap)
                        type.swap_parts = {{0, type.info.size, type.native.swap}};
                } else {
                    for (const auto& field : type.fields) {
                        const auto& target = *field.type;
                        if (target.info.plain_value || target.info.kind == TypeKind::scalar)
                            type.swap_parts.push_back({field.offset, target.info.size, nullptr});
                        else if (target.swap_parts.empty()) {
                            type.swap_parts.clear();
                            break;
                        } else
                            for (const auto& part : target.swap_parts)
                                type.swap_parts.push_back({field.offset + part.offset, part.size, part.swap});
                    }
                }
            }
            states[frame.type] = LayoutState::complete;
            stack.pop_back();
        }
    }

    // Method-only dependencies may be later in the batch. Recheck the shared
    // resolver after all layouts and their GC-reference closure are complete.
    for (std::size_t i = 0; i < staged.size(); ++i) {
        for (const auto& method : definitions[i].methods) {
            auto signature = impl_->resolve_signature(method.signature, staged[i]->info.id, staged);
            if (!signature)
                return std::unexpected(std::move(signature.error()));
        }
    }
    auto& catalog = publication->state_->catalog;
    catalog.reserve(first_index + staged.size());
    for (const auto& type : impl_->types)
        catalog.push_back(&type->info);
    for (const auto& type : staged)
        catalog.push_back(&type->info);
    return publication;
}

Result<TypeId> Runtime::find_type(std::string_view name) const {
    if (const auto* type = impl_->named(name))
        return type->info.id;
    return failure(ErrorCode::invalid_type, "Unknown type: " + std::string{name});
}

Result<TypeId> Runtime::find_representation(const void* representation) const {
    if (const auto* type = impl_->native(representation))
        return type->info.id;
    return failure(ErrorCode::invalid_type, "Register the native value type before exposing its members.");
}

Result<const TypeInfo*> Runtime::describe(TypeId type) const {
    const auto* found = impl_->lookup(type);
    if (!found)
        return failure(ErrorCode::invalid_type, "Type does not belong to this runtime.");
    return &found->info;
}

std::vector<TypeId> Runtime::types(std::size_t first) const {
    std::vector<TypeId> result;
    if (first >= impl_->types.size())
        return result;
    result.reserve(impl_->types.size() - first);
    for (auto index = first; index < impl_->types.size(); ++index)
        result.push_back(impl_->types[index]->info.id);
    return result;
}
std::size_t Runtime::type_count() const noexcept {
    return impl_->types.size();
}
TypeId Runtime::object_type() const noexcept {
    return impl_->object;
}

Result<bool> Runtime::is_a(TypeId actual, TypeId expected) const {
    const auto* type = impl_->lookup(actual);
    const auto* base = impl_->lookup(expected);
    if (!type || !base || type->info.kind != TypeKind::object || base->info.kind != TypeKind::object)
        return failure(ErrorCode::invalid_type, "Ancestry requires object types in this Runtime.");
    return Impl::derives_from(*type, *base);
}

Result<ObjectReference> Runtime::cast_reference(const ObjectReference& reference, TypeId target) const {
    auto valid = validate_reference(reference);
    if (!valid)
        return std::unexpected(valid.error());
    const auto up = is_a(reference.description().type, target);
    if (!up)
        return std::unexpected(up.error());
    if (!*up) {
        const auto down = is_a(target, reference.description().type);
        if (!down)
            return std::unexpected(down.error());
        if (!*down)
            return failure(ErrorCode::type_mismatch, "Reference types have no ancestor relationship.");
        if (!reference.is_null()) {
            const auto actual = type_of(reference.id());
            if (!actual)
                return std::unexpected(actual.error());
            if (!Impl::derives_from(*impl_->lookup(*actual), *impl_->lookup(target)))
                return failure(ErrorCode::type_mismatch, "Object does not satisfy the reference downcast.");
        }
    }
    return ObjectReference{{target, reference.description().form}, reference.id()};
}

Result<FieldId> Runtime::find_field(TypeId type, std::string_view name) const {
    const auto info = describe(type);
    if (!info)
        return std::unexpected(info.error());
    for (const auto& field : (*info)->fields) {
        if (field.name == name)
            return field.id;
    }
    return failure(ErrorCode::invalid_member, "Unknown field: " + std::string{name});
}

Result<MethodId> Runtime::find_method(TypeId type, std::string_view name) const {
    const auto info = describe(type);
    if (!info)
        return std::unexpected(info.error());
    for (const auto& method : (*info)->methods) {
        if (method.name == name)
            return method.id;
    }
    return failure(ErrorCode::invalid_member, "Unknown method: " + std::string{name});
}

Runtime::NativeBorrow::NativeBorrow(Runtime& owner, void* address) noexcept : runtime(&owner), storage(address) {
    ++runtime->impl_->callbacks;
    ++runtime->impl_->native_borrows;
}
Runtime::NativeBorrow::NativeBorrow(NativeBorrow&& other) noexcept
    : runtime(std::exchange(other.runtime, nullptr)), storage(other.storage) {}
Runtime::NativeBorrow::~NativeBorrow() {
    if (runtime) {
        --runtime->impl_->native_borrows;
        --runtime->impl_->callbacks;
    }
}

Result<Runtime::NativeBorrow> Runtime::borrow_native(ObjectId object, const void* representation,
                                                     std::span<const FieldId> path, bool write) {
    if (!impl_->roots->alive || impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Native borrowing is unavailable during traversal or shutdown.");
    auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    const auto* expected = impl_->native(representation);
    if (!expected)
        return failure(ErrorCode::invalid_type, "Register the native value type before exposing its members.");
    if (!path.empty()) {
        NativeBorrow borrow{*this, nullptr};
        auto field = resolve_field(object, path);
        if (!field)
            return std::unexpected(field.error());
        if (field->value.form != ValueForm::inline_value || field->value.type != expected->info.id)
            return failure(ErrorCode::type_mismatch, "Field does not have the requested native value representation.");
        borrow.storage = field->data;
        if (write)
            impl_->stamp(*index);
        return borrow;
    }
    const auto& slot = impl_->slots[*index];
    const auto* actual = impl_->lookup(slot.type);
    if (expected->info.kind != TypeKind::object || !expected->native.construct)
        return failure(ErrorCode::invalid_type, "Native borrowing requires a registered native object representation.");
    if (!Impl::derives_from(*actual, *expected))
        return failure(ErrorCode::type_mismatch, "Object does not contain the requested native base.");
    NativeBorrow borrow{*this, nullptr};
    borrow.storage = Impl::base_storage(*actual, *expected, slot.storage);
    if (write)
        impl_->stamp(*index);
    return borrow;
}

Result<ReferenceField> Runtime::reference_field(ObjectId object, std::span<const FieldId> path) const {
    auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    if (address->value.form == ValueForm::inline_value)
        return failure(ErrorCode::type_mismatch, "Field is not an object reference or reference sequence.");
    ReferenceField field;
    field.object_ = object;
    field.path_.assign(path.begin(), path.end());
    return field;
}

Result<ReferenceField> Runtime::native_reference_field(ObjectId object, const void* storage, bool sequence) const {
    if (!impl_->native_borrows || impl_->roots->traversals)
        return failure(ErrorCode::busy, "Native field lookup requires a protected native borrow.");
    auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    CallbackScope scope{impl_->callbacks};
    const auto& slot = impl_->slots[*index];
    struct Cursor {
        const Impl::Type* type;
        void* storage;
        std::size_t next{};
        std::size_t depth{};
    };
    std::vector<Cursor> cursors{{impl_->lookup(slot.type), slot.storage}};
    std::vector<FieldId> path;
    while (!cursors.empty()) {
        auto& cursor = cursors.back();
        if (cursor.next == cursor.type->fields.size()) {
            cursors.pop_back();
            continue;
        }
        path.resize(cursor.depth);
        const auto field_index = static_cast<std::uint32_t>(cursor.next++);
        const auto& field = cursor.type->fields[field_index];
        auto* address =
            field.address ? field.address(cursor.storage) : static_cast<std::byte*>(cursor.storage) + field.offset;
        // A base segment changes the declaration owner, not the public path.
        if (field.value.form != ValueForm::inline_value || field.type->info.kind != TypeKind::object)
            path.push_back(FieldId{cursor.type->info.id, field_index});
        if (field.value.form == ValueForm::inline_value) {
            if (field.type->info.contains_references)
                cursors.push_back({field.type, address, 0, path.size()});
        } else if (address == storage && (field.value.form == ValueForm::strong_sequence) == sequence) {
            ReferenceField result;
            result.object_ = object;
            result.path_ = std::move(path);
            result.native_only_ = true;
            return result;
        }
    }
    return failure(ErrorCode::invalid_member, "Native slot is not a matching registered member of this object.");
}

Result<ReferenceField> Runtime::reference_field(ObjectId object, const ReferenceSlot& slot) const {
    return native_reference_field(object, &slot, false);
}
Result<ReferenceField> Runtime::reference_field(ObjectId object, const ReferenceSequence& sequence) const {
    return native_reference_field(object, &sequence, true);
}
Result<Runtime::Address> Runtime::resolve_reference_field(const ReferenceField& field) const {
    if (field.native_only_ && !impl_->native_borrows)
        return failure(ErrorCode::busy, "Native field access outlived its protected borrow.");
    return resolve_field(field.object_, field.path_);
}

Result<ObjectReference> Runtime::read_reference(const ReferenceField& field) const {
    auto address = resolve_reference_field(field);
    if (!address)
        return std::unexpected(address.error());
    return read_reference_at(*address);
}

Result<std::size_t> Runtime::sequence_size(const ReferenceField& field) const {
    const auto address = resolve_reference_field(field);
    if (!address)
        return std::unexpected(address.error());
    if (address->value.form != ValueForm::strong_sequence)
        return failure(ErrorCode::type_mismatch, "Field is not a reference sequence.");
    return static_cast<const ReferenceSequence*>(address->data)->objects_.size();
}
Result<ObjectReference> Runtime::sequence_read(const ReferenceField& field, std::size_t index) const {
    const auto address = resolve_reference_field(field);
    if (!address)
        return std::unexpected(address.error());
    if (address->value.form != ValueForm::strong_sequence)
        return failure(ErrorCode::type_mismatch, "Field is not a reference sequence.");
    const auto& objects = static_cast<const ReferenceSequence*>(address->data)->objects_;
    if (index >= objects.size())
        return failure(ErrorCode::argument_mismatch, "Sequence index is out of range.");
    return ObjectReference{{address->value.type, ValueForm::strong_reference}, objects[index]};
}
Result<void> Runtime::visit_sequence(const ReferenceField& field, ReferenceVisitor visitor) const {
    const auto address = resolve_reference_field(field);
    if (!address)
        return std::unexpected(address.error());
    if (address->value.form != ValueForm::strong_sequence)
        return failure(ErrorCode::type_mismatch, "Field is not a reference sequence.");
    return impl_->visit_guarded(visitor, [&]() -> Result<void> {
        for (const auto object : static_cast<const ReferenceSequence*>(address->data)->objects_) {
            const auto result = visitor.visit(
                visitor.context, ObjectReference{{address->value.type, ValueForm::strong_reference}, object});
            if (!result)
                return result;
        }
        return {};
    });
}

Result<void> Runtime::apply_reference_changes(std::span<const ReferenceChange> changes) {
    if (!impl_->roots->alive || impl_->roots->traversals || impl_->changing_references)
        return failure(ErrorCode::busy, "Reference mutation cannot reenter traversal, another batch or shutdown.");
    CallbackScope scope{impl_->callbacks};
    struct MutationScope {
        bool& active;
        explicit MutationScope(bool& flag) : active(flag) { active = true; }
        ~MutationScope() { active = false; }
    } mutation{impl_->changing_references};
    struct Prepared {
        ObjectId owner;
        Address address;
        ObjectId reference;
        std::size_t first{};
        std::size_t erased{};
        std::size_t final_size{};
        std::vector<ObjectId> inserted;
    };
    std::vector<Prepared> prepared;
    if (changes.size() > prepared.max_size())
        return failure(ErrorCode::capacity_exceeded, "Reference batch is too large.");
    prepared.reserve(changes.size());
    for (const auto& change : changes) {
        const auto& field =
            std::visit([](const auto& operation) -> const ReferenceField& { return operation.field; }, change);
        auto address = resolve_reference_field(field);
        if (!address)
            return std::unexpected(address.error());
        for (const auto& previous : prepared) {
            if (previous.address.data == address->data)
                return failure(ErrorCode::argument_mismatch, "A batch cannot modify the same storage twice.");
        }
        Prepared plan{field.object_, *address, {}, 0, 0, 0, {}};
        if (const auto* write = std::get_if<ReferenceWrite>(&change)) {
            auto valid = validate_reference_assignment(address->value, write->value);
            if (!valid)
                return valid;
            plan.reference = write->value.id();
        } else {
            const auto& splice = std::get<SequenceSplice>(change);
            if (address->value.form != ValueForm::strong_sequence)
                return failure(ErrorCode::type_mismatch, "Splice requires a reference sequence.");
            const auto& sequence = static_cast<ReferenceSequence*>(address->data)->objects_;
            if (splice.first > sequence.size() || splice.erase_count > sequence.size() - splice.first)
                return failure(ErrorCode::argument_mismatch, "Splice range is out of bounds.");
            if (splice.inserted.size() > sequence.max_size() - (sequence.size() - splice.erase_count))
                return failure(ErrorCode::capacity_exceeded, "Splice exceeds sequence capacity.");
            plan.first = splice.first;
            plan.erased = splice.erase_count;
            plan.final_size = sequence.size() - splice.erase_count + splice.inserted.size();
            plan.inserted.reserve(splice.inserted.size());
            for (const auto& reference : splice.inserted) {
                auto valid =
                    validate_reference_assignment({address->value.type, ValueForm::strong_reference}, reference);
                if (!valid)
                    return valid;
                plan.inserted.push_back(reference.id());
            }
        }
        prepared.push_back(std::move(plan));
    }
    // All incoming values have been copied before any storage can move.
    for (auto& plan : prepared) {
        if (plan.address.value.form == ValueForm::strong_sequence)
            static_cast<ReferenceSequence*>(plan.address.data)->objects_.reserve(plan.final_size);
    }
    // The intrusive queues and cursor cancellation allocate nothing. Identity
    // storage moves are noexcept; no user callback or validation occurs here.
    for (const auto& plan : prepared) {
        impl_->stamp(plan.owner.index_);
        if (plan.address.value.form != ValueForm::strong_sequence) {
            commit_reference_write(plan.address, plan.reference);
            continue;
        }
        auto& objects = static_cast<ReferenceSequence*>(plan.address.data)->objects_;
        const auto old_size = objects.size();
        if (plan.final_size > old_size)
            objects.resize(plan.final_size);
        if (plan.inserted.size() > plan.erased) {
            std::move_backward(objects.begin() + plan.first + plan.erased, objects.begin() + old_size, objects.end());
        } else if (plan.inserted.size() < plan.erased) {
            std::move(objects.begin() + plan.first + plan.erased, objects.begin() + old_size,
                      objects.begin() + plan.first + plan.inserted.size());
        }
        std::copy(plan.inserted.begin(), plan.inserted.end(), objects.begin() + plan.first);
        if (plan.final_size < old_size)
            objects.resize(plan.final_size);
        impl_->sequence_changed(plan.owner);
        for (const auto object : plan.inserted)
            impl_->shade(object);
    }
    return {};
}

Result<void> Runtime::initialize_value(TypeId type_id, std::span<std::byte> destination) {
    if (impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy,
                       "Value mutation is not allowed during reference traversal or batch preparation.");
    const auto type = impl_->value_type(type_id, destination);
    if (!type)
        return std::unexpected(type.error());
    if (impl_->roots->overlaps_storage(destination.data(), (*type)->info.size)) {
        return failure(ErrorCode::busy, "End value-root registrations before starting a new value lifetime.");
    }
    return impl_->try_construct(**type, destination.data());
}

Result<void> Runtime::copy_construct_value(TypeId type_id, std::span<std::byte> destination,
                                           std::span<const std::byte> source) {
    if (impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy,
                       "Value mutation is not allowed during reference traversal or batch preparation.");
    const auto type = impl_->value_type(type_id, destination);
    if (!type)
        return std::unexpected(type.error());
    const auto valid = validate_storage((*type)->info, source);
    if (!valid)
        return std::unexpected(valid.error());
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
    if (impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy,
                       "Value mutation is not allowed during reference traversal or batch preparation.");
    const auto type = impl_->value_type(type_id, destination);
    if (!type)
        return std::unexpected(type.error());
    const auto valid = validate_storage((*type)->info, source);
    if (!valid)
        return std::unexpected(valid.error());
    if (!(*type)->info.copy_assignable) {
        return failure(ErrorCode::unsupported_operation, "Type does not support copy assignment.");
    }
    if (destination.data() == source.data())
        return {};
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
    if (impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy,
                       "Value mutation is not allowed during reference traversal or batch preparation.");
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    if (impl_->roots->overlaps_storage(storage.data(), (*type)->info.size)) {
        return failure(ErrorCode::busy, "End value-root registrations before destroying their storage.");
    }
    impl_->prepare_walk(**type);
    CallbackScope scope{impl_->callbacks};
    impl_->destroy(**type, storage.data());
    return {};
}

Result<void> Runtime::mutate_sequence(TypeId type_id, std::span<std::byte> storage, SequenceMutation operation,
                                      TypeId input_type, std::span<const std::byte> input, std::size_t count) {
    if (impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy, "Sequence mutation is not allowed during traversal or batch preparation.");
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    const auto& info = (*type)->info;
    if (!info.sequence_mutate || !info.value_operations_preserve_borrows || info.contains_references)
        return failure(ErrorCode::unsupported_operation,
                       "Sequence operations lack a borrow-preserving value contract.");
    switch (operation) {
    case SequenceMutation::add:
    case SequenceMutation::append: {
        const auto expected = operation == SequenceMutation::add ? info.element.value_or(TypeId{}) : type_id;
        if (expected == TypeId{} || input_type != expected)
            return failure(ErrorCode::type_mismatch, "Sequence input type does not match its operation.");
        auto valid = impl_->value_type(expected, input);
        if (!valid)
            return std::unexpected(valid.error());
        break;
    }
    case SequenceMutation::assign_text:
    case SequenceMutation::append_text:
        if (info.kind != TypeKind::string || input_type != TypeId{})
            return failure(ErrorCode::type_mismatch, "Text operations require String and UTF-8 bytes.");
        count = input.size();
        break;
    case SequenceMutation::resize:
        if (info.kind != TypeKind::array)
            return failure(ErrorCode::unsupported_operation, "Only Array supports resize.");
        [[fallthrough]];
    case SequenceMutation::reserve:
    case SequenceMutation::clear:
        if (!input.empty() || input_type != TypeId{})
            return failure(ErrorCode::argument_mismatch, "Unexpected sequence input.");
        break;
    default:
        return failure(ErrorCode::unsupported_operation, "Unknown sequence operation.");
    }
    CallbackScope scope{impl_->callbacks};
    try {
        info.sequence_mutate(operation, storage.data(), input.data(), count);
        return {};
    } catch (const std::exception& error) {
        return failure(ErrorCode::invocation_failed, error.what());
    } catch (...) {
        return failure(ErrorCode::invocation_failed, "Native sequence operation failed.");
    }
}

Result<std::uint32_t> Runtime::resolve_object(ObjectId object) const {
    if (!belongs_to_context(object) || object.index_ >= impl_->slots.size()) {
        return failure(ErrorCode::invalid_object, "Object does not belong to this runtime.");
    }
    const auto& slot = impl_->slots[object.index_];
    if (!impl_->is_live(slot) || slot.generation != object.generation_) {
        return failure(ErrorCode::invalid_object, "Object has been destroyed or committed for collection.");
    }
    return object.index_;
}

Result<ObjectRoot> Runtime::create(TypeId type_id) {
    if (impl_->callbacks != 0)
        return failure(ErrorCode::busy, "Object creation is not allowed during a callback.");
    const auto* type = impl_->lookup(type_id);
    if (!type || type->info.kind != TypeKind::object) {
        return failure(ErrorCode::invalid_type, "Only registered object types have independent identity.");
    }
    if ((impl_->free_head == no_slot && impl_->slots.size() >= no_slot) || !impl_->roots->has_capacity()) {
        return failure(ErrorCode::capacity_exceeded, "Object or root identity space exhausted.");
    }
    // No user callback may resize slots, so commit will not allocate after construction.
    if (impl_->free_head == no_slot && impl_->slots.size() == impl_->slots.capacity()) {
        impl_->slots.reserve(
            std::min<std::size_t>(no_slot, std::max(impl_->slots.size() + 1, impl_->slots.capacity() * 2)));
    }
    impl_->write_pages.resize(std::max(impl_->write_pages.size(), (impl_->slots.size() >> Impl::write_page_shift) + 1));
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
    if (index == impl_->slots.size())
        impl_->slots.emplace_back();
    else
        impl_->free_head = impl_->slots[index].next_free;
    auto& slot = impl_->slots[index];
    slot.type = type_id;
    slot.storage = storage;
    slot.next_free = no_slot;
    slot.mark_epoch = 0;
    impl_->stamp(index);
    ++impl_->live;
    if (impl_->phase == CollectionPhase::sweep) {
        slot.mark_epoch = impl_->epoch;
        ++impl_->marked;
    } else {
        impl_->shade(object);
    }
    return root;
}

Result<void> Runtime::destroy(ObjectId object) {
    if (impl_->callbacks != 0)
        return failure(ErrorCode::busy, "Object destruction is not allowed during a callback.");
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    auto& slot = impl_->slots[*index];
    {
        CallbackScope scope{impl_->callbacks};
        impl_->release(slot);
    }
    impl_->recycle(*index);
    return {};
}

bool Runtime::belongs_to_context(ObjectId object) const noexcept {
    return object != ObjectId{} && object.context_ == impl_->context;
}

Result<TypeId> Runtime::type_of(ObjectId object) const {
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    return impl_->slots[*index].type;
}

Result<std::uint64_t> Runtime::change_cursor() {
    if (impl_->callbacks != 0 || impl_->roots->traversals != 0 || impl_->active_executions != 0)
        return failure(ErrorCode::busy,
                       "Change cursors are taken outside callbacks, native borrows, traversal and execution.");
    return impl_->write_epoch++;
}

bool Runtime::changed_since(ObjectId object, std::uint64_t cursor) const noexcept {
    if (!belongs_to_context(object) || object.index_ >= impl_->slots.size())
        return true;
    const auto& slot = impl_->slots[object.index_];
    return !impl_->is_live(slot) || slot.generation != object.generation_ || slot.written > cursor;
}

void Runtime::changed_objects(std::uint64_t cursor, std::vector<ObjectId>& changed) const {
    changed.clear();
    constexpr std::size_t page_size = std::size_t{1} << Impl::write_page_shift;
    for (std::size_t page = 0; page < impl_->write_pages.size(); ++page) {
        if (impl_->write_pages[page] <= cursor)
            continue;
        const auto end = std::min(impl_->slots.size(), (page + 1) * page_size);
        for (auto index = page * page_size; index < end; ++index) {
            const auto& slot = impl_->slots[index];
            if (slot.written > cursor && impl_->is_live(slot))
                changed.push_back(ObjectId{impl_->context, static_cast<std::uint32_t>(index), slot.generation});
        }
    }
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
        if (reference.id().context_ != impl_->context)
            return failure(ErrorCode::invalid_object, "Reference identity belongs to another runtime.");
        const auto* live = impl_->lookup(reference.id());
        if (live && !Impl::derives_from(*impl_->lookup(live->type), *type)) {
            return failure(ErrorCode::type_mismatch, "Live object does not match the reference target type.");
        }
    }
    return {};
}

Result<ObjectRoot> Runtime::hold(ObjectId object) {
    const auto type = type_of(object);
    if (!type)
        return std::unexpected(type.error());
    return hold(ObjectReference{{*type, ValueForm::strong_reference}, object});
}

Result<ObjectRoot> Runtime::hold(ObjectId object, TypeId declared_type) {
    const auto actual = type_of(object);
    if (!actual)
        return std::unexpected(actual.error());
    const auto compatible = is_a(*actual, declared_type);
    if (!compatible)
        return std::unexpected(compatible.error());
    if (!*compatible)
        return failure(ErrorCode::type_mismatch, "Object does not satisfy the declared root type.");
    return hold(ObjectReference{{declared_type, ValueForm::strong_reference}, object});
}

Result<ObjectRoot> Runtime::hold(const ObjectReference& reference) {
    if (!impl_->roots->alive || impl_->roots->traversals != 0) {
        return failure(ErrorCode::busy, "Cannot add roots during shutdown or reference traversal.");
    }
    const auto valid = validate_reference(reference);
    if (!valid)
        return std::unexpected(valid.error());
    if (reference.description().form != ValueForm::strong_reference) {
        return failure(ErrorCode::type_mismatch, "Use try_acquire to obtain a strong root from a weak reference.");
    }
    if (!impl_->roots->has_capacity())
        return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    return ObjectRoot{impl_->roots, impl_->roots->add(reference)};
}

Result<ObjectReference> Runtime::observe(const ObjectReference& reference) const {
    const auto valid = validate_reference(reference);
    if (!valid)
        return std::unexpected(valid.error());
    return ObjectReference{{reference.description().type, ValueForm::weak_reference}, reference.id()};
}

Result<ObjectReference> Runtime::null_reference(TypeId type_id, ValueForm form) const {
    ObjectReference reference{{type_id, form}, ObjectId{}};
    const auto valid = validate_reference(reference);
    if (!valid)
        return std::unexpected(valid.error());
    return reference;
}

Result<std::optional<ObjectRoot>> Runtime::try_acquire(const ObjectReference& reference) {
    if (!impl_->roots->alive || impl_->roots->traversals != 0) {
        return failure(ErrorCode::busy, "Cannot acquire references during shutdown or reference traversal.");
    }
    const auto valid = validate_reference(reference);
    if (!valid)
        return std::unexpected(valid.error());
    if (reference.description().form != ValueForm::weak_reference) {
        return failure(ErrorCode::type_mismatch, "try_acquire requires a weak reference.");
    }
    if (reference.is_null() || !impl_->lookup(reference.id()))
        return std::optional<ObjectRoot>{};
    if (!impl_->roots->has_capacity())
        return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    ObjectReference strong{{reference.description().type, ValueForm::strong_reference}, reference.id()};
    return std::optional<ObjectRoot>{ObjectRoot{impl_->roots, impl_->roots->add(strong)}};
}

Result<ValueRoot> Runtime::root_value(TypeId type_id, std::span<const std::byte> storage) {
    if (!impl_->roots->alive || impl_->roots->traversals != 0) {
        return failure(ErrorCode::busy, "Cannot register value roots during shutdown or reference traversal.");
    }
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    if (!impl_->roots->has_capacity())
        return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    return ValueRoot{impl_->roots,
                     impl_->roots->add(detail::ValueRootStorage{type_id, storage.first((*type)->info.size)})};
}

Result<void> Runtime::visit_roots(ReferenceVisitor visitor) const {
    if (!impl_->roots->alive)
        return failure(ErrorCode::busy, "Runtime is shutting down.");
    return impl_->visit_guarded(visitor, [&]() -> Result<void> {
        const auto limit = impl_->roots->entries.size();
        for (std::size_t i = 0; i != limit; ++i) {
            if (!impl_->roots->entries[i].active)
                continue;
            // A visitor may release a root; keep the current descriptor independent.
            const auto value = impl_->roots->entries[i].value;
            if (const auto* reference = std::get_if<ObjectReference>(&value)) {
                if (!reference->is_null()) {
                    const auto result = visitor.visit(visitor.context, *reference);
                    if (!result)
                        return result;
                }
            } else {
                const auto& rooted = std::get<detail::ValueRootStorage>(value);
                const auto result = impl_->trace(*impl_->lookup(rooted.type), rooted.storage.data(), visitor);
                if (!result)
                    return result;
            }
        }
        return {};
    });
}

Result<void> Runtime::visit_references(ObjectId object, ReferenceVisitor visitor) const {
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    const auto& slot = impl_->slots[*index];
    return impl_->visit_guarded(visitor,
                                [&] { return impl_->trace(*impl_->lookup(slot.type), slot.storage, visitor); });
}

Result<void> Runtime::visit_references(TypeId type_id, std::span<const std::byte> storage,
                                       ReferenceVisitor visitor) const {
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    return impl_->visit_guarded(visitor, [&] { return impl_->trace(**type, storage.data(), visitor); });
}

std::size_t Runtime::root_count() const noexcept {
    return impl_->roots->count;
}

Result<Runtime::Address> Runtime::resolve_field(ObjectId object, std::span<const FieldId> path) const {
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    const auto& slot = impl_->slots[*index];
    return resolve_path(slot.type, slot.storage, path);
}

Result<Runtime::Address> Runtime::resolve_path(TypeId type, void* storage, std::span<const FieldId> path) const {
    if (path.empty())
        return failure(ErrorCode::invalid_member, "Field path must not be empty.");
    CallbackScope scope{impl_->callbacks};
    Address result{storage, {type}, nullptr, type, path};
    for (std::size_t depth = 0; depth != path.size(); ++depth) {
        const auto field = path[depth];
        if (result.value.form != ValueForm::inline_value) {
            return failure(ErrorCode::invalid_member, "Field paths cannot traverse object references.");
        }
        const auto* actual = impl_->lookup(result.value.type);
        const auto* owner = impl_->lookup(field.owner_);
        if (actual->info.kind == TypeKind::array || (owner && owner->info.kind == TypeKind::array)) {
            if (owner != actual)
                return failure(ErrorCode::invalid_member,
                               actual->info.kind == TypeKind::array
                                   ? "An Array value takes only element selectors of its own type."
                                   : "An element selector requires a value of its Array type.");
            const auto& element = *impl_->lookup(*actual->info.element);
            if (result.data) {
                const auto [items, count] = Impl::elements(*actual, result.data);
                if (field.index_ >= count)
                    return impl_->located(ErrorCode::invalid_member, type, path.first(depth + 1), {},
                                          "Array element index " + std::to_string(field.index_) +
                                              " is out of range of " + std::to_string(count) + " elements.");
                result.data = items + std::size_t{field.index_} * element.info.size;
            }
            // A ranged Array bounds each scalar or vector element; the range carries forward.
            result.value = {element.info.id};
            continue;
        }
        if (!owner || !Impl::derives_from(*actual, *owner) || field.index_ >= owner->fields.size()) {
            return failure(ErrorCode::invalid_member, "Field does not belong to its containing type.");
        }
        const auto& access = owner->fields[field.index_];
        // Null storage resolves only the description and range, for constrain().
        if (result.data) {
            result.data = Impl::base_storage(*actual, *owner, result.data);
            result.data =
                access.address ? access.address(result.data) : static_cast<std::byte*>(result.data) + access.offset;
        }
        result.value = access.value;
        // A math vector's components inherit the vector field's bounds.
        result.range = access.hard ? access.hard.get() : actual->info.math ? result.range : nullptr;
    }
    return result;
}

Result<ObjectReference> Runtime::read_reference_at(Address address) const {
    if (address.value.form != ValueForm::strong_reference && address.value.form != ValueForm::weak_reference)
        return failure(ErrorCode::type_mismatch, "Read requires a single object reference field.");
    ObjectReference reference{address.value, static_cast<const ReferenceSlot*>(address.data)->object_};
    const auto valid = validate_reference(reference);
    if (!valid)
        return std::unexpected(valid.error());
    return reference;
}

Result<void> Runtime::validate_reference_assignment(ValueDescription value, const ObjectReference& reference) const {
    const auto valid = validate_reference(reference);
    if (!valid)
        return std::unexpected(valid.error());
    const auto* target = impl_->lookup(value.type);
    if (!target || target->info.kind != TypeKind::object ||
        (value.form != ValueForm::strong_reference && value.form != ValueForm::weak_reference) ||
        !Impl::derives_from(*impl_->lookup(reference.description().type), *target) ||
        (value.form == ValueForm::strong_reference && reference.description().form != ValueForm::strong_reference)) {
        return failure(ErrorCode::type_mismatch, "Reference target or holding form does not match the field.");
    }
    return {};
}

void Runtime::commit_reference_write(Address address, ObjectId object) noexcept {
    static_cast<ReferenceSlot*>(address.data)->object_ = object;
    if (address.value.form == ValueForm::strong_reference)
        impl_->shade(object);
}

Result<void> Runtime::write_reference_at(Address address, const ObjectReference& reference) {
    if (impl_->changing_references)
        return failure(ErrorCode::busy, "Reference write cannot reenter batch preparation.");
    auto valid = validate_reference_assignment(address.value, reference);
    if (!valid)
        return valid;
    commit_reference_write(address, reference.id());
    return {};
}

Result<ObjectReference> Runtime::read_reference(ValueDescription value, const ReferenceSlot& storage) const {
    return read_reference_at({const_cast<ReferenceSlot*>(&storage), value});
}

Result<void> Runtime::write_reference(ValueDescription value, ReferenceSlot& storage,
                                      const ObjectReference& reference) {
    if (impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Reference mutation is not allowed during traversal.");
    return write_reference_at({&storage, value}, reference);
}

Result<ObjectReference> Runtime::read_reference(ObjectId object, std::span<const FieldId> path) const {
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    return read_reference_at(*address);
}

Result<void> Runtime::write_reference(ObjectId object, std::span<const FieldId> path,
                                      const ObjectReference& reference) {
    if (impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Reference mutation is not allowed during traversal.");
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    impl_->stamp(object.index_);
    return write_reference_at(*address, reference);
}

Result<ObjectReference> Runtime::read_reference(TypeId type_id, std::span<const std::byte> storage,
                                                std::span<const FieldId> path) const {
    const auto address = resolve_storage(type_id, storage, path);
    if (!address)
        return std::unexpected(address.error());
    return read_reference_at(*address);
}

Result<void> Runtime::write_reference(TypeId type_id, std::span<std::byte> storage, std::span<const FieldId> path,
                                      const ObjectReference& reference) {
    if (impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Reference mutation is not allowed during traversal.");
    const auto address = resolve_storage(type_id, storage, path);
    if (!address)
        return std::unexpected(address.error());
    return write_reference_at(*address, reference);
}

Result<Scalar> Runtime::read(ObjectId object, std::span<const FieldId> path) const {
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    return read_at(*address);
}

Result<void> Runtime::write(ObjectId object, std::span<const FieldId> path, Scalar value) {
    if (impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Field mutation is not allowed during reference traversal.");
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    impl_->stamp(object.index_);
    return write_at(*address, value);
}

Result<Scalar> Runtime::read(TypeId type, std::span<const std::byte> storage, std::span<const FieldId> path) const {
    const auto address = resolve_storage(type, storage, path);
    if (!address)
        return std::unexpected(address.error());
    return read_at(*address);
}

Result<void> Runtime::write(TypeId type, std::span<std::byte> storage, std::span<const FieldId> path, Scalar value) {
    if (impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy,
                       "Value mutation is not allowed during reference traversal or batch preparation.");
    const auto address = resolve_storage(type, storage, path);
    if (!address)
        return std::unexpected(address.error());
    return write_at(*address, value);
}

Result<Runtime::Address> Runtime::resolve_storage(TypeId type, std::span<const std::byte> storage,
                                                  std::span<const FieldId> path) const {
    const auto valid = impl_->value_type(type, storage);
    if (!valid)
        return std::unexpected(valid.error());
    return resolve_path(type, const_cast<std::byte*>(storage.data()), path);
}

Result<Scalar> Runtime::read_at(const Address& address) const {
    if (address.value.type == impl_->f32)
        return Scalar{*static_cast<const float*>(address.data)};
    if (address.value.type == impl_->f64)
        return Scalar{*static_cast<const double*>(address.data)};
    if (address.value.type == impl_->u32)
        return Scalar{*static_cast<const std::uint32_t*>(address.data)};
    if (address.value.type == impl_->i32)
        return Scalar{*static_cast<const std::int32_t*>(address.data)};
    if (address.value.type == impl_->boolean)
        return Scalar{*static_cast<const bool*>(address.data)};
    const auto* type = impl_->lookup(address.value.type);
    if (type && type->info.kind == TypeKind::enumeration && address.value.form == ValueForm::inline_value) {
        std::uint32_t bits;
        std::memcpy(&bits, address.data, 4);
        return Scalar{EnumValue{type->info.id, bits}};
    }
    return failure(ErrorCode::type_mismatch, "Read requires a scalar or enum field; select a field inside the record.");
}

Result<void> Runtime::write_at(const Address& address, Scalar value) {
    if (address.value.form != ValueForm::inline_value || address.value.type != impl_->scalar_type(value)) {
        return failure(ErrorCode::type_mismatch, "Value type does not match the field.");
    }
    if (std::holds_alternative<EnumValue>(value) &&
        impl_->lookup(address.value.type)->info.kind != TypeKind::enumeration)
        return failure(ErrorCode::type_mismatch, "Enum value cannot masquerade as a scalar.");
    if (address.range && !constrain_scalar(*address.range, value))
        return impl_->located(ErrorCode::argument_mismatch, address.root, address.path, {},
                              "Hard-bounded fields reject NaN and infinity.");
    if (address.value.type == impl_->f32)
        *static_cast<float*>(address.data) = std::get<float>(value);
    else if (address.value.type == impl_->f64)
        *static_cast<double*>(address.data) = std::get<double>(value);
    else if (address.value.type == impl_->u32)
        *static_cast<std::uint32_t*>(address.data) = std::get<std::uint32_t>(value);
    else if (address.value.type == impl_->i32)
        *static_cast<std::int32_t*>(address.data) = std::get<std::int32_t>(value);
    else if (address.value.type == impl_->boolean)
        *static_cast<bool*>(address.data) = std::get<bool>(value);
    else {
        const auto* type = impl_->lookup(address.value.type);
        if (!type || type->info.kind != TypeKind::enumeration || !std::holds_alternative<EnumValue>(value))
            return failure(ErrorCode::type_mismatch, "Value is not an enum of the field type.");
        std::memcpy(address.data, &std::get<EnumValue>(value).bits, 4);
    }
    return {};
}

Result<Scalar> Runtime::constrain(TypeId type, std::span<const FieldId> path, Scalar value) const {
    if (!impl_->lookup(type))
        return failure(ErrorCode::invalid_type, "Constraint query requires a registered type.");
    const auto address = resolve_path(type, nullptr, path);
    if (!address)
        return std::unexpected(address.error());
    if (address->value.form != ValueForm::inline_value || address->value.type != impl_->scalar_type(value) ||
        (std::holds_alternative<EnumValue>(value) &&
         impl_->lookup(address->value.type)->info.kind != TypeKind::enumeration))
        return failure(ErrorCode::type_mismatch, "Value type does not match the field.");
    if (address->range && !constrain_scalar(*address->range, value))
        return impl_->located(ErrorCode::argument_mismatch, type, path, {},
                              "Hard-bounded fields reject NaN and infinity.");
    return value;
}

Result<std::optional<ValidationFailure>> Runtime::validate(ObjectId object) const {
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    const auto& slot = impl_->slots[*index];
    const auto& type = *impl_->lookup(slot.type);
    if (type.constrained_leaves.empty() && type.validators.empty())
        return std::nullopt;
    return impl_->check_rules(type, slot.storage);
}

Result<std::optional<ValidationFailure>> Runtime::validate_value(TypeId type_id,
                                                                 std::span<const std::byte> storage) const {
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    if ((*type)->constrained_leaves.empty() && (*type)->validators.empty())
        return std::nullopt;
    return impl_->check_rules(**type, const_cast<std::byte*>(storage.data()));
}

Result<FieldId> Runtime::element(TypeId array, std::uint32_t index) const {
    const auto* type = impl_->lookup(array);
    if (!type || type->info.kind != TypeKind::array)
        return failure(ErrorCode::invalid_type, "Element selectors require a registered Array type.");
    return FieldId{array, index};
}

std::optional<std::uint32_t> Runtime::element_index(FieldId member) const {
    const auto* owner = impl_->lookup(member.owner_);
    if (!owner || owner->info.kind != TypeKind::array)
        return std::nullopt;
    return member.index_;
}

Result<Runtime::Address> Runtime::resolve_array(ObjectId object, std::span<const FieldId> path) const {
    auto address = resolve_field(object, path);
    if (!address)
        return address;
    if (address->value.form != ValueForm::inline_value ||
        impl_->lookup(address->value.type)->info.kind != TypeKind::array)
        return failure(ErrorCode::type_mismatch, "Element operations require an inline Array field.");
    return address;
}

Result<std::size_t> Runtime::element_count(ObjectId object, std::span<const FieldId> array) const {
    const auto address = resolve_array(object, array);
    if (!address)
        return std::unexpected(address.error());
    return Impl::elements(*impl_->lookup(address->value.type), address->data).second;
}

Result<void> Runtime::insert_element(ObjectId object, std::span<const FieldId> array, std::size_t index,
                                     ValueInput value) {
    return mutate_elements(object, array, SequenceMutation::insert, index, 0, value);
}

Result<void> Runtime::remove_elements(ObjectId object, std::span<const FieldId> array, std::size_t first,
                                      std::size_t count) {
    return mutate_elements(object, array, SequenceMutation::remove, first, count, {});
}

Result<void> Runtime::move_element(ObjectId object, std::span<const FieldId> array, std::size_t from, std::size_t to) {
    return mutate_elements(object, array, SequenceMutation::move, from, to, {});
}

Result<void> Runtime::mutate_elements(ObjectId object, std::span<const FieldId> path, SequenceMutation operation,
                                      std::size_t position, std::size_t operand, ValueInput value) {
    // Borrowed element addresses (native borrows, method arguments) must not outlive a change.
    if (impl_->callbacks != 0 || impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy, "Array structure cannot change during callbacks, native borrows or traversal.");
    const auto address = resolve_array(object, path);
    if (!address)
        return std::unexpected(address.error());
    impl_->stamp(object.index_);
    const auto& array = *impl_->lookup(address->value.type);
    if (!array.info.sequence_mutate)
        return failure(ErrorCode::unsupported_operation, "This Array type has no structural operations.");
    const auto& element = *impl_->lookup(*array.info.element);
    const auto size = Impl::elements(array, address->data).second;
    const bool copy = value.type != TypeId{} || value.data;
    if (operation == SequenceMutation::insert   ? position > size
        : operation == SequenceMutation::remove ? position > size || operand > size - position
                                                : position >= size || operand >= size)
        return impl_->located(ErrorCode::argument_mismatch, address->root, path, {},
                              (operation == SequenceMutation::insert ? "Insertion at " + std::to_string(position)
                               : operation == SequenceMutation::remove
                                   ? "Removal of " + std::to_string(operand) + " at " + std::to_string(position)
                                   : "Move from " + std::to_string(position) + " to " + std::to_string(operand)) +
                                  " is out of range of " + std::to_string(size) + " elements.");
    if (copy && value.type != element.info.id)
        return failure(ErrorCode::type_mismatch, "Inserted value must have the Array's element type.");
    if (copy && (!value.data || reinterpret_cast<std::uintptr_t>(value.data) % element.info.alignment != 0))
        return failure(ErrorCode::invalid_storage, "Inserted value storage is null or misaligned.");
    CallbackScope scope{impl_->callbacks};
    if (copy && !impl_->constrain_value(element, address->range, const_cast<void*>(value.data), false)) {
        std::vector<FieldId> leaf;
        if (position < Impl::selectable_elements) {
            leaf = impl_->rejected_leaf(element, address->range, const_cast<void*>(value.data));
            leaf.insert(leaf.begin(), FieldId{array.info.id, static_cast<std::uint32_t>(position)});
        }
        return impl_->located(ErrorCode::argument_mismatch, address->root, path, leaf,
                              "Hard-bounded fields reject NaN and infinity.");
    }
    try {
        array.info.sequence_mutate(operation, address->data,
                                   operation == SequenceMutation::insert ? value.data : &operand, position);
    } catch (const std::exception& error) {
        return failure(ErrorCode::invocation_failed, error.what());
    } catch (...) {
        return failure(ErrorCode::invocation_failed, "Native Array operation failed.");
    }
    if (copy)
        impl_->constrain_value(element, address->range,
                               Impl::elements(array, address->data).first + position * element.info.size, true);
    return {};
}

Result<void> Runtime::read(ObjectId object, std::span<const FieldId> path, ValueOutput output) const {
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    return read_at(*address, output);
}

Result<void> Runtime::write(ObjectId object, std::span<const FieldId> path, ValueInput value) {
    if (impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Field mutation is not allowed during reference traversal.");
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    impl_->stamp(object.index_);
    return write_at(*address, value, true);
}

Result<void> Runtime::read(TypeId type, std::span<const std::byte> storage, std::span<const FieldId> path,
                           ValueOutput output) const {
    const auto address = resolve_storage(type, storage, path);
    if (!address)
        return std::unexpected(address.error());
    return read_at(*address, output);
}

Result<void> Runtime::write(TypeId type, std::span<std::byte> storage, std::span<const FieldId> path,
                            ValueInput value) {
    if (impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy,
                       "Value mutation is not allowed during reference traversal or batch preparation.");
    const auto address = resolve_storage(type, storage, path);
    if (!address)
        return std::unexpected(address.error());
    return write_at(*address, value, false);
}

namespace {
// Plain values move as bytes; other whole values must copy without GC references or borrow hazards.
bool whole_value(const TypeInfo& info) {
    return info.plain_value || (info.kind != TypeKind::object && !info.contains_references && info.copy_constructible &&
                                info.copy_assignable && info.value_operations_preserve_borrows);
}
}

Result<void> Runtime::read_at(const Address& address, ValueOutput output) const {
    const auto* type = impl_->lookup(address.value.type);
    if (address.value.form != ValueForm::inline_value || !whole_value(type->info) || output.type != type->info.id)
        return failure(ErrorCode::type_mismatch,
                       "Whole-value field read requires its exact Runtime type, without GC references.");
    if (!output.data || reinterpret_cast<std::uintptr_t>(output.data) % type->info.alignment != 0)
        return failure(ErrorCode::invalid_storage, "Value output storage is null or misaligned.");
    if (type->info.plain_value) {
        std::memmove(output.data, address.data, type->info.size);
        return {};
    }
    return impl_->replace_value(*type, nullptr, output.data, address.data);
}

Result<void> Runtime::write_at(const Address& address, ValueInput value, bool object) {
    const auto* type = impl_->lookup(address.value.type);
    if (address.value.form != ValueForm::inline_value || !whole_value(type->info) || value.type != type->info.id)
        return failure(ErrorCode::type_mismatch,
                       "Whole-value field write requires its exact Runtime type, without GC references.");
    if (!value.data || reinterpret_cast<std::uintptr_t>(value.data) % type->info.alignment != 0)
        return failure(ErrorCode::invalid_storage, "Value input storage is null or misaligned.");
    const bool constrained = address.range || !type->constrained_leaves.empty();
    if (type->info.plain_value && !constrained) {
        std::memmove(address.data, value.data, type->info.size);
        return {};
    }
    // Replacing a buffer would leave borrowed addresses into the old one dangling.
    if (!type->info.plain_value && object && impl_->callbacks != 0)
        return failure(ErrorCode::busy,
                       "Non-plain field values cannot be replaced during callbacks or native borrows.");
    return impl_->replace_value(*type, constrained ? &address : nullptr, address.data, value.data);
}

Result<void> Runtime::restore(ObjectId object, std::span<const FieldId> path, ValueOutput previous) {
    if (impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Field mutation is not allowed during reference traversal.");
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    impl_->stamp(object.index_);
    const auto* type = impl_->lookup(address->value.type);
    if (address->value.form != ValueForm::inline_value || !whole_value(type->info) || previous.type != type->info.id)
        return failure(ErrorCode::type_mismatch,
                       "Restore requires the field's exact Runtime type, without GC references.");
    if (!previous.data || reinterpret_cast<std::uintptr_t>(previous.data) % type->info.alignment != 0)
        return failure(ErrorCode::invalid_storage, "Restored value storage is null or misaligned.");
    auto* field = static_cast<std::byte*>(address->data);
    auto* value = static_cast<std::byte*>(previous.data);
    if (type->info.plain_value) {
        std::swap_ranges(field, field + type->info.size, value);
        return {};
    }
    if (type->swap_parts.empty())
        return failure(ErrorCode::unsupported_operation, "Restore requires a value type with a non-throwing swap.");
    if (impl_->callbacks != 0)
        return failure(ErrorCode::busy,
                       "Non-plain field values cannot be replaced during callbacks or native borrows.");
    CallbackScope scope{impl_->callbacks};
    Impl::swap_value(*type, field, value);
    return {};
}

Result<void> Runtime::prepare_arguments(const FunctionSignature& signature, std::span<const Argument> arguments,
                                        PreparedArguments& prepared) const {
    return prepare_arguments(signature, arguments, prepared, false);
}
Result<void> Runtime::prepare_arguments(const FunctionSignature& signature, std::span<const Argument> arguments,
                                        PreparedArguments& prepared, bool fields) const {
    prepared.size_ = 0;
    if (arguments.size() != signature.parameters.size())
        return failure(ErrorCode::argument_mismatch, "Incorrect argument count.");
    std::size_t copied_size{};
    std::size_t copied_alignment = alignof(double);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& parameter = signature.parameters[i];
        const auto& argument = arguments[i];
        const auto* type = impl_->lookup(parameter.type);
        if (!type || (type->info.kind != TypeKind::scalar && type->info.kind != TypeKind::enumeration &&
                      !type->info.plain_value))
            continue; // The validation pass diagnoses unsupported descriptions before access.
        const bool borrowed = argument.borrowed_ || argument.object_ != ObjectId{} || !argument.path_.empty();
        if (parameter.passing != ParameterPassing::value && borrowed)
            continue;
        const auto offset = aligned_size(copied_size, type->info.alignment);
        if (!offset || type->info.size > std::numeric_limits<std::size_t>::max() - *offset)
            return failure(ErrorCode::capacity_exceeded, "Argument scratch exceeds addressable storage.");
        copied_size = *offset + type->info.size;
        copied_alignment = std::max(copied_alignment, type->info.alignment);
    }
    auto* storage = prepared.storage_.prepare(copied_size, copied_alignment);
    if (arguments.size() > prepared.inline_views_.size())
        prepared.views_.resize(arguments.size());
    auto* views =
        arguments.size() > prepared.inline_views_.size() ? prepared.views_.data() : prepared.inline_views_.data();
    std::size_t copied_offset{};
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        const auto& parameter = signature.parameters[i];
        const auto* expected = impl_->lookup(parameter.type);
        const bool scalar =
            expected && (expected->info.kind == TypeKind::scalar || expected->info.kind == TypeKind::enumeration);
        const bool record_value = expected && expected->info.kind == TypeKind::record && expected->info.plain_value;
        const bool value_borrow = expected && !expected->info.contains_references &&
                                  (expected->info.kind == TypeKind::record || expected->info.kind == TypeKind::array ||
                                   expected->info.kind == TypeKind::string) &&
                                  parameter.passing != ParameterPassing::value;
        if (!expected || (!scalar && !record_value && !value_borrow) || parameter.form != ValueForm::inline_value ||
            (parameter.passing != ParameterPassing::value && parameter.passing != ParameterPassing::read_only &&
             parameter.passing != ParameterPassing::reference) ||
            (signature.execution == FunctionExecution::coroutine &&
             (parameter.passing != ParameterPassing::value || record_value)))
            return failure(ErrorCode::invalid_definition, "Unsupported parameter description.");
        const Impl::Type* actual{};
        const void* data = argument.data_;
        void* writable = argument.writable_;
        const bool field = argument.object_ != ObjectId{} || !argument.path_.empty();
        if (field) {
            if (!fields)
                return failure(ErrorCode::argument_mismatch,
                               "Source entry cannot borrow object-field storage across safe points.");
            auto address = resolve_field(argument.object_, argument.path_);
            if (!address)
                return std::unexpected(address.error());
            if (address->value.form != ValueForm::inline_value)
                return failure(ErrorCode::argument_mismatch, "Reference parameter requires inline value storage.");
            actual = impl_->lookup(address->value.type);
            if (parameter.passing == ParameterPassing::reference &&
                (address->range || !actual->constrained_leaves.empty()))
                return failure(ErrorCode::argument_mismatch,
                               "Hard-bounded fields cannot be borrowed writably; write them through Runtime.");
            if (parameter.passing == ParameterPassing::reference)
                impl_->stamp(argument.object_.index_);
            data = writable = address->data;
        } else if (argument.borrowed_) {
            if (!data)
                return failure(ErrorCode::invalid_storage, "Borrowed argument is null.");
            actual = impl_->lookup(argument.type_);
        } else
            actual = impl_->lookup(impl_->scalar_type(argument.value_));
        if (actual != expected || (std::holds_alternative<EnumValue>(argument.value_) && !data &&
                                   expected->info.kind != TypeKind::enumeration))
            return failure(ErrorCode::argument_mismatch, "Argument type does not match parameter " + std::to_string(i));
        if (data &&
            (reinterpret_cast<std::uintptr_t>(data) % expected->info.alignment != 0 || (writable && data != writable)))
            return failure(ErrorCode::invalid_storage, "Borrowed argument storage is invalid or misaligned.");
        if (parameter.passing == ParameterPassing::reference && !writable)
            return failure(ErrorCode::argument_mismatch, "Writable reference requires mutable lvalue storage.");
        if (parameter.passing == ParameterPassing::value || !data) {
            // The sizing pass checked this alignment and total size before any
            // view was published, so growing the owner cannot invalidate views.
            copied_offset = (copied_offset + expected->info.alignment - 1) & ~(expected->info.alignment - 1);
            auto* destination = storage + copied_offset;
            copied_offset += expected->info.size;
            // Plain copied values need their lifetime started, but trivial destruction
            // requires no Runtime callback when the independent scratch owner ends.
            auto constructed = impl_->try_construct(*expected, destination, data);
            if (!constructed)
                return constructed;
            if (!data) {
                std::visit(
                    [&](const auto& value) {
                        using T = std::remove_cvref_t<decltype(value)>;
                        if constexpr (std::same_as<T, EnumValue>)
                            std::memcpy(destination, &value.bits, 4);
                        else
                            std::memcpy(destination, &value, sizeof(T));
                    },
                    argument.value_);
            }
            data = destination;
            writable = nullptr;
        }
        views[i] = {parameter.type, data, parameter.passing == ParameterPassing::reference ? writable : nullptr};
    }
    prepared.size_ = arguments.size();
    return {};
}

Result<void> Runtime::call(ObjectId object, MethodId method, std::span<const Scalar> arguments, Scalar* output) {
    std::array<Argument, 8> local;
    std::vector<Argument> expanded(arguments.size() > local.size() ? arguments.size() : 0);
    auto values =
        (expanded.empty() ? std::span<Argument>{local} : std::span<Argument>{expanded}).first(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i)
        values[i] = Argument{arguments[i]};
    return call_arguments(object, method, values, output);
}
Result<void> Runtime::call(ObjectId object, MethodId method, std::span<const Scalar> arguments, ValueOutput output) {
    std::array<Argument, 8> local;
    std::vector<Argument> expanded(arguments.size() > local.size() ? arguments.size() : 0);
    auto values =
        (expanded.empty() ? std::span<Argument>{local} : std::span<Argument>{expanded}).first(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i)
        values[i] = Argument{arguments[i]};
    return call_arguments(object, method, values, nullptr, output);
}
Result<void> Runtime::call_arguments(ObjectId object, MethodId method, std::span<const Argument> arguments,
                                     Scalar* output, ValueOutput value_output) {
    if (impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Method execution is not allowed during reference traversal.");
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    const auto& slot = impl_->slots[*index];
    const auto* actual = impl_->lookup(slot.type);
    const auto* type = impl_->lookup(method.owner_);
    if (!type || !Impl::derives_from(*actual, *type) || method.index_ >= type->methods.size()) {
        return failure(ErrorCode::invalid_member, "Method does not belong to the receiver type.");
    }
    const auto inherited = type->base ? type->base->info.methods.size() : 0;
    const auto& signature = type->info.methods[inherited + method.index_].signature;
    const auto* result_type = signature.result ? impl_->lookup(signature.result->type) : nullptr;
    const bool record_result =
        result_type && result_type->info.kind == TypeKind::record && result_type->info.plain_value;
    if (record_result) {
        if (value_output.type != result_type->info.id)
            return failure(ErrorCode::type_mismatch, "Plain value result requires its exact Runtime type.");
        if (!value_output.data ||
            reinterpret_cast<std::uintptr_t>(value_output.data) % result_type->info.alignment != 0)
            return failure(ErrorCode::invalid_storage, "Plain value result storage is null or misaligned.");
    } else if (value_output.type != TypeId{} || value_output.data) {
        return failure(ErrorCode::type_mismatch, "Typed value output requires a plain record result.");
    } else if (signature.result && !output) {
        return failure(ErrorCode::invalid_storage, "Non-void method requires result storage.");
    }
    CallbackScope scope{impl_->callbacks};
    PreparedArguments prepared;
    auto checked = prepare_arguments(signature, arguments, prepared, true);
    if (!checked)
        return checked;
    if (!signature.receiver->read_only)
        impl_->stamp(*index);
    const auto invoke = [&](ValueOutput staging) -> Result<void> {
        CallContext context{*this, object, Impl::base_storage(*actual, *type, slot.storage), prepared.values()};
        context.result_value = staging;
        try {
            auto invoked = type->methods[method.index_](context);
            if (!invoked)
                return invoked;
            if (record_result) {
                if (context.result || !context.result_written || context.result_value.type != staging.type ||
                    context.result_value.data != staging.data)
                    return failure(ErrorCode::invocation_failed,
                                   "Method did not produce its declared plain value result.");
                std::memcpy(value_output.data, staging.data, result_type->info.size);
            } else {
                if (context.result_written || context.result_value.data || context.result_value.type != TypeId{})
                    return failure(ErrorCode::invocation_failed, "Method produced an undeclared plain value result.");
                if (signature.result) {
                    if (!context.result || impl_->scalar_type(*context.result) != signature.result->type ||
                        (std::holds_alternative<EnumValue>(*context.result) &&
                         result_type->info.kind != TypeKind::enumeration))
                        return failure(ErrorCode::invocation_failed,
                                       "Method did not produce its declared result type.");
                    *output = *context.result;
                } else if (context.result) {
                    return failure(ErrorCode::invocation_failed, "Void method produced an undeclared result.");
                }
            }
            return {};
        } catch (const std::exception& error) {
            return failure(ErrorCode::invocation_failed, error.what());
        } catch (...) {
            return failure(ErrorCode::invocation_failed, "Native method raised an unknown exception.");
        }
    };
    if (record_result) {
        detail::ValueStorage staging_owner;
        auto* staging = staging_owner.prepare(result_type->info.size, result_type->info.alignment);
        auto constructed = impl_->try_construct(*result_type, staging);
        if (!constructed)
            return constructed;
        return invoke({result_type->info.id, staging});
    }
    return invoke({});
}

std::size_t Runtime::live_objects() const noexcept {
    return impl_->live;
}

}
