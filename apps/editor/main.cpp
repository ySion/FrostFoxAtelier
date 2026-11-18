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

int profile_editor_render_performance(atelier::application::Application&, const char* output_directory);
atelier::Result<int> profile_editor_ui_reload(atelier::application::Application&, const std::filesystem::path& output_directory);

int capture_editor_snapshot(atelier::application::Application&, const char* output_directory);

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
    std::optional<std::filesystem::path> profile_output;
    bool profile_reload{};
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
        else if (option == L"--profile-render-performance" || option == L"--profile-ui-reload") {
            if (profile_output || ++i == argc)
                throw std::runtime_error("Choose one profile and an absolute output directory.");
            profile_reload = option == L"--profile-ui-reload";
            profile_output = std::filesystem::path(argv[i]);
            if (!profile_output->is_absolute())
                throw std::runtime_error("Profile output directory must be absolute.");
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
    const bool snapshot = argc >= 2 && std::wstring_view(argv[1]) == L"--snapshot";
    const bool project =
        (argc == 3 || (argc == 5 && scene_argument) || preview) && std::wstring_view(argv[1]) == L"--project";
    const bool create = argc == 3 && std::wstring_view(argv[1]) == L"--new-project";
    if (profile_output && !project)
        throw std::runtime_error(
            "Editor profiles require --project, optionally followed by --scene and camera arguments.");
    if (argc != 1 && !project && !((snapshot || create) && argc == 3)) {
        std::fprintf(
            stderr,
            "Usage: atelier_editor [--log-directory absolute-new-directory] [--parent-run run-id] [--profile-render-performance|--profile-ui-reload absolute-output-directory] [--no-activate] [--client-size width height (physical pixels)] [--version] [--project|--new-project folder] [--scene relative.scene [--material-camera|--scene-lit-camera eye-x eye-y eye-z target-x target-y target-z]] [--snapshot output-directory]\n");
        return 2;
    }
    if (snapshot)
        guard_acceptance_native_dialogs();
    if (snapshot || profile_output)
        atelier::application::isolate_shader_cache();
    auto document = std::make_unique<atelier::editor::Session>();
    ATELIER_TRY(document->initialize({}, atelier::editor::SessionRole::editor));
    atelier::application::Application app;
    const auto initialized =
        app.initialize(std::move(document), gpu_validation, !snapshot && !no_activate && !profile_output,
                       initial_client, !profile_output || profile_reload);
    if (!initialized)
        return std::unexpected(initialized.error());
    if (project || create) {
        const auto opened =
            app.open_project(argv[2], create,
                             profile_output ? atelier::application::ProjectOpenPolicy::require_scene
                                                       : atelier::application::ProjectOpenPolicy::allow_repair);
        if (!opened)
            return std::unexpected(opened.error());
    }
    if (scene_argument) {
        ATELIER_TRY(app.open_scene(argv[4]));
    }
    if (preview) {
        const bool lit = std::wstring_view(argv[5]) == L"--scene-lit-camera";
        auto view =
            app.configure_view({camera[0], camera[1], camera[2]}, {camera[3], camera[4], camera[5]},
                               lit ? atelier::renderer::Shading::scene_lit : atelier::renderer::Shading::smooth);
        if (!view)
            throw std::runtime_error(view.error().message);
    }
    if (profile_reload)
        return profile_editor_ui_reload(app, *profile_output);
    std::string output_directory;
    if (profile_output || snapshot) {
        auto converted = atelier::platform::utf16_to_utf8(
            profile_output ? profile_output->native() : std::wstring_view(argv[2]));
        if (!converted)
            return std::unexpected(atelier::Error{atelier::ErrorCode::invalid_storage, "Invalid UTF-16 text"});
        output_directory = std::move(*converted);
    }
    if (profile_output)
        return profile_editor_render_performance(app, output_directory.c_str());
    if (snapshot)
        return capture_editor_snapshot(app, output_directory.c_str());
    if (!no_control) {
        const auto enabled = app.enable_control(control_directory);
        if (!enabled)
            return std::unexpected(enabled.error());
    }
    for (;;) {
        ATELIER_TRY_BIND(running, app.step());
        if (!*running)
            break;
    }
    ATELIER_TRY(app.close());
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
