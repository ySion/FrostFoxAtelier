#pragma once
#include "editor/fields.hpp"
#include "assets/json_format.hpp"
#include "foundation/string.hpp"
#include "runtime/runtime.hpp"
#include <functional>
#include <optional>
#include <span>
#include <variant>

namespace atelier::editor {
// Where a properties record is decoded: an object, or a caller-owned live value of `type`.
struct PropertyTarget {
    TypeId type;
    ObjectId object{};
    std::span<std::byte> storage{};
};
// A whole inline Array value of scalars, enums or strings, decoded as one leaf. It is filled
// before it is shared; copies then share one value, which must not outlive its Runtime.
class ArrayLeaf {
public:
    // An empty Array of `type`.
    [[nodiscard]] static Result<ArrayLeaf> create(Runtime&, TypeId type);
    [[nodiscard]] TypeId type() const noexcept;
    [[nodiscard]] std::span<std::byte> storage() const noexcept;
    [[nodiscard]] ValueInput input() const noexcept;
    [[nodiscard]] ValueOutput output() const noexcept;

private:
    struct Value;
    std::shared_ptr<Value> value_;
};
// One decoded inline leaf: a scalar or enum within its hard bounds, a String, or a whole Array.
using PropertyLeaf = std::variant<Scalar, String, ArrayLeaf>;
// The reference of the field's type and form that a saved value names, or nullopt when it names none.
using ResolveReference =
    std::function<std::optional<ObjectReference>(const assets::json_format::Json&, TypeId, ValueForm)>;
// Stores one decoded inline leaf; by default it is written to the target through the Runtime.
using WriteLeaf = std::function<Result<void>(std::span<const FieldId>, const PropertyLeaf&)>;
// The saved form of one referenced object, called with a null id for an empty reference;
// the caller's side effects, such as assigning document identities, belong here.
using EncodeReference = std::function<assets::json_format::Json(ObjectId)>;

// The properties record of `object` over `fields`, nested by field names: the same leaf encoder
// as scene saves. Non-finite values fail.
[[nodiscard]] Result<assets::json_format::Json> encode_properties(Runtime&, ObjectId object,
                                                                  std::span<const detail::ScalarField> fields,
                                                                  const EncodeReference& reference);

// Strict decoding of a sparse properties record, through the same leaf decoder as scene loads.
// An unknown field, a value of another type, a value outside a hard bound and a write the target
// rejects are errors (argument_mismatch), and nothing is clamped; an integral number written as a
// float converts exactly. Other Runtime failures keep their error code. Fields absent from
// `record` keep their values. A failure can leave the leaves decoded before it written. A write
// hook receives inline leaves only, so it cannot be combined with `resolve`: reference leaves
// would bypass it.
[[nodiscard]] Result<void> decode_properties(Runtime&, const PropertyTarget&, const assets::json_format::Json& record,
                                             const ResolveReference& = {}, const WriteLeaf& = {});

// How a tolerant decode reports a saved leaf it does not apply as saved; paths start at the record.
struct PropertyReport {
    // The saved value is kept; the field keeps its value, or takes the clamped value of a leaf outside its hard bounds.
    std::function<void(std::vector<std::string>, const assets::json_format::Json&, world::DiagnosticKind,
                       const std::string&)>
        keep;
    // An integral number written as a float was converted exactly.
    std::function<void(std::vector<std::string>, const assets::json_format::Json&)> converted;
};
// Tolerant decoding over `fields`, as scene loads do: a leaf that does not fit goes to `report`
// instead of failing. Only Runtime failures fail.
[[nodiscard]] Result<void> decode_properties(Runtime&, const PropertyTarget&,
                                             std::span<const detail::ScalarField> fields,
                                             const assets::json_format::Json& record, const PropertyReport& report,
                                             const ResolveReference& = {});
}
