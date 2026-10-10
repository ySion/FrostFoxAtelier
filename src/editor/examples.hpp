#pragma once
#include "editor/commands.hpp"
#include "editor/project.hpp"
#include "math/math.hpp"

namespace atelier::editor {
struct ExampleDefinition {
    Command command;
    const char* scene;
    math::Vector3 eye, target;
};
inline constexpr std::array official_examples{
    ExampleDefinition{Command::example_sponza,"Examples/Sponza/Scenes/Sponza.scene",{8,2,0},{-6,3,0}},
    ExampleDefinition{Command::example_scene_lit,"Examples/Scene Lit Room/Scenes/Main.scene",{9,6,-12},{0,1.5f,0}},
    ExampleDefinition{Command::example_physic,"Examples/Physic/Scenes/Overview.scene",{22,18,-26},{0,2,0}}
};
constexpr const ExampleDefinition* example_definition(Command command) {
    for(const auto& example:official_examples) if(example.command==command) return &example;
    return nullptr;
}
// Bundled templates are immutable. Initialize an editable copy once, retaining
// saved changes on later opens. An explicit workspace root supports isolated hosts.
Result<std::filesystem::path> prepare_examples(const std::filesystem::path& workspace_root = {});
}
