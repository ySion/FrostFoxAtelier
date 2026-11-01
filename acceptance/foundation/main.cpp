#include "../helpers.hpp"
#include "foundation/fixture.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

void verify_associative_containers();

void verify_container_lifecycle();

void verify_frame_timing();

namespace {
using namespace atelier;

using namespace atelier::acceptance::foundation;

using namespace atelier::acceptance::helpers;

struct alignas(64) Resource {
    static inline int live{};

    static inline int copies_until_failure = -1;

    std::unique_ptr<int> value;

    explicit Resource(int number = 0) : value(std::make_unique<int>(number)) { ++live; }

    Resource(const Resource& other) {
        if (copies_until_failure == 0)
            throw std::runtime_error("Resource copy rejected.");
        if (copies_until_failure > 0)
            --copies_until_failure;
        value = std::make_unique<int>(*other.value);
        ++live;
    }

    Resource(Resource&& other) noexcept(false) : value(std::move(other.value)) { ++live; }

    Resource& operator=(const Resource& other) {
        *value = *other.value;
        return *this;
    }

    Resource& operator=(Resource&& other) noexcept {
        value = std::move(other.value);
        return *this;
    }

    ~Resource() { --live; }
};

void native_workload() {
    String cache{"abcdefghijklmno"};
    cache.append(cache.view().slice(1));
    require(cache.view() == "abcdefghijklmnobcdefghijklmno", "Inline-to-heap append invalidated its self-view.");

    Array<String> filled{String{"a label longer than the inline capacity"}};
    filled.squeeze();
    filled.resize(2, filled[0]);
    for (const auto& label : filled)
        require(label.view() == "a label longer than the inline capacity", "Filled growth lost its aliased value.");

    {
        Array<Resource> resources;
        resources.emplace(7);
        resources.emplace(9);
        resources.squeeze();
        Resource::copies_until_failure = 1;
        bool failed = false;
        try {
            resources.reserve(3);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed, "Expected resource failure.");
        require(resources.size() == 2 && *resources[0].value == 7 && Resource::live == 2,
                "Failed growth leaked or damaged old resources.");
        Resource::copies_until_failure = 2;
        failed = false;
        try {
            resources.append(resources.view());
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed, "Expected prefix relocation failure.");
        require(resources.size() == 2 && Resource::live == 2 && *resources[1].value == 9,
                "Failed self-append left a live suffix.");
        Resource::copies_until_failure = -1;
        {
            Array<Resource> destination;
            destination.emplace(100);
            Resource::copies_until_failure = 1;
            failed = false;
            try {
                destination = resources;
            } catch (const std::runtime_error&) {
                failed = true;
            }
            require(failed && destination.size() == 1 && *destination[0].value == 100 && Resource::live == 3,
                    "Failed resource assignment changed target or leaked its copy.");
            Resource::copies_until_failure = -1;
        }
        Resource::copies_until_failure = 2;
        failed = false;
        try {
            resources.resize(5, resources[1]);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        Resource::copies_until_failure = -1;
        require(failed && resources.size() == 2 && *resources[1].value == 9 && Resource::live == 2,
                "Failed filled resize left a partial suffix.");
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
        value.items.emplace(Item{String{"shield"}, 2, Quality::rare});
        value.items.emplace(Item{String{"asset:/weapons/steel_sword.asset"}, 1, Quality::common});
        return {};
    }));
    const std::array native_path{take(runtime.find_field(types[0], "items"))};
    auto saved = take(runtime.with_native<const Array<Item>>(
        native.id(), native_path, [](const Array<Item>& value) -> Result<Array<Item>> { return value; }));
    const std::array path{take(runtime.find_field(described_type, "items"))};
    take(runtime.with_native<Array<Item>>(data.id(), path, [&](Array<Item>& value) -> Result<void> {
        value = saved;
        value[0].name.append("+1");
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
    try {
        native_workload();
        reflection_workload();
        verify_associative_containers();
        verify_container_lifecycle();
        verify_frame_timing();
        std::puts("Foundation acceptance: PASS (owning aliases, failure cleanup, shared lifetime, GC, frame timing)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Foundation acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}
