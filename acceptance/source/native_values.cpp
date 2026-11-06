#include "../helpers.hpp"
#include "source/native_values.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>

namespace {
using namespace atelier;

using namespace atelier::acceptance::borrow;

using namespace atelier::acceptance::helpers;

struct RetiredAsset {
    static inline int destroyed{};

    ~RetiredAsset() { ++destroyed; }
};
}

void verify_native_value_borrows() {
    Runtime runtime;
    take(runtime.register_types(
        {array_definition<Particle>(runtime, "borrow.Particles"), reflection::Definition<Particle>::build(runtime),
         reflection::Definition<Position>::build(runtime), string_definition(runtime),
         NativeRegistration<RetiredAsset>{runtime, "borrow.RetiredAsset", TypeKind::object}.build()}));
    const auto* layout = take(runtime.describe(take(runtime.native_type<Particle>())));
    require(layout->size == sizeof(Particle) && layout->alignment == alignof(Particle) &&
                layout->fields[0].offset == offsetof(Particle, position) &&
                layout->fields[2].offset == offsetof(Particle, name),
            "Generated direct layout does not match the native record.");
    auto update = take(source::Program::create(runtime));
    auto candidate = take(source::Program::prepare(take(update->request("native-values.source", R"(
        float update(Array<borrow.Particle>& values, const Array<borrow.Particle>& source, float delta) {
            uint index = 0;
            float total = 0.0;
            while (index < values.size()) {
                values[index].position.x = source[index].position.x + delta;
                values[index].count = values[index].count + 1;
                total = total + values[index].position.x;
                index = index + 1;
            }
            return total;
        }
    )"))));
    take(update->try_publish(candidate));
    {
        Array<Particle> particles;
        particles.resize(2);
        for (std::uint32_t i = 0; i < 2; ++i) {
            particles[i].position = {static_cast<float>(i), 2, 3};
            particles[i].count = static_cast<std::int32_t>(i);
        }
        const auto& read_only = particles; // Legal alias, not LLVM noalias storage.
        auto retired = take(runtime.create(take(runtime.native_type<RetiredAsset>())));
        retired.reset();
        take(runtime.begin_collection());
        require(
            std::get<float>(*take(update->entry().invoke(
                {}, std::array{Argument::borrow(particles), Argument::borrow(read_only), Argument{Scalar{1.f}}}))) == 3,
            "Borrowed source returned an incorrect total.");
        require(RetiredAsset::destroyed == 0, "GC ran inside a synchronous value borrow.");
        for (std::uint32_t i = 0; i < 2; ++i)
            require(particles[i].position.x == static_cast<float>(i + 1) && particles[i].position.y == 2 &&
                        particles[i].count == static_cast<std::int32_t>(i + 1),
                    "Direct native stores were lost.");
        while (runtime.collection_phase() != CollectionPhase::idle)
            take(runtime.collect_step({1}));
        require(RetiredAsset::destroyed == 1, "Host could not collect after the borrowed call returned.");
        auto partial = take(source::Program::create(runtime));
        candidate = take(source::Program::prepare(take(partial->request(
            "partial.source",
            "void partial(Array<borrow.Particle>& values) { values[0].count = 77; values[values.size()].count = 0; }"))));
        take(partial->try_publish(candidate));
        require(!partial->entry().invoke({}, std::array{Argument::borrow(particles)}) && particles[0].count == 77,
                "Bounds failure hid already committed native writes.");
        particles.clear();
    }
    require(Particle::live == 0, "Borrowed invocation extended native record lifetime past its owner.");
    std::puts("Native value borrows: PASS (shared layout, direct stores and failed borrowed-call lifetime)");
}
