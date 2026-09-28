// Audit workloads use the public document and complete geometry publications.
void allocation_rounded_saturation() {
    LayoutDocument document(nullptr, nullptr, {6, 1});
    accepted(document.set_root({Kind::row}));
    constexpr double scale = 0x1p50;
    for (double width : {1.0 - 0x1p-24, 0x1p-24 - 0x1p-48, 0x1p-48 - 0x1p-51})
        accepted(document.create(document.root(), NodeInput::box(static_cast<float>(width * scale), 1)));
    auto item = NodeInput::box(1, 1);
    item.width = Axis::expand(1.9053054684343187e22f, 0, static_cast<float>(3 * scale));
    accepted(document.create(document.root(), item));
    item.width = Axis::expand();
    const auto residual = accepted(document.create(document.root(), item));
    const auto view = accepted(document.compute({static_cast<float>(4 * scale), 10}, 1));
    require(box(view, residual).width > .49f && box(view, residual).width < .51f,
        "rounded saturation consumed the remaining allocation of a small weight");
}

void grid_sparse_spans() {
    for (auto flow : {AutoFlow::row, AutoFlow::column}) {
        LayoutDocument document(nullptr, nullptr, {5, 2});
        GridSpec spec{.columns = std::vector<Axis>(3, Axis::fixed(10)),
            .rows = std::vector<Axis>(3, Axis::fixed(10)), .auto_flow = flow};
        const auto parent = accepted(document.create(document.root(), NodeInput::grid(spec)));
        auto input = NodeInput::box(10, 10); input.cell = {.row = 0, .column = 0};
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
        if (transpose) spec.rows.assign(256, Axis::fixed(1));
        else spec.columns.assign(256, Axis::fixed(1));
        spec.auto_flow = transpose ? AutoFlow::column : AutoFlow::row;
        spec.implicit_column = spec.implicit_row = Axis::fixed(1);
        auto grid = NodeInput::grid(spec); grid.width = Axis::fixed(4096); grid.height = Axis::fixed(4096);
        parent = accepted(document.create(document.root(), grid));
        auto obstacle = NodeInput::box(1, 1);
        obstacle.cell = transpose ? Cell{.row = 0, .column = 2047, .row_span = 256} :
            Cell{.row = 2047, .column = 0, .column_span = 256};
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
        if (transpose) ++input.cell.column_span; else ++input.cell.row_span;
        accepted(scene.document.update(scene.moving, input));
        rejected(scene.document.compute({4096, 4096}, 1), ErrorCode::capacity_exceeded);
        require(scene.document.view().publication == held.publication && held.find(scene.moving),
            "placement capacity failure replaced the complete publication");
        if (transpose) --input.cell.column_span; else --input.cell.row_span;
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
        obstacle.cell = transpose ? Cell{.row = 0, .column = 0, .row_span = 64} :
            Cell{.row = 0, .column = 0, .column_span = 64};
        accepted(document.create(parent, obstacle));
        auto item = NodeInput::box(1, 1); item.cell.row_span = item.cell.column_span = 2;
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
    for (unsigned i = 0; i < 32; ++i) children.push_back(accepted(document.create(group, NodeInput::box(10, 10))));
    const auto peer = accepted(document.create(document.root(), NodeInput::box(20, 20)));
    accepted(document.compute({100, 400}, 1)); const auto held = document.snapshot();
    for (unsigned i = 0; i < 6; ++i) {
        input.collapsed = i % 2 == 0; accepted(document.update(group, input));
        const auto view = accepted(document.compute({100, 400}, 1));
        require(view.find(peer) && bool(view.find(group)) != input.collapsed, "collapsed group index mismatch");
        for (auto child : children) require(bool(view.find(child)) != input.collapsed, "stale publication index exposed hidden child");
    }
    input.collapsed = true; accepted(document.update(group, input));
    auto bad = NodeInput::box(20, 20); bad.out_of_flow = true; bad.anchor_x = std::numeric_limits<float>::max();
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
        for (auto child : children) require(!view.find(child) && held.find(child), "old or current identity index corrupted");
    }
}

template<class Action> void timed_audit(const char* scenario, LayoutDocument& document, Action action, unsigned samples = 60) {
    for (unsigned i = 0; i < 4; ++i) action(i);
    const auto before = document.stats();
    std::vector<double> times; times.reserve(samples);
    for (unsigned i = 0; i < samples; ++i) {
        const auto start = std::chrono::steady_clock::now(); action(i);
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    const auto after = document.stats(); std::sort(times.begin(), times.end());
    std::printf("{\"kind\":\"audit_performance\",\"scenario\":\"%s\",\"samples\":%u,\"nodes\":%zu,\"published_nodes\":%zu,\"median_us\":%.3f,\"p95_us\":%.3f,\"node_visits\":%llu,\"buffer_allocations\":%llu,\"peak_buffer_bytes\":%llu}\n",
        scenario, samples, document.node_count(), document.view().boxes.size(), times[samples / 2], times[samples * 95 / 100],
        after.node_visits - before.node_visits, after.buffer_allocations - before.buffer_allocations, after.peak_buffer_bytes);
}

void audit_performance() {
    {
        LayoutDocument row(nullptr, nullptr, {2049, 1});
        accepted(row.set_root({Kind::row}));
        for (unsigned i = 0; i < 2048; ++i) {
            auto item = NodeInput::box(1, 1); item.width = Axis::expand();
            accepted(row.create(row.root(), item));
        }
        timed_audit("expand_2048", row, [&](unsigned i) { accepted(row.compute({4096.f + i % 2, 100}, 1)); });
    }
    for (bool transpose : {false, true}) {
        GridCollisionScene scene(transpose);
        timed_audit(transpose ? "grid_collision_column" : "grid_collision_row", scene.document, [&](unsigned i) {
            accepted(scene.document.compute({4096.f + i % 2, 4096}, 1));
        });
    }
    LayoutDocument document(nullptr, nullptr, {100010, 3});
    auto input = NodeInput::column(); input.collapsed = true;
    const auto group = accepted(document.create(document.root(), input));
    for (unsigned i = 0; i < 100000; ++i) accepted(document.create(group, NodeInput::box(10, 10)));
    for (unsigned i = 0; i < 8; ++i) accepted(document.create(document.root(), NodeInput::box(10, 10)));
    timed_audit("collapsed_100k", document, [&](unsigned i) { accepted(document.compute({100.f + i % 2, 100}, 1)); });
}
