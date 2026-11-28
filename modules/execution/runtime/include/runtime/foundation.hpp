#pragma once

#include "foundation/array.hpp"
#include "foundation/string.hpp"
#include "runtime/native.hpp"

#include <bit>

namespace atelier {

// Explicit producers for the same Runtime publication transaction. Include each
// concrete value type once in the batch, alongside its element/owning records.
inline TypeDefinition string_definition(Runtime& runtime) {
    auto definition = NativeRegistration<String>{runtime, "core.String", TypeKind::string}.build();
    definition.native.sequence_view = [](const void* value, const void** data, std::size_t* size) noexcept {
        const auto& text = *static_cast<const String*>(value);
        *data = text.c_str();
        *size = text.size();
    };
    definition.native.value_operations_preserve_borrows = true;
    definition.native.sequence_mutate = [](SequenceMutation operation, void* target, const void* source,
                                           std::size_t count) noexcept -> Result<void> {
        auto& text = *static_cast<String*>(target);
        const auto appended = operation == SequenceMutation::append        ? static_cast<const String*>(source)->size()
                              : operation == SequenceMutation::append_text ? count
                                                                           : 0;
        if (((operation == SequenceMutation::reserve || operation == SequenceMutation::assign_text) &&
             count > String::max_size) ||
            appended > String::max_size - text.size())
            return std::unexpected(Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
        switch (operation) {
        case SequenceMutation::reserve:
            text.reserve(count);
            break;
        case SequenceMutation::clear:
            text.clear();
            break;
        case SequenceMutation::append:
            text.append(*static_cast<const String*>(source));
            break;
        case SequenceMutation::assign_text:
            if (!text.assign(StringView{static_cast<const char*>(source), count}))
                return std::unexpected(Error{ErrorCode::argument_mismatch, "String input is not valid UTF-8."});
            break;
        case SequenceMutation::append_text:
            if (!text.append(StringView{static_cast<const char*>(source), count}))
                return std::unexpected(Error{ErrorCode::argument_mismatch, "String input is not valid UTF-8."});
            break;
        default:
            return std::unexpected(Error{ErrorCode::unsupported_operation, "Unsupported String mutation."});
        }
        return {};
    };
    definition.native.sequence_apply = [](void* target, const SequenceCall& call) noexcept -> Result<void> {
        auto& text = *static_cast<String*>(target);
        using Alg = SequenceAlgorithm;
        switch (call.algorithm) {
        case Alg::str_append_i32:
            if (16 > String::max_size - text.size())
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            text.append_number(static_cast<std::int32_t>(call.index));
            return {};
        case Alg::str_append_u32:
            if (16 > String::max_size - text.size())
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            text.append_number(static_cast<std::uint32_t>(call.index));
            return {};
        case Alg::str_append_f32:
            if (32 > String::max_size - text.size())
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            text.append_number(std::bit_cast<float>(static_cast<std::uint32_t>(call.index)));
            return {};
        case Alg::str_append_f64:
            if (40 > String::max_size - text.size())
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            text.append_number(std::bit_cast<double>(call.index));
            return {};
        case Alg::str_append_bool: {
            const StringView word = call.index != 0 ? StringView{"true", 4} : StringView{"false", 5};
            if (word.size() > String::max_size - text.size())
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            text.append_validated(word);
            return {};
        }
        case Alg::str_append_ascii:
            if (call.index >= 128)
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "String.append_ascii requires a 7-bit ASCII byte (0..127)."});
            if (call.count > String::max_size - text.size())
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            text.append_ascii(static_cast<char>(call.index), static_cast<std::size_t>(call.count));
            return {};
        case Alg::str_append_hex:
            if (call.count > 1024)
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String.append_hex width exceeds 1024 digits."});
            if (std::max<std::uint64_t>(call.count, 16) > String::max_size - text.size())
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            text.append_hex(static_cast<std::uint32_t>(call.index), static_cast<std::size_t>(call.count),
                            call.flags != 0);
            return {};
        case Alg::str_append_float_fixed:
            if (call.count > 1024)
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String.append_float precision exceeds 1024 digits."});
            if (400 + call.count > String::max_size - text.size())
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            text.append_float_fixed(std::bit_cast<double>(call.index), static_cast<int>(call.count));
            return {};
        case Alg::str_pad_left:
        case Alg::str_pad_right:
            if (call.count >= 128)
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "String padding requires a 7-bit ASCII byte (0..127)."});
            if (call.index > String::max_size)
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            if (call.algorithm == Alg::str_pad_left)
                text.pad_left(static_cast<std::size_t>(call.index), static_cast<char>(call.count));
            else
                text.pad_right(static_cast<std::size_t>(call.index), static_cast<char>(call.count));
            return {};
        case Alg::str_trim:
            text.trim_in_place();
            return {};
        case Alg::str_trim_start:
            text.trim_start_in_place();
            return {};
        case Alg::str_trim_end:
            text.trim_end_in_place();
            return {};
        case Alg::str_to_lower:
            text.to_ascii_lower();
            return {};
        case Alg::str_to_upper:
            text.to_ascii_upper();
            return {};
        case Alg::str_strip_prefix:
        case Alg::str_strip_suffix: {
            const StringView sub{call.first_data ? static_cast<const char*>(call.first_data) : "", call.first_size};
            const bool matched =
                call.algorithm == Alg::str_strip_prefix ? text.strip_prefix(sub) : text.strip_suffix(sub);
            *call.result = matched ? 1u : 0u;
            return {};
        }
        case Alg::str_slice_in_place:
        case Alg::str_erase: {
            const auto first = static_cast<std::size_t>(call.index);
            const auto count = call.flags != 0 ? StringView::npos : static_cast<std::size_t>(call.count);
            if (first > text.size() || (count != StringView::npos && count > text.size() - first))
                return std::unexpected(Error{ErrorCode::argument_mismatch, "String byte range is out of bounds."});
            const auto status = call.algorithm == Alg::str_slice_in_place ? text.slice_in_place(first, count)
                                                                          : text.erase(first, count);
            if (!status)
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "String byte range splits a UTF-8 code point."});
            return {};
        }
        case Alg::str_replace: {
            const auto first = static_cast<std::size_t>(call.index);
            const auto count = static_cast<std::size_t>(call.count);
            if (first > text.size() || count > text.size() - first)
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "String replacement range is out of bounds."});
            const StringView rep{call.first_data ? static_cast<const char*>(call.first_data) : "", call.first_size};
            if (rep.size() > String::max_size - (text.size() - count))
                return std::unexpected(
                    Error{ErrorCode::capacity_exceeded, "String length exceeds addressable storage."});
            if (!text.replace(first, count, rep))
                return std::unexpected(Error{ErrorCode::argument_mismatch,
                                             "String replacement splits a UTF-8 code point or has invalid UTF-8."});
            return {};
        }
        case Alg::str_replace_all: {
            const StringView search{call.first_data ? static_cast<const char*>(call.first_data) : "", call.first_size};
            const StringView rep{call.second_data ? static_cast<const char*>(call.second_data) : "", call.second_size};
            const auto res = text.replace_all(search, rep);
            if (!res)
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "String.replace_all input is not valid UTF-8."});
            *call.result = static_cast<std::uint64_t>(*res);
            return {};
        }
        default:
            return std::unexpected(Error{ErrorCode::unsupported_operation, "Unsupported String sequence operation."});
        }
    };
    return definition;
}

template <class T>
TypeDefinition array_definition(Runtime& runtime, Utf8Text name) {
    static_assert(std::is_copy_constructible_v<T>);
    static_assert(std::is_standard_layout_v<Array<T>> && sizeof(Array<T>) == 3 * sizeof(void*) &&
                  alignof(Array<T>) == alignof(void*));
    auto definition = NativeRegistration<Array<T>>{runtime, std::move(name), TypeKind::array}.build();
    definition.element = TypeReference::native<T>();
    definition.native.element_representation = detail::representation<T>();
    definition.native.standard_array_layout = true;
    definition.native.sequence_view = [](const void* value, const void** data, std::size_t* size) noexcept {
        const auto& array = *static_cast<const Array<T>*>(value);
        *data = array.data();
        *size = array.size();
    };
    definition.native.value_operations_preserve_borrows = true; // composed with element capabilities at publication
    definition.native.sequence_mutate = [](SequenceMutation operation, void* target, const void* source,
                                           std::size_t count) noexcept -> Result<void> {
        auto& array = *static_cast<Array<T>*>(target);
        const auto added = operation == SequenceMutation::append ? static_cast<const Array<T>*>(source)->size()
                           : operation == SequenceMutation::add || operation == SequenceMutation::insert ? 1
                                                                                                         : 0;
        if (((operation == SequenceMutation::reserve || operation == SequenceMutation::resize) &&
             count > Array<T>::max_size) ||
            added > Array<T>::max_size - array.size())
            return std::unexpected(Error{ErrorCode::capacity_exceeded, "Array size exceeds addressable storage."});
        // Ordinary C++ element operations have no recoverable error channel.
        // Fallible registered elements require their own result-based adapter.
        switch (operation) {
        case SequenceMutation::reserve:
            array.reserve(count);
            break;
        case SequenceMutation::resize:
            if constexpr (std::default_initializable<T>)
                array.resize(count);
            else
                return std::unexpected(
                    Error{ErrorCode::unsupported_operation, "Array element is not default constructible."});
            break;
        case SequenceMutation::clear:
            array.clear();
            break;
        case SequenceMutation::add:
            array.add(*static_cast<const T*>(source));
            break;
        case SequenceMutation::append:
            array.append(static_cast<const Array<T>*>(source)->view());
            break;
        case SequenceMutation::insert:
        case SequenceMutation::remove:
        case SequenceMutation::move:
            if constexpr (std::is_move_assignable_v<T>) {
                // The Runtime checks positions first, so removing nothing or moving in place changes nothing.
                if (operation != SequenceMutation::insert) {
                    const auto second = *static_cast<const std::size_t*>(source);
                    if (operation == SequenceMutation::remove) {
                        if (second)
                            array.remove_at(count, second);
                    } else if (count != second)
                        array.move(count, second);
                } else if (source)
                    array.insert(count, *static_cast<const T*>(source));
                else if constexpr (std::default_initializable<T>)
                    array.insert(count);
                else
                    return std::unexpected(
                        Error{ErrorCode::unsupported_operation, "Array element is not default constructible."});
            } else
                return std::unexpected(
                    Error{ErrorCode::unsupported_operation, "Array element is not move assignable."});
            break;
        default:
            return std::unexpected(Error{ErrorCode::unsupported_operation, "Unsupported Array mutation."});
        }
        return {};
    };
    definition.native.sequence_apply = [](void* target, const SequenceCall& call) noexcept -> Result<void> {
        auto& array = *static_cast<Array<T>*>(target);
        using Alg = SequenceAlgorithm;
        switch (call.algorithm) {
        case Alg::arr_fill:
            if constexpr (std::is_copy_assignable_v<T>) {
                array.fill(*static_cast<const T*>(call.first_data));
                return {};
            } else {
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Array element is not copy assignable."});
            }
        case Alg::arr_reverse:
            if constexpr (std::swappable<T>) {
                array.reverse();
                return {};
            } else {
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Array element is not swappable."});
            }
        case Alg::arr_swap:
            if (call.index >= array.size() || call.count >= array.size())
                return std::unexpected(Error{ErrorCode::argument_mismatch, "Array.swap index is out of bounds."});
            if constexpr (std::swappable<T>) {
                array.swap_elements(static_cast<std::size_t>(call.index), static_cast<std::size_t>(call.count));
                return {};
            } else {
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Array element is not swappable."});
            }
        case Alg::arr_sort:
            if constexpr (std::floating_point<T>) {
                for (const T& value : array)
                    if (value != value)
                        return std::unexpected(
                            Error{ErrorCode::argument_mismatch, "Floating-point Array.sort() requires non-NaN elements."});
                array.sort();
                return {};
            } else if constexpr (std::sortable<T*, std::reference_wrapper<std::less<>>>) {
                array.sort();
                return {};
            } else {
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Array element does not support ordering."});
            }
        case Alg::arr_resize_fill:
            if (call.index > Array<T>::max_size)
                return std::unexpected(Error{ErrorCode::capacity_exceeded, "Array size exceeds addressable storage."});
            array.resize(static_cast<std::size_t>(call.index), *static_cast<const T*>(call.first_data));
            return {};
        case Alg::arr_truncate:
            if (call.index > array.size())
                return std::unexpected(Error{ErrorCode::argument_mismatch, "Array.truncate count exceeds current size."});
            array.truncate(static_cast<std::size_t>(call.index));
            return {};
        case Alg::arr_pop:
            if (array.empty())
                return std::unexpected(Error{ErrorCode::argument_mismatch, "Array.pop requires a non-empty Array."});
            array.pop();
            return {};
        case Alg::arr_insert:
            if (call.index > array.size())
                return std::unexpected(Error{ErrorCode::argument_mismatch, "Array.insert index is out of bounds."});
            if (array.size() == Array<T>::max_size)
                return std::unexpected(Error{ErrorCode::capacity_exceeded, "Array size exceeds addressable storage."});
            if constexpr (std::is_move_assignable_v<T>) {
                array.insert(static_cast<std::size_t>(call.index), *static_cast<const T*>(call.first_data));
                return {};
            } else {
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Array element is not move assignable."});
            }
        case Alg::arr_remove_at:
            if (call.index > array.size() || call.count > array.size() - call.index)
                return std::unexpected(Error{ErrorCode::argument_mismatch, "Array.remove_at range is out of bounds."});
            if constexpr (std::is_move_assignable_v<T>) {
                array.remove_at(static_cast<std::size_t>(call.index), static_cast<std::size_t>(call.count));
                return {};
            } else {
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Array element is not move assignable."});
            }
        case Alg::arr_remove_at_swap:
            if (call.index >= array.size())
                return std::unexpected(Error{ErrorCode::argument_mismatch, "Array.remove_at_swap index is out of bounds."});
            if constexpr (std::is_move_assignable_v<T>) {
                array.remove_at_swap(static_cast<std::size_t>(call.index));
                return {};
            } else {
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Array element is not move assignable."});
            }
        default:
            return std::unexpected(Error{ErrorCode::unsupported_operation, "Unsupported Array sequence operation."});
        }
    };
    return definition;
}

}
