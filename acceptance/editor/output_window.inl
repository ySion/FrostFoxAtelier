void verify_output_window(Application& app, const std::filesystem::path& evidence) {
    using namespace atelier;
    using Access = application::EditorAcceptanceAccess;
    namespace controls = editor::output_control;
    auto* run = logging::current_run();
    require(run, "Output window acceptance needs the real process logging host.");
    auto& output = Access::output(app);
    const auto wait = [&](auto predicate, const char* message) {
        // Large-body backlog drains under the same frame budget in Dev; this
        // is a completion bound, not a Release-speed throughput requirement.
        const auto deadline = GetTickCount64() + 60000;
        do {
            tick(app, 1);
            if (predicate())
                return;
        } while (GetTickCount64() < deadline);
        throw std::runtime_error(message);
    };
    const auto submit = [&](logging::Entry entry) {
        require(output.submit_owned(std::move(entry)).status == logging::SubmitStatus::accepted,
                "Output fixture was not accepted by Quill.");
    };
    const auto flush = [&] {
        const auto result = run->flush();
        require(result.backend_acknowledged && result.sink_flush_succeeded,
                "Output fixture could not flush its real JSONL file.");
    };
    const auto find = [&](std::string_view code) {
        for (const auto& row : output.rows())
            if (output.record(row.key)->entry.event_code == code)
                return row.key;
        return ui::TableKey{};
    };
    const auto select_row = [&](ui::TableKey row) {
        const auto* table = app.presented()->find_table(controls::table);
        require(table, "Output has no accepted table geometry.");
        const auto cell = std::ranges::find_if(
            table->cells, [&](const auto& value) { return value.row == row && value.column == 1; });
        require(cell != table->cells.end(), "Output fixture row is not in the accepted visible range.");
        click_at(app, {static_cast<LONG>(cell->bounds.x + cell->bounds.width * .5f),
                       static_cast<LONG>(cell->bounds.y + cell->bounds.height * .5f)});
    };
    const auto source = evidence / "output-location.source";
    const std::string source_text = "// output source\n中文target\n";
    model(editor::write_file(source, source_text));
    logging::Entry long_entry{
        .category = "acceptance.detail", .event_code = "output.long", .message = "Complete body: 中文与 Unicode\nNUL="};
    long_entry.message.push_back('\0');
    long_entry.message += "\nControl=";
    long_entry.message.push_back('\1');
    long_entry.message += '\n';
    while (long_entry.message.size() < logging::maximum_message_bytes)
        long_entry.message += std::string(96, 'x') + "\n";
    long_entry.message.resize(logging::maximum_message_bytes - 4);
    long_entry.message += "END!";
    const auto evidence_text = platform::utf16_to_utf8(evidence.native());
    const auto source_path_text = platform::utf16_to_utf8(source.generic_wstring());
    require(evidence_text && source_path_text, "Invalid UTF-16 Output fixture path.");
    long_entry.source = {.file = "output-location.source",
                         .root = *evidence_text,
                         .line = 2,
                         .column = 7,
                         .version = atelier::source::content_hash(source_text),
                         .disk_path = *source_path_text};
    submit(long_entry);
    flush();
    Access::output_layout(app);
    tick(app, 2);
    wait([&] { return find("output.long"); }, "JSONL records did not reach the real Output pane.");
    for (const auto key : {controls::category, controls::search}) {
        const auto* field = app.presented()->find(key);
        require(field && field->bounds.width > app.presented()->find(controls::table)->bounds.width * .5f,
                "Wide Output filters did not expand into the available pane width.");
    }
    capture(app, evidence / "output-default.png");

    click(app, controls::category);
    type(app, L"acceptance.detail");
    key(app, VK_RETURN);
    require(output.rows().size() == 1, "Native category filter did not isolate the full-body record.");
    select_row(output.rows().front().key);
    const auto* body = app.presented()->find(controls::body);
    require(body && body->read_only && body->code && body->code->text().ends_with(L"END!") &&
                body->code->text().find(L"\\u0000") != std::wstring::npos &&
                body->code->text().find(L"\\u0001") != std::wstring::npos &&
                output.selected_record()->entry.message == long_entry.message,
            "Output truncated or changed the full 128 KiB body.");
    const auto document = body->code;
    const auto full = document->text();
    const auto history = app.document().undo_count();
    const auto wide_body_width = body->bounds.width;
    click(app, controls::body);
    key(app, 'A', true);
    key(app, 'C', true);
    require(!Access::text_input(app), "The read-only Output body enabled native IME.");
    require(OpenClipboard(app.window()) != FALSE, "Cannot inspect the real Output clipboard result.");
    {
        struct ClipboardScope {
            ~ClipboardScope() { CloseClipboard(); }
        } close;
        const auto handle = GetClipboardData(CF_UNICODETEXT);
        require(handle, "Read-only copy did not publish Unicode clipboard text.");
        const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
        require(text, "Cannot inspect the Output clipboard allocation.");
        const bool exact = std::wstring_view(text) == full;
        GlobalUnlock(handle);
        require(exact, "Native Output copy omitted part of its complete escaped body.");
    }
    for (const auto command : {WPARAM{VK_DELETE}, WPARAM{VK_BACK}})
        key(app, command);
    for (const auto command : {WPARAM{'X'}, WPARAM{'V'}, WPARAM{'Z'}, WPARAM{'Y'}})
        key(app, command, true);
    type(app, L"must not be written");
    require(document->text() == full && app.document().undo_count() == history,
            "Read-only Output mutated its body or routed a prohibited action into scene history.");
    key(app, VK_HOME, true);
    capture(app, evidence / "output-body-read-only.png");
    require(output.selected_directory() == run->info().directory &&
                app.presented()->find(controls::open_directory)->enabled,
            "Open Logs lost the selected record's original run directory.");
    click(app, controls::locate);
    tick(app, 2);
    const auto* code = app.presented()->find(editor::source_control::buffer);
    require(code && !code->read_only && code->code->text() == platform::utf8_to_utf16_validated(source_text) &&
                code->code->edit().current().caret.offset == 19,
            "Output Locate lost original root/UTF-8 column or made SourceEditor read-only.");
    require(Access::focused(app) == editor::source_control::buffer && Access::text_input(app),
            "Output Locate did not transfer native editing focus to SourceEditor.");
    const auto editable = code->code;
    click(app, editor::source_control::buffer);
    type(app, L"editable");
    require(editable->text() != platform::utf8_to_utf16_validated(source_text),
            "SourceEditor stopped accepting ordinary edits.");
    key(app, 'Z', true);
    require(editable->text() == platform::utf8_to_utf16_validated(source_text),
            "SourceEditor undo did not restore the source fixture.");
    Access::select_tab(app, control::output_tab);
    tick(app, 2);

    Access::output_layout(app, true);
    tick(app, 2);
    require(app.presented()->find(controls::body) && app.presented()->find(controls::health) &&
                app.presented()->find(controls::category)->bounds.width >= 32,
            "Narrow Output lost its detail, filters or independent loss state.");
    require(app.presented()->find(controls::body)->bounds.width < wide_body_width * .65f,
            "Narrow Output fixture did not actually reduce the accepted pane width.");
    capture(app, evidence / "output-narrow.png");

    // A real material load failure, not an entry submitted by the fixture.
    auto project = model(editor::Project::create(evidence / ("MaterialError-" + run->info().run_id)));
    // Prepare valid author files independently of the material-creation UI.
    auto material = model(project.assets().material("builtin/material/default-grid"));
    auto& material_source = std::get<assets::MaterialSourceAsset>(material.author);
    const auto source_file =
        model(project.assets().source(platform::utf8_to_utf16_validated(material_source.source_path)));
    const auto material_source_text = platform::utf16_to_utf8(L"Content/Materials/错误.ms.source");
    require(bool(material_source_text), "Invalid UTF-16 material Source fixture path.");
    material_source.source_path = *material_source_text;
    const std::filesystem::path material_path = L"Content/Materials/错误.ms.material";
    model(editor::write_file(model(project.resolve(platform::utf8_to_utf16_validated(material_source.source_path))),
                             source_file.source));
    const auto material_json = assets::material_format::encode(material);
    require(material_json.has_value(), "Material fixture failed to encode.");
    const auto material_bytes = json::write(material_json->root(), {true});
    model(editor::write_file(model(project.resolve(material_path)), {material_bytes.data(), material_bytes.size()}));
    model(project.refresh());
    const auto entry = std::ranges::find(project.entries(), material_path, &assets::ProjectEntry::path);
    require(entry != project.entries().end(), "The Unicode material fixture was not published by the Reader.");
    const auto material_key = entry->key;
    editor::Session authored;
    model(authored.initialize());
    model(authored.create_static_mesh(model(project.assets().mesh("builtin/cube"))));
    const auto parts = model(world::components(authored.world().runtime(), authored.selection().entity));
    require(parts.size() == 1, "Error fixture did not create the builtin cube's mesh component.");
    model(authored.assign_material(project, parts.front().id(), "slot/0", material_key));
    model(editor::write_file(model(project.resolve(project.startup_scene())), model(authored.serialize())));
    const auto author = model(project.assets().material(material_key));
    const auto source_path = std::get<assets::MaterialSourceAsset>(author.author).source_path;
    std::filesystem::remove(model(project.resolve(platform::utf8_to_utf16_validated(source_path))));
    const auto unreadable = project.assets().source(platform::utf8_to_utf16_validated(source_path));
    require(!unreadable, "Deleted material Source remained readable.");
    Access::mark_saved(app); // The earlier UI fixture's cube is not part of this document-load case.
    model(app.open_project(project.root()));
    flush();
    Access::output_layout(app);
    tick(app, 2);
    click(app, controls::clear_filters);
    click(app, controls::search);
    type(app, L"错误.ms.source");
    key(app, VK_RETURN);
    wait([&] { return find("world.unreadable_resource") != 0; },
         "Material load error did not reach Project Output through the formal Reader.");
    const auto error_key = find("world.unreadable_resource");
    const auto& captured = output.record(error_key)->entry;
    const auto error_code = std::ranges::find(captured.fields, "error_code", &logging::Field::key);
    require(captured.message == unreadable.error().message && error_code != captured.fields.end() &&
                std::get<std::uint64_t>(error_code->value) == static_cast<std::uint64_t>(unreadable.error().code),
            "Material load error was reduced to a warning label or had its original code/body replaced.");
    select_row(error_key);
    require(output.body()->text() == platform::utf8_to_utf16_validated(unreadable.error().message) &&
                !app.document().dirty(),
            "Output changed the material error body or tolerant loading dirtied the author document.");
    capture(app, evidence / "output-material-error.png");
}
