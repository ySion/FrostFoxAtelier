#pragma once

#include "ui/frame.hpp"
#include "ui_layout/layout.hpp"
#include <expected>
#include <limits>
#include <span>
#include <string>

namespace atelier::ui {

using ControlKey = std::uint64_t; // Application key; zero denotes the layout root.
enum class ControlKind { container, label, button, list, item, number, splitter, viewport };
enum class DragAxis { horizontal, vertical };

struct Control {
    ControlKey key{}, parent{}; // Unique live keys; parent precedes child in the input.
    ControlKind kind{ControlKind::container};
    ui_layout::NodeInput layout{ui_layout::NodeInput::column()};
    std::wstring text;
    float font_size{14}; // Logical pixels (DIPs).
    bool enabled{true}, selected{};
    std::optional<Color> background;
    double value{}, minimum{-10000}, maximum{10000};
    double drag_step{0.01}, fine_step{0.001}; // Value per logical pointer pixel.
    DragAxis drag_axis{DragAxis::horizontal};
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
};
struct UiFrame {
    std::shared_ptr<const Frame> drawing;
    std::vector<ControlBox> controls; // Draw order, also used for reverse hit testing.
    std::uint64_t session_identity{};
    const ControlBox* find(ControlKey) const noexcept;
};

enum class ActionKind { activate, select, edit_begin, edit_preview, edit_commit, edit_cancel };
struct Action {
    ActionKind kind{};
    ControlKey key{};
    double value{}; // Cancel returns the gesture's original value.
};
enum class Capture { unchanged, acquire, release };
enum class Cursor { arrow, text, drag_horizontal, resize_horizontal, resize_vertical };
struct Reply {
    bool consumed{}, redraw{};
    Capture capture{Capture::unchanged};
    Cursor cursor{Cursor::arrow};
    std::vector<Action> actions;
};
enum class EventKind { pointer_move, pointer_down, pointer_up, wheel, key_down, key_up,
                       text, focus_lost, capture_lost };
enum class PointerButton { primary, secondary, middle };
enum class Key { none, backspace, delete_key, enter, escape, tab, left, right, home, end,
                 space, a, z, y };
struct Event {
    EventKind kind{};
    float x{}, y{}; // Client physical pixels; never reinterpret using a future frame.
    float wheel{}; // Positive scrolls toward the beginning, measured in wheel notches.
    PointerButton button{PointerButton::primary};
    Key key{Key::none};
    char32_t character{}; // Text input, separate from key commands. Numeric ASCII only.
    bool shift{}, control{}, alt{};
};
enum class ErrorCode { invalid_surface, unavailable_surface, invalid_control, layout_failed,
                       text_failed, capacity_exceeded };
struct Error {
    ErrorCode code{};
    ControlKey key{};
    std::uint32_t native_code{};
};
template<class T> using Result = std::expected<T, Error>;

// One owner thread. Owns UI state and content versions, not scene objects or undo.
// Keep returned frames through render(); accept only after final renderer accepted.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Resize/DPI transitions cancel presses/edits and gate activation until a
    // matching candidate has been accepted. Zero extent is a valid suspended state.
    Result<Reply> set_surface(Surface);
    // Descriptors are copied. Apply returned cancellation effects when controls
    // disappear/disable. Number editing buffers survive fresh scene projections.
    Result<Reply> set_controls(std::span<const Control>);
    Result<std::shared_ptr<const UiFrame>> prepare();
    bool accept(std::shared_ptr<const UiFrame>);
    const std::shared_ptr<const UiFrame>& presented() const noexcept;
    Reply input(const Event&);
    Reply cancel_interaction();
    bool wants_keyboard() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atelier::ui
