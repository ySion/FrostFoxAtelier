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
    return kind == ControlKind::label || kind == ControlKind::button || kind == ControlKind::item ||
        kind == ControlKind::number || kind == ControlKind::text_edit;
}
bool text_field(ControlKind kind) { return kind == ControlKind::number || kind == ControlKind::text_edit; }
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
        bool dragging{}, selecting{};
    };
    struct Edit {
        struct Composition {
            std::uint64_t id{};
            EditToken token;
            std::uint32_t first{}, last{};
            Preedit preedit;
        };
        ControlKey key;
        EditToken token;
        detail::TextEdit buffer;
        double original_value{}, preview_value{};
        bool text_mode{};
        float scroll{};
        std::optional<Composition> composition;
        std::optional<ClipboardRequest> clipboard;
        std::optional<ImeRequest> ime_request;
        Edit(const State& state, std::wstring value, double numeric, bool mode, bool select_all)
            : key(state.control.key),token{key,state.incarnation,detail::new_identity(),detail::new_identity()},
              buffer(std::move(value),state.control.kind==ControlKind::number?64:4096,select_all),
              original_value(numeric),preview_value(numeric),text_mode(mode) {}
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
    Key terminal_key{Key::none};

    Impl() { static_cast<void>(layout.set_root({ui_layout::Kind::overlay})); }
    bool active() const {
        return published && published->drawing->extent == surface.extent &&
            published->drawing->dpi_scale == surface.dpi_scale && published->drawing->surface_revision == surface.revision &&
            surface.extent.width && surface.extent.height;
    }
    State* live(ControlKey key) { const auto it = states.find(key); return it == states.end() ? nullptr : &it->second; }
    bool current(const ControlBox& box) const {
        const auto found = states.find(box.key);
        if (found == states.end() || found->second.incarnation != box.incarnation ||
            found->second.control.kind != box.kind || !box.enabled) return false;
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
#include "ui/session_edit.inl"
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
            input.content_revision = text_field(state.control.kind) ?
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
    if (impl_->surface != surface) { reply = impl_->cancel(); impl_->edit.reset(); impl_->focus = 0; impl_->surface = surface; reply.redraw = true; }
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
            static_cast<unsigned>(control.kind) > static_cast<unsigned>(ControlKind::text_edit) ||
            static_cast<unsigned>(control.role) > static_cast<unsigned>(Role::caption) || control.text.size() > 4096 || !std::isfinite(control.value) ||
            !std::isfinite(control.minimum) || !std::isfinite(control.maximum) || control.minimum > control.maximum ||
            !std::isfinite(control.drag_step) || !std::isfinite(control.fine_step) || control.drag_step <= 0 || control.fine_step <= 0 ||
            static_cast<unsigned>(control.drag_axis) > static_cast<unsigned>(DragAxis::vertical))
            return std::unexpected(Error{ErrorCode::invalid_control,control.key});
        if (!detail::valid_text(control.text,control.kind==ControlKind::text_edit))
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
    if ((self.press && absent(self.press->key)) || (self.edit && absent(self.edit->key))) { reply = self.cancel(); self.edit.reset(); }
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
        if (self.edit && self.edit->key == control.key && !self.edit->buffer.active() && !self.edit->composition &&
            (control.kind==ControlKind::number?self.edit->original_value!=control.value:self.edit->buffer.original()!=control.text)) {
            const auto caret=self.edit->buffer.current().caret, anchor=self.edit->buffer.current().anchor;
            const auto mode=self.edit->text_mode;
            self.begin_edit(control.key,mode);
            self.edit->buffer.select(anchor,false); self.edit->buffer.select(caret,true);
        }
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
                self.edit && self.edit->key == key && !self.valid_edit()});
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
                const auto metrics = self.text.metrics(key,width);
                float x = bounds.x + padding.left*scale;
                float y = bounds.y + padding.top*scale;
                const float content_height = std::max(0.f,box->height-(padding.top+padding.bottom));
                // Baseline contexts use the same top origin as layout measurement,
                // including text contributing a nested container's first baseline.
                if (kind != ControlKind::label && !state.baseline_content)
                    y += std::max(0.f,(content_height-metrics.height)*.5f)*scale;
                const auto text_clip = intersect(clip,clip_rect(text_field(kind)?
                    Rect{bounds.x+padding.left*scale,bounds.y+padding.top*scale,width*scale,content_height*scale}:bounds));
                const bool editing=self.edit && self.edit->key==key && self.edit->text_mode;
                Rect caret;
                if (editing) {
                    caret=self.text.caret(key,width,self.display_caret());
                    auto& scroll=self.edit->scroll;
                    scroll=std::clamp(scroll,0.f,std::max(0.f,metrics.width+self.style.caret_width-width));
                    if (caret.x<scroll) scroll=caret.x;
                    if (caret.x+self.style.caret_width>scroll+width) scroll=std::max(0.f,caret.x+self.style.caret_width-width);
                    x-=scroll*scale;
                    if (!self.edit->composition) {
                        const auto& s=self.edit->buffer.current();
                        for (auto range:self.text.selection(key,width,std::min(s.caret.offset,s.anchor.offset),std::max(s.caret.offset,s.anchor.offset)))
                            quad({x+range.x*scale,y+range.y*scale,range.width*scale,range.height*scale},text_clip,appearance.selection);
                    } else {
                        const auto& c=*self.edit->composition;
                        for (std::size_t first=0;first<c.preedit.attributes.size();) {
                            auto last=first+1;
                            while (last<c.preedit.attributes.size() && c.preedit.attributes[last]==c.preedit.attributes[first]) ++last;
                            if (c.preedit.attributes[first]==1 || c.preedit.attributes[first]==3)
                                for (auto range:self.text.selection(key,width,c.first+static_cast<std::uint32_t>(first),c.first+static_cast<std::uint32_t>(last)))
                                    quad({x+range.x*scale,y+range.y*scale,range.width*scale,range.height*scale},text_clip,appearance.selection);
                            first=last;
                        }
                    }
                }
                const auto raster=self.text.raster(key,width,scale,text_field(kind)?
                    std::optional<Clip>{clip_rect({static_cast<float>(text_clip.left)-x,static_cast<float>(text_clip.top)-y,
                        static_cast<float>(text_clip.right-text_clip.left),static_cast<float>(text_clip.bottom-text_clip.top)})}:std::nullopt);
                if (raster.coverage) quad({x+raster.left,y+raster.top,static_cast<float>(raster.coverage->width),
                    static_cast<float>(raster.coverage->height)},text_clip,appearance.foreground,raster.coverage);
                if (editing) {
                    if (self.edit->composition) {
                        const auto& c=*self.edit->composition;
                        for (auto range:self.text.selection(key,width,c.first,c.first+static_cast<std::uint32_t>(c.preedit.text.size())))
                            quad({x+range.x*scale,y+(range.y+range.height)*scale-scale,range.width*scale,scale},text_clip,appearance.caret);
                    }
                    const Rect physical{x+caret.x*scale,y+caret.y*scale,self.style.caret_width*scale,std::max(1.f,caret.height*scale)};
                    quad(physical,text_clip,appearance.caret);
                    if (visible(physical,text_clip)) output->caret=TextCaret{self.edit->token,physical,text_clip};
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
bool Session::wants_keyboard() const noexcept {
    const auto* state=impl_->live(impl_->focus);
    return state && text_field(state->control.kind) && impl_->active();
}
std::optional<TextInputState> Session::text_input() const noexcept {
    const auto& self=*impl_;
    if (!self.edit || self.focus!=self.edit->key || !self.active()) return {};
    const auto* box=self.published->find(self.focus);
    if (!box || !self.current(*box)) return {};
    TextInputState value{self.edit->token,self.edit->composition.has_value(),{}};
    if (self.published->caret && self.published->caret->token==value.token) value.accepted_caret=self.published->caret;
    return value;
}

#include "ui/session_input.inl"
} // namespace atelier::ui
