#pragma once

#include "runtime/execution.hpp"
#include <type_traits>

namespace atelier::source {
namespace detail { struct ProgramAccess; }

// One selected module entry with replaceable compatible module code. All operations use
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
    friend struct detail::ProgramAccess;
public:
    class SourceRequest {
        friend class Program;
        friend struct detail::ProgramAccess;
        std::weak_ptr<State> owner_;
        std::shared_ptr<const Contract> contract_;
        std::shared_ptr<const Contract> compilation_contract_;
        std::shared_ptr<atelier::detail::TypePublication> type_publication_;
        std::optional<Error> declaration_error_;
        std::shared_ptr<const Declaration> declaration_;
        SourceRequest(std::weak_ptr<State>, std::shared_ptr<const Contract>,
            std::shared_ptr<const Declaration>, std::uint64_t, std::string, std::string, std::string);
    public:
        const std::uint64_t sequence;
        const std::string source_name;
        const std::string source;
        const std::string entry_name;
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
        // Diagnostic only; this address is not a callable or owning handle.
        std::uintptr_t address() const noexcept;
    };
    class Lease {
        friend class Program;
        friend struct detail::ProgramAccess;
        std::shared_ptr<const Version> version_;
        explicit Lease(std::shared_ptr<const Version>);
    public:
        Lease() = default;
        Result<std::optional<Scalar>> invoke(ObjectId, std::span<const Scalar>, CollectionBudget = {1}) const;
        // Math results require live, aligned storage of the exact declared type.
        // Failure preserves the complete output; scalar results use the overload above.
        Result<void> invoke(ObjectId, std::span<const Scalar>, ValueOutput, CollectionBudget = {1}) const;
        template<std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<std::optional<Scalar>> invoke(ObjectId object, const R& arguments, CollectionBudget budget = {1}) const {
            return invoke_arguments(object, std::span<const Argument>{arguments}, budget);
        }
        template<std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<void> invoke(ObjectId object, const R& arguments, ValueOutput output, CollectionBudget budget = {1}) const {
            return invoke_arguments(object, std::span<const Argument>{arguments}, output, budget);
        }
    private:
        Result<std::optional<Scalar>> invoke_arguments(ObjectId, std::span<const Argument>, CollectionBudget) const;
        Result<void> invoke_arguments(ObjectId, std::span<const Argument>, ValueOutput, CollectionBudget) const;
    public:
        Result<Coroutine> start(ObjectId, std::span<const Scalar>, CollectionBudget = {1}) const;
        std::uintptr_t address() const noexcept;
    };
    class Entry {
        friend class Program;
        friend struct detail::ProgramAccess;
        std::weak_ptr<State> owner_;
        explicit Entry(std::weak_ptr<State>);
    public:
        Entry() = default;
        Result<std::optional<Scalar>> invoke(ObjectId, std::span<const Scalar>, CollectionBudget = {1}) const;
        Result<void> invoke(ObjectId, std::span<const Scalar>, ValueOutput, CollectionBudget = {1}) const;
        template<std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<std::optional<Scalar>> invoke(ObjectId object, const R& arguments, CollectionBudget budget = {1}) const {
            return invoke_arguments(object, std::span<const Argument>{arguments}, budget);
        }
        template<std::ranges::contiguous_range R>
            requires std::same_as<std::ranges::range_value_t<R>, Argument>
        Result<void> invoke(ObjectId object, const R& arguments, ValueOutput output, CollectionBudget budget = {1}) const {
            return invoke_arguments(object, std::span<const Argument>{arguments}, output, budget);
        }
    private:
        Result<std::optional<Scalar>> invoke_arguments(ObjectId, std::span<const Argument>, CollectionBudget) const;
        Result<void> invoke_arguments(ObjectId, std::span<const Argument>, ValueOutput, CollectionBudget) const;
    public:
        Result<Coroutine> start(ObjectId, std::span<const Scalar>, CollectionBudget = {1}) const;
    };

    // TypeId{} declares a receiverless typed value environment. Binding errors are
    // reported by exception during construction; execution errors use Result.
    Program(Runtime&, TypeId);
    ~Program();
    // Idempotently closes entries, requests and publication. Existing leases
    // and coroutines retain their version; their owner must finish/cancel them
    // before destroying the Runtime. Safe during a pinned active invocation.
    void close() noexcept;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    Entry entry() const;
    Result<FunctionSignature> describe() const;
    template<class R, class... Args>
        requires ((!std::is_rvalue_reference_v<Args> && !std::is_volatile_v<std::remove_reference_t<Args>>) && ...) &&
            (!std::is_reference_v<R>)
    Result<Entry> bind(FunctionExecution execution = FunctionExecution::synchronous) const {
        SignatureDefinition expected;
        expected.execution = execution;
        expected.parameters = {ParameterDefinition{{TypeReference::native<std::remove_cvref_t<Args>>()}, parameter_passing<Args>}...};
        if constexpr (!std::is_void_v<R>) expected.result = ValueDefinition{TypeReference::native<R>()};
        return bind_signature(std::move(expected));
    }
    Result<Lease> pin() const;
    // Requests collect source type declarations and capture unpublished Runtime
    // layouts. Preparation reports declaration/body errors without publishing types.
    // Requests own the module source, entry selection and metadata, but no code.
    // Multiple functions require an entry name before
    // first publication; later requests default to the frozen declaration name.
    Result<Request> request(std::string_view name, std::string_view source, std::string_view entry_name = {});
    static Result<Candidate> prepare(const Request&);
    // Busy does not consume the candidate. Publishing requires the latest
    // request, unchanged dependencies and the declaration's frozen signature.
    Result<void> try_publish(Candidate&);
private:
    Result<Entry> bind_signature(SignatureDefinition) const;
    std::shared_ptr<State> state_;
};
}
