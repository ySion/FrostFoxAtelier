#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "ui/session.hpp"
#include "ui/text.hpp"
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace atelier;

namespace {
using acceptance::helpers::require;

template <class T>
T accepted(ui::Result<T> result) {
    if (!result) {
        char message[128];
        std::snprintf(message, sizeof(message), "UI error %d native %u", static_cast<int>(result.error().code),
                      static_cast<unsigned>(result.error().native_code));
        fatal(message);
    }
    return std::move(*result);
}

void accepted(ui::Result<void> result) {
    require(result.has_value(), "UI operation failed");
}

const ui::DrawQuad& first_text(const ui::UiFrame& frame) {
    for (const auto& draw : frame.drawing->draws)
        if (draw.coverage)
            return draw;
    fatal("frame has no text");
}

ui::Control leaf(std::uint64_t key, ui::ControlKind kind, float y, std::wstring text = {}) {
    ui::Control value;
    value.key = key;
    value.kind = kind;
    value.text = std::move(text);
    value.layout = ui_layout::NodeInput::box(180, 30);
    value.layout.offset_x = 10;
    value.layout.offset_y = y;
    if (kind == ui::ControlKind::label)
        value.layout.padding = {6, 4, 6, 4};
    return value;
}

struct Consumer {
    ui::Session ui;

    std::vector<ui::Control> controls;

    std::shared_ptr<const ui::UiFrame> frame;

    Consumer() {
        accepted(ui.initialize());
        accepted(ui.set_surface({{640, 480}, 1, 1}));
    }

    void prepare() {
        accepted(ui.set_controls(controls));
        frame = accepted(ui.prepare());
        require(ui.accept(frame), "candidate not accepted");
    }
};

void identity_and_lifetime() {
    std::shared_ptr<const ui::UiFrame> held;
    std::weak_ptr<const ui::Coverage> released;
    {
        Consumer consumer;
        consumer.controls = {leaf(1, ui::ControlKind::button, 10, L"Original")};
        consumer.prepare();
        held = consumer.frame;
        released = first_text(*held).coverage;
        consumer.controls.clear();
        accepted(consumer.ui.set_controls(consumer.controls));
        consumer.controls = {leaf(1, ui::ControlKind::button, 10, L"Reused key")};
        accepted(consumer.ui.set_controls(consumer.controls));
        consumer.prepare();
    }
    require(held && held->drawing && !held->drawing->draws.empty(), "UI frame failed to outlive Session");
    for (const auto& draw : held->drawing->draws)
        if (draw.coverage)
            require(!draw.coverage->pixels.empty(), "frame lost text pixels after teardown");
    held.reset();
    require(released.expired(), "accounting retained pixels after the last frame was released");
}

}

int main() {
    atelier::install_fatal_handler();
    identity_and_lifetime();
    std::puts("UI acceptance: PASS (accepted frame outlives Session)");
}
