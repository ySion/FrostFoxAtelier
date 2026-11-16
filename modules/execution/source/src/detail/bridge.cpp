#include "source/detail/bridge.hpp"
#include "diagnostics.hpp"
#include "debugger.hpp"
#include "foundation/map.hpp"
#include "language/declarations.hpp"
#include "memory/fatal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

namespace atelier::source::detail {
Result<Binding> bind_type(Runtime& runtime, TypeId type, std::span<const TypeInfo* const> catalog) noexcept {
    std::vector<const TypeInfo*> published;
    if (catalog.empty()) {
        const auto ids = runtime.types();
        published.reserve(ids.size());
        for (const auto id : ids)
            published.push_back(*runtime.describe(id));
        catalog = published;
    }
    Map<TypeId, const TypeInfo*> info_by_id;
    info_by_id.reserve(catalog.size());
    for (const auto* candidate : catalog)
        info_by_id.insert_or_assign(candidate->id, candidate);
    const TypeInfo* info{};
    if (type != TypeId{}) {
        if (const auto* found = info_by_id.find(type))
            info = *found;
        if (!info || info->kind != TypeKind::object)
            return std::unexpected(
                Error{ErrorCode::invalid_type, "Source receiver must be an object in this Runtime."});
    }
    // Published and preview catalogs are validated by Runtime. Only the requested
    // receiver is caller input; all following identities are catalog dependencies.
    const auto describe = [&](TypeId id) -> const TypeInfo* {
        const auto* found = info_by_id.find(id);
        assert(found != nullptr);
        return *found;
    };
    Binding binding{};
    binding.runtime = &runtime;
    binding.type = type;
    binding.f32 = *runtime.native_type<float>();
    binding.f64 = *runtime.native_type<double>();
    binding.u32 = *runtime.native_type<std::uint32_t>();
    binding.i32 = *runtime.native_type<std::int32_t>();
    binding.boolean = *runtime.native_type<bool>();
    Map<TypeId, BusinessType> kind_by_id;
    kind_by_id.reserve(catalog.size());
    kind_by_id.insert_or_assign(binding.f32, ValueKind::f32);
    kind_by_id.insert_or_assign(binding.f64, ValueKind::f64);
    kind_by_id.insert_or_assign(binding.u32, ValueKind::u32);
    kind_by_id.insert_or_assign(binding.i32, ValueKind::i32);
    kind_by_id.insert_or_assign(binding.boolean, ValueKind::boolean);
    for (const auto* enumeration : catalog) {
        const auto id = enumeration->id;
        if (!enumeration->enumeration || language::editor_reserved(enumeration->name))
            continue;
        binding.enumerations.push_back(
            {id, enumeration->name,
             enumeration->enumeration->underlying == binding.i32 ? ValueKind::i32 : ValueKind::u32,
             enumeration->enumeration->values});
    }
    std::sort(binding.enumerations.begin(), binding.enumerations.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    for (std::uint32_t i = 0; i < binding.enumerations.size(); ++i)
        kind_by_id.insert_or_assign(binding.enumerations[i].type, BusinessType{ValueKind::enumeration, i});
    const auto append_value = [&](const TypeInfo& value, ValueKind kind) {
        binding.values.push_back(
            {value.id,
             value.name,
             kind,
             value.size,
             value.alignment,
             ValueKind::void_value,
             {},
             value.sequence_view,
             value.value_operations_preserve_borrows && value.copy_constructible && value.copy_assignable,
             value.sequence_mutate != nullptr,
             value.math,
             value.plain_value});
        kind_by_id.insert_or_assign(value.id, BusinessType{kind, 0});
    };
    std::vector<const TypeInfo*> pending_arrays;
    for (const auto* value : catalog) {
        if (value->contains_references || language::editor_reserved(value->name))
            continue;
        if (value->kind == TypeKind::record)
            append_value(*value, ValueKind::record);
        else if (value->kind == TypeKind::string && value->sequence_view)
            append_value(*value, ValueKind::string);
        else if (value->kind == TypeKind::array && value->sequence_view)
            pending_arrays.push_back(value);
    }
    // A legal Runtime value need not expose a source projection. Admit arrays
    // only after their elements, leaving unavailable dependency chains opaque.
    while (std::erase_if(pending_arrays, [&](const TypeInfo* value) {
               if (!kind_by_id.contains(*value->element))
                   return false;
               append_value(*value, ValueKind::array);
               return true;
           }) != 0) {
    }
    std::sort(binding.values.begin(), binding.values.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    for (std::uint32_t i = 0; i < binding.values.size(); ++i)
        kind_by_id.insert_or_assign(binding.values[i].type, BusinessType{binding.values[i].kind, i});
    const auto lookup_kind = [&](TypeId id) noexcept -> BusinessType {
        const auto* found = kind_by_id.find(id);
        return found ? *found : BusinessType{ValueKind::void_value};
    };
    for (auto& value : binding.values) {
        const auto* value_info = describe(value.type);
        if (value_info->element) {
            value.element = lookup_kind(*value_info->element);
            assert(value.element != ValueKind::void_value);
        }
        for (const auto& field : value_info->fields) {
            if (!field.offset || field.value.form != ValueForm::inline_value)
                continue;
            const auto kind = lookup_kind(field.value.type);
            if (kind != ValueKind::void_value)
                value.fields.push_back({field.name, kind, *field.offset});
        }
    }
    if (type == TypeId{})
        return binding;
    binding.name = info->name;
    const auto append_base = [&](const TypeInfo* actual, TypeId owner, std::vector<BusinessAddressStep>& access) {
        while (actual->id != owner) {
            assert(actual->base.has_value());
            if (actual->base_address)
                access.push_back({actual->base_address});
            actual = describe(*actual->base);
        }
    };
    struct FieldCursor {
        const TypeInfo* type;

        std::size_t next{};

        std::size_t depth{};

        std::size_t prefix_length{};

        bool constrained{};

        std::size_t access_depth{};

        FieldRange range{};
    };
    std::vector<FieldCursor> cursors{{info}};
    std::vector<FieldId> path;
    std::vector<BusinessAddressStep> access;
    std::string prefix;
    while (!cursors.empty()) {
        auto& cursor = cursors.back();
        if (cursor.next == cursor.type->fields.size()) {
            cursors.pop_back();
            continue;
        }
        path.resize(cursor.depth);
        prefix.resize(cursor.prefix_length);
        access.resize(cursor.access_depth);
        const auto& field = cursor.type->fields[cursor.next++];
        if (field.value.form != ValueForm::inline_value)
            continue;
        append_base(cursor.type, field.owner, access);
        assert(field.offset.has_value() || field.native_address);
        access.push_back({field.offset ? nullptr : field.native_address, field.offset.value_or(0)});
        path.push_back(field.id);
        prefix += field.name;
        const auto field_kind = lookup_kind(field.value.type);
        const bool typed_field = field_kind.kind == ValueKind::record && binding.values[field_kind.type_index].math;
        const bool constrained = cursor.constrained || field.range.constrains();
        const auto range = field.range.constrains() ? field.range : cursor.type->math ? cursor.range : FieldRange{};
        if (field_kind != ValueKind::void_value && (!aggregate(field_kind) || typed_field)) {
            binding.fields.push_back({prefix, path, field_kind, constrained, access,
                                      range.hard_min ? scalar(*range.hard_min, binding).bits : 0,
                                      range.hard_max ? scalar(*range.hard_max, binding).bits : 0,
                                      (range.hard_min ? 1u : 0u) | (range.hard_max ? 2u : 0u)});
        }
        if (field_kind == ValueKind::void_value || aggregate(field_kind)) {
            const auto* nested = describe(field.value.type);
            if (nested->kind != TypeKind::record)
                continue;
            prefix += '.';
            cursors.push_back({nested, 0, path.size(), prefix.size(), constrained, access.size(), range});
        }
    }
    for (const auto& method : info->methods) {
        const auto& signature = method.signature;
        assert(signature.receiver.has_value());
        // As with fields, methods whose values have no source projection stay
        // native-only without preventing unrelated source functions from binding.
        const auto result = signature.result ? lookup_kind(signature.result->type) : ValueKind::void_value;
        if ((signature.result && result == ValueKind::void_value) ||
            std::any_of(signature.parameters.begin(), signature.parameters.end(),
                        [&](const auto& parameter) { return lookup_kind(parameter.type) == ValueKind::void_value; }))
            continue;
        if (signature.parameters.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(Error{ErrorCode::capacity_exceeded, "Host signature is too large."});
        BoundMethod bound{
            method.name,        method.id, {}, result, signature.receiver->read_only, method.preserves_borrows,
            method.native_entry};
        append_base(info, signature.receiver->type, bound.receiver_access);
        for (const auto parameter : signature.parameters)
            bound.parameters.push_back({lookup_kind(parameter.type), parameter.passing});
        binding.methods.push_back(std::move(bound));
    }
    if (binding.fields.size() > std::numeric_limits<std::uint32_t>::max() ||
        binding.methods.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(Error{ErrorCode::capacity_exceeded, "Source member catalog is too large."});
    // Stable catalogs are independent of native/descriptor field declaration order.
    std::sort(binding.fields.begin(), binding.fields.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    std::sort(binding.methods.begin(), binding.methods.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return binding;
}

BusinessType value_kind(const Binding& binding, TypeId type) {
    if (type == binding.f32)
        return ValueKind::f32;
    if (type == binding.f64)
        return ValueKind::f64;
    if (type == binding.u32)
        return ValueKind::u32;
    if (type == binding.i32)
        return ValueKind::i32;
    if (type == binding.boolean)
        return ValueKind::boolean;
    for (std::uint32_t i = 0; i < binding.enumerations.size(); ++i)
        if (type == binding.enumerations[i].type)
            return {ValueKind::enumeration, i};
    for (std::uint32_t i = 0; i < binding.values.size(); ++i)
        if (type == binding.values[i].type)
            return {binding.values[i].kind, i};
    return ValueKind::void_value;
}

TypeId value_type_id(const Binding& binding, BusinessType type) {
    if (type == ValueKind::f32)
        return binding.f32;
    if (type == ValueKind::f64)
        return binding.f64;
    if (type == ValueKind::u32)
        return binding.u32;
    if (type == ValueKind::i32)
        return binding.i32;
    if (type == ValueKind::boolean)
        return binding.boolean;
    if (type.kind == ValueKind::enumeration && type.type_index < binding.enumerations.size())
        return binding.enumerations[type.type_index].type;
    if (aggregate(type) && type.type_index < binding.values.size() && type.kind == binding.values[type.type_index].kind)
        return binding.values[type.type_index].type;
    return {};
}

BusinessValue scalar(const Scalar& value, const Binding& binding) {
    return std::visit(
        [&](const auto& item) -> BusinessValue {
            using T = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::same_as<T, EnumValue>)
                return {value_kind(binding, item.type), item.bits};
            else
                return scalar(item);
        },
        value);
}

EnvironmentView::EnvironmentView(const Binding& binding) noexcept {
    enum_items.reserve(binding.enumerations.size());
    enumerations.reserve(binding.enumerations.size());
    for (const auto& enumeration : binding.enumerations) {
        auto& items = enum_items.emplace_back();
        for (const auto& [name, bits] : enumeration.values)
            items.push_back({name.c_str(), bits});
        enumerations.push_back(
            {enumeration.name.c_str(), enumeration.underlying, items.data(), static_cast<std::uint32_t>(items.size())});
    }
    value_fields.reserve(binding.values.size());
    values.reserve(binding.values.size());
    for (const auto& value : binding.values) {
        auto& fields = value_fields.emplace_back();
        for (const auto& field : value.fields)
            fields.push_back({field.name.c_str(), field.type, field.offset});
        values.push_back({value.name.c_str(), value.kind, value.size, value.alignment, value.element, fields.data(),
                          static_cast<std::uint32_t>(fields.size()), value.sequence_view, value.can_own,
                          value.can_mutate, value.math, value.plain_value});
    }
    for (const auto& field : binding.fields)
        fields.push_back({field.name.c_str(), field.kind, field.constrained, field.access.data(),
                          static_cast<std::uint32_t>(field.access.size()), field.minimum, field.maximum, field.bounds});
    for (const auto& method : binding.methods) {
        methods.push_back({method.name.c_str(), method.parameters.data(),
                           static_cast<std::uint32_t>(method.parameters.size()), method.result,
                           method.receiver_read_only, method.preserves_borrows, method.entry,
                           method.receiver_access.data(), static_cast<std::uint32_t>(method.receiver_access.size())});
    }
}

BusinessEnvironment EnvironmentView::view(const char* object_type_name) const {
    return {fields.data(),
            static_cast<std::uint32_t>(fields.size()),
            methods.data(),
            static_cast<std::uint32_t>(methods.size()),
            object_type_name,
            enumerations.data(),
            static_cast<std::uint32_t>(enumerations.size()),
            values.data(),
            static_cast<std::uint32_t>(values.size())};
}

std::optional<Scalar> scalar_value(BusinessValue value, const Binding* binding) {
    if (value.storage != ArgumentStorage::value)
        return std::nullopt;
    if (value.kind == ValueKind::boolean) {
        if (value.bits > 1)
            return std::nullopt;
        return value.bits != 0;
    }
    if (value.kind != ValueKind::f64 && value.bits > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    if (value.kind == ValueKind::f32)
        return std::bit_cast<float>(static_cast<std::uint32_t>(value.bits));
    if (value.kind == ValueKind::f64)
        return std::bit_cast<double>(value.bits);
    if (value.kind == ValueKind::u32)
        return static_cast<std::uint32_t>(value.bits);
    if (value.kind == ValueKind::i32)
        return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value.bits));
    if (binding && value.kind.kind == ValueKind::enumeration && value.kind.type_index < binding->enumerations.size())
        return EnumValue{binding->enumerations[value.kind.type_index].type, static_cast<std::uint32_t>(value.bits)};
    return std::nullopt;
}

namespace {
template <class T, std::size_t N>
InlineArray<T, N> sized_slots(std::size_t count) noexcept {
    InlineArray<T, N> slots;
    if (count)
        slots.resize(count);
    return slots;
}
}

AbiArguments::AbiArguments(const Binding& binding, const FunctionSignature& signature,
                           const BusinessSignature& compiled, std::span<const Scalar> arguments) {
    atelier::detail::assert_serial_access();
    if (arguments.size() != signature.parameters.size()) {
        status = std::unexpected(Error{ErrorCode::argument_mismatch, "Incorrect argument count."});
        return;
    }
    bool direct_scalars = true;
    for (std::uint32_t i = 0; i < compiled.parameter_count; ++i) {
        if (compiled.parameters[i].passing != ParameterPassing::value || aggregate(compiled.parameters[i].type)) {
            direct_scalars = false;
            break;
        }
    }
    if (direct_scalars) {
        values.resize(arguments.size());
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto expected = signature.parameters[i].type;
            const auto kind = compiled.parameters[i].type;
            bool matched = false;
            BusinessValue encoded{};
            std::visit(
                [&](const auto& item) {
                    using T = std::remove_cvref_t<decltype(item)>;
                    if constexpr (std::same_as<T, float>) {
                        if (expected == binding.f32) {
                            encoded = scalar(item);
                            matched = true;
                        }
                    } else if constexpr (std::same_as<T, double>) {
                        if (expected == binding.f64) {
                            encoded = scalar(item);
                            matched = true;
                        }
                    } else if constexpr (std::same_as<T, std::uint32_t>) {
                        if (expected == binding.u32) {
                            encoded = scalar(item);
                            matched = true;
                        }
                    } else if constexpr (std::same_as<T, std::int32_t>) {
                        if (expected == binding.i32) {
                            encoded = scalar(item);
                            matched = true;
                        }
                    } else if constexpr (std::same_as<T, bool>) {
                        if (expected == binding.boolean) {
                            encoded = scalar(item);
                            matched = true;
                        }
                    } else if constexpr (std::same_as<T, EnumValue>) {
                        if (kind.kind == ValueKind::enumeration && item.type == expected) {
                            encoded = {kind, item.bits};
                            matched = true;
                        }
                    }
                },
                arguments[i]);
            if (!matched) {
                values.clear();
                status = std::unexpected(
                    Error{ErrorCode::argument_mismatch, "Argument type does not match parameter " + std::to_string(i)});
                return;
            }
            values[i] = encoded;
        }
        return;
    }
    InlineArray<Argument, 8> input;
    input.resize(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i)
        input[i] = Argument{arguments[i]};
    initialize(binding, signature, compiled, input);
}

AbiArguments::AbiArguments(const Binding& binding, const FunctionSignature& signature,
                           const BusinessSignature& compiled, std::span<const Argument> arguments) {
    initialize(binding, signature, compiled, arguments);
}

void AbiArguments::initialize(const Binding& binding, const FunctionSignature& signature,
                              const BusinessSignature& compiled, std::span<const Argument> arguments) {
    status = binding.runtime->prepare_arguments(signature, arguments, prepared);
    if (!status)
        return;
    values.resize(arguments.size());
    const auto views = prepared.values();
    for (std::size_t i = 0; i < values.size(); ++i) {
        BusinessValue encoded{};
        encoded.kind = compiled.parameters[i].type;
        if (signature.parameters[i].passing == ParameterPassing::value) {
            if (aggregate(encoded.kind))
                encoded.data = views[i].data;
            else if (encoded.kind == ValueKind::boolean)
                encoded.bits = *static_cast<const bool*>(views[i].data) ? 1u : 0u;
            else
                std::memcpy(&encoded.bits, views[i].data, encoded.kind == ValueKind::f64 ? 8 : 4);
        } else {
            encoded.data = views[i].data;
            encoded.storage = signature.parameters[i].passing == ParameterPassing::reference
                                  ? ArgumentStorage::reference
                                  : ArgumentStorage::read_only;
        }
        values[i] = encoded;
    }
}

Invocation::Invocation(Runtime& owner, const Binding& bound, BusinessFrameLayout layout, BusinessTrace* capture,
                       DebugSessionState* session, ExecutionScope* scope)
    : runtime(owner), binding(bound),
      roots(sized_slots<ObjectRoot, 2>(!scope && bound.type != TypeId{} ? layout.strong_slots : 0)),
      references(sized_slots<ObjectReference, 4>(scope && bound.type != TypeId{} ? layout.strong_slots : 0)),
      observations(sized_slots<ObjectReference, 2>(layout.weak_slots)),
      frame(scope ? ExecutionFrame(*scope, references) : ExecutionFrame(owner, roots)),
      native(sized_slots<NativeContext, 4>(bound.type != TypeId{} ? layout.strong_slots + layout.weak_slots : 0)),
      trace(capture), debugger(session) {
    error[0] = '\0';
}

bool Invocation::resources_cleared() const noexcept {
    if (!frame.empty() || live_values != 0 || debug_top)
        return false;
    for (const auto& observation : observations)
        if (observation.description().type != TypeId{})
            return false;
    return true;
}

void Invocation::record_resources(bool cleared) const noexcept {
    if (!trace)
        return;
    trace->remaining_owned_values = live_values;
    trace->resources_cleared = cleared;
}

void Invocation::fail(ErrorCode code, const char* message) noexcept {
    if (failed)
        return;
    failed = true;
    if (trace && trace->failure_location == no_diagnostic_location)
        trace->failure_location = trace->current_location;
    error_code = code;
    std::snprintf(error, sizeof(error), "%s", message);
}

void Invocation::invalidate_native() noexcept {
    for (auto& context : native)
        context.storage = nullptr;
}

std::uint32_t native_receiver(void* context, std::uint32_t slot, NativeContext** output) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    if (slot < invocation.references.size() && slot < invocation.native.size()) {
        const auto& ref = invocation.references[slot];
        auto& cached = invocation.native[slot];
        if (cached.storage && cached.self == ref.id() && !ref.is_null()) {
            *output = &cached;
            return 0;
        }
    }
    return bridge(invocation, [&]() -> Result<void> {
        ATELIER_TRY_BIND(reference, reference_value(invocation, slot));
        const auto index =
            (slot & observation_slot_bit) ? invocation.frame.size() + (slot & ~observation_slot_bit) : slot;
        if (index >= invocation.native.size())
            return std::unexpected(Error{ErrorCode::invalid_object, "Native access requires a live receiver."});
        auto& cached = invocation.native[index];
        if (!cached.storage || cached.self != reference->id()) {
            ATELIER_TRY_BIND(resolved,
                             NativeContext::resolve(invocation.runtime, reference->id(), invocation.binding.type));
            cached = *resolved;
        }
        *output = &cached;
        return {};
    });
}

void native_stamp(NativeContext* context) noexcept {
    context->stamp();
}

void native_failure(void* context, const NativeFailure* failure) noexcept {
    static_cast<Invocation*>(context)->fail(static_cast<ErrorCode>(failure->code), failure->message);
}

void native_segment(void* context) noexcept {
    static_cast<Invocation*>(context)->invalidate_native();
}

std::uint32_t create_slot(void* context, std::uint32_t slot) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&] { return invocation.frame.create(slot, invocation.binding.type); });
}

Result<ObjectReference> reference_value(Invocation& invocation, std::uint32_t slot) {
    if (slot == null_reference_slot)
        return invocation.runtime.null_reference(invocation.binding.type, ValueForm::strong_reference);
    if ((slot & observation_slot_bit) == 0)
        return invocation.frame.reference(slot);
    const auto index = slot & ~observation_slot_bit;
    if (index >= invocation.observations.size())
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Observation slot is out of range."});
    const auto value = invocation.observations[index];
    if (value.description() != ValueDescription{invocation.binding.type, ValueForm::weak_reference})
        return std::unexpected(Error{ErrorCode::invalid_object, "Observation slot is not initialized."});
    return value;
}

std::uint32_t change_reference(void* context, ReferenceOperation operation, std::uint32_t destination,
                               std::uint32_t source) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        const bool weak = (destination & observation_slot_bit) != 0;
        const auto index = destination & ~observation_slot_bit;
        if (destination == null_reference_slot ||
            (weak ? index >= invocation.observations.size() : index >= invocation.frame.size()))
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Reference destination is out of range."});
        ATELIER_TRY_BIND(value, reference_value(invocation, source));
        if (value->description().type != invocation.binding.type)
            return std::unexpected(Error{ErrorCode::type_mismatch, "Reference does not match the bound object type."});
        const auto form = weak ? ValueForm::weak_reference : ValueForm::strong_reference;
        switch (operation) {
        case ReferenceOperation::copy:
            if (source == null_reference_slot)
                value = invocation.runtime.null_reference(invocation.binding.type, form);
            else if (value->description().form != form)
                return std::unexpected(
                    Error{ErrorCode::type_mismatch, "Reference assignment cannot change holding form."});
            break;
        case ReferenceOperation::observe:
            if (!weak || value->description().form != ValueForm::strong_reference || source == null_reference_slot)
                return std::unexpected(Error{ErrorCode::type_mismatch, "Observation requires a holding reference."});
            value = invocation.runtime.observe(*value);
            break;
        case ReferenceOperation::try_acquire:
            if (weak || value->description().form != ValueForm::weak_reference)
                return std::unexpected(
                    Error{ErrorCode::type_mismatch, "Acquisition requires an observation reference."});
            return invocation.frame.try_acquire(index, *value);
        default:
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Unknown reference operation."});
        }
        if (!value)
            return std::unexpected(std::move(value.error()));
        if (weak) {
            invocation.observations[index] = *value;
            return {};
        }
        return invocation.frame.hold(index, *value);
    });
}

std::uint32_t same_reference(void* context, std::uint32_t left, std::uint32_t right, std::uint32_t* output) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        ATELIER_TRY_BIND(lhs, reference_value(invocation, left));
        ATELIER_TRY_BIND(rhs, reference_value(invocation, right));
        if (lhs->description().type != invocation.binding.type || rhs->description().type != invocation.binding.type)
            return std::unexpected(
                Error{ErrorCode::type_mismatch, "Reference comparison crossed the bound object type."});
        *output = lhs->id() == rhs->id() ? 1u : 0u;
        return {};
    });
}

void clear_slot(void* context, std::uint32_t slot) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    (void)bridge(invocation, [&]() -> Result<void> {
        if ((slot & observation_slot_bit) == 0)
            return invocation.frame.clear(slot);
        const auto index = slot & ~observation_slot_bit;
        if (index >= invocation.observations.size())
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Observation slot is out of range."});
        invocation.observations[index] = {};
        return {};
    });
}

const BoundValue& owned_value(Invocation& invocation, std::uint32_t type) noexcept {
    // The frontend admits owned values only with these published capabilities.
    assert(type < invocation.binding.values.size() && invocation.binding.values[type].can_own);
    return invocation.binding.values[type];
}

std::uint32_t construct_value(void* context, std::uint32_t type, void* target, const void* source) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        const auto& value = owned_value(invocation, type);
        auto storage = std::span{static_cast<std::byte*>(target), value.size};
        auto result = source ? invocation.runtime.copy_construct_value(
                                   value.type, storage, {static_cast<const std::byte*>(source), value.size})
                             : invocation.runtime.initialize_value(value.type, storage);
        if (result) {
            ++invocation.live_values;
            if (invocation.trace) {
                invocation.trace->peak_owned_values =
                    std::max(invocation.trace->peak_owned_values, static_cast<std::uint64_t>(invocation.live_values));
                invocation.trace->remaining_owned_values = invocation.live_values;
            }
        }
        return result;
    });
}

std::uint32_t assign_value(void* context, std::uint32_t type, void* target, const void* source) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        const auto& value = owned_value(invocation, type);
        return invocation.runtime.assign_value(value.type, {static_cast<std::byte*>(target), value.size},
                                               {static_cast<const std::byte*>(source), value.size});
    });
}

void destroy_value(void* context, std::uint32_t type, void* target) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    (void)bridge(invocation, [&]() -> Result<void> {
        const auto& value = owned_value(invocation, type);
        if (invocation.live_values == 0)
            fatal("Value cleanup has no live construction.");
        auto result = invocation.runtime.destroy_value(value.type, {static_cast<std::byte*>(target), value.size});
        if (result) {
            --invocation.live_values;
            if (invocation.trace)
                invocation.trace->remaining_owned_values = invocation.live_values;
        }
        return result;
    });
}

std::uint32_t mutate_sequence(void* context, std::uint32_t type, SequenceMutation operation, void* target,
                              const void* source, std::uint64_t count) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        const auto& value = owned_value(invocation, type);
        TypeId source_type;
        std::size_t source_size = 0;
        if (operation == SequenceMutation::add || operation == SequenceMutation::append) {
            source_type =
                operation == SequenceMutation::add ? value_type_id(invocation.binding, value.element) : value.type;
            const auto info = invocation.runtime.describe(source_type);
            assert(info);
            source_size = (*info)->size;
        } else if (operation == SequenceMutation::assign_text || operation == SequenceMutation::append_text)
            source_size = count;
        assert(source || source_size == 0);
        return invocation.runtime.mutate_sequence(value.type, {static_cast<std::byte*>(target), value.size}, operation,
                                                  source_type, {static_cast<const std::byte*>(source), source_size},
                                                  count);
    });
}

const BusinessServices services{create_slot,    clear_slot,    change_reference, same_reference,  construct_value,
                                assign_value,   destroy_value, mutate_sequence,  debug_enter,     debug_point,
                                debug_leave,    nullptr,       nullptr,          native_receiver, native_stamp,
                                native_failure, native_segment};

bool same_environment(const Binding& binding, const BusinessEnvironment& environment) {
    if (binding.fields.size() != environment.field_count || binding.methods.size() != environment.method_count ||
        binding.enumerations.size() != environment.enumeration_count)
        return false;
    for (std::size_t i = 0; i < binding.enumerations.size(); ++i) {
        const auto& expected = binding.enumerations[i];
        const auto& actual = environment.enumerations[i];
        if (!actual.name || expected.name != actual.name || expected.underlying != actual.underlying ||
            expected.values.size() != actual.value_count)
            return false;
        for (std::size_t j = 0; j < expected.values.size(); ++j)
            if (!actual.values[j].name || expected.values[j].first != actual.values[j].name ||
                expected.values[j].second != actual.values[j].bits)
                return false;
    }
    if (binding.values.size() != environment.value_count || (environment.value_count && !environment.values))
        return false;
    for (std::size_t i = 0; i < binding.values.size(); ++i) {
        const auto& a = binding.values[i];
        const auto& b = environment.values[i];
        if (!b.name || a.name != b.name || a.kind != b.kind || a.size != b.size || a.alignment != b.alignment ||
            a.element != b.element || a.sequence_view != b.sequence_view || a.can_own != b.can_own ||
            a.can_mutate != b.can_mutate || a.math != b.math || a.plain_value != b.plain_value ||
            a.fields.size() != b.field_count || (b.field_count && !b.fields))
            return false;
        for (std::size_t j = 0; j < a.fields.size(); ++j) {
            const auto& x = a.fields[j];
            const auto& y = b.fields[j];
            if (!y.name || x.name != y.name || x.type != y.type || x.offset != y.offset)
                return false;
        }
    }
    for (std::size_t i = 0; i < binding.fields.size(); ++i) {
        const auto& expected = binding.fields[i];
        const auto& actual = environment.fields[i];
        if (expected.name != actual.name || expected.kind != actual.kind ||
            expected.constrained != actual.constrained || expected.minimum != actual.minimum ||
            expected.maximum != actual.maximum || expected.bounds != actual.bounds ||
            expected.access.size() != actual.access_count || (actual.access_count && !actual.access) ||
            !std::ranges::equal(expected.access, std::span{actual.access, actual.access_count}))
            return false;
    }
    for (std::size_t i = 0; i < binding.methods.size(); ++i) {
        const auto& actual = environment.methods[i];
        const auto& expected = binding.methods[i];
        if (expected.name != actual.name || expected.parameters.size() != actual.parameter_count ||
            expected.result != actual.result || expected.receiver_read_only != actual.receiver_read_only ||
            expected.preserves_borrows != actual.preserves_borrows || expected.entry != actual.entry ||
            expected.receiver_access.size() != actual.receiver_access_count ||
            (actual.receiver_access_count && !actual.receiver_access) ||
            !std::ranges::equal(expected.receiver_access,
                                std::span{actual.receiver_access, actual.receiver_access_count}))
            return false;
        for (std::size_t j = 0; j < expected.parameters.size(); ++j)
            if (expected.parameters[j] != actual.parameters[j])
                return false;
    }
    return true;
}

bool try_bind_receiver(Invocation& invocation, bool requires_receiver, ObjectId receiver) noexcept {
    if (!requires_receiver && receiver == ObjectId{})
        return true;
    if (invocation.references.empty())
        return false;
    NativeContext resolved;
    if (!NativeContext::try_resolve(invocation.runtime, receiver, invocation.binding.type, resolved))
        return false;
    invocation.frame.set_validated_reference(receiver_slot, invocation.binding.type, receiver);
    invocation.native[receiver_slot] = resolved;
    return true;
}

Result<void> bind_receiver(Invocation& invocation, bool requires_receiver, ObjectId receiver) {
    if (try_bind_receiver(invocation, requires_receiver, receiver))
        return {};
    ATELIER_TRY_BIND(resolved, NativeContext::resolve(invocation.runtime, receiver, invocation.binding.type));
    if (!invocation.references.empty()) {
        invocation.frame.set_validated_reference(receiver_slot, invocation.binding.type, receiver);
        invocation.native[receiver_slot] = *resolved;
        return {};
    }
    ATELIER_TRY(invocation.frame.hold(receiver_slot, receiver, invocation.binding.type));
    invocation.native[receiver_slot] = *resolved;
    return {};
}

Result<void> finish_invocation(Invocation& invocation, std::uint32_t status, const BusinessFailure& failure,
                               std::uint32_t* failure_site) {
    // Inspect generated cleanup before the RAII fallback.
    const bool cleared = invocation.resources_cleared();
    invocation.record_resources(cleared);
    if (status != 0 && !invocation.failed && invocation.debug_aborted) {
        if (!cleared)
            return std::unexpected(
                Error{ErrorCode::invocation_failed, "Aborted source execution left resources alive."});
        return std::unexpected(Error{ErrorCode::execution_aborted, "Source execution aborted by the debugger."});
    }
    if (status != 0 || invocation.failed) {
        if (failure_site)
            *failure_site = failure.location;
        const auto* reason = failure_message(failure.reason);
        return std::unexpected(Error{invocation.failed ? invocation.error_code : ErrorCode::invocation_failed,
                                     invocation.failed ? invocation.error
                                     : reason          ? reason
                                                       : "Source execution failed."});
    }
    if (!cleared)
        return std::unexpected(Error{ErrorCode::invocation_failed, "Generated code left execution resources alive."});
    return {};
}

// The caller retains admission until invocation roots are destroyed.
// Published versions provide checked metadata from their own code.
Result<void> execute_invocation(ExecutionScope& scope, Runtime& runtime, const Binding& binding,
                                BusinessFrameLayout layout, bool requires_receiver, ObjectId receiver, void* context,
                                InvocationBody body, BusinessTrace* trace, DebugSessionState* debugger,
                                std::uint32_t* failure_site) {
    Invocation invocation(runtime, binding, layout, trace, debugger, &scope);
    if (trace)
        trace->resources_cleared = false;
    ATELIER_TRY(bind_receiver(invocation, requires_receiver, receiver));
    BusinessFailure failure{};
    const auto status = body(context, invocation, failure);
    return finish_invocation(invocation, status, failure, failure_site);
}

Result<BusinessValue> execute_admitted(ExecutionScope& scope, Runtime& runtime, const Binding& binding,
                                       const BusinessCodeInfo& info, BusinessFrameLayout layout, bool requires_receiver,
                                       ObjectId receiver, std::span<const BusinessValue> arguments, void* result_data,
                                       BusinessTrace* trace, DebugSessionState* debugger, std::uint32_t* failure_site) {
    struct Call {
        const BusinessCodeInfo& info;

        std::span<const BusinessValue> arguments;

        void* result_data;

        std::uint64_t staged_bits{};
    } call{info, arguments, result_data};
    const auto body = [](void* context, Invocation& invocation, BusinessFailure& failure) noexcept {
        auto& call = *static_cast<Call*>(context);
        void* output = call.result_data ? call.result_data : &call.staged_bits;
        if (call.info.diagnostics)
            return reinterpret_cast<BusinessDiagnosticEntry>(call.info.entry_address)(
                &invocation, call.arguments.data(), output, &failure, invocation.trace);
        return reinterpret_cast<BusinessEntry>(call.info.entry_address)(&invocation, call.arguments.data(), output,
                                                                        &failure);
    };
    ATELIER_TRY(execute_invocation(scope, runtime, binding, layout, requires_receiver, receiver, &call, body, trace,
                                   debugger, failure_site));
    if (result_data)
        return BusinessValue{info.signature.result, 0, ArgumentStorage::value, result_data};
    return BusinessValue{info.signature.result, call.staged_bits};
}
}
