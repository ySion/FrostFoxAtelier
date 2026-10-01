#pragma once
#include "ui/controls.hpp"
#include <functional>
#include <memory>

namespace atelier::ui {
enum class DockSide { tabs, left, right, top, bottom };
struct DockTab {
    ControlKey key{}, content{};
    std::wstring title;
    ui_layout::Size minimum{220,140}; // Includes pane chrome; shrinks only if the host is smaller.
};
// Single-host split tree. Owns geometry and tab placement, never the views' state.
// UI Session owns every gesture/capture. apply() consumes its accepted-frame actions.
class DockSpace {
public:
    DockSpace(ControlKey key_base, std::vector<DockTab>);
    ~DockSpace();
    DockSpace(const DockSpace&) = delete;
    DockSpace& operator=(const DockSpace&) = delete;
    void reset(); // All registered views reopen in one group; host can dock its default arrangement.
    bool move(ControlKey tab, ControlKey target_tab, DockSide, float fraction=.5f);
    bool select(ControlKey);
    bool close(ControlKey);
    void maximize(ControlKey); // Toggle display isolation, preserving the tree and ratios.
    void cycle(int direction);
    bool visible(ControlKey) const noexcept;
    ControlKey focused_tab() const noexcept { return focused_; }
    ControlKey pane(ControlKey tab) const noexcept;
    ControlKey divider(ControlKey tab, DragAxis axis) const noexcept;
    static ControlKey maximize_button(ControlKey pane) { return pane+3; }
    static ControlKey menu_button(ControlKey pane) { return pane+4; }
    void focus_control(ControlKey, const UiFrame&);
    bool apply(const Action&, const UiFrame&);
    std::vector<MenuItem> menu_items(ControlKey source,const UiFrame*);
    using Describe = std::function<void(const DockTab&,std::vector<Control>&,ControlKey)>;
    void append(std::vector<Control>&, ControlKey parent, ui_layout::Size, const Describe&);
    bool dragging() const noexcept { return drag_.has_value(); }
private:
    struct Node {
        ControlKey key{};
        std::vector<ControlKey> tabs;
        ControlKey active{};
        DragAxis axis{};
        float fraction{.5f};
        std::unique_ptr<Node> first, second;
        bool split() const noexcept { return first!=nullptr; }
    };
    struct Drop { ControlKey pane{}, before{}; DockSide side{}; Rect preview{}; };
    struct Drag { ControlKey tab{}; std::optional<Drop> target; };
    ControlKey base_{}, next_{}, focused_{}, maximized_{}, menu_pane_{};
    std::vector<DockTab> tabs_;
    std::unique_ptr<Node> root_;
    std::optional<Drag> drag_;
    std::unique_ptr<Node> make_node();
    const DockTab* tab(ControlKey) const noexcept;
    Node* find(ControlKey) const noexcept;
    Node* owner(ControlKey) const noexcept;
    void normalize(std::unique_ptr<Node>&);
    bool place(ControlKey,ControlKey,DockSide,ControlKey before=0,float fraction=.5f);
    void remove(Node&,ControlKey);
    ui_layout::Size minimum(const Node&) const;
    std::optional<Drop> destination(ControlKey,const DragPoint&,const UiFrame&) const;
    void append_node(std::vector<Control>&,const Node&,ControlKey,ui_layout::Size,const Describe&);
};
} // namespace atelier::ui
