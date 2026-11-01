// Included inside LayoutDocument::Impl. Flex uses the document's transaction,
// explicit traversal, measurement cache and reusable allocation scratch.
struct FlexItem {
    std::uint32_t node{};
    double basis{}, minimum{}, maximum{std::numeric_limits<double>::infinity()};
    double grow{}, shrink{}, padding{}, target{};
};
struct FlexLine {
    std::size_t begin{}, end{};
    double main{}, cross{}, ascent{}, descent{};
    double allocated_pool{-1};
    bool baseline{};
};
struct FlexState {
    std::size_t begin{}, end{}, lines_begin{}, lines_end{};
    bool enabled{}, row{};
};
Buffer<FlexItem> flex_items{BufferAllocator<FlexItem>{&memory}};
Buffer<FlexLine> flex_lines{BufferAllocator<FlexLine>{&memory}};
Buffer<FlexState> flex_states{BufferAllocator<FlexState>{&memory}};
Buffer<double> flex_weights{BufferAllocator<double>{&memory}};

void prepare_flex_work() {
    flex_items.clear(); flex_lines.clear();
    if (!has_flex_work) { flex_states.clear(); return; }
    flex_states.assign(work.size(), {});
    for (const auto index : order) {
        const auto& input = slots[index].input;
        if (input.kind == Kind::row || input.kind == Kind::column) {
            auto& state = flex_states[slots[index].work_index];
            state.row = input.kind == Kind::row;
            state.enabled = input.flow.has_value();
        }
    }
    // Each live edge is inspected once. Every later is_flex query is constant time.
    for (const auto index : order) {
        if (index == 0) continue;
        const auto parent = slots[index].parent;
        const auto& owner = slots[parent].input;
        if ((owner.kind == Kind::row || owner.kind == Kind::column) &&
            in_flow(slots[index].input, owner) &&
            (owner.kind == Kind::row ? slots[index].input.width : slots[index].input.height).rule == SizeRule::flex)
            flex_states[slots[parent].work_index].enabled = true;
    }
}
bool is_flex(std::uint32_t index) const {
    return !flex_states.empty() && flex_states[slots[index].work_index].enabled;
}
FlexSpec flex_spec(std::uint32_t index) const { return slots[index].input.flow.value_or(FlexSpec{}); }
Result<void> flex_dependency(std::uint32_t index) const {
    return std::unexpected(Error{ErrorCode::unsupported_dependency, id(index), 0, InputField::flow});
}
std::optional<double> flex_height_limit(std::uint32_t index) const {
    const auto& input = slots[index].input;
    if (index == 0) return std::max(0.0, static_cast<double>(working[slots[index].work_index].height) - vertical(input.padding));
    if (input.height.rule == SizeRule::fixed)
        return std::max(0.0, static_cast<double>(input.height.value) - vertical(input.padding));
    return {};
}
static double flex_bound(double value, const FlexItem& item) {
    return std::clamp(value, item.minimum, item.maximum);
}
FlexItem flex_item(std::uint32_t node, const Axis& axis, double natural, double padding) const {
    FlexItem item; item.node = node; item.padding = padding;
    item.minimum = std::max<double>(axis.minimum, padding);
    if (axis.maximum) item.maximum = std::max<double>(*axis.maximum, item.minimum);
    switch (axis.rule) {
    case SizeRule::fixed: item.basis = axis.value; break;
    case SizeRule::content: item.basis = natural; break;
    case SizeRule::expand: item.grow = axis.value; break;
    case SizeRule::flex:
        item.basis = axis.basis.natural ? natural : axis.basis.value;
        item.grow = axis.grow_weight;
        item.shrink = axis.shrink_weight;
        break;
    }
    item.target = flex_bound(item.basis, item);
    return item;
}

// Solve clamp(basis +/- lambda * weight, minimum, maximum). Bounds are not
// prepaid into the basis. Event slopes use the same balanced sums as Expand so
// tiny live weights survive after very large weights reach their caps.
void flex_allocate(std::size_t begin, std::size_t end, double pool) {
    const auto count = end - begin;
    if (!count) return;
    double initial = 0;
    bool unclamped = true;
    for (auto at = begin; at < end; ++at) {
        auto& item = flex_items[at]; item.target = flex_bound(item.basis, item);
        initial += item.target;
        unclamped = unclamped && item.target == item.basis;
    }
    if (initial == pool) return;
    const bool grow = pool > initial;
    double total_weight = 0;
    for (auto at = begin; at < end; ++at) {
        const auto& item = flex_items[at];
        total_weight += grow ? item.grow : item.shrink * std::max(0.0, item.basis - item.padding);
    }
    if (total_weight == 0) return;
    if (unclamped) {
        const double level = (pool - initial) / total_weight;
        bool fits = true;
        for (auto at = begin; at < end; ++at) {
            const auto& item = flex_items[at];
            const double weight = grow ? item.grow : item.shrink * std::max(0.0, item.basis - item.padding);
            const double target = item.basis + level * weight;
            // A rounded collision with a bound can hide residual space for
            // smaller weights. Nonparticipating items need no saturation event.
            if (weight > 0 && (target <= item.minimum || target >= item.maximum)) { fits = false; break; }
        }
        if (fits) {
            for (auto at = begin; at < end; ++at) {
                auto& item = flex_items[at];
                const double weight = grow ? item.grow : item.shrink * std::max(0.0, item.basis - item.padding);
                item.target = item.basis + level * weight;
            }
            return;
        }
    }
    flex_weights.assign(count, 0); weight_sums.assign(count * 2, 0); events.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const auto& item = flex_items[begin + i];
        const double weight = grow ? item.grow : item.shrink * std::max(0.0, item.basis - item.padding);
        flex_weights[i] = weight;
        if (weight == 0 || item.minimum == item.maximum) continue;
        const double start = grow ? (item.minimum - item.basis) / weight : (item.basis - item.maximum) / weight;
        const double stop = grow ? (item.maximum - item.basis) / weight : (item.basis - item.minimum) / weight;
        if (stop <= 0) continue;
        if (start > 0) events.emplace_back(start, i * 2);
        else weight_sums[count + i] = weight;
        if (std::isfinite(stop)) events.emplace_back(stop, i * 2 + 1);
    }
    for (auto i = count; i-- > 1;) weight_sums[i] = weight_sums[i * 2] + weight_sums[i * 2 + 1];
    std::sort(events.begin(), events.end());
    double remaining = std::abs(pool - initial), level = 0, slope = weight_sums[1];
    std::size_t at = 0;
    while (at < events.size()) {
        const double next = events[at].first, change = (next - level) * slope;
        if (slope > 0 && remaining <= change) { level += remaining / slope; remaining = 0; break; }
        remaining -= change; level = next;
        while (at < events.size() && events[at].first == next) {
            const auto event = events[at++].second;
            auto position = count + event / 2;
            weight_sums[position] = (event & 1) ? 0 : flex_weights[event / 2];
            for (position /= 2; position; position /= 2)
                weight_sums[position] = weight_sums[position * 2] + weight_sums[position * 2 + 1];
        }
        slope = weight_sums[1];
    }
    if (remaining > 0 && slope > 0) level += remaining / slope;
    for (std::size_t i = 0; i < count; ++i) {
        auto& item = flex_items[begin + i];
        item.target = flex_bound(item.basis + (grow ? 1 : -1) * level * flex_weights[i], item);
    }
}
void flex_build_lines(std::uint32_t index, double available, bool wrapping) {
    auto& state = flex_states[slots[index].work_index];
    state.lines_begin = flex_lines.size();
    for (auto at = state.begin; at < state.end; ++at) {
        const double size = flex_bound(flex_items[at].basis, flex_items[at]);
        const bool first = flex_lines.size() == state.lines_begin;
        if (first || (wrapping && flex_lines.back().main + slots[index].input.gap + size > available))
            flex_lines.push_back(FlexLine{at, at});
        auto& line = flex_lines.back();
        if (line.end != line.begin) line.main += slots[index].input.gap;
        line.main += size; line.end = at + 1;
    }
    state.lines_end = flex_lines.size();
}
double flex_allocate_line(std::size_t line_index, double available, float gap) {
    auto& line = flex_lines[line_index];
    const auto count = line.end - line.begin;
    const double gaps = count ? static_cast<double>(count - 1) * gap : 0;
    const auto pool = std::max(0.0, available - gaps);
    if (line.allocated_pool == pool) return line.main;
    flex_allocate(line.begin, line.end, pool);
    line.allocated_pool = pool;
    line.main = gaps;
    for (auto at = line.begin; at < line.end; ++at) line.main += flex_items[at].target;
    return line.main;
}
struct FlexSpacing { double start{}, between{}, stretch{}; };
static FlexSpacing flex_spacing(Distribution distribution, double free, std::size_t count) {
    FlexSpacing spacing;
    if (free <= 0 || count == 0) return spacing;
    switch (distribution) {
    case Distribution::start: break;
    case Distribution::center: spacing.start = free / 2; break;
    case Distribution::end: spacing.start = free; break;
    case Distribution::space_between: if (count > 1) spacing.between = free / static_cast<double>(count - 1); break;
    case Distribution::space_around: spacing.between = free / static_cast<double>(count); spacing.start = spacing.between / 2; break;
    case Distribution::space_evenly: spacing.between = spacing.start = free / static_cast<double>(count + 1); break;
    case Distribution::stretch: spacing.stretch = free / static_cast<double>(count); break;
    }
    return spacing;
}
Result<void> flex_widths(std::uint32_t index, float available) {
    auto& state = flex_states[slots[index].work_index];
    const auto& parent = slots[index].input;
    const auto spec = flex_spec(index);
    const bool wrapping = spec.wrap != Wrap::no_wrap;
    if (!state.row && wrapping && !flex_height_limit(index)) return flex_dependency(index);
    if (!state.row && wrapping && index != 0 &&
        (parent.width.rule == SizeRule::content || !alignment(index, true).fill)) return flex_dependency(index);
    state.begin = flex_items.size();
    for (const auto child : slots[index].children) {
        const auto& input = slots[child].input;
        if (!in_flow(input, parent)) continue;
        auto& child_work = work_for(child);
        child_work.needs_baseline = child_work.needs_baseline ||
            (state.row && alignment(child, false).first_baseline);
        // A definite slot can render text without asking layout for its unused
        // natural height. Wrapped rows still need cross sizes to form lines;
        // columns need content height only for a natural basis or natural parent.
        child_work.needs_height = input.height.rule != SizeRule::fixed &&
            (work_for(index).needs_height || child_work.needs_baseline ||
             input.height.rule == SizeRule::content || !alignment(child, false).fill ||
             (state.row ? wrapping : input.height.rule == SizeRule::flex && input.height.basis.natural));
        if (state.row) {
            if (input.height.rule == SizeRule::flex) return flex_dependency(child);
            double natural = 0;
            if (input.width.rule == SizeRule::content ||
                (input.width.rule == SizeRule::flex && input.width.basis.natural) || !alignment(child, true).fill) {
                ATELIER_TRY_BIND(measured_width, intrinsic_width(child));
                natural = *measured_width;
            }
            flex_items.push_back(flex_item(child, input.width, natural, horizontal(input.padding)));
        } else {
            if (input.width.rule == SizeRule::flex || (wrapping && input.width.rule != SizeRule::fixed && input.width.rule != SizeRule::content))
                return flex_dependency(child);
            double width = available;
            if (input.width.rule == SizeRule::fixed) width = input.width.value;
            else if (input.width.rule == SizeRule::content || !alignment(child, true).fill) {
                ATELIER_TRY_BIND(natural, intrinsic_width(child));
                width = input.width.rule == SizeRule::expand ? std::min<double>(available, *natural) : *natural;
            }
            ATELIER_TRY_BIND(result, constrained(width, input.width, horizontal(input.padding), child));
            box_for(child).width = *result;
            child_work.slot_width = wrapping ? *result : available;
            flex_items.push_back(flex_item(child, input.height, 0, vertical(input.padding)));
        }
    }
    state.end = flex_items.size();
    if (work_for(index).needs_baseline && state.begin != state.end) {
        const auto first = flex_items[state.begin].node;
        work_for(first).needs_baseline = true;
        if (slots[first].input.height.rule != SizeRule::fixed) work_for(first).needs_height = true;
    }
    if (!state.row) return {};
    flex_build_lines(index, available, wrapping);
    for (auto line = state.lines_begin; line < state.lines_end; ++line) {
        flex_allocate_line(line, available, parent.gap);
        for (auto at = flex_lines[line].begin; at < flex_lines[line].end; ++at) {
            const auto& item = flex_items[at]; const auto& input = slots[item.node].input;
            ATELIER_TRY_BIND(slot, coordinate(item.target, item.node));
            work_for(item.node).slot_width = *slot;
            double width = item.target;
            if (!alignment(item.node, true).fill && (input.width.rule == SizeRule::expand || input.width.rule == SizeRule::flex)) {
                ATELIER_TRY_BIND(natural, intrinsic_width(item.node));
                width = std::min<double>(width, *natural);
            }
            ATELIER_TRY_BIND(result, constrained(width, input.width, horizontal(input.padding), item.node));
            box_for(item.node).width = *result;
        }
    }
    return {};
}
Result<float> flex_heights(std::uint32_t index) {
    auto& state = flex_states[slots[index].work_index];
    const auto& input = slots[index].input;
    const auto spec = flex_spec(index);
    const bool wrapping = spec.wrap != Wrap::no_wrap;
    const auto height_limit = flex_height_limit(index);
    if (!state.row) {
        double natural = 0;
        for (auto at = state.begin; at < state.end; ++at) {
            const auto child = flex_items[at].node;
            flex_items[at] = flex_item(child, slots[child].input.height,
                work_for(child).natural_height, vertical(slots[child].input.padding));
            if (at != state.begin) natural += input.gap;
            natural += slots[child].input.height.rule == SizeRule::expand ?
                work_for(child).natural_height : flex_bound(flex_items[at].basis, flex_items[at]);
        }
        auto known_height = height_limit;
        if (!known_height && input.height.rule == SizeRule::content) {
            ATELIER_TRY_BIND(result, constrained(natural + vertical(input.padding), input.height,
                                                vertical(input.padding), index));
            known_height = std::max(0.0, static_cast<double>(*result) - vertical(input.padding));
        }
        flex_build_lines(index, known_height.value_or(natural), wrapping);
        for (auto line = state.lines_begin; line < state.lines_end; ++line) {
            auto& current = flex_lines[line];
            if (known_height) flex_allocate_line(line, *known_height, input.gap);
            for (auto at = current.begin; at < current.end; ++at)
                current.cross = std::max<double>(current.cross, box_for(flex_items[at].node).width);
        }
        return coordinate(wrapping ? height_limit.value_or(natural) : natural, index);
    }
    double natural = 0;
    for (auto line = state.lines_begin; line < state.lines_end; ++line) {
        auto& current = flex_lines[line];
        for (auto at = current.begin; at < current.end; ++at) {
            const auto child = flex_items[at].node;
            const auto& child_work = work_for(child);
            const double height = child_work.natural_height;
            if (alignment(child, false).first_baseline) {
                const double baseline = child_work.first_baseline.value_or(static_cast<float>(height));
                current.baseline = true; current.ascent = std::max(current.ascent, baseline);
                current.descent = std::max(current.descent, std::max(0.0, height - baseline));
            } else current.cross = std::max(current.cross, height);
        }
        current.cross = std::max(current.cross, current.ascent + current.descent);
        if (line != state.lines_begin) natural += input.cross_gap;
        natural += current.cross;
    }
    return coordinate(natural, index);
}
Result<void> flex_arrange(std::uint32_t index, float width, float height) {
    const auto& state = flex_states[slots[index].work_index];
    const auto& parent = slots[index].input;
    const auto spec = flex_spec(index);
    const bool wrapping = spec.wrap != Wrap::no_wrap;
    const double main_available = state.row ? width : height;
    const double cross_available = state.row ? height : width;
    double cross_total = 0;
    for (auto line = state.lines_begin; line < state.lines_end; ++line) {
        if (line != state.lines_begin) cross_total += parent.cross_gap;
        cross_total += flex_lines[line].cross;
        if (!state.row) flex_allocate_line(line, main_available, parent.gap);
    }
    const auto line_spacing = flex_spacing(spec.align_lines, cross_available - cross_total, state.lines_end - state.lines_begin);
    double cross_cursor = wrapping ? line_spacing.start : 0;
    for (auto line_index = state.lines_begin; line_index < state.lines_end; ++line_index) {
        const auto& line = flex_lines[line_index];
        const double cross_slot = wrapping ? line.cross + line_spacing.stretch : cross_available;
        const double cross_origin = spec.wrap == Wrap::wrap_reverse ? cross_available - cross_cursor - cross_slot : cross_cursor;
        const auto spacing = flex_spacing(spec.justify, main_available - line.main, line.end - line.begin);
        double main_cursor = spacing.start;
        for (auto at = line.begin; at < line.end; ++at) {
            const auto& item = flex_items[at];
            const auto child = item.node;
            auto& bounds = box_for(child);
            auto& child_work = work_for(child);
            const auto main_alignment = alignment(child, state.row);
            const auto cross_alignment = alignment(child, !state.row);
            if (state.row) {
                ATELIER_TRY_BIND(result, height_in_slot(child, cross_slot));
                bounds.height = *result;
                ATELIER_TRY_BIND(slot, coordinate(cross_slot, child));
                child_work.slot_height = *slot;
            } else {
                ATELIER_TRY_BIND(result, height_in_slot(child, item.target));
                bounds.height = *result;
                ATELIER_TRY_BIND(slot, coordinate(item.target, child));
                child_work.slot_height = *slot;
                ATELIER_TRY_BIND(cross, coordinate(cross_slot, child));
                child_work.slot_width = *cross;
            }
            const double main_size = state.row ? bounds.width : bounds.height;
            const double cross_size = state.row ? bounds.height : bounds.width;
            const double main_offset = std::max(0.0, item.target - main_size) * main_alignment.offset;
            const double main = spec.reverse ? main_available - main_cursor - item.target + main_offset : main_cursor + main_offset;
            double cross = cross_origin + std::max(0.0, cross_slot - cross_size) * cross_alignment.offset;
            if (state.row && cross_alignment.first_baseline)
                cross = cross_origin + line.ascent - child_work.first_baseline.value_or(bounds.height);
            auto x = coordinate(parent.padding.left + (state.row ? main : cross), child);
            auto y = coordinate(parent.padding.top + (state.row ? cross : main), child);
            ATELIER_TRY(x);
            ATELIER_TRY(y);
            bounds.x = *x; bounds.y = *y;
            main_cursor += item.target + parent.gap + spacing.between;
        }
        cross_cursor += cross_slot + parent.cross_gap + line_spacing.between;
    }
    return {};
}
