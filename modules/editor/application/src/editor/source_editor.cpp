#include "editor/source_editor.hpp"
#include "editor/project_code.hpp"
#include "assets/file.hpp"
#include "application/diagnostics.hpp"
#include "platform/utf.hpp"
#include <algorithm>
#include <charconv>

namespace atelier::editor {
namespace {
auto failure(Utf8Text message) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(message).into_string()});
}

Result<std::wstring> normalized(StringView bytes) {
    if (bytes.starts_with("\xef\xbb\xbf"))
        bytes = bytes.slice(3);
    auto value = platform::utf8_to_utf16(bytes);
    if (!value)
        return failure("Invalid UTF-8 text.");
    std::wstring result;
    result.reserve(value->size());
    for (std::size_t i = 0; i < value->size(); ++i) {
        if ((*value)[i] == L'\r') {
            result += L'\n';
            if (i + 1 < value->size() && (*value)[i + 1] == L'\n')
                ++i;
        } else
            result += (*value)[i];
    }
    return result;
}

std::wstring bounded(std::wstring value) {
    if (value.size() > 4096)
        value.resize(4096);
    return value;
}
}

bool SourceEditor::File::dirty() const {
    if (checked_revision != document->revision()) {
        modified = document->text() != saved_text;
        checked_revision = document->revision();
    }
    return modified;
}

Result<void> SourceEditor::load(File& file, Array<std::byte> raw) {
    const auto bytes = as_chars(raw);
    ATELIER_TRY_BIND(text, normalized(bytes));
    auto created = ui::CodeDocument::create(*text);
    if (!created) {
        if (created.error().code == ui::ErrorCode::invalid_text)
            return failure("Open a UTF-8 text file; binary/control characters are not editable.");
        return std::unexpected(application::ui_error(created.error()));
    }
    auto document = std::make_shared<ui::CodeDocument>(std::move(*created));
    if (file.document) {
        const auto& old = file.document->edit();
        const auto offset = old.current().caret.offset;
        document->locate(old.line_at(offset) + 1, offset - old.line_start(old.line_at(offset)) + 1);
    }
    file.bom = bytes.starts_with("\xef\xbb\xbf");
    file.crlf = bytes.contains("\r\n");
    file.disk = std::move(raw);
    file.saved_text = std::move(*text);
    file.document = std::move(document);
    file.checked_revision = 0;
    file.conflict = false;
    file.storage_error.clear();
    return {};
}

SourceEditor::File* SourceEditor::active() const noexcept {
    const auto at = std::ranges::find_if(files_, [&](const auto& file) { return file->key == selected_; });
    return at == files_.end() ? nullptr : at->get();
}

Result<void> SourceEditor::open(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error) {
        const auto name = platform::utf16_to_utf8(path.native());
        if (!name)
            return failure("Invalid UTF-16 text.");
        return failure(String::concat("Cannot open source file: ", *name));
    }
    for (const auto& file : files_) {
        if (std::filesystem::equivalent(file->path, canonical, error) && !error) {
            selected_ = file->key;
            problem_line_ = 0;
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
    problem_line_ = 0;
    files_.add(std::move(file));
    return {};
}

Result<void> SourceEditor::save(File& file, bool overwrite) {
    auto disk = assets::read_file(file.path, 1024 * 1024);
    if (!overwrite && (!disk || *disk != file.disk)) {
        file.conflict = true;
        return failure("The file changed on disk. Reload it or confirm overwriting the external changes.");
    }
    auto text = platform::utf16_to_utf8(file.document->text());
    if (!text)
        return failure("Invalid UTF-16 text.");
    if (file.crlf) {
        String converted;
        converted.reserve(text->size() + file.document->edit().line_count());
        const auto sv = text->view();
        std::size_t plain = 0;
        for (std::size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == '\n') {
                if (i > plain)
                    converted.append_validated(sv.slice(plain, i - plain));
                converted.append_validated("\r\n");
                plain = i + 1;
            }
        }
        if (sv.size() > plain)
            converted.append_validated(sv.slice(plain));
        *text = std::move(converted);
    }
    if (file.bom)
        *text = String::concat("\xef\xbb\xbf", *text);
    if (text->size() > 1024 * 1024)
        return failure("Source file exceeds 1 MiB of UTF-8 text.");
    ATELIER_TRY(write_file(file.path, *text));
    file.document->edit().break_group();
    file.disk.assign(as_bytes(text->view()));
    file.saved_text = file.document->text();
    file.checked_revision = 0;
    file.conflict = false;
    file.storage_error.clear();
    return {};
}

Result<void> SourceEditor::reload(File& file) {
    ATELIER_TRY_BIND(bytes, assets::read_file(file.path, 1024 * 1024));
    return load(file, std::move(*bytes));
}

void SourceEditor::close(ui::ControlKey key) {
    files_.remove_if([&](const auto& file) { return file->key == key; });
    if (selected_ == key)
        selected_ = files_.empty() ? 0 : files_.back()->key;
    problem_line_ = 0;
}

void SourceEditor::clear() {
    files_.clear();
    selected_ = 0;
    problem_line_ = 0;
}

bool SourceEditor::select(ui::ControlKey key) {
    for (const auto& file : files_)
        if (file->key == key) {
            selected_ = key;
            problem_line_ = 0;
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
        file->stamp = error ? Optional<std::filesystem::file_time_type>{} : Optional{stamp};
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

Array<ui::MenuItem> SourceEditor::menu_items() const {
    Array<ui::MenuItem> result;
    for (const auto& file : files_)
        result.add({file->key,
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

Result<bool> SourceEditor::locate_problem() {
    if (!problem_line_)
        return false;
    if (!problem_path_.empty()) {
        const auto line = problem_line_;
        const auto column = problem_column_;
        const auto path = problem_path_;
        const auto source = problem_source_;
        auto disk = assets::read_file(path, 1024 * 1024);
        if (!disk)
            return false;
        StringView bytes = as_chars(*disk);
        if (bytes.starts_with("\xef\xbb\xbf"))
            bytes = bytes.slice(3);
        if (bytes != source)
            return false;
        ATELIER_TRY(open(path));
        auto* selected = active();
        if (!selected || selected->dirty())
            return false;
        // A forced build can see a newer file before the document poll does.
        // Locate in the exact checked bytes, without replacing an unsaved draft.
        if (selected->disk != *disk) {
            ATELIER_TRY(load(*selected, std::move(*disk)));
        }
        problem_line_ = line;
        problem_column_ = column;
        problem_document_ = selected->document;
        problem_revision_ = selected->document->revision();
    }
    auto* file = active();
    if (!file || !problem_line_ || file->document != problem_document_.lock() ||
        file->document->revision() != problem_revision_)
        return false;
    const auto& buffer = file->document->edit();
    const auto line = buffer.line(std::min(problem_line_ - 1, buffer.line_count() - 1));
    // Compiler columns count UTF-8 bytes; the native editor uses UTF-16 offsets.
    const auto bytes = platform::utf16_to_utf8(line);
    if (!bytes)
        return false;
    auto count = std::min(problem_column_ ? problem_column_ - 1 : 0, bytes->size());
    while (count && count < bytes->size() && (static_cast<unsigned char>((*bytes)[count]) & 0xc0) == 0x80)
        --count;
    file->document->locate(problem_line_,
                           platform::utf8_to_utf16_validated(bytes->view().slice(0, count)).size() + 1);
    return true;
}

Result<void> SourceEditor::locate(const logging::SourceLocation& location) {
    if (location.file.empty() || location.file.contains('\0'))
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
    const auto disk_chars = as_chars(*disk);
    if (location.version && source::content_hash(disk_chars) != *location.version)
        return failure("The recorded source version is stale; no location was applied.");
    ATELIER_TRY(open(path));
    auto* file = active();
    if (!file || file->dirty())
        return failure("Save or discard the source draft before locating a recorded position.");
    if (file->disk != *disk)
        ATELIER_TRY(load(*file, std::move(*disk)));
    auto& buffer = file->document->edit();
    if (!location.line && location.offset) {
        if (*location.offset > file->disk.size())
            return failure("The recorded source byte offset is outside the file.");
        auto offset = static_cast<std::size_t>(*location.offset);
        const auto file_chars = as_chars(file->disk);
        while (offset && offset < file_chars.size() && (static_cast<unsigned char>(file_chars[offset]) & 0xc0) == 0x80)
            --offset;
        ATELIER_TRY_BIND(prefix, normalized(file_chars.slice(0, offset)));
        if (!ui::CodeDocument::valid_text(*prefix))
            return failure("Open a UTF-8 text file; binary/control characters are not editable.");
        buffer.select({static_cast<std::uint32_t>(prefix->size()), false}, false);
        return {};
    }
    const auto row = location.line ? location.line : 1;
    if (row > buffer.line_count())
        return failure("The recorded source line is outside the file.");
    const auto bytes = platform::utf16_to_utf8(buffer.line(row - 1));
    if (!bytes)
        return failure("Invalid UTF-16 text.");
    auto column = std::min<std::size_t>(location.column ? location.column - 1 : 0, bytes->size());
    while (column && column < bytes->size() && (static_cast<unsigned char>((*bytes)[column]) & 0xc0) == 0x80)
        --column;
    file->document->locate(row, platform::utf8_to_utf16_validated(bytes->view().slice(0, column)).size() + 1);
    return {};
}

Result<void> SourceEditor::append(ui_language::ControlOutput controls, ui::ControlKey parent,
                                  ui_language::LiveDocument& live,
                                  FunctionRef<Result<ui_language::ActionBinding>(OperatorCall)> action,
                                  const Project* project, const ProjectBuild* build, bool running) {
    using namespace ui_language;
    auto* file = active();
    std::wstring diagnostic;
    std::wstring status = L"Open a source file from Asset Browser or File > Open Source.";
    bool conflict = false;
    problem_line_ = 0;
    problem_path_.clear();
    if (file) {
        auto& buffer = file->document->edit();
        const auto offset = buffer.current().caret.offset;
        auto location = file->path.wstring();
        if (project) {
            const auto relative = file->path.lexically_relative(project->root());
            if (!relative.empty() && *relative.begin() != L"..")
                location = project->name() + L" / " + relative.generic_wstring();
        }
        status = location + L"  |  Ln " + std::to_wstring(buffer.line_at(offset) + 1) + L", Col " +
                 std::to_wstring(offset - buffer.line_start(buffer.line_at(offset)) + 1);
        conflict = file->conflict;
        if (conflict)
            diagnostic = L"File changed on disk. Reload discards this draft; Save asks before overwriting.";
        else if (!file->storage_error.empty())
            diagnostic = application::diagnostic_text(file->storage_error);
        std::error_code error;
        const auto* ui_source = &live;
        error.clear();
        const bool authoring = std::filesystem::equivalent(file->path, ui_source->path(), error) && !error;
        if (authoring) {
            const auto source = ui_source->attempted_source();
            const bool current = source && *source == as_chars(file->disk) && !file->dirty();
            status += (file->dirty() ? L"  |  Unsaved" : L"  |  Saved");
            status += L"  |  UI revision " + std::to_wstring(ui_source->revision());
            if (!ui_source->diagnostic().empty() && diagnostic.empty()) {
                diagnostic = application::diagnostic_text(ui_source->diagnostic());
                if (!current)
                    diagnostic = L"Previous saved version: " + diagnostic;
                else {
                    const auto name = platform::utf16_to_utf8(ui_source->path().filename().native());
                    StringView message = ui_source->diagnostic();
                    const auto prefix = name ? String::concat(*name, ":") : String{};
                    if (name && message.starts_with(prefix.view())) {
                        message = message.slice(prefix.size());
                        const auto first =
                            std::from_chars(message.data(), message.data() + message.size(), problem_line_);
                        if (first.ec == std::errc{} && first.ptr != message.data() + message.size() &&
                            *first.ptr == ':') {
                            const auto second =
                                std::from_chars(first.ptr + 1, message.data() + message.size(), problem_column_);
                            if (second.ec != std::errc{} || second.ptr == message.data() + message.size() ||
                                *second.ptr != ':')
                                problem_line_ = 0;
                        } else
                            problem_line_ = 0;
                        problem_revision_ = file->document->revision();
                        problem_document_ = file->document;
                    }
                }
            }
        } else
            status += file->dirty() ? L"  |  Unsaved" : L"  |  Saved";
    }
    if (project && build) {
        status += running ? L"  |  Running" : L"  |  Project";
        status += build->state == BuildState::ready              ? L" · Build ready"
                  : build->state == BuildState::rebuild_required ? L" · Rebuild required"
                                                                 : L" · Build failed";
        if (build->error && diagnostic.empty()) {
            diagnostic = application::diagnostic_text(build->error->message);
            if (const auto& location = build->error->source; location && build->sources) {
                const auto at = std::ranges::find(build->sources->files, location->path, &source::SourceFile::path);
                const auto* input = at != build->sources->files.end()                              ? &*at
                                    : build->ui_source && build->ui_source->path == location->path ? &*build->ui_source
                                                                                                   : nullptr;
                if (input && input->version == location->version) {
                    auto name = platform::utf8_to_utf16(location->path);
                    auto path = name ? project->resolve(*name) : failure("Invalid UTF-8 text.");
                    if (path) {
                        problem_path_ = *path;
                        problem_source_ = input->source;
                        problem_line_ = location->line;
                        problem_column_ = location->column;
                        if (file && file->path == problem_path_) {
                            bool previous = file->dirty();
                            if (!previous) {
                                const auto text = normalized(problem_source_.view());
                                previous =
                                    !text || !ui::CodeDocument::valid_text(*text) || *text != file->document->text();
                            }
                            if (previous) {
                                diagnostic = L"Previous saved version: " + diagnostic;
                                problem_line_ = 0;
                            }
                        }
                    }
                }
            }
        }
    }
    ATELIER_TRY_BIND(open_action, action({"source.open"}));
    ATELIER_TRY_BIND(save_action,
                     action({"file.save", json::Document{}, {.selection = {}, .region = RegionKind::code}}));
    ATELIER_TRY_BIND(close_action, action({"source.close"}));
    ATELIER_TRY_BIND(reload_action, action({"source.reload"}));
    ATELIER_TRY_BIND(new_component_action, action({"source.new_component"}));
    ATELIER_TRY_BIND(build_action, action({"project.build"}));
    ATELIER_TRY_BIND(start_play_action, action({"play.start"}));
    ATELIER_TRY_BIND(stop_play_action, action({"play.stop"}));
    const std::array<InputValue, 17> inputs{
        CodeBinding{file ? file->document : empty_, file != nullptr},
        bounded(file ? file->path.filename().wstring() + (file->dirty() ? L" *" : L"") : L"No source document"),
        bounded(status),
        bounded(diagnostic),
        !diagnostic.empty(),
        problem_line_ != 0,
        conflict,
        source_control::files,
        std::move(*open_action),
        std::move(*save_action),
        std::move(*close_action),
        std::move(*reload_action),
        std::move(*new_component_action),
        std::move(*build_action),
        std::move(*start_play_action),
        std::move(*stop_play_action),
        running};
    const std::array<KeyBinding, 3> keys{
        {{"buffer", source_control::buffer}, {"files", source_control::files}, {"problem", source_control::problem}}};
    ATELIER_TRY(live.append("CodePanel", parent, keys, inputs, controls));
    return {};
}
}
