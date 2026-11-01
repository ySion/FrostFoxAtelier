// A headless editor consumer: real layout and DirectWrite, no window/input/GPU.
void retained_text_frames() {
    TextProvider provider;
    provider.content[1] = {L"The same property appears in a narrow panel and a wide panel with different line breaks.",
                           16, 1};
    LayoutDocument document(TextProvider::measure, &provider, {3, 1});
    auto narrow = NodeInput::measured(1);
    narrow.width = Axis::fixed(100);
    narrow.height = Axis::fixed(80);
    auto wide = narrow;
    wide.width = Axis::fixed(300);
    const auto a = accepted(document.create(document.root(), narrow));
    const auto b = accepted(document.create(document.root(), wide));
    accepted(document.compute({400, 200}, provider.epoch));
    require(provider.calls == 0, "fixed-size text unexpectedly needed layout measurement");
    auto old = TextFrame::capture(document, provider);
    old->check();
    require(old->runs.size() == 2 && old->runs[0].text.paragraph != old->runs[1].text.paragraph &&
                old->runs[0].text.size.height > old->runs[1].text.size.height,
            "shared text key lost per-node constraints");
    std::weak_ptr<const TextProvider::Paragraph> retired = old->runs[0].text.paragraph;
    provider.content[1] = {L"Updated fixed-size content", 16, 2};
    narrow.content_revision = wide.content_revision = 2;
    const auto before = document.stats();
    accepted(document.update(a, narrow));
    accepted(document.update(b, wide));
    accepted(document.compute({400, 200}, provider.epoch));
    auto current = TextFrame::capture(document, provider);
    require(document.stats().geometry_reuses == before.geometry_reuses + 1 && provider.calls == 0 &&
                current->runs[0].query.content_revision == 2,
            "geometry reuse failed to refresh draw content");
    old->check();
    current->check();
    provider.paragraphs.clear();
    provider.content.clear();
    old->check();
    current->check();
    old.reset();
    require(retired.expired(), "retired frame kept an unowned paragraph alive");
}

struct EditorScene {
    struct Frame {
        std::shared_ptr<const TextFrame> shell;

        VirtualInspector::Output inspector;

        std::uint64_t serial{};
    } published;

    TextProvider provider;

    LayoutDocument document{TextProvider::measure, &provider, {256, 6}};

    ComputeCosts compute_costs;

    VirtualInspector inspector;

    LayoutNodeId inspector_host;

    LayoutNodeId hierarchy_group;

    LayoutNodeId canvas;

    LayoutNodeId status;

    float requested_width{420};

    std::optional<float> ghost_width;

    std::uint64_t commits{};

    std::size_t selected_key{50002};

    std::vector<std::size_t> folded_rows;

    std::size_t folded_position{};

    EditorScene() {
        provider.font_family = inspector.provider.font_family;
        accepted(document.set_root({Kind::column, {8, 8, 8, 8}, 6}));
        provider.content[1] = {L"Scene  |  Assets  |  Inspector", 16, 1};
        auto toolbar = NodeInput::measured(1);
        toolbar.height = Axis::fixed(30);
        accepted(document.create(document.root(), toolbar));
        auto body_input = NodeInput::row(8);
        body_input.height = Axis::expand();
        const auto body = accepted(document.create(document.root(), body_input));
        auto hierarchy_input = NodeInput::column(4);
        hierarchy_input.width = Axis::fixed(180);
        hierarchy_input.height = Axis::expand();
        const auto hierarchy = accepted(document.create(body, hierarchy_input));
        hierarchy_group = accepted(document.create(hierarchy, NodeInput::column(4)));
        for (std::uint64_t i = 0; i < 8; ++i) {
            provider.content[10 + i] = {L"Scene object " + std::to_wstring(i), 16, 1};
            accepted(document.create(hierarchy_group, NodeInput::measured(10 + i)));
        }
        auto workspace_input = NodeInput::column(6);
        workspace_input.height = Axis::expand();
        const auto workspace = accepted(document.create(body, workspace_input));
        auto canvas_input = NodeInput::box(0, 0);
        canvas_input.width = canvas_input.height = Axis::expand();
        canvas = accepted(document.create(workspace, canvas_input));
        const auto assets = accepted(document.create(workspace, NodeInput::grid(80, 6, 4)));
        for (std::uint64_t i = 0; i < 12; ++i) {
            provider.content[30 + i] = {L"Asset " + std::to_wstring(i), 16, 1};
            accepted(document.create(assets, NodeInput::measured(30 + i)));
        }
        auto host = NodeInput::box(requested_width, 0);
        host.height = Axis::expand();
        inspector_host = accepted(document.create(body, host));
        provider.content[2] = {L"Ready", 16, 1};
        auto status_input = NodeInput::measured(2);
        status_input.height = Axis::fixed(24);
        status = accepted(document.create(document.root(), status_input));
    }

    void begin_drag() {
        require(!ghost_width, "drag already active");
        ghost_width = requested_width;
    }

    void update_drag(float width) {
        require(ghost_width.has_value(), "drag not active");
        ghost_width = std::clamp(width, 220.f, 520.f);
    }

    void cancel_drag() { ghost_width.reset(); }

    void release_drag() {
        require(ghost_width.has_value(), "drag not active");
        requested_width = *ghost_width;
        ghost_width.reset();
        commit();
    }

    void set_status(const wchar_t* value) {
        auto& content = provider.content.at(2);
        content.text = value;
        ++content.revision;
        auto input = accepted(document.input(status));
        input.content_revision = content.revision;
        accepted(document.update(status, input));
    }

    void collapse_properties(std::size_t position, std::size_t count) {
        require(folded_rows.empty() && position <= inspector.keys.size() && count <= inspector.keys.size() - position,
                "invalid property group collapse");
        folded_rows.assign(inspector.keys.begin() + static_cast<std::ptrdiff_t>(position),
                           inspector.keys.begin() + static_cast<std::ptrdiff_t>(position + count));
        folded_position = position;
        inspector.erase(position, count);
    }

    void expand_properties() {
        inspector.insert(folded_position, folded_rows);
        folded_rows.clear();
    }

    void commit() {
        ++commits;
        auto host = accepted(document.input(inspector_host));
        host.width = Axis::fixed(requested_width);
        accepted(document.update(inspector_host, host));
        const auto shell = compute_costs.run(document, provider, {1200, 720});
        auto complete_shell = published.shell;
        if (!complete_shell || complete_shell->geometry.publication != shell.publication)
            complete_shell = TextFrame::capture(document, provider);
        const auto bounds = box(shell, inspector_host);
        inspector.realize(inspector.list.range().scroll, bounds.width, bounds.height);
        // One consumer publication after all participating layout domains succeed.
        published = {std::move(complete_shell), inspector.output, published.serial + 1};
    }

    void check() const {
        published.shell->check();
        published.inspector.frame->check();
        const auto& host = box(published.shell->geometry.view(), inspector_host);
        require(host.width == published.inspector.frame->geometry.viewport.width &&
                    host.height == published.inspector.frame->geometry.viewport.height,
                "panel/frame viewport mismatch");
        require(inspector.active.size() < 64 && inspector.provider.paragraphs.size() < 128,
                "editor lost virtualization");
    }
};

void editor_session_end_to_end() {
    EditorScene scene;
    scene.commit();
    scene.check();
    const auto initial = scene.published;
    const auto shell_visits = scene.document.stats().node_visits;
    const auto inspector_visits = scene.inspector.document.stats().node_visits;
    const auto calls = scene.provider.calls + scene.inspector.provider.calls;
    const auto commits = scene.commits;
    scene.begin_drag();
    for (unsigned i = 0; i < 64; ++i)
        scene.update_drag(220.f + i * 4.f);
    scene.cancel_drag();
    require(scene.published.serial == initial.serial && scene.commits == commits &&
                scene.document.stats().node_visits == shell_visits &&
                scene.inspector.document.stats().node_visits == inspector_visits &&
                scene.provider.calls + scene.inspector.provider.calls == calls,
            "ghost/cancel performed layout work");
    scene.begin_drag();
    scene.update_drag(280);
    scene.release_drag();
    scene.check();
    require(scene.commits == commits + 1 && scene.published.serial == initial.serial + 1 &&
                box(scene.published.shell->geometry.view(), scene.inspector_host).width == 280,
            "release did not commit final width once");
    initial.shell->check();
    initial.inspector.frame->check();

    // Selection and edits belong to the business key, not the visible row slot.
    scene.inspector.edit(50002, L"Persistent selected value");
    accepted(scene.inspector.list.set_viewport(accepted(scene.inspector.list.offset(50000)), 600, 80));
    scene.commit();
    scene.check();
    const auto selected_node = scene.inspector.active.at(scene.selected_key).node;
    const auto stable_canvas = box(scene.published.shell->geometry.view(), scene.canvas);
    const auto shell_before = scene.document.stats();
    const std::array<std::size_t, 2> inserted{100000, 100001};
    scene.inspector.insert(0, inserted);
    scene.commit();
    scene.check();
    require(scene.inspector.active.at(scene.selected_key).node == selected_node &&
                scene.inspector.keys[50004] == scene.selected_key,
            "insertion above view rebound stable business identity");
    scene.inspector.erase(0, 2);
    scene.commit();
    require(scene.document.stats().node_visits == shell_before.node_visits &&
                box(scene.published.shell->geometry.view(), scene.canvas) == stable_canvas,
            "list edits recomputed unrelated shell geometry");

    auto group = accepted(scene.document.input(scene.hierarchy_group));
    group.collapsed = true;
    accepted(scene.document.update(scene.hierarchy_group, group));
    scene.set_status(L"Property group collapsed");
    const auto before_batch = scene.commits;
    scene.collapse_properties(50001, 4);
    scene.commit();
    scene.check();
    require(scene.commits == before_batch + 1 && !scene.inspector.active.contains(scene.selected_key) &&
                !scene.published.shell->geometry.find(scene.hierarchy_group),
            "batched collapse did not update both domains");
    scene.expand_properties();
    group.collapsed = false;
    accepted(scene.document.update(scene.hierarchy_group, group));
    scene.commit();
    scene.check();
    require(scene.inspector.active.contains(scene.selected_key) &&
                scene.inspector.provider.content.at(scene.selected_key * 2 + 2).text == L"Persistent selected value",
            "collapsed business state was lost or assigned to a recycled row");

    // Font changes invalidate ALL measured height estimates at the consumer seam.
    scene.inspector.provider.scale = 1.25f;
    ++scene.inspector.provider.epoch;
    scene.commit();
    scene.check();
    require(accepted(scene.inspector.list.offset(1)) == 54 &&
                scene.inspector.output.frame->geometry.provider_revision == scene.inspector.provider.epoch,
            "provider change left stale offscreen measured heights");

    auto held = scene.published;
    std::weak_ptr<const TextProvider::Paragraph> retired = held.inspector.frame->runs.front().text.paragraph;
    // Partially prepare a different window at a different width, then fail.
    accepted(scene.inspector.list.set_viewport(accepted(scene.inspector.list.offset(90000)), 600, 80));
    scene.requested_width = 500;
    scene.inspector.provider.fail_at = scene.inspector.provider.calls + 3;
    bool failed = false;
    try {
        scene.commit();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed && scene.published.serial == held.serial && scene.published.shell == held.shell &&
                scene.published.inspector.frame == held.inspector.frame,
            "partial editor submission escaped");
    held.shell->check();
    held.inspector.frame->check();
    scene.inspector.provider.fail_at = 0;
    scene.commit();
    scene.check();
    require(scene.published.serial == held.serial + 1 && scene.inspector.active.contains(90000),
            "editor submission failed to recover");
    held = {};
    require(retired.expired(), "retired visible resources survived their last owner");
}

template <class Action>
void timed_editor(const char* name, EditorScene& scene, Action action, unsigned samples = 80) {
    for (unsigned i = 0; i < 8; ++i)
        action(i);
    const auto shell = scene.document.stats();
    const auto list = scene.inspector.document.stats();
    const auto compute_before = scene.compute_costs.total + scene.inspector.compute_costs.total;
    const auto measure_before = scene.compute_costs.measure + scene.inspector.compute_costs.measure;
    const auto creates = scene.provider.creates + scene.inspector.provider.creates;
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
    const auto compute_us = (scene.compute_costs.total + scene.inspector.compute_costs.total - compute_before) / 1000.0;
    const auto measure_us =
        (scene.compute_costs.measure + scene.inspector.compute_costs.measure - measure_before) / 1000.0;
    const auto after_shell = scene.document.stats();
    const auto after_list = scene.inspector.document.stats();
    std::sort(times.begin(), times.end());
    std::printf(
        "{\"kind\":\"editor\",\"scenario\":\"%s\",\"samples\":%u,\"median_us\":%.3f,\"p95_us\":%.3f,\"compute_mean_us\":%.3f,\"measure_in_compute_mean_us\":%.3f,\"compute_outside_measure_mean_us\":%.3f,\"consumer_mean_us\":%.3f,\"measure_calls\":%llu,\"node_visits\":%llu,\"paragraph_creates\":%llu,\"buffer_allocations\":%llu}\n",
        name, samples, times[samples / 2], times[samples * 95 / 100], compute_us / samples, measure_us / samples,
        (compute_us - measure_us) / samples, (total_us - compute_us) / samples,
        after_shell.measure_calls + after_list.measure_calls - shell.measure_calls - list.measure_calls,
        after_shell.node_visits + after_list.node_visits - shell.node_visits - list.node_visits,
        scene.provider.creates + scene.inspector.provider.creates - creates,
        after_shell.buffer_allocations + after_list.buffer_allocations - shell.buffer_allocations -
            list.buffer_allocations);
}

void editor_performance() {
    EditorScene scene;
    scene.commit();
    std::printf("{\"kind\":\"editor_environment\",\"logical_rows\":%zu,\"explicit_cjk_font\":%s}\n",
                scene.inspector.list.size(), scene.provider.font_family == L"Microsoft YaHei UI" ? "true" : "false");
    timed_editor("static", scene, [&](unsigned) { scene.commit(); });
    scene.begin_drag();
    timed_editor(
        "ghost_update", scene, [&](unsigned i) { scene.update_drag(220.f + static_cast<float>(i % 64) * 4); }, 1000);
    scene.cancel_drag();
    timed_editor(
        "release_resize", scene,
        [&](unsigned i) {
            scene.begin_drag();
            scene.update_drag(i % 2 ? 280.f : 500.f);
            scene.release_drag();
        },
        40);
    timed_editor("visible_property_edit", scene, [&](unsigned i) {
        scene.inspector.edit(0, i % 2 ? L"Short property"
                                      : L"Changing this visible property updates its natural wrapped height.");
        scene.commit();
    });
    timed_editor("row_scroll", scene, [&](unsigned i) {
        accepted(scene.inspector.list.set_viewport(i * 25.0, 628, 80));
        scene.commit();
    });
    timed_editor(
        "distant_jump", scene,
        [&](unsigned i) {
            accepted(scene.inspector.list.set_viewport(accepted(scene.inspector.list.offset(i % 2 ? 50000 : 90000)),
                                                       628, 80));
            scene.commit();
        },
        40);
    timed_editor("fixed_status_edit", scene, [&](unsigned i) {
        scene.set_status(i % 2 ? L"Ready" : L"Selection updated");
        scene.commit();
    });
    timed_editor(
        "font_policy_change", scene,
        [&](unsigned i) {
            scene.inspector.provider.scale = i % 2 ? 1.f : 1.25f;
            ++scene.inspector.provider.epoch;
            scene.commit();
        },
        20);
    scene.check();
    std::printf(
        "{\"kind\":\"editor_resources\",\"maximum_realized_rows\":%zu,\"cached_paragraphs\":%zu,\"retained_visible_paragraphs\":%zu,\"logical_index_bytes\":%zu,\"business_key_bytes\":%zu,\"document_peak_buffer_bytes\":%llu}\n",
        scene.inspector.maximum_realized, scene.inspector.provider.paragraphs.size(),
        scene.published.inspector.frame->runs.size(), scene.inspector.list.storage_bytes(),
        scene.inspector.keys.capacity() * sizeof(std::size_t),
        scene.document.stats().peak_buffer_bytes + scene.inspector.document.stats().peak_buffer_bytes);
}
