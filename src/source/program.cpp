#include "source/program.hpp"
#include "source/detail/bridge.hpp"
#include "source/detail/diagnostics.hpp"
#include "source/detail/debugger.hpp"
#include "runtime/detail/type_publication.hpp"
#include "language/declarations.hpp"
#include "language/module.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include <limits>
#include <stdexcept>

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
            throw std::overflow_error("Source program identity exhausted.");
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
                    if (file.source[i] == '\n') {
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

template <class T>
T take(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    return std::move(*value);
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
                                                     const Binding& binding) {
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
            definition.enumeration = TypeDefinition::Enumeration{declaration.signed_enum ? binding.i32 : binding.u32,
                                                                 declaration.enumerators};
        } else {
            for (const auto& [name, type] : declaration.fields) {
                TypeReference reference;
                const bool declared =
                    std::ranges::any_of(declarations, [&](const auto& value) { return value.name == type.name; });
                if (declared && type.array_depth == 0)
                    reference = TypeReference{type.name};
                else {
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
                        if (value.name == type.name || (type.name == "String" && value.kind == ValueKind::string) ||
                            (value.math && type.name == math::short_name(value.math))) {
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
    return definitions;
}
}

struct Program::Metadata {
    const TypeInfo* identity;

    TypeInfo value;

    bool matches(const TypeInfo& actual) const {
        if (&actual != identity || actual.id != value.id || actual.name != value.name ||
            actual.enumeration != value.enumeration || actual.kind != value.kind || actual.math != value.math ||
            actual.plain_value != value.plain_value || actual.base != value.base || actual.size != value.size ||
            actual.alignment != value.alignment || actual.copy_constructible != value.copy_constructible ||
            actual.copy_assignable != value.copy_assignable ||
            actual.contains_references != value.contains_references || actual.element != value.element ||
            actual.native_storage != value.native_storage || actual.sequence_view != value.sequence_view ||
            actual.sequence_mutate != value.sequence_mutate ||
            actual.value_operations_preserve_borrows != value.value_operations_preserve_borrows ||
            actual.fields.size() != value.fields.size() || actual.methods.size() != value.methods.size())
            return false;
        for (std::size_t i = 0; i < value.fields.size(); ++i) {
            const auto& a = actual.fields[i];
            const auto& b = value.fields[i];
            if (a.id != b.id || a.name != b.name || a.value != b.value || a.offset != b.offset ||
                !same_leaves(a.default_leaves, b.default_leaves))
                return false;
        }
        for (std::size_t i = 0; i < value.methods.size(); ++i) {
            const auto& a = actual.methods[i];
            const auto& b = value.methods[i];
            if (a.id != b.id || a.name != b.name || a.signature != b.signature ||
                a.preserves_borrows != b.preserves_borrows)
                return false;
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
        : binding(bind_type(runtime, type, catalog)), type_name(binding.name), actual_services(services),
          declarations(std::move(source_types)) {
        binding.name = type_name;
        for (const auto dependency : binding.dependencies) {
            const TypeInfo* info{};
            if (catalog.empty())
                info = take(runtime.describe(dependency));
            else
                for (const auto* candidate : catalog)
                    if (candidate->id == dependency) {
                        info = candidate;
                        break;
                    }
            if (!info)
                throw std::logic_error("Source type dependency has no Runtime metadata.");
            metadata.push_back({info, *info});
        }
    }

    std::optional<ValueDescription> value_description(BusinessType kind) const noexcept {
        const auto type = value_type_id(binding, kind);
        if (type != TypeId{})
            return ValueDescription{type};
        return std::nullopt;
    }

    Result<FunctionSignature> resolve_signature(const BusinessCodeInfo& info,
                                                atelier::detail::TypePublication* publication = nullptr) const {
        const auto reference = [&](TypeId type) -> TypeReference {
            if (publication) {
                for (const auto& item : metadata)
                    if (item.value.id == type)
                        return TypeReference{item.value.name};
            }
            return type;
        };
        SignatureDefinition definition;
        definition.execution =
            info.signature.is_coroutine ? FunctionExecution::coroutine : FunctionExecution::synchronous;
        definition.parameters.reserve(info.signature.parameter_count);
        for (std::uint32_t i = 0; i < info.signature.parameter_count; ++i) {
            const auto value = value_description(info.signature.parameters[i].type);
            if (!value)
                return std::unexpected(Error{ErrorCode::invalid_definition, "Unsupported compiled source parameter."});
            definition.parameters.push_back(
                {{reference(value->type), value->form}, info.signature.parameters[i].passing});
        }
        if (info.signature.result != ValueKind::void_value) {
            const auto value = value_description(info.signature.result);
            if (!value)
                return std::unexpected(Error{ErrorCode::invalid_definition, "Unsupported compiled source result."});
            definition.result = ValueDefinition{reference(value->type), value->form};
        }
        if (info.requires_receiver)
            definition.receiver = ReceiverDefinition{reference(binding.type)};
        return publication ? publication->resolve_signature(*binding.runtime, definition)
                           : binding.runtime->resolve_signature(definition);
    }

    Result<void> compatible(BusinessCode* code, std::string_view source_name,
                            const Declaration* declaration = nullptr) const {
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
        if (!same_environment(binding, info.environment))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate changed its bound member catalog."});
        if (layout.strong_slots < business_root_slots || layout.strong_slots >= observation_slot_bit ||
            layout.weak_slots >= observation_slot_bit)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Candidate has an invalid invocation-slot layout."});
        return {};
    }

    Result<void> unchanged() const {
        std::vector<const TypeInfo*> catalog;
        catalog.reserve(metadata.size());
        for (const auto& dependency : metadata) {
            ATELIER_TRY_BIND(info, binding.runtime->describe(dependency.value.id));
            if (!dependency.matches(**info))
                return std::unexpected(Error{ErrorCode::invalid_definition, "Bound Runtime metadata changed."});
            catalog.push_back(*info);
        }
        // Other producers may append unrelated types. Revalidate this program's
        // captured catalog without changing its compiled type-table indices.
        const auto actual = bind_type(*binding.runtime, binding.type, catalog);
        if (actual.runtime != binding.runtime || actual.type != binding.type || actual.name != type_name ||
            actual.fields != binding.fields || actual.methods != binding.methods ||
            actual.enumerations != binding.enumerations || actual.values != binding.values ||
            actual.dependencies != binding.dependencies)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Bound member identities changed."});
        return {};
    }
};

struct Program::Version {
    std::shared_ptr<const Contract> contract;

    std::shared_ptr<const Declaration> declaration;

    std::uint64_t sequence;

    // Shared: a project unit compiled from identical input reuses its code in the next Version.
    std::shared_ptr<BusinessCode> code;

    BusinessCodeInfo info;

    BusinessFrameLayout layout;

    std::shared_ptr<const DiagnosticMetadata> diagnostic;

    std::shared_ptr<const DebugMetadata> debug;

    std::shared_ptr<const SourceMap> map;

    Version(std::shared_ptr<const Contract> bound, std::shared_ptr<const Declaration> declared, std::uint64_t request,
            std::shared_ptr<const DiagnosticMetadata> capture, std::shared_ptr<const DebugMetadata> session,
            std::shared_ptr<BusinessCode> owner, std::shared_ptr<const SourceMap> source_map = {}) noexcept
        : contract(std::move(bound)), declaration(std::move(declared)), sequence(request), code(std::move(owner)),
          info(atelier_business_info(code.get())), layout(atelier_business_layout(code.get())),
          diagnostic(std::move(capture)), debug(std::move(session)), map(std::move(source_map)) {}

    // Generated failure exits report a site index; resolving it allocates, so it
    // happens only after a failure.
    void locate(Error& error, std::uint32_t site) const noexcept {
        if (site >= info.location_count)
            return;
        try {
            error.source =
                resolve_location(map.get(), diagnostic->source_name, diagnostic->code_version, info.locations[site]);
        } catch (...) {
        }
    }

    // Keep captures on this stack until the body has released its arguments.
    // The caller pins this Version; successful starts transfer captures to their frame.
    template <class T, class F>
    Result<T> invoke_captured(Diagnostics* diagnostics, Debugger* debugger, F&& body) const {
        auto activation = ExecutionScope::enter(*contract->binding.runtime);
        if (!activation)
            return std::unexpected(reject_invocation(diagnostics, debugger, std::move(activation.error())));
        auto capture = DiagnosticAccess::begin(diagnostics, diagnostic, *contract->binding.runtime, info.diagnostics);
        if (!capture)
            return std::unexpected(DebugAccess::reject(debugger, std::move(capture.error())));
        DiagnosticScope scope(std::move(*capture), *contract->binding.runtime);
        auto session = DebugAccess::begin(debugger, debug, info.debugging);
        if (!session)
            return scope.complete(Result<T>{std::unexpected(std::move(session.error()))});
        DebugScope debug_scope(std::move(*session));
        auto result = body(scope, debug_scope);
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
            [&](DiagnosticScope& scope, DebugScope& debug_scope) -> Result<std::optional<Scalar>> {
                if (aggregate(info.signature.result))
                    return std::unexpected(Error{ErrorCode::argument_mismatch,
                                                 "Aggregate source results require typed value output storage."});
                const AbiArguments encoded(contract->binding, declaration->signature, arguments);
                if (!encoded.status)
                    return std::unexpected(encoded.status.error());
                ATELIER_TRY_BIND(result,
                                 invoke(receiver, encoded.values, {}, nullptr, scope.trace(), debug_scope.state()));
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
            diagnostics, debugger, [&](DiagnosticScope& scope, DebugScope& debug_scope) -> Result<void> {
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
                const AbiArguments encoded(binding, declaration->signature, arguments);
                if (!encoded.status)
                    return std::unexpected(encoded.status.error());
                // Only the returned representation enters this buffer. Constructing a
                // native value here would execute an unrelated default constructor.
                ::atelier::detail::ValueStorage staging_owner;
                auto* staged = staging_owner.prepare(value.size, value.alignment);
                ATELIER_TRY_BIND(result,
                                 invoke(receiver, encoded.values, {}, staged, scope.trace(), debug_scope.state()));
                if (result->kind != kind || result->storage != ArgumentStorage::value || result->data != staged)
                    return std::unexpected(Error{ErrorCode::invocation_failed,
                                                 "Compiled aggregate result changed its typed output storage."});
                std::memcpy(output.data, staged, value.size);
                return {};
            });
    }

    static Result<Coroutine> start_values(std::shared_ptr<const Version> version, ObjectId receiver,
                                          std::span<const Scalar> arguments, Diagnostics* diagnostics,
                                          Debugger* debugger) {
        return version->invoke_captured<Coroutine>(
            diagnostics, debugger, [&](DiagnosticScope& scope, DebugScope& debug_scope) -> Result<Coroutine> {
                const AbiArguments encoded(version->contract->binding, version->declaration->signature, arguments);
                if (!encoded.status)
                    return std::unexpected(encoded.status.error());
                return ProgramAccess::start_version(version, receiver, encoded.values, {}, scope.state(),
                                                    debug_scope.state());
            });
    }

    Result<BusinessValue> invoke(ObjectId receiver, std::span<const BusinessValue> arguments,
                                 InvocationObserver observer, void* result_data = nullptr,
                                 BusinessTrace* trace = nullptr,
                                 std::shared_ptr<DebugSessionState> debugger = {}) const {
        if (info.signature.is_coroutine)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Coroutine code requires a persistent execution instance."});
        if (aggregate(info.signature.result) && !result_data)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Aggregate source results require typed value output storage."});
        const auto& binding = contract->binding;
        ATELIER_TRY_BIND(activation, ExecutionScope::enter(*binding.runtime));
        ATELIER_TRY(check_source_arguments(binding, info.signature, arguments));
        std::uint32_t site = no_diagnostic_location;
        auto result = execute_admitted(*activation, *binding.runtime, binding, code.get(), layout,
                                       declaration->signature.receiver.has_value(), receiver, arguments, observer,
                                       result_data, trace, std::move(debugger), &site);
        if (!result)
            locate(result.error(), site);
        return result;
    }
};
#include "source/detail/module_state.inl"

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

    std::shared_ptr<const Version> select(std::optional<std::size_t> index) const {
        if (!index)
            return module ? nullptr : current;
        return module && *index < module->versions.size() ? module->versions[*index] : nullptr;
    }
};

struct Program::Frame final : CoroutineFrame {
    std::shared_ptr<const Version> version;

    std::shared_ptr<DiagnosticState> diagnostic;

    Invocation invocation;

    std::unique_ptr<BusinessCoroutine, decltype(&atelier_destroy_business_coroutine)> code_frame{
        nullptr, &atelier_destroy_business_coroutine};

    bool observed_terminal{};

    bool captures_owned{};

    DiagnosticStatus terminal{DiagnosticStatus::cancelled};

    DebugStatus debug_terminal{DebugStatus::cancelled};

    Frame(std::shared_ptr<const Version> owner, InvocationObserver observer, std::shared_ptr<DiagnosticState> capture,
          std::shared_ptr<DebugSessionState> debugger)
        : version(std::move(owner)), diagnostic(std::move(capture)),
          invocation(*version->contract->binding.runtime, version->contract->binding, version->layout, observer,
                     diagnostic ? &diagnostic->trace : nullptr, std::move(debugger)) {}

    ~Frame() override {
        // Generated destruction can call host cleanup, so retain Invocation and
        // Version until the DLL frame has finished using them.
        if (diagnostic)
            DiagnosticAccess::running(*diagnostic);
        if (invocation.debugger)
            DebugAccess::status(*invocation.debugger, DebugStatus::running);
        const bool generated = code_frame != nullptr;
        code_frame.reset();
        complete(false);
        if (!diagnostic && !invocation.debugger)
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
            if (invocation.debugger)
                DebugAccess::finish(*invocation.debugger, debug_terminal);
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

    void complete(bool success) noexcept {
        if (observed_terminal)
            return;
        observed_terminal = true;
        const auto observer = invocation.observer;
        if (observer.complete)
            observer.complete(observer.context, invocation, success);
    }

    Result<CoroutineStep> failure(Error error) {
        if (diagnostic)
            DiagnosticAccess::fail(*diagnostic, error);
        return std::unexpected(std::move(error));
    }

    Result<CoroutineStep> resume() override {
        if (diagnostic)
            DiagnosticAccess::running(*diagnostic, true);
        if (invocation.debugger)
            DebugAccess::status(*invocation.debugger, DebugStatus::running);
        char message[512];
        bool suspended{};
        BusinessValue output{};
        std::uint32_t site = no_diagnostic_location;
        const auto status =
            atelier_resume_business_coroutine(code_frame.get(), &suspended, &output, message, sizeof(message), &site);
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
        if (terminal_step)
            complete(cleared && status == 0 && !invocation.failed && !invalid_result);
        if (status != 0 && !invocation.failed && invocation.debug_aborted) {
            if (!cleared)
                return failure({ErrorCode::invocation_failed, "Aborted coroutine left execution resources alive."});
            request_cancel();
            return CoroutineStep{CoroutineState::completed};
        }
        if (status != 0 || invocation.failed) {
            Error error{invocation.failed ? invocation.error_code : ErrorCode::invocation_failed,
                        invocation.failed ? invocation.error : message};
            version->locate(error, site);
            return failure(std::move(error));
        }
        if (terminal_step && !cleared)
            return failure({ErrorCode::invocation_failed, "Coroutine left execution resources alive."});
        if (suspended) {
            if (diagnostic)
                DiagnosticAccess::pause(*diagnostic, invocation.runtime, DiagnosticStatus::suspended);
            if (invocation.debugger)
                DebugAccess::status(*invocation.debugger, DebugStatus::suspended);
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

Result<Coroutine> ProgramAccess::start_version(std::shared_ptr<const Program::Version> version, ObjectId receiver,
                                               std::span<const BusinessValue> arguments, InvocationObserver observer,
                                               std::shared_ptr<DiagnosticState> capture,
                                               std::shared_ptr<DebugSessionState> debugger) {
    if (!version || !version->info.signature.is_coroutine)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Starting a coroutine requires a coroutine declaration."});
    const auto& binding = version->contract->binding;
    ATELIER_TRY_BIND(activation, ExecutionScope::enter(*binding.runtime));
    ATELIER_TRY(check_source_arguments(binding, version->info.signature, arguments));
    const bool hold_receiver = version->declaration->signature.receiver.has_value() || receiver != ObjectId{};
    if (hold_receiver) {
        ATELIER_TRY(check_source_receiver(binding, receiver));
    }
    auto frame =
        std::make_unique<Program::Frame>(std::move(version), observer, std::move(capture), std::move(debugger));
    if (hold_receiver) {
        ATELIER_TRY(frame->invocation.frame.hold(receiver_slot, receiver, binding.type));
    }
    BusinessCoroutine* compiled{};
    char diagnostic[512]{};
    if (atelier_create_business_coroutine(frame->version->code.get(), &frame->invocation, arguments.data(),
                                          static_cast<std::uint32_t>(arguments.size()), &compiled, diagnostic,
                                          sizeof(diagnostic), frame->invocation.trace) != 0)
        return std::unexpected(Error{ErrorCode::invocation_failed, diagnostic});
    frame->code_frame.reset(compiled);
    if (frame->diagnostic)
        frame->invocation.record_resources(frame->invocation.resources_cleared());
    auto* observed = frame.get();
    auto result = Coroutine::create(*binding.runtime, std::move(frame));
    if (result)
        observed->captures_owned = true;
    return result;
}

Program::SourceRequest::SourceRequest(std::weak_ptr<State> owner, std::shared_ptr<const Contract> contract,
                                      std::shared_ptr<const Declaration> declaration, std::uint64_t number,
                                      std::string name, std::string text, std::string entry, CompileOptions compilation)
    : owner_(std::move(owner)), contract_(std::move(contract)), compilation_contract_(contract_),
      declaration_(std::move(declaration)), sequence(number), source_name(std::move(name)), source(std::move(text)),
      entry_name(std::move(entry)), options(compilation) {}

Program::Candidate::Candidate() = default;

Program::Candidate::Candidate(std::unique_ptr<Prepared> prepared) : prepared_(std::move(prepared)) {}

Program::Candidate::~Candidate() = default;

Program::Candidate::Candidate(Candidate&&) noexcept = default;

Program::Candidate& Program::Candidate::operator=(Candidate&&) noexcept = default;

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

std::uintptr_t Program::Candidate::address() const noexcept {
    return *this ? atelier_business_info(prepared_->code.get()).entry_address : 0;
}

Program::Lease::Lease(std::shared_ptr<const Version> version) : version_(std::move(version)) {}

Program::Entry::Entry(std::weak_ptr<State> owner, std::optional<std::size_t> index)
    : owner_(std::move(owner)), index_(index) {}

std::uintptr_t Program::Lease::address() const noexcept {
    return version_ ? version_->info.entry_address : 0;
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

Result<BusinessValue> ProgramAccess::invoke(const Program::Lease& lease, ObjectId receiver,
                                            std::span<const BusinessValue> arguments, InvocationObserver observer) {
    const auto version = lease.version_;
    if (!version)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Code lease is empty."});
    // Callback destruction of the lease is safe: only the local pin follows.
    return version->invoke(receiver, arguments, observer);
}

Result<BusinessValue> ProgramAccess::invoke(const Program::Entry& entry, ObjectId receiver,
                                            std::span<const BusinessValue> arguments, InvocationObserver observer) {
    const auto owner = entry.owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    const auto version = owner->select(entry.index_);
    if (!version)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no published version."});
    // Select once and never access the entry or program after callbacks start.
    return version->invoke(receiver, arguments, observer);
}

Result<Coroutine> ProgramAccess::start(const Program::Lease& lease, ObjectId receiver,
                                       std::span<const BusinessValue> arguments, InvocationObserver observer) {
    return start_version(lease.version_, receiver, arguments, observer);
}

Result<Coroutine> ProgramAccess::start(const Program::Entry& entry, ObjectId receiver,
                                       std::span<const BusinessValue> arguments, InvocationObserver observer) {
    const auto owner = entry.owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    return start_version(owner->select(entry.index_), receiver, arguments, observer);
}

BusinessFrameLayout ProgramAccess::layout(const Program::Lease& lease) noexcept {
    return lease.version_ ? lease.version_->layout : BusinessFrameLayout{};
}

Result<std::optional<Scalar>> Program::Entry::invoke(ObjectId receiver, std::span<const Scalar> arguments,
                                                     Diagnostics* diagnostics, Debugger* debugger) const {
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return version->invoke_values(receiver, arguments, diagnostics, debugger);
}

Result<std::optional<Scalar>> Program::Entry::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
                                                               Diagnostics* diagnostics, Debugger* debugger) const {
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return version->invoke_values(receiver, arguments, diagnostics, debugger);
}

Result<std::optional<Scalar>> Program::Lease::invoke(ObjectId receiver, std::span<const Scalar> arguments,
                                                     Diagnostics* diagnostics, Debugger* debugger) const {
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return version->invoke_values(receiver, arguments, diagnostics, debugger);
}

Result<std::optional<Scalar>> Program::Lease::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
                                                               Diagnostics* diagnostics, Debugger* debugger) const {
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return version->invoke_values(receiver, arguments, diagnostics, debugger);
}

Result<void> Program::Entry::invoke(ObjectId receiver, std::span<const Scalar> arguments, ValueOutput output,
                                    Diagnostics* diagnostics, Debugger* debugger) const {
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return version->invoke_values(receiver, arguments, output, diagnostics, debugger);
}

Result<void> Program::Entry::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
                                              ValueOutput output, Diagnostics* diagnostics, Debugger* debugger) const {
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return version->invoke_values(receiver, arguments, output, diagnostics, debugger);
}

Result<void> Program::Lease::invoke(ObjectId receiver, std::span<const Scalar> arguments, ValueOutput output,
                                    Diagnostics* diagnostics, Debugger* debugger) const {
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return version->invoke_values(receiver, arguments, output, diagnostics, debugger);
}

Result<void> Program::Lease::invoke_arguments(ObjectId receiver, std::span<const Argument> arguments,
                                              ValueOutput output, Diagnostics* diagnostics, Debugger* debugger) const {
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return version->invoke_values(receiver, arguments, output, diagnostics, debugger);
}

Result<Coroutine> Program::Entry::start(ObjectId receiver, std::span<const Scalar> arguments, Diagnostics* diagnostics,
                                        Debugger* debugger) const {
    const auto owner = owner_.lock();
    const auto version = owner && owner->open ? owner->select(index_) : nullptr;
    if (!version)
        return std::unexpected(reject_invocation(
            diagnostics, debugger, {ErrorCode::invalid_definition, "Business entry has no published version."}));
    return Version::start_values(version, receiver, arguments, diagnostics, debugger);
}

Result<Coroutine> Program::Lease::start(ObjectId receiver, std::span<const Scalar> arguments, Diagnostics* diagnostics,
                                        Debugger* debugger) const {
    const auto version = version_;
    if (!version)
        return std::unexpected(
            reject_invocation(diagnostics, debugger, {ErrorCode::invalid_definition, "Code lease is empty."}));
    return Version::start_values(version, receiver, arguments, diagnostics, debugger);
}

Program::Program(Runtime& runtime, TypeId type)
    : state_(std::make_shared<State>(std::make_shared<const Contract>(runtime, type))) {}

Program::~Program() {
    close();
}

void Program::close() noexcept {
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

Result<Program::Entry> Program::bind_signature(SignatureDefinition expected) const {
    const auto owner = state_;
    if (!owner->open || !owner->declaration)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Business source declaration has not been published."});
    if (const auto& receiver = owner->declaration->signature.receiver)
        expected.receiver = ReceiverDefinition{receiver->type, receiver->read_only};
    ATELIER_TRY_BIND(resolved, owner->contract->binding.runtime->resolve_signature(expected));
    if (*resolved != owner->declaration->signature)
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Host binding does not match the published source signature."});
    return Entry{owner};
}

Result<Program::Lease> Program::pin() const {
    if (!state_->open || !state_->current)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business entry has no version to pin."});
    return Lease{state_->current};
}

Result<Program::Request> Program::request(std::string_view name, std::string_view source, std::string_view entry_name,
                                          CompileOptions options) {
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
    if (declarations->size() == previous.size())
        return result;
    auto definitions = type_definitions(*declarations, state_->contract->binding);
    if (!definitions) {
        report(definitions.error().message);
        return result;
    }
    std::erase_if(*definitions, [&](const auto& definition) { return published(definition.name); });
    auto& runtime = *state_->contract->binding.runtime;
    auto publication = atelier::detail::TypePublication::prepare(runtime, std::move(*definitions));
    if (!publication) {
        report(publication.error().message);
        return result;
    }
    result->type_publication_ = *publication;
    result->compilation_contract_ = std::make_shared<const Contract>(
        runtime, state_->contract->binding.type, (*publication)->catalog(), std::move(*declarations));
    return result;
}

Result<Program::Candidate> Program::prepare(const Request& request) {
    return ProgramAccess::prepare(request, &atelier_compile_business);
}

Result<Program::Candidate> ProgramAccess::prepare(const Program::Request& request, Compiler compile) {
    if (!request)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source request is empty."});
    if (!compile)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source compiler is missing."});
    const auto owner = request->owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source request belongs to a closed program."});
    if (request->declaration_error_)
        return std::unexpected(*request->declaration_error_);
    if (request->module_)
        return Program::prepare_module(request);
    BusinessCode* output{};
    char diagnostic[2048];
    const auto& contract = *request->compilation_contract_;
    const EnvironmentView storage(contract.binding);
    const auto environment = storage.view(contract.type_name.c_str());
    const auto status = compile(&contract.actual_services, request->source_name.c_str(), request->source.data(),
                                static_cast<std::uint32_t>(request->source.size()), &environment, &output, diagnostic,
                                sizeof(diagnostic), request->entry_name.c_str(), request->options);
    CodeOwner code{output, &atelier_destroy_business};
    if (status != 0)
        return std::unexpected(Error{ErrorCode::invalid_definition, diagnostic});
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
    ATELIER_TRY(
        prepared.compilation_contract->compatible(prepared.code.get(), prepared.source_name, owner->declaration.get()));
    ATELIER_TRY(owner->contract->binding.runtime->check_code_publication());
    ATELIER_TRY(owner->contract->unchanged());
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
    // Allocation precedes the noexcept constructor's transfer of candidate code.
    auto version = std::make_shared<const Version>(prepared.compilation_contract, declaration, prepared.sequence,
                                                   prepared.diagnostic, prepared.debug, std::move(prepared.code));
    if (prepared.type_publication)
        prepared.type_publication->commit(*owner->contract->binding.runtime);
    owner->contract = prepared.compilation_contract;
    owner->declaration = std::move(declaration);
    owner->published_request = prepared.sequence;
    ++owner->revision;
    owner->current = std::move(version);
    return {};
}
#include "source/detail/program_module.inl"

Result<void> ProgramAccess::compatible(const Program& program, BusinessCode* code, std::string_view source_name) {
    return program.state_->contract->compatible(code, source_name);
}

Result<void> ProgramAccess::unchanged(const Program& program) {
    return program.state_->contract->unchanged();
}

void ProgramAccess::set_request_sequence(Program& program, std::uint64_t sequence) noexcept {
    program.state_->latest_request = sequence;
}

std::uint64_t ProgramAccess::published_sequence(const Program& program) noexcept {
    return program.state_->published_request;
}
}
