#pragma once
#include "editor/code_intelligence.hpp"
#include "editor/operators.hpp"
#include "editor/project.hpp"
#include "foundation/array.hpp"
#include "foundation/function_ref.hpp"
#include "foundation/optional.hpp"
#include "foundation/ring_deque.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "logging/logging.hpp"
#include "ui_language/live_document.hpp"
#include <array>
#include <chrono>

namespace atelier::editor {
struct ProjectBuild;

namespace source_control {
inline constexpr ui::ControlKey tab = 605;

inline constexpr ui::ControlKey content = 660;

inline constexpr ui::ControlKey buffer = 661;

inline constexpr ui::ControlKey files = 662;

inline constexpr ui::ControlKey problem = 663;

inline constexpr ui::ControlKey close_document_offset = 1ULL << 32;

inline constexpr ui::ControlKey first_document = 1ULL << 45;

inline constexpr ui::ControlKey ui_base = 1ULL << 46;

inline constexpr ui::ControlKey outline = ui_base + 1;

inline constexpr ui::ControlKey nav_back = ui_base + 2;

inline constexpr ui::ControlKey nav_forward = ui_base + 3;

inline constexpr ui::ControlKey find_toggle = ui_base + 4;

inline constexpr ui::ControlKey problems_toggle = ui_base + 5;

inline constexpr ui::ControlKey find_input = ui_base + 6;

inline constexpr ui::ControlKey replace_input = ui_base + 7;

inline constexpr ui::ControlKey goto_input = ui_base + 8;

inline constexpr ui::ControlKey find_prev = ui_base + 9;

inline constexpr ui::ControlKey find_next = ui_base + 10;

inline constexpr ui::ControlKey find_case = ui_base + 11;

inline constexpr ui::ControlKey find_word = ui_base + 12;

inline constexpr ui::ControlKey replace_one = ui_base + 13;

inline constexpr ui::ControlKey replace_all = ui_base + 14;

inline constexpr ui::ControlKey goto_commit = ui_base + 15;

inline constexpr ui::ControlKey search_close = ui_base + 16;

inline constexpr ui::ControlKey open_goto = ui_base + 17;

inline constexpr ui::ControlKey problems_table = ui_base + 18;

inline constexpr ui::ControlKey problem_jump = ui_base + 19;

inline constexpr ui::ControlKey problems_col_first = ui_base + 32;

inline constexpr ui::ControlKey outline_item_first = ui_base + 128;

inline constexpr ui::ControlKey completion_row_first = ui_base + 1024;

inline constexpr ui::ControlKey completion_pick_first = ui_base + 2048;
} // namespace source_control

enum class CodeSearchMode : std::uint8_t { closed, find, replace, goto_line };

// Files belong to the authoring workspace, independent of a scene Runtime.
// The only persistent artifact is UTF-8 source on disk; UI scopes retain buffers.
class SourceEditor {
public:
    struct File {
        ui::ControlKey key{};

        std::filesystem::path path;

        std::shared_ptr<ui::CodeDocument> document;

        bool dirty() const;

        bool conflict{};

        String storage_error;

    private:
        friend class SourceEditor;

        Array<std::byte> disk;

        bool bom{};

        bool crlf{};

        Optional<std::filesystem::file_time_type> stamp;

        std::uintmax_t disk_size{};
    };

    Result<void> open(const std::filesystem::path&);

    Result<void> save(File&, bool overwrite = false);

    Result<void> reload(File&);

    void close(ui::ControlKey);

    void clear();

    bool select(ui::ControlKey);

    File* find(ui::ControlKey) const noexcept;

    File* active() const noexcept;

    void relocate(const std::filesystem::path& old_path, const std::filesystem::path& new_path);

    const Array<std::unique_ptr<File>>& files() const noexcept { return files_; }

    void poll(bool composing = false);

    Result<void> append(ui_language::ControlOutput, ui::ControlKey, ui_language::LiveDocument&,
                        FunctionRef<Result<ui_language::ActionBinding>(OperatorCall)>, const Project* = nullptr,
                        const ProjectBuild* = nullptr, bool running = false, const Runtime* world_runtime = nullptr,
                        const ui::UiFrame* frame = nullptr);

    Array<ui::MenuItem> menu_items() const;

    Array<ui::MenuItem> outline_menu_items() const;

    Result<bool> locate_problem();

    // Original absolute path/root, optional Source content version and UTF-8
    // byte column. Refuses stale locations and never overwrites a dirty draft.
    Result<void> locate(const logging::SourceLocation&);

    // Active diagnostics are actionable whenever CodeIntelligence reports live or project problems.
    bool problem_current() const noexcept { return !intelligence_.problems().empty(); }

    // Synchronizes in-memory language intelligence immediately (also usable headlessly in tests).
    bool sync_intelligence(const Runtime* world_runtime = nullptr, const Project* project = nullptr,
                           const ProjectBuild* build = nullptr, bool force_immediate = false);

    const CodeIntelligence& intelligence() const noexcept { return intelligence_; }

    // Inline Find / Replace / Go-to-Line bar
    ui::ControlKey open_search(CodeSearchMode mode);

    void close_search() noexcept;

    bool search_open() const noexcept { return search_mode_ != CodeSearchMode::closed; }

    CodeSearchMode search_mode() const noexcept { return search_mode_; }

    void set_find_query(StringView query, bool jump_to_nearest = true);

    void set_replace_text(StringView replacement);

    bool find_step(bool forward);

    bool replace_current();

    std::size_t replace_all();

    bool commit_goto_line();

    std::size_t search_match_count() const noexcept { return search_matches_.size(); }

    // Autocomplete & Signature Help
    bool completion_open() const noexcept { return completion_open_ && !completion_items_.empty(); }

    void trigger_completion(bool explicit_request = true);

    void dismiss_completion() noexcept;

    void step_completion(int delta) noexcept;

    bool accept_completion(std::size_t index);

    bool accept_selected_completion();

    Span<const CodeCompletionItem> completion_items() const noexcept { return completion_items_.view(); }

    Optional<CodeSignatureHelp> current_signature_help() const;

    // Navigation & Hover
    bool can_navigate_back() const noexcept { return !nav_back_.empty(); }

    bool can_navigate_forward() const noexcept { return !nav_forward_.empty(); }

    Result<bool> navigate_back();

    Result<bool> navigate_forward();

    Result<bool> go_to_definition(Optional<std::uint32_t> explicit_offset = {});

    bool update_pointer(const ui::UiFrame* frame, float x, float y, bool ctrl_held);

    bool set_ctrl_hover(const ui::UiFrame* frame, bool ctrl_held);

    void dismiss_hover() noexcept;

    // Applies UI actions emitted by CodePanel controls; sets focus_buffer / focus_bar when needed.
    Result<bool> apply_action(const ui::Action& action, bool& focus_buffer, ui::ControlKey& focus_bar);

private:
    using Clock = std::chrono::steady_clock;

    struct NavLocation {
        std::filesystem::path path;

        std::size_t line{1};

        std::size_t column{1};
    };

    Array<std::unique_ptr<File>> files_;

    ui::ControlKey selected_{};

    ui::ControlKey next_key_{source_control::first_document};

    Clock::time_point next_poll_{};

    std::shared_ptr<ui::CodeDocument> empty_{std::make_shared<ui::CodeDocument>()};

    CodeIntelligence intelligence_;

    // Search / Replace / Go-to-Line state
    CodeSearchMode search_mode_{CodeSearchMode::closed};

    String find_query_;

    String replace_text_;

    String goto_text_;

    bool match_case_{};

    bool whole_word_{};

    Array<ui::CodeSearchMatch> search_matches_;

    std::uint32_t active_search_match_{std::numeric_limits<std::uint32_t>::max()};

    std::uint64_t search_revision_{std::numeric_limits<std::uint32_t>::max()};

    ui::ControlKey search_file_key_{};

    std::uint64_t search_generation_{1};

    // Autocomplete & hover state
    bool completion_open_{};

    bool completion_explicit_{};

    Array<CodeCompletionItem> completion_items_;

    std::size_t completion_selected_{};

    ui::ControlKey tracked_file_key_{};

    std::uint64_t tracked_edit_revision_{};

    std::uint32_t tracked_caret_offset_{};

    Optional<CodeHoverInfo> hover_info_;

    float hover_px_x_{};

    float hover_px_y_{};

    float last_pointer_x_{-1.f};

    float last_pointer_y_{-1.f};

    bool ctrl_hover_{};

    // Navigation history & Problems drawer
    RingDeque<NavLocation> nav_back_;

    RingDeque<NavLocation> nav_forward_;

    bool problems_open_{};

    std::size_t selected_problem_row_{};

    std::array<float, 3> problem_col_widths_{68.f, 460.f, 180.f};

    std::uint64_t problem_cols_generation_{1};

    // Retained immutable UI bindings rebuilt only when underlying revisions/generations advance
    struct AnnotationsCacheKey {
        ui::ControlKey file_key{};

        std::uint64_t doc_revision{};

        std::uint64_t intel_generation{};

        std::uint64_t search_generation{};

        std::uint32_t active_search_match{};

        bool show_search{};

        Optional<ui::CodeSearchMatch> link_range;

        bool operator==(const AnnotationsCacheKey&) const noexcept = default;
    };

    AnnotationsCacheKey cached_annotations_key_{};

    std::shared_ptr<const ui::CodeAnnotations> cached_annotations_;

    std::uint64_t cached_problems_intel_gen_{};

    std::uint64_t cached_problems_cols_gen_{};

    std::shared_ptr<const ui::TableData> cached_problems_table_;

    struct SignatureCacheKey {
        ui::ControlKey file_key{};

        std::uint64_t doc_revision{};

        std::uint64_t intel_generation{};

        std::uint32_t caret_offset{};

        bool operator==(const SignatureCacheKey&) const noexcept = default;
    };

    SignatureCacheKey cached_sig_key_{};

    Optional<CodeSignatureHelp> cached_sig_help_;

    ui::ControlKey cached_breadcrumb_file_key_{};

    std::filesystem::path cached_breadcrumb_file_path_;

    std::filesystem::path cached_breadcrumb_project_root_;

    std::wstring cached_breadcrumb_project_name_;

    std::wstring cached_breadcrumb_path_;

    std::uint64_t cached_tabs_digest_{};

    std::shared_ptr<Array<ui_language::Item>> cached_open_tabs_;

    void push_nav_location();

    Result<bool> jump_to_target(const std::filesystem::path& path, std::size_t line, std::size_t column,
                                bool record_history);

    void recompute_search(bool jump_to_nearest);

    static Result<void> load(File&, Array<std::byte>);
};
} // namespace atelier::editor
