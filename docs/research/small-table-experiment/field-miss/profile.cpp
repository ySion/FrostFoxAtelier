#include "index.hpp"
#include "presentation/presentation.hpp"
#include "world/world.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <source_location>

using namespace atelier;
using namespace field_study;

struct Request {
    TypeId type;
    std::string key;
    const Index* index;
    size_t table;
};

struct Table {
    std::string name;
    size_t fields;
    std::vector<Request> queries;
};

void check(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Field-miss diagnostic mismatch at line %u\n", where.line());
        fatal("Field-miss diagnostic mismatch.");
    }
}

template <class F>
void time_trace(const std::string& id, const std::vector<Request>& trace, F&& lookup) {
    const auto sweep = [&] {
        size_t correct = 0;
        for (const auto& q : trace)
            correct += lookup(q);
        return correct;
    };
    check(sweep() == trace.size());
    const auto runs = std::max<size_t>(32, 196608 / trace.size());
    size_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < runs; ++i) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        sum += sweep();
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    check(sum == runs * trace.size());
    std::printf("%s,%.5f\n", id.c_str(), elapsed / (runs * trace.size()));
}

int main(int argc, char** argv) {
    check(argc == 3);
    const std::string mode = argv[1];
    const bool catalog = mode == "catalog";
    const auto cpu = std::stoul(argv[2]);
    check(cpu < 64 && SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu));
    Runtime rt;
    check(world::register_types(rt).has_value() && presentation::register_types(rt).has_value());
    Index::probe = [](const Index& index, StringView key) -> const FieldId* {
        Index::captured = &index;
        return index.baseline.find(key);
    };
    std::vector<Table> tables;
    std::vector<Request> fields;
    for (const auto type : rt.types()) {
        const auto& info = **rt.describe(type);
        if (info.fields.size() <= 4)
            continue;
        Index::captured = nullptr;
        (void)rt.find_field(type, "");
        check(Index::captured != nullptr);
        for (const auto& f : info.fields)
            fields.push_back({type, f.name, Index::captured, tables.size()});
        tables.push_back({info.name, info.fields.size(), {}});
    }

    std::mt19937 gen(1203);
    std::vector<Request> trace;
    for (unsigned i = 0; i < 4096; ++i) {
        auto q = fields[gen() % fields.size()];
        q.key.back() = '#';
        for (auto probe : {baseline, owned, indexed, fixed})
            check(probe(*q.index, StringView{q.key}) == nullptr);
        tables[q.table].queries.push_back(q);
        trace.push_back(std::move(q));
    }
    if (catalog) {
        for (size_t i = 0; i < tables.size(); ++i) {
            const auto& t = tables[i];
            unsigned temporary_heap = 0;
            unsigned before = 0, after = 0, bang_before = 0, bang_after = 0;
            for (const auto& q : t.queries) {
                temporary_heap += q.key.size() > 15;
                if (t.fields <= 16) {
                    auto bang = q.key;
                    bang.back() = '!';
                    for (const auto& f : fields) {
                        if (f.table != i) continue;
                        before += Index::Dense::name_tag(StringView{q.key}) == Index::Dense::name_tag(StringView{f.key});
                        after += Index::Revised::name_tag(StringView{q.key}) == Index::Revised::name_tag(StringView{f.key});
                        bang_before += Index::Dense::name_tag(StringView{bang}) == Index::Dense::name_tag(StringView{f.key});
                        bang_after += Index::Revised::name_tag(StringView{bang}) == Index::Revised::name_tag(StringView{f.key});
                    }
                }
            }
            std::printf("%s,%zu,%zu,%u,%u,%u,%u,%u\n", t.name.c_str(), t.fields, t.queries.size(), temporary_heap,
                        before, after, bang_before, bang_after);
        }
        return 0;
    }

    if (mode == "tag4") Index::probe = baseline;
    else if (mode == "owned") Index::probe = owned;
    else if (mode == "indexed") Index::probe = indexed;
    else if (mode == "fixed") Index::probe = fixed;
    else if (mode == "error") Index::probe = missing;
    else return 1;

    auto run = [&](const std::string& label, const std::vector<Request>& sequence) {
        for (const auto& q : sequence) {
            const auto result = rt.find_field(q.type, q.key);
            check(!result && result.error().code == ErrorCode::invalid_member &&
                  result.error().message == "Unknown field: " + q.key);
        }
        const auto lookup = [&](const Request& q) {
            auto result = rt.find_field(q.type, q.key);
            return !result && result.error().code == ErrorCode::invalid_member;
        };
        time_trace(label + "_full", sequence, lookup);
        omit_message = true;
        time_trace(label + "_emptyerr", sequence, lookup);
        omit_message = false;
        time_trace(label + "_index", sequence, [&](const Request& q) {
            return Index::probe(*q.index, StringView{q.key}) == nullptr;
        });
    };
    run("mixed", trace);
    auto ordered = trace;
    std::stable_partition(ordered.begin(), ordered.end(), [&](const Request& q) { return tables[q.table].fields <= 16; });
    run("partition", ordered);
    for (const auto& t : tables)
        run(t.name, t.queries);
    // Same lengths, addresses of registered names, type sequence, error
    // constructor and number of requests. Only the last query byte changes.
    auto bang = trace;
    for (auto& q : bang) {
        q.key.back() = '!';
        check(!baseline(*q.index, StringView{q.key}) && !owned(*q.index, StringView{q.key}) &&
              !fixed(*q.index, StringView{q.key}));
    }
    run("mixed_bang", bang);
    std::erase_if(bang, [&](const Request& q) { return !tables[q.table].name.starts_with("math.matrix4"); });
    run("matrix_bang", bang);
    for (auto& q : bang) q.key.back() = '#';
    run("matrix_hash", bang);

    if (mode != "error") {
        std::mt19937 hit_gen(1203);
        std::vector<Request> hits;
        for (size_t i = 0; i < trace.size(); ++i) {
            const auto& q = fields[hit_gen() % fields.size()];
            const auto expected = baseline(*q.index, StringView{q.key});
            const auto result = rt.find_field(q.type, q.key);
            check(expected && result && *result == *expected);
            hits.push_back(q);
        }
        time_trace("mixed_hit", hits, [&](const Request& q) { return rt.find_field(q.type, q.key).has_value(); });
    }
}
