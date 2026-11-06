#include "../helpers.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>

using namespace atelier;

using namespace atelier::acceptance::helpers;

void verify_source_types() {
    Runtime runtime;
    auto program = take(source::Program::create(runtime));
    auto candidate = take(source::Program::prepare(take(program->request("damage.source", R"(
        struct combat.DamageInfo { combat.Point point; combat.Kind kind; uint amount; };
        enum combat.Kind : uint { Physical, Fire = 7, Ice }
        struct combat.Point { float x; int count; };
        uint process(combat.DamageInfo& damage) {
            combat.DamageInfo snapshot = damage;
            combat.Point origin;
            origin.x = 2.0;
            combat.DamageInfo copy;
            copy = snapshot;
            copy.point = origin;
            bump(damage.amount, 4);
            damage.point.x = damage.point.x + 1.0;
            damage.kind = combat.Kind::Ice;
            if (copy.point.count != 0) { return 999; }
            if (snapshot.kind != combat.Kind::Fire) { return 998; }
            if (copy.point.x != 2.0) { return 997; }
            return snapshot.amount + copy.amount + damage.amount;
        }
        void bump(uint& value, uint amount) { value = value + amount; }
    )",
                                                                         "process"))));
    take(program->try_publish(candidate));
    const auto damage = take(runtime.find_type("combat.DamageInfo"));
    const auto* info = take(runtime.describe(damage));
    require(info->size == 16 && info->alignment == 4, "Source record layout differs from its declared fields.");
    // Native layout is the oracle; no reflected offset is used to locate the result.
    struct Damage {
        float x;

        std::int32_t count;

        std::uint32_t kind;

        std::uint32_t amount;
    };
    Damage value{5.0f, 0, 7, 3};
    const std::array arguments{Argument::borrow(damage, &value, &value)};
    require(std::get<std::uint32_t>(*take(program->entry().invoke({}, arguments))) == 13 && value.amount == 7 &&
                value.x == 6.0f && value.kind == 8,
            "Source/C++ shared storage lost record copies, reference writes or enum values.");
    std::puts("Source types: PASS (shared record layout and generated value operations)");
}
