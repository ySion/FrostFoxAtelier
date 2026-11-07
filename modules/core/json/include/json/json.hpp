#pragma once

#include "foundation/array.hpp"
#include "foundation/result.hpp"
#include <expected>
#include <initializer_list>
#include <optional>
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
};

template <class T>
using Result = std::expected<T, Error>;

class Document;

class Builder;

// Borrowed node and stable DOM root, not an owner-wrapper or parent-Value address.
// Owner destruction or replacement of this subtree invalidates the view. Moving
// the owner preserves it. Error paths are recovered only on failure.
class Value {
public:
    [[nodiscard]] bool is_null() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] Result<Value> at(std::string_view key) const;

    [[nodiscard]] Result<Value> path(std::initializer_list<std::string_view> keys) const;

    [[nodiscard]] Result<void> members(std::initializer_list<std::string_view> keys) const;

    [[nodiscard]] Result<std::string_view> string(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<bool> boolean(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<std::uint32_t> u32(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<double> number(std::initializer_list<std::string_view> keys = {}) const;

    [[nodiscard]] Result<Array<std::pair<std::string_view, Value>>> object_items() const;

    [[nodiscard]] Error error(Fault fault, std::string_view message) const;

private:
    friend class Document;

    friend class Builder;

    friend struct Access;

    Value(const void* node, bool mutable_node, const void* root = nullptr)
        : node_(node), mutable_(mutable_node), root_(root ? root : node) {}

    const void* node_;

    bool mutable_;

    const void* root_;
};

// Limits are supplied by the input owner. Event depth counts keys and scalars
// as well as containers; zero is the root container's start event.
struct ParseOptions {
    std::size_t maximum_event_depth;
};

class Document {
public:
    ~Document();

    Document(Document&& other) noexcept;

    Document& operator=(Document&& other) noexcept;

    Document(const Document&) = delete;

    Document& operator=(const Document&) = delete;

    [[nodiscard]] static Result<Document> parse(std::string_view bytes, ParseOptions options);

    [[nodiscard]] Value root() const&;

    Value root() const&& = delete;

private:
    friend class Builder;

    explicit Document(void* doc) : doc_(doc) {}

    void* doc_;
};

// A short-lived construction transaction. Strings and keys are copied and
// validated by raw entry points; writer never revalidates them. Nodes cannot cross builders.
class Builder {
public:
    Builder();

    explicit Builder(const Document& source);

    ~Builder();

    Builder(Builder&& other) noexcept;

    Builder& operator=(Builder&& other) noexcept;

    Builder(const Builder&) = delete;

    Builder& operator=(const Builder&) = delete;

    [[nodiscard]] Value root() const&;

    Value root() const&& = delete;

    [[nodiscard]] Result<Value> object(Value parent, std::string_view key);

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, std::string_view text);

    // Both key and text must already be valid UTF-8 and remain unchanged through
    // this call. Neither is rescanned; bytes are copied into this builder.
    // As with other writes, parent must be an attached object of this builder.
    // Violating these preconditions is a contract violation, not a recoverable error;
    // this entry point does not detect arbitrary invalid UTF-8 (in either configuration).
    void set_validated(Value parent, std::string_view key, std::string_view text);

    // The same key/parent preconditions apply; used for admitted field names.
    [[nodiscard]] Value object_validated(Value parent, std::string_view key);

    // Prevent string literals selecting the pointer-to-bool conversion.
    template <std::size_t N>
    [[nodiscard]] Result<void> set(Value parent, std::string_view key, const char (&text)[N]) {
        return set(parent, key, std::string_view{text, N - 1});
    }

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, bool value);

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, std::uint32_t value);

    [[nodiscard]] Result<void> set(Value parent, std::string_view key, double value);

    [[nodiscard]] Result<void> set_null(Value parent, std::string_view key);

private:
    Result<void> put(Value parent, std::string_view key, void* value);

    void put_validated(Value parent, std::string_view key, void* value);

    void* doc_;
};

struct WriteOptions {
    bool pretty{};

    bool trailing_lf{};
};

// UTF-8 JSON bytes, without a terminator. No String conversion or validation.
[[nodiscard]] Array<char> write(Value root, WriteOptions options = {});

}
