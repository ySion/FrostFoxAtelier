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
namespace detail {
// A live parameter or result record in owned, aligned storage.
class OperatorValue {
public:
    OperatorValue(Runtime& runtime, TypeId type, const TypeInfo& info)
        : runtime(runtime), type(type), alignment{info.alignment},
          bytes{static_cast<std::byte*>(::operator new(info.size, alignment)), info.size} {}

    OperatorValue(const OperatorValue&) = delete;

    OperatorValue& operator=(const OperatorValue&) = delete;

    ~OperatorValue() {
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
}

namespace {
Error invalid(std::string message) {
    return Error{ErrorCode::invalid_definition, std::move(message)};
}

// The caller sent an operator call this registry cannot accept.
Error rejected(std::string message) {
    return Error{ErrorCode::argument_mismatch, std::move(message)};
}

// Default-constructed record, resolved by name in this document's Runtime.
Result<std::unique_ptr<detail::OperatorValue>> parameters(Runtime& runtime, std::string_view name) {
    const auto type = runtime.find_type(name);
    if (!type)
        return std::unexpected(invalid("This document does not publish operator record " + std::string(name)));
    const auto info = runtime.describe(*type);
    if (!info)
        return std::unexpected(info.error());
    if ((*info)->kind != TypeKind::record)
        return std::unexpected(invalid("Operator parameters and results must be records: " + std::string(name)));
    auto value = std::make_unique<detail::OperatorValue>(runtime, *type, **info);
    ATELIER_TRY(runtime.initialize_value(*type, value->bytes));
    value->initialized = true;
    if ((*info)->contains_references) {
        ATELIER_TRY_BIND(root, runtime.root_value(*type, value->bytes));
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
    ATELIER_TRY_BIND(members, context.document.world().members());
    std::unordered_set<ObjectId> entities;
    std::unordered_set<ObjectId> seen;
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
    if (has_any(info.flags, OperatorFlag::replaces_document))
        return {};
    if (has_any(info.flags, OperatorFlag::undo)) {
        flag = "undo";
        valid = unchanged || (entries == 1 && redo_after == 0);
    } else if (has_any(info.flags, OperatorFlag::history)) {
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
    if (info.kind == OperatorKind::query)
        return std::unexpected(invalid("Queries use state, schema or inspection, not execute: " + info.id));
    if (info.requires_interaction && (info.kind != OperatorKind::direct || info.parameters.empty() ||
                                      info.parameters == "@context.object" || info.interaction.empty()))
        return std::unexpected(
            invalid("Conditional interaction needs a direct operator, record and guidance: " + info.id));
    if (!info.requires_interaction && !info.interaction.empty())
        return std::unexpected(invalid("Interaction guidance needs a parameter condition: " + info.id));
    const auto exclusive = int(has_any(info.flags, OperatorFlag::undo)) +
                           int(has_any(info.flags, OperatorFlag::history)) +
                           int(has_any(info.flags, OperatorFlag::replaces_document));
    if (exclusive > 1)
        return std::unexpected(invalid("Operator history flags are exclusive: " + info.id));
    operators_.push_back(std::move(info));
    return {};
}

const OperatorInfo* OperatorRegistry::find(std::string_view id) const noexcept {
    const auto found = std::ranges::find(operators_, id, &OperatorInfo::id);
    return found == operators_.end() ? nullptr : &*found;
}

OperatorBinding::OperatorBinding(OperatorCall call) : request(std::move(call)) {}

OperatorBinding::~OperatorBinding() = default;

Result<std::optional<bool>> OperatorBinding::state(const OperatorRegistry& registry, const Context& context) {
    const auto* info = registry.find(request.id);
    if (!info)
        return std::unexpected(rejected("Unknown operator: " + request.id));
    if (!info->state)
        return std::optional<bool>{};
    if (info->parameters.empty())
        return info->state(context, {});
    if (parameters_)
        return info->state(context, {parameters_->type, parameters_->bytes.data()});
    auto& runtime = context.document.world().runtime();
    ATELIER_TRY_BIND(value, parameters(runtime, info->parameters));
    if (info->seed)
        ATELIER_TRY(info->seed(context, {(*value)->type, (*value)->bytes.data()}));
    ATELIER_TRY(decode_properties(runtime, {(*value)->type, {}, (*value)->bytes}, request.args.root()));
    const auto selected = info->state(context, {(*value)->type, (*value)->bytes.data()});
    // A seeded record depends on current Context; a rooted record must not
    // prolong an object's life just because its menu remains open.
    if (!info->seed && !(*value)->root.registered())
        parameters_ = std::move(*value);
    return selected;
}

Result<OperatorReport> call(const OperatorRegistry& registry, OperatorHost& host, const Context& base,
                            const OperatorCall& request, const ResolveReference& resolve) {
    host.begin_operator();
    const auto* info = registry.find(request.id);
    if (!info)
        return std::unexpected(rejected("Unknown operator: " + request.id));
    const auto args = request.args.root();
    const auto members = args.object_items();
    if (!members)
        return std::unexpected(rejected("Operator arguments must be an object."));
    if (info->parameters.empty() && !members->empty())
        return std::unexpected(
            rejected("Unknown argument for " + request.id + ": " + std::string((*members)[0].first)));
    ATELIER_TRY(admit(base, request.context));
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
    std::unique_ptr<detail::OperatorValue> value;
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
            for (const auto& [name, _] : *members) {
                const auto field = runtime.find_field(*type, name);
                if (field && detail::transform_field(document.types(), *field))
                    return std::unexpected(rejected("Transform fields require scene.transform_set/clear/remove."));
            }
        const auto decoded = decode_properties(
            runtime, PropertyTarget{*type, object}, args, {},
            [&](std::span<const FieldId> path, const PropertyLeaf& leaf) -> Result<void> {
                const auto* scalar = std::get_if<Scalar>(&leaf);
                if (!scalar)
                    return std::unexpected(rejected("A property patch supports Scalar leaves only."));
                patch.push_back({{path.begin(), path.end()}, *scalar});
                return {};
            },
            true);
        if (!decoded)
            return std::unexpected(decoded.error());
    } else if (!info->parameters.empty()) {
        ATELIER_TRY_BIND(made, parameters(runtime, info->parameters));
        value = std::move(*made);
        if (info->seed)
            ATELIER_TRY(info->seed(context, ValueOutput{value->type, value->bytes.data()}));
        const ResolveReference reference = [&](const auto& json, TypeId type,
                                               ValueForm form) -> Result<std::optional<ObjectReference>> {
            if (json.is_null()) {
                auto null = runtime.null_reference(type, form);
                return null ? std::optional{*null} : std::nullopt;
            }
            if (resolve)
                return resolve(json, type, form);
            return std::nullopt;
        };
        ATELIER_TRY(decode_properties(runtime, PropertyTarget{value->type, {}, value->bytes}, args, reference));
    }
    const OperatorParameters decoded{value ? ValueInput{value->type, value->bytes.data()} : ValueInput{}, patch};
    if (!context.interactive && (info->kind == OperatorKind::interactive ||
                                 (info->requires_interaction && info->requires_interaction(decoded.value))))
        return OperatorReport{OperatorStatus::interaction_required,
                              "Operator " + info->id + " requires human interaction." +
                                  (info->interaction.empty() ? "" : " " + info->interaction)};
    std::unique_ptr<detail::OperatorValue> output;
    if (!info->result.empty()) {
        ATELIER_TRY_BIND(made, parameters(runtime, info->result));
        output = std::move(*made);
    }
    ATELIER_TRY(host.end_interaction());
    const auto recorded = document.recorded();
    const auto undo = document.undo_count();
    const auto redo = document.redo_count();
    ATELIER_TRY(info->execute(host, context, decoded,
                              output ? ValueOutput{output->type, output->bytes.data()} : ValueOutput{}));
    if (auto violation = history_violation(*info, recorded, undo, redo, document.recorded(), document.undo_count(),
                                           document.redo_count());
        !violation.empty())
        return std::unexpected(invalid(std::move(violation)));
    if (host.operator_cancelled())
        return OperatorReport{OperatorStatus::cancelled, {}};
    OperatorReport report{OperatorStatus::finished, {}};
    if (output) {
        ATELIER_TRY_BIND(fields, detail::author_fields(runtime, output->type));
        if (std::ranges::any_of(*fields, [](const auto& field) { return field.form != ValueForm::inline_value; }))
            return std::unexpected(invalid("Operator results cannot contain references."));
        ATELIER_TRY_BIND(encoded,
                         encode_properties(runtime, PropertyTarget{output->type, {}, output->bytes}, *fields, {}));
        report.result = std::move(*encoded);
    }
    return report;
}

namespace {
json::Result<void> scalar_json(Runtime& runtime, json::Builder& builder, json::Value object, std::string_view key,
                               const Scalar& value) {
    return std::visit(
        [&](const auto& leaf) -> json::Result<void> {
            if constexpr (std::same_as<std::decay_t<decltype(leaf)>, EnumValue>) {
                if (const auto info = runtime.describe(leaf.type); info && (*info)->enumeration)
                    for (const auto& [name, bits] : (*info)->enumeration->values)
                        if (bits == leaf.bits)
                            return builder.set(object, key, name);
                return builder.set(object, key, static_cast<std::uint64_t>(leaf.bits));
            } else if constexpr (std::is_integral_v<std::decay_t<decltype(leaf)>> &&
                                 !std::same_as<std::decay_t<decltype(leaf)>, bool>) {
                if constexpr (std::is_signed_v<std::decay_t<decltype(leaf)>>)
                    return builder.set(object, key, static_cast<std::int64_t>(leaf));
                else
                    return builder.set(object, key, static_cast<std::uint64_t>(leaf));
            } else
                return builder.set(object, key, leaf);
        },
        value);
}

// The fields of the record at `path` inside a live parameter value; with `defaults`, their current values.
Result<void> record_schema(const detail::OperatorValue& value, TypeId record, std::vector<FieldId>& path, bool defaults,
                           json::Builder& builder, json::Value target) {
    auto& runtime = value.runtime;
    const auto info = runtime.describe(record);
    if (!info)
        return std::unexpected(info.error());
    builder.set_validated(target, "type", "object");
    const auto additional = builder.set(target, "additionalProperties", false);
    if (!additional)
        return std::unexpected(invalid(additional.error().describe()));
    const auto properties = builder.object_validated(target, "properties");
    for (const auto& field : (*info)->fields) {
        const auto leaf = runtime.describe(field.value.type);
        if (!leaf)
            return std::unexpected(leaf.error());
        path.push_back(field.id);
        const auto made = builder.object(properties, field.name);
        if (!made)
            return std::unexpected(invalid(made.error().describe()));
        const auto schema = *made;
        const bool material_entries = field.value.type == runtime.native_type<MaterialEntriesJson>().value_or(TypeId{});
        String text;
        std::optional<Scalar> current;
        if (field.value.form == ValueForm::inline_value) {
            if ((*leaf)->kind == TypeKind::record && !material_entries) {
                ATELIER_TRY(record_schema(value, field.value.type, path, defaults, builder, schema));
            } else if ((*leaf)->kind == TypeKind::string && defaults) {
                ATELIER_TRY(runtime.read(value.type, value.bytes, path, ValueOutput{field.value.type, &text}));
            } else if ((*leaf)->kind == TypeKind::scalar || (*leaf)->kind == TypeKind::enumeration) {
                ATELIER_TRY_BIND(read, runtime.read(value.type, value.bytes, path));
                current = *read;
            } else if ((*leaf)->kind != TypeKind::record && (*leaf)->kind != TypeKind::string) {
                return std::unexpected(
                    invalid("Operator parameters hold records, strings, scalars and enums only: " + field.name));
            }
        }
        const auto emitted = [&]() -> json::Result<void> {
            if (!field.description.empty())
                ATELIER_TRY(builder.set(schema, "description", field.description));
            if (field.value.form != ValueForm::inline_value) {
                ATELIER_TRY_BIND(types, builder.array(schema, "type"));
                ATELIER_TRY(builder.append(*types, "string"));
                ATELIER_TRY(builder.append(*types, "null"));
                ATELIER_TRY(builder.set(schema, "reference", (*leaf)->name));
                ATELIER_TRY(builder.set(schema, "weak", field.value.form == ValueForm::weak_reference));
                if (defaults)
                    ATELIER_TRY(builder.set_null(schema, "default"));
                return {};
            }
            switch ((*leaf)->kind) {
            case TypeKind::record:
                if (material_entries) {
                    builder.set_validated(schema, "type", "array");
                    builder.set_validated(schema, "format", "material.entries");
                    ATELIER_TRY(builder.array(schema, "default"));
                }
                break;
            case TypeKind::string:
                builder.set_validated(schema, "type", "string");
                if (defaults)
                    builder.set_validated(schema, "default", std::string_view{text.c_str(), text.size()});
                break;
            case TypeKind::scalar:
            case TypeKind::enumeration: {
                if ((*leaf)->enumeration) {
                    ATELIER_TRY_BIND(names, builder.array(schema, "enum"));
                    for (const auto& member : (*leaf)->enumeration->values)
                        ATELIER_TRY(builder.append(*names, member.first));
                    if ((*leaf)->enumeration->flags)
                        ATELIER_TRY(builder.set(schema, "flags", true));
                } else {
                    const bool number =
                        std::holds_alternative<float>(*current) || std::holds_alternative<double>(*current);
                    builder.set_validated(schema, "type",
                                          std::holds_alternative<bool>(*current) ? "boolean"
                                          : number                               ? "number"
                                                                                 : "integer");
                }
                const auto bound = [&](const char* key, const std::optional<Scalar>& limit) -> json::Result<void> {
                    if (limit)
                        return scalar_json(runtime, builder, schema, key, *limit);
                    return {};
                };
                ATELIER_TRY(bound("minimum", field.range.hard_min));
                ATELIER_TRY(bound("maximum", field.range.hard_max));
                ATELIER_TRY(bound("ui_min", field.range.ui_min));
                ATELIER_TRY(bound("ui_max", field.range.ui_max));
                if (defaults)
                    ATELIER_TRY(scalar_json(runtime, builder, schema, "default", *current));
                break;
            }
            default:
                break; // Unsupported Runtime kinds were rejected before writing.
            }
            return {};
        }();
        if (!emitted)
            return std::unexpected(invalid(emitted.error().describe()));
        path.pop_back();
    }
    return {};
}

Result<json::Document> schema(Runtime& runtime, std::string_view record, bool defaults, bool seeded) {
    json::Builder builder;
    const auto root = builder.root();
    if (record.empty()) {
        builder.set_validated(root, "type", "object");
        const auto additional = builder.set(root, "additionalProperties", false);
        if (!additional)
            return std::unexpected(invalid(additional.error().describe()));
        (void)builder.object_validated(root, "properties");
        return builder.finish();
    }
    if (record == "@context.object") {
        builder.set_validated(root, "dynamic", "context.object");
        return builder.finish();
    }
    ATELIER_TRY_BIND(value, parameters(runtime, record));
    std::vector<FieldId> path;
    ATELIER_TRY(record_schema(**value, (*value)->type, path, defaults, builder, root));
    const auto title = builder.set(root, "title", record);
    if (!title)
        return std::unexpected(invalid(title.error().describe()));
    if (seeded) {
        const auto wrote = builder.set(root, "seeded", true);
        if (!wrote)
            return std::unexpected(invalid(wrote.error().describe()));
    }
    return builder.finish();
}
}

Result<json::Document> parameter_schema(Runtime& runtime, const OperatorInfo& op) {
    return schema(runtime, op.parameters, !op.seed, bool(op.seed));
}

Result<json::Document> result_schema(Runtime& runtime, const OperatorInfo& op) {
    return schema(runtime, op.result, false, false);
}
}
