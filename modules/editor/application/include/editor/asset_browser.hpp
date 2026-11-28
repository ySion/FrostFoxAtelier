#pragma once
#include "editor/project.hpp"
#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "ui/controls.hpp"
#include <array>
#include <memory>

namespace atelier::ui_language {
class LiveDocument;

class ControlOutput;

struct Item;

struct ItemList;
}

namespace atelier::editor {
namespace browser_control {
inline constexpr ui::ControlKey root = 1ULL << 44;

inline constexpr ui::ControlKey project = root + 1;

inline constexpr ui::ControlKey engine = root + 2;

inline constexpr ui::ControlKey up = root + 3;

inline constexpr ui::ControlKey path = root + 4;

inline constexpr ui::ControlKey search = root + 5;

inline constexpr ui::ControlKey clear = root + 6;

inline constexpr ui::ControlKey filter = root + 7;

inline constexpr ui::ControlKey list = root + 8;

inline constexpr ui::ControlKey details = root + 9;

inline constexpr ui::ControlKey tiles = root + 10;

inline constexpr ui::ControlKey open = root + 11;

inline constexpr ui::ControlKey add = root + 12;

inline constexpr ui::ControlKey view_menu = root + 13;

inline constexpr ui::ControlKey actions = root + 14;

inline constexpr ui::ControlKey back = root + 15;

inline constexpr ui::ControlKey directories = root + 16;

inline constexpr ui::ControlKey results = root + 17;

inline constexpr ui::ControlKey content = root + 18;

inline constexpr ui::ControlKey divider = root + 19;

inline constexpr ui::ControlKey filter_first = root + 20;

inline constexpr ui::ControlKey rename = root + 40;

inline constexpr ui::TableKey name_column = 1;

inline constexpr ui::TableKey type_column = 2;

inline constexpr ui::TableKey size_column = 3;
}

class AssetBrowser {
public:
    struct Item {
        String key;

        std::filesystem::path path;

        std::wstring name;

        assets::AssetKind kind{};

        std::uintmax_t bytes{};
    };

    enum class View { list, details, tiles };

    void reset(bool has_project);

    [[nodiscard]] ui::Result<void> append(ui_language::ControlOutput, ui::ControlKey parent, const Project*,
                                          bool can_create, const ui::UiFrame*, ui_language::LiveDocument&);

    bool apply(const ui::Action&);

    Array<ui::MenuItem> menu_items(ui::ControlKey source) const;

    const Optional<Item>& selected() const noexcept { return selected_; }

    // Project files and folders below the top-level roots; Engine resources are read-only.
    bool can_rename() const noexcept {
        return !engine_ && selected_ && !selected_->path.empty() &&
               std::distance(selected_->path.begin(), selected_->path.end()) >= 2;
    }

    void reselect(const std::filesystem::path& path) {
        selected_.reset();
        selected_path_ = path;
        invalidate_items();
    }

    const std::filesystem::path& directory() const noexcept { return directory_; }

    void navigate(std::filesystem::path path);

    bool engine() const noexcept { return engine_; }

    ui::ControlKey directory_control(const std::filesystem::path& path) const;

    ui::TableKey row_key(StringView resource) const {
        if (!resource.is_valid_utf8())
            return 0;
        const auto key = String::concat("item:", String::validated(resource));
        const auto* found = keys_.find(key.view());
        return found ? *found : 0;
    }

    // Directory rows' expansion is view state. The empty path is Project; every
    // path must be a project directory row, otherwise nothing changes.
    Result<void> set_expanded(Span<const std::filesystem::path>, bool expanded);

private:
    View view_{View::details};

    bool engine_{true};

    bool has_project_{};

    bool can_create_{};

    unsigned filter_{};

    std::wstring query_;

    std::filesystem::path directory_;

    struct Location {
        bool engine{};

        std::filesystem::path directory;

        bool operator==(const Location&) const = default;
    };

    Array<Location> navigation_;

    std::size_t navigation_cursor_{};

    Optional<Item> selected_;

    Optional<std::filesystem::path> selected_path_; // Restored once the catalog reflects a rename.

    struct Target {
        ui::ControlKey key;

        Item item;
    };

    Array<Target> targets_;

    Map<String, ui::ControlKey> keys_;

    ui::ControlKey next_key_{browser_control::root + 1000};

    Array<Item> items_;

    Map<ui::TableKey, std::size_t> row_indices_;

    ui::TableSort sort_{browser_control::name_column, ui::TableSortDirection::ascending};

    ui::TableKey active_column_{browser_control::name_column};

    std::array<float, 3> column_widths_{220, 90, 84};

    double directory_width_{144};

    Optional<double> directory_resize_;

    std::shared_ptr<const ui::TableData> table_data_;

    bool table_dirty_{true};

    Array<Item> directories_;

    std::shared_ptr<const Array<ui_language::Item>> directory_items_;

    std::size_t directory_rows_{};

    bool directory_items_dirty_{true};

    std::filesystem::path project_root_;

    std::uint64_t directory_revision_{};

    Map<std::filesystem::path, bool>
        expansion_; // Overrides; Project starts expanded, directories collapsed unless they first appear holding the current directory.

    std::uint64_t catalog_revision_{};

    bool items_dirty_{true};

    ui::ControlKey key_for(String);

    void visit(Location, bool remember = true);

    void update_directories(const Project*);

    ui::Result<ui_language::ItemList> directory_items();

    void update_items(const Project*);

    void sort_items();

    bool select_item(ui::TableKey);

    const std::shared_ptr<const ui::TableData>& table_data();

    void invalidate_items() { items_dirty_ = table_dirty_ = true; }
};
}
