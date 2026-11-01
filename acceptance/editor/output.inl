void verify_output_projection() {
    editor::Output output;
    logging::Record first;
    first.entry = {.severity = logging::Severity::warning,
                   .category = "acceptance.output",
                   .event_code = "output.unicode",
                   .message = "完整正文\nline"};
    first.entry.message.push_back('\0');
    first.entry.message += "tail";
    require(output.accept_owned(first), "Standalone Output rejected an owned record without a logging host.");
    require(output.records().size() == 1 &&
                output.records().front().record.persistence == logging::Persistence::not_persisted,
            "Standalone Output silently discarded or claimed persistence for a user message.");
    const auto& saved = output.records().front().record;
    require(!saved.run_id.empty() && saved.producer_id && saved.producer_sequence,
            "Standalone records have no distinct stream identity.");
    require(output.select(output.rows().front().key) && output.body()->text() == L"完整正文\nline\\u0000tail",
            "Output did not retain the complete escaped multiline body.");
    require(output.accept_owned(first) && output.rows().size() == 2,
            "Equal messages without Repeat were incorrectly coalesced.");
    first.entry.repeat = logging::Repeat{"explicit-diagnostic", 1};
    require(output.accept_owned(first) && output.accept_owned(first) && output.rows().size() == 3 &&
                output.rows().back().count == 2,
            "Explicit repeat increments did not share exactly one projected row.");
    const auto duplicate = output.records().back().record;
    require(!output.accept_owned(duplicate) && output.rows().back().count == 2,
            "A duplicate record incremented its repeat twice.");
    output.filter(logging::Severity::error);
    require(output.rows().empty() && !output.health_text().empty(),
            "Filtering removed health or retained a lower severity.");
    output.filter(logging::Severity::trace, L"acceptance", L"tail");
    require(output.rows().size() == 3, "Category/text filtering searched a truncated preview instead of the record.");

    editor::Output messages;
    require(messages.submit_owned({.category = "acceptance.output",
                                   .event_code = "output.message",
                                   .message = "No host user message"})
                        .status == logging::SubmitStatus::no_active_run &&
                messages.records().size() == 1,
            "A standalone submit silently discarded its user message.");
    logging::Record producer;
    producer.run_id = "owned-producers";
    producer.process_id = 1;
    producer.producer_sequence = 1;
    producer.entry = {
        .category = "acceptance.output", .event_code = "output.producer", .message = "Independent producer"};
    for (std::uint64_t i = 1; i <= 129; ++i) {
        producer.producer_id = i;
        require(messages.accept_owned(producer),
                "Output incorrectly treated the active producer budget as a lifetime identity quota.");
    }
    require(!messages.accept_owned(producer) && messages.records().size() == 130,
            "A supplied identity was projected twice.");
    ++producer.producer_id;
    producer.entry.message = std::string(1023, 'x') + "😀tail";
    require(messages.accept_owned(producer) &&
                messages.table()->rows.back().cells[2].text == std::wstring(256, L'x') + L"…",
            "A bounded row preview split UTF-8 or changed the visible prefix.");
    messages.filter(logging::Severity::trace, {}, L"😀tail");
    require(messages.rows().size() == 1 && messages.select(messages.rows().front().key) &&
                messages.body()->text().ends_with(L"😀tail"),
            "Bounded previews truncated the searchable or selected full body.");

    editor::Output bounded;
    logging::Record payload;
    payload.entry = {.category = "acceptance.output", .event_code = "output.capacity", .message = "x"};
    for (std::size_t i = 0; i < editor::Output::maximum_records + 1; ++i)
        require(bounded.accept_owned(payload), "Output rejected a bounded record.");
    require(bounded.records().size() == 4096 && bounded.statistics().evicted_records == 1 &&
                bounded.statistics().evicted_body_bytes == 1,
            "Output did not enforce its record-count cache budget transparently.");
    editor::Output bytes;
    payload.entry.message.assign(logging::maximum_message_bytes, '\1');
    for (unsigned i = 0; i < 65; ++i)
        require(bytes.accept_owned(payload), "Output rejected a 128 KiB body.");
    require(bytes.records().size() == 64 && bytes.statistics().body_bytes == 8 * 1024 * 1024 &&
                bytes.statistics().evicted_records == 1,
            "Output did not enforce its body-byte cache budget independently of row count.");
    require(bytes.select(bytes.rows().back().key) &&
                bytes.body()->text().size() == 6 * logging::maximum_message_bytes &&
                bytes.body()->text().ends_with(L"\\u0001"),
            "Worst-case control escaping truncated the complete body or exceeded CodeDocument's contract.");
    payload.entry.message.push_back('x');
    require(!bytes.accept_owned(payload) && bytes.statistics().invalid_owned == 1,
            "Output silently truncated an oversized injected body.");

    const auto folder = std::filesystem::temp_directory_path() / ("atelier-output-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(folder);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    editor::Output raw;
    require(raw.add_run(folder / "raw").has_value(), "Raw capture could not attach its source.");
    raw.capture_raw(folder / "raw", false, "prefix\xe4", 0, false);
    raw.capture_raw(folder / "raw", true, "failure without newline", 0, true);
    raw.capture_raw(folder / "raw", false, std::string_view{"\xb8\xad\0\\\xff", 5}, 7, true);
    require(raw.records().size() == 3 && raw.records()[0].record.entry.message == "prefix" &&
                raw.records()[1].record.entry.event_code == "raw.stderr" &&
                raw.records()[1].record.entry.message == "failure without newline" &&
                raw.records()[2].record.entry.message == std::string("中\0", 4) + "\\\\\\xff",
            "Raw projection lost split UTF-8, NUL, invalid bytes, stream identity or unterminated tail.");
    raw.capture_raw(folder / "raw", false, "\xe4", 12, false);
    raw.capture_raw(folder / "raw", false, "\xb8", 20, true);
    require(raw.records()[3].record.entry.message == "\\xe4" && raw.records()[4].record.entry.message == "\\xb8",
            "A raw retention gap spliced unrelated UTF-8 fragments.");
    const auto path = folder / "original.source";
    const std::string source_text = "// original\n中文x\n";
    take(editor::write_file(path, source_text));
    editor::Session scene;
    editor::Workspace workspace;
    first.entry.source = {.file = "original.source",
                          .root = assets::utf8(folder.wstring()),
                          .line = 2,
                          .column = 7,
                          .version = source::content_hash(source_text),
                          .disk_path = assets::utf8(path.generic_wstring())};
    auto invalid_source = first;
    invalid_source.entry.source.root = "relative";
    require(!workspace.output.accept_owned(invalid_source), "Standalone Output bypassed the shared source schema.");
    require(workspace.output.accept_owned(first), "Workspace lost a standalone record.");
    logging::Record independent;
    independent.entry = {
        .category = "acceptance.output", .event_code = "output.independent", .message = "Independent row"};
    require(workspace.output.accept_owned(independent), "Workspace lost its second independent row.");
    require(workspace.output.select(workspace.output.rows().front().key), "Workspace record could not be selected.");
    take(workspace.locate_output());
    auto* file = workspace.source_editor.active();
    require(file && file->document->edit().current().caret.offset == 14,
            "Log source UTF-8 byte column was not converted to UTF-16.");
    file->document->edit().replace(L"draft");
    const auto draft = file->document->text();
    require(!workspace.locate_output() && file->document->text() == draft,
            "Output navigation overwrote an unsaved source draft.");
    file->document->edit().history(false);
    take(editor::write_file(path, source_text + "// newer"));
    require(!workspace.locate_output(), "Output navigated a stale Source version.");
    take(editor::write_file(path, source_text));
    auto relative = first.entry.source;
    relative.root.clear();
    relative.disk_path.clear();
    require(!workspace.source_editor.locate(relative), "A virtual Source version was mistaken for a saved-file hash.");
    workspace.dock.select(editor::control::output_tab);
    workspace.dock.maximize(editor::control::output_tab);
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring(),
                           (editor::asset_directory() / "fonts/SourceCodePro-Regular.otf").wstring()};
    ui::Session surface(fonts);
    require(surface.set_surface({{1000, 720}, 1, 1}).has_value(), "Output surface failed.");
    const auto frame = [&] {
        const auto controls = workspace.describe(scene, {1000, 720}, surface.presented().get());
        require(surface.set_controls(controls).has_value(), "Output DSL controls failed.");
        auto prepared = surface.prepare();
        require(prepared && surface.accept(*prepared), "Output DSL frame failed.");
        workspace.commit_ui_controls();
    };
    frame();
    require(surface.presented()->find_table(editor::output_control::table)->data->rows.size() == 2,
            "A multi-row Output table failed its public UI contract.");
    require(take(workspace.apply_ui(
                {ui::ActionKind::table_column_resize, editor::output_control::table, ui::TableColumnResize{3, 420}})),
            "Output did not consume its column resize action.");
    frame();
    require(surface.presented()->find_table(editor::output_control::table)->data->columns[2].width == 420,
            "Output discarded its committed column width.");
    const auto* body = surface.presented()->find(editor::output_control::body);
    require(body && body->read_only && body->code->text() == L"完整正文\nline\\u0000tail",
            "Real DSL binding did not publish the complete read-only body.");
    surface.focus(editor::output_control::body);
    require(surface.input({.kind = ui::EventKind::key_down, .key = ui::Key::delete_key}).consumed &&
                !surface.text_input() && body->code->text() == L"完整正文\nline\\u0000tail",
            "Real Output body allowed mutation or native IME.");
    workspace.output.filter(logging::Severity::critical);
    frame();
    require(workspace.output.rows().empty() && surface.presented()->find(editor::output_control::health),
            "Filtered empty Output hid the loss/health strip.");
    std::puts(
        "Editor Output projection: PASS (owned records, explicit repeats, both budgets, full body, source identity, real DSL/read-only)");

    // One real Quill run. No replacement sink and no synchronous UI callback.
    const auto directory = folder / "run";
    auto started = logging::start_run({.directory = directory, .process_name = "output-acceptance"});
    require(started.has_value(), "Output acceptance could not start its real logging host.");
    auto run = std::move(*started);
    editor::Output hosted;
    require(hosted.add_run(directory).has_value() && hosted.add_run(directory).has_value(),
            "Output could not idempotently attach the same run.");
    require(!hosted.accept_owned(first), "A hosted Output accepted a parallel direct projection.");
    require(hosted.submit_owned({.severity = logging::Severity::trace,
                                 .category = "acceptance.output",
                                 .event_code = "output.filtered",
                                 .message = "hidden"})
                        .status == logging::SubmitStatus::filtered &&
                hosted.records().empty(),
            "A filtered entry was reintroduced as an unpersisted rejection.");
    const logging::Entry complete{
        .category = "acceptance.output", .event_code = "output.disk", .message = std::string(128 * 1024, '\1')};
    require(hosted.submit_owned(complete).status == logging::SubmitStatus::accepted && hosted.records().empty(),
            "An accepted submission bypassed JSONL or was not accepted.");
    auto flushed = run->flush();
    require(flushed.backend_acknowledged && flushed.sink_flush_succeeded, "Output real logging flush failed.");
    const Error cause{
        ErrorCode::assignment_failed, "完整业务原因\n" + std::string(2048, 'x') + "\n末尾",
        SourceLocation{"original.source", 18446744073709551610ULL, 123, 7, 13, assets::utf8(path.generic_wstring())}};
    for (unsigned i = 0; i < 300; ++i) {
        const auto receipt = hosted.submit_owned(
            i == 0 ? application::error_diagnostic(cause, "editor.operation_failed", folder)
                   : logging::Entry{
                         .category = "acceptance.output", .event_code = "output.budget", .message = std::to_string(i)});
        require(receipt.status == logging::SubmitStatus::accepted,
                "Small record fixture overflowed its producer queue.");
    }
    flushed = run->finish();
    require(flushed.backend_acknowledged && flushed.sink_flush_succeeded && flushed.summary_published,
            "Output real logging finish failed.");
    const auto deadline = GetTickCount64() + 10000;
    do {
        const auto count = hosted.records().size();
        const auto read = hosted.source_status(directory)->bytes_read;
        hosted.poll();
        require(hosted.records().size() - count <= 256 &&
                    hosted.source_status(directory)->bytes_read - read <= 256 * 1024,
                "Output multiplied its per-poll record or byte budget.");
        if (hosted.source_status(directory)->finished)
            break;
    } while (GetTickCount64() < deadline);
    require(hosted.source_status(directory)->finished && hosted.source_status(directory)->has_final_summary &&
                hosted.records().size() == 301,
            "Output did not drain all complete disk records once or recognize normal final evidence.");
    require(hosted.records().front().record.entry.message == complete.message &&
                hosted.records().front().record.persistence == logging::Persistence::jsonl,
            "Incremental JSONL changed the complete body or its persistence classification.");
    const auto& captured = hosted.records()[1].record.entry;
    require(captured.message == cause.message && captured.source.file == "original.source" &&
                captured.source.version == 18446744073709551610ULL && captured.source.offset == 123 &&
                captured.source.line == 7 && captured.source.column == 13 &&
                captured.source.disk_path == cause.source->disk_path,
            "Project Output lost the complete Error body or one of its six source fields through JSONL.");
    const auto code = std::ranges::find(captured.fields, "error_code", &logging::Field::key);
    require(code != captured.fields.end() &&
                std::get<std::uint64_t>(code->value) == static_cast<std::uint64_t>(ErrorCode::assignment_failed) &&
                hosted.select(hosted.records()[1].key) && hosted.body()->text() == assets::wide(cause.message),
            "Project Output changed the original Error code or selected full body.");
    hosted.source_ended(directory); // Also valid after the completed Reader retires.
    const auto before = hosted.records().size();
    hosted.poll();
    require(!hosted.poll() && hosted.records().size() == before,
            "Idle/retired sources replayed records or dirtied the projection.");
    // submit's explicit no_active_run receipt is known rejection, not uncertainty.
    require(hosted.submit_owned({.category = "acceptance.output",
                                 .event_code = "output.rejected",
                                 .message = "Visible after rejection"})
                        .status == logging::SubmitStatus::no_active_run &&
                hosted.records().back().record.persistence == logging::Persistence::not_persisted &&
                hosted.records().back().record.entry.message == "Visible after rejection",
            "A known rejected user message disappeared instead of remaining visibly unpersisted.");
    run.reset(); // Release the completed run's activity lock before archive edits.
    std::filesystem::remove(directory / "health.json");
    {
        std::ofstream tail(directory / "events.jsonl", std::ios::binary | std::ios::app);
        tail << "{\"partial\":";
        require(bool(tail), "Could not construct the real terminated-file tail.");
    }
    editor::Output ended;
    require(ended.add_run(directory).has_value(), "Archive reader could not attach.");
    ended.source_ended(directory);
    const auto ended_deadline = GetTickCount64() + 10000;
    do {
        ended.poll();
        if (ended.source_status(directory)->finished)
            break;
    } while (GetTickCount64() < ended_deadline);
    const auto& state = *ended.source_status(directory);
    require(state.finished && !state.has_final_summary && state.incomplete && state.incomplete_lines == 1 &&
                ended.records().size() == 301,
            "An exited source waited forever for final evidence or silently accepted an incomplete tail.");
    std::puts(
        "Editor Output JSONL: PASS (real Quill/files, global poll budget, no replay, rejected message, ended incomplete source)");
}
