#include "memory/fatal.hpp"
#include "application/application.hpp"
#include "application/diagnostics.hpp"
#include "application/lit_preparation.hpp"
#include "application/project_runner.hpp"
#include "editor/error.hpp"
#include "assets/file.hpp"
#include "control/control.hpp"
#include "platform/utf.hpp"
#include "logging/native.hpp"
#include "atelier/build_info.hpp"
#include <cstdio>
#include <string_view>
#include <array>
#include <cmath>
#include <chrono>

int verify_editor_window(atelier::application::Application&, const char* output_directory);

int verify_editor_output(atelier::application::Application&, const char* output_directory);

int verify_editor_source(atelier::application::Application&, const char* output_directory);

int verify_editor_edit_mode(atelier::application::Application&, const char* output_directory);

int verify_editor_shortcuts(atelier::application::Application&, const char* output_directory);

int verify_editor_surfaces(atelier::application::Application&, const char* output_directory);

int profile_editor_render_performance(atelier::application::Application&, const char* output_directory);

int verify_editor_tooltips(atelier::application::Application&, const char* output_directory);

int verify_editor_dialogs(atelier::application::Application&, const char* output_directory);

int capture_editor_snapshot(atelier::application::Application&, const char* output_directory);

int verify_editor_ime(atelier::application::Application&, const char* output_directory);

int verify_editor_project(atelier::application::Application&, const char* output_directory,
                          const wchar_t* source_project);

int verify_editor_scene_lit(atelier::application::Application&, const char* output_directory,
                            const wchar_t* source_project);

void guard_acceptance_native_dialogs();

namespace {
atelier::Result<int> host_main(int argc, wchar_t** argv) {
    atelier::application::install_frame_timing();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--version") {
        std::printf("FrostFox Atelier | %s | %s\n", atelier::build_info::revision, atelier::build_info::configuration);
        return 0;
    }
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--run-project")
        return atelier::application::run_project_command({argv + 2, static_cast<std::size_t>(argc - 2)});
    auto storage = atelier::control::default_directory();
    if (!storage)
        return std::unexpected(atelier::Error{atelier::ErrorCode::invalid_storage, storage.error().what()});
    auto& control_directory = *storage;
    bool no_control{};
    bool no_activate{};
    bool gpu_validation{};
    std::optional<atelier::ui::PixelExtent> initial_client;
    std::optional<std::filesystem::path> render_performance_output;
    const auto dimension = [](std::wstring_view value) {
        std::uint32_t pixels{};
        for (const auto c : value) {
            if (c < L'0' || c > L'9' || pixels > (32768u - static_cast<unsigned>(c - L'0')) / 10u)
                throw std::runtime_error("--client-size requires two integers in 1..32768 physical pixels.");
            pixels = pixels * 10 + static_cast<unsigned>(c - L'0');
        }
        if (!pixels)
            throw std::runtime_error("--client-size requires two integers in 1..32768 physical pixels.");
        return pixels;
    };
    std::vector<wchar_t*> arguments{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view option(argv[i]);
        if (option == L"--no-control")
            no_control = true;
        else if (option == L"--no-activate")
            no_activate = true;
        else if (option == L"--gpu-validation")
            gpu_validation = true;
        else if (option == L"--profile-render-performance") {
            if (render_performance_output || ++i == argc)
                throw std::runtime_error("--profile-render-performance requires one absolute output directory.");
            render_performance_output = std::filesystem::path(argv[i]);
            if (!render_performance_output->is_absolute())
                throw std::runtime_error("Render performance output directory must be absolute.");
        } else if (option == L"--client-size") {
            if (initial_client)
                throw std::runtime_error("--client-size can only be specified once.");
            if (i + 2 >= argc)
                throw std::runtime_error("--client-size requires width and height in physical pixels.");
            initial_client = atelier::ui::PixelExtent{dimension(argv[i + 1]), dimension(argv[i + 2])};
            i += 2;
        } else if (option == L"--control-directory") {
            if (++i == argc)
                throw std::runtime_error("Missing control directory");
            control_directory = argv[i];
        } else
            arguments.push_back(argv[i]);
    }
    argc = static_cast<int>(arguments.size());
    argv = arguments.data();
    const bool scene_argument = (argc == 5 || argc == 12) && std::wstring_view(argv[1]) == L"--project" &&
                                std::wstring_view(argv[3]) == L"--scene";
    const bool preview =
        argc == 12 && std::wstring_view(argv[1]) == L"--project" && std::wstring_view(argv[3]) == L"--scene" &&
        (std::wstring_view(argv[5]) == L"--material-camera" || std::wstring_view(argv[5]) == L"--scene-lit-camera");
    std::array<float, 6> camera{};
    if (preview)
        for (unsigned i = 0; i < 6; ++i) {
            wchar_t* end{};
            camera[i] = std::wcstof(argv[i + 6], &end);
            if (end == argv[i + 6] || *end || !std::isfinite(camera[i]))
                throw std::runtime_error("Camera values must be finite numbers.");
        }
    const bool acceptance = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance";
    const bool output = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-output";
    const bool source = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-source";
    const bool edit = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-edit";
    const bool shortcuts = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-shortcuts";
    const bool surfaces = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-surfaces";
    const bool tooltips = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-tooltips";
    const bool dialogs = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-dialogs";
    const bool snapshot = argc >= 2 && std::wstring_view(argv[1]) == L"--snapshot";
    const bool ime = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-ime";
    const bool project_acceptance = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-project";
    const bool scene_lit = argc >= 2 && std::wstring_view(argv[1]) == L"--acceptance-scene-lit";
    const bool project =
        (argc == 3 || (argc == 5 && scene_argument) || preview) && std::wstring_view(argv[1]) == L"--project";
    const bool create = argc == 3 && std::wstring_view(argv[1]) == L"--new-project";
    if (render_performance_output && !project)
        throw std::runtime_error(
            "--profile-render-performance requires --project, optionally followed by the existing --scene and camera arguments.");
    if (argc != 1 && !project &&
        !((acceptance || output || source || edit || shortcuts || surfaces || tooltips || dialogs || snapshot || ime ||
           create) &&
          argc == 3) &&
        !((project_acceptance || scene_lit) && (argc == 3 || argc == 4))) {
        std::fprintf(
            stderr,
            "Usage: atelier_editor [--log-directory absolute-new-directory] [--parent-run run-id] [--profile-render-performance absolute-output-directory] [--no-activate] [--client-size width height (physical pixels)] [--version] [--project|--new-project folder] [--scene relative.scene [--material-camera|--scene-lit-camera eye-x eye-y eye-z target-x target-y target-z]] [--snapshot|--acceptance|--acceptance-output|--acceptance-ime|--acceptance-source|--acceptance-edit|--acceptance-shortcuts|--acceptance-surfaces|--acceptance-tooltips|--acceptance-dialogs output-directory] [--acceptance-project|--acceptance-scene-lit output-directory [source-project]]\n");
        return 2;
    }
    const bool harness = acceptance || output || source || edit || shortcuts || surfaces || tooltips || dialogs ||
                         snapshot || ime || project_acceptance || scene_lit;
    if (harness)
        guard_acceptance_native_dialogs();
    if (harness || render_performance_output)
        atelier::application::isolate_shader_cache();
    atelier::application::Application app(
        gpu_validation || acceptance || source || edit || ime || project_acceptance || scene_lit,
        !snapshot && !shortcuts && !surfaces && !tooltips && !dialogs && !no_activate && !render_performance_output,
        initial_client, !render_performance_output);
    if (project || create) {
        const auto opened =
            app.open_project(argv[2], create,
                             render_performance_output ? atelier::application::ProjectOpenPolicy::require_scene
                                                       : atelier::application::ProjectOpenPolicy::allow_repair);
        if (!opened)
            throw std::runtime_error(opened.error().message);
    }
    if (scene_argument) {
        auto opened = app.open_scene(argv[4]);
        if (!opened)
            throw std::runtime_error(opened.error().message);
    }
    if (preview) {
        const bool lit = std::wstring_view(argv[5]) == L"--scene-lit-camera";
        auto view =
            app.configure_view({camera[0], camera[1], camera[2]}, {camera[3], camera[4], camera[5]},
                               lit ? atelier::renderer::Shading::scene_lit : atelier::renderer::Shading::smooth);
        if (!view)
            throw std::runtime_error(view.error().message);
    }
    std::string output_directory;
    if (render_performance_output || harness) {
        auto converted = atelier::platform::utf16_to_utf8(
            render_performance_output ? render_performance_output->native() : std::wstring_view(argv[2]));
        if (!converted)
            return std::unexpected(atelier::Error{atelier::ErrorCode::invalid_storage, "Invalid UTF-16 text"});
        output_directory = std::move(*converted);
    }
    if (render_performance_output)
        return profile_editor_render_performance(app, output_directory.c_str());
    if (source)
        return verify_editor_source(app, output_directory.c_str());
    if (edit)
        return verify_editor_edit_mode(app, output_directory.c_str());
    if (shortcuts)
        return verify_editor_shortcuts(app, output_directory.c_str());
    if (surfaces)
        return verify_editor_surfaces(app, output_directory.c_str());
    if (tooltips)
        return verify_editor_tooltips(app, output_directory.c_str());
    if (dialogs)
        return verify_editor_dialogs(app, output_directory.c_str());
    if (snapshot)
        return capture_editor_snapshot(app, output_directory.c_str());
    if (acceptance)
        return verify_editor_window(app, output_directory.c_str());
    if (output)
        return verify_editor_output(app, output_directory.c_str());
    if (ime)
        return verify_editor_ime(app, output_directory.c_str());
    if (project_acceptance)
        return verify_editor_project(app, output_directory.c_str(), argc == 4 ? argv[3] : nullptr);
    if (scene_lit)
        return verify_editor_scene_lit(app, output_directory.c_str(), argc == 4 ? argv[3] : nullptr);
    if (!no_control) {
        const auto enabled = app.enable_control(control_directory);
        if (!enabled)
            return std::unexpected(enabled.error());
    }
    while (app.step()) {
    }
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    atelier::install_fatal_handler();
    namespace log = atelier::logging;
    // Deliberately outside the business try: Application, World, workers and
    // catch diagnostics all retire before finish, including early command returns.
    std::unique_ptr<log::Run> run;
    int result = 1;
    try {
        log::RunOptions options;
        std::vector<wchar_t*> arguments{argv[0]};
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view option = argv[i];
            if (option == L"--log-directory" || option == L"--parent-run") {
                if (++i == argc)
                    throw std::runtime_error("Missing logging option value.");
                if (option == L"--log-directory")
                    options.directory = argv[i];
                else {
                    auto converted = atelier::platform::utf16_to_utf8(argv[i]);
                    if (!converted) {
                        std::fputs("Host failure: Invalid UTF-16 text\n", stderr);
                        return 1;
                    }
                    options.parent_run_id = std::move(*converted);
                }
            } else
                arguments.push_back(argv[i]);
        }
        options.process_name =
            arguments.size() >= 2 && std::wstring_view(arguments[1]) == L"--run-project" ? "runner" : "editor";
        auto started = log::start_run(std::move(options));
        if (!started) {
            // Startup failure rejects business startup, never pretends to have run.
            std::fprintf(stderr, "Logging startup failed; application was not started: %s\n",
                         started.error().message.c_str());
            return 2;
        }
        run = std::move(*started);
        static_cast<void>(ATELIER_LOG(log::Severity::info, "application", "application.started",
                                      "FrostFox Atelier | {} | {}", atelier::build_info::revision,
                                      atelier::build_info::configuration));
        const auto hosted = host_main(static_cast<int>(arguments.size()), arguments.data());
        if (hosted)
            result = *hosted;
        else {
            auto entry = atelier::application::error_diagnostic(hosted.error(), "application.failed", {});
            if (entry)
                entry->severity = log::Severity::critical;
            if (!entry || log::submit(std::move(*entry)).status != log::SubmitStatus::accepted)
                std::fprintf(stderr, "Host failure: %s\n", hosted.error().message.c_str());
        }
    } catch (const atelier::editor::detail::Failure& error) {
        auto entry = atelier::application::error_diagnostic(error.error, "application.failed", {});
        if (entry)
            entry->severity = log::Severity::critical;
        if (!entry || log::submit(std::move(*entry)).status != log::SubmitStatus::accepted)
            std::fprintf(stderr, "Host failure: %s\n", error.what());
    } catch (const std::exception& error) {
        const auto submitted = ATELIER_LOG(log::Severity::critical, "application", "application.failed",
                                           "Host failure: {}", std::string_view(error.what()));
        if (submitted.status != log::SubmitStatus::accepted)
            std::fprintf(stderr, "Host failure: %s\n", error.what());
    } catch (...) {
        const auto submitted =
            ATELIER_LOG(log::Severity::critical, "application", "application.failed", "Unknown host failure");
        if (submitted.status != log::SubmitStatus::accepted)
            std::fputs("Unknown host failure.\n", stderr);
    }
    if (run) {
        try {
            static_cast<void>(ATELIER_LOG(log::Severity::info, "application", "application.finished",
                                          "Business exit code {}", result));
            const auto final = run->finish();
            if (!final.backend_acknowledged || !final.sink_flush_succeeded || !final.summary_published ||
                final.health.facility_failures || final.health.backend_notifications ||
                final.health.rejected_queue_full || final.health.rejected_producer_limit ||
                final.health.rejected_record_too_large || final.health.rejected_invalid_record ||
                final.health.rejected_internal_failure)
                std::fputs(
                    "Logging completed with incomplete or unhealthy evidence; business exit code is unchanged.\n",
                    stderr);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Logging finish failed: %s\n", error.what());
        } catch (...) {
            std::fputs("Logging finish failed; business exit code is unchanged.\n", stderr);
        }
    }
    return result;
}
