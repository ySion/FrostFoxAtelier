#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/shared_array.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using namespace atelier;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Diagnose the lifetime violation before accessing the destroyed nested source.
void require_detached(bool watched, bool empty) noexcept {
    if (watched && !empty) {
        std::fputs("Owned move source was destroyed before its storage was transferred.\n", stderr);
        std::exit(EXIT_FAILURE);
    }
}

struct ArrayOwner {
    Array<std::shared_ptr<ArrayOwner>> children;

    bool watch{};

    ~ArrayOwner() { require_detached(watch, children.empty()); }
};

void owned_source_moves() {
    Array<std::shared_ptr<ArrayOwner>> array;
    array.add(std::make_shared<ArrayOwner>());
    array[0]->children.add(std::make_shared<ArrayOwner>());
    auto* array_leaf = array[0]->children[0].get();
    array[0]->watch = true;
    array = std::move(array[0]->children);
    require(array.size() == 1 && array[0].get() == array_leaf, "Array subtree promotion lost its child.");
}

struct alignas(32) OptionalResource {
    std::unique_ptr<int> value;

    explicit OptionalResource(std::unique_ptr<int> input) noexcept : value(std::move(input)) {}
};

static_assert(sizeof(Optional<std::uint32_t>) == 8 && alignof(Optional<std::uint32_t>) == 4);

static_assert(sizeof(Optional<OptionalResource>) == 64 && alignof(Optional<OptionalResource>) == 32);

static_assert(!std::is_copy_constructible_v<Optional<OptionalResource>>);

static_assert(std::is_nothrow_move_constructible_v<Optional<OptionalResource>>);

void optional_resources() {
    Optional<OptionalResource> source;
    require(!source && !source.try_get(), "Empty Optional exposed a payload.");
    source.emplace(std::make_unique<int>(37));
    Optional<OptionalResource> moved{std::move(source)};
    require(source.has_value() && !source.try_get()->value, "Optional move reset its source engagement.");
    require(moved && *moved.try_get()->value == 37, "Optional move lost its resource.");
    source.reset();
    source.swap(moved);
    require(source && *source.try_get()->value == 37 && !moved, "Optional swap failed to transfer engagement.");
    moved.emplace(std::make_unique<int>(19));
    source.swap(moved);
    require(*source.try_get()->value == 19 && *moved.try_get()->value == 37, "Engaged Optional swap lost values.");
    source = std::move(moved);
    require(*source.try_get()->value == 37 && moved && !moved.try_get()->value,
            "Optional move assignment changed source engagement or lost ownership.");
    moved.reset();
    source = std::move(moved);
    require(!source && !moved, "Assigning empty Optional retained a resource.");

    auto owner = std::make_shared<int>(53);
    Optional<std::shared_ptr<int>> shared{owner};
    auto copy = shared;
    require(owner.use_count() == 3 && **copy.try_get() == 53, "Optional copy did not retain its resource.");
    shared.reset();
    copy = shared;
    require(!copy && owner.use_count() == 1, "Optional reset or assignment leaked a resource.");
}

void shared_arrays() {
    Array<std::uint32_t> numbers;
    numbers.resize(8);
    for (std::uint32_t i = 0; i != numbers.size(); ++i)
        numbers[i] = i;
    SharedArray<std::uint32_t> shared{std::move(numbers)};
    const auto* original = shared.view().data();
    std::atomic<bool> failed{};
    {
        std::array<std::jthread, 8> workers;
        for (std::uint32_t worker = 0; worker != workers.size(); ++worker)
            workers[worker] = std::jthread{[&, worker] {
                SharedArray<std::uint32_t> copy = shared;
                if (copy.view()[worker] != worker)
                    failed = true;
                auto& own = copy.write();
                if (own.data() == original || own[worker] != worker)
                    failed = true;
                own[worker] = ~0u;
            }};
    }
    require(!failed, "Concurrent copies or writes broke sharing.");
    require(shared.write().data() == original, "Concurrent copies left the shared block counted as shared.");
    for (std::uint32_t i = 0; i != shared.size(); ++i)
        require(shared.array()[i] == i, "A worker's write reached the shared block.");
}
}

void verify_container_lifecycle() {
    owned_source_moves();
    optional_resources();
    shared_arrays();
}
