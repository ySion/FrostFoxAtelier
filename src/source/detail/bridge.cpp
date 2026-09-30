#include "source/detail/bridge.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace atelier::source::detail {
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
}

Binding bind_type(Runtime& runtime, TypeId type) {
    Binding binding{};
    binding.runtime = &runtime;
    binding.type = type;
    binding.f32 = take(runtime.native_type<float>());
    binding.u32 = take(runtime.native_type<std::uint32_t>());
    binding.i32 = take(runtime.native_type<std::int32_t>());
    binding.dependencies = {binding.f32, binding.u32, binding.i32};
    for (const auto id : runtime.types()) {
        const auto* enumeration = take(runtime.describe(id));
        if (!enumeration->enumeration) continue;
        binding.enumerations.push_back({id, enumeration->name,
            enumeration->enumeration->underlying == binding.i32 ? ScalarKind::i32 : ScalarKind::u32,
            enumeration->enumeration->values});
    }
    std::sort(binding.enumerations.begin(), binding.enumerations.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    for (const auto& enumeration : binding.enumerations) binding.dependencies.push_back(enumeration.type);
    if (type == TypeId{}) return binding;
    const auto* info = take(runtime.describe(type));
    require(info->kind == TypeKind::object, "Source receiver must be an object.");
    binding.name = info->name;
    const auto remember = [&](TypeId dependency) {
        if (std::find(binding.dependencies.begin(), binding.dependencies.end(), dependency) == binding.dependencies.end())
            binding.dependencies.push_back(dependency);
    };
    struct FieldCursor { const TypeInfo* type; std::size_t next{}, depth{}, prefix_length{}; };
    std::vector<FieldCursor> cursors{{info}};
    std::vector<FieldId> path;
    std::string prefix;
    remember(type);
    for (auto base = info->base; base; ) {
        remember(*base);
        base = take(runtime.describe(*base))->base;
    }
    while (!cursors.empty()) {
        auto& cursor = cursors.back();
        if (cursor.next == cursor.type->fields.size()) { cursors.pop_back(); continue; }
        path.resize(cursor.depth);
        prefix.resize(cursor.prefix_length);
        const auto& field = cursor.type->fields[cursor.next++];
        if (field.value.form != ValueForm::inline_value) continue;
        path.push_back(field.id);
        prefix += field.name;
        const auto field_kind = scalar_kind(binding, field.value.type);
        if (field_kind != ScalarKind::void_value) {
            binding.fields.push_back({prefix, path, field_kind});
        } else {
            const auto* nested = take(runtime.describe(field.value.type));
            if (nested->kind != TypeKind::record) continue;
            remember(nested->id);
            prefix += '.';
            cursors.push_back({nested, 0, path.size(), prefix.size()});
        }
    }
    for (const auto& method : info->methods) {
        const auto& signature = method.signature;
        require(signature.receiver && take(runtime.is_a(type, signature.receiver->type)),
            "Source host method receiver does not match its owning type.");
        const auto kind = [&](ValueDescription value) {
            const auto result = scalar_kind(binding, value.type);
            require(value.form == ValueForm::inline_value && result != ScalarKind::void_value,
                "Source host methods require supported scalar or enum values.");
            return result;
        };
        BoundMethod bound{method.name, method.id, {},
            signature.result ? kind(*signature.result) : ScalarKind::void_value, signature.receiver->read_only};
        for (const auto parameter : signature.parameters) bound.parameters.push_back({kind(parameter), parameter.passing});
        binding.methods.push_back(std::move(bound));
    }
    // Stable catalogs are independent of native/descriptor field declaration order.
    std::sort(binding.fields.begin(), binding.fields.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    std::sort(binding.methods.begin(), binding.methods.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return binding;
}

BusinessType scalar_kind(const Binding& binding, TypeId type) {
    if (type == binding.f32) return ScalarKind::f32;
    if (type == binding.u32) return ScalarKind::u32;
    if (type == binding.i32) return ScalarKind::i32;
    for (std::uint32_t i = 0; i < binding.enumerations.size(); ++i)
        if (type == binding.enumerations[i].type) return {ScalarKind::enumeration, i};
    return ScalarKind::void_value;
}
TypeId scalar_type(const Binding& binding, BusinessType type) {
    if (type == ScalarKind::f32) return binding.f32;
    if (type == ScalarKind::u32) return binding.u32;
    if (type == ScalarKind::i32) return binding.i32;
    if (type.kind == ScalarKind::enumeration && type.enum_index < binding.enumerations.size())
        return binding.enumerations[type.enum_index].type;
    return {};
}
BusinessScalar scalar(const Scalar& value, const Binding& binding) {
    return std::visit([&](const auto& item) -> BusinessScalar {
        using T = std::remove_cvref_t<decltype(item)>;
        if constexpr (std::same_as<T, EnumValue>) return {scalar_kind(binding, item.type), item.bits};
        else return scalar(item);
    }, value);
}

EnvironmentView::EnvironmentView(const Binding& binding) {
    require(binding.fields.size() <= std::numeric_limits<std::uint32_t>::max() &&
        binding.methods.size() <= std::numeric_limits<std::uint32_t>::max(), "Source member catalog is too large.");
    enum_items.reserve(binding.enumerations.size());
    enumerations.reserve(binding.enumerations.size());
    for (const auto& enumeration : binding.enumerations) {
        auto& items = enum_items.emplace_back();
        for (const auto& [name, bits] : enumeration.values) items.push_back({name.c_str(), bits});
        enumerations.push_back({enumeration.name.c_str(), enumeration.underlying, items.data(), static_cast<std::uint32_t>(items.size())});
    }
    for (const auto& field : binding.fields) fields.push_back({field.name.c_str(), field.kind});
    for (const auto& method : binding.methods) {
        require(method.parameters.size() <= std::numeric_limits<std::uint32_t>::max(), "Host signature is too large.");
        methods.push_back({method.name.c_str(), method.parameters.data(), static_cast<std::uint32_t>(method.parameters.size()),
            method.result, method.receiver_read_only});
    }
}
BusinessEnvironment EnvironmentView::view(const char* object_type_name) const {
    return {fields.data(), static_cast<std::uint32_t>(fields.size()), methods.data(),
        static_cast<std::uint32_t>(methods.size()), object_type_name, enumerations.data(), static_cast<std::uint32_t>(enumerations.size())};
}
std::optional<Scalar> scalar_value(BusinessScalar value, const Binding* binding) {
    if (value.kind == ScalarKind::f32) return std::bit_cast<float>(value.bits);
    if (value.kind == ScalarKind::u32) return value.bits;
    if (value.kind == ScalarKind::i32) return std::bit_cast<std::int32_t>(value.bits);
    if (binding && value.kind.kind == ScalarKind::enumeration && value.kind.enum_index < binding->enumerations.size())
        return EnumValue{binding->enumerations[value.kind.enum_index].type, value.bits};
    return std::nullopt;
}
AbiArguments::AbiArguments(const Binding& binding, const FunctionSignature& signature, std::span<const Scalar> arguments) {
    std::array<Argument, 8> local;
    std::vector<Argument> extra(arguments.size() > local.size() ? arguments.size() : 0);
    auto input = (extra.empty() ? std::span<Argument>{local} : std::span<Argument>{extra}).first(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) input[i] = Argument{arguments[i]};
    initialize(binding, signature, input);
}
AbiArguments::AbiArguments(const Binding& binding, const FunctionSignature& signature, std::span<const Argument> arguments) {
    initialize(binding, signature, arguments);
}
void AbiArguments::initialize(const Binding& binding, const FunctionSignature& signature, std::span<const Argument> arguments) {
    status = binding.runtime->prepare_arguments(signature, arguments, prepared);
    if (!status) return;
    if (arguments.size() > inline_values.size()) expanded.resize(arguments.size());
    values = (expanded.empty() ? std::span<BusinessScalar>{inline_values} : std::span<BusinessScalar>{expanded}).first(arguments.size());
    const auto views = prepared.values();
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = {};
        values[i].kind = scalar_kind(binding, views[i].type);
        if (signature.parameters[i].passing == ParameterPassing::value) std::memcpy(&values[i].bits, views[i].data, 4);
        else {
            values[i].data = views[i].data;
            values[i].storage = signature.parameters[i].passing == ParameterPassing::reference ? ArgumentStorage::reference : ArgumentStorage::read_only;
        }
    }
}

Invocation::Invocation(Runtime& owner, const Binding& bound, BusinessFrameLayout layout,
    CollectionBudget work, InvocationObserver observation)
    : runtime(owner), binding(bound),
      expanded_roots(layout.strong_slots > business_root_slots ? layout.strong_slots : 0),
      roots(expanded_roots.empty() ? std::span<ObjectRoot>{inline_roots} : std::span<ObjectRoot>{expanded_roots}),
      observations(layout.weak_slots), frame(owner, roots),
      expanded_arguments(layout.max_call_arguments > inline_arguments.size() ? layout.max_call_arguments : 0),
      method_arguments(expanded_arguments.empty() ? std::span<Argument>{inline_arguments} : std::span<Argument>{expanded_arguments}),
      budget(work), observer(observation) {}
bool Invocation::references_cleared() const noexcept {
    if (!frame.empty()) return false;
    for (const auto& observation : observations)
        if (observation.description().type != TypeId{}) return false;
    return true;
}
void Invocation::fail(ErrorCode code, const char* message) noexcept {
    if (failed) return;
    failed = true;
    error_code = code;
    std::snprintf(error, sizeof(error), "%s", message);
}

std::uint32_t create_slot(void* context, std::uint32_t slot) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&] { return invocation.frame.create(slot, invocation.binding.type); });
}

std::span<const FieldId> field_path(const Binding& binding, std::uint32_t field, BusinessType kind) {
    require(field < binding.fields.size() && binding.fields[field].kind == kind, "Host field index or scalar kind is invalid.");
    return binding.fields[field].path;
}
std::uint32_t read_float(void* context, std::uint32_t slot, std::uint32_t field, float* output) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        auto object = invocation.frame.object(slot);
        if (!object) return std::unexpected(std::move(object.error()));
        auto value = invocation.runtime.read(*object, field_path(invocation.binding, field, ScalarKind::f32));
        if (!value) return std::unexpected(std::move(value.error()));
        *output = std::get<float>(*value);
        return {};
    });
}
std::uint32_t write_float(void* context, std::uint32_t slot, std::uint32_t field, float value) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        auto object = invocation.frame.object(slot);
        if (!object) return std::unexpected(std::move(object.error()));
        return invocation.runtime.write(*object, field_path(invocation.binding, field, ScalarKind::f32), value);
    });
}
std::uint32_t read_uint(void* context, std::uint32_t slot, std::uint32_t field, std::uint32_t* output) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        auto object = invocation.frame.object(slot);
        if (!object) return std::unexpected(std::move(object.error()));
        require(field < invocation.binding.fields.size() && invocation.binding.fields[field].kind != ScalarKind::f32, "Invalid integer/enum field.");
        auto value = invocation.runtime.read(*object, invocation.binding.fields[field].path);
        if (!value) return std::unexpected(std::move(value.error()));
        *output = scalar(*value, invocation.binding).bits;
        return {};
    });
}
std::uint32_t write_uint(void* context, std::uint32_t slot, std::uint32_t field, std::uint32_t value) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        auto object = invocation.frame.object(slot);
        if (!object) return std::unexpected(std::move(object.error()));
        require(field < invocation.binding.fields.size() && invocation.binding.fields[field].kind != ScalarKind::f32, "Invalid integer/enum field.");
        auto decoded = scalar_value({invocation.binding.fields[field].kind, value}, &invocation.binding);
        require(decoded.has_value(), "Invalid integer/enum value.");
        return invocation.runtime.write(*object, invocation.binding.fields[field].path, *decoded);
    });
}
std::uint32_t call_method(void* context, std::uint32_t slot, std::uint32_t method,
    const BusinessScalar* arguments, std::uint32_t count, BusinessScalar* output) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        require(method < invocation.binding.methods.size(), "Host method index is invalid.");
        const auto& target = invocation.binding.methods[method];
        require(target.result == ScalarKind::void_value || output, "Host method result storage is missing.");
        require(count == target.parameters.size() && count <= invocation.method_arguments.size() &&
            (count == 0 || arguments), "Host argument count or storage is invalid.");
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto& input = arguments[index];
            require(input.kind == target.parameters[index].type, "Host argument type is invalid.");
            if (input.storage == ArgumentStorage::field) {
                require(input.field < invocation.binding.fields.size() && invocation.binding.fields[input.field].kind == input.kind,
                    "Host borrowed field is invalid.");
                auto owner = invocation.frame.object(input.slot);
                if (!owner) return std::unexpected(owner.error());
                invocation.method_arguments[index] = Argument::field(*owner, invocation.binding.fields[input.field].path);
            } else if (input.storage == ArgumentStorage::read_only || input.storage == ArgumentStorage::reference) {
                require(input.data != nullptr, "Host borrowed argument is null.");
                invocation.method_arguments[index] = Argument::borrow(scalar_type(invocation.binding, input.kind), input.data,
                    input.storage == ArgumentStorage::reference ? const_cast<void*>(input.data) : nullptr);
            } else {
                require(input.storage == ArgumentStorage::value, "Invalid argument storage kind.");
                auto decoded = scalar_value(input, &invocation.binding);
                require(decoded.has_value(), "Invalid scalar argument.");
                invocation.method_arguments[index] = Argument{*decoded};
            }
        }
        auto object = invocation.frame.object(slot);
        if (!object) return std::unexpected(std::move(object.error()));
        Scalar result;
        auto invoked = invocation.runtime.call(*object, target.id, invocation.method_arguments.first(count),
            target.result == ScalarKind::void_value ? nullptr : &result);
        if (!invoked) return invoked;
        if (target.result != ScalarKind::void_value) *output = scalar(result, invocation.binding);
        return {};
    });
}

std::uint32_t begin(void* context) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        if (invocation.observer.begin) return invocation.observer.begin(invocation.observer.context, invocation);
        return {};
    });
}
std::uint32_t poll(void* context) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        auto progress = invocation.frame.poll(invocation.budget);
        if (!progress) return std::unexpected(std::move(progress.error()));
        if (invocation.observer.poll)
            return invocation.observer.poll(invocation.observer.context, invocation, *progress);
        return {};
    });
}
std::uint32_t finish(void* context) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        if (invocation.observer.finish) return invocation.observer.finish(invocation.observer.context, invocation);
        return {};
    });
}
Result<ObjectReference> reference_value(Invocation& invocation, std::uint32_t slot) {
    if (slot == null_reference_slot)
        return invocation.runtime.null_reference(invocation.binding.type, ValueForm::strong_reference);
    if ((slot & observation_slot_bit) == 0) return invocation.frame.reference(slot);
    const auto index = slot & ~observation_slot_bit;
    if (index >= invocation.observations.size())
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Observation slot is out of range."});
    const auto value = invocation.observations[index];
    if (value.description() != ValueDescription{invocation.binding.type, ValueForm::weak_reference})
        return std::unexpected(Error{ErrorCode::invalid_object, "Observation slot is not initialized."});
    return value;
}

std::uint32_t change_reference(void* context, ReferenceOperation operation,
    std::uint32_t destination, std::uint32_t source) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        const bool weak = (destination & observation_slot_bit) != 0;
        const auto index = destination & ~observation_slot_bit;
        if (destination == null_reference_slot ||
            (weak ? index >= invocation.observations.size() : index >= invocation.roots.size()))
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Reference destination is out of range."});
        auto value = reference_value(invocation, source);
        if (!value) return std::unexpected(std::move(value.error()));
        if (value->description().type != invocation.binding.type)
            return std::unexpected(Error{ErrorCode::type_mismatch, "Reference does not match the bound object type."});
        const auto form = weak ? ValueForm::weak_reference : ValueForm::strong_reference;
        switch (operation) {
        case ReferenceOperation::copy:
            if (source == null_reference_slot)
                value = invocation.runtime.null_reference(invocation.binding.type, form);
            else if (value->description().form != form)
                return std::unexpected(Error{ErrorCode::type_mismatch, "Reference assignment cannot change holding form."});
            break;
        case ReferenceOperation::observe:
            if (!weak || value->description().form != ValueForm::strong_reference || source == null_reference_slot)
                return std::unexpected(Error{ErrorCode::type_mismatch, "Observation requires a holding reference."});
            value = invocation.runtime.observe(*value);
            break;
        case ReferenceOperation::try_acquire: {
            if (weak || value->description().form != ValueForm::weak_reference)
                return std::unexpected(Error{ErrorCode::type_mismatch, "Acquisition requires an observation reference."});
            auto acquired = invocation.runtime.try_acquire(*value);
            if (!acquired) return std::unexpected(std::move(acquired.error()));
            if (*acquired) {
                invocation.roots[index] = std::move(**acquired);
                return {};
            }
            value = invocation.runtime.null_reference(invocation.binding.type, ValueForm::strong_reference);
            break;
        }
        default:
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Unknown reference operation."});
        }
        if (!value) return std::unexpected(std::move(value.error()));
        if (weak) {
            invocation.observations[index] = *value;
            return {};
        }
        return invocation.frame.hold(index, *value);
    });
}

std::uint32_t same_reference(void* context, std::uint32_t left, std::uint32_t right,
    std::uint32_t* output) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        auto lhs = reference_value(invocation, left);
        if (!lhs) return std::unexpected(std::move(lhs.error()));
        auto rhs = reference_value(invocation, right);
        if (!rhs) return std::unexpected(std::move(rhs.error()));
        if (lhs->description().type != invocation.binding.type || rhs->description().type != invocation.binding.type)
            return std::unexpected(Error{ErrorCode::type_mismatch, "Reference comparison crossed the bound object type."});
        *output = lhs->id() == rhs->id() ? 1u : 0u;
        return {};
    });
}

void clear_slot(void* context, std::uint32_t slot) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    (void)bridge(invocation, [&]() -> Result<void> {
        if ((slot & observation_slot_bit) == 0) return invocation.frame.clear(slot);
        const auto index = slot & ~observation_slot_bit;
        if (index >= invocation.observations.size())
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Observation slot is out of range."});
        invocation.observations[index] = {};
        return {};
    });
}

const BusinessServices services{begin, create_slot, read_float, write_float, read_uint,
    write_uint, call_method, poll, finish, clear_slot, change_reference, same_reference};

bool same_environment(const Binding& binding, const BusinessEnvironment& environment) {
    if (binding.fields.size() != environment.field_count || binding.methods.size() != environment.method_count || binding.enumerations.size() != environment.enumeration_count) return false;
    for (std::size_t i = 0; i < binding.enumerations.size(); ++i) {
        const auto& expected = binding.enumerations[i];
        const auto& actual = environment.enumerations[i];
        if (!actual.name || expected.name != actual.name || expected.underlying != actual.underlying || expected.values.size() != actual.value_count) return false;
        for (std::size_t j = 0; j < expected.values.size(); ++j)
            if (!actual.values[j].name || expected.values[j].first != actual.values[j].name || expected.values[j].second != actual.values[j].bits) return false;
    }
    for (std::size_t i = 0; i < binding.fields.size(); ++i)
        if (binding.fields[i].name != environment.fields[i].name || binding.fields[i].kind != environment.fields[i].kind) return false;
    for (std::size_t i = 0; i < binding.methods.size(); ++i) {
        const auto& actual = environment.methods[i];
        const auto& expected = binding.methods[i];
        if (expected.name != actual.name || expected.parameters.size() != actual.parameter_count ||
            expected.result != actual.result || expected.receiver_read_only != actual.receiver_read_only) return false;
        for (std::size_t j = 0; j < expected.parameters.size(); ++j)
            if (expected.parameters[j] != actual.parameters[j]) return false;
    }
    return true;
}
Result<void> check_source_arguments(const BusinessSignature& signature, std::span<const BusinessScalar> arguments) {
    if (arguments.size() != signature.parameter_count)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Source argument count does not match its signature."});
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        const auto mode = signature.parameters[i].passing;
        const bool valid = mode == ParameterPassing::value ? argument.storage == ArgumentStorage::value :
            argument.data && reinterpret_cast<std::uintptr_t>(argument.data) % 4 == 0 &&
            (argument.storage == ArgumentStorage::reference || (mode == ParameterPassing::read_only && argument.storage == ArgumentStorage::read_only));
        if (argument.kind != signature.parameters[i].type || !valid)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Source argument type or storage does not match its signature."});
    }
    return {};
}

Result<void> check_source_receiver(const Binding& binding, ObjectId receiver) {
    auto actual = binding.runtime->type_of(receiver);
    if (!actual) return std::unexpected(actual.error());
    if (binding.type == TypeId{})
        return std::unexpected(Error{ErrorCode::type_mismatch, "Receiverless source cannot accept an object."});
    auto compatible = binding.runtime->is_a(*actual, binding.type);
    if (!compatible) return std::unexpected(compatible.error());
    if (!*compatible)
        return std::unexpected(Error{ErrorCode::type_mismatch, "Source receiver has the wrong type."});
    return {};
}

// The caller retains admission until invocation roots and observer effects are
// destroyed. Published versions provide checked metadata from their own code.
Result<BusinessScalar> execute_admitted(ExecutionScope&, Runtime& runtime, const Binding& binding,
    BusinessCode* code, BusinessFrameLayout layout, bool requires_receiver, ObjectId receiver,
    std::span<const BusinessScalar> arguments, bool request_collection, InvocationObserver observer,
    CollectionBudget budget) {
    const bool hold_receiver = requires_receiver || receiver != ObjectId{};
    if (hold_receiver) {
        auto checked = check_source_receiver(binding, receiver);
        if (!checked) return std::unexpected(std::move(checked.error()));
    }
    Invocation invocation(runtime, binding, layout, budget, observer);
    if (hold_receiver) {
        auto rooted = invocation.frame.hold(receiver_slot, receiver, binding.type);
        if (!rooted) return std::unexpected(std::move(rooted.error()));
    }
    if (request_collection) invocation.frame.request_collection();
    char backend_error[512];
    BusinessScalar value;
    const auto status = atelier_invoke_business(code, &invocation, arguments.data(),
        static_cast<std::uint32_t>(arguments.size()), &value, backend_error, sizeof(backend_error));
    // Inspect generated cleanup before the RAII fallback. Observers cannot
    // manufacture success or substitute for this production invariant.
    const bool cleared = invocation.references_cleared();
    if (observer.complete) observer.complete(observer.context, invocation, cleared && status == 0 && !invocation.failed);
    if (!cleared)
        return std::unexpected(Error{ErrorCode::invocation_failed, "Generated code left execution roots registered."});
    if (status != 0 || invocation.failed)
        return std::unexpected(Error{invocation.failed ? invocation.error_code : ErrorCode::invocation_failed,
            invocation.failed ? invocation.error : backend_error});
    return value;
}

Result<BusinessScalar> execute_typed(Runtime& runtime, const Binding& binding, BusinessCode* code,
    ObjectId receiver, std::span<const BusinessScalar> arguments, bool request_collection,
    InvocationObserver observer, CollectionBudget budget) {
    auto activation = ExecutionScope::enter(runtime);
    if (!activation) return std::unexpected(std::move(activation.error()));
    if (binding.runtime != &runtime)
        return std::unexpected(Error{ErrorCode::invalid_type, "Business binding belongs to another Runtime."});
    if (!code)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business code has not been compiled."});
    const auto info = atelier_business_info(code);
    if (info.signature.is_coroutine)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Coroutine code requires a persistent execution instance."});
    auto checked = check_source_arguments(info.signature, arguments);
    if (!checked) return std::unexpected(std::move(checked.error()));
    if (!same_environment(binding, info.environment))
        return std::unexpected(Error{ErrorCode::invalid_definition, "Compiled member catalog does not match the host binding."});
    const auto layout = atelier_business_layout(code);
    if (layout.strong_slots < business_root_slots || layout.strong_slots >= observation_slot_bit ||
        layout.weak_slots >= observation_slot_bit)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business code has an invalid reference layout."});
    if (layout.object_type_name && *layout.object_type_name && binding.name != layout.object_type_name)
        return std::unexpected(Error{ErrorCode::type_mismatch, "Business code was compiled for a different object type."});
    return execute_admitted(*activation, runtime, binding, code, layout, info.requires_receiver,
        receiver, arguments, request_collection, observer, budget);
}
}
