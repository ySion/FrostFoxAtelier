#pragma once
#include "foundation/array.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "ui/code_document.hpp"
#include "ui/frame.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace atelier::ui::detail {

inline Color srgb_hex(std::uint32_t rgb, float alpha = 1.f) noexcept {
    const auto chan = [](std::uint32_t byte) noexcept {
        const float x = static_cast<float>(byte & 0xffu) / 255.f;
        return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
    };
    return {chan(rgb >> 16), chan(rgb >> 8), chan(rgb), alpha};
}

struct CodePalette {
    Color gutter_bg{srgb_hex(0x181a1d)};

    Color gutter_divider{srgb_hex(0x272b31)};

    Color active_line_bg{srgb_hex(0x252930)};

    Color active_gutter_bg{srgb_hex(0x1f2329)};

    Color line_num{srgb_hex(0x606872)};

    Color line_num_active{srgb_hex(0xdce4ec)};

    Color diff_modified{srgb_hex(0x528bbd)};

    Color diff_added{srgb_hex(0x539e5a)};

    Color indent_guide{srgb_hex(0x2a2e34)};

    Color indent_guide_active{srgb_hex(0x4c5c6e)};

    Color occurrence_bg{srgb_hex(0x2f4358, .55f)};

    Color occurrence_border{srgb_hex(0x527699, .65f)};

    Color search_bg{srgb_hex(0x6e501b, .58f)};

    Color search_border{srgb_hex(0xb88a32, .80f)};

    Color search_active_bg{srgb_hex(0x96681a, .78f)};

    Color search_active_border{srgb_hex(0xf2bc4e)};

    Color bracket_bg{srgb_hex(0x364a5e, .62f)};

    Color bracket_border{srgb_hex(0x7cb2e0)};

    Color bracket_unmatched{srgb_hex(0xe56b65)};

    Color error_color{srgb_hex(0xe56b65)};

    Color warning_color{srgb_hex(0xe2a84b)};

    Color info_color{srgb_hex(0x78a9cc)};

    Color error_lens{srgb_hex(0xd86b66, .82f)};

    Color warning_lens{srgb_hex(0xcfa04c, .82f)};

    Color link_underline{srgb_hex(0x74aad8)};

    Color kw{srgb_hex(0xcf8e6d)};

    Color type_prim{srgb_hex(0x56a8d6)};

    Color type_user{srgb_hex(0x5ec4b6)};

    Color func{srgb_hex(0xdcdcaa)};

    Color prop{srgb_hex(0x9cdcfe)};

    Color special_var{srgb_hex(0x7fb4e6)};

    Color literal_const{srgb_hex(0xd19a66)};

    Color attr{srgb_hex(0xc5a3d6)};

    Color number{srgb_hex(0xb5cea8)};

    Color string_lit{srgb_hex(0x6aab73)};

    Color comment{srgb_hex(0x6c7680)};

    Color punct{srgb_hex(0xb2bac2)};

    Color ident{srgb_hex(0xd4dae0)};
};

inline const CodePalette& code_palette() noexcept {
    static const CodePalette p{};
    return p;
}

inline std::uint32_t hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9')
        return static_cast<std::uint32_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint32_t>(c - 'a' + 10);
    return static_cast<std::uint32_t>(c - 'A' + 10);
}

inline std::pair<std::uint32_t, std::uint32_t> word_range_at(StringView line, std::uint32_t col,
                                                             CodeLanguage lang = CodeLanguage::plain) noexcept {
    if (line.empty())
        return {0, 0};
    const auto n = static_cast<std::uint32_t>(line.size());
    std::uint32_t p = std::min(col, n);
    if (p == n && p > 0)
        --p;
    while (p > 0 && (static_cast<unsigned char>(line[p]) & 0xc0u) == 0x80u)
        --p;
    if (!is_code_ident_char(line[p], lang) && p > 0 && is_code_ident_char(line[p - 1], lang))
        --p;
    if (is_code_ident_char(line[p], lang)) {
        std::uint32_t a = p;
        while (a > 0 && is_code_ident_char(line[a - 1], lang))
            --a;
        std::uint32_t b = p + 1;
        while (b < n && is_code_ident_char(line[b], lang))
            ++b;
        return {a, b};
    }
    if (line[p] == ' ' || line[p] == '\t') {
        std::uint32_t a = p;
        while (a > 0 && (line[a - 1] == ' ' || line[a - 1] == '\t'))
            --a;
        std::uint32_t b = p + 1;
        while (b < n && (line[b] == ' ' || line[b] == '\t'))
            ++b;
        return {a, b};
    }
    const auto lead = static_cast<unsigned char>(line[p]);
    const std::uint32_t step = lead < 0x80u ? 1u : lead < 0xe0u ? 2u : lead < 0xf0u ? 3u : 4u;
    return {p, std::min(n, p + step)};
}

// Visits every code byte on a single line that is outside string literals, // line comments,
// and /* ... */ block comments, returning whether the line ends inside an unclosed block comment.
template <class Fn>
inline bool scan_unmasked_line(StringView ln, CodeLanguage lang, bool in_comment, Fn&& on_code_char) noexcept {
    if (lang == CodeLanguage::plain) {
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(ln.size()); ++i)
            on_code_char(i, ln[i]);
        return false;
    }
    bool in_str = false;
    char quote = 0;
    const auto len = static_cast<std::uint32_t>(ln.size());
    for (std::uint32_t i = 0; i < len; ++i) {
        const char c = ln[i];
        const char next = i + 1 < len ? ln[i + 1] : '\0';
        if (in_comment) {
            if (c == '*' && next == '/') {
                in_comment = false;
                ++i;
            }
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < len)
                ++i;
            else if (c == quote)
                in_str = false;
            continue;
        }
        if (lang != CodeLanguage::style_sheet && lang != CodeLanguage::json && c == '/' && next == '/')
            break;
        if (lang != CodeLanguage::json && c == '/' && next == '*') {
            in_comment = true;
            ++i;
            continue;
        }
        if (c == '"' || (lang == CodeLanguage::style_sheet && c == '\'')) {
            in_str = true;
            quote = c;
            continue;
        }
        on_code_char(i, c);
    }
    return in_comment;
}

// Computes block-comment entry state (1 if line begins inside /* ... */) per line.
inline void compute_block_comment_states(const TextEdit& buffer, CodeLanguage lang, Array<std::uint8_t>& out) {
    const std::size_t n = buffer.line_count();
    out.clear();
    out.resize(n, 0);
    if (lang == CodeLanguage::plain || lang == CodeLanguage::json)
        return;
    bool in_comment = false;
    for (std::size_t r = 0; r < n; ++r) {
        out[r] = in_comment ? 1u : 0u;
        in_comment = scan_unmasked_line(buffer.line(r), lang, in_comment, [](std::uint32_t, char) noexcept {});
    }
}

struct CodeTokenSpan {
    std::uint32_t start{};

    std::uint32_t end{};

    Color color;
};

struct CodeHexSwatch {
    std::uint32_t start{};

    std::uint32_t end{};

    Color color;
};

inline bool sorted_contains(const Array<String>& vec, StringView word) noexcept {
    const auto it =
        std::lower_bound(vec.begin(), vec.end(), word, [](const String& a, StringView b) { return a.view() < b; });
    return it != vec.end() && it->view() == word;
}

inline bool is_source_keyword(StringView w) noexcept {
    static constexpr StringView kws[]{"async",       "break", "class",        "const",  "continue", "else",
                                      "enum",        "fail",  "for",          "if",     "in",       "new",
                                      "observe",     "on",    "parallel_for", "return", "state",    "struct",
                                      "try_acquire", "ui",    "while",        "yield"};
    static_assert(std::ranges::is_sorted(kws));
    return std::binary_search(std::begin(kws), std::end(kws), w);
}

inline bool is_primitive_type(StringView w) noexcept {
    static constexpr StringView types[]{
        "ActionBinding", "Array",    "CodeBinding",     "EditBinding",    "Items",          "Key",         "String",
        "TableBinding",  "Text",     "TextEditBinding", "TooltipBinding", "TreeRowBinding", "bool",        "double",
        "float",         "int",      "matrix4",         "matrix4d",       "quaternion",     "quaterniond", "uint",
        "vector2",       "vector2d", "vector3",         "vector3d",       "vector4",        "vector4d",    "void",
        "weak"};
    static_assert(std::ranges::is_sorted(types));
    return std::binary_search(std::begin(types), std::end(types), w);
}

inline bool is_ui_node_kind(StringView w) noexcept {
    static constexpr StringView nodes[]{"Button",    "Checkbox",     "ChoiceButton", "CodeEditor", "ColorPlane",
                                        "Column",    "Combo",        "Foldout",      "List",       "MenuItem",
                                        "NumberRow", "Overlay",      "ParameterRow", "Popup",      "Repeat",
                                        "Row",       "Splitter",     "Tab",          "Table",      "Text",
                                        "TextField", "ToggleButton", "Tree",         "TreeItem",   "Viewport"};
    static_assert(std::ranges::is_sorted(nodes));
    return std::binary_search(std::begin(nodes), std::end(nodes), w);
}

inline void tokenize_code_line(StringView line, CodeLanguage lang, bool start_in_block_comment,
                               const CodeAnnotations* annotations, Array<CodeTokenSpan>& out_spans,
                               Array<CodeHexSwatch>& out_swatches) {
    out_spans.clear();
    out_swatches.clear();
    if (line.empty())
        return;
    const auto& pal = code_palette();
    if (lang == CodeLanguage::plain) {
        out_spans.add({0, static_cast<std::uint32_t>(line.size()), pal.ident});
        return;
    }

    const auto push_span = [&](std::uint32_t s, std::uint32_t e, Color col) {
        if (e <= s)
            return;
        if (!out_spans.empty() && out_spans.back().end == s && out_spans.back().color == col)
            out_spans.back().end = e;
        else
            out_spans.add({s, e, col});
    };

    const auto n = static_cast<std::uint32_t>(line.size());
    std::uint32_t i = 0;
    if (start_in_block_comment) {
        std::uint32_t end_c = n;
        for (std::uint32_t k = 0; k + 1 < n; ++k) {
            if (line[k] == '*' && line[k + 1] == '/') {
                end_c = k + 2;
                break;
            }
        }
        push_span(0, end_c, pal.comment);
        i = end_c;
    }

    bool in_attribute = false;
    int brace_depth_on_line = 0;
    while (i < n) {
        const char c = line[i];
        const char next = i + 1 < n ? line[i + 1] : '\0';
        // Whitespace inherits adjacent run or plain foreground.
        if (c == ' ' || c == '\t') {
            std::uint32_t j = i + 1;
            while (j < n && (line[j] == ' ' || line[j] == '\t'))
                ++j;
            push_span(i, j, out_spans.empty() ? pal.ident : out_spans.back().color);
            i = j;
            continue;
        }
        // Comments.
        if (lang != CodeLanguage::json && lang != CodeLanguage::style_sheet && c == '/' && next == '/') {
            push_span(i, n, pal.comment);
            break;
        }
        if (lang != CodeLanguage::json && c == '/' && next == '*') {
            std::uint32_t j = i + 2;
            while (j + 1 < n && !(line[j] == '*' && line[j + 1] == '/'))
                ++j;
            j = j + 1 < n ? j + 2 : n;
            push_span(i, j, pal.comment);
            i = j;
            continue;
        }
        // Strings.
        if (c == '"' || (lang == CodeLanguage::style_sheet && c == '\'')) {
            const char q = c;
            std::uint32_t j = i + 1;
            while (j < n) {
                if (line[j] == '\\' && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (line[j] == q) {
                    ++j;
                    break;
                }
                const auto u = static_cast<unsigned char>(line[j]);
                const std::uint32_t step = u < 0x80u ? 1u : u < 0xe0u ? 2u : u < 0xf0u ? 3u : 4u;
                j = std::min(n, j + step);
            }
            // In JSON, "key": before ':' is a property!
            Color scol = pal.string_lit;
            if (lang == CodeLanguage::json) {
                std::uint32_t k = j;
                while (k < n && (line[k] == ' ' || line[k] == '\t'))
                    ++k;
                if (k < n && line[k] == ':')
                    scol = pal.prop;
            }
            push_span(i, j, scol);
            i = j;
            continue;
        }
        // StyleSheet hex color #rrggbb / #rrggbbaa + inline swatch!
        if (lang == CodeLanguage::style_sheet && c == '#') {
            std::uint32_t j = i + 1;
            while (j < n && is_ascii_hex(line[j]))
                ++j;
            const std::uint32_t hex_len = j - (i + 1);
            if (hex_len == 6 || hex_len == 8) {
                std::uint32_t rgb = 0;
                for (std::uint32_t k = 0; k < 6; ++k)
                    rgb = (rgb << 4) | hex_nibble(line[i + 1 + k]);
                float alpha = 1.f;
                if (hex_len == 8) {
                    const std::uint32_t ab = (hex_nibble(line[i + 7]) << 4) | hex_nibble(line[i + 8]);
                    alpha = static_cast<float>(ab) / 255.f;
                }
                out_swatches.add({i, j, srgb_hex(rgb, alpha)});
            }
            push_span(i, j, pal.number);
            i = j;
            continue;
        }
        // StyleSheet @tokens or .class or :pseudo.
        if (lang == CodeLanguage::style_sheet && (c == '@' || c == '.' || c == ':')) {
            std::uint32_t j = i + 1;
            if (j < n && line[j] == ':')
                ++j;
            while (j < n && is_code_ident_char(line[j], CodeLanguage::style_sheet))
                ++j;
            if (j > i + 1) {
                push_span(i, j, c == '@' ? pal.kw : (c == '.' ? pal.type_user : pal.attr));
                i = j;
                continue;
            }
        }
        // Numbers (and dip/deg units in .ss).
        if (is_ascii_digit(c) || (c == '.' && is_ascii_digit(next))) {
            std::uint32_t j = i;
            if (c == '0' && (next == 'x' || next == 'X')) {
                j += 2;
                while (j < n && is_ascii_hex(line[j]))
                    ++j;
            } else {
                while (j < n && (is_ascii_digit(line[j]) || line[j] == '.'))
                    ++j;
                if (j < n && (line[j] == 'e' || line[j] == 'E')) {
                    ++j;
                    if (j < n && (line[j] == '+' || line[j] == '-'))
                        ++j;
                    while (j < n && is_ascii_digit(line[j]))
                        ++j;
                }
            }
            while (j < n && is_ascii_ident_continue(line[j]))
                ++j;
            push_span(i, j, pal.number);
            i = j;
            continue;
        }
        // Identifiers.
        if (is_ascii_ident_start(c)) {
            std::uint32_t j = i + 1;
            while (j < n && is_code_ident_char(line[j], lang))
                ++j;
            const StringView word = line.slice(i, j - i);
            std::uint32_t k = j;
            while (k < n && (line[k] == ' ' || line[k] == '\t'))
                ++k;
            const char after = k < n ? line[k] : '\0';
            const char after2 = k + 1 < n ? line[k + 1] : '\0';
            std::uint32_t prev_idx = i;
            while (prev_idx > 0 && (line[prev_idx - 1] == ' ' || line[prev_idx - 1] == '\t'))
                --prev_idx;
            const char before = prev_idx > 0 ? line[prev_idx - 1] : '\0';

            Color col = pal.ident;
            if (lang == CodeLanguage::style_sheet) {
                if (word == "token" || word == "linear-gradient")
                    col = pal.func;
                else if (after == ':' && brace_depth_on_line >= 0 && (before == '{' || before == ';' || prev_idx == 0))
                    col = pal.prop;
                else if (word == "transparent" || word == "none" || word == "solid" || word == "inset" ||
                         word == "outset" || word == "center" || word == "left" || word == "right" || word == "top" ||
                         word == "bottom" || word == "all" || word == "normal" || word == "italic" ||
                         word == "overlay" || word == "stable")
                    col = pal.literal_const;
                else
                    col = pal.type_prim;
            } else if (lang == CodeLanguage::json) {
                if (word == "true" || word == "false" || word == "null")
                    col = pal.literal_const;
            } else {
                if (in_attribute)
                    col = pal.attr;
                else if (word == "true" || word == "false" || word == "null")
                    col = pal.literal_const;
                else if (word == "self" || word == "input" || word == "state" || word == "node" || word == "event")
                    col = pal.special_var;
                else if (is_source_keyword(word) || (lang == CodeLanguage::ui_source && word == "activate"))
                    col = pal.kw;
                else if (is_primitive_type(word))
                    col = pal.type_prim;
                else if (lang == CodeLanguage::ui_source && is_ui_node_kind(word) && before != '.')
                    col = pal.type_user;
                else if (annotations && sorted_contains(annotations->semantic_types, word))
                    col = pal.type_user;
                else if (after == ':' && after2 == ':')
                    col = pal.type_user;
                else if (lang == CodeLanguage::ui_source && after == ':' && after2 != ':' && before != '?')
                    col = pal.prop;
                else if (after == '(')
                    col = pal.func;
                else if (annotations && sorted_contains(annotations->semantic_functions, word))
                    col = pal.func;
                else if (before == '.')
                    col = pal.prop;
                else if (annotations && sorted_contains(annotations->semantic_fields, word))
                    col = pal.prop;
                else if (word[0] >= 'A' && word[0] <= 'Z')
                    col = pal.type_user;
            }
            push_span(i, j, col);
            i = j;
            continue;
        }
        // Track declaration or parameter [Attribute(...)] in .source files.
        if (c == '[' && lang == CodeLanguage::source) {
            std::uint32_t p = i;
            while (p > 0 && (line[p - 1] == ' ' || line[p - 1] == '\t'))
                --p;
            if (p == 0 || line[p - 1] == '(' || line[p - 1] == ',')
                in_attribute = true;
        } else if (c == ']' && in_attribute) {
            push_span(i, i + 1, pal.attr);
            in_attribute = false;
            ++i;
            continue;
        }
        if (c == '{')
            ++brace_depth_on_line;
        else if (c == '}')
            --brace_depth_on_line;
        const auto u = static_cast<unsigned char>(c);
        const std::uint32_t step = u < 0x80u ? 1u : u < 0xe0u ? 2u : u < 0xf0u ? 3u : 4u;
        const std::uint32_t next_i = std::min(n, i + step);
        push_span(i, next_i, in_attribute ? pal.attr : pal.punct);
        i = next_i;
    }
}

struct CodeBracketMatch {
    std::uint32_t first{};

    std::uint32_t second{};

    bool matched{};
};

inline bool is_open_bracket(char c) noexcept {
    return c == '(' || c == '[' || c == '{';
}

inline bool is_close_bracket(char c) noexcept {
    return c == ')' || c == ']' || c == '}';
}

inline char matching_bracket(char c) noexcept {
    switch (c) {
    case '(':
        return ')';
    case ')':
        return '(';
    case '[':
        return ']';
    case ']':
        return '[';
    case '{':
        return '}';
    case '}':
        return '{';
    default:
        return '\0';
    }
}

inline bool is_unmasked_bracket_at(const TextEdit& buffer, CodeLanguage lang, Span<const std::uint8_t> block_comments,
                                   std::size_t row, std::uint32_t col) noexcept {
    const bool start_blk = row < block_comments.size() && block_comments[row] != 0;
    bool hit = false;
    scan_unmasked_line(buffer.line(row), lang, start_blk, [&](std::uint32_t idx, char c) noexcept {
        if (idx == col && (is_open_bracket(c) || is_close_bracket(c)))
            hit = true;
    });
    return hit;
}

// Lexically accurate bounded bracket matcher around caret (skips strings, // and /* ... */ comments).
inline Optional<CodeBracketMatch> find_matching_bracket(const TextEdit& buffer, CodeLanguage lang,
                                                        Span<const std::uint8_t> block_comments, std::uint32_t caret) {
    const auto& text = buffer.current().text;
    if (text.empty())
        return {};
    const auto n = static_cast<std::uint32_t>(text.size());
    std::uint32_t pos = n;
    const auto try_pick = [&](std::uint32_t cand) noexcept {
        if (cand >= n || (!is_open_bracket(text[cand]) && !is_close_bracket(text[cand])))
            return false;
        const std::size_t r = buffer.line_at(cand);
        return is_unmasked_bracket_at(buffer, lang, block_comments, r, cand - buffer.line_start(r));
    };
    if (try_pick(caret))
        pos = caret;
    else if (caret > 0 && caret <= n && try_pick(caret - 1))
        pos = caret - 1;
    if (pos == n)
        return {};

    const char ch = text[pos];
    const char want = matching_bracket(ch);
    const bool forward = is_open_bracket(ch);
    const std::size_t pos_row = buffer.line_at(pos);
    const std::uint32_t pos_col = pos - buffer.line_start(pos_row);
    constexpr std::size_t max_rows = 2048;
    int depth = 1;

    if (forward) {
        const std::size_t limit_row = std::min(buffer.line_count(), pos_row + max_rows);
        bool in_blk = pos_row < block_comments.size() && block_comments[pos_row] != 0;
        Optional<std::uint32_t> found;
        for (std::size_t r = pos_row; r < limit_row && !found; ++r) {
            const std::uint32_t rs = buffer.line_start(r);
            in_blk = scan_unmasked_line(buffer.line(r), lang, in_blk, [&](std::uint32_t col, char cur) noexcept {
                if (found || (r == pos_row && col <= pos_col))
                    return;
                if (cur == ch)
                    ++depth;
                else if (cur == want && --depth == 0)
                    found.emplace(rs + col);
            });
        }
        if (found)
            return Optional<CodeBracketMatch>{CodeBracketMatch{pos, *found, true}};
    } else {
        const std::size_t low_row = pos_row > max_rows ? pos_row - max_rows : 0;
        InlineArray<std::pair<std::uint32_t, char>, 32> row_brackets;
        for (std::size_t r = pos_row + 1; r-- > low_row;) {
            row_brackets.clear();
            const bool in_blk = r < block_comments.size() && block_comments[r] != 0;
            scan_unmasked_line(buffer.line(r), lang, in_blk, [&](std::uint32_t col, char cur) {
                if (r == pos_row && col >= pos_col)
                    return;
                if (cur == ch || cur == want)
                    row_brackets.add({col, cur});
            });
            for (std::size_t bi = row_brackets.size(); bi-- > 0;) {
                if (row_brackets[bi].second == ch)
                    ++depth;
                else if (row_brackets[bi].second == want && --depth == 0)
                    return Optional<CodeBracketMatch>{
                        CodeBracketMatch{buffer.line_start(r) + row_brackets[bi].first, pos, true}};
            }
        }
    }
    return Optional<CodeBracketMatch>{CodeBracketMatch{pos, pos, false}};
}

// Returns visual leading space count (1 tab = 4 spaces).
inline std::uint32_t line_indent_spaces(StringView line) noexcept {
    std::uint32_t spaces = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == ' ')
            ++spaces;
        else if (c == '\t')
            spaces = (spaces + 4) & ~3u;
        else
            break;
    }
    return spaces;
}

inline bool is_blank_line(StringView line) noexcept {
    for (std::size_t i = 0; i < line.size(); ++i)
        if (line[i] != ' ' && line[i] != '\t')
            return false;
    return true;
}

struct ActiveIndentScope {
    std::size_t start_row{};

    std::size_t end_row{};

    std::uint32_t guide_column{}; // 4-space column offset (0, 4, 8, ...); UINT32_MAX when none.
};

inline ActiveIndentScope find_active_indent_scope(const TextEdit& buffer, CodeLanguage lang,
                                                  Span<const std::uint8_t> block_comments, std::uint32_t caret_offset) {
    ActiveIndentScope none{0, 0, std::numeric_limits<std::uint32_t>::max()};
    const auto& text = buffer.current().text;
    if (text.empty())
        return none;
    const auto n = static_cast<std::uint32_t>(text.size());
    const std::uint32_t from = std::min(caret_offset, n);
    const std::size_t from_row = buffer.line_at(from);
    const std::uint32_t from_col = from - buffer.line_start(from_row);
    constexpr std::size_t max_rows = 2048;
    const std::size_t low_row = from_row > max_rows ? from_row - max_rows : 0;

    int depth = 0;
    std::uint32_t open_pos = n;
    InlineArray<std::pair<std::uint32_t, char>, 32> row_braces;
    for (std::size_t r = from_row + 1; r-- > low_row && open_pos == n;) {
        row_braces.clear();
        const bool in_blk = r < block_comments.size() && block_comments[r] != 0;
        scan_unmasked_line(buffer.line(r), lang, in_blk, [&](std::uint32_t col, char cur) {
            if (r == from_row && col >= from_col)
                return;
            if (cur == '{' || cur == '}')
                row_braces.add({col, cur});
        });
        for (std::size_t bi = row_braces.size(); bi-- > 0;) {
            if (row_braces[bi].second == '}')
                ++depth;
            else if (row_braces[bi].second == '{') {
                if (depth == 0) {
                    open_pos = buffer.line_start(r) + row_braces[bi].first;
                    break;
                }
                --depth;
            }
        }
    }
    if (open_pos == n)
        return none;
    const std::size_t start_row = buffer.line_at(open_pos);
    const auto matched = find_matching_bracket(buffer, lang, block_comments, open_pos);
    const std::size_t end_row = (matched && matched->matched) ? buffer.line_at(matched->second) : from_row;
    if (end_row <= start_row)
        return none;
    const std::uint32_t col = (line_indent_spaces(buffer.line(start_row)) / 4) * 4;
    return {start_row, end_row, col};
}

} // namespace atelier::ui::detail
