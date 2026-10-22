#include "physics/physics.hpp"
#include "physics/timing.hpp"
#include "presentation/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <limits>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class T> T take(atelier::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(atelier::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
void committed(atelier::Result<atelier::world::Commit> result) {
    const auto value = take(std::move(result));
    if (value.notification_error) throw std::runtime_error(value.notification_error->message);
}

atelier::ObjectId mesh_component(atelier::Runtime& runtime,atelier::ObjectId entity) {
    const auto type=take(runtime.native_type<atelier::presentation::StaticMeshComponent>());
    for(const auto& component:take(atelier::world::components(runtime,entity)))
        if(take(runtime.is_a(take(runtime.type_of(component.id())),type))) return component.id();
    throw std::runtime_error("Expected a mesh component on the Entity.");
}
// World::update is the only frame entry; it steps the registered PhysicsScene.
atelier::physics::StepResult step(atelier::world::World& world, const atelier::physics::PhysicsScene& scene, float elapsed) {
    take(world.update(elapsed));
    return scene.last_step();
}

#include "integration.inl"
#include "scenarios.inl"
#include "components.inl"
#include "scene_queries.inl"
#include "primitives.inl"

struct Scenario { std::string_view name; void (*run)(); };
const Scenario scenarios[]{
    {"world_mesh", simulate_world},
    {"rigid_shapes", rigid_shapes},
    {"forces", forces},
    {"contacts", simulate_filter_and_trigger},
    {"collision_matrix", collision_matrix},
    {"queries", queries},
    {"cooked_assets", simulate_cooked_assets},
    {"fixed_step", fixed_step},
    {"frame_timing", frame_timing},
    {"lifecycle", lifecycle},
    {"world_ownership", world_ownership},
    {"character", character},
    {"constraint", constraint},
    {"ragdoll", ragdoll},
    {"vehicle", vehicle},
    {"special_integration", simulate_special_systems},
    {"components", component_lifecycle},
    {"entity_bindings", entity_physics_bindings},
    {"trace_queries", trace_queries},
    {"query_components", query_components},
    {"query_exclusions", query_exclusions},
    {"primitive_composition", primitive_composition},
    {"primitive_authoring", primitive_authoring},
    {"mirrored_scale", mirrored_scale},
    {"pose_publication", pose_publication},
    {"solver_determinism", solver_determinism},
    {"contact_capacity", contact_capacity},
};
} // namespace

int main(int argc, char** argv) {
    std::string_view selected;
    if (argc == 2 && std::string_view(argv[1]) == "--list") {
        for (const auto& scenario : scenarios) std::printf("%.*s\n", int(scenario.name.size()), scenario.name.data());
        return EXIT_SUCCESS;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--scenario") selected = argv[2];
    else if (argc != 1) {
        std::fputs("Usage: atelier_physics_acceptance [--list | --scenario NAME]\n", stderr);
        return EXIT_FAILURE;
    }
    if (argc == 3 && std::ranges::none_of(scenarios, [&](const auto& s) { return s.name == selected; })) {
        std::fputs("Unknown physics scenario; use --list.\n", stderr);
        return EXIT_FAILURE;
    }
    unsigned passed = 0, failed = 0;
    for (const auto& scenario : scenarios) {
        if (!selected.empty() && selected != scenario.name) continue;
        std::printf("Physics scenario: %.*s RUN\n", int(scenario.name.size()), scenario.name.data());
        std::fflush(stdout);
        try {
            scenario.run();
            ++passed;
            std::printf("Physics scenario: %.*s PASS\n", int(scenario.name.size()), scenario.name.data());
        } catch (const std::exception& error) {
            ++failed;
            std::fprintf(stderr, "Physics scenario: %.*s FAIL (%s)\n",
                int(scenario.name.size()), scenario.name.data(), error.what());
        }
    }
    std::printf("Physics acceptance: %s (%u/%u scenarios)\n", failed ? "FAIL" : "PASS", passed, passed + failed);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
