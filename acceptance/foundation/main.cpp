#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "foundation/array.hpp"
#include "foundation/bit_vector.hpp"
#include "foundation/fixed_array.hpp"
#include "foundation/function_ref.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/map.hpp"
#include "foundation/optional.hpp"
#include "foundation/priority_queue.hpp"
#include "foundation/ring_deque.hpp"
#include "foundation/set.hpp"
#include "foundation/sha256.hpp"
#include "foundation/string.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

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
    constexpr int numbers[]{2, 4, 6, 4, 8};
    atelier::Span<const int> span{numbers};
    if (span.front() != 2 || span.back() != 8 || span.first(2)[1] != 4 || span.last(2)[0] != 4 ||
        span.size_bytes() != sizeof(numbers) || *span.rbegin() != 8 || span.find(4) != 1 || span.find(4, 2) != 3 ||
        span.find_last(4) != 3 || span.find_last(4, 2) != 1 || !span.contains(6) || span.contains(99))
        return false;
    constexpr int prefix[]{2, 4};
    constexpr int suffix[]{4, 8};
    if (!span.starts_with(prefix) || !span.starts_with(2) || !span.ends_with(suffix) || !span.ends_with(8))
        return false;
    if (!span.strip_prefix(2) || !span.strip_suffix(8) || span.size() != 3)
        return false;
    span.remove_prefix(1);
    span.remove_suffix(1);
    return span.size() == 1 && span[0] == 6 && (atelier::Span{prefix} <=> atelier::Span{suffix}) < 0;
}

static_assert(verify_constexpr_views());

static_assert(sizeof(atelier::Span<int>) == 16 && alignof(atelier::Span<int>) == 8 &&
              std::is_trivially_copyable_v<atelier::Span<int>> && std::is_standard_layout_v<atelier::Span<int>> &&
              std::ranges::contiguous_range<atelier::Span<int>> && std::ranges::sized_range<atelier::Span<int>> &&
              std::ranges::borrowed_range<atelier::Span<int>> && std::ranges::view<atelier::Span<int>>);

static_assert(std::is_convertible_v<atelier::Array<int>&, atelier::Span<int>> &&
              std::is_convertible_v<atelier::Array<int>&, atelier::Span<const int>> &&
              std::is_convertible_v<const atelier::Array<int>&, atelier::Span<const int>> &&
              !std::is_convertible_v<const atelier::Array<int>&, atelier::Span<int>> &&
              !std::is_constructible_v<atelier::Span<const int>, atelier::Array<int>&&> &&
              !std::is_constructible_v<atelier::Span<const int>, const atelier::Array<int>&&> &&
              !std::is_constructible_v<atelier::Span<const int>, std::vector<int>&&> &&
              std::is_constructible_v<atelier::Span<const int>, std::span<int>> &&
              !std::is_convertible_v<std::span<int>, atelier::Span<const int>> &&
              std::is_constructible_v<atelier::Span<const int>, std::span<int, 4>> &&
              !std::same_as<decltype(std::ranges::find(std::declval<atelier::Span<const int>>(), 0)),
                            std::ranges::dangling>);

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

void verify_span_simd_and_borrows() {
    using namespace atelier;
    using acceptance::helpers::require;

    auto verify_lanes = []<class V>(V filler, V target, V other) {
        for (const std::size_t total : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{15},
                                        std::size_t{16}, std::size_t{17}, std::size_t{31}, std::size_t{32},
                                        std::size_t{33}, std::size_t{65}}) {
            std::vector<V> storage(total + 3, filler);
            const std::vector<V> baseline(total, filler);
            for (const std::size_t offset : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
                Span<V> view{storage.data() + offset, total};
                const Span<const V> clean_ref{baseline};
                require(view == clean_ref && view.find(target) == Span<V>::npos &&
                            view.find_last(target) == Span<V>::npos,
                        "Span SIMD search matched an absent scalar.");
                for (std::size_t pos = 0; pos < total; ++pos) {
                    view[pos] = target;
                    require(view != clean_ref && view.find(target) == pos && view.find_last(target) == pos &&
                                view.contains(target),
                            "Span SIMD single-hit index mismatch.");
                    if (pos + 1 < total) {
                        const std::size_t second = total - 1;
                        view[second] = target;
                        require(view.find(target) == pos && view.find(target, pos + 1) == second &&
                                    view.find_last(target) == second && view.find_last(target, second - 1) == pos,
                                "Span SIMD multi-hit forward/reverse mismatch.");
                        view[second] = filler;
                    }
                    view[pos] = other;
                    require(view != clean_ref, "Span equality missed mutated lane.");
                    view[pos] = filler;
                }
            }
        }
    };

    verify_lanes(std::uint8_t{0x11}, std::uint8_t{0x7a}, std::uint8_t{0xfe});
    verify_lanes(std::byte{0x22}, std::byte{0xa5}, std::byte{0x00});
    verify_lanes(std::uint16_t{0x1234}, std::uint16_t{0x9abc}, std::uint16_t{0x1299});
    verify_lanes(std::int32_t{-7}, std::int32_t{0x13579bdf}, std::int32_t{42});
    verify_lanes(UINT64_C(0x1111222233334444), UINT64_C(0xdeadbeefcafebabe), UINT64_C(0xdeadbeef00000000));

    // Guard SSE2 64-bit equality against false positives when high/low 32-bit halves match separately.
    const std::uint64_t tricky64[]{
        UINT64_C(0x0000000199999999), UINT64_C(0x9999999900000002),
        UINT64_C(0x9999999900000002), UINT64_C(0x0000000199999999),
        UINT64_C(0x0000000100000002), UINT64_C(0x0000000199999999)};
    const Span tricky_view{tricky64};
    require(tricky_view.find(UINT64_C(0x0000000100000002)) == 4 &&
                tricky_view.find_last(UINT64_C(0x0000000100000002)) == 4 &&
                !tricky_view.slice(0, 4).contains(UINT64_C(0x0000000100000002)),
            "Span 64-bit SIMD search false-matched split 32-bit halves.");

    const int targets[3]{1, 2, 3};
    const int* const ptrs[]{&targets[0], nullptr, &targets[2], &targets[1], &targets[2]};
    const Span ptr_span{ptrs};
    require(ptr_span.find(nullptr) == 1 && ptr_span.find(&targets[2]) == 2 && ptr_span.find_last(&targets[2]) == 4,
            "Span pointer SIMD search failed.");

    // IEEE-754 float/double SIMD rules: -0.0 == +0.0, NaN != NaN, and searching for NaN yields npos.
    const float nan_f = std::numeric_limits<float>::quiet_NaN();
    const float floats_a[]{1.0f, -0.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, +0.0f, 10.0f};
    const float floats_b[]{1.0f, +0.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, -0.0f, 10.0f};
    const float floats_nan[]{1.0f, 2.0f, 3.0f, 4.0f, nan_f, 6.0f, 7.0f, 8.0f, 9.0f};
    require(Span{floats_a} == Span{floats_b} && Span{floats_a}.find(+0.0f) == 1 &&
                Span{floats_a}.find_last(-0.0f) == 8 && Span{floats_nan}.find(nan_f) == Span<const float>::npos &&
                Span{floats_nan}.find_last(nan_f) == Span<const float>::npos &&
                Span{floats_nan}.find(9.0f) == 8 && Span{floats_nan} != Span{floats_nan},
            "Span float SIMD violated IEEE-754 signed-zero or NaN semantics.");

    const double nan_d = std::numeric_limits<double>::quiet_NaN();
    const double doubles_a[]{1.0, -0.0, 3.0, 4.0, +0.0};
    const double doubles_b[]{1.0, +0.0, 3.0, 4.0, -0.0};
    const double doubles_nan[]{1.0, nan_d, 3.0, 4.0, 5.0};
    require(Span{doubles_a} == Span{doubles_b} && Span{doubles_a}.find(+0.0) == 1 &&
                Span{doubles_a}.find_last(-0.0) == 4 && Span{doubles_nan}.find(nan_d) == Span<const double>::npos &&
                Span{doubles_nan} != Span{doubles_nan},
            "Span double SIMD violated IEEE-754 signed-zero or NaN semantics.");

    Array<String> names{String::literal<"alpha">(), String::literal<"beta">(), String::literal<"alpha">()};
    const auto accept_borrowed = [](Span<const String> items) {
        return items.find(StringView{"beta"}) == 1 && items.find_last(StringView{"alpha"}) == 2 &&
               items.contains("beta") && !items.contains("gamma");
    };
    require(accept_borrowed(names) && names.find(StringView{"beta"}) == 1 && names.contains("alpha"),
            "Implicit lvalue Array->Span borrow or heterogeneous search failed.");

    const int zero_elem[]{0};
    Set<Span<const int>> int_spans{Span<const int>{}, Span<const int>{zero_elem}, Span{targets}};
    Set<Span<const String>> str_spans{Span<const String>{}, Span<const String>{names}.first(2)};
    require(int_spans.size() == 3 && int_spans.contains(Span<const int>{}) &&
                int_spans.contains(Span<const int>{zero_elem}) && str_spans.size() == 2 &&
                str_spans.contains(names.slice(0, 2)),
            "Hash<Span> or Equal<Span> failed on empty vs zero-element slices.");

    // Red-team regressions for Span: Derived->Base slicing block, empty-brace prefix/suffix,
    // StringView borrowed_range, and enum BitwiseScalar SIMD/wyhash acceleration.
    struct SliceBase {
        int a;
    };
    struct SliceDerived : SliceBase {
        int b;
    };
    static_assert(!std::is_constructible_v<Span<SliceBase>, SliceDerived*, std::size_t> &&
                  !std::is_constructible_v<Span<const SliceBase>, SliceDerived*, std::size_t> &&
                  std::ranges::borrowed_range<StringView> && std::ranges::view<StringView> &&
                  std::is_constructible_v<Span<const char>, StringView> &&
                  detail::BitwiseScalar<std::byte>);
    enum class FastToken : std::uint8_t { alpha = 1, beta = 2, eof = 9 };
    static_assert(detail::BitwiseScalar<FastToken>);
    Array<FastToken> token_stream(64, FastToken::alpha);
    token_stream[63] = FastToken::eof;
    Span<const int> brace_check{zero_elem};
    Span<const int> strip_check = brace_check;
    require(token_stream.find(FastToken::eof) == 63 && brace_check.starts_with({}) && brace_check.ends_with({}) &&
                strip_check.strip_prefix({}) && strip_check.size() == 1 && strip_check[0] == 0,
            "Span empty-brace prefix/suffix or enum SIMD search failed.");
}

template <class A>
constexpr bool can_resize_uninitialized = requires(A array) { array.resize_uninitialized(8); };

void verify_array_family_enhancements() {
    using namespace atelier;
    using acceptance::helpers::require;

    static_assert(sizeof(Array<int>) == 24 && alignof(Array<int>) == 8 && std::is_standard_layout_v<Array<int>>);
    static_assert(can_resize_uninitialized<Array<std::uint32_t>> && !can_resize_uninitialized<Array<String>>);

    struct MemberHost {
        int field;
    };
    using MemberPtr = int MemberHost::*;
    struct StructWithMemberPtr {
        MemberPtr ptr;
    };
    struct PaddingFreePair {
        std::uint32_t a;

        std::uint32_t b;
    };
    static_assert(!detail::ArrayElements<MemberPtr>::zero_default_init &&
                  !detail::ArrayElements<StructWithMemberPtr>::zero_default_init &&
                  detail::ArrayElements<PaddingFreePair>::zero_default_init &&
                  detail::ArrayElements<int>::zero_default_init &&
                  detail::ArrayElements<float>::zero_default_init &&
                  detail::ArrayElements<const int*>::zero_default_init);
    Array<MemberPtr> member_ptrs(5);
    for (const auto mp : member_ptrs)
        require(mp == nullptr, "Member-pointer default construction corrupted null representation.");

    auto verify_fill_widths = []<class V>(V uniform_val, V patterned_val) {
        for (const std::size_t count :
             {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{15}, std::size_t{16},
              std::size_t{17}, std::size_t{33}}) {
            Array<V> by_uniform(count, uniform_val);
            Array<V> by_pattern(count, patterned_val);
            require(by_uniform.size() == count && by_uniform.capacity() == count && by_pattern.size() == count &&
                        by_pattern.capacity() == count,
                    "Array fill constructor violated exact capacity contract.");
            for (std::size_t i = 0; i < count; ++i)
                require(by_uniform[i] == uniform_val && by_pattern[i] == patterned_val,
                        "SIMD or memset fill_construct wrote wrong element.");
            by_uniform.assign(count + 3, patterned_val);
            for (std::size_t i = 0; i < count + 3; ++i)
                require(by_uniform[i] == patterned_val, "Array::assign fill wrote wrong element.");
        }
    };
    verify_fill_widths(std::uint8_t{0xff}, std::uint8_t{0x3c});
    verify_fill_widths(std::uint16_t{0xffff}, std::uint16_t{0x1234});
    verify_fill_widths(UINT32_C(0xffffffff), UINT32_C(0x12345678));
    verify_fill_widths(0.0f, 3.25f);
    verify_fill_widths(UINT64_C(0xffffffffffffffff), UINT64_C(0x0102030405060708));
    verify_fill_widths(std::array<std::uint32_t, 4>{0, 0, 0, 0}, std::array<std::uint32_t, 4>{10, 20, 30, 40});

    // Self-aliasing assign, self-slice assign, reverse iterator assign, and lazy self-view append.
    Array<String> words{String::literal<"zero">(), String::literal<"one">(), String::literal<"two">(),
                        String::literal<"three">(), String::literal<"four">()};
    words.reserve(8);
    const auto cap_before = words.capacity();
    words.assign(words.slice(1, 3));
    require(words.capacity() == cap_before && words.size() == 3 && words[0] == "one" && words[1] == "two" &&
                words[2] == "three",
            "Self-slice String assign corrupted elements or reallocated.");
    words.assign(5, words[1]);
    require(words.capacity() == cap_before && words.size() == 5 && words[0] == "two" && words[4] == "two",
            "Self-element String assign corrupted elements or reallocated.");

    Array<int> nums{1, 2, 3};
    nums.append(nums | std::views::reverse);
    require((nums == Span<const int>{{1, 2, 3, 3, 2, 1}}) && (Span<const int>{{1, 2, 3, 3, 2, 1}} == nums),
            "Lazy self-view append or Span<->Array equality failed.");
    nums.assign(nums.rbegin(), nums.rend());
    nums.append({7, 8});
    const int expected_nums[]{1, 2, 3, 3, 2, 1, 7, 8};
    require(nums == Span{expected_nums}, "Reverse-iterator self-assign or initializer_list append failed.");
    nums.assign(nums.slice(2, 4));
    const int expected_slice[]{3, 3, 2, 1};
    require(nums == Span{expected_slice}, "Trivial self-slice memmove assign failed.");

    // Move-only transfer across InlineArray, FixedArray, and Array without copies.
    InlineArray<std::unique_ptr<int>, 4> inline_jobs(2);
    require(inline_jobs.size() == 2 && !inline_jobs[0] && !inline_jobs[1], "InlineArray count default-init failed.");
    inline_jobs[0] = std::make_unique<int>(11);
    inline_jobs[1] = std::make_unique<int>(22);
    Array<std::unique_ptr<int>> owned_jobs(std::move(inline_jobs));
    FixedArray<std::unique_ptr<int>, 4> fixed_more;
    fixed_more.emplace(std::make_unique<int>(33));
    owned_jobs.append(std::move(fixed_more));
    require(owned_jobs.size() == 3 && *owned_jobs[0] == 11 && *owned_jobs[1] == 22 && *owned_jobs[2] == 33,
            "Rvalue range move construction or append lost move-only elements.");

    // Ensure rvalue borrowed Span<String> copies instead of stealing from the underlying owner.
    Array<String> keep{String::literal<"keep-a">(), String::literal<"keep-b">()};
    Array<String> copied_from_mutable_span(keep.slice(0, 2));
    require(keep[0] == "keep-a" && keep[1] == "keep-b" && copied_from_mutable_span == keep,
            "Constructing Array from rvalue borrowed Span stole elements from owner.");

    // Non-copy-assignable elements remain usable with assign.
    struct ImmutableItem {
        const int id;

        bool operator==(const ImmutableItem&) const noexcept = default;
    };
    Array<ImmutableItem> immutables(2, ImmutableItem{5});
    immutables.assign(3, immutables[0]);
    require(immutables.size() == 3 && immutables[0].id == 5 && immutables[2].id == 5,
            "Non-copy-assignable element assign failed.");

    // resize_uninitialized preserves live prefix across growth.
    Array<std::uint32_t> raw_buf{10u, 20u};
    raw_buf.resize_uninitialized(64);
    for (std::uint32_t i = 2; i < 64; ++i)
        raw_buf[i] = i * 10u;
    require(raw_buf.size() == 64 && raw_buf[0] == 10u && raw_buf[1] == 20u && raw_buf[63] == 630u,
            "resize_uninitialized lost prefix across reallocation.");

    // Defaulted struct ==/<=> and transparent Map/Set lookup via Span.
    struct CompositeKey {
        Array<int> indices;

        InlineArray<String, 2> tags;

        bool operator==(const CompositeKey&) const noexcept = default;

        auto operator<=>(const CompositeKey&) const noexcept = default;
    };
    CompositeKey key_a{Array<int>{1, 2}, InlineArray<String, 2>{String::literal<"x">()}};
    CompositeKey key_b{Array<int>{1, 3}, InlineArray<String, 2>{String::literal<"x">()}};
    require(key_a == CompositeKey{Array<int>{1, 2}, InlineArray<String, 2>{String::literal<"x">()}} && key_a < key_b,
            "Defaulted struct equality or three-way comparison with Array/InlineArray failed.");

    Map<Array<int>, int> path_table;
    path_table.try_emplace(Array<int>{4, 8, 15}, 99);
    const int query_path[]{4, 8, 15};
    InlineArray<int, 4> inline_query{4, 8, 15};
    Set<FixedArray<String, 4>> tag_sets;
    tag_sets.add(FixedArray<String, 4>{String::literal<"lod0">(), String::literal<"shadow">()});
    const String tag_query[]{String::literal<"lod0">(), String::literal<"shadow">()};
    require(path_table.contains(Span{query_path}) && path_table.at(inline_query) == 99 &&
                tag_sets.contains(Span{tag_query}),
            "Transparent Hash/Equal across Array, InlineArray, FixedArray, and Span failed.");

    // Red-team regressions for Array family:
    // 1) Temporary non-borrowed <ranges> views (filter/transform) must copy, never steal from lvalue owner!
    Array<String> source_words{String::literal<"important-long-heap-string-alpha">(),
                               String::literal<"important-long-heap-string-beta">()};
    Array<String> filtered_copy(source_words | std::views::filter([](const String& s) { return !s.empty(); }));
    require(source_words[0] == "important-long-heap-string-alpha" &&
                source_words[1] == "important-long-heap-string-beta" && filtered_copy == source_words,
            "Constructing Array from temporary filter_view stole lvalue elements.");

    // 2) Unsized lazy self-view append must not infinite-loop or UAF.
    Array<int> self_filtered{1, -2, 3, -4, 5};
    self_filtered.append(self_filtered | std::views::filter([](int v) { return v > 0; }));
    const int expected_filtered[]{1, -2, 3, -4, 5, 1, 3, 5};
    require(self_filtered == expected_filtered, "Unsized lazy self-view append failed.");

    // 3) Padding-bearing types (nullptr_t, Empty, multi-inheritance member function pointer) have no_padding_bytes == false.
    struct EmptyItem {};
    struct BaseA {
        int a;
    };
    struct BaseB {
        int b;
    };
    struct MultiDerived : BaseA, BaseB {
        void method() {}
    };
    static_assert(!detail::ArrayElements<std::nullptr_t>::no_padding_bytes &&
                  !detail::ArrayElements<EmptyItem>::no_padding_bytes &&
                  !detail::ArrayElements<void (MultiDerived::*)()>::no_padding_bytes);
    Array<std::nullptr_t> nulls(4, nullptr);
    Array<EmptyItem> empties(4, EmptyItem{});
    require(nulls.size() == 4 && empties.size() == 4, "Padding-bearing fill_construct failed.");

    // 4) Indirect self-ownership across growing assign, capacity preservation on assign(R&&),
    //    O(1) heap-steal across InlineArray<->Array, cross-sequence operator==, and braced add_unique.
    struct GroupNode {
        int id{};

        std::unique_ptr<Array<GroupNode>> child;

        GroupNode(int v = 0) noexcept : id(v) {}

        GroupNode(const GroupNode& o) noexcept
            : id(o.id), child(o.child ? std::make_unique<Array<GroupNode>>(*o.child) : nullptr) {}

        GroupNode& operator=(const GroupNode& o) noexcept {
            if (this != &o) {
                id = o.id;
                child = o.child ? std::make_unique<Array<GroupNode>>(*o.child) : nullptr;
            }
            return *this;
        }

        GroupNode(GroupNode&&) noexcept = default;

        GroupNode& operator=(GroupNode&&) noexcept = default;
    };
    Array<GroupNode> groups(1);
    groups[0].child = std::make_unique<Array<GroupNode>>(4, GroupNode{77});
    groups.assign(groups[0].child->view());
    require(groups.size() == 4 && groups[3].id == 77, "Indirect self-owned Span assign caused UAF.");

    Array<int> scratch;
    scratch.reserve(1024);
    RingDeque<int> dq;
    dq.emplace_back(10);
    dq.emplace_back(20);
    scratch.assign(dq);
    require(scratch.capacity() == 1024 && scratch.size() == 2 && scratch[0] == 10 && scratch[1] == 20,
            "Array::assign(Range) discarded reserved capacity.");

    InlineArray<String, 2> spilled_inline;
    for (int i = 0; i < 6; ++i)
        spilled_inline.emplace(String::literal<"spilled-heap-item-with-long-payload">());
    const auto* original_heap_ptr = spilled_inline.data();
    Array<String> stolen_array(std::move(spilled_inline));
    require(spilled_inline.empty() && stolen_array.size() == 6 && stolen_array.data() == original_heap_ptr,
            "Array(InlineArray&&) failed O(1) heap buffer steal or left non-empty source.");
    InlineArray<String, 2> stolen_back(std::move(stolen_array));
    require(stolen_array.empty() && stolen_back.size() == 6 && stolen_back.data() == original_heap_ptr &&
                stolen_back == Array<String>(6, String::literal<"spilled-heap-item-with-long-payload">()),
            "InlineArray(Array&&) failed O(1) heap steal or cross-sequence operator==.");

    InlineArray<ImmutableItem, 4> inline_immutables(2, ImmutableItem{5});
    inline_immutables.assign(2, ImmutableItem{99});
    Array<std::pair<int, int>> unique_pairs;
    require(inline_immutables[0].id == 99 && inline_immutables[1].id == 99 && unique_pairs.add_unique({3, 4}) == 0 &&
                unique_pairs.add_unique({3, 4}) == 0 && unique_pairs.size() == 1,
            "InlineArray const-member launder or braced add_unique failed.");
}

template <class B = atelier::BitVector>
constexpr bool bitvec_rvalue_words = requires(B b) { std::move(b).words(); };

template <class B = atelier::BitVector>
constexpr bool bitvec_rvalue_set_bits = requires(B b) { std::move(b).set_bits(); };

void verify_bit_vector_and_optional() {
    using namespace atelier;
    using acceptance::helpers::require;

    static_assert(sizeof(BitVector) == 32 && alignof(BitVector) == 8 &&
                  std::bidirectional_iterator<BitVector::Iterator> &&
                  std::ranges::bidirectional_range<BitVector::SetBits> &&
                  std::ranges::borrowed_range<BitVector::SetBits> && std::ranges::view<BitVector::SetBits> &&
                  !bitvec_rvalue_words<> && !bitvec_rvalue_set_bits<>);

    struct NonEquatable {
        int x;
    };
    static_assert(!detail::EquatableElement<NonEquatable> && !detail::EquatableElement<Optional<NonEquatable>> &&
                  Equal<Optional<int>>{}(Optional<int>{7}, Optional<int>{7}));

    // Guard BitVector{bool, ...} initializer_list against silent (size, fill) constructor hijacking.
    BitVector single_true{true};
    BitVector single_false{false};
    BitVector pair_bits{false, true};
    require(single_true.size() == 1 && single_true[0] && single_false.size() == 1 && !single_false[0] &&
                pair_bits.size() == 2 && !pair_bits[0] && pair_bits[1],
            "BitVector initializer_list<bool> construction failed.");

    BitVector exact_word(64, true);
    require(exact_word.all() && exact_word.find_last() == 63 && exact_word.find_last_from(63) == 63 &&
                exact_word.find_prev(64) == 63,
            "BitVector 64-bit boundary reverse scan failed.");

    BitVector shrunk(200, true);
    shrunk.resize(65);
    const BitVector fresh65(65, true);
    require(shrunk == fresh65 && Hash<BitVector>{}(shrunk) == Hash<BitVector>{}(fresh65),
            "BitVector shrink history broke equality or hash equivalence.");
    shrunk ^= shrunk;
    require(shrunk.none(), "BitVector self-xor failed.");

    BitVector rev_bits(70, false);
    rev_bits.set(3);
    rev_bits.set(64);
    rev_bits.set(68);
    Array<std::size_t> reversed_indices(rev_bits.set_bits() | std::views::reverse);
    const std::size_t expected_rev[]{68, 64, 3};
    require(reversed_indices == expected_rev &&
                *std::ranges::find(rev_bits.set_bits(), std::size_t{64}) == 64,
            "BitVector::SetBits bidirectional view or borrowed_range failed.");

    BitVector bits;
    require(bits.empty() && bits.all() && bits.none() && bits == BitVector{},
            "Empty BitVector initial predicates failed.");
    for (int i = 0; i < 130; ++i)
        bits.add((i % 3) == 0);
    require(bits.size() == 130 && bits.front() && bits.back() && !bits.test(128) && bits.test(129) && !bits.all() &&
                bits.any(),
            "BitVector sequential add across 64-bit word boundaries failed.");
    while (!bits.empty())
        bits.pop();
    require(bits == BitVector{} && bits.words().empty(),
            "BitVector pop to empty violated word-count or equality invariant.");

    BitVector grown(10, false);
    grown.set(2);
    grown.resize(150, true);
    require(grown.size() == 150 && !grown.test(0) && grown.test(2) && !grown.test(9) && grown.test(10) &&
                grown.test(63) && grown.test(64) && grown.test(149),
            "BitVector resize(bits, true) corrupted existing prefix or boundary words.");
    grown.resize(5, false);
    grown.resize(150, false);
    require(grown.count() == 1 && grown.find_first() == 2 && grown.find_last() == 2,
            "BitVector shrink-then-grow resurrected stale bits.");

    grown.set_range(10, 200 - 70, true);
    require(grown.test(9) == false && grown.test(10) && grown.test(139) && !grown.test(140) &&
                grown.find_last() == 139 && grown.find_prev(139) == 138 && grown.find_prev(11) == 10 &&
                grown.find_prev(10) == 2 && grown.find_prev(2) == BitVector::npos &&
                grown.find_prev(BitVector::npos) == BitVector::npos && grown.find_prev(grown.size()) == 139,
            "BitVector multi-word set_range or reverse bit scan failed.");

    grown.fill(true);
    require(grown.all() && grown.count() == 150, "BitVector::fill(true) or all() failed.");
    grown.flip();
    require(grown.none() && grown.count() == 0, "BitVector::flip() left non-zero tail or body bits.");

    BitVector mask_a(130, false);
    BitVector mask_b(130, false);
    mask_a.set(5);
    mask_a.set(100);
    mask_b.set(100);
    require(mask_a.intersects(mask_b) && mask_b.is_subset_of(mask_a) && !mask_a.is_subset_of(mask_b),
            "BitVector intersects or is_subset_of failed.");
    mask_a.and_not(mask_b);
    require(!mask_a.intersects(mask_b) && mask_a.count() == 1 && mask_a.find_first() == 5,
            "BitVector::and_not failed.");

    Set<BitVector> bit_catalog{BitVector(65, false), BitVector(66, false), mask_a};
    require(bit_catalog.size() == 3 && bit_catalog.contains(BitVector(65, false)) &&
                bit_catalog.contains(BitVector(66, false)) && !bit_catalog.contains(BitVector(67, false)),
            "Hash<BitVector> failed to distinguish different bit lengths with identical word count.");

    // Optional enhancements: non-nothrow std types (filesystem::path, pair), in-place operator=(U&&) capacity reuse,
    // opt = {} reset preservation, take(), and operator<=>.
    Optional<std::filesystem::path> opt_path{std::filesystem::path{"Assets/Mesh.glb"}};
    auto copied_path = opt_path;
    require(copied_path == opt_path && *copied_path == std::filesystem::path{"Assets/Mesh.glb"},
            "Optional<std::filesystem::path> copy or equality failed.");
    copied_path = {};
    require(!copied_path.has_value(), "Optional = {} failed to reset engaged value.");

    Optional<int> opt_scalar = 42;
    opt_scalar = {};
    require(!opt_scalar.has_value(), "Optional<int> = {} assigned zero instead of resetting.");
    opt_scalar = 9;
    require(opt_scalar == 9 && Optional<int>{} < opt_scalar && opt_scalar < Optional<int>{12},
            "Optional in-place value assignment or operator<=> failed.");

    Optional<Array<int>> opt_buf;
    opt_buf.emplace();
    opt_buf->reserve(256);
    const auto opt_cap = opt_buf->capacity();
    const Array<int> small_payload{1, 2, 3};
    opt_buf = small_payload;
    require(opt_buf && opt_buf->capacity() == opt_cap && *opt_buf == small_payload,
            "Optional::operator=(U&&) failed to reuse engaged payload capacity.");

    auto taken_buf = opt_buf.take();
    require(!opt_buf.has_value() && taken_buf && *taken_buf == small_payload,
            "Optional::take failed to extract value and reset source.");

    Optional<std::pair<int, int>> pair_a{std::in_place, 1, 2};
    Optional<std::pair<int, int>> pair_b{std::in_place, 1, 3};
    pair_b = {1, 4};
    pair_b = {};
    require(!pair_b.has_value(), "Optional<std::pair> = {} failed to reset.");
    pair_b = {1, 3};
    Optional<float> partial_a = 1.5f;
    Optional<float> partial_b = 2.5f;
    require(pair_a != pair_b && pair_a < pair_b && Optional<float>{} < partial_a && partial_a < partial_b &&
                partial_a < 3.0f,
            "Optional<std::pair> or partial_ordering Optional<float> comparison failed.");

    // Red-team regressions for Optional: CTAD, initializer_list in_place/emplace, converting Optional<U> with
    // rvalue-dangling protection, Optional<String> < "literal", std::optional->Optional<bool> guard, and transform.
    static_assert(!std::is_constructible_v<Optional<StringView>, Optional<String>&&> &&
                  !std::is_constructible_v<Optional<StringView>, const Optional<String>&&> &&
                  !std::is_assignable_v<Optional<StringView>&, Optional<String>&&> &&
                  std::is_constructible_v<Optional<StringView>, const Optional<String>&> &&
                  std::is_constructible_v<Optional<Span<const int>>, Optional<Span<int>>&&> &&
                  !std::is_constructible_v<Optional<bool>, std::optional<int>>);

    auto deduced_opt = Optional{42};
    auto transformed_opt = deduced_opt.transform([](int v) { return v * 2; });
    require(deduced_opt == 42 && transformed_opt == 84 && !Optional<int>{}.transform([](int v) { return v + 1; }),
            "Optional CTAD or transform failed.");

    Optional<Array<int>> init_list_opt{std::in_place, {10, 20, 30}};
    init_list_opt.emplace({40, 50});
    const int expected_opt_arr[]{40, 50};
    require(init_list_opt && *init_list_opt == expected_opt_arr, "Optional initializer_list in_place/emplace failed.");

    Optional<String> opt_owned_str = String::literal<"beta">();
    Optional<StringView> opt_borrowed_sv = opt_owned_str;
    Set<Optional<String>> opt_str_set{Optional<String>{}, opt_owned_str};
    require(opt_borrowed_sv == opt_owned_str && opt_owned_str > "alpha" && opt_owned_str < "gamma" &&
                opt_str_set.contains(std::nullopt) && opt_str_set.contains(opt_borrowed_sv),
            "Optional<U> conversion, heterogeneous literal <=>, or transparent Hash<Optional> failed.");
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
    verify_span_simd_and_borrows();
    verify_array_family_enhancements();
    verify_bit_vector_and_optional();
    verify_tasks();
    std::puts("Foundation acceptance: PASS (owning aliases, queued resources and failed join cleanup)");
}
