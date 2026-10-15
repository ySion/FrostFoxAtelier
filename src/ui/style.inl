// Private validation and state resolution shared by measurement and painting.
struct VisualState {
    bool enabled{true}, hover{}, pressed{}, focus{}, selected{}, invalid{}, modified{};
    bool operator==(const VisualState&) const = default;
};
bool valid_style(const Style& style) {
    const auto bounded = [](float value, float minimum, float maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    const auto valid_name = [](const std::wstring& value) { return !value.empty() && value.size() <= 128 && value.find(L'\0') == std::wstring::npos; };
    if (!valid_name(style.font.family) || !valid_name(style.font.locale) ||
        !bounded(style.font.size,4,256) || !style.font.weight || style.font.weight > 999 ||
        static_cast<unsigned>(style.font.style)>static_cast<unsigned>(FontStyle::italic) ||
        !bounded(style.font.size*style.title_scale,4,256) || !bounded(style.font.size*style.caption_scale,4,256) ||
        !style.title_weight || style.title_weight > 999) return false;
    for (auto value : {style.control_padding.left,style.control_padding.top,style.control_padding.right,style.control_padding.bottom})
        if (!bounded(value,0,128)) return false;
    if (!bounded(style.border_width,0,16) || !bounded(style.caret_width,.25f,16) ||
        !bounded(style.scrollbar_width,.25f,32) || !bounded(style.scrollbar_inset,0,32) || !bounded(style.scrollbar_min_thumb,1,128)) return false;
    const auto& p = style.palette;
    for (auto color : {p.text,p.muted,p.panel,p.button,p.field,p.splitter,p.list,p.selected,p.hover,p.pressed,p.border,p.focus,p.invalid,p.selection,p.modified})
        if (!valid_color(color)) return false;
    return true;
}
bool same_font_policy(const Style& a, const Style& b) {
    return a.font == b.font && a.title_scale == b.title_scale && a.caption_scale == b.caption_scale && a.title_weight == b.title_weight;
}
ResolvedStyle resolve_style(const Style& style, Role role, ControlKind kind, VisualState state = {}) {
    const auto& p = style.palette;
    ResolvedStyle value;
    value.font=style.font; value.caret_width=style.caret_width;
    value.scrollbar_width=style.scrollbar_width; value.scrollbar_inset=style.scrollbar_inset; value.scrollbar_min_thumb=style.scrollbar_min_thumb;
    value.foreground = p.text; value.border = p.border; value.selection = p.selection; value.caret = p.text;
    value.scrollbar=p.muted;
    if (scrolling(kind) || kind==ControlKind::table) { value.indicator_background=p.field; value.indicator_border=p.border; }
    if (kind==ControlKind::slider) { value.indicator_background=p.button; value.indicator_border=p.border; }
    if (role == Role::title) { value.font.size *= style.title_scale; value.font.weight = style.title_weight; }
    if (role == Role::caption) { value.font.size *= style.caption_scale; value.foreground = p.muted; }
    if (role == Role::panel && kind != ControlKind::viewport) value.background = p.panel;
    const bool button = kind == ControlKind::button || kind == ControlKind::combo || kind == ControlKind::tab;
    const bool item = kind == ControlKind::item || kind == ControlKind::tree_item || kind == ControlKind::menu_item;
    const bool toggle = kind == ControlKind::checkbox || kind == ControlKind::foldout;
    const bool number = kind == ControlKind::number || kind == ControlKind::text_edit, splitter = kind == ControlKind::splitter;
    if (button || item || number || toggle) value.text_vertical_align=TextVerticalAlign::center;
    if (button) value.background = p.button;
    if (number) value.background = p.field;
    if (splitter) value.background = p.splitter;
    if (kind == ControlKind::list || kind==ControlKind::tree || kind==ControlKind::table) value.background = p.list;
    if (kind == ControlKind::popup) { value.background=p.panel; value.border_width=style.border_width; }
    if (button || item || number || toggle) value.padding = style.control_padding;
    if (button || number || splitter) value.border_width = style.border_width;
    if (state.selected && (button || (item && kind!=ControlKind::menu_item))) value.background = p.selected;
    if (state.enabled) {
        if (state.hover && (button || item || splitter || toggle) && (!state.selected || toggle || kind==ControlKind::menu_item)) value.background = p.hover;
        if (state.hover && number) value.border = p.hover;
        if (state.pressed && (button || item || splitter || toggle)) value.background = p.pressed;
        if (state.focus) { value.border = p.focus; if (item || toggle) value.border_width = style.border_width; }
        if (state.modified) value.foreground = p.modified;
        if (state.invalid) value.border = p.invalid;
    } else { value.foreground = p.muted; value.caret = p.muted; }
    return value;
}
ui_layout::Insets effective_padding(const Control& control, const ResolvedStyle& style) {
    const auto& padding = control.layout.padding;
    const float marker = control.kind == ControlKind::checkbox || control.kind == ControlKind::foldout ||
        (control.kind==ControlKind::menu_item && control.menu_behavior!=MenuBehavior::command) ? style.indicator_size+6 : 0.f;
    const bool captioned=control.icon!=ControlIcon::none && !control.text.empty();
    const float leading=captioned && style.icon_placement==IconPlacement::leading?style.indicator_size+6:0.f;
    const float above=captioned && style.icon_placement==IconPlacement::above?style.indicator_size+2:0.f;
    return {padding.left+style.padding.left+marker+leading,padding.top+style.padding.top+above,
            padding.right+style.padding.right+(control.kind==ControlKind::combo && (control.icon==ControlIcon::none || !control.text.empty())?
                (style.combo_indicator==ComboIndicator::caret?style.indicator_size+1:style.combo_indicator==ComboIndicator::none?0.f:18.f):0.f),padding.bottom+style.padding.bottom};
}
void apply_properties(ResolvedStyle& value,const StyleProperties& p) {
    if (p.tree_row_height) value.tree_row_height=*p.tree_row_height;
    if (p.tree_indent) value.tree_indent=*p.tree_indent;
    if (p.tree_alternate) value.tree_alternate=*p.tree_alternate;
    if (p.tree_line) value.tree_line=*p.tree_line;
    if (p.font_family) value.font.family=*p.font_family;
    if (p.font_locale) value.font.locale=*p.font_locale;
    if (p.font_size) value.font.size=*p.font_size;
    if (p.font_weight) value.font.weight=*p.font_weight;
    if (p.font_style) value.font.style=*p.font_style;
    if (p.text_align) value.text_align=*p.text_align;
    if (p.text_vertical_align) value.text_vertical_align=*p.text_vertical_align;
    if (p.border_style) value.border_style=*p.border_style;
    if (p.border_edges) value.border_edges=*p.border_edges;
    if (p.border_inner) value.border_inner=*p.border_inner;
    if (p.border_shadow) value.border_shadow=*p.border_shadow;
    if (p.text_shadow) value.text_shadow=*p.text_shadow;
    if (p.scrollbar_gutter) value.scrollbar_gutter=*p.scrollbar_gutter;
    if (p.combo_indicator) value.combo_indicator=*p.combo_indicator;
    if (p.icon_placement) value.icon_placement=*p.icon_placement;
    if (p.icon_align) value.icon_align=*p.icon_align;
    if (p.indicator_size) value.indicator_size=*p.indicator_size;
    if (p.indicator_background) value.indicator_background=*p.indicator_background;
    if (p.indicator_border) value.indicator_border=*p.indicator_border;
    if (p.indicator_foreground) value.indicator_foreground=*p.indicator_foreground;
    if (p.foreground) value.foreground=*p.foreground;
    if (p.background) value.background=*p.background;
    if (p.border) value.border=*p.border;
    if (p.selection) value.selection=*p.selection;
    if (p.caret) value.caret=*p.caret;
    if (p.scrollbar) value.scrollbar=*p.scrollbar;
    if (p.accent) value.accent=*p.accent;
    if (p.accent_width) value.accent_width=*p.accent_width;
    if (p.padding) value.padding=*p.padding;
    if (p.border_width) value.border_width=*p.border_width;
    if (p.caret_width) value.caret_width=*p.caret_width;
    if (p.radius) value.radius=*p.radius;
    if (p.scrollbar_width) value.scrollbar_width=*p.scrollbar_width;
    if (p.scrollbar_inset) value.scrollbar_inset=*p.scrollbar_inset;
    if (p.scrollbar_min_thumb) value.scrollbar_min_thumb=*p.scrollbar_min_thumb;
    if (p.scrollbar_button_size) value.scrollbar_button_size=*p.scrollbar_button_size;
    if (p.image) value.image=*p.image;
    if (p.scrollbar_image) value.scrollbar_image=*p.scrollbar_image;
}
void inherit_properties(StyleProperties& inherited,const StyleProperties& p) {
    if (p.font_family) inherited.font_family=p.font_family;
    if (p.font_locale) inherited.font_locale=p.font_locale;
    if (p.font_size) inherited.font_size=p.font_size;
    if (p.font_weight) inherited.font_weight=p.font_weight;
    if (p.font_style) inherited.font_style=p.font_style;
    if (p.foreground) inherited.foreground=p.foreground;
}
bool matches(const StyleSelector& selector,const Control& control,VisualState state) {
    if ((selector.kind && *selector.kind!=control.kind) || (selector.role && *selector.role!=control.role)) return false;
    for (const auto& name:selector.classes) if (std::find(control.classes.begin(),control.classes.end(),name)==control.classes.end()) return false;
    unsigned bits=state.enabled?1u:2u;
    if (state.hover) bits|=4; if (state.pressed) bits|=8; if (state.focus) bits|=16;
    if (state.selected) bits|=32; if (state.invalid) bits|=64; if (state.modified) bits|=128;
    return (bits&static_cast<unsigned>(selector.states))==static_cast<unsigned>(selector.states);
}
