#pragma once
#include "ui/controls.hpp"
#include <memory>

namespace atelier::ui {
enum class DockSide { tabs, left, right, top, bottom };

struct DockPlacement {
    ControlKey tab;
    ControlKey target;
    DockSide side;
    float fraction{.5f};
};

struct DockTab {
    ControlKey key{};

    ControlKey content{};

    std::wstring title;

    ui_layout::Size minimum{220, 140}; // Includes pane chrome; shrinks only if the host is smaller.

    Array<std::string> classes{};
};

// Single-host split tree. Owns geometry and tab placement, never the views' state.
// UI Session owns every gesture/capture. apply() consumes its accepted-frame actions.
class DockSpace {
public:
    // Fixed registry: 1..64 views, nonzero base with room for base+5'000'256.
    // Each key/content is nonzero, globally distinct and outside that closed range.
    // Titles are nonempty; finite minimum sizes are at least 120x60.
    // Empty/oversized registries and overflowing key ranges are fatal; other violations assert.
    DockSpace(ControlKey key_base, Array<DockTab>);

    ~DockSpace();

    DockSpace(const DockSpace&) = delete;

    DockSpace& operator=(const DockSpace&) = delete;

    // Placements must be legal moves from the initial group of all tabs. Check their full key budget before resetting.
    Result<void> reset(Span<const DockPlacement> placements = {});

    Result<bool> move(ControlKey tab, ControlKey target_tab, DockSide, float fraction = .5f);

    bool select(ControlKey);

    bool close(ControlKey);

    void maximize(ControlKey); // Toggle display isolation, preserving the tree and ratios.

    void cycle(int direction);

    static ControlKey add_tab_button(ControlKey pane) { return pane + 11; }

    static ControlKey collapse_first_button(ControlKey divider) { return divider + 11; }

    static ControlKey collapse_second_button(ControlKey divider) { return divider + 12; }

    static ControlKey divider_grip(ControlKey divider) { return divider + 13; }

    bool visible(ControlKey) const noexcept;

    ControlKey focused_tab() const noexcept { return focused_; }

    ControlKey pane(ControlKey tab) const noexcept;

    ControlKey divider(ControlKey tab, DragAxis axis) const noexcept;

    static ControlKey maximize_button(ControlKey pane) { return pane + 3; }

    static ControlKey menu_button(ControlKey pane) { return pane + 4; }

    void focus_control(ControlKey, const UiFrame&);

    bool focus_point(float x, float y, const UiFrame&) noexcept;

    Result<bool> apply(const Action&, const UiFrame&);

    Array<MenuItem> menu_items(ControlKey source, const UiFrame*);

    struct PresentedTab {
        const DockTab* view{};

        ControlKey wrapper{};

        ControlKey close{};
    };

    struct Pane {
        ControlKey key{};

        ControlKey parent{};

        Rect bounds;

        bool split{};

        bool horizontal{};

        bool maximized{};

        unsigned collapsed{};

        double value{.5};

        double minimum{.5};

        double maximum{.5};

        double step{1};

        Rect divider;

        Array<PresentedTab> tabs;

        const DockTab* active{};
    };

    struct Presentation {
        Array<Pane> panes;

        ControlKey preview_key{};

        Optional<Rect> preview;
    };

    // Logical placement, bounded gestures and host slots. All chrome is authored
    // by the consumer; this model no longer assembles controls or styles.
    Presentation presentation(ControlKey parent, ui_layout::Size) const;

    bool dragging() const noexcept { return drag_.has_value() || resize_.has_value(); }

private:
    struct Node {
        ControlKey key{};

        Array<ControlKey> tabs;

        ControlKey active{};

        DragAxis axis{};

        float fraction{.5f};

        unsigned collapsed{}; // 0 none, 1 first, 2 second; fraction always retains expanded geometry.

        std::unique_ptr<Node> first;

        std::unique_ptr<Node> second;

        bool split() const noexcept { return first != nullptr; }
    };

    struct Drop {
        ControlKey pane{};

        ControlKey before{};

        DockSide side{};

        Rect preview{};
    };

    struct Drag {
        ControlKey tab{};

        Optional<Drop> target;
    };

    struct Resize {
        ControlKey source{};

        Rect preview;
    };

    ControlKey base_{};

    ControlKey next_{};

    ControlKey focused_{};

    ControlKey maximized_{};

    ControlKey menu_pane_{};

    bool menu_add_{};

    bool menu_type_{};

    Array<ControlKey> closed_tabs_;

    Array<DockTab> tabs_;

    std::unique_ptr<Node> root_;

    Optional<Drag> drag_;

    Optional<Resize> resize_; // Preview only; accepted pane geometry changes once at release.

    Result<void> require_nodes(std::size_t count) const;

    std::unique_ptr<Node> make_node();

    const DockTab* tab(ControlKey) const noexcept;

    Node* find(ControlKey) const noexcept;

    Node* owner(ControlKey) const noexcept;

    Node* parent(ControlKey) const noexcept;

    void reveal(ControlKey);

    void collapse(Node&, unsigned);

    void normalize(std::unique_ptr<Node>&);

    Result<bool> place(ControlKey, ControlKey, DockSide, ControlKey before = 0, float fraction = .5f);

    void remove(Node&, ControlKey);

    ui_layout::Size minimum(const Node&) const;

    Optional<Drop> destination(ControlKey, const DragPoint&, const UiFrame&) const;

    void present_node(Presentation&, const Node&, ControlKey, Rect) const;
};
} // namespace atelier::ui
