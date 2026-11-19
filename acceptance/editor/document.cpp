#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "editor/session.hpp"
#include "editor/graph_format.hpp"
#include "editor/project.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/nodes.hpp"
#include "source/program.hpp"
#include "source/snapshot.hpp"
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

void verify_graph_format() {
    Runtime runtime;
    take(procedural::register_types(runtime));
    auto program = take(source::Program::create(runtime));
    {
        auto req =
            take(program->request({"graph_doc", 1, take(source::engine_sources(source::default_engine_root()))}));
        auto cand = take(source::Program::prepare(req));
        take(program->try_publish(cand));
    }
    const std::string_view input_json = R"({
  "nodes": {
    "grid": {
      "inputs": {},
      "position": [10.0, 20.0],
      "properties": {"columns": 3, "rows": 2, "size": {"x": 2.0, "y": 1.0}},
      "type": "procedural.Grid"
    },
    "jitter": {
      "inputs": {"input": "grid.output"},
      "position": [180.0, 20.0],
      "properties": {"amount": 0.25, "group": "", "seed": 4},
      "type": "procedural.Jitter"
    },
    "legacy": {
      "inputs": {"input": "grid.output"},
      "position": [180.0, 140.0],
      "properties": {"foo": 42},
      "type": "game.MissingSop"
    },
    "merged": {
      "inputs": {"inputs": ["jitter.output", "legacy.output"]},
      "position": [360.0, 60.0],
      "properties": {},
      "type": "procedural.Merge"
    },
    "noise": {
      "inputs": {"input": "merged.output"},
      "position": [540.0, 60.0],
      "properties": {"attribute": "nval", "frequency": 2.0, "layer": "point"},
      "type": "procedural.Noise"
    }
  },
  "settings": {"display": "noise", "seed": 9},
  "version": 1
})";
    const auto doc = take(editor::load_graph(runtime, input_json));
    require(doc.nodes.size() == 5 && doc.issues.size() == 2 &&
                doc.issues[0].kind == world::DiagnosticKind::unknown_type &&
                doc.issues[1].kind == world::DiagnosticKind::reference_dropped,
            "load_graph did not tolerantly retain missing SOP type and dropped multi-port connection.");
    const auto saved_once = take(editor::save_graph(runtime, doc));
    const auto reloaded = take(editor::load_graph(runtime, saved_once));
    const auto saved_twice = take(editor::save_graph(runtime, reloaded));
    require(saved_once == saved_twice, "save_graph -> load_graph -> save_graph was not byte-identical.");
    struct NoMeshes final : procedural::Resources {
        Result<geometry::MeshRef> mesh(StringView) const noexcept override {
            return std::unexpected(Error{ErrorCode::invalid_member, "Unused"});
        }

        std::uint64_t revision() const noexcept override { return 1; }
    } resources;
    procedural::Evaluator evaluator{runtime, reloaded.graph, resources, program.get()};
    take(evaluator.evaluate());
    const auto* out = evaluator.output();
    require(out != nullptr && out->count(procedural::Layer::point) == 6 &&
                take(out->read<float>("nval", procedural::Layer::point)) != nullptr,
            "Reloaded C++/Source procedural graph did not evaluate to 6 noisy points.");
}
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
        stage = "procedural graph format";
        verify_graph_format();
        std::puts("Editor document acceptance: PASS (creation undo/redo, glTF transform, mesh reimport, graph format)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor document acceptance: FAIL [%.*s]: %s\n", static_cast<int>(stage.size()),
                     stage.data(), error.what());
        return 1;
    }
}
