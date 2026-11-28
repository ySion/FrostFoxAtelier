#pragma once
#include "foundation/array.hpp"
#include "foundation/hash.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/optional.hpp"
#include "foundation/result.hpp"
#include "foundation/string.hpp"
#include "ui/detail/text_edit.hpp"
#include "ui/error.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace atelier::ui {

enum class CodeLanguage : std::uint8_t { plain, source, ui_source, style_sheet, json };

[[nodiscard]] constexpr bool is_code_ident_char(char ch, CodeLanguage language = CodeLanguage::plain) noexcept {
    return is_ascii_ident_continue(ch) || (language == CodeLanguage::style_sheet && ch == '-');
}

enum class CodeDiagnosticSeverity : std::uint8_t { error, warning, info };

struct CodeDiagnostic {
    std::uint32_t line{}; // Zero-based line index.

    std::uint32_t col_start{}; // Zero-based UTF-8 byte offset within line.

    std::uint32_t col_end{}; // Exclusive UTF-8 byte offset within line; <= col_start highlights token/line.

    CodeDiagnosticSeverity severity{CodeDiagnosticSeverity::error};

    String message;

    bool operator==(const CodeDiagnostic&) const noexcept = default;
};

struct CodeSearchMatch {
    std::uint32_t start{}; // Document UTF-8 byte start offset.

    std::uint32_t end{}; // Document UTF-8 byte end offset.

    constexpr bool operator==(const CodeSearchMatch&) const noexcept = default;
};

struct CodeAnnotations {
    std::uint64_t revision{};

    Array<CodeDiagnostic> diagnostics;

    Array<CodeSearchMatch> search_matches;

    std::uint32_t active_search_match{std::numeric_limits<std::uint32_t>::max()};

    // Sorted UTF-8 identifier tables for semantic coloring of project & Runtime symbols.
    Array<String> semantic_types;

    Array<String> semantic_functions;

    Array<String> semantic_fields;

    // Optional Ctrl+Hover navigable symbol range [start, end) in UTF-8 byte offsets.
    Optional<CodeSearchMatch> link_range;

    bool operator==(const CodeAnnotations& other) const noexcept {
        return revision == other.revision && active_search_match == other.active_search_match &&
               link_range == other.link_range && std::ranges::equal(diagnostics, other.diagnostics) &&
               std::ranges::equal(search_matches, other.search_matches) &&
               std::ranges::equal(semantic_types, other.semantic_types) &&
               std::ranges::equal(semantic_functions, other.semantic_functions) &&
               std::ranges::equal(semantic_fields, other.semantic_fields);
    }
};

enum class CodeLineDiff : std::uint8_t { clean, modified, added };

// The host owns document lifetime. A CodeEditor borrows it through a shared
// descriptor; hiding, docking or replacing UI scopes never discards its edits.
class CodeDocument {
public:
    CodeDocument() {
        buffer_.select({}, false);
        mark_saved();
    }

    // Admission and ICU segmentation complete before publishing the document.
    static Result<CodeDocument> create(String text, CodeLanguage language = CodeLanguage::plain) {
        auto buffer = detail::TextEdit::create(std::move(text), 1024 * 1024, false, false);
        if (!buffer) {
            if (buffer.error().code == ErrorCode::invalid_text)
                return std::unexpected(Error{ErrorCode::invalid_text, 0, 0,
                                             "CodeDocument requires bounded UTF-8 text with LF line endings."});
            return std::unexpected(buffer.error());
        }
        return CodeDocument(std::move(*buffer), language);
    }

    // The same character contract governs creation and edits. Hosts may use it
    // without imposing the document-size budget on non-creating consumers.
    static bool valid_text(StringView text) noexcept { return detail::valid_document_text(text); }

    const String& text() const noexcept { return buffer_.current().text; }

    std::uint64_t revision() const noexcept { return buffer_.revision(); }

    CodeLanguage language() const noexcept { return language_; }

    void set_language(CodeLanguage language) noexcept { language_ = language; }

    detail::TextEdit& edit() noexcept { return buffer_; }

    const detail::TextEdit& edit() const noexcept { return buffer_; }

    void mark_saved() {
        saved_revision_ = buffer_.revision();
        const StringView full = buffer_.current().text.view();
        saved_text_size_ = full.size();
        saved_text_hash_ = hash_text_dual(full);
        const std::size_t n = buffer_.line_count();
        saved_line_hashes_.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            saved_line_hashes_[i] = hash_line(buffer_.line(i));
        diff_revision_ = saved_revision_;
        cached_dirty_ = false;
        cached_line_diffs_.clear();
    }

    bool dirty() const {
        ensure_diff_cache();
        return cached_dirty_;
    }

    CodeLineDiff line_diff(std::size_t row) const {
        ensure_diff_cache();
        if (!cached_dirty_ || row >= cached_line_diffs_.size())
            return CodeLineDiff::clean;
        return cached_line_diffs_[row];
    }

    void locate(std::size_t line, std::size_t column) {
        const auto row = std::min(line ? line - 1 : 0, buffer_.line_count() - 1);
        const auto raw_col = std::min(column ? column - 1 : 0, buffer_.line(row).size());
        const auto offset = buffer_.line_start(row) + static_cast<std::uint32_t>(raw_col);
        buffer_.select(buffer_.snap(offset, false), false);
    }

private:
    static std::uint64_t hash_line(StringView line) noexcept { return Hash<StringView>{}(line); }

    static std::pair<std::uint64_t, std::uint64_t> hash_text_dual(StringView text) noexcept {
        const std::uint64_t h = Hash<StringView>{}(text);
        return {h, hash_fields(std::uint64_t{0x9e3779b97f4a7c15ULL}, text.size(), h)};
    }

    void ensure_diff_cache() const {
        const std::uint64_t rev = buffer_.revision();
        if (diff_revision_ == rev)
            return;
        diff_revision_ = rev;
        if (rev == saved_revision_) {
            cached_dirty_ = false;
            cached_line_diffs_.clear();
            return;
        }
        const std::size_t m = buffer_.line_count();
        const std::size_t n = saved_line_hashes_.size();
        InlineArray<std::uint64_t, 128> cur_hashes;
        cur_hashes.resize(m);
        for (std::size_t i = 0; i < m; ++i)
            cur_hashes[i] = hash_line(buffer_.line(i));

        const StringView full = buffer_.current().text.view();
        if (m == n && full.size() == saved_text_size_ &&
            std::equal(cur_hashes.begin(), cur_hashes.end(), saved_line_hashes_.begin()) &&
            hash_text_dual(full) == saved_text_hash_) {
            cached_dirty_ = false;
            cached_line_diffs_.clear();
            return;
        }
        cached_dirty_ = true;
        cached_line_diffs_.clear();
        cached_line_diffs_.resize(m, CodeLineDiff::clean);

        std::size_t pre = 0;
        while (pre < m && pre < n && cur_hashes[pre] == saved_line_hashes_[pre])
            ++pre;
        std::size_t suf = 0;
        while (suf < (m - pre) && suf < (n - pre) && cur_hashes[m - 1 - suf] == saved_line_hashes_[n - 1 - suf])
            ++suf;

        const std::size_t mid_m = m - pre - suf;
        const std::size_t mid_n = n - pre - suf;
        if (mid_m == 0)
            return;
        if (mid_n == 0) {
            for (std::size_t i = 0; i < mid_m; ++i)
                cached_line_diffs_[pre + i] = CodeLineDiff::added;
            return;
        }

        const auto mark_gap = [&](std::size_t c0, std::size_t c1, std::size_t s0, std::size_t s1) {
            const std::size_t dm = c1 - c0;
            const std::size_t dn = s1 - s0;
            const std::size_t mod_cnt = std::min(dm, dn);
            for (std::size_t k = 0; k < mod_cnt; ++k)
                cached_line_diffs_[pre + c0 + k] = CodeLineDiff::modified;
            for (std::size_t k = mod_cnt; k < dm; ++k)
                cached_line_diffs_[pre + c0 + k] = CodeLineDiff::added;
        };

        if (mid_m <= 256 && mid_n <= 256) {
            const std::size_t stride = mid_n + 1;
            Array<std::uint16_t> dp;
            dp.resize((mid_m + 1) * stride, 0);
            for (std::size_t i = mid_m; i-- > 0;) {
                for (std::size_t j = mid_n; j-- > 0;) {
                    if (cur_hashes[pre + i] == saved_line_hashes_[pre + j])
                        dp[i * stride + j] = static_cast<std::uint16_t>(dp[(i + 1) * stride + (j + 1)] + 1);
                    else
                        dp[i * stride + j] = std::max(dp[(i + 1) * stride + j], dp[i * stride + (j + 1)]);
                }
            }
            std::size_t ci = 0;
            std::size_t sj = 0;
            std::size_t gap_c = 0;
            std::size_t gap_s = 0;
            while (ci < mid_m && sj < mid_n) {
                if (cur_hashes[pre + ci] == saved_line_hashes_[pre + sj]) {
                    mark_gap(gap_c, ci, gap_s, sj);
                    ++ci;
                    ++sj;
                    gap_c = ci;
                    gap_s = sj;
                } else if (dp[(ci + 1) * stride + sj] >= dp[ci * stride + (sj + 1)]) {
                    ++ci;
                } else {
                    ++sj;
                }
            }
            mark_gap(gap_c, mid_m, gap_s, mid_n);
        } else {
            std::size_t ci = 0;
            std::size_t sj = 0;
            std::size_t gap_c = 0;
            std::size_t gap_s = 0;
            while (ci < mid_m && sj < mid_n) {
                if (cur_hashes[pre + ci] == saved_line_hashes_[pre + sj]) {
                    mark_gap(gap_c, ci, gap_s, sj);
                    ++ci;
                    ++sj;
                    gap_c = ci;
                    gap_s = sj;
                    continue;
                }
                std::size_t match_dc = 0;
                std::size_t match_ds = 0;
                bool found = false;
                constexpr std::size_t win = 24;
                for (std::size_t dist = 1; dist <= win && !found; ++dist) {
                    for (std::size_t dc = 0; dc <= dist; ++dc) {
                        const std::size_t ds = dist - dc;
                        if (ci + dc < mid_m && sj + ds < mid_n &&
                            cur_hashes[pre + ci + dc] == saved_line_hashes_[pre + sj + ds]) {
                            match_dc = dc;
                            match_ds = ds;
                            found = true;
                            break;
                        }
                    }
                }
                if (found) {
                    ci += match_dc;
                    sj += match_ds;
                } else {
                    ++ci;
                    ++sj;
                }
            }
            mark_gap(gap_c, mid_m, gap_s, mid_n);
        }
    }

    explicit CodeDocument(detail::TextEdit buffer, CodeLanguage language)
        : buffer_(std::move(buffer)), language_(language) {
        buffer_.select({}, false);
        mark_saved();
    }

    detail::TextEdit buffer_{1024 * 1024, false};

    CodeLanguage language_{CodeLanguage::plain};

    std::uint64_t saved_revision_{};

    std::size_t saved_text_size_{};

    std::pair<std::uint64_t, std::uint64_t> saved_text_hash_{};

    Array<std::uint64_t> saved_line_hashes_;

    mutable std::uint64_t diff_revision_{std::numeric_limits<std::uint64_t>::max()};

    mutable bool cached_dirty_{};

    mutable Array<CodeLineDiff> cached_line_diffs_;
};
} // namespace atelier::ui
