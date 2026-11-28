#pragma once

#include <Windows.h>
#include "ui/session.hpp"
#include "foundation/array.hpp"
#include "foundation/function_ref.hpp"
#include "foundation/optional.hpp"
#include "foundation/string.hpp"
#include "runtime/runtime.hpp"
#include <array>

namespace atelier::application {
struct NativeEvent {
    UINT message{};

    WPARAM wparam{};

    LPARAM lparam{};

    bool shift{};

    bool control{};

    bool alt{};

    bool ime_owned{};

    std::wstring text;

    Optional<ui::CompositionEvent> composition;

    std::uint32_t error{};

    struct Motion {
        float x{};

        float y{};
    };

    Optional<Motion> relative_motion;
};

// Shared translation for editor and project windows. The caller handles
// surface changes and application commands before dispatching UI input.
Optional<ui::Event> input_event(NativeEvent, HWND);

// Owns only the native window. WM_CLOSE requests shutdown; the application must
// close the renderer before this owner destroys the HWND.
class Window final {
public:
    Window() = default;

    // Optional initial main-window client extent is in physical pixels, independent of DPI.
    // Initialize once at this stable address; failure leaves no HWND and permits destruction only.
    Result<void> initialize(HWND popup_owner = nullptr, Optional<ui::PixelExtent> initial_client = {});

    ~Window();

    Window(const Window&) = delete;

    Window& operator=(const Window&) = delete;

    HWND handle() const noexcept { return handle_; }

    void show(bool activate = true); // false keeps the current foreground window and focus.

    // Main windows pump the thread queue and drain owned records after each
    // Dispatch. Owned popups only drain records queued by that pump or synchronous
    // callbacks, with native errors carried by the originating event. The consumer never runs from WndProc.
    // The callback is borrowed only until poll returns, never retained.
    void poll(FunctionRef<void(NativeEvent&&)>);

    enum class QuitSource : std::uint8_t {
        none,
        wm_syscommand_close,
        wm_close,
        wm_quit,
        operator_app_quit,
    };

    static constexpr StringView quit_source_name(QuitSource source) noexcept {
        switch (source) {
        case QuitSource::none:
            return "none";
        case QuitSource::wm_syscommand_close:
            return "wm_syscommand_close";
        case QuitSource::wm_close:
            return "wm_close";
        case QuitSource::wm_quit:
            return "wm_quit";
        case QuitSource::operator_app_quit:
            return "operator:app.quit";
        }
        return "none";
    }

    bool closing() const noexcept { return quit_source_ != QuitSource::none; }

    QuitSource quit_source() const noexcept { return quit_source_; }

    void request_operator_close() noexcept;

    void cancel_close() noexcept {
        quit_source_ = QuitSource::none;
        pending_syscommand_close_ = false;
    }

    float dpi_scale() const noexcept;

    void capture(bool enabled);

    // Raw mouse ownership is bounded by capture/focus; disabling also restores the pointer.
    bool relative_pointer(bool enabled);

    void cursor(ui::Cursor);

    ui::ClipboardResult clipboard(const ui::ClipboardRequest&);

    ui::ImeResult ime(const ui::ImeRequest&);

    std::uint32_t text_input(const Optional<ui::TextInputState>&);

private:
    HWND handle_{};

    HCURSOR cursor_{};

    HCURSOR adjustment_cursor_{};

    int cursor_size_{};

    struct ToolCursor {
        ui::Cursor intent;

        UINT dpi;

        HCURSOR handle;
    };

    Array<ToolCursor> tool_cursors_; // Owned, keyed by semantic intent and display DPI.

    QuitSource quit_source_{QuitSource::none};

    bool pending_syscommand_close_{};

    void latch_close(QuitSource source) noexcept {
        if (quit_source_ == QuitSource::none)
            quit_source_ = source;
    }

    bool releasing_capture_{};

    bool popup_{};

    bool relative_pointer_{};

    bool tracking_pointer_{};

    POINT pointer_before_{};

    RECT clip_before_{};

    Array<NativeEvent> events_;

    Optional<ui::EditToken> input_token_;

    Optional<ui::TextInputAnchor> input_anchor_;

    ui::EditToken composition_token_;

    ui::EditToken native_composition_token_;

    std::uint64_t composition_id_{};

    std::uint64_t native_composition_id_{};

    std::uint64_t next_composition_{1};

    wchar_t high_surrogate_{};

    std::array<std::uint8_t, 256> key_owner_{}; // 0 up, 1 UI, 2 IME; unchanged until key-up.

    bool input_enabled_{true};

    Optional<RECT> candidate_rect_;

    NativeEvent event(UINT, WPARAM = 0, LPARAM = 0) const;

    std::uint32_t cancel_ime();

    void composition(UINT, LPARAM);

    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM) noexcept;

    LRESULT receive(UINT, WPARAM, LPARAM);
};
}
