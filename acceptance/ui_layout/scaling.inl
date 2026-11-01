void virtual_range_and_anchor() {
    VirtualList list(110, 2);
    accepted(list.reset(100, 20));
    accepted(list.set_viewport(225, 100, 22));
    const auto range = list.range();
    require(range.begin == 9 && range.end == 16 && range.local_origin == -27 && range.extent == 2198,
            "visible half-open range or estimated extent");
    accepted(list.set_height(0, 40));
    require(list.range().scroll == 245 && list.range().begin == 9, "height change above view moved the anchor");
}
