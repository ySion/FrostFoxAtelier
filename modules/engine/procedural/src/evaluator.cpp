#include "procedural/evaluator.hpp"
#include "procedural/bvh.hpp"
#include "procedural/nodes.hpp"
#include "procedural/random.hpp"
#include "detail/native_cook.hpp"
#include "detail/node_plan.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/map.hpp"
#include "foundation/result.hpp"
#include "source/program.hpp"

#include <algorithm>
#include <array>
#include <list>
#include <string>
#include <tuple>
#include <utility>

namespace atelier::procedural {
namespace {

std::unexpected<Error> error(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

template <class T>
void append(std::string& key, const T& value) {
    key.append(reinterpret_cast<const char*>(&value), sizeof value);
}

struct SourceKernel {
    source::Program::Entry entry;

    std::uint64_t identity{};
};

template <std::size_t I>
using ColumnArray = Array<typename std::variant_alternative_t<I, ColumnData>::Value>;

template <std::size_t... I>
using EmptyColumnsTuple = std::tuple<ColumnArray<I>...>;

const auto& empty_columns() noexcept {
    static const auto instances = []<std::size_t... I>(std::index_sequence<I...>) {
        return EmptyColumnsTuple<I...>{};
    }(std::make_index_sequence<detail::column_type_count>{});
    return instances;
}

template <std::size_t I>
Result<const void*> read_column(const Geometry& geometry, StringView name, Layer layer, bool optional) {
    using T = typename ColumnArray<I>::Value;
    ATELIER_TRY_BIND(column, geometry.read<T>(name, layer));
    if (!*column) {
        if (optional)
            return &std::get<I>(empty_columns());
        return failure("The input has no attribute " + std::string{name.std_view()} + ".");
    }
    return *column;
}

template <std::size_t I>
Result<void*> inout_column(Geometry& geometry, StringView name, Layer layer) {
    using T = typename ColumnArray<I>::Value;
    ATELIER_TRY_BIND(column, geometry.write<T>(name, layer));
    return *column;
}

template <std::size_t I>
Result<void*> out_column(Geometry& geometry, StringView name, Layer layer) {
    using T = typename ColumnArray<I>::Value;
    ATELIER_TRY_BIND(column, geometry.add<T>(name, layer));
    return *column;
}

template <std::size_t I>
std::size_t borrowed_column_size(const void* column) noexcept {
    return static_cast<const ColumnArray<I>*>(column)->size();
}

struct ColumnAccess {
    Result<const void*> (*read)(const Geometry&, StringView, Layer, bool);

    Result<void*> (*inout)(Geometry&, StringView, Layer);

    Result<void*> (*out)(Geometry&, StringView, Layer);

    std::size_t (*size)(const void*) noexcept;
};

template <std::size_t... I>
constexpr std::array<ColumnAccess, sizeof...(I)> column_accessors(std::index_sequence<I...>) {
    return {ColumnAccess{&read_column<I>, &inout_column<I>, &out_column<I>, &borrowed_column_size<I>}...};
}

constexpr auto accessors = column_accessors(std::make_index_sequence<detail::column_type_count>{});

// One evaluation of one node. Keys are exact byte strings, so equal keys mean equal contents.
struct Entry {
    std::string key;

    std::uint64_t serial{}; // Unique per evaluation; downstream keys use it.

    std::uint32_t subgraph_span{}; // Active AssetNode hops beneath and including this entry.

    std::vector<Geometry> outputs;

    std::vector<Reference> dependencies; // Resources read, with what they resolved to.

    const Resources* confirmed_resources{}; // Host instance `revision` was confirmed against.

    std::uint64_t revision{}; // Resources revision the dependencies were confirmed at.

    std::size_t bytes{};

    bool cached{};

    std::list<Entry*>::iterator recent; // Valid while cached.
};

}

struct CachePool::Impl {
    std::size_t budget;

    detail::BooleanPreparationCache boolean_preparation;

    Map<std::string, std::shared_ptr<Entry>> cache;

    std::list<Entry*> recent; // Most recently used first.

    std::size_t cached_bytes{};

    std::uint64_t serials{};

    explicit Impl(std::size_t bytes) : budget(bytes) {}

    void touch(Entry& entry) {
        if (entry.cached)
            recent.splice(recent.begin(), recent, entry.recent);
    }

    void uncache(Entry& entry) {
        entry.cached = false;
        cached_bytes -= entry.bytes;
        recent.erase(entry.recent);
        const std::string key = entry.key;
        (void)cache.remove(key); // May release the entry; touch nothing after this.
    }

    void trim() {
        for (auto& slot : boolean_preparation.operands)
            if (slot.prepared && slot.input.expired())
                slot = {};
        while (!recent.empty() && cached_bytes + boolean_preparation.bytes() > budget)
            uncache(*recent.back());
    }

    void store(const std::shared_ptr<Entry>& entry) {
        if (auto* old = cache.find(entry->key))
            uncache(**old);
        cache.insert_or_assign(entry->key, entry);
        recent.push_front(entry.get());
        entry->recent = recent.begin();
        entry->cached = true;
        cached_bytes += entry->bytes;
        trim();
    }
};

CachePool::CachePool(std::size_t budget_bytes) : impl_(std::make_unique<Impl>(budget_bytes)) {}

CachePool::~CachePool() = default;

struct Evaluator::Impl {
    struct State {
        TypeId type{};

        const detail::NodePlan* plan{};

        std::unique_ptr<const detail::NodePlan> instance_plan;

        bool is_asset_node{};

        AssetSubgraph asset_subgraph{};

        std::shared_ptr<Evaluator> child_evaluator;

        bool bypassed{};

        bool frozen{};

        std::shared_ptr<const FrozenOutput> frozen_source;

        std::shared_ptr<Entry> frozen_entry;

        std::string local; // Type and public field contents.

        std::vector<Input> inputs;

        std::optional<Error> fault; // Reading or keying the node failed.

        std::shared_ptr<Entry> entry; // The node's current result.

        std::string failed; // Key of the last failed evaluation, at failed_revision.

        std::uint64_t failed_revision{};

        Error failure{};

        std::optional<Error> root_error; // Root-cause diagnostic carried to blocked downstream nodes.

        std::uint64_t synced_generation{};

        NodeStatus status;
    };

    Runtime& runtime;

    ObjectRoot graph;

    const Resources& resources;

    const source::Program* program;

    std::shared_ptr<CachePool> pool;

    std::uint32_t subgraph_depth{};

    bool has_external_inputs{};

    std::vector<ExternalInput> external_inputs;

    std::uint64_t external_inputs_epoch{};

    std::uint64_t synced_external_epoch{};

    const Geometry empty;

    TypeId string_type{};

    TypeId boolean_type{};

    TypeId graph_input_type{};

    TypeId asset_node_type{};

    FieldId asset_seed_offset_field{};

    FieldId graph_seed_field{};

    std::optional<std::uint64_t> cursor;

    std::uint64_t revision{};

    std::uint64_t code_revision{}; // Of the program, when Source entries were bound.

    std::uint32_t authored_seed{};

    std::uint32_t seed{};

    Contents members;

    Map<ObjectId, State> nodes;

    Map<TypeId, std::unique_ptr<const Result<detail::NodePlan>>> plans;

    Map<TypeId, Result<SourceKernel>> source_kernels;

    std::uint64_t sync_generation{1};

    bool display_current{};

    std::shared_ptr<Entry> current_published;

    std::shared_ptr<Entry> last_valid;

    Impl(Runtime& owner, ObjectRoot root, const Resources& host, const source::Program* code,
         std::shared_ptr<CachePool> cache_pool, std::uint32_t depth)
        : runtime(owner), graph(std::move(root)), resources(host), program(code),
          pool(cache_pool ? std::move(cache_pool) : std::make_shared<CachePool>()), subgraph_depth(depth),
          code_revision(code ? code->revision() : 0) {
        if (auto str = runtime.native_type<String>())
            string_type = *str;
        if (auto op = runtime.native_type<Boolean>())
            boolean_type = *op;
        if (auto gin = runtime.native_type<GraphInput>())
            graph_input_type = *gin;
        if (auto anode = runtime.native_type<AssetNode>()) {
            asset_node_type = *anode;
            if (auto found = runtime.find_field(*anode, "seed_offset"))
                asset_seed_offset_field = *found;
        }
        if (auto graph_type = runtime.native_type<Graph>())
            if (auto found = runtime.find_field(*graph_type, "seed"))
                graph_seed_field = *found;
    }

    Result<SourceKernel> bind_source(TypeId type) const {
        ATELIER_TRY_BIND(info, runtime.describe(type));
        const auto& name = (*info)->name;
        if (!program)
            return error(ErrorCode::unsupported_operation, "Source node " + name + " needs a program.");
        const auto entries = program->entries();
        const std::string entry_name = name + ".process";
        const auto found = std::ranges::find(entries, entry_name, &source::EntryInfo::name);
        if (found == entries.end())
            return error(ErrorCode::invalid_definition, name + " has no published process entry.");
        ATELIER_TRY_BIND(entry, program->entry(found->name));
        return SourceKernel{std::move(*entry), found->code};
    }

    Result<void> append_value(std::string& key, const TypeInfo& info, const void* data) const {
        if (info.kind == TypeKind::scalar || info.kind == TypeKind::enumeration || info.math) {
            key.append(static_cast<const char*>(data), info.size);
            return {};
        }
        if (info.kind == TypeKind::record && info.plain_value) {
            const std::span source{static_cast<const std::byte*>(data), info.size};
            for (const auto& field : info.fields) {
                auto child = runtime.describe(field.value.type);
                if (!child)
                    return std::unexpected(child.error());
                if (field.offset) {
                    if (auto added = append_value(key, **child, source.data() + *field.offset); !added)
                        return added;
                    continue;
                }
                atelier::detail::ValueStorage storage;
                const std::span bytes{storage.prepare((*child)->size, (*child)->alignment), (*child)->size};
                if (auto made = runtime.initialize_value(field.value.type, bytes); !made)
                    return made;
                auto added = runtime.read(info.id, source, {&field.id, 1}, ValueOutput{field.value.type, bytes.data()});
                if (added)
                    added = append_value(key, **child, bytes.data());
                (void)runtime.destroy_value(field.value.type, bytes);
                if (!added)
                    return added;
            }
            return {};
        }
        if ((info.kind != TypeKind::string && info.kind != TypeKind::array) || !info.sequence_view)
            return error(ErrorCode::unsupported_operation,
                         "Node parameters of type " + info.name + " are not supported.");
        const void* items{};
        std::size_t count{};
        info.sequence_view(data, &items, &count);
        append(key, count);
        if (info.kind == TypeKind::string) {
            key.append(static_cast<const char*>(items), count);
            return {};
        }
        auto element = runtime.describe(*info.element);
        if (!element)
            return std::unexpected(element.error());
        for (std::size_t i = 0; i < count; ++i)
            if (auto added = append_value(key, **element, static_cast<const std::byte*>(items) + i * (*element)->size);
                !added)
                return added;
        return {};
    }

    Result<void> append_field(std::string& key, ObjectId object, const FieldInfo& field) {
        if (field.value.form != ValueForm::inline_value)
            return error(ErrorCode::unsupported_operation, "Node parameter " + field.name + " is a reference.");
        auto info = runtime.describe(field.value.type);
        if (!info)
            return std::unexpected(info.error());
        if ((*info)->kind == TypeKind::scalar || (*info)->kind == TypeKind::enumeration) {
            auto value = runtime.read(object, field.id);
            if (!value)
                return std::unexpected(value.error());
            key.push_back(static_cast<char>(value->index()));
            std::visit(
                [&](const auto& scalar) {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(scalar)>, EnumValue>)
                        append(key, scalar.bits);
                    else
                        append(key, scalar);
                },
                *value);
            return {};
        }
        atelier::detail::ValueStorage storage;
        const std::span bytes{storage.prepare((*info)->size, (*info)->alignment), (*info)->size};
        if (auto made = runtime.initialize_value(field.value.type, bytes); !made)
            return made;
        auto added = runtime.read(object, field.id, ValueOutput{field.value.type, bytes.data()});
        if (added)
            added = append_value(key, **info, bytes.data());
        (void)runtime.destroy_value(field.value.type, bytes);
        return added;
    }

    Result<void> read(ObjectId id, State& state, const State* prior) {
        if (!state.plan)
            return std::unexpected(*state.fault);
        ATELIER_TRY_BIND(is_bypassed, bypassed(runtime, id));
        ATELIER_TRY_BIND(is_frozen, frozen(runtime, id));
        ATELIER_TRY_BIND(snap, frozen_output(runtime, id));
        state.bypassed = *is_bypassed;
        state.frozen = *is_frozen;
        state.frozen_source.reset();
        state.frozen_entry.reset();
        if (state.frozen) {
            if (*snap) {
                if (prior && prior->frozen_source == *snap && prior->frozen_entry) {
                    state.frozen_source = prior->frozen_source;
                    state.frozen_entry = prior->frozen_entry;
                } else {
                    auto frozen_e = std::make_shared<Entry>();
                    frozen_e->serial = ++pool->impl_->serials;
                    frozen_e->outputs = (*snap)->outputs;
                    if (frozen_e->outputs.size() < state.plan->ports.outputs.size())
                        frozen_e->outputs.resize(state.plan->ports.outputs.size());
                    frozen_e->dependencies = (*snap)->dependencies;
                    frozen_e->revision = revision;
                    state.frozen_source = *snap;
                    state.frozen_entry = std::move(frozen_e);
                }
            } else if (prior && prior->frozen_entry) {
                state.frozen_entry = prior->frozen_entry;
            } else if (prior && !prior->bypassed && !prior->status.error && prior->entry) {
                state.frozen_entry = prior->entry;
            }
            if (state.frozen_entry && state.frozen_entry->outputs.size() < state.plan->ports.outputs.size()) {
                auto resized = std::make_shared<Entry>();
                resized->serial = ++pool->impl_->serials;
                resized->outputs = state.frozen_entry->outputs;
                resized->outputs.resize(state.plan->ports.outputs.size());
                resized->dependencies = state.frozen_entry->dependencies;
                resized->revision = revision;
                state.frozen_entry = std::move(resized);
            }
        }
        if (state.is_asset_node && prior && prior->child_evaluator &&
            prior->asset_subgraph.graph.id() == state.asset_subgraph.graph.id()) {
            state.child_evaluator = prior->child_evaluator;
        }
        const bool locked_frozen = !state.bypassed && state.frozen && state.frozen_entry != nullptr;
        auto info = runtime.describe(state.type);
        if (!info)
            return std::unexpected(info.error());
        if (!state.plan->native && !state.bypassed && !locked_frozen) {
            auto* bound = source_kernels.find(state.type);
            if (!bound)
                bound = source_kernels.try_emplace(state.type, bind_source(state.type)).value;
            if (!*bound)
                return std::unexpected(bound->error());
        }
        std::string local;
        append(local, *info); // Published TypeInfo addresses are stable identities for this Runtime.
        append(local, state.bypassed);
        append(local, state.frozen);
        if (locked_frozen) {
            append(local, state.frozen_entry->serial);
        } else if (!state.bypassed) {
            for (const auto& field : (*info)->fields)
                if (auto added = append_field(local, id, field); !added)
                    return added;
            if (state.is_asset_node) {
                append(local, state.asset_subgraph.asset_key.size());
                local.append(state.asset_subgraph.asset_key);
                append(local, state.asset_subgraph.revision);
                append(local, state.asset_subgraph.ports.inputs.size());
                for (std::size_t p = 0; p < state.asset_subgraph.ports.inputs.size(); ++p) {
                    append(local, state.asset_subgraph.ports.inputs[p].size());
                    local.append(state.asset_subgraph.ports.inputs[p]);
                    const bool multi =
                        p < state.asset_subgraph.ports.multiple.size() && state.asset_subgraph.ports.multiple[p];
                    append(local, multi);
                }
                append(local, state.asset_subgraph.ports.outputs.size());
                for (const auto& out_name : state.asset_subgraph.ports.outputs) {
                    append(local, out_name.size());
                    local.append(out_name);
                }
                const bool has_err = state.asset_subgraph.bind_error.has_value();
                append(local, has_err);
                if (has_err) {
                    append(local, state.asset_subgraph.bind_error->code);
                    append(local, state.asset_subgraph.bind_error->message.size());
                    local.append(state.asset_subgraph.bind_error->message);
                }
            }
        }
        state.local = std::move(local);
        return {};
    }

    // Whether the resources an entry read still resolve as they did.
    bool current(Entry& entry) const {
        if (entry.confirmed_resources == &resources && entry.revision == revision)
            return true;
        for (const auto& dependency : entry.dependencies) {
            auto mesh = resources.mesh(dependency.key);
            if (!mesh || *mesh != dependency.mesh)
                return false;
        }
        entry.confirmed_resources = &resources;
        entry.revision = revision;
        return true;
    }

    const ExternalInput* find_external_input(ObjectId id) const noexcept {
        if (!has_external_inputs)
            return nullptr;
        const auto at = std::ranges::find(external_inputs, id, &ExternalInput::input_node);
        return at != external_inputs.end() ? &*at : nullptr;
    }

    Result<void> execute_asset_node(ObjectId id, State& state, Entry& entry, bool record_timings) {
        if (subgraph_depth >= Evaluator::max_subgraph_depth)
            return error(ErrorCode::invocation_failed, "AssetNode nesting exceeds maximum depth of " +
                                                           std::to_string(Evaluator::max_subgraph_depth) + ".");
        if (state.asset_subgraph.bind_error)
            return std::unexpected(*state.asset_subgraph.bind_error);
        if (!state.asset_subgraph.graph.registered())
            return error(ErrorCode::invocation_failed, "AssetNode has no bound procedural graph asset.");

        std::uint32_t offset = 0;
        if (asset_seed_offset_field != FieldId{}) {
            ATELIER_TRY_BIND(val, runtime.read(id, asset_seed_offset_field));
            offset = std::get<std::uint32_t>(*val);
        }
        const std::uint32_t child_seed = offset == 0u ? seed : hash(seed, offset);

        if (!state.child_evaluator) {
            state.child_evaluator = std::make_shared<Evaluator>(runtime, state.asset_subgraph.graph, resources, program,
                                                                pool, subgraph_depth + 1u);
        }
        std::vector<ExternalInput> child_inputs;
        child_inputs.reserve(state.asset_subgraph.input_nodes.size());
        for (std::size_t p = 0; p < state.asset_subgraph.input_nodes.size(); ++p) {
            ExternalInput ext{};
            ext.input_node = state.asset_subgraph.input_nodes[p];
            for (const auto& connection : state.inputs) {
                if (connection.input == p) {
                    const auto& src_entry = nodes.find(connection.from)->entry;
                    ext.geometry = src_entry->outputs[connection.output];
                    ext.serial = src_entry->serial;
                    ext.output = connection.output;
                    break;
                }
            }
            child_inputs.push_back(std::move(ext));
        }
        state.child_evaluator->set_external_inputs(child_inputs);
        ATELIER_TRY(
            state.child_evaluator->evaluate_targets(state.asset_subgraph.output_nodes, record_timings, child_seed));
        for (const ObjectId out_node : state.asset_subgraph.output_nodes) {
            if (auto err = state.child_evaluator->target_error(out_node))
                return std::unexpected(std::move(*err));
        }
        for (std::size_t out_idx = 0;
             out_idx < state.asset_subgraph.output_nodes.size() && out_idx < entry.outputs.size(); ++out_idx) {
            if (const Geometry* geom =
                    state.child_evaluator->node_output(state.asset_subgraph.output_nodes[out_idx], 0u))
                entry.outputs[out_idx] = *geom;
        }
        entry.subgraph_span = 1u + state.child_evaluator->impl_->max_subgraph_span(state.asset_subgraph.output_nodes);
        entry.dependencies = state.child_evaluator->target_dependencies(state.asset_subgraph.output_nodes);
        return {};
    }

    Result<void> execute_node(ObjectId id, State& state, const SourceKernel* source_kernel, Entry& entry,
                              bool record_timings) {
        if (has_external_inputs && state.type == graph_input_type) {
            const auto* ext = find_external_input(id);
            entry.outputs[0] = ext ? ext->geometry : empty;
            return {};
        }
        if (state.is_asset_node)
            return execute_asset_node(id, state, entry, record_timings);
        const auto& plan = *state.plan;
        Context context;
        context.seed_ = seed;
        context.resources_ = &resources;
        context.dependencies_ = &entry.dependencies;

        const auto upstream_geometry = [&](const Input& connection) -> const Geometry& {
            return nodes.find(connection.from)->entry->outputs[connection.output];
        };
        const auto single_input = [&](std::uint32_t port) -> const Geometry& {
            const Geometry* value = &empty;
            for (const auto& connection : state.inputs)
                if (connection.input == port)
                    value = &upstream_geometry(connection);
            return *value;
        };

        Layer dynamic_layer = Layer::point;
        if (plan.run_over_field != FieldId{}) {
            ATELIER_TRY_BIND(layer_val, runtime.read(id, plan.run_over_field));
            const auto bits = std::get<EnumValue>(*layer_val).bits;
            if (bits > static_cast<std::uint32_t>(Layer::instance))
                return failure("Invalid procedural.Layer value.");
            dynamic_layer = static_cast<Layer>(bits);
        }

        const std::size_t arg_count = plan.arguments.size();
        InlineArray<String, 16> resolved_names;
        InlineArray<Layer, 16> resolved_layers;
        InlineArray<const void*, 16> values;
        InlineArray<void*, 16> writable;
        resolved_names.resize(arg_count);
        resolved_layers.resize(arg_count, Layer::point);
        values.resize(arg_count, nullptr);
        writable.resize(arg_count, nullptr);

        std::size_t multi_port_count = 0;
        std::size_t spatial_port_count = 0;
        bool has_any_columns = false;
        for (const auto& arg : plan.arguments) {
            if (arg.kind == detail::ArgKind::port_inputs)
                ++multi_port_count;
            else if (arg.kind == detail::ArgKind::port_spatial)
                ++spatial_port_count;
            else if (arg.kind == detail::ArgKind::column_in || arg.kind == detail::ArgKind::column_inout ||
                     arg.kind == detail::ArgKind::column_out || arg.kind == detail::ArgKind::column_group)
                has_any_columns = true;
        }
        std::vector<Geometries> multi_inputs;
        multi_inputs.reserve(multi_port_count); // Borrowed below; must not reallocate.
        std::vector<std::shared_ptr<const Spatial>> spatial_inputs;
        spatial_inputs.reserve(spatial_port_count);

        bool wrote_positions = false;
        std::array<std::size_t, 5> expected_layer_counts{};
        if (has_any_columns) {
            if (string_type == TypeId{})
                if (auto str = runtime.native_type<String>())
                    string_type = *str;
            for (std::size_t i = 0; i < arg_count; ++i) {
                const auto& arg = plan.arguments[i];
                if (arg.kind != detail::ArgKind::column_in && arg.kind != detail::ArgKind::column_inout &&
                    arg.kind != detail::ArgKind::column_out && arg.kind != detail::ArgKind::column_group)
                    continue;
                resolved_layers[i] = arg.fixed_layer ? *arg.fixed_layer : dynamic_layer;
                if (arg.name_selector != FieldId{}) {
                    ATELIER_TRY(runtime.read(id, arg.name_selector, ValueOutput{string_type, &resolved_names[i]}));
                } else {
                    resolved_names[i] = arg.static_name;
                }
                if (arg.kind != detail::ArgKind::column_group || !resolved_names[i].empty()) {
                    ATELIER_TRY(Geometry::admit_name(resolved_names[i].view()));
                }
                if (arg.kind == detail::ArgKind::column_inout || arg.kind == detail::ArgKind::column_out) {
                    for (std::size_t j = 0; j < i; ++j) {
                        const auto pk = plan.arguments[j].kind;
                        if ((pk == detail::ArgKind::column_inout || pk == detail::ArgKind::column_out) &&
                            resolved_names[j] == resolved_names[i])
                            return failure("Multiple writable parameters select " +
                                           std::string{resolved_names[i].view().std_view()} + ".");
                    }
                }
            }

            // Resolve all read-only column/group borrows against immutable upstream snapshots first.
            for (std::size_t i = 0; i < arg_count; ++i) {
                const auto& arg = plan.arguments[i];
                if (arg.kind == detail::ArgKind::column_group) {
                    if (resolved_names[i].empty()) {
                        values[i] = &std::get<0>(empty_columns());
                    } else {
                        const Geometry& src = single_input(arg.port_index);
                        ATELIER_TRY_BIND(column, accessors[0].read(src, resolved_names[i], resolved_layers[i], false));
                        values[i] = *column;
                    }
                } else if (arg.kind == detail::ArgKind::column_in) {
                    const Geometry& src = single_input(arg.port_index);
                    ATELIER_TRY_BIND(column, accessors[arg.column_variant].read(src, resolved_names[i],
                                                                                resolved_layers[i], arg.optional));
                    values[i] = *column;
                }
            }

            if (plan.uses_primary_stream) {
                const Geometry& primary_in = single_input(0);
                expected_layer_counts = {primary_in.count(Layer::point), primary_in.count(Layer::vertex),
                                         primary_in.count(Layer::primitive), primary_in.count(Layer::detail),
                                         primary_in.count(Layer::instance)};
                Geometry& primary_out = entry.outputs[0];
                primary_out = primary_in;
                // Create all missing Out columns before taking any writable column borrows.
                for (std::size_t i = 0; i < arg_count; ++i) {
                    const auto& arg = plan.arguments[i];
                    if (arg.kind == detail::ArgKind::column_out) {
                        ATELIER_TRY(
                            accessors[arg.column_variant].out(primary_out, resolved_names[i], resolved_layers[i]));
                    }
                }
                for (std::size_t i = 0; i < arg_count; ++i) {
                    const auto& arg = plan.arguments[i];
                    if (arg.kind == detail::ArgKind::column_inout || arg.kind == detail::ArgKind::column_out) {
                        ATELIER_TRY_BIND(column, accessors[arg.column_variant].inout(primary_out, resolved_names[i],
                                                                                     resolved_layers[i]));
                        values[i] = writable[i] = *column;
                        wrote_positions |= resolved_names[i] == "P";
                    }
                }
            }
        }

        for (std::size_t i = 0; i < arg_count; ++i) {
            const auto& arg = plan.arguments[i];
            switch (arg.kind) {
            case detail::ArgKind::context:
                values[i] = &context;
                break;
            case detail::ArgKind::port_input:
                values[i] = &single_input(arg.port_index);
                break;
            case detail::ArgKind::port_inputs: {
                auto& list = multi_inputs.emplace_back();
                for (const auto& connection : state.inputs)
                    if (connection.input == arg.port_index)
                        list.add(upstream_geometry(connection));
                values[i] = &list;
                break;
            }
            case detail::ArgKind::port_spatial: {
                ATELIER_TRY_BIND(built_spatial, Spatial::acquire(single_input(arg.port_index)));
                spatial_inputs.push_back(std::move(*built_spatial));
                values[i] = spatial_inputs.back().get();
                break;
            }
            case detail::ArgKind::port_output:
                values[i] = writable[i] = &entry.outputs[arg.port_index];
                break;
            default:
                break;
            }
        }

        const Node* active_node{};
        if (!plan.native) {
            ATELIER_TRY(runtime.with_native<const Node>(id, [&](const Node& ref) -> Result<void> {
                active_node = &ref;
                return {};
            }));
        }

        InlineArray<Argument, 16> arguments;
        arguments.reserve(arg_count);
        for (std::size_t i = 0; i < arg_count; ++i)
            arguments.add(Argument::borrow(plan.arguments[i].type, values[i], writable[i]));
        {
            detail::NativeCook collect{state.status.boolean_stats,
                                       pool->impl_->boolean_preparation,
                                       pool->impl_->budget,
                                       active_node,
                                       entry.outputs,
                                       plan.uses_primary_stream};
            if (plan.native) {
                if (pool->impl_->budget && state.type == boolean_type) {
                    for (const auto& input : state.inputs) {
                        const auto& source = nodes.find(input.from)->entry;
                        collect.inputs[input.input] = {source, &source->outputs[input.output]};
                    }
                }
                ATELIER_TRY(runtime.call(id, plan.method, arguments));
            } else {
                ATELIER_TRY(source_kernel->entry.invoke(id, arguments));
            }
        }
        if (plan.uses_primary_stream) {
            for (std::size_t i = 0; i < arg_count; ++i) {
                const auto& arg = plan.arguments[i];
                if (arg.kind != detail::ArgKind::column_inout && arg.kind != detail::ArgKind::column_out)
                    continue;
                if (accessors[arg.column_variant].size(writable[i]) !=
                    expected_layer_counts[static_cast<std::size_t>(resolved_layers[i])])
                    return error(ErrorCode::invocation_failed,
                                 "Column '" + std::string{resolved_names[i].view().std_view()} + "' changed length.");
            }
        }
        if (wrote_positions) {
            for (const auto& position : entry.outputs[0].positions())
                if (!math::is_finite(position))
                    return error(ErrorCode::invocation_failed, "Point positions must be finite.");
        }
        return {};
    }

    Result<std::shared_ptr<Entry>> run(ObjectId id, State& state, const SourceKernel* source_kernel, std::string key,
                                       bool record_timings) {
        auto entry = std::make_shared<Entry>();
        entry->key = std::move(key);
        entry->outputs.resize(state.plan->ports.outputs.size());
        ++state.status.evaluations;
        state.status.time.reset();
        std::chrono::steady_clock::time_point start{};
        if (record_timings)
            start = std::chrono::steady_clock::now();
        Result<void> ran{};
        if (state.bypassed) {
            if (!entry->outputs.empty()) {
                for (const auto& connection : state.inputs) {
                    if (connection.input == 0) {
                        entry->outputs[0] = nodes.find(connection.from)->entry->outputs[connection.output];
                        break;
                    }
                }
            }
        } else {
            ran = execute_node(id, state, source_kernel, *entry, record_timings);
        }
        if (record_timings)
            state.status.time = std::chrono::steady_clock::now() - start;
        if (!ran) {
            pool->impl_->trim();
            return std::unexpected(ran.error());
        }
        entry->serial = ++pool->impl_->serials;
        entry->confirmed_resources = &resources;
        entry->revision = revision;
        // The cache map holds a second copy of the key.
        entry->bytes = sizeof(Entry) + 2 * entry->key.capacity() + entry->dependencies.capacity() * sizeof(Reference);
        for (const auto& geometry : entry->outputs)
            entry->bytes += geometry.bytes();
        pool->impl_->store(entry);
        return entry;
    }

    std::string node_label(TypeId type) const {
        if (auto info = runtime.describe(type))
            return (*info)->name;
        return "procedural.Node";
    }

    void update(ObjectId id, State& state, bool record_timings) {
        if (state.fault) {
            state.status.error = state.fault;
            state.root_error = Error{state.fault->code, node_label(state.type) + ": " + state.fault->message};
            return;
        }
        if (!state.bypassed && state.frozen && state.frozen_entry) {
            if (state.entry != state.frozen_entry)
                ++state.status.cache_hits;
            state.entry = state.frozen_entry;
            pool->impl_->touch(*state.entry);
            state.status.error.reset();
            state.root_error.reset();
            return;
        }
        std::string key = state.local;
        const SourceKernel* source_kernel{};
        if (!state.plan->native && !state.bypassed) {
            source_kernel = &**source_kernels.find(state.type); // read() already validated the current kernel.
            append(key, source_kernel->identity);
        }
        if (state.is_asset_node && !state.bypassed) {
            append(key, program);
            append(key, program ? program->revision() : std::uint64_t{0});
        }
        if (state.type == graph_input_type && !state.bypassed) {
            append(key, has_external_inputs);
            if (has_external_inputs) {
                const auto* ext = find_external_input(id);
                append(key, ext ? ext->serial : std::uint64_t{0});
                append(key, ext ? ext->output : std::uint32_t{0});
            }
        }
        if (state.plan->uses_context && !state.bypassed)
            append(key, seed);
        for (const auto& connection : state.inputs) {
            if (state.bypassed && connection.input != 0)
                continue;
            const auto& upstream = *nodes.find(connection.from);
            if (upstream.status.error || !upstream.entry) {
                const std::string port_name = connection.input < state.plan->ports.inputs.size()
                                                  ? state.plan->ports.inputs[connection.input]
                                                  : std::to_string(connection.input);
                const std::string_view cause = upstream.root_error ? std::string_view{upstream.root_error->message}
                                               : upstream.status.error
                                                   ? std::string_view{upstream.status.error->message}
                                                   : "upstream failed";
                state.root_error = upstream.root_error ? upstream.root_error : upstream.status.error;
                state.status.error = Error{ErrorCode::invocation_failed,
                                           "Input '" + port_name + "' failed (" + std::string{cause} + ")."};
                return;
            }
            append(key, connection.input);
            append(key, connection.output);
            append(key, upstream.entry->serial);
            if (state.bypassed)
                break;
        }
        const auto within_depth = [&](const Entry& candidate_entry) noexcept {
            return state.bypassed || subgraph_depth + candidate_entry.subgraph_span <= Evaluator::max_subgraph_depth;
        };
        if (state.entry && state.entry->key == key && within_depth(*state.entry) && current(*state.entry)) {
            pool->impl_->touch(*state.entry);
            if (!state.bypassed && state.frozen && !state.frozen_entry)
                state.frozen_entry = state.entry;
            state.status.error.reset();
            state.root_error.reset();
            return;
        }
        if (key == state.failed && revision == state.failed_revision) {
            state.status.error = state.failure;
            state.root_error = Error{state.failure.code, node_label(state.type) + ": " + state.failure.message};
            return;
        }
        if (auto* found = pool->impl_->cache.find(key); found &&
                                                        (**found).outputs.size() == state.plan->ports.outputs.size() &&
                                                        within_depth(**found) && current(**found)) {
            state.entry = *found;
            pool->impl_->touch(*state.entry);
            ++state.status.cache_hits;
            if (!state.bypassed && state.frozen && !state.frozen_entry)
                state.frozen_entry = state.entry;
            state.status.error.reset();
            state.root_error.reset();
            return;
        }
        auto evaluated = run(id, state, source_kernel, key, record_timings);
        if (!evaluated) {
            state.entry.reset();
            state.failed = std::move(key);
            state.failed_revision = revision;
            state.failure = evaluated.error();
            state.status.error = evaluated.error();
            state.root_error = Error{state.failure.code, node_label(state.type) + ": " + state.failure.message};
            return;
        }
        state.entry = std::move(*evaluated);
        if (!state.bypassed && state.frozen && !state.frozen_entry)
            state.frozen_entry = state.entry;
        state.failed.clear();
        state.status.error.reset();
        state.root_error.reset();
    }

    // Upstream first, ending at each root in `roots`. When `active_only` is true, bypassed nodes
    // only follow their pass-through port 0 instead of cooking unused secondary branches.
    Result<std::vector<ObjectId>> order(std::span<const ObjectId> roots, const Map<ObjectId, State>& changed,
                                        bool active_only = false) const {
        std::vector<ObjectId> result;
        struct Frame {
            ObjectId id;

            std::size_t next;
        };
        Map<ObjectId, bool> finished; // false while on the path.
        std::vector<Frame> path;
        for (const ObjectId root : roots) {
            if (root == ObjectId{})
                continue;
            if (!finished.try_emplace(root, false).inserted)
                continue;
            path.push_back({root, 0});
            while (!path.empty()) {
                const auto id = path.back().id;
                const auto* staged = changed.find(id);
                const auto* state_ptr = staged ? staged : nodes.find(id);
                if (!state_ptr)
                    return error(ErrorCode::invalid_member, "Evaluation target does not belong to this graph.");
                const auto& state = *state_ptr;
                if (!state.bypassed && state.frozen && state.frozen_entry && active_only)
                    path.back().next = state.inputs.size();
                if (state.bypassed && active_only) {
                    while (path.back().next < state.inputs.size() && state.inputs[path.back().next].input != 0)
                        ++path.back().next;
                }
                if (path.back().next < state.inputs.size()) {
                    const auto idx = path.back().next++;
                    if (state.bypassed && active_only)
                        path.back().next = state.inputs.size();
                    const auto from = state.inputs[idx].from;
                    const auto mark = finished.try_emplace(from, false);
                    if (mark.inserted)
                        path.push_back({from, 0});
                    else if (!*mark.value)
                        return error(ErrorCode::argument_mismatch, "The graph has a cycle.");
                    continue;
                }
                finished.insert_or_assign(id, true);
                result.push_back(id);
                path.pop_back();
            }
        }
        return result;
    }

    Result<std::vector<ObjectId>> order(ObjectId display, const Map<ObjectId, State>& changed,
                                        bool active_only = false) const {
        return display == ObjectId{} ? Result<std::vector<ObjectId>>{std::vector<ObjectId>{}}
                                     : order(std::span<const ObjectId>{&display, 1}, changed, active_only);
    }

    std::uint32_t max_subgraph_span(std::span<const ObjectId> roots) const {
        std::uint32_t span = 0;
        if (roots.empty())
            return span;
        auto sequence = order(roots, {}, true);
        if (!sequence)
            return span;
        for (const auto node : *sequence) {
            const auto* state = nodes.find(node);
            if (!state || !state->entry || state->bypassed)
                continue;
            span = std::max(span, state->entry->subgraph_span);
        }
        return span;
    }

    std::vector<Reference> dependencies_for(std::span<const ObjectId> roots) const {
        std::vector<Reference> result;
        if (roots.empty())
            return result;
        auto sequence = order(roots, {}, true);
        if (!sequence)
            return result;
        for (const auto node : *sequence) {
            const auto* state = nodes.find(node);
            if (!state || !state->entry || state->bypassed)
                continue;
            for (const auto& dep : state->entry->dependencies) {
                if (std::ranges::find(result, dep) == result.end())
                    result.push_back(dep);
            }
        }
        return result;
    }

    std::vector<Reference> dependencies(ObjectId node_id) const {
        return node_id == ObjectId{} ? std::vector<Reference>{}
                                     : dependencies_for(std::span<const ObjectId>{&node_id, 1});
    }

    void sync_published(bool update_last_valid) {
        const auto display = members.display();
        if (display == ObjectId{}) {
            current_published.reset();
            if (update_last_valid)
                last_valid.reset();
        } else if (const auto* shown = nodes.find(display); shown && !shown->status.error && shown->entry) {
            current_published = shown->entry;
            if (update_last_valid)
                last_valid = shown->entry;
        } else {
            current_published.reset();
        }
    }

    Result<void> evaluate(std::optional<std::span<const ObjectId>> explicit_targets, bool record_timings,
                          std::optional<std::uint32_t> seed_override) {
        auto now = runtime.change_cursor();
        if (!now)
            return std::unexpected(now.error());
        const auto id = graph.id();
        const bool ext_changed = external_inputs_epoch != synced_external_epoch;
        bool dirty = !cursor || resources.revision() != revision || ext_changed;
        // A publication may change any Source node's code; rebinding finds which did.
        const bool republished = program && program->revision() != code_revision;
        dirty = dirty || republished;
        std::optional<Contents> refreshed;
        auto next_authored_seed = authored_seed;
        if (!cursor || runtime.changed_since(id, *cursor)) {
            auto listed = contents(runtime, id);
            if (!listed)
                return std::unexpected(listed.error());
            if (graph_seed_field == FieldId{}) {
                auto type = runtime.native_type<Graph>();
                if (!type)
                    return std::unexpected(type.error());
                auto field = runtime.find_field(*type, "seed");
                if (!field)
                    return std::unexpected(field.error());
                graph_seed_field = *field;
            }
            auto value = runtime.read(id, graph_seed_field);
            if (!value)
                return std::unexpected(value.error());
            next_authored_seed = std::get<std::uint32_t>(*value);
            refreshed = std::move(*listed);
            dirty = true;
        }
        const std::uint32_t next_seed = seed_override.value_or(next_authored_seed);
        const bool seed_changed = !cursor || next_seed != seed;
        dirty = dirty || seed_changed;
        const bool update_last_valid = !seed_override.has_value();
        const auto& next_members = refreshed ? *refreshed : members;
        const ObjectId display_target = next_members.display();
        const std::span<const ObjectId> target_roots =
            explicit_targets ? *explicit_targets
                             : (display_target != ObjectId{} ? std::span<const ObjectId>{&display_target, 1}
                                                             : std::span<const ObjectId>{});
        if (explicit_targets) {
            for (const ObjectId root_node : *explicit_targets)
                if (root_node != ObjectId{} && !next_members.contains(root_node))
                    return error(ErrorCode::argument_mismatch, "The node is not in the graph.");
        }
        // Stage only reread nodes; untouched states and their local strings stay in place.
        Map<ObjectId, State> candidate;
        const bool membership_changed = refreshed && !std::ranges::equal(members.nodes(), refreshed->nodes());
        for (const auto node : next_members.nodes()) {
            const auto* prior = nodes.find(node);
            const bool added = prior == nullptr;
            const bool reread = added || !cursor || runtime.changed_since(node, *cursor);
            dirty = dirty || reread;
            const bool stale_code = republished && !added && prior->plan && !prior->plan->native;
            if (!reread && membership_changed)
                ATELIER_TRY(next_members.check(prior->inputs));
            if (!reread && !stale_code)
                continue;
            auto& state = *candidate.try_emplace(node).value;
            if (!reread) {
                state.type = prior->type;
                state.plan = prior->plan;
                state.inputs = prior->inputs;
            }
            if (reread) {
                state.plan = nullptr;
                state.instance_plan.reset();
                state.is_asset_node = false;
                state.asset_subgraph = {};
                state.fault.reset();
                auto type = runtime.type_of(node);
                if (!type) {
                    state.fault = type.error();
                    if (!added) {
                        state.inputs = prior->inputs;
                        if (membership_changed)
                            ATELIER_TRY(next_members.check(state.inputs));
                    }
                    continue;
                }
                state.type = *type;
                if (*type == asset_node_type) {
                    state.is_asset_node = true;
                    auto sub = asset_subgraph(runtime, node);
                    if (!sub) {
                        state.fault = sub.error();
                    } else {
                        state.asset_subgraph = std::move(*sub);
                        auto dyn = std::make_unique<detail::NodePlan>();
                        dyn->native = true;
                        dyn->uses_context = true;
                        dyn->ports = state.asset_subgraph.ports;
                        state.instance_plan = std::move(dyn);
                        state.plan = state.instance_plan.get();
                    }
                } else {
                    auto* known = plans.find(*type);
                    if (!known)
                        known = plans
                                    .try_emplace(*type, std::make_unique<const Result<detail::NodePlan>>(
                                                            detail::inspect_node(runtime, *type)))
                                    .value;
                    if (**known)
                        state.plan = &***known;
                    else
                        state.fault = (**known).error();
                }
                ATELIER_TRY_BIND(into, inputs(runtime, next_members, node));
                state.inputs = std::move(*into);
            }
        }
        const auto cook_sequence = [&](std::span<const ObjectId> sequence) {
            for (const auto node : sequence) {
                auto& state = *nodes.find(node);
                if (state.synced_generation != sync_generation) {
                    update(node, state, record_timings);
                    state.synced_generation = sync_generation;
                }
            }
        };
        if (!dirty && candidate.empty()) {
            cursor = *now;
            if (!explicit_targets && display_current) {
                sync_published(update_last_valid);
                return {};
            }
            if (!target_roots.empty()) {
                const bool any_unsynced = std::ranges::any_of(target_roots, [&](ObjectId root_node) {
                    if (root_node == ObjectId{})
                        return false;
                    const auto* root_state = nodes.find(root_node);
                    return !root_state || root_state->synced_generation != sync_generation;
                });
                if (any_unsynced) {
                    ATELIER_TRY(order(target_roots, {}, false));
                    ATELIER_TRY_BIND(sequence, order(target_roots, {}, true));
                    cook_sequence(*sequence);
                }
            }
            if (!explicit_targets) {
                display_current = true;
                sync_published(update_last_valid);
            }
            return {};
        }
        const auto current = [&](ObjectId node) -> const State& {
            const auto* found = candidate.find(node);
            return found ? *found : *nodes.find(node);
        };
        for (const auto node : next_members.nodes())
            for (const auto& connection : current(node).inputs) {
                const auto& source = current(connection.from);
                if (source.plan && connection.output >= source.plan->ports.outputs.size())
                    return error(ErrorCode::argument_mismatch, "Graph input names a missing output port.");
            }
        ATELIER_TRY(order(target_roots, candidate, false));
        if (republished)
            source_kernels.clear();
        bool changed =
            !cursor || refreshed || republished || resources.revision() != revision || seed_changed || ext_changed;
        for (auto [node, state] : candidate) {
            const auto* prior = nodes.find(node);
            auto read = this->read(node, state, prior);
            state.fault.reset();
            if (!read)
                state.fault = read.error();
            changed = changed || !prior || !read || state.bypassed != prior->bypassed ||
                      state.frozen != prior->frozen || state.frozen_entry != prior->frozen_entry ||
                      state.local != prior->local || state.inputs != prior->inputs;
        }
        for (auto [node, state] : candidate) {
            auto& target = *nodes.try_emplace(node).value;
            target.type = state.type;
            target.plan = state.plan;
            target.instance_plan = std::move(state.instance_plan);
            target.is_asset_node = state.is_asset_node;
            target.asset_subgraph = std::move(state.asset_subgraph);
            target.child_evaluator = std::move(state.child_evaluator);
            target.bypassed = state.bypassed;
            target.frozen = state.frozen;
            target.frozen_source = std::move(state.frozen_source);
            target.frozen_entry = std::move(state.frozen_entry);
            target.inputs = std::move(state.inputs);
            target.fault = std::move(state.fault);
            if (!target.fault)
                target.local = std::move(state.local);
        }
        if (membership_changed) {
            std::vector<ObjectId> removed;
            for (const auto& [node, _] : nodes)
                if (!next_members.contains(node))
                    removed.push_back(node);
            for (const auto node : removed)
                (void)nodes.remove(node);
        }
        if (refreshed)
            members = std::move(*refreshed);
        authored_seed = next_authored_seed;
        seed = next_seed;
        cursor = *now;
        synced_external_epoch = external_inputs_epoch;
        if (republished)
            code_revision = program->revision();
        if (changed) {
            ++sync_generation;
            revision = resources.revision();
            display_current = false;
        }
        if (!target_roots.empty()) {
            const bool any_unsynced = std::ranges::any_of(target_roots, [&](ObjectId root_node) {
                if (root_node == ObjectId{})
                    return false;
                const auto* root_state = nodes.find(root_node);
                return root_state && root_state->synced_generation != sync_generation;
            });
            if (any_unsynced) {
                ATELIER_TRY_BIND(active_sequence, order(target_roots, {}, true));
                cook_sequence(*active_sequence);
            }
        }
        if (!explicit_targets) {
            display_current = true;
            sync_published(update_last_valid);
        }
        return {};
    }
};

Evaluator::Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, const source::Program* program,
                     std::size_t cache_bytes)
    : Evaluator(runtime, std::move(graph), resources, program, std::make_shared<CachePool>(cache_bytes), 0) {}

Evaluator::Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, const source::Program* program,
                     std::shared_ptr<CachePool> cache_pool, std::uint32_t subgraph_depth)
    : impl_(std::make_unique<Impl>(runtime, std::move(graph), resources, program, std::move(cache_pool),
                                   subgraph_depth)) {}

Evaluator::~Evaluator() = default;

std::shared_ptr<CachePool> Evaluator::cache_pool() const noexcept {
    return impl_->pool;
}

void Evaluator::set_external_inputs(std::span<const ExternalInput> inputs) {
    bool same = impl_->has_external_inputs && impl_->external_inputs.size() == inputs.size();
    if (same) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto& old = impl_->external_inputs[i];
            const auto& in = inputs[i];
            const bool equal_geom = (in.serial != 0u && old.serial == in.serial) ||
                                    (in.serial == 0u && old.geometry.same_value(in.geometry));
            if (old.input_node != in.input_node || old.output != in.output || !equal_geom) {
                same = false;
                break;
            }
        }
    }
    if (same)
        return;
    impl_->has_external_inputs = true;
    impl_->external_inputs.assign(inputs.begin(), inputs.end());
    for (auto& ext : impl_->external_inputs) {
        if (ext.serial == 0u && !ext.geometry.same_value(impl_->empty))
            ext.serial = ++impl_->pool->impl_->serials;
    }
    ++impl_->external_inputs_epoch;
}

void Evaluator::clear_external_inputs() noexcept {
    if (!impl_->has_external_inputs && impl_->external_inputs.empty())
        return;
    impl_->has_external_inputs = false;
    impl_->external_inputs.clear();
    ++impl_->external_inputs_epoch;
}

Result<void> Evaluator::evaluate(bool record_timings, std::optional<std::uint32_t> seed_override) {
    auto result = impl_->evaluate(std::nullopt, record_timings, seed_override);
    if (!result)
        impl_->current_published.reset();
    return result;
}

Result<void> Evaluator::evaluate_node(ObjectId node, bool record_timings, std::optional<std::uint32_t> seed_override) {
    const std::span<const ObjectId> single =
        node != ObjectId{} ? std::span<const ObjectId>{&node, 1} : std::span<const ObjectId>{};
    return impl_->evaluate(single, record_timings, seed_override);
}

Result<void> Evaluator::evaluate_targets(std::span<const ObjectId> targets, bool record_timings,
                                         std::optional<std::uint32_t> seed_override) {
    return impl_->evaluate(targets, record_timings, seed_override);
}

const Geometry* Evaluator::output() const noexcept {
    return impl_->current_published && !impl_->current_published->outputs.empty()
               ? &impl_->current_published->outputs.front()
               : nullptr;
}

std::shared_ptr<const Geometry> Evaluator::retain_output() const noexcept {
    const Geometry* ptr = output();
    return ptr ? std::shared_ptr<const Geometry>{impl_->current_published, ptr} : std::shared_ptr<const Geometry>{};
}

const Geometry* Evaluator::last_valid_output() const noexcept {
    return impl_->last_valid && !impl_->last_valid->outputs.empty() ? &impl_->last_valid->outputs.front() : nullptr;
}

std::shared_ptr<const Geometry> Evaluator::retain_last_valid_output() const noexcept {
    const Geometry* ptr = last_valid_output();
    return ptr ? std::shared_ptr<const Geometry>{impl_->last_valid, ptr} : std::shared_ptr<const Geometry>{};
}

bool Evaluator::has_display_node() const noexcept {
    return impl_->members.display() != ObjectId{};
}

std::optional<Error> Evaluator::display_error() const {
    return target_error(impl_->members.display());
}

std::vector<Reference> Evaluator::display_dependencies() const {
    return impl_->dependencies(impl_->members.display());
}

std::uint32_t Evaluator::node_output_count(ObjectId node) const noexcept {
    const auto* found = impl_->nodes.find(node);
    return found && !found->status.error && found->entry ? static_cast<std::uint32_t>(found->entry->outputs.size())
                                                         : 0u;
}

const Geometry* Evaluator::node_output(ObjectId node, std::uint32_t output_index) const noexcept {
    const auto* found = impl_->nodes.find(node);
    return found && !found->status.error && found->entry && output_index < found->entry->outputs.size()
               ? &found->entry->outputs[output_index]
               : nullptr;
}

std::shared_ptr<const Geometry> Evaluator::retain_node_output(ObjectId node,
                                                              std::uint32_t output_index) const noexcept {
    const auto* found = impl_->nodes.find(node);
    if (!found || found->status.error || !found->entry || output_index >= found->entry->outputs.size())
        return {};
    return {found->entry, &found->entry->outputs[output_index]};
}

std::optional<Error> Evaluator::target_error(ObjectId node) const {
    if (node == ObjectId{})
        return std::nullopt;
    const auto* state = impl_->nodes.find(node);
    if (!state || !state->status.error)
        return std::nullopt;
    return state->root_error ? state->root_error : state->status.error;
}

std::vector<Reference> Evaluator::target_dependencies(std::span<const ObjectId> targets) const {
    return impl_->dependencies_for(targets);
}

Result<std::shared_ptr<const FrozenOutput>> Evaluator::freeze_snapshot(ObjectId node,
                                                                       std::optional<std::uint32_t> seed_override) {
    ATELIER_TRY(evaluate_node(node, false, seed_override));
    const auto* state = impl_->nodes.find(node);
    if (state && state->status.error)
        return std::unexpected(*state->status.error);
    if (!state || !state->entry || state->entry->outputs.empty())
        return failure("Cannot freeze a node without a valid cooked output.");
    auto snapshot = std::make_shared<FrozenOutput>();
    snapshot->outputs = state->entry->outputs;
    snapshot->dependencies = impl_->dependencies(node);
    return snapshot;
}

std::vector<ObjectId> Evaluator::display_freezes() const {
    std::vector<ObjectId> result;
    if (const auto sequence = impl_->order(impl_->members.display(), {}, true))
        for (const auto node : *sequence) {
            const auto& state = *impl_->nodes.find(node);
            if (state.frozen && !state.bypassed && state.frozen_entry)
                result.push_back(node);
        }
    return result;
}

const NodeStatus* Evaluator::status(ObjectId node) const {
    const auto* found = impl_->nodes.find(node);
    return found ? &found->status : nullptr;
}

}
