#include "editor/source_editor.hpp"
#include "application/diagnostics.hpp"
#include "assets/file.hpp"
#include "editor/project_code.hpp"
#include "foundation/inline_array.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace atelier::editor {
namespace {
auto failure(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(message)});
}

Result<String> normalized(std::string_view bytes) {
    if (bytes.starts_with("\xef\xbb\xbf"))
        bytes.remove_prefix(3);
    const StringView raw{bytes};
    if (!raw.is_valid_utf8())
        return failure("Invalid UTF-8 text.");
    if (raw.find('\r') == StringView::npos)
        return String::from_utf8_validated(raw);
    std::string result;
    result.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            result += '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n')
                ++i;
        } else {
            result += raw[i];
        }
    }
    return String::from_utf8_validated(StringView{result});
}

std::wstring bounded(std::wstring value) {
    if (value.size() > 4096)
        value.resize(4096);
    return value;
}

std::wstring bounded_wide(StringView validated_u8) {
    return platform::utf8_to_utf16_validated(validated_u8.utf8_prefix(4096).std_view());
}

std::pair<std::uint32_t, std::uint32_t> ordered_selection(const ui::detail::TextEdit& buf) noexcept {
    const std::uint32_t a = buf.current().anchor.offset;
    const std::uint32_t b = buf.current().caret.offset;
    return a <= b ? std::pair{a, b} : std::pair{b, a};
}

bool replace_selection_text(ui::detail::TextEdit& buf, StringView text) {
    const auto res = buf.replace(text, ui::detail::TextEdit::Group::isolated);
    return res && *res != ui::detail::TextEdit::Change::rejected;
}

template <class Loc, class FileT>
Loc capture_nav_location(const FileT& file) {
    const auto& buf = file.document->edit();
    const auto off = buf.current().caret.offset;
    const std::size_t row = buf.line_at(off);
    return Loc{file.path, row + 1, off - buf.line_start(row) + 1};
}

template <class Deque, class Loc>
void push_bounded_nav(Deque& stack, Loc loc) {
    if (stack.size() >= 64)
        stack.pop_front();
    stack.emplace_back(std::move(loc));
}
} // namespace

bool SourceEditor::File::dirty() const {
    return document && document->dirty();
}

Result<void> SourceEditor::load(File& file, std::string bytes) {
    ATELIER_TRY_BIND(text, normalized(bytes));
    const ui::CodeLanguage lang = detect_code_language(file.path);
    auto created = ui::CodeDocument::create(std::move(*text), lang);
    if (!created) {
        if (created.error().code == ui::ErrorCode::invalid_text)
            return failure("Open a UTF-8 text file; binary/control characters are not editable.");
        return std::unexpected(application::ui_error(created.error()));
    }
    auto document = std::make_shared<ui::CodeDocument>(std::move(*created));
    if (file.document) {
        const auto& old = file.document->edit();
        const auto offset = old.current().caret.offset;
        const std::size_t row = old.line_at(offset);
        document->locate(row + 1, offset - old.line_start(row) + 1);
    }
    document->mark_saved();
    file.bom = bytes.starts_with("\xef\xbb\xbf");
    file.crlf = bytes.find("\r\n") != bytes.npos;
    file.disk = std::move(bytes);
    file.document = std::move(document);
    file.conflict = false;
    file.storage_error.clear();
    return {};
}

SourceEditor::File* SourceEditor::find(ui::ControlKey key) const noexcept {
    const auto at = std::ranges::find_if(files_, [&](const auto& file) { return file->key == key; });
    return at == files_.end() ? nullptr : at->get();
}

SourceEditor::File* SourceEditor::active() const noexcept {
    return find(selected_);
}

void SourceEditor::relocate(const std::filesystem::path& old_path, const std::filesystem::path& new_path) {
    std::error_code ec;
    const auto old_norm = std::filesystem::weakly_canonical(old_path, ec);
    ec.clear();
    const auto new_norm = std::filesystem::weakly_canonical(new_path, ec);
    if (old_norm.empty() || new_norm.empty())
        return;
    const auto remap = [&](std::filesystem::path& target) {
        if (target.empty())
            return false;
        const auto target_norm = target.lexically_normal();
        if (platform::paths_lexically_equal(target_norm, old_norm)) {
            target = new_norm;
            return true;
        }
        if (platform::is_subpath_of(target_norm, old_norm)) {
            const auto rel = target_norm.lexically_relative(old_norm.lexically_normal());
            if (!rel.empty() && rel != L".") {
                target = (new_norm / rel).lexically_normal();
                return true;
            }
        }
        return false;
    };
    for (auto& file : files_) {
        if (remap(file->path)) {
            if (file->document)
                file->document->set_language(detect_code_language(file->path));
            ec.clear();
            const auto stamp = std::filesystem::last_write_time(file->path, ec);
            const auto size = ec ? 0 : std::filesystem::file_size(file->path, ec);
            file->stamp = ec ? std::nullopt : std::optional{stamp};
            file->disk_size = size;
            file->storage_error.clear();
        }
    }
}

Result<void> SourceEditor::open(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error) {
        const auto name = platform::path_to_utf8(path);
        if (!name)
            return failure("Invalid UTF-16 text.");
        return failure("Cannot open source file: " + std::string(name->view().std_view()));
    }
    for (const auto& file : files_) {
        if (std::filesystem::equivalent(file->path, canonical, error) && !error) {
            selected_ = file->key;
            completion_open_ = false;
            hover_info_.reset();
            return {};
        }
        error.clear();
    }
    if (files_.size() >= 16)
        return failure("Close a source document before opening more than 16 files.");
    ATELIER_TRY_BIND(bytes, assets::read_file(canonical, 1024 * 1024));
    auto file = std::make_unique<File>();
    file->path = canonical;
    file->key = next_key_++;
    ATELIER_TRY(load(*file, std::move(*bytes)));
    selected_ = file->key;
    completion_open_ = false;
    hover_info_.reset();
    files_.push_back(std::move(file));
    return {};
}

Result<void> SourceEditor::save(File& file, bool overwrite) {
    auto disk = assets::read_file(file.path, 1024 * 1024);
    if (!overwrite && (!disk || *disk != file.disk)) {
        file.conflict = true;
        return failure("The file changed on disk. Reload it or confirm overwriting the external changes.");
    }
    const StringView doc_u8 = file.document->text().view();
    std::string out_bytes;
    if (!file.crlf && !file.bom) {
        if (doc_u8.size() > 1024 * 1024)
            return failure("Source file exceeds 1 MiB of UTF-8 text.");
        ATELIER_TRY(write_file(file.path, doc_u8.std_view()));
        out_bytes.assign(doc_u8.data(), doc_u8.size());
    } else {
        out_bytes.reserve((file.bom ? 3u : 0u) + doc_u8.size() + (file.crlf ? file.document->edit().line_count() : 0u));
        if (file.bom)
            out_bytes.append("\xef\xbb\xbf", 3);
        if (file.crlf) {
            for (std::size_t i = 0; i < doc_u8.size(); ++i) {
                if (doc_u8[i] == '\n')
                    out_bytes += '\r';
                out_bytes += doc_u8[i];
            }
        } else {
            out_bytes.append(doc_u8.data(), doc_u8.size());
        }
        if (out_bytes.size() > 1024 * 1024)
            return failure("Source file exceeds 1 MiB of UTF-8 text.");
        ATELIER_TRY(write_file(file.path, out_bytes));
    }
    file.document->edit().break_group();
    file.document->mark_saved();
    file.disk = std::move(out_bytes);
    file.conflict = false;
    file.storage_error.clear();
    return {};
}

Result<void> SourceEditor::reload(File& file) {
    ATELIER_TRY_BIND(bytes, assets::read_file(file.path, 1024 * 1024));
    completion_open_ = false;
    hover_info_.reset();
    return load(file, std::move(*bytes));
}

void SourceEditor::close(ui::ControlKey key) {
    std::erase_if(files_, [&](const auto& file) { return file->key == key; });
    completion_open_ = false;
    hover_info_.reset();
    if (selected_ == key)
        selected_ = files_.empty() ? 0 : files_.back()->key;
}

void SourceEditor::clear() {
    files_.clear();
    selected_ = 0;
    completion_open_ = false;
    hover_info_.reset();
    nav_back_.clear();
    nav_forward_.clear();
    search_matches_.clear();
    ++search_generation_;
    cached_annotations_.reset();
    cached_annotations_key_ = {};
    cached_problems_table_.reset();
    intelligence_.reset();
}

bool SourceEditor::select(ui::ControlKey key) {
    for (const auto& file : files_)
        if (file->key == key) {
            selected_ = key;
            completion_open_ = false;
            hover_info_.reset();
            return true;
        }
    return false;
}

void SourceEditor::poll(bool composing) {
    const auto now = Clock::now();
    if (now < next_poll_)
        return;
    next_poll_ = now + std::chrono::milliseconds(500);
    for (auto& file : files_) {
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(file->path, error);
        const auto size = error ? 0 : std::filesystem::file_size(file->path, error);
        if (!error && file->stamp == stamp && file->disk_size == size)
            continue;
        auto disk = assets::read_file(file->path, 1024 * 1024);
        if (!disk) {
            file->storage_error = disk.error().message;
            continue;
        }
        file->storage_error.clear();
        file->stamp = error ? std::nullopt : std::optional{stamp};
        file->disk_size = size;
        if (*disk == file->disk) {
            file->conflict = false;
            continue;
        }
        if (file->dirty() || (composing && file->key == selected_)) {
            file->conflict = true;
            continue;
        }
        if (auto loaded = load(*file, std::move(*disk)); !loaded)
            file->storage_error = loaded.error().message;
    }
}

std::vector<ui::MenuItem> SourceEditor::menu_items() const {
    std::vector<ui::MenuItem> result;
    for (const auto& file : files_)
        result.push_back({file->key,
                          bounded(file->path.wstring() + (file->dirty() ? L" *" : L"")),
                          true,
                          file->key == selected_,
                          {},
                          L"",
                          false,
                          {},
                          ui::MenuBehavior::radio,
                          source_control::files});
    return result;
}

std::vector<ui::MenuItem> SourceEditor::outline_menu_items() const {
    std::vector<ui::MenuItem> result;
    const auto symbols = intelligence_.outline();
    if (symbols.empty()) {
        result.push_back({source_control::outline_item_first, L"No symbols in active file", false, false, {}});
        return result;
    }
    const auto* file = active();
    std::size_t caret_line = 1;
    if (file) {
        const auto& buf = file->document->edit();
        caret_line = buf.line_at(buf.current().caret.offset) + 1;
    }
    for (std::size_t i = 0; i < symbols.size() && i < 512; ++i) {
        const auto& sym = symbols[i];
        ui::MenuItem item;
        item.key = source_control::outline_item_first + i;
        item.text = bounded((sym.is_member ? std::wstring{L"    "} : std::wstring{}) + bounded_wide(sym.name.view()));
        item.enabled = true;
        item.selected = caret_line >= sym.line && caret_line <= sym.end_line;
        item.shortcut = L"Ln " + std::to_wstring(sym.line);
        item.icon = sym.icon;
        result.push_back(std::move(item));
    }
    return result;
}

bool SourceEditor::sync_intelligence(const Runtime* world_runtime, const Project* project, const ProjectBuild* build,
                                     bool force_immediate) {
    InlineArray<OpenSourceView, 16> views;
    views.reserve(files_.size());
    const OpenSourceView* active_view = nullptr;
    const File* act = active();
    for (const auto& f : files_) {
        views.add({f->path, f->document.get(), f->dirty()});
        if (act && f.get() == act)
            active_view = &views.back();
    }
    return intelligence_.sync(world_runtime, project, build, views.view(), active_view, force_immediate);
}

void SourceEditor::push_nav_location() {
    const auto* file = active();
    if (!file)
        return;
    const auto loc = capture_nav_location<NavLocation>(*file);
    if (!nav_back_.empty() && platform::paths_lexically_equal(nav_back_.back().path, loc.path) &&
        nav_back_.back().line == loc.line) {
        return;
    }
    push_bounded_nav(nav_back_, loc);
    nav_forward_.clear();
}

Result<bool> SourceEditor::jump_to_target(const std::filesystem::path& path, std::size_t line, std::size_t column,
                                          bool record_history) {
    if (record_history)
        push_nav_location();
    if (!path.empty())
        ATELIER_TRY(open(path));
    auto* file = active();
    if (!file)
        return false;
    file->document->locate(line, column);
    completion_open_ = false;
    return true;
}

Result<bool> SourceEditor::navigate_back() {
    if (nav_back_.empty())
        return false;
    if (const auto* cur = active())
        push_bounded_nav(nav_forward_, capture_nav_location<NavLocation>(*cur));
    const NavLocation target = std::move(nav_back_.back());
    nav_back_.pop_back();
    return jump_to_target(target.path, target.line, target.column, false);
}

Result<bool> SourceEditor::navigate_forward() {
    if (nav_forward_.empty())
        return false;
    if (const auto* cur = active())
        push_bounded_nav(nav_back_, capture_nav_location<NavLocation>(*cur));
    const NavLocation target = std::move(nav_forward_.back());
    nav_forward_.pop_back();
    return jump_to_target(target.path, target.line, target.column, false);
}

Result<bool> SourceEditor::go_to_definition(Optional<std::uint32_t> explicit_offset) {
    auto* file = active();
    if (!file)
        return false;
    sync_intelligence(nullptr, nullptr, nullptr, true);
    const std::uint32_t offset = explicit_offset ? *explicit_offset : file->document->edit().current().caret.offset;
    const auto target = intelligence_.definition(*file->document, offset);
    if (!target)
        return false;
    return jump_to_target(target->path, target->line, target->column, true);
}

ui::ControlKey SourceEditor::open_search(CodeSearchMode mode) {
    search_mode_ = mode;
    auto* file = active();
    if (file) {
        const auto& buf = file->document->edit();
        const auto [s0, s1] = ordered_selection(buf);
        if ((mode == CodeSearchMode::find || mode == CodeSearchMode::replace) && s1 > s0 && s1 - s0 <= 128) {
            const StringView sel = buf.current().text.slice(s0, s1 - s0);
            if (sel.find('\n') == StringView::npos && sel.is_valid_utf8())
                find_query_ = String::from_utf8_validated(sel);
        } else if (mode == CodeSearchMode::goto_line) {
            goto_text_ = *String::from_number(buf.line_at(buf.current().caret.offset) + 1);
        }
    }
    recompute_search(false);
    return mode == CodeSearchMode::goto_line ? source_control::goto_input : source_control::find_input;
}

void SourceEditor::close_search() noexcept {
    search_mode_ = CodeSearchMode::closed;
    search_matches_.clear();
    active_search_match_ = std::numeric_limits<std::uint32_t>::max();
    ++search_generation_;
}

void SourceEditor::recompute_search(bool jump_to_nearest) {
    search_matches_.clear();
    active_search_match_ = std::numeric_limits<std::uint32_t>::max();
    ++search_generation_;
    auto* file = active();
    if (!file || (search_mode_ != CodeSearchMode::find && search_mode_ != CodeSearchMode::replace) ||
        find_query_.empty()) {
        if (file) {
            search_file_key_ = file->key;
            search_revision_ = file->document->revision();
        }
        return;
    }
    search_file_key_ = file->key;
    search_revision_ = file->document->revision();

    const ui::CodeLanguage lang = file->document->language();
    const StringView text = file->document->text().view();
    const StringView query = find_query_.view();
    const std::size_t qlen = query.size();
    if (qlen <= text.size()) {
        std::size_t i = 0;
        while (i + qlen <= text.size()) {
            const std::size_t pos = match_case_ ? text.find(query, i) : text.find_ascii_case_insensitive(query, i);
            if (pos == StringView::npos)
                break;
            bool match = true;
            if (whole_word_) {
                if ((pos > 0 && ui::is_code_ident_char(text[pos - 1], lang)) ||
                    (pos + qlen < text.size() && ui::is_code_ident_char(text[pos + qlen], lang)))
                    match = false;
            }
            if (match) {
                search_matches_.add({static_cast<std::uint32_t>(pos), static_cast<std::uint32_t>(pos + qlen)});
                i = pos + qlen;
                if (search_matches_.size() >= 4096)
                    break;
            } else {
                const auto lead = static_cast<unsigned char>(text[pos]);
                i = pos + (lead < 0x80u ? 1u : lead < 0xe0u ? 2u : lead < 0xf0u ? 3u : 4u);
            }
        }
    }

    if (!search_matches_.empty()) {
        const auto [s0, s1] = ordered_selection(file->document->edit());
        std::uint32_t best = 0;
        for (std::uint32_t idx = 0; idx < search_matches_.size(); ++idx) {
            if (search_matches_[idx].start >= s0) {
                best = idx;
                break;
            }
        }
        active_search_match_ = best;
        if (jump_to_nearest) {
            auto& buf = file->document->edit();
            buf.select({search_matches_[best].start, false}, false);
            buf.select({search_matches_[best].end, true}, true);
        }
    }
}

void SourceEditor::set_find_query(StringView query, bool jump_to_nearest) {
    if (find_query_ == query)
        return;
    if (auto valid = String::from_utf8(query))
        find_query_ = std::move(*valid);
    else
        return;
    recompute_search(jump_to_nearest);
}

void SourceEditor::set_replace_text(StringView replacement) {
    if (auto valid = String::from_utf8(replacement))
        replace_text_ = std::move(*valid);
}

bool SourceEditor::find_step(bool forward) {
    auto* file = active();
    if (!file || find_query_.empty())
        return false;
    if (search_file_key_ != file->key || search_revision_ != file->document->revision())
        recompute_search(false);
    if (search_matches_.empty())
        return false;

    const auto [s0, s1] = ordered_selection(file->document->edit());
    std::uint32_t chosen = 0;
    if (forward) {
        chosen = 0;
        for (std::uint32_t i = 0; i < search_matches_.size(); ++i) {
            if (search_matches_[i].start > s0 || (search_matches_[i].start == s0 && search_matches_[i].end != s1)) {
                chosen = i;
                break;
            }
        }
    } else {
        chosen = static_cast<std::uint32_t>(search_matches_.size() - 1);
        for (std::size_t i = search_matches_.size(); i > 0; --i) {
            if (search_matches_[i - 1].start < s0) {
                chosen = static_cast<std::uint32_t>(i - 1);
                break;
            }
        }
    }
    active_search_match_ = chosen;
    auto& buf = file->document->edit();
    buf.select({search_matches_[chosen].start, false}, false);
    buf.select({search_matches_[chosen].end, true}, true);
    return true;
}

bool SourceEditor::replace_current() {
    auto* file = active();
    if (!file || find_query_.empty())
        return false;
    recompute_search(false);
    if (search_matches_.empty())
        return false;
    if (active_search_match_ >= search_matches_.size())
        active_search_match_ = 0;
    const auto m = search_matches_[active_search_match_];
    auto& buf = file->document->edit();
    buf.break_group();
    buf.select({m.start, false}, false);
    buf.select({m.end, true}, true);
    if (!replace_selection_text(buf, replace_text_.view()))
        return false;
    buf.break_group();
    recompute_search(true);
    return true;
}

std::size_t SourceEditor::replace_all() {
    auto* file = active();
    if (!file || find_query_.empty())
        return 0;
    recompute_search(false);
    if (search_matches_.empty())
        return 0;
    const std::size_t count = search_matches_.size();
    const StringView src = file->document->text().view();
    const std::uint32_t span_start = search_matches_.front().start;
    const std::uint32_t span_end = search_matches_.back().end;
    String built;
    built.reserve((span_end - span_start) + count * replace_text_.size());
    std::size_t prev = span_start;
    for (const auto& m : search_matches_) {
        static_cast<void>(built.append(src.slice(prev, m.start - prev)));
        built.append(replace_text_);
        prev = m.end;
    }
    auto& buf = file->document->edit();
    buf.break_group();
    buf.select({span_start, false}, false);
    buf.select({span_end, true}, true);
    if (!replace_selection_text(buf, built.view()))
        return 0;
    const std::uint32_t end_caret = span_start + static_cast<std::uint32_t>(built.size());
    buf.select({end_caret, false}, false);
    buf.break_group();
    recompute_search(false);
    return count;
}

bool SourceEditor::commit_goto_line() {
    auto* file = active();
    if (!file)
        return false;
    StringView sv = goto_text_.view().trim_start(" ");
    std::size_t ln = 0;
    std::size_t col = 1;
    const auto r1 = std::from_chars(sv.data(), sv.data() + sv.size(), ln);
    if (r1.ec != std::errc{} || ln == 0)
        return false;
    if (r1.ptr < sv.data() + sv.size() && *r1.ptr == ':') {
        std::size_t parsed_col = 1;
        if (std::from_chars(r1.ptr + 1, sv.data() + sv.size(), parsed_col).ec == std::errc{} && parsed_col > 0)
            col = parsed_col;
    }
    push_nav_location();
    file->document->locate(ln, col);
    close_search();
    return true;
}

void SourceEditor::trigger_completion(bool explicit_request) {
    auto* file = active();
    if (!file) {
        completion_open_ = false;
        completion_items_.clear();
        return;
    }
    if (explicit_request)
        sync_intelligence(nullptr, nullptr, nullptr, true);
    const std::uint32_t caret = file->document->edit().current().caret.offset;
    completion_items_ = intelligence_.completions(*file->document, caret, explicit_request);
    completion_explicit_ = explicit_request;
    completion_open_ = !completion_items_.empty();
    if (completion_selected_ >= completion_items_.size())
        completion_selected_ = 0;
    tracked_file_key_ = file->key;
    tracked_edit_revision_ = file->document->revision();
    tracked_caret_offset_ = caret;
}

void SourceEditor::dismiss_completion() noexcept {
    completion_open_ = false;
    completion_explicit_ = false;
    completion_items_.clear();
    completion_selected_ = 0;
}

void SourceEditor::step_completion(int delta) noexcept {
    if (completion_items_.empty())
        return;
    const int n = static_cast<int>(completion_items_.size());
    int cur = static_cast<int>(completion_selected_);
    cur = (cur + delta) % n;
    if (cur < 0)
        cur += n;
    completion_selected_ = static_cast<std::size_t>(cur);
}

bool SourceEditor::accept_completion(std::size_t index) {
    auto* file = active();
    if (!file || index >= completion_items_.size())
        return false;
    const CodeCompletionItem item = completion_items_[index];
    auto& buf = file->document->edit();
    const auto limit = static_cast<std::uint32_t>(buf.current().text.size());
    const std::uint32_t rs = std::min(item.replace_start, limit);
    const std::uint32_t re = std::min(std::max(item.replace_end, rs), limit);
    buf.break_group();
    buf.select({rs, false}, false);
    buf.select({re, true}, true);
    if (!replace_selection_text(buf, item.insert_text.view()))
        return false;
    if (item.cursor_backtrack > 0) {
        const std::uint32_t cur = buf.current().caret.offset;
        if (cur >= item.cursor_backtrack)
            buf.select({cur - item.cursor_backtrack, false}, false);
    }
    buf.break_group();
    tracked_file_key_ = file->key;
    tracked_edit_revision_ = file->document->revision();
    tracked_caret_offset_ = buf.current().caret.offset;
    if (item.insert_text.ends_with('.'))
        trigger_completion(false);
    else
        dismiss_completion();
    return true;
}

bool SourceEditor::accept_selected_completion() {
    return completion_open() && accept_completion(completion_selected_);
}

Optional<CodeSignatureHelp> SourceEditor::current_signature_help() const {
    const auto* file = active();
    if (!file)
        return std::nullopt;
    return intelligence_.signature_help(*file->document, file->document->edit().current().caret.offset);
}

bool SourceEditor::update_pointer(const ui::UiFrame* frame, float x, float y, bool ctrl_held) {
    last_pointer_x_ = x;
    last_pointer_y_ = y;
    auto* file = active();
    if (!file || !frame) {
        const bool had = hover_info_.has_value();
        hover_info_.reset();
        ctrl_hover_ = false;
        return had;
    }
    const auto offset = frame->code_offset_at(source_control::buffer, x, y);
    if (!offset) {
        const bool had = hover_info_.has_value();
        hover_info_.reset();
        ctrl_hover_ = false;
        return had;
    }
    auto next = intelligence_.hover(*file->document, *offset);
    const bool changed = (next.has_value() != hover_info_.has_value()) ||
                         (next && hover_info_ &&
                          (next->range_start != hover_info_->range_start || next->range_end != hover_info_->range_end ||
                           next->tooltip != hover_info_->tooltip)) ||
                         (ctrl_held != ctrl_hover_);
    if (changed) {
        hover_px_x_ = x;
        hover_px_y_ = y;
    }
    hover_info_ = std::move(next);
    ctrl_hover_ = ctrl_held;
    return changed;
}

bool SourceEditor::set_ctrl_hover(const ui::UiFrame* frame, bool ctrl_held) {
    if (ctrl_held == ctrl_hover_)
        return false;
    if (ctrl_held && !hover_info_ && last_pointer_x_ >= 0.f && last_pointer_y_ >= 0.f)
        return update_pointer(frame, last_pointer_x_, last_pointer_y_, true);
    ctrl_hover_ = ctrl_held;
    return hover_info_.has_value() && hover_info_->definition.has_value();
}

void SourceEditor::dismiss_hover() noexcept {
    hover_info_.reset();
    ctrl_hover_ = false;
}

Result<bool> SourceEditor::apply_action(const ui::Action& action, bool& focus_buffer, ui::ControlKey& focus_bar) {
    focus_buffer = false;
    focus_bar = 0;

    if (action.key == source_control::buffer && action.kind == ui::ActionKind::activate) {
        if (const auto* off = std::get_if<double>(&action.value); off && std::isfinite(*off) && *off >= 0.0) {
            ATELIER_TRY_BIND(jumped, go_to_definition(Optional<std::uint32_t>{static_cast<std::uint32_t>(*off)}));
            if (*jumped)
                focus_buffer = true;
            return true;
        }
    }

    if (action.kind == ui::ActionKind::activate && action.key >= source_control::completion_pick_first &&
        action.key < source_control::completion_pick_first + 64) {
        const std::size_t idx = static_cast<std::size_t>(action.key - source_control::completion_pick_first);
        if (accept_completion(idx))
            focus_buffer = true;
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key >= source_control::outline_item_first &&
        action.key < source_control::outline_item_first + 512) {
        const std::size_t idx = static_cast<std::size_t>(action.key - source_control::outline_item_first);
        const auto syms = intelligence_.outline();
        if (idx < syms.size()) {
            if (auto* file = active()) {
                push_nav_location();
                file->document->locate(syms[idx].line, syms[idx].column);
                focus_buffer = true;
            }
        }
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key == source_control::nav_back) {
        ATELIER_TRY_BIND(moved, navigate_back());
        if (*moved)
            focus_buffer = true;
        return true;
    }
    if (action.kind == ui::ActionKind::activate && action.key == source_control::nav_forward) {
        ATELIER_TRY_BIND(moved, navigate_forward());
        if (*moved)
            focus_buffer = true;
        return true;
    }

    if (action.kind == ui::ActionKind::activate && action.key == source_control::find_toggle) {
        if (search_mode_ == CodeSearchMode::find || search_mode_ == CodeSearchMode::replace) {
            close_search();
            focus_buffer = true;
        } else {
            focus_bar = open_search(CodeSearchMode::find);
        }
        return true;
    }
    if (action.kind == ui::ActionKind::activate && action.key == source_control::open_goto) {
        focus_bar = open_search(CodeSearchMode::goto_line);
        return true;
    }
    if (action.kind == ui::ActionKind::activate && action.key == source_control::problems_toggle) {
        problems_open_ = !problems_open_;
        return true;
    }
    if (action.kind == ui::ActionKind::activate && action.key == source_control::problem) {
        if (!intelligence_.problems().empty()) {
            ATELIER_TRY_BIND(located, locate_problem());
            if (*located)
                focus_buffer = true;
        } else {
            problems_open_ = !problems_open_;
        }
        return true;
    }

    if (action.key == source_control::find_input) {
        if (const auto* text = std::get_if<std::wstring>(&action.value)) {
            if (auto u8 = platform::utf16_to_utf8_string(*text))
                set_find_query(u8->view(), action.kind == ui::ActionKind::edit_preview);
        }
        if (action.kind == ui::ActionKind::edit_commit)
            find_step(true);
        else if (action.kind == ui::ActionKind::edit_cancel) {
            close_search();
            focus_buffer = true;
        }
        return true;
    }
    if (action.key == source_control::replace_input) {
        if (const auto* text = std::get_if<std::wstring>(&action.value)) {
            if (auto u8 = platform::utf16_to_utf8_string(*text))
                set_replace_text(u8->view());
        }
        if (action.kind == ui::ActionKind::edit_commit)
            replace_current();
        else if (action.kind == ui::ActionKind::edit_cancel) {
            close_search();
            focus_buffer = true;
        }
        return true;
    }
    if (action.key == source_control::goto_input) {
        if (const auto* text = std::get_if<std::wstring>(&action.value)) {
            if (auto u8 = platform::utf16_to_utf8_string(*text))
                goto_text_ = std::move(*u8);
        }
        if (action.kind == ui::ActionKind::edit_commit) {
            if (commit_goto_line())
                focus_buffer = true;
        } else if (action.kind == ui::ActionKind::edit_cancel) {
            close_search();
            focus_buffer = true;
        }
        return true;
    }

    if (action.kind == ui::ActionKind::activate) {
        if (action.key == source_control::find_next) {
            find_step(true);
            return true;
        }
        if (action.key == source_control::find_prev) {
            find_step(false);
            return true;
        }
        if (action.key == source_control::find_case) {
            match_case_ = !match_case_;
            recompute_search(true);
            return true;
        }
        if (action.key == source_control::find_word) {
            whole_word_ = !whole_word_;
            recompute_search(true);
            return true;
        }
        if (action.key == source_control::replace_one) {
            replace_current();
            return true;
        }
        if (action.key == source_control::replace_all) {
            replace_all();
            return true;
        }
        if (action.key == source_control::goto_commit) {
            if (commit_goto_line())
                focus_buffer = true;
            return true;
        }
        if (action.key == source_control::search_close) {
            close_search();
            focus_buffer = true;
            return true;
        }
    }

    if (action.key == source_control::problems_table || action.key == source_control::problem_jump) {
        if (const auto* sel = std::get_if<ui::TableSelection>(&action.value)) {
            if (sel->row > 0) {
                selected_problem_row_ = static_cast<std::size_t>(sel->row - 1);
                const auto probs = intelligence_.problems();
                if (selected_problem_row_ < probs.size()) {
                    const auto& p = probs[selected_problem_row_];
                    ATELIER_TRY_BIND(jumped, jump_to_target(p.path, p.line, p.column, true));
                    if (*jumped)
                        focus_buffer = true;
                }
            }
        } else if (const auto* resized = std::get_if<ui::TableColumnResize>(&action.value);
                   resized && std::isfinite(resized->width)) {
            if (resized->column >= source_control::problems_col_first &&
                resized->column < source_control::problems_col_first + problem_col_widths_.size()) {
                const std::size_t col_idx =
                    static_cast<std::size_t>(resized->column - source_control::problems_col_first);
                const float clamped = std::clamp(resized->width, 48.f, 2048.f);
                if (problem_col_widths_[col_idx] != clamped) {
                    problem_col_widths_[col_idx] = clamped;
                    ++problem_cols_generation_;
                }
            }
        } else if (action.kind == ui::ActionKind::activate) {
            const auto probs = intelligence_.problems();
            if (selected_problem_row_ < probs.size()) {
                const auto& p = probs[selected_problem_row_];
                ATELIER_TRY_BIND(jumped, jump_to_target(p.path, p.line, p.column, true));
                if (*jumped)
                    focus_buffer = true;
            }
        }
        return true;
    }

    return false;
}

Result<bool> SourceEditor::locate_problem() {
    const auto probs = intelligence_.problems();
    if (probs.empty())
        return false;
    const std::size_t idx = selected_problem_row_ < probs.size() ? selected_problem_row_ : 0;
    problems_open_ = true;
    return jump_to_target(probs[idx].path, probs[idx].line, probs[idx].column, true);
}

Result<void> SourceEditor::locate(const logging::SourceLocation& location) {
    if (location.file.empty() || location.file.find('\0') != std::string::npos)
        return failure("This record has no usable disk source path.");
    if (location.version && location.disk_path.empty())
        return failure("This Source version has no saved-file mapping; it cannot be located on disk.");
    auto name = platform::utf8_to_utf16(location.disk_path.empty() ? location.file : location.disk_path);
    if (!name)
        return failure("Invalid UTF-8 text.");
    auto path = std::filesystem::path{std::move(*name)};
    if (!path.is_absolute()) {
        auto root_name = platform::utf8_to_utf16(location.root);
        if (!root_name)
            return failure("Invalid UTF-8 text.");
        const auto root = std::filesystem::path{std::move(*root_name)};
        if (!root.is_absolute() || path.has_root_path())
            return failure("Relative log source has no original absolute root.");
        path = root / path;
    }
    ATELIER_TRY_BIND(disk, assets::read_file(path, 1024 * 1024));
    if (location.version && source::content_hash(*disk) != *location.version)
        return failure("The recorded source version is stale; no location was applied.");
    ATELIER_TRY(open(path));
    auto* file = active();
    if (!file || file->dirty())
        return failure("Save or discard the source draft before locating a recorded position.");
    if (file->disk != *disk)
        ATELIER_TRY(load(*file, *disk));
    auto& buffer = file->document->edit();
    if (!location.line && location.offset) {
        if (*location.offset > disk->size())
            return failure("The recorded source byte offset is outside the file.");
        auto offset = static_cast<std::size_t>(*location.offset);
        while (offset && offset < disk->size() && (static_cast<unsigned char>((*disk)[offset]) & 0xc0u) == 0x80u)
            --offset;
        ATELIER_TRY_BIND(prefix, normalized(std::string_view(*disk).substr(0, offset)));
        if (!ui::CodeDocument::valid_text(prefix->view()))
            return failure("Open a UTF-8 text file; binary/control characters are not editable.");
        buffer.select({static_cast<std::uint32_t>(prefix->size()), false}, false);
        return {};
    }
    const auto row = location.line ? location.line : 1;
    if (row > buffer.line_count())
        return failure("The recorded source line is outside the file.");
    file->document->locate(row, location.column ? location.column : 1);
    return {};
}

Result<void> SourceEditor::append(ui_language::ControlOutput controls, ui::ControlKey parent,
                                  ui_language::LiveDocument& live,
                                  FunctionRef<Result<ui_language::ActionBinding>(OperatorCall)> action,
                                  const Project* project, const ProjectBuild* build, bool running,
                                  const Runtime* world_runtime, const ui::UiFrame* frame) {
    using namespace ui_language;
    auto* file = active();
    sync_intelligence(world_runtime, project, build, false);

    const bool buffer_focused =
        frame && frame->input_anchor && frame->input_anchor->token.key == source_control::buffer;
    const bool composing = buffer_focused && frame->input_anchor->composition_id != 0;

    if (file) {
        const std::uint64_t rev = file->document->revision();
        const std::uint32_t caret = file->document->edit().current().caret.offset;
        if (search_mode_ == CodeSearchMode::find || search_mode_ == CodeSearchMode::replace) {
            if (search_file_key_ != file->key || search_revision_ != rev)
                recompute_search(false);
        }
        if (tracked_file_key_ != file->key) {
            tracked_file_key_ = file->key;
            tracked_edit_revision_ = rev;
            tracked_caret_offset_ = caret;
            dismiss_completion();
        } else if (rev != tracked_edit_revision_) {
            const bool grew_or_edited = buffer_focused && !composing;
            tracked_edit_revision_ = rev;
            tracked_caret_offset_ = caret;
            if (grew_or_edited) {
                completion_items_ = intelligence_.completions(*file->document, caret, false);
                completion_open_ = !completion_items_.empty();
                completion_explicit_ = false;
                if (completion_selected_ >= completion_items_.size())
                    completion_selected_ = 0;
            } else {
                dismiss_completion();
            }
        } else if (caret != tracked_caret_offset_) {
            tracked_caret_offset_ = caret;
            if (completion_open_) {
                const auto& first = completion_items_.front();
                const auto& edit = file->document->edit();
                const std::size_t c_row = edit.line_at(caret);
                const std::uint32_t c_ls = edit.line_start(c_row);
                const StringView c_ln = edit.line(c_row);
                const ui::CodeLanguage lang = file->document->language();
                std::uint32_t word_end = first.replace_start;
                if (first.replace_start >= c_ls && first.replace_start - c_ls <= c_ln.size()) {
                    std::uint32_t wc = first.replace_start - c_ls;
                    while (wc < c_ln.size() && ui::is_code_ident_char(c_ln[wc], lang))
                        ++wc;
                    word_end = c_ls + wc;
                }
                if (caret < first.replace_start || caret > word_end)
                    dismiss_completion();
                else {
                    completion_items_ = intelligence_.completions(*file->document, caret, completion_explicit_);
                    if (completion_items_.empty())
                        dismiss_completion();
                    else if (completion_selected_ >= completion_items_.size())
                        completion_selected_ = 0;
                }
            }
        }
    } else {
        dismiss_completion();
    }

    if (!buffer_focused && completion_open_)
        dismiss_completion();

    bool conflict = false;
    std::wstring breadcrumb_path = L"Workspace";
    std::wstring breadcrumb_symbol = L"Outline";
    std::wstring cursor_status = L"Ln 1, Col 1";
    std::wstring meta_status = L"UTF-8";
    std::wstring build_status = L"Open a source file from Asset Browser or File > Open Source";

    std::shared_ptr<const ui::CodeAnnotations> annotations;
    if (file) {
        auto& buffer = file->document->edit();
        const auto offset = buffer.current().caret.offset;
        const std::size_t row = buffer.line_at(offset);
        const std::size_t col = offset - buffer.line_start(row) + 1;
        const auto [sel0, sel1] = ordered_selection(buffer);

        cursor_status = L"Ln " + std::to_wstring(row + 1) + L", Col " + std::to_wstring(col);
        if (sel1 > sel0)
            cursor_status += L" (" + std::to_wstring(sel1 - sel0) + L" sel)";

        meta_status = std::wstring(L"Spaces: 4  ·  ") + (file->crlf ? L"CRLF" : L"LF") + L"  ·  UTF-8  ·  " +
                      bounded_wide(code_language_name(file->document->language()));

        const std::filesystem::path proj_root = project ? project->root() : std::filesystem::path{};
        const std::wstring proj_name = project ? project->name() : std::wstring{};
        if (cached_breadcrumb_file_key_ != file->key || cached_breadcrumb_file_path_ != file->path ||
            cached_breadcrumb_project_root_ != proj_root || cached_breadcrumb_project_name_ != proj_name) {
            if (project && platform::is_subpath_of(file->path, proj_root)) {
                const auto relative = file->path.lexically_normal().lexically_relative(proj_root.lexically_normal());
                cached_breadcrumb_path_ = proj_name + L"  ›  " + relative.generic_wstring();
            } else {
                cached_breadcrumb_path_ = file->path.filename().wstring();
            }
            cached_breadcrumb_file_key_ = file->key;
            cached_breadcrumb_file_path_ = file->path;
            cached_breadcrumb_project_root_ = proj_root;
            cached_breadcrumb_project_name_ = proj_name;
        }
        breadcrumb_path = cached_breadcrumb_path_;

        const String sym = intelligence_.breadcrumb_for_line(row);
        breadcrumb_symbol = sym.empty() ? L"Symbols (" + std::to_wstring(intelligence_.outline().size()) + L")"
                                        : bounded_wide(sym.view());

        conflict = file->conflict;
        build_status = file->dirty() ? L"Modified" : L"Saved";
        if (intelligence_.check_pending())
            build_status += L" · Checking...";

        const bool show_search = search_mode_ == CodeSearchMode::find || search_mode_ == CodeSearchMode::replace;
        Optional<ui::CodeSearchMatch> link_rng;
        if (ctrl_hover_ && hover_info_ && hover_info_->definition)
            link_rng.emplace(ui::CodeSearchMatch{hover_info_->range_start, hover_info_->range_end});

        const AnnotationsCacheKey ann_key{file->key,
                                          file->document->revision(),
                                          intelligence_.generation(),
                                          show_search ? search_generation_ : 0,
                                          show_search ? active_search_match_ : 0,
                                          show_search,
                                          link_rng};
        if (!cached_annotations_ || !(cached_annotations_key_ == ann_key)) {
            auto ann = std::make_shared<ui::CodeAnnotations>();
            ann->revision = file->document->revision();
            ann->diagnostics.append(intelligence_.active_diagnostics());
            if (show_search) {
                ann->search_matches = search_matches_;
                ann->active_search_match = active_search_match_;
            }
            ann->semantic_types.append(intelligence_.semantic_types());
            ann->semantic_functions.append(intelligence_.semantic_functions());
            ann->semantic_fields.append(intelligence_.semantic_fields());
            ann->link_range = link_rng;
            cached_annotations_ = std::move(ann);
            cached_annotations_key_ = ann_key;
        }
        annotations = cached_annotations_;
    }

    if (project && build) {
        if (!build_status.empty())
            build_status += L"  ·  ";
        build_status += running ? L"Playing" : L"Project";
        build_status += build->state == BuildState::ready              ? L" (Ready)"
                        : build->state == BuildState::rebuild_required ? L" (Rebuild Required)"
                                                                       : L" (Build Failed)";
    }

    const auto probs = intelligence_.problems();
    const std::wstring diag_summary =
        probs.empty() ? L"0 Problems"
                      : (std::to_wstring(probs.size()) + (probs.size() == 1 ? L" Problem" : L" Problems"));
    const std::wstring diag_badge_style =
        probs.empty() ? L"code-status-item code-status-ok" : L"code-status-item code-status-err";

    if (!cached_problems_table_ || cached_problems_intel_gen_ != intelligence_.generation() ||
        cached_problems_cols_gen_ != problem_cols_generation_) {
        auto problems_data = std::make_shared<ui::TableData>();
        problems_data->columns = {{source_control::problems_col_first + 0, L"Severity", problem_col_widths_[0], 50.f,
                                   ui::TextAlign::left, false},
                                  {source_control::problems_col_first + 1, L"Description", problem_col_widths_[1],
                                   120.f, ui::TextAlign::left, false},
                                  {source_control::problems_col_first + 2, L"Location", problem_col_widths_[2], 80.f,
                                   ui::TextAlign::left, false}};
        problems_data->rows.reserve(probs.size());
        for (std::size_t i = 0; i < probs.size(); ++i) {
            const auto& p = probs[i];
            const std::wstring sev_txt = p.severity == ui::CodeDiagnosticSeverity::error     ? L"Error"
                                         : p.severity == ui::CodeDiagnosticSeverity::warning ? L"Warn"
                                                                                             : L"Info";
            const std::wstring loc_txt = bounded(bounded_wide(p.file_label.view()) + L":" + std::to_wstring(p.line) +
                                                 L":" + std::to_wstring(p.column));
            ui::TableRow tr;
            tr.key = static_cast<ui::TableKey>(i + 1);
            tr.cells = {{tr.key * 10 + 1, sev_txt, ui::ControlIcon::warning},
                        {tr.key * 10 + 2, bounded_wide(p.message.view())},
                        {tr.key * 10 + 3, loc_txt, ui::ControlIcon::source_file}};
            problems_data->rows.push_back(std::move(tr));
        }
        cached_problems_table_ = std::move(problems_data);
        cached_problems_intel_gen_ = intelligence_.generation();
        cached_problems_cols_gen_ = problem_cols_generation_;
    }
    const ui::TableSelection prob_sel{
        probs.empty() ? 0 : static_cast<ui::TableKey>(std::min(selected_problem_row_, probs.size() - 1) + 1), 0};

    double popup_x = 48.0;
    double popup_y = 24.0;
    double sig_x = 48.0;
    double sig_y = 4.0;
    double hover_x = 48.0;
    double hover_y = 28.0;
    if (frame) {
        const float dpi = std::max(frame->drawing ? frame->drawing->dpi_scale : 1.f, 1.f);
        const auto* buf_box = frame->find(source_control::buffer);
        if (buf_box) {
            const float box_w_dip = buf_box->bounds.width / dpi;
            const float box_h_dip = buf_box->bounds.height / dpi;
            if (frame->input_anchor && frame->input_anchor->token.key == source_control::buffer) {
                const float caret_rel_x = (frame->input_anchor->bounds.x - buf_box->bounds.x) / dpi;
                const float caret_rel_y = (frame->input_anchor->bounds.y - buf_box->bounds.y) / dpi;
                const float line_h_dip = std::max(16.f, frame->input_anchor->bounds.height / dpi);
                popup_x = std::clamp(caret_rel_x, 8.f, std::max(8.f, box_w_dip - 348.f));
                const float below_y = caret_rel_y + line_h_dip + 3.f;
                const float est_comp_h = static_cast<float>(completion_items_.size()) * 22.f + 34.f;
                popup_y = (below_y + est_comp_h > box_h_dip && caret_rel_y > est_comp_h + 6.f)
                              ? (caret_rel_y - est_comp_h - 3.f)
                              : below_y;
                sig_x = std::clamp(caret_rel_x, 8.f, std::max(8.f, box_w_dip - 320.f));
                sig_y = caret_rel_y > 30.f ? (caret_rel_y - 26.f) : (caret_rel_y + line_h_dip + 3.f);
            }
            if (hover_info_) {
                const float hx = (hover_px_x_ - buf_box->bounds.x) / dpi;
                const float hy = (hover_px_y_ - buf_box->bounds.y) / dpi;
                hover_x = std::clamp(hx, 8.f, std::max(8.f, box_w_dip - 360.f));
                hover_y = (hy + 86.f > box_h_dip && hy > 86.f) ? (hy - 78.f) : (hy + 18.f);
            }
        }
    }

    Optional<CodeSignatureHelp> sig_help;
    if (file && buffer_focused && !composing) {
        const SignatureCacheKey sig_key{file->key, file->document->revision(), intelligence_.generation(),
                                        file->document->edit().current().caret.offset};
        if (!(cached_sig_key_ == sig_key)) {
            cached_sig_help_ = intelligence_.signature_help(*file->document, sig_key.caret_offset);
            cached_sig_key_ = sig_key;
        }
        sig_help = cached_sig_help_;
    }
    const bool show_comp = completion_open() && buffer_focused && !composing;
    const bool show_sig = sig_help.has_value() && !(show_comp && std::abs(sig_y - popup_y) < 22.0);
    const bool show_hover = hover_info_.has_value() && !show_comp;

    static const auto empty_items = std::make_shared<std::vector<Item>>();
    std::shared_ptr<std::vector<Item>> comp_rows = empty_items;
    std::wstring comp_doc;
    if (show_comp) {
        auto built_comp = std::make_shared<std::vector<Item>>();
        built_comp->reserve(completion_items_.size());
        for (std::size_t i = 0; i < completion_items_.size(); ++i) {
            const auto& ci = completion_items_[i];
            const bool is_sel = i == completion_selected_;
            if (is_sel)
                comp_doc = bounded_wide(ci.documentation.empty() ? ci.detail.view() : ci.documentation.view());
            built_comp->push_back({source_control::completion_row_first + i,
                                   {bounded_wide(ci.badge.view()), bounded_wide(ci.badge_class.view()),
                                    bounded_wide(ci.label.view()), bounded_wide(ci.detail.view()), is_sel,
                                    ActionBinding{source_control::completion_pick_first + i, true},
                                    std::wstring(is_sel ? L"code-completion-row-active" : L"code-completion-row")}});
        }
        comp_rows = std::move(built_comp);
    }

    std::wstring match_readout = L"No results";
    if (!find_query_.empty()) {
        if (!search_matches_.empty()) {
            const std::uint32_t disp_idx = active_search_match_ < search_matches_.size() ? active_search_match_ + 1 : 1;
            match_readout = std::to_wstring(disp_idx) + L" of " + std::to_wstring(search_matches_.size());
        }
    } else {
        match_readout = L"0 results";
    }

    ATELIER_TRY_BIND(open_action, action({"source.open"}));
    ATELIER_TRY_BIND(save_action,
                     action({"file.save", json::Document{}, {.selection = {}, .region = RegionKind::code}}));
    ATELIER_TRY_BIND(reload_action, action({"source.reload"}));
    ATELIER_TRY_BIND(new_component_action, action({"source.new_component"}));
    ATELIER_TRY_BIND(build_action, action({"project.build"}));
    ATELIER_TRY_BIND(start_play_action, action({"play.start"}));
    ATELIER_TRY_BIND(stop_play_action, action({"play.stop"}));

    std::uint64_t tabs_digest = 14695981039346656037ull;
    const auto mix_tab = [&](std::uint64_t v) noexcept {
        tabs_digest ^= v;
        tabs_digest *= 1099511628211ull;
    };
    mix_tab(files_.size());
    mix_tab(file ? file->key : 0);
    for (const auto& doc : files_) {
        mix_tab(doc->key);
        mix_tab(doc->dirty() ? 1u : 0u);
        for (const wchar_t ch : doc->path.filename().native())
            mix_tab(static_cast<std::uint64_t>(ch));
    }
    if (!cached_open_tabs_ || cached_tabs_digest_ != tabs_digest) {
        auto built_tabs = std::make_shared<std::vector<Item>>();
        built_tabs->reserve(files_.size());
        for (const auto& doc : files_) {
            const bool is_sel = file && doc->key == file->key;
            const std::wstring tab_label = bounded(doc->path.filename().wstring() + (doc->dirty() ? L" *" : L""));
            built_tabs->push_back({doc->key + (source_control::close_document_offset * 2),
                                   {tab_label, is_sel, ActionBinding{doc->key, true},
                                    ActionBinding{doc->key + source_control::close_document_offset, true}}});
        }
        cached_open_tabs_ = std::move(built_tabs);
        cached_tabs_digest_ = tabs_digest;
    }
    auto open_tabs = cached_open_tabs_;

    const bool find_or_replace = search_mode_ == CodeSearchMode::find || search_mode_ == CodeSearchMode::replace;
    const bool replace_mode = search_mode_ == CodeSearchMode::replace;
    const bool goto_mode = search_mode_ == CodeSearchMode::goto_line;

    const std::array<InputValue, 65> inputs{
        CodeBinding{file ? file->document : empty_, file != nullptr, false, std::move(annotations)},
        conflict,
        source_control::files,
        source_control::outline,
        std::move(*open_action),
        std::move(*save_action),
        std::move(*reload_action),
        std::move(*new_component_action),
        std::move(*build_action),
        std::move(*start_play_action),
        std::move(*stop_play_action),
        running,
        !files_.empty(),
        ItemList{std::move(open_tabs)},
        bounded(breadcrumb_path),
        bounded(breadcrumb_symbol),
        ActionBinding{source_control::nav_back, can_navigate_back()},
        ActionBinding{source_control::nav_forward, can_navigate_forward()},
        find_or_replace,
        ActionBinding{source_control::find_toggle, file != nullptr},
        problems_open_,
        ActionBinding{source_control::problems_toggle, true},
        search_mode_ != CodeSearchMode::closed,
        find_or_replace,
        replace_mode,
        goto_mode,
        TextEditBinding{source_control::find_input,
                        bounded_wide(find_query_.view()),
                        true,
                        !find_query_.empty() && search_matches_.empty(),
                        true,
                        0,
                        {},
                        false,
                        L"Find..."},
        TextEditBinding{source_control::replace_input,
                        bounded_wide(replace_text_.view()),
                        true,
                        false,
                        true,
                        0,
                        {},
                        false,
                        L"Replace..."},
        TextEditBinding{source_control::goto_input,
                        bounded_wide(goto_text_.view()),
                        true,
                        false,
                        true,
                        0,
                        {},
                        false,
                        L"Line[:Column]..."},
        bounded(match_readout),
        match_case_,
        ActionBinding{source_control::find_case, true},
        whole_word_,
        ActionBinding{source_control::find_word, true},
        ActionBinding{source_control::find_prev, !search_matches_.empty()},
        ActionBinding{source_control::find_next, !search_matches_.empty()},
        ActionBinding{source_control::replace_one, !search_matches_.empty()},
        ActionBinding{source_control::replace_all, !search_matches_.empty()},
        ActionBinding{source_control::goto_commit, true},
        ActionBinding{source_control::search_close, true},
        show_comp,
        popup_x,
        popup_y,
        ItemList{std::move(comp_rows)},
        !comp_doc.empty(),
        bounded(comp_doc),
        show_sig,
        sig_x,
        sig_y,
        sig_help ? bounded_wide(sig_help->prefix.view()) : std::wstring{},
        sig_help ? bounded_wide(sig_help->active_parameter.view()) : std::wstring{},
        sig_help ? bounded_wide(sig_help->suffix.view()) : std::wstring{},
        show_hover,
        hover_x,
        hover_y,
        bounded(hover_info_
                    ? hover_info_->tooltip.title +
                          (hover_info_->tooltip.identity.empty() ? L"" : L"  ·  " + hover_info_->tooltip.identity)
                    : std::wstring{}),
        bounded(hover_info_ ? hover_info_->tooltip.description : std::wstring{}),
        TableBinding{cached_problems_table_, prob_sel, true},
        ActionBinding{source_control::problem_jump, !probs.empty()},
        bounded(diag_summary),
        diag_badge_style,
        bounded(build_status),
        bounded(cursor_status),
        ActionBinding{source_control::open_goto, file != nullptr},
        bounded(meta_status)};

    const std::array<KeyBinding, 5> keys{{{"buffer", source_control::buffer},
                                          {"all_files", source_control::files},
                                          {"outline_combo", source_control::outline},
                                          {"problems_table", source_control::problems_table},
                                          {"problem", source_control::problem}}};
    ATELIER_TRY(live.append("CodePanel", parent, keys, inputs, controls));
    return {};
}
} // namespace atelier::editor
