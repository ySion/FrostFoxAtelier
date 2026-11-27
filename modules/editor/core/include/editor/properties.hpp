#pragma once
#include "editor/fields.hpp"
#include "assets/json_format.hpp"
#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "runtime/runtime.hpp"
#include <functional>
#include <variant>

namespace atelier::editor {
namespace detail {
struct PropertyStorage;

// Removes the retained subtree at path, or a retained leaf on its prefix.
inline bool erase_path(json::Builder& builder, json::Value record, Span<const std::string> path) {
    Array<json::Value> trail{record};
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto node = trail.back();
        const auto at = node.find(path[i]);
        if (!at || !*at)
            return false;
        if (i + 1 == path.size() || (**at).kind() != json::Kind::object) {
            builder.erase(node, path[i]);
            for (std::size_t up = trail.size() - 1; up > 0 && trail[up].size() == 0; --up)
                builder.erase(trail[up - 1], path[up - 1]);
            return true;
        }
        trail.add(**at);
    }
    return false;
}
} // namespace detail

// Where a properties record is decoded: an object, or a caller-owned live value of `type`.
struct PropertyTarget {
    TypeId type;

    ObjectId object{};

    Span<std::byte> storage{};
};

// A whole inline Array value of scalars, enums or strings, decoded as one leaf. It is filled
// before it is shared; copies then share one value, which must not outlive its Runtime.
class ArrayLeaf {
public:
    // An empty Array of `type`.
    [[nodiscard]] static Result<ArrayLeaf> create(Runtime&, TypeId type);

    [[nodiscard]] TypeId type() const noexcept;

    [[nodiscard]] Span<std::byte> storage() const noexcept;

    [[nodiscard]] ValueInput input() const noexcept;

    [[nodiscard]] ValueOutput output() const noexcept;

private:
    std::shared_ptr<detail::PropertyStorage> value_;
};

// One decoded inline leaf: a scalar or enum within its hard bounds, a String, or a whole Array.
using PropertyLeaf = std::variant<Scalar, String, ArrayLeaf>;

// Malformed identities fail; nullopt means the target does not fit the field.
using ResolveReference = std::function<Result<Optional<ObjectReference>>(json::Value, TypeId, ValueForm)>;

// Stores one decoded inline leaf; by default it is written to the target through the Runtime.
using WriteLeaf = std::function<Result<void>(const detail::ScalarField&, const PropertyLeaf&)>;

// The saved form of one referenced object, called with a null id for an empty reference;
// the caller's side effects, such as assigning document identities, belong here.
using EncodeReference = std::function<Result<json::Document>(ObjectId)>;

// The properties record of `object` over `fields`, nested by field names: the same leaf encoder
// as scene saves. Non-finite values fail.
[[nodiscard]] Result<json::Document> encode_properties(Runtime&, ObjectId object,
                                                       Span<const detail::ScalarField> fields,
                                                       const EncodeReference& reference);

// The same encoder for a caller-owned parameter/result record. Array and sequence
// fields are object-only, as in the Runtime's indexed read interface.
[[nodiscard]] Result<json::Document> encode_properties(Runtime&, const PropertyTarget&,
                                                       Span<const detail::ScalarField> fields,
                                                       const EncodeReference& reference);

// Strict decoding of a sparse properties record, through the same leaf decoder as scene loads.
// An unknown field, a value of another type, a value outside a hard bound and a write the target
// rejects are errors (argument_mismatch), and nothing is clamped; an integral number written as a
// float converts exactly. Other Runtime failures keep their error code. Fields absent from
// `record` keep their values. A failure can leave the leaves decoded before it written. A write
// hook receives inline leaves only, so it cannot be combined with `resolve`: reference leaves
// would bypass it.
[[nodiscard]] Result<void> decode_properties(Runtime&, const PropertyTarget&, json::Value record,
                                             const ResolveReference& = {}, const WriteLeaf& = {},
                                             bool include_transient = false);

// Apply a strict inline patch to an existing object. A failed write or post-edit snapshot restores
// every committed leaf. The optional snapshot is the canonical record used by graph history.
[[nodiscard]] Result<void> patch_object_properties(Runtime&, ObjectId, json::Value patch, const EncodeReference&,
                                                   json::Document* after = nullptr);

// How a tolerant decode reports a saved leaf it does not apply as saved; paths start at the record.
struct PropertyReport {
    // The saved value is kept; the field keeps its value, or takes the clamped value of a leaf outside its hard bounds.
    std::function<Result<void>(Array<std::string>, json::Value, world::DiagnosticKind, const std::string&)> keep;

    // An integral number written as a float was converted exactly.
    std::function<Result<void>(Array<std::string>, json::Value)> converted;
};

// Tolerant decoding of a saved properties record, as scene loads do: fields now declared
// transient are skipped, while other leaves that do not fit go to `report` instead of failing.
// Runtime and report failures propagate unchanged. Reports may reject after earlier leaves were
// written; scalar clamps and conversions report after writing, Array clamps before writing.
[[nodiscard]] Result<void> decode_properties(Runtime&, const PropertyTarget&, json::Value record,
                                             const PropertyReport& report, const ResolveReference& = {});
}
