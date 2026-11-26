#include "application/window.hpp"
#include <windowsx.h>

namespace atelier::application {
namespace {
ui::Key key(WPARAM value) {
    switch (value) {
    case VK_BACK:
        return ui::Key::backspace;
    case VK_DELETE:
        return ui::Key::delete_key;
    case VK_RETURN:
        return ui::Key::enter;
    case VK_ESCAPE:
        return ui::Key::escape;
    case VK_TAB:
        return ui::Key::tab;
    case VK_LEFT:
        return ui::Key::left;
    case VK_RIGHT:
        return ui::Key::right;
    case VK_UP:
        return ui::Key::up;
    case VK_DOWN:
        return ui::Key::down;
    case VK_PRIOR:
        return ui::Key::page_up;
    case VK_NEXT:
        return ui::Key::page_down;
    case VK_APPS:
        return ui::Key::menu;
    case VK_F2:
        return ui::Key::f2;
    case VK_F3:
        return ui::Key::f3;
    case VK_F10:
        return ui::Key::f10;
    case VK_F12:
        return ui::Key::f12;
    case VK_OEM_2:
        return ui::Key::slash;
    case VK_HOME:
        return ui::Key::home;
    case VK_END:
        return ui::Key::end;
    case VK_SPACE:
        return ui::Key::space;
    case 'A':
        return ui::Key::a;
    case 'D':
        return ui::Key::d;
    case 'F':
        return ui::Key::f;
    case 'G':
        return ui::Key::g;
    case 'H':
        return ui::Key::h;
    case 'K':
        return ui::Key::k;
    case 'Z':
        return ui::Key::z;
    case 'Y':
        return ui::Key::y;
    case 'C':
        return ui::Key::c;
    case 'X':
        return ui::Key::x;
    case 'V':
        return ui::Key::v;
    default:
        return ui::Key::none;
    }
}

struct ClickTracker {
    HWND last_window{};

    ULONGLONG last_dbl_tick{};

    int last_x{};

    int last_y{};
};

ClickTracker click_tracker{};
}

std::optional<ui::Event> input_event(NativeEvent message, HWND window) {
    if (message.ime_owned || message.error)
        return {};
    ui::Event event;
    event.shift = message.shift;
    event.control = message.control;
    event.alt = message.alt;
    if (message.composition) {
        event.kind = ui::EventKind::composition;
        event.data = std::move(*message.composition);
        return event;
    }
    event.x = static_cast<float>(GET_X_LPARAM(message.lparam));
    event.y = static_cast<float>(GET_Y_LPARAM(message.lparam));
    switch (message.message) {
    case WM_MOUSEMOVE:
        event.kind = ui::EventKind::pointer_move;
        break;
    case WM_MOUSELEAVE:
        event.kind = ui::EventKind::pointer_leave;
        break;
    case WM_LBUTTONDOWN: {
        event.kind = ui::EventKind::pointer_down;
        const ULONGLONG now = GetTickCount64();
        const int x = GET_X_LPARAM(message.lparam);
        const int y = GET_Y_LPARAM(message.lparam);
        if (click_tracker.last_window == window && click_tracker.last_dbl_tick != 0 &&
            now - click_tracker.last_dbl_tick <= GetDoubleClickTime() &&
            std::abs(x - click_tracker.last_x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
            std::abs(y - click_tracker.last_y) <= GetSystemMetrics(SM_CYDOUBLECLK)) {
            event.clicks = 3;
            click_tracker.last_dbl_tick = 0;
        }
        break;
    }
    case WM_LBUTTONDBLCLK:
        event.kind = ui::EventKind::pointer_down;
        event.clicks = 2;
        click_tracker.last_window = window;
        click_tracker.last_dbl_tick = GetTickCount64();
        click_tracker.last_x = GET_X_LPARAM(message.lparam);
        click_tracker.last_y = GET_Y_LPARAM(message.lparam);
        break;
    case WM_LBUTTONUP:
        event.kind = ui::EventKind::pointer_up;
        break;
    case WM_RBUTTONDOWN:
        event.kind = ui::EventKind::pointer_down;
        event.button = ui::PointerButton::secondary;
        break;
    case WM_RBUTTONUP:
        event.kind = ui::EventKind::pointer_up;
        event.button = ui::PointerButton::secondary;
        break;
    case WM_MBUTTONDOWN:
        event.kind = ui::EventKind::pointer_down;
        event.button = ui::PointerButton::middle;
        break;
    case WM_MBUTTONUP:
        event.kind = ui::EventKind::pointer_up;
        event.button = ui::PointerButton::middle;
        break;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(message.lparam), GET_Y_LPARAM(message.lparam)};
        ScreenToClient(window, &point);
        event.x = static_cast<float>(point.x);
        event.y = static_cast<float>(point.y);
        event.kind = ui::EventKind::wheel;
        event.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(message.wparam)) / WHEEL_DELTA;
        break;
    }
    case WM_KEYDOWN:
        event.kind = ui::EventKind::key_down;
        event.key = key(message.wparam);
        event.repeat = (message.lparam & (1LL << 30)) != 0;
        break;
    case WM_KEYUP:
        event.kind = ui::EventKind::key_up;
        event.key = key(message.wparam);
        break;
    case WM_CHAR:
        event.kind = ui::EventKind::text;
        event.data = std::move(message.text);
        break;
    case WM_KILLFOCUS:
        event.kind = ui::EventKind::focus_lost;
        break;
    case WM_CAPTURECHANGED:
        event.kind = ui::EventKind::capture_lost;
        break;
    default:
        return {};
    }
    return event;
}
}
