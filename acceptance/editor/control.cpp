#include "memory/fatal.hpp"
#include "control/control.hpp"
#include "logging/logging.hpp"
#include "foundation/sha256.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <array>

namespace {
using namespace atelier::control;

namespace platform = atelier::platform;

using atelier::foundation::sha256;

using atelier::foundation::sha256_file;

using atelier::platform::utf8_to_utf16_validated;

using namespace std::chrono_literals;

void require(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}

Json read(const std::filesystem::path& path) {
    const auto native = platform::windows_file_path(path);
    require(bool(native), native ? "" : platform::error_message(native.error()));
    std::ifstream stream(*native, std::ios::binary);
    const auto text = platform::utf16_to_utf8(path.native());
    require(bool(text), "Invalid UTF-16 E2E artifact path");
    require(bool(stream), "Cannot read E2E artifact: " + *text);
    return Json::parse(stream);
}

void write(const std::filesystem::path& path, std::string_view text) {
    const auto native = platform::windows_file_path(path);
    require(bool(native), native ? "" : platform::error_message(native.error()));
    std::filesystem::create_directories(native->parent_path());
    std::ofstream stream(*native, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(bool(stream), "Cannot write E2E fixture");
}

// A 1x1 RGBA PNG.
constexpr std::array<unsigned char, 70> tiny_png{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x64, 0xf8, 0xcf, 0x50, 0x0f, 0x00, 0x03, 0x86, 0x01, 0x80,
    0x5a, 0x34, 0x7d, 0x6b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

constexpr std::string_view published_code =
    "class project.ControlProbe : world.Component { float speed = 1.0f; void tick(float dt) { self.translate_owner(self.speed * dt, 0.0f, 0.0f); } }\n";

constexpr std::string_view published_ui =
    "ui ProjectUI(in Text title, in double elapsed, in double entities) { Column hud { Text title { text: input.title; } } }\n";

void project_fixture(const std::filesystem::path& project) {
    write(project / "Content/Scenes/Main.scene", Json{{"version", 10},
                                                      {"next_id", "1"},
                                                      {"roots", Json::array()},
                                                      {"entities", Json::array()},
                                                      {"materials", Json::array()}}
                                                     .dump());
    write(project / "Source/Published.source", published_code);
    write(project / "Source/Published.ui.source", published_ui);
    std::filesystem::create_directories(project / "Cache");
    write(project / "Config/Project.json", Json{{"version", 1},
                                                {"name", "Control review"},
                                                {"startup_scene", "Content/Scenes/Main.scene"},
                                                {"ui_source", "Source/Published.ui.source"}}
                                               .dump());
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
        require(WaitForSingleObject(process, 10000) == WAIT_OBJECT_0, "Editor did not close cleanly");
        DWORD code{};
        GetExitCodeProcess(process, &code);
        require(code == 0, "Editor exited with a failure");
    }
};

struct Driver {
    std::unique_ptr<Client> client;

    std::string token;

    Json descriptor;

    explicit Driver(Json instance) : descriptor(std::move(instance)) {
        auto connected = Client::create(descriptor, 600s);
        require(bool(connected), connected ? "" : connected.error().what());
        client = std::make_unique<Client>(std::move(*connected));
    }

    Json call(std::string op, Json args = Json::object(), std::string id = {}) {
        const auto name = op == "op.call" && args.is_object() && args.contains("id") && args.at("id").is_string()
                              ? op + " " + args.at("id").get_ref<const std::string&>()
                              : op;
        Json request{{"op", std::move(op)}, {"args", std::move(args)}, {"state_token", token}};
        if (!id.empty())
            request["request_id"] = id;
        auto result = client->call(std::move(request));
        if (result.value("status", "") == "error")
            std::cerr << "Control request failed: " << name << '\n';
        if (result.contains("state_token"))
            token = result.at("state_token").get<std::string>();
        return result;
    }

    Json ok(std::string op, Json args = Json::object()) {
        auto result = call(std::move(op), std::move(args));
        require(result.value("status", "") == "ok", result.dump());
        return result.at("result");
    }

    Json inspect() { return ok("editor.inspect"); }

    // A new editor records its surface only after serving that step's requests, so the token of a
    // first inspection can be stale by the next call. Five unchanged inspections, one per step, settle it.
    Json settle() {
        auto state = inspect();
        unsigned stable{};
        for (unsigned i = 0; i < 200 && stable < 5; ++i) {
            const auto before = token;
            std::this_thread::sleep_for(30ms);
            state = inspect();
            stable = before == token ? stable + 1 : 0;
        }
        require(stable == 5, "The editor state token did not settle after start");
        return state;
    }

    Json op(std::string id, Json args = Json::object(), Json context = Json::object()) {
        return ok("op.call", {{"id", std::move(id)}, {"args", std::move(args)}, {"context", std::move(context)}});
    }

    Json try_op(std::string id, Json args, Json context = Json::object()) {
        return call("op.call", {{"id", std::move(id)}, {"args", std::move(args)}, {"context", std::move(context)}});
    }

    Json wait(std::string id, std::chrono::seconds timeout = 125s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto result = call("job.status", {{"job_id", id}});
            if (result.at("status") != "pending")
                return result;
            std::this_thread::sleep_for(20ms);
        }
        throw std::runtime_error("Control job did not complete");
    }
};

void verify_post_trace(Driver& d) {
    const auto before = d.inspect();
    const auto& view = before.at("view");
    // One representative, two states. Expectations come from the recorded
    // shader contract, not from trace formatting or the observation callback.
    const std::array<std::vector<std::string>, 2> entries{
        {{"lit_taa_cs", "lit_bloom_downsample_cs", "lit_bloom_blur_cs", "lit_bloom_blur_cs", "lit_tonemap_cs",
          "lit_smaa_edges_cs", "lit_smaa_weights_cs", "lit_smaa_neighborhood_cs", "lit_smaa_resolve_cs"},
         {"lit_taa_cs", "lit_tonemap_cs"}}};
    for (std::size_t state = 0; state < entries.size(); ++state) {
        d.op("view.set",
             {{"shading", "scene_lit"}, {"bloom", state == 0}, {"antialiasing", state == 0 ? "smaa_t2x" : "off"}});
        const auto selected = d.inspect();
        const auto id = unique_id();
        auto pending = d.call("render.trace", {{"scope", "scene_lit_post"}}, id);
        require(pending.at("status") == "pending", pending.dump());
        const auto finished = d.wait(id);
        require(finished.at("status") == "ok", finished.dump());
        const auto& result = finished.at("result");
        const auto trace = read(utf8_to_utf16_validated(result.at("json").get<std::string>()));
        require(trace.at("schema") == "atelier.render.trace" && trace.at("version") == 1 &&
                    trace.at("observation").at("status") == "complete" && trace.at("frame_status") == "accepted" &&
                    trace.at("attempt") == "first_requested_view" && trace.at("ui_frame_id") != 0 &&
                    trace.at("recording_sequence") != 0,
                "Post trace did not preserve the complete actual render attempt: " + trace.dump());
        const auto& commands = trace.at("commands");
        require(commands.size() == entries[state].size(), "Post trace has a missing or invented dispatch");
        std::ifstream page(utf8_to_utf16_validated(result.at("html").get<std::string>()), std::ios::binary);
        require(bool(page), "Trace HTML is missing");
        const std::string html{std::istreambuf_iterator<char>(page), std::istreambuf_iterator<char>()};
        std::size_t position{};
        for (std::size_t i = 0; i < commands.size(); ++i) {
            const auto& command = commands[i];
            require(command.at("ordinal") == i + 1 && command.at("entry") == entries[state][i] &&
                        command.at("kind") == "dispatch" && command.at("groups")[0] > 0 &&
                        command.at("groups")[1] > 0 && command.at("groups")[2] == 1,
                    "Post trace dispatch order or dimensions differ from the actual chain");
            position = html.find("id=\"command-" + std::to_string(i + 1) + "\"", position);
            require(position != std::string::npos, "Trace SVG omitted or reordered a command");
            const auto end = html.find("</g>", position);
            require(html.substr(position, end - position).find(entries[state][i]) != std::string::npos,
                    "Trace SVG command label differs from its observation");
            position = end;
            std::set<std::pair<unsigned, unsigned>> slots;
            for (const auto& binding : command.at("bindings")) {
                require(binding.at("status") == "valid" && binding.at("kind") != "unsupported",
                        "Normal post chain has a dummy or unobserved binding");
                require(slots
                            .emplace(binding.at("root_parameter").get<unsigned>(),
                                     binding.at("array_index").get<unsigned>())
                            .second,
                        "Post trace repeated a root binding instead of resolving installed state");
            }
        }
        const auto resource = [&](std::size_t command, std::string_view name, std::string_view kind) -> Json {
            for (const auto& binding : commands.at(command).at("bindings"))
                if (binding.at("name").get_ref<const std::string&>() == name) {
                    require(binding.at("kind").get_ref<const std::string&>() == kind &&
                                binding.at("resource").is_number_unsigned() &&
                                binding.at("view").at("first_mip") == 0 && binding.at("view").at("mip_count") == 1,
                            "Post trace lost the actual texture view");
                    const auto start =
                        html.find("id=\"binding-" + std::to_string(command + 1) + "-" +
                                  binding.at("root_parameter").dump() + "-" + binding.at("array_index").dump() + "\"");
                    require(start != std::string::npos, "Trace SVG omitted a binding port");
                    const auto port = html.substr(start, html.find("</g>", start) - start);
                    require(port.find(name) != std::string::npos && port.find(kind) != std::string::npos &&
                                port.find(">R" + binding.at("resource").dump() + "</text>") != std::string::npos,
                            "Trace SVG binding port differs from the actual resource");
                    return binding.at("resource");
                }
            throw std::runtime_error("Missing actual post binding: " + std::string(name));
        };
        const auto taa = resource(0, "lit_taa_output", "texture_uav");
        require(taa != resource(0, "lit_taa_previous", "texture_srv"), "TAA history aliases the current output");
        const auto tone = state == 0 ? 4u : 1u;
        require(resource(tone, "lit_taa", "texture_srv") == taa, "ToneMap lost the real TAA binding");
        const auto bloom = resource(tone, "lit_bloom", "texture_srv");
        if (state == 0) {
            const auto input = resource(2, "lit_bloom_input", "texture_srv");
            const auto output = resource(2, "lit_bloom_output", "texture_uav");
            require(input != output && input == bloom && resource(1, "lit_taa", "texture_srv") == taa &&
                        resource(1, "lit_bloom_output", "texture_uav") == input &&
                        resource(3, "lit_bloom_input", "texture_srv") == output &&
                        resource(3, "lit_bloom_output", "texture_uav") == input &&
                        commands[2].at("kernel") == commands[3].at("kernel"),
                    "Bloom blur instances lost their actual ping-pong bindings");
            const auto spatial = resource(7, "lit_smaa_spatial_output", "texture_uav");
            require(resource(8, "lit_smaa_spatial", "texture_srv") == spatial &&
                        resource(8, "lit_smaa_previous", "texture_srv") != spatial,
                    "SMAA current and history bindings were conflated");
            for (const auto& binding : commands[5].at("bindings"))
                require(binding.at("name") != "lit_smaa_spatial_output" && binding.at("name") != "lit_smaa_previous",
                        "Trace treated unused SMAA setters as actual bindings");
        }
        const auto after = d.inspect();
        require(after.at("view") == selected.at("view") && after.at("undo") == selected.at("undo"),
                "Tracing changed display or author history");
        std::cout << "Editor Control post trace " << state << ": " << result.at("directory") << '\n';
    }
    // Both Bloom and AA are World author settings. The second state always
    // changes them; the first changes them unless it already matches the base.
    d.op("edit.undo", Json::object(), {{"asset", ""}, {"region", "viewport"}});
    if (!view.at("bloom").get<bool>() || view.at("antialiasing") != "smaa_t2x")
        d.op("edit.undo", Json::object(), {{"asset", ""}, {"region", "viewport"}});
    d.op("view.set", {{"shading", view.at("shading")}});
    const auto after = d.inspect();
    require(after.at("view") == view && after.at("undo") == before.at("undo"),
            "Post trace representative did not restore its settings");
}

Json discover_editor(const Process& editor, const std::filesystem::path& directory) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto instances = discover(directory);
        require(bool(instances), instances ? "" : instances.error().what());
        for (const auto& instance : *instances)
            if (instance.at("pid").get<DWORD>() == editor.pid)
                return instance;
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

void verify_smoke_logs(SmokeLog& parent, SmokeLog& child, const std::filesystem::path& project, DWORD pid) {
    parent.finish();
    const auto& attached = parent.one("play.run_attached");
    const auto& ended = parent.one("play.process_ended");
    const auto child_path = platform::utf16_to_utf8(child.directory.generic_wstring());
    const auto project_path = platform::utf16_to_utf8(project.generic_wstring());
    require(child_path && project_path, "Invalid UTF-16 smoke log paths");
    parent.check(smoke_field<std::string>(attached, "directory") == *child_path &&
                     smoke_field<std::string>(attached, "project_root") == *project_path &&
                     smoke_field<std::string>(ended, "directory") == *child_path &&
                     !smoke_field<bool>(ended, "forced") && smoke_field<std::uint64_t>(ended, "exit_code") == 0,
                 "Play did not end normally in its attached run");
    child.finish();
    const auto& editor_run = *parent.latest.run;
    const auto& runner_run = *child.latest.run;
    parent.check(editor_run.process_name == "editor" && editor_run.process_id == pid && !editor_run.parent_run_id &&
                     editor_run.directory == parent.directory,
                 "Editor manifest does not identify the launched process");
    child.check(runner_run.process_name == "runner" && runner_run.process_id != pid &&
                    runner_run.run_id != editor_run.run_id && runner_run.parent_run_id == editor_run.run_id &&
                    runner_run.directory == child.directory && child.directory.parent_path() == parent.directory,
                "Runner manifest does not identify this Editor's child");
    std::set<std::filesystem::path> runs;
    for (const auto& item : std::filesystem::recursive_directory_iterator(parent.directory))
        if (item.path().filename() == "run.json")
            runs.insert(item.path().parent_path());
    parent.check(runs == std::set{parent.directory, child.directory}, "Unattached or missing product run");
    const auto& started = child.one("runner.started");
    const auto& stopped = child.one("runner.stopped");
    child.check(smoke_field<std::string>(started, "project_root") == *project_path &&
                    !smoke_field<bool>(started, "preview_ui") && child.count("runner.stop_observed") == 1 &&
                    smoke_field<std::uint64_t>(started, "document") == smoke_field<std::uint64_t>(stopped, "document"),
                "Play did not start and observe cooperative stop in the same game document");
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
        require(state.at("project") == *project_text &&
                    state.at("scene_path") == read(project / "Config/Project.json").at("startup_scene") &&
                    !state.at("last_frame").empty(),
                "Examples did not open with its startup scene and an accepted frame");
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
    verify_smoke_logs(parent, *child, project, editor.pid);
    std::cout << "Editor log smoke: PASS | " << *root_text << '\n';
}

void error(const Json& result, std::string_view expected) {
    require(result.value("status", "") == "error" && result.at("error").at("code").get<std::string>() == expected,
            result.dump());
}

Json selection(const std::string& entity) {
    return {{"selection", {{"entities", Json::array({entity})}}}};
}

const Json& material_entry(const Json& material, std::string_view path) {
    const auto& entries = material.at("entries");
    const auto at = std::ranges::find_if(
        entries, [&](const Json& entry) { return entry.at("path").get_ref<const std::string&>() == path; });
    require(at != entries.end(), "Material lacks the sparse author entry " + std::string(path));
    return *at;
}

bool material_issue(const Json& material, std::string_view text) {
    return std::ranges::any_of(material.at("diagnostics"), [&](const Json& issue) {
        return issue.at("message").get<std::string>().find(text) != std::string::npos;
    });
}

void repro_engine_assets(Driver& d, const std::filesystem::path& root) {
    const auto exported = [&] {
        const auto id = unique_id();
        require(d.call("repro.export", Json::object(), id).at("status") == "pending",
                "Engine repro export did not start");
        const auto done = d.wait(id);
        require(done.at("status") == "ok", done.dump());
        return std::filesystem::path(utf8_to_utf16_validated(done.at("result").at("directory").get<std::string>()));
    };
    const auto empty = exported();
    const auto empty_text = platform::utf16_to_utf8(empty.native());
    require(bool(empty_text), "Invalid UTF-16 empty repro path");
    require(read(empty / "manifest.json").at("engine_assets") == Json{{"version", 1}, {"records", Json::array()}},
            "An empty no-project document acquired unused installation assets");
    require(read(empty / "manifest.json").at("material_resources") == Json{{"version", 1}, {"records", Json::object()}},
            "An empty repro omitted its current, empty material digest set");
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}, {"mesh", "builtin/cube"}}}});
    const auto bundle = exported();
    const auto original = read(bundle / "manifest.json");
    const auto records = original.at("engine_assets").at("records");
    require(records.size() == 2 && records[0].at("key") == "builtin/cube" &&
                records[1].at("key") == "builtin/material/default-grid",
            "No-project export did not freeze its mesh and implicit DefaultWorldGrid MS");
    for (const auto& record : records) {
        const bool material = record.at("key") == "builtin/material/default-grid";
        require(record.at("kind") == Json(material ? "material" : "mesh") &&
                    record.at("digest_version") == Json(material ? 1 : 2),
                "Engine manifest did not identify material digest v1 and mesh digest v2");
    }
    const auto copy = root / "EngineRepro";
    const auto native_bundle = platform::windows_file_path(bundle);
    const auto native_copy = platform::windows_file_path(copy);
    require(native_bundle && native_copy, "Cannot resolve repro copy paths");
    std::filesystem::copy(*native_bundle, *native_copy, std::filesystem::copy_options::recursive);
    const auto bundle_text = platform::utf16_to_utf8(bundle.native());
    const auto copy_text = platform::utf16_to_utf8(copy.native());
    require(bundle_text && copy_text, "Invalid UTF-16 repro copy path");
    const auto scene_path = copy / "Project" / utf8_to_utf16_validated(original.at("scene").get<std::string>());
    const auto scene = read(scene_path);
    const auto publish = [&](Json manifest, const Json& value) {
        write(scene_path, value.dump(2));
        for (auto& file : manifest.at("files"))
            if (copy / utf8_to_utf16_validated(file.at("path").get<std::string>()) == scene_path) {
                const auto hash = sha256_file(scene_path);
                require(bool(hash), hash ? "" : hash.error());
                file["sha256"] = *hash;
                file["bytes"] = std::filesystem::file_size(scene_path);
            }
        write(copy / "manifest.json", manifest.dump(2));
    };
    d.op("edit.undo");
    d.ok("repro.open", {{"directory", *bundle_text}});
    const auto entity = d.inspect().at("entities")[0].at("handle").get<std::string>();
    d.op("scene.rename", {{"name", "Saved with history"}}, selection(entity));
    d.op("scene.save");
    d.op("scene.select", Json::object(), selection(entity));
    d.op("view.look_at", {{"eye", {{"x", 7}, {"y", 4}, {"z", -9}}}, {"target", {{"x", 1}, {"z", 2}}}});
    d.settle();
    auto incompatible = original;
    incompatible["engine_assets"]["records"][0]["content_sha256"] = std::string(64, '0');
    publish(incompatible, scene);
    {
        const auto before = d.inspect();
        const auto token = d.token;
        require(!before.at("dirty").get<bool>() && before.at("undo").get<unsigned>() > 0 &&
                    !before.at("selection").empty(),
                "Dependency atomicity fixture is not a saved, replaceable document with history and selection");
        static_cast<void>(d.call("repro.open", {{"directory", *copy_text}}));
        const auto after = d.inspect();
        for (const auto* field : {"project", "entities", "selection", "camera", "view", "undo", "redo", "dirty"})
            require(after.at(field) == before.at(field), "Rejected engine dependency changed " + std::string(field));
        require(d.token == token, "Rejected engine dependency changed the state token");
    }
    publish(original, scene);
    d.ok("repro.open", {{"directory", *copy_text}});
    const auto recovered = d.inspect();
    require(recovered.at("entities").size() == scene.at("entities").size() && recovered.at("undo") == 0 &&
                recovered.at("entities")[0].at("name") == scene.at("entities")[0].at("name"),
            "Restoring a valid manifest did not reopen the original document after dependency rejection");
    d.ok("repro.open", {{"directory", *empty_text}});
    require(d.inspect().at("entities").empty(), "Empty engine dependency set did not reopen");

    // A cached but unused material and an obsolete disk texture are not roots.
    // Only the referenced, published draft belongs to the frozen document.
    const auto project = root / "EngineDraftProject";
    project_fixture(project);
    const auto project_text = platform::utf16_to_utf8(project.native());
    require(bool(project_text), "Invalid UTF-16 draft project path");
    d.op("project.open", {{"path", *project_text}});
    const auto used = d.op("asset.new_material", {{"path", "Content/Materials/Used.material"}}, {{"asset", ""}})
                          .at("result")
                          .at("asset");
    const auto cached = d.op("asset.new_material", {{"path", "Content/Materials/Cached.material"}}, {{"asset", ""}})
                            .at("result")
                            .at("asset");
    for (const auto* name : {"Used", "Cached"}) {
        const auto file = project / "Content/Materials" / (std::string(name) + ".material");
        auto value = read(file);
        value["entries"] = Json::array({{{"path", "base_color_texture"},
                                         {"type", "texture"},
                                         {"enabled", true},
                                         {"value", {{"asset_key", "builtin/unknown"}, {"address", "repeat"}}}}});
        write(file, value.dump(2));
    }
    d.op("project.open", {{"path", *project_text}});
    d.ok("material.inspect", {{"asset", cached}});
    const auto loaded = d.ok("material.inspect", {{"asset", used}});
    require(material_issue(loaded, "builtin/unknown"),
            "Unknown-engine draft fixture did not retain its missing texture diagnostic");
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}});
    const auto component = d.inspect().at("entities")[0].at("components")[0].at("handle");
    d.ok("material.assign", {{"component", component}, {"slot", "slot/0"}, {"asset", used}});
    d.op("scene.save");
    d.ok("material.set_texture", {{"object", loaded.at("handle")}, {"path", "base_color_texture"}, {"texture", ""}});
    const auto draft = exported();
    require(!std::filesystem::exists(draft / "Project/Content/Materials/Cached.material") &&
                material_entry(read(draft / "Project/Content/Materials/Used.material"), "base_color_texture")
                        .at("value")
                        .at("asset_key") == "" &&
                material_entry(read(project / "Content/Materials/Used.material"), "base_color_texture")
                        .at("value")
                        .at("asset_key") == "builtin/unknown",
            "Frozen draft roots included an unused cache, ignored the live draft or changed its source file");
    d.op("edit.undo", Json::object(), {{"region", "parameters"}, {"asset", used}});
    const auto draft_text = platform::utf16_to_utf8(draft.native());
    require(bool(draft_text), "Invalid UTF-16 draft repro path");
    d.ok("repro.open", {{"directory", *draft_text}});
    require(d.ok("material.inspect", {{"asset", used}}).at("diagnostics").empty(),
            "Published draft did not reopen strictly");
    write(root / "engine-assets.json",
          Json{{"status", "passed"},
               {"records", records},
               {"cases",
                {"no-project-empty-and-mesh-with-grid", "saved-history-selection-camera-preserved",
                 "valid-manifest-reopen", "material-kind-and-digest-v1", "mesh-digest-v2",
                 "unused-cache-and-superseded-disk-not-roots", "referenced-draft"}}}
              .dump(2));
}

// One operator registry serves menus, keys and Control; the context override lasts one call.
void verify_operators(Driver& d, const std::string& entity, HWND window) {
    const auto before = d.inspect();
    const auto token = d.token;
    const auto thread = GetWindowThreadProcessId(window, nullptr);
    require(window != nullptr && thread != 0, "Operator admission needs the editor's GUI thread");
    const auto capture = [&] {
        GUITHREADINFO state{};
        state.cbSize = sizeof(state);
        require(GetGUIThreadInfo(thread, &state) != FALSE, "Cannot inspect editor thread capture");
        return state.hwndCapture;
    };
    require(!before.at("busy").get<bool>() && capture() == nullptr,
            "Operator admission fixture already owns an interaction or capture");
    const auto directory = d.ok("op.list");
    const auto& operators = directory.at("operators");
    const auto entry = [&](std::string_view id) -> const Json& {
        const auto found = std::ranges::find_if(
            operators, [&](const Json& item) { return item.at("id").get_ref<const std::string&>() == id; });
        require(found != operators.end(), "Operator directory omitted " + std::string(id));
        return *found;
    };
    require(entry("view.select_box").at("kinds") == Json::array({"interactive"}) &&
                entry("view.reset").at("kinds") == Json::array({"direct"}) &&
                entry("scene.duplicate").at("kinds") == Json::array({"direct", "interactive"}) &&
                entry("scene.duplicate").at("parameters").at("properties").at("place").at("default") == false &&
                !entry("scene.duplicate").at("interaction").get<std::string>().empty(),
            "Operator directory did not distinguish direct, interactive and argument-dependent invocation");
    const auto queried = d.inspect();
    require(d.token == token, "Operator discovery changed the state token");
    for (const auto key : {"entities", "selection", "dirty", "undo", "redo", "busy", "busy_reasons"})
        require(queried.at(key) == before.at(key), "Operator discovery changed " + std::string(key));
    error(d.try_op("view.select_box", Json::object(), {{"region", "viewport"}}), "interaction_required");
    require(capture() == nullptr, "External box selection acquired editor capture");
    const auto refused = d.inspect();
    require(d.token == token, "Refused box selection changed the state token");
    for (const auto key : {"entities", "selection", "dirty", "undo", "redo", "busy", "busy_reasons"})
        require(refused.at(key) == before.at(key), "Refused box selection changed " + std::string(key));
    const auto entries = before.at("undo").get<std::size_t>();
    const auto light = d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "point"}}}}).at("active");
    const auto after = d.inspect();
    require(after.at("undo") == entries + 1 && after.at("selection") == Json::array({light}) &&
                std::ranges::any_of(after.at("entities"), [&](const Json& item) { return item.at("handle") == light; }),
            "scene.add did not add and select one Entity in one history entry");
    d.op("edit.undo");
    require(d.inspect().at("undo") == entries, "Undo did not remove the added Entity");
    d.op("scene.select", Json::object(), selection(entity));
    require(d.inspect().at("selection") == Json::array({entity}), "scene.select did not restore the selection");
}

void verify_asset_directory(Driver& d, HWND window, const std::filesystem::path& project) {
    namespace fs = std::filesystem;
    // Refresh is the existing F5 command, not a second Control operation.
    const auto refresh = [&] {
        require(window != nullptr, "Refresh requires the real editor window");
        SendMessageW(window, WM_KEYDOWN, VK_F5, 1);
        SendMessageW(window, WM_KEYUP, VK_F5, 1);
        unsigned stable{};
        for (unsigned i = 0; i < 200 && stable < 5; ++i) {
            const auto before = d.token;
            std::this_thread::sleep_for(30ms);
            d.ok("editor.inspect", {{"detail", "summary"}});
            stable = before == d.token ? stable + 1 : 0;
        }
        require(stable == 5, "Refresh did not settle");
    };
    const auto saved = sha256_file(project / "Content/Scenes/Main.scene");
    require(bool(saved), saved ? "" : saved.error());
    const auto full = d.inspect();
    const auto page = [&](Json args = Json::object()) {
        args["detail"] = "assets";
        return d.ok("editor.inspect", std::move(args));
    };
    const auto first = page({{"limit", 1}});
    const auto cursor = first.at("next_cursor");
    require(!cursor.is_null() && first.at("detail") == "assets", "Small project did not return an assets continuation");
    const auto continuation = page({{"limit", 1}, {"cursor", cursor}});
    write(project / "Content/bad.bmp", "not decoded by catalog");
    write(project / "Content/bad.bmp.asset.json", "{broken");
    refresh();
    require(page({{"limit", 1}, {"cursor", cursor}}) == continuation,
            "Failed refresh replaced the published directory");
    fs::remove(project / "Content/bad.bmp.asset.json");
    fs::remove(project / "Content/bad.bmp");
    refresh();
    require(d.inspect().at("assets") == full.at("assets") &&
                sha256_file(project / "Content/Scenes/Main.scene") == *saved,
            "Failed refresh or fixture cleanup changed the original saved document/catalog");
}

void verify_option_descriptions(const Json& capabilities) {
    const auto& options = capabilities.at("display_options");
    const auto& count = options.at("probe_volume_0_count_x");
    require(count.at("type") == "integer" && count.at("minimum") == 2 && count.at("maximum") == 32 &&
                count.at("scope") == "scene",
            "Metadata changed option types, bounds or ownership");
}

Json verify_object_directory(Driver& d, const Json& full) {
    const auto state_token = d.token;
    const auto& entity = full.at("entities")[0];
    const auto& component = entity.at("components")[0];
    const auto page = d.ok("editor.inspect", {{"detail", "objects"}, {"entity_name", entity.at("name")}, {"limit", 1}});
    const auto first_cursor = page.at("next_cursor");
    const auto rest = d.ok("editor.inspect", {{"detail", "objects"}, {"cursor", first_cursor}});
    require(page.at("objects")[0].at("handle") == entity.at("handle") &&
                rest.at("objects")[0].at("handle") == component.at("handle") && rest.at("next_cursor").is_null(),
            "Object cursor did not retain its exact entity-name filter");
    require(d.token == state_token && d.inspect().at("entities") == full.at("entities"),
            "Read-only object discovery changed the document or its state token");
    return first_cursor;
}

// Only the scene records the sky map and its rotation; the frozen view holds view options.
void repro_sky(Driver& d, const std::filesystem::path& project) {
    const std::string key = "project/{6F1D2C3B-4A5E-4F60-8B7C-9D0E1F2A3B4C}";
    std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n";
    for (unsigned i = 0; i < 32; ++i)
        hdr += std::string{char(64 + 4 * i), char(128), char(32), char(129)};
    write(project / "Content/Sky/Repro.hdr", hdr);
    write(project / "Content/Sky/Repro.hdr.asset.json", Json{{"id", key.substr(8)}, {"version", 1}}.dump());
    const auto environment_of = [](Json& scene) -> Json& {
        for (auto& entity : scene.at("entities"))
            for (auto& component : entity.at("components"))
                if (component.at("type") == "world.EnvironmentComponent")
                    return component;
        throw std::runtime_error("The repro lighting scene has no Environment.");
    };
    auto scene = read(project / "Content/Scenes/Main.scene");
    environment_of(scene)["sky_map"] = key;
    environment_of(scene)["properties"]["sky_rotation"] = 30;
    write(project / "Content/Scenes/Main.scene", scene.dump(2));
    const auto project_text = platform::utf16_to_utf8(project.native());
    require(bool(project_text), "Invalid UTF-16 sky project path");
    d.op("project.open", {{"path", *project_text}});
    const auto exported = [&] {
        const auto id = unique_id();
        require(d.call("repro.export", Json::object(), id).at("status") == "pending", "Sky repro export did not start");
        const auto done = d.wait(id);
        require(done.at("status") == "ok", done.dump());
        const std::filesystem::path bundle =
            utf8_to_utf16_validated(done.at("result").at("directory").get<std::string>());
        const auto manifest = read(bundle / "manifest.json");
        auto frozen = read(bundle / "Project" / utf8_to_utf16_validated(manifest.at("scene").get<std::string>()));
        const auto& sky = environment_of(frozen);
        require(sky.at("sky_map") == key && sky.at("properties").at("sky_rotation") == 30,
                "A sky repro lost its scene's sky map key or rotation");
        return bundle;
    };
    const auto bundle = exported();
    const auto bundle_text = platform::utf16_to_utf8(bundle.native());
    require(bool(bundle_text), "Invalid UTF-16 sky repro path");
    d.ok("repro.open", {{"directory", *bundle_text}});
    exported();
}

// The export copies what the scene references, with the in-memory draft of a referenced material
// rather than what its saved file names: a draft that cleared a texture which has since gone
// missing still exports, the copy holds the draft and reopens strictly, and the source asset stays
// unchanged. A material the session loaded but the scene does not reference stays out.
void repro_material_draft(Driver& d, const std::filesystem::path& project, const std::filesystem::path& root) {
    const auto project_text = platform::utf16_to_utf8(project.native());
    const auto texture_text = platform::utf16_to_utf8((root / "Draft.png").native());
    require(project_text && texture_text, "Invalid UTF-16 material draft fixture path");
    d.op("project.open", {{"path", *project_text}});
    write(root / "Draft.png", std::string(tiny_png.begin(), tiny_png.end()));
    const auto texture = d.op("asset.import", {{"kind", "texture"}, {"path", *texture_text}})
                             .at("result")
                             .at("asset")
                             .get<std::string>();
    const auto asset = d.op("asset.new_material", {{"path", "Content/Materials/Draft.material"}}, {{"asset", ""}})
                           .at("result")
                           .at("asset")
                           .get<std::string>();
    const auto unused = d.op("asset.new_material", {{"path", "Content/Materials/Unused.material"}}, {{"asset", ""}})
                            .at("result")
                            .at("asset")
                            .get<std::string>();
    d.ok("material.set_texture", {{"object", d.ok("material.inspect", {{"asset", asset}}).at("handle")},
                                  {"path", "base_color_texture"},
                                  {"texture", texture}});
    d.ok("material.save", {{"asset", asset}});
    const auto entity = d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}}).at("active");
    Json component;
    for (const auto& item : d.inspect().at("entities"))
        if (item.at("handle") == entity)
            component = item.at("components")[0].at("handle");
    d.ok("material.assign", {{"component", component}, {"slot", "slot/0"}, {"asset", asset}});
    d.op("scene.save");
    const auto saved = project / "Content/Materials/Draft.material";
    const auto saved_text = read(saved).dump();
    for (const auto* file : {L"Content/Textures/Draft.png", L"Content/Textures/Draft.png.asset.json"}) {
        const auto path = platform::windows_file_path(project / file);
        require(bool(path), path ? "" : platform::error_message(path.error()));
        std::filesystem::remove(*path);
    }
    d.op("project.open", {{"path", *project_text}});
    const auto opened = d.ok("material.inspect", {{"asset", asset}});
    require(material_issue(opened, texture), "The draft fixture did not lose its texture: " + opened.dump());
    d.ok("material.inspect", {{"asset", unused}});
    d.ok("material.set_texture", {{"object", opened.at("handle")}, {"path", "base_color_texture"}, {"texture", ""}});
    const auto id = unique_id();
    require(d.call("repro.export", Json::object(), id).at("status") == "pending", "Draft repro export did not start");
    const auto done = d.wait(id);
    require(done.at("status") == "ok", "A material draft without its missing texture did not export: " + done.dump());
    const std::filesystem::path bundle = utf8_to_utf16_validated(done.at("result").at("directory").get<std::string>());
    require(material_entry(read(bundle / "Project/Content/Materials/Draft.material"), "base_color_texture")
                        .at("value")
                        .at("asset_key") == "" &&
                read(saved).dump() == saved_text,
            "A draft repro did not carry the draft, or changed the saved material");
    const auto unused_path = platform::windows_file_path(bundle / "Project/Content/Materials/Unused.material");
    require(bool(unused_path), unused_path ? "" : platform::error_message(unused_path.error()));
    require(!std::filesystem::exists(*unused_path) &&
                std::ranges::none_of(read(bundle / "manifest.json").at("input_files"),
                                     [](const Json& file) {
                                         return file.at("path").get<std::string>().starts_with(
                                             "Content/Materials/Unused");
                                     }),
            "A repro copied a material the session loaded but the scene does not reference");
    d.op("edit.undo", Json::object(), {{"region", "parameters"}, {"asset", asset}});
    const auto bundle_text = platform::utf16_to_utf8(bundle.native());
    require(bool(bundle_text), "Invalid UTF-16 material draft repro path");
    d.ok("repro.open", {{"directory", *bundle_text}});
    require(d.ok("material.inspect", {{"asset", asset}}).at("diagnostics").empty(),
            "A draft repro did not reopen without the missing texture");
}

// Exercise the real author/Runtime/export/strict-load path. Pixel expectations
// come from this RGBA fixture, not from another call to the exporter's decoder.
void repro_material_inputs(Driver& d, const std::filesystem::path& root) {
    const auto project = root / "Project";
    project_fixture(project);
    const auto project_text = platform::utf16_to_utf8(project.native());
    require(bool(project_text), "Invalid UTF-16 material input fixture path");
    d.op("project.open", {{"path", *project_text}});
    const std::array<std::uint8_t, 16> first_pixels{17, 53, 201, 0, 254, 1, 127, 85, 64, 192, 9, 170, 203, 87, 38, 255};
    const std::array<std::uint8_t, 16> next_pixels{225, 13, 87, 255, 41, 219, 3, 170, 99, 7, 181, 85, 6, 149, 231, 0};
    const auto bitmap = [](const std::array<std::uint8_t, 16>& rgba) {
        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = 2;
        header.bV5Height = -2;
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5SizeImage = 16;
        header.bV5RedMask = 0x00ff0000;
        header.bV5GreenMask = 0x0000ff00;
        header.bV5BlueMask = 0x000000ff;
        header.bV5AlphaMask = 0xff000000;
        header.bV5CSType = LCS_sRGB;
        header.bV5Intent = LCS_GM_IMAGES;
        BITMAPFILEHEADER file{};
        file.bfType = 0x4d42;
        file.bfOffBits = static_cast<DWORD>(sizeof(file) + sizeof(header));
        file.bfSize = file.bfOffBits + 16;
        std::string bytes(reinterpret_cast<const char*>(&file), sizeof(file));
        bytes.append(reinterpret_cast<const char*>(&header), sizeof(header));
        for (std::size_t i = 0; i < rgba.size(); i += 4)
            for (const auto channel : {2u, 1u, 0u, 3u})
                bytes.push_back(static_cast<char>(rgba[i + channel]));
        return bytes;
    };
    write(root / "Active.bmp", bitmap(first_pixels));
    write(root / "Dormant.bmp", bitmap(next_pixels));
    const auto active_text = platform::utf16_to_utf8((root / "Active.bmp").native());
    const auto dormant_text = platform::utf16_to_utf8((root / "Dormant.bmp").native());
    require(active_text && dormant_text, "Invalid UTF-16 texture fixture path");
    const auto texture =
        d.op("asset.import", {{"kind", "texture"}, {"path", *active_text}}).at("result").at("asset").get<std::string>();
    const auto dormant = d.op("asset.import", {{"kind", "texture"}, {"path", *dormant_text}})
                             .at("result")
                             .at("asset")
                             .get<std::string>();
    const std::string source_text = R"(struct Params { float gain; };
struct Inputs { material.TextureInput pigment; material.TextureInput dormant; };
Params defaults() { Params p; p.gain=0.75; return p; }
material.Output shade(material.Context context,Params p,Inputs inputs) {
    vector4 color=sample(inputs.pigment,context.uv);
    material.Output result; result.base_color=vector3(color.x,color.y,color.z)*p.gain;
    result.roughness=0.5; result.normal_world=context.normal_world; return result;
}
)";
    const std::string source_file = "Content/Materials/Frozen.ms.material";
    const std::string parent_file = "Content/Materials/Parent.material";
    const std::string leaf_file = "Content/Materials/Leaf.material";
    const std::string texture_file = "Content/Textures/Active.bmp";
    const auto source = d.op("asset.new_material_source", {{"path", source_file}, {"source", source_text}})
                            .at("result")
                            .at("asset")
                            .get<std::string>();
    auto source_author = read(project / source_file);
    const auto source_path = source_author.at("source_path").get<std::string>();
    const auto native_source_path = utf8_to_utf16_validated(source_path);
    source_author["policy"]["double_sided"] = true;
    source_author["inputs"] = Json::array();
    for (const auto* path : {"pigment", "dormant"})
        source_author["inputs"].push_back({{"path", path},
                                           {"binding", {{"asset_key", ""}, {"address", "repeat"}}},
                                           {"color", std::string_view(path) == "pigment" ? "srgb" : "linear"},
                                           {"empty_value", {1.0, 1.0, 1.0, 1.0}}});
    write(project / source_file, source_author.dump(2));
    const auto parent =
        d.op("asset.new_material",
             {{"path", parent_file},
              {"entries", Json::array({{{"path", "gain"}, {"type", "float"}, {"enabled", true}, {"value", 0.5}}})}},
             {{"asset", source}})
            .at("result")
            .at("asset")
            .get<std::string>();
    const auto binding = [](std::string path, std::string key, bool enabled = true) {
        return Json{{"path", std::move(path)},
                    {"type", "texture"},
                    {"enabled", enabled},
                    {"value", {{"asset_key", std::move(key)}, {"address", "repeat"}}}};
    };
    const auto leaf = d.op("asset.new_material",
                           {{"path", leaf_file},
                            {"entries", Json::array({binding("pigment", texture), binding("dormant", dormant, false),
                                                     binding("gain", "builtin/not-a-texture"),
                                                     binding("removed", "builtin/removed-input")})}},
                           {{"asset", parent}})
                          .at("result")
                          .at("asset")
                          .get<std::string>();
    const auto unused = d.op("asset.new_material_source", {{"path", "Content/Materials/Unused.ms.material"}})
                            .at("result")
                            .at("asset")
                            .get<std::string>();
    d.ok("material.inspect", {{"asset", unused}}); // Cached Source text must not enter this scene's closure.
    const auto opened = d.ok("material.inspect", {{"asset", leaf}});
    const auto handle = opened.at("handle");
    require(opened.at("source").at("text") == source_text && !opened.at("default_material").get<bool>(),
            "Custom MS did not load its exact saved text");
    // Keep a real texture root in the disabled entry before freezing. A root or
    // a cached image is not itself a strict execution dependency.
    d.ok("material.set_texture", {{"object", handle}, {"path", "dormant"}, {"texture", dormant}});
    d.ok("material.override", {{"object", handle}, {"path", "dormant"}, {"enabled", false}});
    d.ok("material.save", {{"asset", leaf}});
    const auto entity =
        d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}, {"mesh", "builtin/cube"}}}}).at("active");
    Json component;
    for (const auto& item : d.inspect().at("entities"))
        if (item.at("handle") == entity)
            component = item.at("components")[0].at("handle");
    d.ok("material.assign", {{"component", component}, {"slot", "slot/0"}, {"asset", leaf}});
    d.op("scene.save");
    const auto parent_author = read(project / parent_file);
    const auto leaf_author = read(project / leaf_file);
    const auto hash_json = [](const Json& value) {
        const auto text = value.dump();
        return sha256(std::as_bytes(std::span(text)));
    };
    const auto expected = [&](const std::array<std::uint8_t, 16>& pixels) {
        Json inputs = Json::object();
        for (const auto* path : {"pigment", "dormant"})
            inputs[path] = {{"key", ""}, {"image", nullptr}};
        Json records = Json::object();
        records[source] = hash_json({{"author", source_author},
                                     {"inputs", inputs},
                                     {"source", {{"path", source_path}, {"text", source_text}}}});
        records[parent] = hash_json({{"author", parent_author}, {"inputs", Json::object()}, {"parent", source}});
        records[leaf] = hash_json(
            {{"author", leaf_author},
             {"parent", parent},
             {"inputs",
              {{"pigment",
                {{"key", texture},
                 {"image",
                  {{"width", 2}, {"height", 2}, {"rgba_sha256", sha256(std::as_bytes(std::span(pixels)))}}}}}}}});
        return Json{{"version", 1}, {"records", std::move(records)}};
    };
    const auto exported = [&](const std::array<std::uint8_t, 16>& pixels) {
        const auto id = unique_id();
        require(d.call("repro.export", Json::object(), id).at("status") == "pending",
                "Material input repro export did not start");
        const auto done = d.wait(id);
        require(done.at("status") == "ok", done.dump());
        const std::filesystem::path bundle =
            utf8_to_utf16_validated(done.at("result").at("directory").get<std::string>());
        const auto manifest = read(bundle / "manifest.json");
        require(manifest.at("material_resources") == expected(pixels),
                "Repro digests changed MS text/policy, sparse entries, parents or exact RGBA pixels");
        require(sha256_file(bundle / "Project" / native_source_path) == sha256(std::as_bytes(std::span(source_text))),
                "Repro reread the MS Source instead of freezing its loaded text");
        require(read(bundle / "Project" / leaf_file) == leaf_author &&
                    read(bundle / "Project" / parent_file) == parent_author &&
                    read(bundle / "Project" / source_file) == source_author,
                "Repro lost sparse author entries or a reachable parent");
        std::set<std::string> files;
        for (const auto& file : manifest.at("files"))
            files.insert(file.at("path").get<std::string>());
        require(files.contains("Project/" + texture_file) &&
                    files.contains("Project/" + texture_file + ".asset.json") &&
                    files.contains("Project/" + source_file + ".asset.json") &&
                    files.contains("Project/" + parent_file + ".asset.json"),
                "Repro omitted an active image or a material ancestor's identity");
        require(std::ranges::none_of(files,
                                     [](const auto& path) {
                                         return path.starts_with("Project/Content/Textures/Dormant") ||
                                                path.starts_with("Project/Content/Materials/Unused");
                                     }),
                "Dormant pixels or cached MS author/text entered the execution closure");
        const auto& engine = manifest.at("engine_assets").at("records");
        require(engine.size() == 1 && engine[0].at("key") == "builtin/cube",
                "Mismatched texture keys or an unused default material entered the engine closure");
        return bundle;
    };
    const auto first = exported(first_pixels);
    const auto parent_handle = d.ok("material.inspect", {{"asset", parent}}).at("handle");
    d.ok("material.set_entry", {{"object", parent_handle},
                                {"entry", {{"path", "gain"}, {"type", "float"}, {"enabled", true}, {"value", 0.25}}}});
    const auto before_reimport = d.inspect();
    const auto identity_hash = sha256_file(project / (texture_file + ".asset.json"));
    require(bool(identity_hash), identity_hash ? "" : identity_hash.error());
    write(root / "Replacement.bmp", bitmap(next_pixels));
    const auto replacement_text = platform::utf16_to_utf8((root / "Replacement.bmp").native());
    require(bool(replacement_text), "Invalid UTF-16 texture replacement path");
    d.op("asset.reimport", {{"path", *replacement_text}}, {{"asset", texture}});
    const auto after_reimport = d.inspect();
    for (const auto* field : {"undo", "redo", "dirty", "dirty_materials"})
        require(before_reimport.at(field) == after_reimport.at(field), "Texture Reimport entered parameter history");
    require(sha256_file(project / (texture_file + ".asset.json")) == *identity_hash,
            "Texture Reimport rewrote its identity sidecar");
    d.op("edit.undo", Json::object(), {{"region", "parameters"}, {"asset", parent}});
    require(!d.ok("material.inspect", {{"asset", parent}}).at("dirty").get<bool>(),
            "Parameter Undo did not restore the saved author values");
    // Saved files now disagree with the loaded MS and imported pixels. Neither
    // this ordinary parameter Undo nor exporting may restore/read those files.
    const auto disk_text = source_text + "\n// saved after the MS was loaded\n";
    write(project / native_source_path, disk_text);
    write(project / texture_file, bitmap(first_pixels));
    const auto disk_image = sha256_file(project / texture_file);
    require(bool(disk_image), disk_image ? "" : disk_image.error());
    const auto frozen = exported(next_pixels);
    const auto manifest = read(frozen / "manifest.json");
    require(sha256_file(project / native_source_path) == sha256(std::as_bytes(std::span(disk_text))) &&
                sha256_file(project / texture_file) == *disk_image,
            "Export modified the source project's saved Source or texture");
    const auto reopened = [&](const std::filesystem::path& bundle) {
        const auto bundle_text = platform::utf16_to_utf8(bundle.native());
        require(bool(bundle_text), "Invalid UTF-16 material input repro path");
        d.ok("repro.open", {{"directory", *bundle_text}});
        const auto view = d.ok("material.inspect", {{"asset", leaf}});
        require(view.at("source").at("text") == source_text && view.at("entries") == leaf_author.at("entries") &&
                    !view.at("default_material").get<bool>(),
                "Strict repro reopen lost loaded Source, dormant author intent or the active material");
        require(std::ranges::any_of(
                    view.at("declarations"),
                    [](const Json& value) { return value.at("path") == "gain" && value.at("value") == 0.5; }),
                "A mismatched texture entry replaced its inherited numeric declaration");
    };
    reopened(first);
    exported(first_pixels); // The earlier immutable export still owns the earlier pixels.
    reopened(frozen);
    exported(next_pixels);
    const auto changed = root / "ChangedBundle";
    const auto native_frozen = platform::windows_file_path(frozen);
    const auto native_changed = platform::windows_file_path(changed);
    require(native_frozen && native_changed, "Cannot resolve material repro copy paths");
    std::filesystem::copy(*native_frozen, *native_changed, std::filesystem::copy_options::recursive);
    d.settle();
    const auto native_target = platform::windows_file_path(changed / "Project" / native_source_path);
    require(bool(native_target), "Cannot resolve changed material Source path");
    const auto& target = *native_target;
    write(target, source_text + "\n// valid changed Source\n");
    auto modified = manifest;
    bool listed = false;
    for (auto& file : modified.at("files"))
        if (file.at("path") == "Project/" + source_path) {
            const auto hash = sha256_file(target);
            require(bool(hash), hash ? "" : hash.error());
            file["sha256"] = *hash;
            file["bytes"] = std::filesystem::file_size(target);
            listed = true;
        }
    require(listed, "Changed material input is absent from the manifest");
    write(changed / "manifest.json", modified.dump(2));
    const auto before = d.inspect();
    const auto token = d.token;
    const auto changed_text = platform::utf16_to_utf8(changed.native());
    require(bool(changed_text), "Invalid UTF-16 changed repro path");
    static_cast<void>(d.call("repro.open", {{"directory", *changed_text}}));
    const auto after = d.inspect();
    for (const auto* field : {"project", "scene_path", "entities", "selection", "camera", "view", "dirty",
                              "dirty_materials", "undo", "redo"})
        require(before.at(field) == after.at(field), "Rejected material repro changed " + std::string(field));
    require(d.token == token, "Rejected material repro changed the accepted state token");
    write(target, source_text);
    write(changed / "manifest.json", manifest.dump(2));
    reopened(changed);
    exported(next_pixels);
    write(root / "material-inputs.json",
          Json{{"status", "passed"},
               {"first_rgba", sha256(std::as_bytes(std::span(first_pixels)))},
               {"reimported_rgba", sha256(std::as_bytes(std::span(next_pixels)))},
               {"material_resources", manifest.at("material_resources")},
               {"cases",
                {"MS-loaded-text", "source-and-parent-closure", "exact-RGBA-and-alpha",
                 "manual-reimport-then-parameter-undo", "immutable-earlier-export", "disabled-retained-root",
                 "mismatched-and-removed-texture-paths", "unused-cached-MS-excluded", "strict-reopen-and-reexport",
                 "content-digests-beyond-file-hashes", "rejected-candidate-preserves-state"}}}
              .dump(2));
}

void repro_lighting(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto project = root / "Project";
    const auto directory = root / "Control";
    project_fixture(project);
    Process editor(executable,
                   L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --no-activate",
                   root / "editor.log");
    Driver d(discover_editor(editor, directory));
    d.settle();
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}});
    d.op("view.look_at", {{"eye", {{"x", 8}, {"y", 5}, {"z", -10}}}, {"target", {{"x", 1}, {"y", 2}, {"z", 3}}}});
    d.op("view.set", {{"direct_candidates", 16}, {"exposure_ev", 1.25}});
    d.op("scene.save");
    repro_sky(d, project);
    repro_material_draft(d, project, root);
    d.client.reset();
    editor.close();
    const auto root_text = platform::utf16_to_utf8(root.native());
    require(bool(root_text), "Invalid UTF-16 lighting evidence path");
    std::cout << "Editor Repro Lighting E2E: PASS | " << *root_text << '\n';
}

void reject_nested_destination(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto project = root / "NestedProject";
    project_fixture(project);
    const auto directory = project / "Content/Control";
    Process editor(executable,
                   L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --no-activate",
                   root / "nested-editor.log");
    Driver d(discover_editor(editor, directory));
    d.settle();
    const auto id = unique_id();
    const auto started = d.call("repro.export", Json::object(), id);
    require(started.at("status") == "pending", "Nested output fixture did not start: " + started.dump());
    static_cast<void>(d.wait(id));
    const auto artifacts = platform::windows_file_path(
        directory / "artifacts" / utf8_to_utf16_validated(d.descriptor.at("instance_id").get<std::string>()));
    require(bool(artifacts), artifacts ? "" : platform::error_message(artifacts.error()));
    require(std::filesystem::is_empty(*artifacts), "Rejected nested export left a recursive output");
    d.client.reset();
    editor.close();
}

// An ordinary examples project contains unrelated content too. The repro carries
// only what the Sponza scene references: its two glTF
// meshes with their buffers and images, each slot's default material, and the sun-removed sky
// map; not the unused original HDRI, the unrelated Physic example or the READMEs.
void repro_examples(const std::filesystem::path& executable, const std::filesystem::path& root) try {
    const std::filesystem::path shipped = ATELIER_EXAMPLE_PROJECT;
    const auto project = root / "Project";
    const auto directory = root / "Control";
    for (const auto* folder : {L"Config", L"Source", L"Content/Sponza", L"Content/Physic"}) {
        const auto source = platform::windows_file_path(shipped / folder);
        const auto target = platform::windows_file_path(project / folder);
        require(source && target, "Cannot resolve Examples copy paths");
        std::filesystem::create_directories(target->parent_path());
        std::filesystem::copy(*source, *target, std::filesystem::copy_options::recursive);
    }
    const std::string sponza = "Content/Sponza/";
    const std::string building = sponza + "NewSponza/NewSponza_Main_glTF_003.gltf";
    const std::string curtains = sponza + "SponzaExtend/NewSponza_Curtains_glTF.gltf";
    // A mesh may spell an image in another case than its file; the repro copies the catalogue's file once.
    const std::string image = "textures/arch_stone_wall_01_BaseColor.png";
    const std::string alias = "Textures/Arch_Stone_Wall_01_BaseColor.png";
    {
        const auto path = platform::windows_file_path(project / building);
        require(bool(path), path ? "" : platform::error_message(path.error()));
        std::ifstream stream(*path, std::ios::binary);
        std::string text(std::istreambuf_iterator<char>(stream), {});
        const auto at = text.find("\"" + image + "\"");
        require(at != std::string::npos, "The New Sponza glTF fixture changed");
        text.replace(at + 1, image.size(), alias);
        write(project / building, text);
    }
    // Every slot default the two mesh sidecars name, by the material file that holds its identity.
    std::map<std::string, std::string> material_files;
    std::set<std::string> defaults;
    const auto native_project = platform::windows_file_path(project);
    const auto native_sponza = platform::windows_file_path(project / sponza);
    require(native_project && native_sponza, "Cannot resolve Examples material paths");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(*native_sponza)) {
        auto path = platform::utf16_to_utf8(entry.path().lexically_relative(*native_project).generic_wstring());
        require(bool(path), "Invalid UTF-16 Examples material path");
        if (path->ends_with(".material.asset.json")) {
            const auto key = "project/" + read(entry.path()).at("id").get<std::string>();
            path->resize(path->size() - 11);
            material_files[key] = std::move(*path);
        }
    }
    for (const auto& mesh : {building, curtains})
        for (const auto& [slot, key] : read(project / (mesh + ".asset.json")).at("materials").items())
            defaults.insert(material_files.at(key.get<std::string>()));
    require(defaults.size() == 31, "The New Sponza meshes no longer name 31 slot default materials");
    const std::string sky_file = sponza + "Sky/kloppenheim_05_4k_nosun.hdr";
    {
        Process editor(executable,
                       L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --no-activate",
                       root / "editor.log");
        Driver d(discover_editor(editor, directory));
        d.settle();
        const auto exported = [&] {
            const auto id = unique_id();
            const auto started = d.call("repro.export", Json::object(), id);
            require(started.at("status") == "pending", started.dump());
            const auto done = d.wait(id);
            require(done.at("status") == "ok", done.dump());
            const std::filesystem::path bundle =
                utf8_to_utf16_validated(done.at("result").at("directory").get<std::string>());
            const auto manifest = read(bundle / "manifest.json");
            std::set<std::string> copied;
            std::map<std::string, std::string> hashes;
            for (const auto& file : manifest.at("input_files")) {
                copied.insert(file.at("path").get<std::string>());
                hashes[file.at("path").get<std::string>()] = file.at("sha256").get<std::string>();
            }
            require(!copied.contains(sponza + "NewSponza/" + alias),
                    "An Examples repro copied a case alias instead of its catalogue path");
            // Each asset arrives with the identity file the source project gave it.
            const auto asset = [&](const std::string& path) {
                return copied.contains(path) && hashes.contains(path + ".asset.json") &&
                       hashes.at(path + ".asset.json") == sha256_file(project / (path + ".asset.json"));
            };
            for (const auto& path : copied)
                if (path.find("/textures/") != std::string::npos && !path.ends_with(".asset.json"))
                    require(asset(path), "An Examples repro copied a texture without its identity file: " + path);
            require(asset(building) && asset(curtains) &&
                        copied.contains(sponza + "NewSponza/NewSponza_Main_glTF_003.bin") &&
                        copied.contains(sponza + "SponzaExtend/NewSponza_Curtains_glTF.bin") &&
                        asset(sponza + "NewSponza/" + image) && asset(sky_file),
                    "An Examples repro left out the scene's meshes, their dependencies or its sky map");
            for (const auto& material : defaults)
                require(asset(material), "An Examples repro left out the slot default material " + material);
            require(std::ranges::none_of(copied,
                                         [&](const std::string& path) {
                                             return path.starts_with("Content/Physic/") ||
                                                    path.starts_with(sponza + "Scenes/") ||
                                                    path.starts_with(sponza + "Sky/kloppenheim_05_4k.hdr") ||
                                                    path.ends_with("README.md");
                                         }),
                    "An Examples repro copied files the scene does not use");
            return std::pair{bundle, manifest};
        };
        const auto [bundle, manifest] = exported();
        require(manifest.at("resources").size() == 3,
                "An Examples repro did not check Sponza's two meshes and its sky map: " +
                    manifest.at("resources").dump());
        const auto bundle_text = platform::utf16_to_utf8(bundle.native());
        require(bool(bundle_text), "Invalid UTF-16 Examples repro path");
        d.ok("repro.open", {{"directory", *bundle_text}});
        exported();
        d.client.reset();
        editor.close();
    }
    const auto native_root = platform::windows_file_path(root);
    require(bool(native_root), native_root ? "" : platform::error_message(native_root.error()));
    std::filesystem::remove_all(*native_root);
} catch (...) {
    const auto root_text = platform::utf16_to_utf8(root.native());
    std::fprintf(stderr, "Examples repro failure evidence retained at %s\n",
                 root_text ? root_text->c_str() : "(evidence path is not valid UTF-16)");
    throw;
}

// A glTF import publishes one material per slot as the mesh's default, which the scene does not
// name. The repro carries the default material, its texture and their identity files, reopens
// strictly and exports the same files again.
void repro_gltf_defaults(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto project = root / "Project";
    const auto directory = root / "Control";
    const auto source = root / "source";
    project_fixture(project);
    write(source / "textures/color.png", std::string(tiny_png.begin(), tiny_png.end()));
    std::string bin;
    const auto put = [&](std::initializer_list<float> values) {
        for (const float v : values)
            bin.append(reinterpret_cast<const char*>(&v), 4);
    };
    put({0, 0, 0, 1, 0, 0, 0, 1, 0});
    put({0, 0, -1, 0, 0, -1, 0, 0, -1});
    put({0, 1, 1, 1, 0, 0});
    for (const std::uint16_t i : {0, 1, 2, 0})
        bin.append(reinterpret_cast<const char*>(&i), 2);
    write(source / "scene.bin", bin);
    const auto view = [](int offset, int length) {
        return Json{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}};
    };
    const auto accessor = [](int index, const char* type, int component) {
        return Json{{"bufferView", index}, {"type", type}, {"componentType", component}, {"count", 3}};
    };
    const auto primitive = [](int material) {
        return Json{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                    {"indices", 3},
                    {"material", material}};
    };
    write(source / "scene.gltf",
          Json{{"asset", {{"version", "2.0"}}},
               {"scenes", {{{"nodes", {0}}}}},
               {"nodes", {{{"mesh", 0}}}},
               {"meshes", {{{"primitives", {primitive(0), primitive(1)}}}}},
               {"materials",
                {{{"name", "paint"}, {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}}}},
                 {{"name", "trim"},
                  {"pbrMetallicRoughness", {{"baseColorFactor", {0.2, 0.7, 0.4, 1}}, {"roughnessFactor", 0.3}}}}}},
               {"textures", {{{"source", 0}}}},
               {"images", {{{"uri", "textures/color.png"}}}},
               {"buffers", {{{"uri", "scene.bin"}, {"byteLength", bin.size()}}}},
               {"bufferViews", {view(0, 36), view(36, 36), view(72, 24), view(96, 6)}},
               {"accessors",
                {accessor(0, "VEC3", 5126), accessor(1, "VEC3", 5126), accessor(2, "VEC2", 5126),
                 accessor(3, "SCALAR", 5123)}}}
              .dump());
    Process editor(executable,
                   L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --no-activate",
                   root / "editor.log");
    Driver d(discover_editor(editor, directory));
    d.settle();
    const auto source_text = platform::utf16_to_utf8((source / "scene.gltf").native());
    require(bool(source_text), "Invalid UTF-16 glTF source path");
    const auto mesh =
        d.op("asset.import", {{"kind", "mesh"}, {"path", *source_text}}).at("result").at("asset").get<std::string>();
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}, {"mesh", mesh}}}});
    const std::string folder = "Content/Meshes/scene/";
    const std::string material = folder + "Materials/paint.material";
    const std::string trim = folder + "Materials/trim.material";
    const auto identity = read(project / (folder + "scene.gltf.asset.json"));
    const auto defaults = identity.at("materials");
    require(defaults.size() == 2 &&
                defaults.at("paint") ==
                    "project/" + read(project / (material + ".asset.json")).at("id").get<std::string>() &&
                defaults.at("trim") == "project/" + read(project / (trim + ".asset.json")).at("id").get<std::string>(),
            "The glTF import did not name its generated material as the slot default: " + defaults.dump());
    std::set<std::string> inputs;
    const auto exported = [&] {
        const auto id = unique_id();
        require(d.call("repro.export", Json::object(), id).at("status") == "pending",
                "glTF repro export did not start");
        const auto done = d.wait(id);
        require(done.at("status") == "ok", done.dump());
        const std::filesystem::path bundle =
            utf8_to_utf16_validated(done.at("result").at("directory").get<std::string>());
        std::set<std::string> copied;
        for (const auto& file : read(bundle / "manifest.json").at("input_files"))
            copied.insert(file.at("path").get<std::string>());
        for (const auto& file :
             {folder + "scene.gltf", folder + "scene.bin", folder + "textures/color.png", material, trim})
            require(copied.contains(file) && (file.ends_with(".bin") || copied.contains(file + ".asset.json")),
                    "A glTF repro left out " + file + " or its identity file");
        if (inputs.empty())
            inputs = copied;
        require(copied == inputs, "A reopened glTF repro carried different files");
        return bundle;
    };
    const auto bundle = exported();
    const auto refresh = [&] {
        const auto window = editor.window();
        require(window != nullptr, "Dependency drift needs the real Refresh command");
        SendMessageW(window, WM_KEYDOWN, VK_F5, 1);
        SendMessageW(window, WM_KEYUP, VK_F5, 1);
        d.settle();
    };
    const auto component_details = [&] {
        auto result = Json::array();
        for (const auto& entity : d.inspect().at("entities"))
            for (const auto& component : entity.at("components"))
                result.push_back(d.ok("editor.inspect", {{"detail", "object"}, {"object", component.at("handle")}}));
        return result;
    };
    auto observations = Json::array();
    const auto rejected = [&](const char* label) {
        const auto before = d.inspect();
        const auto details = component_details();
        std::map<std::filesystem::path, std::string> hashes;
        for (const auto& file : std::filesystem::recursive_directory_iterator(project))
            if (file.is_regular_file() && *file.path().lexically_relative(project).begin() != L"Cache") {
                auto hash = sha256_file(file.path());
                require(bool(hash), hash ? "" : hash.error());
                hashes.emplace(file.path(), std::move(*hash));
            }
        const auto id = unique_id();
        auto response = d.call("repro.export", Json::object(), id);
        if (response.at("status") == "pending")
            response = d.wait(id);
        const bool refused =
            response.at("status") == "error" && response.at("error").at("code") == "dependency_mismatch";
        observations.push_back({{"case", label},
                                {"rejected", refused},
                                {"response", response},
                                {"components", details},
                                {"disk_defaults", read(project / (folder + "scene.gltf.asset.json")).at("materials")}});
        write(root / "dependency-consistency.json", observations.dump(2));
        const auto after = d.inspect();
        for (const auto* field :
             {"entities", "selection", "dirty", "dirty_materials", "undo", "redo", "camera", "project", "scene_path"})
            require(before.at(field) == after.at(field), "Rejected export changed " + std::string(field));
        require(component_details() == details, "Export rebound the live document's defaults");
        for (const auto& [path, hash] : hashes)
            require(sha256_file(path) == hash, "Export changed a source file");
        return refused;
    };
    // The same dependency set is not the same relation: both material files remain reachable.
    auto swapped = identity;
    std::swap(swapped["materials"]["paint"], swapped["materials"]["trim"]);
    write(project / (folder + "scene.gltf.asset.json"), swapped.dump());
    refresh();
    const bool relationship_rejected = rejected("same-set-swapped-defaults");
    write(project / (folder + "scene.gltf.asset.json"), identity.dump());
    refresh();
    // Two components retain different immutable versions of the same key after Refresh.
    // Restore disk to the first version: checking only the first component would miss the second.
    auto changed = bin;
    const float longer = 3;
    changed.replace(12, 4, reinterpret_cast<const char*>(&longer), 4);
    write(project / (folder + "scene.bin"), changed);
    refresh();
    const bool content_rejected = rejected("same-key-changed-content");
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}, {"mesh", mesh}}}});
    write(project / (folder + "scene.bin"), bin);
    refresh();
    const bool versions_rejected = rejected("same-key-mixed-loaded-versions");
    d.op("edit.undo");
    exported();
    require(relationship_rejected && content_rejected && versions_rejected,
            "Repro accepted dependency drift; see dependency-consistency.json");
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}, {"mesh", mesh}}}});
    require(read(exported() / "manifest.json").at("resources").size() == 1,
            "Equal loaded content must export once per key");
    d.op("edit.undo");
    // The export froze the unsaved mesh; undoing it leaves the source scene clean so the repro may replace it.
    d.op("edit.undo");
    require(!d.inspect().at("dirty").get<bool>(), "Undoing the placed glTF mesh did not restore the saved scene");
    const auto bundle_text = platform::utf16_to_utf8(bundle.native());
    require(bool(bundle_text), "Invalid UTF-16 glTF repro path");
    d.ok("repro.open", {{"directory", *bundle_text}});
    exported();
    auto reopened_defaults = Json::object();
    for (const auto& binding : component_details()[0].at("object").at("mesh").at("default_materials"))
        reopened_defaults[binding.at("slot").get<std::string>()] = binding.at("source");
    require(reopened_defaults == defaults, "Reopened repro changed slot relationships");
    // Live material drafts are intentionally frozen, unlike external dependency drift.
    const auto draft_source = bundle / "Project" / trim;
    const auto saved_material = sha256_file(draft_source);
    require(bool(saved_material), saved_material ? "" : saved_material.error());
    const auto trim_key = defaults.at("trim").get<std::string>();
    const auto handle = d.ok("material.inspect", {{"asset", trim_key}}).at("handle");
    d.ok("material.set_entry",
         {{"object", handle},
          {"entry", {{"path", "roughness"}, {"type", "float"}, {"enabled", true}, {"value", 0.625}}}});
    const auto draft_bundle = exported();
    require(material_entry(read(draft_bundle / "Project" / trim), "roughness").at("value") == 0.625 &&
                sha256_file(draft_source) == *saved_material &&
                d.ok("material.inspect", {{"asset", trim_key}}).at("dirty") == true,
            "Repro lost or saved the source material draft");
    d.op("edit.undo", Json::object(), {{"region", "parameters"}, {"asset", trim_key}});
    const auto draft_text = platform::utf16_to_utf8(draft_bundle.native());
    require(bool(draft_text), "Invalid UTF-16 material draft repro path");
    d.ok("repro.open", {{"directory", *draft_text}});
    const auto reopened_material = d.ok("material.inspect", {{"asset", trim_key}});
    require(std::ranges::any_of(reopened_material.at("declarations"),
                                [](const Json& p) { return p.at("path") == "roughness" && p.at("value") == 0.625; }),
            "Repro did not load the frozen material draft");
    // A component keeps its material bindings while its mesh is missing. Only the scene names the
    // bound material then, and the repro still carries it.
    const auto project_text = platform::utf16_to_utf8(project.native());
    require(bool(project_text), "Invalid UTF-16 glTF project path");
    d.op("project.open", {{"path", *project_text}});
    const auto bound = d.op("asset.new_material", {{"path", "Content/Materials/Bound.material"}}, {{"asset", ""}})
                           .at("result")
                           .at("asset")
                           .get<std::string>();
    const auto entity =
        d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}, {"mesh", mesh}}}}).at("active");
    Json component;
    for (const auto& item : d.inspect().at("entities"))
        if (item.at("handle") == entity)
            component = item.at("components")[0].at("handle");
    d.ok("material.assign", {{"component", component}, {"slot", "paint"}, {"asset", bound}});
    d.op("scene.save");
    // Missing defaults remain missing, rather than being rebound or preventing diagnostic export.
    const auto trim_bytes = read(project / trim).dump();
    std::filesystem::remove(project / trim);
    d.op("project.open", {{"path", *project_text}});
    const auto missing_id = unique_id();
    require(d.call("repro.export", Json::object(), missing_id).at("status") == "pending",
            "Missing-default export did not start");
    const auto missing_done = d.wait(missing_id);
    require(missing_done.at("status") == "ok", missing_done.dump());
    const std::filesystem::path missing_bundle =
        utf8_to_utf16_validated(missing_done.at("result").at("directory").get<std::string>());
    require(!std::filesystem::exists(missing_bundle / "Project" / trim) &&
                read(missing_bundle / "Project" / (folder + "scene.gltf.asset.json")).at("materials") == defaults,
            "Missing-default export synthesized a material or rebound the slots");
    const auto before_missing_open = d.inspect();
    error(d.call("repro.open", {{"directory", missing_done.at("result").at("directory")}}), "operation_failed");
    require(d.inspect().at("entities") == before_missing_open.at("entities") &&
                d.inspect().at("project") == before_missing_open.at("project"),
            "Strict missing-default reopen replaced the document");
    write(project / trim, trim_bytes);
    const auto mesh_path = platform::windows_file_path(project / (folder + "scene.gltf"));
    require(bool(mesh_path), mesh_path ? "" : platform::error_message(mesh_path.error()));
    std::filesystem::remove(*mesh_path);
    d.op("project.open", {{"path", *project_text}});
    const auto id = unique_id();
    require(d.call("repro.export", Json::object(), id).at("status") == "pending",
            "Missing-mesh repro export did not start");
    const auto done = d.wait(id);
    require(done.at("status") == "ok", done.dump());
    std::set<std::string> copied;
    for (const auto& file :
         read(std::filesystem::path(utf8_to_utf16_validated(done.at("result").at("directory").get<std::string>())) /
              "manifest.json")
             .at("input_files"))
        copied.insert(file.at("path").get<std::string>());
    require(copied.contains("Content/Materials/Bound.material") &&
                copied.contains("Content/Materials/Bound.material.asset.json") && !copied.contains(material),
            "A repro of a component whose mesh is missing lost its bound material, or copied the mesh's default");
    d.client.reset();
    editor.close();
}

#include "client_size.inl"
}

int wmain(int argc, wchar_t** argv) {
    atelier::install_fatal_handler();
    try {
        require(
            argc == 3 ||
                (argc == 4 &&
                 (std::wstring_view(argv[3]) == L"--repro-paths" || std::wstring_view(argv[3]) == L"--repro-lighting" ||
                  std::wstring_view(argv[3]) == L"--descriptions" || std::wstring_view(argv[3]) == L"--client-size" ||
                  std::wstring_view(argv[3]) == L"--smoke-logs")),
            "Usage: atelier_editor_control_acceptance editor.exe output-directory [--repro-paths|--repro-lighting|--descriptions|--client-size|--smoke-logs]");
        const bool descriptions = argc == 4 && std::wstring_view(argv[3]) == L"--descriptions";
        // Launched editors and their Play processes inherit the editor's harness cache, not the user's.
        if (!GetEnvironmentVariableW(L"ATELIER_SHADER_CACHE", nullptr, 0))
            SetEnvironmentVariableW(L"ATELIER_SHADER_CACHE",
                                    (std::filesystem::absolute(argv[1]).parent_path() / L"shader-cache").c_str());
        const auto root = std::filesystem::absolute(argv[2]) / unique_id();
        std::filesystem::create_directories(root);
        const auto root_text = platform::utf16_to_utf8(root.native());
        require(bool(root_text), "Invalid UTF-16 Control evidence path");
        if (argc == 4 && std::wstring_view(argv[3]) == L"--smoke-logs") {
            smoke_logs(argv[1], root);
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[3]) == L"--client-size") {
            client_size(argv[1], root);
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[3]) == L"--repro-lighting") {
            repro_lighting(argv[1], root);
            return 0;
        }
        if (argc == 4 && !descriptions) {
            reject_nested_destination(argv[1], root);
            repro_examples(argv[1], root / "examples");
            repro_gltf_defaults(argv[1], root / "gltf");
            return 0;
        }
        const auto directory = root / "Control";
        const auto project = root / "Project";
        project_fixture(project);
        auto arguments = L"--control-directory " + quote(directory) + L" --no-activate";
        if (!descriptions)
            arguments += L" --gpu-validation";
        Process editor(argv[1], std::move(arguments), root / "editor.log");
        const auto descriptor = discover_editor(editor, directory);
        // A small, visible owned test window can render without taking keyboard focus.
        if (auto window = editor.window()) {
            if (descriptions)
                SetWindowPos(window, nullptr, 0, 0, 960, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            else
                SetWindowPos(window, HWND_TOPMOST, 12, 80, 960, 640, SWP_NOACTIVATE);
        }
        Driver d(descriptor);
        const auto caps = d.ok("capabilities");
        require(std::ranges::any_of(caps.at("operations"),
                                    [](const Json& op) {
                                        return op.at("op") == "op.call" && op.at("requires_state_token") == true &&
                                               op.at("kind") == "direct" &&
                                               op.at("accepted_operator_kinds") == Json::array({"direct"}) &&
                                               op.at("args").at("required") == Json::array({"id"});
                                    }),
                "Capabilities omitted the stateful operator call contract");
        require(std::ranges::any_of(caps.at("operations"),
                                    [](const Json& op) {
                                        return op.at("op") == "op.list" && op.at("kind") == "query" &&
                                               op.at("requires_state_token") == false;
                                    }) &&
                    std::ranges::any_of(caps.at("operations"),
                                        [](const Json& op) {
                                            return op.at("op") == "job.cancel" && op.at("kind") == "direct" &&
                                                   op.at("requires_state_token") == false;
                                        }),
                "Capabilities confused query semantics with state-token requirements");
        verify_option_descriptions(caps);
        d.settle();
        if (!descriptions) {
            repro_engine_assets(d, root);
            repro_material_inputs(d, root / "MaterialInputs");
        }
        const auto project_text = platform::utf16_to_utf8(project.native());
        require(bool(project_text), "Invalid UTF-16 Control project path");
        const auto opened = d.ok("op.call", {{"id", "project.open"}, {"args", {{"path", *project_text}}}});
        require(opened.at("status") == "finished" && opened.at("selection").empty(),
                "Project open did not complete its replacement before returning the operator report");
        d.settle();
        verify_asset_directory(d, editor.window(), project);
        auto initial = d.inspect();
        unsigned stable{};
        for (unsigned i = 0; i < 200 && stable < 5; ++i) {
            const auto token = d.token;
            std::this_thread::sleep_for(30ms);
            initial = d.inspect();
            stable = token == d.token && !initial.at("last_frame").empty() ? stable + 1 : 0;
        }
        require(stable == 5, "Initial surface mapping did not stabilize");
        require(initial.at("entities").empty() && !initial.at("last_frame").empty(),
                "Initial editor state is not ready");
        const auto empty_directory = d.ok("editor.inspect", {{"detail", "objects"}});
        require(empty_directory.at("objects").empty() && empty_directory.at("next_cursor").is_null(),
                "Empty object directory has a continuation");
        const auto original_scene = read(project / "Content/Scenes/Main.scene");
        const auto original_scene_hash =
            descriptions ? sha256_file(project / "Content/Scenes/Main.scene") : std::string{};
        require(bool(original_scene_hash), original_scene_hash ? "" : original_scene_hash.error());
        Json request{{"op", "op.call"},
                     {"args", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}}},
                     {"state_token", d.token},
                     {"request_id", unique_id()}};
        const auto first = d.client->call(request);
        require(first.at("status") == "ok", first.dump());
        const auto replay = d.client->call(request);
        require(first == replay, "Duplicate mutation did not replay its response");
        request["args"]["kind"] = "entity";
        error(d.client->call(request), "request_id_conflict");
        auto current = d.inspect();
        require(current.at("entities").size() == 1 && current.at("undo") == 1, "Duplicate request repeated creation");
        const auto directory_cursor = verify_object_directory(d, current);
        if (descriptions) {
            write(root / "capabilities.json", caps.dump(2));
            write(root / "properties.json", current.at("entities")[0].dump(2));
            d.op("edit.undo");
            const auto restored = d.inspect();
            error(d.call("editor.inspect", {{"detail", "objects"}, {"cursor", directory_cursor}}), "stale_cursor");
            error(d.call("editor.inspect", {{"detail", "object"}, {"object", current.at("entities")[0].at("handle")}}),
                  "stale_object");
            require(restored.at("entities").empty() && !restored.at("dirty").get<bool>() &&
                        restored.at("undo") == initial.at("undo") && restored.at("view") == initial.at("view"),
                    "Description inspection or cleanup changed saved document state.");
            // Placeholders report the saved type they stand in for and the data they keep.
            write(project / "Content/Scenes/Placeholder.scene", Json{{"version", 10},
                                                                     {"next_id", "3"},
                                                                     {"roots", {"1"}},
                                                                     {"materials", Json::array()},
                                                                     {"entities",
                                                                      {{{"id", "1"},
                                                                        {"type", "world.Entity"},
                                                                        {"name", "Holder"},
                                                                        {"enabled", true},
                                                                        {"properties", Json::object()},
                                                                        {"children", Json::array()},
                                                                        {"space", nullptr},
                                                                        {"components",
                                                                         {{{"id", "2"},
                                                                           {"type", "project.Gone"},
                                                                           {"name", ""},
                                                                           {"enabled", true},
                                                                           {"properties", {{"strength", 3}}}}}}}}}}
                                                                    .dump());
            d.op("scene.open", {{"path", "Content/Scenes/Placeholder.scene"}});
            const auto placed = d.ok("editor.inspect", {{"detail", "objects"}}).at("objects");
            const auto gone =
                std::ranges::find_if(placed, [](const Json& entry) { return entry.at("kind") == "component"; });
            require(placed.size() == 2 && !placed[0].contains("placeholder") && !placed[0].contains("saved_type") &&
                        gone != placed.end() && gone->at("placeholder") == true &&
                        gone->at("saved_type") == "project.Gone" && gone->at("type") == "world.Component",
                    "Object directory did not identify a placeholder and its saved type.");
            const auto detail =
                d.ok("editor.inspect", {{"detail", "object"}, {"object", gone->at("handle")}}).at("object");
            require(detail.at("saved_type") == "project.Gone" && detail.at("saved_data").size() == 1 &&
                        detail.at("saved_data")[0].at("path") == "properties.strength" &&
                        detail.at("saved_data")[0].at("value") == "3" &&
                        detail.at("saved_data")[0].at("kind") == "unknown_field",
                    "Object inspection did not report a placeholder's saved data.");
            d.client.reset();
            editor.close();
            const auto remaining = discover(directory);
            require(bool(remaining), remaining ? "" : remaining.error().what());
            require(remaining->empty(), "Description editor descriptor remained live after close.");
            require(read(project / "Content/Scenes/Main.scene") == original_scene &&
                        sha256_file(project / "Content/Scenes/Main.scene") == *original_scene_hash,
                    "Description inspection changed the original saved scene.");
            std::cout << "Editor Control descriptions E2E: PASS | " << *root_text << '\n';
            return 0;
        }
        auto entity = current.at("entities")[0].at("handle").get<std::string>();
        auto component = current.at("entities")[0].at("components")[0].at("handle").get<std::string>();
        const auto stale = d.token;
        d.op("scene.rename", {{"name", "Control cube"}}, selection(entity));
        error(d.call("editor.inspect", {{"detail", "objects"}, {"cursor", directory_cursor}}), "stale_cursor");
        error(d.client->call(
                  {{"op", "op.call"},
                   {"args", {{"id", "scene.rename"}, {"args", {{"name", "Wrong"}}}, {"context", selection(entity)}}},
                   {"state_token", stale}}),
              "stale_state");
        d.op("scene.transform_set", {{"position", {{"x", 1}, {"y", 2}, {"z", 3}}}}, selection(entity));
        d.op("scene.transform_set", {{"position", {{"x", 4}, {"y", 2}, {"z", 3}}}}, selection(entity));
        current = d.inspect();
        require(current.at("entities")[0].at("transform").at("position")[0] == 4,
                "Transform operator did not write the document");
        d.op("edit.undo");
        current = d.inspect();
        require(current.at("entities")[0].at("transform").at("position")[0] == 1, "Undo did not restore the transform");
        d.op("edit.redo");
        verify_operators(d, entity, editor.window());
        const auto added = d.op("scene.add_component", {{"type", "world.Component"}}).at("inspected_object");
        current = d.inspect();
        require(current.at("entities")[0].at("components").size() == 2, "Component addition failed");
        d.op("scene.delete", {{"target", "object"}}, {{"selection", {{"entities", {entity}}, {"object", added}}}});
        current = d.inspect();
        require(current.at("entities")[0].at("components").size() == 1 &&
                    current.at("entities")[0].at("components")[0].at("handle") == component,
                "Component removal failed or replaced the existing mesh identity");
        const auto camera = current.at("camera");
        d.op("view.look_at", {{"eye", {{"x", 8}, {"y", 5}, {"z", -10}}}});
        d.op("view.camera_set", {{"state", camera}});
        d.op("view.set", {{"gtao", false}});
        current = d.inspect();
        require(current.at("view").at("gtao") == false && current.at("entities").size() == 2,
                "A World setting did not create the Environment");
        // Saved but unpublished code must not silently enter the runnable repro.
        write(project / "Source/NeverPublished.source", "invalid saved source");
        write(project / "Source/Published.ui.source", "invalid saved UI");
        std::this_thread::sleep_for(750ms);
        d.inspect();
        current = d.inspect();
        const auto dirty_undo = current.at("undo");
        const auto dirty_path = current.at("scene_path");
        const auto export_id = unique_id();
        require(d.call("repro.export", Json::object(), export_id).at("status") == "pending", "Export did not start");
        auto exported = d.wait(export_id);
        require(exported.at("status") == "ok", exported.dump());
        const auto repro_text = exported.at("result").at("directory").get<std::string>();
        const std::filesystem::path repro = utf8_to_utf16_validated(repro_text);
        const auto manifest = read(repro / "manifest.json");
        const auto frozen = read(repro / "Project" / utf8_to_utf16_validated(manifest.at("scene").get<std::string>()));
        require(!std::filesystem::exists(repro / "Project/Source/NeverPublished.source") &&
                    std::filesystem::exists(repro / "unpublished/Source/NeverPublished.source") &&
                    std::filesystem::exists(repro / "unpublished/Source/Published.ui.source") &&
                    !std::filesystem::exists(repro / "unpublished/Source/Published.source"),
                "Repro mixed live and unpublished source inputs");
        require(sha256_file(repro / "Project/Source/Published.ui.source") ==
                    sha256(std::as_bytes(std::span(published_ui))),
                "Repro lost the published UI source");
        require(manifest.at("publication_version") != 0 && manifest.at("ui_source_version") != 0,
                "Repro did not identify the full publication");
        // The mesh, and the Environment its World settings created.
        require(frozen.at("entities").size() == 2, "Export did not contain the dirty memory scene");
        require(read(project / "Content/Scenes/Main.scene") == original_scene,
                "Repro export overwrote the original scene");
        current = d.inspect();
        require(current.at("dirty") == true && current.at("undo") == dirty_undo &&
                    current.at("scene_path") == dirty_path,
                "Repro export changed authoring state");
        d.op("view.look_at", {{"eye", {{"x", 9}, {"y", 7}, {"z", -8}}}, {"target", {{"x", 4}, {"y", 2}, {"z", 3}}}});
        const auto base_view = current.at("view");
        const auto compare_id = unique_id();
        const auto prepare_id = unique_id();
        auto preparing = d.call("render.prepare", Json::object(), prepare_id);
        require(preparing.at("status") == "pending", preparing.dump());
        std::cout << "Editor Control: preparing cold Scene Lit pipelines\n" << std::flush;
        auto prepared = d.wait(prepare_id, 605s);
        require(prepared.at("status") == "ok", prepared.dump());
        require(prepared.at("result").at("phase") == "ready", "Preparation did not report an accepted ready frame");
        d.inspect();
        auto pending = d.call(
            "render.compare",
            {{"cases",
              Json::array(
                  {{{"name", "scene_lit"},
                    {"patch", {{"shading", "scene_lit"}, {"shadows", false}, {"ddgi", false}, {"gtao", false}}}}})},
             {"sample_frames", {1, 3}}},
            compare_id);
        require(pending.at("status") == "pending", pending.dump());
        auto compared = d.wait(compare_id);
        require(compared.at("status") == "ok", compared.dump());
        const auto& samples = compared.at("result").at("samples");
        require(samples.size() == 2, "Comparison sample count is incorrect");
        require(samples[0].at("accepted_ordinal") == 1 && samples[1].at("accepted_ordinal") == 3,
                "Accepted frame ordinals are incorrect");
        const auto& sample = samples[0];
        require(sample.at("effective_view").at("shading") == "scene_lit", "Scene Lit settings changed");
        const std::filesystem::path output =
            utf8_to_utf16_validated(compared.at("result").at("directory").get<std::string>());
        const auto width = sample.at("extent")[0].get<std::uint64_t>();
        const auto height = sample.at("extent")[1].get<std::uint64_t>();
        require(std::filesystem::file_size(output / utf8_to_utf16_validated(sample.at("depth").get<std::string>())) ==
                    width * height * 4,
                "Depth readback size is incorrect");
        require(std::filesystem::file_size(output / utf8_to_utf16_validated(sample.at("color").get<std::string>())) >
                    100,
                "PNG readback is missing");
        current = d.inspect();
        require(current.at("view") == base_view && current.at("undo") == dirty_undo,
                "Temporary comparison leaked into authoring state");
        verify_post_trace(d);
        auto cancelled_id = unique_id();
        const Json cancelled_request{{"op", "render.capture"},
                                     {"state_token", d.token},
                                     {"request_id", cancelled_id},
                                     {"args", {{"sample_frames", {4096}}}}};
        pending = d.client->call(cancelled_request);
        require(pending.at("status") == "pending", pending.dump());
        const auto unchanged = d.token;
        static_cast<void>(d.try_op("view.set", {{"probe_radius", 0}, {"probe_debug", true}}));
        require(d.inspect().at("view") == base_view, "Rejected debug patch partially mutated the view");
        d.op("view.camera_set", {{"state", d.inspect().at("camera")}});
        d.op("view.set");
        require(d.token == unchanged && d.call("job.status", {{"job_id", cancelled_id}}).at("status") == "pending",
                "Rejected or no-op requests cancelled unchanged rendering");
        error(d.call("job.cancel", {{"job_id", cancelled_id}}), "cancelled");
        error(d.call("job.cancel", {{"job_id", cancelled_id}}), "cancelled");
        for (unsigned i = 0; i < 260; ++i)
            d.op("view.set");
        error(d.client->call(cancelled_request), "request_id_expired");
        error(d.call("job.status", {{"job_id", cancelled_id}}), "cancelled");
        require(!d.inspect().contains("active_job"), "Expired request started another job with the same identity");
        auto changed_id = unique_id();
        pending = d.call("render.capture", {{"sample_frames", {4096}}}, changed_id);
        require(pending.at("status") == "pending", pending.dump());
        d.op("scene.rename", {{"name", "Changed during capture"}}, selection(entity));
        error(d.wait(changed_id), "state_changed");
        auto timed_id = unique_id();
        pending = d.call("render.capture", {{"sample_frames", {4096}}, {"timeout_ms", 1}}, timed_id);
        require(pending.at("status") == "pending", pending.dump());
        error(d.wait(timed_id), "timeout");
        const auto disconnected_id = unique_id();
        pending = d.call("render.capture", {{"sample_frames", {4096}}}, disconnected_id);
        require(pending.at("status") == "pending", pending.dump());
        d.client.reset();
        std::this_thread::sleep_for(80ms);
        auto reconnected = Client::create(descriptor);
        require(bool(reconnected), reconnected ? "" : reconnected.error().what());
        d.client = std::make_unique<Client>(std::move(*reconnected));
        error(d.wait(disconnected_id), "cancelled");
        d.inspect();
        d.op("scene.save");
        const auto saved = read(project / "Content/Scenes/Main.scene");
        require(saved.at("entities").size() == 2, "Explicit scene.save failed");
        const auto before_failed_open = d.inspect();
        // Hash-valid input can still fail compilation. Strict open must leave the
        // current document, object identities and project in place.
        write(repro / "Project/Source/Published.source", "invalid published source");
        auto broken = manifest;
        for (auto& file : broken.at("files"))
            if (file.at("path") == "Project/Source/Published.source") {
                const auto hash = sha256_file(repro / "Project/Source/Published.source");
                require(bool(hash), hash ? "" : hash.error());
                file["sha256"] = *hash;
                file["bytes"] = std::filesystem::file_size(repro / "Project/Source/Published.source");
            }
        write(repro / "manifest.json", broken.dump());
        error(d.call("repro.open", {{"directory", repro_text}}), "operation_failed");
        current = d.inspect();
        require(current.at("project") == before_failed_open.at("project") &&
                    current.at("entities") == before_failed_open.at("entities") &&
                    current.at("selection") == before_failed_open.at("selection"),
                "Failed repro open replaced the current document");
        write(repro / "Project/Source/Published.source", published_code);
        write(repro / "manifest.json", manifest.dump());
        d.ok("repro.open", {{"directory", repro_text}});
        current = d.inspect();
        require(current.at("entities").size() == 2 && current.at("entities")[0].at("name") == "Control cube" &&
                    !current.at("dirty").get<bool>(),
                "Repro reopen failed");
        error(d.try_op("scene.rename", {{"name", "Old handle"}}, selection(entity)), "stale_object");
        d.op("scene.delete");
        const auto remaining = d.inspect().at("entities");
        require(remaining.size() == 1 && remaining[0].at("components")[0].at("type") == "world.EnvironmentComponent",
                "Delete command did not remove the selected entity");
        d.op("edit.undo");
        require(d.inspect().at("entities").size() == 2, "Undo did not restore the entity");
        d.client.reset();
        Process cli(std::filesystem::path(argv[1]).parent_path() / "atelier_control.exe",
                    L"--list --directory " + quote(directory), root / "cli.json");
        require(WaitForSingleObject(cli.process, 10000) == WAIT_OBJECT_0, "CLI discovery did not finish");
        require(read(root / "cli.json").at("instances").size() == 1, "CLI did not discover the live editor");
        {
            std::ofstream request_file(root / "capture-request.json");
            request_file
                << Json{{"op", "render.capture"}, {"state_token", d.token}, {"args", {{"sample_frames", {1}}}}}.dump();
        }
        Process cli_capture(std::filesystem::path(argv[1]).parent_path() / "atelier_control.exe",
                            L"--directory " + quote(directory) + L" --instance " +
                                utf8_to_utf16_validated(descriptor.at("instance_id").get<std::string>()) +
                                L" --request " + quote(root / "capture-request.json"),
                            root / "cli-capture.json");
        require(WaitForSingleObject(cli_capture.process, 30000) == WAIT_OBJECT_0, "CLI capture did not finish");
        const auto cli_result = read(root / "cli-capture.json");
        require(cli_result.at("status") == "ok" && cli_result.at("result").at("samples").size() == 1,
                "CLI did not await its capture job");
        // Every editor step of the run, Control captures included, kept the timing hierarchy.
        auto observer = Client::create(descriptor);
        require(bool(observer), observer ? "" : observer.error().what());
        d.client = std::make_unique<Client>(std::move(*observer));
        const auto cpu = d.inspect().at("performance").at("cpu");
        require(cpu.at("structure_errors") == 0 && cpu.at("first_structure_error").is_null(),
                "An editor step recorded a frame timing phase under the wrong parent: " + cpu.dump());
        editor.close();
        const auto live_instances = discover(directory);
        require(bool(live_instances), live_instances ? "" : live_instances.error().what());
        require(live_instances->empty(), "Closed editor descriptor remained live");
        std::cout << "Editor Control E2E: PASS | " << *root_text << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Editor Control E2E: FAIL: " << error.what() << '\n';
        return 1;
    }
}
