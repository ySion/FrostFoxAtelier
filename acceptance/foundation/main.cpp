#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "foundation/optional.hpp"
#include "foundation/priority_queue.hpp"
#include "foundation/ring_deque.hpp"
#include "foundation/sha256.hpp"
#include "foundation/string.hpp"
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

namespace {
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
    verify_tasks();
    std::puts("Foundation acceptance: PASS (owning aliases, queued resources and failed join cleanup)");
}
