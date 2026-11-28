#include "lighting_lut.hpp"
#include "ltc_lut.hpp"
#include "smaa_lut.hpp"

#include <cmath>
#include <numbers>

namespace atelier::renderer::detail {

Result<void> LightingLutSet::ensure(rhi::Device& device) {
    if (initialized)
        return {};

    bool ok = false;
    struct CleanupGuard {
        LightingLutSet* self;

        rhi::Device* dev;

        bool* ok;

        ~CleanupGuard() {
            if (!*ok)
                self->retire(*dev);
        }
    } guard{this, &device, &ok};

    UploadBatch batch(device, true);

    // 1. 64x64 GGX LTC Matrix & Amplitude LUTs (exact fit to separable Smith GGX + Schlick)
    constexpr std::uint32_t ltc_n = ltc_lut::size;
    Array<float> mat_data(ltc_n * ltc_n * 4u);
    Array<float> amp_data(ltc_n * ltc_n * 2u);
    for (std::uint32_t i = 0u; i < ltc_n * ltc_n; ++i) {
        mat_data[i * 4u + 0u] = ltc_lut::table[i * 6u + 0u];
        mat_data[i * 4u + 1u] = ltc_lut::table[i * 6u + 1u];
        mat_data[i * 4u + 2u] = ltc_lut::table[i * 6u + 2u];
        mat_data[i * 4u + 3u] = ltc_lut::table[i * 6u + 3u];
        amp_data[i * 2u + 0u] = ltc_lut::table[i * 6u + 4u];
        amp_data[i * 2u + 1u] = ltc_lut::table[i * 6u + 5u];
    }
    ATELIER_TRY_ASSIGN(ltc_matrix, batch.stage_texture_2d(nri::Format::RGBA32_SFLOAT, ltc_n, ltc_n, mat_data.data(),
                                                          ltc_n * 16u, "ltc_mat"));
    ATELIER_TRY_ASSIGN(ltc_amplitude, batch.stage_texture_2d(nri::Format::RG32_SFLOAT, ltc_n, ltc_n, amp_data.data(),
                                                             ltc_n * 8u, "ltc_amp"));

    // 2. 32x32 Charlie Sheen Directional Albedo LUT (Estevez & Kulla 2017)
    constexpr std::uint32_t cn = 32u;
    Array<std::uint16_t> charlie_half(cn * cn);
    for (std::uint32_t y = 0u; y < cn; ++y) {
        const double nv = std::max(1e-3, (double(y) + 0.5) / double(cn));
        const double sin_v = std::sqrt(std::max(0.0, 1.0 - nv * nv));
        for (std::uint32_t x = 0u; x < cn; ++x) {
            const double r = std::clamp((double(x) + 0.5) / double(cn), 0.04, 1.0);
            const double inv_r = 1.0 / r;
            double accum = 0.0;
            constexpr std::uint32_t steps_theta = 24u;
            constexpr std::uint32_t steps_phi = 32u;
            const double d_theta = (0.5 * std::numbers::pi) / double(steps_theta);
            const double d_phi = (2.0 * std::numbers::pi) / double(steps_phi);
            for (std::uint32_t it = 0u; it < steps_theta; ++it) {
                const double th = (double(it) + 0.5) * d_theta;
                const double nl = std::cos(th);
                const double sin_l = std::sin(th);
                for (std::uint32_t ip = 0u; ip < steps_phi; ++ip) {
                    const double ph = (double(ip) + 0.5) * d_phi;
                    const double lx = sin_l * std::cos(ph);
                    const double ly = sin_l * std::sin(ph);
                    const double hx = sin_v + lx;
                    const double hy = ly;
                    const double hz = nv + nl;
                    const double h_len = std::sqrt(std::max(1e-12, hx * hx + hy * hy + hz * hz));
                    const double nh = std::clamp(hz / h_len, 0.0, 1.0);
                    const double sin_h = std::sqrt(std::max(1e-8, 1.0 - nh * nh));
                    const double d_charlie = (2.0 + inv_r) * std::pow(sin_h, inv_r) / (2.0 * std::numbers::pi);
                    const double vis = 1.0 / std::max(4.0 * (nl + nv - nl * nv), 0.1);
                    accum += d_charlie * vis * nl * sin_l * d_theta * d_phi;
                }
            }
            charlie_half[y * cn + x] = fp32_to_fp16(static_cast<float>(std::clamp(accum, 0.0, 1.0)));
        }
    }
    ATELIER_TRY_ASSIGN(charlie_lut, batch.stage_texture_2d(nri::Format::R16_SFLOAT, cn, cn, charlie_half.data(),
                                                           cn * 2u, "charlie_lut"));

    // 3. Hillaire 2020 Earth Atmosphere Transmittance (256x64) & Multi-Scattering (32x32) LUTs
    constexpr std::uint32_t tw = 256u;
    constexpr std::uint32_t th = 64u;
    constexpr double r_bottom = 6360.0; // km
    constexpr double r_top = 6460.0; // km
    constexpr double beta_rayleigh[3] = {5.802e-3, 13.558e-3, 33.1e-3};
    constexpr double beta_mie_ext = 4.40e-3;
    constexpr double beta_ozone[3] = {0.650e-3, 1.881e-3, 0.085e-3};

    Array<std::uint16_t> trans_half(tw * th * 4u);
    for (std::uint32_t y = 0u; y < th; ++y) {
        const double v = (double(y) + 0.5) / double(th);
        const double h_km = v * v * (r_top - r_bottom);
        const double r = r_bottom + h_km;
        for (std::uint32_t x = 0u; x < tw; ++x) {
            const double u = (double(x) + 0.5) / double(tw);
            const double cos_z = 2.0 * u - 1.0;
            const double disc = r * r * (cos_z * cos_z - 1.0) + r_top * r_top;
            const double d_max = std::max(0.0, -r * cos_z + std::sqrt(std::max(0.0, disc)));
            double opt_r = 0.0;
            double opt_m = 0.0;
            double opt_o = 0.0;
            constexpr std::uint32_t steps = 32u;
            const double dt = d_max / double(steps);
            for (std::uint32_t s = 0u; s < steps; ++s) {
                const double t = (double(s) + 0.5) * dt;
                const double r_s = std::sqrt(r * r + t * t + 2.0 * r * t * cos_z);
                const double alt = std::max(0.0, r_s - r_bottom);
                opt_r += std::exp(-alt / 8.0) * dt;
                opt_m += std::exp(-alt / 1.2) * dt;
                opt_o += std::max(0.0, 1.0 - std::abs(alt - 25.0) / 15.0) * dt;
            }
            const std::size_t idx = (std::size_t(y) * tw + x) * 4u;
            for (std::size_t c = 0u; c < 3u; ++c) {
                const double tau = beta_rayleigh[c] * opt_r + beta_mie_ext * opt_m + beta_ozone[c] * opt_o;
                trans_half[idx + c] = fp32_to_fp16(static_cast<float>(std::exp(-tau)));
            }
            trans_half[idx + 3u] = fp32_to_fp16(1.0f);
        }
    }
    ATELIER_TRY_ASSIGN(atmos_transmittance, batch.stage_texture_2d(nri::Format::RGBA16_SFLOAT, tw, th,
                                                                   trans_half.data(), tw * 8u, "atmos_transmittance"));

    constexpr std::uint32_t mw = 32u;
    Array<std::uint16_t> ms_half(mw * mw * 4u);
    for (std::uint32_t y = 0u; y < mw; ++y) {
        const float alt_f = float(y) / float(mw - 1u);
        const float density = std::exp(-alt_f * 6.0f);
        for (std::uint32_t x = 0u; x < mw; ++x) {
            const float sun_cos = 2.0f * (float(x) / float(mw - 1u)) - 1.0f;
            const float sun_vis = std::clamp(sun_cos * 0.5f + 0.5f, 0.02f, 1.0f);
            const std::size_t idx = (std::size_t(y) * mw + x) * 4u;
            ms_half[idx + 0u] = fp32_to_fp16(0.08f * density * sun_vis);
            ms_half[idx + 1u] = fp32_to_fp16(0.11f * density * sun_vis);
            ms_half[idx + 2u] = fp32_to_fp16(0.16f * density * sun_vis);
            ms_half[idx + 3u] = fp32_to_fp16(1.0f);
        }
    }
    ATELIER_TRY_ASSIGN(atmos_multiscatter, batch.stage_texture_2d(nri::Format::RGBA16_SFLOAT, mw, mw, ms_half.data(),
                                                                  mw * 8u, "atmos_multiscatter"));

    // 4. Jiménez SMAA AreaTex (160x560 RG8_UNORM) & SearchTex (64x16 R8_UNORM)
    const auto area_bytes = smaa_lut::area_bytes();
    const auto search_bytes = smaa_lut::search_bytes();
    ATELIER_TRY_ASSIGN(smaa_area,
                       batch.stage_texture_2d(nri::Format::RG8_UNORM, smaa_lut::area_width, smaa_lut::area_height,
                                              area_bytes.data(), smaa_lut::area_pitch, "smaa_area"));
    ATELIER_TRY_ASSIGN(smaa_search,
                       batch.stage_texture_2d(nri::Format::R8_UNORM, smaa_lut::search_width, smaa_lut::search_height,
                                              search_bytes.data(), smaa_lut::search_pitch, "smaa_search"));

    ATELIER_TRY(batch.execute());
    initialized = true;
    ok = true;
    return {};
}

void LightingLutSet::retire(rhi::Device& device) noexcept {
    ltc_matrix.retire(device);
    ltc_amplitude.retire(device);
    charlie_lut.retire(device);
    atmos_transmittance.retire(device);
    atmos_multiscatter.retire(device);
    smaa_area.retire(device);
    smaa_search.retire(device);
    initialized = false;
}

} // namespace atelier::renderer::detail
