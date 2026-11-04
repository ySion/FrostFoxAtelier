#include "source/detail/bridge.hpp"
#include "source/detail/diagnostics.hpp"
#include "source/detail/debugger.hpp"
#include "language/declarations.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace atelier::source::detail {
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class T>
T take(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    return std::move(*value);
}
}

Binding bind_type(Runtime& runtime, TypeId type, std::span<const TypeInfo* const> catalog) {
    std::vector<const TypeInfo*> published;
    if (catalog.empty()) {
        for (const auto id : runtime.types())
            published.push_back(take(runtime.describe(id)));
        catalog = published;
    }
    const auto describe = [&](TypeId id) -> const TypeInfo* {
        for (const auto* info : catalog)
            if (info->id == id)
                return info;
        throw std::runtime_error("Source catalog contains an unresolved type identity.");
    };
    Binding binding{};
    binding.runtime = &runtime;
    binding.type = type;
    binding.f32 = take(runtime.native_type<float>());
    binding.f64 = take(runtime.native_type<double>());
    binding.u32 = take(runtime.native_type<std::uint32_t>());
    binding.i32 = take(runtime.native_type<std::int32_t>());
    binding.boolean = take(runtime.native_type<bool>());
    binding.dependencies = {binding.f32, binding.f64, binding.u32, binding.i32, binding.boolean};
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
    for (const auto& enumeration : binding.enumerations)
        binding.dependencies.push_back(enumeration.type);
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
               if (value_kind(binding, *value->element) == ValueKind::void_value)
                   return false;
               append_value(*value, ValueKind::array);
               return true;
           }) != 0) {
    }
    std::sort(binding.values.begin(), binding.values.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    for (auto& value : binding.values) {
        binding.dependencies.push_back(value.type);
        const auto* info = describe(value.type);
        if (info->element) {
            value.element = value_kind(binding, *info->element);
            require(value.element != ValueKind::void_value,
                    "Source array requires a supported native element representation.");
        }
        for (const auto& field : info->fields) {
            if (!field.offset || field.value.form != ValueForm::inline_value)
                continue;
            const auto kind = value_kind(binding, field.value.type);
            if (kind != ValueKind::void_value)
                value.fields.push_back({field.name, kind, *field.offset});
        }
    }
    if (type == TypeId{})
        return binding;
    const auto* info = describe(type);
    require(info->kind == TypeKind::object, "Source receiver must be an object.");
    binding.name = info->name;
    const auto remember = [&](TypeId dependency) {
        if (std::find(binding.dependencies.begin(), binding.dependencies.end(), dependency) ==
            binding.dependencies.end())
            binding.dependencies.push_back(dependency);
    };
    const auto append_base = [&](const TypeInfo* actual, TypeId owner, std::vector<BusinessAddressStep>& access) {
        while (actual->id != owner) {
            require(actual->base.has_value(), "Compiled access does not belong to its receiver.");
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
    remember(type);
    for (auto base = info->base; base;) {
        remember(*base);
        base = describe(*base)->base;
    }
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
        require(field.offset.has_value() || field.native_address, "Compiled field has no storage operation.");
        access.push_back({field.offset ? nullptr : field.native_address, field.offset.value_or(0)});
        path.push_back(field.id);
        prefix += field.name;
        const auto field_kind = value_kind(binding, field.value.type);
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
            remember(nested->id);
            prefix += '.';
            cursors.push_back({nested, 0, path.size(), prefix.size(), constrained, access.size(), range});
        }
    }
    for (const auto& method : info->methods) {
        const auto& signature = method.signature;
        require(signature.receiver.has_value(), "Source host method requires a receiver.");
        // The catalog may contain unpublished producer identities. Follow its
        // validated ancestry without passing preview IDs into live operations.
        const auto* receiver = info;
        while (receiver && receiver->id != signature.receiver->type)
            receiver = receiver->base ? describe(*receiver->base) : nullptr;
        require(receiver != nullptr, "Source host method receiver does not match its owning type.");
        // As with fields, methods whose values have no source projection stay
        // native-only without preventing unrelated source functions from binding.
        const auto result = signature.result ? value_kind(binding, signature.result->type) : ValueKind::void_value;
        if ((signature.result && result == ValueKind::void_value) ||
            std::any_of(signature.parameters.begin(), signature.parameters.end(), [&](const auto& parameter) {
                return value_kind(binding, parameter.type) == ValueKind::void_value;
            }))
            continue;
        BoundMethod bound{
            method.name,        method.id, {}, result, signature.receiver->read_only, method.preserves_borrows,
            method.native_entry};
        append_base(info, signature.receiver->type, bound.receiver_access);
        for (const auto parameter : signature.parameters)
            bound.parameters.push_back({value_kind(binding, parameter.type), parameter.passing});
        binding.methods.push_back(std::move(bound));
    }
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

EnvironmentView::EnvironmentView(const Binding& binding) {
    require(binding.fields.size() <= std::numeric_limits<std::uint32_t>::max() &&
                binding.methods.size() <= std::numeric_limits<std::uint32_t>::max(),
            "Source member catalog is too large.");
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
        require(method.parameters.size() <= std::numeric_limits<std::uint32_t>::max(), "Host signature is too large.");
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

AbiArguments::AbiArguments(const Binding& binding, const FunctionSignature& signature,
                           std::span<const Scalar> arguments) {
    std::array<Argument, 8> local;
    std::vector<Argument> extra(arguments.size() > local.size() ? arguments.size() : 0);
    auto input = (extra.empty() ? std::span<Argument>{local} : std::span<Argument>{extra}).first(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i)
        input[i] = Argument{arguments[i]};
    initialize(binding, signature, input);
}

AbiArguments::AbiArguments(const Binding& binding, const FunctionSignature& signature,
                           std::span<const Argument> arguments) {
    initialize(binding, signature, arguments);
}

void AbiArguments::initialize(const Binding& binding, const FunctionSignature& signature,
                              std::span<const Argument> arguments) {
    status = binding.runtime->prepare_arguments(signature, arguments, prepared);
    if (!status)
        return;
    if (arguments.size() > inline_values.size())
        expanded.resize(arguments.size());
    values = (expanded.empty() ? std::span<BusinessValue>{inline_values} : std::span<BusinessValue>{expanded})
                 .first(arguments.size());
    const auto views = prepared.values();
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = {};
        values[i].kind = value_kind(binding, views[i].type);
        if (signature.parameters[i].passing == ParameterPassing::value) {
            if (aggregate(values[i].kind))
                values[i].data = views[i].data;
            else if (values[i].kind == ValueKind::boolean)
                values[i].bits = *static_cast<const bool*>(views[i].data) ? 1u : 0u;
            else
                std::memcpy(&values[i].bits, views[i].data, values[i].kind == ValueKind::f64 ? 8 : 4);
        } else {
            values[i].data = views[i].data;
            values[i].storage = signature.parameters[i].passing == ParameterPassing::reference
                                    ? ArgumentStorage::reference
                                    : ArgumentStorage::read_only;
        }
    }
}

Invocation::Invocation(Runtime& owner, const Binding& bound, BusinessFrameLayout layout, InvocationObserver observation,
                       BusinessTrace* capture, std::shared_ptr<DebugSessionState> session, ExecutionScope* scope)
    : runtime(owner), binding(bound),
      expanded_roots(!scope && layout.strong_slots > business_root_slots ? layout.strong_slots : 0),
      roots(scope                    ? std::span<ObjectRoot>{}
            : expanded_roots.empty() ? std::span<ObjectRoot>{inline_roots}
                                     : std::span<ObjectRoot>{expanded_roots}),
      expanded_references(scope && layout.strong_slots > business_root_slots ? layout.strong_slots : 0),
      observations(layout.weak_slots),
      frame(scope
                ? ExecutionFrame(*scope, expanded_references.empty() ? std::span<ObjectReference>{inline_references}
                                                                     : std::span<ObjectReference>{expanded_references})
                : ExecutionFrame(owner, roots)),
      expanded_native(layout.strong_slots + layout.weak_slots > 1 ? layout.strong_slots + layout.weak_slots : 0),
      native(expanded_native.empty() ? std::span<NativeContext>{&inline_native, 1}
                                     : std::span<NativeContext>{expanded_native}),
      observer(observation), trace(capture), debugger(std::move(session)) {}

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

std::uint32_t begin(void* context) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        if (invocation.observer.begin) {
            invocation.invalidate_native();
            return invocation.observer.begin(invocation.observer.context, invocation);
        }
        return {};
    });
}

std::uint32_t poll(void* context) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        if (invocation.observer.poll) {
            invocation.invalidate_native();
            return invocation.observer.poll(invocation.observer.context, invocation);
        }
        return {};
    });
}

std::uint32_t finish(void* context) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        if (invocation.observer.finish) {
            invocation.invalidate_native();
            return invocation.observer.finish(invocation.observer.context, invocation);
        }
        return {};
    });
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
        case ReferenceOperation::try_acquire: {
            if (weak || value->description().form != ValueForm::weak_reference)
                return std::unexpected(
                    Error{ErrorCode::type_mismatch, "Acquisition requires an observation reference."});
            ATELIER_TRY_BIND(acquired, invocation.runtime.try_acquire(*value));
            if (*acquired) {
                if (invocation.roots.empty())
                    return invocation.frame.hold(index, (**acquired).reference());
                invocation.roots[index] = std::move(**acquired);
                return {};
            }
            value = invocation.runtime.null_reference(invocation.binding.type, ValueForm::strong_reference);
            break;
        }
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

const BoundValue& owned_value(Invocation& invocation, std::uint32_t type) {
    require(type < invocation.binding.values.size() && invocation.binding.values[type].can_own,
            "Native value lacks a borrow-preserving lifecycle contract.");
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
        require(invocation.live_values != 0, "Value cleanup has no live construction.");
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
            source_size = take(invocation.runtime.describe(source_type))->size;
        } else if (operation == SequenceMutation::assign_text || operation == SequenceMutation::append_text)
            source_size = count;
        require(source || source_size == 0, "Native sequence input is null.");
        return invocation.runtime.mutate_sequence(value.type, {static_cast<std::byte*>(target), value.size}, operation,
                                                  source_type, {static_cast<const std::byte*>(source), source_size},
                                                  count);
    });
}

const BusinessServices services{
    begin,          create_slot,     poll,         finish,        clear_slot,      change_reference,
    same_reference, construct_value, assign_value, destroy_value, mutate_sequence, debug_enter,
    debug_point,    debug_leave,     nullptr,      nullptr,       native_receiver, native_stamp,
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

Result<void> check_source_arguments(const Binding& binding, const BusinessSignature& signature,
                                    std::span<const BusinessValue> arguments) {
    if (arguments.size() != signature.parameter_count)
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Source argument count does not match its signature."});
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        const auto mode = signature.parameters[i].passing;
        const BoundValue* value = aggregate(argument.kind) && argument.kind.type_index < binding.values.size()
                                      ? &binding.values[argument.kind.type_index]
                                      : nullptr;
        const auto alignment = value                                 ? value->alignment
                               : argument.kind == ValueKind::boolean ? 1u
                               : argument.kind == ValueKind::f64     ? 8u
                                                                     : 4u;
        const bool has_storage = argument.data && reinterpret_cast<std::uintptr_t>(argument.data) % alignment == 0;
        const bool valid =
            mode == ParameterPassing::value
                ? argument.storage == ArgumentStorage::value &&
                      (!aggregate(argument.kind) || (value && (value->math || value->plain_value) && has_storage))
                : has_storage &&
                      (argument.storage == ArgumentStorage::reference ||
                       (mode == ParameterPassing::read_only && argument.storage == ArgumentStorage::read_only));
        if (argument.kind != signature.parameters[i].type || !valid ||
            (mode == ParameterPassing::value && argument.kind == ValueKind::boolean && argument.bits > 1) ||
            (mode == ParameterPassing::value && !aggregate(argument.kind) && argument.kind != ValueKind::f64 &&
             argument.bits > std::numeric_limits<std::uint32_t>::max()))
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Source argument type or storage does not match its signature."});
    }
    return {};
}

Result<void> check_source_receiver(const Binding& binding, ObjectId receiver) {
    ATELIER_TRY_BIND(actual, binding.runtime->type_of(receiver));
    if (binding.type == TypeId{})
        return std::unexpected(Error{ErrorCode::type_mismatch, "Receiverless source cannot accept an object."});
    ATELIER_TRY_BIND(compatible, binding.runtime->is_a(*actual, binding.type));
    if (!*compatible)
        return std::unexpected(Error{ErrorCode::type_mismatch, "Source receiver has the wrong type."});
    return {};
}

// The caller retains admission until invocation roots and observer effects are
// destroyed. Published versions provide checked metadata from their own code.
Result<void> execute_invocation(ExecutionScope& scope, Runtime& runtime, const Binding& binding,
                                BusinessFrameLayout layout, bool requires_receiver, ObjectId receiver, void* context,
                                InvocationBody body, InvocationObserver observer, BusinessTrace* trace,
                                std::shared_ptr<DebugSessionState> debugger, std::uint32_t* failure_site) {
    const bool hold_receiver = requires_receiver || receiver != ObjectId{};
    if (hold_receiver) {
        ATELIER_TRY(check_source_receiver(binding, receiver));
    }
    Invocation invocation(runtime, binding, layout, observer, trace, std::move(debugger), &scope);
    if (trace)
        trace->resources_cleared = false;
    if (hold_receiver) {
        ATELIER_TRY(invocation.frame.hold(receiver_slot, receiver, binding.type));
    }
    char backend_error[512]{};
    BusinessFailure failure{};
    const auto status = body(context, invocation, failure, backend_error, sizeof(backend_error));
    // Inspect generated cleanup before the RAII fallback. Observers cannot
    // manufacture success or substitute for this production invariant.
    const bool cleared = invocation.resources_cleared();
    invocation.record_resources(cleared);
    if (observer.complete)
        observer.complete(observer.context, invocation, cleared && status == 0 && !invocation.failed);
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
                                                       : backend_error});
    }
    if (!cleared)
        return std::unexpected(Error{ErrorCode::invocation_failed, "Generated code left execution resources alive."});
    return {};
}

Result<BusinessValue> execute_admitted(ExecutionScope& scope, Runtime& runtime, const Binding& binding,
                                       BusinessCode* code, BusinessFrameLayout layout, bool requires_receiver,
                                       ObjectId receiver, std::span<const BusinessValue> arguments,
                                       InvocationObserver observer, void* result_data, BusinessTrace* trace,
                                       std::shared_ptr<DebugSessionState> debugger, std::uint32_t* failure_site) {
    struct Call {
        BusinessCode* code;

        std::span<const BusinessValue> arguments;

        BusinessValue value;
    } call{code, arguments, {}};
    call.value.data = result_data;
    const auto body = [](void* context, Invocation& invocation, BusinessFailure& failure, char* error,
                         std::uint32_t capacity) noexcept {
        auto& call = *static_cast<Call*>(context);
        return atelier_invoke_business(call.code, &invocation, call.arguments.data(),
                                       static_cast<std::uint32_t>(call.arguments.size()), &call.value, error, capacity,
                                       invocation.trace, &failure.location);
    };
    ATELIER_TRY(execute_invocation(scope, runtime, binding, layout, requires_receiver, receiver, &call, body, observer,
                                   trace, std::move(debugger), failure_site));
    return call.value;
}

Result<BusinessValue> execute_typed(Runtime& runtime, const Binding& binding, BusinessCode* code, ObjectId receiver,
                                    std::span<const BusinessValue> arguments, InvocationObserver observer) {
    ATELIER_TRY_BIND(activation, ExecutionScope::enter(runtime));
    if (binding.runtime != &runtime)
        return std::unexpected(Error{ErrorCode::invalid_type, "Business binding belongs to another Runtime."});
    if (!code)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business code has not been compiled."});
    const auto info = atelier_business_info(code);
    if (info.signature.is_coroutine)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Coroutine code requires a persistent execution instance."});
    if (aggregate(info.signature.result))
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Math source results require typed value output storage."});
    ATELIER_TRY(check_source_arguments(binding, info.signature, arguments));
    if (!same_environment(binding, info.environment))
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Compiled member catalog does not match the host binding."});
    const auto layout = atelier_business_layout(code);
    if (layout.strong_slots < business_root_slots || layout.strong_slots >= observation_slot_bit ||
        layout.weak_slots >= observation_slot_bit)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business code has an invalid reference layout."});
    if (layout.object_type_name && *layout.object_type_name && binding.name != layout.object_type_name)
        return std::unexpected(
            Error{ErrorCode::type_mismatch, "Business code was compiled for a different object type."});
    return execute_admitted(*activation, runtime, binding, code, layout, info.requires_receiver, receiver, arguments,
                            observer);
}
}
