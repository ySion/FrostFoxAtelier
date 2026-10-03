#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace atelier::editor {
enum class Command : std::uint64_t {
    create_actor=10, erase_actor=11, undo=12, redo=13, focus=14, reset_camera=15,
    create_static_mesh=17, reset_layout=603, exit=705,
    show_scene=710, show_parameters=711, show_controls=712, show_actors=713, maximize_pane=714,
    show_assets=721, previous_tab=722, next_tab=723, close_tab=724,
    new_project=750, open_project=751, save_scene=752, save_scene_as=753, open_scene=754,
    import_mesh=755, refresh_assets=756
};
enum class CommandGroup { file, edit, create, view, window };
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
    CommandDefinition{Command::save_scene,CommandGroup::file,L"Save Scene",L"Ctrl+S"},
    CommandDefinition{Command::save_scene_as,CommandGroup::file,L"Save Scene As...",L"Ctrl+Shift+S"},
    CommandDefinition{Command::import_mesh,CommandGroup::file,L"Import Mesh (OBJ)...",L"",true},
    CommandDefinition{Command::refresh_assets,CommandGroup::file,L"Refresh Assets",L"F5"},
    CommandDefinition{Command::exit,CommandGroup::file,L"Exit",L"Alt+F4"},
    CommandDefinition{Command::undo,CommandGroup::edit,L"Undo",L"Ctrl+Z"},
    CommandDefinition{Command::redo,CommandGroup::edit,L"Redo",L"Ctrl+Shift+Z"},
    CommandDefinition{Command::erase_actor,CommandGroup::edit,L"Delete Actor",L"Delete",true},
    CommandDefinition{Command::create_actor,CommandGroup::create,L"Actor",L""},
    CommandDefinition{Command::create_static_mesh,CommandGroup::create,L"Static Mesh Actor",L"",false,L"Static Mesh"},
    CommandDefinition{Command::focus,CommandGroup::view,L"Focus Selection",L"F",false,L"Focus"},
    CommandDefinition{Command::reset_camera,CommandGroup::view,L"Reset Camera",L"",false,L"Reset View"},
    CommandDefinition{Command::show_scene,CommandGroup::window,L"Scene View",L""},
    CommandDefinition{Command::show_parameters,CommandGroup::window,L"Parameters",L""},
    CommandDefinition{Command::show_actors,CommandGroup::window,L"Scene Tree",L""},
    CommandDefinition{Command::show_assets,CommandGroup::window,L"Asset Browser",L""},
    CommandDefinition{Command::show_controls,CommandGroup::window,L"Controls",L""},
    CommandDefinition{Command::previous_tab,CommandGroup::window,L"Previous Pane Tab",L"Ctrl+Shift+Tab",true},
    CommandDefinition{Command::next_tab,CommandGroup::window,L"Next Pane Tab",L"Ctrl+Tab"},
    CommandDefinition{Command::close_tab,CommandGroup::window,L"Close Pane Tab",L"Ctrl+W"},
    CommandDefinition{Command::maximize_pane,CommandGroup::window,L"Maximize / Restore Pane",L"Ctrl+B",true},
    CommandDefinition{Command::reset_layout,CommandGroup::window,L"Reset Layout",L""}
};
constexpr const CommandDefinition* command_definition(std::uint64_t key) {
    for (const auto& item:command_definitions) if (static_cast<std::uint64_t>(item.id)==key) return &item;
    return nullptr;
}
}
