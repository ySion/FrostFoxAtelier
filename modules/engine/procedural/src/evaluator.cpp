#include "procedural/evaluator.hpp"
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

    std::vector<Geometry> outputs;

    std::vector<Reference> dependencies; // Resources read, with what they resolved to.

    std::uint64_t revision{}; // Resources revision the dependencies were confirmed at.

    std::size_t bytes{};

    bool cached{};

    std::list<Entry*>::iterator recent; // Valid while cached.
};

}

struct Evaluator::Impl {
    struct State {
        TypeId type{};

        const detail::NodePlan* plan{};

        bool bypassed{};

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

    std::size_t budget;

    const Geometry empty;

    TypeId string_type{};

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

    Map<std::string, std::shared_ptr<Entry>> cache;

    std::list<Entry*> recent; // Most recently used first.

    std::size_t cached_bytes{};

    std::uint64_t serials{};

    std::uint64_t sync_generation{1};

    bool display_current{};

    std::shared_ptr<Entry> current_published;

    std::shared_ptr<Entry> last_valid;

    Impl(Runtime& owner, ObjectRoot root, const Resources& host, const source::Program* code, std::size_t bytes)
        : runtime(owner), graph(std::move(root)), resources(host), program(code), budget(bytes),
          code_revision(code ? code->revision() : 0) {
        if (auto str = runtime.native_type<String>())
            string_type = *str;
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

    Result<void> read(ObjectId id, State& state) {
        if (!state.plan)
            return std::unexpected(*state.fault);
        ATELIER_TRY_BIND(is_bypassed, bypassed(runtime, id));
        state.bypassed = *is_bypassed;
        auto info = runtime.describe(state.type);
        if (!info)
            return std::unexpected(info.error());
        if (!state.plan->native && !state.bypassed) {
            auto* bound = source_kernels.find(state.type);
            if (!bound)
                bound = source_kernels.try_emplace(state.type, bind_source(state.type)).value;
            if (!*bound)
                return std::unexpected(bound->error());
        }
        std::string local;
        append(local, *info); // Published TypeInfo addresses are stable identities for this Runtime.
        append(local, state.bypassed);
        if (!state.bypassed) {
            for (const auto& field : (*info)->fields)
                if (auto added = append_field(local, id, field); !added)
                    return added;
        }
        state.local = std::move(local);
        return {};
    }

    // Whether the resources an entry read still resolve as they did.
    bool current(Entry& entry) const {
        if (entry.revision == revision)
            return true;
        for (const auto& dependency : entry.dependencies) {
            auto mesh = resources.mesh(dependency.key);
            if (!mesh || *mesh != dependency.mesh)
                return false;
        }
        entry.revision = revision;
        return true;
    }

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

    void store(const std::shared_ptr<Entry>& entry) {
        if (auto* old = cache.find(entry->key))
            uncache(**old);
        cache.insert_or_assign(entry->key, entry);
        recent.push_front(entry.get());
        entry->recent = recent.begin();
        entry->cached = true;
        cached_bytes += entry->bytes;
        while (cached_bytes > budget)
            uncache(*recent.back());
    }

    Result<void> execute_node(ObjectId id, const State& state, const SourceKernel* source_kernel, Entry& entry) {
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
        for (const auto& arg : plan.arguments)
            if (arg.kind == detail::ArgKind::port_inputs)
                ++multi_port_count;
        std::vector<Geometries> multi_inputs;
        multi_inputs.reserve(multi_port_count); // Borrowed below; must not reallocate.

        bool wrote_positions = false;
        std::array<std::size_t, 5> expected_layer_counts{};
        if (plan.uses_primary_stream) {
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

            const Geometry& primary_in = single_input(0);
            expected_layer_counts = {primary_in.count(Layer::point), primary_in.count(Layer::vertex),
                                     primary_in.count(Layer::primitive), primary_in.count(Layer::detail),
                                     primary_in.count(Layer::instance)};
            // Resolve all read-only column/group borrows against the immutable input snapshot first.
            for (std::size_t i = 0; i < arg_count; ++i) {
                const auto& arg = plan.arguments[i];
                if (arg.kind == detail::ArgKind::column_group) {
                    if (resolved_names[i].empty()) {
                        values[i] = &std::get<0>(empty_columns());
                    } else {
                        ATELIER_TRY_BIND(column,
                                         accessors[0].read(primary_in, resolved_names[i], resolved_layers[i], false));
                        values[i] = *column;
                    }
                } else if (arg.kind == detail::ArgKind::column_in) {
                    ATELIER_TRY_BIND(column, accessors[arg.column_variant].read(primary_in, resolved_names[i],
                                                                                resolved_layers[i], arg.optional));
                    values[i] = *column;
                }
            }

            Geometry& primary_out = entry.outputs[0];
            primary_out = primary_in;
            // Create all missing Out columns before taking any writable column borrows.
            for (std::size_t i = 0; i < arg_count; ++i) {
                const auto& arg = plan.arguments[i];
                if (arg.kind == detail::ArgKind::column_out) {
                    ATELIER_TRY(accessors[arg.column_variant].out(primary_out, resolved_names[i], resolved_layers[i]));
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
            case detail::ArgKind::port_output:
                values[i] = writable[i] = &entry.outputs[arg.port_index];
                break;
            default:
                break;
            }
        }

        InlineArray<Argument, 16> arguments;
        arguments.reserve(arg_count);
        for (std::size_t i = 0; i < arg_count; ++i)
            arguments.add(Argument::borrow(plan.arguments[i].type, values[i], writable[i]));
        if (plan.native) {
            ATELIER_TRY(runtime.call(id, plan.method, arguments));
        } else {
            ATELIER_TRY(source_kernel->entry.invoke(id, arguments));
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
            for (const auto& connection : state.inputs) {
                if (connection.input == 0) {
                    entry->outputs[0] = nodes.find(connection.from)->entry->outputs[connection.output];
                    break;
                }
            }
        } else {
            ran = execute_node(id, state, source_kernel, *entry);
        }
        if (record_timings)
            state.status.time = std::chrono::steady_clock::now() - start;
        if (!ran)
            return std::unexpected(ran.error());
        entry->serial = ++serials;
        entry->revision = revision;
        // The cache map holds a second copy of the key.
        entry->bytes = sizeof(Entry) + 2 * entry->key.capacity() + entry->dependencies.capacity() * sizeof(Reference);
        for (const auto& geometry : entry->outputs)
            entry->bytes += geometry.bytes();
        store(entry);
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
        std::string key = state.local;
        const SourceKernel* source_kernel{};
        if (!state.plan->native && !state.bypassed) {
            source_kernel = &**source_kernels.find(state.type); // read() already validated the current kernel.
            append(key, source_kernel->identity);
        }
        if (state.plan->uses_context && !state.bypassed)
            append(key, seed);
        for (const auto& connection : state.inputs) {
            if (state.bypassed && connection.input != 0)
                continue;
            const auto& upstream = *nodes.find(connection.from);
            if (upstream.status.error || !upstream.entry) {
                const auto& port_name = state.plan->ports.inputs[connection.input];
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
        if (state.entry && state.entry->key == key && current(*state.entry)) {
            touch(*state.entry);
            state.status.error.reset();
            state.root_error.reset();
            return;
        }
        if (key == state.failed && revision == state.failed_revision) {
            state.status.error = state.failure;
            state.root_error = Error{state.failure.code, node_label(state.type) + ": " + state.failure.message};
            return;
        }
        if (auto* found = cache.find(key); found && current(**found)) {
            state.entry = *found;
            touch(*state.entry);
            ++state.status.cache_hits;
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
        state.failed.clear();
        state.status.error.reset();
        state.root_error.reset();
    }

    // Upstream first, ending at the display node. When `active_only` is true, bypassed nodes
    // only follow their pass-through port 0 instead of cooking unused secondary branches.
    Result<std::vector<ObjectId>> order(ObjectId display, const Map<ObjectId, State>& changed,
                                        bool active_only = false) const {
        std::vector<ObjectId> result;
        if (display == ObjectId{})
            return result;
        struct Frame {
            ObjectId id;

            std::size_t next;
        };
        Map<ObjectId, bool> finished; // false while on the path.
        std::vector<Frame> path{{display, 0}};
        finished.insert_or_assign(display, false);
        while (!path.empty()) {
            const auto id = path.back().id;
            const auto* staged = changed.find(id);
            const auto& state = staged ? *staged : *nodes.find(id);
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
        return result;
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

    Result<void> evaluate(std::optional<ObjectId> explicit_node, bool record_timings,
                          std::optional<std::uint32_t> seed_override) {
        auto now = runtime.change_cursor();
        if (!now)
            return std::unexpected(now.error());
        const auto id = graph.id();
        bool dirty = !cursor || resources.revision() != revision;
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
        if (explicit_node && *explicit_node != ObjectId{} && !next_members.contains(*explicit_node))
            return error(ErrorCode::argument_mismatch, "The node is not in the graph.");
        const auto target_root = explicit_node ? *explicit_node : next_members.display();
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
            if (!explicit_node && display_current) {
                sync_published(update_last_valid);
                return {};
            }
            if (target_root != ObjectId{}) {
                if (const auto* root_state = nodes.find(target_root);
                    !root_state || root_state->synced_generation != sync_generation) {
                    ATELIER_TRY(order(target_root, {}, false));
                    ATELIER_TRY_BIND(sequence, order(target_root, {}, true));
                    cook_sequence(*sequence);
                }
            }
            if (!explicit_node) {
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
        ATELIER_TRY(order(target_root, candidate, false));
        if (republished)
            source_kernels.clear();
        bool changed = !cursor || refreshed || republished || resources.revision() != revision || seed_changed;
        for (auto [node, state] : candidate) {
            const auto* prior = nodes.find(node);
            auto read = this->read(node, state);
            state.fault.reset();
            if (!read)
                state.fault = read.error();
            changed = changed || !prior || !read || state.bypassed != prior->bypassed || state.local != prior->local ||
                      state.inputs != prior->inputs;
        }
        for (auto [node, state] : candidate) {
            auto& target = *nodes.try_emplace(node).value;
            target.type = state.type;
            target.plan = state.plan;
            target.bypassed = state.bypassed;
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
        if (republished)
            code_revision = program->revision();
        if (changed) {
            ++sync_generation;
            revision = resources.revision();
            display_current = false;
        }
        if (target_root != ObjectId{}) {
            if (const auto* root_state = nodes.find(target_root);
                root_state && root_state->synced_generation != sync_generation) {
                ATELIER_TRY_BIND(active_sequence, order(target_root, {}, true));
                cook_sequence(*active_sequence);
            }
        }
        if (!explicit_node) {
            display_current = true;
            sync_published(update_last_valid);
        }
        return {};
    }
};

Evaluator::Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, const source::Program* program,
                     std::size_t cache_bytes)
    : impl_(std::make_unique<Impl>(runtime, std::move(graph), resources, program, cache_bytes)) {}

Evaluator::~Evaluator() = default;

Result<void> Evaluator::evaluate(bool record_timings, std::optional<std::uint32_t> seed_override) {
    auto result = impl_->evaluate(std::nullopt, record_timings, seed_override);
    if (!result)
        impl_->current_published.reset();
    return result;
}

Result<void> Evaluator::evaluate_node(ObjectId node, bool record_timings) {
    return impl_->evaluate(node, record_timings, std::nullopt);
}

const Geometry* Evaluator::output() const noexcept {
    return impl_->current_published ? &impl_->current_published->outputs.front() : nullptr;
}

std::shared_ptr<const Geometry> Evaluator::retain_output() const noexcept {
    return {impl_->current_published, output()};
}

const Geometry* Evaluator::last_valid_output() const noexcept {
    return impl_->last_valid ? &impl_->last_valid->outputs.front() : nullptr;
}

std::shared_ptr<const Geometry> Evaluator::retain_last_valid_output() const noexcept {
    return {impl_->last_valid, last_valid_output()};
}

bool Evaluator::has_display_node() const noexcept {
    return impl_->members.display() != ObjectId{};
}

std::optional<Error> Evaluator::display_error() const {
    const auto display = impl_->members.display();
    if (display == ObjectId{})
        return std::nullopt;
    const auto* shown = impl_->nodes.find(display);
    if (!shown || !shown->status.error)
        return std::nullopt;
    return shown->root_error ? shown->root_error : shown->status.error;
}

std::vector<Reference> Evaluator::display_dependencies() const {
    std::vector<Reference> result;
    const auto display = impl_->members.display();
    if (display == ObjectId{})
        return result;
    auto sequence = impl_->order(display, {}, true);
    if (!sequence)
        return result;
    for (const auto node : *sequence) {
        const auto* state = impl_->nodes.find(node);
        if (!state || !state->entry || state->bypassed)
            continue;
        for (const auto& dep : state->entry->dependencies) {
            if (std::ranges::find(result, dep) == result.end())
                result.push_back(dep);
        }
    }
    return result;
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

const NodeStatus* Evaluator::status(ObjectId node) const {
    const auto* found = impl_->nodes.find(node);
    return found ? &found->status : nullptr;
}

}
