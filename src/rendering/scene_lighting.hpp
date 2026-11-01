#pragma once
#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include "rendering/sky_map.hpp"

// Scene-owned authoring values shared by World and rendering. No GPU resources,
// camera position, rolling state or diagnostic preferences are persisted here.
namespace atelier::rendering {
// FP32 sampling input budget, separate from the final HDR storage range.
// With bounded analytic attenuation (<2^9), BRDF (<2^28), the uniform
// proposal floor and <=32 initial candidates, 2^32 leaves headroom for
// the evaluator and initial sum. It does not bound every reservoir W.
inline constexpr double analytic_light_radiance_limit = 0x1p32;

inline const char* analytic_light_range_error(const std::array<float, 3>& color, float intensity,
                                              double area = 1) noexcept {
    if (!std::isfinite(area) || area <= 0 || area > std::numeric_limits<float>::max())
        return "Analytic light area exceeds GPU float representation";
    for (float channel : color) {
        const double radiance = double(channel) * intensity;
        if (!std::isfinite(radiance) || radiance < 0 || radiance > analytic_light_radiance_limit ||
            radiance * area > analytic_light_radiance_limit)
            return "Analytic light color * intensity and rectangle area-weighted radiance must not exceed 2^32 per channel";
    }
    return nullptr;
}

enum class Antialiasing : std::uint32_t { none, taa, smaa, smaa_t2x };

enum class ProbePlacement : std::uint32_t { fixed, follow_focus };

inline constexpr std::size_t probe_volume_limit = 4;

inline constexpr std::uint32_t probe_resident_limit = 4096;

struct ProbeVolumeSettings {
    bool operator==(const ProbeVolumeSettings&) const = default;

    bool enabled{};

    ProbePlacement placement{ProbePlacement::fixed};

    // World bounds for fixed placement; offsets from Frame::lighting_focus
    // for follow_focus. Following snaps to whole cells without changing spacing.
    std::array<float, 3> minimum{-6.f, -1.f, -6.f};

    std::array<float, 3> maximum{6.f, 5.f, 6.f};

    std::array<std::uint32_t, 3> counts{8, 4, 8};

    float blend_cells{}; // Border transition measured in cells; 0..2.
};

struct SceneLightingSettings {
    bool operator==(const SceneLightingSettings&) const = default;

    bool shadows{true};

    bool ddgi{true};

    bool gtao{true};

    bool sssr{true};

    bool bloom{true};

    Antialiasing antialiasing{Antialiasing::smaa};

    float exposure_ev{}; // Display exposure; HDR histories remain scene linear.

    std::uint32_t direct_candidates{
        8}; // 1..32 local-light candidates per DDGI probe-ray hit; screen direct light evaluates every listed light.

    float ao_radius{0.35f};

    float reflection_distance{30.f};

    float reflection_roughness{0.6f};

    // Stable slot identity and priority, finest/local support first. All slots
    // share one resident/update budget; valid darkness retains its blend weight.
    std::array<ProbeVolumeSettings, probe_volume_limit> probe_volumes = [] {
        std::array<ProbeVolumeSettings, probe_volume_limit> volumes{};
        volumes[0].enabled = true;
        return volumes;
    }();

    std::uint32_t probe_update_budget{64}; // Probes per frame, including initialization.

    std::uint32_t probe_rays{128}; // At most 256 rays per probe update.

    float probe_bounce{.85f}; // Feedback transport, 0..0.99; changing it invalidates the field.

    float indirect_gain{1.f}; // Final diffuse composition only, 0..8; never feeds the field.

    // Incident sky radiance sky_color * sky_intensity * map (cd/m^2) per channel; map = 1
    // without sky_map. The map turns about +Y by sky_rotation degrees in the sense of a
    // Transform rotation about +Y (+X toward -Z).
    bool sky_enabled{};

    std::array<float, 3> sky_color{1.f, 1.f, 1.f};

    float sky_intensity{.03f};

    SkyMapRef sky_map{};

    float sky_rotation{};
};

// The Frame environment radiance the host passes to the renderer; zero when the sky is off.
inline std::array<float, 3> sky_radiance(const SceneLightingSettings& settings) noexcept {
    if (!settings.sky_enabled)
        return {};
    return {settings.sky_color[0] * settings.sky_intensity, settings.sky_color[1] * settings.sky_intensity,
            settings.sky_color[2] * settings.sky_intensity};
}

inline const char* probe_layout_error(const SceneLightingSettings& settings) noexcept {
    if (settings.probe_update_budget < 1 || settings.probe_update_budget > probe_resident_limit ||
        settings.probe_rays < 1 || settings.probe_rays > 256)
        return "Invalid shared DDGI update budget";
    if (!std::isfinite(settings.probe_bounce) || settings.probe_bounce < 0 || settings.probe_bounce > .99f ||
        !std::isfinite(settings.indirect_gain) || settings.indirect_gain < 0 || settings.indirect_gain > 8)
        return "Invalid DDGI transport or indirect gain";
    std::uint64_t resident = 0;
    for (const auto& volume : settings.probe_volumes) {
        if (volume.placement > ProbePlacement::follow_focus || !std::isfinite(volume.blend_cells) ||
            volume.blend_cells < 0 || volume.blend_cells > 2)
            return "Invalid DDGI placement or blend border";
        std::uint64_t count = 1;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float low = volume.minimum[axis];
            const float high = volume.maximum[axis];
            const auto n = volume.counts[axis];
            if (!std::isfinite(low) || !std::isfinite(high) || low < -100000 || high > 100000 || high <= low || n < 2 ||
                n > 32)
                return "Invalid DDGI volume bounds or counts";
            if (volume.placement == ProbePlacement::follow_focus &&
                float(n) < 3 + 2 * std::max(1.f, volume.blend_cells))
                return "Following DDGI volumes need an interior beyond their blend border (at least five probes per axis)";
            const double spacing = (double(high) - low) / (n - 1);
            if (spacing < .001 ||
                spacing < std::max(std::abs(double(low)), std::abs(double(high))) * 4 * 1.1920928955078125e-7)
                return "DDGI spacing is below supported world precision";
            if (volume.blend_cells * 2 > float(n - 1))
                return "DDGI blend border exceeds half the volume";
            count *= n;
        }
        if (volume.enabled)
            resident += count;
    }
    return resident > probe_resident_limit ? "DDGI volumes exceed the shared 4096 probe budget" : nullptr;
}

// Authoring ranges are stricter than the low-level renderer's frame contract.
inline const char* scene_lighting_error(const SceneLightingSettings& settings) noexcept {
    if (const auto* error = probe_layout_error(settings))
        return error;
    const auto range = [](float v, float low, float high) { return std::isfinite(v) && v >= low && v <= high; };
    if (settings.antialiasing > Antialiasing::smaa_t2x || !range(settings.exposure_ev, -16, 16) ||
        settings.direct_candidates < 1 || settings.direct_candidates > 32 || !range(settings.ao_radius, .001f, 100) ||
        !range(settings.reflection_distance, .001f, 10000) || !range(settings.reflection_roughness, 0, 1))
        return "Invalid World lighting or effects settings";
    if (!range(settings.sky_rotation, -180, 180))
        return "Sky rotation must be finite degrees within [-180, 180]";
    for (float channel : settings.sky_color) {
        const double radiance = double(channel) * settings.sky_intensity;
        if (!std::isfinite(channel) || channel < 0 || !std::isfinite(settings.sky_intensity) ||
            settings.sky_intensity < 0 || radiance > 65504)
            return "Sky radiance must be finite, non-negative and within the HDR scene range";
    }
    return nullptr;
}

// Validate the candidate settings together with their actual world-space anchor
// before either is published. Relative bounds alone cannot prove float precision.
inline const char* scene_lighting_focus_error(const SceneLightingSettings& settings,
                                              const std::array<float, 3>& focus) noexcept {
    if (const auto* error = probe_layout_error(settings))
        return error;
    for (float component : focus)
        if (!std::isfinite(component) || std::abs(component) > 100000)
            return "Invalid DDGI lighting focus";
    for (const auto& volume : settings.probe_volumes)
        if (volume.enabled && volume.placement == ProbePlacement::follow_focus)
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double spacing =
                    (double(volume.maximum[axis]) - volume.minimum[axis]) / (volume.counts[axis] - 1);
                const double magnitude =
                    std::abs(double(focus[axis])) +
                    std::max(std::abs(double(volume.minimum[axis])), std::abs(double(volume.maximum[axis])));
                if (spacing < magnitude * 4 * 1.1920928955078125e-7)
                    return "DDGI spacing is below precision at the world lighting focus";
            }
    return nullptr;
}

} // namespace atelier::rendering
