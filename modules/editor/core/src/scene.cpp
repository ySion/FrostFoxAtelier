#include "editor/session.hpp"
#include "memory/fatal.hpp"
#include "editor/graph_format.hpp"
#include "editor/project.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "editor/fields.hpp"
#include "editor/properties.hpp"
#include "assets/material_format.hpp"
#include "assets/mesh_format.hpp"
#include "physics/physics.hpp"
#include "procedural/graph.hpp"
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

struct SceneFailure {
    enum class Disposition { reject, destroyed, unsupported_pose };

    std::variant<std::string, Error> detail;

    Disposition disposition;

    SceneFailure(std::string message, Disposition action = Disposition::reject)
        : detail(std::move(message)), disposition(action) {}

    SceneFailure(Error cause, Disposition action = Disposition::reject)
        : detail(std::move(cause)), disposition(action) {}

    SceneFailure(const json::Error& error) : SceneFailure(error.describe()) {}

    Error into_error() && {
        if (auto* error = std::get_if<Error>(&detail))
            return std::move(*error);
        return {ErrorCode::invalid_storage, std::move(std::get<std::string>(detail))};
    }
};

template <class T>
using SceneResult = std::expected<T, SceneFailure>;

template <class T>
Result<T> scene_result(SceneResult<T> value) {
    return std::move(value).transform_error([](SceneFailure error) { return std::move(error).into_error(); });
}

template <class T>
Result<T> scene_result(json::Result<T> value) {
    if (!value)
        return std::unexpected(Error{ErrorCode::invalid_storage, value.error().describe()});
    if constexpr (std::is_void_v<T>)
        return {};
    else
        return std::move(*value);
}

// The caller has already admitted this key while constructing the in-memory graph.
template <class Table, class Key>
const auto& required_entry(const Table& table, const Key& key) {
    const auto found = table.find(key);
    if (found == table.end())
        fatal("A validated scene graph entry is missing.");
    return found->second;
}

Result<void> committed(Result<world::Commit> value) {
    ATELIER_TRY(value);
    if (value->notification_error)
        return std::unexpected(std::move(*value->notification_error));
    return {};
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

Result<void> encode_scalar(Runtime& runtime, json::Builder& builder, json::Value target, const Scalar& value) {
    return std::visit(
        [&](const auto& v) -> Result<void> {
            if constexpr (std::same_as<std::decay_t<decltype(v)>, EnumValue>) {
                ATELIER_TRY_BIND(info, runtime.describe(v.type));
                for (const auto& [name, bits] : (*info)->enumeration->values)
                    if (bits == v.bits)
                        return scene_result(builder.set(target, std::string_view{name}));
                if ((*info)->enumeration->flags &&
                    (v.bits & detail::declared_flag_mask(*(*info)->enumeration)) == v.bits) {
                    builder.set(target, static_cast<std::uint64_t>(v.bits));
                    return {};
                }
                return std::unexpected(Error{ErrorCode::invalid_storage, "Scene enum value has no declared name."});
            } else if constexpr (std::is_floating_point_v<std::decay_t<decltype(v)>>) {
                auto result = builder.set(target, v);
                if (!result && result.error().fault == json::Fault::range)
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Scene property values must be finite."});
                return scene_result(std::move(result));
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

// A schema mismatch is a saved value to keep, not a Runtime failure.
using DecodedScalar = std::variant<Scalar, std::string>;

Result<DecodedScalar> decode_scalar(Runtime& runtime, const Scalar& prototype, json::Value value,
                                    bool* converted = nullptr) {
    return std::visit(
        [&](const auto& original) -> Result<DecodedScalar> {
            using T = std::decay_t<decltype(original)>;
            if constexpr (std::same_as<T, bool>) {
                if (value.kind() != json::Kind::boolean)
                    return std::string("Expected a boolean property.");
                return Scalar{*value.boolean()};
            } else if constexpr (std::same_as<T, EnumValue>) {
                ATELIER_TRY_BIND(info, runtime.describe(original.type));
                if (value.kind() == json::Kind::string) {
                    for (const auto& [name, bits] : (*info)->enumeration->values)
                        if (*value.string() == name)
                            return Scalar{EnumValue{original.type, bits}};
                    return std::string("Unknown scene enum member.");
                }
                if ((*info)->enumeration->flags && value.kind() == json::Kind::integer) {
                    const auto number = value.i64();
                    if (number && *number >= 0 && *number <= 4294967295ll) {
                        const auto bits = static_cast<std::uint32_t>(*number);
                        if ((bits & detail::declared_flag_mask(*(*info)->enumeration)) == bits)
                            return Scalar{EnumValue{original.type, bits}};
                    }
                    return std::string("Unknown scene enum flags.");
                }
                return std::string((*info)->enumeration->flags ? "Expected a declared enum member name or flag mask."
                                                               : "Expected a declared enum member name.");
            } else if constexpr (std::is_integral_v<T>) {
                if (value.kind() == json::Kind::real) {
                    // Lossless only: an integral value written by an earlier float field.
                    const auto number = *value.number();
                    if (!std::isfinite(number) || std::trunc(number) != number ||
                        number < static_cast<double>(std::numeric_limits<T>::min()) ||
                        number > static_cast<double>(std::numeric_limits<T>::max()))
                        return std::string("Expected an integer property.");
                    if (converted)
                        *converted = true;
                    return Scalar{static_cast<T>(number)};
                }
                if (value.kind() != json::Kind::integer)
                    return std::string("Expected an integer property.");
                // Runtime scalar integers are 32-bit; every accepted value fits i64.
                const auto number = value.i64();
                if (!number || *number < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                    *number > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
                    return std::string("Integer property is out of range.");
                return Scalar{static_cast<T>(*number)};
            } else {
                if (value.kind() != json::Kind::integer && value.kind() != json::Kind::real)
                    return std::string("Expected a numeric property.");
                const auto number = static_cast<T>(*value.number());
                if (!std::isfinite(number))
                    return std::string("Scene property values must be finite and representable.");
                return Scalar{number};
            }
        },
        prototype);
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

std::string dump_retained(json::Value value) {
    if (value.size() == 0)
        return {};
    const auto bytes = json::write(value);
    return {bytes.data(), bytes.size()};
}

Result<std::vector<detail::ScalarField>> business_fields(Runtime& runtime, const world::Types& types, TypeId type,
                                                         std::vector<detail::FieldNames>* transient_paths = nullptr) {
    ATELIER_TRY_BIND(result, detail::author_fields(runtime, type, false, transient_paths));
    std::erase_if(*result, [&](const auto& field) { return detail::transform_field(types, field.path.front()); });
    return result;
}

Result<String> encode_string(Runtime& runtime, const PropertyTarget& target, const detail::ScalarField& field) {
    String value;
    const ValueOutput output{field.type, &value};
    ATELIER_TRY(target.object != ObjectId{} ? runtime.read(target.object, field.path, output)
                                            : runtime.read(target.type, target.storage, field.path, output));
    return value;
}

// A whole Array leaf as a JSON array of its element leaves.
Result<void> encode_array(Runtime& runtime, ObjectId object, const detail::ScalarField& field, json::Builder& builder,
                          json::Value target) {
    ATELIER_TRY_BIND(array, runtime.describe(field.type));
    const auto element = *(*array)->element;
    ATELIER_TRY_BIND(info, runtime.describe(element));
    const bool text = (*info)->kind == TypeKind::string;
    auto path = field.path;
    path.emplace_back();
    ATELIER_TRY_BIND(count, runtime.element_count(object, field.path));
    for (std::size_t i = 0; i < *count; ++i) {
        ATELIER_TRY_BIND(index, runtime.element(field.type, static_cast<std::uint32_t>(i)));
        path.back() = *index;
        const auto entry = builder.append_null(target);
        if (!text) {
            ATELIER_TRY_BIND(value, runtime.read(object, path));
            ATELIER_TRY(encode_scalar(runtime, builder, entry, *value));
            continue;
        }
        String value;
        ATELIER_TRY(runtime.read(object, path, ValueOutput{element, &value}));
        builder.set_validated(entry, value.view().std_view());
    }
    return {};
}

Result<void> store_leaf(Runtime& runtime, const PropertyTarget& target, const detail::ScalarField& field,
                        const PropertyLeaf& leaf) {
    const auto store = [&](const auto& input) {
        return target.object != ObjectId{} ? runtime.write(target.object, field.path, input)
                                           : runtime.write(target.type, target.storage, field.path, input);
    };
    if (const auto* scalar = std::get_if<Scalar>(&leaf))
        return store(*scalar);
    if (const auto* text = std::get_if<String>(&leaf))
        return store(ValueInput{field.type, text});
    return store(std::get<ArrayLeaf>(leaf).input());
}

// Decodes a properties record leaf by leaf: the one path for scene loads, Play changes and operator
// parameters. Tolerant decodes write a clamped leaf and then keep the saved value; strict decodes
// (no report) return an error at the first rejected leaf and never clamp. Earlier accepted leaves remain written.
template <class E = Error, class Resolver, class Report>
std::expected<void, E>
read_record(Runtime& runtime, const PropertyTarget& target, std::span<const detail::ScalarField> schema,
            json::Value record, const std::vector<std::string>& prefix, const Resolver& resolve, const WriteLeaf& write,
            const Report* report, std::span<const detail::FieldNames> transient_paths = {}) {
    const auto object = target.object != ObjectId{};
    const auto write_leaf = [&](const detail::ScalarField& field,
                                const PropertyLeaf& leaf) -> std::expected<std::string, E> {
        auto written = write ? write(field, leaf) : store_leaf(runtime, target, field, leaf);
        if (written)
            return std::string{};
        if constexpr (std::same_as<E, SceneFailure>)
            if (write)
                return std::unexpected(std::move(written.error()));
        return std::move(written.error().message);
    };
    const auto issue = [&](std::vector<std::string> at, json::Value item, world::DiagnosticKind kind,
                           const std::string& problem, const std::string& kept) -> std::expected<void, E> {
        if (!report)
            return std::unexpected(Error{ErrorCode::argument_mismatch,
                                         detail::dotted(std::span(at).subspan(prefix.size())) + ": " + problem});
        if constexpr (std::same_as<Report, PropertyReport>)
            if (!report->keep)
                return std::unexpected(Error{ErrorCode::invalid_storage, "No callback for a retained property."});
        return report->keep(std::move(at), item, kind, kept);
    };
    const auto read = [&](auto&& self, json::Value value,
                          const std::vector<std::string>& path) -> std::expected<void, E> {
        const auto members = value.object_items();
        for (const auto& [name, item] : *members) {
            auto next_path = path;
            next_path.emplace_back(name);
            auto at = prefix;
            at.insert(at.end(), next_path.begin(), next_path.end());
            const auto field = std::ranges::find_if(schema, [&](const auto& f) { return f.names == next_path; });
            if (field == schema.end()) {
                if (std::ranges::find(transient_paths, next_path) != transient_paths.end())
                    continue;
                const auto descendant = [&](const auto& names) {
                    return names.size() > next_path.size() &&
                           std::equal(next_path.begin(), next_path.end(), names.begin());
                };
                const bool nested = std::ranges::any_of(schema, descendant, &detail::ScalarField::names) ||
                                    std::ranges::any_of(transient_paths, descendant);
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
            std::optional<Scalar> clamped;
            const auto decoded = [&]() -> std::expected<std::string, E> {
                if (field->form == ValueForm::inline_value && field->kind == TypeKind::string) {
                    if (!field->material_entries_json && item.kind() != json::Kind::string)
                        return "Expected a string property.";
                    String stored;
                    if (field->material_entries_json) {
                        const auto bytes = json::write(item);
                        stored = String::from_utf8_validated({bytes.data(), bytes.size()});
                    } else
                        stored = String::from_utf8_validated(StringView{*item.string()});
                    return write_leaf(*field, stored);
                } else if (field->form == ValueForm::inline_value && field->kind == TypeKind::array) {
                    // The whole Array is built aside and stored as one leaf, so a rejected write leaves the field as it was.
                    if (item.kind() != json::Kind::array)
                        return "Expected an array property.";
                    ATELIER_TRY_BIND(leaf, ArrayLeaf::create(runtime, field->type));
                    ATELIER_TRY_BIND(info, runtime.describe(field->type));
                    const auto element = *(*info)->element;
                    ATELIER_TRY_BIND(element_info, runtime.describe(element));
                    const bool text = (*element_info)->kind == TypeKind::string;
                    ATELIER_TRY(runtime.mutate_sequence(field->type, leaf->storage(), SequenceMutation::resize, {}, {},
                                                        item.size()));
                    // Every element takes the field's hard bounds, which constrain() reads through any element selector.
                    auto bounded = field->path;
                    ATELIER_TRY_BIND(first, runtime.element(field->type, 0));
                    bounded.push_back(*first);
                    bool clamped_elements{};
                    std::uint32_t index = 0;
                    const auto items = *item.array();
                    for (auto value : items) {
                        ATELIER_TRY_BIND(selected, runtime.element(field->type, index++));
                        const std::array selector{*selected};
                        if (text) {
                            if (value.kind() != json::Kind::string)
                                return "Expected an array of strings.";
                            auto stored = String::from_utf8_validated(StringView{*value.string()});
                            ATELIER_TRY(
                                runtime.write(field->type, leaf->storage(), selector, ValueInput{element, &stored}));
                            continue;
                        }
                        ATELIER_TRY_BIND(current, runtime.read(field->type, leaf->storage(), selector));
                        ATELIER_TRY_BIND(parsed, decode_scalar(runtime, *current, value, &converted));
                        if (const auto* mismatch = std::get_if<std::string>(&*parsed))
                            return *mismatch;
                        const auto& scalar = std::get<Scalar>(*parsed);
                        auto stored = runtime.constrain(target.type, bounded, scalar);
                        if (!stored)
                            return stored.error().message;
                        if (*stored != scalar) {
                            if (!report)
                                return "An element is outside the field's hard bounds.";
                            clamped_elements = true;
                        }
                        ATELIER_TRY(runtime.write(field->type, leaf->storage(), selector, *stored));
                    }
                    // Reported before the store: a strict report (a Play change) rejects the clamp with the field unchanged.
                    // As for a scalar, the clamp is the one report, whatever elements converted.
                    if (clamped_elements) {
                        auto reported = issue(
                            at, item, world::DiagnosticKind::mismatch, {},
                            "An element is outside the field's hard bounds; the clamped Array is used and the saved value is kept.");
                        if (!reported)
                            return std::unexpected(reported.error());
                        converted = false;
                    }
                    return write_leaf(*field, *leaf);
                } else if (field->form == ValueForm::inline_value) {
                    const auto current = object ? runtime.read(target.object, field->path)
                                                : runtime.read(target.type, target.storage, field->path);
                    if (!current)
                        return std::unexpected(current.error());
                    ATELIER_TRY_BIND(parsed, decode_scalar(runtime, *current, item, &converted));
                    if (const auto* mismatch = std::get_if<std::string>(&*parsed))
                        return *mismatch;
                    const auto& scalar = std::get<Scalar>(*parsed);
                    auto stored = runtime.constrain(target.type, field->path, scalar);
                    if (!stored)
                        return stored.error().message;
                    if (*stored != scalar) {
                        if (!report)
                            return "Outside the field's hard bounds.";
                        clamped = *stored;
                    }
                    return write_leaf(*field, *stored);
                } else if (!resolve) {
                    return "This decode does not accept object references.";
                } else if (field->form == ValueForm::strong_sequence) {
                    if (item.kind() != json::Kind::array ||
                        item.size() > Session::maximum_entities * Session::maximum_components)
                        return "Expected an object reference sequence.";
                    std::vector<ObjectReference> refs;
                    refs.reserve(item.size());
                    const auto items = *item.array();
                    for (auto ref : items) {
                        ATELIER_TRY_BIND(resolved, resolve(ref, field->type, ValueForm::strong_reference));
                        if (!*resolved)
                            return "A sequence element is not a reference of the field's type.";
                        refs.push_back(std::move(**resolved));
                    }
                    ATELIER_TRY_BIND(locator, runtime.reference_field(target.object, field->path));
                    ATELIER_TRY_BIND(count, runtime.sequence_size(*locator));
                    const ReferenceChange change = SequenceSplice{*locator, 0, *count, refs};
                    if (auto applied = runtime.apply_reference_changes({&change, 1}); !applied)
                        return applied.error().message;
                } else {
                    ATELIER_TRY_BIND(resolved, resolve(item, field->type, field->form));
                    if (!*resolved)
                        return "Expected a reference of the field's type.";
                    auto written = object
                                       ? runtime.write_reference(target.object, field->path, **resolved)
                                       : runtime.write_reference(target.type, target.storage, field->path, **resolved);
                    if (!written)
                        return written.error().message;
                }
                return std::string{};
            }();
            if (!decoded)
                return std::unexpected(decoded.error());
            const auto& problem = *decoded;
            if (!problem.empty())
                ATELIER_TRY(issue(std::move(at), item, world::DiagnosticKind::mismatch, problem,
                                  problem + " The default is used and the saved value is kept."));
            else if (clamped) {
                json::Builder kept(json::Kind::null);
                ATELIER_TRY(encode_scalar(runtime, kept, kept.root(), *clamped));
                const auto bytes = json::write(kept.root());
                ATELIER_TRY(issue(std::move(at), item, world::DiagnosticKind::mismatch, {},
                                  "Outside the field's hard bounds; " + std::string(bytes.data(), bytes.size()) +
                                      " is used and the saved value is kept."));
            } else if (converted && report) {
                if constexpr (std::same_as<Report, PropertyReport>)
                    if (!report->converted)
                        return std::unexpected(
                            Error{ErrorCode::invalid_storage, "No callback for a converted property."});
                ATELIER_TRY(report->converted(std::move(at), item));
            }
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

Result<void> trs_json(json::Builder& builder, json::Value target, const world::Trs& trs) {
    ATELIER_TRY_BIND(value, world::canonical_transform(trs));
    ATELIER_TRY(scene_result(vector(builder, target, "position", value->position)));
    ATELIER_TRY_BIND(rotation, scene_result(builder.array(target, "rotation")));
    for (float component : {value->rotation.x, value->rotation.y, value->rotation.z, value->rotation.w})
        ATELIER_TRY(scene_result(builder.append(*rotation, component)));
    ATELIER_TRY(scene_result(vector(builder, target, "scale", value->scale)));
    return scene_result(builder.set_null(target, "origin"));
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

Result<void> validate_property_target(Runtime& runtime, const PropertyTarget& target, json::Value record) {
    if ((target.object == ObjectId{}) == target.storage.empty())
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "A properties target is either an object or a record value."});
    if (target.object != ObjectId{}) {
        ATELIER_TRY_BIND(type, runtime.type_of(target.object));
        if (*type != target.type)
            return std::unexpected(
                Error{ErrorCode::type_mismatch, "A properties target object must have the target type."});
    }
    if (record.kind() != json::Kind::object)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Expected a property record."});
    return {};
}
}

struct detail::PropertyStorage {
    Runtime& runtime;

    TypeId type;

    atelier::detail::ValueStorage bytes;

    std::span<std::byte> storage;

    bool live{};

    PropertyStorage(Runtime& owner, const TypeInfo& info)
        : runtime(owner), type(info.id), storage(bytes.prepare(info.size, info.alignment), info.size) {}

    PropertyStorage(const PropertyStorage&) = delete;

    PropertyStorage& operator=(const PropertyStorage&) = delete;

    Result<void> initialize() {
        ATELIER_TRY(runtime.initialize_value(type, storage));
        live = true;
        return {};
    }

    ~PropertyStorage() {
        if (live)
            static_cast<void>(runtime.destroy_value(type, storage));
    }
};

Result<ArrayLeaf> ArrayLeaf::create(Runtime& runtime, TypeId type) {
    ATELIER_TRY_BIND(info, runtime.describe(type));
    if ((*info)->kind != TypeKind::array)
        return std::unexpected(Error{ErrorCode::invalid_type, "An Array leaf requires an Array type."});
    ArrayLeaf result;
    result.value_ = std::make_shared<detail::PropertyStorage>(runtime, **info);
    ATELIER_TRY(result.value_->initialize());
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
    const auto object = target.object;
    if ((object == ObjectId{}) == target.storage.empty())
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "A properties target is either an object or a record value."});
    const auto encode_reference = [&](ObjectId id) -> Result<json::Document> {
        if (!reference)
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "A reference property requires a reference encoder."});
        return reference(id);
    };
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
                ATELIER_TRY_BIND(text, encode_string(runtime, target, field));
                if (field.material_entries_json) {
                    const auto parsed =
                        json::Document::parse(text->view().std_view(), {std::numeric_limits<std::size_t>::max(),
                                                                        json::DepthPolicy::containers, true});
                    if (!parsed)
                        return std::unexpected(Error{ErrorCode::invalid_storage, parsed.error().describe()});
                    result.set_validated(parent, key, parsed->root());
                } else
                    result.set_validated(parent, key, text->view().std_view());
            } else if (field.kind == TypeKind::array) {
                const auto array = *result.array(parent, key);
                ATELIER_TRY(encode_array(runtime, object, field, result, array));
            } else {
                const auto value = result.object_validated(parent, key);
                ATELIER_TRY_BIND(scalar, object != ObjectId{} ? runtime.read(object, field.path)
                                                              : runtime.read(target.type, target.storage, field.path));
                ATELIER_TRY(encode_scalar(runtime, result, value, *scalar));
            }
        } else if (field.form == ValueForm::strong_sequence) {
            const auto array = *result.array(parent, key);
            ATELIER_TRY_BIND(locator, runtime.reference_field(object, field.path));
            ATELIER_TRY_BIND(count, runtime.sequence_size(*locator));
            for (std::size_t i = 0; i < *count; ++i) {
                ATELIER_TRY_BIND(item, runtime.sequence_read(*locator, i));
                ATELIER_TRY_BIND(value, encode_reference(item->id()));
                result.append(array, value->root());
            }
        } else {
            ATELIER_TRY_BIND(ref, object != ObjectId{}
                                      ? runtime.read_reference(object, field.path)
                                      : runtime.read_reference(target.type, target.storage, field.path));
            ATELIER_TRY_BIND(value, encode_reference(ref->id()));
            result.set_validated(parent, key, value->root());
        }
    }
    return result.finish();
}

Result<void> decode_properties(Runtime& runtime, const PropertyTarget& target, json::Value record,
                               const ResolveReference& resolve, const WriteLeaf& write, bool include_transient) {
    ATELIER_TRY(validate_property_target(runtime, target, record));
    if (write && resolve)
        return std::unexpected(Error{ErrorCode::argument_mismatch,
                                     "A write hook does not receive reference leaves; decode references without one."});
    ATELIER_TRY_BIND(schema, detail::author_fields(runtime, target.type, include_transient));
    return read_record(runtime, target, *schema, record, {}, resolve, write,
                       static_cast<const PropertyReport*>(nullptr));
}

Result<void> decode_properties(Runtime& runtime, const PropertyTarget& target, json::Value record,
                               const PropertyReport& report, const ResolveReference& resolve) {
    ATELIER_TRY(validate_property_target(runtime, target, record));
    std::vector<detail::FieldNames> transient_paths;
    ATELIER_TRY_BIND(fields, detail::author_fields(runtime, target.type, false, &transient_paths));
    return read_record(runtime, target, *fields, record, {}, resolve, {}, &report, transient_paths);
}

Result<void> patch_object_properties(Runtime& runtime, ObjectId object, json::Value patch,
                                     const EncodeReference& reference, json::Document* after) {
    ATELIER_TRY_BIND(type, runtime.type_of(object));
    const PropertyTarget target{*type, object};
    ATELIER_TRY(validate_property_target(runtime, target, patch));
    ATELIER_TRY_BIND(fields, detail::author_fields(runtime, *type));
    // Paths borrow this schema. Each backup owns the exact typed value from before its successful write.
    struct Backup {
        std::span<const FieldId> path;

        std::unique_ptr<detail::PropertyStorage> value;
    };
    std::vector<Backup> written;
    const WriteLeaf write = [&](const detail::ScalarField& field, const PropertyLeaf& leaf) -> Result<void> {
        ATELIER_TRY_BIND(info, runtime.describe(field.type));
        auto old = std::make_unique<detail::PropertyStorage>(runtime, **info);
        ATELIER_TRY(old->initialize());
        ATELIER_TRY(runtime.read_for_restore(object, field.path, ValueOutput{field.type, old->storage.data()}));
        ATELIER_TRY(store_leaf(runtime, target, field, leaf));
        written.push_back({field.path, std::move(old)});
        return {};
    };
    auto result = read_record(runtime, target, *fields, patch, {}, ResolveReference{}, write,
                              static_cast<const PropertyReport*>(nullptr));
    if (result && after) {
        auto snapshot = encode_properties(runtime, target, *fields, reference);
        if (snapshot)
            *after = std::move(*snapshot);
        else
            result = std::unexpected(std::move(snapshot.error()));
    }
    if (!result) {
        for (auto at = written.rbegin(); at != written.rend(); ++at) {
            // The same owner thread, object, paths and borrow state as the writes above.
            if (!runtime.restore(object, at->path, ValueOutput{at->value->type, at->value->storage.data()}))
                fatal("Cannot restore a just-written graph property.");
        }
    }
    return result;
}

struct Session::GraphCodec {
    struct MeshCopy {
        presentation::StaticMeshState state;

        std::vector<presentation::MaterialBinding> bindings;
    };

    struct GeneratorCopy {
        presentation::ProceduralBinding binding;

        std::vector<presentation::MaterialBinding> bindings;

        json::Document overrides;
    };

    struct Graph {
        json::Document data;

        std::unordered_map<std::string, MeshCopy> meshes;

        std::unordered_map<std::string, GeneratorCopy> generators;

        // An in-memory copy keeps the sky map it resolved, not only the key.
        std::unordered_map<std::string, presentation::SkyMapBinding> skies;

        std::unordered_map<std::uint64_t, std::shared_ptr<const std::string>> cooked_shapes;

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

        Retained document_retained;

        std::unordered_map<ObjectId, std::vector<ObjectRoot>> materials; // Named by a kept binding table.

        std::unordered_map<ObjectId, Retained> retained;

        std::string subject(ObjectId object) const {
            if (object == ObjectId{})
                return "Scene";
            const auto type = types.find(object);
            std::string text = type != types.end() ? type->second : std::string("Object");
            if (const auto at = keys.find(object); at != keys.end())
                text += " #" + std::to_string(at->second);
            return text;
        }

        SceneResult<void> report(world::Severity severity, world::DiagnosticKind kind, ObjectId object,
                                 std::vector<std::string> field, const json::Value* value, const std::string& detail,
                                 const Error* cause = nullptr) {
            auto message =
                cause ? cause->message
                      : subject(object) + (field.empty() ? std::string{} : " " + detail::dotted(field)) + ": " + detail;
            if (policy == LoadPolicy::strict && severity != world::Severity::info) {
                if (cause)
                    return std::unexpected(*cause);
                return std::unexpected(SceneFailure(std::move(message)));
            }
            world::Diagnostic item;
            item.serial = issues.size() + 1;
            item.severity = severity;
            item.kind = kind;
            item.phase = world::DiagnosticPhase::load;
            item.object = object;
            if (object != ObjectId{}) {
                const auto owner = owners.find(object);
                item.entity = owner != owners.end() ? owner->second : object;
            }
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
            return {};
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

        SceneResult<void> keep(ObjectId object, std::vector<std::string> path, json::Value value,
                               world::DiagnosticKind kind, const std::string& detail) {
            ATELIER_TRY(report(world::Severity::warning, kind, object, path, &value, detail));
            store(object, path, value);
            return {};
        }

        SceneResult<void> keep_slot(ObjectId object, const std::string& slot, const std::string& key, json::Value value,
                                    world::DiagnosticKind kind, const std::string& detail) {
            ATELIER_TRY(report(world::Severity::warning, kind, object, {"materials", slot, key}, &value, detail));
            auto& builder = pending[object];
            const std::array path{std::string("slots"), slot};
            builder.set_validated(builder.object_path_validated(builder.root(), path), key, value);
            return {};
        }

        SceneResult<void> extra(ObjectId object, json::Value value, std::initializer_list<std::string_view> allowed,
                                std::vector<std::string> prefix = {}) {
            ATELIER_TRY_BIND(members, value.object_items());
            for (const auto& [name, item] : *members)
                if (std::ranges::find(allowed, name) == allowed.end()) {
                    auto path = prefix;
                    path.emplace_back(name);
                    ATELIER_TRY(keep(object, std::move(path), item, world::DiagnosticKind::unknown_key,
                                     "Unknown key kept from the saved scene."));
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
            for (const auto& [instance, parent] : session->scene_material_parents_)
                if (!material_parents_before.contains(instance))
                    (void)session->runtime_.destroy(instance);
            std::erase_if(session->materials_, [&](const auto& entry) {
                if (material_keys.contains(entry.first))
                    return false;
                (void)session->runtime_.destroy(entry.second.root.id());
                return true;
            });
            std::erase_if(session->textures_, [&](const auto& entry) {
                if (texture_keys.contains(entry.first))
                    return false;
                (void)session->runtime_.destroy(entry.second.id());
                return true;
            });
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
        ATELIER_TRY(session.world_.members());
        std::erase_if(session.local_ids_, [&](const auto& entry) { return !runtime.type_of(entry.first); });
        std::unordered_set<ObjectId> selected(requested.begin(), requested.end());
        std::unordered_set<ObjectId> seen;
        std::unordered_set<ObjectId> root_seen;
        root_seen.reserve(requested.size());
        for (const auto object : requested) {
            ATELIER_TRY_BIND(state, world::inspect_entity(runtime, object));
            if ((!persistent && !state->can_edit) || !session.names_.contains(object) ||
                !session.world_.contains(object))
                return std::unexpected(
                    Error{ErrorCode::invalid_storage, "The Entity is not editable in this document."});
            bool covered{};
            for (auto ancestor = object;;) {
                ATELIER_TRY_BIND(parent, world::parent_entity(runtime, ancestor));
                if (*parent == ObjectId{})
                    break;
                if (selected.contains(*parent)) {
                    covered = true;
                    break;
                }
                ancestor = *parent;
            }
            if (!covered && root_seen.insert(object).second)
                roots.push_back(object);
        }
        const auto walk = [&](auto&& self, ObjectId object) -> Result<void> {
            if (!seen.insert(object).second)
                return std::unexpected(Error{ErrorCode::invalid_storage, "Entity graph contains a repeated identity."});
            nodes.push_back(object);
            if (nodes.size() > maximum_entities)
                return std::unexpected(Error{ErrorCode::invalid_storage, "Scene exceeds the Entity limit."});
            ATELIER_TRY_BIND(children, world::children(runtime, object));
            for (const auto& child : *children) {
                ATELIER_TRY(self(self, child.id()));
            }
            return {};
        };
        for (auto root : roots)
            ATELIER_TRY(walk(walk, root));
        std::unordered_map<ObjectId, std::uint64_t> ids;
        for (auto object : nodes) {
            ATELIER_TRY_BIND(key, scene_result(identity(session, object)));
            ids.emplace(object, *key);
            ATELIER_TRY_BIND(parts, world::components(runtime, object));
            if (parts->size() > maximum_components)
                return std::unexpected(Error{ErrorCode::invalid_storage, "Scene exceeds the Component limit."});
            for (const auto& part : *parts) {
                ATELIER_TRY_BIND(part_key, scene_result(identity(session, part.id())));
                if (!ids.emplace(part.id(), *part_key).second)
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Component identity appears twice."});
            }
        }
        json::Builder builder;
        const auto root = builder.root();
        const auto material_values = *builder.array(root, "materials");
        const auto reference = [&](ObjectId object) -> Result<json::Document> {
            if (object == ObjectId{})
                return json::Builder(json::Kind::null).finish();
            ATELIER_TRY(runtime.type_of(object));
            if (auto at = ids.find(object); at != ids.end())
                return ref_key(at->second);
            if (live) {
                ATELIER_TRY_BIND(key, scene_result(identity(session, object)));
                return ref_key(*key);
            }
            if (persistent)
                return std::unexpected(Error{
                    ErrorCode::invalid_storage,
                    "Object reference leaves the captured graph; supply an explicit binding or include its target."});
            ATELIER_TRY_BIND(id, scene_result(identity(session, object)));
            const auto key = std::to_string(*id);
            if (!graph.external.contains(key)) {
                ATELIER_TRY_BIND(held, runtime.hold(object));
                graph.external.emplace(key, std::move(*held));
            }
            json::Builder external;
            external.set_validated(external.root(), "$external", key);
            return external.finish();
        };
        // Retained data refers to document identities. A target that left the
        // document is written as null; a live target follows the ordinary rules.
        std::unordered_map<std::uint64_t, ObjectId> by_key;
        if (!session.play_ && (!session.retained_.empty() || !session.retained_document_.record.empty()))
            for (const auto& [object, key] : session.local_ids_)
                by_key.emplace(key, object);
        const auto& retained_targets = session.play_ ? session.live_objects_ : by_key;
        const auto present = [&](ObjectId object) {
            if (!runtime.type_of(object))
                return false;
            if (session.world_.contains(object) || session.is_material(object))
                return true;
            const auto component = world::inspect_component(runtime, object);
            return component && component->valid && session.world_.contains(component->owner);
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
                    if (const auto found = retained_targets.find(*key); found != retained_targets.end())
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
            ATELIER_TRY_BIND(key, scene_result(identity(session, object)));
            ids.emplace(object, *key);
            ATELIER_TRY_BIND(held, runtime.hold(object));
            graph.materials.emplace(std::to_string(*key), std::move(*held));
            ATELIER_TRY_BIND(value, append_material_definition(session, object, *key, builder, material_values,
                                                               [&](const ObjectRoot& parent) -> Result<void> {
                                                                   if (parent.registered())
                                                                       ATELIER_TRY(archive_material(parent.id()));
                                                                   return {};
                                                               }));
            ATELIER_TRY(merge_retained(*value, object));
            return *key;
        };
        for (auto object : nodes) {
            ATELIER_TRY_BIND(parts, world::components(runtime, object));
            for (const auto& part : *parts) {
                ATELIER_TRY_BIND(type, runtime.type_of(part.id()));
                ATELIER_TRY_BIND(mesh, runtime.is_a(*type, session.presentation_types_.static_mesh_component));
                if (!*mesh) {
                    ATELIER_TRY_BIND(generator,
                                     runtime.is_a(*type, session.presentation_types_.procedural_generator_component));
                    if (!*generator)
                        continue;
                }
                ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime, part.id()));
                for (const auto& binding : *bindings) {
                    ATELIER_TRY(archive_material(binding.resource.id()));
                }
            }
        }
        if (document)
            for (const auto& material : session.retained_document_.materials)
                if (runtime.type_of(material.id())) {
                    ATELIER_TRY(archive_material(material.id()));
                }
        const auto properties = [&](ObjectId object, TypeId type) -> Result<json::Document> {
            ATELIER_TRY_BIND(fields, business_fields(runtime, session.types_, type));
            return encode_properties(runtime, object, *fields, reference);
        };
        const auto values = *builder.array(root, "entities");
        const auto root_keys = *builder.array(root, "roots");
        for (auto object : roots) {
            const auto key = std::to_string(required_entry(ids, object));
            ATELIER_TRY(scene_result(builder.append(root_keys, key)));
            ATELIER_TRY_BIND(parent, world::parent_entity(runtime, object));
            graph.parents.emplace(key, *parent);
        }
        const auto body_type = runtime.native_type<world::PrimitiveComponent>();
        for (auto object : nodes) {
            ATELIER_TRY_BIND(status, world::inspect_entity(runtime, object));
            const auto entity = builder.append_object(values);
            ATELIER_TRY_BIND(has_transform, world::has_transform(runtime, object));
            if (*has_transform) {
                const auto space = builder.object_validated(entity, "space");
                ATELIER_TRY_BIND(transform, world::read_transform(runtime, object));
                ATELIER_TRY(trs_json(builder, space, *transform));
                ATELIER_TRY_BIND(origin, world::space_reference(runtime, object));
                if (*origin) {
                    if (**origin == ObjectId{})
                        builder.set_validated(space, "origin", "0");
                    else {
                        ATELIER_TRY_BIND(ref, reference(**origin));
                        const auto local = *ref->root().find("$ref");
                        builder.set_validated(space, "origin", local ? *local : ref->root());
                    }
                }
            } else {
                ATELIER_TRY(scene_result(builder.set_null(entity, "space")));
            }
            const auto children = *builder.array(entity, "children");
            const auto parts = *builder.array(entity, "components");
            ATELIER_TRY_BIND(child_objects, world::children(runtime, object));
            for (const auto& child : *child_objects) {
                ATELIER_TRY(scene_result(builder.append(children, std::to_string(required_entry(ids, child.id())))));
            }
            ATELIER_TRY_BIND(components, world::components(runtime, object));
            for (const auto& part : *components) {
                ATELIER_TRY_BIND(state, world::inspect_component(runtime, part.id()));
                const auto part_id = required_entry(ids, part.id());
                const auto key = std::to_string(part_id);
                ATELIER_TRY_BIND(is_light, runtime.is_a(state->type, session.presentation_types_.light_component));
                if (*is_light) {
                    ATELIER_TRY_BIND(light, presentation::read_light(runtime, part.id()));
                    ATELIER_TRY(presentation::validate_light(runtime, *light));
                }
                if (body_type) {
                    ATELIER_TRY_BIND(is_body, runtime.is_a(state->type, *body_type));
                    if (*is_body) {
                        ATELIER_TRY_BIND(body, physics::read_settings(runtime, part.id()));
                        ATELIER_TRY(world::validate_body_settings(runtime, *body));
                        if (!persistent)
                            graph.cooked_shapes.emplace(part_id, std::move(body->cooked_shape));
                    }
                }
                ATELIER_TRY_BIND(type, runtime.describe(state->type));
                auto name = platform::utf16_to_utf8(session.name(part.id()));
                if (!name)
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
                const auto value = builder.append_object(parts);
                builder.set_validated(value, "id", key);
                ATELIER_TRY(scene_result(builder.set(value, "type", std::string_view{(*type)->name})));
                builder.set_validated(value, "name", *name);
                ATELIER_TRY(scene_result(builder.set(value, "enabled", state->enabled)));
                ATELIER_TRY_BIND(record, properties(part.id(), state->type));
                builder.set_validated(value, "properties", record->root());
                const auto encode_material_bindings =
                    [&](json::Value target, std::span<const presentation::MaterialBinding> bindings) -> Result<void> {
                    const auto entries = *builder.array(target, "materials");
                    for (const auto& binding : bindings) {
                        const auto entry = builder.append_object(entries);
                        ATELIER_TRY(scene_result(builder.set(entry, "slot", binding.slot)));
                        builder.set_validated(entry, "resource",
                                              std::to_string(required_entry(ids, binding.resource.id())));
                        ATELIER_TRY(scene_result(builder.set(entry, "overridden", binding.overridden)));
                    }
                    return {};
                };
                ATELIER_TRY_BIND(is_mesh, runtime.is_a(state->type, session.presentation_types_.static_mesh_component));
                if (*is_mesh) {
                    ATELIER_TRY_BIND(mesh, presentation::read_static_mesh(runtime, part.id()));
                    if (persistent && mesh->mesh.resolved && mesh->mesh.source.empty())
                        return std::unexpected(
                            Error{ErrorCode::invalid_storage,
                                  "A procedural mesh has no persistent source; save/import it before archiving."});
                    ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime, part.id()));
                    graph.meshes.emplace(key, MeshCopy{*mesh, *bindings});
                    ATELIER_TRY(scene_result(builder.set(value, "mesh", mesh->mesh.source)));
                    ATELIER_TRY(encode_material_bindings(value, *bindings));
                }
                ATELIER_TRY_BIND(is_generator,
                                 runtime.is_a(state->type, session.presentation_types_.procedural_generator_component));
                if (*is_generator) {
                    const auto overrides = session.generator_overrides(part.id());
                    ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime, part.id()));
                    if (persistent) {
                        ATELIER_TRY_BIND(proc, presentation::read_procedural_source(runtime, part.id()));
                        if (session.is_scratch_graph(proc->source))
                            return std::unexpected(Error{
                                ErrorCode::invalid_storage,
                                "A scratch procedural graph has no persistent source; save it into the Project before archiving."});
                        ATELIER_TRY(scene_result(builder.set(value, "graph", proc->source)));
                    } else {
                        ATELIER_TRY_BIND(proc, presentation::read_procedural_binding(runtime, part.id()));
                        ATELIER_TRY(scene_result(builder.set(value, "graph", proc->source)));
                        graph.generators.emplace(key, GeneratorCopy{std::move(*proc), *bindings, overrides});
                    }
                    if (overrides.root().size() > 0)
                        builder.set_validated(value, "overrides", overrides.root());
                    ATELIER_TRY(encode_material_bindings(value, *bindings));
                }
                ATELIER_TRY_BIND(is_environment,
                                 runtime.is_a(state->type, session.presentation_types_.environment_component));
                if (*is_environment) {
                    ATELIER_TRY_BIND(sky, presentation::read_sky_map(runtime, part.id()));
                    ATELIER_TRY(scene_result(builder.set(value, "sky_map", sky->source)));
                    graph.skies.emplace(key, std::move(*sky));
                }
                ATELIER_TRY(merge_retained(value, part.id()));
            }
            ATELIER_TRY_BIND(entity_type, runtime.describe(status->type));
            const std::string_view entity_type_name{(*entity_type)->name};
            const auto id_text = std::to_string(required_entry(ids, object));
            const auto label = session.name(object);
            std::string name;
            if (label.empty())
                name = std::string(entity_type_name) + " " + id_text;
            else {
                auto converted = platform::utf16_to_utf8(label);
                if (!converted)
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
                name = std::move(*converted);
            }
            builder.set_validated(entity, "id", id_text);
            ATELIER_TRY(scene_result(builder.set(entity, "type", entity_type_name)));
            ATELIER_TRY(scene_result(builder.set(entity, "name", name)));
            ATELIER_TRY(scene_result(builder.set(entity, "enabled", status->enabled)));
            ATELIER_TRY_BIND(record, properties(object, status->type));
            builder.set_validated(entity, "properties", record->root());
            ATELIER_TRY(merge_retained(entity, object));
        }
        ATELIER_TRY(scene_result(builder.set(root, "version", std::uint32_t{10})));
        builder.set_validated(root, "next_id", std::to_string(session.next_local_id_));
        if (document && !session.retained_document_.record.empty()) {
            ATELIER_TRY_BIND(kept, restore_references(session.retained_document_.record));
            overlay(builder, root, kept->root());
        }
        graph.data = builder.finish();
        identities.complete = true;
        return graph;
    }

    // Write parents first, without reading each instance's overrides and texture roots twice.
    template <class Parent>
    static Result<json::Value> append_material_definition(const Session& session, ObjectId object, std::uint64_t key,
                                                          json::Builder& builder, json::Value values,
                                                          Parent&& archive_parent) {
        auto& runtime = session.world_.runtime();
        ATELIER_TRY_BIND(source, material::material_key(runtime, object));
        if (!source->empty()) {
            const auto value = builder.append_object(values);
            builder.set_validated(value, "id", std::to_string(key));
            ATELIER_TRY(scene_result(builder.set(value, "source", *source)));
            return value;
        }
        ATELIER_TRY_BIND(state, material::read_instance(runtime, object));
        const auto authored = session.scene_material_parents_.find(object);
        if (authored == session.scene_material_parents_.end())
            return std::unexpected(Error{ErrorCode::invalid_storage, "Scene instance has no author parent identity."});
        const auto& parent_asset = authored->second;
        if (parent_asset.empty())
            ATELIER_TRY(archive_parent(state->parent));
        const auto value = builder.append_object(values);
        builder.set_validated(value, "id", std::to_string(key));
        builder.set_validated(value, "source", "");
        if (parent_asset.empty()) {
            ATELIER_TRY_BIND(parent_key, scene_result(identity(session, state->parent.id())));
            const auto parent = ref_key(*parent_key);
            builder.set_validated(value, "parent", parent.root());
        } else {
            ATELIER_TRY(scene_result(builder.set(value, "parent", parent_asset)));
        }
        const auto entries = *builder.array(value, "entries");
        ATELIER_TRY(scene_result(assets::material_format::encode_entries(builder, entries, state->entries)));
        return value;
    }

    static Result<void> material_definitions(const Session& session, ObjectId object, json::Builder& builder,
                                             json::Value values, std::unordered_set<ObjectId>& seen) {
        if (!seen.insert(object).second)
            return {};
        ATELIER_TRY_BIND(key, scene_result(identity(session, object)));
        ATELIER_TRY(append_material_definition(session, object, *key, builder, values, [&](const ObjectRoot& parent) {
            return material_definitions(session, parent.id(), builder, values, seen);
        }));
        return {};
    }

    // The graph of an attach change: the Entity record lists only these components.
    static Result<json::Document> live_components(const Session& session, ObjectId entity,
                                                  std::span<const ObjectId> components) {
        for (const auto component : components) {
            ATELIER_TRY_BIND(state, world::inspect_component(session.world_.runtime(), component));
            if (state->owner != entity)
                return std::unexpected(
                    Error{ErrorCode::invalid_storage, "The Component does not belong to the Entity."});
        }
        ATELIER_TRY_BIND(graph, capture(session, {&entity, 1}, true, false, true));
        const auto data = graph->data.root();
        std::unordered_set<std::string> wanted;
        for (const auto component : components) {
            const auto id = session.document_identity(component);
            if (!id)
                return std::unexpected(
                    Error{ErrorCode::invalid_storage, "Each listed Component must appear once on the Entity."});
            wanted.insert(std::to_string(*id));
        }
        const auto entity_id = session.document_identity(entity);
        if (!entity_id)
            fatal("Captured Entity is missing its assigned scene identity.");
        const auto key = std::to_string(*entity_id);
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
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Each listed Component must appear once on the Entity."});
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
    static Result<void> live_push(Session& session, std::string key, json::Value op, material::ImageRef texture = {},
                                  std::shared_ptr<const assets::MeshAsset> mesh = {}) {
        const auto bytes = json::write(op);
        if (bytes.size() > assets::json_format::maximum_bytes)
            return std::unexpected(Error{ErrorCode::invalid_storage, "A Play change exceeds the 16 MiB limit."});
        if (!key.empty())
            std::erase_if(session.live_ops_, [&](const LiveOp& item) { return item.key == key; });
        session.live_ops_.push_back(
            {std::move(key), {{bytes.data(), bytes.size()}, std::move(texture), std::move(mesh)}});
        return {};
    }

    static void finish_forward(Session& session, Result<void> sent) {
        if (!sent) {
            session.live_failure_ = std::move(sent.error());
            session.live_ops_.clear();
        }
    }

    static json::Result<std::string> live_id(const Session& session, ObjectId object) {
        ATELIER_TRY_BIND(key, identity(session, object));
        return std::to_string(*key);
    }

    // The final position among the parent's children, or among the roots.
    static Result<std::size_t> live_position(Session& session, ObjectId object, ObjectId parent) {
        std::size_t index{};
        if (parent != ObjectId{}) {
            ATELIER_TRY_BIND(children, world::children(session.runtime_, parent));
            for (const auto& child : *children) {
                if (child.id() == object)
                    return index;
                ++index;
            }
        } else {
            ATELIER_TRY_BIND(members, session.world_.members());
            for (const auto& member : *members) {
                ATELIER_TRY_BIND(owner, world::parent_entity(session.runtime_, member.id()));
                if (*owner != ObjectId{})
                    continue;
                if (member.id() == object)
                    return index;
                ++index;
            }
        }
        return std::unexpected(Error{ErrorCode::invalid_storage, "The Entity is missing from its parent's list."});
    }

    static Result<json::Document> live_origin(Session& session, ObjectId entity) {
        ATELIER_TRY_BIND(origin, world::space_reference(session.runtime_, entity));
        if (!*origin)
            return json::Builder(json::Kind::null).finish();
        json::Builder builder;
        if (**origin == ObjectId{})
            builder.set_validated(builder.root(), "0");
        else {
            ATELIER_TRY_BIND(id, scene_result(live_id(session, **origin)));
            builder.set_validated(builder.root(), *id);
        }
        return builder.finish();
    }

    static Result<json::Document> live_space(Session& session, ObjectId entity) {
        ATELIER_TRY_BIND(spatial, world::has_transform(session.runtime_, entity));
        if (!*spatial)
            return json::Builder(json::Kind::null).finish();
        ATELIER_TRY_BIND(transform, world::read_transform(session.runtime_, entity));
        json::Builder space;
        ATELIER_TRY(trs_json(space, space.root(), *transform));
        ATELIER_TRY_BIND(origin, live_origin(session, entity));
        space.set_validated(space.root(), "origin", origin->root());
        return space.finish();
    }

    // TRS leaves as bits: 1 position, 2 rotation, 4 scale.
    static unsigned leaf_mask(const world::Trs& a, const world::Trs& b) {
        return (a.position != b.position ? 1u : 0u) | (a.rotation != b.rotation ? 2u : 0u) |
               (a.scale != b.scale ? 4u : 0u);
    }

    static Result<json::Document> space_leaves(const world::Trs& value, unsigned mask) {
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
    static Result<json::Document> live_properties(Session& session, ObjectId object,
                                                  std::span<const world::FieldPath> paths, std::string* key = nullptr) {
        auto& runtime = session.runtime_;
        ATELIER_TRY_BIND(type, runtime.type_of(object));
        ATELIER_TRY_BIND(fields, detail::author_fields(runtime, *type));
        json::Builder op;
        const auto root = op.root();
        op.set_validated(root, "op", "set");
        ATELIER_TRY_BIND(id, scene_result(live_id(session, object)));
        op.set_validated(root, "id", *id);
        const auto properties = op.object_validated(root, "properties");
        if (key)
            *key = "field " + *id;
        for (const auto& path : paths) {
            const auto field =
                std::ranges::find_if(*fields, [&](const auto& item) { return std::ranges::equal(item.path, path); });
            if (field == fields->end())
                return std::unexpected(Error{ErrorCode::invalid_storage, "A changed field is not a scene property."});
            if (key)
                *key += " " + detail::dotted(field->names);
            const auto at = op.object_path_validated(properties, field->names);
            if (field->form == ValueForm::strong_reference || field->form == ValueForm::weak_reference) {
                ATELIER_TRY_BIND(reference, runtime.read_reference(object, path));
                if (reference->is_null()) {
                    const auto saved = json::Builder(json::Kind::null).finish();
                    op.replace(at, saved.root());
                } else {
                    ATELIER_TRY_BIND(id, scene_result(identity(session, reference->id())));
                    const auto saved = ref_key(*id);
                    op.replace(at, saved.root());
                }
            } else {
                ATELIER_TRY_BIND(scalar, runtime.read(object, path));
                ATELIER_TRY(encode_scalar(runtime, op, at, *scalar));
            }
        }
        if (!properties.size())
            op.erase(root, "properties");
        return op.finish();
    }

    static Result<json::Document> live_body(Session& session, ObjectId component) {
        auto& runtime = session.runtime_;
        json::Builder op;
        const auto root = op.root();
        op.set_validated(root, "op", "set");
        ATELIER_TRY_BIND(id, scene_result(live_id(session, component)));
        op.set_validated(root, "id", *id);
        const auto properties = op.object_validated(root, "properties");
        const auto body = op.object_validated(properties, "body");
        ATELIER_TRY_BIND(type, runtime.type_of(component));
        ATELIER_TRY_BIND(fields, business_fields(runtime, session.types_, *type));
        for (const auto& field : *fields)
            if (field.form == ValueForm::inline_value && field.names.size() > 1 && field.names.front() == "body") {
                const auto at = op.object_path_validated(body, std::span(field.names).subspan(1));
                ATELIER_TRY_BIND(scalar, runtime.read(component, field.path));
                ATELIER_TRY(encode_scalar(runtime, op, at, *scalar));
            }
        return op.finish();
    }

    // Scene and Play changes decode a properties record leaf by leaf through the shared decoder.
    using SceneResolve = std::function<SceneResult<std::optional<ObjectReference>>(json::Value, TypeId, ValueForm)>;

    static SceneResult<void> read_properties(Runtime& runtime, const PropertyTarget& target, json::Value record,
                                             Load& load, const SceneResolve& resolve,
                                             std::span<const detail::ScalarField> fields,
                                             std::span<const detail::FieldNames> transient_paths,
                                             const WriteLeaf& write = {}) {
        const auto keep_value = [&](std::vector<std::string> at, json::Value item, world::DiagnosticKind kind,
                                    const std::string& detail) {
            return load.keep(target.object, std::move(at), item, kind, detail);
        };
        const auto converted_value = [&](std::vector<std::string> at, json::Value item) {
            return load.report(world::Severity::info, world::DiagnosticKind::converted, target.object, std::move(at),
                               &item, "Integral number converted exactly to the field's integer type.");
        };
        const struct {
            decltype(keep_value)& keep;

            decltype(converted_value)& converted;
        } report{keep_value, converted_value};
        return read_record<SceneFailure>(runtime, target, fields, record, {"properties"}, resolve, write, &report,
                                         transient_paths);
    }

    // Scene instances share the asset entry codec, but their parent may have a
    // document-local identity. The directory resolver handles ordering/cycles.
    static SceneResult<ObjectRoot>
    restore_material(Session& session, Project* project, json::Value entry,
                     const std::function<SceneResult<ObjectRoot>(std::uint64_t)>& resolve,
                     std::vector<MissingResource>* missing = nullptr, ObjectId existing = {}) {
        auto& runtime = session.runtime_;
        if (!project)
            return std::unexpected(SceneFailure("Material restoration requires a Project."));
        ATELIER_TRY_BIND(source, entry.string({"source"}));
        if (!source->empty()) {
            if (*entry.find("parent") || *entry.find("entries"))
                return std::unexpected(SceneFailure("Material binding has competing sources."));
            ATELIER_TRY_BIND(root, session.material_resource(project->assets(), std::string(*source), missing));
            if (existing != ObjectId{} && root->id() != existing)
                return std::unexpected(SceneFailure("Live material identity cannot change its asset."));
            return std::move(*root);
        }
        material::InstanceState state;
        std::string parent_key;
        ATELIER_TRY_BIND(parent, entry.at("parent"));
        if (parent->kind() == json::Kind::string) {
            parent_key = *parent->string();
            if (parent_key.empty())
                return std::unexpected(SceneFailure("A scene instance requires a parent identity."));
            ATELIER_TRY_BIND(parent_asset, session.material_resource(project->assets(), parent_key, missing));
            state.parent = std::move(*parent_asset);
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
        ATELIER_TRY_BIND(surface, material::capture_program(runtime, state.parent.id()));
        state.textures.resize(state.entries.size());
        for (std::size_t i = 0; i < state.entries.size(); ++i) {
            const auto& item = state.entries[i];
            const auto* binding = std::get_if<material::TextureBinding>(&item.value);
            if (!item.enabled || !binding || binding->asset_key.empty() ||
                std::ranges::find((*surface)->program().textures(), item.path, &source::MaterialTextureInput::key) ==
                    (*surface)->program().textures().end())
                continue;
            ATELIER_TRY_BIND(texture, session.texture_resource(project->assets(), binding->asset_key, missing, i));
            state.textures[i] = std::move(*texture);
        }
        ATELIER_TRY_BIND(root, existing == ObjectId{} ? material::create_instance(runtime, {}, state)
                                                      : runtime.hold(existing));
        if (existing != ObjectId{}) {
            ATELIER_TRY_BIND(key, material::material_key(runtime, existing));
            if (!key->empty())
                return std::unexpected(SceneFailure("Live scene instance cannot replace a named asset."));
            ATELIER_TRY(material::update_instance(runtime, existing, state));
        }
        session.scene_material_parents_.insert_or_assign(root->id(), std::move(parent_key));
        return std::move(*root);
    }

    // A Play copy's object for a document identity. Unknown identities mean Play
    // diverged; identities whose object the game destroyed are skipped.

    static std::optional<ObjectId> live_object(const Session& session, std::uint64_t key) {
        const auto at = session.live_objects_.find(key);
        if (at == session.live_objects_.end())
            return std::nullopt;
        return session.runtime_.type_of(at->second) ? std::optional(at->second) : std::optional(ObjectId{});
    }

    static SceneResult<ObjectId> live_target(const Session& session, std::uint64_t key, ObjectId* subject = nullptr) {
        const auto at = session.live_objects_.find(key);
        if (at == session.live_objects_.end())
            return std::unexpected(
                SceneFailure("Document identity #" + std::to_string(key) + " does not exist in Play."));
        if (subject)
            *subject = at->second;
        if (!session.runtime_.type_of(at->second))
            return std::unexpected(SceneFailure("#" + std::to_string(key) + " was destroyed by the game.",
                                                SceneFailure::Disposition::destroyed));
        return at->second;
    }

    // A reference leaving a live graph; nullopt when the game destroyed its target.
    static SceneResult<std::optional<ObjectId>> live_reference(const Session& session, std::uint64_t key, Load& load) {
        const auto object = live_object(session, key);
        if (!object)
            return std::unexpected(
                SceneFailure("Reference target #" + std::to_string(key) + " does not exist in Play."));
        if (*object == ObjectId{}) {
            load.dropped.push_back(key);
            return std::nullopt;
        }
        return object;
    }

    // A document load does not read a key that already failed again for each Environment that
    // names it; Play reads it again, since the editor may send it after the file was restored.
    static SceneResult<rendering::SkyMapRef> resolve_sky(Session& session, Project* project, const std::string& source,
                                                         Load& load, ObjectId object) {
        if (source.empty())
            return {};
        if (!project)
            return std::unexpected(SceneFailure("Sky map restoration requires a Project."));
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
        ATELIER_TRY(load.report(world::Severity::warning, item->kind, object, {"sky_map"}, &value, item->error.message,
                                &item->error));
        return {};
    }

    static SceneResult<Prepared> construct(Session& session, const Graph& graph, Project* project, bool keep_ids,
                                           Load& load) {
        const auto data = graph.data.root();
        ATELIER_TRY_BIND(version, data.u64({"version"}));
        if (*version != 10)
            return std::unexpected(SceneFailure("Object graph requires scene format version 10."));
        ATELIER_TRY_BIND(members, data.object_items());
        for (const auto& [name, value] : *members)
            if (name != "version" && name != "next_id" && name != "roots" && name != "entities" &&
                name != "materials") {
                if (load.document)
                    ATELIER_TRY(load.keep({}, {std::string(name)}, value, world::DiagnosticKind::unknown_key,
                                          "Unknown scene key kept from the saved scene."));
                else
                    ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::unknown_key, {},
                                            {std::string(name)}, &value, "Unknown scene key is not imported."));
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
            return std::unexpected(SceneFailure("Invalid Entity graph directory."));
        if (!keep_ids) {
            ATELIER_TRY_BIND(existing, session.world_.members());
            if (existing->size() + entities.size() > maximum_entities)
                return std::unexpected(SceneFailure("Editor Entity limit reached."));
        }
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
        const auto create = [&](json::Value value, bool entity, ObjectId owner) -> SceneResult<ObjectId> {
            ATELIER_TRY_BIND(id, value.at("id"));
            ATELIER_TRY_BIND(parsed_key, local_key(*id));
            const auto key = *parsed_key;
            if (ids.contains(key))
                return std::unexpected(SceneFailure("Duplicate graph object identity: " + std::to_string(key)));
            if (load.live)
                if (const auto live = live_object(session, key); live && *live != ObjectId{})
                    return std::unexpected(
                        SceneFailure("Document identity #" + std::to_string(key) + " already exists in Play."));
            largest = std::max(largest, key);
            ATELIER_TRY_BIND(type_name_value, value.string({"type"}));
            const auto name = *type_name_value;
            const auto base = entity ? session.types_.entity : session.types_.component;
            const auto found = runtime.find_type(name);
            const auto matches = found ? runtime.is_a(*found, base) : Result<bool>{false};
            const bool placeholder = !matches || !*matches;
            const auto type = !placeholder                     ? *found
                              : !entity && *value.find("mesh") ? session.presentation_types_.static_mesh_component
                              : !entity && *value.find("graph")
                                  ? session.presentation_types_.procedural_generator_component
                                  : base;
            ATELIER_TRY_BIND(root,
                             entity ? world::create_entity(runtime, type) : world::create_component(runtime, type));
            const auto object = root->id();
            ids.emplace(key, object);
            descriptors.emplace(key, value);
            records.emplace(object, value);
            result.objects.push_back(std::move(*root));
            if (keep_ids)
                session.local_ids_.emplace(object, key);
            else
                ATELIER_TRY(identity(session, object));
            load.owners.emplace(object, entity ? object : owner);
            load.types.emplace(object, name);
            load.keys.emplace(object, key);
            if (placeholder) {
                ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::unknown_type, object, {},
                                        nullptr,
                                        std::string(entity ? "Entity" : "Component") +
                                            " type is not registered; a placeholder keeps its saved data."));
                auto& kept = load.pending[object];
                kept.set_validated(kept.root(), "type", name);
            }
            std::wstring label;
            if (const auto at = *value.find("name")) {
                std::string problem;
                if (at->kind() != json::Kind::string)
                    problem = "Expected a name string.";
                else {
                    label = platform::utf8_to_utf16_validated(*at->string());
                    if (entity || !label.empty())
                        if (auto valid = validate_name(label); !valid)
                            problem = std::move(valid.error().message);
                }
                if (!problem.empty()) {
                    label.clear();
                    ATELIER_TRY(load.keep(object, {"name"}, *at, world::DiagnosticKind::mismatch,
                                          problem + " A generated name is used and the saved value is kept."));
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
                    return std::unexpected(SceneFailure("Duplicate graph object identity: " + std::to_string(key)));
                if (entity.at("children")->size())
                    return std::unexpected(SceneFailure("An existing Entity record cannot add children."));
                ATELIER_TRY_BIND(target, live_target(session, key));
                object = *target;
                largest = std::max(largest, key);
                anchors.insert(key);
                ids.emplace(key, object);
                descriptors.emplace(key, entity);
                records.emplace(object, entity);
                load.owners.emplace(object, object);
                ATELIER_TRY_BIND(actual_type, runtime.type_of(object));
                ATELIER_TRY_BIND(info, runtime.describe(*actual_type));
                load.types.emplace(object, std::string_view{(*info)->name});
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
                return std::unexpected(SceneFailure("Invalid Component count."));
            for (auto part : *parts) {
                ATELIER_TRY_BIND(created, create(part, false, object));
                const auto component = *created;
                ATELIER_TRY_BIND(component_type, runtime.type_of(component));
                ATELIER_TRY_BIND(mesh,
                                 runtime.is_a(*component_type, session.presentation_types_.static_mesh_component));
                if (*mesh) {
                    ATELIER_TRY(load.extra(component, part,
                                           {"id", "type", "name", "enabled", "properties", "mesh", "materials"}));
                    continue;
                }
                ATELIER_TRY_BIND(generator, runtime.is_a(*component_type,
                                                         session.presentation_types_.procedural_generator_component));
                if (*generator) {
                    ATELIER_TRY(
                        load.extra(component, part,
                                   {"id", "type", "name", "enabled", "properties", "graph", "overrides", "materials"}));
                    continue;
                }
                ATELIER_TRY_BIND(environment,
                                 runtime.is_a(*component_type, session.presentation_types_.environment_component));
                if (*environment)
                    ATELIER_TRY(
                        load.extra(component, part, {"id", "type", "name", "enabled", "properties", "sky_map"}));
                else
                    ATELIER_TRY(load.extra(component, part, {"id", "type", "name", "enabled", "properties"}));
            }
        }
        // A stand-in keeps the key, so saving writes the reference unchanged.
        const auto report_missing = [&](ObjectId object, std::vector<std::string> base, json::Value entry,
                                        const std::vector<MissingResource>& missing) -> SceneResult<void> {
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
                ATELIER_TRY(load.report(world::Severity::warning, item.kind, object, std::move(at), &value,
                                        item.error.message, &item.error));
            }
            return {};
        };
        std::unordered_map<std::uint64_t, ObjectRoot> material_ids;
        std::unordered_set<ObjectId> material_objects;
        std::unordered_map<ObjectId, std::uint64_t> copied_material_keys;
        std::unordered_set<std::uint64_t> used_materials;
        const json::Builder empty_array(json::Kind::array);
        const auto material_value = *data.find("materials");
        ATELIER_TRY_BIND(material_array, (material_value ? *material_value : empty_array.root()).array());
        const auto definitions = *material_array;
        if (definitions.size() > maximum_entities * maximum_components)
            return std::unexpected(SceneFailure("Invalid scene material directory."));
        std::unordered_map<std::uint64_t, json::Value> material_records;
        for (auto definition : definitions) {
            ATELIER_TRY_BIND(id, definition.at("id"));
            ATELIER_TRY_BIND(parsed_key, local_key(*id));
            const auto key = *parsed_key;
            if (ids.contains(key) || !material_records.emplace(key, definition).second)
                return std::unexpected(SceneFailure("Duplicate graph object identity: " + std::to_string(key)));
            largest = std::max(largest, key);
        }
        std::unordered_set<std::uint64_t> resolving_materials;
        std::function<SceneResult<ObjectRoot>(std::uint64_t)> restore;
        restore = [&](std::uint64_t key) -> SceneResult<ObjectRoot> {
            if (const auto at = material_ids.find(key); at != material_ids.end())
                return at->second;
            const auto record = material_records.find(key);
            if (record == material_records.end())
                return std::unexpected(SceneFailure("Material parent references an unknown scene resource."));
            if (!resolving_materials.insert(key).second)
                return std::unexpected(SceneFailure("Scene material parent cycle."));
            const auto definition = record->second;
            ObjectRoot resource;
            std::vector<MissingResource> missing;
            // Play keeps a material it already has; its value changes travel separately.
            const auto live = load.live ? live_object(session, key) : std::nullopt;
            const bool reused = live && *live != ObjectId{};
            if (reused) {
                if (!session.is_material(*live))
                    return std::unexpected(
                        SceneFailure("Document identity #" + std::to_string(key) + " is not a material in Play."));
                ATELIER_TRY_BIND(held, runtime.hold(*live));
                resource = std::move(*held);
            } else if (auto at = graph.materials.find(std::to_string(key)); at != graph.materials.end()) {
                ATELIER_TRY_BIND(asset_key, material::material_key(runtime, at->second.id()));
                if (asset_key->empty()) {
                    ATELIER_TRY_BIND(state, material::read_instance(runtime, at->second.id()));
                    ATELIER_TRY_BIND(parent, definition.at("parent"));
                    if (reference_value(*parent)) {
                        ATELIER_TRY_BIND(ref, parent->at("$ref"));
                        ATELIER_TRY_BIND(parent_key, local_key(*ref));
                        ATELIER_TRY_BIND(restored, restore(*parent_key));
                        state->parent = std::move(*restored);
                    }
                    ATELIER_TRY_BIND(held, material::create_instance(runtime, {}, *state));
                    resource = std::move(*held);
                    session.scene_material_parents_.emplace(
                        resource.id(), required_entry(session.scene_material_parents_, at->second.id()));
                } else
                    resource = at->second;
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
                return std::unexpected(SceneFailure("One material asset has multiple document identities."));
            ids.emplace(key, resource.id());
            material_ids.emplace(key, resource);
            result.resources.push_back(resource);
            if (keep_ids && !reused) {
                if (!session.local_ids_.emplace(resource.id(), key).second)
                    return std::unexpected(SceneFailure("One material asset has multiple document identities."));
            } else if (!keep_ids)
                ATELIER_TRY(identity(session, resource.id()));
            ATELIER_TRY_BIND(actual_type, runtime.type_of(resource.id()));
            ATELIER_TRY_BIND(info, runtime.describe(*actual_type));
            load.types.emplace(resource.id(), std::string_view{(*info)->name});
            load.keys.emplace(resource.id(), key);
            ATELIER_TRY(report_missing(resource.id(), {}, definition, missing));
            ATELIER_TRY(load.extra(resource.id(), definition, {"id", "source", "parent", "entries"}));
            ATELIER_TRY_BIND(surface, material::capture_program(runtime, resource.id()));
            // Sparse entries may intentionally be disabled or outlive their declaration.
            // Required resources and invalid Source definitions were checked above;
            // resolution diagnostics describe preserved author intent, not load loss.
            for (const auto& issue : (*surface)->diagnostics())
                ATELIER_TRY(load.report(world::Severity::info, world::DiagnosticKind::mismatch, resource.id(),
                                        {"entries", issue.path}, nullptr, issue.message));
            resolving_materials.erase(key);
            return resource;
        };
        for (auto definition : definitions) {
            ATELIER_TRY(restore(*local_key(*definition.at("id"))));
        }
        if (next <= largest)
            return std::unexpected(SceneFailure("next_id must exceed every document object identity."));
        if (keep_ids && !load.live)
            session.next_local_id_ = next;
        std::unordered_map<std::uint64_t, std::uint64_t> owners;
        for (const auto& entity : entities) {
            const auto key = *local_key(*entity.at("id"));
            const auto object = required_entry(ids, key);
            std::vector<world::Attachment> parts;
            const auto components = *entity.at("components")->array();
            for (auto part : components)
                parts.push_back({required_entry(ids, *local_key(*part.at("id")))});
            // A running Entity begins new components on attachment, after they are restored.
            if (anchors.contains(key))
                result.anchored.emplace_back(object, std::move(parts));
            else if (!parts.empty())
                ATELIER_TRY(committed(world.attach_components(object, parts)));
            if (const auto space = *entity.find("space"); space && !space->is_null()) {
                world::Trs trs;
                if (space->kind() != json::Kind::object)
                    ATELIER_TRY(load.keep(
                        object, {"space"}, *space, world::DiagnosticKind::mismatch,
                        "Expected a space record. The identity transform is used and the saved value is kept."));
                else {
                    ATELIER_TRY(load.extra(object, *space, {"position", "rotation", "scale", "origin"}, {"space"}));
                    const auto decoded = json_trs(*space);
                    if (decoded)
                        trs = *decoded;
                    else {
                        // The shared canonical check decides validity; origin stays structural.
                        ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                                {"space"}, &*space,
                                                decoded.error().describe() +
                                                    " The identity transform is used and the saved values are kept."));
                        trs = {};
                        for (const auto name : {"position", "rotation", "scale"})
                            if (const auto at = *space->find(name))
                                load.store(object, std::array{std::string("space"), std::string(name)}, *at);
                    }
                }
                ATELIER_TRY(world.set_transform(object, trs));
            }
            ATELIER_TRY_BIND(children_value, entity.at("children"));
            ATELIER_TRY_BIND(children, children_value->array());
            if (children->size() > entities.size())
                return std::unexpected(SceneFailure("Invalid child directory."));
            for (auto child : *children) {
                ATELIER_TRY_BIND(id, local_key(child));
                if (!entity_ids.contains(*id) || !owners.emplace(*id, key).second)
                    return std::unexpected(SceneFailure("Entity child has a wrong type or multiple parents."));
            }
        }
        std::unordered_set<std::uint64_t> roots;
        std::unordered_set<std::uint64_t> visited;
        const auto visit = [&](auto&& self, std::uint64_t key) -> SceneResult<void> {
            if (!visited.insert(key).second)
                return std::unexpected(SceneFailure("Entity composition contains a cycle or duplicate."));
            const auto children = *required_entry(descriptors, key).at("children")->array();
            for (auto child : children)
                ATELIER_TRY(self(self, *local_key(child)));
            return {};
        };
        for (auto value : root_values) {
            ATELIER_TRY_BIND(parsed_key, local_key(value));
            const auto key = *parsed_key;
            if (!entity_ids.contains(key) || owners.contains(key) || !roots.insert(key).second)
                return std::unexpected(SceneFailure("Invalid graph root."));
            ATELIER_TRY(visit(visit, key));
            result.roots.push_back(required_entry(ids, key));
        }
        if (visited.size() != entity_ids.size())
            return std::unexpected(SceneFailure("Entity graph has unreachable nodes or a cycle."));
        // All identities exist before relations or business references are restored.
        for (auto entity : entities) {
            const auto children = *entity.at("children")->array();
            for (auto child : children)
                ATELIER_TRY(committed(world.reparent(required_entry(ids, *local_key(child)),
                                                     required_entry(ids, *local_key(*entity.at("id"))),
                                                     world::ReparentRule::keep_local)));
        }
        // Dangling or malformed identities are structural damage. A target whose
        // type does not satisfy the field is schema drift: nullopt, not an error.
        const auto locate = [&](json::Value value) -> SceneResult<std::optional<ObjectId>> {
            ObjectId object;
            if (value.kind() == json::Kind::object && *value.find("$ref")) {
                ATELIER_TRY(value.members({"$ref"}));
                ATELIER_TRY_BIND(parsed_key, local_key(*value.at("$ref")));
                const auto key = *parsed_key;
                if (const auto found = ids.find(key); found != ids.end())
                    object = found->second;
                else if (!load.live)
                    return std::unexpected(
                        SceneFailure("Reference target is outside this graph: " + std::to_string(key)));
                else {
                    ATELIER_TRY_BIND(live, live_reference(session, key, load));
                    if (!*live)
                        return std::optional{ObjectId{}};
                    object = **live;
                }
                if (material_ids.contains(key))
                    used_materials.insert(key);
            } else if (value.kind() == json::Kind::object && *value.find("$external")) {
                ATELIER_TRY(value.members({"$external"}));
                ATELIER_TRY_BIND(key, value.string({"$external"}));
                const auto at = graph.external.find(std::string(*key));
                if (at == graph.external.end())
                    return std::unexpected(SceneFailure("External object references cannot be loaded from a file."));
                object = at->second.id();
            } else
                return std::nullopt;
            return std::optional{object};
        };
        const SceneResolve resolve = [&](json::Value value, TypeId target,
                                         ValueForm form) -> SceneResult<std::optional<ObjectReference>> {
            if (value.is_null()) {
                ATELIER_TRY_BIND(reference, runtime.null_reference(target, form == ValueForm::weak_reference
                                                                               ? form
                                                                               : ValueForm::strong_reference));
                return std::optional{*reference};
            }
            ATELIER_TRY_BIND(found, locate(value));
            if (!*found)
                return std::nullopt;
            if (**found == ObjectId{}) {
                ATELIER_TRY_BIND(reference, runtime.null_reference(target, form == ValueForm::weak_reference
                                                                               ? form
                                                                               : ValueForm::strong_reference));
                return std::optional{*reference};
            }
            auto held = runtime.hold(**found, target);
            if (!held)
                return std::nullopt;
            if (form == ValueForm::weak_reference) {
                ATELIER_TRY_BIND(reference, runtime.observe(held->reference()));
                return std::optional{*reference};
            }
            return std::optional{held->reference()};
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
                            target = key ? required_entry(ids, key) : ObjectId{};
                        else if (!load.live)
                            return std::unexpected(
                                SceneFailure("Space reference must target an Entity in this graph."));
                        else {
                            ATELIER_TRY_BIND(live, live_reference(session, key, load));
                            target = *live;
                        }
                    } else {
                        ATELIER_TRY_BIND(found, locate(*origin));
                        if (!*found || (**found != ObjectId{} && !runtime.hold(**found, session.types_.entity)))
                            return std::unexpected(
                                SceneFailure("Space reference must target an Entity in this graph."));
                        target = **found;
                    }
                }
            result.spaces.push_back({required_entry(ids, *local_key(*entity.at("id"))), target});
        }
        using Names = detail::FieldNames;
        using DefaultValue = std::variant<Scalar, String, ArrayLeaf, ObjectReference, std::vector<ObjectReference>>;
        struct DefaultField {
            Names names;

            world::FieldPath path;

            DefaultValue value;
        };
        using Defaults = std::vector<DefaultField>;
        const auto find_path = [](json::Value value, std::span<const std::string> path) -> std::optional<json::Value> {
            for (const auto& name : path) {
                if (value.kind() != json::Kind::object)
                    return {};
                const auto child = *value.find(name);
                if (!child)
                    return {};
                value = *child;
            }
            return value;
        };
        const auto default_value = [&](const PropertyTarget& target,
                                       const detail::ScalarField& field) -> SceneResult<DefaultValue> {
            const auto object = target.object;
            if (field.form == ValueForm::inline_value) {
                if (field.kind == TypeKind::array) {
                    ATELIER_TRY_BIND(array, ArrayLeaf::create(runtime, field.type));

                    ATELIER_TRY(runtime.read(object, field.path, array->output()));
                    return DefaultValue{std::move(*array)};
                }
                if (field.kind == TypeKind::string) {
                    ATELIER_TRY_BIND(text, encode_string(runtime, target, field));
                    return DefaultValue{std::move(*text)};
                }
                ATELIER_TRY_BIND(scalar, runtime.read(object, field.path));
                return DefaultValue{std::move(*scalar)};
            }
            if (field.form != ValueForm::strong_sequence) {
                ATELIER_TRY_BIND(ref, runtime.read_reference(object, field.path));
                return DefaultValue{std::move(*ref)};
            }
            ATELIER_TRY_BIND(locator, runtime.reference_field(object, field.path));
            ATELIER_TRY_BIND(count, runtime.sequence_size(*locator));
            std::vector<ObjectReference> references;
            references.reserve(*count);
            for (std::size_t i = 0; i < *count; ++i) {
                ATELIER_TRY_BIND(ref, runtime.sequence_read(*locator, i));
                references.push_back(std::move(*ref));
            }
            return DefaultValue{std::move(references)};
        };
        const auto properties = [&](json::Value value, ObjectId object, TypeId type) -> SceneResult<Defaults> {
            Defaults defaults;
            if (const auto at = *value.find("properties")) {
                if (at->kind() == json::Kind::object) {
                    const PropertyTarget target{type, object};
                    std::vector<Names> transient;
                    ATELIER_TRY_BIND(fields,
                                     business_fields(runtime, session.types_, type, load.live ? nullptr : &transient));
                    // Capture this instance's constructor values only for leaves the file may overwrite.
                    if (load.policy == LoadPolicy::tolerant) {
                        ATELIER_TRY_BIND(info, runtime.describe(type));
                        if ((*info)->has_validation_rules)
                            for (const auto& field : *fields)
                                if (find_path(*at, field.names)) {
                                    ATELIER_TRY_BIND(value, default_value(target, field));
                                    defaults.push_back({field.names, field.path, std::move(*value)});
                                }
                    }
                    ATELIER_TRY(read_properties(runtime, target, *at, load, resolve, *fields, transient));
                } else
                    ATELIER_TRY(
                        load.keep(object, {"properties"}, *at, world::DiagnosticKind::mismatch,
                                  "Expected a property record. Defaults are used and the saved value is kept."));
            }
            bool enabled = true;
            if (const auto at = *value.find("enabled"); !at)
                ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object, {"enabled"},
                                        nullptr, "The enabled flag is missing; the object is enabled."));
            else if (at->kind() != json::Kind::boolean)
                ATELIER_TRY(load.keep(object, {"enabled"}, *at, world::DiagnosticKind::mismatch,
                                      "Expected a boolean. The object is enabled and the saved value is kept."));
            else
                enabled = *at->boolean();
            ATELIER_TRY(committed(world.set_enabled(object, enabled)));
            return defaults;
        };
        const auto invalid_record = [&](ObjectId object, std::span<const std::string> names, const Defaults& defaults,
                                        const std::string& problem) -> SceneResult<void> {
            Names path{"properties"};
            path.insert(path.end(), names.begin(), names.end());
            const auto saved = find_path(required_entry(records, object), path);
            ATELIER_TRY(load.report(
                world::Severity::warning, world::DiagnosticKind::mismatch, object, path, saved ? &*saved : nullptr,
                load.policy == LoadPolicy::strict ? problem : problem + " Record restored to its defaults."));
            if (const auto entry = load.pending.find(object); entry != load.pending.end()) {
                path.insert(path.begin(), "record");
                detail::erase_path(entry->second, entry->second.root(), path);
                if (entry->second.root().size() == 0)
                    load.pending.erase(entry);
            }
            for (const auto& field : defaults) {
                if (!std::ranges::starts_with(field.names, names))
                    continue;
                const auto& path = field.path;
                const auto& value = field.value;
                if (const auto* scalar = std::get_if<Scalar>(&value))
                    ATELIER_TRY(runtime.write(object, path, *scalar));
                else if (const auto* array = std::get_if<ArrayLeaf>(&value))
                    ATELIER_TRY(runtime.write(object, path, array->input()));
                else if (const auto* text = std::get_if<String>(&value)) {
                    ATELIER_TRY_BIND(type, runtime.native_type<String>());
                    ATELIER_TRY(runtime.write(object, path, ValueInput{*type, text}));
                } else if (const auto* reference = std::get_if<ObjectReference>(&value))
                    ATELIER_TRY(runtime.write_reference(object, path, *reference));
                else {
                    ATELIER_TRY_BIND(locator, runtime.reference_field(object, path));
                    ATELIER_TRY_BIND(count, runtime.sequence_size(*locator));
                    const ReferenceChange change =
                        SequenceSplice{*locator, 0, *count, std::get<std::vector<ObjectReference>>(value)};
                    ATELIER_TRY(runtime.apply_reference_changes({&change, 1}));
                }
            }
            return {};
        };
        const auto validate = [&](ObjectId object, const Defaults& defaults) -> SceneResult<void> {
            ATELIER_TRY_BIND(failure_result, runtime.validate(object));
            auto& failure = *failure_result;
            while (failure) {
                auto names = failure->names;
                // Arrays and other aggregate leaves have one saved value for all their elements.
                for (const auto& field : defaults)
                    if (field.names.size() < names.size() && std::ranges::starts_with(names, field.names)) {
                        names = field.names;
                        break;
                    }
                ATELIER_TRY(invalid_record(object, names, defaults, failure->error.message));
                ATELIER_TRY_BIND(next_result, runtime.validate(object));
                auto& next = *next_result;
                if (next && std::ranges::starts_with(next->names, names))
                    return std::unexpected(SceneFailure("Scene type defaults fail validation: " + next->error.message));
                failure = std::move(next);
            }
            return {};
        };
        const auto copy_bindings = [&](const std::vector<presentation::MaterialBinding>& source) {
            auto bindings = source;
            for (auto& binding : bindings) {
                const auto key = required_entry(copied_material_keys, binding.resource.id());
                used_materials.insert(key);
                binding.resource = required_entry(material_ids, key);
            }
            return bindings;
        };
        // Without its mesh a component has no slots. Its binding table is kept verbatim,
        // with scene material references renamed to document identities kept alive by it.
        const auto keep_bindings = [&](ObjectId object, json::Value entries) -> SceneResult<void> {
            if (!entries.size())
                return {};
            if (entries.size() > 1024)
                return std::unexpected(SceneFailure("Invalid material binding table."));
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
                    return std::unexpected(SceneFailure("Material binding references an unknown scene resource."));
                used_materials.insert(key);
                held.push_back(found->second);
                ATELIER_TRY_BIND(id, identity(session, found->second.id()));
                kept.set_validated(**at, std::to_string(*id));
            }
            load.store(object, std::array{std::string("materials")}, kept.root());
            return {};
        };
        const auto decode_material_bindings =
            [&](ObjectId object, json::Value part, bool unresolved, bool warn_if_missing, auto&& slot_matches,
                const char* slot_mismatch_message) -> SceneResult<std::vector<presentation::MaterialBinding>> {
            std::vector<presentation::MaterialBinding> bindings;
            const auto entries = *part.find("materials");
            if (!entries) {
                if (warn_if_missing)
                    ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                            {"materials"}, nullptr,
                                            "The material binding table is missing; bindings are empty."));
                return bindings;
            }
            if (entries->kind() != json::Kind::array) {
                ATELIER_TRY(
                    load.keep(object, {"materials"}, *entries, world::DiagnosticKind::mismatch,
                              "Expected a material binding table. Bindings are empty and the saved value is kept."));
                return bindings;
            }
            if (unresolved) {
                ATELIER_TRY(keep_bindings(object, *entries));
                return bindings;
            }
            if (entries->size() > 1024)
                return std::unexpected(SceneFailure("Invalid material binding table."));
            const auto items = *entries->array();
            for (auto entry : items) {
                ATELIER_TRY_BIND(slot_value, entry.string({"slot"}));
                const auto slot = std::string(*slot_value);
                const auto members = *entry.object_items();
                for (const auto& [name, item] : members)
                    if (name != "slot" && name != "resource" && name != "overridden")
                        ATELIER_TRY(load.keep_slot(object, slot, std::string(name), item,
                                                   world::DiagnosticKind::unknown_key,
                                                   "Unknown binding key kept from the saved scene."));
                ATELIER_TRY_BIND(resource_value, entry.at("resource"));
                ATELIER_TRY_BIND(resource_key, local_key(*resource_value));
                const auto resource_id = *resource_key;
                const auto at = material_ids.find(resource_id);
                if (at == material_ids.end())
                    return std::unexpected(SceneFailure("Material binding references an unknown scene resource."));
                used_materials.insert(resource_id);
                ObjectRoot resource = at->second;
                bool overridden{};
                if (const auto flag = *entry.find("overridden"); !flag)
                    ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                            {"materials", slot, "overridden"}, nullptr,
                                            "The override flag is missing; the slot is not overridden."));
                else if (flag->kind() != json::Kind::boolean)
                    ATELIER_TRY(
                        load.keep_slot(object, slot, "overridden", *flag, world::DiagnosticKind::mismatch,
                                       "Expected a boolean. The slot is not overridden and the saved value is kept."));
                else
                    overridden = *flag->boolean();
                bindings.push_back({slot, std::move(resource), overridden});
            }
            auto kept = bindings.begin();
            for (auto at = bindings.begin(); at != bindings.end(); ++at) {
                if (slot_matches(at->slot)) {
                    if (kept != at)
                        *kept = std::move(*at);
                    ++kept;
                } else {
                    ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                            {"materials", at->slot}, nullptr, slot_mismatch_message));
                }
            }
            if (kept != bindings.end()) {
                bindings.erase(kept, bindings.end());
                ATELIER_TRY(keep_bindings(object, *entries));
            }
            return bindings;
        };
        const auto bound = [&](ObjectId object, const char* name, Result<void> applied,
                               auto clear) -> SceneResult<void> {
            if (applied)
                return {};
            if (const auto value = find_path(required_entry(records, object), Names{"properties", name}))
                ATELIER_TRY(load.keep(object, {"properties", name}, *value, world::DiagnosticKind::mismatch,
                                      applied.error().message + " The binding is empty and the saved value is kept."));
            else
                ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                        {"properties", name}, nullptr,
                                        applied.error().message + " The binding is empty."));
            return clear();
        };
        const auto body_type = runtime.native_type<world::PrimitiveComponent>();
        for (const auto& entity : entities) {
            if (const auto key = *local_key(*entity.at("id")); !anchors.contains(key)) {
                const auto object = required_entry(ids, key);
                ATELIER_TRY_BIND(entity_type, runtime.type_of(object));
                ATELIER_TRY_BIND(defaults, properties(entity, object, *entity_type));
                ATELIER_TRY(validate(object, *defaults));
            }
            const auto components = *entity.at("components")->array();
            for (auto part : components) {
                const auto part_id = *local_key(*part.at("id"));
                const auto object = required_entry(ids, part_id);
                ATELIER_TRY_BIND(actual, runtime.type_of(object));
                const auto type = *actual;
                ATELIER_TRY_BIND(defaults, properties(part, object, type));
                ATELIER_TRY_BIND(is_mesh, runtime.is_a(type, session.presentation_types_.static_mesh_component));
                if (*is_mesh) {
                    const auto key = std::string(*part.string({"id"}));
                    if (auto at = graph.meshes.find(key); at != graph.meshes.end()) {
                        const auto& copy = at->second;
                        ATELIER_TRY(presentation::set_mesh_binding(runtime, object, copy.state.mesh));
                        auto bindings = copy_bindings(copy.bindings);
                        ATELIER_TRY(presentation::set_material_bindings(runtime, object, bindings));
                        if (const auto entries = *part.find("materials"); !copy.state.mesh.resolved &&
                                                                          !copy.state.mesh.source.empty() && entries &&
                                                                          entries->kind() == json::Kind::array)
                            ATELIER_TRY(keep_bindings(object, *entries));
                    } else {
                        if (!project)
                            return std::unexpected(SceneFailure("Material and mesh restoration requires a Project."));
                        std::string source;
                        if (const auto mesh_at = *part.find("mesh"); !mesh_at)
                            ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                                    {"mesh"}, nullptr,
                                                    "The mesh source is missing; the component has no mesh."));
                        else if (mesh_at->kind() != json::Kind::string)
                            ATELIER_TRY(load.keep(
                                object, {"mesh"}, *mesh_at, world::DiagnosticKind::mismatch,
                                "Expected a mesh source key. The component has no mesh and the saved value is kept."));
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
                                    {
                                        ATELIER_TRY_BIND(value,
                                                         session.mesh_defaults(project->assets(), *loaded, &missing));
                                        defaults = std::move(*value);
                                    }
                                    for (const auto& absent : missing) {
                                        json::Builder saved;
                                        saved.set_validated(saved.root(), absent.key);
                                        const auto value = saved.root();
                                        ATELIER_TRY(load.report(world::Severity::warning, absent.kind, object, {"mesh"},
                                                                &value, absent.error.message, &absent.error));
                                    }
                                } else
                                    item = &session.remember_unavailable(project->assets(), assets::AssetKind::mesh,
                                                                         source, loaded.error());
                            }
                            if (item) {
                                json::Builder saved;
                                saved.set_validated(saved.root(), source);
                                const auto value = saved.root();
                                ATELIER_TRY(load.report(world::Severity::warning, item->kind, object, {"mesh"}, &value,
                                                        item->error.message, &item->error));
                            }
                        }
                        const bool unresolved = !source.empty() && !mesh;
                        ATELIER_TRY(presentation::set_mesh_binding(runtime, object,
                                                                   {source, std::move(mesh), std::move(defaults)}));
                        ATELIER_TRY_BIND(mesh_state, presentation::read_static_mesh(runtime, object));
                        const auto& geometry = mesh_state->mesh.resolved;
                        ATELIER_TRY_BIND(
                            bindings,
                            decode_material_bindings(
                                object, part, unresolved, false,
                                [&](const std::string& slot) {
                                    std::size_t matches{};
                                    if (geometry)
                                        for (std::size_t i = 0; i < geometry->materials().size(); ++i)
                                            matches += geometry::material_slot_name(*geometry, i) == slot;
                                    return matches == 1;
                                },
                                "The mesh no longer has one matching slot; the author binding is kept without being applied."));
                        ATELIER_TRY(presentation::set_material_bindings(runtime, object, *bindings));
                    }
                }
                ATELIER_TRY_BIND(is_generator,
                                 runtime.is_a(type, session.presentation_types_.procedural_generator_component));
                if (*is_generator) {
                    const auto key = std::string(*part.string({"id"}));
                    if (auto at = graph.generators.find(key); at != graph.generators.end()) {
                        const auto& copy = at->second;
                        ATELIER_TRY(presentation::set_procedural_binding(runtime, object, copy.binding));
                        if (copy.overrides.root().kind() == json::Kind::object && copy.overrides.root().size() > 0)
                            session.generator_overrides_[object] = copy.overrides;
                        auto bindings = copy_bindings(copy.bindings);
                        ATELIER_TRY(presentation::set_material_bindings(runtime, object, bindings));
                        if (const auto entries = *part.find("materials");
                            entries && entries->kind() == json::Kind::array && entries->size() > bindings.size())
                            ATELIER_TRY(keep_bindings(object, *entries));
                    } else {
                        if (!project)
                            return std::unexpected(
                                SceneFailure("Procedural generator restoration requires a Project."));
                        std::string source;
                        if (const auto graph_at = *part.find("graph"); !graph_at)
                            ATELIER_TRY(load.report(
                                world::Severity::warning, world::DiagnosticKind::mismatch, object, {"graph"}, nullptr,
                                "The procedural graph key is missing; the generator has no graph."));
                        else if (graph_at->kind() != json::Kind::string)
                            ATELIER_TRY(load.keep(
                                object, {"graph"}, *graph_at, world::DiagnosticKind::mismatch,
                                "Expected a procedural graph key. The generator has no graph and the saved value is kept."));
                        else
                            source = *graph_at->string();
                        if (!source.empty() && !session.has_graph(source)) {
                            const auto* item =
                                load.live ? nullptr : session.unavailable(assets::AssetKind::procedural_graph, source);
                            if (!item) {
                                if (auto loaded = project->assets().procedural_graph(source)) {
                                    Runtime validation;
                                    ATELIER_TRY(procedural::register_types(validation));
                                    if (auto parsed = load_graph(validation, loaded->text); !parsed) {
                                        const Error format_error{ErrorCode::invalid_storage, parsed.error().message};
                                        item = &session.remember_unavailable(project->assets(),
                                                                             assets::AssetKind::procedural_graph,
                                                                             source, format_error);
                                    }
                                } else {
                                    item = &session.remember_unavailable(
                                        project->assets(), assets::AssetKind::procedural_graph, source, loaded.error());
                                }
                            }
                            if (item) {
                                json::Builder saved;
                                saved.set_validated(saved.root(), source);
                                const auto value = saved.root();
                                ATELIER_TRY(load.report(world::Severity::warning, item->kind, object, {"graph"}, &value,
                                                        item->error.message, &item->error));
                            }
                        }
                        ATELIER_TRY(presentation::set_procedural_binding(runtime, object, {source, nullptr, {}}));
                        if (const auto ov_at = *part.find("overrides")) {
                            if (ov_at->kind() != json::Kind::object) {
                                ATELIER_TRY(load.keep(
                                    object, {"overrides"}, *ov_at, world::DiagnosticKind::mismatch,
                                    "Expected an object of generator parameter overrides; the saved value is kept."));
                            } else {
                                json::Builder clean_ov;
                                for (const auto& [ov_key, ov_val] : *ov_at->object_items()) {
                                    if (!valid_graph_identifier(ov_key)) {
                                        ATELIER_TRY(load.keep(
                                            object, {"overrides", std::string(ov_key)}, ov_val,
                                            world::DiagnosticKind::unknown_field,
                                            "Invalid generator parameter override identifier; the saved value is kept."));
                                        continue;
                                    }
                                    clean_ov.set_validated(clean_ov.root(), ov_key, ov_val);
                                }
                                if (clean_ov.root().size() > 0)
                                    session.generator_overrides_[object] = clean_ov.finish();
                            }
                        }
                        ATELIER_TRY_BIND(
                            bindings,
                            decode_material_bindings(
                                object, part, false, true, [](const std::string& slot) { return slot == "default"; },
                                "Procedural generators only expose the default surface slot; the author binding is kept without being applied."));
                        ATELIER_TRY(presentation::set_material_bindings(runtime, object, *bindings));
                    }
                }
                ATELIER_TRY_BIND(environment, runtime.is_a(type, session.presentation_types_.environment_component));
                if (*environment) {
                    if (auto at = graph.skies.find(std::string(*part.string({"id"}))); at != graph.skies.end())
                        ATELIER_TRY(presentation::set_sky_map_binding(runtime, object, at->second));
                    else {
                        std::string source;
                        if (const auto sky_at = *part.find("sky_map"); !sky_at)
                            ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::mismatch, object,
                                                    {"sky_map"}, nullptr,
                                                    "The sky map key is missing; the sky has no map."));
                        else if (sky_at->kind() != json::Kind::string)
                            ATELIER_TRY(
                                load.keep(object, {"sky_map"}, *sky_at, world::DiagnosticKind::mismatch,
                                          "Expected a sky map key. The sky has no map and the saved value is kept."));
                        else
                            source = *sky_at->string();
                        ATELIER_TRY_BIND(sky, resolve_sky(session, project, source, load, object));
                        ATELIER_TRY(presentation::set_sky_map_binding(runtime, object, {source, std::move(*sky)}));
                    }
                }
                ATELIER_TRY_BIND(ragdoll, runtime.is_a(type, session.physics_types_.ragdoll_component));
                if (*ragdoll) {
                    ATELIER_TRY_BIND(parts, physics::ragdoll_parts(runtime, object));
                    ATELIER_TRY(bound(object, "parts", physics::set_ragdoll_parts(runtime, object, *parts),
                                      [&] { return physics::set_ragdoll_parts(runtime, object, {}); }));
                }
                ATELIER_TRY_BIND(query, runtime.is_a(type, session.physics_types_.scene_query_component));
                if (*query) {
                    ATELIER_TRY_BIND(ignored, physics::query_ignored_entities(runtime, object));
                    ATELIER_TRY(bound(object, "ignored_entities",
                                      physics::set_query_ignored_entities(runtime, object, *ignored),
                                      [&] { return physics::set_query_ignored_entities(runtime, object, {}); }));
                }
                ATELIER_TRY(validate(object, *defaults));
                if (body_type) {
                    ATELIER_TRY_BIND(physical, runtime.is_a(type, *body_type));
                    if (!*physical)
                        continue;
                    ATELIER_TRY_BIND(settings, physics::read_settings(runtime, object));
                    std::string problem;
                    if (settings->enabled && settings->shape == physics::ShapeKind::triangle_mesh && project) {
                        const auto mesh = presentation::read_static_mesh(runtime, object);
                        if (!mesh)
                            problem = mesh.error().message;
                        else if (mesh->mesh.source.empty())
                            problem = "Triangle collision needs a persistent mesh source.";
                    }
                    if (!problem.empty()) {
                        ATELIER_TRY(invalid_record(object, Names{"body"}, *defaults, problem));
                        ATELIER_TRY(validate(object, *defaults));
                        {
                            ATELIER_TRY_BIND(value, physics::read_settings(runtime, object));
                            *settings = std::move(*value);
                        }
                    }
                    if (settings->enabled && settings->shape == physics::ShapeKind::triangle_mesh) {
                        ATELIER_TRY_BIND(mesh, presentation::read_static_mesh(runtime, object));
                        if (!mesh->mesh.resolved && !mesh->mesh.source.empty()) {
                            const auto* item = session.unavailable(assets::AssetKind::mesh, mesh->mesh.source);
                            ATELIER_TRY(load.report(
                                world::Severity::warning, item ? item->kind : world::DiagnosticKind::missing_resource,
                                object, {"properties", "body", "shape"}, nullptr,
                                "Triangle collision has no mesh to cook; Play cannot create this body until the mesh is restored."));
                        } else if (mesh->mesh.source.starts_with("project/"))
                            result.triangle_meshes.push_back(object);
                    }
                    if (const auto cached = graph.cooked_shapes.find(part_id);
                        cached != graph.cooked_shapes.end() && cached->second != settings->cooked_shape) {
                        settings->cooked_shape = cached->second;
                        ATELIER_TRY(physics::configure(runtime, object, std::move(*settings)));
                    }
                }
            }
        }
        // Retained references bind to document identities, which stay stable for
        // the life of the document and are re-encoded on save.
        std::unordered_set<std::uint64_t> held_keys;
        const auto rebind_retained = [&](json::Builder& entry, json::Value value,
                                         Retained& owner) -> SceneResult<void> {
            return each_reference(value, [&](json::Value node) -> SceneResult<void> {
                ObjectId target;
                if (*node.find("$ref")) {
                    ATELIER_TRY(node.members({"$ref"}));
                    ATELIER_TRY_BIND(parsed_key, local_key(*node.at("$ref")));
                    const auto key = *parsed_key;
                    // A live graph's outward references already name document identities.
                    const auto found = ids.find(key);
                    if (found != ids.end()) {
                        target = found->second;
                        if (material_ids.contains(key))
                            used_materials.insert(key);
                    } else if (load.live) {
                        const auto current = live_object(session, key);
                        if (!current || *current == ObjectId{})
                            return {};
                        target = *current;
                    } else
                        return std::unexpected(
                            SceneFailure("Reference target is outside this graph: " + std::to_string(key)));
                } else {
                    ATELIER_TRY(node.members({"$external"}));
                    ATELIER_TRY_BIND(key, node.string({"$external"}));
                    const auto found = graph.external.find(std::string(*key));
                    if (found == graph.external.end())
                        return std::unexpected(
                            SceneFailure("External object references cannot be loaded from a file."));
                    target = found->second.id();
                }
                ATELIER_TRY_BIND(id, identity(session, target));
                if (session.is_material(target) && held_keys.insert(*id).second) {
                    ATELIER_TRY_BIND(root, runtime.hold(target));
                    owner.reference_roots.emplace_back(*id, std::move(*root));
                }
                const auto reference = ref_key(*id);
                entry.replace(node, reference.root());
                return {};
            });
        };
        if (load.top.root().size()) {
            ATELIER_TRY(rebind_retained(load.top, load.top.root(), load.document_retained));
            load.document_retained.record = dump_retained(load.top.root());
        }
        for (auto& [object, entry] : load.pending) {
            held_keys.clear();
            Retained kept;
            if (const auto at = *entry.root().find("type"))
                kept.type = *at->string();
            for (const auto name : {"record", "slots"})
                if (auto at = *entry.root().find(name)) {
                    ATELIER_TRY(rebind_retained(entry, *at, kept));
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
                    ATELIER_TRY(load.report(
                        world::Severity::warning, world::DiagnosticKind::unknown_key, resource.id(), {}, nullptr,
                        "The material definition is not referenced; it is kept for the next save."));
                    load.document_retained.materials.push_back(resource);
                } else
                    ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::unknown_key, resource.id(),
                                            {}, nullptr,
                                            "The material definition is not referenced and is not imported."));
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
                target = required_entry(graph.parents, std::string(*graph.data.root().at("roots")->at(i)->string()));
            attachments.push_back({prepared.roots[i], target, world::append});
        }
        return attachments;
    }

    static SceneResult<void> prepare_cook(Session& session, Prepared& prepared,
                                          std::span<const world::EntityAttachment> attachments, Load& load) {
        if (prepared.triangle_meshes.empty())
            return {};
        std::unordered_map<ObjectId, math::Matrix4> worlds;
        if (!attachments.empty()) {
            ATELIER_TRY_BIND(matrices, session.world_.attachment_world_matrices(attachments, prepared.spaces));
            for (const auto& [object, matrix] : *matrices)
                worlds.emplace(object, matrix);
        }
        std::unordered_map<ObjectId, ObjectId> anchored;
        for (const auto& [entity, parts] : prepared.anchored)
            for (const auto& part : parts)
                anchored.emplace(part.component, entity);
        // Bodies sharing a mesh and world scale share one cooked shape for this load.
        std::map<std::pair<std::string, std::array<float, 3>>, Result<std::shared_ptr<const std::string>>> cooks;
        for (const auto object : prepared.triangle_meshes) {
            ATELIER_TRY_BIND(mesh, presentation::read_static_mesh(session.runtime_, object));
            const auto at = anchored.find(object);
            ObjectId owner;
            if (at != anchored.end())
                owner = at->second;
            else {
                ATELIER_TRY_BIND(state, world::inspect_component(session.runtime_, object));
                owner = state->owner;
            }
            const auto found = worlds.find(owner);
            ATELIER_TRY_BIND(matrix, found != worlds.end() ? Result<math::Matrix4>{found->second}
                                                           : world::world_matrix(session.runtime_, owner));
            ATELIER_TRY_BIND(trs, world::relative_transform(*matrix, world::transform_matrix({})));
            ATELIER_TRY_BIND(settings, physics::read_settings(session.runtime_, object));
            if (prepared.project) {
                const auto key = std::pair{mesh->mesh.source, std::array{trs->scale.x, trs->scale.y, trs->scale.z}};
                auto cached = cooks.find(key);
                if (cached == cooks.end())
                    cached = cooks.emplace(key, prepared.project->collision(mesh->mesh.source, trs->scale)).first;
                const auto& cooked = cached->second;
                if (!cooked) {
                    ATELIER_TRY(load.report(world::Severity::warning, world::DiagnosticKind::unreadable_resource,
                                            object, {"properties", "body", "shape"}, nullptr, cooked.error().message,
                                            &cooked.error()));
                    continue;
                }
                settings->cooked_shape = *cooked;
            } else if (settings->cooked_shape)
                ATELIER_TRY(physics::validate_cooked_triangle_mesh(*settings->cooked_shape, mesh->mesh.resolved,
                                                                   trs->scale, mesh->mesh.source));
            ATELIER_TRY(physics::configure(session.runtime_, object, std::move(*settings)));
        }
        return {};
    }

    static SceneResult<world::Commit> publish(Session& session, Prepared& prepared, const Graph& graph, ObjectId parent,
                                              bool duplicate, Load& load) {
        EntityChange history;
        history.added = true;
        history.removals.reserve(prepared.roots.size());
        for (auto root : prepared.roots) {
            world::EntityRemoval removal;
            ATELIER_TRY_BIND(held, session.runtime_.hold(root));
            removal.root = std::move(*held);
            history.removals.push_back(std::move(removal));
        }
        auto plans = attachments(prepared, graph, parent, duplicate);
        ATELIER_TRY(prepare_cook(session, prepared, plans, load));
        auto selection = prepared.roots;
        session.history_.reserve(session.history_.size() + 1);
        session.retained_.reserve(session.retained_.size() + load.retained.size());
        if (!duplicate)
            session.load_issues_.reserve(session.load_issues_.size() + load.issues.size());
        for (const auto& [object, label] : prepared.labels)
            session.names_.emplace(object, label);
        ATELIER_TRY_BIND(result, session.world_.attach_batch(plans, prepared.spaces));
        if (result->changed) {
            // Selection and history storage were prepared before World publication.
            // No admission, allocation or callback can fail between these commits.
            prepared.published = true;
            session.selected_entities_.swap(selection);
            session.selection_ = {prepared.roots.back(), prepared.roots.back()};
            ++session.revision_;
            session.remember(std::move(history));
            session.retained_.merge(load.retained);
            if (!duplicate)
                std::ranges::move(load.issues, std::back_inserter(session.load_issues_));
        }
        return result;
    }

    // Play publication keeps document identities and records no history.
    static SceneResult<world::Commit> publish_live(Session& session, Prepared& prepared, ObjectId parent,
                                                   std::size_t index, Load& load) {
        std::vector<world::EntityAttachment> plans;
        for (const auto root : prepared.roots)
            plans.push_back({root, parent, index});
        ATELIER_TRY(prepare_cook(session, prepared, plans, load));
        for (const auto& [object, label] : prepared.labels)
            session.names_.insert_or_assign(object, label);
        // apply validates one add or one attach before publishing this graph.
        ATELIER_TRY_BIND(result, plans.empty()
                                     ? session.world_.attach_components(prepared.anchored.front().first,
                                                                        prepared.anchored.front().second, index)
                                     : session.world_.attach_batch(plans, prepared.spaces));
        prepared.published = true;
        session.retained_.merge(load.retained);
        for (const auto& object : prepared.objects)
            session.live_objects_.insert_or_assign(required_entry(session.local_ids_, object.id()), object.id());
        for (const auto& resource : prepared.resources)
            session.live_objects_.insert_or_assign(required_entry(session.local_ids_, resource.id()), resource.id());
        return result;
    }

    static SceneResult<void> relocated(Result<void> value) {
        if (!value && value.error().code == ErrorCode::unsupported_operation)
            return std::unexpected(SceneFailure(std::move(value.error()), SceneFailure::Disposition::unsupported_pose));
        return value;
    }

    // A Play index is the final position among the destination's children, or its roots for a null parent.
    // Game spawns and destructions legitimately change those lists, so a stale index clamps to the end.
    // World attaches a new root before a member position, so an added root's ordinal is translated.
    static SceneResult<std::size_t> live_index(Session& session, json::Value value, ObjectId parent,
                                               std::optional<ObjectId> moved = {}) {
        if (value.is_null())
            return world::append;
        ATELIER_TRY_BIND(parsed, value.u64());
        const auto wanted = *parsed;
        auto& runtime = session.runtime_;
        if (parent != ObjectId{}) {
            ATELIER_TRY_BIND(siblings, world::children(runtime, parent));
            return std::min<std::size_t>(
                wanted, siblings->size() -
                            std::ranges::count_if(*siblings, [&](const auto& item) { return item.id() == moved; }));
        }
        ATELIER_TRY_BIND(members, session.world_.members());
        std::size_t ordinal{};
        for (std::size_t i = 0; i < members->size(); ++i) {
            const auto id = (*members)[i].id();
            if (id == moved)
                continue;
            ATELIER_TRY_BIND(actual_parent, world::parent_entity(runtime, id));
            if (*actual_parent != ObjectId{})
                continue;
            if (ordinal == wanted)
                return moved ? wanted : i;
            ++ordinal;
        }
        return moved ? ordinal : world::append;
    }

    struct PlayCommand {
        Session& session;

        Project& project;

        std::vector<world::Diagnostic>& out;

        std::string subject;

        ObjectId object{};

        void note(world::Severity severity, world::DiagnosticKind kind, const std::string& detail,
                  const Error* cause = nullptr) {
            auto& runtime = session.runtime_;
            world::Diagnostic item;
            item.serial = out.size() + 1;
            item.severity = severity;
            item.kind = kind;
            item.phase = world::DiagnosticPhase::command;
            item.object = object;
            if (const auto type = runtime.type_of(object)) {
                if (const auto info = runtime.describe(*type))
                    item.type = std::string_view{(*info)->name};
                if (runtime.is_a(*type, session.types_.entity).value_or(false))
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
        }

        SceneResult<void> apply(json::Value op) {
            auto& runtime = session.runtime_;
            auto& world = session.world_;
            const auto target = [&](json::Value id) -> SceneResult<ObjectId> {
                ATELIER_TRY_BIND(key, local_key(id));
                subject += " #" + std::to_string(*key);
                return live_target(session, *key, &object);
            };
            const auto commit = [&](Result<world::Commit> value) -> Result<void> {
                ATELIER_TRY(value);
                if (value->notification_error)
                    note(world::Severity::warning, world::DiagnosticKind::observer_failed,
                         value->notification_error->message, &*value->notification_error);
                return {};
            };
            Load load;
            load.live = true;

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
                        ATELIER_TRY_BIND(parent_key, local_key(parent_value));
                        ATELIER_TRY_BIND(found, live_target(session, *parent_key));
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
                    ATELIER_TRY(project.refresh());
                }
                if (add ? prepared->roots.size() != 1 || !prepared->anchored.empty()
                        : !prepared->roots.empty() || prepared->anchored.size() != 1 ||
                              prepared->anchored.front().first != entity)
                    return std::unexpected(SceneFailure(add ? "An add carries exactly one root Entity."
                                                            : "An attach carries only new components of its Entity."));
                if (add)
                    object = prepared->roots.front();
                const auto wanted = *op.at("index");
                std::size_t placed = world::append;
                if (add) {
                    ATELIER_TRY_BIND(position, live_index(session, wanted, parent));
                    placed = *position;
                } else if (!wanted.is_null()) {
                    ATELIER_TRY_BIND(position, wanted.u64());
                    ATELIER_TRY_BIND(parts, world::components(runtime, entity));
                    placed = std::min<std::size_t>(*position, parts->size());
                }
                ATELIER_TRY_BIND(publication, publish_live(session, *prepared, parent, placed, load));
                ATELIER_TRY(commit(std::move(*publication)));
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
                ATELIER_TRY_BIND(type, runtime.type_of(removed));
                ATELIER_TRY_BIND(entity, runtime.is_a(*type, session.types_.entity));
                if (*entity)
                    ATELIER_TRY(commit(world::destroy_entity(runtime, removed)));
                else {
                    ATELIER_TRY_BIND(component, runtime.is_a(*type, session.types_.component));
                    if (!*component)
                        return std::unexpected(SceneFailure("Only an Entity or Component can be removed."));
                    ATELIER_TRY(commit(world::destroy_component(runtime, removed)));
                }
            } else if (kind == "set") {
                ATELIER_TRY(op.allowed_members({"op", "id", "name", "enabled", "space", "properties", "materials",
                                                "sky_map", "graph", "overrides"}));
                ATELIER_TRY_BIND(id, op.at("id"));
                ATELIER_TRY_BIND(edited_value, target(*id));
                const auto edited = *edited_value;
                ATELIER_TRY_BIND(type, runtime.type_of(edited));
                ATELIER_TRY_BIND(entity, runtime.is_a(*type, session.types_.entity));
                load.policy = LoadPolicy::strict;
                if (const auto at = *op.find("name")) {
                    ATELIER_TRY_BIND(text, at->string());
                    auto label = platform::utf8_to_utf16_validated(*text);
                    if (*entity || !label.empty())
                        ATELIER_TRY(validate_name(label));
                    if (label.empty())
                        session.names_.erase(edited);
                    else
                        session.names_.insert_or_assign(edited, std::move(label));
                }
                if (const auto at = *op.find("enabled")) {
                    ATELIER_TRY_BIND(enabled, at->boolean());
                    ATELIER_TRY(commit(world.set_enabled(edited, *enabled)));
                }
                if (const auto at = *op.find("properties")) {
                    if (at->kind() != json::Kind::object)
                        return std::unexpected(SceneFailure("Expected a property record."));
                    const SceneResolve resolve = [&](json::Value value, TypeId field_type,
                                                     ValueForm form) -> SceneResult<std::optional<ObjectReference>> {
                        const auto null = [&] {
                            return runtime.null_reference(
                                field_type, form == ValueForm::weak_reference ? form : ValueForm::strong_reference);
                        };
                        if (value.is_null()) {
                            ATELIER_TRY_BIND(reference, null());
                            return std::optional{*reference};
                        }
                        if (value.kind() != json::Kind::object || !*value.find("$ref"))
                            return std::nullopt;
                        ATELIER_TRY(scene_result(value.members({"$ref"})));
                        ATELIER_TRY_BIND(key, scene_result(local_key(*value.at("$ref"))));
                        ATELIER_TRY_BIND(live, live_reference(session, *key, load));
                        if (!*live) {
                            ATELIER_TRY_BIND(reference, null());
                            return std::optional{*reference};
                        }
                        auto held = runtime.hold(**live, field_type);
                        if (!held)
                            return std::nullopt;
                        if (form == ValueForm::weak_reference) {
                            ATELIER_TRY_BIND(reference, runtime.observe(held->reference()));
                            return std::optional{*reference};
                        }
                        return std::optional{held->reference()};
                    };
                    // Scalar leaves are one group write: a type's rules can span them, as when a probe volume moves.
                    std::vector<world::FieldPath> paths;
                    std::vector<Scalar> values;
                    const PropertyTarget target{*type, edited};
                    ATELIER_TRY_BIND(fields, business_fields(runtime, session.types_, target.type));
                    ATELIER_TRY(read_properties(
                        runtime, target, *at, load, resolve, *fields, {},
                        [&](const detail::ScalarField& field, const PropertyLeaf& leaf) -> Result<void> {
                            const auto& path = field.path;
                            if (const auto* scalar = std::get_if<Scalar>(&leaf)) {
                                paths.emplace_back(path.begin(), path.end());
                                values.push_back(*scalar);
                                return {};
                            }
                            if (const auto* array = std::get_if<ArrayLeaf>(&leaf))
                                return world.write(edited, path, array->input());
                            ATELIER_TRY_BIND(string_type, runtime.native_type<String>());
                            return world.write(edited, path, ValueInput{*string_type, &std::get<String>(leaf)});
                        }));
                    if (!paths.empty())
                        ATELIER_TRY(world.write(edited, std::span<const world::FieldPath>(paths),
                                                std::span<const Scalar>(values)));
                }
                if (const auto at = *op.find("materials")) {
                    ATELIER_TRY_BIND(entries, at->array());
                    if (entries->size() > 1024)
                        return std::unexpected(SceneFailure("Expected a material binding table."));
                    std::vector<presentation::MaterialBinding> bindings;
                    for (auto entry : *entries) {
                        ATELIER_TRY(entry.members({"slot", "resource", "overridden"}));
                        ATELIER_TRY_BIND(parsed_key, local_key(*entry.at("resource")));
                        const auto key = *parsed_key;
                        const auto live = live_object(session, key);
                        // Materials are document resources; the editor sends a missing one first.
                        if (!live || *live == ObjectId{} || !session.is_material(*live))
                            return std::unexpected(
                                SceneFailure("Material #" + std::to_string(key) + " does not exist in Play."));
                        ATELIER_TRY_BIND(slot, entry.string({"slot"}));
                        ATELIER_TRY_BIND(overridden, entry.boolean({"overridden"}));
                        ATELIER_TRY_BIND(held, runtime.hold(*live));
                        bindings.push_back({std::string(*slot), std::move(*held), *overridden});
                    }
                    ATELIER_TRY(presentation::set_material_bindings(runtime, edited, bindings));
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
                            ATELIER_TRY(project.refresh());
                            loaded = project.assets().sky_map(source);
                        }
                        if (loaded) {
                            map = loaded->map;
                            session.unavailable_.erase(std::pair{assets::AssetKind::sky, source});
                        } else
                            missing = &session.remember_unavailable(project.assets(), assets::AssetKind::sky, source,
                                                                    loaded.error());
                    }
                    ATELIER_TRY(presentation::set_sky_map_binding(runtime, edited, {source, std::move(map)}));
                    // The document keeps the same key unresolved; Play reports it without diverging.
                    if (missing)
                        note(world::Severity::warning, missing->kind, missing->error.message, &missing->error);
                }
                if (*op.find("graph") || *op.find("overrides")) {
                    ATELIER_TRY_BIND(generator,
                                     runtime.is_a(*type, session.presentation_types_.procedural_generator_component));
                    if (!*generator)
                        return std::unexpected(
                            SceneFailure(*op.find("graph") ? "Only a ProceduralGeneratorComponent has a graph."
                                                           : "Only a ProceduralGeneratorComponent has overrides."));
                }
                if (const auto at = *op.find("graph")) {
                    ATELIER_TRY_BIND(text, at->string());
                    const auto source = std::string(*text);
                    const MissingResource* missing{};
                    if (!source.empty() && !session.has_graph(source)) {
                        auto loaded = project.assets().procedural_graph(source);
                        if (!loaded && !project.entry(source)) {
                            ATELIER_TRY(project.refresh());
                            loaded = project.assets().procedural_graph(source);
                        }
                        if (loaded)
                            session.unavailable_.erase(std::pair{assets::AssetKind::procedural_graph, source});
                        else
                            missing = &session.remember_unavailable(
                                project.assets(), assets::AssetKind::procedural_graph, source, loaded.error());
                    }
                    ATELIER_TRY(presentation::set_procedural_binding(runtime, edited, {source, nullptr, {}}));
                    if (missing)
                        note(world::Severity::warning, missing->kind, missing->error.message, &missing->error);
                }
                if (const auto at = *op.find("overrides")) {
                    if (at->is_null()) {
                        session.generator_overrides_.erase(edited);
                        ++session.revision_;
                    } else if (at->kind() == json::Kind::object) {
                        ATELIER_TRY_BIND(ov_items, at->object_items());
                        for (const auto& [ov_key, _] : *ov_items) {
                            if (!valid_graph_identifier(ov_key))
                                return std::unexpected(
                                    SceneFailure("Invalid generator override parameter identifier."));
                        }
                        if (at->size() > 0)
                            session.generator_overrides_[edited] = json::Document(*at);
                        else
                            session.generator_overrides_.erase(edited);
                        ++session.revision_;
                    } else {
                        return std::unexpected(SceneFailure("Expected a generator overrides object."));
                    }
                }
                if (const auto at = *op.find("space")) {
                    if (!*entity)
                        return std::unexpected(SceneFailure("Only an Entity has a space."));
                    if (at->is_null())
                        ATELIER_TRY(world.remove_transform(edited));
                    else {
                        ATELIER_TRY(at->allowed_members({"position", "rotation", "scale", "origin"}));
                        ATELIER_TRY_BIND(spatial, world::has_transform(runtime, edited));
                        world::Trs trs;
                        if (*spatial) {
                            ATELIER_TRY_BIND(current, world::read_transform(runtime, edited));
                            trs = *current;
                        }
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
                        ATELIER_TRY_BIND(canonical, world::canonical_transform(trs));
                        trs = std::move(*canonical);
                        if (const auto origin = *at->find("origin")) {
                            std::optional<ObjectId> reference;
                            if (!origin->is_null()) {
                                ATELIER_TRY_BIND(key, local_key(*origin, true));
                                if (*key) {
                                    ATELIER_TRY_BIND(live, live_reference(session, *key, load));
                                    reference = *live;
                                } else
                                    reference = ObjectId{};
                            }
                            if (!*spatial)
                                ATELIER_TRY(relocated(world.set_transform(edited, trs)));
                            ATELIER_TRY(
                                commit(world.set_space_reference(edited, reference, world::ReparentRule::keep_local)));
                        }
                        ATELIER_TRY(relocated(world.set_transform(edited, trs)));
                    }
                }
            } else if (kind == "move") {
                ATELIER_TRY(op.members({"op", "id", "parent", "index", "space"}));
                ATELIER_TRY_BIND(moved, target(*op.at("id")));
                ObjectId parent;
                const auto parent_value = *op.at("parent");
                if (!parent_value.is_null()) {
                    ATELIER_TRY_BIND(parent_key, local_key(parent_value));
                    ATELIER_TRY_BIND(found, live_target(session, *parent_key));
                    parent = *found;
                }
                ATELIER_TRY_BIND(position, live_index(session, *op.at("index"), parent, *moved));
                ATELIER_TRY(commit(world.reparent(*moved, parent, world::ReparentRule::keep_world, *position)));
                if (const auto space = *op.at("space"); !space.is_null()) {
                    ATELIER_TRY(space.allowed_members({"position", "rotation", "scale", "origin"}));
                    ATELIER_TRY_BIND(trs, json_trs(space));
                    ATELIER_TRY(relocated(world.set_transform(*moved, *trs)));
                }
            } else if (kind == "materials") {
                ATELIER_TRY(op.members({"op", "materials"}));
                ATELIER_TRY_BIND(definitions, op.at("materials")->array());
                if (definitions->size() > maximum_entities * maximum_components)
                    return std::unexpected(SceneFailure("Expected a material definition list."));
                std::unordered_map<std::uint64_t, json::Value> records;
                for (auto definition : *definitions) {
                    ATELIER_TRY(definition.allowed_members({"id", "source", "parent", "entries"}));
                    ATELIER_TRY_BIND(id, definition.at("id"));
                    ATELIER_TRY_BIND(key, local_key(*id));
                    if (!records.emplace(*key, definition).second)
                        return std::unexpected(SceneFailure("Duplicate Live material identity."));
                }
                std::unordered_set<std::uint64_t> visiting;
                std::unordered_set<std::uint64_t> complete;
                std::function<SceneResult<ObjectRoot>(std::uint64_t)> restore;
                restore = [&](std::uint64_t key) -> SceneResult<ObjectRoot> {
                    const auto live = live_object(session, key);
                    if (live && *live != ObjectId{} && !session.is_material(*live))
                        return std::unexpected(
                            SceneFailure("Document identity #" + std::to_string(key) + " is not a material in Play."));
                    const auto record = records.find(key);
                    if (record == records.end() || complete.contains(key)) {
                        if (!live || *live == ObjectId{})
                            return std::unexpected(SceneFailure("Unknown Live material parent."));
                        return runtime.hold(*live);
                    }
                    if (!visiting.insert(key).second)
                        return std::unexpected(SceneFailure("Live material parent cycle."));
                    // As in a tolerant load: an unavailable source, parent or texture stands in with a warning.
                    std::vector<MissingResource> missing;
                    ATELIER_TRY_BIND(restored, restore_material(session, &project, record->second, restore, &missing,
                                                                live.value_or(ObjectId{})));
                    for (const auto& item : missing)
                        note(world::Severity::warning, item.kind, item.error.message, &item.error);
                    if (!live || *live == ObjectId{}) {
                        if (!session.local_ids_.emplace(restored->id(), key).second)
                            return std::unexpected(
                                SceneFailure("One material asset has multiple document identities."));
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
                    return std::unexpected(SceneFailure("Invalid Play texture dimensions."));
                const auto bytes = width * height * 4;
                ATELIER_TRY_BIND(file, op.string({"file"}));
                const auto rgba = assets::read_file(platform::utf8_to_utf16_validated(*file), bytes);
                if (!rgba) {
                    note(world::Severity::error, world::DiagnosticKind::command_rejected, rgba.error().message,
                         &rgba.error());
                    return {};
                }
                if (rgba->size() != bytes)
                    return std::unexpected(SceneFailure("Play texture RGBA byte count does not match its dimensions."));
                auto image =
                    material::Image::create(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                            std::vector<std::uint8_t>(rgba->begin(), rgba->end()));
                if (!image) {
                    note(world::Severity::error, world::DiagnosticKind::command_rejected, image.error().message);
                    return {};
                }
                ATELIER_TRY(session.publish_texture(key, std::move(*image)));
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
                    return std::unexpected(SceneFailure("Play mesh content does not match its asset key."));
                ATELIER_TRY(project.refresh());
                ATELIER_TRY(session.publish_mesh(project.assets(), *mesh));
            } else if (kind == "resource") {
                ATELIER_TRY(op.allowed_members({"op", "key", "material", "source"}));
                ATELIER_TRY_BIND(key_value, op.string({"key"}));
                const auto key = std::string(*key_value);
                subject += " " + key;
                if (key.empty())
                    return std::unexpected(SceneFailure("Live asset publication requires an asset key."));
                ATELIER_TRY_BIND(material, op.at("material"));
                ATELIER_TRY_BIND(asset, assets::material_format::decode(*material));
                asset->key = key;
                std::optional<source::SourceFile> exact_source;
                if (const auto input = *op.find("source")) {
                    ATELIER_TRY(input->members({"path", "text"}));
                    ATELIER_TRY_BIND(text, input->string({"text"}));
                    if (text->size() > (1u << 20))
                        return std::unexpected(SceneFailure("Invalid Live Material Source text."));
                    ATELIER_TRY_BIND(path, input->string({"path"}));
                    exact_source =
                        source::SourceFile{std::string(*path), source::content_hash(*text), std::string(*text)};
                }
                if (std::holds_alternative<assets::MaterialSourceAsset>(asset->author) && !exact_source)
                    return std::unexpected(SceneFailure("Live Material Source publication requires its exact text."));
                // Refresh only the catalogue for newly created dependencies;
                // published pixels and Source text above remain authoritative.
                ATELIER_TRY(project.refresh());
                std::vector<MissingResource> missing;
                ATELIER_TRY(session.publish_material(project.assets(), *asset, std::move(exact_source), &missing));
                for (const auto& item : missing)
                    note(world::Severity::warning, item.kind, item.error.message, &item.error);
            } else
                return std::unexpected(SceneFailure("Unknown change: " + std::string(kind)));
            for (const auto key : load.dropped)
                note(world::Severity::warning, world::DiagnosticKind::reference_dropped,
                     "#" + std::to_string(key) + " was destroyed by the game; the reference is null.");
            return {};
        }

        void reject(const SceneFailure& failure) {
            const auto* cause = std::get_if<Error>(&failure.detail);
            const auto& message = cause ? cause->message : std::get<std::string>(failure.detail);
            if (failure.disposition == SceneFailure::Disposition::destroyed)
                note(world::Severity::warning, world::DiagnosticKind::reference_dropped,
                     message + " The change is skipped.");
            else
                note(failure.disposition == SceneFailure::Disposition::unsupported_pose ? world::Severity::warning
                                                                                        : world::Severity::error,
                     world::DiagnosticKind::command_rejected, message, cause);
        }
    };
};

Result<std::string> Session::serialize() const {
    if (editing())
        return std::unexpected(Error{ErrorCode::invalid_storage, "Finish the active edit before saving."});
    std::vector<ObjectId> roots;
    ATELIER_TRY_BIND(members, world_.members());
    for (const auto& member : *members) {
        ATELIER_TRY_BIND(parent, world::parent_entity(world_.runtime(), member.id()));
        if (*parent == ObjectId{})
            roots.push_back(member.id());
    }
    ATELIER_TRY_BIND(graph, GraphCodec::capture(*this, roots, true, true));
    const auto text = json::write(graph->data.root(), {true, true});
    if (text.size() > assets::json_format::maximum_bytes)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Scene exceeds the 16 MiB file limit."});
    return std::string(text.data(), text.size());
}

Result<std::string> Session::serialize_subtree(ObjectId root) const {
    if (editing())
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Finish the active edit before saving a composition."});
    ATELIER_TRY_BIND(graph, GraphCodec::capture(*this, {&root, 1}, true));
    const auto text = json::write(graph->data.root(), {true, true});
    if (text.size() > assets::json_format::maximum_bytes)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Composition exceeds the 16 MiB file limit."});
    return std::string(text.data(), text.size());
}

Result<std::string> Session::serialize_live_subtree(ObjectId root) const {
    ATELIER_TRY_BIND(graph, GraphCodec::capture(*this, {&root, 1}, true, false, true));
    const auto text = json::write(graph->data.root());
    if (text.size() > assets::json_format::maximum_bytes)
        return std::unexpected(Error{ErrorCode::invalid_storage, "The Entity exceeds the 16 MiB change limit."});
    return std::string(text.data(), text.size());
}

Result<std::string> Session::serialize_live_components(ObjectId entity, std::span<const ObjectId> components) const {
    ATELIER_TRY_BIND(graph, GraphCodec::live_components(*this, entity, components));
    const auto text = json::write(graph->root());
    if (text.size() > assets::json_format::maximum_bytes)
        return std::unexpected(Error{ErrorCode::invalid_storage, "The Components exceed the 16 MiB change limit."});
    return std::string(text.data(), text.size());
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
    const auto push = [&](json::Value op) { return C::live_push(*this, {}, op); };
    const auto set = [&](ObjectId object, const char* name, const auto& value) -> Result<void> {
        json::Builder op;
        op.set_validated(op.root(), "op", "set");
        ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, object)));
        op.set_validated(op.root(), "id", *id);
        ATELIER_TRY(scene_result(op.set(op.root(), name, value)));
        return push(op.root());
    };
    // A commit also sends every leaf its previews sent, including leaves that returned to their start.
    const auto leaves = [&](ObjectId entity, const world::Trs& before, const world::Trs& after) -> Result<void> {
        auto mask = C::leaf_mask(before, after);
        if (const auto at = live_leaves_.find(entity); at != live_leaves_.end()) {
            mask |= at->second;
            live_leaves_.erase(at);
        }
        if (mask) {
            ATELIER_TRY_BIND(transform, world::read_transform(runtime_, entity));
            ATELIER_TRY_BIND(space, C::space_leaves(*transform, mask));
            ATELIER_TRY(set(entity, "space", space->root()));
        }
        return {};
    };
    auto result = std::visit(
        [&](const auto& item) -> Result<void> {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, EntityChange>) {
                const auto camera = [&]() -> Result<void> {
                    if (item.cleared_camera) {
                        const auto& edit = *item.cleared_camera;
                        ATELIER_TRY_BIND(op, C::live_properties(*this, edit.root.id(), {&edit.path, 1}));
                        ATELIER_TRY(push(op->root()));
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
                        ATELIER_TRY(push(op.root()));
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
                    ATELIER_TRY_BIND(parent, world::parent_entity(runtime_, root));
                    ATELIER_TRY_BIND(index, C::live_position(*this, root, *parent));
                    ATELIER_TRY_BIND(captured, C::capture(*this, {&root, 1}, true, false, true));
                    json::Builder graph(captured->data);
                    const auto entities = *graph.root().at("entities")->array();
                    for (auto entity : entities)
                        added_entities.emplace(*entity.string({"id"}), adds.size());
                    // This transaction restores its camera reference after all roots.
                    // An earlier Environment add must not refer to the still-missing target.
                    if (item.cleared_camera) {
                        ATELIER_TRY_BIND(component, scene_result(C::live_id(*this, item.cleared_camera->root.id())));
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
                    std::uint64_t parent_key{};
                    if (*parent == ObjectId{}) {
                        ATELIER_TRY(scene_result(op.set_null(op.root(), "parent")));
                    } else {
                        ATELIER_TRY_BIND(id, scene_result(C::identity(*this, *parent)));
                        parent_key = *id;
                        op.set_validated(op.root(), "parent", std::to_string(parent_key));
                    }
                    ATELIER_TRY(scene_result(op.set(op.root(), "index", std::uint64_t(*index))));
                    op.set_validated(op.root(), "graph", graph.root());
                    adds.push_back({parent_key, *index, std::move(op)});
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
                                            const auto props = deferred.object_validated(deferred.root(), "properties");
                                            deferred.set_validated(props, "main_camera", *reference);
                                            ATELIER_TRY(scene_result(op.set_null(properties, "main_camera")));
                                        }
                                }
                        }
                    }
                }
                std::ranges::sort(adds, {}, [](const Add& add) { return std::pair{add.parent, add.index}; });
                for (const auto& add : adds)
                    ATELIER_TRY(push(add.op.root()));
                if (imported_camera)
                    ATELIER_TRY(push(imported_camera->root()));
                ATELIER_TRY(camera());
            } else if constexpr (std::is_same_v<T, ComponentChange>) {
                const auto owner = item.removal.owner;
                const auto component = item.removal.id();
                if (item.added == forward) {
                    if (item.created_transform) {
                        ATELIER_TRY_BIND(space, C::live_space(*this, owner));
                        ATELIER_TRY(set(owner, "space", space->root()));
                    }
                    std::size_t index{};
                    ATELIER_TRY_BIND(parts, world::components(runtime_, owner));
                    for (const auto& part : *parts) {
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
                    ATELIER_TRY(push(op.root()));
                } else {
                    json::Builder op;
                    op.set_validated(op.root(), "op", "remove");
                    ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, component)));
                    op.set_validated(op.root(), "id", *id);
                    ATELIER_TRY(push(op.root()));
                    if (item.created_transform) {
                        const json::Builder null(json::Kind::null);
                        ATELIER_TRY(set(owner, "space", null.root()));
                    }
                }
            } else if constexpr (std::is_same_v<T, FieldChange>) {
                if (const auto* after = std::get_if<world::Trs>(&item.after))
                    ATELIER_TRY(leaves(item.root.id(), std::get<world::Trs>(item.before), *after));
                else {
                    ATELIER_TRY_BIND(op, C::live_properties(*this, item.root.id(), {&item.path, 1}));
                    ATELIER_TRY(push(op->root()));
                }
            } else if constexpr (std::is_same_v<T, ReferenceFieldChange>) {
                ATELIER_TRY_BIND(op, C::live_properties(*this, item.root.id(), {&item.path, 1}));
                ATELIER_TRY(push(op->root()));
            } else if constexpr (std::is_same_v<T, FieldGroupChange>) {
                ATELIER_TRY_BIND(op, C::live_properties(*this, item.root.id(), item.paths));
                ATELIER_TRY(push(op->root()));
            } else if constexpr (std::is_same_v<T, TransformChange>) {
                for (const auto& entry : item.entries)
                    ATELIER_TRY(leaves(entry.root.id(), entry.before, entry.after));
            } else if constexpr (std::is_same_v<T, NameChange>) {
                auto text = platform::utf16_to_utf8(name(item.root.id()));
                if (!text)
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
                ATELIER_TRY(set(item.root.id(), "name", *text));
            } else if constexpr (std::is_same_v<T, EnabledChange>) {
                ATELIER_TRY(set(item.root.id(), "enabled", forward ? item.after : item.before));
            } else if constexpr (std::is_same_v<T, BodyChange>) {
                ATELIER_TRY_BIND(op, C::live_body(*this, item.root.id()));
                ATELIER_TRY(push(op->root()));
            } else if constexpr (std::is_same_v<T, TransformPresenceChange>) {
                ATELIER_TRY_BIND(space, C::live_space(*this, item.root.id()));
                ATELIER_TRY(set(item.root.id(), "space", space->root()));
            } else if constexpr (std::is_same_v<T, SpaceReferenceChange>) {
                const auto entity = item.root.id();
                json::Builder space;
                if (item.spatial) {
                    ATELIER_TRY_BIND(transform, world::read_transform(runtime_, entity));
                    ATELIER_TRY_BIND(value, C::space_leaves(*transform, C::leaf_mask(item.before, item.after)));
                    space.replace(space.root(), value->root());
                }
                ATELIER_TRY_BIND(origin, C::live_origin(*this, entity));
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
                        ATELIER_TRY_BIND(space, C::space_leaves(forward ? transform->after : transform->before, 7));
                        op.set_validated(op.root(), "space", space->root());
                    } else
                        ATELIER_TRY(scene_result(op.set_null(op.root(), "space")));
                    ATELIER_TRY(push(op.root()));
                    for (const auto& entry : move.transforms)
                        if (entry.root.id() != root) {
                            ATELIER_TRY_BIND(space, C::space_leaves(forward ? entry.after : entry.before,
                                                                    C::leaf_mask(entry.before, entry.after)));
                            ATELIER_TRY(set(entry.root.id(), "space", space->root()));
                        }
                }
            } else if constexpr (std::is_same_v<T, MaterialBindingChange>) {
                // Play resolves a binding only to a material it already has, so definitions go first.
                const auto component = item.root.id();
                ATELIER_TRY_BIND(bindings, presentation::read_material_bindings(runtime_, component));
                json::Builder op;
                op.set_validated(op.root(), "op", "materials");
                const auto definitions = *op.array(op.root(), "materials");
                json::Builder table(json::Kind::array);
                std::unordered_set<ObjectId> defined;
                for (const auto& binding : *bindings) {
                    const auto resource = binding.resource.id();
                    ATELIER_TRY(C::material_definitions(*this, resource, op, definitions, defined));
                    const auto entry = table.append_object(table.root());
                    ATELIER_TRY(scene_result(table.set(entry, "slot", binding.slot)));
                    ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, resource)));
                    table.set_validated(entry, "resource", *id);
                    ATELIER_TRY(scene_result(table.set(entry, "overridden", binding.overridden)));
                }
                if (definitions.size())
                    ATELIER_TRY(push(op.root()));
                ATELIER_TRY(set(component, "materials", table.root()));
            } else if constexpr (std::is_same_v<T, MaterialValueChange>)
                forward_material(item.root.id());
            else if constexpr (std::is_same_v<T, SkyMapChange>) {
                ATELIER_TRY_BIND(map, presentation::read_sky_map(runtime_, item.root.id()));
                ATELIER_TRY(set(item.root.id(), "sky_map", map->source));
            } else if constexpr (std::is_same_v<T, ProceduralGraphChange>) {
                json::Builder op;
                op.set_validated(op.root(), "op", "set");
                ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, item.root.id())));
                op.set_validated(op.root(), "id", *id);
                if (item.before != item.after)
                    ATELIER_TRY(scene_result(op.set(op.root(), "graph", forward ? item.after : item.before)));
                if (item.before_overrides != item.after_overrides || item.before == item.after) {
                    const auto& ov = forward ? item.after_overrides : item.before_overrides;
                    op.set_validated(op.root(), "overrides", ov.root());
                }
                ATELIER_TRY(push(op.root()));
            } else
                static_assert(std::is_same_v<T, RetainedChange>,
                              "Every change kind is forwarded to Play or explicitly not.");
            return {};
        },
        change);
    C::finish_forward(*this, std::move(result));
}

void Session::forward_material(ObjectId object) {
    if (!forwarding_ || live_failure_)
        return;
    auto encoded = [&]() -> Result<void> {
        ATELIER_TRY_BIND(asset_key, material::material_key(runtime_, object));
        json::Builder op;
        if (!asset_key->empty()) {
            op.set_validated(op.root(), "op", "resource");
            ATELIER_TRY(scene_result(op.set(op.root(), "key", *asset_key)));
            ATELIER_TRY_BIND(asset, material_asset(object));
            ATELIER_TRY_BIND(material, scene_result(assets::material_format::encode(*asset)));
            op.set_validated(op.root(), "material", material->root());
            if (const auto at = materials_.find(*asset_key); at != materials_.end() && at->second.source_file) {
                const auto source = op.object_validated(op.root(), "source");
                ATELIER_TRY(scene_result(op.set(source, "path", at->second.source_file->path)));
                ATELIER_TRY(scene_result(op.set(source, "text", at->second.source_file->source)));
            }
            return GraphCodec::live_push(*this, "resource " + *asset_key, op.root());
        } else {
            ATELIER_TRY_BIND(key, scene_result(GraphCodec::identity(*this, object)));
            op.set_validated(op.root(), "op", "materials");
            const auto definitions = *op.array(op.root(), "materials");
            std::unordered_set<ObjectId> defined;
            ATELIER_TRY(GraphCodec::material_definitions(*this, object, op, definitions, defined));
            return GraphCodec::live_push(*this, "material " + std::to_string(*key), op.root());
        }
    }();
    GraphCodec::finish_forward(*this, std::move(encoded));
}

void Session::forward_texture(std::string_view key, material::ImageRef image) {
    if (!forwarding_ || live_failure_)
        return;
    auto sent = [&]() -> Result<void> {
        if (!image)
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "A Play texture publication requires image content."});
        json::Builder op;
        op.set_validated(op.root(), "op", "texture");
        ATELIER_TRY(scene_result(op.set(op.root(), "key", key)));
        return GraphCodec::live_push(*this, "texture " + std::string(key), op.root(), std::move(image));
    }();
    GraphCodec::finish_forward(*this, std::move(sent));
}

void Session::forward_mesh(const assets::MeshAsset& asset) {
    if (!forwarding_ || live_failure_)
        return;
    auto sent = [&]() -> Result<void> {
        json::Builder op;
        op.set_validated(op.root(), "op", "mesh");
        ATELIER_TRY(scene_result(op.set(op.root(), "key", asset.key)));
        return GraphCodec::live_push(*this, "mesh " + asset.key, op.root(), {},
                                     std::make_shared<const assets::MeshAsset>(asset));
    }();
    GraphCodec::finish_forward(*this, std::move(sent));
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
            kept.prune_reference_roots();
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
    ATELIER_TRY(publish_procedural_mesh(reader, asset));
    ++revision_;
    forward_mesh(asset);
    return {};
}

void Session::forward_edit() {
    if (!forwarding_ || live_failure_)
        return;
    using C = GraphCodec;
    // One op per TRS leaf, so a later preview never drops a leaf an earlier op carried.
    const auto space = [&](ObjectId entity, const world::Trs& before) -> Result<void> {
        ATELIER_TRY_BIND(current, world::read_transform(runtime_, entity));
        auto& sent = live_leaves_[entity];
        sent |= C::leaf_mask(before, *current);
        ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, entity)));
        for (const unsigned leaf : {1u, 2u, 4u})
            if (sent & leaf) {
                ATELIER_TRY_BIND(value, C::space_leaves(*current, leaf));
                json::Builder op;
                op.set_validated(op.root(), "op", "set");
                op.set_validated(op.root(), "id", *id);
                op.set_validated(op.root(), "space", value->root());
                ATELIER_TRY(C::live_push(*this, "space " + *id + " " + std::to_string(leaf), op.root()));
            }
        return {};
    };
    const auto fields = [&](ObjectId object, std::span<const world::FieldPath> paths) -> Result<void> {
        std::string key;
        ATELIER_TRY_BIND(op, C::live_properties(*this, object, paths, &key));
        return C::live_push(*this, std::move(key), op->root());
    };
    auto encoded = [&]() -> Result<void> {
        if (material_edit_) {
            forward_material(material_edit_->root.id());
            if (live_failure_)
                return {};
        }
        if (graph_param_edit_ && graph_param_edit_->generator_root.id() != ObjectId{}) {
            ATELIER_TRY_BIND(id, scene_result(C::live_id(*this, graph_param_edit_->generator_root.id())));
            json::Builder op;
            op.set_validated(op.root(), "op", "set");
            op.set_validated(op.root(), "id", *id);
            const auto ov = generator_overrides(graph_param_edit_->generator_root.id());
            op.set_validated(op.root(), "overrides", ov.root());
            ATELIER_TRY(C::live_push(*this, "overrides " + *id, op.root()));
        }
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
    GraphCodec::finish_forward(*this, std::move(encoded));
}

void Session::enter_play() {
    play_ = true;
    for (const auto& [object, key] : local_ids_)
        if (runtime_.type_of(object))
            live_objects_.insert_or_assign(key, object);
}

Result<std::vector<world::Diagnostic>> Session::apply_live(std::string_view text, Project& project) {
    if (!play_)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Editor changes apply only to a Play copy."});
    if (world_.closed())
        return std::unexpected(Error{ErrorCode::invalid_storage, "The Play World is closed."});
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
    for (auto op : *ops) {
        GraphCodec::PlayCommand command{*this, project, result, "Play change " + std::to_string(index++)};
        if (auto applied = command.apply(op); !applied)
            command.reject(applied.error());
    }
    ATELIER_TRY(sync_procedural_generators(&project));
    return result;
}

Result<std::unique_ptr<Session>> Session::deserialize(std::string_view text, Project& project, Prepare prepare,
                                                      LoadPolicy policy, SessionRole role) {
    ATELIER_TRY_BIND(session, restore_scene(text, project, std::move(prepare), policy, role));
    ATELIER_TRY((*session)->sync_procedural_generators(&project));
    return std::move(*session);
}

Result<std::unique_ptr<Session>> Session::restore_scene(std::string_view text, Project& project, Prepare prepare,
                                                        LoadPolicy policy, SessionRole role) {
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
        ATELIER_TRY(scene_result(GraphCodec::prepare_cook(*session, prepared, plans, load)));
        ATELIER_TRY_BIND(commit, session->world_.attach_batch(plans, prepared.spaces));
        prepared.published = commit->changed;
        if (commit->notification_error)
            return std::unexpected(std::move(*commit->notification_error));
    }
    prepared.published = true;
    session->retained_ = std::move(load.retained);
    session->retained_document_ = std::move(load.document_retained);
    session->load_issues_ = std::move(load.issues);
    session->next_name_ = graph.data.root().at("entities")->size() + 1;
    return session;
}

Result<world::Commit> Session::instantiate_subtree(std::string_view text, Project& project, ObjectId parent) {
    if (editing() || world_.running())
        return std::unexpected(Error{ErrorCode::invalid_storage,
                                     "Composition import requires a stopped document without an active edit."});
    if (parent != ObjectId{})
        ATELIER_TRY(admit_entity_edit(parent));
    GraphCodec::Graph graph;
    ATELIER_TRY_BIND(parsed, scene_result(assets::json_format::parse(text)));
    graph.data = std::move(*parsed);
    GraphCodec::Load load;
    ATELIER_TRY_BIND(prepared, scene_result(GraphCodec::construct(*this, graph, &project, false, load)));
    if (prepared->roots.empty())
        return world::Commit{};
    ATELIER_TRY_BIND(published, scene_result(GraphCodec::publish(*this, *prepared, graph, parent, false, load)));
    if (published->changed)
        ATELIER_TRY(sync_procedural_generators(&project));
    return published;
}

Result<world::Commit> Session::duplicate_entities(std::span<const ObjectId> entities) {
    if (editing())
        return std::unexpected(Error{ErrorCode::invalid_storage, "Finish the active edit before duplicating."});
    if (entities.empty())
        return world::Commit{};
    ATELIER_TRY_BIND(graph, GraphCodec::capture(*this, entities, false));
    GraphCodec::Load load;
    ATELIER_TRY_BIND(built, scene_result(GraphCodec::construct(*this, *graph, nullptr, false, load)));
    auto& prepared = *built;
    for (auto root : prepared.roots) {
        const auto original = required_entry(prepared.labels, root);
        for (std::size_t suffix = 1;; ++suffix) {
            const auto tail = L" Copy" + (suffix == 1 ? std::wstring{} : L" " + std::to_wstring(suffix));
            auto prefix = original.substr(0, 256 - tail.size());
            if (!prefix.empty() && prefix.back() >= 0xd800 && prefix.back() <= 0xdbff)
                prefix.pop_back();
            auto label = prefix + tail;
            const auto matches = [&](const auto& item) { return item.first != root && item.second == label; };
            if (std::ranges::none_of(names_, matches) && std::ranges::none_of(prepared.labels, matches)) {
                ATELIER_TRY(validate_name(label));
                prepared.labels[root] = std::move(label);
                break;
            }
        }
    }
    return scene_result(GraphCodec::publish(*this, prepared, *graph, {}, true, load));
}

void Session::Retained::prune_reference_roots() {
    if (reference_roots.empty())
        return;
    std::unordered_set<std::uint64_t> kept;
    for (const std::string_view text : {std::string_view(record), std::string_view(slots)}) {
        if (text.empty())
            continue;
        const auto parsed =
            json::Document::parse(text, {std::numeric_limits<std::size_t>::max(), json::DepthPolicy::containers, true});
        if (!parsed)
            fatal("Invalid retained JSON produced by the Scene writer.");
        const auto visited = each_reference(parsed->root(), [&](json::Value node) -> SceneResult<void> {
            ATELIER_TRY_BIND(value, node.at("$ref"));
            ATELIER_TRY_BIND(id, local_key(*value));
            kept.insert(*id);
            return {};
        });
        if (!visited)
            fatal("Invalid retained reference produced by the Scene writer.");
    }
    std::erase_if(reference_roots, [&](const auto& entry) { return !kept.contains(entry.first); });
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
    const auto update = [&](ObjectId object, Retained& entry, Retained after) {
        after.prune_reference_roots();
        swaps.push_back({object, entry, after});
        if (object != ObjectId{} && after.empty())
            retained_.erase(object);
        else
            entry = std::move(after);
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
        if (!detail::erase_path(record, record.root(), path))
            return;
        auto after = *entry;
        after.record = dump_retained(record.root());
        update(object, *entry, std::move(after));
    };
    const auto drop_slots = [&](ObjectId object) {
        auto* entry = entry_of(object);
        if (!entry)
            return;
        auto after = *entry;
        bool changed = false;
        if (!after.slots.empty()) {
            after.slots.clear();
            changed = true;
        }
        if (!after.materials.empty()) {
            after.materials.clear();
            changed = true;
        }
        if (!after.record.empty()) {
            const auto parsed = json::Document::parse(
                after.record, {std::numeric_limits<std::size_t>::max(), json::DepthPolicy::containers, true});
            if (!parsed)
                fatal("Invalid retained JSON produced by the Scene writer.");
            json::Builder record(*parsed);
            static const std::array<std::string, 1> materials_key{"materials"};
            if (detail::erase_path(record, record.root(), materials_key)) {
                after.record = dump_retained(record.root());
                changed = true;
            }
        }
        if (!changed)
            return;
        update(object, *entry, std::move(after));
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
    static const std::array<std::string, 1> enabled_key{"enabled"};
    static const std::array<std::string, 1> sky{"sky_map"};
    static const std::array<std::string, 1> proc_graph{"graph"};
    static const std::array<std::string, 1> proc_overrides{"overrides"};
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
            else if constexpr (std::is_same_v<T, EnabledChange>)
                drop(item.root.id(), enabled_key);
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
            else if constexpr (std::is_same_v<T, ProceduralGraphChange>) {
                if (item.before != item.after)
                    drop(item.root.id(), proc_graph);
                if (item.before_overrides != item.after_overrides)
                    drop(item.root.id(), proc_overrides);
            }
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

std::string_view Session::placeholder_type(ObjectId object) const noexcept {
    const auto at = retained_.find(object);
    return at == retained_.end() ? std::string_view{} : std::string_view(at->second.type);
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
    bool material{};
    std::vector<detail::ScalarField> schema;
    const MissingResource* mesh_unavailable{};
    if (!document) {
        ATELIER_TRY_BIND(type, runtime_.type_of(object));
        ATELIER_TRY_BIND(is_material, is_material_type(*type));
        material = *is_material;
        if (!material) {
            ATELIER_TRY_BIND(fields, business_fields(world_.runtime(), types_, *type));
            schema = std::move(*fields);
            ATELIER_TRY_BIND(static_mesh, runtime_.is_a(*type, presentation_types_.static_mesh_component));
            if (*static_mesh) {
                ATELIER_TRY_BIND(mesh, presentation::read_static_mesh(world_.runtime(), object));
                if (!mesh->mesh.resolved)
                    mesh_unavailable = unavailable(assets::AssetKind::mesh, mesh->mesh.source);
            }
        }
    }
    // Descends into records the current schema still declares; anything else is one entry.
    const auto classify = [&](std::span<const std::string> path) -> std::pair<bool, world::DiagnosticKind> {
        using enum world::DiagnosticKind;
        if (document || material)
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
            return {false, path[1] == "position" || path[1] == "rotation" || path[1] == "scale" || path[1] == "origin"
                               ? mismatch
                               : unknown_key};
        }
        if (path.front() == "overrides")
            return path.size() == 1 ? std::pair{true, mismatch} : std::pair{false, unknown_field};
        if (path.front() == "materials" && mesh_unavailable)
            return {false, mesh_unavailable->kind};
        constexpr std::array known{"name", "enabled", "mesh", "graph", "overrides", "materials", "sky_map"};
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
    if (!entry->record.empty()) {
        ATELIER_TRY_BIND(record,
                         scene_result(json::Document::parse(entry->record, {std::numeric_limits<std::size_t>::max(),
                                                                            json::DepthPolicy::containers, true})));
        ATELIER_TRY(scene_result(walk(walk, record->root())));
    }
    if (!entry->slots.empty()) {
        ATELIER_TRY_BIND(slots,
                         scene_result(json::Document::parse(entry->slots, {std::numeric_limits<std::size_t>::max(),
                                                                           json::DepthPolicy::containers, true})));
        ATELIER_TRY_BIND(members, scene_result(slots->root().object_items()));
        for (const auto& [slot, keys] : *members) {
            ATELIER_TRY_BIND(items, scene_result(keys.object_items()));
            for (const auto& [key, value] : *items)
                view.entries.push_back(
                    {{"materials", std::string(slot), std::string(key)},
                     preview(value),
                     key == "overridden" ? world::DiagnosticKind::mismatch : world::DiagnosticKind::unknown_key});
        }
    }
    if (document)
        for (const auto& resource : entry->materials)
            if (runtime_.type_of(resource.id())) {
                const auto key = document_identity(resource.id());
                json::Builder value;
                ATELIER_TRY_BIND(asset_key, material::material_key(world_.runtime(), resource.id()));
                ATELIER_TRY(scene_result(value.set(value.root(), *asset_key)));
                view.entries.push_back({{"materials", key ? std::to_string(*key) : std::string("?")},
                                        preview(value.root()),
                                        world::DiagnosticKind::unknown_key});
            }
    return view;
}

std::vector<world::Diagnostic> Session::issues() const {
    std::vector<world::Diagnostic> result;
    if (retained_.empty() && !has_retained({}))
        return result;
    const bool readable = world_.members().has_value();
    const auto owner = [&](ObjectId object) -> ObjectId {
        if (!readable)
            return {};
        if (world_.contains(object))
            return object;
        const auto component = world::inspect_component(world_.runtime(), object);
        return component && component->valid && world_.contains(component->owner) ? component->owner : ObjectId{};
    };
    std::unordered_map<std::uint64_t, ObjectId> editor_objects;
    if (!play_)
        for (const auto& [object, key] : local_ids_)
            editor_objects.emplace(key, object);
    const auto& by_key = play_ ? live_objects_ : editor_objects;
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
        item.message = subject + (field.empty() ? std::string{} : " " + detail::dotted(field)) + ": " + detail;
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
    // Serials and display order follow document identity. A stale object has no issue to display.
    std::vector<std::tuple<std::uint64_t, ObjectId, const Retained*, TypeId>> listed;
    for (const auto& [object, entry] : retained_) {
        const auto type = runtime_.type_of(object);
        if (!type || (owner(object) == ObjectId{} && !is_material(object)))
            continue;
        if (const auto key = document_identity(object))
            listed.emplace_back(*key, object, &entry, *type);
    }
    std::ranges::sort(listed, {}, [](const auto& item) { return std::get<0>(item); });
    for (const auto& [key, object, entry, type] : listed) {
        if (!entry->type.empty())
            report(object, *entry, entry->type);
        else if (const auto info = runtime_.describe(type))
            report(object, *entry, std::string((*info)->name));
    }
    report({}, retained_document_, {});
    return result;
}

Result<ObjectId> Session::local_object(std::uint64_t identity) const {
    if (!identity)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Document identity cannot be empty."});
    ObjectId object;
    if (play_) {
        const auto found = live_objects_.find(identity);
        if (found != live_objects_.end() && local_ids_.contains(found->second))
            object = found->second;
    } else {
        const auto found = std::ranges::find(local_ids_, identity, &decltype(local_ids_)::value_type::second);
        if (found != local_ids_.end())
            object = found->first;
    }
    if (object == ObjectId{})
        return std::unexpected(Error{ErrorCode::invalid_storage, "Unknown document object identity."});
    ATELIER_TRY(world_.members());
    if (world_.contains(object))
        return object;
    if (const auto type = runtime_.type_of(object)) {
        ATELIER_TRY_BIND(component, runtime_.is_a(*type, types_.component));
        if (*component) {
            ATELIER_TRY_BIND(state, world::inspect_component(world_.runtime(), object));
            if (world_.contains(state->owner))
                return object;
        } else if (is_material(object))
            return object;
    }
    return std::unexpected(Error{ErrorCode::invalid_storage,
                                 "Document identity does not belong to a live Entity, Component or material."});
}
}
