// Audit workloads use the public document and complete geometry publications.
void allocation_rounded_saturation() {
    for (bool flex : {false, true})
        for (bool transpose : {false, true}) {
            LayoutDocument document(nullptr, nullptr, {6, 1});
            RootInput root{transpose ? Kind::column : Kind::row};
            if (flex)
                root.flow = FlexSpec{};
            accepted(document.set_root(root));
            constexpr double scale = 0x1p50;
            for (double width : {1.0 - 0x1p-24, 0x1p-24 - 0x1p-48, 0x1p-48 - 0x1p-51}) {
                const auto size = static_cast<float>(width * scale);
                accepted(
                    document.create(document.root(), NodeInput::box(transpose ? 1.f : size, transpose ? size : 1.f)));
            }
            auto item = NodeInput::box(1, 1);
            auto& axis = transpose ? item.height : item.width;
            axis = Axis::expand(1.9053054684343187e22f, 0, static_cast<float>(3 * scale));
            accepted(document.create(document.root(), item));
            axis = Axis::expand();
            const auto residual = accepted(document.create(document.root(), item));
            const auto size = static_cast<float>(4 * scale);
            const auto view = accepted(document.compute({transpose ? 10.f : size, transpose ? size : 10.f}, 1));
            const auto remaining = transpose ? box(view, residual).height : box(view, residual).width;
            require(remaining > .49f && remaining < .51f,
                    "rounded saturation consumed the remaining allocation of a small weight");
        }
}

void flex_baseline_boundaries() {
    for (bool detached : {false, true}) {
        LayoutDocument document(nullptr, nullptr, {3, 1});
        RootInput root{detached ? Kind::row : Kind::column};
        root.flow = FlexSpec{};
        accepted(document.set_root(root));
        auto input = NodeInput::box(10, 10);
        input.out_of_flow = detached;
        const auto child = accepted(document.create(document.root(), input));
        const auto first = accepted(document.compute({100, 100}, 1));
        input.vertical_alignment = Alignment::baseline();
        accepted(document.update(child, input));
        const auto result = document.compute({100, 100}, 1);
        rejected(result, ErrorCode::unsupported_dependency);
        require(result.error().node == child && result.error().field == InputField::alignment &&
                    document.view().publication == first.publication &&
                    document.view().boxes.data() == first.boxes.data(),
                "unsupported baseline replaced the previous publication or lost its diagnostic");
        input.collapsed = true;
        accepted(document.update(child, input));
        require(!accepted(document.compute({100, 100}, 1)).find(child), "collapsed baseline participated in layout");
        input.collapsed = false;
        input.vertical_alignment = Alignment::stretch();
        accepted(document.update(child, input));
        require(accepted(document.compute({100, 100}, 1)).find(child), "baseline correction did not recover");
    }
    LayoutDocument document(nullptr, nullptr, {2, 1});
    RootInput root{Kind::row};
    root.flow = FlexSpec{.align_items = Alignment::baseline()};
    accepted(document.set_root(root));
    auto input = NodeInput::box(10, 10);
    input.out_of_flow = true;
    const auto child = accepted(document.create(document.root(), input));
    rejected(document.compute({100, 100}, 1), ErrorCode::unsupported_dependency);
    input.vertical_alignment = Alignment::stretch();
    accepted(document.update(child, input));
    require(accepted(document.compute({100, 100}, 1)).find(child),
            "detached item could not override inherited baseline");
}

struct GridSizingScene {
    LayoutDocument document{nullptr, nullptr, {2050, 2}};
    LayoutNodeId parent;
    explicit GridSizingScene(bool fixed) {
        GridSpec spec{.columns = std::vector<Axis>(4096, fixed ? Axis::fixed(1) : Axis::content(1)),
                      .rows = {Axis::fixed(1)}};
        auto grid = NodeInput::grid(spec);
        grid.width = Axis::fixed(4096);
        grid.height = Axis::fixed(1);
        parent = accepted(document.create(document.root(), grid));
        for (unsigned i = 0; i < 2048; ++i) {
            auto child = NodeInput::box(1, 1);
            child.cell = {.row = 0, .column = 0, .column_span = fixed ? 4096u : i % 1024 + 1};
            accepted(document.create(parent, child));
        }
    }
};

void grid_sizing_batches() {
    // Explicit overlapping spans do not require a product-sized occupancy map.
    for (bool fixed : {false, true}) {
        GridSizingScene scene(fixed);
        accepted(scene.document.compute({4096, 10}, 1));
        const auto held = scene.document.snapshot();
        const auto next = accepted(scene.document.compute({4097, 10}, 1));
        require(std::equal(held.boxes.begin() + 1, held.boxes.end(), next.boxes.begin() + 1),
                "large span track sizing changed with an unrelated viewport width");
        require(held.find(scene.parent) && held.boxes.size() == 2050 &&
                    close(box(scene.document.view(), scene.parent).width, 4096),
                "large span sizing changed geometry or snapshot");
    }
    LayoutDocument document(nullptr, nullptr, {8, 2});
    accepted(document.set_root({Kind::row}));
    auto grid = NodeInput::grid(GridSpec{.columns = {Axis::fixed(10), Axis::content(0, 20)},
                                         .rows = {Axis::fixed(1)},
                                         .implicit_column = Axis::expand(1, 5, 30)});
    grid.width = Axis::fixed(100);
    const auto parent = accepted(document.create(document.root(), grid));
    auto wide = NodeInput::box(100, 1);
    wide.cell = {.row = 0, .column = 0, .column_span = 3};
    accepted(document.create(parent, wide));
    auto narrow = NodeInput::box(15, 1);
    narrow.cell = {.row = 0, .column = 1};
    accepted(document.create(parent, narrow));
    std::array<LayoutNodeId, 3> probes;
    for (std::uint32_t column = 0; column < 3; ++column) {
        auto probe = NodeInput::box(1, 1);
        probe.width = Axis::expand();
        probe.cell = {.row = 0, .column = column};
        probes[column] = accepted(document.create(parent, probe));
    }
    auto filling = NodeInput::box(1, 1);
    filling.width = Axis::expand();
    const auto sibling = accepted(document.create(document.root(), filling));
    for (bool intrinsic : {false, true}) {
        if (intrinsic) {
            grid.width = Axis::content();
            accepted(document.update(parent, grid));
        }
        const auto view = accepted(document.compute({200, 10}, 1));
        require(close(box(view, probes[0]).width, 10) && close(box(view, probes[1]).width, 15) &&
                    close(box(view, probes[2]).width, 30) && close(box(view, parent).width, intrinsic ? 60.f : 100.f) &&
                    close(box(view, sibling).width, intrinsic ? 140.f : 100.f),
                "implicit Expand classification, hard maximum or nested parent scratch changed");
    }

    TextProvider provider;
    provider.content[1] = {L"A spanning paragraph for independent track batches.", 16, 1};
    provider.content[2] = {L"Another wide paragraph spanning all three content columns.", 16, 1};
    provider.content[3] = {L"Independent sibling grid.", 16, 1};
    auto populate = [&](LayoutDocument& target) {
        accepted(target.set_root({Kind::row}));
        auto first = NodeInput::grid(GridSpec{.columns = {Axis::content(0, 20), Axis::content(), Axis::content()}});
        first.width = Axis::content();
        const auto first_id = accepted(target.create(target.root(), first));
        auto two = NodeInput::measured(1, provider.content[1].revision);
        two.cell = {.row = 0, .column = 0, .column_span = 2};
        const auto two_id = accepted(target.create(first_id, two));
        auto three = NodeInput::measured(2, provider.content[2].revision);
        three.cell = {.row = 1, .column = 0, .column_span = 3};
        const auto three_id = accepted(target.create(first_id, three));
        auto second = NodeInput::grid(GridSpec{.columns = {Axis::content()}});
        second.width = Axis::content();
        const auto second_id = accepted(target.create(target.root(), second));
        auto leaf = NodeInput::measured(3, provider.content[3].revision);
        leaf.cell = {.row = 0, .column = 0};
        const auto leaf_id = accepted(target.create(second_id, leaf));
        return std::array{first_id, two_id, three_id, second_id, leaf_id};
    };
    LayoutDocument retry(TextProvider::measure, &provider, {6, 2});
    const auto ids = populate(retry);
    accepted(retry.compute({1000, 200}, provider.epoch));
    const auto held = retry.snapshot();
    for (unsigned i = 1; i <= 3; ++i) {
        ++provider.content[i].revision;
        provider.content[i].text += L" Revised.";
    }
    for (unsigned at : {1u, 2u, 4u}) {
        auto input = accepted(retry.input(ids[at]));
        ++input.content_revision;
        if (at == 4)
            input.content_key = 999;
        accepted(retry.update(ids[at], input));
    }
    rejected(retry.compute({1000, 200}, provider.epoch), ErrorCode::measurement_failed);
    require(retry.view().publication == held.publication &&
                std::equal(held.boxes.begin(), held.boxes.end(), retry.view().boxes.begin()),
            "later Grid failure committed earlier span batches");
    auto leaf = accepted(retry.input(ids[4]));
    leaf.content_key = 3;
    accepted(retry.update(ids[4], leaf));
    auto recovered = accepted(retry.compute({1000, 200}, provider.epoch));
    LayoutDocument fresh(TextProvider::measure, &provider, {6, 2});
    populate(fresh);
    same_geometry(recovered, accepted(fresh.compute({1000, 200}, provider.epoch)));
    for (unsigned i = 1; i <= 2; ++i) {
        provider.content[i].text = L"Short";
        ++provider.content[i].revision;
        auto input = accepted(retry.input(ids[i]));
        ++input.content_revision;
        accepted(retry.update(ids[i], input));
    }
    accepted(retry.move(ids[3], retry.root(), 0));
    accepted(retry.move(ids[1], ids[0]));
    recovered = accepted(retry.compute({1000, 200}, provider.epoch));
    LayoutDocument reordered(TextProvider::measure, &provider, {6, 2});
    const auto new_ids = populate(reordered);
    accepted(reordered.move(new_ids[3], reordered.root(), 0));
    accepted(reordered.move(new_ids[1], new_ids[0]));
    same_geometry(recovered, accepted(reordered.compute({1000, 200}, provider.epoch)));
}

void grid_sparse_spans() {
    for (auto flow : {AutoFlow::row, AutoFlow::column}) {
        LayoutDocument document(nullptr, nullptr, {5, 2});
        GridSpec spec{.columns = std::vector<Axis>(3, Axis::fixed(10)),
                      .rows = std::vector<Axis>(3, Axis::fixed(10)),
                      .auto_flow = flow};
        const auto parent = accepted(document.create(document.root(), NodeInput::grid(spec)));
        auto input = NodeInput::box(10, 10);
        input.cell = {.row = 0, .column = 0};
        accepted(document.create(parent, input));
        input.cell = flow == AutoFlow::row ? Cell{.row = 0, .row_span = 2} : Cell{.column = 0, .column_span = 2};
        const auto span = accepted(document.create(parent, input));
        input.cell = flow == AutoFlow::row ? Cell{.row = 1} : Cell{.column = 1};
        const auto last = accepted(document.create(parent, input));
        const auto view = accepted(document.compute({100, 100}, 1));
        require(close(flow == AutoFlow::row ? box(view, span).x : box(view, span).y, 10) &&
                    close(flow == AutoFlow::row ? box(view, last).x : box(view, last).y, 20),
                "sparse placement backfilled a line covered by an earlier automatic span");
    }
}

struct GridCollisionScene {
    LayoutDocument document{nullptr, nullptr, {4, 2}};
    LayoutNodeId parent, moving;
    explicit GridCollisionScene(bool transpose = false) {
        GridSpec spec;
        if (transpose)
            spec.rows.assign(256, Axis::fixed(1));
        else
            spec.columns.assign(256, Axis::fixed(1));
        spec.auto_flow = transpose ? AutoFlow::column : AutoFlow::row;
        spec.implicit_column = spec.implicit_row = Axis::fixed(1);
        auto grid = NodeInput::grid(spec);
        grid.width = Axis::fixed(4096);
        grid.height = Axis::fixed(4096);
        parent = accepted(document.create(document.root(), grid));
        auto obstacle = NodeInput::box(1, 1);
        obstacle.cell = transpose ? Cell{.row = 0, .column = 2047, .row_span = 256}
                                  : Cell{.row = 2047, .column = 0, .column_span = 256};
        accepted(document.create(parent, obstacle));
        auto item = NodeInput::box(1, 1);
        item.cell.row_span = transpose ? 256 : 2048;
        item.cell.column_span = transpose ? 2048 : 256;
        moving = accepted(document.create(parent, item));
    }
};

void grid_collision_boundaries() {
    for (bool transpose : {false, true}) {
        GridCollisionScene scene(transpose);
        auto view = accepted(scene.document.compute({4096, 4096}, 1));
        require(close(transpose ? box(view, scene.moving).x : box(view, scene.moving).y, 2048),
                "large automatic span skipped its first available rectangle");
        const auto held = scene.document.snapshot();
        auto input = accepted(scene.document.input(scene.moving));
        if (transpose)
            ++input.cell.column_span;
        else
            ++input.cell.row_span;
        accepted(scene.document.update(scene.moving, input));
        rejected(scene.document.compute({4096, 4096}, 1), ErrorCode::capacity_exceeded);
        require(scene.document.view().publication == held.publication && held.find(scene.moving),
                "placement capacity failure replaced the complete publication");
        if (transpose)
            --input.cell.column_span;
        else
            --input.cell.row_span;
        accepted(scene.document.update(scene.moving, input));
        accepted(scene.document.compute({4096, 4096}, 1));
    }
    // Exercise bit-word boundaries and both cursor axes with ordinary spans.
    for (bool transpose : {false, true}) {
        LayoutDocument document(nullptr, nullptr, {6, 2});
        GridSpec spec{.columns = std::vector<Axis>(130, Axis::fixed(1)),
                      .rows = std::vector<Axis>(130, Axis::fixed(1)),
                      .auto_flow = transpose ? AutoFlow::column : AutoFlow::row};
        const auto parent = accepted(document.create(document.root(), NodeInput::grid(spec)));
        auto obstacle = NodeInput::box(1, 1);
        obstacle.cell =
            transpose ? Cell{.row = 0, .column = 0, .row_span = 64} : Cell{.row = 0, .column = 0, .column_span = 64};
        accepted(document.create(parent, obstacle));
        auto item = NodeInput::box(1, 1);
        item.cell.row_span = item.cell.column_span = 2;
        const auto crossing = accepted(document.create(parent, item));
        item.cell = transpose ? Cell{.row = 0} : Cell{.column = 0};
        const auto locked = accepted(document.create(parent, item));
        const auto view = accepted(document.compute({200, 200}, 1));
        require(close(transpose ? box(view, crossing).y : box(view, crossing).x, 64) &&
                    close(transpose ? box(view, crossing).x : box(view, crossing).y, 0) &&
                    close(transpose ? box(view, locked).x : box(view, locked).y, 1),
                "collision jump crossed a free position or a 64-bit occupancy boundary");
    }
}

void collapsed_publication_indices() {
    LayoutDocument document(nullptr, nullptr, {40, 3});
    auto input = NodeInput::column();
    const auto group = accepted(document.create(document.root(), input));
    std::vector<LayoutNodeId> children;
    for (unsigned i = 0; i < 32; ++i)
        children.push_back(accepted(document.create(group, NodeInput::box(10, 10))));
    const auto peer = accepted(document.create(document.root(), NodeInput::box(20, 20)));
    accepted(document.compute({100, 400}, 1));
    const auto held = document.snapshot();
    for (unsigned i = 0; i < 6; ++i) {
        input.collapsed = i % 2 == 0;
        accepted(document.update(group, input));
        const auto view = accepted(document.compute({100, 400}, 1));
        require(view.find(peer) && bool(view.find(group)) != input.collapsed, "collapsed group index mismatch");
        for (auto child : children)
            require(bool(view.find(child)) != input.collapsed, "stale publication index exposed hidden child");
    }
    input.collapsed = true;
    accepted(document.update(group, input));
    auto bad = NodeInput::box(20, 20);
    bad.out_of_flow = true;
    bad.anchor_x = std::numeric_limits<float>::max();
    accepted(document.update(peer, bad));
    const auto publication = document.view().publication;
    rejected(document.compute({100, 400}, 1), ErrorCode::numeric_overflow);
    require(document.view().publication == publication && document.view().find(children.front()),
            "failed collapsed compute mutated the published index");
    accepted(document.update(peer, NodeInput::box(20, 20)));
    accepted(document.compute({100, 400}, 1));
    require(!document.view().find(children.front()), "retry reused a failed compute's visible index");
    accepted(document.remove(group));
    const auto replacement = accepted(document.create(document.root(), NodeInput::box(3, 3)));
    for (float width : {100.f, 101.f, 102.f}) {
        const auto view = accepted(document.compute({width, 400}, 1));
        require(view.find(replacement) && !view.find(group), "recycled slot exposed old group");
        for (auto child : children)
            require(!view.find(child) && held.find(child), "old or current identity index corrupted");
    }
}

template <class Action>
void timed_audit(const char* scenario, LayoutDocument& document, Action action, unsigned samples = 60) {
    for (unsigned i = 0; i < 4; ++i)
        action(i);
    const auto before = document.stats();
    std::vector<double> times;
    times.reserve(samples);
    for (unsigned i = 0; i < samples; ++i) {
        const auto start = std::chrono::steady_clock::now();
        action(i);
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    const auto after = document.stats();
    std::sort(times.begin(), times.end());
    std::printf(
        "{\"kind\":\"audit_performance\",\"scenario\":\"%s\",\"samples\":%u,\"nodes\":%zu,\"published_nodes\":%zu,\"median_us\":%.3f,\"p95_us\":%.3f,\"node_visits\":%llu,\"buffer_allocations\":%llu,\"peak_buffer_bytes\":%llu}\n",
        scenario, samples, document.node_count(), document.view().boxes.size(), times[samples / 2],
        times[samples * 95 / 100], after.node_visits - before.node_visits,
        after.buffer_allocations - before.buffer_allocations, after.peak_buffer_bytes);
}

void audit_performance() {
    for (bool fixed : {false, true}) {
        GridSizingScene scene(fixed);
        timed_audit(fixed ? "grid_fixed_spans" : "grid_satisfied_spans", scene.document,
                    [&](unsigned i) { accepted(scene.document.compute({4096.f + i % 2, 10}, 1)); });
    }
    {
        LayoutDocument row(nullptr, nullptr, {2049, 1});
        accepted(row.set_root({Kind::row}));
        for (unsigned i = 0; i < 2048; ++i) {
            auto item = NodeInput::box(1, 1);
            item.width = Axis::expand();
            accepted(row.create(row.root(), item));
        }
        timed_audit("expand_2048", row, [&](unsigned i) { accepted(row.compute({4096.f + i % 2, 100}, 1)); });
    }
    for (bool transpose : {false, true}) {
        GridCollisionScene scene(transpose);
        timed_audit(transpose ? "grid_collision_column" : "grid_collision_row", scene.document,
                    [&](unsigned i) { accepted(scene.document.compute({4096.f + i % 2, 4096}, 1)); });
    }
    LayoutDocument document(nullptr, nullptr, {100010, 3});
    auto input = NodeInput::column();
    input.collapsed = true;
    const auto group = accepted(document.create(document.root(), input));
    for (unsigned i = 0; i < 100000; ++i)
        accepted(document.create(group, NodeInput::box(10, 10)));
    for (unsigned i = 0; i < 8; ++i)
        accepted(document.create(document.root(), NodeInput::box(10, 10)));
    timed_audit("collapsed_100k", document, [&](unsigned i) { accepted(document.compute({100.f + i % 2, 100}, 1)); });
}
