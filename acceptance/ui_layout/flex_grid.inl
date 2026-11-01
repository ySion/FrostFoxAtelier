void flex_allocation_and_distribution() {
    LayoutDocument document(nullptr, nullptr, {16, 2});
    RootInput root{Kind::row};
    root.flow = FlexSpec{};
    accepted(document.set_root(root));
    auto first = NodeInput::box(1, 10);
    auto second = first;
    first.width = Axis::flex(Basis::fixed(200), 1, 2);
    second.width = Axis::flex(Basis::fixed(100), 1, 1);
    const auto a = accepted(document.create(document.root(), first));
    const auto b = accepted(document.create(document.root(), second));
    auto view = accepted(document.compute({240, 40}, 1));
    require(close(box(view, a).width, 152) && close(box(view, b).width, 88),
            "shrink weights were not scaled by the inner basis");

    first.width = second.width = Axis::fixed(20);
    accepted(document.update(a, first));
    accepted(document.update(b, second));
    root.gap = 10;
    root.flow->align_items = Alignment::fit(1);
    root.flow->justify = Distribution::space_evenly;
    accepted(document.set_root(root));
    view = accepted(document.compute({100, 40}, 1));
    require(close(box(view, a).x, 50.f / 3) && close(box(view, b).x, 30.f + 100.f / 3) && close(box(view, a).y, 30),
            "Flex main distribution or inherited cross alignment");
}

void flex_wrapping_and_alignment() {
    LayoutDocument document(nullptr, nullptr, {16, 3});
    auto input = NodeInput::row(10);
    input.height = Axis::fixed(120);
    input.cross_gap = 10;
    input.flow = FlexSpec{.wrap = Wrap::wrap_reverse, .reverse = true};
    const auto panel = accepted(document.create(document.root(), input));
    std::array<LayoutNodeId, 4> items;
    for (auto& item : items)
        item = accepted(document.create(panel, NodeInput::box(70, 20)));
    const auto view = accepted(document.compute({150, 140}, 1));
    require(close(box(view, items[0]).x, 80) && close(box(view, items[0]).y, 100) && close(box(view, items[2]).y, 70),
            "main/cross reverse changed line membership or offsets");
}

void grid_tracks_spans_and_placement() {
    LayoutDocument document(nullptr, nullptr, {32, 4});
    auto grid = NodeInput::grid(GridSpec{.columns = {Axis::fixed(60), Axis::expand(), Axis::expand(2)},
                                         .rows = {Axis::fixed(30), Axis::content()},
                                         .column_gap = 10,
                                         .row_gap = 6});
    const auto parent = accepted(document.create(document.root(), grid));
    std::array<LayoutNodeId, 3> cells;
    for (std::uint32_t column = 0; column < 3; ++column) {
        NodeInput input;
        input.width = input.height = Axis::expand();
        input.cell = {.row = 0, .column = column};
        cells[column] = accepted(document.create(parent, input));
    }
    NodeInput footer;
    footer.height = Axis::fixed(20);
    footer.cell = {.row = 1, .column = 0, .column_span = 3};
    const auto span = accepted(document.create(parent, footer));
    NodeInput across;
    across.width = across.height = Axis::expand();
    across.cell = {.row = 0, .column = 0, .row_span = 2, .column_span = 3};
    const auto two_axes = accepted(document.create(parent, across));
    auto view = accepted(document.compute({300, 160}, 1));
    require(close(box(view, cells[0]).width, 60) && close(box(view, cells[1]).width, 220.f / 3) &&
                close(box(view, cells[2]).width, 440.f / 3) && close(box(view, cells[2]).x, 80 + 220.f / 3),
            "Grid fixed/weighted tracks or gaps");
    require(close(box(view, span).width, 300) && close(box(view, span).y, 36) && close(box(view, parent).height, 56),
            "Grid column span or natural row heights");
    require(close(box(view, two_axes).width, 300) && close(box(view, two_axes).height, 56),
            "Grid row span did not include intervening track gaps");

    LayoutDocument spanning(nullptr, nullptr, {16, 3});
    auto content_grid = NodeInput::grid(GridSpec{.columns = {Axis::content(), Axis::content(), Axis::content()},
                                                 .rows = {Axis::content(), Axis::content()}});
    content_grid.width = Axis::content();
    const auto content_parent = accepted(spanning.create(spanning.root(), content_grid));
    auto left = NodeInput::box(100, 10);
    left.cell = {.row = 0, .column = 0, .column_span = 2};
    auto right = left;
    right.cell.column = 1;
    accepted(spanning.create(content_parent, left));
    accepted(spanning.create(content_parent, right));
    std::array<LayoutNodeId, 3> probes;
    for (std::uint32_t column = 0; column < 3; ++column) {
        NodeInput probe;
        probe.height = Axis::fixed(1);
        probe.cell = {.row = 1, .column = column};
        probes[column] = accepted(spanning.create(content_parent, probe));
    }
    view = accepted(spanning.compute({500, 100}, 1));
    for (auto probe : probes)
        require(close(box(view, probe).width, 50), "same-span batch did not share its frozen starting widths");
    require(close(box(view, content_parent).width, 150), "span policy silently minimized overlapping constraints");
}
