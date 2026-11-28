#include "ui/session.hpp"
#include "detail/code_syntax.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/result.hpp"
#include "memory/fatal.hpp"
#include "platform/utf.hpp"
#include "ui/detail/number_bits.hpp"
#include "ui/text.hpp"
#include "foundation/map.hpp"
#include "foundation/scope_exit.hpp"
#include "foundation/set.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <map>
#include <type_traits>
#include <utility>

namespace atelier::ui {
namespace {
constexpr std::array ladder_steps{100., 10., 1., .1, .01, .001, .0001};

constexpr std::array<StringView, 7> ladder_labels{"100", "10", "1", ".1", ".01", ".001", ".0001"};

constexpr float ladder_tick_distance = 4;

const detail::TextKey tooltip_title_key{0, 0, true};

const detail::TextKey tooltip_identity_key{0, 1, true};

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
           : icon == Icon::points           ? Symbol::points
           : icon == Icon::point_normals    ? Symbol::point_normals
           : icon == Icon::face_normals     ? Symbol::face_normals
           : icon == Icon::orient_scale     ? Symbol::orient_scale
           : icon == Icon::backface_tint    ? Symbol::backface_tint
           : icon == Icon::bounds_cage      ? Symbol::bounds_cage
           : icon == Icon::info             ? Symbol::info
           : icon == Icon::sticky_note      ? Symbol::sticky_note
           : icon == Icon::network_box      ? Symbol::network_box
           : icon == Icon::palette          ? Symbol::palette
           : icon == Icon::node_shapes      ? Symbol::node_shapes
           : icon == Icon::network_layout   ? Symbol::network_layout
           : icon == Icon::pin              ? Symbol::pin
           : icon == Icon::isolate          ? Symbol::isolate
           : icon == Icon::sliders          ? Symbol::sliders
           : icon == Icon::bypass_arrow     ? Symbol::bypass_arrow
           : icon == Icon::info_glyph       ? Symbol::info_glyph
           : icon == Icon::snowflake        ? Symbol::snowflake
           : icon == Icon::eye              ? Symbol::eye
           : icon == Icon::template_flag    ? Symbol::template_flag
           : icon == Icon::eye_off          ? Symbol::eye_off
           : icon == Icon::scene            ? Symbol::scene
           : icon == Icon::material         ? Symbol::material
           : icon == Icon::texture          ? Symbol::texture
           : icon == Icon::source_file      ? Symbol::source_file
           : icon == Icon::procgraph        ? Symbol::procgraph
           : icon == Icon::physics          ? Symbol::physics
           : icon == Icon::camera           ? Symbol::camera
           : icon == Icon::copy             ? Symbol::copy
           : icon == Icon::save             ? Symbol::save
           : icon == Icon::play             ? Symbol::play
           : icon == Icon::stop             ? Symbol::stop
           : icon == Icon::build            ? Symbol::build
           : icon == Icon::reload           ? Symbol::reload
           : icon == Icon::warning          ? Symbol::warning
           : icon == Icon::lock             ? Symbol::lock
           : icon == Icon::unlock           ? Symbol::unlock
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

// These controls only paint local primitives and, for passive fields, publish one immutable text geometry.
bool retains_local_paint(ControlKind kind) {
    return (text_kind(kind) && kind != ControlKind::tree_item) || kind == ControlKind::container ||
           kind == ControlKind::list || kind == ControlKind::splitter || kind == ControlKind::viewport ||
           kind == ControlKind::slider;
}

bool editable(ControlKind kind) {
    return kind == ControlKind::number || kind == ControlKind::splitter || kind == ControlKind::slider;
}

Rect slider_track(Rect bounds, float scale) {
    const auto inset = std::min(5.f * scale, bounds.width * .5f);
    const auto h = std::min(4.f * scale, bounds.height);
    return {bounds.x + inset, bounds.y + (bounds.height - h) * .5f, std::max(0.f, bounds.width - 2 * inset), h};
}

std::array<float, 3> hsv_linear_rgb(double hue_deg, float s, float v) noexcept {
    const float h = static_cast<float>(std::fmod(std::clamp(hue_deg, 0.0, 360.0), 360.0) / 60.0);
    const float c = v * s;
    const float x = c * (1.f - std::abs(std::fmod(h, 2.f) - 1.f));
    const float m = v - c;
    float r = 0;
    float g = 0;
    float b = 0;
    if (h < 1.f) {
        r = c;
        g = x;
    } else if (h < 2.f) {
        r = x;
        g = c;
    } else if (h < 3.f) {
        g = c;
        b = x;
    } else if (h < 4.f) {
        g = x;
        b = c;
    } else if (h < 5.f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    return {r + m, g + m, b + m};
}

std::uint8_t encode_srgb_byte(float linear) noexcept {
    const float clamped = std::clamp(linear, 0.f, 1.f);
    const float srgb = clamped <= .0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.f / 2.4f) - .055f;
    return static_cast<std::uint8_t>(std::lround(std::clamp(srgb, 0.f, 1.f) * 255.f));
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

Result<void> draw_paragraph(detail::TextStore& text, ControlKey key, float width, float scale, float x, float y,
                            Clip visible_clip, Clip preferred_clip, Color foreground, Optional<Color> shadow,
                            const auto& quad) {
    if (visible_clip.left >= visible_clip.right || visible_clip.top >= visible_clip.bottom)
        return {};
    const float offset = shadow ? std::max(1.f, std::round(scale)) : 0.f;
    const auto raster_region = [&](Clip dest) {
        // Include neighboring texels for fractional origins and ink casting a shadow into the destination.
        return clip_rect({static_cast<float>(dest.left) - x - offset - 1, static_cast<float>(dest.top) - y - offset - 1,
                          static_cast<float>(dest.right - dest.left) + offset + 2,
                          static_cast<float>(dest.bottom - dest.top) + offset + 2});
    };
    const auto fits = [](Clip region) {
        return detail::fits_coverage(std::int64_t{region.right} - region.left,
                                     std::int64_t{region.bottom} - region.top);
    };
    const auto paint = [&](Clip region, Clip dest) -> Result<void> {
        ATELIER_TRY_BIND(raster, text.raster(key, width, scale, region));
        if (raster->coverage) {
            Rect ink{x + raster->left, y + raster->top, static_cast<float>(raster->coverage->width),
                     static_cast<float>(raster->coverage->height)};
            if (shadow)
                quad({ink.x + offset, ink.y + offset, ink.width, ink.height}, dest, *shadow, raster->coverage);
            quad(ink, dest, foreground, raster->coverage);
        }
        return {};
    };
    const auto preferred = raster_region(preferred_clip);
    if (fits(preferred))
        return paint(preferred, visible_clip);
    const auto visible = raster_region(visible_clip);
    if (fits(visible))
        return paint(visible, visible_clip);
    // Disjoint destination clips keep foreground and shadow compositing correct across tile boundaries.
    constexpr int tile = 2048;
    for (int top = visible_clip.top; top < visible_clip.bottom; top += tile) {
        for (int left = visible_clip.left; left < visible_clip.right; left += tile) {
            const Clip dest{left, top, std::min(left + tile, visible_clip.right),
                            std::min(top + tile, visible_clip.bottom)};
            ATELIER_TRY(paint(raster_region(dest), dest));
        }
    }
    return {};
}
#include "detail/style.inl"
bool visible(Rect bounds, Clip clip) {
    return bounds.width > 0 && bounds.height > 0 && clip.right > clip.left && clip.bottom > clip.top &&
           bounds.x < clip.right && bounds.y < clip.bottom && bounds.x + bounds.width > clip.left &&
           bounds.y + bounds.height > clip.top;
}

bool valid_wide_text(std::wstring_view text, bool single_line = true) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<std::uint16_t>(text[i]);
        if (!c || (single_line && (c < 32 || c == 127 || c == 0x2028 || c == 0x2029)))
            return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
                return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            return false;
        }
    }
    return true;
}

String wide_to_utf8(std::wstring_view text) {
    auto converted = platform::utf16_to_utf8(text);
    if (!converted)
        fatal("Validated UI wide text failed UTF-8 conversion.");
    return std::move(*converted);
}

StringView wide_to_utf8_scratch(std::wstring_view text, InlineArray<char, 256>& scratch) {
    scratch.clear();
    if (text.empty())
        return {};
    if (text.size() <= 85) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<std::uint32_t>(static_cast<std::uint16_t>(text[i]));
            if (c < 0x80u) {
                scratch.add(static_cast<char>(c));
            } else if (c < 0x800u) {
                scratch.add(static_cast<char>(0xc0u | (c >> 6)));
                scratch.add(static_cast<char>(0x80u | (c & 0x3fu)));
            } else if (c >= 0xd800u && c <= 0xdbffu) {
                if (i + 1 >= text.size())
                    fatal("Validated UI wide text failed UTF-8 conversion.");
                const auto trail = static_cast<std::uint32_t>(static_cast<std::uint16_t>(text[++i]));
                if (trail < 0xdc00u || trail > 0xdfffu)
                    fatal("Validated UI wide text failed UTF-8 conversion.");
                const std::uint32_t cp = 0x10000u + (((c - 0xd800u) << 10) | (trail - 0xdc00u));
                scratch.add(static_cast<char>(0xf0u | (cp >> 18)));
                scratch.add(static_cast<char>(0x80u | ((cp >> 12) & 0x3fu)));
                scratch.add(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
                scratch.add(static_cast<char>(0x80u | (cp & 0x3fu)));
            } else if (c < 0xd800u || c > 0xdfffu) {
                scratch.add(static_cast<char>(0xe0u | (c >> 12)));
                scratch.add(static_cast<char>(0x80u | ((c >> 6) & 0x3fu)));
                scratch.add(static_cast<char>(0x80u | (c & 0x3fu)));
            } else {
                fatal("Validated UI wide text failed UTF-8 conversion.");
            }
        }
        return StringView{scratch.data(), scratch.size()};
    }
    const String heap = wide_to_utf8(text);
    scratch.append(Span<const char>{heap.c_str(), heap.size()});
    return StringView{scratch.data(), scratch.size()};
}

std::wstring utf8_to_wide(StringView text) {
    return platform::utf8_to_utf16_validated(text);
}

String format(double value, bool single_precision = false, bool integral = false) {
    char buffer[64];
    std::to_chars_result result{};
    if (integral && value >= -9007199254740992.0 && value <= 9007199254740992.0 && std::trunc(value) == value) {
        result = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<std::int64_t>(value));
    } else {
        result = single_precision ? std::to_chars(buffer, buffer + sizeof(buffer), static_cast<float>(value))
                                  : std::to_chars(buffer, buffer + sizeof(buffer), value);
    }
    if (result.ec != std::errc{})
        return String::literal<"0">();
    return String::from_utf8_validated({buffer, static_cast<std::size_t>(result.ptr - buffer)});
}

String format(const Control& control, double value) {
    return format(value, control.single_precision, control.integral);
}

double quantize_control_value(const Control& control, double raw) {
    if (!control.integral)
        return raw;
    const double low = std::ceil(control.minimum);
    const double high = std::floor(control.maximum);
    const double res =
        low <= high ? std::clamp(std::round(raw), low, high) : std::clamp(raw, control.minimum, control.maximum);
    return res == 0.0 ? 0.0 : res;
}

Optional<double> number(StringView text, const Control& control) {
    if (text.empty() || text.size() > 64)
        return {};
    for (std::size_t i = 0; i < text.size(); ++i)
        if (static_cast<unsigned char>(text[i]) >= 128u)
            return {};
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    if (*begin == '+')
        ++begin;
    double value{};
    const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value))
        return {};
    if (control.integral && std::trunc(value) != value)
        return {};
    // Like Runtime writes to bounded fields, typed values clamp to the legal range.
    return quantize_control_value(control, std::clamp(value, control.minimum, control.maximum));
}
}

bool Control::operator==(const Control& other) const {
    using detail::number_bits;
    return key == other.key && parent == other.parent && kind == other.kind &&
           number_bits(value) == number_bits(other.value) && number_bits(minimum) == number_bits(other.minimum) &&
           number_bits(maximum) == number_bits(other.maximum) &&
           number_bits(drag_step) == number_bits(other.drag_step) &&
           number_bits(fine_step) == number_bits(other.fine_step) &&
           default_value.transform(number_bits) == other.default_value.transform(number_bits) &&
           layout == other.layout && text == other.text && placeholder == other.placeholder &&
           single_line == other.single_line && role == other.role && icon == other.icon && enabled == other.enabled &&
           selected == other.selected && click_action == other.click_action &&
           single_precision == other.single_precision && integral == other.integral && drag_axis == other.drag_axis &&
           classes == other.classes && style == other.style && style_scope == other.style_scope &&
           invalid == other.invalid && modified == other.modified && context_target == other.context_target &&
           context_group == other.context_group && selection_group == other.selection_group &&
           action_target == other.action_target && tree_row_count == other.tree_row_count &&
           tree_index == other.tree_index && tree_depth == other.tree_depth && tree_branch == other.tree_branch &&
           tree_expanded == other.tree_expanded && tree_lines == other.tree_lines &&
           tree_primary == other.tree_primary && tree_draggable == other.tree_draggable &&
           tree_badges == other.tree_badges && tree_trailing_icon == other.tree_trailing_icon &&
           tree_trailing_dimmed == other.tree_trailing_dimmed && transient_edit == other.transient_edit &&
           rename_anchor == other.rename_anchor && rename_cell == other.rename_cell &&
           popup_owner == other.popup_owner && menu_branch == other.menu_branch && edit_command == other.edit_command &&
           button_behavior == other.button_behavior && menu_behavior == other.menu_behavior &&
           radio_group == other.radio_group && table == other.table && table_selection == other.table_selection &&
           table_layout == other.table_layout && code == other.code &&
           (code_annotations == other.code_annotations ||
            (code_annotations && other.code_annotations && *code_annotations == *other.code_annotations)) &&
           tooltip == other.tooltip &&
           (canvas == other.canvas || (canvas && other.canvas && *canvas == *other.canvas)) &&
           read_only == other.read_only && number_bits(color_hue) == number_bits(other.color_hue) &&
           number_bits(color_value) == number_bits(other.color_value);
}

Result<void> validate_control_properties(const Control& control) {
    if (!control.key || static_cast<unsigned>(control.kind) > static_cast<unsigned>(ControlKind::color_plane) ||
        static_cast<unsigned>(control.role) > static_cast<unsigned>(Role::caption) || control.text.size() > 4096 ||
        control.placeholder.size() > 4096 || !std::isfinite(control.value) || !std::isfinite(control.minimum) ||
        !std::isfinite(control.maximum) || control.minimum > control.maximum || !std::isfinite(control.drag_step) ||
        !std::isfinite(control.fine_step) || control.drag_step <= 0 || control.fine_step <= 0 ||
        !std::isfinite(control.color_hue) || !std::isfinite(control.color_value) ||
        static_cast<unsigned>(control.drag_axis) > static_cast<unsigned>(DragAxis::vertical) ||
        static_cast<unsigned>(control.button_behavior) > static_cast<unsigned>(ButtonBehavior::toggle) ||
        static_cast<unsigned>(control.menu_behavior) > static_cast<unsigned>(MenuBehavior::radio))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if ((!control.placeholder.empty() && control.kind != ControlKind::text_edit) ||
        (control.kind != ControlKind::color_plane && (control.color_hue != 0 || control.color_value != 1)) ||
        (control.kind == ControlKind::color_plane &&
         (control.color_hue < 0 || control.color_hue > 360 || control.color_value < 0 || control.color_value > 1 ||
          (control.drag_axis != DragAxis::vertical && (control.value < 0 || control.value > 1)))))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if (static_cast<unsigned>(control.icon) > static_cast<unsigned>(last_control_icon) ||
        static_cast<unsigned>(control.tree_trailing_icon) > static_cast<unsigned>(last_control_icon) ||
        std::ranges::any_of(
            control.tree_badges,
            [](ControlIcon b) { return static_cast<unsigned>(b) > static_cast<unsigned>(last_control_icon); }) ||
        (control.icon != ControlIcon::none && control.kind != ControlKind::button &&
         control.kind != ControlKind::label && control.kind != ControlKind::splitter &&
         control.kind != ControlKind::combo && control.kind != ControlKind::tree_item &&
         control.kind != ControlKind::menu_item && control.kind != ControlKind::foldout &&
         control.kind != ControlKind::tab) ||
        ((control.transient_edit || control.rename_anchor || control.rename_cell) &&
         control.kind != ControlKind::text_edit) ||
        (control.rename_cell && !control.rename_anchor) ||
        (control.click_action && control.kind != ControlKind::splitter) ||
        (control.action_target && control.kind != ControlKind::button && control.kind != ControlKind::checkbox &&
         control.kind != ControlKind::table) ||
        ((control.kind == ControlKind::table) != static_cast<bool>(control.table)) ||
        ((control.kind == ControlKind::code_editor) != static_cast<bool>(control.code)) ||
        (control.code_annotations && control.kind != ControlKind::code_editor) ||
        (control.read_only && control.kind != ControlKind::code_editor && control.kind != ControlKind::text_edit) ||
        (control.integral &&
         ((control.kind != ControlKind::number && control.kind != ControlKind::slider) || control.single_precision ||
          std::ceil(control.minimum) > std::floor(control.maximum) ||
          (control.kind == ControlKind::number && std::trunc(control.value) != control.value) ||
          (control.default_value && std::trunc(*control.default_value) != *control.default_value))) ||
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
         (control.minimum >= control.maximum || !std::isfinite(control.maximum - control.minimum) ||
          (control.integral && std::ceil(control.minimum) >= std::floor(control.maximum)))))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if (!valid_wide_text(control.text, control.kind == ControlKind::text_edit) ||
        !valid_wide_text(control.placeholder, true))
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if ((control.kind != ControlKind::tree && control.tree_row_count) || control.tree_row_count > maximum_tree_rows ||
        (control.kind != ControlKind::tree_item &&
         (control.tree_index || control.tree_depth || control.tree_branch || control.tree_expanded ||
          control.tree_lines.any() || control.tree_primary || control.tree_trailing_icon != ControlIcon::none ||
          control.tree_trailing_dimmed ||
          std::ranges::any_of(control.tree_badges, [](ControlIcon b) { return b != ControlIcon::none; }))) ||
        control.tree_depth >= 128 || (control.tree_expanded && !control.tree_branch) || control.tree_lines[0] ||
        (control.tree_lines >> (control.tree_depth + 1)).any())
        return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    if (control.tooltip) {
        const auto& help = *control.tooltip;
        const auto multiline = [](std::wstring_view value) {
            return valid_wide_text(value, false) && std::ranges::all_of(value, [](wchar_t c) {
                       return c >= 32 || c == L'\n' || c == L'\r' || c == L'\t';
                   });
        };
        if (!valid_wide_text(help.title) || !valid_wide_text(help.identity) || !multiline(help.description))
            return std::unexpected(Error{ErrorCode::invalid_control, control.key});
    }
    if (control.canvas) {
        if (control.kind != ControlKind::container)
            return std::unexpected(Error{ErrorCode::invalid_control, control.key});
        const auto& c = *control.canvas;
        if (!std::isfinite(c.grid_step) || c.grid_step < 0 || c.grid_step > 4096 || !std::isfinite(c.grid_origin_x) ||
            !std::isfinite(c.grid_origin_y) || !valid_color(c.grid_minor) || !valid_color(c.grid_major) ||
            !valid_color(c.grid_axis) || c.boxes.size() > CanvasData::max_boxes ||
            c.strokes.size() > CanvasData::max_strokes || c.shapes.size() > CanvasData::max_shapes ||
            c.arcs.size() > CanvasData::max_arcs)
            return std::unexpected(Error{ErrorCode::invalid_control, control.key});
        for (const auto& b : c.boxes) {
            if (!std::isfinite(b.bounds.x) || !std::isfinite(b.bounds.y) || std::abs(b.bounds.x) > 1e6f ||
                std::abs(b.bounds.y) > 1e6f || !std::isfinite(b.bounds.width) || !std::isfinite(b.bounds.height) ||
                b.bounds.width < 0 || b.bounds.height < 0 || !valid_color(b.fill) || !valid_color(b.border_color) ||
                !std::isfinite(b.border_width) || b.border_width < 0 || b.border_width > 64)
                return std::unexpected(Error{ErrorCode::invalid_control, control.key});
            for (float r : {b.radius.top_left, b.radius.top_right, b.radius.bottom_right, b.radius.bottom_left})
                if (!std::isfinite(r) || r < 0 || r > 4096)
                    return std::unexpected(Error{ErrorCode::invalid_control, control.key});
        }
        for (const auto& sh : c.shapes) {
            if (sh.shape > 7 || sh.slot > 4 || !std::isfinite(sh.bounds.x) || !std::isfinite(sh.bounds.y) ||
                std::abs(sh.bounds.x) > 1e6f || std::abs(sh.bounds.y) > 1e6f || !std::isfinite(sh.bounds.width) ||
                !std::isfinite(sh.bounds.height) || sh.bounds.width < 0 || sh.bounds.height < 0 ||
                !std::isfinite(sh.strip_start) || !std::isfinite(sh.strip_end) || sh.strip_start < 0 ||
                sh.strip_end < sh.strip_start || sh.strip_end > 4096 || !valid_color(sh.fill) ||
                !valid_color(sh.border_color) || !std::isfinite(sh.border_width) || sh.border_width < 0 ||
                sh.border_width > 64)
                return std::unexpected(Error{ErrorCode::invalid_control, control.key});
        }
        for (const auto& a : c.arcs) {
            if (!std::isfinite(a.cx) || !std::isfinite(a.cy) || std::abs(a.cx) > 1e6f || std::abs(a.cy) > 1e6f ||
                !std::isfinite(a.r_in) || !std::isfinite(a.r_out) || a.r_in < 0 || a.r_out < a.r_in || a.r_out > 8192 ||
                !std::isfinite(a.dir_x) || !std::isfinite(a.dir_y) || std::abs(a.dir_x) > 2.f ||
                std::abs(a.dir_y) > 2.f || !std::isfinite(a.cos_half) || !std::isfinite(a.sin_half) ||
                std::abs(a.cos_half) > 4.f || a.sin_half < 0.f || a.sin_half > 2.f || !valid_color(a.fill) ||
                !valid_color(a.border_color) || !std::isfinite(a.border_width) || a.border_width < 0 ||
                a.border_width > 64)
                return std::unexpected(Error{ErrorCode::invalid_control, control.key});
        }
        for (const auto& s : c.strokes) {
            if (!std::isfinite(s.x0) || !std::isfinite(s.y0) || !std::isfinite(s.x1) || !std::isfinite(s.y1) ||
                std::abs(s.x0) > 1e6f || std::abs(s.y0) > 1e6f || std::abs(s.x1) > 1e6f || std::abs(s.y1) > 1e6f ||
                !std::isfinite(s.width) || s.width < 0 || s.width > max_canvas_stroke_dip || !valid_color(s.color))
                return std::unexpected(Error{ErrorCode::invalid_control, control.key});
        }
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
    if (!key)
        return nullptr;
    if (control_index.size() == controls.size()) {
        const auto at = std::ranges::lower_bound(control_index, key, {}, &std::pair<ControlKey, std::uint32_t>::first);
        return at != control_index.end() && at->first == key ? &controls[at->second] : nullptr;
    }
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

std::optional<std::uint32_t> UiFrame::code_offset_at(ControlKey key, float x, float y) const {
    const auto* geometry = find_code(key);
    if (!geometry || geometry->lines.empty() || x < geometry->body.x || x >= geometry->body.x + geometry->body.width ||
        y < geometry->body.y || y >= geometry->body.y + geometry->body.height)
        return std::nullopt;
    for (const auto& line : geometry->lines) {
        if (y >= line.y && y < line.y + geometry->line_height) {
            if (!line.paragraph)
                return std::nullopt;
            const auto pos = line.paragraph->hit((x - line.x) / geometry->scale, (y - line.y) / geometry->scale);
            if (!pos)
                return std::nullopt;
            return line.start + pos->offset;
        }
    }
    return std::nullopt;
}

struct Session::Impl {
    using TableIndex = Map<TableKey, std::size_t>;

    struct State {
        Control control;

        // Indices into one immutable frame. A discarded candidate can only cause a cache miss.
        struct PaintRange {
            std::uint64_t frame_id{};

            std::size_t box_index{};

            std::size_t draw_begin{};

            std::size_t draw_count{};

            std::size_t text_index{};

            ui_layout::Size size;

            bool baseline{};
        } paint_range;

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

        std::uint64_t code_comment_revision{};

        CodeLanguage code_comment_lang{CodeLanguage::plain};

        Array<std::uint8_t> code_block_comments;

        std::uint64_t code_scope_revision{std::numeric_limits<std::uint64_t>::max()};

        CodeLanguage code_scope_lang{CodeLanguage::plain};

        std::uint32_t code_scope_caret{std::numeric_limits<std::uint32_t>::max()};

        Optional<detail::CodeBracketMatch> code_bracket_match;

        detail::ActiveIndentScope code_active_scope{0, 0, std::numeric_limits<std::uint32_t>::max()};

        float code_width{};

        Array<detail::TextKey> table_text; // Visible cells only; old frames retain their own coverage.

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

        Array<std::shared_ptr<const StyleSheet>> sheets;

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

        Optional<Ladder> ladder{};

        float slider_span{1}; // Accepted physical track width, frozen until release.

        double raw_value{}; // Continuous unquantized drag accumulator for integral controls.

        bool tree_disclosure{};

        bool tree_trailing{};

        bool scrollbar{}; // Scroll offset belongs to the UI, never a document edit.

        bool horizontal{};

        TableSelection table_cell{};

        TableKey table_column{};

        bool table_resize{};

        bool table_activate{};

        Rect color_bounds{};

        DragPoint color_sv_original{};

        DragPoint color_sv{};

        std::uint8_t code_select_mode{}; // 0 = char, 1 = word, 2 = line.

        std::uint32_t code_anchor_start{};

        std::uint32_t code_anchor_end{};
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

        Optional<detail::TextEdit> field_buffer;

        std::shared_ptr<CodeDocument> document;

        std::uint64_t document_selection{};

        detail::TextEdit& buffer() { return document ? document->edit() : *field_buffer; }

        const detail::TextEdit& buffer() const { return document ? document->edit() : *field_buffer; }

        double original_value{};

        double preview_value{};

        bool text_mode{};

        float scroll{};

        Optional<Composition> composition;

        Optional<ClipboardRequest> clipboard;

        Optional<ImeRequest> ime_request;

        bool commit_paste{};

        Edit(const State& state, double numeric, bool mode)
            : key(state.control.key), token{key, state.incarnation, detail::new_identity(), detail::new_identity()},
              document(state.control.code), original_value(numeric), preview_value(numeric),
              text_mode(mode || document != nullptr) {
            if (document)
                document_selection = buffer().selection_revision();
        }

        static Result<Edit> create(const State& state, String value, double numeric, bool mode, bool select_all) {
            Edit result(state, numeric, mode);
            if (!result.document) {
                ATELIER_TRY_BIND(
                    buffer, detail::TextEdit::create(
                                std::move(value), state.control.kind == ControlKind::number ? 64 : 16384, select_all));
                result.field_buffer.emplace(std::move(*buffer));
            }
            return result;
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

    Array<ControlKey> order;

    bool structure_dirty{true};

    std::shared_ptr<const UiFrame> published;

    Optional<Press> press;

    Optional<Edit> edit;

    ControlKey focus{};

    ControlKey hover{};

    float hover_x{};

    float hover_y{};

    std::optional<ClipboardRequest> table_clipboard;

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

        ui_layout::MeasuredSize identity_size{};

        ui_layout::MeasuredSize body_size{};

        detail::RasterText title;

        detail::RasterText identity;

        detail::RasterText body;
    };

    Optional<TooltipHover> tooltip_hover;

    Optional<TooltipCache> tooltip_cache;

    std::shared_ptr<const ColorImage> hue_bar_image;

    double sv_cached_hue{-1.0};

    std::shared_ptr<const ColorImage> sv_plane_image;

    const std::shared_ptr<const ColorImage>& ensure_hue_bar() {
        if (!hue_bar_image) {
            auto img = std::make_shared<ColorImage>();
            img->identity = detail::new_identity();
            img->revision = 1;
            img->width = 1;
            img->height = 128;
            img->row_pitch = 4;
            img->pixels.resize(128 * 4);
            for (unsigned y = 0; y < 128; ++y) {
                const auto rgb = hsv_linear_rgb(static_cast<double>(y) / 127.0 * 360.0, 1.f, 1.f);
                img->pixels[y * 4 + 0] = encode_srgb_byte(rgb[0]);
                img->pixels[y * 4 + 1] = encode_srgb_byte(rgb[1]);
                img->pixels[y * 4 + 2] = encode_srgb_byte(rgb[2]);
                img->pixels[y * 4 + 3] = 255;
            }
            hue_bar_image = std::move(img);
        }
        return hue_bar_image;
    }

    const std::shared_ptr<const ColorImage>& ensure_sv_plane(double hue) {
        if (!sv_plane_image || std::abs(sv_cached_hue - hue) > 1e-4) {
            auto img = std::make_shared<ColorImage>();
            img->identity = detail::new_identity();
            img->revision = 1;
            img->width = 64;
            img->height = 64;
            img->row_pitch = 64 * 4;
            img->pixels.resize(64 * 64 * 4);
            for (unsigned y = 0; y < 64; ++y) {
                const float v = 1.f - static_cast<float>(y) / 63.f;
                for (unsigned x = 0; x < 64; ++x) {
                    const float s = static_cast<float>(x) / 63.f;
                    const auto rgb = hsv_linear_rgb(hue, s, v);
                    const std::size_t idx = (y * 64 + x) * 4;
                    img->pixels[idx + 0] = encode_srgb_byte(rgb[0]);
                    img->pixels[idx + 1] = encode_srgb_byte(rgb[1]);
                    img->pixels[idx + 2] = encode_srgb_byte(rgb[2]);
                    img->pixels[idx + 3] = 255;
                }
            }
            sv_cached_hue = hue;
            sv_plane_image = std::move(img);
        }
        return sv_plane_image;
    }

    Impl() { static_cast<void>(layout.set_root({ui_layout::Kind::overlay})); }

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
        table_clipboard.reset();
        focus = next;
        reply.redraw = true;
    }
#include "detail/session_tooltip.inl"
#include "detail/session_edit.inl"
#include "detail/session_code.inl"
#include "detail/session_popup.inl"
#include "detail/session_tree.inl"
#include "detail/session_table.inl"
#include "detail/session_update.inl"
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
        auto sheets = parent ? parent->sheets : Array<std::shared_ptr<const StyleSheet>>{};
        if (!parent && style_sheet)
            sheets.add(style_sheet);
        if (control.style_scope)
            sheets.add(control.style_scope);
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
        if (state.control.rename_anchor) {
            input.out_of_flow = true;
            input.anchor_x = input.anchor_y = input.pivot_x = input.pivot_y = input.offset_x = input.offset_y = 0;
            input.width = ui_layout::Axis::fixed(padding.left + padding.right + 1.f);
            input.height = ui_layout::Axis::fixed(padding.top + padding.bottom + 1.f);
        } else if (state.control.kind == ControlKind::tree_item) {
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
            if (text_field(state.control.kind)) {
                const String content_u8 = display(state);
                input.content_revision =
                    text.set(state.control.key, content_u8.view(), appearance.font, false, appearance.text_align);
            } else {
                InlineArray<char, 256> u8_scratch;
                const StringView content_u8 = wide_to_utf8_scratch(state.control.text, u8_scratch);
                input.content_revision = text.set(
                    state.control.key, content_u8, appearance.font,
                    state.control.kind == ControlKind::label && !state.control.single_line, appearance.text_align,
                    (state.control.kind == ControlKind::label && state.control.single_line) ||
                        state.control.kind == ControlKind::tree_item);
            }
        } else if (state.control.kind == ControlKind::viewport || state.control.kind == ControlKind::splitter ||
                   state.control.kind == ControlKind::slider || state.control.kind == ControlKind::table ||
                   state.control.kind == ControlKind::color_plane) {
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
        Array<ui_layout::TreeNodeInput> tree;
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
                tree.add({state.node, owner ? Optional<std::size_t>{owner->layout_position} : Optional<std::size_t>{},
                          std::move(input)});
            } else {
                ATELIER_TRY(checked(layout.update(state.node, input), key));
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

Session::Session() = default;

Session::~Session() = default;

Result<void> Session::initialize(Span<const std::wstring> fonts) {
    auto candidate = std::make_unique<Impl>();
    ATELIER_TRY(candidate->text.initialize(fonts));
    impl_ = std::move(candidate);
    return {};
}

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
    if (!std::isfinite(surface.dpi_scale) || surface.dpi_scale < .25f || surface.dpi_scale > max_dpi_scale ||
        !surface.revision || surface.extent.width > 32768 || surface.extent.height > 32768)
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

std::uint64_t ControlList::new_region_revision() {
    return detail::new_identity();
}

void ControlList::begin(std::uint64_t decoration_revision) {
    if (!finished_ || decoration_revision != decoration_revision_)
        regions_.clear();
    if (!submitted_)
        baseline_ = 0;
    decoration_revision_ = decoration_revision;
    finished_ = submitted_ = false;
    region_ = count_ = 0;
    dirty_.clear();
}

bool ControlList::advance(std::size_t count, std::uint64_t revision) {
    const Range range{count_, count};
    count_ += count;
    if (region_ == regions_.size())
        regions_.emplace();
    auto& previous = regions_[region_++];
    if (revision && previous.revision == revision && previous.range.first == range.first &&
        previous.range.count == count)
        return false;
    previous = {range, revision};
    if (count) {
        if (!dirty_.empty() && dirty_.back().first + dirty_.back().count == range.first)
            dirty_.back().count += count;
        else
            dirty_.add(range);
    }
    return true;
}

void ControlList::append(Span<const Control> controls, std::uint64_t revision) {
    const auto first = count_;
    if (!advance(controls.size(), revision))
        return;
    const auto overlap = std::min(controls.size(), values_.size() - first);
    std::copy_n(controls.begin(), overlap, values_.begin() + first);
    values_.append(controls.slice(overlap));
}

void ControlList::append(Array<Control>&& controls) {
    const auto first = count_;
    advance(controls.size(), 0);
    const auto overlap = std::min(controls.size(), values_.size() - first);
    auto middle = controls.begin() + overlap;
    std::move(controls.begin(), middle, values_.begin() + first);
    values_.append(std::ranges::subrange(std::make_move_iterator(middle), std::make_move_iterator(controls.end())));
}

Result<Reply> Session::set_controls(ControlList& list) {
    const auto controls = list.controls();
    const auto submit = [&]() -> Result<Reply> {
        if (!list.finished_)
            return std::unexpected(Error{ErrorCode::invalid_control});
        if (list.baseline_ != impl_->controls_revision || controls.size() != impl_->order.size())
            return set_controls(controls);
        Array<Control> changed;
        for (auto range : list.dirty_) {
            for (std::size_t i = range.first; i < range.first + range.count; ++i) {
                const auto& proposed = controls[i];
                if (proposed.key != impl_->order[i])
                    return impl_->apply_controls(controls, false);
                const auto& previous = impl_->states.at(proposed.key).control;
                if (proposed == previous)
                    continue;
                if (!Impl::same_structure(previous, proposed))
                    return impl_->apply_controls(controls, false);
                changed.add(proposed);
            }
        }
        return changed.empty() ? Result<Reply>{Reply{}} : update_controls(changed);
    };
    auto result = submit();
    list.submitted_ = result.has_value();
    list.baseline_ = result ? impl_->controls_revision : 0;
    return result;
}

Result<Reply> Session::set_controls(Span<const Control> controls) {
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

Result<Reply> Session::update_controls(Span<const Control> controls) {
    return impl_->apply_controls(controls, true);
}

Result<std::shared_ptr<const UiFrame>> Session::prepare(Preparation preparation) {
    auto& self = *impl_;
    const bool paint = preparation == Preparation::complete;
    ++self.prepares;
    const auto frame_id = detail::new_identity();
    ScopeExit attempt{[&]() noexcept { self.failed_frame_id = frame_id; }};
    if (!self.surface.extent.width || !self.surface.extent.height)
        return std::unexpected(Error{ErrorCode::unavailable_surface});
    self.sync_code_edit();
    ATELIER_TRY(self.synchronize());
    const float scale = self.surface.dpi_scale;
    const auto geometry = self.layout.compute({self.surface.extent.width / scale, self.surface.extent.height / scale},
                                              self.text.provider_revision());
    if (!geometry)
        return std::unexpected(Error{ErrorCode::layout_failed, 0, geometry.error().provider_code});
    auto output = std::make_shared<UiFrame>();
    std::shared_ptr<Frame> draws;
    output->controls.reserve(self.order.size());
    if (paint) {
        draws = std::make_shared<Frame>();
        // Canvas primitives can greatly outnumber controls. Use the last accepted size, not its capacity,
        // so a sparse frame immediately lowers the estimate for its successor.
        const auto previous_draws = self.published ? self.published->drawing->draws.size() : 0;
        draws->draws.reserve(std::max(self.order.size() * 3, previous_draws));
        draws->id = frame_id;
        draws->extent = self.surface.extent;
        draws->dpi_scale = scale;
        draws->surface_revision = self.surface.revision;
    }
    output->session_identity = self.identity;
    output->controls_revision = self.controls_revision;
    output->style_revision = self.style_revision;
    output->measure_provider_revision = self.text.provider_revision();
    const Clip target{0, 0, static_cast<std::int32_t>(self.surface.extent.width),
                      static_cast<std::int32_t>(self.surface.extent.height)};
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
                    state.content_bottom =
                        std::max(state.content_bottom,
                                 static_cast<float>(padding.top + static_cast<double>(state.control.tree_row_count) *
                                                                      state.resolved->tree_row_height));
                state.viewport_height = std::max(0.f, box->height - padding.top - padding.bottom);
                const auto content_end = state.content_bottom + padding.bottom;
                const auto overflow = content_end - box->height;
                // Measurement and placement can accumulate fractional rows in
                // different orders. Discard float roundoff, not real overflow.
                const auto roundoff =
                    4 * std::numeric_limits<float>::epsilon() * std::max(1.f, std::max(content_end, box->height));
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
            draws->draws.add({bounds, clip, color, {0, 0, 1, 1}, std::move(coverage)});
    };
    const auto paint_box = [&](Rect bounds, Clip clip, Color background, std::shared_ptr<const ColorImage> image,
                               CornerRadii radius, float border, Color edge, BorderStyle relief,
                               BorderEdges edges = BorderEdges::all, Optional<Color> inner = {},
                               Optional<Color> shadow = {}) {
        const bool beveled = relief != BorderStyle::solid;
        const bool square = beveled || edges != BorderEdges::all || radius == CornerRadii{};
        if (square && border > 0) {
            const auto right = std::round(bounds.x + bounds.width);
            const auto bottom = std::round(bounds.y + bounds.height);
            bounds.x = std::round(bounds.x);
            bounds.y = std::round(bounds.y);
            bounds.width = std::max(0.f, right - bounds.x);
            bounds.height = std::max(0.f, bottom - bounds.y);
            border = std::min(std::max(1.f, std::round(border)), std::min(bounds.width * .5f, bounds.height * .5f));
        }
        const bool separate = beveled || edges != BorderEdges::all;
        draws->draws.add({bounds,
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
    const auto prepare_scrollbar = [&](ScrollbarGeometry bar, const ResolvedStyle& appearance, bool enabled) {
        const auto width = bar.horizontal ? bar.bounds.height : bar.bounds.width;
        const auto span = bar.horizontal ? bar.bounds.width : bar.bounds.height;
        if (bar.maximum <= 0 || width <= 0 || span <= 0)
            return;
        const auto step =
            appearance.scrollbar_button_size > 0 ? std::min(appearance.scrollbar_button_size * scale, span / 3) : 0.f;
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
        output->scrollbars.add(bar);
        if (!paint)
            return;
        const auto stroke = std::max(1.f, std::round(scale * 2.f / 3));
        const Color shadow{0, 0, 0, appearance.indicator_border.a};
        const float r = std::min(1.5f * scale, width * .5f);
        const CornerRadii radii{r, r, r, r};
        const bool dragging = self.press && self.press->scrollbar && self.press->key == bar.key &&
                              self.press->horizontal == bar.horizontal;
        const bool hot = enabled && (dragging || self.hover == bar.key);
        auto thumb_fill = appearance.scrollbar;
        if (hot) {
            const float boost = dragging ? .18f : .09f;
            thumb_fill.r = std::min(1.f, thumb_fill.r + boost);
            thumb_fill.g = std::min(1.f, thumb_fill.g + boost);
            thumb_fill.b = std::min(1.f, thumb_fill.b + boost);
        }
        paint_box(bar.track, bar.clip, appearance.indicator_background, {}, radii, stroke, shadow, BorderStyle::solid);
        paint_box(bar.thumb, bar.clip, thumb_fill, appearance.scrollbar_image, radii, stroke,
                  appearance.indicator_border, BorderStyle::solid);
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
            const auto symbol = bar.horizontal
                                    ? (decrement ? detail::TextStore::Symbol::left : detail::TextStore::Symbol::right)
                                    : (decrement ? detail::TextStore::Symbol::up : detail::TextStore::Symbol::down);
            quad({button.x + (button.width - size) * .5f, button.y + (button.height - size) * .5f, size, size},
                 bar.clip, arrow, self.text.symbol(symbol, static_cast<std::uint32_t>(std::ceil(size))));
        }
    };
    const auto* accepted = self.published.get();
    const bool same_surface = paint && accepted && accepted->drawing->surface_revision == self.surface.revision &&
                              accepted->drawing->dpi_scale == scale &&
                              accepted->drawing->extent == self.surface.extent &&
                              accepted->measure_provider_revision == output->measure_provider_revision &&
                              accepted->style_revision == self.style_revision;
    std::size_t popup_draw_begin = std::numeric_limits<std::size_t>::max();
    for (auto key : self.order) {
        auto& state = self.states.at(key);
        if (state.control.kind == ControlKind::popup && self.popup_dismissed)
            continue;
        const auto* box = geometry->find(state.node);
        if (!box)
            continue;
        float box_width = box->width;
        float box_height = box->height;
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
                    clip.right = std::min(clip.right,
                                          static_cast<std::int32_t>(
                                              std::floor(parent_box.bounds.x + parent_box.bounds.width -
                                                         (style.scrollbar_width + style.scrollbar_inset * 2) * scale)));
            }
        }
        if (state.control.rename_anchor) {
            const auto& anchor_state = self.states.at(state.control.rename_anchor);
            if (anchor_state.frame_index == std::numeric_limits<std::size_t>::max())
                continue;
            const auto& anchor_box = output->controls[anchor_state.frame_index];
            enabled = enabled && anchor_box.enabled;
            if (!enabled)
                continue;
            if (anchor_state.control.kind == ControlKind::tree_item) {
                if (anchor_box.bounds.height <= 0 || !visible(anchor_box.bounds, anchor_box.clip))
                    continue;
                const auto row_pad = self.padding_for(anchor_state);
                const float left_dip = std::max(0.f, row_pad.left - 3.f);
                bounds = {anchor_box.bounds.x + left_dip * scale, anchor_box.bounds.y,
                          std::max(36.f * scale, anchor_box.bounds.width - (left_dip + row_pad.right) * scale),
                          anchor_box.bounds.height};
                clip = intersect(anchor_box.clip, clip_rect(anchor_box.bounds));
            } else {
                const auto* table_geom = output->find_table(state.control.rename_anchor);
                if (!table_geom)
                    continue;
                const auto cell_it =
                    std::ranges::find(table_geom->cells, state.control.rename_cell, &TableCellGeometry::cell);
                if (cell_it == table_geom->cells.end() || !visible(cell_it->bounds, cell_it->clip))
                    continue;
                enabled = enabled && Impl::table_row_enabled(anchor_state, cell_it->row);
                if (!enabled)
                    continue;
                ControlIcon cell_icon = ControlIcon::none;
                if (anchor_state.control.table) {
                    if (const auto* r_idx = anchor_state.table_index.find(cell_it->row);
                        r_idx && *r_idx < anchor_state.control.table->rows.size()) {
                        for (const auto& c : anchor_state.control.table->rows[*r_idx].cells)
                            if (c.key == cell_it->cell) {
                                cell_icon = c.icon;
                                break;
                            }
                    }
                }
                const auto& table_style = *anchor_state.resolved;
                if (anchor_state.control.table_layout.view == TableView::tiles) {
                    float used = 0.f;
                    if (cell_icon != ControlIcon::none) {
                        const float icon_sz =
                            std::min(table_style.indicator_size * scale,
                                     std::min(cell_it->bounds.height - 2 * scale, cell_it->bounds.width));
                        if (icon_sz > 0.f)
                            used = std::min(cell_it->bounds.height, icon_sz + 8 * scale);
                    }
                    const float rem_h = std::max(0.f, cell_it->bounds.height - used);
                    const float band_h = std::min(22.f * scale, std::max(16.f * scale, rem_h));
                    const float band_y = cell_it->bounds.y + used + std::max(0.f, rem_h - band_h) * .5f;
                    bounds = {cell_it->bounds.x + 2 * scale, band_y,
                              std::max(32.f * scale, cell_it->bounds.width - 4 * scale), band_h};
                } else {
                    float lead = std::max(4.f, table_style.padding.left) * scale;
                    if (cell_icon != ControlIcon::none) {
                        const float icon_sz =
                            std::min(table_style.indicator_size * scale, cell_it->bounds.height - 2 * scale);
                        if (icon_sz > 0.f)
                            lead += icon_sz + 2 * scale;
                    }
                    bounds = {cell_it->bounds.x + lead, cell_it->bounds.y,
                              std::max(36.f * scale, cell_it->bounds.width - lead - 2 * scale), cell_it->bounds.height};
                }
                clip = cell_it->clip;
            }
            box_width = bounds.width / scale;
            box_height = bounds.height / scale;
        }
        if (state.control.kind == ControlKind::popup) {
            bounds.x =
                std::clamp(bounds.x, 0.f, std::max(0.f, static_cast<float>(self.surface.extent.width) - bounds.width));
            bounds.y = std::clamp(bounds.y, 0.f,
                                  std::max(0.f, static_cast<float>(self.surface.extent.height) - bounds.height));
            output->popup = key;
            popup_draw_begin = paint ? draws->draws.size() : 0;
        }
        // Cap before conversion to integer clip; layout can legally overflow.
        if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || std::abs(bounds.x) > 1e8f ||
            std::abs(bounds.y) > 1e8f || bounds.width > 1e8f || bounds.height > 1e8f)
            return std::unexpected(Error{ErrorCode::invalid_control, key});
        state.frame_index = output->controls.size();
        output->controls.add({key,
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
            Rect trailing_rect{};
            if (state.control.tree_trailing_icon != ControlIcon::none) {
                const float right_edge = std::min(bounds.x + bounds.width, static_cast<float>(clip.right)) -
                                         (state.control.layout.padding.right + state.resolved->padding.right) * scale;
                const float slot_w = 18.f * scale;
                trailing_rect = {right_edge - slot_w, bounds.y, slot_w, bounds.height};
            }
            output->tree_rows.add({key, state.control.tree_index, state.control.tree_depth,
                                   state.control.tree_branch ? Rect{x, bounds.y, indent, bounds.height} : Rect{},
                                   trailing_rect, state.control.tree_expanded});
        }
        // Keep every arranged box and its parent transform. Only painting is culled.
        if (!visible(bounds, clip)) {
            if (text_kind(kind))
                ++self.culled_text_controls;
            continue;
        }
        // Popup controls are reordered after scrollbars, outside the contiguous base paint ranges.
        const bool retain = paint && retains_local_paint(kind) && !state.control.rename_anchor &&
                            popup_draw_begin == std::numeric_limits<std::size_t>::max() &&
                            !(self.edit && self.edit->key == key) && !(self.press && self.press->key == key);
        const ui_layout::Size size{box_width, box_height};
        if (retain) {
            const auto previous = std::exchange(
                state.paint_range, Impl::State::PaintRange{frame_id, state.frame_index, draws->draws.size(), 0,
                                                           output->text_geometry.size(), size, state.baseline_content});
            if (same_surface && previous.frame_id == accepted->drawing->id && previous.size == size &&
                previous.baseline == state.baseline_content) {
                const auto& prior = accepted->controls[previous.box_index];
                if (prior.bounds == bounds && prior.clip == clip && prior.style == state.resolved &&
                    prior.enabled == enabled) {
                    const auto range = accepted->drawing->draws.slice(previous.draw_begin, previous.draw_count);
                    draws->draws.append(range);
                    if (text_field(kind))
                        output->text_geometry.add(accepted->text_geometry[previous.text_index]);
                    state.paint_range.draw_count = range.size();
                    continue;
                }
            }
        }
        const auto& appearance = *state.resolved;
        const float border =
            std::min(appearance.border_width * scale, std::min(bounds.width * .5f, bounds.height * .5f));
        // Resolve text geometry once for either mode; painting consumes the
        // same width/metrics rather than resolving an alternate layout path.
        const auto text_padding = text_kind(kind) ? self.padding_for(state) : ui_layout::Insets{};
        const float text_width = std::max(0.f, box_width - (text_padding.left + text_padding.right));
        ATELIER_TRY_BIND(text_metrics_result, text_kind(kind)
                                                  ? self.text.metrics(key, text_width)
                                                  : Result<ui_layout::MeasuredSize>{ui_layout::MeasuredSize{}});
        const auto& text_metrics = *text_metrics_result;
        if (paint) {
            const CornerRadii radius{appearance.radius.top_left * scale, appearance.radius.top_right * scale,
                                     appearance.radius.bottom_right * scale, appearance.radius.bottom_left * scale};
            if (appearance.background || appearance.image || (border > 0 && appearance.border.a > 0)) {
                const auto background = appearance.background.value_or(Color{0, 0, 0, 0});
                paint_box(bounds, clip, background, appearance.image, radius, border, appearance.border,
                          appearance.border_style, appearance.border_edges, appearance.border_inner,
                          appearance.border_shadow);
            }
            if (state.control.canvas) {
                const auto& c = *state.control.canvas;
                const auto canvas_clip = intersect(target, intersect(clip, clip_rect(bounds)));
                if (canvas_clip.right > canvas_clip.left && canvas_clip.bottom > canvas_clip.top) {
                    if (c.grid_step * scale >= 6.f) {
                        const float step_px = c.grid_step * scale;
                        const float phase_x = std::fmod(c.grid_origin_x, c.grid_step * 5.f);
                        const float ox = bounds.x + phase_x * scale;
                        const double axis_x =
                            std::round((static_cast<double>(c.grid_origin_x) - phase_x) / c.grid_step);
                        const float phase_y = std::fmod(c.grid_origin_y, c.grid_step * 5.f);
                        const float oy = bounds.y + phase_y * scale;
                        const double axis_y =
                            std::round((static_cast<double>(c.grid_origin_y) - phase_y) / c.grid_step);
                        const float line_w = std::max(1.f, std::round(scale * .667f));
                        const int ix0 = static_cast<int>(std::floor((canvas_clip.left - ox) / step_px));
                        const int ix1 = static_cast<int>(std::ceil((canvas_clip.right - ox) / step_px));
                        for (int ix = ix0; ix <= ix1; ++ix) {
                            const Color col = ix == axis_x && c.grid_axis.a > 0   ? c.grid_axis
                                              : ix % 5 == 0 && c.grid_major.a > 0 ? c.grid_major
                                                                                  : c.grid_minor;
                            if (col.a > 0)
                                quad({std::round(ox + ix * step_px), static_cast<float>(canvas_clip.top), line_w,
                                      static_cast<float>(canvas_clip.bottom - canvas_clip.top)},
                                     canvas_clip, col);
                        }
                        const int iy0 = static_cast<int>(std::floor((canvas_clip.top - oy) / step_px));
                        const int iy1 = static_cast<int>(std::ceil((canvas_clip.bottom - oy) / step_px));
                        for (int iy = iy0; iy <= iy1; ++iy) {
                            const Color col = iy == axis_y && c.grid_axis.a > 0   ? c.grid_axis
                                              : iy % 5 == 0 && c.grid_major.a > 0 ? c.grid_major
                                                                                  : c.grid_minor;
                            if (col.a > 0)
                                quad({static_cast<float>(canvas_clip.left), std::round(oy + iy * step_px),
                                      static_cast<float>(canvas_clip.right - canvas_clip.left), line_w},
                                     canvas_clip, col);
                        }
                    }
                    for (const auto& b : c.boxes) {
                        if (b.bounds.width <= 0 || b.bounds.height <= 0 ||
                            (b.fill.a == 0 && (b.border_width <= 0 || b.border_color.a == 0)))
                            continue;
                        const Rect rect{bounds.x + b.bounds.x * scale, bounds.y + b.bounds.y * scale,
                                        b.bounds.width * scale, b.bounds.height * scale};
                        if (!visible(rect, canvas_clip))
                            continue;
                        const CornerRadii box_radius{b.radius.top_left * scale, b.radius.top_right * scale,
                                                     b.radius.bottom_right * scale, b.radius.bottom_left * scale};
                        const float bw =
                            std::min(b.border_width * scale, std::min(rect.width * .5f, rect.height * .5f));
                        draws->draws.add(
                            {rect, canvas_clip, b.fill, {0, 0, 1, 1}, {}, box_radius, bw, b.border_color});
                    }
                    for (const auto& a : c.arcs) {
                        if (a.r_out <= a.r_in || (a.fill.a == 0 && (a.border_width <= 0 || a.border_color.a == 0)))
                            continue;
                        const float cx = bounds.x + a.cx * scale;
                        const float cy = bounds.y + a.cy * scale;
                        const float ri = a.r_in * scale;
                        const float ro = a.r_out * scale;
                        const float pad = ro + 2.f;
                        const Rect aabb{cx - pad, cy - pad, pad * 2.f, pad * 2.f};
                        if (!visible(aabb, canvas_clip))
                            continue;
                        const float bw = std::min(a.border_width * scale, (ro - ri) * .5f);
                        draws->draws.add({aabb,
                                          canvas_clip,
                                          a.fill,
                                          {cx, cy, ri, ro},
                                          {},
                                          {a.dir_x, a.dir_y, a.cos_half, a.sin_half},
                                          bw,
                                          a.border_color,
                                          {},
                                          {},
                                          0.f,
                                          ProceduralQuad::arc_sector});
                    }
                    for (const auto& sh : c.shapes) {
                        if (sh.bounds.width <= 0 || sh.bounds.height <= 0 ||
                            (sh.fill.a == 0 && (sh.border_width <= 0 || sh.border_color.a == 0)))
                            continue;
                        const Rect rect{bounds.x + sh.bounds.x * scale, bounds.y + sh.bounds.y * scale,
                                        sh.bounds.width * scale, sh.bounds.height * scale};
                        if (!visible(rect, canvas_clip))
                            continue;
                        const float bw =
                            std::min(sh.border_width * scale, std::min(rect.width * .5f, rect.height * .5f));
                        draws->draws.add({rect,
                                          canvas_clip,
                                          sh.fill,
                                          {static_cast<float>(sh.shape), static_cast<float>(sh.slot),
                                           sh.strip_start * scale, sh.strip_end * scale},
                                          {},
                                          {},
                                          bw,
                                          sh.border_color,
                                          {},
                                          {},
                                          0.f,
                                          ProceduralQuad::node_shape});
                    }
                    for (const auto& s : c.strokes) {
                        if (s.width <= 0 || s.color.a == 0)
                            continue;
                        const float ax = bounds.x + s.x0 * scale;
                        const float ay = bounds.y + s.y0 * scale;
                        const float bx = bounds.x + s.x1 * scale;
                        const float by = bounds.y + s.y1 * scale;
                        const float w = s.width * scale;
                        const float pad = w * .5f + 1.f;
                        const Rect aabb{std::min(ax, bx) - pad, std::min(ay, by) - pad, std::abs(bx - ax) + 2.f * pad,
                                        std::abs(by - ay) + 2.f * pad};
                        if (visible(aabb, canvas_clip))
                            draws->draws.add(
                                {aabb, canvas_clip, s.color, {ax, ay, bx, by}, {}, {}, 0.f, {}, {}, {}, w});
                    }
                }
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
                Color guide_color = tree_style.tree_line;
                guide_color.a *= .7f;
                if (depth) {
                    const auto px = std::round(cx - indent);
                    quad({px, bounds.y, stroke, (state.control.tree_lines[depth] ? bounds.height : cy - bounds.y)},
                         tree_clip, guide_color);
                    const float h_len =
                        state.control.tree_branch ? std::max(0.f, indent - 5.5f * scale) : indent * .65f;
                    quad({px, cy, h_len, stroke}, tree_clip, guide_color);
                    for (std::uint32_t level = 1; level < depth; ++level)
                        if (state.control.tree_lines[level])
                            quad({std::round(start + (level - .5f) * indent), bounds.y, stroke, bounds.height},
                                 tree_clip, guide_color);
                }
                if (state.control.tree_branch) {
                    const bool expanded = state.control.tree_expanded;
                    const float tri = std::min(9.5f * scale, std::min(indent - scale, bounds.height - 2 * scale));
                    if (expanded && cy + tri * .55f < bounds.y + bounds.height)
                        quad({cx, cy + tri * .55f, stroke, bounds.y + bounds.height - (cy + tri * .55f)}, tree_clip,
                             guide_color);
                    if (tri > 0) {
                        const bool disc_hot = enabled && self.hover == key &&
                                              contains(output->tree_rows.back().disclosure, self.hover_x, self.hover_y);
                        Color arrow_col = disc_hot || state.control.selected ? appearance.foreground
                                                                             : appearance.indicator_background;
                        quad({cx - tri * .5f, cy - tri * .5f, tri, tri}, tree_clip, arrow_col,
                             self.text.symbol(expanded ? detail::TextStore::Symbol::disclosure_down
                                                       : detail::TextStore::Symbol::disclosure_right,
                                              static_cast<std::uint32_t>(std::ceil(tri))));
                    }
                }
                float cursor_right = std::min(bounds.x + bounds.width, static_cast<float>(clip.right)) -
                                     (state.control.layout.padding.right + appearance.padding.right) * scale;
                if (state.control.tree_trailing_icon != ControlIcon::none) {
                    const Rect& hit_box = output->tree_rows.back().trailing_action;
                    const bool trail_hot =
                        enabled && self.hover == key && contains(hit_box, self.hover_x, self.hover_y);
                    if (trail_hot)
                        quad({hit_box.x + scale, hit_box.y + 2 * scale, std::max(0.f, hit_box.width - 2 * scale),
                              std::max(0.f, hit_box.height - 4 * scale)},
                             tree_clip, Color{1.f, 1.f, 1.f, .09f});
                    const float sz =
                        std::min(12.5f * scale, std::min(hit_box.width - 2 * scale, bounds.height - 2 * scale));
                    if (sz > 0) {
                        Color ic = trail_hot ? Color{1.f, 1.f, 1.f, 1.f} : appearance.foreground;
                        if (state.control.tree_trailing_dimmed && !trail_hot)
                            ic.a *= .42f;
                        else if (!trail_hot)
                            ic.a *= .82f;
                        quad({hit_box.x + (hit_box.width - sz) * .5f, bounds.y + (bounds.height - sz) * .5f, sz, sz},
                             tree_clip, ic,
                             self.text.symbol(icon_symbol(state.control.tree_trailing_icon),
                                              static_cast<std::uint32_t>(std::ceil(sz))));
                    }
                    cursor_right = hit_box.x - 2 * scale;
                }
                const float badge_sz = std::min(11.f * scale, bounds.height - 4 * scale);
                if (badge_sz > 0) {
                    Color bc = appearance.indicator_foreground.value_or(appearance.foreground);
                    bc.a *= state.control.selected ? .88f : .62f;
                    for (auto it = state.control.tree_badges.rbegin(); it != state.control.tree_badges.rend(); ++it) {
                        if (*it == ControlIcon::none)
                            continue;
                        cursor_right -= 13.5f * scale;
                        quad({cursor_right, bounds.y + (bounds.height - badge_sz) * .5f, badge_sz, badge_sz}, tree_clip,
                             bc, self.text.symbol(icon_symbol(*it), static_cast<std::uint32_t>(std::ceil(badge_sz))));
                    }
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
                const float r_tr = std::min(1.5f * scale, track.height * .5f);
                const CornerRadii tr_rad{r_tr, r_tr, r_tr, r_tr};
                paint_box(track, slider_clip, Color{.086f, .086f, .086f, 1.f}, {}, tr_rad,
                          std::min(scale * .667f, track.height * .5f), appearance.border, BorderStyle::solid);
                if (fraction > 0.f)
                    paint_box({track.x, track.y, track.width * fraction, track.height}, slider_clip, appearance.accent,
                              {}, tr_rad, 0.f, {}, BorderStyle::solid);
                const float thumb_w = 4.5f * scale;
                const float thumb_h = std::min(13.f * scale, std::max(track.height, bounds.height - 2 * scale));
                const float r_th = std::min(1.5f * scale, thumb_w * .5f);
                paint_box({track.x + track.width * fraction - thumb_w * .5f, bounds.y + (bounds.height - thumb_h) * .5f,
                           thumb_w, thumb_h},
                          slider_clip, appearance.indicator_background, {}, {r_th, r_th, r_th, r_th},
                          std::min(scale * .667f, thumb_w * .5f), appearance.indicator_border, BorderStyle::solid);
            }
            if (kind == ControlKind::color_plane) {
                const auto plane_clip = intersect(clip, clip_rect(bounds));
                const Rect inner{bounds.x + border, bounds.y + border, std::max(0.f, bounds.width - 2 * border),
                                 std::max(0.f, bounds.height - 2 * border)};
                const CornerRadii inner_r{
                    std::max(0.f, radius.top_left - border), std::max(0.f, radius.top_right - border),
                    std::max(0.f, radius.bottom_right - border), std::max(0.f, radius.bottom_left - border)};
                if (state.control.drag_axis == DragAxis::vertical) {
                    const double hue =
                        self.press && self.press->key == key ? self.press->value : state.control.color_hue;
                    paint_box(inner, plane_clip, {1, 1, 1, 1}, self.ensure_hue_bar(), inner_r, 0.f, {},
                              BorderStyle::solid);
                    const float cy = inner.y + std::clamp(static_cast<float>(hue / 360.0), 0.f, 1.f) * inner.height;
                    const float bar_out = 5.f * scale;
                    const float bar_in = 3.2f * scale;
                    paint_box({bounds.x, cy - bar_out * .5f, bounds.width, bar_out}, plane_clip, {0, 0, 0, 0}, {},
                              {1.5f * scale, 1.5f * scale, 1.5f * scale, 1.5f * scale}, std::max(1.f, 1.25f * scale),
                              {0, 0, 0, .82f}, BorderStyle::solid);
                    paint_box({bounds.x + scale, cy - bar_in * .5f, std::max(0.f, bounds.width - 2.f * scale), bar_in},
                              plane_clip, {0, 0, 0, 0}, {}, {scale, scale, scale, scale}, std::max(1.f, 1.33f * scale),
                              {1, 1, 1, .96f}, BorderStyle::solid);
                } else {
                    const DragPoint sv = self.press && self.press->key == key
                                             ? self.press->color_sv
                                             : DragPoint{static_cast<float>(state.control.value),
                                                         static_cast<float>(state.control.color_value)};
                    paint_box(inner, plane_clip, {1, 1, 1, 1}, self.ensure_sv_plane(state.control.color_hue), inner_r,
                              0.f, {}, BorderStyle::solid);
                    const float cx = inner.x + std::clamp(sv.x, 0.f, 1.f) * inner.width;
                    const float cy = inner.y + (1.f - std::clamp(sv.y, 0.f, 1.f)) * inner.height;
                    const float r_out = 5.f * scale;
                    const float r_in = 4.f * scale;
                    paint_box({cx - r_out, cy - r_out, r_out * 2.f, r_out * 2.f}, plane_clip, {0, 0, 0, 0}, {},
                              {r_out, r_out, r_out, r_out}, std::max(1.f, 1.25f * scale), {0, 0, 0, .82f},
                              BorderStyle::solid);
                    paint_box({cx - r_in, cy - r_in, r_in * 2.f, r_in * 2.f}, plane_clip, {0, 0, 0, 0}, {},
                              {r_in, r_in, r_in, r_in}, std::max(1.f, 1.35f * scale), {1, 1, 1, .96f},
                              BorderStyle::solid);
                }
            }
            if (state.control.icon != ControlIcon::none) {
                using Icon = ControlIcon;
                const auto icon = state.control.icon;
                const auto icon_clip = intersect(clip, clip_rect(bounds));
                if (icon == Icon::grip_horizontal || icon == Icon::grip_vertical) {
                    const bool vertical = icon == Icon::grip_vertical;
                    const auto length =
                        std::min(appearance.indicator_size * scale, vertical ? bounds.height : bounds.width);
                    const auto dot = std::min(std::max(1.f, std::round(scale * 4.f / 3)),
                                              std::min(length, vertical ? bounds.width : bounds.height));
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
                    const float size = std::min(appearance.indicator_size * scale, std::min(width, height));
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
        }
        if (kind == ControlKind::table) {
#include "detail/session_table_draw.inl"
            continue;
        }
        if (paint) {
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
                const auto size = std::min(7.f * scale, std::min(bounds.width * .3f, bounds.height * .3f));
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
                    const float x = bounds.x + (text_padding.left + text_metrics.width + 1) * scale;
                    if (size > 0)
                        quad({x, bounds.y + (bounds.height - size) * .5f + scale, size, size}, arrow_clip,
                             appearance.indicator_foreground.value_or(appearance.foreground),
                             self.text.symbol(detail::TextStore::Symbol::caret,
                                              static_cast<std::uint32_t>(std::ceil(size))));
                } else {
                    const auto arrow = [&](detail::TextStore::Symbol symbol, float cy) {
                        quad({bounds.x + bounds.width - 5 * scale - size, cy - size * .5f, size, size}, arrow_clip,
                             appearance.indicator_foreground.value_or(appearance.foreground),
                             self.text.symbol(symbol, static_cast<std::uint32_t>(std::ceil(size))));
                    };
                    if (appearance.combo_indicator == ComboIndicator::arrows) {
                        arrow(detail::TextStore::Symbol::up, bounds.y + bounds.height * .5f - 3.5f * scale);
                        arrow(detail::TextStore::Symbol::down, bounds.y + bounds.height * .5f + 3.5f * scale);
                    } else
                        arrow(detail::TextStore::Symbol::down, bounds.y + bounds.height * .5f);
                }
            }
        }
        if (kind == ControlKind::code_editor) {
#include "detail/session_code_draw.inl"
        }
        if (text_kind(kind)) {
            const auto padding = text_padding;
            const float width = text_width;
            const auto metrics = text_metrics;
            ATELIER_TRY_BIND(paragraph_result, text_field(kind)
                                                   ? self.text.snapshot(key, width)
                                                   : Result<std::shared_ptr<const detail::TextSnapshot>>{});
            const auto& paragraph = *paragraph_result;
            float x = bounds.x + padding.left * scale;
            float y = bounds.y + padding.top * scale;
            const float content_height = std::max(0.f, box_height - (padding.top + padding.bottom));
            // Baseline contexts use the same top origin as layout measurement,
            // including text contributing a nested container's first baseline.
            if (!state.baseline_content) {
                const auto alignment = appearance.text_vertical_align;
                const float offset = alignment == TextVerticalAlign::center   ? .5f
                                     : alignment == TextVerticalAlign::bottom ? 1.f
                                                                              : 0.f;
                y += std::max(0.f, content_height - metrics.height) * offset * scale;
            }
            const auto text_clip = intersect(
                clip, clip_rect(text_field(kind) ? Rect{bounds.x + padding.left * scale, bounds.y + padding.top * scale,
                                                        width * scale, content_height * scale}
                                                 : bounds));
            const bool editing = self.edit && self.edit->key == key && self.edit->text_mode;
            float scroll = editing ? self.edit->scroll : 0;
            Rect caret;
            if (editing) {
                ATELIER_TRY_BIND(position, self.display_caret());
                ATELIER_TRY_BIND(caret_result, paragraph->caret(*position));
                caret = *caret_result;
                scroll = std::clamp(scroll, 0.f, std::max(0.f, metrics.width + appearance.caret_width - width));
                if (caret.x < scroll)
                    scroll = caret.x;
                if (caret.x + appearance.caret_width > scroll + width)
                    scroll = std::max(0.f, caret.x + appearance.caret_width - width);
                x -= scroll * scale;
                if (paint) {
                    if (!self.edit->composition) {
                        const auto& s = self.edit->buffer().current();
                        ATELIER_TRY_BIND(ranges, paragraph->selection(std::min(s.caret.offset, s.anchor.offset),
                                                                      std::max(s.caret.offset, s.anchor.offset)));
                        for (auto range : *ranges)
                            quad({x + range.x * scale, y + range.y * scale, range.width * scale, range.height * scale},
                                 text_clip, appearance.selection);
                    } else {
                        const auto& c = *self.edit->composition;
                        for (std::size_t first = 0; first < c.preedit.attributes.size();) {
                            auto last = first + 1;
                            while (last < c.preedit.attributes.size() &&
                                   c.preedit.attributes[last] == c.preedit.attributes[first])
                                ++last;
                            if (c.preedit.attributes[first] == 1 || c.preedit.attributes[first] == 3) {
                                ATELIER_TRY_BIND(ranges,
                                                 paragraph->selection(c.first + static_cast<std::uint32_t>(first),
                                                                      c.first + static_cast<std::uint32_t>(last)));
                                for (auto range : *ranges)
                                    quad({x + range.x * scale, y + range.y * scale, range.width * scale,
                                          range.height * scale},
                                         text_clip, appearance.selection);
                            }
                            first = last;
                        }
                    }
                }
            }
            if (text_field(kind)) {
                const bool bound = self.edit && self.edit->key == key;
                output->text_geometry.add({key, state.incarnation, paragraph, x, y, scale, scroll,
                                           bound ? self.edit->token : EditToken{},
                                           bound && self.edit->composition ? self.edit->composition->id : 0});
            }
            if (paint) {
                if (kind == ControlKind::text_edit && !state.control.placeholder.empty() && self.display_empty(state) &&
                    !(editing && self.edit->composition)) {
                    const detail::TextKey ph_key{key, 0, true};
                    InlineArray<char, 256> ph_scratch;
                    const StringView ph_u8 = wide_to_utf8_scratch(state.control.placeholder, ph_scratch);
                    self.text.set(ph_key, ph_u8, appearance.font, false, appearance.text_align, true);
                    ATELIER_TRY_BIND(ph_raster, self.text.raster(ph_key, width, scale));
                    if (ph_raster->coverage) {
                        Color muted = appearance.foreground;
                        muted.a *= .42f;
                        quad({x + ph_raster->left, y + ph_raster->top, static_cast<float>(ph_raster->coverage->width),
                              static_cast<float>(ph_raster->coverage->height)},
                             text_clip, muted, ph_raster->coverage);
                    }
                } else {
                    ATELIER_TRY(draw_paragraph(self.text, key, width, scale, x, y, text_clip,
                                               text_field(kind) ? text_clip : clip_rect(bounds), appearance.foreground,
                                               appearance.text_shadow, quad));
                }
            }
            if (editing) {
                if (paint && self.edit->composition) {
                    const auto& c = *self.edit->composition;
                    ATELIER_TRY_BIND(ranges, paragraph->selection(
                                                 c.first, c.first + static_cast<std::uint32_t>(c.preedit.text.size())));
                    for (auto range : *ranges)
                        quad({x + range.x * scale, y + (range.y + range.height) * scale - scale, range.width * scale,
                              scale},
                             text_clip, appearance.caret);
                }
                const Rect physical{x + caret.x * scale, y + caret.y * scale, appearance.caret_width * scale,
                                    std::max(1.f, caret.height * scale)};
                if (paint)
                    quad(physical, text_clip, appearance.caret);
                if (visible(physical, text_clip))
                    output->caret = TextCaret{self.edit->token, physical, text_clip};
                TextInputAnchor anchor{self.edit->token, 0, 0, {}, text_clip};
                Rect start;
                if (self.edit->composition) {
                    const auto& c = *self.edit->composition;
                    anchor.composition_id = c.id;
                    anchor.composition_revision = c.revision;
                    ATELIER_TRY_BIND(start_result, c.preedit.text.empty() ? paragraph->caret({c.first, false})
                                                                          : paragraph->character(c.first));
                    start = *start_result;
                } else {
                    const auto& selection = self.edit->buffer().current();
                    const auto position =
                        selection.caret.offset == selection.anchor.offset
                            ? selection.caret
                            : detail::TextPosition{std::min(selection.caret.offset, selection.anchor.offset), false};
                    ATELIER_TRY_BIND(start_result, paragraph->caret(position));
                    start = *start_result;
                }
                anchor.bounds = {x + start.x * scale, y + start.y * scale, start.width * scale, start.height * scale};
                if (!state.control.read_only)
                    output->input_anchor = anchor;
            }
        }
        if (retain)
            state.paint_range.draw_count = draws->draws.size() - state.paint_range.draw_begin;
    }
    const auto popup_draw_end = paint ? draws->draws.size() : 0;
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
        prepare_scrollbar(bar, appearance, bounds.enabled);
    };
    for (auto key : self.order)
        if (scrolling(self.states.at(key).control.kind) && self.states.at(key).control.kind != ControlKind::popup)
            scrollbar(self.states.at(key));
    if (popup_draw_begin != std::numeric_limits<std::size_t>::max()) {
        // Root popup owns the top layer, including over ordinary list bars.
        if (paint)
            std::rotate(draws->draws.begin() + static_cast<std::ptrdiff_t>(popup_draw_begin),
                        draws->draws.begin() + static_cast<std::ptrdiff_t>(popup_draw_end), draws->draws.end());
        scrollbar(self.states.at(output->popup));
    }
    output->control_index.reserve(output->controls.size());
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(output->controls.size()); ++i)
        output->control_index.emplace(output->controls[i].key, i);
    std::ranges::sort(output->control_index, {}, &std::pair<ControlKey, std::uint32_t>::first);
    if (paint) {
#include "detail/session_ladder.inl"
#include "detail/session_tooltip_draw.inl"
    }
    output->drawing = std::move(draws);
    attempt.release();
    return std::shared_ptr<const UiFrame>(std::move(output));
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
        return std::unexpected(reply.error.value_or(Error{ErrorCode::invalid_text, impl_->focus}));
    return reply;
}

Reply Session::focus(ControlKey key, std::optional<std::uint32_t> select_length) {
    Reply reply;
    const auto* box = impl_->published ? impl_->published->find(key) : nullptr;
    if (key && (!impl_->active() || !box || !impl_->current(*box) || !focusable(box->kind) ||
                !visible(box->bounds, box->clip)))
        return reply;
    if (impl_->focus != key && !impl_->finish_edit(reply, true))
        return reply;
    impl_->set_focus(reply, key);
    if (key && text_field(box->kind) && !impl_->edit)
        impl_->begin_edit(reply, key, box->kind == ControlKind::text_edit);
    if (select_length && impl_->edit && impl_->edit->key == key && !impl_->edit->document) {
        const auto len = std::min<std::uint32_t>(
            *select_length, static_cast<std::uint32_t>(impl_->edit->buffer().current().text.size()));
        impl_->edit->text_mode = true;
        bool changed = impl_->edit->buffer().select({0, false}, false);
        changed |= impl_->edit->buffer().select({len, len != 0}, true);
        if (changed)
            impl_->revise();
    }
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
    return state && text_field(state->control.kind) && !state->control.read_only && impl_->active();
}

InteractionState Session::interaction_state() const noexcept {
    const auto& state = *impl_;
    return {state.press.has_value(),
            state.keyboard_pressed != 0,
            state.edit && !state.edit->document &&
                (state.edit->buffer().active() || state.states.at(state.edit->key).control.transient_edit),
            state.edit && (state.edit->composition.has_value() || state.edit->ime_request.has_value()),
            (state.edit && state.edit->clipboard.has_value()) || state.table_clipboard.has_value(),
            has_popup()};
}

bool Session::has_popup() const noexcept {
    return impl_->popup || (impl_->published && impl_->published->popup);
}

bool Session::clipboard_current(const ClipboardRequest& request) const noexcept {
    const auto& self = *impl_;
    if (self.table_clipboard && request.kind == ClipboardKind::copy && self.focus == self.table_clipboard->token.key &&
        self.active()) {
        const auto* box = self.published->find(self.focus);
        if (box && self.current(*box) && self.table_clipboard->operation_id == request.operation_id &&
            self.table_clipboard->token == request.token)
            return true;
    }
    if (!self.edit || !self.edit->clipboard || self.focus != self.edit->key || !self.active())
        return false;
    const auto* box = self.published->find(self.focus);
    if (!box || !self.current(*box) || (box->read_only && request.kind != ClipboardKind::copy))
        return false;
    const auto& pending = *self.edit->clipboard;
    return pending.operation_id == request.operation_id && pending.kind == request.kind &&
           pending.token == request.token && self.edit->token == request.token;
}

Optional<TextInputState> Session::text_input() const noexcept {
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

#include "detail/session_input.inl"
} // namespace atelier::ui
