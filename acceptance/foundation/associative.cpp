#include "foundation/map.hpp"

#include <memory>
#include <stdexcept>

namespace {
using namespace atelier;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct alignas(64) Resource {
    static inline int live{};

    static inline int copies_until_failure = -1;

    std::unique_ptr<int> payload;

    explicit Resource(int id) : payload(std::make_unique<int>(id)) { ++live; }

    Resource(const Resource& other) {
        if (copies_until_failure == 0)
            throw std::runtime_error("Resource snapshot rejected.");
        if (copies_until_failure > 0)
            --copies_until_failure;
        payload = std::make_unique<int>(*other.payload);
        ++live;
    }

    Resource(Resource&& other) noexcept : payload(std::move(other.payload)) { ++live; }

    Resource& operator=(Resource&&) noexcept = default;

    ~Resource() { --live; }
};
}

void verify_associative_containers() {
    using Catalog = Map<String, Resource>;
    {
        Catalog catalog;
        catalog.try_emplace(String::literal<"first">(), 7);
        catalog.try_emplace(String::literal<"second">(), 9);
        Catalog destination;
        destination.try_emplace(String::literal<"keep">(), 42);
        Resource::copies_until_failure = 1;
        bool rejected = false;
        try {
            destination = catalog;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        Resource::copies_until_failure = -1;
        require(rejected && destination.size() == 1 && *destination.find("keep")->payload == 42 && Resource::live == 3,
                "Failed catalog snapshot leaked partial resources or changed the destination.");
    }
    require(Resource::live == 0, "Catalog lifetime leaked resources.");
}
