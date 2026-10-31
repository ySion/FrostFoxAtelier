#include "application/editor_control.hpp"
#include "application/application.hpp"
#include "application/control_artifacts.hpp"
#include "editor/view_options.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "platform/windows_path.hpp"
#include "atelier/build_info.hpp"
#include <Windows.h>
#include <deque>
#include <future>
#include <limits>
#include <set>
#include <unordered_set>

namespace atelier::application {
namespace {
using control::Json;
using control::Failure;
// Bumped whenever the frozen camera/view contract changes; repro.open accepts only this schema.
constexpr unsigned repro_view_schema = 6;
constexpr std::array<std::string_view, 3> removed_view_options{"direct_reference", "direct_clustered",
                                                               "direct_shadow_cache"};
constexpr unsigned inspect_page_items = 256;
constexpr std::size_t inspect_page_bytes = control::maximum_message / 4;
template <class T>
T take(Result<T> value) {
    if (!value)
        throw Failure("operation_failed", value.error().message);
    return std::move(*value);
}
void take(Result<void> value) {
    if (!value)
        throw Failure("operation_failed", value.error().message);
}
void committed(Result<world::Commit> value) {
    auto commit = take(std::move(value));
    if (commit.notification_error)
        throw Failure("operation_failed", commit.notification_error->message);
}
std::string text(const Json& value, std::size_t maximum = 4096) {
    if (!value.is_string())
        throw Failure("invalid_request", "Expected a string");
    auto result = value.get<std::string>();
    if (result.size() > maximum || result.find('\0') != result.npos)
        throw Failure("invalid_request", "Invalid string length or NUL");
    static_cast<void>(control::wide(result));
    return result;
}
double number(const Json& value) {
    if (!value.is_number())
        throw Failure("invalid_request", "Expected a number");
    const auto n = value.get<double>();
    if (!std::isfinite(n))
        throw Failure("invalid_request", "Expected a finite number");
    return n;
}
unsigned integer(const Json& value, unsigned low, unsigned high) {
    const auto n = number(value);
    if (n < low || n > high || std::floor(n) != n)
        throw Failure("invalid_request", "Integer outside supported range");
    return static_cast<unsigned>(n);
}
math::Vector3 vector3(const Json& value) {
    if (!value.is_array() || value.size() != 3)
        throw Failure("invalid_request", "Expected three vector components");
    const auto f = [](const Json& n) {
        const auto v = number(n);
        if (std::abs(v) > 100000000)
            throw Failure("invalid_request", "Vector exceeds authoring range");
        return static_cast<float>(v);
    };
    return {f(value[0]), f(value[1]), f(value[2])};
}
Json vector3(math::Vector3 v) {
    return assets::json_format::numbers(std::array{v.x, v.y, v.z});
}
Json camera_json(const editor::CameraState& c) {
    using assets::json_format::number;
    return {{"view", static_cast<unsigned>(c.view)},
            {"target", vector3(c.target)},
            {"yaw", number(c.yaw)},
            {"pitch", number(c.pitch)},
            {"distance", number(c.distance)},
            {"roll", number(c.roll)},
            {"vertical_fov", number(c.vertical_fov)},
            {"near_clip", number(c.near_clip)},
            {"far_clip", number(c.far_clip)}};
}
editor::CameraState camera_state(const Json& args) {
    control::fields(args,
                    {"view", "target", "yaw", "pitch", "distance", "roll", "vertical_fov", "near_clip", "far_clip"});
    return {
        static_cast<editor::CameraView>(integer(args.at("view"), 0, static_cast<unsigned>(editor::CameraView::left))),
        vector3(args.at("target")),
        static_cast<float>(number(args.at("yaw"))),
        static_cast<float>(number(args.at("pitch"))),
        static_cast<float>(number(args.at("distance"))),
        static_cast<float>(number(args.at("roll"))),
        static_cast<float>(number(args.at("vertical_fov"))),
        static_cast<float>(number(args.at("near_clip"))),
        static_cast<float>(number(args.at("far_clip")))};
}
Json view_json(const renderer::ViewSettings& view) {
    Json result = Json::object();
    for (const auto& option : editor::view_options()) {
        const auto value = editor::view_value(view, option.option);
        if (option.kind == editor::ViewValueKind::enumeration)
            result[option.name] = option.choices[static_cast<std::size_t>(value)];
        else if (option.kind == editor::ViewValueKind::boolean)
            result[option.name] = value != 0;
        else if (option.kind == editor::ViewValueKind::number)
            result[option.name] = assets::json_format::number(static_cast<float>(value));
        else
            result[option.name] = value;
    }
    return result;
}
Json probe_volumes_json(const std::array<renderer::ProbeVolumeStats, renderer::probe_volume_limit>& volumes) {
    auto result = Json::array();
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const auto& volume = volumes[i];
        result.push_back({{"slot", i},
                          {"enabled", volume.enabled},
                          {"rolling", volume.rolling},
                          {"minimum", volume.minimum},
                          {"maximum", volume.maximum},
                          {"spacing", volume.spacing},
                          {"counts", volume.counts},
                          {"resident", volume.resident},
                          {"updated", volume.updated},
                          {"reset_probes", volume.reset_probes}});
    }
    return result;
}
Json probe_inspection_json(const std::shared_ptr<const renderer::ProbeInspection>& value) {
    if (!value)
        return nullptr;
    const auto& v = *value;
    const auto& r = v.receiver;
    constexpr std::array modes{"live", "hold_irradiance", "hold_distance"};
    const auto selection = v.requested_probe >= 0 ? (v.traced_probe == ~0u ? "explicit_out_of_range" : "explicit")
                                                  : (v.traced_probe == ~0u ? "automatic_pending" : "automatic");
    Json result{{"sequence", v.sequence},
                {"frame_id", v.frame_id},
                {"scene_identity", v.scene_identity},
                {"pixel", v.pixel},
                {"history", modes[static_cast<unsigned>(v.history)]},
                {"rays", v.rays},
                {"updates", v.updates},
                {"volumes", probe_volumes_json(v.volumes)},
                {"receiver",
                 {{"status", r.identity[0]},
                  {"volume", r.identity[1]},
                  {"contributors", r.identity[2]},
                  {"irradiance_coverage", r.irradiance},
                  {"weight_sum", r.weights[0]},
                  {"dominance", r.weights[1]},
                  {"position_valid", r.position}}},
                {"probes", Json::array()},
                {"requested_probe", v.requested_probe},
                {"probe_selection", selection},
                {"traced_probe", v.traced_probe},
                {"ray_samples", Json::array()}};
    for (const auto& p : v.probes)
        if (p.identity[2])
            result["probes"].push_back({{"volume", p.identity[0]},
                                        {"probe", p.identity[1]},
                                        {"support_stage", p.identity[2]},
                                        {"age_frames", p.identity[3]},
                                        {"irradiance_observations", p.irradiance},
                                        {"spatial_facing_visibility_weight", p.weights},
                                        {"position_distance", p.position},
                                        {"transport_observations", p.transport},
                                        {"response_epoch_min_max_visits", p.response}});
    for (const auto& ray : v.ray_samples)
        if (ray.identity[3])
            result["ray_samples"].push_back({{"identity", ray.identity},
                                             {"direct_luminance", ray.direct},
                                             {"surface_roughness", ray.surface},
                                             {"sampling", ray.sampling},
                                             {"light_endpoint_distance", ray.endpoint_distance},
                                             {"source_radius_range_area_min_distance_squared", ray.source_parameters}});
    return result;
}
Json performance_json(const editor::FramePerformance& performance) {
    const auto& sample = performance.snapshot();
    constexpr std::array states{"unavailable", "waiting", "ready"};
    constexpr std::array aa_names{"off", "taa", "smaa", "smaa_t2x"};
    Json result{{"state", states[static_cast<unsigned>(sample.state)]},
                {"window_capacity", editor::FramePerformance::window_size},
                {"publish_interval_ms", 250},
                {"samples", sample.samples},
                {"sequence", sample.sequence},
                {"frame_id", sample.frame_id},
                {"scene_identity", sample.scene_identity},
                {"domain_first_frame", sample.first_frame},
                {"extent", {sample.extent.width, sample.extent.height}},
                {"viewport", {sample.viewport.x, sample.viewport.y, sample.viewport.width, sample.viewport.height}},
                {"configuration",
                 {{"antialiasing", aa_names[static_cast<unsigned>(sample.antialiasing)]},
                  {"direct_candidates", sample.direct_candidates},
                  {"ddgi", sample.ddgi}}},
                {"logical_resource_bytes", sample.resource_bytes ? Json(*sample.resource_bytes) : Json(nullptr)},
                {"resource_semantics",
                 "Current logical Scene Lit allocation; excludes retired resources and physical residency"},
                {"total_semantics",
                 "Sum of eight measured main intervals; excludes UI, guides, probe overlays, capture and CPU Present"},
                {"passes", Json::array()}};
    // One row of either section; values without an observation are null.
    const auto statistics = [](const std::optional<editor::FramePerformance::Timing>& timing) {
        const bool active = timing && timing->active_samples;
        return Json{{"state", !timing                  ? "unavailable"
                              : timing->current_active ? "measured"
                                                       : "skipped"},
                    {"active_samples", timing ? timing->active_samples : 0},
                    {"current_ms", timing && timing->current_active ? Json(timing->current) : Json(nullptr)},
                    {"mean_ms", active ? Json(timing->mean) : Json(nullptr)},
                    {"p95_ms", active ? Json(timing->p95) : Json(nullptr)},
                    {"max_ms", active ? Json(timing->max) : Json(nullptr)},
                    {"share_percent", active && timing->share ? Json(*timing->share) : Json(nullptr)}};
    };
    const auto definitions = editor::FramePerformance::passes();
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto& pass = definitions[i];
        auto row = statistics(sample.timings[i]);
        row["id"] = pass.key;
        row["title"] = pass.title;
        row["parent"] = pass.parent < 0 ? Json(nullptr) : Json(definitions[pass.parent].key);
        row["depth"] = pass.depth;
        row["included_in_total"] = pass.in_total;
        result["passes"].push_back(std::move(row));
    }
    const auto& cpu = sample.cpu;
    Json phases = Json::array();
    for (const auto* marker : timing::catalog()) {
        auto phase = statistics(cpu.phases[marker->index()]);
        phase["id"] = std::string(marker->key());
        phase["title"] = std::string(marker->title());
        phase["parent"] = marker->parent() ? Json(std::string(marker->parent()->key())) : Json(nullptr);
        phase["depth"] = timing::depth(*marker);
        phase["current_entries"] = cpu.current_entries[marker->index()];
        phases.push_back(std::move(phase));
    }
    auto unattributed = statistics(cpu.unattributed);
    unattributed["id"] = "frame.unattributed";
    unattributed["title"] = "Unattributed";
    unattributed["parent"] = "frame";
    unattributed["depth"] = 1;
    unattributed["current_entries"] = nullptr;
    phases.push_back(std::move(unattributed));
    const auto* first = performance.first_structure_error();
    result["cpu"] = {
        {"state", cpu.samples ? "ready" : "waiting"},
        {"window_capacity", editor::FramePerformance::window_size},
        {"samples", cpu.samples},
        {"sequence", cpu.sequence},
        {"frame_id", cpu.frame_id},
        {"frame_interval_ms",
         cpu.interval ? Json{{"mean", cpu.interval->mean}, {"p95", cpu.interval->p95}, {"max", cpu.interval->max}}
                      : Json(nullptr)},
        {"frames_per_second", cpu.interval ? Json(1000 / cpu.interval->mean) : Json(nullptr)},
        {"structure_errors", performance.structure_errors()},
        {"first_structure_error", first ? Json(std::string(first->key())) : Json(nullptr)},
        {"semantics",
         "CPU wall time in milliseconds of accepted editor steps without capture, read from frame timing records. "
         "Phases are inclusive and sum every entry in the step; frame.unattributed is the frame minus its direct children. "
         "frame_interval_ms is between the begins of consecutive sampled steps. structure_errors counts every step since start, sampled or not."},
        {"phases", std::move(phases)}};
    return result;
}
renderer::ViewSettings patched_view(renderer::ViewSettings view, const Json& patch, std::array<float, 3> focus) {
    if (!patch.is_object())
        throw Failure("invalid_request", "Display patch must be an object");
    std::set<editor::ViewOption> assigned;
    for (const auto& [name, value] : patch.items()) {
        if (std::ranges::find(removed_view_options, name) != removed_view_options.end())
            throw Failure("invalid_request",
                          "Display option was removed: " + name +
                              "; direct light always uses clustered light lists with cached shadows");
        const auto* option = editor::view_option(name);
        if (!option)
            throw Failure("invalid_request", "Unknown display option: " + name);
        if (!assigned.insert(option->option).second)
            throw Failure("invalid_request", "Display option specified through multiple names: " + name);
        double scalar{};
        if (option->kind == editor::ViewValueKind::enumeration) {
            const auto choice = text(value);
            const auto found = std::ranges::find(option->choices, choice);
            if (found == option->choices.end())
                throw Failure("invalid_request", "Unsupported display choice: " + choice);
            scalar = static_cast<double>(found - option->choices.begin());
        } else if (option->kind == editor::ViewValueKind::boolean) {
            if (!value.is_boolean())
                throw Failure("invalid_request", "Display toggle requires a boolean");
            scalar = value.get<bool>() ? 1 : 0;
        } else
            scalar = number(value);
        take(editor::set_view_option(view, option->option, scalar));
    }
    take(editor::validate_view_options(view, focus));
    return view;
}
Json scalar_json(const Scalar& scalar, const Runtime& runtime) {
    return std::visit(
        [&](const auto& value) -> Json {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, EnumValue>) {
                const auto* type = take(runtime.describe(value.type));
                if (type->enumeration)
                    for (const auto& [name, bits] : type->enumeration->values)
                        if (bits == value.bits)
                            return name;
                return value.bits;
            } else
                return value;
        },
        scalar);
}
Scalar scalar_value(const Json& json, const Scalar& current, const Runtime& runtime) {
    return std::visit(
        [&](const auto& value) -> Scalar {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, EnumValue>) {
                const auto name = text(json);
                const auto* type = take(runtime.describe(value.type));
                if (type->enumeration)
                    for (const auto& [choice, bits] : type->enumeration->values)
                        if (name == choice)
                            return EnumValue{value.type, bits};
                throw Failure("invalid_request", "Unknown enum choice");
            } else if constexpr (std::is_same_v<T, bool>) {
                if (!json.is_boolean())
                    throw Failure("invalid_request", "Expected boolean property");
                return json.get<bool>();
            } else {
                const auto n = number(json);
                if (n < std::numeric_limits<T>::lowest() || n > std::numeric_limits<T>::max() ||
                    (std::is_integral_v<T> && std::floor(n) != n))
                    throw Failure("invalid_request", "Property outside scalar range");
                return static_cast<T>(n);
            }
        },
        current);
}
struct Operation {
    std::string_view name, description;
    bool changes;
    Json properties;
    std::vector<std::string> required;
};
const std::vector<Operation>& operations() {
    static const Json str{{"type", "string"}}, obj{{"type", "object"}}, arr{{"type", "array"}};
    static const std::vector<Operation> values{
        {"capabilities", "Discover operations, arguments, limits and display options", false, {}, {}},
        {"op.list",
         "List editor operators: identity, flags and parameter schema; the context override schema",
         false,
         {},
         {}},
        {"op.call",
         "Call one editor operator; context replaces selection or region for this call only",
         true,
         {{"id", str}, {"args", obj}, {"context", obj}},
         {"id"}},
        {"editor.inspect",
         "Read editor state, bounded asset/object directories, or one discovered object",
         false,
         {{"detail", {{"enum", {"full", "summary", "assets", "objects", "object"}}}},
          {"object", str},
          {"entity_name", str},
          {"type", str},
          {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", inspect_page_items}}},
          {"cursor", obj}},
         {}},
        {"project.open",
         "Open an existing project; refuses unsaved scene or source drafts",
         true,
         {{"path", str}},
         {"path"}},
        {"scene.open", "Open a project-relative scene; refuses unsaved scene", true, {{"path", str}}, {"path"}},
        {"scene.save", "Save the current scene through the normal project save command", true, {{"path", str}}, {}},
        {"scene.create",
         "Create one entity, mesh, point/rectangle/directional light; the sky is a World setting (view.set)",
         true,
         {{"kind", {{"enum", {"entity", "mesh", "point", "rectangle", "directional"}}}}, {"mesh", str}},
         {"kind"}},
        {"scene.rename",
         "Rename one entity through document history; components use their type names",
         true,
         {{"object", str}, {"name", str}},
         {"object", "name"}},
        {"scene.set_property",
         "Write one reflected property with its exact scalar/enum type",
         true,
         {{"object", str}, {"property", str}, {"value", Json::object()}},
         {"object", "property", "value"}},
        {"material.create",
         "Create a named Material Source (.ms.material) or Material Instance under Content",
         true,
         {{"path", str},
          {"role", {{"enum", {"source", "instance"}}}},
          {"source", str},
          {"parent", str},
          {"entries", arr}},
         {"path", "role"}},
        {"texture.import", "Import a texture with a Project GUID", true, {{"path", str}}, {"path"}},
        {"texture.reimport",
         "Replace an existing texture's content on the same identity without author history",
         true,
         {{"asset", str}, {"path", str}},
         {"asset", "path"}},
        {"mesh.import",
         "Import an OBJ or glTF mesh with a Project GUID; glTF also publishes one default material per slot",
         true,
         {{"path", str}},
         {"path"}},
        {"mesh.reimport",
         "Replace a mesh on the same identity and publish exact geometry to Editor and Live",
         true,
         {{"asset", str}, {"path", str}},
         {"asset", "path"}},
        {"material.inspect",
         "Read owning MS declarations, sparse MI entries, effective values and diagnostics",
         false,
         {{"asset", str}, {"object", str}},
         {}},
        {"material.set_entry",
         "Edit one typed sparse entry {path,type,enabled,value}; one author history entry",
         true,
         {{"object", str}, {"entry", obj}},
         {"object", "entry"}},
        {"material.set_texture",
         "Assign a texture asset to an MI declaration path; empty key explicitly clears it",
         true,
         {{"object", str}, {"path", str}, {"texture", str}},
         {"object", "path", "texture"}},
        {"material.assign",
         "Assign a shared material to a component slot; empty asset restores its mesh default",
         true,
         {{"component", str}, {"slot", str}, {"asset", str}},
         {"component", "slot", "asset"}},
        {"material.create_scene",
         "Create a scene MI inheriting the current slot binding or mesh default",
         true,
         {{"component", str}, {"slot", str}},
         {"component", "slot"}},
        {"material.override",
         "Enable or disable an MI entry without deleting its local value",
         true,
         {{"object", str}, {"path", str}, {"enabled", {{"type", "boolean"}}}},
         {"object", "path", "enabled"}},
        {"material.discard",
         "Remove one sparse MI entry and inherit again",
         true,
         {{"object", str}, {"path", str}},
         {"object", "path"}},
        {"material.save", "Save a shared material asset", true, {{"asset", str}}, {"asset"}},
        {"material.undo", "Undo one shared material asset edit", true, {{"asset", str}}, {"asset"}},
        {"material.redo", "Redo one shared material asset edit", true, {{"asset", str}}, {"asset"}},
        {"scene.transform",
         "Set or add complete Entity TRS; one undo entry",
         true,
         {{"object", str}, {"position", arr}, {"rotation", arr}, {"scale", arr}},
         {"object", "position", "rotation", "scale"}},
        {"scene.remove_transform",
         "Remove optional Entity space through document history",
         true,
         {{"object", str}},
         {"object"}},
        {"scene.set_space_reference",
         "Set Entity spatial origin independently of composition; null inherits, empty string is world space",
         true,
         {{"object", str}, {"origin", {{"type", {"string", "null"}}}}, {"keep_world", {{"type", "boolean"}}}},
         {"object", "origin"}},
        {"scene.reparent",
         "Reparent Entity composition with explicit transform preservation",
         true,
         {{"object", str},
          {"parent", {{"type", {"string", "null"}}}},
          {"keep_world", {{"type", "boolean"}}},
          {"index", {{"type", "integer"}, {"minimum", 0}}}},
         {"object"}},
        {"scene.add_component",
         "Add one discovered component type to the selected entity",
         true,
         {{"type", str}},
         {"type"}},
        {"scene.remove_component", "Remove a component through document history", true, {{"object", str}}, {"object"}},
        {"scene.delete", "Delete selected entities through document history", true, {}, {}},
        {"camera.set", "Restore an exact camera state from editor.inspect", true, {{"state", obj}}, {"state"}},
        {"camera.look_at",
         "Aim a perspective camera at a world-space target (metres, +Y up)",
         true,
         {{"eye", arr}, {"target", arr}},
         {"eye", "target"}},
        {"view.set",
         "Atomically apply a typed display patch using the same rules as menus",
         true,
         {{"patch", obj}},
         {"patch"}},
        {"view.layout",
         "Apply an explicit DDGI coverage layout; retain propagation and display settings",
         true,
         {{"layout", {{"enum", {"fit_scene", "scene_focus"}}}}},
         {"layout"}},
        {"repro.export",
         "Export an independent project from the current memory scene; does not save the original",
         true,
         {},
         {}},
        {"repro.open",
         "Open a completed repro bundle and restore its camera, display and entity selection",
         true,
         {{"directory", str}},
         {"directory"}},
        {"render.prepare",
         "Prepare Scene Lit pipelines through a real accepted frame; restores base display settings",
         true,
         {{"patch", obj}, {"timeout_ms", {{"type", "integer"}}}},
         {}},
        {"render.capture",
         "Capture at accepted-frame ordinals using current display settings",
         true,
         {{"sample_frames", arr}, {"timeout_ms", {{"type", "integer"}}}},
         {}},
        {"render.compare",
         "Capture sequential temporary display cases; settings never enter document/undo",
         true,
         {{"cases", arr}, {"sample_frames", arr}, {"timeout_ms", {{"type", "integer"}}}},
         {"cases"}},
        {"job.status",
         "Read a bounded asynchronous job; polling does not mutate editor state",
         false,
         {{"job_id", str}},
         {"job_id"}},
        {"job.cancel",
         "Cancel a job idempotently; temporary display is removed at this boundary",
         false,
         {{"job_id", str}},
         {"job_id"}}};
    return values;
}
Json capabilities() {
    auto result = Json{{"operations", Json::array()},
                       {"display_options", Json::object()},
                       {"limits",
                        {{"message_bytes", control::maximum_message},
                         {"inspect_page_items", inspect_page_items},
                         {"inspect_page_bytes", inspect_page_bytes},
                         {"request_history", 256},
                         {"request_history_bytes", 8 * 1024 * 1024},
                         {"job_history", 16},
                         {"captures", 16},
                         {"capture_bytes", 256 * 1024 * 1024},
                         {"timeout_ms", control::maximum_timeout_ms}}}};
    for (const auto& op : operations())
        result["operations"].push_back({{"op", op.name},
                                        {"description", op.description},
                                        {"requires_state_token", op.changes},
                                        {"args",
                                         {{"type", "object"},
                                          {"properties", op.properties.is_null() ? Json::object() : op.properties},
                                          {"required", op.required},
                                          {"additionalProperties", false}}}});
    for (const auto& option : editor::view_options()) {
        Json schema;
        if (option.kind == editor::ViewValueKind::enumeration) {
            schema = {{"type", "string"}, {"enum", Json::array()}};
            for (auto choice : option.choices)
                schema["enum"].push_back(choice);
        } else if (option.kind == editor::ViewValueKind::boolean)
            schema = {{"type", "boolean"}};
        else
            schema = {{"type", option.kind == editor::ViewValueKind::integer ? "integer" : "number"},
                      {"minimum", option.minimum},
                      {"maximum", option.maximum}};
        if (option.option == editor::ViewOption::grid_plane)
            schema["availability"] = "render.compare only; persistent value follows camera";
        schema["scope"] = editor::world_option(option.option) ? "scene" : "view";
        schema["label"] = option.label;
        schema["category"] = option.category;
        schema["description"] = option.description;
        result["display_options"][option.name] = std::move(schema);
    }
    result["display_aliases"] = Json::object();
    for (const auto& alias : editor::view_option_aliases())
        result["display_aliases"][alias.name] = editor::view_option(alias.name)->name;
    result["probe_volume_stats_semantics"] =
        "CPU accepted-frame layout and update schedule; not GPU completion or valid-support counts";
    result["coordinates"] =
        "metres, left-handed +Y up, row-major matrices, row vectors; TRS rotation is quaternion xyzw";
    result["history_policy"] =
        "Renderer histories evolve naturally across cases; no probe reset or GPU-history restoration is promised";
    return result;
}
}

struct EditorControl::Impl {
    Application& app;
    control::Server server;
    std::uint64_t version{1}, document_generation{1}, input_revision{}, next_handle{1}, edit_id{1ull << 62};
    struct Observation {
        std::uint64_t document{}, session{}, input{}, project{}, code{}, view_revision{};
        ui::PixelExtent extent;
        float dpi{};
        editor::CameraState camera;
        renderer::ViewSettings view;
        bool editing{}, running{};
        bool operator==(const Observation&) const = default;
    } observed;
    std::unordered_map<ObjectId, std::string> handles;
    std::unordered_map<std::string, ObjectId> objects;
    std::uint64_t pruned_revision{};
    struct Cached {
        std::string id, request;
        Json response;
        std::size_t bytes{};
    };
    std::deque<Cached> cache;
    std::size_t cache_bytes{};
    struct Case {
        std::string name;
        renderer::ViewSettings view;
    };
    struct Job {
        std::string id, token;
        std::shared_ptr<control::Peer> peer;
        Observation context;
        std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
        std::chrono::steady_clock::time_point deadline, started;
        std::vector<Case> cases;
        std::vector<unsigned> points;
        std::size_t case_index{}, point_index{};
        unsigned accepted{}, skipped{};
        ui::Rect bounds;
        std::filesystem::path directory;
        std::vector<control_artifacts::Sample> samples;
        Json manifest, response;
        std::future<Json> writer;
        bool rendering{}, preparing{}, complete{}, recorded{};
        std::uint64_t history_resets{};
    };
    std::deque<std::unique_ptr<Job>> jobs;
    struct AcceptedFrame {
        std::uint64_t id{}, count{};
        ui::Rect bounds;
        ui::PixelExtent extent;
        renderer::Camera camera;
        renderer::ViewSettings view;
        std::array<float, 3> environment, lighting_focus;
        renderer::FrameStats stats;
    };
    std::optional<AcceptedFrame> last_frame;
    Json frame_json() const {
        if (!last_frame)
            return Json::object();
        const auto& f = *last_frame;
        const auto& s = f.stats;
        return {{"frame_id", f.id},
                {"accepted_count", f.count},
                {"viewport", {f.bounds.x, f.bounds.y, f.bounds.width, f.bounds.height}},
                {"camera",
                 {{"view", f.camera.view},
                  {"projection", f.camera.projection},
                  {"far_depth", assets::json_format::number(f.camera.far_depth)},
                  {"projection_convention", "editor_unjittered"}}},
                {"effective_view", view_json(f.view)},
                {"environment", f.environment},
                {"lighting_focus", f.lighting_focus},
                {"extent", {f.extent.width, f.extent.height}},
                {"frame_stats",
                 {{"scene_draw_calls", s.scene_draw_calls},
                  {"scene_instances", s.scene_instances},
                  {"scene_culled_instances", s.scene_culled_instances},
                  {"history_resets", s.scene_lit_history_resets},
                  {"light_sources", s.scene_lit_light_sources},
                  {"shadow_sources", s.scene_lit_shadow_sources},
                  {"spatial_light_sampling",
                   {{"retained_sources", s.scene_lit_spatial_sources},
                    {"nonempty_cells", s.scene_lit_spatial_cells},
                    {"resource_bytes", s.scene_lit_spatial_bytes},
                    {"uploaded", s.scene_lit_spatial_changed}}},
                  {"probe_volumes", probe_volumes_json(s.scene_lit_probe_volumes)},
                  {"probe_volumes_source", "cpu_layout_and_schedule"},
                  {"probe_inspection", probe_inspection_json(s.scene_lit_probe_inspection)}}},
                {"gpu_sample",
                 {{"valid", s.scene_lit_gpu_timing_valid},
                  {"frame_id", s.scene_lit_gpu_sample_frame_id},
                  {"sequence", s.scene_lit_gpu_sample_sequence},
                  {"pass_ms", s.scene_lit_gpu_ms},
                  {"ddgi_pass_ms", s.scene_lit_ddgi_gpu_ms},
                  {"ddgi_update_pass_ms", s.scene_lit_ddgi_update_gpu_ms},
                  {"direct_cluster_ms", s.scene_lit_direct_cluster_gpu_ms},
                  {"direct_shading_ms", s.scene_lit_direct_shading_gpu_ms},
                  {"shadow_cache_ms", s.scene_lit_shadow_cache_gpu_ms},
                  {"shadow_cache",
                   {{"faces", s.scene_lit_shadow_cache_faces},
                    {"reused_faces", s.scene_lit_shadow_cache_reused_faces},
                    {"static_updates", s.scene_lit_shadow_cache_static_updates},
                    {"dynamic_updates", s.scene_lit_shadow_cache_dynamic_updates},
                    {"draws", s.scene_lit_shadow_cache_draws},
                    {"resolution_downgrades", s.scene_lit_shadow_cache_resolution_downgrades},
                    {"dropped_lights", s.scene_lit_shadow_cache_dropped_lights},
                    {"near_plane_faces", s.scene_lit_shadow_cache_near_plane_faces},
                    {"missing_taps", s.scene_lit_shadow_cache_missing_taps},
                    {"plane_fallbacks", s.scene_lit_shadow_cache_plane_fallbacks},
                    {"deferred_faces", s.scene_lit_shadow_cache_deferred_faces},
                    {"deferral_frames", s.scene_lit_shadow_cache_deferral_frames},
                    {"atlas_cells", s.scene_lit_shadow_cache_atlas_cells},
                    {"pending_lights", s.scene_lit_shadow_cache_pending_lights}}},
                  {"direct_cluster",
                   {{"cells", s.scene_lit_direct_cluster_cells},
                    {"overflows", s.scene_lit_direct_cluster_overflows},
                    {"max_sources", s.scene_lit_direct_cluster_max_sources},
                    {"receiver_histogram", s.scene_lit_direct_receiver_histogram},
                    {"receiver_max_sources", s.scene_lit_direct_receiver_max_sources}}},
                  {"pass_mask", s.scene_lit_gpu_pass_mask},
                  {"ddgi_visibility_rays", s.scene_lit_ddgi_visibility_rays},
                  {"resource_bytes", s.scene_lit_resource_bytes},
                  {"resource_bytes_source", "current_cpu_logical"},
                  {"shadow_rays", s.scene_lit_shadow_rays},
                  {"probe_rays", s.scene_lit_probe_rays}}}};
    }
    Impl(Application& owner, const std::filesystem::path& directory)
        : app(owner),
          server(directory, {{"revision", build_info::revision}, {"configuration", build_info::configuration}}) {
        std::filesystem::create_directories(platform::windows_file_path(
            server.directory() / "artifacts" / server.identity().at("instance_id").get<std::string>()));
        observe();
    }
    ~Impl() {
        server.stop();
        for (auto& job : jobs) {
            job->cancel->store(true);
            if (job->writer.valid())
                job->writer.wait();
        }
    }
    void observe() {
        Observation current{
            document_generation,
            app.document_->revision(),
            input_revision,
            app.project_ ? app.project_->revision() : 0,
            app.project_code_ && app.project_code_->report() ? app.project_code_->report()->published_version : 0,
            app.workspace_.view_revision(),
            app.surface_.extent,
            app.surface_.dpi_scale,
            app.camera_.state(),
            app.workspace_.view,
            app.document_->editing(),
            app.play_.running()};
        if (current != observed) {
            observed = current;
            ++version;
        }
    }
    std::string token() const {
        return server.identity().at("instance_id").get<std::string>() + ":" + std::to_string(version);
    }
    auto busy_states() const {
        const auto ui = app.ui_.interaction_state();
        using State = std::pair<std::string_view, bool>;
        return std::array{State{"document_edit", app.document_->editing()},
                          State{"viewport_tool", app.viewport_tool_.active()},
                          State{"camera_orbit", app.camera_drag_ && !app.camera_drag_->pan},
                          State{"camera_pan", app.camera_drag_ && app.camera_drag_->pan},
                          State{"camera_fly", app.camera_fly_.has_value()},
                          State{"view_edit", app.workspace_.lighting.editing()},
                          State{"menu", app.workspace_.menu.has_value()},
                          State{"ui_pointer", ui.pointer},
                          State{"ui_key", ui.keyboard},
                          State{"text_draft", ui.text_draft},
                          State{"ime", ui.composition},
                          State{"clipboard", ui.clipboard},
                          State{"ui_popup", ui.popup}};
    }
    bool busy() const {
        return std::ranges::any_of(busy_states(), [](const auto& state) { return state.second; });
    }
    Json busy_reasons() const {
        auto reasons = Json::array();
        for (const auto& [name, active] : busy_states())
            if (active)
                reasons.push_back(name);
        return reasons;
    }
    void idle() const {
        if (busy())
            throw Failure("busy", "Finish the current text, menu or viewport interaction first");
    }
    void replaceable() const {
        idle();
        if (app.document_->dirty())
            throw Failure("unsaved_changes", "Save the current scene before replacing it");
        if (!app.document_->dirty_materials().empty())
            throw Failure("unsaved_changes", "Save the open material assets before replacing the document");
        for (const auto& file : app.workspace_.source_editor.files())
            if (file->dirty())
                throw Failure("unsaved_changes", "Source drafts are still open");
        if (app.play_.running())
            throw Failure("busy", "Stop the project before replacing its document");
    }
    Job* active() {
        for (auto& job : jobs)
            if (!job->complete)
                return job.get();
        return nullptr;
    }
    void cancel(Job& job, std::string_view code, std::string_view message) {
        if (job.complete || job.cancel->exchange(true))
            return;
        job.rendering = false;
        job.preparing = false;
        job.response = control::failure(code, message);
        job.response["job_id"] = job.id;
        job.samples.clear();
        if (!job.writer.valid())
            job.complete = true;
    }
    void trim_cache() {
        while (!cache.empty() && (cache.size() > 256 || cache_bytes > 8 * 1024 * 1024)) {
            cache_bytes -= cache.front().bytes;
            cache.pop_front();
        }
    }
    void record_completions() {
        for (auto& job : jobs)
            if (job->complete && !job->recorded) {
                job->recorded = true;
                job->response["state_token"] = token();
                job->response["job_id"] = job->id;
                for (auto& entry : cache)
                    if (entry.id == job->id) {
                        cache_bytes -= entry.bytes;
                        entry.response = job->response;
                        entry.bytes = entry.request.size() + entry.response.dump().size();
                        cache_bytes += entry.bytes;
                        break;
                    }
            }
        trim_cache();
    }
    void checkpoint() {
        observe();
        if (auto* job = active()) {
            if (!job->peer->connected.load())
                cancel(*job, "cancelled", "Control connection ended");
            else if (std::chrono::steady_clock::now() >= job->deadline)
                cancel(*job, "timeout", "Control job deadline exceeded");
            else if (job->rendering && (job->token != token() || busy())) {
                auto changes = Json::array();
                const auto& before = job->context;
                if (before.document != observed.document)
                    changes.push_back("document");
                if (before.session != observed.session)
                    changes.push_back("scene_or_selection");
                if (before.input != observed.input)
                    changes.push_back("user_input");
                if (before.extent != observed.extent || before.dpi != observed.dpi)
                    changes.push_back("surface");
                if (before.project != observed.project)
                    changes.push_back("project_files");
                if (before.code != observed.code)
                    changes.push_back("published_code");
                if (before.camera != observed.camera)
                    changes.push_back("camera");
                if (before.view != observed.view || before.view_revision != observed.view_revision)
                    changes.push_back("display_preferences");
                if (before.running != observed.running)
                    changes.push_back("play_state");
                if (busy())
                    changes.push_back("active_interaction");
                cancel(*job, "state_changed", "Editor changed during the render experiment");
                job->response["error"]["changes"] = std::move(changes);
                job->response["error"]["previous_extent"] = {before.extent.width, before.extent.height};
                job->response["error"]["current_extent"] = {observed.extent.width, observed.extent.height};
            }
            if (job->writer.valid() && job->writer.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    auto result = job->writer.get();
                    if (!job->cancel->load())
                        job->response = {{"status", "ok"}, {"result", std::move(result)}};
                } catch (const Failure& error) {
                    if (!job->cancel->load())
                        job->response = control::failure(error.code, error.what());
                } catch (const std::exception& error) {
                    if (!job->cancel->load())
                        job->response = control::failure("storage_error", error.what());
                }
                job->response["job_id"] = job->id;
                job->complete = true;
            }
        }
        record_completions();
    }
    std::string handle(ObjectId object) {
        if (object == ObjectId{})
            return {};
        if (handles.size() > editor::Session::maximum_entities + editor::Session::maximum_components &&
            pruned_revision != app.document_->revision())
            prune_handles();
        auto [found, added] = handles.try_emplace(object);
        if (added) {
            found->second = std::to_string(document_generation) + "/" + std::to_string(next_handle++);
            objects.emplace(found->second, object);
        }
        return found->second;
    }
    void prune_handles() {
        pruned_revision = app.document_->revision();
        std::unordered_set<ObjectId> live;
        for (const auto& entity : take(app.document_->world().members())) {
            live.insert(entity.id());
            for (const auto& component : take(world::components(app.document_->world().runtime(), entity.id())))
                live.insert(component.id());
        }
        for (const auto id : app.document_->material_objects())
            live.insert(id);
        const auto objects_snapshot = live;
        for (const auto id : objects_snapshot)
            if (auto bindings = presentation::read_material_bindings(app.document_->world().runtime(), id); bindings)
                for (const auto& binding : *bindings)
                    live.insert(binding.resource.id());
        std::erase_if(handles, [&](const auto& entry) {
            if (live.contains(entry.first))
                return false;
            objects.erase(entry.second);
            return true;
        });
    }
    bool member(ObjectId object) const {
        const auto materials = app.document_->material_objects();
        if (std::ranges::find(materials, object) != materials.end())
            return true;
        auto entities = app.document_->world().members();
        if (!entities)
            return false;
        for (const auto& entity : *entities) {
            if (entity.id() == object)
                return true;
            auto components = world::components(app.document_->world().runtime(), entity.id());
            if (components)
                for (const auto& component : *components) {
                    if (component.id() == object)
                        return true;
                    if (auto bindings =
                            presentation::read_material_bindings(app.document_->world().runtime(), component.id());
                        bindings)
                        for (const auto& binding : *bindings)
                            if (binding.resource.id() == object)
                                return true;
                }
        }
        return false;
    }
    ObjectId object(const Json& value) {
        const auto name = text(value, 80);
        const auto found = objects.find(name);
        if (found == objects.end() || !member(found->second))
            throw Failure("stale_object", "Object handle is no longer a document member");
        return found->second;
    }
    Json object_header(ObjectId id, std::string_view type) {
        Json header{{"handle", handle(id)}, {"name", control::utf8(app.document_->name(id))}, {"type", type}};
        // A placeholder stands in for a saved type the current build does not register.
        if (app.document_->has_retained(id))
            if (const auto saved = app.document_->retained(id); saved && !saved->type.empty()) {
                header["placeholder"] = true;
                header["saved_type"] = saved->type;
            }
        return header;
    }
    Json object_json(ObjectId id) {
        auto& runtime = app.document_->world().runtime();
        const auto* type = take(runtime.describe(take(runtime.type_of(id))));
        auto result = object_header(id, type->name);
        result["type_description"] = type->description;
        result["properties"] = Json::array();
        if (app.document_->is_material(id)) {
            const auto view = take(app.document_->inspect_material(id));
            result["asset"] = view.asset_key;
            result["parent"] = view.parent_key;
            result["role"] = view.source ? "source" : "instance";
            result["scene"] = view.scene;
            result["default_material"] = view.default_material;
            result["dirty"] = view.scene ? app.document_->dirty() : app.document_->material_dirty(view.asset_key);
            result["entries"] = assets::material_format::encode_entries(view.entries);
            result["declarations"] = Json::array();
            result["diagnostics"] = Json::array();
            for (const auto& declaration : view.declarations) {
                const std::array values{material::Override{declaration.path, declaration.value, true},
                                        material::Override{declaration.path, declaration.initial, true}};
                auto field = assets::material_format::encode_entries(std::span(values).first(1)).front();
                field.erase("enabled");
                field["initial"] =
                    assets::material_format::encode_entries(std::span(values).last(1)).front().at("value");
                field["components"] = declaration.components;
                result["declarations"].push_back(std::move(field));
            }
            for (const auto& diagnostic : view.diagnostics)
                result["diagnostics"].push_back({{"path", diagnostic.path}, {"message", diagnostic.message}});
            if (view.source_file)
                result["source"] = {{"path", view.source_file->path},
                                    {"version", view.source_file->version},
                                    {"text", view.source_file->source}};
            return result;
        }
        for (const auto& property : take(app.document_->properties(id))) {
            const auto* prop_type = take(runtime.describe(property.type));
            Json field{{"name", property.name},         {"label", property.label},
                       {"category", property.category}, {"description", property.description},
                       {"type", prop_type->name},       {"value", scalar_json(property.value, runtime)}};
            if (prop_type->enumeration) {
                field["choices"] = Json::array();
                for (const auto& [name, bits] : prop_type->enumeration->values) {
                    static_cast<void>(bits);
                    field["choices"].push_back(name);
                }
            }
            if (property.range != FieldRange{}) {
                auto& range = field["range"] = Json::object();
                const auto bound = [&](const char* name, const std::optional<Scalar>& value) {
                    if (value)
                        range[name] = scalar_json(*value, runtime);
                };
                bound("ui_min", property.range.ui_min);
                bound("ui_max", property.range.ui_max);
                bound("hard_min", property.range.hard_min);
                bound("hard_max", property.range.hard_max);
            }
            result["properties"].push_back(std::move(field));
        }
        if (app.document_->has_retained(id))
            if (const auto saved = app.document_->retained(id); saved && !saved->entries.empty()) {
                auto& data = result["saved_data"] = Json::array();
                for (const auto& entry : saved->entries) {
                    std::string path;
                    for (const auto& part : entry.path)
                        path += (path.empty() ? "" : ".") + part;
                    data.push_back(
                        {{"path", path}, {"value", entry.value}, {"kind", std::string(world::name(entry.kind))}});
                }
            }
        if (take(runtime.is_a(take(runtime.type_of(id)), app.document_->types().entity))) {
            const auto origin = take(world::space_reference(runtime, id));
            result["space_reference"] = origin ? Json(*origin == ObjectId{} ? "" : handle(*origin)) : Json(nullptr);
        }
        if (auto transform = world::read_transform(runtime, id))
            result["transform"] = {
                {"position", vector3(transform->position)},
                {"rotation",
                 {transform->rotation.x, transform->rotation.y, transform->rotation.z, transform->rotation.w}},
                {"scale", vector3(transform->scale)}};
        if (auto bindings = presentation::read_material_bindings(runtime, id); bindings) {
            if (const auto mesh = presentation::read_static_mesh(runtime, id)) {
                auto defaults = Json::array();
                for (const auto& binding : mesh->mesh.defaults) {
                    const auto source = take(material::material_key(runtime, binding.resource.id()));
                    defaults.push_back(
                        {{"slot", binding.slot},
                         {"object", handle(binding.resource.id())},
                         {"source", source},
                         {"missing", app.document_->unavailable(assets::AssetKind::material, source) != nullptr}});
                }
                result["mesh"] = {{"source", mesh->mesh.source},
                                  {"missing", !mesh->mesh.resolved && !mesh->mesh.source.empty()},
                                  {"default_materials", std::move(defaults)}};
            }
            result["materials"] = Json::array();
            for (const auto& binding : *bindings) {
                const auto view = take(app.document_->inspect_material(binding.resource.id()));
                result["materials"].push_back({{"slot", binding.slot},
                                               {"object", handle(binding.resource.id())},
                                               {"asset", view.asset_key},
                                               {"parent", view.parent_key},
                                               {"scene", view.scene},
                                               {"assignment_override", binding.overridden},
                                               {"default_material", view.default_material}});
            }
        }
        return result;
    }
    // Full and paged inspection share the existing builtin-then-project entry contract.
    Json asset_json(std::size_t index) const {
        const auto builtins = assets::mesh_assets();
        if (index < builtins.size()) {
            const auto& asset = builtins[index];
            return {{"key", asset.key}, {"name", control::utf8(asset.name)}, {"kind", "mesh"}};
        }
        const auto& entry = app.project_->entries()[index - builtins.size()];
        return {{"key", entry.key},
                {"path", control::utf8(entry.path.generic_wstring())},
                {"kind", static_cast<unsigned>(entry.kind)}};
    }
    Json inspect_assets(const Json& args) {
        control::fields(args, {"detail", "limit", "cursor"});
        const auto limit = args.contains("limit") ? integer(args.at("limit"), 1, inspect_page_items) : 64u;
        const auto revision = app.project_ ? app.project_->revision() : 0;
        const auto instance = server.identity().at("instance_id");
        const auto count = assets::mesh_assets().size() + (app.project_ ? app.project_->entries().size() : 0);
        std::size_t index{};
        if (args.contains("cursor")) {
            const auto& cursor = args.at("cursor");
            control::fields(cursor, {"domain", "instance", "document", "project_revision", "index"});
            if (cursor.at("domain") != "assets")
                throw Failure("invalid_request", "Expected an assets cursor");
            for (const auto* field : {"document", "project_revision", "index"})
                if (!cursor.at(field).is_number_integer() || cursor.at(field) < 0)
                    throw Failure("invalid_request", "Expected an unsigned cursor integer");
            if (text(cursor.at("instance")) != instance.get_ref<const std::string&>() ||
                cursor.at("document") != document_generation || cursor.at("project_revision") != revision)
                throw Failure("stale_cursor", "The asset directory changed; restart without a cursor");
            const auto position = cursor.at("index").get<std::uint64_t>();
            if (position > count)
                throw Failure("invalid_request", "Asset directory cursor is outside the catalog");
            index = static_cast<std::size_t>(position);
        }
        Json result{{"detail", "assets"},
                    {"document_generation", document_generation},
                    {"project_revision", revision},
                    {"assets", Json::array()},
                    {"next_cursor", nullptr}};
        auto& entries = result["assets"];
        std::size_t bytes{};
        for (; index < count; ++index) {
            if (entries.size() == limit)
                break;
            auto entry = asset_json(index);
            const auto size = entry.dump().size() + 1;
            if (size > inspect_page_bytes)
                throw Failure("result_too_large", "One asset directory entry exceeds the page byte limit");
            if (size > inspect_page_bytes - bytes)
                break;
            bytes += size;
            entries.push_back(std::move(entry));
        }
        if (index < count)
            result["next_cursor"] = {{"domain", "assets"},
                                     {"instance", instance},
                                     {"document", document_generation},
                                     {"project_revision", revision},
                                     {"index", index}};
        return result;
    }
    Json inspect_objects(const Json& args) {
        control::fields(args, {"detail", "entity_name", "type", "limit", "cursor"});
        const auto limit = args.contains("limit") ? integer(args.at("limit"), 1, inspect_page_items) : 64u;
        const auto revision = app.document_->revision();
        const auto instance = server.identity().at("instance_id");
        const Json* cursor = args.contains("cursor") ? &args.at("cursor") : nullptr;
        unsigned entity_index{}, object_index{};
        if (cursor) {
            control::fields(*cursor,
                            {"instance", "document", "revision", "entity", "component", "entity_name", "type"});
            if (cursor->at("instance") != instance || cursor->at("document") != document_generation ||
                cursor->at("revision") != revision)
                throw Failure("stale_cursor", "The object directory changed; restart without a cursor");
            entity_index = integer(cursor->at("entity"), 0, editor::Session::maximum_entities);
            object_index = integer(cursor->at("component"), 0, editor::Session::maximum_components);
        }
        const auto filter = [&](const char* key) -> Json {
            const auto value = cursor ? cursor->at(key) : args.value(key, Json(nullptr));
            if (cursor && args.contains(key) && args.at(key) != value)
                throw Failure("invalid_request", "A pagination cursor cannot change its filters");
            if (!value.is_null())
                static_cast<void>(text(value));
            if (args.contains(key) && args.at(key).is_null())
                throw Failure("invalid_request", "Inspection filters must be strings");
            return value;
        };
        const auto entity_filter = filter("entity_name"), type_filter = filter("type");
        const auto entity_name =
            entity_filter.is_null() ? std::wstring{} : control::wide(entity_filter.get<std::string>());
        const auto entities = take(app.document_->world().members());
        if (entity_index > entities.size() || (entity_index == entities.size() && object_index))
            throw Failure("invalid_request", "Object directory cursor is outside the document");
        if (pruned_revision != revision)
            prune_handles();
        Json result{{"detail", "objects"},
                    {"document_generation", document_generation},
                    {"revision", revision},
                    {"objects", Json::array()},
                    {"next_cursor", nullptr}};
        auto& entries = result["objects"];
        std::size_t bytes{};
        auto& runtime = app.document_->world().runtime();
        for (; entity_index < entities.size(); ++entity_index, object_index = 0) {
            const auto entity = entities[entity_index].id();
            if (!entity_filter.is_null() && app.document_->name(entity) != entity_name)
                continue;
            const auto components = take(world::components(runtime, entity));
            if (object_index > components.size())
                throw Failure("invalid_request", "Object directory cursor is outside the entity");
            for (; object_index <= components.size(); ++object_index) {
                const auto id = object_index ? components[object_index - 1].id() : entity;
                const auto* type = take(runtime.describe(take(runtime.type_of(id))));
                if (!type_filter.is_null() && type->name != type_filter.get_ref<const std::string&>())
                    continue;
                auto entry = object_header(id, type->name);
                entry["kind"] = object_index ? "component" : "entity";
                entry["entity"] = handle(entity);
                if (!object_index)
                    entry["parent"] = handle(take(world::parent_entity(runtime, entity)));
                const auto size = entry.dump().size() + 1;
                if (size > inspect_page_bytes)
                    throw Failure("result_too_large", "One object directory entry exceeds the page byte limit");
                if (entries.size() == limit || size > inspect_page_bytes - bytes) {
                    result["next_cursor"] = {{"instance", instance},      {"document", document_generation},
                                             {"revision", revision},      {"entity", entity_index},
                                             {"component", object_index}, {"entity_name", entity_filter},
                                             {"type", type_filter}};
                    return result;
                }
                bytes += size;
                entries.push_back(std::move(entry));
            }
        }
        return result;
    }
    Json inspect(bool summary = false) {
        Json result{{"identity", server.identity()},
                    {"project", app.project_ ? control::utf8(app.project_->root().generic_wstring()) : ""},
                    {"scene_path", control::utf8(app.scene_path_.generic_wstring())},
                    {"dirty", app.document_->dirty()},
                    {"busy", busy()},
                    {"busy_reasons", busy_reasons()},
                    {"text_focus", app.ui_.wants_keyboard()},
                    {"undo", app.document_->undo_count()},
                    {"redo", app.document_->redo_count()},
                    {"camera", camera_json(app.camera_.state())},
                    {"view", view_json(app.workspace_.view)},
                    {"accepted_frames", app.accepted_frames_},
                    {"last_frame", frame_json()}};
        result["performance"] = performance_json(app.workspace_.lighting.performance());
        if (auto* job = active())
            result["active_job"] = {
                {"job_id", job->id}, {"rendering", job->rendering}, {"cancel_requested", job->cancel->load()}};
        if (summary) {
            result["detail"] = "summary";
            return result;
        }
        for (const char* field : {"entities", "selection", "assets", "component_types"})
            result[field] = Json::array();
        for (const auto& entity : take(app.document_->world().members())) {
            auto value = object_json(entity.id());
            value["components"] = Json::array();
            for (const auto& component : take(world::components(app.document_->world().runtime(), entity.id())))
                value["components"].push_back(object_json(component.id()));
            const auto parent = take(world::parent_entity(app.document_->world().runtime(), entity.id()));
            value["parent"] = parent != ObjectId{} ? handle(parent) : "";
            value["children"] = Json::array();
            for (const auto& child : take(world::children(app.document_->world().runtime(), entity.id())))
                value["children"].push_back(handle(child.id()));
            result["entities"].push_back(std::move(value));
        }
        // Released IDs never retain Runtime objects, and are bounded by current membership.
        prune_handles();
        for (auto entity : app.document_->selected_entities())
            result["selection"].push_back(handle(entity));
        result["inspected_object"] = handle(app.document_->selection().object);
        result["dirty_materials"] = app.document_->dirty_materials();
        for (const auto& type : take(app.document_->component_types()))
            result["component_types"].push_back(type.name);
        const auto count = assets::mesh_assets().size() + (app.project_ ? app.project_->entries().size() : 0);
        for (std::size_t i = 0; i < count; ++i)
            result["assets"].push_back(asset_json(i));
        return result;
    }
    Job& new_job(const std::string& id, const std::shared_ptr<control::Peer>& peer) {
        if (active())
            throw Failure("busy", "Another control job is still active");
        if (std::ranges::any_of(jobs, [&](const auto& job) { return job->id == id; }))
            throw Failure("request_id_expired", "Request payload expired; query the retained job using job.status");
        record_completions();
        while (jobs.size() >= 16)
            jobs.pop_front();
        auto job = std::make_unique<Job>();
        job->id = id;
        job->peer = peer;
        job->token = token();
        job->context = observed;
        job->started = std::chrono::steady_clock::now();
        job->deadline = job->started + std::chrono::seconds(120);
        job->directory = server.directory() / "artifacts" / server.identity().at("instance_id").get<std::string>() /
                         control::unique_id();
        job->manifest = {{"protocol", control::protocol_version},
                         {"identity", server.identity()},
                         {"state_token", token()},
                         {"request_id", id},
                         {"camera", camera_json(app.camera_.state())},
                         {"view_schema", repro_view_schema},
                         {"view", view_json(app.workspace_.view)}};
        jobs.push_back(std::move(job));
        return *jobs.back();
    }
    Json start_export(const std::string& id, const std::shared_ptr<control::Peer>& peer) {
        auto data = control_artifacts::snapshot_resources(*app.document_);
        if (app.project_) {
            if (!app.project_code_ || !app.project_code_->published_build() || !app.workspace_.scene_available)
                throw Failure("not_ready", "Repair the project and load its scene before exporting a repro");
            data.project = app.project_->root();
            const auto& published = *app.project_code_->published_build();
            data.sources = published.sources;
            data.ui_source = published.ui_source;
            data.publication_version = published.published_version;
        }
        data.drafts = Json::array();
        for (const auto& file : app.workspace_.source_editor.files())
            if (file->dirty())
                data.drafts.push_back({{"path", control::utf8(file->path.generic_wstring())},
                                       {"text", control::utf8(file->document->text())},
                                       {"conflict", file->conflict}});
        if (app.project_) {
            data.roots.assign(app.project_->roots().begin(), app.project_->roots().end());
            // snapshot_resources already walked the actual parent/default closure
            // against compiled declarations. Do not turn dormant author keys into
            // dependencies or replace its exact Source snapshots with cache contents.
            const auto& doc = *app.document_;
            const auto compare = [](const std::filesystem::path& a, const std::filesystem::path& b) {
                const auto left = a.generic_wstring(), right = b.generic_wstring();
                return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE);
            };
            const auto less = [&](const auto& a, const auto& b) { return compare(a, b) == CSTR_LESS_THAN; };
            std::set<std::filesystem::path, decltype(less)> listed(less);
            const auto add = [&](const std::filesystem::path& named) {
                // Raw mesh references may use a different case than the catalogue.
                // Copy each Windows file once, preserving keyed companions' identities.
                const auto entries = app.project_->entries();
                const auto at = std::ranges::find_if(
                    entries, [&](const auto& entry) { return compare(named, entry.path) == CSTR_EQUAL; });
                const auto& path = at == entries.end() ? named : at->path;
                if (!listed.insert(path).second)
                    return;
                data.files.push_back(path);
                if (at != entries.end() &&
                    (at->kind == assets::AssetKind::mesh || at->kind == assets::AssetKind::material ||
                     at->kind == assets::AssetKind::texture || at->kind == assets::AssetKind::sky)) {
                    auto sidecar = path;
                    sidecar += L".asset.json";
                    if (!std::filesystem::exists(platform::windows_file_path(take(app.project_->resolve(sidecar)))))
                        throw Failure("dependency_mismatch", "Asset identity file is missing: " + at->key);
                    if (listed.insert(sidecar).second)
                        data.files.push_back(std::move(sidecar));
                }
            };
            for (const auto& [kind, key] : data.references) {
                if (key.empty() || key.starts_with("builtin/"))
                    continue;
                const auto* entry = app.project_->entry(key);
                if (!entry || entry->kind != kind) {
                    if (doc.unavailable(kind, key))
                        continue;
                    throw Failure("dependency_mismatch", "A captured resource is no longer in the project: " + key);
                }
                add(entry->path);
                // Raw geometry companions are not MS/MI execution dependencies.
                // Use the Reader's prepared mesh closure, never saved material walks.
                if (kind == assets::AssetKind::mesh && !doc.unavailable(kind, key))
                    for (const auto& path : take(app.project_->assets().mesh_dependency_files(key)))
                        add(path);
            }
        }
        auto& job = new_job(id, peer);
        data.manifest = job.manifest;
        data.manifest["kind"] = "repro";
        data.manifest["original_scene_path"] = control::utf8(app.scene_path_.generic_wstring());
        data.manifest["original_dirty"] = app.document_->dirty();
        data.manifest["selection_indices"] = Json::array();
        const auto members = take(app.document_->world().members());
        for (auto selected : app.document_->selected_entities())
            for (std::size_t i = 0; i < members.size(); ++i)
                if (members[i].id() == selected)
                    data.manifest["selection_indices"].push_back(i);
        const auto selection = app.document_->selection();
        if (selection.object != selection.entity && selection.object != ObjectId{})
            for (std::size_t i = 0; i < members.size(); ++i)
                if (members[i].id() == selection.entity) {
                    const auto components = take(world::components(app.document_->world().runtime(), selection.entity));
                    for (std::size_t j = 0; j < components.size(); ++j)
                        if (components[j].id() == selection.object)
                            data.manifest["inspection"] = {{"entity", i}, {"component", j}};
                }
        data.manifest["source_version"] = data.sources ? data.sources->version : 0;
        data.manifest["ui_source_version"] = data.ui_source ? data.ui_source->version : 0;
        data.manifest["publication_version"] = data.publication_version;
        try {
            job.writer = std::async(std::launch::async,
                                    [path = job.directory, data = std::move(data), cancel = job.cancel]() mutable {
                                        return control_artifacts::export_repro(path, std::move(data), *cancel);
                                    });
        } catch (const std::exception& error) {
            cancel(job, "storage_error", error.what());
            return job.response;
        }
        return {{"status", "pending"}, {"job_id", id}};
    }
    Json start_render(const std::string& id, const std::shared_ptr<control::Peer>& peer, const Json& args, bool compare,
                      bool prepare = false) {
        if (!app.surface_.extent.width || !app.surface_.extent.height || !app.accepted_view_ ||
            app.accepted_view_->bounds.width <= 0 || app.accepted_view_->bounds.height <= 0)
            throw Failure("not_ready", "A visible accepted viewport is required");
        std::vector<Case> cases;
        if (compare) {
            const auto& input = args.at("cases");
            if (!input.is_array() || input.empty() || input.size() > 8)
                throw Failure("invalid_request", "Expected 1 to 8 cases");
            std::set<std::string> names;
            for (const auto& item : input) {
                control::fields(item, {"name", "patch"});
                auto name = text(item.at("name"), 80);
                if (name.empty() || !names.insert(name).second)
                    throw Failure("invalid_request", "Case names must be nonempty and unique");
                cases.push_back({name, patched_view(app.workspace_.view, item.at("patch"), app.lighting_focus())});
            }
        } else if (prepare) {
            auto patch = args.value("patch", Json::object());
            if (!patch.is_object())
                throw Failure("invalid_request", "Preparation patch must be an object");
            if (!patch.contains("shading"))
                patch["shading"] = "scene_lit";
            if (!patch.contains("wireframe"))
                patch["wireframe"] = "off";
            auto view = patched_view(app.workspace_.view, patch, app.lighting_focus());
            if (view.shading != renderer::Shading::scene_lit || view.wireframe == renderer::Wireframe::only ||
                app.scene_.meshes.empty())
                throw Failure("not_ready", "Scene Lit preparation requires a filled view and a mesh instance");
            cases.push_back({"prepare", view});
        } else
            cases.push_back({"current", app.workspace_.view});
        auto points_json = args.value("sample_frames", Json::array({1}));
        std::vector<unsigned> points;
        if (!points_json.is_array() || points_json.empty() || points_json.size() * cases.size() > 16)
            throw Failure("invalid_request", "Expected 1 to 16 total sample points");
        for (const auto& point : points_json) {
            auto n = integer(point, 1, 4096);
            if (!points.empty() && n <= points.back())
                throw Failure("invalid_request", "Sample ordinals must increase strictly");
            points.push_back(n);
        }
        const auto extent = app.surface_.extent;
        if (std::uint64_t(extent.width) * extent.height * 8 * points.size() * cases.size() > 256 * 1024 * 1024)
            throw Failure("resource_limit", "Captured color/depth would exceed 256 MiB");
        const auto timeout = integer(args.value("timeout_ms", Json(prepare ? control::maximum_timeout_ms : 120000)), 1,
                                     control::maximum_timeout_ms);
        auto& job = new_job(id, peer);
        job.cases = std::move(cases);
        job.points = std::move(points);
        job.rendering = true;
        job.preparing = prepare;
        job.bounds = app.accepted_view_->bounds;
        job.deadline = job.started + std::chrono::milliseconds(timeout);
        job.manifest["kind"] = prepare ? "render.prepare" : compare ? "render.compare" : "render.capture";
        job.manifest["history_policy"] =
            "natural renderer transitions, no full history reset or GPU-history restoration; accepted ordinals exclude skipped frames";
        return {{"status", "pending"}, {"job_id", id}, {"result", {{"phase", prepare ? "preparing" : "rendering"}}}};
    }
    Json operator_list() const {
        auto operators = Json::array();
        for (const auto& info : app.workspace_.operators.all()) {
            auto flags = Json::array();
            for (const auto& [flag, name] : {std::pair{editor::OperatorFlag::undo, "undo"},
                                             {editor::OperatorFlag::history, "history"},
                                             {editor::OperatorFlag::replaces_document, "replaces_document"},
                                             {editor::OperatorFlag::prompts, "prompts"}})
                if (editor::has_flag(info.flags, flag))
                    flags.push_back(name);
            auto parameters = editor::parameter_schema(app.document_->world().runtime(), info);
            if (!parameters)
                throw Failure("operation_failed", parameters.error().message);
            operators.push_back({{"id", info.id},
                                 {"title", control::utf8(info.title)},
                                 {"description", info.description},
                                 {"flags", flags},
                                 {"parameters", std::move(*parameters)}});
        }
        auto regions = Json::array();
        for (const auto name : editor::region_names())
            regions.push_back(name);
        const Json handle{{"type", "string"}};
        return {{"operators", std::move(operators)},
                {"context",
                 {{"type", "object"},
                  {"additionalProperties", false},
                  {"properties",
                   {{"selection",
                     {{"type", "object"},
                      {"additionalProperties", false},
                      {"required", {"entities"}},
                      {"properties",
                       {{"entities",
                         {{"type", "array"}, {"items", handle}, {"maxItems", editor::Session::maximum_entities}}},
                        {"active", handle},
                        {"object", handle}}}}},
                    {"region", {{"enum", std::move(regions)}}}}}}}};
    }
    editor::ContextOverride context_override(const Json& value) {
        control::fields(value, {"selection", "region"});
        editor::ContextOverride result;
        if (value.contains("selection")) {
            const auto& selection = value.at("selection");
            control::fields(selection, {"entities", "active", "object"});
            const auto& entities = selection.at("entities");
            if (!entities.is_array() || entities.size() > editor::Session::maximum_entities)
                throw Failure("invalid_request", "Invalid entity selection");
            editor::SelectionContext context;
            for (const auto& entity : entities)
                context.entities.push_back(object(entity));
            context.active = selection.contains("active") ? object(selection.at("active"))
                             : context.entities.empty()   ? ObjectId{}
                                                          : context.entities.back();
            context.object = selection.contains("object") ? object(selection.at("object")) : context.active;
            if (const auto at = std::ranges::find(context.entities, context.active); at != context.entities.end())
                std::rotate(at, at + 1, context.entities.end());
            result.selection = std::move(context);
        }
        if (value.contains("region")) {
            const auto name = text(value.at("region"), 40);
            const auto names = editor::region_names();
            const auto at = std::ranges::find(names, name);
            if (at == names.end())
                throw Failure("invalid_request", "Unknown region");
            result.region = static_cast<editor::RegionKind>(at - names.begin());
        }
        return result;
    }
    Json call_operator(const Json& args) {
        editor::OperatorCall request{text(args.at("id"), 80), Json::object(), {}};
        if (!app.workspace_.operators.find(request.id))
            throw Failure("unknown_operator", "No editor operator has this ID");
        if (args.contains("args")) {
            if (!args.at("args").is_object())
                throw Failure("invalid_request", "args must be an object");
            request.args = args.at("args");
        }
        if (args.contains("context"))
            request.context = context_override(args.at("context"));
        const auto report = app.call_operator(request, false);
        if (!report)
            throw Failure(report.error().code == ErrorCode::argument_mismatch ? "invalid_request" : "operation_failed",
                          report.error().message);
        if (report->status == editor::OperatorStatus::unavailable)
            throw Failure("poll_failed", report->message);
        auto selection = Json::array();
        for (const auto entity : app.document_->selected_entities())
            selection.push_back(handle(entity));
        return {{"operator", request.id},
                {"selection", std::move(selection)},
                {"active", handle(app.document_->selection().entity)},
                {"inspected_object", handle(app.document_->selection().object)}};
    }
    Json dispatch(std::string_view op, const Json& args, const std::string& id,
                  const std::shared_ptr<control::Peer>& peer) {
        auto& doc = *app.document_;
        auto& runtime = doc.world().runtime();
        if (op == "capabilities")
            return capabilities();
        if (op == "op.list")
            return operator_list();
        if (op == "editor.inspect") {
            const auto detail = args.contains("detail") ? text(args.at("detail")) : std::string{"full"};
            if (detail == "assets")
                return inspect_assets(args);
            if (detail == "objects")
                return inspect_objects(args);
            if (detail == "object") {
                control::fields(args, {"detail", "object"});
                return {{"detail", "object"}, {"object", object_json(object(args.at("object")))}};
            }
            control::fields(args, {"detail"});
            if (detail != "full" && detail != "summary")
                throw Failure("invalid_request", "Unsupported editor inspection detail");
            return inspect(detail == "summary");
        }
        if (op == "material.inspect") {
            if (args.contains("asset") == args.contains("object"))
                throw Failure("invalid_request", "Specify exactly one material asset or object");
            if (args.contains("asset") && !app.project_)
                throw Failure("not_ready", "No project is open");
            const auto target = args.contains("asset")
                                    ? take(doc.open_material(*app.project_, text(args.at("asset")))).id()
                                    : object(args.at("object"));
            if (!doc.is_material(target))
                throw Failure("invalid_request", "Expected a material object");
            return object_json(target);
        }
        if (op == "job.status" || op == "job.cancel") {
            const auto job_id = text(args.at("job_id"), 80);
            for (auto& job : jobs)
                if (job->id == job_id) {
                    if (op == "job.cancel")
                        cancel(*job, "cancelled", "Cancelled by control client");
                    if (job->complete)
                        return job->response;
                    Json progress = {{"phase", job->preparing   ? "preparing"
                                               : job->rendering ? "rendering"
                                                                : "writing"},
                                     {"case_index", job->case_index},
                                     {"accepted_frames", job->accepted},
                                     {"cancel_requested", job->cancel->load()}};
                    if (job->preparing)
                        if (const auto prepared = app.renderer_.preparation(job->cases.front().view)) {
                            progress["compiled"] = prepared->done;
                            progress["total"] = prepared->total;
                        }
                    return {{"status", "pending"}, {"job_id", job->id}, {"result", std::move(progress)}};
                }
            throw Failure("unknown_job", "Job expired or belongs to another instance");
        }
        idle();
        if (op == "op.call")
            return call_operator(args);
        if (op.starts_with("material.") || op == "texture.import" || op == "texture.reimport" || op == "mesh.import" ||
            op == "mesh.reimport") {
            if (op == "material.create_scene")
                return {
                    {"object",
                     handle(
                         take(doc.create_scene_material(object(args.at("component")), text(args.at("slot")))).id())}};
            if (op == "material.assign") {
                const auto component = object(args.at("component"));
                const auto slot = text(args.at("slot")), asset = text(args.at("asset"));
                if (asset.empty())
                    take(doc.clear_material(component, slot));
                else {
                    if (!app.project_)
                        throw Failure("not_ready", "No project is open");
                    take(doc.assign_material(*app.project_, component, slot, asset));
                }
                return Json::object();
            }
            if (op == "material.override" || op == "material.discard" || op == "material.set_entry") {
                const auto target = object(args.at("object"));
                if (!doc.is_material(target))
                    throw Failure("invalid_request", "Expected a material object");
                auto* project = app.project_ ? &*app.project_ : nullptr;
                if (op == "material.override")
                    take(doc.set_material_override(target, text(args.at("path")), args.at("enabled").get<bool>(),
                                                   project));
                else if (op == "material.discard")
                    take(doc.discard_material_override(target, text(args.at("path"))));
                else {
                    auto entries = assets::material_format::decode_entries(Json::array({args.at("entry")}));
                    const auto control = ++edit_id;
                    take(doc.begin_material_edit(target, control));
                    auto changed = doc.preview_material_entry(control, std::move(entries.front()), project);
                    if (changed)
                        changed = doc.commit_material_edit(control);
                    if (!changed) {
                        take(doc.cancel_material_edit());
                        take(std::move(changed));
                    }
                }
                return Json::object();
            }
            if (!app.project_)
                throw Failure("not_ready", "No project is open");
            auto& project = *app.project_;
            if (op == "material.create") {
                const auto role = text(args.at("role"));
                const auto path = control::wide(text(args.at("path")));
                if (role == "source") {
                    if (args.contains("parent") || args.contains("entries"))
                        throw Failure("invalid_request",
                                      "A Material Source cannot contain instance entries or a parent");
                    return {{"asset", take(project.create_material_source(
                                          path, text(args.value("source", Json("")), control::maximum_message)))}};
                }
                if (role != "instance" || args.contains("source"))
                    throw Failure("invalid_request", "Expected source or instance author role");
                assets::MaterialAsset asset;
                auto& author = std::get<assets::MaterialInstanceAsset>(asset.author);
                if (args.contains("parent"))
                    author.parent_key = text(args.at("parent"));
                if (args.contains("entries"))
                    author.entries = assets::material_format::decode_entries(args.at("entries"));
                return {{"asset", take(project.create_material(path, asset))}};
            }
            if (op == "texture.import")
                return {{"asset", take(project.import_texture(control::wide(text(args.at("path")))))}};
            if (op == "texture.reimport") {
                const auto key = text(args.at("asset"));
                take(doc.reimport_texture(project, key, control::wide(text(args.at("path")))));
                return {{"asset", key}};
            }
            if (op == "mesh.reimport") {
                const auto key = text(args.at("asset"));
                take(doc.reimport_mesh(project, key, control::wide(text(args.at("path")))));
                return {{"asset", key}};
            }
            if (op == "mesh.import")
                return {{"asset", take(project.import_mesh(control::wide(text(args.at("path")))))}};
            if (op == "material.save") {
                take(doc.save_material(project, text(args.at("asset"))));
                const auto refreshed = project.refresh();
                Json result{{"saved", true}};
                if (!refreshed)
                    result["catalog_warning"] = refreshed.error().message;
                return result;
            }
            if (op == "material.undo" || op == "material.redo") {
                take(doc.material_undo(text(args.at("asset")), op == "material.redo", &project));
                return Json::object();
            }
            if (op == "material.set_texture") {
                take(doc.set_material_texture(project, object(args.at("object")), text(args.at("path")),
                                              text(args.at("texture"))));
                return Json::object();
            }
            throw Failure("invalid_request", "Unknown material operation");
        }
        if (op == "project.open") {
            replaceable();
            take(app.open_project(control::wide(text(args.at("path")))));
        } else if (op == "scene.open") {
            replaceable();
            take(app.open_scene(control::wide(text(args.at("path")))));
        } else if (op == "scene.save")
            take(app.save_scene(control::wide(text(args.value("path", Json(""))))));
        else if (op == "scene.create") {
            const auto kind = text(args.at("kind"), 40);
            editor::Command command;
            if (kind == "entity")
                command = editor::Command::create_entity;
            else if (kind == "mesh")
                command = editor::Command::create_static_mesh;
            else if (kind == "point")
                command = editor::Command::create_point_light;
            else if (kind == "rectangle")
                command = editor::Command::create_rectangle_light;
            else if (kind == "directional")
                command = editor::Command::create_directional_light;
            else
                throw Failure("invalid_request",
                              "Unknown entity creation kind; use entity, mesh, point, rectangle or directional. "
                              "The sky is a World setting: view.set sky_enabled, sky_color_x/y/z and sky_intensity.");
            if (!app.workspace_.command_enabled(command, doc))
                throw Failure("unavailable", "Creation is unavailable in this document");
            take(app.execute(command, text(args.value("mesh", Json("")))));
            return {{"object", handle(doc.selection().entity)}};
        } else if (op == "scene.rename")
            take(doc.rename(object(args.at("object")), control::wide(text(args.at("name"), 1024))));
        else if (op == "scene.set_property") {
            const auto target = object(args.at("object"));
            const auto name = text(args.at("property"), 256);
            const auto properties = take(doc.properties(target));
            const auto found = std::ranges::find(properties, name, &editor::Property::name);
            if (found == properties.end())
                throw Failure("invalid_request", "Unknown reflected property");
            take(doc.set_property(target, found->path, scalar_value(args.at("value"), found->value, runtime),
                                  ++edit_id));
        } else if (op == "scene.transform") {
            const auto target = object(args.at("object"));
            const auto& rotation = args.at("rotation");
            if (!rotation.is_array() || rotation.size() != 4)
                throw Failure("invalid_request", "Rotation requires quaternion xyzw");
            world::Trs value{vector3(args.at("position")),
                             {static_cast<float>(number(rotation[0])), static_cast<float>(number(rotation[1])),
                              static_cast<float>(number(rotation[2])), static_cast<float>(number(rotation[3]))},
                             vector3(args.at("scale"))};
            take(doc.set_transform(target, value));
        } else if (op == "scene.remove_transform")
            take(doc.remove_transform(object(args.at("object"))));
        else if (op == "scene.set_space_reference") {
            std::optional<ObjectId> origin;
            if (!args.at("origin").is_null())
                origin = text(args.at("origin"), 80).empty() ? ObjectId{} : object(args.at("origin"));
            committed(doc.set_space_reference(object(args.at("object")), origin,
                                              args.value("keep_world", true) ? world::ReparentRule::keep_world
                                                                             : world::ReparentRule::keep_local));
        } else if (op == "scene.reparent") {
            const auto parent =
                args.contains("parent") && !args.at("parent").is_null() ? object(args.at("parent")) : ObjectId{};
            const auto rule =
                args.value("keep_world", true) ? world::ReparentRule::keep_world : world::ReparentRule::keep_local;
            committed(doc.reparent(object(args.at("object")), parent, rule, args.value("index", world::append)));
        } else if (op == "scene.add_component") {
            const auto name = text(args.at("type"), 256);
            const auto types = take(doc.component_types());
            const auto found = std::ranges::find(types, name, &editor::ComponentType::name);
            if (found == types.end())
                throw Failure("invalid_request", "Component type is not available");
            committed(doc.add_component(found->type));
            return {{"object", handle(doc.selection().object)}};
        } else if (op == "scene.remove_component")
            committed(doc.remove_component(object(args.at("object"))));
        else if (op == "scene.delete")
            committed(doc.erase_selected());
        else if (op == "camera.set" || op == "camera.look_at") {
            auto camera = app.camera_;
            const auto valid = op == "camera.set" ? camera.set_state(camera_state(args.at("state")))
                                                  : camera.look_at(vector3(args.at("eye")), vector3(args.at("target")));
            if (!valid)
                throw Failure("invalid_request", "Invalid camera state or eye/target");
            take(app.set_camera(camera, true));
        } else if (op == "view.layout") {
            idle();
            const auto layout = text(args.at("layout"));
            if (layout != "fit_scene" && layout != "scene_focus")
                throw Failure("invalid_request", "Unsupported DDGI coverage layout");
            take(app.execute(layout == "fit_scene" ? editor::Command::fit_probe_scene
                                                   : editor::Command::fit_probe_scene_focus));
        } else if (op == "view.set") {
            if (args.at("patch").contains("grid_plane"))
                throw Failure("invalid_request",
                              "Persistent grid plane follows camera view; temporary render cases may override it");
            auto view = patched_view(app.workspace_.view, args.at("patch"), app.lighting_focus());
            take(app.workspace_.set_view_settings(*app.document_, view, app.lighting_focus()));
            app.workspace_.viewport_settings.grid_spacing = view.grid_spacing;
        } else if (op == "repro.export")
            return start_export(id, peer);
        else if (op == "render.capture" || op == "render.compare" || op == "render.prepare")
            return start_render(id, peer, args, op == "render.compare", op == "render.prepare");
        else if (op == "repro.open") {
            replaceable();
            const std::filesystem::path root = control::wide(text(args.at("directory")));
            const auto manifest = control_artifacts::validate_repro(root);
            if (manifest.value("view_schema", Json()) != repro_view_schema)
                throw Failure("invalid_request", "Repro view schema is not current; export the repro again");
            auto camera = app.camera_;
            if (!camera.set_state(camera_state(manifest.at("camera"))))
                throw Failure("invalid_request", "Invalid repro camera");
            const auto focus = camera.state().target;
            auto view = patched_view({}, manifest.at("view"), {focus.x, focus.y, focus.z});
            std::vector<unsigned> indices;
            for (const auto& index : manifest.at("selection_indices")) {
                const auto i = integer(index, 0, editor::Session::maximum_entities);
                if (std::ranges::find(indices, i) != indices.end())
                    throw Failure("dependency_mismatch", "Invalid repro selection");
                indices.push_back(i);
            }
            std::optional<unsigned> inspection;
            if (manifest.contains("inspection")) {
                const auto& value = manifest["inspection"];
                control::fields(value, {"entity", "component"});
                const auto a = integer(value.at("entity"), 0, editor::Session::maximum_entities),
                           c = integer(value.at("component"), 0, editor::Session::maximum_components);
                if (indices.size() != 1 || indices[0] != a)
                    throw Failure("dependency_mismatch", "Invalid repro component inspection");
                inspection = c;
            }
            std::vector<ObjectId> selected;
            ObjectId inspected{};
            bool mismatch = false;
            const auto opened = app.open_project(
                root / "Project", false, ProjectOpenPolicy::require_scene,
                [&](const editor::Session& candidate) -> Result<void> {
                    control_artifacts::validate_candidate_assets(candidate, manifest);
                    const auto reject = [&](std::string message) -> Result<void> {
                        mismatch = true;
                        return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
                    };
                    const auto environment = candidate.presentation().environment();
                    if (!environment)
                        return std::unexpected(environment.error());
                    // Only the scene records the sky map and its rotation; the view snapshot holds view options.
                    view.lit.sky_map = environment->sky_map;
                    view.lit.sky_rotation = environment->sky_rotation;
                    if (static_cast<const rendering::SceneLightingSettings&>(view.lit) != *environment)
                        return reject("Repro frame snapshot disagrees with its Scene Environment");
                    // Indices refer to World members, as written by repro.export; resolve them before the candidate replaces the document.
                    const auto members = candidate.world().members();
                    if (!members)
                        return std::unexpected(members.error());
                    for (const auto i : indices) {
                        if (i >= members->size())
                            return reject("Repro selection exceeds scene size");
                        selected.push_back((*members)[i].id());
                    }
                    if (inspection) {
                        const auto components = world::components(candidate.world().runtime(), selected.front());
                        if (!components)
                            return std::unexpected(components.error());
                        if (*inspection >= components->size())
                            return reject("Invalid repro component inspection");
                        inspected = (*components)[*inspection].id();
                    }
                    return {};
                });
            if (!opened)
                throw Failure(mismatch ? "dependency_mismatch" : "operation_failed", opened.error().message);
            take(app.workspace_.set_view_settings(*app.document_, view, {focus.x, focus.y, focus.z}));
            app.document_->mark_saved();
            app.camera_ = camera;
            app.camera_cut_ = true;
            app.workspace_.viewport_settings.grid_spacing = view.grid_spacing;
            take(app.document_->select_entities(selected, selected.empty() ? ObjectId{} : selected.back()));
            if (inspection)
                take(app.document_->select(inspected));
        } else
            throw Failure("unknown_operation", "Operation has no implementation");
        return Json::object();
    }
    Json request(const Json& request, const std::shared_ptr<control::Peer>& peer) {
        app.sync_world_view();
        Json response;
        std::string id, canonical;
        bool remember{};
        try {
            control::fields(request, {"protocol", "instance_id", "request_id", "op", "args", "state_token"});
            if (integer(request.at("protocol"), 1, 1) != control::protocol_version ||
                request.at("instance_id") != server.identity().at("instance_id"))
                throw Failure("protocol_mismatch", "Wrong protocol or editor instance");
            id = text(request.at("request_id"), 80);
            if (id.empty())
                throw Failure("invalid_request", "Empty request identity");
            canonical = request.dump();
            for (const auto& entry : cache)
                if (entry.id == id) {
                    if (entry.request != canonical)
                        throw Failure("request_id_conflict", "Request identity was used for different content");
                    return entry.response;
                }
            if (std::ranges::any_of(jobs, [&](const auto& job) { return job->id == id; }))
                throw Failure("request_id_expired", "Request payload expired; query the retained job using job.status");
            const auto name = text(request.at("op"), 80);
            const auto& known = operations();
            const auto op = std::ranges::find(known, name, &Operation::name);
            if (op == known.end())
                throw Failure("unknown_operation", "Operation is not exposed by this editor");
            const auto args = request.value("args", Json::object());
            if (!args.is_object())
                throw Failure("invalid_request", "args must be an object");
            for (const auto& item : args.items())
                if (!op->properties.contains(item.key()))
                    throw Failure("invalid_request", "Unknown argument: " + item.key());
            for (const auto& field : op->required)
                if (!args.contains(field))
                    throw Failure("invalid_request", "Missing argument: " + field);
            observe();
            remember = op->changes;
            if (op->changes) {
                if (request.value("state_token", "") != token())
                    throw Failure("stale_state", "Inspect the editor again before changing its state");
            }
            auto result = dispatch(name, args, id, peer);
            if (result.contains("status"))
                response = std::move(result);
            else
                response = {{"status", "ok"}, {"result", std::move(result)}};
        } catch (const Failure& error) {
            response = control::failure(error.code, error.what());
            if (error.code == "busy" && busy())
                response["error"]["busy_reasons"] = busy_reasons();
        } catch (const Json::exception& error) {
            response = control::failure("invalid_request", error.what());
        } catch (const std::exception& error) {
            response = control::failure("operation_failed", error.what());
        }
        observe();
        response["state_token"] = token();
        if (remember) {
            const auto bytes = canonical.size() + response.dump().size();
            cache_bytes += bytes;
            cache.push_back({id, std::move(canonical), response, bytes});
            trim_cache();
        }
        return response;
    }
};
EditorControl::EditorControl(Application& app, const std::filesystem::path& directory)
    : impl_(std::make_unique<Impl>(app, directory)) {}
EditorControl::~EditorControl() = default;
void EditorControl::poll() {
    impl_->checkpoint();
    impl_->server.poll([&](const auto& request, const auto& peer) { return impl_->request(request, peer); });
    impl_->checkpoint();
}
void EditorControl::document_replaced() {
    ++impl_->document_generation;
    impl_->handles.clear();
    impl_->objects.clear();
    impl_->last_frame.reset();
}
void EditorControl::input(UINT message, WPARAM wparam) {
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_CHAR || message == WM_MOUSEWHEEL ||
        message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
        (message == WM_MOUSEMOVE && (wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))))
        ++impl_->input_revision;
}
renderer::ViewSettings EditorControl::view() const {
    const auto* job = impl_->active();
    return job && job->rendering ? job->cases[job->case_index].view : impl_->app.workspace_.view;
}
bool EditorControl::wants_capture() {
    impl_->checkpoint();
    const auto* job = impl_->active();
    return job && job->rendering && !job->preparing && job->accepted + 1 == job->points[job->point_index];
}
void EditorControl::render_failed(std::string_view message) {
    if (auto* job = impl_->active(); job && job->rendering)
        impl_->cancel(*job, "render_failure", message);
}
void EditorControl::rendered(const renderer::Frame& frame, const renderer::FrameResult& result,
                             renderer::Capture* capture) {
    auto& state = *impl_;
    state.checkpoint();
    auto* job = state.active();
    if (job && job->rendering)
        job->history_resets += result.stats.scene_lit_history_resets;
    if (result.status != renderer::FrameStatus::accepted) {
        if (job && job->rendering)
            ++job->skipped;
        return;
    }
    const auto& bounds = frame.scene_viewport;
    state.last_frame = Impl::AcceptedFrame{
        result.accepted_id, state.app.accepted_frames_, bounds,      result.accepted_extent, frame.camera, frame.view,
        frame.environment,  frame.lighting_focus,       result.stats};
    if (!job || !job->rendering)
        return;
    if (bounds != job->bounds) {
        state.cancel(*job, "state_changed", "Viewport layout changed");
        return;
    }
    ++job->accepted;
    if (job->preparing) {
        job->rendering = false;
        job->preparing = false;
        job->complete = true;
        job->response = {
            {"status", "ok"}, {"job_id", job->id}, {"result", {{"phase", "ready"}, {"frame", state.frame_json()}}}};
        return;
    }
    if (job->accepted != job->points[job->point_index])
        return;
    if (!capture || capture->frame_id != result.accepted_id || capture->extent != result.accepted_extent) {
        state.cancel(*job, "render_failure", "Capture does not belong to the accepted frame");
        return;
    }
    auto metadata = state.frame_json();
    metadata["case"] = job->cases[job->case_index].name;
    metadata["accepted_ordinal"] = job->accepted;
    metadata["warmup_accepted_frames"] = job->accepted - 1;
    metadata["skipped_frames"] = job->skipped;
    metadata["case_reported_history_resets"] = job->history_resets;
    job->samples.push_back({std::move(*capture), std::move(metadata)});
    if (++job->point_index == job->points.size()) {
        job->point_index = 0;
        job->accepted = 0;
        job->skipped = 0;
        job->history_resets = 0;
        if (++job->case_index == job->cases.size()) {
            job->rendering = false;
            try {
                job->writer =
                    std::async(std::launch::async, [path = job->directory, samples = std::move(job->samples),
                                                    manifest = job->manifest, cancel = job->cancel]() mutable {
                        return control_artifacts::write_samples(path, std::move(samples), std::move(manifest), *cancel);
                    });
            } catch (const std::exception& error) {
                state.cancel(*job, "storage_error", error.what());
            }
        }
    }
}
}
