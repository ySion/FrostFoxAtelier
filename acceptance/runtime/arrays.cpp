#include "../helpers.hpp"
#include "runtime/foundation.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

// Copies may throw while moves are only copies, so Array edits take the copy-and-swap path.
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

// Moves cannot throw but swaps can, and Array::insert/move rotate elements with swap.
struct Swapper {
    std::int32_t value{};

    static inline bool fail{};

    friend void swap(Swapper& first, Swapper& second) {
        std::swap(first.value, second.value);
        if (fail)
            throw std::runtime_error("swap refused");
    }
};

static_assert(std::is_nothrow_move_constructible_v<Swapper> && !std::is_nothrow_swappable_v<Swapper>);

struct Owner {
    Array<Fragile> fragile;

    Array<Swapper> swappers;
};
}

void verify_array_elements() {
    Runtime runtime;
    const auto types = take(runtime.register_types({
        NativeRegistration<Fragile>{runtime, "arrays.Fragile", TypeKind::record}
            .field<&Fragile::value>("value")
            .field<&Fragile::fail>("fail")
            .build(),
        array_definition<Fragile>(runtime, "arrays.FragileArray"),
        NativeRegistration<Swapper>{runtime, "arrays.Swapper", TypeKind::record}
            .field<&Swapper::value>("value")
            .build(),
        array_definition<Swapper>(runtime, "arrays.SwapperArray"),
        NativeRegistration<Owner>{runtime, "arrays.Owner", TypeKind::object}
            .field<&Owner::fragile>("fragile")
            .field<&Owner::swappers>("swappers")
            .build(),
    }));
    const auto field = [&](TypeId type, const char* name) { return take(runtime.find_field(type, name)); };
    const auto at = [&](TypeId array, std::uint32_t index) { return take(runtime.element(array, index)); };
    auto owner = take(runtime.create(types[4]));
    const auto id = owner.id();
    const auto count = [&](std::span<const FieldId> path) { return take(runtime.element_count(id, path)); };
    const std::array fragile{field(types[4], "fragile")};
    const auto value = field(types[0], "value");
    take(runtime.insert_element(id, fragile, 0));
    take(runtime.insert_element(id, fragile, 1));
    take(runtime.write(id, std::array{fragile[0], at(types[1], 1), value}, Scalar{2.0f}));
    Fragile refused;
    refused.value = 9.0f;
    refused.fail = true;
    require(!runtime.insert_element(id, fragile, 1, {types[0], &refused}), "The element copy did not fail.");
    require(count(fragile) == 2 &&
                take(runtime.read(id, std::array{fragile[0], at(types[1], 1), value})) == Scalar{2.0f},
            "A failed insertion changed the Array.");

    const std::array swappers{field(types[4], "swappers")};
    const auto swapped = field(types[2], "value");
    for (const std::int32_t number : {11, 22}) {
        const Swapper item{number};
        take(runtime.insert_element(id, swappers, count(swappers), {types[2], &item}));
    }
    const auto order = [&] {
        std::string text;
        for (std::uint32_t i = 0; i != count(swappers); ++i)
            text += std::to_string(std::get<std::int32_t>(
                        take(runtime.read(id, std::array{swappers[0], at(types[3], i), swapped})))) +
                    ",";
        return text;
    };
    Swapper::fail = true;
    require(!runtime.insert_element(id, swappers, 0), "The insertion swap did not fail.");
    const auto inserted = order();
    require(!runtime.move_element(id, swappers, 0, 1), "The move swap did not fail.");
    Swapper::fail = false;
    require(inserted == "11,22," && order() == "11,22,", "A failed insertion or move left the Array reordered.");
    std::puts("[pass] Array edits: copy and swap failure rollback");
}
