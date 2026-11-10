// Exercise reversible publication with the real compiler, scopes and Session.
void verify_ui_reload_transaction() {
    namespace language = ui_language;
    Runtime runtime;
    take(ui_language::register_types(runtime));
    const std::string source = R"(
ui Stateful() {
    state bool expanded = true;
    Column root {
        Foldout toggle {
            text: "Transaction child";
            selected: state.expanded;
            on activate { state.expanded = event.checked; }
        }
    }
}
ui Transaction(in bool visible, in TextEditBinding edit, in double rows) {
    Column root {
        Stateful child { mounted: input.visible; }
        TextField later { edit: input.edit; }
        Tree tree { rows: input.rows; TreeItem item { text: "Item"; } }
    }
}
ui Late(in double rows) { Tree tree { rows: input.rows; TreeItem item {} } }
)";
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("atelier-ui-transaction-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    const auto path = folder / "transaction.ui.source";
    take(editor::write_file(path, source));
    ui::ControlKey next_key = 20000;
    language::LiveDocument live(runtime, path, next_key);
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface;
    take(surface.initialize(fonts));
    require(surface.set_surface({{640, 480}, 1, 1}).has_value(), "Transaction surface failed.");
    // A row outside its tree's row count is Session-invalid topology.
    const auto inputs = [](bool visible, ui::ControlKey edit = 7001, double rows = 1) {
        return std::array<language::InputValue, 3>{visible, language::TextEditBinding{edit, L"Draft"}, rows};
    };
    std::vector<ui::Control> controls;
    const auto route = [&](const ui::Reply& reply) {
        require(!reply.error, "Transaction input failed.");
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect))
                take(live.event(*action));
    };
    const auto draw = [&] {
        auto reply = surface.set_controls(controls);
        require(reply.has_value(), "Transaction snapshot rejected.");
        route(*reply);
        live.commit_controls();
        auto frame = surface.prepare();
        require(frame.has_value() && surface.accept(*frame), "Transaction frame failed.");
    };
    const auto append = [&](bool visible, ui::ControlKey edit = 7001, double rows = 1) {
        controls.clear();
        return live.append("Transaction", 0, {}, inputs(visible, edit, rows), controls);
    };
    const auto toggle = [&] {
        const auto at = std::ranges::find_if(
            controls, [](const auto& control) { return control.kind == ui::ControlKind::foldout; });
        require(at != controls.end(), "Transaction child disappeared.");
        return *at;
    };
    live.poll();
    take(append(true));
    draw();
    const auto child_key = toggle().key;
    const auto incarnation = surface.presented()->find(child_key)->incarnation;
    const auto click = [&] {
        const auto* target = surface.presented()->find(child_key);
        require(target, "Retained child has no hit target.");
        const auto box = target->bounds;
        route(surface.input({.kind = ui::EventKind::pointer_down, .x = box.x + 8, .y = box.y + 8}));
        route(surface.input({.kind = ui::EventKind::pointer_up, .x = box.x + 8, .y = box.y + 8}));
    };
    click();
    take(append(true));
    draw();
    require(!toggle().selected, "Initial child event did not update local state.");
    const auto accepted = controls;
    (void)append(false, 0);
    require(std::ranges::equal(controls, accepted), "Failed unmount changed the accepted controls.");
    draw();
    require(surface.presented()->find(child_key)->incarnation == incarnation, "Failure changed child identity.");
    click();
    take(append(true));
    draw();
    require(toggle().key == child_key && toggle().selected,
            "Failure discarded the retained child's event route or state.");
    take(append(false));
    draw();
    require(!surface.presented()->find(child_key), "Successful removal kept the child visible.");
    take(append(true));
    draw();
    require(toggle().key != child_key && toggle().selected, "Successful remount reused a retired scope.");

    const auto live_accepted = controls;
    const auto live_key = toggle().key;
    take(append(false, 7001, 0));
    static_cast<void>(surface.set_controls(controls));
    static_cast<void>(live.reject_controls("Invalid dynamic topology"));
    take(append(false, 7001, 0));
    draw();
    require(std::ranges::equal(controls, live_accepted), "Rejected inputs replaced the accepted controls.");
    take(append(true));
    draw();
    require(live.diagnostic().empty() && toggle().key == live_key,
            "Corrected live inputs retained a stale diagnostic or lost the old scope.");
    const language::KeyBinding different_alias{"root", 9000};
    controls.clear();
    static_cast<void>(live.append("Transaction", 0, std::span{&different_alias, 1}, inputs(true), controls));
    require(std::ranges::equal(controls, live_accepted), "A live region silently changed its key aliases.");
    take(append(true));
    draw();
    require(live.diagnostic().empty(), "Restored aliases did not clear the region diagnostic.");
    const std::array<language::InputValue, 1> invalid_late{0.0};
    const std::array<language::InputValue, 1> valid_late{1.0};
    take(append(true));
    take(live.append("Late", 0, {}, invalid_late, controls));
    static_cast<void>(surface.set_controls(controls));
    static_cast<void>(live.reject_controls("Refused first mount"));
    take(append(true));
    take(live.append("Late", 0, {}, invalid_late, controls));
    draw();
    require(std::ranges::equal(controls, live_accepted) && !live.statistics("Late"),
            "Refused first mount was recreated or retained a scope.");
    take(append(true));
    take(live.append("Late", 0, {}, valid_late, controls));
    draw();
    require(live.diagnostic().empty() && live.statistics("Late").has_value(), "Corrected first mount did not recover.");

    // Session topology rejection is later than successful scope preparation.
    auto module = take(language::Module::compile(runtime, "transaction.ui.source", source));
    auto type = module->find("Transaction");
    auto instance = take(language::Instance::create(type, 0, next_key, {}, inputs(true)));
    const auto find_toggle = [&] {
        const auto values = instance->controls();
        const auto at =
            std::ranges::find_if(values, [](const auto& control) { return control.kind == ui::ControlKind::foldout; });
        require(at != values.end(), "Direct transaction lost its child.");
        return *at;
    };
    const auto direct_key = find_toggle().key;
    require(surface.set_controls(instance->controls()).has_value(), "Direct transaction snapshot rejected.");
    instance->commit_controls();
    require(take(instance->event({ui::ActionKind::activate, direct_key, false})),
            "Direct child event was not handled.");
    take(instance->update());
    require(surface.set_controls(instance->controls()).has_value(), "Direct transaction snapshot rejected.");
    instance->commit_controls();
    const std::vector<ui::Control> direct_accepted(instance->controls().begin(), instance->controls().end());
    take(instance->update(inputs(false, 7001, 0)));
    (void)surface.set_controls(instance->controls());
    instance->reject_controls();
    instance->commit_controls();
    require(std::ranges::equal(instance->controls(), direct_accepted),
            "Session refusal changed accepted scopes or controls.");
    take(instance->update(inputs(true)));
    require(surface.set_controls(instance->controls()).has_value(), "Direct transaction snapshot rejected.");
    instance->commit_controls();
    require(find_toggle().key == direct_key && !find_toggle().selected,
            "Session recovery lost accepted inputs or state.");
    // A closed, retained C++ instance must release its compiled module ownership.
    std::weak_ptr<language::Component> weak = type;
    module.reset();
    type.reset();
    require(!weak.expired(), "Mounted instance did not retain compiled code.");
    require(surface.set_controls({}).has_value(), "Transaction cleanup failed.");
    instance->close();
    require(weak.expired(), "Closed retained instance kept its compiled component alive.");
    std::printf(
        "UI reload transaction acceptance: PASS (failed unmount, retained events, first mount refusal, topology recovery, closed code ownership)\n");
}
