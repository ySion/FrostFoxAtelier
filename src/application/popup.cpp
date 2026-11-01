#include "application/popup.hpp"
#include "foundation/frame_timing.hpp"
#include "logging/native.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace atelier::application {
namespace {
template <class T>
T take(ui::Result<T> result) {
    if (!result)
        throw std::runtime_error("Popup UI failure (code " + std::to_string(static_cast<int>(result.error().code)) +
                                 ", key " + std::to_string(result.error().key) + ", native " +
                                 std::to_string(result.error().native_code) + ").");
    return std::move(*result);
}
void take(ui::Result<void> result) {
    if (!result)
        throw std::runtime_error("Popup UI rejected its style.");
}
template <class T>
T graphics(renderer::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().cause.message);
    return std::move(*result);
}
}
PopupHost::PopupHost(HWND owner, std::span<const std::wstring> fonts, bool validation)
    : window_(owner), ui_(fonts), measurement_(fonts) {
    renderer::Options options;
    options.validation = validation ? rhi::Validation::gpu : rhi::Validation::disabled;
    options.vsync = false; // The main surface owns frame pacing.
    renderer_ = graphics(renderer::Renderer::open(window_.handle(), options));
}
void PopupHost::update(ui::ControlKey source, RECT anchor, bool below, std::span<const ui::MenuItem> items,
                       const ui::Style& style, std::shared_ptr<const ui::StyleSheet> sheet,
                       std::shared_ptr<ui_language::Component> component, bool center_first_item) {
    const auto reject = [&](std::string message) {
        diagnostic_ = std::move(message);
        if (instance_)
            instance_->reject_controls();
        hide();
        measured_dpi_ = 0; // A refused projection must never reuse the old geometry for new items.
    };
    if (!component) {
        reject("PopupMenu is missing from the accepted UI authoring module.");
        return;
    }
    const bool continuing = visible_ && source_ == source;
    const auto previous_focus = continuing ? ui_.focused_control() : 0;
    const auto monitor_handle = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(monitor_handle, &monitor))
        throw std::runtime_error("Cannot query popup monitor.");
    const auto area = monitor.rcWork;
    const ui::PixelExtent work_area{static_cast<std::uint32_t>(area.right - area.left),
                                    static_cast<std::uint32_t>(area.bottom - area.top)};
    // Cross monitors before measuring DPI; an in-place toggle must not first move
    // the visible menu to its anchor and then back to its fitted position.
    if (MonitorFromWindow(window(), MONITOR_DEFAULTTONEAREST) != monitor_handle &&
        !SetWindowPos(window(), nullptr, anchor.left, anchor.bottom, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER))
        throw std::runtime_error("Cannot position popup window.");
    const auto dpi = window_.dpi_scale();
    const bool measure = controls_.empty() || !std::ranges::equal(items, items_) || style != ui_.style() ||
                         sheet_ != sheet || measured_dpi_ != dpi || measured_area_ != work_area ||
                         component_ != component;
    source_ = source;
    LONG width = static_cast<LONG>(extent_.width);
    LONG height = static_cast<LONG>(extent_.height);
    if (measure) {
        auto rows = std::make_shared<std::vector<ui_language::Item>>();
        rows->reserve(items.size());
        std::erase_if(row_keys_, [&](const auto& entry) {
            return std::ranges::none_of(items, [&](const auto& item) { return item.key == entry.first; });
        });
        for (const auto& item : items) {
            auto [at, inserted] = row_keys_.try_emplace(item.key, 0);
            if (inserted)
                at->second = next_key_++;
            const bool branch = !item.children.empty();
            rows->push_back(
                {at->second,
                 {item.text, branch ? std::wstring{} : item.shortcut, item.enabled, item.selected, branch,
                  !rows->empty() && item.separator_before,
                  std::wstring(item.behavior == ui::MenuBehavior::radio      ? L"radio"
                               : item.behavior == ui::MenuBehavior::checkbox ? L"checkbox"
                                                                             : L"command"),
                  item.radio_group, double(item.icon), double(branch ? ui::ControlIcon::right : ui::ControlIcon::none),
                  std::wstring(item.enabled ? L"menu-shortcut" : L"menu-shortcut unavailable"), item.heading,
                  !item.heading.empty()},
                 {{"item", item.key}}});
        }
        const std::array<ui_language::InputValue, 1> inputs{ui_language::ItemList{std::move(rows)}};
        if (!instance_) {
            const std::array<ui_language::KeyBinding, 1> keys{{{"menu", 500}}};
            auto instance = ui_language::Instance::create(component, 0, next_key_, keys, inputs);
            if (!instance) {
                reject(instance.error().message);
                return;
            }
            instance_ = std::move(*instance);
        } else if (auto update = instance_->update(inputs, component); !update) {
            reject(update.error().message);
            return;
        }
        std::vector<ui::Control> controls(instance_->controls().begin(), instance_->controls().end());
        if (controls.empty() || controls.front().key != 500 || controls.front().kind != ui::ControlKind::popup ||
            controls.front().parent) {
            reject("PopupMenu requires its named root to be a standalone Popup.");
            return;
        }
        // Intrinsic measurement has its own retained CPU session. A checked/enabled
        // update must not resize the live input surface to the monitor and back.
        take(measurement_.set_style(style));
        take(measurement_.set_style_sheet(sheet));
        if (!measurement_revision_ || measured_dpi_ != dpi || measured_area_ != work_area) {
            // Surface changes dismiss modal input. Retire the measurement-only tree
            // first so the new projection opens its root in the new DPI/work area.
            // The live popup Session and its accepted input identities stay intact.
            take(measurement_.set_controls({}));
            ++measurement_revision_;
        }
        take(measurement_.set_surface({work_area, dpi, measurement_revision_}));
        // Measure both text columns with the same SS/font/layout path used to draw.
        for (auto& control : controls)
            if (control.kind == ui::ControlKind::menu_item)
                control.layout.width = ui_layout::Axis::content();
        if (auto accepted = measurement_.set_controls(controls); !accepted) {
            reject("PopupMenu controls were rejected at key " + std::to_string(accepted.error().key));
            return;
        }
        const auto prepared = measurement_.prepare();
        if (!prepared) {
            reject("PopupMenu layout failed at key " + std::to_string(prepared.error().key));
            return;
        }
        const auto& measurement = *prepared;
        const auto* menu = measurement->find(500);
        if (!menu) {
            reject("PopupMenu requires a visible menu root.");
            return;
        }
        float title_width{};
        float shortcut_width{};
        for (const auto& control : controls) {
            const auto* box = measurement->find(control.key);
            if (!box)
                continue;
            if (control.kind == ui::ControlKind::menu_item)
                title_width = std::max(title_width, box->bounds.width / dpi);
            if (control.kind == ui::ControlKind::label)
                shortcut_width = std::max(shortcut_width, box->bounds.width / dpi);
        }
        const auto gap = shortcut_width > 0 ? 18.f : 0.f;
        const bool scrolling = !measurement->scrollbars.empty();
        const auto gutter = scrolling ? menu->style->scrollbar_width + 2 * menu->style->scrollbar_inset : 0.f;
        const auto padding = controls.front().layout.padding;
        const auto wanted = std::max(160.f, title_width + shortcut_width + gap + padding.left + padding.right + gutter);
        width = std::min(static_cast<LONG>(std::ceil(wanted * dpi)), area.right - area.left);
        height = std::min(static_cast<LONG>(std::ceil(menu->bounds.height)), area.bottom - area.top);
        controls.front().layout.width = ui_layout::Axis::fixed(static_cast<float>(width) / dpi);
        // Pin the root to the rounded window height too, so a fractional content height leaves no unpainted bottom row.
        controls.front().layout.height = ui_layout::Axis::fixed(static_cast<float>(height) / dpi);
        controls.front().style.scrollbar_gutter =
            scrolling ? ui::ScrollbarGutter::stable : ui::ScrollbarGutter::overlay;
        for (auto& control : controls)
            if (control.kind == ui::ControlKind::menu_item) {
                control.layout.width = ui_layout::Axis::expand();
                control.layout.padding.right = shortcut_width + gap;
            }
        items_.assign(items.begin(), items.end());
        controls_ = std::move(controls);
        measured_dpi_ = dpi;
        measured_area_ = work_area;
        take(ui_.set_style(style));
        take(ui_.set_style_sheet(sheet));
        sheet_ = std::move(sheet);
        instance_->commit_controls();
        component_ = std::move(component);
        diagnostic_.clear();
    }
    auto x = below ? anchor.left : anchor.right;
    auto y = below ? anchor.bottom : anchor.top;
    // Single-action confirmation opens with its action under the pointer.
    if (center_first_item) {
        x = anchor.left - std::min(width / 2, static_cast<LONG>(48 * dpi));
        y = anchor.top - height / 2;
    }
    if (!below && x + width > area.right)
        x = anchor.left - width;
    if (y + height > area.bottom)
        y = below ? anchor.top - height : area.bottom - height;
    x = std::clamp(x, area.left, area.right - width);
    y = std::clamp(y, area.top, area.bottom - height);
    RECT previous_bounds{};
    const bool place = !continuing || !GetWindowRect(window(), &previous_bounds) || previous_bounds.left != x ||
                       previous_bounds.top != y || previous_bounds.right - previous_bounds.left != width ||
                       previous_bounds.bottom - previous_bounds.top != height;
    const ui::PixelExtent extent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    const bool resize_surface = !continuing || extent != extent_ || dpi != surface_dpi_;
    // A scrolling parent can move this anchor without changing menu content.
    // Reuse its measured extent, but still resolve placement before returning.
    if (!measure && !place && !resize_surface)
        return;
    if (place && !SetWindowPos(window(), HWND_TOP, x, y, width, height, SWP_NOACTIVATE))
        throw std::runtime_error("Cannot size popup window.");
    // Match the client before showing a newly opened popup; never paint its old-sized canvas.
    if (place)
        static_cast<void>(graphics(renderer_.sync_window_target()));
    if (resize_surface) {
        retarget(extent);
        restore_focus_ = previous_focus;
    } else if (measure)
        take(ui_.set_controls(controls_));
    if (!continuing) {
        window_.show();
        opening_ = true;
        open_present_count_ = 0;
    }
    visible_ = true;
    dirty_ |= measure || resize_surface;
}
void PopupHost::suspend() {
    if (controls_.empty())
        return;
    controls_.front().layout.collapsed = true;
    take(ui_.set_controls(controls_));
}
void PopupHost::retarget(ui::PixelExtent extent) {
    // Retire modal input while retaining text/layout caches and stable identities.
    suspend();
    take(ui_.set_surface({extent, window_.dpi_scale(), ++revision_}));
    controls_.front().layout.collapsed = false;
    take(ui_.set_controls(controls_));
    extent_ = extent;
    surface_dpi_ = window_.dpi_scale();
}
void PopupHost::render(renderer::Capture* capture) {
    if (!visible_)
        return;
    window_.poll([&](NativeEvent&& event) {
        if (event.message == WM_SIZE || event.message == WM_DPICHANGED)
            dirty_ = true;
    });
    if (!dirty_ && !capture)
        return;
    // The popup surface is part of whichever host phase opened or updated it.
    timing::Suspend whole;
    const auto target = graphics(renderer_.sync_window_target());
    if (!target.drawable)
        return;
    if (target.extent != extent_)
        retarget(target.extent);
    const auto candidate = take(ui_.prepare());
    const auto result = graphics(renderer_.render({*candidate->drawing, {}, {}, {}}, capture));
    if (opening_)
        ++open_present_count_;
    if (result.status == renderer::FrameStatus::accepted) {
        if (!ui_.accept(candidate))
            throw std::runtime_error("Popup rejected its accepted GPU frame.");
        if (opening_) {
            const std::array fields{logging::Field{"source_control", source_},
                                    logging::Field{"surface_revision", revision_},
                                    logging::Field{"frame", result.accepted_id},
                                    logging::Field{"present_attempts", std::uint64_t{open_present_count_}}};
            static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "menu", "menu.frame_accepted",
                                               logging::native::Context{fields}, "Menu opening frame accepted"));
        }
        opening_ = dirty_ = false;
        if (restore_focus_) {
            dirty_ |= ui_.focus(restore_focus_).redraw;
            restore_focus_ = 0;
        }
    }
}
ui::Reply PopupHost::input(ui::Event event) {
    if (!visible_)
        return {};
    RECT bounds{};
    GetClientRect(window(), &bounds);
    const auto& accepted = ui_.presented();
    if (accepted && (accepted->drawing->extent != ui::PixelExtent{static_cast<std::uint32_t>(bounds.right),
                                                                  static_cast<std::uint32_t>(bounds.bottom)} ||
                     accepted->drawing->dpi_scale != window_.dpi_scale()))
        return ui_.cancel_interaction();
    const auto reply = ui_.input(event);
    dirty_ |= reply.redraw;
    if (reply.cursor)
        window_.cursor(*reply.cursor);
    return reply;
}
void PopupHost::hide() {
    if (!visible_)
        return;
    ShowWindow(window(), SW_HIDE);
    visible_ = false;
    source_ = restore_focus_ = 0;
    // Key-up after dismissal belongs to the owner. Retire the popup's terminal
    // key latch now, so the next opening does not swallow its first Escape.
    ui::Event loss;
    loss.kind = ui::EventKind::focus_lost;
    static_cast<void>(ui_.input(loss));
    suspend();
}
void PopupHost::reset_document() {
    hide();
    take(ui_.set_controls({}));
    take(measurement_.set_controls({}));
    controls_.clear();
    items_.clear();
    row_keys_.clear();
    instance_.reset();
    component_.reset();
    diagnostic_.clear();
}
void PopupHost::close() {
    if (closed_)
        return;
    reset_document();
    const auto result = renderer_.close();
    closed_ = true;
    if (!result)
        throw std::runtime_error(result.error().cause.message);
}
}
