#include "memory/fatal.hpp"
#include "foundation/string.hpp"
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <cstdio>
#include <cstdint>
#include <array>
#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

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
}

int main() {
    atelier::install_fatal_handler();
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
