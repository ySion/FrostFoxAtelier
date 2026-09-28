#include "editor/workspace.hpp"
#include <stdexcept>

namespace atelier::editor {
std::vector<ui::Control> Workspace::describe(const Session& session) const {
    using namespace ui_layout;
    using ui::ControlKind;
    std::vector<ui::Control> result;
    result.reserve(32 + session.scene().members().size());
    const auto add = [&](ui::ControlKey key, ui::ControlKey parent, ControlKind kind,
                         NodeInput layout, std::wstring text = {}) -> ui::Control& {
        ui::Control item;
        item.key = key; item.parent = parent; item.kind = kind; item.layout = std::move(layout); item.text = std::move(text);
        result.push_back(std::move(item));
        return result.back();
    };
    auto root = NodeInput::column(); root.width = root.height = Axis::expand();
    add(1, 0, ControlKind::container, root);
    auto toolbar = NodeInput::row(6); toolbar.height = Axis::fixed(48); toolbar.padding = {12, 8, 12, 8};
    add(2, 1, ControlKind::container, toolbar).role = ui::Role::panel;
    add(3, 2, ControlKind::label, NodeInput::box(210, 32), L"FROSTFOX / Scene").role = ui::Role::title;
    add(control::add, 2, ControlKind::button, NodeInput::box(88, 32), L"+ Add cube");
    add(control::remove, 2, ControlKind::button, NodeInput::box(72, 32), L"Delete").enabled = session.selected() != 0;
    add(control::undo, 2, ControlKind::button, NodeInput::box(64, 32), L"Undo").enabled = session.undo_count() != 0;
    add(control::redo, 2, ControlKind::button, NodeInput::box(64, 32), L"Redo").enabled = session.redo_count() != 0;
    add(control::focus, 2, ControlKind::button, NodeInput::box(64, 32), L"Focus").enabled = session.selected() != 0;
    add(control::reset, 2, ControlKind::button, NodeInput::box(108, 32), L"Reset camera");
    auto body = NodeInput::row(); body.height = Axis::expand();
    add(20, 1, ControlKind::container, body);
    auto left = NodeInput::column(8); left.width = Axis::fixed(192); left.height = Axis::expand(); left.padding = {10, 12, 10, 12};
    add(21, 20, ControlKind::container, left).role = ui::Role::panel;
    auto heading = NodeInput::box(0, 28); heading.width = Axis::expand();
    add(22, 21, ControlKind::label, heading, L"OBJECTS").role = ui::Role::caption;
    auto list = NodeInput::column(3); list.height = Axis::expand();
    add(23, 21, ControlKind::list, list);
    for (const auto& member : session.scene().members()) {
        auto row = NodeInput::box(0, 32); row.width = Axis::expand();
        auto& item = add(control::first_object + member.key, 23, ControlKind::item, row, L"Cube " + std::to_wstring(member.key));
        item.selected = member.key == session.selected();
    }
    auto viewport = NodeInput::box(0, 0); viewport.width = viewport.height = Axis::expand();
    add(control::viewport, 20, ControlKind::viewport, viewport);
    auto splitter = NodeInput::box(6, 0); splitter.height = Axis::expand();
    auto& divider = add(control::splitter, 20, ControlKind::splitter, splitter);
    divider.value = -parameter_width; divider.minimum = -440; divider.maximum = -220;
    divider.drag_step = divider.fine_step = 1;
    auto parameters = NodeInput::column(10); parameters.width = Axis::fixed(parameter_width); parameters.height = Axis::expand();
    parameters.padding = {16, 12, 16, 12};
    add(50, 20, ControlKind::container, parameters).role = ui::Role::panel;
    add(51, 50, ControlKind::label, heading, L"PARAMETERS").role = ui::Role::caption;
    add(52, 50, ControlKind::label, heading, session.selected() ? L"Cube " + std::to_wstring(session.selected()) : L"No selection").role = ui::Role::title;
    add(53, 50, ControlKind::label, heading, L"Position  /  metres");
    scene::Vec3 position{};
    if (session.selected()) {
        const auto value = session.scene().position(session.selected());
        if (!value) throw std::runtime_error(value.error().message);
        position = *value;
    }
    const std::array values{position.x, position.y, position.z};
    constexpr std::array names{L"X", L"Y", L"Z"};
    for (std::size_t axis = 0; axis < values.size(); ++axis) {
        auto row = NodeInput::row(8); row.height = Axis::fixed(34);
        const auto row_key = static_cast<ui::ControlKey>(70 + axis);
        add(row_key, 50, ControlKind::container, row);
        add(80 + axis, row_key, ControlKind::label, NodeInput::box(20, 34), names[axis]);
        auto number = NodeInput::box(0, 34); number.width = Axis::expand();
        auto& input = add(control::position(session.selected(), axis), row_key, ControlKind::number, number);
        input.value = values[axis]; input.enabled = session.selected() != 0;
        input.minimum = position_parameters[axis].minimum; input.maximum = position_parameters[axis].maximum;
        input.drag_step = position_parameters[axis].drag_step;
    }
    add(90, 50, ControlKind::label, heading, L"Drag values to adjust").role = ui::Role::caption;
    add(91, 50, ControlKind::label, heading, L"Click to type / Esc to cancel").role = ui::Role::caption;
    add(92, 50, ControlKind::label, heading, L"Right drag: orbit / Wheel: zoom").role = ui::Role::caption;
    auto footer = NodeInput::row(); footer.height = Axis::fixed(28); footer.padding = {12, 0, 12, 0};
    add(100, 1, ControlKind::container, footer).role = ui::Role::panel;
    auto status = NodeInput::box(0, 28); status.width = Axis::expand();
    add(101, 100, ControlKind::label, status, this->status + L"  |  " + std::to_wstring(session.scene().members().size()) + L" objects").role = ui::Role::caption;
    return result;
}
}
