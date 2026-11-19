#pragma once
#include "runtime/runtime.hpp"

namespace atelier::detail {

// Private producer transaction shared by native registration and the source host.
// Preview identities never leave producer metadata or enter live Runtime operations.
// Runtime must outlive calls on the transaction; destruction needs no live Runtime.
class TypePublication final {
    friend class atelier::Runtime;

    struct State;

    std::unique_ptr<State> state_;

    TypePublication();

public:
    ~TypePublication();

    static Result<std::shared_ptr<TypePublication>> prepare(Runtime&, std::vector<TypeDefinition>);

    std::span<const TypeInfo* const> catalog() const noexcept;

    Result<FunctionSignature> resolve_signature(Runtime&, const SignatureDefinition&) const;

    // Validates the full method list of an already-published described object type against its own
    // fields, its ancestor and descendant chains, and any staged types in `publication`; stages
    // newly added method names so commit records them on the Runtime type descriptor.
    static Result<void> validate_methods(Runtime&, TypeId existing, std::vector<DescribedMethodDefinition> methods,
                                         std::shared_ptr<TypePublication>& publication);

    // Validate the captured catalog and reserve all commit storage. No semantic
    // publication occurs here. No callbacks or Runtime changes may intervene.
    Result<void> reserve_commit(Runtime&);

    void commit(Runtime&) noexcept;
};
}
