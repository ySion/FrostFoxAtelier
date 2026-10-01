#include "application/popup.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atelier::application {
namespace {
template<class T> T take(ui::Result<T> result) {
    if (!result) throw std::runtime_error("Popup UI failure (code "+std::to_string(static_cast<int>(result.error().code))+
        ", key "+std::to_string(result.error().key)+", native "+std::to_string(result.error().native_code)+").");
    return std::move(*result);
}
void take(ui::Result<void> result) {
    if (!result) throw std::runtime_error("Popup UI rejected its style.");
}
template<class T> T graphics(renderer::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().cause.message);
    return std::move(*result);
}
}
PopupHost::PopupHost(HWND owner,std::span<const std::wstring> fonts,bool validation) : window_(owner),ui_(fonts) {
    renderer::Options options;
    options.validation=validation?rhi::Validation::gpu:rhi::Validation::disabled;
    options.vsync=false; // The main surface owns frame pacing.
    renderer_=graphics(renderer::Renderer::open(window_.handle(),options));
}
void PopupHost::update(ui::ControlKey source,RECT anchor,bool below,std::span<const ui::MenuItem> items,
                       const ui::Style& style,std::shared_ptr<const ui::StyleSheet> sheet) {
    if (visible_ && source_==source && std::ranges::equal(items,items_) && style==ui_.style() && sheet_==sheet) return;
    take(ui_.set_style(style)); take(ui_.set_style_sheet(sheet)); sheet_=std::move(sheet);
    source_=source; items_.assign(items.begin(),items.end());
    // Position before measurement so GetDpiForWindow uses the anchor monitor.
    if (!SetWindowPos(window(),nullptr,anchor.left,anchor.bottom,32,32,SWP_NOACTIVATE|SWP_NOZORDER))
        throw std::runtime_error("Cannot position popup window.");
    MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&monitor))
        throw std::runtime_error("Cannot query popup monitor.");
    const auto area=monitor.rcWork;
    const auto dpi=window_.dpi_scale();
    take(ui_.set_controls({}));
    take(ui_.set_surface({{static_cast<std::uint32_t>(area.right-area.left),static_cast<std::uint32_t>(area.bottom-area.top)},dpi,++revision_}));
    auto display=items_;
    // Edit commands execute in the origin Session after validating its lifetime.
    for (auto& item:display) item.edit_command.reset();
    std::vector<ui::Control> controls;
    ui::append_menu(controls,500,0,0,0,280,display);
    take(ui_.set_controls(controls));
    const auto measured=take(ui_.prepare())->find(500)->bounds;
    const auto width=std::min(static_cast<LONG>(std::ceil(measured.width)),area.right-area.left);
    const auto height=std::min(static_cast<LONG>(std::ceil(measured.height)),area.bottom-area.top);
    auto x=anchor.left, y=anchor.bottom;
    if (y+height>area.bottom) y=below?anchor.top-height:area.bottom-height;
    x=std::clamp(x,area.left,area.right-width); y=std::clamp(y,area.top,area.bottom-height);
    if (!SetWindowPos(window(),HWND_TOP,x,y,width,height,SWP_NOACTIVATE)) throw std::runtime_error("Cannot size popup window.");
    take(ui_.set_controls({}));
    take(ui_.set_surface({{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)},window_.dpi_scale(),++revision_}));
    take(ui_.set_controls(controls));
    controls_=std::move(controls); extent_={static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)};
    window_.show(); visible_=dirty_=true;
}
void PopupHost::render(renderer::Capture* capture) {
    if (!visible_) return;
    window_.poll([&](NativeEvent&& event) {
        if (event.message==WM_SIZE || event.message==WM_DPICHANGED) dirty_=true;
    });
    if (!dirty_ && !capture) return;
    // The renderer publishes a resize after Present. Drive that same protocol,
    // accepting input only once the back buffer matches the native client.
    for (unsigned attempt=0;attempt<2;++attempt) {
        const auto target=graphics(renderer_.target());
        if (!target.drawable) return;
        if (target.extent!=extent_) {
            take(ui_.set_controls({}));
            take(ui_.set_surface({target.extent,window_.dpi_scale(),++revision_}));
            take(ui_.set_controls(controls_)); extent_=target.extent;
        }
        const auto candidate=take(ui_.prepare());
        RECT client{}; GetClientRect(window(),&client);
        const bool current=target.extent==ui::PixelExtent{static_cast<std::uint32_t>(client.right),static_cast<std::uint32_t>(client.bottom)};
        const auto result=graphics(renderer_.render({*candidate->drawing,{}, {},{}},current?capture:nullptr));
        if (current && result.status==renderer::FrameStatus::accepted) {
            if (!ui_.accept(candidate)) throw std::runtime_error("Popup rejected its accepted GPU frame.");
            dirty_=false; return;
        }
    }
}
ui::Reply PopupHost::input(ui::Event event) {
    if (!visible_) return {};
    RECT bounds{}; GetClientRect(window(),&bounds);
    const auto& accepted=ui_.presented();
    if (accepted && (accepted->drawing->extent!=ui::PixelExtent{static_cast<std::uint32_t>(bounds.right),static_cast<std::uint32_t>(bounds.bottom)} ||
        accepted->drawing->dpi_scale!=window_.dpi_scale())) return ui_.cancel_interaction();
    const auto reply=ui_.input(event); dirty_|=reply.redraw;
    if (reply.cursor) window_.cursor(*reply.cursor);
    return reply;
}
void PopupHost::hide() {
    if (!visible_) return;
    ShowWindow(window(),SW_HIDE); visible_=false; source_=0;
    // Key-up after dismissal belongs to the owner. Retire the popup's terminal
    // key latch now, so the next opening does not swallow its first Escape.
    ui::Event loss; loss.kind=ui::EventKind::focus_lost;
    static_cast<void>(ui_.input(loss)); take(ui_.set_controls({})); items_.clear();
}
void PopupHost::close() {
    if (closed_) return;
    hide(); const auto result=renderer_.close(); closed_=true;
    if (!result) throw std::runtime_error(result.error().cause.message);
}
}
