#pragma once

#include "foundation/array.hpp"
#include "foundation/result.hpp"
#include "foundation/string.hpp"
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace atelier::json {

enum class Fault { syntax, depth, duplicate_key, type, missing, range, members, encoding };

struct Error {
    Fault fault;

    std::string message;

    std::string path;

    std::optional<std::size_t> offset;

    std::size_t line{};

    std::size_t column{};

    [[nodiscard]] std::string describe() const;
};

template <class T>
using Result = std::expected<T, Error>;

class Document;

class Builder;

class ArrayView;

enum class Kind { null, boolean, integer, real, string, array, object };

// Borrowed node and stable DOM root, not an owner-wrapper or parent-Value address.
// Owner destruction or replacement of this subtree invalidates the view. Moving
// the owner preserves it. Error paths are recovered only on failure.
class Value {
public:
    [[nodiscard]] Kind kind() const noexcept;

    [[nodiscard]] bool is_null() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] Result<Value> at(std::size_t index) const;

    [[nodiscard]] Result<Value> at(std::string_view key) const;

    [[nodiscard]] Result<std::optional<Value>> find(std::string_view key) const;

    [[nodiscard]] Result<Value> path(std::initializer_list<std::string_view> keys) const;

    [[nodiscard]] Result<void> members(std::initializer_list<std::string_view> keys) const;

    // Reject unknown fields, but do not require optional fields to be present.
    [[nodiscard]] Result<void> allowed_members(std::initializer_list<std::string_view> keys) const;

    [[nodiscard]] Result<std::string_view> string(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<bool> boolean(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<std::uint32_t> u32(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<std::uint64_t> u64(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<std::int64_t> i64(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<float> f32(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<double> number(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<ArrayView> array() const;

    // Member references in bytewise key order; never copies subtrees or keys.
    [[nodiscard]] Result<Array<std::pair<std::string_view, Value>>> object_items() const;

    [[nodiscard]] Error error(Fault fault, std::string_view message) const;

    [[nodiscard]] bool operator==(Value other) const;

private:
    friend class Document;

    friend class Builder;

    friend class ArrayView;

    friend struct Access;

    Value(const void* node, bool mutable_node, const void* root = nullptr)
        : node_(node), mutable_(mutable_node), root_(root ? root : node) {}

    const void* node_;

    bool mutable_;

    const void* root_;
};

// A linear, allocation-free borrowed traversal. Mutating this array invalidates
// its iterators; element views retain the usual Value lifetime contract.
class ArrayView {
public:
    class Iterator {
    public:
        Value operator*() const;

        Iterator& operator++();

        bool operator==(const Iterator& other) const { return remaining_ == other.remaining_; }

    private:
        friend class ArrayView;

        Iterator(Value current, std::size_t remaining) : current_(current), remaining_(remaining) {}

        Value current_;

        std::size_t remaining_;
    };

    Iterator begin() const;

    Iterator end() const { return {value_, 0}; }

    std::size_t size() const { return value_.size(); }

private:
    friend class Value;

    explicit ArrayView(Value value) : value_(value) {}

    Value value_;
};

// Limits are supplied by the input owner. Event depth counts keys and scalars
// as well as containers; zero is the root container's start event.
enum class DepthPolicy { events, containers };

struct ParseOptions {
    std::size_t maximum_depth;

    // Assets/control count containers instead: root is container depth zero,
    // and containers at this limit are rejected. Scalars do not consume it.
    DepthPolicy depth_policy{DepthPolicy::events};

    bool allow_bom{};
};

class Document {
public:
    // Immutable owners may share storage (Operator bindings and frozen payloads).
    Document();

    explicit Document(Value source);

    [[nodiscard]] static Result<Document> parse(std::string_view bytes, ParseOptions options);

    // Startup table data, parsed once and shared immutably. Referencing modules
    // must remain loaded while references or copies survive. Runtime input uses parse().
    template <atelier::detail::StringLiteral Text>
    [[nodiscard]] static const Document& literal() {
        constexpr auto length = sizeof(Text.bytes) - 1;
        static_assert(Text.bytes[length] == '\0' && StringView{Text.bytes, length}.is_valid_utf8());
        static const Document value = [] {
            auto parsed = parse({Text.bytes, sizeof(Text.bytes) - 1},
                                {std::numeric_limits<std::size_t>::max(), DepthPolicy::containers});
            if (!parsed) {
                const auto& error = parsed.error();
                const auto diagnostic = "Invalid JSON literal: " + std::string(Text.bytes, sizeof(Text.bytes) - 1) +
                                        " (offset " + (error.offset ? std::to_string(*error.offset) : "unavailable") +
                                        "): " + error.describe();
                fatal(diagnostic);
            }
            return std::move(*parsed);
        }();
        return value;
    }

    [[nodiscard]] Value root() const&;

    Value root() const&& = delete;

    bool operator==(const Document& other) const { return root() == other.root(); }

private:
    friend class Builder;

    explicit Document(void* doc);

    std::shared_ptr<void> doc_;
};

// A short-lived construction transaction. Strings and keys are copied and
// validated by raw entry points; writer never revalidates them. Nodes cannot cross builders.
class Builder {
public:
    explicit Builder(Kind root = Kind::object);

    explicit Builder(const Document& source);

    explicit Builder(Value source);

    ~Builder();

    Builder(Builder&& other) noexcept;

    Builder& operator=(Builder&& other) noexcept;

    Builder(const Builder&) = delete;

    Builder& operator=(const Builder&) = delete;

    [[nodiscard]] Value root() const&;

    Value root() const&& = delete;

    // Freezes a copied immutable snapshot; subsequent edits cannot affect it.
    [[nodiscard]] Document finish() const;

    [[nodiscard]] Result<Value> object(Value parent, std::string_view key);

    [[nodiscard]] Result<Value> array(Value parent, std::string_view key);

    // Typed contiguous numeric fields (vectors, matrices, histograms). Integers
    // keep their signedness and never pass through double.
    template <class T, std::size_t N>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
    [[nodiscard]] Result<void> numbers(Value parent, std::string_view key, std::span<T, N> values) {
        ATELIER_TRY_BIND(items, array(parent, key));
        for (const auto value : values) {
            if constexpr (std::is_floating_point_v<T>) {
                ATELIER_TRY(append(*items, value));
            } else if constexpr (std::is_signed_v<T>)
                append(*items, static_cast<std::int64_t>(value));
            else
                append(*items, static_cast<std::uint64_t>(value));
        }
        return {};
    }

    [[nodiscard]] Value append_object(Value parent);

    [[nodiscard]] Value append_array(Value parent);

    // Cross-owner values are copied, never attached by borrowing their nodes.
    [[nodiscard]] Result<void> set(Value parent, std::string_view key, Value source);

    // Copies before replacing, so source may belong to the same DOM.
    // All previous views of the replaced subtree are invalid afterwards.
    void replace(Value target, Value source);

    // Replace an attached node, including the root, with a scalar. String bytes
    // are copied; the validated overload has the same UTF-8 contract as below.
    [[nodiscard]] Result<void> set(Value target, std::string_view text);

    void set_validated(Value target, std::string_view text);

    void set(Value target, bool value);

    void set(Value target, std::int64_t value);

    void set(Value target, std::uint64_t value);

    [[nodiscard]] Result<void> set(Value target, float value);

    [[nodiscard]] Result<void> set(Value target, double value);

    template <std::size_t N>
    [[nodiscard]] Result<void> set(Value target, const char (&text)[N]) {
        return set(target, std::string_view{text, N - 1});
    }

    // Parent is an attached object. Removing a member invalidates its subtree
    // views; a missing key leaves the object unchanged and returns false.
    bool erase(Value parent, std::string_view key);

    void append(Value parent, Value source);

    [[nodiscard]] Result<void> append(Value parent, std::string_view text);

    void append(Value parent, bool value);

    void append(Value parent, std::int64_t value);

    void append(Value parent, std::uint64_t value);

    [[nodiscard]] Result<void> append(Value parent, double value);

    [[nodiscard]] Result<void> append(Value parent, float value);

    Value append_null(Value parent);

    template <std::size_t N>
    [[nodiscard]] Result<void> append(Value parent, const char (&text)[N]) {
        return append(parent, std::string_view{text, N - 1});
    }

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, std::string_view text);

    // Both key and text must already be valid UTF-8 and remain unchanged through
    // this call. Neither is rescanned; bytes are copied into this builder.
    // As with other writes, parent must be an attached object of this builder.
    // Violating these preconditions is a contract violation, not a recoverable error;
    // this entry point does not detect arbitrary invalid UTF-8 (in either configuration).
    void set_validated(Value parent, std::string_view key, std::string_view text);

    void set_validated(Value parent, std::string_view key, Value source);

    // The same key/parent preconditions apply; used for admitted field names.
    [[nodiscard]] Value object_validated(Value parent, std::string_view key);

    // Create missing object prefixes, replacing nonobjects. Names are already
    // validated UTF-8 (schema fields or keys borrowed from a parsed document).
    [[nodiscard]] Value object_path_validated(Value parent, std::span<const std::string> keys);

    // Prevent string literals selecting the pointer-to-bool conversion.
    template <std::size_t N>
    [[nodiscard]] Result<void> set(Value parent, std::string_view key, const char (&text)[N]) {
        return set(parent, key, std::string_view{text, N - 1});
    }

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, bool value);

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, std::uint32_t value);

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, std::uint64_t value);

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, std::int64_t value);

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, float value);

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, double value);

    [[nodiscard]] Result<void> set_null(Value parent, std::string_view key);

private:
    void replace_node(Value target, const void* source);

    Result<void> put(Value parent, std::string_view key, void* value);

    void put_validated(Value parent, std::string_view key, void* value);

    Value push(Value parent, void* value);

    void* copy(Value source);

    void* doc_;
};

struct WriteOptions {
    bool pretty{};

    bool trailing_lf{};
};

// UTF-8 JSON bytes, without a terminator. No String conversion or validation.
[[nodiscard]] Array<char> write(Value root, WriteOptions options = {});

}
