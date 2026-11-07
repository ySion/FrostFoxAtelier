#include "json/json.hpp"

#include "foundation/string.hpp"
#include "memory/fatal.hpp"
#include <yyjson.h>
#include <mimalloc.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace atelier::json {
namespace {
void* allocate(void*, std::size_t size) {
    auto* result = mi_malloc(size);
    if (!result)
        fatal("JSON allocation failed");
    return result;
}

void* reallocate(void*, void* pointer, std::size_t, std::size_t size) {
    auto* result = mi_realloc(pointer, size);
    if (!result)
        fatal("JSON allocation failed");
    return result;
}

void release(void*, void* pointer) {
    mi_free(pointer);
}

const yyjson_alc allocator{allocate, reallocate, release, nullptr};

std::string child_path(std::string_view parent, std::string_view key) {
    std::string result{parent};
    result += '/';
    for (char c : key) {
        if (c == '~')
            result += "~0";
        else if (c == '/')
            result += "~1";
        else
            result += c;
    }
    return result;
}
}

struct Access {
    static yyjson_val* scalar(const Value& value) { return static_cast<yyjson_val*>(const_cast<void*>(value.node_)); }

    static yyjson_mut_val* mutable_node(const Value& value) {
        return static_cast<yyjson_mut_val*>(const_cast<void*>(value.node_));
    }

    template <class F>
    static void object(const Value& value, F&& visit) {
        std::size_t index;
        std::size_t count;
        if (value.mutable_) {
            yyjson_mut_val* key;
            yyjson_mut_val* child;
            yyjson_mut_obj_foreach(mutable_node(value), index, count, key, child) visit(
                std::string_view{yyjson_mut_get_str(key), yyjson_mut_get_len(key)}, Value{child, true, value.root_});
        } else {
            yyjson_val* key;
            yyjson_val* child;
            yyjson_obj_foreach(scalar(value), index, count, key, child)
                visit(std::string_view{yyjson_get_str(key), yyjson_get_len(key)}, Value{child, false, value.root_});
        }
    }

    template <class F>
    static void array(const Value& value, F&& visit) {
        std::size_t index;
        std::size_t count;
        if (value.mutable_) {
            yyjson_mut_val* child;
            yyjson_mut_arr_foreach(mutable_node(value), index, count, child)
                visit(index, Value{child, true, value.root_});
        } else {
            yyjson_val* child;
            yyjson_arr_foreach(scalar(value), index, count, child) visit(index, Value{child, false, value.root_});
        }
    }

    static bool locate(Value current, const void* target, std::string& path) {
        if (current.node_ == target)
            return true;
        bool found = false;
        if (yyjson_is_obj(scalar(current))) {
            object(current, [&](auto key, Value child) {
                if (!found && locate(child, target, path)) {
                    path.insert(0, child_path({}, key));
                    found = true;
                }
            });
        } else if (yyjson_is_arr(scalar(current))) {
            array(current, [&](auto index, Value child) {
                if (!found && locate(child, target, path)) {
                    path.insert(0, child_path({}, std::to_string(index)));
                    found = true;
                }
            });
        }
        return found;
    }

    static std::string location(Value value) {
        std::string path;
        [[maybe_unused]] const bool found = locate({value.root_, value.mutable_}, value.node_, path);
        assert(found); // Detached/replaced nodes are invalid borrows.
        return path;
    }
};

Error Value::error(Fault fault, std::string_view message) const {
    return {fault, std::string{message}, Access::location(*this), {}};
}

namespace {
Error member_error(Value parent, std::string_view key, Fault fault, std::string_view message) {
    auto result = parent.error(fault, message);
    result.path = child_path(result.path, key);
    return result;
}
}

bool Value::is_null() const noexcept {
    return yyjson_is_null(Access::scalar(*this));
}

std::size_t Value::size() const noexcept {
    return yyjson_get_len(Access::scalar(*this));
}

Result<Value> Value::at(std::string_view key) const {
    if (!yyjson_is_obj(Access::scalar(*this)))
        return std::unexpected(error(Fault::type, "Expected object"));
    const auto* name = key.empty() ? "" : key.data();
    void* child = mutable_ ? static_cast<void*>(yyjson_mut_obj_getn(Access::mutable_node(*this), name, key.size()))
                           : static_cast<void*>(yyjson_obj_getn(Access::scalar(*this), name, key.size()));
    if (!child)
        return std::unexpected(member_error(*this, key, Fault::missing, "Missing member"));
    return Value{child, mutable_, root_};
}

Result<Value> Value::path(std::initializer_list<std::string_view> keys) const {
    auto result = *this;
    for (auto key : keys) {
        ATELIER_TRY_BIND(next, result.at(key));
        result = std::move(*next);
    }
    return result;
}

Result<void> Value::members(std::initializer_list<std::string_view> keys) const {
    if (!yyjson_is_obj(Access::scalar(*this)) || size() != keys.size())
        return std::unexpected(error(Fault::members, "Unexpected object members"));
    for (auto key : keys)
        ATELIER_TRY(at(key));
    return {};
}

Result<std::string_view> Value::string(std::initializer_list<std::string_view> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->string();
    }
    auto* value = Access::scalar(*this);
    if (!yyjson_is_str(value))
        return std::unexpected(error(Fault::type, "Expected string"));
    return std::string_view{yyjson_get_str(value), yyjson_get_len(value)};
}

Result<bool> Value::boolean(std::initializer_list<std::string_view> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->boolean();
    }
    if (!yyjson_is_bool(Access::scalar(*this)))
        return std::unexpected(error(Fault::type, "Expected boolean"));
    return yyjson_get_bool(Access::scalar(*this));
}

Result<std::uint32_t> Value::u32(std::initializer_list<std::string_view> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->u32();
    }
    auto* value = Access::scalar(*this);
    if (!yyjson_is_int(value))
        return std::unexpected(error(Fault::type, "Expected unsigned integer"));
    if ((yyjson_is_sint(value) && yyjson_get_sint(value) < 0) ||
        yyjson_get_uint(value) > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(error(Fault::range, "Integer exceeds uint32"));
    return static_cast<std::uint32_t>(yyjson_get_uint(value));
}

Result<double> Value::number(std::initializer_list<std::string_view> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->number();
    }
    if (!yyjson_is_num(Access::scalar(*this)))
        return std::unexpected(error(Fault::type, "Expected number"));
    return yyjson_get_num(Access::scalar(*this));
}

Result<Array<std::pair<std::string_view, Value>>> Value::object_items() const {
    if (!yyjson_is_obj(Access::scalar(*this)))
        return std::unexpected(error(Fault::type, "Expected object"));
    Array<std::pair<std::string_view, Value>> result;
    result.reserve(size());
    Access::object(*this, [&](auto key, Value child) { result.emplace(key, child); });
    return result;
}

Document::~Document() {
    yyjson_doc_free(static_cast<yyjson_doc*>(doc_));
}

Document::Document(Document&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}

Document& Document::operator=(Document&& other) noexcept {
    std::swap(doc_, other.doc_);
    return *this;
}

Value Document::root() const& {
    return {yyjson_doc_get_root(static_cast<yyjson_doc*>(doc_)), false};
}

namespace {
Result<void> validate(Value value, std::size_t depth, std::size_t limit) {
    if (depth > limit)
        return std::unexpected(Error{Fault::depth, "JSON event depth exceeded", {}, {}});
    std::optional<Error> failure;
    if (yyjson_is_obj(Access::scalar(value))) {
        std::unordered_set<std::string_view> keys;
        Access::object(value, [&](auto key, Value child) {
            if (failure)
                return;
            if (depth == limit)
                failure = Error{Fault::depth, "JSON key event depth exceeded", {}, {}};
            else if (!keys.insert(key).second)
                failure = Error{Fault::duplicate_key, "Duplicate JSON key", {}, {}};
            else if (auto result = validate(child, depth + 1, limit); !result)
                failure = std::move(result.error());
            if (failure)
                failure->path.insert(0, child_path({}, key));
        });
    } else if (yyjson_is_arr(Access::scalar(value))) {
        Access::array(value, [&](auto index, Value child) {
            if (failure)
                return;
            if (auto result = validate(child, depth + 1, limit); !result) {
                failure = std::move(result.error());
                failure->path.insert(0, child_path({}, std::to_string(index)));
            }
        });
    }
    if (failure)
        return std::unexpected(std::move(*failure));
    return {};
}
}

Result<Document> Document::parse(std::string_view bytes, ParseOptions options) {
    // Before allocating the DOM, bound container nesting. Syntax, Unicode and
    // actual event depths remain the parser/validator's responsibility.
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '[' || c == '{') {
            if (depth++ > options.maximum_event_depth)
                return std::unexpected(Error{Fault::depth, "JSON nesting exceeded", {}, i});
        } else if ((c == ']' || c == '}') && depth)
            --depth;
    }
    yyjson_read_err error{};
    auto* doc = yyjson_read_opts(const_cast<char*>(bytes.data()), bytes.size(), 0, &allocator, &error);
    if (!doc) {
        Error result{Fault::syntax, error.msg, {}, error.pos};
        yyjson_locate_pos(bytes.data(), bytes.size(), error.pos, &result.line, &result.column, nullptr);
        return std::unexpected(std::move(result));
    }
    Document result{doc};
    ATELIER_TRY(validate(result.root(), 0, options.maximum_event_depth));
    return result;
}

Builder::Builder() : doc_(yyjson_mut_doc_new(&allocator)) {
    auto* doc = static_cast<yyjson_mut_doc*>(doc_);
    yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
}

Builder::Builder(const Document& source)
    : doc_(yyjson_doc_mut_copy(static_cast<yyjson_doc*>(source.doc_), &allocator)) {}

Builder::~Builder() {
    yyjson_mut_doc_free(static_cast<yyjson_mut_doc*>(doc_));
}

Builder::Builder(Builder&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}

Builder& Builder::operator=(Builder&& other) noexcept {
    std::swap(doc_, other.doc_);
    return *this;
}

Value Builder::root() const& {
    return {yyjson_mut_doc_get_root(static_cast<yyjson_mut_doc*>(doc_)), true};
}

Result<void> Builder::put(Value parent, std::string_view key, void* value) {
    if (!StringView{key}.is_valid_utf8())
        return std::unexpected(member_error(parent, key, Fault::encoding, "Invalid UTF-8 key"));
    put_validated(parent, key, value);
    return {};
}

void Builder::put_validated(Value parent, std::string_view key, void* value) {
    assert(parent.mutable_ && yyjson_is_obj(Access::scalar(parent)));
    auto* doc = static_cast<yyjson_mut_doc*>(doc_);
    auto* name = yyjson_mut_strncpy(doc, key.empty() ? "" : key.data(), key.size());
    yyjson_mut_obj_put(Access::mutable_node(parent), name, static_cast<yyjson_mut_val*>(value));
}

Result<Value> Builder::object(Value parent, std::string_view key) {
    if (!StringView{key}.is_valid_utf8())
        return std::unexpected(member_error(parent, key, Fault::encoding, "Invalid UTF-8 key"));
    return object_validated(parent, key);
}

Value Builder::object_validated(Value parent, std::string_view key) {
    auto* child = yyjson_mut_obj(static_cast<yyjson_mut_doc*>(doc_));
    put_validated(parent, key, child);
    return Value{child, true, parent.root_};
}

Result<void> Builder::set(Value parent, std::string_view key, std::string_view text) {
    if (!StringView{text}.is_valid_utf8())
        return std::unexpected(member_error(parent, key, Fault::encoding, "Invalid UTF-8 string"));
    return put(parent, key,
               yyjson_mut_strncpy(static_cast<yyjson_mut_doc*>(doc_), text.empty() ? "" : text.data(), text.size()));
}

void Builder::set_validated(Value parent, std::string_view key, std::string_view text) {
    put_validated(parent, key,
                  yyjson_mut_strncpy(static_cast<yyjson_mut_doc*>(doc_), text.empty() ? "" : text.data(), text.size()));
}

Result<void> Builder::set(Value parent, std::string_view key, bool value) {
    return put(parent, key, yyjson_mut_bool(static_cast<yyjson_mut_doc*>(doc_), value));
}

Result<void> Builder::set(Value parent, std::string_view key, std::uint32_t value) {
    return put(parent, key, yyjson_mut_uint(static_cast<yyjson_mut_doc*>(doc_), value));
}

Result<void> Builder::set(Value parent, std::string_view key, double value) {
    if (!std::isfinite(value))
        return std::unexpected(member_error(parent, key, Fault::range, "Nonfinite JSON number"));
    return put(parent, key, yyjson_mut_real(static_cast<yyjson_mut_doc*>(doc_), value));
}

Result<void> Builder::set_null(Value parent, std::string_view key) {
    return put(parent, key, yyjson_mut_null(static_cast<yyjson_mut_doc*>(doc_)));
}

namespace {
class Writer {
public:
    explicit Writer(WriteOptions options) : options_(options) {}

    Array<char> finish(Value root) {
        value(root, 0);
        if (options_.trailing_lf)
            output_.emplace('\n');
        return std::move(output_);
    }

private:
    struct Member {
        std::string_view key;

        Value value;
    };

    void append(std::string_view text) { output_.append({text.data(), text.size()}); }

    void newline(std::size_t depth) {
        if (!options_.pretty)
            return;
        output_.emplace('\n');
        for (std::size_t i = 0; i < depth * 2; ++i)
            output_.emplace(' ');
    }

    void string(std::string_view text) {
        constexpr char hex[] = "0123456789abcdef";
        output_.emplace('"');
        for (unsigned char c : text) {
            switch (c) {
            case '"':
                append("\\\"");
                break;
            case '\\':
                append("\\\\");
                break;
            case '\b':
                append("\\b");
                break;
            case '\f':
                append("\\f");
                break;
            case '\n':
                append("\\n");
                break;
            case '\r':
                append("\\r");
                break;
            case '\t':
                append("\\t");
                break;
            default:
                if (c < 0x20) {
                    append("\\u00");
                    output_.emplace(hex[c >> 4]);
                    output_.emplace(hex[c & 15]);
                } else
                    output_.emplace(static_cast<char>(c));
            }
        }
        output_.emplace('"');
    }

    void value(Value node, std::size_t depth) {
        auto* raw = Access::scalar(node);
        if (yyjson_is_obj(raw)) {
            output_.emplace('{');
            const auto base = members_.size();
            Access::object(node, [&](auto key, Value child) { members_.emplace(key, std::move(child)); });
            const auto end = members_.size();
            if (end != base)
                std::sort(members_.begin() + base, members_.end(),
                          [](const auto& a, const auto& b) { return a.key < b.key; });
            for (auto i = base; i < end; ++i) {
                if (i != base)
                    output_.emplace(',');
                newline(depth + 1);
                // Recursion can reallocate the member stack. Copy the borrowed
                // references, never retain a reference to an element in it.
                auto member = members_[i];
                string(member.key);
                append(options_.pretty ? ": " : ":");
                value(member.value, depth + 1);
            }
            while (members_.size() != base)
                members_.pop();
            if (end != base)
                newline(depth);
            output_.emplace('}');
        } else if (yyjson_is_arr(raw)) {
            output_.emplace('[');
            Access::array(node, [&](auto index, Value child) {
                if (index)
                    output_.emplace(',');
                newline(depth + 1);
                value(child, depth + 1);
            });
            if (node.size())
                newline(depth);
            output_.emplace(']');
        } else if (yyjson_is_str(raw))
            string({yyjson_get_str(raw), yyjson_get_len(raw)});
        else if (yyjson_is_num(raw)) {
            char buffer[40];
            auto* end = yyjson_write_number(raw, buffer);
            append({buffer, static_cast<std::size_t>(end - buffer)});
        } else if (yyjson_is_bool(raw))
            append(yyjson_get_bool(raw) ? "true" : "false");
        else
            append("null");
    }

    WriteOptions options_;

    Array<char> output_;

    Array<Member> members_;
};
}

Array<char> write(Value root, WriteOptions options) {
    return Writer{options}.finish(root);
}

}
