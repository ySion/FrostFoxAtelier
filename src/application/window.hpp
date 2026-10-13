#pragma once

#include <Windows.h>
#include "ui/session.hpp"
#include <array>
#include <exception>
#include <functional>
#include <vector>

namespace atelier::application {
struct NativeEvent {
    UINT message{};
    WPARAM wparam{};
    LPARAM lparam{};
    bool shift{}, control{}, alt{}, ime_owned{};
    std::wstring text;
    std::optional<ui::CompositionEvent> composition;
    std::uint32_t error{};
    struct Motion {float x{},y{};};
    std::optional<Motion> relative_motion;
};
struct NativeInputStatistics {
    std::uint64_t compositions{}, updates{}, results{}, cancellations{}, candidate_opens{}, candidate_changes{};
};
// Shared translation for editor and project windows. The caller handles
// surface changes and application commands before dispatching UI input.
std::optional<ui::Event> input_event(NativeEvent,HWND);

// Owns only the native window. WM_CLOSE requests shutdown; the application must
// close the renderer before this owner destroys the HWND.
class Window final {
public:
    explicit Window(HWND popup_owner = nullptr);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    HWND handle() const noexcept { return handle_; }
    void show(bool activate = true); // false keeps the current foreground window and focus.
    // Main windows pump the thread queue and drain owned records after each
    // Dispatch. Owned popups only drain records queued by that pump or synchronous
    // callbacks, including callback failures. The consumer never runs from WndProc.
    void poll(const std::function<void(NativeEvent&&)>&);
    bool closing() const noexcept { return closing_; }
    void cancel_close() noexcept { closing_=false; }
    float dpi_scale() const noexcept;
    void capture(bool enabled);
    // Raw mouse ownership is bounded by capture/focus; disabling also restores the pointer.
    bool relative_pointer(bool enabled);
    void cursor(ui::Cursor);
    ui::ClipboardResult clipboard(const ui::ClipboardRequest&);
    ui::ImeResult ime(const ui::ImeRequest&);
    std::uint32_t text_input(const std::optional<ui::TextInputState>&);
    const NativeInputStatistics& input_statistics() const noexcept { return input_statistics_; }
private:
    HWND handle_{};
    HCURSOR cursor_{}, adjustment_cursor_{};
    int cursor_size_{};
    struct ToolCursor {ui::Cursor intent;UINT dpi;HCURSOR handle;};
    std::vector<ToolCursor> tool_cursors_; // Owned, keyed by semantic intent and display DPI.
    bool closing_{}, releasing_capture_{};
    bool popup_{};
    bool relative_pointer_{};
    POINT pointer_before_{};
    RECT clip_before_{};
    std::vector<NativeEvent> events_;
    std::exception_ptr callback_failure_;
    std::optional<ui::EditToken> input_token_;
    std::optional<ui::TextInputAnchor> input_anchor_;
    ui::EditToken composition_token_, native_composition_token_;
    std::uint64_t composition_id_{}, native_composition_id_{}, next_composition_{1};
    wchar_t high_surrogate_{};
    std::array<std::uint8_t, 256> key_owner_{}; // 0 up, 1 UI, 2 IME; unchanged until key-up.
    bool input_enabled_{true};
    std::optional<RECT> candidate_rect_;
    NativeInputStatistics input_statistics_;
    NativeEvent event(UINT, WPARAM = 0, LPARAM = 0) const;
    std::uint32_t cancel_ime();
    void composition(UINT, LPARAM);
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT receive(UINT, WPARAM, LPARAM);
};
}
