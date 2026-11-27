#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "physics/physics.hpp"
#include "presentation/presentation.hpp"
#include "world/detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

namespace {

using namespace atelier::acceptance::helpers;

void committed(atelier::Result<atelier::world::Commit> result) {
    const auto value = take(std::move(result));
    if (value.notification_error)
        atelier::fatal(value.notification_error->message);
}

atelier::ObjectId mesh_component(atelier::Runtime& runtime, atelier::ObjectId entity) {
    const auto type = take(runtime.native_type<atelier::presentation::StaticMeshComponent>());
    for (const auto& component : take(atelier::world::components(runtime, entity)))
        if (take(runtime.is_a(take(runtime.type_of(component.id())), type)))
            return component.id();
    atelier::fatal("Expected a mesh component on the Entity.");
}

#include "integration.inl"
#include "scenarios.inl"

struct Scenario {
    std::string_view name;

    void (*run)();
};

const Scenario scenarios[]{
    {"rigid_shapes", rigid_shapes},
    {"cooked_assets", simulate_cooked_assets},
    {"failed_lifecycle", failed_lifecycle},
};
} // namespace

int main(int argc, char** argv) {
    atelier::install_fatal_handler();
    std::string_view selected;
    if (argc == 2 && std::string_view(argv[1]) == "--list") {
        for (const auto& scenario : scenarios)
            std::printf("%.*s\n", int(scenario.name.size()), scenario.name.data());
        return EXIT_SUCCESS;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--scenario")
        selected = argv[2];
    else
        require(argc == 1, "Usage: atelier_physics_acceptance [--list | --scenario NAME]");
    require(argc != 3 || std::ranges::any_of(scenarios, [&](const auto& s) { return s.name == selected; }),
            "Unknown physics scenario; use --list.");
    unsigned passed = 0;
    for (const auto& scenario : scenarios) {
        if (!selected.empty() && selected != scenario.name)
            continue;
        std::printf("Physics scenario: %.*s RUN\n", int(scenario.name.size()), scenario.name.data());
        std::fflush(stdout);
        scenario.run();
        ++passed;
        std::printf("Physics scenario: %.*s PASS\n", int(scenario.name.size()), scenario.name.data());
    }
    std::printf("Physics acceptance: PASS (%u/%u scenarios)\n", passed, passed);
    return EXIT_SUCCESS;
}
