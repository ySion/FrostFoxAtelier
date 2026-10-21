#include "editor/operator_parameters.hpp"
#include <vector>

namespace atelier::editor {
Result<void> register_editor_types(Runtime& runtime) {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(reflection::Definition<TransformTool>::build(runtime));
    definitions.push_back(reflection::Definition<PivotMode>::build(runtime));
    definitions.push_back(reflection::Definition<SelectMode>::build(runtime));
    definitions.push_back(reflection::Definition<Orientation>::build(runtime));
    definitions.push_back(reflection::Definition<CameraView>::build(runtime));
    definitions.push_back(reflection::Definition<AddKind>::build(runtime));
    definitions.push_back(reflection::Definition<ToolSettings>::build(runtime));
    definitions.push_back(reflection::Definition<SceneAdd>::build(runtime));
    definitions.push_back(reflection::Definition<ViewAxis>::build(runtime));
    if (auto registered = runtime.register_types(std::move(definitions)); !registered) return std::unexpected(registered.error());
    return {};
}
}
