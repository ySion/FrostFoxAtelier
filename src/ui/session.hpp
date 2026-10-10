#pragma once

#include "ui/frame.hpp"
#include "ui/style.hpp"
#include "ui/style_sheet.hpp"
#include "ui/table.hpp"
#include "ui/code_document.hpp"
#include "ui_layout/layout.hpp"
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <variant>

namespace atelier::ui {

namespace detail { class TextSnapshot; }

using ControlKey = std::uint64_t; // Application key; zero denotes the layout root.
enum class DragAxis { horizontal, vertical };
enum class ButtonBehavior { command, toggle };
enum class MenuBehavior { command, checkbox, radio };
enum class EditCommandKind { copy, paste, reset };
struct EditCommand {
    EditCommandKind kind{};
    ControlKey target{};
    bool operator==(const EditCommand&) const = default;
};

struct Control {
    ControlKey key{}, parent{}; // Unique live keys; parent precedes child in the input.
    ControlKind kind{ControlKind::container};
    ui_layout::NodeInput layout{ui_layout::NodeInput::column()};
    std::wstring text;
    bool single_line{}; // Labels: one line with end ellipsis when their allocated width is exhausted.
    Role role{Role::normal};
    ControlIcon icon{ControlIcon::none}; // Standalone or leading glyph; text reserves the same measured icon space.
    bool enabled{true}, selected{};
    bool click_action{}; // splitter grip only: activate on click; bool payload means Alt modifier.
    double value{}, minimum{-10000}, maximum{10000};
    bool single_precision{}; // Float storage uses its shortest round-tripping decimal, without widened double noise.
    double drag_step{0.01}, fine_step{0.001}; // Number/splitter: per DIP. Slider: keyboard increment.
    DragAxis drag_axis{DragAxis::horizontal};
    std::vector<std::string> classes;
    StyleProperties style;
    std::shared_ptr<const StyleSheet> style_scope; // Applies to this control and its descendants.
    bool invalid{}, modified{}; // Modified compares the authoritative value with its declared default.
    std::optional<double> default_value;
    ControlKey context_target{}, context_group{}; // Context request target and optional group reset command.
    ControlKey selection_group{}; // Button group: arrow navigation; Tab visits the selected member.
    ControlKey action_target{}; // Button/table activation route; zero uses this control's key. Never a UI identity.
    // Tree rows are direct layout children of a tree, in depth-first order.
    // tree_parent references an earlier row in that same tree (zero for roots).
    // Expansion belongs to Session; initialize on insertion or kind/parent changes.
    // A host selected false-to-true transition expands logical ancestors and
    // reveals the row once; stable selected input preserves manual collapse.
    ControlKey tree_parent{};
    bool tree_initially_expanded{true};
    std::wstring tree_filter; // tree only: substring with ASCII case folding; retains matching ancestry.
    ControlKey popup_owner{}; // popup only: earlier visible local owner; zero for an externally owned surface.
    bool menu_branch{}; // Host opens a child menu; activating keeps this parent modal.
    std::optional<EditCommand> edit_command; // menu_item only; executes through the shared editor.
    ButtonBehavior button_behavior{ButtonBehavior::command}; // button only: request the inverse selected value.
    MenuBehavior menu_behavior{MenuBehavior::command}; // menu_item only; selected is the authoritative checked value.
    ControlKey radio_group{}; // Nonzero for radio menu items; at most one selected item per group.
    std::shared_ptr<const TableData> table; // table only; no layout children or synthetic row controls.
    TableSelection table_selection; // Lightweight host selection, independent of immutable row storage.
    TableLayout table_layout;
    std::shared_ptr<CodeDocument> code; // CodeEditor only; document owns text and history.
    bool operator==(const Control&) const = default;
};
struct Surface {
    PixelExtent extent;
    float dpi_scale{1};
    std::uint64_t revision{1}; // Advance on client-coordinate/DPI mapping changes.
    bool operator==(const Surface&) const = default;
};
struct ControlBox {
    ControlKey key{};
    ControlKey parent{};
    ControlKind kind{};
    Rect bounds; // Physical pixels, including scrolling.
    Clip clip; // Effective ancestor clip, in the same space.
    bool enabled{};
    std::uint64_t incarnation{}; // UI lifetime guard; not a business object ID.
    std::shared_ptr<const ResolvedStyle> style;
    bool selected{}; // Accepted toggle value; activation requests its inverse.
    ControlKey tree_parent{}; // Logical parent snapshot, independent of the layout parent.
    ControlKey action_target{}; // Accepted route; retargeting invalidates input against this frame.
    ButtonBehavior button_behavior{ButtonBehavior::command}; // Accepted activation semantics.
    MenuBehavior menu_behavior{MenuBehavior::command};
    ControlKey radio_group{};
    std::shared_ptr<const TableData> table; // Exact immutable content accepted with this geometry.
    TableSelection table_selection;
    ControlKey context_target{}, context_group{}; // Accepted menu route, including group actions.
    TableLayout table_layout;
    std::shared_ptr<CodeDocument> code; // CodeEditor only; document owns text and history.
};
struct EditToken {
    ControlKey key{};
    std::uint64_t incarnation{}, generation{}, revision{};
    bool operator==(const EditToken&) const = default;
};
struct TextCaret {
    EditToken token;
    Rect bounds; // Client physical pixels from the same accepted drawing.
    Clip clip;
};
struct TextInputAnchor {
    EditToken token;
    std::uint64_t composition_id{}, composition_revision{};
    Rect bounds; // Composition/replacement start; client physical pixels, height is line height.
    Clip clip; // Effective visible editable region, in the same space.
};
struct TextInputState {
    EditToken token;
    bool composing{};
    std::optional<TextCaret> accepted_caret;
    std::optional<TextInputAnchor> accepted_anchor{}; // Only IME offset zero is supported.
};
// Focus and passive text bindings are not pending interactions. Field drafts
// require commit/cancel; CodeDocument edits already belong to their document.
struct InteractionState {
    bool pointer{}, keyboard{}, text_draft{}, composition{}, clipboard{}, popup{};
    bool active() const noexcept { return pointer || keyboard || text_draft || composition || clipboard || popup; }
};
struct TextGeometry {
    ControlKey key{};
    std::uint64_t incarnation{};
    std::shared_ptr<const detail::TextSnapshot> paragraph;
    float x{}, y{}, dpi_scale{1}, scroll{}; // Final displayed origin, including horizontal scroll.
    EditToken token;
    std::uint64_t composition_id{};
};
struct ScrollbarGeometry {
    ControlKey key{};
    Rect bounds, track, thumb, decrement, increment;
    Clip clip;
    double maximum{}, value{}, page{};
    bool horizontal{};
};
struct ValueLadderGeometry {
    ControlKey key{};
    Rect bounds; // Frozen client pixels for this captured gesture.
    float row_height{};
    int selected_row{3};
    double increment{.1};
};
struct TreeRowGeometry {
    ControlKey key{}, parent{};
    Rect disclosure; // Accepted physical hit area; empty on leaves.
    bool expanded{}, filtering{}; // Retained expansion; filtering temporarily reveals matching descendants.
};
struct TableColumnGeometry {
    TableKey column{};
    Rect bounds, resize;
    Clip clip;
};
struct TableCellGeometry {
    TableKey row{}, column{}, cell{};
    Rect bounds;
    Clip clip;
};
struct TableGeometry {
    ControlKey key{};
    std::shared_ptr<const TableData> data;
    TableSelection selection;
    Rect header, body;
    Clip clip;
    std::vector<TableColumnGeometry> columns{};
    std::vector<TableCellGeometry> cells{}; // Only visible cells; stable data identities, never ControlKeys.
    std::size_t columns_per_row{1}; // Actual accepted grid width for keyboard navigation.
    float row_stride{}; // Physical pixels between visual rows.
};
struct CodeLineGeometry {
    std::size_t row{};
    std::uint32_t start{};
    std::shared_ptr<const detail::TextSnapshot> paragraph;
    float x{}, y{};
};
struct CodeGeometry {
    ControlKey key{};
    std::uint64_t revision{}, selection_revision{};
    Rect body;
    float line_height{}, scale{1};
    std::vector<CodeLineGeometry> lines; // Accepted visible lines only.
};
struct UiFrame {
    std::shared_ptr<const Frame> drawing;
    std::vector<ControlBox> controls; // Draw order, also used for reverse hit testing.
    std::uint64_t session_identity{};
    std::uint64_t controls_revision{}; // Complete descriptor batch used by this candidate.
    std::uint64_t style_revision{}, measure_provider_revision{};
    std::optional<TextCaret> caret;
    std::optional<TextInputAnchor> input_anchor{};
    std::vector<TextGeometry> text_geometry;
    std::vector<ScrollbarGeometry> scrollbars; // Accepted geometry shared by paint and input.
    std::vector<TreeRowGeometry> tree_rows;
    std::vector<CodeGeometry> code_editors;
    std::vector<TableGeometry> tables;
    std::optional<ValueLadderGeometry> value_ladder;
    ControlKey popup{};
    const ControlBox* find(ControlKey) const noexcept;
    const TextGeometry* find_text(ControlKey) const noexcept;
    const TableGeometry* find_table(ControlKey) const noexcept;
    const CodeGeometry* find_code(ControlKey) const noexcept;
};

enum class ActionKind { activate, select, edit_begin, edit_preview, edit_commit, edit_cancel, context_menu, dismiss, reset, drag_begin, drag_preview, drag_commit, drag_cancel, table_sort, table_column_resize };
struct ContextMenuRequest { ControlKey source{}, group{}; float x{}, y{}; std::optional<TableSelection> table{}; }; // Accepted client pixels.
struct DragPoint { float x{}, y{}; }; // Accepted-frame client pixels.
struct SelectionModifiers { bool shift{}, control{}; };
using ActionValue = std::variant<std::monostate,double,std::wstring,bool,ContextMenuRequest,DragPoint,TableSelection,TableSort,TableColumnResize,SelectionModifiers>;
struct Action {
    ActionKind kind{};
    ControlKey key{};
    ActionValue value; // Cancel returns the original value; toggles activate with the requested bool.
    ControlKey target{}; // Optional host route. key always identifies the control that emitted the action.
    bool keep_menu_open{}; // Checkbox indicator click; host refreshes the authoritative value in place.
};
enum class ClipboardKind { read, copy, cut };
struct ClipboardRequest {
    ClipboardKind kind{};
    std::uint64_t operation_id{};
    EditToken token;
    std::wstring text; // Owned selected text for copy/cut.
    std::size_t limit{4096}; // Bounded by the active editor; native adapters cap at 1 MiB UTF-16 units.
};
struct ClipboardResult {
    ClipboardKind kind{};
    std::uint64_t operation_id{};
    EditToken token;
    bool success{};
    std::uint32_t native_error{};
    std::wstring text; // Owned CF_UNICODETEXT payload for read.
};
enum class ImeKind { complete, cancel };
struct ImeRequest {
    ImeKind kind{};
    std::uint64_t composition_id{};
    EditToken token;
};
struct ImeResult {
    ImeKind kind{};
    std::uint64_t composition_id{};
    EditToken token;
    bool success{};
    std::uint32_t native_error{};
};
enum class CompositionKind { start, update, end, cancel };
struct Preedit {
    std::wstring text;
    std::uint32_t cursor{}; // UTF-16 offset, clamped to an editing boundary by UI.
    std::vector<std::uint8_t> attributes; // IMM attributes per UTF-16 unit; empty if absent.
};
struct CompositionEvent {
    CompositionKind kind{};
    std::uint64_t composition_id{};
    EditToken token; // Fixed start binding; advanced only after a confirmed segment.
    std::optional<std::wstring> result;
    std::optional<Preedit> preedit; // Missing differs from an empty preedit.
};
using PlatformRequest = std::variant<ClipboardRequest,ImeRequest>;
using Effect = std::variant<Action,PlatformRequest>;

enum class ErrorCode { invalid_surface, unavailable_surface, invalid_control, layout_failed,
                       text_failed, capacity_exceeded, invalid_style, invalid_text, platform_failed };
struct Error {
    ErrorCode code{};
    ControlKey key{};
    std::uint32_t native_code{};
};
template<class T> using Result = std::expected<T, Error>;

// Validate a producer's descriptor before publishing it. This does not inspect
// tree references, table contents or solve layout; Session remains the complete batch boundary.
Result<void> validate_control_properties(const Control&);
enum class Capture { unchanged, acquire, release };
enum class Cursor { arrow, text, drag_horizontal, resize_horizontal, resize_vertical, move, pan, crosshair };
struct Reply {
    bool consumed{}, redraw{};
    Capture capture{Capture::unchanged};
    std::optional<Cursor> cursor; // No change for non-pointer events.
    std::vector<Effect> effects; // Execute in order; business failure stops ordinary effects, not cleanup.
    std::optional<Error> error; // Input/platform failure; the edit and its history remain intact.
};
enum class EventKind { pointer_move, pointer_down, pointer_up, wheel, key_down, key_up,
                       text, focus_lost, capture_lost, clipboard_result, ime_result, composition };
enum class PointerButton { primary, secondary, middle };
enum class Key { none, backspace, delete_key, enter, escape, tab, left, right, home, end,
                 space, a, z, y, c, x, v, up, down, menu, f10, page_up, page_down };
using InputData = std::variant<std::monostate,std::wstring,ClipboardResult,ImeResult,CompositionEvent>;
struct Event {
    EventKind kind{};
    float x{}, y{}; // Client physical pixels; never reinterpret using a future frame.
    float wheel{}; // Positive scrolls toward the beginning, measured in wheel notches.
    PointerButton button{PointerButton::primary};
    Key key{Key::none};
    InputData data{}; // Text is an owned, complete UTF-16 string, separate from key commands.
    bool shift{}, control{}, alt{};
    unsigned clicks{1}; // Native click count; two activates the accepted table row.
    bool repeat{}; // Native key auto-repeat; one-shot commands can ignore repeats.
};
struct Statistics {
    // Cumulative since Session construction. prepares/rasterizations count
    // attempts; text_layouts counts successful native layout creations;
    // raster_bytes sums completed R8 pixel payloads, excluding native overhead.
    std::uint64_t prepares{}, measure_calls{}, text_layouts{}, rasterizations{}, raster_bytes{}, coverage_cache_hits{};
    std::uint64_t culled_text_controls{}, provider_revision{};
    // Unique CPU coverages produced by this Session and still retained by any
    // owner (text cache, current/old frames or renderer). No per-frame scan.
    std::uint64_t live_coverages{}, live_coverage_bytes{};
    // Successful descriptor changes; identical values do not count as updates.
    std::uint64_t control_batches{}, controls_updated{}, controls_created{}, controls_removed{};
    // Attempts to synchronize a dirty control, and successful layout-node creates.
    std::uint64_t layout_syncs{}, layout_nodes_created{};
    ui_layout::Stats layout;
};

// One owner thread. Owns UI state and content versions, not scene objects or undo.
// Keep returned frames through render(); accept only after final renderer accepted.
class Session {
public:
    // Optional application fonts join the system collection for this Session.
    // Files must remain available for its lifetime; no global font installation.
    explicit Session(std::span<const std::wstring> font_files = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Result<void> set_style(const Style&); // Validate/copy the complete value before applying.
    Result<void> set_style_sheet(std::shared_ptr<const StyleSheet>);
    const Style& style() const noexcept;
    Statistics statistics() const noexcept;
    // Resize/DPI transitions cancel presses/edits and gate activation until a
    // matching candidate has been accepted. Zero extent is a valid suspended state.
    Result<Reply> set_surface(Surface);
    // Descriptors are copied. Apply returned cancellation effects when controls
    // disappear/disable. Active text drafts survive fresh model projections;
    // passive buffers instead follow the latest authoritative value.
    Result<Reply> set_controls(std::span<const Control>);
    // Sparse complete values for existing keys. Order is irrelevant; duplicate or
    // missing keys and changes to kind/parent/structural references are rejected.
    // Both description entries validate and prepare before changing live controls.
    // Layout dependencies are resolved by prepare; its failure retains the old
    // frame, not the previous descriptors or cancelled interaction.
    Result<Reply> update_controls(std::span<const Control>);
    Result<std::shared_ptr<const UiFrame>> prepare();
    bool accept(std::shared_ptr<const UiFrame>);
    const std::shared_ptr<const UiFrame>& presented() const noexcept;
    // Topmost enabled target in the accepted frame; zero while input is suspended.
    ControlKey pointer_target(float x,float y) const;
    Reply input(const Event&);
    Reply cancel_interaction();
    // Finish a valid text draft before a document save. Invalid input stays
    // editable; callers must apply the returned effects before serialization.
    Result<Reply> commit_edit();
    // Zero clears focus; other targets must be visible, current accepted controls.
    // A transition finishes the previous draft. Same focus preserves its draft,
    // selection and composition; text fields bind input immediately, numbers passively.
    Reply focus(ControlKey);
    Reply execute(EditCommand, std::uint64_t incarnation); // External menu command, guarded by its opening target lifetime.
    ControlKey focused_control() const noexcept;
    bool wants_keyboard() const noexcept;
    InteractionState interaction_state() const noexcept;
    bool has_popup() const noexcept; // Modal barrier includes an opening/closing frame transition.
    // Live binding plus matching accepted caret only; no pending-frame geometry.
    std::optional<TextInputState> text_input() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atelier::ui
