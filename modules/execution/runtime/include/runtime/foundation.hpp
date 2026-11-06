#pragma once

#include "foundation/array.hpp"
#include "foundation/string.hpp"
#include "runtime/native.hpp"

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
    return definition;
}

template <class T>
TypeDefinition array_definition(Runtime& runtime, std::string name) {
    static_assert(std::is_copy_constructible_v<T>);
    auto definition = NativeRegistration<Array<T>>{runtime, std::move(name), TypeKind::array}.build();
    definition.element = TypeReference::native<T>();
    definition.native.element_representation = detail::representation<T>();
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
    return definition;
}

}
