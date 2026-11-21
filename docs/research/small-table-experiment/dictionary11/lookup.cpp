#include "foundation/map.hpp"
#include "presentation/presentation.hpp"
#include "world/world.hpp"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using atelier::StringView;
using Clock = std::chrono::steady_clock;
bool report = false;

void require(bool ok) {
    if (!ok) {
        std::fputs("Invalid lookup trace or unexpected result.\n", stderr);
        std::exit(1);
    }
}

// The generator and BinaryReader both use little-endian numbers on Windows x64.
template <class T>
T number(std::istream& in) {
    T value;
    require(static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value))));
    return value;
}

std::string read_text(std::istream& in) {
    std::string value(number<uint32_t>(in), '\0');
    require(static_cast<bool>(in.read(value.data(), value.size())));
    return value;
}

template <class Key>
using EngineMap = atelier::Map<Key, uint32_t>;
template <class Key>
using OriginalMap = ankerl::unordered_dense::map<Key, uint32_t, atelier::Hash<Key>, atelier::Equal<Key>>;

template <class Table, class Key>
uint32_t lookup(const Table& table, Key key) {
    const auto found = table.find(key);
    if constexpr (std::is_pointer_v<decltype(table.find(key))>)
        return found ? *found : 0;
    else
        return found != table.end() ? found->second : 0;
}

template <class F>
void measure(const std::string& metric, size_t queries, uint64_t expected, F sweep) {
    const auto warm_until = Clock::now() + std::chrono::milliseconds(150);
    do {
        std::atomic_signal_fence(std::memory_order_acq_rel);
        require(sweep() == expected);
    } while (Clock::now() < warm_until);

    constexpr unsigned passes = 32;
    uint64_t total = 0;
    const auto start = Clock::now();
    for (unsigned i = 0; i < passes; ++i) {
        std::atomic_signal_fence(std::memory_order_acq_rel);
        total += sweep();
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    require(total == passes * expected);
    if (report)
        std::printf("%s,%.5f,%llu,0\n", metric.c_str(), ns / (queries * passes),
                    static_cast<unsigned long long>(expected));
}

template <template <class> class Map>
void integer_case(std::istream& in, const std::string& metric) {
    Map<uint64_t> table;
    const auto count = number<uint32_t>(in);
    table.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto key = number<uint64_t>(in);
        table.try_emplace(key, i + 1);
    }
    require(table.size() == count);
    std::vector<uint64_t> keys(number<uint32_t>(in));
    uint64_t expected = 0;
    for (auto& key : keys) {
        key = number<uint64_t>(in);
        const auto value = number<uint32_t>(in);
        require(lookup(table, key) == value);
        expected += value;
    }
    measure(metric, keys.size(), expected, [&] {
        uint64_t sum = 0;
        for (auto key : keys)
            sum += lookup(table, key);
        return sum;
    });
}

struct NameQuery {
    uint32_t table;
    StringView key;
};

template <template <class> class Map>
void name_case(std::istream& in, const std::string& metric) {
    const auto count = number<uint32_t>(in);
    std::vector<std::vector<std::string>> names(count);
    std::vector<Map<StringView>> tables(count);
    for (size_t t = 0; t < count; ++t) {
        const auto size = number<uint32_t>(in);
        names[t].reserve(size);
        tables[t].reserve(size);
        for (uint32_t i = 0; i < size; ++i) {
            names[t].push_back(read_text(in));
            tables[t].try_emplace(StringView{names[t].back()}, i + 1);
        }
        require(tables[t].size() == size);
    }
    std::vector<NameQuery> queries(number<uint32_t>(in));
    std::vector<std::string> query_names;
    query_names.reserve(queries.size());
    uint64_t expected = 0;
    for (auto& query : queries) {
        query.table = number<uint32_t>(in);
        query_names.push_back(read_text(in));
        query.key = StringView{query_names.back()};
        const auto value = number<uint32_t>(in);
        require(query.table < count && lookup(tables[query.table], query.key) == value);
        expected += value;
    }
    measure(metric, queries.size(), expected, [&] {
        uint64_t sum = 0;
        for (const auto& query : queries)
            sum += lookup(tables[query.table], query.key);
        return sum;
    });
}

void export_members() {
    atelier::Runtime rt;
    require(atelier::world::register_types(rt).has_value());
    require(atelier::presentation::register_types(rt).has_value());
    unsigned t = 0;
    for (const auto id : rt.types()) {
        const auto& info = **rt.describe(id);
        for (const auto& f : info.fields)
            std::printf("field\t%u\t%s\n", t, f.name.c_str());
        for (const auto& m : info.methods)
            std::printf("method\t%u\t%s\n", t, m.name.c_str());
        ++t;
    }
}

void replay(std::string_view backend, const char* path) {
    std::ifstream in(path, std::ios::binary);
    const auto cases = number<uint32_t>(in);
    for (uint32_t c = 0; c < cases; ++c) {
        const auto metric = read_text(in);
        const auto kind = number<uint32_t>(in);
        require(kind <= 1);
        if (kind == 0) {
            if (backend == "atelier") integer_case<EngineMap>(in, metric);
            else integer_case<OriginalMap>(in, metric);
        } else {
            if (backend == "atelier") name_case<EngineMap>(in, metric);
            else name_case<OriginalMap>(in, metric);
        }
    }
    require(in.peek() == std::char_traits<char>::eof());
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "members") {
        export_members();
        return 0;
    }
    require(argc == 4);
    const std::string_view backend{argv[1]};
    require(backend == "atelier" || backend == "ankerl");
    const unsigned cpu = std::stoul(argv[3]);
    require(cpu < sizeof(DWORD_PTR) * 8 && SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu));
    replay(backend, argv[2]);
    report = true;
    replay(backend, argv[2]);
}
