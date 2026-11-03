#include "editor/project_code.hpp"
#include "assets/file.hpp"
#include "platform/windows_path.hpp"
#include "procedural/graph.hpp"
#include "ui_language/component.hpp"
#include "logging/logging.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>

namespace atelier::editor {
namespace {
auto fail(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}

bool same(const source::SourceSnapshot& a, const source::SourceSnapshot& b) {
    if (a.files.size() != b.files.size())
        return false;
    for (std::size_t i = 0; i < a.files.size(); ++i)
        if (a.files[i].path != b.files[i].path || a.files[i].source != b.files[i].source)
            return false;
    return true;
}

Result<void> completed(Result<std::optional<Scalar>> result) {
    if (!result)
        return std::unexpected(result.error());
    return {};
}

bool lifecycle(std::string_view entry, std::string_view type) {
    if (entry.size() <= type.size() + 1 || !entry.starts_with(type) || entry[type.size()] != '.')
        return false;
    const auto name = entry.substr(type.size() + 1);
    return name == "begin" || name == "tick" || name == "post_physics_tick" || name == "late_tick" || name == "end";
}

Result<void> validate_ui(const source::SourceFile& file) {
    // Compilation only. Project expressions and event handlers are evaluated
    // exclusively by the isolated runner, including UI previews.
    Runtime runtime;
    ATELIER_TRY_BIND(registered, ui_language::register_types(runtime));
    auto module = ui_language::Module::compile(runtime, file.path, file.source);
    const auto reject = [&](std::string message) -> Result<void> {
        SourceLocation location{file.path, file.version, 0, 1, 1, file.disk_path};
        const auto prefix = file.path + ":";
        if (message.starts_with(prefix)) {
            auto text = std::string_view(message).substr(prefix.size());
            const auto row = std::from_chars(text.data(), text.data() + text.size(), location.line);
            if (row.ec == std::errc{} && row.ptr != text.data() + text.size() && *row.ptr == ':')
                static_cast<void>(std::from_chars(row.ptr + 1, text.data() + text.size(), location.column));
        }
        return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message), std::move(location)});
    };
    if (!module)
        return reject(module.error().message);
    const auto entry = (*module)->find("ProjectUI");
    if (!entry)
        return reject("Project UI requires ui ProjectUI(in Text title, in double elapsed, in double entities).");
    const auto& inputs = entry->declaration().inputs;
    if (inputs.size() != 3 || inputs[0].name != "title" || inputs[0].kind != ui_language::InputKind::text ||
        inputs[1].name != "elapsed" || inputs[1].kind != ui_language::InputKind::number ||
        inputs[2].name != "entities" || inputs[2].kind != ui_language::InputKind::number)
        return reject("ProjectUI inputs must be title (Text), elapsed (double), entities (double).");
    return {};
}
}

ProjectCode::ProjectCode(world::World& world) : world_(world), report_(std::make_shared<ProjectBuild>()) {}

Result<source::SourceSnapshot> ProjectCode::snapshot(const Project& project) {
    ATELIER_TRY_BIND(paths, project.source_paths());
    ATELIER_TRY_BIND(engine, source::engine_sources(source::default_engine_root()));
    source::SourceSnapshot result;
    result.name = assets::utf8(project.name());
    result.version = 14695981039346656037ULL;
    std::size_t bytes{};
    const auto add = [&](source::SourceFile file) {
        bytes += file.source.size();
        if (bytes > 16 * 1024 * 1024)
            return false;
        result.version = source::content_hash(file.path, source::content_hash(file.source, result.version));
        result.files.push_back(std::move(file));
        return true;
    };
    for (const auto& relative : *paths) {
        ATELIER_TRY_BIND(path, project.resolve(relative));
        ATELIER_TRY_BIND(file, source::read_source_file(*path, assets::utf8(relative.generic_wstring())));
        if (!add(std::move(*file)))
            return fail("Project and engine Source exceed the 16 MiB compilation limit.");
    }
    for (auto& file : *engine)
        if (!add(std::move(file)))
            return fail("Project and engine Source exceed the 16 MiB compilation limit.");
    return result;
}

Result<std::optional<source::SourceFile>> ProjectCode::ui_snapshot(const Project& project) {
    if (project.ui_source().empty())
        return std::optional<source::SourceFile>{};
    ATELIER_TRY_BIND(path, project.resolve(project.ui_source()));
    ATELIER_TRY_BIND(file, source::read_source_file(*path, assets::utf8(project.ui_source().generic_wstring())));
    return std::optional<source::SourceFile>{std::move(*file)};
}

Result<void> ProjectCode::build(const Project& project) {
    auto next = std::make_shared<ProjectBuild>();
    next->published_version = report_->published_version;
    std::uint64_t request_sequence{};
    std::string_view phase = "snapshot";
    const auto record = [&](std::string_view code, const Error* error = nullptr) noexcept {
        try {
            logging::Entry entry{.severity = error ? logging::Severity::error : logging::Severity::info,
                                 .category = "source",
                                 .event_code = std::string(code),
                                 .message = error ? error->message : "Project Source build transition."};
            entry.fields = {{"project_root", assets::utf8(project.root().generic_wstring())},
                            {"phase", std::string(phase)},
                            {"snapshot_version", next->sources ? next->sources->version : 0},
                            {"published_content_version", next->published_version}};
            if (request_sequence)
                entry.fields.push_back({"request_sequence", request_sequence});
            if (code == "source.published")
                entry.fields.push_back({"code_version", request_sequence});
            if (error) {
                entry.fields.push_back({"error_code", static_cast<std::uint64_t>(error->code)});
                if (error->source) {
                    const auto& location = *error->source;
                    entry.source = {location.path,
                                    {},
                                    {},
                                    location.line <= UINT32_MAX ? static_cast<std::uint32_t>(location.line) : 0,
                                    location.column <= UINT32_MAX ? static_cast<std::uint32_t>(location.column) : 0,
                                    location.version,
                                    location.offset,
                                    location.disk_path};
                    entry.fields.insert(entry.fields.end(), {{"error.path", location.path},
                                                             {"error.version", location.version},
                                                             {"error.offset", std::uint64_t{location.offset}},
                                                             {"error.line", std::uint64_t{location.line}},
                                                             {"error.column", std::uint64_t{location.column}},
                                                             {"error.disk_path", location.disk_path}});
                }
            }
            static_cast<void>(logging::submit(std::move(entry)));
        } catch (...) {
            std::fputs("Source build diagnostic capture failed.\n", stderr);
        }
    };
    const auto reject = [&](Error error) -> Result<void> {
        next->state = error.code == ErrorCode::rebuild_required ? BuildState::rebuild_required : BuildState::failed;
        next->error = std::move(error);
        record("source.build_rejected", &*next->error);
        report_ = next;
        return std::unexpected(*next->error);
    };
    auto input = snapshot(project);
    if (!input)
        return reject(input.error());
    next->sources = std::make_shared<source::SourceSnapshot>(std::move(*input));
    auto ui_source = ui_snapshot(project);
    if (!ui_source)
        return reject(ui_source.error());
    next->ui_source = std::move(*ui_source);
    const bool ui_same = (!report_->ui_source && !next->ui_source) ||
                         (report_->ui_source && next->ui_source && report_->ui_source->path == next->ui_source->path &&
                          report_->ui_source->source == next->ui_source->source);
    if (report_->sources && same(*next->sources, *report_->sources) && ui_same) {
        if (report_->error)
            return std::unexpected(*report_->error);
        return {};
    }
    record("source.build_requested");
    if (next->ui_source && (!ui_same || report_->error)) {
        phase = "ui_validation";
        auto checked = validate_ui(*next->ui_source);
        if (!checked)
            return reject(checked.error());
    }
    if (!published_ || !same(*published_->sources, *next->sources)) {
        while (world_.runtime().collection_phase() != CollectionPhase::idle) {
            auto step = world_.runtime().collect_step({1024});
            if (!step)
                return reject(step.error());
        }
        if (!program_) {
            // The engine Source in every snapshot declares procedural nodes.
            if (auto registered = procedural::register_types(world_.runtime()); !registered)
                return reject(registered.error());
            program_ = std::make_unique<source::Program>(world_.runtime());
        }
        // Request through publish runs synchronously here, and this is the Runtime's only
        // Program, so no other producer can invalidate the candidate; an asynchronous
        // build would need to re-request once when publication reports a stale catalog.
        phase = "request";
        auto request = program_->request(*next->sources);
        if (!request)
            return reject(request.error());
        request_sequence = (*request)->sequence;
        record("source.requested");
        phase = "prepare";
        auto candidate = source::Program::prepare(*request);
        if (!candidate)
            return reject(candidate.error());
        record("source.prepared");
        phase = "validate";
        auto valid = validate(*candidate);
        if (!valid)
            return reject(valid.error());
        const bool rebind = !bound_ || adds_behavior(*candidate);
        phase = "publish";
        auto published = program_->try_publish(*candidate);
        if (!published)
            return reject(published.error());
        record("source.published");
        phase = "bind";
        if (rebind) {
            auto binding = bind();
            if (!binding) {
                bound_ = false;
                return reject(binding.error());
            }
            bound_ = true;
        }
        // Published code may fix isolated objects; they resume without a second Begin.
        if (world_.running())
            if (auto cleared = world_.clear_faults(); !cleared)
                return reject(cleared.error());
    }
    next->published_version = next->ui_source ? source::content_hash(next->ui_source->source, next->sources->version)
                                              : next->sources->version;
    phase = "ready";
    record("source.build_ready");
    published_ = next;
    report_ = std::move(next);
    return {};
}

// Compatible publications may add classes or lifecycle methods; those need a
// new binding. build runs between frames, where World accepts it while running:
// begun objects take the new methods from the next phase without a second Begin.
bool ProjectCode::adds_behavior(const source::Program::Candidate& candidate) const {
    auto& runtime = world_.runtime();
    for (const auto& type : candidate.classes()) {
        const auto entity = runtime.is_a(type.base, world_.types().entity);
        const auto component = runtime.is_a(type.base, world_.types().component);
        if (!entity || !component || (!*entity && !*component))
            continue;
        for (const auto& entry : candidate.entries())
            if (lifecycle(entry.name, type.name) && !(bound_ && program_->entry(entry.name)))
                return true;
    }
    return false;
}

Result<void> ProjectCode::validate(const source::Program::Candidate& candidate) const {
    auto& runtime = world_.runtime();
    for (const auto& type : candidate.classes()) {
        const auto entity = runtime.is_a(type.base, world_.types().entity);
        const auto component = runtime.is_a(type.base, world_.types().component);
        if (!entity || !component)
            return fail("Cannot resolve a project class's published native base.");
    }
    return {};
}

Result<void> ProjectCode::bind() {
    auto& runtime = world_.runtime();
    for (const auto& type : program_->classes()) {
        const auto entity = runtime.is_a(type.type, world_.types().entity);
        const auto component = runtime.is_a(type.type, world_.types().component);
        if (!entity || !component)
            return fail("Cannot resolve a project behavior's base type.");
        if (!*entity && !*component)
            continue;
        world::World::Behavior behavior;
        for (const auto& method : program_->entries()) {
            const auto prefix = type.name + ".";
            if (!method.name.starts_with(prefix))
                continue;
            const auto name = std::string_view(method.name).substr(prefix.size());
            auto* tick = name == "tick"                ? &behavior.tick
                         : name == "post_physics_tick" ? &behavior.post_physics_tick
                         : name == "late_tick"         ? &behavior.late_tick
                                                       : nullptr;
            if (!tick && name != "begin" && name != "end")
                continue;
            ATELIER_TRY_BIND(entry, program_->entry(method.name));
            if (tick)
                *tick = [entry = *entry](Runtime&, ObjectId self, float dt) {
                    const std::array<Scalar, 1> args{dt};
                    return completed(entry.invoke(self, args));
                };
            else {
                auto call = [entry = *entry](Runtime&, ObjectId self) {
                    return completed(entry.invoke(self, std::span<const Scalar>{}));
                };
                if (name == "begin")
                    behavior.begin = std::move(call);
                else
                    behavior.end = std::move(call);
            }
        }
        if (behavior.begin || behavior.tick || behavior.post_physics_tick || behavior.late_tick || behavior.end)
            ATELIER_TRY(world_.bind_behavior(type.type, std::move(behavior)));
    }
    return {};
}

Result<void> ProjectCode::poll(const Project& project, bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < next_poll_)
        return {};
    next_poll_ = now + std::chrono::milliseconds(500);
    auto paths = project.source_paths();
    if (!paths)
        return build(project);
    if (!project.ui_source().empty())
        paths->push_back(project.ui_source());
    const auto engine_root = source::default_engine_root();
    auto engine = source::engine_source_paths(engine_root);
    if (!engine)
        return build(project);
    std::vector<Stamp> stamps;
    stamps.reserve(paths->size() + engine->size());
    try {
        for (const auto& relative : *paths) {
            const auto path = project.resolve(relative);
            if (!path)
                return build(project);
            const auto native = platform::windows_file_path(*path);
            stamps.push_back({relative, std::filesystem::last_write_time(native), std::filesystem::file_size(native)});
        }
        for (const auto& relative : *engine) {
            const auto native = platform::windows_file_path(engine_root / relative);
            stamps.push_back({native, std::filesystem::last_write_time(native), std::filesystem::file_size(native)});
        }
    } catch (const std::exception&) {
        return build(project);
    }
    if (!force && stamps == stamps_)
        return {};
    stamps_ = std::move(stamps);
    return build(project);
}

ProjectDocument load_project_scene(Project& project, std::string_view source, LoadPolicy policy,
                                   bool allow_failed_build, SessionRole role) {
    ProjectDocument result;
    auto document = Session::deserialize(
        source, project,
        [&](world::World& world) -> Result<std::shared_ptr<void>> {
            auto code = std::make_shared<ProjectCode>(world);
            const auto built = code->build(project);
            result.build = code->report();
            if (!built) {
                if (!allow_failed_build)
                    return std::unexpected(built.error());
                // Placeholders keep a different type identity than the project types they stand in
                // for, so a successful build replaces this document through a fresh load.
                return std::shared_ptr<void>{};
            }
            result.code = code.get();
            return std::static_pointer_cast<void>(std::move(code));
        },
        policy, role);
    if (document) {
        result.session = std::move(*document);
        if (result.build && result.build->error)
            result.error = *result.build->error;
    } else {
        result.error = result.build && result.build->error ? *result.build->error : document.error();
        result.code = nullptr;
    }
    return result;
}

std::wstring describe_diagnostic(const Session& session, const world::Diagnostic& item) {
    auto line = L"Diagnostic [" + assets::wide(world::name(item.severity)) + L"] " +
                assets::wide(world::name(item.kind)) + L" in " + assets::wide(world::name(item.phase));
    if (item.entity != ObjectId{}) {
        const auto label = session.name(item.entity);
        line += label.empty() ? std::wstring(L" Entity") : L" \"" + std::wstring(label) + L"\"";
    }
    if (!item.type.empty())
        line += L" (" + assets::wide(item.type) + L")";
    for (std::size_t i = 0; i < item.field.size(); ++i)
        line += (i ? L"." : L" ") + assets::wide(item.field[i]);
    line += L": " + assets::wide(item.message);
    if (!item.value.empty())
        line += L" Value: " + assets::wide(item.value);
    if (item.source)
        line += L" at " + assets::wide(item.source->path) + L":" + std::to_wstring(item.source->line) + L":" +
                std::to_wstring(item.source->column);
    if (item.repeats > 1)
        line += L" (×" + std::to_wstring(item.repeats) + L")";
    return line;
}

std::string ProjectCode::component_source(std::string_view name) {
    return "class project." + std::string(name) +
           " : world.Component {\n    float speed = 1.0f;\n\n    void tick(float dt) {\n        self.translate_owner(self.speed * dt, 0.0f, 0.0f);\n    }\n}\n";
}

std::string ProjectCode::ui_source() {
    return "ui ProjectUI(in Text title, in double elapsed, in double entities) {\n    Column hud {\n        padding_left: 8; padding_top: 8; gap: 4;\n        Text title { text: input.title; }\n        Text hint { text: \"Edit this UI in your project.\"; }\n    }\n}\n";
}
}
