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
        *data = text.c_str(); *size = text.size();
    };
    definition.native.value_operations_preserve_borrows = true;
    definition.native.sequence_mutate = [](SequenceMutation operation, void* target, const void* source, std::size_t count) {
        auto& text = *static_cast<String*>(target);
        switch (operation) {
        case SequenceMutation::reserve: text.reserve(count); break;
        case SequenceMutation::clear: text.clear(); break;
        case SequenceMutation::append: text.append(*static_cast<const String*>(source)); break;
        case SequenceMutation::assign_text: text.assign(StringView{static_cast<const char*>(source), count}); break;
        case SequenceMutation::append_text: text.append(StringView{static_cast<const char*>(source), count}); break;
        default: throw std::invalid_argument("Unsupported String mutation.");
        }
    };
    return definition;
}

template<class T>
TypeDefinition array_definition(Runtime& runtime, std::string name) {
    static_assert(std::is_copy_constructible_v<T>);
    auto definition = NativeRegistration<Array<T>>{runtime, std::move(name), TypeKind::array}.build();
    definition.element = TypeReference::native<T>();
    definition.native.element_representation = detail::representation<T>();
    definition.native.sequence_view = [](const void* value, const void** data, std::size_t* size) noexcept {
        const auto& array = *static_cast<const Array<T>*>(value);
        *data = array.data(); *size = array.size();
    };
    definition.native.value_operations_preserve_borrows = true; // composed with element capabilities at publication
    definition.native.sequence_mutate = [](SequenceMutation operation, void* target, const void* source, std::size_t count) {
        auto& array = *static_cast<Array<T>*>(target);
        // Element moves or swaps (std::rotate in Array::insert/move) that can throw would leave a
        // partial shift; edit a copy instead.
        const auto atomically = [&](auto change) {
            if constexpr (std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
                          std::is_nothrow_swappable_v<T>) change(array);
            else { Array<T> copy{array}; change(copy); array.swap(copy); }
        };
        switch (operation) {
        case SequenceMutation::reserve: array.reserve(count); break;
        case SequenceMutation::resize:
            if constexpr (std::default_initializable<T>) array.resize(count);
            else throw std::invalid_argument("Array element is not default constructible.");
            break;
        case SequenceMutation::clear: array.clear(); break;
        case SequenceMutation::add: array.add(*static_cast<const T*>(source)); break;
        case SequenceMutation::append: array.append(static_cast<const Array<T>*>(source)->view()); break;
        case SequenceMutation::insert:
        case SequenceMutation::remove:
        case SequenceMutation::move:
            if constexpr (std::is_move_assignable_v<T>) {
                if (operation != SequenceMutation::insert) {
                    const auto second = *static_cast<const std::size_t*>(source);
                    if (operation == SequenceMutation::remove) atomically([&](Array<T>& values) { values.remove_at(count, second); });
                    else atomically([&](Array<T>& values) { values.move(count, second); });
                }
                else if (source) atomically([&](Array<T>& values) { values.insert(count, *static_cast<const T*>(source)); });
                else if constexpr (std::default_initializable<T>) atomically([&](Array<T>& values) { values.insert(count); });
                else throw std::invalid_argument("Array element is not default constructible.");
            } else throw std::invalid_argument("Array element is not move assignable.");
            break;
        default: throw std::invalid_argument("Unsupported Array mutation.");
        }
    };
    return definition;
}

}
