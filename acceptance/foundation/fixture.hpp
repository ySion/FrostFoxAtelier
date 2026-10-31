#pragma once

#include "runtime/foundation.hpp"
#include "runtime/reflection.hpp"
#include "foundation/fixture.gen.hpp"

namespace atelier::acceptance::foundation {

AENUM(Name = "foundation.Quality")
enum class Quality : int { common, rare };

ASTRUCT(Name = "foundation.Item")
struct Item {
    AGENERATED_BODY(Item)
    AFIELD()
    String name;
    AFIELD()
    std::int32_t count{};
    AFIELD()
    Quality quality{};
};

ACLASS(Name = "foundation.Inventory")
struct Inventory {
    AGENERATED_BODY(Inventory)
    AFIELD()
    String label;
    AFIELD()
    Array<Item> items;
    AFIELD()
    Array<String> tags;
    AFIELD()
    Array<Array<std::int32_t>> batches;
    AFIELD(Strong = "core.Object")
    ReferenceSlot owner;
};

}
