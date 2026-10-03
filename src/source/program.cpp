#include "source/program.hpp"
#include "source/detail/bridge.hpp"
#include "runtime/detail/type_publication.hpp"
#include "language/declarations.hpp"

#include <algorithm>
#include <cstring>

#include <limits>
#include <stdexcept>

namespace atelier::source {
using namespace detail;
namespace {
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
}
namespace {
Result<std::vector<TypeDefinition>> type_definitions(const std::vector<language::TypeDeclaration>& declarations, const Binding& binding) {
    std::vector<TypeDefinition> definitions;
    for (const auto& declaration : declarations) {
        for (const auto& value : binding.values)
            if (value.math && declaration.name == math::short_name(value.math))
                return std::unexpected(Error{ErrorCode::invalid_definition,
                    "Source type declaration conflicts with a builtin math name: " + declaration.name});
        TypeDefinition definition;
        definition.name = declaration.name;
        definition.kind = declaration.enumeration ? TypeKind::enumeration : TypeKind::record;
        if (declaration.enumeration) {
            definition.enumeration = TypeDefinition::Enumeration{
                declaration.signed_enum ? binding.i32 : binding.u32, declaration.enumerators};
        } else {
            for (const auto& [name, type] : declaration.fields) {
                TypeReference reference;
                const bool declared = std::ranges::any_of(declarations, [&](const auto& value) { return value.name == type.name; });
                if (declared && type.array_depth == 0) reference = TypeReference{type.name};
                else {
                    TypeId resolved{};
                    if (type.name == "float") resolved = binding.f32;
                    else if (type.name == "double") resolved = binding.f64;
                    else if (type.name == "uint") resolved = binding.u32;
                    else if (type.name == "int") resolved = binding.i32;
                    for (const auto& value : binding.enumerations) if (value.name == type.name) resolved = value.type;
                    for (const auto& value : binding.values)
                        if (value.name == type.name || (type.name == "String" && value.kind == ValueKind::string) ||
                            (value.math && type.name == math::short_name(value.math))) {
                            resolved = value.type;
                            break;
                        }
                    if (resolved == TypeId{})
                        return std::unexpected(Error{ErrorCode::invalid_definition,
                            "Source struct field type requires a declared or registered value without GC references: " + type.name});
                    for (std::uint32_t depth = 0; depth < type.array_depth; ++depth) {
                        const auto element = value_kind(binding, resolved);
                        const auto array = std::ranges::find_if(binding.values, [&](const auto& value) {
                            return value.kind == ValueKind::array && value.element == element;
                        });
                        if (array == binding.values.end())
                            return std::unexpected(Error{ErrorCode::invalid_definition,
                                "Source struct Array field requires an already registered array for its element: " + type.name});
                        resolved = array->type;
                    }
                    reference = resolved;
                }
                definition.fields.push_back({name, {std::move(reference)}});
            }
        }
        definitions.push_back(std::move(definition));
    }
    return definitions;
}
}
struct Program::Metadata {
    const TypeInfo* identity;
    TypeInfo value;
    bool matches(const TypeInfo& actual) const {
        if (&actual != identity || actual.id != value.id || actual.name != value.name ||
            actual.enumeration != value.enumeration || actual.kind != value.kind || actual.math != value.math || actual.base != value.base || actual.size != value.size || actual.alignment != value.alignment ||
            actual.copy_constructible != value.copy_constructible || actual.copy_assignable != value.copy_assignable ||
            actual.contains_references != value.contains_references || actual.element != value.element ||
            actual.native_storage != value.native_storage || actual.sequence_view != value.sequence_view ||
            actual.sequence_mutate != value.sequence_mutate || actual.value_operations_preserve_borrows != value.value_operations_preserve_borrows || actual.fields.size() != value.fields.size() ||
            actual.methods.size() != value.methods.size()) return false;
        for (std::size_t i = 0; i < value.fields.size(); ++i) {
            const auto& a = actual.fields[i];
            const auto& b = value.fields[i];
            if (a.id != b.id || a.name != b.name || a.value != b.value || a.offset != b.offset) return false;
        }
        for (std::size_t i = 0; i < value.methods.size(); ++i) {
            const auto& a = actual.methods[i];
            const auto& b = value.methods[i];
            if (a.id != b.id || a.name != b.name || a.signature != b.signature || a.preserves_borrows != b.preserves_borrows) return false;
        }
        return true;
    }
};
struct Program::Declaration {
    std::string name;
    FunctionSignature signature;
};
struct Program::Contract {
    Binding binding;
    std::string type_name;
    BusinessServices actual_services;
    std::vector<Metadata> metadata;
    std::vector<language::TypeDeclaration> declarations;
    Contract(Runtime& runtime, TypeId type, std::span<const TypeInfo* const> catalog = {},
        std::vector<language::TypeDeclaration> source_types = {})
        : binding(bind_type(runtime, type, catalog)), type_name(binding.name),
          actual_services(services), declarations(std::move(source_types)) {
        binding.name = type_name;
        for (const auto dependency : binding.dependencies) {
            const TypeInfo* info{};
            if (catalog.empty()) info = take(runtime.describe(dependency));
            else for (const auto* candidate : catalog) if (candidate->id == dependency) { info = candidate; break; }
            if (!info) throw std::logic_error("Source type dependency has no Runtime metadata.");
            metadata.push_back({info, *info});
        }
    }
    std::optional<ValueDescription> value_description(BusinessType kind) const noexcept {
        const auto type = value_type_id(binding, kind);
        if (type != TypeId{}) return ValueDescription{type};
        return std::nullopt;
    }
    Result<FunctionSignature> resolve_signature(const BusinessCodeInfo& info, atelier::detail::TypePublication* publication = nullptr) const {
        const auto reference = [&](TypeId type) -> TypeReference {
            if (publication) {
                for (const auto& item : metadata) if (item.value.id == type) return TypeReference{item.value.name};
            }
            return type;
        };
        SignatureDefinition definition;
        definition.execution = info.signature.is_coroutine ? FunctionExecution::coroutine : FunctionExecution::synchronous;
        definition.parameters.reserve(info.signature.parameter_count);
        for (std::uint32_t i = 0; i < info.signature.parameter_count; ++i) {
            const auto value = value_description(info.signature.parameters[i].type);
            if (!value)
                return std::unexpected(Error{ErrorCode::invalid_definition, "Unsupported compiled source parameter."});
            definition.parameters.push_back({{reference(value->type), value->form}, info.signature.parameters[i].passing});
        }
        if (info.signature.result != ValueKind::void_value) {
            const auto value = value_description(info.signature.result);
            if (!value)
                return std::unexpected(Error{ErrorCode::invalid_definition, "Unsupported compiled source result."});
            definition.result = ValueDefinition{reference(value->type), value->form};
        }
        if (info.requires_receiver) definition.receiver = ReceiverDefinition{binding.type};
        return publication ? publication->resolve_signature(*binding.runtime, definition) : binding.runtime->resolve_signature(definition);
    }
    Result<void> compatible(BusinessCode* code, std::string_view source_name,
        const Declaration* declaration = nullptr) const {
        if (!code) return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate has no compiled code."});
        const auto info = atelier_business_info(code);
        const auto layout = atelier_business_layout(code);
        if (!info.function_name || !*info.function_name ||
            (declaration && declaration->name != info.function_name))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate renamed the bound source entry."});
        if (!info.source_name || source_name != info.source_name || info.entry_address == 0 ||
            !(info.services == actual_services))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate has a different source or actual service ABI."});
        if (!layout.object_type_name || type_name != layout.object_type_name)
            return std::unexpected(Error{ErrorCode::type_mismatch, "Candidate has a different compiled receiver requirement."});
        const auto result = value_description(info.signature.result);
        if (declaration && (declaration->signature.execution == FunctionExecution::coroutine) != info.signature.is_coroutine)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate changed its execution kind."});
        if ((info.signature.parameter_count != 0 && !info.signature.parameters) ||
            (info.signature.result != ValueKind::void_value && !result) ||
            (declaration && (declaration->signature.parameters.size() != info.signature.parameter_count ||
                declaration->signature.result != result)))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate changed its source parameter/result signature."});
        for (std::uint32_t i = 0; i < info.signature.parameter_count; ++i) {
            const auto value = value_description(info.signature.parameters[i].type);
            if (!value || (declaration && declaration->signature.parameters[i] != ParameterDescription{*value, info.signature.parameters[i].passing}))
                return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate changed its source parameter/result signature."});
        }
        if (declaration && info.requires_receiver && !declaration->signature.receiver)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate added a receiver to a receiverless source declaration."});
        if (!same_environment(binding, info.environment))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate changed its bound member catalog."});
        if (layout.strong_slots < business_root_slots || layout.strong_slots >= observation_slot_bit ||
            layout.weak_slots >= observation_slot_bit)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate has an invalid invocation-slot layout."});
        return {};
    }
    Result<void> unchanged() const {
        std::vector<const TypeInfo*> catalog;
        catalog.reserve(metadata.size());
        for (const auto& dependency : metadata) {
            auto info = binding.runtime->describe(dependency.value.id);
            if (!info) return std::unexpected(std::move(info.error()));
            if (!dependency.matches(**info))
                return std::unexpected(Error{ErrorCode::invalid_definition, "Bound Runtime metadata changed."});
            catalog.push_back(*info);
        }
        // Other producers may append unrelated types. Revalidate this program's
        // captured catalog without changing its compiled type-table indices.
        const auto actual = bind_type(*binding.runtime, binding.type, catalog);
        if (actual.runtime != binding.runtime || actual.type != binding.type || actual.name != type_name ||
            actual.fields != binding.fields || actual.methods != binding.methods || actual.enumerations != binding.enumerations || actual.values != binding.values || actual.dependencies != binding.dependencies)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Bound member identities changed."});
        return {};
    }
};
struct Program::Version {
    std::shared_ptr<const Contract> contract;
    std::shared_ptr<const Declaration> declaration;
    std::uint64_t sequence;
    CodeOwner code;
    BusinessCodeInfo info;
    BusinessFrameLayout layout;
    Version(std::shared_ptr<const Contract> bound, std::shared_ptr<const Declaration> declared,
        std::uint64_t request, CodeOwner owner) noexcept
        : contract(std::move(bound)), declaration(std::move(declared)), sequence(request), code(std::move(owner)),
          info(atelier_business_info(code.get())), layout(atelier_business_layout(code.get())) {}
    template<class T>
    Result<std::optional<Scalar>> invoke_values(ObjectId receiver, std::span<const T> arguments, CollectionBudget budget) const {
        if (aggregate(info.signature.result))
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Math source results require typed value output storage."});
        const AbiArguments encoded(contract->binding, declaration->signature, arguments);
        if (!encoded.status) return std::unexpected(encoded.status.error());
        auto result = invoke(receiver, encoded.values, {}, budget);
        if (!result) return std::unexpected(result.error());
        auto decoded = detail::scalar_value(*result, &contract->binding);
        if (info.signature.result != ValueKind::void_value && !decoded)
            return std::unexpected(Error{ErrorCode::invocation_failed, "Compiled source returned an invalid scalar value."});
        return decoded;
    }
    template<class T>
    Result<void> invoke_values(ObjectId receiver, std::span<const T> arguments, ValueOutput output, CollectionBudget budget) const {
        const auto kind = info.signature.result;
        const auto& binding = contract->binding;
        if (!aggregate(kind) || kind.type_index >= binding.values.size() || !binding.values[kind.type_index].math)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Typed value output requires a builtin math source result."});
        const auto& value = binding.values[kind.type_index];
        if (output.type != value.type || !output.data || reinterpret_cast<std::uintptr_t>(output.data) % value.alignment != 0)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Source output storage does not match its declared math type and alignment."});
        alignas(double) std::array<std::byte, 16 * sizeof(double)> staged{};
        if (value.size > staged.size() || value.alignment > alignof(double))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Builtin math layout exceeds the source result ABI."});
        const AbiArguments encoded(binding, declaration->signature, arguments);
        if (!encoded.status) return std::unexpected(encoded.status.error());
        auto result = invoke(receiver, encoded.values, {}, budget, staged.data());
        if (!result) return std::unexpected(result.error());
        if (result->kind != kind || result->storage != ArgumentStorage::value || result->data != staged.data())
            return std::unexpected(Error{ErrorCode::invocation_failed, "Compiled math result changed its typed output storage."});
        std::memcpy(output.data, staged.data(), value.size);
        return {};
    }
    Result<BusinessValue> invoke(ObjectId receiver, std::span<const BusinessValue> arguments,
        InvocationObserver observer, CollectionBudget budget, void* result_data = nullptr) const {
        if (info.signature.is_coroutine)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Coroutine code requires a persistent execution instance."});
        if (aggregate(info.signature.result) && !result_data)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Math source results require typed value output storage."});
        const auto& binding = contract->binding;
        auto activation = ExecutionScope::enter(*binding.runtime);
        if (!activation) return std::unexpected(std::move(activation.error()));
        auto checked = check_source_arguments(binding, info.signature, arguments);
        if (!checked) return std::unexpected(std::move(checked.error()));
        return execute_admitted(*activation, *binding.runtime, binding, code.get(), layout,
            declaration->signature.receiver.has_value(), receiver, arguments, false, observer, budget, result_data);
    }
};
struct Program::State {
    std::shared_ptr<const Contract> contract;
    std::shared_ptr<const Declaration> declaration;
    std::shared_ptr<const Version> current;
    std::uint64_t latest_request{}, published_request{};
    bool open{true};
    explicit State(std::shared_ptr<const Contract> bound) : contract(std::move(bound)) {}
};
struct Program::Frame final : CoroutineFrame {
    std::shared_ptr<const Version> version;
    Invocation invocation;
    std::unique_ptr<BusinessCoroutine, decltype(&atelier_destroy_business_coroutine)> code_frame{
        nullptr, &atelier_destroy_business_coroutine};
    bool observed_terminal{};
    Frame(std::shared_ptr<const Version> owner, InvocationObserver observer, CollectionBudget budget)
        : version(std::move(owner)), invocation(*version->contract->binding.runtime, version->contract->binding,
            version->layout, budget, observer) {}
    ~Frame() override {
        // Generated destruction can call host cleanup, so retain Invocation and
        // Version until the DLL frame has finished using them.
        code_frame.reset();
        complete(false);
    }
    void complete(bool success) noexcept {
        if (observed_terminal) return;
        observed_terminal = true;
        const auto observer = invocation.observer;
        if (observer.complete) observer.complete(observer.context, invocation, success);
    }
    Result<CoroutineStep> resume() override {
        char diagnostic[512];
        bool suspended{};
        BusinessValue output;
        const auto status = atelier_resume_business_coroutine(code_frame.get(), &suspended, &output,
            diagnostic, sizeof(diagnostic));
        if (status != 0 || !suspended || invocation.failed) {
            const bool cleared = invocation.resources_cleared();
            complete(cleared && status == 0 && !invocation.failed);
            if (!cleared)
                return std::unexpected(Error{ErrorCode::invocation_failed, "Coroutine left execution resources alive."});
        }
        if (status != 0 || invocation.failed)
            return std::unexpected(Error{invocation.failed ? invocation.error_code : ErrorCode::invocation_failed,
                invocation.failed ? invocation.error : diagnostic});
        if (suspended) return CoroutineStep{CoroutineState::suspended};
        return CoroutineStep{CoroutineState::completed, scalar_value(output, &version->contract->binding)};
    }
};
struct Program::Prepared {
    std::weak_ptr<State> owner;
    std::shared_ptr<const Contract> contract;
    std::shared_ptr<const Contract> compilation_contract;
    std::shared_ptr<atelier::detail::TypePublication> type_publication;
    std::uint64_t sequence;
    CodeOwner code;
    std::string source_name;
};

Result<Coroutine> ProgramAccess::start_version(std::shared_ptr<const Program::Version> version, ObjectId receiver,
    std::span<const BusinessValue> arguments, InvocationObserver observer, CollectionBudget budget) {
    if (!version || !version->info.signature.is_coroutine)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Starting a coroutine requires a coroutine declaration."});
    const auto& binding = version->contract->binding;
    auto activation = ExecutionScope::enter(*binding.runtime);
    if (!activation) return std::unexpected(std::move(activation.error()));
    auto checked = check_source_arguments(binding, version->info.signature, arguments);
    if (!checked) return std::unexpected(std::move(checked.error()));
    const bool hold_receiver = version->declaration->signature.receiver.has_value() || receiver != ObjectId{};
    if (hold_receiver) {
        auto checked_receiver = check_source_receiver(binding, receiver);
        if (!checked_receiver) return std::unexpected(std::move(checked_receiver.error()));
    }
    auto frame = std::make_unique<Program::Frame>(std::move(version), observer, budget);
    if (hold_receiver) {
        auto held = frame->invocation.frame.hold(receiver_slot, receiver, binding.type);
        if (!held) return std::unexpected(std::move(held.error()));
    }
    BusinessCoroutine* compiled{};
    char diagnostic[512]{};
    if (atelier_create_business_coroutine(frame->version->code.get(), &frame->invocation,
            arguments.data(), static_cast<std::uint32_t>(arguments.size()), &compiled,
            diagnostic, sizeof(diagnostic)) != 0)
        return std::unexpected(Error{ErrorCode::invocation_failed, diagnostic});
    frame->code_frame.reset(compiled);
    return Coroutine::create(*binding.runtime, std::move(frame));
}

Program::SourceRequest::SourceRequest(std::weak_ptr<State> owner, std::shared_ptr<const Contract> contract,
    std::shared_ptr<const Declaration> declaration, std::uint64_t number, std::string name, std::string text, std::string entry)
    : owner_(std::move(owner)), contract_(std::move(contract)), compilation_contract_(contract_), declaration_(std::move(declaration)),
      sequence(number), source_name(std::move(name)), source(std::move(text)), entry_name(std::move(entry)) {}
Program::Candidate::Candidate() = default;
Program::Candidate::Candidate(std::unique_ptr<Prepared> prepared) : prepared_(std::move(prepared)) {}
Program::Candidate::~Candidate() = default;
Program::Candidate::Candidate(Candidate&&) noexcept = default;
Program::Candidate& Program::Candidate::operator=(Candidate&&) noexcept = default;
Program::Candidate::operator bool() const noexcept { return prepared_ && prepared_->code; }
std::uintptr_t Program::Candidate::address() const noexcept {
    return *this ? atelier_business_info(prepared_->code.get()).entry_address : 0;
}
Program::Lease::Lease(std::shared_ptr<const Version> version) : version_(std::move(version)) {}
Program::Entry::Entry(std::weak_ptr<State> owner) : owner_(std::move(owner)) {}
std::uintptr_t Program::Lease::address() const noexcept { return version_ ? version_->info.entry_address : 0; }

Result<BusinessValue> ProgramAccess::invoke(const Program::Lease& lease, ObjectId receiver,
    std::span<const BusinessValue> arguments, InvocationObserver observer, CollectionBudget budget) {
    const auto version = lease.version_;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    // Callback destruction of the lease is safe: only the local pin follows.
    return version->invoke(receiver, arguments, observer, budget);
}
Result<BusinessValue> ProgramAccess::invoke(const Program::Entry& entry, ObjectId receiver,
    std::span<const BusinessValue> arguments, InvocationObserver observer, CollectionBudget budget) {
    const auto owner = entry.owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    const auto version = owner->current;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no published version."});
    // Select once and never access the entry or program after callbacks start.
    return version->invoke(receiver, arguments, observer, budget);
}
Result<Coroutine> ProgramAccess::start(const Program::Lease& lease, ObjectId receiver,
    std::span<const BusinessValue> arguments, InvocationObserver observer, CollectionBudget budget) {
    return start_version(lease.version_, receiver, arguments, observer, budget);
}
Result<Coroutine> ProgramAccess::start(const Program::Entry& entry, ObjectId receiver,
    std::span<const BusinessValue> arguments, InvocationObserver observer, CollectionBudget budget) {
    const auto owner = entry.owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    return start_version(owner->current, receiver, arguments, observer, budget);
}
BusinessFrameLayout ProgramAccess::layout(const Program::Lease& lease) noexcept {
    return lease.version_ ? lease.version_->layout : BusinessFrameLayout{};
}
Result<std::optional<Scalar>> Program::Entry::invoke(ObjectId receiver, std::span<const Scalar> arguments,
    CollectionBudget budget) const {
    const auto owner = owner_.lock();
    if (!owner || !owner->open || !owner->current)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no published version."});
    const auto version = owner->current;
    return version->invoke_values(receiver, arguments, budget);
}
Result<std::optional<Scalar>> Program::Entry::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
    CollectionBudget budget) const {
    const auto owner = owner_.lock();
    if (!owner || !owner->open || !owner->current)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no published version."});
    const auto version = owner->current;
    return version->invoke_values(receiver, arguments, budget);
}
Result<std::optional<Scalar>> Program::Lease::invoke(ObjectId receiver, std::span<const Scalar> arguments,
    CollectionBudget budget) const {
    const auto version = version_;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    return version->invoke_values(receiver, arguments, budget);
}
Result<std::optional<Scalar>> Program::Lease::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
    CollectionBudget budget) const {
    const auto version = version_;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    return version->invoke_values(receiver, arguments, budget);
}
Result<void> Program::Entry::invoke(ObjectId receiver, std::span<const Scalar> arguments,
    ValueOutput output, CollectionBudget budget) const {
    const auto owner = owner_.lock();
    if (!owner || !owner->open || !owner->current)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no published version."});
    const auto version = owner->current;
    return version->invoke_values(receiver, arguments, output, budget);
}
Result<void> Program::Entry::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
    ValueOutput output, CollectionBudget budget) const {
    const auto owner = owner_.lock();
    if (!owner || !owner->open || !owner->current)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no published version."});
    const auto version = owner->current;
    return version->invoke_values(receiver, arguments, output, budget);
}
Result<void> Program::Lease::invoke(ObjectId receiver, std::span<const Scalar> arguments,
    ValueOutput output, CollectionBudget budget) const {
    const auto version = version_;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    return version->invoke_values(receiver, arguments, output, budget);
}
Result<void> Program::Lease::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
    ValueOutput output, CollectionBudget budget) const {
    const auto version = version_;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    return version->invoke_values(receiver, arguments, output, budget);
}
Result<Coroutine> Program::Entry::start(ObjectId receiver, std::span<const Scalar> arguments,
    CollectionBudget budget) const {
    const auto owner = owner_.lock();
    if (!owner || !owner->open || !owner->current)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no published version."});
    const auto version = owner->current;
    const AbiArguments encoded(version->contract->binding, version->declaration->signature, arguments);
    if (!encoded.status) return std::unexpected(encoded.status.error());
    return ProgramAccess::start(*this, receiver, encoded.values, {}, budget);
}
Result<Coroutine> Program::Lease::start(ObjectId receiver, std::span<const Scalar> arguments,
    CollectionBudget budget) const {
    const auto version = version_;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    const AbiArguments encoded(version->contract->binding, version->declaration->signature, arguments);
    if (!encoded.status) return std::unexpected(encoded.status.error());
    return ProgramAccess::start(*this, receiver, encoded.values, {}, budget);
}
Program::Program(Runtime& runtime, TypeId type)
    : state_(std::make_shared<State>(std::make_shared<const Contract>(runtime, type))) {}
Program::~Program() { close(); }
void Program::close() noexcept { state_->open = false; state_->current.reset(); }
Program::Entry Program::entry() const { return Entry{state_}; }
Result<FunctionSignature> Program::describe() const {
    if (!state_->open || !state_->declaration)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business source declaration has not been published."});
    return state_->declaration->signature;
}
Result<Program::Entry> Program::bind_signature(SignatureDefinition expected) const {
    const auto owner = state_;
    if (!owner->open || !owner->declaration)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business source declaration has not been published."});
    if (const auto& receiver = owner->declaration->signature.receiver)
        expected.receiver = ReceiverDefinition{receiver->type, receiver->read_only};
    auto resolved = owner->contract->binding.runtime->resolve_signature(expected);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    if (*resolved != owner->declaration->signature)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Host binding does not match the published source signature."});
    return Entry{owner};
}
Result<Program::Lease> Program::pin() const {
    if (!state_->open || !state_->current)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no version to pin."});
    return Lease{state_->current};
}
Result<Program::Request> Program::request(std::string_view name, std::string_view source, std::string_view entry_name) {
    if (!state_->open) return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    if (state_->latest_request == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::capacity_exceeded, "Business request sequence exhausted."});
    if (source.size() > std::numeric_limits<std::uint32_t>::max() || name.find('\0') != std::string_view::npos || entry_name.find('\0') != std::string_view::npos)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business source/name exceeds its compiler contract."});
    if (entry_name.empty() && state_->declaration) entry_name = state_->declaration->name;
    const auto sequence = state_->latest_request + 1;
    auto result = std::shared_ptr<SourceRequest>{new SourceRequest(state_, state_->contract, state_->declaration, sequence,
        std::string(name) + "[request#" + std::to_string(sequence) + "]", std::string(source), std::string(entry_name))};
    state_->latest_request = sequence;
    const auto report = [&](std::string message, language::SourcePosition position = {}) {
        result->declaration_error_ = Error{ErrorCode::invalid_definition, result->source_name + ":" +
            std::to_string(position.line) + ":" + std::to_string(position.column) + ": " + std::move(message)};
    };
    auto declarations = language::collect_declarations(source);
    if (!declarations) { report(declarations.error().message, declarations.error().position); return result; }
    if (state_->declaration) {
        const auto& previous = state_->contract->declarations;
        if (declarations->size() != previous.size() || !std::equal(declarations->begin(), declarations->end(), previous.begin(),
            [](const auto& a, const auto& b) { return a.same_definition(b); }))
            report("Published source type definitions are immutable; restart the Runtime to change them.");
        return result;
    }
    if (declarations->empty()) return result;
    auto definitions = type_definitions(*declarations, state_->contract->binding);
    if (!definitions) { report(definitions.error().message); return result; }
    auto& runtime = *state_->contract->binding.runtime;
    auto publication = atelier::detail::TypePublication::prepare(runtime, std::move(*definitions));
    if (!publication) { report(publication.error().message); return result; }
    result->type_publication_ = *publication;
    result->compilation_contract_ = std::make_shared<const Contract>(runtime, state_->contract->binding.type,
        (*publication)->catalog(), std::move(*declarations));
    return result;
}
Result<Program::Candidate> Program::prepare(const Request& request) {
    return ProgramAccess::prepare(request, &atelier_compile_business);
}
Result<Program::Candidate> ProgramAccess::prepare(const Program::Request& request, Compiler compile) {
    if (!request) return std::unexpected(Error{ErrorCode::invalid_definition, "Source request is empty."});
    if (!compile) return std::unexpected(Error{ErrorCode::invalid_definition, "Source compiler is missing."});
    const auto owner = request->owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source request belongs to a closed program."});
    if (request->declaration_error_) return std::unexpected(*request->declaration_error_);
    BusinessCode* output{};
    char diagnostic[2048];
    const auto& contract = *request->compilation_contract_;
    const EnvironmentView storage(contract.binding);
    const auto environment = storage.view(contract.type_name.c_str());
    const auto status = compile(&contract.actual_services, request->source_name.c_str(), request->source.data(),
        static_cast<std::uint32_t>(request->source.size()), &environment, &output, diagnostic, sizeof(diagnostic), request->entry_name.c_str());
    CodeOwner code{output, &atelier_destroy_business};
    if (status != 0) return std::unexpected(Error{ErrorCode::invalid_definition, diagnostic});
    const auto info = atelier_business_info(code.get());
    if (!request->entry_name.empty() && (!info.function_name || request->entry_name != info.function_name))
        return std::unexpected(Error{ErrorCode::invalid_definition, "Compiler did not produce the selected module entry."});
    auto compatible = contract.compatible(code.get(), request->source_name, request->declaration_.get());
    if (!compatible)
        return std::unexpected(Error{compatible.error().code, request->source_name + ": " + compatible.error().message});
    return Program::Candidate{std::make_unique<Program::Prepared>(request->owner_, request->contract_, request->compilation_contract_, request->type_publication_, request->sequence,
        std::move(code), request->source_name)};
}
Result<void> Program::try_publish(Candidate& candidate) {
    const auto owner = state_;
    if (!owner->open || !candidate)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Program closed or candidate already consumed."});
    auto& prepared = *candidate.prepared_;
    if (prepared.owner.lock() != owner || prepared.contract != owner->contract)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate belongs to another program/Runtime contract."});
    if (prepared.sequence != owner->latest_request || prepared.sequence <= owner->published_request)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate is stale or its request was already published."});
    auto compatible = prepared.compilation_contract->compatible(prepared.code.get(), prepared.source_name, owner->declaration.get());
    if (!compatible) return compatible;
    auto admission = owner->contract->binding.runtime->check_code_publication();
    if (!admission) return admission;
    auto unchanged = owner->contract->unchanged();
    if (!unchanged) return unchanged;
    if (prepared.type_publication) {
        auto ready = prepared.type_publication->reserve_commit(*owner->contract->binding.runtime);
        if (!ready) return ready;
    }
    auto declaration = owner->declaration;
    if (!declaration) {
        const auto info = atelier_business_info(prepared.code.get());
        auto signature = prepared.compilation_contract->resolve_signature(info, prepared.type_publication.get());
        if (!signature) return std::unexpected(std::move(signature.error()));
        declaration = std::make_shared<const Declaration>(Declaration{info.function_name, std::move(*signature)});
    }
    // Allocation precedes the noexcept constructor's transfer of candidate code.
    auto version = std::make_shared<const Version>(prepared.compilation_contract, declaration, prepared.sequence, std::move(prepared.code));
    if (prepared.type_publication) prepared.type_publication->commit(*owner->contract->binding.runtime);
    owner->contract = prepared.compilation_contract;
    owner->declaration = std::move(declaration);
    owner->published_request = prepared.sequence;
    owner->current = std::move(version);
    return {};
}
#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
Result<void> ProgramAccess::compatible(const Program& program, BusinessCode* code, std::string_view source_name) {
    return program.state_->contract->compatible(code, source_name);
}
Result<void> ProgramAccess::unchanged(const Program& program) { return program.state_->contract->unchanged(); }
void ProgramAccess::set_request_sequence(Program& program, std::uint64_t sequence) noexcept {
    program.state_->latest_request = sequence;
}
std::uint64_t ProgramAccess::published_sequence(const Program& program) noexcept { return program.state_->published_request; }
#endif
}
