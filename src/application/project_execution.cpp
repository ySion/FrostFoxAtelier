#include "application/application.hpp"
#include "application/diagnostics.hpp"
#include "assets/file.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace atelier::application {
namespace {
auto failure(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(message)});
}
bool same_sources(const source::SourceSnapshot& a, const source::SourceSnapshot& b) {
    if (a.files.size() != b.files.size())
        return false;
    for (std::size_t i = 0; i < a.files.size(); ++i)
        if (a.files[i].path != b.files[i].path || a.files[i].source != b.files[i].source)
            return false;
    return true;
}
}
void Application::attach_play_output() noexcept try {
    play_log_ended_ = false;
    play_output_status_ = {};
    const auto attached = workspace_.output.add_run(play_.log_directory());
    if (!attached)
        notice("play.log_attachment_failed", assets::wide(attached.error().message), logging::Severity::warning);
    logging::Entry entry{.category = "play",
                         .event_code = "play.run_attached",
                         .message = "Runner process launched; waiting for its startup evidence."};
    entry.fields = {{"directory", assets::utf8(play_.log_directory().generic_wstring())},
                    {"project_root", assets::utf8(project_->root().generic_wstring())}};
    static_cast<void>(workspace_.output.submit_owned(std::move(entry)));
} catch (...) {
    std::fputs("Cannot attach the Runner output; the process launch result is unchanged.\n", stderr);
}
void Application::consume_play_output() noexcept try {
    if (play_.log_directory().empty())
        return;
    const auto raw = play_.take_raw_output();
    const auto capture = [&](bool standard_error, const PlayProcess::RawStream& stream,
                             const PlayProcess::StreamStatus& state) {
        workspace_.output.capture_raw(play_.log_directory(), standard_error, stream.bytes, stream.offset,
                                      state.eof || state.read_error || (!play_.running() && !raw.status.pending));
        auto& previous = play_output_status_[standard_error ? 1 : 0];
        if (previous == state)
            return;
        const bool loss = state.dropped_bytes > previous.dropped_bytes ||
                          state.abandoned_bytes > previous.abandoned_bytes || state.read_error != previous.read_error ||
                          state.tail_loss_unknown != previous.tail_loss_unknown;
        // Byte counts accompany data records. Publish transport transitions even
        // with no body, but not a second status record for every successful read.
        if (loss || state.eof != previous.eof) {
            logging::Entry entry{.severity = loss ? logging::Severity::warning : logging::Severity::info,
                                 .category = "process.raw",
                                 .event_code = "raw.transport",
                                 .message =
                                     loss ? "Raw process output is incomplete." : "Raw process stream reached EOF."};
            entry.fields = {{"directory", assets::utf8(play_.log_directory().generic_wstring())},
                            {"stream", std::string(standard_error ? "stderr" : "stdout")},
                            {"received_bytes", state.received_bytes},
                            {"dropped_bytes", state.dropped_bytes},
                            {"abandoned_bytes", state.abandoned_bytes},
                            {"read_error", std::uint64_t{state.read_error}},
                            {"eof", state.eof},
                            {"tail_loss_unknown", state.tail_loss_unknown}};
            static_cast<void>(workspace_.output.submit_owned(std::move(entry)));
        }
        previous = state;
    };
    capture(false, raw.standard_output, raw.status.standard_output);
    capture(true, raw.standard_error, raw.status.standard_error);
    if (play_log_ended_ || !raw.status.process_exited)
        return;
    play_log_ended_ = true;
    workspace_.output.source_ended(play_.log_directory());
    logging::Entry entry{.category = "play",
                         .event_code = "play.process_ended",
                         .message = "Runner process ended; retained JSONL tail remains readable."};
    entry.fields = {{"directory", assets::utf8(play_.log_directory().generic_wstring())}, {"forced", play_.forced()}};
    if (play_.exit_code())
        entry.fields.push_back({"exit_code", std::uint64_t{*play_.exit_code()}});
    static_cast<void>(workspace_.output.submit_owned(std::move(entry)));
} catch (...) {
    std::fputs("Cannot capture the Runner output; its exit result is unchanged.\n", stderr);
}
void Application::publish_build(std::shared_ptr<const editor::ProjectBuild> report) {
    if (!report || workspace_.project_build == report)
        return;
    workspace_.project_build = std::move(report);
    if (workspace_.project_build->error)
        workspace_.status = assets::wide(workspace_.project_build->error->message);
    else
        workspace_.status = L"Project build ready";
    // ProjectCode records the authoritative request/publication, not this UI projection.
}
Result<void> Application::apply_document_request(const editor::DocumentRequest& request) {
    using Kind = editor::DocumentRequest::Kind;
    close_menu(false);
    if (request.kind == Kind::open_scene)
        return open_scene(request.path, request.interactive);
    if (request.kind == Kind::new_project || request.kind == Kind::open_project)
        return open_project(request.path, request.kind == Kind::new_project, ProjectOpenPolicy::allow_repair, {},
                            request.interactive);
    if (request.rebuild) {
        if (play_.running())
            return failure("Stop the project before rebuilding its types.");
        if (request.interactive && (document_->undo_count() || document_->redo_count()) &&
            MessageBoxW(
                window(),
                L"Rebuild the scene with the new types?\n\nCurrent scene values and source documents will be kept. Scene Undo/Redo history will be cleared.",
                L"Rebuild project types", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
            cancel_operator();
            return {};
        }
        const auto edit = ui_.commit_edit();
        if (!edit || !effects(*edit))
            return failure("Finish the current edit before rebuilding types.");
    }
    for (const auto& file : workspace_.source_editor.files()) {
        const auto relative = file->path.lexically_relative(project_->root());
        if (file->dirty() && !relative.empty() && *relative.begin() == L"Source")
            if (auto saved = save_source(*file, request.interactive); !saved)
                return saved;
    }
    return build_project(request.rebuild);
}
Result<void> Application::build_project(bool rebuild) {
    if (!project_)
        return failure("Open a project folder first.");
    source_build_pending_ = false;
    if (project_code_ && !rebuild) {
        auto result = project_code_->poll(*project_, true);
        publish_build(project_code_->report());
        return result;
    }
    if (play_.running())
        return failure("Stop the project before rebuilding its types.");
    for (const auto& key : document_->dirty_materials()) {
        auto saved = document_->save_material(*project_, key);
        if (!saved)
            return saved;
    }
    auto text = workspace_.scene_available ? document_->serialize() : Result<std::string>{pending_scene_};
    if (!text)
        return std::unexpected(text.error());
    const bool dirty = workspace_.scene_available && document_->dirty();
    auto next =
        editor::load_project_scene(*project_, *text, editor::LoadPolicy::tolerant, false, editor::SessionRole::editor);
    if (!next.session) {
        auto report = std::make_shared<editor::ProjectBuild>();
        if (next.build)
            *report = *next.build;
        report->state = editor::BuildState::failed;
        report->error = next.error;
        publish_build(std::move(report));
        return std::unexpected(*next.error);
    }
    // Without a code owner the document holds placeholders from a broken build.
    // Replacing it drops scene history, which only an explicit Rebuild Types may do.
    if (!rebuild && (document_->undo_count() || document_->redo_count())) {
        publish_build(next.build);
        workspace_.status =
            L"Project build ready. Rebuild Types to restore placeholder objects; scene Undo/Redo history will be cleared.";
        notice("source.rebuild_required", workspace_.status);
        return {};
    }
    // A rebuild retains authored values, but its Runtime identities are new.
    // Preserve the unsaved marker independently of the retired history roots.
    if (dirty)
        next.session->mark_modified();
    workspace_.scene_available = true;
    pending_scene_ = *text;
    publish_document(std::move(next.session), next.code);
    publish_build(next.build);
    report_load(*document_, document_root_);
    return {};
}
Result<void> Application::create_source(const std::filesystem::path& relative, bool is_ui) {
    if (!project_)
        return failure("Open a project folder first.");
    const auto stem = assets::utf8(relative.stem().wstring());
    if (!is_ui && (stem.empty() ||
                   !((stem.front() >= 'A' && stem.front() <= 'Z') || (stem.front() >= 'a' && stem.front() <= 'z') ||
                     stem.front() == '_') ||
                   std::ranges::any_of(stem, [](unsigned char c) {
                       return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
                   })))
        return failure("Use a Source identifier as the component file name (letters, digits and underscore).");
    auto created = project_->create_source(relative, is_ui ? editor::ProjectCode::ui_source()
                                                           : editor::ProjectCode::component_source(stem));
    if (!created)
        return created;
    if (is_ui) {
        auto configured = project_->set_ui_source(relative);
        if (!configured) {
            notice("source.ui_configuration_failed",
                   L"UI file created; project entry was not changed: " + assets::wide(configured.error().message),
                   logging::Severity::error);
            return configured;
        }
        workspace_.project_ui_configured = true;
    }
    const auto path = project_->resolve(relative);
    if (!path)
        return std::unexpected(path.error());
    auto opened = open_source(*path);
    if (!opened)
        return opened;
    auto refreshed = project_->refresh();
    if (!refreshed)
        notice("source.browser_refresh_failed",
               L"Source created; browser refresh failed: " + assets::wide(refreshed.error().message),
               logging::Severity::warning);
    source_build_pending_ = true;
    workspace_.status = is_ui ? L"Project UI created"
                              : L"Component source created. Build, then rebuild types to add the new component.";
    return {};
}
Result<void> Application::run_project() {
    if (!project_ || !workspace_.scene_available)
        return failure("Build and load a project scene before running.");
    if (play_.running())
        return {};
    const auto edit = ui_.commit_edit();
    if (!edit || !effects(*edit))
        return failure("Finish the current field edit before running.");
    for (const auto& key : document_->dirty_materials()) {
        auto saved = document_->save_material(*project_, key);
        if (!saved)
            return saved;
    }
    auto built = build_project();
    if (!built)
        return built;
    const auto source = document_->serialize();
    if (!source)
        return std::unexpected(source.error());
    static std::atomic<std::uint64_t> serial{};
    const auto relative = std::filesystem::path("Cache/Play") /
                          (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                           std::to_wstring(++serial) + L".scene");
    const auto path = project_->resolve(relative);
    if (!path)
        return std::unexpected(path.error());
    auto saved = editor::write_file(*path, *source, editor::WriteMode::create);
    if (!saved)
        return saved;
    std::unique_ptr<LiveLink> link;
    consume_play_output(); // Consume the previous run before a successful start resets its buffers.
    auto started = play_.start(project_->root(), *path, false, 0, {}, false, false, &link);
    if (!started) {
        std::error_code ignored;
        std::filesystem::remove(*path, ignored);
        return started;
    }
    attach_play_output();
    // Forwarding starts with the snapshot just serialized, so no change falls between them;
    // the pipe holds batches until Play has loaded.
    auto spill = *path;
    spill.replace_extension();
    live_.open(std::move(link), std::move(spill));
    document_->set_live_forwarding(true);
    play_snapshot_ = *path;
    workspace_.project_running = true;
    workspace_.project_stopping = false;
    notice("play.launched", L"Running " + project_->name());
    return {};
}
Result<void> Application::preview_project_ui() {
    if (!project_ || project_->ui_source().empty())
        return failure("Create a project UI source before previewing.");
    if (play_.running())
        return failure("Stop the current run before previewing the project UI.");
    consume_play_output();
    auto started = play_.start(project_->root(), {}, false, 0, {}, true);
    if (!started)
        return started;
    attach_play_output();
    workspace_.project_running = true;
    workspace_.project_stopping = false;
    notice("play.preview_launched", L"Previewing " + project_->ui_source().generic_wstring());
    return {};
}
void Application::stop_play(bool restart) {
    close_menu(false);
    stop_project();
    restart_play_ = restart;
}
Result<void> Application::start_play(bool preview_ui, bool interactive) {
    close_menu(false);
    if (project_)
        for (const auto& file : workspace_.source_editor.files()) {
            const auto relative = file->path.lexically_relative(project_->root());
            if (file->dirty() && !relative.empty() && *relative.begin() == L"Source") {
                auto saved = save_source(*file, interactive);
                if (!saved)
                    return saved;
            }
        }
    if (preview_ui)
        return preview_project_ui();
    return this->run_project();
}
void Application::poll_project() {
    const bool was_running = play_.running();
    play_.poll();
    consume_play_output();
    workspace_.project_running = play_.running();
    workspace_.project_stopping = play_.stopping();
    live_.poll(play_.running(), play_.stopping());
    if (!live_.divergence().empty() && !workspace_.play_diverged) {
        workspace_.play_diverged = true;
        document_->set_live_forwarding(false);
        workspace_.status = L"Play no longer follows the scene: " + assets::wide(live_.divergence());
        notice("play.diverged", workspace_.status + L" Restart Play to continue.", logging::Severity::error);
    }
    if (was_running && !play_.running()) {
        workspace_.status = play_.forced()                       ? L"Project stopped: run did not respond to Stop"
                            : play_.exit_code().value_or(1) == 0 ? L"Project stopped"
                                                                 : L"Project exited with an error; see Project Output";
        notice("play.exited", workspace_.status,
               play_.exit_code().value_or(1) == 0 ? logging::Severity::info : logging::Severity::warning);
    }
    if (!play_.running() && live_.active()) {
        document_->set_live_forwarding(false);
        if (const auto error = live_.close())
            notice("play.cleanup_failed", L"Cannot remove the run's change files: " + assets::wide(error.message()),
                   logging::Severity::warning);
    }
    workspace_.play_pending = live_.active() ? std::optional(live_.unacknowledged()) : std::nullopt;
    workspace_.play_diverged = !live_.divergence().empty();
    if (!play_.running() && !play_snapshot_.empty()) {
        std::error_code error;
        std::filesystem::remove(play_snapshot_, error);
        if (error)
            notice("play.cleanup_failed", L"Cannot remove the run snapshot: " + assets::wide(error.message()),
                   logging::Severity::warning);
        play_snapshot_.clear();
    }
    if (restart_play_ && !play_.running()) {
        restart_play_ = false;
        if (auto restarted = run_project(); !restarted) {
            workspace_.status = L"Cannot restart Play: " + assets::wide(restarted.error().message);
            notice("play.restart_failed", workspace_.status, logging::Severity::error);
        } else
            workspace_.status = L"Play restarted";
    }
    if (!project_)
        return;
    if (project_code_) {
        static_cast<void>(project_code_->poll(*project_, source_build_pending_));
        source_build_pending_ = false;
        publish_build(project_code_->report());
    } else {
        const auto now = std::chrono::steady_clock::now();
        if (!source_build_pending_ && now < next_project_poll_)
            return;
        next_project_poll_ = now + std::chrono::milliseconds(500);
        auto snapshot = editor::ProjectCode::snapshot(*project_);
        auto ui_source = editor::ProjectCode::ui_snapshot(*project_);
        const auto& report = workspace_.project_build;
        if (source_build_pending_ ||
            (snapshot && ui_source &&
             (!report || !report->sources || !same_sources(*snapshot, *report->sources) ||
              (!*ui_source) != (!report->ui_source) ||
              (*ui_source && report->ui_source &&
               ((*ui_source)->path != report->ui_source->path || (*ui_source)->source != report->ui_source->source)))))
            static_cast<void>(build_project());
    }
}
void Application::forward_live() noexcept {
    if (!live_.active())
        return;
    try {
        auto ops = document_->take_live_ops();
        if (!ops)
            live_.fail(ops.error().message);
        else
            live_.forward(*ops);
    } catch (const std::exception& error) {
        live_.fail(error.what());
    }
}
}
