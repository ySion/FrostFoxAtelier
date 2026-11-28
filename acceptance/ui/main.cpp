#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "ui/code_document.hpp"
#include "ui/controls.hpp"
#include "ui/session.hpp"
#include "ui/text.hpp"
#include "ui_language/component.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace atelier;

void verify_ui_error_recovery();

void verify_ui_entry_dependencies();

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

    Array<ui::Control> controls;

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
        const auto earlier = accepted(consumer.ui.prepare());
        const auto later = accepted(consumer.ui.prepare());
        require(later->drawing->id > earlier->drawing->id && consumer.ui.accept(earlier),
                "A prepared candidate must remain usable until a newer frame is accepted.");
        consumer.controls.front().text = L"Same key, new text";
        consumer.prepare();
        require(first_text(*consumer.frame).coverage != first_text(*held).coverage && !consumer.ui.accept(later),
                "Changed text retained an obsolete paint range or candidate.");
        consumer.controls.clear();
        accepted(consumer.ui.set_controls(consumer.controls));
        consumer.controls = {leaf(1, ui::ControlKind::button, 10, L"Reused key")};
        accepted(consumer.ui.set_controls(consumer.controls));
        consumer.prepare();
        require(first_text(*consumer.frame).coverage != first_text(*held).coverage,
                "A recreated key reused paint from its previous incarnation.");
    }
    require(held && held->drawing && !held->drawing->draws.empty(), "UI frame failed to outlive Session");
    for (const auto& draw : held->drawing->draws)
        if (draw.coverage)
            require(!draw.coverage->pixels.empty(), "frame lost text pixels after teardown");
    held.reset();
    require(released.expired(), "accounting retained pixels after the last frame was released");
}

void bounded_document_history() {
    auto document = accepted(ui::CodeDocument::create(String::literal<"seed">()));
    auto& edit = document.edit();
    for (unsigned revision = 1; revision <= 800; ++revision) {
        edit.select_all();
        const String rev_str = String::from_number(revision);
        accepted(edit.replace(rev_str.view()));
    }
    for (unsigned step = 0; step < 512; ++step)
        accepted(edit.history(false));
    require(!edit.can_undo() && document.text() == "288", "Document undo crossed its retention boundary.");
    accepted(edit.history(true));
    require(document.text() == "289", "Redo selected the wrong retained edit.");
    edit.select_all();
    accepted(edit.replace("branch"));
    require(!edit.can_redo(), "New document edit retained an abandoned redo branch.");
    accepted(edit.history(false));
    require(document.text() == "289", "Undo on the new branch restored the wrong document.");
}

void repeat_updates() {
    using namespace ui_language;
    using acceptance::helpers::take;
    Runtime runtime;
    take(register_types(runtime));
    const auto list = take(Component::compile(runtime, "repeat.ui.source", R"(
ui Detail() { Text content { text: "Detail"; height: 18; } }
ui Entry(in ActionBinding command) {
    state bool open = false;
    Column row {
        Checkbox fold { text: "Group"; width: 150; height: 24; selected: state.open; on activate { state.open = event.checked; } }
        Button run { text: "Run"; width: 120; height: 24; action: input.command; }
        Detail detail { mounted: state.open; }
    }
}
ui Group(in Items<Entry> entries) { Column view { Repeat rows { items: input.entries; } } }
ui ListView(in Items<Group> groups) { Repeat groups { items: input.groups; } }
)",
                                              "ListView"));
    const auto row = [](ui::ControlKey key, ui::ControlKey fold, ui::ControlKey target) {
        return Item{key, {ActionBinding{target}}, {{"fold", fold}, {"run", key + 2}}};
    };
    const auto inputs = [](Array<Item> rows) {
        auto entries = ItemList{std::make_shared<const Array<Item>>(std::move(rows))};
        Array<Item> groups{{50, {std::move(entries)}}};
        return std::array<InputValue, 1>{ItemList{std::make_shared<const Array<Item>>(std::move(groups))}};
    };
    auto first = row(100, 101, 501);
    auto second = row(200, 201, 502);
    ui::ControlKey next = 1000;
    const auto initial_inputs = inputs({first, second});
    auto root = take(Instance::create(list, 0, next, {}, initial_inputs));
    const auto update_rows = [&](Array<Item> rows) {
        const auto next_inputs = inputs(std::move(rows));
        return root->update(next_inputs);
    };
    Consumer consumer;
    const auto accept_frame = [&] {
        consumer.frame = accepted(consumer.ui.prepare());
        require(consumer.ui.accept(consumer.frame), "Repeat frame was refused.");
    };
    const auto present = [&] {
        accepted(consumer.ui.set_controls(root->controls()));
        root->commit_controls();
        accept_frame();
    };
    const auto fold = [&](ui::ControlKey key, bool checked) {
        return take(root->event({ui::ActionKind::activate, key, checked}));
    };
    const auto selected = [&](ui::ControlKey key) {
        const auto at = std::ranges::find(root->controls(), key, &ui::Control::key);
        require(at != root->controls().end(), "Repeat fold key is missing.");
        return at->selected;
    };
    const auto has_detail = [&](ui::ControlKey row) {
        return std::ranges::any_of(root->controls(), [row](const auto& control) {
            return control.parent == row && control.text == L"Detail";
        });
    };
    present();
    const auto initial = root->controls().size();
    require(fold(201, true) && fold(101, true) && fold(201, true), "Repeat did not handle both rows.");
    take(root->update());
    require(root->controls().size() == initial + 2 && selected(101) && selected(201),
            "Batched row events patched the wrong control ranges.");
    present();
    require(fold(101, false), "First fold event was lost.");
    take(root->update());
    require(root->controls().size() == initial + 1, "Fold did not remove its child.");
    root->reject_controls();
    require(root->controls().size() == initial + 2 && selected(101), "Refusal lost the accepted child or fold.");
    take(root->update());
    present();
    require(fold(201, false), "Second fold event was lost.");
    take(root->update());
    present();
    const auto point = [](const ui::UiFrame& frame, ui::EventKind kind) {
        const auto* run = frame.find(102);
        require(run && run->bounds.width > 0 && run->bounds.height > 0, "Missing command geometry.");
        return ui::Event{kind, run->bounds.x + run->bounds.width / 2, run->bounds.y + run->bounds.height / 2};
    };
    const auto command = [](const ui::Reply& reply) -> ui::ControlKey {
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect);
                action && action->kind == ui::ActionKind::activate)
                return action->target;
        return 0;
    };
    const auto old_frame = consumer.frame;
    require(consumer.ui.input(point(*old_frame, ui::EventKind::pointer_down)).capture == ui::Capture::acquire,
            "Command press was not captured.");
    first = row(100, 109, 701);
    take(update_rows({second, first}));
    const auto controls = root->controls();
    require(std::ranges::find(controls, 200, &ui::Control::key) < std::ranges::find(controls, 100, &ui::Control::key) &&
                selected(109) && !selected(201),
            "Reorder moved fold state away from its item key.");
    accepted(consumer.ui.set_controls(controls));
    require(!consumer.ui.interaction_state().pointer, "Rebinding kept the old command pressed.");
    root->commit_controls();
    require(!command(consumer.ui.input(point(*old_frame, ui::EventKind::pointer_up))),
            "Old press executed after rebind.");
    accept_frame();
    const auto* rebound = consumer.frame->find(102);
    require(rebound && rebound->action_target == 701 && old_frame->find(102)->action_target == 501,
            "Command rebind changed the accepted old frame.");
    consumer.ui.input(point(*consumer.frame, ui::EventKind::pointer_down));
    require(command(consumer.ui.input(point(*consumer.frame, ui::EventKind::pointer_up))) == 701,
            "Fresh click did not use the rebound target.");
    require(!fold(101, false) && fold(109, false) && fold(201, true), "Rebound fold aliases routed to the wrong row.");
    auto invalid = first;
    invalid.inputs = {ActionBinding{0}};
    require(!update_rows({second, invalid}), "Invalid row command was accepted.");
    take(root->update());
    present();
    require(selected(109) && !selected(201) && has_detail(100) && !has_detail(200) &&
                root->controls().size() == initial + 1,
            "Failed list update did not restore both rows and their mounted children.");
    require(fold(109, false) && fold(201, true), "Recovered rows no longer handled events.");
    take(root->update());
    present();
    require(!selected(109) && selected(201) && !has_detail(100) && has_detail(200) &&
                root->controls().size() == initial + 1,
            "Local patches used positions from before reorder or rollback.");
    require(fold(201, false) && fold(201, true), "Removed row did not receive its last events.");
    take(update_rows({first}));
    present();
    require(!fold(201, true), "Retired item still received events.");
    take(update_rows({first, second}));
    present();
    require(!selected(201) && root->controls().size() == initial, "Reinserted item retained retired state.");
    const auto last_frame = consumer.frame;
    next = std::numeric_limits<ui::ControlKey>::max() - 1;
    const auto refused = update_rows({first, second, row(300, 301, 503), row(400, 401, 504)});
    require(!refused && refused.error().code == ErrorCode::capacity_exceeded &&
                refused.error().message == "UI control keys exhausted." &&
                next == std::numeric_limits<ui::ControlKey>::max() && root->controls().size() == initial &&
                !last_frame->find(301),
            "A Repeat update wrapped an exhausted identity or retained a partial row.");
    take(root->update());
    present();
    require(consumer.frame->find(201) && !consumer.frame->find(301),
            "The accepted rows did not recover after key exhaustion.");
}

void integral_slider_and_number_quantization() {
    Consumer consumer;
    accepted(consumer.ui.set_surface({{640, 480}, 1.5f, 2}));
    const auto build_row = [&](double current) {
        Array<ui::Control> controls;
        ui::Control root;
        root.key = 10;
        root.kind = ui::ControlKind::container;
        root.layout = ui_layout::NodeInput::column();
        root.layout.width = ui_layout::Axis::fixed(360);
        root.layout.height = ui_layout::Axis::content(40);
        controls.add(std::move(root));

        ui::ParameterRow row{11, 12, 13, 14, 10, L"Stories"};
        ui::NumberField field;
        field.key = 15;
        field.value = current;
        field.default_value = 3;
        field.minimum = 1;
        field.maximum = 100;
        field.drag_step = 1;
        field.fine_step = 1;
        field.integral = true;
        field.slider = ui::SliderRange{16, 1, 6};
        accepted(ui::append_number_row(controls, row, Span{&field, 1}));
        consumer.controls = std::move(controls);
        consumer.prepare();
    };

    build_row(3);
    const auto* slider = consumer.frame->find(16);
    require(slider && slider->bounds.width > 40, "Missing integral slider geometry.");
    const float mid_y = slider->bounds.y + slider->bounds.height * .5f;

    // 1. Click-jump on slider quantizes to whole integer, and slow sub-integer drag steps accumulate without deadzone.
    Array<double> previews;
    double committed = -1;
    const auto collect_edits = [&](const ui::Reply& reply) {
        for (const auto& effect : reply.effects) {
            if (const auto* act = std::get_if<ui::Action>(&effect)) {
                if (act->kind == ui::ActionKind::edit_preview) {
                    const double v = std::get<double>(act->value);
                    require(std::trunc(v) == v, "Integral control emitted a fractional preview.");
                    previews.add(v);
                } else if (act->kind == ui::ActionKind::edit_commit) {
                    committed = std::get<double>(act->value);
                    require(std::trunc(committed) == committed, "Integral control committed a fractional value.");
                }
            }
        }
    };

    const float jump_x = slider->bounds.x + slider->bounds.width * .12f;
    collect_edits(consumer.ui.input({ui::EventKind::pointer_down, jump_x, mid_y}));
    require(!previews.empty() && previews.back() >= 1 && previews.back() <= 2,
            "Integral slider click-jump did not quantize near minimum.");
    const double jumped = previews.back();
    previews.clear();

    // Drag slowly across the slider in 2px increments (sub-integer steps) all the way toward 85% of the track.
    const float target_x = slider->bounds.x + slider->bounds.width * .88f;
    for (float x = jump_x + 2.f; x <= target_x; x += 2.f)
        collect_edits(consumer.ui.input({ui::EventKind::pointer_move, x, mid_y}));
    collect_edits(consumer.ui.input({ui::EventKind::pointer_up, target_x, mid_y}));
    require(!previews.empty() && committed >= 5 && committed > jumped,
            "Slow sub-integer slider drag stalled in a rounding deadzone.");
    for (std::size_t i = 1; i < previews.size(); ++i)
        require(previews[i] != previews[i - 1], "Integral slider emitted duplicate unchanged integer previews.");

    // 2. Number field horizontal drag at 1.5x DPI (14px active drag / 1.5 = +9.333 -> +9) emits exact whole integers.
    const double before_drag = committed;
    build_row(before_drag);
    const auto* number = consumer.frame->find(15);
    require(number && number->bounds.width > 20, "Missing integral number box geometry.");
    const float num_x = number->bounds.x + number->bounds.width * .5f;
    const float num_y = number->bounds.y + number->bounds.height * .5f;
    previews.clear();
    committed = -1;
    collect_edits(consumer.ui.input({ui::EventKind::pointer_down, num_x, num_y}));
    for (int step = 1; step <= 8; ++step)
        collect_edits(consumer.ui.input({ui::EventKind::pointer_move, num_x + float(step * 2), num_y}));
    collect_edits(consumer.ui.input({ui::EventKind::pointer_up, num_x + 16.f, num_y}));
    require(!previews.empty() && committed == before_drag + 9,
            "1.5x DPI horizontal drag on integral number box did not quantize to exact +9 delta.");

    // 3. Middle-mouse value ladder with Shift (fine 0.1 tick) accumulates whole integers cleanly.
    build_row(committed);
    previews.clear();
    const double before_ladder = committed;
    committed = -1;
    ui::Event ladder_down{.kind = ui::EventKind::pointer_down,
                          .x = num_x,
                          .y = num_y,
                          .button = ui::PointerButton::middle,
                          .shift = true};
    collect_edits(consumer.ui.input(ladder_down));
    for (int step = 1; step <= 12; ++step) {
        ui::Event move{.kind = ui::EventKind::pointer_move,
                       .x = num_x + float(step * 8),
                       .y = num_y,
                       .button = ui::PointerButton::middle,
                       .shift = true};
        collect_edits(consumer.ui.input(move));
    }
    ui::Event ladder_up{.kind = ui::EventKind::pointer_up,
                        .x = num_x + 96.f,
                        .y = num_y,
                        .button = ui::PointerButton::middle,
                        .shift = true};
    collect_edits(consumer.ui.input(ladder_up));
    require(committed > before_ladder,
            "Fine middle-mouse ladder on integral number box did not accumulate whole steps.");

    // 4. Fractional text input on integral number box refuses commit, while integer text commits cleanly.
    build_row(committed);
    consumer.ui.input({ui::EventKind::pointer_down, num_x, num_y});
    consumer.ui.input({ui::EventKind::pointer_up, num_x, num_y});
    consumer.prepare();
    ui::Event bad_text{ui::EventKind::text};
    bad_text.data = String::literal<"3.5">();
    previews.clear();
    committed = -1;
    collect_edits(consumer.ui.input(bad_text));
    ui::Event enter{ui::EventKind::key_down};
    enter.key = ui::Key::enter;
    collect_edits(consumer.ui.input(enter));
    require(previews.empty() && committed == -1, "Integral number box accepted fractional text '3.5'.");
    ui::Event escape{ui::EventKind::key_down};
    escape.key = ui::Key::escape;
    consumer.ui.input(escape);
}

void standardized_kit_and_style_contracts() {
    // 1. StyleSheet @tokens, color_plane selector, and transient state layout-mutation rejection.
    require(
        ui::StyleSheet::parse("@tokens { ink: #d4d4d4; } color_plane { border-width: 1dip; border-color: token(ink); }")
            .has_value(),
        "StyleSheet failed to parse @tokens and color_plane selector.");
    require(!ui::StyleSheet::parse("button:hover { padding: 4dip; }").has_value(),
            "StyleSheet allowed padding mutation on :hover.");
    require(!ui::StyleSheet::parse("button:focus { font-weight: 600; }").has_value(),
            "StyleSheet allowed font-weight mutation on transient :focus.");

    // 2. Read-only text_edit + placeholder: renders hint when empty, suppresses IME anchor, blocks mutation, allows Ctrl+C.
    Consumer consumer;
    ui::Control ro = leaf(50, ui::ControlKind::text_edit, 10, L"");
    ro.read_only = true;
    ro.placeholder = L"Filter hint...";
    consumer.controls = {ro};
    consumer.prepare();
    require(!first_text(*consumer.frame).coverage->pixels.empty(), "Empty read-only field did not render placeholder.");
    require(!consumer.ui.focus(50).error, "Failed to focus read-only text_edit.");
    consumer.prepare();
    require(!consumer.frame->input_anchor.has_value(), "Read-only text_edit exposed an active IME input_anchor.");

    ro.text = L"entity://copy_target";
    consumer.controls = {ro};
    consumer.prepare();
    require(!consumer.ui.focus(50).error, "Failed to focus populated read-only text_edit.");
    ui::Event typed{ui::EventKind::text};
    typed.data = String::literal<"X">();
    const auto mut_reply = consumer.ui.input(typed);
    require(std::ranges::none_of(mut_reply.effects,
                                 [](const ui::Effect& e) { return std::holds_alternative<ui::Action>(e); }),
            "Read-only text_edit emitted an edit action on text input.");
    ui::Event key{ui::EventKind::key_down};
    key.control = true;
    key.key = ui::Key::a;
    consumer.ui.input(key);
    key.key = ui::Key::c;
    const auto copy_reply = consumer.ui.input(key);
    require(std::ranges::any_of(copy_reply.effects,
                                [](const ui::Effect& e) {
                                    const auto* p = std::get_if<ui::PlatformRequest>(&e);
                                    const auto* cb = p ? std::get_if<ui::ClipboardRequest>(p) : nullptr;
                                    return cb && cb->kind == ui::ClipboardKind::copy &&
                                           cb->text == "entity://copy_target";
                                }),
            "Read-only text_edit did not copy selected value on Ctrl+C.");
    key.key = ui::Key::z;
    const auto ro_undo_reply = consumer.ui.input(key);
    require(!ro_undo_reply.consumed && !consumer.ui.wants_keyboard(),
            "Read-only text_edit swallowed Ctrl+Z or claimed wants_keyboard.");
    require(!consumer.ui.focus(50, 6).error, "Failed to select prefix on read-only text_edit.");
    static_cast<void>(consumer.ui.cancel_interaction());
    key.key = ui::Key::c;
    const auto partial_copy = consumer.ui.input(key);
    require(std::ranges::any_of(partial_copy.effects,
                                [](const ui::Effect& e) {
                                    const auto* p = std::get_if<ui::PlatformRequest>(&e);
                                    const auto* cb = p ? std::get_if<ui::ClipboardRequest>(p) : nullptr;
                                    return cb && cb->kind == ui::ClipboardKind::copy && cb->text == "entity";
                                }),
            "cancel_interaction() wiped passive read-only substring selection before copy.");

    // 3. Tree trailing observability gutter + native anchored inline rename + stem focus selection.
    ui::Control tree;
    tree.key = 200;
    tree.kind = ui::ControlKind::tree;
    tree.layout = ui_layout::NodeInput::column();
    tree.layout.width = ui_layout::Axis::fixed(260);
    tree.layout.height = ui_layout::Axis::fixed(120);
    tree.tree_row_count = 2;

    ui::Control root_item;
    root_item.key = 2001;
    root_item.parent = 200;
    root_item.kind = ui::ControlKind::tree_item;
    root_item.text = L"World";
    root_item.icon = ui::ControlIcon::world;
    root_item.tree_index = 0;
    root_item.tree_depth = 0;
    root_item.tree_branch = true;
    root_item.tree_expanded = true;

    ui::Control item;
    item.key = 201;
    item.parent = 200;
    item.kind = ui::ControlKind::tree_item;
    item.text = L"HeroMesh.gltf";
    item.icon = ui::ControlIcon::mesh;
    item.tree_index = 1;
    item.tree_depth = 1;
    item.tree_badges = {ui::ControlIcon::physics, ui::ControlIcon::procgraph};
    item.tree_trailing_icon = ui::ControlIcon::eye;

    ui::Control rename;
    rename.key = 202;
    rename.parent = 0;
    rename.kind = ui::ControlKind::text_edit;
    rename.text = L"HeroMesh.gltf";
    rename.transient_edit = true;
    rename.rename_anchor = 201;
    rename.layout = ui_layout::NodeInput::box(120, 22);
    rename.layout.out_of_flow = true;

    consumer.controls = {tree, root_item, item, rename};
    consumer.prepare();
    const auto* item_box = consumer.frame->find(201);
    const auto* ren_box = consumer.frame->find(202);
    require(item_box && ren_box && ren_box->bounds.x > item_box->bounds.x &&
                std::abs(ren_box->bounds.y - item_box->bounds.y) < 4.f,
            "Anchored inline rename was not positioned over its target TreeItem.");
    require(!consumer.ui.focus(202, 8).error, "Failed to focus anchored rename field with stem selection.");
    key.control = true;
    key.key = ui::Key::c;
    const auto stem_copy = consumer.ui.input(key);
    require(std::ranges::any_of(stem_copy.effects,
                                [](const ui::Effect& e) {
                                    const auto* p = std::get_if<ui::PlatformRequest>(&e);
                                    const auto* cb = p ? std::get_if<ui::ClipboardRequest>(p) : nullptr;
                                    return cb && cb->kind == ui::ClipboardKind::copy && cb->text == "HeroMesh";
                                }),
            "Session::focus(key, select_length) did not select the file stem.");
    ui::Event enter{ui::EventKind::key_down};
    enter.key = ui::Key::enter;
    const auto commit_reply = consumer.ui.input(enter);
    require(std::ranges::any_of(commit_reply.effects,
                                [](const ui::Effect& e) {
                                    const auto* a = std::get_if<ui::Action>(&e);
                                    return a && a->kind == ui::ActionKind::edit_commit && a->key == 202 &&
                                           std::get<std::wstring>(a->value) == L"HeroMesh.gltf";
                                }),
            "Unmodified transient_edit field did not emit edit_commit on Enter.");

    // Unmount rename overlay and verify TreeItem trailing eye icon emits activate without select.
    consumer.controls = {tree, root_item, item};
    consumer.prepare();
    item_box = consumer.frame->find(201);
    const float trail_x = item_box->bounds.x + item_box->bounds.width - 9.f;
    const float trail_y = item_box->bounds.y + item_box->bounds.height * .5f;
    consumer.ui.input({ui::EventKind::pointer_down, trail_x, trail_y});
    const auto trail_up = consumer.ui.input({ui::EventKind::pointer_up, trail_x, trail_y});
    bool got_activate = false;
    bool got_select = false;
    for (const auto& e : trail_up.effects) {
        if (const auto* a = std::get_if<ui::Action>(&e)) {
            if (a->key == 201 && a->kind == ui::ActionKind::activate)
                got_activate = true;
            if (a->key == 201 && a->kind == ui::ActionKind::select)
                got_select = true;
        }
    }
    require(got_activate && !got_select, "TreeItem trailing icon click did not isolate activate from row select.");

    // 4. 2D ColorPlane pointer drag emits edit_begin / edit_preview / edit_commit with normalized DragPoint.
    ui::Control plane = leaf(300, ui::ControlKind::color_plane, 10);
    plane.layout = ui_layout::NodeInput::box(120, 100);
    plane.color_hue = 200.0;
    plane.value = .3;
    plane.color_value = .6;
    consumer.controls = {plane};
    consumer.prepare();
    require(
        std::ranges::any_of(consumer.frame->drawing->draws, [](const ui::DrawQuad& q) { return q.image != nullptr; }),
        "ColorPlane did not emit cached gradient ColorImage quad.");
    const auto* plane_box = consumer.frame->find(300);
    const auto p_down =
        consumer.ui.input({ui::EventKind::pointer_down, plane_box->bounds.x + 24.f, plane_box->bounds.y + 30.f});
    const auto p_move =
        consumer.ui.input({ui::EventKind::pointer_move, plane_box->bounds.x + 90.f, plane_box->bounds.y + 25.f});
    const auto p_up =
        consumer.ui.input({ui::EventKind::pointer_up, plane_box->bounds.x + 90.f, plane_box->bounds.y + 25.f});
    require(std::ranges::any_of(p_down.effects,
                                [](const ui::Effect& e) {
                                    const auto* a = std::get_if<ui::Action>(&e);
                                    return a && a->key == 300 && a->kind == ui::ActionKind::edit_begin;
                                }) &&
                std::ranges::any_of(p_move.effects,
                                    [](const ui::Effect& e) {
                                        const auto* a = std::get_if<ui::Action>(&e);
                                        if (!a || a->key != 300 || a->kind != ui::ActionKind::edit_preview)
                                            return false;
                                        const auto pt = std::get<ui::DragPoint>(a->value);
                                        return pt.x > .6f && pt.x <= 1.f && pt.y > .6f && pt.y <= 1.f;
                                    }) &&
                std::ranges::any_of(p_up.effects,
                                    [](const ui::Effect& e) {
                                        const auto* a = std::get_if<ui::Action>(&e);
                                        return a && a->key == 300 && a->kind == ui::ActionKind::edit_commit;
                                    }),
            "ColorPlane drag gesture did not emit edit_begin / edit_preview / edit_commit.");

    // 5. Table Ctrl+C copy and double-click column border auto-fit.
    auto table_data = std::make_shared<ui::TableData>();
    table_data->columns = {{1, L"Name", 64.f, 40.f, ui::TextAlign::left, true},
                           {2, L"Type", 72.f, 40.f, ui::TextAlign::left, true}};
    table_data->rows = {{10, {{101, L"VeryLongArchitecturalPillarAssetName", ui::ControlIcon::mesh}, {102, L"Mesh"}}}};
    ui::Control tbl;
    tbl.key = 400;
    tbl.kind = ui::ControlKind::table;
    tbl.layout = ui_layout::NodeInput::box(280, 120);
    tbl.table = table_data;
    tbl.table_selection = {10, 1};
    consumer.controls = {tbl};
    consumer.prepare();
    require(!consumer.ui.focus(400).error, "Failed to focus Table.");
    key.control = true;
    key.key = ui::Key::c;
    const auto tbl_copy = consumer.ui.input(key);
    require(std::ranges::any_of(tbl_copy.effects,
                                [](const ui::Effect& e) {
                                    const auto* p = std::get_if<ui::PlatformRequest>(&e);
                                    const auto* cb = p ? std::get_if<ui::ClipboardRequest>(p) : nullptr;
                                    return cb && cb->kind == ui::ClipboardKind::copy &&
                                           cb->text.contains("VeryLongArchitecturalPillarAssetName");
                                }),
            "Table Ctrl+C did not copy the selected row/cell text.");
    const auto* tbl_info = consumer.frame->find_table(400);
    require(tbl_info && !tbl_info->columns.empty(), "Missing accepted Table column geometry.");
    const float border_x = tbl_info->columns.front().resize.x + tbl_info->columns.front().resize.width * .5f;
    const float border_y = tbl_info->columns.front().resize.y + tbl_info->columns.front().resize.height * .5f;
    ui::Event dbl{ui::EventKind::pointer_down, border_x, border_y};
    dbl.clicks = 2;
    const auto fit_reply = consumer.ui.input(dbl);
    require(std::ranges::any_of(fit_reply.effects,
                                [](const ui::Effect& e) {
                                    const auto* a = std::get_if<ui::Action>(&e);
                                    if (!a || a->key != 400 || a->kind != ui::ActionKind::table_column_resize)
                                        return false;
                                    const auto r = std::get<ui::TableColumnResize>(a->value);
                                    return r.column == 1 && r.width > 96.f;
                                }),
            "Double-clicking Table column divider did not auto-fit column width to visible text.");

    // 6. Table rename_cell anchoring in both details and tiles views + wheel auto-commit.
    ui::Control cell_rename;
    cell_rename.key = 401;
    cell_rename.parent = 0;
    cell_rename.kind = ui::ControlKind::text_edit;
    cell_rename.text = L"RenamedPillar";
    cell_rename.transient_edit = true;
    cell_rename.rename_anchor = 400;
    cell_rename.rename_cell = 101;
    cell_rename.layout = ui_layout::NodeInput::box(80, 20);
    cell_rename.layout.out_of_flow = true;
    consumer.controls = {tbl, cell_rename};
    consumer.prepare();
    const auto* details_cell_box = consumer.frame->find(401);
    const auto* tbl_box = consumer.frame->find(400);
    require(details_cell_box && tbl_box && details_cell_box->bounds.x >= tbl_box->bounds.x &&
                details_cell_box->bounds.y > tbl_box->bounds.y,
            "Table rename_cell overlay was not anchored in details view.");
    tbl.table_layout.view = ui::TableView::tiles;
    consumer.controls = {tbl, cell_rename};
    consumer.prepare();
    tbl_box = consumer.frame->find(400);
    const auto* tiles_cell_box = consumer.frame->find(401);
    const float dpi = consumer.frame->drawing->dpi_scale;
    require(tbl_box && tiles_cell_box && tiles_cell_box->bounds.height <= 24.f * dpi &&
                tiles_cell_box->bounds.y > tbl_box->bounds.y + 16.f * dpi,
            "Table rename_cell overlay was not anchored to bottom label band in tiles view.");
    require(!consumer.ui.focus(401).error, "Failed to focus Table cell rename overlay.");
    ui::Event wheel{ui::EventKind::wheel, tiles_cell_box->bounds.x + 8.f, tiles_cell_box->bounds.y + 4.f};
    wheel.wheel = -1.f;
    const auto wheel_reply = consumer.ui.input(wheel);
    require(std::ranges::any_of(wheel_reply.effects,
                                [](const ui::Effect& e) {
                                    const auto* a = std::get_if<ui::Action>(&e);
                                    return a && a->key == 401 && a->kind == ui::ActionKind::edit_commit;
                                }),
            "Scrolling a Table with an active anchored rename field did not commit the edit.");

    // 7. Live-refreshing Table preserves column-resize drag and row press across TableData snapshots.
    tbl.table_layout = {};
    consumer.controls = {tbl};
    consumer.prepare();
    tbl_info = consumer.frame->find_table(400);
    require(tbl_info && !tbl_info->columns.empty(), "Missing details Table column geometry.");
    const float drag_start_x = tbl_info->columns.front().resize.x + tbl_info->columns.front().resize.width * .5f;
    const float drag_start_y = tbl_info->columns.front().resize.y + tbl_info->columns.front().resize.height * .5f;
    const auto drag_down = consumer.ui.input({ui::EventKind::pointer_down, drag_start_x, drag_start_y});
    require(drag_down.capture == ui::Capture::acquire, "Table column resize did not acquire pointer capture.");
    consumer.ui.input({ui::EventKind::pointer_move, drag_start_x + 48.f * dpi, drag_start_y});
    auto refreshed_table = std::make_shared<ui::TableData>(*table_data);
    refreshed_table->rows[0].cells[1].text = L"Mesh (Updated 16.7ms)";
    tbl.table = refreshed_table;
    consumer.controls = {tbl};
    consumer.prepare();
    consumer.ui.input({ui::EventKind::pointer_move, drag_start_x + 60.f * dpi, drag_start_y});
    const auto drag_up = consumer.ui.input({ui::EventKind::pointer_up, drag_start_x + 60.f * dpi, drag_start_y});
    require(drag_up.capture == ui::Capture::release &&
                std::ranges::any_of(drag_up.effects,
                                    [](const ui::Effect& e) {
                                        const auto* a = std::get_if<ui::Action>(&e);
                                        if (!a || a->key != 400 || a->kind != ui::ActionKind::table_column_resize)
                                            return false;
                                        const auto r = std::get<ui::TableColumnResize>(a->value);
                                        return r.column == 1 && r.width > 110.f;
                                    }),
            "Live TableData snapshot replacement cancelled an active column-resize drag.");

    tbl_info = consumer.frame->find_table(400);
    require(tbl_info && !tbl_info->cells.empty(), "Missing Table cell geometry for live click check.");
    const float row_x = tbl_info->cells.front().bounds.x + 12.f;
    const float row_y = tbl_info->cells.front().bounds.y + tbl_info->cells.front().bounds.height * .5f;
    consumer.ui.input({ui::EventKind::pointer_down, row_x, row_y});
    auto refreshed_table_2 = std::make_shared<ui::TableData>(*refreshed_table);
    refreshed_table_2->rows[0].cells[1].text = L"Mesh (Updated 8.3ms)";
    tbl.table = refreshed_table_2;
    consumer.controls = {tbl};
    consumer.prepare();
    const auto row_up = consumer.ui.input({ui::EventKind::pointer_up, row_x, row_y});
    require(std::ranges::any_of(row_up.effects,
                                [](const ui::Effect& e) {
                                    const auto* a = std::get_if<ui::Action>(&e);
                                    return a && a->key == 400 && a->kind == ui::ActionKind::select;
                                }),
            "Live TableData snapshot replacement wiped out an in-flight row click.");

    // 8. Secondary click on an unfocused control commits prior edit, moves focus, and requests context menu in one step.
    ui::Control pane_root;
    pane_root.key = 499;
    pane_root.kind = ui::ControlKind::container;
    pane_root.layout = ui_layout::NodeInput::column();
    ui::Control field_a;
    field_a.key = 501;
    field_a.parent = 499;
    field_a.kind = ui::ControlKind::text_edit;
    field_a.text = L"OriginalDraft";
    field_a.layout = ui_layout::NodeInput::box(160, 24);
    ui::Control tree_box;
    tree_box.key = 500;
    tree_box.parent = 499;
    tree_box.kind = ui::ControlKind::tree;
    tree_box.tree_row_count = 1;
    tree_box.layout = ui_layout::NodeInput::box(160, 48);
    ui::Control item_b;
    item_b.key = 502;
    item_b.parent = 500;
    item_b.kind = ui::ControlKind::tree_item;
    item_b.text = L"TargetRow";
    item_b.context_target = 502;
    consumer.controls = {pane_root, field_a, tree_box, item_b};
    consumer.prepare();
    const auto* fa_box = consumer.frame->find(501);
    const auto* ib_box = consumer.frame->find(502);
    require(fa_box && ib_box, "Missing controls for cross-control right-click focus test.");
    consumer.ui.input({ui::EventKind::pointer_down, fa_box->bounds.x + 6.f, fa_box->bounds.y + 6.f});
    consumer.ui.input({ui::EventKind::pointer_up, fa_box->bounds.x + 6.f, fa_box->bounds.y + 6.f});
    ui::Event text_in{ui::EventKind::text};
    text_in.data = String::literal<"!">();
    consumer.ui.input(text_in);
    ui::Event rclick{ui::EventKind::pointer_down, ib_box->bounds.x + 24.f, ib_box->bounds.y + 8.f};
    rclick.button = ui::PointerButton::secondary;
    const auto rclick_reply = consumer.ui.input(rclick);
    require(consumer.ui.focused_control() == 502 &&
                std::ranges::any_of(rclick_reply.effects,
                                    [](const ui::Effect& e) {
                                        const auto* a = std::get_if<ui::Action>(&e);
                                        return a && a->key == 501 && a->kind == ui::ActionKind::edit_commit;
                                    }) &&
                std::ranges::any_of(rclick_reply.effects,
                                    [](const ui::Effect& e) {
                                        const auto* a = std::get_if<ui::Action>(&e);
                                        return a && a->key == 502 && a->kind == ui::ActionKind::context_menu;
                                    }),
            "Single secondary click did not commit active edit, focus target control, and emit context_menu.");

    require(ui::parse_control_icon("lock") == ui::ControlIcon::lock &&
                ui::parse_control_icon("unlock") == ui::ControlIcon::unlock,
            "Failed to parse lock/unlock ControlIcon names.");
    ui::Control lock_btn;
    lock_btn.key = 503;
    lock_btn.parent = 499;
    lock_btn.kind = ui::ControlKind::button;
    lock_btn.icon = ui::ControlIcon::lock;
    lock_btn.button_behavior = ui::ButtonBehavior::toggle;
    lock_btn.selected = true;
    lock_btn.layout = ui_layout::NodeInput::box(24, 24);
    consumer.controls = {pane_root, field_a, tree_box, item_b, lock_btn};
    consumer.prepare();
}

void verify_code_editor_syntax_and_gestures() {
    Consumer consumer;
    auto created = ui::CodeDocument::create(
        String::literal<"class project.Test : world.Component {\n    float speed = 1.0f;\n}">(),
        ui::CodeLanguage::source);
    require(created.has_value(), "Failed to create CodeDocument for syntax test.");
    auto doc = std::make_shared<ui::CodeDocument>(std::move(*created));
    require(doc->line_diff(0) == ui::CodeLineDiff::clean && doc->line_diff(1) == ui::CodeLineDiff::clean,
            "Freshly created CodeDocument reported dirty line diffs.");

    auto annotations = std::make_shared<ui::CodeAnnotations>();
    annotations->revision = doc->revision();
    annotations->semantic_types.add(String::literal<"Component">());
    annotations->semantic_types.add(String::literal<"Test">());
    annotations->semantic_fields.add(String::literal<"speed">());
    annotations->diagnostics.add(
        {1u, 10u, 15u, ui::CodeDiagnosticSeverity::error, String::literal<"Invalid speed — 中文与🚀长诊断预览">()});
    annotations->search_matches.add({6u, 18u});
    annotations->active_search_match = 0;

    ui::Control editor_ctrl;
    editor_ctrl.key = 700;
    editor_ctrl.kind = ui::ControlKind::code_editor;
    editor_ctrl.code = doc;
    editor_ctrl.code_annotations = annotations;
    editor_ctrl.layout = ui_layout::NodeInput::box(640, 220);
    consumer.controls = {editor_ctrl};
    consumer.prepare();

    // Verify zero-extra-texture horizontal clip slicing: line 0 emits multiple coverage quads sharing one Coverage
    // mask with distinct token colors.
    const auto* code_geom = consumer.frame->find_code(700);
    require(code_geom && !code_geom->lines.empty(), "CodeEditor did not publish CodeGeometry lines.");
    const float line0_y = code_geom->lines.front().y;
    std::vector<std::pair<const ui::Coverage*, std::size_t>> shared_masks;
    std::vector<std::uint32_t> distinct_colors;
    for (const auto& quad : consumer.frame->drawing->draws) {
        if (quad.coverage && std::abs(quad.bounds.y - line0_y) < code_geom->line_height * 0.6f &&
            quad.bounds.x >= code_geom->body.x - 2.f) {
            auto mask_it = std::ranges::find(shared_masks, quad.coverage.get(),
                                             &std::pair<const ui::Coverage*, std::size_t>::first);
            if (mask_it == shared_masks.end())
                shared_masks.emplace_back(quad.coverage.get(), 1u);
            else
                ++mask_it->second;
            const std::uint32_t packed = (static_cast<std::uint32_t>(quad.color.r * 255.f) << 16) |
                                         (static_cast<std::uint32_t>(quad.color.g * 255.f) << 8) |
                                         static_cast<std::uint32_t>(quad.color.b * 255.f);
            if (std::ranges::find(distinct_colors, packed) == distinct_colors.end())
                distinct_colors.push_back(packed);
        }
    }
    const bool has_sliced_line = std::ranges::any_of(shared_masks, [](const auto& kv) { return kv.second >= 3; });
    require(has_sliced_line && distinct_colors.size() >= 3,
            "CodeEditor did not slice a single line Coverage into multi-color syntax DrawQuads.");

    // Verify .ss hex color swatch quad emission
    auto ss_doc = std::make_shared<ui::CodeDocument>(accepted(
        ui::CodeDocument::create(String::literal<".panel { background: #ff4422; }">(), ui::CodeLanguage::style_sheet)));
    editor_ctrl.code = ss_doc;
    editor_ctrl.code_annotations.reset();
    consumer.controls = {editor_ctrl};
    consumer.prepare();
    const bool has_swatch = std::ranges::any_of(consumer.frame->drawing->draws, [](const ui::DrawQuad& q) {
        return !q.coverage && !q.image && q.bounds.width >= 7.f && q.bounds.width <= 12.f && q.bounds.height >= 7.f &&
               q.bounds.height <= 12.f && q.color.r > 0.7f && q.color.b < 0.1f;
    });
    require(has_swatch, "StyleSheet CodeEditor did not render inline #rrggbb color swatch.");

    // Verify auto-pair, smart Enter inside {|}, Ctrl+/ comment toggle, and mark_saved() line diffs
    auto edit_doc = std::make_shared<ui::CodeDocument>(
        accepted(ui::CodeDocument::create(String::literal<"void tick() ">(), ui::CodeLanguage::source)));
    edit_doc->locate(1, 13);
    editor_ctrl.code = edit_doc;
    consumer.controls = {editor_ctrl};
    consumer.prepare();
    consumer.ui.focus(700);
    consumer.prepare();

    ui::Event open_brace{ui::EventKind::text};
    open_brace.data = String::literal<"{">();
    consumer.ui.input(open_brace);
    require(edit_doc->text() == "void tick() {}", "Typing '{' did not auto-insert matching '}'.");

    ui::Event enter_ev{ui::EventKind::key_down};
    enter_ev.key = ui::Key::enter;
    consumer.ui.input(enter_ev);
    require(edit_doc->edit().line_count() == 3 && edit_doc->edit().line(1) == "    " && edit_doc->edit().line(2) == "}",
            "Pressing Enter inside '{|}' did not expand block with 4-space indent.");
    require(edit_doc->line_diff(0) == ui::CodeLineDiff::modified && edit_doc->line_diff(1) == ui::CodeLineDiff::added,
            "CodeDocument::line_diff did not distinguish modified vs added lines.");

    ui::Event comment_ev{ui::EventKind::key_down};
    comment_ev.key = ui::Key::slash;
    comment_ev.control = true;
    edit_doc->locate(1, 1);
    consumer.ui.input(comment_ev);
    require(edit_doc->edit().line(0).starts_with("// "), "Ctrl+/ did not comment the active line.");
    consumer.ui.input(comment_ev);
    require(edit_doc->edit().line(0) == "void tick() {", "Second Ctrl+/ did not uncomment the active line.");

    edit_doc->mark_saved();
    require(!edit_doc->dirty() && edit_doc->line_diff(0) == ui::CodeLineDiff::clean &&
                edit_doc->line_diff(1) == ui::CodeLineDiff::clean && edit_doc->line_diff(2) == ui::CodeLineDiff::clean,
            "CodeDocument::mark_saved() did not reset dirty state and line diffs to clean.");

    // Verify anchor-aligned line_diff (inserting at top does NOT mark unchanged trailing lines as modified),
    // lexical bracket scope resilience across string literals containing '{', and Ctrl+/ caret idempotency.
    auto anchor_doc = std::make_shared<ui::CodeDocument>(accepted(
        ui::CodeDocument::create(String::literal<"void test() {\n    String s = \"{\";\n    int x = 1;\n        ">(),
                                 ui::CodeLanguage::source)));
    anchor_doc->locate(1, 1);
    accepted(anchor_doc->edit().replace("// header\n"));
    require(anchor_doc->dirty() && anchor_doc->line_diff(0) == ui::CodeLineDiff::added &&
                anchor_doc->line_diff(1) == ui::CodeLineDiff::clean &&
                anchor_doc->line_diff(2) == ui::CodeLineDiff::clean &&
                anchor_doc->line_diff(3) == ui::CodeLineDiff::clean,
            "Inserting a line shifted unchanged lines into false-positive modified diffs.");

    editor_ctrl.code = anchor_doc;
    consumer.controls = {editor_ctrl};
    consumer.prepare();
    consumer.ui.focus(700);
    consumer.prepare();

    // Typing '}' at 8-space indent on row 4 should NOT auto-dedent against the '{' inside String literal on row 2,
    // because the real enclosing '{' is at column 0 (expected dedent target column 0 only from 4-space indent).
    anchor_doc->locate(5, 5);
    anchor_doc->edit().select({anchor_doc->edit().line_start(4), false}, false);
    anchor_doc->edit().select({anchor_doc->edit().line_start(4) + 8, true}, true);
    accepted(anchor_doc->edit().replace("    "));
    anchor_doc->locate(5, 5);
    ui::Event close_brace{ui::EventKind::text};
    close_brace.data = String::literal<"}">();
    consumer.ui.input(close_brace);
    require(anchor_doc->edit().line(4) == "}",
            "'}' auto-dedent was corrupted by '{' inside an earlier string literal.");

    // Ctrl+/ with caret inside leading indentation preserves caret column idempotently
    anchor_doc->locate(4, 3); // col 2 (0-based) inside 4-space indent of `    int x = 1;`
    const std::uint32_t before_toggle_caret = anchor_doc->edit().current().caret.offset;
    consumer.ui.input(comment_ev);
    require(anchor_doc->edit().line(3) == "    // int x = 1;" &&
                anchor_doc->edit().current().caret.offset == before_toggle_caret,
            "Ctrl+/ shifted caret when positioned before comment insertion column.");
    consumer.ui.input(comment_ev);
    require(anchor_doc->edit().line(3) == "    int x = 1;" &&
                anchor_doc->edit().current().caret.offset == before_toggle_caret,
            "Second Ctrl+/ was not idempotent on line text and caret position.");

    // StyleSheet Ctrl+/ on sibling block comments `/* a */ color: #fff; /* b */` must not strip outer delimiters
    auto ss_sib = std::make_shared<ui::CodeDocument>(accepted(
        ui::CodeDocument::create(String::literal<"/* a */ color: #ffffff; /* b */">(), ui::CodeLanguage::style_sheet)));
    ss_sib->locate(1, 1);
    editor_ctrl.code = ss_sib;
    consumer.controls = {editor_ctrl};
    consumer.prepare();
    consumer.ui.focus(700);
    consumer.prepare();
    consumer.ui.input(comment_ev);
    require(ss_sib->edit().line(0).starts_with("/* /* a */"),
            "StyleSheet Ctrl+/ erroneously stripped outer delimiters of sibling block comments.");

    // Multi-byte UTF-8 (CJK + surrogate-pair emoji) grapheme navigation and DirectWrite caret mapping
    auto utf8_doc = std::make_shared<ui::CodeDocument>(accepted(
        ui::CodeDocument::create(String::literal<"String msg = \"你好🚀世界\";">(), ui::CodeLanguage::source)));
    utf8_doc->locate(1, 15); // right before '你' (byte offset 14)
    editor_ctrl.code = utf8_doc;
    consumer.controls = {editor_ctrl};
    consumer.prepare();
    consumer.ui.focus(700);
    consumer.prepare();
    ui::Event right_ev{ui::EventKind::key_down};
    right_ev.key = ui::Key::right;
    consumer.ui.input(right_ev); // step across '你' (3 UTF-8 bytes)
    require(utf8_doc->edit().current().caret.offset == 17u,
            "Arrow right did not advance 3 UTF-8 bytes across CJK character.");
    consumer.ui.input(right_ev); // step across '好' (3 UTF-8 bytes)
    consumer.ui.input(right_ev); // step across '🚀' (4 UTF-8 bytes)
    require(utf8_doc->edit().current().caret.offset == 24u,
            "Arrow right did not advance 4 UTF-8 bytes across astral emoji grapheme.");
    consumer.prepare();
    require(consumer.frame->input_anchor.has_value(),
            "UTF-8 CodeEditor lost IME input_anchor after stepping across astral emoji.");

    // Adversarial boundary checks:
    // 1) CodeDocument rejects CR and ASCII control bytes at creation & edit time.
    require(!ui::CodeDocument::create(String::literal<"bad\r\nline">()) &&
                !ui::CodeDocument::create(String::literal<"bad\007bell">()),
            "CodeDocument::create accepted CR or control bytes.");
    // 2) StyleSheet Ctrl+D selects hyphenated property `border-color` as a whole word and skips `border-radius`.
    auto ss_word_doc = std::make_shared<ui::CodeDocument>(accepted(ui::CodeDocument::create(
        String::literal<".a { border-radius: 2dip; border-color: #112233; border-color: #445566; }">(),
        ui::CodeLanguage::style_sheet)));
    const std::size_t first_bc = ss_word_doc->text().find("border-color");
    const std::size_t second_bc = ss_word_doc->text().find("border-color", first_bc + 12);
    ss_word_doc->locate(1, first_bc + 3);
    editor_ctrl.code = ss_word_doc;
    consumer.controls = {editor_ctrl};
    consumer.prepare();
    consumer.ui.focus(700);
    consumer.prepare();
    ui::Event ctrl_d{ui::EventKind::key_down};
    ctrl_d.key = ui::Key::d;
    ctrl_d.control = true;
    consumer.ui.input(ctrl_d); // 1st Ctrl+D selects full `border-color`
    require(ss_word_doc->edit().current().anchor.offset == first_bc &&
                ss_word_doc->edit().current().caret.offset == first_bc + 12,
            "Ctrl+D in StyleSheet split hyphenated identifier `border-color`.");
    consumer.ui.input(ctrl_d); // 2nd Ctrl+D jumps to second `border-color`, not `border` in `border-radius`
    require(ss_word_doc->edit().current().anchor.offset == second_bc &&
                ss_word_doc->edit().current().caret.offset == second_bc + 12,
            "Second Ctrl+D in StyleSheet did not match next whole hyphenated identifier.");
}
}

int main() {
    atelier::install_fatal_handler();
    identity_and_lifetime();
    bounded_document_history();
    repeat_updates();
    integral_slider_and_number_quantization();
    standardized_kit_and_style_contracts();
    verify_code_editor_syntax_and_gestures();
    verify_ui_error_recovery();
    verify_ui_entry_dependencies();
    std::puts(
        "UI acceptance: PASS (accepted frames, bounded document history, Repeat, standardized kit, CodeEditor syntax/gestures, error recovery, editor entry dependencies)");
}
