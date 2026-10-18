#pragma once
#include "editor/json_format.hpp"
#include "runtime/runtime.hpp"
#include <functional>
#include <optional>
#include <variant>

namespace atelier::editor {
// Where a properties record is decoded: an object, or a caller-owned live value of `type`.
struct PropertyTarget {
    TypeId type;
    ObjectId object{};
    std::span<std::byte> storage{};
};
// One decoded inline leaf: a scalar or enum within its hard bounds, or a String.
using PropertyLeaf = std::variant<Scalar, String>;
// The reference of the field's type and form that a saved value names, or nullopt when it names none.
using ResolveReference = std::function<std::optional<ObjectReference>(const json_format::Json&, TypeId, ValueForm)>;
// Stores one decoded inline leaf; by default it is written to the target through the Runtime.
using WriteLeaf = std::function<Result<void>(std::span<const FieldId>, const PropertyLeaf&)>;

// Strict decoding of a sparse properties record, through the same leaf decoder as scene loads.
// An unknown field, a value of another type and a value outside a hard bound are errors
// (argument_mismatch), and nothing is clamped; an integral number written as a float converts
// exactly. Other failures keep their Runtime error code. Fields absent from `record` keep their
// values. A failure can leave the leaves decoded before it written. A write hook receives inline
// leaves only, so it cannot be combined with `resolve`: reference leaves would bypass it.
[[nodiscard]] Result<void> decode_properties(Runtime&, const PropertyTarget&, const json_format::Json& record,
                                             const ResolveReference& = {}, const WriteLeaf& = {});
}
