#include "ui/session.hpp"
#include "ui/text.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <map>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace atelier::ui {
namespace {
constexpr std::array ladder_steps{100., 10., 1., .1, .01, .001, .0001};
constexpr std::array ladder_labels{L"100", L"10", L"1", L".1", L".01", L".001", L".0001"};
constexpr float ladder_tick_distance = 4;
const detail::TextKey tooltip_title_key{0, 0, true};
const detail::TextKey tooltip_body_key{0, 0, false}; // Transient slot; value ladder and help are mutually exclusive.
detail::TextStore::Symbol icon_symbol(ControlIcon icon) {
    using Icon = ControlIcon;
    using Symbol = detail::TextStore::Symbol;
    return icon == Icon::close              ? Symbol::close
           : icon == Icon::maximize         ? Symbol::maximize
           : icon == Icon::restore          ? Symbol::restore
           : icon == Icon::plus             ? Symbol::plus
           : icon == Icon::back             ? Symbol::back
           : icon == Icon::forward          ? Symbol::forward
           : icon == Icon::world            ? Symbol::world
           : icon == Icon::object           ? Symbol::object
           : icon == Icon::axes             ? Symbol::transform
           : icon == Icon::component        ? Symbol::component
           : icon == Icon::mesh             ? Symbol::mesh
           : icon == Icon::folder           ? Symbol::folder
           : icon == Icon::search           ? Symbol::search
           : icon == Icon::view_list        ? Symbol::view_list
           : icon == Icon::view_details     ? Symbol::view_details
           : icon == Icon::view_tiles       ? Symbol::view_tiles
           : icon == Icon::filter           ? Symbol::filter
           : icon == Icon::more             ? Symbol::more
           : icon == Icon::pointer          ? Symbol::pointer
           : icon == Icon::translate        ? Symbol::translate
           : icon == Icon::rotate           ? Symbol::rotate
           : icon == Icon::scale            ? Symbol::scale
           : icon == Icon::cursor3d         ? Symbol::cursor3d
           : icon == Icon::magnet           ? Symbol::magnet
           : icon == Icon::fly              ? Symbol::fly
           : icon == Icon::point_light      ? Symbol::point_light
           : icon == Icon::area_light       ? Symbol::area_light
           : icon == Icon::sun_light        ? Symbol::sun_light
           : icon == Icon::sky_light        ? Symbol::sky_light
           : icon == Icon::shade_wire       ? Symbol::shade_wire
           : icon == Icon::shade_solid      ? Symbol::shade_solid
           : icon == Icon::shade_matcap     ? Symbol::shade_matcap
           : icon == Icon::shade_lit        ? Symbol::shade_lit
           : icon == Icon::overlays         ? Symbol::overlays
           : icon == Icon::select_new       ? Symbol::select_new
           : icon == Icon::select_extend    ? Symbol::select_extend
           : icon == Icon::select_subtract  ? Symbol::select_subtract
           : icon == Icon::select_invert    ? Symbol::select_invert
           : icon == Icon::select_intersect ? Symbol::select_intersect
           : icon == Icon::grid             ? Symbol::grid
           : icon == Icon::axes_origin      ? Symbol::axes_origin
           : icon == Icon::wire_overlay     ? Symbol::wire_overlay
           : icon == Icon::lights_off       ? Symbol::lights_off
           : icon == Icon::fps              ? Symbol::fps
           : icon == Icon::color_picker     ? Symbol::color_picker
           : icon == Icon::rename           ? Symbol::rename
           : icon == Icon::duplicate        ? Symbol::duplicate
           : icon == Icon::trash            ? Symbol::trash
           : icon == Icon::unparent         ? Symbol::unparent
           : icon == Icon::left             ? Symbol::left
           : icon == Icon::right            ? Symbol::right
           : icon == Icon::up               ? Symbol::up
                                            : Symbol::down;
}
bool text_kind(ControlKind kind) {
    return kind == ControlKind::label || kind == ControlKind::button || kind == ControlKind::item ||
           kind == ControlKind::tree_item || kind == ControlKind::number || kind == ControlKind::text_edit ||
           kind == ControlKind::checkbox || kind == ControlKind::foldout || kind == ControlKind::menu_item ||
           kind == ControlKind::combo || kind == ControlKind::tab;
}
bool toggle_kind(ControlKind kind) {
    return kind == ControlKind::checkbox || kind == ControlKind::foldout;
}
bool activatable(ControlKind kind) {
    return kind == ControlKind::button || kind == ControlKind::item || kind == ControlKind::tree_item ||
           toggle_kind(kind) || kind == ControlKind::menu_item || kind == ControlKind::combo ||
           kind == ControlKind::tab;
}
bool scrolling(ControlKind kind) {
    return kind == ControlKind::tree || kind == ControlKind::list || kind == ControlKind::popup;
}
bool text_field(ControlKind kind) {
    return kind == ControlKind::number || kind == ControlKind::text_edit || kind == ControlKind::code_editor;
}
bool editable(ControlKind kind) {
    return kind == ControlKind::number || kind == ControlKind::splitter || kind == ControlKind::slider;
}
Rect slider_track(Rect bounds, float scale) {
    const auto inset = std::min(7 * scale, bounds.width * .5f);
    return {bounds.x + inset, bounds.y + bounds.height * .5f - scale, std::max(0.f, bounds.width - 2 * inset),
            2 * scale};
}
bool focusable(ControlKind kind) {
    return (text_kind(kind) && kind != ControlKind::label) || kind == ControlKind::slider ||
           kind == ControlKind::table || kind == ControlKind::code_editor;
}
bool contains(Rect rect, float x, float y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}
bool contains(Clip clip, float x, float y) {
    return x >= clip.left && x < clip.right && y >= clip.top && y < clip.bottom;
}
Clip intersect(Clip a, Clip b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
Clip clip_rect(Rect value) {
    return {static_cast<std::int32_t>(std::floor(value.x)), static_cast<std::int32_t>(std::floor(value.y)),
            static_cast<std::int32_t>(std::ceil(value.x + value.width)),
            static_cast<std::int32_t>(std::ceil(value.y + value.height))};
}
bool valid_color(Color c) {
    return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b) && std::isfinite(c.a) && c.r >= 0 &&
           c.r <= 1 && c.g >= 0 && c.g <= 1 && c.b >= 0 && c.b <= 1 && c.a >= 0 && c.a <= 1;
}
#include "ui/style.inl"
bool visible(Rect bounds, Clip clip) {
    return bounds.width > 0 && bounds.height > 0 && clip.right > clip.left && clip.bottom > clip.top &&
           bounds.x < clip.right && bounds.y < clip.bottom && bounds.x + bounds.width > clip.left &&
           bounds.y + bounds.height > clip.top;
}
std::wstring format(double value, bool single_precision = false) {
    char buffer[64];
    const auto result = single_precision ? std::to_chars(buffer, buffer + sizeof(buffer), static_cast<float>(value))
                                         : std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{})
        return L"0";
    return std::wstring(buffer, result.ptr);
}
std::optional<double> number(std::wstring_view text, const Control& control) {
    if (text.empty() || text.size() > 64)
        return {};
    char buffer[64];
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] > 127)
            return {};
        buffer[i] = static_cast<char>(text[i]);
    }
    const auto* begin = buffer;
    if (*begin == '+')
        ++begin;
    double value{};
    const auto parsed = std::from_chars(begin, buffer + text.size(), value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != buffer + text.size() || !std::isfinite(value))
        return {};
    // Like Runtime writes to bounded fields, typed values clamp to the legal range.
    return std::clamp(value, control.minimum, control.maximum);
}
}

Result<void> validate_control_properties(const Control& control) {
    if (!control.key || static_cast<unsigned>(control.kind) > static_cast<unsigned>(ControlKind::code_editor) ||
        static_cast<unsigned>(control.role) > static_cast<unsigned>(Role::caption) || control.text.size() > 4096 ||
        !std::isfinite(control.value) || !std::isfinite(control.minimum) || !std::isfinite(control.maximum) ||
        control.minimum > control.maximum || !std::isfinite(control.drag_step) || !std::isfinite(control.fine_step) ||
        control.drag_step <= 0 || control.fine_step <= 0 ||
        static_cast<unsigned>(control.drag_axis) > static_cast<unsigned>(DragAxis::vertical) ||
        static_cast<unsigned>(control.button_behavior) > static_cast<unsigned>(ButtonBehavior::toggle) ||
        static_cast<unsigned>(control.menu_behavior) > static_cast<unsigned>(MenuBehavior::radio))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if (static_cast<unsigned>(control.icon) > static_cast<unsigned>(last_control_icon) ||
        (control.icon != ControlIcon::none && control.kind != ControlKind::button &&
         control.kind != ControlKind::label && control.kind != ControlKind::splitter &&
         control.kind != ControlKind::combo && control.kind != ControlKind::tree_item &&
         control.kind != ControlKind::menu_item) ||
        (control.click_action && control.kind != ControlKind::splitter) ||
        (control.action_target && control.kind != ControlKind::button && control.kind != ControlKind::checkbox &&
         control.kind != ControlKind::table) ||
        ((control.kind == ControlKind::table) != static_cast<bool>(control.table)) ||
        ((control.kind == ControlKind::code_editor) != static_cast<bool>(control.code)) ||
        (control.read_only && control.kind != ControlKind::code_editor) ||
        (control.kind != ControlKind::table &&
         (control.table_selection != TableSelection{} || control.table_layout != TableLayout{})) ||
        static_cast<unsigned>(control.table_layout.view) > static_cast<unsigned>(TableView::tiles) ||
        !std::isfinite(control.table_layout.row_height) || control.table_layout.row_height < 12 ||
        control.table_layout.row_height > 256 || !std::isfinite(control.table_layout.header_height) ||
        control.table_layout.header_height < 12 || control.table_layout.header_height > 256 ||
        !std::isfinite(control.table_layout.tile_width) || control.table_layout.tile_width < 12 ||
        control.table_layout.tile_width > 4096 || !std::isfinite(control.table_layout.tile_height) ||
        control.table_layout.tile_height < 12 || control.table_layout.tile_height > 4096 ||
        !std::isfinite(control.table_layout.item_gap) || control.table_layout.item_gap < 0 ||
        control.table_layout.item_gap > 256 ||
        (control.button_behavior != ButtonBehavior::command && control.kind != ControlKind::button) ||
        (control.menu_behavior != MenuBehavior::command &&
         (control.kind != ControlKind::menu_item || control.menu_branch || control.edit_command)) ||
        ((control.menu_behavior == MenuBehavior::radio) != (control.radio_group != 0)) ||
        (control.kind == ControlKind::slider &&
         (control.minimum >= control.maximum || !std::isfinite(control.maximum - control.minimum))))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if (!detail::valid_text(control.text, control.kind == ControlKind::text_edit))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if ((control.kind != ControlKind::tree && control.tree_row_count) || control.tree_row_count > maximum_tree_rows ||
        (control.kind != ControlKind::tree_item &&
         (control.tree_index || control.tree_depth || control.tree_branch || control.tree_expanded ||
          control.tree_lines.any() || control.tree_primary)) ||
        control.tree_depth >= 128 || (control.tree_expanded && !control.tree_branch) || control.tree_lines[0] ||
        (control.tree_lines >> (control.tree_depth + 1)).any())
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if (control.tooltip) {
        const auto& help = *control.tooltip;
        const auto multiline = [](std::wstring_view value) {
            return detail::valid_text(value, false) && std::ranges::all_of(value, [](wchar_t c) {
                       return c >= 32 || c == L'\n' || c == L'\r' || c == L'\t';
                   });
        };
        if (!detail::valid_text(help.title) || !detail::valid_text(help.identity) || !multiline(help.description))
            return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    }
    if (control.default_value && (!std::isfinite(*control.default_value) || *control.default_value < control.minimum ||
                                  *control.default_value > control.maximum))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if (!StyleSheet::valid_properties(control.style) || control.classes.size() > 16 ||
        !std::all_of(control.classes.begin(), control.classes.end(),
                     [](const auto& name) { return StyleSheet::valid_class(name); }))
        return std::unexpected(Error{ErrorCode::invalid_style, control.key});
    if (editable(control.kind) && control.kind != ControlKind::slider &&
        (control.value < control.minimum || control.value > control.maximum))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    return {};
}
const ControlBox* UiFrame::find(ControlKey key) const noexcept {
    const auto found =
        std::find_if(controls.begin(), controls.end(), [key](const auto& box) { return box.key == key; });
    return found == controls.end() ? nullptr : &*found;
}
const TextGeometry* UiFrame::find_text(ControlKey key) const noexcept {
    const auto found =
        std::find_if(text_geometry.begin(), text_geometry.end(), [key](const auto& value) { return value.key == key; });
    return found == text_geometry.end() ? nullptr : &*found;
}
const TableGeometry* UiFrame::find_table(ControlKey key) const noexcept {
    const auto found =
        std::find_if(tables.begin(), tables.end(), [key](const auto& table) { return table.key == key; });
    return found == tables.end() ? nullptr : &*found;
}

const CodeGeometry* UiFrame::find_code(ControlKey key) const noexcept {
    const auto at =
        std::find_if(code_editors.begin(), code_editors.end(), [&](const auto& code) { return code.key == key; });
    return at == code_editors.end() ? nullptr : &*at;
}

struct Session::Impl {
    using TableIndex = std::unordered_map<TableKey, std::size_t>;
    struct State {
        Control control;
        ui_layout::LayoutNodeId node;
        std::uint64_t incarnation{};
        double scroll{};
        double scroll_limit{};
        double scroll_x{};
        double scroll_x_limit{};
        float viewport_height{}; // Scrolling kinds: last laid-out content height, in DIP.
        bool table_reveal{true};
        bool tree_reveal{};
        std::uint64_t code_selection_revision{};
        std::uint64_t code_revision{};
        float code_width{};
        std::vector<detail::TextKey> table_text; // Visible cells only; old frames retain their own coverage.
        TableIndex table_index;
        float content_bottom{};
        bool baseline_content{};
        std::size_t frame_index{std::numeric_limits<std::size_t>::max()};
        std::size_t layout_position{}; // Temporary index in a complete-tree input batch.
        bool style_dirty{true};
        bool layout_dirty{true};
        ui_layout::Insets layout_padding{};
        float layout_tree_height{};
        std::uint64_t display_revision{};
        VisualState visual;
        std::uint64_t theme_revision{};
        std::uint64_t parent_revision{};
        std::uint64_t inheritance_revision{};
        StyleProperties inherited;
        std::vector<std::shared_ptr<const StyleSheet>> sheets;
        std::shared_ptr<const ResolvedStyle> resolved;
    };
    struct Ladder {
        ValueLadderGeometry geometry;
        std::shared_ptr<const ResolvedStyle> style;
        std::array<detail::RasterText, 7> labels;
        float text_height{};
        double remainder{};
        double multiplier{1};
        bool fine{};
        bool multiply{};
    };
    struct Press {
        ControlKey key{};
        float x{};
        float y{};
        float last_x{};
        float last_y{};
        double original{};
        double value{};
        bool dragging{};
        bool selecting{};
        PointerButton button{PointerButton::primary};
        std::optional<Ladder> ladder{};
        float slider_span{1}; // Accepted physical track width, frozen until release.
        bool tree_disclosure{};
        bool scrollbar{}; // Scroll offset belongs to the UI, never a document edit.
        bool horizontal{};
        TableSelection table_cell{};
        TableKey table_column{};
        bool table_resize{};
        bool table_activate{};
    };
    struct Edit {
        struct Composition {
            std::uint64_t id{};
            EditToken token;
            std::uint32_t first{};
            std::uint32_t last{};
            Preedit preedit;
            std::uint64_t revision{detail::new_identity()};
        };
        ControlKey key;
        EditToken token;
        std::optional<detail::TextEdit> field_buffer;
        std::shared_ptr<CodeDocument> document;
        std::uint64_t document_selection{};
        detail::TextEdit& buffer() { return document ? document->edit() : *field_buffer; }
        const detail::TextEdit& buffer() const { return document ? document->edit() : *field_buffer; }
        double original_value{};
        double preview_value{};
        bool text_mode{};
        float scroll{};
        std::optional<Composition> composition;
        std::optional<ClipboardRequest> clipboard;
        std::optional<ImeRequest> ime_request;
        bool commit_paste{};
        Edit(const State& state, std::wstring value, double numeric, bool mode, bool select_all)
            : key(state.control.key), token{key, state.incarnation, detail::new_identity(), detail::new_identity()},
              document(state.control.code), original_value(numeric), preview_value(numeric),
              text_mode(mode || document != nullptr) {
            if (document)
                document_selection = buffer().selection_revision();
            else
                field_buffer.emplace(std::move(value), state.control.kind == ControlKind::number ? 64 : 4096,
                                     select_all);
        }
    };
    std::uint64_t identity{detail::new_identity()};
    Style style;
    std::shared_ptr<const StyleSheet> style_sheet;
    std::uint64_t style_revision{detail::new_identity()};
    std::uint64_t prepares{};
    std::uint64_t culled_text_controls{};
    std::uint64_t controls_revision{detail::new_identity()};
    std::uint64_t control_batches{};
    std::uint64_t controls_updated{};
    std::uint64_t controls_created{};
    std::uint64_t controls_removed{};
    std::uint64_t layout_syncs{};
    std::uint64_t layout_nodes_created{};
    std::uint64_t failed_frame_id{};
    Surface surface;
    detail::TextStore text;
    ui_layout::LayoutDocument layout{detail::TextStore::measure, &text, {10001, 128}};
    std::map<ControlKey, State> states;
    std::vector<ControlKey> order;
    bool structure_dirty{true};
    std::shared_ptr<const UiFrame> published;
    std::optional<Press> press;
    std::optional<Edit> edit;
    ControlKey focus{};
    ControlKey hover{};
    ControlKey keyboard_pressed{};
    Key keyboard_key{Key::none};
    Key terminal_key{Key::none};
    ControlKey popup{};
    ControlKey popup_return_focus{};
    bool popup_dismissed{};
    struct TooltipHover {
        ControlKey owner{};
        std::uint64_t incarnation{};
        Rect anchor;
        Clip clip;
        std::shared_ptr<const Tooltip> content;
        float x{};
        float y{};
        std::chrono::steady_clock::time_point since;
        std::uint64_t generation{};
    };
    struct TooltipCache {
        std::shared_ptr<const Tooltip> content;
        std::uint64_t style_revision{};
        float width{};
        float height{};
        float scale{};
        ui_layout::MeasuredSize title_size{};
        ui_layout::MeasuredSize body_size{};
        detail::RasterText title;
        detail::RasterText body;
    };
    std::optional<TooltipHover> tooltip_hover;
    std::optional<TooltipCache> tooltip_cache;

    explicit Impl(std::span<const std::wstring> fonts) : text(fonts) {
        static_cast<void>(layout.set_root({ui_layout::Kind::overlay}));
    }
    bool active() const {
        return !failed_frame_id && published && published->drawing->extent == surface.extent &&
               published->drawing->dpi_scale == surface.dpi_scale &&
               published->drawing->surface_revision == surface.revision && surface.extent.width &&
               surface.extent.height;
    }
    State* live(ControlKey key) {
        const auto it = states.find(key);
        return it == states.end() ? nullptr : &it->second;
    }
    bool current(const ControlBox& box) const {
        const auto found = states.find(box.key);
        if (found == states.end() || found->second.incarnation != box.incarnation ||
            found->second.control.kind != box.kind || found->second.control.action_target != box.action_target ||
            found->second.control.context_target != box.context_target ||
            found->second.control.context_group != box.context_group ||
            found->second.control.button_behavior != box.button_behavior ||
            found->second.control.menu_behavior != box.menu_behavior ||
            found->second.control.radio_group != box.radio_group || found->second.control.table != box.table ||
            found->second.control.table_layout != box.table_layout || found->second.control.code != box.code ||
            !box.enabled || found->second.control.read_only != box.read_only ||
            (box.kind == ControlKind::tree_item && box.parent != found->second.control.parent))
            return false;
        auto key = box.key;
        while (key) {
            const auto& control = states.at(key).control;
            if (!control.enabled || control.layout.collapsed)
                return false;
            key = control.parent;
        }
        return true;
    }
    const ControlBox* hit(float x, float y) const {
        if (!active())
            return nullptr;
        for (auto it = published->controls.rbegin(); it != published->controls.rend(); ++it)
            if (contains(it->bounds, x, y) && contains(it->clip, x, y) && current(*it))
                return &*it;
        return nullptr;
    }
    void retire_keyboard(Reply& reply) noexcept {
        if (!keyboard_pressed)
            return;
        terminal_key = keyboard_key; // Native repeats belong to the retired owner until key-up.
        keyboard_pressed = 0;
        keyboard_key = Key::none;
        reply.redraw = true;
    }
    void set_focus(Reply& reply, ControlKey next) noexcept {
        if (focus == next)
            return;
        retire_keyboard(reply);
        focus = next;
        reply.redraw = true;
    }
#include "ui/session_tooltip.inl"
#include "ui/session_edit.inl"
#include "ui/session_code.inl"
#include "ui/session_popup.inl"
#include "ui/session_tree.inl"
#include "ui/session_table.inl"
#include "ui/session_update.inl"
    void resolve(State& state) {
        const auto& control = state.control;
        const auto* parent = control.parent ? &states.at(control.parent) : nullptr;
        const bool enabled = control.enabled && (!parent || parent->visual.enabled);
        const VisualState visual{enabled,
                                 enabled && hover == control.key,
                                 enabled && ((press && press->key == control.key) || keyboard_pressed == control.key),
                                 enabled && focus == control.key,
                                 control.selected,
                                 control.invalid || (edit && edit->key == control.key && !valid_edit()),
                                 control.modified};
        const auto parent_revision = parent ? parent->inheritance_revision : 0;
        if (!state.style_dirty && state.resolved && state.theme_revision == style_revision &&
            state.parent_revision == parent_revision && state.visual == visual)
            return;
        auto sheets = parent ? parent->sheets : std::vector<std::shared_ptr<const StyleSheet>>{};
        if (!parent && style_sheet)
            sheets.push_back(style_sheet);
        if (control.style_scope)
            sheets.push_back(control.style_scope);
        auto inherited = parent ? parent->inherited : StyleProperties{};
        auto value = resolve_style(style, control.role, control.kind, visual);
        // A one-line label sits on its box's centre line like every control beside it;
        // paragraphs stay top-aligned. Sheets may still override either.
        if (control.kind == ControlKind::label && control.single_line)
            value.text_vertical_align = TextVerticalAlign::center;
        apply_properties(value, inherited);
        for (const auto& sheet : sheets)
            for (const auto& rule : sheet->rules())
                if (matches(rule.selector, control, visual)) {
                    apply_properties(value, rule.properties);
                    inherit_properties(inherited, rule.properties);
                }
        apply_properties(value, control.style);
        inherit_properties(inherited, control.style);
        if (!state.resolved || state.inherited != inherited || state.sheets != sheets)
            state.inheritance_revision = detail::new_identity();
        if (!state.resolved || *state.resolved != value)
            state.resolved = std::make_shared<const ResolvedStyle>(std::move(value));
        state.inherited = std::move(inherited);
        state.sheets = std::move(sheets);
        state.visual = visual;
        state.parent_revision = parent_revision;
        state.theme_revision = style_revision;
        state.style_dirty = false;
    }
    ui_layout::NodeInput input_for(State& state, ui_layout::Insets padding) {
        auto input = state.control.layout;
        const auto& appearance = *state.resolved;
        input.padding = padding;
        if (state.control.kind == ControlKind::tree_item) {
            // A described row sits at its host row index; rows outside the window have no node.
            const auto row_height = states.at(state.control.parent).resolved->tree_row_height;
            input.out_of_flow = true;
            input.anchor_x = input.anchor_y = input.pivot_x = input.pivot_y = input.offset_x = 0;
            input.offset_y = static_cast<float>(static_cast<double>(state.control.tree_index) * row_height);
            input.height = ui_layout::Axis::fixed(row_height);
        } else if (state.control.kind == ControlKind::tree && input.height.rule == ui_layout::SizeRule::content) {
            // Content height covers every host row, up to the tree's own maximum; scrolling covers the rest.
            auto rows =
                static_cast<float>(static_cast<double>(state.control.tree_row_count) * appearance.tree_row_height +
                                   padding.top + padding.bottom);
            if (input.height.maximum)
                rows = std::min(rows, *input.height.maximum);
            input.height.minimum = std::max(input.height.minimum, rows);
        }
        if (text_kind(state.control.kind)) {
            input.kind = ui_layout::Kind::measured;
            input.content_key = state.control.key;
            input.content_revision =
                text_field(state.control.kind)
                    ? text.set(state.control.key, display(state), appearance.font, false, appearance.text_align)
                    : text.set(state.control.key, state.control.text, appearance.font,
                               state.control.kind == ControlKind::label && !state.control.single_line,
                               appearance.text_align,
                               state.control.kind == ControlKind::label && state.control.single_line);
        } else if (state.control.kind == ControlKind::viewport || state.control.kind == ControlKind::splitter ||
                   state.control.kind == ControlKind::slider || state.control.kind == ControlKind::table) {
            input.kind = ui_layout::Kind::box;
        } else if (scrolling(state.control.kind)) {
            input.kind = ui_layout::Kind::column;
            if (state.control.kind == ControlKind::popup) {
                input.height.maximum = surface.extent.height / surface.dpi_scale;
                input.width.maximum = surface.extent.width / surface.dpi_scale;
                if (input.width.rule == ui_layout::SizeRule::fixed)
                    input.width.value = std::min(input.width.value, *input.width.maximum);
            }
        }
        return input;
    }
    Result<void> synchronize() {
        const auto checked = [](const auto& result, ControlKey key) -> Result<void> {
            if (!result)
                return std::unexpected(Error{ErrorCode::layout_failed, key, result.error().provider_code});
            return {};
        };
        std::vector<ui_layout::TreeNodeInput> tree;
        if (structure_dirty)
            tree.reserve(order.size());
        for (auto key : order) {
            auto& state = states.at(key);
            auto alignment = state.control.layout.vertical_alignment;
            const auto* owner = state.control.parent ? &states.at(state.control.parent) : nullptr;
            state.baseline_content = owner && owner->baseline_content;
            if (owner && alignment.inherited && owner->control.kind == ControlKind::container &&
                owner->control.layout.kind == ui_layout::Kind::row && owner->control.layout.flow)
                alignment = owner->control.layout.flow->align_items;
            state.baseline_content |= alignment.first_baseline;
            const auto previous = state.resolved;
            resolve(state);
            const auto& appearance = *state.resolved;
            auto padding = padding_for(state);
            if (scrolling(state.control.kind) && appearance.scrollbar_gutter == ScrollbarGutter::stable)
                padding.right += appearance.scrollbar_width + appearance.scrollbar_inset * 2;
            const auto row_height = state.control.kind == ControlKind::tree_item ? owner->resolved->tree_row_height
                                    : state.control.kind == ControlKind::tree    ? appearance.tree_row_height
                                                                                 : 0.f;
            const auto display_version =
                edit && edit->key == key ? (edit->composition ? edit->composition->revision : edit->token.revision) : 0;
            state.layout_dirty |= !previous ||
                                  (text_kind(state.control.kind) && (previous->font != appearance.font ||
                                                                     previous->text_align != appearance.text_align)) ||
                                  padding != state.layout_padding || row_height != state.layout_tree_height ||
                                  state.display_revision != display_version;
            if (!structure_dirty && !state.layout_dirty)
                continue;
            ++layout_syncs;
            auto input = input_for(state, padding);
            if (structure_dirty) {
                state.layout_position = tree.size();
                tree.push_back({state.node, owner ? std::optional<std::size_t>{owner->layout_position} : std::nullopt,
                                std::move(input)});
            } else {
                if (auto result = checked(layout.update(state.node, input), key); !result)
                    return result;
                state.layout_dirty = false;
            }
            // These are preparation caches. A failed tree batch keeps structure_dirty
            // and all pending layout dirty flags, so retry regenerates every entry.
            state.layout_padding = padding;
            state.layout_tree_height = row_height;
            state.display_revision = display_version;
        }
        if (structure_dirty) {
            const auto result = layout.set_tree(tree);
            if (!result) {
                const auto index = result.error().tree_index;
                const auto key = index && *index < order.size() ? order[*index] : ControlKey{};
                return std::unexpected(Error{ErrorCode::layout_failed, key, result.error().provider_code});
            }
            for (std::size_t i = 0; i < order.size(); ++i) {
                auto& state = states.at(order[i]);
                if (!state.node)
                    ++layout_nodes_created;
                state.node = (*result)[i];
                state.layout_dirty = false;
            }
        }
        structure_dirty = false;
        return {};
    }
};

Session::Session(std::span<const std::wstring> fonts) : impl_(std::make_unique<Impl>(fonts)) {}
Session::~Session() = default;
Result<void> Session::set_style(const Style& style) {
    if (!valid_style(style))
        return std::unexpected(Error{ErrorCode::invalid_style});
    auto& self = *impl_;
    if (self.style == style)
        return {};
    Style candidate = style; // Finish allocating before changing the active configuration.
    const bool font_changed = !same_font_policy(self.style, candidate);
    const auto revision = detail::new_identity();
    const auto provider = font_changed ? detail::new_identity() : self.text.provider_revision();
    static_assert(std::is_nothrow_move_assignable_v<Style>);
    self.style = std::move(candidate);
    self.style_revision = revision;
    self.dismiss_tooltip();
    if (font_changed)
        self.text.set_provider_revision(provider);
    return {};
}
const Style& Session::style() const noexcept {
    return impl_->style;
}
Result<void> Session::set_style_sheet(std::shared_ptr<const StyleSheet> sheet) {
    if (impl_->style_sheet != sheet) {
        const auto revision = detail::new_identity();
        impl_->style_sheet = std::move(sheet);
        impl_->style_revision = revision;
        impl_->dismiss_tooltip();
    }
    return {};
}
Statistics Session::statistics() const noexcept {
    const auto text = impl_->text.statistics();
    return {
        impl_->prepares,         text.measure_calls,       text.text_layouts,           text.rasterizations,
        text.raster_bytes,       text.coverage_cache_hits, impl_->culled_text_controls, impl_->text.provider_revision(),
        text.live_coverages,     text.live_coverage_bytes, impl_->control_batches,      impl_->controls_updated,
        impl_->controls_created, impl_->controls_removed,  impl_->layout_syncs,         impl_->layout_nodes_created,
        impl_->layout.stats()};
}
Result<Reply> Session::set_surface(Surface surface) {
    if (!std::isfinite(surface.dpi_scale) || surface.dpi_scale < .25f || surface.dpi_scale > 8 || !surface.revision ||
        surface.extent.width > 32768 || surface.extent.height > 32768)
        return std::unexpected(Error{ErrorCode::invalid_surface});
    Reply reply;
    if (impl_->surface != surface) {
        reply = impl_->cancel();
        impl_->edit.reset();
        impl_->set_focus(reply, 0);
        impl_->surface = surface;
        reply.redraw = true;
        for (auto& [key, state] : impl_->states)
            if (state.control.kind == ControlKind::popup)
                state.layout_dirty = true;
    }
    return reply;
}
Result<Reply> Session::set_controls(std::span<const Control> controls) {
    bool unchanged = controls.size() == impl_->order.size();
    for (std::size_t i = 0; unchanged && i < controls.size(); ++i)
        unchanged = controls[i].key == impl_->order[i] && controls[i] == impl_->states.at(impl_->order[i]).control;
    if (unchanged)
        return Reply{};
    return impl_->apply_controls(controls, false);
}
ControlKey Session::pointer_target(float x, float y) const {
    const auto* hit = impl_->hit(x, y);
    return hit ? hit->key : 0;
}
TreeViewport Session::tree_viewport(ControlKey key) const noexcept {
    const auto& self = *impl_;
    const auto surface_height = static_cast<float>(self.surface.extent.height) / self.surface.dpi_scale;
    const auto found = self.states.find(key);
    // An unresolved tree uses the smallest legal row height so a host window covers the surface.
    if (found == self.states.end() || found->second.control.kind != ControlKind::tree || !found->second.resolved)
        return {0, surface_height, 12};
    const auto& state = found->second;
    return {state.scroll, state.node && state.viewport_height > 0 ? state.viewport_height : surface_height,
            state.resolved->tree_row_height};
}
Result<Reply> Session::update_controls(std::span<const Control> controls) {
    return impl_->apply_controls(controls, true);
}

Result<std::shared_ptr<const UiFrame>> Session::prepare() {
    auto& self = *impl_;
    ++self.prepares;
    const auto frame_id = detail::new_identity();
    struct Attempt {
        std::uint64_t& failed;
        std::uint64_t frame_id;
        bool completed{};
        ~Attempt() {
            if (!completed)
                failed = frame_id;
        }
    } attempt{self.failed_frame_id, frame_id};
    if (!self.surface.extent.width || !self.surface.extent.height)
        return std::unexpected(Error{ErrorCode::unavailable_surface});
    try {
        self.sync_code_edit();
        if (auto synced = self.synchronize(); !synced)
            return std::unexpected(synced.error());
        const float scale = self.surface.dpi_scale;
        const auto geometry = self.layout.compute(
            {self.surface.extent.width / scale, self.surface.extent.height / scale}, self.text.provider_revision());
        if (!geometry)
            return std::unexpected(Error{ErrorCode::layout_failed, 0, geometry.error().provider_code});
        auto output = std::make_shared<UiFrame>();
        auto draws = std::make_shared<Frame>();
        output->controls.reserve(self.order.size());
        draws->draws.reserve(self.order.size() * 3);
        draws->id = frame_id;
        draws->extent = self.surface.extent;
        draws->dpi_scale = scale;
        draws->surface_revision = self.surface.revision;
        output->session_identity = self.identity;
        output->controls_revision = self.controls_revision;
        output->style_revision = self.style_revision;
        output->measure_provider_revision = self.text.provider_revision();
        const Clip target{0, 0, static_cast<std::int32_t>(draws->extent.width),
                          static_cast<std::int32_t>(draws->extent.height)};
        for (auto key : self.order) {
            auto& state = self.states.at(key);
            state.frame_index = std::numeric_limits<std::size_t>::max();
            state.content_bottom = 0;
        }
        for (auto key : self.order) {
            const auto& state = self.states.at(key);
            if (state.control.parent && scrolling(self.states.at(state.control.parent).control.kind))
                if (const auto* box = geometry->find(state.node))
                    self.states.at(state.control.parent).content_bottom =
                        std::max(self.states.at(state.control.parent).content_bottom, box->y + box->height);
        }
        for (auto key : self.order) {
            auto& state = self.states.at(key);
            if (scrolling(state.control.kind))
                if (const auto* box = geometry->find(state.node)) {
                    const auto padding = effective_padding(state.control, *state.resolved);
                    // Trees scroll over every host row, described or not.
                    if (state.control.kind == ControlKind::tree)
                        state.content_bottom = std::max(
                            state.content_bottom,
                            static_cast<float>(padding.top + static_cast<double>(state.control.tree_row_count) *
                                                                 state.resolved->tree_row_height));
                    state.viewport_height = std::max(0.f, box->height - padding.top - padding.bottom);
                    const auto content_end = state.content_bottom + padding.bottom;
                    const auto overflow = content_end - box->height;
                    // Measurement and placement can accumulate fractional rows in
                    // different orders. Discard float roundoff, not real overflow.
                    const auto roundoff =
                        4 * std::numeric_limits<float>::epsilon() * std::max({1.f, content_end, box->height});
                    state.scroll_limit = overflow > roundoff ? overflow : 0.f;
                    state.scroll = std::clamp(state.scroll, 0.0, state.scroll_limit);
                }
        }
        for (auto key : self.order) {
            auto& state = self.states.at(key);
            if (state.control.kind != ControlKind::tree_item || !state.tree_reveal)
                continue;
            auto& tree = self.states.at(state.control.parent);
            const auto* row = geometry->find(state.node);
            const auto* owner = geometry->find(tree.node);
            if (!row || !owner || owner->height <= 0)
                continue;
            if (row->y < tree.scroll)
                tree.scroll = row->y;
            if (row->y + row->height > tree.scroll + owner->height)
                tree.scroll = row->y + row->height - owner->height;
            tree.scroll = std::clamp(tree.scroll, 0.0, tree.scroll_limit);
            state.tree_reveal = false;
        }
        auto quad = [&](Rect bounds, Clip clip, Color color, std::shared_ptr<const Coverage> coverage = {}) {
            clip = intersect(clip, target);
            if (visible(bounds, clip) && color.a > 0)
                draws->draws.push_back({bounds, clip, color, {0, 0, 1, 1}, std::move(coverage)});
        };
        const auto paint_box = [&](Rect bounds, Clip clip, Color background, std::shared_ptr<const ColorImage> image,
                                   CornerRadii radius, float border, Color edge, BorderStyle relief,
                                   BorderEdges edges = BorderEdges::all, std::optional<Color> inner = {},
                                   std::optional<Color> shadow = {}) {
            const bool beveled = relief != BorderStyle::solid;
            const bool square = beveled || edges != BorderEdges::all || radius == CornerRadii{};
            if (square && border > 0) {
                const auto right = std::round(bounds.x + bounds.width);
                const auto bottom = std::round(bounds.y + bounds.height);
                bounds.x = std::round(bounds.x);
                bounds.y = std::round(bounds.y);
                bounds.width = std::max(0.f, right - bounds.x);
                bounds.height = std::max(0.f, bottom - bounds.y);
                border = std::min({std::max(1.f, std::round(border)), bounds.width * .5f, bounds.height * .5f});
            }
            const bool separate = beveled || edges != BorderEdges::all;
            draws->draws.push_back({bounds,
                                    clip,
                                    image ? Color{1, 1, 1, 1} : background,
                                    {0, 0, 1, 1},
                                    {},
                                    square ? CornerRadii{} : radius,
                                    separate ? 0 : border,
                                    edge,
                                    std::move(image),
                                    background});
            if (border <= 0)
                return;
            if (separate) {
                const auto dark = shadow.value_or(Color{edge.r * .15f, edge.g * .15f, edge.b * .15f, edge.a});
                const auto near = relief == BorderStyle::inset ? dark : edge;
                const auto far = relief == BorderStyle::outset ? dark : edge;
                const auto top = has_edge(edges, BorderEdges::top) ? border : 0.f;
                const auto bottom = has_edge(edges, BorderEdges::bottom) ? border : 0.f;
                if (top)
                    quad({bounds.x, bounds.y, bounds.width, border}, clip, near);
                if (bottom)
                    quad({bounds.x, bounds.y + bounds.height - border, bounds.width, border}, clip, far);
                if (has_edge(edges, BorderEdges::left))
                    quad({bounds.x, bounds.y + top, border, std::max(0.f, bounds.height - top - bottom)}, clip, near);
                if (has_edge(edges, BorderEdges::right))
                    quad({bounds.x + bounds.width - border, bounds.y + top, border,
                          std::max(0.f, bounds.height - top - bottom)},
                         clip, far);
            }
            // Square frames can carry an independent top/left inner relief.
            if (square && inner) {
                if (has_edge(edges, BorderEdges::top))
                    quad({bounds.x + border, bounds.y + border, std::max(0.f, bounds.width - 2 * border), border}, clip,
                         *inner);
                if (has_edge(edges, BorderEdges::left))
                    quad({bounds.x + border, bounds.y + 2 * border, border, std::max(0.f, bounds.height - 3 * border)},
                         clip, *inner);
            }
        };
        const auto paint_scrollbar = [&](ScrollbarGeometry bar, const ResolvedStyle& appearance, bool enabled) {
            const auto width = bar.horizontal ? bar.bounds.height : bar.bounds.width;
            const auto span = bar.horizontal ? bar.bounds.width : bar.bounds.height;
            if (bar.maximum <= 0 || width <= 0 || span <= 0)
                return;
            const auto step = std::min(appearance.scrollbar_button_size > 0 ? appearance.scrollbar_button_size * scale
                                                                            : (width >= 8 * scale ? width : 0.f),
                                       span / 3);
            const auto segment = [&](float offset, float length) {
                return bar.horizontal ? Rect{bar.bounds.x + offset, bar.bounds.y, length, width}
                                      : Rect{bar.bounds.x, bar.bounds.y + offset, width, length};
            };
            const auto track_span = span - 2 * step;
            bar.track = segment(0, track_span);
            bar.decrement = segment(track_span, step);
            bar.increment = segment(track_span + step, step);
            const auto length =
                std::min(track_span, std::max(appearance.scrollbar_min_thumb * scale,
                                              track_span * static_cast<float>(bar.page / (bar.page + bar.maximum))));
            bar.thumb = segment(static_cast<float>(bar.value / bar.maximum) * (track_span - length), length);
            output->scrollbars.push_back(bar);
            const auto stroke = std::max(1.f, std::round(scale * 2.f / 3));
            const Color shadow{0, 0, 0, appearance.indicator_border.a};
            paint_box(bar.track, bar.clip, appearance.indicator_background, {}, {}, stroke, shadow, BorderStyle::solid);
            paint_box(bar.thumb, bar.clip, appearance.scrollbar, appearance.scrollbar_image, {}, stroke,
                      appearance.indicator_border, BorderStyle::solid);
            if (length > 12 * scale && width > 7 * scale) {
                const auto pitch = std::max(2.f, std::round(scale * 4.f / 3));
                const auto center_x = bar.thumb.x + bar.thumb.width * .5f;
                const auto center_y = bar.thumb.y + bar.thumb.height * .5f;
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 3; ++column) {
                        const auto across = (column - 1) * pitch;
                        const auto along = (row - 1.5f) * pitch;
                        const auto x = std::round(center_x + (bar.horizontal ? along : across));
                        const auto y = std::round(center_y + (bar.horizontal ? across : along));
                        quad({x + stroke, y + stroke, stroke, stroke}, bar.clip, appearance.indicator_border);
                        quad({x, y, stroke, stroke}, bar.clip, shadow);
                    }
            }
            for (const auto [button, decrement] : {std::pair{bar.decrement, true}, std::pair{bar.increment, false}}) {
                if (step <= 0)
                    continue;
                const bool available = enabled && (decrement ? bar.value > 0 : bar.value < bar.maximum);
                paint_box(button, bar.clip, appearance.scrollbar, available ? appearance.scrollbar_image : nullptr, {},
                          stroke, available ? appearance.indicator_border : appearance.scrollbar, BorderStyle::solid);
                const auto size = std::min(16.f * scale, std::min(step, width));
                auto arrow = appearance.foreground;
                const auto tone = available ? .5f : .085f;
                arrow.r *= tone;
                arrow.g *= tone;
                arrow.b *= tone;
                const auto symbol =
                    bar.horizontal ? (decrement ? detail::TextStore::Symbol::left : detail::TextStore::Symbol::right)
                                   : (decrement ? detail::TextStore::Symbol::up : detail::TextStore::Symbol::down);
                quad({button.x + (button.width - size) * .5f, button.y + (button.height - size) * .5f, size, size},
                     bar.clip, arrow, self.text.symbol(symbol, static_cast<std::uint32_t>(std::ceil(size))));
            }
        };
        std::size_t popup_draw_begin = std::numeric_limits<std::size_t>::max();
        for (auto key : self.order) {
            auto& state = self.states.at(key);
            if (state.control.kind == ControlKind::popup && self.popup_dismissed)
                continue;
            const auto* box = geometry->find(state.node);
            if (!box)
                continue;
            Rect bounds{box->x * scale, box->y * scale, box->width * scale, box->height * scale};
            Clip clip = target;
            bool enabled = state.control.enabled;
            if (state.control.parent) {
                const auto& parent_state = self.states.at(state.control.parent);
                if (parent_state.frame_index == std::numeric_limits<std::size_t>::max())
                    continue;
                const auto& parent_box = output->controls[parent_state.frame_index];
                bounds.x += parent_box.bounds.x;
                bounds.y += parent_box.bounds.y;
                clip = intersect(parent_box.clip, clip_rect(parent_box.bounds));
                enabled &= parent_box.enabled;
                if (scrolling(parent_state.control.kind)) {
                    bounds.y -= static_cast<float>(parent_state.scroll) * scale;
                    const auto& style = *parent_state.resolved;
                    if (style.scrollbar_gutter == ScrollbarGutter::stable)
                        clip.right =
                            std::min(clip.right, static_cast<std::int32_t>(std::floor(
                                                     parent_box.bounds.x + parent_box.bounds.width -
                                                     (style.scrollbar_width + style.scrollbar_inset * 2) * scale)));
                }
            }
            if (state.control.kind == ControlKind::popup) {
                bounds.x = std::clamp(bounds.x, 0.f,
                                      std::max(0.f, static_cast<float>(self.surface.extent.width) - bounds.width));
                bounds.y = std::clamp(bounds.y, 0.f,
                                      std::max(0.f, static_cast<float>(self.surface.extent.height) - bounds.height));
                output->popup = key;
                popup_draw_begin = draws->draws.size();
            }
            // Cap before conversion to integer clip; layout can legally overflow.
            if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || std::abs(bounds.x) > 1e8f ||
                std::abs(bounds.y) > 1e8f || bounds.width > 1e8f || bounds.height > 1e8f)
                return std::unexpected(Error{ErrorCode::invalid_control, key});
            state.frame_index = output->controls.size();
            output->controls.push_back({key,
                                        state.control.parent,
                                        state.control.kind,
                                        bounds,
                                        clip,
                                        enabled,
                                        state.incarnation,
                                        state.resolved,
                                        state.control.selected,
                                        state.control.action_target,
                                        state.control.button_behavior,
                                        state.control.menu_behavior,
                                        state.control.radio_group,
                                        state.control.table,
                                        state.control.table_selection,
                                        state.control.context_target,
                                        state.control.context_group,
                                        state.control.table_layout,
                                        state.control.code,
                                        state.control.tooltip,
                                        state.control.read_only});
            const auto kind = state.control.kind;
            if (kind == ControlKind::tree_item) {
                const auto indent = self.states.at(state.control.parent).resolved->tree_indent * scale;
                const auto x = bounds.x + (state.control.layout.padding.left + state.resolved->padding.left) * scale +
                               state.control.tree_depth * indent;
                output->tree_rows.push_back(
                    {key, state.control.tree_index, state.control.tree_depth,
                     state.control.tree_branch ? Rect{x, bounds.y, indent, bounds.height} : Rect{},
                     state.control.tree_expanded});
            }
            // Keep every arranged box and its parent transform. Only painting is culled.
            if (!visible(bounds, clip)) {
                if (text_kind(kind))
                    ++self.culled_text_controls;
                continue;
            }
            const auto& appearance = *state.resolved;
            const float border = std::min({appearance.border_width * scale, bounds.width * .5f, bounds.height * .5f});
            const CornerRadii radius{appearance.radius.top_left * scale, appearance.radius.top_right * scale,
                                     appearance.radius.bottom_right * scale, appearance.radius.bottom_left * scale};
            if (appearance.background || appearance.image || (border > 0 && appearance.border.a > 0)) {
                const auto background = appearance.background.value_or(Color{0, 0, 0, 0});
                paint_box(bounds, clip, background, appearance.image, radius, border, appearance.border,
                          appearance.border_style, appearance.border_edges, appearance.border_inner,
                          appearance.border_shadow);
            }
            if (kind == ControlKind::tree) {
                const auto padding = effective_padding(state.control, appearance);
                const auto row_height = appearance.tree_row_height * scale;
                auto stripe_clip = intersect(clip, clip_rect(bounds));
                if (appearance.scrollbar_gutter == ScrollbarGutter::stable)
                    stripe_clip.right = std::min(
                        stripe_clip.right,
                        static_cast<int>(bounds.x + bounds.width -
                                         (appearance.scrollbar_width + 2 * appearance.scrollbar_inset) * scale));
                const auto origin = bounds.y + padding.top * scale - static_cast<float>(state.scroll) * scale;
                const auto first = std::max(0, static_cast<int>(std::floor((stripe_clip.top - origin) / row_height)));
                for (auto row = first; origin + row * row_height < stripe_clip.bottom; ++row)
                    if (row % 2)
                        quad({bounds.x, origin + row * row_height, bounds.width, row_height}, stripe_clip,
                             appearance.tree_alternate);
            }
            if (kind == ControlKind::tree_item) {
                const auto& tree_style = *self.states.at(state.control.parent).resolved;
                const auto indent = tree_style.tree_indent * scale;
                const auto start = bounds.x + (state.control.layout.padding.left + appearance.padding.left) * scale;
                const auto depth = state.control.tree_depth;
                const auto cx = std::round(start + (depth + .5f) * indent);
                const auto cy = std::round(bounds.y + bounds.height * .5f);
                const auto tree_clip = intersect(clip, clip_rect(bounds));
                const auto stroke = std::max(1.f, std::round(scale * .667f));
                if (depth) {
                    const auto px = std::round(cx - indent);
                    quad({px, bounds.y, stroke, (state.control.tree_lines[depth] ? bounds.height : cy - bounds.y)},
                         tree_clip, tree_style.tree_line);
                    quad({px, cy, indent + (state.control.tree_branch ? 0 : indent * .5f), stroke}, tree_clip,
                         tree_style.tree_line);
                    for (std::uint32_t level = 1; level < depth; ++level)
                        if (state.control.tree_lines[level])
                            quad({std::round(start + (level - .5f) * indent), bounds.y, stroke, bounds.height},
                                 tree_clip, tree_style.tree_line);
                }
                if (state.control.tree_branch) {
                    const bool expanded = state.control.tree_expanded;
                    if (expanded)
                        quad({cx, cy, stroke, bounds.y + bounds.height - cy}, tree_clip, tree_style.tree_line);
                    const auto diameter = std::min(9 * scale, indent - 2 * scale);
                    const auto radius = diameter * .5f;
                    paint_box({cx - radius, cy - radius, diameter, diameter}, tree_clip,
                              appearance.indicator_background, {}, {radius, radius, radius, radius}, 0, {},
                              BorderStyle::solid);
                    quad({cx - 2.5f * scale, cy - stroke * .5f, 5 * scale, stroke}, tree_clip,
                         appearance.indicator_border);
                    if (!expanded)
                        quad({cx - stroke * .5f, cy - 2.5f * scale, stroke, 5 * scale}, tree_clip,
                             appearance.indicator_border);
                }
            }
            if (appearance.accent_width > 0) {
                const auto height = std::min(appearance.accent_width * scale, bounds.height);
                quad({bounds.x, bounds.y + bounds.height - height, bounds.width, height},
                     intersect(clip, clip_rect(bounds)), appearance.accent);
            }
            if (kind == ControlKind::slider) {
                const auto track = slider_track(bounds, scale);
                const auto value = self.press && self.press->key == key ? self.press->value : state.control.value;
                const auto fraction = static_cast<float>(std::clamp(
                    (value - state.control.minimum) / (state.control.maximum - state.control.minimum), 0.0, 1.0));
                const auto slider_clip = intersect(clip, clip_rect(bounds));
                quad(track, slider_clip, appearance.border);
                quad({track.x, track.y, track.width * fraction, track.height}, slider_clip, appearance.accent);
                const auto height = std::min(16 * scale, bounds.height);
                paint_box({track.x + track.width * fraction - 2.5f * scale, bounds.y + (bounds.height - height) * .5f,
                           5 * scale, height},
                          slider_clip, appearance.indicator_background, {}, {}, scale, appearance.indicator_border,
                          BorderStyle::outset);
            }
            if (state.control.icon != ControlIcon::none) {
                using Icon = ControlIcon;
                const auto icon = state.control.icon;
                const auto icon_clip = intersect(clip, clip_rect(bounds));
                if (icon == Icon::grip_horizontal || icon == Icon::grip_vertical) {
                    const bool vertical = icon == Icon::grip_vertical;
                    const auto length =
                        std::min(appearance.indicator_size * scale, vertical ? bounds.height : bounds.width);
                    const auto dot = std::min(
                        {std::max(1.f, std::round(scale * 4.f / 3)), length, vertical ? bounds.width : bounds.height});
                    const auto pitch = std::round((length - dot) / 6);
                    for (int i = 0; i < 7 && dot > 0; ++i) {
                        const float offset = (static_cast<float>(i) - 3) * pitch;
                        const auto x = std::round(bounds.x + bounds.width * .5f - dot * .5f + (vertical ? 0 : offset));
                        const auto y = std::round(bounds.y + bounds.height * .5f - dot * .5f + (vertical ? offset : 0));
                        quad({x, y, dot, dot}, icon_clip, appearance.foreground);
                    }
                } else {
                    const auto symbol = icon_symbol(icon);
                    auto padding = self.padding_for(state);
                    const bool captioned = !state.control.text.empty();
                    const bool above = captioned && appearance.icon_placement == IconPlacement::above;
                    const bool leading = captioned && !above;
                    if (leading)
                        padding.left -= appearance.indicator_size + 6;
                    if (above)
                        padding.top -= appearance.indicator_size + 2;
                    const float width = std::max(0.f, bounds.width - (padding.left + padding.right) * scale);
                    const float height = std::max(0.f, bounds.height - (padding.top + padding.bottom) * scale);
                    const float size = std::min({appearance.indicator_size * scale, width, height});
                    const auto alignment = captioned ? TextAlign::center : appearance.icon_align;
                    const float offset = alignment == TextAlign::left    ? 0
                                         : alignment == TextAlign::right ? width - size
                                                                         : (width - size) * .5f;
                    const float x = bounds.x + padding.left * scale + (leading ? 0 : offset);
                    const float y = bounds.y + padding.top * scale + (above ? 0 : (height - size) * .5f);
                    if (size > 0) {
                        if (icon == Icon::maximize)
                            quad({x + size * .15f, y + size * .225f, size * .7f, size * .55f}, icon_clip,
                                 appearance.indicator_background);
                        if (icon == Icon::back || icon == Icon::forward) {
                            quad({x, y, size, size}, icon_clip, appearance.indicator_border,
                                 self.text.symbol(symbol, static_cast<std::uint32_t>(std::ceil(size))));
                            const auto stroke = std::max(1.f, std::round(scale));
                            if (size > stroke * 2)
                                quad(
                                    {x + stroke, y + stroke, size - 2 * stroke, size - 2 * stroke}, icon_clip,
                                    appearance.foreground,
                                    self.text.symbol(symbol, static_cast<std::uint32_t>(std::ceil(size - 2 * stroke))));
                        } else
                            quad({x, y, size, size}, icon_clip,
                                 appearance.indicator_foreground.value_or(appearance.foreground),
                                 self.text.symbol(symbol, static_cast<std::uint32_t>(std::ceil(size))));
                    }
                }
            }
            if (toggle_kind(kind) ||
                (kind == ControlKind::menu_item && state.control.menu_behavior == MenuBehavior::checkbox)) {
                const auto marker_clip = intersect(clip, clip_rect(bounds));
                const float x = bounds.x + (state.control.layout.padding.left + appearance.padding.left) * scale;
                const float size = appearance.indicator_size * scale;
                const float y = bounds.y + (bounds.height - size) * .5f;
                const float glyph = std::max(2.f, appearance.indicator_size - 2) * scale;
                if (kind == ControlKind::checkbox || kind == ControlKind::menu_item) {
                    paint_box({x, y, size, size}, marker_clip, appearance.indicator_background, {}, {}, scale,
                              appearance.indicator_border.a > 0 ? appearance.indicator_border : appearance.foreground,
                              kind == ControlKind::menu_item      ? BorderStyle::solid
                              : appearance.indicator_border.a > 0 ? BorderStyle::inset
                                                                  : BorderStyle::solid);
                    if (state.control.selected)
                        quad({x + scale, y + scale, glyph, glyph}, marker_clip,
                             appearance.indicator_foreground.value_or(appearance.foreground),
                             self.text.symbol(detail::TextStore::Symbol::check,
                                              static_cast<std::uint32_t>(std::ceil(glyph))));
                } else {
                    quad({x, y, size, size}, marker_clip,
                         appearance.indicator_foreground.value_or(appearance.foreground),
                         self.text.symbol(state.control.selected ? detail::TextStore::Symbol::disclosure_down
                                                                 : detail::TextStore::Symbol::disclosure_right,
                                          static_cast<std::uint32_t>(std::ceil(size))));
                }
            }
            if (kind == ControlKind::table) {
#include "ui/session_table_draw.inl"
                continue;
            }
            if (kind == ControlKind::menu_item && state.control.menu_behavior == MenuBehavior::radio) {
                using Symbol = detail::TextStore::Symbol;
                const auto size = appearance.indicator_size * scale;
                const auto x = bounds.x + (state.control.layout.padding.left + appearance.padding.left) * scale;
                const auto y = bounds.y + (bounds.height - size) * .5f;
                const auto symbol = state.control.selected ? Symbol::radio_checked : Symbol::radio;
                if (size > 0)
                    quad({x, y, size, size}, intersect(clip, clip_rect(bounds)), appearance.foreground,
                         self.text.symbol(symbol, static_cast<std::uint32_t>(std::ceil(size))));
            }
            if (kind == ControlKind::combo && state.control.icon != ControlIcon::none && state.control.text.empty()) {
                const auto size = std::min({7.f * scale, bounds.width * .3f, bounds.height * .3f});
                if (size > 0)
                    quad(
                        {bounds.x + bounds.width - size - 2 * scale, bounds.y + bounds.height - size - 2 * scale, size,
                         size},
                        intersect(clip, clip_rect(bounds)),
                        appearance.indicator_foreground.value_or(appearance.foreground),
                        self.text.symbol(detail::TextStore::Symbol::down, static_cast<std::uint32_t>(std::ceil(size))));
            }
            if (kind == ControlKind::combo && appearance.combo_indicator != ComboIndicator::none &&
                (state.control.icon == ControlIcon::none || !state.control.text.empty())) {
                // Houdini's dropdown arrow stays ~9 px whatever the combo's icon size.
                const float size = std::min(appearance.indicator_size,
                                            appearance.combo_indicator == ComboIndicator::caret ? 14.f : 9.f) *
                                   scale;
                const auto arrow_clip = intersect(clip, clip_rect(bounds));
                if (appearance.combo_indicator == ComboIndicator::caret) {
                    // Inline caret immediately after the label; no separate arrow well.
                    const auto padding = self.padding_for(state);
                    const auto metrics =
                        self.text.metrics(key, std::max(0.f, box->width - (padding.left + padding.right)));
                    const float x = bounds.x + (padding.left + metrics.width + 1) * scale;
                    if (size > 0)
                        quad({x, bounds.y + (bounds.height - size) * .5f + scale, size, size}, arrow_clip,
                             appearance.indicator_foreground.value_or(appearance.foreground),
                             self.text.symbol(detail::TextStore::Symbol::caret,
                                              static_cast<std::uint32_t>(std::ceil(size))));
                } else {
                    quad({bounds.x + bounds.width - 15 * scale, bounds.y + border, 15 * scale - border,
                          std::max(0.f, bounds.height - 2 * border)},
                         arrow_clip, {0, 0, 0, .17f});
                    const auto arrow = [&](detail::TextStore::Symbol symbol, float cy) {
                        quad({bounds.x + bounds.width - (15 * scale + size) * .5f, cy - size * .5f, size, size},
                             arrow_clip, appearance.indicator_foreground.value_or(appearance.foreground),
                             self.text.symbol(symbol, static_cast<std::uint32_t>(std::ceil(size))));
                    };
                    if (appearance.combo_indicator == ComboIndicator::arrows) {
                        arrow(detail::TextStore::Symbol::up, bounds.y + bounds.height * .5f - 3.5f * scale);
                        arrow(detail::TextStore::Symbol::down, bounds.y + bounds.height * .5f + 3.5f * scale);
                    } else
                        arrow(detail::TextStore::Symbol::down, bounds.y + bounds.height * .5f);
                }
            }
            if (kind == ControlKind::code_editor) {
#include "ui/session_code_draw.inl"
            }
            if (text_kind(kind)) {
                const auto padding = self.padding_for(state);
                const float width = std::max(0.f, box->width - (padding.left + padding.right));
                const auto metrics = self.text.metrics(key, width);
                const auto paragraph = text_field(kind) ? self.text.snapshot(key, width) : nullptr;
                float x = bounds.x + padding.left * scale;
                float y = bounds.y + padding.top * scale;
                const float content_height = std::max(0.f, box->height - (padding.top + padding.bottom));
                // Baseline contexts use the same top origin as layout measurement,
                // including text contributing a nested container's first baseline.
                if (!state.baseline_content) {
                    const auto alignment = appearance.text_vertical_align;
                    const float offset = alignment == TextVerticalAlign::center   ? .5f
                                         : alignment == TextVerticalAlign::bottom ? 1.f
                                                                                  : 0.f;
                    y += std::max(0.f, content_height - metrics.height) * offset * scale;
                }
                const auto text_clip =
                    intersect(clip, clip_rect(text_field(kind) ? Rect{bounds.x + padding.left * scale,
                                                                      bounds.y + padding.top * scale, width * scale,
                                                                      content_height * scale}
                                                               : bounds));
                const bool editing = self.edit && self.edit->key == key && self.edit->text_mode;
                float scroll = editing ? self.edit->scroll : 0;
                Rect caret;
                if (editing) {
                    caret = paragraph->caret(self.display_caret());
                    scroll = std::clamp(scroll, 0.f, std::max(0.f, metrics.width + appearance.caret_width - width));
                    if (caret.x < scroll)
                        scroll = caret.x;
                    if (caret.x + appearance.caret_width > scroll + width)
                        scroll = std::max(0.f, caret.x + appearance.caret_width - width);
                    x -= scroll * scale;
                    if (!self.edit->composition) {
                        const auto& s = self.edit->buffer().current();
                        for (auto range : paragraph->selection(std::min(s.caret.offset, s.anchor.offset),
                                                               std::max(s.caret.offset, s.anchor.offset)))
                            quad({x + range.x * scale, y + range.y * scale, range.width * scale, range.height * scale},
                                 text_clip, appearance.selection);
                    } else {
                        const auto& c = *self.edit->composition;
                        for (std::size_t first = 0; first < c.preedit.attributes.size();) {
                            auto last = first + 1;
                            while (last < c.preedit.attributes.size() &&
                                   c.preedit.attributes[last] == c.preedit.attributes[first])
                                ++last;
                            if (c.preedit.attributes[first] == 1 || c.preedit.attributes[first] == 3)
                                for (auto range : paragraph->selection(c.first + static_cast<std::uint32_t>(first),
                                                                       c.first + static_cast<std::uint32_t>(last)))
                                    quad({x + range.x * scale, y + range.y * scale, range.width * scale,
                                          range.height * scale},
                                         text_clip, appearance.selection);
                            first = last;
                        }
                    }
                }
                if (text_field(kind)) {
                    const bool bound = self.edit && self.edit->key == key;
                    output->text_geometry.push_back({key, state.incarnation, paragraph, x, y, scale, scroll,
                                                     bound ? self.edit->token : EditToken{},
                                                     bound && self.edit->composition ? self.edit->composition->id : 0});
                }
                const auto raster = self.text.raster(
                    key, width, scale,
                    text_field(kind)
                        ? std::optional<Clip>{clip_rect({static_cast<float>(text_clip.left) - x,
                                                         static_cast<float>(text_clip.top) - y,
                                                         static_cast<float>(text_clip.right - text_clip.left),
                                                         static_cast<float>(text_clip.bottom - text_clip.top)})}
                        : std::nullopt);
                if (raster.coverage && appearance.text_shadow) {
                    const auto offset = std::max(1.f, std::round(scale));
                    quad({x + raster.left + offset, y + raster.top + offset, static_cast<float>(raster.coverage->width),
                          static_cast<float>(raster.coverage->height)},
                         text_clip, *appearance.text_shadow, raster.coverage);
                }
                if (raster.coverage)
                    quad({x + raster.left, y + raster.top, static_cast<float>(raster.coverage->width),
                          static_cast<float>(raster.coverage->height)},
                         text_clip, appearance.foreground, raster.coverage);
                if (editing) {
                    if (self.edit->composition) {
                        const auto& c = *self.edit->composition;
                        for (auto range :
                             paragraph->selection(c.first, c.first + static_cast<std::uint32_t>(c.preedit.text.size())))
                            quad({x + range.x * scale, y + (range.y + range.height) * scale - scale,
                                  range.width * scale, scale},
                                 text_clip, appearance.caret);
                    }
                    const Rect physical{x + caret.x * scale, y + caret.y * scale, appearance.caret_width * scale,
                                        std::max(1.f, caret.height * scale)};
                    quad(physical, text_clip, appearance.caret);
                    if (visible(physical, text_clip))
                        output->caret = TextCaret{self.edit->token, physical, text_clip};
                    TextInputAnchor anchor{self.edit->token, 0, 0, {}, text_clip};
                    Rect start;
                    if (self.edit->composition) {
                        const auto& c = *self.edit->composition;
                        anchor.composition_id = c.id;
                        anchor.composition_revision = c.revision;
                        start =
                            c.preedit.text.empty() ? paragraph->caret({c.first, false}) : paragraph->character(c.first);
                    } else {
                        const auto& selection = self.edit->buffer().current();
                        const auto position =
                            selection.caret.offset == selection.anchor.offset
                                ? selection.caret
                                : detail::TextPosition{std::min(selection.caret.offset, selection.anchor.offset),
                                                       false};
                        start = paragraph->caret(position);
                    }
                    anchor.bounds = {x + start.x * scale, y + start.y * scale, start.width * scale,
                                     start.height * scale};
                    output->input_anchor = anchor;
                }
            }
        }
        const auto popup_draw_end = draws->draws.size();
        const auto scrollbar = [&](const auto& state) {
            if (state.frame_index == std::numeric_limits<std::size_t>::max())
                return;
            const auto& bounds = output->controls[state.frame_index];
            const auto& appearance = *state.resolved;
            if (state.scroll_limit <= 0 || !visible(bounds.bounds, bounds.clip))
                return;
            ScrollbarGeometry bar;
            bar.key = bounds.key;
            bar.clip = intersect(bounds.clip, clip_rect(bounds.bounds));
            const auto width = appearance.scrollbar_width * scale;
            const auto inset = appearance.scrollbar_inset * scale;
            bar.bounds = {bounds.bounds.x + bounds.bounds.width - width - inset, bounds.bounds.y + inset, width,
                          std::max(0.f, bounds.bounds.height - 2 * inset)};
            bar.maximum = state.scroll_limit;
            bar.value = state.scroll;
            bar.page = bounds.bounds.height / scale;
            paint_scrollbar(bar, appearance, bounds.enabled);
        };
        for (auto key : self.order)
            if (scrolling(self.states.at(key).control.kind) && self.states.at(key).control.kind != ControlKind::popup)
                scrollbar(self.states.at(key));
        if (popup_draw_begin != std::numeric_limits<std::size_t>::max()) {
            // Root popup owns the top layer, including over ordinary list bars.
            std::rotate(draws->draws.begin() + static_cast<std::ptrdiff_t>(popup_draw_begin),
                        draws->draws.begin() + static_cast<std::ptrdiff_t>(popup_draw_end), draws->draws.end());
            scrollbar(self.states.at(output->popup));
        }
#include "ui/session_ladder.inl"
#include "ui/session_tooltip_draw.inl"
        output->drawing = std::move(draws);
        attempt.completed = true;
        return std::shared_ptr<const UiFrame>(std::move(output));
    } catch (const detail::TextFailure& failure) {
        return std::unexpected(Error{ErrorCode::text_failed, 0, failure.code});
    }
}
bool Session::accept(std::shared_ptr<const UiFrame> frame) {
    auto& self = *impl_;
    if (!frame || !frame->drawing || frame->session_identity != self.identity ||
        frame->controls_revision != self.controls_revision || frame->style_revision != self.style_revision ||
        frame->drawing->extent != self.surface.extent || frame->drawing->dpi_scale != self.surface.dpi_scale ||
        frame->drawing->surface_revision != self.surface.revision || frame->drawing->id <= self.failed_frame_id ||
        (self.published && frame->drawing->id < self.published->drawing->id))
        return false;
    if (frame->tooltip &&
        (!self.tooltip_hover || self.tooltip_blocked() || frame->tooltip->generation != self.tooltip_hover->generation))
        return false;
    for (const auto& geometry : frame->code_editors) {
        const auto* state = self.live(geometry.key);
        if (!state || !state->control.code || state->control.code->revision() != geometry.revision ||
            state->control.code->edit().selection_revision() != geometry.selection_revision)
            return false;
    }
    if (self.edit)
        if (const auto* geometry = frame->find_text(self.edit->key); geometry && geometry->token == self.edit->token)
            self.edit->scroll = geometry->scroll;
    self.published = std::move(frame);
    self.failed_frame_id = 0;
    self.validate_tooltip(*self.published);
    return true;
}
const std::shared_ptr<const UiFrame>& Session::presented() const noexcept {
    return impl_->published;
}
Reply Session::cancel_interaction() {
    return impl_->cancel();
}
void Session::dismiss_tooltip() noexcept {
    impl_->dismiss_tooltip();
}
Result<Reply> Session::commit_edit() {
    Reply reply;
    if (!impl_->finish_edit(reply, true, false, true))
        return std::unexpected(Error{ErrorCode::invalid_text, impl_->focus});
    return reply;
}
Reply Session::focus(ControlKey key) {
    Reply reply;
    const auto* box = impl_->published ? impl_->published->find(key) : nullptr;
    if (key && (!impl_->active() || !box || !impl_->current(*box) || !focusable(box->kind) ||
                !visible(box->bounds, box->clip)))
        return reply;
    if (impl_->focus != key)
        impl_->finish_edit(reply, true);
    impl_->set_focus(reply, key);
    if (key && text_field(box->kind) && !impl_->edit)
        impl_->begin_edit(key, box->kind == ControlKind::text_edit);
    reply.redraw = true;
    return reply;
}
Reply Session::execute(EditCommand command, std::uint64_t incarnation) {
    Reply reply;
    const auto* box = impl_->published ? impl_->published->find(command.target) : nullptr;
    if (impl_->active() && box && box->incarnation == incarnation)
        impl_->execute_edit_command(reply, command);
    return reply;
}
ControlKey Session::focused_control() const noexcept {
    return impl_->focus;
}
bool Session::wants_keyboard() const noexcept {
    const auto* state = impl_->live(impl_->focus);
    return state && text_field(state->control.kind) && impl_->active();
}
InteractionState Session::interaction_state() const noexcept {
    const auto& state = *impl_;
    return {state.press.has_value(),
            state.keyboard_pressed != 0,
            state.edit && !state.edit->document && state.edit->buffer().active(),
            state.edit && (state.edit->composition.has_value() || state.edit->ime_request.has_value()),
            state.edit && state.edit->clipboard.has_value(),
            has_popup()};
}
bool Session::has_popup() const noexcept {
    return impl_->popup || (impl_->published && impl_->published->popup);
}
bool Session::clipboard_current(const ClipboardRequest& request) const noexcept {
    const auto& self = *impl_;
    if (!self.edit || !self.edit->clipboard || self.focus != self.edit->key || !self.active())
        return false;
    const auto* box = self.published->find(self.focus);
    if (!box || !self.current(*box) || (box->read_only && request.kind != ClipboardKind::copy))
        return false;
    const auto& pending = *self.edit->clipboard;
    return pending.operation_id == request.operation_id && pending.kind == request.kind &&
           pending.token == request.token && self.edit->token == request.token;
}
std::optional<TextInputState> Session::text_input() const noexcept {
    const auto& self = *impl_;
    if (!self.edit || self.focus != self.edit->key || !self.active())
        return {};
    const auto* box = self.published->find(self.focus);
    if (!box || !self.current(*box) || box->read_only)
        return {};
    TextInputState value{self.edit->token, self.edit->composition.has_value(), {}};
    const auto& anchor = self.published->input_anchor;
    const auto composition_id = self.edit->composition ? self.edit->composition->id : 0;
    const auto composition_revision = self.edit->composition ? self.edit->composition->revision : 0;
    if (anchor && anchor->token == value.token && anchor->composition_id == composition_id &&
        anchor->composition_revision == composition_revision) {
        if (anchor->bounds.height > 0 && contains(anchor->clip, anchor->bounds.x, anchor->bounds.y))
            value.accepted_anchor = anchor;
        if (self.published->caret && self.published->caret->token == value.token)
            value.accepted_caret = self.published->caret;
    }
    return value;
}

#include "ui/session_input.inl"
} // namespace atelier::ui
