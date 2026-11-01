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
    accepted(d.set_tree(tree));
    require(d.view().publication == old.publication && d.view().boxes.data() == old.boxes.data() &&
                box(d.view(), b).parent == a,
            "tree edit changed accepted geometry before compute");
    const auto result = accepted(d.compute({200, 300}, 1));
    require(box(result, b).parent == d.root() && box(result, a).parent == b && box(result, x).parent == a &&
                box(result, y).parent == b,
            "final ancestor reversal or sibling order was not applied");
    // Remove B while its retained descendants escape, and turn A into a leaf
    // in the same transaction that moves A's child to the root.
    tree = {
        {a, {}, fixed_box(60, 5)}, {x, {}, fixed_box(30, 10)}, {y, {}, fixed_box(40, 20)}, {z, {}, fixed_box(50, 30)}};
    require(accepted(d.set_tree(tree)) == std::vector<LayoutNodeId>{a, x, y, z},
            "escaped descendants changed identity");
    const auto escaped = accepted(d.compute({200, 300}, 1));
    require(box(escaped, a).parent == d.root() && box(escaped, x).parent == d.root() && close(box(escaped, x).y, 5) &&
                close(box(escaped, y).y, 15),
            "retiring ancestor removed retained descendants or leaf conversion failed");
    accepted(d.set_tree({}));
    accepted(d.compute({200, 300}, 1));
    require(held.find(a) && held.find(b) && held.find(x)->parent == b,
            "owned old geometry lost removed nodes or original topology");
}
