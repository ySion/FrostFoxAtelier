#include "procedural/evaluator.hpp"
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

// A PointNode type's Source evaluate, bound for one program revision.
struct Code {
    enum class Use : std::uint8_t { context, read, write };

    struct Parameter {
        Use use;

        String name; // The point column, unless the context.

        std::size_t column{}; // ColumnData alternative.
    };

    source::Program::Entry entry;

    std::uint64_t identity{}; // Changes whenever the compiled code may behave differently.

    bool context{};

    std::vector<Parameter> parameters;
};

template <std::size_t I>
using ColumnArray = Array<typename std::variant_alternative_t<I, ColumnData>::Value>;

template <std::size_t I>
Result<void> prepare_column(Geometry& geometry, const Code::Parameter& parameter) {
    using T = typename ColumnArray<I>::Value;
    if (parameter.use == Code::Use::write) {
        auto column = geometry.add<T>(parameter.name, Layer::point);
        if (!column)
            return std::unexpected(column.error());
        return {};
    }
    auto column = geometry.read<T>(parameter.name, Layer::point);
    if (!column)
        return std::unexpected(column.error());
    if (!*column)
        return failure("The input has no point attribute " + std::string{parameter.name.view().std_view()} + ".");
    return {};
}

template <std::size_t I>
Argument borrow_column(Geometry& geometry, const Code::Parameter& parameter) {
    using T = typename ColumnArray<I>::Value;
    // prepare_column already checked the layer/type and made every written column unique.
    if (parameter.use == Code::Use::write)
        return Argument::borrow(**geometry.write<T>(parameter.name, Layer::point));
    return Argument::borrow(**geometry.read<T>(parameter.name, Layer::point));
}

struct ColumnAccess {
    Result<void> (*prepare)(Geometry&, const Code::Parameter&);

    Argument (*borrow)(Geometry&, const Code::Parameter&);
};

template <std::size_t... I>
constexpr std::array<ColumnAccess, sizeof...(I)> column_accessors(std::index_sequence<I...>) {
    return {ColumnAccess{&prepare_column<I>, &borrow_column<I>}...};
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

    std::uint64_t code_revision{}; // Of the program, when `sources` was bound.

    std::uint32_t seed{};

    ObjectId display;

    std::vector<ObjectId> members;

    std::unordered_map<ObjectId, State> nodes;

    std::unordered_map<TypeId, Result<Ports>> types;

    std::unordered_map<TypeId, Result<Code>> sources; // PointNode types.

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
        std::string local;
        append(local, *info); // Published TypeInfo addresses are stable identities for this Runtime.
        for (const auto& field : (*info)->fields)
            if (auto added = append_field(local, id, field); !added)
                return added;
        auto into = inputs(runtime, id);
        if (!into)
            return std::unexpected(into.error());
        state.type = *type;
        state.ports = &*known->second;
        state.local = std::move(local);
        state.inputs = std::move(*into);
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

    // Resolves `<type name>.evaluate` in the program and maps its parameters to the context and
    // point columns by name.
    Result<Code> bind(TypeId type) const {
        auto info = runtime.describe(type);
        if (!info)
            return std::unexpected(info.error());
        const auto& name = (*info)->name;
        if (!program)
            return error(ErrorCode::unsupported_operation, "Source node " + name + " needs a program.");
        const auto entries = program->entries();
        const auto found = std::ranges::find(entries, name + ".evaluate", &source::EntryInfo::name);
        if (found == entries.end())
            return error(ErrorCode::invalid_definition, "Source node " + name + " has no evaluate method.");
        const auto& signature = found->signature;
        if (signature.result || signature.execution != FunctionExecution::synchronous)
            return error(ErrorCode::invalid_definition, name + ".evaluate must be synchronous without a result.");
        // Borrowing makes evaluate a value region, where the compiler rejects writes to self.
        if (signature.parameters.empty())
            return error(ErrorCode::invalid_definition, name + ".evaluate must borrow the context or a point column.");
        auto context = runtime.native_type<Context>();
        if (!context)
            return std::unexpected(context.error());
        Code code;
        code.identity = found->code;
        for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
            const auto& parameter = signature.parameters[i];
            const auto& parameter_name = found->parameter_names[i];
            if (parameter.form == ValueForm::inline_value && parameter.passing != ParameterPassing::value) {
                const bool read = parameter.passing == ParameterPassing::read_only;
                if (parameter.type == *context && read && !code.context) {
                    code.context = true;
                    code.parameters.push_back({Code::Use::context, {}, 0});
                    continue;
                }
                if (const auto column = std::ranges::find(columns, parameter.type); column != columns.end()) {
                    code.parameters.push_back({read ? Code::Use::read : Code::Use::write,
                                               String{StringView{parameter_name}},
                                               static_cast<std::size_t>(column - columns.begin())});
                    continue;
                }
            }
            return error(ErrorCode::invalid_definition,
                         name + ".evaluate parameter " + parameter_name +
                             " is not a const procedural.Context& or a borrowed point column.");
        }
        auto entry = program->entry(found->name);
        if (!entry)
            return std::unexpected(entry.error());
        code.entry = std::move(*entry);
        return code;
    }

    // The input geometry with its point columns rewritten by the Source evaluate.
    Result<void> run_source(ObjectId id, const State& state, const Code& code, Geometry& output) const {
        output = empty;
        for (const auto& connection : state.inputs)
            output = nodes.at(connection.from).entry->outputs[connection.output];
        // Adding attributes can move their handles. Finish that, validation and detach before
        // taking any column references; the handles then stay untouched throughout invoke.
        for (const auto& parameter : code.parameters) {
            if (parameter.use == Code::Use::context)
                continue;
            auto prepared = accessors[parameter.column].prepare(output, parameter);
            if (!prepared)
                return std::unexpected(prepared.error());
        }
        Context context;
        context.seed_ = seed;
        std::vector<Argument> arguments;
        arguments.reserve(code.parameters.size());
        for (const auto& parameter : code.parameters) {
            if (parameter.use == Code::Use::context) {
                arguments.push_back(Argument::borrow(std::as_const(context)));
                continue;
            }
            arguments.push_back(accessors[parameter.column].borrow(output, parameter));
        }
        auto called = code.entry.invoke(id, arguments);
        if (!called)
            return std::unexpected(called.error());
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
        auto ran = code ? run_source(id, state, *code, entry->outputs.front()) : run_native(id, state, *entry);
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
            auto bound = sources.find(state.type);
            if (bound == sources.end())
                bound = sources.emplace(state.type, bind(state.type)).first;
            if (!bound->second) {
                state.status.error = bound->second.error();
                return;
            }
            code = &*bound->second;
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
        if (program && program->revision() != code_revision) {
            code_revision = program->revision();
            sources.clear();
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
            if (!added && !runtime.changed_since(node, *cursor))
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
