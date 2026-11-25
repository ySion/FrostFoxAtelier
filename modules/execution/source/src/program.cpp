#include "source/program.hpp"
#include "source/detail/bridge.hpp"
#include "detail/diagnostics.hpp"
#include "detail/debugger.hpp"
#include "runtime/detail/type_publication.hpp"
#include "language/declarations.hpp"
#include "language/module.hpp"
#include "foundation/string.hpp"
#include "memory/fatal.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>

#include <limits>

namespace atelier::source {
using namespace detail;

namespace {
Error reject_invocation(Diagnostics* diagnostics, Debugger* debugger, Error error) {
    return DebugAccess::reject(debugger, DiagnosticAccess::reject(diagnostics, std::move(error)));
}

std::uint64_t next_program_id() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == UINT64_MAX)
            fatal("Source program identity exhausted.");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}

// Entry codes are unique across every Program in the process, so caches may outlive a Program.
std::atomic<std::uint64_t> next_entry_code{1};

// Project units map combined-text offsets back to snapshot files on demand.
struct SourceMap {
    std::shared_ptr<const SourceSnapshot> snapshot;

    std::vector<language::SourcePiece> pieces;
};

// Line indexes are optional: eager diagnostic tables pass them, while a single
// failure scans its file instead of retaining an index.
bool is_line_break(std::string_view text, std::size_t index) noexcept {
    return text[index] == '\n' || (text[index] == '\r' && (index + 1 == text.size() || text[index + 1] != '\n'));
}

SourceLocation resolve_location(const SourceMap* map, std::string_view path, std::uint64_t version,
                                const BusinessLocation& position,
                                const std::vector<std::vector<std::size_t>>* line_starts = nullptr) {
    const auto offset = static_cast<std::size_t>(position.offset);
    if (map)
        for (const auto& piece : map->pieces) {
            if (offset < piece.begin || offset >= piece.end)
                continue;
            const auto& file = map->snapshot->files[piece.file];
            const auto mapped = std::min(file.source.size(), piece.offset + offset - piece.begin);
            std::size_t line = 1;
            std::size_t column = 1;
            if (line_starts) {
                const auto& lines = (*line_starts)[piece.file];
                const auto index =
                    static_cast<std::size_t>(std::upper_bound(lines.begin(), lines.end(), mapped) - lines.begin() - 1);
                line = index + 1;
                column = mapped - lines[index] + 1;
            } else {
                for (std::size_t i = 0; i < mapped; ++i)
                    if (is_line_break(file.source, i)) {
                        ++line;
                        column = 1;
                    } else
                        ++column;
            }
            return {file.path, file.version, mapped, line, column, file.disk_path};
        }
    return {std::string(path), version, offset, static_cast<std::size_t>(position.line),
            static_cast<std::size_t>(position.column)};
}

// Only diagnostic and debug captures need every site resolved up front.
std::shared_ptr<const std::vector<DiagnosticLocation>>
diagnostic_locations(const BusinessCodeInfo& info, const SourceMap* map, std::string_view path, std::uint64_t version,
                     const std::vector<std::vector<std::size_t>>* line_starts = nullptr) {
    if (!info.diagnostics && !info.debugging)
        return nullptr;
    auto locations = std::make_shared<std::vector<DiagnosticLocation>>();
    locations->reserve(info.location_count);
    for (std::uint32_t i = 0; i < info.location_count; ++i) {
        const auto& value = info.locations[i];
        locations->push_back(
            {value.function ? value.function : "", resolve_location(map, path, version, value, line_starts)});
    }
    return locations;
}

// Floating leaves compare by representation, so signed zeros differ.
bool same_scalar(const Scalar& a, const Scalar& b) {
    return std::visit(
        [&](const auto& value) {
            using T = std::remove_cvref_t<decltype(value)>;
            const auto* other = std::get_if<T>(&b);
            if (!other)
                return false;
            if constexpr (std::same_as<T, float>)
                return std::bit_cast<std::uint32_t>(value) == std::bit_cast<std::uint32_t>(*other);
            else if constexpr (std::same_as<T, double>)
                return std::bit_cast<std::uint64_t>(value) == std::bit_cast<std::uint64_t>(*other);
            else
                return value == *other;
        },
        a);
}

bool same_leaves(const std::vector<Scalar>& a, const std::vector<Scalar>& b) {
    return std::ranges::equal(a, b, same_scalar);
}
}

namespace detail {
Result<std::vector<TypeDefinition>> type_definitions(const std::vector<language::TypeDeclaration>& declarations,
                                                     std::span<const language::TypeName> array_uses,
                                                     const Binding& binding) {
    auto& runtime = *binding.runtime;
    const auto is_described_element = [&](std::string_view name) {
        if (language::editor_reserved(name) || language::synthesized_array_name(name))
            return false;
        if (std::ranges::any_of(declarations, [&](const auto& value) { return value.name == name; }))
            return true;
        for (const auto& value : binding.enumerations)
            if (value.name == name)
                return !(*runtime.describe(value.type))->native_storage;
        for (const auto& value : binding.values)
            if (value.name == name && value.kind == ValueKind::record)
                return !(*runtime.describe(value.type))->native_storage;
        return false;
    };
    std::vector<TypeDefinition> synthesized;
    const auto ensure_synthesized_array = [&](std::string_view base_name, std::uint32_t array_depth) {
        for (std::uint32_t depth = 1; depth <= array_depth; ++depth) {
            auto arr_name = language::make_synthesized_array_name(base_name, depth);
            if (std::ranges::any_of(synthesized, [&](const auto& def) { return def.name == arr_name; }))
                continue;
            TypeDefinition arr_def;
            arr_def.name = std::move(arr_name);
            arr_def.kind = TypeKind::array;
            arr_def.element = TypeReference{depth == 1 ? std::string(base_name)
                                                       : language::make_synthesized_array_name(base_name, depth - 1)};
            synthesized.push_back(std::move(arr_def));
        }
    };
    std::vector<TypeDefinition> definitions;
    for (const auto& declaration : declarations) {
        for (const auto& value : binding.values)
            if (value.math && declaration.name == math::short_name(value.math))
                return std::unexpected(
                    Error{ErrorCode::invalid_definition,
                          "Source type declaration conflicts with a builtin math name: " + declaration.name});
        TypeDefinition definition;
        definition.name = declaration.name;
        definition.kind = declaration.enumeration ? TypeKind::enumeration : TypeKind::record;
        if (declaration.enumeration) {
            std::vector<EnumeratorMetadata> enum_metadata;
            enum_metadata.reserve(declaration.enumerator_attributes.size());
            for (const auto& attr : declaration.enumerator_attributes)
                enum_metadata.push_back({attr.label, attr.description});
            definition.enumeration =
                TypeDefinition::Enumeration{declaration.signed_enum ? binding.i32 : binding.u32,
                                            declaration.enumerators, declaration.flags, std::move(enum_metadata)};
        } else {
            for (const auto& [name, type] : declaration.fields) {
                TypeReference reference;
                if (is_described_element(type.name)) {
                    if (type.array_depth == 0) {
                        reference = TypeReference{type.name};
                    } else {
                        ensure_synthesized_array(type.name, type.array_depth);
                        reference = TypeReference{language::make_synthesized_array_name(type.name, type.array_depth)};
                    }
                } else {
                    TypeId resolved{};
                    if (type.name == "float")
                        resolved = binding.f32;
                    else if (type.name == "double")
                        resolved = binding.f64;
                    else if (type.name == "uint")
                        resolved = binding.u32;
                    else if (type.name == "int")
                        resolved = binding.i32;
                    else if (type.name == "bool")
                        resolved = binding.boolean;
                    for (const auto& value : binding.enumerations)
                        if (value.name == type.name)
                            resolved = value.type;
                    for (const auto& value : binding.values)
                        if (!language::synthesized_array_name(value.name) &&
                            (value.name == type.name || (type.name == "String" && value.kind == ValueKind::string) ||
                             (value.math && type.name == math::short_name(value.math)))) {
                            resolved = value.type;
                            break;
                        }
                    if (resolved == TypeId{})
                        return std::unexpected(Error{
                            ErrorCode::invalid_definition,
                            "Source struct field type requires a declared or registered value without GC references: " +
                                type.name});
                    for (std::uint32_t depth = 0; depth < type.array_depth; ++depth) {
                        const auto element = value_kind(binding, resolved);
                        const auto array = std::ranges::find_if(binding.values, [&](const auto& value) {
                            return value.kind == ValueKind::array && value.element == element;
                        });
                        if (array == binding.values.end())
                            return std::unexpected(Error{
                                ErrorCode::invalid_definition,
                                "Source struct Array field requires an already registered array for its element: " +
                                    type.name});
                        resolved = array->type;
                    }
                    reference = resolved;
                }
                definition.fields.push_back({name, {std::move(reference)}});
            }
        }
        definitions.push_back(std::move(definition));
    }
    for (const auto& use : array_uses)
        if (use.array_depth > 0 && is_described_element(use.name))
            ensure_synthesized_array(use.name, use.array_depth);
    for (auto& arr : synthesized)
        definitions.push_back(std::move(arr));
    return definitions;
}
}

struct Program::Declaration {
    std::string name;

    FunctionSignature signature;
};

struct Program::Contract {
    Binding binding;

    std::string type_name;

    BusinessServices actual_services;

    std::vector<language::TypeDeclaration> declarations;

    explicit Contract(Binding bound, std::vector<language::TypeDeclaration> source_types = {}) noexcept
        : binding(std::move(bound)), type_name(binding.name), actual_services(services),
          declarations(std::move(source_types)) {
        binding.name = type_name;
    }

    std::optional<ValueDescription> value_description(BusinessType kind) const noexcept {
        const auto type = value_type_id(binding, kind);
        if (type != TypeId{})
            return ValueDescription{type};
        return std::nullopt;
    }

    Result<FunctionSignature> resolve_signature(const BusinessCodeInfo& info,
                                                atelier::detail::TypePublication* publication = nullptr) const {
        const auto reference = [&](BusinessType kind, TypeId type) -> TypeReference {
            if (publication) {
                if (kind.kind == ValueKind::enumeration)
                    return TypeReference{binding.enumerations[kind.type_index].name};
                if (aggregate(kind))
                    return TypeReference{binding.values[kind.type_index].name};
            }
            return type;
        };
        SignatureDefinition definition;
        definition.execution =
            info.signature.is_coroutine ? FunctionExecution::coroutine : FunctionExecution::synchronous;
        definition.parameters.reserve(info.signature.parameter_count);
        for (std::uint32_t i = 0; i < info.signature.parameter_count; ++i) {
            const auto kind = info.signature.parameters[i].type;
            const auto value = value_description(kind);
            if (!value)
                return std::unexpected(Error{ErrorCode::invalid_definition, "Unsupported compiled source parameter."});
            definition.parameters.push_back(
                {{reference(kind, value->type), value->form}, info.signature.parameters[i].passing});
        }
        if (info.signature.result != ValueKind::void_value) {
            const auto value = value_description(info.signature.result);
            if (!value)
                return std::unexpected(Error{ErrorCode::invalid_definition, "Unsupported compiled source result."});
            definition.result = ValueDefinition{reference(info.signature.result, value->type), value->form};
        }
        if (info.requires_receiver)
            definition.receiver =
                ReceiverDefinition{publication ? TypeReference{type_name} : TypeReference{binding.type}};
        return publication ? publication->resolve_signature(*binding.runtime, definition)
                           : binding.runtime->resolve_signature(definition);
    }

    Result<void> compatible(BusinessCode* code, std::string_view source_name, const Declaration* declaration = nullptr,
                            bool check_environment = true) const {
        if (!code)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate has no compiled code."});
        const auto info = atelier_business_info(code);
        const auto layout = atelier_business_layout(code);
        if (!info.function_name || !*info.function_name || (declaration && declaration->name != info.function_name))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate renamed the bound source entry."});
        if (!info.source_name || source_name != info.source_name || info.entry_address == 0 ||
            !(info.services == actual_services))
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Candidate has a different source or actual service ABI."});
        if (!layout.object_type_name || type_name != layout.object_type_name)
            return std::unexpected(
                Error{ErrorCode::type_mismatch, "Candidate has a different compiled receiver requirement."});
        const auto result = value_description(info.signature.result);
        if (declaration &&
            (declaration->signature.execution == FunctionExecution::coroutine) != info.signature.is_coroutine)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate changed its execution kind."});
        if ((info.signature.parameter_count != 0 && !info.signature.parameters) ||
            (info.signature.result != ValueKind::void_value && !result) ||
            (declaration && (declaration->signature.parameters.size() != info.signature.parameter_count ||
                             declaration->signature.result != result)))
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Candidate changed its source parameter/result signature."});
        for (std::uint32_t i = 0; i < info.signature.parameter_count; ++i) {
            const auto value = value_description(info.signature.parameters[i].type);
            if (!value || (declaration && declaration->signature.parameters[i] !=
                                              ParameterDescription{*value, info.signature.parameters[i].passing}))
                return std::unexpected(
                    Error{ErrorCode::invalid_definition, "Candidate changed its source parameter/result signature."});
        }
        if (declaration && info.requires_receiver && !declaration->signature.receiver)
            return std::unexpected(Error{ErrorCode::invalid_definition,
                                         "Candidate added a receiver to a receiverless source declaration."});
        if (check_environment && !same_environment(binding, info.environment))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate changed its bound member catalog."});
        if (layout.strong_slots < business_root_slots || layout.strong_slots >= observation_slot_bit ||
            layout.weak_slots >= observation_slot_bit)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Candidate has an invalid invocation-slot layout."});
        return {};
    }
};

struct Program::Version : Runtime::RetiredCode {
    std::shared_ptr<const Contract> contract;

    std::shared_ptr<const Declaration> declaration;

    std::uint64_t sequence;

    // Shared: a project unit compiled from identical input reuses its code in the next Version.
    std::shared_ptr<BusinessCode> code;

    BusinessCodeInfo info;

    BusinessFrameLayout layout;

    std::uintptr_t typed_address;

    bool stateless_direct;

    std::shared_ptr<const DiagnosticMetadata> diagnostic;

    std::shared_ptr<const DebugMetadata> debug;

    std::shared_ptr<const SourceMap> map;

    Version(std::shared_ptr<const Contract> bound, std::shared_ptr<const Declaration> declared, std::uint64_t request,
            std::shared_ptr<const DiagnosticMetadata> capture, std::shared_ptr<const DebugMetadata> session,
            std::shared_ptr<BusinessCode> owner, std::shared_ptr<const SourceMap> source_map = {}) noexcept
        : Runtime::RetiredCode{nullptr,
                               [](Runtime::RetiredCode* code) noexcept { delete static_cast<Version*>(code); }},
          contract(std::move(bound)), declaration(std::move(declared)), sequence(request), code(std::move(owner)),
          info(atelier_business_info(code.get())), layout(atelier_business_layout(code.get())),
          typed_address(atelier_business_typed_entry(code.get())),
          stateless_direct(typed_address != 0 && !declaration->signature.receiver.has_value() && info.stateless_typed),
          diagnostic(std::move(capture)), debug(std::move(session)), map(std::move(source_map)) {}

    static void retire(Version* version) noexcept {
        atelier::detail::assert_serial_access();
        version->contract->binding.runtime->retire_code(*version);
    }

    Result<void> bind_signature(SignatureDefinition expected) const {
        const auto& signature = declaration->signature;
        if (const auto& receiver = signature.receiver)
            expected.receiver = ReceiverDefinition{receiver->type, receiver->read_only};
        ATELIER_TRY_BIND(resolved, contract->binding.runtime->resolve_signature(expected));
        if (*resolved != signature)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Host binding does not match the source signature."});
        if (!typed_address)
            return std::unexpected(
                Error{ErrorCode::unsupported_operation, "Code has no synchronous direct typed entry."});
        return {};
    }

    // Generated failure exits report a site index; resolving it allocates, so it
    // happens only after a failure.
    void locate(Error& error, std::uint32_t site) const noexcept {
        if (site >= info.location_count)
            return;
        error.source =
            resolve_location(map.get(), diagnostic->source_name, diagnostic->code_version, info.locations[site]);
    }

    // Keep captures on this stack until the body has released its arguments.
    // The activation protects this Version even when a callback releases its last
    // shared owner. Successful starts transfer captures to their persistent frame.
    template <class T, class F>
    Result<T> invoke_captured(Diagnostics* diagnostics, Debugger* debugger, F&& body) const {
        auto activation = ExecutionScope::enter(*contract->binding.runtime);
        if (!activation)
            return std::unexpected(reject_invocation(diagnostics, debugger, std::move(activation.error())));
        if (!diagnostics && !debugger)
            return body(*activation, nullptr, nullptr);
        auto capture = DiagnosticAccess::begin(diagnostics, diagnostic, *contract->binding.runtime, info.diagnostics);
        if (!capture)
            return std::unexpected(DebugAccess::reject(debugger, std::move(capture.error())));
        DiagnosticScope scope(std::move(*capture), *contract->binding.runtime);
        auto session = DebugAccess::begin(debugger, debug, info.debugging);
        if (!session)
            return scope.complete(Result<T>{std::unexpected(std::move(session.error()))});
        DebugScope debug_scope(std::move(*session));
        auto result = body(*activation, &scope, &debug_scope);
        if constexpr (std::same_as<T, Coroutine>) {
            if (result) {
                scope.transfer(DiagnosticStatus::ready);
                debug_scope.transfer(DebugStatus::ready);
                return result;
            }
        }
        return scope.complete(debug_scope.complete(std::move(result)));
    }

    template <class T>
    Result<std::optional<Scalar>> invoke_values(ObjectId receiver, std::span<const T> arguments,
                                                Diagnostics* diagnostics, Debugger* debugger) const {
        return invoke_captured<std::optional<Scalar>>(
            diagnostics, debugger,
            [&](ExecutionScope& activation, DiagnosticScope* scope,
                DebugScope* debug_scope) -> Result<std::optional<Scalar>> {
                if (aggregate(info.signature.result))
                    return std::unexpected(Error{ErrorCode::argument_mismatch,
                                                 "Aggregate source results require typed value output storage."});
                const AbiArguments encoded(contract->binding, declaration->signature, info.signature, arguments);
                if (!encoded.status)
                    return std::unexpected(encoded.status.error());
                ATELIER_TRY_BIND(result,
                                 invoke(activation, receiver, encoded.values, nullptr, scope ? scope->trace() : nullptr,
                                        debug_scope ? debug_scope->state().get() : nullptr));
                auto decoded = detail::scalar_value(*result, &contract->binding);
                if (result->kind != info.signature.result || result->storage != ArgumentStorage::value ||
                    (info.signature.result != ValueKind::void_value && !decoded))
                    return std::unexpected(
                        Error{ErrorCode::invocation_failed, "Compiled source returned an invalid scalar value."});
                return decoded;
            });
    }

    template <class T>
    Result<void> invoke_values(ObjectId receiver, std::span<const T> arguments, ValueOutput output,
                               Diagnostics* diagnostics, Debugger* debugger) const {
        return invoke_captured<void>(
            diagnostics, debugger,
            [&](ExecutionScope& activation, DiagnosticScope* scope, DebugScope* debug_scope) -> Result<void> {
                const auto kind = info.signature.result;
                const auto& binding = contract->binding;
                if (!aggregate(kind) || kind.type_index >= binding.values.size() ||
                    !(binding.values[kind.type_index].math || binding.values[kind.type_index].plain_value))
                    return std::unexpected(Error{ErrorCode::argument_mismatch,
                                                 "Typed value output requires a math or plain record source result."});
                const auto& value = binding.values[kind.type_index];
                if (output.type != value.type || !output.data ||
                    reinterpret_cast<std::uintptr_t>(output.data) % value.alignment != 0)
                    return std::unexpected(
                        Error{ErrorCode::argument_mismatch,
                              "Source output storage does not match its declared value type and alignment."});
                const AbiArguments encoded(binding, declaration->signature, info.signature, arguments);
                if (!encoded.status)
                    return std::unexpected(encoded.status.error());
                // Only the returned representation enters this buffer. Constructing a
                // native value here would execute an unrelated default constructor.
                ::atelier::detail::ValueStorage staging_owner;
                auto* staged = staging_owner.prepare(value.size, value.alignment);
                ATELIER_TRY_BIND(result,
                                 invoke(activation, receiver, encoded.values, staged, scope ? scope->trace() : nullptr,
                                        debug_scope ? debug_scope->state().get() : nullptr));
                if (result->kind != kind || result->storage != ArgumentStorage::value || result->data != staged)
                    return std::unexpected(Error{ErrorCode::invocation_failed,
                                                 "Compiled aggregate result changed its typed output storage."});
                std::memcpy(output.data, staged, value.size);
                return {};
            });
    }

    static Result<Coroutine> start_values(std::shared_ptr<const Version> version, ObjectId receiver,
                                          std::span<const Scalar> arguments, Diagnostics* diagnostics,
                                          Debugger* debugger);

    Result<BusinessValue> invoke(ExecutionScope& activation, ObjectId receiver,
                                 std::span<const BusinessValue> arguments, void* result_data = nullptr,
                                 BusinessTrace* trace = nullptr, DebugSessionState* debugger = nullptr) const {
        if (info.signature.is_coroutine)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Coroutine code requires a persistent execution instance."});
        if (aggregate(info.signature.result) && !result_data)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Aggregate source results require typed value output storage."});
        const auto& binding = contract->binding;
        std::uint32_t site = no_diagnostic_location;
        auto result = execute_admitted(activation, *binding.runtime, binding, info, layout,
                                       declaration->signature.receiver.has_value(), receiver, arguments, result_data,
                                       trace, debugger, &site);
        if (!result)
            locate(result.error(), site);
        return result;
    }
};

#include "detail/module_state.inl"

struct Program::State {
    std::shared_ptr<const Contract> contract;

    std::shared_ptr<const Declaration> declaration;

    std::shared_ptr<const Version> current;

    std::shared_ptr<const ModuleVersion> module;

    const std::uint64_t id{next_program_id()};

    std::uint64_t latest_request{};

    std::uint64_t published_request{};

    std::uint64_t revision{};

    bool open{true};

    explicit State(std::shared_ptr<const Contract> bound) : contract(std::move(bound)) {}

    std::optional<std::size_t> find(std::string_view name) const {
        if (!module)
            return {};
        for (std::size_t i = 0; i < module->entries.size(); ++i)
            if (module->entries[i].name == name)
                return i;
        return {};
    }

    const std::shared_ptr<const Version>& select(std::optional<std::size_t> index) const {
        static const std::shared_ptr<const Version> empty;
        if (!index)
            return module ? empty : current;
        return module && *index < module->versions.size() ? module->versions[*index] : empty;
    }
};

struct Program::DirectScope::Stateless {
    ExecutionScope scope;

    const Version* version;

    BusinessFailure failure{};
};

struct Program::DirectScope::Active {
    ExecutionScope scope;

    const Version* version;

    Invocation invocation;

    BusinessFailure failure{};

    Active(ExecutionScope entered, const Version& owner)
        : scope(std::move(entered)), version(&owner),
          invocation(*owner.contract->binding.runtime, owner.contract->binding, owner.layout, nullptr, nullptr,
                     &scope) {}
};

bool Program::DirectScope::try_enter(const Version* version, const State* owner, std::optional<std::size_t> index,
                                     ObjectId receiver, DirectContext& out) noexcept {
    static_assert(sizeof(Stateless) <= sizeof(storage_) && alignof(Stateless) <= 16);
    static_assert(sizeof(Active) <= sizeof(storage_) && alignof(Active) <= 16);
    atelier::detail::assert_serial_access();
    if (owner)
        version = owner->open ? owner->select(index).get() : nullptr;
    if (!version)
        return false;
    const auto& binding = version->contract->binding;
    if (version->stateless_direct) {
        auto entered = ExecutionScope::try_enter(*binding.runtime);
        if (!entered)
            return false;
        if (receiver != ObjectId{}) {
            NativeContext unused;
            if (!NativeContext::try_resolve(*binding.runtime, receiver, binding.type, unused))
                return false;
        }
        auto* stateless =
            std::construct_at(reinterpret_cast<Stateless*>(storage_), Stateless{std::move(entered), version, {}});
        mode_ = Mode::stateless;
        out = {version->typed_address, nullptr, &stateless->failure};
        return true;
    }
    auto entered = ExecutionScope::try_enter(*binding.runtime);
    if (!entered)
        return false;
    auto* active = std::construct_at(reinterpret_cast<Active*>(storage_), std::move(entered), *version);
    mode_ = Mode::full;
    if (!try_bind_receiver(active->invocation, version->declaration->signature.receiver.has_value(), receiver)) {
        leave();
        return false;
    }
    out = {version->typed_address, &active->invocation, &active->failure};
    return true;
}

Error Program::DirectScope::enter_error(const Version* version, const State* owner, std::optional<std::size_t> index,
                                        ObjectId receiver) {
    atelier::detail::assert_serial_access();
    if (owner)
        version = owner->open ? owner->select(index).get() : nullptr;
    if (!version)
        return {ErrorCode::invalid_definition, "Typed entry has no published version."};
    const auto& binding = version->contract->binding;
    auto entered = ExecutionScope::enter(*binding.runtime);
    if (!entered)
        return std::move(entered.error());
    Active active(std::move(*entered), *version);
    auto bound = bind_receiver(active.invocation, version->declaration->signature.receiver.has_value(), receiver);
    return bound ? Error{ErrorCode::invocation_failed, "Direct entry admission failed."} : std::move(bound.error());
}

void Program::DirectScope::leave() noexcept {
    if (mode_ == Mode::stateless)
        std::destroy_at(std::launder(reinterpret_cast<Stateless*>(storage_)));
    else if (mode_ == Mode::full)
        std::destroy_at(std::launder(reinterpret_cast<Active*>(storage_)));
    mode_ = Mode::none;
}

bool Program::DirectScope::try_finish(std::uint32_t status) noexcept {
    if (mode_ == Mode::stateless) {
        if (status != 0)
            return false;
        std::destroy_at(std::launder(reinterpret_cast<Stateless*>(storage_)));
        mode_ = Mode::none;
        return true;
    }
    auto& active = *std::launder(reinterpret_cast<Active*>(storage_));
    if (status != 0 || active.invocation.failed || !active.invocation.resources_cleared())
        return false;
    leave();
    return true;
}

Error Program::DirectScope::finish_error(std::uint32_t status) {
    if (mode_ == Mode::stateless) {
        auto& stateless = *std::launder(reinterpret_cast<Stateless*>(storage_));
        const auto reason = failure_message(stateless.failure.reason, stateless.version->info.failure_messages,
                                            stateless.version->info.failure_message_count);
        Error error{ErrorCode::invocation_failed,
                    reason ? std::string(*reason) : std::string("Source execution failed."), std::nullopt,
                    resolve_failure_element(stateless.version->contract->binding, stateless.failure)};
        stateless.version->locate(error, stateless.failure.location);
        leave();
        return error;
    }
    auto& active = *std::launder(reinterpret_cast<Active*>(storage_));
    std::uint32_t site = no_diagnostic_location;
    auto result = finish_invocation(active.invocation, status, active.failure, &site,
                                    active.version->info.failure_messages, active.version->info.failure_message_count);
    Error error = result ? Error{ErrorCode::invocation_failed, "Generated code left execution resources alive."}
                         : std::move(result.error());
    active.version->locate(error, site);
    leave();
    return error;
}

Result<std::uint64_t> Program::invoke_bound(const Version* version, const State* owner,
                                            std::optional<std::size_t> index, ObjectId receiver, void* arguments,
                                            DirectCall invoke, Diagnostics* diagnostics, Debugger* debugger) {
    atelier::detail::assert_serial_access();
    if (owner)
        version = owner->open ? owner->select(index).get() : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Typed entry has no published version."}));
    // No handle/State access follows this selection. invoke_captured enters before
    // any callback and leaves after error location, diagnostics and value cleanup.
    return version->invoke_captured<std::uint64_t>(
        diagnostics, debugger,
        [&](ExecutionScope& activation, DiagnosticScope* capture, DebugScope* debug) -> Result<std::uint64_t> {
            struct Call {
                std::uintptr_t address;

                void* arguments;

                DirectCall invoke;

                std::uint64_t output{};
            } call{version->typed_address, arguments, invoke};
            const auto body = [](void* context, Invocation& invocation, BusinessFailure& failure) noexcept {
                auto& call = *static_cast<Call*>(context);
                return call.invoke(call.address, call.arguments, &invocation, &call.output, &failure, invocation.trace);
            };
            const auto& binding = version->contract->binding;
            std::uint32_t site = no_diagnostic_location;
            auto result =
                execute_invocation(activation, *binding.runtime, binding, version->layout,
                                   version->declaration->signature.receiver.has_value(), receiver, &call, body,
                                   capture ? capture->trace() : nullptr, debug ? debug->state().get() : nullptr, &site,
                                   version->info.failure_messages, version->info.failure_message_count);
            if (!result) {
                version->locate(result.error(), site);
                return std::unexpected(std::move(result.error()));
            }
            return call.output;
        });
}

struct Program::Frame final : CoroutineFrame {
    std::shared_ptr<const Version> version;

    std::shared_ptr<DiagnosticState> diagnostic;

    std::shared_ptr<DebugSessionState> debug;

    Invocation invocation;

    std::unique_ptr<BusinessCoroutine, decltype(&atelier_destroy_business_coroutine)> code_frame{
        nullptr, &atelier_destroy_business_coroutine};

    bool captures_owned{};

    DiagnosticStatus terminal{DiagnosticStatus::cancelled};

    DebugStatus debug_terminal{DebugStatus::cancelled};

    Frame(std::shared_ptr<const Version> owner, std::shared_ptr<DiagnosticState> capture,
          std::shared_ptr<DebugSessionState> debugger)
        : version(std::move(owner)), diagnostic(std::move(capture)), debug(std::move(debugger)),
          invocation(*version->contract->binding.runtime, version->contract->binding, version->layout,
                     diagnostic ? &diagnostic->trace : nullptr, debug.get()) {}

    ~Frame() override {
        // Generated destruction can call host cleanup, so retain Invocation and
        // Version until the DLL frame has finished using them.
        if (diagnostic)
            DiagnosticAccess::running(*diagnostic);
        if (debug)
            DebugAccess::status(*debug, DebugStatus::running);
        const bool generated = code_frame != nullptr;
        code_frame.reset();
        if (!diagnostic && !debug)
            return;
        const bool cleared = generated && invocation.resources_cleared();
        // Report Runtime resources after fallback roots are released, while
        // preserving whether generated cleanup itself honored its contract.
        for (auto& root : invocation.roots)
            root.reset();
        invocation.observations.clear();
        invocation.record_resources(generated ? cleared : invocation.resources_cleared());
        if (captures_owned) {
            if (diagnostic)
                DiagnosticAccess::finish(*diagnostic, invocation.runtime, terminal);
            if (debug)
                DebugAccess::finish(*debug, debug_terminal);
        }
    }

    void on_terminal(CoroutineState state, const Error* error) noexcept override {
        terminal = state == CoroutineState::failed      ? DiagnosticStatus::failed
                   : state == CoroutineState::completed ? DiagnosticStatus::completed
                                                        : DiagnosticStatus::cancelled;
        debug_terminal = state == CoroutineState::failed      ? DebugStatus::failed
                         : state == CoroutineState::completed ? DebugStatus::completed
                                                              : DebugStatus::cancelled;
        if (diagnostic && error)
            DiagnosticAccess::fail(*diagnostic, *error);
    }

    Result<CoroutineStep> failure(Error error) {
        if (diagnostic)
            DiagnosticAccess::fail(*diagnostic, error);
        return std::unexpected(std::move(error));
    }

    Result<CoroutineStep> resume() noexcept override {
        if (diagnostic)
            DiagnosticAccess::running(*diagnostic, true);
        if (debug)
            DebugAccess::status(*debug, DebugStatus::running);
        char message[512];
        bool suspended{};
        BusinessValue output{};
        std::uint32_t site = no_diagnostic_location;
        EntryFailure reason = EntryFailure::none;
        BusinessFailure failure_detail{};
        const auto status = atelier_resume_business_coroutine(code_frame.get(), &suspended, &output, message,
                                                              sizeof(message), &site, &reason, &failure_detail);
        const auto decoded = status == 0 && !suspended && !invocation.failed
                                 ? scalar_value(output, &version->contract->binding)
                                 : std::optional<Scalar>{};
        const bool invalid_result =
            status == 0 && !suspended && !invocation.failed &&
            (output.kind != version->info.signature.result || output.storage != ArgumentStorage::value ||
             (version->info.signature.result != ValueKind::void_value && !decoded));
        const bool terminal_step = status != 0 || !suspended || invocation.failed;
        const bool cleared = (terminal_step || diagnostic) && invocation.resources_cleared();
        invocation.record_resources(cleared);
        if (status != 0 && !invocation.failed && invocation.debug_aborted) {
            if (!cleared)
                return failure({ErrorCode::invocation_failed, "Aborted coroutine left execution resources alive."});
            request_cancel();
            return CoroutineStep{CoroutineState::completed};
        }
        if (status != 0 || invocation.failed) {
            const auto resolved =
                !invocation.failed && reason != EntryFailure::none
                    ? failure_message(reason, version->info.failure_messages, version->info.failure_message_count)
                    : std::nullopt;
            std::optional<ErrorElement> element;
            if (invocation.failed) {
                if (invocation.error_element != no_failure_element)
                    element = ErrorElement{std::string(invocation.error_layer), invocation.error_element};
            } else {
                element = resolve_failure_element(version->contract->binding, failure_detail);
            }
            Error error{invocation.failed ? invocation.error_code : ErrorCode::invocation_failed,
                        invocation.failed ? std::string(invocation.error_message())
                        : resolved        ? std::string(*resolved)
                                          : std::string(message),
                        std::nullopt, std::move(element)};
            version->locate(error, site);
            return failure(std::move(error));
        }
        if (terminal_step && !cleared)
            return failure({ErrorCode::invocation_failed, "Coroutine left execution resources alive."});
        if (suspended) {
            if (diagnostic)
                DiagnosticAccess::pause(*diagnostic, invocation.runtime, DiagnosticStatus::suspended);
            if (debug)
                DebugAccess::status(*debug, DebugStatus::suspended);
            return CoroutineStep{CoroutineState::suspended};
        }
        if (invalid_result)
            return failure({ErrorCode::invocation_failed, "Coroutine returned an invalid scalar value."});
        return CoroutineStep{CoroutineState::completed, decoded};
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

    std::shared_ptr<ModuleVersion> module;

    std::shared_ptr<const DiagnosticMetadata> diagnostic;

    std::shared_ptr<const DebugMetadata> debug;
};

Result<Coroutine> Program::Version::start_values(std::shared_ptr<const Version> version, ObjectId receiver,
                                                 std::span<const Scalar> arguments, Diagnostics* diagnostics,
                                                 Debugger* debugger) {
    return version->invoke_captured<Coroutine>(
        diagnostics, debugger,
        [&](ExecutionScope&, DiagnosticScope* scope, DebugScope* debug_scope) -> Result<Coroutine> {
            const AbiArguments encoded(version->contract->binding, version->declaration->signature,
                                       version->info.signature, arguments);
            if (!encoded.status)
                return std::unexpected(encoded.status.error());
            if (!version->info.signature.is_coroutine)
                return std::unexpected(
                    Error{ErrorCode::invalid_definition, "Starting a coroutine requires a coroutine declaration."});
            const auto& binding = version->contract->binding;
            const bool requires_receiver = version->declaration->signature.receiver.has_value();
            auto frame = std::make_unique<Frame>(version, scope ? scope->state() : nullptr,
                                                 debug_scope ? debug_scope->state() : nullptr);
            ATELIER_TRY(bind_receiver(frame->invocation, requires_receiver, receiver));
            BusinessCoroutine* compiled{};
            char diagnostic[512];
            if (atelier_create_business_coroutine(frame->version->code.get(), &frame->invocation, encoded.values.data(),
                                                  static_cast<std::uint32_t>(encoded.values.size()), &compiled,
                                                  diagnostic, sizeof(diagnostic), frame->invocation.trace) != 0)
                return std::unexpected(Error{ErrorCode::invocation_failed, diagnostic});
            frame->code_frame.reset(compiled);
            if (frame->diagnostic)
                frame->invocation.record_resources(frame->invocation.resources_cleared());
            auto* observed = frame.get();
            auto result = Coroutine::create(*binding.runtime, std::move(frame));
            if (result)
                observed->captures_owned = true;
            return result;
        });
}

Program::SourceRequest::SourceRequest(std::weak_ptr<State> owner, std::shared_ptr<const Contract> contract,
                                      std::shared_ptr<const Declaration> declaration, std::uint64_t number,
                                      std::string name, std::string text, std::string entry, CompileOptions compilation)
    : owner_(std::move(owner)), contract_(std::move(contract)), compilation_contract_(contract_),
      declaration_(std::move(declaration)), sequence(number), source_name(std::move(name)), source(std::move(text)),
      entry_name(std::move(entry)), options(compilation) {}

Program::Candidate::Candidate() = default;

Program::Candidate::Candidate(std::unique_ptr<Prepared> prepared) : prepared_(std::move(prepared)) {}

Program::Candidate::~Candidate() {
    atelier::detail::assert_serial_access();
}

Program::Candidate::Candidate(Candidate&&) noexcept = default;

Program::Candidate& Program::Candidate::operator=(Candidate&& other) noexcept {
    atelier::detail::assert_serial_access();
    prepared_ = std::move(other.prepared_);
    return *this;
}

Program::Candidate::operator bool() const noexcept {
    return prepared_ && (prepared_->code || prepared_->module);
}

std::span<const ClassInfo> Program::Candidate::classes() const noexcept {
    return prepared_ && prepared_->module ? std::span<const ClassInfo>{prepared_->module->classes}
                                          : std::span<const ClassInfo>{};
}

std::span<const EntryInfo> Program::Candidate::entries() const noexcept {
    return prepared_ && prepared_->module ? std::span<const EntryInfo>{prepared_->module->entries}
                                          : std::span<const EntryInfo>{};
}

Program::Lease::Lease(std::shared_ptr<const Version> version) : version_(std::move(version)) {}

Program::Entry::Entry(std::weak_ptr<State> owner, std::optional<std::size_t> index)
    : owner_(std::move(owner)), index_(index) {}

std::uintptr_t ProgramAccess::address(const Program::Lease& lease) noexcept {
    return lease.version_ ? lease.version_->info.entry_address : 0;
}

bool ProgramAccess::stateless_direct(const Program::Lease& lease) noexcept {
    return lease.version_ && lease.version_->stateless_direct;
}

Result<CompilationArtifacts> Program::Lease::artifacts() const {
    const auto version = version_;
    if (!version)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    BusinessArtifacts view{};
    if (!atelier_query_business_artifacts(version->code.get(), &view))
        return std::unexpected(
            Error{ErrorCode::unsupported_operation, "This source version did not retain compiler artifacts."});
    CompilationArtifacts result;
    result.entry_symbol = view.entry_symbol;
    result.target_triple = view.target_triple;
    result.cpu_features = view.cpu_features;
    result.original_ir.assign(view.original_ir, view.original_ir_size);
    result.optimized_ir.assign(view.optimized_ir, view.optimized_ir_size);
    result.assembly.assign(view.assembly, view.assembly_size);
    result.object_code.assign(view.object_code, view.object_code + view.object_code_size);
    return result;
}

Result<std::vector<DiagnosticLocation>> Program::Lease::debug_sites() const {
    const auto version = version_;
    if (!version)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    if (!version->info.debugging || !version->debug || !version->debug->tables || !version->debug->locations)
        return std::unexpected(
            Error{ErrorCode::unsupported_operation, "This source version was compiled without debugging."});
    std::vector<DiagnosticLocation> sites;
    sites.reserve(version->debug->tables->sites.size());
    for (const auto index : version->debug->tables->sites) {
        sites.push_back((*version->debug->locations)[index]);
    }
    return sites;
}

Result<std::optional<Scalar>> Program::Entry::invoke(ObjectId receiver, std::span<const Scalar> arguments,
                                                     Diagnostics* diagnostics, Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return version->invoke_values(receiver, arguments, diagnostics, debugger);
}

Result<std::optional<Scalar>> Program::Entry::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
                                                               Diagnostics* diagnostics, Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return version->invoke_values(receiver, arguments, diagnostics, debugger);
}

Result<void> Program::Lease::bind_signature(SignatureDefinition expected) const {
    atelier::detail::assert_serial_access();
    const auto version = version_;
    if (!version)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    return version->bind_signature(std::move(expected));
}

Result<std::shared_ptr<Program::State>> Program::Entry::bind_signature(SignatureDefinition expected) const {
    atelier::detail::assert_serial_access();
    auto owner = owner_.lock();
    const auto* version = owner && owner->open ? owner->select(index_).get() : nullptr;
    if (!version)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no published version."});
    ATELIER_TRY(version->bind_signature(std::move(expected)));
    return owner;
}

Result<std::optional<Scalar>> Program::Lease::invoke(ObjectId receiver, std::span<const Scalar> arguments,
                                                     Diagnostics* diagnostics, Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return version->invoke_values(receiver, arguments, diagnostics, debugger);
}

Result<std::optional<Scalar>> Program::Lease::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
                                                               Diagnostics* diagnostics, Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return version->invoke_values(receiver, arguments, diagnostics, debugger);
}

Result<void> Program::Entry::invoke(ObjectId receiver, std::span<const Scalar> arguments, ValueOutput output,
                                    Diagnostics* diagnostics, Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return version->invoke_values(receiver, arguments, output, diagnostics, debugger);
}

Result<void> Program::Entry::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
                                              ValueOutput output, Diagnostics* diagnostics, Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return version->invoke_values(receiver, arguments, output, diagnostics, debugger);
}

Result<void> Program::Lease::invoke(ObjectId receiver, std::span<const Scalar> arguments, ValueOutput output,
                                    Diagnostics* diagnostics, Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return version->invoke_values(receiver, arguments, output, diagnostics, debugger);
}

Result<void> Program::Lease::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
                                              ValueOutput output, Diagnostics* diagnostics, Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return version->invoke_values(receiver, arguments, output, diagnostics, debugger);
}

Result<Coroutine> Program::Entry::start(ObjectId receiver, std::span<const Scalar> arguments, Diagnostics* diagnostics,
                                        Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return Version::start_values(version, receiver, arguments, diagnostics, debugger);
}

Result<Coroutine> Program::Lease::start(ObjectId receiver, std::span<const Scalar> arguments, Diagnostics* diagnostics,
                                        Debugger* debugger) const {
    atelier::detail::assert_serial_access();
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return Version::start_values(version, receiver, arguments, diagnostics, debugger);
}

Result<std::unique_ptr<Program>> Program::create(Runtime& runtime, TypeId type) noexcept {
    atelier::detail::assert_serial_access();
    ATELIER_TRY_BIND(binding, bind_type(runtime, type));
    return std::unique_ptr<Program>{new Program(std::make_shared<const Contract>(std::move(*binding)))};
}

Result<std::unique_ptr<Program>> Program::create_like(const Program& prototype) noexcept {
    atelier::detail::assert_serial_access();
    if (!prototype.state_->open || !prototype.state_->contract->declarations.empty())
        return std::unexpected(
            Error{ErrorCode::invalid_definition,
                  "Shared Program contracts require an open prototype without source type declarations."});
    return std::unique_ptr<Program>{new Program(prototype.state_->contract)};
}

Program::Program(std::shared_ptr<const Contract> contract) noexcept
    : state_(std::make_shared<State>(std::move(contract))) {}

Program::~Program() {
    close();
}

void Program::close() noexcept {
    atelier::detail::assert_serial_access();
    state_->open = false;
    state_->current.reset();
    state_->module.reset();
}

Program::Entry Program::entry() const {
    return Entry{state_};
}

Result<FunctionSignature> Program::describe() const {
    if (!state_->open || !state_->declaration)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Business source declaration has not been published."});
    return state_->declaration->signature;
}

Result<Program::Lease> Program::pin() const {
    atelier::detail::assert_serial_access();
    if (!state_->open || !state_->current)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no version to pin."});
    return Lease{state_->current};
}

Result<Program::Request> Program::request(std::string_view name, std::string_view source, std::string_view entry_name,
                                          CompileOptions options) {
    atelier::detail::assert_serial_access();
    if (!state_->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    if (state_->module)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "A project program requires a complete source snapshot."});
    if (state_->latest_request == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::capacity_exceeded, "Business request sequence exhausted."});
    if (source.size() > std::numeric_limits<std::uint32_t>::max() || name.find('\0') != std::string_view::npos ||
        entry_name.find('\0') != std::string_view::npos)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Business source/name exceeds its compiler contract."});
    if (entry_name.empty() && state_->declaration)
        entry_name = state_->declaration->name;
    const auto sequence = state_->latest_request + 1;
    auto result = std::shared_ptr<SourceRequest>{
        new SourceRequest(state_, state_->contract, state_->declaration, sequence,
                          std::string(name) + "[request#" + std::to_string(sequence) + "]", std::string(source),
                          std::string(entry_name), options)};
    result->source_path_ = name;
    state_->latest_request = sequence;
    const auto report = [&](std::string message, language::SourcePosition position = {}) {
        result->declaration_error_ =
            Error{ErrorCode::invalid_definition, result->source_name + ":" + std::to_string(position.line) + ":" +
                                                     std::to_string(position.column) + ": " + std::move(message)};
    };
    auto declarations = language::collect_declarations(source);
    if (!declarations) {
        report(declarations.error().message, declarations.error().position);
        return result;
    }
    auto array_uses = language::collect_array_types(source);
    if (!array_uses) {
        report(array_uses.error().message, array_uses.error().position);
        return result;
    }
    // Published declarations are immutable; new ones publish with the next version.
    const auto& previous = state_->contract->declarations;
    const auto published = [&](std::string_view name) {
        return std::ranges::find(previous, name, &language::TypeDeclaration::name) != previous.end();
    };
    for (const auto& type : previous) {
        const auto found = std::ranges::find(*declarations, type.name, &language::TypeDeclaration::name);
        if (found == declarations->end() || !found->same_definition(type)) {
            report("Published source type definitions are immutable; restart the Runtime to change them.");
            return result;
        }
    }
    auto& runtime = *state_->contract->binding.runtime;
    auto definitions = type_definitions(*declarations, *array_uses, state_->contract->binding);
    if (!definitions) {
        report(definitions.error().message);
        return result;
    }
    std::erase_if(*definitions, [&](const auto& definition) {
        return published(definition.name) ||
               (language::synthesized_array_name(definition.name) && runtime.find_type(definition.name).has_value());
    });
    if (definitions->empty())
        return result;
    auto publication = atelier::detail::TypePublication::prepare(runtime, std::move(*definitions));
    if (!publication) {
        report(publication.error().message);
        return result;
    }
    result->type_publication_ = *publication;
    auto binding = bind_type(runtime, state_->contract->binding.type, (*publication)->catalog());
    if (!binding) {
        report(binding.error().message);
        return result;
    }
    result->compilation_contract_ = std::make_shared<const Contract>(std::move(*binding), std::move(*declarations));
    return result;
}

Result<Program::Candidate> Program::prepare(const Request& request, std::optional<NeedsCode> needs_code,
                                            CompileTiming* timing) {
    atelier::detail::assert_serial_access();
    if (timing)
        *timing = {};
    if (!request)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source request is empty."});
    const auto owner = request->owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source request belongs to a closed program."});
    if (request->declaration_error_)
        return std::unexpected(*request->declaration_error_);
    if (request->module_) {
        if (needs_code)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Entry dependency selection requires a single source input."});
        if (timing)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Compile timing requires a single source input."});
        return Program::prepare_module(request);
    }
    ATELIER_TRY_BIND(emission, emit_entry(request, needs_code, timing));
    return finish_entry(std::move(*emission));
}

void Program::Emission::Destroy::operator()(BusinessCode* code) const noexcept {
    atelier_destroy_business(code);
}

Result<Program::Emission> Program::emit_entry(const Request& request, std::optional<NeedsCode> needs_code,
                                              CompileTiming* timing) {
    if (!request || request->module_)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Entry compilation requires a single Source input."});
    if (request->declaration_error_)
        return std::unexpected(*request->declaration_error_);
    if (timing)
        *timing = {};
    BusinessCode* output{};
    char diagnostic[2048];
    const auto& contract = *request->compilation_contract_;
    const EnvironmentView storage(contract.binding);
    const auto environment = storage.view(contract.type_name.c_str());
    auto callback = [&](std::span<const EntryFunction> functions) noexcept {
        return (*needs_code)(functions) || request->type_publication_ != nullptr;
    };
    const EntryFilter selection{&callback,
                                [](void* context, const EntryFunction* functions, std::uint32_t count) noexcept {
                                    return (*static_cast<decltype(callback)*>(context))({functions, count});
                                }};
    const auto status = atelier_compile_business(
        &contract.actual_services, request->source_name.c_str(), request->source.data(),
        static_cast<std::uint32_t>(request->source.size()), &environment, &output, diagnostic, sizeof(diagnostic),
        request->entry_name.c_str(), request->options, needs_code ? &selection : nullptr, timing);
    if (status != 0)
        return std::unexpected(Error{ErrorCode::invalid_definition, diagnostic});
    return Emission{request, output};
}

Result<Program::Candidate> Program::finish_entry(Emission emission) {
    atelier::detail::assert_serial_access();
    const auto& request = emission.request_;
    if (!request)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source emission is empty."});
    const auto owner = request->owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source emission belongs to a closed program."});
    const auto& contract = *request->compilation_contract_;
    CodeOwner code{emission.code_.release(), &atelier_destroy_business};
    if (!code)
        return Candidate{};
    const auto info = atelier_business_info(code.get());
    if (!request->entry_name.empty() && (!info.function_name || request->entry_name != info.function_name))
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Compiler did not produce the selected module entry."});
    auto compatible = contract.compatible(code.get(), request->source_name, request->declaration_.get());
    if (!compatible)
        return std::unexpected(
            Error{compatible.error().code, request->source_name + ": " + compatible.error().message});
    auto metadata = std::make_shared<const DiagnosticMetadata>(
        DiagnosticMetadata{owner->id, request->sequence, info.function_name, request->source_path_,
                           diagnostic_locations(info, nullptr, request->source_path_, request->sequence)});
    auto debug = make_debug_metadata(*metadata, info);
    return Program::Candidate{std::make_unique<Program::Prepared>(
        request->owner_, request->contract_, request->compilation_contract_, request->type_publication_,
        request->sequence, std::move(code), request->source_name, nullptr, std::move(metadata), std::move(debug))};
}

Result<void> Program::try_publish(Candidate& candidate) {
    atelier::detail::assert_serial_access();
    const auto owner = state_;
    if (!owner->open || !candidate)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Program closed or candidate already consumed."});
    if (candidate.prepared_->module)
        return publish_module(candidate);
    auto& prepared = *candidate.prepared_;
    if (prepared.owner.lock() != owner || prepared.contract != owner->contract)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Candidate belongs to another program/Runtime contract."});
    if (prepared.sequence != owner->latest_request || prepared.sequence <= owner->published_request)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Candidate is stale or its request was already published."});
    ATELIER_TRY(prepared.compilation_contract->compatible(prepared.code.get(), prepared.source_name,
                                                          owner->declaration.get(), false));
    ATELIER_TRY(owner->contract->binding.runtime->check_code_publication());
    if (prepared.type_publication) {
        ATELIER_TRY(prepared.type_publication->reserve_commit(*owner->contract->binding.runtime));
    }
    auto declaration = owner->declaration;
    if (!declaration) {
        const auto info = atelier_business_info(prepared.code.get());
        ATELIER_TRY_BIND(signature,
                         prepared.compilation_contract->resolve_signature(info, prepared.type_publication.get()));
        declaration = std::make_shared<const Declaration>(Declaration{info.function_name, std::move(*signature)});
    }
    auto version =
        std::shared_ptr<const Version>{new Version(prepared.compilation_contract, declaration, prepared.sequence,
                                                   prepared.diagnostic, prepared.debug, std::move(prepared.code)),
                                       &Version::retire};
    if (owner->current && owner->current->typed_address && !version->typed_address)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate removed a bound typed entry."});
    if (prepared.type_publication)
        prepared.type_publication->commit(*owner->contract->binding.runtime);
    owner->contract = prepared.compilation_contract;
    owner->declaration = std::move(declaration);
    owner->published_request = prepared.sequence;
    ++owner->revision;
    owner->current = std::move(version);
    return {};
}
#include "detail/program_module.inl"
}
