#include "application/window.hpp"
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
                    composition_id_ = composition.composition_id;
                    composition_token_ = input_token_.value_or(ui::EditToken{});
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
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { closing_ = true; continue; }
        if (message.hwnd == handle_ && message.message == WM_KEYDOWN && native_composition_id_) {
            const auto code = message.wParam == VK_PROCESSKEY ? ImmGetVirtualKey(handle_) : message.wParam;
            if (code < ime_keys_.size()) ime_keys_[code] = true;
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
        events_.push_back(event(message, wparam)); // LPARAM points to stack storage
        return 0;
    }
    case WM_IME_SETCONTEXT:
        return DefWindowProcW(handle_, message, wparam, lparam & ~ISC_SHOWUICOMPOSITIONWINDOW);
    case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION: case WM_IME_ENDCOMPOSITION:
        composition(message, lparam); return 0;
    case WM_IME_CHAR: return 0; // Confirmed strings have one owner: GCS_RESULTSTR.
    case WM_IME_NOTIFY:
        if (wparam == IMN_OPENCANDIDATE) ++input_statistics_.candidate_opens;
        return DefWindowProcW(handle_, message, wparam, lparam);
    case WM_INPUTLANGCHANGE:
        candidate_rect_.reset();
        return DefWindowProcW(handle_, message, wparam, lparam);
    case WM_CHAR: {
        auto value = event(message, wparam, lparam);
        const auto character = static_cast<wchar_t>(wparam);
        if (character < 0x20 || character == 0x7f) return 0; // Commands arrive as key events.
        value.text.push_back(character); // Assemble surrogate pairs in ordered pump delivery.
        events_.push_back(std::move(value)); return 0;
    }
    case WM_KEYDOWN: case WM_KEYUP: {
        auto value = event(message, wparam, lparam);
        const auto code = wparam == VK_PROCESSKEY ? ImmGetVirtualKey(handle_) : wparam;
        if (code < ime_keys_.size()) {
            value.ime_owned = native_composition_id_ || ime_keys_[code];
            if (message == WM_KEYDOWN && value.ime_owned) ime_keys_[code] = true;
            if (message == WM_KEYUP) ime_keys_[code] = false;
        }
        if (value.ime_owned) return DefWindowProcW(handle_, message, wparam, lparam);
        events_.push_back(std::move(value)); return 0;
    }
    case WM_KILLFOCUS:
        high_surrogate_ = 0; ime_keys_.fill(false);
        events_.push_back(event(message, wparam, lparam)); return 0;
    case WM_CAPTURECHANGED:
        if (releasing_capture_) return 0;
        [[fallthrough]];
    case WM_SIZE: case WM_SETFOCUS:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        events_.push_back(event(message, wparam, lparam));
        return 0;
    default: return DefWindowProcW(handle_, message, wparam, lparam);
    }
}
void Window::composition(UINT message, LPARAM flags) {
    auto record = event(message, 0, flags);
    if (message == WM_IME_STARTCOMPOSITION) {
        candidate_rect_.reset();
        native_composition_id_ = next_composition_++;
        record.composition = ui::CompositionEvent{ui::CompositionKind::start, native_composition_id_, {}, {}, {}};
        ++input_statistics_.compositions;
    } else if (native_composition_id_) {
        auto& value = record.composition.emplace();
        value.composition_id = native_composition_id_;
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
    if (request.kind == ui::ClipboardKind::read) {
        const auto memory = GetClipboardData(CF_UNICODETEXT);
        if (!memory) { result.native_error = native_error(); return result; }
        const auto bytes = GlobalSize(memory);
        MemoryLock lock(memory);
        const auto* text = static_cast<const wchar_t*>(lock.data);
        if (!text) { result.native_error = native_error(); return result; }
        const auto units = std::min<std::size_t>(bytes / sizeof(wchar_t), text_limit + 1);
        const auto end = std::find(text, text + units, L'\0');
        if (end == text + units || static_cast<std::size_t>(end - text) > text_limit) result.native_error = ERROR_BUFFER_OVERFLOW;
        else { result.text.assign(text, end); result.success = true; }
    } else {
        if (request.text.size() > text_limit) { result.native_error = ERROR_BUFFER_OVERFLOW; return result; }
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
    InputContext input(handle_);
    if (!input.context || !ImmNotifyIME(input.context, NI_COMPOSITIONSTR,
        request.kind == ui::ImeKind::complete ? CPS_COMPLETE : CPS_CANCEL, 0)) {
        result.native_error = native_error(); return result;
    }
    result.success = true;
    if (request.kind == ui::ImeKind::cancel) {
        composition_id_ = native_composition_id_ = 0; ++input_statistics_.cancellations;
    }
    return result;
}
std::uint32_t Window::text_input(const std::optional<ui::TextInputState>& input) {
    const auto token = input ? std::optional{input->token} : std::nullopt;
    if (input_token_ != token) high_surrogate_ = 0;
    if (composition_id_ && (!input || !input->composing || input->token.key != composition_token_.key ||
        input->token.incarnation != composition_token_.incarnation || input->token.generation != composition_token_.generation)) {
        const auto cancelled = ime({ui::ImeKind::cancel, composition_id_, composition_token_});
        composition_id_ = 0;
        if (!cancelled.success && cancelled.native_error != ERROR_INVALID_STATE) return cancelled.native_error;
    }
    if (composition_id_ && input && input->composing) composition_token_ = input->token;
    if (native_composition_id_ && !composition_id_ && (!input || !input->composing)) {
        InputContext context(handle_);
        if (!context.context || !ImmNotifyIME(context.context, NI_COMPOSITIONSTR, CPS_CANCEL, 0)) return native_error();
        native_composition_id_ = 0;
    }
    input_token_ = token;
    if (input_enabled_ != input.has_value()) {
        if (!ImmAssociateContextEx(handle_, nullptr, input ? IACE_DEFAULT : 0)) return native_error();
        input_enabled_ = input.has_value();
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
