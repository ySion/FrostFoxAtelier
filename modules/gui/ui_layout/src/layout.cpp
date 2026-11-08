#include "ui_layout/layout.hpp"
#include "foundation/result.hpp"
#include "memory/fatal.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <type_traits>
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
            fatal("Layout document identities exhausted");
        if (next_document.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}

bool nonnegative(double value) {
    return std::isfinite(value) && value >= 0;
}

bool container(Kind kind) {
    return kind == Kind::row || kind == Kind::column || kind == Kind::overlay || kind == Kind::table ||
           kind == Kind::wrap || kind == Kind::grid;
}

bool vertical_flow(Kind kind) {
    return kind == Kind::column || kind == Kind::table;
}

float horizontal(const Insets& padding) {
    return padding.left + padding.right;
}

float vertical(const Insets& padding) {
    return padding.top + padding.bottom;
}

bool valid_padding(const Insets& padding) {
    return nonnegative(padding.left) && nonnegative(padding.right) && nonnegative(padding.top) &&
           nonnegative(padding.bottom) && nonnegative(horizontal(padding)) && nonnegative(vertical(padding));
}

bool valid_axis(const Axis& axis, float padding) {
    if (static_cast<unsigned>(axis.rule) > static_cast<unsigned>(SizeRule::flex) || !nonnegative(axis.value) ||
        !nonnegative(axis.minimum))
        return false;
    if (!nonnegative(axis.basis.value) || (axis.basis.natural && axis.basis.value != 0) ||
        !nonnegative(axis.grow_weight) || !nonnegative(axis.shrink_weight))
        return false;
    if (axis.rule != SizeRule::flex && (axis.basis != Basis{} || axis.grow_weight != 0 || axis.shrink_weight != 0))
        return false;
    if (axis.rule == SizeRule::flex && axis.value != 0)
        return false;
    if (axis.maximum && (!nonnegative(*axis.maximum) || *axis.maximum < std::max(axis.minimum, padding)))
        return false;
    if (axis.rule == SizeRule::expand && axis.value == 0)
        return false;
    if (axis.rule == SizeRule::content && axis.value != 0)
        return false;
    return axis.rule != SizeRule::fixed ||
           (axis.value >= std::max(axis.minimum, padding) && (!axis.maximum || axis.value <= *axis.maximum));
}

bool valid_alignment(const Alignment& value) {
    return nonnegative(value.offset) && value.offset <= 1 &&
           (!value.first_baseline || (!value.fill && value.offset == 0)) && (!value.inherited || value == Alignment{});
}

bool valid_flow(const FlexSpec& value) {
    return static_cast<unsigned>(value.wrap) <= static_cast<unsigned>(Wrap::wrap_reverse) &&
           static_cast<unsigned>(value.justify) < static_cast<unsigned>(Distribution::stretch) &&
           static_cast<unsigned>(value.align_lines) <= static_cast<unsigned>(Distribution::stretch) &&
           valid_alignment(value.align_items) && !value.align_items.inherited;
}

std::optional<Error> validate(const NodeInput& input) {
    const auto invalid = [](InputField field) { return Error{ErrorCode::invalid_input, {}, 0, field}; };
    if (static_cast<unsigned>(input.kind) > static_cast<unsigned>(Kind::grid))
        return invalid(InputField::kind);
    if (!valid_padding(input.padding))
        return invalid(InputField::padding);
    if (!valid_axis(input.width, horizontal(input.padding)))
        return invalid(InputField::width);
    if (!valid_axis(input.height, vertical(input.padding)))
        return invalid(InputField::height);
    if (!nonnegative(input.gap) || !nonnegative(input.cross_gap) ||
        (input.gap != 0 && (!container(input.kind) || input.kind == Kind::overlay)) ||
        (input.cross_gap != 0 && input.kind != Kind::table && input.kind != Kind::wrap && input.kind != Kind::row &&
         input.kind != Kind::column))
        return invalid(InputField::gap);
    if (input.kind != Kind::measured && (input.content_key != 0 || input.content_revision != 0))
        return invalid(InputField::content);
    for (const float value :
         {input.anchor_x, input.anchor_y, input.pivot_x, input.pivot_y, input.offset_x, input.offset_y})
        if (!std::isfinite(value))
            return invalid(InputField::anchor);
    if (!valid_alignment(input.horizontal_alignment) || !valid_alignment(input.vertical_alignment) ||
        input.horizontal_alignment.first_baseline)
        return invalid(InputField::alignment);
    if ((input.kind == Kind::table) != !input.columns.empty())
        return invalid(InputField::columns);
    for (const auto& track : input.columns)
        if (!valid_axis(track, 0) || track.rule == SizeRule::flex)
            return invalid(InputField::columns);
    if (input.flow && ((input.kind != Kind::row && input.kind != Kind::column) || !valid_flow(*input.flow)))
        return invalid(InputField::flow);
    if (input.cross_gap != 0 && (input.kind == Kind::row || input.kind == Kind::column) && !input.flow)
        return invalid(InputField::gap);
    if (input.flow && input.kind == Kind::column && input.flow->align_items.first_baseline)
        return Error{ErrorCode::unsupported_dependency, {}, 0, InputField::alignment};
    if ((input.kind == Kind::grid) != input.grid_spec.has_value())
        return invalid(InputField::grid);
    if (input.grid_spec) {
        const auto& grid = *input.grid_spec;
        if (input.gap != 0 || input.cross_gap != 0 || !nonnegative(grid.column_gap) || !nonnegative(grid.row_gap) ||
            static_cast<unsigned>(grid.auto_flow) > static_cast<unsigned>(AutoFlow::column))
            return invalid(InputField::grid);
        for (const auto& tracks : {std::span<const Axis>(grid.columns), std::span<const Axis>(grid.rows)})
            for (const auto& track : tracks)
                if (!valid_axis(track, 0) || track.rule == SizeRule::flex)
                    return invalid(InputField::grid);
        for (const auto& track : {grid.implicit_column, grid.implicit_row})
            if (!valid_axis(track, 0) || track.rule == SizeRule::flex)
                return invalid(InputField::grid);
    }
    if (!input.cell.row_span || !input.cell.column_span)
        return invalid(InputField::cell);
    if (!nonnegative(input.item_width) || !nonnegative(input.line_align) || input.line_align > 1 ||
        (input.stretch_items && input.item_width == 0) ||
        (input.kind != Kind::wrap && (input.item_width != 0 || input.stretch_items || input.line_align != 0)))
        return invalid(InputField::wrap);
    if (input.kind == Kind::overlay &&
        (input.width.rule == SizeRule::content || input.height.rule == SizeRule::content ||
         input.width.rule == SizeRule::flex || input.height.rule == SizeRule::flex ||
         (input.width.rule == SizeRule::expand && !input.horizontal_alignment.fill) ||
         (input.height.rule == SizeRule::expand && !input.vertical_alignment.fill)))
        return Error{ErrorCode::unsupported_dependency, {}, 0, InputField::kind};
    return {};
}

bool in_flow(const NodeInput& child, const NodeInput& parent) {
    return !child.collapsed && !child.out_of_flow && parent.kind != Kind::overlay;
}

struct BufferMemory {
    std::uint64_t allocations{};

    std::uint64_t allocated{};

    std::uint64_t live{};

    std::uint64_t peak{};
};

// Counts the actual backing allocations of internal buffers, using the process allocator.
template <class T>
struct BufferAllocator {
    using value_type = T;

    BufferMemory* memory;

    explicit BufferAllocator(BufferMemory* value) noexcept : memory(value) {}

    template <class U>
    BufferAllocator(const BufferAllocator<U>& value) noexcept : memory(value.memory) {}

    T* allocate(std::size_t count) {
        auto* result = std::allocator<T>{}.allocate(count);
        const auto bytes = count * sizeof(T);
        ++memory->allocations;
        memory->allocated += bytes;
        memory->live += bytes;
        memory->peak = std::max(memory->peak, memory->live);
        return result;
    }

    void deallocate(T* value, std::size_t count) noexcept {
        memory->live -= count * sizeof(T);
        std::allocator<T>{}.deallocate(value, count);
    }

    template <class U>
    bool operator==(const BufferAllocator<U>& other) const noexcept {
        return memory == other.memory;
    }
};

template <class T>
using Buffer = std::vector<T, BufferAllocator<T>>;

struct Share {
    double minimum{};

    double weight{};

    std::optional<double> maximum;
};

// Private scratch is reused across containers and computes. Inputs have already
// passed the public float-domain checks. Events preserve each active weight;
// subtracting from a single rounded sum loses small weights after large caps.
void allocate_slots(std::span<const Share> shares, double pool, Buffer<double>& output,
                    Buffer<std::pair<double, std::size_t>>& events, Buffer<double>& sums) {
    output.resize(shares.size());
    double minimum_sum = 0;
    double total_weight = 0;
    for (std::size_t i = 0; i < shares.size(); ++i) {
        output[i] = shares[i].minimum;
        minimum_sum += shares[i].minimum;
        total_weight += shares[i].weight;
    }
    if (shares.empty() || pool <= minimum_sum)
        return;
    // Most Expand groups fit without saturating a bound. Keep them linear;
    // a violated bound still uses the balanced event solver below so tiny
    // weights remain available after much larger weights reach their caps.
    const double proportional_level = pool / total_weight;
    bool fits = true;
    for (std::size_t i = 0; i < shares.size(); ++i) {
        const double value = proportional_level * shares[i].weight;
        // Equality can be a rounded collision with a bound. The event solver
        // must preserve any residual pool for smaller weights after saturation.
        if (value <= shares[i].minimum || (shares[i].maximum && value >= *shares[i].maximum)) {
            fits = false;
            break;
        }
        output[i] = value;
    }
    if (fits)
        return;
    events.clear();
    for (std::size_t i = 0; i < shares.size(); ++i) {
        events.emplace_back(shares[i].minimum / shares[i].weight, i * 2);
        if (shares[i].maximum)
            events.emplace_back(*shares[i].maximum / shares[i].weight, i * 2 + 1);
    }
    std::sort(events.begin(), events.end());
    const auto base = shares.size();
    sums.assign(base * 2, 0);
    double remaining = pool - minimum_sum;
    double level = 0;
    double slope = 0;
    std::size_t at = 0;
    while (at < events.size()) {
        const double next = events[at].first;
        const double growth = (next - level) * slope;
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
    if (remaining > 0 && slope > 0)
        level += remaining / slope;
    for (std::size_t i = 0; i < shares.size(); ++i) {
        double value = std::max(shares[i].minimum, level * shares[i].weight);
        if (shares[i].maximum)
            value = std::min(value, *shares[i].maximum);
        output[i] = value;
    }
}
} // namespace

struct LayoutDocument::Impl {
    struct Cached {
        MeasureQuery query{};

        MeasuredSize size{};

        bool valid{};
    };

    struct Slot {
        explicit Slot(BufferMemory* memory) : children(BufferAllocator<std::uint32_t>{memory}) {}

        NodeInput input{};

        Buffer<std::uint32_t> children;

        Cached intrinsic{};

        Cached constrained{};

        std::uint32_t generation{1};

        std::uint32_t parent{no_slot};

        std::uint32_t next_free{no_slot};

        std::uint32_t depth{};

        std::uint32_t work_index{};

        bool live{};

        std::uint64_t measured_generation{};
    };

    struct Work {
        float intrinsic_width{};

        float natural_height{};

        float slot_width{};

        float slot_height{};

        std::size_t pending_intrinsic{no_pending};

        std::size_t pending_width{no_pending};

        bool intrinsic_valid{};

        bool needs_height{};

        bool uses_measurement{};

        bool needs_baseline{};

        bool children_arranged{};

        std::optional<float> first_baseline;

        std::size_t tracks{no_pending};

        std::size_t line{no_pending};

        std::size_t lines_begin{};

        std::size_t lines_end{};
    };

    struct Pending {
        std::uint32_t slot;

        MeasureQuery query;

        MeasuredSize size;
    };

    struct Line {
        double width{};

        double height{};

        double y{};

        double cursor{};

        std::size_t count{};
    };

    Impl(Measure callback, void* user, Limits capacity)
        : identity(document_identity()), measure(callback), context(user), limits(capacity),
          slots(BufferAllocator<Slot>{&memory}), stack(BufferAllocator<std::uint32_t>{&memory}),
          order(BufferAllocator<std::uint32_t>{&memory}), share_indices(BufferAllocator<std::uint32_t>{&memory}),
          work(BufferAllocator<Work>{&memory}), pending(BufferAllocator<Pending>{&memory}),
          working(BufferAllocator<LayoutBox>{&memory}), published(BufferAllocator<LayoutBox>{&memory}),
          working_index(BufferAllocator<std::uint32_t>{&memory}),
          published_index(BufferAllocator<std::uint32_t>{&memory}), shares(BufferAllocator<Share>{&memory}),
          share_output(BufferAllocator<double>{&memory}), weight_sums(BufferAllocator<double>{&memory}),
          events(BufferAllocator<std::pair<double, std::size_t>>{&memory}), tracks(BufferAllocator<float>{&memory}),
          lines(BufferAllocator<Line>{&memory}) {
        slots.emplace_back(&memory);
        slots[0].live = true;
        slots[0].input.kind = Kind::column;
    }

    LayoutNodeId id(std::uint32_t index) const { return {identity, index, slots[index].generation}; }

    bool valid(LayoutNodeId node) const {
        return node.document_ == identity && node.slot_ < slots.size() && slots[node.slot_].live &&
               slots[node.slot_].generation == node.generation_;
    }

    Result<void> editable() const {
        if (active)
            return std::unexpected(Error{ErrorCode::busy});
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
            children.reserve(
                std::min<std::size_t>(limits.max_nodes, std::max<std::size_t>(4, children.capacity() * 2)));
    }

    void collect_subtree(std::uint32_t start) {
        stack.clear();
        stack.push_back(start);
        for (std::size_t i = 0; i < stack.size(); ++i)
            for (const auto child : slots[stack[i]].children)
                stack.push_back(child);
    }

    bool retire(std::uint32_t index, bool release_children = true) noexcept {
        auto& slot = slots[index];
        slot.live = false;
        slot.parent = slot.next_free = no_slot;
        slot.intrinsic = {};
        slot.constrained = {};
        slot.measured_generation = 0;
        slot.children.clear();
        if (release_children)
            Buffer<std::uint32_t>(slot.children.get_allocator()).swap(slot.children);
        slot.input = {};
        // Exhausted generations permanently retire; a free slot at MAX still
        // has one remaining lifetime when its preceding removal advanced to MAX.
        if (slot.generation == std::numeric_limits<std::uint32_t>::max())
            return false;
        ++slot.generation;
        return true;
    }

    Work& work_for(std::uint32_t slot) { return work[slots[slot].work_index]; }

    LayoutBox& box_for(std::uint32_t slot) { return working[slots[slot].work_index]; }

    Alignment alignment(std::uint32_t index, bool horizontal_axis) const {
        const auto& slot = slots[index];
        const auto& value = horizontal_axis ? slot.input.horizontal_alignment : slot.input.vertical_alignment;
        if (!value.inherited)
            return value;
        if (slot.parent != no_slot) {
            const auto& parent = slots[slot.parent].input;
            if (parent.flow && (horizontal_axis ? parent.kind == Kind::column : parent.kind == Kind::row))
                return parent.flow->align_items;
        }
        return Alignment::stretch();
    }

    Result<float> coordinate(double value, std::uint32_t slot) const {
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            return std::unexpected(Error{ErrorCode::numeric_overflow, id(slot)});
        return static_cast<float>(value);
    }

    Result<float> constrained(double value, const Axis& axis, float padding, std::uint32_t slot) const {
        if (!std::isfinite(value))
            return std::unexpected(Error{ErrorCode::numeric_overflow, id(slot)});
        if (axis.maximum)
            value = std::min<double>(value, *axis.maximum);
        return coordinate(std::max<double>(std::max(axis.minimum, padding), value), slot);
    }

    Result<MeasuredSize> measured(std::uint32_t index, std::optional<float> width) {
        work_for(index).uses_measurement = true;
        auto& slot = slots[index];
        const MeasureQuery query{slot.input.content_key, slot.input.content_revision, current_provider, width,
                                 width.has_value() && work_for(index).needs_baseline};
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
        if (width)
            ++statistics.width_queries;
        else
            ++statistics.intrinsic_queries;
        if (!measure)
            return std::unexpected(Error{ErrorCode::measurement_failed, id(index)});
        const auto result = measure(context, query);
        if (!result)
            return std::unexpected(Error{ErrorCode::measurement_failed, id(index), result.error().code});
        if (!nonnegative(result->width) || !nonnegative(result->height) ||
            (result->first_baseline &&
             (!nonnegative(*result->first_baseline) || *result->first_baseline > result->height)))
            return std::unexpected(Error{ErrorCode::invalid_measurement, id(index)});
        pending.push_back({index, query, *result});
        staging_index = pending.size() - 1;
        return *result;
    }

    std::uint32_t table_for_row(std::uint32_t index) const {
        const auto& row = slots[index];
        return row.parent != no_slot && slots[row.parent].input.kind == Kind::table &&
                       in_flow(row.input, slots[row.parent].input)
                   ? row.parent
                   : no_slot;
    }

    Result<void> validate_rows(std::uint32_t index) const {
        const auto& table = slots[index];
        for (const auto row_index : table.children) {
            const auto& row = slots[row_index];
            if (!in_flow(row.input, table.input))
                continue;
            if (row.input.kind != Kind::row || row.input.width != Axis::expand() || !alignment(row_index, true).fill ||
                horizontal(row.input.padding) != 0 || row.input.gap != 0 || row.input.flow)
                return std::unexpected(
                    Error{ErrorCode::unsupported_dependency, id(row_index), 0, InputField::table_row});
            std::size_t cells = 0;
            for (const auto cell : row.children)
                if (!slots[cell].input.out_of_flow)
                    ++cells;
            if (cells > table.input.columns.size())
                return std::unexpected(Error{ErrorCode::invalid_structure, id(row_index), 0, InputField::table_row});
        }
        return {};
    }

    Result<float> cell_width(std::uint32_t index, double slot_width) {
        const auto& input = slots[index].input;
        double width = slot_width;
        if (input.width.rule == SizeRule::fixed)
            width = input.width.value;
        else if (input.width.rule == SizeRule::content || !alignment(index, true).fill) {
            ATELIER_TRY_BIND(natural, intrinsic_width(index));
            width = input.width.rule == SizeRule::content ? *natural : std::min<double>(slot_width, *natural);
        }
        return constrained(width, input.width, horizontal(input.padding), index);
    }

    Result<void> resolve_tracks(std::uint32_t index, float available) {
        ATELIER_TRY(validate_rows(index));
        const auto& table = slots[index];
        const auto offset = tracks.size();
        work_for(index).tracks = offset;
        tracks.resize(offset + table.input.columns.size(), 0);
        // Scan each row once; only Content columns need intrinsic measurements.
        for (const auto row : table.children) {
            if (!in_flow(slots[row].input, table.input))
                continue;
            std::size_t column = 0;
            for (const auto cell : slots[row].children) {
                const auto& input = slots[cell].input;
                if (input.out_of_flow)
                    continue;
                const auto at = column++;
                if (input.collapsed || table.input.columns[at].rule != SizeRule::content)
                    continue;
                ATELIER_TRY_BIND(natural, intrinsic_width(cell));
                tracks[offset + at] = std::max(tracks[offset + at], *natural);
            }
        }
        shares.clear();
        share_indices.clear();
        double fixed = static_cast<double>(table.input.cross_gap) * (table.input.columns.size() - 1);
        for (std::size_t column = 0; column < table.input.columns.size(); ++column) {
            const auto& rule = table.input.columns[column];
            if (rule.rule == SizeRule::expand) {
                shares.push_back(
                    {rule.minimum, rule.value, rule.maximum ? std::optional<double>(*rule.maximum) : std::nullopt});
                share_indices.push_back(static_cast<std::uint32_t>(column));
            } else {
                ATELIER_TRY_BIND(size, constrained(rule.rule == SizeRule::fixed ? rule.value : tracks[offset + column],
                                                   rule, 0, index));
                tracks[offset + column] = *size;
                fixed += *size;
            }
        }
        allocate_slots(shares, std::max(0.0, static_cast<double>(available) - fixed), share_output, events,
                       weight_sums);
        for (std::size_t i = 0; i < share_indices.size(); ++i) {
            ATELIER_TRY_BIND(size, coordinate(share_output[i], index));
            tracks[offset + share_indices[i]] = *size;
        }
        return {};
    }

    Result<float> intrinsic_width(std::uint32_t start) {
        stack.clear();
        stack.push_back(start);
        while (!stack.empty()) {
            const auto index = stack.back();
            const auto& slot = slots[index];
            auto& state = work_for(index);
            if (state.intrinsic_valid) {
                stack.pop_back();
                continue;
            }
            if (slot.input.width.rule == SizeRule::fixed ||
                (slot.input.width.rule == SizeRule::flex && !slot.input.width.basis.natural &&
                 alignment(index, true).fill)) {
                auto size = constrained(slot.input.width.rule == SizeRule::fixed ? slot.input.width.value
                                                                                 : slot.input.width.basis.value,
                                        slot.input.width, horizontal(slot.input.padding), index);
                if (!size)
                    return std::unexpected(size.error());
                state.intrinsic_width = *size;
                state.intrinsic_valid = true;
                stack.pop_back();
                continue;
            }
            if (slot.input.kind == Kind::overlay)
                return std::unexpected(Error{ErrorCode::unsupported_dependency, id(index)});
            bool missing = false;
            if (slot.input.kind == Kind::grid) {
                ATELIER_TRY(prepare_grid(index));
                for (const auto child : slot.children)
                    if (in_flow(slots[child].input, slot.input) && grid_intrinsic_required(child) &&
                        !work_for(child).intrinsic_valid) {
                        stack.push_back(child);
                        missing = true;
                    }
            } else if (slot.input.kind == Kind::table) {
                ATELIER_TRY(validate_rows(index));
                for (const auto row : slot.children) {
                    if (!in_flow(slots[row].input, slot.input))
                        continue;
                    std::size_t column = 0;
                    for (const auto cell : slots[row].children) {
                        const auto& input = slots[cell].input;
                        if (input.out_of_flow)
                            continue;
                        const auto at = column++;
                        if (!input.collapsed && slot.input.columns[at].rule != SizeRule::fixed &&
                            !work_for(cell).intrinsic_valid) {
                            stack.push_back(cell);
                            missing = true;
                        }
                    }
                }
            } else if (slot.input.kind != Kind::wrap || slot.input.item_width == 0)
                for (const auto child : slot.children) {
                    if (in_flow(slots[child].input, slot.input) && !work_for(child).intrinsic_valid) {
                        stack.push_back(child);
                        missing = true;
                    }
                }
            if (missing)
                continue;
            const float padding = horizontal(slot.input.padding);
            double value = padding;
            std::size_t count = 0;
            if (slot.input.kind == Kind::measured) {
                ATELIER_TRY_BIND(size, measured(index, {}));
                value += size->width;
            } else if (slot.input.kind == Kind::grid) {
                ATELIER_TRY_BIND(size, grid_intrinsic(index));
                value += *size;
            } else if (slot.input.kind == Kind::table) {
                // The scratch track buffer is also safe here: no reentrant computation.
                const auto offset = tracks.size();
                tracks.resize(offset + slot.input.columns.size(), 0);
                for (const auto row : slot.children) {
                    if (!in_flow(slots[row].input, slot.input))
                        continue;
                    std::size_t column = 0;
                    for (const auto cell : slots[row].children) {
                        const auto& input = slots[cell].input;
                        if (input.out_of_flow)
                            continue;
                        const auto at = column++;
                        if (!input.collapsed && slot.input.columns[at].rule != SizeRule::fixed)
                            tracks[offset + at] = std::max(tracks[offset + at], work_for(cell).intrinsic_width);
                    }
                }
                for (std::size_t column = 0; column < slot.input.columns.size(); ++column) {
                    const auto& rule = slot.input.columns[column];
                    ATELIER_TRY_BIND(size,
                                     constrained(rule.rule == SizeRule::fixed ? rule.value : tracks[offset + column],
                                                 rule, 0, index));
                    value += *size;
                    if (column)
                        value += slot.input.cross_gap;
                }
                tracks.resize(offset);
            } else {
                for (const auto child : slot.children) {
                    if (!in_flow(slots[child].input, slot.input))
                        continue;
                    if (slot.input.kind == Kind::row || slot.input.kind == Kind::wrap) {
                        value += slot.input.kind == Kind::wrap && slot.input.item_width > 0
                                     ? slot.input.item_width
                                     : work_for(child).intrinsic_width;
                        if (count++)
                            value += slot.input.gap;
                    } else
                        value = std::max(value, static_cast<double>(padding) + work_for(child).intrinsic_width);
                }
            }
            ATELIER_TRY_BIND(result, constrained(value, slot.input.width, padding, index));
            state.intrinsic_width = *result;
            state.intrinsic_valid = true;
            stack.pop_back();
        }
        return work_for(start).intrinsic_width;
    }

    void prepare_work() {
        has_grid_work = has_flex_work = has_placement_work = false;
        order.clear();
        work.clear();
        working.clear();
        pending.clear();
        tracks.clear();
        lines.clear();
        // Unvisited slots may retain an old ordinal. LayoutView::find validates
        // the complete identity after its bounds check, so only visible entries
        // need overwriting, including across failed computes and buffer swaps.
        working_index.resize(slots.size(), no_slot);
        stack.clear();
        stack.push_back(0);
        std::uint32_t at = 0;
        while (!stack.empty()) {
            const auto index = stack.back();
            stack.pop_back();
            if (slots[index].input.collapsed)
                continue;
            const auto& input = slots[index].input;
            has_grid_work |= input.kind == Kind::grid;
            has_flex_work |=
                input.flow.has_value() || input.width.rule == SizeRule::flex || input.height.rule == SizeRule::flex;
            has_placement_work |= input.cell != Cell{} || input.vertical_alignment.first_baseline;
            order.push_back(index);
            work.emplace_back();
            slots[index].work_index = at;
            working_index[index] = at;
            working.push_back({id(index), index ? id(slots[index].parent) : LayoutNodeId{}, 0, 0, 0, 0});
            ++at;
            const auto& children = slots[index].children;
            for (auto child = children.rbegin(); child != children.rend(); ++child)
                stack.push_back(*child);
        }
        prepare_grid_work();
        prepare_flex_work();
    }

    Result<void> resolve_widths(Size viewport) {
        box_for(0).width = viewport.width;
        for (const auto index : order) {
            ++statistics.node_visits;
            const auto& parent = slots[index].input;
            const float available = std::max(0.0f, box_for(index).width - horizontal(parent.padding));
            if (work_for(index).needs_baseline && parent.kind != Kind::measured) {
                for (const auto child : slots[index].children)
                    if (in_flow(slots[child].input, parent)) {
                        work_for(child).needs_baseline = true;
                        break;
                    }
            }
            if (has_grid_work || has_flex_work || has_placement_work)
                for (const auto child : slots[index].children) {
                    const auto& input = slots[child].input;
                    if (input.collapsed)
                        continue;
                    if (input.cell != Cell{} && parent.kind != Kind::grid)
                        return std::unexpected(Error{ErrorCode::invalid_structure, id(child), 0, InputField::cell});
                    if ((input.width.rule == SizeRule::flex &&
                         (parent.kind != Kind::row || !in_flow(input, parent) || table_for_row(index) != no_slot)) ||
                        (input.height.rule == SizeRule::flex &&
                         (parent.kind != Kind::column || !in_flow(input, parent))))
                        return std::unexpected(
                            Error{ErrorCode::unsupported_dependency, id(child), 0, InputField::flow});
                    if (alignment(child, false).first_baseline &&
                        (!is_flex(index) || parent.kind != Kind::row || !in_flow(input, parent)))
                        return std::unexpected(
                            Error{ErrorCode::unsupported_dependency, id(child), 0, InputField::alignment});
                }
            if (parent.kind == Kind::grid) {
                ATELIER_TRY(grid_widths(index, available));
            } else if (is_flex(index)) {
                ATELIER_TRY(flex_widths(index, available));
            }
            if (parent.kind == Kind::table) {
                ATELIER_TRY(resolve_tracks(index, available));
            }
            const auto table = table_for_row(index);
            std::size_t column = 0;
            const double repeat_columns =
                parent.item_width > 0 ? std::max(1.0, std::floor((static_cast<double>(available) + parent.gap) /
                                                                 (static_cast<double>(parent.item_width) + parent.gap)))
                                      : 0;
            const double repeat_width =
                parent.stretch_items
                    ? std::max<double>(parent.item_width,
                                       (available - (repeat_columns - 1) * parent.gap) / repeat_columns)
                    : parent.item_width;
            work_for(index).lines_begin = lines.size();
            shares.clear();
            share_indices.clear();
            double fixed = 0;
            std::size_t count = 0;
            for (const auto child : slots[index].children) {
                const auto& input = slots[child].input;
                const auto cell_column = table != no_slot && !input.out_of_flow ? column++ : 0;
                if (input.collapsed)
                    continue;
                auto& state = work_for(child);
                if ((parent.kind == Kind::grid || is_flex(index)) && in_flow(input, parent))
                    continue;
                state.needs_height =
                    input.height.rule != SizeRule::fixed &&
                    (parent.kind == Kind::wrap || input.height.rule == SizeRule::content ||
                     !alignment(child, false).fill || (in_flow(input, parent) && work_for(index).needs_height));
                if (table != no_slot && in_flow(input, parent)) {
                    const auto slot_width = tracks[work_for(table).tracks + cell_column];
                    ATELIER_TRY_BIND(width, cell_width(child, slot_width));
                    box_for(child).width = *width;
                    state.slot_width = slot_width;
                    continue;
                }
                if (parent.kind == Kind::wrap && in_flow(input, parent)) {
                    float slot_width = 0;
                    if (parent.item_width > 0) {
                        ATELIER_TRY_BIND(size, coordinate(repeat_width, child));
                        slot_width = *size;
                    } else {
                        ATELIER_TRY_BIND(natural, intrinsic_width(child));
                        slot_width = input.width.rule == SizeRule::expand ? std::min(available, *natural) : *natural;
                        ATELIER_TRY_BIND(size, constrained(slot_width, input.width, horizontal(input.padding), child));
                        slot_width = *size;
                    }
                    ATELIER_TRY_BIND(width, cell_width(child, slot_width));
                    box_for(child).width = *width;
                    state.slot_width = slot_width;
                    const bool first = lines.size() == work_for(index).lines_begin;
                    const bool full =
                        !first && (parent.item_width > 0 ? static_cast<double>(lines.back().count) >= repeat_columns
                                                         : lines.back().width + parent.gap + slot_width > available);
                    if (first || full)
                        lines.emplace_back();
                    auto& line = lines.back();
                    if (line.count++)
                        line.width += parent.gap;
                    line.width += slot_width;
                    state.line = lines.size() - 1;
                    continue;
                }
                const bool shared = parent.kind == Kind::row && in_flow(input, parent);
                if (shared) {
                    if (count++)
                        fixed += parent.gap;
                    if (input.width.rule == SizeRule::expand) {
                        shares.push_back(
                            {std::max(input.width.minimum, horizontal(input.padding)), input.width.value,
                             input.width.maximum ? std::optional<double>(*input.width.maximum) : std::nullopt});
                        share_indices.push_back(child);
                        continue;
                    }
                }
                ATELIER_TRY_BIND(result, cell_width(child, available));
                box_for(child).width = *result;
                state.slot_width = shared ? *result : available;
                if (shared)
                    fixed += state.slot_width;
            }
            allocate_slots(shares, std::max(0.0, static_cast<double>(available) - fixed), share_output, events,
                           weight_sums);
            for (std::size_t i = 0; i < share_indices.size(); ++i) {
                const auto child = share_indices[i];
                ATELIER_TRY_BIND(slot, coordinate(share_output[i], child));
                work_for(child).slot_width = *slot;
                ATELIER_TRY_BIND(result, cell_width(child, *slot));
                box_for(child).width = *result;
            }
            work_for(index).lines_end = lines.size();
        }
        return {};
    }

    Result<void> resolve_heights() {
        for (auto entry = order.rbegin(); entry != order.rend(); ++entry) {
            ++statistics.node_visits;
            const auto index = *entry;
            const auto& slot = slots[index];
            auto& state = work_for(index);
            std::optional<float> special_height;
            if (slot.input.kind == Kind::grid) {
                ATELIER_TRY_BIND(result, grid_heights(index));
                special_height = *result;
            } else if (is_flex(index)) {
                ATELIER_TRY_BIND(result, flex_heights(index));
                special_height = *result;
            } else if (slot.input.kind == Kind::wrap) {
                double height = 0;
                for (const auto child : slot.children) {
                    if (!in_flow(slots[child].input, slot.input))
                        continue;
                    const auto& cell = work_for(child);
                    lines[cell.line].height = std::max<double>(lines[cell.line].height, cell.natural_height);
                }
                for (auto line = state.lines_begin; line < state.lines_end; ++line) {
                    if (line != state.lines_begin)
                        height += slot.input.cross_gap;
                    lines[line].y = height;
                    height += lines[line].height;
                }
                ATELIER_TRY_BIND(result, coordinate(height, index));
                special_height = *result;
            }
            const auto padding = vertical(slot.input.padding);
            if (index == 0)
                state.natural_height = box_for(0).height;
            else if (slot.input.height.rule == SizeRule::fixed)
                state.natural_height = slot.input.height.value;
            else if (state.needs_height) {
                if (slot.input.kind == Kind::overlay)
                    return std::unexpected(Error{ErrorCode::unsupported_dependency, id(index)});
                double height = padding;
                if (slot.input.height.rule == SizeRule::flex && !slot.input.height.basis.natural &&
                    alignment(index, false).fill) {
                    height = slot.input.height.basis.value;
                } else if (slot.input.kind == Kind::measured) {
                    ATELIER_TRY_BIND(
                        size, measured(index, std::max(0.0f, box_for(index).width - horizontal(slot.input.padding))));
                    height += size->height;
                } else if (special_height)
                    height += *special_height;
                else {
                    std::size_t count = 0;
                    for (const auto child : slot.children) {
                        if (!in_flow(slots[child].input, slot.input))
                            continue;
                        if (vertical_flow(slot.input.kind)) {
                            height += work_for(child).natural_height;
                            if (count++)
                                height += slot.input.gap;
                        } else
                            height = std::max(height, static_cast<double>(padding) + work_for(child).natural_height);
                    }
                }
                ATELIER_TRY_BIND(result, constrained(height, slot.input.height, padding, index));
                state.natural_height = *result;
            }
            if (state.needs_baseline)
                ATELIER_TRY(resolve_baseline(index));
        }
        return {};
    }

    Result<float> height_in_slot(std::uint32_t index, double available) const {
        const auto& input = slots[index].input;
        const auto& state = work[slots[index].work_index];
        double value = input.height.rule == SizeRule::fixed
                           ? input.height.value
                           : (input.height.rule == SizeRule::content ? state.natural_height : available);
        if ((input.height.rule == SizeRule::expand || input.height.rule == SizeRule::flex) &&
            !alignment(index, false).fill)
            value = std::min(value, static_cast<double>(state.natural_height));
        return constrained(value, input.height, vertical(input.padding), index);
    }

    Result<void> resolve_baseline(std::uint32_t index) {
        auto& state = work_for(index);
        const auto& input = slots[index].input;
        const auto unsupported = [&] {
            return Result<void>{
                std::unexpected(Error{ErrorCode::unsupported_dependency, id(index), 0, InputField::alignment})};
        };
        if (input.kind == Kind::measured) {
            ATELIER_TRY_BIND(size, measured(index, std::max(0.0f, box_for(index).width - horizontal(input.padding))));
            ATELIER_TRY_BIND(
                result, coordinate(static_cast<double>(input.padding.top) + size->first_baseline.value_or(size->height),
                                   index));
            state.first_baseline = *result;
            return {};
        }
        std::uint32_t first = no_slot;
        for (const auto child : slots[index].children)
            if (in_flow(slots[child].input, input)) {
                first = child;
                break;
            }
        // An empty non-text box synthesizes its baseline from its final bottom.
        // Keeping the value absent lets its parent use the actual assigned size.
        if (first == no_slot)
            return {};
        if (input.kind == Kind::grid || input.kind == Kind::overlay)
            return unsupported();
        const bool known_height =
            index == 0 || input.height.rule == SizeRule::fixed || input.height.rule == SizeRule::content;
        double baseline = 0;
        if (known_height) {
            // All descendant height/line plans are complete in this bottom-up
            // pass. These containers keep this exact height during arrangement,
            // so use the same placement once and skip it in the top-down pass.
            const auto width = std::max(0.0f, box_for(index).width - horizontal(input.padding));
            const auto height = std::max(0.0f, state.natural_height - vertical(input.padding));
            ATELIER_TRY(arrange_children(index, width, height));
            state.children_arranged = true;
            if (is_flex(index) && input.kind == Kind::row) {
                const auto& flex = flex_states[slots[index].work_index];
                const auto& line = flex_lines[flex.lines_begin];
                if (line.baseline)
                    for (auto at = line.begin; at < line.end; ++at) {
                        if (alignment(flex_items[at].node, false).first_baseline) {
                            first = flex_items[at].node;
                            break;
                        }
                    }
            }
            baseline = box_for(first).y + work_for(first).first_baseline.value_or(box_for(first).height);
        } else if (input.kind == Kind::wrap) {
            const auto& line = lines[work_for(first).line];
            ATELIER_TRY_BIND(height, height_in_slot(first, line.height));
            baseline = input.padding.top + line.y + (line.height - *height) * alignment(first, false).offset +
                       work_for(first).first_baseline.value_or(*height);
        } else {
            const auto placement = alignment(first, false);
            const auto& child = slots[first].input;
            const bool flexible = child.height.rule == SizeRule::expand || child.height.rule == SizeRule::flex;
            if (is_flex(index)) {
                const auto spec = flex_spec(index);
                const auto& flex = flex_states[slots[index].work_index];
                const auto& line = flex_lines[flex.lines_begin];
                if (input.kind == Kind::column) {
                    if (spec.reverse || spec.justify != Distribution::start || (flexible && placement.offset != 0))
                        return unsupported();
                } else {
                    if (spec.wrap == Wrap::wrap_reverse ||
                        (spec.wrap != Wrap::no_wrap && spec.align_lines != Distribution::start))
                        return unsupported();
                    if (line.baseline) {
                        ATELIER_TRY_BIND(result, coordinate(input.padding.top + line.ascent, index));
                        state.first_baseline = *result;
                        return {};
                    }
                    if (placement.offset != 0)
                        return unsupported();
                }
            } else if (placement.offset != 0 && (input.kind == Kind::row || flexible))
                return unsupported();
            // A text baseline (or an already resolved descendant baseline) is
            // independent of slot height at the start edge. A flexible empty
            // box's bottom is not, and must wait for a definite-height contract.
            if (flexible && !work_for(first).first_baseline)
                return unsupported();
            baseline = input.padding.top + work_for(first).first_baseline.value_or(work_for(first).natural_height);
        }
        ATELIER_TRY_BIND(result, coordinate(baseline, index));
        state.first_baseline = *result;
        return {};
    }

    Result<void> arrange_children(std::uint32_t index, float width, float height) {
        const auto& parent = slots[index].input;
        const bool specialized = parent.kind == Kind::grid || is_flex(index);
        if (parent.kind == Kind::grid) {
            ATELIER_TRY(grid_arrange(index, width, height));
        } else if (is_flex(index)) {
            ATELIER_TRY(flex_arrange(index, width, height));
        }
        shares.clear();
        share_indices.clear();
        double fixed = 0;
        std::size_t count = 0;
        for (const auto child : slots[index].children) {
            const auto& input = slots[child].input;
            if (input.collapsed)
                continue;
            if (specialized && in_flow(input, parent))
                continue;
            const bool wrapping = parent.kind == Kind::wrap && in_flow(input, parent);
            const double available_height = wrapping ? lines[work_for(child).line].height : height;
            const bool shared = vertical_flow(parent.kind) && in_flow(input, parent);
            if (shared) {
                if (count++)
                    fixed += parent.gap;
                if (input.height.rule == SizeRule::expand) {
                    shares.push_back(
                        {std::max(input.height.minimum, vertical(input.padding)), input.height.value,
                         input.height.maximum ? std::optional<double>(*input.height.maximum) : std::nullopt});
                    share_indices.push_back(child);
                    continue;
                }
            }
            ATELIER_TRY_BIND(result, height_in_slot(child, available_height));
            box_for(child).height = *result;
            ATELIER_TRY_BIND(slot_height, coordinate(shared ? *result : available_height, child));
            work_for(child).slot_height = *slot_height;
            if (shared)
                fixed += work_for(child).slot_height;
        }
        allocate_slots(shares, std::max(0.0, static_cast<double>(height) - fixed), share_output, events, weight_sums);
        for (std::size_t i = 0; i < share_indices.size(); ++i) {
            const auto child = share_indices[i];
            ATELIER_TRY_BIND(slot, coordinate(share_output[i], child));
            work_for(child).slot_height = *slot;
            ATELIER_TRY_BIND(result, height_in_slot(child, *slot));
            box_for(child).height = *result;
        }
        double cursor = 0;
        count = 0;
        const auto table = table_for_row(index);
        std::size_t column = 0;
        for (const auto child : slots[index].children) {
            const auto& input = slots[child].input;
            double column_x = 0;
            if (table != no_slot && !input.out_of_flow) {
                if (column)
                    cursor += slots[table].input.cross_gap;
                column_x = cursor;
                cursor += tracks[work_for(table).tracks + column++];
            }
            if (input.collapsed)
                continue;
            if (specialized && in_flow(input, parent))
                continue;
            auto& box = box_for(child);
            double x = parent.padding.left;
            double y = parent.padding.top;
            if (!in_flow(input, parent)) {
                x += static_cast<double>(width) * input.anchor_x - static_cast<double>(box.width) * input.pivot_x +
                     input.offset_x;
                y += static_cast<double>(height) * input.anchor_y - static_cast<double>(box.height) * input.pivot_y +
                     input.offset_y;
            } else if (parent.kind == Kind::wrap) {
                auto& line = lines[work_for(child).line];
                x += (static_cast<double>(width) - line.width) * parent.line_align + line.cursor +
                     (static_cast<double>(work_for(child).slot_width) - box.width) * alignment(child, true).offset;
                y += line.y + (line.height - box.height) * alignment(child, false).offset;
                line.cursor += work_for(child).slot_width + parent.gap;
            } else if (table != no_slot) {
                x += column_x +
                     (static_cast<double>(work_for(child).slot_width) - box.width) * alignment(child, true).offset;
                y += (static_cast<double>(height) - box.height) * alignment(child, false).offset;
            } else if (parent.kind == Kind::row) {
                if (count++)
                    cursor += parent.gap;
                x += cursor +
                     (static_cast<double>(work_for(child).slot_width) - box.width) * alignment(child, true).offset;
                y += (static_cast<double>(height) - box.height) * alignment(child, false).offset;
                cursor += work_for(child).slot_width;
            } else {
                if (count++)
                    cursor += parent.gap;
                y += cursor +
                     (static_cast<double>(work_for(child).slot_height) - box.height) * alignment(child, false).offset;
                x += (static_cast<double>(width) - box.width) * alignment(child, true).offset;
                cursor += work_for(child).slot_height;
            }
            auto left = coordinate(x, child);
            auto top = coordinate(y, child);
            ATELIER_TRY(left);
            ATELIER_TRY(top);
            box.x = *left;
            box.y = *top;
        }
        return {};
    }

    Result<void> arrange(Size viewport) {
        box_for(0).height = viewport.height;
        for (const auto index : order) {
            ++statistics.node_visits;
            if (work_for(index).children_arranged)
                continue;
            const auto& parent = slots[index].input;
            const float width = std::max(0.0f, box_for(index).width - horizontal(parent.padding));
            const float height = std::max(0.0f, box_for(index).height - vertical(parent.padding));
            ATELIER_TRY(arrange_children(index, width, height));
        }
        return {};
    }

    LayoutView view() const noexcept {
        return {publication, published_input, published_provider, published_viewport, published, published_index};
    }

    const std::uint64_t identity;

    Measure measure;

    void* context;

    Limits limits;

    bool active{};

    bool geometry_dirty{true};

    bool has_grid_work{};

    bool has_flex_work{};

    bool has_placement_work{};

    std::uint64_t geometry_generation{};

    std::uint32_t free_head{no_slot};

    std::size_t live_count{1};

    std::uint64_t input_revision{1};

    std::uint64_t publication{};

    std::uint64_t published_input{};

    std::uint64_t published_provider{};

    std::uint64_t current_provider{};

    Size published_viewport{};

    Stats statistics{};

    BufferMemory memory;

    Buffer<Slot> slots;

    Buffer<std::uint32_t> stack;

    Buffer<std::uint32_t> order;

    Buffer<std::uint32_t> share_indices;

    Buffer<Work> work;

    Buffer<Pending> pending;

    Buffer<LayoutBox> working;

    Buffer<LayoutBox> published;

    Buffer<std::uint32_t> working_index;

    Buffer<std::uint32_t> published_index;

    Buffer<Share> shares;

    Buffer<double> share_output;

    Buffer<double> weight_sums;

    Buffer<std::pair<double, std::size_t>> events;

    Buffer<float> tracks;

    Buffer<Line> lines;

#include "detail/grid.inl"
#include "detail/flex.inl"
};

LayoutDocument::LayoutDocument(Measure measure, void* context, Limits limits) {
    assert(limits.max_nodes != 0 && limits.max_nodes != no_slot && limits.max_grid_tracks != 0 &&
           limits.max_grid_tracks != no_slot && limits.max_grid_cells != 0 &&
           "Layout max_nodes must include the root and fit slot identities");
    impl_ = std::make_unique<Impl>(measure, context, limits);
}

LayoutDocument::~LayoutDocument() = default;

LayoutNodeId LayoutDocument::root() const noexcept {
    return impl_->id(0);
}

std::uint64_t LayoutDocument::input_revision() const noexcept {
    return impl_->input_revision;
}

Result<void> LayoutDocument::set_root(const RootInput& input) {
    auto& d = *impl_;
    ATELIER_TRY(d.editable());
    if ((input.kind != Kind::row && input.kind != Kind::column && input.kind != Kind::overlay) ||
        !valid_padding(input.padding) || !nonnegative(input.gap) ||
        (input.kind == Kind::overlay && (input.gap != 0 || input.flow)) ||
        (input.flow &&
         (!valid_flow(*input.flow) || (input.kind == Kind::column && input.flow->align_items.first_baseline))))
        return std::unexpected(Error{ErrorCode::invalid_input, root()});
    auto& old = d.slots[0].input;
    if (old.kind == input.kind && old.padding == input.padding && old.gap == input.gap && old.flow == input.flow)
        return {};
    ATELIER_TRY(d.revision_available());
    old.kind = input.kind;
    old.padding = input.padding;
    old.gap = input.gap;
    old.flow = input.flow;
    d.geometry_dirty = true;
    ++d.input_revision;
    return {};
}

Result<LayoutNodeId> LayoutDocument::create(LayoutNodeId parent, const NodeInput& input, std::size_t position) {
    auto& d = *impl_;
    ATELIER_TRY(d.editable());
    if (!d.valid(parent))
        return std::unexpected(Error{ErrorCode::invalid_node, parent});
    if (!container(d.slots[parent.slot_].input.kind))
        return std::unexpected(Error{ErrorCode::invalid_structure, parent});
    if (auto error = validate(input)) {
        error->node = parent;
        return std::unexpected(*error);
    }
    const auto child_count = d.slots[parent.slot_].children.size();
    if (position == append_position)
        position = child_count;
    if (position > child_count)
        return std::unexpected(Error{ErrorCode::invalid_input, parent});
    if (d.slots[parent.slot_].depth >= d.limits.max_depth ||
        (d.free_head == no_slot && d.slots.size() >= d.limits.max_nodes))
        return std::unexpected(Error{ErrorCode::capacity_exceeded, parent});
    ATELIER_TRY(d.revision_available());
    NodeInput prepared = input; // Copy fallible descriptor storage before editing the pool.
    d.reserve_child(parent.slot_);
    const auto index = d.free_head == no_slot ? static_cast<std::uint32_t>(d.slots.size()) : d.free_head;
    if (d.free_head == no_slot)
        d.slots.emplace_back(&d.memory);
    else
        d.free_head = d.slots[index].next_free;
    auto& slot = d.slots[index];
    slot.input = std::move(prepared);
    slot.parent = parent.slot_;
    slot.depth = d.slots[parent.slot_].depth + 1;
    slot.next_free = no_slot;
    slot.live = true;
    auto& children = d.slots[parent.slot_].children;
    children.insert(children.begin() + static_cast<std::ptrdiff_t>(position), index);
    ++d.live_count;
    d.geometry_dirty = true;
    ++d.input_revision;
    return d.id(index);
}

Result<NodeInput> LayoutDocument::input(LayoutNodeId node) const {
    const auto& d = *impl_;
    if (!d.valid(node))
        return std::unexpected(Error{ErrorCode::invalid_node, node});
    if (node.slot_ == 0)
        return std::unexpected(Error{ErrorCode::root_operation, node});
    return d.slots[node.slot_].input;
}

Result<void> LayoutDocument::update(LayoutNodeId node, const NodeInput& input) {
    auto& d = *impl_;
    ATELIER_TRY(d.editable());
    if (!d.valid(node))
        return std::unexpected(Error{ErrorCode::invalid_node, node});
    if (node.slot_ == 0)
        return std::unexpected(Error{ErrorCode::root_operation, node});
    if (auto error = validate(input)) {
        error->node = node;
        return std::unexpected(*error);
    }
    auto& slot = d.slots[node.slot_];
    if (!container(input.kind) && !slot.children.empty())
        return std::unexpected(Error{ErrorCode::invalid_structure, node});
    if (slot.input == input)
        return {};
    ATELIER_TRY(d.revision_available());
    NodeInput prepared = input;
    prepared.content_key = slot.input.content_key;
    prepared.content_revision = slot.input.content_revision;
    const bool content_only = prepared == slot.input;
    prepared.content_key = input.content_key;
    prepared.content_revision = input.content_revision;
    d.geometry_dirty |= !content_only || slot.measured_generation == d.geometry_generation;
    slot.input = std::move(prepared);
    ++d.input_revision;
    return {};
}

Result<void> LayoutDocument::move(LayoutNodeId node, LayoutNodeId parent, std::size_t position) {
    auto& d = *impl_;
    ATELIER_TRY(d.editable());
    if (!d.valid(node))
        return std::unexpected(Error{ErrorCode::invalid_node, node});
    if (!d.valid(parent))
        return std::unexpected(Error{ErrorCode::invalid_node, parent});
    if (node.slot_ == 0)
        return std::unexpected(Error{ErrorCode::root_operation, node});
    if (!container(d.slots[parent.slot_].input.kind))
        return std::unexpected(Error{ErrorCode::invalid_structure, parent});
    for (auto ancestor = parent.slot_; ancestor != no_slot; ancestor = d.slots[ancestor].parent)
        if (ancestor == node.slot_)
            return std::unexpected(Error{ErrorCode::invalid_structure, node});
    const auto old_parent = d.slots[node.slot_].parent;
    const auto destination_size = d.slots[parent.slot_].children.size() - (old_parent == parent.slot_ ? 1 : 0);
    if (position == append_position)
        position = destination_size;
    if (position > destination_size)
        return std::unexpected(Error{ErrorCode::invalid_input, parent});
    auto& source = d.slots[old_parent].children;
    const auto old_position =
        static_cast<std::size_t>(std::find(source.begin(), source.end(), node.slot_) - source.begin());
    if (old_parent == parent.slot_ && position == old_position)
        return {};
    ATELIER_TRY(d.revision_available());
    d.collect_subtree(node.slot_);
    const auto delta = static_cast<std::int64_t>(d.slots[parent.slot_].depth) + 1 - d.slots[node.slot_].depth;
    for (const auto descendant : d.stack) {
        if (static_cast<std::int64_t>(d.slots[descendant].depth) + delta > d.limits.max_depth)
            return std::unexpected(Error{ErrorCode::capacity_exceeded, node});
    }
    if (old_parent != parent.slot_)
        d.reserve_child(parent.slot_);
    source.erase(source.begin() + static_cast<std::ptrdiff_t>(old_position));
    auto& destination = d.slots[parent.slot_].children;
    destination.insert(destination.begin() + static_cast<std::ptrdiff_t>(position), node.slot_);
    d.slots[node.slot_].parent = parent.slot_;
    for (const auto descendant : d.stack)
        d.slots[descendant].depth =
            static_cast<std::uint32_t>(static_cast<std::int64_t>(d.slots[descendant].depth) + delta);
    d.geometry_dirty = true;
    ++d.input_revision;
    return {};
}

Result<void> LayoutDocument::remove(LayoutNodeId node) {
    auto& d = *impl_;
    ATELIER_TRY(d.editable());
    if (!d.valid(node))
        return std::unexpected(Error{ErrorCode::invalid_node, node});
    if (node.slot_ == 0)
        return std::unexpected(Error{ErrorCode::root_operation, node});
    ATELIER_TRY(d.revision_available());
    d.collect_subtree(node.slot_);
    auto& siblings = d.slots[d.slots[node.slot_].parent].children;
    siblings.erase(std::find(siblings.begin(), siblings.end(), node.slot_));
    for (const auto index : d.stack) {
        if (d.retire(index)) {
            d.slots[index].next_free = d.free_head;
            d.free_head = index;
        }
    }
    d.live_count -= d.stack.size();
    d.geometry_dirty = true;
    ++d.input_revision;
    return {};
}

Result<std::vector<LayoutNodeId>> LayoutDocument::set_tree(std::span<const TreeNodeInput> input) {
    auto& d = *impl_;
    ATELIER_TRY(d.editable());
    if (input.size() >= d.limits.max_nodes)
        return std::unexpected(Error{ErrorCode::capacity_exceeded});
    struct Plan {
        std::uint32_t slot{};

        std::uint32_t parent{};

        std::uint32_t depth{};

        std::size_t begin{};

        std::size_t count{};

        std::size_t cursor{};

        bool fresh{};

        bool children_changed{};
    };
    struct PreparedInput {
        std::uint32_t slot;

        NodeInput input;
    };
    Buffer<Plan> plan(input.size() + 1, Plan{}, BufferAllocator<Plan>{&d.memory});
    Buffer<std::uint32_t> positions(d.slots.size(), no_slot, BufferAllocator<std::uint32_t>{&d.memory});
    positions[0] = 0;
    std::size_t fresh_count = 0;
    const auto failure = [](Error error, std::size_t index) {
        error.tree_index = index;
        return std::unexpected(error);
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto& item = input[i];
        auto& entry = plan[i + 1];
        if (item.node) {
            if (!d.valid(item.node))
                return failure({ErrorCode::invalid_node, item.node}, i);
            if (item.node.slot_ == 0)
                return failure({ErrorCode::root_operation, item.node}, i);
            if (positions[item.node.slot_] != no_slot)
                return failure({ErrorCode::invalid_structure, item.node}, i);
            entry.slot = item.node.slot_;
            positions[entry.slot] = static_cast<std::uint32_t>(i + 1);
        } else {
            entry.fresh = true;
            ++fresh_count;
        }
        if (auto error = validate(item.input)) {
            error->node = item.node;
            return failure(*error, i);
        }
        if (item.parent && (*item.parent >= i || !container(input[*item.parent].input.kind)))
            return failure({ErrorCode::invalid_structure, item.node}, i);
        entry.parent = item.parent ? static_cast<std::uint32_t>(*item.parent + 1) : 0;
        auto& parent = plan[entry.parent];
        if (parent.depth >= d.limits.max_depth)
            return failure({ErrorCode::capacity_exceeded, item.node}, i);
        entry.depth = parent.depth + 1;
        ++parent.count;
    }

    Buffer<std::uint32_t> available{BufferAllocator<std::uint32_t>{&d.memory}};
    available.reserve(d.slots.size() - 1);
    for (auto index = d.free_head; index != no_slot; index = d.slots[index].next_free)
        available.push_back(index);
    for (std::uint32_t index = 1; index < d.slots.size(); ++index)
        if (d.slots[index].live && positions[index] == no_slot && d.slots[index].generation != no_slot)
            available.push_back(index);
    const auto reused = std::min(fresh_count, available.size());
    const auto appended_count = fresh_count - reused;
    const auto old_size = d.slots.size();
    if (appended_count > d.limits.max_nodes - old_size)
        return std::unexpected(Error{ErrorCode::capacity_exceeded});
    std::size_t used = 0;
    std::size_t appended = 0;
    std::vector<LayoutNodeId> result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        auto& entry = plan[i + 1];
        if (entry.fresh) {
            entry.slot = used < reused ? available[used++] : static_cast<std::uint32_t>(old_size + appended++);
            if (entry.slot < old_size)
                positions[entry.slot] = static_cast<std::uint32_t>(i + 1);
        }
        const auto generation =
            entry.slot < old_size
                ? d.slots[entry.slot].generation + static_cast<std::uint32_t>(entry.fresh && d.slots[entry.slot].live)
                : 1;
        result.push_back(LayoutNodeId{d.identity, entry.slot, generation});
    }
    Buffer<std::uint32_t> children(input.size(), 0, BufferAllocator<std::uint32_t>{&d.memory});
    std::size_t begin = 0;
    for (auto& entry : plan) {
        entry.begin = entry.cursor = begin;
        begin += entry.count;
    }
    for (std::size_t i = 1; i < plan.size(); ++i)
        children[plan[plan[i].parent].cursor++] = plan[i].slot;

    Buffer<PreparedInput> prepared{BufferAllocator<PreparedInput>{&d.memory}};
    bool changed = input.size() + 1 != d.live_count || fresh_count != 0;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        auto& entry = plan[i];
        const auto sequence = std::span<const std::uint32_t>{children}.subspan(entry.begin, entry.count);
        entry.children_changed = entry.fresh || !std::ranges::equal(d.slots[entry.slot].children, sequence);
        changed |= entry.children_changed;
        if (i == 0)
            continue;
        if (entry.fresh || d.slots[entry.slot].input != input[i - 1].input) {
            prepared.push_back({entry.slot, input[i - 1].input});
            changed = true;
        }
        changed |= entry.fresh || d.slots[entry.slot].parent != plan[entry.parent].slot;
    }
    if (!changed)
        return result;
    ATELIER_TRY(d.revision_available());

    // All descriptor copies and allocations precede the logical commit. Capacity
    // may grow on failure, but inputs, identities, dirty state and publication stay.
    static_assert(std::is_nothrow_move_constructible_v<Impl::Slot>);
    static_assert(std::is_nothrow_move_assignable_v<NodeInput>);
    Buffer<Impl::Slot> additions{BufferAllocator<Impl::Slot>{&d.memory}};
    additions.reserve(appended_count);
    for (std::size_t i = 0; i < appended_count; ++i)
        additions.emplace_back(&d.memory);
    for (const auto& entry : plan)
        if (entry.children_changed) {
            auto& slot = entry.slot < old_size ? d.slots[entry.slot] : additions[entry.slot - old_size];
            if (entry.count > slot.children.capacity())
                slot.children.reserve(
                    std::min<std::size_t>(d.limits.max_nodes, std::max(entry.count, slot.children.capacity() * 2)));
        }
    if (old_size + appended_count > d.slots.capacity())
        d.slots.reserve(
            std::min<std::size_t>(d.limits.max_nodes, std::max(old_size + appended_count, d.slots.capacity() * 2)));

    for (auto& slot : additions)
        d.slots.push_back(std::move(slot));
    for (std::uint32_t index = 1; index < old_size; ++index) {
        const auto position = positions[index];
        if (d.slots[index].live && (position == no_slot || plan[position].fresh))
            d.retire(index, position == no_slot);
    }
    for (auto& item : prepared)
        d.slots[item.slot].input = std::move(item.input);
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const auto& entry = plan[i];
        auto& slot = d.slots[entry.slot];
        slot.live = true;
        slot.parent = i ? plan[entry.parent].slot : no_slot;
        slot.depth = entry.depth;
        slot.next_free = no_slot;
        if (entry.children_changed) {
            slot.children.resize(entry.count);
            std::copy_n(children.begin() + static_cast<std::ptrdiff_t>(entry.begin), entry.count,
                        slot.children.begin());
            d.statistics.topology_links_written += entry.count;
        }
    }
    d.free_head = no_slot;
    for (auto i = available.size(); i > reused; --i) {
        d.slots[available[i - 1]].next_free = d.free_head;
        d.free_head = available[i - 1];
    }
    d.live_count = input.size() + 1;
    d.geometry_dirty = true;
    ++d.input_revision;
    ++d.statistics.topology_commits;
    return result;
}

Result<LayoutView> LayoutDocument::compute(Size viewport, std::uint64_t provider_revision) {
    auto& d = *impl_;
    if (d.active)
        return std::unexpected(Error{ErrorCode::busy});
    if (!nonnegative(viewport.width) || !nonnegative(viewport.height))
        return std::unexpected(Error{ErrorCode::invalid_input, root()});
    if (d.publication && d.published_input == d.input_revision && d.published_provider == provider_revision &&
        d.published_viewport == viewport)
        return d.view();
    if (d.publication == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::revision_exhausted});
    if (d.publication && !d.geometry_dirty && d.published_provider == provider_revision &&
        d.published_viewport == viewport) {
        ++d.statistics.computes;
        ++d.statistics.publications;
        ++d.statistics.geometry_reuses;
        ++d.publication;
        d.published_input = d.input_revision;
        return d.view();
    }
    struct Activity {
        bool& active;

        explicit Activity(bool& value) : active(value) { active = true; }

        ~Activity() { active = false; }
    } activity(d.active);
    ++d.statistics.computes;
    d.current_provider = provider_revision;
    d.prepare_work();
    d.box_for(0).height = viewport.height;
    ATELIER_TRY(d.resolve_widths(viewport));
    ATELIER_TRY(d.resolve_heights());
    ATELIER_TRY(d.arrange(viewport));
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
    d.published_index.swap(d.working_index);
    ++d.publication;
    d.geometry_generation = d.publication;
    for (const auto index : d.order)
        d.slots[index].measured_generation = d.work_for(index).uses_measurement ? d.geometry_generation : 0;
    d.geometry_dirty = false;
    ++d.statistics.publications;
    d.published_input = d.input_revision;
    d.published_provider = provider_revision;
    d.published_viewport = viewport;
    return d.view();
}

LayoutView LayoutDocument::view() const noexcept {
    return impl_->view();
}

LayoutSnapshot LayoutDocument::snapshot() const {
    const auto current = view();
    LayoutSnapshot result;
    result.publication = current.publication;
    result.input_revision = current.input_revision;
    result.provider_revision = current.provider_revision;
    result.viewport = current.viewport;
    result.boxes.assign(current.boxes.begin(), current.boxes.end());
    result.index_.assign(current.index_.begin(), current.index_.end());
    return result;
}

Stats LayoutDocument::stats() const noexcept {
    auto result = impl_->statistics;
    result.buffer_allocations = impl_->memory.allocations;
    result.buffer_bytes_allocated = impl_->memory.allocated;
    result.buffer_bytes = impl_->memory.live;
    result.peak_buffer_bytes = impl_->memory.peak;
    return result;
}

const LayoutBox* LayoutView::find(LayoutNodeId node) const noexcept {
    if (!node || node.slot_ >= index_.size())
        return nullptr;
    const auto index = index_[node.slot_];
    return index < boxes.size() && boxes[index].node == node ? &boxes[index] : nullptr;
}

LayoutView LayoutSnapshot::view() const noexcept {
    return {publication, input_revision, provider_revision, viewport, boxes, index_};
}

NodeInput NodeInput::row(float gap) {
    NodeInput value;
    value.kind = Kind::row;
    value.gap = gap;
    return value;
}

NodeInput NodeInput::column(float gap) {
    NodeInput value;
    value.kind = Kind::column;
    value.gap = gap;
    return value;
}

NodeInput NodeInput::overlay() {
    NodeInput value;
    value.kind = Kind::overlay;
    value.height = Axis::expand();
    return value;
}

NodeInput NodeInput::box(float width, float height) {
    NodeInput value;
    value.width = Axis::fixed(width);
    value.height = Axis::fixed(height);
    return value;
}

NodeInput NodeInput::measured(std::uint64_t key, std::uint64_t revision) {
    NodeInput value;
    value.kind = Kind::measured;
    value.content_key = key;
    value.content_revision = revision;
    return value;
}

NodeInput NodeInput::table(std::vector<Axis> columns, float column_gap, float row_gap) {
    auto value = column(row_gap);
    value.kind = Kind::table;
    value.columns = std::move(columns);
    value.cross_gap = column_gap;
    return value;
}

NodeInput NodeInput::wrap(float item_gap, float line_gap) {
    auto value = row(item_gap);
    value.kind = Kind::wrap;
    value.cross_gap = line_gap;
    return value;
}

NodeInput NodeInput::grid(float minimum_item_width, float item_gap, float line_gap) {
    auto value = wrap(item_gap, line_gap);
    value.item_width = minimum_item_width;
    value.stretch_items = true;
    return value;
}

NodeInput NodeInput::grid(GridSpec spec) {
    NodeInput value;
    value.kind = Kind::grid;
    value.grid_spec = std::move(spec);
    return value;
}

std::string_view describe(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_input:
        return "Invalid layout input";
    case ErrorCode::invalid_node:
        return "Foreign or expired node identity";
    case ErrorCode::invalid_structure:
        return "Invalid parent, child or tree relationship";
    case ErrorCode::root_operation:
        return "Use set_root for the root node";
    case ErrorCode::capacity_exceeded:
        return "Layout node, depth or grid capacity exceeded";
    case ErrorCode::unsupported_dependency:
        return "Unsupported sizing dependency";
    case ErrorCode::busy:
        return "Cannot edit or compute during measurement";
    case ErrorCode::measurement_failed:
        return "Content measurement failed";
    case ErrorCode::invalid_measurement:
        return "Measurement dimensions and optional first baseline must be finite and valid";
    case ErrorCode::numeric_overflow:
        return "Geometry exceeds the supported numeric range";
    case ErrorCode::revision_exhausted:
        return "Publication or input revision exhausted";
    }
    return "Unknown layout error";
}

std::string_view describe(InputField field) noexcept {
    switch (field) {
    case InputField::none:
        return "unspecified";
    case InputField::kind:
        return "container kind or sizing combination";
    case InputField::width:
        return "width rule, bounds or horizontal padding";
    case InputField::height:
        return "height rule, bounds or vertical padding";
    case InputField::padding:
        return "padding";
    case InputField::gap:
        return "gap or cross_gap";
    case InputField::alignment:
        return "alignment must be in [0,1]";
    case InputField::anchor:
        return "anchor, pivot, offset or alignment must be finite";
    case InputField::content:
        return "content is only valid on measured leaves";
    case InputField::columns:
        return "table requires valid nonempty column tracks";
    case InputField::table_row:
        return "table requires fill-width rows without horizontal padding or gap; cells must fit column count";
    case InputField::wrap:
        return "wrap item width, stretch policy or line alignment";
    case InputField::flow:
        return "flex basis, distribution or dependency";
    case InputField::grid:
        return "grid tracks, gaps or placement capacity";
    case InputField::cell:
        return "grid cell position or span";
    }
    return "unknown field";
}

namespace {
bool list_height(double value) {
    return std::isfinite(value) && value > 0 && value <= std::numeric_limits<float>::max();
}
}

Result<VirtualList> VirtualList::create(std::size_t max_items, double gap) {
    if (max_items == 0 || max_items >= no_slot || !nonnegative(gap) || gap > std::numeric_limits<float>::max())
        return std::unexpected(Error{ErrorCode::invalid_input});
    return VirtualList(max_items, gap);
}

Result<void> VirtualList::revision_available() const {
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::revision_exhausted});
    return {};
}

void VirtualList::rebuild(std::vector<double>& sums, const std::vector<double>& heights) const noexcept {
    std::fill(sums.begin(), sums.end(), 0);
    for (std::size_t i = 1; i < sums.size(); ++i) {
        sums[i] += heights[i - 1] + gap_;
        const auto parent = i + (i & (~i + 1));
        if (parent < sums.size())
            sums[parent] += sums[i];
    }
}

double VirtualList::prefix(std::size_t count) const noexcept {
    double sum = 0;
    for (; count; count -= count & (~count + 1))
        sum += sums_[count];
    return sum;
}

double VirtualList::extent() const noexcept {
    return heights_.empty() ? 0 : std::max(0.0, prefix(heights_.size()) - gap_);
}

std::size_t VirtualList::locate(double offset) const noexcept {
    std::size_t index = 0;
    double sum = 0;
    for (auto bit = std::bit_floor(heights_.size()); bit; bit >>= 1) {
        const auto next = index + bit;
        if (next < sums_.size() && sum + sums_[next] <= offset) {
            sum += sums_[next];
            index = next;
        }
    }
    return index;
}

void VirtualList::restore_anchor(std::size_t row, double within) noexcept {
    if (heights_.empty()) {
        scroll_ = 0;
        return;
    }
    row = std::min(row, heights_.size() - 1);
    const double stride = heights_[row] + (row + 1 < heights_.size() ? gap_ : 0);
    scroll_ = std::clamp(prefix(row) + std::clamp(within, 0.0, std::nextafter(stride, 0.0)), 0.0,
                         std::max(0.0, extent() - viewport_));
}

Result<void> VirtualList::reset(std::size_t count, double estimated_height) {
    if (count > max_items_)
        return std::unexpected(Error{ErrorCode::capacity_exceeded});
    if (!list_height(estimated_height))
        return std::unexpected(Error{ErrorCode::invalid_input});
    ATELIER_TRY(revision_available());
    std::vector<double> heights(count, estimated_height);
    std::vector<double> sums(count + 1);
    rebuild(sums, heights);
    heights_.swap(heights);
    sums_.swap(sums);
    scroll_ = 0;
    ++revision_;
    return {};
}

Result<void> VirtualList::set_viewport(double scroll, double height, double overscan) {
    if (!nonnegative(scroll) || !nonnegative(height) || !nonnegative(overscan) ||
        height > std::numeric_limits<float>::max() || overscan > std::numeric_limits<float>::max())
        return std::unexpected(Error{ErrorCode::invalid_input});
    scroll = std::min(scroll, std::max(0.0, extent() - height));
    if (scroll == scroll_ && height == viewport_ && overscan == overscan_)
        return {};
    ATELIER_TRY(revision_available());
    scroll_ = scroll;
    viewport_ = height;
    overscan_ = overscan;
    ++revision_;
    return {};
}

Result<void> VirtualList::set_height(std::size_t row, double height) {
    if (row >= heights_.size() || !list_height(height))
        return std::unexpected(Error{ErrorCode::invalid_input});
    if (heights_[row] == height)
        return {};
    ATELIER_TRY(revision_available());
    const auto anchor = std::min(locate(scroll_), heights_.size() - 1);
    const auto within = scroll_ - prefix(anchor);
    const auto delta = height - heights_[row];
    heights_[row] = height;
    for (auto index = row + 1; index < sums_.size(); index += index & (~index + 1))
        sums_[index] += delta;
    restore_anchor(anchor, within);
    ++revision_;
    return {};
}

Result<void> VirtualList::invalidate_heights(double estimated_height) {
    if (!list_height(estimated_height))
        return std::unexpected(Error{ErrorCode::invalid_input});
    ATELIER_TRY(revision_available());
    const auto anchor = heights_.empty() ? 0 : std::min(locate(scroll_), heights_.size() - 1);
    const auto within = scroll_ - prefix(anchor);
    std::fill(heights_.begin(), heights_.end(), estimated_height);
    rebuild(sums_, heights_);
    restore_anchor(anchor, within);
    ++revision_;
    return {};
}

Result<void> VirtualList::insert(std::size_t position, std::size_t count, double estimated_height) {
    if (position > heights_.size() || !list_height(estimated_height))
        return std::unexpected(Error{ErrorCode::invalid_input});
    if (count > max_items_ - heights_.size())
        return std::unexpected(Error{ErrorCode::capacity_exceeded});
    if (!count)
        return {};
    ATELIER_TRY(revision_available());
    auto anchor = heights_.empty() ? 0 : std::min(locate(scroll_), heights_.size() - 1);
    const auto within = scroll_ - prefix(anchor);
    std::vector<double> heights;
    heights.reserve(heights_.size() + count);
    heights.insert(heights.end(), heights_.begin(), heights_.begin() + static_cast<std::ptrdiff_t>(position));
    heights.insert(heights.end(), count, estimated_height);
    heights.insert(heights.end(), heights_.begin() + static_cast<std::ptrdiff_t>(position), heights_.end());
    std::vector<double> sums(heights.size() + 1);
    rebuild(sums, heights);
    if (!heights_.empty() && position <= anchor)
        anchor += count;
    heights_.swap(heights);
    sums_.swap(sums);
    restore_anchor(anchor, within);
    ++revision_;
    return {};
}

Result<void> VirtualList::erase(std::size_t position, std::size_t count) {
    if (position > heights_.size() || count > heights_.size() - position)
        return std::unexpected(Error{ErrorCode::invalid_input});
    if (!count)
        return {};
    ATELIER_TRY(revision_available());
    auto anchor = std::min(locate(scroll_), heights_.size() - 1);
    double within = scroll_ - prefix(anchor);
    if (anchor >= position + count)
        anchor -= count;
    else if (anchor >= position) {
        anchor = position;
        within = 0;
    }
    // Erasing doubles and rebuilding within existing capacity cannot allocate.
    heights_.erase(heights_.begin() + static_cast<std::ptrdiff_t>(position),
                   heights_.begin() + static_cast<std::ptrdiff_t>(position + count));
    sums_.resize(heights_.size() + 1);
    rebuild(sums_, heights_);
    restore_anchor(anchor, within);
    ++revision_;
    return {};
}

VisibleRange VirtualList::range() const noexcept {
    const auto total = extent();
    if (heights_.empty() || viewport_ == 0)
        return {0, 0, 0, scroll_, total, revision_};
    const auto begin = std::min(locate(std::max(0.0, scroll_ - overscan_)), heights_.size() - 1);
    const auto bottom = std::min(total, scroll_ + viewport_ + overscan_);
    auto end = locate(bottom);
    if (end < heights_.size() && prefix(end) < bottom)
        ++end;
    return {begin, end, prefix(begin) - scroll_, scroll_, total, revision_};
}

Result<double> VirtualList::offset(std::size_t row) const {
    if (row > heights_.size())
        return std::unexpected(Error{ErrorCode::invalid_input});
    return row == heights_.size() ? extent() : prefix(row);
}

} // namespace atelier::ui_layout
