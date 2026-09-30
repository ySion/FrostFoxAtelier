#pragma once

#include <cstdint>
#include <type_traits>

namespace atelier {
// Passing mode is independent of value type and object-reference ownership.
// Native sequence operations preserve other live values. The target's element
// addresses may change; callers must own its mutation interval.
enum class SequenceMutation : std::uint32_t { reserve, resize, clear, add, append, assign_text, append_text };

enum class ParameterPassing : std::uint32_t { value, read_only, reference };
template<class T>
inline constexpr ParameterPassing parameter_passing = !std::is_lvalue_reference_v<T>
    ? ParameterPassing::value : std::is_const_v<std::remove_reference_t<T>>
    ? ParameterPassing::read_only : ParameterPassing::reference;
}
