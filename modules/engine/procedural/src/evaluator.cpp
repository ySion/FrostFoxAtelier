#include "procedural/evaluator.hpp"
#include "foundation/result.hpp"
#include "source/program.hpp"

#include <list>
#include <string>
#include <unordered_map>
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

// A PointNode type's evaluate. Only Source entries are rebound on publication.
struct Code {
    enum class Use : std::uint8_t { context, read, write, group };

    struct Parameter {
        Use use;

        String name; // Parameter name; also the fallback point column.

        std::size_t column{}; // ColumnData alternative.

        TypeId type;

        FieldId selector; // Optional String field selecting this column at call time.

        Layer layer = Layer::point;
    };

    MethodId method; // Native target; null for Source.

    TypeId string_type;

    source::Program::Entry entry;

    std::uint64_t identity{}; // Changes whenever the compiled code may behave differently.

    bool context{};

    std::vector<Parameter> parameters;
};

template <std::size_t I>
using ColumnArray = Array<typename std::variant_alternative_t<I, ColumnData>::Value>;

template <std::size_t I>
Result<const void*> read_column(const Geometry& geometry, StringView name, Layer layer) {
    using T = typename ColumnArray<I>::Value;
    ATELIER_TRY_BIND(column, geometry.read<T>(name, layer));
    if (!*column)
        return failure("The input has no attribute " + std::string{name.std_view()} + ".");
    return *column;
}

template <std::size_t I>
Result<void*> write_column(Geometry& geometry, StringView name, Layer layer) {
    using T = typename ColumnArray<I>::Value;
    ATELIER_TRY_BIND(column, geometry.add<T>(name, layer));
    return *column;
}

struct ColumnAccess {
    Result<const void*> (*read)(const Geometry&, StringView, Layer);

    Result<void*> (*write)(Geometry&, StringView, Layer);
};

template <std::size_t... I>
constexpr std::array<ColumnAccess, sizeof...(I)> column_accessors(std::index_sequence<I...>) {
    return {ColumnAccess{&read_column<I>, &write_column<I>}...};
}

constexpr auto accessors = column_accessors(std::make_index_sequence<std::variant_size_v<ColumnData>>{});

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
        TypeId type;

        const Ports* ports{};

        std::string local; // Type and public field contents.

        std::vector<Input> inputs;

        std::optional<Error> fault; // Reading or keying the node failed.

        std::shared_ptr<Entry> entry; // The node's current result.

        std::string failed; // Key of the last failed evaluation, at failed_revision.

        std::uint64_t failed_revision{};

        Error failure;

        NodeStatus status;
    };

    Runtime& runtime;

    ObjectRoot graph;

    const Resources& resources;

    const source::Program* program;

    std::size_t budget;

    const Geometry empty;

    std::optional<std::uint64_t> cursor;

    std::uint64_t revision{};

    std::uint64_t code_revision{}; // Of the program, when Source entries were bound.

    std::uint32_t seed{};

    ObjectId display;

    std::vector<ObjectId> members;

    std::unordered_map<ObjectId, State> nodes;

    std::unordered_map<TypeId, Result<Ports>> types;

    std::unordered_map<TypeId, Result<Code>> kernels; // Native and Source PointNode types.

    std::array<TypeId, std::variant_size_v<ColumnData>>
        columns{}; // Array type per column alternative; null for String.

    std::unordered_map<std::string, std::shared_ptr<Entry>> cache;

    std::list<Entry*> recent; // Most recently used first.

    std::size_t cached_bytes{};

    std::uint64_t serials{};

    std::shared_ptr<Entry> published;

    Impl(Runtime& owner, ObjectRoot root, const Resources& host, const source::Program* code, std::size_t bytes)
        : runtime(owner), graph(std::move(root)), resources(host), program(code), budget(bytes),
          code_revision(code ? code->revision() : 0) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((columns[I] = [&]() -> TypeId {
                 if constexpr (std::same_as<typename ColumnArray<I>::Value, String>)
                     return {};
                 else {
                     auto type = runtime.native_type<ColumnArray<I>>();
                     return type ? *type : TypeId{};
                 }
             }()),
             ...);
        }(std::make_index_sequence<std::variant_size_v<ColumnData>>{});
    }

    Result<void> append_value(std::string& key, const TypeInfo& info, const void* data) const {
        if (info.kind == TypeKind::scalar || info.kind == TypeKind::enumeration || info.math) {
            key.append(static_cast<const char*>(data), info.size);
            return {};
        }
        if (info.kind == TypeKind::record && info.plain_value) {
            const std::span source{static_cast<const std::byte*>(data), info.size};
            // Ordinary records have padding; only their declared public fields are parameters.
            // Runtime resolves native member pointers too, so no published offset is required.
            for (const auto& field : info.fields) {
                auto child = runtime.describe(field.value.type);
                if (!child)
                    return std::unexpected(child.error());
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
        auto type = runtime.type_of(id);
        if (!type)
            return std::unexpected(type.error());
        auto known = types.find(*type);
        if (known == types.end())
            known = types.emplace(*type, procedural::ports(runtime, *type)).first;
        if (!known->second)
            return std::unexpected(known->second.error());
        auto info = runtime.describe(*type);
        if (!info)
            return std::unexpected(info.error());
        state.type = *type;
        state.ports = &*known->second;
        // Graph traversal still needs current connections when this node's binding fails.
        auto into = inputs(runtime, id);
        if (!into)
            return std::unexpected(into.error());
        state.inputs = std::move(*into);
        // Validate the declaration before field keying can reject an invalid selector's value type.
        if (state.ports->evaluate == MethodId{}) {
            auto bound = kernels.find(*type);
            if (bound == kernels.end())
                bound = kernels.emplace(*type, bind(*type)).first;
            if (!bound->second)
                return std::unexpected(bound->second.error());
        }
        std::string local;
        append(local, *info); // Published TypeInfo addresses are stable identities for this Runtime.
        for (const auto& field : (*info)->fields)
            if (auto added = append_field(local, id, field); !added)
                return added;
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
        cache.erase(cache.find(entry.key)); // May release the entry; touch nothing after this.
    }

    void store(const std::shared_ptr<Entry>& entry) {
        if (auto old = cache.find(entry->key); old != cache.end())
            uncache(*old->second);
        cache.emplace(entry->key, entry);
        recent.push_front(entry.get());
        entry->recent = recent.begin();
        entry->cached = true;
        cached_bytes += entry->bytes;
        while (cached_bytes > budget)
            uncache(*recent.back());
    }

    // Both producers supply the same signature and field Meta. The plan owns no instance names
    // or column addresses; Source publication invalidates its entry and parameter mapping.
    Result<Code> bind(TypeId type) const {
        ATELIER_TRY_BIND(info, runtime.describe(type));
        const auto& name = (*info)->name;
        Code code;
        const FunctionSignature* signature;
        std::span<const std::string> names;
        if ((*info)->native_storage) {
            const auto method = std::ranges::find((*info)->methods, "evaluate", &MethodInfo::name);
            if (method == (*info)->methods.end())
                return error(ErrorCode::invalid_definition, name + " has no evaluate method.");
            if (!method->signature.receiver || !method->signature.receiver->read_only)
                return error(ErrorCode::invalid_definition, name + ".evaluate must be const.");
            code.method = method->id;
            signature = &method->signature;
            names = method->parameter_names;
        } else {
            if (!program)
                return error(ErrorCode::unsupported_operation, "Source node " + name + " needs a program.");
            const auto entries = program->entries();
            const auto found = std::ranges::find(entries, name + ".evaluate", &source::EntryInfo::name);
            if (found == entries.end())
                return error(ErrorCode::invalid_definition, name + " has no evaluate method.");
            ATELIER_TRY_BIND(entry, program->entry(found->name));
            code.entry = std::move(*entry);
            code.identity = found->code;
            signature = &found->signature;
            names = found->parameter_names;
        }
        if (signature->result || signature->execution != FunctionExecution::synchronous)
            return error(ErrorCode::invalid_definition, name + ".evaluate must be synchronous without a result.");
        // Borrowing makes evaluate a value region, where the compiler rejects writes to self.
        if (signature->parameters.empty())
            return error(ErrorCode::invalid_definition, name + ".evaluate must borrow the context or a point column.");
        ATELIER_TRY_BIND(context, runtime.native_type<Context>());
        for (std::size_t i = 0; i < signature->parameters.size(); ++i) {
            const auto& parameter = signature->parameters[i];
            const auto& parameter_name = names[i];
            if (parameter.form == ValueForm::inline_value && parameter.passing != ParameterPassing::value) {
                const bool read = parameter.passing == ParameterPassing::read_only;
                if (parameter.type == *context && read && !code.context) {
                    code.context = true;
                    code.parameters.push_back({Code::Use::context, {}, 0, parameter.type, {}});
                    continue;
                }
                if (const auto column = std::ranges::find(columns, parameter.type); column != columns.end()) {
                    auto owned_name = String::from_utf8(StringView{parameter_name});
                    if (!owned_name || parameter_name.empty())
                        return error(ErrorCode::invalid_definition,
                                     name + ".evaluate column parameters need nonempty UTF-8 names.");
                    code.parameters.push_back({read ? Code::Use::read : Code::Use::write,
                                               std::move(*owned_name),
                                               static_cast<std::size_t>(column - columns.begin()),
                                               parameter.type,
                                               {}});
                    continue;
                }
            }
            return error(ErrorCode::invalid_definition,
                         name + ".evaluate parameter " + parameter_name +
                             " is not a const procedural.Context& or a borrowed point column.");
        }
        ATELIER_TRY_BIND(string_type, runtime.native_type<String>());
        code.string_type = *string_type;
        constexpr std::array<std::string_view, 5> layers{"point", "vertex", "primitive", "detail", "instance"};
        for (const auto& field : (*info)->fields) {
            std::string_view binding;
            std::string_view layer;
            std::string_view kind;
            bool declared = false;
            for (const auto& [key, value] : field.metadata) {
                if (!key.starts_with("procedural."))
                    continue;
                declared = true;
                if (key == "procedural.bind")
                    binding = value;
                else if (key == "procedural.layer")
                    layer = value;
                else if (key == "procedural.kind")
                    kind = value;
                else
                    return error(ErrorCode::invalid_definition, name + "." + field.name + ": unknown Meta " + key);
            }
            if (!declared)
                continue;
            const auto at = std::ranges::find(layers, layer);
            if (field.value.form != ValueForm::inline_value || field.value.type != *string_type ||
                !binding.starts_with("evaluate.") || at == layers.end() || (kind != "attribute" && kind != "group"))
                return error(ErrorCode::invalid_definition, name + "." + field.name + ": invalid column binding.");
            const auto target = binding.substr(std::string_view{"evaluate."}.size());
            const auto parameter = std::ranges::find_if(code.parameters, [&](const auto& p) {
                return p.use != Code::Use::context && p.name.view().std_view() == target;
            });
            if (parameter == code.parameters.end() || parameter->selector != FieldId{})
                return error(ErrorCode::invalid_definition,
                             name + "." + field.name + ": binding target is not unique.");
            if (kind == "group") {
                if (parameter->use != Code::Use::read || parameter->type != columns[0])
                    return error(ErrorCode::invalid_definition,
                                 name + "." + field.name + ": a group requires const Array<bool>&.");
                parameter->use = Code::Use::group;
            }
            parameter->selector = field.id;
            parameter->layer = static_cast<Layer>(at - layers.begin());
        }
        return code;
    }

    Result<void> run_columns(ObjectId id, const State& state, const Code& code, Entry& entry) const {
        const Geometry* input = &empty;
        for (const auto& connection : state.inputs)
            input = &nodes.at(connection.from).entry->outputs[connection.output];
        std::vector<String> names(code.parameters.size());
        for (std::size_t i = 0; i < code.parameters.size(); ++i) {
            const auto& parameter = code.parameters[i];
            if (parameter.selector != FieldId{}) {
                ATELIER_TRY(runtime.read(id, parameter.selector, ValueOutput{code.string_type, &names[i]}));
            } else {
                names[i] = parameter.name;
            }
            if (parameter.use == Code::Use::write)
                for (std::size_t j = 0; j < i; ++j)
                    if (code.parameters[j].use == Code::Use::write && names[j] == names[i])
                        return failure("Multiple writable parameters select " +
                                       std::string{names[i].view().std_view()} + ".");
        }
        Context context;
        context.seed_ = seed;
        context.resources_ = &resources;
        context.dependencies_ = &entry.dependencies;
        std::array<Array<bool>, 5> all;
        std::vector<const void*> values(code.parameters.size());
        std::vector<void*> writable(code.parameters.size());
        // Reads must resolve from the old input, even if an earlier write parameter would
        // create their column. Empty groups are call-owned masks, never output attributes.
        for (std::size_t i = 0; i < code.parameters.size(); ++i) {
            const auto& parameter = code.parameters[i];
            if (parameter.use == Code::Use::context) {
                values[i] = &context;
                continue;
            }
            if (parameter.use == Code::Use::write)
                continue;
            if (parameter.use == Code::Use::group && names[i].empty()) {
                auto& mask = all[static_cast<std::size_t>(parameter.layer)];
                if (mask.empty())
                    mask.resize(input->count(parameter.layer), true);
                values[i] = &mask;
            } else {
                ATELIER_TRY_BIND(column, accessors[parameter.column].read(*input, names[i], parameter.layer));
                values[i] = *column;
            }
        }
        auto& output = entry.outputs.front();
        output = *input;
        for (std::size_t i = 0; i < code.parameters.size(); ++i) {
            const auto& parameter = code.parameters[i];
            if (parameter.use != Code::Use::write)
                continue;
            ATELIER_TRY_BIND(column, accessors[parameter.column].write(output, names[i], parameter.layer));
            values[i] = writable[i] = *column;
        }
        // Array owners live in stable SharedArray blocks even when attributes grow. Do not
        // use the prepared addresses until all additions/COW finish; no writes repeat a column.
        std::vector<Argument> arguments;
        arguments.reserve(code.parameters.size());
        for (std::size_t i = 0; i < code.parameters.size(); ++i)
            arguments.push_back(Argument::borrow(code.parameters[i].type, values[i], writable[i]));
        if (code.method != MethodId{}) {
            return runtime.call(id, code.method, arguments);
        }
        ATELIER_TRY(code.entry.invoke(id, arguments));
        return {};
    }

    // Calls the native evaluate with its ports in parameter order.
    Result<void> run_native(ObjectId id, const State& state, Entry& entry) {
        Context context;
        context.seed_ = seed;
        context.resources_ = &resources;
        context.dependencies_ = &entry.dependencies;
        std::vector<Geometries> lists;
        lists.reserve(state.ports->multiple.size()); // Borrowed below; must not move.
        std::vector<Argument> arguments;
        arguments.reserve(state.ports->parameters.size());
        std::uint32_t input = 0;
        std::uint32_t output = 0;
        const auto upstream = [&](const Input& connection) -> const Geometry& {
            return nodes.at(connection.from).entry->outputs[connection.output];
        };
        for (const auto slot : state.ports->parameters) {
            switch (slot) {
            case Slot::context:
                arguments.push_back(Argument::borrow(std::as_const(context)));
                break;
            case Slot::input: {
                const Geometry* value = &empty;
                for (const auto& connection : state.inputs)
                    if (connection.input == input)
                        value = &upstream(connection);
                arguments.push_back(Argument::borrow(*value));
                ++input;
                break;
            }
            case Slot::inputs: {
                auto& list = lists.emplace_back();
                for (const auto& connection : state.inputs)
                    if (connection.input == input)
                        list.add(upstream(connection));
                arguments.push_back(Argument::borrow(std::as_const(list)));
                ++input;
                break;
            }
            case Slot::output:
                arguments.push_back(Argument::borrow(entry.outputs[output++]));
                break;
            }
        }
        return runtime.call(id, state.ports->evaluate, arguments);
    }

    Result<std::shared_ptr<Entry>> run(ObjectId id, State& state, const Code* code, std::string key,
                                       bool record_timings) {
        auto entry = std::make_shared<Entry>();
        entry->key = std::move(key);
        entry->outputs.resize(state.ports->outputs.size());
        ++state.status.evaluations;
        state.status.time.reset();
        std::chrono::steady_clock::time_point start{};
        if (record_timings)
            start = std::chrono::steady_clock::now();
        auto ran = code ? run_columns(id, state, *code, *entry) : run_native(id, state, *entry);
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

    void update(ObjectId id, State& state, bool record_timings) {
        if (state.fault) {
            state.status.error = state.fault;
            return;
        }
        std::string key = state.local;
        const Code* code{};
        if (state.ports->evaluate == MethodId{}) {
            code = &*kernels.at(state.type); // read() already validated the current plan.
            append(key, code->identity);
            if (code->context)
                append(key, seed);
        } else if (std::ranges::find(state.ports->parameters, Slot::context) != state.ports->parameters.end())
            append(key, seed);
        for (const auto& connection : state.inputs) {
            const auto& upstream = nodes.at(connection.from);
            if (upstream.status.error || !upstream.entry) {
                state.status.error =
                    Error{ErrorCode::invocation_failed, "Input " + std::to_string(connection.input) + " failed."};
                return;
            }
            append(key, connection.input);
            append(key, connection.output);
            append(key, upstream.entry->serial);
        }
        if (state.entry && state.entry->key == key && current(*state.entry)) {
            touch(*state.entry);
            state.status.error.reset();
            return;
        }
        if (key == state.failed && revision == state.failed_revision) {
            state.status.error = state.failure;
            return;
        }
        if (const auto found = cache.find(key); found != cache.end() && current(*found->second)) {
            state.entry = found->second;
            touch(*state.entry);
            ++state.status.cache_hits;
            state.status.error.reset();
            return;
        }
        auto evaluated = run(id, state, code, key, record_timings);
        if (!evaluated) {
            state.entry.reset();
            state.failed = std::move(key);
            state.failed_revision = revision;
            state.failure = evaluated.error();
            state.status.error = evaluated.error();
            return;
        }
        state.entry = std::move(*evaluated);
        state.failed.clear();
        state.status.error.reset();
    }

    // Upstream first, ending at the display node.
    Result<std::vector<ObjectId>> order() const {
        std::vector<ObjectId> result;
        if (display == ObjectId{})
            return result;
        struct Frame {
            ObjectId id;

            std::size_t next;
        };
        std::unordered_map<ObjectId, bool> finished; // false while on the path.
        std::vector<Frame> path{{display, 0}};
        finished.emplace(display, false);
        while (!path.empty()) {
            const auto id = path.back().id;
            const auto found = nodes.find(id);
            if (found == nodes.end())
                return error(ErrorCode::invalid_member, "A connection leaves the graph.");
            if (path.back().next < found->second.inputs.size()) {
                const auto from = found->second.inputs[path.back().next++].from;
                const auto [mark, added] = finished.emplace(from, false);
                if (added)
                    path.push_back({from, 0});
                else if (!mark->second)
                    return error(ErrorCode::argument_mismatch, "The graph has a cycle.");
                continue;
            }
            finished[id] = true;
            result.push_back(id);
            path.pop_back();
        }
        return result;
    }

    Result<void> evaluate(bool record_timings) {
        auto now = runtime.change_cursor();
        if (!now)
            return std::unexpected(now.error());
        const auto id = graph.id();
        bool dirty = !cursor || resources.revision() != revision;
        // A publication may change any Source node's code; rebinding finds which did.
        const bool republished = program && program->revision() != code_revision;
        if (republished) {
            code_revision = program->revision();
            std::erase_if(kernels, [](const auto& item) { return !item.second || item.second->method == MethodId{}; });
            dirty = true;
        }
        if (!cursor || runtime.changed_since(id, *cursor)) {
            auto listed = contents(runtime, id);
            if (!listed)
                return std::unexpected(listed.error());
            auto type = runtime.native_type<Graph>();
            if (!type)
                return std::unexpected(type.error());
            auto field = runtime.find_field(*type, "seed");
            if (!field)
                return std::unexpected(field.error());
            auto value = runtime.read(id, *field);
            if (!value)
                return std::unexpected(value.error());
            seed = std::get<std::uint32_t>(*value);
            display = listed->display;
            members = std::move(listed->nodes);
            std::erase_if(nodes,
                          [&](const auto& node) { return std::ranges::find(members, node.first) == members.end(); });
            dirty = true;
        }
        for (const auto node : members) {
            auto [found, added] = nodes.try_emplace(node);
            auto& state = found->second;
            // Every instance must refresh a possible binding fault, not only the first of its type.
            const bool stale_code = republished && state.ports && state.ports->evaluate == MethodId{};
            if (!added && !stale_code && !runtime.changed_since(node, *cursor))
                continue;
            const auto local = state.local;
            const auto connections = state.inputs;
            auto read = this->read(node, state);
            state.fault.reset();
            if (!read)
                state.fault = read.error();
            dirty = dirty || added || !read || state.local != local || state.inputs != connections;
        }
        cursor = *now;
        if (!dirty)
            return {};
        revision = resources.revision();
        auto sequence = order();
        if (!sequence)
            return std::unexpected(sequence.error());
        for (const auto node : *sequence)
            update(node, nodes.at(node), record_timings);
        if (display == ObjectId{})
            published.reset();
        else if (const auto& shown = nodes.at(display); !shown.status.error && shown.entry)
            published = shown.entry;
        return {};
    }
};

Evaluator::Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, const source::Program* program,
                     std::size_t cache_bytes)
    : impl_(std::make_unique<Impl>(runtime, std::move(graph), resources, program, cache_bytes)) {}

Evaluator::~Evaluator() = default;

Result<void> Evaluator::evaluate(bool record_timings) {
    return impl_->evaluate(record_timings);
}

const Geometry* Evaluator::output() const noexcept {
    return impl_->published ? &impl_->published->outputs.front() : nullptr;
}

const NodeStatus* Evaluator::status(ObjectId node) const {
    const auto found = impl_->nodes.find(node);
    return found == impl_->nodes.end() ? nullptr : &found->second.status;
}

}
