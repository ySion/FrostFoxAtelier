#include "layout.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>
#include <stdexcept>
#include <utility>

namespace atelier::ui_layout {
namespace {
constexpr auto no_slot = std::numeric_limits<std::uint32_t>::max();
constexpr auto no_pending = std::numeric_limits<std::size_t>::max();
std::atomic<std::uint64_t> next_document{1};

std::uint64_t document_identity() {
    auto value = next_document.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Layout document identities exhausted");
        if (next_document.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}
bool nonnegative(double value) { return std::isfinite(value) && value >= 0; }
bool container(Kind kind) { return kind == Kind::row || kind == Kind::column || kind == Kind::overlay; }
float horizontal(const Insets& padding) { return padding.left + padding.right; }
float vertical(const Insets& padding) { return padding.top + padding.bottom; }
bool valid_padding(const Insets& padding) {
    return nonnegative(padding.left) && nonnegative(padding.right) &&
        nonnegative(padding.top) && nonnegative(padding.bottom) &&
        nonnegative(horizontal(padding)) && nonnegative(vertical(padding));
}
bool valid_axis(const Axis& axis, float padding) {
    if (static_cast<unsigned>(axis.rule) > static_cast<unsigned>(SizeRule::expand) ||
        !nonnegative(axis.value) || !nonnegative(axis.minimum)) return false;
    if (axis.maximum && (!nonnegative(*axis.maximum) ||
        *axis.maximum < std::max(axis.minimum, padding))) return false;
    if (axis.rule == SizeRule::expand && axis.value == 0) return false;
    if (axis.rule == SizeRule::content && axis.value != 0) return false;
    return axis.rule != SizeRule::fixed || (axis.value >= std::max(axis.minimum, padding) &&
        (!axis.maximum || axis.value <= *axis.maximum));
}
std::optional<ErrorCode> validate(const NodeInput& input) {
    if (static_cast<unsigned>(input.kind) > static_cast<unsigned>(Kind::measured) ||
        !valid_padding(input.padding) || !nonnegative(input.gap) ||
        !valid_axis(input.width, horizontal(input.padding)) ||
        !valid_axis(input.height, vertical(input.padding))) return ErrorCode::invalid_input;
    if (input.gap != 0 && input.kind != Kind::row && input.kind != Kind::column)
        return ErrorCode::invalid_input;
    if (input.kind != Kind::measured && (input.content_key != 0 || input.content_revision != 0))
        return ErrorCode::invalid_input;
    for (const float value : {input.align_x, input.align_y, input.anchor_x, input.anchor_y,
         input.pivot_x, input.pivot_y, input.offset_x, input.offset_y})
        if (!std::isfinite(value)) return ErrorCode::invalid_input;
    if (input.align_x < 0 || input.align_x > 1 || input.align_y < 0 || input.align_y > 1)
        return ErrorCode::invalid_input;
    if (input.kind == Kind::overlay &&
        (input.width.rule == SizeRule::content || input.height.rule == SizeRule::content ||
         (input.width.rule == SizeRule::expand && !input.fill_width) ||
         (input.height.rule == SizeRule::expand && !input.fill_height)))
        return ErrorCode::unsupported_dependency;
    return {};
}
bool in_flow(const NodeInput& child, const NodeInput& parent) {
    return !child.out_of_flow && parent.kind != Kind::overlay;
}
struct Share { double minimum{}, weight{}; std::optional<double> maximum; };

// Private scratch is reused across containers and computes. Inputs have already
// passed the public float-domain checks. Events preserve each active weight;
// subtracting from a single rounded sum loses small weights after large caps.
void allocate_slots(std::span<const Share> shares, double pool, std::vector<double>& output,
                    std::vector<std::pair<double, std::size_t>>& events,
                    std::vector<double>& sums) {
    output.resize(shares.size());
    double minimum_sum = 0;
    for (std::size_t i = 0; i < shares.size(); ++i) {
        output[i] = shares[i].minimum;
        minimum_sum += shares[i].minimum;
    }
    if (shares.empty() || pool <= minimum_sum) return;
    events.clear();
    for (std::size_t i = 0; i < shares.size(); ++i) {
        events.emplace_back(shares[i].minimum / shares[i].weight, i * 2);
        if (shares[i].maximum)
            events.emplace_back(*shares[i].maximum / shares[i].weight, i * 2 + 1);
    }
    std::sort(events.begin(), events.end());
    const auto base = shares.size();
    sums.assign(base * 2, 0);
    double remaining = pool - minimum_sum, level = 0, slope = 0;
    std::size_t at = 0;
    while (at < events.size()) {
        const double next = events[at].first, growth = (next - level) * slope;
        if (slope > 0 && remaining <= growth) {
            level += remaining / slope;
            remaining = 0;
            break;
        }
        remaining -= growth;
        level = next;
        while (at < events.size() && events[at].first == next) {
            const auto event = events[at++].second;
            auto position = base + event / 2;
            sums[position] = (event & 1) ? 0 : shares[event / 2].weight;
            for (position /= 2; position != 0; position /= 2)
                sums[position] = sums[position * 2] + sums[position * 2 + 1];
        }
        slope = sums[1];
    }
    if (remaining > 0 && slope > 0) level += remaining / slope;
    for (std::size_t i = 0; i < shares.size(); ++i) {
        double value = std::max(shares[i].minimum, level * shares[i].weight);
        if (shares[i].maximum) value = std::min(value, *shares[i].maximum);
        output[i] = value;
    }
}
} // namespace

struct LayoutDocument::Impl {
    struct Cached {
        MeasureQuery query{};
        Size size{};
        bool valid{};
    };
    struct Slot {
        NodeInput input{};
        std::vector<std::uint32_t> children;
        Cached intrinsic{}, constrained{};
        std::uint32_t generation{1}, parent{no_slot}, next_free{no_slot}, depth{}, work_index{};
        bool live{};
    };
    struct Work {
        float intrinsic_width{}, natural_height{}, slot_width{}, slot_height{};
        std::size_t pending_intrinsic{no_pending}, pending_width{no_pending};
        bool intrinsic_valid{}, needs_height{};
    };
    struct Pending { std::uint32_t slot; MeasureQuery query; Size size; };

    Impl(Measure callback, void* user, Limits capacity)
        : identity(document_identity()), measure(callback), context(user), limits(capacity) {
        slots.emplace_back();
        slots[0].live = true;
        slots[0].input.kind = Kind::column;
    }
    LayoutNodeId id(std::uint32_t index) const { return {identity, index, slots[index].generation}; }
    bool valid(LayoutNodeId node) const {
        return node.document_ == identity && node.slot_ < slots.size() &&
            slots[node.slot_].live && slots[node.slot_].generation == node.generation_;
    }
    Result<void> editable() const {
        if (active) return std::unexpected(Error{ErrorCode::busy});
        return {};
    }
    Result<void> revision_available() const {
        if (input_revision == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(Error{ErrorCode::revision_exhausted});
        return {};
    }
    void reserve_child(std::uint32_t parent) {
        auto& children = slots[parent].children;
        if (children.size() == children.capacity())
            children.reserve(std::min<std::size_t>(limits.max_nodes,
                std::max<std::size_t>(4, children.capacity() * 2)));
    }
    void collect_subtree(std::uint32_t start) {
        stack.clear();
        stack.push_back(start);
        for (std::size_t i = 0; i < stack.size(); ++i)
            for (const auto child : slots[stack[i]].children) stack.push_back(child);
    }
    Work& work_for(std::uint32_t slot) { return work[slots[slot].work_index]; }
    LayoutBox& box_for(std::uint32_t slot) { return working[slots[slot].work_index]; }
    Result<float> coordinate(double value, std::uint32_t slot) const {
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            return std::unexpected(Error{ErrorCode::numeric_overflow, id(slot)});
        return static_cast<float>(value);
    }
    Result<float> constrained(double value, const Axis& axis, float padding, std::uint32_t slot) const {
        if (!std::isfinite(value))
            return std::unexpected(Error{ErrorCode::numeric_overflow, id(slot)});
        if (axis.maximum) value = std::min<double>(value, *axis.maximum);
        return coordinate(std::max<double>(std::max(axis.minimum, padding), value), slot);
    }
    Result<Size> measured(std::uint32_t index, std::optional<float> width) {
        auto& slot = slots[index];
        const MeasureQuery query{slot.input.content_key, slot.input.content_revision,
                                 current_provider, width};
        auto& staging_index = width ? work_for(index).pending_width : work_for(index).pending_intrinsic;
        if (staging_index != no_pending && pending[staging_index].query == query) {
            ++statistics.cache_hits;
            return pending[staging_index].size;
        }
        const auto& committed = width ? slot.constrained : slot.intrinsic;
        if (committed.valid && committed.query == query) {
            ++statistics.cache_hits;
            return committed.size;
        }
        ++statistics.measure_calls;
        if (width) ++statistics.width_queries; else ++statistics.intrinsic_queries;
        if (!measure) return std::unexpected(Error{ErrorCode::measurement_failed, id(index)});
        MeasureResult result = std::unexpected(MeasureError{});
        try { result = measure(context, query); }
        catch (const std::bad_alloc&) { throw; }
        catch (...) { return std::unexpected(Error{ErrorCode::measurement_failed, id(index)}); }
        if (!result)
            return std::unexpected(Error{ErrorCode::measurement_failed, id(index), result.error().code});
        if (!nonnegative(result->width) || !nonnegative(result->height))
            return std::unexpected(Error{ErrorCode::invalid_measurement, id(index)});
        pending.push_back({index, query, *result});
        staging_index = pending.size() - 1;
        return *result;
    }
    Result<float> intrinsic_width(std::uint32_t start) {
        stack.clear();
        stack.push_back(start);
        while (!stack.empty()) {
            const auto index = stack.back();
            const auto& slot = slots[index];
            auto& state = work_for(index);
            if (state.intrinsic_valid) { stack.pop_back(); continue; }
            if (slot.input.width.rule == SizeRule::fixed) {
                state.intrinsic_width = slot.input.width.value;
                state.intrinsic_valid = true;
                stack.pop_back();
                continue;
            }
            if (slot.input.kind == Kind::overlay)
                return std::unexpected(Error{ErrorCode::unsupported_dependency, id(index)});
            bool missing = false;
            for (const auto child : slot.children) {
                if (in_flow(slots[child].input, slot.input) && !work_for(child).intrinsic_valid) {
                    stack.push_back(child);
                    missing = true;
                }
            }
            if (missing) continue;
            const float padding = horizontal(slot.input.padding);
            double value = padding;
            std::size_t count = 0;
            if (slot.input.kind == Kind::measured) {
                auto size = measured(index, {});
                if (!size) return std::unexpected(size.error());
                value += size->width;
            } else {
                for (const auto child : slot.children) {
                    if (!in_flow(slots[child].input, slot.input)) continue;
                    if (slot.input.kind == Kind::row) {
                        value += work_for(child).intrinsic_width;
                        if (count++) value += slot.input.gap;
                    } else value = std::max(value, static_cast<double>(padding) + work_for(child).intrinsic_width);
                }
            }
            auto result = constrained(value, slot.input.width, padding, index);
            if (!result) return std::unexpected(result.error());
            state.intrinsic_width = *result;
            state.intrinsic_valid = true;
            stack.pop_back();
        }
        return work_for(start).intrinsic_width;
    }
    void prepare_work() {
        order.resize(live_count);
        work.assign(live_count, {});
        working.resize(live_count);
        pending.clear();
        stack.clear();
        stack.push_back(0);
        std::uint32_t at = 0;
        while (!stack.empty()) {
            const auto index = stack.back();
            stack.pop_back();
            order[at] = index;
            slots[index].work_index = at;
            working[at++] = {id(index), index ? id(slots[index].parent) : LayoutNodeId{}, 0, 0, 0, 0};
            const auto& children = slots[index].children;
            for (auto child = children.rbegin(); child != children.rend(); ++child) stack.push_back(*child);
        }
    }
    Result<void> resolve_widths(Size viewport) {
        box_for(0).width = viewport.width;
        for (const auto index : order) {
            ++statistics.node_visits;
            const auto& parent = slots[index].input;
            const float available = std::max(0.0f, box_for(index).width - horizontal(parent.padding));
            shares.clear();
            share_indices.clear();
            double fixed = 0;
            std::size_t count = 0;
            for (const auto child : slots[index].children) {
                const auto& input = slots[child].input;
                auto& state = work_for(child);
                state.needs_height = input.height.rule != SizeRule::fixed &&
                    (input.height.rule == SizeRule::content || !input.fill_height ||
                     (in_flow(input, parent) && work_for(index).needs_height));
                const bool shared = parent.kind == Kind::row && in_flow(input, parent);
                if (shared) {
                    if (count++) fixed += parent.gap;
                    if (input.width.rule == SizeRule::expand) {
                        shares.push_back({std::max(input.width.minimum, horizontal(input.padding)),
                            input.width.value, input.width.maximum ? std::optional<double>(*input.width.maximum) : std::nullopt});
                        share_indices.push_back(child);
                        continue;
                    }
                }
                double width = available;
                if (input.width.rule == SizeRule::fixed) width = input.width.value;
                else if (input.width.rule == SizeRule::content || !input.fill_width) {
                    auto natural = intrinsic_width(child);
                    if (!natural) return std::unexpected(natural.error());
                    width = input.width.rule == SizeRule::expand ? std::min(*natural, available) : *natural;
                }
                auto result = constrained(width, input.width, horizontal(input.padding), child);
                if (!result) return std::unexpected(result.error());
                box_for(child).width = *result;
                state.slot_width = shared ? *result : available;
                if (shared) fixed += state.slot_width;
            }
            allocate_slots(shares, std::max(0.0, static_cast<double>(available) - fixed), share_output, events, weight_sums);
            for (std::size_t i = 0; i < share_indices.size(); ++i) {
                const auto child = share_indices[i];
                const auto& input = slots[child].input;
                auto slot = coordinate(share_output[i], child);
                if (!slot) return std::unexpected(slot.error());
                work_for(child).slot_width = *slot;
                double width = *slot;
                if (!input.fill_width) {
                    auto natural = intrinsic_width(child);
                    if (!natural) return std::unexpected(natural.error());
                    width = std::min(width, static_cast<double>(*natural));
                }
                auto result = constrained(width, input.width, horizontal(input.padding), child);
                if (!result) return std::unexpected(result.error());
                box_for(child).width = *result;
            }
        }
        return {};
    }
    Result<void> resolve_heights() {
        for (auto entry = order.rbegin(); entry != order.rend(); ++entry) {
            ++statistics.node_visits;
            const auto index = *entry;
            const auto& slot = slots[index];
            auto& state = work_for(index);
            if (index == 0) continue; // Root height is authoritative viewport input.
            if (slot.input.height.rule == SizeRule::fixed) {
                state.natural_height = slot.input.height.value;
                continue;
            }
            if (!state.needs_height) continue;
            if (slot.input.kind == Kind::overlay)
                return std::unexpected(Error{ErrorCode::unsupported_dependency, id(index)});
            const float padding = vertical(slot.input.padding);
            double height = padding;
            if (slot.input.kind == Kind::measured) {
                auto size = measured(index, std::max(0.0f, box_for(index).width - horizontal(slot.input.padding)));
                if (!size) return std::unexpected(size.error());
                height += size->height;
            } else {
                std::size_t count = 0;
                for (const auto child : slot.children) {
                    if (!in_flow(slots[child].input, slot.input)) continue;
                    if (slot.input.kind == Kind::column) {
                        height += work_for(child).natural_height;
                        if (count++) height += slot.input.gap;
                    } else height = std::max(height, static_cast<double>(padding) + work_for(child).natural_height);
                }
            }
            auto result = constrained(height, slot.input.height, padding, index);
            if (!result) return std::unexpected(result.error());
            state.natural_height = *result;
        }
        return {};
    }
    Result<void> arrange(Size viewport) {
        box_for(0).height = viewport.height;
        for (const auto index : order) {
            ++statistics.node_visits;
            const auto& parent = slots[index].input;
            const float width = std::max(0.0f, box_for(index).width - horizontal(parent.padding));
            const float height = std::max(0.0f, box_for(index).height - vertical(parent.padding));
            shares.clear();
            share_indices.clear();
            double fixed = 0;
            std::size_t count = 0;
            for (const auto child : slots[index].children) {
                const auto& input = slots[child].input;
                const bool shared = parent.kind == Kind::column && in_flow(input, parent);
                if (shared) {
                    if (count++) fixed += parent.gap;
                    if (input.height.rule == SizeRule::expand) {
                        shares.push_back({std::max(input.height.minimum, vertical(input.padding)),
                            input.height.value, input.height.maximum ? std::optional<double>(*input.height.maximum) : std::nullopt});
                        share_indices.push_back(child);
                        continue;
                    }
                }
                double value = input.height.rule == SizeRule::fixed ? input.height.value :
                    (input.height.rule == SizeRule::content ? work_for(child).natural_height : height);
                if (input.height.rule == SizeRule::expand && !input.fill_height)
                    value = std::min(value, static_cast<double>(work_for(child).natural_height));
                auto result = constrained(value, input.height, vertical(input.padding), child);
                if (!result) return std::unexpected(result.error());
                box_for(child).height = *result;
                work_for(child).slot_height = shared ? *result : height;
                if (shared) fixed += work_for(child).slot_height;
            }
            allocate_slots(shares, std::max(0.0, static_cast<double>(height) - fixed), share_output, events, weight_sums);
            for (std::size_t i = 0; i < share_indices.size(); ++i) {
                const auto child = share_indices[i];
                const auto& input = slots[child].input;
                auto slot = coordinate(share_output[i], child);
                if (!slot) return std::unexpected(slot.error());
                work_for(child).slot_height = *slot;
                double value = *slot;
                if (!input.fill_height) value = std::min(value, static_cast<double>(work_for(child).natural_height));
                auto result = constrained(value, input.height, vertical(input.padding), child);
                if (!result) return std::unexpected(result.error());
                box_for(child).height = *result;
            }
            double cursor = 0;
            count = 0;
            for (const auto child : slots[index].children) {
                const auto& input = slots[child].input;
                auto& box = box_for(child);
                double x = parent.padding.left, y = parent.padding.top;
                if (!in_flow(input, parent)) {
                    x += static_cast<double>(width) * input.anchor_x - static_cast<double>(box.width) * input.pivot_x + input.offset_x;
                    y += static_cast<double>(height) * input.anchor_y - static_cast<double>(box.height) * input.pivot_y + input.offset_y;
                } else if (parent.kind == Kind::row) {
                    if (count++) cursor += parent.gap;
                    x += cursor + (static_cast<double>(work_for(child).slot_width) - box.width) * input.align_x;
                    y += (static_cast<double>(height) - box.height) * input.align_y;
                    cursor += work_for(child).slot_width;
                } else {
                    if (count++) cursor += parent.gap;
                    y += cursor + (static_cast<double>(work_for(child).slot_height) - box.height) * input.align_y;
                    x += (static_cast<double>(width) - box.width) * input.align_x;
                    cursor += work_for(child).slot_height;
                }
                auto left = coordinate(x, child), top = coordinate(y, child);
                if (!left) return std::unexpected(left.error());
                if (!top) return std::unexpected(top.error());
                box.x = *left;
                box.y = *top;
            }
        }
        return {};
    }
    LayoutView view() const noexcept {
        return {publication, published_input, published_provider, published_viewport, published};
    }

    const std::uint64_t identity;
    Measure measure;
    void* context;
    Limits limits;
    bool active{};
    std::uint32_t free_head{no_slot};
    std::size_t live_count{1};
    std::uint64_t input_revision{1}, publication{}, published_input{}, published_provider{}, current_provider{};
    Size published_viewport{};
    Stats statistics{};
    std::vector<Slot> slots;
    std::vector<std::uint32_t> stack, order, share_indices;
    std::vector<Work> work;
    std::vector<Pending> pending;
    std::vector<LayoutBox> working, published;
    std::vector<Share> shares;
    std::vector<double> share_output, weight_sums;
    std::vector<std::pair<double, std::size_t>> events;
};

LayoutDocument::LayoutDocument(Measure measure, void* context, Limits limits) {
    if (limits.max_nodes == 0 || limits.max_nodes == no_slot)
        throw std::invalid_argument("Layout max_nodes must include the root and fit slot identities");
    impl_ = std::make_unique<Impl>(measure, context, limits);
}
LayoutDocument::~LayoutDocument() = default;
LayoutNodeId LayoutDocument::root() const noexcept { return impl_->id(0); }
std::size_t LayoutDocument::node_count() const noexcept { return impl_->live_count; }
std::uint64_t LayoutDocument::input_revision() const noexcept { return impl_->input_revision; }

Result<void> LayoutDocument::set_root(const RootInput& input) {
    auto& d = *impl_;
    if (auto status = d.editable(); !status) return status;
    if (!container(input.kind) || !valid_padding(input.padding) || !nonnegative(input.gap) ||
        (input.kind == Kind::overlay && input.gap != 0))
        return std::unexpected(Error{ErrorCode::invalid_input, root()});
    auto& old = d.slots[0].input;
    if (old.kind == input.kind && old.padding == input.padding && old.gap == input.gap) return {};
    if (auto status = d.revision_available(); !status) return status;
    old.kind = input.kind;
    old.padding = input.padding;
    old.gap = input.gap;
    ++d.input_revision;
    return {};
}
Result<LayoutNodeId> LayoutDocument::create(LayoutNodeId parent, const NodeInput& input, std::size_t position) {
    auto& d = *impl_;
    if (auto status = d.editable(); !status) return std::unexpected(status.error());
    if (!d.valid(parent)) return std::unexpected(Error{ErrorCode::invalid_node, parent});
    if (!container(d.slots[parent.slot_].input.kind))
        return std::unexpected(Error{ErrorCode::invalid_structure, parent});
    if (auto error = validate(input)) return std::unexpected(Error{*error, parent});
    const auto child_count = d.slots[parent.slot_].children.size();
    if (position == append_position) position = child_count;
    if (position > child_count) return std::unexpected(Error{ErrorCode::invalid_input, parent});
    if (d.slots[parent.slot_].depth >= d.limits.max_depth ||
        (d.free_head == no_slot && d.slots.size() >= d.limits.max_nodes))
        return std::unexpected(Error{ErrorCode::capacity_exceeded, parent});
    if (auto status = d.revision_available(); !status) return std::unexpected(status.error());
    d.reserve_child(parent.slot_);
    const auto index = d.free_head == no_slot ? static_cast<std::uint32_t>(d.slots.size()) : d.free_head;
    if (d.free_head == no_slot) d.slots.emplace_back();
    else d.free_head = d.slots[index].next_free;
    auto& slot = d.slots[index];
    slot.input = input;
    slot.parent = parent.slot_;
    slot.depth = d.slots[parent.slot_].depth + 1;
    slot.next_free = no_slot;
    slot.live = true;
    auto& children = d.slots[parent.slot_].children;
    children.insert(children.begin() + static_cast<std::ptrdiff_t>(position), index);
    ++d.live_count;
    ++d.input_revision;
    return d.id(index);
}
Result<NodeInput> LayoutDocument::input(LayoutNodeId node) const {
    const auto& d = *impl_;
    if (!d.valid(node)) return std::unexpected(Error{ErrorCode::invalid_node, node});
    if (node.slot_ == 0) return std::unexpected(Error{ErrorCode::root_operation, node});
    return d.slots[node.slot_].input;
}
Result<void> LayoutDocument::update(LayoutNodeId node, const NodeInput& input) {
    auto& d = *impl_;
    if (auto status = d.editable(); !status) return status;
    if (!d.valid(node)) return std::unexpected(Error{ErrorCode::invalid_node, node});
    if (node.slot_ == 0) return std::unexpected(Error{ErrorCode::root_operation, node});
    if (auto error = validate(input)) return std::unexpected(Error{*error, node});
    auto& slot = d.slots[node.slot_];
    if (!container(input.kind) && !slot.children.empty())
        return std::unexpected(Error{ErrorCode::invalid_structure, node});
    if (slot.input == input) return {};
    if (auto status = d.revision_available(); !status) return status;
    slot.input = input;
    ++d.input_revision;
    return {};
}
Result<void> LayoutDocument::move(LayoutNodeId node, LayoutNodeId parent, std::size_t position) {
    auto& d = *impl_;
    if (auto status = d.editable(); !status) return status;
    if (!d.valid(node)) return std::unexpected(Error{ErrorCode::invalid_node, node});
    if (!d.valid(parent)) return std::unexpected(Error{ErrorCode::invalid_node, parent});
    if (node.slot_ == 0) return std::unexpected(Error{ErrorCode::root_operation, node});
    if (!container(d.slots[parent.slot_].input.kind))
        return std::unexpected(Error{ErrorCode::invalid_structure, parent});
    for (auto ancestor = parent.slot_; ancestor != no_slot; ancestor = d.slots[ancestor].parent)
        if (ancestor == node.slot_) return std::unexpected(Error{ErrorCode::invalid_structure, node});
    const auto old_parent = d.slots[node.slot_].parent;
    const auto destination_size = d.slots[parent.slot_].children.size() - (old_parent == parent.slot_ ? 1 : 0);
    if (position == append_position) position = destination_size;
    if (position > destination_size) return std::unexpected(Error{ErrorCode::invalid_input, parent});
    auto& source = d.slots[old_parent].children;
    const auto old_position = static_cast<std::size_t>(std::find(source.begin(), source.end(), node.slot_) - source.begin());
    if (old_parent == parent.slot_ && position == old_position) return {};
    if (auto status = d.revision_available(); !status) return status;
    d.collect_subtree(node.slot_);
    const auto delta = static_cast<std::int64_t>(d.slots[parent.slot_].depth) + 1 - d.slots[node.slot_].depth;
    for (const auto descendant : d.stack) {
        if (static_cast<std::int64_t>(d.slots[descendant].depth) + delta > d.limits.max_depth)
            return std::unexpected(Error{ErrorCode::capacity_exceeded, node});
    }
    if (old_parent != parent.slot_) d.reserve_child(parent.slot_);
    source.erase(source.begin() + static_cast<std::ptrdiff_t>(old_position));
    auto& destination = d.slots[parent.slot_].children;
    destination.insert(destination.begin() + static_cast<std::ptrdiff_t>(position), node.slot_);
    d.slots[node.slot_].parent = parent.slot_;
    for (const auto descendant : d.stack)
        d.slots[descendant].depth = static_cast<std::uint32_t>(static_cast<std::int64_t>(d.slots[descendant].depth) + delta);
    ++d.input_revision;
    return {};
}
Result<void> LayoutDocument::remove(LayoutNodeId node) {
    auto& d = *impl_;
    if (auto status = d.editable(); !status) return status;
    if (!d.valid(node)) return std::unexpected(Error{ErrorCode::invalid_node, node});
    if (node.slot_ == 0) return std::unexpected(Error{ErrorCode::root_operation, node});
    if (auto status = d.revision_available(); !status) return status;
    d.collect_subtree(node.slot_);
    auto& siblings = d.slots[d.slots[node.slot_].parent].children;
    siblings.erase(std::find(siblings.begin(), siblings.end(), node.slot_));
    for (const auto index : d.stack) {
        auto& slot = d.slots[index];
        slot.live = false;
        slot.parent = no_slot;
        slot.intrinsic = {};
        slot.constrained = {};
        std::vector<std::uint32_t>().swap(slot.children);
        if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
            ++slot.generation;
            slot.next_free = d.free_head;
            d.free_head = index;
        }
        // Exhausted generations are permanently retired, never wrapped/reused.
    }
    d.live_count -= d.stack.size();
    ++d.input_revision;
    return {};
}
Result<LayoutView> LayoutDocument::compute(Size viewport, std::uint64_t provider_revision) {
    auto& d = *impl_;
    if (d.active) return std::unexpected(Error{ErrorCode::busy});
    if (!nonnegative(viewport.width) || !nonnegative(viewport.height))
        return std::unexpected(Error{ErrorCode::invalid_input, root()});
    if (d.publication && d.published_input == d.input_revision &&
        d.published_provider == provider_revision && d.published_viewport == viewport) return d.view();
    if (d.publication == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::revision_exhausted});
    struct Activity {
        bool& active;
        explicit Activity(bool& value) : active(value) { active = true; }
        ~Activity() { active = false; }
    } activity(d.active);
    ++d.statistics.computes;
    d.current_provider = provider_revision;
    d.prepare_work();
    if (auto status = d.resolve_widths(viewport); !status) return std::unexpected(status.error());
    if (auto status = d.resolve_heights(); !status) return std::unexpected(status.error());
    if (auto status = d.arrange(viewport); !status) return std::unexpected(status.error());
    for (const auto& box : d.working) {
        if (!std::isfinite(box.x) || !std::isfinite(box.y) || !nonnegative(box.width) || !nonnegative(box.height))
            return std::unexpected(Error{ErrorCode::numeric_overflow, box.node});
    }
    // No allocations or callbacks beyond this point: cache and geometry commit
    // together. A failed attempt leaves both published values and caches intact.
    for (const auto& entry : d.pending) {
        auto& cached = entry.query.width ? d.slots[entry.slot].constrained : d.slots[entry.slot].intrinsic;
        cached = {entry.query, entry.size, true};
    }
    d.published.swap(d.working);
    ++d.publication;
    ++d.statistics.publications;
    d.published_input = d.input_revision;
    d.published_provider = provider_revision;
    d.published_viewport = viewport;
    return d.view();
}
LayoutView LayoutDocument::view() const noexcept { return impl_->view(); }
LayoutSnapshot LayoutDocument::snapshot() const {
    const auto current = view();
    return {current.publication, current.input_revision, current.provider_revision, current.viewport,
            std::vector<LayoutBox>(current.boxes.begin(), current.boxes.end())};
}
Stats LayoutDocument::stats() const noexcept { return impl_->statistics; }

} // namespace atelier::ui_layout
