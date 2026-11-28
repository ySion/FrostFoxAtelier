#pragma once
#include "editor/project.hpp"
#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "ui/code_document.hpp"
#include "ui/controls.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace atelier {
class Runtime;
}

namespace atelier::editor {
struct ProjectBuild;

[[nodiscard]] ui::CodeLanguage detect_code_language(const std::filesystem::path& path) noexcept;

[[nodiscard]] StringView code_language_name(ui::CodeLanguage language) noexcept;

struct OpenSourceView {
    std::filesystem::path path;

    const ui::CodeDocument* document{};

    bool dirty{};
};

struct CodeProblemItem {
    ui::CodeDiagnosticSeverity severity{ui::CodeDiagnosticSeverity::error};

    std::filesystem::path path;

    String file_label;

    std::size_t line{1};

    std::size_t column{1};

    String message;

    bool operator==(const CodeProblemItem&) const = default;
};

struct DocumentSymbol {
    String name;

    String detail;

    ui::ControlIcon icon{ui::ControlIcon::component};

    std::uint32_t line{1};

    std::uint32_t column{1};

    std::uint32_t end_line{1};

    bool is_member{};

    bool operator==(const DocumentSymbol&) const = default;
};

struct CodeCompletionItem {
    String badge;

    String badge_class;

    String label;

    String detail;

    String documentation;

    String insert_text;

    std::uint32_t replace_start{};

    std::uint32_t replace_end{};

    std::uint32_t cursor_backtrack{};

    int score{};
};

struct CodeSignatureHelp {
    String prefix;

    String active_parameter;

    String suffix;

    String documentation;

    bool operator==(const CodeSignatureHelp&) const = default;
};

struct CodeDefinitionTarget {
    std::filesystem::path path;

    std::size_t line{1};

    std::size_t column{1};

    bool operator==(const CodeDefinitionTarget&) const = default;
};

struct CodeHoverInfo {
    ui::Tooltip tooltip;

    std::uint32_t range_start{};

    std::uint32_t range_end{};

    Optional<CodeDefinitionTarget> definition;
};

class CodeIntelligence {
public:
    CodeIntelligence();

    ~CodeIntelligence();

    CodeIntelligence(const CodeIntelligence&) = delete;

    CodeIntelligence& operator=(const CodeIntelligence&) = delete;

    void reset();

    // Synchronizes symbol tables and runs debounced (or immediate) in-memory validation
    // without mutating the live World or requiring disk writes.
    bool sync(const Runtime* world_runtime, const Project* project, const ProjectBuild* build,
              Span<const OpenSourceView> open_files, const OpenSourceView* active_file, bool force_immediate = false);

    [[nodiscard]] bool check_pending() const noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept;

    [[nodiscard]] Span<const ui::CodeDiagnostic> active_diagnostics() const noexcept;

    [[nodiscard]] Span<const CodeProblemItem> problems() const noexcept;

    [[nodiscard]] Span<const DocumentSymbol> outline() const noexcept;

    [[nodiscard]] String breadcrumb_for_line(std::size_t zero_based_line) const;

    [[nodiscard]] Span<const String> semantic_types() const noexcept;

    [[nodiscard]] Span<const String> semantic_functions() const noexcept;

    [[nodiscard]] Span<const String> semantic_fields() const noexcept;

    [[nodiscard]] Array<CodeCompletionItem> completions(const ui::CodeDocument& document, std::uint32_t caret_offset,
                                                        bool explicit_request) const;

    [[nodiscard]] Optional<CodeSignatureHelp> signature_help(const ui::CodeDocument& document,
                                                             std::uint32_t caret_offset) const;

    [[nodiscard]] Optional<CodeHoverInfo> hover(const ui::CodeDocument& document, std::uint32_t doc_offset) const;

    [[nodiscard]] Optional<CodeDefinitionTarget> definition(const ui::CodeDocument& document,
                                                            std::uint32_t doc_offset) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};
} // namespace atelier::editor
