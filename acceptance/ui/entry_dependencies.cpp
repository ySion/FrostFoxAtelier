#include "reload_fixture.hpp"
#include "runtime/native.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

using namespace atelier;

using namespace ui_language;

using acceptance::helpers::require;

using acceptance::helpers::take;

namespace {
void verify_signed_zero_bindings(Runtime& runtime, ui::ControlKey& next) {
    const String child = "ui Child(in double n) { Button b { enabled: (1.0d/input.n) < 0.0d; } } "
                         "ui Rows(in Items<Child> rows) { Repeat r { items: input.rows; } } "_s;
    std::shared_ptr<Module> module;
    std::unique_ptr<Instance> parent;
    ui::ControlKey key{};
    for (const StringView literal : {"0.0d", "-0.0d", "(-0.0d)", "-0", "(-0)", "-0.0f"}) {
        const auto source = String::concat(child, "ui Parent() { Child c { n: ", String::validated(literal), "; } }");
        module = take(Module::compile(runtime, "signed-zero.ui.source", source, module.get()));
        const auto component = module->find("Parent");
        auto fresh = take(Instance::create(component, 0, next, {}, {}));
        const bool negative = literal == "-0.0d" || literal == "(-0.0d)" || literal == "-0.0f";
        require(fresh->controls().front().enabled == negative, "Literal binding changed the Source sign of zero.");
        if (!parent) {
            key = fresh->controls().front().key;
            parent = std::move(fresh);
        } else {
            take(parent->update({}, component));
            parent->commit_controls();
            const auto& button = parent->controls().front();
            require(button.key == key && button.enabled == negative, "Hot and fresh signed-zero bindings differ.");
        }
    }
    const auto row_key = next++;
    const auto items = [row_key](double value) {
        Array<Item> rows{{row_key, {value}}};
        return std::array<InputValue, 1>{ItemList{std::make_shared<const Array<Item>>(std::move(rows))}};
    };
    const auto items_pos_zero = items(0.0);
    const auto items_neg_zero = items(-0.0);
    auto list = take(Instance::create(module->find("Rows"), 0, next, {}, items_pos_zero));
    take(list->update(items_neg_zero));
    list->commit_controls();
    require(list->controls().front().key == row_key && list->controls().front().enabled,
            "Repeat failed to publish a signed-zero change to an existing item.");

    const auto event = take(Component::compile(runtime, "event-zero.ui.source", R"(
        ui Probe(in double n) {
            state double value = 0.0d;
            Button set { on activate { state.value = input.n; } }
            Button indicator { enabled: (1.0d/state.value) < 0.0d; }
            Splitter grip { value: 0.0d * input.n; }
        })",
                                               "Probe"));
    const InputValue one_in[]{1.0};
    const InputValue pos_zero_in[]{0.0};
    const InputValue neg_zero_in[]{-0.0};
    auto instance = take(Instance::create(event, 0, next, {}, one_in));
    take(instance->update(pos_zero_in));
    take(instance->update(neg_zero_in)); // Supersede an uncommitted input.
    instance->commit_controls();
    require(std::signbit(instance->controls()[2].value), "Splitter lost the new input's sign.");
    const auto activate = [&] {
        require(take(instance->event({ui::ActionKind::activate, instance->controls()[0].key, {}})), "Event missed.");
        take(instance->update());
        require(instance->controls()[1].enabled, "Changing only the sign of state did not update the binding.");
    };
    activate();
    instance->reject_controls();
    take(instance->update());
    instance->commit_controls();
    require(!instance->controls()[1].enabled, "Rejecting an event failed to restore positive zero.");
    activate();
    instance->commit_controls();
}

void verify_number_row_sign(std::shared_ptr<Component> component, ui::ControlKey& next) {
    ui::NumberField field;
    field.key = next++;
    EditBinding edit{.group = next++, .enabled = true, .fields = {field}};
    const InputValue initial_edit[]{edit};
    auto row = take(Instance::create(component, 0, next, {}, initial_edit));
    ui::Session session;
    take(session.initialize());
    take(session.set_surface({{600, 200}, 1, 1}));
    const auto present = [&] {
        take(session.set_controls(row->controls()));
        require(session.accept(take(session.prepare())), "NumberRow frame was refused.");
        row->commit_controls();
    };
    present();
    require(!session.focus(field.key).error && session.focused_control() == field.key,
            "Could not focus the numeric field.");
    edit.fields.front().value = -0.0;
    const InputValue updated_edit[]{edit};
    take(row->update(updated_edit));
    present();
    ui::Event shortcut{};
    shortcut.kind = ui::EventKind::key_down;
    shortcut.control = true;
    shortcut.key = ui::Key::a;
    session.input(shortcut);
    shortcut.key = ui::Key::c;
    const auto copy = session.input(shortcut);
    require(std::ranges::any_of(copy.effects,
                                [](const ui::Effect& effect) {
                                    const auto* platform = std::get_if<ui::PlatformRequest>(&effect);
                                    const auto* clipboard =
                                        platform ? std::get_if<ui::ClipboardRequest>(platform) : nullptr;
                                    return clipboard && clipboard->kind == ui::ClipboardKind::copy &&
                                           clipboard->text == L"-0";
                                }),
            "The focused editor number field did not expose the updated -0 text.");
    ui::Event typed{};
    typed.kind = ui::EventKind::text;
    typed.data = std::wstring{L"0"};
    const auto preview = session.input(typed);
    require(std::ranges::any_of(preview.effects,
                                [&](const ui::Effect& effect) {
                                    const auto* action = std::get_if<ui::Action>(&effect);
                                    return action && action->kind == ui::ActionKind::edit_preview &&
                                           action->key == field.key && std::get<double>(action->value) == 0.0 &&
                                           !std::signbit(std::get<double>(action->value));
                                }),
            "Editing -0 to 0 did not deliver the new value to the host.");
}

void verify_state_initializer_order(Runtime& runtime, ui::ControlKey& next) {
    for (const bool parentheses : {false, true}) {
        const auto literal = [&](Utf8Text value) {
            auto text = std::move(value).into_string();
            return parentheses ? String::concat("(", text, ")") : text;
        };
        const auto source = String::concat(
            "ui Order() { state double sign = ", literal("-0.0d"),
            "; state double prefix = 3.0d; state double before = state.later; state double later = ", literal("9.0d"),
            "; state double after = state.prefix + state.later; "
            "state bool early = state.flag; state bool flag = ",
            literal("true"),
            "; Button size { width: state.before; height: state.after; enabled: state.early; } "
            "Button flag { enabled: state.flag; } Button sign { enabled: (1.0d/state.sign) < 0.0d; } }");
        auto module = take(Module::compile(runtime, "order.ui.source", source));
        auto instance = take(Instance::create(module->find("Order"), 0, next, {}, {}));
        const auto controls = instance->controls();
        require(controls[0].layout.width.value == 0 && controls[0].layout.height.value == 12,
                "State initializer must read the current value in declaration order.");
        require(!controls[0].enabled && controls[1].enabled,
                "Boolean state initializer observed a later declaration too early.");
        require(controls[2].enabled, "Numeric state initialization lost the sign of negative zero.");
    }
}

void verify_component_environments(Runtime& runtime, ui::ControlKey& next) {
    // Top-level helpers are pure module-level Source functions: parameters may be named
    // input/state, and helpers do not implicitly capture component inputs or couple components.
    String source = "const double margin = 2.0d;\n"
                    "double extent(double input) { double state = input; return state + margin; }\n"
                    "ui A(in double size) { Button a { height: extent(input.size); } }\n"
                    "ui B(in bool flag, in double size) { Button b { height: extent(input.size); } }"_s;
    auto initial = take(Module::compile(runtime, "environment", source));
    const InputValue a_in[]{30.};
    const InputValue b_in[]{true, 43.};
    auto a = take(Instance::create(initial->find("A"), 0, next, {}, a_in));
    auto b = take(Instance::create(initial->find("B"), 0, next, {}, b_in));
    require(a->controls().front().layout.height.value == 32 && b->controls().front().layout.height.value == 45,
            "Explicit helper parameters computed the wrong component height.");
    const auto decoupled_source = String::concat(source, "\nui C() { Button c { height: 20; } }");
    auto decoupled = take(Module::compile(runtime, "environment", decoupled_source, initial.get()));
    require(decoupled->compilation().compiled == 1 && decoupled->compilation().reused == 2,
            "Unrelated component failed or invalidated sibling components.");
    auto implicit_capture =
        Module::compile(runtime, "environment",
                        "double bad() { return input.size; }\nui A(in double size) { Button a { height: bad(); } }");
    require(!implicit_capture, "Top-level helper implicit input capture must be rejected.");
    auto empty_property = Module::compile(runtime, "environment", "ui A() { Button a { height: ; } }");
    require(!empty_property && empty_property.error().message.contains("Expected a UI expression"),
            "Empty UI property expression escaped frontend validation.");
    // Pure-declarative components bypass LLVM/Program creation while preserving hot reload and helper diagnostics.
    const String plain_v1 = "ui Plain(in double w) { state bool active = true; Button b { enabled: state.active; "
                            "out_of_flow: true; offset_x: -16; width: input.w; height: 20; } }"_s;
    const String plain_v2 = "ui Plain(in double w) { state bool active = true; Button b { enabled: state.active; "
                            "out_of_flow: true; offset_x: -16; width: input.w; height: 24; } }"_s;
    auto plain1 = take(Module::compile(runtime, "plain", plain_v1));
    require(!plain1->find("Plain")->declaration().needs_jit,
            "Pure-declarative UI component unexpectedly required JIT.");
    const InputValue width_80[]{80.};
    const InputValue width_96[]{96.};
    auto plain_instance = take(Instance::create(plain1->find("Plain"), 0, next, {}, width_80));
    require(plain_instance->controls().front().layout.width.value == 80 &&
                plain_instance->controls().front().layout.height.value == 20 &&
                plain_instance->controls().front().layout.offset_x == -16,
            "Pure-declarative UI component evaluated wrong initial dimensions.");
    auto plain2 = take(Module::compile(runtime, "plain", plain_v2, plain1.get()));
    require(plain2->find("Plain")->compilation() == Component::Compilation::compiled,
            "Pure-declarative literal change was not reported as compiled.");
    take(plain_instance->update(width_96, plain2->find("Plain")));
    plain_instance->commit_controls();
    require(plain_instance->controls().front().layout.width.value == 96 &&
                plain_instance->controls().front().layout.height.value == 24,
            "Pure-declarative UI component did not update after hot reload.");
    const auto plain_commented = String::concat("// comment\n", plain_v2);
    auto plain3 = take(Module::compile(runtime, "plain", plain_commented, plain2.get()));
    require(plain3->find("Plain")->compilation() == Component::Compilation::reused_source,
            "Comment-only edit invalidated a pure-declarative UI component.");
    const auto bad_helper_source = String::concat("double bad() { return \"oops\"; }\n", plain_v1);
    auto bad_declarative_helper = Module::compile(runtime, "plain", bad_helper_source);
    require(!bad_declarative_helper && bad_declarative_helper.error().message.contains(":1:23: Type mismatch"),
            bad_declarative_helper ? "Invalid helper in a pure-declarative UI module compiled."_s
                                   : bad_declarative_helper.error().message);
    // Editing a literal inside a mixed JIT component reuses machine code while still updating the live control.
    const String mixed_v1 = "ui Mixed(in double w) { Button b { width: input.w + 1.0d; height: 20; } }"_s;
    const String mixed_v2 = "ui Mixed(in double w) { Button b { width: input.w + 1.0d; height: 28; } }"_s;
    auto mixed1 = take(Module::compile(runtime, "mixed", mixed_v1));
    require(mixed1->find("Mixed")->declaration().needs_jit, "Mixed UI component should require JIT.");
    auto mixed_instance = take(Instance::create(mixed1->find("Mixed"), 0, next, {}, width_80));
    require(mixed_instance->controls().front().layout.width.value == 81 &&
                mixed_instance->controls().front().layout.height.value == 20,
            "Mixed UI component evaluated wrong initial dimensions.");
    auto mixed2 = take(Module::compile(runtime, "mixed", mixed_v2, mixed1.get()));
    require(mixed2->find("Mixed")->compilation() == Component::Compilation::reused_source,
            "Literal-only edit in a mixed UI component unexpectedly recompiled JIT code.");
    take(mixed_instance->update(width_80, mixed2->find("Mixed")));
    mixed_instance->commit_controls();
    require(mixed_instance->controls().front().layout.width.value == 81 &&
                mixed_instance->controls().front().layout.height.value == 28,
            "Literal-only edit in a mixed UI component did not update the live control.");
    require(source.replace(source.find("2.0d"), 4, "5.0d").has_value(), "replace margin constant");
    auto changed = take(Module::compile(runtime, "environment", source, initial.get()));
    take(a->update(a_in, changed->find("A")));
    require(changed->compilation().compiled == 2 && a->controls().front().layout.height.value == 35,
            "Changed module constant was dropped from the entry dependencies.");
    struct Extra {
        std::uint32_t id;
    };
    take(runtime.register_type(
        NativeRegistration<Extra>(runtime, "ui.AcceptanceExtra", TypeKind::record)
            .field<&Extra::id>("id", {TypeReference::native<std::uint32_t>()}, offsetof(Extra, id))
            .build()));
    auto rebound = take(Module::compile(runtime, "environment", source, changed.get()));
    require(rebound->compilation().compiled == 2, "Code was reused across a changed type catalog.");
}
}

void verify_ui_entry_dependencies() {
    std::ifstream editor(ATELIER_EDITOR_UI_SOURCE, std::ios::binary | std::ios::ate);
    require(editor.good(), "Cannot open assets/editor.ui.source.");
    const auto editor_size = editor.tellg();
    require(editor_size >= 0, "Cannot inspect assets/editor.ui.source size.");
    Array<char> editor_bytes;
    editor_bytes.resize_uninitialized(static_cast<std::size_t>(editor_size));
    editor.seekg(0);
    if (!editor_bytes.empty())
        require(bool(editor.read(editor_bytes.data(), static_cast<std::streamsize>(editor_bytes.size()))),
                "Cannot read assets/editor.ui.source.");
    auto valid_source = String::from_utf8({editor_bytes.data(), editor_bytes.size()});
    require(valid_source.has_value(), "assets/editor.ui.source is not valid UTF-8.");
    String source = std::move(*valid_source);
    const auto declarations = take(lower_module("editor.ui.source", source));
    require(declarations.size() == 48, "Update the real-editor dependency workload when its component set changes.");
    const auto jit_components = std::ranges::count_if(declarations, [](const auto& item) { return item.needs_jit; });
    require(jit_components == 20, "Unexpected JIT component count for assets/editor.ui.source.");
    auto now = LiveDocument::Clock::now();
    const auto file_name = String::concat("editor-dependencies-", now.time_since_epoch().count(), ".ui.source");
    const auto path = std::filesystem::temp_directory_path() / file_name.c_str();
    Runtime runtime;
    take(register_types(runtime));
    ui::ControlKey next = 100;
    LiveDocument document(runtime, path, next);
    ui::Session session;
    take(session.initialize());
    take(session.set_surface({{1100, 600}, 1, 1}));
    const std::array<KeyBinding, 1> keys{{{"shelf", 11}}};
    const std::array<InputValue, 12> inputs{
        false, true, ItemList{}, ItemList{}, ActionBinding{41}, ActionBinding{42}, ActionBinding{43}, 36.,
        24.,   16.,  0.,         0.};
    using namespace acceptance::ui_reload;
    const auto reload = [&](const auto& text) {
        save(path, StringView{text});
        begin(document, now);
        drain(document, now);
    };
    const auto present = [&](float height) {
        Array<ui::Control> controls;
        take(document.append("ToolShelf", 0, keys, inputs, controls));
        take(session.set_controls(controls));
        auto frame = take(session.prepare());
        require(session.accept(frame), "Editor UI frame was refused.");
        document.commit_controls();
        const auto* shelf = frame->find(11);
        require(shelf && shelf->bounds.height == height, "Accepted ToolShelf height differs from the source.");
    };
    using Compilation = Component::Compilation;
    const auto changed = [&](std::initializer_list<StringView> compiled, Compilation reused) {
        require(document.diagnostic().empty(), document.diagnostic());
        for (const auto& declaration : declarations) {
            const auto component = document.component(declaration.name);
            const auto expected =
                std::ranges::find(compiled, declaration.name.view()) == compiled.end() ? reused : Compilation::compiled;
            require(component && component->compilation() == expected, declaration.name);
        }
    };
    const auto replace = [&](StringView original, StringView replacement) {
        const auto at = source.find(original);
        require(at != StringView::npos && source.find(original, at + 1) == StringView::npos, original);
        require(source.replace(at, original.size(), replacement).has_value(), original);
    };
    reload(source);
    present(56);
    verify_number_row_sign(document.component("WorkbenchRgb"), next);
    const auto old = document.component("ToolShelf");
    const auto initial_shelf_stats = document.statistics("ToolShelf");
    require(initial_shelf_stats.has_value(), "Missing initial ToolShelf statistics.");
    const auto instance = initial_shelf_stats->instance;
    const auto types = runtime.type_count();
    replace("return 56.0d;", "return 57.0d;");
    reload(source);
    present(57);
    changed({"ToolShelf"}, Compilation::reused_dependencies);
    require(source.insert(0, "// move all author positions\n").has_value(), "insert comment in editor source");
    reload(source);
    present(57);
    changed({}, Compilation::reused_source);
    const auto revision = document.revision();
    reload(String::concat("double unused_probe() { return \"wrong\"; }\n", source));
    require(document.diagnostic().contains(":1:32: Type mismatch") && document.revision() == revision &&
                document.committed_source() == source,
            document.diagnostic());
    present(57);
    for (const StringView type_decl :
         {"struct RejectedUiType { double value; };\n", "[Flags] enum RejectedUiFlags : uint { bit = 1 }\n"}) {
        reload(String::concat(String::validated(type_decl), source));
        require(document.diagnostic().contains(":1:1:") &&
                    document.diagnostic().contains("do not support Source struct or enum declarations") &&
                    document.revision() == revision && document.committed_source() == source &&
                    runtime.type_count() == types,
                document.diagnostic());
        present(57);
    }
    require(source.insert(0, "double unused_probe() { return 1.0d; }\n").has_value(), "insert valid unused_probe");
    reload(source);
    present(57);
    changed({}, Compilation::reused_dependencies);
    replace("return 57.0d;", "return rail_button_length(180.0d);");
    reload(source);
    present(36);
    changed({"ToolShelf"}, Compilation::reused_dependencies);
    replace("return 36.0d;", "return 38.0d;");
    reload(source);
    present(38);
    changed({"PaneDivider", "ToolShelf"}, Compilation::reused_dependencies);
    const auto helper_shelf_stats = document.statistics("ToolShelf");
    require(helper_shelf_stats && helper_shelf_stats->instance == instance && runtime.type_count() == types,
            "Helper changes replaced the accepted UI context or type catalog.");
    // Save again after dispatch but before the next file poll. Both successful
    // code and a diagnostic from the previous request must be discarded.
    const auto supersede = [&](const String& first, const String& last, float old_height) {
        const auto revision_before = document.revision();
        const auto accepted_before = document.committed_source();
        save(path, first);
        begin(document, now);
        require(document.compiling(), "Expected a detached editor compilation.");
        const auto first_stamp = std::filesystem::last_write_time(path);
        save(path, last);
        if (first.size() == last.size())
            std::filesystem::last_write_time(path, first_stamp);
        present(old_height);
        drain(document, now);
        present(old_height);
        require(document.revision() == revision_before && document.committed_source() == accepted_before &&
                    document.diagnostic().empty(),
                "Stale code or diagnostics escaped supersession.");
        now += std::chrono::milliseconds(300);
        document.poll(now);
        drain(document, now);
    };
    auto newer = source;
    require(newer.replace(newer.find("38.0d"), 2, "42").has_value(), "replace 38 with 42");
    const auto broken = String::concat("double unused_invalid(){return true;}\n", newer);
    auto newest = newer;
    require(newest.replace(newest.find("42.0d"), 2, "43").has_value(), "replace 42 with 43");
    supersede(newer, newest, 38); // Identical size and timestamp; contents select the winner.
    present(43);
    require(document.committed_source() == newest, "An older same-stamp save won the race.");
    supersede(newer, broken, 43);
    require(document.diagnostic().contains("Type mismatch"), "Latest bad save did not report a type error.");
    present(43);
    save(path, newer);
    begin(document, now);
    require(document.compiling() && document.attempted_source() == broken &&
                document.diagnostic().contains("Type mismatch"),
            "A running build attached the previous diagnostic to newer source.");
    drain(document, now);
    present(42);
    require(document.diagnostic().empty() && document.committed_source() == newer, "Valid edit did not recover.");
    supersede(broken, source, 42);
    present(38);
    const auto recovered_shelf_stats = document.statistics("ToolShelf");
    require(document.committed_source() == source && recovered_shelf_stats &&
                recovered_shelf_stats->instance == instance,
            "A superseded failure displaced the accepted editor instance.");
    auto pinned = take(Instance::create(old, 0, next, keys, inputs));
    require(pinned->controls().front().layout.height.value == 56, "Old ToolShelf lost its pinned entry.");
    // A new Runtime type invalidates all 44 capture-time reuse decisions.
    save(path, newer);
    begin(document, now);
    require(document.compiling(), "Expected a live compiler during catalog growth.");
    verify_state_initializer_order(runtime, next);
    verify_signed_zero_bindings(runtime, next);
    verify_component_environments(runtime, next);
    drain(document, now);
    present(42);
    for (const auto& declaration : declarations)
        require(document.component(declaration.name)->compilation() == Compilation::compiled,
                "Code survived a changed Runtime catalog during background compilation.");
    save(path, source);
    begin(document, now);
    require(document.compiling(), "Document close did not exercise an outstanding build.");
    std::filesystem::remove(path); // The document joins before its Runtime goes away.
}
