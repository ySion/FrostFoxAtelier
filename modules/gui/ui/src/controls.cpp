#include "ui/controls.hpp"
#include "ui/detail/number_bits.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace atelier::ui {
bool SliderRange::operator==(const SliderRange& other) const {
    using detail::number_bits;
    return key == other.key && number_bits(minimum) == number_bits(other.minimum) &&
           number_bits(maximum) == number_bits(other.maximum);
}

bool NumberField::operator==(const NumberField& other) const {
    using detail::number_bits;
    return key == other.key && number_bits(value) == number_bits(other.value) &&
           number_bits(default_value) == number_bits(other.default_value) &&
           number_bits(minimum) == number_bits(other.minimum) && number_bits(maximum) == number_bits(other.maximum) &&
           number_bits(drag_step) == number_bits(other.drag_step) &&
           number_bits(fine_step) == number_bits(other.fine_step) && label == other.label && axis == other.axis &&
           slider == other.slider && single_precision == other.single_precision && integral == other.integral &&
           enabled == other.enabled;
}

ui_layout::NodeInput single_line_layout(float width) {
    auto layout = ui_layout::NodeInput::box(width, single_line_height);
    layout.height = ui_layout::Axis::content(single_line_height);
    return layout;
}

void append_parameter_row(std::vector<Control>& controls, const ParameterRow& row, float body_minimum) {
    using namespace ui_layout;
    const auto append = [&](ControlKey key, ControlKey parent, ControlKind kind, NodeInput layout,
                            std::wstring text = {}) -> Control& {
        Control control;
        control.key = key;
        control.parent = parent;
        control.kind = kind;
        control.layout = std::move(layout);
        control.text = std::move(text);
        controls.push_back(std::move(control));
        return controls.back();
    };
    const bool modified = row.group_modified.value_or(false);
    const ControlKey ctx = row.context_target ? row.context_target : row.group_key;
    auto layout = NodeInput::row(6);
    layout.cross_gap = 2;
    layout.flow = FlexSpec{};
    layout.flow->wrap = Wrap::wrap;
    layout.flow->align_items = Alignment::fit(.5f);
    auto& root = append(row.key, row.parent, ControlKind::container, layout);
    root.enabled = row.enabled;
    root.context_target = ctx;
    root.context_group = row.group_key;
    root.classes = {"parameter-row"};
    auto caption = single_line_layout();
    caption.width = Axis::flex(Basis::fixed(116.f), .34f, 1.f, 76.f, 162.f);
    auto& label =
        append(row.group_key ? row.group_key : row.label_key, row.key, ControlKind::label, caption, row.label);
    label.single_line = true;
    label.classes = {"parameter-label"};
    label.modified = modified;
    label.context_target = ctx;
    label.context_group = row.group_key;
    auto body = NodeInput::row(2);
    body.height = Axis::content(single_line_height);
    body.width = Axis::flex(Basis::fixed(std::max(128.f, body_minimum)), .66f, 1.f, body_minimum);
    auto& body_ctrl = append(row.body_key, row.key, ControlKind::container, body);
    body_ctrl.context_target = ctx;
    body_ctrl.context_group = row.group_key;
}

Result<void> append_number_row(std::vector<Control>& controls, ParameterRow row, std::span<const NumberField> fields) {
    if (fields.empty() || fields.size() > 4)
        return std::unexpected(Error{ErrorCode::invalid_control, row.key, 0, "A number row requires 1–4 fields."});
    for (const auto& field : fields)
        if (static_cast<unsigned>(field.axis) > static_cast<unsigned>(VectorAxis::w))
            return std::unexpected(
                Error{ErrorCode::invalid_control, field.key, 0, "A number field requires a valid vector axis."});
    row.group_modified = row.group_modified.value_or(std::any_of(
        fields.begin(), fields.end(), [](const auto& field) { return field.value != field.default_value; }));
    constexpr float field_minimum = 24;
    append_parameter_row(controls, row,
                         fields.front().slider ? 114.f
                                               : static_cast<float>(fields.size()) * field_minimum +
                                                     static_cast<float>(fields.size() - 1) * 2);
    for (const auto& field : fields) {
        if (field.slider &&
            (fields.size() != 1 || !field.slider->key || field.slider->minimum >= field.slider->maximum ||
             field.slider->minimum < field.minimum || field.slider->maximum > field.maximum ||
             (field.integral && std::ceil(field.slider->minimum) >= std::floor(field.slider->maximum))))
            return std::unexpected(
                Error{ErrorCode::invalid_control, field.key, 0,
                      "A scalar slider requires a unique key and a range within the number's legal range."});
        Control number;
        number.key = field.key;
        number.parent = row.body_key;
        number.kind = ControlKind::number;
        number.enabled = row.fields_enabled && field.enabled;
        number.text = field.label;
        number.layout = single_line_layout();
        number.layout.width = ui_layout::Axis::expand(1, field_minimum);
        number.value = field.value;
        number.minimum = field.minimum;
        number.maximum = field.maximum;
        number.single_precision = field.single_precision;
        number.integral = field.integral;
        number.default_value = field.default_value;
        number.context_target = field.key;
        number.context_group = row.group_key;
        number.drag_step = field.drag_step;
        number.fine_step = field.fine_step;
        number.modified = field.value != field.default_value;
        number.classes = {"parameter-field"};
        constexpr std::array axis_classes{"", "axis-x", "axis-y", "axis-z", "axis-w"};
        if (field.axis != VectorAxis::none)
            number.classes.push_back(axis_classes.at(static_cast<std::size_t>(field.axis)));
        if (field.slider)
            number.layout.width = ui_layout::Axis::fixed(72);
        // Houdini: a lone scalar is one tuple column wide (about a third of the
        // body), never a row-wide strip; tuples and slider pairs keep expanding.
        else if (fields.size() == 1)
            number.layout.width =
                ui_layout::Axis::flex(ui_layout::Basis::fixed(scalar_field_width), 0, 1, field_minimum);
        controls.push_back(std::move(number));
        if (field.slider) {
            Control slider;
            slider.key = field.slider->key;
            slider.parent = row.body_key;
            slider.kind = ControlKind::slider;
            slider.enabled = row.fields_enabled && field.enabled;
            slider.layout = single_line_layout();
            slider.layout.width = ui_layout::Axis::expand(1, 40);
            slider.value = field.value;
            slider.minimum = field.slider->minimum;
            slider.maximum = field.slider->maximum;
            slider.drag_step = field.drag_step;
            slider.fine_step = field.fine_step;
            slider.integral = field.integral;
            slider.context_target = field.key;
            slider.context_group = row.group_key;
            slider.classes = {"parameter-slider"};
            controls.push_back(std::move(slider));
        }
    }
    return {};
}

namespace {
// Bits [0, depth).
std::bitset<128> levels_below(std::uint32_t depth) {
    return ~std::bitset<128>{} >> (128 - depth);
}
}

Result<std::vector<TreeRow>> flatten_tree(std::span<const TreeNode> nodes, std::wstring_view filter,
                                          std::vector<TreeRow> rows) {
    rows.clear();
    if (nodes.size() >= no_tree_node)
        return std::unexpected(Error{ErrorCode::capacity_exceeded, {}, {}, "A tree has too many nodes."});
    // Ancestor path of the current node: node and its row (no_tree_node when not shown).
    std::array<std::uint32_t, 128> path{};
    std::array<std::uint32_t, 128> path_rows{};
    std::uint32_t path_size{};
    const auto branch = [&](std::uint32_t index) {
        return index + 1 < nodes.size() && nodes[index + 1].parent == index;
    };
    // Filtering keeps matches, their subtrees (below) and their ancestors (above).
    enum : std::uint8_t { match = 1, below = 2, above = 4 };
    std::vector<std::uint8_t> kept;
    if (!filter.empty()) {
        kept.resize(nodes.size());
        const auto fold = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + L'a' - L'A') : c; };
        for (std::uint32_t i = 0; i < nodes.size(); ++i) {
            const auto& text = nodes[i].text;
            if (std::search(text.begin(), text.end(), filter.begin(), filter.end(),
                            [&](wchar_t a, wchar_t b) { return fold(a) == fold(b); }) != text.end())
                kept[i] |= match;
            if (const auto parent = nodes[i].parent;
                parent != no_tree_node && parent < i && (kept[parent] & (match | below)))
                kept[i] |= below;
        }
        for (auto i = nodes.size(); i-- > 0;)
            if ((kept[i] & (match | above)) && nodes[i].parent < i)
                kept[nodes[i].parent] |= above;
    }
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const auto parent = nodes[i].parent;
        if (parent == no_tree_node)
            path_size = 0;
        else {
            while (path_size && path[path_size - 1] != parent)
                --path_size;
            if (!path_size)
                return std::unexpected(
                    Error{ErrorCode::invalid_control, {}, {}, "Tree nodes must be in depth-first preorder."});
        }
        if (path_size == path.size())
            return std::unexpected(Error{ErrorCode::capacity_exceeded, {}, {}, "A tree exceeds 128 levels."});
        const auto depth = path_size;
        const auto parent_row = depth ? path_rows[depth - 1] : no_tree_node;
        const bool shown =
            filter.empty() ? (!depth || (parent_row != no_tree_node && nodes[path[depth - 1]].expanded)) : kept[i] != 0;
        path[depth] = i;
        path_rows[depth] = shown ? static_cast<std::uint32_t>(rows.size()) : no_tree_node;
        path_size = depth + 1;
        if (!shown)
            continue;
        if (rows.size() == maximum_tree_rows)
            return std::unexpected(
                Error{ErrorCode::capacity_exceeded, {}, {}, "A tree shows more rows than Session accepts."});
        const bool children = branch(i);
        rows.push_back({i, parent_row, depth, children, children && (!filter.empty() || nodes[i].expanded), {}});
    }
    // A guide continues through a row while the ancestor at its level, or the
    // row itself, has a later shown sibling. Bit 0 temporarily holds that fact.
    std::bitset<128> later;
    for (auto row = rows.rbegin(); row != rows.rend(); ++row) {
        row->lines[0] = later[row->depth];
        later.set(row->depth);
        later &= levels_below(row->depth + 1);
    }
    std::bitset<128> guides;
    for (auto& row : rows) {
        guides &= levels_below(row.depth);
        if (row.depth && row.lines[0])
            guides.set(row.depth);
        row.lines = guides;
    }
    return rows;
}

std::vector<std::uint32_t> tree_window(std::span<const TreeRow> rows, TreeViewport viewport,
                                       std::span<const std::uint32_t> pinned) {
    std::vector<std::uint32_t> result;
    if (rows.empty())
        return result;
    const auto last = static_cast<std::uint32_t>(rows.size() - 1);
    const double row_height = std::max(1.f, viewport.row_height);
    const double height = std::max(0.f, viewport.height);
    // NaN or a negative position maps to the first row.
    const auto row_at = [&](double y) {
        return y > 0 ? static_cast<std::uint32_t>(std::min(std::floor(y / row_height), static_cast<double>(last))) : 0u;
    };
    const auto begin = row_at(viewport.scroll - height);
    const auto end = row_at(viewport.scroll + 2 * height);
    result.reserve(end - begin + 3 + pinned.size() * 4);
    result.push_back(0);
    result.push_back(last);
    for (auto i = begin; i <= end; ++i)
        result.push_back(i);
    for (const auto row : pinned)
        if (row <= last) {
            if (row)
                result.push_back(row - 1);
            result.push_back(row);
            if (row < last)
                result.push_back(row + 1);
            for (auto parent = rows[row].parent; parent != no_tree_node; parent = rows[parent].parent)
                result.push_back(parent);
        }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
} // namespace atelier::ui
