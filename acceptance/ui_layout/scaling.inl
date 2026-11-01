void text_cost_case(const char* name, const wchar_t* family, const wchar_t* text, bool wrap) {
    TextProvider provider;
    provider.font_family = family;
    provider.wrap_enabled = wrap;
    LayoutDocument document(TextProvider::measure, &provider, {65, 1});
    for (std::uint64_t i = 1; i <= 64; ++i) {
        provider.content[i] = {text, 16, 1};
        accepted(document.create(document.root(), NodeInput::measured(i)));
    }
    accepted(document.compute({120, 600}, 1));
    const auto cold = provider.costs;
    std::vector<double> times;
    const auto creates = provider.creates;
    for (unsigned i = 0; i < 40; ++i) {
        const auto start = std::chrono::steady_clock::now();
        accepted(document.compute({i % 2 ? 120.f : 340.f, 600}, 1));
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    const auto warm = provider.costs;
    require(provider.creates == creates, "profiling resize recreated text objects");
    std::sort(times.begin(), times.end());
    for (unsigned i = 0; i < 256; ++i)
        (void)provider.metrics(provider.paragraphs.at(1)->layout.Get(), 120);
    std::printf(
        "{\"kind\":\"text_cost\",\"scenario\":\"%s\",\"paragraphs\":64,\"resizes\":40,\"median_us\":%.3f,\"cold_format_us\":%.3f,\"cold_create_us\":%.3f,\"cold_metrics_us\":%.3f,\"warm_format_us\":%.3f,\"warm_create_us\":%.3f,\"warm_wrapping_us\":%.3f,\"warm_width_us\":%.3f,\"warm_metrics_us\":%.3f,\"same_width_metrics_256_us\":%.3f}\n",
        name, times[20], cold.format / 1000.0, cold.create / 1000.0, cold.metrics / 1000.0,
        (warm.format - cold.format) / 1000.0, (warm.create - cold.create) / 1000.0,
        (warm.wrapping - cold.wrapping) / 1000.0, (warm.width - cold.width) / 1000.0,
        (warm.metrics - cold.metrics) / 1000.0, (provider.costs.metrics - warm.metrics) / 1000.0);
}

void text_costs() {
    constexpr auto chinese = L"可动态编辑的属性内容，随实际列宽换行。 Value with trailing spaces  ";
    text_cost_case("chinese_fallback_wrap", L"Segoe UI", chinese, true);
    text_cost_case("chinese_fallback_no_wrap", L"Segoe UI", chinese, false);
    text_cost_case("ascii_wrap", L"Segoe UI", L"Editable property value with several words and trailing spaces  ",
                   true);
    TextProvider fonts;
    ComPtr<IDWriteFontCollection> collection;
    hr(fonts.factory->GetSystemFontCollection(collection.GetAddressOf()));
    UINT32 index{};
    BOOL found{};
    hr(collection->FindFamilyName(L"Microsoft YaHei UI", &index, &found));
    if (found)
        text_cost_case("chinese_explicit_yahei_ui_wrap", L"Microsoft YaHei UI", chinese, true);
    else
        std::puts("{\"kind\":\"text_cost\",\"scenario\":\"chinese_explicit_yahei_ui_wrap\",\"available\":false}");
}

void content_invalidation_scope() {
    TextProvider p;
    p.content[1] = {L"fixed content", 16, 1};
    p.content[2] = {L"natural content", 16, 1};
    LayoutDocument d(TextProvider::measure, &p, {5, 2});
    auto fixed = NodeInput::measured(1);
    fixed.width = Axis::fixed(100);
    fixed.height = Axis::fixed(20);
    const auto fixed_id = accepted(d.create(d.root(), fixed));
    const auto natural_id = accepted(d.create(d.root(), NodeInput::measured(2)));
    accepted(d.compute({200, 300}, 1));
    const auto before = d.stats();
    const auto publication = d.view().publication;
    for (unsigned i = 0; i < 8; ++i) {
        fixed.content_revision = ++p.content[1].revision;
        accepted(d.update(fixed_id, fixed));
        accepted(d.compute({200, 300}, 1));
    }
    require(d.stats().geometry_reuses == before.geometry_reuses + 8 && d.stats().node_visits == before.node_visits &&
                d.stats().measure_calls == before.measure_calls && d.view().publication == publication + 8 &&
                d.view().input_revision == d.input_revision(),
            "unused content invalidated geometry or lost publication");
    auto natural = accepted(d.input(natural_id));
    natural.content_revision = ++p.content[2].revision;
    p.content[2].text = L"真实自然高度依赖仍然必须重新计算，不能因为之前复用了几何而丢失依赖。";
    accepted(d.update(natural_id, natural));
    accepted(d.compute({200, 300}, 1));
    require(d.stats().measure_calls == before.measure_calls + 1 && d.stats().node_visits > before.node_visits,
            "geometry reuse erased measured dependency");
    // Fixed height still has an intrinsic-width dependency when width is Content.
    fixed.width = Axis::content();
    accepted(d.update(fixed_id, fixed));
    accepted(d.compute({200, 300}, 1));
    const auto width_before = d.view().find(fixed_id)->width;
    p.content[1].text = L"Much longer independent width";
    fixed.content_revision = ++p.content[1].revision;
    accepted(d.update(fixed_id, fixed));
    accepted(d.compute({200, 300}, 1));
    require(d.view().find(fixed_id)->width > width_before,
            "fixed height incorrectly suppressed intrinsic width invalidation");
    natural.collapsed = true;
    accepted(d.update(natural_id, natural));
    accepted(d.compute({200, 300}, 1));
    const auto collapsed_visits = d.stats().node_visits;
    natural.content_revision = ++p.content[2].revision;
    accepted(d.update(natural_id, natural));
    accepted(d.compute({200, 300}, 1));
    require(d.stats().node_visits == collapsed_visits && !d.view().find(natural_id),
            "hidden content unnecessarily laid out");
    natural.collapsed = false;
    accepted(d.update(natural_id, natural));
    p.fail_at = p.calls + 1;
    const auto held = d.snapshot();
    rejected(d.compute({200, 300}, 1), ErrorCode::measurement_failed);
    require(d.view().publication == held.publication && !d.view().find(natural_id),
            "failed restore published geometry");
    p.fail_at = 0;
    require(accepted(d.compute({200, 300}, 1)).find(natural_id) != nullptr, "restore did not retry dependency");
}

void virtual_range_and_anchor() {
    VirtualList list(100010, 2);
    accepted(list.reset(100000, 20));
    accepted(list.set_viewport(220005, 100, 22));
    auto range = list.range();
    require(range.begin == 9999 && range.end == 10006 && range.local_origin == -27 && range.extent == 2199998,
            "visible half-open range or estimated extent");
    accepted(list.set_height(0, 40));
    require(list.range().scroll == 220025 && list.range().begin == range.begin,
            "height change above view moved the anchor");
    accepted(list.insert(0, 3, 20));
    require(list.range().scroll == 220091 && list.range().begin == range.begin + 3, "insertion lost logical anchor");
    accepted(list.erase(0, 3));
    require(list.range().scroll == 220025, "erasure failed to restore anchor");
    accepted(list.invalidate_heights(30));
    require(list.range().scroll == 320005, "width invalidation lost top row and within-row offset");
    const auto revision = list.range().revision;
    rejected(list.set_height(100000, 1), ErrorCode::invalid_input);
    rejected(list.set_height(0, 0), ErrorCode::invalid_input);
    rejected(list.insert(0, 20, 30), ErrorCode::capacity_exceeded);
    rejected(list.set_viewport(std::numeric_limits<double>::infinity(), 100), ErrorCode::invalid_input);
    require(list.range().revision == revision, "invalid list edit mutated state");
    // Extent is far outside float pixel precision; only a small local origin is consumed.
    accepted(list.reset(100000, 10000000));
    accepted(list.set_viewport(900000180003.25, 480, 0));
    range = list.range();
    require(range.extent > 1e11 && range.begin == 90000 && range.local_origin == -3.25,
            "large extent lost fractional local coordinates");
    accepted(list.erase(0, list.size()));
    require(list.range().begin == 0 && list.range().end == 0 && list.range().extent == 0 && list.range().scroll == 0,
            "empty list state");
    accepted(list.reset(4, 20));
    accepted(list.set_viewport(10000, 100));
    require(list.range().scroll == 0 && list.range().end == 4, "short list scroll clamp");
    accepted(list.set_viewport(0, 0));
    require(list.range().end == 0, "zero viewport must realize no rows");
}

struct VirtualInspector {
    struct Row {
        LayoutNodeId node;
        LayoutNodeId label;
        LayoutNodeId value;
    };
    struct Output {
        std::shared_ptr<const TextFrame> frame;
        VisibleRange range;
    };
    TextProvider provider;
    LayoutDocument document{TextProvider::measure, &provider, {2048, 4}};
    VirtualList list{200000, 6};
    LayoutNodeId table;
    std::map<std::size_t, Row> active;
    std::map<std::size_t, std::pair<std::wstring, std::uint64_t>> edits;
    std::vector<std::size_t> keys;
    Output output;
    ComputeCosts compute_costs;
    std::uint64_t height_provider_revision{};
    float width{};
    std::size_t maximum_realized{};
    VirtualInspector() {
        ComPtr<IDWriteFontCollection> fonts;
        hr(provider.factory->GetSystemFontCollection(fonts.GetAddressOf()));
        UINT32 index{};
        BOOL found{};
        hr(fonts->FindFamilyName(L"Microsoft YaHei UI", &index, &found));
        if (found)
            provider.font_family = L"Microsoft YaHei UI";
        // Explicit fixed label policy: scrolling never changes shared column width.
        table = accepted(
            document.create(document.root(), NodeInput::table({Axis::fixed(120), Axis::expand(1, 40)}, 12, 6)));
        accepted(list.reset(100000, 48));
        keys.reserve(list.size());
        for (std::size_t key = 0; key < list.size(); ++key)
            keys.push_back(key);
    }
    void edit(std::size_t position, const wchar_t* text) {
        const auto key = keys.at(position);
        auto& value = edits[key];
        value.first = text;
        value.second = value.second ? value.second + 1 : 2;
        if (const auto found = active.find(key); found != active.end()) {
            provider.content[key * 2 + 2] = {value.first, 16, value.second};
            accepted(document.update(found->second.value, NodeInput::measured(key * 2 + 2, value.second)));
        }
        accepted(list.set_height(position, 48)); // Unseen data remains an estimate until realized.
    }
    void insert(std::size_t position, std::span<const std::size_t> inserted) {
        require(position <= keys.size(), "invalid business insertion");
        auto candidate = keys;
        candidate.insert(candidate.begin() + static_cast<std::ptrdiff_t>(position), inserted.begin(), inserted.end());
        accepted(list.insert(position, inserted.size(), 48));
        keys.swap(candidate);
    }
    void erase(std::size_t position, std::size_t count) {
        accepted(list.erase(position, count));
        keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(position),
                   keys.begin() + static_cast<std::ptrdiff_t>(position + count));
    }
    LayoutView realize(double scroll, float new_width = 420, float height = 480) {
        accepted(list.set_viewport(scroll, height, 80));
        if (width != new_width || height_provider_revision != provider.epoch) {
            accepted(list.invalidate_heights(48));
            width = new_width;
            height_provider_revision = provider.epoch;
        }
        for (std::size_t pass = 0; pass <= list.size(); ++pass) {
            const auto range = list.range();
            const auto visible = std::span<const std::size_t>(keys).subspan(range.begin, range.end - range.begin);
            for (auto it = active.begin(); it != active.end();) {
                if (std::find(visible.begin(), visible.end(), it->first) != visible.end()) {
                    ++it;
                    continue;
                }
                accepted(document.remove(it->second.node));
                for (auto key : {it->first * 2 + 1, it->first * 2 + 2}) {
                    provider.content.erase(key);
                    provider.paragraphs.erase(key);
                }
                it = active.erase(it);
            }
            for (auto position = range.begin; position < range.end; ++position) {
                const auto key = keys[position];
                if (const auto found = active.find(key); found != active.end()) {
                    accepted(document.move(found->second.node, table, position - range.begin));
                    continue;
                }
                provider.content[key * 2 + 1] = {L"属性 " + std::to_wstring(key), 16, 1};
                const auto edit = edits.find(key);
                provider.content[key * 2 + 2] =
                    edit == edits.end()
                        ? TextProvider::Content{key % 3 ? L"可编辑内容 Value"
                                                        : L"较长的属性内容会在可用宽度内自动换行并改变实际行高。",
                                                16, 1}
                        : TextProvider::Content{edit->second.first, 16, edit->second.second};
                const auto node = accepted(document.create(table, NodeInput::row(), position - range.begin));
                try {
                    const auto label = accepted(document.create(node, NodeInput::measured(key * 2 + 1)));
                    const auto value = accepted(document.create(
                        node, NodeInput::measured(key * 2 + 2, provider.content.at(key * 2 + 2).revision)));
                    active.emplace(key, Row{node, label, value});
                } catch (...) {
                    accepted(document.remove(node));
                    throw;
                }
            }
            maximum_realized = std::max(maximum_realized, active.size());
            const auto view = compute_costs.run(document, provider, {new_width, height});
            for (auto position = range.begin; position < range.end; ++position)
                accepted(list.set_height(position, box(view, active.at(keys[position]).node).height));
            const auto adjusted = list.range();
            if (adjusted.begin == range.begin && adjusted.end == range.end) {
                // Intermediate successful computes may precede a later failure.
                // Own the final geometry and publish its viewport origin together.
                auto complete = output.frame;
                if (!complete || complete->geometry.publication != view.publication)
                    complete = TextFrame::capture(document, provider);
                output = {std::move(complete), adjusted};
                return output.frame->geometry.view();
            }
        }
        throw std::runtime_error("Virtual row estimates did not converge");
    }
};

void virtual_inspector_end_to_end() {
    VirtualInspector scene;
    auto view = scene.realize(0);
    require(scene.active.size() < 64 && scene.document.node_count() < 194 && scene.provider.paragraphs.size() < 128,
            "virtual view instantiated logical collection");
    auto calls = scene.provider.calls;
    auto visits = scene.document.stats().node_visits;
    const auto old_range = scene.list.range();
    view = scene.realize(1);
    require(scene.provider.calls == calls && scene.document.stats().node_visits == visits &&
                scene.list.range().begin == old_range.begin && scene.list.range().local_origin == -1,
            "sub-row scrolling invalidated text/geometry instead of composition origin");
    const auto stable = scene.active.begin()->second.node;
    calls = scene.provider.calls;
    scene.edit(50000, L"屏幕外内容变化不会立即触发文字排版");
    view = scene.realize(scene.list.range().scroll);
    require(scene.provider.calls == calls && view.find(stable), "offscreen edit invalidated visible text");
    scene.edit(0, L"当前可见行改变，需要正确测量这一项并更新滚动范围。更新内容足够长以跨越多行。");
    calls = scene.provider.calls;
    view = scene.realize(scene.list.range().scroll);
    require(scene.provider.calls > calls && view.find(stable), "visible update lost row identity or measurement");
    view = scene.realize(accepted(scene.list.offset(50000)));
    require(scene.active.contains(50000) && scene.provider.content.at(100002).revision == 2 &&
                scene.active.size() < 64 && scene.provider.paragraphs.size() < 128,
            "jump or provider cache eviction");
    // A caller commits the final width here; this consumer does not simulate UI input.
    const float pending_width = 399;
    view = scene.realize(scene.list.range().scroll, pending_width);
    require(view.viewport.width == 399 && scene.active.size() < 64, "committed width failed to reflow visible rows");
    for (const auto& [row, ids] : scene.active) {
        const auto& cell = box(view, ids.value);
        require(cell.x == 132 && close(cell.height, scene.provider.fresh(row * 2 + 2, cell.width).height),
                "virtual shared column or real metrics mismatch");
        const auto expected_y = accepted(scene.list.offset(row)) - scene.output.range.scroll;
        const auto consumed_y = scene.output.range.local_origin + box(view, ids.node).y;
        require(std::abs(expected_y - consumed_y) < .001, "visible geometry disagrees with corrected logical offsets");
    }
    const auto retained = scene.output;
    scene.provider.scale = 1.25f;
    ++scene.provider.epoch;
    scene.provider.fail_at = scene.provider.calls + 1;
    bool failed = false;
    try {
        scene.realize(scene.list.range().scroll, pending_width);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed && scene.output.frame == retained.frame && scene.output.range.scroll == retained.range.scroll &&
                scene.output.range.begin == retained.range.begin,
            "failed visible measurement published a mismatched frame");
    scene.provider.fail_at = 0;
    view = scene.realize(scene.list.range().scroll, pending_width);
    require(view.provider_revision == scene.provider.epoch && scene.active.size() < 64,
            "virtual view failed to recover");
    // Fail after one successful estimate-correction pass, not only on the first compute.
    VirtualInspector deferred;
    deferred.realize(0, 2000);
    const auto complete_publication = deferred.output.frame->geometry.publication;
    const auto complete_range = deferred.output.range;
    ++deferred.provider.epoch;
    // The consumer installs 48px estimates + 6px gaps for 480px + 80px overscan.
    const auto estimated_rows = static_cast<std::uint64_t>(std::ceil(560.0 / 54));
    deferred.provider.fail_at = deferred.provider.calls + estimated_rows * 2 + 1;
    failed = false;
    try {
        deferred.realize(0, 2000);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed && deferred.document.view().publication > complete_publication &&
                deferred.output.frame->geometry.publication == complete_publication &&
                deferred.output.range.end == complete_range.end,
            "later correction failure replaced the last complete visible frame");
    deferred.provider.fail_at = 0;
    deferred.realize(0, 2000);
    require(deferred.output.frame->geometry.publication > complete_publication, "multi-pass failure did not recover");
}

void virtual_performance() {
    const auto start = std::chrono::steady_clock::now();
    VirtualInspector scene;
    scene.realize(0);
    std::printf(
        "{\"kind\":\"virtual\",\"logical_rows\":%zu,\"collection_bytes\":%zu,\"cold_us\":%.3f,\"explicit_cjk_font\":%s}\n",
        scene.list.size(), scene.list.storage_bytes(),
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count(),
        scene.provider.font_family == L"Microsoft YaHei UI" ? "true" : "false");
    timed_scene("virtual_static", scene, [&](unsigned) { scene.realize(scene.list.range().scroll); }, 1000);
    timed_scene("virtual_subrow_scroll", scene, [&](unsigned i) { scene.realize(i % 2 ? 1 : 2); });
    timed_scene("virtual_row_scroll", scene, [&](unsigned i) { scene.realize(i * 25.0); });
    timed_scene(
        "virtual_jump", scene, [&](unsigned i) { scene.realize(accepted(scene.list.offset(i % 2 ? 50000 : 90000))); },
        40);
    timed_scene(
        "virtual_committed_resize", scene,
        [&](unsigned i) { scene.realize(scene.list.range().scroll, i % 2 ? 280.f : 500.f); }, 40);
    timed_scene("virtual_offscreen_edit", scene, [&](unsigned) {
        scene.edit(1, L"unseen update");
        scene.realize(scene.list.range().scroll, 500);
    });
    std::printf(
        "{\"kind\":\"virtual\",\"maximum_realized_rows\":%zu,\"paragraph_cache\":%zu,\"logical_extent\":%.3f}\n",
        scene.maximum_realized, scene.provider.paragraphs.size(), scene.list.range().extent);
}
