// Real consumers of the shared LayoutDocument Interface; no window or mock provider.
void flex_allocation_and_distribution() {
    LayoutDocument document(nullptr, nullptr, {16, 2});
    RootInput root{Kind::row};
    root.flow = FlexSpec{};
    accepted(document.set_root(root));
    std::array<LayoutNodeId, 3> children;
    for (std::size_t i = 0; i < children.size(); ++i) {
        auto input = NodeInput::box(1, 10);
        input.width = Axis::expand(1, 0,
                                   i == 0   ? std::optional<float>(120)
                                   : i == 2 ? std::optional<float>(30)
                                            : std::nullopt);
        children[i] = accepted(document.create(document.root(), input));
    }
    auto view = accepted(document.compute({300, 40}, 1));
    require(close(box(view, children[0]).width, 120) && close(box(view, children[1]).width, 150) &&
                close(box(view, children[2]).width, 30),
            "Flex changed Expand saturation 120/150/30");
    accepted(document.remove(children[2]));
    auto first = NodeInput::box(1, 10), second = first;
    first.width = Axis::flex(Basis::fixed(200), 1, 1);
    second.width = Axis::flex(Basis::fixed(100), 1, 1);
    accepted(document.update(children[0], first));
    accepted(document.update(children[1], second));
    view = accepted(document.compute({240, 40}, 1));
    require(close(box(view, children[0]).width, 160) && close(box(view, children[1]).width, 80),
            "native shrink weights did not produce 160/80");
    first.width.minimum = 180;
    accepted(document.update(children[0], first));
    view = accepted(document.compute({240, 40}, 1));
    require(close(box(view, children[0]).width, 180) && close(box(view, children[1]).width, 60),
            "shrink did not redistribute after reaching a hard minimum");
    first.width.minimum = 0;
    first.width.shrink_weight = 2;
    accepted(document.update(children[0], first));
    view = accepted(document.compute({240, 40}, 1));
    require(close(box(view, children[0]).width, 152) && close(box(view, children[1]).width, 88),
            "shrink weights were not scaled by the inner basis");
    first.width = second.width = Axis::flex(Basis::fixed(0), .25f, 0);
    accepted(document.update(children[0], first));
    accepted(document.update(children[1], second));
    view = accepted(document.compute({100, 40}, 1));
    require(close(box(view, children[0]).width, 50) && close(box(view, children[1]).width, 50),
            "native fractional weights adopted CSS partial-fill semantics");

    first.width = second.width = Axis::fixed(20);
    accepted(document.update(children[0], first));
    accepted(document.update(children[1], second));
    root.gap = 10;
    root.flow->align_items = Alignment::fit(1);
    struct Placement {
        Distribution distribution;
        float first, second;
    };
    const std::array placements{Placement{Distribution::start, 0, 30},
                                Placement{Distribution::center, 25, 55},
                                Placement{Distribution::end, 50, 80},
                                Placement{Distribution::space_between, 0, 80},
                                Placement{Distribution::space_around, 12.5f, 67.5f},
                                Placement{Distribution::space_evenly, 50.f / 3, 30.f + 100.f / 3}};
    for (const auto& expected : placements) {
        root.flow->justify = expected.distribution;
        accepted(document.set_root(root));
        view = accepted(document.compute({100, 40}, 1));
        require(close(box(view, children[0]).x, expected.first) && close(box(view, children[1]).x, expected.second) &&
                    close(box(view, children[0]).y, 30),
                "Flex main distribution or inherited cross alignment");
    }
    root.flow->justify = Distribution::start;
    root.flow->reverse = true;
    accepted(document.set_root(root));
    view = accepted(document.compute({100, 40}, 1));
    require(close(box(view, children[0]).x, 80) && close(box(view, children[1]).x, 50) &&
                view.boxes[1].node == children[0] && view.boxes[2].node == children[1],
            "reverse reordered publication identities");
    root.flow->reverse = false;
    root.flow->justify = Distribution::center;
    accepted(document.set_root(root));
    view = accepted(document.compute({30, 40}, 1));
    require(close(box(view, children[0]).x, 0) && close(box(view, children[1]).x, 30),
            "negative free space did not retain start alignment and hard sizes");
    LayoutDocument known(nullptr, nullptr, {3, 1});
    RootInput fixed_root{Kind::row};
    fixed_root.flow = FlexSpec{};
    accepted(known.set_root(fixed_root));
    auto fixed_text = NodeInput::measured(999);
    fixed_text.width = Axis::flex(Basis::fixed(0));
    fixed_text.height = Axis::expand();
    const auto fixed_id = accepted(known.create(known.root(), fixed_text));
    view = accepted(known.compute({100, 30}, 1));
    require(box(view, fixed_id).width == 100 && box(view, fixed_id).height == 30 && known.stats().measure_calls == 0,
            "known Flex dimensions requested unused text metrics");
}

void flex_wrapping_and_alignment() {
    LayoutDocument document(nullptr, nullptr, {16, 3});
    auto input = NodeInput::row(10);
    input.height = Axis::fixed(120);
    input.cross_gap = 10;
    input.flow = FlexSpec{.wrap = Wrap::wrap};
    const auto panel = accepted(document.create(document.root(), input));
    std::array<LayoutNodeId, 4> items;
    for (auto& item : items)
        item = accepted(document.create(panel, NodeInput::box(70, 20)));
    auto view = accepted(document.compute({150, 140}, 1));
    require(close(box(view, items[0]).x, 0) && close(box(view, items[1]).x, 80) && close(box(view, items[2]).y, 30),
            "Flex basis packing or inter-line gap");
    struct Lines {
        Distribution distribution;
        float first, second;
    };
    const std::array lines{Lines{Distribution::start, 0, 30},
                           Lines{Distribution::center, 35, 65},
                           Lines{Distribution::end, 70, 100},
                           Lines{Distribution::space_between, 0, 100},
                           Lines{Distribution::space_around, 17.5f, 82.5f},
                           Lines{Distribution::space_evenly, 70.f / 3, 30.f + 140.f / 3}};
    for (const auto& expected : lines) {
        input.flow->align_lines = expected.distribution;
        accepted(document.update(panel, input));
        view = accepted(document.compute({150, 140}, 1));
        require(close(box(view, items[0]).y, expected.first) && close(box(view, items[2]).y, expected.second),
                "Flex inter-line distribution");
    }
    input.flow->align_lines = Distribution::stretch;
    accepted(document.update(panel, input));
    auto stretching = NodeInput::box(70, 1);
    stretching.height = Axis::expand();
    accepted(document.update(items[3], stretching));
    view = accepted(document.compute({150, 140}, 1));
    require(close(box(view, items[2]).y, 65) && close(box(view, items[3]).height, 55),
            "stretched lines did not allocate their cross-axis slot");
    stretching.vertical_alignment = Alignment::fit(1);
    accepted(document.update(items[3], stretching));
    // This empty box has no natural demand; explicit cross alignment overrides the parent.
    view = accepted(document.compute({150, 140}, 1));
    require(close(box(view, items[3]).height, 0) && close(box(view, items[3]).y, 120),
            "child cross-axis override did not fit its independent natural height");
    accepted(document.update(items[3], NodeInput::box(70, 20)));
    input.flow->align_lines = Distribution::start;
    input.flow->wrap = Wrap::wrap_reverse;
    input.flow->reverse = true;
    accepted(document.update(panel, input));
    view = accepted(document.compute({150, 140}, 1));
    require(close(box(view, items[0]).x, 80) && close(box(view, items[0]).y, 100) && close(box(view, items[2]).y, 70),
            "main/cross reverse changed line membership or offsets");

    TextProvider column_provider;
    column_provider.content[1] = {L"Column wrapping depends on this paragraph's available width and natural height.",
                                  16, 1};
    LayoutDocument columns(TextProvider::measure, &column_provider, {8, 3});
    auto vertical = NodeInput::column(10);
    vertical.width = Axis::fixed(160);
    vertical.height = Axis::fixed(70);
    vertical.cross_gap = 12;
    vertical.flow = FlexSpec{.wrap = Wrap::wrap};
    const auto holder = accepted(columns.create(columns.root(), vertical));
    std::array<LayoutNodeId, 3> cells;
    for (auto& cell : cells)
        cell = accepted(columns.create(holder, NodeInput::box(40, 30)));
    view = accepted(columns.compute({160, 100}, 1));
    require(close(box(view, cells[1]).y, 40) && close(box(view, cells[2]).x, 52) && close(box(view, cells[2]).y, 0),
            "definite-height column wrapping failed");
    const auto held = columns.snapshot();
    auto cyclic = NodeInput::measured(1);
    cyclic.width = Axis::expand();
    accepted(columns.update(cells[2], cyclic));
    rejected(columns.compute({160, 100}, 1), ErrorCode::unsupported_dependency);
    require(columns.view().publication == held.publication,
            "unsupported column-wrap dependency published partial geometry");
}

float direct_first_baseline(TextProvider& provider, std::uint64_t key, float width) {
    const auto layout = provider.create(provider.content.at(key));
    provider.metrics(layout.Get(), width);
    UINT32 count{};
    static_cast<void>(layout->GetLineMetrics(nullptr, 0, &count));
    require(count != 0, "real paragraph has no first line");
    std::vector<DWRITE_LINE_METRICS> lines(count);
    hr(layout->GetLineMetrics(lines.data(), count, &count));
    return lines.front().baseline;
}

void flex_real_text_baseline() {
    TextProvider provider;
    provider.content[1] = {L"Property", 12, 1};
    provider.content[2] = {L"Mixed font size", 28, 1};
    LayoutDocument document(TextProvider::measure, &provider, {8, 3});
    RootInput root{Kind::row};
    root.gap = 8;
    root.flow = FlexSpec{.align_items = Alignment::baseline()};
    accepted(document.set_root(root));
    auto small_input = NodeInput::measured(1);
    small_input.width = Axis::fixed(130);
    small_input.height = Axis::fixed(60);
    small_input.padding = {4, 3, 2, 5};
    auto large_input = small_input;
    large_input.content_key = 2;
    large_input.padding.top = 7;
    const auto a = accepted(document.create(document.root(), small_input));
    const auto b = accepted(document.create(document.root(), large_input));
    const auto fallback = accepted(document.create(document.root(), NodeInput::box(20, 30)));
    auto view = accepted(document.compute({340, 120}, provider.epoch));
    require(provider.calls >= 2, "fixed text boxes skipped required baseline measurement");
    auto check = [&] {
        const auto current = document.view();
        const float baseline_a = box(current, a).y + small_input.padding.top + direct_first_baseline(provider, 1, 124);
        const float baseline_b = box(current, b).y + large_input.padding.top + direct_first_baseline(provider, 2, 124);
        require(close(baseline_a, baseline_b) && close(baseline_b, box(current, fallback).y + 30),
                "real first baseline, padding or bottom-edge fallback disagrees");
    };
    check();
    const auto old = TextFrame::capture(document, provider);
    const auto before = document.stats();
    provider.content[1] = {L"Updated property", 22, 2};
    small_input.content_revision = 2;
    accepted(document.update(a, small_input));
    provider.fail_at = provider.calls + 1;
    rejected(document.compute({340, 120}, provider.epoch), ErrorCode::measurement_failed);
    require(document.view().publication == old->geometry.publication, "baseline failure replaced published geometry");
    old->check();
    provider.fail_at = 0;
    view = accepted(document.compute({340, 120}, provider.epoch));
    require(document.stats().measure_calls > before.measure_calls &&
                document.stats().geometry_reuses == before.geometry_reuses &&
                view.publication > old->geometry.publication,
            "fixed-size baseline content edit reused stale geometry");
    check();
    old->check();
    auto current = TextFrame::capture(document, provider);
    current->check();

    LayoutDocument content_basis(TextProvider::measure, &provider, {4, 2});
    RootInput natural_root{Kind::row};
    natural_root.gap = 4;
    natural_root.flow = FlexSpec{};
    accepted(content_basis.set_root(natural_root));
    auto natural_a = NodeInput::measured(1, 2), natural_b = NodeInput::measured(2);
    natural_a.width = natural_b.width = Axis::flex(Basis::content(), 1, 1, 20);
    const auto natural_id_a = accepted(content_basis.create(content_basis.root(), natural_a));
    const auto natural_id_b = accepted(content_basis.create(content_basis.root(), natural_b));
    const auto width_a = provider.fresh(1, {}).width, width_b = provider.fresh(2, {}).width;
    const auto available = width_a + width_b + 104;
    view = accepted(content_basis.compute({available, 120}, provider.epoch));
    require(close(box(view, natural_id_a).width, width_a + 50) && close(box(view, natural_id_b).width, width_b + 50),
            "content basis was replaced by zero-basis proportional sizing");

    LayoutDocument fitted(TextProvider::measure, &provider, {2, 1});
    RootInput fitted_root{Kind::row};
    fitted_root.flow = FlexSpec{};
    accepted(fitted.set_root(fitted_root));
    auto fitted_text = NodeInput::measured(1, 2);
    fitted_text.width = Axis::flex(Basis::fixed(500), 0, 0);
    fitted_text.horizontal_alignment = Alignment::fit(.5f);
    const auto fitted_id = accepted(fitted.create(fitted.root(), fitted_text));
    view = accepted(fitted.compute({600, 300}, provider.epoch));
    require(close(box(view, fitted_id).width, width_a) && close(box(view, fitted_id).x, (500 - width_a) / 2),
            "fixed Flex basis masked the fitted natural content width");
    fitted_root.kind = Kind::column;
    accepted(fitted.set_root(fitted_root));
    fitted_text.width = Axis::fixed(130);
    fitted_text.horizontal_alignment = {};
    fitted_text.height = Axis::flex(Basis::fixed(200), 0, 0);
    fitted_text.vertical_alignment = Alignment::fit(.5f);
    accepted(fitted.update(fitted_id, fitted_text));
    view = accepted(fitted.compute({600, 300}, provider.epoch));
    const auto fitted_height = provider.fresh(1, 130).height;
    require(close(box(view, fitted_id).height, fitted_height) &&
                close(box(view, fitted_id).y, (200 - fitted_height) / 2),
            "fixed Flex basis masked the fitted natural content height");

    LayoutDocument propagated(TextProvider::measure, &provider, {5, 3});
    accepted(propagated.set_root(root));
    auto nested_row = NodeInput::row();
    nested_row.width = Axis::content();
    nested_row.flow = FlexSpec{.align_items = Alignment::baseline()};
    const auto group = accepted(propagated.create(propagated.root(), nested_row));
    auto child = NodeInput::measured(1, 2);
    child.width = Axis::content();
    const auto child_id = accepted(propagated.create(group, child));
    auto peer = NodeInput::measured(2);
    peer.width = Axis::content();
    const auto peer_id = accepted(propagated.create(propagated.root(), peer));
    view = accepted(propagated.compute({700, 120}, provider.epoch));
    require(close(box(view, group).y + box(view, child_id).y +
                      direct_first_baseline(provider, 1, box(view, child_id).width),
                  box(view, peer_id).y + direct_first_baseline(provider, 2, box(view, peer_id).width)),
            "nested container did not propagate its first participating baseline");

    for (const bool flex_column : {false, true}) {
        for (const bool fitted_box : {false, true}) {
            LayoutDocument bottoms(nullptr, nullptr, {4, 2});
            accepted(bottoms.set_root(root));
            auto column = NodeInput::column();
            column.width = Axis::fixed(50);
            column.height = Axis::fixed(100);
            if (flex_column)
                column.flow = FlexSpec{};
            const auto column_id = accepted(bottoms.create(bottoms.root(), column));
            auto expanding = NodeInput::box(20, 20);
            expanding.height = fitted_box ? Axis::expand(1, 20) : Axis::expand();
            if (fitted_box)
                expanding.vertical_alignment = Alignment::fit(.5f);
            const auto expanding_id = accepted(bottoms.create(column_id, expanding));
            const auto peer_box = accepted(bottoms.create(bottoms.root(), NodeInput::box(20, 30)));
            const auto result = accepted(bottoms.compute({200, 200}, 1));
            const auto& child_box = box(result, expanding_id);
            require(close(box(result, column_id).y + child_box.y + child_box.height,
                          box(result, peer_box).y + box(result, peer_box).height),
                    "nested column exported natural bottom instead of arranged child bottom");
            require(close(child_box.height, fitted_box ? 20.f : 100.f) && close(child_box.y, fitted_box ? 40.f : 0.f),
                    "baseline propagation changed the column child's size or placement");
        }

        LayoutDocument centered(TextProvider::measure, &provider, {4, 2});
        accepted(centered.set_root(root));
        auto column = NodeInput::column();
        column.width = Axis::fixed(130);
        column.height = Axis::fixed(100);
        if (flex_column)
            column.flow = FlexSpec{};
        const auto column_id = accepted(centered.create(centered.root(), column));
        auto centered_text = NodeInput::measured(1, 2);
        centered_text.height = Axis::expand();
        centered_text.vertical_alignment = Alignment::fit(.5f);
        const auto centered_id = accepted(centered.create(column_id, centered_text));
        auto peer_text = NodeInput::measured(2);
        peer_text.width = Axis::fixed(130);
        const auto centered_peer = accepted(centered.create(centered.root(), peer_text));
        const auto result = accepted(centered.compute({400, 200}, provider.epoch));
        const auto& text_box = box(result, centered_id);
        require(text_box.y > 0 &&
                    close(box(result, column_id).y + text_box.y + direct_first_baseline(provider, 1, text_box.width),
                          box(result, centered_peer).y +
                              direct_first_baseline(provider, 2, box(result, centered_peer).width)),
                "centered real text lost its local offset during column baseline propagation");
        column.height = Axis::expand();
        accepted(centered.update(column_id, column));
        rejected(centered.compute({400, 200}, provider.epoch), ErrorCode::unsupported_dependency);
        require(centered.view().publication == result.publication,
                "indefinite centered baseline published partial child placement");
        column.height = Axis::fixed(100);
        accepted(centered.update(column_id, column));
        require(accepted(centered.compute({400, 200}, provider.epoch)).publication > result.publication,
                "definite baseline failed to recover after an unsupported dependency");
    }

    LayoutDocument wrapped(TextProvider::measure, &provider, {5, 2});
    accepted(wrapped.set_root(root));
    auto wrapping = NodeInput::wrap();
    wrapping.width = Axis::fixed(200);
    const auto wrapping_id = accepted(wrapped.create(wrapped.root(), wrapping));
    auto wrapped_text = NodeInput::measured(1, 2);
    wrapped_text.width = Axis::fixed(130);
    wrapped_text.vertical_alignment = Alignment::fit(.5f);
    const auto wrapped_id = accepted(wrapped.create(wrapping_id, wrapped_text));
    accepted(wrapped.create(wrapping_id, NodeInput::box(30, 100)));
    auto wrapped_peer_text = NodeInput::measured(2);
    wrapped_peer_text.width = Axis::fixed(130);
    const auto wrapped_peer = accepted(wrapped.create(wrapped.root(), wrapped_peer_text));
    view = accepted(wrapped.compute({400, 200}, provider.epoch));
    require(box(view, wrapped_id).y > 0 &&
                close(box(view, wrapping_id).y + box(view, wrapped_id).y +
                          direct_first_baseline(provider, 1, box(view, wrapped_id).width),
                      box(view, wrapped_peer).y + direct_first_baseline(provider, 2, box(view, wrapped_peer).width)),
            "wrap exported a child baseline without its line-local alignment offset");
}

void grid_tracks_spans_and_placement() {
    const auto repeated = NodeInput::grid(80, 4, 6);
    require(repeated.kind == Kind::wrap && repeated.item_width == 80 && repeated.stretch_items && !repeated.grid_spec,
            "legacy grid(float) changed its observable description");
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
    const auto left_id = accepted(spanning.create(content_parent, left));
    const auto right_id = accepted(spanning.create(content_parent, right));
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
    const auto before = spanning.snapshot();
    accepted(spanning.move(left_id, content_parent));
    view = accepted(spanning.compute({500, 100}, 1));
    for (auto id : {left_id, right_id, probes[0], probes[1], probes[2]}) {
        const auto& a = *before.find(id);
        const auto& b = box(view, id);
        require(close(a.x, b.x) && close(a.y, b.y) && close(a.width, b.width) && close(a.height, b.height),
                "same-span content tracks depend on child insertion order");
    }

    for (auto direction : {AutoFlow::row, AutoFlow::column}) {
        LayoutDocument placed(nullptr, nullptr, {12, 3});
        auto spec = GridSpec{.columns = {Axis::fixed(40), Axis::fixed(40)},
                             .rows = {Axis::fixed(20), Axis::fixed(20)},
                             .auto_flow = direction,
                             .implicit_column = Axis::fixed(40),
                             .implicit_row = Axis::fixed(20)};
        const auto holder = accepted(placed.create(placed.root(), NodeInput::grid(spec)));
        auto occupied = NodeInput::box(10, 10);
        occupied.cell = {.row = 0, .column = 0};
        accepted(placed.create(holder, occupied));
        std::array<LayoutNodeId, 3> automatic;
        for (auto& id : automatic)
            id = accepted(placed.create(holder, NodeInput::box(10, 10)));
        const auto positions = accepted(placed.compute({200, 100}, 1));
        const bool row = direction == AutoFlow::row;
        require(close(box(positions, automatic[0]).x, row ? 40.f : 0.f) &&
                    close(box(positions, automatic[0]).y, row ? 0.f : 20.f) &&
                    close(box(positions, automatic[1]).x, row ? 0.f : 40.f) &&
                    close(box(positions, automatic[1]).y, row ? 20.f : 0.f) &&
                    close(box(positions, automatic[2]).x, 40) && close(box(positions, automatic[2]).y, 20),
                "Grid automatic row/column placement did not avoid occupied cells");
    }
    LayoutDocument partial(nullptr, nullptr, {8, 3});
    const auto partial_parent = accepted(partial.create(
        partial.root(), NodeInput::grid(GridSpec{.columns = {Axis::fixed(40), Axis::fixed(40), Axis::fixed(40)},
                                                 .rows = {Axis::fixed(20), Axis::fixed(20), Axis::fixed(20)}})));
    auto row_only = NodeInput::box(10, 10);
    row_only.cell.row = 2;
    auto column_only = NodeInput::box(10, 10);
    column_only.cell.column = 2;
    const auto row_id = accepted(partial.create(partial_parent, row_only));
    const auto column_id = accepted(partial.create(partial_parent, column_only));
    view = accepted(partial.compute({120, 100}, 1));
    require(close(box(view, row_id).y, 40) && close(box(view, column_id).x, 80),
            "single-axis Grid placement ignored its definite coordinate");
}

void grid_limits_and_nested_content() {
    LayoutDocument limited(nullptr, nullptr, {12, 3, 4, 16});
    const auto parent =
        accepted(limited.create(limited.root(), NodeInput::grid(GridSpec{.columns = {Axis::fixed(20), Axis::fixed(20)},
                                                                         .rows = {Axis::fixed(20), Axis::fixed(20)}})));
    auto safe = NodeInput::box(10, 10);
    safe.cell = {.row = 0, .column = 0};
    accepted(limited.create(parent, safe));
    accepted(limited.compute({100, 100}, 1));
    const auto held = limited.snapshot();
    auto excessive = safe;
    excessive.cell.column_span = 5;
    const auto excessive_id = accepted(limited.create(parent, excessive));
    rejected(limited.compute({100, 100}, 1), ErrorCode::capacity_exceeded);
    require(limited.view().publication == held.publication && !limited.view().find(excessive_id),
            "track capacity failure changed old publication");
    accepted(limited.remove(excessive_id));
    excessive.cell.column = std::numeric_limits<std::uint32_t>::max();
    excessive.cell.column_span = 2;
    const auto overflow_id = accepted(limited.create(parent, excessive));
    rejected(limited.compute({100, 100}, 1), ErrorCode::capacity_exceeded);
    accepted(limited.remove(overflow_id));
    accepted(limited.compute({100, 100}, 1));
    LayoutDocument occupied(nullptr, nullptr, {8, 3, 4, 4});
    const auto occupied_parent = accepted(
        occupied.create(occupied.root(), NodeInput::grid(GridSpec{.columns = {Axis::fixed(10), Axis::fixed(10)},
                                                                  .rows = {Axis::fixed(10), Axis::fixed(10)}})));
    accepted(occupied.create(occupied_parent, NodeInput::box(1, 1)));
    accepted(occupied.compute({100, 100}, 1));
    const auto old = occupied.snapshot();
    auto larger = accepted(occupied.input(occupied_parent));
    larger.grid_spec->rows.push_back(Axis::fixed(10));
    accepted(occupied.update(occupied_parent, larger));
    rejected(occupied.compute({100, 100}, 1), ErrorCode::capacity_exceeded);
    require(occupied.view().publication == old.publication, "occupancy capacity failure replaced geometry");

    TextProvider provider;
    provider.content[1] = {L"A wrapping property value must be measured after its spanning Grid width is known.", 16,
                           1};
    LayoutDocument nested(TextProvider::measure, &provider, {16, 5});
    const auto outer =
        accepted(nested.create(nested.root(), NodeInput::grid(GridSpec{.columns = {Axis::fixed(30), Axis::expand()},
                                                                       .rows = {Axis::content()},
                                                                       .column_gap = 10})));
    auto inner_input = NodeInput::grid(GridSpec{.columns = {Axis::fixed(40), Axis::expand()},
                                                .rows = {Axis::content(), Axis::content()},
                                                .column_gap = 5,
                                                .row_gap = 7});
    inner_input.cell = {.row = 0, .column = 1};
    const auto inner = accepted(nested.create(outer, inner_input));
    auto field = NodeInput::measured(1);
    field.cell = {.row = 0, .column = 0, .column_span = 2};
    const auto text_id = accepted(nested.create(inner, field));
    auto badge = NodeInput::box(20, 12);
    badge.cell = {.row = 1, .column = 1};
    accepted(nested.create(inner, badge));
    auto result = accepted(nested.compute({220, 300}, provider.epoch));
    const auto natural = provider.fresh(1, 180);
    require(close(box(result, inner).width, 180) && close(box(result, text_id).width, 180) &&
                close(box(result, text_id).height, natural.height) &&
                close(box(result, inner).height, natural.height + 19),
            "nested Grid did not resolve spanned width before real natural height");
    const auto frame = TextFrame::capture(nested, provider);
    provider.content[1] = {L"Changed value with another line of explanatory text and a retained old frame.", 20, 2};
    field.content_revision = 2;
    accepted(nested.update(text_id, field));
    provider.fail_at = provider.calls + 1;
    rejected(nested.compute({140, 300}, provider.epoch), ErrorCode::measurement_failed);
    require(nested.view().publication == frame->geometry.publication, "nested Grid failure replaced old geometry");
    frame->check();
    provider.fail_at = 0;
    result = accepted(nested.compute({140, 300}, provider.epoch));
    require(close(box(result, text_id).width, 100) && close(box(result, text_id).height, provider.fresh(1, 100).height),
            "nested Grid recovery used stale constrained metrics");
    frame->check();
    TextFrame::capture(nested, provider)->check();
}

struct FlexGridEditorScene {
    TextProvider provider;
    LayoutDocument document{TextProvider::measure, &provider, {256, 10, 128, 8192}};
    ComputeCosts compute_costs;
    LayoutNodeId shell, toolbar, inspector, help, status;
    std::vector<LayoutNodeId> values;
    std::shared_ptr<const TextFrame> published;
    Size viewport{1100, 700};
    FlexGridEditorScene() {
        ComPtr<IDWriteFontCollection> fonts;
        hr(provider.factory->GetSystemFontCollection(fonts.GetAddressOf()));
        UINT32 index{};
        BOOL found{};
        hr(fonts->FindFamilyName(L"Microsoft YaHei UI", &index, &found));
        if (found)
            provider.font_family = L"Microsoft YaHei UI";
        auto shell_input = NodeInput::grid(GridSpec{.columns = {Axis::fixed(160), Axis::expand(), Axis::fixed(300)},
                                                    .rows = {Axis::fixed(38), Axis::expand(), Axis::fixed(24)},
                                                    .column_gap = 8,
                                                    .row_gap = 6});
        shell_input.height = Axis::expand();
        shell = accepted(document.create(document.root(), shell_input));
        auto bar = NodeInput::row(10);
        bar.height = Axis::expand();
        bar.cell = {.row = 0, .column = 0, .column_span = 3};
        bar.flow = FlexSpec{.justify = Distribution::space_between, .align_items = Alignment::baseline()};
        toolbar = accepted(document.create(shell, bar));
        provider.content[1] = {L"Scene", 22, 1};
        provider.content[2] = {L"Search assets and properties", 14, 1};
        provider.content[3] = {L"Build", 16, 1};
        for (std::uint64_t key = 1; key <= 3; ++key) {
            auto item = NodeInput::measured(key);
            item.width = key == 2 ? Axis::flex(Basis::fixed(220), 1, 1, 80) : Axis::content();
            accepted(document.create(toolbar, item));
        }
        auto hierarchy = NodeInput::column(5);
        hierarchy.height = Axis::expand();
        hierarchy.cell = {.row = 1, .column = 0};
        const auto tree = accepted(document.create(shell, hierarchy));
        for (std::uint64_t key = 10; key < 18; ++key) {
            provider.content[key] = {L"Object " + std::to_wstring(key), 14, 1};
            accepted(document.create(tree, NodeInput::measured(key)));
        }
        auto assets = NodeInput::grid(90, 8, 8);
        assets.cell = {.row = 1, .column = 1};
        assets.height = Axis::expand();
        const auto browser = accepted(document.create(shell, assets));
        for (std::uint64_t key = 30; key < 42; ++key) {
            provider.content[key] = {L"Asset " + std::to_wstring(key), 14, 1};
            accepted(document.create(browser, NodeInput::measured(key)));
        }
        auto form = NodeInput::grid(
            GridSpec{.columns = {Axis::content(60, 100), Axis::expand()}, .rows = {}, .column_gap = 8, .row_gap = 6});
        form.cell = {.row = 1, .column = 2};
        inspector = accepted(document.create(shell, form));
        for (std::uint32_t row = 0; row < 6; ++row) {
            const std::uint64_t key = 100 + row * 2;
            provider.content[key] = {L"Property " + std::to_wstring(row), 14, 1};
            provider.content[key + 1] = {L"Editable value with natural wrapping", 16, 1};
            auto label = NodeInput::measured(key);
            label.cell = {.row = row, .column = 0};
            auto value = NodeInput::measured(key + 1);
            value.cell = {.row = row, .column = 1};
            accepted(document.create(inspector, label));
            values.push_back(accepted(document.create(inspector, value)));
        }
        provider.content[200] = {L"Validation help spans the complete property form and wraps at its resolved width.",
                                 13, 1};
        auto explanation = NodeInput::measured(200);
        explanation.cell = {.row = 6, .column = 0, .column_span = 2};
        help = accepted(document.create(inspector, explanation));
        provider.content[201] = {L"Ready", 12, 1};
        auto footer = NodeInput::measured(201);
        footer.height = Axis::fixed(24);
        footer.cell = {.row = 2, .column = 0, .column_span = 3};
        status = accepted(document.create(shell, footer));
    }
    void commit() {
        compute_costs.run(document, provider, viewport);
        if (!published || published->geometry.publication != document.view().publication) {
            auto candidate = TextFrame::capture(document, provider);
            published = std::move(candidate);
        }
    }
    void edit(unsigned row, const wchar_t* value) {
        auto input = accepted(document.input(values.at(row)));
        auto& content = provider.content.at(input.content_key);
        content.text = value;
        ++content.revision;
        input.content_revision = content.revision;
        accepted(document.update(values.at(row), input));
    }
    void check() const {
        require(published != nullptr, "editor has no complete frame");
        published->check();
        const auto view = published->geometry.view();
        require(close(box(view, shell).width, view.viewport.width) &&
                    close(box(view, shell).height, view.viewport.height) &&
                    close(box(view, toolbar).width, view.viewport.width) && close(box(view, inspector).width, 300) &&
                    close(box(view, status).y, view.viewport.height - 24) && close(box(view, help).width, 300),
                "Grid/Flex editor shell, spanning help or status geometry");
        for (auto value : values)
            require(view.find(value) != nullptr, "property identity missing after editor commit");
    }
};

void flex_grid_editor_end_to_end() {
    FlexGridEditorScene scene;
    scene.commit();
    scene.check();
    const auto initial = scene.published;
    const auto stable = scene.values.front();
    const auto before = scene.document.stats();
    scene.commit();
    require(scene.published == initial && scene.document.stats().node_visits == before.node_visits &&
                scene.document.stats().measure_calls == before.measure_calls,
            "static Grid/Flex editor did layout work");
    scene.viewport.width = 820;
    scene.commit();
    scene.check();
    require(scene.published->geometry.find(stable) != nullptr, "resizing replaced property identity");
    initial->check();
    scene.edit(0, L"A changed visible property with enough content to grow across several lines.");
    scene.commit();
    scene.check();
    auto explanation = accepted(scene.document.input(scene.help));
    explanation.collapsed = true;
    accepted(scene.document.update(scene.help, explanation));
    scene.commit();
    scene.published->check();
    require(!scene.published->geometry.find(scene.help), "collapsed spanning help still participates in Grid");
    explanation.collapsed = false;
    accepted(scene.document.update(scene.help, explanation));
    scene.commit();
    scene.check();
    const auto held = scene.published;
    scene.viewport.width = 1000;
    scene.provider.scale = 1.15f;
    ++scene.provider.epoch;
    scene.provider.fail_at = scene.provider.calls + 3;
    bool failed = false;
    try {
        scene.commit();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed && scene.published == held, "partial Grid/Flex editor frame escaped");
    held->check();
    scene.provider.fail_at = 0;
    scene.commit();
    scene.check();
    held->check();
    require(scene.published->geometry.provider_revision == scene.provider.epoch,
            "Grid/Flex editor failed to recover font change");
}

template <class Action>
void timed_flex_grid(const char* name, FlexGridEditorScene& scene, Action action, unsigned samples = 80) {
    for (unsigned i = 0; i < 8; ++i)
        action(i);
    const auto before = scene.document.stats();
    const auto compute_before = scene.compute_costs.total, measure_before = scene.compute_costs.measure;
    const auto creates = scene.provider.creates;
    std::vector<double> times;
    times.reserve(samples);
    double total_us = 0;
    for (unsigned i = 0; i < samples; ++i) {
        const auto start = std::chrono::steady_clock::now();
        action(i);
        const auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        total_us += elapsed;
        times.push_back(elapsed);
    }
    const auto after = scene.document.stats();
    const double compute_us = (scene.compute_costs.total - compute_before) / 1000.0;
    const double measure_us = (scene.compute_costs.measure - measure_before) / 1000.0;
    std::sort(times.begin(), times.end());
    std::printf(
        "{\"kind\":\"flex_grid\",\"scenario\":\"%s\",\"samples\":%u,\"median_us\":%.3f,\"p95_us\":%.3f,\"total_mean_us\":%.3f,\"compute_mean_us\":%.3f,\"measure_in_compute_mean_us\":%.3f,\"compute_outside_measure_mean_us\":%.3f,\"consumer_mean_us\":%.3f,\"measure_calls\":%llu,\"node_visits\":%llu,\"paragraph_creates\":%llu,\"buffer_allocations\":%llu}\n",
        name, samples, times[samples / 2], times[samples * 95 / 100], total_us / samples, compute_us / samples,
        measure_us / samples, (compute_us - measure_us) / samples, (total_us - compute_us) / samples,
        after.measure_calls - before.measure_calls, after.node_visits - before.node_visits,
        scene.provider.creates - creates, after.buffer_allocations - before.buffer_allocations);
}

void flex_grid_performance() {
    FlexGridEditorScene scene;
    scene.commit();
    scene.check();
    std::printf("{\"kind\":\"flex_grid_environment\",\"nodes\":%zu,\"explicit_cjk_font\":%s}\n",
                scene.document.node_count(), scene.provider.font_family == L"Microsoft YaHei UI" ? "true" : "false");
    timed_flex_grid("static", scene, [&](unsigned) { scene.commit(); }, 1000);
    timed_flex_grid(
        "resize", scene,
        [&](unsigned i) {
            scene.viewport.width = i % 2 ? 820.f : 1100.f;
            scene.commit();
        },
        40);
    timed_flex_grid("visible_property", scene, [&](unsigned i) {
        scene.edit(0, i % 2 ? L"Short value" : L"A changing property wraps using its resolved Grid column width.");
        scene.commit();
    });
    timed_flex_grid("span_collapse", scene, [&](unsigned i) {
        auto input = accepted(scene.document.input(scene.help));
        input.collapsed = i % 2 != 0;
        accepted(scene.document.update(scene.help, input));
        scene.commit();
    });
    timed_flex_grid(
        "font_change", scene,
        [&](unsigned i) {
            scene.provider.scale = i % 2 ? 1.f : 1.2f;
            ++scene.provider.epoch;
            scene.commit();
        },
        20);
    scene.published->check();
    std::printf(
        "{\"kind\":\"flex_grid_resources\",\"cached_paragraphs\":%zu,\"frame_runs\":%zu,\"document_peak_buffer_bytes\":%llu}\n",
        scene.provider.paragraphs.size(), scene.published->runs.size(), scene.document.stats().peak_buffer_bytes);
}
