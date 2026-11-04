#pragma once

#include "foundation/result.hpp"
#include "runtime/execution.hpp"
#include "source/snapshot.hpp"
#include "source/diagnostics.hpp"
#include "source/debugger.hpp"
#include <bit>
#include <tuple>
#include <type_traits>

namespace atelier::source {
namespace detail {
struct ProgramAccess;

struct BusinessFailure;

struct BusinessTrace;
}

// A selected entry or a project snapshot with atomically replaceable code. All operations use
// the Runtime thread. Runtime and the backend DLL outlive programs, leases and
// nonterminal coroutines. Closing a program invalidates entries; leases and
// already-created coroutines retain their own version.
class Program final {
    struct Metadata;

    struct Contract;

    struct Declaration;

    struct Version;

    struct State;

    struct Prepared;

    struct Frame;

    struct ModuleRequest;

    struct ModuleVersion;

    friend struct detail::ProgramAccess;

    template <class T>
    static constexpr bool direct_scalar = std::same_as<T, std::int32_t> || std::same_as<T, std::uint32_t> ||
                                          std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, bool>;

    template <class T>
    using DirectScalar = std::conditional_t<std::same_as<T, bool>, std::uint32_t, T>;

    using DirectCall = std::uint32_t (*)(std::uintptr_t, void*, void*, std::uint64_t*, detail::BusinessFailure*,
                                         detail::BusinessTrace*) noexcept;

    static Result<std::uint64_t> invoke_bound(std::shared_ptr<const Version>, std::uintptr_t, ObjectId, void*,
                                              DirectCall, Diagnostics*, Debugger*);

public:
    class Lease;

    // Owns one checked synchronous signature and Version, not the receiver or Runtime.
    // Each invocation pins its Version before callbacks may release this handle.
    template <class R, class... Args>
        requires(std::is_void_v<R> || direct_scalar<R>) && (direct_scalar<Args> && ...)
    class Typed {
        friend class Lease;

        std::shared_ptr<const Version> version_;

        std::uintptr_t address_{};

        Typed(std::shared_ptr<const Version> version, std::uintptr_t address)
            : version_(std::move(version)), address_(address) {}

    public:
        Typed() = default;

        Result<R> invoke(ObjectId receiver, Args... arguments, Diagnostics* diagnostics = nullptr,
                         Debugger* debugger = nullptr) const {
            std::tuple<Args...> values{arguments...};
            const auto call = [](std::uintptr_t address, void* context, void* invocation, std::uint64_t* output,
                                 detail::BusinessFailure* failure, detail::BusinessTrace* trace) noexcept {
                using Entry = std::uint32_t (*)(void*, DirectScalar<Args>..., std::uint64_t*, detail::BusinessFailure*,
                                                detail::BusinessTrace*) noexcept;
                const auto entry = reinterpret_cast<Entry>(address);
                return std::apply(
                    [&](Args... values) {
                        return entry(invocation, static_cast<DirectScalar<Args>>(values)...,
                                     std::is_void_v<R> ? nullptr : output, failure, trace);
                    },
                    *static_cast<std::tuple<Args...>*>(context));
            };
            ATELIER_TRY_BIND(result, invoke_bound(version_, address_, receiver, &values, call, diagnostics, debugger));
            if constexpr (std::is_void_v<R>)
                return {};
            else if constexpr (std::same_as<R, double>)
                return std::bit_cast<double>(*result);
            else if constexpr (std::same_as<R, bool>)
                return *result != 0;
            else
                return std::bit_cast<R>(static_cast<std::uint32_t>(*result));
        }
    };

    class SourceRequest {
        friend class Program;

        friend struct detail::ProgramAccess;

        std::weak_ptr<State> owner_;

        std::shared_ptr<const Contract> contract_;

        std::shared_ptr<const Contract> compilation_contract_;

        std::shared_ptr<atelier::detail::TypePublication> type_publication_;

        std::optional<Error> declaration_error_;

        std::shared_ptr<const Declaration> declaration_;

        std::shared_ptr<ModuleRequest> module_;

        std::string source_path_; // Original single-input name, without the compiler's request display suffix.

        SourceRequest(std::weak_ptr<State>, std::shared_ptr<const Contract>, std::shared_ptr<const Declaration>,
                      std::uint64_t, std::string, std::string, std::string, CompileOptions);

    public:
        const std::uint64_t sequence;

        const std::string source_name;

        const std::string source;

        const std::string entry_name;

        const CompileOptions options;

        std::shared_ptr<const SourceSnapshot> snapshot() const;
    };

    using Request = std::shared_ptr<const SourceRequest>;

    class Candidate {
        friend class Program;

        friend struct detail::ProgramAccess;

        std::unique_ptr<Prepared> prepared_;

        explicit Candidate(std::unique_ptr<Prepared>);

    public:
        Candidate();

        ~Candidate();

        Candidate(Candidate&&) noexcept;

        Candidate& operator=(Candidate&&) noexcept;

        Candidate(const Candidate&) = delete;

        Candidate& operator=(const Candidate&) = delete;

        explicit operator bool() const noexcept;

        // Project metadata for host validation before commit. Preview class IDs
        // are not live Runtime identities; base IDs already name native types.
        // Views expire on move, destruction or successful publication.
        std::span<const ClassInfo> classes() const noexcept;

        std::span<const EntryInfo> entries() const noexcept;
    };

    class Lease {
        friend class Program;

        friend struct detail::ProgramAccess;

        std::shared_ptr<const Version> version_;

        explicit Lease(std::shared_ptr<const Version>);

    public:
        Lease() = default;

        // Binding checks the shared signature once. No generic-argument fallback.
        template <class R, class... Args>
            requires(std::is_void_v<R> || direct_scalar<R>) && (direct_scalar<Args> && ...)
        Result<Typed<R, Args...>> bind() const {
            SignatureDefinition expected;
            expected.parameters = {ParameterDefinition{{TypeReference::native<Args>()}, ParameterPassing::value}...};
            if constexpr (!std::is_void_v<R>)
                expected.result = ValueDefinition{TypeReference::native<R>()};
            ATELIER_TRY_BIND(address, bind_signature(std::move(expected)));
            return Typed<R, Args...>{version_, *address};
        }

        Result<std::optional<Scalar>> invoke(ObjectId, std::span<const Scalar>, Diagnostics* = nullptr,
                                             Debugger* = nullptr) const;

        // Math and plain record results require live, aligned storage of the exact declared type.
        // Failure preserves the complete output; scalar results use the overload above.
        Result<void> invoke(ObjectId, std::span<const Scalar>, ValueOutput, Diagnostics* = nullptr,
                            Debugger* = nullptr) const;

        template <std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<std::optional<Scalar>> invoke(ObjectId object, const R& arguments, Diagnostics* diagnostics = nullptr,
                                             Debugger* debugger = nullptr) const {
            return invoke_arguments(object, std::span<const Argument>{arguments}, diagnostics, debugger);
        }

        template <std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<void> invoke(ObjectId object, const R& arguments, ValueOutput output, Diagnostics* diagnostics = nullptr,
                            Debugger* debugger = nullptr) const {
            return invoke_arguments(object, std::span<const Argument>{arguments}, output, diagnostics, debugger);
        }

    private:
        Result<std::uintptr_t> bind_signature(SignatureDefinition) const;

        Result<std::optional<Scalar>> invoke_arguments(ObjectId, std::span<const Argument>, Diagnostics*,
                                                       Debugger*) const;

        Result<void> invoke_arguments(ObjectId, std::span<const Argument>, ValueOutput, Diagnostics*, Debugger*) const;

    public:
        Result<Coroutine> start(ObjectId, std::span<const Scalar>, Diagnostics* = nullptr, Debugger* = nullptr) const;

        Result<CompilationArtifacts> artifacts() const;

        Result<std::vector<DiagnosticLocation>> debug_sites() const;
    };

    class Entry {
        friend class Program;

        friend struct detail::ProgramAccess;

        std::weak_ptr<State> owner_;

        std::optional<std::size_t> index_;

        explicit Entry(std::weak_ptr<State>, std::optional<std::size_t> = {});

    public:
        Entry() = default;

        Result<std::optional<Scalar>> invoke(ObjectId, std::span<const Scalar>, Diagnostics* = nullptr,
                                             Debugger* = nullptr) const;

        Result<void> invoke(ObjectId, std::span<const Scalar>, ValueOutput, Diagnostics* = nullptr,
                            Debugger* = nullptr) const;

        template <std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<std::optional<Scalar>> invoke(ObjectId object, const R& arguments, Diagnostics* diagnostics = nullptr,
                                             Debugger* debugger = nullptr) const {
            return invoke_arguments(object, std::span<const Argument>{arguments}, diagnostics, debugger);
        }

        template <std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<void> invoke(ObjectId object, const R& arguments, ValueOutput output, Diagnostics* diagnostics = nullptr,
                            Debugger* debugger = nullptr) const {
            return invoke_arguments(object, std::span<const Argument>{arguments}, output, diagnostics, debugger);
        }

    private:
        Result<std::optional<Scalar>> invoke_arguments(ObjectId, std::span<const Argument>, Diagnostics*,
                                                       Debugger*) const;

        Result<void> invoke_arguments(ObjectId, std::span<const Argument>, ValueOutput, Diagnostics*, Debugger*) const;

    public:
        Result<Coroutine> start(ObjectId, std::span<const Scalar>, Diagnostics* = nullptr, Debugger* = nullptr) const;
    };

    // TypeId{} declares a receiverless typed value environment. Binding errors are
    // reported by exception during construction; execution errors use Result.
    Program(Runtime&, TypeId = {});

    ~Program();

    // Idempotently closes entries, requests and publication. Existing leases
    // and coroutines retain their version; their owner must finish/cancel them
    // before destroying the Runtime. Safe during a pinned active invocation.
    void close() noexcept;

    Program(const Program&) = delete;

    Program& operator=(const Program&) = delete;

    Entry entry() const;

    Result<Entry> entry(std::string_view name) const;

    Result<Lease> pin(std::string_view name) const;

    Result<FunctionSignature> describe(std::string_view name) const;

    // Views remain valid until the next successful publication or close.
    std::span<const ClassInfo> classes() const noexcept;

    std::span<const EntryInfo> entries() const noexcept;

    // Counts successful publications; 0 before the first.
    std::uint64_t revision() const noexcept;

    Result<FunctionSignature> describe() const;

    template <class R, class... Args>
        requires((!std::is_rvalue_reference_v<Args> && !std::is_volatile_v<std::remove_reference_t<Args>>) && ...) &&
                (!std::is_reference_v<R>)
    Result<Entry> bind(FunctionExecution execution = FunctionExecution::synchronous) const {
        SignatureDefinition expected;
        expected.execution = execution;
        expected.parameters = {
            ParameterDefinition{{TypeReference::native<std::remove_cvref_t<Args>>()}, parameter_passing<Args>}...};
        if constexpr (!std::is_void_v<R>)
            expected.result = ValueDefinition{TypeReference::native<R>()};
        return bind_signature(std::move(expected));
    }

    Result<Lease> pin() const;

    // Requests collect source type declarations and capture unpublished Runtime
    // layouts. Preparation reports declaration/body errors without publishing types.
    // Requests own the module source, entry selection and metadata, but no code.
    // Multiple functions require an entry name before
    // first publication; later requests default to the frozen declaration name.
    Result<Request> request(std::string_view name, std::string_view source, std::string_view entry_name = {},
                            CompileOptions = {});

    Result<Request> request(SourceSnapshot, CompileOptions = {});

    static Result<Candidate> prepare(const Request&);

    // Busy does not consume the candidate. Publishing requires the latest
    // request, unchanged dependencies and the declaration's frozen signature.
    Result<void> try_publish(Candidate&);

private:
    static Result<Candidate> prepare_module(const Request&);

    Result<void> publish_module(Candidate&);

    Result<Entry> bind_signature(SignatureDefinition) const;

    std::shared_ptr<State> state_;
};
}
