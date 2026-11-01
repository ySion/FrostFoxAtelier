// Real source file -> DSL/JIT -> docked native CodeEditor -> edits/save ->
// publication/error recovery. No mock compiler, document or text input path.
void verify_project_source_diagnostics(const std::filesystem::path& folder, ui::Session& surface) {
    auto project = take(editor::Project::create(folder));
    const auto helper = folder / "Source/Math.source";
    const auto component = folder / "Source/Move.source";
    std::string saved = "// Saved helper\nfloat travel(float speed,float dt) { return speed * dt; }\n";
    std::string broken = "// Changed helper\n\nfloat travel(float speed,float dt) {\n    return missing;\n}\n";
    saved.resize(256, ' ');
    broken.resize(256, ' ');
    take(editor::write_file(helper, saved));
    take(editor::write_file(
        component,
        "class project.Move : world.Component { float speed=1.0f; void tick(float dt) { self.translate_owner(travel(self.speed,dt),0.0f,0.0f); } }"));
    auto loaded = editor::load_project_scene(project, take(assets::read_file(folder / "Content/Scenes/Main.scene")));
    require(loaded.session && loaded.code, "Project diagnostic scene did not load.");
    editor::Workspace workspace;
    workspace.project_open = true;
    workspace.project_build = loaded.code->report();
    workspace.dock.select(editor::source_control::tab);
    take(workspace.source_editor.open(helper));
    auto* target = workspace.source_editor.active();
    const auto old_document = target->document;
    take(workspace.source_editor.open(component));
    const auto caller = workspace.source_editor.active()->key;
    surface.focus(0);
    const auto frame = [&] {
        const auto controls = workspace.describe(*loaded.session, {1200, 800}, surface.presented().get(), &project);
        require(surface.set_controls(controls).has_value(), "Project diagnostic controls failed.");
        workspace.commit_ui_controls();
        const auto next = surface.prepare();
        require(next.has_value() && surface.accept(*next), "Project diagnostic frame failed.");
    };
    frame();
    // Keep poll metadata unchanged to model the interval before its next read,
    // regardless of compiler speed. Explicit Build still reads actual bytes.
    const auto stamp = std::filesystem::last_write_time(helper);
    take(editor::write_file(helper, broken));
    std::filesystem::last_write_time(helper, stamp);
    require(!loaded.code->poll(project, true), "Broken helper unexpectedly compiled.");
    workspace.project_build = loaded.code->report();
    const auto& error = workspace.project_build->error;
    require(error && error->source && error->source->path == "Source/Math.source",
            "Project diagnostic lost its source file.");
    frame();
    require(target->document == old_document && target->document->text() == assets::wide(saved) && !target->dirty(),
            "Project diagnostic setup did not retain the stale clean buffer.");
    require(workspace.source_editor.problem_current() && workspace.source_editor.locate_problem(),
            "Project diagnostic did not refresh the clean target to its compiled source.");
    const auto current = target->document;
    require(workspace.source_editor.active() == target && current != old_document &&
                current->text() == assets::wide(broken) && !target->dirty(),
            "Project diagnostic selected stale source or dirtied its refreshed buffer.");
    require(current->edit().current().caret.offset ==
                current->edit().line_start(error->source->line - 1) + error->source->column - 1,
            "Project diagnostic caret does not identify the compiled source location.");
    require(workspace.source_editor.locate_problem() && target->document == current,
            "Repeated diagnostic navigation replaced the current source document.");

    require(current->edit().replace(L"draft_") == ui::detail::TextEdit::Change::text && target->dirty(),
            "Could not create the diagnostic target draft.");
    const auto draft = current->text();
    require(workspace.source_editor.select(caller), "Could not return to the diagnostic caller.");
    frame();
    require(!workspace.source_editor.locate_problem() && target->document == current && current->text() == draft &&
                target->dirty(),
            "Cross-file diagnostic overwrote an unsaved target draft.");
    require(current->edit().history(false) == ui::detail::TextEdit::Change::text && !target->dirty(),
            "Could not undo the target draft.");
    require(workspace.source_editor.select(caller), "Could not select caller before the disk-version check.");
    frame();
    take(editor::write_file(helper, saved));
    require(!workspace.source_editor.locate_problem() && target->document == current &&
                current->text() == assets::wide(broken),
            "Project diagnostic used a source version that changed after compilation.");
    take(loaded.code->poll(project, true));
    workspace.project_build = loaded.code->report();
    frame();
    require(!workspace.source_editor.problem_current(), "A corrected project retained a stale diagnostic target.");
    std::puts(
        "Project source diagnostics: PASS (compiled cross-file locations, stale clean buffer, repeated navigation, dirty draft and changed-disk rejection)");
}

void verify_source_editor() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("atelier-source-editor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    editor::Session scene;
    editor::Workspace workspace;
    const auto original = take(assets::read_file(workspace.ui_source()));
    const auto path = folder / "editor.ui.source";
    take(editor::write_file(path, original));
    workspace.set_ui_source(path);
    take(workspace.source_editor.open(path));
    workspace.dock.select(editor::source_control::tab);
    const auto* file = workspace.source_editor.active();
    require(file, "Source document was not opened.");
    const auto document = file->document;
    const auto file_key = file->key;
    take(workspace.source_editor.open(path.parent_path() / "." / path.filename()));
    require(workspace.source_editor.files().size() == 1, "Repeated source open created a second buffer.");
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring(),
                           (editor::asset_directory() / "fonts/SourceCodePro-Regular.otf").wstring()};
    ui::Session surface(fonts);
    require(surface.set_surface({{1200, 800}, 1, 1}).has_value(), "Source surface failed.");
    auto theme = ui::StyleSheet::parse(take(assets::read_file(editor::asset_directory() / "editor.ss")));
    require(theme.has_value(), "Source theme failed.");
    require(surface.set_style_sheet(*theme).has_value(), "Source theme publication failed.");
    const auto frame = [&] {
        const auto controls = workspace.describe(scene, {1200, 800}, surface.presented().get());
        auto reply = surface.set_controls(controls);
        require(reply.has_value(), "Source control publication failed.");
        workspace.commit_ui_controls();
        auto next = surface.prepare();
        require(next.has_value() && surface.accept(*next), "Source frame failed.");
    };
    const auto input = [&](ui::Event event) {
        const auto reply = surface.input(event);
        require(!reply.error, "Source input failed.");
    };
    const auto key = [&](ui::Key value, bool control = false, bool shift = false) {
        input({.kind = ui::EventKind::key_down, .key = value, .shift = shift, .control = control});
        input({.kind = ui::EventKind::key_up, .key = value, .shift = shift, .control = control});
    };
    const auto wait_for = [&](auto condition, const char* message) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        do {
            frame();
            if (condition())
                return;
            Sleep(25);
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error(std::string(message) + ": " + std::string(workspace.ui_error()));
    };
    frame();
    const auto* geometry = surface.presented()->find_code(editor::source_control::buffer);
    require(geometry && !geometry->lines.empty() && geometry->lines.size() < document->edit().line_count() / 4,
            "CodeEditor failed to virtualize the real multi-hundred-line DSL file.");
    surface.focus(editor::source_control::buffer);
    frame();
    key(ui::Key::end, true);
    input({.kind = ui::EventKind::text, .data = std::wstring(L"\n// Native authoring: Unicode 中文 😀\n")});
    frame();
    require(file->dirty() && scene.undo_count() == 0,
            "Source input did not dirty its own document independently of scene history.");
    const auto revision = workspace.ui_revision();
    take(workspace.source_editor.save(*workspace.source_editor.active()));
    require(!file->dirty(), "Successful source save retained the modified flag.");
    wait_for([&] { return workspace.ui_revision() > revision; }, "Saved source did not reach the live compiler");
    require(workspace.source_editor.active()->document == document,
            "UI publication replaced the source editor's buffer/history.");
    surface.focus(editor::source_control::buffer);
    key(ui::Key::end, true);
    input({.kind = ui::EventKind::text, .data = std::wstring(L"ui Broken( {\n")});
    frame();
    take(workspace.source_editor.save(*workspace.source_editor.active()));
    const auto good_revision = workspace.ui_revision();
    wait_for([&] { return !workspace.ui_error().empty(); }, "Invalid saved source produced no compiler diagnostic");
    require(workspace.ui_revision() == good_revision && workspace.source_editor.problem_current() &&
                workspace.source_editor.locate_problem(),
            "Failed publication lost the old UI or its actionable source location.");
    require(document->edit().line_at(document->edit().current().caret.offset) > 500,
            "Diagnostic navigation did not locate the source error.");
    surface.focus(editor::source_control::buffer);
    key(ui::Key::z, true);
    frame();
    require(!workspace.source_editor.problem_current(),
            "Stale diagnostics remained actionable after an unsaved correction.");
    take(workspace.source_editor.save(*workspace.source_editor.active()));
    wait_for([&] { return workspace.ui_revision() > good_revision && workspace.ui_error().empty(); },
             "Source error recovery failed");
    const auto saved = document->text();
    // Clipboard and IME retain the same token/accepted-caret protocol as fields.
    surface.focus(editor::source_control::buffer);
    key(ui::Key::end, true);
    frame();
    const auto request = surface.input({.kind = ui::EventKind::key_down, .key = ui::Key::v, .control = true});
    std::optional<ui::ClipboardRequest> clipboard;
    for (const auto& effect : request.effects)
        if (const auto* platform = std::get_if<ui::PlatformRequest>(&effect))
            if (const auto* value = std::get_if<ui::ClipboardRequest>(platform))
                clipboard = *value;
    require(clipboard.has_value(), "Source paste did not use the native clipboard protocol.");
    input({.kind = ui::EventKind::clipboard_result,
           .data = ui::ClipboardResult{clipboard->kind, clipboard->operation_id, clipboard->token, true, 0,
                                       L"// pasted\r\n"}});
    require(document->text().find(L'\r') == std::wstring::npos, "Source paste retained CR in the LF document buffer.");
    key(ui::Key::z, true);
    require(document->text() == saved, "Source paste undo did not restore the saved revision.");
    frame();
    const auto ime_token = surface.text_input()->token;
    input({.kind = ui::EventKind::composition,
           .data = ui::CompositionEvent{ui::CompositionKind::start, 99, ime_token, {}, {}}});
    input({.kind = ui::EventKind::composition,
           .data = ui::CompositionEvent{ui::CompositionKind::update, 99, ime_token, {}, ui::Preedit{L"zhong", 3, {}}}});
    frame();
    require(surface.text_input()->accepted_anchor && surface.text_input()->composing && document->text() == saved,
            "Source IME preedit changed the document or lost its accepted anchor.");
    input({.kind = ui::EventKind::composition,
           .data = ui::CompositionEvent{ui::CompositionKind::update, 99, ime_token, std::wstring(L"中文"), {}}});
    input({.kind = ui::EventKind::composition,
           .data = ui::CompositionEvent{ui::CompositionKind::end, 99, surface.text_input()->token, {}, {}}});
    key(ui::Key::z, true);
    require(document->text() == saved, "Source IME result failed to enter independent undo as one edit.");
    const auto stale = surface.prepare();
    require(stale.has_value(), "Source frame candidate missing.");
    key(ui::Key::home, true);
    require(!surface.accept(*stale), "A source frame with a stale selection was accepted.");
    frame();
    key(ui::Key::end, true);
    input({.kind = ui::EventKind::text, .data = std::wstring(L"// local draft\n")});
    frame();
    take(editor::write_file(path, original + "\n// external writer\n"));
    const auto refused = workspace.source_editor.save(*workspace.source_editor.active());
    require(!refused && file->conflict && take(assets::read_file(path)) == original + "\n// external writer\n",
            "Source save silently overwrote external changes.");
    take(workspace.source_editor.reload(*workspace.source_editor.active()));
    frame();
    require(!file->dirty() && !file->conflict && file->document != document,
            "Explicit reload failed to replace the conflicted draft.");
    const auto other = folder / "other.source";
    take(editor::write_file(other, "// other\r\n"));
    take(workspace.source_editor.open(other));
    frame();
    surface.focus(editor::source_control::buffer);
    key(ui::Key::end, true);
    input({.kind = ui::EventKind::text, .data = std::wstring(L"// new\n")});
    take(workspace.source_editor.save(*workspace.source_editor.active()));
    require(take(assets::read_file(other)) == "// other\r\n// new\r\n",
            "Source save failed to preserve the file's line-ending convention.");
    workspace.source_editor.select(file_key);
    frame();
    workspace.dock.close(editor::source_control::tab);
    frame();
    workspace.dock.select(editor::source_control::tab);
    frame();
    require(workspace.source_editor.active() == file && surface.presented()->find_code(editor::source_control::buffer),
            "Hiding and reopening Code lost document state.");
    workspace.source_editor.clear();
    frame();
    require(!surface.presented()->find(editor::source_control::buffer)->enabled,
            "Closed source left a live editing target.");
    require(!workspace.operators.find("edit.undo")->poll(workspace.operator_context(scene, nullptr, true)) &&
                !workspace.operators.find("file.save")->poll(workspace.operator_context(scene, nullptr, true)),
            "An empty Code panel routed save/undo to the scene.");
    take(editor::write_file(path, "ui Broken( {"));
    editor::Session cold_scene;
    editor::Workspace cold;
    cold.set_ui_source(path);
    take(cold.source_editor.open(path));
    cold.dock.select(editor::source_control::tab);
    const auto recovered = cold.describe(cold_scene, {1200, 800}, nullptr);
    require(!cold.ui_error().empty() &&
                std::ranges::any_of(recovered,
                                    [](const auto& control) { return control.kind == ui::ControlKind::code_editor; }),
            "An invalid UI file at startup prevented opening the editor to repair it.");
    require(surface.set_controls(recovered).has_value(), "Recovery UI controls were invalid.");
    cold.commit_ui_controls();
    const auto recovery_frame = surface.prepare();
    require(recovery_frame.has_value() && surface.accept(*recovery_frame),
            "Recovery UI did not produce an accepted frame.");
    cold.reset_document();
    const auto restarted = cold.describe(cold_scene, {1200, 800}, nullptr);
    require(std::ranges::any_of(restarted,
                                [](const auto& control) { return control.kind == ui::ControlKind::code_editor; }),
            "Recreating UI scopes lost the committed source recovery checkpoint.");
    verify_project_source_diagnostics(folder / "Project", surface);
    std::puts(
        "Editor source authoring: PASS (real DSL, native input, save/JIT, diagnostics, recovery, conflicts, retained documents)");
}
