#pragma once

#include <cstdint>
#include <type_traits>

namespace atelier {
// Passing mode is independent of value type and object-reference ownership.
enum class ParameterPassing : std::uint32_t { value, read_only, reference };
template<class T>
inline constexpr ParameterPassing parameter_passing = !std::is_lvalue_reference_v<T>
    ? ParameterPassing::value : std::is_const_v<std::remove_reference_t<T>>
    ? ParameterPassing::read_only : ParameterPassing::reference;
}
