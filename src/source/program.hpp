#pragma once

#include "runtime/execution.hpp"
#include <type_traits>

namespace atelier::source {
namespace detail { struct ProgramAccess; }

// One source declaration with replaceable compatible code. All operations use
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
        std::shared_ptr<const Declaration> declaration_;
        SourceRequest(std::weak_ptr<State>, std::shared_ptr<const Contract>,
            std::shared_ptr<const Declaration>, std::uint64_t, std::string, std::string);
    public:
        const std::uint64_t sequence;
        const std::string source_name;
        const std::string source;
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
        Result<Coroutine> start(ObjectId, std::span<const Scalar>, CollectionBudget = {1}) const;
    };

    // TypeId{} declares a receiverless scalar environment. Binding errors are
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
    Result<Entry> bind(FunctionExecution execution = FunctionExecution::synchronous) const {
        SignatureDefinition expected;
        expected.execution = execution;
        expected.parameters = {ValueDefinition{TypeReference::native<Args>()}...};
        if constexpr (!std::is_void_v<R>) expected.result = ValueDefinition{TypeReference::native<R>()};
        return bind_signature(std::move(expected));
    }
    Result<Lease> pin() const;
    // Requests own source and captured metadata, but do not retain code.
    Result<Request> request(std::string_view name, std::string_view source);
    static Result<Candidate> prepare(const Request&);
    // Busy does not consume the candidate. Publishing requires the latest
    // request, unchanged dependencies and the declaration's frozen signature.
    Result<void> try_publish(Candidate&);
private:
    Result<Entry> bind_signature(SignatureDefinition) const;
    std::shared_ptr<State> state_;
};
}
