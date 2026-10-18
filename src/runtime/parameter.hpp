#pragma once

#include <cstdint>
#include <type_traits>

namespace atelier {
// Passing mode is independent of value type and object-reference ownership.
// Native sequence operations preserve other live values. The target's element
// addresses may change; callers must own its mutation interval.
// insert/remove/move apply to Array object fields only. For them `count` is the
// position (insert index, first removed, moved element); insert's source is the
// element to copy or null for default construction, and remove/move read their
// second operand (removed count, destination index) from a std::size_t at source.
enum class SequenceMutation : std::uint32_t { reserve, resize, clear, add, append, assign_text, append_text, insert, remove, move };

enum class ParameterPassing : std::uint32_t { value, read_only, reference };
template<class T>
inline constexpr ParameterPassing parameter_passing = !std::is_lvalue_reference_v<T>
    ? ParameterPassing::value : std::is_const_v<std::remove_reference_t<T>>
    ? ParameterPassing::read_only : ParameterPassing::reference;
}
