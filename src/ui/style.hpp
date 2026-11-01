#pragma once

#include "ui/frame.hpp"
#include "ui_layout/layout.hpp"
#include <string>

namespace atelier::ui {

enum class ControlIcon {
    none,
    close,
    maximize,
    restore,
    left,
    right,
    up,
    down,
    grip_horizontal,
    grip_vertical,
    plus,
    back,
    forward,
    world,
    object,
    axes,
    component,
    mesh,
    folder,
    search,
    view_list,
    view_details,
    view_tiles,
    filter,
    more,
    pointer,
    translate,
    rotate,
    scale,
    cursor3d,
    magnet,
    fly,
    point_light,
    area_light,
    sun_light,
    sky_light,
    shade_wire,
    shade_solid,
    shade_matcap,
    shade_lit,
    overlays,
    select_new,
    select_extend,
    select_subtract,
    select_invert,
    select_intersect,
    grid,
    axes_origin,
    wire_overlay,
    lights_off,
    fps,
    color_picker,
    rename,
    duplicate,
    trash,
    unparent
};
inline constexpr ControlIcon last_control_icon = ControlIcon::unparent;
enum class Role { normal, panel, title, caption };
enum class IconPlacement { leading, above };
enum class TextAlign { left, center, right };
enum class TextVerticalAlign { top, center, bottom };
enum class BorderStyle { solid, inset, outset };
enum class ScrollbarGutter { stable, overlay };
enum class BorderEdges : unsigned { none = 0, top = 1, right = 2, bottom = 4, left = 8, all = 15 };
constexpr bool has_edge(BorderEdges edges, BorderEdges edge) {
    return (static_cast<unsigned>(edges) & static_cast<unsigned>(edge)) != 0;
}
enum class ComboIndicator { down, arrows, caret, none }; // none: a flat text menu (Blender header).
enum class FontStyle { normal, italic };
enum class ControlKind {
    container,
    label,
    button,
    list,
    item,
    number,
    splitter,
    viewport,
    text_edit,
    checkbox,
    foldout,
    popup,
    menu_item,
    combo,
    tab,
    slider,
    tree,
    tree_item,
    table,
    code_editor
};
struct Font {
    std::wstring family{L"Microsoft YaHei UI"};
    std::wstring locale{L"en-US"};
    float size{14}; // DIPs. Role scales preserve the typography hierarchy.
    std::uint16_t weight{400};
    FontStyle style{FontStyle::normal};
    bool operator==(const Font&) const = default;
};
struct Palette {
    Color text{.76f, .79f, .83f, 1};
    Color muted{.34f, .37f, .42f, 1};
    Color panel{.032f, .037f, .044f, 1};
    Color button{.065f, .075f, .09f, 1};
    Color field{.012f, .016f, .022f, 1};
    Color splitter{.045f, .055f, .067f, 1};
    Color list{.021f, .026f, .032f, 1};
    Color selected{.055f, .16f, .27f, 1};
    Color hover{.10f, .13f, .17f, 1};
    Color pressed{.11f, .23f, .35f, 1};
    Color border{.095f, .11f, .13f, 1};
    Color focus{.15f, .38f, .62f, 1};
    Color invalid{.65f, .08f, .04f, 1};
    Color selection{.08f, .23f, .42f, 1};
    Color modified{.9f, .73f, .43f, 1};
    bool operator==(const Palette&) const = default;
};
// One value owns all presentation defaults. Explicit NodeInput geometry remains
// the only structural layout policy; control_padding adds text insets for
// interactive text controls, and does not modify containers, labels or the viewport.
struct Style {
    Font font;
    float title_scale{9.f / 7};
    float caption_scale{6.f / 7};
    std::uint16_t title_weight{600};
    Palette palette;
    ui_layout::Insets control_padding{6, 4, 6, 4};
    float border_width{1};
    float caret_width{1}; // DIPs.
    float scrollbar_width{3};
    float scrollbar_inset{1};
    float scrollbar_min_thumb{12};
    bool operator==(const Style&) const = default;
};

} // namespace atelier::ui
