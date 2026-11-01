#include "control/control.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <cmath>
#include <array>

namespace {
using namespace atelier::control;

using namespace std::chrono_literals;

void require(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}

Json read(const std::filesystem::path& path) {
    std::ifstream stream(atelier::platform::windows_file_path(path), std::ios::binary);
    require(bool(stream), "Cannot read E2E artifact: " + utf8(path.wstring()));
    return Json::parse(stream);
}

void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(atelier::platform::windows_file_path(path.parent_path()));
    std::ofstream stream(atelier::platform::windows_file_path(path), std::ios::binary | std::ios::trunc);
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

    explicit Driver(Json instance)
        : client(std::make_unique<Client>(instance, 600s)), descriptor(std::move(instance)) {}

    Json call(std::string op, Json args = Json::object(), std::string id = {}) {
        Json request{{"op", std::move(op)}, {"args", std::move(args)}, {"state_token", token}};
        if (!id.empty())
            request["request_id"] = id;
        auto result = client->call(std::move(request));
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

Json discover_editor(const Process& editor, const std::filesystem::path& directory) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& instance : discover(directory))
            if (instance.at("pid").get<DWORD>() == editor.pid)
                return instance;
        require(WaitForSingleObject(editor.process, 0) == WAIT_TIMEOUT,
                "Editor stopped before publishing its endpoint");
        std::this_thread::sleep_for(30ms);
    }
    throw std::runtime_error("Editor did not publish its endpoint");
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

void no_project_scene_material(Driver& d) {
    const auto initial = d.inspect();
    require(initial.at("project") == "" && initial.at("entities").empty(),
            "Scene MI fixture requires an empty no-project document");
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}, {"mesh", "builtin/cube"}}}});
    const auto cube = d.inspect().at("entities").front();
    const auto component = cube.at("components").front().at("handle");
    std::cout << "No-project scene MI: create_scene on builtin cube\n" << std::flush;
    const auto instance = d.ok("material.create_scene", {{"component", component}, {"slot", "slot/0"}}).at("object");
    const auto inspect = [&] { return d.ok("material.inspect", {{"object", instance}}); };
    const auto created = inspect();
    require(created.at("scene") == true && created.at("parent") == "builtin/material/default-grid" &&
                created.at("entries").empty(),
            "A no-project scene MI did not inherit Grid without copying defaults");
    d.ok("material.set_entry",
         {{"object", instance},
          {"entry", {{"path", "cell_size"}, {"type", "float"}, {"enabled", true}, {"value", 2.5}}}});
    d.ok("material.override", {{"object", instance}, {"path", "cell_size"}, {"enabled", false}});
    const auto dormant = material_entry(inspect(), "cell_size");
    require(dormant.at("value") == 2.5 && dormant.at("enabled") == false,
            "No-project override disable lost its authored value");
    d.ok("material.discard", {{"object", instance}, {"path", "cell_size"}});
    require(inspect().at("entries").empty(), "No-project discard retained the local entry");
    d.op("edit.undo");
    require(material_entry(inspect(), "cell_size") == dormant, "Scene Undo did not restore the dormant MI entry");
    d.op("edit.redo");
    require(inspect().at("entries").empty(), "Scene Redo did not discard the MI entry again");
    d.ok("material.assign", {{"component", component}, {"slot", "slot/0"}, {"asset", ""}});
    require(d.ok("editor.inspect", {{"detail", "object"}, {"object", component}}).at("object").at("materials").empty(),
            "Use Mesh Default did not clear the no-project scene binding");
    error(d.try_op("asset.new_material", {{"path", "Content/Materials/NeedsProject.material"}}), "poll_failed");
    const auto undo = d.inspect().at("undo").get<std::size_t>() - initial.at("undo").get<std::size_t>();
    for (std::size_t i = 0; i < undo; ++i)
        d.op("edit.undo");
    const auto restored = d.inspect();
    require(restored.at("entities").empty() && restored.at("dirty") == initial.at("dirty") &&
                restored.at("undo") == initial.at("undo"),
            "No-project material commands did not undo back to the initial document");
    std::cout << "No-project scene MI commands: PASS\n" << std::flush;
}

void repro_engine_assets(Driver& d, const std::filesystem::path& root) {
    const auto exported = [&] {
        const auto id = unique_id();
        require(d.call("repro.export", Json::object(), id).at("status") == "pending",
                "Engine repro export did not start");
        const auto done = d.wait(id);
        require(done.at("status") == "ok", done.dump());
        return std::filesystem::path(wide(done.at("result").at("directory").get<std::string>()));
    };
    const auto empty = exported();
    require(read(empty / "manifest.json").at("engine_assets") == Json{{"version", 1}, {"records", Json::array()}},
            "An empty no-project document acquired unused installation assets");
    require(read(empty / "manifest.json").at("material_resources") == Json{{"version", 1}, {"records", Json::object()}},
            "An empty repro omitted its current, empty material digest set");
    for (const auto* key : {"builtin/plane", "builtin/cube", "builtin/sphere"})
        d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}, {"mesh", key}}}});
    const auto bundle = exported();
    const auto original = read(bundle / "manifest.json");
    const auto records = original.at("engine_assets").at("records");
    require(records.size() == 4 && records[0].at("key") == "builtin/cube" &&
                records[1].at("key") == "builtin/material/default-grid" && records[2].at("key") == "builtin/plane" &&
                records[3].at("key") == "builtin/sphere",
            "No-project export did not freeze its three meshes and their implicit DefaultWorldGrid MS");
    for (const auto& record : records) {
        const bool material = record.at("key") == "builtin/material/default-grid";
        require(record.at("kind") == Json(material ? "material" : "mesh") &&
                    record.at("digest_version") == Json(material ? 1 : 2),
                "Engine manifest did not identify material digest v1 and mesh digest v2");
    }
    const auto copy = root / "EngineRepro";
    std::filesystem::copy(bundle, copy, std::filesystem::copy_options::recursive);
    const auto scene_path = copy / "Project" / wide(original.at("scene").get<std::string>());
    const auto scene = read(scene_path);
    const auto publish = [&](Json manifest, const Json& value) {
        write(scene_path, value.dump(2));
        for (auto& file : manifest.at("files"))
            if (copy / wide(file.at("path").get<std::string>()) == scene_path) {
                file["sha256"] = sha256_file(scene_path);
                file["bytes"] = std::filesystem::file_size(scene_path);
            }
        write(copy / "manifest.json", manifest.dump(2));
    };
    error(d.call("repro.open", {{"directory", utf8(copy.wstring())}}),
          "unsaved_changes"); // guard, not dependency atomicity
    for (unsigned i = 0; i < 3; ++i)
        d.op("edit.undo");
    d.ok("repro.open", {{"directory", utf8(bundle.wstring())}});
    const auto entity = d.inspect().at("entities")[0].at("handle").get<std::string>();
    d.op("scene.rename", {{"name", "Saved with history"}}, selection(entity));
    d.op("scene.save");
    d.op("scene.select", Json::object(), selection(entity));
    d.op("view.look_at", {{"eye", {{"x", 7}, {"y", 4}, {"z", -9}}}, {"target", {{"x", 1}, {"z", 2}}}});
    d.settle();
    const auto rejected_open = [&](std::string_view code) {
        const auto before = d.inspect();
        const auto token = d.token;
        require(!before.at("dirty").get<bool>() && before.at("undo").get<unsigned>() > 0 &&
                    !before.at("selection").empty(),
                "Dependency atomicity fixture is not a saved, replaceable document with history and selection");
        error(d.call("repro.open", {{"directory", utf8(copy.wstring())}}), code);
        const auto after = d.inspect();
        for (const auto* field : {"project", "entities", "selection", "camera", "view", "undo", "redo", "dirty"})
            require(after.at(field) == before.at(field), "Rejected engine dependency changed " + std::string(field));
        require(d.token == token, "Rejected engine dependency changed the state token");
    };
    const auto rejected = [&](const Json& manifest, const Json& value, std::string_view code) {
        publish(manifest, value);
        rejected_open(code);
    };
    for (unsigned change = 0; change < 21; ++change) {
        auto manifest = original;
        auto& engine = manifest.at("engine_assets");
        auto& items = engine.at("records");
        std::string_view code = "dependency_mismatch";
        if (change == 0)
            items.erase(items.begin());
        if (change == 1)
            items = Json::array();
        if (change == 2) {
            items.push_back(items[0]);
            code = "invalid_request";
        }
        if (change == 3)
            items[0]["kind"] = "texture";
        if (change == 4)
            items[0]["semantic_version"] = 2;
        if (change == 5)
            items[0]["content_sha256"] = std::string(64, '0');
        if (change == 6) {
            manifest.erase("engine_assets");
            code = "invalid_request";
        }
        if (change == 7) {
            engine["version"] = 2;
            code = "invalid_request";
        }
        if (change == 8) {
            items[0]["digest_version"] = 3;
            code = "invalid_request";
        }
        if (change == 9) {
            items[0]["semantic_version"] = 1.5;
            code = "invalid_request";
        }
        if (change == 10)
            items[0]["key"] = "builtin/unknown";
        if (change == 11) {
            items[0]["content_sha256"] = "not-a-digest";
            code = "invalid_request";
        }
        if (change == 12) {
            items[1]["digest_version"] = 2;
            code = "invalid_request";
        }
        if (change == 13) {
            items[1]["kind"] = "mesh";
            items[1]["digest_version"] = 2;
        }
        if (change == 14)
            items[1]["content_sha256"] = std::string(64, '0');
        if (change == 15) {
            manifest.erase("material_resources");
            code = "invalid_request";
        }
        if (change == 16) {
            manifest["material_resources"]["version"] = 2;
            code = "invalid_request";
        }
        if (change == 17)
            manifest["material_resources"]["records"]["project/unused"] = std::string(64, '0');
        if (change == 18) {
            manifest["material_resources"]["records"] = Json::array();
            code = "invalid_request";
        }
        if (change == 19) {
            manifest["material_resources"]["records"]["project/unused"] = "not-a-digest";
            code = "invalid_request";
        }
        if (change == 20)
            items.erase(items.begin() + 1); // The implicit grid is still an actual dependency.
        rejected(manifest, scene, code);
    }
    auto unused = scene;
    unused["roots"] = Json::array();
    unused["entities"] = Json::array();
    rejected(original, unused, "dependency_mismatch");
    auto unresolved = scene;
    unresolved["entities"][0]["components"][0]["mesh"] = "builtin/unknown";
    rejected(original, unresolved, "dependency_mismatch");
    auto wrong_kind = scene;
    wrong_kind["materials"].push_back({{"id", "9000"}, {"source", "builtin/cube"}});
    rejected(original, wrong_kind, "dependency_mismatch");
    publish(original, scene);
    const auto valid_manifest = read(copy / "manifest.json").dump();
    auto duplicate = valid_manifest;
    constexpr std::string_view engine_field = "\"engine_assets\":{";
    const auto at = duplicate.find(engine_field);
    require(at != std::string::npos, "Engine dependency fixture lacks its record");
    // Preserve duplicate keys in raw text: a JSON DOM would silently replace them.
    duplicate.insert(at + engine_field.size(), "\"version\":1,");
    write(root / "duplicate-engine-version.json", duplicate);
    write(copy / "manifest.json", duplicate);
    rejected_open("invalid_request");
    require(std::filesystem::remove(copy / "manifest.json"), "Missing-manifest fixture did not remove the file");
    rejected_open("storage_error");
    write(copy / "manifest.json", valid_manifest);
    d.ok("repro.open", {{"directory", utf8(copy.wstring())}});
    const auto recovered = d.inspect();
    require(recovered.at("entities").size() == scene.at("entities").size() && recovered.at("undo") == 0 &&
                recovered.at("entities")[0].at("name") == scene.at("entities")[0].at("name"),
            "Restoring a valid manifest did not reopen the original document after parse/read rejection");
    // Disabled members remain dependencies. A name is not an asset protocol.
    auto disabled = scene;
    for (auto& item : disabled.at("entities")) {
        item["enabled"] = false;
        item["name"] = "builtin/not-an-asset-reference";
    }
    publish(original, disabled);
    d.ok("repro.open", {{"directory", utf8(copy.wstring())}});
    require(read(exported() / "manifest.json").at("engine_assets").at("records") == records,
            "Export dropped disabled members or treated an ordinary name as a resource slot");
    d.ok("repro.open", {{"directory", utf8(empty.wstring())}});
    require(d.inspect().at("entities").empty(), "Empty engine dependency set did not reopen");

    // A cached but unused material and an obsolete disk texture are not roots.
    // Only the referenced, published draft belongs to the frozen document.
    const auto project = root / "EngineDraftProject";
    project_fixture(project);
    d.op("project.open", {{"path", utf8(project.wstring())}});
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
    d.op("project.open", {{"path", utf8(project.wstring())}});
    d.ok("material.inspect", {{"asset", cached}});
    const auto loaded = d.ok("material.inspect", {{"asset", used}});
    require(material_issue(loaded, "builtin/unknown"),
            "Unknown-engine draft fixture did not retain its missing texture diagnostic");
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}});
    const auto component = d.inspect().at("entities")[0].at("components")[0].at("handle");
    d.ok("material.assign", {{"component", component}, {"slot", "slot/0"}, {"asset", used}});
    d.op("scene.save");
    error(d.call("repro.export"), "dependency_mismatch");
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
    d.ok("repro.open", {{"directory", utf8(draft.wstring())}});
    require(d.ok("material.inspect", {{"asset", used}}).at("diagnostics").empty(),
            "Published draft did not reopen strictly");
    write(root / "engine-assets.json",
          Json{{"status", "passed"},
               {"records", records},
               {"cases",
                {"no-project-empty-and-three-meshes-with-grid", "missing-and-extra-records",
                 "schema-kind-version-digest", "saved-history-selection-camera-preserved", "unknown-actual-resource",
                 "duplicate-json-field-invalid-request", "missing-manifest-storage-error", "valid-manifest-reopen",
                 "required-material-digests", "material-kind-and-digest-v1", "mesh-digest-v2", "disabled-members",
                 "ordinary-name", "unused-cache-and-superseded-disk-not-roots", "referenced-draft"}}}
              .dump(2));
}

// One operator registry serves menus, keys and Control; the context override lasts one call.
void verify_operators(Driver& d, const std::string& entity) {
    const auto listed = d.ok("op.list");
    const auto find = [&](std::string_view id) -> const Json& {
        for (const auto& info : listed.at("operators"))
            if (info.at("id").get<std::string>() == id)
                return info;
        throw std::runtime_error("op.list is missing " + std::string(id));
    };
    for (const auto& info : listed.at("operators")) {
        const auto& parameters = info.at("parameters");
        require(!info.at("title").get<std::string>().empty(), "Operator lacks a title: " + info.dump());
        if (info.at("id") == "scene.set_properties")
            require(parameters == Json{{"dynamic", "context.object"}},
                    "Property patch schema must resolve from the inspected object's actual type: " + info.dump());
        else
            require(parameters.at("type") == "object" && parameters.at("additionalProperties") == false,
                    "Operator lacks a closed parameter schema: " + info.dump());
    }
    require(find("edit.undo").at("flags") == Json::array({"history"}) &&
                find("edit.redo").at("flags") == Json::array({"history"}) &&
                find("view.frame_all").at("flags").empty() && find("scene.select").at("flags").empty() &&
                find("layout.reset").at("flags").empty() &&
                find("scene.transform_remove").at("flags") == Json::array({"undo"}) &&
                find("layout.close_tab").at("flags").empty(),
            "Operator flags do not match their history contracts");
    const auto& regions = listed.at("context").at("properties").at("region").at("enum");
    require(std::ranges::find(regions, Json("viewport")) != regions.end() &&
                std::ranges::find(regions, Json("code")) != regions.end(),
            "Context schema lacks regions");

    d.op("scene.select", Json::object(), selection(entity));
    d.op("scene.select", Json::object(), {{"selection", {{"entities", Json::array()}}}});
    auto before = d.inspect();
    require(before.at("selection").empty(), "scene.select did not clear the selection");
    auto unavailable = d.try_op("view.frame_selected", Json::object());
    error(unavailable, "poll_failed");
    require(unavailable.at("error").at("message").get<std::string>().find("Select an Entity") != std::string::npos,
            "Poll failure did not give its reason: " + unavailable.dump());
    d.op("view.frame_selected", Json::object(), selection(entity));
    auto after = d.inspect();
    require(after.at("camera") != before.at("camera") && after.at("selection").empty() &&
                after.at("undo") == before.at("undo"),
            "A context selection must frame its Entity without changing the document selection or history");

    const auto undo = before.at("undo").get<std::size_t>();
    const auto redo = before.at("redo").get<std::size_t>();
    require(undo > 0, "The operator check needs scene history");
    d.op("edit.undo");
    after = d.inspect();
    require(after.at("undo") == undo - 1 && after.at("redo") == redo + 1, "edit.undo did not move the cursor by one");
    d.op("edit.redo");
    after = d.inspect();
    require(after.at("undo") == undo && after.at("redo") == redo, "edit.redo did not restore the cursor");
    // A plain Entity has no spatial capabilities, so its Transform can come and go.
    const auto plain =
        d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "entity"}}}}).at("active").get<std::string>();
    error(d.try_op("scene.transform_remove", Json::object(), selection(plain)), "poll_failed");
    d.op("scene.transform_add", Json::object(), selection(plain));
    error(d.try_op("scene.transform_add", Json::object(), selection(plain)), "poll_failed");
    d.op("scene.select", Json::object(), {{"selection", {{"entities", Json::array()}}}});
    d.op("scene.transform_remove", Json::object(), selection(plain));
    after = d.inspect();
    require(after.at("undo") == undo + 3 && after.at("redo") == 0 && after.at("selection").empty(),
            "scene.transform_add/remove did not record one entry each for their context Entity");
    d.op("edit.undo");
    error(d.try_op("scene.transform_add", Json::object(), selection(plain)), "poll_failed");
    d.op("edit.undo");
    d.op("edit.undo");
    error(d.try_op("edit.undo", Json::object(), {{"region", "code"}}), "poll_failed");
    require(d.inspect().at("undo") == undo, "A Code-region undo without a source file must not touch the scene");

    error(d.try_op("view.unknown", Json::object()), "unknown_operator");
    error(d.try_op("view.frame_all", {{"zoom", 2}}), "invalid_request");
    error(d.try_op("scene.select", Json::object(),
                   {{"selection", {{"entities", Json::array({entity})}, {"active", "stale/1"}}}}),
          "stale_object");
    error(d.try_op("view.frame_all", Json::object(), {{"region", "nowhere"}}), "invalid_request");
    error(d.try_op("scene.select", Json::object(), {{"selection", {{"entities", Json::array({entity, entity})}}}}),
          "invalid_request");
    // Arguments decode strictly into the operator's parameter record; a rejected call changes nothing.
    require(find("scene.add").at("parameters").at("properties").at("kind").at("enum") ==
                    Json::array({"entity", "mesh", "point", "rectangle", "directional", "camera"}) &&
                find("scene.add").at("flags") == Json::array({"undo"}) &&
                find("tool.settings").at("parameters").value("seeded", false),
            "op.list does not publish the operator parameter records");
    const auto entries = d.inspect().at("undo").get<std::size_t>();
    error(d.try_op("scene.add", {{"kind", "sky"}}), "invalid_request");
    require(d.inspect().at("undo") == entries, "A rejected scene.add changed the scene");
    const auto light = d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "point"}}}}).at("active");
    after = d.inspect();
    require(after.at("undo") == entries + 1 && after.at("selection") == Json::array({light}) &&
                std::ranges::any_of(after.at("entities"), [&](const Json& item) { return item.at("handle") == light; }),
            "scene.add did not add and select one Entity in one history entry");
    d.op("edit.undo");
    require(d.inspect().at("undo") == entries, "Undo did not remove the added Entity");
    d.op("scene.select", Json::object(), selection(entity));
    require(d.inspect().at("selection") == Json::array({entity}), "scene.select did not restore the selection");
}

void verify_asset_directory(Driver& d, HWND window, const std::filesystem::path& project, const Json& capabilities) {
    namespace fs = std::filesystem;
    const auto maximum = capabilities.at("limits").at("inspect_page_items").get<unsigned>();
    const auto budget = capabilities.at("limits").at("inspect_page_bytes").get<std::size_t>();
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
    const auto full = d.inspect();
    const auto token = d.token;
    const auto page = [&](Json args = Json::object()) {
        args["detail"] = "assets";
        return d.ok("editor.inspect", std::move(args));
    };
    const auto collect = [&](unsigned limit, const Json& expected) {
        Json actual = Json::array();
        Json cursor = nullptr;
        unsigned pages{};
        do {
            Json args{{"limit", limit}};
            if (!cursor.is_null())
                args["cursor"] = cursor;
            const auto result = page(args);
            const auto& entries = result.at("assets");
            std::size_t bytes{};
            for (const auto& entry : entries) {
                bytes += entry.dump().size() + 1;
                actual.push_back(entry);
            }
            require(entries.size() <= limit && bytes <= budget && actual.size() <= expected.size(),
                    "Assets page exceeded its item/byte bounds");
            const auto next = result.at("next_cursor");
            require(next.is_null() || (!entries.empty() && next != cursor), "Assets pagination did not advance");
            cursor = next;
            require(++pages <= expected.size() + 1, "Assets pagination did not terminate");
        } while (!cursor.is_null());
        require(actual == expected, "Assets pages diverged from full.assets order, shape or tail");
        return pages;
    };
    collect(1, full.at("assets"));
    collect(maximum, full.at("assets"));
    const auto first = page({{"limit", 1}});
    const auto cursor = first.at("next_cursor");
    require(!cursor.is_null() && first.at("detail") == "assets", "Small project did not return an assets continuation");
    const auto continuation = page({{"limit", 1}, {"cursor", cursor}});
    for (const auto& limit : Json::array({0, -1, 257, 1.5, "2", nullptr}))
        error(d.call("editor.inspect", {{"detail", "assets"}, {"limit", limit}}), "invalid_request");
    for (const auto* field : {"entity_name", "type", "object"})
        error(d.call("editor.inspect", {{"detail", "assets"}, {field, "not-an-assets-filter"}}), "invalid_request");
    for (const auto& malformed : Json::array({nullptr, Json::object(), "not-a-cursor"}))
        error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", malformed}}), "invalid_request");
    auto foreign = cursor;
    foreign["domain"] = "objects";
    error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", foreign}}), "invalid_request");
    foreign = cursor;
    foreign["index"] = full.at("assets").size() + 1;
    error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", foreign}}), "invalid_request");
    foreign = cursor;
    foreign["index"] = -1;
    error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", foreign}}), "invalid_request");
    for (const auto* field : {"instance", "document", "project_revision"}) {
        foreign = cursor;
        foreign[field] = field == std::string_view("instance") ? Json("other-instance")
                                                               : Json(cursor.at(field).get<std::uint64_t>() + 1);
        error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", foreign}}), "stale_cursor");
    }
    error(d.call("editor.inspect", {{"detail", "objects"}, {"cursor", cursor}}), "invalid_request");
    for (const auto* detail : {"full", "summary"})
        error(d.call("editor.inspect", {{"detail", detail}, {"limit", 1}}), "invalid_request");
    require(d.token == token && d.inspect().at("entities") == full.at("entities"),
            "Asset queries or rejected parameters mutated the document");
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}});
    const auto created = d.inspect();
    const auto object_cursor = d.ok("editor.inspect", {{"detail", "objects"}, {"limit", 1}}).at("next_cursor");
    error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", object_cursor}}), "invalid_request");
    d.op("scene.rename", {{"name", "Assets cursor edit"}}, selection(created.at("entities")[0].at("handle")));
    d.op("scene.select", Json::object(), selection(created.at("entities")[0].at("handle").get<std::string>()));
    d.op("view.look_at", {{"eye", {{"x", 3}, {"y", 4}, {"z", -6}}}});
    require(page({{"limit", 1}, {"cursor", cursor}}) == continuation,
            "Ordinary scene/selection/camera edits invalidated assets cursor");
    d.op("edit.undo");
    d.op("edit.undo");
    d.op("view.camera_set", {{"state", full.at("camera")}});
    refresh();
    require(page({{"limit", 1}, {"cursor", cursor}}) == continuation, "Unchanged refresh invalidated assets cursor");
    // Inspection must not scan the disk, decode a new asset, or publish a failed refresh.
    write(project / "Content/bad.bmp", "not decoded by catalog");
    write(project / "Content/bad.bmp.asset.json", "{broken");
    require(page({{"limit", 1}, {"cursor", cursor}}) == continuation, "Inspection refreshed the catalog");
    refresh();
    require(page({{"limit", 1}, {"cursor", cursor}}) == continuation,
            "Failed refresh replaced the published directory");
    fs::remove(project / "Content/bad.bmp.asset.json");
    fs::remove(project / "Content/bad.bmp");
    write(project / "Content/new.txt", "catalog change");
    refresh();
    error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", cursor}}), "stale_cursor");
    const auto changed_cursor = page({{"limit", 1}}).at("next_cursor");
    d.op("scene.open", {{"path", "Content/Scenes/Main.scene"}});
    error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", changed_cursor}}), "stale_cursor");
    const auto scene_cursor = page({{"limit", 1}}).at("next_cursor");
    d.op("project.open", {{"path", utf8(project.wstring())}});
    error(d.call("editor.inspect", {{"detail", "assets"}, {"cursor", scene_cursor}}), "stale_cursor");
    // A real catalog beyond the old limit; page through its final item, not just its first page.
    const auto large = project / "Content/Catalog";
    fs::create_directory(large);
    for (unsigned i = 0; i < 10005; ++i)
        fs::create_directory(large / ("Entry" + std::to_string(i)));
    refresh();
    const auto expanded = d.inspect();
    const auto expanded_token = d.token;
    require(expanded.at("assets").size() > 10000 && page().at("assets").size() == 64,
            "Large asset catalog/default page is incomplete");
    const auto pages = collect(maximum, expanded.at("assets"));
    require(d.token == expanded_token && !expanded.at("dirty").get<bool>(), "Large directory inspection mutated state");
    write(project.parent_path() / "assets-pagination.json", Json{{"status", "passed"},
                                                                 {"count", expanded.at("assets").size()},
                                                                 {"pages", pages},
                                                                 {"first", expanded.at("assets").front()},
                                                                 {"last", expanded.at("assets").back()},
                                                                 {"sample_page", first}}
                                                                .dump(2));
    fs::remove_all(large);
    fs::remove(project / "Content/new.txt");
    refresh();
    // Real long paths exceed the full response limit and hit the byte budget before 256 items.
    auto expected = full.at("assets");
    fs::path deep = "Content/Wide";
    const auto add_directory = [&](const fs::path& path) {
        fs::create_directories(atelier::platform::windows_file_path(project / path));
        const auto key = utf8(path.generic_wstring());
        expected.push_back({{"key", key}, {"path", key}, {"kind", 0}});
    };
    add_directory(deep);
    for (unsigned i = 0; i < 30; ++i) {
        deep /= std::string(160, 'a');
        add_directory(deep);
    }
    for (unsigned i = 0; i < 450; ++i)
        add_directory(deep / ("Leaf" + std::to_string(i)));
    std::stable_sort(expected.begin(), expected.end(), [](const Json& a, const Json& b) {
        if (!a.contains("path") || !b.contains("path"))
            return !a.contains("path") && b.contains("path");
        return fs::path(wide(a.at("path").get<std::string>())) < fs::path(wide(b.at("path").get<std::string>()));
    });
    refresh();
    error(d.call("editor.inspect"), "result_too_large");
    const auto byte_pages = collect(maximum, expected);
    require(byte_pages > (expected.size() + maximum - 1) / maximum,
            "Long-path pagination did not exercise the byte budget");
    fs::remove_all(atelier::platform::windows_file_path(project / "Content/Wide"));
    refresh();
    require(d.inspect().at("assets") == full.at("assets") &&
                sha256_file(project / "Content/Scenes/Main.scene") == saved,
            "Asset pagination or fixture cleanup changed the original saved document/catalog");
    std::cout << "Assets pagination E2E: PASS (" << expanded.at("assets").size() << " entries, " << pages << " pages; "
              << byte_pages << " byte-limited pages; full equality/oversize, cursor domains/invalidation, read-only)\n";
}

void verify_option_descriptions(const Json& capabilities) {
    const auto& options = capabilities.at("display_options");
    for (const auto& [name, option] : options.items()) {
        static_cast<void>(name);
        for (const char* field : {"label", "category", "description"})
            require(option.at(field).is_string() && !option.at(field).get<std::string>().empty(),
                    "Control option metadata is missing");
    }
    const auto& count = options.at("probe_volume_0_count_x");
    const auto description = count.at("description").get<std::string>();
    require(description.find('\n') != std::string::npos &&
                std::any_of(description.begin(), description.end(), [](unsigned char c) { return c >= 0x80; }),
            "Control metadata lost Unicode or decoded line breaks");
    require(count.at("type") == "integer" && count.at("minimum") == 2 && count.at("maximum") == 32 &&
                count.at("scope") == "scene" && options.at("probe_visualization").at("scope") == "view",
            "Metadata changed option types, bounds or ownership");
    for (unsigned slot = 0; slot < 4; ++slot)
        for (const char* axis : {"x", "y", "z"})
            require(options.at("probe_volume_" + std::to_string(slot) + "_count_" + axis).at("description") ==
                        description,
                    "Volume slots or vector lanes diverged from their shared description");
}

void verify_property_descriptions(const Json& object) {
    require(object.at("type_description").is_string(), "Object type description is missing");
    std::string position;
    unsigned lanes{};
    for (const auto& field : object.at("properties")) {
        require(field.at("description").is_string(), "Reflected property description is missing");
        if (field.at("name").get<std::string>().starts_with("position.")) {
            const auto description = field.at("description").get<std::string>();
            require(!description.empty(), "Position aggregate description did not reach Control");
            if (lanes++)
                require(description == position, "Vector lanes did not inherit their aggregate description");
            else
                position = description;
        }
    }
    require(lanes == 3, "Position descriptor did not expose its three scalar lanes");
}

Json verify_object_directory(Driver& d, const Json& full, const Json& capabilities) {
    const auto limit = capabilities.at("limits").at("inspect_page_items").get<unsigned>();
    const auto bytes = capabilities.at("limits").at("inspect_page_bytes").get<std::size_t>();
    require(limit == 256 && bytes < maximum_message,
            "Object directory limits are not bounded below the transport limit");
    Json expected = Json::array();
    for (const auto& entity : full.at("entities")) {
        expected.push_back({{"handle", entity.at("handle")},
                            {"name", entity.at("name")},
                            {"type", entity.at("type")},
                            {"kind", "entity"},
                            {"entity", entity.at("handle")},
                            {"parent", entity.at("parent")}});
        for (const auto& component : entity.at("components"))
            expected.push_back({{"handle", component.at("handle")},
                                {"name", component.at("name")},
                                {"type", component.at("type")},
                                {"kind", "component"},
                                {"entity", entity.at("handle")}});
    }
    const auto state_token = d.token;
    auto page = d.ok("editor.inspect", {{"detail", "objects"}, {"limit", 1}});
    const auto first_cursor = page.at("next_cursor");
    Json actual = Json::array();
    for (std::size_t count = 0;; ++count) {
        require(count <= expected.size() && page.at("objects").size() <= 1 && page.dump().size() < bytes,
                "Object directory exceeded its bounds or did not advance");
        for (const auto& entry : page.at("objects"))
            actual.push_back(entry);
        if (page.at("next_cursor").is_null())
            break;
        page = d.ok("editor.inspect", {{"detail", "objects"}, {"limit", 1}, {"cursor", page.at("next_cursor")}});
    }
    require(actual == expected && !first_cursor.is_null(), "Object directory omitted, duplicated or expanded objects");
    const auto& entity = full.at("entities")[0];
    const auto& component = entity.at("components")[0];
    page = d.ok("editor.inspect", {{"detail", "objects"}, {"entity_name", entity.at("name")}, {"limit", 1}});
    const auto rest = d.ok("editor.inspect", {{"detail", "objects"}, {"cursor", page.at("next_cursor")}});
    require(page.at("objects")[0].at("handle") == entity.at("handle") &&
                rest.at("objects")[0].at("handle") == component.at("handle") && rest.at("next_cursor").is_null(),
            "Object cursor did not retain its exact entity-name filter");
    page = d.ok("editor.inspect", {{"detail", "objects"}, {"type", component.at("type")}});
    require(page.at("objects").size() == 1 && page.at("objects")[0].at("handle") == component.at("handle"),
            "Object type filter returned another object");
    require(d.ok("editor.inspect", {{"detail", "objects"}, {"entity_name", "No such entity"}}).at("objects").empty(),
            "Missing entity name returned unrelated objects");
    require(d.ok("editor.inspect", {{"detail", "object"}, {"object", component.at("handle")}}).at("object") ==
                component,
            "Single-object inspection diverged from the shared reflected object output");
    error(d.call("editor.inspect", {{"detail", "objects"}, {"limit", 0}}), "invalid_request");
    error(d.call("editor.inspect", {{"detail", "objects"}, {"limit", limit + 1}}), "invalid_request");
    error(d.call("editor.inspect", {{"detail", "summary"}, {"limit", 1}}), "invalid_request");
    error(d.call("editor.inspect", {{"detail", "object"}}), "invalid_request");
    error(d.call("editor.inspect", {{"detail", "object"}, {"object", "not-a-handle"}}), "stale_object");
    error(
        d.call("editor.inspect", {{"detail", "objects"}, {"cursor", first_cursor}, {"entity_name", "Changed filter"}}),
        "invalid_request");
    auto foreign = first_cursor;
    foreign["instance"] = "another-instance";
    error(d.call("editor.inspect", {{"detail", "objects"}, {"cursor", foreign}}), "stale_cursor");
    foreign = first_cursor;
    foreign["document"] = foreign.at("document").get<std::uint64_t>() + 1;
    error(d.call("editor.inspect", {{"detail", "objects"}, {"cursor", foreign}}), "stale_cursor");
    foreign = first_cursor;
    foreign["entity"] = full.at("entities").size() + 1;
    error(d.call("editor.inspect", {{"detail", "objects"}, {"cursor", foreign}}), "invalid_request");
    require(d.token == state_token && d.inspect().at("entities") == full.at("entities"),
            "Read-only object discovery changed the document or its state token");
    return first_cursor;
}

// A repro opens only in the current view schema and scene format, and its frozen view
// must agree with the scene's Environment. Every rejection keeps the current document.
void repro_lighting_agreement(Driver& d, const std::filesystem::path& repro, const std::filesystem::path& root) {
    const auto original = read(repro / "manifest.json");
    const auto original_scene = read(repro / "Project" / wide(original.at("scene").get<std::string>()));
    require(original.at("view_schema") == 7 && original_scene.at("version") == 10 && !original_scene.contains("world"),
            "Current repro export did not identify view schema 7 and Scene v10");
    const auto copy = root / "LightingRepro";
    std::filesystem::copy(atelier::platform::windows_file_path(repro), atelier::platform::windows_file_path(copy),
                          std::filesystem::copy_options::recursive);
    const auto scene_path = copy / "Project" / wide(original.at("scene").get<std::string>());
    const auto publish = [&](Json manifest, const Json& scene) {
        write(scene_path, scene.dump(2));
        bool listed = false;
        for (auto& file : manifest.at("files"))
            if (copy / wide(file.at("path").get<std::string>()) == scene_path) {
                file["sha256"] = sha256_file(scene_path);
                file["bytes"] = std::filesystem::file_size(atelier::platform::windows_file_path(scene_path));
                listed = true;
            }
        require(listed, "Repro scene was absent from the dependency manifest");
        write(copy / "manifest.json", manifest.dump(2));
    };
    const auto rejected = [&](const Json& manifest, const Json& scene, std::string_view code = "invalid_request") {
        publish(manifest, scene);
        const auto before = d.inspect();
        const auto token = d.token;
        error(d.call("repro.open", {{"directory", utf8(copy.wstring())}}), code);
        const auto after = d.inspect();
        for (const auto* field : {"project", "scene_path", "entities", "selection", "inspected_object", "camera",
                                  "view", "dirty", "undo", "redo"})
            require(after.at(field) == before.at(field), std::string("Rejected lighting repro changed ") + field);
        require(d.token == token, "Rejected lighting repro changed the accepted editor state token");
    };
    auto unmarked = original;
    unmarked.erase("view_schema");
    rejected(unmarked, original_scene);
    for (unsigned schema : {6u, 8u}) {
        auto other = original;
        other["view_schema"] = schema;
        rejected(other, original_scene);
    }
    for (unsigned version : {8u, 9u}) {
        auto old_scene = original_scene;
        old_scene["version"] = version;
        rejected(original, old_scene, "operation_failed");
    }
    for (const auto& [name, value] : Json{
             {"ssil", false},
             {"shadow_bias", .002},
             {"shadow_light_budget", 16},
             {"shadow_samples", 64},
             {"direct_reference", true},
             {"direct_clustered", false},
             {"direct_shadow_cache",
              true}}.items()) {
        auto retired = original;
        retired["view"][name] = value;
        rejected(retired, original_scene);
    }
    auto drifted = original;
    drifted["view"]["exposure_ev"] = original.at("view").at("exposure_ev").get<double>() + .25;
    rejected(drifted, original_scene, "dependency_mismatch");
    auto sky_drifted = original;
    sky_drifted["view"]["sky_enabled"] = !original.at("view").at("sky_enabled").get<bool>();
    rejected(sky_drifted, original_scene, "dependency_mismatch");
    // Selection and inspection resolve against the loaded candidate's World members before it replaces the document.
    auto beyond = original;
    beyond.erase("inspection");
    beyond["selection_indices"] = Json::array({original_scene.at("entities").size()});
    rejected(beyond, original_scene, "dependency_mismatch");
    auto absent_component = original;
    absent_component["selection_indices"] = Json::array({0});
    absent_component["inspection"] = {{"entity", 0}, {"component", 1000}};
    rejected(absent_component, original_scene, "dependency_mismatch");
    publish(original, original_scene);
    d.ok("repro.open", {{"directory", utf8(copy.wstring())}});
    const auto reopened = d.inspect();
    require(reopened.at("view") == original.at("view") && reopened.at("camera") == original.at("camera") &&
                !reopened.at("dirty").get<bool>() &&
                reopened.at("selection").size() == original.at("selection_indices").size(),
            "Current lighting repro did not reopen with its view, camera and selection");
    d.ok("repro.open", {{"directory", utf8(repro.wstring())}});
}

// Only the scene records the sky map and its rotation; the frozen view holds view options.
// A repro of a scene with both opens, its copied map equals the loaded one, and the
// reopened repro still binds the same map content.
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
    d.op("project.open", {{"path", utf8(project.wstring())}});
    std::string content;
    const auto exported = [&] {
        const auto id = unique_id();
        require(d.call("repro.export", Json::object(), id).at("status") == "pending", "Sky repro export did not start");
        const auto done = d.wait(id);
        require(done.at("status") == "ok", done.dump());
        const std::filesystem::path bundle = wide(done.at("result").at("directory").get<std::string>());
        const auto manifest = read(bundle / "manifest.json");
        auto frozen = read(bundle / "Project" / wide(manifest.at("scene").get<std::string>()));
        const auto& sky = environment_of(frozen);
        require(sky.at("sky_map") == key && sky.at("properties").at("sky_rotation") == 30,
                "A sky repro lost its scene's sky map key or rotation");
        const auto resource =
            std::ranges::find_if(manifest.at("resources"), [&](const Json& entry) { return entry.at("key") == key; });
        require(resource != manifest.at("resources").end(),
                "A sky repro did not check its copied sky map against the loaded one");
        if (content.empty())
            content = resource->at("content_sha256").get<std::string>();
        require(resource->at("content_sha256") == content, "A reopened sky repro bound different sky map content");
        return std::pair{bundle, manifest};
    };
    const auto [bundle, manifest] = exported();
    d.ok("repro.open", {{"directory", utf8(bundle.wstring())}});
    require(d.inspect().at("view") == manifest.at("view"), "A sky repro did not open with its frozen view");
    exported();
}

// The export copies what the scene references, with the in-memory draft of a referenced material
// rather than what its saved file names: a draft that cleared a texture which has since gone
// missing still exports, the copy holds the draft and reopens strictly, and the source asset stays
// unchanged. A material the session loaded but the scene does not reference stays out.
void repro_material_draft(Driver& d, const std::filesystem::path& project, const std::filesystem::path& root) {
    d.op("project.open", {{"path", utf8(project.wstring())}});
    write(root / "Draft.png", std::string(tiny_png.begin(), tiny_png.end()));
    const auto texture = d.op("asset.import", {{"kind", "texture"}, {"path", utf8((root / "Draft.png").wstring())}})
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
    for (const auto* file : {L"Content/Textures/Draft.png", L"Content/Textures/Draft.png.asset.json"})
        std::filesystem::remove(atelier::platform::windows_file_path(project / file));
    d.op("project.open", {{"path", utf8(project.wstring())}});
    const auto opened = d.ok("material.inspect", {{"asset", asset}});
    require(material_issue(opened, texture), "The draft fixture did not lose its texture: " + opened.dump());
    d.ok("material.inspect", {{"asset", unused}});
    d.ok("material.set_texture", {{"object", opened.at("handle")}, {"path", "base_color_texture"}, {"texture", ""}});
    const auto id = unique_id();
    require(d.call("repro.export", Json::object(), id).at("status") == "pending", "Draft repro export did not start");
    const auto done = d.wait(id);
    require(done.at("status") == "ok", "A material draft without its missing texture did not export: " + done.dump());
    const std::filesystem::path bundle = wide(done.at("result").at("directory").get<std::string>());
    require(material_entry(read(bundle / "Project/Content/Materials/Draft.material"), "base_color_texture")
                        .at("value")
                        .at("asset_key") == "" &&
                read(saved).dump() == saved_text,
            "A draft repro did not carry the draft, or changed the saved material");
    require(!std::filesystem::exists(
                atelier::platform::windows_file_path(bundle / "Project/Content/Materials/Unused.material")) &&
                std::ranges::none_of(read(bundle / "manifest.json").at("input_files"),
                                     [](const Json& file) {
                                         return file.at("path").get<std::string>().starts_with(
                                             "Content/Materials/Unused");
                                     }),
            "A repro copied a material the session loaded but the scene does not reference");
    d.op("edit.undo", Json::object(), {{"region", "parameters"}, {"asset", asset}});
    d.ok("repro.open", {{"directory", utf8(bundle.wstring())}});
    require(d.ok("material.inspect", {{"asset", asset}}).at("diagnostics").empty(),
            "A draft repro did not reopen without the missing texture");
}

// Exercise the real author/Runtime/export/strict-load path. Pixel expectations
// come from this RGBA fixture, not from another call to the exporter's decoder.
void repro_material_inputs(Driver& d, const std::filesystem::path& root) {
    const auto project = root / "Project";
    project_fixture(project);
    d.op("project.open", {{"path", utf8(project.wstring())}});
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
    const auto texture = d.op("asset.import", {{"kind", "texture"}, {"path", utf8((root / "Active.bmp").wstring())}})
                             .at("result")
                             .at("asset")
                             .get<std::string>();
    const auto dormant = d.op("asset.import", {{"kind", "texture"}, {"path", utf8((root / "Dormant.bmp").wstring())}})
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
        const std::filesystem::path bundle = wide(done.at("result").at("directory").get<std::string>());
        const auto manifest = read(bundle / "manifest.json");
        require(manifest.at("material_resources") == expected(pixels),
                "Repro digests changed MS text/policy, sparse entries, parents or exact RGBA pixels");
        require(sha256_file(bundle / "Project" / wide(source_path)) == sha256(std::as_bytes(std::span(source_text))),
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
    write(root / "Replacement.bmp", bitmap(next_pixels));
    d.op("asset.reimport", {{"path", utf8((root / "Replacement.bmp").wstring())}}, {{"asset", texture}});
    const auto after_reimport = d.inspect();
    for (const auto* field : {"undo", "redo", "dirty", "dirty_materials"})
        require(before_reimport.at(field) == after_reimport.at(field), "Texture Reimport entered parameter history");
    require(sha256_file(project / (texture_file + ".asset.json")) == identity_hash,
            "Texture Reimport rewrote its identity sidecar");
    d.op("edit.undo", Json::object(), {{"region", "parameters"}, {"asset", parent}});
    require(!d.ok("material.inspect", {{"asset", parent}}).at("dirty").get<bool>(),
            "Parameter Undo did not restore the saved author values");
    // Saved files now disagree with the loaded MS and imported pixels. Neither
    // this ordinary parameter Undo nor exporting may restore/read those files.
    const auto disk_text = source_text + "\n// saved after the MS was loaded\n";
    write(project / wide(source_path), disk_text);
    write(project / texture_file, bitmap(first_pixels));
    const auto disk_image = sha256_file(project / texture_file);
    const auto frozen = exported(next_pixels);
    const auto manifest = read(frozen / "manifest.json");
    require(sha256_file(project / wide(source_path)) == sha256(std::as_bytes(std::span(disk_text))) &&
                sha256_file(project / texture_file) == disk_image,
            "Export modified the source project's saved Source or texture");
    const auto reopened = [&](const std::filesystem::path& bundle) {
        d.ok("repro.open", {{"directory", utf8(bundle.wstring())}});
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
    std::filesystem::copy(atelier::platform::windows_file_path(frozen), atelier::platform::windows_file_path(changed),
                          std::filesystem::copy_options::recursive);
    const auto bytes = [](const std::filesystem::path& path) {
        std::ifstream stream(atelier::platform::windows_file_path(path), std::ios::binary);
        require(bool(stream), "Cannot read material repro fixture");
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    auto observations = Json::array();
    d.settle();
    for (const auto* kind : {"source-text", "policy", "disabled-entry", "parent", "pixels"}) {
        std::string relative;
        std::string contents;
        if (std::string_view(kind) == "source-text") {
            relative = source_path;
            contents = source_text + "\n// valid changed Source\n";
        } else if (std::string_view(kind) == "policy") {
            relative = source_file;
            auto author = source_author;
            author["policy"]["double_sided"] = false;
            contents = author.dump(2);
        } else if (std::string_view(kind) == "disabled-entry") {
            relative = leaf_file;
            auto author = leaf_author;
            for (auto& entry : author.at("entries"))
                if (entry.at("path") == "dormant")
                    entry["value"]["asset_key"] = "builtin/still-disabled";
            contents = author.dump(2);
        } else if (std::string_view(kind) == "parent") {
            relative = leaf_file;
            auto author = leaf_author;
            author["parent_key"] = source;
            contents = author.dump(2);
        } else {
            relative = texture_file;
            contents = bitmap(first_pixels);
        }
        const auto target = changed / "Project" / wide(relative);
        write(target, contents);
        auto modified = manifest;
        bool listed = false;
        for (auto& file : modified.at("files"))
            if (file.at("path") == "Project/" + relative) {
                file["sha256"] = sha256_file(target);
                file["bytes"] = std::filesystem::file_size(atelier::platform::windows_file_path(target));
                listed = true;
            }
        require(listed, "Changed material input is absent from the manifest");
        write(changed / "manifest.json", modified.dump(2));
        const auto before = d.inspect();
        const auto token = d.token;
        const auto response = d.call("repro.open", {{"directory", utf8(changed.wstring())}});
        error(response, "dependency_mismatch");
        const auto after = d.inspect();
        for (const auto* field : {"project", "scene_path", "entities", "selection", "camera", "view", "dirty",
                                  "dirty_materials", "undo", "redo"})
            require(before.at(field) == after.at(field), "Rejected material repro changed " + std::string(field));
        require(d.token == token, "Rejected material repro changed the accepted state token");
        observations.push_back({{"case", kind}, {"response", response}});
        write(target, bytes(frozen / "Project" / wide(relative)));
    }
    write(changed / "manifest.json", manifest.dump(2));
    reopened(changed);
    exported(next_pixels);
    write(root / "material-inputs.json",
          Json{{"status", "passed"},
               {"first_rgba", sha256(std::as_bytes(std::span(first_pixels)))},
               {"reimported_rgba", sha256(std::as_bytes(std::span(next_pixels)))},
               {"material_resources", manifest.at("material_resources")},
               {"rejected", std::move(observations)},
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
    const auto removed = d.try_op("view.set", {{"direct_reference", true}});
    error(removed, "invalid_request");
    require(removed.at("error").at("message").get<std::string>().find("direct_reference") != std::string::npos,
            "Control did not identify the rejected direct reference diagnostic: " + removed.dump());
    d.op("view.set", {{"direct_candidates", 16}, {"exposure_ev", 1.25}});
    d.op("scene.save");
    const auto id = unique_id();
    require(d.call("repro.export", Json::object(), id).at("status") == "pending",
            "Lighting repro export did not start");
    const auto exported = d.wait(id);
    require(exported.at("status") == "ok", exported.dump());
    repro_lighting_agreement(d, wide(exported.at("result").at("directory").get<std::string>()), root);
    repro_sky(d, project);
    repro_material_draft(d, project, root);
    repro_material_inputs(d, root / "MaterialInputs");
    d.client.reset();
    editor.close();
    write(root / "result.json",
          Json{{"status", "passed"},
               {"cases",
                {"current-schema-and-format-only", "retired-view-keys-rejected", "rejected-repro-preserves-state",
                 "view-and-environment-agreement", "selection-resolved-before-replacement", "current-repro-reopen",
                 "sky-map-and-rotation", "material-draft-dependencies", "material-exact-inputs-and-strict-reopen"}}}
              .dump(2));
    std::cout << "Editor Repro Lighting E2E: PASS | " << utf8(root.wstring()) << '\n';
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
    error(d.wait(id), "invalid_destination");
    require(std::filesystem::is_empty(atelier::platform::windows_file_path(
                directory / "artifacts" / wide(d.descriptor.at("instance_id").get<std::string>()))),
            "Rejected nested export left a recursive output");
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
        std::filesystem::create_directories(atelier::platform::windows_file_path((project / folder).parent_path()));
        std::filesystem::copy(atelier::platform::windows_file_path(shipped / folder),
                              atelier::platform::windows_file_path(project / folder),
                              std::filesystem::copy_options::recursive);
    }
    const std::string sponza = "Content/Sponza/";
    const std::string building = sponza + "NewSponza/NewSponza_Main_glTF_003.gltf";
    const std::string curtains = sponza + "SponzaExtend/NewSponza_Curtains_glTF.gltf";
    // A mesh may spell an image in another case than its file; the repro copies the catalogue's file once.
    const std::string image = "textures/arch_stone_wall_01_BaseColor.png";
    const std::string alias = "Textures/Arch_Stone_Wall_01_BaseColor.png";
    {
        std::ifstream stream(atelier::platform::windows_file_path(project / building), std::ios::binary);
        std::string text(std::istreambuf_iterator<char>(stream), {});
        const auto at = text.find("\"" + image + "\"");
        require(at != std::string::npos, "The New Sponza glTF fixture changed");
        text.replace(at + 1, image.size(), alias);
        write(project / building, text);
    }
    // Every slot default the two mesh sidecars name, by the material file that holds its identity.
    std::map<std::string, std::string> material_files;
    std::set<std::string> defaults;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(atelier::platform::windows_file_path(project / sponza)))
        if (auto path =
                utf8(entry.path().lexically_relative(atelier::platform::windows_file_path(project)).generic_wstring());
            path.ends_with(".material.asset.json")) {
            const auto key = "project/" + read(entry.path()).at("id").get<std::string>();
            path.resize(path.size() - 11);
            material_files[key] = std::move(path);
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
        Json resources;
        std::set<std::string> inputs;
        const auto exported = [&] {
            const auto id = unique_id();
            const auto started = d.call("repro.export", Json::object(), id);
            require(started.at("status") == "pending", started.dump());
            const auto done = d.wait(id);
            require(done.at("status") == "ok", done.dump());
            const std::filesystem::path bundle = wide(done.at("result").at("directory").get<std::string>());
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
            if (inputs.empty()) {
                inputs = copied;
                resources = manifest.at("resources");
            }
            require(copied == inputs && manifest.at("resources") == resources,
                    "A reopened Examples repro carried different assets");
            return std::pair{bundle, manifest};
        };
        const auto [bundle, manifest] = exported();
        require(manifest.at("resources").size() == 3,
                "An Examples repro did not check Sponza's two meshes and its sky map: " +
                    manifest.at("resources").dump());
        d.ok("repro.open", {{"directory", utf8(bundle.wstring())}});
        require(d.inspect().at("view") == manifest.at("view"), "An Examples repro did not open with its frozen view");
        exported();
        d.client.reset();
        editor.close();
    }
    std::filesystem::remove_all(atelier::platform::windows_file_path(root));
} catch (...) {
    std::fprintf(stderr, "Examples repro failure evidence retained at %s\n", utf8(root.wstring()).c_str());
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
    const auto mesh = d.op("asset.import", {{"kind", "mesh"}, {"path", utf8((source / "scene.gltf").wstring())}})
                          .at("result")
                          .at("asset")
                          .get<std::string>();
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
        const std::filesystem::path bundle = wide(done.at("result").at("directory").get<std::string>());
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
            if (file.is_regular_file() && *file.path().lexically_relative(project).begin() != L"Cache")
                hashes.emplace(file.path(), sha256_file(file.path()));
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
    d.ok("repro.open", {{"directory", utf8(bundle.wstring())}});
    exported();
    auto reopened_defaults = Json::object();
    for (const auto& binding : component_details()[0].at("object").at("mesh").at("default_materials"))
        reopened_defaults[binding.at("slot").get<std::string>()] = binding.at("source");
    require(reopened_defaults == defaults, "Reopened repro changed slot relationships");
    // Live material drafts are intentionally frozen, unlike external dependency drift.
    const auto draft_source = bundle / "Project" / trim;
    const auto saved_material = sha256_file(draft_source);
    const auto trim_key = defaults.at("trim").get<std::string>();
    const auto handle = d.ok("material.inspect", {{"asset", trim_key}}).at("handle");
    d.ok("material.set_entry",
         {{"object", handle},
          {"entry", {{"path", "roughness"}, {"type", "float"}, {"enabled", true}, {"value", 0.625}}}});
    const auto draft_bundle = exported();
    require(material_entry(read(draft_bundle / "Project" / trim), "roughness").at("value") == 0.625 &&
                sha256_file(draft_source) == saved_material &&
                d.ok("material.inspect", {{"asset", trim_key}}).at("dirty") == true,
            "Repro lost or saved the source material draft");
    d.op("edit.undo", Json::object(), {{"region", "parameters"}, {"asset", trim_key}});
    d.ok("repro.open", {{"directory", utf8(draft_bundle.wstring())}});
    const auto reopened_material = d.ok("material.inspect", {{"asset", trim_key}});
    require(std::ranges::any_of(reopened_material.at("declarations"),
                                [](const Json& p) { return p.at("path") == "roughness" && p.at("value") == 0.625; }),
            "Repro did not load the frozen material draft");
    // A component keeps its material bindings while its mesh is missing. Only the scene names the
    // bound material then, and the repro still carries it.
    d.op("project.open", {{"path", utf8(project.wstring())}});
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
    d.op("project.open", {{"path", utf8(project.wstring())}});
    const auto missing_id = unique_id();
    require(d.call("repro.export", Json::object(), missing_id).at("status") == "pending",
            "Missing-default export did not start");
    const auto missing_done = d.wait(missing_id);
    require(missing_done.at("status") == "ok", missing_done.dump());
    const std::filesystem::path missing_bundle = wide(missing_done.at("result").at("directory").get<std::string>());
    require(!std::filesystem::exists(missing_bundle / "Project" / trim) &&
                read(missing_bundle / "Project" / (folder + "scene.gltf.asset.json")).at("materials") == defaults,
            "Missing-default export synthesized a material or rebound the slots");
    const auto before_missing_open = d.inspect();
    error(d.call("repro.open", {{"directory", utf8(missing_bundle.wstring())}}), "operation_failed");
    require(d.inspect().at("entities") == before_missing_open.at("entities") &&
                d.inspect().at("project") == before_missing_open.at("project"),
            "Strict missing-default reopen replaced the document");
    write(project / trim, trim_bytes);
    std::filesystem::remove(atelier::platform::windows_file_path(project / (folder + "scene.gltf")));
    d.op("project.open", {{"path", utf8(project.wstring())}});
    const auto id = unique_id();
    require(d.call("repro.export", Json::object(), id).at("status") == "pending",
            "Missing-mesh repro export did not start");
    const auto done = d.wait(id);
    require(done.at("status") == "ok", done.dump());
    std::set<std::string> copied;
    for (const auto& file :
         read(std::filesystem::path(wide(done.at("result").at("directory").get<std::string>())) / "manifest.json")
             .at("input_files"))
        copied.insert(file.at("path").get<std::string>());
    require(copied.contains("Content/Materials/Bound.material") &&
                copied.contains("Content/Materials/Bound.material.asset.json") && !copied.contains(material),
            "A repro of a component whose mesh is missing lost its bound material, or copied the mesh's default");
    d.client.reset();
    editor.close();
    write(root / "result.json",
          Json{{"status", "passed"},
               {"cases",
                {"slot-relationship-drift", "loaded-content-drift", "mixed-loaded-versions", "equal-loaded-versions",
                 "repaired-retry", "slot-relationship-reopen", "default-material-draft",
                 "missing-default-export-strict-reopen", "missing-mesh-bound-material"}}}
              .dump(2));
}

void repro_paths(const std::filesystem::path& executable, const std::filesystem::path& root, bool deep_control) {
    const auto project = root / "Project";
    const auto directory = deep_control ? root / "Control" / std::wstring(100, L'x') : root / "Control";
    project_fixture(project);
    // The frozen copy is always named Control-<id>.scene. With a short control directory a
    // long-name Source file, copied whole, is what crosses MAX_PATH ("Project/Source/" plus
    // 230 characters); with the deep control directory the frozen copy itself does. The
    // long-name startup scene is read, not copied.
    const auto scene = std::filesystem::path("Content/Scenes") /
                       ("Control-" + unique_id() + (deep_control ? "" : "-" + std::string(200, 'n')) + ".scene");
    const auto long_source = std::filesystem::path("Source") / (std::string(226, 'l') + ".txt");
    write(project / long_source, "copied whole");
    std::filesystem::rename(atelier::platform::windows_file_path(project / "Content/Scenes/Main.scene"),
                            atelier::platform::windows_file_path(project / scene));
    const auto bytes = [](const std::filesystem::path& path) {
        std::ifstream stream(atelier::platform::windows_file_path(path), std::ios::binary);
        require(bool(stream), "Cannot read E2E artifact: " + utf8(path.wstring()));
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const auto source_scene = bytes(project / scene);
    auto config = read(project / "Config/Project.json");
    config["startup_scene"] = utf8(scene.generic_wstring());
    write(project / "Config/Project.json", config.dump());
    Process editor(executable,
                   L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --no-activate",
                   root / "editor.log");
    Driver d(discover_editor(editor, directory));
    d.settle();
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}});
    const auto unpublished = std::filesystem::path("Source") / (std::wstring(72, L'u') + L".source");
    write(project / unpublished, "invalid saved source");
    std::this_thread::sleep_for(750ms);
    d.inspect();
    std::filesystem::path repro;
    for (unsigned pass = 0; pass < 2; ++pass) {
        const auto id = unique_id();
        const auto started = d.call("repro.export", Json::object(), id);
        require(started.at("status") == "pending", started.dump());
        const auto exported = d.wait(id);
        require(exported.at("status") == "ok", exported.dump());
        repro = wide(exported.at("result").at("directory").get<std::string>());
        const auto manifest = read(repro / "manifest.json");
        const auto frozen = repro / "Project" / wide(manifest.at("scene").get<std::string>());
        if (deep_control)
            require(frozen.wstring().size() > 260, "Deep-control export did not place the frozen scene past MAX_PATH");
        else {
            const auto copied = repro / "Project" / long_source;
            require(copied.wstring().size() > 260 && bytes(copied) == "copied whole",
                    "Short-control export did not copy the long-name Source file unchanged");
            if (pass == 0)
                require(bytes(project / scene) == source_scene, "Export changed the long-name source scene");
        }
        require(read(frozen).at("entities").size() == 1, "Deep-path export did not preserve the scene");
        if (pass == 0)
            require(std::filesystem::exists(atelier::platform::windows_file_path(repro / "unpublished" / unpublished)),
                    "Deep-path source archive is missing");
        if (pass == 0)
            d.op("edit.undo");
        d.ok("repro.open", {{"directory", utf8(repro.wstring())}});
        const auto reopened = d.inspect();
        require(reopened.at("entities").size() == 1 && !reopened.at("dirty").get<bool>(),
                "Deep-path repro did not reopen");
        d.op("scene.save");
    }
    const auto instance = d.descriptor.at("instance_id").get<std::string>();
    const auto request_path = repro / "Project/Cache" / (std::wstring(80, L'q') + L".request.json");
    require(request_path.wstring().size() > 260, "CLI request fixture is not a long path");
    write(request_path, Json{{"op", "editor.inspect"}}.dump());
    d.client.reset();
    Process cli(executable.parent_path() / "atelier_control.exe",
                L"--directory " + quote(directory) + L" --instance " + wide(instance) + L" --request " +
                    quote(request_path),
                root / "cli.json");
    require(WaitForSingleObject(cli.process, 30000) == WAIT_OBJECT_0 && read(root / "cli.json").at("status") == "ok",
            "CLI could not read a deep request or discover the editor");
    editor.close();
    require(
        !std::filesystem::exists(atelier::platform::windows_file_path(directory / "instances" / (instance + ".json"))),
        "Deep-path instance descriptor was not removed on close");
    write(root / "result.json", Json{{"status", "passed"},
                                     {"cases",
                                      {"long-destination", "long-source", "deep-project-reopen-save", "source-archive",
                                       "cli-request", "descriptor-cleanup"}},
                                     {"deep_control", deep_control},
                                     {"last_repro", utf8(repro.wstring())}}
                                    .dump(2));
    std::cout << "Editor Control path E2E: PASS | " << utf8(root.wstring()) << '\n';
}

void ddgi_debug(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto project = root / "Project";
    const auto directory = root / "Control";
    project_fixture(project);
    Process editor(executable,
                   L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --no-activate",
                   root / "editor.log");
    Driver d(discover_editor(editor, directory));
    if (auto window = editor.window())
        SetWindowPos(window, HWND_TOPMOST, 12, 80, 960, 640, SWP_NOACTIVATE);
    auto state = d.inspect();
    unsigned stable = 0;
    for (unsigned i = 0; i < 200 && stable < 5; ++i) {
        const auto token = d.token;
        std::this_thread::sleep_for(30ms);
        state = d.inspect();
        stable = token == d.token && !state.at("last_frame").empty() ? stable + 1 : 0;
    }
    require(stable == 5, "DDGI diagnostic editor did not settle");
    const auto capabilities = d.ok("capabilities");
    verify_option_descriptions(capabilities);
    for (const char* name :
         {"probe_visualization", "probe_volume", "probe_relocation", "probe_xray", "probe_radius", "probe_debug",
          "probe_bounce", "indirect_gain", "probe_feedback_visibility", "probe_coverage", "probe_debug_volume"})
        require(capabilities.at("display_options").contains(name),
                "DDGI debug option absent from shared Control schema");
    for (unsigned slot = 0; slot < 4; ++slot)
        for (const char* field : {"enabled", "placement", "min_x", "min_y", "min_z", "max_x", "max_y", "max_z",
                                  "count_x", "count_y", "count_z", "blend_cells"})
            require(capabilities.at("display_options").contains("probe_volume_" + std::to_string(slot) + "_" + field),
                    "Volume field absent from Control schema");
    const auto summary = d.ok("editor.inspect", {{"detail", "summary"}});
    require(!summary.contains("entities") && !summary.contains("assets") && !summary.contains("component_types") &&
                summary.at("view") == state.at("view") && summary.contains("last_frame"),
            "Summary inspection enumerated the document or omitted view state");
    d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}});
    d.op("view.look_at", {{"eye", {{"x", 5}, {"y", 4}, {"z", -7}}}});
    Json patch{{"shading", "scene_lit"},
               {"ddgi", true},
               {"gtao", false},
               {"sssr", false},
               {"bloom", false},
               {"probe_volume_0_count_x", 3},
               {"probe_volume_0_count_y", 3},
               {"probe_volume_0_count_z", 3},
               {"probe_update_budget", 27},
               {"probe_rays", 32},
               {"probe_volume_0_min_x", -2},
               {"probe_volume_0_min_y", -2},
               {"probe_volume_0_min_z", -2},
               {"probe_volume_0_max_x", 2},
               {"probe_volume_0_max_y", 2},
               {"probe_volume_0_max_z", 2},
               {"probe_visualization", "state"},
               {"probe_volume", true},
               {"probe_relocation", true},
               {"probe_xray", true},
               {"probe_radius", .12}};
    d.op("view.set", patch);
    state = d.inspect();
    const auto base_view = state.at("view");
    const auto entities = state.at("entities");
    const auto undo = state.at("undo");
    const auto token = d.token;
    d.op("view.set", patch);
    require(d.token == token, "Repeated DDGI view patch changed the state token");
    error(d.try_op("view.set", {{"probe_radius", 0}, {"probe_debug", true}}), "operation_failed");
    require(d.inspect().at("view") == base_view, "Rejected debug patch partially mutated the view");
    error(d.try_op("view.set", {{"probe_visualization", "unknown"}}), "invalid_request");
    const auto id = unique_id();
    const Json cases = Json::array({{{"name", "probe-state"}, {"patch", Json::object()}},
                                    {{"name", "probe-age"}, {"patch", {{"probe_visualization", "update_age"}}}},
                                    {{"name", "receiver-coverage"},
                                     {"patch",
                                      {{"probe_visualization", "off"},
                                       {"probe_volume", false},
                                       {"probe_relocation", false},
                                       {"probe_debug", true}}}},
                                    {{"name", "ddgi-disabled"}, {"patch", {{"ddgi", false}, {"probe_debug", true}}}}});
    auto pending = d.call("render.compare", {{"cases", cases}, {"sample_frames", {2}}, {"timeout_ms", 120000}}, id);
    require(pending.at("status") == "pending", pending.dump());
    const auto compared = d.wait(id);
    require(compared.at("status") == "ok", compared.dump());
    require(compared.at("result").at("samples").size() == 4, "DDGI comparison did not produce every accepted sample");
    state = d.inspect();
    require(state.at("view") == base_view && state.at("entities") == entities && state.at("undo") == undo,
            "DDGI diagnostic comparison changed scene, undo or base display policy");
    write(root / "captures.json", compared.dump(2));
    write(root / "capabilities.json", capabilities.dump(2));
    d.op("view.set", {{"probe_bounce", .5}, {"indirect_gain", 1.25}, {"probe_feedback_visibility", false}});
    d.ok("op.call", {{"id", "view.lighting_fit"}, {"args", {{"mode", "scene"}}}});
    state = d.inspect();
    require(state.at("view").at("probe_volume_0_placement") == "fixed" && state.at("view").at("probe_bounce") == .5 &&
                state.at("view").at("indirect_gain") == 1.25,
            "Fit Scene changed propagation preferences");
    d.ok("op.call", {{"id", "view.lighting_fit"}, {"args", {{"mode", "scene_focus"}}}});
    state = d.inspect();
    const auto focus_view = state.at("view");
    const auto layout_token = d.token;
    require(focus_view.at("probe_volume_0_placement") == "follow_focus" &&
                focus_view.at("probe_volume_1_enabled") == true &&
                focus_view.at("probe_volume_1_placement") == "fixed" && state.at("entities") == entities &&
                state.at("undo").get<unsigned>() == undo.get<unsigned>() + 3,
            "Scene+Focus layout lost a volume or did not commit to scene history");
    d.ok("op.call", {{"id", "view.lighting_fit"}, {"args", {{"mode", "scene_focus"}}}});
    require(d.token == layout_token, "Repeated layout changed the state token");
    for (const auto& invalid : std::vector<Json>{
             {{"probe_volume_0_count_y", 4}},
             {{"probe_volume_0_count_x", 32}, {"probe_volume_0_count_y", 32}, {"probe_volume_0_count_z", 32}},
             {{"probe_volume_0_max_x", -6}},
             {{"probe_volume_2_placement", "follow_focus"}, {"probe_volume_2_count_x", 3}}}) {
        error(d.try_op("view.set", invalid), "operation_failed");
        require(d.ok("editor.inspect", {{"detail", "summary"}}).at("view") == focus_view && d.token == layout_token,
                "Rejected volume configuration changed state or token");
    }
    d.op("view.look_at", {{"eye", {{"x", 12}, {"y", 4}, {"z", -7}}}, {"target", {{"x", 7}}}});
    const auto capture_id = unique_id();
    pending = d.call("render.capture", {{"sample_frames", {2}}, {"timeout_ms", 120000}}, capture_id);
    require(pending.at("status") == "pending", pending.dump());
    const auto captured = d.wait(capture_id);
    require(captured.at("status") == "ok", captured.dump());
    const auto& frame = captured.at("result").at("samples").at(0);
    const auto& volumes = frame.at("frame_stats").at("probe_volumes");
    require(frame.at("lighting_focus") == Json::array({7, 0, 0}) && volumes.size() == 4 &&
                volumes.at(0).at("rolling") == true && volumes.at(0).at("minimum").at(0).get<float>() >= 0 &&
                volumes.at(1).at("enabled") == true &&
                frame.at("frame_stats").at("probe_volumes_source") == "cpu_layout_and_schedule",
            "Accepted frame did not report camera-target rolling volume placement and CPU schedule provenance");
    write(root / "focus-capture.json", captured.dump(2));
    const Json tiny{{"probe_volume_0_count_x", 5},   {"probe_volume_0_count_y", 5},   {"probe_volume_0_count_z", 5},
                    {"probe_volume_0_min_x", -.004}, {"probe_volume_0_min_y", -.004}, {"probe_volume_0_min_z", -.004},
                    {"probe_volume_0_max_x", .004},  {"probe_volume_0_max_y", .004},  {"probe_volume_0_max_z", .004}};
    d.op("view.look_at", {{"eye", {{"x", 100005}, {"y", 4}, {"z", -7}}}, {"target", {{"x", 100000}}}});
    const auto far_state = d.ok("editor.inspect", {{"detail", "summary"}});
    const auto far_token = d.token;
    error(d.try_op("view.set", tiny), "operation_failed");
    require(d.ok("editor.inspect", {{"detail", "summary"}}).at("view") == far_state.at("view") && d.token == far_token,
            "World-precision failure published unusable view settings");
    d.op("view.look_at", {{"eye", {{"x", 5}, {"y", 4}, {"z", -7}}}});
    d.op("view.set", tiny);
    const auto small_state = d.ok("editor.inspect", {{"detail", "summary"}});
    const auto small_token = d.token;
    error(d.try_op("view.look_at", {{"eye", {{"x", 100005}, {"y", 4}, {"z", -7}}}, {"target", {{"x", 100000}}}}),
          "operation_failed");
    auto far_camera = small_state.at("camera");
    far_camera["target"] = {{"x", 100000}, {"y", 0}, {"z", 0}};
    error(d.try_op("view.camera_set", {{"state", far_camera}}), "operation_failed");
    require(d.ok("editor.inspect", {{"detail", "summary"}}).at("camera") == small_state.at("camera") &&
                d.token == small_token,
            "World-precision failure published an unusable camera");
    d.ok("op.call", {{"id", "view.lighting_fit"}, {"args", {{"mode", "scene_focus"}}}});
    d.op("view.set", {{"probe_volume_2_enabled", true},
                      {"probe_volume_3_enabled", true},
                      {"probe_volume_2_count_x", 3},
                      {"probe_volume_3_count_x", 2},
                      {"probe_debug_volume", 2},
                      {"probe_coverage", "contributors"}});
    const auto repro_view = d.inspect().at("view");
    const auto export_id = unique_id();
    require(d.call("repro.export", Json::object(), export_id).at("status") == "pending",
            "Multi-volume export did not start");
    const auto exported = d.wait(export_id);
    require(exported.at("status") == "ok", exported.dump());
    const std::filesystem::path repro = wide(exported.at("result").at("directory").get<std::string>());
    auto manifest = read(repro / "manifest.json");
    require(manifest.at("view") == repro_view, "Repro lost canonical volume settings");
    d.op("edit.undo");
    d.op("scene.save", {{"path", "Content/Scenes/Editing.scene"}});
    d.ok("repro.open", {{"directory", utf8(repro.wstring())}});
    require(d.inspect().at("view") == repro_view, "Repro did not restore all four volume settings");
    d.client.reset();
    editor.close();
    require(read(project / "Content/Scenes/Main.scene").at("entities").empty(),
            "Diagnostic session changed the saved scene");
    write(root / "result.json",
          Json{{"status", "passed"},
               {"cases",
                {"shared-schema", "summary-inspection", "idempotent-patch", "atomic-rejection", "real-render-compare",
                 "coverage-presets", "rolling-focus-capture", "focus-precision-atomicity", "four-volume-repro",
                 "history-and-scene-isolation", "clean-close"}}}
              .dump(2));
    std::cout << "Editor DDGI Control E2E: PASS | " << utf8(root.wstring()) << '\n';
}

void camera_projection(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto project = root / "Project";
    const auto directory = root / "Control";
    project_fixture(project);
    Process editor(executable,
                   L"--project " + quote(project) + L" --control-directory " + quote(directory) +
                       L" --no-activate --gpu-validation",
                   root / "editor.log");
    Driver d(discover_editor(editor, directory));
    const auto initial = d.settle().at("camera");
    const auto mesh = d.ok("op.call", {{"id", "scene.add"}, {"args", {{"kind", "mesh"}}}}).at("active");
    // A normal authoring range with a known front face 2.5 metres from the eye.
    d.op("scene.transform_set", {{"position", {{"z", -3}}}, {"scale", {{"x", .5}, {"y", .5}}}}, selection(mesh));
    d.op("scene.select", Json::object(), {{"selection", {{"entities", Json::array()}}}});
    d.op("scene.save");
    auto cases = Json::array();
    for (const unsigned view : {0u, 2u}) {
        const std::string name = view == 0 ? "perspective" : "orthographic";
        auto camera = initial;
        camera["view"] = view == 0 ? "perspective" : "front";
        camera["target"] = {{"x", 0}, {"y", 0}, {"z", -1}};
        camera["yaw"] = 0;
        camera["pitch"] = 0;
        camera["roll"] = 0;
        camera["distance"] = 1;
        camera["vertical_fov"] = 60;
        camera["near_clip"] = .25;
        camera["far_clip"] = 6.5;
        d.op("view.camera_set", {{"state", camera}});
        const auto accepted = d.settle().at("camera");
        require(accepted.at("near_clip").get<float>() == .25f && accepted.at("far_clip").get<float>() == 6.5f &&
                    accepted.at("view") == camera.at("view") && accepted.at("vertical_fov") == 60,
                "Control changed camera projection parameters");
        auto invalid = accepted;
        invalid["far_clip"] = invalid.at("near_clip");
        error(d.try_op("view.camera_set", {{"state", invalid}}), "invalid_request");
        require(d.inspect().at("camera") == accepted, "Rejected equal clip planes changed the camera");
        const auto export_id = unique_id();
        require(d.call("repro.export", Json::object(), export_id).at("status") == "pending",
                "Camera repro export did not start");
        const auto exported = d.wait(export_id);
        require(exported.at("status") == "ok", exported.dump());
        const std::filesystem::path repro = wide(exported.at("result").at("directory").get<std::string>());
        const auto manifest = read(repro / "manifest.json");
        require(manifest.at("camera") == accepted, "Repro changed the camera projection");
        require(manifest.at("engine_sources").is_object() && !manifest.at("engine_sources").empty(),
                "Repro did not identify its published engine Source dependencies");
        if (view == 0) {
            const auto before = d.inspect();
            for (unsigned change = 0; change < 3; ++change) {
                auto incompatible = manifest;
                auto& engine = incompatible.at("engine_sources");
                if (change == 0)
                    engine.begin().value() = std::string(64, '0');
                else if (change == 1)
                    engine.erase(engine.begin());
                else
                    engine["engine/absent/Extra.source"] = std::string(64, '0');
                write(repro / "manifest.json", incompatible.dump(2));
                error(d.call("repro.open", {{"directory", utf8(repro.wstring())}}), "dependency_mismatch");
                const auto after = d.inspect();
                require(after.at("project") == before.at("project") && after.at("entities") == before.at("entities") &&
                            after.at("selection") == before.at("selection") &&
                            after.at("camera") == before.at("camera"),
                        "An incompatible engine Source dependency replaced the current document");
            }
            write(repro / "manifest.json", manifest.dump(2));
        }
        d.op("view.camera_set", {{"state", initial}});
        // Switching view kind updates its derived grid plane on the next editor step.
        require(d.settle().at("camera") == initial, "Camera did not reset before repro reopen");
        d.ok("repro.open", {{"directory", utf8(repro.wstring())}});
        require(d.settle().at("camera") == accepted, "Repro did not restore the projection and view kind");
        const auto capture_id = unique_id();
        require(d.call("render.capture", {{"sample_frames", {1}}}, capture_id).at("status") == "pending",
                "Camera capture did not start");
        const auto captured = d.wait(capture_id);
        require(captured.at("status") == "ok", captured.dump());
        const auto& sample = captured.at("result").at("samples").at(0);
        const std::filesystem::path output = wide(captured.at("result").at("directory").get<std::string>());
        require(sample.at("camera").at("far_depth").get<float>() == (view == 0 ? .25f / 6.5f : 0),
                "Capture metadata lost the independent finite far boundary");
        const auto pixels = sample.at("extent")[0].get<std::size_t>() * sample.at("extent")[1].get<std::size_t>();
        std::ifstream input(output / wide(sample.at("depth").get<std::string>()), std::ios::binary);
        std::vector<float> depth(pixels);
        input.read(reinterpret_cast<char*>(depth.data()), static_cast<std::streamsize>(pixels * sizeof(float)));
        const auto geometry = std::ranges::count_if(depth, [](float z) { return z > 0; });
        require(bool(input) &&
                    std::ranges::all_of(depth, [](float z) { return std::isfinite(z) && z >= 0 && z <= 1; }) &&
                    geometry > 100,
                "Camera projection did not capture visible geometry with finite reversed-Z depth");
        const float expected_depth = view == 0 ? .25f / 2.5f : (6.5f - 2.5f) / (6.5f - .25f);
        require(std::abs(*std::ranges::max_element(depth) - expected_depth) < .0001f,
                "Captured front-face depth does not match the camera's projection kind");
        std::filesystem::copy_file(output / wide(sample.at("color").get<std::string>()), root / (name + ".png"));
        cases.push_back({{"view", name},
                         {"camera", accepted},
                         {"geometry_pixels", geometry},
                         {"repro", utf8(repro.wstring())},
                         {"capture", captured.at("result")}});
        auto shortened = accepted;
        shortened["far_clip"] = 2;
        d.op("view.camera_set", {{"state", shortened}});
        d.settle();
        const auto clipped_id = unique_id();
        require(d.call("render.capture", {{"sample_frames", {1}}}, clipped_id).at("status") == "pending",
                "Far clip capture did not start");
        const auto clipped = d.wait(clipped_id);
        require(clipped.at("status") == "ok", clipped.dump());
        const auto& clipped_sample = clipped.at("result").at("samples").at(0);
        const std::filesystem::path clipped_output = wide(clipped.at("result").at("directory").get<std::string>());
        std::ifstream clipped_input(clipped_output / wide(clipped_sample.at("depth").get<std::string>()),
                                    std::ios::binary);
        clipped_input.read(reinterpret_cast<char*>(depth.data()), static_cast<std::streamsize>(pixels * sizeof(float)));
        require(bool(clipped_input) && std::ranges::all_of(depth, [](float z) { return z == 0; }),
                "Camera far edit did not clip the actual mesh to background depth zero");
        if (view == 0)
            require(clipped_sample.at("camera").at("projection") == sample.at("camera").at("projection") &&
                        clipped_sample.at("camera").at("far_depth").get<float>() == .125f,
                    "Perspective far edit changed P instead of its derived clip boundary");
        cases.back()["far_clipped_capture"] = clipped.at("result");
        d.op("view.camera_set", {{"state", accepted}});
        d.settle();
        std::cout << "Camera Control " << name
                  << ": PASS (set, equality rejected, export/open, captured geometry=" << geometry << ")\n"
                  << std::flush;
    }
    d.client.reset();
    editor.close();
    require(discover(directory).empty(), "Camera editor retained its Control endpoint");
    write(root / "result.json", Json{{"status", "passed"}, {"cases", cases}}.dump(2));
    std::cout << "Editor camera projection E2E: PASS | " << utf8(root.wstring()) << '\n';
}
#include "client_size.inl"
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(
            argc == 3 ||
                (argc == 4 &&
                 (std::wstring_view(argv[3]) == L"--repro-paths" || std::wstring_view(argv[3]) == L"--ddgi-debug" ||
                  std::wstring_view(argv[3]) == L"--repro-lighting" ||
                  std::wstring_view(argv[3]) == L"--descriptions" || std::wstring_view(argv[3]) == L"--client-size" ||
                  std::wstring_view(argv[3]) == L"--camera-projection")),
            "Usage: atelier_editor_control_acceptance editor.exe output-directory [--repro-paths|--ddgi-debug|--repro-lighting|--descriptions|--client-size|--camera-projection]");
        const bool descriptions = argc == 4 && std::wstring_view(argv[3]) == L"--descriptions";
        // Launched editors and their Play processes inherit the editor's harness cache, not the user's.
        if (!GetEnvironmentVariableW(L"ATELIER_SHADER_CACHE", nullptr, 0))
            SetEnvironmentVariableW(L"ATELIER_SHADER_CACHE",
                                    (std::filesystem::absolute(argv[1]).parent_path() / L"shader-cache").c_str());
        const auto root = std::filesystem::absolute(argv[2]) / unique_id();
        std::filesystem::create_directories(root);
        if (argc == 4 && std::wstring_view(argv[3]) == L"--camera-projection") {
            camera_projection(argv[1], root);
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[3]) == L"--client-size") {
            client_size(argv[1], root);
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[3]) == L"--ddgi-debug") {
            ddgi_debug(argv[1], root);
            return 0;
        }
        if (argc == 4 && std::wstring_view(argv[3]) == L"--repro-lighting") {
            repro_lighting(argv[1], root);
            return 0;
        }
        if (argc == 4 && !descriptions) {
            repro_paths(argv[1], root / "exports", false);
            repro_paths(argv[1], root / "deep-control", true);
            reject_nested_destination(argv[1], root);
            repro_examples(argv[1], root / "examples");
            repro_gltf_defaults(argv[1], root / "gltf");
            write(root / "result.json",
                  Json{{"status", "passed"},
                       {"cases",
                        {"nested-exports", "deep-control", "nested-destination-rejected", "examples-scene-assets",
                         "gltf-default-materials", "missing-mesh-bound-material"}}}
                      .dump(2));
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
                                               op.at("args").at("required") == Json::array({"id"});
                                    }),
                "Capabilities omitted the stateful operator call contract");
        verify_option_descriptions(caps);
        const auto empty = d.settle();
        const auto builtins = d.ok("editor.inspect", {{"detail", "assets"}});
        require(empty.at("project") == "" && builtins.at("assets") == empty.at("assets") &&
                    builtins.at("next_cursor").is_null(),
                "No-project assets query did not return the builtin catalog");
        if (!descriptions) {
            no_project_scene_material(d);
            repro_engine_assets(d, root);
            repro_material_inputs(d, root / "MaterialInputs");
        }
        const auto opened = d.ok("op.call", {{"id", "project.open"}, {"args", {{"path", utf8(project.wstring())}}}});
        require(opened.at("status") == "finished" && opened.at("selection").empty(),
                "Project open did not complete its replacement before returning the operator report");
        d.settle();
        const auto before_missing = d.inspect();
        error(d.try_op("scene.open", Json::object()), "invalid_request");
        const auto after_missing = d.inspect();
        require(after_missing.at("project") == before_missing.at("project") &&
                    after_missing.at("scene_path") == before_missing.at("scene_path") &&
                    after_missing.at("undo") == before_missing.at("undo") &&
                    after_missing.at("redo") == before_missing.at("redo"),
                "A noninteractive missing scene path replaced or edited the document");
        verify_asset_directory(d, editor.window(), project, caps);
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
        verify_property_descriptions(current.at("entities")[0]);
        const auto directory_cursor = verify_object_directory(d, current, caps);
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
            require(discover(directory).empty(), "Description editor descriptor remained live after close.");
            require(read(project / "Content/Scenes/Main.scene") == original_scene &&
                        sha256_file(project / "Content/Scenes/Main.scene") == original_scene_hash,
                    "Description inspection changed the original saved scene.");
            write(root / "result.json",
                  Json{{"status", "passed"},
                       {"cases",
                        {"discovery", "shared-option-descriptions", "utf8-newlines", "aggregate-field-descriptions",
                         "bounded-object-directory", "single-object-inspection", "stale-cursor-and-object",
                         "undo-to-clean", "placeholder-inspection", "saved-scene-preserved", "clean-close"}}}
                      .dump(2));
            std::cout << "Editor Control descriptions E2E: PASS | " << utf8(root.wstring()) << '\n';
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
        verify_operators(d, entity);
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
        auto invalid_camera = camera;
        invalid_camera["distance"] = 0.0000000001;
        error(d.try_op("view.camera_set", {{"state", invalid_camera}}), "invalid_request");
        // The sky is a World setting; scene.add rejects it as an undeclared kind.
        const auto retired_sky = d.try_op("scene.add", {{"kind", "sky"}});
        require(retired_sky.value("status", "") == "error" && retired_sky.at("error").at("code") == "invalid_request" &&
                    retired_sky.at("error").at("message").get<std::string>().find("kind: Unknown scene enum member") !=
                        std::string::npos,
                "scene.add did not reject the retired sky kind during strict decoding: " + retired_sky.dump());
        for (const auto* name : {"sky_enabled", "sky_intensity", "sky_color_x", "sky_color_y", "sky_color_z"})
            require(caps.at("display_options").at(name).at("scope") == "scene",
                    "A sky option is absent from the shared Control schema or not a scene option");
        d.op("view.set", {{"sky_enabled", true},
                          {"sky_intensity", 2},
                          {"sky_color_x", .25},
                          {"sky_color_y", .5},
                          {"sky_color_z", .75}});
        current = d.inspect();
        require(current.at("view").at("sky_enabled") == true && current.at("view").at("sky_intensity") == 2 &&
                    current.at("view").at("sky_color_x") == .25 && current.at("view").at("sky_color_y") == .5 &&
                    current.at("view").at("sky_color_z") == .75 && current.at("entities").size() == 2 &&
                    current.at("entities")[1].at("components")[0].at("type") == "world.EnvironmentComponent",
                "The sky settings did not create the Environment");
        d.op("edit.undo");
        current = d.inspect();
        require(current.at("view").at("sky_enabled") == false && current.at("entities").size() == 1,
                "Undoing the first sky edit did not remove the Environment");
        for (const auto* removed : {"ssil", "shadow_bias"})
            error(d.try_op("view.set", {{removed, true}}), "invalid_request");
        d.op("view.set", {{"gtao", false}});
        current = d.inspect();
        require(current.at("view").at("gtao") == false && current.at("entities").size() == 2,
                "A World setting did not create the Environment");
        error(d.try_op("view.set", {{"shadows", "wrong"}}), "invalid_request");
        error(d.try_op("scene.rename", {{"name", "No"}}, selection("stale/999")), "stale_object");
        error(d.try_op("project.open", {{"path", utf8(project.wstring())}}), "unsaved_changes");
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
        const std::filesystem::path repro = wide(exported.at("result").at("directory").get<std::string>());
        const auto manifest = read(repro / "manifest.json");
        const auto frozen = read(repro / "Project" / wide(manifest.at("scene").get<std::string>()));
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
                  {{{"name", "baseline"}, {"patch", Json::object()}},
                   {{"name", "scene_lit"},
                    {"patch", {{"shading", "scene_lit"}, {"shadows", false}, {"ddgi", false}, {"gtao", false}}}}})},
             {"sample_frames", {1, 3}}},
            compare_id);
        require(pending.at("status") == "pending", pending.dump());
        auto compared = d.wait(compare_id);
        require(compared.at("status") == "ok", compared.dump());
        const auto& samples = compared.at("result").at("samples");
        require(samples.size() == 4, "Comparison sample count is incorrect");
        require(samples[0].at("accepted_ordinal") == 1 && samples[1].at("accepted_ordinal") == 3 &&
                    samples[2].at("accepted_ordinal") == 1,
                "Accepted frame ordinals are incorrect");
        require(samples[2].at("effective_view").at("shading") == "scene_lit" &&
                    samples[2].at("environment") == Json::array({0, 0, 0}),
                "Scene Lit settings or explicit zero-sky contract changed");
        for (const auto& sample : samples) {
            const std::filesystem::path output = wide(compared.at("result").at("directory").get<std::string>());
            const auto width = sample.at("extent")[0].get<std::uint64_t>();
            const auto height = sample.at("extent")[1].get<std::uint64_t>();
            require(std::filesystem::file_size(output / wide(sample.at("depth").get<std::string>())) ==
                        width * height * 4,
                    "Depth readback size is incorrect");
            require(std::filesystem::file_size(output / wide(sample.at("color").get<std::string>())) > 100,
                    "PNG readback is missing");
            std::ifstream depth_file(
                atelier::platform::windows_file_path(output / wide(sample.at("depth").get<std::string>())),
                std::ios::binary);
            std::vector<float> depth(static_cast<std::size_t>(width * height));
            depth_file.read(reinterpret_cast<char*>(depth.data()),
                            static_cast<std::streamsize>(depth.size() * sizeof(float)));
            require(bool(depth_file) &&
                        std::ranges::all_of(
                            depth, [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }) &&
                        std::ranges::any_of(depth, [](float value) { return value > 0; }),
                    "Depth capture does not contain valid scene geometry");
        }
        current = d.inspect();
        require(current.at("view") == base_view && current.at("undo") == dirty_undo,
                "Temporary comparison leaked into authoring state");
        auto cancelled_id = unique_id();
        const Json cancelled_request{{"op", "render.capture"},
                                     {"state_token", d.token},
                                     {"request_id", cancelled_id},
                                     {"args", {{"sample_frames", {4096}}}}};
        pending = d.client->call(cancelled_request);
        require(pending.at("status") == "pending", pending.dump());
        const auto unchanged = d.token;
        error(d.try_op("scene.transform_set", {{"position", {{"x", 4}, {"y", 2}, {"z", 3}}}, {"rotation", {{"w", 0}}}},
                       selection(entity)),
              "invalid_request");
        const auto transform_patch = d.try_op("scene.set_properties", {{"rotation", {{"w", 0}}}}, selection(entity));
        error(transform_patch, "invalid_request");
        require(transform_patch.at("error").at("message").get<std::string>().contains("scene.transform_set"),
                "Scalar Transform rejection did not direct the caller to the dedicated operator");
        error(d.try_op("view.set", {{"ssil", true}}), "invalid_request");
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
        const Json retired_request{{"op", "render.capture"},
                                   {"state_token", d.token},
                                   {"request_id", unique_id()},
                                   {"args", {{"sample_frames", {4096}}}}};
        pending = d.client->call(retired_request);
        require(pending.at("status") == "pending", pending.dump());
        error(d.call("job.cancel", {{"job_id", retired_request.at("request_id")}}), "cancelled");
        for (unsigned i = 0; i < 17; ++i) {
            const auto id = unique_id();
            require(d.call("render.capture", {{"sample_frames", {4096}}}, id).at("status") == "pending",
                    "Retention fixture did not start");
            error(d.call("job.cancel", {{"job_id", id}}), "cancelled");
        }
        error(d.call("job.status", {{"job_id", retired_request.at("request_id")}}), "unknown_job");
        error(d.client->call(retired_request), "cancelled");
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
        d.client = std::make_unique<Client>(descriptor);
        error(d.wait(disconnected_id), "cancelled");
        d.inspect();
        d.op("scene.save");
        const auto saved = read(project / "Content/Scenes/Main.scene");
        require(saved.at("entities").size() == 2, "Explicit scene.save failed");
        const auto before_failed_open = d.inspect();
        write(repro / "Project/Source/Injected.source", "invalid added source");
        error(d.call("repro.open", {{"directory", utf8(repro.wstring())}}), "dependency_mismatch");
        std::filesystem::remove(repro / "Project/Source/Injected.source");
        // Hash-valid input can still fail compilation. Strict open must leave the
        // current document, object identities and project in place.
        write(repro / "Project/Source/Published.source", "invalid published source");
        auto broken = manifest;
        for (auto& file : broken.at("files"))
            if (file.at("path") == "Project/Source/Published.source") {
                file["sha256"] = sha256_file(repro / "Project/Source/Published.source");
                file["bytes"] = std::filesystem::file_size(repro / "Project/Source/Published.source");
            }
        write(repro / "manifest.json", broken.dump());
        error(d.call("repro.open", {{"directory", utf8(repro.wstring())}}), "operation_failed");
        current = d.inspect();
        require(current.at("project") == before_failed_open.at("project") &&
                    current.at("entities") == before_failed_open.at("entities") &&
                    current.at("selection") == before_failed_open.at("selection"),
                "Failed repro open replaced the current document");
        write(repro / "Project/Source/Published.source", published_code);
        write(repro / "manifest.json", manifest.dump());
        d.ok("repro.open", {{"directory", utf8(repro.wstring())}});
        current = d.inspect();
        require(current.at("entities").size() == 2 && current.at("entities")[0].at("name") == "Control cube" &&
                    !current.at("dirty").get<bool>(),
                "Repro reopen failed");
        error(d.try_op("scene.rename", {{"name", "Old handle"}}, selection(entity)), "stale_object");
        repro_lighting_agreement(d, repro, root);
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
                                wide(descriptor.at("instance_id").get<std::string>()) + L" --request " +
                                quote(root / "capture-request.json"),
                            root / "cli-capture.json");
        require(WaitForSingleObject(cli_capture.process, 30000) == WAIT_OBJECT_0, "CLI capture did not finish");
        const auto cli_result = read(root / "cli-capture.json");
        require(cli_result.at("status") == "ok" && cli_result.at("result").at("samples").size() == 1,
                "CLI did not await its capture job");
        // Every editor step of the run, Control captures included, kept the timing hierarchy.
        d.client = std::make_unique<Client>(descriptor);
        const auto cpu = d.inspect().at("performance").at("cpu");
        require(cpu.at("structure_errors") == 0 && cpu.at("first_structure_error").is_null(),
                "An editor step recorded a frame timing phase under the wrong parent: " + cpu.dump());
        editor.close();
        require(discover(directory).empty(), "Closed editor descriptor remained live");
        reject_nested_destination(argv[1], root);
        std::ofstream report(root / "result.json");
        report << Json{{"status", "passed"},
                       {"cases",
                        {"discovery",
                         "capabilities",
                         "idempotency",
                         "request-retention",
                         "job-retention",
                         "stale-token",
                         "create",
                         "select",
                         "property",
                         "transform",
                         "undo-redo",
                         "camera",
                         "display-policy",
                         "repro-dirty-scene",
                         "published-source-set",
                         "published-ui",
                         "repro-file-set",
                         "failed-open-preserves-document",
                         "nested-output",
                         "repro-reopen",
                         "current-lighting-repro",
                         "rejected-repro-preserves-camera-selection",
                         "sky-world-setting",
                         "multi-point-capture",
                         "cancel",
                         "invalid-edit-preserves-job",
                         "context-change",
                         "timeout",
                         "disconnect",
                         "clean-close"}},
                       {"repro", utf8(repro.wstring())},
                       {"comparison", compared.at("result").at("directory")}}
                      .dump(2);
        std::cout << "Editor Control E2E: PASS | " << utf8(root.wstring()) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Editor Control E2E: FAIL: " << error.what() << '\n';
        return 1;
    }
}
