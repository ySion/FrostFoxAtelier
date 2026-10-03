    // Included inside Impl. These are compute-local plans, indexed by the existing
    // work index; no second node tree or persistent ownership is introduced.
    struct GridPlan {
        std::size_t begin{}, count{}, columns_begin{}, rows_begin{};
        std::size_t columns_prefix{no_pending}, rows_prefix{no_pending};
        std::uint32_t columns{}, rows{};
        SizeRule columns_rule{SizeRule::fixed}, rows_rule{SizeRule::fixed};
        bool ready{};
    };
    struct GridItem {
        std::uint32_t node{}, row{no_slot}, column{no_slot}, row_span{1}, column_span{1};
    };
    struct GridTrack { double size{}, position{}; };
    struct GridPrefix { std::uint32_t non_fixed{}, expand{}; };
    Buffer<GridPlan> grid_plans{BufferAllocator<GridPlan>{&memory}};
    Buffer<GridItem> grid_items{BufferAllocator<GridItem>{&memory}};
    Buffer<std::size_t> grid_item_for_work{BufferAllocator<std::size_t>{&memory}};
    Buffer<GridTrack> grid_tracks{BufferAllocator<GridTrack>{&memory}};
    Buffer<GridPrefix> grid_prefixes{BufferAllocator<GridPrefix>{&memory}};
    Buffer<std::uint64_t> grid_occupancy{BufferAllocator<std::uint64_t>{&memory}};
    Buffer<std::uint32_t> grid_locked_cursors{BufferAllocator<std::uint32_t>{&memory}};
    Buffer<std::size_t> grid_candidates{BufferAllocator<std::size_t>{&memory}};
    Buffer<double> grid_increases{BufferAllocator<double>{&memory}};
    Buffer<std::uint32_t> grid_touched_tracks{BufferAllocator<std::uint32_t>{&memory}};
    // Intrinsic Grid sizing can run while the parent is collecting its ordinary
    // shares. Keep track allocation scratch separate from that live parent state.
    Buffer<Share> grid_shares{BufferAllocator<Share>{&memory}};
    Buffer<std::uint32_t> grid_share_indices{BufferAllocator<std::uint32_t>{&memory}};
    Buffer<double> grid_share_output{BufferAllocator<double>{&memory}};
    Buffer<double> grid_weight_sums{BufferAllocator<double>{&memory}};
    Buffer<std::pair<double, std::size_t>> grid_events{BufferAllocator<std::pair<double, std::size_t>>{&memory}};

    void prepare_grid_work() {
        grid_items.clear();
        grid_tracks.clear();
        grid_prefixes.clear();
        const bool present = has_grid_work;
        grid_plans.assign(present ? work.size() : 0, {});
        grid_item_for_work.assign(present ? work.size() : 0, no_pending);
    }
    Error grid_error(std::uint32_t node, ErrorCode code, InputField field = InputField::grid) const {
        return {code, id(node), 0, field};
    }
    static std::uint64_t grid_mask(std::uint32_t column, std::uint32_t count) {
        const auto bits = count == 64 ? ~std::uint64_t{} : (std::uint64_t{1} << count) - 1;
        return bits << (column % 64);
    }
    struct GridCollision { std::uint32_t row{}, column{}; };
    std::optional<GridCollision> grid_collision(const GridItem& item, std::size_t stride) const {
        for (auto row = item.row; row < item.row + item.row_span; ++row) {
            for (auto column = item.column; column < item.column + item.column_span;) {
                const auto count = std::min<std::uint32_t>(64 - column % 64, item.column + item.column_span - column);
                const auto occupied = grid_occupancy[static_cast<std::size_t>(row) * stride + column / 64] & grid_mask(column, count);
                if (occupied) return GridCollision{row, column - column % 64 + static_cast<std::uint32_t>(std::countr_zero(occupied))};
                column += count;
            }
        }
        return {};
    }
    void grid_mark_area(const GridItem& item, std::size_t stride) {
        for (auto row = item.row; row < item.row + item.row_span; ++row) {
            for (auto column = item.column; column < item.column + item.column_span;) {
                const auto count = std::min<std::uint32_t>(64 - column % 64, item.column + item.column_span - column);
                grid_occupancy[static_cast<std::size_t>(row) * stride + column / 64] |= grid_mask(column, count);
                column += count;
            }
        }
    }
    Result<void> grid_extent(std::uint32_t index, GridPlan& plan, std::uint64_t columns,
                             std::uint64_t rows, bool occupancy) {
        if (columns > limits.max_grid_tracks || rows > limits.max_grid_tracks)
            return std::unexpected(grid_error(index, ErrorCode::capacity_exceeded));
        if (occupancy && columns != 0 && rows > limits.max_grid_cells / columns)
            return std::unexpected(grid_error(index, ErrorCode::capacity_exceeded));
        const auto previous_stride = (static_cast<std::size_t>(plan.columns) + 63) / 64;
        const auto stride = (static_cast<std::size_t>(columns) + 63) / 64;
        plan.columns = static_cast<std::uint32_t>(columns);
        plan.rows = static_cast<std::uint32_t>(rows);
        if (!occupancy) return {};
        if (stride != previous_stride) {
            grid_occupancy.assign(static_cast<std::size_t>(rows) * stride, 0);
            // A changed word stride changes existing row addresses. Rebuild from
            // already placed rectangles; growing rows with the same stride is cheap.
            for (std::size_t i = plan.begin; i < plan.begin + plan.count; ++i) {
                const auto& item = grid_items[i];
                if (item.row != no_slot && item.column != no_slot) grid_mark_area(item, stride);
            }
        } else grid_occupancy.resize(static_cast<std::size_t>(rows) * stride, 0);
        return {};
    }
    Result<void> prepare_grid(std::uint32_t index) {
        auto& plan = grid_plans[slots[index].work_index];
        if (plan.ready) return {};
        const auto& spec = *slots[index].input.grid_spec;
        if (spec.columns.size() > limits.max_grid_tracks || spec.rows.size() > limits.max_grid_tracks)
            return std::unexpected(grid_error(index, ErrorCode::capacity_exceeded));
        plan.begin = grid_items.size();
        std::uint64_t columns = spec.columns.size(), rows = spec.rows.size();
        bool automatic = false;
        // Check every extent before creating a product-sized placement workspace.
        for (const auto child : slots[index].children) {
            const auto& input = slots[child].input;
            if (!in_flow(input, slots[index].input)) continue;
            const auto& cell = input.cell;
            if (!cell.row_span || !cell.column_span)
                return std::unexpected(grid_error(child, ErrorCode::invalid_input, InputField::cell));
            columns = std::max(columns, static_cast<std::uint64_t>(cell.column.value_or(0)) + cell.column_span);
            rows = std::max(rows, static_cast<std::uint64_t>(cell.row.value_or(0)) + cell.row_span);
            if (columns > limits.max_grid_tracks || rows > limits.max_grid_tracks)
                return std::unexpected(grid_error(child, ErrorCode::capacity_exceeded, InputField::cell));
            automatic |= !cell.row || !cell.column;
            grid_item_for_work[slots[child].work_index] = grid_items.size();
            grid_items.push_back({child, cell.row.value_or(no_slot), cell.column.value_or(no_slot), cell.row_span, cell.column_span});
        }
        plan.count = grid_items.size() - plan.begin;
        if (automatic) {
            grid_occupancy.clear();
            if (auto status = grid_extent(index, plan, columns, rows, true); !status) return status;
            // grid_extent rebuilt explicit rectangles when it established the stride.
            const bool row_flow = spec.auto_flow == AutoFlow::row;
            auto primary = [&](const GridItem& item) { return row_flow ? item.column : item.row; };
            auto secondary = [&](const GridItem& item) { return row_flow ? item.row : item.column; };
            auto primary_span = [&](const GridItem& item) { return row_flow ? item.column_span : item.row_span; };
            auto secondary_span = [&](const GridItem& item) { return row_flow ? item.row_span : item.column_span; };
            auto assign = [&](GridItem& item, std::uint32_t p, std::uint32_t s) {
                if (row_flow) { item.column = p; item.row = s; }
                else { item.row = p; item.column = s; }
            };
            auto grow = [&](std::uint64_t p, std::uint64_t s) -> Result<void> {
                return grid_extent(index, plan,
                    std::max<std::uint64_t>(plan.columns, row_flow ? p : s),
                    std::max<std::uint64_t>(plan.rows, row_flow ? s : p), true);
            };
            // Locked secondary-axis items precede ordinary sparse auto placement.
            // Their per-line cursor prevents backfilling holes on that line.
            grid_locked_cursors.assign(row_flow ? plan.rows : plan.columns, 0);
            for (std::size_t i = plan.begin; i < plan.begin + plan.count; ++i) {
                auto& item = grid_items[i];
                if (primary(item) != no_slot || secondary(item) == no_slot) continue;
                const auto s = secondary(item);
                auto p = grid_locked_cursors[s];
                for (;;) {
                    if (auto status = grow(static_cast<std::uint64_t>(p) + primary_span(item),
                                           static_cast<std::uint64_t>(s) + secondary_span(item)); !status) return status;
                    auto candidate = item; assign(candidate, p, s);
                    const auto collision = grid_collision(candidate, (static_cast<std::size_t>(plan.columns) + 63) / 64);
                    if (!collision) {
                        item = candidate;
                        grid_mark_area(item, (static_cast<std::size_t>(plan.columns) + 63) / 64);
                        for (auto line = s; line < s + secondary_span(item); ++line)
                            grid_locked_cursors[line] = std::max(grid_locked_cursors[line], p + primary_span(item));
                        break;
                    }
                    p = (row_flow ? collision->column : collision->row) + 1;
                }
            }
            std::uint32_t cursor_p = 0, cursor_s = 0;
            for (std::size_t i = plan.begin; i < plan.begin + plan.count; ++i) {
                auto& item = grid_items[i];
                if (primary(item) != no_slot && secondary(item) != no_slot) continue;
                const auto specified = primary(item);
                auto p = specified == no_slot ? cursor_p : specified;
                auto s = cursor_s + (specified != no_slot && specified < cursor_p ? 1u : 0u);
                const auto primary_count = row_flow ? plan.columns : plan.rows;
                for (;;) {
                    if (static_cast<std::uint64_t>(p) + primary_span(item) > primary_count) { p = 0; ++s; }
                    if (auto status = grow(primary_count, static_cast<std::uint64_t>(s) + secondary_span(item)); !status) return status;
                    auto candidate = item; assign(candidate, p, s);
                    const auto collision = grid_collision(candidate, (static_cast<std::size_t>(plan.columns) + 63) / 64);
                    if (!collision) {
                        item = candidate;
                        grid_mark_area(item, (static_cast<std::size_t>(plan.columns) + 63) / 64);
                        cursor_p = p + primary_span(item); cursor_s = s;
                        break;
                    }
                    // Every intermediate start still covers this occupied cell.
                    // A full-primary-span item has only one legal primary start.
                    if (specified != no_slot || primary_span(item) == primary_count) {
                        s = (row_flow ? collision->row : collision->column) + 1;
                        if (specified == no_slot) p = 0;
                    } else p = (row_flow ? collision->column : collision->row) + 1;
                }
            }
        } else if (auto status = grid_extent(index, plan, columns, rows, false); !status) return status;
        plan.columns_begin = grid_tracks.size();
        grid_tracks.resize(grid_tracks.size() + plan.columns);
        plan.rows_begin = grid_tracks.size();
        grid_tracks.resize(grid_tracks.size() + plan.rows);
        // Uniform axes need only their rule class. Mixed axes retain inclusive
        // counts in compute-local storage for constant-time span classification.
        for (const bool horizontal_axis : {true, false}) {
            const auto count = horizontal_axis ? plan.columns : plan.rows;
            const auto& rules = horizontal_axis ? spec.columns : spec.rows;
            const auto implicit_rule = (horizontal_axis ? spec.implicit_column : spec.implicit_row).rule;
            auto& common_rule = horizontal_axis ? plan.columns_rule : plan.rows_rule;
            auto& prefix_begin = horizontal_axis ? plan.columns_prefix : plan.rows_prefix;
            common_rule = rules.empty() ? implicit_rule : rules.front().rule;
            bool uniform = true;
            for (const auto& rule : rules) {
                if (rule.rule != common_rule) { uniform = false; break; }
            }
            if (count > rules.size() && implicit_rule != common_rule) uniform = false;
            if (uniform) continue;
            prefix_begin = grid_prefixes.size();
            std::uint32_t non_fixed = 0, expand = 0;
            for (std::uint32_t track = 0; track < count; ++track) {
                const auto rule = grid_rule(index, horizontal_axis, track).rule;
                non_fixed += rule != SizeRule::fixed;
                expand += rule == SizeRule::expand;
                grid_prefixes.push_back({non_fixed, expand});
            }
        }
        plan.ready = true;
        return {};
    }
    const Axis& grid_rule(std::uint32_t index, bool horizontal_axis, std::uint32_t track) const {
        const auto& spec = *slots[index].input.grid_spec;
        const auto& rules = horizontal_axis ? spec.columns : spec.rows;
        return track < rules.size() ? rules[track] : (horizontal_axis ? spec.implicit_column : spec.implicit_row);
    }
    bool grid_contributes(std::uint32_t index, const GridItem& item, bool horizontal_axis, bool intrinsic) const {
        const auto start = horizontal_axis ? item.column : item.row;
        const auto count = horizontal_axis ? item.column_span : item.row_span;
        const auto& plan = grid_plans[slots[index].work_index];
        const auto offset = horizontal_axis ? plan.columns_prefix : plan.rows_prefix;
        if (offset == no_pending) {
            const auto rule = horizontal_axis ? plan.columns_rule : plan.rows_rule;
            return rule != SizeRule::fixed && (intrinsic || rule != SizeRule::expand);
        }
        const auto& last = grid_prefixes[offset + start + count - 1];
        const auto non_fixed_before = start ? grid_prefixes[offset + start - 1].non_fixed : 0;
        const auto expand_before = start ? grid_prefixes[offset + start - 1].expand : 0;
        return last.non_fixed != non_fixed_before &&
            (intrinsic || last.expand == expand_before);
    }
    bool grid_intrinsic_required(std::uint32_t child) {
        const auto index = slots[child].parent;
        const auto position = grid_item_for_work[slots[child].work_index];
        return position != no_pending && grid_contributes(index, grid_items[position], true, true);
    }
    Result<float> grid_size_axis(std::uint32_t index, bool horizontal_axis, std::optional<float> available) {
        const auto plan = grid_plans[slots[index].work_index];
        const auto offset = horizontal_axis ? plan.columns_begin : plan.rows_begin;
        const auto count = horizontal_axis ? plan.columns : plan.rows;
        const auto& spec = *slots[index].input.grid_spec;
        const double gap = horizontal_axis ? spec.column_gap : spec.row_gap;
        for (std::uint32_t track = 0; track < count; ++track) {
            const auto& rule = grid_rule(index, horizontal_axis, track);
            auto& value = grid_tracks[offset + track];
            value.size = rule.rule == SizeRule::fixed ? rule.value : rule.minimum;
            value.position = 0;
        }
        grid_candidates.clear();
        bool spanning = false;
        for (std::size_t i = plan.begin; i < plan.begin + plan.count; ++i) {
            if (!grid_contributes(index, grid_items[i], horizontal_axis, !available)) continue;
            grid_candidates.push_back(i);
            spanning |= (horizontal_axis ? grid_items[i].column_span : grid_items[i].row_span) > 1;
        }
        if (spanning) std::sort(grid_candidates.begin(), grid_candidates.end(), [&](std::size_t a, std::size_t b) {
            return (horizontal_axis ? grid_items[a].column_span : grid_items[a].row_span) <
                   (horizontal_axis ? grid_items[b].column_span : grid_items[b].row_span);
        });
        grid_increases.assign(grid_candidates.empty() ? 0 : count, 0);
        grid_touched_tracks.clear();
        auto plan_increase = [&](std::uint32_t track, double increase) {
            auto& planned = grid_increases[track];
            if (increase <= planned) return;
            if (planned == 0) grid_touched_tracks.push_back(track);
            planned = increase;
        };
        std::size_t begin = 0;
        while (begin < grid_candidates.size()) {
            const auto& first = grid_items[grid_candidates[begin]];
            const auto span = horizontal_axis ? first.column_span : first.row_span;
            auto end = begin + 1;
            while (end < grid_candidates.size()) {
                const auto& item = grid_items[grid_candidates[end]];
                if ((horizontal_axis ? item.column_span : item.row_span) != span) break;
                ++end;
            }
            for (auto candidate = begin; candidate < end; ++candidate) {
                const auto& item = grid_items[grid_candidates[candidate]];
                const auto start = horizontal_axis ? item.column : item.row;
                const auto& state = work_for(item.node);
                const double demand = horizontal_axis ? state.intrinsic_width : state.natural_height;
                if (span == 1) {
                    const auto& rule = grid_rule(index, horizontal_axis, start);
                    const auto current = grid_tracks[offset + start].size;
                    const double desired = rule.maximum ? std::min<double>(demand, *rule.maximum) : demand;
                    plan_increase(start, std::max(0.0, desired - current));
                    continue;
                }
                double occupied = gap * (span - 1);
                // Sizes and gaps are nonnegative. Once the shared batch base
                // meets demand, later tracks cannot create a positive deficit.
                for (auto track = start; track < start + span && occupied < demand; ++track)
                    occupied += grid_tracks[offset + track].size;
                if (occupied >= demand) continue;
                grid_shares.clear(); grid_share_indices.clear();
                for (auto track = start; track < start + span; ++track) {
                    const auto& rule = grid_rule(index, horizontal_axis, track);
                    const auto current = grid_tracks[offset + track].size;
                    if (rule.rule == SizeRule::fixed) continue;
                    grid_shares.push_back({0, 1, rule.maximum ? std::optional<double>(std::max(0.0, *rule.maximum - current)) : std::nullopt});
                    grid_share_indices.push_back(track);
                }
                allocate_slots(grid_shares, std::max(0.0, demand - occupied), grid_share_output, grid_events, grid_weight_sums);
                for (std::size_t track = 0; track < grid_share_indices.size(); ++track)
                    plan_increase(grid_share_indices[track], grid_share_output[track]);
            }
            // Every item in a span batch observes the same base sizes. Taking the
            // maximum planned increase makes dimensions independent of child order.
            for (const auto track : grid_touched_tracks) {
                grid_tracks[offset + track].size += grid_increases[track];
                grid_increases[track] = 0;
            }
            grid_touched_tracks.clear();
            begin = end;
        }
        if (available) {
            grid_shares.clear(); grid_share_indices.clear();
            double fixed = count ? gap * (count - 1) : 0;
            for (std::uint32_t track = 0; track < count; ++track) {
                const auto& rule = grid_rule(index, horizontal_axis, track);
                if (rule.rule == SizeRule::expand) {
                    grid_shares.push_back({rule.minimum, rule.value, rule.maximum ? std::optional<double>(*rule.maximum) : std::nullopt});
                    grid_share_indices.push_back(track);
                } else fixed += grid_tracks[offset + track].size;
            }
            allocate_slots(grid_shares, std::max(0.0, static_cast<double>(*available) - fixed), grid_share_output, grid_events, grid_weight_sums);
            for (std::size_t i = 0; i < grid_share_indices.size(); ++i) grid_tracks[offset + grid_share_indices[i]].size = grid_share_output[i];
        }
        double total = 0;
        for (std::uint32_t track = 0; track < count; ++track) {
            auto& value = grid_tracks[offset + track];
            value.position = total;
            if (auto result = coordinate(value.size, index); !result) return std::unexpected(result.error());
            total += value.size;
            if (track + 1 < count) total += gap;
        }
        return coordinate(total, index);
    }
    Result<float> grid_intrinsic(std::uint32_t index) { return grid_size_axis(index, true, {}); }
    double grid_area(const GridPlan& plan, const GridItem& item, bool horizontal_axis) const {
        const auto offset = horizontal_axis ? plan.columns_begin : plan.rows_begin;
        const auto start = horizontal_axis ? item.column : item.row;
        const auto span = horizontal_axis ? item.column_span : item.row_span;
        const auto& last = grid_tracks[offset + start + span - 1];
        return last.position + last.size - grid_tracks[offset + start].position;
    }
    Result<void> grid_widths(std::uint32_t index, float available) {
        if (auto status = prepare_grid(index); !status) return status;
        const auto plan = grid_plans[slots[index].work_index];
        for (std::size_t i = plan.begin; i < plan.begin + plan.count; ++i) {
            const auto item = grid_items[i];
            const auto& input = slots[item.node].input;
            if (alignment(item.node, false).first_baseline || alignment(item.node, true).first_baseline)
                return std::unexpected(grid_error(item.node, ErrorCode::unsupported_dependency, InputField::alignment));
            if (input.width.rule == SizeRule::flex || input.height.rule == SizeRule::flex)
                return std::unexpected(grid_error(item.node, ErrorCode::unsupported_dependency, InputField::flow));
            if (grid_contributes(index, item, true, false)) {
                auto width = intrinsic_width(item.node);
                if (!width) return std::unexpected(width.error());
            }
        }
        if (auto result = grid_size_axis(index, true, available); !result) return std::unexpected(result.error());
        const bool natural_rows = work_for(index).needs_height && slots[index].input.height.rule != SizeRule::fixed;
        for (std::size_t i = plan.begin; i < plan.begin + plan.count; ++i) {
            // cell_width may resolve nested intrinsic grids and grow shared buffers.
            const auto item = grid_items[i];
            auto slot_width = coordinate(grid_area(plan, item, true), item.node);
            if (!slot_width) return std::unexpected(slot_width.error());
            auto width = cell_width(item.node, *slot_width);
            if (!width) return std::unexpected(width.error());
            box_for(item.node).width = *width;
            work_for(item.node).slot_width = *slot_width;
            const auto& input = slots[item.node].input;
            work_for(item.node).needs_height |= input.height.rule != SizeRule::fixed &&
                (input.height.rule == SizeRule::content || !alignment(item.node, false).fill ||
                 grid_contributes(index, item, false, natural_rows));
        }
        return {};
    }
    Result<float> grid_heights(std::uint32_t index) {
        const auto& input = slots[index].input;
        if (work_for(index).needs_height && input.height.rule != SizeRule::fixed)
            return grid_size_axis(index, false, {});
        // Child measurement dependencies were established by grid_widths. Without
        // a natural-height consumer, only arrange needs to solve the row tracks.
        return 0;
    }
    Result<void> grid_arrange(std::uint32_t index, float width, float height) {
        (void)width;
        if (auto result = grid_size_axis(index, false, height); !result) return std::unexpected(result.error());
        const auto plan = grid_plans[slots[index].work_index];
        const auto& parent = slots[index].input;
        for (std::size_t i = plan.begin; i < plan.begin + plan.count; ++i) {
            const auto& item = grid_items[i];
            const auto& input = slots[item.node].input;
            auto slot_height = coordinate(grid_area(plan, item, false), item.node);
            if (!slot_height) return std::unexpected(slot_height.error());
            work_for(item.node).slot_height = *slot_height;
            const auto align_y = alignment(item.node, false), align_x = alignment(item.node, true);
            double desired = input.height.rule == SizeRule::fixed ? input.height.value :
                (input.height.rule == SizeRule::content ? work_for(item.node).natural_height : *slot_height);
            if (input.height.rule == SizeRule::expand && !align_y.fill)
                desired = std::min<double>(desired, work_for(item.node).natural_height);
            auto actual_height = constrained(desired, input.height, vertical(input.padding), item.node);
            if (!actual_height) return std::unexpected(actual_height.error());
            auto& box = box_for(item.node);
            box.height = *actual_height;
            auto x = coordinate(parent.padding.left + grid_tracks[plan.columns_begin + item.column].position +
                (static_cast<double>(work_for(item.node).slot_width) - box.width) * align_x.offset, item.node);
            auto y = coordinate(parent.padding.top + grid_tracks[plan.rows_begin + item.row].position +
                (static_cast<double>(*slot_height) - box.height) * align_y.offset, item.node);
            if (!x) return std::unexpected(x.error());
            if (!y) return std::unexpected(y.error());
            box.x = *x; box.y = *y;
        }
        return {};
    }
