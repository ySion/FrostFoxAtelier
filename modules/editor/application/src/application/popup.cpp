#include "application/popup.hpp"
#include "application/diagnostics.hpp"
#include "foundation/frame_timing.hpp"
#include "logging/native.hpp"
#include "memory/fatal.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace atelier::application {
namespace {
Error popup_error(const ui::Error& error) {
    return {ErrorCode::invalid_definition,
            String::concat("Popup UI failure (code ", static_cast<int>(error.code), ", key ", error.key, ", native ",
                           error.native_code, "). ", String::validated(diagnostic_utf8(error.message)))};
}

Result<void> checked(ui::Result<ui::Reply> result) {
    if (!result)
        return std::unexpected(popup_error(result.error()));
    if (result->error)
        return std::unexpected(popup_error(*result->error));
    return {};
}

Result<void> checked(ui::Result<void> result) {
    if (!result) {
        auto error = ui_error(result.error());
        error.message = String::concat("Popup UI rejected its style. ", error.message);
        return std::unexpected(std::move(error));
    }
    return {};
}

void retain_failure(Result<void>& first, Result<void> next) {
    if (next)
        return;
    if (first)
        first = std::move(next);
    else
        static_cast<void>(logging::submit({.severity = logging::Severity::error,
                                           .category = "menu"_s,
                                           .event_code = "menu.cleanup_failed"_s,
                                           .message = std::move(next.error().message)}));
}

auto window_failure(const char* message) {
    const auto code = GetLastError();
    return std::unexpected(
        Error{ErrorCode::invalid_storage, String::concat(String::validated(message), " (Win32 ", code, ").")});
}
}

Result<void> PopupHost::initialize(HWND owner, Span<const std::wstring> fonts, bool validation) {
    ATELIER_TRY(window_.initialize(owner));
    ATELIER_TRY(ui_.initialize(fonts).transform_error(ui_error));
    ATELIER_TRY(measurement_.initialize(fonts).transform_error(ui_error));
    renderer::Options options;
    options.validation = validation ? rhi::Validation::gpu : rhi::Validation::disabled;
    options.vsync = false; // The main surface owns frame pacing.
    ATELIER_TRY_BIND(opened, renderer::Renderer::open(window_.handle(), options).transform_error(renderer_error));
    renderer_ = std::move(*opened);
    return {};
}

Result<void> PopupHost::update(ui::ControlKey source, RECT anchor, bool below, Span<const ui::MenuItem> items,
                               const ui::Style& style, std::shared_ptr<const ui::StyleSheet> sheet,
                               std::shared_ptr<ui_language::Component> component, bool center_first_item) {
    const auto reject = [&](Utf8Text message) {
        diagnostic_ = std::move(message).into_string();
        if (instance_)
            instance_->reject_controls();
        auto hidden = hide();
        measured_dpi_ = 0; // A refused projection must never reuse the old geometry for new items.
        return hidden;
    };
    if (!component) {
        return reject("PopupMenu is missing from the accepted UI authoring module.");
    }
    const bool continuing = visible_ && source_ == source;
    const auto previous_focus = continuing ? ui_.focused_control() : 0;
    const auto monitor_handle = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(monitor_handle, &monitor))
        return window_failure("Cannot query popup monitor.");
    const auto area = monitor.rcWork;
    const ui::PixelExtent work_area{static_cast<std::uint32_t>(area.right - area.left),
                                    static_cast<std::uint32_t>(area.bottom - area.top)};
    // Cross monitors before measuring DPI; an in-place toggle must not first move
    // the visible menu to its anchor and then back to its fitted position.
    if (MonitorFromWindow(window(), MONITOR_DEFAULTTONEAREST) != monitor_handle &&
        !SetWindowPos(window(), nullptr, anchor.left, anchor.bottom, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER))
        return window_failure("Cannot position popup window.");
    const auto dpi = window_.dpi_scale();
    const bool measure = controls_.empty() || !std::ranges::equal(items, items_) || style != ui_.style() ||
                         sheet_ != sheet || measured_dpi_ != dpi || measured_area_ != work_area ||
                         component_ != component;
    source_ = source;
    LONG width = static_cast<LONG>(extent_.width);
    LONG height = static_cast<LONG>(extent_.height);
    if (measure) {
        auto rows = std::make_shared<Array<ui_language::Item>>();
        rows->reserve(items.size());
        row_keys_.remove_if([&](ui::ControlKey key, ui::ControlKey) {
            return std::ranges::none_of(items, [&](const auto& item) { return item.key == key; });
        });
        for (const auto& item : items) {
            auto [at, inserted] = row_keys_.try_emplace(item.key, 0);
            if (inserted)
                *at = next_key_++;
            const bool branch = !item.children.empty();
            rows->add(
                {*at,
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
                return reject(instance.error().message);
            }
            instance_ = std::move(*instance);
        } else if (auto update = instance_->update(inputs, component); !update) {
            return reject(update.error().message);
        }
        Array<ui::Control> controls(instance_->controls());
        if (controls.empty() || controls.front().key != 500 || controls.front().kind != ui::ControlKind::popup ||
            controls.front().parent) {
            return reject("PopupMenu requires its named root to be a standalone Popup.");
        }
        // Intrinsic measurement has its own retained CPU session. A checked/enabled
        // update must not resize the live input surface to the monitor and back.
        ATELIER_TRY(checked(measurement_.set_style(style)));
        ATELIER_TRY(checked(measurement_.set_style_sheet(sheet)));
        if (!measurement_revision_ || measured_dpi_ != dpi || measured_area_ != work_area) {
            // Surface changes dismiss modal input. Retire the measurement-only tree
            // first so the new projection opens its root in the new DPI/work area.
            // The live popup Session and its accepted input identities stay intact.
            ATELIER_TRY(checked(measurement_.set_controls({})));
            ++measurement_revision_;
        }
        ATELIER_TRY(checked(measurement_.set_surface({work_area, dpi, measurement_revision_})));
        // Measure both text columns with the same SS/font/layout path used to draw.
        for (auto& control : controls)
            if (control.kind == ui::ControlKind::menu_item)
                control.layout.width = ui_layout::Axis::content();
        auto accepted = measurement_.set_controls(controls);
        if (!accepted || accepted->error) {
            const auto& cause = accepted ? *accepted->error : accepted.error();
            auto error = ui_error(cause);
            error.message =
                String::concat("PopupMenu controls were rejected at key ", cause.key, ": ", error.message);
            Result<void> failed = std::unexpected(std::move(error));
            retain_failure(failed, reject(failed.error().message));
            return failed;
        }
        const auto prepared = measurement_.prepare(ui::Preparation::geometry);
        if (!prepared) {
            auto error = ui_error(prepared.error());
            error.message =
                String::concat("PopupMenu layout failed at key ", prepared.error().key, ": ", error.message);
            Result<void> failed = std::unexpected(std::move(error));
            retain_failure(failed, reject(failed.error().message));
            return failed;
        }
        const auto& measurement = *prepared;
        const auto* menu = measurement->find(500);
        if (!menu) {
            return reject("PopupMenu requires a visible menu root.");
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
        items_.assign(items);
        controls_ = std::move(controls);
        measured_dpi_ = dpi;
        measured_area_ = work_area;
        ATELIER_TRY(checked(ui_.set_style(style)));
        ATELIER_TRY(checked(ui_.set_style_sheet(sheet)));
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
        return {};
    if (place && !SetWindowPos(window(), HWND_TOP, x, y, width, height, SWP_NOACTIVATE))
        return window_failure("Cannot size popup window.");
    // Match the client before showing a newly opened popup; never paint its old-sized canvas.
    if (place) {
        auto target = renderer_.sync_window_target();
        if (!target)
            return std::unexpected(graphics_failure(target.error()));
    }
    if (resize_surface) {
        ATELIER_TRY(retarget(extent));
        restore_focus_ = previous_focus;
    } else if (measure)
        ATELIER_TRY(checked(ui_.set_controls(controls_)));
    if (!continuing) {
        window_.show();
        opening_ = true;
        open_present_count_ = 0;
    }
    visible_ = true;
    dirty_ |= measure || resize_surface;
    return {};
}

Result<void> PopupHost::suspend() {
    if (controls_.empty())
        return {};
    controls_.front().layout.collapsed = true;
    return checked(ui_.set_controls(controls_));
}

Result<void> PopupHost::retarget(ui::PixelExtent extent) {
    // Retire modal input while retaining text/layout caches and stable identities.
    ATELIER_TRY(suspend());
    ATELIER_TRY(checked(ui_.set_surface({extent, window_.dpi_scale(), ++revision_})));
    controls_.front().layout.collapsed = false;
    ATELIER_TRY(checked(ui_.set_controls(controls_)));
    extent_ = extent;
    surface_dpi_ = window_.dpi_scale();
    return {};
}

Result<void> PopupHost::render(renderer::Capture* capture) {
    if (!visible_)
        return {};
    window_.poll([&](NativeEvent&& event) {
        if (event.message == WM_SIZE || event.message == WM_DPICHANGED)
            dirty_ = true;
    });
    if (!dirty_ && !capture)
        return {};
    // The popup surface is part of whichever host phase opened or updated it.
    timing::Suspend whole;
    const auto target = renderer_.sync_window_target();
    if (!target)
        return std::unexpected(graphics_failure(target.error()));
    if (!target->drawable)
        return {};
    if (target->extent != extent_)
        ATELIER_TRY(retarget(target->extent));
    ATELIER_TRY_BIND(candidate, ui_.prepare().transform_error(popup_error));
    const auto result = renderer_.render({*(*candidate)->drawing, {}, {}, {}}, capture);
    if (!result)
        return std::unexpected(graphics_failure(result.error()));
    if (opening_)
        ++open_present_count_;
    if (result->status == renderer::FrameStatus::accepted) {
        if (!ui_.accept(*candidate))
            fatal("Popup rejected its accepted GPU frame.");
        if (opening_) {
            const std::array fields{logging::Field{"source_control", source_},
                                    logging::Field{"surface_revision", revision_},
                                    logging::Field{"frame", result->accepted_id},
                                    logging::Field{"present_attempts", std::uint64_t{open_present_count_}}};
            static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "menu", "menu.frame_accepted",
                                               logging::native::Context{fields}, "Menu opening frame accepted"));
        }
        opening_ = dirty_ = false;
        if (restore_focus_) {
            const auto focused = ui_.focus(restore_focus_);
            dirty_ |= focused.redraw;
            restore_focus_ = 0;
            if (focused.error)
                return std::unexpected(ui_error(*focused.error));
        }
    }
    return {};
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

Result<void> PopupHost::hide() {
    if (!visible_)
        return {};
    ShowWindow(window(), SW_HIDE);
    visible_ = false;
    source_ = restore_focus_ = 0;
    // Key-up after dismissal belongs to the owner. Retire the popup's terminal
    // key latch now, so the next opening does not swallow its first Escape.
    ui::Event loss;
    loss.kind = ui::EventKind::focus_lost;
    auto result = checked(ui_.input(loss));
    retain_failure(result, suspend());
    return result;
}

Result<void> PopupHost::reset_document() {
    auto result = hide();
    retain_failure(result, checked(ui_.set_controls({})));
    retain_failure(result, checked(measurement_.set_controls({})));
    controls_.clear();
    items_.clear();
    row_keys_.clear();
    instance_.reset();
    component_.reset();
    diagnostic_.clear();
    return result;
}

Result<void> PopupHost::close() {
    if (closed_)
        return {};
    auto result = reset_document();
    retain_failure(result, renderer_.close().transform_error(renderer_error));
    closed_ = true;
    return result;
}

Error PopupHost::graphics_failure(const renderer::Error& error) {
    Result<void> result = std::unexpected(renderer_error(error));
    if (error.terminal)
        retain_failure(result, close());
    return std::move(result.error());
}
}
