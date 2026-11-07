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
using namespace atelier;

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
    write(project / "Content/Scenes/Main.scene",
          R"({"version":10,"next_id":"1","roots":[],"entities":[],"materials":[]})");
    write(project / "Source/Published.source", published_code);
    write(project / "Source/Published.ui.source", published_ui);
    std::filesystem::create_directories(project / "Cache");
    write(
        project / "Config/Project.json",
        R"({"version":1,"name":"Control review","startup_scene":"Content/Scenes/Main.scene","ui_source":"Source/Published.ui.source"})");
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

    // A new editor records its surface only after serving that step's requests, so the token of a
    // first inspection can be stale by the next call. Five unchanged inspections, one per step, settle it.
    json::Document settle() {
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

    json::Document wait(std::string id, std::chrono::seconds timeout = 125s) {
        json::Builder args;
        require(bool(args.set(args.root(), "job_id", id)), "Invalid job ID");
        const auto request = args.finish();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto result = call("job.status", request);
            const auto status = result.root().string({"status"});
            require(bool(status), "Missing job status");
            if (*status != "pending")
                return result;
            std::this_thread::sleep_for(20ms);
        }
        throw std::runtime_error("Control job did not complete");
    }
};

void verify_post_trace(Driver& d) {
    const auto before = d.inspect();
    const auto view = *before.root().at("view");
    // One representative, two states. Expectations come from the recorded
    // shader contract, not from trace formatting or the observation callback.
    const std::array<std::vector<std::string>, 2> entries{
        {{"lit_taa_cs", "lit_bloom_downsample_cs", "lit_bloom_blur_cs", "lit_bloom_blur_cs", "lit_tonemap_cs",
          "lit_smaa_edges_cs", "lit_smaa_weights_cs", "lit_smaa_neighborhood_cs", "lit_smaa_resolve_cs"},
         {"lit_taa_cs", "lit_tonemap_cs"}}};
    for (std::size_t state = 0; state < entries.size(); ++state) {
        d.op("view.set",
             state == 0 ? json::Document::literal<R"({"shading":"scene_lit","bloom":true,"antialiasing":"smaa_t2x"})">()
                        : json::Document::literal<R"({"shading":"scene_lit","bloom":false,"antialiasing":"off"})">());
        const auto selected = d.inspect();
        const auto id = unique_id();
        auto pending = d.call("render.trace", json::Document::literal<R"({"scope":"scene_lit_post"})">(), id);
        require(pending.root().string({"status"}) == "pending", "Post trace did not start");
        const auto finished = d.wait(id);
        require(finished.root().string({"status"}) == "ok", "Post trace failed");
        const auto result = *finished.root().at("result");
        const auto trace = read(utf8_to_utf16_validated(*result.string({"json"})));
        const auto frame_id = trace.root().u64({"ui_frame_id"});
        const auto recording_sequence = trace.root().u64({"recording_sequence"});
        require(trace.root().string({"schema"}) == "atelier.render.trace" && trace.root().u32({"version"}) == 1 &&
                    trace.root().string({"observation", "status"}) == "complete" &&
                    trace.root().string({"frame_status"}) == "accepted" &&
                    trace.root().string({"attempt"}) == "first_requested_view" && frame_id && *frame_id != 0 &&
                    recording_sequence && *recording_sequence != 0,
                "Post trace did not preserve the complete actual render attempt");
        const auto commands = *trace.root().at("commands");
        require(commands.size() == entries[state].size(), "Post trace has a missing or invented dispatch");
        std::ifstream page(utf8_to_utf16_validated(*result.string({"html"})), std::ios::binary);
        require(bool(page), "Trace HTML is missing");
        const std::string html{std::istreambuf_iterator<char>(page), std::istreambuf_iterator<char>()};
        std::size_t position{};
        for (std::size_t i = 0; i < commands.size(); ++i) {
            const auto command = *commands.at(i);
            require(command.u64({"ordinal"}) == i + 1 && command.string({"entry"}) == entries[state][i] &&
                        command.string({"kind"}) == "dispatch" && *command.at("groups")->at(0)->u32() > 0 &&
                        *command.at("groups")->at(1)->u32() > 0 && command.at("groups")->at(2)->u32() == 1,
                    "Post trace dispatch order or dimensions differ from the actual chain");
            position = html.find("id=\"command-" + std::to_string(i + 1) + "\"", position);
            require(position != std::string::npos, "Trace SVG omitted or reordered a command");
            const auto end = html.find("</g>", position);
            require(html.substr(position, end - position).find(entries[state][i]) != std::string::npos,
                    "Trace SVG command label differs from its observation");
            position = end;
            std::set<std::pair<unsigned, unsigned>> slots;
            for (const auto binding : *command.at("bindings")->array()) {
                const auto kind = binding.string({"kind"});
                require(binding.string({"status"}) == "valid" && kind && *kind != "unsupported",
                        "Normal post chain has a dummy or unobserved binding");
                require(slots.emplace(*binding.u32({"root_parameter"}), *binding.u32({"array_index"})).second,
                        "Post trace repeated a root binding instead of resolving installed state");
            }
        }
        const auto resource = [&](std::size_t command, std::string_view name, std::string_view kind) -> std::uint64_t {
            for (const auto binding : *commands.at(command)->at("bindings")->array())
                if (binding.string({"name"}) == name) {
                    require(binding.string({"kind"}) == kind && binding.u64({"resource"}) &&
                                binding.u32({"view", "first_mip"}) == 0 && binding.u32({"view", "mip_count"}) == 1,
                            "Post trace lost the actual texture view");
                    const auto start = html.find("id=\"binding-" + std::to_string(command + 1) + "-" +
                                                 std::to_string(*binding.u32({"root_parameter"})) + "-" +
                                                 std::to_string(*binding.u32({"array_index"})) + "\"");
                    require(start != std::string::npos, "Trace SVG omitted a binding port");
                    const auto port = html.substr(start, html.find("</g>", start) - start);
                    require(port.find(name) != std::string::npos && port.find(kind) != std::string::npos &&
                                port.find(">R" + std::to_string(*binding.u64({"resource"})) + "</text>") !=
                                    std::string::npos,
                            "Trace SVG binding port differs from the actual resource");
                    return *binding.u64({"resource"});
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
                        *commands.at(2)->at("kernel") == *commands.at(3)->at("kernel"),
                    "Bloom blur instances lost their actual ping-pong bindings");
            const auto spatial = resource(7, "lit_smaa_spatial_output", "texture_uav");
            require(resource(8, "lit_smaa_spatial", "texture_srv") == spatial &&
                        resource(8, "lit_smaa_previous", "texture_srv") != spatial,
                    "SMAA current and history bindings were conflated");
            for (const auto binding : *commands.at(5)->at("bindings")->array()) {
                const auto name = binding.string({"name"});
                require(name && *name != "lit_smaa_spatial_output" && *name != "lit_smaa_previous",
                        "Trace treated unused SMAA setters as actual bindings");
            }
        }
        const auto after = d.inspect();
        require(*after.root().at("view") == *selected.root().at("view") &&
                    *after.root().at("undo") == *selected.root().at("undo"),
                "Tracing changed display or author history");
        std::cout << "Editor Control post trace " << state << ": " << *result.string({"directory"}) << '\n';
    }
    // Both Bloom and AA are World author settings. The second state always
    // changes them; the first changes them unless it already matches the base.
    d.op("edit.undo", {}, json::Document::literal<R"({"asset":"","region":"viewport"})">());
    const auto bloom = view.boolean({"bloom"});
    const auto antialiasing = view.string({"antialiasing"});
    require(bloom && antialiasing, "Missing original post settings");
    if (!*bloom || *antialiasing != "smaa_t2x")
        d.op("edit.undo", {}, json::Document::literal<R"({"asset":"","region":"viewport"})">());
    json::Builder restore;
    require(bool(restore.set(restore.root(), "shading", *view.at("shading"))), "Cannot restore shading");
    d.op("view.set", restore.finish());
    const auto after = d.inspect();
    require(*after.root().at("view") == view && *after.root().at("undo") == *before.root().at("undo"),
            "Post trace representative did not restore its settings");
}

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
        const auto config = read(project / "Config/Project.json");
        require(state.root().string({"project"}) == *project_text &&
                    *state.root().at("scene_path") == *config.root().at("startup_scene") &&
                    state.root().at("last_frame")->size() != 0,
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

void error(const json::Document& result, std::string_view expected) {
    const auto bytes = json::write(result.root());
    require(result.root().string({"status"}) == "error" && result.root().string({"error", "code"}) == expected,
            {bytes.data(), bytes.size()});
}

json::Document selection(const std::string& entity) {
    json::Builder result{json::Document::literal<R"({"selection":{"entities":[]}})">()};
    require(bool(result.append(*result.root().path({"selection", "entities"}), entity)), "Invalid selected entity");
    return result.finish();
}

json::Value material_entry(const json::Document& material, std::string_view path) {
    for (auto entry : *material.root().at("entries")->array())
        if (entry.string({"path"}) == path)
            return entry;
    throw std::runtime_error("Material lacks the sparse author entry " + std::string(path));
}

bool material_issue(const json::Document& material, std::string_view text) {
    for (auto issue : *material.root().at("diagnostics")->array())
        if (issue.string({"message"})->find(text) != std::string_view::npos)
            return true;
    return false;
}

void repro_engine_assets(Driver& d, const std::filesystem::path& root) {
    const auto exported = [&] {
        const auto id = unique_id();
        const auto started = d.call("repro.export", {}, id);
        require(started.root().string({"status"}) == "pending", "Engine repro export did not start");
        const auto done = d.wait(id);
        require(done.root().string({"status"}) == "ok", "Engine repro export failed");
        return std::filesystem::path(utf8_to_utf16_validated(*done.root().string({"result", "directory"})));
    };
    const auto empty = exported();
    const auto empty_text = platform::utf16_to_utf8(empty.native());
    require(bool(empty_text), "Invalid UTF-16 empty repro path");
    const auto empty_manifest = read(empty / "manifest.json");
    require(*empty_manifest.root().at("engine_assets") ==
                json::Document::literal<R"({"version":1,"records":[]})">().root(),
            "An empty no-project document acquired unused installation assets");
    require(*empty_manifest.root().at("material_resources") ==
                json::Document::literal<R"({"version":2,"records":{}})">().root(),
            "An empty repro omitted its current, empty material digest set");
    require(empty_manifest.root().u32({"json_bytes_version"}) == 2, "Repro omitted its JSON byte encoding version");
    d.ok("op.call", json::Document::literal<R"({"id":"scene.add","args":{"kind":"mesh","mesh":"builtin/cube"}})">());
    const auto bundle = exported();
    const auto original = read(bundle / "manifest.json");
    const auto records = *original.root().path({"engine_assets", "records"});
    require(records.size() == 2 && records.at(0)->string({"key"}) == "builtin/cube" &&
                records.at(1)->string({"key"}) == "builtin/material/default-grid",
            "No-project export did not freeze its mesh and implicit DefaultWorldGrid MS");
    for (const auto record : *records.array()) {
        const bool material = record.string({"key"}) == "builtin/material/default-grid";
        require(record.string({"kind"}) == (material ? "material" : "mesh") && record.u32({"digest_version"}) == 2,
                "Engine manifest did not identify material and mesh digest v2");
    }
    const auto copy = root / "EngineRepro";
    const auto native_bundle = platform::windows_file_path(bundle);
    const auto native_copy = platform::windows_file_path(copy);
    require(native_bundle && native_copy, "Cannot resolve repro copy paths");
    std::filesystem::copy(*native_bundle, *native_copy, std::filesystem::copy_options::recursive);
    const auto bundle_text = platform::utf16_to_utf8(bundle.native());
    const auto copy_text = platform::utf16_to_utf8(copy.native());
    require(bundle_text && copy_text, "Invalid UTF-16 repro copy path");
    const auto scene_path = copy / "Project" / utf8_to_utf16_validated(*original.root().string({"scene"}));
    const auto scene = read(scene_path);
    const auto publish = [&](const json::Document& manifest, const json::Document& value) {
        const auto bytes = json::write(value.root(), {.pretty = true});
        write(scene_path, {bytes.data(), bytes.size()});
        json::Builder updated{manifest};
        for (auto file : *updated.root().at("files")->array())
            if (copy / utf8_to_utf16_validated(*file.string({"path"})) == scene_path) {
                const auto hash = sha256_file(scene_path);
                require(bool(hash), hash ? "" : hash.error());
                require(updated.set(file, "sha256", *hash) &&
                            updated.set(file, "bytes", std::filesystem::file_size(scene_path)),
                        "Cannot update scene hash");
            }
        const auto manifest_bytes = json::write(updated.root(), {.pretty = true});
        write(copy / "manifest.json", {manifest_bytes.data(), manifest_bytes.size()});
    };
    d.op("edit.undo");
    json::Builder open;
    require(bool(open.set(open.root(), "directory", *bundle_text)), "Invalid bundle path");
    d.ok("repro.open", open.finish());
    const auto state = d.inspect();
    const std::string entity{*state.root().at("entities")->at(0)->string({"handle"})};
    d.op("scene.rename", json::Document::literal<R"({"name":"Saved with history"})">(), selection(entity));
    d.op("scene.save");
    d.op("scene.select", {}, selection(entity));
    d.op("view.look_at", json::Document::literal<R"({"eye":{"x":7,"y":4,"z":-9},"target":{"x":1,"z":2}})">());
    d.settle();
    json::Builder incompatible{original};
    require(bool(incompatible.set(*incompatible.root().path({"engine_assets", "records"})->at(0), "content_sha256",
                                  std::string(64, '0'))),
            "Cannot damage engine hash");
    publish(incompatible.finish(), scene);
    require(bool(open.set(open.root(), "directory", *copy_text)), "Invalid copy path");
    {
        const auto before = d.inspect();
        const auto token = d.token;
        require(!*before.root().boolean({"dirty"}) && *before.root().u32({"undo"}) > 0 &&
                    before.root().at("selection")->size() != 0,
                "Dependency atomicity fixture is not a saved, replaceable document with history and selection");
        static_cast<void>(d.call("repro.open", open.finish()));
        const auto after = d.inspect();
        for (const auto* field : {"project", "entities", "selection", "camera", "view", "undo", "redo", "dirty"})
            require(*after.root().at(field) == *before.root().at(field),
                    "Rejected engine dependency changed " + std::string(field));
        require(d.token == token, "Rejected engine dependency changed the state token");
    }
    for (const bool missing : {true, false}) {
        json::Builder legacy{original};
        if (missing)
            legacy.erase(legacy.root(), "json_bytes_version");
        else
            require(bool(legacy.set(legacy.root(), "json_bytes_version", 1u)), "Cannot set legacy JSON version");
        publish(legacy.finish(), scene);
        const auto before = d.inspect();
        const auto token = d.token;
        const auto rejected = d.call("repro.open", open.finish());
        require(rejected.root().string({"status"}) == "error" &&
                    rejected.root().string({"error", "code"}) == "invalid_request",
                "Repro accepted missing or obsolete JSON byte encoding");
        const auto after = d.inspect();
        for (const auto* field : {"project", "entities", "selection", "camera", "view", "undo", "redo", "dirty"})
            require(*after.root().at(field) == *before.root().at(field),
                    "Legacy JSON version rejection changed " + std::string(field));
        require(d.token == token, "Legacy JSON version rejection changed the state token");
    }
    publish(original, scene);
    d.ok("repro.open", open.finish());
    const auto recovered = d.inspect();
    require(recovered.root().at("entities")->size() == scene.root().at("entities")->size() &&
                recovered.root().u32({"undo"}) == 0 &&
                *recovered.root().at("entities")->at(0)->at("name") == *scene.root().at("entities")->at(0)->at("name"),
            "Restoring a valid manifest did not reopen the original document after dependency rejection");
    require(bool(open.set(open.root(), "directory", *empty_text)), "Invalid empty bundle path");
    d.ok("repro.open", open.finish());
    const auto cleared = d.inspect();
    require(cleared.root().at("entities")->size() == 0, "Empty engine dependency set did not reopen");

    // A cached but unused material and an obsolete disk texture are not roots.
    // Only the referenced, published draft belongs to the frozen document.
    const auto project = root / "EngineDraftProject";
    project_fixture(project);
    const auto project_text = platform::utf16_to_utf8(project.native());
    require(bool(project_text), "Invalid UTF-16 draft project path");
    json::Builder project_args;
    require(bool(project_args.set(project_args.root(), "path", *project_text)), "Invalid project path");
    d.op("project.open", project_args.finish());
    const auto used =
        d.op("asset.new_material", json::Document::literal<R"({"path":"Content/Materials/Used.material"})">(),
             json::Document::literal<R"({"asset":""})">());
    const auto cached =
        d.op("asset.new_material", json::Document::literal<R"({"path":"Content/Materials/Cached.material"})">(),
             json::Document::literal<R"({"asset":""})">());
    for (const auto* name : {"Used", "Cached"}) {
        const auto file = project / "Content/Materials" / (std::string(name) + ".material");
        json::Builder value{read(file)};
        require(
            bool(value.set(
                value.root(), "entries",
                json::Document::literal<
                    R"([{"path":"base_color_texture","type":"texture","enabled":true,"value":{"asset_key":"builtin/unknown","address":"repeat"}}])">()
                    .root())),
            "Cannot construct missing-texture fixture");
        const auto bytes = json::write(value.root(), {.pretty = true});
        write(file, {bytes.data(), bytes.size()});
    }
    d.op("project.open", project_args.finish());
    json::Builder material_args;
    require(bool(material_args.set(material_args.root(), "asset", *cached.root().path({"result", "asset"}))),
            "Invalid cached asset");
    d.ok("material.inspect", material_args.finish());
    require(bool(material_args.set(material_args.root(), "asset", *used.root().path({"result", "asset"}))),
            "Invalid used asset");
    const auto loaded = d.ok("material.inspect", material_args.finish());
    require(material_issue(loaded, "builtin/unknown"),
            "Unknown-engine draft fixture did not retain its missing texture diagnostic");
    d.ok("op.call", json::Document::literal<R"({"id":"scene.add","args":{"kind":"mesh"}})">());
    const auto placed = d.inspect();
    json::Builder assign{material_args.root()};
    require(assign.set(assign.root(), "component",
                       *placed.root().at("entities")->at(0)->at("components")->at(0)->at("handle")) &&
                assign.set(assign.root(), "slot", "slot/0"),
            "Cannot assign material");
    d.ok("material.assign", assign.finish());
    d.op("scene.save");
    json::Builder edit{json::Document::literal<R"({"path":"base_color_texture","texture":""})">()};
    require(bool(edit.set(edit.root(), "object", *loaded.root().at("handle"))), "Invalid material handle");
    d.ok("material.set_texture", edit.finish());
    const auto draft = exported();
    require(!std::filesystem::exists(draft / "Project/Content/Materials/Cached.material") &&
                material_entry(read(draft / "Project/Content/Materials/Used.material"), "base_color_texture")
                        .string({"value", "asset_key"}) == "" &&
                material_entry(read(project / "Content/Materials/Used.material"), "base_color_texture")
                        .string({"value", "asset_key"}) == "builtin/unknown",
            "Frozen draft roots included an unused cache, ignored the live draft or changed its source file");
    json::Builder context{material_args.root()};
    require(bool(context.set(context.root(), "region", "parameters")), "Invalid material context");
    d.op("edit.undo", {}, context.finish());
    const auto draft_text = platform::utf16_to_utf8(draft.native());
    require(bool(draft_text), "Invalid UTF-16 draft repro path");
    require(bool(open.set(open.root(), "directory", *draft_text)), "Invalid draft path");
    d.ok("repro.open", open.finish());
    const auto inspected = d.ok("material.inspect", material_args.finish());
    require(inspected.root().at("diagnostics")->size() == 0, "Published draft did not reopen strictly");
    json::Builder report{json::Document::literal<
        R"({"status":"passed","cases":["no-project-empty-and-mesh-with-grid","saved-history-selection-camera-preserved","valid-manifest-reopen","material-kind-and-digest-v2","mesh-digest-v2","unused-cache-and-superseded-disk-not-roots","referenced-draft"]})">()};
    require(bool(report.set(report.root(), "records", records)), "Cannot record engine assets");
    const auto report_bytes = json::write(report.root(), {.pretty = true});
    write(root / "engine-assets.json", {report_bytes.data(), report_bytes.size()});
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
    require(!*before.root().boolean({"busy"}) && capture() == nullptr,
            "Operator admission fixture already owns an interaction or capture");
    const auto directory = d.ok("op.list");
    const auto operators = *directory.root().at("operators")->array();
    const auto entry = [&](std::string_view id) -> json::Value {
        for (auto item : operators)
            if (item.string({"id"}) == id)
                return item;
        throw std::runtime_error("Operator directory omitted " + std::string(id));
    };
    require(*entry("view.select_box").at("kinds") == json::Document::literal<R"(["interactive"])">().root() &&
                *entry("view.reset").at("kinds") == json::Document::literal<R"(["direct"])">().root() &&
                *entry("scene.duplicate").at("kinds") ==
                    json::Document::literal<R"(["direct","interactive"])">().root() &&
                entry("scene.duplicate").boolean({"parameters", "properties", "place", "default"}) == false &&
                !entry("scene.duplicate").string({"interaction"})->empty(),
            "Operator directory did not distinguish direct, interactive and argument-dependent invocation");
    const auto queried = d.inspect();
    require(d.token == token, "Operator discovery changed the state token");
    for (const auto key : {"entities", "selection", "dirty", "undo", "redo", "busy", "busy_reasons"})
        require(*queried.root().at(key) == *before.root().at(key), "Operator discovery changed " + std::string(key));
    error(d.try_op("view.select_box", {}, json::Document::literal<R"({"region":"viewport"})">()),
          "interaction_required");
    require(capture() == nullptr, "External box selection acquired editor capture");
    const auto refused = d.inspect();
    require(d.token == token, "Refused box selection changed the state token");
    for (const auto key : {"entities", "selection", "dirty", "undo", "redo", "busy", "busy_reasons"})
        require(*refused.root().at(key) == *before.root().at(key), "Refused box selection changed " + std::string(key));
    const auto entries = *before.root().u64({"undo"});
    const auto added = d.ok("op.call", json::Document::literal<R"({"id":"scene.add","args":{"kind":"point"}})">());
    const auto light = *added.root().string({"active"});
    const auto after = d.inspect();
    bool found = false;
    for (auto item : *after.root().at("entities")->array())
        found |= item.string({"handle"}) == light;
    require(after.root().u64({"undo"}) == entries + 1 && after.root().at("selection")->size() == 1 &&
                after.root().at("selection")->at(0)->string() == light && found,
            "scene.add did not add and select one Entity in one history entry");
    d.op("edit.undo");
    const auto undone = d.inspect();
    require(undone.root().u64({"undo"}) == entries, "Undo did not remove the added Entity");
    d.op("scene.select", {}, selection(entity));
    const auto restored = d.inspect();
    require(restored.root().at("selection")->size() == 1 && restored.root().at("selection")->at(0)->string() == entity,
            "scene.select did not restore the selection");
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
            d.ok("editor.inspect", json::Document::literal<R"({"detail":"summary"})">());
            stable = before == d.token ? stable + 1 : 0;
        }
        require(stable == 5, "Refresh did not settle");
    };
    const auto saved = sha256_file(project / "Content/Scenes/Main.scene");
    require(bool(saved), saved ? "" : saved.error());
    const auto full = d.inspect();
    const auto page = [&](const json::Document& args = {}) {
        json::Builder request{args};
        require(bool(request.set(request.root(), "detail", "assets")), "Cannot build assets query");
        return d.ok("editor.inspect", request.finish());
    };
    const auto first = page(json::Document::literal<R"({"limit":1})">());
    const auto cursor = *first.root().at("next_cursor");
    require(!cursor.is_null() && first.root().string({"detail"}) == "assets",
            "Small project did not return an assets continuation");
    json::Builder args{json::Document::literal<R"({"limit":1})">()};
    require(bool(args.set(args.root(), "cursor", cursor)), "Cannot build assets continuation");
    const auto continuation_args = args.finish();
    const auto continuation = page(continuation_args);
    write(project / "Content/bad.bmp", "not decoded by catalog");
    write(project / "Content/bad.bmp.asset.json", "{broken");
    refresh();
    require(page(continuation_args) == continuation, "Failed refresh replaced the published directory");
    fs::remove(project / "Content/bad.bmp.asset.json");
    fs::remove(project / "Content/bad.bmp");
    refresh();
    const auto restored = d.inspect();
    require(*restored.root().at("assets") == *full.root().at("assets") &&
                sha256_file(project / "Content/Scenes/Main.scene") == *saved,
            "Failed refresh or fixture cleanup changed the original saved document/catalog");
}

void verify_option_descriptions(const json::Document& capabilities) {
    const auto count = *capabilities.root().path({"display_options", "probe_volume_0_count_x"});
    require(count.string({"type"}) == "integer" && count.number({"minimum"}) == 2 && count.number({"maximum"}) == 32 &&
                count.string({"scope"}) == "scene",
            "Metadata changed option types, bounds or ownership");
}

json::Document verify_object_directory(Driver& d, const json::Document& full) {
    const auto state_token = d.token;
    const auto entity = *full.root().at("entities")->at(0);
    const auto component = *entity.at("components")->at(0);
    json::Builder args{json::Document::literal<R"({"detail":"objects","limit":1})">()};
    require(bool(args.set(args.root(), "entity_name", *entity.at("name"))), "Cannot build objects query");
    const auto page = d.ok("editor.inspect", args.finish());
    const auto first_cursor = *page.root().at("next_cursor");
    json::Builder next{json::Document::literal<R"({"detail":"objects"})">()};
    require(bool(next.set(next.root(), "cursor", first_cursor)), "Cannot build objects continuation");
    const auto rest = d.ok("editor.inspect", next.finish());
    require(*page.root().at("objects")->at(0)->at("handle") == *entity.at("handle") &&
                *rest.root().at("objects")->at(0)->at("handle") == *component.at("handle") &&
                rest.root().at("next_cursor")->is_null(),
            "Object cursor did not retain its exact entity-name filter");
    const auto unchanged = d.inspect();
    require(d.token == state_token && *unchanged.root().at("entities") == *full.root().at("entities"),
            "Read-only object discovery changed the document or its state token");
    return json::Document{first_cursor};
}

// Only the scene records the sky map and its rotation; the frozen view holds view options.
void repro_sky(Driver& d, const std::filesystem::path& project) {
    const std::string key = "project/{6F1D2C3B-4A5E-4F60-8B7C-9D0E1F2A3B4C}";
    std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n";
    for (unsigned i = 0; i < 32; ++i)
        hdr += std::string{char(64 + 4 * i), char(128), char(32), char(129)};
    write(project / "Content/Sky/Repro.hdr", hdr);
    json::Builder identity{json::Document::literal<R"({"version":1})">()};
    require(bool(identity.set(identity.root(), "id", key.substr(8))), "Invalid sky identity");
    const auto identity_bytes = json::write(identity.root());
    write(project / "Content/Sky/Repro.hdr.asset.json", {identity_bytes.data(), identity_bytes.size()});
    const auto environment_of = [](json::Value scene) -> json::Value {
        for (auto entity : *scene.at("entities")->array())
            for (auto component : *entity.at("components")->array())
                if (component.string({"type"}) == "world.EnvironmentComponent")
                    return component;
        throw std::runtime_error("The repro lighting scene has no Environment.");
    };
    json::Builder scene{read(project / "Content/Scenes/Main.scene")};
    const auto environment = environment_of(scene.root());
    require(scene.set(environment, "sky_map", key) && scene.set(*environment.at("properties"), "sky_rotation", 30u),
            "Cannot set sky fixture");
    const auto scene_bytes = json::write(scene.root(), {.pretty = true});
    write(project / "Content/Scenes/Main.scene", {scene_bytes.data(), scene_bytes.size()});
    const auto project_text = platform::utf16_to_utf8(project.native());
    require(bool(project_text), "Invalid UTF-16 sky project path");
    json::Builder open_project;
    require(bool(open_project.set(open_project.root(), "path", *project_text)), "Invalid sky project path");
    d.op("project.open", open_project.finish());
    const auto exported = [&] {
        const auto id = unique_id();
        const auto started = d.call("repro.export", {}, id);
        require(started.root().string({"status"}) == "pending", "Sky repro export did not start");
        const auto done = d.wait(id);
        require(done.root().string({"status"}) == "ok", "Sky repro export failed");
        const std::filesystem::path bundle = utf8_to_utf16_validated(*done.root().string({"result", "directory"}));
        const auto manifest = read(bundle / "manifest.json");
        const auto frozen = read(bundle / "Project" / utf8_to_utf16_validated(*manifest.root().string({"scene"})));
        const auto sky = environment_of(frozen.root());
        require(sky.string({"sky_map"}) == key && sky.number({"properties", "sky_rotation"}) == 30,
                "A sky repro lost its scene's sky map key or rotation");
        return bundle;
    };
    const auto bundle = exported();
    const auto bundle_text = platform::utf16_to_utf8(bundle.native());
    require(bool(bundle_text), "Invalid UTF-16 sky repro path");
    json::Builder open_bundle;
    require(bool(open_bundle.set(open_bundle.root(), "directory", *bundle_text)), "Invalid sky bundle path");
    d.ok("repro.open", open_bundle.finish());
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
    json::Builder project_args;
    require(bool(project_args.set(project_args.root(), "path", *project_text)), "Invalid project path");
    d.op("project.open", project_args.finish());
    write(root / "Draft.png", std::string(tiny_png.begin(), tiny_png.end()));
    json::Builder import{json::Document::literal<R"({"kind":"texture"})">()};
    require(bool(import.set(import.root(), "path", *texture_text)), "Invalid texture path");
    const auto imported = d.op("asset.import", import.finish());
    const auto texture = *imported.root().string({"result", "asset"});
    const auto asset =
        d.op("asset.new_material", json::Document::literal<R"({"path":"Content/Materials/Draft.material"})">(),
             json::Document::literal<R"({"asset":""})">());
    const auto unused =
        d.op("asset.new_material", json::Document::literal<R"({"path":"Content/Materials/Unused.material"})">(),
             json::Document::literal<R"({"asset":""})">());
    json::Builder material;
    require(bool(material.set(material.root(), "asset", *asset.root().path({"result", "asset"}))),
            "Invalid material key");
    const auto initial = d.ok("material.inspect", material.finish());
    json::Builder edit{json::Document::literal<R"({"path":"base_color_texture"})">()};
    require(edit.set(edit.root(), "object", *initial.root().at("handle")) && edit.set(edit.root(), "texture", texture),
            "Cannot construct texture edit");
    d.ok("material.set_texture", edit.finish());
    d.ok("material.save", material.finish());
    const auto added = d.ok("op.call", json::Document::literal<R"({"id":"scene.add","args":{"kind":"mesh"}})">());
    const auto entity = *added.root().string({"active"});
    json::Builder assign{material.root()};
    require(assign.set_null(assign.root(), "component") && assign.set(assign.root(), "slot", "slot/0"),
            "Cannot construct material assignment");
    const auto state = d.inspect();
    for (auto item : *state.root().at("entities")->array())
        if (item.string({"handle"}) == entity)
            require(bool(assign.set(assign.root(), "component", *item.at("components")->at(0)->at("handle"))),
                    "Invalid component handle");
    d.ok("material.assign", assign.finish());
    d.op("scene.save");
    const auto saved = project / "Content/Materials/Draft.material";
    const auto saved_json = read(saved);
    const auto saved_text = json::write(saved_json.root());
    for (const auto* file : {L"Content/Textures/Draft.png", L"Content/Textures/Draft.png.asset.json"}) {
        const auto path = platform::windows_file_path(project / file);
        require(bool(path), path ? "" : platform::error_message(path.error()));
        std::filesystem::remove(*path);
    }
    d.op("project.open", project_args.finish());
    const auto opened = d.ok("material.inspect", material.finish());
    require(material_issue(opened, texture), "The draft fixture did not lose its texture");
    json::Builder cached;
    require(bool(cached.set(cached.root(), "asset", *unused.root().path({"result", "asset"}))),
            "Invalid unused material key");
    d.ok("material.inspect", cached.finish());
    require(edit.set(edit.root(), "object", *opened.root().at("handle")) && edit.set(edit.root(), "texture", ""),
            "Cannot clear texture");
    d.ok("material.set_texture", edit.finish());
    const auto id = unique_id();
    const auto started = d.call("repro.export", {}, id);
    require(started.root().string({"status"}) == "pending", "Draft repro export did not start");
    const auto done = d.wait(id);
    require(done.root().string({"status"}) == "ok", "A material draft without its missing texture did not export");
    const std::filesystem::path bundle = utf8_to_utf16_validated(*done.root().string({"result", "directory"}));
    const auto resaved = read(saved);
    const auto resaved_text = json::write(resaved.root());
    require(material_entry(read(bundle / "Project/Content/Materials/Draft.material"), "base_color_texture")
                        .string({"value", "asset_key"}) == "" &&
                std::string_view{resaved_text.data(), resaved_text.size()} ==
                    std::string_view{saved_text.data(), saved_text.size()},
            "A draft repro did not carry the draft, or changed the saved material");
    const auto unused_path = platform::windows_file_path(bundle / "Project/Content/Materials/Unused.material");
    require(bool(unused_path), unused_path ? "" : platform::error_message(unused_path.error()));
    const auto manifest = read(bundle / "manifest.json");
    bool copied_unused = false;
    for (auto file : *manifest.root().at("input_files")->array())
        copied_unused |= file.string({"path"})->starts_with("Content/Materials/Unused");
    require(!std::filesystem::exists(*unused_path) && !copied_unused,
            "A repro copied a material the session loaded but the scene does not reference");
    json::Builder context{material.root()};
    require(bool(context.set(context.root(), "region", "parameters")), "Invalid material context");
    d.op("edit.undo", {}, context.finish());
    const auto bundle_text = platform::utf16_to_utf8(bundle.native());
    require(bool(bundle_text), "Invalid UTF-16 material draft repro path");
    json::Builder open;
    require(bool(open.set(open.root(), "directory", *bundle_text)), "Invalid bundle path");
    d.ok("repro.open", open.finish());
    const auto reopened = d.ok("material.inspect", material.finish());
    require(reopened.root().at("diagnostics")->size() == 0, "A draft repro did not reopen without the missing texture");
}

// Exercise the real author/Runtime/export/strict-load path. Pixel expectations
// come from this RGBA fixture, not from another call to the exporter's decoder.
void repro_material_inputs(Driver& d, const std::filesystem::path& root) {
    const auto project = root / "Project";
    project_fixture(project);
    const auto project_text = platform::utf16_to_utf8(project.native());
    require(bool(project_text), "Invalid UTF-16 material input fixture path");
    json::Builder project_args;
    require(bool(project_args.set(project_args.root(), "path", *project_text)), "Invalid project path");
    d.op("project.open", project_args.finish());
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
    json::Builder import{json::Document::literal<R"({"kind":"texture"})">()};
    require(bool(import.set(import.root(), "path", *active_text)), "Invalid active texture path");
    const auto imported = d.op("asset.import", import.finish());
    const auto texture = *imported.root().string({"result", "asset"});
    require(bool(import.set(import.root(), "path", *dormant_text)), "Invalid dormant texture path");
    const auto imported_dormant = d.op("asset.import", import.finish());
    const auto dormant = *imported_dormant.root().string({"result", "asset"});
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
    json::Builder create;
    require(create.set(create.root(), "path", source_file) && create.set(create.root(), "source", source_text),
            "Cannot construct material source");
    const auto created_source = d.op("asset.new_material_source", create.finish());
    const auto source = *created_source.root().string({"result", "asset"});
    json::Builder source_author{read(project / source_file)};
    const auto source_path = *source_author.root().string({"source_path"});
    const auto native_source_path = utf8_to_utf16_validated(source_path);
    const auto policy = source_author.root().at("policy");
    require(bool(policy), "Source policy is missing");
    require(bool(source_author.set(*policy, "double_sided", true)), "Cannot set source policy");
    require(bool(source_author.set(source_author.root(), "inputs",
                                   json::Document::literal<R"([
        {"path":"pigment","binding":{"asset_key":"","address":"repeat"},"color":"srgb","empty_value":[1.0,1.0,1.0,1.0]},
        {"path":"dormant","binding":{"asset_key":"","address":"repeat"},"color":"linear","empty_value":[1.0,1.0,1.0,1.0]}
    ])">()
                                       .root())),
            "Cannot set source inputs");
    const auto source_bytes = json::write(source_author.root(), {.pretty = true});
    write(project / source_file, {source_bytes.data(), source_bytes.size()});
    json::Builder parent_args{
        json::Document::literal<R"({"entries":[{"path":"gain","type":"float","enabled":true,"value":0.5}]})">()};
    json::Builder context;
    require(parent_args.set(parent_args.root(), "path", parent_file) && context.set(context.root(), "asset", source),
            "Cannot construct parent material");
    const auto created_parent = d.op("asset.new_material", parent_args.finish(), context.finish());
    const auto parent = *created_parent.root().string({"result", "asset"});
    json::Builder leaf_args{json::Document::literal<R"({"entries":[
        {"path":"pigment","type":"texture","enabled":true,"value":{"asset_key":"","address":"repeat"}},
        {"path":"dormant","type":"texture","enabled":false,"value":{"asset_key":"","address":"repeat"}},
        {"path":"gain","type":"texture","enabled":true,"value":{"asset_key":"builtin/not-a-texture","address":"repeat"}},
        {"path":"removed","type":"texture","enabled":true,"value":{"asset_key":"builtin/removed-input","address":"repeat"}}
    ]})">()};
    const auto entries = *leaf_args.root().at("entries");
    require(leaf_args.set(leaf_args.root(), "path", leaf_file) &&
                leaf_args.set(*entries.at(0)->at("value"), "asset_key", texture) &&
                leaf_args.set(*entries.at(1)->at("value"), "asset_key", dormant) &&
                context.set(context.root(), "asset", parent),
            "Cannot construct leaf material");
    const auto created_leaf = d.op("asset.new_material", leaf_args.finish(), context.finish());
    const auto leaf = *created_leaf.root().string({"result", "asset"});
    const auto unused = d.op("asset.new_material_source",
                             json::Document::literal<R"({"path":"Content/Materials/Unused.ms.material"})">());
    json::Builder material;
    require(bool(material.set(material.root(), "asset", *unused.root().path({"result", "asset"}))),
            "Invalid unused material");
    d.ok("material.inspect", material.finish()); // Cached Source text must not enter this scene's closure.
    require(bool(material.set(material.root(), "asset", leaf)), "Invalid leaf material");
    const auto opened = d.ok("material.inspect", material.finish());
    const auto handle = *opened.root().at("handle");
    require(opened.root().string({"source", "text"}) == source_text &&
                opened.root().boolean({"default_material"}) == false,
            "Custom MS did not load its exact saved text");
    // Keep a real texture root in the disabled entry before freezing. A root or
    // a cached image is not itself a strict execution dependency.
    json::Builder edit{json::Document::literal<R"({"path":"dormant"})">()};
    require(edit.set(edit.root(), "object", handle) && edit.set(edit.root(), "texture", dormant),
            "Invalid texture edit");
    d.ok("material.set_texture", edit.finish());
    edit.erase(edit.root(), "texture");
    require(bool(edit.set(edit.root(), "enabled", false)), "Invalid override edit");
    d.ok("material.override", edit.finish());
    d.ok("material.save", material.finish());
    const auto added = d.ok(
        "op.call", json::Document::literal<R"({"id":"scene.add","args":{"kind":"mesh","mesh":"builtin/cube"}})">());
    const auto entity = *added.root().at("active");
    json::Builder assign{json::Document::literal<R"({"component":null,"slot":"slot/0"})">()};
    require(bool(assign.set(assign.root(), "asset", leaf)), "Invalid assignment asset");
    const auto state = d.inspect();
    for (auto item : *state.root().at("entities")->array())
        if (*item.at("handle") == entity)
            require(bool(assign.set(assign.root(), "component", *item.at("components")->at(0)->at("handle"))),
                    "Invalid component handle");
    d.ok("material.assign", assign.finish());
    d.op("scene.save");
    const auto parent_author = read(project / parent_file);
    const auto leaf_author = read(project / leaf_file);
    const auto hash_json = [](json::Value value) {
        const auto text = json::write(value);
        return sha256(std::as_bytes(std::span(text.data(), text.size())));
    };
    const auto expected = [&](const std::array<std::uint8_t, 16>& pixels) {
        json::Builder result{json::Document::literal<R"({"version":2,"records":{}})">()};
        const auto records = *result.root().at("records");
        json::Builder record{json::Document::literal<
            R"({"inputs":{"pigment":{"key":"","image":null},"dormant":{"key":"","image":null}},"source":{}})">()};
        const auto source_record = *record.root().at("source");
        require(record.set(record.root(), "author", source_author.root()) &&
                    record.set(source_record, "path", source_path) && record.set(source_record, "text", source_text) &&
                    result.set(records, source, hash_json(record.root())),
                "Cannot construct source digest");
        json::Builder parent_record{json::Document::literal<R"({"inputs":{}})">()};
        require(parent_record.set(parent_record.root(), "author", parent_author.root()) &&
                    parent_record.set(parent_record.root(), "parent", source) &&
                    result.set(records, parent, hash_json(parent_record.root())),
                "Cannot construct parent digest");
        json::Builder leaf_record{
            json::Document::literal<R"({"inputs":{"pigment":{"image":{"width":2,"height":2}}}})">()};
        const auto pigment = *leaf_record.root().path({"inputs", "pigment"});
        require(leaf_record.set(leaf_record.root(), "author", leaf_author.root()) &&
                    leaf_record.set(leaf_record.root(), "parent", parent) && leaf_record.set(pigment, "key", texture) &&
                    leaf_record.set(*pigment.at("image"), "rgba_sha256", sha256(std::as_bytes(std::span(pixels)))) &&
                    result.set(records, leaf, hash_json(leaf_record.root())),
                "Cannot construct leaf digest");
        return result.finish();
    };
    const auto exported = [&](const std::array<std::uint8_t, 16>& pixels) {
        const auto id = unique_id();
        const auto started = d.call("repro.export", {}, id);
        require(started.root().string({"status"}) == "pending", "Material input repro export did not start");
        const auto done = d.wait(id);
        require(done.root().string({"status"}) == "ok", "Material input repro export failed");
        const std::filesystem::path bundle = utf8_to_utf16_validated(*done.root().string({"result", "directory"}));
        const auto manifest = read(bundle / "manifest.json");
        const auto expected_resources = expected(pixels);
        require(*manifest.root().at("material_resources") == expected_resources.root(),
                "Repro digests changed MS text/policy, sparse entries, parents or exact RGBA pixels");
        require(sha256_file(bundle / "Project" / native_source_path) == sha256(std::as_bytes(std::span(source_text))),
                "Repro reread the MS Source instead of freezing its loaded text");
        require(read(bundle / "Project" / leaf_file) == leaf_author &&
                    read(bundle / "Project" / parent_file) == parent_author &&
                    read(bundle / "Project" / source_file) == source_author.finish(),
                "Repro lost sparse author entries or a reachable parent");
        std::set<std::string> files;
        for (auto file : *manifest.root().at("files")->array())
            files.emplace(*file.string({"path"}));
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
        const auto engine = *manifest.root().path({"engine_assets", "records"});
        require(engine.size() == 1 && engine.at(0)->string({"key"}) == "builtin/cube",
                "Mismatched texture keys or an unused default material entered the engine closure");
        return bundle;
    };
    const auto first = exported(first_pixels);
    const auto parent_view = d.ok("material.inspect", context.finish());
    json::Builder parent_edit{
        json::Document::literal<R"({"entry":{"path":"gain","type":"float","enabled":true,"value":0.25}})">()};
    require(bool(parent_edit.set(parent_edit.root(), "object", *parent_view.root().at("handle"))),
            "Invalid parent handle");
    d.ok("material.set_entry", parent_edit.finish());
    const auto before_reimport = d.inspect();
    const auto identity_hash = sha256_file(project / (texture_file + ".asset.json"));
    require(bool(identity_hash), identity_hash ? "" : identity_hash.error());
    write(root / "Replacement.bmp", bitmap(next_pixels));
    const auto replacement_text = platform::utf16_to_utf8((root / "Replacement.bmp").native());
    require(bool(replacement_text), "Invalid UTF-16 texture replacement path");
    json::Builder reimport;
    json::Builder texture_context;
    require(reimport.set(reimport.root(), "path", *replacement_text) &&
                texture_context.set(texture_context.root(), "asset", texture),
            "Cannot construct reimport");
    d.op("asset.reimport", reimport.finish(), texture_context.finish());
    const auto after_reimport = d.inspect();
    for (const auto* field : {"undo", "redo", "dirty", "dirty_materials"})
        require(*before_reimport.root().at(field) == *after_reimport.root().at(field),
                "Texture Reimport entered parameter history");
    require(sha256_file(project / (texture_file + ".asset.json")) == *identity_hash,
            "Texture Reimport rewrote its identity sidecar");
    require(bool(context.set(context.root(), "region", "parameters")), "Invalid parameter context");
    d.op("edit.undo", {}, context.finish());
    context.erase(context.root(), "region");
    const auto restored_parent = d.ok("material.inspect", context.finish());
    require(restored_parent.root().boolean({"dirty"}) == false,
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
        json::Builder open;
        require(bool(open.set(open.root(), "directory", *bundle_text)), "Invalid bundle directory");
        d.ok("repro.open", open.finish());
        const auto view = d.ok("material.inspect", material.finish());
        require(view.root().string({"source", "text"}) == source_text &&
                    *view.root().at("entries") == *leaf_author.root().at("entries") &&
                    view.root().boolean({"default_material"}) == false,
                "Strict repro reopen lost loaded Source, dormant author intent or the active material");
        bool inherited_gain = false;
        for (auto value : *view.root().at("declarations")->array())
            inherited_gain |= value.string({"path"}) == "gain" && value.number({"value"}) == 0.5;
        require(inherited_gain, "A mismatched texture entry replaced its inherited numeric declaration");
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
    json::Builder modified{manifest};
    bool listed = false;
    for (auto file : *modified.root().at("files")->array())
        if (file.string({"path"}) == "Project/" + std::string{source_path}) {
            const auto hash = sha256_file(target);
            require(bool(hash), hash ? "" : hash.error());
            require(modified.set(file, "sha256", *hash) &&
                        modified.set(file, "bytes", std::filesystem::file_size(target)),
                    "Cannot update changed file metadata");
            listed = true;
        }
    require(listed, "Changed material input is absent from the manifest");
    const auto modified_bytes = json::write(modified.root(), {.pretty = true});
    write(changed / "manifest.json", {modified_bytes.data(), modified_bytes.size()});
    const auto before = d.inspect();
    const auto token = d.token;
    const auto changed_text = platform::utf16_to_utf8(changed.native());
    require(bool(changed_text), "Invalid UTF-16 changed repro path");
    json::Builder open;
    require(bool(open.set(open.root(), "directory", *changed_text)), "Invalid changed bundle directory");
    static_cast<void>(d.call("repro.open", open.finish()));
    const auto after = d.inspect();
    for (const auto* field : {"project", "scene_path", "entities", "selection", "camera", "view", "dirty",
                              "dirty_materials", "undo", "redo"})
        require(*before.root().at(field) == *after.root().at(field),
                "Rejected material repro changed " + std::string(field));
    require(d.token == token, "Rejected material repro changed the accepted state token");
    write(target, source_text);
    const auto manifest_bytes = json::write(manifest.root(), {.pretty = true});
    write(changed / "manifest.json", {manifest_bytes.data(), manifest_bytes.size()});
    reopened(changed);
    exported(next_pixels);
    json::Builder report{json::Document::literal<R"({"status":"passed","cases":[
        "MS-loaded-text","source-and-parent-closure","exact-RGBA-and-alpha","manual-reimport-then-parameter-undo",
        "immutable-earlier-export","disabled-retained-root","mismatched-and-removed-texture-paths",
        "unused-cached-MS-excluded","strict-reopen-and-reexport","content-digests-beyond-file-hashes","rejected-candidate-preserves-state"]})">()};
    require(report.set(report.root(), "first_rgba", sha256(std::as_bytes(std::span(first_pixels)))) &&
                report.set(report.root(), "reimported_rgba", sha256(std::as_bytes(std::span(next_pixels)))) &&
                report.set(report.root(), "material_resources", *manifest.root().at("material_resources")),
            "Cannot construct material report");
    const auto report_bytes = json::write(report.root(), {.pretty = true});
    write(root / "material-inputs.json", {report_bytes.data(), report_bytes.size()});
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
    d.ok("op.call", json::Document::literal<R"({"id":"scene.add","args":{"kind":"mesh"}})">());
    d.op("view.look_at", json::Document::literal<R"({"eye":{"x":8,"y":5,"z":-10},"target":{"x":1,"y":2,"z":3}})">());
    d.op("view.set", json::Document::literal<R"({"direct_candidates":16,"exposure_ev":1.25})">());
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
    const auto started = d.call("repro.export", {}, id);
    require(started.root().string({"status"}) == "pending", "Nested output fixture did not start");
    static_cast<void>(d.wait(id));
    const auto artifacts = platform::windows_file_path(
        directory / "artifacts" / utf8_to_utf16_validated(*d.descriptor.root().string({"instance_id"})));
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
            const auto identity = read(entry.path());
            const auto key = "project/" + std::string{*identity.root().string({"id"})};
            path->resize(path->size() - 11);
            material_files[key] = std::move(*path);
        }
    }
    for (const auto& mesh : {building, curtains}) {
        const auto identity = read(project / (mesh + ".asset.json"));
        for (const auto& [slot, key] : *identity.root().at("materials")->object_items())
            defaults.insert(material_files.at(std::string{*key.string()}));
    }
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
            const auto started = d.call("repro.export", {}, id);
            require(started.root().string({"status"}) == "pending", "Examples export did not start");
            const auto done = d.wait(id);
            require(done.root().string({"status"}) == "ok", "Examples export failed");
            const std::filesystem::path bundle = utf8_to_utf16_validated(*done.root().string({"result", "directory"}));
            const auto manifest = read(bundle / "manifest.json");
            std::set<std::string> copied;
            std::map<std::string, std::string> hashes;
            for (auto file : *manifest.root().at("input_files")->array()) {
                copied.emplace(*file.string({"path"}));
                hashes[std::string{*file.string({"path"})}] = *file.string({"sha256"});
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
        require(manifest.root().at("resources")->size() == 3,
                "An Examples repro did not check Sponza's two meshes and its sky map");
        const auto bundle_text = platform::utf16_to_utf8(bundle.native());
        require(bool(bundle_text), "Invalid UTF-16 Examples repro path");
        json::Builder open;
        require(bool(open.set(open.root(), "directory", *bundle_text)), "Invalid Examples bundle path");
        d.ok("repro.open", open.finish());
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
    json::Builder gltf{json::Document::literal<R"({
        "asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
        "meshes":[{"primitives":[
            {"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0},
            {"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":1}]}],
        "materials":[{"name":"paint","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},
            {"name":"trim","pbrMetallicRoughness":{"baseColorFactor":[0.2,0.7,0.4,1],"roughnessFactor":0.3}}],
        "textures":[{"source":0}],"images":[{"uri":"textures/color.png"}],"buffers":[{"uri":"scene.bin"}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},
            {"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6}],
        "accessors":[{"bufferView":0,"type":"VEC3","componentType":5126,"count":3},
            {"bufferView":1,"type":"VEC3","componentType":5126,"count":3},
            {"bufferView":2,"type":"VEC2","componentType":5126,"count":3},
            {"bufferView":3,"type":"SCALAR","componentType":5123,"count":3}]
    })">()};
    require(bool(gltf.set(*gltf.root().at("buffers")->at(0), "byteLength", bin.size())), "Invalid glTF buffer size");
    const auto gltf_bytes = json::write(gltf.root());
    write(source / "scene.gltf", {gltf_bytes.data(), gltf_bytes.size()});
    Process editor(executable,
                   L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --no-activate",
                   root / "editor.log");
    Driver d(discover_editor(editor, directory));
    d.settle();
    const auto source_text = platform::utf16_to_utf8((source / "scene.gltf").native());
    require(bool(source_text), "Invalid UTF-16 glTF source path");
    json::Builder import{json::Document::literal<R"({"kind":"mesh"})">()};
    require(bool(import.set(import.root(), "path", *source_text)), "Invalid glTF path");
    const auto imported = d.op("asset.import", import.finish());
    const auto mesh = *imported.root().string({"result", "asset"});
    json::Builder add{json::Document::literal<R"({"id":"scene.add","args":{"kind":"mesh"}})">()};
    require(bool(add.set(*add.root().at("args"), "mesh", mesh)), "Invalid mesh key");
    d.ok("op.call", add.finish());
    const std::string folder = "Content/Meshes/scene/";
    const std::string material = folder + "Materials/paint.material";
    const std::string trim = folder + "Materials/trim.material";
    const auto identity = read(project / (folder + "scene.gltf.asset.json"));
    const auto defaults = *identity.root().at("materials");
    const auto paint_identity = read(project / (material + ".asset.json"));
    const auto trim_identity = read(project / (trim + ".asset.json"));
    require(defaults.size() == 2 &&
                defaults.string({"paint"}) == "project/" + std::string{*paint_identity.root().string({"id"})} &&
                defaults.string({"trim"}) == "project/" + std::string{*trim_identity.root().string({"id"})},
            "The glTF import did not name its generated material as the slot default");
    std::set<std::string> inputs;
    const auto exported = [&] {
        const auto id = unique_id();
        const auto started = d.call("repro.export", {}, id);
        require(started.root().string({"status"}) == "pending", "glTF repro export did not start");
        const auto done = d.wait(id);
        require(done.root().string({"status"}) == "ok", "glTF repro export failed");
        const std::filesystem::path bundle = utf8_to_utf16_validated(*done.root().string({"result", "directory"}));
        std::set<std::string> copied;
        const auto manifest = read(bundle / "manifest.json");
        for (auto file : *manifest.root().at("input_files")->array())
            copied.emplace(*file.string({"path"}));
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
        json::Builder result{json::Kind::array};
        const auto state = d.inspect();
        json::Builder inspect{json::Document::literal<R"({"detail":"object"})">()};
        for (auto entity : *state.root().at("entities")->array())
            for (auto component : *entity.at("components")->array()) {
                require(bool(inspect.set(inspect.root(), "object", *component.at("handle"))),
                        "Invalid component handle");
                const auto detail = d.ok("editor.inspect", inspect.finish());
                result.append(result.root(), detail.root());
            }
        return result.finish();
    };
    json::Builder observations{json::Kind::array};
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
        auto response = d.call("repro.export", {}, id);
        if (response.root().string({"status"}) == "pending")
            response = d.wait(id);
        const bool refused = response.root().string({"status"}) == "error" &&
                             response.root().string({"error", "code"}) == "dependency_mismatch";
        const auto observation = observations.append_object(observations.root());
        const auto disk_identity = read(project / (folder + "scene.gltf.asset.json"));
        require(observations.set(observation, "case", std::string_view{label}) &&
                    observations.set(observation, "rejected", refused) &&
                    observations.set(observation, "response", response.root()) &&
                    observations.set(observation, "components", details.root()) &&
                    observations.set(observation, "disk_defaults", *disk_identity.root().at("materials")),
                "Cannot construct dependency observation");
        const auto bytes = json::write(observations.root(), {.pretty = true});
        write(root / "dependency-consistency.json", {bytes.data(), bytes.size()});
        const auto after = d.inspect();
        for (const auto* field :
             {"entities", "selection", "dirty", "dirty_materials", "undo", "redo", "camera", "project", "scene_path"})
            require(*before.root().at(field) == *after.root().at(field),
                    "Rejected export changed " + std::string(field));
        require(component_details() == details, "Export rebound the live document's defaults");
        for (const auto& [path, hash] : hashes)
            require(sha256_file(path) == hash, "Export changed a source file");
        return refused;
    };
    // The same dependency set is not the same relation: both material files remain reachable.
    json::Builder swapped{identity};
    const auto swapped_defaults = *swapped.root().at("materials");
    require(swapped.set(swapped_defaults, "paint", *defaults.at("trim")) &&
                swapped.set(swapped_defaults, "trim", *defaults.at("paint")),
            "Cannot swap material defaults");
    const auto swapped_bytes = json::write(swapped.root());
    write(project / (folder + "scene.gltf.asset.json"), {swapped_bytes.data(), swapped_bytes.size()});
    refresh();
    const bool relationship_rejected = rejected("same-set-swapped-defaults");
    const auto identity_bytes = json::write(identity.root());
    write(project / (folder + "scene.gltf.asset.json"), {identity_bytes.data(), identity_bytes.size()});
    refresh();
    // Two components retain different immutable versions of the same key after Refresh.
    // Restore disk to the first version: checking only the first component would miss the second.
    auto changed = bin;
    const float longer = 3;
    changed.replace(12, 4, reinterpret_cast<const char*>(&longer), 4);
    write(project / (folder + "scene.bin"), changed);
    refresh();
    const bool content_rejected = rejected("same-key-changed-content");
    d.ok("op.call", add.finish());
    write(project / (folder + "scene.bin"), bin);
    refresh();
    const bool versions_rejected = rejected("same-key-mixed-loaded-versions");
    d.op("edit.undo");
    exported();
    require(relationship_rejected && content_rejected && versions_rejected,
            "Repro accepted dependency drift; see dependency-consistency.json");
    d.ok("op.call", add.finish());
    const auto repeated_manifest = read(exported() / "manifest.json");
    require(repeated_manifest.root().at("resources")->size() == 1, "Equal loaded content must export once per key");
    d.op("edit.undo");
    // The export froze the unsaved mesh; undoing it leaves the source scene clean so the repro may replace it.
    d.op("edit.undo");
    const auto clean_state = d.inspect();
    require(clean_state.root().boolean({"dirty"}) == false,
            "Undoing the placed glTF mesh did not restore the saved scene");
    const auto bundle_text = platform::utf16_to_utf8(bundle.native());
    require(bool(bundle_text), "Invalid UTF-16 glTF repro path");
    json::Builder open;
    require(bool(open.set(open.root(), "directory", *bundle_text)), "Invalid glTF bundle directory");
    d.ok("repro.open", open.finish());
    exported();
    json::Builder reopened_defaults;
    const auto details = component_details();
    for (auto binding : *details.root().at(0)->path({"object", "mesh", "default_materials"})->array())
        require(bool(reopened_defaults.set(reopened_defaults.root(), *binding.string({"slot"}), *binding.at("source"))),
                "Invalid reopened binding");
    require(reopened_defaults.root() == defaults, "Reopened repro changed slot relationships");
    // Live material drafts are intentionally frozen, unlike external dependency drift.
    const auto draft_source = bundle / "Project" / trim;
    const auto saved_material = sha256_file(draft_source);
    require(bool(saved_material), saved_material ? "" : saved_material.error());
    const auto trim_key = *defaults.string({"trim"});
    json::Builder material_args;
    require(bool(material_args.set(material_args.root(), "asset", trim_key)), "Invalid trim material key");
    const auto trim_view = d.ok("material.inspect", material_args.finish());
    json::Builder edit{
        json::Document::literal<R"({"entry":{"path":"roughness","type":"float","enabled":true,"value":0.625}})">()};
    require(bool(edit.set(edit.root(), "object", *trim_view.root().at("handle"))), "Invalid trim material handle");
    d.ok("material.set_entry", edit.finish());
    const auto draft_bundle = exported();
    const auto dirty_material = d.ok("material.inspect", material_args.finish());
    require(material_entry(read(draft_bundle / "Project" / trim), "roughness").number({"value"}) == 0.625 &&
                sha256_file(draft_source) == *saved_material && dirty_material.root().boolean({"dirty"}) == true,
            "Repro lost or saved the source material draft");
    json::Builder context{material_args.root()};
    require(bool(context.set(context.root(), "region", "parameters")), "Invalid material context");
    d.op("edit.undo", {}, context.finish());
    const auto draft_text = platform::utf16_to_utf8(draft_bundle.native());
    require(bool(draft_text), "Invalid UTF-16 material draft repro path");
    require(bool(open.set(open.root(), "directory", *draft_text)), "Invalid draft bundle directory");
    d.ok("repro.open", open.finish());
    const auto reopened_material = d.ok("material.inspect", material_args.finish());
    bool frozen_roughness = false;
    for (auto p : *reopened_material.root().at("declarations")->array())
        frozen_roughness |= p.string({"path"}) == "roughness" && p.number({"value"}) == 0.625;
    require(frozen_roughness, "Repro did not load the frozen material draft");
    // A component keeps its material bindings while its mesh is missing. Only the scene names the
    // bound material then, and the repro still carries it.
    const auto project_text = platform::utf16_to_utf8(project.native());
    require(bool(project_text), "Invalid UTF-16 glTF project path");
    json::Builder project_args;
    require(bool(project_args.set(project_args.root(), "path", *project_text)), "Invalid glTF project path");
    d.op("project.open", project_args.finish());
    const auto bound =
        d.op("asset.new_material", json::Document::literal<R"({"path":"Content/Materials/Bound.material"})">(),
             json::Document::literal<R"({"asset":""})">());
    const auto added = d.ok("op.call", add.finish());
    const auto entity = *added.root().at("active");
    json::Builder assign{json::Document::literal<R"({"component":null,"slot":"paint"})">()};
    require(bool(assign.set(assign.root(), "asset", *bound.root().path({"result", "asset"}))),
            "Invalid bound material");
    const auto state = d.inspect();
    for (auto item : *state.root().at("entities")->array())
        if (*item.at("handle") == entity)
            require(bool(assign.set(assign.root(), "component", *item.at("components")->at(0)->at("handle"))),
                    "Invalid bound component");
    d.ok("material.assign", assign.finish());
    d.op("scene.save");
    // Missing defaults remain missing, rather than being rebound or preventing diagnostic export.
    const auto trim_document = read(project / trim);
    const auto trim_bytes = json::write(trim_document.root());
    std::filesystem::remove(project / trim);
    d.op("project.open", project_args.finish());
    const auto missing_id = unique_id();
    const auto missing_started = d.call("repro.export", {}, missing_id);
    require(missing_started.root().string({"status"}) == "pending", "Missing-default export did not start");
    const auto missing_done = d.wait(missing_id);
    require(missing_done.root().string({"status"}) == "ok", "Missing-default export failed");
    const std::filesystem::path missing_bundle =
        utf8_to_utf16_validated(*missing_done.root().string({"result", "directory"}));
    const auto missing_identity = read(missing_bundle / "Project" / (folder + "scene.gltf.asset.json"));
    require(!std::filesystem::exists(missing_bundle / "Project" / trim) &&
                *missing_identity.root().at("materials") == defaults,
            "Missing-default export synthesized a material or rebound the slots");
    const auto before_missing_open = d.inspect();
    require(bool(open.set(open.root(), "directory", *missing_done.root().path({"result", "directory"}))),
            "Invalid missing bundle directory");
    error(d.call("repro.open", open.finish()), "operation_failed");
    const auto after_missing_open = d.inspect();
    require(*after_missing_open.root().at("entities") == *before_missing_open.root().at("entities") &&
                *after_missing_open.root().at("project") == *before_missing_open.root().at("project"),
            "Strict missing-default reopen replaced the document");
    write(project / trim, {trim_bytes.data(), trim_bytes.size()});
    const auto mesh_path = platform::windows_file_path(project / (folder + "scene.gltf"));
    require(bool(mesh_path), mesh_path ? "" : platform::error_message(mesh_path.error()));
    std::filesystem::remove(*mesh_path);
    d.op("project.open", project_args.finish());
    const auto id = unique_id();
    const auto started = d.call("repro.export", {}, id);
    require(started.root().string({"status"}) == "pending", "Missing-mesh repro export did not start");
    const auto done = d.wait(id);
    require(done.root().string({"status"}) == "ok", "Missing-mesh repro export failed");
    std::set<std::string> copied;
    const auto missing_manifest = read(
        std::filesystem::path(utf8_to_utf16_validated(*done.root().string({"result", "directory"}))) / "manifest.json");
    for (auto file : *missing_manifest.root().at("input_files")->array())
        copied.emplace(*file.string({"path"}));
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
        bool has_call = false;
        bool has_list = false;
        bool has_cancel = false;
        for (auto op : *caps.root().at("operations")->array()) {
            has_call |= op.string({"op"}) == "op.call" && op.boolean({"requires_state_token"}) == true &&
                        op.string({"kind"}) == "direct" &&
                        *op.at("accepted_operator_kinds") == json::Document::literal<R"(["direct"])">().root() &&
                        *op.path({"args", "required"}) == json::Document::literal<R"(["id"])">().root();
            has_list |= op.string({"op"}) == "op.list" && op.string({"kind"}) == "query" &&
                        op.boolean({"requires_state_token"}) == false;
            has_cancel |= op.string({"op"}) == "job.cancel" && op.string({"kind"}) == "direct" &&
                          op.boolean({"requires_state_token"}) == false;
        }
        require(has_call, "Capabilities omitted the stateful operator call contract");
        require(has_list && has_cancel, "Capabilities confused query semantics with state-token requirements");
        verify_option_descriptions(caps);
        d.settle();
        if (!descriptions) {
            repro_engine_assets(d, root);
            repro_material_inputs(d, root / "MaterialInputs");
        }
        const auto project_text = platform::utf16_to_utf8(project.native());
        require(bool(project_text), "Invalid UTF-16 Control project path");
        json::Builder open_project{json::Document::literal<R"({"id":"project.open","args":{}})">()};
        require(bool(open_project.set(*open_project.root().at("args"), "path", *project_text)), "Invalid project path");
        const auto opened = d.ok("op.call", open_project.finish());
        require(opened.root().string({"status"}) == "finished" && opened.root().at("selection")->size() == 0,
                "Project open did not complete its replacement before returning the operator report");
        d.settle();
        verify_asset_directory(d, editor.window(), project);
        auto initial = d.inspect();
        unsigned stable{};
        for (unsigned i = 0; i < 200 && stable < 5; ++i) {
            const auto token = d.token;
            std::this_thread::sleep_for(30ms);
            initial = d.inspect();
            stable = token == d.token && initial.root().at("last_frame")->size() != 0 ? stable + 1 : 0;
        }
        require(stable == 5, "Initial surface mapping did not stabilize");
        require(initial.root().at("entities")->size() == 0 && initial.root().at("last_frame")->size() != 0,
                "Initial editor state is not ready");
        const auto empty_directory = d.ok("editor.inspect", json::Document::literal<R"({"detail":"objects"})">());
        require(empty_directory.root().at("objects")->size() == 0 &&
                    empty_directory.root().at("next_cursor")->is_null(),
                "Empty object directory has a continuation");
        const auto original_scene = read(project / "Content/Scenes/Main.scene");
        const auto original_scene_hash =
            descriptions ? sha256_file(project / "Content/Scenes/Main.scene") : std::string{};
        require(bool(original_scene_hash), original_scene_hash ? "" : original_scene_hash.error());
        json::Builder request{
            json::Document::literal<R"({"op":"op.call","args":{"id":"scene.add","args":{"kind":"mesh"}}})">()};
        require(request.set(request.root(), "state_token", d.token) &&
                    request.set(request.root(), "request_id", unique_id()),
                "Invalid mutation request");
        const auto first = d.client->call(request.finish());
        require(bool(first), first ? "" : first.error().what());
        require(first->root().string({"status"}) == "ok", "Initial mutation failed");
        const auto replay = d.client->call(request.finish());
        require(bool(replay), replay ? "" : replay.error().what());
        require(*first == *replay, "Duplicate mutation did not replay its response");
        require(bool(request.set(*request.root().at("args"), "kind", "entity")), "Cannot change replay request");
        const auto conflict = d.client->call(request.finish());
        require(bool(conflict), conflict ? "" : conflict.error().what());
        error(*conflict, "request_id_conflict");
        auto current = d.inspect();
        require(current.root().at("entities")->size() == 1 && current.root().u64({"undo"}) == 1,
                "Duplicate request repeated creation");
        const auto directory_cursor = verify_object_directory(d, current);
        if (descriptions) {
            const auto caps_bytes = json::write(caps.root(), {.pretty = true});
            write(root / "capabilities.json", {caps_bytes.data(), caps_bytes.size()});
            const auto properties_bytes = json::write(*current.root().at("entities")->at(0), {.pretty = true});
            write(root / "properties.json", {properties_bytes.data(), properties_bytes.size()});
            d.op("edit.undo");
            const auto restored = d.inspect();
            json::Builder inspect{json::Document::literal<R"({"detail":"objects"})">()};
            require(bool(inspect.set(inspect.root(), "cursor", directory_cursor.root())), "Invalid directory cursor");
            error(d.call("editor.inspect", inspect.finish()), "stale_cursor");
            inspect.erase(inspect.root(), "cursor");
            require(inspect.set(inspect.root(), "detail", "object") &&
                        inspect.set(inspect.root(), "object", *current.root().at("entities")->at(0)->at("handle")),
                    "Invalid stale object request");
            error(d.call("editor.inspect", inspect.finish()), "stale_object");
            require(restored.root().at("entities")->size() == 0 && restored.root().boolean({"dirty"}) == false &&
                        *restored.root().at("undo") == *initial.root().at("undo") &&
                        *restored.root().at("view") == *initial.root().at("view"),
                    "Description inspection or cleanup changed saved document state.");
            // Placeholders report the saved type they stand in for and the data they keep.
            const auto placeholder_bytes =
                json::write(json::Document::literal<R"({"version":10,"next_id":"3","roots":["1"],"materials":[],
                "entities":[{"id":"1","type":"world.Entity","name":"Holder","enabled":true,"properties":{},"children":[],"space":null,
                "components":[{"id":"2","type":"project.Gone","name":"","enabled":true,"properties":{"strength":3}}]}]})">()
                                .root());
            write(project / "Content/Scenes/Placeholder.scene", {placeholder_bytes.data(), placeholder_bytes.size()});
            d.op("scene.open", json::Document::literal<R"({"path":"Content/Scenes/Placeholder.scene"})">());
            const auto placed_result = d.ok("editor.inspect", json::Document::literal<R"({"detail":"objects"})">());
            const auto placed = *placed_result.root().at("objects");
            std::optional<json::Value> gone;
            for (auto entry : *placed.array())
                if (entry.string({"kind"}) == "component")
                    gone = entry;
            require(placed.size() == 2 && !*placed.at(0)->find("placeholder") && !*placed.at(0)->find("saved_type") &&
                        gone && gone->boolean({"placeholder"}) == true &&
                        gone->string({"saved_type"}) == "project.Gone" && gone->string({"type"}) == "world.Component",
                    "Object directory did not identify a placeholder and its saved type.");
            require(bool(inspect.set(inspect.root(), "object", *gone->at("handle"))), "Invalid placeholder handle");
            const auto detail_result = d.ok("editor.inspect", inspect.finish());
            const auto detail = *detail_result.root().at("object");
            require(detail.string({"saved_type"}) == "project.Gone" && detail.at("saved_data")->size() == 1 &&
                        detail.at("saved_data")->at(0)->string({"path"}) == "properties.strength" &&
                        detail.at("saved_data")->at(0)->string({"value"}) == "3" &&
                        detail.at("saved_data")->at(0)->string({"kind"}) == "unknown_field",
                    "Object inspection did not report a placeholder's saved data.");
            d.client.reset();
            editor.close();
            const auto remaining = discover(directory);
            require(bool(remaining), remaining ? "" : remaining.error().what());
            require(remaining->root().size() == 0, "Description editor descriptor remained live after close.");
            require(read(project / "Content/Scenes/Main.scene") == original_scene &&
                        sha256_file(project / "Content/Scenes/Main.scene") == *original_scene_hash,
                    "Description inspection changed the original saved scene.");
            std::cout << "Editor Control descriptions E2E: PASS | " << *root_text << '\n';
            return 0;
        }
        const std::string entity{*current.root().at("entities")->at(0)->string({"handle"})};
        const std::string component{*current.root().at("entities")->at(0)->at("components")->at(0)->string({"handle"})};
        const auto stale = d.token;
        d.op("scene.rename", json::Document::literal<R"({"name":"Control cube"})">(), selection(entity));
        json::Builder inspect{json::Document::literal<R"({"detail":"objects"})">()};
        require(bool(inspect.set(inspect.root(), "cursor", directory_cursor.root())), "Invalid directory cursor");
        error(d.call("editor.inspect", inspect.finish()), "stale_cursor");
        json::Builder stale_request{
            json::Document::literal<R"({"op":"op.call","args":{"id":"scene.rename","args":{"name":"Wrong"}}})">()};
        const auto selected = selection(entity);
        require(stale_request.set(stale_request.root(), "state_token", stale) &&
                    stale_request.set(*stale_request.root().at("args"), "context", selected.root()),
                "Invalid stale request");
        const auto stale_response = d.client->call(stale_request.finish());
        require(bool(stale_response), stale_response ? "" : stale_response.error().what());
        error(*stale_response, "stale_state");
        d.op("scene.transform_set", json::Document::literal<R"({"position":{"x":1,"y":2,"z":3}})">(), selected);
        d.op("scene.transform_set", json::Document::literal<R"({"position":{"x":4,"y":2,"z":3}})">(), selected);
        current = d.inspect();
        require(current.root().at("entities")->at(0)->path({"transform", "position"})->at(0)->number() == 4,
                "Transform operator did not write the document");
        d.op("edit.undo");
        current = d.inspect();
        require(current.root().at("entities")->at(0)->path({"transform", "position"})->at(0)->number() == 1,
                "Undo did not restore the transform");
        d.op("edit.redo");
        verify_operators(d, entity, editor.window());
        const auto added = d.op("scene.add_component", json::Document::literal<R"({"type":"world.Component"})">());
        current = d.inspect();
        require(current.root().at("entities")->at(0)->at("components")->size() == 2, "Component addition failed");
        json::Builder remove_context{selected};
        require(bool(remove_context.set(*remove_context.root().at("selection"), "object",
                                        *added.root().at("inspected_object"))),
                "Invalid component selection");
        d.op("scene.delete", json::Document::literal<R"({"target":"object"})">(), remove_context.finish());
        current = d.inspect();
        require(current.root().at("entities")->at(0)->at("components")->size() == 1 &&
                    current.root().at("entities")->at(0)->at("components")->at(0)->string({"handle"}) == component,
                "Component removal failed or replaced the existing mesh identity");
        json::Builder camera;
        require(bool(camera.set(camera.root(), "state", *current.root().at("camera"))), "Invalid camera state");
        d.op("view.look_at", json::Document::literal<R"({"eye":{"x":8,"y":5,"z":-10}})">());
        d.op("view.camera_set", camera.finish());
        d.op("view.set", json::Document::literal<R"({"gtao":false})">());
        current = d.inspect();
        require(current.root().boolean({"view", "gtao"}) == false && current.root().at("entities")->size() == 2,
                "A World setting did not create the Environment");
        // Saved but unpublished code must not silently enter the runnable repro.
        write(project / "Source/NeverPublished.source", "invalid saved source");
        write(project / "Source/Published.ui.source", "invalid saved UI");
        std::this_thread::sleep_for(750ms);
        d.inspect();
        current = d.inspect();
        const auto dirty_undo = *current.root().u64({"undo"});
        const std::string dirty_path{*current.root().string({"scene_path"})};
        const auto export_id = unique_id();
        const auto export_started = d.call("repro.export", {}, export_id);
        require(export_started.root().string({"status"}) == "pending", "Export did not start");
        auto exported = d.wait(export_id);
        require(exported.root().string({"status"}) == "ok", "Repro export failed");
        const auto repro_text = *exported.root().string({"result", "directory"});
        const std::filesystem::path repro = utf8_to_utf16_validated(repro_text);
        const auto manifest = read(repro / "manifest.json");
        const auto frozen = read(repro / "Project" / utf8_to_utf16_validated(*manifest.root().string({"scene"})));
        require(!std::filesystem::exists(repro / "Project/Source/NeverPublished.source") &&
                    std::filesystem::exists(repro / "unpublished/Source/NeverPublished.source") &&
                    std::filesystem::exists(repro / "unpublished/Source/Published.ui.source") &&
                    !std::filesystem::exists(repro / "unpublished/Source/Published.source"),
                "Repro mixed live and unpublished source inputs");
        require(sha256_file(repro / "Project/Source/Published.ui.source") ==
                    sha256(std::as_bytes(std::span(published_ui))),
                "Repro lost the published UI source");
        const auto publication_version = manifest.root().u64({"publication_version"});
        const auto ui_source_version = manifest.root().u64({"ui_source_version"});
        require(publication_version && *publication_version != 0 && ui_source_version && *ui_source_version != 0,
                "Repro did not identify the full publication");
        // The mesh, and the Environment its World settings created.
        require(frozen.root().at("entities")->size() == 2, "Export did not contain the dirty memory scene");
        require(read(project / "Content/Scenes/Main.scene") == original_scene,
                "Repro export overwrote the original scene");
        current = d.inspect();
        require(current.root().boolean({"dirty"}) == true && current.root().u64({"undo"}) == dirty_undo &&
                    current.root().string({"scene_path"}) == dirty_path,
                "Repro export changed authoring state");
        d.op("view.look_at", json::Document::literal<R"({"eye":{"x":9,"y":7,"z":-8},"target":{"x":4,"y":2,"z":3}})">());
        const json::Document base_view{*current.root().at("view")};
        const auto compare_id = unique_id();
        const auto prepare_id = unique_id();
        auto preparing = d.call("render.prepare", {}, prepare_id);
        require(preparing.root().string({"status"}) == "pending", "Render preparation did not start");
        std::cout << "Editor Control: preparing cold Scene Lit pipelines\n" << std::flush;
        auto prepared = d.wait(prepare_id, 605s);
        require(prepared.root().string({"status"}) == "ok", "Render preparation failed");
        require(prepared.root().string({"result", "phase"}) == "ready",
                "Preparation did not report an accepted ready frame");
        d.inspect();
        auto pending = d.call(
            "render.compare",
            json::Document::literal<
                R"({"cases":[{"name":"scene_lit","patch":{"shading":"scene_lit","shadows":false,"ddgi":false,"gtao":false}}],"sample_frames":[1,3]})">(),
            compare_id);
        require(pending.root().string({"status"}) == "pending", "Render comparison did not start");
        auto compared = d.wait(compare_id);
        require(compared.root().string({"status"}) == "ok", "Render comparison failed");
        const auto samples = *compared.root().path({"result", "samples"});
        require(samples.size() == 2, "Comparison sample count is incorrect");
        require(samples.at(0)->u64({"accepted_ordinal"}) == 1 && samples.at(1)->u64({"accepted_ordinal"}) == 3,
                "Accepted frame ordinals are incorrect");
        const auto sample = *samples.at(0);
        require(sample.string({"effective_view", "shading"}) == "scene_lit", "Scene Lit settings changed");
        const std::filesystem::path output = utf8_to_utf16_validated(*compared.root().string({"result", "directory"}));
        const auto width = *sample.at("extent")->at(0)->u64();
        const auto height = *sample.at("extent")->at(1)->u64();
        require(std::filesystem::file_size(output / utf8_to_utf16_validated(*sample.string({"depth"}))) ==
                    width * height * 4,
                "Depth readback size is incorrect");
        require(std::filesystem::file_size(output / utf8_to_utf16_validated(*sample.string({"color"}))) > 100,
                "PNG readback is missing");
        current = d.inspect();
        require(*current.root().at("view") == base_view.root() && current.root().u64({"undo"}) == dirty_undo,
                "Temporary comparison leaked into authoring state");
        verify_post_trace(d);
        auto cancelled_id = unique_id();
        json::Builder cancelled_request{
            json::Document::literal<R"({"op":"render.capture","args":{"sample_frames":[4096]}})">()};
        require(cancelled_request.set(cancelled_request.root(), "state_token", d.token) &&
                    cancelled_request.set(cancelled_request.root(), "request_id", cancelled_id),
                "Invalid cancelled capture request");
        const auto cancellation_started = d.client->call(cancelled_request.finish());
        require(bool(cancellation_started), cancellation_started ? "" : cancellation_started.error().what());
        require(cancellation_started->root().string({"status"}) == "pending", "Cancellation capture did not start");
        const auto unchanged = d.token;
        static_cast<void>(d.try_op("view.set", json::Document::literal<R"({"probe_radius":0,"probe_debug":true})">()));
        current = d.inspect();
        require(*current.root().at("view") == base_view.root(), "Rejected debug patch partially mutated the view");
        require(bool(camera.set(camera.root(), "state", *current.root().at("camera"))), "Invalid unchanged camera");
        d.op("view.camera_set", camera.finish());
        d.op("view.set");
        json::Builder job;
        require(bool(job.set(job.root(), "job_id", cancelled_id)), "Invalid job ID");
        const auto job_status = d.call("job.status", job.finish());
        require(d.token == unchanged && job_status.root().string({"status"}) == "pending",
                "Rejected or no-op requests cancelled unchanged rendering");
        error(d.call("job.cancel", job.finish()), "cancelled");
        error(d.call("job.cancel", job.finish()), "cancelled");
        for (unsigned i = 0; i < 260; ++i)
            d.op("view.set");
        const auto expired = d.client->call(cancelled_request.finish());
        require(bool(expired), expired ? "" : expired.error().what());
        error(*expired, "request_id_expired");
        error(d.call("job.status", job.finish()), "cancelled");
        current = d.inspect();
        require(!*current.root().find("active_job"), "Expired request started another job with the same identity");
        auto changed_id = unique_id();
        pending = d.call("render.capture", json::Document::literal<R"({"sample_frames":[4096]})">(), changed_id);
        require(pending.root().string({"status"}) == "pending", "State-change capture did not start");
        d.op("scene.rename", json::Document::literal<R"({"name":"Changed during capture"})">(), selection(entity));
        error(d.wait(changed_id), "state_changed");
        auto timed_id = unique_id();
        pending =
            d.call("render.capture", json::Document::literal<R"({"sample_frames":[4096],"timeout_ms":1})">(), timed_id);
        require(pending.root().string({"status"}) == "pending", "Timeout capture did not start");
        error(d.wait(timed_id), "timeout");
        const auto disconnected_id = unique_id();
        pending = d.call("render.capture", json::Document::literal<R"({"sample_frames":[4096]})">(), disconnected_id);
        require(pending.root().string({"status"}) == "pending", "Disconnect capture did not start");
        d.client.reset();
        std::this_thread::sleep_for(80ms);
        auto reconnected = Client::create(descriptor);
        require(bool(reconnected), reconnected ? "" : reconnected.error().what());
        d.client = std::make_unique<Client>(std::move(*reconnected));
        error(d.wait(disconnected_id), "cancelled");
        d.inspect();
        d.op("scene.save");
        const auto saved = read(project / "Content/Scenes/Main.scene");
        require(saved.root().at("entities")->size() == 2, "Explicit scene.save failed");
        const auto before_failed_open = d.inspect();
        // Hash-valid input can still fail compilation. Strict open must leave the
        // current document, object identities and project in place.
        write(repro / "Project/Source/Published.source", "invalid published source");
        json::Builder broken{manifest};
        for (auto file : *broken.root().at("files")->array())
            if (file.string({"path"}) == "Project/Source/Published.source") {
                const auto hash = sha256_file(repro / "Project/Source/Published.source");
                require(bool(hash), hash ? "" : hash.error());
                require(broken.set(file, "sha256", *hash) &&
                            broken.set(file, "bytes",
                                       std::filesystem::file_size(repro / "Project/Source/Published.source")),
                        "Cannot update broken source metadata");
            }
        const auto broken_bytes = json::write(broken.root());
        write(repro / "manifest.json", {broken_bytes.data(), broken_bytes.size()});
        json::Builder reopen;
        require(bool(reopen.set(reopen.root(), "directory", repro_text)), "Invalid repro directory");
        error(d.call("repro.open", reopen.finish()), "operation_failed");
        current = d.inspect();
        require(*current.root().at("project") == *before_failed_open.root().at("project") &&
                    *current.root().at("entities") == *before_failed_open.root().at("entities") &&
                    *current.root().at("selection") == *before_failed_open.root().at("selection"),
                "Failed repro open replaced the current document");
        write(repro / "Project/Source/Published.source", published_code);
        const auto manifest_bytes = json::write(manifest.root());
        write(repro / "manifest.json", {manifest_bytes.data(), manifest_bytes.size()});
        d.ok("repro.open", reopen.finish());
        current = d.inspect();
        require(current.root().at("entities")->size() == 2 &&
                    current.root().at("entities")->at(0)->string({"name"}) == "Control cube" &&
                    current.root().boolean({"dirty"}) == false,
                "Repro reopen failed");
        error(d.try_op("scene.rename", json::Document::literal<R"({"name":"Old handle"})">(), selection(entity)),
              "stale_object");
        d.op("scene.delete");
        current = d.inspect();
        const auto remaining = *current.root().at("entities");
        require(remaining.size() == 1 &&
                    remaining.at(0)->at("components")->at(0)->string({"type"}) == "world.EnvironmentComponent",
                "Delete command did not remove the selected entity");
        d.op("edit.undo");
        current = d.inspect();
        require(current.root().at("entities")->size() == 2, "Undo did not restore the entity");
        d.client.reset();
        Process cli(std::filesystem::path(argv[1]).parent_path() / "atelier_control.exe",
                    L"--list --directory " + quote(directory), root / "cli.json");
        require(WaitForSingleObject(cli.process, 10000) == WAIT_OBJECT_0, "CLI discovery did not finish");
        const auto cli_instances = read(root / "cli.json");
        require(cli_instances.root().at("instances")->size() == 1, "CLI did not discover the live editor");
        {
            json::Builder capture{json::Document::literal<R"({"op":"render.capture","args":{"sample_frames":[1]}})">()};
            require(bool(capture.set(capture.root(), "state_token", d.token)), "Invalid capture token");
            const auto bytes = json::write(capture.root());
            write(root / "capture-request.json", {bytes.data(), bytes.size()});
        }
        Process cli_capture(std::filesystem::path(argv[1]).parent_path() / "atelier_control.exe",
                            L"--directory " + quote(directory) + L" --instance " +
                                utf8_to_utf16_validated(*descriptor.root().string({"instance_id"})) + L" --request " +
                                quote(root / "capture-request.json"),
                            root / "cli-capture.json");
        require(WaitForSingleObject(cli_capture.process, 30000) == WAIT_OBJECT_0, "CLI capture did not finish");
        const auto cli_result = read(root / "cli-capture.json");
        require(cli_result.root().string({"status"}) == "ok" &&
                    cli_result.root().path({"result", "samples"})->size() == 1,
                "CLI did not await its capture job");
        // Every editor step of the run, Control captures included, kept the timing hierarchy.
        auto observer = Client::create(descriptor);
        require(bool(observer), observer ? "" : observer.error().what());
        d.client = std::make_unique<Client>(std::move(*observer));
        current = d.inspect();
        const auto cpu = *current.root().path({"performance", "cpu"});
        require(cpu.u64({"structure_errors"}) == 0 && cpu.at("first_structure_error")->is_null(),
                "An editor step recorded a frame timing phase under the wrong parent");
        editor.close();
        const auto live_instances = discover(directory);
        require(bool(live_instances), live_instances ? "" : live_instances.error().what());
        require(live_instances->root().size() == 0, "Closed editor descriptor remained live");
        std::cout << "Editor Control E2E: PASS | " << *root_text << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Editor Control E2E: FAIL: " << error.what() << '\n';
        return 1;
    }
}
