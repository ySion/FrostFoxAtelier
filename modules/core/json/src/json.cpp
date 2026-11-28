#include "json/json.hpp"

#include "foundation/inline_array.hpp"
#include "foundation/map.hpp"
#include "foundation/set.hpp"
#include "foundation/string.hpp"
#include "memory/fatal.hpp"
#include <yyjson.h>
#include <mimalloc.h>
#include <algorithm>
#include <cassert>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>

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

String child_path(StringView parent, StringView key) {
    String result;
    result.reserve(parent.size() + 1 + key.size());
    if (parent.is_valid_utf8())
        result.append_validated(parent);
    result.append_ascii('/');
    if (key.is_valid_utf8()) {
        std::size_t plain = 0;
        for (std::size_t i = 0; i < key.size(); ++i) {
            if (key[i] == '~' || key[i] == '/') {
                if (i > plain)
                    result.append_validated(key.slice(plain, i - plain));
                result.append_validated(key[i] == '~' ? "~0" : "~1");
                plain = i + 1;
            }
        }
        if (plain < key.size())
            result.append_validated(key.slice(plain));
    } else {
        for (std::size_t i = 0; i < key.size(); ++i) {
            const auto b = static_cast<unsigned char>(key[i]);
            if (b == '~')
                result.append_literal<"~0">();
            else if (b == '/')
                result.append_literal<"~1">();
            else if (b >= 0x20 && b < 0x7f)
                result.append_ascii(key[i]);
            else {
                result.append_ascii('%');
                result.append_hex(b, 2, true);
            }
        }
    }
    return result;
}
}

struct Access {
    static bool mutable_layout(Value value) { return value.storage_ != Value::Storage::immutable; }

    static yyjson_val* scalar(const Value& value) { return static_cast<yyjson_val*>(const_cast<void*>(value.node_)); }

    static yyjson_mut_val* mutable_node(const Value& value) {
        return static_cast<yyjson_mut_val*>(const_cast<void*>(value.node_));
    }

    template <class F>
    static void object(const Value& value, F&& visit) {
        std::size_t index;
        std::size_t count;
        if (mutable_layout(value)) {
            yyjson_mut_val* key;
            yyjson_mut_val* child;
            yyjson_mut_obj_foreach(mutable_node(value), index, count, key, child)
                visit(StringView{yyjson_mut_get_str(key), yyjson_mut_get_len(key)},
                      Value{child, value.storage_, value.root_});
        } else {
            yyjson_val* key;
            yyjson_val* child;
            yyjson_obj_foreach(scalar(value), index, count, key, child)
                visit(StringView{yyjson_get_str(key), yyjson_get_len(key)}, Value{child, value.storage_, value.root_});
        }
    }

    template <class F>
    static void array(const Value& value, F&& visit) {
        std::size_t index;
        std::size_t count;
        if (mutable_layout(value)) {
            yyjson_mut_val* child;
            yyjson_mut_arr_foreach(mutable_node(value), index, count, child)
                visit(index, Value{child, value.storage_, value.root_});
        } else {
            yyjson_val* child;
            yyjson_arr_foreach(scalar(value), index, count, child)
                visit(index, Value{child, value.storage_, value.root_});
        }
    }

    static bool locate(Value current, const void* target, String& path) {
        if (current.node_ == target)
            return true;
        struct Node {
            Value value;

            std::size_t parent;

            StringView key;

            std::size_t index;
        };
        Array<Node> nodes;
        nodes.emplace(current, 0, StringView{}, 0);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto value = nodes[i].value;
            if (value.node_ == target) {
                Array<std::size_t> ancestors;
                for (auto at = i; at; at = nodes[at].parent)
                    ancestors.emplace(at);
                while (!ancestors.empty()) {
                    const auto& node = nodes[ancestors.back()];
                    if (nodes[node.parent].value.kind() == Kind::object) {
                        path = child_path(path, node.key);
                    } else {
                        path.append_ascii('/');
                        path.append_number(node.index);
                    }
                    ancestors.pop();
                }
                return true;
            }
            if (value.kind() == Kind::object)
                object(value, [&](auto key, Value child) { nodes.emplace(child, i, key, 0); });
            else if (value.kind() == Kind::array)
                array(value, [&](auto index, Value child) { nodes.emplace(child, i, StringView{}, index); });
        }
        return false;
    }

    static String location(Value value) {
        String path;
        [[maybe_unused]] const bool found = locate({value.root_, value.storage_}, value.node_, path);
        assert(found); // Detached/replaced nodes are invalid borrows.
        return path;
    }
};

Error Value::error(Fault fault, StringView message) const {
    return {fault, String::from_utf8(message).value_or("JSON error"_s), Access::location(*this), {}};
}

String Error::describe() const {
    auto result = message;
    if (!path.empty()) {
        result.append_literal<" at ">();
        result.append(path);
    }
    if (offset) {
        result.append_literal<" at byte ">();
        result.append_number(*offset);
    }
    return result;
}

namespace {
Error member_error(Value parent, StringView key, Fault fault, StringView message) {
    auto result = parent.error(fault, message);
    result.path = child_path(result.path, key);
    return result;
}

Error index_error(Value parent, std::size_t index, Fault fault, StringView message) {
    auto result = parent.error(fault, message);
    result.path.append_ascii('/');
    result.path.append_number(index);
    return result;
}
}

Kind Value::kind() const noexcept {
    auto* value = Access::scalar(*this);
    if (yyjson_is_obj(value))
        return Kind::object;
    if (yyjson_is_arr(value))
        return Kind::array;
    if (yyjson_is_str(value))
        return Kind::string;
    if (yyjson_is_int(value))
        return Kind::integer;
    if (yyjson_is_real(value))
        return Kind::real;
    if (yyjson_is_bool(value))
        return Kind::boolean;
    return Kind::null;
}

bool Value::is_null() const noexcept {
    return yyjson_is_null(Access::scalar(*this));
}

std::size_t Value::size() const noexcept {
    return yyjson_get_len(Access::scalar(*this));
}

Result<Value> Value::at(std::size_t index) const {
    if (kind() != Kind::array)
        return std::unexpected(error(Fault::type, "Expected array"));
    if (index >= size())
        return std::unexpected(index_error(*this, index, Fault::range, "Array index out of range"));
    const void* child = Access::mutable_layout(*this)
                            ? static_cast<void*>(yyjson_mut_arr_get(Access::mutable_node(*this), index))
                            : static_cast<void*>(yyjson_arr_get(Access::scalar(*this), index));
    return Value{child, storage_, root_};
}

Result<Optional<Value>> Value::find(StringView key) const {
    if (!yyjson_is_obj(Access::scalar(*this)))
        return std::unexpected(error(Fault::type, "Expected object"));
    const auto* name = key.empty() ? "" : key.data();
    void* child = Access::mutable_layout(*this)
                      ? static_cast<void*>(yyjson_mut_obj_getn(Access::mutable_node(*this), name, key.size()))
                      : static_cast<void*>(yyjson_obj_getn(Access::scalar(*this), name, key.size()));
    if (!child)
        return Optional<Value>{};
    return Value{child, storage_, root_};
}

Result<Value> Value::at(StringView key) const {
    ATELIER_TRY_BIND(child, find(key));
    if (!*child)
        return std::unexpected(member_error(*this, key, Fault::missing, "Missing member"));
    return **child;
}

Result<Value> Value::path(std::initializer_list<StringView> keys) const {
    auto result = *this;
    for (auto key : keys) {
        ATELIER_TRY_BIND(next, result.at(key));
        result = std::move(*next);
    }
    return result;
}

Result<void> Value::members(std::initializer_list<StringView> keys) const {
    if (!yyjson_is_obj(Access::scalar(*this)) || size() != keys.size())
        return std::unexpected(error(Fault::members, "Unexpected object members"));
    for (auto key : keys)
        ATELIER_TRY(at(key));
    return {};
}

Result<void> Value::allowed_members(std::initializer_list<StringView> keys) const {
    if (kind() != Kind::object)
        return std::unexpected(error(Fault::type, "Expected object"));
    Optional<Error> failure;
    Access::object(*this, [&](auto key, Value child) {
        if (!failure && std::ranges::find(keys, key) == keys.end())
            failure = child.error(Fault::members, "Unsupported member");
    });
    if (failure)
        return std::unexpected(std::move(*failure));
    return {};
}

Result<StringView> Value::string(std::initializer_list<StringView> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->string();
    }
    auto* value = Access::scalar(*this);
    if (!yyjson_is_str(value))
        return std::unexpected(error(Fault::type, "Expected string"));
    return StringView{yyjson_get_str(value), yyjson_get_len(value)};
}

Result<bool> Value::boolean(std::initializer_list<StringView> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->boolean();
    }
    if (!yyjson_is_bool(Access::scalar(*this)))
        return std::unexpected(error(Fault::type, "Expected boolean"));
    return yyjson_get_bool(Access::scalar(*this));
}

Result<std::uint32_t> Value::u32(std::initializer_list<StringView> keys) const {
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

Result<double> Value::number(std::initializer_list<StringView> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->number();
    }
    auto* value = Access::scalar(*this);
    if (!yyjson_is_num(value))
        return std::unexpected(error(Fault::type, "Expected number"));
    return yyjson_get_num(value);
}

Result<std::uint64_t> Value::u64(std::initializer_list<StringView> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->u64();
    }
    auto* value = Access::scalar(*this);
    if (!yyjson_is_int(value))
        return std::unexpected(error(Fault::type, "Expected unsigned integer"));
    if (yyjson_is_sint(value) && yyjson_get_sint(value) < 0)
        return std::unexpected(error(Fault::range, "Negative unsigned integer"));
    return yyjson_get_uint(value);
}

Result<std::int64_t> Value::i64(std::initializer_list<StringView> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->i64();
    }
    auto* value = Access::scalar(*this);
    if (!yyjson_is_int(value))
        return std::unexpected(error(Fault::type, "Expected signed integer"));
    if (yyjson_is_uint(value) && yyjson_get_uint(value) > static_cast<std::uint64_t>(INT64_MAX))
        return std::unexpected(error(Fault::range, "Integer exceeds int64"));
    return yyjson_get_sint(value);
}

Result<float> Value::f32(std::initializer_list<StringView> keys) const {
    if (keys.size()) {
        ATELIER_TRY_BIND(node, path(keys));
        return node->f32();
    }
    ATELIER_TRY_BIND(value, number());
    const auto result = static_cast<float>(*value);
    if (!std::isfinite(result))
        return std::unexpected(error(Fault::range, "Number exceeds finite float range"));
    return result;
}

Result<ArrayView> Value::array() const {
    if (kind() != Kind::array)
        return std::unexpected(error(Fault::type, "Expected array"));
    return ArrayView{*this};
}

ArrayView::Iterator ArrayView::begin() const {
    const void* first = Access::mutable_layout(value_)
                            ? static_cast<void*>(yyjson_mut_arr_get_first(Access::mutable_node(value_)))
                            : static_cast<void*>(yyjson_arr_get_first(Access::scalar(value_)));
    return {Value{first, value_.storage_, value_.root_}, size()};
}

Value ArrayView::Iterator::operator*() const {
    assert(remaining_);
    return current_;
}

ArrayView::Iterator& ArrayView::Iterator::operator++() {
    assert(remaining_);
    if (--remaining_) {
        current_.node_ = Access::mutable_layout(current_)
                             ? static_cast<void*>(Access::mutable_node(current_)->next)
                             : static_cast<void*>(unsafe_yyjson_get_next(Access::scalar(current_)));
    }
    return *this;
}

Result<Array<std::pair<StringView, Value>>> Value::object_items() const {
    if (!yyjson_is_obj(Access::scalar(*this)))
        return std::unexpected(error(Fault::type, "Expected object"));
    Array<std::pair<StringView, Value>> result;
    result.reserve(size());
    Access::object(*this, [&](auto key, Value child) { result.emplace(key, child); });
    if (result.size() > 1)
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return result;
}

bool Value::operator==(Value other) const {
    const auto left = kind();
    const auto right = other.kind();
    if ((left == Kind::integer || left == Kind::real) && (right == Kind::integer || right == Kind::real)) {
        if (left == Kind::real && right == Kind::real)
            return *number() == *other.number();
        if (left == Kind::real || right == Kind::real) {
            const auto real = left == Kind::real ? *number() : *other.number();
            const auto* integer = Access::scalar(left == Kind::integer ? *this : other);
            if (std::trunc(real) != real)
                return false;
            if (yyjson_is_sint(integer) && yyjson_get_sint(integer) < 0)
                return real >= -0x1p63 && real < 0 && static_cast<std::int64_t>(real) == yyjson_get_sint(integer);
            return real >= 0 && real < 0x1p64 && static_cast<std::uint64_t>(real) == yyjson_get_uint(integer);
        }
        const auto* a = Access::scalar(*this);
        const auto* b = Access::scalar(other);
        const bool negative_a = yyjson_is_sint(a) && yyjson_get_sint(a) < 0;
        const bool negative_b = yyjson_is_sint(b) && yyjson_get_sint(b) < 0;
        return negative_a == negative_b && yyjson_get_uint(a) == yyjson_get_uint(b);
    }
    if (left != right)
        return false;
    switch (left) {
    case Kind::null:
        return true;
    case Kind::boolean:
        return *boolean() == *other.boolean();
    case Kind::string:
        return *string() == *other.string();
    case Kind::array: {
        if (size() != other.size())
            return false;
        const auto a = *array();
        const auto b = *other.array();
        auto it = b.begin();
        for (auto value : a) {
            if (!(value == *it))
                return false;
            ++it;
        }
        return true;
    }
    case Kind::object: {
        if (size() != other.size())
            return false;
        bool equal = true;
        Access::object(*this, [&](auto key, Value value) {
            if (!equal)
                return;
            const auto candidate = other.find(key);
            equal = candidate->has_value() && value == **candidate;
        });
        return equal;
    }
    default:
        return false;
    }
}

Document::Document(void* doc) : doc_(doc, [](void* value) { yyjson_doc_free(static_cast<yyjson_doc*>(value)); }) {}

Document::Document(void* doc, Value::Storage storage)
    : doc_(doc, [](void* value) { yyjson_mut_doc_free(static_cast<yyjson_mut_doc*>(value)); }), storage_(storage) {}

Document::Document() : Document(Builder{}.finish()) {}

Document::Document(Value source) : Document(Builder{source}.finish()) {}

Value Document::root() const& {
    return {storage_ == Value::Storage::immutable
                ? static_cast<void*>(yyjson_doc_get_root(static_cast<yyjson_doc*>(doc_.get())))
                : static_cast<void*>(yyjson_mut_doc_get_root(static_cast<yyjson_mut_doc*>(doc_.get()))),
            storage_};
}

namespace {
Result<yyjson_doc*> read_document(StringView bytes, bool allow_bom) {
    yyjson_read_err error{};
    auto* doc = yyjson_read_opts(const_cast<char*>(bytes.data()), bytes.size(), allow_bom ? YYJSON_READ_ALLOW_BOM : 0,
                                 &allocator, &error);
    if (!doc) {
        const auto fault = error.code == YYJSON_READ_ERROR_INVALID_STRING && error.pos < bytes.size() &&
                                   static_cast<unsigned char>(bytes[error.pos]) >= 0x80
                               ? Fault::encoding
                               : Fault::syntax;
        Error result{fault, error.msg ? String::from_utf8(error.msg).value_or("JSON parse error"_s) : "JSON parse error"_s,
                     {}, error.pos};
        yyjson_locate_pos(bytes.data(), bytes.size(), error.pos, &result.line, &result.column, nullptr);
        return std::unexpected(std::move(result));
    }
    return doc;
}

Result<void> validate(Value value, std::size_t depth, ParseOptions options) {
    struct Frame {
        Value value;

        Array<std::pair<StringView, Value>> children;

        std::size_t next{};

        Set<StringView> keys;

        explicit Frame(Value node) : value(node) {
            if (node.kind() == Kind::object) {
                Access::object(node, [&](auto key, Value child) { children.emplace(key, child); });
                keys.reserve(children.size());
            } else if (node.kind() == Kind::array) {
                Access::array(node, [&](auto, Value child) { children.emplace(StringView{}, child); });
            }
        }
    };
    const bool events = options.depth_policy == DepthPolicy::events;
    if (events && depth > options.maximum_depth)
        return std::unexpected(Error{Fault::depth, "JSON event depth exceeded"_s, {}, {}});
    Array<Frame> stack;
    stack.emplace(value);
    while (!stack.empty()) {
        auto& frame = stack.back();
        if (frame.next == frame.children.size()) {
            stack.pop();
            continue;
        }
        const auto [key, child] = frame.children[frame.next++];
        const auto child_depth = depth + stack.size();
        if (frame.value.kind() == Kind::object) {
            if (events && child_depth > options.maximum_depth)
                return std::unexpected(child.error(Fault::depth, "JSON key event depth exceeded"));
            if (!frame.keys.add(key))
                return std::unexpected(child.error(Fault::duplicate_key, "Duplicate JSON key"));
        } else if (events && child_depth > options.maximum_depth)
            return std::unexpected(child.error(Fault::depth, "JSON event depth exceeded"));
        if (child.kind() == Kind::object || child.kind() == Kind::array)
            stack.emplace(child);
    }
    return {};
}
}

Result<Document> Document::parse(StringView bytes, ParseOptions options) {
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
            if (options.depth_policy == DepthPolicy::events ? depth > options.maximum_depth
                                                            : depth >= options.maximum_depth)
                return std::unexpected(Error{Fault::depth, "JSON nesting exceeded"_s, {}, i});
            ++depth;
        } else if ((c == ']' || c == '}') && depth)
            --depth;
    }
    ATELIER_TRY_BIND(doc, read_document(bytes, options.allow_bom));
    Document result{*doc};
    ATELIER_TRY(validate(result.root(), 0, options));
    return result;
}

Result<Document> Document::parse_gltf(StringView bytes) {
    ATELIER_TRY_BIND(parsed, read_document(bytes, true));
    auto* doc = yyjson_doc_mut_copy(*parsed, &allocator);
    yyjson_doc_free(*parsed);
    Document result{doc, Value::Storage::readonly_mutable};
    Array<yyjson_mut_val*> pending;
    pending.emplace(yyjson_mut_doc_get_root(doc));
    Map<StringView, yyjson_mut_val*> last;
    while (!pending.empty()) {
        auto* node = pending.back();
        pending.pop();
        if (yyjson_mut_is_obj(node)) {
            const auto count = yyjson_mut_obj_size(node);
            if (count == 1) {
                auto iter = yyjson_mut_obj_iter_with(node);
                pending.emplace(yyjson_mut_obj_iter_get_val(yyjson_mut_obj_iter_next(&iter)));
            } else if (count > 1) {
                last.clear();
                last.reserve(count);
                auto scan = yyjson_mut_obj_iter_with(node);
                while (auto* key = yyjson_mut_obj_iter_next(&scan))
                    last.insert_or_assign(StringView{yyjson_mut_get_str(key), yyjson_mut_get_len(key)}, key);
                auto iter = yyjson_mut_obj_iter_with(node);
                while (auto* key = yyjson_mut_obj_iter_next(&iter)) {
                    if (last.at(StringView{yyjson_mut_get_str(key), yyjson_mut_get_len(key)}) != key)
                        yyjson_mut_obj_iter_remove(&iter);
                    else
                        pending.emplace(yyjson_mut_obj_iter_get_val(key));
                }
            }
        } else if (yyjson_mut_is_arr(node)) {
            auto iter = yyjson_mut_arr_iter_with(node);
            while (auto* child = yyjson_mut_arr_iter_next(&iter))
                pending.emplace(child);
        }
    }
    return result;
}

Builder::Builder(Kind root) : doc_(yyjson_mut_doc_new(&allocator)) {
    auto* doc = static_cast<yyjson_mut_doc*>(doc_);
    assert(root == Kind::object || root == Kind::array || root == Kind::null);
    yyjson_mut_doc_set_root(doc, root == Kind::object  ? yyjson_mut_obj(doc)
                                 : root == Kind::array ? yyjson_mut_arr(doc)
                                                       : yyjson_mut_null(doc));
}

Builder::Builder(const Document& source) : Builder(source.root()) {}

Builder::Builder(Value source) : doc_(yyjson_mut_doc_new(&allocator)) {
    yyjson_mut_doc_set_root(static_cast<yyjson_mut_doc*>(doc_), static_cast<yyjson_mut_val*>(copy(source)));
}

void* Builder::copy(Value source) {
    auto* doc = static_cast<yyjson_mut_doc*>(doc_);
    return Access::mutable_layout(source) ? yyjson_mut_val_mut_copy(doc, Access::mutable_node(source))
                                          : yyjson_val_mut_copy(doc, Access::scalar(source));
}

Document Builder::finish() const {
    return Document{yyjson_mut_doc_imut_copy(static_cast<yyjson_mut_doc*>(doc_), &allocator)};
}

Builder::~Builder() {
    yyjson_mut_doc_free(static_cast<yyjson_mut_doc*>(doc_));
}

Builder::Builder(Builder&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}

Builder& Builder::operator=(Builder&& other) noexcept {
    std::swap(doc_, other.doc_);
    return *this;
}

Value Builder::root() const& {
    return {yyjson_mut_doc_get_root(static_cast<yyjson_mut_doc*>(doc_)), Value::Storage::writable};
}

void Builder::require_writable(Value target) const {
    if (target.storage_ != Value::Storage::writable || target.root_ != root().node_)
        fatal("JSON write requires a node owned by this Builder");
}

Result<void> Builder::put(Value parent, StringView key, void* value) {
    if (!key.is_valid_utf8())
        return std::unexpected(member_error(parent, key, Fault::encoding, "Invalid UTF-8 key"));
    put_validated(parent, key, value);
    return {};
}

void Builder::put_validated(Value parent, StringView key, void* value) {
    require_writable(parent);
    assert(yyjson_is_obj(Access::scalar(parent)));
    auto* doc = static_cast<yyjson_mut_doc*>(doc_);
    auto* name = yyjson_mut_strncpy(doc, key.empty() ? "" : key.data(), key.size());
    yyjson_mut_obj_put(Access::mutable_node(parent), name, static_cast<yyjson_mut_val*>(value));
}

Result<Value> Builder::object(Value parent, StringView key) {
    if (!key.is_valid_utf8())
        return std::unexpected(member_error(parent, key, Fault::encoding, "Invalid UTF-8 key"));
    return object_validated(parent, key);
}

Value Builder::object_validated(Value parent, StringView key) {
    auto* child = yyjson_mut_obj(static_cast<yyjson_mut_doc*>(doc_));
    put_validated(parent, key, child);
    return Value{child, Value::Storage::writable, parent.root_};
}

Value Builder::object_path_validated(Value parent, Span<const String> keys) {
    require_writable(parent);
    for (const auto& key : keys) {
        if (parent.kind() != Kind::object)
            replace_node(parent, yyjson_mut_obj(static_cast<yyjson_mut_doc*>(doc_)));
        const auto found = *parent.find(key);
        parent = found ? *found : object_validated(parent, key);
    }
    if (parent.kind() != Kind::object)
        replace_node(parent, yyjson_mut_obj(static_cast<yyjson_mut_doc*>(doc_)));
    return parent;
}

Result<void> Builder::set(Value parent, StringView key, StringView text) {
    if (!text.is_valid_utf8())
        return std::unexpected(member_error(parent, key, Fault::encoding, "Invalid UTF-8 string"));
    return put(parent, key,
               yyjson_mut_strncpy(static_cast<yyjson_mut_doc*>(doc_), text.empty() ? "" : text.data(), text.size()));
}

Result<void> Builder::set(Value parent, StringView key, const String& text) {
    return put(parent, key, yyjson_mut_strncpy(static_cast<yyjson_mut_doc*>(doc_), text.c_str(), text.size()));
}

void Builder::set_validated(Value parent, StringView key, StringView text) {
    put_validated(parent, key,
                  yyjson_mut_strncpy(static_cast<yyjson_mut_doc*>(doc_), text.empty() ? "" : text.data(), text.size()));
}

Result<void> Builder::set_bool(Value parent, StringView key, bool value) {
    return put(parent, key, yyjson_mut_bool(static_cast<yyjson_mut_doc*>(doc_), value));
}

Result<void> Builder::set(Value parent, StringView key, std::uint32_t value) {
    return put(parent, key, yyjson_mut_uint(static_cast<yyjson_mut_doc*>(doc_), value));
}

Result<void> Builder::set(Value parent, StringView key, double value) {
    if (!std::isfinite(value))
        return std::unexpected(member_error(parent, key, Fault::range, "Nonfinite JSON number"));
    return put(parent, key, yyjson_mut_real(static_cast<yyjson_mut_doc*>(doc_), value));
}

Result<void> Builder::set_null(Value parent, StringView key) {
    return put(parent, key, yyjson_mut_null(static_cast<yyjson_mut_doc*>(doc_)));
}

Result<Value> Builder::array(Value parent, StringView key) {
    auto* child = yyjson_mut_arr(static_cast<yyjson_mut_doc*>(doc_));
    ATELIER_TRY(put(parent, key, child));
    return Value{child, Value::Storage::writable, parent.root_};
}

Value Builder::push(Value parent, void* value) {
    require_writable(parent);
    assert(parent.kind() == Kind::array);
    auto* child = static_cast<yyjson_mut_val*>(value);
    yyjson_mut_arr_append(Access::mutable_node(parent), child);
    return Value{child, Value::Storage::writable, parent.root_};
}

Value Builder::append_object(Value parent) {
    return push(parent, yyjson_mut_obj(static_cast<yyjson_mut_doc*>(doc_)));
}

Value Builder::append_array(Value parent) {
    return push(parent, yyjson_mut_arr(static_cast<yyjson_mut_doc*>(doc_)));
}

Result<void> Builder::set(Value parent, StringView key, Value source) {
    return put(parent, key, copy(source));
}

void Builder::set_validated(Value parent, StringView key, Value source) {
    put_validated(parent, key, copy(source));
}

void Builder::replace(Value target, Value source) {
    replace_node(target, copy(source));
}

void Builder::replace_node(Value target, const void* source) {
    require_writable(target);
    const auto* replacement = static_cast<const yyjson_mut_val*>(source);
    auto* value = Access::mutable_node(target);
    value->tag = replacement->tag;
    value->uni = replacement->uni;
}

Result<void> Builder::set(Value target, StringView text) {
    if (!text.is_valid_utf8())
        return std::unexpected(target.error(Fault::encoding, "Invalid UTF-8 string"));
    set_validated(target, text);
    return {};
}

void Builder::set_validated(Value target, StringView text) {
    replace_node(target,
                 yyjson_mut_strncpy(static_cast<yyjson_mut_doc*>(doc_), text.empty() ? "" : text.data(), text.size()));
}

void Builder::set_bool(Value target, bool value) {
    replace_node(target, yyjson_mut_bool(static_cast<yyjson_mut_doc*>(doc_), value));
}

void Builder::set(Value target, std::int64_t value) {
    replace_node(target, yyjson_mut_sint(static_cast<yyjson_mut_doc*>(doc_), value));
}

void Builder::set(Value target, std::uint64_t value) {
    replace_node(target, yyjson_mut_uint(static_cast<yyjson_mut_doc*>(doc_), value));
}

Result<void> Builder::set(Value target, double value) {
    if (!std::isfinite(value))
        return std::unexpected(target.error(Fault::range, "Nonfinite JSON number"));
    replace_node(target, yyjson_mut_real(static_cast<yyjson_mut_doc*>(doc_), value));
    return {};
}

bool Builder::erase(Value parent, StringView key) {
    require_writable(parent);
    assert(parent.kind() == Kind::object);
    return yyjson_mut_obj_remove_keyn(Access::mutable_node(parent), key.empty() ? "" : key.data(), key.size()) !=
           nullptr;
}

Result<void> Builder::set(Value parent, StringView key, std::uint64_t value) {
    return put(parent, key, yyjson_mut_uint(static_cast<yyjson_mut_doc*>(doc_), value));
}

Result<void> Builder::set(Value parent, StringView key, std::int64_t value) {
    return put(parent, key, yyjson_mut_sint(static_cast<yyjson_mut_doc*>(doc_), value));
}

namespace {
yyjson_mut_val* float_value(void* owner, float value) {
    auto* doc = static_cast<yyjson_mut_doc*>(owner);
    if (value == 0 && std::signbit(value))
        return yyjson_mut_real(doc, -0.0);
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), value).ptr;
    double widened{};
    std::from_chars(text, end, widened);
    // Preserve the two double-rounding exceptions ±7.038531e-26.
    if (std::bit_cast<std::uint32_t>(static_cast<float>(widened)) != std::bit_cast<std::uint32_t>(value))
        return yyjson_mut_real(doc, static_cast<double>(value));
    return yyjson_mut_real(doc, widened);
}
}

Result<void> Builder::set(Value parent, StringView key, float value) {
    if (!std::isfinite(value))
        return std::unexpected(member_error(parent, key, Fault::range, "Nonfinite JSON number"));
    return put(parent, key, float_value(doc_, value));
}

Result<void> Builder::set(Value target, float value) {
    if (!std::isfinite(value))
        return std::unexpected(target.error(Fault::range, "Nonfinite JSON number"));
    replace_node(target, float_value(doc_, value));
    return {};
}

void Builder::append(Value parent, Value source) {
    push(parent, copy(source));
}

void Builder::append_bool(Value parent, bool value) {
    push(parent, yyjson_mut_bool(static_cast<yyjson_mut_doc*>(doc_), value));
}

void Builder::append(Value parent, std::int64_t value) {
    push(parent, yyjson_mut_sint(static_cast<yyjson_mut_doc*>(doc_), value));
}

void Builder::append(Value parent, std::uint64_t value) {
    push(parent, yyjson_mut_uint(static_cast<yyjson_mut_doc*>(doc_), value));
}

Value Builder::append_null(Value parent) {
    return push(parent, yyjson_mut_null(static_cast<yyjson_mut_doc*>(doc_)));
}

Result<void> Builder::append(Value parent, StringView text) {
    if (!text.is_valid_utf8())
        return std::unexpected(index_error(parent, parent.size(), Fault::encoding, "Invalid UTF-8 string"));
    append_validated(parent, text);
    return {};
}

void Builder::append_validated(Value parent, StringView text) {
    push(parent, yyjson_mut_strncpy(static_cast<yyjson_mut_doc*>(doc_), text.empty() ? "" : text.data(), text.size()));
}

Result<void> Builder::append(Value parent, double value) {
    if (!std::isfinite(value))
        return std::unexpected(index_error(parent, parent.size(), Fault::range, "Nonfinite JSON number"));
    push(parent, yyjson_mut_real(static_cast<yyjson_mut_doc*>(doc_), value));
    return {};
}

Result<void> Builder::append(Value parent, float value) {
    if (!std::isfinite(value))
        return std::unexpected(index_error(parent, parent.size(), Fault::range, "Nonfinite JSON number"));
    push(parent, float_value(doc_, value));
    return {};
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
        StringView key;

        Value value;
    };

    void append(StringView text) { output_.append({text.data(), text.size()}); }

    void newline(std::size_t depth) {
        if (!options_.pretty)
            return;
        output_.emplace('\n');
        output_.append(depth * 2, ' ');
    }

    void string(StringView text) {
        constexpr char hex[] = "0123456789abcdef";
        output_.emplace('"');
        std::size_t plain = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c >= 0x20 && c != '"' && c != '\\')
                continue;
            if (i > plain)
                append(text.slice(plain, i - plain));
            plain = i + 1;
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
                append("\\u00");
                output_.emplace(hex[c >> 4]);
                output_.emplace(hex[c & 15]);
                break;
            }
        }
        if (plain < text.size())
            append(text.slice(plain));
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
            members_.truncate(base);
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

    InlineArray<Member, 32> members_;
};
}

Array<char> write(Value root, WriteOptions options) {
    return Writer{options}.finish(root);
}

}
