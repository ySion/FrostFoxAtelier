#include "application/window.hpp"
#include <stdexcept>

namespace atelier::application {
namespace { constexpr wchar_t window_class[] = L"FrostFoxAtelier.Editor"; }
Window::Window() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = window_class;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register editor window class.");
    RECT size{0, 0, 1280, 800};
    AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    handle_ = CreateWindowExW(0, window_class, L"FrostFox Atelier | Scene", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
        nullptr, nullptr, cls.hInstance, this);
    if (!handle_) throw std::runtime_error("Cannot create editor window.");
}
Window::~Window() { if (handle_) DestroyWindow(handle_); }
void Window::show() {
    // The first call can honor a launcher's hidden STARTUPINFO instead of SW_SHOW.
    ShowWindow(handle_, SW_SHOW); ShowWindow(handle_, SW_SHOW); UpdateWindow(handle_);
}
float Window::dpi_scale() const noexcept { return static_cast<float>(GetDpiForWindow(handle_)) / 96.f; }
void Window::capture(bool enabled) {
    if (enabled && GetCapture() != handle_) SetCapture(handle_);
    else if (!enabled && GetCapture() == handle_) {
        releasing_capture_ = true; ReleaseCapture(); releasing_capture_ = false;
    }
}
void Window::poll(std::vector<NativeEvent>& output) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { closing_ = true; continue; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    output.clear();
    output.swap(events_); // synchronous capture messages enqueue for the next poll
}
LRESULT CALLBACK Window::procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->handle_ = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(handle, message, wparam, lparam);
    return self->receive(message, wparam, lparam);
}
LRESULT Window::receive(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CLOSE: closing_ = true; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint; BeginPaint(handle_, &paint); EndPaint(handle_, &paint); return 0; }
    case WM_GETMINMAXINFO: {
        const auto dpi = GetDpiForWindow(handle_);
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {MulDiv(900, static_cast<int>(dpi), 96), MulDiv(560, static_cast<int>(dpi), 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        const auto bounds = *reinterpret_cast<RECT*>(lparam);
        SetWindowPos(handle_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        events_.push_back({message, wparam, 0}); // LPARAM points to stack storage
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (releasing_capture_) return 0;
        [[fallthrough]];
    case WM_SIZE: case WM_SETFOCUS: case WM_KILLFOCUS:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        events_.push_back({message, wparam, lparam, (GetKeyState(VK_SHIFT) & 0x8000) != 0,
            (GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_MENU) & 0x8000) != 0});
        return 0;
    default: return DefWindowProcW(handle_, message, wparam, lparam);
    }
}
}
