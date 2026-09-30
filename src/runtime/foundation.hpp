#pragma once

#include "foundation/array.hpp"
#include "foundation/string.hpp"
#include "runtime/native.hpp"

namespace atelier {

// Explicit producers for the same Runtime publication transaction. Include each
// concrete value type once in the batch, alongside its element/owning records.
inline TypeDefinition string_definition(Runtime& runtime) {
    return NativeRegistration<String>{runtime, "core.String", TypeKind::string}.build();
}

template<class T>
TypeDefinition array_definition(Runtime& runtime, std::string name) {
    static_assert(std::is_copy_constructible_v<T>);
    auto definition = NativeRegistration<Array<T>>{runtime, std::move(name), TypeKind::array}.build();
    definition.element = TypeReference::native<T>();
    definition.native.element_representation = detail::representation<T>();
    return definition;
}

}
