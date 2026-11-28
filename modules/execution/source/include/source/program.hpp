#pragma once

#include "foundation/result.hpp"
#include "foundation/function_ref.hpp"
#include "source/entry_dependencies.hpp"
#include "source/compile_timing.hpp"
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

struct BusinessCode;

struct BusinessFailure;

struct BusinessTrace;
}

// A selected entry or a project snapshot with atomically replaceable code. Except for
// emit_entry, operations use the Runtime thread. Runtime and the backend DLL outlive programs, leases and
// nonterminal coroutines. Closing a program invalidates entries; leases and
// already-created coroutines retain their own version.
class Program final {
    struct Contract;

    struct Declaration;

    struct Version;

    struct State;

    struct Prepared;

    struct Frame;

    struct ModuleRequest;

    struct ModuleVersion;

    template <class T>
    static constexpr bool direct_scalar = std::same_as<T, std::int32_t> || std::same_as<T, std::uint32_t> ||
                                          std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, bool>;

    template <class T, bool IsEnum = std::is_enum_v<T>>
    struct DirectEnum : std::false_type {};

    template <class T>
    struct DirectEnum<T, true> : std::bool_constant<std::same_as<std::underlying_type_t<T>, std::int32_t> ||
                                                    std::same_as<std::underlying_type_t<T>, std::uint32_t>> {};

    template <class T>
    static constexpr bool direct_enum = DirectEnum<T>::value;

    template <class T>
    static constexpr bool direct_value = direct_scalar<T> || direct_enum<T>;

    template <class T>
    static constexpr bool direct_parameter =
        direct_value<T> || (std::is_lvalue_reference_v<T> && !std::is_volatile_v<std::remove_reference_t<T>>);

    template <class T>
    using DirectAbi = std::conditional_t<std::is_lvalue_reference_v<T>, const void*,
                                         std::conditional_t<std::same_as<T, bool> || direct_enum<T>, std::uint32_t, T>>;

    template <class T>
    static DirectAbi<T> direct_abi(const std::remove_reference_t<T>& value) noexcept {
        if constexpr (std::is_lvalue_reference_v<T>)
            return std::addressof(value);
        else if constexpr (std::same_as<T, bool>)
            return value ? 1u : 0u;
        else if constexpr (direct_enum<T>)
            return static_cast<std::uint32_t>(value);
        else
            return value;
    }

    struct DirectContext {
        std::uintptr_t address{};

        void* invocation{};

        detail::BusinessFailure* failure{};
    };

    class DirectScope {
        struct Stateless;

        struct Active;

        enum class Mode : std::uint8_t { none, stateless, full };

        alignas(16) std::byte storage_[1664];

        Mode mode_{Mode::none};

        void leave() noexcept;

    public:
        DirectScope() noexcept = default;

        ~DirectScope() {
            if (mode_ != Mode::none)
                leave();
        }

        DirectScope(const DirectScope&) = delete;

        DirectScope& operator=(const DirectScope&) = delete;

        [[nodiscard]] bool try_enter(const Version*, const State*, Optional<std::size_t>, ObjectId,
                                     DirectContext&) noexcept;

        [[nodiscard]] Error enter_error(const Version*, const State*, Optional<std::size_t>, ObjectId);

        [[nodiscard]] bool try_finish(std::uint32_t status) noexcept;

        [[nodiscard]] Error finish_error(std::uint32_t status);
    };

    using DirectCall = std::uint32_t (*)(std::uintptr_t, void*, void*, std::uint64_t*, detail::BusinessFailure*,
                                         detail::BusinessTrace*) noexcept;

    static Result<std::uint64_t> invoke_bound(const Version*, const State*, Optional<std::size_t>, ObjectId, void*,
                                              DirectCall, Diagnostics*, Debugger*);

public:
    class Lease;

    class Entry;

    // One checked synchronous signature: Lease fixes its Version, Entry follows
    // publication. Active execution, not the handle, protects code during callbacks.
    template <class R, class... Args>
        requires(std::is_void_v<R> || direct_value<R>) && (direct_parameter<Args> && ...)
    class Typed {
        friend class Lease;

        friend class Entry;

        std::shared_ptr<const Version> version_;

        std::shared_ptr<State> owner_;

        Optional<std::size_t> index_;

        explicit Typed(std::shared_ptr<const Version> version) : version_(std::move(version)) {}

        Typed(std::shared_ptr<State> owner, Optional<std::size_t> index)
            : owner_(std::move(owner)), index_(index) {}

    public:
        Typed() = default;

        ~Typed() { atelier::detail::assert_serial_access(); }

        Typed(const Typed&) = default;

        Typed(Typed&&) noexcept = default;

        Typed& operator=(const Typed& other) noexcept {
            atelier::detail::assert_serial_access();
            version_ = other.version_;
            owner_ = other.owner_;
            index_ = other.index_;
            return *this;
        }

        Typed& operator=(Typed&& other) noexcept {
            atelier::detail::assert_serial_access();
            version_ = std::move(other.version_);
            owner_ = std::move(other.owner_);
            index_ = std::move(other.index_);
            return *this;
        }

        Result<R> invoke(ObjectId receiver, Args... arguments, Diagnostics* diagnostics = nullptr,
                         Debugger* debugger = nullptr) const {
            using Entry = std::uint32_t (*)(void*, DirectAbi<Args>..., std::uint64_t*, detail::BusinessFailure*,
                                            detail::BusinessTrace*) noexcept;
            std::uint64_t output{};
            if (!diagnostics && !debugger) {
                DirectScope scope;
                DirectContext direct;
                if (!scope.try_enter(version_.get(), owner_.get(), index_, receiver, direct)) [[unlikely]]
                    return std::unexpected(scope.enter_error(version_.get(), owner_.get(), index_, receiver));
                const auto status = reinterpret_cast<Entry>(direct.address)(
                    direct.invocation, Program::direct_abi<Args>(arguments)..., std::is_void_v<R> ? nullptr : &output,
                    direct.failure, nullptr);
                if (!scope.try_finish(status)) [[unlikely]]
                    return std::unexpected(scope.finish_error(status));
            } else {
                std::tuple<DirectAbi<Args>...> values{Program::direct_abi<Args>(arguments)...};
                const auto call = [](std::uintptr_t address, void* context, void* invocation, std::uint64_t* staged,
                                     detail::BusinessFailure* failure, detail::BusinessTrace* trace) noexcept {
                    const auto entry = reinterpret_cast<Entry>(address);
                    return std::apply(
                        [&](DirectAbi<Args>... unpacked) {
                            return entry(invocation, unpacked..., std::is_void_v<R> ? nullptr : staged, failure, trace);
                        },
                        *static_cast<std::tuple<DirectAbi<Args>...>*>(context));
                };
                ATELIER_TRY_BIND(result, invoke_bound(version_.get(), owner_.get(), index_, receiver, &values, call,
                                                      diagnostics, debugger));
                output = *result;
            }
            if constexpr (std::is_void_v<R>)
                return {};
            else if constexpr (std::same_as<R, double>)
                return std::bit_cast<double>(output);
            else if constexpr (std::same_as<R, bool>)
                return output != 0;
            else if constexpr (direct_enum<R>)
                return static_cast<R>(static_cast<std::uint32_t>(output));
            else
                return std::bit_cast<R>(static_cast<std::uint32_t>(output));
        }
    };

    class SourceRequest {
        friend class Program;

        std::weak_ptr<State> owner_;

        std::shared_ptr<const Contract> contract_;

        std::shared_ptr<const Contract> compilation_contract_;

        std::shared_ptr<atelier::detail::TypePublication> type_publication_;

        Optional<Error> declaration_error_;

        std::shared_ptr<const Declaration> declaration_;

        std::shared_ptr<ModuleRequest> module_;

        String source_path_; // Original single-input name, without the compiler's request display suffix.

        SourceRequest(std::weak_ptr<State>, std::shared_ptr<const Contract>, std::shared_ptr<const Declaration>,
                      std::uint64_t, String, String, String, CompileOptions);

    public:
        const std::uint64_t sequence;

        const String source_name;

        const String source;

        const String entry_name;

        const CompileOptions options;

        std::shared_ptr<const SourceSnapshot> snapshot() const;
    };

    using Request = std::shared_ptr<const SourceRequest>;

    class Emission {
        friend class Program;

        struct Destroy {
            void operator()(detail::BusinessCode*) const noexcept;
        };

        Request request_;

        std::unique_ptr<detail::BusinessCode, Destroy> code_;

        Emission(Request request, detail::BusinessCode* code) : request_(std::move(request)), code_(code) {}
    };

    class Candidate {
        friend class Program;

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
        Span<const ClassInfo> classes() const noexcept;

        Span<const EntryInfo> entries() const noexcept;

        Span<const FunctionInfo> functions() const noexcept;

        Span<const String> engine_modules() const noexcept;
    };

    class Lease {
        friend class Program;

        friend struct detail::ProgramAccess;

        std::shared_ptr<const Version> version_;

        explicit Lease(std::shared_ptr<const Version>);

    public:
        Lease() = default;

        ~Lease() { atelier::detail::assert_serial_access(); }

        Lease(const Lease&) = default;

        Lease(Lease&&) noexcept = default;

        Lease& operator=(const Lease& other) noexcept {
            atelier::detail::assert_serial_access();
            version_ = other.version_;
            return *this;
        }

        Lease& operator=(Lease&& other) noexcept {
            atelier::detail::assert_serial_access();
            version_ = std::move(other.version_);
            return *this;
        }

        // Binding checks the shared signature once. No generic-argument fallback.
        template <class R, class... Args>
            requires(std::is_void_v<R> || direct_value<R>) && (direct_parameter<Args> && ...)
        Result<Typed<R, Args...>> bind() const {
            SignatureDefinition expected;
            expected.parameters = {
                ParameterDefinition{{TypeReference::native<std::remove_cvref_t<Args>>()}, parameter_passing<Args>}...};
            if constexpr (!std::is_void_v<R>)
                expected.result = ValueDefinition{TypeReference::native<R>()};
            ATELIER_TRY(bind_signature(std::move(expected)));
            return Typed<R, Args...>{version_};
        }

        Result<Optional<Scalar>> invoke(ObjectId, Span<const Scalar>, Diagnostics* = nullptr,
                                        Debugger* = nullptr) const;

        // Math and plain record results require live, aligned storage of the exact declared type.
        // Failure preserves the complete output; scalar results use the overload above.
        Result<void> invoke(ObjectId, Span<const Scalar>, ValueOutput, Diagnostics* = nullptr,
                            Debugger* = nullptr) const;

        template <std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<Optional<Scalar>> invoke(ObjectId object, const R& arguments, Diagnostics* diagnostics = nullptr,
                                        Debugger* debugger = nullptr) const {
            return invoke_arguments(object, Span<const Argument>{arguments}, diagnostics, debugger);
        }

        template <std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<void> invoke(ObjectId object, const R& arguments, ValueOutput output, Diagnostics* diagnostics = nullptr,
                            Debugger* debugger = nullptr) const {
            return invoke_arguments(object, Span<const Argument>{arguments}, output, diagnostics, debugger);
        }

    private:
        Result<void> bind_signature(SignatureDefinition) const;

        Result<Optional<Scalar>> invoke_arguments(ObjectId, Span<const Argument>, Diagnostics*, Debugger*) const;

        Result<void> invoke_arguments(ObjectId, Span<const Argument>, ValueOutput, Diagnostics*, Debugger*) const;

    public:
        Result<Coroutine> start(ObjectId, Span<const Scalar>, Diagnostics* = nullptr, Debugger* = nullptr) const;

        Result<CompilationArtifacts> artifacts() const;

        Result<Array<DiagnosticLocation>> debug_sites() const;
    };

    class Entry {
        friend class Program;

        std::weak_ptr<State> owner_;

        Optional<std::size_t> index_;

        explicit Entry(std::weak_ptr<State>, Optional<std::size_t> = {});

    public:
        Entry() = default;

        // Retains the closed/open control, not a code version. Each invocation
        // selects the current published version once, including after reordering.
        template <class R, class... Args>
            requires(std::is_void_v<R> || direct_value<R>) && (direct_parameter<Args> && ...)
        Result<Typed<R, Args...>> bind() const {
            SignatureDefinition expected;
            expected.parameters = {
                ParameterDefinition{{TypeReference::native<std::remove_cvref_t<Args>>()}, parameter_passing<Args>}...};
            if constexpr (!std::is_void_v<R>)
                expected.result = ValueDefinition{TypeReference::native<R>()};
            ATELIER_TRY_BIND(owner, bind_signature(std::move(expected)));
            return Typed<R, Args...>{std::move(*owner), index_};
        }

        Result<Optional<Scalar>> invoke(ObjectId, Span<const Scalar>, Diagnostics* = nullptr,
                                        Debugger* = nullptr) const;

        Result<void> invoke(ObjectId, Span<const Scalar>, ValueOutput, Diagnostics* = nullptr,
                            Debugger* = nullptr) const;

        template <std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<Optional<Scalar>> invoke(ObjectId object, const R& arguments, Diagnostics* diagnostics = nullptr,
                                        Debugger* debugger = nullptr) const {
            return invoke_arguments(object, Span<const Argument>{arguments}, diagnostics, debugger);
        }

        template <std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<void> invoke(ObjectId object, const R& arguments, ValueOutput output, Diagnostics* diagnostics = nullptr,
                            Debugger* debugger = nullptr) const {
            return invoke_arguments(object, Span<const Argument>{arguments}, output, diagnostics, debugger);
        }

    private:
        Result<std::shared_ptr<State>> bind_signature(SignatureDefinition) const;

        Result<Optional<Scalar>> invoke_arguments(ObjectId, Span<const Argument>, Diagnostics*, Debugger*) const;

        Result<void> invoke_arguments(ObjectId, Span<const Argument>, ValueOutput, Diagnostics*, Debugger*) const;

    public:
        Result<Coroutine> start(ObjectId, Span<const Scalar>, Diagnostics* = nullptr, Debugger* = nullptr) const;
    };

    // TypeId{} declares a receiverless typed value environment. A binding failure
    // publishes no Program. The nonmovable owner retains a stable address.
    static Result<std::unique_ptr<Program>> create(Runtime&, TypeId = {}) noexcept;

    // Creates an unbound Program sharing an already-bound Runtime contract.
    static Result<std::unique_ptr<Program>> create_like(const Program& prototype) noexcept;

    ~Program();

    // Idempotently closes entries, requests and publication. Existing leases
    // and coroutines retain their version; their owner must finish/cancel them
    // before destroying the Runtime. Safe during an active invocation.
    void close() noexcept;

    Program(const Program&) = delete;

    Program& operator=(const Program&) = delete;

    Entry entry() const;

    Result<Entry> entry(StringView name) const;

    Result<Lease> pin(StringView name) const;

    Result<FunctionSignature> describe(StringView name) const;

    // Views remain valid until the next successful publication or close.
    Span<const ClassInfo> classes() const noexcept;

    Span<const EntryInfo> entries() const noexcept;

    Span<const FunctionInfo> functions() const noexcept;

    Span<const String> engine_modules() const noexcept;

    // Counts successful publications; 0 before the first.
    std::uint64_t revision() const noexcept;

    Result<FunctionSignature> describe() const;

    Result<Lease> pin() const;

    // Requests collect source type declarations and capture unpublished Runtime
    // layouts. Preparation reports declaration/body errors without publishing types.
    // Requests own the module source, entry selection and metadata, but no code.
    // Multiple functions require an entry name before
    // first publication; later requests default to the frozen declaration name.
    Result<Request> request(StringView name, StringView source, StringView entry_name = {}, CompileOptions = {});

    Result<Request> request(Utf8Text name, std::uint64_t version, Array<SourceFile> files, CompileOptions = {});

    Result<Request> request(SourceSnapshot, CompileOptions = {});

    // For a single input the optional selector inspects borrowed dependency facts
    // synchronously after all bodies pass. Returning false skips LLVM and yields
    // an empty candidate unless the request also has types to publish. Single-input
    // timing is optional, synchronous and independent of generated-code diagnostics.
    using NeedsCode = FunctionRef<bool(Span<const EntryFunction>) noexcept>;

    // Compiler-only phase. The worker borrows the immutable request and filter; the owner
    // retains the request until join. finish_entry validates the result on the Runtime thread.
    static Result<Emission> emit_entry(const Request&, Optional<NeedsCode> = {}, CompileTiming* = nullptr);

    static Result<Candidate> finish_entry(Emission);

    static Result<Candidate> prepare(const Request&, Optional<NeedsCode> = {}, CompileTiming* = nullptr);

    // Busy does not consume the candidate. Publishing requires the latest
    // request, unchanged dependencies and the declaration's frozen signature.
    Result<void> try_publish(Candidate&);

private:
    explicit Program(std::shared_ptr<const Contract>) noexcept;

    static Result<Candidate> prepare_module(const Request&);

    Result<void> publish_module(Candidate&);

    std::shared_ptr<State> state_;
};
}
