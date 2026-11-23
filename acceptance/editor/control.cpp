#include "memory/fatal.hpp"
#include "files.hpp"
#include "assets/file.hpp"
#include "../helpers.hpp"
#include "application/application.hpp"
#include "application/editor_control.hpp"
#include "control/control.hpp"
#include "foundation/sha256.hpp"
#include "foundation/scope_exit.hpp"
#include "logging/logging.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

namespace {
using namespace atelier;

using namespace atelier::control;

namespace platform = atelier::platform;

using atelier::platform::utf8_to_utf16_validated;

using namespace std::chrono_literals;

auto fixture_failure(std::string_view message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::string(message)});
}

Error transport_error(const Failure& error) {
    return {ErrorCode::invalid_storage, error.code + ": " + error.message};
}

Error json_error(const json::Error& error) {
    return {ErrorCode::invalid_definition, error.describe()};
}

Result<std::string> read_bytes(const std::filesystem::path& path) {
    return assets::read_file(path, std::string{}.max_size());
}

Result<void> write_bytes(const std::filesystem::path& path, std::span<const char> bytes) {
    return editor::write_file(path, {bytes.data(), bytes.size()});
}

Result<json::Document> read(const std::filesystem::path& path) {
    ATELIER_TRY_BIND(bytes, read_bytes(path));
    return json::Document::parse(*bytes, {32, json::DepthPolicy::containers, true}).transform_error(json_error);
}

std::wstring quote(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

auto system_failure(std::string_view action, DWORD code = GetLastError()) {
    return std::unexpected(
        Error{ErrorCode::invalid_storage,
              std::string(action) + ": " + platform::error_message(std::error_code(code, std::system_category()))});
}

struct CloseNative {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};

using NativeHandle = std::unique_ptr<void, CloseNative>;

struct Process {
    NativeHandle process;

    DWORD pid{};

    Process(Process&&) = default;

    static Result<Process> launch(const std::filesystem::path& executable, std::wstring arguments,
                                  const std::filesystem::path& output, const std::filesystem::path& input = {}) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const auto out = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE)
            return system_failure("Cannot create child log");
        NativeHandle log{out}, in;
        if (!input.empty()) {
            const auto file = CreateFileW(input.c_str(), GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return system_failure("Cannot open child standard input");
            in.reset(file);
        }
        auto command = quote(executable) + L" " + arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = log.get();
        startup.hStdInput = in.get();
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                            executable.parent_path().c_str(), &startup, &child))
            return system_failure("Cannot launch control E2E child");
        CloseHandle(child.hThread);
        return Process{child};
    }

    Result<DWORD> wait(std::chrono::milliseconds timeout, application::EditorControl* endpoint = nullptr) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            const auto status = WaitForSingleObject(process.get(), endpoint ? 1 : static_cast<DWORD>(timeout.count()));
            if (status == WAIT_OBJECT_0) {
                DWORD code{};
                if (!GetExitCodeProcess(process.get(), &code))
                    return system_failure("Cannot read child exit code");
                return code;
            }
            if (status != WAIT_TIMEOUT)
                return system_failure("Cannot wait for child");
            if (!endpoint)
                break;
            endpoint->poll();
        } while (std::chrono::steady_clock::now() < deadline);
        return fixture_failure("Timed out waiting for child exit");
    }

    ~Process() {
        if (!process || WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
            return;
        if (auto result = reap(); !result)
            std::cerr << "Child cleanup also failed: " << result.error().message << '\n';
    }

    HWND window() const {
        struct Query {
            DWORD pid;

            HWND window{};
        } query{pid};
        EnumWindows(
            [](HWND window, LPARAM parameter) -> BOOL {
                auto& query = *reinterpret_cast<Query*>(parameter);
                DWORD pid{};
                GetWindowThreadProcessId(window, &pid);
                if (pid == query.pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
                    query.window = window;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&query));
        return query.window;
    }

    Result<void> finish(Result<void> operation) {
        auto closed = close(operation ? 300s : 2s);
        if (!closed) {
            if (auto forced = reap(); !forced)
                std::cerr << "Child termination also failed: " << forced.error().message << '\n';
            if (operation)
                return closed;
            std::cerr << "Smoke cleanup also failed: " << closed.error().message << '\n';
        }
        return operation;
    }

private:
    explicit Process(PROCESS_INFORMATION child) : process(child.hProcess), pid(child.dwProcessId) {}

    Result<void> reap() {
        if (WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0 && !TerminateProcess(process.get(), 99)) {
            const auto error = GetLastError();
            if (WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0)
                return system_failure("Cannot terminate child", error);
        }
        ATELIER_TRY(wait(10s));
        return {};
    }

    Result<void> close(std::chrono::milliseconds timeout) {
        if (auto handle = window(); handle && !PostMessageW(handle, WM_CLOSE, 0, 0)) {
            const auto error = GetLastError();
            if (WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0)
                return system_failure("Cannot request editor close", error);
        }
        ATELIER_TRY_BIND(code, wait(timeout));
        if (*code != 0)
            return fixture_failure("Editor exited with code " + std::to_string(*code) + " (expected 0)");
        return {};
    }
};

Result<json::Document> run_cli(const std::filesystem::path& executable, const std::filesystem::path& directory,
                               std::wstring arguments, DWORD expected_exit,
                               application::EditorControl* endpoint = nullptr, const std::filesystem::path& input = {},
                               std::chrono::seconds timeout = 30s) {
    const auto output = directory / "cli-output.json";
    ATELIER_TRY_BIND(child,
                     Process::launch(executable, L"--directory " + quote(directory) + L" " + arguments, output, input));
    ATELIER_TRY_BIND(code, child->wait(timeout, endpoint));
    if (*code != expected_exit) {
        const auto bytes = read_bytes(output);
        return fixture_failure("CLI exited " + std::to_string(*code) + " instead of " + std::to_string(expected_exit) +
                               ": " + (bytes ? *bytes : bytes.error().message));
    }
    return read(output);
}

template <class Work>
auto while_polling(application::EditorControl& endpoint, Work work) {
    auto reply = std::async(std::launch::async, std::move(work));
    while (reply.wait_for(0ms) != std::future_status::ready)
        if (!endpoint.poll())
            std::this_thread::yield();
    return reply.get();
}

Result<json::Document> control_request(std::string_view op, const json::Document& args, std::string_view token,
                                       std::string_view id) {
    json::Builder request;
    ATELIER_TRY(request.set(request.root(), "op", op).transform_error(json_error));
    ATELIER_TRY(request.set(request.root(), "args", args.root()).transform_error(json_error));
    ATELIER_TRY(request.set(request.root(), "state_token", token).transform_error(json_error));
    if (!id.empty())
        ATELIER_TRY(request.set(request.root(), "request_id", id).transform_error(json_error));
    return request.finish();
}

struct Driver {
    Client client;

    std::string token;

    json::Document descriptor;

    static Result<Driver> connect(json::Document instance) {
        ATELIER_TRY_BIND(client, Client::create(instance, 600s).transform_error(transport_error));
        return Driver{std::move(*client), {}, std::move(instance)};
    }

    Result<json::Document> call(std::string op, const json::Document& args = {}, std::string id = {}) {
        const auto operator_id = args.root().string({"id"});
        const auto name = op == "op.call" && operator_id ? op + " " + std::string{*operator_id} : op;
        ATELIER_TRY_BIND(request, control_request(op, args, token, id));
        ATELIER_TRY_BIND(result, client.call(std::move(*request)).transform_error(transport_error));
        if (result->root().string({"status"}) == "error")
            std::cerr << "Control request failed: " << name << '\n';
        if (auto value = result->root().string({"state_token"}))
            token = *value;
        return std::move(*result);
    }

    Result<json::Document> run_job(const std::string& op, const std::string& id) {
        ATELIER_TRY_BIND(pending, call(op, {}, id));
        if (pending->root().string({"status"}) != "pending" || pending->root().string({"job_id"}) != id)
            return fixture_failure(op + " did not start under its request ID");
        json::Builder query;
        query.set_validated(query.root(), "job_id", id);
        const auto args = query.finish();
        const auto deadline = std::chrono::steady_clock::now() + 125s;
        do {
            ATELIER_TRY_BIND(result, call("job.status", args));
            if (result->root().string({"job_id"}) != id)
                return fixture_failure(op + " status lost its job ID");
            if (result->root().string({"status"}) != "pending")
                return result;
            std::this_thread::sleep_for(30ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return fixture_failure(op + " job did not finish");
    }

    Result<json::Document> ok(std::string op, const json::Document& args = {}) {
        return success(call(std::move(op), args));
    }

    static Result<json::Document> success(Result<json::Document> reply) {
        ATELIER_TRY(reply);
        if (reply->root().string({"status"}) != "ok") {
            const auto bytes = json::write(reply->root());
            return fixture_failure({bytes.data(), bytes.size()});
        }
        ATELIER_TRY_BIND(result, reply->root().at("result").transform_error(json_error));
        return json::Document{*result};
    }

    Result<json::Document> inspect() { return ok("editor.inspect"); }

    // Material preparation does not change the token. Wait for an accepted frame as well as
    // five unchanged inspections before using the editor state, including on a cold shader cache.
    Result<json::Document> settle() {
        const auto deadline = std::chrono::steady_clock::now() + 125s;
        ATELIER_TRY_BIND(state, inspect());

        std::string before = token;
        unsigned stable{};
        while (stable < 5 && std::chrono::steady_clock::now() < deadline) {
            before = token;
            std::this_thread::sleep_for(30ms);
            ATELIER_TRY_BIND(inspection, inspect());
            *state = std::move(*inspection);
            stable = before == token && state->root().at("last_frame")->size() != 0 ? stable + 1 : 0;
        }
        if (stable != 5) {
            const auto accepted = state->root().u64({"accepted_frames"}).value_or(0);
            const auto frame_fields = state->root().at("last_frame")->size();
            const auto busy = state->root().boolean({"busy"}).value_or(false);
            return fixture_failure(
                "Editor accepted frame did not arrive with a stable state token within 125s (before=" + before +
                ", token=" + token + ", accepted_frames=" + std::to_string(accepted) +
                ", last_frame_fields=" + std::to_string(frame_fields) + ", busy=" + (busy ? "true" : "false") + ")");
        }
        return state;
    }

    Result<json::Document> op(std::string id, const json::Document& args = {}, const json::Document& context = {}) {
        return success(try_op(std::move(id), args, context));
    }

    Result<json::Document> try_op(std::string id, const json::Document& args, const json::Document& context = {}) {
        json::Builder request;
        ATELIER_TRY(request.set(request.root(), "id", id).transform_error(json_error));
        ATELIER_TRY(request.set(request.root(), "args", args.root()).transform_error(json_error));
        ATELIER_TRY(request.set(request.root(), "context", context.root()).transform_error(json_error));
        return call("op.call", request.finish());
    }
};

Result<json::Document> discover_editor(const Process& editor, const std::filesystem::path& directory) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        ATELIER_TRY_BIND(instances, discover(directory).transform_error(transport_error));
        for (const auto instance : *instances->root().array())
            if (instance.u32({"pid"}) == editor.pid)
                return json::Document{instance};
        if (WaitForSingleObject(editor.process.get(), 0) != WAIT_TIMEOUT)
            return fixture_failure("Editor stopped before publishing its endpoint");
        std::this_thread::sleep_for(30ms);
    }
    return fixture_failure("Editor did not publish its endpoint");
}

Result<Driver> connect_editor(const Process& editor, const std::filesystem::path& directory) {
    ATELIER_TRY_BIND(instance, discover_editor(editor, directory));
    return Driver::connect(std::move(*instance));
}

struct SmokeLog {
    std::filesystem::path directory;

    std::string directory_text;

    std::unique_ptr<atelier::logging::Reader> reader;

    atelier::logging::ReadBatch latest;

    std::vector<atelier::logging::Record> records;

    static Result<SmokeLog> open(const std::filesystem::path& path) {
        const auto text = platform::utf16_to_utf8(path.native());
        if (!text)
            return fixture_failure("Invalid UTF-16 smoke log directory");
        auto reader = atelier::logging::Reader::open(path);
        if (!reader)
            return std::unexpected(Error{ErrorCode::invalid_storage, reader.error().message});
        return SmokeLog{path, *text, std::move(*reader), {}, {}};
    }

    Result<void> check(bool value, std::string_view message) const {
        if (!value)
            return fixture_failure(directory_text + ": " + std::string(message));
        return {};
    }

    Result<void> poll(bool failed_command = false) {
        latest = reader->poll();
        for (auto& record : latest.records) {
            const auto& event = record.entry.event_code;
            const bool expected =
                failed_command && (event == "application.failed" || event == "application.failure_preserved" ||
                                   event == "recovery.failed");
            ATELIER_TRY(check(expected || record.entry.severity < atelier::logging::Severity::error,
                              "run=" + record.run_id + " pid=" + std::to_string(record.process_id) +
                                  " record=" + std::to_string(record.record_sequence) + " " + record.entry.event_code +
                                  ": " + record.entry.message));
            records.push_back(std::move(record));
        }
        latest.records.clear();

        return {};
    }

    std::size_t count(std::string_view code) const {
        return std::ranges::count_if(records, [&](const auto& record) { return record.entry.event_code == code; });
    }

    Result<const atelier::logging::Record*> one(std::string_view code) const {
        const atelier::logging::Record* found{};
        for (const auto& record : records) {
            if (record.entry.event_code != code)
                continue;
            if (found)
                return fixture_failure(directory_text + ": Expected exactly one " + std::string(code));
            found = &record;
        }
        if (!found)
            return fixture_failure(directory_text + ": Missing " + std::string(code));
        return found;
    }

    // Only called after an OS exit, or the parent's OS-backed process_ended event.
    Result<void> finish(bool failed_command = false) {
        reader->source_ended();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            ATELIER_TRY(poll(failed_command));
            if (latest.status.finished)
                break;
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        const auto& status = latest.status;
        if (status.latest_issue) {
            const auto text = platform::utf16_to_utf8(status.latest_issue->file.native());
            if (!text)
                return fixture_failure("Invalid UTF-16 log issue path");
            ATELIER_TRY(check(false, *text + ":" + std::to_string(status.latest_issue->offset) + " " +
                                         status.latest_issue->message));
        }
        ATELIER_TRY(check(latest.run && status.finished && status.has_final_summary && !status.incomplete &&
                              !status.waiting_for_run && !status.partial_line && !status.unquantified_loss &&
                              !status.invalid_lines && !status.invalid_metadata && !status.oversized_lines &&
                              !status.incomplete_lines && !status.rotation_losses && !status.sequence_gaps &&
                              !status.duplicate_records && !status.out_of_order_records && !status.io_failures,
                          "Formal log is missing, incomplete or corrupt"));
        ATELIER_TRY(check(latest.health && latest.health->final, "Missing final=true health"));
        const auto& report = latest.health->report;
        const auto& health = report.health;
        ATELIER_TRY(check(health.state == atelier::logging::RunState::finished && report.backend_acknowledged &&
                              report.sink_flush_succeeded && report.summary_published && !health.rejected_queue_full &&
                              !health.rejected_producer_limit && !health.rejected_record_too_large &&
                              !health.rejected_invalid_record && !health.rejected_internal_failure &&
                              !health.facility_failures && !health.backend_notifications &&
                              health.first_backend_notification.empty() && !health.first_facility_issue &&
                              bool(health.first_error) == failed_command,
                          "Final logging health reports an error or facility fault"));
        ATELIER_TRY(one("application.started"));
        ATELIER_TRY(one("application.finished"));

        return {};
    }
};

template <class T>
Result<T> smoke_field(const atelier::logging::Record& record, std::string_view key) {
    for (const auto& field : record.entry.fields)
        if (field.key == key)
            if (const auto* value = std::get_if<T>(&field.value))
                return *value;
    return fixture_failure("run=" + record.run_id + " record=" + std::to_string(record.record_sequence) + " " +
                           record.entry.event_code + ": missing or mistyped field " + std::string(key));
}

// Drive the same executable, host logging, WIC writer and scene recovery used by a real command.
Result<void> verify_host_failures(const std::filesystem::path& executable, const std::filesystem::path& root) {
    ATELIER_TRY_BIND(project, editor::Project::create(root / "Project"));
    const auto output = root / "Output";
    const auto local_data = root / "LocalAppData";
    const auto png_target = output / "snapshot-rotate.png";
    ATELIER_TRY(acceptance::create_directories(png_target));
    std::wstring previous(32768, L'\0');
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), static_cast<DWORD>(previous.size()));
    if (!size || !(size < previous.size()))
        return fixture_failure("Cannot isolate recovery storage.");
    previous.resize(size);
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", local_data.c_str()))
        return fixture_failure("Cannot select recovery storage.");
    ScopeExit restore{[&]() noexcept { SetEnvironmentVariableW(L"LOCALAPPDATA", previous.c_str()); }};
    const auto run = [&](const wchar_t* name, std::wstring arguments) -> Result<SmokeLog> {
        const auto directory = root / name;
        ATELIER_TRY_BIND(child,
                         Process::launch(executable,
                                         arguments + L" --no-control --no-activate --log-directory " + quote(directory),
                                         root / (std::wstring{name} + L".log")));
        ATELIER_TRY_BIND(code, child->wait(120s));

        if (*code != 1)
            return fixture_failure("A rejected host command did not exit with code 1.");
        ATELIER_TRY_BIND(log, SmokeLog::open(directory));
        ATELIER_TRY(log->finish(true));
        ATELIER_TRY_BIND(failure_result, log->one("application.failed"));
        const auto& failure = **failure_result;
        ATELIER_TRY_BIND(application_failure_preserved_record, log->one("application.failure_preserved"));
        ATELIER_TRY_BIND(error_code_field, smoke_field<std::uint64_t>(failure, "error_code"));
        ATELIER_TRY_BIND(application_finished_record, log->one("application.finished"));
        if ((*application_failure_preserved_record)->entry.message != failure.entry.message ||
            *error_code_field != static_cast<std::uint64_t>(atelier::ErrorCode::invalid_storage) ||
            (*application_finished_record)->entry.message != "Business exit code 1")
            return fixture_failure("The host changed the first error or reported a different exit code.");
        return log;
    };
    const auto snapshot = L"--snapshot " + quote(output);
    ATELIER_TRY_BIND(dirty, run(L"Snapshot", snapshot));
    ATELIER_TRY_BIND(application_failed_record, dirty->one("application.failed"));
    const auto& cause = (*application_failed_record)->entry.message;
    ATELIER_TRY_BIND(saved_result, dirty->one("recovery.saved"));
    const auto& saved = **saved_result;
    ATELIER_TRY_BIND(reason_field, smoke_field<std::string>(saved, "reason"));
    if (!cause.starts_with("PNG encoding failed: ") || *reason_field != cause)
        return fixture_failure("The snapshot lost the WIC failure before recovery.");
    ATELIER_TRY_BIND(recovery_scene_field, smoke_field<std::string>(saved, "recovery_scene"));
    const auto scene = std::filesystem::path(utf8_to_utf16_validated(*recovery_scene_field));
    ATELIER_TRY_BIND(metadata, read(scene.parent_path() / "recovery.json"));
    if (metadata->root().string({"reason"}) != cause || metadata->root().string({"scene"}) != "Scene.scene")
        return fixture_failure("Recovery metadata lost the original snapshot failure.");
    ATELIER_TRY_BIND(bytes, read_bytes(scene));
    ATELIER_TRY_BIND(recovered, editor::Session::deserialize(*bytes, *project, {}, editor::LoadPolicy::strict));
    ATELIER_TRY_BIND(members, (*recovered)->world().members());

    if (members->size() != 1)
        return fixture_failure("The recovered snapshot lost its authored mesh.");
    ATELIER_TRY_BIND(mesh_type, (*recovered)->world().runtime().type_of(members->front().id()));
    ATELIER_TRY_BIND(rendered, (*recovered)->presentation().render_items());
    if (*mesh_type != (*recovered)->presentation_types().static_mesh_entity || rendered->empty())
        return fixture_failure("The recovered snapshot lost its authored mesh.");

    // A second failure while saving recovery must leave the WIC error as the host's first error.
    const auto blocked = root / "BlockedAppData";
    ATELIER_TRY(acceptance::create_directories(blocked / "FrostFoxAtelier"));
    ATELIER_TRY(write_bytes(blocked / "FrostFoxAtelier/Recovery", std::string_view{"occupied"}));
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", blocked.c_str()))
        return fixture_failure("Cannot select blocked recovery storage.");
    ATELIER_TRY_BIND(lost, run(L"RecoveryFailure", snapshot));
    ATELIER_TRY_BIND(recovery_failure, lost->one("application.failed"));
    ATELIER_TRY_BIND(recovery_failed_record, lost->one("recovery.failed"));
    ATELIER_TRY_BIND(retry_reason, smoke_field<std::string>(**recovery_failed_record, "reason"));
    if ((*recovery_failure)->entry.message != cause || *retry_reason != cause || lost->count("recovery.saved"))
        return fixture_failure("Recovery I/O replaced the original snapshot error.");

    const auto args = L"--project " + quote(project->root()) + L" ";
    const auto reject_profile = [&](const wchar_t* name, const wchar_t* option, const wchar_t* file,
                                    editor::WriteMode mode) -> Result<void> {
        const auto target = output / file;
        const auto expected = editor::write_file(target, "probe", mode);
        if (expected)
            return fixture_failure("The profile output fixture was writable.");
        ATELIER_TRY_BIND(log, run(name, args + option + L" " + quote(output)));
        ATELIER_TRY_BIND(application_failed_record, log->one("application.failed"));
        ATELIER_TRY_BIND(recovery_not_needed_record, log->one("recovery.not_needed"));
        ATELIER_TRY_BIND(reason_field, smoke_field<std::string>(**recovery_not_needed_record, "reason"));
        if ((*application_failed_record)->entry.message != expected.error().message ||
            *reason_field != expected.error().message)
            return fixture_failure("The profile lost its output error or saved a clean scene.");

        return {};
    };
    ATELIER_TRY(acceptance::create_directory(output / "render-performance.json"));
    ATELIER_TRY(reject_profile(L"RenderProfile", L"--profile-render-performance", L"render-performance.json",
                               editor::WriteMode::replace));
    ATELIER_TRY(write_bytes(output / "editor.ui.source", std::string_view{"keep"}));
    ATELIER_TRY(reject_profile(L"UiProfile", L"--profile-ui-reload", L"editor.ui.source", editor::WriteMode::create));
    ATELIER_TRY_BIND(retained_source, read_bytes(output / "editor.ui.source"));
    if (*retained_source != "keep")
        return fixture_failure("Rejected profile replaced the existing UI source.");

    return {};
}

Result<void> verify_smoke_logs(SmokeLog& parent, SmokeLog& child) {
    ATELIER_TRY(parent.finish());
    ATELIER_TRY_BIND(ended_result, parent.one("play.process_ended"));
    const auto& ended = **ended_result;
    ATELIER_TRY_BIND(forced_field, smoke_field<bool>(ended, "forced"));
    ATELIER_TRY_BIND(exit_code_field, smoke_field<std::uint64_t>(ended, "exit_code"));
    ATELIER_TRY(
        parent.check(!*forced_field && *exit_code_field == 0, "Play did not end normally when its Editor closed"));
    ATELIER_TRY(child.finish());
    ATELIER_TRY(child.one("runner.stop_observed"));
    ATELIER_TRY(child.one("runner.stopped"));

    return {};
}

Result<void> prune_passed_smoke_runs(const std::filesystem::path& base, std::size_t keep) {
    struct Entry {
        std::filesystem::path path;

        std::filesystem::file_time_type modified{};
    };
    std::vector<Entry> passed;
    std::error_code error;
    for (std::filesystem::directory_iterator it(base, error), end; !error && it != end; it.increment(error)) {
        const auto& item = *it;
        if (!item.is_directory(error)) {
            if (error)
                return std::unexpected(acceptance::file_error(error));
            continue;
        }
        const auto marker = item.path() / "passed";
        ATELIER_TRY_BIND(complete, acceptance::file_exists(marker));
        if (*complete) {
            const auto timestamp = std::filesystem::last_write_time(marker, error);
            if (error)
                return std::unexpected(acceptance::file_error(error));
            passed.push_back({item.path(), timestamp});
        }
    }
    if (error)
        return std::unexpected(acceptance::file_error(error));
    std::ranges::sort(passed, [](const Entry& left, const Entry& right) { return left.modified > right.modified; });
    for (std::size_t index = keep; index < passed.size(); ++index) {
        std::filesystem::remove_all(passed[index].path, error);
        if (error)
            return std::unexpected(acceptance::file_error(error));
    }
    return {};
}

struct ArtifactCli {
    const std::filesystem::path& executable;
    const std::filesystem::path& directory;
    std::string instance;
    std::string token;

    Result<json::Document> call(std::string_view op, DWORD exit_code, const json::Document& args = {},
                                std::string_view id = {}) {
        const auto path = directory / "cli-request.json";
        ATELIER_TRY_BIND(request, control_request(op, args, token, id));
        ATELIER_TRY(write_bytes(path, json::write(request->root())));
        ATELIER_TRY_BIND(reply,
                         run_cli(executable, directory,
                                 L"--instance " + utf8_to_utf16_validated(instance) + L" --request " + quote(path),
                                 exit_code, nullptr, {}, 125s));
        const auto state = reply->root().string({"state_token"});
        if (reply->root().string({"instance_id"}) != instance || !state ||
            reply->root().string({"status"}) != (exit_code ? "error" : "ok"))
            return fixture_failure("Expected a terminal response from the editor for " + std::string(op));
        token = *state;
        return reply;
    }

    Result<json::Document> job(std::string_view op, const std::string& id, DWORD exit_code) {
        ATELIER_TRY_BIND(reply, call(op, exit_code, {}, id));
        const auto poll_id = reply->root().string({"request_id"});
        const auto field = exit_code ? "error" : "result";
        const auto outcome = reply->root().at(field);
        if (reply->root().string({"job_id"}) != id || !poll_id || *poll_id == id || !outcome)
            return fixture_failure("CLI did not obtain the writer's terminal outcome from job.status");
        json::Builder query;
        query.set_validated(query.root(), "job_id", id);
        const auto args = query.finish();
        const auto confirm = [&](const std::string& method) -> Result<void> {
            const auto request_id = id + "-" + method;
            ATELIER_TRY_BIND(retained, call(method, exit_code, args, request_id));
            const auto payload = retained->root().at(field);
            if (retained->root().string({"request_id"}) != request_id || retained->root().string({"job_id"}) != id ||
                !payload || *payload != *outcome)
                return fixture_failure("Completed writer did not retain its first outcome on " + method);

            return {};
        };
        if (exit_code)
            ATELIER_TRY(confirm("job.cancel"));
        ATELIER_TRY(confirm("job.status"));
        return reply;
    }
};

Result<void> verify_capture(ArtifactCli& cli) {
    ATELIER_TRY_BIND(
        invalid, cli.call("render.capture", 1, json::Document::literal<R"({"sample_frames":[2,1],"timeout_ms":0})">()));
    if (invalid->root().string({"status"}) != "error" ||
        invalid->root().string({"error", "code"}) != "invalid_request" ||
        invalid->root().string({"error", "message"}) != "Sample ordinals must increase strictly")
        return fixture_failure("Capture validation lost its first error");

    const auto artifacts = cli.directory / "artifacts" / cli.instance;
    ATELIER_TRY_BIND(removed, acceptance::remove_file(artifacts));
    if (!*removed)
        return fixture_failure("Expected a fresh empty instance artifact directory");
    if (!std::ofstream(artifacts, std::ios::binary))
        return fixture_failure("Could not install the capture write obstruction");
    std::error_code cause;
    std::filesystem::create_directory(artifacts / "probe", cause);
    if (!cause)
        return fixture_failure("Expected the filesystem to reject a child of a regular file");
    for (const auto op : {std::string_view{"render.capture"}, std::string_view{"repro.export"}}) {
        ATELIER_TRY_BIND(failed, cli.job(op, std::string(op) + "-write-failure", 1));
        if (failed->root().string({"error", "code"}) != "storage_error" ||
            failed->root().string({"error", "message"}) != platform::error_message(cause))
            return fixture_failure("Writer lost the filesystem error for " + std::string(op));
    }

    ATELIER_TRY_BIND(obstruction_removed, acceptance::remove_file(artifacts));
    if (!*obstruction_removed)
        return fixture_failure("Could not remove the artifact obstruction");
    ATELIER_TRY_BIND(created, acceptance::create_directory(artifacts));
    if (!*created)
        return fixture_failure("Could not restore the instance artifact directory");
    ATELIER_TRY_BIND(saved, cli.job("render.capture", "capture-after-repair", 0));
    if (saved->root().string({"status"}) != "ok")
        return fixture_failure("Capture did not recover after repairing storage");
    const auto manifest_name = saved->root().string({"result", "manifest"});
    if (!manifest_name)
        return fixture_failure("Completed capture did not return its manifest");
    const std::filesystem::path path = utf8_to_utf16_validated(*manifest_name);
    ATELIER_TRY_BIND(manifest, read(path));
    const auto samples = manifest->root().at("samples");
    if (path.parent_path().parent_path() != artifacts || manifest->root().boolean({"complete"}) != true ||
        manifest->root().string({"kind"}) != "render.capture" ||
        manifest->root().string({"request_id"}) != "capture-after-repair" || !samples || samples->size() != 1 ||
        *samples != *saved->root().at("result")->at("samples"))
        return fixture_failure("Published capture manifest and protocol response differ");
    const auto sample = *samples->at(0);
    if (sample.string({"color"}) != "sample-1.png" || sample.string({"depth"}) != "sample-1.depth.f32" ||
        sample.u32({"accepted_ordinal"}) != 1)
        return fixture_failure("Capture returned the wrong sample");
    const auto extent = sample.at("extent");
    if (!extent || extent->size() != 2)
        return fixture_failure("Missing capture extent");
    const auto width = extent->at(0)->u32().value_or(0);
    const auto height = extent->at(1)->u32().value_or(0);
    std::ifstream color(path.parent_path() / "sample-1.png", std::ios::binary);
    char signature[8];
    if (!color.read(signature, sizeof(signature)) ||
        std::string_view(signature, sizeof(signature)) != "\x89PNG\r\n\x1a\n" || !width || !height)
        return fixture_failure("Capture PNG or extent does not match the manifest");
    ATELIER_TRY_BIND(depth_bytes, acceptance::file_operation([&](auto& error) {
                         return std::filesystem::file_size(path.parent_path() / "sample-1.depth.f32", error);
                     }));
    if (*depth_bytes != std::uint64_t{width} * height * sizeof(float))
        return fixture_failure("Capture depth payload does not match the manifest");

    return {};
}

Result<void> verify_repro(ArtifactCli& cli, const std::filesystem::path& project) {
    const auto oversized = project / "Source/export-budget.bin";
    if (!std::ofstream(oversized, std::ios::binary))
        return fixture_failure("Could not create the export budget fixture");
    ATELIER_TRY(acceptance::file_operation(
        [&](auto& error) { std::filesystem::resize_file(oversized, 256ull * 1024 * 1024 + 1, error); }));
    ATELIER_TRY_BIND(failed, cli.job("repro.export", "export-over-budget", 1));
    if (failed->root().string({"status"}) != "error" || failed->root().string({"error", "code"}) != "resource_limit" ||
        failed->root().string({"error", "message"}) != "Repro exceeds 10000 files / 1 GiB / 256 MiB per file")
        return fixture_failure("The writer's file budget error did not reach job.status");
    ATELIER_TRY_BIND(removed, acceptance::remove_file(oversized));
    if (!*removed)
        return fixture_failure("Could not remove the oversized input");

    const auto artifacts = cli.directory / "artifacts" / cli.instance;
    std::filesystem::path partial;
    std::error_code scan_error;
    for (std::filesystem::directory_iterator entry(artifacts, scan_error), end; !scan_error && entry != end;
         entry.increment(scan_error)) {
        ATELIER_TRY_BIND(exists, acceptance::file_exists(entry->path() / "Project/Config/Project.json"));
        if (*exists) {
            if (!partial.empty())
                return fixture_failure("Expected exactly one partial repro");
            partial = entry->path();
        }
    }
    if (scan_error)
        return std::unexpected(acceptance::file_error(scan_error));
    if (partial.empty())
        return fixture_failure("Failed export did not retain its partially copied project");
    ATELIER_TRY_BIND(exists, acceptance::file_exists(partial / "manifest.json"));
    if (*exists)
        return fixture_failure("Failed export must leave its partially copied project without a completion marker");
    const auto path = platform::utf16_to_utf8(partial.generic_wstring());
    if (!path)
        return fixture_failure("Invalid UTF-16 repro fixture path");
    json::Builder args;
    args.set_validated(args.root(), "directory", *path);
    ATELIER_TRY_BIND(before, cli.call("editor.inspect", 0));
    const auto baseline = *before->root().at("result");
    ATELIER_TRY_BIND(rejected, cli.call("repro.open", 1, args.finish()));
    ATELIER_TRY_BIND(after, cli.call("editor.inspect", 0));

    const auto current = *after->root().at("result");
    if (rejected->root().string({"status"}) != "error" ||
        rejected->root().string({"error", "code"}) != "storage_error" ||
        !rejected->root().string({"error", "message"}).value_or("").ends_with("manifest.json") ||
        *baseline.at("project") != *current.at("project") || *baseline.at("scene_path") != *current.at("scene_path"))
        return fixture_failure(
            "Opening a partial repro must reject its missing manifest and preserve the open document");

    const auto blocked = project / "Source/export-locked.bin";
    {
        const auto handle =
            CreateFileW(blocked.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return fixture_failure("Could not create an exclusively held export input");
        const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
        const auto cause = foundation::sha256_file(blocked);
        if (cause)
            return fixture_failure("Expected a sharing violation while hashing the export input");
        ATELIER_TRY_BIND(unreadable, cli.job("repro.export", "export-read-failure", 1));
        if (unreadable->root().string({"status"}) != "error" ||
            unreadable->root().string({"error", "code"}) != "storage_error" ||
            unreadable->root().string({"error", "message"}) != cause.error())
            return fixture_failure("Failed dependency read lost its storage error or original diagnostic");
    }
    ATELIER_TRY_BIND(locked_file_removed, acceptance::remove_file(blocked));
    if (!*locked_file_removed)
        return fixture_failure("Could not remove the locked input after releasing it");
    ATELIER_TRY_BIND(saved, cli.job("repro.export", "export-after-repair", 0));
    const auto manifest_name = saved->root().string({"result", "manifest"});
    if (saved->root().string({"status"}) != "ok" || !manifest_name)
        return fixture_failure("Export did not recover after removing the oversized file");
    const std::filesystem::path manifest_path = utf8_to_utf16_validated(*manifest_name);
    ATELIER_TRY_BIND(manifest, read(manifest_path));
    const auto scene = manifest->root().string({"scene"});
    if (manifest_path.parent_path() == partial || manifest->root().boolean({"complete"}) != true ||
        manifest->root().string({"kind"}) != "repro" ||
        manifest->root().string({"request_id"}) != "export-after-repair" || !scene)
        return fixture_failure("Recovered export returned an incomplete or unrelated manifest");
    ATELIER_TRY_BIND(frozen, read(manifest_path.parent_path() / "Project" / utf8_to_utf16_validated(*scene)));
    if (frozen->root().at("entities")->size() != baseline.at("entities")->size())
        return fixture_failure("The exported scene lost members of the open document");
    const auto files = manifest->root().at("input_files")->array();
    if (!files)
        return fixture_failure("Completed repro lacks its project dependency list");
    std::filesystem::path identity;
    for (const auto file : *files) {
        const auto name = file.string({"path"});
        if (name && name->ends_with(".asset.json")) {
            identity = utf8_to_utf16_validated(*name);
            break;
        }
    }
    if (identity.empty() || !identity.is_relative() || *identity.begin() != L"Content")
        return fixture_failure("Expected an asset identity among the Sponza scene dependencies");
    const auto source = project / identity;
    const auto copied = manifest_path.parent_path() / "Project" / identity;
    ATELIER_TRY_BIND(metadata, read(copied));
    const auto id = metadata->root().string({"id"});
    ATELIER_TRY_BIND(original, read(source));
    if (!id || original->root() != metadata->root())
        return fixture_failure("Export changed a project dependency's identity");
    const auto held = cli.directory / "withheld-asset-identity.json";
    ATELIER_TRY(acceptance::file_operation([&](auto& error) { std::filesystem::rename(source, held, error); }));
    auto missing = cli.call("repro.export", 1, {}, "export-missing-identity");
    auto restored = acceptance::file_operation([&](auto& error) { std::filesystem::rename(held, source, error); });
    if (!missing) {
        if (!restored)
            std::cerr << "Identity cleanup also failed: " << restored.error().message << '\n';
        return std::unexpected(std::move(missing).error());
    }
    ATELIER_TRY(restored);
    if (missing->root().string({"status"}) != "error" ||
        missing->root().string({"error", "code"}) != "dependency_mismatch" ||
        missing->root().string({"error", "message"}) != "Asset identity file is missing: project/" + std::string(*id))
        return fixture_failure("Snapshot did not return the missing asset's identity before launching the writer");
    ATELIER_TRY_BIND(unstarted,
                     cli.call("job.status", 1, json::Document::literal<R"({"job_id":"export-missing-identity"})">()));
    if (unstarted->root().string({"error", "code"}) != "unknown_job")
        return fixture_failure("A failed dependency snapshot leaked a job");

    return {};
}

json::Document object_query(json::Value row) {
    json::Builder query;
    query.set_validated(query.root(), "detail", "object");
    query.set_validated(query.root(), "object", *row.at("handle"));
    return query.finish();
}

Result<void> require_control_error(const Result<json::Document>& received, std::string_view code,
                                   std::string_view message) {
    ATELIER_TRY(received);
    const auto& reply = *received;
    if (reply.root().string({"status"}) != "error" || reply.root().string({"error", "code"}) != code ||
        reply.root().string({"error", "message"}) != message || *reply.root().find("result")) {
        const auto bytes = json::write(reply.root());
        return fixture_failure({bytes.data(), bytes.size()});
    }
    return {};
}

Result<void> require_stale_object(Driver& driver, const json::Document& query) {
    return require_control_error(driver.call("editor.inspect", query), "stale_object",
                                 "Object handle is no longer a document member");
}

Result<json::Document> verify_inspection_pages(Driver& driver, const std::filesystem::path& project) {
    ATELIER_TRY_BIND(parent,
                     driver.op("scene.add", json::Document::literal<R"({"kind":"mesh","mesh":"builtin/cube"})">()));
    ATELIER_TRY_BIND(child, driver.op("scene.add", json::Document::literal<R"({"kind":"entity"})">()));

    json::Builder reparent;
    reparent.set_validated(reparent.root(), "parent", *parent->root().at("active"));
    ATELIER_TRY(driver.op("scene.parent_set", reparent.finish()));

    ATELIER_TRY_BIND(full, driver.ok("editor.inspect", json::Document::literal<R"({"detail":"full"})">()));
    const auto members = *full->root().at("entities");
    if (members.size() != 2)
        return fixture_failure("Full inspection lost a scene member");
    const auto root_entity = *members.at(0);
    const auto leaf = *members.at(1);
    if (*root_entity.at("handle") != *parent->root().at("active") || root_entity.string({"parent"}) != "" ||
        *leaf.at("handle") != *child->root().at("active") || *leaf.at("parent") != *root_entity.at("handle") ||
        root_entity.at("children")->size() != 1 || *root_entity.at("children")->at(0) != *leaf.at("handle") ||
        leaf.at("children")->size() != 0 || leaf.at("components")->size() != 0 ||
        root_entity.at("components")->size() != 1)
        return fixture_failure("Full inspection changed the authored hierarchy or component ownership");
    const auto mesh = *root_entity.at("components")->at(0);
    std::size_t mesh_types{};
    for (const auto name : *full->root().at("component_types")->array())
        if (name == *mesh.at("type"))
            ++mesh_types;
    if (mesh_types != 1)
        return fixture_failure("Component type directory must list the mesh component type once");
    const auto args = [](std::string_view detail, json::Value cursor) {
        json::Builder query;
        query.set_validated(query.root(), "detail", detail);
        query.set_validated(query.root(), "cursor", cursor);
        return query;
    };
    const auto reject = [&](const json::Builder& query, std::string_view code,
                            std::string_view message) -> Result<void> {
        return require_control_error(driver.call("editor.inspect", query.finish()), code, message);
    };
    json::Builder objects(json::Kind::array);
    for (const auto entity : *members.array()) {
        const auto components = *entity.at("components")->array();
        const auto add = [&](json::Value source, bool component) {
            const auto row = objects.append_object(objects.root());
            for (const auto key : {"handle", "name", "type", "placeholder", "saved_type"})
                if (const auto found = *source.find(key))
                    objects.set_validated(row, key, *found);
            objects.set_validated(row, "kind", component ? "component" : "entity");
            objects.set_validated(row, "entity", *entity.at("handle"));
            if (!component)
                objects.set_validated(row, "parent", *entity.at("parent"));
        };
        add(entity, false);
        for (const auto component : components)
            add(component, true);
    }
    const auto walk = [&](std::string_view detail, json::Value expected) -> Result<json::Document> {
        json::Builder query;
        query.set_validated(query.root(), "detail", detail);
        if (!query.set(query.root(), "limit", 1u))
            return fixture_failure("Invalid first-page limit");
        json::Document first_cursor;
        std::size_t consumed{};
        for (;;) {
            ATELIER_TRY_BIND(page, driver.ok("editor.inspect", query.finish()));
            const auto rows = *page->root().at(detail)->array();
            const auto next = *page->root().at("next_cursor");
            if (page->root().string({"detail"}) != detail || !(rows.size() <= (consumed ? 64u : 1u)) ||
                (!next.is_null() && !rows.size()))
                return fixture_failure("Invalid directory page or cursor without progress");
            for (const auto row : rows) {
                if (!(consumed < expected.size()) || row != *expected.at(consumed))
                    return fixture_failure("Paged directory differs from full inspection in content or order");
                ++consumed;
            }
            if (next.is_null())
                break;
            if (consumed == 1)
                first_cursor = json::Document(next);
            query.set_validated(query.root(), "cursor", next);
            if (!query.set(query.root(), "limit", 64u))
                return fixture_failure("Invalid continuation limit");
        }
        if (consumed != expected.size() || first_cursor.root().size() == 0)
            return fixture_failure("Directory did not span complete pages");
        return first_cursor;
    };
    ATELIER_TRY_BIND(assets, walk("assets", *full->root().at("assets")));
    ATELIER_TRY_BIND(positions, walk("objects", objects.root()));

    ATELIER_TRY(
        reject(args("objects", assets->root()), "invalid_request", "Unexpected object members at /args/cursor"));
    ATELIER_TRY(
        reject(args("assets", positions->root()), "invalid_request", "Unexpected object members at /args/cursor"));

    const std::string filter_name{*root_entity.string({"name"})};
    json::Builder filtered;
    filtered.set_validated(filtered.root(), "detail", "objects");
    filtered.set_validated(filtered.root(), "entity_name", filter_name);
    if (!filtered.set(filtered.root(), "limit", 1u))
        return fixture_failure("Invalid filtered page limit");
    ATELIER_TRY_BIND(first, driver.ok("editor.inspect", filtered.finish()));
    const auto first_row = *first->root().at("objects")->at(0);
    if (first_row.string({"kind"}) != "entity" || first_row.string({"name"}) != filter_name)
        return fixture_failure("Wrong filtered entity");
    ATELIER_TRY_BIND(entity_detail, driver.ok("editor.inspect", object_query(first_row)));
    const auto entity = *entity_detail->root().at("object");
    if (entity_detail->root().string({"detail"}) != "object" || *entity.at("handle") != *first_row.at("handle") ||
        *entity.at("type") != *first_row.at("type") || entity.at("properties")->kind() != json::Kind::array)
        return fixture_failure("Paged entity handle did not resolve to its reflected detail");
    auto resume = args("objects", *first->root().at("next_cursor"));
    ATELIER_TRY_BIND(second, driver.ok("editor.inspect", resume.finish()));
    const auto next_row = *second->root().at("objects")->at(0);
    if (next_row.string({"kind"}) != "component" || *next_row.at("entity") != *first_row.at("handle"))
        return fixture_failure("Filtered cursor did not resume at the entity's component");
    const auto component_query = object_query(next_row);
    ATELIER_TRY_BIND(component_detail, driver.ok("editor.inspect", component_query));
    const auto component = *component_detail->root().at("object");
    if (component != mesh || *component.at("handle") != *next_row.at("handle") ||
        component.string({"mesh", "source"}) != "builtin/cube" ||
        component.at("materials")->kind() != json::Kind::array)
        return fixture_failure("Paged cube component lost its binding or mesh detail");
    for (const auto row : *second->root().at("objects")->array()) {
        if (*row.at("entity") != *first_row.at("handle"))
            return fixture_failure("Continuation dropped its inherited name filter");
    }
    if (!second->root().at("next_cursor")->is_null())
        return fixture_failure("Filtered page advertised a nonmatching continuation");
    resume.set_validated(resume.root(), "entity_name", filter_name + "-other");
    ATELIER_TRY(reject(resume, "invalid_request", "A pagination cursor cannot change its filters"));

    auto bad_asset = args("assets", assets->root());
    const auto asset_pos = *bad_asset.root().at("cursor");
    if (!bad_asset.set(asset_pos, "index", static_cast<std::uint64_t>(full->root().at("assets")->size() + 1)))
        return fixture_failure("Invalid fixture asset index");
    ATELIER_TRY(reject(bad_asset, "invalid_request", "Asset directory cursor is outside the catalog"));
    bad_asset.set_validated(asset_pos, "domain", "objects");
    ATELIER_TRY(reject(bad_asset, "invalid_request", "Expected an assets cursor"));

    auto bad_object = args("objects", positions->root());
    const auto object_pos = *bad_object.root().at("cursor");
    if (!bad_object.set(object_pos, "entity", static_cast<std::uint64_t>(full->root().at("entities")->size() + 1)))
        return fixture_failure("Invalid fixture entity index");
    ATELIER_TRY(reject(bad_object, "invalid_request", "Object directory cursor is outside the document"));
    ATELIER_TRY_BIND(added, driver.op("scene.add", json::Document::literal<R"({"kind":"entity"})">()));
    ATELIER_TRY_BIND(current,
                     driver.ok("editor.inspect", json::Document::literal<R"({"detail":"objects","limit":256})">()));

    const auto rows = *current->root().at("objects");
    if (!(rows.size() > 0) || !current->root().at("next_cursor")->is_null())
        return fixture_failure("Expected a single page for the fixture");
    const auto added_row = *rows.at(rows.size() - 1);
    if (*added_row.at("handle") != *added->root().at("active"))
        return fixture_failure("New entity missing from the object directory");
    const auto removed = object_query(added_row);
    ATELIER_TRY(driver.op("scene.delete", json::Document::literal<R"({"target":"entities"})">()));
    ATELIER_TRY(require_stale_object(driver, removed));
    ATELIER_TRY(driver.op("edit.undo"));
    ATELIER_TRY(reject(args("objects", positions->root()), "stale_cursor",
                       "The object directory changed; restart without a cursor"));
    ATELIER_TRY(reject(bad_object, "stale_cursor", "The object directory changed; restart without a cursor"));

    const auto asset_query = args("assets", assets->root()).finish();
    ATELIER_TRY(driver.op("asset.refresh"));
    ATELIER_TRY_BIND(unchanged, driver.ok("editor.inspect", asset_query));
    if (unchanged->root().u64({"project_revision"}) != *assets->root().u64({"project_revision"}))
        return fixture_failure("Scene edit or unchanged refresh invalidated the published asset directory");
    ATELIER_TRY_BIND(new_objects,
                     driver.ok("editor.inspect", json::Document::literal<R"({"detail":"objects","limit":1})">()));
    const auto folder = project / "Content/ControlPagination";
    ATELIER_TRY_BIND(created, acceptance::create_directory(folder));
    if (!*created)
        return fixture_failure("Could not create a new asset directory entry");
    ATELIER_TRY(driver.op("asset.refresh"));
    ATELIER_TRY(reject(args("assets", assets->root()), "stale_cursor",
                       "The asset directory changed; restart without a cursor"));
    ATELIER_TRY(reject(bad_asset, "invalid_request", "Expected an assets cursor"));
    bad_asset.set_validated(asset_pos, "domain", "assets");
    bad_asset.set_validated(asset_pos, "index", "invalid");
    ATELIER_TRY(reject(bad_asset, "invalid_request", "Expected unsigned integer at /args/cursor/index"));
    ATELIER_TRY_BIND(still_current,
                     driver.ok("editor.inspect", args("objects", *new_objects->root().at("next_cursor")).finish()));
    if (still_current->root().u64({"revision"}) != *new_objects->root().u64({"revision"}))
        return fixture_failure("Publishing assets invalidated the object directory");
    ATELIER_TRY_BIND(changed, driver.inspect());
    const auto published = *changed->root().at("assets")->array();
    bool found{};
    for (const auto entry : published)
        if (entry.string({"path"}) == "Content/ControlPagination")
            found = true;
    if (!found)
        return fixture_failure("Refresh did not publish the new folder");
    ATELIER_TRY_BIND(folder_removed, acceptance::remove_file(folder));
    if (!*folder_removed)
        return fixture_failure("Could not remove the test asset directory");
    ATELIER_TRY(driver.op("asset.refresh"));
    return component_query;
}

Result<void> verify_repro_open(Driver& driver, const std::filesystem::path& examples) {
    const auto path_args = [](std::string_view field, const std::filesystem::path& path) -> Result<json::Document> {
        const auto name = platform::utf16_to_utf8(path.generic_wstring());
        if (!name)
            return fixture_failure("Invalid repro fixture path");
        json::Builder args;
        args.set_validated(args.root(), field, *name);
        return args.finish();
    };

    const auto project = examples.parent_path() / "ReproSource";
    ATELIER_TRY_BIND(arguments, path_args("path", project));
    ATELIER_TRY(driver.op("project.new", *arguments));
    ATELIER_TRY_BIND(old_component, verify_inspection_pages(driver, project));
    ATELIER_TRY(driver.op("scene.save"));
    ATELIER_TRY_BIND(before, driver.settle());
    ATELIER_TRY_BIND(exported, driver.run_job("repro.export", "open-fixture"));

    const auto manifest_name = exported->root().string({"result", "manifest"});
    if (exported->root().string({"status"}) != "ok" || !manifest_name)
        return fixture_failure("Could not export the strict repro fixture");
    const std::filesystem::path marker = utf8_to_utf16_validated(*manifest_name);
    const auto bundle = marker.parent_path();
    ATELIER_TRY_BIND(args, path_args("directory", bundle));
    ATELIER_TRY_BIND(manifest, read(marker));

    const auto scene_name = *manifest->root().string({"scene"});
    const auto scene = bundle / "Project" / utf8_to_utf16_validated(scene_name);
    ATELIER_TRY_BIND(original, read_bytes(scene));
    ATELIER_TRY_BIND(frozen, read(scene));

    const auto reject = [&](std::string_view code, std::string_view message) -> Result<void> {
        ATELIER_TRY_BIND(reply, driver.call("repro.open", *args));
        const auto encoded = json::write(reply->root());
        if (reply->root().string({"status"}) != "error" || reply->root().string({"error", "code"}) != code ||
            reply->root().string({"error", "message"}) != message)
            return fixture_failure({encoded.data(), encoded.size()});
        ATELIER_TRY_BIND(after, driver.inspect());

        for (const auto field : {"project", "scene_path", "entities", "selection", "inspected_object", "dirty", "undo",
                                 "redo", "camera", "view"}) {
            if (*before->root().at(field) != *after->root().at(field))
                return fixture_failure(std::string("Rejected repro changed current ") + field);
        }

        return {};
    };

    // Same length and still valid JSON: only the stored digest distinguishes the modified file.
    auto modified = *original;
    const auto space = modified.find_first_of(" \n\r\t");
    if (space == modified.npos)
        return fixture_failure("Expected whitespace in the exported scene");
    modified[space] = modified[space] == ' ' ? '\t' : ' ';
    ATELIER_TRY(write_bytes(scene, modified));
    ATELIER_TRY(reject("dependency_mismatch", "Repro bundle file set or content changed"));

    // Update the file record so the malformed scene reaches the strict object constructor.
    json::Builder broken{*frozen};
    const auto entities = *broken.root().at("entities");
    const auto first = *frozen->root().at("entities")->at(0);
    const auto id = *first.string({"id"});
    broken.append(entities, first);
    broken.append(entities, json::Document::literal<R"({})">().root());
    const auto bytes = json::write(broken.root());
    ATELIER_TRY(write_bytes(scene, bytes));
    json::Builder listing{*manifest};
    const auto file_path = "Project/" + std::string(scene_name);
    bool updated{};
    for (const auto file : *listing.root().at("files")->array())
        if (file.string({"path"}) == file_path) {
            if (!listing.set(file, "bytes", bytes.size()))
                return fixture_failure("Could not update scene length");
            listing.set_validated(file, "sha256", foundation::sha256(std::as_bytes(std::span(bytes))));
            updated = true;
            break;
        }
    if (!updated)
        return fixture_failure("Exported scene is absent from the manifest file list");
    ATELIER_TRY(write_bytes(marker, json::write(listing.root())));
    ATELIER_TRY(reject("operation_failed", "Duplicate graph object identity: " + std::string(id)));

    // A well-formed, loadable scene may still fail admission at selection restoration.
    ATELIER_TRY(write_bytes(scene, *original));
    json::Builder invalid_selection{*manifest};
    const auto indices = *invalid_selection.array(invalid_selection.root(), "selection_indices");
    invalid_selection.append(indices, static_cast<std::uint64_t>(entities.size()));
    ATELIER_TRY(write_bytes(marker, json::write(invalid_selection.root())));
    ATELIER_TRY(reject("dependency_mismatch", "Repro selection exceeds scene size"));

    json::Builder wrong_scene{*manifest};
    wrong_scene.set_validated(wrong_scene.root(), "scene", "Content/Scenes/Other.scene");
    ATELIER_TRY(write_bytes(marker, json::write(wrong_scene.root())));
    ATELIER_TRY(reject("dependency_mismatch", "Repro scene differs from the project startup scene"));

    json::Builder omitted{*manifest};
    const auto engine = *omitted.root().at("engine_assets");
    const auto records = *manifest->root().at("engine_assets")->at("records");
    if (records.size() == 0)
        return fixture_failure("Expected the cube's engine dependencies in the repro");
    const auto first_key = *records.at(0)->string({"key"});
    if (!omitted.array(engine, "records"))
        return fixture_failure("Could not clear the declared engine dependencies");
    ATELIER_TRY(write_bytes(marker, json::write(omitted.root())));
    ATELIER_TRY(
        reject("dependency_mismatch", "Repro omits or changes an actual engine dependency: " + std::string(first_key)));

    ATELIER_TRY(write_bytes(marker, json::write(manifest->root())));
    ATELIER_TRY(driver.ok("editor.inspect", *old_component));
    ATELIER_TRY(driver.ok("repro.open", *args));
    ATELIER_TRY(require_stale_object(driver, *old_component));
    ATELIER_TRY_BIND(opened, driver.settle());
    const auto project_name = *exported->root().string({"result", "project"});
    if (opened->root().string({"project"}) != project_name || opened->root().string({"scene_path"}) != scene_name ||
        *opened->root().at("camera") != *before->root().at("camera") ||
        *opened->root().at("view") != *before->root().at("view") || opened->root().boolean({"dirty"}) != false)
        return fixture_failure("Repro did not adopt the frozen project, scene and view");
    const auto selected = opened->root().at("selection");
    const auto selected_indices = manifest->root().at("selection_indices");
    const auto members = opened->root().at("entities");
    if (members->size() != frozen->root().at("entities")->size() || selected->size() != 1 ||
        selected_indices->size() != 1)
        return fixture_failure("Repro did not restore the scene members and selection");
    const auto member = members->at(*selected_indices->at(0)->u32());
    if (*member->at("handle") != *selected->at(0))
        return fixture_failure("Restored selection identifies another entity");
    ATELIER_TRY_BIND(example_args, path_args("path", examples));
    ATELIER_TRY(driver.op("project.open", *example_args));
    ATELIER_TRY(driver.settle());

    return {};
}

template <class T>
Result<void> require_transport_error(const std::expected<T, Failure>& result, std::string_view code,
                                     std::string_view message) {
    if (result)
        return fixture_failure("Expected Control transport failure: " + std::string(code));
    const auto& error = result.error();
    if (error.code != code || error.message != message)
        return fixture_failure(error.code + ": " + error.message);

    return {};
}

Result<void> verify_cli(const std::filesystem::path& exe, const std::filesystem::path& directory,
                        application::EditorControl& endpoint, const json::Document& instance) {
    const auto id = *instance.root().string({"instance_id"});
    const auto target = L"--instance " + utf8_to_utf16_validated(id);
    const auto path = directory / "cli-request.json";
    const auto from_file = target + L" --request " + quote(path);
    const auto cli = [&](std::wstring args, DWORD code) {
        return run_cli(exe, directory, std::move(args), code, &endpoint);
    };
    ATELIER_TRY_BIND(listed, cli(L"--list", 0));
    if (listed->root().at("instances")->at(0)->string({"instance_id"}) != id)
        return fixture_failure("CLI did not discover the live editor");
    ATELIER_TRY(require_control_error(cli(L"--timeout-ms", 1), "invalid_request", "Missing command-line option value"));
    ATELIER_TRY(
        require_control_error(cli(L"--timeout-ms absent --unknown x", 1), "invalid_request", "invalid stoul argument"));
    ATELIER_TRY(require_control_error(cli(L"--timeout-ms 999999999999999999999", 1), "invalid_request",
                                      "stoul argument out of range"));
    ATELIER_TRY(require_control_error(cli(L"--timeout-ms -4294967295 --list", 1), "invalid_request",
                                      "Timeout must be between 1 and 600000 ms"));
    ATELIER_TRY(require_control_error(cli(from_file, 1), "storage_error", "Cannot open request file"));

    const std::string malformed = "{";
    ATELIER_TRY(write_bytes(path, malformed));
    const auto parsed = parse(malformed);
    if (parsed)
        return fixture_failure("Expected malformed CLI request fixture");
    ATELIER_TRY(require_control_error(cli(from_file, 1), "invalid_request", parsed.error().describe()));
    {
        auto occupying = Client::create(instance, 5s);
        if (!occupying)
            return std::unexpected(transport_error(occupying.error()));
        ATELIER_TRY(require_control_error(cli(from_file + L" --timeout-ms 75", 1), "busy",
                                          "Editor control endpoint is busy or unavailable"));
        ATELIER_TRY(write_bytes(path, std::string(maximum_message + 1, ' ')));
        ATELIER_TRY(require_control_error(cli(from_file + L" --timeout-ms 75", 1), "invalid_request",
                                          "Request exceeds its size limit"));
    }
    const auto inspect =
        json::Document::literal<R"({"op":"editor.inspect","args":{"detail":"summary"},"request_id":"cli-inspect"})">();
    ATELIER_TRY(write_bytes(path, json::write(inspect.root())));
    ATELIER_TRY_BIND(observed, run_cli(exe, directory, target + L" --request -", 0, &endpoint, path));
    if (observed->root().string({"status"}) != "ok" || observed->root().string({"instance_id"}) != id ||
        observed->root().string({"request_id"}) != "cli-inspect")
        return fixture_failure("CLI lost the standard input request identity");

    json::Builder view(
        json::Document::literal<R"({"op":"op.call","args":{"id":"view.set","args":{"shading":"scene_lit"}}})">());
    view.set_validated(view.root(), "state_token", *observed->root().at("state_token"));
    ATELIER_TRY(write_bytes(path, json::write(view.root())));
    ATELIER_TRY_BIND(updated, cli(from_file, 0));

    // This owner services real Control requests without rendering a frame, so the trace stays pending.
    json::Builder trace(json::Document::literal<R"({"op":"render.trace","request_id":"cli-trace"})">());
    trace.set_validated(trace.root(), "state_token", *updated->root().at("state_token"));
    json::Builder short_job(trace.root());
    short_job.set_validated(short_job.root(), "request_id", "cli-server-timeout");
    if (!short_job.set(short_job.object_validated(short_job.root(), "args"), "timeout_ms", 150u))
        return fixture_failure("Could not set trace deadline");
    ATELIER_TRY(write_bytes(path, json::write(short_job.root())));
    ATELIER_TRY_BIND(expired, cli(from_file + L" --timeout-ms 5000", 1));
    ATELIER_TRY(require_control_error(expired, "timeout", "Control job deadline exceeded"));
    if (expired->root().string({"job_id"}) != "cli-server-timeout" || expired->root().string({"instance_id"}) != id)
        return fixture_failure("CLI replaced the server deadline with a local timeout");
    ATELIER_TRY(write_bytes(path, json::write(trace.root())));
    ATELIER_TRY(require_control_error(cli(from_file + L" --timeout-ms 400", 1), "timeout",
                                      "Control job timed out; disconnect cancels its temporary override"));
    const auto status =
        json::Document::literal<R"({"op":"job.status","request_id":"cli-status","args":{"job_id":"cli-trace"}})">();
    ATELIER_TRY(write_bytes(path, json::write(status.root())));
    ATELIER_TRY_BIND(cancelled, cli(from_file, 1));
    ATELIER_TRY(require_control_error(cancelled, "cancelled", "Control connection ended"));
    if (cancelled->root().string({"job_id"}) != "cli-trace" ||
        cancelled->root().string({"request_id"}) != "cli-status" || cancelled->root().string({"instance_id"}) != id)
        return fixture_failure("CLI lost the server's terminal job response");

    json::Builder reset(
        json::Document::literal<R"({"op":"op.call","args":{"id":"layout.reset"},"request_id":"initial-layout"})">());
    reset.set_validated(reset.root(), "state_token", *cancelled->root().at("state_token"));
    ATELIER_TRY(write_bytes(path, json::write(reset.root())));
    ATELIER_TRY_BIND(initial, cli(from_file, 0));
    if (initial->root().string({"result", "status"}) != "finished" ||
        initial->root().string({"request_id"}) != "initial-layout")
        return fixture_failure("CLI could not reset the initial layout");

    // Consume real dock node identities on a persistent client; the final rejection goes through the CLI.
    auto fill = while_polling(endpoint, [&]() -> Result<std::string> {
        ATELIER_TRY_BIND(driver, Driver::connect(instance));
        driver->token = *initial->root().string({"state_token"});
        for (unsigned i = 0; i < 65536; ++i) {
            ATELIER_TRY_BIND(reply, driver->try_op("layout.reset", {}));
            if (reply->root().string({"status"}) == "ok")
                continue;
            ATELIER_TRY(require_control_error(reply, "operation_failed", "Dock identities exhausted."));
            ATELIER_TRY(driver->op("layout.show", json::Document::literal<R"({"tab":"viewport"})">()));
            return driver->token;
        }
        return fixture_failure("Dock reset never reached the identity budget");
    });
    if (!fill)
        return std::unexpected(std::move(fill).error());
    reset.set_validated(reset.root(), "request_id", "exhausted-layout");
    reset.set_validated(reset.root(), "state_token", *fill);
    ATELIER_TRY(write_bytes(path, json::write(reset.root())));
    ATELIER_TRY_BIND(exhausted, cli(from_file, 1));
    ATELIER_TRY(require_control_error(exhausted, "operation_failed", "Dock identities exhausted."));
    if (exhausted->root().string({"instance_id"}) != id ||
        exhausted->root().string({"request_id"}) != "exhausted-layout" ||
        exhausted->root().string({"state_token"}) != *reset.root().string({"state_token"}))
        return fixture_failure("Failed host request lost its identity or state token");
    ATELIER_TRY_BIND(replay, cli(from_file, 1));
    if (*replay != *exhausted)
        return fixture_failure("Replaying the failed host request lost its first error");

    reset.set_validated(reset.root(), "request_id", "invalid-layout-args");
    const auto arguments = reset.object_validated(*reset.root().at("args"), "args");
    reset.set_validated(arguments, "unknown", "field");
    ATELIER_TRY(write_bytes(path, json::write(reset.root())));
    ATELIER_TRY(
        require_control_error(cli(from_file, 1), "invalid_request", "Unknown argument for layout.reset: unknown"));

    json::Builder menu(json::Document::literal<R"({"op":"op.call","args":{"id":"ui.menu"}})">());
    menu.set_validated(menu.root(), "state_token", *exhausted->root().at("state_token"));
    ATELIER_TRY(write_bytes(path, json::write(menu.root())));
    // Menu polling requires viewport focus before it can report the human-interaction requirement.
    return require_control_error(cli(from_file, 1), "interaction_required",
                                 "Operator ui.menu requires human interaction.");
}

Result<void> next_editor_frame(application::Application& app) {
    const auto before = app.accepted_frames();
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (app.accepted_frames() == before) {
        ATELIER_TRY_BIND(running, app.step());
        if (!*running || !(std::chrono::steady_clock::now() < deadline))
            return fixture_failure("Editor did not accept the next tree frame");
    }
    return {};
}

Result<void> verify_editor_trees(application::Application& app, editor::Session& session, world::World& world,
                                 Driver& driver, application::EditorControl& endpoint) {
    const auto parent = session.selection().entity;
    ATELIER_TRY(session.add_entity());
    const auto child = session.selection().entity;
    ATELIER_TRY(session.reparent(child, parent));
    ATELIER_TRY(session.select(parent));
    session.mark_saved();

    struct VisibleRow {
        ui::ControlKey key;
        std::uint32_t index, depth;
        bool expanded;
        bool operator==(const VisibleRow&) const = default;
    };
    const auto visible = [&](ui::ControlKey tree) {
        std::vector<VisibleRow> rows;
        const auto& frame = *app.presented();
        for (const auto& row : frame.tree_rows)
            if (frame.find(row.key)->parent == tree)
                rows.emplace_back(row.key, row.index, row.depth, row.expanded);
        return rows;
    };
    std::optional<world::EntityRemoval> deep_branch;
    for (const bool structure : {false, true}) {
        const auto tree = structure ? editor::control::structure_tree : editor::control::entity_tree;
        const auto tab = structure ? "structure" : "scene_tree";
        auto opened = while_polling(endpoint, [&]() -> Result<void> {
            json::Builder args;
            args.set_validated(args.root(), "tab", tab);
            ATELIER_TRY(driver.call("editor.inspect", json::Document::literal<R"({"detail":"summary"})">()));
            ATELIER_TRY(driver.op("layout.show", args.finish()));
            return {};
        });
        ATELIER_TRY(opened);
        ATELIER_TRY(next_editor_frame(app));
        const auto baseline = visible(tree);
        if (baseline.size() != 2 || baseline.back().depth != 1 || !baseline.front().expanded ||
            baseline.back().expanded)
            return fixture_failure("Unexpected initial parent/child rows in the editor tree");
        const auto toggle = [&]() -> Result<void> {
            const auto key = (structure ? baseline.front() : baseline.back()).key;
            const auto& rows = app.presented()->tree_rows;
            const auto row = std::ranges::find(rows, key, &ui::TreeRowGeometry::key);
            if (row == rows.end() || !(row->disclosure.width > 0))
                return fixture_failure("Missing tree disclosure");
            const auto& box = row->disclosure;
            const auto point = MAKELPARAM(int(box.x + box.width / 2), int(box.y + box.height / 2));
            if (!PostMessageW(app.window(), WM_LBUTTONDOWN, MK_LBUTTON, point) ||
                !PostMessageW(app.window(), WM_LBUTTONUP, 0, point))
                return fixture_failure("Could not click the tree disclosure");

            return {};
        };
        ATELIER_TRY(toggle());
        ATELIER_TRY(next_editor_frame(app));
        if (visible(tree).size() != (structure ? 1u : 3u))
            return fixture_failure("Tree disclosure did not change visible rows");
        ATELIER_TRY(toggle());
        ATELIER_TRY(next_editor_frame(app));
        if (visible(tree) != baseline)
            return fixture_failure("Tree disclosure did not restore the original rows");
        const auto accepted = app.presented();
        ObjectId branch;
        if (deep_branch) {
            ATELIER_TRY(world.restore(*deep_branch));
            branch = deep_branch->root.id();
        } else {
            auto at = child;
            for (unsigned i = 0; i < 128; ++i) {
                ATELIER_TRY_BIND(entity, world::create_entity(world.runtime()));
                ATELIER_TRY(world.attach(entity->id(), world::append, at));
                if (i == 0)
                    branch = entity->id();
                at = entity->id();
            }
        }
        ATELIER_TRY(toggle()); // Reject an explicit expansion edit along with the candidate rows.
        if (!structure)
            ATELIER_TRY(session.select(child)); // Automatic reveal must roll back as well.
        const auto refused = next_editor_frame(app);
        if (refused || refused.error().code != ErrorCode::capacity_exceeded ||
            refused.error().message != "A tree exceeds 128 levels." || app.presented() != accepted)
            return fixture_failure("Over-depth tree replaced the accepted frame or lost the first tree error");
        ATELIER_TRY_ASSIGN(deep_branch, world.detach(branch));
        ATELIER_TRY(session.select(parent));
        ATELIER_TRY(next_editor_frame(app));
        if (visible(tree) != baseline)
            return fixture_failure("Rejected tree changed stable row keys, positions or expansion");
        if (!structure) {
            ATELIER_TRY(session.select(child));
            ATELIER_TRY(next_editor_frame(app));
            const auto expanded = visible(tree);
            if (expanded.size() != 3 || expanded[1].key != baseline.back().key || !expanded[1].expanded ||
                expanded.back().depth != 2)
                return fixture_failure("Selecting a child did not expand its accepted Scene Tree parent");
            ATELIER_TRY(session.select(parent));
        }
    }

    return {};
}

// Drive the real owner-thread endpoint while the same document changes World phase.
Result<void> verify_inspect_read_errors(const std::filesystem::path& directory, const std::filesystem::path& cli) {
    auto document = std::make_unique<editor::Session>();
    auto& session = *document;
    world::World* scene_world{};
    ATELIER_TRY(session.initialize(
        [&](world::World& world) -> Result<std::shared_ptr<void>> {
            scene_world = &world;
            return std::shared_ptr<void>{};
        },
        editor::SessionRole::editor));
    ATELIER_TRY(session.add_entity());
    application::Application app;
    ATELIER_TRY(app.initialize(std::move(document), false, false));
    auto endpoint = application::EditorControl::create(app, directory);
    if (!endpoint)
        return std::unexpected(transport_error(endpoint.error()));
    const auto instances = discover(directory);
    if (!instances || instances->root().size() != 1)
        return fixture_failure("Inspection fixture did not publish its endpoint");
    const json::Document instance{*instances->root().at(0)};
    ATELIER_TRY(verify_cli(cli, directory, *endpoint, instance));
    {
        auto client = Client::create(instance, 5s);
        if (!client)
            return std::unexpected(transport_error(client.error()));
        json::Builder nested;
        auto args = nested.object_validated(nested.root(), "args");
        for (unsigned i = 0; i < 32; ++i)
            args = nested.object_validated(args, "x");
        nested.set_validated(nested.root(), "op", "unknown");
        const auto bytes = json::write(nested.root());
        const auto parsed = parse({bytes.data(), bytes.size()});
        if (parsed || parsed.error().fault != json::Fault::depth)
            return fixture_failure("Expected a request beyond the Control nesting limit");
        ATELIER_TRY(
            require_transport_error(client->call(nested.finish()), "invalid_request", parsed.error().describe()));
    }
    auto stalled = Client::create(instance, 250ms);
    if (!stalled)
        return std::unexpected(transport_error(stalled.error()));
    ATELIER_TRY(require_transport_error(Client::create(instance, 50ms), "busy",
                                        "Editor control endpoint is busy or unavailable"));
    const auto query_summary = json::Document::literal<R"({"op":"editor.inspect","args":{"detail":"summary"}})">();
    // Withhold owner-thread polling until the client times out waiting for the reply.
    ATELIER_TRY(require_transport_error(stalled->call(query_summary), "timeout", "Control I/O cancelled or timed out"));
    // A new connection is accepted only after the server has retired the previous peer.
    ATELIER_TRY_BIND(driver, Driver::connect(instance));
    if (endpoint->poll())
        return fixture_failure("A request abandoned by the timed-out client reached the editor");
    auto disconnected = while_polling(*endpoint, [&] { return stalled->call(query_summary); });
    ATELIER_TRY(require_transport_error(disconnected, "disconnected", "Control peer disconnected"));

    // The driver occupies the pipe. The second CLI must be reaped while it waits to connect.
    const auto blocked_request = directory / "blocked-request.json";
    ATELIER_TRY(write_bytes(blocked_request, json::write(query_summary.root())));
    const auto instance_id = utf8_to_utf16_validated(*instance.root().string({"instance_id"}));
    const auto arguments = L"--directory " + quote(directory) + L" --instance " + instance_id +
                           L" --timeout-ms 60000 --request " + quote(blocked_request);
    ATELIER_TRY_BIND(blocked, Process::launch(cli, arguments, directory / "blocked-cli.log"));
    const auto primary = transport_error(disconnected.error());
    const auto failure = blocked->finish(std::unexpected(primary));
    ATELIER_TRY_BIND(exit_code, blocked->wait(0ms));
    if (failure || failure.error().code != primary.code || failure.error().message != primary.message ||
        *exit_code != 99)
        return fixture_failure("Child cleanup lost the asynchronous disconnect or did not reap the blocked CLI");

    const auto inspect = [&](const json::Document& args) {
        return while_polling(*endpoint, [&] { return driver->call("editor.inspect", args); });
    };
    ATELIER_TRY_BIND(page, inspect(json::Document::literal<R"({"detail":"objects","limit":1})">()));
    if (page->root().string({"status"}) != "ok")
        return fixture_failure("Could not page the inspection fixture");
    const auto query = object_query(*page->root().at("result")->at("objects")->at(0));
    session.mark_saved();
    ATELIER_TRY(next_editor_frame(app));
    const auto prior_frame = app.presented();
    ATELIER_TRY(session.start());
    const auto properties = session.properties(session.selection().object);
    const auto refused = next_editor_frame(app);
    if (properties || refused || refused.error().code != properties.error().code ||
        refused.error().message != properties.error().message || app.presented() != prior_frame)
        return fixture_failure("The parameter panel lost the property error or published a partial frame.");
    ATELIER_TRY(
        require_control_error(inspect(query), "operation_failed", "Properties require an open, stopped document."));
    ATELIER_TRY(session.stop());
    ATELIER_TRY_BIND(recovered, inspect(query));
    if (recovered->root().string({"status"}) != "ok")
        return fixture_failure("Stopped document did not recover object detail");
    const auto full = json::Document::literal<R"({"detail":"full"})">();
    // A World-owned child can be traversed, but has no author record for its property read.
    ATELIER_TRY_BIND(runtime_child, world::create_entity(scene_world->runtime()));
    ATELIER_TRY(scene_world->attach(runtime_child->id(), world::append, session.selection().entity));
    ATELIER_TRY(
        require_control_error(inspect(full), "operation_failed", "The Entity is not editable in this document."));
    ATELIER_TRY(scene_world->detach(runtime_child->id()));
    ATELIER_TRY_BIND(complete, inspect(full));
    if (complete->root().string({"status"}) != "ok" || complete->root().at("result")->at("entities")->size() != 1 ||
        complete->root().at("result")->at("entities")->at(0)->at("children")->size() != 0)
        return fixture_failure("Full inspection did not recover the complete hierarchy after the failed child read");
    ATELIER_TRY(verify_editor_trees(app, session, *scene_world, *driver, *endpoint));
    const auto accepted_tree = app.presented();
    ATELIER_TRY(session.close());
    const auto closed = next_editor_frame(app);
    if (closed || closed.error().code != ErrorCode::unsupported_operation ||
        closed.error().message != "World is closed." || app.presented() != accepted_tree)
        return fixture_failure("Tree build on a closed World replaced the accepted frame or lost its first read error");
    ATELIER_TRY(require_control_error(inspect(query), "operation_failed", "World is closed."));
    return require_control_error(inspect(full), "operation_failed", "World is closed.");
}

Result<void> verify_graph_requests(Driver& driver) {
    ATELIER_TRY_BIND(empty,
                     driver.ok("graph.create_scratch", json::Document::literal<R"({"key":"scratch/control"})">()));
    ATELIER_TRY_BIND(graph, driver.ok("graph.inspect"));

    if (graph->root().string({"asset"}) != "scratch/control" ||
        *graph->root().at("graph") != *empty->root().at("graph") || graph->root().at("nodes")->size() != 0 ||
        !(graph->root().at("node_types")->size() > 0) || graph->root().boolean({"can_undo"}) != false)
        return fixture_failure("Scratch graph did not open with its own empty history and node catalog");
    ATELIER_TRY_BIND(reopened,
                     driver.ok("graph.create_scratch", json::Document::literal<R"({"key":"scratch/control"})">()));
    if (*reopened != *empty)
        return fixture_failure("Reopening a scratch key changed the graph handle");
    ATELIER_TRY(require_control_error(driver.call("graph.inspect", json::Document::literal<R"({"node":"absent"})">()),
                                      "invalid_request", "Unknown node in procedural graph: absent"));
    ATELIER_TRY(
        require_control_error(driver.call("graph.set_properties", json::Document::literal<R"({"properties":[]})">()),
                              "invalid_request", "properties must be a JSON object"));

    // A valid scalar precedes two bad fields. The first bad field owns the diagnostic.
    ATELIER_TRY(require_control_error(
        driver.call(
            "graph.set_properties",
            json::Document::literal<R"({"node":"","properties":{"seed":12345,"first_bad":0,"later_bad":0}})">()),
        "operation_failed", "first_bad: Unknown field."));
    ATELIER_TRY_BIND(rejected, driver.ok("graph.inspect"));
    if (*rejected->root().at("seed") != *graph->root().at("seed") || rejected->root().boolean({"can_undo"}) != false)
        return fixture_failure("Rejected graph-root patch changed seed or created history");

    ATELIER_TRY(
        driver.op("graph.add_node", json::Document::literal<R"({"type":"procedural.Grid","name":"points"})">()));
    ATELIER_TRY(driver.op("graph.add_node",
                          json::Document::literal<R"({"type":"procedural.CopyToPoints","name":"copies"})">()));
    ATELIER_TRY(
        driver.op("graph.connect",
                  json::Document::literal<
                      R"({"from_node":"points","from_port":"output","to_node":"copies","to_port":"points"})">()));
    const auto patch = [&](std::string_view mesh, std::string_view variant) -> Result<void> {
        json::Builder args;
        args.set_validated(args.root(), "node", "copies");
        const auto props = args.object_validated(args.root(), "properties");
        const auto meshes = *args.array(props, "meshes");
        if (!args.append(meshes, mesh))
            return fixture_failure("Invalid graph fixture mesh");
        args.set_validated(props, "variant", variant);
        ATELIER_TRY(driver.ok("graph.set_properties", args.finish()));

        return {};
    };
    const auto query = json::Document::literal<R"({"asset":"scratch/control","node":"copies","samples":0})">();
    ATELIER_TRY_BIND(original, driver.ok("graph.inspect", query));
    const auto nodes = *original->root().at("nodes");
    std::optional<json::Value> copies;
    for (const auto node : *nodes.array())
        if (node.string({"name"}) == "copies")
            copies = node;
    const auto links = *original->root().at("connections");
    if (nodes.size() != 2 || !copies || links.size() != 1 || links.at(0)->string({"from_node"}) != "points" ||
        links.at(0)->string({"to_node"}) != "copies")
        return fixture_failure("Graph topology lost the authored nodes or connection");
    const auto detail_query = object_query(*copies);
    const auto check = [&](std::string_view mesh, std::string_view variant, bool can_redo) -> Result<void> {
        ATELIER_TRY_BIND(view, driver.ok("graph.inspect", query));
        if (!view->root().at("inspected_error")->is_null())
            return fixture_failure(view->root().string({"inspected_error"}).value_or("Cannot inspect graph output"));
        const auto references = *view->root().at("inspected_summary")->at("instance_references");
        const auto observed = json::write(references);
        if (references.size() != 1 || references.at(0)->string({"mesh"}) != mesh ||
            !(references.at(0)->u64({"count"}).value_or(0) > 0))
            return fixture_failure("Expected instances of " + std::string(mesh) + ", got " +
                                   std::string(observed.data(), observed.size()));
        if (view->root().boolean({"can_undo"}) != true || view->root().boolean({"can_redo"}) != can_redo)
            return fixture_failure("Graph patch displaced the undo or redo entry");
        ATELIER_TRY_BIND(node, driver.ok("editor.inspect", detail_query));

        bool found{};
        for (const auto field : *node->root().at("object")->at("properties")->array())
            if (field.string({"name"}) == "variant" && field.string({"value"}) == variant)
                found = true;
        if (!found)
            return fixture_failure("Graph patch lost the reflected String value");

        return {};
    };
    ATELIER_TRY(patch("builtin/sphere", "original"));
    ATELIER_TRY(patch("builtin/cube", "committed"));
    ATELIER_TRY(check("builtin/cube", "committed", false));
    const auto bad = json::Document::literal<
        R"({"node":"copies","properties":{"meshes":["builtin/sphere"],"variant":"rejected","first_bad":1,"later_bad":2}})">();
    const auto reject = [&]() -> Result<void> {
        return require_control_error(driver.call("graph.set_properties", bad), "operation_failed",
                                     "first_bad: Unknown field.");
    };
    ATELIER_TRY(reject());
    ATELIER_TRY(check("builtin/cube", "committed", false));
    const auto context = json::Document::literal<R"({"asset":"scratch/control"})">();
    ATELIER_TRY(driver.op("edit.undo", {}, context));
    ATELIER_TRY(reject());
    ATELIER_TRY(check("builtin/sphere", "original", true));
    ATELIER_TRY(driver.op("edit.redo", {}, context));
    ATELIER_TRY(check("builtin/cube", "committed", false));

    return {};
}

Result<void> verify_operator_requests(Driver& driver, json::Value entity) {
    ATELIER_TRY_BIND(catalog, driver.ok("op.list"));

    bool found_add{};
    for (const auto op : *catalog->root().at("operators")->array())
        if (op.string({"id"}) == "scene.add")
            found_add =
                op.string({"parameters", "title"}) == "editor.SceneAdd" && op.string({"result", "type"}) == "object";
    if (!found_add)
        return fixture_failure("Operator list lost the registered scene.add schemas");
    const auto limit =
        catalog->root().u64({"context", "properties", "selection", "properties", "entities", "maxItems"});
    if (limit != editor::Session::maximum_entities)
        return fixture_failure("Operator list published the wrong selection limit");

    json::Builder context(json::Document::literal<R"({"selection":{"entities":[]},"region":"viewport"})">());
    const auto selection = *context.root().at("selection");
    context.set_validated(selection, "active", entity);
    ATELIER_TRY(require_control_error(driver.try_op("app.about", {}, context.finish()), "invalid_request",
                                      "The active Entity must be selected."));
    const auto entities = *selection.at("entities");
    context.append(entities, entity);
    ATELIER_TRY_BIND(selected, driver.op("scene.select", {}, context.finish()));
    if (selected->root().string({"status"}) != "finished" || selected->root().string({"operator"}) != "scene.select" ||
        *selected->root().at("active") != entity || *selected->root().at("selection") != entities)
        return fixture_failure("Operator did not apply its explicit selection");

    ATELIER_TRY(require_control_error(driver.try_op("scene.discard_saved_data", {}, context.finish()), "poll_failed",
                                      "The inspected object has no saved data."));
    ATELIER_TRY(require_control_error(driver.try_op("app.about", {}), "interaction_required",
                                      "Operator app.about requires human interaction."));
    ATELIER_TRY(
        require_control_error(driver.try_op("scene.parent_set", json::Document::literal<R"({"parent":"missing"})">()),
                              "operation_failed", "Object handle is no longer a document member"));

    context.set_validated(context.root(), "region", "missing");
    const auto bad_args = json::Document::literal<"[]">();
    ATELIER_TRY(require_control_error(driver.try_op("missing.operator", bad_args, context.finish()), "unknown_operator",
                                      "No editor operator has this ID"));
    ATELIER_TRY(require_control_error(driver.try_op("scene.select", bad_args, context.finish()), "invalid_request",
                                      "args must be an object"));
    ATELIER_TRY(require_control_error(driver.try_op("scene.select", {}, context.finish()), "invalid_request",
                                      "Unknown region"));
    if (!context.append(entities, "missing"))
        return fixture_failure("Could not append stale handle");
    ATELIER_TRY(require_control_error(driver.try_op("scene.select", {}, context.finish()), "stale_object",
                                      "Object handle is no longer a document member"));
    for (std::size_t i = entities.size(); i <= *limit; ++i)
        context.append_null(entities);
    return require_control_error(driver.try_op("scene.select", {}, context.finish()), "invalid_request",
                                 "Invalid entity selection");
}

Result<void> verify_trace_requests(Driver& driver, json::Value original_shading) {
    ATELIER_TRY(driver.op("view.set", json::Document::literal<R"({"shading":"smooth"})">()));
    ATELIER_TRY(require_control_error(
        driver.call("render.trace", json::Document::literal<R"({"scope":"unknown","timeout_ms":0})">()),
        "invalid_request", "Only scene_lit_post observation is supported"));
    const auto invalid_timeout = json::Document::literal<R"({"timeout_ms":0})">();
    ATELIER_TRY(require_control_error(driver.call("render.trace", invalid_timeout), "not_ready",
                                      "Select Scene Lit before requesting a post-chain trace"));
    ATELIER_TRY(driver.op("view.set", json::Document::literal<R"({"shading":"scene_lit"})">()));
    ATELIER_TRY(require_control_error(driver.call("render.trace", invalid_timeout, "rejected-trace"), "invalid_request",
                                      "Integer outside supported range at /args/timeout_ms"));
    ATELIER_TRY(
        require_control_error(driver.call("job.status", json::Document::literal<R"({"job_id":"rejected-trace"})">()),
                              "unknown_job", "Job expired or belongs to another instance"));
    json::Builder restore;
    restore.set_validated(restore.root(), "shading", original_shading);
    ATELIER_TRY(driver.op("view.set", restore.finish()));

    return {};
}

Result<void> verify_edit_requests(const std::filesystem::path& executable, const std::filesystem::path& root,
                                  const std::filesystem::path& cli) {
    const auto project = root / "MaterialProject";
    const auto directory = root / "MaterialControl";
    ATELIER_TRY_BIND(editor,
                     Process::launch(executable,
                                     L"--new-project " + quote(project) + L" --control-directory " + quote(directory) +
                                         L" --log-directory " + quote(root / "MaterialLogs") + L" --no-activate",
                                     root / "material-editor.log"));
    auto connected = connect_editor(*editor, directory);
    if (!connected)
        return editor->finish(std::unexpected(std::move(connected).error()));
    auto& driver = *connected;
    const auto edit = [&]() -> Result<void> {
        ATELIER_TRY(driver.settle());
        ATELIER_TRY_BIND(created, driver.op("asset.new_material",
                                            json::Document::literal<R"({"path":"Content/Probe.material"})">()));
        const auto asset = *created->root().at("result")->at("asset");
        json::Builder by_asset;
        by_asset.set_validated(by_asset.root(), "asset", asset);
        const auto asset_args = by_asset.finish();
        ATELIER_TRY_BIND(opened, driver.ok("material.inspect", asset_args));
        if (opened->root().string({"role"}) != "instance" || *opened->root().at("asset") != asset)
            return fixture_failure("Material query did not open the created instance");
        json::Builder by_object;
        by_object.set_validated(by_object.root(), "object", *opened->root().at("handle"));
        const auto object_args = by_object.finish();
        const auto inspect = [&] { return driver.ok("material.inspect", object_args); };
        ATELIER_TRY_BIND(inspection, inspect());
        if (*inspection != *opened)
            return fixture_failure("Asset and object queries disagree about the same material");

        const auto entry_args = [&](const json::Document& entry) {
            json::Builder args(object_args);
            args.set_validated(args.root(), "entry", entry.root());
            return args.finish();
        };
        const auto toggle = [&](std::string_view path, bool enabled) -> Result<json::Document> {
            json::Builder args(object_args);
            args.set_validated(args.root(), "path", path);
            if (!args.set(args.root(), "enabled", enabled))
                return fixture_failure("Invalid override toggle");
            return args.finish();
        };
        ATELIER_TRY(driver.ok(
            "material.set_entry",
            entry_args(
                json::Document::literal<R"({"path":"roughness","enabled":true,"type":"float","value":0.375})">())));
        ATELIER_TRY_BIND(disable_roughness, toggle("roughness", false));
        ATELIER_TRY(driver.ok("material.override", *disable_roughness));
        ATELIER_TRY_BIND(disabled, inspect());
        const auto local = *disabled->root().at("entries")->at(0);
        if (local.string({"path"}) != "roughness" || local.boolean({"enabled"}) != false ||
            local.f32({"value"}) != .375f || disabled->root().boolean({"dirty"}) != true)
            return fixture_failure("Disabling the override lost the local value or the dirty state");
        ATELIER_TRY_BIND(enable_roughness, toggle("roughness", true));
        ATELIER_TRY(driver.ok("material.override", *enable_roughness));
        const auto dormant = json::Document::literal<
            R"({"path":"normal_texture","enabled":false,"type":"texture","value":{"asset_key":"missing-texture","address":"mirror"}})">();
        ATELIER_TRY(driver.ok("material.set_entry", entry_args(dormant)));
        ATELIER_TRY_BIND(saved, driver.ok("material.save", asset_args));
        ATELIER_TRY_BIND(baseline, inspect());

        const auto file = project / "Content/Probe.material";
        ATELIER_TRY_BIND(disk, read(file));
        if (saved->root().boolean({"saved"}) != true || baseline->root().boolean({"dirty"}) != false ||
            disk->root().at("entries")->size() != 2 || *disk->root().at("entries") != *baseline->root().at("entries"))
            return fixture_failure("Saved material entries differ from the inspected instance");
        bool effective{};
        for (const auto field : *baseline->root().at("declarations")->array())
            if (field.string({"path"}) == "roughness" && field.f32({"value"}) == .375f)
                effective = true;
        if (!effective)
            return fixture_failure("Enabling the override did not publish the effective roughness");

        // Both requests reach the texture reader. Keep the dormant value, history state and saved author intact.
        json::Builder replacement(dormant);
        if (!replacement.set(replacement.root(), "enabled", true))
            return fixture_failure("Invalid texture replacement");
        replacement.set_validated(*replacement.root().at("value"), "asset_key", "missing-replacement");
        const auto reject = [&](const char* op, const json::Document& args, std::string_view key) -> Result<void> {
            ATELIER_TRY(require_control_error(driver.call(op, args), "operation_failed",
                                              "Missing resource or incorrect resource type: " + std::string(key)));
            ATELIER_TRY_BIND(state, driver.ok("editor.inspect", json::Document::literal<R"({"detail":"summary"})">()));
            if (state->root().boolean({"busy"}) != false)
                return fixture_failure("Failed material edit left an active transaction");
            ATELIER_TRY_BIND(inspection, inspect());
            if (*inspection != *baseline)
                return fixture_failure("Failed material edit changed the inspected instance");
            ATELIER_TRY_BIND(contents, read(file));
            if (*contents != *disk)
                return fixture_failure("Failed material edit changed the saved author");

            return {};
        };
        ATELIER_TRY(reject("material.set_entry", entry_args(replacement.finish()), "missing-replacement"));
        ATELIER_TRY_BIND(enable_texture, toggle("normal_texture", true));
        ATELIER_TRY(reject("material.override", *enable_texture, "missing-texture"));
        ATELIER_TRY(driver.op("edit.undo", {}, asset_args));
        ATELIER_TRY_BIND(undone, inspect());
        if (undone->root().at("entries")->size() != 1 ||
            undone->root().at("entries")->at(0)->string({"path"}) != "roughness")
            return fixture_failure("Failed edit displaced the last successful material history entry");
        ATELIER_TRY(driver.op("edit.redo", {}, asset_args));
        ATELIER_TRY_BIND(redone, inspect());
        if (*redone != *baseline)
            return fixture_failure("Redo did not restore the saved material after failed edits");
        json::Builder texture(object_args);
        texture.set_validated(texture.root(), "path", "normal_texture");
        texture.set_validated(texture.root(), "texture", "");
        ATELIER_TRY(driver.ok("material.set_texture", texture.finish()));
        json::Builder discard(object_args);
        discard.set_validated(discard.root(), "path", "roughness");
        ATELIER_TRY(driver.ok("material.discard", discard.finish()));
        ATELIER_TRY(driver.ok("material.save", asset_args));
        ATELIER_TRY_BIND(recovered, inspect());
        ATELIER_TRY_BIND(persisted, read(file));

        const auto entries = *persisted->root().at("entries");
        if (recovered->root().boolean({"dirty"}) != false || entries != *recovered->root().at("entries") ||
            entries.size() != 1 || entries.at(0)->string({"path"}) != "normal_texture" ||
            entries.at(0)->boolean({"enabled"}) != true || entries.at(0)->string({"value", "asset_key"}) != "" ||
            entries.at(0)->string({"value", "address"}) != "mirror")
            return fixture_failure("Editing and saving after a rejected override did not persist the recovered values");

        ATELIER_TRY(driver.op("scene.add", json::Document::literal<R"({"kind":"procedural_generator"})">()));
        ATELIER_TRY_BIND(scene, driver.inspect());
        const auto entity = *scene->root().at("entities")->at(0);
        ATELIER_TRY(verify_operator_requests(driver, *entity.at("handle")));
        ATELIER_TRY(verify_trace_requests(driver, *scene->root().at("view")->at("shading")));
        const auto component = *entity.at("components")->at(0);
        json::Builder slot;
        slot.set_validated(slot.root(), "component", *component.at("handle"));
        slot.set_validated(slot.root(), "slot", "default");
        json::Builder assign(slot.finish());
        assign.set_validated(assign.root(), "asset", asset);
        ATELIER_TRY(driver.ok("material.assign", assign.finish()));
        ATELIER_TRY_BIND(instance, driver.ok("material.create_scene", slot.finish()));
        ATELIER_TRY_BIND(scene_material, driver.ok("material.inspect", *instance));

        if (scene_material->root().boolean({"scene"}) != true || *scene_material->root().at("parent") != asset)
            return fixture_failure("Scene instance did not inherit the assigned shared material");
        assign.set_validated(assign.root(), "asset", "");
        ATELIER_TRY(driver.ok("material.assign", assign.finish()));
        ATELIER_TRY(driver.op("scene.save"));
        ATELIER_TRY(verify_graph_requests(driver));
        return {};
    };
    ATELIER_TRY(editor->finish(edit()));
    ATELIER_TRY(require_transport_error(driver.client.call(json::Document::literal<R"({"op":"capabilities"})">()),
                                        "disconnected", "Control peer disconnected"));
    ATELIER_TRY(require_transport_error(Client::create(driver.descriptor), "stale_instance",
                                        "Editor instance is no longer running"));
    const auto retired_id = utf8_to_utf16_validated(*driver.descriptor.root().string({"instance_id"}));
    ATELIER_TRY(
        require_control_error(run_cli(cli, directory, L"--instance " + retired_id + L" --request missing.json", 1),
                              "stale_instance", "The selected editor instance is unavailable"));
    json::Builder stale(driver.descriptor);
    stale.set_validated(stale.root(), "pipe", "invalid");
    ATELIER_TRY(require_transport_error(Client::create(stale.finish()), "stale_instance",
                                        "Editor instance is no longer running"));

    return {};
}

Result<void> smoke_logs(const std::filesystem::path& executable, const std::filesystem::path& cli,
                        const std::filesystem::path& root) {
    const auto project = root / "Project";
    const auto directory = root / "Control";
    const auto logs = root / "Logs" / "editor";
    const auto root_text = platform::utf16_to_utf8(root.native());
    const auto project_text = platform::utf16_to_utf8(project.generic_wstring());
    if (!root_text || !project_text)
        return fixture_failure("Invalid UTF-16 smoke fixture path");
    std::cout << "Editor log smoke evidence: " << *root_text << '\n' << std::flush;
    const std::filesystem::path example_root(ATELIER_EXAMPLE_PROJECT);
    ATELIER_TRY(acceptance::create_directories(project / "Content"));
    for (const auto* name : {L"Config", L"Source"})
        ATELIER_TRY(acceptance::file_operation([&](auto& error) {
            std::filesystem::copy(example_root / name, project / name, std::filesystem::copy_options::recursive, error);
        }));
    ATELIER_TRY_BIND(config, read(project / "Config/Project.json"));
    const auto startup_scene = config->root().string({"startup_scene"});
    if (!startup_scene)
        return fixture_failure("Examples Config/Project.json is missing startup_scene");
    const std::filesystem::path startup_relative = utf8_to_utf16_validated(*startup_scene);
    auto segment = startup_relative.begin();
    if (segment == startup_relative.end() || *segment != L"Content" || ++segment == startup_relative.end() ||
        segment->native() == L"." || segment->native() == L"..")
        return fixture_failure("Examples startup_scene must reside under Content/<pack>/");
    const auto startup_pack = std::filesystem::path(L"Content") / *segment;
    ATELIER_TRY(acceptance::file_operation([&](auto& error) {
        std::filesystem::copy(example_root / startup_pack, project / startup_pack,
                              std::filesystem::copy_options::recursive, error);
    }));
    ATELIER_TRY_BIND(exists, acceptance::file_exists(logs));
    if (*exists)
        return fixture_failure("Smoke log directory must be fresh");
    ATELIER_TRY_BIND(parent, SmokeLog::open(logs));
    ATELIER_TRY_BIND(editor,
                     Process::launch(executable,
                                     L"--project " + quote(project) + L" --control-directory " + quote(directory) +
                                         L" --log-directory " + quote(logs) + L" --no-activate --gpu-validation",
                                     root / "editor.log"));

    std::optional<SmokeLog> child;
    const auto drive = [&]() -> Result<void> {
        ATELIER_TRY_BIND(instance, discover_editor(*editor, directory));

        ArtifactCli artifacts{cli, directory, std::string(*instance->root().string({"instance_id"})), {}};
        json::Document state;
        {
            ATELIER_TRY_BIND(driver, Driver::connect(*instance));
            ATELIER_TRY_BIND(reply, driver->settle());
            state = *reply;
            artifacts.token = std::move(driver->token);
        }
        if (state.root().string({"project"}) != *project_text ||
            *state.root().at("scene_path") != *config->root().at("startup_scene") ||
            state.root().at("last_frame")->size() == 0)
            return fixture_failure("Examples did not open with its startup scene and an accepted frame");
        ATELIER_TRY(verify_capture(artifacts));
        ATELIER_TRY(verify_repro(artifacts, project));
        ATELIER_TRY_BIND(d, Driver::connect(*instance));

        d->token = std::move(artifacts.token);
        ATELIER_TRY(verify_repro_open(*d, project));
        const auto scene_path = project / startup_relative;
        ATELIER_TRY_BIND(scene, read(scene_path));
        const auto original_count = scene->root().at("entities")->size();
        if (state.root().at("entities")->size() != original_count)
            return fixture_failure("Example scene membership did not load");
        ATELIER_TRY(d->op("scene.add", json::Document::literal<R"({"kind":"entity"})">()));
        ATELIER_TRY(d->op("scene.save"));
        ATELIER_TRY_BIND(saved, read(scene_path));
        if (saved->root().at("entities")->size() != original_count + 1)
            return fixture_failure("Saving the edited example lost the added Entity");
        ATELIER_TRY(d->op("edit.undo"));
        ATELIER_TRY_BIND(restored, d->settle());
        const auto initial_frame = state.root().u64({"last_frame", "frame_id"});
        const auto restored_frame = restored->root().u64({"last_frame", "frame_id"});
        if (restored->root().at("entities")->size() != original_count || !initial_frame || !restored_frame ||
            !(*restored_frame > *initial_frame))
            return fixture_failure("Undo did not restore example membership with an accepted frame");
        ATELIER_TRY(d->op("scene.save")); // Keep normal close noninteractive after undoing the saved edit.
        ATELIER_TRY(parent->poll());
        ATELIER_TRY(d->op("play.start"));
        const auto deadline = std::chrono::steady_clock::now() + 125s;
        bool started{};
        do {
            ATELIER_TRY(parent->poll());
            if (!child && parent->count("play.run_attached")) {
                ATELIER_TRY_BIND(play_run_attached_record, parent->one("play.run_attached"));
                ATELIER_TRY_BIND(directory_field, smoke_field<std::string>(**play_run_attached_record, "directory"));
                const auto path = std::filesystem::path(utf8_to_utf16_validated(*directory_field));
                if (path.parent_path() != logs)
                    return fixture_failure("Play log is outside this Editor run");
                ATELIER_TRY_ASSIGN(child, SmokeLog::open(path));
            }
            if (child) {
                ATELIER_TRY(child->poll());
                started = child->count("runner.started") == 1;
            }
            if (WaitForSingleObject(editor->process.get(), 0) != WAIT_TIMEOUT || parent->count("play.process_ended"))
                return fixture_failure("Editor or Play exited before the requested close");
            if (started)
                break;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        if (!started)
            return fixture_failure("Play did not publish runner.started");
        // Driver releases its pipe before WM_CLOSE; Play intentionally remains active.
        return {};
    };
    ATELIER_TRY(editor->finish(drive()));
    ATELIER_TRY(verify_smoke_logs(*parent, *child));
    // Keep four previous successful runs alongside this one. Unmarked failure evidence stays in place.
    ATELIER_TRY(prune_passed_smoke_runs(root.parent_path(), 4));
    ATELIER_TRY(acceptance::file_operation([&](auto& error) { return std::filesystem::remove_all(project, error); }));
    ATELIER_TRY(editor::write_file(root / "passed", "", editor::WriteMode::create));
    std::cout << "Editor log smoke: PASS (edit/save/undo, accepted frame, zero errors, Play close) | " << *root_text
              << '\n';

    return {};
}
}

Result<void> run_acceptance(int argc, wchar_t** argv) {
    if (argc != 5 || std::wstring_view(argv[4]) != L"--smoke-logs")
        return fixture_failure(
            "Usage: atelier_editor_control_acceptance editor.exe control.exe output-directory --smoke-logs");
    if (!GetEnvironmentVariableW(L"ATELIER_SHADER_CACHE", nullptr, 0)) {
        ATELIER_TRY_BIND(executable, acceptance::file_operation(
                                         [&](auto& error) { return std::filesystem::absolute(argv[1], error); }));
        if (!SetEnvironmentVariableW(L"ATELIER_SHADER_CACHE", (executable->parent_path() / L"shader-cache").c_str()))
            return system_failure("Cannot select shader cache");
    }
    ATELIER_TRY_BIND(
        base, acceptance::file_operation([&](auto& error) { return std::filesystem::absolute(argv[3], error); }));

    const auto id = unique_id();
    if (!id)
        return std::unexpected(transport_error(id.error()));
    const auto root = *base / *id;
    ATELIER_TRY(acceptance::create_directories(root));
    ATELIER_TRY(verify_host_failures(argv[1], root / "HostFailures"));
    ATELIER_TRY(verify_inspect_read_errors(root / "InspectReadErrors", argv[2]));
    ATELIER_TRY(verify_edit_requests(argv[1], root, argv[2]));
    return smoke_logs(argv[1], argv[2], root);
}

int wmain(int argc, wchar_t** argv) {
    atelier::install_fatal_handler();
    atelier::application::install_frame_timing();
    auto result = run_acceptance(argc, argv);
    if (!result) {
        std::cerr << "Editor log smoke: FAIL: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
