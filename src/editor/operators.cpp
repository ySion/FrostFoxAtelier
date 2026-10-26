#include "editor/operators.hpp"
#include "editor/properties.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <new>
#include <unordered_set>

namespace atelier::editor {
namespace {
Error invalid(std::string message) { return Error{ErrorCode::invalid_definition, std::move(message)}; }
// The caller sent an operator call this registry cannot accept.
Error rejected(std::string message) { return Error{ErrorCode::argument_mismatch, std::move(message)}; }
// A live value of an operator's parameter record in owned, aligned storage.
class Parameters {
public:
    Parameters(Runtime& runtime, TypeId type, std::span<std::byte> bytes, std::align_val_t alignment) noexcept
        : runtime(runtime), type(type), bytes(bytes), alignment(alignment) {}
    Parameters(const Parameters&) = delete;
    Parameters& operator=(const Parameters&) = delete;
    ~Parameters() {
        (void)runtime.destroy_value(type, bytes);
        ::operator delete(bytes.data(), alignment);
    }
    Runtime& runtime;
    const TypeId type;
    const std::span<std::byte> bytes;
    const std::align_val_t alignment;
};
// Default-constructed parameters of `op`, resolved by name in this document's Runtime.
Result<std::unique_ptr<Parameters>> parameters(Runtime& runtime, const OperatorInfo& op) {
    const auto type = runtime.find_type(op.parameters);
    if (!type) return std::unexpected(invalid("Operator " + op.id + " takes " + op.parameters + ", which this document does not publish."));
    const auto info = runtime.describe(*type);
    if (!info) return std::unexpected(info.error());
    if ((*info)->kind != TypeKind::record) return std::unexpected(invalid("Operator parameters must be a record: " + op.parameters));
    // Rooting references inside parameter values arrives with the first reference parameter.
    if ((*info)->contains_references) return std::unexpected(invalid("Operator parameters cannot hold references yet: " + op.parameters));
    const std::align_val_t alignment{(*info)->alignment};
    const std::span bytes{static_cast<std::byte*>(::operator new((*info)->size, alignment)), (*info)->size};
    if (auto ready = runtime.initialize_value(*type, bytes); !ready) {
        ::operator delete(bytes.data(), alignment);
        return std::unexpected(ready.error());
    }
    return std::make_unique<Parameters>(runtime, *type, bytes, alignment);
}
bool valid_id(std::string_view id) {
    const auto dot = id.find('.');
    if (dot == std::string_view::npos || id.find('.', dot + 1) != std::string_view::npos) return false;
    const auto part = [](std::string_view text) {
        return !text.empty() && std::islower(static_cast<unsigned char>(text.front())) &&
            std::ranges::all_of(text, [](char c) { return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_'; });
    };
    return part(id.substr(0, dot)) && part(id.substr(dot + 1));
}
Result<void> admit(const Context& context, const ContextOverride& changes) {
    if (!changes.selection) return {};
    const auto& selection = *changes.selection;
    if (selection.entities.size() > Session::maximum_entities) return std::unexpected(rejected("The selection exceeds the Entity limit."));
    auto members = context.document.world().members();
    if (!members) return std::unexpected(members.error());
    std::unordered_set<ObjectId> entities, seen;
    for (const auto& member : *members) entities.insert(member.id());
    for (const auto entity : selection.entities) {
        if (!seen.insert(entity).second) return std::unexpected(rejected("The selection repeats an Entity."));
        if (!entities.contains(entity)) return std::unexpected(rejected("The selection names an object that is not an Entity of this document."));
    }
    if (selection.active != ObjectId{} && std::ranges::find(selection.entities, selection.active) == selection.entities.end())
        return std::unexpected(rejected("The active Entity must be selected."));
    if (selection.object != ObjectId{} && selection.object != selection.active) {
        auto component = world::inspect_component(context.document.world().runtime(), selection.object);
        if (!component || !component->valid || component->owner != selection.active)
            return std::unexpected(rejected("The inspected object must be the active Entity or one of its components."));
    }
    return {};
}
std::string history_violation(const OperatorInfo& info, std::uint64_t recorded, std::size_t undo, std::size_t redo,
    std::uint64_t recorded_after, std::size_t undo_after, std::size_t redo_after) {
    const auto entries = recorded_after - recorded;
    const bool unchanged = entries == 0 && undo_after == undo && redo_after == redo;
    bool valid = unchanged;
    std::string_view flag = "none";
    if (has_flag(info.flags, OperatorFlag::replaces_document)) return {};
    if (has_flag(info.flags, OperatorFlag::undo)) {
        flag = "undo";
        valid = unchanged || (entries == 1 && redo_after == 0);
    } else if (has_flag(info.flags, OperatorFlag::history)) {
        flag = "history";
        valid = entries == 0 && undo_after + redo_after == undo + redo && (undo_after > undo ? undo_after - undo : undo - undo_after) <= 1;
    }
    if (valid) return {};
    return "Operator " + info.id + " (" + std::string(flag) + ") broke its history contract: " + std::to_string(entries) +
        " entries recorded, undo " + std::to_string(undo) + "->" + std::to_string(undo_after) + ", redo " +
        std::to_string(redo) + "->" + std::to_string(redo_after) + ".";
}
}
std::span<const std::string_view> region_names() noexcept {
    static constexpr std::array<std::string_view, 10> names{"none", "viewport", "scene_tree", "structure", "parameters", "assets", "code", "output", "world", "controls"};
    return names;
}
Result<void> OperatorRegistry::add(OperatorInfo info) {
    if (!valid_id(info.id)) return std::unexpected(invalid("Operator ID must be lowercase <domain>.<action>: " + info.id));
    if (find(info.id)) return std::unexpected(invalid("Operator is already registered: " + info.id));
    if (!info.poll || !info.execute) return std::unexpected(invalid("Operator needs poll and execute: " + info.id));
    if (info.seed && info.parameters.empty()) return std::unexpected(invalid("Only an operator with parameters can seed them: " + info.id));
    const auto exclusive = int(has_flag(info.flags, OperatorFlag::undo)) + int(has_flag(info.flags, OperatorFlag::history)) +
        int(has_flag(info.flags, OperatorFlag::replaces_document));
    if (exclusive > 1) return std::unexpected(invalid("Operator history flags are exclusive: " + info.id));
    operators_.push_back(std::move(info));
    return {};
}
const OperatorInfo* OperatorRegistry::find(std::string_view id) const noexcept {
    const auto found = std::ranges::find(operators_, id, &OperatorInfo::id);
    return found == operators_.end() ? nullptr : &*found;
}
Result<OperatorReport> call(const OperatorRegistry& registry, OperatorHost& host, const Context& base, const OperatorCall& request) {
    const auto* info = registry.find(request.id);
    if (!info) return std::unexpected(rejected("Unknown operator: " + request.id));
    if (!request.args.is_object()) return std::unexpected(rejected("Operator arguments must be an object."));
    if (info->parameters.empty() && !request.args.empty()) return std::unexpected(rejected("Unknown argument for " + request.id + ": " + request.args.begin().key()));
    if (auto admitted = admit(base, request.context); !admitted) return std::unexpected(admitted.error());
    Context context = base;
    if (request.context.selection) context.selection = *request.context.selection;
    if (request.context.region) context.region = *request.context.region;
    if (auto available = info->poll(context); !available) return OperatorReport{OperatorStatus::unavailable, available.error().message};
    // A replacing operator only files a host request, so the Session stays valid here.
    auto& document = context.document;
    std::unique_ptr<Parameters> value;
    if (!info->parameters.empty()) {
        auto& runtime = document.world().runtime();
        auto made = parameters(runtime, *info);
        if (!made) return std::unexpected(made.error());
        value = std::move(*made);
        if (info->seed) if (auto seeded = info->seed(context, ValueOutput{value->type, value->bytes.data()}); !seeded) return std::unexpected(seeded.error());
        if (auto decoded = decode_properties(runtime, PropertyTarget{value->type, {}, value->bytes}, request.args); !decoded)
            return std::unexpected(decoded.error());
    }
    if (auto ended = host.end_interaction(); !ended) return std::unexpected(ended.error());
    const auto recorded = document.recorded(), undo = document.undo_count(), redo = document.redo_count();
    if (auto executed = info->execute(host, context, value ? ValueInput{value->type, value->bytes.data()} : ValueInput{}); !executed)
        return std::unexpected(executed.error());
    if (auto violation = history_violation(*info, recorded, undo, redo, document.recorded(), document.undo_count(), document.redo_count());
        !violation.empty())
        return std::unexpected(invalid(std::move(violation)));
    return OperatorReport{OperatorStatus::finished, {}};
}
namespace {
using assets::json_format::Json;
Json scalar_json(Runtime& runtime, const Scalar& value) {
    return std::visit([&](const auto& leaf) -> Json {
        if constexpr (std::same_as<std::decay_t<decltype(leaf)>, EnumValue>) {
            if (const auto info = runtime.describe(leaf.type); info && (*info)->enumeration)
                for (const auto& [name, bits] : (*info)->enumeration->values) if (bits == leaf.bits) return name;
            return leaf.bits;
        } else return leaf;
    }, value);
}
// The fields of the record at `path` inside a live parameter value; with `defaults`, their current values.
Result<Json> record_schema(const Parameters& value, TypeId record, std::vector<FieldId>& path, bool defaults) {
    auto& runtime = value.runtime;
    const auto info = runtime.describe(record);
    if (!info) return std::unexpected(info.error());
    auto properties = Json::object();
    for (const auto& field : (*info)->fields) {
        const auto leaf = runtime.describe(field.value.type);
        if (!leaf) return std::unexpected(leaf.error());
        if (field.value.form != ValueForm::inline_value)
            return std::unexpected(invalid("Operator parameters cannot hold references yet: " + field.name));
        path.push_back(field.id);
        Json schema;
        switch ((*leaf)->kind) {
        case TypeKind::record: {
            auto nested = record_schema(value, field.value.type, path, defaults);
            if (!nested) return nested;
            schema = std::move(*nested);
            break;
        }
        case TypeKind::string: {
            schema = {{"type", "string"}};
            if (!defaults) break;
            String text;
            if (auto read = runtime.read(value.type, value.bytes, path, ValueOutput{field.value.type, &text}); !read) return std::unexpected(read.error());
            schema["default"] = std::string(text.c_str(), text.size());
            break;
        }
        case TypeKind::scalar: case TypeKind::enumeration: {
            const auto current = runtime.read(value.type, value.bytes, path);
            if (!current) return std::unexpected(current.error());
            if ((*leaf)->enumeration) {
                auto names = Json::array();
                for (const auto& member : (*leaf)->enumeration->values) names.push_back(member.first);
                schema = {{"enum", std::move(names)}};
            } else {
                const bool number = std::holds_alternative<float>(*current) || std::holds_alternative<double>(*current);
                schema = {{"type", std::holds_alternative<bool>(*current) ? "boolean" : number ? "number" : "integer"}};
            }
            const auto bound = [&](const char* key, const std::optional<Scalar>& limit) { if (limit) schema[key] = scalar_json(runtime, *limit); };
            bound("minimum", field.range.hard_min); bound("maximum", field.range.hard_max);
            bound("ui_min", field.range.ui_min); bound("ui_max", field.range.ui_max);
            if (defaults) schema["default"] = scalar_json(runtime, *current);
            break;
        }
        default: return std::unexpected(invalid("Operator parameters hold records, strings, scalars and enums only: " + field.name));
        }
        path.pop_back();
        if (!field.description.empty()) schema["description"] = field.description;
        properties[field.name] = std::move(schema);
    }
    return Json{{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
}
}
Result<assets::json_format::Json> parameter_schema(Runtime& runtime, const OperatorInfo& op) {
    if (op.parameters.empty()) return Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}};
    auto value = parameters(runtime, op);
    if (!value) return std::unexpected(value.error());
    std::vector<FieldId> path;
    auto schema = record_schema(**value, (*value)->type, path, !op.seed);
    if (!schema) return schema;
    (*schema)["title"] = op.parameters;
    if (op.seed) (*schema)["seeded"] = true;
    return schema;
}
}
