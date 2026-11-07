#include "memory/fatal.hpp"
#include "foundation/string.hpp"
#include "foundation/function_ref.hpp"
#include "foundation/scope_exit.hpp"
#include "foundation/flags.hpp"
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <cstdio>
#include <cstdint>
#include <array>
#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

enum class WorkFlag : unsigned { none = 0, read = 1, write = 2, publish = 8 };

template <>
inline constexpr bool atelier::enable_enum_flags<WorkFlag> = true;

namespace {
using namespace atelier;

void require(bool condition, const char* message) {
    if (!condition)
        fatal(message);
}

std::int64_t live_bytes() {
    mi_collect(true);
    mi_stats_t stats;
    mi_stats_init(&stats);
    require(mi_stats_get(&stats), "mimalloc statistics ABI mismatch.");
    return stats.malloc_normal.current + stats.malloc_huge.current;
}

void utility_workload() {
    auto callback = [sum = std::make_unique<int>(3)](int value) noexcept { return *sum += value; };
    static_assert(std::is_constructible_v<FunctionRef<int(int) noexcept>, decltype(callback)&&>);
    static_assert(!std::is_constructible_v<FunctionRef<int(int) noexcept>, decltype([](int n) { return n; })>);
    const auto invoke = [](FunctionRef<int(int) noexcept> function) { return function(5); };
    require(invoke([value = std::make_unique<int>(7)](int n) noexcept { return *value + n; }) == 12,
            "FunctionRef lost a temporary callable during its call expression.");
    FunctionRef<int(int) noexcept> borrowed{callback};
    static_assert(noexcept(borrowed(1)) && sizeof(borrowed) == 16);
    auto copied = borrowed;
    require(borrowed(4) == 7 && copied(2) == 9, "FunctionRef copied or lost its borrowed callable state.");
    FunctionRef<int(int) noexcept> function{+[](int value) noexcept { return value * 3; }};
    require(function(5) == 15, "FunctionRef did not retain a free function pointer.");
    struct QualifiedCallable {
        int operator()() volatile noexcept { return 29; }
    };
    volatile QualifiedCallable qualified;
    FunctionRef<int() noexcept> qualified_ref{qualified};
    require(qualified_ref() == 29, "FunctionRef lost the callable's volatile qualification.");
    int cleanups{};
    const auto reject = [&]() -> std::expected<void, int> {
        ScopeExit cleanup{[&]() noexcept { ++cleanups; }};
        auto transferred = std::move(cleanup);
        return std::unexpected(7);
    };
    require(!reject() && cleanups == 1, "ScopeExit lost cleanup on explicit failure or ran a moved action twice.");
    {
        ScopeExit cleanup{[&]() noexcept { ++cleanups; }};
        cleanup.release();
    }
    require(cleanups == 1, "ScopeExit release still executed cleanup.");
    const auto mask = flags_or(WorkFlag::read, WorkFlag::write);
    require(has_any(WorkFlag::read, mask) && !has_all(WorkFlag::read, mask) && has_all(mask, WorkFlag::none) &&
                !has_any(mask, WorkFlag::none) &&
                flags_without(flags_or(mask, WorkFlag::publish), mask) == WorkFlag::publish &&
                flags_xor(mask, WorkFlag::write) == WorkFlag::read,
            "Flag any/all, zero mask or bit removal semantics changed.");
}

void editing_workload() {
    auto text = String::literal<"Ab中-Ab中">();
    auto preserved = text;
    text.to_ascii_lower();
    require(text.view() == "ab中-ab中" && preserved.view() == "Ab中-Ab中", "ASCII conversion changed a literal copy.");
    text.to_ascii_upper();
    require(text.view() == "AB中-AB中" && text.view().starts_with('A') && text.view().ends_with_any({"x", "中"}) &&
                text.view().starts_with_any({"x", "AB"}) && text.view().starts_with_ascii_case_insensitive("ab中") &&
                text.view().ends_with_ascii_case_insensitive("ab中") &&
                !text.view().equals_ascii_case_insensitive("AB中"),
            "ASCII matching changed non-ASCII bytes or prefix/suffix length semantics.");
    const auto replaced = text.replace_all(text.view().slice(0, 2), text.view().slice(2, 3));
    require(replaced && *replaced == 2 && text.view() == "中中-中中", "Replacement invalidated aliased views.");
    const auto saved = text;
    require(!text.replace(1, 1, "x") && text == saved && !text.replace_all("missing", StringView{"\xff", 1}) &&
                text == saved,
            "Invalid UTF-8 replacement modified the original text.");
    require(text.replace(0, 3, "").has_value() && text.view() == "中-中中" && text.replace_all("", "x") == 0,
            "Erase or empty-search replacement changed its contract.");
    auto overlap = String::literal<"aaa">();
    require(overlap.replace_all("aa", "b") == 1 && overlap.view() == "ba", "Replacement used overlapping matches.");
    auto heap = String::literal<"012345678901234567890123456789中\0Z">();
    require(heap.replace(0, 1, heap.view().slice(30)).has_value() &&
                heap.view() == StringView{"中\0Z12345678901234567890123456789中\0Z", 39},
            "Heap replacement lost aliased UTF-8 or embedded zero bytes.");
    const auto minimum = String::from_number(std::numeric_limits<std::int64_t>::min());
    require(minimum && minimum->view() == "-9223372036854775808" &&
                minimum->view().parse_number<std::int64_t>() == std::numeric_limits<std::int64_t>::min() &&
                StringView{"ff"}.parse_number<unsigned>(16) == 255,
            "Integer text conversion lost value or base semantics.");
    const auto decimal = String::from_number(-0.125);
    require(decimal.view() == "-0.125" && decimal.view().parse_number<double>() == -0.125 &&
                StringView{"1.25e2"}.parse_number<double>() == 125.0,
            "Floating text conversion changed value.");
    const auto integer_suffix = StringView{"256x"}.parse_number<std::uint8_t>();
    const auto floating_suffix = StringView{"1e999x"}.parse_number<double>();
    require(!integer_suffix && integer_suffix.error() == std::errc::invalid_argument,
            "Integer overflow hid trailing input.");
    require(!floating_suffix && floating_suffix.error() == std::errc::invalid_argument,
            "Floating overflow hid trailing input.");
    for (const double value : {std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min(),
                               -std::numeric_limits<double>::infinity()}) {
        const auto formatted = String::from_number(value);
        require(formatted.view().parse_number<double>() == value, "Floating text failed to round-trip an extreme.");
    }
}
}

int main() {
    atelier::install_fatal_handler();
    utility_workload();
    editing_workload();
    static_assert(std::ranges::input_range<StringSplit> && std::ranges::input_range<CodePointRange>);
    const auto text = StringView{"\t甲,,乙,\r\n"}.trim();
    const std::array<StringView, 4> pieces{"甲", "", "乙", ""};
    require(std::ranges::equal(text.split(","), pieces), "Split lost an empty piece or trim changed text.");
    require(std::ranges::equal(StringView{}.split(","), std::array{StringView{}}) &&
                std::ranges::equal(text.split(""), std::array{text}),
            "Empty text or delimiter did not produce one whole piece.");
    const auto first = text.partition(",");
    const auto last = text.rpartition(",");
    const auto absent = text.rpartition(";");
    require(first.before == "甲" && first.separator == "," && first.after == ",乙," && last.before == "甲,,乙" &&
                last.separator == "," && last.after.empty() && absent.before.empty() && absent.separator.empty() &&
                absent.after == text,
            "Partition boundaries or missing-separator direction changed.");
    const auto points = StringView{"A😀\0中", 9}.code_points();
    require(points.has_value(), "Valid Unicode code points were rejected.");
    const std::array<CodePoint, 4> expected{{{U'A', 0, 1}, {U'😀', 1, 4}, {U'\0', 5, 1}, {U'中', 6, 3}}};
    require(std::ranges::equal(*points, expected,
                               [](CodePoint a, CodePoint b) {
                                   return a.value == b.value && a.byte_offset == b.byte_offset &&
                                          a.byte_count == b.byte_count;
                               }),
            "Code point traversal confused byte offsets, embedded NUL or non-BMP text.");
    require(!StringView{"A\xf0\x9f", 3}.code_points(), "Code point traversal accepted a truncated final character.");
    const std::string ascii(192, 'a');
    std::vector<String> catalog;
    catalog.reserve(2);
    const auto before = live_bytes();
    auto short_text = String::from_utf8("012345678901234567890123456789");
    require(short_text && short_text->size() == 30 && short_text->capacity() == 30, "Thirty bytes did not fit inline.");
    require(live_bytes() == before, "Inline String allocated a buffer.");
    {
        auto literal = String::literal<"A static label longer than thirty bytes">();
        catalog.push_back(literal);
        catalog.push_back(literal);
        require(live_bytes() == before, "Literal construction or copy allocated a buffer.");
        require(catalog[0].assign(catalog[0].view().slice(2, 6)).has_value(), "Literal slice assignment failed.");
        require(catalog[0].view() == "static" && catalog[1] == literal, "Literal write modified its other copies.");
        catalog.clear();
        literal.clear();
        require(literal.empty() && literal.capacity() == 30, "Literal clear did not return to empty inline storage.");
    }
    require(short_text->append(short_text->view().slice(29)).has_value(), "Self-view append failed.");
    require(short_text->view() == "0123456789012345678901234567899" && short_text->size() == 31,
            "Inline-to-heap append invalidated its self-view.");
    short_text->release();
    require(live_bytes() == before, "Released String retained its buffer.");
    auto owned = String::from_utf8(StringView{ascii});
    require(owned.has_value(), "Valid UTF-8 was rejected.");
    catalog.emplace_back(std::move(*owned));
    require(catalog[0].view().std_view() == ascii, "String buffer lost its input.");
    const auto bad = catalog[0].assign(StringView{"\xed\xa0\x80", 3});
    require(!bad && bad.error() == StringError::invalid_utf8 && catalog[0].view().std_view() == ascii,
            "Rejected UTF-8 changed the previous value.");
    catalog.clear();
    require(live_bytes() == before, "String catalog retained live allocations after clear.");
    auto embedded = String::literal<"a\0b">();
    require(embedded.size() == 3 && embedded[1] == '\0' && embedded.c_str()[3] == '\0',
            "String literal lost an embedded zero or trailing zero.");
    std::puts("String buffer lifetime: PASS");
    return 0;
}
