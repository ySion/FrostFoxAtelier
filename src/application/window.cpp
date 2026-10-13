#include "application/window.hpp"
#include "ui/icons.hpp"
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace atelier::application {
namespace {
constexpr wchar_t window_class[] = L"FrostFoxAtelier.Editor";
constexpr std::size_t text_limit = 4096;
DWORD native_error() { const auto value = GetLastError(); return value ? value : ERROR_GEN_FAILURE; }
// The same alpha glyph serves editor chrome and native tools. Premultiplied
// BGRA plus a dark dilation keeps light strokes readable on any scene surface.
HCURSOR tool_cursor(ui::Icon icon,UINT dpi) {
    const auto size=std::clamp(MulDiv(32,static_cast<int>(dpi),96),16,256);
    const auto glyph=size*3/4,padding=(size-glyph)/2;
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(glyph*glyph));
    ui::raster_icon(icon,static_cast<std::uint32_t>(glyph),alpha);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size*size));
    const auto sample=[&](int x,int y)->unsigned {
        x-=padding;y-=padding;
        return x>=0 && y>=0 && x<glyph && y<glyph?alpha[static_cast<std::size_t>(y*glyph+x)]:0;
    };
    const auto radius=std::max(1,MulDiv(1,static_cast<int>(dpi),96));
    for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
        const auto ink=sample(x,y);unsigned outline{};
        for(int dy=-radius;dy<=radius;++dy) for(int dx=-radius;dx<=radius;++dx)
            if(dx*dx+dy*dy<=radius*radius+1) outline=std::max(outline,sample(x+dx,y+dy));
        const auto shadow=outline*(255-ink)/255;
        const auto opacity=ink+shadow,color=(240*ink+18*shadow)/255;
        pixels[static_cast<std::size_t>(y*size+x)]=(opacity<<24)|(color<<16)|(color<<8)|color;
    }
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=size;info.bmiHeader.biHeight=-size;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    const auto stride=((size+15)/16)*2;
    std::vector<std::uint8_t> mask_bits(static_cast<std::size_t>(stride*size),0);
    void* bits{};const auto color=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!color) return nullptr;
    std::memcpy(bits,pixels.data(),pixels.size()*sizeof(std::uint32_t));
    const auto mask=CreateBitmap(size,size,1,1,mask_bits.data());
    if(!mask) {DeleteObject(color);return nullptr;}
    ICONINFO cursor{};cursor.fIcon=FALSE;cursor.xHotspot=cursor.yHotspot=static_cast<DWORD>(size/2);
    cursor.hbmMask=mask;cursor.hbmColor=color;
    const auto handle=static_cast<HCURSOR>(CreateIconIndirect(&cursor));
    DeleteObject(mask);DeleteObject(color);return handle;
}
struct InputContext {
    HWND window;
    HIMC context;
    explicit InputContext(HWND value) : window(value), context(ImmGetContext(value)) {}
    ~InputContext() { if (context) ImmReleaseContext(window, context); }
};
struct ClipboardScope { ~ClipboardScope() { CloseClipboard(); } };
struct MemoryLock {
    HGLOBAL memory;
    void* data;
    explicit MemoryLock(HGLOBAL value) : memory(value), data(GlobalLock(value)) {}
    ~MemoryLock() { if (data) GlobalUnlock(memory); }
};
std::expected<std::wstring, DWORD> composition_text(HIMC context, DWORD index) {
    const auto bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (bytes < 0 || bytes % sizeof(wchar_t)) return std::unexpected(ERROR_NO_UNICODE_TRANSLATION);
    if (static_cast<std::size_t>(bytes) > text_limit * sizeof(wchar_t)) return std::unexpected(ERROR_BUFFER_OVERFLOW);
    std::wstring value(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
    if (bytes && ImmGetCompositionStringW(context, index, value.data(), static_cast<DWORD>(bytes)) != bytes)
        return std::unexpected(ERROR_INVALID_DATA);
    return value;
}
}
Window::Window(HWND popup_owner) : popup_(popup_owner!=nullptr) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_DBLCLKS;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = window_class;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register editor window class.");
    const auto dpi=GetDpiForSystem();
    RECT size{0,0,MulDiv(1280,static_cast<int>(dpi),96),MulDiv(840,static_cast<int>(dpi),96)};
    AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    handle_ = CreateWindowExW(popup_?WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE:0, window_class,
        popup_?L"FrostFox Atelier | Menu":L"FrostFox Atelier | Scene", popup_?WS_POPUP:WS_OVERLAPPEDWINDOW,
        popup_?0:CW_USEDEFAULT, popup_?0:CW_USEDEFAULT, popup_?32:size.right-size.left, popup_?32:size.bottom-size.top,
        popup_owner, nullptr, cls.hInstance, this);
    if (!handle_) throw std::runtime_error("Cannot create editor window.");
}
Window::~Window() {
    relative_pointer(false);
    SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    for(const auto& cursor:tool_cursors_) DestroyCursor(cursor.handle);
    tool_cursors_.clear();cursor_=LoadCursorW(nullptr,IDC_ARROW);
    if (handle_) DestroyWindow(handle_);
    if (adjustment_cursor_) {
        if (GetCursor()==adjustment_cursor_) SetCursor(LoadCursorW(nullptr,IDC_ARROW));
        DestroyCursor(adjustment_cursor_);
    }
}
void Window::cursor(ui::Cursor intent) {
    const wchar_t* standard=IDC_ARROW;
    if (intent==ui::Cursor::text) standard=IDC_IBEAM;
    else if (intent==ui::Cursor::resize_horizontal || intent==ui::Cursor::drag_horizontal) standard=IDC_SIZEWE;
    else if (intent==ui::Cursor::resize_vertical) standard=IDC_SIZENS;
    else if (intent==ui::Cursor::move) standard=IDC_SIZEALL;
    else if (intent==ui::Cursor::crosshair) standard=IDC_CROSS;
    cursor_=LoadCursorW(nullptr,standard);
    if (intent==ui::Cursor::drag_horizontal) {
        const int size=GetSystemMetricsForDpi(SM_CXCURSOR,GetDpiForWindow(handle_));
        if (cursor_size_!=size) {
            SetCursor(cursor_);
            if (adjustment_cursor_) DestroyCursor(adjustment_cursor_);
            adjustment_cursor_=nullptr; cursor_size_=size;
            const int stride=((size+15)/16)*2;
            std::vector<BYTE> mask(static_cast<std::size_t>(stride*size),0xff),ink(mask.size(),0);
            // Original adjustment glyph: opposing arrows with a central fine-step notch.
            const auto shape=[](int x,int y) {
                return (y==16 && x>=4 && x<=27) || (x>=4 && x<=9 && std::abs(y-16)==x-4) ||
                    (x>=22 && x<=27 && std::abs(y-16)==27-x) || (x==15 && y>=12 && y<=20);
            };
            for (int y=0;y<size;++y) for (int x=0;x<size;++x) {
                const int sx=x*32/size,sy=y*32/size;
                bool edge=false;
                for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) edge|=shape(sx+dx,sy+dy);
                const auto index=static_cast<std::size_t>(y*stride+x/8); const auto bit=static_cast<BYTE>(0x80>>(x%8));
                if (edge) mask[index]&=static_cast<BYTE>(~bit);
                if (shape(sx,sy)) ink[index]|=bit;
            }
            adjustment_cursor_=CreateCursor(GetModuleHandleW(nullptr),size/2,size/2,size,size,mask.data(),ink.data());
        }
        if (adjustment_cursor_) cursor_=adjustment_cursor_;
    }
    std::optional<ui::Icon> icon;
    if(intent==ui::Cursor::pan) icon=ui::Icon::pan;
    else if(intent==ui::Cursor::orbit) icon=ui::Icon::orbit;
    else if(intent==ui::Cursor::translate) icon=ui::Icon::translate;
    else if(intent==ui::Cursor::rotate) icon=ui::Icon::rotate;
    else if(intent==ui::Cursor::scale) icon=ui::Icon::scale;
    if(icon) {
        const auto dpi=GetDpiForWindow(handle_);
        const auto found=std::ranges::find_if(tool_cursors_,[&](const auto& value){return value.intent==intent && value.dpi==dpi;});
        if(found!=tool_cursors_.end()) cursor_=found->handle;
        else if(const auto handle=tool_cursor(*icon,dpi)) {
            try {tool_cursors_.push_back({intent,dpi,handle});}
            catch(...) {DestroyCursor(handle);throw;}
            cursor_=handle;
        }
    }
    SetCursor(relative_pointer_?nullptr:cursor_);
}
void Window::show(bool activate) {
    if (popup_ || !activate) { ShowWindow(handle_,SW_SHOWNOACTIVATE); UpdateWindow(handle_); return; }
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
bool Window::relative_pointer(bool enabled) {
    // Capture/ClipCursor are desktop-wide effects, never granted to a background host.
    if(enabled && (GetForegroundWindow()!=handle_ || GetFocus()!=handle_)) return false;
    if(enabled==relative_pointer_) return true;
    if(enabled) {
        RECT bounds{};POINT origin{};
        if(!GetCursorPos(&pointer_before_) || !GetClipCursor(&clip_before_) ||
            !GetClientRect(handle_,&bounds) || !ClientToScreen(handle_,&origin)) return false;
        OffsetRect(&bounds,origin.x,origin.y);
        RAWINPUTDEVICE device{1,2,0,handle_};
        if(!RegisterRawInputDevices(&device,1,sizeof(device))) return false;
        capture(true);
        if(GetCapture()!=handle_ || !ClipCursor(&bounds)) {
            device.dwFlags=RIDEV_REMOVE;device.hwndTarget=nullptr;
            RegisterRawInputDevices(&device,1,sizeof(device));capture(false);return false;
        }
        relative_pointer_=true;SetCursor(nullptr);
    } else {
        relative_pointer_=false;
        RAWINPUTDEVICE device{1,2,RIDEV_REMOVE,nullptr};
        RegisterRawInputDevices(&device,1,sizeof(device));
        ClipCursor(&clip_before_);
        if(GetForegroundWindow()==handle_) SetCursorPos(pointer_before_.x,pointer_before_.y);
        SetCursor(cursor_?cursor_:LoadCursorW(nullptr,IDC_ARROW));capture(false);
    }
    return true;
}
void Window::poll(const std::function<void(NativeEvent&&)>& consume) {
    const auto drain = [&] {
        for (std::size_t index = 0; index < events_.size(); ++index) {
            auto owned = std::move(events_[index]);
            if (owned.message == WM_CHAR && !owned.text.empty()) {
                const auto character = owned.text.front();
                if (character >= 0xd800 && character <= 0xdbff) { high_surrogate_ = character; continue; }
                if (character >= 0xdc00 && character <= 0xdfff) {
                    if (high_surrogate_) owned.text.insert(owned.text.begin(), high_surrogate_);
                    else { owned.text.clear(); owned.error = ERROR_NO_UNICODE_TRANSLATION; }
                } else if (high_surrogate_) { owned.text.clear(); owned.error = ERROR_NO_UNICODE_TRANSLATION; }
                high_surrogate_ = 0;
            }
            if (owned.composition) {
                auto& composition = *owned.composition;
                if (composition.kind == ui::CompositionKind::start) {
                    if (!input_token_ || composition.token != *input_token_) continue;
                    composition_id_ = composition.composition_id;
                    composition_token_ = composition.token;
                }
                if (composition.composition_id == composition_id_) {
                    composition.token = composition_token_;
                    if (composition.kind == ui::CompositionKind::end || composition.kind == ui::CompositionKind::cancel)
                        composition_id_ = 0;
                }
            }
            consume(std::move(owned)); // receive() may append; no reference into events_ survives.
        }
        events_.clear();
        if (callback_failure_) std::rethrow_exception(std::exchange(callback_failure_, {}));
    };
    drain();
    if (popup_) return; // The owning main window alone pumps the thread queue.
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { closing_ = true; continue; }
        if (message.hwnd == handle_ && message.message == WM_KEYDOWN) {
            const auto code = message.wParam == VK_PROCESSKEY ? ImmGetVirtualKey(handle_) : message.wParam;
            if (code < key_owner_.size() && !key_owner_[code])
                key_owner_[code] = native_composition_id_ || message.wParam == VK_PROCESSKEY ? 2 : 1;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        drain();
    }
}
NativeEvent Window::event(UINT message, WPARAM wparam, LPARAM lparam) const {
    NativeEvent value;
    value.message = message; value.wparam = wparam; value.lparam = lparam;
    value.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    value.control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    value.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    return value;
}
LRESULT CALLBACK Window::procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->handle_ = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(handle, message, wparam, lparam);
    try { return self->receive(message, wparam, lparam); }
    catch (...) { self->callback_failure_ = std::current_exception(); return 0; }
}
LRESULT Window::receive(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message==WM_SETCURSOR && LOWORD(lparam)==HTCLIENT) {
        SetCursor(relative_pointer_?nullptr:cursor_?cursor_:LoadCursorW(nullptr,IDC_ARROW)); return TRUE;
    }
    switch (message) {
    case WM_INPUT: {
        if(relative_pointer_) {
            RAWINPUT raw{};UINT bytes=sizeof(raw);
            if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),RID_INPUT,&raw,&bytes,sizeof(RAWINPUTHEADER))!=UINT(-1) &&
                raw.header.dwType==RIM_TYPEMOUSE && !(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)) {
                auto record=event(message,wparam);
                record.relative_motion=NativeEvent::Motion{float(raw.data.mouse.lLastX),float(raw.data.mouse.lLastY)};
                events_.push_back(std::move(record));
            }
        }
        return DefWindowProcW(handle_,message,wparam,lparam);
    }
    case WM_CLOSE: relative_pointer(false);input_anchor_.reset(); closing_ = true; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint; BeginPaint(handle_, &paint); EndPaint(handle_, &paint); return 0; }
    case WM_CANCELMODE:
        relative_pointer(false);
        // A native modal loop can suspend poll(). Release OS capture now via
        // default processing; defer business cancellation until normal delivery.
        events_.push_back(event(message, wparam, lparam));
        return DefWindowProcW(handle_, message, wparam, lparam);
    case WM_GETMINMAXINFO: {
        if (popup_) return 0;
        const auto dpi = GetDpiForWindow(handle_);
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {MulDiv(900, static_cast<int>(dpi), 96), MulDiv(560, static_cast<int>(dpi), 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        input_anchor_.reset();
        const auto bounds = *reinterpret_cast<RECT*>(lparam);
        SetWindowPos(handle_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        events_.push_back(event(message, wparam)); // LPARAM points to stack storage
        return 0;
    }
    case WM_IME_SETCONTEXT:
        return DefWindowProcW(handle_, message, wparam, lparam & ~ISC_SHOWUICOMPOSITIONWINDOW);
    case WM_IME_REQUEST:
        if (wparam == IMR_QUERYCHARPOSITION) {
            auto* position = reinterpret_cast<IMECHARPOSITION*>(lparam);
            if (!position || position->dwSize != sizeof(IMECHARPOSITION) || position->dwCharPos != 0 ||
                !input_anchor_ || !input_token_ || input_anchor_->token != *input_token_ || GetFocus() != handle_) return 0;
            const auto& origin = input_anchor_->bounds;
            const auto& document = input_anchor_->clip;
            POINT point{static_cast<LONG>(std::floor(origin.x)), static_cast<LONG>(std::floor(origin.y))};
            if (!ClientToScreen(handle_, &point)) return 0;
            POINT corners[2]{{document.left, document.top}, {document.right, document.bottom}};
            if (!ClientToScreen(handle_, &corners[0]) || !ClientToScreen(handle_, &corners[1])) return 0;
            position->pt = point;
            position->cLineHeight = static_cast<UINT>(std::ceil(origin.height));
            position->rcDocument = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
            return TRUE;
        }
        return DefWindowProcW(handle_, message, wparam, lparam);
    case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION: case WM_IME_ENDCOMPOSITION:
        composition(message, lparam); return 0;
    case WM_IME_CHAR: return 0; // Confirmed strings have one owner: GCS_RESULTSTR.
    case WM_IME_NOTIFY:
        if (wparam == IMN_OPENCANDIDATE) ++input_statistics_.candidate_opens;
        if (wparam == IMN_CHANGECANDIDATE) ++input_statistics_.candidate_changes;
        return DefWindowProcW(handle_, message, wparam, lparam);
    case WM_INPUTLANGCHANGE:
        input_anchor_.reset();
        candidate_rect_.reset();
        return DefWindowProcW(handle_, message, wparam, lparam);
    case WM_CHAR: {
        auto value = event(message, wparam, lparam);
        const auto character = static_cast<wchar_t>(wparam);
        if (character < 0x20 || character == 0x7f) return 0; // Commands arrive as key events.
        value.text.push_back(character); // Assemble surrogate pairs in ordered pump delivery.
        events_.push_back(std::move(value)); return 0;
    }
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        // Keep system shortcuts such as Alt+F4 with Windows; menu mnemonics,
        // F10 and the Blender viewport chords (Alt+A/G/R/S) use the same queued
        // key ownership as ordinary controls.
        if (wparam!=VK_F10 && wparam!=VK_MENU && wparam!='F' && wparam!='E' && wparam!='C' && wparam!='V' && wparam!='W' &&
            wparam!='A' && wparam!='G' && wparam!='R' && wparam!='S')
            return DefWindowProcW(handle_,message,wparam,lparam);
        [[fallthrough]];
    case WM_KEYDOWN: case WM_KEYUP: {
        const auto key_message=message==WM_SYSKEYDOWN?WM_KEYDOWN:message==WM_SYSKEYUP?WM_KEYUP:message;
        auto value = event(key_message, wparam, lparam);
        const auto code = wparam == VK_PROCESSKEY ? ImmGetVirtualKey(handle_) : wparam;
        if (code < key_owner_.size()) {
            if (key_message == WM_KEYDOWN && !key_owner_[code])
                key_owner_[code] = native_composition_id_ || wparam == VK_PROCESSKEY ? 2 : 1;
            value.ime_owned = key_owner_[code] == 2;
            if (key_message == WM_KEYUP) key_owner_[code] = 0;
        }
        if (value.ime_owned) return DefWindowProcW(handle_, message, wparam, lparam);
        events_.push_back(std::move(value)); return 0;
    }
    case WM_KILLFOCUS:
        relative_pointer(false);
        input_anchor_.reset();
        high_surrogate_ = 0; key_owner_.fill(0);
        events_.push_back(event(message, wparam, lparam)); return 0;
    case WM_SYSCHAR: return 0; // Menu mnemonics are handled by their key event.
    case WM_CAPTURECHANGED:
        if (releasing_capture_) return 0;
        relative_pointer(false);
        [[fallthrough]];
    case WM_SIZE:
        input_anchor_.reset(); events_.push_back(event(message, wparam, lparam)); return 0;
    case WM_SETFOCUS:
    case WM_MOVE: case WM_ACTIVATEAPP:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        events_.push_back(event(message, wparam, lparam));
        return 0;
    case WM_MOUSEACTIVATE:
        if (popup_) return MA_NOACTIVATE;
        return DefWindowProcW(handle_,message,wparam,lparam);
    default: return DefWindowProcW(handle_, message, wparam, lparam);
    }
}
void Window::composition(UINT message, LPARAM flags) {
    input_anchor_.reset(); // Native changes invalidate the previously displayed composition before UI delivery.
    auto record = event(message, 0, flags);
    if (message == WM_IME_STARTCOMPOSITION) {
        candidate_rect_.reset();
        native_composition_id_ = next_composition_++;
        native_composition_token_ = input_token_.value_or(ui::EditToken{});
        record.composition = ui::CompositionEvent{ui::CompositionKind::start, native_composition_id_, native_composition_token_, {}, {}};
        ++input_statistics_.compositions;
    } else if (native_composition_id_) {
        auto& value = record.composition.emplace();
        value.composition_id = native_composition_id_;
        value.token = native_composition_token_;
        if (message == WM_IME_ENDCOMPOSITION) {
            value.kind = ui::CompositionKind::end; native_composition_id_ = 0;
        } else if (!flags) {
            value.kind = ui::CompositionKind::cancel; native_composition_id_ = 0; ++input_statistics_.cancellations;
        } else {
            value.kind = ui::CompositionKind::update; ++input_statistics_.updates;
            InputContext input(handle_);
            if (!input.context) record.error = ERROR_INVALID_HANDLE;
            else {
                if (flags & GCS_RESULTSTR) {
                    auto text = composition_text(input.context, GCS_RESULTSTR);
                    if (text) { value.result = std::move(*text); ++input_statistics_.results; }
                    else record.error = text.error();
                }
                if (flags & (GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS)) {
                    auto text = composition_text(input.context, GCS_COMPSTR);
                    if (text) {
                        ui::Preedit preedit;
                        preedit.text = std::move(*text);
                        const auto cursor = ImmGetCompositionStringW(input.context, GCS_CURSORPOS, nullptr, 0);
                        preedit.cursor = cursor < 0 ? 0 : static_cast<std::uint32_t>(cursor);
                        const auto bytes = ImmGetCompositionStringW(input.context, GCS_COMPATTR, nullptr, 0);
                        if (bytes > 0 && static_cast<std::size_t>(bytes) <= preedit.text.size()) {
                            preedit.attributes.resize(static_cast<std::size_t>(bytes));
                            if (ImmGetCompositionStringW(input.context, GCS_COMPATTR, preedit.attributes.data(), static_cast<DWORD>(bytes)) != bytes)
                                record.error = ERROR_INVALID_DATA;
                        }
                        value.preedit = std::move(preedit);
                    } else record.error = text.error();
                }
            }
            if (record.error) { value.kind = ui::CompositionKind::cancel; value.result.reset(); value.preedit.reset(); }
        }
    }
    if (record.composition) events_.push_back(std::move(record));
}
ui::ClipboardResult Window::clipboard(const ui::ClipboardRequest& request) {
    ui::ClipboardResult result{request.kind, request.operation_id, request.token, false, 0, {}};
    if (!OpenClipboard(handle_)) { result.native_error = native_error(); return result; }
    ClipboardScope close;
    const auto limit=std::min<std::size_t>(request.limit,1024*1024);
    if (request.kind == ui::ClipboardKind::read) {
        const auto memory = GetClipboardData(CF_UNICODETEXT);
        if (!memory) { result.native_error = native_error(); return result; }
        const auto bytes = GlobalSize(memory);
        MemoryLock lock(memory);
        const auto* text = static_cast<const wchar_t*>(lock.data);
        if (!text) { result.native_error = native_error(); return result; }
        const auto units = std::min<std::size_t>(bytes / sizeof(wchar_t), limit + 1);
        const auto end = std::find(text, text + units, L'\0');
        if (end == text + units || static_cast<std::size_t>(end - text) > limit) result.native_error = ERROR_BUFFER_OVERFLOW;
        else { result.text.assign(text, end); result.success = true; }
    } else {
        if (request.text.size() > limit) { result.native_error = ERROR_BUFFER_OVERFLOW; return result; }
        const auto bytes = (request.text.size() + 1) * sizeof(wchar_t);
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) { result.native_error = native_error(); return result; }
        auto* text = GlobalLock(memory);
        if (!text) { result.native_error = native_error(); GlobalFree(memory); return result; }
        std::memcpy(text, request.text.c_str(), bytes); GlobalUnlock(memory);
        if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
            result.native_error = native_error(); GlobalFree(memory); return result;
        }
        result.success = true; // The system now owns memory.
    }
    return result;
}
std::uint32_t Window::cancel_ime() {
    input_anchor_.reset();
    InputContext input(handle_);
    if (!input.context) return ERROR_INVALID_HANDLE;
    // A reading/candidate UI can exist before WM_IME_STARTCOMPOSITION. Its
    // lifetime still belongs to the input binding, even without a UI composition.
    if (composition_id_ || native_composition_id_ || ImmGetOpenStatus(input.context)) {
        if (!ImmNotifyIME(input.context, NI_COMPOSITIONSTR, CPS_CANCEL, 0)) return native_error();
        ++input_statistics_.cancellations;
    }
    composition_id_ = native_composition_id_ = 0;
    candidate_rect_.reset();
    return 0;
}
ui::ImeResult Window::ime(const ui::ImeRequest& request) {
    ui::ImeResult result{request.kind, request.composition_id, request.token, false, 0};
    if (!composition_id_ || composition_id_ != request.composition_id || composition_token_ != request.token) {
        result.native_error = ERROR_INVALID_STATE; return result;
    }
    if (native_composition_id_ != request.composition_id) {
        result.success = true; // Native end is already queued; never act on a newer native stream.
        if (request.kind == ui::ImeKind::cancel) composition_id_ = 0;
        return result;
    }
    if (request.kind == ui::ImeKind::cancel) {
        result.native_error = cancel_ime();
        result.success = !result.native_error;
        return result;
    }
    InputContext input(handle_);
    if (!input.context || !ImmNotifyIME(input.context, NI_COMPOSITIONSTR, CPS_COMPLETE, 0)) {
        result.native_error = native_error(); return result;
    }
    result.success = true;
    return result;
}
std::uint32_t Window::text_input(const std::optional<ui::TextInputState>& input) {
    const auto token = input ? std::optional{input->token} : std::nullopt;
    const bool binding_changed = input_token_ && (!token || token->key != input_token_->key ||
        token->incarnation != input_token_->incarnation || token->generation != input_token_->generation);
    // Clear geometry and cancel against the old token before exposing the new
    // binding. Synchronous IMM callbacks snapshot that old token into the queue.
    input_anchor_ = binding_changed ? std::nullopt : (input ? input->accepted_anchor : std::nullopt);
    if (binding_changed && input_enabled_) {
        if (const auto error = cancel_ime()) return error;
    }
    if (input_token_ != token) high_surrogate_ = 0;
    if (composition_id_ && (!input || !input->composing || input->token.key != composition_token_.key ||
        input->token.incarnation != composition_token_.incarnation || input->token.generation != composition_token_.generation)) {
        const auto cancelled = ime({ui::ImeKind::cancel, composition_id_, composition_token_});
        composition_id_ = 0;
        if (!cancelled.success && cancelled.native_error != ERROR_INVALID_STATE) return cancelled.native_error;
    }
    if (composition_id_ && input && input->composing) composition_token_ = input->token;
    if (native_composition_id_ && !composition_id_ && (!input || !input->composing)) {
        if (const auto error = cancel_ime()) return error;
    }
    if (input_enabled_ && (binding_changed || !input)) {
        // Logical fields share one HWND. End the old native association before
        // publishing a new field, including reading that has not sent a start.
        if (!ImmAssociateContextEx(handle_, nullptr, 0)) return native_error();
        input_enabled_ = false;
        composition_id_ = native_composition_id_ = 0;
        candidate_rect_.reset();
    }
    input_token_ = token;
    input_anchor_ = input ? input->accepted_anchor : std::nullopt;
    if (input && !input_enabled_) {
        if (!ImmAssociateContextEx(handle_, nullptr, IACE_DEFAULT)) {
            input_anchor_.reset();
            return native_error();
        }
        input_enabled_ = true;
        candidate_rect_.reset();
    }
    if (input && input->accepted_caret) {
        const auto& caret = *input->accepted_caret;
        InputContext context(handle_);
        if (!context.context) return ERROR_INVALID_HANDLE;
        CANDIDATEFORM candidate{};
        candidate.dwStyle = CFS_EXCLUDE;
        candidate.ptCurrentPos = {static_cast<LONG>(std::floor(caret.bounds.x)), static_cast<LONG>(std::ceil(caret.bounds.y + caret.bounds.height))};
        candidate.rcArea = {static_cast<LONG>(std::floor(caret.bounds.x)), static_cast<LONG>(std::floor(caret.bounds.y)),
            static_cast<LONG>(std::ceil(caret.bounds.x + caret.bounds.width)), static_cast<LONG>(std::ceil(caret.bounds.y + caret.bounds.height))};
        const auto& area = candidate.rcArea;
        if (!candidate_rect_ || candidate_rect_->left != area.left || candidate_rect_->top != area.top ||
            candidate_rect_->right != area.right || candidate_rect_->bottom != area.bottom) {
            if (!ImmSetCandidateWindow(context.context, &candidate)) return native_error();
            candidate_rect_ = area;
        }
    }
    return 0;
}
}
