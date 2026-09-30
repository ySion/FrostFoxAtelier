#include "foundation/array.hpp"
#include "foundation/string.hpp"
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
using namespace atelier;
using Clock = std::chrono::steady_clock;
volatile std::uint64_t observed{};
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// Adapt only operation names. Both containers use the same allocator, inputs,
// value initialization and ordered erase; no unchecked indexing is timed.
template<class T>
class StdVector : public std::vector<T> {
public:
    StdVector() = default;
    explicit StdVector(Span<const T> values) : std::vector<T>(values.begin(), values.end()) {}
    void append(Span<const T> values) { this->insert(this->end(), values.begin(), values.end()); }
    template<class... A> T& emplace(A&&... arguments) { return this->emplace_back(std::forward<A>(arguments)...); }
    void remove_at(std::size_t first, std::size_t count) {
        this->erase(this->begin() + first, this->begin() + first + count);
    }
    template<class Predicate> std::size_t remove_if(Predicate predicate) { return std::erase_if(*this, predicate); }
};

struct alignas(64) Packet { std::array<std::uint32_t, 16> words; };
struct Resources { std::int64_t allocations, bytes, live; };
Resources resources() {
    mi_collect(true); // Flush page statistics outside timing.
    mi_stats_t stats;
    mi_stats_init(&stats);
    require(mi_stats_get(&stats), "mimalloc statistics ABI mismatch.");
    return {stats.malloc_normal_count.total + stats.malloc_huge_count.total,
        stats.malloc_normal.total + stats.malloc_huge.total,
        stats.malloc_normal.current + stats.malloc_huge.current};
}

// Keep the timed producer observable without scanning its entire output or
// letting inlining discard stores. This target does not enable LTO.
template<class T> __declspec(noinline) std::uint64_t consume(const T* data, std::size_t size) {
    if (!size) return 1;
    if constexpr (std::same_as<T, Packet>) return size + data[0].words[0] + data[size - 1].words[15];
    else if constexpr (std::same_as<T, String>) return size + data[0].size() + data[size - 1].size();
    else return size + data[0] + data[size - 1];
}

template<class Operation>
void measure(const char* implementation, const char* scenario, std::size_t iterations, Operation operation) {
    for (std::size_t i = 0; i != 1000; ++i) observed = operation(i);
    std::array<double, 7> samples{};
    Resources delta{};
    for (auto& sample : samples) {
        const auto before = resources();
        std::uint64_t checksum = 0;
        const auto start = Clock::now();
        for (std::size_t i = 0; i != iterations; ++i) checksum += operation(i);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        observed = checksum;
        require(checksum != 0, "Array measurement did not consume its results.");
        const auto after = resources();
        sample = static_cast<double>(elapsed) / static_cast<double>(iterations);
        delta.allocations += after.allocations - before.allocations;
        delta.bytes += after.bytes - before.bytes;
        require(after.live == before.live, "Array operation retained additional heap storage after warmup.");
    }
    std::ranges::sort(samples);
    std::printf("{\"kind\":\"array_operation\",\"implementation\":\"%s\",\"scenario\":\"%s\","
        "\"iterations\":%zu,\"samples\":7,\"median_ns_per_op\":%.3f,\"min_ns_per_op\":%.3f,"
        "\"allocations_per_op\":%.6f,\"rounded_bytes_per_op\":%.3f}\n",
        implementation, scenario, iterations, samples[3], samples[0],
        static_cast<double>(delta.allocations) / static_cast<double>(iterations * samples.size()),
        static_cast<double>(delta.bytes) / static_cast<double>(iterations * samples.size()));
}

template<template<class> class Container>
void workload(const char* implementation) {
    constexpr std::size_t iterations = 50000;
    std::array<std::uint32_t, 4096> inputs;
    for (std::size_t i = 0; i != inputs.size(); ++i) inputs[i] = static_cast<std::uint32_t>(i + 1);
    const Container<std::uint32_t> source{Span<const std::uint32_t>{inputs.data(), inputs.size()}};
    const Container<std::uint32_t> half{Span<const std::uint32_t>{inputs.data(), inputs.size() / 2}};
    Container<std::uint32_t> scratch;
    scratch.reserve(8192);
    std::printf("{\"kind\":\"array_layout\",\"implementation\":\"%s\",\"object_bytes\":%zu}\n",
        implementation, sizeof(scratch));
    measure(implementation, "copy_scalar_4096", iterations, [&](std::size_t) {
        Container<std::uint32_t> copy{source}; return consume(copy.data(), copy.size());
    });
    measure(implementation, "append_scalar_reserved", iterations, [&](std::size_t) {
        scratch.clear(); scratch.append({inputs.data(), inputs.size()}); return consume(scratch.data(), scratch.size());
    });
    measure(implementation, "assign_scalar_reuse", iterations, [&](std::size_t i) {
        scratch = i % 2 ? source : half; return consume(scratch.data(), scratch.size());
    });
    measure(implementation, "grow_scalar_4096", iterations, [&](std::size_t) {
        Container<std::uint32_t> copy{source}; copy.reserve(8192); return consume(copy.data(), copy.size());
    });
    scratch = source;
    scratch.reserve(8192);
    measure(implementation, "erase_scalar_ordered", iterations, [&](std::size_t) {
        scratch.remove_at(0, 32); scratch.append({inputs.data(), 32}); return consume(scratch.data(), scratch.size());
    });
    measure(implementation, "filter_scalar_batch", iterations / 5, [&](std::size_t) {
        scratch.clear(); scratch.append({inputs.data(), inputs.size()});
        scratch.remove_if([](std::uint32_t value) { return value % 4 == 0; });
        return consume(scratch.data(), scratch.size());
    });
    measure(implementation, "emplace_scalar_64", iterations, [&](std::size_t) {
        Container<std::uint32_t> values;
        for (std::uint32_t i = 1; i != 65; ++i) values.emplace(i);
        return consume(values.data(), values.size());
    });
    measure(implementation, "resize_zero_4096", iterations, [&](std::size_t) {
        Container<std::uint32_t> values; values.resize(4096); return consume(values.data(), values.size());
    });
    std::array<Packet, 512> packets;
    for (std::size_t i = 0; i != packets.size(); ++i)
        for (std::size_t j = 0; j != 16; ++j) packets[i].words[j] = static_cast<std::uint32_t>(i + j + 1);
    const Container<Packet> packet_source{Span<const Packet>{packets.data(), packets.size()}};
    measure(implementation, "copy_packet_512", iterations, [&](std::size_t) {
        Container<Packet> copy{packet_source}; return consume(copy.data(), copy.size());
    });
    std::array<String, 64> labels;
    for (std::size_t i = 0; i != labels.size(); ++i)
        labels[i].assign(i % 2 ? "Transform" : "asset:/weapons/steel_sword.asset");
    const Container<String> owned_source{Span<const String>{labels.data(), labels.size()}};
    Container<String> owned_scratch;
    owned_scratch.reserve(128);
    measure(implementation, "append_owned_strings_64", iterations / 5, [&](std::size_t) {
        owned_scratch.clear(); owned_scratch.append({labels.data(), labels.size()});
        return consume(owned_scratch.data(), owned_scratch.size());
    });
    measure(implementation, "assign_owned_strings_64", iterations / 5, [&](std::size_t) {
        owned_scratch = owned_source; return consume(owned_scratch.data(), owned_scratch.size());
    });
}
}

int main() {
    try {
        std::puts("{\"kind\":\"array_measurement\",\"timing\":\"steady_clock\",\"samples\":7}");
        const auto before = resources();
        workload<Array>("atelier");
        workload<StdVector>("msvc_stl");
        require(resources().live == before.live, "Array measurement leaked retained buffers.");
        std::puts("{\"kind\":\"array_measurement_result\",\"status\":\"PASS\"}");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Array measurement: FAIL: %s\n", error.what()); return 1;
    }
}
