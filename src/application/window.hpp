#pragma once

#include <Windows.h>
#include <vector>

namespace atelier::application {
struct NativeEvent { UINT message; WPARAM wparam; LPARAM lparam; bool shift{}, control{}, alt{}; };

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
    void poll(std::vector<NativeEvent>&);
    bool closing() const noexcept { return closing_; }
    float dpi_scale() const noexcept;
    void capture(bool enabled);
private:
    HWND handle_{};
    bool closing_{}, releasing_capture_{};
    std::vector<NativeEvent> events_;
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT receive(UINT, WPARAM, LPARAM);
};
}
