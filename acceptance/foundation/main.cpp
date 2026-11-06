#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "foundation/fixture.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

void verify_associative_containers();

void verify_container_lifecycle();

void verify_frame_timing();

void verify_tasks();

namespace {
using namespace atelier;

using namespace atelier::acceptance::foundation;

using namespace atelier::acceptance::helpers;

struct alignas(64) Resource {
    static inline int live{};

    static inline unsigned copies{};

    static inline unsigned moves{};

    static inline unsigned destroyed{};

    std::unique_ptr<int> value;

    explicit Resource(int number = 0) noexcept : value(std::make_unique<int>(number)) { ++live; }

    Resource(const Resource& other) noexcept : value(std::make_unique<int>(*other.value)) {
        ++copies;
        ++live;
    }

    Resource(Resource&& other) noexcept : value(std::move(other.value)) {
        ++moves;
        ++live;
    }

    Resource& operator=(const Resource& other) noexcept {
        value = std::make_unique<int>(*other.value);
        return *this;
    }

    Resource& operator=(Resource&& other) noexcept {
        value = std::move(other.value);
        return *this;
    }

    ~Resource() {
        ++destroyed;
        --live;
    }
};

void native_workload() {
    auto cache = String::from_utf8("abcdefghijklmnopqrst");
    require(cache.has_value(), "Valid UTF-8 was rejected.");
    require(cache->append(cache->view().slice(1)).has_value(), "Valid self-view was rejected.");
    require(cache->view() == "abcdefghijklmnopqrstbcdefghijklmnopqrst",
            "Inline-to-heap append invalidated its self-view.");

    Array<String> filled{String::literal<"a label longer than the inline capacity">()};
    filled.squeeze();
    filled.resize(2, filled[0]);
    for (const auto& label : filled)
        require(label.view() == "a label longer than the inline capacity", "Filled growth lost its aliased value.");

    {
        Array<Resource> resources;
        resources.emplace(7);
        resources.emplace(9);
        resources.squeeze();
        const auto moves = Resource::moves;
        const auto destroyed = Resource::destroyed;
        resources.reserve(3);
        require(Resource::moves == moves + 2 && Resource::destroyed == destroyed + 2 && Resource::live == 2,
                "Array relocation bypassed resource construction or destruction.");
        const auto copies = Resource::copies;
        resources.append(resources.view());
        require(resources.size() == 4 && Resource::copies == copies + 2 && Resource::live == 4 &&
                    *resources[2].value == 7 && *resources[3].value == 9 &&
                    resources[0].value.get() != resources[2].value.get(),
                "Array self-append did not independently own the copied resources.");
        {
            Array<Resource> destination;
            destination.emplace(100);
            destination = resources;
            require(destination.size() == 4 && *destination[0].value == 7 && Resource::live == 8,
                    "Resource assignment retained the old target or lost its copy.");
        }
        resources.squeeze();
        resources.resize(5, resources[1]);
        require(resources.size() == 5 && *resources[4].value == 9 && Resource::live == 5,
                "Filled resize lost its aliased resource.");
    }
    require(Resource::live == 0, "Array leaked resource elements.");
}

void reflection_workload() {
    Runtime runtime;
    const auto types = take(runtime.register_types(
        {reflection::Definition<Inventory>::build(runtime), array_definition<Item>(runtime, "foundation.Items"),
         reflection::Definition<Item>::build(runtime), reflection::Definition<Quality>::build(runtime),
         string_definition(runtime)}));
    const auto array_type = take(runtime.native_type<Array<Item>>());
    take(runtime.register_types({NativeRegistration<Resource>{runtime, "foundation.Resource", TypeKind::record}.build(),
                                 array_definition<Resource>(runtime, "foundation.Resources")}));
    TypeDefinition described;
    described.name = "foundation.DescribedInventory";
    described.fields = {{"items", {array_type}}, {"resources", {take(runtime.native_type<Array<Resource>>())}}};
    const auto described_type = take(runtime.register_type(std::move(described)));

    auto native = take(runtime.create(types[0]));
    auto data = take(runtime.create(described_type));
    take(runtime.with_native<Inventory>(native.id(), [&](Inventory& value) -> Result<void> {
        value.items.emplace(Item{String::literal<"shield">(), 2, Quality::rare});
        value.items.emplace(Item{String::literal<"asset:/weapons/steel_sword.asset">(), 1, Quality::common});
        return {};
    }));
    const std::array native_path{take(runtime.find_field(types[0], "items"))};
    auto saved = take(runtime.with_native<const Array<Item>>(
        native.id(), native_path, [](const Array<Item>& value) -> Result<Array<Item>> { return value; }));
    const std::array path{take(runtime.find_field(described_type, "items"))};
    take(runtime.with_native<Array<Item>>(data.id(), path, [&](Array<Item>& value) -> Result<void> {
        value = saved;
        value[0].name.append(String::literal<"+1">());
        return {};
    }));
    const std::array resource_path{take(runtime.find_field(described_type, "resources"))};
    take(runtime.with_native<Array<Resource>>(data.id(), resource_path, [](Array<Resource>& resources) -> Result<void> {
        resources.emplace(123);
        resources.emplace(456);
        return {};
    }));
    require(Resource::live == 2, "Described object did not own its native resources.");
    take(runtime.destroy(native.id()));
    data.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        (void)take(runtime.collect_step({1}));
    require(Resource::live == 0, "GC did not destroy the value array and its resources.");
    require(saved[0].name.view() == "shield" && saved[1].name.view() == "asset:/weapons/steel_sword.asset",
            "Owned inline/heap snapshot depended on destroyed runtime object.");
}
}

int main() {
    atelier::install_fatal_handler();
    try {
        native_workload();
        reflection_workload();
        verify_associative_containers();
        verify_container_lifecycle();
        verify_frame_timing();
        verify_tasks();
        std::puts(
            "Foundation acceptance: PASS (owning aliases, failure cleanup, shared lifetime, GC, frame timing, tasks)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Foundation acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}
