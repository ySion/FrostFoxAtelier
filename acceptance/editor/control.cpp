#include "memory/fatal.hpp"
#include "control/control.hpp"
#include "logging/logging.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace atelier;

using namespace atelier::control;

namespace platform = atelier::platform;

using atelier::platform::utf8_to_utf16_validated;

using namespace std::chrono_literals;

void require(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}

json::Document read(const std::filesystem::path& path) {
    const auto native = platform::windows_file_path(path);
    require(bool(native), native ? "" : platform::error_message(native.error()));
    std::ifstream stream(*native, std::ios::binary);
    const auto text = platform::utf16_to_utf8(path.native());
    require(bool(text), "Invalid UTF-16 E2E artifact path");
    require(bool(stream), "Cannot read E2E artifact: " + *text);
    const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    auto parsed = json::Document::parse(bytes, {32, json::DepthPolicy::containers, true});
    require(bool(parsed), parsed ? "" : parsed.error().describe());
    return std::move(*parsed);
}

std::wstring quote(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

struct Process {
    HANDLE process{};

    DWORD pid{};

    HANDLE log{};

    Process(const std::filesystem::path& executable, std::wstring arguments, const std::filesystem::path& output) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        log = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        require(log != INVALID_HANDLE_VALUE, "Cannot create child log");
        auto command = quote(executable) + L" " + arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                            executable.parent_path().c_str(), &startup, &created)) {
            CloseHandle(log);
            throw std::runtime_error("Cannot launch control E2E child");
        }
        process = created.hProcess;
        pid = created.dwProcessId;
        CloseHandle(created.hThread);
    }

    ~Process() {
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 99);
                WaitForSingleObject(process, 10000);
            }
            CloseHandle(process);
        }
        if (log && log != INVALID_HANDLE_VALUE)
            CloseHandle(log);
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

    void close() {
        if (auto handle = window())
            PostMessageW(handle, WM_CLOSE, 0, 0);
        require(WaitForSingleObject(process, 300000) == WAIT_OBJECT_0, "Editor did not close cleanly");
        DWORD code{};
        GetExitCodeProcess(process, &code);
        require(code == 0, "Editor exited with a failure");
    }
};

struct Driver {
    std::unique_ptr<Client> client;

    std::string token;

    json::Document descriptor;

    explicit Driver(json::Document instance) : descriptor(std::move(instance)) {
        auto connected = Client::create(descriptor, 600s);
        require(bool(connected), connected ? "" : connected.error().what());
        client = std::make_unique<Client>(std::move(*connected));
    }

    json::Document call(std::string op, const json::Document& args = {}, std::string id = {}) {
        const auto operator_id = args.root().string({"id"});
        const auto name = op == "op.call" && operator_id ? op + " " + std::string{*operator_id} : op;
        json::Builder request;
        require(request.set(request.root(), "op", op) && request.set(request.root(), "args", args.root()) &&
                    request.set(request.root(), "state_token", token),
                "Invalid Control request fields");
        if (!id.empty())
            require(bool(request.set(request.root(), "request_id", id)), "Invalid Control request ID");
        auto result = client->call(request.finish());
        require(bool(result), result ? "" : result.error().what());
        if (result->root().string({"status"}) == "error")
            std::cerr << "Control request failed: " << name << '\n';
        if (auto value = result->root().string({"state_token"}))
            token = *value;
        return std::move(*result);
    }

    json::Document ok(std::string op, const json::Document& args = {}) {
        auto result = call(std::move(op), args);
        const auto bytes = json::write(result.root());
        require(result.root().string({"status"}) == "ok", {bytes.data(), bytes.size()});
        return json::Document{*result.root().at("result")};
    }

    json::Document inspect() { return ok("editor.inspect"); }

    // Material preparation does not change the token. Wait for an accepted frame as well as
    // five unchanged inspections before using the editor state, including on a cold shader cache.
    json::Document settle() {
        const auto deadline = std::chrono::steady_clock::now() + 125s;
        auto state = inspect();
        unsigned stable{};
        while (stable < 5 && std::chrono::steady_clock::now() < deadline) {
            const auto before = token;
            std::this_thread::sleep_for(30ms);
            state = inspect();
            stable = before == token && state.root().at("last_frame")->size() != 0 ? stable + 1 : 0;
        }
        require(stable == 5, "Editor accepted frame did not arrive with a stable state token within 125s");
        return state;
    }

    json::Document op(std::string id, const json::Document& args = {}, const json::Document& context = {}) {
        auto result = try_op(std::move(id), args, context);
        const auto bytes = json::write(result.root());
        require(result.root().string({"status"}) == "ok", {bytes.data(), bytes.size()});
        return json::Document{*result.root().at("result")};
    }

    json::Document try_op(std::string id, const json::Document& args, const json::Document& context = {}) {
        json::Builder request;
        require(request.set(request.root(), "id", id) && request.set(request.root(), "args", args.root()) &&
                    request.set(request.root(), "context", context.root()),
                "Invalid Operator fields");
        return call("op.call", request.finish());
    }
};

json::Document discover_editor(const Process& editor, const std::filesystem::path& directory) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto instances = discover(directory);
        require(bool(instances), instances ? "" : instances.error().what());
        for (const auto instance : *instances->root().array())
            if (instance.u32({"pid"}) == editor.pid)
                return json::Document{instance};
        require(WaitForSingleObject(editor.process, 0) == WAIT_TIMEOUT,
                "Editor stopped before publishing its endpoint");
        std::this_thread::sleep_for(30ms);
    }
    throw std::runtime_error("Editor did not publish its endpoint");
}

struct SmokeLog {
    std::filesystem::path directory;

    std::string directory_text;

    std::unique_ptr<atelier::logging::Reader> reader;

    atelier::logging::ReadBatch latest;

    std::vector<atelier::logging::Record> records;

    explicit SmokeLog(std::filesystem::path path) : directory(std::move(path)) {
        auto text = platform::utf16_to_utf8(directory.native());
        require(bool(text), "Invalid UTF-16 smoke log directory");
        directory_text = std::move(*text);
        auto opened = atelier::logging::Reader::open(directory);
        require(bool(opened), opened ? "" : opened.error().message);
        reader = std::move(*opened);
    }

    void check(bool value, std::string_view message) const {
        require(value, directory_text + ": " + std::string(message));
    }

    void poll() {
        latest = reader->poll();
        for (auto& record : latest.records) {
            check(record.entry.severity < atelier::logging::Severity::error,
                  "run=" + record.run_id + " pid=" + std::to_string(record.process_id) +
                      " record=" + std::to_string(record.record_sequence) + " " + record.entry.event_code + ": " +
                      record.entry.message);
            records.push_back(std::move(record));
        }
        latest.records.clear();
    }

    std::size_t count(std::string_view code) const {
        return std::ranges::count_if(records, [&](const auto& record) { return record.entry.event_code == code; });
    }

    const atelier::logging::Record& one(std::string_view code) const {
        check(count(code) == 1, "Expected exactly one " + std::string(code));
        return *std::ranges::find_if(records, [&](const auto& record) { return record.entry.event_code == code; });
    }

    // Only called after an OS exit, or the parent's OS-backed process_ended event.
    void finish() {
        reader->source_ended();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            poll();
            if (latest.status.finished)
                break;
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        const auto& status = latest.status;
        if (status.latest_issue) {
            const auto text = platform::utf16_to_utf8(status.latest_issue->file.native());
            require(bool(text), "Invalid UTF-16 log issue path");
            check(false,
                  *text + ":" + std::to_string(status.latest_issue->offset) + " " + status.latest_issue->message);
        }
        check(latest.run && status.finished && status.has_final_summary && !status.incomplete &&
                  !status.waiting_for_run && !status.partial_line && !status.unquantified_loss &&
                  !status.invalid_lines && !status.invalid_metadata && !status.oversized_lines &&
                  !status.incomplete_lines && !status.rotation_losses && !status.sequence_gaps &&
                  !status.duplicate_records && !status.out_of_order_records && !status.io_failures,
              "Formal log is missing, incomplete or corrupt");
        check(latest.health && latest.health->final, "Missing final=true health");
        const auto& report = latest.health->report;
        const auto& health = report.health;
        check(health.state == atelier::logging::RunState::finished && report.backend_acknowledged &&
                  report.sink_flush_succeeded && report.summary_published && !health.rejected_queue_full &&
                  !health.rejected_producer_limit && !health.rejected_record_too_large &&
                  !health.rejected_invalid_record && !health.rejected_internal_failure && !health.facility_failures &&
                  !health.backend_notifications && health.first_backend_notification.empty() &&
                  !health.first_facility_issue && !health.first_error,
              "Final logging health reports an error or facility fault");
        one("application.started");
        one("application.finished");
    }
};

template <class T>
const T& smoke_field(const atelier::logging::Record& record, std::string_view key) {
    for (const auto& field : record.entry.fields)
        if (field.key == key)
            if (const auto* value = std::get_if<T>(&field.value))
                return *value;
    throw std::runtime_error("run=" + record.run_id + " record=" + std::to_string(record.record_sequence) + " " +
                             record.entry.event_code + ": missing or mistyped field " + std::string(key));
}

void verify_smoke_logs(SmokeLog& parent, SmokeLog& child) {
    parent.finish();
    const auto& ended = parent.one("play.process_ended");
    parent.check(!smoke_field<bool>(ended, "forced") && smoke_field<std::uint64_t>(ended, "exit_code") == 0,
                 "Play did not end normally when its Editor closed");
    child.finish();
    child.one("runner.stop_observed");
    child.one("runner.stopped");
}

void smoke_logs(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto project = root / "Project";
    const auto directory = root / "Control";
    const auto logs = root / "Logs" / "editor";
    const auto root_text = platform::utf16_to_utf8(root.native());
    const auto project_text = platform::utf16_to_utf8(project.generic_wstring());
    require(root_text && project_text, "Invalid UTF-16 smoke fixture path");
    std::cout << "Editor log smoke evidence: " << *root_text << '\n' << std::flush;
    std::filesystem::create_directories(project);
    for (const auto* name : {L"Config", L"Source", L"Content"})
        std::filesystem::copy(std::filesystem::path(ATELIER_EXAMPLE_PROJECT) / name, project / name,
                              std::filesystem::copy_options::recursive);
    require(!std::filesystem::exists(logs), "Smoke log directory must be fresh");
    Process editor(executable,
                   L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --log-directory " +
                       quote(logs) + L" --no-activate --gpu-validation",
                   root / "editor.log");
    SmokeLog parent(logs);
    std::unique_ptr<SmokeLog> child;
    std::exception_ptr failure;
    try {
        Driver d(discover_editor(editor, directory));
        const auto state = d.settle();
        const auto config = read(project / "Config/Project.json");
        require(state.root().string({"project"}) == *project_text &&
                    *state.root().at("scene_path") == *config.root().at("startup_scene") &&
                    state.root().at("last_frame")->size() != 0,
                "Examples did not open with its startup scene and an accepted frame");
        const auto scene_path = project / utf8_to_utf16_validated(*config.root().string({"startup_scene"}));
        const auto scene = read(scene_path);
        const auto original_count = scene.root().at("entities")->size();
        require(state.root().at("entities")->size() == original_count, "Example scene membership did not load");
        d.op("scene.add", json::Document::literal<R"({"kind":"entity"})">());
        d.op("scene.save");
        const auto saved = read(scene_path);
        require(saved.root().at("entities")->size() == original_count + 1,
                "Saving the edited example lost the added Entity");
        d.op("edit.undo");
        const auto restored = d.settle();
        const auto initial_frame = state.root().u64({"last_frame", "frame_id"});
        const auto restored_frame = restored.root().u64({"last_frame", "frame_id"});
        require(restored.root().at("entities")->size() == original_count && initial_frame && restored_frame &&
                    *restored_frame > *initial_frame,
                "Undo did not restore example membership with an accepted frame");
        d.op("scene.save"); // Keep normal close noninteractive after undoing the saved edit.
        parent.poll();
        d.op("play.start");
        const auto deadline = std::chrono::steady_clock::now() + 125s;
        bool started{};
        do {
            parent.poll();
            if (!child && parent.count("play.run_attached")) {
                const auto path = std::filesystem::path(
                    utf8_to_utf16_validated(smoke_field<std::string>(parent.one("play.run_attached"), "directory")));
                require(path.parent_path() == logs, "Play log is outside this Editor run");
                child = std::make_unique<SmokeLog>(path);
            }
            if (child) {
                child->poll();
                started = child->count("runner.started") == 1;
            }
            require(WaitForSingleObject(editor.process, 0) == WAIT_TIMEOUT && !parent.count("play.process_ended"),
                    "Editor or Play exited before the requested close");
            if (started)
                break;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        require(started, "Play did not publish runner.started");
        // Driver releases its pipe before WM_CLOSE; Play intentionally remains active.
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        editor.close();
    } catch (const std::exception& error) {
        if (!failure)
            failure = std::current_exception();
        else
            std::cerr << "Smoke cleanup also failed: " << error.what() << '\n';
    }
    if (failure)
        std::rethrow_exception(failure);
    verify_smoke_logs(parent, *child);
    std::cout << "Editor log smoke: PASS (edit/save/undo, accepted frame, zero errors, Play close) | " << *root_text
              << '\n';
}
}

int wmain(int argc, wchar_t** argv) {
    atelier::install_fatal_handler();
    try {
        require(argc == 4 && std::wstring_view(argv[3]) == L"--smoke-logs",
                "Usage: atelier_editor_control_acceptance editor.exe output-directory --smoke-logs");
        if (!GetEnvironmentVariableW(L"ATELIER_SHADER_CACHE", nullptr, 0))
            SetEnvironmentVariableW(L"ATELIER_SHADER_CACHE",
                                    (std::filesystem::absolute(argv[1]).parent_path() / L"shader-cache").c_str());
        const auto root = std::filesystem::absolute(argv[2]) / unique_id();
        std::filesystem::create_directories(root);
        smoke_logs(argv[1], root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Editor log smoke: FAIL: " << error.what() << '\n';
        return 1;
    }
}
