#include "../helpers.hpp"
#include "runtime/foundation.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

std::string_view text(const String& value) {
    return {value.c_str(), value.size()};
}

// Copies may throw and moves are copies, so only the Array that holds them swaps without throwing.
struct Fragile {
    float value{};

    bool fail{};

    Fragile() = default;

    Fragile(const Fragile& other) : value(other.value), fail(other.fail) {
        if (other.fail)
            throw std::runtime_error("copy refused");
    }

    Fragile& operator=(const Fragile&) = default;
};
}

void verify_value_paths() {
    Runtime runtime;
    auto fragile_definition = NativeRegistration<Fragile>{runtime, "values.Fragile", TypeKind::record}
                                  .field<&Fragile::value>("value")
                                  .field<&Fragile::fail>("fail")
                                  .build();
    fragile_definition.native.value_operations_preserve_borrows = true;
    FieldDefinition weight;
    weight.name = "weight";
    weight.value = {TypeReference::native<float>()};
    weight.range = {.hard_min = Scalar{0.0f}, .hard_max = Scalar{1.0f}};
    const auto types = take(runtime.register_types({
        string_definition(runtime),
        std::move(fragile_definition),
        array_definition<Fragile>(runtime, "values.FragileArray"),
        {"values.Pair", TypeKind::record, {{"text", {TypeReference{"core.String"}}}, std::move(weight)}, {}, {}},
        {"values.Twin",
         TypeKind::record,
         {{"first", {TypeReference{"values.FragileArray"}}}, {"second", {TypeReference{"values.FragileArray"}}}},
         {},
         {}},
        {"values.Bag",
         TypeKind::object,
         {{"pair", {TypeReference{"values.Pair"}}},
          {"fragile", {TypeReference{"values.FragileArray"}}},
          {"twin", {TypeReference{"values.Twin"}}}},
         {},
         {}},
    }));
    const auto string_type = types[0];
    const auto fragile_array = types[2];
    const auto pair_type = types[3];
    const auto twin_type = types[4];
    const auto field = [&](TypeId type, const char* name) { return take(runtime.find_field(type, name)); };
    const auto pair = field(types[5], "pair");
    const auto fragile = field(types[5], "fragile");
    const auto twin = field(types[5], "twin");
    const auto first = field(twin_type, "first");
    const auto second = field(twin_type, "second");
    auto bag = take(runtime.create(types[5]));
    const auto id = bag.id();

    Array<Fragile> good{Fragile{}};
    good[0].value = 2.0f;
    take(runtime.write(id, std::array{fragile}, ValueInput{fragile_array, &good}));
    Array<Fragile> bad{Fragile{}, Fragile{}};
    bad[1].fail = true;
    require(!runtime.write(id, std::array{fragile}, ValueInput{fragile_array, &bad}), "The element copy did not fail.");
    require(take(runtime.element_count(id, std::array{fragile})) == 1 &&
                take(runtime.read(id, std::array{fragile, take(runtime.element(fragile_array, 0)),
                                                 field(types[1], "value")})) == Scalar{2.0f},
            "A failed whole-Array write changed the field.");
    struct TwinValue {
        Array<Fragile> first;

        Array<Fragile> second;
    };
    TwinValue twin_value;
    twin_value.first.add(good[0]);
    take(runtime.write(id, std::array{twin}, ValueInput{twin_type, &twin_value}));
    TwinValue partial;
    partial.first.resize(3);
    partial.second.add(Fragile{});
    partial.second[0].fail = true;
    require(!runtime.write(id, std::array{twin}, ValueInput{twin_type, &partial}), "The record copy did not fail.");
    require(take(runtime.element_count(id, std::array{twin, first})) == 1 &&
                take(runtime.element_count(id, std::array{twin, second})) == 0,
            "A failed whole-record write changed an earlier field.");
    take(runtime.with_native<Array<Fragile>>(id, std::array{fragile}, [](Array<Fragile>& values) -> Result<void> {
        values[0].fail = true;
        return {};
    }));
    Array<Fragile> output{Fragile{}};
    output[0].value = 7.0f;
    require(!runtime.read(id, std::array{fragile}, ValueOutput{fragile_array, &output}), "The read copy did not fail.");
    require(output.size() == 1 && output[0].value == 7.0f, "A failed read changed its output.");

    struct PairValue {
        String text;

        float weight;
    };
    PairValue heavy{String{StringView{"heavy"}}, 7.0f};
    take(runtime.write(id, std::array{pair}, ValueInput{pair_type, &heavy}));
    const std::array weight_path{pair, field(pair_type, "weight")};
    take(runtime.with_native<float>(id, weight_path, [](float& value) -> Result<void> {
        value = std::bit_cast<float>(0x7fc0'1234u);
        return {};
    }));
    PairValue before;
    take(runtime.read(id, std::array{pair}, ValueOutput{pair_type, &before}));
    PairValue replacement{String{StringView{"replacement"}}, 0.5f};
    take(runtime.write(id, std::array{pair}, ValueInput{pair_type, &replacement}));
    take(runtime.restore(id, std::array{pair}, ValueOutput{pair_type, &before}));
    String copy;
    take(runtime.read(id, std::array{pair, field(pair_type, "text")}, ValueOutput{string_type, &copy}));
    float restored{};
    take(runtime.read(id, weight_path, ValueOutput{take(runtime.native_type<float>()), &restored}));
    require(text(copy) == "heavy" && std::bit_cast<std::uint32_t>(restored) == 0x7fc0'1234u &&
                text(before.text) == "replacement",
            "restore did not exchange a whole record with a NaN leaf.");
    std::puts("[pass] Value paths: failed replacement/read atomicity and owning record restore");
}
