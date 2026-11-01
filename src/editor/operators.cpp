#include "editor/operators.hpp"
#include "editor/properties.hpp"
#include "editor/project.hpp"
#include "assets/builtin_materials.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <new>
#include <unordered_set>

namespace atelier::editor {
namespace {
Error invalid(std::string message) {
    return Error{ErrorCode::invalid_definition, std::move(message)};
}
// The caller sent an operator call this registry cannot accept.
Error rejected(std::string message) {
    return Error{ErrorCode::argument_mismatch, std::move(message)};
}
// A live parameter or result record in owned, aligned storage.
class Parameters {
public:
    Parameters(Runtime& runtime, TypeId type, const TypeInfo& info)
        : runtime(runtime), type(type), alignment{info.alignment},
          bytes{static_cast<std::byte*>(::operator new(info.size, alignment)), info.size} {}
    Parameters(const Parameters&) = delete;
    Parameters& operator=(const Parameters&) = delete;
    ~Parameters() {
        root.reset();
        if (initialized)
            (void)runtime.destroy_value(type, bytes);
        ::operator delete(bytes.data(), alignment);
    }
    Runtime& runtime;
    const TypeId type;
    const std::align_val_t alignment;
    const std::span<std::byte> bytes;
    bool initialized{};
    ValueRoot root;
};
// Default-constructed record, resolved by name in this document's Runtime.
Result<std::unique_ptr<Parameters>> parameters(Runtime& runtime, std::string_view name) {
    const auto type = runtime.find_type(name);
    if (!type)
        return std::unexpected(invalid("This document does not publish operator record " + std::string(name)));
    const auto info = runtime.describe(*type);
    if (!info)
        return std::unexpected(info.error());
    if ((*info)->kind != TypeKind::record)
        return std::unexpected(invalid("Operator parameters and results must be records: " + std::string(name)));
    auto value = std::make_unique<Parameters>(runtime, *type, **info);
    if (auto ready = runtime.initialize_value(*type, value->bytes); !ready)
        return std::unexpected(ready.error());
    value->initialized = true;
    if ((*info)->contains_references) {
        auto root = runtime.root_value(*type, value->bytes);
        if (!root)
            return std::unexpected(root.error());
        value->root = std::move(*root);
    }
    return value;
}
bool valid_id(std::string_view id) {
    const auto dot = id.find('.');
    if (dot == std::string_view::npos || id.find('.', dot + 1) != std::string_view::npos)
        return false;
    const auto part = [](std::string_view text) {
        return !text.empty() && std::islower(static_cast<unsigned char>(text.front())) &&
               std::ranges::all_of(text, [](char c) {
                   return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) ||
                          c == '_';
               });
    };
    return part(id.substr(0, dot)) && part(id.substr(dot + 1));
}
Result<void> admit(const Context& context, const ContextOverride& changes) {
    if (changes.asset && !changes.asset->empty() && context.document.loaded_material(*changes.asset) == ObjectId{} &&
        !(context.project && context.project->entry(*changes.asset)) &&
        std::ranges::find(assets::mesh_assets(), *changes.asset, &assets::MeshAsset::key) ==
            assets::mesh_assets().end() &&
        std::ranges::find(assets::material_assets(), *changes.asset, &assets::MaterialAsset::key) ==
            assets::material_assets().end())
        return std::unexpected(rejected("Context asset is not in the document or its asset catalogue."));
    if (!changes.selection)
        return {};
    const auto& selection = *changes.selection;
    if (selection.entities.size() > Session::maximum_entities)
        return std::unexpected(rejected("The selection exceeds the Entity limit."));
    auto members = context.document.world().members();
    if (!members)
        return std::unexpected(members.error());
    std::unordered_set<ObjectId> entities, seen;
    for (const auto& member : *members)
        entities.insert(member.id());
    for (const auto entity : selection.entities) {
        if (!seen.insert(entity).second)
            return std::unexpected(rejected("The selection repeats an Entity."));
        if (!entities.contains(entity))
            return std::unexpected(rejected("The selection names an object that is not an Entity of this document."));
    }
    if (selection.active != ObjectId{} &&
        std::ranges::find(selection.entities, selection.active) == selection.entities.end())
        return std::unexpected(rejected("The active Entity must be selected."));
    if (selection.object != ObjectId{} && selection.object != selection.active) {
        auto component = world::inspect_component(context.document.world().runtime(), selection.object);
        if (!component || !component->valid || component->owner != selection.active)
            return std::unexpected(
                rejected("The inspected object must be the active Entity or one of its components."));
    }
    return {};
}
std::string history_violation(const OperatorInfo& info, std::uint64_t recorded, std::size_t undo, std::size_t redo,
                              std::uint64_t recorded_after, std::size_t undo_after, std::size_t redo_after) {
    const auto entries = recorded_after - recorded;
    const bool unchanged = entries == 0 && undo_after == undo && redo_after == redo;
    bool valid = unchanged;
    std::string_view flag = "none";
    if (has_flag(info.flags, OperatorFlag::replaces_document))
        return {};
    if (has_flag(info.flags, OperatorFlag::undo)) {
        flag = "undo";
        valid = unchanged || (entries == 1 && redo_after == 0);
    } else if (has_flag(info.flags, OperatorFlag::history)) {
        flag = "history";
        valid = entries == 0 && undo_after + redo_after == undo + redo &&
                (undo_after > undo ? undo_after - undo : undo - undo_after) <= 1;
    }
    if (valid)
        return {};
    return "Operator " + info.id + " (" + std::string(flag) +
           ") broke its history contract: " + std::to_string(entries) + " entries recorded, undo " +
           std::to_string(undo) + "->" + std::to_string(undo_after) + ", redo " + std::to_string(redo) + "->" +
           std::to_string(redo_after) + ".";
}
}
std::span<const std::string_view> region_names() noexcept {
    static constexpr std::array<std::string_view, 10> names{
        "none", "viewport", "scene_tree", "structure", "parameters", "assets", "code", "output", "world", "controls"};
    return names;
}
Result<void> OperatorRegistry::add(OperatorInfo info) {
    if (!valid_id(info.id))
        return std::unexpected(invalid("Operator ID must be lowercase <domain>.<action>: " + info.id));
    if (find(info.id))
        return std::unexpected(invalid("Operator is already registered: " + info.id));
    if (!info.poll || !info.execute)
        return std::unexpected(invalid("Operator needs poll and execute: " + info.id));
    if (info.seed && info.parameters.empty())
        return std::unexpected(invalid("Only an operator with parameters can seed them: " + info.id));
    const auto exclusive = int(has_flag(info.flags, OperatorFlag::undo)) +
                           int(has_flag(info.flags, OperatorFlag::history)) +
                           int(has_flag(info.flags, OperatorFlag::replaces_document));
    if (exclusive > 1)
        return std::unexpected(invalid("Operator history flags are exclusive: " + info.id));
    operators_.push_back(std::move(info));
    return {};
}
const OperatorInfo* OperatorRegistry::find(std::string_view id) const noexcept {
    const auto found = std::ranges::find(operators_, id, &OperatorInfo::id);
    return found == operators_.end() ? nullptr : &*found;
}
Result<std::optional<bool>> operator_state(const OperatorInfo& info, const Context& context,
                                           const assets::json_format::Json& args) {
    if (!info.state)
        return std::optional<bool>{};
    if (info.parameters.empty())
        return info.state(context, {});
    auto& runtime = context.document.world().runtime();
    auto value = parameters(runtime, info.parameters);
    if (!value)
        return std::unexpected(value.error());
    if (info.seed)
        if (auto seeded = info.seed(context, {(*value)->type, (*value)->bytes.data()}); !seeded)
            return std::unexpected(seeded.error());
    if (auto decoded = decode_properties(runtime, {(*value)->type, {}, (*value)->bytes}, args, {}); !decoded)
        return std::unexpected(decoded.error());
    return info.state(context, {(*value)->type, (*value)->bytes.data()});
}
Result<OperatorReport> call(const OperatorRegistry& registry, OperatorHost& host, const Context& base,
                            const OperatorCall& request, const ResolveReference& resolve) {
    host.begin_operator();
    const auto* info = registry.find(request.id);
    if (!info)
        return std::unexpected(rejected("Unknown operator: " + request.id));
    if (!request.args.is_object())
        return std::unexpected(rejected("Operator arguments must be an object."));
    if (info->parameters.empty() && !request.args.empty())
        return std::unexpected(rejected("Unknown argument for " + request.id + ": " + request.args.begin().key()));
    if (auto admitted = admit(base, request.context); !admitted)
        return std::unexpected(admitted.error());
    Context context = base;
    if (request.context.selection)
        context.selection = *request.context.selection;
    if (request.context.region)
        context.region = *request.context.region;
    if (request.context.asset)
        context.asset = *request.context.asset;
    if (auto available = info->poll(context); !available)
        return OperatorReport{OperatorStatus::failed, available.error().message};
    // A replacing operator only files a host request, so the Session stays valid here.
    auto& document = context.document;
    auto& runtime = document.world().runtime();
    std::unique_ptr<Parameters> value;
    std::vector<PropertyPatch> patch;
    if (info->parameters == "@context.object") {
        const auto object = context.selection.object;
        const auto type = runtime.type_of(object);
        if (!type)
            return std::unexpected(type.error());
        const auto entity = runtime.is_a(*type, document.types().entity);
        if (!entity)
            return std::unexpected(entity.error());
        // Reject even empty Transform records before ending the host's interaction.
        // Inherited field identity, not its spelling, distinguishes Transform from business fields.
        if (*entity)
            for (const auto& [name, _] : request.args.items()) {
                const auto field = runtime.find_field(*type, name);
                if (field && (*field == document.types().position_fields.front().front() ||
                              *field == document.types().rotation_fields.front().front() ||
                              *field == document.types().scale_fields.front().front()))
                    return std::unexpected(rejected("Transform fields require scene.transform_set/clear/remove."));
            }
        const auto decoded =
            decode_properties(runtime, PropertyTarget{*type, object}, request.args, {},
                              [&](std::span<const FieldId> path, const PropertyLeaf& leaf) -> Result<void> {
                                  const auto* scalar = std::get_if<Scalar>(&leaf);
                                  if (!scalar)
                                      return std::unexpected(rejected("A property patch supports Scalar leaves only."));
                                  patch.push_back({{path.begin(), path.end()}, *scalar});
                                  return {};
                              });
        if (!decoded)
            return std::unexpected(decoded.error());
    } else if (!info->parameters.empty()) {
        auto made = parameters(runtime, info->parameters);
        if (!made)
            return std::unexpected(made.error());
        value = std::move(*made);
        if (info->seed)
            if (auto seeded = info->seed(context, ValueOutput{value->type, value->bytes.data()}); !seeded)
                return std::unexpected(seeded.error());
        const ResolveReference reference = [&](const auto& json, TypeId type,
                                               ValueForm form) -> std::optional<ObjectReference> {
            if (json.is_null()) {
                auto null = runtime.null_reference(type, form);
                return null ? std::optional{*null} : std::nullopt;
            }
            return resolve ? resolve(json, type, form) : std::nullopt;
        };
        if (auto decoded =
                decode_properties(runtime, PropertyTarget{value->type, {}, value->bytes}, request.args, reference);
            !decoded)
            return std::unexpected(decoded.error());
    }
    std::unique_ptr<Parameters> output;
    if (!info->result.empty()) {
        auto made = parameters(runtime, info->result);
        if (!made)
            return std::unexpected(made.error());
        output = std::move(*made);
    }
    if (auto ended = host.end_interaction(); !ended)
        return std::unexpected(ended.error());
    const auto recorded = document.recorded(), undo = document.undo_count(), redo = document.redo_count();
    if (auto executed =
            info->execute(host, context, {value ? ValueInput{value->type, value->bytes.data()} : ValueInput{}, patch},
                          output ? ValueOutput{output->type, output->bytes.data()} : ValueOutput{});
        !executed)
        return std::unexpected(executed.error());
    if (auto violation = history_violation(*info, recorded, undo, redo, document.recorded(), document.undo_count(),
                                           document.redo_count());
        !violation.empty())
        return std::unexpected(invalid(std::move(violation)));
    if (host.operator_cancelled())
        return OperatorReport{OperatorStatus::cancelled, {}};
    OperatorReport report{OperatorStatus::finished, {}};
    if (output) {
        auto fields = detail::author_fields(runtime, output->type);
        if (!fields)
            return std::unexpected(fields.error());
        if (std::ranges::any_of(*fields, [](const auto& field) { return field.form != ValueForm::inline_value; }))
            return std::unexpected(invalid("Operator results cannot contain references."));
        auto encoded = encode_properties(runtime, PropertyTarget{output->type, {}, output->bytes}, *fields, {});
        if (!encoded)
            return std::unexpected(encoded.error());
        report.result = std::move(*encoded);
    }
    return report;
}
namespace {
using assets::json_format::Json;
Json scalar_json(Runtime& runtime, const Scalar& value) {
    return std::visit(
        [&](const auto& leaf) -> Json {
            if constexpr (std::same_as<std::decay_t<decltype(leaf)>, EnumValue>) {
                if (const auto info = runtime.describe(leaf.type); info && (*info)->enumeration)
                    for (const auto& [name, bits] : (*info)->enumeration->values)
                        if (bits == leaf.bits)
                            return name;
                return leaf.bits;
            } else
                return leaf;
        },
        value);
}
// The fields of the record at `path` inside a live parameter value; with `defaults`, their current values.
Result<Json> record_schema(const Parameters& value, TypeId record, std::vector<FieldId>& path, bool defaults) {
    auto& runtime = value.runtime;
    const auto info = runtime.describe(record);
    if (!info)
        return std::unexpected(info.error());
    auto properties = Json::object();
    for (const auto& field : (*info)->fields) {
        const auto leaf = runtime.describe(field.value.type);
        if (!leaf)
            return std::unexpected(leaf.error());
        path.push_back(field.id);
        Json schema;
        if (field.value.form != ValueForm::inline_value) {
            schema = {{"type", Json::array({"string", "null"})},
                      {"reference", (*leaf)->name},
                      {"weak", field.value.form == ValueForm::weak_reference}};
            if (defaults)
                schema["default"] = nullptr;
        } else
            switch ((*leaf)->kind) {
            case TypeKind::record: {
                if (field.value.type == runtime.native_type<MaterialEntriesJson>().value_or(TypeId{})) {
                    schema = {{"type", "array"}, {"format", "material.entries"}, {"default", Json::array()}};
                    break;
                }
                auto nested = record_schema(value, field.value.type, path, defaults);
                if (!nested)
                    return nested;
                schema = std::move(*nested);
                break;
            }
            case TypeKind::string: {
                schema = {{"type", "string"}};
                if (!defaults)
                    break;
                String text;
                if (auto read = runtime.read(value.type, value.bytes, path, ValueOutput{field.value.type, &text});
                    !read)
                    return std::unexpected(read.error());
                schema["default"] = std::string(text.c_str(), text.size());
                break;
            }
            case TypeKind::scalar:
            case TypeKind::enumeration: {
                const auto current = runtime.read(value.type, value.bytes, path);
                if (!current)
                    return std::unexpected(current.error());
                if ((*leaf)->enumeration) {
                    auto names = Json::array();
                    for (const auto& member : (*leaf)->enumeration->values)
                        names.push_back(member.first);
                    schema = {{"enum", std::move(names)}};
                } else {
                    const bool number =
                        std::holds_alternative<float>(*current) || std::holds_alternative<double>(*current);
                    schema = {{"type", std::holds_alternative<bool>(*current) ? "boolean"
                                       : number                               ? "number"
                                                                              : "integer"}};
                }
                const auto bound = [&](const char* key, const std::optional<Scalar>& limit) {
                    if (limit)
                        schema[key] = scalar_json(runtime, *limit);
                };
                bound("minimum", field.range.hard_min);
                bound("maximum", field.range.hard_max);
                bound("ui_min", field.range.ui_min);
                bound("ui_max", field.range.ui_max);
                if (defaults)
                    schema["default"] = scalar_json(runtime, *current);
                break;
            }
            default:
                return std::unexpected(
                    invalid("Operator parameters hold records, strings, scalars and enums only: " + field.name));
            }
        path.pop_back();
        if (!field.description.empty())
            schema["description"] = field.description;
        properties[field.name] = std::move(schema);
    }
    return Json{{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
}
}
Result<assets::json_format::Json> parameter_schema(Runtime& runtime, const OperatorInfo& op) {
    if (op.parameters.empty())
        return Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}};
    if (op.parameters == "@context.object")
        return Json{{"dynamic", "context.object"}};
    auto value = parameters(runtime, op.parameters);
    if (!value)
        return std::unexpected(value.error());
    std::vector<FieldId> path;
    auto schema = record_schema(**value, (*value)->type, path, !op.seed);
    if (!schema)
        return schema;
    (*schema)["title"] = op.parameters;
    if (op.seed)
        (*schema)["seeded"] = true;
    return schema;
}
Result<assets::json_format::Json> result_schema(Runtime& runtime, const OperatorInfo& op) {
    if (op.result.empty())
        return Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}};
    auto value = parameters(runtime, op.result);
    if (!value)
        return std::unexpected(value.error());
    std::vector<FieldId> path;
    auto schema = record_schema(**value, (*value)->type, path, false);
    if (schema)
        (*schema)["title"] = op.result;
    return schema;
}
}
