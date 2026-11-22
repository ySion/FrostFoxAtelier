#include "editor/operator_parameters.hpp"
#include "editor/view_options.hpp"
#include <vector>

namespace atelier::editor {
Result<void> register_editor_types(Runtime& runtime) {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(reflection::Definition<TransformTool>::build(runtime));
    definitions.push_back(reflection::Definition<PivotMode>::build(runtime));
    definitions.push_back(reflection::Definition<SelectMode>::build(runtime));
    definitions.push_back(reflection::Definition<SelectAll>::build(runtime));
    definitions.push_back(reflection::Definition<TransformChannel>::build(runtime));
    definitions.push_back(reflection::Definition<SnapTarget>::build(runtime));
    definitions.push_back(reflection::Definition<DeleteTarget>::build(runtime));
    definitions.push_back(reflection::Definition<SceneDelete>::build(runtime));
    definitions.push_back(reflection::Definition<SceneDuplicate>::build(runtime));
    definitions.push_back(reflection::Definition<SceneAddComponent>::build(runtime));
    definitions.push_back(reflection::Definition<SceneTransform>::build(runtime));
    definitions.push_back(reflection::Definition<SceneRename>::build(runtime));
    definitions.push_back(reflection::Definition<SceneSelectAll>::build(runtime));
    definitions.push_back(reflection::Definition<SceneTransformClear>::build(runtime));
    definitions.push_back(reflection::Definition<SceneSnap>::build(runtime));
    definitions.push_back(reflection::Definition<Orientation>::build(runtime));
    definitions.push_back(reflection::Definition<CameraView>::build(runtime));
    definitions.push_back(reflection::Definition<AddKind>::build(runtime));
    definitions.push_back(reflection::Definition<RegionKind>::build(runtime));
    definitions.push_back(reflection::Definition<LayoutShow>::build(runtime));
    definitions.push_back(reflection::Definition<LayoutCycle>::build(runtime));
    definitions.push_back(reflection::Definition<ToolSetting>::build(runtime));
    definitions.push_back(reflection::Definition<ToolToggle>::build(runtime));
    definitions.push_back(reflection::Definition<ToolSettings>::build(runtime));
    definitions.push_back(reflection::Definition<SceneAdd>::build(runtime));
    definitions.push_back(reflection::Definition<SceneParent>::build(runtime));
    definitions.push_back(reflection::Definition<SceneSpaceReference>::build(runtime));
    definitions.push_back(reflection::Definition<ViewAxis>::build(runtime));
    definitions.push_back(reflection::Definition<CameraState>::build(runtime));
    definitions.push_back(reflection::Definition<ViewCameraSet>::build(runtime));
    definitions.push_back(reflection::Definition<ViewLookAt>::build(runtime));
    definitions.push_back(reflection::Definition<OpenPath>::build(runtime));
    definitions.push_back(reflection::Definition<SceneSave>::build(runtime));
    definitions.push_back(reflection::Definition<ImportKind>::build(runtime));
    definitions.push_back(reflection::Definition<AssetImport>::build(runtime));
    definitions.push_back(reflection::Definition<ViewToggleOption>::build(runtime));
    definitions.push_back(reflection::Definition<ViewToggle>::build(runtime));
    definitions.push_back(reflection::Definition<MenuKind>::build(runtime));
    // The public menu name is a C++ keyword.
    definitions.back().enumeration->values[static_cast<unsigned>(MenuKind::delete_selection)].first = "delete";
    definitions.push_back(reflection::Definition<MenuParameters>::build(runtime));
    definitions.push_back(reflection::Definition<LightingFitMode>::build(runtime));
    definitions.push_back(reflection::Definition<LightingFit>::build(runtime));
    definitions.push_back(reflection::Definition<ProjectBuildParameters>::build(runtime));
    definitions.push_back(reflection::Definition<MaterialEntriesJson>::build(runtime));
    definitions.push_back(reflection::Definition<NewMaterial>::build(runtime));
    definitions.push_back(reflection::Definition<NewMaterialSource>::build(runtime));
    definitions.push_back(reflection::Definition<AssetResult>::build(runtime));
    definitions.push_back(reflection::Definition<NewProceduralGraph>::build(runtime));
    definitions.push_back(reflection::Definition<GraphOpen>::build(runtime));
    definitions.push_back(reflection::Definition<GraphDiveIn>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSave>::build(runtime));
    definitions.push_back(reflection::Definition<GraphAddNode>::build(runtime));
    definitions.push_back(reflection::Definition<GraphNodeResult>::build(runtime));
    definitions.push_back(reflection::Definition<GraphDeleteNode>::build(runtime));
    definitions.push_back(reflection::Definition<GraphDuplicateNodes>::build(runtime));
    definitions.push_back(reflection::Definition<GraphRenameNode>::build(runtime));
    definitions.push_back(reflection::Definition<GraphMoveNode>::build(runtime));
    definitions.push_back(reflection::Definition<GraphConnect>::build(runtime));
    definitions.push_back(reflection::Definition<GraphDisconnect>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSetDisplay>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSetTemplate>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSetBypass>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSetFrozen>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSetAppearance>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSelect>::build(runtime));
    definitions.push_back(reflection::Definition<GraphLayout>::build(runtime));
    definitions.push_back(reflection::Definition<GraphExposeParameter>::build(runtime));
    definitions.push_back(reflection::Definition<GraphBindParameter>::build(runtime));
    definitions.push_back(reflection::Definition<GraphUnbindParameter>::build(runtime));
    definitions.push_back(reflection::Definition<GraphUpdateParameter>::build(runtime));
    definitions.push_back(reflection::Definition<GraphRemoveParameter>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSetParameter>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSetOverride>::build(runtime));
    definitions.push_back(reflection::Definition<GraphSetNodeAsset>::build(runtime));
    definitions.push_back(reflection::Definition<GraphExtractAsset>::build(runtime));
    definitions.push_back(reflection::Definition<GraphExtractAssetResult>::build(runtime));
    definitions.push_back(reflection::Definition<ProceduralSetOverride>::build(runtime));
    definitions.push_back(reflection::Definition<AssignProceduralGraph>::build(runtime));
    definitions.push_back(reflection::Definition<ProceduralBake>::build(runtime));
    TypeDefinition view;
    view.name = "editor.ViewSettings";
    view.kind = TypeKind::record;
    for (const auto& option : view_options()) {
        FieldDefinition field;
        field.name = option.name;
        field.label = option.label;
        field.category = option.category;
        field.description = option.description;
        switch (option.kind) {
        case ViewValueKind::boolean:
            field.value.type = TypeReference::native<bool>();
            break;
        case ViewValueKind::integer:
            field.value.type = TypeReference::native<std::int32_t>();
            field.range = {.ui_min = Scalar{static_cast<std::int32_t>(option.minimum)},
                           .ui_max = Scalar{static_cast<std::int32_t>(option.maximum)},
                           .hard_min = Scalar{static_cast<std::int32_t>(option.minimum)},
                           .hard_max = Scalar{static_cast<std::int32_t>(option.maximum)}};
            break;
        case ViewValueKind::number:
            field.value.type = TypeReference::native<double>();
            field.range = {.ui_min = Scalar{option.minimum},
                           .ui_max = Scalar{option.maximum},
                           .hard_min = Scalar{option.minimum},
                           .hard_max = Scalar{option.maximum}};
            break;
        case ViewValueKind::enumeration: {
            TypeDefinition enumeration;
            enumeration.name = "editor.ViewOption_" + std::string(option.name);
            enumeration.kind = TypeKind::enumeration;
            enumeration.enumeration = TypeDefinition::Enumeration{TypeReference::native<std::uint32_t>(), {}};
            for (std::size_t i = 0; i < option.choices.size(); ++i)
                enumeration.enumeration->values.emplace_back(option.choices[i], static_cast<std::uint32_t>(i));
            field.value.type = TypeReference{enumeration.name};
            definitions.push_back(std::move(enumeration));
            break;
        }
        }
        view.fields.push_back(std::move(field));
    }
    definitions.push_back(std::move(view));
    ATELIER_TRY(runtime.register_types(std::move(definitions)));
    return {};
}
}
