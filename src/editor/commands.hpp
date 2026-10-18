#pragma once
#include "editor/camera.hpp"
#include <array>
#include <optional>
#include <cstdint>
#include <string_view>

namespace atelier::editor {
enum class Command : std::uint64_t {
    create_entity=10, erase_selected=11, undo=12, redo=13, focus=14, reset_camera=15, duplicate_selected=16,
    create_static_mesh=17, erase_entities=19, reset_layout=603, exit=705,
    show_scene=710, show_parameters=711, show_controls=712, show_entities=713, maximize_pane=714,
    show_assets=721, previous_tab=722, next_tab=723, close_tab=724,
    new_project=750, open_project=751, save_scene=752, save_scene_as=753, open_scene=754,
    import_mesh=755, refresh_assets=756, about=757,
    open_source=780, edit_ui_source=781, show_code=782, save_document=783, close_source=784, reload_source=785, save_all=786,
    new_source=787, build_project=788, rebuild_project=789, run_project=790, stop_project=791,
    new_project_ui=792, edit_project_ui=793, show_project_ui=794, show_output=795, show_structure=796,
    create_material=797, import_texture=798,
    tool_select=810, tool_transform=811, tool_move=812, tool_rotate=813, tool_scale=814,
    pivot_median=815, pivot_cursor=816, cursor_origin=817,
    snap_toggle=818, snap_grid_01=819, snap_grid_02=820, snap_grid_05=821, snap_grid_1=822,
    selection_grid=823, selection_cursor=824, cursor_selected=825, cursor_grid=826, cursor_surface=827,
    view_perspective=830, view_top=831, view_front=832, view_right=833, viewport_options=834,
    create_point_light=835, create_rectangle_light=836, create_directional_light=837, create_sky_light=838,
    example_sponza=841, example_scene_lit=842, example_physic=843, example_dungeons=844,
    view_bottom=900, view_back=901, view_left=902, show_fps=903,
    show_world_settings=906,fit_probe_scene=907,fit_probe_scene_focus=908, add_transform=909,remove_transform=910,
    // Blender viewport header semantics: Select menu, Object > Clear, View > Frame All, select modes.
    select_all=961, select_none=962, select_invert=963, select_box=964,
    clear_location=965, clear_rotation=966, clear_scale=967, frame_all=968,
    select_mode_set=969, select_mode_extend=970, select_mode_subtract=971, select_mode_invert=972, select_mode_intersect=973,
    rename_selected=986, unparent_selected=987, discard_saved_data=988,
    // Blender snapping panel: Affect toggles and rotation increment.
    snap_affect_move=981, snap_affect_rotate=982, snap_affect_scale=983,
    snap_rotation_1=957, snap_rotation_5=958, snap_rotation_15=959, snap_rotation_45=984,
    orientation_global=985, orientation_local=997
};
inline constexpr std::array camera_commands{Command::view_perspective,Command::view_top,Command::view_front,
    Command::view_right,Command::view_bottom,Command::view_back,Command::view_left};
constexpr std::optional<CameraView> camera_view_command(Command command) noexcept {
    for(std::size_t i=0;i<camera_commands.size();++i)
        if(camera_commands[i]==command) return static_cast<CameraView>(i);
    return {};
}
enum class CommandGroup { file, edit, create, project, view, window, help };
struct CommandGroupDefinition {
    CommandGroup id;
    std::uint64_t key;
    std::wstring_view title;
    wchar_t access_key;
};
inline constexpr std::array command_groups{
    CommandGroupDefinition{CommandGroup::file,700,L"File",L'F'},
    CommandGroupDefinition{CommandGroup::edit,701,L"Edit",L'E'},
    CommandGroupDefinition{CommandGroup::create,702,L"Create",L'C'},
    CommandGroupDefinition{CommandGroup::project,707,L"Project",L'P'},
    CommandGroupDefinition{CommandGroup::view,703,L"View",L'V'},
    CommandGroupDefinition{CommandGroup::window,704,L"Window",L'W'},
    CommandGroupDefinition{CommandGroup::help,706,L"Help",L'H'}
};
constexpr const CommandGroupDefinition* command_group(std::uint64_t key) {
    for (const auto& group:command_groups) if (group.key==key) return &group;
    return nullptr;
}
constexpr std::uint64_t menu_key(CommandGroup id) {
    for (const auto& group:command_groups) if (group.id==id) return group.key;
    return 0;
}
constexpr std::uint64_t cycle_menu(std::uint64_t key,bool previous) {
    for (std::size_t i=0;i<command_groups.size();++i) if (command_groups[i].key==key)
        return command_groups[(i+(previous?command_groups.size()-1:1))%command_groups.size()].key;
    return 0;
}
struct CommandDefinition {
    Command id;
    CommandGroup group;
    std::wstring_view title, shortcut;
    bool separator{};
    std::wstring_view shelf_title{};
};
inline constexpr std::array command_definitions{
    CommandDefinition{Command::new_project,CommandGroup::file,L"New Project Folder...",L""},
    CommandDefinition{Command::open_project,CommandGroup::file,L"Open Project Folder...",L"Ctrl+O"},
    CommandDefinition{Command::open_scene,CommandGroup::file,L"Open Scene...",L"",true},
    CommandDefinition{Command::open_source,CommandGroup::file,L"Open Source...",L"",true},
    CommandDefinition{Command::save_document,CommandGroup::file,L"Save",L"Ctrl+S"},
    CommandDefinition{Command::save_all,CommandGroup::file,L"Save All",L""},
    CommandDefinition{Command::save_scene,CommandGroup::file,L"Save Scene",L"",true},
    CommandDefinition{Command::save_scene_as,CommandGroup::file,L"Save Scene As...",L"Ctrl+Shift+S"},
    CommandDefinition{Command::import_mesh,CommandGroup::file,L"Import Mesh (OBJ)...",L"",true},
    CommandDefinition{Command::import_texture,CommandGroup::file,L"Import Texture...",L""},
    CommandDefinition{Command::create_material,CommandGroup::file,L"New Material...",L""},
    CommandDefinition{Command::refresh_assets,CommandGroup::file,L"Refresh Assets",L"F5"},
    CommandDefinition{Command::reload_source,CommandGroup::file,L"Reload Source from Disk",L"",true},
    CommandDefinition{Command::close_source,CommandGroup::file,L"Close Source Document",L""},
    CommandDefinition{Command::exit,CommandGroup::file,L"Exit",L"Alt+F4"},
    CommandDefinition{Command::undo,CommandGroup::edit,L"Undo",L"Ctrl+Z"},
    CommandDefinition{Command::redo,CommandGroup::edit,L"Redo",L"Ctrl+Shift+Z"},
    CommandDefinition{Command::duplicate_selected,CommandGroup::edit,L"Duplicate Entities",L"Shift+D",true},
    CommandDefinition{Command::add_transform,CommandGroup::edit,L"Add Transform",L""},
    CommandDefinition{Command::remove_transform,CommandGroup::edit,L"Remove Transform",L""},
    CommandDefinition{Command::erase_selected,CommandGroup::edit,L"Delete Selected Object",L"Delete",true},
    CommandDefinition{Command::erase_entities,CommandGroup::edit,L"Delete Entities",L"X"},
    CommandDefinition{Command::rename_selected,CommandGroup::edit,L"Rename",L"F2"},
    CommandDefinition{Command::unparent_selected,CommandGroup::edit,L"Clear Parent",L"Alt+P"},
    CommandDefinition{Command::discard_saved_data,CommandGroup::edit,L"Discard Saved Data",L""},
    CommandDefinition{Command::select_all,CommandGroup::edit,L"Select All",L"A",true},
    CommandDefinition{Command::select_none,CommandGroup::edit,L"Select None",L"Alt+A"},
    CommandDefinition{Command::select_invert,CommandGroup::edit,L"Invert Selection",L"Ctrl+I"},
    CommandDefinition{Command::select_box,CommandGroup::edit,L"Box Select",L"B"},
    CommandDefinition{Command::clear_location,CommandGroup::edit,L"Clear Location",L"Alt+G",true},
    CommandDefinition{Command::clear_rotation,CommandGroup::edit,L"Clear Rotation",L"Alt+R"},
    CommandDefinition{Command::clear_scale,CommandGroup::edit,L"Clear Scale",L"Alt+S"},
    CommandDefinition{Command::create_entity,CommandGroup::create,L"Entity",L""},
    CommandDefinition{Command::create_static_mesh,CommandGroup::create,L"Static Mesh Entity",L"",false,L"Static Mesh"},
    CommandDefinition{Command::create_point_light,CommandGroup::create,L"Point Light",L"",true,L"Point Light"},
    CommandDefinition{Command::create_rectangle_light,CommandGroup::create,L"Rectangle Light",L"",false,L"Area Light"},
    CommandDefinition{Command::create_directional_light,CommandGroup::create,L"Directional Light",L"",false,L"Sun Light"},
    CommandDefinition{Command::create_sky_light,CommandGroup::create,L"Sky Light",L"",false,L"Sky Light"},
    CommandDefinition{Command::focus,CommandGroup::view,L"Focus Selection",L"F",false,L"Focus"},
    CommandDefinition{Command::frame_all,CommandGroup::view,L"Frame All",L"Home"},
    CommandDefinition{Command::select_mode_set,CommandGroup::view,L"Set New Selection",L"",true},
    CommandDefinition{Command::select_mode_extend,CommandGroup::view,L"Extend Selection",L""},
    CommandDefinition{Command::select_mode_subtract,CommandGroup::view,L"Subtract Selection",L""},
    CommandDefinition{Command::select_mode_invert,CommandGroup::view,L"Invert Selection Mode",L""},
    CommandDefinition{Command::select_mode_intersect,CommandGroup::view,L"Intersect Selection",L""},
    CommandDefinition{Command::tool_select,CommandGroup::view,L"Select",L"Q",true},
    CommandDefinition{Command::tool_transform,CommandGroup::view,L"Universal Handle",L""},
    CommandDefinition{Command::tool_move,CommandGroup::view,L"Move Handle",L"G (modal)"},
    CommandDefinition{Command::tool_rotate,CommandGroup::view,L"Rotate Handle",L"R (modal)"},
    CommandDefinition{Command::tool_scale,CommandGroup::view,L"Scale Handle",L"S (modal)"},
    CommandDefinition{Command::pivot_median,CommandGroup::view,L"Pivot: Selection Center",L"",true},
    CommandDefinition{Command::pivot_cursor,CommandGroup::view,L"Pivot: 3D Cursor",L""},
    CommandDefinition{Command::cursor_origin,CommandGroup::view,L"Reset 3D Cursor",L""},
    CommandDefinition{Command::snap_toggle,CommandGroup::view,L"Grid Snapping",L"Shift+Tab",true},
    CommandDefinition{Command::snap_grid_01,CommandGroup::view,L"0.1 m",L""},
    CommandDefinition{Command::snap_grid_02,CommandGroup::view,L"0.2 m",L""},
    CommandDefinition{Command::snap_grid_05,CommandGroup::view,L"0.5 m",L""},
    CommandDefinition{Command::snap_grid_1,CommandGroup::view,L"1 m",L""},
    CommandDefinition{Command::orientation_global,CommandGroup::view,L"Global",L"",true},
    CommandDefinition{Command::orientation_local,CommandGroup::view,L"Local",L""},
    CommandDefinition{Command::snap_affect_move,CommandGroup::view,L"Snap Move",L"",true},
    CommandDefinition{Command::snap_affect_rotate,CommandGroup::view,L"Snap Rotate",L""},
    CommandDefinition{Command::snap_affect_scale,CommandGroup::view,L"Snap Scale",L""},
    CommandDefinition{Command::snap_rotation_1,CommandGroup::view,L"Rotation 1°",L"",true},
    CommandDefinition{Command::snap_rotation_5,CommandGroup::view,L"Rotation 5°",L""},
    CommandDefinition{Command::snap_rotation_15,CommandGroup::view,L"Rotation 15°",L""},
    CommandDefinition{Command::snap_rotation_45,CommandGroup::view,L"Rotation 45°",L""},
    CommandDefinition{Command::selection_grid,CommandGroup::view,L"Selection to Grid",L"",true},
    CommandDefinition{Command::selection_cursor,CommandGroup::view,L"Selection to Cursor (Keep Offset)",L""},
    CommandDefinition{Command::cursor_selected,CommandGroup::view,L"Cursor to Selection",L""},
    CommandDefinition{Command::cursor_grid,CommandGroup::view,L"Cursor to Grid",L""},
    CommandDefinition{Command::cursor_surface,CommandGroup::view,L"Cursor Surface Placement",L""},
    CommandDefinition{Command::view_perspective,CommandGroup::view,L"Perspective",L"Numpad 5",true},
    CommandDefinition{Command::view_top,CommandGroup::view,L"Top",L"Numpad 7"},
    CommandDefinition{Command::view_front,CommandGroup::view,L"Front",L"Numpad 1"},
    CommandDefinition{Command::view_right,CommandGroup::view,L"Right",L"Numpad 3"},
    CommandDefinition{Command::view_bottom,CommandGroup::view,L"Bottom",L"Ctrl+Numpad 7"},
    CommandDefinition{Command::view_back,CommandGroup::view,L"Back",L"Ctrl+Numpad 1"},
    CommandDefinition{Command::view_left,CommandGroup::view,L"Left",L"Ctrl+Numpad 3"},
    CommandDefinition{Command::viewport_options,CommandGroup::view,L"Show Tool Options",L"",true},
    CommandDefinition{Command::show_fps,CommandGroup::view,L"Show FPS",L""},
    CommandDefinition{Command::edit_ui_source,CommandGroup::file,L"Edit Editor UI Source",L""},
    CommandDefinition{Command::example_sponza,CommandGroup::project,L"Sponza",L""},
    CommandDefinition{Command::example_scene_lit,CommandGroup::project,L"Scene Lit Room",L""},
    CommandDefinition{Command::example_physic,CommandGroup::project,L"Physic",L""},
    CommandDefinition{Command::example_dungeons,CommandGroup::project,L"Dungeons",L""},
    CommandDefinition{Command::new_source,CommandGroup::project,L"New Component Source...",L""},
    CommandDefinition{Command::build_project,CommandGroup::project,L"Build Project",L"F6"},
    CommandDefinition{Command::rebuild_project,CommandGroup::project,L"Rebuild Types and Scene",L""},
    CommandDefinition{Command::run_project,CommandGroup::project,L"Run Project",L"F8",true},
    CommandDefinition{Command::stop_project,CommandGroup::project,L"Stop Project",L"Shift+F8"},
    CommandDefinition{Command::new_project_ui,CommandGroup::project,L"New Project UI...",L"",true},
    CommandDefinition{Command::edit_project_ui,CommandGroup::project,L"Edit Project UI",L""},
    CommandDefinition{Command::show_project_ui,CommandGroup::project,L"Preview Project UI",L""},
    CommandDefinition{Command::reset_camera,CommandGroup::view,L"Reset Camera",L"",false,L"Reset View"},
    CommandDefinition{Command::show_scene,CommandGroup::window,L"Scene View",L""},
    CommandDefinition{Command::show_parameters,CommandGroup::window,L"Parameters",L""},
    CommandDefinition{Command::show_entities,CommandGroup::window,L"Scene Tree",L""},
    CommandDefinition{Command::show_structure,CommandGroup::window,L"Entity Structure",L""},
    CommandDefinition{Command::show_assets,CommandGroup::window,L"Asset Browser",L""},
    CommandDefinition{Command::show_code,CommandGroup::window,L"Code",L""},
    CommandDefinition{Command::show_output,CommandGroup::window,L"Project Output",L""},
    CommandDefinition{Command::show_controls,CommandGroup::window,L"Controls",L""},
    CommandDefinition{Command::show_world_settings,CommandGroup::window,L"World Settings",L""},
    CommandDefinition{Command::fit_probe_scene,CommandGroup::view,L"Fit Scene Lighting Coverage",L""},
    CommandDefinition{Command::fit_probe_scene_focus,CommandGroup::view,L"Scene + Follow Focus Lighting",L""},
    CommandDefinition{Command::previous_tab,CommandGroup::window,L"Previous Pane Tab",L"Ctrl+Shift+Tab",true},
    CommandDefinition{Command::next_tab,CommandGroup::window,L"Next Pane Tab",L"Ctrl+Tab"},
    CommandDefinition{Command::close_tab,CommandGroup::window,L"Close Pane Tab",L"Ctrl+W"},
    CommandDefinition{Command::maximize_pane,CommandGroup::window,L"Maximize / Restore Pane",L"Ctrl+B",true},
    CommandDefinition{Command::reset_layout,CommandGroup::window,L"Reset Layout",L""},
    CommandDefinition{Command::about,CommandGroup::help,L"About FrostFox Atelier",L""}
};
constexpr const CommandDefinition* command_definition(std::uint64_t key) {
    for (const auto& item:command_definitions) if (static_cast<std::uint64_t>(item.id)==key) return &item;
    return nullptr;
}
}
