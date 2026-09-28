// Private validation and state resolution shared by measurement and painting.
struct Appearance {
    float font_size{};
    std::uint16_t font_weight{};
    ui_layout::Insets padding;
    Color foreground, border, selection, caret;
    std::optional<Color> background;
    float border_width{};
};
struct VisualState {
    bool enabled{true}, hover{}, pressed{}, focus{}, selected{}, invalid{};
};
bool valid_style(const Style& style) {
    const auto bounded = [](float value, float minimum, float maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    const auto valid_name = [](const std::wstring& value) { return !value.empty() && value.size() <= 128 && value.find(L'\0') == std::wstring::npos; };
    if (!valid_name(style.font.family) || !valid_name(style.font.locale) ||
        !bounded(style.font.size,4,256) || !style.font.weight || style.font.weight > 999 ||
        !bounded(style.font.size*style.title_scale,4,256) || !bounded(style.font.size*style.caption_scale,4,256) ||
        !style.title_weight || style.title_weight > 999) return false;
    for (auto value : {style.control_padding.left,style.control_padding.top,style.control_padding.right,style.control_padding.bottom})
        if (!bounded(value,0,128)) return false;
    if (!bounded(style.border_width,0,16) || !bounded(style.caret_width,.25f,16) ||
        !bounded(style.scrollbar_width,.25f,32) || !bounded(style.scrollbar_inset,0,32) || !bounded(style.scrollbar_min_thumb,1,128)) return false;
    const auto& p = style.palette;
    for (auto color : {p.text,p.muted,p.panel,p.button,p.field,p.splitter,p.list,p.selected,p.hover,p.pressed,p.border,p.focus,p.invalid,p.selection})
        if (!valid_color(color)) return false;
    return true;
}
bool same_font_policy(const Style& a, const Style& b) {
    return a.font == b.font && a.title_scale == b.title_scale && a.caption_scale == b.caption_scale && a.title_weight == b.title_weight;
}
Appearance resolve_style(const Style& style, Role role, ControlKind kind, VisualState state = {}) {
    const auto& p = style.palette;
    Appearance value;
    value.font_size = style.font.size; value.font_weight = style.font.weight;
    value.foreground = p.text; value.border = p.border; value.selection = p.selection; value.caret = p.text;
    if (role == Role::title) { value.font_size *= style.title_scale; value.font_weight = style.title_weight; }
    if (role == Role::caption) { value.font_size *= style.caption_scale; value.foreground = p.muted; }
    if (role == Role::panel && kind != ControlKind::viewport) value.background = p.panel;
    const bool button = kind == ControlKind::button, item = kind == ControlKind::item;
    const bool number = kind == ControlKind::number || kind == ControlKind::text_edit, splitter = kind == ControlKind::splitter;
    if (button) value.background = p.button;
    if (number) value.background = p.field;
    if (splitter) value.background = p.splitter;
    if (kind == ControlKind::list) value.background = p.list;
    if (button || item || number) value.padding = style.control_padding;
    if (button || number || splitter) value.border_width = style.border_width;
    if (state.selected && (button || item)) value.background = p.selected;
    if (state.enabled) {
        if (state.hover && (button || item || splitter) && !state.selected) value.background = p.hover;
        if (state.hover && number) value.border = p.hover;
        if (state.pressed && (button || item || splitter)) value.background = p.pressed;
        if (state.focus) { value.border = p.focus; if (item) value.border_width = style.border_width; }
        if (state.invalid) value.border = p.invalid;
    } else { value.foreground = p.muted; value.caret = p.muted; }
    return value;
}
ui_layout::Insets effective_padding(const Control& control, const Appearance& style) {
    const auto& padding = control.layout.padding;
    return {padding.left+style.padding.left,padding.top+style.padding.top,
            padding.right+style.padding.right,padding.bottom+style.padding.bottom};
}
