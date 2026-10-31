#include "editor/asset_browser.hpp"
#include "editor/commands.hpp"
#include "ui_language/live_document.hpp"
#include "assets/file.hpp"
#include "assets/builtin_materials.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace atelier::editor {
namespace {
std::wstring_view kind_name(assets::AssetKind kind) {
    switch (kind) {
    case assets::AssetKind::folder:
        return L"Folder";
    case assets::AssetKind::mesh:
        return L"Mesh";
    case assets::AssetKind::scene:
        return L"Scene";
    case assets::AssetKind::source:
        return L"Source";
    case assets::AssetKind::texture:
        return L"Texture";
    case assets::AssetKind::material:
        return L"Material";
    default:
        return L"File";
    }
}
std::wstring folded(std::wstring text) {
    for (auto& c : text)
        c = static_cast<wchar_t>(std::towlower(c));
    return text;
}
bool matches_filter(const AssetBrowser::Item& item, std::wstring_view query, unsigned filter) {
    return (query.empty() || folded(item.name).find(query) != std::wstring::npos) &&
           (!filter || item.kind == assets::AssetKind::folder ||
            item.kind == (filter == 1   ? assets::AssetKind::mesh
                          : filter == 2 ? assets::AssetKind::scene
                          : filter == 3 ? assets::AssetKind::source
                          : filter == 4 ? assets::AssetKind::material
                                        : assets::AssetKind::texture));
}
int compare_names(std::wstring_view a, std::wstring_view b) {
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        const auto left = std::towlower(a[i]), right = std::towlower(b[i]);
        if (left != right)
            return left < right ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}
// Directory lists preserve the Project scan's path order, independently of the result table's sorting.
bool listed(const std::vector<AssetBrowser::Item>& directories, const std::filesystem::path& path) {
    return std::ranges::binary_search(directories, path, {}, &AssetBrowser::Item::path);
}
constexpr std::array column_minima{96.f, 64.f, 64.f};
std::optional<std::size_t> column_index(ui::TableKey key) {
    if (key >= browser_control::name_column && key <= browser_control::size_column)
        return static_cast<std::size_t>(key - browser_control::name_column);
    return std::nullopt;
}
ui::ControlIcon item_icon(assets::AssetKind kind) {
    return kind == assets::AssetKind::folder ? ui::ControlIcon::folder
           : kind == assets::AssetKind::mesh ? ui::ControlIcon::mesh
                                             : ui::ControlIcon::object;
}
}
void AssetBrowser::reset(bool has_project) {
    if (directory_resize_) {
        directory_width_ = *directory_resize_;
        directory_resize_.reset();
    }
    engine_ = !has_project;
    has_project_ = has_project;
    directory_ = has_project ? std::filesystem::path{L"Content"} : std::filesystem::path{};
    query_.clear();
    filter_ = 0;
    selected_.reset();
    targets_.clear();
    keys_.clear();
    row_indices_.clear();
    directories_.clear();
    project_root_.clear();
    table_data_.reset();
    active_column_ = browser_control::name_column;
    directory_items_.reset();
    directory_items_dirty_ = true;
    directory_rows_ = 0;
    navigation_ = {{engine_, directory_}};
    navigation_cursor_ = 0;
    catalog_revision_ = directory_revision_ = 0;
    expansion_.clear();
    invalidate_items(); // Keys remain monotonic.
}
ui::ControlKey AssetBrowser::key_for(const std::string& value) {
    const auto [key, inserted] = keys_.try_emplace(StringView{value}, next_key_);
    if (inserted)
        ++next_key_;
    return *key;
}
ui::ControlKey AssetBrowser::directory_control(const std::filesystem::path& path) const {
    if (path.empty())
        return browser_control::project;
    const auto name = "nav:" + assets::utf8(path.generic_wstring());
    const auto* key = keys_.find(StringView{name});
    return key ? *key : 0;
}
void AssetBrowser::visit(Location location, bool remember) {
    if (location.engine)
        location.directory.clear();
    else {
        location.directory = location.directory.lexically_normal();
        if (location.directory == L".")
            location.directory.clear();
    }
    if (location == Location{engine_, directory_})
        return;
    engine_ = location.engine;
    directory_ = location.directory;
    query_.clear();
    selected_.reset();
    directory_items_dirty_ = true;
    // The visited directory's row is shown: Project and every ancestor expand.
    if (!engine_)
        for (auto ancestor = directory_; !ancestor.empty();) {
            ancestor = ancestor.parent_path();
            expansion_[ancestor] = true;
        }
    if (navigation_.empty()) {
        navigation_.push_back(location);
        navigation_cursor_ = 0;
    } else if (remember) {
        navigation_.resize(navigation_cursor_ + 1);
        navigation_.push_back(location);
        if (navigation_.size() > 64)
            navigation_.erase(navigation_.begin());
        navigation_cursor_ = navigation_.size() - 1;
    } else
        navigation_[navigation_cursor_] = location;
    invalidate_items();
}
void AssetBrowser::navigate(std::filesystem::path path) {
    visit({false, std::move(path)});
}
void AssetBrowser::update_directories(const Project* project) {
    const auto root = project ? project->root() : std::filesystem::path{};
    const auto revision = project ? project->revision() : 0;
    if (root == project_root_ && revision == directory_revision_ && has_project_ == (project != nullptr))
        return;
    has_project_ = project != nullptr;
    project_root_ = root;
    directory_revision_ = revision;
    directory_items_dirty_ = true;
    const auto previous = std::exchange(directories_, {});
    targets_.clear();
    if (project)
        for (const auto& entry : project->entries())
            if (entry.kind == assets::AssetKind::folder) {
                directories_.push_back({entry.key, entry.path, entry.path.filename().wstring(), entry.kind, 0});
                targets_.push_back({key_for("nav:" + assets::utf8(entry.path.generic_wstring())), directories_.back()});
                // A directory first appears expanded when it is or holds the current directory.
                if (!engine_ && !listed(previous, entry.path))
                    if (const auto relative = directory_.lexically_relative(entry.path);
                        !relative.empty() && *relative.begin() != L"..")
                        expansion_.try_emplace(entry.path, true);
            }
    std::erase_if(expansion_,
                  [&](const auto& entry) { return !entry.first.empty() && !listed(directories_, entry.first); });
    const auto valid = [&](const Location& location) {
        return location.engine ||
               (has_project_ && (location.directory.empty() || listed(directories_, location.directory)));
    };
    if (!valid({engine_, directory_}))
        visit({!has_project_, {}}, false);
    std::size_t destination{}, cursor{};
    for (std::size_t i = 0; i < navigation_.size(); ++i)
        if (valid(navigation_[i])) {
            if (i <= navigation_cursor_)
                cursor = destination;
            if (destination != i)
                navigation_[destination] = std::move(navigation_[i]);
            ++destination;
        }
    navigation_.resize(destination);
    navigation_cursor_ = cursor;
    invalidate_items();
}
const std::shared_ptr<const std::vector<ui_language::Item>>& AssetBrowser::directory_items() {
    if (!directory_items_dirty_ && directory_items_)
        return directory_items_;
    // Project, its directories in path (preorder) order, then Engine. Few rows: all are described.
    struct Node {
        ui::ControlKey key;
        std::wstring_view name;
        bool enabled, selected;
        ui::ControlIcon icon;
    };
    std::vector<Node> labels;
    labels.reserve(directories_.size() + 2);
    std::vector<ui::TreeNode> nodes;
    nodes.reserve(directories_.size() + 2);
    const auto expanded = [&](const std::filesystem::path& path, bool initial) {
        const auto at = expansion_.find(path);
        return at == expansion_.end() ? initial : at->second;
    };
    labels.push_back(
        {browser_control::project, L"Project", has_project_, !engine_ && directory_.empty(), ui::ControlIcon::folder});
    nodes.push_back({ui::no_tree_node, expanded({}, true), {}});
    std::vector<std::pair<const std::filesystem::path*, std::uint32_t>> path{{nullptr, 0}};
    for (const auto& entry : directories_) {
        const auto parent = entry.path.parent_path();
        while (path.size() > 1 && *path.back().first != parent)
            path.pop_back();
        labels.push_back({directory_control(entry.path), entry.name, true, !engine_ && entry.path == directory_,
                          ui::ControlIcon::folder});
        nodes.push_back({path.back().second, expanded(entry.path, false), {}});
        path.emplace_back(&entry.path, static_cast<std::uint32_t>(nodes.size() - 1));
    }
    labels.push_back({browser_control::engine, L"Engine", true, engine_, ui::ControlIcon::mesh});
    nodes.push_back({});
    std::vector<ui::TreeRow> rows;
    ui::flatten_tree(nodes, {}, rows);
    auto items = std::make_shared<std::vector<ui_language::Item>>();
    items->reserve(rows.size());
    for (std::uint32_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const auto& label = labels[row.node];
        items->push_back(
            {label.key,
             {std::wstring(label.name), label.enabled, label.selected, static_cast<double>(label.icon),
              ui_language::TreeRowBinding{index, row.depth, row.branch, row.expanded, label.selected, row.lines},
              std::wstring{}, false, ui::ControlKey{}}});
    }
    directory_rows_ = rows.size();
    directory_items_ = std::move(items);
    directory_items_dirty_ = false;
    return directory_items_;
}
Result<void> AssetBrowser::set_expanded(std::span<const std::filesystem::path> paths, bool expanded) {
    for (const auto& path : paths)
        if (path.empty() ? !has_project_ : !listed(directories_, path))
            return std::unexpected(
                Error{ErrorCode::invalid_member, "The path is not a directory row of the Asset Browser."});
    for (const auto& path : paths)
        expansion_[path] = expanded;
    directory_items_dirty_ = true;
    return {};
}
void AssetBrowser::sort_items() {
    std::sort(items_.begin(), items_.end(), [&](const auto& a, const auto& b) {
        if ((a.kind == assets::AssetKind::folder) != (b.kind == assets::AssetKind::folder))
            return a.kind == assets::AssetKind::folder;
        int order{};
        if (sort_.column == browser_control::size_column)
            order = a.bytes == b.bytes ? 0 : a.bytes < b.bytes ? -1 : 1;
        else if (sort_.column == browser_control::type_column)
            order = kind_name(a.kind).compare(kind_name(b.kind));
        else
            order = compare_names(a.name, b.name);
        if (order)
            return sort_.direction == ui::TableSortDirection::ascending ? order < 0 : order > 0;
        return a.key < b.key;
    });
    row_indices_.clear();
    row_indices_.reserve(items_.size());
    for (std::size_t i = 0; i < items_.size(); ++i)
        row_indices_.emplace(key_for("item:" + items_[i].key), i);
    table_dirty_ = true;
}
void AssetBrowser::update_items(const Project* project) {
    if (!items_dirty_ && catalog_revision_ == (project ? project->revision() : 0))
        return;
    items_.clear();
    if (engine_) {
        for (const auto& asset : assets::mesh_assets())
            items_.push_back({asset.key, {}, asset.name, assets::AssetKind::mesh, 0});
        for (const auto& asset : assets::material_assets())
            items_.push_back({asset.key, {}, asset.name, assets::AssetKind::material, 0});
    } else if (project)
        for (const auto& entry : project->entries())
            if (entry.path.parent_path() == directory_)
                items_.push_back({entry.key, entry.path, entry.path.filename().wstring(), entry.kind, entry.bytes});
    const auto query = folded(query_);
    std::erase_if(items_, [&](const auto& item) { return !matches_filter(item, query, filter_); });
    if (selected_) {
        const auto found = std::ranges::find(items_, selected_->key, &Item::key);
        if (found == items_.end())
            selected_.reset();
        else
            selected_ = *found;
    }
    sort_items();
    items_dirty_ = false;
    catalog_revision_ = project ? project->revision() : 0;
    if (selected_path_) {
        const auto at = std::ranges::find(items_, *selected_path_, &Item::path);
        if (at != items_.end())
            selected_ = *at;
        selected_path_.reset();
    }
}
bool AssetBrowser::select_item(ui::TableKey key) {
    const auto found = row_indices_.find(key);
    if (found == row_indices_.end())
        return false;
    const auto& item = items_[found->second];
    if (!matches_filter(item, folded(query_), filter_))
        return false;
    if (!selected_ || selected_->key != item.key)
        selected_ = item;
    return true;
}
const std::shared_ptr<const ui::TableData>& AssetBrowser::table_data() {
    if (!table_dirty_ && table_data_)
        return table_data_;
    auto data = std::make_shared<ui::TableData>();
    data->columns = {
        {browser_control::name_column, L"Name", column_widths_[0], column_minima[0]},
        {browser_control::type_column, L"Type", column_widths_[1], column_minima[1]},
        {browser_control::size_column, L"Size", column_widths_[2], column_minima[2], ui::TextAlign::right}};
    data->rows.reserve(items_.size());
    for (const auto& item : items_) {
        const auto row = row_key(item.key);
        const auto size = item.kind == assets::AssetKind::folder || item.path.empty() ? L""
                          : item.bytes < 1024 ? std::to_wstring(item.bytes) + L" B"
                                              : std::to_wstring(item.bytes / 1024 + (item.bytes % 1024 != 0)) + L" KB";
        data->rows.push_back({row,
                              {{(row << 2) | browser_control::name_column, item.name, item_icon(item.kind)},
                               {(row << 2) | browser_control::type_column, std::wstring(kind_name(item.kind))},
                               {(row << 2) | browser_control::size_column, size}}});
    }
    data->sort = sort_;
    table_data_ = std::move(data);
    table_dirty_ = false;
    return table_data_;
}
std::vector<ui::MenuItem> AssetBrowser::menu_items(ui::ControlKey source) const {
    namespace id = browser_control;
    std::vector<ui::MenuItem> items;
    const auto radio = [&](ui::ControlKey key, std::wstring title, bool enabled, bool selected,
                           ui::ControlIcon icon = ui::ControlIcon::none) {
        ui::MenuItem item{key, std::move(title), enabled, selected, {}};
        item.behavior = ui::MenuBehavior::radio;
        item.radio_group = source;
        item.icon = icon;
        items.push_back(std::move(item));
    };
    if (source == id::filter) {
        const std::array names{L"All types", L"Meshes", L"Scenes", L"Source", L"Materials", L"Textures"};
        for (std::size_t i = 0; i < names.size(); ++i)
            radio(id::filter_first + i, names[i], true, filter_ == i);
    } else if (source == id::view_menu) {
        radio(id::list, L"List", true, view_ == View::list, ui::ControlIcon::view_list);
        radio(id::details, L"Details", true, view_ == View::details, ui::ControlIcon::view_details);
        radio(id::tiles, L"Tiles", true, view_ == View::tiles, ui::ControlIcon::view_tiles);
    } else if (source == id::path) {
        radio(id::project, L"Project", has_project_, !engine_ && directory_.empty(), ui::ControlIcon::folder);
        if (!engine_) {
            std::filesystem::path ancestor;
            for (const auto& part : directory_) {
                ancestor /= part;
                if (const auto key = directory_control(ancestor))
                    radio(key, ancestor.generic_wstring(), true, ancestor == directory_, ui::ControlIcon::folder);
            }
        }
        radio(id::engine, L"Engine", true, engine_, ui::ControlIcon::mesh);
    } else {
        if (source != id::actions && source != id::results)
            return {};
        items.push_back(
            {id::open,
             L"Open",
             selected_ &&
                 (selected_->kind == assets::AssetKind::folder || selected_->kind == assets::AssetKind::scene ||
                  selected_->kind == assets::AssetKind::source || selected_->kind == assets::AssetKind::material),
             false,
             {}});
        items.back().icon = ui::ControlIcon::folder;
        items.push_back({id::add,
                         L"Add to Scene",
                         can_create_ && selected_ && selected_->kind == assets::AssetKind::mesh,
                         false,
                         {}});
        items.back().icon = ui::ControlIcon::plus;
        items.push_back({id::rename, L"Rename", can_rename(), false, {}, L"F2"});
        items.back().icon = ui::ControlIcon::rename;
        items.push_back({static_cast<ui::ControlKey>(Command::reimport_texture),
                         L"Reimport Texture From...",
                         has_project_ && !engine_ && selected_ && selected_->kind == assets::AssetKind::texture,
                         false,
                         {}});
        items.push_back({static_cast<ui::ControlKey>(Command::reimport_mesh),
                         L"Reimport Mesh From...",
                         has_project_ && !engine_ && selected_ && selected_->kind == assets::AssetKind::mesh,
                         false,
                         {}});
        for (const auto command : {Command::import_mesh, Command::import_texture, Command::create_material_source,
                                   Command::create_material, Command::refresh_assets}) {
            const auto* definition = command_definition(static_cast<ui::ControlKey>(command));
            items.push_back({static_cast<ui::ControlKey>(command),
                             std::wstring(definition->title),
                             has_project_,
                             false,
                             {},
                             std::wstring(definition->shortcut),
                             command == Command::import_mesh});
        }
    }
    return items;
}
bool AssetBrowser::apply(const ui::Action& action) {
    using namespace browser_control;
    if (action.key == divider) {
        if (action.kind == ui::ActionKind::edit_begin && !directory_resize_)
            directory_resize_ = directory_width_;
        else if (action.kind == ui::ActionKind::edit_cancel && directory_resize_) {
            directory_width_ = *directory_resize_;
            directory_resize_.reset();
        } else if (directory_resize_ &&
                   (action.kind == ui::ActionKind::edit_preview || action.kind == ui::ActionKind::edit_commit)) {
            if (const auto* value = std::get_if<double>(&action.value); value && std::isfinite(*value))
                directory_width_ = std::max(96., *value);
            if (action.kind == ui::ActionKind::edit_commit)
                directory_resize_.reset();
        }
        return true;
    }
    const auto filter_changed = [&] {
        if (selected_ && !matches_filter(*selected_, folded(query_), filter_))
            selected_.reset();
        invalidate_items();
    };
    if (action.key == search) {
        if (const auto* value = std::get_if<std::wstring>(&action.value); value && query_ != *value) {
            query_ = *value;
            filter_changed();
        }
        return true;
    }
    if (action.key == results) {
        if (action.kind == ui::ActionKind::select) {
            if (const auto* value = std::get_if<ui::TableSelection>(&action.value);
                value && column_index(value->column)) {
                if (!value->row)
                    selected_.reset();
                else if (!select_item(value->row))
                    return true;
                if (view_ == View::details)
                    active_column_ = value->column;
            }
            return true;
        }
        if (action.kind == ui::ActionKind::table_sort) {
            if (const auto* value = std::get_if<ui::TableSort>(&action.value);
                value && column_index(value->column) && sort_ != *value) {
                sort_ = *value;
                sort_items();
            }
            return true;
        }
        if (action.kind == ui::ActionKind::table_column_resize) {
            if (const auto* value = std::get_if<ui::TableColumnResize>(&action.value);
                value && std::isfinite(value->width)) {
                if (const auto index = column_index(value->column)) {
                    const auto width = std::max(value->width, column_minima[*index]);
                    if (column_widths_[*index] != width) {
                        column_widths_[*index] = width;
                        table_dirty_ = true;
                    }
                }
            }
            return true;
        }
    }
    if (action.kind == ui::ActionKind::expand) {
        const auto* expanded = std::get_if<bool>(&action.value);
        if (!expanded)
            return false;
        if (action.key == project) {
            static_cast<void>(set_expanded(std::array{std::filesystem::path{}}, *expanded));
            return true;
        }
        for (const auto& target : targets_)
            if (target.key == action.key) {
                static_cast<void>(set_expanded(std::span{&target.item.path, 1}, *expanded));
                return true;
            }
        return false;
    }
    if (action.kind != ui::ActionKind::activate && action.kind != ui::ActionKind::select)
        return false;
    if (action.key == project) {
        if (has_project_)
            visit({false, {}});
        return true;
    }
    if (action.key == browser_control::engine) {
        visit({true, {}});
        return true;
    }
    for (const auto& target : targets_)
        if (target.key == action.key) {
            navigate(target.item.path);
            return true;
        }
    if (action.kind != ui::ActionKind::activate)
        return false;
    if (action.key >= filter_first && action.key < filter_first + 6) {
        filter_ = static_cast<unsigned>(action.key - filter_first);
        filter_changed();
        return true;
    }
    switch (action.key) {
    case back:
        if (navigation_cursor_) {
            const auto location = navigation_[--navigation_cursor_];
            visit(location, false);
        }
        return true;
    case up:
        if (!engine_ && !directory_.empty())
            navigate(directory_.parent_path());
        return true;
    case clear:
        query_.clear();
        filter_ = 0;
        filter_changed();
        return true;
    case list:
        view_ = View::list;
        return true;
    case details:
        view_ = View::details;
        return true;
    case tiles:
        view_ = View::tiles;
        return true;
    default:
        break;
    }
    return false;
}
void AssetBrowser::append(std::vector<ui::Control>& controls, ui::ControlKey parent, const Project* project,
                          bool can_create, const ui::UiFrame* frame, ui_language::LiveDocument& document) {
    namespace id = browser_control;
    update_directories(project);
    can_create_ = can_create;
    if (navigation_.empty())
        navigation_.push_back({engine_, directory_});
    update_items(project);
    const auto address = engine_              ? L"Engine"
                         : directory_.empty() ? L"Project"
                                              : L"Project / " + directory_.generic_wstring();
    const auto count = items_.empty() ? (query_.empty() && !filter_ ? L"This folder is empty." : L"No matches.")
                                      : std::to_wstring(items_.size()) + (items_.size() == 1 ? L" item" : L" items");
    const auto* body = frame ? frame->find(id::root + 103) : nullptr;
    const double directory_limit =
        body ? std::max(96., static_cast<double>(body->bounds.width / frame->drawing->dpi_scale) - 69.) : 144.;
    const std::array<ui_language::InputValue, 18> inputs{
        ui_language::ItemList{directory_items()},
        static_cast<double>(directory_rows_),
        ui_language::TextEditBinding{id::search, query_},
        address,
        count + (selected_ ? L"  |  " + selected_->name : L""),
        ui_language::ActionBinding{id::back, navigation_cursor_ != 0},
        ui_language::ActionBinding{id::up, !engine_ && !directory_.empty()},
        ui_language::ActionBinding{id::clear, !query_.empty() || filter_ != 0},
        filter_ != 0,
        static_cast<double>(view_ == View::list      ? ui::ControlIcon::view_list
                            : view_ == View::details ? ui::ControlIcon::view_details
                                                     : ui::ControlIcon::view_tiles),
        static_cast<double>(ui::ControlIcon::back),
        static_cast<double>(ui::ControlIcon::up),
        static_cast<double>(ui::ControlIcon::search),
        static_cast<double>(ui::ControlIcon::close),
        static_cast<double>(ui::ControlIcon::filter),
        static_cast<double>(ui::ControlIcon::more),
        std::min(directory_width_, directory_limit),
        directory_limit};
    const std::array<ui_language::KeyBinding, 15> keys{{{"toolbar", id::root + 100},
                                                        {"back", id::back},
                                                        {"up", id::up},
                                                        {"path", id::path},
                                                        {"search_area", id::root + 101},
                                                        {"search_icon", id::root + 102},
                                                        {"clear", id::clear},
                                                        {"filter", id::filter},
                                                        {"view", id::view_menu},
                                                        {"actions", id::actions},
                                                        {"body", id::root + 103},
                                                        {"tree", id::directories},
                                                        {"divider", id::divider},
                                                        {"content", id::content},
                                                        {"status", id::root + 104}}};
    static_cast<void>(document.append("AssetBrowserShell", parent, keys, inputs, controls));
    const std::array<ui_language::InputValue, 5> results{
        ui_language::TableBinding{table_data(), {selected_ ? row_key(selected_->key) : 0, active_column_}},
        ui_language::ActionBinding{id::open}, id::results,
        std::wstring(view_ == View::details ? L"details"
                     : view_ == View::tiles ? L"tiles"
                                            : L"list"),
        std::wstring(query_.empty() && !filter_ ? L"This folder is empty."
                                                : L"No matches. Clear the search or type filter.")};
    const std::array<ui_language::KeyBinding, 1> result_keys{{{"results", id::results}}};
    static_cast<void>(document.append("AssetResults", id::content, result_keys, results, controls));
}
}
