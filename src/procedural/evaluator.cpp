#include "procedural/evaluator.hpp"

#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace atelier::procedural {
namespace {

std::unexpected<Error> error(ErrorCode code, std::string message) { return std::unexpected(Error{code, std::move(message)}); }

template<class T> void append(std::string& key, const T& value) {
    key.append(reinterpret_cast<const char*>(&value), sizeof value);
}

// One evaluation of one node. Keys are exact byte strings, so equal keys mean equal contents.
struct Entry {
    std::string key;
    std::uint64_t serial{};               // Unique per evaluation; downstream keys use it.
    std::vector<Geometry> outputs;
    std::vector<Reference> dependencies;  // Resources read, with what they resolved to.
    std::uint64_t revision{};             // Resources revision the dependencies were confirmed at.
    std::size_t bytes{};
    bool cached{};
    std::list<Entry*>::iterator recent;   // Valid while cached.
};

}

struct Evaluator::Impl {
    struct State {
        const Ports* ports{};
        std::string local;                // Type and public field contents.
        std::vector<Input> inputs;
        std::optional<Error> fault;       // Reading or keying the node failed.
        std::shared_ptr<Entry> entry;     // The node's current result.
        std::string failed;               // Key of the last failed evaluation, at failed_revision.
        std::uint64_t failed_revision{};
        Error failure;
        NodeStatus status;
    };

    Runtime& runtime;
    ObjectRoot graph;
    const Resources& resources;
    std::size_t budget;
    const Geometry empty;
    std::optional<std::uint64_t> cursor;
    std::uint64_t revision{};
    std::uint32_t seed{};
    ObjectId display;
    std::vector<ObjectId> members;
    std::unordered_map<ObjectId, State> nodes;
    std::unordered_map<TypeId, Result<Ports>> types;
    std::unordered_map<std::string, std::shared_ptr<Entry>> cache;
    std::list<Entry*> recent;             // Most recently used first.
    std::size_t cached_bytes{};
    std::uint64_t serials{};
    std::shared_ptr<Entry> published;

    Impl(Runtime& owner, ObjectRoot root, const Resources& source, std::size_t bytes)
        : runtime(owner), graph(std::move(root)), resources(source), budget(bytes) {}

    Result<void> append_value(std::string& key, const TypeInfo& info, const void* data) const {
        if (info.kind == TypeKind::scalar || info.kind == TypeKind::enumeration || info.plain_value) {
            key.append(static_cast<const char*>(data), info.size);
            return {};
        }
        if ((info.kind != TypeKind::string && info.kind != TypeKind::array) || !info.sequence_view)
            return error(ErrorCode::unsupported_operation, "Node parameters of type " + info.name + " are not supported.");
        const void* items{};
        std::size_t count{};
        info.sequence_view(data, &items, &count);
        append(key, count);
        if (info.kind == TypeKind::string) {
            key.append(static_cast<const char*>(items), count);
            return {};
        }
        auto element = runtime.describe(*info.element);
        if (!element) return std::unexpected(element.error());
        for (std::size_t i = 0; i < count; ++i)
            if (auto added = append_value(key, **element, static_cast<const std::byte*>(items) + i * (*element)->size); !added)
                return added;
        return {};
    }

    Result<void> append_field(std::string& key, ObjectId object, const FieldInfo& field) {
        if (field.value.form != ValueForm::inline_value)
            return error(ErrorCode::unsupported_operation, "Node parameter " + field.name + " is a reference.");
        auto info = runtime.describe(field.value.type);
        if (!info) return std::unexpected(info.error());
        if ((*info)->kind == TypeKind::scalar || (*info)->kind == TypeKind::enumeration) {
            auto value = runtime.read(object, field.id);
            if (!value) return std::unexpected(value.error());
            key.push_back(static_cast<char>(value->index()));
            std::visit([&](const auto& scalar) {
                if constexpr (std::same_as<std::remove_cvref_t<decltype(scalar)>, EnumValue>) append(key, scalar.bits);
                else append(key, scalar);
            }, *value);
            return {};
        }
        atelier::detail::ValueStorage storage;
        const std::span bytes{storage.prepare((*info)->size, (*info)->alignment), (*info)->size};
        if (auto made = runtime.initialize_value(field.value.type, bytes); !made) return made;
        auto added = runtime.read(object, field.id, ValueOutput{field.value.type, bytes.data()});
        if (added) added = append_value(key, **info, bytes.data());
        (void)runtime.destroy_value(field.value.type, bytes);
        return added;
    }

    Result<void> read(ObjectId id, State& state) {
        auto type = runtime.type_of(id);
        if (!type) return std::unexpected(type.error());
        auto known = types.find(*type);
        if (known == types.end()) known = types.emplace(*type, procedural::ports(runtime, *type)).first;
        if (!known->second) return std::unexpected(known->second.error());
        auto info = runtime.describe(*type);
        if (!info) return std::unexpected(info.error());
        std::string local;
        append(local, *info); // Published TypeInfo addresses are stable identities for this Runtime.
        for (const auto& field : (*info)->fields)
            if (auto added = append_field(local, id, field); !added) return added;
        auto into = inputs(runtime, id);
        if (!into) return std::unexpected(into.error());
        state.ports = &*known->second;
        state.local = std::move(local);
        state.inputs = std::move(*into);
        return {};
    }

    // Whether the resources an entry read still resolve as they did.
    bool current(Entry& entry) const {
        if (entry.revision == revision) return true;
        for (const auto& dependency : entry.dependencies) {
            auto mesh = resources.mesh(dependency.key);
            if (!mesh || *mesh != dependency.mesh) return false;
        }
        entry.revision = revision;
        return true;
    }

    void touch(Entry& entry) {
        if (entry.cached) recent.splice(recent.begin(), recent, entry.recent);
    }
    void uncache(Entry& entry) {
        entry.cached = false;
        cached_bytes -= entry.bytes;
        recent.erase(entry.recent);
        cache.erase(cache.find(entry.key)); // May release the entry; touch nothing after this.
    }
    void store(const std::shared_ptr<Entry>& entry) {
        if (auto old = cache.find(entry->key); old != cache.end()) uncache(*old->second);
        cache.emplace(entry->key, entry);
        recent.push_front(entry.get());
        entry->recent = recent.begin();
        entry->cached = true;
        cached_bytes += entry->bytes;
        while (cached_bytes > budget) uncache(*recent.back());
    }

    Result<std::shared_ptr<Entry>> run(ObjectId id, State& state, std::string key) {
        auto entry = std::make_shared<Entry>();
        entry->key = std::move(key);
        entry->outputs.resize(state.ports->outputs);
        Context context;
        context.seed = seed;
        context.resources_ = &resources;
        context.dependencies_ = &entry->dependencies;
        std::vector<Geometries> lists;
        lists.reserve(state.ports->multiple.size()); // Borrowed below; must not move.
        std::vector<Argument> arguments;
        arguments.reserve(state.ports->parameters.size());
        std::uint32_t input = 0, output = 0;
        const auto upstream = [&](const Input& connection) -> const Geometry& {
            return nodes.at(connection.from).entry->outputs[connection.output];
        };
        for (const auto slot : state.ports->parameters) {
            switch (slot) {
            case Slot::context: arguments.push_back(Argument::borrow(std::as_const(context))); break;
            case Slot::input: {
                const Geometry* value = &empty;
                for (const auto& connection : state.inputs)
                    if (connection.input == input) value = &upstream(connection);
                arguments.push_back(Argument::borrow(*value));
                ++input;
                break;
            }
            case Slot::inputs: {
                auto& list = lists.emplace_back();
                for (const auto& connection : state.inputs)
                    if (connection.input == input) list.add(upstream(connection));
                arguments.push_back(Argument::borrow(std::as_const(list)));
                ++input;
                break;
            }
            case Slot::output: arguments.push_back(Argument::borrow(entry->outputs[output++])); break;
            }
        }
        ++state.status.evaluations;
        if (auto called = runtime.call(id, state.ports->evaluate, arguments); !called) return std::unexpected(called.error());
        entry->serial = ++serials;
        entry->revision = revision;
        entry->bytes = sizeof(Entry) + entry->key.size();
        for (const auto& geometry : entry->outputs) entry->bytes += geometry.bytes();
        store(entry);
        return entry;
    }

    void update(ObjectId id, State& state) {
        if (state.fault) {
            state.status.error = state.fault;
            return;
        }
        std::string key = state.local;
        if (std::ranges::find(state.ports->parameters, Slot::context) != state.ports->parameters.end()) append(key, seed);
        for (const auto& connection : state.inputs) {
            const auto& upstream = nodes.at(connection.from);
            if (upstream.status.error || !upstream.entry) {
                state.status.error = Error{ErrorCode::invocation_failed, "Input " + std::to_string(connection.input) + " failed."};
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
        auto evaluated = run(id, state, key);
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
        if (display == ObjectId{}) return result;
        struct Frame { ObjectId id; std::size_t next; };
        std::unordered_map<ObjectId, bool> finished; // false while on the path.
        std::vector<Frame> path{{display, 0}};
        finished.emplace(display, false);
        while (!path.empty()) {
            const auto id = path.back().id;
            const auto found = nodes.find(id);
            if (found == nodes.end()) return error(ErrorCode::invalid_member, "A connection leaves the graph.");
            if (path.back().next < found->second.inputs.size()) {
                const auto from = found->second.inputs[path.back().next++].from;
                const auto [mark, added] = finished.emplace(from, false);
                if (added) path.push_back({from, 0});
                else if (!mark->second) return error(ErrorCode::argument_mismatch, "The graph has a cycle.");
                continue;
            }
            finished[id] = true;
            result.push_back(id);
            path.pop_back();
        }
        return result;
    }

    Result<void> evaluate() {
        auto now = runtime.change_cursor();
        if (!now) return std::unexpected(now.error());
        const auto id = graph.id();
        bool dirty = !cursor || resources.revision() != revision;
        if (!cursor || runtime.changed_since(id, *cursor)) {
            auto listed = contents(runtime, id);
            if (!listed) return std::unexpected(listed.error());
            auto type = runtime.native_type<Graph>();
            if (!type) return std::unexpected(type.error());
            auto field = runtime.find_field(*type, "seed");
            if (!field) return std::unexpected(field.error());
            auto value = runtime.read(id, *field);
            if (!value) return std::unexpected(value.error());
            seed = std::get<std::uint32_t>(*value);
            display = listed->display;
            members = std::move(listed->nodes);
            std::erase_if(nodes, [&](const auto& node) { return std::ranges::find(members, node.first) == members.end(); });
            dirty = true;
        }
        for (const auto node : members) {
            auto [found, added] = nodes.try_emplace(node);
            auto& state = found->second;
            if (!added && !runtime.changed_since(node, *cursor)) continue;
            const auto local = state.local;
            const auto connections = state.inputs;
            auto read = this->read(node, state);
            state.fault.reset();
            if (!read) state.fault = read.error();
            dirty = dirty || added || !read || state.local != local || state.inputs != connections;
        }
        cursor = *now;
        if (!dirty) return {};
        revision = resources.revision();
        auto sequence = order();
        if (!sequence) return std::unexpected(sequence.error());
        for (const auto node : *sequence) update(node, nodes.at(node));
        if (display == ObjectId{}) published.reset();
        else if (const auto& shown = nodes.at(display); !shown.status.error && shown.entry) published = shown.entry;
        return {};
    }
};

Evaluator::Evaluator(Runtime& runtime, ObjectRoot graph, const Resources& resources, std::size_t cache_bytes)
    : impl_(std::make_unique<Impl>(runtime, std::move(graph), resources, cache_bytes)) {}
Evaluator::~Evaluator() = default;

Result<void> Evaluator::evaluate() { return impl_->evaluate(); }

const Geometry* Evaluator::output() const noexcept {
    return impl_->published ? &impl_->published->outputs.front() : nullptr;
}

const NodeStatus* Evaluator::status(ObjectId node) const {
    const auto found = impl_->nodes.find(node);
    return found == impl_->nodes.end() ? nullptr : &found->second.status;
}

}
