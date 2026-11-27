#include "memory/arena.hpp"
#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "foundation/array.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/map.hpp"
#include "foundation/optional.hpp"
#include "foundation/priority_queue.hpp"
#include "foundation/ring_deque.hpp"
#include "foundation/set.hpp"
#include "foundation/sha256.hpp"
#include "foundation/string.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <array>
#include <cstdio>
#include <functional>
#include <memory>
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

template <class T>
concept ArenaAllocatable = requires(atelier::ScratchArena& a) { a.allocate<T>(1); };

template <class T, class... Args>
concept ArenaEmplaceable =
    requires(atelier::ScratchArena& a, Args&&... args) { a.emplace<T>(std::forward<Args>(args)...); };

struct ThrowingArenaProbe {
    int v;

    ThrowingArenaProbe() noexcept(false) : v(1) {}

    explicit ThrowingArenaProbe(int x) noexcept(false) : v(x) {}
};

static_assert(ArenaAllocatable<int> && !ArenaAllocatable<ThrowingArenaProbe>);

static_assert(ArenaEmplaceable<int, int> && !ArenaEmplaceable<ThrowingArenaProbe, int>);

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
    if (!atelier::is_ascii_ident_start('_') || !atelier::is_ascii_ident_continue('9') ||
        atelier::is_ascii_ident_start('9') || !atelier::is_ascii_hex('F') || atelier::is_ascii_hex('G'))
        return false;
    constexpr atelier::StringView mixed{"Scene::TransformComponent"};
    if (mixed.find_ascii_case_insensitive("transform") != 7 || !mixed.contains_ascii_case_insensitive("COMPONENT") ||
        mixed.find_ascii_case_insensitive("missing") != atelier::StringView::npos)
        return false;
    constexpr atelier::StringView emoji{"a😀b"};
    if (atelier::platform::utf8_offset_to_utf16(emoji, 0) != 0 ||
        atelier::platform::utf8_offset_to_utf16(emoji, 1) != 1 ||
        atelier::platform::utf8_offset_to_utf16(emoji, 5) != 3 ||
        atelier::platform::utf8_offset_to_utf16(emoji, 6) != 4 ||
        atelier::platform::utf16_offset_to_utf8(emoji, 3) != 5 ||
        atelier::platform::utf16_offset_to_utf8(emoji, 2) != 1)
        return false;
    return span.front() == 2 && span.back() == 8 && span.first(2)[1] == 4 && span.last(2)[0] == 6 &&
           span.size_bytes() == sizeof(numbers);
}

static_assert(verify_constexpr_views());

void verify_utf_paths_and_arena() {
    using namespace atelier;
    using acceptance::helpers::require;

    InlineArray<wchar_t, 16> wide_buf;
    const auto ascii_wide = platform::utf8_to_utf16_into("Transform_01", wide_buf);
    require(ascii_wide && *ascii_wide == L"Transform_01" && wide_buf.capacity() == 16,
            "ASCII utf8_to_utf16_into spilled or corrupted inline storage.");

    const StringView rich{"alpha_中文_😀_omega_0123456789"};
    const auto rich_wide = platform::utf8_to_utf16_into(rich, wide_buf);
    require(rich_wide && wide_buf.capacity() > 16, "Non-ASCII utf8_to_utf16_into failed to spill.");
    const auto roundtrip = platform::utf16_to_utf8_string(*rich_wide);
    require(roundtrip && *roundtrip == rich, "utf16_to_utf8_string round-trip mismatch.");
    require(!platform::utf8_to_utf16_into("\xff\xfe", wide_buf), "Invalid UTF-8 was accepted by utf8_to_utf16_into.");
    const wchar_t isolated_surrogate[]{0xd800, 0};
    require(!platform::utf16_to_utf8_string({isolated_surrogate, 1}), "Isolated surrogate was accepted.");

    for (std::size_t u8 = 0; u8 <= rich.size(); ++u8) {
        const auto u16 = platform::utf8_offset_to_utf16(rich, u8);
        const auto snapped = platform::utf16_offset_to_utf8(rich, u16);
        if (u8 == rich.size() || (static_cast<unsigned char>(rich[u8]) & 0xc0u) != 0x80u)
            require(snapped == u8, "UTF-8 <-> UTF-16 runtime offset round-trip failed at code-point boundary.");
    }

    require(platform::paths_lexically_equal("C:/Project/Assets/../Scripts/Main.source",
                                            "c:\\project\\scripts\\main.source"),
            "paths_lexically_equal rejected equivalent Windows spellings.");
    require(platform::is_subpath_of("C:/Project/Scripts/Sub/Main.source", "c:\\project\\scripts\\") &&
                !platform::is_subpath_of("C:/Project_Other/Main.source", "C:/Project") &&
                !platform::is_subpath_of("C:/Project/../Outside/Main.source", "C:/Project") &&
                !platform::is_subpath_of("C:\\Project\\Main.source", "C:") &&
                !platform::is_subpath_of("C:Relative\\Main.source", "C:\\") &&
                platform::is_subpath_of("../shared/sub/file.source", "../shared") &&
                !platform::is_subpath_of("../other/file.source", "../shared"),
            "is_subpath_of containment check failed.");
    const auto generic = platform::generic_path_to_utf8("assets\\ui\\editor.ui.source");
    const auto long_generic =
        platform::generic_path_to_utf8(L"C:\\Projects\\FrostFoxAtelier\\Content\\脚本目录\\🚀SceneMain.source");
    const auto fname = platform::filename_utf8("assets\\ui\\editor.ui.source");
    require(generic && *generic == "assets/ui/editor.ui.source" && long_generic &&
                *long_generic == "C:/Projects/FrostFoxAtelier/Content/脚本目录/🚀SceneMain.source" && fname &&
                *fname == "editor.ui.source",
            "UTF-8 path helpers produced unexpected output.");

    // Verify String::replace in-place capacity reuse, geometric growth, literal replacement, and self-aliasing slice:
    {
        String lit_rep = String::literal<"hello_world">();
        require(lit_rep.replace(5, 1, "::").has_value() && lit_rep == "hello::world",
                "String::replace on literal_tag failed.");
        auto heap_str = String::from_utf8("0123456789abcdefghijklmnopqrstuvwxyz_0123456789");
        require(heap_str.has_value(), "Failed to construct heap String.");
        heap_str->reserve(128);
        const char* initial_ptr = heap_str->c_str();
        const std::size_t initial_cap = heap_str->capacity();
        require(heap_str->replace(10, 4, "ABCD").has_value() && heap_str->c_str() == initial_ptr &&
                    heap_str->capacity() == initial_cap,
                "Equal-length String::replace reallocated unnecessarily.");
        require(heap_str->replace(10, 4, "XY").has_value() && heap_str->c_str() == initial_ptr &&
                    heap_str->capacity() == initial_cap,
                "Shrinking String::replace did not reuse heap capacity in place.");
        require(heap_str->replace(10, 2, "ABCDEFGH").has_value() && heap_str->c_str() == initial_ptr,
                "Growing String::replace within reserved capacity reallocated unnecessarily.");
        require(heap_str->replace(0, 5, heap_str->slice(10, 8)).has_value() && heap_str->starts_with("ABCDEFGH56789"),
                "Self-aliasing String::replace corrupted content.");
    }

    // Verify SIMD pure-ASCII fast paths (> 32 units) for utf8_to_utf16 and utf16_to_utf8:
    {
        constexpr std::string_view long_ascii = "modules/editor/application/src/editor/code_intelligence.cpp";
        const auto w_ascii = platform::utf8_to_utf16(long_ascii);
        require(w_ascii.has_value() && w_ascii->size() == long_ascii.size(), "ASCII utf8_to_utf16 failed.");
        const auto u8_ascii = platform::utf16_to_utf8(*w_ascii);
        require(u8_ascii.has_value() && *u8_ascii == long_ascii, "ASCII utf16_to_utf8 round-trip failed.");
    }

    ScratchArena arena{1024};
    require(arena.allocate<int>(0).empty() && arena.reserved_bytes() == 0,
            "Zero-length arena allocation touched heap.");
    std::size_t peak_reserved = 0;
    {
        ScratchScope outer{arena};
        auto ints = arena.allocate<std::uint64_t>(8);
        ints[0] = 99;
        {
            ScratchScope inner{arena};
            auto big = arena.allocate_uninitialized<std::byte>(2048);
            require(big.size() == 2048, "Oversized scratch allocation failed.");
            auto* over = arena.allocate_bytes(64, 256);
            require((reinterpret_cast<std::uintptr_t>(over) & 255u) == 0,
                    "Over-aligned scratch allocation violated alignment.");
            peak_reserved = arena.reserved_bytes();
        }
        require(ints[0] == 99 && arena.reserved_bytes() == peak_reserved,
                "Inner ScratchScope rewind clobbered outer scope or released reusable blocks.");
        constexpr double sample_doubles[]{1.5, -2.25, 4.75, 8.125};
        auto reused_doubles = arena.copy<double>(Span<const double>{sample_doubles});
        require(reused_doubles.size() == 4 && reused_doubles[2] == 4.75 && arena.reserved_bytes() == peak_reserved,
                "Heterogeneous implicit-lifetime block reuse failed.");
        auto reused = arena.allocate_uninitialized<std::byte>(1536);
        require(reused.size() == 1536 && arena.reserved_bytes() == peak_reserved,
                "Sibling scope failed to reuse retained block.");
    }
    require(arena.used_bytes() == 0 && arena.reserved_bytes() == peak_reserved,
            "Outer ScratchScope failed to rewind watermark to zero.");
    const auto copied = arena.copy_string("scratch-label");
    require(copied == "scratch-label" && copied.data()[copied.size()] == '\0', "copy_string failed.");
    arena.release();
    require(arena.reserved_bytes() == 0 && arena.used_bytes() == 0, "ScratchArena::release retained memory.");
}

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
    verify_utf_paths_and_arena();
    verify_tasks();
    std::puts("Foundation acceptance: PASS (owning aliases, queued resources and failed join cleanup)");
}
