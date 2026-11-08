// Current v1 bytes come from an authored fixture, never from the encoder under test.
class RejectedNode : public procedural::Node {
public:
    void evaluate(procedural::Geometry&) const {}
};

void verify_procedural_graph() {
    using namespace procedural;
    const auto& authored = json::Document::literal<R"({
        "version": 1,
        "settings": {"seed": 7, "display": "moved"},
        "nodes": {
            "grid": {
                "type": "procedural.Grid",
                "position": [0.0, 120.0],
                "properties": {"size": {"x": 3.0, "y": 2.0}, "rows": 3, "columns": 4},
                "inputs": {}
            },
            "moved": {
                "type": "procedural.Transform",
                "position": [100.5, -40.25],
                "properties": {
                    "translate": {"x": 1.0, "y": 2.0, "z": 3.0},
                    "rotate": {"x": 0.0, "y": 0.70710677, "z": 0.0, "w": 0.70710677},
                    "scale": {"x": 2.0, "y": 1.0, "z": 1.0}
                },
                "inputs": {"input": "grid.output"}
            },
            "box": {
                "type": "procedural.Box", "position": [200.0, 0.0],
                "properties": {"size": {"x": 2.0, "y": 4.0, "z": 6.0}}, "inputs": {}
            },
            "sphere": {
                "type": "procedural.Sphere", "position": [200.0, 100.0],
                "properties": {"radius": 2.0, "segments": 4, "latitudes": 3}, "inputs": {}
            },
            "tube": {
                "type": "procedural.Tube", "position": [200.0, 200.0],
                "properties": {"radius": 1.5, "height": 3.0, "segments": 5}, "inputs": {}
            },
            "volume": {
                "type": "procedural.VolumeScatter", "position": [300.0, 0.0],
                "properties": {"density": 0.25, "seed": 19, "origin": {"x": 0.5, "y": -2.0, "z": 3.0},
                               "cell_size": 2.0}, "inputs": {"surface": "box.output"}
            },
            "line": {
                "type": "procedural.Line", "position": [100.0, 300.0],
                "properties": {"start": {"x": 0.0, "y": 0.0, "z": 0.0},
                               "end": {"x": 4.0, "y": 2.0, "z": 6.0}, "count": 3}, "inputs": {}
            },
            "path": {
                "type": "procedural.Polyline", "position": [200.0, 300.0],
                "properties": {}, "inputs": {"points": "line.output"}
            },
            "selected": {
                "type": "procedural.GroupBox", "position": [300.0, 300.0],
                "properties": {"group": "chosen", "minimum": {"x": 0.0, "y": -1.0, "z": 2.0},
                               "maximum": {"x": 2.0, "y": 1.0, "z": 4.0}},
                "inputs": {"input": "path.output"}
            },
            "noise": {
                "type": "procedural.Noise", "position": [350.0, 300.0],
                "properties": {"source_attribute": "rest", "attribute": "density", "group": "chosen",
                               "seed": 23, "frequency": -0.75, "offset": {"x": 0.5, "y": -1.25, "z": 2.5}},
                "inputs": {"input": "selected.output"}
            },
            "remap": {
                "type": "procedural.Remap", "position": [375.0, 300.0],
                "properties": {"source_attribute": "density", "attribute": "size", "group": "chosen",
                               "input_min": -1.0, "input_max": 2.0, "output_min": 4.0, "output_max": -2.0},
                "inputs": {"input": "noise.output"}
            },
            "realized": {
                "type": "procedural.Realize", "position": [400.0, 300.0],
                "properties": {"group": "chosen", "variant": "prototype", "target_attributes": ["id", "weight"]},
                "inputs": {"sources": ["tube.output", "sphere.output"], "points": "remap.output"}
            }
        }
    })">();
    const auto text_bytes = json::write(authored.root(), {true, true});
    const std::string text{text_bytes.data(), text_bytes.size()};
    Runtime runtime;
    take(register_types(runtime));
    auto program = take(source::Program::create(runtime));
    auto candidate = take(source::Program::prepare(take(program->request(
        source::SourceSnapshot{"procedural-graph", 1, take(source::engine_sources(source::default_engine_root()))}))));
    take(program->try_publish(candidate));
    auto document = take(editor::load_graph(runtime, text));
    const auto entry = [&](const char* name) -> const editor::GraphNode& {
        const auto found = std::ranges::find(document.nodes, std::string{name}, &editor::GraphNode::name);
        require(found != document.nodes.end() && found->node.registered(), "An authored node did not load.");
        return *found;
    };
    const auto& grid = entry("grid");
    const auto& moved = entry("moved");
    require(document.issues.empty() && grid.position == std::array<float, 2>{0, 120} &&
                moved.position == std::array<float, 2>{100.5f, -40.25f},
            "An authored graph lost node positions.");
    require(take(runtime.with_native<const Grid>(grid.node.id(),
                                                 [](const Grid& value) -> Result<bool> {
                                                     return value.size == math::Vector2{3, 2} && value.rows == 3 &&
                                                            value.columns == 4;
                                                 })),
            "Authored Grid parameters did not load.");
    require(take(runtime.with_native<const Transform>(moved.node.id(),
                                                      [](const Transform& value) -> Result<bool> {
                                                          return value.translate == math::Vector3{1, 2, 3} &&
                                                                 value.scale == math::Vector3{2, 1, 1} &&
                                                                 value.rotate ==
                                                                     math::Quaternion{0, .70710677f, 0, .70710677f};
                                                      })),
            "Authored Transform parameters did not load.");
    require(take(runtime.with_native<const Graph>(document.graph.id(),
                                                  [](const Graph& value) -> Result<bool> { return value.seed == 7; })),
            "The authored graph seed did not load.");
    require(take(runtime.with_native<const Realize>(
                entry("realized").node.id(),
                [](const Realize& value) -> Result<bool> {
                    return value.group.view() == "chosen" && value.variant.view() == "prototype" &&
                           std::ranges::equal(value.target_attributes,
                                              Array<String>{String::literal<"id">(), String::literal<"weight">()});
                })),
            "Authored Realize selectors or broadcast names did not load.");
    for (const auto* node : {"noise", "remap"}) {
        const auto id = entry(node).node.id();
        const auto owner = take(runtime.type_of(id));
        for (const auto* name : {"source_attribute", "attribute", "group"}) {
            String value;
            take(runtime.read(id, take(runtime.find_field(owner, name)),
                              ValueOutput{take(runtime.native_type<String>()), &value}));
            require(authored.root().string({"nodes", node, "properties", name}) == value.view().std_view(),
                    "An authored Source selector did not reach its String field.");
        }
    }
    const auto scalar = [&](const char* node, const char* name) {
        const auto id = entry(node).node.id();
        return take(runtime.read(id, take(runtime.find_field(take(runtime.type_of(id)), name))));
    };
    math::Vector3 offset;
    take(runtime.read(entry("noise").node.id(),
                      take(runtime.find_field(take(runtime.find_type("procedural.Noise")), "offset")),
                      ValueOutput{take(runtime.native_type<math::Vector3>()), &offset}));
    require(
        std::get<std::uint32_t>(scalar("noise", "seed")) == 23 &&
            std::get<float>(scalar("noise", "frequency")) == -.75f && offset == math::Vector3{.5f, -1.25f, 2.5f} &&
            std::get<float>(scalar("remap", "input_min")) == -1 && std::get<float>(scalar("remap", "input_max")) == 2 &&
            std::get<float>(scalar("remap", "output_min")) == 4 && std::get<float>(scalar("remap", "output_max")) == -2,
        "Authored Noise/Remap numeric parameters did not load.");
    const auto graph_contents = take(contents(runtime, document.graph.id()));
    require(graph_contents.display() == moved.node.id() &&
                take(inputs(runtime, graph_contents, moved.node.id())) == std::vector<Input>{{grid.node.id(), 0, 0}} &&
                take(inputs(runtime, graph_contents, entry("path").node.id())) ==
                    std::vector<Input>{{entry("line").node.id(), 0, 0}} &&
                take(inputs(runtime, graph_contents, entry("volume").node.id())) ==
                    std::vector<Input>{{entry("box").node.id(), 0, 0}} &&
                take(inputs(runtime, graph_contents, entry("noise").node.id())) ==
                    std::vector<Input>{{entry("selected").node.id(), 0, 0}} &&
                take(inputs(runtime, graph_contents, entry("remap").node.id())) ==
                    std::vector<Input>{{entry("noise").node.id(), 0, 0}} &&
                take(inputs(runtime, graph_contents, entry("realized").node.id())) ==
                    std::vector<Input>{{entry("tube").node.id(), 0, 0},
                                       {entry("sphere").node.id(), 0, 0},
                                       {entry("remap").node.id(), 0, 1}} &&
                take(editor::save_graph(runtime, document)) == text,
            "The authored display, edge or canonical v1 bytes changed.");

    // The only tolerant fixture: a missing type stays null and keeps all of its frozen JSON.
    json::Builder missing{authored};
    require(missing
                .set(*missing.root().at("nodes"), "mystery",
                     json::Document::literal<R"({"type":"game.Missing","position":[10.5,20.0],
                            "properties":{"x":1},"inputs":{"source":"grid.output"}})">()
                         .root())
                .has_value(),
            "Missing-node fixture construction failed.");
    const auto missing_bytes = json::write(missing.root(), {true, true});
    const std::string missing_text{missing_bytes.data(), missing_bytes.size()};
    auto tolerant = take(editor::load_graph(runtime, missing_text));
    const auto mystery = std::ranges::find(tolerant.nodes, std::string{"mystery"}, &editor::GraphNode::name);
    require(mystery != tolerant.nodes.end() && !mystery->node.registered() &&
                mystery->position == std::array<float, 2>{10.5f, 20} &&
                take(editor::save_graph(runtime, tolerant)) == missing_text,
            "A missing node was loaded or lost frozen data on save.");

    // A real constructor failure must not turn into a successfully loaded null node.
    auto rejected = NativeRegistration<RejectedNode>{runtime, "graphAcceptance.Rejected", TypeKind::object}
                        .base<Node>()
                        .method<&RejectedNode::evaluate>("evaluate")
                        .parameter_names({"output"})
                        .build();
    rejected.native.construct = [](void*) noexcept -> Result<void> {
        return std::unexpected(Error{ErrorCode::construction_failed, "This node cannot be constructed."});
    };
    take(runtime.register_type(std::move(rejected)));
    json::Builder invalid{authored};
    const auto invalid_grid = invalid.root().path({"nodes", "grid"});
    require(invalid_grid && invalid.set(*invalid_grid, "type", "graphAcceptance.Rejected") &&
                invalid.set(*invalid_grid, "properties", json::Document::literal<"{}">().root()),
            "Rejected-node fixture construction failed.");
    const auto invalid_bytes = json::write(invalid.root());
    const auto failed = editor::load_graph(runtime, {invalid_bytes.data(), invalid_bytes.size()});
    require(!failed && failed.error().code == ErrorCode::construction_failed,
            "A node that failed to construct was kept as an unknown type.");
    std::puts("Procedural graph files: PASS (authored v1 bytes, missing-type frozen roundtrip, constructor failure)");
}
