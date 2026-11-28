#include "application/project_runner.hpp"
#include "assets/file.hpp"
#include "application/play_link.hpp"
#include "application/render_clock.hpp"
#include "application/lit_preparation.hpp"
#include "application/window.hpp"
#include "application/presentation.hpp"
#include "application/scene_snapshot.hpp"
#include "application/diagnostics.hpp"
#include "application/timing.hpp"
#include "editor/performance.hpp"
#include "editor/project_code.hpp"
#include "editor/project_ui.hpp"
#include "renderer/renderer.hpp"
#include "foundation/optional.hpp"
#include "foundation/result.hpp"
#include "foundation/scope_exit.hpp"
#include "logging/native.hpp"
#include "platform/utf.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>

namespace atelier::application {
namespace {
struct RunOptions {
    std::filesystem::path project;

    std::filesystem::path scene_snapshot;

    std::filesystem::path report;

    HANDLE stop_event{}; // Inherited event; this process closes its own handle.

    HANDLE stop_ack_event{}; // Inherited acknowledgement; signalled before document.stop, closed with stop_event.

    bool headless{};

    bool preview_ui{};

    // Stops the World at the first object failure (acceptance and CI) instead of isolating it.
    bool halt{};

    std::uint64_t frames{};

    Optional<PlayEnd> channel;
};

Result<void> complete(Result<world::Commit> result) {
    ATELIER_TRY(result);
    if (result->notification_error)
        return std::unexpected(std::move(*result->notification_error));
    return {};
}

Result<std::uint64_t> number(std::wstring_view text) {
    std::uint64_t value{};
    if (text.empty())
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Expected an unsigned command argument."});
    for (const auto ch : text) {
        if (ch < L'0' || ch > L'9' || value > (UINT64_MAX - static_cast<unsigned>(ch - L'0')) / 10)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid unsigned command argument."});
        value = value * 10 + static_cast<unsigned>(ch - L'0');
    }
    return value;
}

void report_performance(const editor::FramePerformance& performance, std::uint64_t frames) noexcept {
    const auto& cpu = performance.snapshot().cpu;
    logging::Entry entry{
        .category = "runner"_s, .event_code = "runner.performance"_s, .message = "Run frame performance."_s};
    entry.fields = {{"frames", frames},
                    {"samples", std::uint64_t{cpu.samples}},
                    {"structure_errors", performance.structure_errors()}};
    if (cpu.samples) {
        const auto& frame = *cpu.phases[timing::frame.index()];
        entry.fields.append({{"frame_mean_ms", frame.mean},
                             {"frame_p95_ms", frame.p95},
                             {"frame_max_ms", frame.max},
                             {"unattributed_mean_ms", cpu.unattributed->mean}});
    }
    static_cast<void>(logging::submit(std::move(entry)));
    for (const auto* marker : timing::catalog())
        if (const auto& phase = cpu.phases[marker->index()]; phase && marker->parent() == &timing::frame) {
            const std::array fields{logging::Field{"phase", String::from_utf8_validated(marker->key())},
                                    logging::Field{"mean_ms", phase->mean}};
            static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "runner", "runner.performance_phase",
                                               logging::native::Context{fields}, "Run phase {} mean {} ms",
                                               marker->key(), phase->mean));
        }
}

void startup_checkpoint() noexcept {
    // One startup boundary, not a per-frame barrier. The record must be visible
    // even if the very next Source invocation never returns.
    if (auto* run = logging::current_run())
        static_cast<void>(run->flush());
}

Result<RunOptions> parse_options(Span<wchar_t*> args) {
    if (args.empty() || !*args.front())
        return std::unexpected(Error{ErrorCode::argument_mismatch, "--run-project requires a project folder."});
    RunOptions options;
    options.project = args.front();
    args = args.slice(1);
    const auto argument = [&]() -> Result<std::wstring_view> {
        if (args.empty())
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Missing run option value."});
        const std::wstring_view value = args.front();
        args = args.slice(1);
        return value;
    };
    const auto handle = [&]() -> Result<HANDLE> {
        ATELIER_TRY_BIND(text, argument());
        ATELIER_TRY_BIND(value, number(*text));
        const auto inherited = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(*value));
        DWORD flags{};
        if (!GetHandleInformation(inherited, &flags))
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid inherited Play handle."});
        return inherited;
    };
    HANDLE live_read{};
    HANDLE live_write{};
    while (!args.empty()) {
        const std::wstring_view option = args.front();
        args = args.slice(1);
        if (option == L"--headless")
            options.headless = true;
        else if (option == L"--preview-ui")
            options.preview_ui = true;
        else if (option == L"--halt-on-fault")
            options.halt = true;
        else if (option == L"--scene-snapshot" || option == L"--report") {
            ATELIER_TRY_BIND(value, argument());
            (option == L"--report" ? options.report : options.scene_snapshot) = *value;
        } else if (option == L"--frames") {
            ATELIER_TRY_BIND(value, argument());
            ATELIER_TRY_BIND(frames, number(*value));
            options.frames = *frames;
        } else if (option == L"--stop-event" || option == L"--stop-ack-event") {
            ATELIER_TRY_BIND(event, handle());
            (option == L"--stop-event" ? options.stop_event : options.stop_ack_event) = *event;
        } else if (option == L"--live-channel") {
            ATELIER_TRY_BIND(read, handle());
            ATELIER_TRY_BIND(write, handle());
            live_read = *read;
            live_write = *write;
        } else
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Unknown project run option."});
    }
    const std::array handles{live_read, live_write, options.stop_event, options.stop_ack_event};
    for (auto it = handles.begin(); it != handles.end(); ++it)
        if (*it && std::find(handles.begin(), it, *it) != it)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Inherited Play handles must be distinct."});
    if (live_read && options.preview_ui)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "UI preview does not accept scene changes."});
    if (live_read)
        options.channel.emplace(std::make_unique<LiveLink>(live_read, live_write));
    return options;
}

void report_failure(const Error& error, StringView event, const std::filesystem::path& project) {
    auto entry = error_diagnostic(error, event, project);
    if (entry) {
        entry->category = "runner"_s;
        if (logging::submit(std::move(*entry)).status == logging::SubmitStatus::accepted)
            return;
    }
    std::fprintf(stderr, "Project run: %s\n", error.message.c_str());
}

Result<void> run_project(RunOptions& options) {
    ScopeExit stop_event{[&]() noexcept {
        if (options.stop_event)
            CloseHandle(options.stop_event);
    }};
    ScopeExit stop_ack_event{[&]() noexcept {
        if (options.stop_ack_event)
            CloseHandle(options.stop_ack_event);
    }};
    require_frame_timing();
    ATELIER_TRY_BIND(opened, editor::Project::open(options.project));
    auto& project = *opened;
    auto root = platform::utf16_to_utf8(project.root().generic_wstring());
    auto native_root = platform::utf16_to_utf8(project.root().native());
    if (!root || !native_root)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 project path."});
    editor::ProjectDocument loaded;
    if (options.preview_ui) {
        if (project.ui_source().empty())
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Configure a project UI source before previewing."});
        loaded.session = std::make_unique<editor::Session>();
        ATELIER_TRY(loaded.session->initialize());
    } else {
        ATELIER_TRY_BIND(scene, options.scene_snapshot.empty() ? project.resolve(project.startup_scene())
                                                               : Result<std::filesystem::path>{options.scene_snapshot});
        ATELIER_TRY_BIND(bytes, assets::read_file(*scene));
        loaded = editor::load_project_scene(project, as_chars(*bytes));
    }
    if (!loaded.session)
        return std::unexpected(std::move(*loaded.error));
    auto& document = *loaded.session;
    if (options.halt)
        ATELIER_TRY(document.set_failure_policy(world::FailurePolicy::halt));
    // The editor owns document identities; this copy addresses objects it spawns by runtime handle.
    if (!options.preview_ui)
        document.enter_play();
    report_load(document, project.root());
    // Isolated failures keep Play running; under halt the failing call is reported before it stops the run.
    std::uint64_t dropped{};
    const auto report_diagnostics = [&] { drain_world(document, project.root(), dropped); };
    ScopeExit stop_world{[&]() noexcept {
        if (!document.world().running())
            return;
        auto stopped = complete(document.stop());
        drain_world(document, project.root(), dropped);
        if (!stopped)
            report_failure(stopped.error(), "runner.cleanup_failed", project.root());
    }};
    editor::ProjectUI project_ui;
    ATELIER_TRY(project_ui.initialize());
    project_ui.sync(&project);
    ui::Session ui;
    ATELIER_TRY_BIND(fonts, editor_fonts());
    ATELIER_TRY(ui.initialize(*fonts).transform_error(ui_error));
    auto style = ui.style();
    style.font.family = L"Sarasa UI SC";
    style.font.size = 12.5f;
    style.caption_scale = 1;
    ATELIER_TRY(ui.set_style(style).transform_error(ui_error));
    ATELIER_TRY_BIND(styles, editor_styles());
    ATELIER_TRY(ui.set_style_sheet(*styles).transform_error(ui_error));
    std::unique_ptr<Window> window;
    Optional<renderer::Renderer> renderer;
    if (!options.headless) {
        window = std::make_unique<Window>();
        ATELIER_TRY(window->initialize());
        SetWindowTextW(window->handle(),
                       (project.name() + (options.preview_ui ? L" — UI Preview" : L" — Running")).c_str());
        renderer::Options render_options;
        render_options.shader_cache = shader_cache_directory();
        ATELIER_TRY_BIND(device,
                         renderer::Renderer::open(window->handle(), render_options).transform_error(renderer_error));
        renderer.emplace(std::move(*device));
        window->show();
    }
    ScopeExit close_renderer{[&]() noexcept {
        if (renderer)
            static_cast<void>(renderer->close());
    }};
    const auto effects = [&](this auto&& self, const ui::Reply& reply) -> void {
        if (reply.error)
            report_failure(ui_error(*reply.error), "runner.ui_input_failed", project.root());
        for (const auto& effect : reply.effects) {
            if (const auto* action = std::get_if<ui::Action>(&effect)) {
                if (reply.error && action->kind != ui::ActionKind::edit_cancel &&
                    action->kind != ui::ActionKind::dismiss && action->kind != ui::ActionKind::drag_cancel)
                    continue;
                const auto handled = project_ui.event(*action);
                if (!handled)
                    static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.ui_action_failed",
                                                  "Project UI: {}", handled.error().message));
            } else if (window)
                std::visit(
                    [&](const auto& request) {
                        using T = std::decay_t<decltype(request)>;
                        if constexpr (std::same_as<T, ui::ClipboardRequest>) {
                            if (reply.error)
                                return;
                            self(ui.input(
                                {.kind = ui::EventKind::clipboard_result, .data = window->clipboard(request)}));
                        } else {
                            if (reply.error && request.kind != ui::ImeKind::cancel)
                                return;
                            self(ui.input({.kind = ui::EventKind::ime_result, .data = window->ime(request)}));
                        }
                    },
                    std::get<ui::PlatformRequest>(effect));
        }
        if (window) {
            if (reply.capture != ui::Capture::unchanged)
                window->capture(reply.capture == ui::Capture::acquire);
            if (reply.cursor)
                window->cursor(*reply.cursor);
        }
    };
    if (project_ui.configured()) {
        static_cast<void>(
            ATELIER_LOG(logging::Severity::info, "runner", "runner.ui_evaluating", "Project UI evaluating"));
        startup_checkpoint();
    }
    SceneSnapshot scene;
    std::uint64_t frame{};
    std::uint64_t surface_revision{1};
    std::uint64_t ui_revision{};
    double elapsed{};
    bool started{};
    bool first_render{true};
    Optional<atelier::presentation::CameraSnapshot> accepted_camera;
    String camera_error;
    auto previous = std::chrono::steady_clock::now();
    String ui_error;
    RenderClock lighting_clock;
    LitFallback lit_fallback;
    String render_failure;
    String material_failure;
    const auto prepare_scene = [&]() -> Result<bool> {
        timing::Scope snapshot{phases::scene};
        ATELIER_TRY(document.sync_procedural_generators(&project));
        auto refreshed = scene.refresh(document, false);
        report_diagnostics();
        ATELIER_TRY(refreshed);
        const auto progress = scene.prepare(*renderer);
        if (!progress && progress.error().terminal)
            return std::unexpected(renderer_error(progress.error()));
        const auto failure = progress ? String{} : progress.error().cause.message;
        if (failure != material_failure) {
            material_failure = failure;
            if (!failure.empty())
                static_cast<void>(ATELIER_LOG(logging::Severity::error, "material", "material.failed",
                                              "Material preparation failed: {}", failure));
        }
        return progress && progress->ready;
    };
    timing::Recorder recorder;
    editor::FramePerformance performance;
    DWORD stop_error{};
    const std::function<bool()> stop_requested = [&] {
        if (!options.stop_event)
            return false;
        if (stop_error)
            return true;
        const auto stopped = WaitForSingleObject(options.stop_event, 0);
        if (stopped == WAIT_FAILED) {
            stop_error = GetLastError();
            return true;
        }
        return stopped == WAIT_OBJECT_0;
    };
    while (!options.frames || frame < options.frames) {
        if (stop_requested())
            break;
        // One frame per loop; the idle sleeps below stay outside it.
        timing::Recorder::Frame timed{recorder};
        Optional<std::uint64_t> sampled;
        if (window) {
            timing::Scope input{phases::input};
            window->poll([&](NativeEvent message) {
                if (message.message == WM_SIZE || message.message == WM_DPICHANGED) {
                    ++surface_revision;
                    effects(ui.cancel_interaction());
                    return;
                }
                if (message.message == WM_CANCELMODE || (message.message == WM_ACTIVATEAPP && !message.wparam)) {
                    effects(ui.cancel_interaction());
                    return;
                }
                auto input = input_event(std::move(message), window->handle());
                if (!input)
                    return;
                const bool pointer = input->kind == ui::EventKind::pointer_down ||
                                     input->kind == ui::EventKind::pointer_move || input->kind == ui::EventKind::wheel;
                if (pointer && (!ui.presented() || ui.presented()->drawing->surface_revision != surface_revision))
                    return;
                effects(ui.input(*input));
            });
        }
        if (window && window->closing()) {
            performance.observe(timed.finish(), {});
            break;
        }
        if (loaded.code) {
            timing::Scope polling{phases::project};
            static_cast<void>(loaded.code->poll(project));
        }
        renderer::Target target{{1280, 720}, true};
        if (renderer) {
            ATELIER_TRY_BIND(current, renderer->target().transform_error(renderer_error));
            target = *current;
        }
        const auto extent = target.drawable ? target.extent : ui::PixelExtent{};
        const auto now = std::chrono::steady_clock::now();
        const float dt = options.headless && options.frames
                             ? 1.f / 60
                             : std::clamp(std::chrono::duration<float>(now - previous).count(), 0.f, .1f);
        previous = now;
        if (started || target.drawable)
            elapsed += dt;
        auto preparation = [&]() -> Result<std::shared_ptr<const ui::UiFrame>> {
            timing::Scope describing{phases::ui};
            project_ui.sync(&project);
            ATELIER_TRY_BIND(members, document.world().members());
            return project_ui.prepare(ui, {extent, window ? window->dpi_scale() : 1.f, surface_revision}, elapsed,
                                      members->size(), effects);
        }();
        ATELIER_TRY(preparation);
        const auto& prepared = *preparation;
        if (options.channel) {
            timing::Scope linking{phases::control};
            options.channel->service(document, project);
        }
        // The first complete material group is prepared before Begin can
        // start simulation. Headless execution has no GPU preparation gate.
        bool can_start = prepared && !started;
        if (can_start && renderer) {
            ATELIER_TRY_BIND(ready, prepare_scene());
            can_start = *ready;
        }
        if (can_start) {
            if (!options.preview_ui) {
                auto begun = document.start();
                report_diagnostics();
                ATELIER_TRY(complete(std::move(begun)));
            }
            started = true;
            const std::array fields{logging::Field{"project_root", *root},
                                    logging::Field{"document", document.presentation().identity()},
                                    logging::Field{"preview_ui", options.preview_ui}};
            static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "runner", "runner.started",
                                               logging::native::Context{fields}, "Project started: {}", *native_root));
            startup_checkpoint();
            static_cast<void>(ATELIER_LOG(logging::Severity::info, "runner", "runner.startup_complete",
                                          "Startup checkpoint completed"));
        }
        if (started && !options.preview_ui) {
            auto updated = document.update(dt);
            report_diagnostics();
            ATELIER_TRY(updated);
        }
        {
            timing::Scope collecting{phases::gc};
            ATELIER_TRY(document.collect_step());
        }
        // Begin/update may have authored a different request. Freeze it
        // now; pending preparation leaves the previous complete group live.
        if (renderer && started)
            ATELIER_TRY(prepare_scene());
        else if (started && !options.preview_ui)
            ATELIER_TRY(document.sync_procedural_generators(&project));
        // Reference writes do not change World revision. Resolve after every update,
        // including its late flush; only value snapshots survive this boundary.
        Optional<atelier::presentation::CameraSnapshot> camera;
        renderer::Camera matrices;
        renderer::ViewSettings view;
        math::Vector3 focus{};
        String unavailable;
        if (!options.preview_ui) {
            timing::Scope snapshot{phases::scene};
            auto selected = document.presentation().main_camera();
            if (!selected)
                unavailable = selected.error().message;
            else {
                auto projected =
                    selected->matrices(static_cast<float>(std::max(extent.width, 1u)) / std::max(extent.height, 1u));
                auto environment = document.presentation().environment();
                if (!projected)
                    unavailable = projected.error().message;
                else if (!environment)
                    unavailable = environment.error().message;
                else {
                    focus = selected->position;
                    static_cast<rendering::SceneLightingSettings&>(view.lit) = *environment;
                    const auto* invalid = rendering::scene_lighting_focus_error(view.lit, focus);
                    if (!invalid)
                        invalid = renderer::probe_focus_error(view.lit, focus);
                    if (invalid)
                        unavailable = String::from_utf8_validated(invalid);
                    else {
                        camera = *selected;
                        matrices = {projected->view, projected->projection, projected->far_depth};
                        view.shading = renderer::Shading::scene_lit;
                    }
                }
            }
            report_diagnostics();
        }
        if (!camera) {
            accepted_camera.reset();
            lighting_clock.reset();
            focus = {};
            view = {};
        }
        if (unavailable != camera_error) {
            camera_error = std::move(unavailable);
            if (!camera_error.empty())
                static_cast<void>(ATELIER_LOG(logging::Severity::warning, "runner", "runner.camera_unavailable",
                                              "Main camera: {}", camera_error));
        }
        const bool camera_cut = camera && (!accepted_camera || camera->entity != accepted_camera->entity ||
                                           camera->vertical_fov != accepted_camera->vertical_fov ||
                                           camera->near_clip != accepted_camera->near_clip ||
                                           camera->far_clip != accepted_camera->far_clip);
        if (project_ui.revision() != ui_revision) {
            ui_revision = project_ui.revision();
            const std::array fields{logging::Field{"ui_revision", ui_revision}};
            static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "runner", "runner.ui_published",
                                               logging::native::Context{fields}, "Project UI published: {}",
                                               ui_revision));
        }
        if (project_ui.diagnostic() != ui_error) {
            ui_error = String::from_utf8_validated(project_ui.diagnostic());
            if (!ui_error.empty())
                static_cast<void>(
                    ATELIER_LOG(logging::Severity::error, "runner", "runner.ui_failed", "Project UI: {}", ui_error));
        }
        if (prepared && started) {
            if (renderer) {
                const auto& group = *scene.draw();
                renderer::Frame rendered{
                    *prepared->drawing,
                    matrices,
                    camera ? ui::Rect{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height)}
                           : ui::Rect{},
                    camera ? Span<const renderer::MeshInstance>{group.meshes}
                           : Span<const renderer::MeshInstance>{},
                    view,
                    {},
                    camera ? Span<const renderer::Light>{group.lights} : Span<const renderer::Light>{},
                    group.identity,
                    camera_cut,
                    rendering::sky_radiance(view.lit),
                    focus};
                rendered.materials = camera ? group.materials.get() : nullptr;
                rendered.stop_requested = &stop_requested;
                const auto lighting_start = RenderClock::Clock::now();
                const bool lighting_active = target.drawable && camera && !group.meshes.empty() && view.lit.ddgi;
                rendered.render_delta_seconds = lighting_clock.delta(lighting_start, lighting_active);
                if (first_render)
                    static_cast<void>(ATELIER_LOG(logging::Severity::info, "runner", "runner.first_render_begin",
                                                  "First render beginning"));
                ATELIER_TRY_BIND(drawn,
                                 lit_fallback.render(*renderer, rendered, nullptr).transform_error(renderer_error));
                if (first_render) {
                    static_cast<void>(ATELIER_LOG(logging::Severity::info, "runner", "runner.first_render_end",
                                                  "First render completed"));
                    first_render = false;
                }
                if (drawn->result.status == renderer::FrameStatus::cancelled) {
                    performance.observe(timed.finish(), {});
                    break;
                }
                if (drawn->failure != render_failure) {
                    render_failure = drawn->failure;
                    if (!render_failure.empty())
                        static_cast<void>(ATELIER_LOG(logging::Severity::error, "material", "material.shader_failed",
                                                      "Shader preparation failed: {}", render_failure));
                }
                const auto& result = drawn->result;
                if (result.status == renderer::FrameStatus::accepted) {
                    if (result.accepted_id != rendered.ui_frame.id ||
                        result.accepted_extent != rendered.ui_frame.extent)
                        return std::unexpected(
                            Error{ErrorCode::invalid_storage, "Project renderer accepted a mismatched frame."});
                    if (!ui.accept(prepared))
                        return std::unexpected(
                            Error{ErrorCode::invalid_storage, "Project UI refused the rendered frame."});
                    if (camera)
                        scene.accept(result.accepted_id, drawn->fallback, drawn->view.shading);
                    lighting_clock.accept(lighting_start,
                                          lighting_active && drawn->view.shading == renderer::Shading::scene_lit);
                    if (drawn->view.shading == renderer::Shading::scene_lit)
                        accepted_camera = camera;
                    sampled = result.accepted_id;
                } else
                    lighting_clock.reset();
            } else {
                if (!ui.accept(prepared))
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Project UI refused the headless frame."});
                sampled = prepared->drawing->id;
            }
        } else
            lighting_clock.reset();
        if (window)
            static_cast<void>(window->text_input(ui.text_input()));
        if (started)
            ++frame; // Startup compilation is not a simulated/rendered frame budget.
        performance.observe(timed.finish(), sampled);
        if (!started)
            Sleep(10);
        else if (!renderer && !options.frames)
            Sleep(4);
        else if (!target.drawable)
            Sleep(16);
    }
    if (stop_requested()) {
        if (stop_error)
            static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.stop_wait_failed",
                                          "WaitForSingleObject(stop event) failed: Windows {}", stop_error));
        else {
            static_cast<void>(ATELIER_LOG(logging::Severity::info, "runner", "runner.stop_observed",
                                          "Stop request observed after {} frames", frame));
            if (options.stop_ack_event && !SetEvent(options.stop_ack_event)) {
                const auto error = GetLastError();
                static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.stop_ack_failed",
                                              "SetEvent(stop acknowledgement) failed: Windows {}", error));
            }
        }
    }
    {
        static_cast<void>(
            ATELIER_LOG(logging::Severity::info, "runner", "runner.document_stop_begin", "Document stop beginning"));
        auto stopped = document.stop();
        static_cast<void>(
            ATELIER_LOG(logging::Severity::info, "runner", "runner.document_stop_end", "Document stop returned"));
        report_diagnostics();
        ATELIER_TRY(complete(std::move(stopped)));
    }
    if (!options.report.empty()) {
        ATELIER_TRY_BIND(scene_text, document.serialize());
        ATELIER_TRY(editor::write_file(options.report, *scene_text));
    }
    performance.publish();
    report_performance(performance, frame);
    const std::array fields{logging::Field{"frames", frame},
                            logging::Field{"document", document.presentation().identity()}};
    static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "runner", "runner.stopped",
                                       logging::native::Context{fields}, "Project stopped: {} frames", frame));
    return {};
}
}

int run_project_command(Span<wchar_t*> arguments) {
    auto options = parse_options(arguments);
    if (!options) {
        report_failure(options.error(), "runner.command_rejected", {});
        return 2;
    }
    if (auto run = run_project(*options); !run) {
        report_failure(run.error(), "runner.failed", options->project);
        return 1;
    }
    return 0;
}
}
