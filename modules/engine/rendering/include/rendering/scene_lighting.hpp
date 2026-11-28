#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include "rendering/sky_map.hpp"

// Scene-owned authoring values shared by World and rendering. No GPU resources,
// camera position, rolling state or diagnostic preferences are persisted here.
namespace atelier::rendering {

// FP32 sampling input budget, separate from the final HDR storage range.
inline constexpr double analytic_light_radiance_limit = 0x1p32;

inline const char* analytic_light_range_error(math::Vector3 color, float intensity, double area = 1) noexcept {
    if (!std::isfinite(area) || area <= 0 || area > std::numeric_limits<float>::max())
        return "Analytic light area exceeds GPU float representation";
    if (!std::isfinite(intensity) || intensity < 0.f)
        return "Analytic light color * intensity and rectangle area-weighted radiance must not exceed 2^32 per channel";
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(color[axis]) || color[axis] < 0.f)
            return "Analytic light color * intensity and rectangle area-weighted radiance must not exceed 2^32 per channel";
        const double radiance = double(color[axis]) * intensity;
        if (!std::isfinite(radiance) || radiance < 0 || radiance > analytic_light_radiance_limit ||
            radiance * area > analytic_light_radiance_limit)
            return "Analytic light color * intensity and rectangle area-weighted radiance must not exceed 2^32 per channel";
    }
    return nullptr;
}

enum class Antialiasing : std::uint32_t { none, smaa, taa };

enum class GiQuality : std::uint32_t { high, ultra };

enum class SkyMode : std::uint32_t { none, physical_atmosphere, hdri_sky_map };

enum class Tonemapper : std::uint32_t { pbr_neutral, agx };

struct SceneLightingSettings {
    bool operator==(const SceneLightingSettings&) const = default;

    bool shadows{true};

    float contact_shadow_length{.15f};

    bool indirect_lighting{true};

    GiQuality gi_quality{GiQuality::high};

    float indirect_gain{1.f};

    bool ssilvb{true};

    float ao_radius{.35f};

    bool reflections{true};

    float reflection_distance{30.f};

    float reflection_roughness{.6f};

    SkyMode sky_mode{SkyMode::none};

    math::Vector3 sky_color{1.f, 1.f, 1.f};

    float sky_intensity{.03f};

    SkyMapRef sky_map{};

    float sky_rotation{};

    bool fog_enabled{};

    float fog_density{.01f};

    float fog_height_falloff{.2f};

    float fog_anisotropy{.3f};

    float exposure_ev{};

    bool bloom{true};

    Tonemapper tonemapper{Tonemapper::agx};

    Antialiasing antialiasing{Antialiasing::smaa};

    std::uint32_t direct_candidates{8};
};

// The Frame environment radiance the host passes to the renderer; zero when the sky is off.
inline math::Vector3 sky_radiance(const SceneLightingSettings& settings) noexcept {
    if (settings.sky_mode == SkyMode::none)
        return {};
    return settings.sky_color * settings.sky_intensity;
}

// Authoring ranges are stricter than the low-level renderer's frame contract.
inline const char* scene_lighting_error(const SceneLightingSettings& settings) noexcept {
    const auto range = [](float v, float low, float high) { return std::isfinite(v) && v >= low && v <= high; };
    if (!range(settings.contact_shadow_length, 0.f, 2.f) || settings.gi_quality > GiQuality::ultra ||
        !range(settings.indirect_gain, 0.f, 8.f) || !range(settings.ao_radius, .001f, 100.f) ||
        !range(settings.reflection_distance, .001f, 10000.f) || !range(settings.reflection_roughness, 0.f, 1.f) ||
        settings.sky_mode > SkyMode::hdri_sky_map || !range(settings.fog_density, 0.f, 10.f) ||
        !range(settings.fog_height_falloff, .001f, 10.f) || !range(settings.fog_anisotropy, -.95f, .95f) ||
        !range(settings.exposure_ev, -16.f, 16.f) || settings.tonemapper > Tonemapper::agx ||
        settings.antialiasing > Antialiasing::taa || settings.direct_candidates < 1 ||
        settings.direct_candidates > 32)
        return "Invalid World lighting, GI, atmosphere or post-processing settings";
    if (!range(settings.sky_rotation, -180.f, 180.f))
        return "Sky rotation must be finite degrees within [-180, 180]";
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float channel = settings.sky_color[axis];
        const double radiance = double(channel) * settings.sky_intensity;
        if (!std::isfinite(channel) || channel < 0.f || !std::isfinite(settings.sky_intensity) ||
            settings.sky_intensity < 0.f || radiance > 65504.)
            return "Sky radiance must be finite, non-negative and within the HDR scene range";
    }
    return nullptr;
}

} // namespace atelier::rendering
