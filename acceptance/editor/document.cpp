#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/assets.hpp"
#include "assets/file.hpp"
#include "platform/utf.hpp"
#include "foundation/frame_timing.hpp"
#include "physics/timing.hpp"
#include "editor/workspace.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "material/resources.hpp"
#include "geometry/mesh.hpp"
#include "runtime/native.hpp"
#include "application/diagnostics.hpp"
#include "application/scene_snapshot.hpp"
#include "renderer/scene_visibility.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <numeric>
#include <numbers>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>
#include <windows.h>

void verify_mesh_reimport();

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void changed(Result<world::Commit> result) {
    const auto commit = take(std::move(result));
    if (commit.notification_error)
        throw std::runtime_error(commit.notification_error->message);
}

void collect(editor::Session& document) {
    for (unsigned step = 0; step < 96; ++step)
        take(document.collect_step());
}

ObjectId component(editor::Session& document, ObjectId entity, TypeId type) {
    for (const auto& root : take(world::components(document.world().runtime(), entity)))
        if (take(document.world().runtime().type_of(root.id())) == type)
            return root.id();
    throw std::runtime_error("Expected component is absent.");
}

#include "gltf_import.inl"
#include "project_profile.inl"
}

int main(int argc, char** argv) {
    atelier::install_fatal_handler();
    std::string_view stage = argc > 1 ? argv[1] : "document history";
    try {
        if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "--profile-scene-cpu") {
            profile_scene_cpu(argv[2], argv[3], argv[4],
                              argc == 6 ? std::filesystem::path(argv[5]) : std::filesystem::path{});
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--profile-physics-cpu") {
            profile_physics_cpu(argv[2], argv[3], argv[4]);
            return 0;
        }
        require(argc == 1, "Expected no arguments, --profile-scene-cpu or --profile-physics-cpu.");
        editor::Session single;
        take(single.initialize());
        changed(single.create_static_mesh(assets::mesh_assets().front()));
        require(single.undo_count() == 1 && take(single.world().runtime().type_of(single.selection().entity)) ==
                                                single.presentation_types().static_mesh_entity,
                "Static mesh factory did not create one typed Entity history entry.");
        const auto entity = single.selection().entity;
        const auto mesh = component(single, entity, single.presentation_types().static_mesh_component);
        require(take(world::components(single.world().runtime(), entity)).size() == 1,
                "Mesh Entity acquired an extra component.");
        changed(single.undo());
        collect(single);
        require(take(single.world().members()).empty() && take(single.presentation().render_items()).empty(),
                "Creation undo retained the Entity or its rendering.");
        changed(single.redo());
        require(single.selection().entity == entity &&
                    component(single, entity, single.presentation_types().static_mesh_component) == mesh &&
                    take(single.presentation().render_items()).front().geometry == assets::mesh_assets().front().mesh,
                "Creation redo changed identity or mesh resource.");
        changed(single.close());
        stage = "glTF transform";
        verify_gltf_import();
        stage = "mesh reimport";
        verify_mesh_reimport();
        std::puts("Editor document acceptance: PASS (creation undo/redo, glTF transform, mesh reimport)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor document acceptance: FAIL [%.*s]: %s\n", static_cast<int>(stage.size()),
                     stage.data(), error.what());
        return 1;
    }
}
