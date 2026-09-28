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
};
struct NativeInputStatistics {
    std::uint64_t compositions{}, updates{}, results{}, cancellations{}, candidate_opens{};
};

// Owns only the native window. WM_CLOSE requests shutdown; the application must
// close the renderer before this owner destroys the HWND.
class Window final {
public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    HWND handle() const noexcept { return handle_; }
    void show();
    // Drain owned records after each Dispatch, including synchronous callbacks.
    // The consumer never runs from WndProc.
    void poll(const std::function<void(NativeEvent&&)>&);
    bool closing() const noexcept { return closing_; }
    float dpi_scale() const noexcept;
    void capture(bool enabled);
    ui::ClipboardResult clipboard(const ui::ClipboardRequest&);
    ui::ImeResult ime(const ui::ImeRequest&);
    std::uint32_t text_input(const std::optional<ui::TextInputState>&);
    const NativeInputStatistics& input_statistics() const noexcept { return input_statistics_; }
private:
    HWND handle_{};
    bool closing_{}, releasing_capture_{};
    std::vector<NativeEvent> events_;
    std::exception_ptr callback_failure_;
    std::optional<ui::EditToken> input_token_;
    ui::EditToken composition_token_;
    std::uint64_t composition_id_{}, native_composition_id_{}, next_composition_{1};
    wchar_t high_surrogate_{};
    std::array<std::uint8_t, 256> key_owner_{}; // 0 up, 1 UI, 2 IME; unchanged until key-up.
    bool input_enabled_{true};
    std::optional<RECT> candidate_rect_;
    NativeInputStatistics input_statistics_;
    NativeEvent event(UINT, WPARAM = 0, LPARAM = 0) const;
    void composition(UINT, LPARAM);
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT receive(UINT, WPARAM, LPARAM);
};
}
