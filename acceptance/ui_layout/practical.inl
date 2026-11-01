void shared_columns_structure_and_intrinsic() {
    LayoutDocument d(nullptr, nullptr, {20, 6});
    auto spec = NodeInput::table({Axis::content(), Axis::content()}, 10, 3);
    spec.width = Axis::content();
    const auto table = accepted(d.create(d.root(), spec));
    const auto r1 = accepted(d.create(table, NodeInput::row()));
    const auto r2 = accepted(d.create(table, NodeInput::row()));
    accepted(d.create(r1, NodeInput::box(40, 10)));
    accepted(d.create(r1, NodeInput::box(100, 20)));
    accepted(d.create(r2, NodeInput::box(70, 30)));
    const auto last = accepted(d.create(r2, NodeInput::box(20, 10)));
    const auto view = accepted(d.compute({500, 200}, 1));
    require(box(view, table).width == 180 && box(view, table).height == 53 && box(view, last).x == 80,
            "table intrinsic shared maximum");
}

void wrap_flow_and_repeated_grid() {
    LayoutDocument d(nullptr, nullptr, {12, 3});
    auto spec = NodeInput::grid(60, 10, 5);
    spec.line_align = 1;
    const auto flow = accepted(d.create(d.root(), spec));
    std::array<LayoutNodeId, 5> items;
    const float heights[]{10, 20, 15, 30, 10};
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto input = NodeInput::box(1, heights[i]);
        input.width = Axis::expand();
        items[i] = accepted(d.create(flow, input));
    }
    const auto view = accepted(d.compute({199, 200}, 1));
    require(box(view, items[0]).width == 94.5f && box(view, items[4]).y == 60 && box(view, items[4]).x == 104.5f &&
                box(view, flow).height == 70,
            "grid width threshold");
}
