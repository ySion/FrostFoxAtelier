#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "ui_layout/layout.hpp"
#include <cstdio>

namespace {
using namespace atelier::ui_layout;

using atelier::acceptance::helpers::require;

template <class T>
T accepted(Result<T> result) {
    if (!result)
        atelier::fatal("Layout operation failed.");
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

void layout_sequence() {
    LayoutSnapshot retained;
    {
        LayoutDocument document(nullptr, nullptr, {3, 1});
        accepted(document.set_root({Kind::row}));
        NodeInput first = NodeInput::box(0, 1);
        first.width = Axis::expand(1, 80);
        NodeInput second = NodeInput::box(0, 1);
        second.width = Axis::expand(1, 0, 10);
        const auto a = accepted(document.create(document.root(), first));
        const auto b = accepted(document.create(document.root(), second));
        const auto view = accepted(document.compute({100, 10}, 1));
        // The capped child takes 10; the other receives the remaining 90.
        require(view.find(a) && view.find(b) && view.find(a)->width == 90 && view.find(b)->width == 10,
                "Bounded row allocation disagrees with the independent partition.");
        retained = document.snapshot();
    }
    require(retained.boxes.size() == 3 && retained.boxes[1].width == 90 && retained.boxes[2].width == 10,
            "Owned layout geometry did not survive document destruction.");
}
}

int main() {
    atelier::install_fatal_handler();
    layout_sequence();
    std::puts("UI layout acceptance: PASS (bounded allocation and owned geometry)");
}
