#include "editor/operator_parameters.hpp"
#include "editor/view_options.hpp"

namespace atelier::editor {
using namespace atelier::literals;

Result<void> register_editor_types(Runtime& runtime) {
    Array<TypeDefinition> definitions;
    definitions.reserve(84);
    definitions.add(reflection::Definition<TransformTool>::build(runtime));
    definitions.add(reflection::Definition<PivotMode>::build(runtime));
    definitions.add(reflection::Definition<SelectMode>::build(runtime));
    definitions.add(reflection::Definition<SelectAll>::build(runtime));
    definitions.add(reflection::Definition<TransformChannel>::build(runtime));
    definitions.add(reflection::Definition<SnapTarget>::build(runtime));
    definitions.add(reflection::Definition<DeleteTarget>::build(runtime));
    definitions.add(reflection::Definition<SceneDelete>::build(runtime));
    definitions.add(reflection::Definition<SceneDuplicate>::build(runtime));
    definitions.add(reflection::Definition<SceneAddComponent>::build(runtime));
    definitions.add(reflection::Definition<SceneTransform>::build(runtime));
    definitions.add(reflection::Definition<SceneRename>::build(runtime));
    definitions.add(reflection::Definition<SceneSelectAll>::build(runtime));
    definitions.add(reflection::Definition<SceneTransformClear>::build(runtime));
    definitions.add(reflection::Definition<SceneSnap>::build(runtime));
    definitions.add(reflection::Definition<Orientation>::build(runtime));
    definitions.add(reflection::Definition<CameraView>::build(runtime));
    definitions.add(reflection::Definition<AddKind>::build(runtime));
    definitions.add(reflection::Definition<RegionKind>::build(runtime));
    definitions.add(reflection::Definition<LayoutShow>::build(runtime));
    definitions.add(reflection::Definition<LayoutCycle>::build(runtime));
    definitions.add(reflection::Definition<ToolSetting>::build(runtime));
    definitions.add(reflection::Definition<ToolToggle>::build(runtime));
    definitions.add(reflection::Definition<ToolSettings>::build(runtime));
    definitions.add(reflection::Definition<SceneAdd>::build(runtime));
    definitions.add(reflection::Definition<SceneParent>::build(runtime));
    definitions.add(reflection::Definition<SceneSpaceReference>::build(runtime));
    definitions.add(reflection::Definition<ViewAxis>::build(runtime));
    definitions.add(reflection::Definition<CameraState>::build(runtime));
    definitions.add(reflection::Definition<ViewCameraSet>::build(runtime));
    definitions.add(reflection::Definition<ViewLookAt>::build(runtime));
    definitions.add(reflection::Definition<OpenPath>::build(runtime));
    definitions.add(reflection::Definition<SceneSave>::build(runtime));
    definitions.add(reflection::Definition<ImportKind>::build(runtime));
    definitions.add(reflection::Definition<AssetImport>::build(runtime));
    definitions.add(reflection::Definition<ViewToggleOption>::build(runtime));
    definitions.add(reflection::Definition<ViewToggle>::build(runtime));
    definitions.add(reflection::Definition<MenuKind>::build(runtime));
    // The public menu name is a C++ keyword.
    definitions.back().enumeration->values[static_cast<unsigned>(MenuKind::delete_selection)].first = "delete"_s;
    definitions.add(reflection::Definition<MenuParameters>::build(runtime));
    definitions.add(reflection::Definition<LightingFitMode>::build(runtime));
    definitions.add(reflection::Definition<LightingFit>::build(runtime));
    definitions.add(reflection::Definition<ProjectBuildParameters>::build(runtime));
    definitions.add(reflection::Definition<MaterialEntriesJson>::build(runtime));
    definitions.add(reflection::Definition<NewMaterial>::build(runtime));
    definitions.add(reflection::Definition<NewMaterialSource>::build(runtime));
    definitions.add(reflection::Definition<AssetResult>::build(runtime));
    definitions.add(reflection::Definition<NewProceduralGraph>::build(runtime));
    definitions.add(reflection::Definition<GraphOpen>::build(runtime));
    definitions.add(reflection::Definition<GraphDiveIn>::build(runtime));
    definitions.add(reflection::Definition<GraphSave>::build(runtime));
    definitions.add(reflection::Definition<GraphAddNode>::build(runtime));
    definitions.add(reflection::Definition<GraphNodeResult>::build(runtime));
    definitions.add(reflection::Definition<GraphDeleteNode>::build(runtime));
    definitions.add(reflection::Definition<GraphDuplicateNodes>::build(runtime));
    definitions.add(reflection::Definition<GraphRenameNode>::build(runtime));
    definitions.add(reflection::Definition<GraphMoveNode>::build(runtime));
    definitions.add(reflection::Definition<GraphConnect>::build(runtime));
    definitions.add(reflection::Definition<GraphDisconnect>::build(runtime));
    definitions.add(reflection::Definition<GraphSetDisplay>::build(runtime));
    definitions.add(reflection::Definition<GraphSetTemplate>::build(runtime));
    definitions.add(reflection::Definition<GraphSetBypass>::build(runtime));
    definitions.add(reflection::Definition<GraphSetFrozen>::build(runtime));
    definitions.add(reflection::Definition<GraphSetAppearance>::build(runtime));
    definitions.add(reflection::Definition<GraphSelect>::build(runtime));
    definitions.add(reflection::Definition<GraphLayout>::build(runtime));
    definitions.add(reflection::Definition<GraphExposeParameter>::build(runtime));
    definitions.add(reflection::Definition<GraphBindParameter>::build(runtime));
    definitions.add(reflection::Definition<GraphUnbindParameter>::build(runtime));
    definitions.add(reflection::Definition<GraphUpdateParameter>::build(runtime));
    definitions.add(reflection::Definition<GraphRemoveParameter>::build(runtime));
    definitions.add(reflection::Definition<GraphSetParameter>::build(runtime));
    definitions.add(reflection::Definition<GraphSetOverride>::build(runtime));
    definitions.add(reflection::Definition<GraphSetNodeAsset>::build(runtime));
    definitions.add(reflection::Definition<GraphExtractAsset>::build(runtime));
    definitions.add(reflection::Definition<GraphExtractAssetResult>::build(runtime));
    definitions.add(reflection::Definition<ProceduralSetOverride>::build(runtime));
    definitions.add(reflection::Definition<AssignProceduralGraph>::build(runtime));
    definitions.add(reflection::Definition<ProceduralBake>::build(runtime));
    TypeDefinition view;
    view.name = "editor.ViewSettings"_s;
    view.kind = TypeKind::record;
    for (const auto& option : view_options()) {
        FieldDefinition field;
        field.name = String::from_utf8_validated(option.name);
        field.label = String::from_utf8_validated(option.label);
        field.category = String::from_utf8_validated(option.category);
        field.description = String::from_utf8_validated(option.description);
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
            enumeration.name = String::concat("editor.ViewOption_", String::validated(option.name));
            enumeration.kind = TypeKind::enumeration;
            enumeration.enumeration = TypeDefinition::Enumeration{TypeReference::native<std::uint32_t>(), {}};
            for (std::size_t i = 0; i < option.choices.size(); ++i)
                enumeration.enumeration->values.emplace(String::from_utf8_validated(option.choices[i]),
                                                        static_cast<std::uint32_t>(i));
            field.value.type = TypeReference{enumeration.name};
            definitions.add(std::move(enumeration));
            break;
        }
        }
        view.fields.add(std::move(field));
    }
    definitions.add(std::move(view));
    ATELIER_TRY(runtime.register_types(std::move(definitions)));
    return {};
}
}
