#include "editor/session.hpp"
#include "editor/error.hpp"
#include "editor/project.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "editor/fields.hpp"
#include "editor/properties.hpp"
#include "assets/material_format.hpp"
#include "assets/mesh_format.hpp"
#include "physics/physics.hpp"
#include "platform/utf.hpp"
#include "runtime/detail/value_storage.hpp"
#include <limits>
#include <charconv>
#include <utility>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <tuple>
#include <unordered_set>

namespace atelier::editor {
namespace {
using assets::json_format::overlay;

using assets::json_format::preview;

using detail::Failure;

template <class T>
Result<T> scene_result(json::Result<T> value) {
    if (!value)
        return std::unexpected(Error{ErrorCode::invalid_storage, value.error().describe()});
    if constexpr (std::is_void_v<T>)
        return {};
    else
        return std::move(*value);
}

template <class T>
T take(Result<T> value) {
    if (!value)
        throw Failure(std::move(value.error()));
    if constexpr (!std::is_void_v<T>)
        return std::move(*value);
}

void committed(Result<world::Commit> value) {
    auto result = take(std::move(value));
    if (result.notification_error)
        throw Failure(std::move(*result.notification_error));
}

json::Result<void> vector(json::Builder& builder, json::Value target, std::string_view key, math::Vector3 value) {
    ATELIER_TRY_BIND(array, builder.array(target, key));
    for (float component : {value.x, value.y, value.z})
        ATELIER_TRY(builder.append(*array, component));
    return {};
}

json::Result<math::Vector3> vector(json::Value value) {
    ATELIER_TRY_BIND(v, assets::json_format::vector<3>(value));
    return math::Vector3{(*v)[0], (*v)[1], (*v)[2]};
}

std::string type_name(Runtime& runtime, TypeId type) {
    return std::string(take(runtime.describe(type))->name);
}

auto failed(const std::exception& error) {
    return std::unexpected(detail::caught_error(error, ErrorCode::invalid_storage));
}

bool derives(Runtime& runtime, TypeId type, TypeId base) {
    return take(runtime.is_a(type, base));
}

bool physical_type(Runtime& runtime, TypeId type) {
    const auto body = runtime.native_type<world::PrimitiveComponent>();
    return body && derives(runtime, type, *body);
}

json::Result<void> encode_scalar(Runtime& runtime, json::Builder& builder, json::Value target, const Scalar& value) {
    return std::visit(
        [&](const auto& v) -> json::Result<void> {
            if constexpr (std::same_as<std::decay_t<decltype(v)>, EnumValue>) {
                const auto info = take(runtime.describe(v.type));
                for (const auto& [name, bits] : info->enumeration->values)
                    if (bits == v.bits)
                        return builder.set(target, std::string_view{name});
                throw std::runtime_error("Scene enum value has no declared name.");
            } else if constexpr (std::is_floating_point_v<std::decay_t<decltype(v)>>) {
                auto result = builder.set(target, v);
                if (!result && result.error().fault == json::Fault::range)
                    throw std::runtime_error("Scene property values must be finite.");
                return result;
            } else {
                if constexpr (std::same_as<std::decay_t<decltype(v)>, bool>)
                    builder.set(target, v);
                else if constexpr (std::is_signed_v<std::decay_t<decltype(v)>>)
                    builder.set(target, static_cast<std::int64_t>(v));
                else
                    builder.set(target, static_cast<std::uint64_t>(v));
                return {};
            }
        },
        value);
}

// A saved value that does not fit the current schema. Unlike other exceptions it
// does not reject the scene: the value is kept and the field keeps its default.
struct Mismatch : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Scalar decode_scalar(Runtime& runtime, const Scalar& prototype, json::Value value, bool* converted = nullptr) {
    return std::visit(
        [&](const auto& original) -> Scalar {
            using T = std::decay_t<decltype(original)>;
            if constexpr (std::same_as<T, bool>) {
                if (value.kind() != json::Kind::boolean)
                    throw Mismatch("Expected a boolean property.");
                return *value.boolean();
            } else if constexpr (std::same_as<T, EnumValue>) {
                if (value.kind() != json::Kind::string)
                    throw Mismatch("Expected a declared enum member name.");
                const auto info = take(runtime.describe(original.type));
                for (const auto& [name, bits] : info->enumeration->values)
                    if (*value.string() == name)
                        return EnumValue{original.type, bits};
                throw Mismatch("Unknown scene enum member.");
            } else if constexpr (std::is_integral_v<T>) {
                if (value.kind() == json::Kind::real) {
                    // Lossless only: an integral value written by an earlier float field.
                    const auto number = *value.number();
                    if (!std::isfinite(number) || std::trunc(number) != number ||
                        number < static_cast<double>(std::numeric_limits<T>::min()) ||
                        number > static_cast<double>(std::numeric_limits<T>::max()))
                        throw Mismatch("Expected an integer property.");
                    if (converted)
                        *converted = true;
                    return static_cast<T>(number);
                }
                if (value.kind() != json::Kind::integer)
                    throw Mismatch("Expected an integer property.");
                // Runtime scalar integers are 32-bit; every accepted value fits i64.
                const auto number = value.i64();
                if (!number || *number < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                    *number > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
                    throw Mismatch("Integer property is out of range.");
                return static_cast<T>(*number);
            } else {
                if (value.kind() != json::Kind::integer && value.kind() != json::Kind::real)
                    throw Mismatch("Expected a numeric property.");
                const auto number = static_cast<T>(*value.number());
                if (!std::isfinite(number))
                    throw Mismatch("Scene property values must be finite and representable.");
                return number;
            }
        },
        prototype);
}

// Removes the retained subtree at path, or a retained leaf on its prefix.
bool erase_path(json::Builder& builder, json::Value record, std::span<const std::string> path) {
    std::vector<json::Value> trail{record};
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto node = trail.back();
        const auto at = node.find(path[i]);
        if (!at || !*at)
            return false;
        if (i + 1 == path.size() || (**at).kind() != json::Kind::object) {
            builder.erase(node, path[i]);
            for (std::size_t up = trail.size() - 1; up > 0 && trail[up].size() == 0; --up)
                builder.erase(trail[up - 1], path[up - 1]);
            return true;
        }
        trail.push_back(**at);
    }
    return false;
}

bool reference_value(json::Value value) {
    return value.kind() == json::Kind::object && (*value.find("$ref") || *value.find("$external"));
}

template <class F>
auto each_reference(json::Value value, F&& visit) -> decltype(visit(value)) {
    if (reference_value(value)) {
        return visit(value);
    }
    if (value.kind() == json::Kind::object) {
        const auto members = *value.object_items();
        for (auto [key, item] : members)
            ATELIER_TRY(each_reference(item, visit));
    } else if (value.kind() == json::Kind::array) {
        const auto items = *value.array();
        for (auto item : items)
            ATELIER_TRY(each_reference(item, visit));
    }
    return {};
}

std::string dotted(std::span<const std::string> path) {
    std::string text;
    for (const auto& name : path) {
        if (!text.empty())
            text += '.';
        text += name;
    }
    return text;
}

std::string dump_retained(json::Value value) {
    if (value.size() == 0)
        return {};
    const auto bytes = json::write(value);
    return {bytes.data(), bytes.size()};
}

std::vector<detail::ScalarField> business_fields(Runtime& runtime, const world::Types& types,
                                                 const presentation::Types& shown, TypeId type) {
    static_cast<void>(shown);
    auto result = take(detail::author_fields(runtime, type));
    std::vector<FieldId> reserved;
    if (derives(runtime, type, types.entity))
        for (const auto key : {"position", "rotation", "scale"})
            reserved.push_back(take(runtime.find_field(types.entity, key)));
    std::erase_if(result,
                  [&](const auto& field) { return std::ranges::find(reserved, field.path.front()) != reserved.end(); });
    return result;
}

String encode_string(Runtime& runtime, const PropertyTarget& target, const detail::ScalarField& field) {
    String value;
    const ValueOutput output{field.type, &value};
    take(target.object != ObjectId{} ? runtime.read(target.object, field.path, output)
                                     : runtime.read(target.type, target.storage, field.path, output));
    return value;
}

// A whole Array leaf as a JSON array of its element leaves.
json::Result<void> encode_array(Runtime& runtime, ObjectId object, const detail::ScalarField& field,
                                json::Builder& builder, json::Value target) {
    const auto element = *take(runtime.describe(field.type))->element;
    const bool text = take(runtime.describe(element))->kind == TypeKind::string;
    auto path = field.path;
    path.emplace_back();
    for (std::size_t i = 0, count = take(runtime.element_count(object, field.path)); i < count; ++i) {
        path.back() = take(runtime.element(field.type, static_cast<std::uint32_t>(i)));
        const auto entry = builder.append_null(target);
        if (!text) {
            ATELIER_TRY(encode_scalar(runtime, builder, entry, take(runtime.read(object, path))));
            continue;
        }
        String value;
        take(runtime.read(object, path, ValueOutput{element, &value}));
        builder.set_validated(entry, value.view().std_view());
    }
    return {};
}

// Decodes a properties record leaf by leaf: the one path for scene loads, Play changes and operator
// parameters. Tolerant decodes write a clamped leaf and then keep the saved value; strict decodes
// (no report) return an error at the first rejected leaf and never clamp. Earlier accepted leaves remain written.
Result<void> read_record(Runtime& runtime, const PropertyTarget& target, std::span<const detail::ScalarField> schema,
                         json::Value record, const std::vector<std::string>& prefix, const ResolveReference& resolve,
                         const WriteLeaf& write, const PropertyReport* report) {
    const auto object = target.object != ObjectId{};
    const auto store = [&](std::span<const FieldId> path, const PropertyLeaf& leaf) -> Result<void> {
        if (write)
            return write(path, leaf);
        return std::visit(
            [&](const auto& value) -> Result<void> {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, Scalar>)
                    return object ? runtime.write(target.object, path, value)
                                  : runtime.write(target.type, target.storage, path, value);
                else {
                    ValueInput input;
                    if constexpr (std::same_as<T, String>)
                        input = {take(runtime.native_type<String>()), &value};
                    else
                        input = value.input();
                    return object ? runtime.write(target.object, path, input)
                                  : runtime.write(target.type, target.storage, path, input);
                }
            },
            leaf);
    };
    const auto issue = [&](std::vector<std::string> at, json::Value item, world::DiagnosticKind kind,
                           const std::string& problem, const std::string& kept) -> Result<void> {
        if (!report)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, dotted(std::span(at).subspan(prefix.size())) + ": " + problem});
        report->keep(std::move(at), item, kind, kept);
        return {};
    };
    const auto read = [&](auto&& self, json::Value value, const std::vector<std::string>& path) -> Result<void> {
        const auto members = value.object_items();
        for (const auto& [name, item] : *members) {
            auto next_path = path;
            next_path.emplace_back(name);
            auto at = prefix;
            at.insert(at.end(), next_path.begin(), next_path.end());
            const auto field = std::ranges::find_if(schema, [&](const auto& f) { return f.names == next_path; });
            if (field == schema.end()) {
                const bool nested = std::ranges::any_of(schema, [&](const auto& f) {
                    return f.names.size() > next_path.size() &&
                           std::equal(next_path.begin(), next_path.end(), f.names.begin());
                });
                if (!nested)
                    ATELIER_TRY(issue(std::move(at), item, world::DiagnosticKind::unknown_field, "Unknown field.",
                                      "Unknown field kept from the saved scene."));
                else if (item.kind() != json::Kind::object)
                    ATELIER_TRY(issue(std::move(at), item, world::DiagnosticKind::mismatch, "Expected a record.",
                                      "Expected a record. Defaults are used and the saved value is kept."));
                else
                    ATELIER_TRY(self(self, item, next_path));
                continue;
            }
            bool converted{};
            std::string problem;
            std::optional<Scalar> clamped;
            try {
                if (field->form == ValueForm::inline_value && field->kind == TypeKind::string) {
                    if (!field->material_entries_json && item.kind() != json::Kind::string)
                        throw Mismatch("Expected a string property.");
                    String stored;
                    if (field->material_entries_json) {
                        const auto bytes = json::write(item);
                        stored = String::from_utf8_validated({bytes.data(), bytes.size()});
                    } else
                        stored = String::from_utf8_validated(StringView{*item.string()});
                    if (auto written = store(field->path, stored); !written)
                        problem = written.error().message;
                } else if (field->form == ValueForm::inline_value && field->kind == TypeKind::array) {
                    // The whole Array is built aside and stored as one leaf, so a rejected write leaves the field as it was.
                    if (item.kind() != json::Kind::array)
                        throw Mismatch("Expected an array property.");
                    const auto leaf = take(ArrayLeaf::create(runtime, field->type));
                    const auto element = *take(runtime.describe(field->type))->element;
                    const bool text = take(runtime.describe(element))->kind == TypeKind::string;
                    take(runtime.mutate_sequence(field->type, leaf.storage(), SequenceMutation::resize, {}, {},
                                                 item.size()));
                    // Every element takes the field's hard bounds, which constrain() reads through any element selector.
                    auto bounded = field->path;
                    bounded.push_back(take(runtime.element(field->type, 0)));
                    bool clamped_elements{};
                    std::uint32_t index = 0;
                    const auto items = *item.array();
                    for (auto value : items) {
                        const std::array selector{take(runtime.element(field->type, index++))};
                        if (text) {
                            if (value.kind() != json::Kind::string) {
                                problem = "Expected an array of strings.";
                                break;
                            }
                            auto stored = String::from_utf8_validated(StringView{*value.string()});
                            take(runtime.write(field->type, leaf.storage(), selector, ValueInput{element, &stored}));
                            continue;
                        }
                        const auto scalar = decode_scalar(
                            runtime, take(runtime.read(field->type, leaf.storage(), selector)), value, &converted);
                        auto stored = runtime.constrain(target.type, bounded, scalar);
                        if (!stored)
                            throw Mismatch(stored.error().message);
                        if (*stored != scalar) {
                            if (!report)
                                throw Mismatch("An element is outside the field's hard bounds.");
                            clamped_elements = true;
                        }
                        take(runtime.write(field->type, leaf.storage(), selector, *stored));
                    }
                    // Reported before the store: a strict report (a Play change) rejects the clamp with the field unchanged.
                    // As for a scalar, the clamp is the one report, whatever elements converted.
                    if (problem.empty() && clamped_elements) {
                        auto reported = issue(
                            at, item, world::DiagnosticKind::mismatch, {},
                            "An element is outside the field's hard bounds; the clamped Array is used and the saved value is kept.");
                        if (!reported)
                            return std::unexpected(reported.error());
                        converted = false;
                    }
                    if (problem.empty()) {
                        if (auto written = store(field->path, leaf); !written)
                            throw Mismatch(written.error().message);
                    }
                } else if (field->form == ValueForm::inline_value) {
                    const auto current = object ? runtime.read(target.object, field->path)
                                                : runtime.read(target.type, target.storage, field->path);
                    const auto scalar = decode_scalar(runtime, take(current), item, &converted);
                    auto stored = runtime.constrain(target.type, field->path, scalar);
                    if (!stored)
                        throw Mismatch(stored.error().message);
                    if (*stored != scalar) {
                        if (!report)
                            throw Mismatch("Outside the field's hard bounds.");
                        clamped = *stored;
                    }
                    if (auto written = store(field->path, *stored); !written)
                        throw Mismatch(written.error().message);
                } else if (!resolve) {
                    throw Mismatch("This decode does not accept object references.");
                } else if (field->form == ValueForm::strong_sequence) {
                    if (item.kind() != json::Kind::array ||
                        item.size() > Session::maximum_entities * Session::maximum_components)
                        throw Mismatch("Expected an object reference sequence.");
                    std::vector<ObjectReference> refs;
                    refs.reserve(item.size());
                    const auto items = *item.array();
                    for (auto ref : items) {
                        auto resolved = resolve(ref, field->type, ValueForm::strong_reference);
                        if (!resolved)
                            return std::unexpected(Error{ErrorCode::invalid_storage, resolved.error().describe()});
                        if (!*resolved)
                            throw Mismatch("A sequence element is not a reference of the field's type.");
                        refs.push_back(std::move(**resolved));
                    }
                    const auto locator = take(runtime.reference_field(target.object, field->path));
                    const ReferenceChange change =
                        SequenceSplice{locator, 0, take(runtime.sequence_size(locator)), refs};
                    if (auto applied = runtime.apply_reference_changes({&change, 1}); !applied)
                        throw Mismatch(applied.error().message);
                } else {
                    auto resolved = resolve(item, field->type, field->form);
                    if (!resolved)
                        return std::unexpected(Error{ErrorCode::invalid_storage, resolved.error().describe()});
                    if (!*resolved)
                        throw Mismatch("Expected a reference of the field's type.");
                    auto written = object
                                       ? runtime.write_reference(target.object, field->path, **resolved)
                                       : runtime.write_reference(target.type, target.storage, field->path, **resolved);
                    if (!written)
                        throw Mismatch(written.error().message);
                }
            } catch (const Mismatch& error) {
                problem = error.what();
            }
            if (!problem.empty())
                ATELIER_TRY(issue(std::move(at), item, world::DiagnosticKind::mismatch, problem,
                                  problem + " The default is used and the saved value is kept."));
            else if (clamped) {
                json::Builder kept(json::Kind::null);
                const auto encoded = encode_scalar(runtime, kept, kept.root(), *clamped);
                if (!encoded)
                    return std::unexpected(Error{ErrorCode::argument_mismatch, encoded.error().describe()});
                const auto bytes = json::write(kept.root());
                ATELIER_TRY(issue(std::move(at), item, world::DiagnosticKind::mismatch, {},
                                  "Outside the field's hard bounds; " + std::string(bytes.data(), bytes.size()) +
                                      " is used and the saved value is kept."));
            } else if (converted && report)
                report->converted(std::move(at), item);
        }
        return {};
    };
    return read(read, record, {});
}

json::Result<std::uint64_t> local_key(json::Value value, bool allow_null = false) {
    ATELIER_TRY_BIND(input, value.string());
    const auto text = *input;
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || std::to_string(result) != text ||
        (!allow_null && result == 0))
        return std::unexpected(
            value.error(json::Fault::range, "Invalid document object identity: " + std::string(text)));
    return result;
}

json::Document ref_key(std::uint64_t id) {
    json::Builder result;
    result.set_validated(result.root(), "$ref", std::to_string(id));
    return result.finish();
}

json::Result<void> trs_json(json::Builder& builder, json::Value target, world::Trs value) {
    value = take(world::canonical_transform(value));
    ATELIER_TRY(vector(builder, target, "position", value.position));
    ATELIER_TRY_BIND(rotation, builder.array(target, "rotation"));
    for (float component : {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w})
        ATELIER_TRY(builder.append(*rotation, component));
    ATELIER_TRY(vector(builder, target, "scale", value.scale));
    return builder.set_null(target, "origin");
}

json::Result<world::Trs> json_trs(json::Value value) {
    ATELIER_TRY_BIND(rotation, value.at("rotation"));
    ATELIER_TRY_BIND(q, assets::json_format::vector<4>(*rotation));
    ATELIER_TRY_BIND(position, value.at("position"));
    ATELIER_TRY_BIND(p, vector(*position));
    ATELIER_TRY_BIND(scale, value.at("scale"));
    ATELIER_TRY_BIND(s, vector(*scale));
    auto canonical = world::canonical_transform({*p, {(*q)[0], (*q)[1], (*q)[2], (*q)[3]}, *s});
    if (!canonical)
        return std::unexpected(value.error(json::Fault::range, canonical.error().message));
    return *canonical;
}
}

struct ArrayLeaf::Value {
    Runtime& runtime;

    TypeId type;

    atelier::detail::ValueStorage bytes;

    std::span<std::byte> storage;

    bool live{};

    Value(Runtime& owner, TypeId array) : runtime(owner), type(array) {}

    Value(const Value&) = delete;

    Value& operator=(const Value&) = delete;

    ~Value() {
        if (live)
            static_cast<void>(runtime.destroy_value(type, storage));
    }
};

Result<ArrayLeaf> ArrayLeaf::create(Runtime& runtime, TypeId type) {
    const auto info = runtime.describe(type);
    if (!info)
        return std::unexpected(info.error());
    if ((*info)->kind != TypeKind::array)
        return std::unexpected(Error{ErrorCode::invalid_type, "An Array leaf requires an Array type."});
    ArrayLeaf result;
    result.value_ = std::make_shared<Value>(runtime, type);
    auto& value = *result.value_;
    value.storage = {value.bytes.prepare((*info)->size, (*info)->alignment), (*info)->size};
    ATELIER_TRY(runtime.initialize_value(type, value.storage));
    value.live = true;
    return result;
}

TypeId ArrayLeaf::type() const noexcept {
    return value_->type;
}

std::span<std::byte> ArrayLeaf::storage() const noexcept {
    return value_->storage;
}

ValueInput ArrayLeaf::input() const noexcept {
    return {value_->type, value_->storage.data()};
}

ValueOutput ArrayLeaf::output() const noexcept {
    return {value_->type, value_->storage.data()};
}

Result<json::Document> encode_properties(Runtime& runtime, ObjectId object, std::span<const detail::ScalarField> fields,
                                         const EncodeReference& reference) {
    const auto type = runtime.type_of(object);
    if (!type)
        return std::unexpected(type.error());
    return encode_properties(runtime, PropertyTarget{*type, object}, fields, reference);
}

Result<json::Document> encode_properties(Runtime& runtime, const PropertyTarget& target,
                                         std::span<const detail::ScalarField> fields,
                                         const EncodeReference& reference) {
    try {
        const auto object = target.object;
        if ((object == ObjectId{}) == target.storage.empty())
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "A properties target is either an object or a record value."});
        json::Builder result;
        for (const auto& field : fields) {
            if (object == ObjectId{} && (field.kind == TypeKind::array || field.form == ValueForm::strong_sequence))
                return std::unexpected(
                    Error{ErrorCode::unsupported_operation, "Indexed properties require an object target."});
            const auto parent =
                result.object_path_validated(result.root(), std::span(field.names).first(field.names.size() - 1));
            const auto& key = field.names.back();
            if (field.form == ValueForm::inline_value) {
                if (field.kind == TypeKind::string) {
                    const auto text = encode_string(runtime, target, field);
                    if (field.material_entries_json) {
                        const auto parsed =
                            json::Document::parse(text.view().std_view(), {std::numeric_limits<std::size_t>::max(),
                                                                           json::DepthPolicy::containers, true});
                        if (!parsed)
                            return std::unexpected(Error{ErrorCode::invalid_storage, parsed.error().describe()});
                        result.set_validated(parent, key, parsed->root());
                    } else
                        result.set_validated(parent, key, text.view().std_view());
                } else if (field.kind == TypeKind::array) {
                    const auto array = *result.array(parent, key);
                    const auto written = encode_array(runtime, object, field, result, array);
                    if (!written)
                        return std::unexpected(Error{ErrorCode::invalid_storage, written.error().describe()});
                } else {
                    const auto value = result.object_validated(parent, key);
                    const auto scalar =
                        take(object != ObjectId{} ? runtime.read(object, field.path)
                                                  : runtime.read(target.type, target.storage, field.path));
                    const auto written = encode_scalar(runtime, result, value, scalar);
                    if (!written)
                        return std::unexpected(Error{ErrorCode::invalid_storage, written.error().describe()});
                }
            } else if (field.form == ValueForm::strong_sequence) {
                const auto array = *result.array(parent, key);
                const auto locator = take(runtime.reference_field(object, field.path));
                const auto count = take(runtime.sequence_size(locator));
                for (std::size_t i = 0; i < count; ++i) {
                    ATELIER_TRY_BIND(value, reference(take(runtime.sequence_read(locator, i)).id()));
                    result.append(array, value->root());
                }
            } else {
                const auto ref =
                    take(object != ObjectId{} ? runtime.read_reference(object, field.path)
                                              : runtime.read_reference(target.type, target.storage, field.path));
                ATELIER_TRY_BIND(value, reference(ref.id()));
                result.set_validated(parent, key, value->root());
            }
        }
        return result.finish();
    } catch (const std::exception& error) {
        return failed(error);
    }
}

Result<void> decode_properties(Runtime& runtime, const PropertyTarget& target, json::Value record,
                               const ResolveReference& resolve, const WriteLeaf& write) {
    try {
        if ((target.object == ObjectId{}) == target.storage.empty())
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "A properties target is either an object or a record value."});
        if (write && resolve)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch,
                      "A write hook does not receive reference leaves; decode references without one."});
        if (target.object != ObjectId{} && take(runtime.type_of(target.object)) != target.type)
            return std::unexpected(
                Error{ErrorCode::type_mismatch, "A properties target object must have the target type."});
        if (record.kind() != json::Kind::object)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Expected a property record."});
        ATELIER_TRY_BIND(schema, detail::author_fields(runtime, target.type));
        return read_record(runtime, target, *schema, record, {}, resolve, write, nullptr);
    } catch (const std::exception& error) {
        return failed(error);
    }
}

Result<void> decode_properties(Runtime& runtime, const PropertyTarget& target,
                               std::span<const detail::ScalarField> fields, json::Value record,
                               const PropertyReport& report, const ResolveReference& resolve) {
    try {
        if ((target.object == ObjectId{}) == target.storage.empty())
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "A properties target is either an object or a record value."});
        if (target.object != ObjectId{} && take(runtime.type_of(target.object)) != target.type)
            return std::unexpected(
                Error{ErrorCode::type_mismatch, "A properties target object must have the target type."});
        if (record.kind() != json::Kind::object)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Expected a property record."});
        return read_record(runtime, target, fields, record, {}, resolve, {}, &report);
    } catch (const std::exception& error) {
        return failed(error);
    }
}

struct Session::GraphCodec {
    struct MeshCopy {
        presentation::StaticMeshState state;

        std::vector<presentation::MaterialBinding> bindings;
    };

    struct Graph {
        json::Document data;

        std::unordered_map<std::string, MeshCopy> meshes;

        // An in-memory copy keeps the sky map it resolved, not only the key.
        std::unordered_map<std::string, presentation::SkyMapBinding> skies;

        std::unordered_map<std::string, physics::BodySettings> bodies;

        std::unordered_map<std::string, ObjectRoot> materials;

        std::unordered_map<std::string, ObjectRoot> external;

        std::unordered_map<std::string, ObjectId> parents;
    };

    // One load's diagnostics and the saved data it could not apply.
    struct Load {
        LoadPolicy policy{LoadPolicy::tolerant};

        bool document{}; // Loading a whole document, not importing into one.

        // Applying an editor graph to a Play copy: references outside the graph
        // name live objects, and targets the game destroyed are dropped.
        bool live{};

        std::vector<std::uint64_t> dropped;

        std::vector<world::Diagnostic> issues;

        std::unordered_map<ObjectId, json::Builder> pending; // {"type","record","slots"} with file identities.

        std::unordered_map<ObjectId, ObjectId> owners;

        std::unordered_map<ObjectId, std::string> types;

        std::unordered_map<ObjectId, std::uint64_t> keys;

        json::Builder top{};

        std::vector<ObjectRoot> unreferenced;

        std::unordered_map<ObjectId, std::vector<ObjectRoot>> materials; // Named by a kept binding table.

        std::unordered_map<ObjectId, Retained> retained;

        std::string subject(ObjectId object) const {
            if (object == ObjectId{})
                return "Scene";
            std::string text = types.contains(object) ? types.at(object) : std::string("Object");
            if (const auto at = keys.find(object); at != keys.end())
                text += " #" + std::to_string(at->second);
            return text;
        }

        void report(world::Severity severity, world::DiagnosticKind kind, ObjectId object,
                    std::vector<std::string> field, const json::Value* value, const std::string& detail,
                    const Error* cause = nullptr) {
            auto message =
                cause ? cause->message
                      : subject(object) + (field.empty() ? std::string{} : " " + dotted(field)) + ": " + detail;
            if (policy == LoadPolicy::strict && severity != world::Severity::info) {
                if (cause)
                    throw detail::Failure(*cause);
                throw std::runtime_error(message);
            }
            world::Diagnostic item;
            item.serial = issues.size() + 1;
            item.severity = severity;
            item.kind = kind;
            item.phase = world::DiagnosticPhase::load;
            item.object = object;
            if (object != ObjectId{})
                item.entity = owners.contains(object) ? owners.at(object) : object;
            if (const auto at = types.find(object); at != types.end())
                item.type = at->second;
            item.field = std::move(field);
            if (value)
                item.value = preview(*value);
            item.message = std::move(message);
            if (cause) {
                item.error_code = cause->code;
                item.source = cause->source;
            }
            issues.push_back(std::move(item));
        }

        void store(ObjectId object, std::span<const std::string> path, json::Value value) {
            auto& builder = object == ObjectId{} ? top : pending[object];
            auto node = builder.root();
            if (object != ObjectId{}) {
                const auto record = *node.find("record");
                node = record ? *record : builder.object_validated(node, "record");
            }
            node = builder.object_path_validated(node, path.first(path.size() - 1));
            builder.set_validated(node, path.back(), value);
        }

        void keep(ObjectId object, std::vector<std::string> path, json::Value value, world::DiagnosticKind kind,
                  const std::string& detail) {
            report(world::Severity::warning, kind, object, path, &value, detail);
            store(object, path, value);
        }

        void keep_slot(ObjectId object, const std::string& slot, const std::string& key, json::Value value,
                       world::DiagnosticKind kind, const std::string& detail) {
            report(world::Severity::warning, kind, object, {"materials", slot, key}, &value, detail);
            auto& builder = pending[object];
            const std::array path{std::string("slots"), slot};
            builder.set_validated(builder.object_path_validated(builder.root(), path), key, value);
        }

        json::Result<void> extra(ObjectId object, json::Value value, std::initializer_list<std::string_view> allowed,
                                 std::vector<std::string> prefix = {}) {
            ATELIER_TRY_BIND(members, value.object_items());
            for (const auto& [name, item] : *members)
                if (std::ranges::find(allowed, name) == allowed.end()) {
                    auto path = prefix;
                    path.emplace_back(name);
                    keep(object, std::move(path), item, world::DiagnosticKind::unknown_key,
                         "Unknown key kept from the saved scene.");
                }
            return {};
        }
    };

    struct Prepared {
        Session* session{};

        std::vector<ObjectRoot> objects;

        std::vector<ObjectId> roots;

        std::vector<world::SpaceReference> spaces;

        std::vector<ObjectId> triangle_meshes;

        std::vector<ObjectRoot> resources;

        // Live graphs only: new components of an existing Entity, attached on publication.
        std::vector<std::pair<ObjectId, std::vector<world::Attachment>>> anchored;

        Project* project{};

        std::unordered_map<ObjectId, std::wstring> labels;

        std::unordered_set<std::string> material_keys;

        std::unordered_set<std::string> texture_keys;

        std::unordered_map<ObjectId, std::uint64_t> identities_before;

        std::unordered_map<ObjectId, std::string> material_parents_before;

        UnavailableResources unavailable_before;

        std::uint64_t next_before{};

        bool published{};

        explicit Prepared(Session& owner)
            : session(&owner), identities_before(owner.local_ids_),
              material_parents_before(owner.scene_material_parents_), unavailable_before(owner.unavailable_),
              next_before(owner.next_local_id_) {
            for (const auto& [key, value] : owner.materials_) {
                static_cast<void>(value);
                material_keys.insert(key);
            }
            for (const auto& [key, value] : owner.textures_) {
                static_cast<void>(value);
                texture_keys.insert(key);
            }
        }

        Prepared(const Prepared&) = delete;

        Prepared& operator=(const Prepared&) = delete;

        Prepared(Prepared&& other) noexcept
            : session(std::exchange(other.session, nullptr)), objects(std::move(other.objects)),
              roots(std::move(other.roots)), spaces(std::move(other.spaces)),
              triangle_meshes(std::move(other.triangle_meshes)), resources(std::move(other.resources)),
              anchored(std::move(other.anchored)), project(other.project), labels(std::move(other.labels)),
              material_keys(std::move(other.material_keys)), texture_keys(std::move(other.texture_keys)),
              identities_before(std::move(other.identities_before)),
              material_parents_before(std::move(other.material_parents_before)),
              unavailable_before(std::move(other.unavailable_before)), next_before(other.next_before),
              published(other.published) {}

        ~Prepared() {
            if (!session || published)
                return;
            for (const auto& object : objects) {
                session->names_.erase(object.id());
                session->local_ids_.erase(object.id());
                auto type = session->runtime_.type_of(object.id());
                if (!type)
                    continue;
                auto entity = session->runtime_.is_a(*type, session->types_.entity);
                if (entity && *entity)
                    (void)world::destroy_entity(session->runtime_, object.id());
            }
            for (const auto& object : objects) {
                if (session->runtime_.type_of(object.id()))
                    (void)world::destroy_component(session->runtime_, object.id());
            }
            std::erase_if(session->materials_, [&](const auto& entry) { return !material_keys.contains(entry.first); });
            std::erase_if(session->textures_, [&](const auto& entry) { return !texture_keys.contains(entry.first); });
            session->scene_material_parents_.swap(material_parents_before);
            session->unavailable_.swap(unavailable_before);
            // Unpublished objects never acquired a document identity visible to
            // history or authoring. Failed construction must leave metadata intact.
            session->next_local_id_ = next_before;
            session->local_ids_.swap(identities_before);
        }
    };

    static json::Result<std::uint64_t> identity(const Session& session, ObjectId object) {
        if (auto at = session.local_ids_.find(object); at != session.local_ids_.end())
            return at->second;
        // The editor allocates every identity; Play objects without one use runtime handles.
        if (session.play_)
            return std::unexpected(
                json::Error{json::Fault::range, "A Play copy does not assign document identities.", {}, {}});
        if (session.next_local_id_ == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(
                json::Error{json::Fault::range, "Document object identity space is exhausted.", {}, {}});
        const auto id = session.next_local_id_++;
        session.local_ids_.emplace(object, id);
        return id;
    }

    static Result<Graph> capture(const Session& session, std::span<const ObjectId> requested, bool persistent,
                                 bool document = false, bool live = false) {
        struct IdentityRollback {
            const Session& session;

            std::unordered_map<ObjectId, std::uint64_t> before;

            std::uint64_t next;

            bool complete{};

            ~IdentityRollback() {
                if (!complete) {
                    session.local_ids_.swap(before);
                    session.next_local_id_ = next;
                }
            }
        } identities{session, session.local_ids_, session.next_local_id_};
        auto& runtime = session.world_.runtime();
        Graph graph;
        std::vector<ObjectId> roots;
        std::vector<ObjectId> nodes;
        std::unordered_set<ObjectId> members;
        for (const auto& member : take(session.world_.members()))
            members.insert(member.id());
        std::erase_if(session.local_ids_, [&](const auto& entry) { return !runtime.type_of(entry.first); });
        std::unordered_set<ObjectId> selected(requested.begin(), requested.end());
        std::unordered_set<ObjectId> seen;
        std::unordered_set<ObjectId> root_seen;
        root_seen.reserve(requested.size());
        for (const auto object : requested) {
            const auto state = take(world::inspect_entity(runtime, object));
            if ((!persistent && !state.can_edit) || !session.names_.contains(object) || !members.contains(object))
                throw std::runtime_error("The Entity is not editable in this document.");
            bool covered{};
            for (auto parent = take(world::parent_entity(runtime, object)); parent != ObjectId{};
                 parent = take(world::parent_entity(runtime, parent)))
                if (selected.contains(parent)) {
                    covered = true;
                    break;
                }
            if (!covered && root_seen.insert(object).second)
                roots.push_back(object);
        }
        const auto walk = [&](auto&& self, ObjectId object) -> void {
            if (!seen.insert(object).second)
                throw std::runtime_error("Entity graph contains a repeated identity.");
            nodes.push_back(object);
            if (nodes.size() > maximum_entities)
                throw std::runtime_error("Scene exceeds the Entity limit.");
            for (const auto& child : take(world::children(runtime, object)))
                self(self, child.id());
        };
        for (auto root : roots)
            walk(walk, root);
        std::unordered_map<ObjectId, std::uint64_t> ids;
        for (auto object : nodes) {
            ATELIER_TRY_BIND(key, scene_result(identity(session, object)));
            ids.emplace(object, *key);
            const auto parts = take(world::components(runtime, object));
            if (parts.size() > maximum_components)
                throw std::runtime_error("Scene exceeds the Component limit.");
            for (const auto& part : parts) {
                ATELIER_TRY_BIND(part_key, scene_result(identity(session, part.id())));
                if (!ids.emplace(part.id(), *part_key).second)
                    throw std::runtime_error("Component identity appears twice.");
            }
        }
        json::Builder builder;
        const auto root = builder.root();
        const auto material_values = *builder.array(root, "materials");
        const auto reference = [&](ObjectId object) -> Result<json::Document> {
            if (object == ObjectId{})
                return json::Builder(json::Kind::null).finish();
            take(runtime.type_of(object));
            if (auto at = ids.find(object); at != ids.end())
                return ref_key(at->second);
            if (live) {
                ATELIER_TRY_BIND(key, scene_result(identity(session, object)));
                return ref_key(*key);
            }
            if (persistent)
                throw std::runtime_error(
                    "Object reference leaves the captured graph; supply an explicit binding or include its target.");
            ATELIER_TRY_BIND(id, scene_result(identity(session, object)));
            const auto key = std::to_string(*id);
            if (!graph.external.contains(key))
                graph.external.emplace(key, take(runtime.hold(object)));
            json::Builder external;
            external.set_validated(external.root(), "$external", key);
            return external.finish();
        };
        // Retained data refers to document identities. A target that left the
        // document is written as null; a live target follows the ordinary rules.
        std::unordered_map<std::uint64_t, ObjectId> by_key;
        if (!session.retained_.empty() || !session.retained_document_.record.empty())
            for (const auto& [object, key] : session.local_ids_)
                by_key.emplace(key, object);
        const auto present = [&](ObjectId object) {
            if (!runtime.type_of(object))
                return false;
            if (members.contains(object) || session.is_material(object))
                return true;
            const auto component = world::inspect_component(runtime, object);
            return component && component->valid && members.contains(component->owner);
        };
        std::function<Result<std::uint64_t>(ObjectId)> archive_material;
        const auto restore_references = [&](std::string_view text) -> Result<json::Builder> {
            ATELIER_TRY_BIND(parsed, scene_result(json::Document::parse(text, {std::numeric_limits<std::size_t>::max(),
                                                                               json::DepthPolicy::containers, true})));
            json::Builder value(*parsed);
            const auto restored = each_reference(value.root(), [&](json::Value node) -> Result<void> {
                ObjectId object;
                if (const auto at = *node.find("$ref")) {
                    ATELIER_TRY_BIND(key, scene_result(local_key(*at)));
                    if (const auto found = by_key.find(*key); found != by_key.end())
                        object = found->second;
                }
                if (object == ObjectId{} || !present(object)) {
                    const json::Builder null(json::Kind::null);
                    value.replace(node, null.root());
                    return {};
                }
                if (session.is_material(object)) {
                    ATELIER_TRY(archive_material(object));
                }
                ATELIER_TRY_BIND(saved, reference(object));
                value.replace(node, saved->root());
                return {};
            });
            ATELIER_TRY(restored);
            return value;
        };
        const auto merge_retained = [&](json::Value value, ObjectId object) -> Result<void> {
            const auto at = session.retained_.find(object);
            if (at == session.retained_.end())
                return {};
            const auto& kept = at->second;
            for (const auto& material : kept.materials)
                if (runtime.type_of(material.id())) {
                    ATELIER_TRY(archive_material(material.id()));
                }
            if (!kept.type.empty())
                builder.set_validated(value, "type", kept.type);
            if (!kept.record.empty()) {
                ATELIER_TRY_BIND(record, restore_references(kept.record));
                overlay(builder, value, record->root());
            }
            if (const auto entries = *value.find("materials");
                !kept.slots.empty() && entries && entries->kind() == json::Kind::array) {
                ATELIER_TRY_BIND(slots, restore_references(kept.slots));
                const auto items = *entries->array();
                for (auto entry : items) {
                    ATELIER_TRY_BIND(slot, scene_result(entry.string({"slot"})));
                    ATELIER_TRY_BIND(extra, scene_result(slots->root().find(*slot)));
                    if (*extra)
                        overlay(builder, entry, **extra);
                }
            }
            return {};
        };
        archive_material = [&](ObjectId object) -> Result<std::uint64_t> {
            if (auto at = ids.find(object); at != ids.end())
                return at->second;
            const auto source = take(material::material_key(runtime, object));
            ATELIER_TRY_BIND(key, scene_result(identity(session, object)));
            ids.emplace(object, *key);
            graph.materials.emplace(std::to_string(*key), take(runtime.hold(object)));
            if (source.empty()) {
                const auto state = take(material::read_instance(runtime, object));
                const auto parent = session.scene_material_parents_.find(object);
                if (parent == session.scene_material_parents_.end())
                    throw std::runtime_error("Scene instance has no author parent identity.");
                if (parent->second.empty() && state.parent.registered()) {
                    ATELIER_TRY(archive_material(state.parent.id()));
                }
            }
            const auto value = builder.append_object(material_values);
            ATELIER_TRY(scene_result(material_definition(session, object, *key, builder, value)));
            ATELIER_TRY(merge_retained(value, object));
            return *key;
        };
        const auto encoded = [&]() -> Result<void> {
            for (auto object : nodes)
                for (const auto& part : take(world::components(runtime, object)))
                    if (derives(runtime, take(runtime.type_of(part.id())),
                                session.presentation_types_.static_mesh_component))
                        for (const auto& binding : take(presentation::read_material_bindings(runtime, part.id()))) {
                            ATELIER_TRY(archive_material(binding.resource.id()));
                        }
            if (document)
                for (const auto& material : session.retained_document_.materials)
                    if (runtime.type_of(material.id())) {
                        ATELIER_TRY(archive_material(material.id()));
                    }
            const auto properties = [&](ObjectId object, TypeId type) {
                return encode_properties(runtime, object,
                                         business_fields(runtime, session.types_, session.presentation_types_, type),
                                         reference);
            };
            const auto values = *builder.array(root, "entities");
            const auto root_keys = *builder.array(root, "roots");
            for (auto object : roots) {
                const auto key = std::to_string(ids.at(object));
                ATELIER_TRY(scene_result(builder.append(root_keys, key)));
                graph.parents.emplace(key, take(world::parent_entity(runtime, object)));
            }
            for (auto object : nodes) {
                const auto status = take(world::inspect_entity(runtime, object));
                const auto entity = builder.append_object(values);
                if (take(world::has_transform(runtime, object))) {
                    const auto space = builder.object_validated(entity, "space");
                    ATELIER_TRY(scene_result(trs_json(builder, space, take(world::read_transform(runtime, object)))));
                    const auto origin = take(world::space_reference(runtime, object));
                    if (origin) {
                        if (*origin == ObjectId{})
                            builder.set_validated(space, "origin", "0");
                        else {
                            ATELIER_TRY_BIND(ref, reference(*origin));
                            const auto local = *ref->root().find("$ref");
                            builder.set_validated(space, "origin", local ? *local : ref->root());
                        }
                    }
                } else {
                    ATELIER_TRY(scene_result(builder.set_null(entity, "space")));
                }
                const auto children = *builder.array(entity, "children");
                const auto parts = *builder.array(entity, "components");
                for (const auto& child : take(world::children(runtime, object))) {
                    ATELIER_TRY(scene_result(builder.append(children, std::to_string(ids.at(child.id())))));
                }
                for (const auto& part : take(world::components(runtime, object))) {
                    const auto state = take(world::inspect_component(runtime, part.id()));
                    const auto key = std::to_string(ids.at(part.id()));
                    if (derives(runtime, state.type, session.presentation_types_.light_component))
                        take(presentation::validate_light(runtime, take(presentation::read_light(runtime, part.id()))));
                    if (physical_type(runtime, state.type)) {
                        auto body = take(physics::read_settings(runtime, part.id()));
                        take(world::validate_body_settings(runtime, body));
                        graph.bodies.emplace(key, std::move(body));
                    }
                    auto type = type_name(runtime, state.type);
                    auto name = platform::utf16_to_utf8(session.name(part.id()));
                    if (!name)
                        return scene_result(json::Result<void>{
                            std::unexpected(root.error(json::Fault::encoding, "Invalid UTF-16 text."))});
                    const auto value = builder.append_object(parts);
                    builder.set_validated(value, "id", key);
                    ATELIER_TRY(scene_result(builder.set(value, "type", type)));
                    builder.set_validated(value, "name", *name);
                    ATELIER_TRY(scene_result(builder.set(value, "enabled", state.enabled)));
                    ATELIER_TRY_BIND(record, properties(part.id(), state.type));
                    builder.set_validated(value, "properties", record->root());
                    if (derives(runtime, state.type, session.presentation_types_.static_mesh_component)) {
                        auto mesh = take(presentation::read_static_mesh(runtime, part.id()));
                        if (persistent && mesh.mesh.resolved && mesh.mesh.source.empty())
                            throw std::runtime_error(
                                "A procedural mesh has no persistent source; save/import it before archiving.");
                        auto bindings = take(presentation::read_material_bindings(runtime, part.id()));
                        graph.meshes.emplace(key, MeshCopy{mesh, bindings});
                        ATELIER_TRY(scene_result(builder.set(value, "mesh", mesh.mesh.source)));
                        const auto entries = *builder.array(value, "materials");
                        for (const auto& binding : bindings) {
                            const auto entry = builder.append_object(entries);
                            ATELIER_TRY(scene_result(builder.set(entry, "slot", binding.slot)));
                            builder.set_validated(entry, "resource", std::to_string(ids.at(binding.resource.id())));
                            ATELIER_TRY(scene_result(builder.set(entry, "overridden", binding.overridden)));
                        }
                    }
                    if (derives(runtime, state.type, session.presentation_types_.environment_component)) {
                        auto sky = take(presentation::read_sky_map(runtime, part.id()));
                        ATELIER_TRY(scene_result(builder.set(value, "sky_map", sky.source)));
                        graph.skies.emplace(key, std::move(sky));
                    }
                    ATELIER_TRY(merge_retained(value, part.id()));
                }
                const auto label = session.name(object);
                std::string name;
                if (label.empty())
                    name = type_name(runtime, status.type) + " " + std::to_string(ids.at(object));
                else {
                    auto converted = platform::utf16_to_utf8(label);
                    if (!converted)
                        return scene_result(json::Result<void>{
                            std::unexpected(root.error(json::Fault::encoding, "Invalid UTF-16 text."))});
                    name = std::move(*converted);
                }
                builder.set_validated(entity, "id", std::to_string(ids.at(object)));
                ATELIER_TRY(scene_result(builder.set(entity, "type", type_name(runtime, status.type))));
                ATELIER_TRY(scene_result(builder.set(entity, "name", name)));
                ATELIER_TRY(scene_result(builder.set(entity, "enabled", status.enabled)));
                ATELIER_TRY_BIND(record, properties(object, status.type));
                builder.set_validated(entity, "properties", record->root());
                ATELIER_TRY(merge_retained(entity, object));
            }
            ATELIER_TRY(scene_result(builder.set(root, "version", std::uint32_t{10})));
            builder.set_validated(root, "next_id", std::to_string(session.next_local_id_));
            if (document && !session.retained_document_.record.empty()) {
                ATELIER_TRY_BIND(kept, restore_references(session.retained_document_.record));
                overlay(builder, root, kept->root());
            }
            return {};
        }();
        ATELIER_TRY(encoded);
        graph.data = builder.finish();
        identities.complete = true;
        return graph;
    }

    // A scene material definition without retained data.
    static json::Result<void> material_definition(const Session& session, ObjectId object, std::uint64_t key,
                                                  json::Builder& builder, json::Value value) {
        auto& runtime = session.world_.runtime();
        const auto source = take(material::material_key(runtime, object));
        builder.set_validated(value, "id", std::to_string(key));
        ATELIER_TRY(builder.set(value, "source", source));
        if (source.empty()) {
            const auto state = take(material::read_instance(runtime, object));
            const auto authored = session.scene_material_parents_.find(object);
            if (authored == session.scene_material_parents_.end())
                throw std::runtime_error("Scene instance has no author parent identity.");
            if (authored->second.empty()) {
                ATELIER_TRY_BIND(parent_key, identity(session, state.parent.id()));
                const auto parent = ref_key(*parent_key);
                builder.set_validated(value, "parent", parent.root());
            } else {
                ATELIER_TRY(builder.set(value, "parent", authored->second));
            }
            const auto entries = *builder.array(value, "entries");
            ATELIER_TRY(assets::material_format::encode_entries(builder, entries, state.entries));
        }
        return {};
    }

    // Parents precede their scene children in Live definitions. Named parents
    // remain asset relationships and are loaded through the ordinary directory.
    static json::Result<void> material_definitions(const Session& session, ObjectId object, json::Builder& builder,
                                                   json::Value values, std::unordered_set<ObjectId>& seen) {
        if (!seen.insert(object).second)
            return {};
        json::Builder value;
        ATELIER_TRY_BIND(key, identity(session, object));
        ATELIER_TRY(material_definition(session, object, *key, value, value.root()));
        if (const auto parent = *value.root().find("parent"); parent && reference_value(*parent)) {
            ATELIER_TRY(material_definitions(
                session, take(material::read_instance(session.world_.runtime(), object)).parent.id(), builder, values,
                seen));
        }
        builder.append(values, value.root());
        return {};
    }

    // The graph of an attach change: the Entity record lists only these components.
    static Result<json::Document> live_components(const Session& session, ObjectId entity,
                                                  std::span<const ObjectId> components) {
        for (const auto component : components)
            if (take(world::inspect_component(session.world_.runtime(), component)).owner != entity)
                throw std::runtime_error("The Component does not belong to the Entity.");
        ATELIER_TRY_BIND(graph, capture(session, {&entity, 1}, true, false, true));
        const auto data = graph->data.root();
        std::unordered_set<std::string> wanted;
        for (const auto component : components)
            wanted.insert(std::to_string(session.local_ids_.at(component)));
        const auto key = std::to_string(session.local_ids_.at(entity));
        json::Builder builder;
        const auto root = builder.root();
        (void)builder.set(root, "version", *data.at("version"));
        (void)builder.set(root, "next_id", *data.at("next_id"));
        const auto entities = *builder.array(root, "entities");
        const auto record = builder.append_object(entities);
        builder.set_validated(record, "id", key);
        (void)builder.set(record, "existing", true);
        (void)builder.array(record, "children");
        const auto parts = *builder.array(record, "components");
        std::unordered_set<std::string> used;
        const auto records = *data.at("entities")->array();
        for (auto source : records) {
            if (*source.string({"id"}) != key)
                continue;
            const auto source_parts = *source.at("components")->array();
            for (auto part : source_parts) {
                if (!wanted.contains(std::string(*part.string({"id"}))))
                    continue;
                if (const auto at = *part.find("materials")) {
                    if (at->kind() == json::Kind::object) {
                        for (const auto& [key, entry] : *at->object_items()) {
                            ATELIER_TRY_BIND(resource, scene_result(entry.string({"resource"})));
                            used.emplace(*resource);
                        }
                    } else if (!at->is_null()) {
                        ATELIER_TRY_BIND(entries, scene_result(at->array()));
                        for (auto entry : *entries) {
                            ATELIER_TRY_BIND(resource, scene_result(entry.string({"resource"})));
                            used.emplace(*resource);
                        }
                    }
                }
                const auto refs = each_reference(part, [&](json::Value node) -> json::Result<void> {
                    if (const auto at = *node.find("$ref"))
                        used.emplace(*at->string());
                    return {};
                });
                ATELIER_TRY(scene_result(refs));
                builder.append(parts, part);
            }
        }
        if (parts.size() != components.size())
            throw std::runtime_error("Each listed Component must appear once on the Entity.");
        // Keep scene-local ancestors even if only their child is bound to a slot.
        for (bool expanded = true; expanded;) {
            expanded = false;
            const auto definitions = *data.at("materials")->array();
            for (auto definition : definitions)
                if (used.contains(std::string(*definition.string({"id"}))))
                    if (const auto parent = *definition.find("parent"); parent && reference_value(*parent))
                        expanded |= used.emplace(*parent->string({"$ref"})).second;
        }
        const auto materials = *builder.array(root, "materials");
        const auto definitions = *data.at("materials")->array();
        for (auto definition : definitions)
            if (used.contains(std::string(*definition.string({"id"}))))
                builder.append(materials, definition);
        (void)builder.array(root, "roots");
        return builder.finish();
    }

    // Play forwarding. A keyed op replaces the pending op with the same key.
    static void live_push(Session& session, std::string key, json::Value op, material::ImageRef texture = {},
                          std::shared_ptr<const assets::MeshAsset> mesh = {}) {
        const auto bytes = json::write(op);
        if (bytes.size() > assets::json_format::maximum_bytes)
            throw std::runtime_error("A Play change exceeds the 16 MiB limit.");
        if (!key.empty())
            std::erase_if(session.live_ops_, [&](const LiveOp& item) { return item.key == key; });
        session.live_ops_.push_back(
            {std::move(key), {{bytes.data(), bytes.size()}, std::move(texture), std::move(mesh)}});
    }

    static json::Result<std::string> live_id(const Session& session, ObjectId object) {
        ATELIER_TRY_BIND(key, identity(session, object));
        return std::to_string(*key);
    }

    // The final position among the parent's children, or among the roots.
    static std::size_t live_position(Session& session, ObjectId object, ObjectId parent) {
        std::size_t result{};
        if (parent != ObjectId{}) {
            for (const auto& child : take(world::children(session.runtime_, parent))) {
                if (child.id() == object)
                    return result;
                ++result;
            }
        } else
            for (const auto& member : take(session.world_.members()))
                if (take(world::parent_entity(session.runtime_, member.id())) == ObjectId{}) {
                    if (member.id() == object)
                        return result;
                    ++result;
                }
        throw std::runtime_error("The Entity is missing from its parent's list.");
    }

    static json::Result<json::Document> live_origin(Session& session, ObjectId entity) {
        const auto origin = take(world::space_reference(session.runtime_, entity));
        if (!origin)
            return json::Builder(json::Kind::null).finish();
        json::Builder builder;
        std::string key = "0";
        if (*origin != ObjectId{}) {
            ATELIER_TRY_BIND(id, live_id(session, *origin));
            key = std::move(*id);
        }
        builder.set_validated(builder.root(), key);
        return builder.finish();
    }

    static json::Result<json::Document> live_space(Session& session, ObjectId entity) {
        if (!take(world::has_transform(session.runtime_, entity)))
            return json::Builder(json::Kind::null).finish();
        json::Builder space;
        ATELIER_TRY(trs_json(space, space.root(), take(world::read_transform(session.runtime_, entity))));
        ATELIER_TRY_BIND(origin, live_origin(session, entity));
        space.set_validated(space.root(), "origin", origin->root());
        return space.finish();
    }

    // TRS leaves as bits: 1 position, 2 rotation, 4 scale.
    static unsigned leaf_mask(const world::Trs& a, const world::Trs& b) {
        return (a.position != b.position ? 1u : 0u) | (a.rotation != b.rotation ? 2u : 0u) |
               (a.scale != b.scale ? 4u : 0u);
    }

    static json::Result<json::Document> space_leaves(const world::Trs& value, unsigned mask) {
        json::Builder result;
        ATELIER_TRY(trs_json(result, result.root(), value));
        result.erase(result.root(), "origin");
        if (!(mask & 1))
            result.erase(result.root(), "position");
        if (!(mask & 2))
            result.erase(result.root(), "rotation");
        if (!(mask & 4))
            result.erase(result.root(), "scale");
        return result.finish();
    }

    // A set of these fields' current values; `key` names the object and fields.
    static json::Result<json::Document> live_properties(Session& session, ObjectId object,
                                                        std::span<const world::FieldPath> paths,
                                                        std::string* key = nullptr) {
        auto& runtime = session.runtime_;
        const auto type = take(runtime.type_of(object));
        const auto fields = take(detail::author_fields(runtime, type));
        json::Builder op;
        const auto root = op.root();
        op.set_validated(root, "op", "set");
        ATELIER_TRY_BIND(id, live_id(session, object));
        op.set_validated(root, "id", *id);
        const auto properties = op.object_validated(root, "properties");
        if (key)
            *key = "field " + *id;
        for (const auto& path : paths) {
            const auto field =
                std::ranges::find_if(fields, [&](const auto& item) { return std::ranges::equal(item.path, path); });
            if (field == fields.end())
                throw std::runtime_error("A changed field is not a scene property.");
            if (key)
                *key += " " + dotted(field->names);
            const auto at = op.object_path_validated(properties, field->names);
            if (field->form == ValueForm::strong_reference || field->form == ValueForm::weak_reference) {
                const auto reference = take(runtime.read_reference(object, path));
                if (reference.is_null()) {
                    const auto saved = json::Builder(json::Kind::null).finish();
                    op.replace(at, saved.root());
                } else {
                    ATELIER_TRY_BIND(id, identity(session, reference.id()));
                    const auto saved = ref_key(*id);
                    op.replace(at, saved.root());
                }
            } else {
                ATELIER_TRY(encode_scalar(runtime, op, at, take(runtime.read(object, path))));
            }
        }
        if (!properties.size())
            op.erase(root, "properties");
        return op.finish();
    }

    static json::Result<json::Document> live_body(Session& session, ObjectId component) {
        auto& runtime = session.runtime_;
        json::Builder op;
        const auto root = op.root();
        op.set_validated(root, "op", "set");
        ATELIER_TRY_BIND(id, live_id(session, component));
        op.set_validated(root, "id", *id);
        const auto properties = op.object_validated(root, "properties");
        const auto body = op.object_validated(properties, "body");
        for (const auto& field :
             business_fields(runtime, session.types_, session.presentation_types_, take(runtime.type_of(component))))
            if (field.form == ValueForm::inline_value && field.names.size() > 1 && field.names.front() == "body") {
                const auto at = op.object_path_validated(body, std::span(field.names).subspan(1));
                ATELIER_TRY(encode_scalar(runtime, op, at, take(runtime.read(component, field.path))));
            }
        return op.finish();
    }

    // Scene and Play changes decode a properties record leaf by leaf through the shared decoder.
    static void read_properties(Session& session, ObjectId object, json::Value record, Load& load,
                                const ResolveReference& resolve, const WriteLeaf& write = {}) {
        auto& runtime = session.runtime_;
        const auto type = take(runtime.type_of(object));
        const PropertyReport report{
            [&](std::vector<std::string> at, json::Value item, world::DiagnosticKind kind, const std::string& detail) {
                load.keep(object, std::move(at), item, kind, detail);
            },
            [&](std::vector<std::string> at, json::Value item) {
                load.report(world::Severity::info, world::DiagnosticKind::converted, object, std::move(at), &item,
                            "Integral number converted exactly to the field's integer type.");
            }};
        take(read_record(runtime, {type, object},
                         business_fields(runtime, session.types_, session.presentation_types_, type), record,
                         {"properties"}, resolve, write, &report));
    }

    // Scene instances share the asset entry codec, but their parent may have a
    // document-local identity. The directory resolver handles ordering/cycles.
    static json::Result<ObjectRoot>
    restore_material(Session& session, Project* project, json::Value entry,
                     const std::function<json::Result<ObjectRoot>(std::uint64_t)>& resolve,
                     std::vector<MissingResource>* missing = nullptr, ObjectId existing = {}) {
        auto& runtime = session.runtime_;
        if (!project)
            throw std::runtime_error("Material restoration requires a Project.");
        ATELIER_TRY_BIND(source, entry.string({"source"}));
        if (!source->empty()) {
            if (*entry.find("parent") || *entry.find("entries"))
                throw std::runtime_error("Material binding has competing sources.");
            auto root = take(session.material_resource(project->assets(), std::string(*source), missing));
            if (existing != ObjectId{} && root.id() != existing)
                throw std::runtime_error("Live material identity cannot change its asset.");
            return root;
        }
        material::InstanceState state;
        std::string parent_key;
        ATELIER_TRY_BIND(parent, entry.at("parent"));
        if (parent->kind() == json::Kind::string) {
            parent_key = *parent->string();
            if (parent_key.empty())
                throw std::runtime_error("A scene instance requires a parent identity.");
            state.parent = take(session.material_resource(project->assets(), parent_key, missing));
        } else {
            ATELIER_TRY(parent->members({"$ref"}));
            ATELIER_TRY_BIND(ref, parent->at("$ref"));
            ATELIER_TRY_BIND(key, local_key(*ref));
            ATELIER_TRY_BIND(resolved, resolve(*key));
            state.parent = std::move(*resolved);
        }
        ATELIER_TRY_BIND(entries, entry.at("entries"));
        ATELIER_TRY_BIND(decoded, assets::material_format::decode_entries(*entries));
        state.entries = std::move(*decoded);
        const auto surface = take(material::capture_program(runtime, state.parent.id()));
        state.textures.resize(state.entries.size());
        for (std::size_t i = 0; i < state.entries.size(); ++i) {
            const auto& item = state.entries[i];
            const auto* binding = std::get_if<material::TextureBinding>(&item.value);
            if (!item.enabled || !binding || binding->asset_key.empty() ||
                std::ranges::find(surface->program().textures(), item.path, &source::MaterialTextureInput::key) ==
                    surface->program().textures().end())
                continue;
            state.textures[i] = take(session.texture_resource(project->assets(), binding->asset_key, missing, i));
        }
        auto root =
            existing == ObjectId{} ? take(material::create_instance(runtime, {}, state)) : take(runtime.hold(existing));
        if (existing != ObjectId{}) {
            if (!take(material::material_key(runtime, existing)).empty())
                throw std::runtime_error("Live scene instance cannot replace a named asset.");
            take(material::update_instance(runtime, existing, state));
        }
        session.scene_material_parents_.insert_or_assign(root.id(), std::move(parent_key));
        return root;
    }

    // A Play copy's object for a document identity. Unknown identities mean Play
    // diverged; identities whose object the game destroyed are skipped.
    struct Gone : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    static std::optional<ObjectId> live_object(const Session& session, std::uint64_t key) {
        const auto at = session.live_objects_.find(key);
        if (at == session.live_objects_.end())
            return std::nullopt;
        return session.runtime_.type_of(at->second) ? std::optional(at->second) : std::optional(ObjectId{});
    }

    static json::Result<ObjectId> live_target(const Session& session, json::Value id) {
        ATELIER_TRY_BIND(key, local_key(id));
        const auto object = live_object(session, *key);
        if (!object)
            throw std::runtime_error("Document identity #" + std::to_string(*key) + " does not exist in Play.");
        if (*object == ObjectId{})
            throw Gone("#" + std::to_string(*key) + " was destroyed by the game.");
        return *object;
    }

    // A reference leaving a live graph; nullopt when the game destroyed its target.
    static std::optional<ObjectId> live_reference(const Session& session, std::uint64_t key, Load& load) {
        const auto object = live_object(session, key);
        if (!object)
            throw std::runtime_error("Reference target #" + std::to_string(key) + " does not exist in Play.");
        if (*object == ObjectId{}) {
            load.dropped.push_back(key);
            return std::nullopt;
        }
        return *object;
    }

    // A document load does not read a key that already failed again for each Environment that
    // names it; Play reads it again, since the editor may send it after the file was restored.
    static rendering::SkyMapRef resolve_sky(Session& session, Project* project, const std::string& source, Load& load,
                                            ObjectId object) {
        if (source.empty())
            return {};
        if (!project)
            throw std::runtime_error("Sky map restoration requires a Project.");
        const auto* item = load.live ? nullptr : session.unavailable(assets::AssetKind::sky, source);
        if (!item) {
            if (auto loaded = project->assets().sky_map(source)) {
                session.unavailable_.erase(std::pair{assets::AssetKind::sky, source});
                return loaded->map;
            } else
                item = &session.remember_unavailable(project->assets(), assets::AssetKind::sky, source, loaded.error());
        }
        json::Builder saved;
        saved.set_validated(saved.root(), source);
        const auto value = saved.root();
        load.report(world::Severity::warning, item->kind, object, {"sky_map"}, &value, item->error.message,
                    &item->error);
        return {};
    }

    static json::Result<Prepared> construct(Session& session, const Graph& graph, Project* project, bool keep_ids,
                                            Load& load) {
        const auto data = graph.data.root();
        ATELIER_TRY_BIND(version, data.u64({"version"}));
        if (*version != 10)
            throw std::runtime_error("Object graph requires scene format version 10.");
        ATELIER_TRY_BIND(members, data.object_items());
        for (const auto& [name, value] : *members)
            if (name != "version" && name != "next_id" && name != "roots" && name != "entities" &&
                name != "materials") {
                if (load.document)
                    load.keep({}, {std::string(name)}, value, world::DiagnosticKind::unknown_key,
                              "Unknown scene key kept from the saved scene.");
                else
                    load.report(world::Severity::warning, world::DiagnosticKind::unknown_key, {}, {std::string(name)},
                                &value, "Unknown scene key is not imported.");
            }
        ATELIER_TRY_BIND(next_value, data.at("next_id"));
        ATELIER_TRY_BIND(next_key, local_key(*next_value));
        const auto next = *next_key;
        ATELIER_TRY_BIND(entity_value, data.at("entities"));
        ATELIER_TRY_BIND(entity_array, entity_value->array());
        const auto entities = *entity_array;
        ATELIER_TRY_BIND(roots_value, data.at("roots"));
        ATELIER_TRY_BIND(roots_array, roots_value->array());
        const auto root_values = *roots_array;
        if (entities.size() > maximum_entities || root_values.size() > entities.size())
            throw std::runtime_error("Invalid Entity graph directory.");
        if (!keep_ids && take(session.world_.members()).size() + entities.size() > maximum_entities)
            throw std::runtime_error("Editor Entity limit reached.");
        Prepared result{session};
        result.project = project;
        auto& runtime = session.runtime_;
        auto& world = session.world_;
        std::unordered_map<std::uint64_t, ObjectId> ids;
        std::unordered_map<std::uint64_t, json::Value> descriptors;
        std::unordered_map<ObjectId, json::Value> records;
        std::unordered_set<std::uint64_t> entity_ids;
        std::uint64_t largest{};
        // An unregistered type keeps identity, composition and saved data in a
        // placeholder. A mesh payload is format evidence of a StaticMeshComponent base.
        const auto create = [&](json::Value value, bool entity, ObjectId owner) -> json::Result<ObjectId> {
            ATELIER_TRY_BIND(id, value.at("id"));
            ATELIER_TRY_BIND(parsed_key, local_key(*id));
            const auto key = *parsed_key;
            if (ids.contains(key))
                throw std::runtime_error("Duplicate graph object identity: " + std::to_string(key));
            if (load.live)
                if (const auto live = live_object(session, key); live && *live != ObjectId{})
                    throw std::runtime_error("Document identity #" + std::to_string(key) + " already exists in Play.");
            largest = std::max(largest, key);
            ATELIER_TRY_BIND(type_name_value, value.string({"type"}));
            const auto name = *type_name_value;
            const auto base = entity ? session.types_.entity : session.types_.component;
            const auto found = runtime.find_type(name);
            const auto matches = found ? runtime.is_a(*found, base) : Result<bool>{false};
            const bool placeholder = !matches || !*matches;
            const auto type = !placeholder                     ? *found
                              : !entity && *value.find("mesh") ? session.presentation_types_.static_mesh_component
                                                               : base;
            auto root =
                entity ? take(world::create_entity(runtime, type)) : take(world::create_component(runtime, type));
            const auto object = root.id();
            ids.emplace(key, object);
            descriptors.emplace(key, value);
            records.emplace(object, value);
            result.objects.push_back(std::move(root));
            if (keep_ids)
                session.local_ids_.emplace(object, key);
            else
                ATELIER_TRY(identity(session, object));
            load.owners.emplace(object, entity ? object : owner);
            load.types.emplace(object, name);
            load.keys.emplace(object, key);
            if (placeholder) {
                load.report(world::Severity::warning, world::DiagnosticKind::unknown_type, object, {}, nullptr,
                            std::string(entity ? "Entity" : "Component") +
                                " type is not registered; a placeholder keeps its saved data.");
                auto& kept = load.pending[object];
                kept.set_validated(kept.root(), "type", name);
            }
            std::wstring label;
            if (const auto at = *value.find("name")) {
                std::string problem;
                try {
                    if (at->kind() != json::Kind::string)
                        throw Mismatch("Expected a name string.");
                    label = platform::utf8_to_utf16_validated(*at->string());
                    if (entity || !label.empty())
                        if (auto valid = validate_name(label); !valid)
                            throw Mismatch(valid.error().message);
                } catch (const std::exception& error) {
                    problem = error.what();
                }
                if (!problem.empty()) {
                    label.clear();
                    load.keep(object, {"name"}, *at, world::DiagnosticKind::mismatch,
                              problem + " A generated name is used and the saved value is kept.");
                }
            }
            if (entity && label.empty()) {
                const auto dot = name.find_last_of('.');
                label = platform::utf8_to_utf16_validated(dot == name.npos ? name : name.substr(dot + 1)) + L" " +
                        std::to_wstring(key);
                if (!validate_name(label))
                    label = L"Entity " + std::to_wstring(key);
            }
            if (entity || !label.empty())
                result.labels.emplace(object, label);
            if (entity)
                entity_ids.insert(key);
            return object;
        };
        // A live record {"id","existing":true,"children":[],"components"} adds
        // components to an Entity Play already has; the Entity itself is untouched.
        std::unordered_set<std::uint64_t> anchors;
        for (auto entity : entities) {
            ObjectId object;
            bool existing = false;
            if (load.live && entity.kind() == json::Kind::object)
                if (const auto field = *entity.find("existing")) {
                    ATELIER_TRY_BIND(flag, field->boolean());
                    existing = *flag;
                }
            if (existing) {
                ATELIER_TRY(entity.members({"id", "existing", "children", "components"}));
                ATELIER_TRY_BIND(id, entity.at("id"));
                ATELIER_TRY_BIND(parsed_key, local_key(*id));
                const auto key = *parsed_key;
                if (ids.contains(key))
                    throw std::runtime_error("Duplicate graph object identity: " + std::to_string(key));
                if (entity.at("children")->size())
                    throw std::runtime_error("An existing Entity record cannot add children.");
                ATELIER_TRY_BIND(target, live_target(session, *id));
                object = *target;
                largest = std::max(largest, key);
                anchors.insert(key);
                ids.emplace(key, object);
                descriptors.emplace(key, entity);
                records.emplace(object, entity);
                load.owners.emplace(object, object);
                load.types.emplace(object, type_name(runtime, take(runtime.type_of(object))));
                load.keys.emplace(object, key);
            } else {
                ATELIER_TRY_BIND(created, create(entity, true, {}));
                object = *created;
                ATELIER_TRY(
                    load.extra(object, entity,
                               {"id", "type", "name", "enabled", "space", "children", "components", "properties"}));
            }
            ATELIER_TRY_BIND(parts_value, entity.at("components"));
            ATELIER_TRY_BIND(parts, parts_value->array());
            if (parts->size() > maximum_components)
                throw std::runtime_error("Invalid Component count.");
            for (auto part : *parts) {
                ATELIER_TRY_BIND(created, create(part, false, object));
                const auto component = *created;
                const auto component_type = take(runtime.type_of(component));
                if (derives(runtime, component_type, session.presentation_types_.static_mesh_component))
                    ATELIER_TRY(load.extra(component, part,
                                           {"id", "type", "name", "enabled", "properties", "mesh", "materials"}));
                else if (derives(runtime, component_type, session.presentation_types_.environment_component))
                    ATELIER_TRY(
                        load.extra(component, part, {"id", "type", "name", "enabled", "properties", "sky_map"}));
                else
                    ATELIER_TRY(load.extra(component, part, {"id", "type", "name", "enabled", "properties"}));
            }
        }
        // A stand-in keeps the key, so saving writes the reference unchanged.
        const auto report_missing = [&](ObjectId object, std::vector<std::string> base, json::Value entry,
                                        const std::vector<MissingResource>& missing) {
            for (const auto& item : missing) {
                auto at = base;
                json::Builder saved;
                saved.set_validated(saved.root(), item.key);
                const auto value = saved.root();
                if (item.input) {
                    at.push_back("textures");
                    at.emplace_back(std::to_string(*item.input));
                } else
                    at.emplace_back(*entry.find("parent") ? "parent" : "source");
                load.report(world::Severity::warning, item.kind, object, std::move(at), &value, item.error.message,
                            &item.error);
            }
        };
        std::unordered_map<std::uint64_t, ObjectRoot> material_ids;
        std::unordered_set<ObjectId> material_objects;
        std::unordered_map<ObjectId, ObjectRoot> copied_materials;
        std::unordered_map<ObjectId, std::uint64_t> copied_material_keys;
        std::unordered_set<std::uint64_t> used_materials;
        const json::Builder empty_array(json::Kind::array);
        const auto material_value = *data.find("materials");
        ATELIER_TRY_BIND(material_array, (material_value ? *material_value : empty_array.root()).array());
        const auto definitions = *material_array;
        if (definitions.size() > maximum_entities * maximum_components)
            throw std::runtime_error("Invalid scene material directory.");
        std::unordered_map<std::uint64_t, json::Value> material_records;
        for (auto definition : definitions) {
            ATELIER_TRY_BIND(id, definition.at("id"));
            ATELIER_TRY_BIND(parsed_key, local_key(*id));
            const auto key = *parsed_key;
            if (ids.contains(key) || !material_records.emplace(key, definition).second)
                throw std::runtime_error("Duplicate graph object identity: " + std::to_string(key));
            largest = std::max(largest, key);
        }
        std::unordered_set<std::uint64_t> resolving_materials;
        std::function<json::Result<ObjectRoot>(std::uint64_t)> restore;
        restore = [&](std::uint64_t key) -> json::Result<ObjectRoot> {
            if (const auto at = material_ids.find(key); at != material_ids.end())
                return at->second;
            const auto record = material_records.find(key);
            if (record == material_records.end())
                throw std::runtime_error("Material parent references an unknown scene resource.");
            if (!resolving_materials.insert(key).second)
                throw std::runtime_error("Scene material parent cycle.");
            const auto definition = record->second;
            ObjectRoot resource;
            std::vector<MissingResource> missing;
            // Play keeps a material it already has; its value changes travel separately.
            const auto live = load.live ? live_object(session, key) : std::nullopt;
            const bool reused = live && *live != ObjectId{};
            if (reused) {
                if (!session.is_material(*live))
                    throw std::runtime_error("Document identity #" + std::to_string(key) +
                                             " is not a material in Play.");
                resource = take(runtime.hold(*live));
            } else if (auto at = graph.materials.find(std::to_string(key)); at != graph.materials.end()) {
                if (take(material::material_key(runtime, at->second.id())).empty()) {
                    auto state = take(material::read_instance(runtime, at->second.id()));
                    ATELIER_TRY_BIND(parent, definition.at("parent"));
                    if (reference_value(*parent)) {
                        ATELIER_TRY_BIND(ref, parent->at("$ref"));
                        ATELIER_TRY_BIND(parent_key, local_key(*ref));
                        ATELIER_TRY_BIND(restored, restore(*parent_key));
                        state.parent = std::move(*restored);
                    }
                    resource = take(material::create_instance(runtime, {}, state));
                    session.scene_material_parents_.emplace(resource.id(),
                                                            session.scene_material_parents_.at(at->second.id()));
                } else
                    resource = at->second;
                copied_materials.emplace(at->second.id(), resource);
                copied_material_keys.emplace(at->second.id(), key);
            } else {
                ATELIER_TRY_BIND(restored, restore_material(session, project, definition, restore, &missing));
                resource = std::move(*restored);
            }
            if (const auto parent = *definition.find("parent"); parent && reference_value(*parent)) {
                ATELIER_TRY_BIND(ref, parent->at("$ref"));
                ATELIER_TRY_BIND(parent_key, local_key(*ref));
                used_materials.insert(*parent_key);
            }
            if (!material_objects.insert(resource.id()).second)
                throw std::runtime_error("One material asset has multiple document identities.");
            ids.emplace(key, resource.id());
            material_ids.emplace(key, resource);
            result.resources.push_back(resource);
            if (keep_ids && !reused) {
                if (!session.local_ids_.emplace(resource.id(), key).second)
                    throw std::runtime_error("One material asset has multiple document identities.");
            } else if (!keep_ids)
                ATELIER_TRY(identity(session, resource.id()));
            load.types.emplace(resource.id(), type_name(runtime, take(runtime.type_of(resource.id()))));
            load.keys.emplace(resource.id(), key);
            report_missing(resource.id(), {}, definition, missing);
            ATELIER_TRY(load.extra(resource.id(), definition, {"id", "source", "parent", "entries"}));
            const auto surface = take(material::capture_program(runtime, resource.id()));
            // Sparse entries may intentionally be disabled or outlive their declaration.
            // Required resources and invalid Source definitions were checked above;
            // resolution diagnostics describe preserved author intent, not load loss.
            for (const auto& issue : surface->diagnostics())
                load.report(world::Severity::info, world::DiagnosticKind::mismatch, resource.id(),
                            {"entries", issue.path}, nullptr, issue.message);
            resolving_materials.erase(key);
            return resource;
        };
        for (auto definition : definitions) {
            ATELIER_TRY(restore(*local_key(*definition.at("id"))));
        }
        if (next <= largest)
            throw std::runtime_error("next_id must exceed every document object identity.");
        if (keep_ids && !load.live)
            session.next_local_id_ = next;
        std::unordered_map<std::uint64_t, std::uint64_t> owners;
        for (const auto& entity : entities) {
            const auto key = *local_key(*entity.at("id"));
            const auto object = ids.at(key);
            std::vector<world::Attachment> parts;
            const auto components = *entity.at("components")->array();
            for (auto part : components)
                parts.push_back({ids.at(*local_key(*part.at("id")))});
            // A running Entity begins new components on attachment, after they are restored.
            if (anchors.contains(key))
                result.anchored.emplace_back(object, std::move(parts));
            else if (!parts.empty())
                committed(world.attach_components(object, parts));
            if (const auto space = *entity.find("space"); space && !space->is_null()) {
                world::Trs trs;
                if (space->kind() != json::Kind::object)
                    load.keep(object, {"space"}, *space, world::DiagnosticKind::mismatch,
                              "Expected a space record. The identity transform is used and the saved value is kept.");
                else {
                    ATELIER_TRY(load.extra(object, *space, {"position", "rotation", "scale", "origin"}, {"space"}));
                    const auto decoded = json_trs(*space);
                    if (decoded)
                        trs = *decoded;
                    else {
                        // The shared canonical check decides validity; origin stays structural.
                        load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object, {"space"},
                                    &*space,
                                    decoded.error().describe() +
                                        " The identity transform is used and the saved values are kept.");
                        trs = {};
                        for (const auto name : {"position", "rotation", "scale"})
                            if (const auto at = *space->find(name))
                                load.store(object, std::array{std::string("space"), std::string(name)}, *at);
                    }
                }
                take(world.set_transform(object, trs));
            }
            ATELIER_TRY_BIND(children_value, entity.at("children"));
            ATELIER_TRY_BIND(children, children_value->array());
            if (children->size() > entities.size())
                throw std::runtime_error("Invalid child directory.");
            for (auto child : *children) {
                ATELIER_TRY_BIND(id, local_key(child));
                if (!entity_ids.contains(*id) || !owners.emplace(*id, key).second)
                    throw std::runtime_error("Entity child has a wrong type or multiple parents.");
            }
        }
        std::unordered_set<std::uint64_t> roots;
        std::unordered_set<std::uint64_t> visited;
        const auto visit = [&](auto&& self, std::uint64_t key) -> void {
            if (!visited.insert(key).second)
                throw std::runtime_error("Entity composition contains a cycle or duplicate.");
            const auto children = *descriptors.at(key).at("children")->array();
            for (auto child : children)
                self(self, *local_key(child));
        };
        for (auto value : root_values) {
            ATELIER_TRY_BIND(parsed_key, local_key(value));
            const auto key = *parsed_key;
            if (!entity_ids.contains(key) || owners.contains(key) || !roots.insert(key).second)
                throw std::runtime_error("Invalid graph root.");
            visit(visit, key);
            result.roots.push_back(ids.at(key));
        }
        if (visited.size() != entity_ids.size())
            throw std::runtime_error("Entity graph has unreachable nodes or a cycle.");
        // All identities exist before relations or business references are restored.
        for (auto entity : entities) {
            const auto children = *entity.at("children")->array();
            for (auto child : children)
                committed(world.reparent(ids.at(*local_key(child)), ids.at(*local_key(*entity.at("id"))),
                                         world::ReparentRule::keep_local));
        }
        // Dangling or malformed identities are structural damage. A target whose
        // type does not satisfy the field is schema drift: nullopt, not an error.
        const auto resolve = [&](json::Value value, TypeId target,
                                 ValueForm form) -> json::Result<std::optional<ObjectReference>> {
            if (value.is_null())
                return std::optional{take(runtime.null_reference(
                    target, form == ValueForm::weak_reference ? form : ValueForm::strong_reference))};
            ObjectId object;
            if (value.kind() == json::Kind::object && *value.find("$ref")) {
                ATELIER_TRY(value.members({"$ref"}));
                ATELIER_TRY_BIND(parsed_key, local_key(*value.at("$ref")));
                const auto key = *parsed_key;
                if (ids.contains(key))
                    object = ids.at(key);
                else if (!load.live)
                    throw std::runtime_error("Reference target is outside this graph: " + std::to_string(key));
                else if (const auto live = live_reference(session, key, load))
                    object = *live;
                else
                    return std::optional{take(runtime.null_reference(
                        target, form == ValueForm::weak_reference ? form : ValueForm::strong_reference))};
                if (material_ids.contains(key))
                    used_materials.insert(key);
            } else if (value.kind() == json::Kind::object && *value.find("$external")) {
                ATELIER_TRY(value.members({"$external"}));
                ATELIER_TRY_BIND(key, value.string({"$external"}));
                const auto at = graph.external.find(std::string(*key));
                if (at == graph.external.end())
                    throw std::runtime_error("External object references cannot be loaded from a file.");
                object = at->second.id();
            } else
                return std::nullopt;
            auto held = runtime.hold(object, target);
            if (!held)
                return std::nullopt;
            return std::optional{form == ValueForm::weak_reference ? take(runtime.observe(held->reference()))
                                                                   : held->reference()};
        };
        for (const auto& entity : entities) {
            const auto space = *entity.find("space");
            if (!space || space->is_null())
                continue;
            std::optional<ObjectId> target;
            if (space->kind() == json::Kind::object)
                if (const auto origin = *space->find("origin"); origin && !origin->is_null()) {
                    if (origin->kind() == json::Kind::string) {
                        ATELIER_TRY_BIND(parsed_key, local_key(*origin, true));
                        const auto key = *parsed_key;
                        if (!key || entity_ids.contains(key))
                            target = key ? ids.at(key) : ObjectId{};
                        else if (!load.live)
                            throw std::runtime_error("Space reference must target an Entity in this graph.");
                        else
                            target = live_reference(session, key, load);
                    } else {
                        ATELIER_TRY_BIND(resolved, resolve(*origin, session.types_.entity, ValueForm::weak_reference));
                        if (!*resolved)
                            throw std::runtime_error("Space reference must target an Entity in this graph.");
                        target = (*resolved)->id();
                    }
                }
            result.spaces.push_back({ids.at(*local_key(*entity.at("id"))), target});
        }
        const auto properties = [&](json::Value value, ObjectId object) {
            if (const auto at = *value.find("properties")) {
                if (at->kind() == json::Kind::object)
                    read_properties(session, object, *at, load, resolve);
                else
                    load.keep(object, {"properties"}, *at, world::DiagnosticKind::mismatch,
                              "Expected a property record. Defaults are used and the saved value is kept.");
            }
            bool enabled = true;
            if (const auto at = *value.find("enabled"); !at)
                load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object, {"enabled"}, nullptr,
                            "The enabled flag is missing; the object is enabled.");
            else if (at->kind() != json::Kind::boolean)
                load.keep(object, {"enabled"}, *at, world::DiagnosticKind::mismatch,
                          "Expected a boolean. The object is enabled and the saved value is kept.");
            else
                enabled = *at->boolean();
            committed(world.set_enabled(object, enabled));
        };
        // A record that fails its combined validation returns to the type defaults.
        using Defaults = std::vector<std::pair<
            world::FieldPath, std::variant<Scalar, String, ArrayLeaf, ObjectReference, std::vector<ObjectReference>>>>;
        const auto snapshot = [&](ObjectId object, std::string_view prefix) {
            Defaults values;
            for (const auto& field :
                 business_fields(runtime, session.types_, session.presentation_types_, take(runtime.type_of(object))))
                if (!field.names.empty() && (prefix.empty() || field.names.front() == prefix)) {
                    if (field.form == ValueForm::inline_value) {
                        if (field.kind == TypeKind::array) {
                            const auto array = take(ArrayLeaf::create(runtime, field.type));
                            take(runtime.read(object, field.path, array.output()));
                            values.emplace_back(field.path, array);
                            continue;
                        }
                        if (field.kind != TypeKind::string) {
                            values.emplace_back(field.path, take(runtime.read(object, field.path)));
                            continue;
                        }
                        String text;
                        take(runtime.read(object, field.path, ValueOutput{field.type, &text}));
                        values.emplace_back(field.path, std::move(text));
                    } else if (field.form == ValueForm::strong_sequence) {
                        const auto locator = take(runtime.reference_field(object, field.path));
                        const auto count = take(runtime.sequence_size(locator));
                        std::vector<ObjectReference> references;
                        references.reserve(count);
                        for (std::size_t i = 0; i < count; ++i)
                            references.push_back(take(runtime.sequence_read(locator, i)));
                        values.emplace_back(field.path, std::move(references));
                    } else
                        values.emplace_back(field.path, take(runtime.read_reference(object, field.path)));
                }
            return values;
        };
        const auto saved_property = [&](ObjectId object, std::string_view name) -> std::optional<json::Value> {
            const auto record = records.at(object);
            const auto at = *record.find("properties");
            if (!at || at->kind() != json::Kind::object)
                return std::nullopt;
            if (name.empty())
                return at;
            return *at->find(name);
        };
        // An empty name is the whole properties record.
        const auto invalid_record = [&](ObjectId object, const std::string& name, const Defaults& defaults,
                                        const std::string& problem) {
            std::vector<std::string> path{"properties"};
            if (!name.empty())
                path.push_back(name);
            if (const auto value = saved_property(object, name))
                load.keep(object, path, *value, world::DiagnosticKind::mismatch,
                          problem + " Defaults are used and the saved value is kept.");
            else
                load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object, path, nullptr,
                            problem + " Defaults are used.");
            for (const auto& [path, value] : defaults) {
                if (const auto* scalar = std::get_if<Scalar>(&value))
                    take(runtime.write(object, path, *scalar));
                else if (const auto* array = std::get_if<ArrayLeaf>(&value))
                    take(runtime.write(object, path, array->input()));
                else if (const auto* text = std::get_if<String>(&value))
                    take(runtime.write(object, path, ValueInput{take(runtime.native_type<String>()), text}));
                else if (const auto* reference = std::get_if<ObjectReference>(&value))
                    take(runtime.write_reference(object, path, *reference));
                else {
                    const auto locator = take(runtime.reference_field(object, path));
                    const ReferenceChange change = SequenceSplice{locator, 0, take(runtime.sequence_size(locator)),
                                                                  std::get<std::vector<ObjectReference>>(value)};
                    take(runtime.apply_reference_changes({&change, 1}));
                }
            }
        };
        // Without its mesh a component has no slots. Its binding table is kept verbatim,
        // with scene material references renamed to document identities kept alive by it.
        const auto keep_bindings = [&](ObjectId object, json::Value entries) -> json::Result<void> {
            if (!entries.size())
                return {};
            if (entries.size() > 1024)
                throw std::runtime_error("Invalid material binding table.");
            json::Builder kept(entries);
            auto& held = load.materials[object];
            ATELIER_TRY_BIND(kept_entries, kept.root().array());
            for (auto entry : *kept_entries) {
                ATELIER_TRY_BIND(at, entry.find("resource"));
                if (!*at)
                    continue;
                ATELIER_TRY_BIND(parsed_key, local_key(**at));
                const auto key = *parsed_key;
                const auto found = material_ids.find(key);
                if (found == material_ids.end())
                    throw std::runtime_error("Material binding references an unknown scene resource.");
                used_materials.insert(key);
                held.push_back(found->second);
                ATELIER_TRY_BIND(id, identity(session, found->second.id()));
                kept.set_validated(**at, std::to_string(*id));
            }
            load.store(object, std::array{std::string("materials")}, kept.root());
            return {};
        };
        for (const auto& entity : entities) {
            if (const auto key = *local_key(*entity.at("id")); !anchors.contains(key))
                properties(entity, ids.at(key));
            const auto components = *entity.at("components")->array();
            for (auto part : components) {
                const auto object = ids.at(*local_key(*part.at("id")));
                const auto type = take(runtime.type_of(object));
                const bool light = derives(runtime, type, session.presentation_types_.light_component);
                const bool physical = physical_type(runtime, type);
                const bool environment = derives(runtime, type, session.presentation_types_.environment_component);
                const bool camera = derives(runtime, type, session.presentation_types_.camera_component);
                const auto light_defaults = light ? snapshot(object, "settings") : Defaults{};
                const auto record_defaults = environment || camera ? snapshot(object, {}) : Defaults{};
                const auto body_defaults = physical ? snapshot(object, "body") : Defaults{};
                properties(part, object);
                if (derives(runtime, type, session.presentation_types_.static_mesh_component)) {
                    const auto key = std::string(*part.string({"id"}));
                    if (auto at = graph.meshes.find(key); at != graph.meshes.end()) {
                        const auto& copy = at->second;
                        take(presentation::set_mesh_binding(runtime, object, copy.state.mesh));
                        auto bindings = copy.bindings;
                        for (auto& binding : bindings) {
                            used_materials.insert(copied_material_keys.at(binding.resource.id()));
                            binding.resource = copied_materials.at(binding.resource.id());
                        }
                        take(presentation::set_material_bindings(runtime, object, bindings));
                        if (const auto entries = *part.find("materials"); !copy.state.mesh.resolved &&
                                                                          !copy.state.mesh.source.empty() && entries &&
                                                                          entries->kind() == json::Kind::array)
                            ATELIER_TRY(keep_bindings(object, *entries));
                    } else {
                        if (!project)
                            throw std::runtime_error("Material and mesh restoration requires a Project.");
                        std::string source;
                        if (const auto mesh_at = *part.find("mesh"); !mesh_at)
                            load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object, {"mesh"},
                                        nullptr, "The mesh source is missing; the component has no mesh.");
                        else if (mesh_at->kind() != json::Kind::string)
                            load.keep(
                                object, {"mesh"}, *mesh_at, world::DiagnosticKind::mismatch,
                                "Expected a mesh source key. The component has no mesh and the saved value is kept.");
                        else
                            source = *mesh_at->string();
                        geometry::MeshRef mesh;
                        std::vector<presentation::MaterialBinding> defaults;
                        if (!source.empty()) {
                            // A source that already failed is not read again for each component that uses it.
                            const auto* item = session.unavailable(assets::AssetKind::mesh, source);
                            if (!item) {
                                if (auto loaded = project->assets().mesh(source)) {
                                    mesh = loaded->mesh;
                                    std::vector<MissingResource> missing;
                                    defaults = take(session.mesh_defaults(project->assets(), *loaded, &missing));
                                    for (const auto& absent : missing) {
                                        json::Builder saved;
                                        saved.set_validated(saved.root(), absent.key);
                                        const auto value = saved.root();
                                        load.report(world::Severity::warning, absent.kind, object, {"mesh"}, &value,
                                                    absent.error.message, &absent.error);
                                    }
                                } else
                                    item = &session.remember_unavailable(project->assets(), assets::AssetKind::mesh,
                                                                         source, loaded.error());
                            }
                            if (item) {
                                json::Builder saved;
                                saved.set_validated(saved.root(), source);
                                const auto value = saved.root();
                                load.report(world::Severity::warning, item->kind, object, {"mesh"}, &value,
                                            item->error.message, &item->error);
                            }
                        }
                        const bool unresolved = !source.empty() && !mesh;
                        take(presentation::set_mesh_binding(runtime, object,
                                                            {source, std::move(mesh), std::move(defaults)}));
                        std::vector<presentation::MaterialBinding> bindings;
                        if (const auto entries = *part.find("materials");
                            entries && entries->kind() != json::Kind::array)
                            load.keep(
                                object, {"materials"}, *entries, world::DiagnosticKind::mismatch,
                                "Expected a material binding table. Bindings are empty and the saved value is kept.");
                        else if (entries && unresolved)
                            ATELIER_TRY(keep_bindings(object, *entries));
                        else if (entries) {
                            if (entries->size() > 1024)
                                throw std::runtime_error("Invalid material binding table.");
                            const auto items = *entries->array();
                            for (auto entry : items) {
                                ATELIER_TRY_BIND(slot_value, entry.string({"slot"}));
                                const auto slot = std::string(*slot_value);
                                const auto members = *entry.object_items();
                                for (const auto& [name, item] : members)
                                    if (name != "slot" && name != "resource" && name != "overridden")
                                        load.keep_slot(object, slot, std::string(name), item,
                                                       world::DiagnosticKind::unknown_key,
                                                       "Unknown binding key kept from the saved scene.");
                                ATELIER_TRY_BIND(resource_value, entry.at("resource"));
                                ATELIER_TRY_BIND(resource_key, local_key(*resource_value));
                                const auto key = *resource_key;
                                const auto at = material_ids.find(key);
                                if (at == material_ids.end())
                                    throw std::runtime_error("Material binding references an unknown scene resource.");
                                used_materials.insert(key);
                                ObjectRoot resource = at->second;
                                bool overridden{};
                                if (const auto flag = *entry.find("overridden"); !flag)
                                    load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                                {"materials", slot, "overridden"}, nullptr,
                                                "The override flag is missing; the slot is not overridden.");
                                else if (flag->kind() != json::Kind::boolean)
                                    load.keep_slot(
                                        object, slot, "overridden", *flag, world::DiagnosticKind::mismatch,
                                        "Expected a boolean. The slot is not overridden and the saved value is kept.");
                                else
                                    overridden = *flag->boolean();
                                bindings.push_back({slot, std::move(resource), overridden});
                            }
                        }
                        const auto geometry = take(presentation::read_static_mesh(runtime, object)).mesh.resolved;
                        bool missing_slot{};
                        std::erase_if(bindings, [&](const auto& binding) {
                            std::size_t matches{};
                            if (geometry)
                                for (std::size_t i = 0; i < geometry->materials().size(); ++i)
                                    matches += geometry::material_slot_name(*geometry, i) == binding.slot;
                            if (matches == 1)
                                return false;
                            load.report(
                                world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                {"materials", binding.slot}, nullptr,
                                "The mesh no longer has one matching slot; the author binding is kept without being applied.");
                            missing_slot = true;
                            return true;
                        });
                        if (missing_slot)
                            ATELIER_TRY(keep_bindings(object, *part.at("materials")));
                        take(presentation::set_material_bindings(runtime, object, bindings));
                    }
                }
                if (environment) {
                    if (auto at = graph.skies.find(std::string(*part.string({"id"}))); at != graph.skies.end())
                        take(presentation::set_sky_map_binding(runtime, object, at->second));
                    else {
                        std::string source;
                        if (const auto sky_at = *part.find("sky_map"); !sky_at)
                            load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object, {"sky_map"},
                                        nullptr, "The sky map key is missing; the sky has no map.");
                        else if (sky_at->kind() != json::Kind::string)
                            load.keep(object, {"sky_map"}, *sky_at, world::DiagnosticKind::mismatch,
                                      "Expected a sky map key. The sky has no map and the saved value is kept.");
                        else
                            source = *sky_at->string();
                        take(presentation::set_sky_map_binding(
                            runtime, object, {source, resolve_sky(session, project, source, load, object)}));
                    }
                }
                if (light)
                    if (auto valid =
                            presentation::validate_light(runtime, take(presentation::read_light(runtime, object)));
                        !valid)
                        invalid_record(object, "settings", light_defaults, valid.error().message);
                // These rules span fields, so a record that breaks them returns whole to the type defaults.
                if (environment || camera)
                    if (const auto failure = take(runtime.validate(object)))
                        invalid_record(object, {}, record_defaults, failure->error.message);
                if (physical) {
                    auto settings = take(physics::read_settings(runtime, object));
                    std::string problem;
                    if (auto valid = world::validate_body_settings(runtime, settings); !valid)
                        problem = valid.error().message;
                    else if (settings.enabled && settings.shape == physics::ShapeKind::triangle_mesh && project) {
                        const auto mesh = presentation::read_static_mesh(runtime, object);
                        if (!mesh)
                            problem = mesh.error().message;
                        else if (mesh->mesh.source.empty())
                            problem = "Triangle collision needs a persistent mesh source.";
                    }
                    if (!problem.empty()) {
                        invalid_record(object, "body", body_defaults, problem);
                        settings = take(physics::read_settings(runtime, object));
                    }
                    if (auto at = graph.bodies.find(std::string(*part.string({"id"}))); at != graph.bodies.end())
                        settings.cooked_shape = at->second.cooked_shape;
                    if (settings.enabled && settings.shape == physics::ShapeKind::triangle_mesh) {
                        const auto mesh = take(presentation::read_static_mesh(runtime, object));
                        if (!mesh.mesh.resolved && !mesh.mesh.source.empty()) {
                            const auto* item = session.unavailable(assets::AssetKind::mesh, mesh.mesh.source);
                            load.report(
                                world::Severity::warning, item ? item->kind : world::DiagnosticKind::missing_resource,
                                object, {"properties", "body", "shape"}, nullptr,
                                "Triangle collision has no mesh to cook; Play cannot create this body until the mesh is restored.");
                        } else if (mesh.mesh.source.starts_with("project/"))
                            result.triangle_meshes.push_back(object);
                    }
                    if (auto configured = physics::configure(runtime, object, settings); !configured) {
                        invalid_record(object, "body", body_defaults, configured.error().message);
                        std::erase(result.triangle_meshes, object);
                        take(physics::configure(runtime, object, take(physics::read_settings(runtime, object))));
                    }
                }
            }
        }
        // Controller bindings validate their entity sequences as a whole.
        const auto bound = [&](ObjectId object, const char* name, Result<void> applied, auto clear) {
            if (applied)
                return;
            if (const auto value = saved_property(object, name))
                load.keep(object, {"properties", name}, *value, world::DiagnosticKind::mismatch,
                          applied.error().message + " The binding is empty and the saved value is kept.");
            else
                load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object, {"properties", name},
                            nullptr, applied.error().message + " The binding is empty.");
            take(clear());
        };
        for (const auto& root : result.objects) {
            const auto object = root.id();
            const auto type = take(runtime.type_of(object));
            if (derives(runtime, type, session.physics_types_.ragdoll_component))
                bound(object, "parts",
                      physics::set_ragdoll_parts(runtime, object, take(physics::ragdoll_parts(runtime, object))),
                      [&] { return physics::set_ragdoll_parts(runtime, object, {}); });
            if (derives(runtime, type, session.physics_types_.scene_query_component))
                bound(object, "ignored_entities",
                      physics::set_query_ignored_entities(runtime, object,
                                                          take(physics::query_ignored_entities(runtime, object))),
                      [&] { return physics::set_query_ignored_entities(runtime, object, {}); });
        }
        // Retained references bind to document identities, which stay stable for
        // the life of the document and are re-encoded on save.
        for (auto& [object, entry] : load.pending) {
            Retained kept;
            if (const auto at = *entry.root().find("type"))
                kept.type = *at->string();
            for (const auto name : {"record", "slots"})
                if (auto at = *entry.root().find(name)) {
                    const auto restored = each_reference(*at, [&](json::Value node) -> json::Result<void> {
                        ObjectId target;
                        if (*node.find("$ref")) {
                            ATELIER_TRY(node.members({"$ref"}));
                            ATELIER_TRY_BIND(parsed_key, local_key(*node.at("$ref")));
                            const auto key = *parsed_key;
                            // A live graph's outward references already name document identities.
                            if (!ids.contains(key) && load.live)
                                return {};
                            if (!ids.contains(key))
                                throw std::runtime_error("Reference target is outside this graph: " +
                                                         std::to_string(key));
                            target = ids.at(key);
                            if (material_ids.contains(key))
                                used_materials.insert(key);
                        } else {
                            ATELIER_TRY(node.members({"$external"}));
                            ATELIER_TRY_BIND(key, node.string({"$external"}));
                            const auto found = graph.external.find(std::string(*key));
                            if (found == graph.external.end())
                                throw std::runtime_error("External object references cannot be loaded from a file.");
                            target = found->second.id();
                        }
                        ATELIER_TRY_BIND(id, identity(session, target));
                        const auto reference = ref_key(*id);
                        entry.replace(node, reference.root());
                        return {};
                    });
                    ATELIER_TRY(restored);
                    (std::string_view(name) == "record" ? kept.record : kept.slots) =
                        dump_retained(*entry.root().at(name));
                }
            if (auto at = load.materials.find(object); at != load.materials.end())
                kept.materials = std::move(at->second);
            load.retained.emplace(object, std::move(kept));
        }
        for (const auto& [key, resource] : material_ids)
            if (!used_materials.contains(key) && !load.live) {
                if (load.document) {
                    load.report(world::Severity::warning, world::DiagnosticKind::unknown_key, resource.id(), {},
                                nullptr, "The material definition is not referenced; it is kept for the next save.");
                    load.unreferenced.push_back(resource);
                } else
                    load.report(world::Severity::warning, world::DiagnosticKind::unknown_key, resource.id(), {},
                                nullptr, "The material definition is not referenced and is not imported.");
            }
        return result;
    }

    static std::vector<world::EntityAttachment> attachments(const Prepared& prepared, const Graph& graph,
                                                            ObjectId parent, bool duplicate) {
        std::vector<world::EntityAttachment> attachments;
        attachments.reserve(prepared.roots.size());
        for (std::size_t i = 0; i < prepared.roots.size(); ++i) {
            auto target = parent;
            if (duplicate)
                target = graph.parents.at(std::string(*graph.data.root().at("roots")->at(i)->string()));
            attachments.push_back({prepared.roots[i], target, world::append});
        }
        return attachments;
    }

    static void prepare_cook(Session& session, Prepared& prepared, std::span<const world::EntityAttachment> attachments,
                             Load& load) {
        if (prepared.triangle_meshes.empty())
            return;
        std::unordered_map<ObjectId, math::Matrix4> worlds;
        if (!attachments.empty())
            for (const auto& [object, matrix] :
                 take(session.world_.attachment_world_matrices(attachments, prepared.spaces)))
                worlds.emplace(object, matrix);
        std::unordered_map<ObjectId, ObjectId> anchored;
        for (const auto& [entity, parts] : prepared.anchored)
            for (const auto& part : parts)
                anchored.emplace(part.component, entity);
        // Bodies sharing a mesh and world scale share one cooked shape for this load.
        std::map<std::pair<std::string, std::array<float, 3>>, Result<std::shared_ptr<const std::string>>> cooks;
        for (const auto object : prepared.triangle_meshes) {
            const auto mesh = take(presentation::read_static_mesh(session.runtime_, object));
            const auto at = anchored.find(object);
            const auto owner =
                at != anchored.end() ? at->second : take(world::inspect_component(session.runtime_, object)).owner;
            // Components added to a live Entity take its current space.
            const auto found = worlds.find(owner);
            const auto matrix =
                found != worlds.end() ? found->second : take(world::world_matrix(session.runtime_, owner));
            const auto trs = take(world::relative_transform(matrix, world::transform_matrix({})));
            auto settings = take(physics::read_settings(session.runtime_, object));
            if (prepared.project) {
                const auto key = std::pair{mesh.mesh.source, std::array{trs.scale.x, trs.scale.y, trs.scale.z}};
                auto cached = cooks.find(key);
                if (cached == cooks.end())
                    cached = cooks.emplace(key, prepared.project->collision(mesh.mesh.source, trs.scale)).first;
                const auto& cooked = cached->second;
                if (!cooked) {
                    load.report(world::Severity::warning, world::DiagnosticKind::unreadable_resource, object,
                                {"properties", "body", "shape"}, nullptr, cooked.error().message, &cooked.error());
                    continue;
                }
                settings.cooked_shape = *cooked;
            } else if (settings.cooked_shape)
                take(physics::validate_cooked_triangle_mesh(*settings.cooked_shape, mesh.mesh.resolved, trs.scale,
                                                            mesh.mesh.source));
            take(physics::configure(session.runtime_, object, std::move(settings)));
        }
    }

    static world::Commit publish(Session& session, Prepared& prepared, const Graph& graph, ObjectId parent,
                                 bool duplicate, Load& load) {
        EntityChange history;
        history.added = true;
        history.removals.reserve(prepared.roots.size());
        for (auto root : prepared.roots) {
            world::EntityRemoval removal;
            removal.root = take(session.runtime_.hold(root));
            history.removals.push_back(std::move(removal));
        }
        auto plans = attachments(prepared, graph, parent, duplicate);
        prepare_cook(session, prepared, plans, load);
        auto selection = prepared.roots;
        session.history_.reserve(session.history_.size() + 1);
        session.retained_.reserve(session.retained_.size() + load.retained.size());
        if (!duplicate)
            session.load_issues_.reserve(session.load_issues_.size() + load.issues.size());
        for (const auto& [object, label] : prepared.labels)
            session.names_.emplace(object, label);
        // Retained data travels with its objects, which history keeps alive.
        const auto adopt = [&] {
            session.retained_.merge(load.retained);
            if (!duplicate)
                std::ranges::move(load.issues, std::back_inserter(session.load_issues_));
        };
        world::Commit result;
        try {
            result = take(session.world_.attach_batch(plans, prepared.spaces));
        } catch (...) {
            const auto state = world::inspect_entity(session.runtime_, prepared.roots.front());
            const bool attached = state && (state->state == world::EntityState::attached_inactive ||
                                            state->state == world::EntityState::active);
            const bool retained = attached && !session.world_.detach_batch(prepared.roots);
            if (retained) {
                prepared.published = true;
                session.remember(std::move(history));
                adopt();
            }
            throw;
        }
        if (result.changed) {
            // Selection and history storage were prepared before World publication.
            // No admission, allocation or callback can fail between these commits.
            prepared.published = true;
            session.selected_entities_.swap(selection);
            session.selection_ = {prepared.roots.back(), prepared.roots.back()};
            ++session.revision_;
            session.remember(std::move(history));
            adopt();
        }
        return result;
    }

    // Play publication keeps document identities and records no history.
    static world::Commit publish_live(Session& session, Prepared& prepared, ObjectId parent, std::size_t index,
                                      Load& load) {
        std::vector<world::EntityAttachment> plans;
        for (const auto root : prepared.roots)
            plans.push_back({root, parent, index});
        prepare_cook(session, prepared, plans, load);
        for (const auto& [object, label] : prepared.labels)
            session.names_.insert_or_assign(object, label);
        world::Commit result;
        if (!plans.empty())
            result = take(session.world_.attach_batch(plans, prepared.spaces));
        for (const auto& [entity, parts] : prepared.anchored)
            result = take(session.world_.attach_components(entity, parts, index));
        prepared.published = true;
        session.retained_.merge(load.retained);
        for (const auto& object : prepared.objects)
            session.live_objects_.insert_or_assign(session.local_ids_.at(object.id()), object.id());
        for (const auto& resource : prepared.resources)
            session.live_objects_.insert_or_assign(session.local_ids_.at(resource.id()), resource.id());
        return result;
    }

    struct Unsupported : detail::Failure {
        using detail::Failure::Failure;
    };

    // A simulation that cannot follow an authored pose leaves the edit unapplied.
    static void relocated(Result<void> value) {
        if (!value && value.error().code == ErrorCode::unsupported_operation)
            throw Unsupported(std::move(value.error()));
        take(std::move(value));
    }

    // A Play index is the final position among the destination's children, or its roots for a null parent.
    // Game spawns and destructions legitimately change those lists, so a stale index clamps to the end.
    // World attaches a new root before a member position, so an added root's ordinal is translated.
    static json::Result<std::size_t> live_index(Session& session, json::Value value, ObjectId parent,
                                                std::optional<ObjectId> moved = {}) {
        if (value.is_null())
            return world::append;
        ATELIER_TRY_BIND(parsed, value.u64());
        const auto wanted = *parsed;
        auto& runtime = session.runtime_;
        if (parent != ObjectId{}) {
            const auto siblings = take(world::children(runtime, parent));
            return std::min<std::size_t>(
                wanted, siblings.size() -
                            std::ranges::count_if(siblings, [&](const auto& item) { return item.id() == moved; }));
        }
        const auto members = take(session.world_.members());
        std::size_t ordinal{};
        for (std::size_t i = 0; i < members.size(); ++i) {
            const auto id = members[i].id();
            if (id == moved || take(world::parent_entity(runtime, id)) != ObjectId{})
                continue;
            if (ordinal == wanted)
                return moved ? wanted : i;
            ++ordinal;
        }
        return moved ? ordinal : world::append;
    }

    // One op of a Play change batch; see Session::apply_live.
    static void apply_change(Session& session, Project& project, json::Value op, std::size_t index,
                             std::vector<world::Diagnostic>& out) {
        auto& runtime = session.runtime_;
        auto& world = session.world_;
        std::string subject = "Play change " + std::to_string(index);
        ObjectId object;
        const auto note = [&](world::Severity severity, world::DiagnosticKind kind, const std::string& detail,
                              const Error* cause = nullptr) {
            world::Diagnostic item;
            item.serial = out.size() + 1;
            item.severity = severity;
            item.kind = kind;
            item.phase = world::DiagnosticPhase::command;
            item.object = object;
            if (const auto type = runtime.type_of(object)) {
                item.type = type_name(runtime, *type);
                if (derives(runtime, *type, session.types_.entity))
                    item.entity = object;
                else if (const auto component = world::inspect_component(runtime, object))
                    item.entity = component->owner;
            }
            item.message = cause ? cause->message : subject + ": " + detail;
            if (cause) {
                item.error_code = cause->code;
                item.source = cause->source;
            }
            out.push_back(std::move(item));
        };
        const auto target = [&](json::Value id) -> json::Result<ObjectId> {
            ATELIER_TRY_BIND(key, local_key(id));
            subject += " #" + std::to_string(*key);
            if (const auto at = session.live_objects_.find(*key); at != session.live_objects_.end())
                object = at->second;
            return live_target(session, id);
        };
        const auto commit = [&](Result<world::Commit> value) {
            const auto result = take(std::move(value));
            if (result.notification_error)
                note(world::Severity::warning, world::DiagnosticKind::observer_failed,
                     result.notification_error->message, &*result.notification_error);
        };
        Load load;
        load.live = true;
        try {
            const auto applied = [&]() -> json::Result<void> {
                ATELIER_TRY_BIND(kind_value, op.string({"op"}));
                const auto kind = *kind_value;
                if (kind == "add" || kind == "attach") {
                    const bool add = kind == "add";
                    if (add)
                        ATELIER_TRY(op.members({"op", "parent", "index", "graph"}));
                    else
                        ATELIER_TRY(op.members({"op", "entity", "index", "graph"}));
                    ATELIER_TRY_BIND(graph_value, op.at("graph"));
                    ObjectId parent;
                    if (add) {
                        ATELIER_TRY_BIND(roots, graph_value->at("roots"));
                        if (roots->kind() == json::Kind::array && roots->size() == 1) {
                            ATELIER_TRY_BIND(key, local_key(*roots->at(0)));
                            subject += " #" + std::to_string(*key);
                        }
                        const auto parent_value = *op.at("parent");
                        if (!parent_value.is_null()) {
                            ATELIER_TRY_BIND(found, live_target(session, parent_value));
                            parent = *found;
                        }
                    }
                    ObjectId entity;
                    if (!add) {
                        ATELIER_TRY_BIND(found, target(*op.at("entity")));
                        entity = *found;
                    }
                    Graph graph;
                    graph.data = json::Document(*graph_value);
                    // A resource imported after Play opened its Project is not catalogued yet.
                    std::optional<Prepared> prepared;
                    for (bool refreshed = false;; refreshed = true) {
                        load = Load{};
                        load.live = true;
                        ATELIER_TRY_BIND(created, construct(session, graph, &project, true, load));
                        prepared.emplace(std::move(*created));
                        if (refreshed || std::ranges::none_of(load.issues, [](const auto& issue) {
                                return issue.kind == world::DiagnosticKind::missing_resource;
                            }))
                            break;
                        prepared.reset();
                        take(project.refresh());
                    }
                    if (add ? prepared->roots.size() != 1 || !prepared->anchored.empty()
                            : !prepared->roots.empty() || prepared->anchored.size() != 1 ||
                                  prepared->anchored.front().first != entity)
                        throw std::runtime_error(add ? "An add carries exactly one root Entity."
                                                     : "An attach carries only new components of its Entity.");
                    if (add)
                        object = prepared->roots.front();
                    const auto wanted = *op.at("index");
                    std::size_t placed = world::append;
                    if (add) {
                        ATELIER_TRY_BIND(position, live_index(session, wanted, parent));
                        placed = *position;
                    } else if (!wanted.is_null()) {
                        ATELIER_TRY_BIND(position, wanted.u64());
                        placed = std::min<std::size_t>(*position, take(world::components(runtime, entity)).size());
                    }
                    commit(publish_live(session, *prepared, parent, placed, load));
                    // The document carries the same unresolved data; Play reports it without diverging.
                    for (auto issue : load.issues)
                        if (issue.severity != world::Severity::info) {
                            issue.serial = out.size() + 1;
                            issue.phase = world::DiagnosticPhase::command;
                            issue.message = subject + ": " + issue.message;
                            out.push_back(std::move(issue));
                        }
                } else if (kind == "remove") {
                    ATELIER_TRY(op.members({"op", "id"}));
                    ATELIER_TRY_BIND(removed_value, target(*op.at("id")));
                    const auto removed = *removed_value;
                    const auto type = take(runtime.type_of(removed));
                    if (derives(runtime, type, session.types_.entity))
                        commit(world::destroy_entity(runtime, removed));
                    else if (derives(runtime, type, session.types_.component))
                        commit(world::destroy_component(runtime, removed));
                    else
                        throw std::runtime_error("Only an Entity or Component can be removed.");
                } else if (kind == "set") {
                    ATELIER_TRY(op.allowed_members(
                        {"op", "id", "name", "enabled", "space", "properties", "materials", "sky_map"}));
                    ATELIER_TRY_BIND(id, op.at("id"));
                    ATELIER_TRY_BIND(edited_value, target(*id));
                    const auto edited = *edited_value;
                    const auto type = take(runtime.type_of(edited));
                    const bool entity = derives(runtime, type, session.types_.entity);
                    load.policy = LoadPolicy::strict;
                    if (const auto at = *op.find("name")) {
                        ATELIER_TRY_BIND(text, at->string());
                        auto label = platform::utf8_to_utf16_validated(*text);
                        if (entity || !label.empty())
                            take(validate_name(label));
                        if (label.empty())
                            session.names_.erase(edited);
                        else
                            session.names_.insert_or_assign(edited, std::move(label));
                    }
                    if (const auto at = *op.find("enabled")) {
                        ATELIER_TRY_BIND(enabled, at->boolean());
                        commit(world.set_enabled(edited, *enabled));
                    }
                    if (const auto at = *op.find("properties")) {
                        if (at->kind() != json::Kind::object)
                            throw std::runtime_error("Expected a property record.");
                        const ResolveReference resolve =
                            [&](json::Value value, TypeId field_type,
                                ValueForm form) -> json::Result<std::optional<ObjectReference>> {
                            const auto null = [&] {
                                return take(runtime.null_reference(field_type, form == ValueForm::weak_reference
                                                                                   ? form
                                                                                   : ValueForm::strong_reference));
                            };
                            if (value.is_null())
                                return std::optional{null()};
                            if (value.kind() != json::Kind::object || !*value.find("$ref"))
                                return std::nullopt;
                            ATELIER_TRY(value.members({"$ref"}));
                            ATELIER_TRY_BIND(key, local_key(*value.at("$ref")));
                            const auto live = live_reference(session, *key, load);
                            if (!live)
                                return std::optional{null()};
                            auto held = runtime.hold(*live, field_type);
                            if (!held)
                                return std::nullopt;
                            return std::optional{form == ValueForm::weak_reference
                                                     ? take(runtime.observe(held->reference()))
                                                     : held->reference()};
                        };
                        // Scalar leaves are one group write: a type's rules can span them, as when a probe volume moves.
                        std::vector<world::FieldPath> paths;
                        std::vector<Scalar> values;
                        read_properties(session, edited, *at, load, resolve,
                                        [&](std::span<const FieldId> path, const PropertyLeaf& leaf) -> Result<void> {
                                            if (const auto* scalar = std::get_if<Scalar>(&leaf)) {
                                                paths.emplace_back(path.begin(), path.end());
                                                values.push_back(*scalar);
                                                return {};
                                            }
                                            if (const auto* array = std::get_if<ArrayLeaf>(&leaf))
                                                return world.write(edited, path, array->input());
                                            return world.write(edited, path,
                                                               ValueInput{take(runtime.native_type<String>()),
                                                                          &std::get<String>(leaf)});
                                        });
                        if (!paths.empty())
                            take(world.write(edited, std::span<const world::FieldPath>(paths),
                                             std::span<const Scalar>(values)));
                    }
                    if (const auto at = *op.find("materials")) {
                        ATELIER_TRY_BIND(entries, at->array());
                        if (entries->size() > 1024)
                            throw std::runtime_error("Expected a material binding table.");
                        std::vector<presentation::MaterialBinding> bindings;
                        for (auto entry : *entries) {
                            ATELIER_TRY(entry.members({"slot", "resource", "overridden"}));
                            ATELIER_TRY_BIND(parsed_key, local_key(*entry.at("resource")));
                            const auto key = *parsed_key;
                            const auto live = live_object(session, key);
                            // Materials are document resources; the editor sends a missing one first.
                            if (!live || *live == ObjectId{} || !session.is_material(*live))
                                throw std::runtime_error("Material #" + std::to_string(key) +
                                                         " does not exist in Play.");
                            ATELIER_TRY_BIND(slot, entry.string({"slot"}));
                            ATELIER_TRY_BIND(overridden, entry.boolean({"overridden"}));
                            bindings.push_back({std::string(*slot), take(runtime.hold(*live)), *overridden});
                        }
                        take(presentation::set_material_bindings(runtime, edited, bindings));
                    }
                    if (const auto at = *op.find("sky_map")) {
                        ATELIER_TRY_BIND(text, at->string());
                        const auto source = std::string(*text);
                        rendering::SkyMapRef map;
                        const MissingResource* missing{};
                        if (!source.empty()) {
                            auto loaded = project.assets().sky_map(source);
                            // A sky map imported after Play opened its Project is not catalogued yet.
                            if (!loaded && !project.entry(source)) {
                                take(project.refresh());
                                loaded = project.assets().sky_map(source);
                            }
                            if (loaded) {
                                map = loaded->map;
                                session.unavailable_.erase(std::pair{assets::AssetKind::sky, source});
                            } else
                                missing = &session.remember_unavailable(project.assets(), assets::AssetKind::sky,
                                                                        source, loaded.error());
                        }
                        take(presentation::set_sky_map_binding(runtime, edited, {source, std::move(map)}));
                        // The document keeps the same key unresolved; Play reports it without diverging.
                        if (missing)
                            note(world::Severity::warning, missing->kind, missing->error.message, &missing->error);
                    }
                    if (const auto at = *op.find("space")) {
                        if (!entity)
                            throw std::runtime_error("Only an Entity has a space.");
                        if (at->is_null())
                            take(world.remove_transform(edited));
                        else {
                            ATELIER_TRY(at->allowed_members({"position", "rotation", "scale", "origin"}));
                            const bool spatial = take(world::has_transform(runtime, edited));
                            auto trs = spatial ? take(world::read_transform(runtime, edited)) : world::Trs{};
                            if (const auto v = *at->find("position")) {
                                ATELIER_TRY_BIND(position, vector(*v));
                                trs.position = *position;
                            }
                            if (const auto v = *at->find("rotation")) {
                                ATELIER_TRY_BIND(q, assets::json_format::vector<4>(*v));
                                trs.rotation = {(*q)[0], (*q)[1], (*q)[2], (*q)[3]};
                            }
                            if (const auto v = *at->find("scale")) {
                                ATELIER_TRY_BIND(scale, vector(*v));
                                trs.scale = *scale;
                            }
                            trs = take(world::canonical_transform(trs));
                            if (const auto origin = *at->find("origin")) {
                                std::optional<ObjectId> reference;
                                if (!origin->is_null()) {
                                    ATELIER_TRY_BIND(key, local_key(*origin, true));
                                    reference = *key ? live_reference(session, *key, load) : std::optional(ObjectId{});
                                }
                                if (!spatial)
                                    relocated(world.set_transform(edited, trs));
                                commit(world.set_space_reference(edited, reference, world::ReparentRule::keep_local));
                            }
                            relocated(world.set_transform(edited, trs));
                        }
                    }
                } else if (kind == "move") {
                    ATELIER_TRY(op.members({"op", "id", "parent", "index", "space"}));
                    ATELIER_TRY_BIND(moved, target(*op.at("id")));
                    ObjectId parent;
                    const auto parent_value = *op.at("parent");
                    if (!parent_value.is_null()) {
                        ATELIER_TRY_BIND(found, live_target(session, parent_value));
                        parent = *found;
                    }
                    ATELIER_TRY_BIND(position, live_index(session, *op.at("index"), parent, *moved));
                    commit(world.reparent(*moved, parent, world::ReparentRule::keep_world, *position));
                    if (const auto space = *op.at("space"); !space.is_null()) {
                        ATELIER_TRY(space.allowed_members({"position", "rotation", "scale", "origin"}));
                        ATELIER_TRY_BIND(trs, json_trs(space));
                        relocated(world.set_transform(*moved, *trs));
                    }
                } else if (kind == "materials") {
                    ATELIER_TRY(op.members({"op", "materials"}));
                    ATELIER_TRY_BIND(definitions, op.at("materials")->array());
                    if (definitions->size() > maximum_entities * maximum_components)
                        throw std::runtime_error("Expected a material definition list.");
                    std::unordered_map<std::uint64_t, json::Value> records;
                    for (auto definition : *definitions) {
                        ATELIER_TRY(definition.allowed_members({"id", "source", "parent", "entries"}));
                        ATELIER_TRY_BIND(id, definition.at("id"));
                        ATELIER_TRY_BIND(key, local_key(*id));
                        if (!records.emplace(*key, definition).second)
                            throw std::runtime_error("Duplicate Live material identity.");
                    }
                    std::unordered_set<std::uint64_t> visiting;
                    std::unordered_set<std::uint64_t> complete;
                    std::function<json::Result<ObjectRoot>(std::uint64_t)> restore;
                    restore = [&](std::uint64_t key) -> json::Result<ObjectRoot> {
                        const auto live = live_object(session, key);
                        if (live && *live != ObjectId{} && !session.is_material(*live))
                            throw std::runtime_error("Document identity #" + std::to_string(key) +
                                                     " is not a material in Play.");
                        const auto record = records.find(key);
                        if (record == records.end() || complete.contains(key)) {
                            if (!live || *live == ObjectId{})
                                throw std::runtime_error("Unknown Live material parent.");
                            return take(runtime.hold(*live));
                        }
                        if (!visiting.insert(key).second)
                            throw std::runtime_error("Live material parent cycle.");
                        // As in a tolerant load: an unavailable source, parent or texture stands in with a warning.
                        std::vector<MissingResource> missing;
                        ATELIER_TRY_BIND(restored, restore_material(session, &project, record->second, restore,
                                                                    &missing, live.value_or(ObjectId{})));
                        for (const auto& item : missing)
                            note(world::Severity::warning, item.kind, item.error.message, &item.error);
                        if (!live || *live == ObjectId{}) {
                            if (!session.local_ids_.emplace(restored->id(), key).second)
                                throw std::runtime_error("One material asset has multiple document identities.");
                            session.live_objects_.insert_or_assign(key, restored->id());
                            session.live_materials_.push_back(*restored);
                        }
                        visiting.erase(key);
                        complete.insert(key);
                        return restored;
                    };
                    for (auto definition : *definitions) {
                        ATELIER_TRY(restore(*local_key(*definition.at("id"))));
                    }
                } else if (kind == "texture") {
                    ATELIER_TRY(op.members({"op", "key", "width", "height", "file"}));
                    ATELIER_TRY_BIND(key_value, op.string({"key"}));
                    const auto key = std::string(*key_value);
                    subject += " " + key;
                    ATELIER_TRY_BIND(width_value, op.u64({"width"}));
                    ATELIER_TRY_BIND(height_value, op.u64({"height"}));
                    const auto width = *width_value;
                    const auto height = *height_value;
                    if (!width || !height || width > 8192 || height > 8192)
                        throw std::runtime_error("Invalid Play texture dimensions.");
                    const auto bytes = width * height * 4;
                    ATELIER_TRY_BIND(file, op.string({"file"}));
                    const auto rgba = assets::read_file(platform::utf8_to_utf16_validated(*file), bytes);
                    if (!rgba) {
                        note(world::Severity::error, world::DiagnosticKind::command_rejected, rgba.error().message,
                             &rgba.error());
                        return {};
                    }
                    if (rgba->size() != bytes)
                        throw std::runtime_error("Play texture RGBA byte count does not match its dimensions.");
                    auto image =
                        material::Image::create(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                                std::vector<std::uint8_t>(rgba->begin(), rgba->end()));
                    if (!image) {
                        note(world::Severity::error, world::DiagnosticKind::command_rejected, image.error().message);
                        return {};
                    }
                    take(session.publish_texture(key, std::move(*image)));
                } else if (kind == "mesh") {
                    ATELIER_TRY(op.members({"op", "key", "file"}));
                    ATELIER_TRY_BIND(key_value, op.string({"key"}));
                    const auto key = std::string(*key_value);
                    subject += " " + key;
                    ATELIER_TRY_BIND(file, op.string({"file"}));
                    const auto bytes =
                        assets::read_file(platform::utf8_to_utf16_validated(*file), assets::mesh_format::maximum_bytes);
                    if (!bytes) {
                        note(world::Severity::error, world::DiagnosticKind::command_rejected, bytes.error().message,
                             &bytes.error());
                        return {};
                    }
                    auto mesh = assets::mesh_format::decode(*bytes);
                    if (!mesh) {
                        note(world::Severity::error, world::DiagnosticKind::command_rejected, mesh.error().message);
                        return {};
                    }
                    if (mesh->key != key)
                        throw std::runtime_error("Play mesh content does not match its asset key.");
                    take(project.refresh());
                    take(session.publish_mesh(project.assets(), *mesh));
                } else if (kind == "resource") {
                    ATELIER_TRY(op.allowed_members({"op", "key", "material", "source"}));
                    ATELIER_TRY_BIND(key_value, op.string({"key"}));
                    const auto key = std::string(*key_value);
                    subject += " " + key;
                    if (key.empty())
                        throw std::runtime_error("Live asset publication requires an asset key.");
                    ATELIER_TRY_BIND(material, op.at("material"));
                    ATELIER_TRY_BIND(asset, assets::material_format::decode(*material));
                    asset->key = key;
                    std::optional<source::SourceFile> exact_source;
                    if (const auto input = *op.find("source")) {
                        ATELIER_TRY(input->members({"path", "text"}));
                        ATELIER_TRY_BIND(text, input->string({"text"}));
                        if (text->size() > (1u << 20))
                            throw std::runtime_error("Invalid Live Material Source text.");
                        ATELIER_TRY_BIND(path, input->string({"path"}));
                        exact_source =
                            source::SourceFile{std::string(*path), source::content_hash(*text), std::string(*text)};
                    }
                    if (std::holds_alternative<assets::MaterialSourceAsset>(asset->author) && !exact_source)
                        throw std::runtime_error("Live Material Source publication requires its exact text.");
                    // Refresh only the catalogue for newly created dependencies;
                    // published pixels and Source text above remain authoritative.
                    take(project.refresh());
                    std::vector<MissingResource> missing;
                    take(session.publish_material(project.assets(), *asset, std::move(exact_source), &missing));
                    for (const auto& item : missing)
                        note(world::Severity::warning, item.kind, item.error.message, &item.error);
                } else
                    throw std::runtime_error("Unknown change: " + std::string(kind));
                for (const auto key : load.dropped)
                    note(world::Severity::warning, world::DiagnosticKind::reference_dropped,
                         "#" + std::to_string(key) + " was destroyed by the game; the reference is null.");
                return {};
            }();
            if (!applied)
                note(world::Severity::error, world::DiagnosticKind::command_rejected, applied.error().describe());
        } catch (const Gone& error) {
            note(world::Severity::warning, world::DiagnosticKind::reference_dropped,
                 std::string(error.what()) + " The change is skipped.");
        } catch (const Unsupported& error) {
            note(world::Severity::warning, world::DiagnosticKind::command_rejected, error.error.message, &error.error);
        } catch (const detail::Failure& error) {
            note(world::Severity::error, world::DiagnosticKind::command_rejected, error.error.message, &error.error);
        } catch (const std::exception& error) {
            note(world::Severity::error, world::DiagnosticKind::command_rejected, error.what());
        }
    }
};

Result<std::string> Session::serialize() const {
    try {
        if (editing())
            throw std::runtime_error("Finish the active edit before saving.");
        std::vector<ObjectId> roots;
        for (const auto& member : take(world_.members()))
            if (take(world::parent_entity(world_.runtime(), member.id())) == ObjectId{})
                roots.push_back(member.id());
        ATELIER_TRY_BIND(graph, GraphCodec::capture(*this, roots, true, true));
        const auto text = json::write(graph->data.root(), {true, true});
        if (text.size() > assets::json_format::maximum_bytes)
            throw std::runtime_error("Scene exceeds the 16 MiB file limit.");
        return std::string(text.data(), text.size());
    } catch (const std::exception& error) {
        return failed(error);
    }
}

Result<std::string> Session::serialize_subtree(ObjectId root) const {
    try {
        if (editing())
            throw std::runtime_error("Finish the active edit before saving a composition.");
        ATELIER_TRY_BIND(graph, GraphCodec::capture(*this, {&root, 1}, true));
        const auto text = json::write(graph->data.root(), {true, true});
        if (text.size() > assets::json_format::maximum_bytes)
            throw std::runtime_error("Composition exceeds the 16 MiB file limit.");
        return std::string(text.data(), text.size());
    } catch (const std::exception& error) {
        return failed(error);
    }
}

Result<std::string> Session::serialize_live_subtree(ObjectId root) const {
    try {
        ATELIER_TRY_BIND(graph, GraphCodec::capture(*this, {&root, 1}, true, false, true));
        const auto text = json::write(graph->data.root());
        if (text.size() > assets::json_format::maximum_bytes)
            throw std::runtime_error("The Entity exceeds the 16 MiB change limit.");
        return std::string(text.data(), text.size());
    } catch (const std::exception& error) {
        return failed(error);
    }
}

Result<std::string> Session::serialize_live_components(ObjectId entity, std::span<const ObjectId> components) const {
    try {
        ATELIER_TRY_BIND(graph, GraphCodec::live_components(*this, entity, components));
        const auto text = json::write(graph->root());
        if (text.size() > assets::json_format::maximum_bytes)
            throw std::runtime_error("The Components exceed the 16 MiB change limit.");
        return std::string(text.data(), text.size());
    } catch (const std::exception& error) {
        return failed(error);
    }
}

void Session::set_live_forwarding(bool enabled) {
    forwarding_ = enabled;
    live_ops_.clear();
    live_failure_.reset();
    live_leaves_.clear();
}

Result<std::vector<LiveChange>> Session::take_live_ops() {
    if (live_failure_)
        return std::unexpected(*live_failure_);
    std::vector<LiveChange> result;
    result.reserve(live_ops_.size());
    for (auto& op : live_ops_)
        result.push_back(std::move(op.change));
    live_ops_.clear();
    return result;
}

void Session::forward_change(const Change& change, bool forward) {
    if (!forwarding_ || live_failure_)
        return;
    using C = GraphCodec;
    const auto push = [&](json::Value op) { C::live_push(*this, {}, op); };
    const auto set = [&](ObjectId object, const char* name, const auto& value) -> Result<void> {
        json::Builder op;
        op.set_validated(op.root(), "op", "set");
        ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, object)));
        op.set_validated(op.root(), "id", *id);
        ATELIER_TRY(scene_result(op.set(op.root(), name, value)));
        push(op.root());
        return {};
    };
    // A commit also sends every leaf its previews sent, including leaves that returned to their start.
    const auto leaves = [&](ObjectId entity, const world::Trs& before, const world::Trs& after) -> Result<void> {
        auto mask = C::leaf_mask(before, after);
        if (const auto at = live_leaves_.find(entity); at != live_leaves_.end()) {
            mask |= at->second;
            live_leaves_.erase(at);
        }
        if (mask) {
            ATELIER_TRY_BIND(space, scene_result(C::space_leaves(take(world::read_transform(runtime_, entity)), mask)));
            ATELIER_TRY(set(entity, "space", space->root()));
        }
        return {};
    };
    try {
        auto result = std::visit(
            [&](const auto& item) -> Result<void> {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, EntityChange>) {
                    const auto camera = [&]() -> Result<void> {
                        if (item.cleared_camera) {
                            const auto& edit = *item.cleared_camera;
                            ATELIER_TRY_BIND(op,
                                             scene_result(C::live_properties(*this, edit.root.id(), {&edit.path, 1})));
                            push(op->root());
                        }
                        return {};
                    };
                    if (item.added != forward) {
                        ATELIER_TRY(camera());
                        for (const auto& removal : item.removals) {
                            json::Builder op;
                            op.set_validated(op.root(), "op", "remove");
                            ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, removal.id())));
                            op.set_validated(op.root(), "id", *id);
                            push(op.root());
                        }
                        return {};
                    }
                    struct Add {
                        std::uint64_t parent;

                        std::size_t index;

                        json::Builder op;
                    };
                    std::vector<Add> adds;
                    std::unordered_map<std::string, std::size_t> added_entities;
                    for (const auto& removal : item.removals) {
                        const auto root = removal.id();
                        const auto parent = take(world::parent_entity(runtime_, root));
                        const auto index = C::live_position(*this, root, parent);
                        ATELIER_TRY_BIND(captured, C::capture(*this, {&root, 1}, true, false, true));
                        json::Builder graph(captured->data);
                        const auto entities = *graph.root().at("entities")->array();
                        for (auto entity : entities)
                            added_entities.emplace(*entity.string({"id"}), adds.size());
                        // This transaction restores its camera reference after all roots.
                        // An earlier Environment add must not refer to the still-missing target.
                        if (item.cleared_camera) {
                            ATELIER_TRY_BIND(component,
                                             scene_result(C::live_id(*this, item.cleared_camera->root.id())));
                            for (auto entity : entities) {
                                const auto components = *entity.at("components")->array();
                                for (auto part : components)
                                    if (*part.string({"id"}) == *component) {
                                        ATELIER_TRY_BIND(properties, scene_result(part.at("properties")));
                                        ATELIER_TRY(scene_result(graph.set_null(*properties, "main_camera")));
                                    }
                            }
                        }
                        json::Builder op;
                        op.set_validated(op.root(), "op", "add");
                        if (parent == ObjectId{})
                            ATELIER_TRY(scene_result(op.set_null(op.root(), "parent")));
                        else {
                            ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, parent)));
                            op.set_validated(op.root(), "parent", *id);
                        }
                        ATELIER_TRY(scene_result(op.set(op.root(), "index", std::uint64_t(index))));
                        op.set_validated(op.root(), "graph", graph.root());
                        std::uint64_t parent_key{};
                        if (parent != ObjectId{}) {
                            ATELIER_TRY_BIND(id, scene_result(C::identity(*this, parent)));
                            parent_key = *id;
                        }
                        adds.push_back({parent_key, index, std::move(op)});
                    }
                    // Import and Redo also own references inside their incoming forest.
                    // Defer only main_camera across its roots; never resend an existing
                    // Environment or pull a target outside this batch back into Play.
                    std::optional<json::Builder> imported_camera;
                    if (const auto env = environment(); env != ObjectId{}) {
                        ATELIER_TRY_BIND(component, scene_result(C::live_id(*this, env)));
                        for (std::size_t i = 0; i < adds.size(); ++i) {
                            auto& op = adds[i].op;
                            const auto entities = *op.root().path({"graph", "entities"})->array();
                            for (auto entity : entities) {
                                const auto components = *entity.at("components")->array();
                                for (auto part : components)
                                    if (*part.string({"id"}) == *component) {
                                        const auto properties = *part.at("properties");
                                        if (properties.kind() != json::Kind::object)
                                            continue; // A retained malformed record may replace properties.
                                        const auto reference = *properties.find("main_camera");
                                        if (!reference || reference->kind() != json::Kind::object)
                                            continue;
                                        if (const auto target = *reference->find("$ref"))
                                            if (const auto at = added_entities.find(std::string(*target->string()));
                                                at != added_entities.end() && at->second != i) {
                                                imported_camera.emplace();
                                                auto& deferred = *imported_camera;
                                                deferred.set_validated(deferred.root(), "op", "set");
                                                deferred.set_validated(deferred.root(), "id", *component);
                                                const auto props =
                                                    deferred.object_validated(deferred.root(), "properties");
                                                deferred.set_validated(props, "main_camera", *reference);
                                                ATELIER_TRY(scene_result(op.set_null(properties, "main_camera")));
                                            }
                                    }
                            }
                        }
                    }
                    std::ranges::sort(adds, {}, [](const Add& add) { return std::pair{add.parent, add.index}; });
                    for (const auto& add : adds)
                        push(add.op.root());
                    if (imported_camera)
                        push(imported_camera->root());
                    ATELIER_TRY(camera());
                } else if constexpr (std::is_same_v<T, ComponentChange>) {
                    const auto owner = item.removal.owner;
                    const auto component = item.removal.id();
                    if (item.added == forward) {
                        if (item.created_transform) {
                            ATELIER_TRY_BIND(space, scene_result(C::live_space(*this, owner)));
                            ATELIER_TRY(set(owner, "space", space->root()));
                        }
                        std::size_t index{};
                        for (const auto& part : take(world::components(runtime_, owner))) {
                            if (part.id() == component)
                                break;
                            ++index;
                        }
                        ATELIER_TRY_BIND(entity, scene_result(C::live_id(*this, owner)));
                        ATELIER_TRY_BIND(graph, C::live_components(*this, owner, {&component, 1}));
                        json::Builder op;
                        op.set_validated(op.root(), "op", "attach");
                        op.set_validated(op.root(), "entity", *entity);
                        ATELIER_TRY(scene_result(op.set(op.root(), "index", std::uint64_t(index))));
                        op.set_validated(op.root(), "graph", graph->root());
                        push(op.root());
                    } else {
                        json::Builder op;
                        op.set_validated(op.root(), "op", "remove");
                        ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, component)));
                        op.set_validated(op.root(), "id", *id);
                        push(op.root());
                        if (item.created_transform) {
                            const json::Builder null(json::Kind::null);
                            ATELIER_TRY(set(owner, "space", null.root()));
                        }
                    }
                } else if constexpr (std::is_same_v<T, FieldChange>) {
                    if (const auto* after = std::get_if<world::Trs>(&item.after))
                        ATELIER_TRY(leaves(item.root.id(), std::get<world::Trs>(item.before), *after));
                    else {
                        ATELIER_TRY_BIND(op, scene_result(C::live_properties(*this, item.root.id(), {&item.path, 1})));
                        push(op->root());
                    }
                } else if constexpr (std::is_same_v<T, ReferenceFieldChange>) {
                    ATELIER_TRY_BIND(op, scene_result(C::live_properties(*this, item.root.id(), {&item.path, 1})));
                    push(op->root());
                } else if constexpr (std::is_same_v<T, FieldGroupChange>) {
                    ATELIER_TRY_BIND(op, scene_result(C::live_properties(*this, item.root.id(), item.paths)));
                    push(op->root());
                } else if constexpr (std::is_same_v<T, TransformChange>) {
                    for (const auto& entry : item.entries)
                        ATELIER_TRY(leaves(entry.root.id(), entry.before, entry.after));
                } else if constexpr (std::is_same_v<T, NameChange>) {
                    auto text = platform::utf16_to_utf8(name(item.root.id()));
                    if (!text)
                        return scene_result(json::Result<void>{
                            std::unexpected(json::Error{json::Fault::encoding, "Invalid UTF-16 text.", {}, {}})});
                    ATELIER_TRY(set(item.root.id(), "name", *text));
                } else if constexpr (std::is_same_v<T, BodyChange>) {
                    ATELIER_TRY_BIND(op, scene_result(C::live_body(*this, item.root.id())));
                    push(op->root());
                } else if constexpr (std::is_same_v<T, TransformPresenceChange>) {
                    ATELIER_TRY_BIND(space, scene_result(C::live_space(*this, item.root.id())));
                    ATELIER_TRY(set(item.root.id(), "space", space->root()));
                } else if constexpr (std::is_same_v<T, SpaceReferenceChange>) {
                    const auto entity = item.root.id();
                    json::Builder space;
                    if (item.spatial) {
                        ATELIER_TRY_BIND(value,
                                         scene_result(C::space_leaves(take(world::read_transform(runtime_, entity)),
                                                                      C::leaf_mask(item.before, item.after))));
                        space.replace(space.root(), value->root());
                    }
                    ATELIER_TRY_BIND(origin, scene_result(C::live_origin(*this, entity)));
                    space.set_validated(space.root(), "origin", origin->root());
                    ATELIER_TRY(set(entity, "space", space.root()));
                } else if constexpr (std::is_same_v<T, ReparentChange>) {
                    for (std::size_t i = 0; i < item.moves.size(); ++i) {
                        const auto& move = item.moves[forward ? i : item.moves.size() - 1 - i];
                        const auto root = move.root.id();
                        const auto parent = forward ? move.after_parent : move.before_parent;
                        const auto transform = std::ranges::find_if(
                            move.transforms, [&](const auto& entry) { return entry.root.id() == root; });
                        json::Builder op;
                        op.set_validated(op.root(), "op", "move");
                        ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, root)));
                        op.set_validated(op.root(), "id", *id);
                        if (parent == ObjectId{})
                            ATELIER_TRY(scene_result(op.set_null(op.root(), "parent")));
                        else {
                            ATELIER_TRY_BIND(parent_id, scene_result(C::live_id(*this, parent)));
                            op.set_validated(op.root(), "parent", *parent_id);
                        }
                        ATELIER_TRY(scene_result(
                            op.set(op.root(), "index", std::uint64_t(forward ? move.after_index : move.before_index))));
                        if (transform != move.transforms.end()) {
                            ATELIER_TRY_BIND(space, scene_result(C::space_leaves(
                                                        forward ? transform->after : transform->before, 7)));
                            op.set_validated(op.root(), "space", space->root());
                        } else
                            ATELIER_TRY(scene_result(op.set_null(op.root(), "space")));
                        push(op.root());
                        for (const auto& entry : move.transforms)
                            if (entry.root.id() != root) {
                                ATELIER_TRY_BIND(
                                    space, scene_result(C::space_leaves(forward ? entry.after : entry.before,
                                                                        C::leaf_mask(entry.before, entry.after))));
                                ATELIER_TRY(set(entry.root.id(), "space", space->root()));
                            }
                    }
                } else if constexpr (std::is_same_v<T, MaterialBindingChange>) {
                    // Play resolves a binding only to a material it already has, so definitions go first.
                    const auto component = item.root.id();
                    const auto bindings = take(presentation::read_material_bindings(runtime_, component));
                    json::Builder op;
                    op.set_validated(op.root(), "op", "materials");
                    const auto definitions = *op.array(op.root(), "materials");
                    json::Builder table(json::Kind::array);
                    std::unordered_set<ObjectId> defined;
                    for (const auto& binding : bindings) {
                        const auto resource = binding.resource.id();
                        ATELIER_TRY(scene_result(C::material_definitions(*this, resource, op, definitions, defined)));
                        const auto entry = table.append_object(table.root());
                        ATELIER_TRY(scene_result(table.set(entry, "slot", binding.slot)));
                        ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, resource)));
                        table.set_validated(entry, "resource", *id);
                        ATELIER_TRY(scene_result(table.set(entry, "overridden", binding.overridden)));
                    }
                    if (definitions.size())
                        push(op.root());
                    ATELIER_TRY(set(component, "materials", table.root()));
                } else if constexpr (std::is_same_v<T, MaterialValueChange>)
                    forward_material(item.root.id());
                else if constexpr (std::is_same_v<T, SkyMapChange>)
                    ATELIER_TRY(set(item.root.id(), "sky_map",
                                    take(presentation::read_sky_map(runtime_, item.root.id())).source));
                else
                    static_assert(std::is_same_v<T, RetainedChange>,
                                  "Every change kind is forwarded to Play or explicitly not.");
                return {};
            },
            change);
        if (!result)
            live_failure_ = std::move(result.error());
    } catch (const std::exception& error) {
        live_failure_ = failed(error).error();
    }
    if (live_failure_)
        live_ops_.clear();
}

void Session::forward_material(ObjectId object) {
    if (!forwarding_ || live_failure_)
        return;
    try {
        const auto encoded = [&]() -> json::Result<void> {
            const auto asset_key = take(material::material_key(runtime_, object));
            json::Builder op;
            if (!asset_key.empty()) {
                op.set_validated(op.root(), "op", "resource");
                ATELIER_TRY(op.set(op.root(), "key", asset_key));
                ATELIER_TRY_BIND(material, assets::material_format::encode(take(material_asset(object))));
                op.set_validated(op.root(), "material", material->root());
                if (const auto at = materials_.find(asset_key); at != materials_.end() && at->second.source_file) {
                    const auto source = op.object_validated(op.root(), "source");
                    ATELIER_TRY(op.set(source, "path", at->second.source_file->path));
                    ATELIER_TRY(op.set(source, "text", at->second.source_file->source));
                }
                GraphCodec::live_push(*this, "resource " + asset_key, op.root());
            } else {
                ATELIER_TRY_BIND(key, GraphCodec::identity(*this, object));
                op.set_validated(op.root(), "op", "materials");
                const auto definitions = *op.array(op.root(), "materials");
                std::unordered_set<ObjectId> defined;
                ATELIER_TRY(GraphCodec::material_definitions(*this, object, op, definitions, defined));
                GraphCodec::live_push(*this, "material " + std::to_string(*key), op.root());
            }
            return {};
        }();
        if (auto result = scene_result(encoded); !result)
            live_failure_ = std::move(result.error());
    } catch (const std::exception& error) {
        live_failure_ = failed(error).error();
    }
    if (live_failure_)
        live_ops_.clear();
}

void Session::forward_texture(std::string_view key, material::ImageRef image) {
    if (!forwarding_ || live_failure_)
        return;
    try {
        if (!image)
            throw std::runtime_error("A Play texture publication requires image content.");
        json::Builder op;
        op.set_validated(op.root(), "op", "texture");
        const auto written = op.set(op.root(), "key", key);
        if (!written) {
            live_failure_ = Error{ErrorCode::invalid_storage, written.error().describe()};
            live_ops_.clear();
            return;
        }
        GraphCodec::live_push(*this, "texture " + std::string(key), op.root(), std::move(image));
    } catch (const std::exception& error) {
        live_failure_ = failed(error).error();
        live_ops_.clear();
    }
}

void Session::forward_mesh(const assets::MeshAsset& asset) {
    if (!forwarding_ || live_failure_)
        return;
    try {
        json::Builder op;
        op.set_validated(op.root(), "op", "mesh");
        const auto written = op.set(op.root(), "key", asset.key);
        if (!written) {
            live_failure_ = Error{ErrorCode::invalid_storage, written.error().describe()};
            live_ops_.clear();
            return;
        }
        GraphCodec::live_push(*this, "mesh " + asset.key, op.root(), {},
                              std::make_shared<const assets::MeshAsset>(asset));
    } catch (const std::exception& error) {
        live_failure_ = failed(error).error();
        live_ops_.clear();
    }
}

Result<void> Session::reimport_mesh(Project& project, std::string_view key, const std::filesystem::path& source) {
    if (world_.closed() || world_.running() || editing())
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Finish the active edit and stop the editor World before reimporting."});
    ATELIER_TRY_BIND(publication, project.reimport_mesh(key, source));
    for (const auto& texture : publication->textures)
        ATELIER_TRY(publish_texture(texture.key, texture.image));
    // Existing documents may have unsaved author edits. Importing geometry
    // must not replace those edits with the material file's saved values.
    std::vector<MissingResource> missing;
    for (const auto& asset : publication->materials) {
        ATELIER_TRY_BIND(root, material_resource(project.assets(), asset, &missing));
        forward_material(root->id());
    }
    return publish_mesh(project.assets(), publication->mesh);
}

Result<void> Session::publish_mesh(assets::Reader& reader, const assets::MeshAsset& asset) {
    if (world_.closed() || asset.key.empty() || !asset.mesh)
        return std::unexpected(Error{ErrorCode::invalid_storage,
                                     "Mesh publication needs a live Session, asset key and complete geometry."});
    std::vector<MissingResource> missing;
    ATELIER_TRY_BIND(defaults, mesh_defaults(reader, asset, &missing));
    struct Replacement {
        ObjectId object;

        std::vector<presentation::MaterialBinding> active;

        Retained kept;
    };
    std::vector<Replacement> replacements;
    // Include detached history objects with a document identity: later Undo
    // reattaches the current mesh rather than resurrecting an imported version.
    std::unordered_set<ObjectId> components;
    for (const auto& [object, id] : local_ids_) {
        static_cast<void>(id);
        const auto type = runtime_.type_of(object);
        if (!type)
            continue;
        ATELIER_TRY_BIND(matches, runtime_.is_a(*type, presentation_types_.static_mesh_component));
        if (*matches)
            components.insert(object);
    }
    {
        ATELIER_TRY_BIND(members, world_.members());
        for (const auto& entity : *members) {
            ATELIER_TRY_BIND(parts, world::components(runtime_, entity.id()));
            for (const auto& component : *parts) {
                ATELIER_TRY_BIND(type, runtime_.type_of(component.id()));
                ATELIER_TRY_BIND(matches, runtime_.is_a(*type, presentation_types_.static_mesh_component));
                if (*matches)
                    components.insert(component.id());
            }
        }
    }
    for (const auto object : components) {
        {
            ATELIER_TRY_BIND(mesh, presentation::read_static_mesh(runtime_, object));
            if (mesh->mesh.source != asset.key)
                continue;
        }
        ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime_, object));
        Retained kept;
        if (const auto at = retained_.find(object); at != retained_.end())
            kept = at->second;
        auto changed = [&]() -> Result<Replacement> {
            json::Builder record;
            if (!kept.record.empty()) {
                ATELIER_TRY_BIND(
                    parsed, scene_result(json::Document::parse(kept.record, {std::numeric_limits<std::size_t>::max(),
                                                                             json::DepthPolicy::containers, true})));
                record = json::Builder(*parsed);
            }
            if (const auto at = *record.root().find("materials"); at && at->kind() == json::Kind::array) {
                const auto items = *at->array();
                for (auto item : items) {
                    ATELIER_TRY_BIND(slot_value, scene_result(item.string({"slot"})));
                    const auto slot = std::string(*slot_value);
                    if (std::ranges::find(*bindings, slot, &presentation::MaterialBinding::slot) != bindings->end())
                        continue;
                    ATELIER_TRY_BIND(resource, scene_result(item.at("resource")));
                    ATELIER_TRY_BIND(key, scene_result(local_key(*resource)));
                    ATELIER_TRY_BIND(id, local_object(*key));
                    bool overridden = false;
                    if (const auto flag = *item.find("overridden")) {
                        ATELIER_TRY_BIND(value, scene_result(flag->boolean()));
                        overridden = *value;
                    }
                    ATELIER_TRY_BIND(root, runtime_.hold(*id));
                    bindings->push_back({slot, std::move(*root), overridden});
                }
            }
            std::vector<presentation::MaterialBinding> active;
            bool dormant{};
            for (const auto& binding : *bindings) {
                std::size_t matches{};
                for (std::size_t i = 0; i < asset.mesh->materials().size(); ++i)
                    matches += geometry::material_slot_name(*asset.mesh, i) == binding.slot;
                if (matches == 1)
                    active.push_back(binding);
                else
                    dormant = true;
            }
            kept.materials.clear();
            if (dormant) {
                const auto table = *record.array(record.root(), "materials");
                for (const auto& binding : *bindings) {
                    const auto entry = record.append_object(table);
                    ATELIER_TRY(scene_result(record.set(entry, "slot", binding.slot)));
                    ATELIER_TRY_BIND(id, scene_result(GraphCodec::identity(*this, binding.resource.id())));
                    record.set_validated(entry, "resource", std::to_string(*id));
                    ATELIER_TRY(scene_result(record.set(entry, "overridden", binding.overridden)));
                    kept.materials.push_back(binding.resource);
                }
            } else
                record.erase(record.root(), "materials");
            kept.record = dump_retained(record.root());
            return Replacement{object, std::move(active), std::move(kept)};
        }();
        if (!changed)
            return std::unexpected(std::move(changed.error()));
        replacements.push_back(std::move(*changed));
    }
    ATELIER_TRY(reader.publish_mesh_content(asset));
    for (auto& replacement : replacements) {
        ATELIER_TRY(presentation::set_mesh_binding(runtime_, replacement.object, {asset.key, asset.mesh, *defaults}));
        ATELIER_TRY(presentation::set_material_bindings(runtime_, replacement.object, replacement.active));
        if (replacement.kept.empty())
            retained_.erase(replacement.object);
        else
            retained_.insert_or_assign(replacement.object, std::move(replacement.kept));
    }
    unavailable_.erase(std::pair{assets::AssetKind::mesh, asset.key});
    ++revision_;
    forward_mesh(asset);
    return {};
}

void Session::forward_edit() {
    if (!forwarding_ || live_failure_)
        return;
    using C = GraphCodec;
    // One op per TRS leaf, so a later preview never drops a leaf an earlier op carried.
    const auto space = [&](ObjectId entity, const world::Trs& before) -> json::Result<void> {
        const auto current = take(world::read_transform(runtime_, entity));
        auto& sent = live_leaves_[entity];
        sent |= C::leaf_mask(before, current);
        ATELIER_TRY_BIND(id, C::live_id(*this, entity));
        for (const unsigned leaf : {1u, 2u, 4u})
            if (sent & leaf) {
                ATELIER_TRY_BIND(value, C::space_leaves(current, leaf));
                json::Builder op;
                op.set_validated(op.root(), "op", "set");
                op.set_validated(op.root(), "id", *id);
                op.set_validated(op.root(), "space", value->root());
                C::live_push(*this, "space " + *id + " " + std::to_string(leaf), op.root());
            }
        return {};
    };
    const auto fields = [&](ObjectId object, std::span<const world::FieldPath> paths) -> json::Result<void> {
        std::string key;
        ATELIER_TRY_BIND(op, C::live_properties(*this, object, paths, &key));
        C::live_push(*this, std::move(key), op->root());
        return {};
    };
    try {
        const auto encoded = [&]() -> json::Result<void> {
            if (material_edit_)
                forward_material(material_edit_->root.id());
            if (group_)
                ATELIER_TRY(fields(group_->root.id(), group_->paths));
            if (transform_)
                for (const auto& entry : transform_->entries)
                    ATELIER_TRY(space(entry.root.id(), entry.before));
            if (active_) {
                if (const auto* before = std::get_if<world::Trs>(&active_->before))
                    ATELIER_TRY(space(active_->root.id(), *before));
                else
                    ATELIER_TRY(fields(active_->root.id(), {&active_->path, 1}));
            }
            return {};
        }();
        if (auto result = scene_result(encoded); !result)
            live_failure_ = std::move(result.error());
    } catch (const std::exception& error) {
        live_failure_ = failed(error).error();
    }
    if (live_failure_)
        live_ops_.clear();
}

void Session::enter_play() {
    play_ = true;
    for (const auto& [object, key] : local_ids_)
        if (runtime_.type_of(object))
            live_objects_.insert_or_assign(key, object);
}

Result<std::vector<world::Diagnostic>> Session::apply_live(std::string_view text, Project& project) {
    try {
        if (!play_)
            throw std::runtime_error("Editor changes apply only to a Play copy.");
        if (world_.closed())
            throw std::runtime_error("The Play World is closed.");
        const auto batch = assets::json_format::parse(text);
        if (!batch)
            return std::unexpected(Error{ErrorCode::invalid_storage, batch.error().describe()});
        const auto fields = batch->root().members({"ops"});
        if (!fields)
            return std::unexpected(Error{ErrorCode::invalid_storage, fields.error().describe()});
        const auto ops = batch->root().at("ops")->array();
        if (!ops)
            return std::unexpected(Error{ErrorCode::invalid_storage, ops.error().describe()});
        std::vector<world::Diagnostic> result;
        std::size_t index = 0;
        for (auto op : *ops)
            GraphCodec::apply_change(*this, project, op, index++, result);
        return result;
    } catch (const std::exception& error) {
        return failed(error);
    }
}

Result<std::unique_ptr<Session>> Session::deserialize(std::string_view text, Project& project, Prepare prepare,
                                                      LoadPolicy policy, SessionRole role) {
    try {
        GraphCodec::Graph graph;
        ATELIER_TRY_BIND(parsed, scene_result(assets::json_format::parse(text)));
        graph.data = std::move(*parsed);
        auto session = std::make_unique<Session>();
        ATELIER_TRY(session->initialize(std::move(prepare), role));
        GraphCodec::Load load;
        load.policy = policy;
        load.document = true;
        ATELIER_TRY_BIND(built, scene_result(GraphCodec::construct(*session, graph, &project, true, load)));
        auto& prepared = *built;
        for (const auto& [object, label] : prepared.labels)
            session->names_.emplace(object, label);
        if (!prepared.roots.empty()) {
            const auto plans = GraphCodec::attachments(prepared, graph, {}, false);
            GraphCodec::prepare_cook(*session, prepared, plans, load);
            const auto commit = take(session->world_.attach_batch(plans, prepared.spaces));
            prepared.published = commit.changed;
            if (commit.notification_error)
                throw Failure(*commit.notification_error);
        }
        prepared.published = true;
        session->retained_ = std::move(load.retained);
        session->retained_document_.record = dump_retained(load.top.root());
        session->retained_document_.materials = std::move(load.unreferenced);
        session->load_issues_ = std::move(load.issues);
        session->next_name_ = graph.data.root().at("entities")->size() + 1;
        return session;
    } catch (const std::exception& error) {
        return failed(error);
    }
}

Result<world::Commit> Session::instantiate_subtree(std::string_view text, Project& project, ObjectId parent) {
    try {
        if (editing() || world_.running())
            throw std::runtime_error("Composition import requires a stopped document without an active edit.");
        if (parent != ObjectId{})
            take(admit_entity_edit(parent));
        GraphCodec::Graph graph;
        ATELIER_TRY_BIND(parsed, scene_result(assets::json_format::parse(text)));
        graph.data = std::move(*parsed);
        GraphCodec::Load load;
        ATELIER_TRY_BIND(prepared, scene_result(GraphCodec::construct(*this, graph, &project, false, load)));
        if (prepared->roots.empty())
            return world::Commit{};
        return GraphCodec::publish(*this, *prepared, graph, parent, false, load);
    } catch (const std::exception& error) {
        return failed(error);
    }
}

Result<world::Commit> Session::duplicate_entities(std::span<const ObjectId> entities) {
    try {
        if (editing())
            throw std::runtime_error("Finish the active edit before duplicating.");
        if (entities.empty())
            return world::Commit{};
        ATELIER_TRY_BIND(graph, GraphCodec::capture(*this, entities, false));
        GraphCodec::Load load;
        ATELIER_TRY_BIND(built, scene_result(GraphCodec::construct(*this, *graph, nullptr, false, load)));
        auto& prepared = *built;
        for (auto root : prepared.roots) {
            const auto original = prepared.labels.at(root);
            for (std::size_t suffix = 1;; ++suffix) {
                const auto tail = L" Copy" + (suffix == 1 ? std::wstring{} : L" " + std::to_wstring(suffix));
                auto prefix = original.substr(0, 256 - tail.size());
                if (!prefix.empty() && prefix.back() >= 0xd800 && prefix.back() <= 0xdbff)
                    prefix.pop_back();
                auto label = prefix + tail;
                const auto matches = [&](const auto& item) { return item.first != root && item.second == label; };
                if (std::ranges::none_of(names_, matches) && std::ranges::none_of(prepared.labels, matches)) {
                    take(validate_name(label));
                    prepared.labels[root] = std::move(label);
                    break;
                }
            }
        }
        return GraphCodec::publish(*this, prepared, *graph, {}, true, load);
    } catch (const std::exception& error) {
        return failed(error);
    }
}

std::vector<Session::RetainedSwap> Session::release(const Change& change) {
    std::vector<RetainedSwap> swaps;
    if (retained_.empty() && retained_document_.record.empty())
        return swaps;
    const auto entry_of = [&](ObjectId object) -> Retained* {
        if (object == ObjectId{})
            return &retained_document_;
        const auto at = retained_.find(object);
        return at == retained_.end() ? nullptr : &at->second;
    };
    const auto drop = [&](ObjectId object, std::span<const std::string> path) {
        auto* entry = entry_of(object);
        if (!entry || entry->record.empty())
            return;
        const auto parsed = json::Document::parse(
            entry->record, {std::numeric_limits<std::size_t>::max(), json::DepthPolicy::containers, true});
        if (!parsed)
            fatal("Invalid retained JSON produced by the Scene writer.");
        json::Builder record(*parsed);
        if (!erase_path(record, record.root(), path))
            return;
        auto after = *entry;
        after.record = dump_retained(record.root());
        swaps.push_back({object, *entry, after});
        if (object != ObjectId{} && after.empty())
            retained_.erase(object);
        else
            *entry = std::move(after);
    };
    const auto drop_slots = [&](ObjectId object) {
        auto* entry = entry_of(object);
        if (!entry || entry->slots.empty())
            return;
        auto after = *entry;
        after.slots.clear();
        swaps.push_back({object, *entry, after});
        if (object != ObjectId{} && after.empty())
            retained_.erase(object);
        else
            *entry = std::move(after);
    };
    // Business field paths map directly to the properties record.
    const auto field = [&](ObjectId object, std::span<const FieldId> path) {
        if (!retained_.contains(object))
            return;
        const auto type = runtime_.type_of(object);
        if (!type || path.empty())
            return;
        std::vector<std::string> names{"properties"};
        auto owner = *type;
        for (const auto id : path) {
            const auto info = runtime_.describe(owner);
            if (!info)
                return;
            const auto at = std::ranges::find((*info)->fields, id, &FieldInfo::id);
            if (at == (*info)->fields.end())
                return;
            names.emplace_back(at->name);
            owner = at->value.type;
        }
        drop(object, names);
    };
    static const std::array<std::string, 1> space{"space"};
    static const std::array<std::string, 1> name{"name"};
    static const std::array<std::string, 1> sky{"sky_map"};
    static const std::array<std::string, 2> body{"properties", "body"};
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, FieldChange>) {
                if (std::holds_alternative<world::Trs>(item.after))
                    drop(item.root.id(), space);
                else
                    field(item.root.id(), item.path);
            } else if constexpr (std::is_same_v<T, FieldGroupChange>) {
                for (const auto& path : item.paths)
                    field(item.root.id(), path);
            } else if constexpr (std::is_same_v<T, ReferenceFieldChange>)
                field(item.root.id(), item.path);
            else if constexpr (std::is_same_v<T, EntityChange>) {
                if (item.cleared_camera)
                    field(item.cleared_camera->root.id(), item.cleared_camera->path);
            } else if constexpr (std::is_same_v<T, NameChange>)
                drop(item.root.id(), name);
            else if constexpr (std::is_same_v<T, BodyChange>)
                drop(item.root.id(), body);
            else if constexpr (std::is_same_v<T, TransformChange>) {
                for (const auto& entry : item.entries)
                    drop(entry.root.id(), space);
            } else if constexpr (std::is_same_v<T, ReparentChange>) {
                for (const auto& move : item.moves) {
                    drop(move.root.id(), space);
                    for (const auto& entry : move.transforms)
                        drop(entry.root.id(), space);
                }
            } else if constexpr (std::is_same_v<T, TransformPresenceChange> || std::is_same_v<T, SpaceReferenceChange>)
                drop(item.root.id(), space);
            else if constexpr (std::is_same_v<T, MaterialBindingChange>)
                drop_slots(item.root.id());
            else if constexpr (std::is_same_v<T, SkyMapChange>)
                drop(item.root.id(), sky);
        },
        change);
    return swaps;
}

void Session::restore(std::span<const RetainedSwap> swaps, bool forward) {
    const auto apply = [&](const RetainedSwap& swap) {
        const auto& value = forward ? swap.after : swap.before;
        if (swap.object == ObjectId{})
            retained_document_ = value;
        else if (value.empty())
            retained_.erase(swap.object);
        else
            retained_[swap.object] = value;
    };
    if (forward)
        for (const auto& swap : swaps)
            apply(swap);
    else
        for (auto at = swaps.rbegin(); at != swaps.rend(); ++at)
            apply(*at);
}

Result<void> Session::discard_retained(ObjectId object) {
    const auto reject = [](const char* message) {
        return std::unexpected(Error{ErrorCode::invalid_definition, message});
    };
    if (editing())
        return reject("Finish the active edit before discarding saved data.");
    if (world_.running())
        return reject("Stop the World before discarding saved data.");
    Retained before;
    if (object == ObjectId{})
        before = retained_document_;
    else {
        ATELIER_TRY(admit_property_edit(object));
        const auto at = retained_.find(object);
        if (at == retained_.end())
            return {};
        before = at->second;
    }
    if (before.record.empty() && before.slots.empty() && before.materials.empty())
        return {};
    Retained after;
    after.type = before.type;
    RetainedSwap swap{object, std::move(before), std::move(after)};
    restore({&swap, 1}, true);
    remember(RetainedChange{});
    history_.back().retained.push_back(std::move(swap));
    return {};
}

bool Session::has_retained(ObjectId object) const noexcept {
    if (object == ObjectId{})
        return !retained_document_.record.empty() || !retained_document_.materials.empty();
    return retained_.contains(object);
}

std::size_t Session::placeholder_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(retained_, [&](const auto& item) {
        return !item.second.type.empty() && runtime_.type_of(item.first).has_value();
    }));
}

std::optional<std::uint64_t> Session::document_identity(ObjectId object) const {
    const auto at = local_ids_.find(object);
    return at == local_ids_.end() ? std::nullopt : std::optional(at->second);
}

Result<RetainedView> Session::retained(ObjectId object) const {
    try {
        RetainedView view;
        const Retained* entry = &retained_document_;
        if (object != ObjectId{}) {
            const auto at = retained_.find(object);
            if (at == retained_.end())
                return view;
            entry = &at->second;
        }
        view.type = entry->type;
        const bool document = object == ObjectId{};
        const bool material = !document && is_material(object);
        std::vector<detail::ScalarField> schema;
        if (!document && !material)
            schema = business_fields(world_.runtime(), types_, presentation_types_, take(runtime_.type_of(object)));
        const MissingResource* mesh_unavailable{};
        if (!document && !material)
            if (const auto mesh = presentation::read_static_mesh(world_.runtime(), object);
                mesh && !mesh->mesh.resolved)
                mesh_unavailable = unavailable(assets::AssetKind::mesh, mesh->mesh.source);
        // Descends into records the current schema still declares; anything else is one entry.
        const auto classify = [&](std::span<const std::string> path) -> std::pair<bool, world::DiagnosticKind> {
            using enum world::DiagnosticKind;
            if (document)
                return {false, unknown_key};
            if (material)
                return {false, unknown_key};
            if (path.front() == "properties") {
                if (path.size() == 1)
                    return {true, mismatch};
                const auto names = path.subspan(1);
                bool exact{};
                bool prefix{};
                for (const auto& field : schema) {
                    if (std::ranges::equal(field.names, names))
                        exact = true;
                    else if (field.names.size() > names.size() &&
                             std::equal(names.begin(), names.end(), field.names.begin()))
                        prefix = true;
                }
                return {!exact && prefix, exact || prefix ? mismatch : unknown_field};
            }
            if (path.front() == "space") {
                if (path.size() == 1)
                    return {true, mismatch};
                return {false,
                        path[1] == "position" || path[1] == "rotation" || path[1] == "scale" || path[1] == "origin"
                            ? mismatch
                            : unknown_key};
            }
            if (path.front() == "materials" && mesh_unavailable)
                return {false, mesh_unavailable->kind};
            constexpr std::array known{"name", "enabled", "mesh", "materials", "sky_map"};
            return {false, std::ranges::find(known, path.front()) != known.end() ? mismatch : unknown_key};
        };
        std::vector<std::string> path;
        const auto walk = [&](auto&& self, json::Value value) -> json::Result<void> {
            ATELIER_TRY_BIND(members, value.object_items());
            for (const auto& [name, item] : *members) {
                path.emplace_back(name);
                const auto [record, kind] = classify(path);
                if (record && item.kind() == json::Kind::object && !reference_value(item))
                    ATELIER_TRY(self(self, item));
                else
                    view.entries.push_back({path, preview(item), kind});
                path.pop_back();
            }
            return {};
        };
        const auto decoded = [&]() -> json::Result<void> {
            if (!entry->record.empty()) {
                ATELIER_TRY_BIND(record, json::Document::parse(entry->record, {std::numeric_limits<std::size_t>::max(),
                                                                               json::DepthPolicy::containers, true}));
                ATELIER_TRY(walk(walk, record->root()));
            }
            if (!entry->slots.empty()) {
                ATELIER_TRY_BIND(slots, json::Document::parse(entry->slots, {std::numeric_limits<std::size_t>::max(),
                                                                             json::DepthPolicy::containers, true}));
                ATELIER_TRY_BIND(members, slots->root().object_items());
                for (const auto& [slot, keys] : *members) {
                    ATELIER_TRY_BIND(items, keys.object_items());
                    for (const auto& [key, value] : *items)
                        view.entries.push_back({{"materials", std::string(slot), std::string(key)},
                                                preview(value),
                                                key == "overridden" ? world::DiagnosticKind::mismatch
                                                                    : world::DiagnosticKind::unknown_key});
                }
            }
            if (document)
                for (const auto& resource : entry->materials)
                    if (runtime_.type_of(resource.id())) {
                        const auto key = document_identity(resource.id());
                        json::Builder value;
                        ATELIER_TRY(
                            value.set(value.root(), take(material::material_key(world_.runtime(), resource.id()))));
                        view.entries.push_back({{"materials", key ? std::to_string(*key) : std::string("?")},
                                                preview(value.root()),
                                                world::DiagnosticKind::unknown_key});
                    }
            return {};
        }();
        if (!decoded)
            return std::unexpected(Error{ErrorCode::invalid_storage, decoded.error().describe()});
        return view;
    } catch (const std::exception& error) {
        return failed(error);
    }
}

std::vector<world::Diagnostic> Session::issues() const {
    std::vector<world::Diagnostic> result;
    std::unordered_set<ObjectId> members;
    if (const auto list = world_.members())
        for (const auto& member : *list)
            members.insert(member.id());
    const auto owner = [&](ObjectId object) -> ObjectId {
        if (members.contains(object))
            return object;
        const auto component = world::inspect_component(world_.runtime(), object);
        return component && component->valid && members.contains(component->owner) ? component->owner : ObjectId{};
    };
    std::unordered_map<std::uint64_t, ObjectId> by_key;
    for (const auto& [object, key] : local_ids_)
        by_key.emplace(key, object);
    const auto add = [&](ObjectId object, std::string type, world::DiagnosticKind kind, std::vector<std::string> field,
                         std::string value, std::string detail) {
        world::Diagnostic item;
        item.serial = result.size() + 1;
        item.severity = world::Severity::warning;
        item.kind = kind;
        item.phase = world::DiagnosticPhase::load;
        item.object = object;
        item.entity = object == ObjectId{} ? ObjectId{} : owner(object);
        item.type = std::move(type);
        auto subject = object == ObjectId{} ? std::string("Scene") : item.type;
        if (const auto key = document_identity(object); object != ObjectId{} && key)
            subject += " #" + std::to_string(*key);
        item.message = subject + (field.empty() ? std::string{} : " " + dotted(field)) + ": " + detail;
        item.field = std::move(field);
        item.value = std::move(value);
        result.push_back(std::move(item));
    };
    const auto report = [&](ObjectId object, const Retained& entry, std::string type) {
        if (!entry.type.empty())
            add(object, type, world::DiagnosticKind::unknown_type, {}, {},
                "The type is not registered; a placeholder keeps its saved data.");
        const auto view = retained(object);
        if (!view)
            return;
        for (const auto& item : view->entries)
            add(object, type, item.kind, item.path, item.value,
                item.kind == world::DiagnosticKind::mismatch
                    ? "The saved value does not fit the current type; it is kept until edited or discarded."
                : item.kind == world::DiagnosticKind::missing_resource ||
                        item.kind == world::DiagnosticKind::unreadable_resource
                    ? "The resource it needs is unavailable; the saved value is kept unchanged."
                    : "Saved data kept for the next save.");
        for (const auto* text : {&entry.record, &entry.slots})
            if (!text->empty()) {
                const auto value = json::Document::parse(
                    *text, {std::numeric_limits<std::size_t>::max(), json::DepthPolicy::containers, true});
                if (!value)
                    return;
                const auto visited = each_reference(value->root(), [&](json::Value node) -> json::Result<void> {
                    const auto at = *node.find("$ref");
                    if (!at)
                        return {};
                    ATELIER_TRY_BIND(key, local_key(*at));
                    const auto found = by_key.find(*key);
                    const bool live = found != by_key.end() && runtime_.type_of(found->second) &&
                                      (owner(found->second) != ObjectId{} || is_material(found->second));
                    if (!live) {
                        const auto bytes = json::write(node);
                        add(object, type, world::DiagnosticKind::reference_dropped, {}, {bytes.data(), bytes.size()},
                            "A saved reference targets a deleted object; it will be saved as null.");
                    }
                    return {};
                });
                if (!visited)
                    return;
            }
    };
    try {
        // Serials and display order follow document identity, not hash-table order.
        std::vector<std::tuple<std::uint64_t, ObjectId, const Retained*>> listed;
        for (const auto& [object, entry] : retained_)
            if (runtime_.type_of(object) && (owner(object) != ObjectId{} || is_material(object)))
                listed.emplace_back(local_ids_.at(object), object, &entry);
        std::ranges::sort(listed, {}, [](const auto& item) { return std::get<0>(item); });
        for (const auto& [key, object, entry] : listed)
            report(object, *entry,
                   entry->type.empty() ? type_name(world_.runtime(), take(runtime_.type_of(object))) : entry->type);
        report({}, retained_document_, {});
    } catch (const std::exception&) {
    }
    return result;
}

Result<ObjectId> Session::local_object(std::uint64_t identity) const {
    try {
        if (!identity)
            throw std::runtime_error("Document identity cannot be empty.");
        // A Play copy that re-adds a removed identity keeps the destroyed object's entry beside the new one.
        std::vector<ObjectId> candidates;
        for (const auto& [object, key] : local_ids_)
            if (key == identity)
                candidates.push_back(object);
        if (candidates.empty())
            throw std::runtime_error("Unknown document object identity.");
        const auto live = [&](ObjectId object) { return std::ranges::contains(candidates, object); };
        for (const auto& entity : take(world_.members())) {
            if (live(entity.id()))
                return entity.id();
            for (const auto& component : take(world::components(world_.runtime(), entity.id())))
                if (live(component.id()))
                    return component.id();
        }
        for (const auto object : candidates)
            if (is_material(object))
                return object;
        throw std::runtime_error("Document identity does not belong to a live Entity, Component or material.");
    } catch (const std::exception& error) {
        return failed(error);
    }
}
}
