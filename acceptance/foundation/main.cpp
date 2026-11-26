#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "foundation/array.hpp"
#include "foundation/function_ref.hpp"
#include "foundation/map.hpp"
#include "foundation/optional.hpp"
#include "foundation/priority_queue.hpp"
#include "foundation/ring_deque.hpp"
#include "foundation/set.hpp"
#include "foundation/sha256.hpp"
#include "foundation/string.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {
#if _ITERATOR_DEBUG_LEVEL == 0
static_assert(sizeof(atelier::Map<int, int>) == 64 && sizeof(atelier::Set<int>) == 64);
#else
static_assert(sizeof(atelier::Map<int, int>) == 72 && sizeof(atelier::Set<int>) == 72);
#endif

static_assert(sizeof(atelier::Optional<int>) == 8 && alignof(atelier::Optional<int>) == 4);

static_assert(std::is_trivially_copyable_v<atelier::Optional<int>> &&
              std::is_implicit_lifetime_v<atelier::Optional<int>> &&
              atelier::detail::ArrayElements<atelier::Optional<int>>::byte_reallocate);

static_assert(atelier::Optional<int>{7} == 7 && atelier::Optional<int>{} == std::nullopt &&
              atelier::Optional<int>{}.value_or(9) == 9 && atelier::Optional<int>{7}.value_or(9) == 7);

constexpr bool verify_constexpr_views() {
    atelier::StringView path{"project.ui.source"};
    if (!path.strip_prefix("project.") || !path.strip_suffix(".source") || path != "ui")
        return false;
    constexpr atelier::StringView sample{"alpha/beta/gamma"};
    if (sample.first(5) != "alpha" || sample.last(5) != "gamma" || sample.rfind('/') != 10)
        return false;
    const auto [before, sep, after] = sample.rpartition('/');
    if (before != "alpha/beta" || sep != "/" || after != "gamma" || !atelier::StringView{"你好"}.is_valid_utf8())
        return false;
    constexpr int numbers[]{2, 4, 6, 8};
    const atelier::Span<const int> span{numbers};
    return span.front() == 2 && span.back() == 8 && span.first(2)[1] == 4 && span.last(2)[0] == 6 &&
           span.size_bytes() == sizeof(numbers);
}

static_assert(verify_constexpr_views());

void queued_resources() {
    using namespace atelier;
    using acceptance::helpers::require;
    RingDeque<String> queue;
    queue.reserve(3);
    queue.emplace_back(String::literal<"a">());
    queue.emplace_back(String::literal<"b">());
    queue.emplace_back(String::literal<"c">());
    queue.pop_front();
    queue.emplace_back(String::literal<"d">());
    const auto [first, second] = std::as_const(queue).segments();
    require(first.size() == 2 && first.front() == "b" && second.size() == 1 && second.front() == "d",
            "Wrapped queue segments lost logical order.");
    const auto snapshot = queue;
    queue.emplace_front(queue.back());
    queue.pop_back();
    require(queue.size() == 3 && queue[0] == "d" && queue[1] == "b" && queue[2] == "c" && snapshot.front() == "b" &&
                snapshot.back() == "d",
            "Growing a wrapped queue lost a borrowed input.");

    Array<String> forward_items;
    for (const auto& item : std::as_const(queue))
        forward_items.add(item);
    require(forward_items.size() == 3 && forward_items[0] == "d" && forward_items[1] == "b" &&
                forward_items[2] == "c" && (queue.end() - std::as_const(queue).begin()) == 3 &&
                (std::as_const(queue).end() - queue.begin()) == 3 && queue.begin()[2] == "c",
            "RingDeque random-access forward iteration failed.");
    const auto rev_hit = std::find(queue.rbegin(), queue.rend(), StringView{"b"});
    require(rev_hit != queue.rend() && *rev_hit == "b" && *(queue.rbegin()) == "c",
            "RingDeque reverse iteration failed.");

    Array<std::unique_ptr<int>> jobs;
    for (const int rank : {4, 9, 1})
        jobs.emplace(std::make_unique<int>(rank));
    const auto lower = [](const auto& a, const auto& b) { return *a < *b; };
    PriorityQueue<std::unique_ptr<int>, decltype(lower)> ready{std::move(jobs), lower};
    ready.emplace(std::make_unique<int>(7));
    for (const int expected : {9, 7, 4, 1})
        require(*ready.pop() == expected, "Priority queue lost job ownership or order.");
    require(ready.empty() && jobs.empty(), "Queue transfer retained jobs.");
}

// Collisions exercise full key comparison and swap-removal during catalog growth.
struct CatalogHash {
    using is_transparent = void;

    using is_avalanching = void;

    std::size_t operator()(atelier::StringView) const noexcept { return 0; }
};

void bound_catalog() {
    using namespace atelier;
    using acceptance::helpers::require;
    using Names = std::array<StringView, 6>;
    struct ByName {
        using is_transparent = void;

        using is_avalanching = void;

        std::reference_wrapper<const Names> names;

        size_t operator()(size_t i) const noexcept { return Hash<StringView>{}(names.get()[i]); }

        size_t operator()(StringView s) const noexcept { return Hash<StringView>{}(s); }

        bool operator()(size_t i, size_t j) const noexcept { return i == j; }

        bool operator()(StringView s, size_t i) const noexcept { return s == names.get()[i]; }

        bool operator()(size_t i, StringView s) const noexcept { return (*this)(s, i); }
    };
    const Names first{"a", "b", "c", "d", "e", "f"};
    const Names second{"f", "e", "d", "c", "b", "a"};
    Set<size_t, ByName, ByName> selected{ByName{first}, ByName{first}};
    Set<size_t, ByName, ByName> other{ByName{second}, ByName{second}};
    for (size_t i = 0; i < first.size(); ++i)
        require(selected.add(i), "Catalog index insertion failed.");
    auto copy = selected;
    other = std::move(selected);
    require(selected.empty() && selected.add(size_t{0}) && selected.contains(StringView{"a"}) &&
                other.find(StringView{"e"}) && *other.find(StringView{"e"}) == 4 && copy.remove(StringView{"e"}) &&
                other.contains(StringView{"e"}),
            "Moving or copying the catalog lost its policy binding.");
    other.release();
    require(other.empty() && other.add(size_t{5}) && other.contains(StringView{"f"}), "Release lost catalog binding.");
}

void borrowed_name_catalog() {
    using namespace atelier;
    using acceptance::helpers::require;
    std::array<char, 40> bytes{};
    const auto name = [&](std::size_t length) { return StringView{bytes.data(), length}; };
    Map<StringView, std::size_t> fields;
    Set<StringView, CatalogHash> colliding;
    for (std::size_t i = 0; i < 16; ++i)
        fields.try_emplace(name(i), i);
    auto copy = fields;
    for (std::size_t i = 0; i <= bytes.size(); ++i) {
        if (i >= 16)
            fields.try_emplace(name(i), i);
        require(colliding.add(name(i)), "Registering distinct names with a shared hash failed.");
        require(copy.contains(name(i)) == (i < 16), "Copy changed the inline name catalog.");
    }
    for (std::size_t i = 0; i <= bytes.size(); i += 2)
        require(fields.remove(name(i)) && colliding.remove(name(i)), "Removing a registered name failed.");
    for (std::size_t i = 0; i <= bytes.size(); ++i) {
        const auto* field = fields.find(name(i));
        require((i % 2 ? field && *field == i : !field) && colliding.contains(name(i)) == bool(i % 2),
                "Catalog promotion or removal changed another name.");
    }
}

void resource_catalog_growth() {
    using namespace atelier;
    using acceptance::helpers::require;
    Map<String, std::unique_ptr<int>> resources;
    Set<String, CatalogHash> selected;
    for (String name :
         {String::literal<"a">(), String::literal<"b">(), String::literal<"c">(), String::literal<"d">()}) {
        resources.try_emplace(name, std::make_unique<int>(int(resources.size())));
        selected.add(std::move(name));
    }
    auto duplicate = std::make_unique<int>(42);
    auto snapshot = selected;
    require(snapshot.remove(StringView{"c"}) && snapshot.contains(StringView{"d"}) &&
                selected.contains(StringView{"c"}),
            "Copying an unindexed catalog lost the original or the entry moved into the hole.");
    require(!resources.try_emplace(String::literal<"a">(), std::move(duplicate)).inserted && duplicate &&
                !selected.add(String::literal<"a">()),
            "Duplicate resource consumed its input.");
    auto added = resources.try_emplace(String::literal<"e">(), std::move(*resources.find(StringView{"d"})));
    require(added.inserted && **added.value == 3 && selected.add(String::literal<"e">()) &&
                selected.contains(StringView{"e"}) && **resources.find(StringView{"b"}) == 1,
            "Catalog growth lost a new or an existing resource.");
    require(resources.remove(StringView{"b"}) && selected.remove(StringView{"b"}) &&
                !resources.contains(StringView{"b"}) && resources.size() == selected.size(),
            "Catalog removal lost membership.");
    resources.clear();
    resources.try_emplace(String::literal<"only">(), std::move(duplicate));
    require(**resources.find(StringView{"only"}) == 42 && selected.contains(StringView{"d"}),
            "Catalog reuse lost ownership.");
    selected.swap(snapshot);
    auto moved = std::move(selected);
    moved.reserve(64);
    require(moved.size() == 3 && moved.contains(StringView{"a"}) && !moved.contains(StringView{"c"}) &&
                snapshot.contains(StringView{"e"}) && selected.empty() && selected.add(String::literal<"new">()),
            "Catalog swap, move or explicit promotion lost membership.");
    selected.clear();
    require(!selected.contains(StringView{"new"}) && selected.add(String::literal<"fresh">()) &&
                selected.contains(StringView{"fresh"}),
            "Clearing a small catalog retained stale membership.");

    // Removing colliding names leaves holes before live entries further
    // down the probe sequence. Reuse those holes and check both snapshots.
    Set<String, CatalogHash> catalog;
    std::vector<std::string> names;
    for (unsigned i = 0; i < 320; ++i) {
        names.push_back("resource/" + std::to_string(i));
        require(catalog.add(*String::from_utf8(StringView{names.back()})), "Catalog rejected a distinct resource.");
    }
    auto saved = catalog;
    for (unsigned i = 0; i < names.size(); i += 2)
        require(catalog.remove(StringView{names[i]}), "Catalog removal missed a displaced resource.");
    for (unsigned i = 0; i < names.size(); ++i) {
        const StringView name{names[i]};
        require(saved.contains(name) && catalog.contains(name) == (i % 2 != 0), "Catalog collision chain was broken.");
        require(catalog.add(*String::from_utf8(name)) == (i % 2 == 0), "Catalog inserted a duplicate after removal.");
    }
    catalog.clear();
    for (const auto& name : names)
        require(!catalog.contains(StringView{name}), "Cleared catalog retained a colliding resource.");
}

template <class M = atelier::Map<int, int>>
constexpr bool map_rvalue_at = requires(M map) { std::move(map).at(1); };

template <class S = atelier::Set<int>>
constexpr bool set_rvalue_view = requires(S set) { std::move(set).view(); };

template <class D = atelier::RingDeque<int>>
constexpr bool deque_rvalue_begin = requires(D deque) { std::move(deque).begin(); };

static_assert(!map_rvalue_at<> && !set_rvalue_view<> && !deque_rvalue_begin<>);

static_assert(std::ranges::random_access_range<atelier::RingDeque<int>> &&
              std::ranges::random_access_range<const atelier::RingDeque<int>>);

static_assert(std::is_convertible_v<int, atelier::Optional<int>> &&
              !std::is_convertible_v<std::string_view, atelier::Optional<atelier::StringView>>);

void catalog_bulk_operations() {
    using namespace atelier;
    using acceptance::helpers::require;

    const std::array<int, 5> raw_ids{4, 2, 4, 8, 6};
    const Set<int> from_span{Span<const int>{raw_ids}};
    const std::vector<int> vec_ids{8, 10, 2, 12};
    Set<int> from_range{vec_ids};
    Set<int> from_iters(raw_ids.begin(), raw_ids.end());
    Set<int> from_init{2, 6, 14};
    require(from_span.size() == 4 && from_span.view().size() == 4 && from_iters.size() == 4 &&
                from_range.contains(10) && from_init.contains(14),
            "Set range, iterator or initializer construction lost unique membership.");

    from_range.merge(from_init);
    require(from_range.size() == 6 && from_range.contains(6) && from_range.contains(14) && from_init.size() == 1 &&
                from_init.contains(2),
            "Set merge failed to transfer distinct members or retain duplicates.");
    from_range.merge(from_range);
    require(from_range.size() == 6, "Set self-merge altered membership.");

    auto taken_member = from_range.take(10);
    require(taken_member == 10 && !from_range.contains(10) && !from_range.take(999),
            "Set take failed to extract or report missing value.");

    Map<String, std::unique_ptr<int>> items;
    for (unsigned i = 0; i < 40; ++i) {
        const auto key_text = "item/" + std::to_string(i);
        items.try_emplace(*String::from_utf8(StringView{key_text}), std::make_unique<int>(static_cast<int>(i)));
    }
    require(*items.at(StringView{"item/0"}) == 0 && *std::as_const(items).at(StringView{"item/39"}) == 39,
            "Map::at failed to borrow live values.");
    require(items.remove_if([](auto entry) { return *entry.value < 0; }) == 0 && items.size() == 40,
            "Zero-hit Map::remove_if altered table.");
    const auto removed_even = items.remove_if([](const String&, const std::unique_ptr<int>& value) {
        return (*value % 2) == 0;
    });
    require(removed_even == 20 && items.size() == 20, "Map::remove_if count mismatch.");
    for (unsigned i = 0; i < 40; ++i) {
        const auto key_text = "item/" + std::to_string(i);
        const auto* found = items.find(StringView{key_text});
        require((i % 2 == 1 ? (found && **found == static_cast<int>(i)) : !found),
                "Map::remove_if corrupted surviving entries or probe chains.");
    }

    auto extracted = items.take(StringView{"item/21"});
    require(extracted && *extracted && **extracted == 21 && !items.contains(StringView{"item/21"}) &&
                !items.take(StringView{"item/21"}),
            "Map::take failed to move out resource.");

    Map<String, std::unique_ptr<int>> incoming;
    incoming.try_emplace(String::literal<"item/3">(), std::make_unique<int>(300));
    incoming.try_emplace(String::literal<"item/100">(), std::make_unique<int>(100));
    items.reserve(items.size() + incoming.size());
    items.merge(incoming);
    require(*items.at(StringView{"item/3"}) == 3 && *items.at(StringView{"item/100"}) == 100 &&
                incoming.size() == 1 && *incoming.at(StringView{"item/3"}) == 300,
            "Map::merge overwrote existing entry or lost incoming move-only value.");

    Map<int, int> empty_dest;
    Map<int, int> small_src{{1, 10}, {2, 20}};
    empty_dest.merge(std::move(small_src));
    require(empty_dest.size() == 2 && empty_dest.at(1) == 10 && empty_dest.at(2) == 20 && small_src.empty(),
            "Empty Map::merge fast path failed.");
    const Map<int, int> copied_range(empty_dest);
    const Map<int, int> iter_copy(empty_dest.begin(), empty_dest.end());
    const Map<int, int> reverse_order{{2, 20}, {1, 10}};
    require(copied_range.size() == 2 && iter_copy.at(2) == 20 && empty_dest == reverse_order &&
                from_span == from_iters,
            "Map/Set range construction or order-independent equality failed.");

    Set<String, CatalogHash> colliding;
    std::vector<std::string> names;
    for (unsigned i = 0; i < 96; ++i) {
        names.push_back("collide/" + std::to_string(i));
        colliding.add(*String::from_utf8(StringView{names.back()}));
    }
    const auto dropped = colliding.remove_if([](const String& s) { return s.ends_with('0') || s.ends_with('5'); });
    require(dropped == 20 && colliding.size() == 76, "Colliding Set::remove_if count mismatch.");
    for (unsigned i = 0; i < 96; ++i) {
        const bool removed_expected = (i % 5) == 0;
        require(colliding.contains(StringView{names[i]}) == !removed_expected,
                "Colliding Set::remove_if broke displaced lookup.");
    }
    require(colliding.remove_if([](const String&) { return true; }) == 76 && colliding.empty(),
            "Full Set::remove_if left residual elements.");

    Map<StringView, int> borrowed{{"alpha", 1}, {"beta", 2}, {"gamma", 3}, {"delta", 4}};
    require(borrowed.remove_if([](auto entry) { return entry.value % 2 == 0; }) == 2 && borrowed.size() == 2 &&
                !borrowed.contains("beta") && borrowed.at("gamma") == 3,
            "Borrowed-name inline Map::remove_if failed.");
    Map<StringView, int> borrowed_more{{"alpha", 99}, {"epsilon", 5}};
    borrowed.merge(borrowed_more);
    require(borrowed.size() == 3 && borrowed.at("alpha") == 1 && borrowed.at("epsilon") == 5 &&
                borrowed_more.size() == 1 && borrowed_more.take("alpha") == 99 && borrowed_more.empty() &&
                borrowed_more.try_emplace("zeta", 6).inserted && borrowed_more.at("zeta") == 6,
            "Borrowed-name inline Map::merge or take left stale mask state.");

    Map<std::string, int> std_keyed{{"mesh/wall", 7}, {"mesh/arch", 11}};
    require(std_keyed.contains(StringView{"mesh/wall"}) && std_keyed.at(std::string_view{"mesh/arch"}) == 11 &&
                borrowed.contains(std::string{"epsilon"}) && borrowed.at(std::string_view{"gamma"}) == 3,
            "Transparent cross-string lookup failed.");

    Map<std::wstring, int> wide_keyed{{L"P", 3}, {L"N", 4}};
    require(wide_keyed.contains(std::wstring_view{L"P"}) && wide_keyed.at(L"N") == 4,
            "Transparent wide-string lookup failed.");

    Map<std::pair<int, std::string>, int> pair_map;
    pair_map.try_emplace(std::pair{2, std::string{"lod"}}, 8);
    Set<std::tuple<int, std::uint64_t, std::string>> tuple_set;
    tuple_set.add(std::tuple{1, UINT64_C(42), std::string{"ok"}});
    Set<std::array<int, 3>> array_set{{1, 2, 3}, {0, 0, 0}};
    Set<Optional<std::tuple<int, String>>> opt_set{
        Optional<std::tuple<int, String>>{},
        Optional<std::tuple<int, String>>{std::in_place, 5, String::literal<"x">()},
        Optional<std::tuple<int, String>>{std::in_place, 5, String::literal<"x">()}};
    require(pair_map.at(std::pair{2, std::string_view{"lod"}}) == 8 &&
                pair_map.contains(std::pair{2, StringView{"lod"}}) &&
                tuple_set.contains(std::tuple{1, UINT64_C(42), std::string{"ok"}}) && array_set.contains({1, 2, 3}) &&
                array_set.contains({0, 0, 0}) && opt_set.size() == 2 &&
                opt_set.contains(Optional<std::tuple<int, String>>{}) &&
                opt_set.contains(Optional<std::tuple<int, String>>{std::in_place, 5, String::literal<"x">()}),
            "Composite pair/tuple/array/Optional hashing failed.");

    require(Optional<StringView>{"abc"} == "abc" && "abc" == Optional<StringView>{"abc"} &&
                Optional<StringView>{"abc"} != "def",
            "Optional heterogeneous equality failed.");

    Map<int, String> alias_growth;
    for (int i = 0; i < 16; ++i)
        alias_growth.try_emplace(i, String::literal<"seed-value-across-inline-boundary">());
    alias_growth.try_emplace(16, alias_growth.at(0));
    alias_growth.insert_or_assign(0, alias_growth.at(0));
    require(alias_growth.size() == 17 && alias_growth.at(16) == "seed-value-across-inline-boundary" &&
                alias_growth.at(0) == "seed-value-across-inline-boundary",
            "Map self-aliasing across inline-to-indexed growth or self-overwrite failed.");

    struct MoveCounted {
        int value{};

        int* moves{};

        MoveCounted(int v, int* m) noexcept : value(v), moves(m) {}

        MoveCounted(MoveCounted&& other) noexcept : value(other.value), moves(other.moves) {
            if (moves)
                ++*moves;
        }

        MoveCounted& operator=(MoveCounted&& other) noexcept {
            value = other.value;
            moves = other.moves;
            if (moves)
                ++*moves;
            return *this;
        }
    };
    int key_moves = 0;
    int val_moves = 0;
    struct KeyWithMoves {
        int id{};

        int* moves{};

        KeyWithMoves(int i, int* m) noexcept : id(i), moves(m) {}

        KeyWithMoves(KeyWithMoves&& other) noexcept : id(other.id), moves(other.moves) {
            if (moves)
                ++*moves;
        }

        KeyWithMoves& operator=(KeyWithMoves&& other) noexcept {
            id = other.id;
            moves = other.moves;
            if (moves)
                ++*moves;
            return *this;
        }

        bool operator==(const KeyWithMoves& other) const noexcept { return id == other.id; }
    };
    struct KeyWithMovesHash {
        using is_avalanching = void;

        std::size_t operator()(const KeyWithMoves& k) const noexcept { return Hash<int>{}(k.id); }
    };
    Map<KeyWithMoves, MoveCounted, KeyWithMovesHash> single_move_map;
    single_move_map.try_emplace(KeyWithMoves{1, &key_moves}, 99, &val_moves);
    key_moves = 0;
    val_moves = 0;
    Map<KeyWithMoves, MoveCounted, KeyWithMovesHash> empty_merge_target;
    empty_merge_target.merge(single_move_map);
    require(empty_merge_target.size() == 1 && single_move_map.empty() && key_moves == 0 && val_moves == 0,
            "Empty Map::merge failed to steal buffer with zero element moves.");
    Map<KeyWithMoves, MoveCounted, KeyWithMovesHash> merge_target;
    merge_target.reserve(4);
    merge_target.try_emplace(KeyWithMoves{2, nullptr}, 77, nullptr);
    key_moves = 0;
    val_moves = 0;
    merge_target.merge(empty_merge_target);
    require(merge_target.size() == 2 && empty_merge_target.empty() && key_moves == 1 && val_moves == 1,
            "Map::merge performed redundant element moves.");
    key_moves = 0;
    val_moves = 0;
    auto single_taken = merge_target.take(KeyWithMoves{1, nullptr});
    require(single_taken && single_taken->value == 99 && key_moves == 0 && val_moves == 1,
            "Map::take performed redundant key or value moves.");

    const auto invoke_optional_cb = [](int x, Optional<FunctionRef<int(int)>> transform = {}) {
        return transform ? (*transform)(x) : x;
    };
    int factor = 3;
    require(invoke_optional_cb(5) == 5 && invoke_optional_cb(5, [&](int v) { return v * factor; }) == 15,
            "Optional<FunctionRef> synchronous callback failed.");

    Map<std::pair<int, bool>, int> braced_map;
    require(braced_map.try_emplace({4, true}, 42).inserted && braced_map.contains({4, true}) &&
                braced_map.at({4, true}) == 42 && braced_map.remove({4, true}),
            "Braced-init Map key deduction failed.");
    Set<std::tuple<int, int, int>> braced_set;
    require(braced_set.add({1, 2, 3}) && braced_set.contains({1, 2, 3}) && braced_set.remove({1, 2, 3}),
            "Braced-init Set key deduction failed.");

    enum class SampleMode : std::uint8_t { alpha = 1, beta = 2 };
    int anchor_a = 1;
    int anchor_b = 2;
    require(hash_fields(&anchor_a, SampleMode::alpha, StringView{"k"}) ==
                hash_fields(&anchor_a, SampleMode::alpha, std::string_view{"k"}) &&
                hash_fields(&anchor_a, SampleMode::alpha, StringView{"k"}) !=
                    hash_fields(&anchor_b, SampleMode::beta, StringView{"k"}) &&
                hash_fields(0) != 0 && hash_fields(0) != hash_fields(0, 0) && hash_fields(0, 7) != hash_fields(7) &&
                Hash<std::pair<int, int>>{}({0, 0}) == hash_fields(0, 0),
            "Generalized hash_fields failed on zero-prefix or composite inputs.");
}
}

void verify_tasks();

int main() {
    atelier::install_fatal_handler();
    using namespace atelier;
    using namespace acceptance::helpers;
    auto text = String::from_utf8("abcdefghijklmnopqrst");
    require(text && text->append(text->slice(1)), "String self-append failed.");
    require(*text == "abcdefghijklmnopqrstbcdefghijklmnopqrst", "String self-view was invalidated.");
    const auto removed = text->replace_all(text->slice(1, 19), {});
    require(removed && *removed == 2 && *text == "a" && text->capacity() == String::inline_capacity,
            "Shrinking a heap string to one byte must leave an inline result.");
    Map<int, int> map;
    map.reserve(8);
    require(map.begin() == std::as_const(map).end() && std::as_const(map).begin() == map.end(),
            "Reserved empty map has inconsistent mutable and const iterators.");
    Array<String> values{String::literal<"seven">(), String::literal<"nine">()};
    require(values.capacity() == 2, "Array initializer construction over-allocated capacity.");
    values.append(values.view());
    const Array<String> copied = values;
    require(copied.capacity() == 4, "Array copy construction over-allocated capacity.");
    require(values.size() == 4 && values[2] == "seven" && values[3] == "nine" && values.front() == "seven",
            "Array self-append lost its inputs.");
    require(String::join(values, String::literal<"/">()) == "seven/nine/seven/nine", "String::join strings failed.");
    const auto dotted = String::join(StringView{"a::b::c"}.split("::"), ".");
    require(dotted && *dotted == "a.b.c" && !String::join({"ok", "\xff"}, ","), "String::join views failed.");
    require(foundation::sha256(as_bytes(StringView{"abc"})) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "Span sha256 bridge failed.");
    queued_resources();
    borrowed_name_catalog();
    resource_catalog_growth();
    bound_catalog();
    catalog_bulk_operations();
    verify_tasks();
    std::puts("Foundation acceptance: PASS (owning aliases, queued resources and failed join cleanup)");
}
