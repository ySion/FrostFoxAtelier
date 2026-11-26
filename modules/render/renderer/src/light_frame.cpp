#include "detail/light_frame.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

namespace atelier::renderer::detail {
namespace {
Result<void> check(bool condition, const char* message) {
    if (!condition)
        return std::unexpected(Error{{rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, message}, false});
    return {};
}

Result<float> gpu_float(double value) {
    ATELIER_TRY(check(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
                      "Light source exceeds GPU float representation"));
    return static_cast<float>(value);
}

Result<void> source_capacity(std::size_t count) {
    // GFX structured buffers use uint32 element counts and uint64 byte sizes.
    // Reserve ~0 as the invalid index; allocation limits remain RHI-owned.
    return check(count < no_light_source && count <= std::numeric_limits<std::size_t>::max() / sizeof(GpuLightSource),
                 "Light source buffer exceeds addressable storage");
}

double luminance(const std::array<float, 4>& value) {
    return .2126 * value[0] + .7152 * value[1] + .0722 * value[2];
}

Result<GpuLightSource> analytic_source(const Light& light) {
    GpuLightSource result;
    result.origin_range = {light.position[0], light.position[1], light.position[2], light.range};
    result.direction_radius = {light.direction[0], light.direction[1], light.direction[2], light.source_radius};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        ATELIER_TRY_BIND(radiance, gpu_float(double(light.color[axis]) * light.intensity));
        result.radiance[axis] = *radiance;
        if (light.kind == LightKind::rectangle) {
            ATELIER_TRY_BIND(right, gpu_float(double(light.right[axis]) * light.width));
            result.edge1_area[axis] = *right;
            ATELIER_TRY_BIND(up, gpu_float(double(light.up[axis]) * light.height));
            result.edge2[axis] = *up;
        }
    }
    if (light.kind == LightKind::rectangle) {
        ATELIER_TRY_BIND(area, gpu_float(double(light.width) * light.height));
        result.edge1_area[3] = *area;
    }
    result.meta = {static_cast<std::uint32_t>(light.kind), (light.two_sided ? 1u : 0u) | (light.casts_shadow ? 2u : 0u),
                   0, 0};
    return result;
}

double analytic_power(const GpuLightSource& source) {
    const double flux = source.meta[0] == 1 ? std::numbers::pi * source.edge1_area[3] * ((source.meta[1] & 1u) ? 2 : 1)
                                            : 4 * std::numbers::pi;
    return std::max(0., luminance(source.radiance) * flux);
}

bool light_grid_origin(math::Vector3 focus, std::array<float, 4>& origin) {
    origin = {0, 0, 0, light_grid_spacing};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(focus[axis]))
            return false;
        const double minimum =
            (std::floor(double(focus[axis]) / light_grid_spacing) - double(light_grid_counts[axis] / 2)) *
            light_grid_spacing;
        if (!std::isfinite(minimum) || std::abs(minimum) > std::numeric_limits<float>::max())
            return false;
        origin[axis] = static_cast<float>(minimum);
        if (double(origin[axis]) != minimum)
            return false;
        // Validate every edge before any integer conversion. Large finite
        // worlds may still render globally while eight-metre cells collapse.
        float previous = origin[axis];
        for (std::uint32_t edge = 1; edge <= light_grid_counts[axis]; ++edge) {
            const double position = minimum + double(edge) * light_grid_spacing;
            if (std::abs(position) > std::numeric_limits<float>::max())
                return false;
            const float current = static_cast<float>(position);
            if (!std::isfinite(current) || double(current) != position || current - previous != light_grid_spacing)
                return false;
            previous = current;
        }
    }
    return true;
}
}

std::size_t LightFrame::KeyHash::operator()(const SourceKey& key) const noexcept {
    return hash_fields(key.owner, key.kind);
}

void LightFrame::clear() {
    sources.clear();
    previous_sources_.clear();
    proposals.clear();
    current_to_previous.clear();
    analytic_.clear();
    next_analytic_.clear();
    analytic_keys_.clear();
    next_analytic_keys_.clear();
    keys_.clear();
    previous_keys_.clear();
    previous_lookup_.clear();
    powers_.clear();
    scaled_.clear();
    actual_.clear();
    below_.clear();
    above_.clear();
    spatial_origin = {};
    spatial_cells.clear();
    spatial_proposals.clear();
    spatial_source_slots.clear();
    spatial_bins_.clear();
    local_count = directional_count = 0;
    scene_identity_ = frame_id_ = 0;
    source_revision_ = spatial_source_revision_ = spatial_frame_id_ = 0;
    spatial_nonempty_cells = 0;
    spatial_initialized_ = spatial_changed = false;
    valid_ = advance_previous_ = changed = radiance_changed = false;
}

Result<void> LightFrame::update(const Frame& frame, std::uint64_t sequence) {
    auto result = update_impl(frame, sequence);
    if (result)
        result = update_spatial(frame.lighting_focus, sequence);
    if (!result)
        clear();
    return result;
}

Result<void> LightFrame::update_impl(const Frame& frame, std::uint64_t sequence) {
    if (valid_ && scene_identity_ != frame.scene_identity)
        clear();
    ATELIER_TRY(source_capacity(frame.lights.size()));
    bool rebuild = !valid_;
    next_analytic_.clear();
    next_analytic_keys_.clear();
    // Keep the same order inside each partition. Stable identity, not the
    // packed index, determines temporal correspondence after any reorder.
    for (bool directional : {false, true})
        for (const auto& light : frame.lights) {
            if ((light.kind == LightKind::directional) != directional)
                continue;
            ATELIER_TRY_BIND(source, analytic_source(light));
            next_analytic_.push_back(*source);
            next_analytic_keys_.push_back({light.stable_key, static_cast<std::uint32_t>(light.kind)});
        }
    const bool rebuild_sources = rebuild || next_analytic_ != analytic_ || next_analytic_keys_ != analytic_keys_;
    analytic_.swap(next_analytic_);
    analytic_keys_.swap(next_analytic_keys_);
    // Several passes may request the same immutable frame snapshot. Preserve
    // both its correspondence and its publication flag on repeated calls.
    if (!rebuild_sources && sequence == frame_id_)
        return {};
    changed = rebuild_sources;
    radiance_changed = false;
    if (!rebuild_sources) {
        frame_id_ = sequence;
        if (advance_previous_) {
            for (std::size_t i = 0; i < sources.size(); ++i)
                current_to_previous[i] = keys_[i].owner ? static_cast<std::uint32_t>(i) : no_light_source;
            advance_previous_ = false;
        }
        return {};
    }

    previous_sources_.swap(sources);
    sources.clear();
    previous_keys_.swap(keys_);
    keys_.clear();
    powers_.clear();
    directional_count = 0;
    const auto append = [&](std::size_t i) {
        sources.push_back(analytic_[i]);
        keys_.push_back(analytic_keys_[i]);
    };
    for (std::size_t i = 0; i < analytic_.size(); ++i)
        if (analytic_[i].meta[0] != 2) {
            append(i);
            powers_.push_back(analytic_power(analytic_[i]));
        }
    local_count = static_cast<std::uint32_t>(sources.size());
    for (std::size_t i = 0; i < analytic_.size(); ++i)
        if (analytic_[i].meta[0] == 2) {
            append(i);
            ++directional_count;
        }
    ATELIER_TRY(build_proposals());
    map_previous();
    ++source_revision_;
    radiance_changed = !valid_ || sources.size() != previous_sources_.size();
    if (!radiance_changed)
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto previous = current_to_previous[i];
            if (previous == no_light_source || sources[i] != previous_sources_[previous]) {
                radiance_changed = true;
                break;
            }
        }
    scene_identity_ = frame.scene_identity;
    frame_id_ = sequence;
    valid_ = true;
    advance_previous_ = true;
    return {};
}

Result<void> LightFrame::build_proposals() {
    proposals.assign(local_count, {});
    if (local_count == 0)
        return {};
    scaled_.resize(local_count);
    actual_.assign(local_count, 0);
    below_.clear();
    above_.clear();
    double total = 0;
    for (double power : powers_) {
        ATELIER_TRY(check(std::isfinite(power) && power >= 0, "Light proposal power is invalid"));
        total += power;
    }
    ATELIER_TRY(check(std::isfinite(total), "Light proposal power sum is invalid"));
    for (std::uint32_t i = 0; i < local_count; ++i) {
        scaled_[i] = total > 0 ? .95 * (powers_[i] / total) * local_count + .05 : 1;
        proposals[i].accept = 1;
        proposals[i].alias = i;
        (scaled_[i] < 1 ? below_ : above_).push_back(i);
    }
    while (!below_.empty() && !above_.empty()) {
        const auto underfull = below_.back();
        below_.pop_back();
        const auto large = above_.back();
        above_.pop_back();
        proposals[underfull].accept = static_cast<float>(std::clamp(scaled_[underfull], 0., 1.));
        proposals[underfull].alias = large;
        scaled_[large] += scaled_[underfull] - 1;
        (scaled_[large] < 1 ? below_ : above_).push_back(large);
    }
    for (std::uint32_t i = 0; i < local_count; ++i) {
        const double accept = proposals[i].accept;
        actual_[i] += accept / local_count;
        actual_[proposals[i].alias] += (1 - accept) / local_count;
    }
    for (std::uint32_t i = 0; i < local_count; ++i)
        proposals[i].probability = static_cast<float>(actual_[i]);
    return {};
}

Result<void> LightFrame::update_spatial(math::Vector3 focus, std::uint64_t sequence) {
    if (!spatial_initialized_ || sequence != spatial_frame_id_)
        spatial_changed = false;
    std::array<float, 4> origin{};
    if (local_count == 0 || !light_grid_origin(focus, origin))
        origin = {};
    if (spatial_initialized_ && spatial_source_revision_ == source_revision_ && origin == spatial_origin) {
        spatial_frame_id_ = sequence;
        return {};
    }
    spatial_nonempty_cells = 0;
    spatial_origin = origin;
    spatial_cells.clear();
    spatial_proposals.clear();
    spatial_source_slots.clear();
    spatial_source_revision_ = source_revision_;
    spatial_frame_id_ = sequence;
    spatial_initialized_ = spatial_changed = true;
    if (origin[3] == 0)
        return {};

    spatial_cells.resize(light_grid_cell_count);
    spatial_bins_.resize(light_grid_cell_count);
    // A min heap under this ordering keeps the worst retained source at its
    // root. Stable source identity resolves equal weights independently of
    // packing order; ambiguous legacy identities use the packed index last.
    const auto better = [&](std::uint32_t a, std::uint32_t b) {
        const float pa = proposals[a].probability;
        const float pb = proposals[b].probability;
        if (pa != pb)
            return pa > pb;
        const auto& ka = keys_[a];
        const auto& kb = keys_[b];
        if (ka.owner != kb.owner)
            return ka.owner < kb.owner;
        if (ka.kind != kb.kind)
            return ka.kind < kb.kind;
        return a < b;
    };
    for (std::uint32_t source = 0; source < local_count; ++source) {
        const auto& light = sources[source];
        std::array<std::uint32_t, 3> cell{};
        bool inside = true;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double coordinate = (double(light.origin_range[axis]) - double(origin[axis])) / light_grid_spacing;
            if (!std::isfinite(coordinate) || coordinate < 0 || coordinate >= light_grid_counts[axis]) {
                inside = false;
                break;
            }
            cell[axis] = static_cast<std::uint32_t>(std::floor(coordinate));
        }
        if (!inside)
            continue;
        const auto index = cell[0] + light_grid_counts[0] * (cell[1] + light_grid_counts[1] * cell[2]);
        auto& count = spatial_cells[index].count;
        auto& heap = spatial_bins_[index];
        if (count < light_grid_cell_capacity) {
            heap[count++] = source;
            std::push_heap(heap.begin(), heap.begin() + count, better);
        } else if (better(source, heap[0])) {
            std::pop_heap(heap.begin(), heap.begin() + count, better);
            heap[count - 1] = source;
            std::push_heap(heap.begin(), heap.begin() + count, better);
        }
    }

    std::size_t retained = 0;
    for (const auto& cell : spatial_cells)
        retained += cell.count;
    if (retained == 0) {
        spatial_cells.clear();
        return {};
    }
    spatial_proposals.reserve(retained);
    spatial_source_slots.assign(local_count, no_light_source);
    for (std::uint32_t cell = 0; cell < light_grid_cell_count; ++cell) {
        auto& header = spatial_cells[cell];
        const auto count = header.count;
        if (count == 0)
            continue;
        ++spatial_nonempty_cells;
        auto& selected = spatial_bins_[cell];
        std::sort(selected.begin(), selected.begin() + count, better);
        header.offset = static_cast<std::uint32_t>(spatial_proposals.size());
        double mass = 0;
        for (std::uint32_t slot = 0; slot < count; ++slot)
            mass += proposals[selected[slot]].probability;
        ATELIER_TRY(check(std::isfinite(mass) && mass > 0, "Spatial light proposal mass is invalid"));
        header.mass = static_cast<float>(mass);
        std::array<double, light_grid_cell_capacity> scaled{};
        std::array<double, light_grid_cell_capacity> actual{};
        std::array<std::uint32_t, light_grid_cell_capacity> below{};
        std::array<std::uint32_t, light_grid_cell_capacity> above{};
        std::uint32_t below_count = 0;
        std::uint32_t above_count = 0;
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            const auto source = selected[slot];
            spatial_proposals.push_back({source, 1, slot, 0});
            spatial_source_slots[source] = (cell << 5) | slot;
            scaled[slot] = (double(proposals[source].probability) / mass) * count;
            if (scaled[slot] < 1)
                below[below_count++] = slot;
            else
                above[above_count++] = slot;
        }
        while (below_count && above_count) {
            const auto under = below[--below_count];
            const auto over = above[--above_count];
            auto& proposal = spatial_proposals[header.offset + under];
            proposal.accept = static_cast<float>(std::clamp(scaled[under], 0., 1.));
            proposal.alias = over;
            scaled[over] += scaled[under] - 1;
            if (scaled[over] < 1)
                below[below_count++] = over;
            else
                above[above_count++] = over;
        }
        // As with the global alias table, publish probabilities derived from
        // the actual float thresholds, rather than the intended CPU weights.
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            const auto& proposal = spatial_proposals[header.offset + slot];
            const double accept = proposal.accept;
            actual[slot] += accept / count;
            actual[proposal.alias] += (1 - accept) / count;
        }
        for (std::uint32_t slot = 0; slot < count; ++slot)
            spatial_proposals[header.offset + slot].probability = static_cast<float>(actual[slot]);
    }
    return {};
}

void LightFrame::map_previous() {
    current_to_previous.assign(sources.size(), no_light_source);
    previous_lookup_.clear();
    previous_lookup_.reserve(previous_keys_.size());
    // Frame validation keeps nonzero light identities unique; zero has none.
    for (std::size_t i = 0; i < previous_keys_.size(); ++i)
        if (previous_keys_[i].owner)
            previous_lookup_.try_emplace(previous_keys_[i], static_cast<std::uint32_t>(i));
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (!keys_[i].owner)
            continue;
        if (const auto* at = previous_lookup_.find(keys_[i]))
            current_to_previous[i] = *at;
    }
}

} // namespace atelier::renderer::detail
