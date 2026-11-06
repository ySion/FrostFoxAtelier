#pragma once

#include "math/math.hpp"
#include "runtime/native.hpp"

namespace atelier::math {
namespace detail {

template <class T>
TypeDefinition components(Runtime& runtime) {
    NativeRegistration<T> registration(runtime, std::string(name(type_of<T>)), TypeKind::record);
    using Scalar = typename T::Scalar;
    const ValueDefinition scalar{TypeReference::native<Scalar>()};
    registration.template field<&T::x>("x", scalar, offsetof(T, x));
    registration.template field<&T::y>("y", scalar, offsetof(T, y));
    if constexpr (requires { &T::z; })
        registration.template field<&T::z>("z", scalar, offsetof(T, z));
    if constexpr (requires { &T::w; })
        registration.template field<&T::w>("w", scalar, offsetof(T, w));
    auto definition = registration.build();
    // Initialization only writes components; the default member initializers
    // make the C++ default constructor nontrivial without introducing effects.
    definition.native.value_operations_preserve_borrows = true;
    return definition;
}

template <class T, std::size_t... I>
TypeDefinition matrix(Runtime& runtime, std::index_sequence<I...>) {
    auto definition = NativeRegistration<T>(runtime, std::string(name(type_of<T>)), TypeKind::record).build();
    using Scalar = typename T::Scalar;
    const auto add = [&]<std::size_t Index>() {
        std::string field{"m00"};
        field[1] += static_cast<char>(Index / 4);
        field[2] += static_cast<char>(Index % 4);
        definition.fields.push_back(
            {std::move(field),
             {TypeReference::native<Scalar>()},
             [](void* storage) noexcept -> void* { return &static_cast<T*>(storage)->elements[Index]; },
             atelier::detail::representation<Scalar>(),
             true,
             offsetof(T, elements) + Index * sizeof(Scalar)});
    };
    (add.template operator()<I>(), ...);
    definition.native.value_operations_preserve_borrows = true;
    return definition;
}
}

// Runtime bootstrap publishes these native values once, through its ordinary
// registry. Neither the math library nor World maintains another type table.
[[nodiscard]] inline std::vector<TypeDefinition> type_definitions(Runtime& runtime) {
    std::vector<TypeDefinition> definitions;
    definitions.reserve(10);
    definitions.push_back(detail::components<Vector2>(runtime));
    definitions.push_back(detail::components<Vector3>(runtime));
    definitions.push_back(detail::components<Vector4>(runtime));
    definitions.push_back(detail::components<Quaternion>(runtime));
    definitions.push_back(detail::matrix<Matrix4>(runtime, std::make_index_sequence<16>{}));
    definitions.push_back(detail::components<Vector2d>(runtime));
    definitions.push_back(detail::components<Vector3d>(runtime));
    definitions.push_back(detail::components<Vector4d>(runtime));
    definitions.push_back(detail::components<Quaterniond>(runtime));
    definitions.push_back(detail::matrix<Matrix4d>(runtime, std::make_index_sequence<16>{}));
    return definitions;
}

}
