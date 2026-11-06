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
#include "logging/native.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <functional>

namespace atelier::application {
namespace {
template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

void complete(Result<world::Commit> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    if (result->notification_error)
        throw std::runtime_error(result->notification_error->message);
}

template <class T>
T graphics(renderer::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().cause.message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

template <class T>
T presentation(ui::Result<T> result) {
    if (!result)
        throw std::runtime_error("Project UI failed at control " + std::to_string(result.error().key) + ".");
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

std::uint64_t number(std::wstring_view text) {
    std::uint64_t value{};
    if (text.empty())
        throw std::runtime_error("Expected an unsigned command argument.");
    for (const auto ch : text) {
        if (ch < L'0' || ch > L'9' || value > (UINT64_MAX - static_cast<unsigned>(ch - L'0')) / 10)
            throw std::runtime_error("Invalid unsigned command argument.");
        value = value * 10 + static_cast<unsigned>(ch - L'0');
    }
    return value;
}

void report_performance(const editor::FramePerformance& performance, std::uint64_t frames) noexcept try {
    const auto& cpu = performance.snapshot().cpu;
    logging::Entry entry{.category = "runner", .event_code = "runner.performance", .message = "Run frame performance."};
    entry.fields = {{"frames", frames},
                    {"samples", std::uint64_t{cpu.samples}},
                    {"structure_errors", performance.structure_errors()}};
    if (cpu.samples) {
        const auto& frame = *cpu.phases[timing::frame.index()];
        entry.fields.insert(entry.fields.end(), {{"frame_mean_ms", frame.mean},
                                                 {"frame_p95_ms", frame.p95},
                                                 {"frame_max_ms", frame.max},
                                                 {"unattributed_mean_ms", cpu.unattributed->mean}});
    }
    static_cast<void>(logging::submit(std::move(entry)));
    for (const auto* marker : timing::catalog())
        if (const auto& phase = cpu.phases[marker->index()]; phase && marker->parent() == &timing::frame) {
            const std::array fields{logging::Field{"phase", std::string(marker->key())},
                                    logging::Field{"mean_ms", phase->mean}};
            static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "runner", "runner.performance_phase",
                                               logging::native::Context{fields}, "Run phase {} mean {} ms",
                                               marker->key(), phase->mean));
        }
} catch (...) {
    std::fputs("Runner performance diagnostic capture failed.\n", stderr);
}

void startup_checkpoint() noexcept {
    // One startup boundary, not a per-frame barrier. The record must be visible
    // even if the very next Source invocation never returns.
    try {
        if (auto* run = logging::current_run())
            static_cast<void>(run->flush());
    } catch (...) {
        std::fputs("Runner startup log checkpoint failed.\n", stderr);
    }
}
}

int run_project_command(std::span<wchar_t*> arguments) {
    try {
        if (arguments.empty())
            throw std::runtime_error("--run-project requires a project folder.");
        RunOptions options;
        options.project = arguments.front();
        HANDLE live_read{};
        HANDLE live_write{};
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            const std::wstring_view option = arguments[i];
            if (option == L"--headless") {
                options.headless = true;
                continue;
            }
            if (option == L"--preview-ui") {
                options.preview_ui = true;
                continue;
            }
            if (option == L"--halt-on-fault") {
                options.halt = true;
                continue;
            }
            if (++i == arguments.size())
                throw std::runtime_error("Missing run option value.");
            if (option == L"--live-channel") {
                live_read = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number(arguments[i])));
                if (++i == arguments.size())
                    throw std::runtime_error("Missing run option value.");
                live_write = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number(arguments[i])));
                continue;
            }
            if (option == L"--scene-snapshot")
                options.scene_snapshot = arguments[i];
            else if (option == L"--report")
                options.report = arguments[i];
            else if (option == L"--frames")
                options.frames = number(arguments[i]);
            else if (option == L"--stop-event")
                options.stop_event = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number(arguments[i])));
            else if (option == L"--stop-ack-event")
                options.stop_ack_event = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number(arguments[i])));
            else
                throw std::runtime_error("Unknown project run option.");
        }
        // Inherited from the editor: changes arrive on the first pipe, acknowledgements leave on the second.
        std::optional<PlayEnd> live;
        if (live_read || live_write) {
            DWORD flags{};
            if (!GetHandleInformation(live_read, &flags) || !GetHandleInformation(live_write, &flags))
                throw std::runtime_error("Invalid inherited Play change link.");
            live.emplace(std::make_unique<LiveLink>(live_read, live_write));
            options.channel = &*live;
        }
        return run_project(options);
    } catch (const std::exception& error) {
        static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.command_rejected",
                                      "Project command: {}", std::string_view(error.what())));
        return 2;
    }
}

int run_project(const RunOptions& options) {
    struct StopEvent {
        HANDLE value;

        ~StopEvent() {
            if (value)
                CloseHandle(value);
        }
    } stop_event{options.stop_event}, stop_ack_event{options.stop_ack_event};
    try {
        require_frame_timing();
        for (const auto event : {options.stop_event, options.stop_ack_event})
            if (event) {
                DWORD flags{};
                if (!GetHandleInformation(event, &flags))
                    throw std::runtime_error("Invalid inherited stop/ack event.");
            }
        auto project = take(editor::Project::open(options.project));
        editor::ProjectDocument loaded;
        if (options.preview_ui) {
            if (project.ui_source().empty())
                throw std::runtime_error("Configure a project UI source before previewing.");
            loaded.session = std::make_unique<editor::Session>();
        } else {
            const auto scene = options.scene_snapshot.empty() ? take(project.resolve(project.startup_scene()))
                                                              : options.scene_snapshot;
            loaded = editor::load_project_scene(project, take(assets::read_file(scene)));
        }
        if (!loaded.session)
            throw std::runtime_error(loaded.error ? loaded.error->message : "Project scene was not loaded.");
        auto& document = *loaded.session;
        if (options.halt)
            take(document.set_failure_policy(world::FailurePolicy::halt));
        // The editor owns document identities; this copy addresses objects it spawns by runtime handle.
        if (!options.preview_ui)
            document.enter_play();
        report_load(document, project.root());
        // Isolated failures keep Play running; under halt the failing call is reported before it stops the run.
        std::uint64_t dropped{};
        const Report report = [&](std::span<const world::Diagnostic> items) {
            report_world(document, items, project.root(), "live");
        };
        const auto report_diagnostics = [&] { drain_world(document, project.root(), dropped); };
        struct StopWorld {
            editor::Session& session;

            const std::filesystem::path& root;

            std::uint64_t& dropped;

            ~StopWorld() {
                try {
                    auto stopped = session.stop();
                    drain_world(session, root, dropped);
                    if (!stopped)
                        static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.cleanup_failed",
                                                      "Project stop: {}", stopped.error().message));
                    else if (stopped->notification_error)
                        static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.cleanup_failed",
                                                      "Project stop notification: {}",
                                                      stopped->notification_error->message));
                } catch (...) {
                    std::fputs("Runner World cleanup failed.\n", stderr);
                }
            }
        } stop_world{document, project.root(), dropped};
        editor::ProjectUI project_ui;
        project_ui.sync(&project);
        ui::Session ui(editor_fonts());
        auto style = ui.style();
        style.font.family = L"Sarasa UI SC";
        style.font.size = 12.5f;
        style.caption_scale = 1;
        presentation(ui.set_style(style));
        presentation(ui.set_style_sheet(editor_styles()));
        std::unique_ptr<Window> window;
        std::optional<renderer::Renderer> renderer;
        if (!options.headless) {
            window = std::make_unique<Window>();
            SetWindowTextW(window->handle(),
                           (project.name() + (options.preview_ui ? L" — UI Preview" : L" — Running")).c_str());
            renderer::Options render_options;
            render_options.shader_cache = shader_cache_directory();
            renderer.emplace(graphics(renderer::Renderer::open(window->handle(), render_options)));
            window->show();
        }
        struct CloseRenderer {
            std::optional<renderer::Renderer>& renderer;

            ~CloseRenderer() {
                if (renderer)
                    static_cast<void>(renderer->close());
            }
        } close_renderer{renderer};
        std::function<void(const ui::Reply&)> effects;
        effects = [&](const ui::Reply& reply) {
            for (const auto& effect : reply.effects) {
                if (const auto* action = std::get_if<ui::Action>(&effect)) {
                    const auto handled = project_ui.event(*action);
                    if (!handled)
                        static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.ui_action_failed",
                                                      "Project UI: {}", handled.error().message));
                } else if (window)
                    std::visit(
                        [&](const auto& request) {
                            using T = std::decay_t<decltype(request)>;
                            if constexpr (std::same_as<T, ui::ClipboardRequest>)
                                effects(ui.input(
                                    {.kind = ui::EventKind::clipboard_result, .data = window->clipboard(request)}));
                            else
                                effects(ui.input({.kind = ui::EventKind::ime_result, .data = window->ime(request)}));
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
        std::optional<atelier::presentation::CameraSnapshot> accepted_camera;
        std::string camera_error;
        auto previous = std::chrono::steady_clock::now();
        std::string ui_error;
        RenderClock lighting_clock;
        LitFallback lit_fallback;
        std::string render_failure;
        std::string material_failure;
        const auto prepare_scene = [&] {
            timing::Scope snapshot{phases::scene};
            auto refreshed = scene.refresh(document, false);
            report_diagnostics();
            take(std::move(refreshed));
            const auto progress = scene.prepare(*renderer);
            if (!progress && progress.error().terminal)
                throw std::runtime_error("Renderer stopped: " + progress.error().cause.message);
            const auto failure = progress ? std::string{} : progress.error().cause.message;
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
            std::optional<std::uint64_t> sampled;
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
                                         input->kind == ui::EventKind::pointer_move ||
                                         input->kind == ui::EventKind::wheel;
                    if (pointer && (!ui.presented() || ui.presented()->drawing->surface_revision != surface_revision))
                        return;
                    effects(ui.input(*input));
                });
            }
            if (window && window->closing()) {
                performance.observe(timed.finish(), std::nullopt);
                break;
            }
            if (loaded.code) {
                timing::Scope polling{phases::project};
                static_cast<void>(loaded.code->poll(project));
            }
            const auto target = renderer ? graphics(renderer->target()) : renderer::Target{{1280, 720}, true};
            const auto extent = target.drawable ? target.extent : ui::PixelExtent{};
            const auto now = std::chrono::steady_clock::now();
            const float dt = options.headless && options.frames
                                 ? 1.f / 60
                                 : std::clamp(std::chrono::duration<float>(now - previous).count(), 0.f, .1f);
            previous = now;
            if (started || target.drawable)
                elapsed += dt;
            auto prepared = [&] {
                timing::Scope describing{phases::ui};
                project_ui.sync(&project);
                return take(project_ui.prepare(ui, {extent, window ? window->dpi_scale() : 1.f, surface_revision},
                                               elapsed, take(document.world().members()).size(), effects));
            }();
            if (options.channel && !options.preview_ui) {
                timing::Scope linking{phases::control};
                options.channel->service(document, project, report);
            }
            // The first complete material group is prepared before Begin can
            // start simulation. Headless execution has no GPU preparation gate.
            const bool can_start = !renderer || started || (prepared && prepare_scene());
            if (prepared && !started && can_start) {
                if (!options.preview_ui) {
                    auto begun = document.start();
                    report_diagnostics();
                    complete(std::move(begun));
                }
                started = true;
                const std::array fields{logging::Field{"project_root", assets::utf8(project.root().generic_wstring())},
                                        logging::Field{"document", document.presentation().identity()},
                                        logging::Field{"preview_ui", options.preview_ui}};
                static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "runner", "runner.started",
                                                   logging::native::Context{fields}, "Project started: {}",
                                                   assets::utf8(project.root().wstring())));
                startup_checkpoint();
                static_cast<void>(ATELIER_LOG(logging::Severity::info, "runner", "runner.startup_complete",
                                              "Startup checkpoint completed"));
            }
            if (started && !options.preview_ui) {
                auto updated = document.update(dt);
                report_diagnostics();
                take(std::move(updated));
            }
            {
                timing::Scope collecting{phases::gc};
                take(document.collect_step());
            }
            // Begin/update may have authored a different request. Freeze it
            // now; pending preparation leaves the previous complete group live.
            if (renderer && started)
                static_cast<void>(prepare_scene());
            // Reference writes do not change World revision. Resolve after every update,
            // including its late flush; only value snapshots survive this boundary.
            std::optional<atelier::presentation::CameraSnapshot> camera;
            renderer::Camera matrices;
            renderer::ViewSettings view;
            std::array<float, 3> focus{};
            std::string unavailable;
            if (!options.preview_ui) {
                timing::Scope snapshot{phases::scene};
                auto selected = document.presentation().main_camera();
                if (!selected)
                    unavailable = selected.error().message;
                else {
                    auto projected = selected->matrices(static_cast<float>(std::max(extent.width, 1u)) /
                                                        std::max(extent.height, 1u));
                    auto environment = document.presentation().environment();
                    if (!projected)
                        unavailable = projected.error().message;
                    else if (!environment)
                        unavailable = environment.error().message;
                    else {
                        focus = {selected->position.x, selected->position.y, selected->position.z};
                        static_cast<rendering::SceneLightingSettings&>(view.lit) = *environment;
                        const auto* invalid = rendering::scene_lighting_focus_error(view.lit, focus);
                        if (!invalid)
                            invalid = renderer::probe_focus_error(view.lit, focus);
                        if (invalid)
                            unavailable = invalid;
                        else {
                            camera = *selected;
                            matrices = {projected->view.elements, projected->projection.elements, projected->far_depth};
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
                ui_error = project_ui.diagnostic();
                if (!ui_error.empty())
                    static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.ui_failed",
                                                  "Project UI: {}", ui_error));
            }
            if (prepared && started) {
                if (renderer) {
                    const auto& group = *scene.draw();
                    renderer::Frame rendered{
                        *prepared->drawing,
                        matrices,
                        camera ? ui::Rect{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height)}
                               : ui::Rect{},
                        camera ? std::span<const renderer::MeshInstance>{group.meshes}
                               : std::span<const renderer::MeshInstance>{},
                        view,
                        {},
                        camera ? std::span<const renderer::Light>{group.lights} : std::span<const renderer::Light>{},
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
                    const auto drawn = graphics(lit_fallback.render(*renderer, rendered, nullptr));
                    if (first_render) {
                        static_cast<void>(ATELIER_LOG(logging::Severity::info, "runner", "runner.first_render_end",
                                                      "First render completed"));
                        first_render = false;
                    }
                    if (drawn.result.status == renderer::FrameStatus::cancelled) {
                        performance.observe(timed.finish(), std::nullopt);
                        break;
                    }
                    if (drawn.failure != render_failure) {
                        render_failure = drawn.failure;
                        if (!render_failure.empty())
                            static_cast<void>(ATELIER_LOG(logging::Severity::error, "material",
                                                          "material.shader_failed", "Shader preparation failed: {}",
                                                          render_failure));
                    }
                    const auto& result = drawn.result;
                    if (result.status == renderer::FrameStatus::accepted) {
                        if (result.accepted_id != rendered.ui_frame.id ||
                            result.accepted_extent != rendered.ui_frame.extent)
                            throw std::runtime_error("Project renderer accepted a mismatched frame.");
                        if (!ui.accept(prepared))
                            throw std::runtime_error("Project UI refused the rendered frame.");
                        if (camera)
                            scene.accept(result.accepted_id, drawn.fallback, drawn.view.shading);
                        lighting_clock.accept(lighting_start,
                                              lighting_active && drawn.view.shading == renderer::Shading::scene_lit);
                        if (drawn.view.shading == renderer::Shading::scene_lit)
                            accepted_camera = camera;
                        sampled = result.accepted_id;
                    } else
                        lighting_clock.reset();
                } else {
                    if (!ui.accept(prepared))
                        throw std::runtime_error("Project UI refused the headless frame.");
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
            static_cast<void>(ATELIER_LOG(logging::Severity::info, "runner", "runner.document_stop_begin",
                                          "Document stop beginning"));
            auto stopped = document.stop();
            static_cast<void>(
                ATELIER_LOG(logging::Severity::info, "runner", "runner.document_stop_end", "Document stop returned"));
            report_diagnostics();
            complete(std::move(stopped));
        }
        if (!options.report.empty())
            take(editor::write_file(options.report, take(document.serialize())));
        performance.publish();
        report_performance(performance, frame);
        const std::array fields{logging::Field{"frames", frame},
                                logging::Field{"document", document.presentation().identity()}};
        static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "runner", "runner.stopped",
                                           logging::native::Context{fields}, "Project stopped: {} frames", frame));
        return 0;
    } catch (const std::exception& error) {
        static_cast<void>(ATELIER_LOG(logging::Severity::error, "runner", "runner.failed", "Project run: {}",
                                      std::string_view(error.what())));
        return 1;
    }
}
}
