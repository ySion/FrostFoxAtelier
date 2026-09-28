#include "ui/session.hpp"
#include "ui/text.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

namespace atelier::ui {
namespace {
bool text_kind(ControlKind kind) {
    return kind == ControlKind::label || kind == ControlKind::button || kind == ControlKind::item || kind == ControlKind::number;
}
bool editable(ControlKind kind) { return kind == ControlKind::number || kind == ControlKind::splitter; }
bool focusable(ControlKind kind) { return text_kind(kind) && kind != ControlKind::label; }
bool contains(Rect rect, float x, float y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}
bool contains(Clip clip, float x, float y) { return x >= clip.left && x < clip.right && y >= clip.top && y < clip.bottom; }
Clip intersect(Clip a, Clip b) {
    return {std::max(a.left,b.left),std::max(a.top,b.top),std::min(a.right,b.right),std::min(a.bottom,b.bottom)};
}
Clip clip_rect(Rect value) {
    return {static_cast<std::int32_t>(std::floor(value.x)),static_cast<std::int32_t>(std::floor(value.y)),
        static_cast<std::int32_t>(std::ceil(value.x + value.width)),static_cast<std::int32_t>(std::ceil(value.y + value.height))};
}
bool valid_color(Color c) {
    return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b) && std::isfinite(c.a) &&
        c.r >= 0 && c.r <= 1 && c.g >= 0 && c.g <= 1 && c.b >= 0 && c.b <= 1 && c.a >= 0 && c.a <= 1;
}
#include "ui/style.inl"
bool visible(Rect bounds, Clip clip) {
    return bounds.width > 0 && bounds.height > 0 && clip.right > clip.left && clip.bottom > clip.top &&
        bounds.x < clip.right && bounds.y < clip.bottom && bounds.x+bounds.width > clip.left && bounds.y+bounds.height > clip.top;
}
std::wstring format(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 9);
    if (result.ec != std::errc{}) return L"0";
    return std::wstring(buffer, result.ptr);
}
std::optional<double> number(std::wstring_view text, const Control& control) {
    if (text.empty() || text.size() > 64) return {};
    char buffer[64];
    for (std::size_t i = 0; i < text.size(); ++i) { if (text[i] > 127) return {}; buffer[i] = static_cast<char>(text[i]); }
    const auto* begin = buffer;
    if (*begin == '+') ++begin;
    double value{};
    const auto parsed = std::from_chars(begin, buffer + text.size(), value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != buffer + text.size() || !std::isfinite(value) ||
        value < control.minimum || value > control.maximum) return {};
    return value;
}
}

const ControlBox* UiFrame::find(ControlKey key) const noexcept {
    const auto found = std::find_if(controls.begin(), controls.end(), [key](const auto& box) { return box.key == key; });
    return found == controls.end() ? nullptr : &*found;
}

struct Session::Impl {
    struct State {
        Control control;
        ui_layout::LayoutNodeId node;
        std::uint64_t incarnation{};
        double scroll{}, scroll_limit{};
        float content_bottom{};
        bool baseline_content{};
        std::size_t frame_index{std::numeric_limits<std::size_t>::max()};
    };
    struct Press {
        ControlKey key{};
        float x{}, y{}, last_x{}, last_y{};
        double original{}, value{};
        bool dragging{};
    };
    struct Edit {
        ControlKey key{};
        std::wstring text;
        std::size_t caret{}, anchor{};
        double original{}, preview{};
        bool begun{};
    };
    std::uint64_t identity{detail::new_identity()};
    Style style;
    std::uint64_t style_revision{detail::new_identity()}, prepares{}, culled_text_controls{};
    Surface surface;
    detail::TextStore text;
    ui_layout::LayoutDocument layout{detail::TextStore::measure, &text, {10001, 128}};
    std::map<ControlKey, State> states;
    std::vector<ControlKey> order;
    std::vector<ui_layout::LayoutNodeId> retired;
    bool structure_dirty{true};
    std::shared_ptr<const UiFrame> published;
    std::optional<Press> press;
    std::optional<Edit> edit;
    ControlKey focus{}, hover{};
    ControlKey keyboard_pressed{};
    Key keyboard_key{Key::none};

    Impl() { static_cast<void>(layout.set_root({ui_layout::Kind::overlay})); }
    bool active() const {
        return published && published->drawing->extent == surface.extent &&
            published->drawing->dpi_scale == surface.dpi_scale && published->drawing->surface_revision == surface.revision &&
            surface.extent.width && surface.extent.height;
    }
    State* live(ControlKey key) { const auto it = states.find(key); return it == states.end() ? nullptr : &it->second; }
    bool current(const ControlBox& box) const {
        const auto found = states.find(box.key);
        if (found == states.end() || found->second.incarnation != box.incarnation || !box.enabled) return false;
        auto key = box.key;
        while (key) {
            const auto& control = states.at(key).control;
            if (!control.enabled || control.layout.collapsed) return false;
            key = control.parent;
        }
        return true;
    }
    const ControlBox* hit(float x, float y) const {
        if (!active()) return nullptr;
        for (auto it = published->controls.rbegin(); it != published->controls.rend(); ++it)
            if (contains(it->bounds,x,y) && contains(it->clip,x,y) && current(*it)) return &*it;
        return nullptr;
    }
    void begin_edit(ControlKey key) {
        const auto& control = states.at(key).control;
        edit = Edit{key, format(control.value), 0, 0, control.value, control.value, false};
        edit->caret = edit->text.size(); // Initial click/select focuses the complete number.
    }
    void finish_edit(Reply& reply, bool commit) {
        if (!edit) return;
        const auto* state = live(edit->key);
        const auto parsed = state ? number(edit->text, state->control) : std::nullopt;
        if (edit->begun) reply.actions.push_back({commit && parsed ? ActionKind::edit_commit : ActionKind::edit_cancel,
            edit->key, commit && parsed ? *parsed : edit->original});
        edit.reset(); reply.redraw = true;
    }
    Reply cancel() {
        Reply reply;
        if (press) {
            if (press->dragging) reply.actions.push_back({ActionKind::edit_cancel, press->key, press->original});
            press.reset(); reply.capture = Capture::release; reply.redraw = true;
        }
        finish_edit(reply, false); keyboard_pressed = false; hover = 0;
        return reply;
    }
    void change_text(Reply& reply, std::wstring replacement) {
        if (!edit || replacement == edit->text) return;
        if (!edit->begun) {
            reply.actions.push_back({ActionKind::edit_begin, edit->key, edit->original}); edit->begun = true;
        }
        edit->text = std::move(replacement);
        const auto parsed = number(edit->text, states.at(edit->key).control);
        if (parsed && *parsed != edit->preview) {
            reply.actions.push_back({ActionKind::edit_preview, edit->key, *parsed}); edit->preview = *parsed;
        }
        reply.redraw = true;
    }
    std::wstring display(const State& state) const {
        if (state.control.kind != ControlKind::number) return state.control.text;
        if (press && press->key == state.control.key && press->dragging) return format(press->value);
        if (edit && edit->key == state.control.key) return edit->text;
        return format(state.control.value);
    }
    ui_layout::NodeInput input_for(State& state) {
        auto input = state.control.layout;
        auto alignment = input.vertical_alignment;
        state.baseline_content = false;
        if (state.control.parent) {
            const auto& parent = states.at(state.control.parent);
            state.baseline_content = parent.baseline_content;
            if (alignment.inherited && parent.control.kind == ControlKind::container &&
                parent.control.layout.kind == ui_layout::Kind::row && parent.control.layout.flow)
                alignment = parent.control.layout.flow->align_items;
        }
        state.baseline_content |= alignment.first_baseline;
        const auto appearance = resolve_style(style,state.control.role,state.control.kind);
        input.padding = effective_padding(state.control,appearance);
        if (text_kind(state.control.kind)) {
            input.kind = ui_layout::Kind::measured;
            input.content_key = state.control.key;
            input.content_revision = state.control.kind == ControlKind::number ?
                text.set(state.control.key, display(state), style.font,appearance.font_size,appearance.font_weight,false) :
                text.set(state.control.key, state.control.text, style.font,appearance.font_size,appearance.font_weight,state.control.kind == ControlKind::label);
        } else if (state.control.kind == ControlKind::viewport || state.control.kind == ControlKind::splitter) {
            input.kind = ui_layout::Kind::box;
        } else if (state.control.kind == ControlKind::list) {
            input.kind = ui_layout::Kind::column;
        }
        return input;
    }
    Result<void> synchronize() {
        const auto checked = [](const auto& result, ControlKey key) -> Result<void> {
            if (!result) return std::unexpected(Error{ErrorCode::layout_failed,key,result.error().provider_code});
            return {};
        };
        if (structure_dirty) {
            // Retained nodes detach before obsolete ancestors are removed.
            for (auto key : order) if (states.at(key).node)
                if (auto result = checked(layout.move(states.at(key).node, layout.root()), key); !result) return result;
            for (auto node : retired) if (layout.input(node))
                if (auto result = checked(layout.remove(node), 0); !result) return result;
            retired.clear();
        }
        for (auto key : order) {
            auto& state = states.at(key);
            auto input = input_for(state);
            const auto parent = state.control.parent ? states.at(state.control.parent).node : layout.root();
            if (!state.node) {
                const auto created = layout.create(parent, input);
                if (!created) return std::unexpected(Error{ErrorCode::layout_failed,key,created.error().provider_code});
                state.node = *created;
            } else {
                if (auto result = checked(layout.update(state.node, input), key); !result) return result;
                if (structure_dirty)
                    if (auto result = checked(layout.move(state.node, parent), key); !result) return result;
            }
        }
        structure_dirty = false;
        return {};
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;
Result<void> Session::set_style(const Style& style) {
    if (!valid_style(style)) return std::unexpected(Error{ErrorCode::invalid_style});
    auto& self = *impl_;
    if (self.style == style) return {};
    Style candidate = style; // Finish allocating before changing the active configuration.
    const bool font_changed = !same_font_policy(self.style,candidate);
    const auto revision = detail::new_identity();
    const auto provider = font_changed ? detail::new_identity() : self.text.provider_revision();
    static_assert(std::is_nothrow_move_assignable_v<Style>);
    self.style = std::move(candidate);
    self.style_revision = revision;
    if (font_changed) self.text.set_provider_revision(provider);
    return {};
}
const Style& Session::style() const noexcept { return impl_->style; }
Statistics Session::statistics() const noexcept {
    const auto text = impl_->text.statistics();
    return {impl_->prepares,text.measure_calls,text.text_layouts,text.rasterizations,text.raster_bytes,text.coverage_cache_hits,
        impl_->culled_text_controls,impl_->text.provider_revision(),text.live_coverages,text.live_coverage_bytes};
}
Result<Reply> Session::set_surface(Surface surface) {
    if (!std::isfinite(surface.dpi_scale) || surface.dpi_scale < .25f || surface.dpi_scale > 8 ||
        !surface.revision || surface.extent.width > 32768 || surface.extent.height > 32768)
        return std::unexpected(Error{ErrorCode::invalid_surface});
    Reply reply;
    if (impl_->surface != surface) { reply = impl_->cancel(); impl_->focus = 0; impl_->surface = surface; reply.redraw = true; }
    return reply;
}
Result<Reply> Session::set_controls(std::span<const Control> controls) {
    auto& self = *impl_;
    if (controls.size() > 10000) return std::unexpected(Error{ErrorCode::capacity_exceeded});
    bool unchanged = controls.size() == self.order.size();
    for (std::size_t i = 0; unchanged && i < controls.size(); ++i)
        unchanged = controls[i].key == self.order[i] && controls[i] == self.states.at(self.order[i]).control;
    if (unchanged) return Reply{};
    std::map<ControlKey, const Control*> validated;
    std::vector<ControlKey> order; order.reserve(controls.size());
    for (const auto& control : controls) {
        if (!control.key || validated.contains(control.key) || (control.parent && !validated.contains(control.parent)) ||
            static_cast<unsigned>(control.kind) > static_cast<unsigned>(ControlKind::viewport) ||
            static_cast<unsigned>(control.role) > static_cast<unsigned>(Role::caption) || control.text.size() > 4096 || !std::isfinite(control.value) ||
            !std::isfinite(control.minimum) || !std::isfinite(control.maximum) || control.minimum > control.maximum ||
            !std::isfinite(control.drag_step) || !std::isfinite(control.fine_step) || control.drag_step <= 0 || control.fine_step <= 0 ||
            static_cast<unsigned>(control.drag_axis) > static_cast<unsigned>(DragAxis::vertical))
            return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        if (control.parent) {
            const auto kind = validated.at(control.parent)->kind;
            if (kind != ControlKind::container && kind != ControlKind::list)
                return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        }
        if (editable(control.kind) && (control.value < control.minimum || control.value > control.maximum))
            return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        validated.emplace(control.key,&control); order.push_back(control.key);
    }
    Reply reply;
    const auto absent = [&](ControlKey key) {
        while (key) {
            const auto found = validated.find(key);
            if (found == validated.end() || !found->second->enabled || found->second->layout.collapsed ||
                (self.live(key) && found->second->kind != self.live(key)->control.kind)) return true;
            key = found->second->parent;
        }
        return false;
    };
    if ((self.press && absent(self.press->key)) || (self.edit && absent(self.edit->key))) reply = self.cancel();
    if (absent(self.focus)) self.focus = 0;
    self.structure_dirty |= order != self.order;
    for (auto it = self.states.begin(); it != self.states.end();) {
        if (!validated.contains(it->first)) {
            if (it->second.node) self.retired.push_back(it->second.node);
            self.text.erase(it->first); it = self.states.erase(it);
        } else ++it;
    }
    for (const auto& control : controls) {
        auto [it, added] = self.states.try_emplace(control.key);
        auto& state = it->second;
        if (added) state.incarnation = detail::new_identity();
        self.structure_dirty |= added || state.control.parent != control.parent || state.control.kind != control.kind;
        if (text_kind(state.control.kind) && !text_kind(control.kind)) self.text.erase(control.key);
        state.control = control;
        if (self.edit && self.edit->key == control.key && !self.edit->begun && self.edit->original != control.value) self.begin_edit(control.key);
    }
    self.order = std::move(order);
    return reply;
}

Result<std::shared_ptr<const UiFrame>> Session::prepare() {
    auto& self = *impl_;
    ++self.prepares;
    if (!self.surface.extent.width || !self.surface.extent.height) return std::unexpected(Error{ErrorCode::unavailable_surface});
    try {
        if (auto synced = self.synchronize(); !synced) return std::unexpected(synced.error());
        const float scale = self.surface.dpi_scale;
        const auto geometry = self.layout.compute({self.surface.extent.width / scale,self.surface.extent.height / scale}, self.text.provider_revision());
        if (!geometry) return std::unexpected(Error{ErrorCode::layout_failed,0,geometry.error().provider_code});
        auto output = std::make_shared<UiFrame>();
        auto draws = std::make_shared<Frame>();
        output->controls.reserve(self.order.size()); draws->draws.reserve(self.order.size()*3);
        draws->id = detail::new_identity();
        draws->extent = self.surface.extent; draws->dpi_scale = scale; draws->surface_revision = self.surface.revision;
        output->session_identity = self.identity;
        output->style_revision = self.style_revision;
        output->measure_provider_revision = self.text.provider_revision();
        const Clip target{0,0,static_cast<std::int32_t>(draws->extent.width),static_cast<std::int32_t>(draws->extent.height)};
        for (auto key : self.order) {
            auto& state = self.states.at(key); state.frame_index = std::numeric_limits<std::size_t>::max(); state.content_bottom = 0;
        }
        for (auto key : self.order) {
            const auto& state = self.states.at(key);
            if (state.control.parent && self.states.at(state.control.parent).control.kind == ControlKind::list)
                if (const auto* box = geometry->find(state.node))
                    self.states.at(state.control.parent).content_bottom = std::max(self.states.at(state.control.parent).content_bottom,box->y+box->height);
        }
        for (auto key : self.order) {
            auto& state = self.states.at(key);
            if (state.control.kind == ControlKind::list)
                if (const auto* box = geometry->find(state.node)) {
                    state.scroll_limit = std::max(0.f,state.content_bottom+state.control.layout.padding.bottom-box->height);
                    state.scroll = std::clamp(state.scroll,0.0,state.scroll_limit);
                }
        }
        auto quad = [&](Rect bounds, Clip clip, Color color, std::shared_ptr<const Coverage> coverage = {}) {
            clip = intersect(clip,target);
            if (visible(bounds,clip) && color.a > 0)
                draws->draws.push_back({bounds,clip,color,{0,0,1,1},std::move(coverage)});
        };
        for (auto key : self.order) {
            auto& state = self.states.at(key);
            const auto* box = geometry->find(state.node);
            if (!box) continue;
            Rect bounds{box->x*scale,box->y*scale,box->width*scale,box->height*scale};
            Clip clip = target;
            bool enabled = state.control.enabled;
            if (state.control.parent) {
                const auto& parent_state = self.states.at(state.control.parent);
                if (parent_state.frame_index == std::numeric_limits<std::size_t>::max()) continue;
                const auto& parent_box = output->controls[parent_state.frame_index];
                bounds.x += parent_box.bounds.x; bounds.y += parent_box.bounds.y;
                clip = intersect(parent_box.clip,clip_rect(parent_box.bounds));
                enabled &= parent_box.enabled;
                if (parent_state.control.kind == ControlKind::list) bounds.y -= static_cast<float>(parent_state.scroll)*scale;
            }
            // Cap before conversion to integer clip; layout can legally overflow.
            if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || std::abs(bounds.x) > 1e8f || std::abs(bounds.y) > 1e8f ||
                bounds.width > 1e8f || bounds.height > 1e8f) return std::unexpected(Error{ErrorCode::invalid_control,key});
            state.frame_index = output->controls.size();
            output->controls.push_back({key,state.control.parent,state.control.kind,bounds,clip,enabled,state.incarnation});
            const auto kind = state.control.kind;
            // Keep every arranged box and its parent transform. Only painting is culled.
            if (!visible(bounds,clip)) {
                if (text_kind(kind)) ++self.culled_text_controls;
                continue;
            }
            const auto appearance = resolve_style(self.style,state.control.role,kind,{enabled,self.hover == key,
                (self.press && self.press->key == key) || self.keyboard_pressed == key,self.focus == key,state.control.selected,
                self.edit && self.edit->key == key && !number(self.edit->text,state.control)});
            if (appearance.background) quad(bounds,clip,*appearance.background);
            const float border = std::min({appearance.border_width*scale,bounds.width*.5f,bounds.height*.5f});
            if (border > 0) {
                quad({bounds.x,bounds.y,bounds.width,border},clip,appearance.border);
                quad({bounds.x,bounds.y+bounds.height-border,bounds.width,border},clip,appearance.border);
                quad({bounds.x,bounds.y+border,border,bounds.height-2*border},clip,appearance.border);
                quad({bounds.x+bounds.width-border,bounds.y+border,border,bounds.height-2*border},clip,appearance.border);
            }
            if (text_kind(kind)) {
                const auto padding = effective_padding(state.control,appearance);
                const float width = std::max(0.f, box->width - (padding.left + padding.right));
                auto raster = self.text.raster(key,width,scale);
                const float x = bounds.x + padding.left*scale;
                float y = bounds.y + padding.top*scale;
                const float content_height = std::max(0.f,box->height-(padding.top+padding.bottom));
                // Baseline contexts use the same top origin as layout measurement,
                // including text contributing a nested container's first baseline.
                if (kind != ControlKind::label && !state.baseline_content)
                    y += std::max(0.f,(content_height-raster.logical_height)*.5f)*scale;
                const auto text_clip = intersect(clip,clip_rect(bounds));
                if (self.edit && self.edit->key == key) {
                    const auto first = std::min(self.edit->caret,self.edit->anchor), last = std::max(self.edit->caret,self.edit->anchor);
                    if (first != last) {
                        const auto begin = self.text.caret(key,width,static_cast<std::uint32_t>(first))*scale;
                        const auto end = self.text.caret(key,width,static_cast<std::uint32_t>(last))*scale;
                        quad({x+begin,y,end-begin,raster.logical_height*scale},text_clip,appearance.selection);
                    }
                }
                if (raster.coverage) quad({x+raster.left,y+raster.top,static_cast<float>(raster.coverage->width),
                    static_cast<float>(raster.coverage->height)},text_clip,appearance.foreground,raster.coverage);
                if (self.edit && self.edit->key == key) {
                    const auto caret = self.text.caret(key,width,static_cast<std::uint32_t>(self.edit->caret))*scale;
                    quad({x+caret,y,self.style.caret_width*scale,std::max(1.f,raster.logical_height*scale)},text_clip,appearance.caret);
                }
            }
        }
        for (auto key : self.order) {
            auto& state = self.states.at(key);
            if (state.control.kind != ControlKind::list || state.frame_index == std::numeric_limits<std::size_t>::max()) continue;
            const auto& bounds = output->controls[state.frame_index];
            if (state.scroll_limit > 0) {
                const float track = bounds.bounds.height;
                const float thumb = std::min(track,std::max(self.style.scrollbar_min_thumb*scale, track*track/(track+static_cast<float>(state.scroll_limit)*scale)));
                const float y = bounds.bounds.y + static_cast<float>(state.scroll/state.scroll_limit)*(track-thumb);
                quad({bounds.bounds.x+bounds.bounds.width-(self.style.scrollbar_width+self.style.scrollbar_inset)*scale,
                    y,self.style.scrollbar_width*scale,thumb},intersect(bounds.clip,clip_rect(bounds.bounds)),self.style.palette.muted);
            }
        }
        output->drawing = std::move(draws);
        return std::shared_ptr<const UiFrame>(std::move(output));
    } catch (const detail::TextFailure& failure) { return std::unexpected(Error{ErrorCode::text_failed,0,failure.code}); }
}
bool Session::accept(std::shared_ptr<const UiFrame> frame) {
    auto& self = *impl_;
    if (!frame || !frame->drawing || frame->session_identity != self.identity || frame->drawing->extent != self.surface.extent ||
        frame->drawing->dpi_scale != self.surface.dpi_scale || frame->drawing->surface_revision != self.surface.revision ||
        (self.published && frame->drawing->id < self.published->drawing->id)) return false;
    self.published = std::move(frame); return true;
}
const std::shared_ptr<const UiFrame>& Session::presented() const noexcept { return impl_->published; }
Reply Session::cancel_interaction() { return impl_->cancel(); }
bool Session::wants_keyboard() const noexcept { return impl_->focus && impl_->active(); }

Reply Session::input(const Event& event) {
    auto& self = *impl_;
    // Win32 also reports capture loss after our normal ReleaseCapture request.
    // With no pointer press left, this must not cancel the new keyboard editor.
    if (event.kind == EventKind::capture_lost && !self.press) return {};
    if (event.kind == EventKind::focus_lost || event.kind == EventKind::capture_lost) {
        auto reply = self.cancel(); if (event.kind == EventKind::focus_lost) self.focus = 0; return reply;
    }
    Reply reply;
    if (!self.active()) return reply;
    if (!std::isfinite(event.x) || !std::isfinite(event.y) || !std::isfinite(event.wheel)) return reply;
    const auto* hit = self.hit(event.x,event.y);
    if (event.kind == EventKind::pointer_move) {
        const auto key = hit ? hit->key : 0;
        reply.redraw = self.hover != key; self.hover = key;
        if (self.press) {
            auto& press = *self.press;
            const auto* state = self.live(press.key);
            if (!state) return self.cancel();
            const auto& control = state->control;
            if (editable(control.kind)) {
                const auto axis_delta = control.drag_axis == DragAxis::horizontal ? event.x-press.last_x : event.y-press.last_y;
                if (!press.dragging && (std::abs(event.x-press.x) > 3 || std::abs(event.y-press.y) > 3)) {
                    self.finish_edit(reply,true); press.dragging = true;
                    reply.actions.push_back({ActionKind::edit_begin,press.key,press.original});
                }
                if (press.dragging) {
                    press.value = std::clamp(press.value + axis_delta/self.surface.dpi_scale*(event.shift ? control.fine_step : control.drag_step),control.minimum,control.maximum);
                    reply.actions.push_back({ActionKind::edit_preview,press.key,press.value}); reply.redraw = true;
                }
            }
            press.last_x = event.x; press.last_y = event.y; reply.consumed = true;
        } else reply.consumed = hit && hit->kind != ControlKind::viewport;
        if (hit) {
            const auto& control = self.states.at(hit->key).control;
            if (control.kind == ControlKind::number) reply.cursor = Cursor::drag_horizontal;
            else if (control.kind == ControlKind::splitter) reply.cursor = control.drag_axis == DragAxis::horizontal ? Cursor::resize_horizontal : Cursor::resize_vertical;
        }
        return reply;
    }
    if (event.kind == EventKind::pointer_down) {
        reply.consumed = hit && hit->kind != ControlKind::viewport;
        if (event.button != PointerButton::primary) return reply;
        self.keyboard_pressed = 0;
        self.finish_edit(reply,true);
        self.focus = hit && focusable(hit->kind) ? hit->key : 0;
        if (!hit || (!focusable(hit->kind) && hit->kind != ControlKind::splitter)) return reply;
        const auto& control = self.states.at(hit->key).control;
        self.press = Impl::Press{hit->key,event.x,event.y,event.x,event.y,control.value,control.value,false};
        reply.capture = Capture::acquire; reply.redraw = true; return reply;
    }
    if (event.kind == EventKind::pointer_up) {
        if (event.button != PointerButton::primary || !self.press) return reply;
        const auto press = *self.press; self.press.reset();
        reply.consumed = true; reply.capture = Capture::release; reply.redraw = true;
        if (press.dragging) reply.actions.push_back({ActionKind::edit_commit,press.key,press.value});
        else if (hit && hit->key == press.key) {
            if (hit->kind == ControlKind::number) self.begin_edit(hit->key);
            else if (hit->kind == ControlKind::button || hit->kind == ControlKind::item)
                reply.actions.push_back({hit->kind == ControlKind::item ? ActionKind::select : ActionKind::activate,hit->key,0});
        }
        return reply;
    }
    if (event.kind == EventKind::wheel) {
        auto key = hit ? hit->key : 0;
        while (key) {
            auto& state = self.states.at(key);
            if (state.control.kind == ControlKind::list) {
                state.scroll = std::clamp(state.scroll-static_cast<double>(event.wheel)*48,0.0,state.scroll_limit);
                reply.consumed = reply.redraw = true; return reply;
            }
            const auto* box = self.published->find(key); key = box ? box->parent : 0;
        }
        reply.consumed = hit && hit->kind != ControlKind::viewport; return reply;
    }
    if (event.kind == EventKind::key_down && event.key == Key::escape) {
        const bool had_interaction = self.press.has_value() || self.edit.has_value();
        reply = self.cancel(); reply.consumed = had_interaction; self.focus = 0; return reply;
    }
    if (self.press && self.press->dragging) { reply.consumed = true; return reply; }
    if (event.kind == EventKind::key_down && event.key == Key::tab) {
        self.finish_edit(reply,true);
        std::vector<ControlKey> keys;
        for (const auto& box : self.published->controls) if (self.current(box) && focusable(box.kind) &&
            box.clip.right > box.clip.left && box.clip.bottom > box.clip.top) keys.push_back(box.key);
        if (!keys.empty()) {
            const auto at = std::find(keys.begin(),keys.end(),self.focus);
            const auto index = at == keys.end() ? (event.shift ? keys.size()-1 : 0) :
                (static_cast<std::size_t>(at-keys.begin())+keys.size()+(event.shift ? -1 : 1))%keys.size();
            self.focus = keys[index];
            if (self.states.at(self.focus).control.kind == ControlKind::number) self.begin_edit(self.focus);
        }
        reply.consumed = reply.redraw = true; return reply;
    }
    auto* focused = self.live(self.focus);
    const auto* focused_box = self.published->find(self.focus);
    if (!focused || !focused_box || !self.current(*focused_box)) return reply;
    if (focused->control.kind == ControlKind::number) {
        reply.consumed = true;
        if (!self.edit) self.begin_edit(self.focus);
        if (event.kind == EventKind::text && !event.control && !event.alt) {
            if ((event.character >= U'0' && event.character <= U'9') || event.character == U'.' || event.character == U'-' ||
                event.character == U'+' || event.character == U'e' || event.character == U'E') {
                const auto first = std::min(self.edit->caret,self.edit->anchor), last = std::max(self.edit->caret,self.edit->anchor);
                auto replacement = self.edit->text;
                replacement.replace(first,last-first,1,static_cast<wchar_t>(event.character));
                if (replacement.size() <= 64) { self.change_text(reply,std::move(replacement)); self.edit->caret = self.edit->anchor = first+1; }
            }
        } else if (event.kind == EventKind::key_down) {
            if (event.key == Key::enter) { self.finish_edit(reply,true); self.focus = 0; }
            else if (event.control && event.key == Key::a) { self.edit->anchor = 0; self.edit->caret = self.edit->text.size(); reply.redraw = true; }
            else if (event.control && event.key == Key::z) { self.finish_edit(reply,false); self.begin_edit(self.focus); reply.redraw = true; }
            else if (!event.control && (event.key == Key::delete_key || event.key == Key::backspace)) {
                auto first = std::min(self.edit->caret,self.edit->anchor), last = std::max(self.edit->caret,self.edit->anchor);
                if (first == last) { if (event.key == Key::backspace && first) --first; else if (event.key == Key::delete_key && last < self.edit->text.size()) ++last; }
                auto replacement = self.edit->text; replacement.erase(first,last-first);
                self.change_text(reply,std::move(replacement)); self.edit->caret = self.edit->anchor = first;
            } else if (!event.control && (event.key == Key::left || event.key == Key::right || event.key == Key::home || event.key == Key::end)) {
                auto& position = self.edit->caret;
                if (event.key == Key::left && position) --position;
                if (event.key == Key::right && position < self.edit->text.size()) ++position;
                if (event.key == Key::home) position = 0;
                if (event.key == Key::end) position = self.edit->text.size();
                if (!event.shift) self.edit->anchor = position;
                reply.redraw = true;
            }
        }
        return reply;
    }
    if ((focused->control.kind == ControlKind::button || focused->control.kind == ControlKind::item) &&
        (event.key == Key::space || event.key == Key::enter)) {
        reply.consumed = true;
        if (event.kind == EventKind::key_down && !self.keyboard_pressed) {
            self.keyboard_pressed = self.focus; self.keyboard_key = event.key;
            reply.redraw = true;
        } else if (event.kind == EventKind::key_up) {
            const auto pressed = std::exchange(self.keyboard_pressed,ControlKey{});
            reply.redraw = pressed != 0;
            if (pressed == self.focus && self.keyboard_key == event.key)
                reply.actions.push_back({focused->control.kind == ControlKind::item ? ActionKind::select : ActionKind::activate,self.focus,0});
        }
    }
    return reply;
}
} // namespace atelier::ui
