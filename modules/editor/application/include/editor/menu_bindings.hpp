#pragma once
#include "editor/operators.hpp"
#include <array>

namespace atelier::editor {
enum class MenuGroup { file, edit, create, project, view, window, help };

struct MenuGroupDefinition {
    MenuGroup id;

    std::uint64_t key;

    std::wstring_view title;

    wchar_t access_key;
};

inline constexpr std::array menu_groups{MenuGroupDefinition{MenuGroup::file, 700, L"File", L'F'},
                                        MenuGroupDefinition{MenuGroup::edit, 701, L"Edit", L'E'},
                                        MenuGroupDefinition{MenuGroup::create, 702, L"Create", L'C'},
                                        MenuGroupDefinition{MenuGroup::project, 707, L"Project", L'P'},
                                        MenuGroupDefinition{MenuGroup::view, 703, L"View", L'V'},
                                        MenuGroupDefinition{MenuGroup::window, 704, L"Window", L'W'},
                                        MenuGroupDefinition{MenuGroup::help, 706, L"Help", L'H'}};

constexpr const MenuGroupDefinition* menu_group(std::uint64_t key) {
    for (const auto& group : menu_groups)
        if (group.key == key)
            return &group;
    return nullptr;
}

constexpr std::uint64_t menu_key(MenuGroup id) {
    for (const auto& group : menu_groups)
        if (group.id == id)
            return group.key;
    return 0;
}

constexpr std::uint64_t cycle_menu(std::uint64_t key, bool previous) {
    for (std::size_t i = 0; i < menu_groups.size(); ++i)
        if (menu_groups[i].key == key)
            return menu_groups[(i + (previous ? menu_groups.size() - 1 : 1)) % menu_groups.size()].key;
    return 0;
}

struct MenuBinding {
    OperatorCall call;

    MenuGroup group;

    std::wstring_view title;

    bool separator{};

    std::wstring_view shelf_title{};

    std::uint64_t radio_group{};

    std::optional<assets::AssetKind> asset_kind{};
};

// Presentation only: these are the same calls used by keyboard and Control, not another action identity.
inline const MenuBinding menu_bindings[]{
    {{"project.new"}, MenuGroup::file, L"New Project Folder..."},
    {{"project.open"}, MenuGroup::file, L"Open Project Folder..."},
    {{"scene.open"}, MenuGroup::file, L"Open Scene...", true},
    {{"source.open"}, MenuGroup::file, L"Open Source...", true},
    {{"file.save"}, MenuGroup::file, L"Save"},
    {{"file.save_all"}, MenuGroup::file, L"Save All"},
    {{"scene.save"}, MenuGroup::file, L"Save Scene", true},
    {{"scene.save", {{"as_new", true}}}, MenuGroup::file, L"Save Scene As..."},
    {{"asset.import", {{"kind", "mesh"}}}, MenuGroup::file, L"Import Mesh (OBJ, glTF)...", true},
    {{"asset.import", {{"kind", "texture"}}}, MenuGroup::file, L"Import Texture..."},
    {{"asset.reimport"}, MenuGroup::file, L"Reimport Texture From...", false, {}, 0, assets::AssetKind::texture},
    {{"asset.reimport"}, MenuGroup::file, L"Reimport Mesh From...", false, {}, 0, assets::AssetKind::mesh},
    {{"asset.new_material_source"}, MenuGroup::file, L"New Material Source..."},
    {{"asset.new_material"}, MenuGroup::file, L"New Material Instance..."},
    {{"asset.refresh"}, MenuGroup::file, L"Refresh Assets"},
    {{"source.reload"}, MenuGroup::file, L"Reload Source from Disk", true},
    {{"source.close"}, MenuGroup::file, L"Close Source Document"},
    {{"app.quit"}, MenuGroup::file, L"Exit"},
    {{"edit.undo"}, MenuGroup::edit, L"Undo"},
    {{"edit.redo"}, MenuGroup::edit, L"Redo"},
    {{"scene.duplicate"}, MenuGroup::edit, L"Duplicate Entities", true},
    {{"scene.transform_add"}, MenuGroup::edit, L"Add Transform"},
    {{"scene.transform_remove"}, MenuGroup::edit, L"Remove Transform"},
    {{"scene.main_camera_set"}, MenuGroup::edit, L"Set as Main Camera", true},
    {{"scene.main_camera_clear"}, MenuGroup::edit, L"Clear Main Camera"},
    {{"scene.delete", {{"target", "object"}}}, MenuGroup::edit, L"Delete Selected Object", true},
    {{"scene.delete", {{"target", "entities"}}}, MenuGroup::edit, L"Delete Entities"},
    {{"scene.rename"}, MenuGroup::edit, L"Rename"},
    {{"scene.parent_set", {{"parent", nullptr}}}, MenuGroup::edit, L"Clear Parent"},
    {{"scene.discard_saved_data"}, MenuGroup::edit, L"Discard Saved Data"},
    {{"scene.select_all", {{"action", "select"}}}, MenuGroup::edit, L"Select All", true},
    {{"scene.select_all", {{"action", "deselect"}}}, MenuGroup::edit, L"Select None"},
    {{"scene.select_all", {{"action", "invert"}}}, MenuGroup::edit, L"Invert Selection"},
    {{"view.select_box"}, MenuGroup::edit, L"Box Select"},
    {{"scene.transform_clear", {{"channel", "location"}}}, MenuGroup::edit, L"Clear Location", true},
    {{"scene.transform_clear", {{"channel", "rotation"}}}, MenuGroup::edit, L"Clear Rotation"},
    {{"scene.transform_clear", {{"channel", "scale"}}}, MenuGroup::edit, L"Clear Scale"},
    {{"scene.add", {{"kind", "entity"}}}, MenuGroup::create, L"Entity"},
    {{"scene.add", {{"kind", "mesh"}}}, MenuGroup::create, L"Static Mesh Entity", false, L"Static Mesh"},
    {{"scene.add", {{"kind", "camera"}}}, MenuGroup::create, L"Camera"},
    {{"scene.add", {{"kind", "point"}}}, MenuGroup::create, L"Point Light", true, L"Point Light"},
    {{"scene.add", {{"kind", "rectangle"}}}, MenuGroup::create, L"Rectangle Light", false, L"Area Light"},
    {{"scene.add", {{"kind", "directional"}}}, MenuGroup::create, L"Directional Light", false, L"Sun Light"},
    {{"view.frame_selected"}, MenuGroup::view, L"Focus Selection", false, L"Focus"},
    {{"view.frame_all"}, MenuGroup::view, L"Frame All"},
    {{"view.from_camera"}, MenuGroup::view, L"View from Camera"},
    {{"tool.settings", {{"select_mode", "set"}}}, MenuGroup::view, L"Set New Selection", true, {}, 1},
    {{"tool.settings", {{"select_mode", "extend"}}}, MenuGroup::view, L"Extend Selection", false, {}, 1},
    {{"tool.settings", {{"select_mode", "subtract"}}}, MenuGroup::view, L"Subtract Selection", false, {}, 1},
    {{"tool.settings", {{"select_mode", "invert"}}}, MenuGroup::view, L"Invert Selection Mode", false, {}, 1},
    {{"tool.settings", {{"select_mode", "intersect"}}}, MenuGroup::view, L"Intersect Selection", false, {}, 1},
    {{"tool.settings", {{"tool", "select"}}}, MenuGroup::view, L"Select", true, {}, 2},
    {{"tool.settings", {{"tool", "transform"}}}, MenuGroup::view, L"Universal Handle", false, {}, 2},
    {{"tool.settings", {{"tool", "move"}}}, MenuGroup::view, L"Move Handle", false, {}, 2},
    {{"tool.settings", {{"tool", "rotate"}}}, MenuGroup::view, L"Rotate Handle", false, {}, 2},
    {{"tool.settings", {{"tool", "scale"}}}, MenuGroup::view, L"Scale Handle", false, {}, 2},
    {{"tool.settings", {{"pivot", "median"}}}, MenuGroup::view, L"Pivot: Selection Center", true, {}, 3},
    {{"tool.settings", {{"pivot", "cursor"}}}, MenuGroup::view, L"Pivot: 3D Cursor", false, {}, 3},
    {{"cursor.reset"}, MenuGroup::view, L"Reset 3D Cursor"},
    {{"tool.toggle", {{"setting", "grid_snap"}}}, MenuGroup::view, L"Grid Snapping", true},
    {{"tool.settings", {{"grid_spacing", .1}}}, MenuGroup::view, L"0.1 m", false, {}, 4},
    {{"tool.settings", {{"grid_spacing", .2}}}, MenuGroup::view, L"0.2 m", false, {}, 4},
    {{"tool.settings", {{"grid_spacing", .5}}}, MenuGroup::view, L"0.5 m", false, {}, 4},
    {{"tool.settings", {{"grid_spacing", 1.}}}, MenuGroup::view, L"1 m", false, {}, 4},
    {{"tool.settings", {{"orientation", "global"}}}, MenuGroup::view, L"Global", true, {}, 5},
    {{"tool.settings", {{"orientation", "local"}}}, MenuGroup::view, L"Local", false, {}, 5},
    {{"tool.toggle", {{"setting", "snap_move"}}}, MenuGroup::view, L"Snap Move", true},
    {{"tool.toggle", {{"setting", "snap_rotate"}}}, MenuGroup::view, L"Snap Rotate"},
    {{"tool.toggle", {{"setting", "snap_scale"}}}, MenuGroup::view, L"Snap Scale"},
    {{"tool.settings", {{"rotation_step", 1.}}}, MenuGroup::view, L"Rotation 1°", true, {}, 6},
    {{"tool.settings", {{"rotation_step", 5.}}}, MenuGroup::view, L"Rotation 5°", false, {}, 6},
    {{"tool.settings", {{"rotation_step", 15.}}}, MenuGroup::view, L"Rotation 15°", false, {}, 6},
    {{"tool.settings", {{"rotation_step", 45.}}}, MenuGroup::view, L"Rotation 45°", false, {}, 6},
    {{"scene.snap_selection", {{"to", "grid"}}}, MenuGroup::view, L"Selection to Grid", true},
    {{"scene.snap_selection", {{"to", "cursor"}}}, MenuGroup::view, L"Selection to Cursor (Keep Offset)"},
    {{"cursor.to_selection"}, MenuGroup::view, L"Cursor to Selection"},
    {{"cursor.to_grid"}, MenuGroup::view, L"Cursor to Grid"},
    {{"tool.toggle", {{"setting", "cursor_surface"}}}, MenuGroup::view, L"Cursor Surface Placement"},
    {{"view.axis", {{"view", "perspective"}}}, MenuGroup::view, L"Perspective", true, {}, 7},
    {{"view.axis", {{"view", "top"}}}, MenuGroup::view, L"Top", false, {}, 7},
    {{"view.axis", {{"view", "front"}}}, MenuGroup::view, L"Front", false, {}, 7},
    {{"view.axis", {{"view", "right"}}}, MenuGroup::view, L"Right", false, {}, 7},
    {{"view.axis", {{"view", "bottom"}}}, MenuGroup::view, L"Bottom", false, {}, 7},
    {{"view.axis", {{"view", "back"}}}, MenuGroup::view, L"Back", false, {}, 7},
    {{"view.axis", {{"view", "left"}}}, MenuGroup::view, L"Left", false, {}, 7},
    {{"view.toggle", {{"option", "options"}}}, MenuGroup::view, L"Show Tool Options", true},
    {{"view.toggle", {{"option", "fps"}}}, MenuGroup::view, L"Show FPS"},
    {{"source.open_editor_ui"}, MenuGroup::file, L"Edit Editor UI Source"},
    {{"source.new_component"}, MenuGroup::project, L"New Component Source..."},
    {{"project.build"}, MenuGroup::project, L"Build Project"},
    {{"project.build", {{"rebuild", true}}}, MenuGroup::project, L"Rebuild Types and Scene"},
    {{"play.start"}, MenuGroup::project, L"Run Project", true},
    {{"play.stop"}, MenuGroup::project, L"Stop Project"},
    {{"play.restart"}, MenuGroup::project, L"Restart Project"},
    {{"source.new_project_ui"}, MenuGroup::project, L"New Project UI...", true},
    {{"source.open_project_ui"}, MenuGroup::project, L"Edit Project UI"},
    {{"play.preview_ui"}, MenuGroup::project, L"Preview Project UI"},
    {{"view.reset"}, MenuGroup::view, L"Reset Camera", false, L"Reset View"},
    {{"layout.show", {{"tab", "viewport"}}}, MenuGroup::window, L"Scene View"},
    {{"layout.show", {{"tab", "parameters"}}}, MenuGroup::window, L"Parameters"},
    {{"layout.show", {{"tab", "scene_tree"}}}, MenuGroup::window, L"Scene Tree"},
    {{"layout.show", {{"tab", "structure"}}}, MenuGroup::window, L"Entity Structure"},
    {{"layout.show", {{"tab", "assets"}}}, MenuGroup::window, L"Asset Browser"},
    {{"layout.show", {{"tab", "code"}}}, MenuGroup::window, L"Code"},
    {{"layout.show", {{"tab", "output"}}}, MenuGroup::window, L"Project Output"},
    {{"layout.show", {{"tab", "controls"}}}, MenuGroup::window, L"Controls"},
    {{"layout.show", {{"tab", "world"}}}, MenuGroup::window, L"World Settings"},
    {{"view.lighting_fit", {{"mode", "scene"}}}, MenuGroup::view, L"Fit Scene Lighting Coverage"},
    {{"view.lighting_fit", {{"mode", "scene_focus"}}}, MenuGroup::view, L"Scene + Follow Focus Lighting"},
    {{"layout.cycle_tab", {{"direction", -1}}}, MenuGroup::window, L"Previous Pane Tab", true},
    {{"layout.cycle_tab", {{"direction", 1}}}, MenuGroup::window, L"Next Pane Tab"},
    {{"layout.close_tab"}, MenuGroup::window, L"Close Pane Tab"},
    {{"layout.maximize"}, MenuGroup::window, L"Maximize / Restore Pane", true},
    {{"layout.reset"}, MenuGroup::window, L"Reset Layout"},
    {{"app.about"}, MenuGroup::help, L"About FrostFox Atelier"}};
}
