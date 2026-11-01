#include "editor/source_editor.hpp"
#include "editor/project_code.hpp"
#include "assets/file.hpp"
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace atelier::editor {
namespace {
auto failure(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(message)});
}
std::wstring normalized(std::string_view bytes) {
    if (bytes.starts_with("\xef\xbb\xbf"))
        bytes.remove_prefix(3);
    auto value = assets::wide(bytes);
    std::wstring result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\r') {
            result += L'\n';
            if (i + 1 < value.size() && value[i + 1] == L'\n')
                ++i;
        } else
            result += value[i];
    }
    if (!ui::detail::valid_text(result, false) ||
        std::ranges::any_of(result, [](wchar_t c) { return (c < 32 && c != L'\t' && c != L'\n') || c == 127; }))
        throw std::runtime_error("Open a UTF-8 text file; binary/control characters are not editable.");
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
Result<void> SourceEditor::load(File& file, std::string bytes) {
    try {
        auto text = normalized(bytes);
        auto document = std::make_shared<ui::CodeDocument>(text);
        if (file.document) {
            const auto& old = file.document->edit();
            const auto offset = old.current().caret.offset;
            document->locate(old.line_at(offset) + 1, offset - old.line_start(old.line_at(offset)) + 1);
        }
        file.bom = bytes.starts_with("\xef\xbb\xbf");
        file.crlf = bytes.find("\r\n") != bytes.npos;
        file.disk = std::move(bytes);
        file.saved_text = std::move(text);
        file.document = std::move(document);
        file.checked_revision = 0;
        file.conflict = false;
        file.storage_error.clear();
        return {};
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}
SourceEditor::File* SourceEditor::active() const noexcept {
    const auto at = std::ranges::find_if(files_, [&](const auto& file) { return file->key == selected_; });
    return at == files_.end() ? nullptr : at->get();
}
Result<void> SourceEditor::open(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error)
        return failure("Cannot open source file: " + assets::utf8(path.wstring()));
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
    auto bytes = assets::read_file(canonical, 1024 * 1024);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto file = std::make_unique<File>();
    file->path = canonical;
    file->key = next_key_++;
    if (auto loaded = load(*file, std::move(*bytes)); !loaded)
        return loaded;
    selected_ = file->key;
    problem_line_ = 0;
    files_.push_back(std::move(file));
    return {};
}
Result<void> SourceEditor::save(File& file, bool overwrite) {
    auto disk = assets::read_file(file.path, 1024 * 1024);
    if (!overwrite && (!disk || *disk != file.disk)) {
        file.conflict = true;
        return failure("The file changed on disk. Reload it or confirm overwriting the external changes.");
    }
    try {
        auto text = assets::utf8(file.document->text());
        if (file.crlf) {
            std::string converted;
            converted.reserve(text.size() + file.document->edit().line_count());
            for (auto c : text) {
                if (c == '\n')
                    converted += '\r';
                converted += c;
            }
            text = std::move(converted);
        }
        if (file.bom)
            text.insert(0, "\xef\xbb\xbf");
        if (text.size() > 1024 * 1024)
            return failure("Source file exceeds 1 MiB of UTF-8 text.");
        if (auto saved = write_file(file.path, text); !saved)
            return saved;
        file.document->edit().break_group();
        file.disk = std::move(text);
        file.saved_text = file.document->text();
        file.checked_revision = 0;
        file.conflict = false;
        file.storage_error.clear();
        return {};
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}
Result<void> SourceEditor::reload(File& file) {
    auto bytes = assets::read_file(file.path, 1024 * 1024);
    if (!bytes)
        return std::unexpected(bytes.error());
    return load(file, std::move(*bytes));
}
void SourceEditor::close(ui::ControlKey key) {
    std::erase_if(files_, [&](const auto& file) { return file->key == key; });
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
bool SourceEditor::locate_problem() {
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
        std::string_view bytes = *disk;
        if (bytes.starts_with("\xef\xbb\xbf"))
            bytes.remove_prefix(3);
        if (bytes != source)
            return false;
        if (!open(path))
            return false;
        auto* selected = active();
        if (!selected || selected->dirty())
            return false;
        // A forced build can see a newer file before the document poll does.
        // Locate in the exact checked bytes, without replacing an unsaved draft.
        if (selected->disk != *disk && !load(*selected, std::move(*disk)))
            return false;
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
    const auto bytes = assets::utf8(line);
    auto count = std::min(problem_column_ ? problem_column_ - 1 : 0, bytes.size());
    while (count && count < bytes.size() && (static_cast<unsigned char>(bytes[count]) & 0xc0) == 0x80)
        --count;
    file->document->locate(problem_line_, assets::wide(std::string_view(bytes).substr(0, count)).size() + 1);
    return true;
}
Result<void> SourceEditor::locate(const logging::SourceLocation& location) {
    try {
        if (location.file.empty() || location.file.find('\0') != std::string::npos)
            return failure("This record has no usable disk source path.");
        if (location.version && location.disk_path.empty())
            return failure("This Source version has no saved-file mapping; it cannot be located on disk.");
        auto path =
            std::filesystem::path{assets::wide(location.disk_path.empty() ? location.file : location.disk_path)};
        if (!path.is_absolute()) {
            const auto root = std::filesystem::path{assets::wide(location.root)};
            if (!root.is_absolute() || path.has_root_path())
                return failure("Relative log source has no original absolute root.");
            path = root / path;
        }
        auto disk = assets::read_file(path, 1024 * 1024);
        if (!disk)
            return std::unexpected(disk.error());
        if (location.version && source::content_hash(*disk) != *location.version)
            return failure("The recorded source version is stale; no location was applied.");
        if (auto opened = open(path); !opened)
            return opened;
        auto* file = active();
        if (!file || file->dirty())
            return failure("Save or discard the source draft before locating a recorded position.");
        if (file->disk != *disk)
            if (auto loaded = load(*file, *disk); !loaded)
                return loaded;
        auto& buffer = file->document->edit();
        if (!location.line && location.offset) {
            if (*location.offset > disk->size())
                return failure("The recorded source byte offset is outside the file.");
            auto offset = static_cast<std::size_t>(*location.offset);
            while (offset && offset < disk->size() && (static_cast<unsigned char>((*disk)[offset]) & 0xc0) == 0x80)
                --offset;
            const auto prefix = normalized(std::string_view(*disk).substr(0, offset));
            buffer.select({static_cast<std::uint32_t>(prefix.size()), false}, false);
            return {};
        }
        const auto row = location.line ? location.line : 1;
        if (row > buffer.line_count())
            return failure("The recorded source line is outside the file.");
        const auto bytes = assets::utf8(buffer.line(row - 1));
        auto column = std::min<std::size_t>(location.column ? location.column - 1 : 0, bytes.size());
        while (column && column < bytes.size() && (static_cast<unsigned char>(bytes[column]) & 0xc0) == 0x80)
            --column;
        file->document->locate(row, assets::wide(std::string_view(bytes).substr(0, column)).size() + 1);
        return {};
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}
void SourceEditor::append(std::vector<ui::Control>& controls, ui::ControlKey parent, ui_language::LiveDocument& live,
                          const std::function<ui_language::ActionBinding(OperatorCall)>& action, const Project* project,
                          const ProjectBuild* build, bool running) {
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
            diagnostic = assets::wide(file->storage_error);
        std::error_code error;
        const auto* ui_source = &live;
        error.clear();
        const bool authoring = std::filesystem::equivalent(file->path, ui_source->path(), error) && !error;
        if (authoring) {
            const auto source = ui_source->attempted_source();
            const bool current = source && *source == file->disk && !file->dirty();
            status += (file->dirty() ? L"  |  Unsaved" : L"  |  Saved");
            status += L"  |  UI revision " + std::to_wstring(ui_source->revision());
            if (!ui_source->diagnostic().empty() && diagnostic.empty()) {
                diagnostic = assets::wide(ui_source->diagnostic());
                if (!current)
                    diagnostic = L"Previous saved version: " + diagnostic;
                else {
                    const auto prefix = assets::utf8(ui_source->path().filename().wstring()) + ":";
                    auto message = ui_source->diagnostic();
                    if (message.starts_with(prefix)) {
                        message.remove_prefix(prefix.size());
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
            diagnostic = assets::wide(build->error->message);
            if (const auto& location = build->error->source; location && build->sources) {
                const auto at = std::ranges::find(build->sources->files, location->path, &source::SourceFile::path);
                const auto* input = at != build->sources->files.end()                              ? &*at
                                    : build->ui_source && build->ui_source->path == location->path ? &*build->ui_source
                                                                                                   : nullptr;
                if (input && input->version == location->version) {
                    auto path = project->resolve(assets::wide(location->path));
                    if (path) {
                        problem_path_ = *path;
                        problem_source_ = input->source;
                        problem_line_ = location->line;
                        problem_column_ = location->column;
                        if (file && file->path == problem_path_ &&
                            (file->dirty() || normalized(problem_source_) != file->document->text())) {
                            diagnostic = L"Previous saved version: " + diagnostic;
                            problem_line_ = 0;
                        }
                    }
                }
            }
        }
    }
    const std::array<InputValue, 17> inputs{
        CodeBinding{file ? file->document : empty_, file != nullptr},
        bounded(file ? file->path.filename().wstring() + (file->dirty() ? L" *" : L"") : L"No source document"),
        bounded(status),
        bounded(diagnostic),
        !diagnostic.empty(),
        problem_line_ != 0,
        conflict,
        source_control::files,
        action({"source.open"}),
        action({"file.save", assets::json_format::Json::object(), {.selection = {}, .region = RegionKind::code}}),
        action({"source.close"}),
        action({"source.reload"}),
        action({"source.new_component"}),
        action({"project.build"}),
        action({"play.start"}),
        action({"play.stop"}),
        running};
    const std::array<KeyBinding, 3> keys{
        {{"buffer", source_control::buffer}, {"files", source_control::files}, {"problem", source_control::problem}}};
    static_cast<void>(live.append("CodePanel", parent, keys, inputs, controls));
}
}
