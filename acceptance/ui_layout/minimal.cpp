#include "ui_layout/layout.hpp"
#include <cstdio>
#include <exception>

int main() {
    using namespace atelier::ui_layout;
    try {
        LayoutDocument document(nullptr, nullptr, {3, 1});
        if (!document.set_root({Kind::row, {10, 0, 10, 0}, 5})) return 1;
        NodeInput input; input.height = Axis::fixed(10);
        const auto first = document.create(document.root(), input);
        input.width = Axis::expand(2);
        const auto second = document.create(document.root(), input);
        const auto result = document.compute({205, 20}, 1);
        if (!first || !second || !result || result->boxes.size() != 3) return 2;
        const auto* a = result->find(*first); const auto* b = result->find(*second);
        if (!a || !b || a->width != 60 || b->width != 120 || a->x != 10 || b->x != 75) return 3;
        const auto held = document.snapshot();
        if (!document.remove(*first) || document.input(*first) || !held.find(*first) || held.find(*first)->width != 60) return 4;
        std::puts("UI layout minimal consumer: PASS");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 5; }
}
