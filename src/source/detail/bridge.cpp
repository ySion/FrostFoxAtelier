#include "source/detail/bridge.hpp"

#include <algorithm>
#include <cstdio>
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
    if (type == TypeId{}) {
        Binding binding{};
        binding.runtime = &runtime;
        binding.dependencies = {take(runtime.native_type<float>()), take(runtime.native_type<std::uint32_t>())};
        return binding;
    }
    const auto* info = take(runtime.describe(type));
    require(info->kind == TypeKind::object, "Source receiver must be an object.");
    const auto f32 = take(runtime.native_type<float>());
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    Binding binding{&runtime, type, info->name, {}, {}, {}};
    const auto remember = [&](TypeId dependency) {
        if (std::find(binding.dependencies.begin(), binding.dependencies.end(), dependency) == binding.dependencies.end())
            binding.dependencies.push_back(dependency);
    };
    struct FieldCursor { const TypeInfo* type; std::size_t next{}, depth{}, prefix_length{}; };
    std::vector<FieldCursor> cursors{{info}};
    std::vector<FieldId> path;
    std::string prefix;
    remember(type);
    while (!cursors.empty()) {
        auto& cursor = cursors.back();
        if (cursor.next == cursor.type->fields.size()) { cursors.pop_back(); continue; }
        path.resize(cursor.depth);
        prefix.resize(cursor.prefix_length);
        const auto& field = cursor.type->fields[cursor.next++];
        if (field.value.form != ValueForm::inline_value) continue;
        path.push_back(field.id);
        prefix += field.name;
        if (field.value.type == f32 || field.value.type == u32) {
            binding.fields.push_back({prefix, path, field.value.type == f32 ? ScalarKind::f32 : ScalarKind::u32});
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
        require(signature.receiver && signature.receiver->type == type,
            "Source host method receiver does not match its owning type.");
        const auto kind = [&](ValueDescription value) {
            require(value.form == ValueForm::inline_value && (value.type == f32 || value.type == u32),
                "Source host methods require float/uint values.");
            return value.type == f32 ? ScalarKind::f32 : ScalarKind::u32;
        };
        BoundMethod bound{method.name, method.id, {},
            signature.result ? kind(*signature.result) : ScalarKind::void_value, signature.receiver->read_only};
        for (const auto parameter : signature.parameters) bound.parameters.push_back(kind(parameter));
        binding.methods.push_back(std::move(bound));
    }
    // Stable catalogs are independent of native/descriptor field declaration order.
    std::sort(binding.fields.begin(), binding.fields.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    std::sort(binding.methods.begin(), binding.methods.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    remember(f32);
    remember(u32);
    return binding;
}

EnvironmentView::EnvironmentView(const Binding& binding) {
    require(binding.fields.size() <= std::numeric_limits<std::uint32_t>::max() &&
        binding.methods.size() <= std::numeric_limits<std::uint32_t>::max(), "Source member catalog is too large.");
    for (const auto& field : binding.fields) fields.push_back({field.name.c_str(), field.kind});
    for (const auto& method : binding.methods) {
        require(method.parameters.size() <= std::numeric_limits<std::uint32_t>::max(), "Host signature is too large.");
        methods.push_back({method.name.c_str(), method.parameters.data(), static_cast<std::uint32_t>(method.parameters.size()),
            method.result, method.receiver_read_only});
    }
}
BusinessEnvironment EnvironmentView::view(const char* object_type_name) const {
    return {fields.data(), static_cast<std::uint32_t>(fields.size()), methods.data(),
        static_cast<std::uint32_t>(methods.size()), object_type_name};
}
std::optional<Scalar> scalar_value(BusinessScalar value) {
    if (value.kind == ScalarKind::f32) return std::bit_cast<float>(value.bits);
    if (value.kind == ScalarKind::u32) return value.bits;
    return std::nullopt;
}
AbiArguments::AbiArguments(std::span<const Scalar> arguments)
    : expanded(arguments.size() > inline_values.size() ? arguments.size() : 0),
      values((expanded.empty() ? std::span<BusinessScalar>{inline_values} : std::span<BusinessScalar>{expanded}).first(arguments.size())) {
    for (std::size_t i = 0; i < arguments.size(); ++i)
        values[i] = std::visit([](auto value) { return scalar(value); }, arguments[i]);
}

Invocation::Invocation(Runtime& owner, const Binding& bound, BusinessFrameLayout layout,
    CollectionBudget work, InvocationObserver observation)
    : runtime(owner), binding(bound),
      expanded_roots(layout.strong_slots > business_root_slots ? layout.strong_slots : 0),
      roots(expanded_roots.empty() ? std::span<ObjectRoot>{inline_roots} : std::span<ObjectRoot>{expanded_roots}),
      observations(layout.weak_slots), frame(owner, roots),
      expanded_arguments(layout.max_call_arguments > inline_arguments.size() ? layout.max_call_arguments : 0),
      method_arguments(expanded_arguments.empty() ? std::span<Scalar>{inline_arguments} : std::span<Scalar>{expanded_arguments}),
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

std::span<const FieldId> field_path(const Binding& binding, std::uint32_t field, ScalarKind kind) {
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
        auto value = invocation.runtime.read(*object, field_path(invocation.binding, field, ScalarKind::u32));
        if (!value) return std::unexpected(std::move(value.error()));
        *output = std::get<std::uint32_t>(*value);
        return {};
    });
}
std::uint32_t write_uint(void* context, std::uint32_t slot, std::uint32_t field, std::uint32_t value) noexcept {
    auto& invocation = *static_cast<Invocation*>(context);
    return bridge(invocation, [&]() -> Result<void> {
        auto object = invocation.frame.object(slot);
        if (!object) return std::unexpected(std::move(object.error()));
        return invocation.runtime.write(*object, field_path(invocation.binding, field, ScalarKind::u32), value);
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
            require(arguments[index].kind == target.parameters[index], "Host argument scalar kind is invalid.");
            invocation.method_arguments[index] = arguments[index].kind == ScalarKind::f32 ?
                Scalar{std::bit_cast<float>(arguments[index].bits)} : Scalar{arguments[index].bits};
        }
        auto object = invocation.frame.object(slot);
        if (!object) return std::unexpected(std::move(object.error()));
        Scalar result;
        auto invoked = invocation.runtime.call(*object, target.id, invocation.method_arguments.first(count),
            target.result == ScalarKind::void_value ? nullptr : &result);
        if (!invoked) return invoked;
        if (target.result == ScalarKind::f32) *output = scalar(std::get<float>(result));
        else if (target.result == ScalarKind::u32) *output = scalar(std::get<std::uint32_t>(result));
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
    if (binding.fields.size() != environment.field_count || binding.methods.size() != environment.method_count) return false;
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
    for (std::size_t i = 0; i < arguments.size(); ++i)
        if (arguments[i].kind != signature.parameters[i])
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Source argument kind does not match its signature."});
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
        auto type = runtime.type_of(receiver);
        if (!type) return std::unexpected(std::move(type.error()));
        if (*type != binding.type)
            return std::unexpected(Error{ErrorCode::type_mismatch, "Business receiver has the wrong type."});
    }
    Invocation invocation(runtime, binding, layout, budget, observer);
    if (hold_receiver) {
        auto rooted = invocation.frame.hold(receiver_slot, receiver);
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
