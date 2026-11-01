void grid_collision_boundaries() {
    LayoutDocument document(nullptr, nullptr, {6, 2});
    GridSpec spec{.columns = std::vector<Axis>(130, Axis::fixed(1)),
                  .rows = std::vector<Axis>(130, Axis::fixed(1)),
                  .auto_flow = AutoFlow::row};
    const auto parent = accepted(document.create(document.root(), NodeInput::grid(spec)));
    auto obstacle = NodeInput::box(1, 1);
    obstacle.cell = {.row = 0, .column = 0, .column_span = 64};
    accepted(document.create(parent, obstacle));
    auto item = NodeInput::box(1, 1);
    item.cell.row_span = item.cell.column_span = 2;
    const auto crossing = accepted(document.create(parent, item));
    item.cell = {.column = 0};
    const auto locked = accepted(document.create(parent, item));
    const auto view = accepted(document.compute({200, 200}, 1));
    require(close(box(view, crossing).x, 64) && close(box(view, crossing).y, 0) && close(box(view, locked).y, 1),
            "collision jump crossed a free position or a 64-bit occupancy boundary");
}
