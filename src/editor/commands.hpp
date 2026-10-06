#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace atelier::editor {
enum class Command : std::uint64_t {
    create_actor=10, erase_selected=11, undo=12, redo=13, focus=14, reset_camera=15,
    create_static_mesh=17, reset_layout=603, exit=705,
    show_scene=710, show_parameters=711, show_controls=712, show_actors=713, maximize_pane=714,
    show_assets=721, previous_tab=722, next_tab=723, close_tab=724,
    new_project=750, open_project=751, save_scene=752, save_scene_as=753, open_scene=754,
    import_mesh=755, refresh_assets=756, about=757,
    open_source=780, edit_ui_source=781, show_code=782, save_document=783, close_source=784, reload_source=785, save_all=786,
    new_source=787, build_project=788, rebuild_project=789, run_project=790, stop_project=791,
    new_project_ui=792, edit_project_ui=793, show_project_ui=794, show_output=795, show_structure=796
};
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
    CommandDefinition{Command::refresh_assets,CommandGroup::file,L"Refresh Assets",L"F5"},
    CommandDefinition{Command::reload_source,CommandGroup::file,L"Reload Source from Disk",L"",true},
    CommandDefinition{Command::close_source,CommandGroup::file,L"Close Source Document",L""},
    CommandDefinition{Command::exit,CommandGroup::file,L"Exit",L"Alt+F4"},
    CommandDefinition{Command::undo,CommandGroup::edit,L"Undo",L"Ctrl+Z"},
    CommandDefinition{Command::redo,CommandGroup::edit,L"Redo",L"Ctrl+Shift+Z"},
    CommandDefinition{Command::erase_selected,CommandGroup::edit,L"Delete Selected Object",L"Delete",true},
    CommandDefinition{Command::create_actor,CommandGroup::create,L"Actor",L""},
    CommandDefinition{Command::create_static_mesh,CommandGroup::create,L"Static Mesh Actor",L"",false,L"Static Mesh"},
    CommandDefinition{Command::focus,CommandGroup::view,L"Focus Selection",L"F",false,L"Focus"},
    CommandDefinition{Command::edit_ui_source,CommandGroup::file,L"Edit Editor UI Source",L""},
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
    CommandDefinition{Command::show_actors,CommandGroup::window,L"Scene Tree",L""},
    CommandDefinition{Command::show_structure,CommandGroup::window,L"Actor Structure",L""},
    CommandDefinition{Command::show_assets,CommandGroup::window,L"Asset Browser",L""},
    CommandDefinition{Command::show_code,CommandGroup::window,L"Code",L""},
    CommandDefinition{Command::show_output,CommandGroup::window,L"Project Output",L""},
    CommandDefinition{Command::show_controls,CommandGroup::window,L"Controls",L""},
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
