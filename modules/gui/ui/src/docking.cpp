#include "ui/docking.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atelier::ui {
namespace {
constexpr float divider_width = 8;

constexpr ControlKey close_offset = 1'000'000;

constexpr ControlKey wrapper_offset = 2'000'000;

constexpr ControlKey open_offset = 3'000'000;

constexpr ControlKey preview_offset = 4'000'000;

constexpr ControlKey command_offset = 5'000'000;

bool inside(Rect r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

struct SplitRange {
    float span{};

    float low{};

    float high{};
};

SplitRange split_range(float size, float first, float second) {
    const auto span = std::max(0.f, size - divider_width);
    const auto low = span >= first + second ? first : span * first / (first + second);
    return {span, low, span >= first + second ? span - second : low};
}

}

DockSpace::DockSpace(ControlKey base, std::vector<DockTab> tabs) : base_(base), next_(base), tabs_(std::move(tabs)) {
    if (!base || base > std::numeric_limits<ControlKey>::max() - command_offset - 256 || tabs_.empty() ||
        tabs_.size() > 64)
        throw std::invalid_argument("Invalid dock registry or key range.");
    std::vector<ControlKey> keys;
    for (const auto& view : tabs_) {
        if (!view.key || !view.content || view.title.empty() || !std::isfinite(view.minimum.width) ||
            !std::isfinite(view.minimum.height) || view.minimum.width < 120 || view.minimum.height < 60)
            throw std::invalid_argument("Invalid dock view.");
        for (auto key : {view.key, view.content}) {
            if ((key >= base && key <= base + command_offset + 256) ||
                std::find(keys.begin(), keys.end(), key) != keys.end())
                throw std::invalid_argument("Overlapping dock identities.");
            keys.push_back(key);
        }
    }
    reset();
}

DockSpace::~DockSpace() = default;

std::unique_ptr<DockSpace::Node> DockSpace::make_node() {
    if (next_ >= base_ + close_offset - 16)
        throw std::length_error("Dock identities exhausted.");
    auto result = std::make_unique<Node>();
    result->key = next_;
    next_ += 16;
    return result;
}

const DockTab* DockSpace::tab(ControlKey key) const noexcept {
    const auto at = std::find_if(tabs_.begin(), tabs_.end(), [&](const auto& view) { return view.key == key; });
    return at == tabs_.end() ? nullptr : &*at;
}

DockSpace::Node* DockSpace::find(ControlKey key) const noexcept {
    const auto visit = [&](auto&& self, Node* node) -> Node* {
        if (node->key == key)
            return node;
        if (!node->split())
            return nullptr;
        if (auto* found = self(self, node->first.get()))
            return found;
        return self(self, node->second.get());
    };
    return visit(visit, root_.get());
}

DockSpace::Node* DockSpace::owner(ControlKey key) const noexcept {
    const auto visit = [&](auto&& self, Node* node) -> Node* {
        if (!node->split())
            return std::find(node->tabs.begin(), node->tabs.end(), key) != node->tabs.end() ? node : nullptr;
        if (auto* found = self(self, node->first.get()))
            return found;
        return self(self, node->second.get());
    };
    return visit(visit, root_.get());
}

void DockSpace::reset() {
    root_ = make_node();
    for (const auto& view : tabs_)
        root_->tabs.push_back(view.key);
    focused_ = root_->active = tabs_.front().key;
    maximized_ = menu_pane_ = 0;
    menu_add_ = menu_type_ = false;
    closed_tabs_.clear();
    drag_.reset();
    resize_.reset();
}

ControlKey DockSpace::pane(ControlKey key) const noexcept {
    const auto* node = owner(key);
    return node ? node->key : 0;
}

bool DockSpace::visible(ControlKey key) const noexcept {
    const auto* node = owner(key);
    if (!node || node->active != key || (maximized_ && maximized_ != node->key))
        return false;
    for (auto* above = parent(node->key); above; node = above, above = parent(node->key))
        if ((above->collapsed == 1 && above->first.get() == node) ||
            (above->collapsed == 2 && above->second.get() == node))
            return false;
    return true;
}

DockSpace::Node* DockSpace::parent(ControlKey key) const noexcept {
    const auto visit = [&](auto&& self, Node* node) -> Node* {
        if (!node->split())
            return nullptr;
        if (node->first->key == key || node->second->key == key)
            return node;
        if (auto* found = self(self, node->first.get()))
            return found;
        return self(self, node->second.get());
    };
    return visit(visit, root_.get());
}

void DockSpace::reveal(ControlKey key) {
    auto* node = owner(key);
    if (!node)
        return;
    for (auto* above = parent(node->key); above; node = above, above = parent(node->key)) {
        if ((above->collapsed == 1 && above->first.get() == node) ||
            (above->collapsed == 2 && above->second.get() == node))
            above->collapsed = 0;
    }
}

void DockSpace::collapse(Node& node, unsigned side) {
    node.collapsed = node.collapsed == side ? 0 : side;
    maximized_ = 0;
    drag_.reset();
    resize_.reset();
    if (!visible(focused_)) {
        focused_ = 0;
        for (const auto& view : tabs_)
            if (visible(view.key)) {
                focused_ = view.key;
                break;
            }
    }
}

void DockSpace::remove(Node& node, ControlKey key) {
    const auto at = std::find(node.tabs.begin(), node.tabs.end(), key);
    if (at == node.tabs.end())
        return;
    const auto index = static_cast<std::size_t>(at - node.tabs.begin());
    node.tabs.erase(at);
    if (node.active == key)
        node.active = node.tabs.empty() ? 0 : node.tabs[std::min(index, node.tabs.size() - 1)];
}

void DockSpace::normalize(std::unique_ptr<Node>& node) {
    if (!node->split())
        return;
    normalize(node->first);
    normalize(node->second);
    if (!node->first->split() && node->first->tabs.empty()) {
        auto remaining = std::move(node->second);
        node = std::move(remaining);
    } else if (!node->second->split() && node->second->tabs.empty()) {
        auto remaining = std::move(node->first);
        node = std::move(remaining);
    }
}

bool DockSpace::select(ControlKey key) {
    if (!tab(key))
        return false;
    resize_.reset();
    auto* node = owner(key);
    if (!node) {
        node = find(menu_pane_);
        if (!node || node->split()) {
            node = root_.get();
            while (node->split())
                node = node->second.get();
        }
        node->tabs.push_back(key);
    }
    std::erase(closed_tabs_, key);
    reveal(key);
    focused_ = node->active = key;
    if (maximized_)
        maximized_ = node->key;
    return true;
}

bool DockSpace::close(ControlKey key) {
    auto* node = owner(key);
    if (!node)
        return false;
    std::erase(closed_tabs_, key);
    closed_tabs_.push_back(key);
    remove(*node, key);
    const auto next = node->active;
    normalize(root_);
    if (maximized_ && !find(maximized_))
        maximized_ = 0;
    if (focused_ == key) {
        focused_ = next;
        if (!focused_)
            for (const auto& view : tabs_)
                if (visible(view.key)) {
                    focused_ = view.key;
                    break;
                }
    }
    drag_.reset();
    resize_.reset();
    return true;
}

void DockSpace::maximize(ControlKey key) {
    resize_.reset();
    reveal(key);
    if (const auto* node = owner(key)) {
        maximized_ = maximized_ == node->key ? 0 : node->key;
        focused_ = node->active;
    }
}

void DockSpace::cycle(int direction) {
    auto* node = owner(focused_);
    if (!node || node->tabs.empty())
        return;
    const auto at = std::find(node->tabs.begin(), node->tabs.end(), node->active) - node->tabs.begin();
    const auto count = static_cast<std::ptrdiff_t>(node->tabs.size());
    select(node->tabs[static_cast<std::size_t>((at + count + (direction < 0 ? -1 : 1)) % count)]);
}

void DockSpace::focus_control(ControlKey key, const UiFrame& frame) {
    while (key) {
        for (const auto& view : tabs_)
            if (key == view.content && visible(view.key)) {
                focused_ = view.key;
                return;
            }
        const auto* box = frame.find(key);
        if (!box)
            return;
        key = box->parent;
    }
}

bool DockSpace::move(ControlKey key, ControlKey target, DockSide side, float fraction) {
    const auto* node = owner(target);
    return node && place(key, node->key, side, 0, fraction);
}

bool DockSpace::place(ControlKey key, ControlKey target, DockSide side, ControlKey before, float fraction) {
    if (!tab(key) || !std::isfinite(fraction) || fraction <= 0 || fraction >= 1)
        return false;
    auto* to = find(target);
    auto* from = owner(key);
    if (!to || to->split() || !from || (to == from && side != DockSide::tabs && from->tabs.size() == 1))
        return false;
    if (before == key)
        return true;
    if (before && std::find(to->tabs.begin(), to->tabs.end(), before) == to->tabs.end())
        return false;
    resize_.reset();
    remove(*from, key);
    if (side == DockSide::tabs) {
        const auto at = before ? std::find(to->tabs.begin(), to->tabs.end(), before) : to->tabs.end();
        to->tabs.insert(at, key);
        to->active = key;
    } else {
        // Preserve the old leaf key and all content keys while inserting a split above it.
        auto previous = make_node();
        std::swap(*previous, *to);
        auto added = make_node();
        added->tabs = {key};
        added->active = key;
        to->axis = side == DockSide::left || side == DockSide::right ? DragAxis::horizontal : DragAxis::vertical;
        to->fraction = fraction;
        const bool leading = side == DockSide::left || side == DockSide::top;
        to->first = leading ? std::move(added) : std::move(previous);
        to->second = leading ? std::move(previous) : std::move(added);
    }
    normalize(root_);
    maximized_ = 0;
    reveal(key);
    focused_ = key;
    return true;
}

ui_layout::Size DockSpace::minimum(const Node& node) const {
    if (node.split()) {
        auto a = node.collapsed == 1 ? ui_layout::Size{} : minimum(*node.first);
        auto b = node.collapsed == 2 ? ui_layout::Size{} : minimum(*node.second);
        return node.axis == DragAxis::horizontal
                   ? ui_layout::Size{a.width + b.width + divider_width, std::max(a.height, b.height)}
                   : ui_layout::Size{std::max(a.width, b.width), a.height + b.height + divider_width};
    }
    ui_layout::Size size{120, 60};
    for (auto key : node.tabs) {
        const auto& value = tab(key)->minimum;
        size.width = std::max(size.width, value.width);
        size.height = std::max(size.height, value.height);
    }
    return size;
}

ControlKey DockSpace::divider(ControlKey key, DragAxis axis) const noexcept {
    const auto leaf = pane(key);
    const auto visit = [&](auto&& self, const Node* node) -> std::pair<bool, ControlKey> {
        if (!node->split())
            return {node->key == leaf, 0};
        auto child = self(self, node->first.get());
        if (!child.first)
            child = self(self, node->second.get());
        if (child.first && !child.second && node->axis == axis)
            child.second = node->key + 1;
        return child;
    };
    return visit(visit, root_.get()).second;
}

std::optional<DockSpace::Drop> DockSpace::destination(ControlKey source, const DragPoint& point,
                                                      const UiFrame& frame) const {
    const auto* from = owner(source);
    if (!from)
        return {};
    const auto scale = frame.drawing->dpi_scale;
    const auto visit = [&](auto&& self, const Node& node) -> std::optional<Drop> {
        if (node.split()) {
            if (auto result = self(self, *node.first))
                return result;
            return self(self, *node.second);
        }
        const auto* box = frame.find(node.key);
        if (!box || !inside(box->bounds, point.x, point.y))
            return {};
        const auto b = box->bounds;
        Drop drop{node.key, 0, DockSide::tabs, {b.x / scale, b.y / scale, b.width / scale, b.height / scale}};
        const auto* strip = frame.find(node.key + 2);
        const float tab_height = strip ? strip->bounds.height / scale : 0.f;
        if (point.y < b.y + tab_height * scale) {
            for (auto key : node.tabs)
                if (key != source)
                    if (const auto* item = frame.find(key))
                        if (point.x < item->bounds.x + item->bounds.width * .5f) {
                            drop.before = key;
                            break;
                        }
            drop.preview.height = tab_height;
            return drop;
        }
        const float edge = std::min({48.f * scale, b.width * .22f, b.height * .22f});
        if (point.x < b.x + edge)
            drop.side = DockSide::left;
        else if (point.x > b.x + b.width - edge)
            drop.side = DockSide::right;
        else if (point.y < b.y + tab_height * scale + edge)
            drop.side = DockSide::top;
        else if (point.y > b.y + b.height - edge)
            drop.side = DockSide::bottom;
        if (drop.side != DockSide::tabs) {
            if (from == &node && from->tabs.size() == 1)
                return {};
            auto other = minimum(node);
            const auto own = tab(source)->minimum;
            if (drop.side == DockSide::left || drop.side == DockSide::right) {
                if (b.width / scale < other.width + own.width + divider_width)
                    return {};
                drop.preview.width = (drop.preview.width - divider_width) * .5f;
                if (drop.side == DockSide::right)
                    drop.preview.x += drop.preview.width + divider_width;
            } else {
                if (b.height / scale < other.height + own.height + divider_width)
                    return {};
                drop.preview.height = (drop.preview.height - divider_width) * .5f;
                if (drop.side == DockSide::bottom)
                    drop.preview.y += drop.preview.height + divider_width;
            }
        }
        return drop;
    };
    return visit(visit, *root_);
}

bool DockSpace::apply(const Action& action, const UiFrame& frame) {
    if (tab(action.key)) {
        if (action.kind == ActionKind::activate)
            return select(action.key);
        if (action.kind == ActionKind::drag_begin) {
            resize_.reset();
            drag_ = Drag{action.key, {}};
            return true;
        }
        if (action.kind == ActionKind::drag_cancel) {
            drag_.reset();
            return true;
        }
        if (action.kind == ActionKind::drag_preview || action.kind == ActionKind::drag_commit) {
            if (!drag_ || drag_->tab != action.key)
                return true;
            const auto* point = std::get_if<DragPoint>(&action.value);
            if (!point)
                return true;
            drag_->target = destination(action.key, *point, frame);
            if (action.kind == ActionKind::drag_commit) {
                const auto target = drag_->target;
                drag_.reset();
                if (target)
                    place(action.key, target->pane, target->side, target->before);
            }
            return true;
        }
        return false;
    }
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        if (action.kind == ActionKind::activate && action.key == base_ + close_offset + index)
            return close(tabs_[index].key);
        if (action.kind == ActionKind::activate && action.key == base_ + open_offset + index) {
            const auto key = tabs_[index].key;
            if (menu_type_) {
                auto* destination = find(menu_pane_);
                auto* origin = owner(key);
                if (!destination || destination->split() || destination == origin)
                    return select(key);
                const auto previous = destination->active;
                auto slot = std::find(destination->tabs.begin(), destination->tabs.end(), previous);
                if (slot == destination->tabs.end())
                    return select(key);
                if (origin) {
                    *std::find(origin->tabs.begin(), origin->tabs.end(), key) = previous;
                    if (origin->active == key)
                        origin->active = previous;
                } else {
                    std::erase(closed_tabs_, previous);
                    closed_tabs_.push_back(previous);
                }
                *slot = key;
                destination->active = key;
                std::erase(closed_tabs_, key);
                reveal(key);
                focused_ = key;
                return true;
            }
            // Registered views retain a single state/history when added to another group.
            if (menu_add_ && owner(key) && pane(key) != menu_pane_)
                return place(key, menu_pane_, DockSide::tabs);
            return select(key);
        }
    }
    if (action.kind == ActionKind::activate && action.key >= base_ + command_offset &&
        action.key < base_ + command_offset + 5) {
        auto* node = find(menu_pane_);
        if (!node || node->split())
            return true;
        const auto command = action.key - base_ - command_offset;
        if (command == 1 || command == 2) {
            focused_ = node->active;
            cycle(command == 1 ? -1 : 1);
        } else if (command == 3 && !closed_tabs_.empty()) {
            const auto key = closed_tabs_.back();
            select(key);
        } else if (command == 4) {
            const auto tabs = node->tabs;
            for (auto key : tabs)
                close(key);
        }
        return true;
    }
    if (action.key < base_ || action.key >= base_ + close_offset)
        return false;
    const auto key = base_ + (action.key - base_) / 16 * 16;
    auto* node = find(key);
    if (!node)
        return true;
    const auto offset = action.key - key;
    if (node->split()) {
        if (action.kind == ActionKind::activate) {
            resize_.reset();
            if (offset == 12)
                collapse(*node, 1);
            else if (offset == 13)
                collapse(*node, 2);
            else if (offset == 14) {
                const auto* rotate = std::get_if<bool>(&action.value);
                if (rotate && *rotate)
                    node->axis = node->axis == DragAxis::horizontal ? DragAxis::vertical : DragAxis::horizontal;
                else {
                    std::swap(node->first, node->second);
                    if (node->collapsed)
                        node->collapsed = 3 - node->collapsed;
                }
            }
        } else if (offset == 1 || offset == 14) {
            if (action.kind == ActionKind::edit_cancel) {
                if (resize_ && resize_->source == action.key)
                    resize_.reset();
                return true;
            }
            const auto* value = std::get_if<double>(&action.value);
            const auto* pane = frame.find(node->key);
            if (!value || !std::isfinite(*value) || !pane || node->collapsed)
                return true;
            if (action.kind == ActionKind::edit_begin) {
                drag_.reset();
                resize_ = Resize{action.key, {}};
            }
            if (!resize_ || resize_->source != action.key)
                return true;
            const auto scale = frame.drawing->dpi_scale;
            const bool horizontal = node->axis == DragAxis::horizontal;
            const auto a = minimum(*node->first);
            const auto b = minimum(*node->second);
            const auto range = split_range((horizontal ? pane->bounds.width : pane->bounds.height) / scale,
                                           horizontal ? a.width : a.height, horizontal ? b.width : b.height);
            const auto cut =
                std::clamp(range.span * static_cast<float>(std::clamp(*value, 0., 1.)), range.low, range.high);
            resize_->preview = {
                pane->bounds.x / scale + (horizontal ? cut : 0.f), pane->bounds.y / scale + (horizontal ? 0.f : cut),
                horizontal ? std::min(divider_width, pane->bounds.width / scale) : pane->bounds.width / scale,
                horizontal ? pane->bounds.height / scale : std::min(divider_width, pane->bounds.height / scale)};
            if (action.kind == ActionKind::edit_commit) {
                node->fraction = range.span ? cut / range.span : .5f;
                resize_.reset();
            }
        }
        return true;
    }
    if (action.kind == ActionKind::activate) {
        if (offset == 3 || offset == 6)
            maximize(node->active);
        else if (offset == 5)
            close(node->active);
        else if ((offset == 7 || offset == 8) && node->tabs.size() > 1)
            place(node->active, node->key, offset == 7 ? DockSide::right : DockSide::bottom);
        return true;
    }
    return false;
}

std::vector<MenuItem> DockSpace::menu_items(ControlKey source, const UiFrame* frame) {
    Node* node = owner(source);
    if (!node && source >= base_ && source < base_ + close_offset)
        node = find(base_ + (source - base_) / 16 * 16);
    if (!node || node->split())
        return {};
    menu_pane_ = node->key;
    menu_add_ = source == add_tab_button(node->key);
    menu_type_ = !menu_add_;
    std::vector<MenuItem> types;
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        types.push_back({base_ + open_offset + index, tabs_[index].title, true, node->active == tabs_[index].key, {}});
        types.back().behavior = MenuBehavior::radio;
        types.back().radio_group = node->key;
    }
    if (menu_add_)
        return types;
    // Choosing a registered view moves its existing state to this pane.
    std::vector<MenuItem> result;
    result.push_back({base_ + command_offset, L"Set Pane Tab Type", true, false, {}, L"", false, std::move(types)});
    result.push_back(
        {base_ + command_offset + 1, L"Previous Pane Tab", node->tabs.size() > 1, false, {}, L"Ctrl+Shift+Tab"});
    result.push_back({base_ + command_offset + 2, L"Next Pane Tab", node->tabs.size() > 1, false, {}, L"Ctrl+Tab"});
    result.push_back({node->key + 5, L"Close Pane Tab", node->active != 0, false, {}, L"Ctrl+W", true});
    result.push_back({base_ + command_offset + 3, L"Undo Close Tab", !closed_tabs_.empty(), false, {}});
    result.push_back({node->key + 6,
                      maximized_ ? L"Restore Pane" : L"Maximize Pane",
                      node->active != 0,
                      false,
                      {},
                      L"Ctrl+B",
                      true});
    result.push_back({base_ + command_offset + 4, L"Close Pane (All Tabs)", node->active != 0, false, {}});
    const auto* bounds = frame ? frame->find(node->key) : nullptr;
    const auto need = minimum(*node);
    const auto own = tab(node->active) ? tab(node->active)->minimum : ui_layout::Size{};
    const bool horizontal = node->tabs.size() > 1 && bounds &&
                            bounds->bounds.width / frame->drawing->dpi_scale >= need.width + own.width + divider_width;
    const bool vertical = node->tabs.size() > 1 && bounds &&
                          bounds->bounds.height / frame->drawing->dpi_scale >= need.height + own.height + divider_width;
    result.push_back({node->key + 7, L"Split Pane Left/Right", horizontal, false, {}, L"", true});
    result.push_back({node->key + 8, L"Split Pane Top/Bottom", vertical, false, {}});
    return result;
}

void DockSpace::present_node(Presentation& result, const Node& node, ControlKey parent, Rect bounds) const {
    Pane pane;
    pane.key = node.key;
    pane.parent = parent;
    pane.bounds = bounds;
    pane.split = node.split();
    pane.horizontal = node.axis == DragAxis::horizontal;
    pane.maximized = maximized_ != 0;
    pane.collapsed = node.collapsed;
    if (!node.split()) {
        for (auto key : node.tabs) {
            const auto* view = tab(key);
            const auto index = static_cast<ControlKey>(view - tabs_.data());
            pane.tabs.push_back({view, base_ + wrapper_offset + index, base_ + close_offset + index});
        }
        pane.active = tab(node.active);
        result.panes.push_back(std::move(pane));
        return;
    }
    const bool horizontal = pane.horizontal;
    const auto a = minimum(*node.first);
    const auto b = minimum(*node.second);
    const auto range = split_range(horizontal ? bounds.width : bounds.height, horizontal ? a.width : a.height,
                                   horizontal ? b.width : b.height);
    const auto span = range.span;
    const auto floor = range.low;
    const auto ceiling = range.high;
    const auto cut = node.collapsed == 1   ? 0.f
                     : node.collapsed == 2 ? span
                                           : std::clamp(span * node.fraction, floor, ceiling);
    pane.value = span ? cut / span : .5f;
    pane.minimum = span ? floor / span : .5f;
    pane.maximum = span ? ceiling / span : .5f;
    pane.step = 1 / std::max(span, 1.f);
    if (node.collapsed)
        pane.value = pane.minimum = pane.maximum = node.fraction;
    pane.divider = {horizontal ? cut : 0.f, horizontal ? 0.f : cut,
                    horizontal ? std::min(divider_width, bounds.width) : bounds.width,
                    horizontal ? bounds.height : std::min(divider_width, bounds.height)};
    result.panes.push_back(std::move(pane));
    Rect first{0, 0, bounds.width, bounds.height};
    Rect second = first;
    (horizontal ? first.width : first.height) = cut;
    (horizontal ? second.x : second.y) = cut + divider_width;
    (horizontal ? second.width : second.height) = std::max(0.f, span - cut);
    if (node.collapsed != 1)
        present_node(result, *node.first, node.key, first);
    if (node.collapsed != 2)
        present_node(result, *node.second, node.key, second);
}

DockSpace::Presentation DockSpace::presentation(ControlKey parent, ui_layout::Size size) const {
    Presentation result;
    result.preview_key = base_ + preview_offset;
    auto* node = maximized_ ? find(maximized_) : root_.get();
    if (!node)
        node = root_.get();
    present_node(result, *node, parent, {0, 0, size.width, size.height});
    if (drag_ && drag_->target)
        result.preview = drag_->target->preview;
    if (resize_)
        result.preview = resize_->preview;
    return result;
}
} // namespace atelier::ui
