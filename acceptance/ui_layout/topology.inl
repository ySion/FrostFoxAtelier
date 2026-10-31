void final_tree_transactions() {
    LayoutDocument d(nullptr, nullptr, {8, 4});
    const auto a = accepted(d.create(d.root(), column()));
    const auto b = accepted(d.create(a, column()));
    const auto x = accepted(d.create(b, fixed_box(30, 10)));
    const auto y = accepted(d.create(a, fixed_box(40, 20)));
    const auto z = accepted(d.create(d.root(), fixed_box(50, 30)));
    const auto old = accepted(d.compute({200, 300}, 1));
    const auto held = d.snapshot();
    // Parent-first input need not be depth-first: Y precedes A's child X.
    std::vector<TreeNodeInput> tree{{b, {}, column()},
                                    {a, 0, column()},
                                    {y, 0, fixed_box(40, 20)},
                                    {x, 1, fixed_box(30, 10)},
                                    {z, {}, fixed_box(50, 30)}};
    const auto revision = d.input_revision();
    const auto reversed = accepted(d.set_tree(tree));
    require(reversed == std::vector<LayoutNodeId>{b, a, y, x, z} && d.input_revision() == revision + 1,
            "ancestor reversal replaced identities or made several commits");
    require(d.view().publication == old.publication && d.view().boxes.data() == old.boxes.data() &&
                box(d.view(), b).parent == a,
            "tree edit changed accepted geometry before compute");
    const auto result = accepted(d.compute({200, 300}, 1));
    require(box(result, b).parent == d.root() && box(result, a).parent == b && box(result, x).parent == a &&
                box(result, y).parent == b && box(result, y).y > box(result, x).y,
            "final ancestor reversal or sibling order was not applied");
    // Remove B while its retained descendants escape, and turn A into a leaf
    // in the same transaction that moves A's child to the root.
    tree = {
        {a, {}, fixed_box(60, 5)}, {x, {}, fixed_box(30, 10)}, {y, {}, fixed_box(40, 20)}, {z, {}, fixed_box(50, 30)}};
    require(accepted(d.set_tree(tree)) == std::vector<LayoutNodeId>{a, x, y, z},
            "escaped descendants changed identity");
    rejected(d.input(b), ErrorCode::invalid_node);
    const auto escaped = accepted(d.compute({200, 300}, 1));
    require(d.node_count() == 5 && box(escaped, a).parent == d.root() && box(escaped, x).parent == d.root() &&
                close(box(escaped, x).y, 5) && close(box(escaped, y).y, 15),
            "retiring ancestor removed retained descendants or leaf conversion failed");
    const auto before_noop = d.stats();
    const auto unchanged = d.input_revision();
    accepted(d.set_tree(tree));
    require(d.input_revision() == unchanged && d.stats().topology_commits == before_noop.topology_commits &&
                d.stats().topology_links_written == before_noop.topology_links_written &&
                accepted(d.compute({200, 300}, 1)).publication == escaped.publication,
            "identical final tree did work");
    accepted(d.set_tree({}));
    require(d.node_count() == 1 && accepted(d.compute({200, 300}, 1)).boxes.size() == 1,
            "empty final tree did not retire all explicit nodes");
    require(held.find(a) && held.find(b) && held.find(x)->parent == b,
            "owned old geometry lost removed nodes or original topology");
}

void final_tree_validation_atomicity() {
    LayoutDocument d(nullptr, nullptr, {5, 2}), foreign(nullptr, nullptr, {2, 1});
    const auto a = accepted(d.create(d.root(), column()));
    const auto b = accepted(d.create(d.root(), column()));
    const auto c = accepted(d.create(d.root(), fixed_box(10, 10)));
    const auto stale = accepted(d.create(d.root(), fixed_box(1, 1)));
    accepted(d.remove(stale));
    const auto alien = accepted(foreign.create(foreign.root(), fixed_box(1, 1)));
    const auto initial = accepted(d.compute({100, 100}, 1));
    const auto revision = d.input_revision();
    const auto statistics = d.stats();
    const std::array ids{a, b, c};
    const std::array inputs{accepted(d.input(a)), accepted(d.input(b)), accepted(d.input(c))};
    const std::vector<TreeNodeInput> valid{{a, {}, column()}, {b, {}, column()}, {c, {}, fixed_box(10, 10)}};
    const auto refuse = [&](std::vector<TreeNodeInput> candidate, ErrorCode error, std::optional<std::size_t> index) {
        const auto result = d.set_tree(candidate);
        rejected(result, error);
        if (index)
            require(result.error().tree_index == index, "batch error lost its input position");
        require(d.node_count() == 4 && d.input_revision() == revision &&
                    d.stats().topology_commits == statistics.topology_commits &&
                    d.stats().topology_links_written == statistics.topology_links_written,
                "rejected tree changed nodes, revision or committed topology");
        for (std::size_t i = 0; i < ids.size(); ++i)
            require(accepted(d.input(ids[i])) == inputs[i], "rejected tree partially updated an earlier input");
        require(d.view().publication == initial.publication && d.view().boxes.data() == initial.boxes.data(),
                "rejected tree changed accepted geometry");
    };
    auto candidate = valid;
    candidate[0].input.padding.left = 7;
    candidate[2].node = alien;
    refuse(candidate, ErrorCode::invalid_node, 2);
    candidate[2].node = stale;
    refuse(candidate, ErrorCode::invalid_node, 2);
    candidate[2].node = a;
    refuse(candidate, ErrorCode::invalid_structure, 2);
    candidate[2].node = d.root();
    refuse(candidate, ErrorCode::root_operation, 2);
    candidate = valid;
    candidate[1].parent = 2;
    refuse(candidate, ErrorCode::invalid_structure, 1);
    candidate[1].parent = 1;
    refuse(candidate, ErrorCode::invalid_structure, 1);
    candidate = valid;
    candidate[0].input = fixed_box(5, 5);
    candidate[2].parent = 0;
    refuse(candidate, ErrorCode::invalid_structure, 2);
    candidate = valid;
    candidate[1].parent = 0;
    candidate[2].parent = 1;
    refuse(candidate, ErrorCode::capacity_exceeded, 2);
    candidate = valid;
    candidate[0].input.padding.left = 7;
    candidate[2].input.width = Axis::fixed(-1);
    refuse(candidate, ErrorCode::invalid_input, 2);
    candidate = valid;
    candidate.push_back({{}, {}, fixed_box(1, 1)});
    candidate.push_back({{}, {}, fixed_box(1, 1)});
    refuse(candidate, ErrorCode::capacity_exceeded, {});
    require(accepted(d.set_tree(valid)) == std::vector<LayoutNodeId>{a, b, c} &&
                accepted(d.compute({100, 100}, 1)).publication == initial.publication,
            "rejected batches dirtied a subsequent unchanged tree");
}

void final_tree_capacity_reuse() {
    LayoutDocument d(nullptr, nullptr, {4, 3});
    std::vector<TreeNodeInput> tree{{{}, {}, column()}, {{}, 0, column()}, {{}, 1, fixed_box(10, 10)}};
    const auto ids = accepted(d.set_tree(tree));
    accepted(d.compute({100, 100}, 1));
    const auto held = d.snapshot();
    // The pool is full. The old ancestor is retired while its child escapes;
    // a new sibling must reuse that slot without inheriting the ancestor's ID.
    tree = {{ids[0], {}, column()}, {ids[2], 0, fixed_box(10, 10)}, {{}, 0, fixed_box(20, 20)}};
    const auto replacement = accepted(d.set_tree(tree));
    require(replacement[0] == ids[0] && replacement[1] == ids[2] && replacement[2] != ids[1] && d.node_count() == 4,
            "full-pool replacement failed to retain nodes or advance the reused generation");
    rejected(d.input(ids[1]), ErrorCode::invalid_node);
    auto view = accepted(d.compute({100, 100}, 1));
    require(!view.find(ids[1]) && box(view, ids[2]).parent == ids[0] && held.find(ids[1]) &&
                held.find(ids[2])->parent == ids[1],
            "reused slot aliased old publication or retained descendants");
    tree = {{replacement[2], {}, fixed_box(20, 20)}};
    accepted(d.set_tree(tree));
    const auto created = accepted(d.create(d.root(), column()));
    const auto descendant = accepted(d.create(created, fixed_box(3, 4)));
    accepted(d.remove(created));
    tree = {{replacement[2], {}, fixed_box(20, 20)}, {{}, {}, column()}, {{}, 1, fixed_box(5, 6)}};
    const auto rebuilt = accepted(d.set_tree(tree));
    require(rebuilt[1] != created && rebuilt[2] != descendant && d.node_count() == 4,
            "bulk and individual edits disagreed about the reusable-slot chain");
    view = accepted(d.compute({100, 100}, 1));
    require(box(view, rebuilt[2]).parent == rebuilt[1] && close(box(view, rebuilt[2]).height, 6),
            "recycled subtree retained stale children or input");
}

void final_tree_measurement_recovery() {
    TextProvider provider;
    provider.content[1] = {L"First real paragraph", 16, 1};
    provider.content[2] = {L"Second real paragraph", 16, 1};
    LayoutDocument d(TextProvider::measure, &provider, {4, 2});
    const auto parent = accepted(d.create(d.root(), column()));
    const auto a = accepted(d.create(parent, text(1))), b = accepted(d.create(parent, text(2)));
    const auto initial = accepted(d.compute({160, 200}, provider.epoch));
    const auto held = d.snapshot();
    for (auto& [key, content] : provider.content) {
        ++content.revision;
        content.text += L" changes and wraps at the constrained width.";
    }
    std::vector<TreeNodeInput> tree{{b, {}, text(2, 2)}, {parent, {}, column()}, {a, 1, text(1, 2)}};
    accepted(d.set_tree(tree));
    provider.active = &d;
    provider.active_leaf = a;
    provider.try_reentry = true;
    provider.fail_at = provider.calls + 2;
    rejected(d.compute({160, 200}, provider.epoch), ErrorCode::measurement_failed);
    require(provider.reentry_rejected && d.view().publication == initial.publication &&
                d.view().boxes.data() == initial.boxes.data() &&
                std::equal(held.boxes.begin(), held.boxes.end(), d.view().boxes.begin()),
            "measurement failure published partial final-tree geometry");
    provider.fail_at = 0;
    const auto recovered = accepted(d.compute({160, 200}, provider.epoch));
    LayoutDocument fresh(TextProvider::measure, &provider, {4, 2});
    for (auto& node : tree)
        node.node = {};
    accepted(fresh.set_tree(tree));
    same_geometry(recovered, accepted(fresh.compute({160, 200}, provider.epoch)));
    require(recovered.publication == initial.publication + 1 && box(recovered, b).parent == d.root() &&
                box(recovered, a).parent == parent,
            "retry lost final topology or published twice");
}

void final_tree_linear_siblings() {
    constexpr std::size_t count = 2048;
    LayoutDocument d(nullptr, nullptr, {count + 2, 1});
    std::vector<TreeNodeInput> tree(count);
    for (auto& node : tree)
        node.input = fixed_box(10, 1);
    const auto ids = accepted(d.set_tree(tree));
    for (std::size_t i = 0; i < count; ++i)
        tree[i].node = ids[i];
    accepted(d.compute({100, 3000}, 1));
    auto before = d.stats();
    tree.insert(tree.begin(), TreeNodeInput{{}, {}, fixed_box(10, 2)});
    const auto inserted = accepted(d.set_tree(tree));
    tree.front().node = inserted.front();
    auto after = d.stats();
    require(after.topology_commits == before.topology_commits + 1 &&
                after.topology_links_written - before.topology_links_written == tree.size(),
            "single sibling insertion rewrote more than the final child sequence");
    require(std::equal(ids.begin(), ids.end(), inserted.begin() + 1), "inserting a sibling replaced retained IDs");
    auto view = accepted(d.compute({100, 3000}, 1));
    require(close(box(view, ids.front()).y, 2) && close(box(view, ids.back()).y, static_cast<float>(count + 1)),
            "large sibling insertion produced the wrong order");
    before = after;
    std::reverse(tree.begin(), tree.end());
    accepted(d.set_tree(tree));
    after = d.stats();
    require(after.topology_commits == before.topology_commits + 1 &&
                after.topology_links_written - before.topology_links_written == tree.size(),
            "sibling reversal repeatedly rewrote the child sequence");
    view = accepted(d.compute({100, 3000}, 1));
    require(close(box(view, ids.back()).y, 0) && close(box(view, inserted.front()).y, static_cast<float>(count)),
            "large sibling reversal produced the wrong order");
    std::printf("{\"kind\":\"final_tree\",\"siblings\":%zu,\"reversal_links_written\":%llu}\n", tree.size(),
                static_cast<unsigned long long>(after.topology_links_written - before.topology_links_written));
}
