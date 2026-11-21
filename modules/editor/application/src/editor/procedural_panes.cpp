#include "application/diagnostics.hpp"
#include "editor/procedural_panes.hpp"
#include "platform/utf.hpp"
#include "procedural/operations.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <format>
#include <functional>
#include <numbers>

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

void GeometrySpreadsheet::reset(bool preserve_view) noexcept {
    if (!preserve_view)
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

void GeometrySpreadsheet::append(ui_language::ControlOutput controls, ui::ControlKey parent,
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

std::uint32_t blend_hex(std::uint32_t a, std::uint32_t b, float t) noexcept {
    const auto ch = [&](unsigned shift) {
        const float ca = static_cast<float>((a >> shift) & 0xffu);
        const float cb = static_cast<float>((b >> shift) & 0xffu);
        return static_cast<std::uint32_t>(std::clamp(std::lround(ca + (cb - ca) * t), 0L, 255L)) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

std::string short_type_name(std::string_view full_name) {
    const auto dot = full_name.find_last_of('.');
    return std::string(dot == std::string_view::npos ? full_name : full_name.substr(dot + 1));
}

std::wstring sanitize_single_line(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (const wchar_t c : raw) {
        if (c == L'\n')
            out.push_back(L'\u21b5');
        else if (c == L'\r')
            continue;
        else if (c < 32 || c == 127 || c == 0x2028 || c == 0x2029)
            out.push_back(L' ');
        else
            out.push_back(c);
    }
    return out;
}

std::wstring sanitize_multiline_label(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (const wchar_t c : raw) {
        if (c == L'\r')
            continue;
        if ((c < 32 && c != L'\n' && c != L'\t') || c == 127)
            out.push_back(L' ');
        else
            out.push_back(c);
    }
    return out;
}

std::wstring escape_note_single_line(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size() + 8);
    for (const wchar_t c : raw) {
        if (c == L'\r')
            continue;
        if (c == L'\n') {
            out += L"\\n";
        } else if (c < 32 || c == 127 || c == 0x2028 || c == 0x2029) {
            out.push_back(L' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string unescape_note_utf8(std::wstring_view raw) {
    std::wstring expanded;
    expanded.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\u21b5') {
            expanded.push_back(L'\n');
        } else if (raw[i] == L'\\' && i + 1 < raw.size() && raw[i + 1] == L'n') {
            expanded.push_back(L'\n');
            ++i;
        } else {
            expanded.push_back(raw[i]);
        }
    }
    return platform::utf16_to_utf8(expanded).value_or(std::string{});
}

std::uint64_t steady_millis() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::wstring extract_acronym(std::string_view short_name) {
    std::wstring out;
    for (std::size_t i = 0; i < short_name.size(); ++i) {
        const char c = short_name[i];
        if ((c >= 'A' && c <= 'Z') || i == 0 || (i > 0 && short_name[i - 1] == '_')) {
            out.push_back(static_cast<wchar_t>(std::towlower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

bool is_subsequence(std::wstring_view needle, std::wstring_view haystack) noexcept {
    std::size_t ni = 0;
    for (std::size_t hi = 0; hi < haystack.size() && ni < needle.size(); ++hi) {
        if (haystack[hi] == needle[ni])
            ++ni;
    }
    return ni == needle.size();
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

float point_segment_distance_sq(std::array<float, 2> p, std::array<float, 2> a, std::array<float, 2> b) noexcept {
    const float vx = b[0] - a[0];
    const float vy = b[1] - a[1];
    const float wx = p[0] - a[0];
    const float wy = p[1] - a[1];
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 1e-6f ? std::clamp((wx * vx + wy * vy) / len2, 0.f, 1.f) : 0.f;
    const float dx = wx - vx * t;
    const float dy = wy - vy * t;
    return dx * dx + dy * dy;
}

// Houdini-style 3-part wire routing: [Start Bezier] -> [Axis-Aligned Straight Middle] -> [End Bezier].
template <class Fn>
void for_each_wire_segment(std::array<float, 2> from_dip, std::array<float, 2> to_dip, float zoom,
                           Fn&& visit) noexcept {
    const float x0 = from_dip[0];
    const float y0 = from_dip[1];
    const float x1 = to_dip[0];
    const float y1 = to_dip[1];
    const float dx = x1 - x0;
    const float abs_dx = std::abs(dx);
    const float dy = y1 - y0;
    const float s = dx >= 0.f ? 1.f : -1.f;
    constexpr float kappa = 0.55228475f;
    constexpr int curve_steps = 8;

    const auto sample_cubic = [&](std::array<float, 2> b0, std::array<float, 2> c1, std::array<float, 2> c2,
                                  std::array<float, 2> b3) {
        auto prev = b0;
        for (int i = 1; i <= curve_steps; ++i) {
            const auto next = bezier_point(b0, c1, c2, b3, static_cast<float>(i) / static_cast<float>(curve_steps));
            if (visit(prev, next))
                return true;
            prev = next;
        }
        return false;
    };

    if (dy >= 0.f && abs_dx < 0.75f) {
        static_cast<void>(visit(from_dip, to_dip));
        return;
    }

    if (dy > 0.f && abs_dx >= dy) {
        const float r = 0.5f * dy;
        const float y_mid = 0.5f * (y0 + y1);
        const std::array m0{x0 + s * r, y_mid};
        const std::array m1{x1 - s * r, y_mid};
        if (sample_cubic(from_dip, {x0, y0 + kappa * r}, {x0 + s * (1.f - kappa) * r, y_mid}, m0))
            return;
        if (std::abs(m1[0] - m0[0]) > 0.25f && visit(m0, m1))
            return;
        static_cast<void>(sample_cubic(m1, {x1 - s * (1.f - kappa) * r, y_mid}, {x1, y1 - kappa * r}, to_dip));
        return;
    }

    if (dy > 0.f) {
        const float x_mid = 0.5f * (x0 + x1);
        const float h = std::min(0.5f * dy, std::max(0.5f * abs_dx, std::min(20.f * zoom, 0.35f * dy)));
        const std::array m0{x_mid, y0 + h};
        const std::array m1{x_mid, y1 - h};
        if (sample_cubic(from_dip, {x0, y0 + 0.55f * h}, {x_mid, y0 + 0.45f * h}, m0))
            return;
        if (m1[1] - m0[1] > 0.25f && visit(m0, m1))
            return;
        static_cast<void>(sample_cubic(m1, {x_mid, y1 - 0.45f * h}, {x1, y1 - 0.55f * h}, to_dip));
        return;
    }

    const float x_chan = abs_dx >= 56.f * zoom ? 0.5f * (x0 + x1) : (std::max(x0, x1) + 48.f * zoom);
    const float u = std::clamp(0.35f * std::max(abs_dx, 48.f * zoom), 18.f * zoom, 34.f * zoom);
    const std::array m0{x_chan, y0};
    const std::array m1{x_chan, y1};
    if (sample_cubic(from_dip, {x0, y0 + u}, {x_chan, y0 + u}, m0))
        return;
    if (std::abs(m1[1] - m0[1]) > 0.25f && visit(m0, m1))
        return;
    static_cast<void>(sample_cubic(m1, {x_chan, y1 - u}, {x1, y1 - u}, to_dip));
}

float point_wire_distance(std::array<float, 2> pt, std::array<float, 2> from_dip, std::array<float, 2> to_dip,
                          float zoom) noexcept {
    float best_d2 = 1e18f;
    for_each_wire_segment(from_dip, to_dip, zoom, [&](std::array<float, 2> a, std::array<float, 2> b) {
        best_d2 = std::min(best_d2, point_segment_distance_sq(pt, a, b));
        return false;
    });
    return std::sqrt(best_d2);
}

// Each layer emits bounded protocol batches directly, including when all geometry fits on screen.
struct CanvasLayer {
    std::vector<std::shared_ptr<ui::CanvasData>> batches{std::make_shared<ui::CanvasData>()};

    void box(ui::CanvasBox value) {
        if (batches.back()->boxes.size() == ui::CanvasData::max_boxes)
            batches.push_back(std::make_shared<ui::CanvasData>());
        batches.back()->boxes.push_back(value);
    }

    void shape(ui::CanvasNodeShape value) {
        if (batches.back()->shapes.size() == ui::CanvasData::max_shapes)
            batches.push_back(std::make_shared<ui::CanvasData>());
        batches.back()->shapes.push_back(value);
    }

    void arc(ui::CanvasArc value) {
        if (batches.back()->arcs.size() == ui::CanvasData::max_arcs)
            batches.push_back(std::make_shared<ui::CanvasData>());
        batches.back()->arcs.push_back(value);
    }

    void stroke(ui::CanvasStroke value) {
        if (batches.back()->strokes.size() == ui::CanvasData::max_strokes)
            batches.push_back(std::make_shared<ui::CanvasData>());
        batches.back()->strokes.push_back(value);
    }
};

bool wire_near_region(std::array<float, 2> from, std::array<float, 2> to, float zoom, ui::Rect region) {
    // Covers the reverse-route detour, its control points and the widest interactive stroke.
    const float pad = 90.f * zoom + 11.f;
    return std::max(from[0], to[0]) >= region.x - pad && std::min(from[0], to[0]) <= region.x + region.width + pad &&
           std::max(from[1], to[1]) >= region.y - pad && std::min(from[1], to[1]) <= region.y + region.height + pad;
}

void append_wire_segment(CanvasLayer& layer, std::array<float, 2> prev, std::array<float, 2> next, float width,
                         ui::Color color, ui_layout::Size clip) {
    const float pad = width + 1.f;
    const auto [left, right] = std::minmax(prev[0], next[0]);
    const auto [top, bottom] = std::minmax(prev[1], next[1]);
    if (right < -pad || left > clip.width + pad || bottom < -pad || top > clip.height + pad)
        return;
    if (left >= -pad && right <= clip.width + pad && top >= -pad && bottom <= clip.height + pad) {
        layer.stroke({prev[0], prev[1], next[0], next[1], width, color});
        return;
    }
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
}

bool segments_intersect(std::array<float, 2> a, std::array<float, 2> b, std::array<float, 2> c,
                        std::array<float, 2> d) noexcept {
    constexpr double tolerance = 0.01;
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

bool wire_intersects_segment(std::array<float, 2> from_dip, std::array<float, 2> to_dip, float zoom,
                             std::array<float, 2> cut_a, std::array<float, 2> cut_b) noexcept {
    bool hit = false;
    for_each_wire_segment(from_dip, to_dip, zoom, [&](std::array<float, 2> prev, std::array<float, 2> next) {
        if (segments_intersect(cut_a, cut_b, prev, next)) {
            hit = true;
            return true;
        }
        return false;
    });
    return hit;
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

struct AirSectorDef {
    std::uint8_t button_id; // Matches NetworkCanvas::AirButton

    float angle_deg;

    float half_deg;

    ui::ControlIcon icon;

    std::uint32_t idle_rgb;

    const wchar_t* title;

    const char* description;
};

constexpr std::array<AirSectorDef, 5> air_sector_defs{{
    {1, -140.f, 17.5f, ui::ControlIcon::bypass_arrow, 0xeab308, L"Bypass Node [B]",
     "跳过此节点求值并将第 0 个输入几何原样直通到输出。"},
    {2, 180.f, 16.5f, ui::ControlIcon::info_glyph, 0xf1f5f9, L"Node Info & Comment [I]",
     "查看该节点的求值耗时、缓存命中、布尔统计与端口拓扑，并编辑节点备注。"},
    {3, 140.f, 17.5f, ui::ControlIcon::snowflake, 0x7dd3fc, L"Freeze Node",
     "冻结并锁定该节点当前已求值的几何输出快照；显式解冻前不受上游变化影响。"},
    {4, -52.f, 18.0f, ui::ControlIcon::eye, 0x38bdf8, L"Display Flag [D]",
     "切换蓝色主输出标记，将该节点设为当前程序化图的主视输出几何结果。"},
    {5, 52.f, 18.0f, ui::ControlIcon::template_flag, 0xd896ff, L"Template Flag [T]",
     "切换紫红色副视对比线框标记，在 3D 视口中将此节点作为参考线框叠加对比。"},
}};

ui::ControlIcon sop_node_icon(std::string_view type_name, bool is_source_sop, bool has_error) noexcept {
    if (has_error)
        return ui::ControlIcon::close;
    if (is_source_sop)
        return ui::ControlIcon::sliders;
    if (type_name == "procedural.Null")
        return ui::ControlIcon::cursor3d;
    if (type_name == "procedural.Boolean")
        return ui::ControlIcon::select_intersect;
    if (type_name == "procedural.Scatter" || type_name == "procedural.VolumeScatter")
        return ui::ControlIcon::points;
    if (type_name == "procedural.CopyToPoints" || type_name == "procedural.Realize")
        return ui::ControlIcon::orient_scale;
    if (type_name == "procedural.Ray")
        return ui::ControlIcon::point_normals;
    if (type_name == "procedural.NormalAlign")
        return ui::ControlIcon::face_normals;
    if (type_name == "procedural.Transform")
        return ui::ControlIcon::axes;
    if (type_name == "procedural.GroupBox")
        return ui::ControlIcon::bounds_cage;
    if (type_name == "procedural.Promote" || type_name == "procedural.Connectivity")
        return ui::ControlIcon::network_layout;
    if (type_name == "procedural.Merge")
        return ui::ControlIcon::duplicate;
    if (type_name == "procedural.Remove")
        return ui::ControlIcon::trash;
    if (type_name == "procedural.Polyline")
        return ui::ControlIcon::axes_origin;
    return ui::ControlIcon::mesh;
}

std::uint8_t hit_air_sector(float lx, float ly, float cx, float cy, float z) noexcept {
    const float dx = lx - cx;
    const float dy = ly - cy;
    const float dist = std::hypot(dx, dy);
    if (dist < (NetworkCanvas::air_inner_radius - 2.f) * z || dist > (NetworkCanvas::air_outer_radius + 2.5f) * z ||
        dist < 1e-4f)
        return 0;
    const float ux = dx / dist;
    const float uy = dy / dist;
    constexpr float deg_to_rad = std::numbers::pi_v<float> / 180.f;
    for (const auto& def : air_sector_defs) {
        const float rad = def.angle_deg * deg_to_rad;
        const float half_rad = (def.half_deg + 1.5f) * deg_to_rad;
        if (ux * std::cos(rad) + uy * std::sin(rad) >= std::cos(half_rad))
            return def.button_id;
    }
    return 0;
}

} // namespace

std::uint8_t NetworkCanvas::resolve_shape_id(const GraphNodeView& node, bool is_source_sop) noexcept {
    switch (node.shape) {
    case GraphNodeShape::pill:
        return 0;
    case GraphNodeShape::rect:
        return 1;
    case GraphNodeShape::domed:
        return 2;
    case GraphNodeShape::trapezoid:
        return 3;
    case GraphNodeShape::hexagon:
        return 4;
    case GraphNodeShape::ribbon:
        return 5;
    case GraphNodeShape::diamond:
        return 6;
    case GraphNodeShape::circle:
        return 7;
    case GraphNodeShape::auto_role:
        break;
    }
    if (node.type == "procedural.Null")
        return 6;
    if (is_source_sop)
        return 4;
    if (node.ports.inputs.empty())
        return 2;
    if (node.ports.inputs.size() > 1 || (!node.ports.multiple.empty() && node.ports.multiple[0]) ||
        node.type == "procedural.CopyToPoints")
        return 3;
    if (node.type == "procedural.Scatter" || node.type == "procedural.VolumeScatter")
        return 5;
    return 0;
}

NetworkCanvas::TabCategory NetworkCanvas::categorize_node_type(const GraphNodeCatalogEntry& entry) noexcept {
    if (!entry.native)
        return TabCategory::source_sop;
    const auto short_name = short_type_name(entry.name);
    if (short_name == "Scatter" || short_name == "VolumeScatter" || short_name == "CopyToPoints" ||
        short_name == "Realize")
        return TabCategory::scatter_copy;
    if (short_name == "Merge" || short_name == "Remove" || short_name == "Boolean" || short_name == "Null")
        return TabCategory::utility;
    if (entry.ports.inputs.empty() || short_name == "Polyline" || short_name == "MeshImport")
        return TabCategory::primitive;
    return TabCategory::attribute;
}

bool NetworkCanvas::is_canvas_control(ui::ControlKey key) noexcept {
    return key == network_control::canvas || key == network_control::canvas_backdrop ||
           key == network_control::canvas_nodes || key == network_control::canvas_overlay ||
           key == network_control::hover_port_tag ||
           (key >= network_control::node_key_base && key < network_control::node_key_limit);
}

void NetworkCanvas::reset(bool preserve_view) {
    wire_routes_ = {};
    idle_projection_.reset();
    cancel_gesture();
    close_tab_search();
    close_popup();
    if (!preserve_view) {
        active_graph_.clear();
        pan_x_ = 160.f;
        pan_y_ = 56.f;
        zoom_index_ = default_zoom_index;
        framed_ = false;
        saved_views_.clear();
        selected_note_.clear();
        selected_box_.clear();
    }
    hovered_port_.reset();
    hovered_node_.clear();
    air_node_.clear();
    hovered_flag_ = FlagZone::none;
    hovered_air_ = AirButton::none;
    cut_key_held_ = false;
    wheel_remainder_ = 0.f;
    last_click_tick_ = 0;
    last_click_target_.clear();
    visible_tooltips_.clear();
    node_keys_.clear();
    next_node_key_ = network_control::node_key_base;
}

void NetworkCanvas::cancel_gesture() noexcept {
    gesture_ = Gesture::none;
    keyboard_grab_ = false;
    amend_duplicate_ = false;
    pressed_node_.clear();
    active_annotation_.clear();
    drag_origins_.clear();
    drag_note_origins_.clear();
    drag_box_origins_.clear();
    drag_world_delta_ = {};
    splice_wire_.reset();
    detached_wire_.reset();
    snapped_port_.reset();
}

ui::ControlKey NetworkCanvas::node_base_key(std::string_view graph_key, std::string_view item_name) {
    std::string combined;
    combined.reserve(graph_key.size() + 1 + item_name.size());
    combined.append(graph_key);
    combined.push_back('\n');
    combined.append(item_name);
    if (const auto found = node_keys_.find(combined); found != node_keys_.end())
        return found->second;
    const auto allocated = next_node_key_;
    if (next_node_key_ + 12 < network_control::batch_key_base)
        next_node_key_ += 12;
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
    if (gesture_ == Gesture::move_nodes || gesture_ == Gesture::move_box) {
        const auto found =
            std::ranges::find_if(drag_origins_, [&](const auto& item) { return item.first == node.name; });
        if (found != drag_origins_.end())
            return {found->second[0] + drag_world_delta_[0], found->second[1] + drag_world_delta_[1]};
    }
    return node.position;
}

std::array<float, 2> NetworkCanvas::displayed_note_position(const GraphStickyNote& note) const {
    if (gesture_ == Gesture::move_note || gesture_ == Gesture::move_box) {
        const auto found =
            std::ranges::find_if(drag_note_origins_, [&](const auto& item) { return item.first == note.name; });
        if (found != drag_note_origins_.end())
            return {found->second[0] + drag_world_delta_[0], found->second[1] + drag_world_delta_[1]};
    }
    return note.position;
}

std::array<float, 2> NetworkCanvas::displayed_note_size(const GraphStickyNote& note) const {
    if (gesture_ == Gesture::resize_note && active_annotation_ == note.name) {
        return {std::max(120.f, annotation_origin_size_[0] + drag_world_delta_[0]),
                std::max(64.f, annotation_origin_size_[1] + drag_world_delta_[1])};
    }
    return note.size;
}

std::array<float, 2> NetworkCanvas::displayed_box_position(const GraphNetworkBox& box) const {
    if (gesture_ == Gesture::move_box) {
        const auto found =
            std::ranges::find_if(drag_box_origins_, [&](const auto& item) { return item.first == box.name; });
        if (found != drag_box_origins_.end())
            return {found->second[0] + drag_world_delta_[0], found->second[1] + drag_world_delta_[1]};
    }
    return box.position;
}

std::array<float, 2> NetworkCanvas::displayed_box_size(const GraphNetworkBox& box) const {
    if (gesture_ == Gesture::resize_box && active_annotation_ == box.name) {
        return {std::max(160.f, annotation_origin_size_[0] + drag_world_delta_[0]),
                std::max(96.f, annotation_origin_size_[1] + drag_world_delta_[1])};
    }
    return box.size;
}

std::array<float, 2> NetworkCanvas::port_dip(const GraphNodeView& node, bool is_output, std::size_t port_index) const {
    const auto pos = displayed_position(node);
    const auto top_left = to_dip(pos[0], pos[1]);
    const float z = zoom();
    const float sw = node_width * z;
    const float sh = node_height * z;
    const bool is_circle = resolve_shape_id(node, false) == 7;
    const float vis_x = is_circle ? (top_left[0] + (node_width - circle_width) * .5f * z) : top_left[0];
    const float vis_w = (is_circle ? circle_width : node_width) * z;
    const std::size_t total =
        std::max<std::size_t>(1, is_output ? node.ports.outputs.size() : node.ports.inputs.size());
    const float px = total == 1 ? (top_left[0] + sw * .5f)
                                : (vis_x + vis_w * static_cast<float>(port_index + 1) / static_cast<float>(total + 1));
    const float py = is_output ? (top_left[1] + sh + pin_offset * z) : (top_left[1] - pin_offset * z);
    return {px, py};
}

void NetworkCanvas::sync_graph(std::string_view graph_key, std::span<const GraphNodeView> nodes,
                               ui_layout::Size canvas_dip, std::span<const GraphNetworkBox> boxes,
                               std::span<const GraphStickyNote> notes) {
    if (canvas_dip.width > 32.f && canvas_dip.height > 32.f)
        last_canvas_dip_ = canvas_dip;
    if (active_graph_ != graph_key) {
        if (!active_graph_.empty())
            saved_views_[active_graph_] = {pan_x_, pan_y_, zoom_index_, framed_};
        cancel_gesture();
        close_tab_search();
        close_popup();
        selected_note_.clear();
        selected_box_.clear();
        hovered_port_.reset();
        hovered_node_.clear();
        air_node_.clear();
        hovered_flag_ = FlagZone::none;
        hovered_air_ = AirButton::none;
        active_graph_ = std::string(graph_key);
        if (const auto found = saved_views_.find(active_graph_); found != saved_views_.end()) {
            pan_x_ = found->second.pan_x;
            pan_y_ = found->second.pan_y;
            zoom_index_ = found->second.zoom_index;
            framed_ = found->second.framed;
        } else {
            pan_x_ = 160.f;
            pan_y_ = 56.f;
            zoom_index_ = default_zoom_index;
            framed_ = false;
        }
    }
    const auto has_node = [&](std::string_view name) {
        return std::ranges::find(nodes, name, &GraphNodeView::name) != nodes.end();
    };
    const auto has_note = [&](std::string_view name) {
        return std::ranges::find(notes, name, &GraphStickyNote::name) != notes.end();
    };
    const auto has_box = [&](std::string_view name) {
        return std::ranges::find(boxes, name, &GraphNetworkBox::name) != boxes.end();
    };
    if (!selected_note_.empty() && !has_note(selected_note_))
        selected_note_.clear();
    if (!selected_box_.empty() && !has_box(selected_box_))
        selected_box_.clear();
    if (!hovered_node_.empty() && !has_node(hovered_node_)) {
        hovered_node_.clear();
        hovered_flag_ = FlagZone::none;
    }
    if (!air_node_.empty() && !has_node(air_node_)) {
        air_node_.clear();
        hovered_air_ = AirButton::none;
    }
    if (hovered_port_ && !has_node(hovered_port_->node))
        hovered_port_.reset();
    if (popup_.open()) {
        if (popup_.kind == PopupKind::edit_note) {
            if (!has_note(popup_.target_name))
                close_popup();
        } else if (popup_.kind == PopupKind::edit_box) {
            if (!has_box(popup_.target_name))
                close_popup();
        } else if (popup_.kind == PopupKind::node_info) {
            if (!has_node(popup_.target_name))
                close_popup();
        } else {
            std::erase_if(popup_.target_nodes, [&](const std::string& n) { return !has_node(n); });
            if (!popup_.target_name.empty() && !has_node(popup_.target_name))
                popup_.target_name.clear();
            if (!popup_.palette_floating && popup_.target_nodes.empty())
                close_popup();
        }
    }
    if (!framed_ && last_canvas_dip_.width > 32.f && last_canvas_dip_.height > 32.f) {
        if (!nodes.empty() || !boxes.empty() || !notes.empty())
            frame_nodes(nodes, last_canvas_dip_, {}, boxes, notes);
        else {
            zoom_index_ = default_zoom_index;
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
    zoom_index_ = default_zoom_index;
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
                                std::span<const std::string> only_selected, std::span<const GraphNetworkBox> boxes,
                                std::span<const GraphStickyNote> notes) {
    const float cw = canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width;
    const float ch = canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height;
    if (nodes.empty() && boxes.empty() && notes.empty()) {
        zoom_index_ = default_zoom_index;
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
            max_x = std::max(max_x, pos[0] + node_width + 72.f);
            max_y = std::max(max_y, pos[1] + node_height);
            ++matched;
        }
    };
    if (!only_selected.empty())
        accumulate(true);
    const bool include_annotations = (matched == 0);
    if (matched == 0)
        accumulate(false);
    if (include_annotations) {
        for (const auto& b : boxes) {
            const auto pos = displayed_box_position(b);
            const auto sz = displayed_box_size(b);
            min_x = std::min(min_x, pos[0]);
            min_y = std::min(min_y, pos[1]);
            max_x = std::max(max_x, pos[0] + sz[0]);
            max_y = std::max(max_y, pos[1] + sz[1]);
            ++matched;
        }
        for (const auto& note : notes) {
            const auto pos = displayed_note_position(note);
            const auto sz = displayed_note_size(note);
            min_x = std::min(min_x, pos[0]);
            min_y = std::min(min_y, pos[1]);
            max_x = std::max(max_x, pos[0] + sz[0]);
            max_y = std::max(max_y, pos[1] + sz[1]);
            ++matched;
        }
    }

    const float span_w = std::max(node_width + 72.f, max_x - min_x) + 144.f;
    const float span_h = std::max(node_height, max_y - min_y) + 128.f;
    std::size_t chosen = 0;
    for (std::size_t i = 0; i <= default_zoom_index; ++i) {
        if (span_w * zoom_steps[i] <= cw && span_h * zoom_steps[i] <= ch)
            chosen = i;
    }
    zoom_index_ = chosen;
    const float z = zoom();
    const float center_wx = (min_x + max_x - 36.f) * .5f;
    const float center_wy = (min_y + max_y) * .5f;
    pan_x_ = std::round(cw * .5f - center_wx * z);
    pan_y_ = std::round(ch * .5f - center_wy * z);
    framed_ = true;
}

void NetworkCanvas::open_tab_search(float dip_x, float dip_y, ui_layout::Size canvas_dip, std::string from_node,
                                    std::string from_port, std::string to_node, std::string to_port) {
    close_popup();
    const float cw = canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width;
    const float ch = canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height;
    tab_search_.open = true;
    tab_search_.world_pos = to_world(dip_x, dip_y);
    tab_search_.dip_x = std::clamp(dip_x, 8.f, std::max(8.f, cw - 260.f));
    tab_search_.dip_y = std::clamp(dip_y, 8.f, std::max(8.f, ch - 296.f));
    tab_search_.query.clear();
    tab_search_.category = TabCategory::all;
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

void NetworkCanvas::set_tab_category(TabCategory category) noexcept {
    if (tab_search_.category != category) {
        tab_search_.category = category;
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
    const auto q = lowercase_w(tab_search_.query);
    struct ScoredEntry {
        int score{};

        const GraphNodeCatalogEntry* entry{};
    };
    std::vector<ScoredEntry> scored;
    scored.reserve(catalog.size());
    for (const auto& item : catalog) {
        if (tab_search_.category != TabCategory::all && categorize_node_type(item) != tab_search_.category)
            continue;
        if (q.empty()) {
            scored.push_back({100, &item});
            continue;
        }
        const auto sname = short_type_name(item.name);
        const auto short_lower = lowercase_w(platform::utf8_to_utf16_validated(sname));
        const auto full_lower = lowercase_w(platform::utf8_to_utf16_validated(item.name));
        const auto acronym = extract_acronym(sname);
        int score = 0;
        if (short_lower == q)
            score = 500;
        else if (short_lower.starts_with(q))
            score = 380;
        else if (!acronym.empty() && acronym == q)
            score = 340;
        else if (!acronym.empty() && acronym.starts_with(q))
            score = 290;
        else if (short_lower.find(q) != std::wstring::npos)
            score = 220;
        else if (full_lower.find(q) != std::wstring::npos)
            score = 160;
        else if (is_subsequence(q, short_lower))
            score = 110;
        if (score > 0)
            scored.push_back({score, &item});
    }
    std::ranges::sort(scored, [](const ScoredEntry& a, const ScoredEntry& b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.entry->name < b.entry->name;
    });
    std::vector<GraphNodeCatalogEntry> out;
    out.reserve(std::min(scored.size(), max_tab_results));
    for (std::size_t i = 0; i < scored.size() && i < max_tab_results; ++i)
        out.push_back(*scored[i].entry);
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

void NetworkCanvas::open_node_popup(PopupKind kind, const GraphInspectView& view, std::string_view primary_selected,
                                    std::span<const std::string> selected_nodes, ui_layout::Size canvas_dip,
                                    std::optional<std::array<float, 2>> anchor_dip) {
    const float cw = canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width;
    const float ch = canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height;
    std::string target = !primary_selected.empty() ? std::string(primary_selected)
                                                   : (!selected_nodes.empty() ? selected_nodes.back() : std::string{});
    const auto* nv = target.empty() ? nullptr : view.find_node(target);
    if (!nv && kind == PopupKind::node_info)
        return;
    close_tab_search();
    Popup next;
    next.kind = kind;
    next.palette_floating = (nv == nullptr);
    if (nv) {
        next.target_name = target;
        if (!selected_nodes.empty() && std::ranges::find(selected_nodes, target) != selected_nodes.end()) {
            next.target_nodes.assign(selected_nodes.begin(), selected_nodes.end());
        } else {
            next.target_nodes = {target};
        }
        next.body_edit = sanitize_single_line(platform::utf8_to_utf16_validated(nv->comment));
    }

    float px = cw - 244.f;
    float py = 10.f;
    if (anchor_dip) {
        px = (*anchor_dip)[0];
        py = (*anchor_dip)[1];
    } else if (nv) {
        const auto tl = to_dip(nv->position[0], nv->position[1]);
        px = tl[0] + (node_width + 18.f) * zoom();
        py = tl[1] - 12.f;
    }
    next.dip_x = std::clamp(px, 8.f, std::max(8.f, cw - 246.f));
    next.dip_y = std::clamp(py, 8.f, std::max(8.f, ch - 196.f));
    popup_ = std::move(next);
}

Result<bool> NetworkCanvas::apply_action(Session& session, std::string_view graph_key, std::string& primary_selected,
                                         std::vector<std::string>& selected_nodes, ui_layout::Size canvas_dip,
                                         const ui::Action& action, std::wstring& out_status) {
    if (action.key == network_control::tab_search_input) {
        if (const auto* text = std::get_if<std::wstring>(&action.value))
            if (action.kind == ui::ActionKind::edit_preview || action.kind == ui::ActionKind::edit_commit)
                set_tab_query(*text);
        return true;
    }
    if (action.kind == ui::ActionKind::activate && action.key >= network_control::tab_cat_first &&
        action.key < network_control::tab_cat_first + 6) {
        set_tab_category(static_cast<TabCategory>(action.key - network_control::tab_cat_first));
        return true;
    }
    if (action.kind == ui::ActionKind::activate && action.key == network_control::zoom_reset) {
        reset_zoom(canvas_dip);
        out_status = L"Network zoom 100%";
        return true;
    }
    if (graph_key.empty() || !session.has_graph(graph_key))
        return false;

    if (action.kind == ui::ActionKind::activate &&
        (action.key == network_control::frame_all || action.key == network_control::frame_selected)) {
        ATELIER_TRY_BIND(insp, session.graph_topology(graph_key));
        const bool sel_only = action.key == network_control::frame_selected;
        frame_nodes((*insp)->nodes, canvas_dip,
                    sel_only ? std::span<const std::string>{selected_nodes} : std::span<const std::string>{},
                    (*insp)->boxes, (*insp)->notes);
        out_status = (sel_only && !selected_nodes.empty()) ? L"Framed SOP selection" : L"Framed all SOP nodes";
        return true;
    }
    if (action.kind == ui::ActionKind::activate &&
        (action.key == network_control::select_all_nodes || action.key == network_control::deselect_all_nodes ||
         action.key == network_control::invert_select_nodes)) {
        ATELIER_TRY_BIND(insp, session.graph_topology(graph_key));
        if (action.key == network_control::select_all_nodes) {
            selected_nodes.clear();
            for (const auto& n : (*insp)->nodes)
                selected_nodes.push_back(n.name);
            primary_selected = selected_nodes.empty() ? std::string{} : selected_nodes.back();
            out_status = L"Selected all SOP nodes";
        } else if (action.key == network_control::deselect_all_nodes) {
            selected_nodes.clear();
            primary_selected.clear();
            selected_note_.clear();
            selected_box_.clear();
            out_status = L"Cleared SOP selection";
        } else {
            std::vector<std::string> inverted;
            for (const auto& n : (*insp)->nodes)
                if (std::ranges::find(selected_nodes, n.name) == selected_nodes.end())
                    inverted.push_back(n.name);
            selected_nodes = std::move(inverted);
            primary_selected = selected_nodes.empty() ? std::string{} : selected_nodes.back();
            out_status = L"Inverted SOP selection";
        }
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key == network_control::popup_close) {
        close_popup();
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key == network_control::popup_switch_style) {
        if (popup_.kind == PopupKind::color_picker)
            popup_.kind = PopupKind::shape_picker;
        else if (popup_.kind == PopupKind::shape_picker)
            popup_.kind = PopupKind::color_picker;
        return true;
    }

    if (action.kind == ui::ActionKind::activate &&
        (action.key == network_control::open_shape_picker || action.key == network_control::open_color_picker ||
         action.key == network_control::open_node_info)) {
        const PopupKind kind = action.key == network_control::open_shape_picker   ? PopupKind::shape_picker
                               : action.key == network_control::open_color_picker ? PopupKind::color_picker
                                                                                  : PopupKind::node_info;
        if (popup_.open() && popup_.kind == kind) {
            close_popup();
            return true;
        }
        ATELIER_TRY_BIND(insp, session.graph_topology(graph_key));
        open_node_popup(kind, **insp, primary_selected, selected_nodes, canvas_dip);
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key == network_control::add_sticky_note) {
        if (session.world().running())
            return true;
        const float cx = (canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width) * .5f;
        const float cy = (canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height) * .5f;
        const auto wp = to_world(cx - 105.f, cy - 58.f);
        ATELIER_TRY_BIND(created, session.graph_add_note(graph_key, {std::round(wp[0]), std::round(wp[1])},
                                                         {216.f, 116.f}, "Note", "Double-click header to edit."));
        selected_note_ = *created;
        selected_box_.clear();
        out_status = L"Added Sticky Note: " + platform::utf8_to_utf16_validated(*created);
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key == network_control::add_network_box) {
        if (session.world().running())
            return true;
        ATELIER_TRY_BIND(insp, session.graph_topology(graph_key));
        std::array<float, 2> pos{};
        std::array<float, 2> sz{300.f, 210.f};
        if (!selected_nodes.empty()) {
            float min_x = 1e9f;
            float min_y = 1e9f;
            float max_x = -1e9f;
            float max_y = -1e9f;
            for (const auto& name : selected_nodes) {
                if (const auto* n = (*insp)->find_node(name)) {
                    min_x = std::min(min_x, n->position[0]);
                    min_y = std::min(min_y, n->position[1]);
                    max_x = std::max(max_x, n->position[0] + node_width + 76.f);
                    max_y = std::max(max_y, n->position[1] + node_height);
                }
            }
            if (min_x <= max_x) {
                pos = {std::round(min_x - 28.f), std::round(min_y - 40.f)};
                sz = {std::max(220.f, std::round(max_x - min_x + 56.f)),
                      std::max(130.f, std::round(max_y - min_y + 68.f))};
            }
        } else {
            const float cx = (canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width) * .5f;
            const float cy = (canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height) * .5f;
            const auto wp = to_world(cx - 150.f, cy - 105.f);
            pos = {std::round(wp[0]), std::round(wp[1])};
        }
        ATELIER_TRY_BIND(created, session.graph_add_box(graph_key, pos, sz, "Network Box"));
        selected_box_ = *created;
        selected_note_.clear();
        out_status = L"Added Network Box: " + platform::utf8_to_utf16_validated(*created);
        return true;
    }

    if (!popup_.open() || session.world().running())
        return false;

    const auto active_style_targets = [&]() -> const std::vector<std::string>& {
        return !selected_nodes.empty() ? selected_nodes : popup_.target_nodes;
    };

    if (action.kind == ui::ActionKind::activate && action.key >= network_control::shape_pick_first &&
        action.key < network_control::shape_pick_first + 9) {
        const auto& targets = active_style_targets();
        if (targets.empty()) {
            out_status = L"Select one or more SOP nodes to apply shape";
            return true;
        }
        const auto shape = static_cast<GraphNodeShape>(action.key - network_control::shape_pick_first);
        ATELIER_TRY(session.graph_set_node_appearance(graph_key, targets, shape, {}, {}));
        const auto sname = graph_node_shape_name(shape);
        out_status =
            L"Node shape: " + (sname.empty() ? std::wstring(L"Auto (Role)") : platform::utf8_to_utf16_validated(sname));
        if (!popup_.palette_floating)
            close_popup();
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key == network_control::color_reset_btn) {
        const auto& targets = active_style_targets();
        if (!targets.empty()) {
            ATELIER_TRY(session.graph_set_node_appearance(graph_key, targets, {}, std::optional<std::uint32_t>{}, {}));
            out_status = L"Reset node color to role default";
        }
        if (!popup_.palette_floating)
            close_popup();
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key >= network_control::color_pick_first &&
        action.key < network_control::color_pick_first + color_palette.size()) {
        const std::uint32_t rgb = color_palette[action.key - network_control::color_pick_first];
        if (popup_.kind == PopupKind::edit_note) {
            ATELIER_TRY(session.graph_update_note(graph_key, popup_.target_name, {}, {}, {}, {}, rgb));
            out_status = L"Updated Sticky Note color";
        } else if (popup_.kind == PopupKind::edit_box) {
            ATELIER_TRY(session.graph_update_box(graph_key, popup_.target_name, {}, {}, {}, rgb));
            out_status = L"Updated Network Box color";
        } else {
            const auto& targets = active_style_targets();
            if (!targets.empty()) {
                ATELIER_TRY(
                    session.graph_set_node_appearance(graph_key, targets, {}, std::optional<std::uint32_t>{rgb}, {}));
                out_status = L"Updated node color: " + platform::utf8_to_utf16_validated(format_graph_hex_color(rgb));
                if (!popup_.palette_floating)
                    close_popup();
            } else if (!selected_note_.empty()) {
                ATELIER_TRY(session.graph_update_note(graph_key, selected_note_, {}, {}, {}, {}, rgb));
                out_status = L"Updated Sticky Note color";
            } else if (!selected_box_.empty()) {
                ATELIER_TRY(session.graph_update_box(graph_key, selected_box_, {}, {}, {}, rgb));
                out_status = L"Updated Network Box color";
            } else {
                out_status = L"Select SOP nodes, a Sticky Note, or a Network Box to color";
            }
        }
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key == network_control::popup_delete_item) {
        const std::array names{popup_.target_name};
        if (popup_.kind == PopupKind::edit_note) {
            ATELIER_TRY(session.graph_remove_notes(graph_key, names));
            selected_note_.clear();
            out_status = L"Deleted Sticky Note";
        } else if (popup_.kind == PopupKind::edit_box) {
            ATELIER_TRY(session.graph_remove_boxes(graph_key, names));
            selected_box_.clear();
            out_status = L"Deleted Network Box";
        }
        close_popup();
        return true;
    }

    if (action.key == network_control::info_comment_input) {
        if (const auto* text = std::get_if<std::wstring>(&action.value)) {
            popup_.body_edit = sanitize_single_line(*text);
            if (action.kind == ui::ActionKind::edit_commit) {
                const auto utf8 = platform::utf16_to_utf8(popup_.body_edit).value_or(std::string{});
                ATELIER_TRY(session.graph_set_node_appearance(graph_key, popup_.target_nodes, {}, {}, utf8));
                out_status = L"Updated node comment";
            }
        }
        return true;
    }

    if (action.key == network_control::note_title_input) {
        if (const auto* text = std::get_if<std::wstring>(&action.value)) {
            popup_.title_edit = sanitize_single_line(*text);
            if (action.kind == ui::ActionKind::edit_commit) {
                const auto utf8 = platform::utf16_to_utf8(popup_.title_edit).value_or(std::string{});
                ATELIER_TRY(session.graph_update_note(graph_key, popup_.target_name, {}, {}, utf8, {}, {}));
                out_status = L"Updated Sticky Note title";
            }
        }
        return true;
    }

    if (action.key == network_control::note_body_input) {
        if (const auto* text = std::get_if<std::wstring>(&action.value)) {
            popup_.body_edit = sanitize_single_line(*text);
            if (action.kind == ui::ActionKind::edit_commit) {
                ATELIER_TRY(session.graph_update_note(graph_key, popup_.target_name, {}, {}, {},
                                                      unescape_note_utf8(popup_.body_edit), {}));
                out_status = L"Updated Sticky Note text";
            }
        }
        return true;
    }

    if (action.key == network_control::box_title_input) {
        if (const auto* text = std::get_if<std::wstring>(&action.value)) {
            popup_.title_edit = sanitize_single_line(*text);
            if (action.kind == ui::ActionKind::edit_commit) {
                const auto utf8 = platform::utf16_to_utf8(popup_.title_edit).value_or(std::string{});
                ATELIER_TRY(session.graph_update_box(graph_key, popup_.target_name, {}, {}, utf8, {}));
                out_status = L"Updated Network Box title";
            }
        }
        return true;
    }

    return false;
}

Result<NetworkCanvas::InteractionOutcome> NetworkCanvas::pointer_event(Session& session, std::string_view graph_key,
                                                                       std::string& primary_selected,
                                                                       std::vector<std::string>& selected_nodes,
                                                                       ui::Rect canvas_px, float dpi_scale,
                                                                       const ui::Event& event, Project* project) {
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
    sync_graph(graph_key, view.nodes, canvas_dip, view.boxes, view.notes);

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
        const float z = zoom();

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
            } else {
                if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                    const std::string clicked = pressed_node_;
                    const bool shift = gesture_shift_;
                    cancel_gesture();
                    out.capture_release = true;
                    out.cursor = ui::Cursor::arrow;
                    selected_note_.clear();
                    selected_box_.clear();
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
                }
                return out;
            }
        }

        if (gesture_ == Gesture::move_nodes) {
            drag_world_delta_ = {(lx - start_dip_x_) / z, (ly - start_dip_y_) / z};
            splice_wire_.reset();
            if (drag_origins_.size() == 1) {
                const auto* dragged = view.find_node(drag_origins_.front().first);
                const bool has_existing_wires =
                    dragged && std::ranges::any_of(view.connections, [&](const GraphConnectionView& c) {
                        return c.from_node == dragged->name || c.to_node == dragged->name;
                    });
                if (dragged && !dragged->placeholder && !dragged->ports.inputs.empty() &&
                    !dragged->ports.outputs.empty() && !has_existing_wires) {
                    const auto pos = displayed_position(*dragged);
                    const auto tl = to_dip(pos[0], pos[1]);
                    const std::array center{tl[0] + node_width * .5f * z, tl[1] + node_height * .5f * z};
                    float best_dist = std::max(12.f, 15.f * z);
                    for (const auto& conn : view.connections) {
                        if (conn.from_node == dragged->name || conn.to_node == dragged->name)
                            continue;
                        const auto* src = view.find_node(conn.from_node);
                        const auto* dst = view.find_node(conn.to_node);
                        if (!src || !dst)
                            continue;
                        const float d = point_wire_distance(center, port_dip(*src, true, conn.from_index),
                                                            port_dip(*dst, false, conn.to_index), z);
                        if (d <= best_dist) {
                            best_dist = d;
                            splice_wire_ = conn;
                        }
                    }
                }
            }
            if (event.kind == ui::EventKind::pointer_move) {
                out.cursor = ui::Cursor::move;
                return out;
            }
            if (keyboard_grab_ && event.kind == ui::EventKind::pointer_down &&
                event.button == ui::PointerButton::secondary) {
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                out.status = L"Cancelled SOP node move";
                return out;
            }
            const bool commit =
                keyboard_grab_
                    ? (event.kind == ui::EventKind::pointer_down && event.button == ui::PointerButton::primary)
                    : (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_);
            if (commit) {
                std::vector<std::pair<std::string, std::array<float, 2>>> moves;
                moves.reserve(drag_origins_.size());
                for (const auto& [name, origin] : drag_origins_)
                    moves.emplace_back(name, std::array{std::round(origin[0] + drag_world_delta_[0]),
                                                        std::round(origin[1] + drag_world_delta_[1])});
                const auto splice = splice_wire_;
                const bool amend = amend_duplicate_;
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                if (splice && moves.size() == 1) {
                    if (session.graph_splice_node(graph_key, moves.front().first, moves.front().second, *splice)) {
                        out.status =
                            L"Spliced '" + platform::utf8_to_utf16_validated(moves.front().first) + L"' into wire";
                        return out;
                    }
                }
                ATELIER_TRY(session.graph_move_nodes(graph_key, moves, amend));
                out.status = amend ? L"Placed duplicated SOP node(s)" : L"Moved SOP node(s)";
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::move_note) {
            drag_world_delta_ = {(lx - start_dip_x_) / z, (ly - start_dip_y_) / z};
            if (event.kind == ui::EventKind::pointer_move) {
                out.cursor = ui::Cursor::move;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const std::string target = active_annotation_;
                const std::array next_pos{std::round(annotation_origin_pos_[0] + drag_world_delta_[0]),
                                          std::round(annotation_origin_pos_[1] + drag_world_delta_[1])};
                const bool clicked_only = move_dist < 4.f;
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                if (clicked_only) {
                    const auto now = steady_millis();
                    if (last_click_target_ == "note:" + target && now - last_click_tick_ <= 420) {
                        if (const auto* n = view.find_note(target)) {
                            const auto tl = to_dip(n->position[0], n->position[1]);
                            popup_.kind = PopupKind::edit_note;
                            popup_.target_name = target;
                            popup_.title_edit = sanitize_single_line(platform::utf8_to_utf16_validated(n->title));
                            popup_.body_edit = escape_note_single_line(platform::utf8_to_utf16_validated(n->text));
                            popup_.dip_x =
                                std::clamp(tl[0] + n->size[0] * z + 10.f, 8.f, std::max(8.f, canvas_dip.width - 252.f));
                            popup_.dip_y = std::clamp(tl[1], 8.f, std::max(8.f, canvas_dip.height - 180.f));
                        }
                        last_click_tick_ = 0;
                    } else {
                        last_click_target_ = "note:" + target;
                        last_click_tick_ = now;
                    }
                    return out;
                }
                const std::array note_moves{std::pair{target, next_pos}};
                ATELIER_TRY(session.graph_move_canvas_items(graph_key, {}, note_moves, {}));
                out.status = L"Moved Sticky Note";
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::resize_note) {
            drag_world_delta_ = {(lx - start_dip_x_) / z, (ly - start_dip_y_) / z};
            if (event.kind == ui::EventKind::pointer_move) {
                out.cursor = ui::Cursor::scale;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const std::string target = active_annotation_;
                const std::array next_sz{std::max(120.f, std::round(annotation_origin_size_[0] + drag_world_delta_[0])),
                                         std::max(64.f, std::round(annotation_origin_size_[1] + drag_world_delta_[1]))};
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                ATELIER_TRY(session.graph_update_note(graph_key, target, {}, next_sz, {}, {}, {}));
                out.status = L"Resized Sticky Note";
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::move_box) {
            drag_world_delta_ = {(lx - start_dip_x_) / z, (ly - start_dip_y_) / z};
            if (event.kind == ui::EventKind::pointer_move) {
                out.cursor = ui::Cursor::move;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const std::string target = active_annotation_;
                const bool clicked_only = move_dist < 4.f;
                std::vector<std::pair<std::string, std::array<float, 2>>> node_moves;
                std::vector<std::pair<std::string, std::array<float, 2>>> note_moves;
                std::vector<std::pair<std::string, std::array<float, 2>>> box_moves;
                for (const auto& [n, orig] : drag_origins_)
                    node_moves.emplace_back(n, std::array{std::round(orig[0] + drag_world_delta_[0]),
                                                          std::round(orig[1] + drag_world_delta_[1])});
                for (const auto& [n, orig] : drag_note_origins_)
                    note_moves.emplace_back(n, std::array{std::round(orig[0] + drag_world_delta_[0]),
                                                          std::round(orig[1] + drag_world_delta_[1])});
                for (const auto& [n, orig] : drag_box_origins_)
                    box_moves.emplace_back(n, std::array{std::round(orig[0] + drag_world_delta_[0]),
                                                         std::round(orig[1] + drag_world_delta_[1])});
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                if (clicked_only) {
                    const auto now = steady_millis();
                    if (last_click_target_ == "box:" + target && now - last_click_tick_ <= 420) {
                        if (const auto* b = view.find_box(target)) {
                            const auto tl = to_dip(b->position[0], b->position[1]);
                            popup_.kind = PopupKind::edit_box;
                            popup_.target_name = target;
                            popup_.title_edit = sanitize_single_line(platform::utf8_to_utf16_validated(b->title));
                            popup_.dip_x = std::clamp(tl[0] + 12.f, 8.f, std::max(8.f, canvas_dip.width - 244.f));
                            popup_.dip_y = std::clamp(tl[1] + 28.f, 8.f, std::max(8.f, canvas_dip.height - 140.f));
                        }
                        last_click_tick_ = 0;
                    } else {
                        last_click_target_ = "box:" + target;
                        last_click_tick_ = now;
                    }
                    return out;
                }
                ATELIER_TRY(session.graph_move_canvas_items(graph_key, node_moves, note_moves, box_moves));
                out.status = std::format(L"Moved Network Box ({} enclosed nodes)", node_moves.size());
                return out;
            }
            return out;
        }

        if (gesture_ == Gesture::resize_box) {
            drag_world_delta_ = {(lx - start_dip_x_) / z, (ly - start_dip_y_) / z};
            if (event.kind == ui::EventKind::pointer_move) {
                out.cursor = ui::Cursor::scale;
                return out;
            }
            if (event.kind == ui::EventKind::pointer_up && event.button == gesture_button_) {
                const std::string target = active_annotation_;
                const std::array next_sz{std::max(160.f, std::round(annotation_origin_size_[0] + drag_world_delta_[0])),
                                         std::max(96.f, std::round(annotation_origin_size_[1] + drag_world_delta_[1]))};
                cancel_gesture();
                out.capture_release = true;
                out.cursor = ui::Cursor::arrow;
                ATELIER_TRY(session.graph_update_box(graph_key, target, {}, next_sz, {}, {}));
                out.status = L"Resized Network Box";
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
                    selected_note_.clear();
                    selected_box_.clear();
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
                selected_note_.clear();
                selected_box_.clear();
                if (!shift)
                    selected_nodes.clear();
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
            snapped_port_ =
                hit_port(lx, ly, std::clamp(30.f * z, 20.f, 40.f), !wire_anchor_.is_output, wire_anchor_.node);
            if (!snapped_port_) {
                for (auto it = view.nodes.rbegin(); it != view.nodes.rend(); ++it) {
                    if (it->name == wire_anchor_.node || it->placeholder)
                        continue;
                    const auto pos = displayed_position(*it);
                    const auto tl = to_dip(pos[0], pos[1]);
                    const bool is_circle = resolve_shape_id(*it, false) == 7;
                    const float vis_x = is_circle ? (tl[0] + (node_width - circle_width) * .5f * z) : tl[0];
                    const float vis_w = (is_circle ? circle_width : node_width) * z;
                    const float sh = node_height * z;
                    if (lx >= vis_x && lx <= vis_x + vis_w && ly >= tl[1] && ly <= tl[1] + sh) {
                        if (wire_anchor_.is_output && !it->ports.inputs.empty()) {
                            const bool multi = !it->ports.multiple.empty() && it->ports.multiple[0];
                            snapped_port_ = PortRef{it->name, it->ports.inputs[0], 0, false, multi};
                        } else if (!wire_anchor_.is_output && !it->ports.outputs.empty()) {
                            snapped_port_ = PortRef{it->name, it->ports.outputs[0], 0, true, false};
                        }
                        break;
                    }
                }
            }
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
                for (const auto& conn : view.connections) {
                    const auto* src = view.find_node(conn.from_node);
                    const auto* dst = view.find_node(conn.to_node);
                    if (!src || !dst)
                        continue;
                    if (wire_intersects_segment(port_dip(*src, true, conn.from_index),
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
        air_node_.clear();
        hovered_flag_ = FlagZone::none;
        hovered_air_ = AirButton::none;
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

    // Check Air-Ring hit on current air_node_ first when visible:
    AirButton air_hit = AirButton::none;
    if (!air_node_.empty() && z >= .55f && !tab_search_.open && !popup_.open()) {
        if (const auto* an = view.find_node(air_node_)) {
            const auto pos = displayed_position(*an);
            const auto tl = to_dip(pos[0], pos[1]);
            const float cx = tl[0] + node_width * .5f * z;
            const float cy = tl[1] + node_height * .5f * z;
            const auto sec = hit_air_sector(lx, ly, cx, cy, z);
            if (sec > 0)
                air_hit = static_cast<AirButton>(sec);
        } else {
            air_node_.clear();
        }
    }

    std::string node_under;
    FlagZone flag_under = FlagZone::none;
    bool body_interior = false;
    for (auto it = view.nodes.rbegin(); it != view.nodes.rend(); ++it) {
        const auto pos = displayed_position(*it);
        const auto tl = to_dip(pos[0], pos[1]);
        const bool is_circle = resolve_shape_id(*it, false) == 7;
        const float vis_x = is_circle ? (tl[0] + (node_width - circle_width) * .5f * z) : tl[0];
        const float vis_w = (is_circle ? circle_width : node_width) * z;
        const float sh = node_height * z;
        if (lx >= vis_x && lx <= vis_x + vis_w && ly >= tl[1] && ly <= tl[1] + sh) {
            node_under = it->name;
            const float bw_l = (is_circle ? 10.f : left_flag_width) * z;
            const float bw_d = (is_circle ? 10.f : flag_width) * z;
            const float bw_t = (is_circle ? 9.f : template_flag_width) * z;
            if (lx <= vis_x + bw_l)
                flag_under = FlagZone::bypass;
            else if (lx >= vis_x + vis_w - bw_d)
                flag_under = FlagZone::display;
            else if (lx >= vis_x + vis_w - bw_d - bw_t)
                flag_under = FlagZone::template_flag;
            body_interior = (flag_under != FlagZone::none) || (ly >= tl[1] + 2.f * z && ly <= tl[1] + sh - 2.f * z);
            break;
        }
    }

    const auto port_under = (body_interior || air_hit != AirButton::none)
                                ? std::optional<PortRef>{}
                                : hit_port(lx, ly, std::clamp(22.f * z, 14.f, 30.f));
    if (port_under) {
        node_under.clear();
        flag_under = FlagZone::none;
    }

    // Maintain wide hover Air-Ring envelope around active node:
    if (!node_under.empty()) {
        air_node_ = node_under;
    } else if (!air_node_.empty()) {
        const auto* an = view.find_node(air_node_);
        if (!an) {
            air_node_.clear();
        } else {
            const auto pos = displayed_position(*an);
            const auto tl = to_dip(pos[0], pos[1]);
            const float cx = tl[0] + node_width * .5f * z;
            const float cy = tl[1] + node_height * .5f * z;
            if (std::hypot(lx - cx, ly - cy) > (air_outer_radius + 6.f) * z)
                air_node_.clear();
        }
    }

    if (event.kind == ui::EventKind::pointer_move) {
        hovered_port_ = port_under;
        hovered_node_ = node_under;
        hovered_flag_ = flag_under;
        hovered_air_ = air_hit;
        out.consumed = true;
        out.cursor = (cut_key_held_ || port_under.has_value()) ? ui::Cursor::crosshair : ui::Cursor::arrow;
        return out;
    }

    if (event.kind != ui::EventKind::pointer_down)
        return out;

    out.consumed = true;
    if (tab_search_.open)
        close_tab_search();
    if (popup_.open() && !popup_.palette_floating)
        close_popup();

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

    // Air-Ring click handling:
    if (event.button == ui::PointerButton::primary && air_hit != AirButton::none && !air_node_.empty() &&
        !session.world().running()) {
        if (const auto* n = view.find_node(air_node_)) {
            const auto tl = to_dip(n->position[0], n->position[1]);
            const std::array anchor{tl[0] + (node_width + 22.f) * z, tl[1] - 12.f};
            if (air_hit == AirButton::bypass && !n->placeholder) {
                ATELIER_TRY(session.graph_set_bypass(graph_key, air_node_, !n->is_bypassed));
                out.status = !n->is_bypassed ? L"Bypass enabled: " + platform::utf8_to_utf16_validated(air_node_)
                                             : L"Bypass cleared: " + platform::utf8_to_utf16_validated(air_node_);
            } else if (air_hit == AirButton::display && !n->placeholder) {
                const bool make_disp = !n->is_display;
                ATELIER_TRY(session.graph_set_display(graph_key, make_disp ? std::string_view{air_node_} : ""));
                out.status = make_disp ? L"Display Flag: " + platform::utf8_to_utf16_validated(air_node_)
                                       : L"Cleared Display Flag";
            } else if (air_hit == AirButton::template_flag && !n->placeholder) {
                const bool make_tmpl = !n->is_template;
                ATELIER_TRY(session.graph_set_template(graph_key, make_tmpl ? std::string_view{air_node_} : ""));
                out.status = make_tmpl ? L"Template Flag: " + platform::utf8_to_utf16_validated(air_node_)
                                       : L"Cleared Template Flag";
            } else if (air_hit == AirButton::info) {
                selected_nodes = {air_node_};
                primary_selected = air_node_;
                open_node_popup(PopupKind::node_info, view, primary_selected, selected_nodes, canvas_dip, anchor);
            } else if (air_hit == AirButton::freeze && !n->placeholder) {
                ATELIER_TRY(session.graph_set_frozen(graph_key, air_node_, !n->is_frozen, project));
                out.status = !n->is_frozen ? L"Frozen SOP node: " + platform::utf8_to_utf16_validated(air_node_)
                                           : L"Unfrozen SOP node: " + platform::utf8_to_utf16_validated(air_node_);
            }
            return out;
        }
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
        if (event.button == ui::PointerButton::primary && flag_under != FlagZone::none && !session.world().running()) {
            const auto* n = view.find_node(node_under);
            if (n && !n->placeholder) {
                if (flag_under == FlagZone::display) {
                    const bool make_display = !n->is_display;
                    ATELIER_TRY(session.graph_set_display(graph_key, make_display ? std::string_view{node_under} : ""));
                    out.status = make_display ? L"Display Flag: " + platform::utf8_to_utf16_validated(node_under)
                                              : L"Cleared Display Flag";
                } else if (flag_under == FlagZone::template_flag) {
                    const bool make_tmpl = !n->is_template;
                    ATELIER_TRY(session.graph_set_template(graph_key, make_tmpl ? std::string_view{node_under} : ""));
                    out.status = make_tmpl ? L"Template Flag: " + platform::utf8_to_utf16_validated(node_under)
                                           : L"Cleared Template Flag";
                } else if (flag_under == FlagZone::bypass) {
                    ATELIER_TRY(session.graph_set_bypass(graph_key, node_under, !n->is_bypassed));
                    out.status = !n->is_bypassed ? L"Bypass enabled: " + platform::utf8_to_utf16_validated(node_under)
                                                 : L"Bypass cleared: " + platform::utf8_to_utf16_validated(node_under);
                }
                return out;
            }
        }
        if (event.button == ui::PointerButton::secondary) {
            selected_note_.clear();
            selected_box_.clear();
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
            drag_origins_.clear();
            drag_note_origins_.clear();
            drag_box_origins_.clear();
            const bool already_selected = std::ranges::find(selected_nodes, node_under) != selected_nodes.end();
            if (!event.shift && already_selected) {
                for (const auto& sel : selected_nodes) {
                    if (const auto* nv = view.find_node(sel))
                        drag_origins_.emplace_back(sel, nv->position);
                }
            } else if (const auto* nv = view.find_node(node_under)) {
                drag_origins_.emplace_back(node_under, nv->position);
            }
            drag_world_delta_ = {};
            gesture_ = Gesture::press_node;
            out.capture_acquire = true;
            return out;
        }
    }

    // Sticky Notes hit-test (above Network Boxes, below Nodes):
    if (event.button == ui::PointerButton::primary && !session.world().running()) {
        for (auto it = view.notes.rbegin(); it != view.notes.rend(); ++it) {
            const auto tl = to_dip(it->position[0], it->position[1]);
            const float nw = it->size[0] * z;
            const float nh = it->size[1] * z;
            if (lx >= tl[0] && lx <= tl[0] + nw && ly >= tl[1] && ly <= tl[1] + nh) {
                selected_note_ = it->name;
                selected_box_.clear();
                selected_nodes.clear();
                primary_selected.clear();
                active_annotation_ = it->name;
                annotation_origin_pos_ = it->position;
                annotation_origin_size_ = it->size;
                drag_note_origins_ = {{it->name, it->position}};
                drag_world_delta_ = {};
                const float corner = std::max(11.f, 14.f * z);
                gesture_ = (lx >= tl[0] + nw - corner && ly >= tl[1] + nh - corner) ? Gesture::resize_note
                                                                                    : Gesture::move_note;
                out.capture_acquire = true;
                return out;
            }
        }

        // Network Boxes hit-test (header bar drags box + enclosed nodes/notes; bottom-right corner resizes):
        for (auto it = view.boxes.rbegin(); it != view.boxes.rend(); ++it) {
            const auto tl = to_dip(it->position[0], it->position[1]);
            const float bw = it->size[0] * z;
            const float bh = it->size[1] * z;
            if (lx < tl[0] || lx > tl[0] + bw || ly < tl[1] || ly > tl[1] + bh)
                continue;
            const float corner = std::max(12.f, 15.f * z);
            const float header_h = std::max(18.f, 24.f * z);
            const bool in_resize = lx >= tl[0] + bw - corner && ly >= tl[1] + bh - corner;
            const bool in_header = ly <= tl[1] + header_h;
            if (!in_resize && !in_header)
                continue;
            selected_box_ = it->name;
            selected_note_.clear();
            active_annotation_ = it->name;
            annotation_origin_pos_ = it->position;
            annotation_origin_size_ = it->size;
            drag_origins_.clear();
            drag_note_origins_.clear();
            drag_box_origins_ = {{it->name, it->position}};
            if (!in_resize) {
                const float bx0 = it->position[0];
                const float by0 = it->position[1];
                const float bx1 = bx0 + it->size[0];
                const float by1 = by0 + it->size[1];
                for (const auto& n : view.nodes) {
                    const float ncx = n.position[0] + node_width * .5f;
                    const float ncy = n.position[1] + node_height * .5f;
                    if (ncx >= bx0 && ncx <= bx1 && ncy >= by0 && ncy <= by1)
                        drag_origins_.emplace_back(n.name, n.position);
                }
                for (const auto& note : view.notes) {
                    const float ncx = note.position[0] + note.size[0] * .5f;
                    const float ncy = note.position[1] + note.size[1] * .5f;
                    if (ncx >= bx0 && ncx <= bx1 && ncy >= by0 && ncy <= by1)
                        drag_note_origins_.emplace_back(note.name, note.position);
                }
            }
            drag_world_delta_ = {};
            gesture_ = in_resize ? Gesture::resize_box : Gesture::move_box;
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

    if (event.key == ui::Key::escape && popup_.open()) {
        close_popup();
        out.consumed = true;
        return out;
    }

    if (active_gesture()) {
        if (event.key == ui::Key::enter && gesture_ == Gesture::move_nodes && keyboard_grab_) {
            std::vector<std::pair<std::string, std::array<float, 2>>> moves;
            moves.reserve(drag_origins_.size());
            for (const auto& [name, origin] : drag_origins_)
                moves.emplace_back(name, std::array{std::round(origin[0] + drag_world_delta_[0]),
                                                    std::round(origin[1] + drag_world_delta_[1])});
            const auto splice = splice_wire_;
            const bool amend = amend_duplicate_;
            cancel_gesture();
            out.consumed = true;
            out.capture_release = true;
            out.cursor = ui::Cursor::arrow;
            if (splice && moves.size() == 1 &&
                session.graph_splice_node(graph_key, moves.front().first, moves.front().second, *splice)) {
                out.status = L"Spliced '" + platform::utf8_to_utf16_validated(moves.front().first) + L"' into wire";
                return out;
            }
            ATELIER_TRY(session.graph_move_nodes(graph_key, moves, amend));
            out.status = amend ? L"Placed duplicated SOP node(s)" : L"Moved SOP node(s)";
            return out;
        }
        if (event.key == ui::Key::escape) {
            cancel_gesture();
            out.consumed = true;
            out.capture_release = true;
            out.cursor = ui::Cursor::arrow;
            out.status = L"Cancelled Network gesture";
            return out;
        }
        out.consumed = true;
        return out;
    }

    const auto inspected_res = session.graph_topology(graph_key);
    if (!inspected_res)
        return std::unexpected(inspected_res.error());
    const auto& view = **inspected_res;

    const std::string target_node = !hovered_node_.empty() ? hovered_node_
                                    : !air_node_.empty()   ? air_node_
                                                           : primary_selected;
    const auto resolve_targets = [&]() {
        std::vector<std::string> targets = selected_nodes;
        if (targets.empty() && !target_node.empty())
            targets.push_back(target_node);
        return targets;
    };
    const auto begin_keyboard_grab = [&](const GraphInspectView& gview, bool amend_dup) {
        drag_origins_.clear();
        drag_note_origins_.clear();
        drag_box_origins_.clear();
        for (const auto& sel : selected_nodes) {
            if (const auto* nv = gview.find_node(sel))
                drag_origins_.emplace_back(sel, nv->position);
        }
        start_dip_x_ =
            (current_dip_x_ > 0.f && current_dip_x_ < canvas_dip.width) ? current_dip_x_ : canvas_dip.width * .5f;
        start_dip_y_ =
            (current_dip_y_ > 0.f && current_dip_y_ < canvas_dip.height) ? current_dip_y_ : canvas_dip.height * .5f;
        drag_world_delta_ = {};
        splice_wire_.reset();
        gesture_ = Gesture::move_nodes;
        keyboard_grab_ = true;
        amend_duplicate_ = amend_dup;
        out.consumed = true;
        out.capture_acquire = true;
        out.cursor = ui::Cursor::move;
    };
    const auto open_add_menu = [&]() -> InteractionOutcome {
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
    };

    if (event.control && !event.alt && !event.shift) {
        if (!event.repeat && native_key == 'D' && !session.world().running()) {
            const auto targets = resolve_targets();
            if (!targets.empty()) {
                ATELIER_TRY_BIND(created, session.graph_duplicate_nodes(graph_key, targets, {36.f, 36.f}));
                selected_note_.clear();
                selected_box_.clear();
                selected_nodes = *created;
                primary_selected = created->empty() ? std::string{} : created->back();
                out.consumed = true;
                out.status = std::format(L"Duplicated {} SOP node(s)", created->size());
            }
            return out;
        }
        if (!event.repeat && (native_key == 'A' || event.key == ui::Key::a)) {
            selected_nodes.clear();
            for (const auto& n : view.nodes)
                selected_nodes.push_back(n.name);
            primary_selected = selected_nodes.empty() ? std::string{} : selected_nodes.back();
            out.consumed = true;
            return out;
        }
        if (!event.repeat && native_key == 'I') {
            std::vector<std::string> inverted;
            for (const auto& n : view.nodes) {
                if (std::ranges::find(selected_nodes, n.name) == selected_nodes.end())
                    inverted.push_back(n.name);
            }
            selected_nodes = std::move(inverted);
            primary_selected = selected_nodes.empty() ? std::string{} : selected_nodes.back();
            out.consumed = true;
            out.status = L"Inverted SOP selection";
            return out;
        }
        return out;
    }

    if (event.alt && !event.control && !event.shift) {
        if (!event.repeat && (native_key == 'A' || event.key == ui::Key::a)) {
            selected_nodes.clear();
            primary_selected.clear();
            selected_note_.clear();
            selected_box_.clear();
            out.consumed = true;
            out.status = L"Cleared SOP selection";
            return out;
        }
        return out;
    }

    if (event.control || event.alt)
        return out;

    if (event.shift) {
        if (!event.repeat && native_key == 'D' && !session.world().running()) {
            const auto targets = resolve_targets();
            if (!targets.empty()) {
                ATELIER_TRY_BIND(created, session.graph_duplicate_nodes(graph_key, targets, {28.f, 28.f}));
                selected_note_.clear();
                selected_box_.clear();
                selected_nodes = *created;
                primary_selected = created->empty() ? std::string{} : created->back();
                if (const auto after = session.graph_topology(graph_key))
                    begin_keyboard_grab(**after, true);
                out.status =
                    std::format(L"Duplicated {} SOP node(s) · Move cursor, LMB/Enter confirm, Esc/RMB keep offset",
                                created->size());
            }
            return out;
        }
        if (!event.repeat && (native_key == 'A' || event.key == ui::Key::a) && !session.world().running())
            return open_add_menu();
        return out;
    }

    if (native_key == 'Y' || event.key == ui::Key::y) {
        cut_key_held_ = true;
        out.consumed = true;
        out.cursor = ui::Cursor::crosshair;
        out.status = L"Wire Cut: hold Y and drag across wires to disconnect";
        return out;
    }

    if (event.repeat)
        return out;

    if (event.key == ui::Key::tab && !session.world().running())
        return open_add_menu();

    if (native_key == 'H' || event.key == ui::Key::home) {
        frame_nodes(view.nodes, canvas_dip, {}, view.boxes, view.notes);
        out.consumed = true;
        out.status = L"Framed all SOP nodes";
        return out;
    }

    if (native_key == 'F' || native_key == 0x6E /* VK_DECIMAL */) {
        frame_nodes(view.nodes, canvas_dip, selected_nodes, view.boxes, view.notes);
        out.consumed = true;
        out.status = selected_nodes.empty() ? L"Framed SOP network" : L"Framed SOP selection";
        return out;
    }

    if (native_key == 'G' && !session.world().running()) {
        if (selected_nodes.empty() && !target_node.empty()) {
            selected_nodes = {target_node};
            primary_selected = target_node;
        }
        if (!selected_nodes.empty()) {
            begin_keyboard_grab(view, false);
            out.status = L"Move SOP node(s) · Move cursor, LMB/Enter confirm, Esc/RMB cancel";
        }
        return out;
    }

    if (native_key == 'L' && !session.world().running()) {
        ATELIER_TRY(session.graph_auto_layout(graph_key));
        if (const auto after = session.graph_topology(graph_key))
            frame_nodes((*after)->nodes, canvas_dip, {}, (*after)->boxes, (*after)->notes);
        out.consumed = true;
        out.status = L"Auto-arranged SOP network";
        return out;
    }

    if (native_key == 'D' && !session.world().running() && !target_node.empty()) {
        if (const auto* n = view.find_node(target_node); n && !n->placeholder) {
            ATELIER_TRY(session.graph_set_display(graph_key, n->is_display ? std::string_view{} : target_node));
            out.consumed = true;
            out.status = n->is_display ? L"Cleared Display Flag"
                                       : L"Display Flag: " + platform::utf8_to_utf16_validated(target_node);
            return out;
        }
    }

    if (native_key == 'T' && !session.world().running() && !target_node.empty()) {
        if (const auto* n = view.find_node(target_node); n && !n->placeholder) {
            ATELIER_TRY(session.graph_set_template(graph_key, n->is_template ? std::string_view{} : target_node));
            out.consumed = true;
            out.status = n->is_template ? L"Cleared Template Flag"
                                        : L"Template Flag: " + platform::utf8_to_utf16_validated(target_node);
            return out;
        }
    }

    if (native_key == 'B' && !session.world().running() && !target_node.empty()) {
        if (const auto* n = view.find_node(target_node); n && !n->placeholder) {
            ATELIER_TRY(session.graph_set_bypass(graph_key, target_node, !n->is_bypassed));
            out.consumed = true;
            out.status = !n->is_bypassed ? L"Bypass enabled: " + platform::utf8_to_utf16_validated(target_node)
                                         : L"Bypass cleared: " + platform::utf8_to_utf16_validated(target_node);
            return out;
        }
    }

    if (native_key == 'Z' && !session.world().running()) {
        if (popup_.open() && popup_.kind == PopupKind::shape_picker)
            close_popup();
        else
            open_node_popup(PopupKind::shape_picker, view, target_node, selected_nodes, canvas_dip);
        out.consumed = true;
        return out;
    }

    if (native_key == 'C' && !session.world().running()) {
        if (popup_.open() && popup_.kind == PopupKind::color_picker)
            close_popup();
        else
            open_node_popup(PopupKind::color_picker, view, target_node, selected_nodes, canvas_dip);
        out.consumed = true;
        return out;
    }

    if (native_key == 'I' && !target_node.empty()) {
        open_node_popup(PopupKind::node_info, view, target_node, selected_nodes, canvas_dip);
        out.consumed = true;
        return out;
    }

    if (native_key == 'N' && !session.world().running()) {
        float tx = current_dip_x_ > 0.f ? current_dip_x_ : canvas_dip.width * .5f;
        float ty = current_dip_y_ > 0.f ? current_dip_y_ : canvas_dip.height * .5f;
        const auto wp = to_world(tx - 96.f, ty - 48.f);
        ATELIER_TRY_BIND(created, session.graph_add_note(graph_key, {std::round(wp[0]), std::round(wp[1])},
                                                         {216.f, 116.f}, "Note", "Double-click header to edit."));
        selected_note_ = *created;
        selected_box_.clear();
        out.consumed = true;
        out.status = L"Added Sticky Note: " + platform::utf8_to_utf16_validated(*created);
        return out;
    }

    if (native_key == 'O' && !session.world().running()) {
        std::wstring status_msg;
        ui::Action act{ui::ActionKind::activate, network_control::add_network_box, {}};
        ATELIER_TRY(apply_action(session, graph_key, primary_selected, selected_nodes, canvas_dip, act, status_msg));
        out.consumed = true;
        out.status = std::move(status_msg);
        return out;
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

    if ((event.key == ui::Key::delete_key || event.key == ui::Key::backspace || native_key == 'X' ||
         event.key == ui::Key::x) &&
        !session.world().running()) {
        if (!selected_note_.empty()) {
            const std::array names{selected_note_};
            ATELIER_TRY(session.graph_remove_notes(graph_key, names));
            selected_note_.clear();
            out.consumed = true;
            out.status = L"Deleted Sticky Note";
            return out;
        }
        if (!selected_box_.empty()) {
            const std::array names{selected_box_};
            ATELIER_TRY(session.graph_remove_boxes(graph_key, names));
            selected_box_.clear();
            out.consumed = true;
            out.status = L"Deleted Network Box";
            return out;
        }
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

void NetworkCanvas::append_tab_categories(std::vector<ui::Control>& controls, ui::ControlKey tab_box_key) const {
    if (!tab_search_.open)
        return;
    const auto search_it = std::ranges::find(controls, network_control::tab_search_input, &ui::Control::key);
    if (search_it == controls.end())
        return;

    std::vector<ui::Control> cat_controls;
    ui::Control row;
    row.key = network_control::tab_cat_row;
    row.parent = tab_box_key;
    row.kind = ui::ControlKind::container;
    row.layout = ui_layout::NodeInput::box(0.f, 21.f);
    row.layout.kind = ui_layout::Kind::row;
    row.layout.width = ui_layout::Axis::expand();
    row.layout.gap = 2.f;
    row.style.background = ui::Color{0, 0, 0, 0};
    row.style.border_width = 0.f;
    cat_controls.push_back(std::move(row));

    constexpr std::array<const wchar_t*, 6> labels{L"All", L"Prim", L"Attr", L"Scat", L"Util", L"Src"};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const bool active = static_cast<std::size_t>(tab_search_.category) == i;
        ui::Control btn;
        btn.key = network_control::tab_cat_first + i;
        btn.parent = network_control::tab_cat_row;
        btn.kind = ui::ControlKind::button;
        btn.text = labels[i];
        btn.single_line = true;
        btn.selected = active;
        btn.classes = {"ghost-button"};
        btn.layout = ui_layout::NodeInput::box(0.f, 20.f);
        btn.layout.width = ui_layout::Axis::expand();
        btn.style.font_size = 10.5f;
        btn.style.font_weight = active ? std::uint16_t{700} : std::uint16_t{400};
        btn.style.radius = {3.f, 3.f, 3.f, 3.f};
        cat_controls.push_back(std::move(btn));
    }

    controls.insert(search_it + 1, std::make_move_iterator(cat_controls.begin()),
                    std::make_move_iterator(cat_controls.end()));
}

std::array<float, 2> NetworkCanvas::prepare_wire_routes(const std::shared_ptr<const GraphInspectView>& topology,
                                                        ui_layout::Size viewport) {
    const float z = zoom();
    const std::array offset{pan_x_ - wire_routes_.origin[0], pan_y_ - wire_routes_.origin[1]};
    const auto& region = wire_routes_.region;
    const bool moving_ports = gesture_ == Gesture::move_nodes || gesture_ == Gesture::move_box;
    const bool same_topology = wire_routes_.topology.lock() == topology;
    // The cut commit tests the full graph, including off-screen wires, in the current pointer coordinate system.
    const bool reanchor_cut = gesture_ == Gesture::wire_cut && (offset[0] != 0.f || offset[1] != 0.f);
    if (!moving_ports && !reanchor_cut && same_topology && wire_routes_.zoom == z && -offset[0] >= region.x &&
        -offset[1] >= region.y && viewport.width - offset[0] <= region.x + region.width &&
        viewport.height - offset[1] <= region.y + region.height)
        return offset;

    // Sample in viewport-local DIP to retain precision far from the graph origin. Always translate from this
    // anchor, never from last frame's projected points. Routes entering beyond the margin rebuild the working set.
    if (!same_topology) {
        wire_routes_ = {};
        wire_routes_.topology = topology;
    } else {
        wire_routes_.wires.clear();
        wire_routes_.segments.clear();
    }
    wire_routes_.origin = pan();
    // Drag previews depend on live port positions and cannot be reused after the gesture ends.
    wire_routes_.zoom = moving_ports ? 0.f : z;
    wire_routes_.region = {-viewport.width * .25f, -viewport.height * .25f, viewport.width * 1.5f,
                           viewport.height * 1.5f};
    for (std::size_t i = 0; i < topology->connections.size(); ++i) {
        const auto& connection = topology->connections[i];
        const auto* src = topology->find_node(connection.from_node);
        const auto* dst = topology->find_node(connection.to_node);
        if (!src || !dst)
            continue;
        const auto from = port_dip(*src, true, connection.from_index);
        const auto to = port_dip(*dst, false, connection.to_index);
        if (!wire_near_region(from, to, z, wire_routes_.region))
            continue;
        for_each_wire_segment(from, to, z, [&](auto a, auto b) {
            wire_routes_.segments.push_back({a, b});
            return false;
        });
        wire_routes_.wires.push_back({i, wire_routes_.segments.size(), src->status && src->status->error.has_value()});
    }
    return {};
}

void NetworkCanvas::append_canvas(ui_language::ControlOutput output, ui::ControlKey canvas_parent,
                                  ui_layout::Size canvas_dip, const std::shared_ptr<const GraphInspectView>& topology,
                                  std::span<const GraphNodeCatalogEntry> catalog, std::string_view primary_selected,
                                  std::span<const std::string> selected_nodes) {
    const auto& view = *topology;
    const auto& graph_key = view.key;
    sync_graph(graph_key, view.nodes, canvas_dip, view.boxes, view.notes);
    const float z = zoom();
    const float cw = std::max(64.f, canvas_dip.width > 32.f ? canvas_dip.width : last_canvas_dip_.width);
    const float ch = std::max(64.f, canvas_dip.height > 32.f ? canvas_dip.height : last_canvas_dip_.height);

    // Active gestures can move nodes and reroute wires. Popups are appended separately from retained graph content.
    const bool settled = !active_gesture() && !detached_wire_ && !splice_wire_ && !snapped_port_;
    std::optional<IdleCanvasState> idle;
    if (settled) {
        idle.emplace(IdleCanvasState{canvas_parent,
                                     {cw, ch},
                                     pan(),
                                     z,
                                     std::string(primary_selected),
                                     selected_note_,
                                     selected_box_,
                                     hovered_node_,
                                     air_node_,
                                     hovered_port_,
                                     hovered_flag_,
                                     hovered_air_,
                                     tab_search_.open || popup_.open()});
        if (idle_projection_ && idle_projection_->topology.lock() == topology && idle_projection_->state == *idle &&
            std::ranges::equal(selected_nodes, idle_projection_->selection) &&
            std::ranges::equal(catalog, idle_projection_->catalog)) {
            if (!popup_.open()) {
                output.append(idle_projection_->controls, idle_projection_->revision);
                return;
            }
            std::vector<ui::Control> controls = idle_projection_->controls;
            append_canvas_popup(controls, canvas_parent, cw, ch, view, catalog);
            output.append(controls, 0);
            return;
        }
    }
    idle_projection_.reset();
    std::vector<ui::Control> controls;

    CanvasLayer backdrop;
    auto& grid = *backdrop.batches.front();
    grid.grid_step = 32.f * z;
    grid.grid_origin_x = pan_x_;
    grid.grid_origin_y = pan_y_;
    grid.grid_minor = hex_color(0x2d2e30);
    grid.grid_major = hex_color(0x383a3e);
    grid.grid_axis = hex_color(0x4a525c);
    CanvasLayer bodies;
    CanvasLayer overlay;
    decltype(visible_tooltips_) tooltips;

    const auto intern_tooltip = [&](ui::ControlKey key, ui::Tooltip help) {
        const auto previous = visible_tooltips_.find(key);
        auto ptr = previous != visible_tooltips_.end() && *previous->second == help
                       ? previous->second
                       : std::make_shared<const ui::Tooltip>(std::move(help));
        tooltips.emplace(key, ptr);
        return ptr;
    };

    const auto first_label = controls.size();

    // 1. Network Boxes in backdrop (drawn beneath wires and nodes):
    for (const auto& box : view.boxes) {
        const auto pos = displayed_box_position(box);
        const auto sz = displayed_box_size(box);
        const auto tl = to_dip(pos[0], pos[1]);
        const float bx = tl[0];
        const float by = tl[1];
        const float bw = sz[0] * z;
        const float bh = sz[1] * z;
        if (bx + bw < -16.f || bx > cw + 16.f || by + bh < -16.f || by > ch + 16.f)
            continue;
        const bool is_sel = box.name == selected_box_;
        const float r = std::clamp(6.f * z, 2.f, 10.f);
        const float header_h = std::clamp(22.f * z, 12.f, 34.f);
        backdrop.box({{bx, by, bw, bh},
                      hex_color(box.color, .13f),
                      {r, r, r, r},
                      is_sel ? 1.8f : 1.15f,
                      is_sel ? hex_color(0xffd900, .92f) : hex_color(box.color, .58f)});
        backdrop.box({{bx, by, bw, std::min(bh, header_h)}, hex_color(box.color, .40f), {r, r, 0.f, 0.f}, 0.f, {}});
        // Resize corner grip:
        const float g = std::clamp(8.f * z, 5.f, 12.f);
        backdrop.stroke({bx + bw - g, by + bh - 2.f, bx + bw - 2.f, by + bh - g, 1.2f, hex_color(box.color, .75f)});

        if (z >= .45f) {
            const ui::ControlKey bkey = node_base_key(graph_key, "box:" + box.name);
            ui::Control title_ctrl;
            title_ctrl.key = bkey;
            title_ctrl.parent = canvas_parent;
            title_ctrl.kind = ui::ControlKind::label;
            title_ctrl.enabled = false;
            title_ctrl.single_line = true;
            title_ctrl.text =
                sanitize_single_line(platform::utf8_to_utf16_validated(box.title.empty() ? box.name : box.title));
            title_ctrl.layout = out_of_flow_box(bx + 8.f * z, by, std::max(8.f, bw - 16.f * z), header_h);
            title_ctrl.style.background = ui::Color{0, 0, 0, 0};
            title_ctrl.style.border_width = 0.f;
            title_ctrl.style.text_vertical_align = ui::TextVerticalAlign::center;
            title_ctrl.style.font_size = std::clamp(std::round(11.5f * z), 7.f, 22.f);
            title_ctrl.style.font_weight = 700;
            title_ctrl.style.foreground = hex_color(0xf1f5f9);
            controls.push_back(std::move(title_ctrl));
        }
    }

    // 2. Existing Bezier wires in backdrop:
    const auto offset = prepare_wire_routes(topology, {cw, ch});
    const auto translate = [&](std::array<float, 2> p) { return std::array{p[0] + offset[0], p[1] + offset[1]}; };
    std::size_t begin = 0;
    for (const auto& route : wire_routes_.wires) {
        const auto& conn = view.connections[route.connection];
        const auto segments = std::span{wire_routes_.segments}.subspan(begin, route.end - begin);
        begin = route.end;
        if (detached_wire_ && conn.from_node == detached_wire_->from_node &&
            conn.from_port == detached_wire_->from_port && conn.to_node == detached_wire_->to_node &&
            conn.to_port == detached_wire_->to_port)
            continue;

        const bool splice_hit = splice_wire_ && conn.from_node == splice_wire_->from_node &&
                                conn.from_port == splice_wire_->from_port && conn.to_node == splice_wire_->to_node &&
                                conn.to_port == splice_wire_->to_port;
        const bool cut_hit =
            gesture_ == Gesture::wire_cut && std::ranges::any_of(segments, [&](const auto& s) {
                return segments_intersect({start_dip_x_, start_dip_y_}, {current_dip_x_, current_dip_y_},
                                          translate(s.from), translate(s.to));
            });
        const bool active_touch = conn.from_node == primary_selected || conn.to_node == primary_selected;
        const ui::Color wire_col = splice_hit             ? hex_color(0xffd900)
                                   : cut_hit              ? hex_color(0xff5252)
                                   : route.upstream_error ? hex_color(0xd9534f)
                                   : active_touch         ? hex_color(0xf0c43e)
                                                          : hex_color(0x94a6b8);
        const float wire_w = splice_hit ? std::clamp(2.2f * std::sqrt(z), 1.6f, 2.8f)
                                        : (cut_hit || active_touch ? std::clamp(1.45f * std::sqrt(z), 1.1f, 1.85f)
                                                                   : std::clamp(1.05f * std::sqrt(z), 0.85f, 1.35f));
        for (const auto& segment : segments)
            append_wire_segment(backdrop, translate(segment.from), translate(segment.to), wire_w, wire_col, {cw, ch});
    }

    // 3. Sticky Notes in bodies:
    for (const auto& note : view.notes) {
        const auto pos = displayed_note_position(note);
        const auto sz = displayed_note_size(note);
        const auto tl = to_dip(pos[0], pos[1]);
        const float nx = tl[0];
        const float ny = tl[1];
        const float nw = sz[0] * z;
        const float nh = sz[1] * z;
        if (nx + nw < -16.f || nx > cw + 16.f || ny + nh < -16.f || ny > ch + 16.f)
            continue;
        const bool is_sel = note.name == selected_note_;
        const float r = std::clamp(4.f * z, 2.f, 8.f);
        const float header_h = std::clamp(20.f * z, 11.f, 30.f);
        const std::uint32_t paper_rgb = blend_hex(0x23211d, note.color, .22f);
        bodies.box({{nx + 2.f * z, ny + 3.f * z, nw, nh}, {0.f, 0.f, 0.f, .32f}, {r, r, r, r}, 0.f, {}});
        bodies.box({{nx, ny, nw, nh},
                    hex_color(paper_rgb, .95f),
                    {r, r, r, r},
                    is_sel ? 1.8f : 1.1f,
                    is_sel ? hex_color(0xffd900) : hex_color(note.color, .75f)});
        bodies.box({{nx, ny, nw, std::min(nh, header_h)}, hex_color(note.color, .52f), {r, r, 0.f, 0.f}, 0.f, {}});

        if (z >= .45f) {
            const ui::ControlKey nkey = node_base_key(graph_key, "note:" + note.name);
            ui::Control h_ctrl;
            h_ctrl.key = nkey;
            h_ctrl.parent = canvas_parent;
            h_ctrl.kind = ui::ControlKind::label;
            h_ctrl.enabled = false;
            h_ctrl.single_line = true;
            h_ctrl.text =
                sanitize_single_line(platform::utf8_to_utf16_validated(note.title.empty() ? note.name : note.title));
            h_ctrl.layout = out_of_flow_box(nx + 7.f * z, ny, std::max(8.f, nw - 14.f * z), header_h);
            h_ctrl.style.background = ui::Color{0, 0, 0, 0};
            h_ctrl.style.border_width = 0.f;
            h_ctrl.style.text_vertical_align = ui::TextVerticalAlign::center;
            h_ctrl.style.font_size = std::clamp(std::round(11.f * z), 6.5f, 20.f);
            h_ctrl.style.font_weight = 700;
            h_ctrl.style.foreground = hex_color(0xfffbeb);
            controls.push_back(std::move(h_ctrl));

            if (nh > header_h + 10.f && !note.text.empty()) {
                ui::Control body_ctrl;
                body_ctrl.key = nkey + 1;
                body_ctrl.parent = canvas_parent;
                body_ctrl.kind = ui::ControlKind::label;
                body_ctrl.enabled = false;
                body_ctrl.single_line = false;
                body_ctrl.text = sanitize_multiline_label(platform::utf8_to_utf16_validated(note.text));
                body_ctrl.layout = out_of_flow_box(nx + 7.f * z, ny + header_h + 4.f * z, std::max(8.f, nw - 14.f * z),
                                                   std::max(8.f, nh - header_h - 8.f * z));
                body_ctrl.style.background = ui::Color{0, 0, 0, 0};
                body_ctrl.style.border_width = 0.f;
                body_ctrl.style.font_size = std::clamp(std::round(10.5f * z), 6.f, 19.f);
                body_ctrl.style.foreground = hex_color(0xe5e0d5);
                controls.push_back(std::move(body_ctrl));
            }
        }
    }

    // 4. Compact Houdini-inspired Analytic SDF Nodes, Concentric Halos, Contour Flag Strips & Wide Air-Ring:
    for (const auto& node : view.nodes) {
        const auto pos = displayed_position(node);
        const auto tl = to_dip(pos[0], pos[1]);
        const float sx = tl[0];
        const float sy = tl[1];
        const float sw = node_width * z;
        const float sh = node_height * z;
        const float cx = sx + sw * .5f;
        const float cy = sy + sh * .5f;
        const bool visible_on_canvas =
            sx + sw + 170.f * z >= 0.f && sx - 80.f * z <= cw && sy + sh + 80.f * z >= 0.f && sy - 80.f * z <= ch;
        if (!visible_on_canvas)
            continue;

        const bool is_primary = node.name == primary_selected;
        const bool is_selected = is_primary || std::ranges::find(selected_nodes, node.name) != selected_nodes.end();
        const bool is_hovered = node.name == hovered_node_;
        const bool has_error = node.placeholder || (node.status && node.status->error.has_value());
        const auto cat_it = std::ranges::find(catalog, node.type, &GraphNodeCatalogEntry::name);
        const bool is_source_sop = cat_it != catalog.end() && !cat_it->native;
        const std::uint8_t shape_id = resolve_shape_id(node, is_source_sop);
        const bool is_circle = shape_id == 7;
        const float vis_x = is_circle ? (sx + (node_width - circle_width) * .5f * z) : sx;
        const float vis_w = (is_circle ? circle_width : node_width) * z;
        const float bw_l = (is_circle ? 10.f : left_flag_width) * z;
        const float bw_t = (is_circle ? 9.f : template_flag_width) * z;
        const float bw_d = (is_circle ? 10.f : flag_width) * z;
        const float body_alpha = node.is_bypassed ? .62f : 1.f;

        // Subtle ambient shadow box beneath node silhouette:
        const float sh_r = sh * .45f;
        bodies.box(
            {{vis_x + 2.f * z, sy + 2.5f * z, vis_w, sh}, {0.f, 0.f, 0.f, .24f}, {sh_r, sh_r, sh_r, sh_r}, 0.f, {}});

        // Concentric background halos for Display (outer sky-blue) & Template (inner magenta):
        if (node.is_display) {
            bodies.arc({cx, cy, 22.f * z, 30.f * z, 1.f, 0.f, -2.f, 0.f, hex_color(0x0ea5e9, .24f), 1.f,
                        hex_color(0x38bdf8, .55f)});
        }
        if (node.is_template) {
            bodies.arc({cx, cy, 16.f * z, 22.f * z, 1.f, 0.f, -2.f, 0.f, hex_color(0xd946ef, .26f), 1.f,
                        hex_color(0xe879f9, .58f)});
        }

        // Calibrated studio dark-UI body fill:
        std::uint32_t base_rgb = 0x3a3e45;
        if (has_error)
            base_rgb = 0x522528;
        else if (node.color)
            base_rgb = blend_hex(0x282b30, *node.color, .40f);
        else if (node.type == "procedural.Null")
            base_rgb = 0x484228;
        else if (is_source_sop)
            base_rgb = 0x284957;
        else if (shape_id == 2)
            base_rgb = 0x384352;
        else if (shape_id == 3)
            base_rgb = 0x3f3954;
        else if (shape_id == 5)
            base_rgb = 0x324b43;
        if (node.is_frozen)
            base_rgb = blend_hex(base_rgb, 0x38bdf8, .18f);
        if (is_hovered)
            base_rgb = blend_hex(base_rgb, 0xffffff, .08f);

        const ui::Rect shape_bounds{vis_x, sy, vis_w, sh};
        bodies.shape({shape_bounds, shape_id, 0, 0.f, 0.f, hex_color(base_rgb, body_alpha), 0.f, {}});

        // Contour-matched vertical flag strips (Bypass = Left, Freeze = Left Inner when frozen, Template = Right Inner, Display = Right Outer):
        const ui::Color bypass_fill = node.is_bypassed                                    ? hex_color(0xeab308, .95f)
                                      : (is_hovered && hovered_flag_ == FlagZone::bypass) ? hex_color(0x6e5b1d, .85f)
                                                                                          : hex_color(0xffffff, .03f);
        bodies.shape({shape_bounds, shape_id, 1, 0.f, bw_l, bypass_fill, 0.f, {}});
        if (node.is_frozen) {
            const float bw_f = (is_circle ? 6.f : 7.5f) * z;
            bodies.shape({shape_bounds, shape_id, 1, bw_l, bw_l + bw_f, hex_color(0x38bdf8, .78f), 0.f, {}});
            bodies.shape(
                {shape_bounds, shape_id, 3, bw_l + bw_f, bw_l + bw_f, {0, 0, 0, 0}, 1.f, hex_color(0x131518, .62f)});
        }

        const ui::Color tmpl_fill = node.is_template ? hex_color(0xd946ef, .92f)
                                    : (is_hovered && hovered_flag_ == FlagZone::template_flag)
                                        ? hex_color(0x632d70, .85f)
                                        : hex_color(0x000000, .08f);
        bodies.shape({shape_bounds, shape_id, 2, bw_d, bw_d + bw_t, tmpl_fill, 0.f, {}});

        const ui::Color disp_fill = node.is_display                                      ? hex_color(0x0ea5e9, .95f)
                                    : (is_hovered && hovered_flag_ == FlagZone::display) ? hex_color(0x215370, .88f)
                                                                                         : hex_color(0x000000, .15f);
        bodies.shape({shape_bounds, shape_id, 2, 0.f, bw_d, disp_fill, 0.f, {}});

        // Contour-parallel hairline dividers:
        const ui::Color seam_col = hex_color(0x131518, .62f);
        bodies.shape({shape_bounds, shape_id, 3, bw_l, bw_l, {0, 0, 0, 0}, 1.f, seam_col});
        bodies.shape({shape_bounds, shape_id, 4, bw_d + bw_t, bw_d + bw_t, {0, 0, 0, 0}, 1.f, seam_col});
        bodies.shape({shape_bounds, shape_id, 4, bw_d, bw_d, {0, 0, 0, 0}, 1.f, seam_col});

        // Outer silhouette border / selection gold ring:
        const ui::Color outline = is_primary         ? hex_color(0xffd900)
                                  : is_selected      ? hex_color(0xd4af37)
                                  : has_error        ? hex_color(0xef4444)
                                  : node.is_frozen   ? hex_color(0x38bdf8, .92f)
                                  : node.is_bypassed ? hex_color(0xca8a04, .85f)
                                                     : hex_color(0x16181b);
        const float border_w =
            (is_selected || has_error || node.is_frozen) ? std::max(1.45f, 1.85f * std::sqrt(z)) : 1.1f;
        bodies.shape({shape_bounds, shape_id, 0, 0.f, 0.f, {0, 0, 0, 0}, border_w, outline});

        if (node.is_bypassed) {
            bodies.stroke({cx - 7.f * z, cy + 6.f * z, cx + 7.f * z, cy - 6.f * z, std::max(1.2f, 1.6f * z),
                           hex_color(0xfacc15, .90f)});
        }

        // Detached Houdini-style Input & Output pin dots in overlay:
        for (std::size_t p = 0; p < node.ports.inputs.size(); ++p) {
            const auto pt = port_dip(node, false, p);
            const bool multi = p < node.ports.multiple.size() && node.ports.multiple[p];
            const bool hot = (hovered_port_ && !hovered_port_->is_output && hovered_port_->node == node.name &&
                              hovered_port_->index == p) ||
                             (snapped_port_ && !snapped_port_->is_output && snapped_port_->node == node.name &&
                              snapped_port_->index == p);
            const float base_d = std::clamp(6.4f * z, 3.6f, 11.5f);
            const float pw = (multi ? base_d * 2.1f : base_d) + (hot ? 1.8f * z : 0.f);
            const float ph = (multi ? base_d * .9f : base_d) + (hot ? 1.8f * z : 0.f);
            const float pr = ph * .5f;
            if (hot) {
                const float hw = pw + 6.f * z;
                const float hh = ph + 6.f * z;
                overlay.box({{pt[0] - hw * .5f, pt[1] - hh * .5f, hw, hh},
                             hex_color(0xffd900, .28f),
                             {hh * .5f, hh * .5f, hh * .5f, hh * .5f},
                             0.f,
                             {}});
            }
            overlay.box({{pt[0] - pw * .5f, pt[1] - ph * .5f, pw, ph},
                         hot ? hex_color(0xffd900) : hex_color(0xb8c2cc),
                         {pr, pr, pr, pr},
                         .8f,
                         hex_color(0x16181c, .85f)});
        }
        for (std::size_t p = 0; p < node.ports.outputs.size(); ++p) {
            const auto pt = port_dip(node, true, p);
            const bool hot = (hovered_port_ && hovered_port_->is_output && hovered_port_->node == node.name &&
                              hovered_port_->index == p) ||
                             (snapped_port_ && snapped_port_->is_output && snapped_port_->node == node.name &&
                              snapped_port_->index == p);
            const float d = std::clamp(6.4f * z, 3.6f, 11.5f) + (hot ? 1.8f * z : 0.f);
            const float pr = d * .5f;
            if (hot) {
                const float hd = d + 6.f * z;
                overlay.box({{pt[0] - hd * .5f, pt[1] - hd * .5f, hd, hd},
                             hex_color(0xffd900, .28f),
                             {hd * .5f, hd * .5f, hd * .5f, hd * .5f},
                             0.f,
                             {}});
            }
            overlay.box({{pt[0] - pr, pt[1] - pr, d, d},
                         hot ? hex_color(0xffd900) : hex_color(0xb8c2cc),
                         {pr, pr, pr, pr},
                         .8f,
                         hex_color(0x16181c, .85f)});
        }

        const ui::ControlKey base_key = node_base_key(graph_key, node.name);
        ui::Tooltip help;
        help.title = sanitize_single_line(platform::utf8_to_utf16_validated(node.name));
        help.identity = sanitize_single_line(
            platform::utf8_to_utf16_validated(node.type + (is_source_sop ? " · Source SOP" : " · C++ SOP")));
        std::string desc = cat_it != catalog.end() ? cat_it->description : std::string{};
        if (!node.comment.empty()) {
            if (!desc.empty())
                desc += "\n";
            desc += "Comment: " + node.comment;
        }
        if (node.is_bypassed) {
            if (!desc.empty())
                desc += "\n";
            desc += "[Bypassed: input 0 passed straight through]";
        }
        if (node.is_frozen) {
            if (!desc.empty())
                desc += "\n";
            desc += "[Frozen: cooked geometry snapshot locked]";
        }
        if (node.status) {
            if (!desc.empty())
                desc += "\n";
            desc += std::format("Evaluations: {} · Cache hits: {}", node.status->evaluations, node.status->cache_hits);
            if (node.status->time)
                desc += std::format(" · Cook: {:.2f} ms", double(node.status->time->count()) / 1e6);
            if (const auto& bs = node.status->boolean_stats)
                desc += std::format("\nBoolean: {} faces · {} pairs · {} splits", bs->input_faces, bs->candidate_pairs,
                                    bs->splits);
            if (node.status->error)
                desc += "\nError: " + node.status->error->message;
        }
        help.description = application::diagnostic_text(desc);
        auto content = intern_tooltip(base_key, std::move(help));

        // Center vector icon inside node body (collinear with top/bottom port connectors):
        if (z >= .50f) {
            ui::Control icon_ctrl;
            icon_ctrl.key = base_key + 1;
            icon_ctrl.parent = canvas_parent;
            icon_ctrl.kind = ui::ControlKind::label;
            icon_ctrl.enabled = false;
            icon_ctrl.tooltip = content;
            icon_ctrl.layout = out_of_flow_box(vis_x, sy, vis_w, sh);
            icon_ctrl.icon = sop_node_icon(node.type, is_source_sop, has_error);
            icon_ctrl.style.background = ui::Color{0, 0, 0, 0};
            icon_ctrl.style.border_width = 0.f;
            icon_ctrl.style.padding = ui_layout::Insets{};
            icon_ctrl.style.icon_align = ui::TextAlign::center;
            icon_ctrl.style.indicator_size = std::clamp(std::round(13.5f * z), 7.f, 26.f);
            icon_ctrl.style.indicator_foreground =
                has_error ? hex_color(0xffb4b4)
                          : (is_source_sop ? hex_color(0x7dd3fc, body_alpha) : hex_color(0xe2e8f0, body_alpha));
            controls.push_back(std::move(icon_ctrl));
        }

        // External right-side Line 1: Bold Node Name (carries `base_key` & `tooltip` for hover):
        {
            ui::Control title_ctrl;
            title_ctrl.key = base_key;
            title_ctrl.parent = canvas_parent;
            title_ctrl.kind = ui::ControlKind::label;
            title_ctrl.layout = out_of_flow_box(sx + sw + 8.f * z, sy - 1.f * z, 148.f * z, 16.f * z);
            title_ctrl.text = sanitize_single_line(platform::utf8_to_utf16_validated(node.name));
            title_ctrl.single_line = true;
            title_ctrl.tooltip = content;
            title_ctrl.style.background = ui::Color{0, 0, 0, 0};
            title_ctrl.style.border_width = 0.f;
            title_ctrl.style.padding = ui_layout::Insets{};
            title_ctrl.style.text_vertical_align = ui::TextVerticalAlign::center;
            title_ctrl.style.font_size = std::clamp(std::round(12.f * z), 6.5f, 26.f);
            title_ctrl.style.font_weight = is_selected ? std::uint16_t{700} : std::uint16_t{600};
            title_ctrl.style.foreground = is_selected ? hex_color(0xffffff) : hex_color(0xe6ebf0, body_alpha);
            title_ctrl.style.text_shadow = ui::Color{0.f, 0.f, 0.f, .68f};
            controls.push_back(std::move(title_ctrl));
        }

        // External right-side Line 2: ShortType · cook_ms / Comment / ERR:
        if (z >= .55f) {
            std::wstring sub = sanitize_single_line(platform::utf8_to_utf16_validated(short_type_name(node.type)));
            if (has_error)
                sub += L" · ERR";
            else if (node.is_frozen)
                sub += L" · Frozen";
            else if (!node.comment.empty())
                sub += L" · " + sanitize_single_line(platform::utf8_to_utf16_validated(node.comment));
            else if (node.status && node.status->time)
                sub += std::format(L" · {:.1f} ms", double(node.status->time->count()) / 1e6);
            ui::Control badge_ctrl;
            badge_ctrl.key = base_key + 2;
            badge_ctrl.parent = canvas_parent;
            badge_ctrl.kind = ui::ControlKind::label;
            badge_ctrl.enabled = false;
            badge_ctrl.tooltip = content;
            badge_ctrl.layout = out_of_flow_box(sx + sw + 8.f * z, sy + 14.f * z, 156.f * z, 15.f * z);
            badge_ctrl.text = std::move(sub);
            badge_ctrl.single_line = true;
            badge_ctrl.role = ui::Role::caption;
            badge_ctrl.style.background = ui::Color{0, 0, 0, 0};
            badge_ctrl.style.border_width = 0.f;
            badge_ctrl.style.padding = ui_layout::Insets{};
            badge_ctrl.style.text_vertical_align = ui::TextVerticalAlign::center;
            badge_ctrl.style.font_size = std::clamp(std::round(10.f * z), 6.f, 20.f);
            badge_ctrl.style.foreground = has_error               ? hex_color(0xff8080)
                                          : node.is_frozen        ? hex_color(0x7dd3fc)
                                          : !node.comment.empty() ? hex_color(0x93c5fd)
                                                                  : hex_color(0x8c96a2);
            badge_ctrl.style.text_shadow = ui::Color{0.f, 0.f, 0.f, .65f};
            controls.push_back(std::move(badge_ctrl));
        }

        // Wide Non-Overlapping Hover Air-Ring (r_in = 54, r_out = 72 DIP):
        if (node.name == air_node_ && gesture_ == Gesture::none && !tab_search_.open && !popup_.open() && z >= .55f) {
            const float rin = air_inner_radius * z;
            const float rout = air_outer_radius * z;
            const float rmid = (rin + rout) * .5f;
            constexpr float deg_to_rad = std::numbers::pi_v<float> / 180.f;
            for (std::size_t s = 0; s < air_sector_defs.size(); ++s) {
                const auto& def = air_sector_defs[s];
                const float rad = def.angle_deg * deg_to_rad;
                const float half_rad = def.half_deg * deg_to_rad;
                const float dx = std::cos(rad);
                const float dy = std::sin(rad);
                const bool hot = static_cast<std::uint8_t>(hovered_air_) == def.button_id;
                const bool active = (def.button_id == 1 && node.is_bypassed) ||
                                    (def.button_id == 3 && node.is_frozen) || (def.button_id == 4 && node.is_display) ||
                                    (def.button_id == 5 && node.is_template);
                ui::Color s_fill = hex_color(0x1b1e24, 1.f);
                ui::Color s_border = hex_color(0x4b586a, 1.f);
                ui::Color s_fg = hex_color(def.idle_rgb, 1.f);
                if (def.button_id == 1 && active) {
                    s_fill = hex_color(0x78580a, 1.f);
                    s_border = hex_color(0xfacc15, 1.f);
                    s_fg = hex_color(0xfef08a, 1.f);
                } else if (def.button_id == 3 && active) {
                    s_fill = hex_color(0x0e4f68, 1.f);
                    s_border = hex_color(0x38bdf8, 1.f);
                    s_fg = hex_color(0xe0f2fe, 1.f);
                } else if (def.button_id == 4 && active) {
                    s_fill = hex_color(0x0c4a6e, 1.f);
                    s_border = hex_color(0x38bdf8, 1.f);
                    s_fg = hex_color(0xe0f2fe, 1.f);
                } else if (def.button_id == 5 && active) {
                    s_fill = hex_color(0x581c68, 1.f);
                    s_border = hex_color(0xe879f9, 1.f);
                    s_fg = hex_color(0xfae8ff, 1.f);
                }
                if (hot) {
                    s_fill =
                        active ? hex_color(blend_hex(0x364356, def.idle_rgb, .30f), 1.f) : hex_color(0x2e3746, 1.f);
                    s_border = hex_color(0xffd900, 1.f);
                    s_fg = hex_color(0xffffff, 1.f);
                }
                overlay.arc({cx, cy, rin, rout, dx, dy, std::cos(half_rad), std::sin(half_rad), s_fill,
                             hot ? 1.45f : 1.05f, s_border});

                const ui::ControlKey akey = base_key + 3 + s;
                ui::Tooltip ahelp{def.title, L"Air-Ring Action", platform::utf8_to_utf16_validated(def.description)};
                ui::Control gl;
                gl.key = akey;
                gl.parent = canvas_parent;
                gl.kind = ui::ControlKind::label;
                gl.tooltip = intern_tooltip(akey, std::move(ahelp));
                const float gw = 18.f * z;
                const float gh = 18.f * z;
                gl.layout = out_of_flow_box(cx + dx * rmid - gw * .5f, cy + dy * rmid - gh * .5f, gw, gh);
                gl.icon = def.icon;
                gl.style.background = ui::Color{0, 0, 0, 0};
                gl.style.border_width = 0.f;
                gl.style.padding = ui_layout::Insets{};
                gl.style.icon_align = ui::TextAlign::center;
                gl.style.indicator_size = std::clamp(std::round(12.5f * z), 8.f, 21.f);
                gl.style.indicator_foreground = s_fg;
                gl.style.foreground = s_fg;
                controls.push_back(std::move(gl));
            }
        }
    }

    // 5. Active interactive foreground overlays (live wire drag, marquee box, Y wire-cut):
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
            const float width = std::clamp(1.35f * std::sqrt(z), 1.05f, 1.75f);
            const auto color = snapped_port_ ? hex_color(0xffd900) : hex_color(0x69b6ff);
            if (wire_near_region(from_pt, to_pt, z, {0, 0, cw, ch}))
                for_each_wire_segment(from_pt, to_pt, z, [&](auto a, auto b) {
                    append_wire_segment(overlay, a, b, width, color, {cw, ch});
                    return false;
                });
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
            tag.text = sanitize_single_line(platform::utf8_to_utf16_validated(label));
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

    visible_tooltips_.swap(tooltips);
    if (idle) {
        const auto revision = ui::ControlList::new_region_revision();
        if (!popup_.open()) {
            idle_projection_.emplace(IdleProjection{topology,
                                                    std::move(*idle),
                                                    {selected_nodes.begin(), selected_nodes.end()},
                                                    {catalog.begin(), catalog.end()},
                                                    std::move(controls),
                                                    revision});
            output.append(idle_projection_->controls, revision);
            return;
        }
        idle_projection_.emplace(IdleProjection{topology,
                                                std::move(*idle),
                                                {selected_nodes.begin(), selected_nodes.end()},
                                                {catalog.begin(), catalog.end()},
                                                controls,
                                                revision});
    }
    append_canvas_popup(controls, canvas_parent, cw, ch, view, catalog);
    output.append(controls, 0);
}

void NetworkCanvas::append_canvas_popup(std::vector<ui::Control>& controls, ui::ControlKey canvas_parent, float cw,
                                        float ch, const GraphInspectView& view,
                                        std::span<const GraphNodeCatalogEntry> catalog) const {
    if (popup_.open()) {
        std::size_t aux_idx = 0;
        const auto next_aux = [&]() { return network_control::popup_aux_first + (aux_idx++); };

        const float pw = popup_.kind == PopupKind::node_info ? 252.f : 236.f;
        ui::Control card;
        card.key = network_control::popup_box;
        card.parent = canvas_parent;
        card.kind = ui::ControlKind::container;
        card.layout = ui_layout::NodeInput::box(pw, 0.f);
        card.layout.out_of_flow = true;
        card.layout.offset_x = std::clamp(popup_.dip_x, 6.f, std::max(6.f, cw - pw - 6.f));
        card.layout.offset_y = std::clamp(popup_.dip_y, 6.f, std::max(6.f, ch - 188.f));
        card.layout.kind = ui_layout::Kind::column;
        card.layout.height = ui_layout::Axis::content();
        card.layout.gap = 5.f;
        card.style.padding = {7.f, 6.f, 7.f, 7.f};
        card.style.background = hex_color(0x1e2126, .97f);
        card.style.border = hex_color(0x4b5563);
        card.style.border_width = 1.f;
        card.style.radius = {6.f, 6.f, 6.f, 6.f};
        controls.push_back(std::move(card));

        // Popup header row:
        ui::Control hrow;
        hrow.key = network_control::popup_header;
        hrow.parent = network_control::popup_box;
        hrow.kind = ui::ControlKind::container;
        hrow.layout = ui_layout::NodeInput::box(0.f, 20.f);
        hrow.layout.kind = ui_layout::Kind::row;
        hrow.layout.width = ui_layout::Axis::expand();
        hrow.layout.gap = 4.f;
        hrow.style.background = ui::Color{0, 0, 0, 0};
        controls.push_back(std::move(hrow));

        ui::Control htitle;
        htitle.key = next_aux();
        htitle.parent = network_control::popup_header;
        htitle.kind = ui::ControlKind::label;
        htitle.single_line = true;
        htitle.text = popup_.kind == PopupKind::shape_picker   ? L"Node Shape Preset [Z]"
                      : popup_.kind == PopupKind::color_picker ? L"Node Color Swatch [C]"
                      : popup_.kind == PopupKind::edit_note    ? L"Sticky Note"
                      : popup_.kind == PopupKind::edit_box
                          ? L"Network Box"
                          : sanitize_single_line(platform::utf8_to_utf16_validated(popup_.target_name));
        htitle.layout = ui_layout::NodeInput::box(0.f, 20.f);
        htitle.layout.width = ui_layout::Axis::expand();
        htitle.style.font_size = 11.5f;
        htitle.style.font_weight = 700;
        htitle.style.foreground = hex_color(0xf8fafc);
        htitle.style.text_vertical_align = ui::TextVerticalAlign::center;
        controls.push_back(std::move(htitle));

        if (popup_.kind == PopupKind::shape_picker || popup_.kind == PopupKind::color_picker) {
            ui::Control sw_btn;
            sw_btn.key = network_control::popup_switch_style;
            sw_btn.parent = network_control::popup_header;
            sw_btn.kind = ui::ControlKind::button;
            sw_btn.text = popup_.kind == PopupKind::color_picker ? L"Shapes [Z]" : L"Colors [C]";
            sw_btn.single_line = true;
            sw_btn.classes = {"ghost-button"};
            sw_btn.layout = ui_layout::NodeInput::box(64.f, 20.f);
            sw_btn.style.font_size = 10.5f;
            sw_btn.style.radius = {3.f, 3.f, 3.f, 3.f};
            controls.push_back(std::move(sw_btn));
        }

        if (popup_.kind == PopupKind::edit_note || popup_.kind == PopupKind::edit_box) {
            ui::Control del_btn;
            del_btn.key = network_control::popup_delete_item;
            del_btn.parent = network_control::popup_header;
            del_btn.kind = ui::ControlKind::button;
            del_btn.icon = ui::ControlIcon::trash;
            del_btn.classes = {"ghost-tool"};
            del_btn.layout = ui_layout::NodeInput::box(22.f, 20.f);
            del_btn.style.radius = {3.f, 3.f, 3.f, 3.f};
            controls.push_back(std::move(del_btn));
        }

        ui::Control close_btn;
        close_btn.key = network_control::popup_close;
        close_btn.parent = network_control::popup_header;
        close_btn.kind = ui::ControlKind::button;
        close_btn.icon = ui::ControlIcon::close;
        close_btn.classes = {"ghost-tool"};
        close_btn.layout = ui_layout::NodeInput::box(22.f, 20.f);
        close_btn.style.radius = {3.f, 3.f, 3.f, 3.f};
        controls.push_back(std::move(close_btn));

        if (popup_.kind == PopupKind::shape_picker) {
            const auto* target_nv = view.find_node(popup_.target_name);
            const GraphNodeShape cur_shape = target_nv ? target_nv->shape : GraphNodeShape::auto_role;

            ui::Control auto_btn;
            auto_btn.key = network_control::shape_pick_first;
            auto_btn.parent = network_control::popup_box;
            auto_btn.kind = ui::ControlKind::button;
            auto_btn.text = L"Auto (Match SOP Role)";
            auto_btn.single_line = true;
            auto_btn.selected = cur_shape == GraphNodeShape::auto_role;
            auto_btn.layout = ui_layout::NodeInput::box(0.f, 22.f);
            auto_btn.layout.width = ui_layout::Axis::expand();
            auto_btn.style.font_size = 11.f;
            auto_btn.style.radius = {3.f, 3.f, 3.f, 3.f};
            auto_btn.style.background =
                cur_shape == GraphNodeShape::auto_role ? hex_color(0x2a4f6e) : hex_color(0x292d34);
            auto_btn.style.border = cur_shape == GraphNodeShape::auto_role ? hex_color(0x38bdf8) : hex_color(0x3f4652);
            auto_btn.style.border_width = 1.f;
            auto_btn.style.foreground = hex_color(0xe2e8f0);
            auto_btn.style.text_align = ui::TextAlign::center;
            auto_btn.style.text_vertical_align = ui::TextVerticalAlign::center;
            controls.push_back(std::move(auto_btn));

            constexpr std::array<const wchar_t*, 8> shape_titles{L"Pill",    L"Rect",   L"Domed",   L"Trap",
                                                                 L"Hexagon", L"Ribbon", L"Diamond", L"Circle"};
            constexpr std::array<const char*, 8> shape_descs{"圆角胶囊形：常规属性与拓扑修改节点的经典外观。",
                                                             "倒角矩形：清晰利落的通用工程节点轮廓。",
                                                             "上拱圆顶形：无上游输入的原始几何生成器造型。",
                                                             "上宽下窄梯形：多路合并（Merge）与实例化汇流造型。",
                                                             "外折六边形：项目自定义 .source 语言 SOP 专属造型。",
                                                             "内折燕尾丝带形：撒点（Scatter）与点云发射器造型。",
                                                             "菱形里程碑：Null 空节点与关键输出锚点造型。",
                                                             "紧凑圆形：轻量控制与开关节点造型。"};
            static const auto shape_help = [&] {
                std::array<std::shared_ptr<const ui::Tooltip>, shape_titles.size()> result;
                for (std::size_t i = 0; i < result.size(); ++i)
                    result[i] = std::make_shared<const ui::Tooltip>(
                        ui::Tooltip{shape_titles[i], L"Node Shape", platform::utf8_to_utf16_validated(shape_descs[i])});
                return result;
            }();
            for (std::size_t row_i = 0; row_i < 2; ++row_i) {
                const ui::ControlKey rkey = next_aux();
                ui::Control srow;
                srow.key = rkey;
                srow.parent = network_control::popup_box;
                srow.kind = ui::ControlKind::container;
                srow.layout = ui_layout::NodeInput::box(0.f, 38.f);
                srow.layout.kind = ui_layout::Kind::row;
                srow.layout.width = ui_layout::Axis::expand();
                srow.layout.gap = 4.f;
                controls.push_back(std::move(srow));

                for (std::size_t col_i = 0; col_i < 4; ++col_i) {
                    const std::size_t idx = row_i * 4 + col_i;
                    const auto enum_val = static_cast<GraphNodeShape>(idx + 1);
                    const bool active = cur_shape == enum_val;
                    const ui::ControlKey wrap_key = next_aux();
                    const ui::ControlKey bkey = network_control::shape_pick_first + 1 + idx;

                    ui::Control wrap;
                    wrap.key = wrap_key;
                    wrap.parent = rkey;
                    wrap.kind = ui::ControlKind::container;
                    wrap.layout = ui_layout::NodeInput::overlay();
                    wrap.layout.width = ui_layout::Axis::expand();
                    wrap.layout.height = ui_layout::Axis::fixed(38.f);
                    controls.push_back(std::move(wrap));

                    ui::Control cell;
                    cell.key = bkey;
                    cell.parent = wrap_key;
                    cell.kind = ui::ControlKind::button;
                    cell.text = shape_titles[idx];
                    cell.single_line = true;
                    cell.selected = active;
                    cell.tooltip = shape_help[idx];
                    cell.layout = ui_layout::NodeInput::box(0.f, 38.f);
                    cell.layout.width = ui_layout::Axis::expand();
                    cell.style.font_size = 9.5f;
                    cell.style.padding = {0.f, 20.f, 0.f, 0.f};
                    cell.style.radius = {4.f, 4.f, 4.f, 4.f};
                    cell.style.background = active ? hex_color(0x294861) : hex_color(0x262a31);
                    cell.style.border = active ? hex_color(0xffd900) : hex_color(0x3b424d);
                    cell.style.border_width = 1.f;
                    cell.style.foreground = hex_color(0xcbd5e1);
                    cell.style.text_align = ui::TextAlign::center;
                    cell.style.text_vertical_align = ui::TextVerticalAlign::center;
                    controls.push_back(std::move(cell));

                    // Miniature analytic SDF preview overlaid on the shape cell:
                    ui::Control preview;
                    preview.key = next_aux();
                    preview.parent = wrap_key;
                    preview.kind = ui::ControlKind::container;
                    preview.enabled = false;
                    preview.layout = out_of_flow_box(6.f, 4.f, 42.f, 15.f);
                    auto cv = std::make_shared<ui::CanvasData>();
                    const float pw_mini = idx == 7 ? 24.f : 40.f;
                    const float px_mini = idx == 7 ? 9.f : 1.f;
                    cv->shapes.push_back({{px_mini, 1.f, pw_mini, 13.f},
                                          static_cast<std::uint8_t>(idx),
                                          0,
                                          0.f,
                                          0.f,
                                          hex_color(0x94a3b8),
                                          1.f,
                                          hex_color(0x0f172a)});
                    preview.canvas = std::move(cv);
                    controls.push_back(std::move(preview));
                }
            }
        } else if (popup_.kind == PopupKind::color_picker) {
            static const auto swatch_help = [] {
                std::array<std::shared_ptr<const ui::Tooltip>, color_palette.size()> result;
                for (std::size_t i = 0; i < result.size(); ++i)
                    result[i] = std::make_shared<const ui::Tooltip>(
                        ui::Tooltip{platform::utf8_to_utf16_validated(format_graph_hex_color(color_palette[i])),
                                    L"Color Swatch", L"将选中节点的自定义颜色设为此预设色板值。"});
                return result;
            }();
            for (std::size_t row_i = 0; row_i < 4; ++row_i) {
                const ui::ControlKey rkey = next_aux();
                ui::Control crow;
                crow.key = rkey;
                crow.parent = network_control::popup_box;
                crow.kind = ui::ControlKind::container;
                crow.layout = ui_layout::NodeInput::box(0.f, 22.f);
                crow.layout.kind = ui_layout::Kind::row;
                crow.layout.width = ui_layout::Axis::expand();
                crow.layout.gap = 4.f;
                controls.push_back(std::move(crow));

                for (std::size_t col_i = 0; col_i < 6; ++col_i) {
                    const std::size_t idx = row_i * 6 + col_i;
                    const std::uint32_t rgb = color_palette[idx];
                    const ui::ControlKey skey = network_control::color_pick_first + idx;
                    ui::Control sw_btn;
                    sw_btn.key = skey;
                    sw_btn.parent = rkey;
                    sw_btn.kind = ui::ControlKind::button;
                    sw_btn.tooltip = swatch_help[idx];
                    sw_btn.layout = ui_layout::NodeInput::box(0.f, 22.f);
                    sw_btn.layout.width = ui_layout::Axis::expand();
                    sw_btn.style.radius = {3.f, 3.f, 3.f, 3.f};
                    sw_btn.style.background = hex_color(rgb);
                    sw_btn.style.border = hex_color(0x0f172a, .8f);
                    sw_btn.style.border_width = 1.f;
                    controls.push_back(std::move(sw_btn));
                }
            }

            ui::Control rst_btn;
            rst_btn.key = network_control::color_reset_btn;
            rst_btn.parent = network_control::popup_box;
            rst_btn.kind = ui::ControlKind::button;
            rst_btn.text = L"Reset to Role Color";
            rst_btn.single_line = true;
            rst_btn.layout = ui_layout::NodeInput::box(0.f, 22.f);
            rst_btn.layout.width = ui_layout::Axis::expand();
            rst_btn.style.font_size = 11.f;
            rst_btn.style.radius = {3.f, 3.f, 3.f, 3.f};
            rst_btn.style.background = hex_color(0x292d34);
            rst_btn.style.border = hex_color(0x3f4652);
            rst_btn.style.border_width = 1.f;
            rst_btn.style.foreground = hex_color(0xcbd5e1);
            rst_btn.style.text_align = ui::TextAlign::center;
            rst_btn.style.text_vertical_align = ui::TextVerticalAlign::center;
            controls.push_back(std::move(rst_btn));
        } else if (popup_.kind == PopupKind::node_info) {
            if (const auto* nv = view.find_node(popup_.target_name)) {
                const auto cat_it = std::ranges::find(catalog, nv->type, &GraphNodeCatalogEntry::name);
                const bool is_src = cat_it != catalog.end() && !cat_it->native;
                std::wstring info_txt = L"Type: " + sanitize_single_line(platform::utf8_to_utf16_validated(nv->type)) +
                                        (is_src ? L" (Source SOP)" : L" (C++ SOP)");
                info_txt += std::format(L"\nPorts: {} in / {} out", nv->ports.inputs.size(), nv->ports.outputs.size());
                if (nv->is_frozen)
                    info_txt += L"\nState: Frozen (output snapshot locked)";
                else if (nv->is_bypassed)
                    info_txt += L"\nState: Bypassed (input 0 pass-through)";
                if (nv->status) {
                    info_txt +=
                        std::format(L"\nCooks: {}  ·  Cache hits: {}", nv->status->evaluations, nv->status->cache_hits);
                    if (nv->status->time)
                        info_txt += std::format(L"\nCook time: {:.2f} ms", double(nv->status->time->count()) / 1e6);
                    if (const auto& bs = nv->status->boolean_stats)
                        info_txt += std::format(L"\nBoolean: {} faces · {} pairs · {} splits", bs->input_faces,
                                                bs->candidate_pairs, bs->splits);
                    if (nv->status->error)
                        info_txt += L"\nError: " + application::diagnostic_text(nv->status->error->message);
                }

                ui::Control stats;
                stats.key = next_aux();
                stats.parent = network_control::popup_box;
                stats.kind = ui::ControlKind::label;
                stats.single_line = false;
                stats.text = sanitize_multiline_label(info_txt);
                const float stats_h = 68.f + (nv->status && nv->status->boolean_stats ? 16.f : 0.f) +
                                      ((nv->is_frozen || nv->is_bypassed) ? 15.f : 0.f);
                stats.layout = ui_layout::NodeInput::box(0.f, stats_h);
                stats.layout.width = ui_layout::Axis::expand();
                stats.style.font_size = 10.5f;
                stats.style.foreground = hex_color(0xcbd5e1);
                controls.push_back(std::move(stats));

                ui::Control clabel;
                clabel.key = next_aux();
                clabel.parent = network_control::popup_box;
                clabel.kind = ui::ControlKind::label;
                clabel.single_line = true;
                clabel.text = L"Comment Badge:";
                clabel.layout = ui_layout::NodeInput::box(0.f, 15.f);
                clabel.layout.width = ui_layout::Axis::expand();
                clabel.style.font_size = 10.5f;
                clabel.style.foreground = hex_color(0x94a3b8);
                controls.push_back(std::move(clabel));

                ui::Control cedit;
                cedit.key = network_control::info_comment_input;
                cedit.parent = network_control::popup_box;
                cedit.kind = ui::ControlKind::text_edit;
                cedit.text = popup_.body_edit;
                cedit.layout = ui_layout::NodeInput::box(0.f, 23.f);
                cedit.layout.width = ui_layout::Axis::expand();
                cedit.classes = {"field"};
                controls.push_back(std::move(cedit));
            }
        } else if (popup_.kind == PopupKind::edit_note || popup_.kind == PopupKind::edit_box) {
            const bool is_note = popup_.kind == PopupKind::edit_note;
            ui::Control tedit;
            tedit.key = is_note ? network_control::note_title_input : network_control::box_title_input;
            tedit.parent = network_control::popup_box;
            tedit.kind = ui::ControlKind::text_edit;
            tedit.text = popup_.title_edit;
            tedit.layout = ui_layout::NodeInput::box(0.f, 23.f);
            tedit.layout.width = ui_layout::Axis::expand();
            tedit.classes = {"field"};
            controls.push_back(std::move(tedit));

            if (is_note) {
                ui::Control blabel;
                blabel.key = next_aux();
                blabel.parent = network_control::popup_box;
                blabel.kind = ui::ControlKind::label;
                blabel.single_line = true;
                blabel.text = L"Body (use \\n for newline):";
                blabel.layout = ui_layout::NodeInput::box(0.f, 15.f);
                blabel.layout.width = ui_layout::Axis::expand();
                blabel.style.font_size = 10.f;
                blabel.style.foreground = hex_color(0x94a3b8);
                controls.push_back(std::move(blabel));

                ui::Control bedit;
                bedit.key = network_control::note_body_input;
                bedit.parent = network_control::popup_box;
                bedit.kind = ui::ControlKind::text_edit;
                bedit.text = popup_.body_edit;
                bedit.layout = ui_layout::NodeInput::box(0.f, 23.f);
                bedit.layout.width = ui_layout::Axis::expand();
                bedit.classes = {"field"};
                controls.push_back(std::move(bedit));
            }

            const ui::ControlKey crow_key = next_aux();
            ui::Control crow;
            crow.key = crow_key;
            crow.parent = network_control::popup_box;
            crow.kind = ui::ControlKind::container;
            crow.layout = ui_layout::NodeInput::box(0.f, 20.f);
            crow.layout.kind = ui_layout::Kind::row;
            crow.layout.width = ui_layout::Axis::expand();
            crow.layout.gap = 4.f;
            controls.push_back(std::move(crow));

            constexpr std::array<std::size_t, 6> quick_swatches{3, 1, 7, 11, 14, 20};
            for (const std::size_t sw_i : quick_swatches) {
                ui::Control sw;
                sw.key = network_control::color_pick_first + sw_i;
                sw.parent = crow_key;
                sw.kind = ui::ControlKind::button;
                sw.layout = ui_layout::NodeInput::box(0.f, 20.f);
                sw.layout.width = ui_layout::Axis::expand();
                sw.style.radius = {3.f, 3.f, 3.f, 3.f};
                sw.style.background = hex_color(color_palette[sw_i]);
                sw.style.border = hex_color(0x0f172a, .8f);
                sw.style.border_width = 1.f;
                controls.push_back(std::move(sw));
            }
        }
    }
}

} // namespace atelier::editor
