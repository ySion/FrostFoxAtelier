// Included by the existing production consumer; all scenes cross the public Interface.
void publication_lookup_lifetime() {
    LayoutDocument d(nullptr, nullptr, {5, 2});
    LayoutDocument foreign(nullptr, nullptr, {2, 1});
    const auto a = accepted(d.create(d.root(), NodeInput::box(20, 10)));
    const auto first = accepted(d.compute({100, 100}, 1));
    auto held = d.snapshot();
    require(first.find(a) && first.find(d.root()) && !first.find({}) && !first.find(foreign.root()),
            "lookup identity guard");
    accepted(d.remove(a));
    const auto b = accepted(d.create(d.root(), NodeInput::box(30, 15)));
    require(first.find(a) && !first.find(b), "slot reuse rewrote published lookup");
    rejected(d.compute({-1, 100}, 1), ErrorCode::invalid_input);
    require(d.view().find(a) && !d.view().find(b), "failed compute rewrote lookup");
    const auto second = accepted(d.compute({100, 100}, 1));
    require(!second.find(a) && second.find(b) && held.find(a) && !held.find(b), "lookup publication mismatch");
    auto copied = held;
    auto moved = std::move(held);
    accepted(d.remove(b));
    accepted(d.compute({100, 100}, 1));
    require(copied.find(a)->width == 20 && moved.find(a)->height == 10, "snapshot lookup copy/move");
    LayoutSnapshot detached;
    LayoutNodeId detached_id;
    {
        LayoutDocument owner(nullptr, nullptr, {2, 1});
        detached_id = accepted(owner.create(owner.root(), NodeInput::box(7, 8)));
        accepted(owner.compute({10, 10}, 1));
        detached = owner.snapshot();
    }
    require(detached.find(detached_id) && detached.find(detached_id)->height == 8, "snapshot index owner lifetime");
}

void descriptors_and_collapsed_subtrees() {
    TextProvider p;
    LayoutDocument d(TextProvider::measure, &p, {8, 3});
    auto invalid = NodeInput::box(-1, 10);
    auto failed = d.create(d.root(), invalid);
    require(!failed && failed.error().field == InputField::width && !describe(failed.error().field).empty(),
            "width diagnostic");
    failed = d.create(d.root(), NodeInput::table({}));
    require(!failed && failed.error().field == InputField::columns, "empty table diagnostic");
    failed = d.create(d.root(), NodeInput::grid(0));
    require(!failed && failed.error().field == InputField::wrap, "grid width diagnostic");
    accepted(d.create(d.root(), NodeInput::overlay()));
    auto group_input = NodeInput::column(5);
    group_input.collapsed = true;
    const auto group = accepted(d.create(d.root(), group_input));
    const auto hidden = accepted(d.create(group, NodeInput::measured(999)));
    const auto view = accepted(d.compute({100, 100}, 1));
    require(!view.find(group) && !view.find(hidden) && p.calls == 0 && d.node_count() == 4,
            "collapse must preserve identity without measuring");
    group_input.collapsed = false;
    accepted(d.update(group, group_input));
    rejected(d.compute({100, 100}, 1), ErrorCode::measurement_failed);
    require(d.view().publication == view.publication && !d.view().find(hidden), "failed restore changed old geometry");
    p.content[999] = {L"恢复后仍使用原来的节点身份", 16, 1};
    require(accepted(d.compute({100, 100}, 1)).find(hidden) != nullptr, "restored identity missing");
}

struct InspectorScene {
    TextProvider provider;

    LayoutDocument document{TextProvider::measure, &provider, {4096, 8}};

    LayoutNodeId table;

    std::vector<LayoutNodeId> rows;

    std::vector<LayoutNodeId> labels;

    std::vector<LayoutNodeId> values;

    explicit InspectorScene(std::size_t count) {
        auto input = NodeInput::table({Axis::content(40, 140), Axis::expand(1, 40)}, 12, 6);
        input.padding = {4, 4, 4, 4};
        table = accepted(document.create(document.root(), input));
        rows.reserve(count);
        labels.reserve(count);
        values.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            append(i);
    }

    void append(std::size_t key) {
        const auto label_key = key * 2 + 1;
        const auto value_key = label_key + 1;
        provider.content[label_key] = {key % 5 == 0 ? L"属性名称与说明" : L"Name", 16, 1};
        provider.content[value_key] = {L"可动态编辑的属性内容，随实际列宽换行。 Value with trailing spaces  ", 16, 1};
        const auto row = accepted(document.create(table, NodeInput::row()));
        rows.push_back(row);
        labels.push_back(accepted(document.create(row, NodeInput::measured(label_key))));
        values.push_back(accepted(document.create(row, NodeInput::measured(value_key))));
    }

    LayoutView compute(float width = 420) { return accepted(document.compute({width, 600}, provider.epoch)); }

    void check(float width) {
        const auto view = compute(width);
        float label_width = 40;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (accepted(document.input(rows[i])).collapsed)
                continue;
            const auto input = accepted(document.input(labels[i]));
            if (!input.collapsed)
                label_width = std::max(label_width, provider.fresh(input.content_key, {}).width);
        }
        label_width = std::min(140.f, label_width);
        const auto value_width = std::max(40.f, width - 8 - 12 - label_width);
        double y = 4;
        bool first = true;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (accepted(document.input(rows[i])).collapsed) {
                require(!view.find(rows[i]) && !view.find(labels[i]) && !view.find(values[i]),
                        "collapsed row geometry");
                continue;
            }
            if (!first)
                y += 6;
            first = false;
            const auto& row = box(view, rows[i]);
            const auto& value = box(view, values[i]);
            require(close(row.y, static_cast<float>(y)) && close(value.x, label_width + 12) &&
                        close(value.width, value_width),
                    "shared column allocation or row spacing");
            const auto value_input = accepted(document.input(values[i]));
            float natural = provider.fresh(value_input.content_key, value_width).height;
            const auto label_input = accepted(document.input(labels[i]));
            if (!label_input.collapsed) {
                require(close(box(view, labels[i]).width, label_width), "label shared width");
                natural = std::max(natural, provider.fresh(label_input.content_key, label_width).height);
            }
            require(close(row.height, natural), "Inspector natural row height");
            y += row.height;
        }
        require(close(box(view, table).height, static_cast<float>(y + 4)), "Inspector natural extent");
        LayoutDocument fresh(TextProvider::measure, &provider, {4096, 8});
        const auto fresh_table = accepted(fresh.create(fresh.root(), accepted(document.input(table))));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto row = accepted(fresh.create(fresh_table, accepted(document.input(rows[i]))));
            accepted(fresh.create(row, accepted(document.input(labels[i]))));
            accepted(fresh.create(row, accepted(document.input(values[i]))));
        }
        same_geometry(view, accepted(fresh.compute({width, 600}, provider.epoch)));
    }
};

void shared_inspector_dynamic() {
    InspectorScene scene(4);
    scene.check(420);
    scene.check(220);
    auto label = accepted(scene.document.input(scene.labels[0]));
    auto& text = scene.provider.content.at(label.content_key);
    text.text = L"X";
    ++text.revision;
    label.content_revision = text.revision;
    accepted(scene.document.update(scene.labels[0], label));
    scene.check(420);
    auto row = accepted(scene.document.input(scene.rows[1]));
    row.collapsed = true;
    accepted(scene.document.update(scene.rows[1], row));
    scene.check(420);
    row.collapsed = false;
    accepted(scene.document.update(scene.rows[1], row));
    scene.check(420);
    auto cell = accepted(scene.document.input(scene.labels[2]));
    cell.collapsed = true;
    accepted(scene.document.update(scene.labels[2], cell));
    scene.check(420);
    scene.provider.scale = 1.4f;
    ++scene.provider.epoch;
    scene.check(220);
    scene.append(20);
    scene.check(420);
    accepted(scene.document.remove(scene.rows.back()));
    scene.rows.pop_back();
    scene.labels.pop_back();
    scene.values.pop_back();
    scene.check(420);
    accepted(scene.document.move(scene.rows[0], scene.table));
    std::rotate(scene.rows.begin(), scene.rows.begin() + 1, scene.rows.end());
    std::rotate(scene.labels.begin(), scene.labels.begin() + 1, scene.labels.end());
    std::rotate(scene.values.begin(), scene.values.begin() + 1, scene.values.end());
    scene.check(420);
}

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
    auto view = accepted(d.compute({500, 200}, 1));
    require(box(view, table).width == 180 && box(view, table).height == 53 && box(view, last).x == 80,
            "table intrinsic shared maximum");
    const auto held = d.snapshot();
    const auto extra = accepted(d.create(r2, NodeInput::box(1, 1)));
    const auto failed = d.compute({500, 200}, 1);
    require(!failed && failed.error().field == InputField::table_row && d.view().publication == held.publication,
            "extra cell diagnostic and rollback");
    accepted(d.remove(extra));
    auto row = NodeInput::row(2);
    accepted(d.update(r1, row));
    require(!d.compute({500, 200}, 1), "row gap must not silently disagree with tracks");
    row.gap = 0;
    accepted(d.update(r1, row));
    spec.columns = {Axis::fixed(40), Axis::fixed(60), Axis::expand()};
    spec.width = Axis::expand();
    accepted(d.update(table, spec));
    auto collapsed = NodeInput::box(20, 10);
    collapsed.collapsed = true;
    accepted(d.update(last, collapsed));
    auto fill = NodeInput::box(20, 10);
    fill.width = Axis::expand();
    const auto third = accepted(d.create(r2, fill));
    view = accepted(d.compute({200, 200}, 1));
    require(!view.find(last) && box(view, third).x == 120 && box(view, third).width == 80,
            "collapsed cell must retain its column ordinal");
}

void wrap_flow_and_repeated_grid() {
    LayoutDocument d(nullptr, nullptr, {12, 3});
    auto spec = NodeInput::wrap(10, 5);
    spec.line_align = .5f;
    const auto flow = accepted(d.create(d.root(), spec));
    const auto a = accepted(d.create(flow, NodeInput::box(60, 20)));
    const auto b = accepted(d.create(flow, NodeInput::box(90, 30)));
    const auto c = accepted(d.create(flow, NodeInput::box(40, 10)));
    auto view = accepted(d.compute({170, 200}, 1));
    require(box(view, a).x == 5 && box(view, b).x == 75 && box(view, c).x == 65 && box(view, c).y == 35 &&
                box(view, flow).height == 45,
            "natural flow packing/alignment");
    accepted(d.remove(a));
    accepted(d.remove(b));
    accepted(d.remove(c));
    spec = NodeInput::grid(60, 10, 5);
    spec.line_align = 1;
    accepted(d.update(flow, spec));
    std::array<LayoutNodeId, 5> items;
    const float heights[]{10, 20, 15, 30, 10};
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto input = NodeInput::box(1, heights[i]);
        input.width = Axis::expand();
        items[i] = accepted(d.create(flow, input));
    }
    view = accepted(d.compute({200, 200}, 1));
    require(box(view, items[0]).width == 60 && box(view, items[3]).x == 70 && box(view, items[3]).y == 25 &&
                box(view, flow).height == 55,
            "three-column grid/tail");
    view = accepted(d.compute({199, 200}, 1));
    require(box(view, items[0]).width == 94.5f && box(view, items[4]).y == 60 && box(view, items[4]).x == 104.5f &&
                box(view, flow).height == 70,
            "grid width threshold");
    view = accepted(d.compute({0, 200}, 1));
    require(view.boxes.size() == 7 && box(view, items[0]).width == 60 && box(view, items[4]).y == 95,
            "zero-width grid must make progress with explicit overflow");
    accepted(d.move(items[4], flow, 0));
    auto hidden = accepted(d.input(items[1]));
    hidden.collapsed = true;
    accepted(d.update(items[1], hidden));
    view = accepted(d.compute({200, 200}, 1));
    require(!view.find(items[1]) && box(view, items[4]).x == 0 && box(view, items[3]).y == 20, "grid reorder/collapse");
}

void nested_wrap_and_shared_columns() {
    LayoutDocument d(nullptr, nullptr, {12, 6});
    const auto table = accepted(d.create(d.root(), NodeInput::table({Axis::fixed(50), Axis::expand()}, 10)));
    const auto row = accepted(d.create(table, NodeInput::row()));
    accepted(d.create(row, NodeInput::box(50, 10)));
    const auto wrap = accepted(d.create(row, NodeInput::wrap(5, 5)));
    accepted(d.create(wrap, NodeInput::box(40, 10)));
    const auto last = accepted(d.create(wrap, NodeInput::box(40, 10)));
    auto view = accepted(d.compute({140, 200}, 1));
    require(box(view, wrap).width == 80 && box(view, last).y == 15 && box(view, row).height == 25,
            "shared width must precede wrapped height");
    view = accepted(d.compute({200, 200}, 1));
    require(box(view, last).x == 45 && box(view, last).y == 0 && box(view, table).height == 10,
            "nested reflow must shrink natural height");
    auto intrinsic_grid = NodeInput::grid(30, 5);
    intrinsic_grid.width = Axis::content();
    const auto grid = accepted(d.create(d.root(), intrinsic_grid));
    auto leaf = NodeInput::measured(1);
    leaf.height = Axis::fixed(6);
    accepted(d.create(grid, leaf));
    accepted(d.create(grid, leaf));
    view = accepted(d.compute({200, 200}, 1));
    require(box(view, grid).width == 65 && box(view, grid).height == 6 && d.stats().measure_calls == 0,
            "known repeated slots must not request unused intrinsic content");
}

void grid_text_and_failure_recovery() {
    TextProvider p;
    LayoutDocument d(TextProvider::measure, &p, {8, 3});
    const auto grid = accepted(d.create(d.root(), NodeInput::grid(80, 8, 4)));
    std::array<LayoutNodeId, 4> items;
    for (std::size_t i = 0; i < items.size(); ++i) {
        p.content[i + 1] = {L"物品描述随格子宽度换行并保持自然高度", 16, 1};
        items[i] = accepted(d.create(grid, NodeInput::measured(i + 1)));
    }
    auto view = accepted(d.compute({200, 200}, 1));
    require(close(box(view, items[0]).height, p.fresh(1, 96).height), "grid text actual width");
    const auto before = d.snapshot();
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto& content = p.content.at(i + 1);
        content.text += L" 更新";
        ++content.revision;
        accepted(d.update(items[i], NodeInput::measured(i + 1, content.revision)));
    }
    p.fail_at = p.calls + 3;
    rejected(d.compute({260, 200}, 1), ErrorCode::measurement_failed);
    require(d.view().publication == before.publication &&
                d.view().find(items[0])->width == before.find(items[0])->width,
            "failed reflow changed published index/geometry");
    p.fail_at = 0;
    const auto calls = p.calls;
    view = accepted(d.compute({260, 200}, 1));
    require(p.calls == calls + 4 && close(box(view, items[0]).width, (260.f - 16) / 3),
            "failure cached partial new measurements");
    LayoutDocument fresh(TextProvider::measure, &p, {8, 3});
    const auto fresh_grid = accepted(fresh.create(fresh.root(), NodeInput::grid(80, 8, 4)));
    for (std::size_t i = 0; i < items.size(); ++i)
        accepted(fresh.create(fresh_grid, NodeInput::measured(i + 1, 2)));
    same_geometry(view, accepted(fresh.compute({260, 200}, 1)));
}

template <class Scene, class Action>
void timed_scene(const char* name, Scene& scene, Action action, unsigned iterations = 120, unsigned batch = 1) {
    for (unsigned i = 0; i < 8; ++i)
        action(i);
    std::vector<double> samples;
    samples.reserve(iterations);
    const auto before = scene.document.stats();
    const auto creates = scene.provider.creates;
    const auto measure_nanoseconds = scene.provider.measure_nanoseconds;
    double total_us = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned j = 0; j < batch; ++j)
            action(i * batch + j);
        const auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        total_us += elapsed;
        samples.push_back(elapsed / batch);
    }
    const auto after = scene.document.stats();
    std::sort(samples.begin(), samples.end());
    const auto measure_us = static_cast<double>(scene.provider.measure_nanoseconds - measure_nanoseconds) / 1000;
    std::printf(
        "{\"kind\":\"performance\",\"scenario\":\"%s\",\"nodes\":%zu,\"samples\":%u,\"batch\":%u,\"median_us\":%.3f,\"p95_us\":%.3f,\"measure_total_us\":%.3f,\"mean_outside_measure_us\":%.3f,\"buffer_allocations\":%llu,\"buffer_bytes_allocated\":%llu,\"retained_buffer_bytes\":%llu,\"peak_buffer_bytes\":%llu,\"measure_calls\":%llu,\"paragraph_creates\":%llu,\"node_visits\":%llu}\n",
        name, scene.document.node_count(), iterations, batch, samples[iterations / 2], samples[(iterations * 95) / 100],
        measure_us, (total_us - measure_us) / (iterations * batch),
        after.buffer_allocations - before.buffer_allocations,
        after.buffer_bytes_allocated - before.buffer_bytes_allocated, after.buffer_bytes, after.peak_buffer_bytes,
        after.measure_calls - before.measure_calls, scene.provider.creates - creates,
        after.node_visits - before.node_visits);
}

struct InventoryScene {
    TextProvider provider;

    LayoutDocument document{TextProvider::measure, &provider, {2048, 4}};

    LayoutNodeId grid;

    std::vector<LayoutNodeId> items;

    InventoryScene() {
        grid = accepted(document.create(document.root(), NodeInput::grid(80, 8, 6)));
        for (std::uint64_t i = 1; i <= 256; ++i) {
            provider.content[i] = {L"物品名称与数量 Item description", 16, 1};
            items.push_back(accepted(document.create(grid, NodeInput::measured(i))));
        }
    }

    void compute(float width = 640) { accepted(document.compute({width, 480}, provider.epoch)); }
};

void performance() {
    const auto start = std::chrono::steady_clock::now();
    InspectorScene scene(256);
    scene.compute();
    const auto cold = scene.document.stats();
    std::printf(
        "{\"kind\":\"performance\",\"scenario\":\"cold_inspector\",\"nodes\":%zu,\"elapsed_us\":%.3f,\"buffer_allocations\":%llu,\"buffer_bytes_allocated\":%llu,\"retained_buffer_bytes\":%llu,\"measure_calls\":%llu}\n",
        scene.document.node_count(),
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count(),
        cold.buffer_allocations, cold.buffer_bytes_allocated, cold.buffer_bytes, cold.measure_calls);
    timed_scene("static", scene, [&](unsigned) { scene.compute(); }, 1000, 100);
    timed_scene("single_content", scene, [&](unsigned i) {
        auto input = accepted(scene.document.input(scene.values[3]));
        auto& content = scene.provider.content.at(input.content_key);
        content.text = i % 2 ? L"短值" : L"A changed property value with multiple words and natural wrapping.";
        input.content_revision = ++content.revision;
        accepted(scene.document.update(scene.values[3], input));
        scene.compute();
    });
    timed_scene("resize", scene, [&](unsigned i) { scene.compute(i % 2 ? 280.f : 500.f); });
    timed_scene("collapse_restore", scene, [&](unsigned i) {
        auto input = accepted(scene.document.input(scene.rows[3]));
        input.collapsed = i % 2 != 0;
        accepted(scene.document.update(scene.rows[3], input));
        scene.compute();
    });
    timed_scene("insert_remove", scene, [&](unsigned) {
        scene.append(300);
        scene.compute();
        accepted(scene.document.remove(scene.rows.back()));
        scene.rows.pop_back();
        scene.labels.pop_back();
        scene.values.pop_back();
        scene.compute();
    });
    const auto view = scene.compute();
    timed_scene(
        "lookup_all_values", scene,
        [&](unsigned) {
            double total = 0;
            for (const auto id : scene.values)
                if (const auto* value = view.find(id))
                    total += value->width;
            require(total > 0, "lookup checksum");
        },
        1000, 10);
    timed_scene("owned_snapshot", scene, [&](unsigned) {
        const auto copy = scene.document.snapshot();
        require(copy.find(scene.table) != nullptr, "snapshot query");
    });
    for (const auto row : scene.rows)
        accepted(scene.document.remove(row));
    scene.compute();
    const auto reduced = scene.document.stats();
    std::printf(
        "{\"kind\":\"performance\",\"scenario\":\"after_remove_all_rows\",\"nodes\":%zu,\"retained_buffer_bytes\":%llu,\"peak_buffer_bytes\":%llu}\n",
        scene.document.node_count(), reduced.buffer_bytes, reduced.peak_buffer_bytes);
    InventoryScene inventory;
    inventory.compute();
    timed_scene("grid_static", inventory, [&](unsigned) { inventory.compute(); }, 1000, 100);
    timed_scene("grid_resize", inventory, [&](unsigned i) { inventory.compute(i % 2 ? 480.f : 720.f); });
    timed_scene("grid_single_content", inventory, [&](unsigned i) {
        auto& content = inventory.provider.content.at(1);
        content.text = i % 2 ? L"物品 x1" : L"物品名称更新，数量与描述变化 Item x2";
        ++content.revision;
        accepted(inventory.document.update(inventory.items[0], NodeInput::measured(1, content.revision)));
        inventory.compute();
    });
    timed_scene("grid_reorder", inventory, [&](unsigned i) {
        accepted(inventory.document.move(inventory.items[0], inventory.grid, i % 2 ? 0 : append_position));
        inventory.compute();
    });
}
