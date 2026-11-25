#include "application/editor_control.hpp"
#include "application/application.hpp"
#include "application/diagnostics.hpp"
#include "detail/control_artifacts.hpp"
#include "editor/fields.hpp"
#include "editor/view_options.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "procedural/bvh.hpp"
#include "procedural/operations.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include "atelier/build_info.hpp"
#include <Windows.h>
#include <charconv>
#include <deque>
#include <future>
#include <limits>
#include <set>
#include <unordered_set>

namespace atelier::application {
namespace {
using control::Failure;

// Bumped whenever the frozen camera/view contract changes; repro.open accepts only this schema.
constexpr unsigned repro_view_schema = 7;

constexpr std::array<std::string_view, 3> removed_view_options{"direct_reference", "direct_clustered",
                                                               "direct_shadow_cache"};

constexpr unsigned inspect_page_items = 256;
constexpr std::size_t graph_ray_limit = 32;

constexpr std::size_t inspect_page_bytes = control::maximum_message / 4;

json::Result<std::string> text(json::Value value, std::size_t maximum = 4096) {
    ATELIER_TRY_BIND(result, value.string());
    if (result->size() > maximum || result->find('\0') != result->npos)
        return std::unexpected(value.error(json::Fault::range, "Invalid string length or NUL"));
    return std::string(*result);
}

std::expected<std::string, Failure> utf8_text(std::wstring_view value) {
    return platform::utf16_to_utf8(value).transform_error(
        [](platform::UtfError) { return Failure("invalid_request", "Invalid UTF-16 text"); });
}

json::Result<unsigned> integer(json::Value value, unsigned low, unsigned high) {
    if (value.kind() == json::Kind::integer) {
        ATELIER_TRY_BIND(n, value.u32());
        if (*n < low || *n > high)
            return std::unexpected(value.error(json::Fault::range, "Integer outside supported range"));
        return *n;
    }
    ATELIER_TRY_BIND(n, value.number());
    if (*n < low || *n > high || std::floor(*n) != *n)
        return std::unexpected(value.error(json::Fault::range, "Integer outside supported range"));
    return static_cast<unsigned>(*n);
}

Result<editor::CameraState> camera_state(Runtime& runtime, json::Value args) {
    editor::CameraState value;
    ATELIER_TRY_BIND(type, runtime.native_type<editor::CameraState>());
    ATELIER_TRY(editor::decode_properties(runtime, {*type, {}, std::as_writable_bytes(std::span{&value, 1})}, args));
    return value;
}

Failure operation_failure(const Error& error) {
    return Failure("operation_failed", error.message);
}

json::Result<json::Document> view_json(const renderer::ViewSettings& view) {
    json::Builder result;
    for (const auto& option : editor::view_options()) {
        const auto value = editor::view_value(view, option.option);
        if (option.kind == editor::ViewValueKind::enumeration) {
            ATELIER_TRY(result.set(result.root(), option.name, option.choices[static_cast<std::size_t>(value)]));
        } else if (option.kind == editor::ViewValueKind::boolean) {
            ATELIER_TRY(result.set(result.root(), option.name, value != 0));
        } else if (option.kind == editor::ViewValueKind::number) {
            ATELIER_TRY(result.set(result.root(), option.name, static_cast<float>(value)));
        } else {
            ATELIER_TRY(result.set(result.root(), option.name, value));
        }
    }
    return result.finish();
}

json::Result<void> numbers(json::Builder& result, json::Value node, std::string_view key, math::Vector3 value) {
    const std::array values{value.x, value.y, value.z};
    return result.numbers(node, key, std::span{values});
}

json::Result<json::Document>
probe_volumes_json(const std::array<renderer::ProbeVolumeStats, renderer::probe_volume_limit>& volumes) {
    json::Builder result(json::Kind::array);
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const auto& volume = volumes[i];
        const auto item = result.append_object(result.root());
        ATELIER_TRY(result.set(item, "slot", i));
        ATELIER_TRY(result.set(item, "enabled", volume.enabled));
        ATELIER_TRY(result.set(item, "rolling", volume.rolling));
        ATELIER_TRY(numbers(result, item, "minimum", volume.minimum));
        ATELIER_TRY(numbers(result, item, "maximum", volume.maximum));
        ATELIER_TRY(numbers(result, item, "spacing", volume.spacing));
        ATELIER_TRY(result.numbers(item, "counts", std::span{volume.counts}));
        ATELIER_TRY(result.set(item, "resident", volume.resident));
        ATELIER_TRY(result.set(item, "updated", volume.updated));
        ATELIER_TRY(result.set(item, "reset_probes", volume.reset_probes));
    }
    return result.finish();
}

json::Result<json::Document> probe_inspection_json(const std::shared_ptr<const renderer::ProbeInspection>& value) {
    if (!value)
        return json::Builder(json::Kind::null).finish();
    const auto& v = *value;
    const auto& r = v.receiver;
    constexpr std::array modes{"live", "hold_irradiance", "hold_distance"};
    const auto selection = v.requested_probe >= 0 ? (v.traced_probe == ~0u ? "explicit_out_of_range" : "explicit")
                                                  : (v.traced_probe == ~0u ? "automatic_pending" : "automatic");
    json::Builder result;
    const auto root = result.root();
    ATELIER_TRY(result.set(root, "sequence", v.sequence));
    ATELIER_TRY(result.set(root, "frame_id", v.frame_id));
    ATELIER_TRY(result.set(root, "scene_identity", v.scene_identity));
    ATELIER_TRY(result.numbers(root, "pixel", std::span{v.pixel}));
    ATELIER_TRY(result.set(root, "history", std::string_view{modes[static_cast<unsigned>(v.history)]}));
    ATELIER_TRY(result.set(root, "rays", v.rays));
    ATELIER_TRY(result.set(root, "updates", v.updates));
    ATELIER_TRY_BIND(volumes, probe_volumes_json(v.volumes));
    result.set_validated(root, "volumes", volumes->root());
    const auto receiver = result.object_validated(root, "receiver");
    ATELIER_TRY(result.set(receiver, "status", r.identity[0]));
    ATELIER_TRY(result.set(receiver, "volume", r.identity[1]));
    ATELIER_TRY(result.set(receiver, "contributors", r.identity[2]));
    ATELIER_TRY(result.numbers(receiver, "irradiance_coverage", std::span{r.irradiance}));
    ATELIER_TRY(result.set(receiver, "weight_sum", r.weights[0]));
    ATELIER_TRY(result.set(receiver, "dominance", r.weights[1]));
    ATELIER_TRY(result.numbers(receiver, "position_valid", std::span{r.position}));
    ATELIER_TRY_BIND(probes, result.array(root, "probes"));
    for (const auto& p : v.probes) {
        if (!p.identity[2])
            continue;
        const auto item = result.append_object(*probes);
        ATELIER_TRY(result.set(item, "volume", p.identity[0]));
        ATELIER_TRY(result.set(item, "probe", p.identity[1]));
        ATELIER_TRY(result.set(item, "support_stage", p.identity[2]));
        ATELIER_TRY(result.set(item, "age_frames", p.identity[3]));
        ATELIER_TRY(result.numbers(item, "irradiance_observations", std::span{p.irradiance}));
        ATELIER_TRY(result.numbers(item, "spatial_facing_visibility_weight", std::span{p.weights}));
        ATELIER_TRY(result.numbers(item, "position_distance", std::span{p.position}));
        ATELIER_TRY(result.numbers(item, "transport_observations", std::span{p.transport}));
        ATELIER_TRY(result.numbers(item, "response_epoch_min_max_visits", std::span{p.response}));
    }
    ATELIER_TRY(result.set(root, "requested_probe", static_cast<std::int64_t>(v.requested_probe)));
    ATELIER_TRY(result.set(root, "probe_selection", std::string_view{selection}));
    ATELIER_TRY(result.set(root, "traced_probe", v.traced_probe));
    ATELIER_TRY_BIND(rays, result.array(root, "ray_samples"));
    for (const auto& ray : v.ray_samples) {
        if (!ray.identity[3])
            continue;
        const auto item = result.append_object(*rays);
        ATELIER_TRY(result.numbers(item, "identity", std::span{ray.identity}));
        ATELIER_TRY(result.numbers(item, "direct_luminance", std::span{ray.direct}));
        ATELIER_TRY(result.numbers(item, "surface_roughness", std::span{ray.surface}));
        ATELIER_TRY(result.numbers(item, "sampling", std::span{ray.sampling}));
        ATELIER_TRY(result.numbers(item, "light_endpoint_distance", std::span{ray.endpoint_distance}));
        ATELIER_TRY(
            result.numbers(item, "source_radius_range_area_min_distance_squared", std::span{ray.source_parameters}));
    }
    return result.finish();
}

json::Result<json::Document> performance_json(const editor::FramePerformance& performance) {
    const auto& sample = performance.snapshot();
    constexpr std::array states{"unavailable", "waiting", "ready"};
    constexpr std::array aa_names{"off", "taa", "smaa", "smaa_t2x"};
    json::Builder result;
    const auto root = result.root();
    ATELIER_TRY(result.set(root, "state", std::string_view{states[static_cast<unsigned>(sample.state)]}));
    ATELIER_TRY(result.set(root, "window_capacity", std::uint64_t{editor::FramePerformance::window_size}));
    ATELIER_TRY(result.set(root, "publish_interval_ms", 250u));
    ATELIER_TRY(result.set(root, "samples", std::uint64_t{sample.samples}));
    ATELIER_TRY(result.set(root, "sequence", sample.sequence));
    ATELIER_TRY(result.set(root, "frame_id", sample.frame_id));
    ATELIER_TRY(result.set(root, "scene_identity", sample.scene_identity));
    ATELIER_TRY(result.set(root, "domain_first_frame", sample.first_frame));
    const std::array extent{sample.extent.width, sample.extent.height};
    const std::array viewport{sample.viewport.x, sample.viewport.y, sample.viewport.width, sample.viewport.height};
    ATELIER_TRY(result.numbers(root, "extent", std::span{extent}));
    ATELIER_TRY(result.numbers(root, "viewport", std::span{viewport}));
    const auto configuration = result.object_validated(root, "configuration");
    ATELIER_TRY(result.set(configuration, "antialiasing",
                           std::string_view{aa_names[static_cast<unsigned>(sample.antialiasing)]}));
    ATELIER_TRY(result.set(configuration, "direct_candidates", sample.direct_candidates));
    ATELIER_TRY(result.set(configuration, "ddgi", sample.ddgi));
    if (sample.resource_bytes) {
        ATELIER_TRY(result.set(root, "logical_resource_bytes", *sample.resource_bytes));
    } else {
        ATELIER_TRY(result.set_null(root, "logical_resource_bytes"));
    }
    result.set_validated(root, "resource_semantics",
                         "Current logical Scene Lit allocation; excludes retired resources and physical residency");
    result.set_validated(
        root, "total_semantics",
        "Sum of eight measured main intervals; excludes UI, guides, probe overlays, capture and CPU Present");
    // One row of either section; values without an observation are null.
    const auto statistics = [&](json::Value row,
                                const std::optional<editor::FramePerformance::Timing>& timing) -> json::Result<void> {
        const bool active = timing && timing->active_samples;
        result.set_validated(row, "state", !timing ? "unavailable" : timing->current_active ? "measured" : "skipped");
        ATELIER_TRY(result.set(row, "active_samples", std::uint64_t{timing ? timing->active_samples : 0}));
        for (const auto field : {"current_ms", "mean_ms", "p95_ms", "max_ms", "share_percent"}) {
            ATELIER_TRY(result.set_null(row, field));
        }
        if (timing && timing->current_active) {
            ATELIER_TRY(result.set(row, "current_ms", timing->current));
        }
        if (active) {
            ATELIER_TRY(result.set(row, "mean_ms", timing->mean));
            ATELIER_TRY(result.set(row, "p95_ms", timing->p95));
            ATELIER_TRY(result.set(row, "max_ms", timing->max));
            if (timing->share) {
                ATELIER_TRY(result.set(row, "share_percent", *timing->share));
            }
        }
        return {};
    };
    ATELIER_TRY_BIND(passes, result.array(root, "passes"));
    const auto definitions = editor::FramePerformance::passes();
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto& pass = definitions[i];
        const auto row = result.append_object(*passes);
        ATELIER_TRY(statistics(row, sample.timings[i]));
        ATELIER_TRY(result.set(row, "id", pass.key));
        ATELIER_TRY(result.set(row, "title", pass.title));
        if (pass.parent < 0) {
            ATELIER_TRY(result.set_null(row, "parent"));
        } else {
            ATELIER_TRY(result.set(row, "parent", definitions[pass.parent].key));
        }
        ATELIER_TRY(result.set(row, "depth", std::uint64_t{pass.depth}));
        ATELIER_TRY(result.set(row, "included_in_total", pass.in_total));
    }
    const auto& cpu = sample.cpu;
    const auto cpu_record = result.object_validated(root, "cpu");
    ATELIER_TRY_BIND(phases, result.array(cpu_record, "phases"));
    for (const auto* marker : timing::catalog()) {
        const auto phase = result.append_object(*phases);
        ATELIER_TRY(statistics(phase, cpu.phases[marker->index()]));
        ATELIER_TRY(result.set(phase, "id", marker->key()));
        ATELIER_TRY(result.set(phase, "title", marker->title()));
        if (marker->parent()) {
            ATELIER_TRY(result.set(phase, "parent", marker->parent()->key()));
        } else {
            ATELIER_TRY(result.set_null(phase, "parent"));
        }
        ATELIER_TRY(result.set(phase, "depth", std::uint64_t{timing::depth(*marker)}));
        ATELIER_TRY(result.set(phase, "current_entries", cpu.current_entries[marker->index()]));
    }
    const auto unattributed = result.append_object(*phases);
    ATELIER_TRY(statistics(unattributed, cpu.unattributed));
    result.set_validated(unattributed, "id", "frame.unattributed");
    result.set_validated(unattributed, "title", "Unattributed");
    result.set_validated(unattributed, "parent", "frame");
    ATELIER_TRY(result.set(unattributed, "depth", 1u));
    ATELIER_TRY(result.set_null(unattributed, "current_entries"));
    const auto* first = performance.first_structure_error();
    result.set_validated(cpu_record, "state", cpu.samples ? "ready" : "waiting");
    ATELIER_TRY(result.set(cpu_record, "window_capacity", std::uint64_t{editor::FramePerformance::window_size}));
    ATELIER_TRY(result.set(cpu_record, "samples", std::uint64_t{cpu.samples}));
    ATELIER_TRY(result.set(cpu_record, "sequence", cpu.sequence));
    ATELIER_TRY(result.set(cpu_record, "frame_id", cpu.frame_id));
    if (cpu.interval) {
        const auto interval = result.object_validated(cpu_record, "frame_interval_ms");
        ATELIER_TRY(result.set(interval, "mean", cpu.interval->mean));
        ATELIER_TRY(result.set(interval, "p95", cpu.interval->p95));
        ATELIER_TRY(result.set(interval, "max", cpu.interval->max));
        ATELIER_TRY(result.set(cpu_record, "frames_per_second", 1000 / cpu.interval->mean));
    } else {
        ATELIER_TRY(result.set_null(cpu_record, "frame_interval_ms"));
        ATELIER_TRY(result.set_null(cpu_record, "frames_per_second"));
    }
    ATELIER_TRY(result.set(cpu_record, "structure_errors", performance.structure_errors()));
    if (first) {
        ATELIER_TRY(result.set(cpu_record, "first_structure_error", first->key()));
    } else {
        ATELIER_TRY(result.set_null(cpu_record, "first_structure_error"));
    }
    result.set_validated(
        cpu_record, "semantics",
        "CPU wall time in milliseconds of accepted editor steps without capture, read from frame timing records. "
        "Phases are inclusive and sum every entry in the step; frame.unattributed is the frame minus its direct children. "
        "frame_interval_ms is between the begins of consecutive sampled steps. structure_errors counts every step since start, sampled or not.");
    return result.finish();
}

json::Result<json::Document> scalar_json(const Scalar& scalar, const TypeInfo& type) {
    return std::visit(
        [&](const auto& value) -> json::Result<json::Document> {
            json::Builder result(json::Kind::null);
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, EnumValue>) {
                if (type.enumeration)
                    for (const auto& [name, bits] : type.enumeration->values)
                        if (bits == value.bits) {
                            ATELIER_TRY(result.set(result.root(), name));
                            return result.finish();
                        }
                result.set(result.root(), static_cast<std::uint64_t>(value.bits));
            } else if constexpr (std::is_floating_point_v<T>) {
                ATELIER_TRY(result.set(result.root(), value));
            } else if constexpr (std::is_same_v<T, bool>)
                result.set(result.root(), value);
            else if constexpr (std::is_signed_v<T>)
                result.set(result.root(), static_cast<std::int64_t>(value));
            else
                result.set(result.root(), static_cast<std::uint64_t>(value));
            return result.finish();
        },
        scalar);
}

enum class Argument {
    string,
    object,
    array,
    integer,
    boolean,
    detail,
    page_limit,
    trace_scope,
    graph_scope,
    graph_view,
    graph_raycast
};

struct Operation {
    std::string_view name;

    std::string_view description;

    bool changes;

    std::vector<std::pair<std::string_view, Argument>> properties;

    std::vector<std::string> required;

    editor::OperatorKind kind{editor::OperatorKind::direct};
};

constexpr std::array operator_kind_names{"query", "direct", "interactive"};

const std::vector<Operation>& operations() {
    constexpr auto str = Argument::string;
    constexpr auto obj = Argument::object;
    constexpr auto arr = Argument::array;
    static const std::vector<Operation> values{
        {"capabilities",
         "Discover operations, arguments, limits and display options; scope=graph lists the six graph authoring tools",
         false,
         {{"scope", Argument::graph_scope}},
         {},
         editor::OperatorKind::query},
        {"op.list",
         "List editor operators: invocation kinds, identity, flags and schema; the context override schema",
         false,
         {},
         {},
         editor::OperatorKind::query},
        {"op.call",
         "Execute a direct operator call; discover invocation kinds and argument conditions with op.list",
         true,
         {{"id", str}, {"args", obj}, {"context", obj}},
         {"id"}},
        {"editor.inspect",
         "Read editor state, bounded asset/object directories, or one discovered object",
         false,
         {{"detail", Argument::detail},
          {"object", str},
          {"entity_name", str},
          {"type", str},
          {"limit", Argument::page_limit},
          {"cursor", obj}},
         {},
         editor::OperatorKind::query},
        {"material.inspect",
         "Read owning MS declarations, sparse MI entries, effective values and diagnostics; asset may load resources",
         false,
         {{"asset", str}, {"object", str}},
         {},
         editor::OperatorKind::direct},
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
         {{"object", str}, {"path", str}, {"enabled", Argument::boolean}},
         {"object", "path", "enabled"}},
        {"material.discard",
         "Remove one sparse MI entry and inherit again",
         true,
         {{"object", str}, {"path", str}},
         {"object", "path"}},
        {"material.save", "Save a shared material asset", true, {{"asset", str}}, {"asset"}},
        {"graph.inspect",
         "Use view=catalog to discover SOPs without an open graph. Add type for fields, port_descriptions (wiring requirements by input/output name) and named attribute contracts. topology and node inspect authoring state. A node's controlled_fields maps promoted field paths to graph parameter names. With view=node, bindings=true cooks the target and explains its columns. With explicit node, raycast measures a named polygon output in graph-local metres. full cooks geometry",
         false,
         {{"asset", str},
          {"node", str},
          {"view", Argument::graph_view},
          {"bindings", Argument::boolean},
          {"raycast", Argument::graph_raycast},
          {"type", str},
          {"timings", Argument::boolean},
          {"samples", Argument::integer},
          {"spatial", Argument::boolean},
          {"reference_node", str}},
         {},
         editor::OperatorKind::query},
        {"graph.apply",
         "Atomically edit nodes by name, ordered inputs and graph settings in one undo step. Explicit removals "
         "require existing nodes, including on retry. For a field listed in the node's controlled_fields, use op.call "
         "graph.set_parameter with its public parameter name and a value_json string containing JSON. Both writes "
         "require the latest response state_token.",
         true,
         {{"asset", str}, {"nodes", obj}, {"remove", Argument::array}, {"settings", obj}},
         {}},
        {"graph.create_scratch",
         "Create or open an in-memory scratch procedural graph and enter its SOP viewport context",
         true,
         {{"key", str}},
         {}},
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
         {{"patch", obj}, {"timeout_ms", Argument::integer}},
         {}},
        {"render.capture",
         "Capture at accepted-frame ordinals using current display settings",
         true,
         {{"sample_frames", arr}, {"timeout_ms", Argument::integer}},
         {}},
        {"render.trace",
         "Observe one requested-view recording attempt; export actual post-chain commands and bindings without readback",
         true,
         {{"scope", Argument::trace_scope}, {"timeout_ms", Argument::integer}},
         {}},
        {"render.compare",
         "Capture sequential temporary display cases; settings never enter document/undo",
         true,
         {{"cases", arr}, {"sample_frames", arr}, {"timeout_ms", Argument::integer}},
         {"cases"}},
        {"job.status",
         "Read a bounded asynchronous job; polling does not mutate editor state",
         false,
         {{"job_id", str}},
         {"job_id"},
         editor::OperatorKind::query},
        {"job.cancel",
         "Cancel a job idempotently; temporary display is removed at this boundary",
         false,
         {{"job_id", str}},
         {"job_id"}}};
    return values;
}

json::Result<void> write_range(json::Builder& result, json::Value field, const FieldRange& limits,
                               const TypeInfo& type) {
    if (limits != FieldRange{}) {
        const auto range = result.object_validated(field, "range");
        for (const auto& [name, bound] : {std::pair{"ui_min", &limits.ui_min},
                                          {"ui_max", &limits.ui_max},
                                          {"hard_min", &limits.hard_min},
                                          {"hard_max", &limits.hard_max}}) {
            if (*bound) {
                ATELIER_TRY_BIND(number, scalar_json(**bound, type));
                result.set_validated(range, name, number->root());
            }
        }
    }
    return {};
}

json::Result<void> write_graph_ports(json::Builder& result, json::Value item, const editor::GraphPortInfo& ports) {
    ATELIER_TRY_BIND(inputs, result.array(item, "inputs"));
    for (std::size_t i = 0; i < ports.inputs.size(); ++i) {
        const auto port = result.append_object(*inputs);
        ATELIER_TRY(result.set(port, "name", ports.inputs[i]));
        ATELIER_TRY(result.set(port, "multiple", i < ports.multiple.size() && ports.multiple[i]));
    }
    ATELIER_TRY_BIND(outputs, result.array(item, "outputs"));
    for (const auto& name : ports.outputs)
        ATELIER_TRY(result.append(*outputs, name));
    return {};
}

json::Result<void> write_attribute_rule(json::Builder& result, json::Value item,
                                        const procedural::AttributeRule& rule) {
    ATELIER_TRY(result.set(item, "port", rule.port));
    ATELIER_TRY(result.set(item, "access", procedural::access_name(rule.access)));
    ATELIER_TRY(result.set(item, "type", rule.type));
    if (!rule.name.empty())
        ATELIER_TRY(result.set(item, "name", rule.name));
    if (!rule.selector.empty())
        ATELIER_TRY(result.set(item, "name_from", rule.selector));
    if (!rule.layer.empty())
        ATELIER_TRY(result.set(item, "layer", rule.layer));
    if (!rule.layer_selector.empty())
        ATELIER_TRY(result.set(item, "layer_from", rule.layer_selector));
    else if (rule.layer.empty())
        ATELIER_TRY(result.set(item, "layer_from_column", true));
    if (!rule.when.field.empty()) {
        const auto when = result.object_validated(item, "when");
        ATELIER_TRY(result.set(when, "field", rule.when.field));
        ATELIER_TRY(result.set(when, rule.when.exclude ? "not_equal" : "equals", rule.when.value));
    }
    ATELIER_TRY(result.set(item, "missing", rule.missing));
    if (!rule.purpose.empty())
        ATELIER_TRY(result.set(item, "purpose", rule.purpose));
    if (!rule.unless_present.empty())
        ATELIER_TRY(result.set(item, "unless_present", rule.unless_present));
    if (!rule.needs_present.empty())
        ATELIER_TRY(result.set(item, "needs_present", rule.needs_present));
    return {};
}

json::Result<void> write_graph_type(json::Builder& result, json::Value item,
                                    const editor::GraphNodeCatalogEntry& type) {
    ATELIER_TRY(result.set(item, "name", type.name));
    ATELIER_TRY(result.set(item, "description", type.description));
    ATELIER_TRY(result.set(item, "native", type.native));
    if (!type.asset_key.empty()) {
        ATELIER_TRY(result.set(item, "asset", type.asset_key));
        ATELIER_TRY(result.set(item, "display_label", type.display_label));
    }
    return write_graph_ports(result, item, type.ports);
}

json::Result<void> graph_raycast_schema(json::Builder& result, json::Value item, json::Value args,
                                        json::Value properties) {
    const auto field = result.object_validated(properties, "raycast");
    result.set_validated(field, "type", "object");
    result.set_validated(
        field, "description",
        "Nearest two-sided polygon hits on the current named output. Coordinates and distances are in graph-local metres.");
    ATELIER_TRY(result.set(field, "additionalProperties", false));
    result.set_validated(field, "required", json::Document::literal<R"(["output","rays"])">().root());
    const auto keys = result.object_validated(field, "properties");
    result.set_validated(
        keys, "output",
        json::Document::literal<
            R"({"type":"string","minLength":1,"description":"Exact output port name from the node's topology or type catalog."})">()
            .root());
    const auto rays = result.object_validated(keys, "rays");
    result.set_validated(rays, "type", "array");
    ATELIER_TRY(result.set(rays, "minItems", 1u));
    ATELIER_TRY(result.set(rays, "maxItems", static_cast<unsigned>(graph_ray_limit)));
    const auto ray = result.object_validated(rays, "items");
    result.set_validated(ray, "type", "object");
    ATELIER_TRY(result.set(ray, "additionalProperties", false));
    result.set_validated(ray, "required", json::Document::literal<R"(["origin","direction","max_distance"])">().root());
    const auto coordinates =
        json::Document::literal<
            R"({"type":"array","minItems":3,"maxItems":3,"items":{"type":"number","format":"float"}})">()
            .root();
    const auto components = result.object_validated(ray, "properties");
    result.set_validated(components, "origin", coordinates);
    result.set_validated(
        components, "direction",
        json::Document::literal<
            R"({"type":"array","minItems":3,"maxItems":3,"items":{"type":"number","format":"float"},"description":"Finite nonzero [x,y,z]; normalized by the query."})">()
            .root());
    result.set_validated(
        components, "max_distance",
        json::Document::literal<R"({"type":"number","format":"float","exclusiveMinimum":0,"unit":"m"})">().root());
    result.set_validated(args, "if", json::Document::literal<R"({"required":["raycast"]})">().root());
    result.set_validated(
        args, "then",
        json::Document::literal<
            R"({"required":["node","view"],"properties":{"node":{"minLength":1},"view":{"const":"node"}}})">()
            .root());
    result.set_validated(
        args, "examples",
        json::Document::literal<
            R"([{"view":"node","node":"roof","raycast":{"output":"output","rays":[{"origin":[0,10,0],"direction":[0,-1,0],"max_distance":20}]}}])">()
            .root());
    result.set_validated(item, "raycast_result",
                         json::Document::literal<R"({
        "path":"result.raycast","type":"object","required":["output","hits"],
        "properties":{"output":{"type":"string"},"hits":{"type":"array","description":"One entry per input ray, in order. Null means no polygon hit within max_distance. Hit distances use metres; position is graph-local XYZ; primitive and fan triangle indices are zero-based.",
          "items":{"oneOf":[{"type":"null"},{"type":"object","required":["distance","position","primitive","triangle"],
          "properties":{"distance":{"type":"number","minimum":0,"unit":"m"},"position":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},"primitive":{"type":"integer","minimum":0},"triangle":{"type":"integer","minimum":0}}}]}}}
    })">()
                             .root());
    return {};
}

json::Result<void> append_operation(json::Builder& result, json::Value entries, const Operation& op) {
    const auto item = result.append_object(entries);
    result.set_validated(item, "op", op.name);
    result.set_validated(item, "description", op.description);
    result.set_validated(item, "kind", operator_kind_names[static_cast<std::size_t>(op.kind)]);
    ATELIER_TRY(result.set(item, "requires_state_token", op.changes));
    if (op.name == "graph.apply") {
        const auto examples = result.object_validated(item, "examples");
        result.set_validated(examples, "edit_nodes",
                             json::Document::literal<R"({
            "op":"graph.apply","state_token":"<latest state_token>","args":{"nodes":{
                "roof":{"type":"procedural.Wedge","properties":{"size":{"x":4,"y":2,"z":4}}},
                "place":{"type":"procedural.Transform","inputs":{"input":"roof.output"}}}}})">()
                                 .root());
        result.set_validated(examples, "set_promoted_parameter",
                             json::Document::literal<R"({
            "op":"op.call","state_token":"<latest state_token>",
            "args":{"id":"graph.set_parameter","args":{"parameter":"clear_width","value_json":"5.0"}}})">()
                                 .root());
    }
    const auto args = result.object_validated(item, "args");
    result.set_validated(args, "type", "object");
    const auto properties = result.object_validated(args, "properties");
    for (const auto& [name, kind] : op.properties) {
        if (kind == Argument::graph_raycast) {
            ATELIER_TRY(graph_raycast_schema(result, item, args, properties));
            continue;
        }
        const auto field = result.object_validated(properties, name);
        constexpr std::array types{"string", "object", "array", "integer", "boolean"};
        if (kind <= Argument::boolean)
            result.set_validated(field, "type", types[static_cast<std::size_t>(kind)]);
        else if (kind == Argument::page_limit) {
            result.set_validated(field, "type", "integer");
            ATELIER_TRY(result.set(field, "minimum", 1u));
            ATELIER_TRY(result.set(field, "maximum", inspect_page_items));
        } else {
            ATELIER_TRY_BIND(choices, result.array(field, "enum"));
            static constexpr std::string_view details[]{"full", "summary", "assets", "objects", "object"};
            static constexpr std::string_view views[]{"catalog", "topology", "node", "full"};
            static constexpr std::string_view graph_scope[]{"graph"};
            static constexpr std::string_view trace_scope[]{"scene_lit_post"};
            std::span<const std::string_view> names = trace_scope;
            if (kind == Argument::detail)
                names = details;
            else if (kind == Argument::graph_view)
                names = views;
            else if (kind == Argument::graph_scope)
                names = graph_scope;
            result.set_validated(field, "type", "string");
            for (const auto name : names)
                ATELIER_TRY(result.append(*choices, name));
        }
    }
    ATELIER_TRY_BIND(required, result.array(args, "required"));
    for (const auto& name : op.required) {
        ATELIER_TRY(result.append(*required, name));
    }
    ATELIER_TRY(result.set(args, "additionalProperties", false));
    if (op.name == "op.call") {
        ATELIER_TRY_BIND(kinds, result.array(item, "accepted_operator_kinds"));
        ATELIER_TRY(result.append(*kinds, "direct"));
    }
    return {};
}

json::Result<json::Document> capabilities() {
    json::Builder result;
    const auto root = result.root();
    const auto limits = result.object_validated(root, "limits");
    ATELIER_TRY(result.set(limits, "message_bytes", control::maximum_message));
    ATELIER_TRY(result.set(limits, "inspect_page_items", inspect_page_items));
    ATELIER_TRY(result.set(limits, "inspect_page_bytes", inspect_page_bytes));
    ATELIER_TRY(result.set(limits, "request_history", 256u));
    ATELIER_TRY(result.set(limits, "request_history_bytes", 8u * 1024 * 1024));
    ATELIER_TRY(result.set(limits, "job_history", 16u));
    ATELIER_TRY(result.set(limits, "captures", 16u));
    ATELIER_TRY(result.set(limits, "capture_bytes", 256u * 1024 * 1024));
    ATELIER_TRY(result.set(limits, "timeout_ms", control::maximum_timeout_ms));
    ATELIER_TRY_BIND(ops, result.array(root, "operations"));
    for (const auto& op : operations())
        ATELIER_TRY(append_operation(result, *ops, op));
    const auto options = result.object_validated(root, "display_options");
    for (const auto& option : editor::view_options()) {
        const auto schema = result.object_validated(options, option.name);
        if (option.kind == editor::ViewValueKind::enumeration) {
            result.set_validated(schema, "type", "string");
            ATELIER_TRY_BIND(choices, result.array(schema, "enum"));
            for (auto choice : option.choices) {
                ATELIER_TRY(result.append(*choices, choice));
            }
        } else if (option.kind == editor::ViewValueKind::boolean)
            result.set_validated(schema, "type", "boolean");
        else {
            result.set_validated(schema, "type", option.kind == editor::ViewValueKind::integer ? "integer" : "number");
            ATELIER_TRY(result.set(schema, "minimum", option.minimum));
            ATELIER_TRY(result.set(schema, "maximum", option.maximum));
        }
        if (option.option == editor::ViewOption::grid_plane)
            result.set_validated(schema, "availability", "render.compare only; persistent value follows camera");
        result.set_validated(schema, "scope", editor::world_option(option.option) ? "scene" : "view");
        result.set_validated(schema, "label", option.label);
        result.set_validated(schema, "category", option.category);
        result.set_validated(schema, "description", option.description);
    }
    result.set_validated(root, "probe_volume_stats_semantics",
                         "CPU accepted-frame layout and update schedule; not GPU completion or valid-support counts");
    result.set_validated(root, "coordinates",
                         "metres, left-handed +Y up, row-major matrices, row vectors; TRS rotation is quaternion xyzw");
    result.set_validated(
        root, "history_policy",
        "Renderer histories evolve naturally across cases; no probe reset or GPU-history restoration is promised");
    return result.finish();
}
}

struct EditorControl::Impl {
    Application& app;

    control::Server server;

    std::uint64_t version{1};

    std::uint64_t document_generation{1};

    std::uint64_t input_revision{};

    std::uint64_t next_handle{1};

    struct Observation {
        std::uint64_t document{};

        std::uint64_t session{};

        std::uint64_t input{};

        std::uint64_t project{};

        std::uint64_t code{};

        std::uint64_t view_revision{};

        ui::PixelExtent extent;

        float dpi{};

        editor::CameraState camera;

        renderer::ViewSettings view;

        bool editing{};

        bool running{};

        bool operator==(const Observation&) const = default;
    } observed;

    static constexpr std::array<std::string_view, 12> observation_fields{
        "document", "session", "input",  "project", "code",    "view_revision",
        "extent",   "dpi",     "camera", "view",    "editing", "running"};

    struct ObservationChange {
        std::uint64_t version{};

        std::array<bool, observation_fields.size()> fields{};
    };

    std::array<ObservationChange, 16> observation_changes{};

    std::unordered_map<ObjectId, std::string> handles;

    std::unordered_map<std::string, ObjectId> objects;

    std::uint64_t pruned_revision{};

    struct Cached {
        std::string id;

        Array<char> request;

        json::Document response;

        std::size_t bytes{};
    };

    std::deque<Cached> cache;

    std::size_t cache_bytes{};

    struct Case {
        std::string name;

        renderer::ViewSettings view;
    };

    struct Job {
        std::string id;

        std::string kind;

        std::string token;

        std::shared_ptr<control::Peer> peer;

        Observation context;

        std::shared_ptr<control_artifacts::WriteState> work = std::make_shared<control_artifacts::WriteState>();

        std::chrono::steady_clock::time_point deadline;

        std::chrono::steady_clock::time_point started;

        std::vector<Case> cases;

        std::vector<unsigned> points;

        std::size_t case_index{};

        std::size_t point_index{};

        unsigned accepted{};

        unsigned skipped{};

        ui::Rect bounds;

        std::filesystem::path directory;

        std::vector<control_artifacts::Sample> samples;

        json::Document manifest;

        json::Document response;

        std::future<std::expected<json::Document, Failure>> writer;

        std::unique_ptr<rhi::CommandTrace> trace;

        bool tracing{};

        bool rendering{};

        bool preparing{};

        bool complete{};

        bool recorded{};

        std::uint64_t history_resets{};
    };

    std::deque<std::unique_ptr<Job>> jobs;

    struct AcceptedFrame {
        std::uint64_t id{};

        std::uint64_t count{};

        ui::Rect bounds;

        ui::PixelExtent extent;

        renderer::Camera camera;

        renderer::ViewSettings view;

        math::Vector3 environment;

        math::Vector3 lighting_focus;

        renderer::FrameStats stats;
    };

    std::optional<AcceptedFrame> last_frame;

    json::Result<json::Document> frame_json() const {
        if (!last_frame)
            return json::Document{};
        const auto& f = *last_frame;
        const auto& s = f.stats;
        json::Builder result;
        const auto root = result.root();
        ATELIER_TRY(result.set(root, "frame_id", f.id));
        ATELIER_TRY(result.set(root, "accepted_count", f.count));
        const std::array viewport{f.bounds.x, f.bounds.y, f.bounds.width, f.bounds.height};
        const std::array extent{f.extent.width, f.extent.height};
        ATELIER_TRY(result.numbers(root, "viewport", std::span{viewport}));
        ATELIER_TRY(result.numbers(root, "extent", std::span{extent}));
        ATELIER_TRY(numbers(result, root, "environment", f.environment));
        ATELIER_TRY(numbers(result, root, "lighting_focus", f.lighting_focus));
        const auto camera = result.object_validated(root, "camera");
        ATELIER_TRY(result.numbers(camera, "view", std::span{f.camera.view}));
        ATELIER_TRY(result.numbers(camera, "projection", std::span{f.camera.projection}));
        ATELIER_TRY(result.set(camera, "far_depth", f.camera.far_depth));
        result.set_validated(camera, "projection_convention", "editor_unjittered");
        ATELIER_TRY_BIND(view, view_json(f.view));
        result.set_validated(root, "effective_view", view->root());
        const auto stats = result.object_validated(root, "frame_stats");
        ATELIER_TRY(result.set(stats, "scene_draw_calls", s.scene_draw_calls));
        ATELIER_TRY(result.set(stats, "scene_instances", s.scene_instances));
        ATELIER_TRY(result.set(stats, "scene_culled_instances", s.scene_culled_instances));
        ATELIER_TRY(result.set(stats, "history_resets", s.scene_lit_history_resets));
        ATELIER_TRY(result.set(stats, "light_sources", s.scene_lit_light_sources));
        ATELIER_TRY(result.set(stats, "shadow_sources", s.scene_lit_shadow_sources));
        const auto spatial = result.object_validated(stats, "spatial_light_sampling");
        ATELIER_TRY(result.set(spatial, "retained_sources", s.scene_lit_spatial_sources));
        ATELIER_TRY(result.set(spatial, "nonempty_cells", s.scene_lit_spatial_cells));
        ATELIER_TRY(result.set(spatial, "resource_bytes", s.scene_lit_spatial_bytes));
        ATELIER_TRY(result.set(spatial, "uploaded", s.scene_lit_spatial_changed));
        ATELIER_TRY_BIND(volumes, probe_volumes_json(s.scene_lit_probe_volumes));
        result.set_validated(stats, "probe_volumes", volumes->root());
        result.set_validated(stats, "probe_volumes_source", "cpu_layout_and_schedule");
        ATELIER_TRY_BIND(inspection, probe_inspection_json(s.scene_lit_probe_inspection));
        result.set_validated(stats, "probe_inspection", inspection->root());
        const auto gpu = result.object_validated(root, "gpu_sample");
        ATELIER_TRY(result.set(gpu, "valid", s.scene_lit_gpu_timing_valid));
        ATELIER_TRY(result.set(gpu, "frame_id", s.scene_lit_gpu_sample_frame_id));
        ATELIER_TRY(result.set(gpu, "sequence", s.scene_lit_gpu_sample_sequence));
        ATELIER_TRY(result.numbers(gpu, "pass_ms", std::span{s.scene_lit_gpu_ms}));
        ATELIER_TRY(result.numbers(gpu, "ddgi_pass_ms", std::span{s.scene_lit_ddgi_gpu_ms}));
        ATELIER_TRY(result.numbers(gpu, "ddgi_update_pass_ms", std::span{s.scene_lit_ddgi_update_gpu_ms}));
        ATELIER_TRY(result.set(gpu, "direct_cluster_ms", s.scene_lit_direct_cluster_gpu_ms));
        ATELIER_TRY(result.set(gpu, "direct_shading_ms", s.scene_lit_direct_shading_gpu_ms));
        ATELIER_TRY(result.set(gpu, "shadow_cache_ms", s.scene_lit_shadow_cache_gpu_ms));
        const auto shadow = result.object_validated(gpu, "shadow_cache");
        ATELIER_TRY(result.set(shadow, "faces", s.scene_lit_shadow_cache_faces));
        ATELIER_TRY(result.set(shadow, "reused_faces", s.scene_lit_shadow_cache_reused_faces));
        ATELIER_TRY(result.set(shadow, "static_updates", s.scene_lit_shadow_cache_static_updates));
        ATELIER_TRY(result.set(shadow, "dynamic_updates", s.scene_lit_shadow_cache_dynamic_updates));
        ATELIER_TRY(result.set(shadow, "draws", s.scene_lit_shadow_cache_draws));
        ATELIER_TRY(result.set(shadow, "resolution_downgrades", s.scene_lit_shadow_cache_resolution_downgrades));
        ATELIER_TRY(result.set(shadow, "dropped_lights", s.scene_lit_shadow_cache_dropped_lights));
        ATELIER_TRY(result.set(shadow, "near_plane_faces", s.scene_lit_shadow_cache_near_plane_faces));
        ATELIER_TRY(result.set(shadow, "missing_taps", s.scene_lit_shadow_cache_missing_taps));
        ATELIER_TRY(result.set(shadow, "plane_fallbacks", s.scene_lit_shadow_cache_plane_fallbacks));
        ATELIER_TRY(result.set(shadow, "deferred_faces", s.scene_lit_shadow_cache_deferred_faces));
        ATELIER_TRY(result.set(shadow, "deferral_frames", s.scene_lit_shadow_cache_deferral_frames));
        ATELIER_TRY(result.set(shadow, "atlas_cells", s.scene_lit_shadow_cache_atlas_cells));
        ATELIER_TRY(result.set(shadow, "pending_lights", s.scene_lit_shadow_cache_pending_lights));
        const auto cluster = result.object_validated(gpu, "direct_cluster");
        ATELIER_TRY(result.set(cluster, "cells", s.scene_lit_direct_cluster_cells));
        ATELIER_TRY(result.set(cluster, "overflows", s.scene_lit_direct_cluster_overflows));
        ATELIER_TRY(result.set(cluster, "max_sources", s.scene_lit_direct_cluster_max_sources));
        ATELIER_TRY(result.numbers(cluster, "receiver_histogram", std::span{s.scene_lit_direct_receiver_histogram}));
        ATELIER_TRY(result.set(cluster, "receiver_max_sources", s.scene_lit_direct_receiver_max_sources));
        ATELIER_TRY(result.set(gpu, "pass_mask", s.scene_lit_gpu_pass_mask));
        ATELIER_TRY(result.numbers(gpu, "ddgi_visibility_rays", std::span{s.scene_lit_ddgi_visibility_rays}));
        ATELIER_TRY(result.set(gpu, "resource_bytes", s.scene_lit_resource_bytes));
        result.set_validated(gpu, "resource_bytes_source", "current_cpu_logical");
        ATELIER_TRY(result.set(gpu, "shadow_rays", s.scene_lit_shadow_rays));
        ATELIER_TRY(result.set(gpu, "probe_rays", s.scene_lit_probe_rays));
        return result.finish();
    }

    Impl(Application& owner, control::Server transport) : app(owner), server(std::move(transport)) { observe(); }

    ~Impl() {
        server.stop();
        for (auto& job : jobs) {
            job->work->cancel();
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
            observation_changes[(version + 1) % observation_changes.size()] = {
                version + 1,
                {current.document != observed.document, current.session != observed.session,
                 current.input != observed.input, current.project != observed.project, current.code != observed.code,
                 current.view_revision != observed.view_revision, current.extent != observed.extent,
                 current.dpi != observed.dpi, current.camera != observed.camera, current.view != observed.view,
                 current.editing != observed.editing, current.running != observed.running}};
            observed = current;
            ++version;
        }
    }

    std::string token() const {
        return std::string(*server.identity().root().string({"instance_id"})) + ":" + std::to_string(version);
    }

    std::string stale_message(std::string_view client_token, std::string_view op, json::Value args) const {
        std::string message = "Inspect the editor again before changing its state; op=" + std::string(op);
        if (op == "op.call") {
            const auto value = args.string({"id"});
            message += " id=";
            message += value && value->size() <= 80 &&
                               std::ranges::all_of(*value, [](unsigned char c) { return c >= 0x20 && c <= 0x7e; })
                           ? *value
                           : "<invalid id>";
        }
        message += "; current_version=" + std::to_string(version);
        const auto separator = client_token.rfind(':');
        if (separator == client_token.npos)
            return message + "; client_version=invalid; missing or invalid state_token";
        const auto number = client_token.substr(separator + 1);
        std::uint64_t client_version{};
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), client_version);
        const bool valid = parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size();
        message += "; client_version=" + (valid ? std::to_string(client_version) : "invalid");
        if (client_token.substr(0, separator) != *server.identity().root().string({"instance_id"}))
            return message + "; state_token instance differs";
        if (!valid)
            return message + "; invalid state_token version";
        const auto first = version > observation_changes.size() ? version - observation_changes.size() + 1 : 2;
        if (client_version < first - 1)
            message += "; change history truncated before version " + std::to_string(first);
        message += "; changes=";
        bool any{};
        for (auto next = first; next <= version; ++next) {
            if (next <= client_version)
                continue;
            const auto& change = observation_changes[next % observation_changes.size()];
            message += (any ? "; " : "") + std::to_string(change.version) + ":[";
            bool field{};
            for (std::size_t i = 0; i < change.fields.size(); ++i)
                if (change.fields[i]) {
                    if (field)
                        message += ',';
                    message += observation_fields[i];
                    field = true;
                }
            message += ']';
            any = true;
        }
        if (!any)
            message += "none (token differs without a recorded version change)";
        return message;
    }

    auto busy_states() const {
        const auto ui = app.ui_.interaction_state();
        using State = std::pair<std::string_view, bool>;
        return std::array{State{"document_edit", app.document_->editing()},
                          State{"viewport_tool", app.viewport_tool_.active()},
                          State{"network_gesture", app.workspace_.network.active_gesture()},
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

    json::Result<json::Document> busy_reasons() const {
        json::Builder reasons(json::Kind::array);
        for (const auto& [name, active] : busy_states()) {
            if (active) {
                ATELIER_TRY(reasons.append(reasons.root(), name));
            }
        }
        return reasons.finish();
    }

    Failure operation_error(const Error& error) const {
        app.report_error(error, "editor.control_failed");
        return operation_failure(error);
    }

    template <class T>
    std::expected<T, Failure> operation(Result<T> result) const {
        return std::move(result).transform_error([this](const Error& error) { return operation_error(error); });
    }

    std::expected<json::Document, Failure> camera_json() const {
        auto& runtime = app.document_->world().runtime();
        auto camera = app.camera_.state();
        ATELIER_TRY_BIND(type, operation(runtime.native_type<editor::CameraState>()));
        ATELIER_TRY_BIND(fields, operation(editor::detail::author_fields(runtime, *type)));
        return operation(editor::encode_properties(runtime, {*type, {}, std::as_writable_bytes(std::span{&camera, 1})},
                                                   *fields, {}));
    }

    std::expected<renderer::ViewSettings, Failure> patched_view(renderer::ViewSettings view, json::Value patch,
                                                                math::Vector3 focus) {
        ATELIER_TRY_BIND(items, patch.object_items());
        for (const auto& [name, value] : *items) {
            if (std::ranges::find(removed_view_options, name) != removed_view_options.end())
                return std::unexpected(Failure(
                    "invalid_request", "Display option was removed: " + std::string(name) +
                                           "; direct light always uses clustered light lists with cached shadows"));
            const auto* option = editor::view_option(name);
            if (!option)
                return std::unexpected(Failure("invalid_request", "Unknown display option: " + std::string(name)));
            double scalar{};
            if (option->kind == editor::ViewValueKind::enumeration) {
                ATELIER_TRY_BIND(choice, text(value));
                const auto found = std::ranges::find(option->choices, *choice);
                if (found == option->choices.end())
                    return std::unexpected(Failure("invalid_request", "Unsupported display choice: " + *choice));
                scalar = static_cast<double>(found - option->choices.begin());
            } else if (option->kind == editor::ViewValueKind::boolean) {
                ATELIER_TRY_BIND(toggle, value.boolean());
                scalar = *toggle ? 1 : 0;
            } else {
                ATELIER_TRY_BIND(n, value.number());
                scalar = *n;
            }
            ATELIER_TRY(operation(editor::set_view_option(view, option->option, scalar)));
        }
        ATELIER_TRY(operation(editor::validate_view_options(view, focus)));
        return view;
    }

    std::expected<void, Failure> idle() const {
        if (busy())
            return std::unexpected(Failure("busy", "Finish the current text, menu or viewport interaction first"));
        return {};
    }

    std::expected<void, Failure> replaceable() const {
        ATELIER_TRY(idle());
        if (app.document_->dirty())
            return std::unexpected(Failure("unsaved_changes", "Save the current scene before replacing it"));
        if (!app.document_->dirty_materials().empty())
            return std::unexpected(
                Failure("unsaved_changes", "Save the open material assets before replacing the document"));
        if (!app.document_->dirty_graphs().empty())
            return std::unexpected(
                Failure("unsaved_changes", "Save the open procedural graph assets before replacing the document"));
        for (const auto& file : app.workspace_.source_editor.files())
            if (file->dirty())
                return std::unexpected(Failure("unsaved_changes", "Source drafts are still open"));
        if (app.play_.running())
            return std::unexpected(Failure("busy", "Stop the project before replacing its document"));
        return {};
    }

    Job* active() {
        for (auto& job : jobs)
            if (!job->complete)
                return job.get();
        return nullptr;
    }

    // Codes are application-owned UTF-8 identifiers; messages may be raw diagnostics.
    void cancel(Job& job, std::string_view code, std::string_view message) {
        if (!job.complete && job.work->cancel())
            fail(job, code, message);
    }

    void fail(Job& job, std::string_view code, std::string_view message) {
        json::Builder response;
        response.set_validated(response.root(), "status", "error");
        const auto error = response.object_validated(response.root(), "error");
        response.set_validated(error, "code", code);
        response.set_validated(error, "message", diagnostic_utf8(message));
        job.rendering = false;
        job.preparing = false;
        job.tracing = false;
        job.trace.reset();
        response.set_validated(response.root(), "job_id", job.id);
        job.response = response.finish();
        job.samples.clear();
        if (!job.writer.valid())
            job.complete = true;
    }

    static void start_writer(Job& job, auto write) noexcept {
        job.writer = std::async(std::launch::async, [work = job.work, write = std::move(write)]() mutable noexcept {
            auto result = write(*work);
            if (!result)
                work->claim_outcome();
            return result;
        });
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
                json::Builder response(job->response);
                response.set_validated(response.root(), "state_token", token());
                response.set_validated(response.root(), "job_id", job->id);
                job->response = response.finish();
                for (auto& entry : cache)
                    if (entry.id == job->id) {
                        cache_bytes -= entry.bytes;
                        entry.response = job->response;
                        entry.bytes = entry.request.size() + json::write(entry.response.root()).size();
                        cache_bytes += entry.bytes;
                        break;
                    }
            }
        trim_cache();
    }

    json::Result<void> checkpoint() {
        observe();
        if (auto* job = active()) {
            if (!job->peer->connected.load()) {
                cancel(*job, "cancelled", "Control connection ended");
            } else if (std::chrono::steady_clock::now() >= job->deadline) {
                cancel(*job, "timeout", "Control job deadline exceeded");
            } else if ((job->rendering || job->tracing) && (job->token != token() || busy())) {
                cancel(*job, "state_changed", "Editor changed during the render experiment");
                json::Builder response(job->response);
                const auto error = *response.root().at("error");
                ATELIER_TRY_BIND(changes, response.array(error, "changes"));
                const auto& before = job->context;
                if (before.document != observed.document) {
                    ATELIER_TRY(response.append(*changes, "document"));
                }
                if (before.session != observed.session) {
                    ATELIER_TRY(response.append(*changes, "scene_or_selection"));
                }
                if (before.input != observed.input) {
                    ATELIER_TRY(response.append(*changes, "user_input"));
                }
                if (before.extent != observed.extent || before.dpi != observed.dpi) {
                    ATELIER_TRY(response.append(*changes, "surface"));
                }
                if (before.project != observed.project) {
                    ATELIER_TRY(response.append(*changes, "project_files"));
                }
                if (before.code != observed.code) {
                    ATELIER_TRY(response.append(*changes, "published_code"));
                }
                if (before.camera != observed.camera) {
                    ATELIER_TRY(response.append(*changes, "camera"));
                }
                if (before.view != observed.view || before.view_revision != observed.view_revision) {
                    ATELIER_TRY(response.append(*changes, "display_preferences"));
                }
                if (before.running != observed.running) {
                    ATELIER_TRY(response.append(*changes, "play_state"));
                }
                if (busy()) {
                    ATELIER_TRY(response.append(*changes, "active_interaction"));
                }
                const std::array previous{before.extent.width, before.extent.height};
                const std::array current{observed.extent.width, observed.extent.height};
                ATELIER_TRY(response.numbers(error, "previous_extent", std::span{previous}));
                ATELIER_TRY(response.numbers(error, "current_extent", std::span{current}));
                job->response = response.finish();
            }
            if (job->writer.valid() && job->writer.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result = job->writer.get();
                if (!job->work->cancelled()) {
                    if (result) {
                        json::Builder response;
                        response.set_validated(response.root(), "status", "ok");
                        response.set_validated(response.root(), "result", result->root());
                        job->response = response.finish();
                    } else {
                        fail(*job, result.error().code, result.error().message);
                    }
                }
                job->complete = true;
            }
        }
        record_completions();
        return {};
    }

    bool check_jobs() {
        const auto checked = checkpoint();
        if (!checked)
            app.report_error({ErrorCode::invalid_definition, checked.error().describe()}, "editor.control_failed");
        return checked.has_value();
    }

    std::expected<std::string_view, Failure> handle(ObjectId object) {
        if (object == ObjectId{})
            return std::string_view{};
        if (handles.size() > editor::Session::maximum_entities + editor::Session::maximum_components &&
            pruned_revision != app.document_->revision()) {
            ATELIER_TRY_BIND(entities, operation(app.document_->world().members()));
            ATELIER_TRY(prune_handles(*entities));
        }
        auto [found, added] = handles.try_emplace(object);
        if (added) {
            found->second = std::to_string(document_generation) + "/" + std::to_string(next_handle++);
            objects.emplace(found->second, object);
        }
        return found->second;
    }

    std::expected<void, Failure> set_handle(json::Builder& output, json::Value parent, std::string_view key,
                                            ObjectId id) {
        ATELIER_TRY_BIND(value, handle(id));
        output.set_validated(parent, key, *value);
        return {};
    }

    // The directory contains scene members, author objects, and materials bound to live components.
    Result<bool> any_document_member(std::span<const world::Member> entities, const auto& matches) const {
        const auto& doc = *app.document_;
        const auto& world = doc.world();
        auto& runtime = world.runtime();
        for (const auto& entity : entities) {
            if (matches(entity.id()))
                return true;
            ATELIER_TRY_BIND(components, world::components(runtime, entity.id()));
            for (const auto& component : *components)
                if (matches(component.id()))
                    return true;
        }
        if (std::ranges::any_of(doc.material_objects(), matches) || std::ranges::any_of(doc.graph_objects(), matches))
            return true;
        const auto& types = doc.presentation_types();
        for (const auto type : {types.static_mesh_component, types.procedural_generator_component}) {
            ATELIER_TRY_BIND(owners, world.query(type));
            for (const auto owner : *owners) {
                ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime, owner));
                for (const auto& binding : *bindings)
                    if (matches(binding.resource.id()))
                        return true;
            }
        }
        return false;
    }

    std::expected<void, Failure> prune_handles(std::span<const world::Member> entities) {
        if (!handles.empty()) {
            std::unordered_set<ObjectId> live;
            ATELIER_TRY(operation(any_document_member(entities, [&](ObjectId id) {
                live.insert(id);
                return false;
            })));
            std::erase_if(handles, [&](const auto& entry) {
                if (live.contains(entry.first))
                    return false;
                objects.erase(entry.second);
                return true;
            });
        }
        pruned_revision = app.document_->revision();
        return {};
    }

    std::expected<ObjectId, Failure> object(json::Value value) const {
        ATELIER_TRY_BIND(name, text(value, 80));
        const auto found = objects.find(*name);
        if (found != objects.end()) {
            ATELIER_TRY_BIND(entities, operation(app.document_->world().members()));
            ATELIER_TRY_BIND(
                member, operation(any_document_member(*entities, [&](ObjectId id) { return id == found->second; })));
            if (*member)
                return found->second;
        }
        return std::unexpected(Failure("stale_object", "Object handle is no longer a document member"));
    }

    std::expected<void, Failure> write_object_header(json::Builder& header, json::Value root, ObjectId id,
                                                     std::string_view type) {
        ATELIER_TRY_BIND(object_handle, handle(id));
        ATELIER_TRY_BIND(name, utf8_text(app.document_->name(id)));
        header.set_validated(root, "handle", *object_handle);
        header.set_validated(root, "name", *name);
        ATELIER_TRY(header.set(root, "type", type));
        // A placeholder stands in for a saved type the current build does not register.
        if (const auto saved_type = app.document_->placeholder_type(id); !saved_type.empty()) {
            ATELIER_TRY(header.set(root, "placeholder", true));
            ATELIER_TRY(header.set(root, "saved_type", saved_type));
        }
        return {};
    }

    std::expected<void, Failure> write_object(json::Builder& result, json::Value root, ObjectId id) {
        const auto& doc = *app.document_;
        auto& runtime = doc.world().runtime();
        ATELIER_TRY_BIND(type_id, operation(runtime.type_of(id)));
        ATELIER_TRY_BIND(type, operation(runtime.describe(*type_id)));
        ATELIER_TRY(write_object_header(result, root, id, (*type)->name));
        ATELIER_TRY(result.set(root, "type_description", (*type)->description));
        ATELIER_TRY_BIND(properties, result.array(root, "properties"));
        ATELIER_TRY_BIND(is_material, operation(doc.is_material_type(*type_id)));
        if (*is_material) {
            ATELIER_TRY_BIND(view, operation(doc.inspect_material(id)));
            ATELIER_TRY(result.set(root, "asset", view->asset_key));
            ATELIER_TRY(result.set(root, "parent", view->parent_key));
            result.set_validated(root, "role", view->source ? "source" : "instance");
            ATELIER_TRY(result.set(root, "scene", view->scene));
            ATELIER_TRY(result.set(root, "default_material", view->default_material));
            ATELIER_TRY(result.set(root, "dirty", view->scene ? doc.dirty() : doc.material_dirty(view->asset_key)));
            ATELIER_TRY_BIND(entries, assets::material_format::encode_entries(view->entries));
            result.set_validated(root, "entries", entries->root());
            ATELIER_TRY_BIND(declarations, result.array(root, "declarations"));
            ATELIER_TRY_BIND(diagnostics, result.array(root, "diagnostics"));
            for (const auto& declaration : view->declarations) {
                const std::array values{material::Override{declaration.path, declaration.value, true},
                                        material::Override{declaration.path, declaration.initial, true}};
                ATELIER_TRY_BIND(encoded, assets::material_format::encode_entries(std::span(values).first(1)));
                ATELIER_TRY_BIND(initial, assets::material_format::encode_entries(std::span(values).last(1)));
                result.append(*declarations, *encoded->root().at(0));
                const auto field = *declarations->at(declarations->size() - 1);
                result.erase(field, "enabled");
                result.set_validated(field, "initial", *initial->root().at(0)->at("value"));
                ATELIER_TRY(result.set(field, "components", declaration.components));
            }
            for (const auto& diagnostic : view->diagnostics) {
                const auto item = result.append_object(*diagnostics);
                ATELIER_TRY(result.set(item, "path", diagnostic.path));
                ATELIER_TRY(result.set(item, "message", diagnostic.message));
            }
            if (view->source_file) {
                const auto source = result.object_validated(root, "source");
                ATELIER_TRY(result.set(source, "path", view->source_file->path));
                ATELIER_TRY(result.set(source, "version", view->source_file->version));
                ATELIER_TRY(result.set(source, "text", view->source_file->source));
            }
            return {};
        }
        if (doc.is_graph_object(id)) {
            if (const auto graph_key = doc.graph_of_object(id))
                ATELIER_TRY(result.set(root, "graph", *graph_key));
            if (const auto node_name = doc.graph_node_name(id))
                ATELIER_TRY(result.set(root, "node_name", *node_name));
        }
        ATELIER_TRY_BIND(fields, operation(doc.properties(id)));
        for (const auto& property : *fields) {
            ATELIER_TRY_BIND(prop_type, operation(runtime.describe(property.type)));
            const auto field = result.append_object(*properties);
            ATELIER_TRY(result.set(field, "name", property.name));
            ATELIER_TRY(result.set(field, "label", property.label));
            ATELIER_TRY(result.set(field, "category", property.category));
            ATELIER_TRY(result.set(field, "description", property.description));
            ATELIER_TRY(result.set(field, "type", (*prop_type)->name));
            if (property.is_string) {
                ATELIER_TRY(result.set(field, "value", property.string_value));
            } else {
                ATELIER_TRY_BIND(value, scalar_json(property.value, **prop_type));
                result.set_validated(field, "value", value->root());
            }
            if ((*prop_type)->enumeration) {
                const auto& enum_info = *(*prop_type)->enumeration;
                ATELIER_TRY_BIND(choices, result.array(field, "choices"));
                for (const auto& [name, bits] : enum_info.values) {
                    static_cast<void>(bits);
                    ATELIER_TRY(result.append(*choices, name));
                }
                if (enum_info.flags)
                    ATELIER_TRY(result.set(field, "flags", true));
                if (enum_info.flags || std::ranges::any_of(enum_info.metadata, [](const auto& m) {
                        return !m.label.empty() || !m.description.empty();
                    })) {
                    ATELIER_TRY_BIND(enumerators, result.array(field, "enumerators"));
                    for (std::size_t i = 0; i < enum_info.values.size(); ++i) {
                        const auto item = result.append_object(*enumerators);
                        ATELIER_TRY(result.set(item, "name", enum_info.values[i].first));
                        ATELIER_TRY(result.set(item, "value", static_cast<std::uint64_t>(enum_info.values[i].second)));
                        ATELIER_TRY(result.set(item, "label", enum_info.metadata[i].label));
                        ATELIER_TRY(result.set(item, "description", enum_info.metadata[i].description));
                    }
                }
            }
            if (property.transient)
                ATELIER_TRY(result.set(field, "transient", true));
            if (property.read_only)
                ATELIER_TRY(result.set(field, "read_only", true));
            ATELIER_TRY(write_range(result, field, property.range, **prop_type));
        }
        ATELIER_TRY_BIND(saved, operation(doc.retained(id)));
        if (!saved->entries.empty()) {
            ATELIER_TRY_BIND(data, result.array(root, "saved_data"));
            for (const auto& entry : saved->entries) {
                std::string path;
                for (const auto& part : entry.path)
                    path += (path.empty() ? "" : ".") + part;
                const auto item = result.append_object(*data);
                ATELIER_TRY(result.set(item, "path", path));
                ATELIER_TRY(result.set(item, "value", entry.value));
                ATELIER_TRY(result.set(item, "kind", world::name(entry.kind)));
            }
        }
        ATELIER_TRY_BIND(entity, operation(runtime.is_a(*type_id, doc.types().entity)));
        if (*entity) {
            ATELIER_TRY_BIND(origin, operation(world::space_reference(runtime, id)));
            if (*origin)
                ATELIER_TRY(set_handle(result, root, "space_reference", **origin));
            else
                ATELIER_TRY(result.set_null(root, "space_reference"));
            ATELIER_TRY_BIND(spatial, operation(world::has_transform(runtime, id)));
            if (*spatial) {
                ATELIER_TRY_BIND(transform, operation(world::read_transform(runtime, id)));
                const auto field = result.object_validated(root, "transform");
                const std::array position{transform->position.x, transform->position.y, transform->position.z};
                const std::array rotation{transform->rotation.x, transform->rotation.y, transform->rotation.z,
                                          transform->rotation.w};
                const std::array scale{transform->scale.x, transform->scale.y, transform->scale.z};
                ATELIER_TRY(result.numbers(field, "position", std::span{position}));
                ATELIER_TRY(result.numbers(field, "rotation", std::span{rotation}));
                ATELIER_TRY(result.numbers(field, "scale", std::span{scale}));
            }
            return {};
        }
        const auto& types = doc.presentation_types();
        ATELIER_TRY_BIND(static_mesh, operation(runtime.is_a(*type_id, types.static_mesh_component)));
        ATELIER_TRY_BIND(generator, operation(runtime.is_a(*type_id, types.procedural_generator_component)));
        if (*static_mesh || *generator) {
            ATELIER_TRY_BIND(bindings, operation(presentation::read_material_bindings(runtime, id)));
            if (*static_mesh) {
                ATELIER_TRY_BIND(mesh, operation(presentation::read_static_mesh(runtime, id)));
                const auto mesh_record = result.object_validated(root, "mesh");
                ATELIER_TRY(result.set(mesh_record, "source", mesh->mesh.source));
                ATELIER_TRY(result.set(mesh_record, "missing", !mesh->mesh.resolved && !mesh->mesh.source.empty()));
                ATELIER_TRY_BIND(defaults, result.array(mesh_record, "default_materials"));
                for (const auto& binding : mesh->mesh.defaults) {
                    ATELIER_TRY_BIND(source, operation(material::material_key(runtime, binding.resource.id())));
                    const auto item = result.append_object(*defaults);
                    ATELIER_TRY(result.set(item, "slot", binding.slot));
                    ATELIER_TRY(set_handle(result, item, "object", binding.resource.id()));
                    ATELIER_TRY(result.set(item, "source", *source));
                    ATELIER_TRY(
                        result.set(item, "missing", doc.unavailable(assets::AssetKind::material, *source) != nullptr));
                }
            } else {
                ATELIER_TRY_BIND(proc, operation(presentation::read_procedural_source(runtime, id)));
                const auto proc_record = result.object_validated(root, "procedural");
                ATELIER_TRY(result.set(proc_record, "graph", proc->source));
                ATELIER_TRY(
                    result.set(proc_record, "missing",
                               !proc->source.empty() && !doc.has_graph(proc->source) &&
                                   doc.unavailable(assets::AssetKind::procedural_graph, proc->source) != nullptr));
                ATELIER_TRY(
                    result.set(proc_record, "has_surface", proc->output && proc->output->surface_mesh != nullptr));
                std::uint64_t instance_count = 0;
                if (proc->output)
                    for (const auto& col : proc->output->publication.collections)
                        if (col.transforms)
                            instance_count += col.transforms->size();
                ATELIER_TRY(result.set(proc_record, "instance_count", instance_count));
                const auto ov_doc = app.document_->generator_overrides(id);
                if (ov_doc.root().kind() == json::Kind::object && ov_doc.root().size() > 0)
                    result.set_validated(proc_record, "overrides", ov_doc.root());
            }
            ATELIER_TRY_BIND(materials, result.array(root, "materials"));
            for (const auto& binding : *bindings) {
                ATELIER_TRY_BIND(view, operation(doc.inspect_material(binding.resource.id())));
                const auto item = result.append_object(*materials);
                ATELIER_TRY(result.set(item, "slot", binding.slot));
                ATELIER_TRY(set_handle(result, item, "object", binding.resource.id()));
                ATELIER_TRY(result.set(item, "asset", view->asset_key));
                ATELIER_TRY(result.set(item, "parent", view->parent_key));
                ATELIER_TRY(result.set(item, "scene", view->scene));
                ATELIER_TRY(result.set(item, "assignment_override", binding.overridden));
                ATELIER_TRY(result.set(item, "default_material", view->default_material));
            }
        }
        return {};
    }

    // Full and paged inspection share the existing builtin-then-project entry contract.
    std::expected<json::Document, Failure> asset_json(std::size_t index) const {
        json::Builder result;
        const auto root = result.root();
        const auto builtins = assets::mesh_assets();
        if (index < builtins.size()) {
            const auto& asset = builtins[index];
            ATELIER_TRY_BIND(name, utf8_text(asset.name));
            ATELIER_TRY(result.set(root, "key", asset.key));
            result.set_validated(root, "name", *name);
            result.set_validated(root, "kind", "mesh");
            return result.finish();
        }
        const auto& entry = app.project_->entries()[index - builtins.size()];
        ATELIER_TRY_BIND(path, utf8_text(entry.path.generic_wstring()));
        ATELIER_TRY(result.set(root, "key", entry.key));
        result.set_validated(root, "path", *path);
        ATELIER_TRY(result.set(root, "kind", static_cast<unsigned>(entry.kind)));
        return result.finish();
    }

    enum class Directory { objects, assets };

    struct InspectionPage {
        Directory directory;
        std::string_view instance;
        std::uint64_t generation;
        std::uint64_t revision;
        unsigned limit;
        std::optional<json::Value> cursor;
        json::Builder result{};
        std::size_t remaining = inspect_page_bytes;

        std::string_view revision_key() const {
            return directory == Directory::assets ? "project_revision" : "revision";
        }

        std::expected<void, Failure> check_version(std::string_view saved_instance, std::uint64_t saved_generation,
                                                   std::uint64_t saved_revision) const {
            if (saved_instance != instance || saved_generation != generation || saved_revision != revision)
                return std::unexpected(
                    Failure("stale_cursor", directory == Directory::assets
                                                ? "The asset directory changed; restart without a cursor"
                                                : "The object directory changed; restart without a cursor"));
            return {};
        }

        json::Result<json::Value> begin() {
            const auto root = result.root();
            const auto detail = directory == Directory::assets ? "assets" : "objects";
            result.set_validated(root, "detail", detail);
            ATELIER_TRY(result.set(root, "document_generation", generation));
            ATELIER_TRY(result.set(root, revision_key(), revision));
            ATELIER_TRY(result.set_null(root, "next_cursor"));
            return result.array(root, detail);
        }

        std::expected<bool, Failure> append(json::Value entries, json::Value entry) {
            const auto size = json::write(entry).size() + 1;
            if (size > inspect_page_bytes)
                return std::unexpected(
                    Failure("result_too_large", directory == Directory::assets
                                                    ? "One asset directory entry exceeds the page byte limit"
                                                    : "One object directory entry exceeds the page byte limit"));
            if (entries.size() == limit || size > remaining)
                return false;
            result.append(entries, entry);
            remaining -= size;
            return true;
        }

        json::Result<json::Value> next_cursor() {
            const auto next = result.object_validated(result.root(), "next_cursor");
            result.set_validated(next, "instance", instance);
            ATELIER_TRY(result.set(next, "document", generation));
            ATELIER_TRY(result.set(next, revision_key(), revision));
            return next;
        }
    };

    std::expected<InspectionPage, Failure> inspection_page(Directory directory, json::Value args) const {
        if (directory == Directory::assets) {
            ATELIER_TRY(args.allowed_members({"detail", "limit", "cursor"}));
        } else {
            ATELIER_TRY(args.allowed_members({"detail", "entity_name", "type", "limit", "cursor"}));
        }
        unsigned limit = 64;
        ATELIER_TRY_BIND(limit_arg, args.find("limit"));
        if (*limit_arg) {
            ATELIER_TRY_BIND(n, integer(**limit_arg, 1, inspect_page_items));
            limit = *n;
        }
        ATELIER_TRY_BIND(cursor, args.find("cursor"));
        const auto revision =
            directory == Directory::assets ? (app.project_ ? app.project_->revision() : 0) : app.document_->revision();
        return InspectionPage{
            directory, *server.identity().root().string({"instance_id"}), document_generation, revision, limit,
            *cursor};
    }

    std::expected<json::Document, Failure> inspect_assets(json::Value args) {
        ATELIER_TRY_BIND(page, inspection_page(Directory::assets, args));
        const auto count = assets::mesh_assets().size() + (app.project_ ? app.project_->entries().size() : 0);
        std::size_t index{};
        if (page->cursor) {
            const auto cursor = *page->cursor;
            ATELIER_TRY(cursor.members({"domain", "instance", "document", "project_revision", "index"}));
            ATELIER_TRY_BIND(domain, cursor.string({"domain"}));
            if (*domain != "assets")
                return std::unexpected(Failure("invalid_request", "Expected an assets cursor"));
            ATELIER_TRY_BIND(document, cursor.u64({"document"}));
            ATELIER_TRY_BIND(project_revision, cursor.u64({"project_revision"}));
            ATELIER_TRY_BIND(position, cursor.u64({"index"}));
            ATELIER_TRY_BIND(instance_arg, cursor.at("instance"));
            ATELIER_TRY_BIND(cursor_instance, text(*instance_arg));
            ATELIER_TRY(page->check_version(*cursor_instance, *document, *project_revision));
            if (*position > count)
                return std::unexpected(Failure("invalid_request", "Asset directory cursor is outside the catalog"));
            index = static_cast<std::size_t>(*position);
        }
        ATELIER_TRY_BIND(entries, page->begin());
        for (; index < count && entries->size() < page->limit; ++index) {
            ATELIER_TRY_BIND(value, asset_json(index));
            ATELIER_TRY_BIND(appended, page->append(*entries, value->root()));
            if (!*appended)
                break;
        }
        if (index < count) {
            ATELIER_TRY_BIND(next, page->next_cursor());
            page->result.set_validated(*next, "domain", "assets");
            ATELIER_TRY(page->result.set(*next, "index", index));
        }
        return page->result.finish();
    }

    std::expected<json::Document, Failure> inspect_objects(json::Value args) {
        ATELIER_TRY_BIND(page, inspection_page(Directory::objects, args));
        const auto& cursor = page->cursor;
        unsigned entity_index{};
        unsigned object_index{};
        if (cursor) {
            ATELIER_TRY(
                cursor->members({"instance", "document", "revision", "entity", "component", "entity_name", "type"}));
            ATELIER_TRY_BIND(cursor_instance, cursor->string({"instance"}));
            ATELIER_TRY_BIND(document, cursor->u64({"document"}));
            ATELIER_TRY_BIND(cursor_revision, cursor->u64({"revision"}));
            ATELIER_TRY(page->check_version(*cursor_instance, *document, *cursor_revision));
            ATELIER_TRY_BIND(entity, cursor->at("entity"));
            ATELIER_TRY_BIND(entity_number, integer(*entity, 0, editor::Session::maximum_entities));
            ATELIER_TRY_BIND(component, cursor->at("component"));
            ATELIER_TRY_BIND(component_number, integer(*component, 0, editor::Session::maximum_components));
            entity_index = *entity_number;
            object_index = *component_number;
        }
        const auto filter = [&](std::string_view key) -> std::expected<std::optional<std::string>, Failure> {
            ATELIER_TRY_BIND(argument, args.find(key));
            auto value = *argument;
            if (cursor) {
                ATELIER_TRY_BIND(saved, cursor->at(key));
                if (*argument && **argument != *saved)
                    return std::unexpected(Failure("invalid_request", "A pagination cursor cannot change its filters"));
                value = *saved;
            }
            if (*argument && (**argument).is_null())
                return std::unexpected(Failure("invalid_request", "Inspection filters must be strings"));
            if (!value || value->is_null())
                return std::nullopt;
            ATELIER_TRY_BIND(name, text(*value));
            return std::move(*name);
        };
        ATELIER_TRY_BIND(entity_filter, filter("entity_name"));
        ATELIER_TRY_BIND(type_filter, filter("type"));
        const auto entity_name = *entity_filter ? platform::utf8_to_utf16_validated(**entity_filter) : std::wstring{};
        ATELIER_TRY_BIND(entities, operation(app.document_->world().members()));
        if (entity_index > entities->size() || (entity_index == entities->size() && object_index))
            return std::unexpected(Failure("invalid_request", "Object directory cursor is outside the document"));
        if (pruned_revision != page->revision)
            ATELIER_TRY(prune_handles(*entities));
        ATELIER_TRY_BIND(entries, page->begin());
        auto& runtime = app.document_->world().runtime();
        for (; entity_index < entities->size(); ++entity_index, object_index = 0) {
            const auto entity = (*entities)[entity_index].id();
            const bool matches_name = !*entity_filter || app.document_->name(entity) == entity_name;
            if (!matches_name && object_index == 0)
                continue;
            ATELIER_TRY_BIND(components, operation(world::components(runtime, entity)));
            if (object_index > components->size())
                return std::unexpected(Failure("invalid_request", "Object directory cursor is outside the entity"));
            if (!matches_name)
                continue;
            for (; object_index <= components->size(); ++object_index) {
                const auto id = object_index ? (*components)[object_index - 1].id() : entity;
                ATELIER_TRY_BIND(type_id, operation(runtime.type_of(id)));
                ATELIER_TRY_BIND(type, operation(runtime.describe(*type_id)));
                if (*type_filter && (*type)->name != **type_filter)
                    continue;
                json::Builder entry;
                ATELIER_TRY(write_object_header(entry, entry.root(), id, (*type)->name));
                entry.set_validated(entry.root(), "kind", object_index ? "component" : "entity");
                ATELIER_TRY(set_handle(entry, entry.root(), "entity", entity));
                if (!object_index) {
                    ATELIER_TRY_BIND(parent, operation(world::parent_entity(runtime, entity)));
                    ATELIER_TRY(set_handle(entry, entry.root(), "parent", *parent));
                }
                ATELIER_TRY_BIND(appended, page->append(*entries, entry.root()));
                if (!*appended) {
                    ATELIER_TRY_BIND(next, page->next_cursor());
                    ATELIER_TRY(page->result.set(*next, "entity", entity_index));
                    ATELIER_TRY(page->result.set(*next, "component", object_index));
                    const auto save_filter = [&](std::string_view key,
                                                 const std::optional<std::string>& value) -> json::Result<void> {
                        if (!value)
                            return page->result.set_null(*next, key);
                        page->result.set_validated(*next, key, *value);
                        return {};
                    };
                    ATELIER_TRY(save_filter("entity_name", *entity_filter));
                    ATELIER_TRY(save_filter("type", *type_filter));
                    return page->result.finish();
                }
            }
        }
        return page->result.finish();
    }

    Result<bool> inspect_guide_points() const {
        const auto& workspace = app.workspace_;
        if (workspace.sop_show_points)
            return *workspace.sop_show_points;
        auto& doc = *app.document_;
        if (!workspace.active_graph.empty() && doc.has_graph(workspace.active_graph)) {
            ATELIER_TRY_BIND(graph, doc.evaluate_graph(workspace.active_graph, app.project_.get()));
            return workspace.effective_show_points(graph->preview_output.get());
        }
        ATELIER_TRY_BIND(generator, workspace.active_procedural_generator(doc));
        if (*generator == ObjectId{})
            return false;
        ATELIER_TRY_BIND(source, presentation::read_procedural_source(doc.world().runtime(), *generator));
        return workspace.effective_show_points(source->output ? source->output->geometry.get() : nullptr);
    }

    std::expected<json::Document, Failure> inspect(bool summary = false) {
        ATELIER_TRY_BIND(project, utf8_text(app.project_ ? app.project_->root().generic_wstring() : L""));
        ATELIER_TRY_BIND(scene, utf8_text(app.scene_path_.generic_wstring()));
        json::Builder result;
        const auto root = result.root();
        result.set_validated(root, "identity", server.identity().root());
        result.set_validated(root, "project", *project);
        result.set_validated(root, "scene_path", *scene);
        ATELIER_TRY(result.set(root, "dirty", app.document_->dirty()));
        ATELIER_TRY(result.set(root, "busy", busy()));
        ATELIER_TRY_BIND(reasons, busy_reasons());
        result.set_validated(root, "busy_reasons", reasons->root());
        ATELIER_TRY(result.set(root, "text_focus", app.ui_.wants_keyboard()));
        ATELIER_TRY(result.set(root, "undo", app.document_->undo_count()));
        ATELIER_TRY(result.set(root, "redo", app.document_->redo_count()));
        ATELIER_TRY(result.set(root, "active_graph", app.workspace_.active_graph));
        ATELIER_TRY(result.set(root, "selected_graph_node", app.workspace_.selected_graph_node));
        {
            const auto points = inspect_guide_points();
            // Summary remains available when a preview cannot be read. Full requires every requested field.
            if (!points && !summary)
                return std::unexpected(operation_error(points.error()));
            const auto guides = result.object_validated(root, "sop_guides");
            ATELIER_TRY(result.set(guides, "points", points.value_or(false)));
            ATELIER_TRY(result.set(guides, "points_auto", !app.workspace_.sop_show_points.has_value()));
            ATELIER_TRY(result.set(guides, "normals", app.workspace_.sop_show_normals));
            ATELIER_TRY(result.set(guides, "face_normals", app.workspace_.sop_show_face_normals));
            ATELIER_TRY(result.set(guides, "orient", app.workspace_.sop_show_orient));
            ATELIER_TRY(result.set(guides, "bounds", app.workspace_.sop_show_bounds));
            ATELIER_TRY(result.set(guides, "info", app.workspace_.sop_show_info));
        }
        ATELIER_TRY_BIND(camera, camera_json());
        result.set_validated(root, "camera", camera->root());
        ATELIER_TRY_BIND(view, view_json(app.workspace_.view));
        result.set_validated(root, "view", view->root());
        ATELIER_TRY(result.set(root, "accepted_frames", app.accepted_frames_));
        ATELIER_TRY_BIND(frame, frame_json());
        result.set_validated(root, "last_frame", frame->root());
        ATELIER_TRY_BIND(performance, performance_json(app.workspace_.lighting.performance()));
        result.set_validated(root, "performance", performance->root());
        if (auto* job = active()) {
            const auto item = result.object_validated(root, "active_job");
            result.set_validated(item, "job_id", job->id);
            ATELIER_TRY(result.set(item, "rendering", job->rendering));
            ATELIER_TRY(result.set(item, "cancel_requested", job->work->cancelled()));
        }
        if (summary) {
            result.set_validated(root, "detail", "summary");
            return result.finish();
        }
        ATELIER_TRY_BIND(entities, result.array(root, "entities"));
        const auto& world = app.document_->world();
        auto& runtime = world.runtime();
        ATELIER_TRY_BIND(members, operation(world.members()));
        for (const auto& entity : *members) {
            const auto id = entity.id();
            const auto record = result.append_object(*entities);
            ATELIER_TRY(write_object(result, record, id));
            ATELIER_TRY_BIND(parts, operation(world::components(runtime, id)));
            ATELIER_TRY_BIND(components, result.array(record, "components"));
            for (const auto& part : *parts)
                ATELIER_TRY(write_object(result, result.append_object(*components), part.id()));
            ATELIER_TRY_BIND(parent, operation(world::parent_entity(runtime, id)));
            ATELIER_TRY(set_handle(result, record, "parent", *parent));
            ATELIER_TRY_BIND(child_roots, operation(world::children(runtime, id)));
            ATELIER_TRY_BIND(children, result.array(record, "children"));
            for (const auto& child : *child_roots) {
                ATELIER_TRY_BIND(child_handle, handle(child.id()));
                ATELIER_TRY(result.append(*children, *child_handle));
            }
        }
        // Released IDs never retain Runtime objects, and are bounded by current membership.
        ATELIER_TRY(prune_handles(*members));
        ATELIER_TRY_BIND(selection, result.array(root, "selection"));
        for (auto entity : app.document_->selected_entities()) {
            ATELIER_TRY_BIND(value_handle, handle(entity));
            ATELIER_TRY(result.append(*selection, *value_handle));
        }
        ATELIER_TRY(set_handle(result, root, "inspected_object", app.document_->selection().object));
        ATELIER_TRY_BIND(dirty, result.array(root, "dirty_materials"));
        for (const auto& key : app.document_->dirty_materials()) {
            ATELIER_TRY(result.append(*dirty, key));
        }
        ATELIER_TRY_BIND(dirty_graphs, result.array(root, "dirty_graphs"));
        for (const auto& key : app.document_->dirty_graphs()) {
            ATELIER_TRY(result.append(*dirty_graphs, key));
        }
        ATELIER_TRY_BIND(open_graphs, result.array(root, "open_graphs"));
        for (const auto& key : app.document_->open_graphs()) {
            ATELIER_TRY(result.append(*open_graphs, key));
        }
        ATELIER_TRY_BIND(types, result.array(root, "component_types"));
        ATELIER_TRY_BIND(component_types, operation(app.document_->component_types()));
        for (const auto& type : *component_types) {
            ATELIER_TRY(result.append(*types, type.name));
        }
        ATELIER_TRY_BIND(assets, result.array(root, "assets"));
        const auto count = assets::mesh_assets().size() + (app.project_ ? app.project_->entries().size() : 0);
        for (std::size_t i = 0; i < count; ++i) {
            ATELIER_TRY_BIND(asset, asset_json(i));
            result.append(*assets, asset->root());
        }
        return result.finish();
    }

    std::expected<json::Document, Failure> graph_summary_json(const editor::GraphGeometrySummary& summary) const {
        json::Builder result;
        const auto root = result.root();
        ATELIER_TRY(result.set(root, "valid", summary.valid));
        const auto counts = result.object_validated(root, "counts");
        constexpr std::array<std::string_view, 5> layer_names{"point", "vertex", "primitive", "detail", "instance"};
        for (std::size_t i = 0; i < layer_names.size(); ++i)
            ATELIER_TRY(result.set(counts, layer_names[i], summary.counts[i]));
        ATELIER_TRY(result.set(root, "has_bounds", summary.has_bounds));
        if (summary.has_bounds) {
            ATELIER_TRY(numbers(result, root, "bounds_min", summary.bounds_min));
            ATELIER_TRY(numbers(result, root, "bounds_max", summary.bounds_max));
        } else {
            ATELIER_TRY(result.set_null(root, "bounds_min"));
            ATELIER_TRY(result.set_null(root, "bounds_max"));
        }
        ATELIER_TRY_BIND(attributes, result.array(root, "attributes"));
        for (std::size_t layer = 0; layer < layer_names.size(); ++layer)
            for (const auto& attr : summary.attributes[layer]) {
                const auto item = result.append_object(*attributes);
                ATELIER_TRY(result.set(item, "layer", layer_names[layer]));
                ATELIER_TRY(result.set(item, "name", attr.name));
                ATELIER_TRY(result.set(item, "type", attr.type));
                ATELIER_TRY(result.set(item, "components", attr.components));
                ATELIER_TRY(result.set(item, "count", attr.count));
                if (attr.selected_count)
                    ATELIER_TRY(result.set(item, "selected_count", *attr.selected_count));
                ATELIER_TRY(result.set(item, "non_finite_count", attr.non_finite_count));
                if (!attr.minimum.empty()) {
                    ATELIER_TRY(result.numbers(item, "minimum", std::span{attr.minimum}));
                    ATELIER_TRY(result.numbers(item, "maximum", std::span{attr.maximum}));
                    ATELIER_TRY(result.numbers(item, "mean", std::span{attr.mean}));
                }
                ATELIER_TRY_BIND(samples, result.array(item, "samples"));
                for (const auto& sample : attr.sample_values)
                    ATELIER_TRY(result.append(*samples, sample));
            }
        ATELIER_TRY_BIND(refs, result.array(root, "instance_references"));
        for (const auto& [mesh_key, inst_count] : summary.instance_references) {
            const auto item = result.append_object(*refs);
            ATELIER_TRY(result.set(item, "mesh", mesh_key));
            ATELIER_TRY(result.set(item, "count", inst_count));
        }
        if (summary.spatial) {
            const auto sp = result.object_validated(root, "spatial");
            if (summary.spatial->topology) {
                const auto& t = *summary.spatial->topology;
                const auto top = result.object_validated(sp, "topology");
                ATELIER_TRY(result.set(top, "skipped", t.skipped));
                ATELIER_TRY(result.set(top, "polygon_count", t.polygon_count));
                ATELIER_TRY(result.set(top, "polyline_count", t.polyline_count));
                ATELIER_TRY(result.set(top, "degenerate_primitives", t.degenerate_primitives));
                ATELIER_TRY(result.set(top, "open_boundary_edges", t.open_boundary_edges));
                ATELIER_TRY(result.set(top, "non_manifold_edges", t.non_manifold_edges));
            } else {
                ATELIER_TRY(result.set_null(sp, "topology"));
            }
            if (summary.spatial->spacing) {
                const auto& s = *summary.spatial->spacing;
                const auto spc = result.object_validated(sp, "spacing");
                ATELIER_TRY(result.set(spc, "domain", s.domain));
                ATELIER_TRY(result.set(spc, "element_count", s.element_count));
                ATELIER_TRY(result.set(spc, "sample_count", s.sample_count));
                ATELIER_TRY(result.set(spc, "coincident_count", s.coincident_count));
                ATELIER_TRY(result.set(spc, "min", s.minimum));
                ATELIER_TRY(result.set(spc, "p5", s.p5));
                ATELIER_TRY(result.set(spc, "p50", s.p50));
                ATELIER_TRY(result.set(spc, "p95", s.p95));
            } else {
                ATELIER_TRY(result.set_null(sp, "spacing"));
            }
            if (summary.spatial->instance_overlaps) {
                const auto& ov = *summary.spatial->instance_overlaps;
                const auto obj = result.object_validated(sp, "instance_overlaps");
                ATELIER_TRY(result.set(obj, "tested_instances", ov.tested_instances));
                ATELIER_TRY(result.set(obj, "overlapping_pairs", ov.overlapping_pairs));
                ATELIER_TRY(result.set(obj, "pairs_capped", ov.pairs_capped));
                ATELIER_TRY(result.set(obj, "overlapping_instances", ov.overlapping_instances));
                ATELIER_TRY(result.set(obj, "overlapping_ratio", ov.overlapping_ratio));
                ATELIER_TRY(result.set(obj, "max_penetration_depth", ov.max_penetration_depth));
            } else {
                ATELIER_TRY(result.set_null(sp, "instance_overlaps"));
            }
            if (summary.spatial->surface_clearance) {
                const auto& cl = *summary.spatial->surface_clearance;
                const auto obj = result.object_validated(sp, "surface_clearance");
                ATELIER_TRY(result.set(obj, "reference_node", cl.reference_node));
                ATELIER_TRY(result.set(obj, "domain", cl.domain));
                ATELIER_TRY(result.set(obj, "tested_count", cl.tested_count));
                ATELIER_TRY(result.set(obj, "grounded_count", cl.grounded_count));
                ATELIER_TRY(result.set(obj, "floating_count", cl.floating_count));
                ATELIER_TRY(result.set(obj, "buried_count", cl.buried_count));
                ATELIER_TRY(result.set(obj, "off_surface_count", cl.off_surface_count));
                if (cl.has_gaps) {
                    ATELIER_TRY(result.set(obj, "gap_min", cl.gap_min));
                    ATELIER_TRY(result.set(obj, "gap_p5", cl.gap_p5));
                    ATELIER_TRY(result.set(obj, "gap_p50", cl.gap_p50));
                    ATELIER_TRY(result.set(obj, "gap_p95", cl.gap_p95));
                    ATELIER_TRY(result.set(obj, "gap_max", cl.gap_max));
                } else {
                    ATELIER_TRY(result.set_null(obj, "gap_min"));
                    ATELIER_TRY(result.set_null(obj, "gap_p5"));
                    ATELIER_TRY(result.set_null(obj, "gap_p50"));
                    ATELIER_TRY(result.set_null(obj, "gap_p95"));
                    ATELIER_TRY(result.set_null(obj, "gap_max"));
                }
            } else {
                ATELIER_TRY(result.set_null(sp, "surface_clearance"));
            }
        } else {
            ATELIER_TRY(result.set_null(root, "spatial"));
        }
        return result.finish();
    }

    static json::Result<void> write_graph_diagnostic(json::Builder& result, json::Value parent, std::string_view key,
                                                     const std::optional<Error>& error, std::string_view node_name) {
        if (!error)
            return result.set_null(parent, key);
        const auto diag = result.object_validated(parent, key);
        ATELIER_TRY(result.set(diag, "message", error->message));
        ATELIER_TRY(result.set(diag, "node", node_name));
        if (error->source) {
            const auto src = result.object_validated(diag, "source");
            ATELIER_TRY(result.set(src, "path", error->source->path));
            ATELIER_TRY(result.set(src, "line", error->source->line));
            ATELIER_TRY(result.set(src, "column", error->source->column));
        }
        if (error->element) {
            const auto elem = result.object_validated(diag, "element");
            ATELIER_TRY(result.set(elem, "layer", error->element->layer));
            ATELIER_TRY(result.set(elem, "index", error->element->index));
        }
        return {};
    }

    std::expected<json::Document, Failure> graph_inspect_json(const editor::GraphInspectView& view,
                                                              std::string_view inspected_name,
                                                              std::span<const editor::GraphNodeCatalogEntry> catalog,
                                                              bool topology_only = false) {
        json::Builder result;
        const auto root = result.root();
        ATELIER_TRY(result.set(root, "asset", view.key));
        ATELIER_TRY(set_handle(result, root, "graph", view.graph));
        ATELIER_TRY(result.set(root, "display", view.display));
        ATELIER_TRY(result.set(root, "template", view.template_node));
        ATELIER_TRY(result.set(root, "can_undo", view.can_undo));
        ATELIER_TRY(result.set(root, "can_redo", view.can_redo));
        const auto external_ports = result.object_validated(root, "external_ports");
        ATELIER_TRY(write_graph_ports(result, external_ports, view.external_ports));
        if (!topology_only) {
            ATELIER_TRY(result.set(root, "dirty", app.document_->graph_dirty(view.key)));
            ATELIER_TRY(result.set(root, "seed", view.seed));
            ATELIER_TRY(result.set(root, "inspected_node", inspected_name));
            if (view.display_error)
                ATELIER_TRY(result.set(root, "display_error", view.display_error->message));
            else
                ATELIER_TRY(result.set_null(root, "display_error"));
            if (view.inspected_error)
                ATELIER_TRY(result.set(root, "inspected_error", view.inspected_error->message));
            else
                ATELIER_TRY(result.set_null(root, "inspected_error"));
            if (!inspected_name.empty() && view.inspected_error) {
                const std::string_view node_ref =
                    view.inspected_error_node.empty() ? inspected_name : std::string_view{view.inspected_error_node};
                ATELIER_TRY(write_graph_diagnostic(result, root, "diagnostic", view.inspected_error, node_ref));
            } else if (view.display_error) {
                const std::string_view node_ref = view.display_error_node.empty()
                                                      ? std::string_view{view.display}
                                                      : std::string_view{view.display_error_node};
                ATELIER_TRY(write_graph_diagnostic(result, root, "diagnostic", view.display_error, node_ref));
            } else {
                ATELIER_TRY(result.set_null(root, "diagnostic"));
            }
            ATELIER_TRY_BIND(parameters, result.array(root, "parameters"));
            for (const auto& param : view.parameters) {
                const auto item = result.append_object(*parameters);
                ATELIER_TRY(result.set(item, "name", param.name));
                ATELIER_TRY(result.set(item, "label", param.label));
                ATELIER_TRY(result.set(item, "category", param.category));
                ATELIER_TRY(result.set(item, "description", param.description));
                ATELIER_TRY_BIND(targets, result.array(item, "targets"));
                for (const auto& t : param.targets)
                    ATELIER_TRY(result.append(*targets, t));
            }
            ATELIER_TRY_BIND(types, result.array(root, "node_types"));
            for (const auto& type : catalog)
                ATELIER_TRY(write_graph_type(result, result.append_object(*types), type));
        }
        ATELIER_TRY_BIND(nodes, result.array(root, "nodes"));
        for (const auto& node : view.nodes) {
            const auto item = result.append_object(*nodes);
            ATELIER_TRY(result.set(item, "name", node.name));
            ATELIER_TRY(set_handle(result, item, "handle", node.id));
            ATELIER_TRY(result.set(item, "type", node.type));
            if (!node.asset_key.empty()) {
                ATELIER_TRY(result.set(item, "asset", node.asset_key));
                ATELIER_TRY(result.set(item, "asset_label", node.asset_label));
            }
            ATELIER_TRY(write_graph_ports(result, item, node.ports));
            ATELIER_TRY(result.set(item, "placeholder", node.placeholder));
            if (topology_only) {
                if (const auto* entry = app.document_->graph_node(view.key, node.name)) {
                    if (const auto kept = *entry->kept.root().find("inputs"))
                        result.set_validated(item, "kept_inputs", *kept);
                }
                continue;
            }
            if (node.overrides.root().kind() == json::Kind::object && node.overrides.root().size() > 0)
                result.set_validated(item, "overrides", node.overrides.root());
            ATELIER_TRY(result.set(item, "display", node.is_display));
            ATELIER_TRY(result.set(item, "template", node.is_template));
            ATELIER_TRY(result.set(item, "bypass", node.is_bypassed));
            ATELIER_TRY(result.set(item, "frozen", node.is_frozen));
            const auto shape_name = editor::graph_node_shape_name(node.shape);
            ATELIER_TRY(result.set(item, "shape", shape_name.empty() ? std::string_view{"auto"} : shape_name));
            if (node.color)
                ATELIER_TRY(result.set(item, "color", editor::format_graph_hex_color(*node.color)));
            else
                ATELIER_TRY(result.set_null(item, "color"));
            ATELIER_TRY(result.set(item, "comment", node.comment));
            ATELIER_TRY(result.numbers(item, "position", std::span{node.position}));
            if (node.status) {
                const auto st = result.object_validated(item, "status");
                ATELIER_TRY(result.set(st, "evaluations", node.status->evaluations));
                ATELIER_TRY(result.set(st, "cache_hits", node.status->cache_hits));
                if (const auto& stats = node.status->boolean_stats) {
                    const auto metrics = result.object_validated(st, "boolean");
                    ATELIER_TRY(result.set(metrics, "input_faces", stats->input_faces));
                    ATELIER_TRY(result.set(metrics, "candidate_pairs", stats->candidate_pairs));
                    ATELIER_TRY(result.set(metrics, "prepared_input_hits", stats->prepared_input_hits));
                    ATELIER_TRY(result.set(metrics, "ray_candidates", stats->ray_candidates));
                    ATELIER_TRY(result.set(metrics, "splits", stats->splits));
                    ATELIER_TRY(result.set(metrics, "peak_face_scratch_bytes", stats->peak_face_scratch_bytes));
                    ATELIER_TRY(result.set(metrics, "prepare_ms", double(stats->prepare.count()) / 1e6));
                    ATELIER_TRY(result.set(metrics, "arrange_ms", double(stats->arrange.count()) / 1e6));
                    ATELIER_TRY(result.set(metrics, "sew_ms", double(stats->sew.count()) / 1e6));
                    ATELIER_TRY(result.set(metrics, "publish_ms", double(stats->publish.count()) / 1e6));
                }
                if (node.status->time)
                    ATELIER_TRY(result.set(st, "cook_ms", double(node.status->time->count()) / 1e6));
                else
                    ATELIER_TRY(result.set_null(st, "cook_ms"));
                if (node.status->error)
                    ATELIER_TRY(result.set(st, "error", node.status->error->message));
                else
                    ATELIER_TRY(result.set_null(st, "error"));
                const std::string_view diag_node =
                    node.root_error_node.empty() ? std::string_view{node.name} : std::string_view{node.root_error_node};
                ATELIER_TRY(write_graph_diagnostic(result, st, "diagnostic", node.status->error, diag_node));
            } else {
                ATELIER_TRY(result.set_null(item, "status"));
            }
        }
        ATELIER_TRY_BIND(connections, result.array(root, "connections"));
        for (const auto& conn : view.connections) {
            const auto item = result.append_object(*connections);
            ATELIER_TRY(result.set(item, "from_node", conn.from_node));
            ATELIER_TRY(result.set(item, "from_port", conn.from_port));
            ATELIER_TRY(result.set(item, "to_node", conn.to_node));
            ATELIER_TRY(result.set(item, "to_port", conn.to_port));
        }
        if (topology_only)
            return result.finish();
        ATELIER_TRY_BIND(notes, result.array(root, "notes"));
        for (const auto& note : view.notes) {
            const auto item = result.append_object(*notes);
            ATELIER_TRY(result.set(item, "name", note.name));
            ATELIER_TRY(result.numbers(item, "position", std::span{note.position}));
            ATELIER_TRY(result.numbers(item, "size", std::span{note.size}));
            ATELIER_TRY(result.set(item, "title", note.title));
            ATELIER_TRY(result.set(item, "text", note.text));
            ATELIER_TRY(result.set(item, "color", editor::format_graph_hex_color(note.color)));
        }
        ATELIER_TRY_BIND(boxes, result.array(root, "boxes"));
        for (const auto& box : view.boxes) {
            const auto item = result.append_object(*boxes);
            ATELIER_TRY(result.set(item, "name", box.name));
            ATELIER_TRY(result.numbers(item, "position", std::span{box.position}));
            ATELIER_TRY(result.numbers(item, "size", std::span{box.size}));
            ATELIER_TRY(result.set(item, "title", box.title));
            ATELIER_TRY(result.set(item, "color", editor::format_graph_hex_color(box.color)));
        }
        ATELIER_TRY_BIND(issues, result.array(root, "issues"));
        for (const auto& issue : view.issues) {
            const auto item = result.append_object(*issues);
            ATELIER_TRY(result.set(item, "kind", world::name(issue.kind)));
            ATELIER_TRY(result.set(item, "message", issue.message));
        }
        ATELIER_TRY_BIND(disp_summary, graph_summary_json(view.display_summary));
        result.set_validated(root, "display_summary", disp_summary->root());
        if (view.inspected_summary) {
            ATELIER_TRY_BIND(insp_summary, graph_summary_json(*view.inspected_summary));
            result.set_validated(root, "inspected_summary", insp_summary->root());
        } else {
            ATELIER_TRY(result.set_null(root, "inspected_summary"));
        }
        return result.finish();
    }

    std::expected<std::unique_ptr<Job>, Failure> new_job(const std::string& id,
                                                         const std::shared_ptr<control::Peer>& peer) {
        if (active())
            return std::unexpected(Failure("busy", "Another control job is still active"));
        if (std::ranges::any_of(jobs, [&](const auto& job) { return job->id == id; }))
            return std::unexpected(
                Failure("request_id_expired", "Request payload expired; query the retained job using job.status"));
        auto job = std::make_unique<Job>();
        job->id = id;
        job->peer = peer;
        job->token = token();
        job->context = observed;
        job->started = std::chrono::steady_clock::now();
        job->deadline = job->started + std::chrono::seconds(120);
        ATELIER_TRY_BIND(directory_id, control::unique_id());
        job->directory =
            server.directory() / "artifacts" / *server.identity().root().string({"instance_id"}) / *directory_id;
        json::Builder manifest;
        const auto root = manifest.root();
        ATELIER_TRY(manifest.set(root, "protocol", control::protocol_version));
        manifest.set_validated(root, "identity", server.identity().root());
        manifest.set_validated(root, "state_token", token());
        manifest.set_validated(root, "request_id", id);
        ATELIER_TRY_BIND(camera, camera_json());
        manifest.set_validated(root, "camera", camera->root());
        ATELIER_TRY(manifest.set(root, "view_schema", repro_view_schema));
        ATELIER_TRY_BIND(view, view_json(app.workspace_.view));
        manifest.set_validated(root, "view", view->root());
        job->manifest = manifest.finish();
        return job;
    }

    void publish_job(std::unique_ptr<Job> job) {
        record_completions();
        while (jobs.size() >= 16)
            jobs.pop_front();
        jobs.push_back(std::move(job));
    }

    std::expected<json::Document, Failure> start_export(const std::string& id,
                                                        const std::shared_ptr<control::Peer>& peer) {
        if (app.project_ &&
            (!app.project_code_ || !app.project_code_->published_build() || !app.workspace_.scene_available))
            return std::unexpected(
                Failure("not_ready", "Repair the project and load its scene before exporting a repro"));
        if (auto synced = app.document_->sync_procedural_generators(app.project_.get()); !synced)
            return std::unexpected(Failure("operation_failed", synced.error().message));
        ATELIER_TRY_BIND(snapshot, control_artifacts::snapshot_resources(*app.document_));
        auto data = std::move(*snapshot);
        if (app.project_) {
            data.project = app.project_->root();
            const auto& published = *app.project_code_->published_build();
            data.sources = published.sources;
            data.ui_source = published.ui_source;
            data.publication_version = published.published_version;
        }
        json::Builder drafts(json::Kind::array);
        for (const auto& file : app.workspace_.source_editor.files())
            if (file->dirty()) {
                ATELIER_TRY_BIND(path, utf8_text(file->path.generic_wstring()));
                ATELIER_TRY_BIND(source, utf8_text(file->document->text()));
                const auto draft = drafts.append_object(drafts.root());
                drafts.set_validated(draft, "path", *path);
                drafts.set_validated(draft, "text", *source);
                ATELIER_TRY(drafts.set(draft, "conflict", file->conflict));
            }
        data.drafts = drafts.finish();
        if (app.project_) {
            data.roots.assign(app.project_->roots().begin(), app.project_->roots().end());
            // snapshot_resources already walked the actual parent/default closure
            // against compiled declarations. Do not turn dormant author keys into
            // dependencies or replace its exact Source snapshots with cache contents.
            const auto& doc = *app.document_;
            const auto compare = [](const std::filesystem::path& a, const std::filesystem::path& b) {
                const auto left = a.generic_wstring();
                const auto right = b.generic_wstring();
                return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE);
            };
            const auto less = [&](const auto& a, const auto& b) { return compare(a, b) == CSTR_LESS_THAN; };
            std::set<std::filesystem::path, decltype(less)> listed(less);
            const auto add = [&](const std::filesystem::path& named) -> std::expected<void, Failure> {
                // Raw mesh references may use a different case than the catalogue.
                // Copy each Windows file once, preserving keyed companions' identities.
                const auto entries = app.project_->entries();
                const auto at = std::ranges::find_if(
                    entries, [&](const auto& entry) { return compare(named, entry.path) == CSTR_EQUAL; });
                const auto& path = at == entries.end() ? named : at->path;
                if (!listed.insert(path).second)
                    return {};
                data.files.push_back(path);
                if (at != entries.end() &&
                    (at->kind == assets::AssetKind::mesh || at->kind == assets::AssetKind::material ||
                     at->kind == assets::AssetKind::texture || at->kind == assets::AssetKind::sky ||
                     at->kind == assets::AssetKind::procedural_graph)) {
                    auto sidecar = path;
                    sidecar += L".asset.json";
                    ATELIER_TRY_BIND(resolved, operation(app.project_->resolve(sidecar)));
                    const auto native = platform::windows_file_path(*resolved);
                    if (!native)
                        return std::unexpected(Failure("operation_failed", platform::error_message(native.error())));
                    std::error_code error;
                    const auto exists = std::filesystem::exists(*native, error);
                    if (error)
                        return std::unexpected(Failure("operation_failed", platform::error_message(error)));
                    if (!exists)
                        return std::unexpected(
                            Failure("dependency_mismatch", "Asset identity file is missing: " + at->key));
                    if (listed.insert(sidecar).second)
                        data.files.push_back(std::move(sidecar));
                }
                return {};
            };
            for (const auto& [kind, key] : data.references) {
                if (key.empty() || key.starts_with("builtin/"))
                    continue;
                const auto* entry = app.project_->entry(key);
                if (!entry || entry->kind != kind) {
                    if (doc.unavailable(kind, key))
                        continue;
                    return std::unexpected(
                        Failure("dependency_mismatch", "A captured resource is no longer in the project: " + key));
                }
                ATELIER_TRY(add(entry->path));
                // Raw geometry companions are not MS/MI execution dependencies.
                // Use the Reader's prepared mesh closure, never saved material walks.
                if (kind == assets::AssetKind::mesh && !doc.unavailable(kind, key)) {
                    ATELIER_TRY_BIND(files, operation(app.project_->assets().mesh_dependency_files(key)));
                    for (const auto& path : *files)
                        ATELIER_TRY(add(path));
                }
            }
        }
        ATELIER_TRY_BIND(created, new_job(id, peer));
        (*created)->kind = "repro.export";
        json::Builder manifest(std::move((*created)->manifest));
        const auto root = manifest.root();
        manifest.set_validated(root, "kind", "repro");
        ATELIER_TRY_BIND(scene, utf8_text(app.scene_path_.generic_wstring()));
        manifest.set_validated(root, "original_scene_path", *scene);
        ATELIER_TRY(manifest.set(root, "original_dirty", app.document_->dirty()));
        ATELIER_TRY_BIND(indices, manifest.array(root, "selection_indices"));
        ATELIER_TRY_BIND(members, operation(app.document_->world().members()));
        for (auto selected : app.document_->selected_entities())
            for (std::size_t i = 0; i < members->size(); ++i)
                if ((*members)[i].id() == selected)
                    manifest.append(*indices, static_cast<std::uint64_t>(i));
        const auto selection = app.document_->selection();
        if (selection.object != selection.entity && selection.object != ObjectId{})
            for (std::size_t i = 0; i < members->size(); ++i)
                if ((*members)[i].id() == selection.entity) {
                    ATELIER_TRY_BIND(components,
                                     operation(world::components(app.document_->world().runtime(), selection.entity)));
                    for (std::size_t j = 0; j < components->size(); ++j)
                        if ((*components)[j].id() == selection.object) {
                            const auto inspection = manifest.object_validated(root, "inspection");
                            ATELIER_TRY(manifest.set(inspection, "entity", i));
                            ATELIER_TRY(manifest.set(inspection, "component", j));
                        }
                }
        ATELIER_TRY(manifest.set(root, "source_version", data.sources ? data.sources->version : 0));
        ATELIER_TRY(manifest.set(root, "ui_source_version", data.ui_source ? data.ui_source->version : 0));
        ATELIER_TRY(manifest.set(root, "publication_version", data.publication_version));
        data.manifest = manifest.finish();
        start_writer(**created, [path = (*created)->directory,
                                 data = std::move(data)](control_artifacts::WriteState& work) mutable {
            return control_artifacts::export_repro(path, std::move(data), work);
        });
        json::Builder response;
        response.set_validated(response.root(), "status", "pending");
        response.set_validated(response.root(), "job_id", id);
        publish_job(std::move(*created));
        return response.finish();
    }

    std::expected<json::Document, Failure> start_trace(const std::string& id,
                                                       const std::shared_ptr<control::Peer>& peer, json::Value args) {
        ATELIER_TRY_BIND(scope_arg, args.find("scope"));
        if (*scope_arg) {
            ATELIER_TRY_BIND(scope, (**scope_arg).string());
            if (*scope != "scene_lit_post")
                return std::unexpected(Failure("invalid_request", "Only scene_lit_post observation is supported"));
        }
        if (app.workspace_.view.shading != renderer::Shading::scene_lit)
            return std::unexpected(Failure("not_ready", "Select Scene Lit before requesting a post-chain trace"));
        unsigned timeout = 120000;
        ATELIER_TRY_BIND(timeout_arg, args.find("timeout_ms"));
        if (*timeout_arg) {
            ATELIER_TRY_BIND(n, integer(**timeout_arg, 1, control::maximum_timeout_ms));
            timeout = *n;
        }
        auto trace = std::make_unique<rhi::CommandTrace>();
        ATELIER_TRY_BIND(created, new_job(id, peer));
        auto& job = **created;
        job.kind = "render.trace";
        job.trace = std::move(trace);
        job.tracing = true;
        job.deadline = job.started + std::chrono::milliseconds(timeout);
        json::Builder manifest(job.manifest);
        manifest.set_validated(manifest.root(), "kind", "render.trace");
        manifest.set_validated(manifest.root(), "scope", "scene_lit_post");
        ATELIER_TRY(manifest.set(manifest.root(), "document_generation", job.context.document));
        job.manifest = manifest.finish();
        json::Builder response;
        response.set_validated(response.root(), "status", "pending");
        response.set_validated(response.root(), "job_id", id);
        response.set_validated(response.object_validated(response.root(), "result"), "phase", "observing");
        publish_job(std::move(*created));
        return response.finish();
    }

    std::expected<json::Document, Failure> start_render(const std::string& id,
                                                        const std::shared_ptr<control::Peer>& peer, json::Value args,
                                                        bool compare, bool prepare = false) {
        if (!app.surface_.extent.width || !app.surface_.extent.height || !app.accepted_view_ ||
            app.accepted_view_->bounds.width <= 0 || app.accepted_view_->bounds.height <= 0)
            return std::unexpected(Failure("not_ready", "A visible accepted viewport is required"));
        std::vector<Case> cases;
        if (compare) {
            ATELIER_TRY_BIND(input, args.at("cases"));
            ATELIER_TRY_BIND(items, input->array());
            if (!items->size() || items->size() > 8)
                return std::unexpected(Failure("invalid_request", "Expected 1 to 8 cases"));
            std::set<std::string> names;
            for (const auto item : *items) {
                ATELIER_TRY(item.members({"name", "patch"}));
                ATELIER_TRY_BIND(name_arg, item.at("name"));
                ATELIER_TRY_BIND(name, text(*name_arg, 80));
                if (name->empty() || !names.insert(*name).second)
                    return std::unexpected(Failure("invalid_request", "Case names must be nonempty and unique"));
                ATELIER_TRY_BIND(patch, item.at("patch"));
                ATELIER_TRY_BIND(view, patched_view(app.workspace_.view, *patch, app.lighting_focus()));
                cases.push_back({std::move(*name), *view});
            }
        } else if (prepare) {
            ATELIER_TRY_BIND(patch_arg, args.find("patch"));
            json::Builder patch;
            if (*patch_arg)
                patch.replace(patch.root(), **patch_arg);
            const auto root = patch.root();
            if (root.kind() != json::Kind::object)
                return std::unexpected(Failure("invalid_request", "Preparation patch must be an object"));
            if (!*root.find("shading"))
                patch.set_validated(root, "shading", "scene_lit");
            if (!*root.find("wireframe"))
                patch.set_validated(root, "wireframe", "off");
            ATELIER_TRY_BIND(view, patched_view(app.workspace_.view, root, app.lighting_focus()));
            if (view->shading != renderer::Shading::scene_lit || view->wireframe == renderer::Wireframe::only ||
                app.scene_.meshes.empty())
                return std::unexpected(
                    Failure("not_ready", "Scene Lit preparation requires a filled view and a mesh instance"));
            cases.push_back({"prepare", *view});
        } else
            cases.push_back({"current", app.workspace_.view});
        ATELIER_TRY_BIND(points_arg, args.find("sample_frames"));
        std::vector<unsigned> points;
        if (*points_arg) {
            ATELIER_TRY_BIND(items, (**points_arg).array());
            if (!items->size() || items->size() * cases.size() > 16)
                return std::unexpected(Failure("invalid_request", "Expected 1 to 16 total sample points"));
            for (const auto point : *items) {
                ATELIER_TRY_BIND(n, integer(point, 1, 4096));
                if (!points.empty() && *n <= points.back())
                    return std::unexpected(Failure("invalid_request", "Sample ordinals must increase strictly"));
                points.push_back(*n);
            }
        } else
            points.push_back(1);
        const auto extent = app.surface_.extent;
        if (std::uint64_t(extent.width) * extent.height > (256 * 1024 * 1024) / (8 * points.size() * cases.size()))
            return std::unexpected(Failure("resource_limit", "Captured color/depth would exceed 256 MiB"));
        unsigned timeout = prepare ? control::maximum_timeout_ms : 120000;
        ATELIER_TRY_BIND(timeout_arg, args.find("timeout_ms"));
        if (*timeout_arg) {
            ATELIER_TRY_BIND(n, integer(**timeout_arg, 1, control::maximum_timeout_ms));
            timeout = *n;
        }
        ATELIER_TRY_BIND(created, new_job(id, peer));
        auto& job = **created;
        job.kind = prepare ? "render.prepare" : compare ? "render.compare" : "render.capture";
        job.cases = std::move(cases);
        job.points = std::move(points);
        job.rendering = true;
        job.preparing = prepare;
        job.bounds = app.accepted_view_->bounds;
        job.deadline = job.started + std::chrono::milliseconds(timeout);
        json::Builder manifest(job.manifest);
        manifest.set_validated(manifest.root(), "kind",
                               prepare   ? "render.prepare"
                               : compare ? "render.compare"
                                         : "render.capture");
        manifest.set_validated(
            manifest.root(), "history_policy",
            "natural renderer transitions, no full history reset or GPU-history restoration; accepted ordinals exclude skipped frames");
        job.manifest = manifest.finish();
        json::Builder response;
        response.set_validated(response.root(), "status", "pending");
        response.set_validated(response.root(), "job_id", id);
        response.set_validated(response.object_validated(response.root(), "result"), "phase",
                               prepare ? "preparing" : "rendering");
        publish_job(std::move(*created));
        return response.finish();
    }

    std::expected<json::Document, Failure> operator_list() const {
        json::Builder output;
        ATELIER_TRY_BIND(operators, output.array(output.root(), "operators"));
        auto& runtime = app.document_->world().runtime();
        for (const auto& info : app.workspace_.operators.all()) {
            const auto item = output.append_object(*operators);
            ATELIER_TRY_BIND(flags, output.array(item, "flags"));
            for (const auto& [flag, name] : {std::pair{editor::OperatorFlag::undo, "undo"},
                                             {editor::OperatorFlag::history, "history"},
                                             {editor::OperatorFlag::replaces_document, "replaces_document"}}) {
                if (has_any(info.flags, flag)) {
                    ATELIER_TRY(output.append(*flags, std::string_view{name}));
                }
            }
            ATELIER_TRY_BIND(parameters, editor::parameter_schema(runtime, info).transform_error(operation_failure));
            ATELIER_TRY_BIND(result, editor::result_schema(runtime, info).transform_error(operation_failure));
            ATELIER_TRY_BIND(kinds, output.array(item, "kinds"));
            ATELIER_TRY(
                output.append(*kinds, std::string_view{operator_kind_names[static_cast<std::size_t>(info.kind)]}));
            if (info.requires_interaction) {
                ATELIER_TRY(output.append(*kinds, "interactive"));
            }
            ATELIER_TRY_BIND(title, utf8_text(info.title));
            ATELIER_TRY(output.set(item, "id", info.id));
            output.set_validated(item, "title", *title);
            ATELIER_TRY(output.set(item, "description", info.description));
            output.set_validated(item, "parameters", parameters->root());
            output.set_validated(item, "result", result->root());
            if (!info.interaction.empty()) {
                ATELIER_TRY(output.set(item, "interaction", info.interaction));
            }
        }
        const auto context = output.object_validated(output.root(), "context");
        output.set_validated(context, "type", "object");
        ATELIER_TRY(output.set(context, "additionalProperties", false));
        const auto properties = output.object_validated(context, "properties");
        const auto selection = output.object_validated(properties, "selection");
        output.set_validated(selection, "type", "object");
        ATELIER_TRY(output.set(selection, "additionalProperties", false));
        ATELIER_TRY_BIND(required, output.array(selection, "required"));
        ATELIER_TRY(output.append(*required, "entities"));
        const auto fields = output.object_validated(selection, "properties");
        const auto entities = output.object_validated(fields, "entities");
        output.set_validated(entities, "type", "array");
        output.set_validated(output.object_validated(entities, "items"), "type", "string");
        ATELIER_TRY(output.set(entities, "maxItems", editor::Session::maximum_entities));
        output.set_validated(output.object_validated(fields, "active"), "type", "string");
        output.set_validated(output.object_validated(fields, "object"), "type", "string");
        const auto region = output.object_validated(properties, "region");
        ATELIER_TRY_BIND(regions, output.array(region, "enum"));
        for (const auto name : editor::region_names()) {
            ATELIER_TRY(output.append(*regions, name));
        }
        output.set_validated(output.object_validated(properties, "asset"), "type", "string");
        return output.finish();
    }

    std::expected<editor::ContextOverride, Failure> context_override(json::Value value) {
        ATELIER_TRY(value.allowed_members({"selection", "region", "asset"}));
        editor::ContextOverride result;
        ATELIER_TRY_BIND(asset, value.find("asset"));
        if (*asset) {
            ATELIER_TRY_BIND(name, text(**asset));
            result.asset = std::move(*name);
        }
        ATELIER_TRY_BIND(selection_arg, value.find("selection"));
        if (*selection_arg) {
            const auto selection = **selection_arg;
            ATELIER_TRY(selection.allowed_members({"entities", "active", "object"}));
            ATELIER_TRY_BIND(entities, selection.at("entities"));
            ATELIER_TRY_BIND(items, entities->array());
            if (items->size() > editor::Session::maximum_entities)
                return std::unexpected(Failure("invalid_request", "Invalid entity selection"));
            editor::SelectionContext context;
            for (const auto entity : *items) {
                ATELIER_TRY_BIND(id, object(entity));
                context.entities.push_back(*id);
            }
            context.active = context.entities.empty() ? ObjectId{} : context.entities.back();
            ATELIER_TRY_BIND(active, selection.find("active"));
            if (*active) {
                ATELIER_TRY_BIND(id, object(**active));
                context.active = *id;
            }
            context.object = context.active;
            ATELIER_TRY_BIND(inspected, selection.find("object"));
            if (*inspected) {
                ATELIER_TRY_BIND(id, object(**inspected));
                context.object = *id;
            }
            if (const auto at = std::ranges::find(context.entities, context.active); at != context.entities.end())
                std::rotate(at, at + 1, context.entities.end());
            result.selection = std::move(context);
        }
        ATELIER_TRY_BIND(region, value.find("region"));
        if (*region) {
            ATELIER_TRY_BIND(name, text(**region, 40));
            const auto names = editor::region_names();
            const auto at = std::ranges::find(names, *name);
            if (at == names.end())
                return std::unexpected(Failure("invalid_request", "Unknown region"));
            result.region = static_cast<editor::RegionKind>(at - names.begin());
        }
        return result;
    }

    std::expected<json::Document, Failure> call_operator(json::Value args) {
        ATELIER_TRY_BIND(id_arg, args.at("id"));
        ATELIER_TRY_BIND(id, text(*id_arg, 80));
        editor::OperatorCall request{std::move(*id), json::Document{}, {}};
        const auto* info = app.workspace_.operators.find(request.id);
        if (!info)
            return std::unexpected(Failure("unknown_operator", "No editor operator has this ID"));
        ATELIER_TRY_BIND(arguments, args.find("args"));
        if (*arguments) {
            if ((**arguments).kind() != json::Kind::object)
                return std::unexpected(Failure("invalid_request", "args must be an object"));
            request.args = json::Document(**arguments);
        }
        ATELIER_TRY_BIND(context_arg, args.find("context"));
        if (*context_arg) {
            ATELIER_TRY_BIND(context, context_override(**context_arg));
            request.context = std::move(*context);
        }
        // Build preserves in-memory author values and dirty state; rebuild:true explicitly retires history.
        // Unlike Open/New it saves Source drafts and materials itself, without discarding the scene.
        if (has_any(info->flags, editor::OperatorFlag::replaces_document) && request.id != "project.build")
            ATELIER_TRY(replaceable());
        const editor::ResolveReference resolve = [&](json::Value value, TypeId type,
                                                     ValueForm form) -> Result<std::optional<ObjectReference>> {
            if (value.kind() != json::Kind::string)
                return std::nullopt;
            auto& runtime = app.document_->world().runtime();
            const auto id = object(value);
            if (!id)
                return std::unexpected(Error{ErrorCode::invalid_storage, id.error().message});
            const auto target = runtime.hold(*id, type);
            if (!target)
                return std::nullopt;
            if (form == ValueForm::strong_reference)
                return target->reference();
            const auto reference = runtime.observe(target->reference());
            return reference ? std::optional{*reference} : std::nullopt;
        };
        const auto report = app.call_operator(request, false, resolve);
        if (!report) {
            const auto& error = report.error();
            return std::unexpected(error.code == ErrorCode::argument_mismatch
                                       ? Failure("invalid_request", error.message)
                                       : operation_failure(error));
        }
        if (report->status == editor::OperatorStatus::failed)
            return std::unexpected(Failure("poll_failed", report->message));
        if (report->status == editor::OperatorStatus::interaction_required)
            return std::unexpected(Failure("interaction_required", report->message));
        json::Builder response;
        response.set_validated(response.root(), "status", "ok");
        const auto result = response.object_validated(response.root(), "result");
        response.set_validated(result, "operator", request.id);
        response.set_validated(result, "status",
                               report->status == editor::OperatorStatus::cancelled ? "cancelled" : "finished");
        ATELIER_TRY(response.set(result, "message", report->message));
        response.set_validated(result, "result", report->result.root());
        ATELIER_TRY_BIND(selection, response.array(result, "selection"));
        for (const auto entity : app.document_->selected_entities()) {
            ATELIER_TRY_BIND(value_handle, handle(entity));
            ATELIER_TRY(response.append(*selection, *value_handle));
        }
        ATELIER_TRY(set_handle(response, result, "active", app.document_->selection().entity));
        ATELIER_TRY(set_handle(response, result, "inspected_object", app.document_->selection().object));
        return response.finish();
    }

    std::expected<json::Document, Failure> open_repro(json::Value args) {
        ATELIER_TRY(replaceable());
        ATELIER_TRY_BIND(directory_arg, args.at("directory"));
        ATELIER_TRY_BIND(directory, text(*directory_arg));
        const std::filesystem::path root = platform::utf8_to_utf16_validated(*directory);
        ATELIER_TRY_BIND(validated, control_artifacts::validate_repro(root));
        const auto manifest = validated->manifest.root();
        const auto schema = manifest.u32({"view_schema"});
        if (!schema || *schema != repro_view_schema)
            return std::unexpected(
                Failure("invalid_request", "Repro view schema is not current; export the repro again"));
        auto camera = app.camera_;
        ATELIER_TRY_BIND(camera_arg, manifest.at("camera"));
        ATELIER_TRY_BIND(state, operation(camera_state(app.document_->world().runtime(), *camera_arg)));
        if (!camera.set_state(*state))
            return std::unexpected(Failure("invalid_request", "Invalid repro camera"));
        const auto focus = camera.state().target;
        ATELIER_TRY_BIND(view_arg, manifest.at("view"));
        ATELIER_TRY_BIND(view, patched_view({}, *view_arg, focus));
        std::vector<unsigned> indices;
        ATELIER_TRY_BIND(selection, manifest.at("selection_indices"));
        ATELIER_TRY_BIND(items, selection->array());
        for (const auto index : *items) {
            ATELIER_TRY_BIND(i, integer(index, 0, editor::Session::maximum_entities));
            if (std::ranges::find(indices, *i) != indices.end())
                return std::unexpected(Failure("dependency_mismatch", "Invalid repro selection"));
            indices.push_back(*i);
        }
        std::optional<unsigned> inspection;
        ATELIER_TRY_BIND(inspection_arg, manifest.find("inspection"));
        if (*inspection_arg) {
            const auto value = **inspection_arg;
            ATELIER_TRY(value.members({"entity", "component"}));
            ATELIER_TRY_BIND(a, integer(*value.at("entity"), 0, editor::Session::maximum_entities));
            ATELIER_TRY_BIND(c, integer(*value.at("component"), 0, editor::Session::maximum_components));
            if (indices.size() != 1 || indices[0] != *a)
                return std::unexpected(Failure("dependency_mismatch", "Invalid repro component inspection"));
            inspection = *c;
        }

        ATELIER_TRY_BIND(candidate,
                         app.prepare_project_scene(std::move(validated->project), std::move(validated->scene_text),
                                                   ProjectOpenPolicy::require_scene)
                             .transform_error(operation_failure));
        auto& session = *candidate->document.session;
        ATELIER_TRY(control_artifacts::validate_candidate_assets(session, *validated));
        ATELIER_TRY_BIND(environment, session.presentation().environment().transform_error(operation_failure));
        // The scene owns the sky map and rotation. All other lighting fields must match the view snapshot.
        view->lit.sky_map = environment->sky_map;
        view->lit.sky_rotation = environment->sky_rotation;
        if (static_cast<const rendering::SceneLightingSettings&>(view->lit) != *environment)
            return std::unexpected(
                Failure("dependency_mismatch", "Repro frame snapshot disagrees with its Scene Environment"));
        ATELIER_TRY(operation(editor::validate_view_options(*view, focus)));

        ATELIER_TRY_BIND(members, session.world().members().transform_error(operation_failure));
        std::vector<ObjectId> selected;
        for (const auto i : indices) {
            if (i >= members->size())
                return std::unexpected(Failure("dependency_mismatch", "Repro selection exceeds scene size"));
            selected.push_back((*members)[i].id());
        }
        ATELIER_TRY(operation(session.select_entities(selected, selected.empty() ? ObjectId{} : selected.back())));
        if (inspection) {
            ATELIER_TRY_BIND(
                components,
                world::components(session.world().runtime(), selected.front()).transform_error(operation_failure));
            if (*inspection >= components->size())
                return std::unexpected(Failure("dependency_mismatch", "Invalid repro component inspection"));
            ATELIER_TRY(operation(session.select((*components)[*inspection].id())));
        }
        session.mark_saved();
        ATELIER_TRY(app.publish_project(std::move(*candidate)).transform_error(operation_failure));
        app.workspace_.apply_view_preferences(*view);
        app.camera_ = camera;
        app.camera_cut_ = true;
        app.workspace_.viewport_settings.grid_spacing = view->grid_spacing;
        return json::Document{};
    }

    std::expected<json::Document, Failure> graph_tools() const {
        json::Builder result;
        result.set_validated(result.root(), "scope", "graph");
        ATELIER_TRY_BIND(entries, result.array(result.root(), "operations"));
        for (std::string_view name : {"asset.new_procedural_graph", "graph.open", "graph.inspect", "graph.apply",
                                      "graph.save", "graph.create_scratch"}) {
            const auto direct = std::ranges::find(operations(), name, &Operation::name);
            if (direct != operations().end()) {
                ATELIER_TRY(append_operation(result, *entries, *direct));
                continue;
            }
            const auto* op = app.workspace_.operators.find(name);
            if (!op)
                return std::unexpected(Failure("operation_failed", "Unregistered graph tool: " + std::string(name)));
            const auto item = result.append_object(*entries);
            result.set_validated(item, "op", "op.call");
            ATELIER_TRY(result.set(item, "id", op->id));
            ATELIER_TRY(result.set(item, "description", op->description));
            ATELIER_TRY(result.set(item, "requires_state_token", true));
            ATELIER_TRY_BIND(
                schema,
                editor::parameter_schema(app.document_->world().runtime(), *op).transform_error(operation_failure));
            result.set_validated(item, "args", schema->root());
            if (!op->interaction.empty())
                ATELIER_TRY(result.set(item, "interaction", op->interaction));
        }
        return result.finish();
    }

    std::expected<void, Failure> graph_raycast(json::Builder& result, std::string_view key, std::string_view node_name,
                                               json::Value request) {
        ATELIER_TRY(request.members({"output", "rays"}));
        ATELIER_TRY_BIND(port_value, request.at("output"));
        ATELIER_TRY_BIND(port, text(*port_value, 256));
        if (port->empty())
            return std::unexpected(port_value->error(json::Fault::range, "Output port name must not be empty"));
        ATELIER_TRY_BIND(rays, request.at("rays"));
        ATELIER_TRY_BIND(entries, rays->array());
        if (!rays->size() || rays->size() > graph_ray_limit)
            return std::unexpected(rays->error(json::Fault::range, "Raycast requires 1 to 32 rays"));
        struct Ray {
            math::Vector3 origin, direction;
            float distance;
        };
        std::vector<Ray> queries;
        queries.reserve(rays->size());
        const auto vector = [](json::Value value) -> json::Result<math::Vector3> {
            ATELIER_TRY_BIND(array, value.array());
            if (value.size() != 3)
                return std::unexpected(value.error(json::Fault::range, "Expected graph-local [x,y,z]"));
            math::Vector3 v;
            unsigned i = 0;
            for (const auto component : *array) {
                ATELIER_TRY_BIND(number, component.f32());
                v[i++] = *number;
            }
            return v;
        };
        for (const auto ray : *entries) {
            ATELIER_TRY(ray.members({"origin", "direction", "max_distance"}));
            ATELIER_TRY_BIND(origin, vector(*ray.at("origin")));
            const auto direction_value = *ray.at("direction");
            ATELIER_TRY_BIND(direction, vector(direction_value));
            const auto unit = math::try_normalize(*direction);
            if (!unit)
                return std::unexpected(direction_value.error(json::Fault::range, "Ray direction must be nonzero"));
            const auto limit = *ray.at("max_distance");
            ATELIER_TRY_BIND(distance, limit.f32());
            if (*distance <= 0)
                return std::unexpected(limit.error(json::Fault::range, "Ray max_distance must be positive"));
            queries.push_back({*origin, *unit, *distance});
        }
        ATELIER_TRY_BIND(surface,
                         operation(app.document_->graph_node_surface(key, node_name, *port, app.project_.get())));
        const auto response = result.object_validated(result.root(), "raycast");
        ATELIER_TRY(result.set(response, "output", *port));
        ATELIER_TRY_BIND(hits, result.array(response, "hits"));
        for (const auto& ray : queries) {
            const auto hit = (*surface)->ray(ray.origin, ray.direction, ray.distance);
            if (!hit) {
                result.append_null(*hits);
                continue;
            }
            const auto row = result.append_object(*hits);
            ATELIER_TRY(result.set(row, "distance", hit->distance));
            ATELIER_TRY(numbers(result, row, "position", hit->position));
            ATELIER_TRY(result.set(row, "primitive", hit->primitive));
            ATELIER_TRY(result.set(row, "triangle", hit->triangle));
        }
        return {};
    }

    std::expected<json::Document, Failure> graph_catalog(json::Value args) {
        ATELIER_TRY(args.allowed_members({"view", "type"}));
        auto& doc = *app.document_;
        json::Builder result;
        const auto root = result.root();
        if (const auto query = *args.find("type")) {
            ATELIER_TRY_BIND(name, text(*query, 256));
            ATELIER_TRY_BIND(entry, operation(doc.procedural_node_type(*name, app.project_.get())));
            const auto& found = *entry;
            if (!found)
                return std::unexpected(Failure("invalid_request", "Unknown procedural node type: " + *name));
            ATELIER_TRY(write_graph_type(result, root, *found));
            ATELIER_TRY_BIND(attributes, result.array(root, "attribute_contracts"));
            if (found->help) {
                if (!found->help->ports.empty()) {
                    const auto descriptions = result.object_validated(root, "port_descriptions");
                    const auto inputs = result.object_validated(descriptions, "inputs");
                    const auto outputs = result.object_validated(descriptions, "outputs");
                    for (const auto& port : found->help->ports)
                        ATELIER_TRY(result.set(port.output ? outputs : inputs, port.port, port.description));
                }
                for (const auto& rule : found->help->attributes)
                    ATELIER_TRY(write_attribute_rule(result, result.append_object(*attributes), rule));
            }
            auto& runtime = doc.world().runtime();
            if (!found->asset_key.empty()) {
                ATELIER_TRY_BIND(params,
                                 operation(doc.graph_promoted_parameters(found->asset_key, app.project_.get())));
                ATELIER_TRY_BIND(entries, result.array(root, "parameters"));
                for (const auto& param : *params) {
                    const auto item = result.append_object(*entries);
                    ATELIER_TRY(result.set(item, "name", param.name));
                    ATELIER_TRY(result.set(item, "label", param.label));
                    ATELIER_TRY(result.set(item, "description", param.description));
                    if (param.valid) {
                        ATELIER_TRY_BIND(type, operation(runtime.describe(param.value_type)));
                        ATELIER_TRY(result.set(item, "type", (*type)->name));
                    } else {
                        ATELIER_TRY(result.set(item, "error", param.invalid_reason));
                    }
                }
            } else {
                ATELIER_TRY_BIND(fields, operation(editor::detail::author_fields(runtime, found->type)));
                ATELIER_TRY_BIND(entries, result.array(root, "fields"));
                for (const auto& field : *fields) {
                    if (field.read_only || field.form != ValueForm::inline_value)
                        continue;
                    ATELIER_TRY_BIND(info, operation(runtime.describe(field.type)));
                    const auto item = result.append_object(*entries);
                    ATELIER_TRY(result.set(item, "path", editor::detail::dotted(field.names)));
                    ATELIER_TRY(result.set(item, "type", (*info)->name));
                    ATELIER_TRY(result.set(item, "label", field.label));
                    ATELIER_TRY(result.set(item, "description", field.description));
                    ATELIER_TRY(result.set(item, "category", field.category));
                    const TypeInfo* leaf = *info;
                    if (leaf->kind == TypeKind::array) {
                        ATELIER_TRY_BIND(element, operation(runtime.describe(*leaf->element)));
                        leaf = *element;
                        ATELIER_TRY(result.set(item, "element_type", leaf->name));
                    }
                    if (leaf->enumeration) {
                        ATELIER_TRY_BIND(choices, result.array(item, "enum"));
                        for (const auto& member : leaf->enumeration->values)
                            ATELIER_TRY(result.append(*choices, member.first));
                        if (leaf->enumeration->flags)
                            ATELIER_TRY(result.set(item, "flags", true));
                    }
                    ATELIER_TRY(write_range(result, item, field.range, *leaf));
                    if (!field.metadata.empty()) {
                        const auto meta = result.object_validated(item, "metadata");
                        for (const auto& [key, value] : field.metadata)
                            ATELIER_TRY(result.set(meta, key, value));
                    }
                }
            }
        } else {
            ATELIER_TRY_BIND(catalog, operation(doc.procedural_node_types(app.project_.get())));
            ATELIER_TRY_BIND(entries, result.array(root, "node_types"));
            for (const auto& type : *catalog)
                ATELIER_TRY(write_graph_type(result, result.append_object(*entries), type));
        }
        return result.finish();
    }

    std::expected<json::Document, Failure> graph_request(std::string_view op, json::Value args) {
        auto& doc = *app.document_;
        if (op == "graph.create_scratch") {
            std::string key = "scratch/procedural";
            ATELIER_TRY_BIND(key_arg, args.find("key"));
            if (*key_arg) {
                ATELIER_TRY_BIND(value, text(**key_arg, 256));
                if (!value->empty())
                    key = std::move(*value);
            }
            ATELIER_TRY_BIND(graph, operation(doc.create_scratch_graph(key)));
            ATELIER_TRY(operation(app.open_graph_context(key)));
            json::Builder result;
            ATELIER_TRY(result.set(result.root(), "asset", key));
            ATELIER_TRY(set_handle(result, result.root(), "graph", *graph));
            return result.finish();
        }
        if (op == "graph.inspect") {
            if (*args.find("raycast") && (args.string({"view"}) != "node" || !*args.find("node")))
                return std::unexpected(Failure("invalid_request", "raycast requires view=node and an explicit node"));
            if (*args.find("bindings") && args.string({"view"}) != "node")
                return std::unexpected(Failure("invalid_request", "bindings requires view=node"));
            if (args.string({"view"}) == "catalog")
                return graph_catalog(args);
            if (*args.find("type"))
                return std::unexpected(Failure("invalid_request", "type requires view=catalog"));
        }
        ATELIER_TRY_BIND(asset_arg, args.find("asset"));
        std::string key = app.workspace_.active_graph;
        if (*asset_arg) {
            ATELIER_TRY_BIND(value, text(**asset_arg));
            key = std::move(*value);
        }
        if (key.empty() || !doc.has_graph(key))
            return std::unexpected(
                Failure("invalid_request",
                        op == "graph.inspect"
                            ? "Open the procedural graph first (via op.call graph.open or graph.create_scratch)"
                            : "Specify an open procedural graph or enter a graph context first"));
        ATELIER_TRY_BIND(node_arg, args.find("node"));
        std::string node_name = key == app.workspace_.active_graph ? app.workspace_.selected_graph_node : std::string{};
        if (*node_arg) {
            ATELIER_TRY_BIND(value, text(**node_arg, 256));
            node_name = std::move(*value);
        }
        if (op == "graph.apply") {
            ATELIER_TRY_BIND(changed, operation(doc.graph_apply(key, args, app.project_.get())));
            json::Builder result;
            ATELIER_TRY(result.set(result.root(), "asset", key));
            ATELIER_TRY(result.set(result.root(), "changed", *changed));
            return result.finish();
        }
        if (const auto mode = *args.find("view")) {
            ATELIER_TRY_BIND(value, text(*mode));
            if (*value == "topology") {
                for (const auto name : {"node", "timings", "samples", "spatial", "reference_node"})
                    if (*args.find(name))
                        return std::unexpected(Failure("invalid_request", std::string(name) + " requires view=full"));
                ATELIER_TRY_BIND(view, operation(doc.graph_topology(key)));
                return graph_inspect_json(**view, {}, {}, true);
            }
            if (*value == "node") {
                if (node_name.empty())
                    return std::unexpected(Failure("invalid_request", "view=node requires a node name"));
                for (const auto name : {"timings", "samples", "spatial", "reference_node"})
                    if (*args.find(name))
                        return std::unexpected(Failure("invalid_request", std::string(name) + " requires view=full"));
                ATELIER_TRY_BIND(props, operation(doc.graph_node_properties(key, node_name)));
                json::Builder result;
                ATELIER_TRY(result.set(result.root(), "asset", key));
                ATELIER_TRY(result.set(result.root(), "node", node_name));
                result.set_validated(result.root(), "properties", props->root());
                const auto prefix = node_name + ".";
                std::optional<json::Value> controls;
                for (const auto& parameter : doc.graph_document(key)->parameters)
                    for (std::string_view target : parameter.targets)
                        if (target.starts_with(prefix)) {
                            if (!controls)
                                controls = result.object_validated(result.root(), "controlled_fields");
                            ATELIER_TRY(result.set(*controls, target.substr(prefix.size()), parameter.name));
                        }
                if (const auto rays = *args.find("raycast"))
                    ATELIER_TRY(graph_raycast(result, key, node_name, *rays));
                if (const auto requested = *args.find("bindings")) {
                    ATELIER_TRY_BIND(enabled, requested->boolean());
                    if (*enabled) {
                        ATELIER_TRY_BIND(bindings,
                                         operation(doc.graph_node_bindings(key, node_name, app.project_.get())));
                        const auto object = result.object_validated(result.root(), "bindings");
                        ATELIER_TRY(result.set(object, "mode", bindings->mode));
                        ATELIER_TRY_BIND(entries, result.array(object, "attributes"));
                        for (const auto& binding : bindings->attributes) {
                            const auto item = result.append_object(*entries);
                            ATELIER_TRY(write_attribute_rule(result, item, binding.rule));
                            ATELIER_TRY(result.set(item, "name", binding.name));
                            ATELIER_TRY(result.set(item, "layer", binding.layer));
                            ATELIER_TRY(result.set(item, "status", binding.status));
                            if (auto upstream = doc.graph_node_name(binding.upstream))
                                ATELIER_TRY(result.set(item, "upstream", *upstream));
                            if (auto source = doc.graph_node_name(binding.column_source))
                                ATELIER_TRY(result.set(item, "column_source", *source));
                        }
                    }
                }
                return result.finish();
            }
            if (*value != "full")
                return std::unexpected(
                    Failure("invalid_request", "Graph view must be catalog, topology, node or full"));
        }
        ObjectId inspected_id{};
        if (!node_name.empty()) {
            const auto* node = doc.graph_node(key, node_name);
            if (!node)
                return std::unexpected(Failure("invalid_request", "Unknown node in procedural graph: " + node_name));
            inspected_id = node->node.id();
        }
        bool timings = false;
        ATELIER_TRY_BIND(timings_arg, args.find("timings"));
        if (*timings_arg) {
            ATELIER_TRY_BIND(flag, (**timings_arg).boolean());
            timings = *flag;
        }
        unsigned samples = 4;
        ATELIER_TRY_BIND(samples_arg, args.find("samples"));
        if (*samples_arg) {
            ATELIER_TRY_BIND(n, integer(**samples_arg, 0, 32));
            samples = *n;
        }
        bool spatial = true;
        ATELIER_TRY_BIND(spatial_arg, args.find("spatial"));
        if (*spatial_arg) {
            ATELIER_TRY_BIND(flag, (**spatial_arg).boolean());
            spatial = *flag;
        }
        std::string ref_name;
        ObjectId ref_id{};
        ATELIER_TRY_BIND(ref_arg, args.find("reference_node"));
        if (*ref_arg) {
            ATELIER_TRY_BIND(value, text(**ref_arg, 256));
            ref_name = std::move(*value);
            if (!ref_name.empty()) {
                const auto* ref_node = doc.graph_node(key, ref_name);
                if (!ref_node || !ref_node->node.registered())
                    return std::unexpected(Failure(
                        "invalid_request", "Unknown or unresolved reference_node in procedural graph: " + ref_name));
                ref_id = ref_node->node.id();
            }
        }
        ATELIER_TRY_BIND(view, operation(doc.inspect_graph(key, app.project_.get(), inspected_id, timings, samples,
                                                           spatial, ref_id, ref_name)));
        ATELIER_TRY_BIND(catalog, operation(doc.procedural_node_types(app.project_.get(), key)));
        return graph_inspect_json(*view, node_name, *catalog);
    }

    std::expected<ObjectId, Failure> material_object(json::Value handle) const {
        ATELIER_TRY_BIND(id, object(handle));
        const auto& doc = *app.document_;
        ATELIER_TRY_BIND(type, operation(doc.world().runtime().type_of(*id)));
        ATELIER_TRY_BIND(material, operation(doc.is_material_type(*type)));
        if (!*material)
            return std::unexpected(Failure("invalid_request", "Expected a material object"));
        return *id;
    }

    std::expected<json::Document, Failure> material_request(std::string_view op, json::Value args) {
        auto& doc = *app.document_;
        auto* project = app.project_.get();
        json::Builder result;
        if (op == "material.inspect") {
            ATELIER_TRY_BIND(asset_arg, args.find("asset"));
            ATELIER_TRY_BIND(object_arg, args.find("object"));
            if (asset_arg->has_value() == object_arg->has_value())
                return std::unexpected(Failure("invalid_request", "Specify exactly one material asset or object"));
            ObjectId target;
            if (*asset_arg) {
                if (!project)
                    return std::unexpected(Failure("not_ready", "No project is open"));
                ATELIER_TRY_BIND(key, text(**asset_arg));
                ATELIER_TRY_BIND(loaded, operation(doc.open_material(*project, *key)));
                target = loaded->id();
            } else {
                ATELIER_TRY_BIND(found, material_object(**object_arg));
                target = *found;
            }
            ATELIER_TRY(write_object(result, result.root(), target));
        } else if (op == "material.create_scene" || op == "material.assign") {
            ATELIER_TRY_BIND(component, object(*args.at("component")));
            ATELIER_TRY_BIND(slot, text(*args.at("slot")));
            if (op == "material.create_scene") {
                ATELIER_TRY_BIND(created, operation(doc.create_scene_material(*component, *slot)));
                ATELIER_TRY(set_handle(result, result.root(), "object", created->id()));
            } else {
                ATELIER_TRY_BIND(asset, text(*args.at("asset")));
                if (asset->empty()) {
                    ATELIER_TRY(operation(doc.clear_material(*component, *slot)));
                } else {
                    if (!project)
                        return std::unexpected(Failure("not_ready", "No project is open"));
                    ATELIER_TRY(operation(doc.assign_material(*project, *component, *slot, *asset)));
                }
            }
        } else if (op == "material.override" || op == "material.discard" || op == "material.set_entry") {
            ATELIER_TRY_BIND(target, material_object(*args.at("object")));
            if (op == "material.set_entry") {
                json::Builder input(json::Kind::array);
                input.append(input.root(), *args.at("entry"));
                ATELIER_TRY_BIND(entries, assets::material_format::decode_entries(input.root()));
                ATELIER_TRY(operation(doc.set_material_entry(*target, std::move(entries->front()), project)));
            } else {
                ATELIER_TRY_BIND(path, text(*args.at("path")));
                if (op == "material.override") {
                    ATELIER_TRY_BIND(enabled, args.boolean({"enabled"}));
                    ATELIER_TRY(operation(doc.set_material_override(*target, *path, *enabled, project)));
                } else {
                    ATELIER_TRY(operation(doc.discard_material_override(*target, *path)));
                }
            }
        } else {
            if (!project)
                return std::unexpected(Failure("not_ready", "No project is open"));
            if (op == "material.save") {
                ATELIER_TRY_BIND(asset, text(*args.at("asset")));
                ATELIER_TRY(operation(doc.save_material(*project, *asset)));
                ATELIER_TRY(result.set(result.root(), "saved", true));
                if (const auto refreshed = project->refresh(); !refreshed)
                    ATELIER_TRY(result.set(result.root(), "catalog_warning", refreshed.error().message));
            } else if (op == "material.set_texture") {
                ATELIER_TRY_BIND(target, object(*args.at("object")));
                ATELIER_TRY_BIND(path, text(*args.at("path")));
                ATELIER_TRY_BIND(texture, text(*args.at("texture")));
                ATELIER_TRY(operation(doc.set_material_texture(*project, *target, *path, *texture)));
            } else {
                return std::unexpected(Failure("invalid_request", "Unknown material operation"));
            }
        }
        return result.finish();
    }

    std::expected<json::Document, Failure> dispatch(std::string_view op, json::Value args, const std::string& id,
                                                    const std::shared_ptr<control::Peer>& peer) {
        if (op == "capabilities") {
            if (const auto scope = *args.find("scope")) {
                if (scope->string() != "graph")
                    return std::unexpected(Failure("invalid_request", "Unknown capabilities scope"));
                return graph_tools();
            }
            return capabilities();
        }
        if (op == "op.list")
            return operator_list();
        if (op == "editor.inspect") {
            ATELIER_TRY_BIND(detail_arg, args.find("detail"));
            std::string detail = "full";
            if (*detail_arg) {
                ATELIER_TRY_BIND(name, text(**detail_arg));
                detail = std::move(*name);
            }
            if (detail == "assets")
                return inspect_assets(args);
            if (detail == "objects")
                return inspect_objects(args);
            if (detail == "object") {
                ATELIER_TRY(args.allowed_members({"detail", "object"}));
                ATELIER_TRY_BIND(target_arg, args.at("object"));
                ATELIER_TRY_BIND(target, object(*target_arg));
                json::Builder result;
                result.set_validated(result.root(), "detail", "object");
                ATELIER_TRY(write_object(result, result.object_validated(result.root(), "object"), *target));
                return result.finish();
            }
            ATELIER_TRY(args.allowed_members({"detail"}));
            if (detail != "full" && detail != "summary")
                return std::unexpected(Failure("invalid_request", "Unsupported editor inspection detail"));
            return inspect(detail == "summary");
        }
        if (op.starts_with("material."))
            return material_request(op, args);
        if (op == "graph.inspect")
            return graph_request(op, args);
        if (op == "job.status" || op == "job.cancel") {
            ATELIER_TRY_BIND(job_id, text(*args.at("job_id"), 80));
            for (auto& job : jobs)
                if (job->id == *job_id) {
                    if (op == "job.cancel") {
                        cancel(*job, "cancelled", "Cancelled by control client");
                    }
                    if (job->complete)
                        return job->response;
                    json::Builder response;
                    response.set_validated(response.root(), "status", "pending");
                    response.set_validated(response.root(), "job_id", job->id);
                    const auto progress = response.object_validated(response.root(), "result");
                    response.set_validated(progress, "phase",
                                           job->preparing   ? "preparing"
                                           : job->rendering ? "rendering"
                                           : job->tracing   ? "observing"
                                                            : "writing");
                    ATELIER_TRY(response.set(progress, "case_index", job->case_index));
                    ATELIER_TRY(response.set(progress, "accepted_frames", job->accepted));
                    ATELIER_TRY(response.set(progress, "cancel_requested", job->work->cancelled()));
                    if (job->preparing)
                        if (const auto prepared = app.renderer_.preparation(job->cases.front().view)) {
                            ATELIER_TRY(response.set(progress, "compiled", prepared->done));
                            ATELIER_TRY(response.set(progress, "total", prepared->total));
                        }
                    return response.finish();
                }
            return std::unexpected(Failure("unknown_job", "Job expired or belongs to another instance"));
        }
        ATELIER_TRY(idle());
        if (op == "op.call")
            return call_operator(args);
        if (op == "graph.create_scratch" || op == "graph.apply")
            return graph_request(op, args);
        if (op == "repro.export")
            return start_export(id, peer);
        else if (op == "render.trace")
            return start_trace(id, peer, args);
        else if (op == "render.capture" || op == "render.compare" || op == "render.prepare")
            return start_render(id, peer, args, op == "render.compare", op == "render.prepare");
        else if (op == "repro.open")
            return open_repro(args);
        return std::unexpected(Failure("unknown_operation", "Operation has no implementation"));
    }

    std::expected<json::Document, Failure> request(json::Value request, const std::shared_ptr<control::Peer>& peer) {
        app.sync_world_view();
        json::Document response;
        std::string id;
        Array<char> canonical;
        bool remember{};
        bool replay{};
        const auto execute = [&]() -> std::expected<json::Document, Failure> {
            ATELIER_TRY(
                request.allowed_members({"protocol", "instance_id", "request_id", "op", "args", "state_token"}));
            ATELIER_TRY_BIND(protocol, request.u32({"protocol"}));
            ATELIER_TRY_BIND(instance, request.string({"instance_id"}));
            if (*protocol != control::protocol_version ||
                *instance != *server.identity().root().string({"instance_id"}))
                return std::unexpected(Failure("protocol_mismatch", "Wrong protocol or editor instance"));
            ATELIER_TRY_BIND(id_arg, request.at("request_id"));
            ATELIER_TRY_BIND(identity, text(*id_arg, 80));
            id = std::move(*identity);
            if (id.empty())
                return std::unexpected(Failure("invalid_request", "Empty request identity"));
            canonical = json::write(request);
            for (const auto& entry : cache)
                if (entry.id == id) {
                    if (!std::ranges::equal(entry.request, canonical))
                        return std::unexpected(
                            Failure("request_id_conflict", "Request identity was used for different content"));
                    replay = true;
                    return entry.response;
                }
            if (std::ranges::any_of(jobs, [&](const auto& job) { return job->id == id; }))
                return std::unexpected(
                    Failure("request_id_expired", "Request payload expired; query the retained job using job.status"));
            ATELIER_TRY_BIND(op_arg, request.at("op"));
            ATELIER_TRY_BIND(name, text(*op_arg, 80));
            const auto& known = operations();
            const auto op = std::ranges::find(known, *name, &Operation::name);
            if (op == known.end())
                return std::unexpected(Failure("unknown_operation", "Operation is not exposed by this editor"));
            ATELIER_TRY_BIND(args_arg, request.find("args"));
            const json::Document empty_args;
            const auto args = args_arg->value_or(empty_args.root());
            ATELIER_TRY_BIND(items, args.object_items());
            for (const auto& [key, value] : *items)
                if (std::ranges::none_of(op->properties, [&](const auto& field) { return field.first == key; }))
                    return std::unexpected(Failure("invalid_request", "Unknown argument: " + std::string(key)));
            for (const auto& field : op->required) {
                ATELIER_TRY(args.at(field));
            }
            observe();
            remember = op->changes;
            if (op->changes) {
                ATELIER_TRY_BIND(token_arg, request.find("state_token"));
                std::string_view client_token;
                if (*token_arg) {
                    ATELIER_TRY_BIND(value, (**token_arg).string());
                    client_token = *value;
                }
                if (client_token != token())
                    return std::unexpected(Failure("stale_state", stale_message(client_token, *name, args)));
            }
            ATELIER_TRY_BIND(result, dispatch(*name, args, id, peer));
            ATELIER_TRY_BIND(status, result->root().find("status"));
            if (*status)
                return std::move(*result);
            json::Builder output;
            output.set_validated(output.root(), "status", "ok");
            output.set_validated(output.root(), "result", result->root());
            return output.finish();
        };
        auto result = execute();
        if (result)
            response = std::move(*result);
        else {
            const auto& error = result.error();
            ATELIER_TRY_BIND(failure, control::failure(error.code, diagnostic_utf8(error.message)));
            response = std::move(*failure);
            if (error.code == "busy" && busy()) {
                ATELIER_TRY_BIND(reasons, busy_reasons());
                json::Builder output(response);
                output.set_validated(*output.root().at("error"), "busy_reasons", reasons->root());
                response = output.finish();
            }
        }
        if (replay)
            return response;
        observe();
        json::Builder output(response);
        output.set_validated(output.root(), "state_token", token());
        response = output.finish();
        if (remember) {
            const auto bytes = canonical.size() + json::write(response.root()).size();
            cache_bytes += bytes;
            cache.push_back({id, std::move(canonical), response, bytes});
            trim_cache();
        }
        return response;
    }
};

std::expected<EditorControl, control::Failure> EditorControl::create(Application& app,
                                                                     const std::filesystem::path& directory) {
    json::Builder build;
    build.set_validated(build.root(), "revision", build_info::revision);
    build.set_validated(build.root(), "configuration", build_info::configuration);
    auto server = control::Server::create(directory, build.finish());
    if (!server)
        return std::unexpected(std::move(server.error()));
    const auto native = platform::windows_file_path(server->directory() / "artifacts" /
                                                    *server->identity().root().string({"instance_id"}));
    if (!native)
        return std::unexpected(Failure("storage_error", platform::error_message(native.error())));
    std::error_code error;
    std::filesystem::create_directories(*native, error);
    if (error)
        return std::unexpected(Failure("storage_error", platform::error_message(error)));
    return EditorControl(std::make_unique<Impl>(app, std::move(*server)));
}

EditorControl::EditorControl(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

EditorControl::EditorControl(EditorControl&&) noexcept = default;

EditorControl::~EditorControl() = default;

std::optional<EditorControl::ActiveJobInfo> EditorControl::active_job() const {
    const auto* job = impl_->active();
    if (!job)
        return std::nullopt;
    const std::string_view phase = job->preparing   ? "preparing"
                                   : job->rendering ? "rendering"
                                   : job->tracing   ? "observing"
                                                    : "writing";
    return ActiveJobInfo{job->id, job->kind, std::string(phase)};
}

bool EditorControl::poll() {
    bool serviced = impl_->active() != nullptr;
    impl_->check_jobs();
    impl_->server.poll([&](const auto& request, const auto& peer) {
        serviced = true;
        return impl_->request(request, peer);
    });
    impl_->check_jobs();
    return serviced;
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
    if (!impl_->check_jobs())
        return false;
    const auto* job = impl_->active();
    return job && job->rendering && !job->preparing && job->accepted + 1 == job->points[job->point_index];
}

rhi::CommandTrace* EditorControl::command_trace() const {
    const auto* job = impl_->active();
    return job && job->tracing ? job->trace.get() : nullptr;
}

void EditorControl::traced(const renderer::Frame& frame, const renderer::Result<renderer::FrameResult>& result,
                           bool host_stopping) {
    auto& state = *impl_;
    auto* job = state.active();
    if (!job || !job->tracing || frame.command_trace != job->trace.get())
        return;
    job->tracing = false;
    const auto snapshot = [&]() -> json::Result<json::Document> {
        json::Builder metadata(job->manifest);
        const auto root = metadata.root();
        ATELIER_TRY(metadata.set(root, "ui_frame_id", frame.ui_frame.id));
        ATELIER_TRY(metadata.set(root, "scene_identity", frame.scene_identity));
        const std::array extent{frame.ui_frame.extent.width, frame.ui_frame.extent.height};
        ATELIER_TRY(metadata.numbers(root, "extent", std::span{extent}));
        ATELIER_TRY_BIND(view, view_json(frame.view));
        metadata.set_validated(root, "effective_view", view->root());
        metadata.set_validated(root, "attempt", "first_requested_view");
        if (result) {
            constexpr std::array statuses{"accepted", "skipped", "not_ready", "cancelled"};
            metadata.set_validated(root, "frame_status", statuses[std::to_underlying(result->status)]);
            ATELIER_TRY(metadata.set(root, "accepted_id", result->accepted_id));
            const std::array accepted{result->accepted_extent.width, result->accepted_extent.height};
            const std::array next{result->next_extent.width, result->next_extent.height};
            ATELIER_TRY(metadata.numbers(root, "accepted_extent", std::span{accepted}));
            ATELIER_TRY(metadata.numbers(root, "next_extent", std::span{next}));
        } else {
            metadata.set_validated(root, "frame_status", "error");
            const auto error = metadata.object_validated(root, "error");
            ATELIER_TRY(metadata.set(error, "code", static_cast<std::int64_t>(result.error().cause.code)));
            ATELIER_TRY(
                metadata.set(error, "native_result", static_cast<std::int64_t>(result.error().cause.native_result)));
            metadata.set_validated(error, "message", diagnostic_utf8(result.error().cause.message));
            ATELIER_TRY(metadata.set(error, "terminal", result.error().terminal));
        }
        return metadata.finish();
    };
    auto metadata = snapshot();
    if (!metadata) {
        state.cancel(*job, "invalid_request", metadata.error().describe());
        return;
    }
    state.start_writer(*job, [path = job->directory, trace = std::move(job->trace),
                              metadata = std::move(*metadata)](control_artifacts::WriteState& work) mutable {
        return control_artifacts::write_trace(path, std::move(*trace), std::move(metadata), work);
    });
    // A terminal error in either the first render or a later fallback tears
    // down the host and cancels jobs. Preserve the first attempt beforehand.
    // This is owned CPU file output only, never a GPU wait or a render retry.
    if (host_stopping)
        job->writer.wait();
}

void EditorControl::render_failed(std::string_view message) {
    if (auto* job = impl_->active(); job && job->rendering)
        impl_->cancel(*job, "render_failure", message);
}

void EditorControl::rendered(const renderer::Frame& frame, const renderer::FrameResult& result,
                             renderer::Capture* capture) {
    auto& state = *impl_;
    if (!state.check_jobs())
        return;
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
        const auto frame_info = state.frame_json();
        if (!frame_info) {
            state.cancel(*job, "invalid_request", frame_info.error().describe());
            return;
        }
        json::Builder response;
        response.set_validated(response.root(), "status", "ok");
        response.set_validated(response.root(), "job_id", job->id);
        const auto ready = response.object_validated(response.root(), "result");
        response.set_validated(ready, "phase", "ready");
        response.set_validated(ready, "frame", frame_info->root());
        job->rendering = false;
        job->preparing = false;
        job->complete = true;
        job->response = response.finish();
        return;
    }
    if (job->accepted != job->points[job->point_index])
        return;
    if (!capture || capture->frame_id != result.accepted_id || capture->extent != result.accepted_extent) {
        state.cancel(*job, "render_failure", "Capture does not belong to the accepted frame");
        return;
    }
    const auto snapshot = [&]() -> json::Result<json::Document> {
        ATELIER_TRY_BIND(frame_info, state.frame_json());
        json::Builder metadata(*frame_info);
        const auto root = metadata.root();
        metadata.set_validated(root, "case", job->cases[job->case_index].name);
        ATELIER_TRY(metadata.set(root, "accepted_ordinal", job->accepted));
        ATELIER_TRY(metadata.set(root, "warmup_accepted_frames", job->accepted - 1));
        ATELIER_TRY(metadata.set(root, "skipped_frames", job->skipped));
        ATELIER_TRY(metadata.set(root, "case_reported_history_resets", job->history_resets));
        return metadata.finish();
    };
    auto metadata = snapshot();
    if (!metadata) {
        state.cancel(*job, "invalid_request", metadata.error().describe());
        return;
    }
    job->samples.push_back({std::move(*capture), std::move(*metadata)});
    if (++job->point_index == job->points.size()) {
        job->point_index = 0;
        job->accepted = 0;
        job->skipped = 0;
        job->history_resets = 0;
        if (++job->case_index == job->cases.size()) {
            job->rendering = false;
            state.start_writer(*job, [path = job->directory, samples = std::move(job->samples),
                                      manifest = job->manifest](control_artifacts::WriteState& work) mutable {
                return control_artifacts::write_samples(path, std::move(samples), std::move(manifest), work);
            });
        }
    }
}
}
