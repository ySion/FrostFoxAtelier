#include "foundation/array.hpp"
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
    shared_arrays();
}
