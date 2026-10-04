#pragma once
#include "editor/project.hpp"
#include "foundation/map.hpp"
#include "ui/controls.hpp"

namespace atelier::editor {
namespace browser_control {
inline constexpr ui::ControlKey root=1ULL<<44, project=root+1, engine=root+2, up=root+3,
    path=root+4, search=root+5, clear=root+6, filter=root+7, list=root+8, details=root+9, tiles=root+10,
    open=root+11, add=root+12, filter_first=root+20;
}
class AssetBrowser {
public:
    struct Item { std::string key; std::filesystem::path path; std::wstring name; AssetKind kind{}; std::uintmax_t bytes{}; };
    enum class View { list, details, tiles };
    void reset(bool has_project);
    void append(std::vector<ui::Control>&,ui::ControlKey parent,const Project*,bool can_create,const ui::UiFrame*);
    bool apply(const ui::Action&);
    std::vector<ui::MenuItem> filter_menu() const;
    const std::optional<Item>& selected() const noexcept { return selected_; }
    const std::filesystem::path& directory() const noexcept { return directory_; }
    void navigate(std::filesystem::path path) { directory_=std::move(path); query_.clear(); selected_.reset(); invalidate_items(); }
    bool engine() const noexcept { return engine_; }
    ui::ControlKey control(std::string_view resource) const {
        const auto key="item:"+std::string(resource);
        const auto* found=keys_.find(StringView{key});return found?*found:0;
    }
private:
    View view_{View::details};
    bool engine_{true};
    unsigned filter_{};
    std::wstring query_;
    std::filesystem::path directory_{L"Content"};
    std::optional<Item> selected_;
    struct Target { ui::ControlKey key; Item item; bool navigation{}; };
    std::vector<Target> targets_;
    Map<String,ui::ControlKey> keys_;
    ui::ControlKey next_key_{browser_control::root+1000};
    ui::ControlKey scroll_key_{browser_control::root+99};
    std::vector<Item> items_;
    std::uint64_t catalog_revision_{};
    bool items_dirty_{true};
    ui_layout::VirtualList rows_{10000};
    float row_height_{};
    void invalidate_items() { items_dirty_=true;scroll_key_=next_key_++; }
};
}
