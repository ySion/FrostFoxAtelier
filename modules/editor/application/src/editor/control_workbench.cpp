#include "editor/control_workbench.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>

namespace atelier::editor {
namespace {
std::array<double, 3> sample_rgb_to_hsv(std::array<double, 3> c, double fallback_hue) noexcept {
    const double high = std::max(c[0], std::max(c[1], c[2]));
    const double low = std::min(c[0], std::min(c[1], c[2]));
    const double span = high - low;
    double h = fallback_hue;
    if (span > 1e-6) {
        if (high == c[0])
            h = 60.0 * std::fmod((c[1] - c[2]) / span, 6.0);
        else if (high == c[1])
            h = 60.0 * ((c[2] - c[0]) / span + 2.0);
        else
            h = 60.0 * ((c[0] - c[1]) / span + 4.0);
        if (h < 0.0)
            h += 360.0;
    }
    return {h, high > 1e-6 ? span / high : 0.0, high};
}

std::array<double, 3> sample_hsv_to_rgb(double h_deg, double s, double v) noexcept {
    const double h = std::fmod(std::max(h_deg, 0.0), 360.0) / 60.0;
    s = std::clamp(s, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const double chroma = v * s;
    const double x = chroma * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    const double m = v - chroma;
    const int sector = std::clamp(static_cast<int>(h), 0, 5);
    const std::array<std::array<double, 3>, 6> table{
        {{chroma, x, 0}, {x, chroma, 0}, {0, chroma, x}, {0, x, chroma}, {x, 0, chroma}, {chroma, 0, x}}};
    const auto& rgb = table[sector];
    return {rgb[0] + m, rgb[1] + m, rgb[2] + m};
}

std::wstring lower_wide(std::wstring_view text) {
    std::wstring out(text);
    for (auto& c : out)
        c = static_cast<wchar_t>(std::towlower(c));
    return out;
}
} // namespace

void ControlWorkbench::sync_hsv() noexcept {
    const auto hsv = sample_rgb_to_hsv(values_.color, hue_);
    if (hsv[1] > 1e-5)
        hue_ = hsv[0];
    if (hsv[2] > 1e-6)
        saturation_ = hsv[1];
}

void ControlWorkbench::remember() {
    if (values_ == history_[cursor_])
        return;
    history_.truncate(cursor_ + 1);
    history_.add(values_);
    if (history_.size() > 65)
        history_.remove_at(0);
    cursor_ = history_.size() - 1;
}

void ControlWorkbench::undo() {
    if (active_) {
        values_ = std::move(active_->second);
        active_.reset();
    }
    if (cursor_)
        values_ = history_[--cursor_];
    sync_hsv();
}

void ControlWorkbench::redo() {
    if (active_) {
        values_ = std::move(active_->second);
        active_.reset();
    }
    if (cursor_ + 1 < history_.size())
        values_ = history_[++cursor_];
    sync_hsv();
}

bool ControlWorkbench::apply(const ui::Action& action) {
    using namespace workbench_control;
    if (action.kind == ui::ActionKind::activate || action.kind == ui::ActionKind::reset) {
        if (action.key == general_page || action.key == appearance_page) {
            page_ = action.key;
            return true;
        }
        if (action.key == rgb_page || action.key == channels_page) {
            color_page_ = action.key;
            return true;
        }
        if (action.key == color_fold) {
            color_expanded_ = std::get<bool>(action.value);
            return true;
        }
        if (action.key == disable) {
            disabled_ = std::get<bool>(action.value);
            return true;
        }
        if (action.key == long_labels) {
            long_labels_ = std::get<bool>(action.value);
            return true;
        }
        if (action.key == fold) {
            expanded_ = std::get<bool>(action.value);
            return true;
        }
        if (action.key == readonly_fold) {
            readonly_expanded_ = std::get<bool>(action.value);
            return true;
        }
        if (action.key == filter_clear) {
            query_.clear();
            return true;
        }
        if (action.key == color_reset) {
            values_.color = Values{}.color;
            sync_hsv();
        } else if (action.key == position_reset)
            values_.position = {};
        else if (action.key == vector4_reset)
            values_.vector4 = Values{}.vector4;
        else if (action.key == scalar_reset)
            values_.exposure = 1;
        else if (action.key == enabled)
            values_.enabled = std::get<bool>(action.value);
        else if (action.key == reset) {
            values_ = Values{};
            sync_hsv();
        } else if (action.key >= mode_first && action.key < mode_first + mode_names.size())
            values_.mode = action.key - mode_first;
        else
            return false;
        remember();
        return true;
    }
    if (action.key == filter) {
        if (const auto* text = std::get_if<std::wstring>(&action.value))
            query_ = *text;
        return true;
    }
    if (action.key == readonly_id || action.key == readonly_summary)
        return true;
    const bool vector = action.key >= position_x && action.key <= position_z;
    const bool four = action.key >= vector4_x && action.key <= vector4_w;
    const bool color = action.key >= red && action.key <= blue;
    const bool channel_slider = action.key >= 480 && action.key <= 482;
    const bool plane = action.key == sv_plane || action.key == hue_bar;
    if (!vector && !four && !color && !channel_slider && !plane && action.key != scalar &&
        action.key != scalar_slider && action.key != name)
        return false;
    if (action.kind == ui::ActionKind::edit_begin) {
        active_.emplace(action.key, values_);
        return true;
    }
    if (!active_ || active_->first != action.key)
        return true;
    if (action.kind == ui::ActionKind::edit_cancel) {
        values_ = std::move(active_->second);
        sync_hsv();
        active_.reset();
        return true;
    }
    if (action.kind != ui::ActionKind::edit_preview && action.kind != ui::ActionKind::edit_commit)
        return true;
    if (plane) {
        double v = std::max(values_.color[0], std::max(values_.color[1], values_.color[2]));
        if (action.key == sv_plane) {
            const auto pt = std::get<ui::DragPoint>(action.value);
            saturation_ = std::clamp(static_cast<double>(pt.x), 0.0, 1.0);
            v = std::clamp(static_cast<double>(pt.y), 0.0, 1.0);
        } else {
            hue_ = std::clamp(std::get<double>(action.value), 0.0, 360.0);
        }
        values_.color = sample_hsv_to_rgb(hue_, saturation_, v);
    } else if (color) {
        values_.color[action.key - red] = std::get<double>(action.value);
        sync_hsv();
    } else if (channel_slider) {
        values_.color[action.key - 480] = std::get<double>(action.value);
        sync_hsv();
    } else if (vector)
        values_.position[action.key - position_x] = std::get<double>(action.value);
    else if (four)
        values_.vector4[action.key - vector4_x] = std::get<double>(action.value);
    else if (action.key == scalar || action.key == scalar_slider)
        values_.exposure = std::get<double>(action.value);
    else
        values_.name = std::get<std::wstring>(action.value);
    if (action.kind == ui::ActionKind::edit_commit) {
        remember();
        active_.reset();
    }
    return true;
}

void ControlWorkbench::append(ui_language::ControlOutput controls, ui::ControlKey parent,
                              ui_language::LiveDocument& document) const {
    using namespace workbench_control;
    using namespace ui_language;
    static const Values defaults;
    EditBinding position{position_reset, true, values_.position != defaults.position, {}};
    EditBinding vector{vector4_reset, true, values_.vector4 != defaults.vector4, {}};
    EditBinding color{color_reset, true, values_.color != defaults.color, {}};
    constexpr std::array names{L"X", L"Y", L"Z", L"W"};
    constexpr std::array colors{L"Red", L"Green", L"Blue"};
    for (std::size_t axis = 0; axis < 4; ++axis) {
        const auto channel = static_cast<ui::VectorAxis>(axis + 1);
        vector.fields.add({vector4_x + axis, names[axis], values_.vector4[axis], defaults.vector4[axis], -10000, 10000,
                           .01, .001, channel});
        if (axis == 3)
            continue;
        position.fields.add(
            {position_x + axis, names[axis], values_.position[axis], 0, -10000, 10000, .01, .001, channel});
        color.fields.add(
            {red + axis, colors[axis], values_.color[axis], defaults.color[axis], 0, 1, .01, .001, channel});
    }
    EditBinding exposure{scalar_reset,
                         true,
                         values_.exposure != 1,
                         {{scalar, L"", values_.exposure, 1, 0, 32, .05, .005, ui::VectorAxis::none,
                           ui::SliderRange{scalar_slider, 0, 16}}}};
    std::array<EditBinding, 3> channels;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        auto field = color.fields[axis];
        field.slider = ui::SliderRange{480 + axis, 0, 1};
        channels[axis] = {0, true, field.value != field.default_value, {std::move(field)}};
    }
    const double val_norm =
        std::clamp(std::max(values_.color[0], std::max(values_.color[1], values_.color[2])), 0.0, 1.0);
    const auto folded = lower_wide(query_);
    const auto matches = [&](std::wstring_view haystack) {
        return folded.empty() || lower_wide(haystack).find(folded) != std::wstring::npos;
    };
    const bool show_options = matches(L"disable sample fields show long labels options");
    const bool show_position = matches(L"transform display local position metres x y z");
    const bool show_exposure = matches(L"transform display exposure compensation slider");
    const bool show_enabled = matches(L"transform display enabled active checkbox");
    const bool show_vector4 = matches(L"transform display vector4 x y z w");
    const bool show_projection =
        matches(L"transform display projection mode combo perspective orthographic uv texture");
    const bool show_transform_group = show_position || show_exposure || show_enabled || show_vector4 || show_projection;
    const bool show_readonly_id = matches(L"read-only observability object uri entity copy");
    const bool show_readonly_summary = matches(L"read-only observability live digest summary copy");
    const bool show_readonly_group = show_readonly_id || show_readonly_summary;
    const bool no_matches = !show_options && !show_transform_group && !show_readonly_group;
    TextEditBinding ro_id{readonly_id, L"entity://world/sample_01 (0x0042A9F0)"};
    ro_id.read_only = true;
    TextEditBinding ro_summary{readonly_summary,
                               std::format(L"pos=({:.2f}, {:.2f}, {:.2f}) · exp={:.2f} · {}", values_.position[0],
                                           values_.position[1], values_.position[2], values_.exposure,
                                           mode_names[values_.mode])};
    ro_summary.read_only = true;
    const std::array<InputValue, 46> inputs{TextEditBinding{name, values_.name, !disabled_},
                                            values_.name != defaults.name,
                                            disabled_,
                                            long_labels_,
                                            expanded_ || !folded.empty(),
                                            page_ == general_page,
                                            color_page_ == rgb_page,
                                            color_expanded_,
                                            values_.enabled,
                                            values_.enabled != defaults.enabled,
                                            std::wstring(mode_names[values_.mode]),
                                            values_.mode != defaults.mode,
                                            position,
                                            exposure,
                                            vector,
                                            color,
                                            channels[0],
                                            channels[1],
                                            channels[2],
                                            values_.color[0],
                                            values_.color[1],
                                            values_.color[2],
                                            values_ != defaults,
                                            L"Sample history  \u00b7  " + std::to_wstring(undo_count()) + L" undo / " +
                                                std::to_wstring(redo_count()) + L" redo",
                                            std::wstring(long_labels_ ? L"Local position in metres" : L"Position (m)"),
                                            std::wstring(long_labels_ ? L"Exposure compensation" : L"Exposure"),
                                            TextEditBinding{filter, query_},
                                            query_.empty(),
                                            ActionBinding{filter_clear, !query_.empty()},
                                            std::move(ro_id),
                                            std::move(ro_summary),
                                            std::clamp(hue_, 0.0, 360.0),
                                            std::clamp(saturation_, 0.0, 1.0),
                                            val_norm,
                                            readonly_expanded_ || !folded.empty(),
                                            show_options,
                                            show_transform_group,
                                            show_position,
                                            show_exposure,
                                            show_enabled,
                                            show_vector4,
                                            show_projection,
                                            show_readonly_group,
                                            show_readonly_id,
                                            show_readonly_summary,
                                            no_matches};
    const std::array keys{KeyBinding{"header", 301},
                          KeyBinding{"type", 302},
                          KeyBinding{"pages", 445},
                          KeyBinding{"tabs", pages},
                          KeyBinding{"general", general_page},
                          KeyBinding{"appearance", appearance_page},
                          KeyBinding{"scroll", list},
                          KeyBinding{"general_body", 444},
                          KeyBinding{"options", 446},
                          KeyBinding{"disable", disable},
                          KeyBinding{"long_labels", long_labels},
                          KeyBinding{"fold", fold},
                          KeyBinding{"body", body},
                          KeyBinding{"readonly_fold", readonly_fold},
                          KeyBinding{"readonly_body", readonly_body},
                          KeyBinding{"position", 330, 332, 333},
                          KeyBinding{"exposure", 350, 352, 353},
                          KeyBinding{"enabled_row", 363, 364, 365},
                          KeyBinding{"enabled", enabled},
                          KeyBinding{"vector4", 380, 382, 383},
                          KeyBinding{"projection", 417, 419, 418},
                          KeyBinding{"mode", mode},
                          KeyBinding{"appearance_body", appearance_body},
                          KeyBinding{"color_tabs", color_pages},
                          KeyBinding{"rgb", rgb_page},
                          KeyBinding{"channels", channels_page},
                          KeyBinding{"nested", 455},
                          KeyBinding{"swatch_row", 456},
                          KeyBinding{"sv_plane", sv_plane},
                          KeyBinding{"hue_bar", hue_bar},
                          KeyBinding{"swatch", swatch},
                          KeyBinding{"color_fold", color_fold},
                          KeyBinding{"color_body", color_body},
                          KeyBinding{"rgb_fields.color", 463, 464, 465},
                          KeyBinding{"channel_fields.red", 490, 491, 492},
                          KeyBinding{"channel_fields.green", 493, 494, 495},
                          KeyBinding{"channel_fields.blue", 496, 497, 498},
                          KeyBinding{"preview", 471},
                          KeyBinding{"explanation", 472},
                          KeyBinding{"footer", 447},
                          KeyBinding{"reset", reset},
                          KeyBinding{"history", 371},
                          KeyBinding{"gestures", 372}};
    static_cast<void>(document.append("Controls", parent, keys, inputs, controls));
}
} // namespace atelier::editor
