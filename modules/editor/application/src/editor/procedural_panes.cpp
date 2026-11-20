#include "application/diagnostics.hpp"
#include "editor/procedural_panes.hpp"
#include "platform/utf.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <functional>

namespace atelier::editor {
namespace {

std::wstring lowercase_w(std::wstring_view text) {
    std::wstring out(text);
    for (auto& c : out)
        c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

bool filter_matches_column(std::wstring_view attr_name, std::wstring_view col_title, std::wstring_view raw_filter) {
    if (raw_filter.empty())
        return true;
    const auto folded_attr = lowercase_w(attr_name);
    const auto folded_title = lowercase_w(col_title);
    std::size_t pos = 0;
    bool any_token = false;
    while (pos < raw_filter.size()) {
        while (pos < raw_filter.size() && (raw_filter[pos] == L' ' || raw_filter[pos] == L','))
            ++pos;
        if (pos >= raw_filter.size())
            break;
        const auto start = pos;
        while (pos < raw_filter.size() && raw_filter[pos] != L' ' && raw_filter[pos] != L',')
            ++pos;
        any_token = true;
        const auto token = lowercase_w(raw_filter.substr(start, pos - start));
        if (folded_attr.find(token) != std::wstring::npos || folded_title.find(token) != std::wstring::npos)
            return true;
    }
    return !any_token;
}

std::wstring format_float(float v) {
    if (!std::isfinite(v))
        return std::isnan(v) ? L"nan" : (v < 0.f ? L"-inf" : L"inf");
    if (std::abs(v) < 1e-7f)
        v = 0.f;
    return std::format(L"{:.4f}", v);
}

struct ColumnExtractor {
    std::wstring title;

    float default_width{84.f};

    ui::TextAlign align{ui::TextAlign::right};

    std::function<std::wstring(std::size_t)> cell;
};

} // namespace

void GeometrySpreadsheet::reset() noexcept {
    page_offset_ = 0;
    total_rows_ = 0;
    selected_row_ = 0;
    cached_geometry_ = nullptr;
    cached_generation_ = 0;
    dirty_ = true;
    table_.reset();
}

void GeometrySpreadsheet::set_follow_selected(bool follow) noexcept {
    if (follow_selected_ != follow) {
        follow_selected_ = follow;
        page_offset_ = 0;
        dirty_ = true;
    }
}

void GeometrySpreadsheet::set_layer(procedural::Layer next) noexcept {
    if (layer_ != next) {
        layer_ = next;
        page_offset_ = 0;
        selected_row_ = 0;
        dirty_ = true;
    }
}

void GeometrySpreadsheet::set_page_offset(std::size_t offset) noexcept {
    if (page_offset_ != offset) {
        page_offset_ = offset;
        dirty_ = true;
    }
}

void GeometrySpreadsheet::set_filter(std::wstring query) {
    if (filter_ != query) {
        filter_ = std::move(query);
        dirty_ = true;
    }
}

bool GeometrySpreadsheet::apply(const ui::Action& action) {
    if (action.key == spreadsheet_control::table) {
        if (const auto* selected = std::get_if<ui::TableSelection>(&action.value))
            selected_row_ = selected->row;
        if (const auto* resized = std::get_if<ui::TableColumnResize>(&action.value);
            resized && std::isfinite(resized->width) && table_) {
            const auto col = std::ranges::find(table_->columns, resized->column, &ui::TableColumn::key);
            if (col != table_->columns.end()) {
                column_widths_[col->title] = std::clamp(resized->width, col->min_width, 4096.f);
                dirty_ = true;
            }
        }
        return true;
    }
    if (action.key == spreadsheet_control::filter) {
        if (const auto* text = std::get_if<std::wstring>(&action.value))
            set_filter(*text);
        return true;
    }
    if (action.kind != ui::ActionKind::activate)
        return false;
    if (action.key == spreadsheet_control::follow_toggle) {
        if (const auto* checked = std::get_if<bool>(&action.value))
            set_follow_selected(*checked);
        else
            set_follow_selected(!follow_selected_);
        return true;
    }
    if (action.key == spreadsheet_control::layer_point) {
        set_layer(procedural::Layer::point);
        return true;
    }
    if (action.key == spreadsheet_control::layer_vertex) {
        set_layer(procedural::Layer::vertex);
        return true;
    }
    if (action.key == spreadsheet_control::layer_primitive) {
        set_layer(procedural::Layer::primitive);
        return true;
    }
    if (action.key == spreadsheet_control::layer_detail) {
        set_layer(procedural::Layer::detail);
        return true;
    }
    if (action.key == spreadsheet_control::layer_instance) {
        set_layer(procedural::Layer::instance);
        return true;
    }
    if (action.key == spreadsheet_control::page_first) {
        set_page_offset(0);
        return true;
    }
    if (action.key == spreadsheet_control::page_prev) {
        set_page_offset(page_offset_ > page_size ? page_offset_ - page_size : 0);
        return true;
    }
    if (action.key == spreadsheet_control::page_next) {
        if (page_offset_ + page_size < total_rows_)
            set_page_offset(page_offset_ + page_size);
        return true;
    }
    return false;
}

const std::shared_ptr<const ui::TableData>& GeometrySpreadsheet::build_table(const procedural::Geometry* geometry,
                                                                             std::uint64_t generation) {
    const std::size_t next_total = geometry ? geometry->count(layer_) : 0;
    std::size_t clamped_offset = page_offset_;
    if (next_total == 0)
        clamped_offset = 0;
    else if (clamped_offset >= next_total)
        clamped_offset = ((next_total - 1) / page_size) * page_size;
    page_offset_ = clamped_offset;
    total_rows_ = next_total;

    if (!dirty_ && table_ && cached_geometry_ == geometry && cached_generation_ == generation &&
        cached_layer_ == layer_ && cached_offset_ == page_offset_ && cached_filter_ == filter_)
        return table_;

    auto data = std::make_shared<ui::TableData>();
    std::vector<ColumnExtractor> extractors;

    extractors.push_back({L"#", 58.f, ui::TextAlign::right, [](std::size_t idx) { return std::to_wstring(idx); }});

    if (geometry) {
        if (layer_ == procedural::Layer::vertex) {
            if (filter_matches_column(L"point", L"Point", filter_)) {
                const auto vp = geometry->vertex_points();
                extractors.push_back(
                    {L"Point", 68.f, ui::TextAlign::right, [vp](std::size_t idx) { return std::to_wstring(vp[idx]); }});
            }
        } else if (layer_ == procedural::Layer::primitive) {
            if (filter_matches_column(L"kind", L"Kind", filter_)) {
                extractors.push_back({L"Kind", 78.f, ui::TextAlign::left, [geometry](std::size_t idx) {
                                          const auto prim = geometry->primitive(static_cast<std::uint32_t>(idx));
                                          return prim.kind == procedural::PrimitiveKind::polygon ? L"polygon"
                                                                                                 : L"polyline";
                                      }});
            }
            if (filter_matches_column(L"vertices", L"Vertices", filter_)) {
                extractors.push_back({L"Vertices", 72.f, ui::TextAlign::right, [geometry](std::size_t idx) {
                                          const auto prim = geometry->primitive(static_cast<std::uint32_t>(idx));
                                          return std::to_wstring(prim.count);
                                      }});
            }
        } else if (layer_ == procedural::Layer::instance) {
            const auto refs = geometry->references();
            const auto& indices = geometry->instance_references();
            const auto& transforms = geometry->instance_transforms();
            if (filter_matches_column(L"reference", L"Reference", filter_)) {
                extractors.push_back({L"Reference", 160.f, ui::TextAlign::left, [refs, &indices](std::size_t idx) {
                                          const auto r = indices[idx];
                                          return r < refs.size()
                                                     ? platform::utf8_to_utf16_validated(refs[r].key.view().std_view())
                                                     : L"<invalid>";
                                      }});
            }
            constexpr std::array<const wchar_t*, 3> tx_titles{L"Translate[x]", L"Translate[y]", L"Translate[z]"};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (filter_matches_column(L"translate", tx_titles[axis], filter_)) {
                    extractors.push_back(
                        {tx_titles[axis], 90.f, ui::TextAlign::right,
                         [&transforms, axis](std::size_t idx) { return format_float(transforms[idx][12 + axis]); }});
                }
            }
        }

        for (const auto& attr : geometry->attributes()) {
            if (attr.layer != layer_)
                continue;
            const auto attr_name = platform::utf8_to_utf16_validated(attr.name.view().std_view());
            std::visit(
                [&](const auto& col) {
                    using Shared = std::decay_t<decltype(col)>;
                    using T = typename Shared::Value;
                    const std::span<const T> values = col.array();
                    if constexpr (std::same_as<T, bool>) {
                        if (filter_matches_column(attr_name, attr_name, filter_))
                            extractors.push_back({attr_name, 64.f, ui::TextAlign::right,
                                                  [values](std::size_t idx) { return values[idx] ? L"1" : L"0"; }});
                    } else if constexpr (std::same_as<T, std::int32_t> || std::same_as<T, std::uint32_t>) {
                        if (filter_matches_column(attr_name, attr_name, filter_))
                            extractors.push_back({attr_name, 74.f, ui::TextAlign::right,
                                                  [values](std::size_t idx) { return std::to_wstring(values[idx]); }});
                    } else if constexpr (std::same_as<T, float>) {
                        if (filter_matches_column(attr_name, attr_name, filter_))
                            extractors.push_back({attr_name, 84.f, ui::TextAlign::right,
                                                  [values](std::size_t idx) { return format_float(values[idx]); }});
                    } else if constexpr (std::same_as<T, math::Vector2> || std::same_as<T, math::Vector3> ||
                                         std::same_as<T, math::Vector4>) {
                        constexpr std::size_t lanes = std::same_as<T, math::Vector2>   ? 2
                                                      : std::same_as<T, math::Vector3> ? 3
                                                                                       : 4;
                        constexpr std::array suffixes{L"[x]", L"[y]", L"[z]", L"[w]"};
                        constexpr std::array color_suffixes{L"[r]", L"[g]", L"[b]", L"[a]"};
                        const bool is_color = attr.usage == procedural::Usage::color;
                        for (std::size_t lane = 0; lane < lanes; ++lane) {
                            const auto title = attr_name + (is_color ? color_suffixes[lane] : suffixes[lane]);
                            if (!filter_matches_column(attr_name, title, filter_))
                                continue;
                            extractors.push_back({title, 82.f, ui::TextAlign::right, [values, lane](std::size_t idx) {
                                                      return format_float(values[idx][lane]);
                                                  }});
                        }
                    } else if constexpr (std::same_as<T, math::Quaternion>) {
                        constexpr std::array suffixes{L"[x]", L"[y]", L"[z]", L"[w]"};
                        for (std::size_t lane = 0; lane < 4; ++lane) {
                            const auto title = attr_name + suffixes[lane];
                            if (!filter_matches_column(attr_name, title, filter_))
                                continue;
                            extractors.push_back(
                                {title, 82.f, ui::TextAlign::right, [values, lane](std::size_t idx) {
                                     const auto& q = values[idx];
                                     const float c = lane == 0 ? q.x : lane == 1 ? q.y : lane == 2 ? q.z : q.w;
                                     return format_float(c);
                                 }});
                        }
                    } else if constexpr (std::same_as<T, math::Matrix4>) {
                        if (filter_matches_column(attr_name, attr_name, filter_)) {
                            extractors.push_back({attr_name, 160.f, ui::TextAlign::left, [values](std::size_t idx) {
                                                      const auto& m = values[idx];
                                                      return L"t=(" + format_float(m[12]) + L", " +
                                                             format_float(m[13]) + L", " + format_float(m[14]) + L")";
                                                  }});
                        }
                    } else if constexpr (std::same_as<T, String>) {
                        if (filter_matches_column(attr_name, attr_name, filter_)) {
                            extractors.push_back({attr_name, 130.f, ui::TextAlign::left, [values](std::size_t idx) {
                                                      return platform::utf8_to_utf16_validated(
                                                          values[idx].view().std_view());
                                                  }});
                        }
                    }
                },
                attr.data);
        }
    }

    data->columns.reserve(extractors.size());
    for (std::size_t col_idx = 0; col_idx < extractors.size(); ++col_idx) {
        const auto& ext = extractors[col_idx];
        const auto width_it = column_widths_.find(ext.title);
        const float width = width_it == column_widths_.end() ? ext.default_width : width_it->second;
        data->columns.push_back(
            {spreadsheet_control::column_first + col_idx, ext.title, width, 42.f, ext.align, false});
    }

    if (geometry && total_rows_ > 0) {
        const std::size_t end = std::min(total_rows_, page_offset_ + page_size);
        data->rows.reserve(end - page_offset_);
        const std::uint64_t stride = std::max<std::size_t>(extractors.size() + 2, 16);
        for (std::size_t idx = page_offset_; idx < end; ++idx) {
            const ui::TableKey row_key = static_cast<ui::TableKey>(idx + 1);
            ui::TableRow row;
            row.key = row_key;
            row.cells.reserve(extractors.size());
            for (std::size_t col_idx = 0; col_idx < extractors.size(); ++col_idx) {
                row.cells.push_back({row_key * stride + col_idx + 1, extractors[col_idx].cell(idx)});
            }
            data->rows.push_back(std::move(row));
        }
    }

    cached_geometry_ = geometry;
    cached_generation_ = generation;
    cached_layer_ = layer_;
    cached_offset_ = page_offset_;
    cached_filter_ = filter_;
    dirty_ = false;
    table_ = std::move(data);
    return table_;
}

void GeometrySpreadsheet::append(std::vector<ui::Control>& controls, ui::ControlKey parent,
                                 ui_language::LiveDocument& live, const procedural::Geometry* geometry,
                                 std::uint64_t generation, std::wstring_view target_label,
                                 std::wstring_view error_text) {
    using namespace ui_language;
    const auto& data = build_table(geometry, generation);
    const auto count_of = [&](procedural::Layer l) { return geometry ? geometry->count(l) : 0u; };

    std::wstring range_text;
    if (!error_text.empty() && !geometry) {
        range_text = std::wstring(error_text);
    } else if (total_rows_ == 0) {
        range_text = L"0 rows";
    } else if (total_rows_ <= page_size) {
        range_text = std::to_wstring(total_rows_) + L" rows";
    } else {
        const std::size_t end = std::min(total_rows_, page_offset_ + page_size);
        range_text =
            std::to_wstring(page_offset_) + L"–" + std::to_wstring(end - 1) + L" of " + std::to_wstring(total_rows_);
    }

    const std::wstring empty_msg =
        !error_text.empty() && !geometry
            ? std::wstring(error_text)
            : (target_label.empty() ? L"Open a Procedural Graph to inspect geometry." : L"No elements in this layer.");

    const std::array<InputValue, 20> inputs{
        TableBinding{data, {selected_row_, spreadsheet_control::column_first}},
        TextEditBinding{spreadsheet_control::filter, filter_},
        std::wstring(target_label.empty() ? L"No graph" : target_label),
        follow_selected_,
        ActionBinding{spreadsheet_control::follow_toggle},
        L"Points (" + std::to_wstring(count_of(procedural::Layer::point)) + L")",
        L"Vertices (" + std::to_wstring(count_of(procedural::Layer::vertex)) + L")",
        L"Primitives (" + std::to_wstring(count_of(procedural::Layer::primitive)) + L")",
        L"Detail (" + std::to_wstring(count_of(procedural::Layer::detail)) + L")",
        L"Instances (" + std::to_wstring(count_of(procedural::Layer::instance)) + L")",
        layer_ == procedural::Layer::point,
        layer_ == procedural::Layer::vertex,
        layer_ == procedural::Layer::primitive,
        layer_ == procedural::Layer::detail,
        layer_ == procedural::Layer::instance,
        range_text,
        total_rows_ > page_size,
        ActionBinding{spreadsheet_control::page_prev, page_offset_ > 0},
        ActionBinding{spreadsheet_control::page_next, page_offset_ + page_size < total_rows_},
        empty_msg};

    const std::array<KeyBinding, 9> keys{{{"table", spreadsheet_control::table},
                                          {"follow", spreadsheet_control::follow_toggle},
                                          {"points", spreadsheet_control::layer_point},
                                          {"vertices", spreadsheet_control::layer_vertex},
                                          {"primitives", spreadsheet_control::layer_primitive},
                                          {"detail", spreadsheet_control::layer_detail},
                                          {"instances", spreadsheet_control::layer_instance},
                                          {"prev_page", spreadsheet_control::page_prev},
                                          {"next_page", spreadsheet_control::page_next}}};

    static_cast<void>(live.append("GeometrySpreadsheet", parent, keys, inputs, controls));
}

namespace {

float srgb_channel(unsigned byte) noexcept {
    const float x = static_cast<float>(byte) / 255.f;
    return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}

ui::Color hex_color(std::uint32_t rgb, float alpha = 1.f) noexcept {
    return {srgb_channel((rgb >> 16) & 0xffu), srgb_channel((rgb >> 8) & 0xffu), srgb_channel(rgb & 0xffu), alpha};
}

std::string short_type_name(std::string_view full_name) {
    const auto dot = full_name.find_last_of('.');
    return std::string(dot == std::string_view::npos ? full_name : full_name.substr(dot + 1));
}

std::array<float, 2> bezier_point(std::array<float, 2> p0, std::array<float, 2> p1, std::array<float, 2> p2,
                                  std::array<float, 2> p3, float t) noexcept {
    const float u = 1.f - t;
    const float b0 = u * u * u;
    const float b1 = 3.f * u * u * t;
    const float b2 = 3.f * u * t * t;
    const float b3 = t * t * t;
    return {b0 * p0[0] + b1 * p1[0] + b2 * p2[0] + b3 * p3[0], b0 * p0[1] + b1 * p1[1] + b2 * p2[1] + b3 * p3[1]};
}

std::pair<std::array<float, 2>, std::array<float, 2>> wire_controls(std::array<float, 2> from_dip,
                                                                    std::array<float, 2> to_dip, float zoom) noexcept {
    const float dy_raw = to_dip[1] - from_dip[1];
    const float dx_abs = std::abs(to_dip[0] - from_dip[0]);
    const float handle = dy_raw >= 0.f ? std::max(28.f * zoom, dy_raw * .5f)
                                       : std::max(48.f * zoom, std::abs(dy_raw) * .45f + dx_abs * .25f);
    return {{from_dip[0], from_dip[1] + handle}, {to_dip[0], to_dip[1] - handle}};
}

// Each layer emits bounded protocol batches directly, including when all geometry fits on screen.
struct CanvasLayer {
    std::vector<std::shared_ptr<ui::CanvasData>> batches{std::make_shared<ui::CanvasData>()};

    void box(ui::CanvasBox value) {
        if (batches.back()->boxes.size() == ui::CanvasData::max_boxes)
            batches.push_back(std::make_shared<ui::CanvasData>());
        batches.back()->boxes.push_back(value);
    }

    void stroke(ui::CanvasStroke value) {
        if (batches.back()->strokes.size() == ui::CanvasData::max_strokes)
            batches.push_back(std::make_shared<ui::CanvasData>());
        batches.back()->strokes.push_back(value);
    }
};

void append_bezier_wire(CanvasLayer& layer, std::array<float, 2> from_dip, std::array<float, 2> to_dip, float zoom,
                        float width, ui::Color color, ui_layout::Size clip) {
    const auto [p1, p2] = wire_controls(from_dip, to_dip, zoom);
    const float pad = width + 1.f;
    // A cubic is contained in the convex hull of its control points.
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const float high = (axis == 0 ? clip.width : clip.height) + pad;
        if (std::max(std::max(from_dip[axis], to_dip[axis]), std::max(p1[axis], p2[axis])) < -pad ||
            std::min(std::min(from_dip[axis], to_dip[axis]), std::min(p1[axis], p2[axis])) > high)
            return;
    }
    constexpr int segments = 16;
    auto prev = from_dip;
    for (int i = 1; i <= segments; ++i) {
        const auto next = bezier_point(from_dip, p1, p2, to_dip, static_cast<float>(i) / static_cast<float>(segments));
        // Clip the segment parametrically before handing local coordinates to the UI protocol.
        double enter = 0.;
        double leave = 1.;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const double high = (axis == 0 ? clip.width : clip.height) + pad;
            const double delta = static_cast<double>(next[axis]) - prev[axis];
            if (!std::isfinite(delta)) {
                leave = -1.;
                break;
            }
            if (delta == 0.) {
                if (prev[axis] < -pad || prev[axis] > high)
                    leave = -1.;
            } else {
                const double a = (-static_cast<double>(pad) - prev[axis]) / delta;
                const double b = (high - prev[axis]) / delta;
                enter = std::max(enter, std::min(a, b));
                leave = std::min(leave, std::max(a, b));
            }
        }
        if (enter <= leave) {
            const auto coord = [&](int axis, double t) {
                return static_cast<float>(std::clamp(
                    std::lerp(static_cast<double>(prev[axis]), static_cast<double>(next[axis]), t),
                    -static_cast<double>(pad), static_cast<double>((axis == 0 ? clip.width : clip.height) + pad)));
            };
            layer.stroke({coord(0, enter), coord(1, enter), coord(0, leave), coord(1, leave), width, color});
        }
        prev = next;
    }
}

bool segments_intersect(std::array<float, 2> a, std::array<float, 2> b, std::array<float, 2> c,
                        std::array<float, 2> d) noexcept {
    constexpr double tolerance = 0.01; // DIPs
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (std::max(a[axis], b[axis]) + tolerance < std::min(c[axis], d[axis]) ||
            std::max(c[axis], d[axis]) + tolerance < std::min(a[axis], b[axis]))
            return false;
    }
    const auto side = [&](const auto& p, const auto& q, const auto& r) {
        const double dx = static_cast<double>(q[0]) - p[0];
        const double dy = static_cast<double>(q[1]) - p[1];
        const double cross = dx * (r[1] - p[1]) - dy * (r[0] - p[0]);
        const double epsilon = tolerance * std::hypot(dx, dy);
        return (cross > epsilon) - (cross < -epsilon);
    };
    return side(a, b, c) * side(a, b, d) <= 0 && side(c, d, a) * side(c, d, b) <= 0;
}

bool bezier_intersects_segment(std::array<float, 2> from_dip, std::array<float, 2> to_dip, float zoom,
                               std::array<float, 2> cut_a, std::array<float, 2> cut_b) noexcept {
    const auto [p1, p2] = wire_controls(from_dip, to_dip, zoom);
    constexpr int segments = 16;
    auto prev = from_dip;
    for (int i = 1; i <= segments; ++i) {
        const auto next = bezier_point(from_dip, p1, p2, to_dip, static_cast<float>(i) / static_cast<float>(segments));
        if (segments_intersect(cut_a, cut_b, prev, next))
            return true;
        prev = next;
    }
    return false;
}

ui_layout::NodeInput out_of_flow_box(float x, float y, float w, float h) {
    auto in = ui_layout::NodeInput::box(std::max(1.f, w), std::max(1.f, h));
    in.out_of_flow = true;
    in.offset_x = x;
    in.offset_y = y;
    in.horizontal_alignment = ui_layout::Alignment::fit();
    in.vertical_alignment = ui_layout::Alignment::fit();
    return in;
}

} // namespace

bool NetworkCanvas::is_canvas_control(ui::ControlKey key) noexcept {
    return key == network_control::canvas || key == network_control::canvas_backdrop ||
           key == network_control::canvas_nodes || key == network_control::canvas_overlay ||
           key == network_control::hover_port_tag ||
           (key >= network_control::node_key_base && key < network_control::node_key_limit);
}

void NetworkCanvas::reset() {
    cancel_gesture();
    close_tab_search();
    active_graph_.clear();
    pan_x_ = 160.f;
    pan_y_ = 56.f;
    zoom_index_ = 5;
    framed_ = false;
    saved_views_.clear();
    hovered_port_.reset();
    hovered_node_.clear();
    hovered_display_flag_ = false;
    cut_key_held_ = false;
    wheel_remainder_ = 0.f;
    visible_tooltips_.clear();
    node_keys_.clear();
    next_node_key_ = network_control::node_key_base;
}

void NetworkCanvas::cancel_gesture() noexcept {
    gesture_ = Gesture::none;
    pressed_node_.clear();
    drag_origins_.clear();
    drag_world_delta_ = {};
    detached_wire_.reset();
    snapped_port_.reset();
}

ui::ControlKey NetworkCanvas::node_base_key(std::string_view graph_key, std::string_view node_name) {
    std::string combined;
    combined.reserve(graph_key.size() + 1 + node_name.size());
    combined.append(graph_key);
    combined.push_back('\n');
    combined.append(node_name);
    if (const auto found = node_keys_.find(combined); found != node_keys_.end())
        return found->second;
    const auto allocated = next_node_key_;
    if (next_node_key_ + 4 < network_control::batch_key_base)
        next_node_key_ += 4;
    node_keys_.emplace(std::move(combined), allocated);
    return allocated;
}

std::array<float, 2> NetworkCanvas::to_world(float dip_x, float dip_y) const noexcept {
    const float z = zoom();
    return {(dip_x - pan_x_) / z, (dip_y - pan_y_) / z};
}

std::array<float, 2> NetworkCanvas::to_dip(float world_x, float world_y) const noexcept {
    const float z = zoom();
    return {pan_x_ + world_x * z, pan_y_ + world_y * z};
}

std::array<float, 2> NetworkCanvas::displayed_position(const GraphNodeView& node) const {
    if (gesture_ == Gesture::move_nodes) {
        const auto found =
            std::ranges::find_if(drag_origins_, [&](const auto& item) { return item.first == node.name; });
        if (found != drag_origins_.end())
            return {found->second[0] + drag_world_delta_[0], found->second[1] + drag_world_delta_[1]};
    }
    return node.position;
}

std::array<float, 2> NetworkCanvas::port_dip(const GraphNodeView& node, bool is_output, std::size_t port_index) const {
    const auto pos = displayed_position(node);
    const auto top_left = to_dip(pos[0], pos[1]);
    const float z = zoom();
    const float sw = node_width * z;
    const float sh = node_height * z;
    const std::size_t total =
        std::max<std::size_t>(1, is_output ? node.ports.outputs.size() : node.ports.inputs.size());
    const float px = top_left[0] + sw * static_cast<float>(port_index + 1) / static_cast<float>(total + 1);
    const float py = is_output ? (top_left[1] + sh) : top_left[1];
    return {px, py};
}

void NetworkCanvas::sync_graph(std::string_view graph_key, std::span<const GraphNodeView> nodes,
                               ui_layout::Size canvas_dip) {
    if (canvas_dip.width > 32.f && canvas_dip.height > 32.f)
        last_canvas_dip_ = canvas_dip;
    if (active_graph_ != graph_key) {
        if (!active_graph_.empty())
            saved_views_[active_graph_] = {pan_x_, pan_y_, zoom_index_, framed_};
        cancel_gesture();
        close_tab_search();
        hovered_port_.reset();
        hovered_node_.clear();
        active_graph_ = std::string(graph_key);
        if (const auto found = saved_views_.find(active_graph_); found != saved_views_.end()) {
            pan_x_ = found->second.pan_x;
            pan_y_ = found->second.pan_y;
            zoom_index_ = found->second.zoom_index;
            framed_ = found->second.framed;
        } else {
            pan_x_ = 160.f;
            pan_y_ = 56.f;
            zoom_index_ = 5;
            framed_ = false;
        }
    }
    if (!framed_ && last_canvas_dip_.width > 32.f && last_canvas_dip_.height > 32.f) {
        if (!nodes.empty())
            frame_nodes(nodes, last_canvas_dip_);
        else {
            zoom_index_ = 5;
            pan_x_ = std::round((last_canvas_dip_.width - node_width) * .5f);
            pan_y_ = 56.f;
            framed_ = true;
        }
    }
}

void NetworkCanvas::reset_zoom(ui_layout::Size canvas_dip) {
    const float cx = (canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width) * .5f;
    const float cy = (canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height) * .5f;
    const auto world = to_world(cx, cy);
    zoom_index_ = 5;
    pan_x_ = std::clamp(cx - world[0] * zoom(), -250000.f, 250000.f);
    pan_y_ = std::clamp(cy - world[1] * zoom(), -250000.f, 250000.f);
}

void NetworkCanvas::step_zoom(int delta_steps, float anchor_dip_x, float anchor_dip_y) {
    if (delta_steps == 0)
        return;
    const auto world = to_world(anchor_dip_x, anchor_dip_y);
    const int current = static_cast<int>(zoom_index_);
    const int next = std::clamp(current + delta_steps, 0, static_cast<int>(zoom_steps.size()) - 1);
    zoom_index_ = static_cast<std::size_t>(next);
    const float z = zoom();
    pan_x_ = std::clamp(anchor_dip_x - world[0] * z, -250000.f, 250000.f);
    pan_y_ = std::clamp(anchor_dip_y - world[1] * z, -250000.f, 250000.f);
}

void NetworkCanvas::frame_nodes(std::span<const GraphNodeView> nodes, ui_layout::Size canvas_dip,
                                std::span<const std::string> only_selected) {
    const float cw = canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width;
    const float ch = canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height;
    if (nodes.empty()) {
        zoom_index_ = 5;
        pan_x_ = std::round((cw - node_width) * .5f);
        pan_y_ = 56.f;
        framed_ = true;
        return;
    }
    float min_x = 1e9f;
    float min_y = 1e9f;
    float max_x = -1e9f;
    float max_y = -1e9f;
    std::size_t matched = 0;
    const auto accumulate = [&](bool filter) {
        for (const auto& n : nodes) {
            if (filter && std::ranges::find(only_selected, n.name) == only_selected.end())
                continue;
            const auto pos = displayed_position(n);
            min_x = std::min(min_x, pos[0]);
            min_y = std::min(min_y, pos[1]);
            max_x = std::max(max_x, pos[0] + node_width);
            max_y = std::max(max_y, pos[1] + node_height);
            ++matched;
        }
    };
    if (!only_selected.empty())
        accumulate(true);
    if (matched == 0)
        accumulate(false);

    const float span_w = std::max(node_width, max_x - min_x) + 120.f;
    const float span_h = std::max(node_height, max_y - min_y) + 96.f;
    std::size_t chosen = 0;
    for (std::size_t i = 0; i <= 5; ++i) {
        if (span_w * zoom_steps[i] <= cw && span_h * zoom_steps[i] <= ch)
            chosen = i;
    }
    zoom_index_ = chosen;
    const float z = zoom();
    const float center_wx = (min_x + max_x) * .5f;
    const float center_wy = (min_y + max_y) * .5f;
    pan_x_ = std::round(cw * .5f - center_wx * z);
    pan_y_ = std::round(ch * .5f - center_wy * z);
    framed_ = true;
}

void NetworkCanvas::open_tab_search(float dip_x, float dip_y, ui_layout::Size canvas_dip, std::string from_node,
                                    std::string from_port, std::string to_node, std::string to_port) {
    const float cw = canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width;
    const float ch = canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height;
    tab_search_.open = true;
    tab_search_.world_pos = to_world(dip_x, dip_y);
    tab_search_.dip_x = std::clamp(dip_x, 8.f, std::max(8.f, cw - 248.f));
    tab_search_.dip_y = std::clamp(dip_y, 8.f, std::max(8.f, ch - 260.f));
    tab_search_.query.clear();
    tab_search_.highlight = 0;
    tab_search_.auto_from_node = std::move(from_node);
    tab_search_.auto_from_port = std::move(from_port);
    tab_search_.auto_to_node = std::move(to_node);
    tab_search_.auto_to_port = std::move(to_port);
}

void NetworkCanvas::close_tab_search() noexcept {
    tab_search_ = {};
}

void NetworkCanvas::set_tab_query(std::wstring query) {
    if (tab_search_.query != query) {
        tab_search_.query = std::move(query);
        tab_search_.highlight = 0;
    }
}

void NetworkCanvas::move_tab_highlight(int delta, std::size_t count) noexcept {
    if (count == 0) {
        tab_search_.highlight = 0;
        return;
    }
    const int cur = static_cast<int>(std::min(tab_search_.highlight, count - 1));
    const int next = std::clamp(cur + delta, 0, static_cast<int>(count) - 1);
    tab_search_.highlight = static_cast<std::size_t>(next);
}

std::vector<GraphNodeCatalogEntry> NetworkCanvas::filter_catalog(std::span<const GraphNodeCatalogEntry> catalog) const {
    std::vector<GraphNodeCatalogEntry> out;
    const auto q = lowercase_w(tab_search_.query);
    for (const auto& item : catalog) {
        if (!q.empty()) {
            const auto haystack =
                lowercase_w(platform::utf8_to_utf16_validated(item.name + " " + short_type_name(item.name)));
            if (haystack.find(q) == std::wstring::npos)
                continue;
        }
        out.push_back(item);
        if (out.size() >= 12)
            break;
    }
    return out;
}

Result<std::string> NetworkCanvas::commit_tab_pick(Session& session, std::string_view graph_key,
                                                   std::span<const GraphNodeCatalogEntry> catalog,
                                                   std::size_t pick_index, std::string& primary_selected,
                                                   std::vector<std::string>& selected_nodes, bool set_display) {
    const auto matches = filter_catalog(catalog);
    if (pick_index >= matches.size())
        return std::unexpected(Error{ErrorCode::argument_mismatch, "No matching SOP node type selected."});
    const auto search = tab_search_;
    close_tab_search();
    ATELIER_TRY_BIND(created, session.graph_place_node(graph_key, matches[pick_index].name, search.world_pos,
                                                       search.auto_from_node, search.auto_from_port,
                                                       search.auto_to_node, search.auto_to_port, set_display));
    primary_selected = *created;
    selected_nodes = {*created};
    return std::move(*created);
}

Result<NetworkCanvas::InteractionOutcome> NetworkCanvas::pointer_event(Session& session, std::string_view graph_key,
                                                                       std::string& primary_selected,
                                                                       std::vector<std::string>& selected_nodes,
                                                                       ui::Rect canvas_px, float dpi_scale,
                                                                       const ui::Event& event) {
    InteractionOutcome out;
    if (graph_key.empty() || !session.has_graph(graph_key) || canvas_px.width <= 0 || canvas_px.height <= 0 ||
        dpi_scale <= 0.f)
        return out;

    const ui_layout::Size canvas_dip{canvas_px.width / dpi_scale, canvas_px.height / dpi_scale};
    const float lx = (event.x - canvas_px.x) / dpi_scale;
    const float ly = (event.y - canvas_px.y) / dpi_scale;
    const bool inside = event.x >= canvas_px.x && event.x < canvas_px.x + canvas_px.width && event.y >= canvas_px.y &&
                        event.y < canvas_px.y + canvas_px.height;

    const auto inspected_res = session.graph_topology(graph_key);
    if (!inspected_res)
        return std::unexpected(inspected_res.error());
    const auto& view = **inspected_res;
    sync_graph(graph_key, view.nodes, canvas_dip);

    const auto hit_port = [&](float x, float y, float radius, std::optional<bool> want_output = std::nullopt,
                              std::string_view exclude_node = {}) -> std::optional<PortRef> {
        float best_d2 = radius * radius;
        std::optional<PortRef> best;
        for (const auto& node : view.nodes) {
            if (!exclude_node.empty() && node.name == exclude_node)
                continue;
            if (!want_output || !*want_output) {
                for (std::size_t p = 0; p < node.ports.inputs.size(); ++p) {
                    const auto pt = port_dip(node, false, p);
                    const float dx = x - pt[0];
                    const float dy = y - pt[1];
                    const float d2 = dx * dx + dy * dy;
                    if (d2 <= best_d2) {
                        best_d2 = d2;
                        const bool multi = p < node.ports.multiple.size() && node.ports.multiple[p];
                        best = PortRef{node.name, node.ports.inputs[p], p, false, multi};
                    }
                }
            }
            if (!want_output || *want_output) {
                for (std::size_t p = 0; p < node.ports.outputs.size(); ++p) {
                    const auto pt = port_dip(node, true, p);
                    const float dx = x - pt[0];
                    const float dy = y - pt[1];
                    const float d2 = dx * dx + dy * dy;
                    if (d2 <= best_d2) {
                        best_d2 = d2;
                        best = PortRef{node.name, node.ports.outputs[p], p, true, false};
                    }
                }
            }
        }
        return best;
    };

    if (gesture_ != Gesture::none) {
        out.consumed = true;
        current_dip_x_ = lx;
        current_dip_y_ = ly;
        const float move_dist = std::hypot(lx - start_dip_x_, ly - start_dip_y_);

        if (gesture_ == Gesture::pan) {
            if (event.kind == ui::EventKind::pointer_move) {
                pan_x_ = std::clamp(pan_start_x_ + (lx - start_dip_x_), -250000.f, 250000.f);
                pan_y_ = std::clamp(pan_start_y_ + (ly - start_dip_y_), -250000.f, 250000.f);
                out.cursor = ui::Cursor::pan;
            } else if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
            }
            return out;
        }

        if (gesture_ == Gesture::press_node) {
            if (event.kind == ui::EventKind::pointer_move && move_dist >= 4.f && !session.world().running()) {
                gesture_ = Gesture::move_nodes;
                drag_world_delta_ = {(lx - start_dip_x_) / zoom(), (ly - start_dip_y_) / zoom()};
                out.cursor = ui::Cursor::move;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const std::string clicked = pressed_node_;
                const bool shift = gesture_shift_;
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                if (shift) {
                    if (const auto it = std::ranges::find(selected_nodes, clicked); it != selected_nodes.end()) {
                        selected_nodes.erase(it);
                        if (primary_selected == clicked)
                            primary_selected = selected_nodes.empty() ? std::string{} : selected_nodes.back();
                    } else {
                        selected_nodes.push_back(clicked);
                        primary_selected = clicked;
                    }
                } else {
                    selected_nodes = {clicked};
                    primary_selected = clicked;
                }
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::move_nodes) {
            if (event.kind == ui::EventKind::pointer_move) {
                drag_world_delta_ = {(lx - start_dip_x_) / zoom(), (ly - start_dip_y_) / zoom()};
                out.cursor = ui::Cursor::move;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                std::vector<std::pair<std::string, std::array<float, 2>>> moves;
                moves.reserve(drag_origins_.size());
                for (const auto& [name, origin] : drag_origins_)
                    moves.emplace_back(name, std::array{std::round(origin[0] + drag_world_delta_[0]),
                                                        std::round(origin[1] + drag_world_delta_[1])});
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                ATELIER_TRY(session.graph_move_nodes(graph_key, moves));
                out.status = L"Moved SOP node(s)";
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::press_canvas) {
            if (event.kind == ui::EventKind::pointer_move && move_dist >= 4.f) {
                if (gesture_button_ == ui::PointerButton::secondary) {
                    gesture_ = Gesture::pan;
                    pan_x_ = std::clamp(pan_start_x_ + (lx - start_dip_x_), -250000.f, 250000.f);
                    pan_y_ = std::clamp(pan_start_y_ + (ly - start_dip_y_), -250000.f, 250000.f);
                    out.cursor = ui::Cursor::pan;
                } else {
                    gesture_ = Gesture::marquee;
                    out.cursor = ui::Cursor::crosshair;
                }
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const auto btn = gesture_button_;
                const bool shift = gesture_shift_;
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                if (btn == ui::PointerButton::secondary) {
                    out.open_canvas_context = true;
                    out.context_world_pos = to_world(lx, ly);
                } else if (!shift) {
                    selected_nodes.clear();
                    primary_selected.clear();
                }
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::marquee) {
            if (event.kind == ui::EventKind::pointer_move) {
                out.cursor = ui::Cursor::crosshair;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const float rx0 = std::min(start_dip_x_, lx);
                const float ry0 = std::min(start_dip_y_, ly);
                const float rx1 = std::max(start_dip_x_, lx);
                const float ry1 = std::max(start_dip_y_, ly);
                const bool shift = gesture_shift_;
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                if (!shift)
                    selected_nodes.clear();
                const float z = zoom();
                for (const auto& node : view.nodes) {
                    const auto tl = to_dip(node.position[0], node.position[1]);
                    if (tl[0] + node_width * z >= rx0 && tl[0] <= rx1 && tl[1] + node_height * z >= ry0 &&
                        tl[1] <= ry1) {
                        if (std::ranges::find(selected_nodes, node.name) == selected_nodes.end())
                            selected_nodes.push_back(node.name);
                    }
                }
                primary_selected = selected_nodes.empty() ? std::string{} : selected_nodes.back();
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::wire_drag) {
            snapped_port_ = hit_port(lx, ly, 15.f, !wire_anchor_.is_output, wire_anchor_.node);
            if (event.kind == ui::EventKind::pointer_move) {
                out.cursor = ui::Cursor::crosshair;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const auto anchor = wire_anchor_;
                const auto detached = detached_wire_;
                const auto snapped = snapped_port_;
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                if (detached) {
                    if (snapped && !snapped->is_output) {
                        ATELIER_TRY(session.graph_rewire(graph_key, *detached, snapped->node, snapped->port));
                        out.status = L"Rewired SOP port";
                    } else {
                        ATELIER_TRY(session.graph_disconnect_edges(graph_key, std::span{&*detached, 1}));
                        out.status = L"Disconnected SOP wire";
                    }
                    return out;
                }
                if (anchor.is_output) {
                    if (snapped && !snapped->is_output) {
                        ATELIER_TRY(
                            session.graph_connect(graph_key, anchor.node, anchor.port, snapped->node, snapped->port));
                        out.status = L"Connected SOP wire";
                    } else if (inside) {
                        open_tab_search(lx, ly, canvas_dip, anchor.node, anchor.port);
                        out.focus_tab_search = true;
                    }
                } else {
                    if (snapped && snapped->is_output) {
                        ATELIER_TRY(
                            session.graph_connect(graph_key, snapped->node, snapped->port, anchor.node, anchor.port));
                        out.status = L"Connected SOP wire";
                    } else if (inside) {
                        open_tab_search(lx, ly, canvas_dip, {}, {}, anchor.node, anchor.port);
                        out.focus_tab_search = true;
                    }
                }
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::wire_cut) {
            if (event.kind == ui::EventKind::pointer_move) {
                out.cursor = ui::Cursor::crosshair;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const std::array cut_a{start_dip_x_, start_dip_y_};
                const std::array cut_b{lx, ly};
                cancel_gesture();
                out.capture_release = true;
                out.cursor = cut_key_held_ ? ui::Cursor::crosshair : ui::Cursor::arrow;
                std::vector<GraphConnectionView> cut_edges;
                const float z = zoom();
                for (const auto& conn : view.connections) {
                    const auto* src = view.find_node(conn.from_node);
                    const auto* dst = view.find_node(conn.to_node);
                    if (!src || !dst)
                        continue;

                    if (bezier_intersects_segment(port_dip(*src, true, conn.from_index),
                                                  port_dip(*dst, false, conn.to_index), z, cut_a, cut_b))
                        cut_edges.push_back(conn);
                }
                if (!cut_edges.empty()) {
                    ATELIER_TRY(session.graph_disconnect_edges(graph_key, cut_edges));
                    out.status = std::format(L"Cut {} SOP wire(s)", cut_edges.size());
                }
                return out;
            }
            return out;
        }
    }

    if (!inside) {
        hovered_port_.reset();
        hovered_node_.clear();
        hovered_display_flag_ = false;
        return out;
    }

    current_dip_x_ = lx;
    current_dip_y_ = ly;

    if (event.kind == ui::EventKind::wheel) {
        wheel_remainder_ += event.wheel;
        int steps = 0;
        while (wheel_remainder_ >= 1.f) {
            ++steps;
            wheel_remainder_ -= 1.f;
        }
        while (wheel_remainder_ <= -1.f) {
            --steps;
            wheel_remainder_ += 1.f;
        }
        if (steps == 0 && std::abs(event.wheel) > .05f) {
            steps = event.wheel > 0.f ? 1 : -1;
            wheel_remainder_ = 0.f;
        }
        step_zoom(steps, lx, ly);
        out.consumed = true;
        return out;
    }

    const float z = zoom();
    std::string node_under;
    bool flag_under = false;
    bool body_interior = false;
    for (auto it = view.nodes.rbegin(); it != view.nodes.rend(); ++it) {
        const auto pos = displayed_position(*it);
        const auto tl = to_dip(pos[0], pos[1]);
        const float sw = node_width * z;
        const float sh = node_height * z;
        if (lx >= tl[0] && lx <= tl[0] + sw && ly >= tl[1] && ly <= tl[1] + sh) {
            node_under = it->name;
            flag_under = lx >= tl[0] + sw - flag_width * z;
            body_interior = flag_under || (ly >= tl[1] + sh * .28f && ly <= tl[1] + sh * .72f);
            break;
        }
    }
    const auto port_under =
        body_interior ? std::optional<PortRef>{} : hit_port(lx, ly, std::clamp(10.f * z, 4.5f, 11.f));
    if (port_under) {
        node_under.clear();
        flag_under = false;
    }

    if (event.kind == ui::EventKind::pointer_move) {
        hovered_port_ = port_under;
        hovered_node_ = node_under;
        hovered_display_flag_ = flag_under;
        out.consumed = true;
        out.cursor = (cut_key_held_ || port_under.has_value()) ? ui::Cursor::crosshair : ui::Cursor::arrow;
        return out;
    }

    if (event.kind != ui::EventKind::pointer_down)
        return out;

    out.consumed = true;
    if (tab_search_.open)
        close_tab_search();

    start_dip_x_ = lx;
    start_dip_y_ = ly;
    gesture_button_ = event.button;
    gesture_shift_ = event.shift;

    if (event.button == ui::PointerButton::middle || (event.button == ui::PointerButton::primary && event.alt)) {
        gesture_ = Gesture::pan;
        pan_start_x_ = pan_x_;
        pan_start_y_ = pan_y_;
        out.capture_acquire = true;
        out.cursor = ui::Cursor::pan;
        return out;
    }

    if (event.button == ui::PointerButton::primary && cut_key_held_ && !session.world().running()) {
        gesture_ = Gesture::wire_cut;
        out.capture_acquire = true;
        out.cursor = ui::Cursor::crosshair;
        return out;
    }

    if (event.button == ui::PointerButton::primary && port_under && !session.world().running()) {
        detached_wire_.reset();
        wire_anchor_ = *port_under;
        if (!port_under->is_output && !port_under->multiple) {
            const auto existing = std::ranges::find_if(view.connections, [&](const GraphConnectionView& c) {
                return c.to_node == port_under->node && c.to_port == port_under->port;
            });
            if (existing != view.connections.end()) {
                if (const auto* src = view.find_node(existing->from_node)) {
                    const auto out_it = std::ranges::find(src->ports.outputs, existing->from_port);
                    const std::size_t out_i = out_it == src->ports.outputs.end()
                                                  ? 0
                                                  : static_cast<std::size_t>(out_it - src->ports.outputs.begin());
                    detached_wire_ = *existing;
                    wire_anchor_ = PortRef{existing->from_node, existing->from_port, out_i, true, false};
                }
            }
        }
        gesture_ = Gesture::wire_drag;
        snapped_port_.reset();
        out.capture_acquire = true;
        out.cursor = ui::Cursor::crosshair;
        return out;
    }

    if (!node_under.empty()) {
        if (event.button == ui::PointerButton::primary && flag_under && !session.world().running()) {
            const auto* n = view.find_node(node_under);
            const bool make_display = !(n && n->is_display);
            ATELIER_TRY(
                session.graph_set_display(graph_key, make_display ? std::string_view{node_under} : std::string_view{}));
            out.status = make_display ? L"Display Flag: " + platform::utf8_to_utf16_validated(node_under)
                                      : L"Cleared Display Flag";
            return out;
        }
        if (event.button == ui::PointerButton::secondary) {
            if (std::ranges::find(selected_nodes, node_under) == selected_nodes.end()) {
                selected_nodes = {node_under};
                primary_selected = node_under;
            } else {
                primary_selected = node_under;
            }
            out.open_node_context = true;
            out.context_node = node_under;
            return out;
        }
        if (event.button == ui::PointerButton::primary) {
            pressed_node_ = node_under;
            if (!event.shift && std::ranges::find(selected_nodes, node_under) == selected_nodes.end()) {
                selected_nodes = {node_under};
                primary_selected = node_under;
            } else if (!event.shift) {
                primary_selected = node_under;
            }
            drag_origins_.clear();
            for (const auto& sel : selected_nodes) {
                if (const auto* nv = view.find_node(sel))
                    drag_origins_.emplace_back(sel, nv->position);
            }
            if (std::ranges::none_of(drag_origins_, [&](const auto& item) { return item.first == node_under; })) {
                if (const auto* nv = view.find_node(node_under))
                    drag_origins_.emplace_back(node_under, nv->position);
            }
            drag_world_delta_ = {};
            gesture_ = Gesture::press_node;
            out.capture_acquire = true;
            return out;
        }
    }

    if (event.button == ui::PointerButton::primary || event.button == ui::PointerButton::secondary) {
        gesture_ = Gesture::press_canvas;
        pan_start_x_ = pan_x_;
        pan_start_y_ = pan_y_;
        out.capture_acquire = true;
        return out;
    }

    return out;
}

Result<NetworkCanvas::InteractionOutcome> NetworkCanvas::key_event(Session& session, std::string_view graph_key,
                                                                   std::string& primary_selected,
                                                                   std::vector<std::string>& selected_nodes,
                                                                   ui::Rect canvas_px, float dpi_scale,
                                                                   const ui::Event& event, std::uint32_t native_key) {
    InteractionOutcome out;
    if (event.kind == ui::EventKind::key_up) {
        if (native_key == 'Y' || event.key == ui::Key::y)
            cut_key_held_ = false;
        return out;
    }
    if (event.kind != ui::EventKind::key_down || graph_key.empty() || !session.has_graph(graph_key))
        return out;

    const ui_layout::Size canvas_dip = (canvas_px.width > 0 && canvas_px.height > 0 && dpi_scale > 0.f)
                                           ? ui_layout::Size{canvas_px.width / dpi_scale, canvas_px.height / dpi_scale}
                                           : last_canvas_dip_;

    if (tab_search_.open) {
        ATELIER_TRY_BIND(catalog, session.procedural_node_types());
        const auto matches = filter_catalog(*catalog);
        if (event.key == ui::Key::escape) {
            close_tab_search();
            out.consumed = true;
            return out;
        }
        if (event.key == ui::Key::up) {
            move_tab_highlight(-1, matches.size());
            out.consumed = true;
            return out;
        }
        if (event.key == ui::Key::down) {
            move_tab_highlight(1, matches.size());
            out.consumed = true;
            return out;
        }
        if (event.key == ui::Key::enter) {
            out.consumed = true;
            if (!matches.empty()) {
                ATELIER_TRY_BIND(created, commit_tab_pick(session, graph_key, *catalog, tab_search_.highlight,
                                                          primary_selected, selected_nodes, event.shift));
                out.status = L"Added SOP node: " + platform::utf8_to_utf16_validated(*created);
            } else {
                close_tab_search();
            }
            return out;
        }
        return out;
    }

    if (event.key == ui::Key::escape && active_gesture()) {
        cancel_gesture();
        out.consumed = true;
        out.capture_release = true;
        out.cursor = ui::Cursor::arrow;
        out.status = L"Cancelled Network gesture";
        return out;
    }

    if (event.control || event.alt)
        return out;

    const auto inspected_res = session.graph_topology(graph_key);
    if (!inspected_res)
        return std::unexpected(inspected_res.error());
    const auto& view = **inspected_res;

    if (native_key == 'Y' || event.key == ui::Key::y) {
        cut_key_held_ = true;
        out.consumed = true;
        out.cursor = ui::Cursor::crosshair;
        out.status = L"Wire Cut: hold Y and drag across wires to disconnect";
        return out;
    }

    if (event.repeat)
        return out;

    if (event.key == ui::Key::tab && !session.world().running()) {
        std::string auto_from;
        std::string auto_from_port;
        float tx = current_dip_x_;
        float ty = current_dip_y_;
        if (tx <= 0.f || ty <= 0.f || tx >= canvas_dip.width || ty >= canvas_dip.height) {
            tx = canvas_dip.width * .5f;
            ty = canvas_dip.height * .5f;
        }
        if (!primary_selected.empty()) {
            const auto it = std::ranges::find(view.nodes, primary_selected, &GraphNodeView::name);
            if (it != view.nodes.end() && !it->ports.outputs.empty()) {
                auto_from = it->name;
                auto_from_port = it->ports.outputs.front();
            }
        }
        open_tab_search(tx, ty, canvas_dip, std::move(auto_from), std::move(auto_from_port));
        out.consumed = true;
        out.focus_tab_search = true;
        return out;
    }

    if (native_key == 'H' || event.key == ui::Key::home) {
        frame_nodes(view.nodes, canvas_dip);
        out.consumed = true;
        out.status = L"Framed all SOP nodes";
        return out;
    }

    if (native_key == 'F') {
        frame_nodes(view.nodes, canvas_dip, selected_nodes);
        out.consumed = true;
        out.status = L"Framed SOP selection";
        return out;
    }

    if (native_key == 'L' && !session.world().running()) {
        ATELIER_TRY(session.graph_auto_layout(graph_key));
        if (const auto after = session.graph_topology(graph_key))
            frame_nodes((*after)->nodes, canvas_dip);
        out.consumed = true;
        out.status = L"Auto-arranged SOP network";
        return out;
    }

    if (native_key == 'D' && !session.world().running()) {
        const std::string target = !hovered_node_.empty() ? hovered_node_ : primary_selected;
        if (!target.empty()) {
            const auto it = std::ranges::find(view.nodes, target, &GraphNodeView::name);
            if (it != view.nodes.end()) {
                ATELIER_TRY(session.graph_set_display(graph_key, it->is_display ? std::string_view{} : target));
                out.consumed = true;
                out.status = it->is_display ? L"Cleared Display Flag"
                                            : L"Display Flag: " + platform::utf8_to_utf16_validated(target);
                return out;
            }
        }
    }

    if (native_key == 'A' || event.key == ui::Key::a) {
        if (selected_nodes.size() == view.nodes.size()) {
            selected_nodes.clear();
            primary_selected.clear();
        } else {
            selected_nodes.clear();
            for (const auto& n : view.nodes)
                selected_nodes.push_back(n.name);
            primary_selected = selected_nodes.empty() ? std::string{} : selected_nodes.back();
        }
        out.consumed = true;
        return out;
    }

    if ((event.key == ui::Key::delete_key || event.key == ui::Key::backspace) && !session.world().running()) {
        std::vector<std::string> targets = selected_nodes;
        if (targets.empty() && !primary_selected.empty())
            targets.push_back(primary_selected);
        if (!targets.empty()) {
            ATELIER_TRY(session.graph_remove_nodes(graph_key, targets));
            selected_nodes.clear();
            primary_selected.clear();
            out.consumed = true;
            out.status = L"Deleted SOP node(s)";
            return out;
        }
    }

    return out;
}

void NetworkCanvas::append_canvas(std::vector<ui::Control>& controls, ui::ControlKey canvas_parent,
                                  ui_layout::Size canvas_dip, std::string_view graph_key, const GraphInspectView& view,
                                  std::span<const GraphNodeCatalogEntry> catalog, std::string_view primary_selected,
                                  std::span<const std::string> selected_nodes) {
    sync_graph(graph_key, view.nodes, canvas_dip);
    const float z = zoom();
    const float cw = std::max(64.f, canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width);
    const float ch = std::max(64.f, canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height);

    CanvasLayer backdrop;
    auto& grid = *backdrop.batches.front();
    grid.grid_step = 32.f * z;
    grid.grid_origin_x = pan_x_;
    grid.grid_origin_y = pan_y_;
    grid.grid_minor = hex_color(0x2f2f2f);
    grid.grid_major = hex_color(0x3a3a3a);
    grid.grid_axis = hex_color(0x4a525a);
    CanvasLayer bodies;
    CanvasLayer overlay;
    decltype(visible_tooltips_) tooltips;

    // 1. Existing wires in backdrop:
    for (const auto& conn : view.connections) {
        if (detached_wire_ && conn.from_node == detached_wire_->from_node &&
            conn.from_port == detached_wire_->from_port && conn.to_node == detached_wire_->to_node &&
            conn.to_port == detached_wire_->to_port)
            continue;
        const auto* src = view.find_node(conn.from_node);
        const auto* dst = view.find_node(conn.to_node);
        if (!src || !dst)
            continue;

        const auto p0 = port_dip(*src, true, conn.from_index);
        const auto p3 = port_dip(*dst, false, conn.to_index);

        const bool cut_hit =
            gesture_ == Gesture::wire_cut &&
            bezier_intersects_segment(p0, p3, z, {start_dip_x_, start_dip_y_}, {current_dip_x_, current_dip_y_});
        const bool upstream_err = src->status && src->status->error.has_value();
        const bool active_touch = conn.from_node == primary_selected || conn.to_node == primary_selected;
        const ui::Color wire_col = cut_hit        ? hex_color(0xff5252)
                                   : upstream_err ? hex_color(0xd9534f)
                                   : active_touch ? hex_color(0xf0c43e)
                                                  : hex_color(0x8fa2b4);
        const float wire_w = std::clamp((cut_hit || active_touch ? 2.2f : 1.65f) * std::sqrt(z), 1.2f, 4.f);
        append_bezier_wire(backdrop, p0, p3, z, wire_w, wire_col, {cw, ch});
    }

    const auto first_label = controls.size();
    for (const auto& node : view.nodes) {
        const auto pos = displayed_position(node);
        const auto tl = to_dip(pos[0], pos[1]);
        const float sx = tl[0];
        const float sy = tl[1];
        const float sw = node_width * z;
        const float sh = node_height * z;
        const float r = std::max(2.f, 5.f * z);
        const bool visible_on_canvas =
            sx + sw + 160.f * z >= 0.f && sx <= cw + 32.f && sy + sh + 24.f >= 0.f && sy <= ch + 24.f;

        const bool is_primary = node.name == primary_selected;
        const bool is_selected = is_primary || std::ranges::find(selected_nodes, node.name) != selected_nodes.end();
        const bool is_hovered = node.name == hovered_node_;
        const bool has_error = node.placeholder || (node.status && node.status->error.has_value());
        const auto cat_it = std::ranges::find(catalog, node.type, &GraphNodeCatalogEntry::name);
        const bool is_source_sop = cat_it != catalog.end() && !cat_it->native;

        if (visible_on_canvas) {
            // Subtle drop shadow:
            bodies.box({{sx + 1.5f * z, sy + 2.f * z, sw, sh}, {0.f, 0.f, 0.f, .28f}, {r, r, r, r}, 0.f, {}});

            // Node main body:
            const ui::Color body_fill = has_error    ? hex_color(0x4c2626)
                                        : is_hovered ? hex_color(0x444444)
                                                     : hex_color(0x383838);
            bodies.box({{sx, sy, sw, sh}, body_fill, {r, r, r, r}, 0.f, {}});

            // Left category/origin cap:
            const float cap_w = 22.f * z;
            const ui::Color left_fill = has_error       ? hex_color(0x7d2d2d)
                                        : is_source_sop ? hex_color(0x2f5360)
                                                        : hex_color(0x474747);
            bodies.box({{sx, sy, cap_w, sh}, left_fill, {r, 0.f, 0.f, r}, 0.f, {}});

            // Right Houdini Display Flag ('D') cap:
            const float fw = flag_width * z;
            const ui::Color flag_fill = node.is_display                         ? hex_color(0x2982dc)
                                        : (is_hovered && hovered_display_flag_) ? hex_color(0x3a4754)
                                                                                : hex_color(0x2a3037);
            bodies.box({{sx + sw - fw, sy, fw, sh}, flag_fill, {0.f, r, r, 0.f}, 0.f, {}});

            // Outer node border / Gold selection highlight:
            const ui::Color outline = is_primary    ? hex_color(0xffd900)
                                      : is_selected ? hex_color(0xd4af37)
                                      : has_error   ? hex_color(0xe05252)
                                                    : hex_color(0x1a1a1a);
            const float border_w = (is_selected || has_error) ? std::max(1.5f, 2.f * std::sqrt(z)) : 1.f;
            bodies.box({{sx, sy, sw, sh}, {0.f, 0.f, 0.f, 0.f}, {r, r, r, r}, border_w, outline});

            // Input & Output port dots in overlay (straddling top & bottom borders):
            for (std::size_t p = 0; p < node.ports.inputs.size(); ++p) {
                const auto pt = port_dip(node, false, p);
                const bool multi = p < node.ports.multiple.size() && node.ports.multiple[p];
                const bool hot = (hovered_port_ && !hovered_port_->is_output && hovered_port_->node == node.name &&
                                  hovered_port_->index == p) ||
                                 (snapped_port_ && !snapped_port_->is_output && snapped_port_->node == node.name &&
                                  snapped_port_->index == p);
                const float pw = (multi ? 18.f : 8.5f) * z;
                const float ph = (multi ? 7.f : 8.5f) * z;
                const float pr = ph * .5f;
                if (hot) {
                    const float hw = pw + 5.f * z;
                    const float hh = ph + 5.f * z;
                    overlay.box({{pt[0] - hw * .5f, pt[1] - hh * .5f, hw, hh},
                                 hex_color(0xffd900, .35f),
                                 {hh * .5f, hh * .5f, hh * .5f, hh * .5f},
                                 0.f,
                                 {}});
                }
                overlay.box({{pt[0] - pw * .5f, pt[1] - ph * .5f, pw, ph},
                             hot ? hex_color(0xffd900) : hex_color(0x9fb5c9),
                             {pr, pr, pr, pr},
                             1.f,
                             hex_color(0x171717)});
            }
            for (std::size_t p = 0; p < node.ports.outputs.size(); ++p) {
                const auto pt = port_dip(node, true, p);
                const bool hot = (hovered_port_ && hovered_port_->is_output && hovered_port_->node == node.name &&
                                  hovered_port_->index == p) ||
                                 (snapped_port_ && snapped_port_->is_output && snapped_port_->node == node.name &&
                                  snapped_port_->index == p);
                const float d = 8.5f * z;
                const float pr = d * .5f;
                if (hot) {
                    const float hd = d + 5.f * z;
                    overlay.box({{pt[0] - hd * .5f, pt[1] - hd * .5f, hd, hd},
                                 hex_color(0xffd900, .35f),
                                 {hd * .5f, hd * .5f, hd * .5f, hd * .5f},
                                 0.f,
                                 {}});
                }
                overlay.box({{pt[0] - pr, pt[1] - pr, d, d},
                             hot ? hex_color(0xffd900) : hex_color(0x9fb5c9),
                             {pr, pr, pr, pr},
                             1.f,
                             hex_color(0x171717)});
            }

            const ui::ControlKey base_key = node_base_key(graph_key, node.name);
            ui::Tooltip help;
            help.title = platform::utf8_to_utf16_validated(node.name);
            help.identity =
                platform::utf8_to_utf16_validated(node.type + (is_source_sop ? " · Source SOP" : " · C++ SOP"));
            std::string desc = cat_it != catalog.end() ? cat_it->description : std::string{};
            if (node.status) {
                if (!desc.empty())
                    desc += "\n";
                desc +=
                    std::format("Evaluations: {} · Cache hits: {}", node.status->evaluations, node.status->cache_hits);
                if (node.status->time)
                    desc += std::format(" · Cook: {:.2f} ms", double(node.status->time->count()) / 1e6);
                if (node.status->error)
                    desc += "\nError: " + node.status->error->message;
            }
            help.description = application::diagnostic_text(desc);
            const auto previous = visible_tooltips_.find(base_key);
            auto content = previous != visible_tooltips_.end() && *previous->second == help
                               ? previous->second
                               : std::make_shared<const ui::Tooltip>(std::move(help));
            tooltips.emplace(base_key, content);

            // Node Title & Tooltip hit target:
            {
                ui::Control title_ctrl;
                title_ctrl.key = base_key;
                title_ctrl.parent = canvas_parent;
                title_ctrl.kind = ui::ControlKind::label;
                title_ctrl.layout = out_of_flow_box(sx, sy, sw, sh);
                title_ctrl.text = platform::utf8_to_utf16_validated(node.name);
                title_ctrl.single_line = true;
                title_ctrl.tooltip = content;
                title_ctrl.style.background = ui::Color{0, 0, 0, 0};
                title_ctrl.style.border_width = 0.f;
                title_ctrl.style.padding = ui_layout::Insets{26.f * z, 0.f, (flag_width + 3.f) * z, 0.f};
                title_ctrl.style.text_vertical_align = ui::TextVerticalAlign::center;
                title_ctrl.style.font_size = std::clamp(std::round(12.f * z), 6.f, 28.f);
                title_ctrl.style.font_weight = is_selected ? std::uint16_t{700} : std::uint16_t{400};
                title_ctrl.style.foreground = is_selected ? hex_color(0xffffff) : hex_color(0xe4e8ec);
                controls.push_back(std::move(title_ctrl));
            }

            if (z >= .55f) {
                // Left SOP icon:
                ui::Control icon_ctrl;
                icon_ctrl.key = base_key + 1;
                icon_ctrl.parent = canvas_parent;
                icon_ctrl.kind = ui::ControlKind::label;
                icon_ctrl.enabled = false;
                icon_ctrl.tooltip = content;
                icon_ctrl.layout = out_of_flow_box(sx + 2.f * z, sy, 20.f * z, sh);
                icon_ctrl.icon = has_error       ? ui::ControlIcon::close
                                 : is_source_sop ? ui::ControlIcon::component
                                                 : ui::ControlIcon::mesh;
                icon_ctrl.style.background = ui::Color{0, 0, 0, 0};
                icon_ctrl.style.border_width = 0.f;
                icon_ctrl.style.padding = ui_layout::Insets{3.f * z, 0.f, 0.f, 0.f};
                icon_ctrl.style.indicator_size = std::clamp(std::round(13.f * z), 7.f, 26.f);
                icon_ctrl.style.indicator_foreground =
                    has_error ? hex_color(0xffb4b4) : (is_source_sop ? hex_color(0x8ed8f8) : hex_color(0xc8d0d8));
                controls.push_back(std::move(icon_ctrl));

                // Right 'D' flag glyph:
                ui::Control flag_ctrl;
                flag_ctrl.key = base_key + 2;
                flag_ctrl.parent = canvas_parent;
                flag_ctrl.kind = ui::ControlKind::label;
                flag_ctrl.enabled = false;
                flag_ctrl.tooltip = content;
                flag_ctrl.layout = out_of_flow_box(sx + sw - fw, sy, fw, sh);
                flag_ctrl.text = L"D";
                flag_ctrl.single_line = true;
                flag_ctrl.style.background = ui::Color{0, 0, 0, 0};
                flag_ctrl.style.border_width = 0.f;
                flag_ctrl.style.padding = ui_layout::Insets{};
                flag_ctrl.style.text_align = ui::TextAlign::center;
                flag_ctrl.style.text_vertical_align = ui::TextVerticalAlign::center;
                flag_ctrl.style.font_size = std::clamp(std::round(11.f * z), 6.f, 24.f);
                flag_ctrl.style.font_weight = 700;
                flag_ctrl.style.foreground = node.is_display ? hex_color(0xffffff) : hex_color(0x7c8a98);
                controls.push_back(std::move(flag_ctrl));
            }

            if (z >= .67f) {
                // External Houdini side badge (Type short-name / cook ms / error):
                std::wstring badge = platform::utf8_to_utf16_validated(short_type_name(node.type));
                if (has_error)
                    badge += L" · ERR";
                else if (node.status && node.status->time)
                    badge += std::format(L" · {:.1f} ms", double(node.status->time->count()) / 1e6);
                ui::Control badge_ctrl;
                badge_ctrl.key = base_key + 3;
                badge_ctrl.parent = canvas_parent;
                badge_ctrl.kind = ui::ControlKind::label;
                badge_ctrl.enabled = false;
                badge_ctrl.tooltip = content;
                badge_ctrl.layout = out_of_flow_box(sx + sw + 6.f * z, sy, 140.f * z, sh);
                badge_ctrl.text = std::move(badge);
                badge_ctrl.single_line = true;
                badge_ctrl.role = ui::Role::caption;
                badge_ctrl.style.background = ui::Color{0, 0, 0, 0};
                badge_ctrl.style.border_width = 0.f;
                badge_ctrl.style.padding = ui_layout::Insets{};
                badge_ctrl.style.text_vertical_align = ui::TextVerticalAlign::center;
                badge_ctrl.style.font_size = std::clamp(std::round(10.5f * z), 6.f, 22.f);
                badge_ctrl.style.foreground = has_error ? hex_color(0xff8080) : hex_color(0x8c96a0);
                badge_ctrl.style.text_shadow = ui::Color{0.f, 0.f, 0.f, .65f};
                controls.push_back(std::move(badge_ctrl));
            }
        }
    }

    // 3. Active interactive foreground overlays (live wire drag, marquee box, Y wire-cut):
    if (gesture_ == Gesture::wire_drag) {
        if (const auto* anchor_node = view.find_node(wire_anchor_.node)) {
            const auto anchor_pt = port_dip(*anchor_node, wire_anchor_.is_output, wire_anchor_.index);
            std::array<float, 2> target_pt{current_dip_x_, current_dip_y_};
            if (snapped_port_) {
                if (const auto* snap_node = view.find_node(snapped_port_->node))
                    target_pt = port_dip(*snap_node, snapped_port_->is_output, snapped_port_->index);
            }
            const auto from_pt = wire_anchor_.is_output ? anchor_pt : target_pt;
            const auto to_pt = wire_anchor_.is_output ? target_pt : anchor_pt;
            append_bezier_wire(overlay, from_pt, to_pt, z, 2.4f,
                               snapped_port_ ? hex_color(0xffd900) : hex_color(0x69b6ff), {cw, ch});
        }
    } else if (gesture_ == Gesture::marquee) {
        const float rx = std::min(start_dip_x_, current_dip_x_);
        const float ry = std::min(start_dip_y_, current_dip_y_);
        const float rw = std::abs(current_dip_x_ - start_dip_x_);
        const float rh = std::abs(current_dip_y_ - start_dip_y_);
        overlay.box({{rx, ry, rw, rh}, hex_color(0xffd900, .12f), {}, 1.25f, hex_color(0xffd900, .9f)});
    } else if (gesture_ == Gesture::wire_cut) {
        overlay.stroke({start_dip_x_, start_dip_y_, current_dip_x_, current_dip_y_, 2.f, hex_color(0xff4d4d)});
    }

    visible_tooltips_.swap(tooltips);
    // Keep paint-only layers below the node hit controls. Extra batches have stable keys.
    std::vector<ui::Control> layers;
    const auto add_layer = [&](CanvasLayer& data, ui::ControlKey key, unsigned layer) {
        for (std::size_t i = 0; i < data.batches.size(); ++i) {
            ui::Control control;
            control.key = i == 0 ? key : network_control::batch_key_base + (i - 1) * 3 + layer;
            control.parent = canvas_parent;
            control.kind = ui::ControlKind::container;
            if (key == network_control::canvas_backdrop && i == 0) {
                control.layout = ui_layout::NodeInput::box(cw, ch);
                control.layout.width = ui_layout::Axis::expand();
                control.layout.height = ui_layout::Axis::expand();
                control.classes = {"network-canvas-bg"};
            } else {
                control.enabled = false;
                control.layout = out_of_flow_box(0.f, 0.f, cw, ch);
                control.style.background = ui::Color{0, 0, 0, 0};
                control.style.border_width = 0.f;
            }
            control.canvas = std::move(data.batches[i]);
            layers.push_back(std::move(control));
        }
    };
    add_layer(backdrop, network_control::canvas_backdrop, 0);
    add_layer(bodies, network_control::canvas_nodes, 1);
    add_layer(overlay, network_control::canvas_overlay, 2);
    controls.insert(controls.begin() + first_label, std::make_move_iterator(layers.begin()),
                    std::make_move_iterator(layers.end()));

    // Instant hovered/snapped connector name badge:
    const auto active_port = snapped_port_ ? snapped_port_ : hovered_port_;
    if (active_port) {
        if (const auto* pn = view.find_node(active_port->node)) {
            const auto pt = port_dip(*pn, active_port->is_output, active_port->index);
            std::string label = active_port->port;
            if (active_port->multiple)
                label += " []";
            ui::Control tag;
            tag.key = network_control::hover_port_tag;
            tag.parent = canvas_parent;
            tag.kind = ui::ControlKind::label;
            tag.enabled = false;
            tag.single_line = true;
            tag.text = platform::utf8_to_utf16_validated(label);
            const float tag_w = std::clamp(static_cast<float>(label.size()) * 7.5f + 16.f, 44.f, 160.f);
            const float tag_y = active_port->is_output ? (pt[1] + 8.f) : (pt[1] - 26.f);
            tag.layout = out_of_flow_box(std::clamp(pt[0] - tag_w * .5f, 2.f, std::max(2.f, cw - tag_w - 2.f)),
                                         std::clamp(tag_y, 2.f, std::max(2.f, ch - 22.f)), tag_w, 20.f);
            tag.style.background = hex_color(0x181818, .92f);
            tag.style.border = hex_color(0xffd900, .75f);
            tag.style.border_width = 1.f;
            tag.style.radius = {3.f, 3.f, 3.f, 3.f};
            tag.style.text_align = ui::TextAlign::center;
            tag.style.text_vertical_align = ui::TextVerticalAlign::center;
            tag.style.font_size = 11.f;
            tag.style.foreground = hex_color(0xffeb80);
            controls.push_back(std::move(tag));
        }
    }
}

} // namespace atelier::editor
