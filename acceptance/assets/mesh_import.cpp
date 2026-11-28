#include "../helpers.hpp"
#include "assets/atx_texture.hpp"
#include "assets/mesh_import.hpp"
#include "geometry/meshlet.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>

void verify_mesh_import() {
    using namespace atelier;
    using namespace acceptance::helpers;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto file_name = String::concat("atelier-import-", stamp, ".obj");

    const auto path = std::filesystem::temp_directory_path() / file_name.c_str();
    {
        std::ofstream output(path, std::ios::binary);
        output << "v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n";
        output.close();
        require(bool(output), "Cannot write OBJ fixture.");
    }
    const auto imported = take(assets::load_obj(path));
    std::filesystem::remove(path);
    const auto vertices = imported.mesh->vertices();
    require(vertices.size() == 3 && vertices[1].position[0] == 2 && vertices[2].position[1] == 3 &&
                imported.mesh->edge_mask(0) == 7,
            "Imported triangle differs from the authored OBJ coordinates.");

    // Open single-triangle mesh must NOT set the closed 2-manifold bit in face_meta.
    const auto open_meshlets = geometry::cook_meshlets(*imported.mesh);
    require(open_meshlets && open_meshlets->face_meta.size() == 1u &&
                (open_meshlets->face_meta[0] & geometry::face_meta_manifold_closed_bit) == 0u,
            "Open single-triangle mesh falsely set face_meta_manifold_closed_bit.");
}

void verify_meshlet_and_atx_pipeline() {
    using namespace atelier;
    using namespace acceptance::helpers;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto cache_root =
        std::filesystem::temp_directory_path() / String::concat("atelier-headless-cook-", stamp).c_str();
    const auto amsh_dir = cache_root / "amsh";
    const auto atx_dir = cache_root / "atx";
    std::error_code ec;
    std::filesystem::remove_all(cache_root, ec);

    // 1. Octahedral SNORM16 normal round-trip across all 8 octants:
    for (float sx : {-1.0f, 1.0f}) {
        for (float sy : {-1.0f, 1.0f}) {
            for (float sz : {-1.0f, 1.0f}) {
                const float inv_len = 1.0f / std::sqrt(1.0f + 4.0f + 9.0f);
                const float nx = sx * 1.0f * inv_len;
                const float ny = sy * 2.0f * inv_len;
                const float nz = sz * 3.0f * inv_len;
                const auto unpacked = geometry::unpack_oct_normal_snorm16(geometry::pack_oct_normal_snorm16(nx, ny, nz));
                const float dot = nx * unpacked[0] + ny * unpacked[1] + nz * unpacked[2];
                require(dot > 0.999f, "Octahedral SNORM16 normal round-trip lost direction accuracy.");
            }
        }
    }

    // 2. Closed 2-manifold multi-material UV sphere (>124 triangles -> discrete LOD chain + locked material seam):
    {
        constexpr std::uint32_t stacks = 24u;
        constexpr std::uint32_t slices = 32u;
        Array<geometry::Vertex> part0_verts;
        Array<geometry::Vertex> part1_verts;
        auto sphere_vertex = [](std::uint32_t st, std::uint32_t sl) noexcept -> geometry::Vertex {
            const float phi = std::numbers::pi_v<float> * float(st) / float(stacks);
            const std::uint32_t wrapped_sl = sl % slices;
            const float theta = 2.0f * std::numbers::pi_v<float> * float(wrapped_sl) / float(slices);
            const float sin_p = std::sin(phi);
            const float x = (st == 0u || st == stacks) ? 0.0f : (sin_p * std::cos(theta));
            const float y = (st == 0u) ? 1.0f : ((st == stacks) ? -1.0f : std::cos(phi));
            const float z = (st == 0u || st == stacks) ? 0.0f : (sin_p * std::sin(theta));
            return {
                .position = {x * 2.0f, y * 2.0f, z * 2.0f},
                .normal = {x, y, z},
                .uv = {float(sl) / float(slices), float(st) / float(stacks)},
            };
        };

        for (std::uint32_t st = 0u; st < stacks; ++st) {
            auto& dst = (st < stacks / 2u) ? part0_verts : part1_verts;
            for (std::uint32_t sl = 0u; sl < slices; ++sl) {
                const auto v00 = sphere_vertex(st, sl);
                const auto v10 = sphere_vertex(st + 1u, sl);
                const auto v11 = sphere_vertex(st + 1u, sl + 1u);
                const auto v01 = sphere_vertex(st, sl + 1u);
                if (st > 0u) {
                    dst.add(v00);
                    dst.add(v10);
                    dst.add(v01);
                }
                if (st + 1u < stacks) {
                    dst.add(v01);
                    dst.add(v10);
                    dst.add(v11);
                }
            }
        }

        const auto p0_count = static_cast<std::uint32_t>(part0_verts.size());
        const auto p1_count = static_cast<std::uint32_t>(part1_verts.size());
        Array<geometry::Vertex> all_verts;
        all_verts.reserve(part0_verts.size() + part1_verts.size());
        all_verts.append(part0_verts.view());
        all_verts.append(part1_verts.view());

        Array<geometry::MaterialSlot> slots{geometry::MaterialSlot{"top"}, geometry::MaterialSlot{"bottom"}};
        Array<geometry::MeshPart> parts{{0u, p0_count, 0u}, {p0_count, p1_count, 1u}};
        const auto sphere = take(geometry::Mesh::create(std::move(all_verts), {}, std::move(slots), std::move(parts)));

        const auto cooked = geometry::load_or_cook_meshlets(*sphere, amsh_dir);
        require(cooked != nullptr && cooked->lod_count > 1u,
                "Multi-material sphere (>124 tris) did not build discrete Meshlet LODs.");
        require(cooked->lods[0].part_count == 2u &&
                    cooked->lods[0].triangle_count == (p0_count + p1_count) / 3u &&
                    cooked->lods[0].triangle_count > cooked->lods[cooked->lod_count - 1u].triangle_count,
                "Meshlet LOD chain did not preserve LOD0 count or simplify coarser LODs.");
        require(cooked->blas_indices.size() == cooked->face_meta.size() * 3u,
                "BLAS index stream and FaceMeta stream are out of sync.");

        // Closed 2-manifold sphere must mark LOD0 triangles with face_meta_manifold_closed_bit.
        for (std::uint32_t t = 0u; t < cooked->lods[0].triangle_count; ++t) {
            require((cooked->face_meta[t] & geometry::face_meta_manifold_closed_bit) != 0u,
                    "Closed 2-manifold sphere missed face_meta_manifold_closed_bit.");
        }

        // Verify meshlet bounds, capacity limits, and 1:1 BLAS index correspondence:
        for (const auto& ml : cooked->meshlets) {
            const std::uint32_t v_cnt = ml.counts & 0xFFu;
            const std::uint32_t t_cnt = (ml.counts >> 8u) & 0xFFu;
            require(v_cnt > 0u && v_cnt <= geometry::meshlet_max_vertices && t_cnt > 0u &&
                        t_cnt <= geometry::meshlet_max_triangles,
                    "GpuMeshlet exceeded 64v/124t hardware invariants.");
            for (std::uint32_t vi = 0u; vi < v_cnt; ++vi) {
                const std::uint32_t global_v = cooked->meshlet_vertices[ml.vertex_offset + vi];
                require(global_v < cooked->positions.size(), "Meshlet vertex index out of bounds.");
                const auto& p = cooked->positions[global_v];
                const float dx = p.x - ml.center[0];
                const float dy = p.y - ml.center[1];
                const float dz = p.z - ml.center[2];
                require(std::sqrt(dx * dx + dy * dy + dz * dz) <= ml.radius + 1e-3f,
                        "Meshlet bounding sphere failed to enclose a member vertex.");
            }
            for (std::uint32_t ti = 0u; ti < t_cnt; ++ti) {
                const std::size_t tri_byte = ml.triangle_offset + std::size_t(ti) * 3u;
                const std::uint32_t i0 = cooked->meshlet_vertices[ml.vertex_offset + cooked->meshlet_triangles[tri_byte]];
                const std::uint32_t i1 =
                    cooked->meshlet_vertices[ml.vertex_offset + cooked->meshlet_triangles[tri_byte + 1u]];
                const std::uint32_t i2 =
                    cooked->meshlet_vertices[ml.vertex_offset + cooked->meshlet_triangles[tri_byte + 2u]];
                const std::size_t blas_base = std::size_t(ml.global_tri_base + ti) * 3u;
                require(cooked->blas_indices[blas_base] == i0 && cooked->blas_indices[blas_base + 1u] == i1 &&
                            cooked->blas_indices[blas_base + 2u] == i2,
                        "Meshlet local triangle indices diverged from BLAS global index stream.");
            }
        }

        // Verify .amsh cache hit + self-healing when cached .amsh is corrupted:
        const auto cached_hit = geometry::load_or_cook_meshlets(*sphere, amsh_dir);
        require(cached_hit && cached_hit->meshlets.size() == cooked->meshlets.size() &&
                    cached_hit->blas_indices == cooked->blas_indices,
                "Reloaded .amsh cache diverged from cooked Meshlets.");

        std::size_t amsh_files = 0u;
        for (const auto& entry : std::filesystem::directory_iterator(amsh_dir)) {
            if (entry.path().extension() == L".amsh") {
                ++amsh_files;
                std::ofstream bad(entry.path(), std::ios::binary | std::ios::trunc);
                bad.write("CORRUPT_AMSH", 12);
            }
        }
        require(amsh_files == 1u, "Expected exactly one .amsh cache entry.");
        const auto healed = geometry::load_or_cook_meshlets(*sphere, amsh_dir);
        require(healed && healed->meshlets.size() == cooked->meshlets.size(),
                "Corrupt .amsh cache did not self-heal by re-cooking.");
    }

    // 3. Image cold-pixel eviction/inflation + .atx v4 BC1/BC4/BC5(Toksvig)/BC6H/BC7 + container validation:
    {
        Array<std::uint8_t> rgba8(8u * 8u * 4u);
        for (std::uint32_t y = 0u; y < 8u; ++y) {
            for (std::uint32_t x = 0u; x < 8u; ++x) {
                const std::size_t off = (std::size_t(y) * 8u + x) * 4u;
                const bool checker = ((x ^ y) & 1u) != 0u;
                // Encode tilted tangent-space normals (+0.6/+0.6/+0.529 vs -0.6/-0.6/+0.529) for Toksvig variance:
                rgba8[off + 0u] = checker ? 204u : 51u;
                rgba8[off + 1u] = checker ? 204u : 51u;
                rgba8[off + 2u] = 191u;
                rgba8[off + 3u] = 255u;
            }
        }
        const Array<std::uint8_t> expected_rgba = rgba8;
        const auto img = take(material::Image::create(8u, 8u, std::move(rgba8)));
        require(img->has_resident_pixels(), "Fresh material::Image should start with resident pixels.");
        img->evict_cold_pixels();
        require(!img->has_resident_pixels(), "evict_cold_pixels() left uncompressed pixels resident.");
        const auto inflated = img->acquire_pixels();
        require(inflated && *inflated == expected_rgba,
                "acquire_pixels() after evict_cold_pixels() did not losslessly restore RGBA8 bytes.");

        const auto bc7 = assets::cook_atx_image(*img, assets::AtxRole::color_srgb);
        const auto bc1 = assets::cook_atx_image(*img, assets::AtxRole::color_opaque_bc1);
        const auto bc4 = assets::cook_atx_image(*img, assets::AtxRole::scalar_bc4);
        const auto bc5 = assets::cook_atx_image(*img, assets::AtxRole::normal_bc5);
        require(bc7->format == assets::AtxFormat::bc7_rgba_srgb && bc7->mips.size() == 4u &&
                    bc1->format == assets::AtxFormat::bc1_rgba_srgb &&
                    bc4->format == assets::AtxFormat::bc4_r_unorm &&
                    bc5->format == assets::AtxFormat::bc5_rg_unorm,
                "8x8 ATX cooking selected unexpected BCn format or mip count.");
        for (const auto& mip : bc7->mips) {
            require((mip.byte_offset & 511ull) == 0ull && (mip.row_pitch & 255u) == 0u,
                    "ATX mip slice violated D3D12 512B offset / 256B row pitch alignment.");
        }
        require(bc5->toksvig_mip_lengths.size() == 4u &&
                    bc5->toksvig_mip_lengths[1] < 0.9f &&
                    assets::compute_atx_toksvig_slope(*bc5) > 0.1f,
                "BC5 normal map cooking did not record positive Toksvig mip normal variance.");

        // Unaligned / <4x4 fallback:
        Array<std::uint8_t> tiny_px(3u * 3u * 4u, 180u);
        const auto tiny_img = take(material::Image::create(3u, 3u, std::move(tiny_px)));
        const auto tiny_atx = assets::cook_atx_image(*tiny_img, assets::AtxRole::color_srgb);
        require(tiny_atx->format == assets::AtxFormat::rgba8_srgb && !assets::is_block_compressed(tiny_atx->format),
                "Non-multiple-of-4 image did not fall back to uncompressed RGBA8_SRGB.");

        // Binary encode/decode round-trip + corrupt rejection:
        Array<std::uint8_t> encoded = assets::encode_atx(*bc5);
        const auto decoded = take(assets::decode_atx(encoded));
        require(decoded->format == bc5->format && decoded->role == bc5->role &&
                    decoded->payload == bc5->payload &&
                    decoded->toksvig_mip_lengths == bc5->toksvig_mip_lengths,
                "encode_atx -> decode_atx did not preserve .atx v4 container contents.");

        Array<std::uint8_t> truncated(encoded.view().slice(0u, encoded.size() - 16u));
        require(!assets::decode_atx(truncated), "decode_atx accepted a truncated .atx payload.");
        encoded[0] ^= 0xFFu;
        require(!assets::decode_atx(encoded), "decode_atx accepted a corrupt .atx magic header.");

        // HDR SkyMap BC6H_UF16 cooking + disk cache self-healing:
        Array<float> sky_rgb(8u * 4u * 3u, 0.25f);
        sky_rgb[0] = 6.0f;
        sky_rgb[1] = 4.0f;
        sky_rgb[2] = 2.0f;
        const auto sky_res = rendering::SkyMap::create(8u, 4u, sky_rgb);
        require(sky_res.has_value(), "SkyMap::create(8, 4) failed on valid HDR input.");
        const auto sky_atx = assets::load_or_cook_atx_sky_map(**sky_res, atx_dir);
        require(sky_atx && sky_atx->format == assets::AtxFormat::bc6h_uf16 &&
                    (*sky_res)->sh3()[0][0] > 0.0f,
                "8x4 HDR SkyMap did not cook to BC6H_UF16 with positive L0 irradiance.");

        const auto cached_img_atx = assets::load_or_cook_atx_image(*img, assets::AtxRole::normal_bc5, atx_dir);
        require(cached_img_atx && cached_img_atx->payload == bc5->payload,
                "load_or_cook_atx_image produced mismatched payload.");
        for (const auto& entry : std::filesystem::directory_iterator(atx_dir)) {
            if (entry.path().extension() == L".atx") {
                std::ofstream bad(entry.path(), std::ios::binary | std::ios::trunc);
                bad.write("BAD_ATX", 7);
            }
        }
        const auto healed_atx = assets::load_or_cook_atx_image(*img, assets::AtxRole::normal_bc5, atx_dir);
        require(healed_atx && healed_atx->payload == bc5->payload,
                "Corrupt .atx cache file did not self-heal on reload.");
    }

    std::filesystem::remove_all(cache_root, ec);
    std::puts("[pass] Headless Meshlet (.amsh) & ATX (.atx BC1/BC4/BC5/BC6H/BC7) cooking pipeline");
}
