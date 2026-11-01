#include "foundation/string.hpp"
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace atelier;

using Clock = std::chrono::steady_clock;

volatile std::uint64_t observed{};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Same UTF-8 boundary contract for the STL comparison. Native std::string alone
// accepts arbitrary bytes and would omit part of the engine's work.
class CheckedStdString {
public:
    CheckedStdString() = default;

    explicit CheckedStdString(StringView text) { assign(text); }

    void reserve(std::size_t capacity) { text_.reserve(capacity); }

    void clear() noexcept { text_.clear(); }

    void assign(StringView text) {
        validate(text);
        text_.assign(text.data(), text.size());
    }

    void append(StringView text) {
        validate(text);
        text_.append(text.data(), text.size());
    }

    void append(const CheckedStdString& text) { text_.append(text.text_); }

    StringView view() const { return {text_.data(), text_.size()}; }

private:
    static void validate(StringView text) {
        if (!text.is_valid_utf8())
            throw std::invalid_argument("Invalid UTF-8 in measurement.");
    }

    std::string text_;
};

struct Resources {
    std::int64_t allocations;

    std::int64_t bytes;

    std::int64_t live;
};

Resources resources() {
    // Flush mimalloc v3 page statistics outside the timed interval.
    mi_collect(true);
    mi_stats_t stats;
    mi_stats_init(&stats);
    require(mi_stats_get(&stats), "mimalloc statistics ABI mismatch.");
    return {stats.malloc_normal_count.total + stats.malloc_huge_count.total,
            stats.malloc_normal.total + stats.malloc_huge.total,
            stats.malloc_normal.current + stats.malloc_huge.current};
}

std::uint64_t consume(StringView text) {
    return text.size() +
           (text.empty() ? 0 : static_cast<unsigned char>(text[0]) + static_cast<unsigned char>(text[text.size() - 1]));
}

template <class Operation>
void measure(const char* implementation, const char* scenario, std::size_t iterations, Operation operation) {
    for (std::size_t i = 0; i != 1000; ++i)
        observed = operation(i);
    std::array<double, 7> samples{};
    Resources delta{};
    for (auto& sample : samples) {
        const auto before = resources();
        std::uint64_t checksum = 0;
        const auto start = Clock::now();
        for (std::size_t i = 0; i != iterations; ++i)
            checksum += operation(i);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        observed = checksum;
        require(checksum != 0, "String measurement did not consume its results.");
        const auto after = resources();
        sample = static_cast<double>(elapsed) / static_cast<double>(iterations);
        delta.allocations += after.allocations - before.allocations;
        delta.bytes += after.bytes - before.bytes;
    }
    std::ranges::sort(samples);
    std::printf("{\"kind\":\"string_operation\",\"implementation\":\"%s\",\"scenario\":\"%s\","
                "\"iterations\":%zu,\"samples\":7,\"median_ns_per_op\":%.3f,\"min_ns_per_op\":%.3f,"
                "\"allocations_per_op\":%.6f,\"rounded_bytes_per_op\":%.3f,\"allocator_flushed\":true}\n",
                implementation, scenario, iterations, samples[3], samples[0],
                static_cast<double>(delta.allocations) / static_cast<double>(iterations * samples.size()),
                static_cast<double>(delta.bytes) / static_cast<double>(iterations * samples.size()));
}

template <class S>
void workload(const char* implementation) {
    constexpr std::size_t iterations = 200000;
    constexpr std::array<StringView, 4> labels{"Health", "Transform", "Collider", "Position"};
    const std::string ascii(192, 'a');
    std::string utf8;
    for (int i = 0; i != 32; ++i)
        utf8 += "装备🙂";
    const std::string large(4096, 'b');
    const S short_source{StringView{"Transform"}};
    const S owned_source{StringView{ascii}};
    const S unicode_source{StringView{utf8}};
    const S long_source{StringView{large}};
    S scratch;
    scratch.reserve(512);
    std::printf("{\"kind\":\"string_layout\",\"implementation\":\"%s\",\"object_bytes\":%zu}\n", implementation,
                sizeof(S));

    measure(implementation, "short_build", iterations, [&](std::size_t i) {
        S text{labels[i % labels.size()]};
        return consume(text.view());
    });
    measure(implementation, "short_copy", iterations, [&](std::size_t) {
        S copy{short_source};
        return consume(copy.view());
    });
    measure(implementation, "assign_reuse", iterations, [&](std::size_t i) {
        scratch.assign(StringView{ascii}.slice(0, i % 2 ? 128 : 192));
        return consume(scratch.view());
    });
    measure(implementation, "append_reserved", iterations, [&](std::size_t) {
        scratch.clear();
        scratch.append(StringView{"path/"});
        scratch.append(StringView{ascii});
        scratch.append(StringView{".asset"});
        return consume(scratch.view());
    });
    measure(implementation, "append_owned_ascii", iterations, [&](std::size_t) {
        scratch.clear();
        scratch.append(owned_source);
        return consume(scratch.view());
    });
    measure(implementation, "append_owned_utf8", iterations, [&](std::size_t) {
        scratch.clear();
        scratch.append(unicode_source);
        return consume(scratch.view());
    });
    measure(implementation, "external_utf8", iterations, [&](std::size_t) {
        scratch.assign(StringView{utf8});
        return consume(scratch.view());
    });
    measure(implementation, "long_copy", iterations / 4, [&](std::size_t) {
        S copy{long_source};
        return consume(copy.view());
    });

    // A retained engine-style catalog: 75% short names, 25% asset paths.
    constexpr std::size_t count = 20000;
    std::vector<S> catalog;
    catalog.reserve(count); // Owner array capacity is outside the string sample.
    const auto before = resources();
    const auto start = Clock::now();
    for (std::size_t i = 0; i != count; ++i) {
        if (i % 4 == 0)
            catalog.emplace_back(StringView{ascii});
        else
            catalog.emplace_back(labels[i % labels.size()]);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    const auto retained = resources();
    std::uint64_t checksum = 0;
    for (const auto& text : catalog)
        checksum += consume(text.view());
    observed = checksum;
    catalog.clear();
    const auto released = resources();
    require(released.live == before.live, "String catalog retained live allocations after clear.");
    std::printf("{\"kind\":\"string_catalog\",\"implementation\":\"%s\",\"strings\":%zu,"
                "\"object_array_bytes\":%zu,\"build_ns_per_string\":%.3f,\"allocations\":%lld,"
                "\"retained_rounded_heap_bytes\":%lld,\"released_live_byte_delta\":%lld}\n",
                implementation, count, count * sizeof(S), static_cast<double>(elapsed) / static_cast<double>(count),
                static_cast<long long>(retained.allocations - before.allocations),
                static_cast<long long>(retained.live - before.live),
                static_cast<long long>(released.live - before.live));
}
}

int main() {
    try {
        std::puts(
            "{\"kind\":\"string_measurement\",\"validation\":\"strict_utf8\",\"timing\":\"steady_clock\",\"samples\":7}");
        const auto before = resources();
        void* block = ::operator new(4096);
        require(mi_usable_size(block) >= 4096, "Measurement allocator is not shared mimalloc.");
        const auto live = resources();
        ::operator delete(block);
        require(live.allocations > before.allocations && resources().live == before.live,
                "Allocator counters are not observable.");
        workload<String>("atelier");
        workload<CheckedStdString>("msvc_stl_checked");
        std::puts("{\"kind\":\"string_measurement_result\",\"status\":\"PASS\"}");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "String measurement: FAIL: %s\n", error.what());
        return 1;
    }
}
