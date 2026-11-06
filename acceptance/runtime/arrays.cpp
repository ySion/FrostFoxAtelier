#include "../helpers.hpp"
#include "runtime/foundation.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

// Failure belongs to registered operations, not C++ special members.
struct Fragile {
    float value{};

    bool fail{};
};

struct Swapper {
    std::int32_t value{};

    static inline bool fail{};
};

template <class T>
TypeDefinition checked_array_definition(Runtime& runtime, std::string name) {
    auto definition = array_definition<T>(runtime, std::move(name));
    static const auto mutate = definition.native.sequence_mutate;
    definition.native.sequence_mutate = [](SequenceMutation operation, void* target, const void* source,
                                           std::size_t count) noexcept -> Result<void> {
        auto& array = *static_cast<Array<T>*>(target);
        auto candidate = array;
        ATELIER_TRY(mutate(operation, &candidate, source, count));
        // Deliberately reject after modifying the candidate, before publication.
        if constexpr (std::same_as<T, Fragile>) {
            for (const auto& element : candidate)
                if (element.fail)
                    return std::unexpected(Error{ErrorCode::invocation_failed, "copy refused"});
        } else if (Swapper::fail)
            return std::unexpected(Error{ErrorCode::invocation_failed, "reorder refused"});
        array.swap(candidate);
        return {};
    };
    return definition;
}

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
        checked_array_definition<Fragile>(runtime, "arrays.FragileArray"),
        NativeRegistration<Swapper>{runtime, "arrays.Swapper", TypeKind::record}
            .field<&Swapper::value>("value")
            .build(),
        checked_array_definition<Swapper>(runtime, "arrays.SwapperArray"),
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
    take(runtime.move_element(id, swappers, 0, 1));
    require(order() == "22,11,", "Array edit did not recover after a rejected candidate.");
    std::puts("[pass] Array edits: explicit candidate failure rollback and recovery");
}
