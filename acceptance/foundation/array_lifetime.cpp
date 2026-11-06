#include "memory/fatal.hpp"
#include "foundation/array.hpp"
#include "foundation/fixed_array.hpp"
#include "foundation/inline_array.hpp"
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <cstdio>
#include <cstdint>

namespace {
using namespace atelier;

struct alignas(64) AlignedValue {
    std::uint64_t value;
};

unsigned relocation_moves{};

// A trivial default constructor permits implicit lifetime, but must not erase
// the side effects of the selected relocation constructor.
template <bool Default = false>
struct ConstructedValue {
    int value;

    ConstructedValue()
        requires Default
    = default;

    explicit ConstructedValue(int input) noexcept : value(input) {}

    ConstructedValue(const ConstructedValue&) = delete;

    ConstructedValue& operator=(ConstructedValue&&) = default;

    template <class U>
        requires std::same_as<U, ConstructedValue>
    ConstructedValue(U&& other) noexcept : value(other.value) {
        ++relocation_moves;
    }
};

static_assert(detail::ArrayElements<AlignedValue>::byte_reallocate);

static_assert(std::is_trivially_copyable_v<ConstructedValue<>> &&
              !detail::ReallocatableArrayElement<ConstructedValue<>>);

static_assert(detail::ReallocatableArrayElement<ConstructedValue<true>> &&
              !detail::ArrayElements<ConstructedValue<true>>::byte_reallocate);

static_assert(std::is_standard_layout_v<Array<int>> && sizeof(Array<int>) == 24 && alignof(Array<int>) == 8);

void require(bool condition, const char* message) {
    if (!condition)
        fatal(message);
}

template <class ArrayType>
void typed_relocation() {
    ArrayType values;
    values.reserve(3);
    values.emplace(53);
    values.emplace(97);
    const auto moves = relocation_moves;
    values.reserve(11);
    require(relocation_moves == moves + 2 && values[0].value == 53 && values[1].value == 97,
            "Trivially copyable elements bypassed their nontrivial constructors during relocation.");
}

struct Selection {
    int id;

    bool enabled;
};

template <class ArrayType>
void predicate_selection() {
    ArrayType values{{7, false}, {3, true}, {7, true}, {11, false}, {7, false}};
    const auto selected = [] {
        return [key = std::make_unique<int>(7)](const Selection& item) noexcept { return item.id == *key; };
    };
    require(values.contains_if(selected()) && values.find_if(selected()) == 0 && values.find_last_if(selected()) == 4 &&
                values.find_if(&Selection::enabled) == 1 && values.find_last_if(&Selection::enabled) == 2,
            "Move-only predicates or member projections lost matching positions.");
    require(values.remove_first_if(selected()) && values.remove_last_if(selected()) &&
                values.remove_swap_if(selected()) && values.size() == 2 && values[0].id == 3 && values[1].id == 11,
            "Move-only predicate removal changed the surviving values.");
    require(values.contains_if(&Selection::enabled) && values.remove_if(&Selection::enabled) == 1 &&
                values.size() == 1 && values[0].id == 11,
            "Member projection compaction lost the unselected value.");
    values.add({7, true});
    require(values.remove_if(selected()) == 1 && values.remove_all_swap_if(selected()) == 0 &&
                values.remove_all_swap_if(&Selection::enabled) == 0 && values[0].id == 11,
            "Predicate compaction changed an unmatched value.");
}

std::int64_t live_bytes() {
    mi_collect(true);
    mi_stats_t stats;
    mi_stats_init(&stats);
    require(mi_stats_get(&stats), "mimalloc statistics ABI mismatch.");
    return stats.malloc_normal.current + stats.malloc_huge.current;
}

struct InlineOwner {
    InlineArray<std::shared_ptr<InlineOwner>, 2> children;

    bool watched{};

    ~InlineOwner() {
        if (watched && !children.empty())
            fatal("Inline subtree source destroyed before transfer.");
    }
};

void inline_storage() {
    const auto before = live_bytes();
    InlineArray<AlignedValue, 2> values;
    auto* inline_data = values.data();
    values.emplace(17);
    values.emplace(43);
    require(live_bytes() == before && values.capacity() == 2,
            "Inline elements allocated before exhausting inline storage.");
    values.emplace(values[0].value);
    values.append(values.view().slice(1, 2));
    values.reserve(129);
    require(values.size() == 5 && values[2].value == 17 && values[3].value == 43 && values[4].value == 17 &&
                reinterpret_cast<std::uintptr_t>(values.data()) % 64 == 0,
            "Inline spill or heap growth lost aliased values or alignment.");
    auto* heap_data = values.data();
    auto moved = std::move(values);
    require(moved.data() == heap_data && values.empty() && values.data() == inline_data && values.capacity() == 2,
            "Inline heap move failed to transfer storage or reset the source.");
    moved.resize(2);
    moved.squeeze();
    require(moved.capacity() == 2 && moved[0].value == 17 && moved[1].value == 43 && live_bytes() == before,
            "Inline squeeze failed to restore inline storage and release its heap.");
    values = std::move(moved);
    require(values.data() == inline_data && moved.empty() && values[1].value == 43,
            "Inline move borrowed the source object's inline storage.");
    values.reserve(37);
    values.clear();
    require(values.capacity() == 37, "Inline clear released reserved storage.");
    values.release();
    require(values.capacity() == 2 && values.data() == inline_data && live_bytes() == before,
            "Inline release retained heap storage.");

    InlineArray<int, 0> zero;
    zero.add(31);
    zero.release();
    FixedArray<int, 0> fixed_zero;
    fixed_zero.reserve(0);
    fixed_zero.squeeze();
    require(zero.empty() && zero.capacity() == 0 && fixed_zero.empty(), "Zero inline capacity retained a value.");

    InlineArray<std::shared_ptr<InlineOwner>, 2> owners;
    owners.emplace(std::make_shared<InlineOwner>());
    owners[0]->children.emplace(std::make_shared<InlineOwner>());
    auto* child = owners[0]->children[0].get();
    owners[0]->watched = true;
    owners = std::move(owners[0]->children);
    require(owners.size() == 1 && owners[0].get() == child, "Inline subtree promotion lost its child.");
}

void fixed_storage() {
    FixedArray<int, 12> keys{7, 3, 7, 5, 3, 11};
    require(keys.find(7) == 0 && keys.find_last(7) == 2 && keys.index_of(&keys[3]) == 3 &&
                keys.find_if([](int value) { return value > 7; }) == 5 &&
                keys.find_last_if([](int value) { return value < 5; }) == 4 &&
                keys.contains_if([](int value) { return value == 11; }),
            "Array value/predicate search confused the first and last matching index.");
    require(keys.add_unique(7) == 0 && keys.add_unique(13) == 6 && keys.size() == 7,
            "Unique insertion changed an existing entry or returned the wrong index.");
    require(keys.remove_all(keys[1]) == 2 && keys.remove_last(7) && keys.remove_swap(5) && keys.size() == 3 &&
                keys[0] == 7 && keys[1] == 13 && keys[2] == 11,
            "Value removal lost its aliased comparison key or swap ordering.");
    require(keys.remove_first_if([](int value) { return value > 10; }) &&
                keys.remove_last_if([](int value) { return value % 2 == 1; }) && keys.size() == 1 && keys[0] == 7,
            "Predicate removal changed the surviving prefix.");
    keys.add(2);
    keys.add(7);
    keys.add(2);
    keys.add(7);
    require(keys.remove_all_swap_if([](int value) { return value == 7; }) == 3 && keys.size() == 2 && keys[0] == 2 &&
                keys[1] == 2 && keys.remove_all_swap(keys[0]) == 2 && keys.empty(),
            "Unordered removal skipped a swapped entry or lost its aliased comparison key.");

    auto first = std::make_shared<int>(19);
    auto second = std::make_shared<int>(71);
    const auto before = live_bytes();
    FixedArray<std::shared_ptr<int>, 5> values;
    values.add(first);
    values.add(second);
    values.append(values.view());
    values.insert(1, values[3]);
    values.move(4, 0);
    values.remove_at(2, 2);
    require(values.size() == 3 && *values[0] == 71 && *values[1] == 19 && *values[2] == 19,
            "Fixed array edits changed retained resource ordering.");
    auto copy = values;
    auto moved = std::move(values);
    copy.remove_at_swap(0);
    require(copy.remove_if([](const auto& value) { return *value == 19; }) == 2 && copy.empty(),
            "Fixed resource compaction retained removed entries.");
    moved.pop();
    moved.release();
    require(values.empty() && moved.capacity() == 5 && first.use_count() == 1 && second.use_count() == 1 &&
                live_bytes() == before,
            "Fixed array allocated storage or failed to release element ownership.");
}
}

int main() {
    atelier::install_fatal_handler();
    const auto before = live_bytes();
    inline_storage();
    fixed_storage();
    typed_relocation<Array<ConstructedValue<>>>();
    typed_relocation<Array<ConstructedValue<true>>>();
    typed_relocation<InlineArray<ConstructedValue<true>, 1>>();
    predicate_selection<Array<Selection>>();
    predicate_selection<InlineArray<Selection, 2>>();
    predicate_selection<FixedArray<Selection, 5>>();
    {
        const Array<std::uint32_t> source{7, 9};
        const Array<std::uint32_t> copy{source};
        require(copy[0] == 7 && copy[1] == 9, "Array buffer copy lost its input.");
        Array<AlignedValue> aligned;
        aligned.reserve(3);
        aligned.emplace(17);
        aligned.emplace(41);
        aligned.emplace(83);
        aligned.emplace(aligned[1].value);
        aligned.squeeze();
        aligned.append(aligned.view().slice(1, 2));
        aligned.squeeze();
        aligned.resize(137, aligned[2]);
        require(reinterpret_cast<std::uintptr_t>(aligned.data()) % alignof(AlignedValue) == 0 &&
                    aligned[0].value == 17 && aligned[3].value == 41 && aligned[4].value == 41 &&
                    aligned[5].value == 83 && aligned[136].value == 83,
                "Array growth lost alignment, aliased constructor arguments or self-append/fill contents.");
        aligned.reserve(4097);
        aligned.resize(140);
        require(aligned[136].value == 83 && aligned[137].value == 0 && aligned[139].value == 0 &&
                    reinterpret_cast<std::uintptr_t>(aligned.data()) % alignof(AlignedValue) == 0,
                "Array reserve or value initialization changed its live prefix or alignment.");
        aligned.pop();
        require(aligned.size() == 139 && aligned.back().value == 0, "Array stack operations changed the prefix.");
    }
    require(live_bytes() == before, "Array retained live buffers after owner destruction.");
    std::puts("Array buffer lifetime: PASS");
    return 0;
}
