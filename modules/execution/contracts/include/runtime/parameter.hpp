#pragma once

#include <cstddef>
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
enum class SequenceMutation : std::uint32_t {
    reserve,
    resize,
    clear,
    add,
    append,
    assign_text,
    append_text,
    insert,
    remove,
    move
};

enum class SequenceAlgorithm : std::uint32_t {
    str_append_i32,
    str_append_u32,
    str_append_f32,
    str_append_f64,
    str_append_bool,
    str_append_ascii,
    str_append_hex,
    str_append_float_fixed,
    str_pad_left,
    str_pad_right,
    str_trim,
    str_trim_start,
    str_trim_end,
    str_to_lower,
    str_to_upper,
    str_strip_prefix,
    str_strip_suffix,
    str_slice_in_place,
    str_erase,
    str_replace,
    str_replace_all,
    arr_fill,
    arr_reverse,
    arr_swap,
    arr_sort,
    arr_resize_fill,
    arr_truncate,
    arr_pop,
    arr_insert,
    arr_remove_at,
    arr_remove_at_swap
};

struct SequenceCall {
    SequenceAlgorithm algorithm{};

    std::uint32_t flags{};

    std::uint64_t index{};

    std::uint64_t count{};

    const void* first_data{};

    std::size_t first_size{};

    const void* second_data{};

    std::size_t second_size{};

    std::uint64_t* result{};
};

static_assert(std::is_standard_layout_v<SequenceCall> && std::is_trivially_copyable_v<SequenceCall>);

enum class ParameterPassing : std::uint32_t { value, read_only, reference };

template <class T>
inline constexpr ParameterPassing parameter_passing =
    !std::is_lvalue_reference_v<T>                ? ParameterPassing::value
    : std::is_const_v<std::remove_reference_t<T>> ? ParameterPassing::read_only
                                                  : ParameterPassing::reference;
}
