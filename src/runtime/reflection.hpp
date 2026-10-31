#pragma once

#include "runtime/runtime.hpp"

namespace atelier::reflection {
// Generated specializations produce the existing TypeDefinition. They hold no
// Runtime, TypeId, object state or automatic registration side effects.
template <class T>
struct Definition {
    static TypeDefinition build(Runtime&);
};
}

// The reflection tool observes these invocations with Clang's preprocessor and
// associates them with actual AST declarations. Ordinary C++ needs no plugin.
#define ACLASS(...)
#define ASTRUCT(...)
#define AENUM(...)
#define AFIELD(...)
#define AFUNCTION(...)
#define AGENERATED_BODY(Type) friend struct ::atelier::reflection::Definition<Type>;
