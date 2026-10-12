#pragma once
#include "editor/project.hpp"
#include "foundation/map.hpp"
#include "ui/controls.hpp"
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace atelier::ui_language {class LiveDocument;struct Item;}

namespace atelier::editor {
namespace browser_control {
inline constexpr ui::ControlKey root=1ULL<<44, project=root+1, engine=root+2, up=root+3,
    path=root+4, search=root+5, clear=root+6, filter=root+7, list=root+8, details=root+9, tiles=root+10,
    open=root+11, add=root+12, view_menu=root+13, actions=root+14, back=root+15, directories=root+16,
    results=root+17, content=root+18, divider=root+19, filter_first=root+20;
inline constexpr ui::TableKey name_column=1, type_column=2, size_column=3;
}
class AssetBrowser {
public:
    struct Item { std::string key; std::filesystem::path path; std::wstring name; AssetKind kind{}; std::uintmax_t bytes{}; };
    enum class View { list, details, tiles };
    void reset(bool has_project);
    void append(std::vector<ui::Control>&,ui::ControlKey parent,const Project*,bool can_create,const ui::UiFrame*,ui_language::LiveDocument&);
    bool apply(const ui::Action&);
    std::vector<ui::MenuItem> menu_items(ui::ControlKey source) const;
    const std::optional<Item>& selected() const noexcept { return selected_; }
    const std::filesystem::path& directory() const noexcept { return directory_; }
    void navigate(std::filesystem::path path);
    bool engine() const noexcept { return engine_; }
    ui::ControlKey directory_control(const std::filesystem::path& path) const;
    ui::TableKey row_key(std::string_view resource) const {
        const auto key="item:"+std::string(resource);
        const auto* found=keys_.find(StringView{key});return found?*found:0;
    }
private:
    View view_{View::details};
    bool engine_{true}, has_project_{}, can_create_{};
    unsigned filter_{};
    std::wstring query_;
    std::filesystem::path directory_;
    struct Location { bool engine{};std::filesystem::path directory;bool operator==(const Location&) const = default; };
    std::vector<Location> navigation_;
    std::size_t navigation_cursor_{};
    std::optional<Item> selected_;
    struct Target { ui::ControlKey key; Item item; };
    std::vector<Target> targets_;
    Map<String,ui::ControlKey> keys_;
    ui::ControlKey next_key_{browser_control::root+1000};
    std::vector<Item> items_;
    std::unordered_map<ui::TableKey,std::size_t> row_indices_;
    ui::TableSort sort_{browser_control::name_column,ui::TableSortDirection::ascending};
    ui::TableKey active_column_{browser_control::name_column};
    std::array<float,3> column_widths_{220,90,84};
    double directory_width_{144};
    std::optional<double> directory_resize_;
    std::shared_ptr<const ui::TableData> table_data_;
    bool table_dirty_{true};
    std::vector<Item> directories_;
    std::shared_ptr<const std::vector<ui_language::Item>> directory_items_;
    bool directory_items_dirty_{true};
    std::filesystem::path project_root_;
    std::uint64_t directory_revision_{},tree_frame_{};
    std::unordered_map<ui::ControlKey,bool> tree_expansion_;
    std::unordered_set<ui::ControlKey> directory_keys_{browser_control::project,browser_control::engine};
    std::uint64_t catalog_revision_{};
    bool items_dirty_{true};
    ui::ControlKey key_for(const std::string&);
    void visit(Location,bool remember=true);
    void update_directories(const Project*);
    const std::shared_ptr<const std::vector<ui_language::Item>>& directory_items();
    void update_items(const Project*);
    void sort_items();
    bool select_item(ui::TableKey);
    const std::shared_ptr<const ui::TableData>& table_data();
    void invalidate_items() { items_dirty_=table_dirty_=true; }
};
}
