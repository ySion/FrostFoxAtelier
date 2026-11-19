#include "foundation/inline_array.hpp"
#include "foundation/map.hpp"
#include "foundation/sparse_array.hpp"
#include "foundation/string.hpp"
#include "runtime/runtime.hpp"
#include "runtime/execution.hpp"
#include "runtime/detail/type_publication.hpp"
#include "runtime/detail/parallel_access.hpp"
#include "detail/math_registration.hpp"
#include "memory/fatal.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace atelier {
namespace {

constexpr auto no_slot = std::numeric_limits<std::uint32_t>::max();

std::atomic<std::uint64_t> next_context{1};

auto failure(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

bool normalize_description(std::string& text) {
    if (!StringView{text}.is_valid_utf8())
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

    explicit CallbackScope(std::size_t& count) : depth(count) {
        detail::assert_serial_access();
        ++depth;
    }

    ~CallbackScope() {
        detail::assert_serial_access();
        --depth;
    }
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
    struct ValueEntry {
        ValueRootStorage value;

        std::uint32_t trace_next{no_slot};

        std::uint32_t trace_previous{no_slot};

        bool trace_queued{};
    };

    SparseArray<ObjectReference> object_entries;

    SparseArray<ValueEntry> value_entries;

    std::size_t references{1};

    std::size_t traversals{};

    bool alive{true};

    bool marking{};

    void* observer{};

    void (*object_added)(void*, ObjectId) noexcept {};

    void (*value_added)(void*, std::uint32_t) noexcept {};

    void (*value_removing)(void*, std::uint32_t) noexcept {};

    void retain() noexcept { ++references; }

    static void release(RootState* state) noexcept {
        if (state) {
            assert(state->references != 0);
            if (--state->references == 0)
                delete state;
        }
    }

    bool has_capacity() const noexcept { return object_entries.size() + value_entries.size() < no_slot; }

    std::uint32_t add_object(const ObjectReference& value) noexcept {
        assert_serial_access();
        if (traversals != 0)
            fatal("Cannot add roots during reference traversal.");
        if (!has_capacity())
            fatal("Root identity space exhausted.");
        const auto index = static_cast<std::uint32_t>(object_entries.emplace(value));
        if (marking)
            object_added(observer, value.id());
        return index;
    }

    void assign_object(std::uint32_t index, const ObjectReference& value) noexcept {
        if (traversals != 0)
            fatal("Cannot add roots during reference traversal.");
        object_entries[index] = value;
        if (marking)
            object_added(observer, value.id());
    }

    void remove_object(std::uint32_t index) noexcept {
        assert_serial_access();
        object_entries.erase(index);
    }

    std::uint32_t add_value(ValueRootStorage value) noexcept {
        assert_serial_access();
        if (traversals != 0)
            fatal("Cannot add roots during reference traversal.");
        if (!has_capacity())
            fatal("Root identity space exhausted.");
        const auto index = static_cast<std::uint32_t>(value_entries.emplace(ValueEntry{value}));
        if (marking)
            value_added(observer, index);
        return index;
    }

    void remove_value(std::uint32_t index) noexcept {
        assert_serial_access();
        if (value_removing)
            value_removing(observer, index);
        value_entries.erase(index);
    }

    bool overlaps_storage(const void* storage, std::size_t size) const noexcept {
        if (value_entries.empty())
            return false;
        for (const auto entry : value_entries.entries()) {
            const auto& value = entry.value.value;
            if (overlaps(storage, size, value.storage.data(), value.storage.size()))
                return true;
        }
        return false;
    }
};

}

ObjectRoot::ObjectRoot(detail::RootState* state, std::uint32_t index) noexcept : state_(state), index_(index) {
    if (state_)
        state_->retain();
}

ObjectRoot::~ObjectRoot() {
    reset();
}

ObjectRoot::ObjectRoot(const ObjectRoot& other) noexcept {
    detail::assert_serial_access();
    if (other.state_) {
        index_ = other.state_->add_object(other.reference());
        state_ = other.state_;
        state_->retain();
    }
}

ObjectRoot& ObjectRoot::operator=(const ObjectRoot& other) noexcept {
    if (this != &other) {
        detail::assert_serial_access();
        if (state_ && state_ == other.state_) {
            state_->assign_object(index_, other.reference());
            return *this;
        }
        ObjectRoot copy{other};
        *this = std::move(copy);
    }
    return *this;
}

ObjectRoot::ObjectRoot(ObjectRoot&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)), index_(std::exchange(other.index_, 0)) {}

ObjectRoot& ObjectRoot::operator=(ObjectRoot&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::exchange(other.state_, nullptr);
        index_ = std::exchange(other.index_, 0);
    }
    return *this;
}

void ObjectRoot::reset() noexcept {
    detail::assert_serial_access();
    if (state_) {
        auto* state = std::exchange(state_, nullptr);
        const auto index = std::exchange(index_, 0);
        state->remove_object(index);
        detail::RootState::release(state);
    }
}

bool ObjectRoot::registered() const noexcept {
    return state_ && state_->alive;
}

ObjectReference ObjectRoot::reference() const noexcept {
    return state_ ? state_->object_entries[index_] : ObjectReference{};
}

ValueRoot::ValueRoot(detail::RootState* state, std::uint32_t index) noexcept : state_(state), index_(index) {
    if (state_)
        state_->retain();
}

ValueRoot::~ValueRoot() {
    reset();
}

ValueRoot::ValueRoot(ValueRoot&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)), index_(std::exchange(other.index_, 0)) {}

ValueRoot& ValueRoot::operator=(ValueRoot&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::exchange(other.state_, nullptr);
        index_ = std::exchange(other.index_, 0);
    }
    return *this;
}

void ValueRoot::reset() noexcept {
    detail::assert_serial_access();
    if (state_) {
        auto* state = std::exchange(state_, nullptr);
        const auto index = std::exchange(index_, 0);
        state->remove_value(index);
        detail::RootState::release(state);
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

        std::optional<std::string> default_text{};
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

        Result<void> (*validate)(const void*) noexcept;

        const Type* array{};
    };

    struct Type {
        TypeInfo info;

        NativeLayout native;

        const Type* base{};

        // Cohen's display for O(1) single-inheritance tests: ancestors[0] is core.Object
        // and ancestors[depth] is this type. Append-only type publication leaves existing entries immutable.
        InlineArray<const Type*, 8> ancestors;

        std::uint32_t depth{};

        // Nearest ancestor depth (<= depth) requiring a non-offset native base_address callback, or 0.
        std::uint32_t native_base_depth{};

        // Cumulative constant base-subobject byte offset above native_base_depth.
        std::size_t base_root_offset{};

        // Own declarations, including hidden names, for inherited conflict checks.
        std::vector<std::string> member_names;

        // An internal leading base segment reuses layout/lifetime/trace walkers.
        // Native constructors still own their complete C++ object themselves.
        struct CompiledParameter {
            const Type* type{};

            ParameterPassing passing{ParameterPassing::value};

            std::uint8_t scalar_variant{0xff};

            std::size_t scratch_offset{};
        };

        struct CompiledMethod {
            Invoker invoke{};

            const Type* result_type{};

            bool record_result{};

            bool receiver_read_only{};

            bool valid{};

            std::size_t scratch_size{};

            std::size_t scratch_alignment{alignof(double)};

            InlineArray<CompiledParameter, 8> parameters;
        };

        Array<FieldStorage> fields;

        Array<CompiledMethod> compiled_methods;

        Map<StringView, FieldId> field_by_name;

        Map<StringView, MethodId> method_by_name;

        std::uint8_t scalar_variant_index{0xff};

        Array<std::uint32_t> trace_fields;

        std::size_t trace_depth{1};

        Array<std::uint32_t> destroy_fields;

        std::size_t walk_depth{1};

        bool needs_destroy{};

        // Only built-in scalars and padding-free homogeneous described layouts qualify.
        const Type* scalar_leaf{};

        // Pre-initialized CDO byte image for side-effect-free described types (scalars, enums,
        // math values, plain structs, ReferenceSlots and default_leaves). Enables single-memcpy
        // default construction, copy construction and assignment.
        std::byte* prototype{};

        // Every hard-bounded leaf (and Array of them), so whole-value writes and validate() can enforce them.
        Array<ConstrainedLeaf> constrained_leaves;

        Result<void> (*validate)(const void*) noexcept {};

        // Post-order: base segment, inline records and Arrays innermost first, the type's own validator last.
        Array<ValidatorStep> validators;

        // Non-trivial values only: byte ranges (null swap) and native swaps that exchange two
        // values without throwing. Empty when the type cannot, so writes fall back to assignment.
        struct SwapPart {
            std::size_t offset;

            std::size_t size;

            void (*swap)(void*, void*) noexcept;
        };

        Array<SwapPart> swap_parts;

        Type() = default;

        Type(const Type&) = delete;

        Type& operator=(const Type&) = delete;

        ~Type() {
            if (prototype)
                ::operator delete(prototype, std::align_val_t{info.alignment});
        }
    };

    // Hot object-identity and write-stamp fields packed into half a 64-byte cache line.
    // Cold GC mark bitmaps and gray-list links live out-of-line.
    struct alignas(32) Slot {
        void* storage{};

        std::uint64_t generation{1};

        // write_epoch of the latest write; see stamp().
        std::uint64_t written{};

        std::uint32_t type_index{no_slot};

        std::uint32_t next_free{no_slot};
    };

    static_assert(sizeof(Slot) == 32);

    struct GrayLink {
        std::uint32_t next{no_slot};

        std::uint32_t previous{no_slot};
    };

    static_assert(sizeof(GrayLink) == 8);

    std::uint64_t context{next_context.fetch_add(1, std::memory_order_relaxed)};

    Array<std::unique_ptr<Type>> types;

    // Published C++ representation -> types index; registration keeps representations unique.
    Map<const void*, std::uint32_t> representations;

    // Published name -> types index. Keys view the immutable name inside each heap Type.
    Map<StringView, std::uint32_t> names;

    Array<Slot> slots;

    BitVector live_slots;

    BitVector marked_slots;

    Array<GrayLink> gray_links;

    // Two-level hierarchical write-stamp tracking: each WritePage tracks the exact 64-bit masks
    // of its two newest write epochs; each write_super_pages entry summarizes 64 pages (4096 slots).
    static constexpr unsigned write_page_shift = 6;

    static constexpr unsigned write_super_page_shift = 12;

    struct WritePage {
        std::uint64_t newest_epoch{};

        std::uint64_t previous_epoch{};

        std::uint64_t floor_epoch{};

        std::uint64_t newest_mask{};

        std::uint64_t previous_mask{};
    };

    std::uint64_t write_epoch{1};

    Array<WritePage> write_pages;

    Array<std::uint64_t> write_super_pages;

    void stamp(std::uint32_t index) noexcept {
        const auto current = write_epoch;
        slots.data()[index].written = current;
        const auto bit = std::uint64_t{1} << (index & 63);
        auto& page = write_pages.data()[index >> write_page_shift];
        if (page.newest_epoch == current) [[likely]] {
            page.newest_mask |= bit;
            return;
        }
        page.floor_epoch = page.previous_epoch;
        page.previous_epoch = page.newest_epoch;
        page.previous_mask = page.newest_mask;
        page.newest_epoch = current;
        page.newest_mask = bit;
        write_super_pages.data()[index >> write_super_page_shift] = current;
    }

    // Handles keep only this registry alive, never the Runtime or its objects.
    detail::RootState* roots{new detail::RootState()};

    std::uint32_t free_head{no_slot};

    std::size_t live{};

    std::size_t callbacks{};

    std::size_t native_borrows{};

    bool changing_references{};

    std::size_t active_executions{};

    RetiredCode* retired_code{};

    TypeId f32;

    TypeId f64;

    TypeId u32;

    TypeId i32;

    TypeId boolean;

    TypeId object;

    CollectionPhase phase{CollectionPhase::idle};

    std::size_t marked{};

    std::size_t pending{};

    std::size_t object_root_cursor{};

    std::size_t object_root_limit{};

    std::size_t value_root_cursor{};

    std::size_t value_root_limit{};

    std::size_t sweep_word{};

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

    Array<TraceFrame> trace_stack;

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

    // Synchronous walks use disjoint suffixes; callbacks may grow this array.
    // No reference into it survives a callback. GC's suspended cursor stays separate.
    Array<WalkFrame> walk_stack;

    struct WalkScope {
        Array<WalkFrame>& stack;

        const std::size_t base{stack.size()};

        ~WalkScope() { stack.resize(base); }
    };

    Impl() {
        walk_stack.reserve(1);
        roots->observer = this;
        roots->object_added = [](void* context, ObjectId object) noexcept {
            static_cast<Impl*>(context)->shade(object);
        };
        roots->value_added = [](void* context, std::uint32_t index) noexcept {
            auto& impl = *static_cast<Impl*>(context);
            impl.value_root_added(index, impl.roots->value_entries[index]);
        };
        roots->value_removing = [](void* context, std::uint32_t index) noexcept {
            static_cast<Impl*>(context)->root_removing(index);
        };
    }

    ~Impl() { detail::RootState::release(roots); }

    void prepare_walk(const Type& type) {
        if (type.scalar_leaf || type.prototype || (type.native.construct && !type.info.contains_references))
            return;
        // Reserve before touching values, including space for failure cleanup/barriers.
        walk_stack.reserve(walk_stack.size() + type.walk_depth);
    }

    bool marking() const noexcept { return phase == CollectionPhase::roots || phase == CollectionPhase::mark; }

    bool is_live(std::uint32_t index, const Slot& slot) const noexcept {
        return slot.storage && (phase != CollectionPhase::sweep || marked_slots.contains(index));
    }

    void cancel_cursor() noexcept {
        trace_stack.clear();
        trace_owner = TraceOwner::none;
        trace_owner_index = no_slot;
    }

    bool is_gray_queued(std::uint32_t index) const noexcept {
        return gray_head == index || gray_links.data()[index].previous != no_slot;
    }

    void remove_gray(std::uint32_t index) noexcept {
        if (gray_head == no_slot || !is_gray_queued(index))
            return;
        auto& link = gray_links.data()[index];
        if (link.previous != no_slot)
            gray_links.data()[link.previous].next = link.next;
        else
            gray_head = link.next;
        if (link.next != no_slot)
            gray_links.data()[link.next].previous = link.previous;
        else
            gray_tail = link.previous;
        link.next = no_slot;
        link.previous = no_slot;
    }

    void queue_gray(std::uint32_t index) noexcept {
        if (is_gray_queued(index))
            return;
        auto& link = gray_links.data()[index];
        link.previous = gray_tail;
        link.next = no_slot;
        if (gray_tail != no_slot)
            gray_links.data()[gray_tail].next = index;
        else
            gray_head = index;
        gray_tail = index;
    }

    void shade(ObjectId object) noexcept {
        if (!marking())
            return;
        const auto* slot = lookup(object);
        if (!slot || marked_slots.test(object.index_))
            return;
        marked_slots.set(object.index_);
        ++marked;
        if (!types.data()[slot->type_index]->trace_fields.empty())
            queue_gray(object.index_);
    }

    void sequence_changed(ObjectId owner) noexcept {
        if (!marking())
            return;
        // shade alone cannot reschedule an already marked, partially scanned object.
        if (trace_owner == TraceOwner::object && trace_owner_index == owner.index_)
            cancel_cursor();
        if (marked_slots.contains(owner.index_))
            queue_gray(owner.index_);
    }

    void remove_root_queue(std::uint32_t index) noexcept {
        if (root_head == no_slot)
            return;
        auto& entry = roots->value_entries[index];
        if (!entry.trace_queued)
            return;
        if (entry.trace_previous != no_slot)
            roots->value_entries[entry.trace_previous].trace_next = entry.trace_next;
        else
            root_head = entry.trace_next;
        if (entry.trace_next != no_slot)
            roots->value_entries[entry.trace_next].trace_previous = entry.trace_previous;
        else
            root_tail = entry.trace_previous;
        entry.trace_next = no_slot;
        entry.trace_previous = no_slot;
        entry.trace_queued = false;
    }

    void value_root_added(std::uint32_t index, detail::RootState::ValueEntry& entry) noexcept {
        if (!marking())
            return;
        if (entry.trace_queued || lookup(entry.value.type)->trace_fields.empty())
            return;
        entry.trace_previous = root_tail;
        entry.trace_next = no_slot;
        entry.trace_queued = true;
        if (root_tail != no_slot)
            roots->value_entries[root_tail].trace_next = index;
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
        walk_stack.add({&type, static_cast<std::byte*>(const_cast<void*>(storage))});
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == frame.type->trace_fields.size()) {
                walk_stack.pop();
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
                walk_stack.add({field.type, static_cast<std::byte*>(address)});
            }
        }
    }

    void start_trace(const Type& type, const void* storage, TraceOwner owner, std::uint32_t index) noexcept {
        if (type.trace_fields.empty())
            return;
        trace_owner = owner;
        trace_owner_index = index;
        // begin_collection reserved the maximum depth of every published layout.
        trace_stack.add({&type, storage});
    }

    void retire_trace_frames() noexcept {
        while (!trace_stack.empty()) {
            auto& top = trace_stack.back();
            if (top.sequence && top.next_element == top.sequence->objects_.size()) {
                top.sequence = nullptr;
                top.next_element = 0;
            }
            if (!top.sequence && top.next_field == top.type->trace_fields.size()) {
                trace_stack.pop();
                continue;
            }
            return;
        }
        cancel_cursor();
    }

    void trace_one() noexcept {
        auto& frame = trace_stack.back();
        if (frame.sequence) {
            shade(frame.sequence->objects_[frame.next_element++]);
            retire_trace_frames();
            return;
        }
        const auto& field = frame.type->fields.data()[frame.type->trace_fields.data()[frame.next_field++]];
        const auto* address = field.address ? field.address(const_cast<void*>(frame.storage))
                                            : static_cast<const std::byte*>(frame.storage) + field.offset;
        // A locator may revoke its ValueRoot. Never resurrect the cancelled borrow.
        if (trace_owner == TraceOwner::none)
            return;
        if (field.value.form == ValueForm::strong_reference) {
            shade(static_cast<const ReferenceSlot*>(address)->object_);
        } else if (field.value.form == ValueForm::strong_sequence) {
            const auto* sequence = static_cast<const ReferenceSequence*>(address);
            if (!sequence->objects_.empty()) {
                frame.sequence = sequence;
                frame.next_element = 0;
                return;
            }
        } else {
            trace_stack.add({field.type, address});
            return;
        }
        retire_trace_frames();
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
            if (object_root_cursor < object_root_limit) {
                const auto found = roots->object_entries.find_index_from(object_root_cursor);
                if (found < object_root_limit) {
                    object_root_cursor = found + 1;
                    shade(roots->object_entries[found].id());
                    return false;
                }
                object_root_cursor = object_root_limit;
            }
            if (value_root_cursor < value_root_limit) {
                const auto found = roots->value_entries.find_index_from(value_root_cursor);
                if (found < value_root_limit) {
                    value_root_cursor = found + 1;
                    value_root_added(static_cast<std::uint32_t>(found), roots->value_entries[found]);
                    return false;
                }
                value_root_cursor = value_root_limit;
            }
            phase = CollectionPhase::mark;
            return false;
        }
        if (phase == CollectionPhase::mark) {
            if (!trace_stack.empty()) {
                CallbackScope traversal_scope{roots->traversals};
                trace_one();
            } else if (root_head != no_slot) {
                const auto index = root_head;
                const auto value = roots->value_entries[index].value;
                remove_root_queue(index);
                start_trace(*lookup(value.type), value.storage.data(), TraceOwner::value_root, index);
            } else if (gray_head != no_slot) {
                const auto index = gray_head;
                remove_gray(index);
                const auto& slot = slots.data()[index];
                start_trace(*types.data()[slot.type_index], slot.storage, TraceOwner::object, index);
            } else {
                // One atomic logical-death commit. No heap-wide invalidation pass.
                roots->marking = false;
                pending = live - marked;
                live = marked;
                if (pending == 0) {
                    phase = CollectionPhase::idle;
                } else {
                    sweep_word = 0;
                    phase = CollectionPhase::sweep;
                }
            }
            return false;
        }
        const auto occupied = live_slots.words();
        const auto marked_words = marked_slots.words();
        assert(pending && sweep_word < occupied.size() && occupied.size() == marked_words.size());
        // Bound sparse scans to 4096 slots per work unit; at most one destructor runs.
        const auto end = std::min(occupied.size(), sweep_word + 64);
        for (; sweep_word != end; ++sweep_word) {
            const auto dead = occupied[sweep_word] & ~marked_words[sweep_word];
            if (!dead)
                continue;
            const auto index = static_cast<std::uint32_t>(sweep_word * 64 + std::countr_zero(dead));
            if (std::has_single_bit(dead))
                ++sweep_word;
            release(index);
            recycle(index);
            if (pending == 0)
                phase = CollectionPhase::idle;
            return true;
        }
        return false;
    }

    Type* lookup(TypeId id) const noexcept {
        if (id.context_ != context || id.index_ >= types.size())
            return nullptr;
        return types.data()[id.index_].get();
    }

    static bool derives_from(const Type& actual, const Type& expected) noexcept {
        return &actual == &expected ||
               (expected.depth < actual.ancestors.size() && actual.ancestors.data()[expected.depth] == &expected);
    }

    // Caller validates the ancestry and protects native address callbacks.
    static void* base_storage(const Type& actual, const Type& expected, void* storage) noexcept {
        if (actual.native_base_depth <= expected.depth)
            return static_cast<std::byte*>(storage) + (actual.base_root_offset - expected.base_root_offset);
        const auto* const* ancestors = actual.ancestors.data();
        for (auto depth = actual.depth; depth > expected.depth; --depth) {
            const auto* type = ancestors[depth];
            if (type->native.base_offset)
                storage = static_cast<std::byte*>(storage) + *type->native.base_offset;
            else if (type->native.base_address)
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
            if (auto assigned = assign(type, destination, source); !assigned) {
                assigned.error().code = ErrorCode::assignment_failed;
                return assigned;
            }
            if (constrained)
                constrain_value(type, range, destination, true);
            return {};
        }
        detail::ValueStorage temporary;
        auto* copy = temporary.prepare(type.info.size, type.info.alignment);
        if (auto made = try_construct(type, copy, source); !made) {
            made.error().code = ErrorCode::assignment_failed;
            return made;
        }
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
        auto passes = [](Result<void> (*validate)(const void*) noexcept, const void* data) {
            return validate(data).has_value();
        };
        if (each_bounded(root, storage, nullptr, within) && each_validator(root, storage, nullptr, passes))
            return std::nullopt;
        std::vector<FieldId> field;
        std::optional<ValidationFailure> found;
        const auto fail = [&](std::optional<Scalar> value, std::string reason) {
            found = ValidationFailure{field, {}, std::move(value), {ErrorCode::argument_mismatch, root.info.name}};
            for (const auto id : field)
                found->names.push_back(member_name(id));
            append_path(found->error.message, field);
            found->error.message += ": " + reason;
            return false;
        };
        auto bound = [&](const FieldRange& range, void* data) {
            Scalar value;
            auto reason = bound_violation(range, data, value);
            return reason.empty() || fail(value, std::move(reason));
        };
        auto rule = [&](Result<void> (*validate)(const void*) noexcept, const void* data) {
            auto checked = validate(data);
            return checked || fail({}, std::move(checked.error().message));
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
            ATELIER_TRY_BIND(value, resolve_value(parameter, parameter.passing != ParameterPassing::value));
            if (parameter.passing != ParameterPassing::value && parameter.passing != ParameterPassing::read_only &&
                parameter.passing != ParameterPassing::reference)
                return failure(ErrorCode::invalid_definition, "Unknown parameter passing mode.");
            if (definition.execution == FunctionExecution::coroutine && parameter.passing != ParameterPassing::value)
                return failure(ErrorCode::invalid_definition, "Coroutine parameters cannot borrow caller storage.");
            signature.parameters.push_back({*value, parameter.passing});
        }
        if (definition.result) {
            ATELIER_TRY_BIND(value, resolve_value(*definition.result));
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
        const auto& slot = slots.data()[id.index_];
        return is_live(id.index_, slot) && slot.generation == id.generation_ ? &slot : nullptr;
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
        type->info.zero_default = true;
        type->scalar_leaf = type.get();
        type->native = {sizeof(T),
                        alignof(T),
                        detail::representation<T>(),
                        [](void* address) noexcept -> Result<void> {
                            std::construct_at(static_cast<T*>(address));
                            return {};
                        },
                        [](void* address) noexcept { std::destroy_at(static_cast<T*>(address)); },
                        [](void* destination, const void* source) noexcept -> Result<void> {
                            std::construct_at(static_cast<T*>(destination), *static_cast<const T*>(source));
                            return {};
                        },
                        [](void* destination, const void* source) noexcept -> Result<void> {
                            *static_cast<T*>(destination) = *static_cast<const T*>(source);
                            return {};
                        }};
        type->native.plain_value = true;
        type->native.trivially_destructible = true;
        type->native.prototype_constructor = type->native.construct;
        type->scalar_variant_index = static_cast<std::uint8_t>(Scalar{T{}}.index());
        representations.try_emplace(type->native.representation, id.index_);
        names.try_emplace(StringView{type->info.name}, id.index_);
        types.add(std::move(type));
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

    static_assert(std::is_trivially_copyable_v<ReferenceSlot> && std::is_implicit_lifetime_v<ReferenceSlot>);

    static void apply_default_leaves(std::byte* destination, const std::vector<Scalar>& leaves) noexcept {
        auto* leaf = destination;
        for (const auto& value : leaves)
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

    // Native callbacks clean their failed destination. Described frames track
    // only completed fields, so reverse cleanup never destroys the failed child.
    Result<void> construct(const Type& type, void* storage, const void* source = nullptr) {
        if (type.native.construct)
            return source ? type.native.copy_construct(storage, source) : type.native.construct(storage);
        if (type.prototype) {
            std::memcpy(storage, source ? source : type.prototype, type.info.size);
            return {};
        }
        if (type.scalar_leaf) {
            construct_scalar_block(type, storage, source);
            return {};
        }
        WalkScope scope{walk_stack};
        walk_stack.add({&type, static_cast<std::byte*>(storage), static_cast<const std::byte*>(source)});
        Result<void> result;
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == frame.type->fields.size()) {
                walk_stack.pop();
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
                        result = field.type->native.copy_construct(destination, origin);
                    else if (field.default_text) {
                        auto text = String::from_utf8(StringView{*field.default_text});
                        if (!text)
                            result = failure(ErrorCode::construction_failed, "Default text is not valid UTF-8.");
                        else
                            std::construct_at(reinterpret_cast<String*>(destination), std::move(*text));
                    } else
                        result = field.type->native.construct(destination);
                    if (!result)
                        break;
                } else if (field.type->prototype) {
                    std::memcpy(destination, origin ? origin : field.type->prototype, field.type->info.size);
                } else if (field.type->scalar_leaf) {
                    construct_scalar_block(*field.type, destination, origin);
                } else {
                    walk_stack.add({field.type, destination, origin});
                    continue;
                }
                if (!origin && !field.default_leaves.empty())
                    apply_default_leaves(destination, field.default_leaves);
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
        if (!result) {
            while (walk_stack.size() != scope.base) {
                const auto index = walk_stack.size() - 1;
                const auto frame = walk_stack[index];
                if (frame.next_field == 0) {
                    walk_stack.pop();
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
        }
        return result;
    }

    Result<void> try_construct(const Type& type, void* storage, const void* source = nullptr) {
        if (type.prototype) {
            std::memcpy(storage, source ? source : type.prototype, type.info.size);
            shade_value(type, storage);
            return {};
        }
        prepare_walk(type);
        CallbackScope scope{callbacks};
        auto result = construct(type, storage, source);
        if (!result) {
            result.error().code = ErrorCode::construction_failed;
            return result;
        }
        shade_value(type, storage);
        return {};
    }

    Result<void> assign(const Type& type, void* storage, const void* source) {
        if (type.native.construct)
            return type.native.copy_assign(storage, source);
        if (type.prototype || type.scalar_leaf) {
            std::memcpy(storage, source, type.info.size);
            return {};
        }
        WalkScope scope{walk_stack};
        walk_stack.add({&type, static_cast<std::byte*>(storage), static_cast<const std::byte*>(source)});
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == frame.type->fields.size()) {
                walk_stack.pop();
                continue;
            }
            ++walk_stack[index].next_field;
            const auto& field = frame.type->fields[frame.next_field];
            auto* destination = frame.storage + field.offset;
            const auto* origin = frame.source + field.offset;
            if (field.value.form == ValueForm::inline_value) {
                if (field.type->native.construct) {
                    ATELIER_TRY(field.type->native.copy_assign(destination, origin));
                } else if (field.type->prototype || field.type->scalar_leaf)
                    std::memcpy(destination, origin, field.type->info.size);
                else
                    walk_stack.add({field.type, destination, origin});
            } else {
                *reinterpret_cast<ReferenceSlot*>(destination) = *reinterpret_cast<const ReferenceSlot*>(origin);
            }
        }
        return {};
    }

    void destroy(const Type& type, void* storage) noexcept {
        if (!type.needs_destroy)
            return;
        if (type.native.destroy) {
            type.native.destroy(storage);
            return;
        }
        WalkScope scope{walk_stack};
        walk_stack.add({&type, static_cast<std::byte*>(storage), nullptr, type.destroy_fields.size()});
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == 0) {
                walk_stack.pop();
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
                walk_stack.add({field.type, address, nullptr, field.type->destroy_fields.size()});
        }
    }

    Result<void> trace(const Type& type, const void* storage, ReferenceVisitor visitor) {
        if (!type.info.contains_references)
            return {};
        prepare_walk(type);
        WalkScope scope{walk_stack};
        walk_stack.add({&type, static_cast<std::byte*>(const_cast<void*>(storage))});
        while (walk_stack.size() != scope.base) {
            const auto index = walk_stack.size() - 1;
            const auto frame = walk_stack[index];
            if (frame.next_field == frame.type->fields.size()) {
                walk_stack.pop();
                continue;
            }
            ++walk_stack[index].next_field;
            const auto& field = frame.type->fields[frame.next_field];
            // Preserve locator order, including scalar fields in reference-bearing native types.
            auto* address = field.address ? field.address(frame.storage) : frame.storage + field.offset;
            if (field.value.form == ValueForm::inline_value) {
                if (field.type->info.contains_references) {
                    walk_stack.add({field.type, static_cast<std::byte*>(address)});
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
        return enumerate();
    }

    bool try_resolve_inline_field(std::uint32_t slot_index, std::span<const FieldId> path, void*& out_data,
                                  const Type*& out_type, const FieldRange*& out_range) const noexcept {
        const auto& slot = slots.data()[slot_index];
        const Type* actual = types.data()[slot.type_index].get();
        void* data = slot.storage;
        const FieldRange* range = nullptr;
        for (std::size_t depth = 0; depth != path.size(); ++depth) {
            const FieldId step = path[depth];
            const auto* owner = lookup(step.owner_);
            if (!owner || actual->info.kind == TypeKind::array || owner->info.kind == TypeKind::array ||
                !derives_from(*actual, *owner) || step.index_ >= owner->fields.size()) [[unlikely]]
                return false;
            const auto& access = owner->fields.data()[step.index_];
            if (access.address || actual->native_base_depth > owner->depth ||
                access.value.form != ValueForm::inline_value) [[unlikely]]
                return false;
            data = static_cast<std::byte*>(data) + (actual->base_root_offset - owner->base_root_offset) + access.offset;
            range = access.hard ? access.hard.get() : actual->info.math ? range : nullptr;
            actual = access.type;
        }
        out_data = data;
        out_type = actual;
        out_range = range;
        return true;
    }

    static Result<Scalar> read_scalar_bytes(const Type& type, const void* source) noexcept {
        switch (type.scalar_variant_index) {
        case 0:
            return Scalar{*static_cast<const float*>(source)};
        case 1:
            return Scalar{*static_cast<const std::uint32_t*>(source)};
        case 2:
            return Scalar{*static_cast<const std::int32_t*>(source)};
        case 3: {
            std::uint32_t bits;
            std::memcpy(&bits, source, 4);
            return Scalar{EnumValue{type.info.id, bits}};
        }
        case 4:
            return Scalar{*static_cast<const double*>(source)};
        case 5:
            return Scalar{*static_cast<const bool*>(source)};
        default:
            return failure(ErrorCode::type_mismatch,
                           "Read requires a scalar or enum field; select a field inside the record.");
        }
    }

    static void write_scalar_bytes(void* destination, std::uint8_t variant, const Scalar& value) noexcept {
        switch (variant) {
        case 0:
            *static_cast<float*>(destination) = std::get<float>(value);
            break;
        case 1:
            *static_cast<std::uint32_t*>(destination) = std::get<std::uint32_t>(value);
            break;
        case 2:
            *static_cast<std::int32_t*>(destination) = std::get<std::int32_t>(value);
            break;
        case 3:
            std::memcpy(destination, &std::get<EnumValue>(value).bits, 4);
            break;
        case 4:
            *static_cast<double*>(destination) = std::get<double>(value);
            break;
        case 5:
            *static_cast<bool*>(destination) = std::get<bool>(value);
            break;
        default:
            break;
        }
    }

    static Result<void> validate_call_output(const Type::CompiledMethod& compiled, Scalar* output,
                                             ValueOutput value_output) {
        const auto* result_type = compiled.result_type;
        if (compiled.record_result) {
            if (value_output.type != result_type->info.id) [[unlikely]]
                return failure(ErrorCode::type_mismatch, "Plain value result requires its exact Runtime type.");
            if (!value_output.data || (reinterpret_cast<std::uintptr_t>(value_output.data) &
                                       (result_type->info.alignment - 1)) != 0) [[unlikely]]
                return failure(ErrorCode::invalid_storage, "Plain value result storage is null or misaligned.");
        } else if (value_output.type != TypeId{} || value_output.data) [[unlikely]] {
            return failure(ErrorCode::type_mismatch, "Typed value output requires a plain record result.");
        } else if (result_type && !output) [[unlikely]] {
            return failure(ErrorCode::invalid_storage, "Non-void method requires result storage.");
        }
        return {};
    }

    Result<void> invoke_compiled(Runtime& self, std::uint32_t slot_index, ObjectId object, const Type& actual,
                                 const Type& type, const Type::CompiledMethod& compiled, void* slot_storage,
                                 std::span<const ArgumentView> views, Scalar* output, ValueOutput value_output) {
        if (!compiled.receiver_read_only)
            stamp(slot_index);
        void* receiver = actual.native_base_depth > type.depth ? base_storage(actual, type, slot_storage)
                                                               : static_cast<std::byte*>(slot_storage) +
                                                                     (actual.base_root_offset - type.base_root_offset);
        const auto* result_type = compiled.result_type;
        const auto run = [&](ValueOutput staging) -> Result<void> {
            CallContext context{self, object, receiver, views};
            context.result_value = staging;
            context.expected_result_type = result_type ? result_type->info.id : TypeId{};
            ATELIER_TRY(compiled.invoke(context));
            if (compiled.record_result) {
                if (context.result || !context.result_written || context.result_value.type != staging.type ||
                    context.result_value.data != staging.data) [[unlikely]]
                    return failure(ErrorCode::invocation_failed,
                                   "Method did not produce its declared plain value result.");
                std::memcpy(value_output.data, staging.data, result_type->info.size);
            } else {
                if (context.result_written || context.result_value.data || context.result_value.type != TypeId{})
                    [[unlikely]]
                    return failure(ErrorCode::invocation_failed, "Method produced an undeclared plain value result.");
                if (result_type) {
                    if (!context.result || context.result->index() != result_type->scalar_variant_index ||
                        (result_type->scalar_variant_index == 3 &&
                         std::get<EnumValue>(*context.result).type != result_type->info.id)) [[unlikely]]
                        return failure(ErrorCode::invocation_failed,
                                       "Method did not produce its declared result type.");
                    *output = *context.result;
                } else if (context.result) [[unlikely]] {
                    return failure(ErrorCode::invocation_failed, "Void method produced an undeclared result.");
                }
            }
            return {};
        };
        if (compiled.record_result) {
            if (result_type->info.size <= 128 && result_type->info.alignment <= 32) {
                alignas(32) std::byte staging[128];
                if (result_type->prototype)
                    std::memcpy(staging, result_type->prototype, result_type->info.size);
                else if (result_type->scalar_leaf)
                    std::memset(staging, 0, result_type->info.size);
                else
                    ATELIER_TRY(try_construct(*result_type, staging));
                return run({result_type->info.id, staging});
            }
            detail::ValueStorage staging_owner;
            auto* staging = staging_owner.prepare(result_type->info.size, result_type->info.alignment);
            ATELIER_TRY(try_construct(*result_type, staging));
            return run({result_type->info.id, staging});
        }
        return run({});
    }

    void release(std::uint32_t index) noexcept {
        auto& slot = slots.data()[index];
        remove_gray(index);
        if (trace_owner == TraceOwner::object && trace_owner_index == index)
            cancel_cursor();
        if (is_live(index, slot)) {
            --live;
            if (phase != CollectionPhase::idle && marked_slots.contains(index)) {
                marked_slots.reset(index);
                --marked;
            }
        } else {
            --pending;
        }
        live_slots.reset(index);
        auto* type = types.data()[slot.type_index].get();
        void* storage = std::exchange(slot.storage, nullptr);
        destroy(*type, storage);
        ::operator delete(storage, std::align_val_t{type->info.alignment});
    }
};

Runtime::Runtime() noexcept {
    detail::assert_serial_access();
    impl_ = std::make_unique<Impl>();
    if (impl_->context == 0)
        fatal("Runtime identity space exhausted.");
    impl_->f32 = impl_->add_scalar<float>("core.f32");
    impl_->u32 = impl_->add_scalar<std::uint32_t>("core.u32");
    impl_->i32 = impl_->add_scalar<std::int32_t>("core.i32");
    impl_->f64 = impl_->add_scalar<double>("core.f64");
    static_assert(sizeof(bool) == 1 && alignof(bool) == 1);
    impl_->boolean = impl_->add_scalar<bool>("core.bool");
    impl_->object = TypeId{impl_->context, static_cast<std::uint32_t>(impl_->types.size())};
    auto root = std::make_unique<Impl::Type>();
    root->info = {impl_->object, "core.Object", TypeKind::object, 1, 1, {}, {}};
    root->ancestors.add(root.get());
    impl_->names.try_emplace(StringView{root->info.name}, impl_->object.index_);
    impl_->types.add(std::move(root));
    const auto registered = register_types(math::type_definitions(*this));
    if (!registered)
        fatal(registered.error().message);
    const auto classify = [&]<class T>() {
        static_assert(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>);
        constexpr auto descriptor = math::type_of<T>;
        const auto identity = native_type<T>();
        if (!identity)
            fatal("Builtin math type has no native identity.");
        auto& info = impl_->lookup(*identity)->info;
        if (info.kind != TypeKind::record || info.contains_references || !info.copy_constructible ||
            !info.copy_assignable || info.size != descriptor.size() || info.alignment != descriptor.scalar_size() ||
            info.fields.size() != descriptor.components())
            fatal("Builtin math layout does not match its execution description.");
        const auto scalar = descriptor.precision == math::Precision::f64 ? impl_->f64 : impl_->f32;
        for (std::size_t index = 0; index < info.fields.size(); ++index) {
            const auto& field = info.fields[index];
            if (field.value != ValueDescription{scalar} || field.offset != index * descriptor.scalar_size())
                fatal("Builtin math components do not match their native layout.");
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
    detail::assert_serial_access();
    if (!impl_->roots->alive || impl_->callbacks != 0 || impl_->roots->traversals != 0)
        return failure(ErrorCode::busy,
                       "Generated execution cannot enter during a native borrow, traversal or shutdown.");
    return {};
}

bool Runtime::try_enter_execution() noexcept {
    detail::assert_serial_access();
    if (!impl_->roots->alive || impl_->callbacks != 0 || impl_->roots->traversals != 0 ||
        impl_->active_executions == std::numeric_limits<std::size_t>::max())
        return false;
    ++impl_->active_executions;
    return true;
}

Result<void> Runtime::enter_execution() {
    if (try_enter_execution())
        return {};
    ATELIER_TRY(check_execution_entry());
    return failure(ErrorCode::capacity_exceeded, "Execution nesting space exhausted.");
}

void Runtime::leave_execution() noexcept {
    detail::assert_serial_access();
    if (--impl_->active_executions != 0)
        return;
    while (auto* code = impl_->retired_code) {
        impl_->retired_code = code->next;
        code->destroy(code);
    }
}

void Runtime::retire_code(RetiredCode& code) noexcept {
    detail::assert_serial_access();
    if (impl_->active_executions == 0) {
        code.destroy(&code);
        return;
    }
    code.next = impl_->retired_code;
    impl_->retired_code = &code;
}

Result<void> Runtime::enter_execution_inspection() {
    ATELIER_TRY(check_execution_entry());
    if (impl_->active_executions == 0)
        return failure(ErrorCode::busy, "Execution inspection requires an active generated invocation.");
    // Share reference traversal's read-only admission, including root copying
    // through RootState. This is not a new execution or a native storage borrow.
    ++impl_->callbacks;
    ++impl_->roots->traversals;
    return {};
}

void Runtime::leave_execution_inspection() noexcept {
    detail::assert_serial_access();
    --impl_->roots->traversals;
    --impl_->callbacks;
}

Result<void> Runtime::check_code_publication() const {
    ATELIER_TRY(check_execution_entry());
    if (impl_->active_executions != 0)
        return failure(ErrorCode::busy, "Code publication requires all generated invocations to return.");
    return {};
}

Result<void> Runtime::begin_collection() {
    detail::assert_serial_access();
    if (impl_->callbacks != 0 || impl_->active_executions != 0 || !impl_->roots->alive ||
        impl_->phase != CollectionPhase::idle) {
        return failure(ErrorCode::busy, "Collection can begin only at an idle host safe point.");
    }
    impl_->trace_stack.reserve(impl_->maximum_trace_depth);
    impl_->marked_slots.clear();
    impl_->marked_slots.resize(impl_->slots.size(), false);
    impl_->cancel_cursor();
    impl_->marked = 0;
    impl_->pending = 0;
    impl_->object_root_cursor = 0;
    impl_->object_root_limit = impl_->roots->object_entries.extent();
    impl_->value_root_cursor = 0;
    impl_->value_root_limit = impl_->roots->value_entries.extent();
    impl_->roots->marking = true;
    impl_->phase = CollectionPhase::roots;
    return {};
}

Result<CollectionProgress> Runtime::collect_step(CollectionBudget budget) {
    detail::assert_serial_access();
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
    detail::assert_serial_access();
    impl_->roots->alive = false;
    impl_->roots->marking = false;
    impl_->roots->observer = nullptr;
    impl_->roots->object_added = nullptr;
    impl_->roots->value_added = nullptr;
    impl_->roots->value_removing = nullptr;
    CallbackScope scope{impl_->callbacks};
    for (std::uint32_t index = 0; index < impl_->slots.size(); ++index) {
        if (impl_->slots.data()[index].storage)
            impl_->release(index);
    }
}

Result<TypeId> Runtime::register_type(TypeDefinition definition) {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(std::move(definition));
    ATELIER_TRY_BIND(published, register_types(std::move(definitions)));
    return published->front();
}

Result<FunctionSignature> Runtime::resolve_signature(const SignatureDefinition& definition) const {
    return impl_->resolve_signature(definition);
}

struct detail::TypePublication::State {
    std::uint64_t context{};

    std::size_t first_index{};

    Array<std::unique_ptr<Runtime::Impl::Type>> staged;

    Array<const TypeInfo*> catalog;

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
    assert_serial_access();
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
    assert_serial_access();
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
        impl.names.try_emplace(StringView{type->info.name}, index);
        impl.types.add(std::move(type));
    }
    state_->staged.clear();
    state_->committed = true;
}

Result<std::vector<TypeId>> Runtime::register_types(std::vector<TypeDefinition> definitions) {
    ATELIER_TRY_BIND(publication, prepare_types(std::move(definitions)));
    std::vector<TypeId> result;
    for (const auto& type : (*publication)->state_->staged)
        result.push_back(type->info.id);
    ATELIER_TRY((*publication)->reserve_commit(*this));
    (*publication)->commit(*this);
    return result;
}

Result<void> normalize_metadata_entries(MetadataList& metadata) {
    for (const auto& [key, value] : metadata) {
        bool start = true;
        bool valid = key.find('.') != std::string::npos;
        for (const char c : key) {
            if (c == '.') {
                valid &= !start;
                start = true;
            } else {
                valid &=
                    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (!start && c >= '0' && c <= '9');
                start = false;
            }
        }
        if (!valid || start)
            return failure(ErrorCode::invalid_definition, "Meta keys require dotted ASCII identifiers");
        if (!StringView{value}.is_valid_utf8())
            return failure(ErrorCode::invalid_definition, "Meta values require valid UTF-8");
    }
    std::ranges::sort(metadata);
    for (std::size_t i = 1; i < metadata.size(); ++i)
        if (metadata[i - 1].first == metadata[i].first)
            return failure(ErrorCode::invalid_definition, "Duplicate Meta key: " + metadata[i].first);
    return {};
}

Result<void> FieldDefinition::normalize_metadata() {
    return normalize_metadata_entries(metadata);
}

Result<std::shared_ptr<detail::TypePublication>> Runtime::prepare_types(std::vector<TypeDefinition> definitions) {
    detail::assert_serial_access();
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
        if (!StringView{definition.name}.is_valid_utf8())
            return failure(ErrorCode::invalid_definition, "Type names require valid UTF-8.");
        const auto invalid_description = [](const std::string& owner) {
            return failure(ErrorCode::invalid_definition,
                           "Description requires valid UTF-8 without controls other than line breaks and tabs: " +
                               owner);
        };
        if (!normalize_description(definition.description))
            return invalid_description(definition.name);
        for (auto& field : definition.fields) {
            if (!StringView{field.name}.is_valid_utf8() || !StringView{field.label}.is_valid_utf8() ||
                !StringView{field.category}.is_valid_utf8())
                return failure(ErrorCode::invalid_definition,
                               "Field names, labels and categories require valid UTF-8.");
            if (!normalize_description(field.description))
                return invalid_description(definition.name + "." + field.name);
            const auto normalized = field.normalize_metadata();
            if (!normalized)
                return failure(normalized.error().code,
                               normalized.error().message + ": " + definition.name + "." + field.name);
        }
        for (auto& method : definition.methods) {
            if (!StringView{method.name}.is_valid_utf8())
                return failure(ErrorCode::invalid_definition, "Method names require valid UTF-8.");
            if (!normalize_description(method.description))
                return invalid_description(definition.name + "." + method.name);
        }
        for (auto& method : definition.described_methods) {
            if (!StringView{method.name}.is_valid_utf8())
                return failure(ErrorCode::invalid_definition, "Method names require valid UTF-8.");
        }
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
        if (definition.fields.size() >= no_slot || definition.methods.size() >= no_slot ||
            definition.described_methods.size() >= no_slot) {
            return failure(ErrorCode::capacity_exceeded, "Member identity space exhausted.");
        }
        const bool is_native = native.size || native.alignment || native.representation || native.construct ||
                               native.destroy || native.copy_construct || native.copy_assign || native.swap ||
                               native.base_address || native.base_offset || native.element_representation ||
                               native.sequence_view || native.sequence_mutate ||
                               native.value_operations_preserve_borrows || native.plain_value ||
                               native.trivially_destructible || native.prototype_constructor;
        if (is_native && (!native.size || !std::has_single_bit(native.alignment) || !native.representation ||
                          !native.construct || !native.destroy || native.size % native.alignment != 0)) {
            return failure(ErrorCode::invalid_definition,
                           "Native layout requires valid size, alignment and lifecycle operations.");
        }
        if (is_native && !definition.described_methods.empty()) {
            return failure(ErrorCode::invalid_definition,
                           "Native types publish executable methods, not described method declarations.");
        }
        if (definition.kind != TypeKind::object &&
            (!definition.methods.empty() || !definition.described_methods.empty())) {
            return failure(ErrorCode::invalid_definition, "This slice supports methods on object types only.");
        }
        if (definition.kind != TypeKind::object && (definition.base || native.base_address || native.base_offset)) {
            return failure(ErrorCode::invalid_definition, "Value types cannot declare an object base.");
        }
        if ((definition.kind == TypeKind::enumeration) != definition.enumeration.has_value())
            return failure(ErrorCode::invalid_definition, "Enumeration metadata must match its type kind.");
        if (definition.kind == TypeKind::enumeration &&
            (!definition.fields.empty() || !definition.methods.empty() || definition.base || native.base_address ||
             native.base_offset ||
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
        staged.add(std::move(type));
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
            if (definition.enumeration->flags && underlying->info.id != impl_->u32)
                return failure(ErrorCode::invalid_definition, "Flags enums require core.u32 underlying type.");
            const auto& values = definition.enumeration->values;
            auto& enum_metadata = definition.enumeration->metadata;
            if (!enum_metadata.empty() && enum_metadata.size() != values.size())
                return failure(ErrorCode::invalid_definition,
                               "Enumerator metadata must match the enumerators of: " + type.info.name);
            for (std::size_t e = 0; e < values.size(); ++e) {
                if (values[e].first.empty())
                    return failure(ErrorCode::invalid_definition, "Enum names cannot be empty.");
                if (!StringView{values[e].first}.is_valid_utf8())
                    return failure(ErrorCode::invalid_definition, "Enum names require valid UTF-8.");
                for (std::size_t j = 0; j < e; ++j)
                    if (values[j].first == values[e].first)
                        return failure(ErrorCode::duplicate_name, "Duplicate enum name: " + values[e].first);
                if (e < enum_metadata.size()) {
                    if (!StringView{enum_metadata[e].label}.is_valid_utf8() ||
                        std::ranges::any_of(enum_metadata[e].label, [](unsigned char c) { return c < 32 || c == 127; }))
                        return failure(ErrorCode::invalid_definition,
                                       "Enumerator labels require valid UTF-8 without control characters: " +
                                           type.info.name + "." + values[e].first);
                    if (!normalize_description(enum_metadata[e].description))
                        return failure(
                            ErrorCode::invalid_definition,
                            "Description requires valid UTF-8 without controls other than line breaks and tabs: " +
                                type.info.name + "." + values[e].first);
                }
            }
            enum_metadata.resize(values.size());
            type.info.enumeration =
                TypeInfo::Enumeration{underlying->info.id, std::move(definition.enumeration->values),
                                      definition.enumeration->flags, std::move(enum_metadata)};
            type.scalar_variant_index = static_cast<std::uint8_t>(Scalar{EnumValue{}}.index());
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
        type.info.base_address = type.native.base_address;
        type.info.base_offset = type.native.base_offset;
        if (type.info.kind == TypeKind::object) {
            type.base = definition.base ? resolve(*definition.base) : impl_->lookup(impl_->object);
            if (!type.base || type.base->info.kind != TypeKind::object)
                return failure(ErrorCode::invalid_definition, "Object base must resolve to an object type.");
            type.info.base = type.base->info.id;
            if (!is_native && type.native.base_offset)
                return failure(ErrorCode::invalid_definition, "Described objects cannot supply native base offsets.");
            if (is_native && type.base->info.id != impl_->object &&
                (!type.base->native.construct || !type.native.base_address)) {
                return failure(ErrorCode::invalid_definition,
                               "Native inheritance requires a native base and its address adapter.");
            }
            if (type.base->info.id == impl_->object && (type.native.base_address || type.native.base_offset))
                return failure(ErrorCode::invalid_definition,
                               "The storage-free Object root has no native base adapter.");
            if (type.base->info.id != impl_->object)
                type.fields.add({type.native.base_offset.value_or(0),
                                 type.native.base_offset ? nullptr : type.native.base_address,
                                 {*type.info.base}});
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
        for (std::size_t method_index = 0; method_index != definition.described_methods.size(); ++method_index) {
            const auto& name = definition.described_methods[method_index].name;
            for (const auto& field : definition.fields) {
                if (field.name == name)
                    return failure(ErrorCode::duplicate_name, "Duplicate member: " + type.info.name + "." + name);
            }
            for (std::size_t previous = 0; previous != method_index; ++previous) {
                if (definition.described_methods[previous].name == name)
                    return failure(ErrorCode::duplicate_name, "Duplicate method: " + type.info.name + "." + name);
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
            const bool text_field = !is_native && type.info.kind == TypeKind::object &&
                                    form == ValueForm::inline_value && target->info.kind == TypeKind::string;
            if (text_field && !field.default_text)
                field.default_text.emplace();
            if (field.default_text && (!text_field || !StringView{*field.default_text}.is_valid_utf8()))
                return failure(ErrorCode::invalid_definition,
                               "Text defaults require a described object's String field and valid UTF-8: " +
                                   type.info.name + "." + field.name);
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
            if (!field.exposed && (field.transient || field.read_only))
                return failure(ErrorCode::invalid_definition, "Hidden fields cannot declare Transient or ReadOnly: " +
                                                                  type.info.name + "." + field.name);
            type.fields.add({field.native_offset.value_or(0), field.native_offset ? nullptr : field.native_address,
                             value, nullptr, std::move(field.default_leaves),
                             field.range.constrains() ? std::make_unique<const FieldRange>(field.range) : nullptr,
                             field.default_text});
            if (field.exposed)
                type.info.fields.push_back({FieldId{id, index}, std::move(field.name), value, field.native_offset,
                                            std::move(published_leaves), std::move(field.label),
                                            std::move(field.category), std::move(field.description), field.range,
                                            field.native_address, id, std::move(field.metadata),
                                            std::move(field.default_text), field.transient, field.read_only});
        }
        const auto validate_parameters = [&](const std::string& method_name, std::size_t count,
                                             std::vector<std::string>& names,
                                             std::vector<MetadataList>& metadata) -> Result<void> {
            const std::string qualified = type.info.name + "." + method_name;
            if (!names.empty() && names.size() != count)
                return failure(ErrorCode::invalid_definition,
                               "Parameter names must match the parameters of method: " + qualified);
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (!StringView{names[i]}.is_valid_utf8())
                    return failure(ErrorCode::invalid_definition, "Parameter names require valid UTF-8: " + qualified);
                if (!names[i].empty() && std::find(names.begin(), names.begin() + i, names[i]) != names.begin() + i)
                    return failure(ErrorCode::duplicate_name,
                                   "Duplicate parameter " + names[i] + " in method: " + qualified);
            }
            names.resize(count);
            if (!metadata.empty() && metadata.size() != count)
                return failure(ErrorCode::invalid_definition,
                               "Parameter metadata must match the parameters of method: " + qualified);
            for (auto& entries : metadata) {
                const auto normalized = normalize_metadata_entries(entries);
                if (!normalized)
                    return failure(normalized.error().code, normalized.error().message + ": " + qualified);
            }
            metadata.resize(count);
            return {};
        };
        type.info.methods.reserve(definition.methods.size());
        for (auto& method : definition.methods) {
            type.member_names.push_back(method.name);
            if (method.name.empty() || !method.invoke) {
                return failure(ErrorCode::invalid_definition, "Methods require a name and implementation.");
            }
            ATELIER_TRY_BIND(signature, impl_->resolve_signature(method.signature, id, staged));
            auto& names = method.parameter_names;
            auto& metadata = method.parameter_metadata;
            ATELIER_TRY(validate_parameters(method.name, signature->parameters.size(), names, metadata));
            auto& defaults = method.parameter_defaults;
            if (!defaults.empty() && defaults.size() != signature->parameters.size())
                return failure(ErrorCode::invalid_definition,
                               "Parameter defaults must match the parameters of method: " + method.name);
            bool seen_default = false;
            for (std::size_t p = 0; p < defaults.size(); ++p) {
                if (!defaults[p]) {
                    if (seen_default)
                        return failure(ErrorCode::invalid_definition,
                                       "Parameter defaults must cover a trailing suffix of method: " + method.name);
                    continue;
                }
                seen_default = true;
                const auto& parameter = signature->parameters[p];
                const auto* target = resolved_type(parameter.type);
                if (!target || parameter.form != ValueForm::inline_value ||
                    parameter.passing == ParameterPassing::reference)
                    return failure(ErrorCode::invalid_definition,
                                   "Parameter defaults require value or read_only inline parameters: " + method.name);
                if (target->info.kind == TypeKind::scalar) {
                    if (std::holds_alternative<EnumValue>(*defaults[p]) ||
                        impl_->scalar_type(*defaults[p]) != target->info.id ||
                        !std::isfinite(range_value(*defaults[p])))
                        return failure(ErrorCode::invalid_definition,
                                       "Scalar parameter default must be a finite value of the exact parameter type: " +
                                           method.name);
                } else if (target->info.kind == TypeKind::enumeration) {
                    if (const auto* enum_val = std::get_if<EnumValue>(&*defaults[p])) {
                        if (enum_val->type != target->info.id)
                            return failure(ErrorCode::invalid_definition,
                                           "Enum parameter default does not match the parameter enum type: " +
                                               method.name);
                    } else {
                        const auto* underlying =
                            target->info.enumeration
                                ? resolved_type(target->info.enumeration->underlying)
                                : resolve(definitions[target->info.id.index_ - first_index].enumeration->underlying);
                        if (!underlying || impl_->scalar_type(*defaults[p]) != underlying->info.id)
                            return failure(ErrorCode::invalid_definition,
                                           "Enum parameter default must match the parameter enum or underlying type: " +
                                               method.name);
                        const std::uint32_t bits =
                            std::holds_alternative<std::int32_t>(*defaults[p])
                                ? std::bit_cast<std::uint32_t>(std::get<std::int32_t>(*defaults[p]))
                                : std::get<std::uint32_t>(*defaults[p]);
                        defaults[p] = Scalar{EnumValue{target->info.id, bits}};
                    }
                } else {
                    return failure(ErrorCode::invalid_definition,
                                   "Parameter defaults support scalar and enum parameters only: " + method.name);
                }
            }
            defaults.resize(signature->parameters.size());
            type.info.methods.push_back({MethodId{id, static_cast<std::uint32_t>(type.info.methods.size())},
                                         std::move(method.name), std::move(*signature), method.preserves_borrows,
                                         std::move(method.description), std::move(names), method.native_entry,
                                         std::move(defaults), std::move(metadata)});
        }
        type.info.described_methods.reserve(definition.described_methods.size());
        for (auto& method : definition.described_methods) {
            type.member_names.push_back(method.name);
            if (method.name.empty())
                return failure(ErrorCode::invalid_definition, "Described methods require a name.");
            auto signature = impl_->resolve_signature(method.signature, id, staged);
            if (!signature)
                return failure(signature.error().code,
                               signature.error().message + ": " + type.info.name + "." + method.name);
            auto& names = method.parameter_names;
            auto& metadata = method.parameter_metadata;
            ATELIER_TRY(validate_parameters(method.name, signature->parameters.size(), names, metadata));
            type.info.described_methods.push_back(
                {std::move(method.name), std::move(*signature), std::move(names), std::move(metadata)});
        }
    }

    // Inline values and the private base segment constrain layout. Reject cycles without
    // consuming the C++ call stack for deeply nested, but valid, definition batches.
    enum class LayoutState { unseen, active, complete };
    struct LayoutFrame {
        std::size_t type;

        std::size_t next_field;
    };
    Array<LayoutState> states;
    states.resize(staged.size(), LayoutState::unseen);
    Array<LayoutFrame> stack;
    for (std::size_t start = 0; start != staged.size(); ++start) {
        if (states[start] == LayoutState::complete)
            continue;
        states[start] = LayoutState::active;
        stack.add({start, 0});
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
                    stack.add({dependency, 0});
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
                type.depth = type.base->depth + 1;
                type.ancestors.reserve(type.depth + 1);
                type.ancestors.append(type.base->ancestors.view());
                type.ancestors.add(&type);
                const bool custom_base = type.native.base_address && !type.native.base_offset;
                type.native_base_depth = custom_base ? type.depth : type.base->native_base_depth;
                type.base_root_offset =
                    custom_base ? 0 : type.base->base_root_offset + type.native.base_offset.value_or(0);
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
                    type.constrained_leaves.add({{}, nullptr, {}, &type});
                if (!element_type->validators.empty())
                    type.validators.add({{}, {}, nullptr, &type});
            }
            type.needs_destroy = is_native && !type.native.trivially_destructible;
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
                            type.constrained_leaves.add({{index}, range, reported({})});
                        else if (target->info.kind == TypeKind::array)
                            type.constrained_leaves.add({{index}, range, reported({}), target});
                        else
                            for (std::uint32_t component = 0; component != target->info.math.components(); ++component)
                                type.constrained_leaves.add(
                                    {{index, component}, range, reported({FieldId{target->info.id, component}})});
                    } else
                        for (const auto& leaf : target->constrained_leaves)
                            type.constrained_leaves.add(
                                {stored(leaf.path), leaf.range, reported(leaf.field), leaf.array});
                    for (const auto& step : target->validators)
                        type.validators.add({stored(step.path), reported(step.field), step.validate, step.array});
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
                        type.destroy_fields.add(static_cast<std::uint32_t>(field_index));
                        type.needs_destroy = true;
                    }
                }
                if (!is_native && value.form == ValueForm::strong_sequence) {
                    type.destroy_fields.add(static_cast<std::uint32_t>(field_index));
                    type.needs_destroy = true;
                }
                if (value.form == ValueForm::strong_reference || value.form == ValueForm::strong_sequence ||
                    (!reference && !target->trace_fields.empty())) {
                    type.trace_fields.add(static_cast<std::uint32_t>(field_index));
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
                type.validators.add({{}, {}, type.validate});
            type.info.has_validation_rules = !type.constrained_leaves.empty() || !type.validators.empty();
            // Direct offsets are published only after every dependent layout is complete.
            if (is_native && type.base && type.base->info.id != impl_->object &&
                type.info.alignment < type.base->info.alignment)
                return failure(ErrorCode::invalid_definition, type.info.name + " guarantees alignment " +
                                                                  std::to_string(type.info.alignment) + ", but base " +
                                                                  type.base->info.name + " requires alignment " +
                                                                  std::to_string(type.base->info.alignment) + ".");
            if (type.native.base_offset && (*type.native.base_offset > type.info.size ||
                                            type.base->info.size > type.info.size - *type.native.base_offset ||
                                            *type.native.base_offset % type.base->info.alignment != 0))
                return failure(ErrorCode::invalid_definition,
                               "Direct base offset exceeds its type layout or alignment.");
            const std::size_t first_own_field = type.base && type.base->info.id != impl_->object ? 1u : 0u;
            for (std::size_t field_index = first_own_field; field_index != type.fields.size(); ++field_index) {
                const auto& storage = type.fields[field_index];
                if (storage.address)
                    continue;
                const bool inline_value = storage.value.form == ValueForm::inline_value;
                const auto size = inline_value                                       ? storage.type->info.size
                                  : storage.value.form == ValueForm::strong_sequence ? sizeof(ReferenceSequence)
                                                                                     : sizeof(ReferenceSlot);
                const auto alignment = inline_value                                       ? storage.type->info.alignment
                                       : storage.value.form == ValueForm::strong_sequence ? alignof(ReferenceSequence)
                                                                                          : alignof(ReferenceSlot);
                if (storage.offset > type.info.size || size > type.info.size - storage.offset ||
                    storage.offset % alignment != 0 || type.info.alignment < alignment)
                    return failure(ErrorCode::invalid_definition,
                                   "Direct field offset exceeds its type layout or alignment.");
            }
            if (!is_native) {
                for (auto& field : type.info.fields)
                    if (field.id.owner_ == type.info.id)
                        field.offset = type.fields[field.id.index_].offset;
            }
            if (is_native && type.info.kind == TypeKind::record && type.info.plain_value &&
                type.native.prototype_constructor != nullptr &&
                type.native.prototype_constructor == type.native.construct) {
                auto* scratch =
                    static_cast<std::byte*>(::operator new(type.info.size, std::align_val_t{type.info.alignment}));
                std::memset(scratch, 0, type.info.size);
                bool constructed_ok = false;
                {
                    CallbackScope scope{impl_->callbacks};
                    constructed_ok = type.native.construct(scratch).has_value();
                }
                if (constructed_ok) {
                    type.info.zero_default =
                        std::all_of(scratch, scratch + type.info.size, [](std::byte b) { return b == std::byte{0}; });
                    CallbackScope scope{impl_->callbacks};
                    type.native.destroy(scratch);
                }
                ::operator delete(scratch, std::align_val_t{type.info.alignment});
            }
            if (!is_native) {
                const auto size = aligned_size(std::max(std::size_t{1}, type.info.size), type.info.alignment);
                if (!size)
                    return std::unexpected(size.error());
                type.info.size = *size;
                if (!type.fields.empty()) {
                    const auto* scalar = type.fields[0].type->scalar_leaf;
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
                if (type.info.kind == TypeKind::record && type.info.plain_value &&
                    (type.fields.empty() || type.scalar_leaf != nullptr))
                    type.info.zero_default = true;
                if (!type.scalar_leaf && !type.needs_destroy) {
                    bool can_prototype = true;
                    for (const auto& field : type.fields) {
                        if (field.value.form == ValueForm::strong_reference ||
                            field.value.form == ValueForm::weak_reference)
                            continue;
                        if (field.value.form != ValueForm::inline_value || field.default_text) {
                            can_prototype = false;
                            break;
                        }
                        const auto& target = *field.type;
                        const bool target_ready =
                            !target.needs_destroy && target.info.copy_constructible && target.info.copy_assignable &&
                            (target.prototype != nullptr || target.scalar_leaf != nullptr ||
                             (target.info.plain_value && target.native.prototype_constructor != nullptr &&
                              target.native.prototype_constructor == target.native.construct));
                        if (!target_ready) {
                            can_prototype = false;
                            break;
                        }
                    }
                    if (can_prototype) {
                        auto* image = static_cast<std::byte*>(
                            ::operator new(type.info.size, std::align_val_t{type.info.alignment}));
                        std::memset(image, 0, type.info.size);
                        type.prototype = image;
                        for (const auto& field : type.fields) {
                            auto* destination = image + field.offset;
                            if (field.value.form != ValueForm::inline_value) {
                                std::construct_at(reinterpret_cast<ReferenceSlot*>(destination));
                                continue;
                            }
                            if (field.type->prototype)
                                std::memcpy(destination, field.type->prototype, field.type->info.size);
                            else if (field.type->scalar_leaf)
                                impl_->construct_scalar_block(*field.type, destination, nullptr);
                            else {
                                CallbackScope scope{impl_->callbacks};
                                if (auto made = field.type->native.construct(destination); !made) {
                                    made.error().code = ErrorCode::construction_failed;
                                    return std::unexpected(std::move(made.error()));
                                }
                            }
                            if (!field.default_leaves.empty())
                                Impl::apply_default_leaves(destination, field.default_leaves);
                        }
                        type.walk_depth = 1;
                        if (type.info.kind == TypeKind::record && type.info.plain_value)
                            type.info.zero_default = std::all_of(image, image + type.info.size,
                                                                 [](std::byte b) { return b == std::byte{0}; });
                    }
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
                            type.swap_parts.add({field.offset, target.info.size, nullptr});
                        else if (target.swap_parts.empty()) {
                            type.swap_parts.clear();
                            break;
                        } else
                            for (const auto& part : target.swap_parts)
                                type.swap_parts.add({field.offset + part.offset, part.size, part.swap});
                    }
                }
            }
            if (!type.info.fields.empty()) {
                type.field_by_name.reserve(type.info.fields.size());
                for (const auto& field : type.info.fields)
                    type.field_by_name.try_emplace(StringView{field.name}, field.id);
            }
            if (!type.info.methods.empty()) {
                type.method_by_name.reserve(type.info.methods.size());
                for (const auto& method : type.info.methods)
                    type.method_by_name.try_emplace(StringView{method.name}, method.id);
            }
            states[frame.type] = LayoutState::complete;
            stack.pop();
        }
    }

    // Method-only dependencies may be later in the batch. Recheck the shared
    // resolver after all layouts and their GC-reference closure are complete.
    for (std::size_t i = 0; i < staged.size(); ++i) {
        auto& type = *staged[i];
        for (const auto& method : definitions[i].methods) {
            ATELIER_TRY_BIND(signature, impl_->resolve_signature(method.signature, type.info.id, staged));
        }
        for (std::size_t m = 0; m < definitions[i].described_methods.size(); ++m) {
            auto signature =
                impl_->resolve_signature(definitions[i].described_methods[m].signature, type.info.id, staged);
            if (!signature)
                return failure(signature.error().code, signature.error().message + ": " + type.info.name + "." +
                                                           type.info.described_methods[m].name);
        }
        const std::size_t inherited = type.base ? type.base->info.methods.size() : 0;
        type.compiled_methods.reserve(definitions[i].methods.size());
        for (std::size_t m = 0; m < definitions[i].methods.size(); ++m) {
            const auto& method_info = type.info.methods[inherited + m];
            const auto& signature = method_info.signature;
            Impl::Type::CompiledMethod compiled;
            compiled.invoke = definitions[i].methods[m].invoke;
            compiled.result_type = signature.result ? resolved_type(signature.result->type) : nullptr;
            compiled.record_result = compiled.result_type && compiled.result_type->info.kind == TypeKind::record &&
                                     compiled.result_type->info.plain_value;
            compiled.receiver_read_only = signature.receiver->read_only;
            bool valid = true;
            std::size_t scratch_size = 0;
            std::size_t scratch_alignment = alignof(double);
            compiled.parameters.reserve(signature.parameters.size());
            for (const auto& param : signature.parameters) {
                const auto* expected = resolved_type(param.type);
                const bool scalar = expected && expected->scalar_variant_index != 0xff;
                const bool record_value =
                    expected && expected->info.kind == TypeKind::record && expected->info.plain_value;
                const bool value_borrow =
                    expected && !expected->info.contains_references &&
                    (expected->info.kind == TypeKind::record || expected->info.kind == TypeKind::array ||
                     expected->info.kind == TypeKind::string) &&
                    param.passing != ParameterPassing::value;
                const bool ok =
                    expected && (scalar || record_value || value_borrow) && param.form == ValueForm::inline_value &&
                    (param.passing == ParameterPassing::value || param.passing == ParameterPassing::read_only ||
                     param.passing == ParameterPassing::reference) &&
                    (signature.execution != FunctionExecution::coroutine ||
                     (param.passing == ParameterPassing::value && !record_value));
                std::size_t offset = 0;
                if (ok && (scalar || record_value)) {
                    const auto aligned = aligned_size(scratch_size, expected->info.alignment);
                    if (!aligned || expected->info.size > std::numeric_limits<std::size_t>::max() - *aligned) {
                        valid = false;
                    } else {
                        offset = *aligned;
                        scratch_size = *aligned + expected->info.size;
                        scratch_alignment = std::max(scratch_alignment, expected->info.alignment);
                    }
                }
                valid &= ok;
                compiled.parameters.add(
                    {expected, param.passing, expected ? expected->scalar_variant_index : std::uint8_t{0xff}, offset});
            }
            compiled.valid = valid;
            compiled.scratch_size = scratch_size;
            compiled.scratch_alignment = scratch_alignment;
            type.compiled_methods.add(std::move(compiled));
        }
    }
    auto& catalog = publication->state_->catalog;
    catalog.reserve(first_index + staged.size());
    for (const auto& type : impl_->types)
        catalog.add(&type->info);
    for (const auto& type : staged)
        catalog.add(&type->info);
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
    ATELIER_TRY(validate_reference(reference));
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
    const auto* found = impl_->lookup(type);
    if (!found)
        return failure(ErrorCode::invalid_type, "Type does not belong to this runtime.");
    if (const auto* id = found->field_by_name.find(StringView{name}))
        return *id;
    return failure(ErrorCode::invalid_member, "Unknown field: " + std::string{name});
}

Result<MethodId> Runtime::find_method(TypeId type, std::string_view name) const {
    const auto* found = impl_->lookup(type);
    if (!found)
        return failure(ErrorCode::invalid_type, "Type does not belong to this runtime.");
    if (const auto* id = found->method_by_name.find(StringView{name}))
        return *id;
    return failure(ErrorCode::invalid_member, "Unknown method: " + std::string{name});
}

Runtime::NativeBorrow::NativeBorrow(Runtime& owner, void* address) noexcept : runtime(&owner), storage(address) {
    detail::assert_serial_access();
    ++runtime->impl_->callbacks;
    ++runtime->impl_->native_borrows;
}

Runtime::NativeBorrow::NativeBorrow(NativeBorrow&& other) noexcept
    : runtime(std::exchange(other.runtime, nullptr)), storage(other.storage) {}

Runtime::NativeBorrow::~NativeBorrow() {
    detail::assert_serial_access();
    if (runtime) {
        --runtime->impl_->native_borrows;
        --runtime->impl_->callbacks;
    }
}

Result<Runtime::NativeBorrow> Runtime::borrow_native(ObjectId object, const void* representation,
                                                     std::span<const FieldId> path, bool write) {
    detail::assert_serial_access();
    if (!impl_->roots->alive || impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Native borrowing is unavailable during traversal or shutdown.");
    ATELIER_TRY_BIND(index, resolve_object(object));
    const auto* expected = impl_->native(representation);
    if (!expected)
        return failure(ErrorCode::invalid_type, "Register the native value type before exposing its members.");
    if (!path.empty()) {
        NativeBorrow borrow{*this, nullptr};
        ATELIER_TRY_BIND(field, resolve_field(object, path));
        if (field->value.form != ValueForm::inline_value || field->value.type != expected->info.id)
            return failure(ErrorCode::type_mismatch, "Field does not have the requested native value representation.");
        borrow.storage = field->data;
        if (write)
            impl_->stamp(*index);
        return borrow;
    }
    const auto& slot = impl_->slots.data()[*index];
    const auto* actual = impl_->types.data()[slot.type_index].get();
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
    ATELIER_TRY_BIND(address, resolve_field(object, path));
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
    ATELIER_TRY_BIND(index, resolve_object(object));
    CallbackScope scope{impl_->callbacks};
    const auto& slot = impl_->slots.data()[*index];
    struct Cursor {
        const Impl::Type* type;

        void* storage;

        std::size_t next{};

        std::size_t depth{};
    };
    Array<Cursor> cursors{{impl_->types.data()[slot.type_index].get(), slot.storage}};
    std::vector<FieldId> path;
    while (!cursors.empty()) {
        auto& cursor = cursors.back();
        if (cursor.next == cursor.type->fields.size()) {
            cursors.pop();
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
                cursors.add({field.type, address, 0, path.size()});
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
    ATELIER_TRY_BIND(address, resolve_reference_field(field));
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

Result<void> Runtime::apply_reference_changes(std::span<const ReferenceChange> changes) {
    detail::assert_serial_access();
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

        Array<ObjectId> inserted;
    };
    Array<Prepared> prepared;
    if (changes.size() > prepared.max_size)
        return failure(ErrorCode::capacity_exceeded, "Reference batch is too large.");
    prepared.reserve(changes.size());
    for (const auto& change : changes) {
        const auto& field =
            std::visit([](const auto& operation) -> const ReferenceField& { return operation.field; }, change);
        ATELIER_TRY_BIND(address, resolve_reference_field(field));
        for (const auto& previous : prepared) {
            if (previous.address.data == address->data)
                return failure(ErrorCode::argument_mismatch, "A batch cannot modify the same storage twice.");
        }
        Prepared plan{field.object_, *address, {}, 0, 0, 0, {}};
        if (const auto* write = std::get_if<ReferenceWrite>(&change)) {
            ATELIER_TRY(validate_reference_assignment(address->value, write->value));
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
                ATELIER_TRY(
                    validate_reference_assignment({address->value.type, ValueForm::strong_reference}, reference));
                plan.inserted.add(reference.id());
            }
        }
        prepared.add(std::move(plan));
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
    detail::assert_serial_access();
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
    detail::assert_serial_access();
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
    detail::assert_serial_access();
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
    auto result = impl_->assign(**type, destination.data(), source.data());
    // A failed assignment can still have copied references into earlier fields.
    impl_->shade_value(**type, destination.data());
    if (!result)
        result.error().code = ErrorCode::assignment_failed;
    return result;
}

Result<void> Runtime::destroy_value(TypeId type_id, std::span<std::byte> storage) {
    detail::assert_serial_access();
    if (impl_->roots->traversals != 0 || impl_->changing_references)
        return failure(ErrorCode::busy,
                       "Value mutation is not allowed during reference traversal or batch preparation.");
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    if (impl_->roots->overlaps_storage(storage.data(), (*type)->info.size)) {
        return failure(ErrorCode::busy, "End value-root registrations before destroying their storage.");
    }
    if (!(*type)->needs_destroy)
        return {};
    impl_->prepare_walk(**type);
    CallbackScope scope{impl_->callbacks};
    impl_->destroy(**type, storage.data());
    return {};
}

Result<void> Runtime::mutate_sequence(TypeId type_id, std::span<std::byte> storage, SequenceMutation operation,
                                      TypeId input_type, std::span<const std::byte> input, std::size_t count) {
    detail::assert_serial_access();
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
        ATELIER_TRY_BIND(valid, impl_->value_type(expected, input));
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
    return info.sequence_mutate(operation, storage.data(), input.data(), count);
}

std::uint32_t Runtime::try_resolve_object(ObjectId object) const noexcept {
    if (!belongs_to_context(object) || object.index_ >= impl_->slots.size())
        return no_slot;
    const auto& slot = impl_->slots.data()[object.index_];
    if (!impl_->is_live(object.index_, slot) || slot.generation != object.generation_)
        return no_slot;
    return object.index_;
}

Result<std::uint32_t> Runtime::resolve_object(ObjectId object) const {
    if (!belongs_to_context(object) || object.index_ >= impl_->slots.size()) {
        return failure(ErrorCode::invalid_object, "Object does not belong to this runtime.");
    }
    const auto& slot = impl_->slots.data()[object.index_];
    if (!impl_->is_live(object.index_, slot) || slot.generation != object.generation_) {
        return failure(ErrorCode::invalid_object, "Object has been destroyed or committed for collection.");
    }
    return object.index_;
}

bool NativeContext::try_resolve(Runtime& runtime, ObjectId object, TypeId expected, NativeContext& out) noexcept {
    detail::assert_serial_access();
    auto& impl = *runtime.impl_;
    const auto* type = impl.lookup(expected);
    if (!type || type->info.kind != TypeKind::object)
        return false;
    const auto index = runtime.try_resolve_object(object);
    if (index == no_slot)
        return false;
    const auto& slot = impl.slots.data()[index];
    const auto* actual = impl.types.data()[slot.type_index].get();
    if (!Runtime::Impl::derives_from(*actual, *type))
        return false;
    void* storage = slot.storage;
    if (actual->native_base_depth > type->depth) {
        CallbackScope scope{impl.callbacks};
        storage = Runtime::Impl::base_storage(*actual, *type, storage);
    } else {
        storage = static_cast<std::byte*>(storage) + (actual->base_root_offset - type->base_root_offset);
    }
    out = NativeContext{&runtime, object, storage, &impl.callbacks, index};
    return true;
}

Result<NativeContext> NativeContext::resolve(Runtime& runtime, ObjectId object, TypeId expected) {
    if (NativeContext out; try_resolve(runtime, object, expected, out))
        return out;
    auto& impl = *runtime.impl_;
    ATELIER_TRY_BIND(index, runtime.resolve_object(object));
    if (expected == TypeId{})
        return failure(ErrorCode::type_mismatch, "Receiverless source cannot accept an object.");
    const auto& slot = impl.slots.data()[*index];
    const auto* actual = impl.types.data()[slot.type_index].get();
    const auto* type = impl.lookup(expected);
    if (!type || type->info.kind != TypeKind::object)
        return failure(ErrorCode::invalid_type, "Ancestry requires object types in this Runtime.");
    if (!Runtime::Impl::derives_from(*actual, *type))
        return failure(ErrorCode::type_mismatch, "Source receiver has the wrong type.");
    CallbackScope scope{impl.callbacks};
    return NativeContext{&runtime, object, Runtime::Impl::base_storage(*actual, *type, slot.storage), &impl.callbacks,
                         *index};
}

void NativeContext::stamp() const noexcept {
    detail::assert_serial_access();
    runtime->impl_->stamp(object_index);
}

Result<ObjectRoot> Runtime::create(TypeId type_id) {
    detail::assert_serial_access();
    if (impl_->callbacks != 0)
        return failure(ErrorCode::busy, "Object creation is not allowed during a callback.");
    const auto* type = impl_->lookup(type_id);
    if (!type || type->info.kind != TypeKind::object) {
        return failure(ErrorCode::invalid_type, "Only registered object types have independent identity.");
    }
    if (impl_->free_head == no_slot && impl_->slots.size() >= no_slot) {
        return failure(ErrorCode::capacity_exceeded, "Object identity space exhausted.");
    }
    // No user callback may resize slots, so commit will not allocate after construction.
    if (impl_->free_head == no_slot && impl_->slots.size() == impl_->slots.capacity()) {
        const auto grown =
            std::min<std::size_t>(no_slot, std::max(impl_->slots.size() + 1, impl_->slots.capacity() * 2));
        impl_->slots.reserve(grown);
        impl_->live_slots.reserve(grown);
        impl_->marked_slots.reserve(grown);
        impl_->gray_links.reserve(grown);
    }
    impl_->write_pages.resize(std::max(impl_->write_pages.size(), (impl_->slots.size() >> Impl::write_page_shift) + 1));
    impl_->write_super_pages.resize(
        std::max(impl_->write_super_pages.size(), (impl_->slots.size() >> Impl::write_super_page_shift) + 1));
    void* storage{};
    if (type->prototype) {
        if (!impl_->roots->has_capacity())
            return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
        storage = ::operator new(type->info.size, std::align_val_t{type->info.alignment});
        std::memcpy(storage, type->prototype, type->info.size);
    } else {
        impl_->prepare_walk(*type);
        CallbackScope scope{impl_->callbacks};
        storage = ::operator new(type->info.size, std::align_val_t{type->info.alignment});
        if (auto made = impl_->construct(*type, storage); !made) {
            ::operator delete(storage, std::align_val_t{type->info.alignment});
            made.error().code = ErrorCode::construction_failed;
            return std::unexpected(std::move(made.error()));
        }
        // Construction may retain roots of existing objects, but cannot create object slots.
        if (!impl_->roots->has_capacity()) {
            impl_->destroy(*type, storage);
            ::operator delete(storage, std::align_val_t{type->info.alignment});
            return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
        }
    }
    const auto index = impl_->free_head == no_slot ? static_cast<std::uint32_t>(impl_->slots.size()) : impl_->free_head;
    const auto generation = index == impl_->slots.size() ? std::uint64_t{1} : impl_->slots.data()[index].generation;
    const ObjectId object{impl_->context, index, generation};
    const auto root_index = impl_->roots->add_object(ObjectReference{{type_id, ValueForm::strong_reference}, object});
    ObjectRoot root{impl_->roots, root_index};
    if (index == impl_->slots.size()) {
        impl_->slots.emplace();
        impl_->live_slots.resize(index + 1);
        impl_->gray_links.emplace();
        if (impl_->phase != CollectionPhase::idle)
            impl_->marked_slots.resize(index + 1);
    } else {
        impl_->free_head = impl_->slots.data()[index].next_free;
    }
    auto& slot = impl_->slots.data()[index];
    slot.type_index = type_id.index_;
    slot.storage = storage;
    slot.next_free = no_slot;
    impl_->live_slots.set(index);
    impl_->stamp(index);
    ++impl_->live;
    if (impl_->phase == CollectionPhase::sweep) {
        impl_->marked_slots.set(index);
        ++impl_->marked;
    } else {
        impl_->shade(object);
    }
    return root;
}

Result<void> Runtime::destroy(ObjectId object) {
    detail::assert_serial_access();
    if (impl_->callbacks != 0)
        return failure(ErrorCode::busy, "Object destruction is not allowed during a callback.");
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    {
        CallbackScope scope{impl_->callbacks};
        impl_->release(*index);
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
    return TypeId{impl_->context, impl_->slots.data()[*index].type_index};
}

Result<std::uint64_t> Runtime::change_cursor() {
    detail::assert_serial_access();
    if (impl_->callbacks != 0 || impl_->roots->traversals != 0 || impl_->active_executions != 0)
        return failure(ErrorCode::busy,
                       "Change cursors are taken outside callbacks, native borrows, traversal and execution.");
    return impl_->write_epoch++;
}

bool Runtime::changed_since(ObjectId object, std::uint64_t cursor) const noexcept {
    if (!belongs_to_context(object) || object.index_ >= impl_->slots.size())
        return true;
    const auto& slot = impl_->slots.data()[object.index_];
    return !impl_->is_live(object.index_, slot) || slot.generation != object.generation_ || slot.written > cursor;
}

void Runtime::changed_objects(std::uint64_t cursor, std::vector<ObjectId>& changed) const {
    changed.clear();
    if (impl_->live == 0)
        return;
    const auto live_words = (impl_->phase == CollectionPhase::sweep ? impl_->marked_slots : impl_->live_slots).words();
    const auto page_count = std::min(impl_->write_pages.size(), live_words.size());
    constexpr std::size_t pages_per_super = std::size_t{1} << (Impl::write_super_page_shift - Impl::write_page_shift);
    const auto* pages = impl_->write_pages.data();
    const auto* super_pages = impl_->write_super_pages.data();
    const auto* slots = impl_->slots.data();
    const auto context = impl_->context;
    for (std::size_t sp = 0; sp < impl_->write_super_pages.size(); ++sp) {
        if (super_pages[sp] <= cursor)
            continue;
        const auto page_end = std::min(page_count, (sp + 1) * pages_per_super);
        for (std::size_t page_index = sp * pages_per_super; page_index < page_end; ++page_index) {
            const auto& page = pages[page_index];
            if (page.newest_epoch <= cursor)
                continue;
            const auto base_index = static_cast<std::uint32_t>(page_index << Impl::write_page_shift);
            if (cursor >= page.floor_epoch) {
                const auto dirty =
                    cursor >= page.previous_epoch ? page.newest_mask : (page.newest_mask | page.previous_mask);
                for (auto mask = dirty & live_words.data()[page_index]; mask != 0; mask &= mask - 1) {
                    const auto index = base_index + static_cast<std::uint32_t>(std::countr_zero(mask));
                    changed.push_back(ObjectId{context, index, slots[index].generation});
                }
            } else {
                for (auto mask = live_words.data()[page_index]; mask != 0; mask &= mask - 1) {
                    const auto index = base_index + static_cast<std::uint32_t>(std::countr_zero(mask));
                    const auto& slot = slots[index];
                    if (slot.written > cursor)
                        changed.push_back(ObjectId{context, index, slot.generation});
                }
            }
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
        if (live && !Impl::derives_from(*impl_->types.data()[live->type_index], *type)) {
            return failure(ErrorCode::type_mismatch, "Live object does not match the reference target type.");
        }
    }
    return {};
}

Result<ObjectReference> Runtime::strong_reference(ObjectId object) const {
    detail::assert_serial_access();
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    if (!impl_->roots->alive || impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Cannot add roots during shutdown or reference traversal.");
    return ObjectReference{
        {TypeId{impl_->context, impl_->slots.data()[*index].type_index}, ValueForm::strong_reference}, object};
}

Result<ObjectReference> Runtime::strong_reference(ObjectId object, TypeId declared_type) const {
    detail::assert_serial_access();
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    const auto* actual = impl_->types.data()[impl_->slots.data()[*index].type_index].get();
    const auto* expected = impl_->lookup(declared_type);
    if (!expected || expected->info.kind != TypeKind::object)
        return failure(ErrorCode::invalid_type, "Ancestry requires object types in this Runtime.");
    if (!Impl::derives_from(*actual, *expected))
        return failure(ErrorCode::type_mismatch, "Object does not satisfy the declared root type.");
    if (!impl_->roots->alive || impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Cannot add roots during shutdown or reference traversal.");
    return ObjectReference{{declared_type, ValueForm::strong_reference}, object};
}

Result<ObjectRoot> Runtime::hold(ObjectId object) {
    ATELIER_TRY_BIND(reference, strong_reference(object));
    if (!impl_->roots->has_capacity())
        return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    return ObjectRoot{impl_->roots, impl_->roots->add_object(*reference)};
}

Result<ObjectRoot> Runtime::hold(ObjectId object, TypeId declared_type) {
    ATELIER_TRY_BIND(reference, strong_reference(object, declared_type));
    if (!impl_->roots->has_capacity())
        return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    return ObjectRoot{impl_->roots, impl_->roots->add_object(*reference)};
}

Result<ObjectRoot> Runtime::hold(const ObjectReference& reference) {
    detail::assert_serial_access();
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
    return ObjectRoot{impl_->roots, impl_->roots->add_object(reference)};
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

Result<ObjectReference> Runtime::try_acquire_reference(const ObjectReference& reference) {
    detail::assert_serial_access();
    if (!impl_->roots->alive || impl_->roots->traversals != 0) {
        return failure(ErrorCode::busy, "Cannot acquire references during shutdown or reference traversal.");
    }
    const auto valid = validate_reference(reference);
    if (!valid)
        return std::unexpected(valid.error());
    if (reference.description().form != ValueForm::weak_reference) {
        return failure(ErrorCode::type_mismatch, "try_acquire requires a weak reference.");
    }
    const ObjectId id = !reference.is_null() && impl_->lookup(reference.id()) ? reference.id() : ObjectId{};
    if (id != ObjectId{})
        impl_->shade(id);
    return ObjectReference{{reference.description().type, ValueForm::strong_reference}, id};
}

Result<std::optional<ObjectRoot>> Runtime::try_acquire(const ObjectReference& reference) {
    ATELIER_TRY_BIND(strong, try_acquire_reference(reference));
    if (strong->is_null())
        return std::optional<ObjectRoot>{};
    if (!impl_->roots->has_capacity())
        return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    return std::optional<ObjectRoot>{ObjectRoot{impl_->roots, impl_->roots->add_object(*strong)}};
}

Result<ValueRoot> Runtime::root_value(TypeId type_id, std::span<const std::byte> storage) {
    detail::assert_serial_access();
    if (!impl_->roots->alive || impl_->roots->traversals != 0) {
        return failure(ErrorCode::busy, "Cannot register value roots during shutdown or reference traversal.");
    }
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    if (!impl_->roots->has_capacity())
        return failure(ErrorCode::capacity_exceeded, "Root identity space exhausted.");
    return ValueRoot{impl_->roots,
                     impl_->roots->add_value(detail::ValueRootStorage{type_id, storage.first((*type)->info.size)})};
}

Result<void> Runtime::visit_roots(ReferenceVisitor visitor) const {
    if (!impl_->roots->alive)
        return failure(ErrorCode::busy, "Runtime is shutting down.");
    return impl_->visit_guarded(visitor, [&]() -> Result<void> {
        const auto object_limit = impl_->roots->object_entries.extent();
        for (auto i = impl_->roots->object_entries.find_index_from(0); i < object_limit;
             i = impl_->roots->object_entries.find_index_from(i + 1)) {
            // A visitor may release a root; keep the current descriptor independent.
            const auto copy = impl_->roots->object_entries[i];
            if (copy.is_null())
                continue;
            const auto result = visitor.visit(visitor.context, copy);
            if (!result)
                return result;
        }
        const auto value_limit = impl_->roots->value_entries.extent();
        for (auto i = impl_->roots->value_entries.find_index_from(0); i < value_limit;
             i = impl_->roots->value_entries.find_index_from(i + 1)) {
            const auto rooted = impl_->roots->value_entries[i].value;
            const auto result = impl_->trace(*impl_->lookup(rooted.type), rooted.storage.data(), visitor);
            if (!result)
                return result;
        }
        return {};
    });
}

Result<void> Runtime::visit_references(ObjectId object, ReferenceVisitor visitor) const {
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    const auto& slot = impl_->slots.data()[*index];
    return impl_->visit_guarded(
        visitor, [&] { return impl_->trace(*impl_->types.data()[slot.type_index], slot.storage, visitor); });
}

Result<void> Runtime::visit_references(TypeId type_id, std::span<const std::byte> storage,
                                       ReferenceVisitor visitor) const {
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    return impl_->visit_guarded(visitor, [&] { return impl_->trace(**type, storage.data(), visitor); });
}

std::size_t Runtime::root_count() const noexcept {
    return impl_->roots->object_entries.size() + impl_->roots->value_entries.size();
}

Result<Runtime::Address> Runtime::resolve_field(ObjectId object, std::span<const FieldId> path) const {
    const auto index = resolve_object(object);
    if (!index)
        return std::unexpected(index.error());
    const auto& slot = impl_->slots.data()[*index];
    return resolve_path(TypeId{impl_->context, slot.type_index}, slot.storage, path);
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
    ATELIER_TRY(validate_reference_assignment(address.value, reference));
    commit_reference_write(address, reference.id());
    return {};
}

Result<ObjectReference> Runtime::read_reference(ValueDescription value, const ReferenceSlot& storage) const {
    return read_reference_at({const_cast<ReferenceSlot*>(&storage), value});
}

Result<void> Runtime::write_reference(ValueDescription value, ReferenceSlot& storage,
                                      const ObjectReference& reference) {
    detail::assert_serial_access();
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
    detail::assert_serial_access();
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
    detail::assert_serial_access();
    if (impl_->roots->traversals != 0)
        return failure(ErrorCode::busy, "Reference mutation is not allowed during traversal.");
    const auto address = resolve_storage(type_id, storage, path);
    if (!address)
        return std::unexpected(address.error());
    return write_reference_at(*address, reference);
}

Result<Scalar> Runtime::read(ObjectId object, std::span<const FieldId> path) const {
    if (!path.empty()) [[likely]] {
        const std::uint32_t slot_index = try_resolve_object(object);
        void* data = nullptr;
        const Impl::Type* type = nullptr;
        const FieldRange* range = nullptr;
        if (slot_index != no_slot && impl_->try_resolve_inline_field(slot_index, path, data, type, range)) [[likely]] {
            return Impl::read_scalar_bytes(*type, data);
        }
    }
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    return read_at(*address);
}

Result<void> Runtime::write(ObjectId object, std::span<const FieldId> path, Scalar value) {
    detail::assert_serial_access();
    if (impl_->roots->traversals != 0) [[unlikely]]
        return failure(ErrorCode::busy, "Field mutation is not allowed during reference traversal.");
    if (!path.empty()) [[likely]] {
        const std::uint32_t slot_index = try_resolve_object(object);
        void* data = nullptr;
        const Impl::Type* type = nullptr;
        const FieldRange* range = nullptr;
        if (slot_index != no_slot && impl_->try_resolve_inline_field(slot_index, path, data, type, range)) [[likely]] {
            const std::uint8_t variant = type->scalar_variant_index;
            if (value.index() != variant || (variant == 3 && std::get<EnumValue>(value).type != type->info.id))
                [[unlikely]] {
                impl_->stamp(slot_index);
                return failure(ErrorCode::type_mismatch, "Value type does not match the field.");
            }
            if (range && !constrain_scalar(*range, value)) [[unlikely]] {
                impl_->stamp(slot_index);
                return impl_->located(ErrorCode::argument_mismatch,
                                      TypeId{impl_->context, impl_->slots.data()[slot_index].type_index}, path, {},
                                      "Hard-bounded fields reject NaN and infinity.");
            }
            impl_->stamp(slot_index);
            Impl::write_scalar_bytes(data, variant, value);
            return {};
        }
    }
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
    detail::assert_serial_access();
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
    const auto* type = impl_->lookup(address.value.type);
    if (!type || address.value.form != ValueForm::inline_value)
        return failure(ErrorCode::type_mismatch,
                       "Read requires a scalar or enum field; select a field inside the record.");
    return Impl::read_scalar_bytes(*type, address.data);
}

Result<void> Runtime::write_at(const Address& address, Scalar value) {
    const auto* type = impl_->lookup(address.value.type);
    const std::uint8_t variant = type ? type->scalar_variant_index : std::uint8_t{0xff};
    if (address.value.form != ValueForm::inline_value || value.index() != variant ||
        (variant == 3 && std::get<EnumValue>(value).type != type->info.id))
        return failure(ErrorCode::type_mismatch, "Value type does not match the field.");
    if (address.range && !constrain_scalar(*address.range, value))
        return impl_->located(ErrorCode::argument_mismatch, address.root, address.path, {},
                              "Hard-bounded fields reject NaN and infinity.");
    Impl::write_scalar_bytes(address.data, variant, value);
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
    const auto& slot = impl_->slots.data()[*index];
    const auto& type = *impl_->types.data()[slot.type_index];
    if (!type.info.has_validation_rules)
        return std::nullopt;
    return impl_->check_rules(type, slot.storage);
}

Result<std::optional<ValidationFailure>> Runtime::validate_value(TypeId type_id,
                                                                 std::span<const std::byte> storage) const {
    const auto type = impl_->value_type(type_id, storage);
    if (!type)
        return std::unexpected(type.error());
    if (!(*type)->info.has_validation_rules)
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
    ATELIER_TRY_BIND(address, resolve_field(object, path));
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
    detail::assert_serial_access();
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
    ATELIER_TRY(array.info.sequence_mutate(operation, address->data,
                                           operation == SequenceMutation::insert ? value.data : &operand, position));
    if (copy)
        impl_->constrain_value(element, address->range,
                               Impl::elements(array, address->data).first + position * element.info.size, true);
    return {};
}

Result<void> Runtime::read(ObjectId object, std::span<const FieldId> path, ValueOutput output) const {
    if (!path.empty()) [[likely]] {
        const std::uint32_t slot_index = try_resolve_object(object);
        void* data = nullptr;
        const Impl::Type* type = nullptr;
        const FieldRange* range = nullptr;
        if (slot_index != no_slot && impl_->try_resolve_inline_field(slot_index, path, data, type, range) &&
            type->info.plain_value) [[likely]] {
            if (output.type != type->info.id) [[unlikely]]
                return failure(ErrorCode::type_mismatch,
                               "Whole-value field read requires its exact Runtime type, without GC references.");
            if (!output.data || (reinterpret_cast<std::uintptr_t>(output.data) & (type->info.alignment - 1)) != 0)
                [[unlikely]]
                return failure(ErrorCode::invalid_storage, "Value output storage is null or misaligned.");
            std::memmove(output.data, data, type->info.size);
            return {};
        }
    }
    const auto address = resolve_field(object, path);
    if (!address)
        return std::unexpected(address.error());
    return read_at(*address, output);
}

Result<void> Runtime::write(ObjectId object, std::span<const FieldId> path, ValueInput value) {
    detail::assert_serial_access();
    if (impl_->roots->traversals != 0) [[unlikely]]
        return failure(ErrorCode::busy, "Field mutation is not allowed during reference traversal.");
    if (!path.empty()) [[likely]] {
        const std::uint32_t slot_index = try_resolve_object(object);
        void* data = nullptr;
        const Impl::Type* type = nullptr;
        const FieldRange* range = nullptr;
        if (slot_index != no_slot && impl_->try_resolve_inline_field(slot_index, path, data, type, range) &&
            type->info.plain_value && !range && type->constrained_leaves.empty()) [[likely]] {
            if (value.type != type->info.id) [[unlikely]] {
                impl_->stamp(slot_index);
                return failure(ErrorCode::type_mismatch,
                               "Whole-value field write requires its exact Runtime type, without GC references.");
            }
            if (!value.data || (reinterpret_cast<std::uintptr_t>(value.data) & (type->info.alignment - 1)) != 0)
                [[unlikely]] {
                impl_->stamp(slot_index);
                return failure(ErrorCode::invalid_storage, "Value input storage is null or misaligned.");
            }
            impl_->stamp(slot_index);
            std::memmove(data, value.data, type->info.size);
            return {};
        }
    }
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
    detail::assert_serial_access();
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
    detail::assert_serial_access();
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
    detail::assert_serial_access();
    prepared.views_.clear();
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
    prepared.views_.reserve(arguments.size());
    struct ViewsGuard {
        InlineArray<ArgumentView, 8>& views;

        bool committed{};

        ~ViewsGuard() {
            if (!committed)
                views.clear();
        }
    } guard{prepared.views_};
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
            ATELIER_TRY_BIND(address, resolve_field(argument.object_, argument.path_));
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
            ATELIER_TRY(impl_->try_construct(*expected, destination, data));
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
        prepared.views_.add(
            {parameter.type, data, parameter.passing == ParameterPassing::reference ? writable : nullptr});
    }
    guard.committed = true;
    return {};
}

Result<void> Runtime::call(ObjectId object, MethodId method, std::span<const Scalar> arguments, Scalar* output) {
    return call_scalars(object, method, arguments, output);
}

Result<void> Runtime::call(ObjectId object, MethodId method, std::span<const Scalar> arguments, ValueOutput output) {
    return call_scalars(object, method, arguments, nullptr, output);
}

Result<void> Runtime::call_scalars(ObjectId object, MethodId method, std::span<const Scalar> arguments, Scalar* output,
                                   ValueOutput value_output) {
    detail::assert_serial_access();
    if (impl_->roots->traversals != 0) [[unlikely]]
        return failure(ErrorCode::busy, "Method execution is not allowed during reference traversal.");
    const std::uint32_t slot_index = try_resolve_object(object);
    if (slot_index == no_slot) [[unlikely]]
        return std::unexpected(resolve_object(object).error());
    const auto& slot = impl_->slots.data()[slot_index];
    const auto* actual = impl_->types.data()[slot.type_index].get();
    const auto* type = impl_->lookup(method.owner_);
    if (!type || !Impl::derives_from(*actual, *type) || method.index_ >= type->compiled_methods.size()) [[unlikely]]
        return failure(ErrorCode::invalid_member, "Method does not belong to the receiver type.");
    const auto& compiled = type->compiled_methods.data()[method.index_];
    if (!compiled.valid || compiled.parameters.size() > 8 || compiled.scratch_size > 128 ||
        compiled.scratch_alignment > 32) [[unlikely]] {
        InlineArray<Argument, 8> values;
        values.resize(arguments.size());
        for (std::size_t i = 0; i < arguments.size(); ++i)
            values[i] = Argument{arguments[i]};
        return call_arguments(object, method, values, output, value_output);
    }
    ATELIER_TRY(Impl::validate_call_output(compiled, output, value_output));
    const std::size_t param_count = compiled.parameters.size();
    const auto inherited = type->base ? type->base->info.methods.size() : 0;
    const auto& method_info = type->info.methods[inherited + method.index_];
    if (arguments.size() > param_count ||
        (arguments.size() < param_count && !method_info.parameter_defaults[arguments.size()])) [[unlikely]]
        return failure(ErrorCode::argument_mismatch, "Incorrect argument count.");
    alignas(32) std::byte scratch[128];
    ArgumentView views[8];
    for (std::size_t i = 0; i < param_count; ++i) {
        const auto& param = compiled.parameters.data()[i];
        const Scalar& arg = i < arguments.size() ? arguments[i] : *method_info.parameter_defaults[i];
        if (arg.index() != param.scalar_variant ||
            (param.scalar_variant == 3 && std::get<EnumValue>(arg).type != param.type->info.id)) [[unlikely]]
            return failure(ErrorCode::argument_mismatch, "Argument type does not match parameter " + std::to_string(i));
        if (param.passing == ParameterPassing::reference) [[unlikely]]
            return failure(ErrorCode::argument_mismatch, "Writable reference requires mutable lvalue storage.");
        auto* destination = scratch + param.scratch_offset;
        Impl::write_scalar_bytes(destination, param.scalar_variant, arg);
        views[i] = {param.type->info.id, destination, nullptr};
    }
    CallbackScope scope{impl_->callbacks};
    return impl_->invoke_compiled(*this, slot_index, object, *actual, *type, compiled, slot.storage,
                                  {views, param_count}, output, value_output);
}

Result<void> Runtime::call_arguments(ObjectId object, MethodId method, std::span<const Argument> arguments,
                                     Scalar* output, ValueOutput value_output) {
    detail::assert_serial_access();
    if (impl_->roots->traversals != 0) [[unlikely]]
        return failure(ErrorCode::busy, "Method execution is not allowed during reference traversal.");
    const std::uint32_t slot_index = try_resolve_object(object);
    if (slot_index == no_slot) [[unlikely]]
        return std::unexpected(resolve_object(object).error());
    const auto& slot = impl_->slots.data()[slot_index];
    const auto* actual = impl_->types.data()[slot.type_index].get();
    const auto* type = impl_->lookup(method.owner_);
    if (!type || !Impl::derives_from(*actual, *type) || method.index_ >= type->compiled_methods.size()) [[unlikely]]
        return failure(ErrorCode::invalid_member, "Method does not belong to the receiver type.");
    const auto& compiled = type->compiled_methods.data()[method.index_];
    ATELIER_TRY(Impl::validate_call_output(compiled, output, value_output));
    const auto inherited = type->base ? type->base->info.methods.size() : 0;
    const auto& method_info = type->info.methods[inherited + method.index_];
    const auto& signature = method_info.signature;
    CallbackScope scope{impl_->callbacks};
    const std::size_t param_count = compiled.parameters.size();
    const bool has_field_arg = std::ranges::any_of(
        arguments, [](const Argument& arg) { return arg.object_ != ObjectId{} || !arg.path_.empty(); });
    if (compiled.valid && !has_field_arg && param_count <= 8 && compiled.scratch_size <= 128 &&
        compiled.scratch_alignment <= 32) [[likely]] {
        if (arguments.size() > param_count ||
            (arguments.size() < param_count && !method_info.parameter_defaults[arguments.size()])) [[unlikely]]
            return failure(ErrorCode::argument_mismatch, "Incorrect argument count.");
        alignas(32) std::byte scratch[128];
        ArgumentView views[8];
        for (std::size_t i = 0; i < param_count; ++i) {
            const auto& param = compiled.parameters.data()[i];
            if (i >= arguments.size()) {
                const Scalar& def = *method_info.parameter_defaults[i];
                auto* destination = scratch + param.scratch_offset;
                Impl::write_scalar_bytes(destination, param.scalar_variant, def);
                views[i] = {param.type->info.id, destination, nullptr};
                continue;
            }
            const Argument& argument = arguments[i];
            if (argument.borrowed_) {
                const void* data = argument.data_;
                void* writable = argument.writable_;
                if (!data) [[unlikely]]
                    return failure(ErrorCode::invalid_storage, "Borrowed argument is null.");
                bool type_matches = false;
                if (const auto* by_id = std::get_if<TypeId>(&argument.type_.target_))
                    type_matches = (*by_id == param.type->info.id);
                else if (const auto* by_rep = std::get_if<const void*>(&argument.type_.target_))
                    type_matches = (*by_rep != nullptr && *by_rep == param.type->native.representation);
                else
                    type_matches = (impl_->lookup(argument.type_) == param.type);
                if (!type_matches) [[unlikely]]
                    return failure(ErrorCode::argument_mismatch,
                                   "Argument type does not match parameter " + std::to_string(i));
                if ((reinterpret_cast<std::uintptr_t>(data) & (param.type->info.alignment - 1)) != 0 ||
                    (writable && data != writable)) [[unlikely]]
                    return failure(ErrorCode::invalid_storage, "Borrowed argument storage is invalid or misaligned.");
                if (param.passing == ParameterPassing::reference && !writable) [[unlikely]]
                    return failure(ErrorCode::argument_mismatch, "Writable reference requires mutable lvalue storage.");
                if (param.passing == ParameterPassing::value) {
                    auto* destination = scratch + param.scratch_offset;
                    std::memcpy(destination, data, param.type->info.size);
                    views[i] = {param.type->info.id, destination, nullptr};
                } else {
                    views[i] = {param.type->info.id, data,
                                param.passing == ParameterPassing::reference ? writable : nullptr};
                }
            } else {
                const Scalar& val = argument.value_;
                if (val.index() != param.scalar_variant ||
                    (param.scalar_variant == 3 && std::get<EnumValue>(val).type != param.type->info.id)) [[unlikely]]
                    return failure(ErrorCode::argument_mismatch,
                                   "Argument type does not match parameter " + std::to_string(i));
                if (param.passing == ParameterPassing::reference) [[unlikely]]
                    return failure(ErrorCode::argument_mismatch, "Writable reference requires mutable lvalue storage.");
                auto* destination = scratch + param.scratch_offset;
                Impl::write_scalar_bytes(destination, param.scalar_variant, val);
                views[i] = {param.type->info.id, destination, nullptr};
            }
        }
        return impl_->invoke_compiled(*this, slot_index, object, *actual, *type, compiled, slot.storage,
                                      {views, param_count}, output, value_output);
    }
    PreparedArguments prepared;
    InlineArray<Argument, 8> expanded;
    std::span<const Argument> effective_arguments = arguments;
    if (arguments.size() < signature.parameters.size() && method_info.parameter_defaults[arguments.size()]) {
        expanded.reserve(signature.parameters.size());
        for (const auto& arg : arguments)
            expanded.add(arg);
        for (std::size_t p = arguments.size(); p < signature.parameters.size(); ++p)
            expanded.add(Argument{*method_info.parameter_defaults[p]});
        effective_arguments = expanded;
    }
    ATELIER_TRY(prepare_arguments(signature, effective_arguments, prepared, true));
    return impl_->invoke_compiled(*this, slot_index, object, *actual, *type, compiled, slot.storage, prepared.values(),
                                  output, value_output);
}

std::size_t Runtime::live_objects() const noexcept {
    return impl_->live;
}

}
