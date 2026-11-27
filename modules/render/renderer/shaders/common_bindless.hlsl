#ifndef ATELIER_COMMON_BINDLESS_HLSL
#define ATELIER_COMMON_BINDLESS_HLSL 1

static const float ATELIER_PI = 3.14159265358979323846f;
static const float ATELIER_INV_PI = 0.31830988618379067154f;
static const uint VISBUFFER_EMPTY = 0u;

#ifndef ATELIER_SAFE_DESC_DEFINED
#define ATELIER_SAFE_DESC_DEFINED 1
uint atelier_safe_desc(uint idx) {
    return (idx < 262144u) ? idx : 0u;
}
#endif

struct GpuMeshDescriptor {
    uint positions_srv;
    uint attributes_srv;
    uint meshlets_srv;
    uint meshlet_vertices_srv;
    uint meshlet_triangles_srv;
    uint blas_indices_srv;
    uint face_meta_srv;
    uint lod_count;
    uint4 lod_first_meshlet;
    uint4 lod_meshlet_count;
    float4 lod_error;
    float3 bounds_center;
    float bounds_radius;
};

struct GpuPackedPosition {
    float3 pos;
};

struct GpuPackedAttributes {
    uint oct_normal;
    uint oct_tangent_sign;
    float2 uv;
};

struct GpuMeshlet {
    uint vertex_offset;
    uint triangle_offset;
    uint global_tri_base;
    uint counts; // bits 0..7: vertex_count, bits 8..15: triangle_count, bits 16..25: oct_cone_axis_s5, bits 26..31: cone_cutoff_u6
    float3 center;
    float radius;
};

struct GpuInstanceRecord {
    row_major float4x4 world;
    row_major float4x4 prev_world;
    float3 normal_row0;
    float tangent_sign;
    float3 normal_row1;
    uint mesh_pool_index;
    float3 normal_row2;
    uint material_slot_offset;
    uint part_table_base;
    uint flags;
    uint stable_key_lo;
    uint stable_key_hi;
};

struct GpuSurfaceRecord {
    uint archetype_index;
    uint parameter_base;
    uint input_base;
    uint flags; // bit 0: masked, bit 1: double_sided, bit 2: emission_two_sided
    uint masked_bin_index;
    uint masked_cov_index;
    uint2 pad;
};

struct GpuVisibleCluster {
    uint instance_id;
    uint meshlet_id;
};

struct GpuVsmLightView {
    row_major float4x4 view_proj;
    float3 light_pos_or_dir;
    float near_or_zmin;
    float3 right_axis;
    float far_or_zspan;
    float3 up_axis;
    float world_span_or_tan_half;
    int2 page_offset_xy;
    uint virtual_page_base;
    uint flags; // bit 0: valid, bit 1: is_ortho_clipmap, bits 8..15: grid_dim
};

struct GpuVsmPageJob {
    row_major float4x4 page_view_proj;
    uint phys_page;
    uint flags;
    uint2 pad;
};

struct GpuVsmVisibleCluster {
    uint page_job_idx;
    uint instance_id;
    uint meshlet_id;
    uint surface_idx;
};

struct GpuPenumbraItem {
    uint px_packed; // x | (y << 16)
    uint todo_lo;
    uint todo_hi;
    uint init_vis_lo;
    uint init_vis_hi;
    uint meta; // bits 0..7: area_slot (0xFF if none), bits 8..15: init_sun_u8, bits 16..23: init_area_u8, bit 24: is_foliage
};

struct GpuBaryDerivatives {
    float2 bary;   // perspective-correct (b1, b2), w0 = 1 - b1 - b2
    float2 db_dx;  // analytical d(b1, b2)/dx
    float2 db_dy;  // analytical d(b1, b2)/dy
    uint surface_and_flags; // bits 0..26: surface_index, bit 27: winding_flipped, bit 28: backfacing, bits 29..31: edge_mask
    uint global_tri_id;
};

struct GpuLightRecord {
    float3 position;
    uint kind_and_flags; // bits 0..1: kind (0: point, 1: rectangle, 2: directional, 3: spot), bit 2: two_sided, bit 3: casts_shadow
    float3 direction;
    float range;
    float3 color_intensity;
    float source_radius;
    float3 right;
    float width;
    float3 up;
    float height;
};

struct GlobalFrameConstants {
    row_major float4x4 view;
    row_major float4x4 inv_view;
    row_major float4x4 projection;
    row_major float4x4 inv_projection;
    row_major float4x4 view_projection;
    row_major float4x4 inv_view_projection;
    row_major float4x4 unjittered_view_projection;
    row_major float4x4 inv_unjittered_vp;
    row_major float4x4 prev_unjittered_vp;
    row_major float4x4 prev_view;
    row_major float4x4 prev_projection;

    float4 raster_viewport_transform;
    float4 scene_viewport;      // x, y, width, height (logical pixels)
    float4 prev_scene_viewport; // previous frame logical viewport
    int4 scissor_rect;          // left, top, right, bottom

    float3 camera_eye;
    float camera_far_depth;

    float3 environment_radiance;
    float sky_rotation_rad;

    uint2 render_extent;
    uint2 output_extent;

    float2 jitter_pixels;
    float exposure_scale;
    float indirect_gain;

    float ao_radius;
    float reflection_distance;
    float reflection_roughness;
    float exposure_ratio;

    float contact_shadow_length;
    float render_delta_seconds;
    uint frame_index;
    uint flags; // bit0: shadows, bit1: indirect_lighting, bit2: ssilvb, bit3: reflections, bit4: bloom, bit5: sky_map_valid, bit7: has_prev_history, bit8: tonemap_agx, bit9: backface_tint, bit10: triangle_edges, bit11: temporal_aa_active

    uint shading_mode;   // 0: smooth, 1: flat, 2: matcap, 3: scene_lit
    uint wireframe_mode; // 0: off, 1: overlay, 2: only
    float2 wire_style;   // x: width, y: opacity

    // Global Bindless SRVs:
    uint mesh_table_srv;
    uint part_tri_base_srv;
    uint instances_srv;
    uint selected_mask_srv;
    uint material_slot_map_srv;
    uint material_surfaces_srv;
    uint material_params_srv;
    uint material_inputs_srv;

    uint lights_srv;
    uint light_count;
    uint dir_light_count;
    uint primary_sun_and_gi_cands; // bits 0..15: primary_sun_idx (0xFFFF if none), bits 16..31: direct_candidates (1..32)

    uint tlas_srv;
    uint sky_map_srv;
    float sky_scale;
    uint matcap_srv;

    uint ltc_matrix_srv;
    uint ltc_amp_srv;
    uint charlie_lut_srv;
    uint atmos_trans_srv;

    uint atmos_multiscatter_srv;
    uint smaa_area_srv;
    uint smaa_search_srv;
    uint instance_count;

    uint archetype_count;
    uint masked_archetype_count;
    uint gi_rays_per_probe;
    uint wrc_voxel_keys_srv;

    uint max_opaque_clusters;
    uint masked_cluster_stride;
    uint max_selected_clusters;
    uint max_cull_chunks;

    float4 sky_sh3[9];

    float3 lighting_focus;
    uint hzb_srv;
    float3 prev_camera_eye;
    uint prev_view_z_srv;
    uint wrc_curr_srv;
    uint wrc_hash_keys_srv;
    uint wrc_hash_keys_uav;
    uint froxel_srv;
    float fog_density;
    float fog_height_falloff;
    float fog_anisotropy;
    uint env_sky_mode;

    uint vsm_static_srv;
    uint vsm_dynamic_srv;
    uint vsm_page_table_srv;
    uint vsm_views_srv;
};

// ============================================================================
// Octahedral Unit Vector Codec (SNORM16x2 -> 0.002 deg precision)
// ============================================================================
float2 oct_encode(float3 n) {
    float inv_l1 = 1.0f / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-12f);
    float2 p = n.xy * inv_l1;
    if (n.z < 0.0f) {
        p = (1.0f - abs(p.yx)) * float2(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
    }
    return clamp(p, -1.0f, 1.0f);
}

float3 oct_decode(float2 e) {
    float3 v = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    if (v.z < 0.0f) {
        v.xy = (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
    }
    return normalize(v);
}

uint pack_oct_snorm16(float3 n) {
    float2 e = oct_encode(n);
    int2 q = int2(round(e * 32767.0f));
    return (uint(q.x) & 0xFFFFu) | ((uint(q.y) & 0xFFFFu) << 16u);
}

float3 unpack_oct_snorm16(uint packed) {
    int qx = (int(packed << 16u)) >> 16;
    int qy = int(packed) >> 16;
    return oct_decode(clamp(float2(qx, qy) * (1.0f / 32767.0f), -1.0f, 1.0f));
}

float4 unpack_oct_tangent_sign(uint packed) {
    int qx = (int(packed << 16u)) >> 16;
    int raw_y = int((packed >> 16u) & 0x7FFFu);
    if ((raw_y & 0x4000) != 0) raw_y |= ~0x7FFF;
    float sign_w = ((packed >> 31u) != 0u) ? -1.0f : 1.0f;
    float2 e = clamp(float2(float(qx) / 32767.0f, float(raw_y) / 16383.0f), -1.0f, 1.0f);
    return float4(oct_decode(e), sign_w);
}

bool atelier_finite_f(float x) {
    return (asuint(x) & 0x7FFFFFFFu) < 0x7F800000u;
}

bool atelier_finite3(float3 v) {
    return all((asuint(v) & 0x7FFFFFFFu) < 0x7F800000u);
}

float3 safe_unit_normal(float3 v) {
    if (!atelier_finite3(v))
        return float3(0.0f, 0.0f, 1.0f);
    float m = max(abs(v.x), max(abs(v.y), abs(v.z)));
    return m > 0.0f ? normalize(v / m) : float3(0.0f, 0.0f, 1.0f);
}

float3 transform_local_normal(float3 local_n, GpuInstanceRecord inst) {
    return safe_unit_normal(local_n.x * inst.normal_row0 + local_n.y * inst.normal_row1 + local_n.z * inst.normal_row2);
}

float4 transform_local_tangent(float4 local_t, GpuInstanceRecord inst) {
    float3x3 basis = (float3x3)inst.world;
    float3 largest = max(abs(basis[0]), max(abs(basis[1]), abs(basis[2])));
    float scale = max(largest.x, max(largest.y, largest.z));
    float3 tw = scale > 0.0f ? mul(local_t.xyz, basis / scale) : 0.0f;
    return float4(tw, local_t.w * inst.tangent_sign);
}

float3 reconstruct_geom_normal(float3 p0, float3 p1, float3 p2, float3 fallback_vn0, bool winding_flipped, GpuInstanceRecord inst) {
    float3 e1 = p1 - p0;
    float3 e2 = p2 - p0;
    float3 m = max(abs(e1), abs(e2));
    float escale = max(m.x, max(m.y, m.z));
    float3 lgn = escale > 0.0f ? cross(e1 / escale, e2 / escale) : 0.0f;
    if (winding_flipped) lgn = -lgn;
    if (dot(lgn, lgn) <= 0.0f) lgn = fallback_vn0;
    return transform_local_normal(lgn, inst);
}

float exact_view_z_from_depth(uint2 px, float dz, GlobalFrameConstants fcb) {
    float2 pxc = float2(px) + 0.5f;
    float2 ndc = float2((pxc.x - fcb.scene_viewport.x) / max(fcb.scene_viewport.z, 1.0f) * 2.0f - 1.0f,
                        1.0f - (pxc.y - fcb.scene_viewport.y) / max(fcb.scene_viewport.w, 1.0f) * 2.0f);
    precise float4 vh = mul(float4(ndc, dz, 1.0f), fcb.inv_projection);
    float vw = abs(vh.w) > 1e-7f ? vh.w : (vh.w >= 0.0f ? 1e-7f : -1e-7f);
    return vh.z / vw;
}

uint depth_to_z_bin(float view_z) {
    float norm = saturate(log2(max(view_z, 0.1f) * 10.0f) * (1.0f / 13.28771238f));
    return min(uint(norm * 32.0f), 31u);
}

uint float_to_sortable_uint(float f) {
    uint u = asuint(f);
    return ((u & 0x80000000u) != 0u) ? (~u) : (u ^ 0x80000000u);
}

float sortable_uint_to_float(uint k) {
    return asfloat(((k & 0x80000000u) != 0u) ? (k ^ 0x80000000u) : (~k));
}

float3 clip_quad_gn_centroid(float3 wpos, float3 gn, float3 c0, float3 c1, float3 c2, float3 c3) {
    float3 P[4] = {c0, c1, c2, c3};
    float3 sum = 0.0f;
    float cnt = 0.0f;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        float3 a = P[i];
        float3 b = P[(i + 1u) & 3u];
        float ga = dot(a - wpos, gn);
        float gb = dot(b - wpos, gn);
        bool a_in = (ga >= 0.0f);
        bool b_in = (gb >= 0.0f);
        if (a_in) {
            sum += a;
            cnt += 1.0f;
        }
        if (a_in != b_in) {
            float t = a_in ? (ga / max(ga - gb, 1e-7f)) : ((-ga) / max(gb - ga, 1e-7f));
            sum += lerp(a, b, saturate(t));
            cnt += 1.0f;
        }
    }
    return (cnt > 0.0f) ? (sum / cnt) : c0;
}

// Authoritative geometric light-horizon predicate shared by VSM interval classification,
// analytic shadow-ray preparation, and penumbra bilateral/temporal filtering:
bool is_light_front_horizon(GpuLightRecord l, float3 wpos, float3 gn) {
    uint kind = l.kind_and_flags & 3u;
    bool two_sided = (l.kind_and_flags & 4u) != 0u;
    if (kind == 2u) {
        float3 L_center = safe_unit_normal(-l.direction);
        float alpha = (l.source_radius > 1e-4f) ? min(l.source_radius, 0.45f) : 0.0f;
        return dot(gn, L_center) > -0.1f - sin(alpha);
    }
    if (kind == 1u) {
        if (!two_sided && dot(wpos - l.position, l.direction) <= 0.0f)
            return false;
        float3 hx = l.right * (0.5f * l.width);
        float3 hy = l.up * (0.5f * l.height);
        float g0 = dot(l.position - hx - hy - wpos, gn);
        float g1 = dot(l.position + hx - hy - wpos, gn);
        float g2 = dot(l.position + hx + hy - wpos, gn);
        float g3 = dot(l.position - hx + hy - wpos, gn);
        return max(max(g0, g1), max(g2, g3)) > 0.0f;
    }
    float3 to_center = l.position - wpos;
    float d_center = length(to_center);
    if (d_center <= 1e-4f)
        return true;
    float3 L_center = to_center / d_center;
    if (kind == 3u && dot(-L_center, l.direction) <= l.width)
        return false;
    float sin_s = (l.source_radius > 1e-4f) ? min(l.source_radius / d_center, 1.0f) : 0.0f;
    return dot(gn, L_center) > -0.1f - sin_s;
}

// Shared analytic shadow-ray preparation for both 64-bit clustered ShadowRayQuery CS and DirectLight overflow fallback:
// Returns 1 = unoccluded/culled (skip trace), 0 = occluded (skip trace), -1 = trace [out_origin, out_dir, out_t_max].
int build_analytic_shadow_ray(GpuLightRecord l, float3 wpos, float3 gn, bool is_foliage, bool jitter_penumbra,
                              float ign, float ign2, out float3 out_origin, out float3 out_dir, out float out_t_max) {
    out_origin = wpos;
    out_dir = float3(0.0f, 0.0f, 1.0f);
    out_t_max = 0.0f;

    uint kind = l.kind_and_flags & 3u;
    bool two_sided = (l.kind_and_flags & 4u) != 0u;

    if (kind == 2u) {
        float3 L_center = safe_unit_normal(-l.direction);
        float alpha = (l.source_radius > 1e-4f) ? min(l.source_radius, 0.45f) : 0.0f;
        float sin_r = sin(alpha);
        float h_c = dot(gn, L_center);
        if (!is_foliage && h_c <= -0.1f - sin_r)
            return 0;
        float3 L = L_center;
        if (alpha > 0.0f && jitter_penumbra) {
            float3 up_ref = abs(L.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            float3 t1 = normalize(cross(up_ref, L));
            float3 t2 = cross(L, t1);
            float r_disk = tan(min(alpha, 0.2f)) * sqrt(ign);
            float phi = 2.0f * ATELIER_PI * ign2;
            L = normalize(L + (t1 * cos(phi) + t2 * sin(phi)) * r_disk);
        }
        if (!is_foliage) {
            if (alpha > 0.0f && !jitter_penumbra && h_c < 0.5f * sin_r) {
                float target_h = max(2e-3f, 0.5f * (h_c + sin_r));
                L = safe_unit_normal(L_center + gn * (target_h - h_c));
            } else if (dot(gn, L) < 2e-3f) {
                float target_h = max(2e-3f, 0.5f * (h_c + sin_r));
                float3 L_cap = safe_unit_normal(L_center + gn * (target_h - h_c));
                float g_c = dot(gn, L_cap);
                float g_t = dot(gn, L);
                L = safe_unit_normal(lerp(L_cap, L, saturate((g_c - 2e-3f) / max(g_c - g_t, 1e-5f)) * 0.85f));
            }
        }
        float3 offset_n = is_foliage ? (dot(gn, L) >= 0.0f ? gn : -gn) : gn;
        out_origin = wpos + offset_n * 3e-3f;
        out_dir = L;
        out_t_max = 1e4f;
        return -1;
    }

    float3 to_center = l.position - wpos;
    float d_center = length(to_center);
    float3 target_pt = l.position;
    float normal_bias = clamp(d_center * 5e-4f, 5e-4f, 4e-3f);

    if (kind == 1u) {
        float max_r = l.range + 0.5f * (l.width + l.height);
        if (d_center >= max_r || d_center <= 1e-4f)
            return 1;
        if (!two_sided && dot(wpos - l.position, l.direction) <= 0.0f)
            return 0;

        float3 hx = l.right * (0.5f * l.width);
        float3 hy = l.up * (0.5f * l.height);
        float3 c0 = l.position - hx - hy;
        float3 c1 = l.position + hx - hy;
        float3 c2 = l.position + hx + hy;
        float3 c3 = l.position - hx + hy;
        float3 vis_center = l.position;
        bool straddles_gn = false;
        if (!is_foliage) {
            float g0 = dot(c0 - wpos, gn);
            float g1 = dot(c1 - wpos, gn);
            float g2 = dot(c2 - wpos, gn);
            float g3 = dot(c3 - wpos, gn);
            if (max(max(g0, g1), max(g2, g3)) <= 0.0f)
                return 0;
            if (min(min(g0, g1), min(g2, g3)) <= 0.0f) {
                straddles_gn = true;
                vis_center = clip_quad_gn_centroid(wpos, gn, c0, c1, c2, c3);
            }
        }
        if (jitter_penumbra) {
            target_pt = l.position + l.right * ((ign - 0.5f) * l.width) + l.up * ((ign2 - 0.5f) * l.height);
            if (straddles_gn) {
                float g_t = dot(target_pt - wpos, gn);
                if (g_t <= 2e-3f) {
                    float g_c = dot(vis_center - wpos, gn);
                    target_pt = lerp(vis_center, target_pt, saturate((g_c - 2e-3f) / max(g_c - g_t, 1e-5f)) * 0.85f);
                }
            }
        } else {
            target_pt = vis_center;
        }
    } else {
        if (d_center >= l.range + l.source_radius || d_center <= 1e-4f)
            return 1;
        float3 L_center = to_center / d_center;
        if (kind == 3u && dot(-L_center, l.direction) <= l.width)
            return 0;
        float sin_s = (l.source_radius > 1e-4f) ? min(l.source_radius / d_center, 1.0f) : 0.0f;
        if (!is_foliage && dot(gn, L_center) <= -0.1f - sin_s)
            return 0;
        float h_c = dot(to_center, gn);
        float3 cap_mid = (!is_foliage && l.source_radius > 1e-4f)
                             ? (l.position + gn * max(0.0f, 0.5f * (h_c + l.source_radius) - h_c))
                             : l.position;
        if (l.source_radius > 1e-4f && jitter_penumbra) {
            float3 up_ref = abs(L_center.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            float3 t1 = normalize(cross(up_ref, L_center));
            float3 t2 = cross(L_center, t1);
            float r_s = min(l.source_radius, d_center * 0.5f) * sqrt(ign);
            float phi = 2.0f * ATELIER_PI * ign2;
            target_pt = l.position + (t1 * cos(phi) + t2 * sin(phi)) * r_s;
            if (!is_foliage) {
                float g_t = dot(target_pt - wpos, gn);
                if (g_t <= 2e-3f) {
                    float g_c = dot(cap_mid - wpos, gn);
                    target_pt = lerp(cap_mid, target_pt, saturate((g_c - 2e-3f) / max(g_c - g_t, 1e-5f)) * 0.85f);
                }
            }
        } else {
            target_pt = cap_mid;
        }
    }

    // Non-foliage surfaces ALWAYS bias outward along +gn (never -gn into closed wall/pillar interior!):
    float3 offset_n = is_foliage ? (dot(gn, target_pt - wpos) >= 0.0f ? gn : -gn) : gn;
    float3 biased_origin = wpos + offset_n * normal_bias;
    float3 biased_to_l = target_pt - biased_origin;
    if (!is_foliage) {
        float h_gn = dot(biased_to_l, gn);
        if (h_gn < 1e-3f)
            biased_to_l += gn * (1e-3f - h_gn);
    }
    float biased_dist = length(biased_to_l);
    if (biased_dist <= 2e-3f)
        return 1;

    float end_trim = 2e-3f;
    if ((kind == 0u || kind == 3u) && l.source_radius > 0.0f && d_center > 1.5f * l.source_radius) {
        float3 disk_off = target_pt - l.position;
        float chord = sqrt(max(0.0f, l.source_radius * l.source_radius - dot(disk_off, disk_off)));
        end_trim = min(biased_dist * 0.45f, max(2e-3f, chord * 0.90f));
    }
    if (biased_dist <= end_trim + 1e-4f)
        return 1;

    out_origin = biased_origin;
    out_dir = biased_to_l / biased_dist;
    out_t_max = biased_dist - end_trim;
    return -1;
}

static const uint VSM_PAGE_UNMAPPED  = 0xFFFFFFFFu;
static const uint VSM_PAGE_EMPTY_LIT = 0xFFFFFFFEu;

uint find_tile_primary_area_slot(uint c_base, uint c_count, uint local_bit_cap, StructuredBuffer<uint> cluster_buf,
                                 StructuredBuffer<GpuLightRecord> lights) {
    uint lim = min(c_count, local_bit_cap);
    [loop] for (uint it = 0u; it < lim; ++it) {
        uint li = cluster_buf[c_base + 2u + it];
        GpuLightRecord l = lights[li];
        if ((l.kind_and_flags & 8u) == 0u)
            continue;
        uint kind = l.kind_and_flags & 3u;
        if (kind == 1u || l.source_radius > 1e-3f)
            return it;
    }
    return 0xFFu;
}

float load_vsm_view_texel(GpuVsmLightView v, int tx, int ty, GlobalFrameConstants fcb) {
    StructuredBuffer<uint> page_table = ResourceDescriptorHeap[atelier_safe_desc(fcb.vsm_page_table_srv)];
    uint entry = VSM_PAGE_UNMAPPED;
    uint in_x = 0u;
    uint in_y = 0u;
    if ((v.flags & 2u) != 0u) {
        tx = clamp(tx, 0, 2047);
        ty = clamp(ty, 0, 2047);
        int world_ix = v.page_offset_xy.x + (tx >> 7);
        int world_iy = v.page_offset_xy.y + (15 - (ty >> 7));
        uint vx = uint(world_ix) & 15u;
        uint vy = uint(world_iy) & 15u;
        in_x = uint(tx) & 127u;
        in_y = uint(ty) & 127u;
        entry = page_table[v.virtual_page_base + vy * 16u + vx];
    } else {
        tx = clamp(tx, 0, 255);
        ty = clamp(ty, 0, 255);
        uint vx = uint(tx) >> 7u;
        uint vy = uint(ty) >> 7u;
        in_x = uint(tx) & 127u;
        in_y = uint(ty) & 127u;
        entry = page_table[v.virtual_page_base + vy * 2u + vx];
    }

    if (entry == VSM_PAGE_EMPTY_LIT)
        return 0.0f;
    if (entry >= 2048u)
        return -1.0f;
    if (entry < 1024u) {
        Texture2D<float> st = ResourceDescriptorHeap[atelier_safe_desc(fcb.vsm_static_srv)];
        return st.Load(int3(int(((entry & 31u) << 7u) | in_x), int(((entry >> 5u) << 7u) | in_y), 0));
    }
    uint dp = entry - 1024u;
    Texture2D<float> dt = ResourceDescriptorHeap[atelier_safe_desc(fcb.vsm_dynamic_srv)];
    return dt.Load(int3(int(((dp & 31u) << 7u) | in_x), int(((dp >> 5u) << 7u) | in_y), 0));
}

float sample_vsm_sun_visibility(GpuLightRecord l, float3 wpos, float bias_m, GlobalFrameConstants fcb) {
    uint vsm_base = (l.kind_and_flags >> 8u) & 0xFFu;
    if (vsm_base != 0u || fcb.vsm_page_table_srv == 0xFFFFFFFFu || fcb.vsm_views_srv == 0xFFFFFFFFu ||
        fcb.vsm_static_srv == 0xFFFFFFFFu || fcb.vsm_dynamic_srv == 0xFFFFFFFFu) {
        return -1.0f;
    }
    StructuredBuffer<GpuVsmLightView> views = ResourceDescriptorHeap[atelier_safe_desc(fcb.vsm_views_srv)];
    [loop] for (uint c = 0u; c < 4u; ++c) {
        GpuVsmLightView cv = views[c];
        if ((cv.flags & 1u) == 0u)
            continue;
        float span = cv.world_span_or_tan_half;
        float page_sz = span * (1.0f / 16.0f);
        float x_min = float(cv.page_offset_xy.x) * page_sz;
        float y_max = float(cv.page_offset_xy.y + 16) * page_sz;
        float x_p = dot(wpos, cv.right_axis);
        float y_p = dot(wpos, cv.up_axis);
        float z_p = dot(wpos, cv.light_pos_or_dir);
        float u_n = (x_p - x_min) / span;
        float v_n = (y_max - y_p) / span;
        if (u_n < 0.01f || u_n > 0.99f || v_n < 0.01f || v_n > 0.99f ||
            z_p < cv.near_or_zmin || z_p > cv.near_or_zmin + cv.far_or_zspan)
            continue;

        float texel_sz = span * (1.0f / 2048.0f);
        float z_test = z_p - max(bias_m, 1.5f * texel_sz);
        float2 tc = float2(u_n * 2048.0f - 0.5f, v_n * 2048.0f - 0.5f);
        int2 b0 = int2(floor(tc));
        float2 fr = tc - float2(b0);
        float vis_sum = 0.0f;
        [unroll] for (int dy = 0; dy <= 1; ++dy) {
            [unroll] for (int dx = 0; dx <= 1; ++dx) {
                float dz = load_vsm_view_texel(cv, b0.x + dx, b0.y + dy, fcb);
                if (dz < 0.0f)
                    return -1.0f;
                float z_occ = (dz <= 0.0f) ? 1e30f : (cv.near_or_zmin + (1.0f - dz) * cv.far_or_zspan);
                float w = ((dx == 0) ? (1.0f - fr.x) : fr.x) * ((dy == 0) ? (1.0f - fr.y) : fr.y);
                vis_sum += w * ((z_occ >= z_test) ? 1.0f : 0.0f);
            }
        }
        return vis_sum;
    }
    return -1.0f;
}

// Conservative Plane-Predicted 9-Tap VSM Interval Classifier:
//   1 = Guaranteed LIT (0 RayQuery), 0 = Guaranteed DEEP UMBRA (0 RayQuery),
//   2 = Far-Field VSM PCF (`out_far_pcf` in [0..1], 0 RayQuery), -1 = Indeterminate/Penumbra -> `PenumbraQueue`!
int evaluate_vsm_light_interval(GpuLightRecord l, float3 wpos, float3 gn, bool is_foliage, float cam_dist,
                                GlobalFrameConstants fcb, out float out_far_pcf) {
    out_far_pcf = 1.0f;
    uint kind = l.kind_and_flags & 3u;
    bool two_sided = (l.kind_and_flags & 4u) != 0u;
    float recv_rel = min(cam_dist, length(wpos - fcb.lighting_focus));
    bool far_tlas_handoff = (fcb.instance_count > 256u) && (recv_rel >= 76.0f);

    if (kind == 2u) {
        float3 L_center = safe_unit_normal(-l.direction);
        float alpha = (l.source_radius > 1e-4f) ? min(l.source_radius, 0.45f) : 0.0f;
        float sin_r = sin(alpha);
        if (!is_foliage && dot(gn, L_center) <= -0.1f - sin_r)
            return 0;

        uint vsm_base = (l.kind_and_flags >> 8u) & 0xFFu;
        if (vsm_base != 0u || fcb.vsm_page_table_srv == 0xFFFFFFFFu || fcb.vsm_views_srv == 0xFFFFFFFFu ||
            fcb.vsm_static_srv == 0xFFFFFFFFu || fcb.vsm_dynamic_srv == 0xFFFFFFFFu) {
            return (fcb.tlas_srv != 0xFFFFFFFFu && !far_tlas_handoff) ? -1 : 1;
        }

        StructuredBuffer<GpuVsmLightView> views = ResourceDescriptorHeap[atelier_safe_desc(fcb.vsm_views_srv)];
        [loop] for (uint c = 0u; c < 4u; ++c) {
            GpuVsmLightView cv = views[c];
            if ((cv.flags & 1u) == 0u)
                continue;
            float span = cv.world_span_or_tan_half;
            float page_sz = span * (1.0f / 16.0f);
            float x_min = float(cv.page_offset_xy.x) * page_sz;
            float y_max = float(cv.page_offset_xy.y + 16) * page_sz;
            float3 q_pos = wpos;
            float texel_sz = span * (1.0f / 2048.0f);
            if (is_foliage && dot(gn, L_center) < 0.0f)
                q_pos -= gn * max(0.01f, 1.5f * texel_sz);

            float x_p = dot(q_pos, cv.right_axis);
            float y_p = dot(q_pos, cv.up_axis);
            float z_p = dot(q_pos, cv.light_pos_or_dir);
            float u_n = (x_p - x_min) / span;
            float v_n = (y_max - y_p) / span;
            if (u_n < 0.015f || u_n > 0.985f || v_n < 0.015f || v_n > 0.985f ||
                z_p < cv.near_or_zmin || z_p > cv.near_or_zmin + cv.far_or_zspan)
                continue;

            float2 f_px = float2(u_n * 2048.0f, v_n * 2048.0f);
            float3 gn_l = float3(dot(gn, cv.right_axis), dot(gn, cv.up_axis), dot(gn, cv.light_pos_or_dir));
            float safe_gz = (abs(gn_l.z) >= 0.12f) ? gn_l.z : (gn_l.z < 0.0f ? -0.12f : 0.12f);
            float bias_far = max(0.015f, 1.75f * texel_sz);

            if (far_tlas_handoff) {
                float pcf_sum = 0.0f;
                bool unmapped_far = false;
                [unroll] for (int dy = -1; dy <= 1; ++dy) {
                    [unroll] for (int dx = -1; dx <= 1; ++dx) {
                        int tx = int(floor(f_px.x)) + dx;
                        int ty = int(floor(f_px.y)) + dy;
                        float dz = load_vsm_view_texel(cv, tx, ty, fcb);
                        if (dz < 0.0f) {
                            unmapped_far = true;
                        } else if (dz <= 0.0f) {
                            pcf_sum += 1.0f;
                        } else {
                            float z_occ = cv.near_or_zmin + (1.0f - dz) * cv.far_or_zspan;
                            float x_t = x_min + (float(tx) + 0.5f) * texel_sz;
                            float y_t = y_max - (float(ty) + 0.5f) * texel_sz;
                            float dz_pl = clamp(-((x_t - x_p) * gn_l.x + (y_t - y_p) * gn_l.y) / safe_gz,
                                                -4.0f * texel_sz, 4.0f * texel_sz);
                            pcf_sum += (z_occ >= z_p + dz_pl - bias_far) ? 1.0f : 0.0f;
                        }
                    }
                }
                if (unmapped_far)
                    continue;
                out_far_pcf = pcf_sum * (1.0f / 9.0f);
                return 2;
            }

            float tan_a = tan(min(alpha, 0.25f));
            float tol_lit = max(3e-3f, 0.20f * texel_sz);
            int tx0 = int(floor(f_px.x));
            int ty0 = int(floor(f_px.y));
            float dz0 = load_vsm_view_texel(cv, tx0, ty0, fcb);
            if (dz0 < 0.0f)
                continue;

            float max_delta = -1e30f;
            float min_delta = 1e30f;
            float pcf_sum = 0.0f;
            float step_pen = clamp(1.25f + (10.0f * tan_a) / max(texel_sz, 1e-4f), 1.25f, 20.0f);
            float delta0 = 0.0f;
            if (dz0 <= 0.0f) {
                min_delta = -1e30f;
                pcf_sum = 1.0f;
            } else {
                float z_occ0 = cv.near_or_zmin + (1.0f - dz0) * cv.far_or_zspan;
                float x_t0 = x_min + (float(tx0) + 0.5f) * texel_sz;
                float y_t0 = y_max - (float(ty0) + 0.5f) * texel_sz;
                float dz_pl0 = clamp(-((x_t0 - x_p) * gn_l.x + (y_t0 - y_p) * gn_l.y) / safe_gz,
                                     -4.0f * texel_sz, 4.0f * texel_sz);
                delta0 = (z_p + dz_pl0) - z_occ0;
                max_delta = delta0;
                min_delta = delta0;
                pcf_sum = (delta0 <= bias_far) ? 1.0f : 0.0f;
                if (delta0 > tol_lit)
                    step_pen = clamp(ceil((delta0 * tan_a * 1.15f) / max(texel_sz, 1e-4f)), 1.5f, 40.0f);
            }

            if (f_px.x < step_pen + 1.0f || f_px.x >= 2047.0f - step_pen ||
                f_px.y < step_pen + 1.0f || f_px.y >= 2047.0f - step_pen)
                continue;

            float diag_pen = step_pen * 0.75f;
            const float2 offsets[12] = {
                float2(-1.25f, 0.0f), float2(1.25f, 0.0f), float2(0.0f, -1.25f), float2(0.0f, 1.25f),
                float2(-step_pen, 0.0f), float2(step_pen, 0.0f), float2(0.0f, -step_pen), float2(0.0f, step_pen),
                float2(-diag_pen, -diag_pen), float2(diag_pen, -diag_pen),
                float2(-diag_pen, diag_pen), float2(diag_pen, diag_pen)
            };

            bool unmapped_ring = false;
            [unroll] for (uint k = 0u; k < 12u; ++k) {
                int tx = int(floor(f_px.x + offsets[k].x));
                int ty = int(floor(f_px.y + offsets[k].y));
                float dz = load_vsm_view_texel(cv, tx, ty, fcb);
                if (dz < 0.0f) {
                    unmapped_ring = true;
                    break;
                }
                if (dz <= 0.0f) {
                    min_delta = -1e30f;
                    if (k < 8u)
                        pcf_sum += 1.0f;
                } else {
                    float z_occ = cv.near_or_zmin + (1.0f - dz) * cv.far_or_zspan;
                    float x_t = x_min + (float(tx) + 0.5f) * texel_sz;
                    float y_t = y_max - (float(ty) + 0.5f) * texel_sz;
                    float dz_pl = clamp(-((x_t - x_p) * gn_l.x + (y_t - y_p) * gn_l.y) / safe_gz,
                                        -4.0f * texel_sz, 4.0f * texel_sz);
                    float delta = (z_p + dz_pl) - z_occ;
                    max_delta = max(max_delta, delta);
                    min_delta = min(min_delta, delta);
                    if (k < 8u)
                        pcf_sum += (delta <= bias_far) ? 1.0f : 0.0f;
                }
            }
            if (unmapped_ring)
                continue;

            float foliage_lit_tol = -max(0.025f, 5.75f * texel_sz);
            if (max_delta <= (is_foliage ? foliage_lit_tol : tol_lit))
                return 1;

            float pen_depth = max(0.0f, min_delta) * tan_a * 1.15f;
            float center_cone = max(0.0f, delta0) * tan_a * 1.15f;
            float tol_umbra = is_foliage ? max(0.15f, 3.0f * texel_sz + 0.25f * pen_depth)
                                         : max(0.025f, 1.75f * texel_sz + 0.25f * pen_depth);
            if (min_delta >= tol_umbra && step_pen * texel_sz >= center_cone)
                return 0;

            if (fcb.instance_count > 256u && max_delta > 0.0f) {
                float3 occ_pos = q_pos + L_center * max_delta;
                float occ_rel = min(length(occ_pos - fcb.camera_eye), length(occ_pos - fcb.lighting_focus));
                if (occ_rel >= 74.0f) {
                    out_far_pcf = pcf_sum * (1.0f / 9.0f);
                    return 2;
                }
            }

            return -1;
        }
        return (fcb.tlas_srv != 0xFFFFFFFFu && !far_tlas_handoff) ? -1 : 1;
    }

    // Local Point/Sphere (`kind == 0`), Rectangle (`kind == 1`), or Spot (`kind == 3`) Shadow Light:
    float3 to_center = l.position - wpos;
    float d_center = length(to_center);
    float src_ext = (kind == 1u) ? (0.5f * max(l.width, l.height)) : l.source_radius;
    float max_r = l.range + ((kind == 1u) ? (0.5f * (l.width + l.height)) : l.source_radius);
    if (d_center >= max_r || d_center <= 1e-4f)
        return 1;
    if (kind == 1u) {
        if (!two_sided && dot(wpos - l.position, l.direction) <= 0.0f)
            return 0;
    } else if (kind == 3u && dot(-to_center / d_center, l.direction) <= l.width) {
        return 0;
    }
    if (!is_foliage && !is_light_front_horizon(l, wpos, gn))
        return 0;

    uint vsm_base = (l.kind_and_flags >> 8u) & 0xFFu;
    if (vsm_base >= 64u || fcb.vsm_page_table_srv == 0xFFFFFFFFu || fcb.vsm_views_srv == 0xFFFFFFFFu ||
        fcb.vsm_static_srv == 0xFFFFFFFFu || fcb.vsm_dynamic_srv == 0xFFFFFFFFu) {
        return (fcb.tlas_srv != 0xFFFFFFFFu && !far_tlas_handoff) ? -1 : 1;
    }

    float3 rel = wpos - l.position;
    uint view_idx = vsm_base;
    if (kind == 0u) {
        float3 ar = abs(rel);
        if (ar.x >= ar.y && ar.x >= ar.z)
            view_idx = vsm_base + ((rel.x >= 0.0f) ? 0u : 1u);
        else if (ar.y >= ar.x && ar.y >= ar.z)
            view_idx = vsm_base + ((rel.y >= 0.0f) ? 2u : 3u);
        else
            view_idx = vsm_base + ((rel.z >= 0.0f) ? 4u : 5u);
    }

    StructuredBuffer<GpuVsmLightView> views = ResourceDescriptorHeap[atelier_safe_desc(fcb.vsm_views_srv)];
    GpuVsmLightView lv = views[view_idx];
    if ((lv.flags & 1u) == 0u)
        return far_tlas_handoff ? 1 : -1;

    float3 rv = lv.right_axis;
    float3 uv = lv.up_axis;
    float3 fv = cross(rv, uv);
    float x_v = dot(rel, rv);
    float y_v = dot(rel, uv);
    float z_v = dot(rel, fv);
    float near_z = lv.near_or_zmin;
    float far_z = lv.far_or_zspan;
    if (z_v <= near_z || z_v >= far_z)
        return far_tlas_handoff ? 1 : -1;

    float th = lv.world_span_or_tan_half;
    float2 ndc = float2(x_v / (z_v * th), y_v / (z_v * th));
    float texel_sz = (2.0f * z_v * th) * (1.0f / 256.0f);
    float2 f_px = float2((ndc.x * 0.5f + 0.5f) * 256.0f, (0.5f - ndc.y * 0.5f) * 256.0f);
    float3 p_v = float3(x_v, y_v, z_v);
    float3 gn_v = float3(dot(gn, rv), dot(gn, uv), dot(gn, fv));
    float p_dot_gn = dot(p_v, gn_v);
    float bias_far = max(0.01f, 1.65f * texel_sz);

    if (far_tlas_handoff) {
        if (max(abs(ndc.x), abs(ndc.y)) > 1.0f)
            return 1;
        float pcf_sum = 0.0f;
        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                int tx = int(floor(f_px.x)) + dx;
                int ty = int(floor(f_px.y)) + dy;
                float dz = load_vsm_view_texel(lv, tx, ty, fcb);
                if (dz <= 0.0f) {
                    pcf_sum += 1.0f;
                } else {
                    float z_occ = (near_z * far_z) / (dz * (far_z - near_z) + near_z);
                    float3 r_tap = float3(((float(tx) + 0.5f) * (1.0f / 128.0f) - 1.0f) * th,
                                          (1.0f - (float(ty) + 0.5f) * (1.0f / 128.0f)) * th, 1.0f);
                    float denom = dot(r_tap, gn_v);
                    float safe_den = (abs(denom) >= 0.12f) ? denom : (denom < 0.0f ? -0.12f : 0.12f);
                    float z_pl = clamp(p_dot_gn / safe_den, z_v - 4.0f * texel_sz, z_v + 4.0f * texel_sz);
                    pcf_sum += (z_occ >= z_pl - bias_far) ? 1.0f : 0.0f;
                }
            }
        }
        out_far_pcf = pcf_sum * (1.0f / 9.0f);
        return 2;
    }

    float min_safe_z = (kind == 1u) ? max(0.15f, 2.2f * max(l.width, l.height)) : max(0.12f, 2.5f * l.source_radius);
    if (z_v <= min_safe_z || max(abs(ndc.x), abs(ndc.y)) >= 1.0f)
        return -1;

    float tol_lit = max(2.5e-3f, 0.18f * texel_sz);
    int tx0 = int(floor(f_px.x));
    int ty0 = int(floor(f_px.y));
    float dz0 = load_vsm_view_texel(lv, tx0, ty0, fcb);
    if (dz0 < 0.0f)
        return -1;

    float max_delta = -1e30f;
    float min_delta = 1e30f;
    float min_z_occ = 1e30f;
    float pcf_sum = 0.0f;
    float near_occ = max(near_z * 1.5f, 1.8f * src_ext);
    float pot_pen = (src_ext > 1e-3f && z_v > near_occ) ? (src_ext * ((z_v - near_occ) / near_occ) * 0.45f) : 0.0f;
    float tap_step = clamp(1.25f + (max(2.0f * src_ext, pot_pen) / max(texel_sz, 1e-4f)), 1.25f, 24.0f);
    if (dz0 <= 0.0f) {
        min_delta = -1e30f;
        pcf_sum = 1.0f;
    } else {
        float z_occ0 = (near_z * far_z) / (dz0 * (far_z - near_z) + near_z);
        float3 r_tap0 = float3(((float(tx0) + 0.5f) * (1.0f / 128.0f) - 1.0f) * th,
                               (1.0f - (float(ty0) + 0.5f) * (1.0f / 128.0f)) * th, 1.0f);
        float denom0 = dot(r_tap0, gn_v);
        float safe_den0 = (abs(denom0) >= 0.12f) ? denom0 : (denom0 < 0.0f ? -0.12f : 0.12f);
        float z_pl0 = clamp(p_dot_gn / safe_den0, z_v - 4.0f * texel_sz, z_v + 4.0f * texel_sz);
        float delta0 = z_pl0 - z_occ0;
        max_delta = delta0;
        min_delta = delta0;
        pcf_sum = (delta0 <= bias_far) ? 1.0f : 0.0f;
        if (delta0 > tol_lit) {
            min_z_occ = z_occ0;
            if (src_ext > 0.0f && z_occ0 < z_v) {
                float pen0 = src_ext * ((z_v - z_occ0) / max(z_occ0, near_z)) * 1.15f;
                tap_step = clamp(ceil(pen0 / max(texel_sz, 1e-4f)), 1.5f, 96.0f);
            }
        }
    }

    if (f_px.x < tap_step + 1.0f || f_px.x >= 255.0f - tap_step ||
        f_px.y < tap_step + 1.0f || f_px.y >= 255.0f - tap_step)
        return -1;

    float mid_step = max(2.0f, tap_step * 0.35f);
    const float2 offsets[12] = {
        float2(-1.25f, 0.0f), float2(1.25f, 0.0f), float2(0.0f, -1.25f), float2(0.0f, 1.25f),
        float2(-mid_step, 0.0f), float2(mid_step, 0.0f), float2(0.0f, -mid_step), float2(0.0f, mid_step),
        float2(-tap_step, -tap_step), float2(tap_step, -tap_step),
        float2(-tap_step, tap_step), float2(tap_step, tap_step)
    };

    [unroll] for (uint k = 0u; k < 12u; ++k) {
        int tx = int(floor(f_px.x + offsets[k].x));
        int ty = int(floor(f_px.y + offsets[k].y));
        float dz = load_vsm_view_texel(lv, tx, ty, fcb);
        if (dz < 0.0f)
            return -1;
        if (dz <= 0.0f) {
            min_delta = -1e30f;
            pcf_sum += 1.0f;
        } else {
            float z_occ = (near_z * far_z) / (dz * (far_z - near_z) + near_z);
            float3 r_tap = float3(((float(tx) + 0.5f) * (1.0f / 128.0f) - 1.0f) * th,
                                  (1.0f - (float(ty) + 0.5f) * (1.0f / 128.0f)) * th, 1.0f);
            float denom = dot(r_tap, gn_v);
            float safe_den = (abs(denom) >= 0.12f) ? denom : (denom < 0.0f ? -0.12f : 0.12f);
            float z_pl = clamp(p_dot_gn / safe_den, z_v - 4.0f * texel_sz, z_v + 4.0f * texel_sz);
            float delta = z_pl - z_occ;
            if (delta > tol_lit)
                min_z_occ = min(min_z_occ, z_occ);
            max_delta = max(max_delta, delta);
            min_delta = min(min_delta, delta);
            pcf_sum += (delta <= bias_far) ? 1.0f : 0.0f;
        }
    }

    float foliage_lit_tol = -max(0.02f, 4.25f * texel_sz);
    if (max_delta <= (is_foliage ? foliage_lit_tol : tol_lit))
        return 1;

    float pen_world = (src_ext > 0.0f && min_z_occ < z_v)
                          ? (src_ext * ((z_v - min_z_occ) / max(min_z_occ, near_z)) * 1.15f)
                          : 0.0f;
    float tol_umbra = is_foliage ? max(0.15f, 3.0f * texel_sz + 0.25f * pen_world)
                                 : max(0.025f, 1.65f * texel_sz + 0.25f * pen_world);
    if (min_delta >= tol_umbra && tap_step * texel_sz >= pen_world && min_z_occ >= 3.5f * src_ext)
        return 0;

    if (fcb.instance_count > 256u && min_z_occ < z_v) {
        float3 occ_pos = l.position + rel * saturate(min_z_occ / max(z_v, 1e-4f));
        float occ_rel = min(length(occ_pos - fcb.camera_eye), length(occ_pos - fcb.lighting_focus));
        if (occ_rel >= 74.0f) {
            out_far_pcf = pcf_sum * (1.0f / 13.0f);
            return 2;
        }
    }

    return -1;
}

// ============================================================================
// OpenPBR v1.1 Canonical 24B/px Slab (3 x 8B) Codec
// ============================================================================
static const uint SLAB_PROFILE_STANDARD_F82 = 0u;
static const uint SLAB_PROFILE_COAT         = 1u;
static const uint SLAB_PROFILE_FUZZ         = 2u;
static const uint SLAB_PROFILE_FOLIAGE      = 3u;
static const uint SLAB_PROFILE_IRIDESCENT   = 4u;
static const uint SLAB_PROFILE_SUBSURFACE   = 5u;

struct OpenPbrSurface {
    float3 shading_normal;
    float roughness;
    float metallic;
    float spec_f0_dielectric; // [0..0.25] (IOR in [1.0..3.0]), default 0.04 (IOR=1.5, weight=1.0)
    float micro_ao;           // [0..1], default 1.0
    uint profile_id;
    uint flags;               // bit 0: valid_surface, bit 1: double_sided, bit 2: two_sided_emissive
    float3 base_color;
    float diffuse_roughness;
    float3 f82_or_lobe_color;
    float lobe_weight;
    float lobe_param0;        // coat_roughness / fuzz_roughness / scatter_g / thin_film_nm / sss_radius
    float lobe_param1;        // coat_darkening / thin_film_ior
};

uint pack_unorm(float v, uint bits) {
    uint max_v = (1u << bits) - 1u;
    return uint(round(saturate(v) * float(max_v))) & max_v;
}

float unpack_unorm(uint q, uint bits) {
    uint max_v = (1u << bits) - 1u;
    return float(q & max_v) / float(max_v);
}

uint pack_rgb_10_10_10_2(float3 linear_rgb, uint two_bit_aux) {
    float3 s = sqrt(saturate(linear_rgb));
    return pack_unorm(s.r, 10u) | (pack_unorm(s.g, 10u) << 10u) | (pack_unorm(s.b, 10u) << 20u) | ((two_bit_aux & 3u) << 30u);
}

float3 unpack_rgb_10_10_10(uint packed) {
    float3 s = float3(unpack_unorm(packed, 10u), unpack_unorm(packed >> 10u, 10u), unpack_unorm(packed >> 20u, 10u));
    return s * s;
}

void pack_openpbr_slab(OpenPbrSurface s, out uint2 slab0, out uint2 slab1) {
    slab0.x = pack_oct_snorm16(s.shading_normal);
    uint q_rough = pack_unorm(s.roughness, 8u);
    uint q_metal = pack_unorm(s.metallic, 6u);
    uint q_f0    = pack_unorm(sqrt(saturate(s.spec_f0_dielectric / 0.25f)), 6u); // 0.04 -> sqrt(0.16)=0.4 -> q_f0=25 exact!
    uint q_ao    = pack_unorm(s.micro_ao, 6u);
    slab0.y = q_rough | (q_metal << 8u) | (q_f0 << 14u) | (q_ao << 20u) | ((s.profile_id & 7u) << 26u) | ((s.flags & 7u) << 29u);

    uint q_eon = pack_unorm(s.diffuse_roughness, 2u);
    slab1.x = pack_rgb_10_10_10_2(s.base_color, q_eon);

    if (s.profile_id == SLAB_PROFILE_STANDARD_F82) {
        slab1.y = pack_rgb_10_10_10_2(s.f82_or_lobe_color, 0u);
    } else if (s.profile_id == SLAB_PROFILE_IRIDESCENT) {
        uint qw = pack_unorm(s.lobe_weight, 6u);
        uint qt = pack_unorm((s.lobe_param0 - 100.0f) / 900.0f, 10u);
        uint qi = pack_unorm((s.lobe_param1 - 1.0f) / 1.5f, 6u);
        slab1.y = qw | (qt << 6u) | (qi << 16u);
    } else {
        uint qw = pack_unorm(s.lobe_weight, 6u);
        uint qp = pack_unorm(s.lobe_param0, 6u);
        uint qd = pack_unorm(s.lobe_param1, 2u);
        float3 sc = sqrt(saturate(s.f82_or_lobe_color));
        uint cr = pack_unorm(sc.r, 6u);
        uint cg = pack_unorm(sc.g, 6u);
        uint cb = pack_unorm(sc.b, 6u);
        slab1.y = qw | (qp << 6u) | (qd << 12u) | (cr << 14u) | (cg << 20u) | (cb << 26u);
    }
}

OpenPbrSurface unpack_openpbr_slab(uint2 slab0, uint2 slab1) {
    OpenPbrSurface s = (OpenPbrSurface)0;
    s.flags = (slab0.y >> 29u) & 7u;
    if ((s.flags & 1u) == 0u) return s;
    s.shading_normal = unpack_oct_snorm16(slab0.x);
    s.roughness = unpack_unorm(slab0.y, 8u);
    s.metallic = unpack_unorm(slab0.y >> 8u, 6u);
    // Exact preservation of 0.04 default dielectric F0 (q_f0 == 25 -> 0.4^2 * 0.25 = 0.04):
    uint q_f0 = (slab0.y >> 14u) & 63u;
    float rt_f0 = (q_f0 == 25u) ? 0.4f : unpack_unorm(q_f0, 6u);
    s.spec_f0_dielectric = rt_f0 * rt_f0 * 0.25f;
    s.micro_ao = unpack_unorm(slab0.y >> 20u, 6u);
    s.profile_id = (slab0.y >> 26u) & 7u;
    s.base_color = unpack_rgb_10_10_10(slab1.x);
    s.diffuse_roughness = unpack_unorm(slab1.x >> 30u, 2u);

    if (s.profile_id == SLAB_PROFILE_STANDARD_F82) {
        s.f82_or_lobe_color = unpack_rgb_10_10_10(slab1.y);
    } else if (s.profile_id == SLAB_PROFILE_IRIDESCENT) {
        s.lobe_weight = unpack_unorm(slab1.y, 6u);
        s.lobe_param0 = 100.0f + 900.0f * unpack_unorm(slab1.y >> 6u, 10u);
        s.lobe_param1 = 1.0f + 1.5f * unpack_unorm(slab1.y >> 16u, 6u);
        s.f82_or_lobe_color = 1.0f;
    } else {
        s.lobe_weight = unpack_unorm(slab1.y, 6u);
        s.lobe_param0 = unpack_unorm(slab1.y >> 6u, 6u);
        s.lobe_param1 = unpack_unorm(slab1.y >> 12u, 2u);
        float3 sc = float3(unpack_unorm(slab1.y >> 14u, 6u), unpack_unorm(slab1.y >> 20u, 6u), unpack_unorm(slab1.y >> 26u, 6u));
        s.f82_or_lobe_color = sc * sc;
    }
    return s;
}

float3 eval_coat_normal(float3 shading_n, float3 geom_n, float coat_weight) {
    float tilt = 1.0f - smoothstep(0.90f, 0.998f, dot(shading_n, geom_n));
    return safe_unit_normal(lerp(shading_n, geom_n, saturate(coat_weight * tilt * 0.65f)));
}

float3 eval_openpbr_coat_darkening(float3 base_albedo, float base_roughness, float coat_weight, float coat_darkening,
                                   float3 coat_color) {
    float kr = lerp(0.20f, 0.60f, saturate(base_roughness));
    float3 delta = (1.0f - kr) / max(1.0f - kr * saturate(base_albedo), 1e-3f);
    float3 dark = lerp(1.0f.xxx, delta, saturate(coat_weight * coat_darkening));
    float3 tint = lerp(1.0f.xxx, max(coat_color, 0.0f), saturate(coat_weight));
    return dark * tint;
}

// ============================================================================
// NVIDIA SHaRC v1.8.3 (`HashGridCommon.h` + `SharcCommon.h`) + AMD GI-1.2 Multi-Bounce Cache
// ============================================================================
static const uint SHARC_CAPACITY                = 262144u; // 16,384 buckets x 16 slots (64B cache-line aligned)
static const uint SHARC_BUCKET_SIZE             = 16u;
static const uint SHARC_BUCKET_COUNT            = SHARC_CAPACITY / SHARC_BUCKET_SIZE;
static const uint SHARC_INVALID_SLOT            = 0xFFFFFFFFu;
static const float SHARC_SCENE_SCALE            = 10.0f;   // Level 1 (0..4m) = 0.20m, Level 2 (4..8m) = 0.40m, Level 3 = 0.80m ...
static const float SHARC_RADIANCE_SCALE         = 16384.0f; // High-precision pre-exposed u32 InterlockedAdd scale (`2^14`, zero dark-GI truncation)
static const uint SHARC_ACCUM_FRAME_MAX         = 64u;
static const uint SHARC_INDIRECT_FRAME_MAX      = 48u;
static const uint SHARC_STALE_FRAME_MAX         = 64u;
static const uint SHARC_POS_BITS                = 17u;
static const uint SHARC_POS_MASK                = (1u << SHARC_POS_BITS) - 1u;
static const uint SHARC_LEVEL_BITS              = 7u;
static const uint SHARC_LEVEL_MASK              = (1u << SHARC_LEVEL_BITS) - 1u;

uint sharc_hash_jenkins32(uint a) {
    a = (a + 0x7ed55d16u) + (a << 12u);
    a = (a ^ 0xc761c23cu) ^ (a >> 19u);
    a = (a + 0x165667b1u) + (a << 5u);
    a = (a + 0xd3a2646cu) ^ (a << 9u);
    a = (a + 0xfd7046c5u) + (a << 3u);
    a = (a ^ 0xb55a4f09u) ^ (a >> 16u);
    return a;
}

uint sharc_hash_xx32_pair(uint2 k) {
    uint h = 0x9E3779B1u + k.x * 0x85EBCA77u;
    h = ((h << 13u) | (h >> 19u)) * 0x9E3779B1u;
    h += k.y * 0xC2B2AE3Du;
    h = ((h << 17u) | (h >> 15u)) * 0x27D4EB2Fu;
    h ^= h >> 15u;
    h *= 0x85EBCA77u;
    h ^= h >> 13u;
    h *= 0xC2B2AE3Du;
    h ^= h >> 16u;
    return max(h, 1u); // 0u is reserved for empty slot (`HASH_GRID_INVALID_HASH_KEY`)
}

uint sharc_bucket_base_from_key(uint2 key) {
    uint h = sharc_hash_jenkins32(key.x) ^ sharc_hash_jenkins32(key.y);
    return (h & (SHARC_BUCKET_COUNT - 1u)) * SHARC_BUCKET_SIZE;
}

uint sharc_get_level(float3 pos, float3 camera_pos) {
    float3 d = camera_pos - pos;
    float d2 = max(dot(d, d), 1e-10f);
    float lvl = 0.5f * log2(d2);
    return uint(clamp(lvl, 1.0f, float(SHARC_LEVEL_MASK)));
}

float sharc_get_voxel_size(uint level) {
    return exp2(float(level)) * (1.0f / SHARC_SCENE_SCALE);
}

uint sharc_encode_normal_bits(float3 n) {
    float2 oct = oct_encode(safe_unit_normal(n + float3(1e-3f, 1e-3f, 1e-3f)));
    uint bx = min(uint(max(0.0f, (oct.x * 0.5f + 0.5f) * 8.0f)), 7u);
    uint by = min(uint(max(0.0f, (oct.y * 0.5f + 0.5f) * 8.0f)), 7u);
    return bx | (by << 3u); // 6-bit octahedral normal (64 directional sectors, thin-wall & curved-pillar safe)
}

uint2 sharc_pack_grid_key(int3 grid_pos, uint level, uint normal_bits) {
    uint ux = uint(grid_pos.x) & SHARC_POS_MASK;
    uint uy = uint(grid_pos.y) & SHARC_POS_MASK;
    uint uz = uint(grid_pos.z) & SHARC_POS_MASK;
    // Bit layout (64 bits = uint2(lo, hi)):
    //   [0..16]: ux (17b), [17..31]: uy_lo (15b)
    //   [32..33]: uy_hi (2b), [34..50]: uz (17b), [51..57]: level (7b), [58..63]: normal_bits (6b)
    uint lo = ux | (uy << 17u);
    uint hi = (uy >> 15u) | (uz << 2u) | ((level & SHARC_LEVEL_MASK) << 19u) | ((normal_bits & 63u) << 26u);
    return uint2(lo, hi);
}

uint2 sharc_compute_key(float3 pos, float3 normal, float3 camera_pos, out float out_voxel_size) {
    float3 biased_p = pos + 1e-4f;
    uint level = sharc_get_level(biased_p, camera_pos);
    out_voxel_size = sharc_get_voxel_size(level);
    int3 gp = int3(floor(biased_p / out_voxel_size));
    return sharc_pack_grid_key(gp, level, sharc_encode_normal_bits(normal));
}

uint2 sharc_adjacent_level_key(uint2 key, float3 camera_pos, float3 camera_pos_prev) {
    uint ux = key.x & SHARC_POS_MASK;
    uint uy = (key.x >> 17u) | ((key.y & 3u) << 15u);
    uint uz = (key.y >> 2u) & SHARC_POS_MASK;
    int3 gp = int3(ux, uy, uz);
    gp = (gp << (32 - int(SHARC_POS_BITS))) >> (32 - int(SHARC_POS_BITS));
    int level = int((key.y >> 19u) & SHARC_LEVEL_MASK);
    uint norm_bits = (key.y >> 26u) & 63u;

    float vsz = sharc_get_voxel_size(uint(max(level, 1)));
    float inv_vsz = 1.0f / vsz;
    int3 cam_d = int3(floor(camera_pos * inv_vsz)) - gp;
    int3 prv_d = int3(floor(camera_pos_prev * inv_vsz)) - gp;
    int dist_cur = dot(cam_d, cam_d);
    int dist_prv = dot(prv_d, prv_d);
    if (dist_cur < dist_prv) {
        gp = int3(floor(float3(gp) * 0.5f));
        level = min(level + 1, int(SHARC_LEVEL_MASK));
    } else {
        gp = gp * 2;
        level = max(level - 1, 1);
    }
    return sharc_pack_grid_key(gp, uint(level), norm_bits);
}

uint sharc_find_slot_srv(StructuredBuffer<uint> hash_keys, StructuredBuffer<uint2> voxel_keys, uint2 key) {
    uint checksum = sharc_hash_xx32_pair(key);
    uint base_slot = sharc_bucket_base_from_key(key);
    [loop] for (uint off = 0u; off < SHARC_BUCKET_SIZE; ++off) {
        uint slot = base_slot + off;
        if (hash_keys[slot] == checksum && all(voxel_keys[slot] == key))
            return slot;
    }
    return SHARC_INVALID_SLOT;
}

uint sharc_find_slot_uav(RWStructuredBuffer<uint> hash_keys, uint2 key) {
    uint checksum = sharc_hash_xx32_pair(key);
    uint base_slot = sharc_bucket_base_from_key(key);
    [loop] for (uint off = 0u; off < SHARC_BUCKET_SIZE; ++off) {
        if (hash_keys[base_slot + off] == checksum)
            return base_slot + off;
    }
    return SHARC_INVALID_SLOT;
}

uint sharc_find_or_insert_slot(RWStructuredBuffer<uint> hash_keys, RWStructuredBuffer<uint2> voxel_keys, uint2 key) {
    uint checksum = sharc_hash_xx32_pair(key);
    uint base_slot = sharc_bucket_base_from_key(key);
    // Fast read-only probe across the 64-byte bucket cache line (avoids atomic contention on 98% of steady-state hits):
    [loop] for (uint r = 0u; r < SHARC_BUCKET_SIZE; ++r) {
        if (hash_keys[base_slot + r] == checksum)
            return base_slot + r;
    }
    [loop] for (uint off = 0u; off < SHARC_BUCKET_SIZE; ++off) {
        uint prev = 0u;
        InterlockedCompareExchange(hash_keys[base_slot + off], 0u, checksum, prev);
        if (prev == 0u || prev == checksum) {
            if (prev == 0u)
                voxel_keys[base_slot + off] = key;
            return base_slot + off;
        }
    }
    return SHARC_INVALID_SLOT;
}

uint sharc_pack_fp16x2(float a, float b) {
    uint lo = f32tof16(clamp(a, 0.0f, 65504.0f)) & 0xFFFFu;
    uint hi = f32tof16(clamp(b, 0.0f, 65504.0f)) & 0xFFFFu;
    return lo | (hi << 16u);
}

float2 sharc_unpack_fp16x2(uint p) {
    return float2(f16tof32(p & 0xFFFFu), f16tof32(p >> 16u));
}

uint4 sharc_pack_resolved(float3 radiance, float sample_num, uint accum_frames, uint stale_frames, uint fade_mask) {
    return uint4(sharc_pack_fp16x2(radiance.r, radiance.g),
                 sharc_pack_fp16x2(radiance.b, min(sample_num, 65504.0f)),
                 (min(accum_frames, 65535u) & 0xFFFFu) | ((min(stale_frames, 65535u) & 0xFFFFu) << 16u),
                 fade_mask);
}

void sharc_unpack_resolved(uint4 packed, out float3 radiance, out float sample_num, out uint accum_frames,
                           out uint stale_frames, out uint fade_mask) {
    float2 rg = sharc_unpack_fp16x2(packed.x);
    float2 bs = sharc_unpack_fp16x2(packed.y);
    radiance = float3(rg.x, rg.y, bs.x);
    sample_num = bs.y;
    accum_frames = packed.z & 0xFFFFu;
    stale_frames = packed.z >> 16u;
    fade_mask = packed.w;
}

float3 sharc_jitter_query_pos(float3 pos, float3 normal, float voxel_size, float max_dist, float2 u) {
    float3 up_ref = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T = normalize(cross(up_ref, normal));
    float3 B = cross(normal, T);
    float r = min(voxel_size * 0.45f, max_dist * 0.40f);
    return pos + (T * (u.x - 0.5f) + B * (u.y - 0.5f)) * r;
}

bool sharc_query_resolved_srv(StructuredBuffer<uint> hash_keys, StructuredBuffer<uint2> voxel_keys,
                              StructuredBuffer<uint4> resolved_buf, uint sub_slot, float3 pos, float3 normal,
                              float3 camera_pos, out float3 out_rad) {
    out_rad = 0.0f;
    float vsz;
    uint2 key = sharc_compute_key(pos, normal, camera_pos, vsz);
    uint slot = sharc_find_slot_srv(hash_keys, voxel_keys, key);
    if (slot == SHARC_INVALID_SLOT)
        return false;
    float3 rad;
    float snum;
    uint af, sf, fm;
    sharc_unpack_resolved(resolved_buf[slot * 2u + sub_slot], rad, snum, af, sf, fm);
    if (snum <= 0.0f)
        return false;
    out_rad = rad;
    return true;
}

float3 sample_atmos_transmittance(float h_km, float cos_zenith, GlobalFrameConstants fcb) {
    if (fcb.atmos_trans_srv == 0xFFFFFFFFu)
        return 1.0f;
    Texture2D<float4> lut = ResourceDescriptorHeap[atelier_safe_desc(fcb.atmos_trans_srv)];
    SamplerState smp = SamplerDescriptorHeap[0];
    float u = clamp(0.5f * cos_zenith + 0.5f, 0.5f / 256.0f, 255.5f / 256.0f);
    float v = sqrt(saturate(h_km * 0.01f)) * (63.0f / 64.0f) + (0.5f / 64.0f);
    return lut.SampleLevel(smp, float2(u, v), 0.0f).rgb;
}

float3 sample_atmos_multiscatter(float h_km, float sun_cos_z, GlobalFrameConstants fcb) {
    if (fcb.atmos_multiscatter_srv == 0xFFFFFFFFu)
        return 0.0f;
    Texture2D<float4> lut = ResourceDescriptorHeap[atelier_safe_desc(fcb.atmos_multiscatter_srv)];
    SamplerState smp = SamplerDescriptorHeap[0];
    float2 uv = float2(0.5f * sun_cos_z + 0.5f, saturate(h_km * 0.01f)) * (31.0f / 32.0f) + (0.5f / 32.0f);
    return lut.SampleLevel(smp, uv, 0.0f).rgb;
}

float3 eval_hillaire_sky(float3 world_dir, float roughness_mip, GlobalFrameConstants fcb) {
    float3 sun_dir = float3(0.0f, 0.85f, 0.5268f);
    float3 sun_illum = max(fcb.environment_radiance, 0.0f) * ATELIER_PI;
    uint sun_idx = fcb.primary_sun_and_gi_cands & 0xFFFFu;
    if (sun_idx != 0xFFFFu && fcb.lights_srv != 0xFFFFFFFFu && sun_idx < fcb.dir_light_count) {
        StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
        GpuLightRecord sun = lights[sun_idx];
        sun_dir = safe_unit_normal(-sun.direction);
        sun_illum = sun.color_intensity;
    }
    const float r_bottom = 6360.0f;
    const float r_top = 6460.0f;
    float h_km = clamp(max(fcb.camera_eye.y, 0.0f) * 1e-3f + 0.05f, 0.01f, 90.0f);
    float r0 = r_bottom + h_km;
    float mu = clamp(world_dir.y, -0.35f, 1.0f);
    float disc_top = max(0.0f, r0 * r0 * (mu * mu - 1.0f) + r_top * r_top);
    float t_max = min(max(0.0f, -r0 * mu + sqrt(disc_top)), 120.0f);

    float cos_theta = dot(world_dir, sun_dir);
    float phase_r = (3.0f / (16.0f * ATELIER_PI)) * (1.0f + cos_theta * cos_theta);
    const float g = lerp(0.78f, 0.35f, saturate(roughness_mip * 0.16f));
    float g2 = g * g;
    float phase_m = (3.0f / (8.0f * ATELIER_PI)) * ((1.0f - g2) * (1.0f + cos_theta * cos_theta)) /
                    ((2.0f + g2) * pow(max(1.0f + g2 - 2.0f * g * cos_theta, 1e-3f), 1.5f));

    const float3 beta_r = float3(5.802e-3f, 13.558e-3f, 33.1e-3f);
    const float beta_m_s = 3.996e-3f;
    const float beta_m_e = 4.40e-3f;
    const float3 beta_o = float3(0.650e-3f, 1.881e-3f, 0.085e-3f);

    float3 trans = 1.0f;
    float3 lum = 0.0f;
    const uint steps = 8u;
    float dt = t_max / float(steps);
    [unroll] for (uint s = 0u; s < steps; ++s) {
        float t = (float(s) + 0.5f) * dt;
        float3 p = float3(0.0f, r0, 0.0f) + world_dir * t;
        float r_p = length(p);
        float h = max(r_p - r_bottom, 0.0f);
        float d_r = exp(-h / 8.0f);
        float d_m = exp(-h / 1.2f);
        float d_o = max(0.0f, 1.0f - abs(h - 25.0f) / 15.0f);
        float3 sig_s = beta_r * d_r + beta_m_s * d_m;
        float3 sig_t = beta_r * d_r + beta_m_e * d_m + beta_o * d_o;
        float3 step_t = exp(-sig_t * dt);

        float sun_mu = dot(p / max(r_p, 1.0f), sun_dir);
        float3 sun_tr = sample_atmos_transmittance(h, sun_mu, fcb);
        float3 ms = sample_atmos_multiscatter(h, sun_mu, fcb);
        float3 in_scat = (beta_r * d_r * phase_r + beta_m_s * d_m * phase_m) * sun_tr + sig_s * ms;
        float3 sint = in_scat * (1.0f - step_t) / max(sig_t, 1e-6f);
        lum += trans * sint;
        trans *= step_t;
    }
    if (world_dir.y < 0.0f)
        lum *= exp(world_dir.y * 6.0f);
    return max(lum * sun_illum, 0.0f);
}

float3 eval_atelier_sky(float3 world_dir, float roughness_mip, GlobalFrameConstants fcb) {
    float3 rad = 0.0f;
    if ((fcb.flags & (1u << 5u)) != 0u && fcb.sky_map_srv != 0xFFFFFFFFu) {
        if (any(fcb.environment_radiance > 0.0f)) {
            float ca = cos(fcb.sky_rotation_rad);
            float sa = sin(fcb.sky_rotation_rad);
            float3 md = float3(world_dir.x * ca - world_dir.z * sa, world_dir.y, world_dir.x * sa + world_dir.z * ca);
            float u = atan2(md.x, md.z) * (0.5f * ATELIER_INV_PI) + 0.5f;
            float v = 0.5f - asin(clamp(md.y, -1.0f, 1.0f)) * ATELIER_INV_PI;
            Texture2D<float4> sky = ResourceDescriptorHeap[atelier_safe_desc(fcb.sky_map_srv)];
            uint sw = 1u, sh = 1u, slvls = 1u;
            sky.GetDimensions(0u, sw, sh, slvls);
            uint mip_hi = min(uint(ceil(max(roughness_mip, 0.0f))), max(slvls, 1u) - 1u);
            float half_v = min(0.5f / float(max(1u, sh >> mip_hi)), 0.499f);
            v = clamp(v, half_v, 1.0f - half_v);
            SamplerState smp = SamplerDescriptorHeap[1]; // linear_wrap (u wraps 360 deg; v clamped away from pole wrap)
            rad = sky.SampleLevel(smp, float2(u, v), roughness_mip).rgb * fcb.sky_scale * fcb.environment_radiance;
        }
    } else if (fcb.env_sky_mode == 1u) {
        rad = eval_hillaire_sky(world_dir, roughness_mip, fcb);
    } else {
        rad = max(fcb.environment_radiance, 0.0f);
    }
    return clamp(rad * fcb.exposure_scale, 0.0f, 65504.0f);
}

float3 eval_atelier_sky_diffuse_radiance(float3 world_n, GlobalFrameConstants fcb) {
    float3 rad = 0.0f;
    if ((fcb.flags & (1u << 5u)) != 0u) {
        if (any(fcb.environment_radiance > 0.0f)) {
            float ca = cos(fcb.sky_rotation_rad);
            float sa = sin(fcb.sky_rotation_rad);
            float3 n = float3(world_n.x * ca - world_n.z * sa, world_n.y, world_n.x * sa + world_n.z * ca);
            float3 res = fcb.sky_sh3[0].rgb +
                         fcb.sky_sh3[1].rgb * n.y +
                         fcb.sky_sh3[2].rgb * n.z +
                         fcb.sky_sh3[3].rgb * n.x +
                         fcb.sky_sh3[4].rgb * (n.x * n.y) +
                         fcb.sky_sh3[5].rgb * (n.y * n.z) +
                         fcb.sky_sh3[6].rgb * (3.0f * n.z * n.z - 1.0f) +
                         fcb.sky_sh3[7].rgb * (n.x * n.z) +
                         fcb.sky_sh3[8].rgb * (n.x * n.x - n.y * n.y);
            rad = max(res, 0.0f);
        }
    } else if (fcb.env_sky_mode == 1u) {
        rad = eval_hillaire_sky(world_n, 4.0f, fcb);
    } else {
        rad = max(fcb.environment_radiance, 0.0f);
    }
    return clamp(rad * fcb.exposure_scale, 0.0f, 65504.0f);
}

// ============================================================================
// Decorrelated PCG2D Cranley-Patterson Jitter, Pixar ONB, Clarberg Equal-Area
// Hemi-Octahedron Bijection, & White-Furnace Calibrated Orthonormal SH2
// ============================================================================
uint2 atelier_pcg2d(uint2 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= (v >> 16u);
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= (v >> 16u);
    return v;
}

float2 atelier_blue_jitter2(uint2 coord, uint temporal_idx, uint salt) {
    uint epoch = temporal_idx >> 6u;
    uint2 h = atelier_pcg2d(uint2(coord.x ^ (salt * 0x9E3779B9u), coord.y ^ (epoch * 0x85EBCA6Bu + salt * 0xC2B2AE35u)));
    float2 cp_shift = float2(float(h.x & 0xFFFFu), float(h.y & 0xFFFFu)) * (1.0f / 65536.0f);
    float2 r2_seq = float(temporal_idx & 63u) * float2(0.7548776662f, 0.5698402910f);
    return frac(cp_shift + r2_seq);
}

// Pixar's Revised Orthonormal Basis (Duff et al., JCGT 2017) — zero sqrt, zero denormal NaN on S^2:
void atelier_build_onb(float3 n, out float3 b1, out float3 b2) {
    float s = (n.z >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z);
    float b = n.x * n.y * a;
    b1 = float3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
    b2 = float3(b, s + n.y * n.y * a, -n.y);
}

float3 atelier_local_to_world(float3 local_dir, float3 n) {
    float3 b1, b2;
    atelier_build_onb(n, b1, b2);
    return local_dir.x * b1 + local_dir.y * b2 + local_dir.z * n;
}

float3 atelier_world_to_local(float3 world_dir, float3 n) {
    float3 b1, b2;
    atelier_build_onb(n, b1, b2);
    return float3(dot(world_dir, b1), dot(world_dir, b2), dot(world_dir, n));
}

// Clarberg (2008) / AMD Capsaicin GI-1.2 Equal-Area Hemi-Octahedron Bijection (`Delta_omega = 2*pi / 64` per cell):
float3 hemi_oct_decode(float2 u) {
    float2 st = 2.0f * clamp(u, 0.0f, 1.0f) - 1.0f;
    float2 s = float2(st.x + st.y, st.x - st.y) * 0.5f;
    float2 a = abs(s);
    float distance = 1.0f - (a.x + a.y);
    float radius = 1.0f - abs(distance);
    float phi = (radius <= 0.0f) ? 0.0f : (0.25f * ATELIER_PI * ((a.y - a.x) / radius + 1.0f));
    float r2 = radius * radius;
    float sin_theta = radius * sqrt(max(0.0f, 2.0f - r2));
    float sin_phi, cos_phi;
    sincos(phi, sin_phi, cos_phi);
    float sx = (s.x > 0.0f) ? 1.0f : ((s.x < 0.0f) ? -1.0f : 0.0f);
    float sy = (s.y > 0.0f) ? 1.0f : ((s.y < 0.0f) ? -1.0f : 0.0f);
    float sz = (distance >= 0.0f) ? 1.0f : -1.0f;
    return float3(sin_theta * sx * cos_phi, sin_theta * sy * sin_phi, sz * max(0.0f, 1.0f - r2));
}

float2 hemi_oct_encode(float3 local_dir) {
    float3 d = safe_unit_normal(float3(local_dir.x, local_dir.y, max(local_dir.z, 0.0f)));
    float3 a = abs(d);
    float radius = sqrt(max(0.0f, 1.0f - a.z));
    float ma = max(a.x, a.y);
    float mi = min(a.x, a.y);
    float ratio = (ma <= 0.0f) ? 0.0f : (mi / ma);
    float phi = atan(ratio) * (2.0f * ATELIER_INV_PI);
    phi = (a.x >= a.y) ? phi : (1.0f - phi);
    float t = phi * radius;
    float s = radius - t;
    float sx = s * ((d.x > 0.0f) ? 1.0f : ((d.x < 0.0f) ? -1.0f : 0.0f));
    float sy = t * ((d.y > 0.0f) ? 1.0f : ((d.y < 0.0f) ? -1.0f : 0.0f));
    return clamp(0.5f * float2(sx + sy, sx - sy) + 0.5f, 0.0f, 1.0f);
}

// Exact SO(3)-invariant discrete 8x8 Clarberg midpoint quadrature weight (`E(N)/pi == 1.000000000000` for all N in S^2):
static const float ATELIER_PROBE_CELL_SH_WEIGHT = 0.0962792578f;

void sh2_get_coefficients(float3 d, out float c[9]) {
    float pz2 = d.z * d.z;
    c[0] = 0.2820947918f;
    c[2] = 0.4886025119f * d.z;
    c[6] = 0.9461746958f * pz2 - 0.3153915653f;
    float fTmpA = -0.4886025119f;
    c[3] = fTmpA * d.x;
    c[1] = fTmpA * d.y;
    float fTmpB = -1.0925484306f * d.z;
    c[7] = fTmpB * d.x;
    c[5] = fTmpB * d.y;
    float fC1 = d.x * d.x - d.y * d.y;
    float fS1 = 2.0f * d.x * d.y;
    float fTmpC = 0.5462742153f;
    c[8] = fTmpC * fC1;
    c[4] = fTmpC * fS1;
}

void sh2_get_clamped_cosine_over_pi(float3 n, out float c[9]) {
    const float a0 = 1.0f;
    const float a1 = 2.0f / 3.0f;
    const float a2 = 0.25f * 0.75f; // L2 window w_l2 = 0.75f suppresses ringing while preserving white-furnace = 1.0
    float pz2 = n.z * n.z;
    c[0] = 0.2820947918f * a0;
    c[2] = 0.4886025119f * n.z * a1;
    c[6] = (0.9461746958f * pz2 - 0.3153915653f) * a2;
    float fTmpA = -0.4886025119f * a1;
    c[3] = fTmpA * n.x;
    c[1] = fTmpA * n.y;
    float fTmpB = -1.0925484306f * n.z * a2;
    c[7] = fTmpB * n.x;
    c[5] = fTmpB * n.y;
    float fC1 = n.x * n.x - n.y * n.y;
    float fS1 = 2.0f * n.x * n.y;
    float fTmpC = 0.5462742153f * a2;
    c[8] = fTmpC * fC1;
    c[4] = fTmpC * fS1;
}

float gi_pixel_world_size(float3 world_pos, GlobalFrameConstants fcb) {
    float3 pv = mul(float4(world_pos, 1.0f), fcb.view).xyz;
    float clip_w = max(abs(pv.z * fcb.projection[2][3] + fcb.projection[3][3]), 1e-3f);
    float proj_h = max(abs(fcb.projection[1][1]) * max(fcb.scene_viewport.w, 1.0f), 1.0f);
    return (2.0f * clip_w) / proj_h;
}

float atelier_luma(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 atelier_karis_compress(float3 c) {
    float3 pos_c = max(c, 0.0f);
    return pos_c / (1.0f + atelier_luma(pos_c));
}

float3 atelier_karis_expand(float3 k) {
    float3 pos_k = max(k, 0.0f);
    return pos_k / max(1.0f - min(atelier_luma(pos_k), 0.9999f), 1e-4f);
}

// Precomputed `(cos(alpha_b), sin(alpha_b))` for 32 signed GT-VBAO sectors `alpha_b = ((b + 0.5)/32 - 0.5) * pi`:
static const float2 k_gtvbao_sector_cs[32] = {
    float2(0.0490676743f, -0.9987954562f), float2(0.1467304745f, -0.9891765100f),
    float2(0.2429801799f, -0.9700312532f), float2(0.3368898534f, -0.9415440652f),
    float2(0.4275550934f, -0.9039892931f), float2(0.5141027442f, -0.8577286100f),
    float2(0.5956993045f, -0.8032075315f), float2(0.6715589548f, -0.7409511254f),
    float2(0.7409511254f, -0.6715589548f), float2(0.8032075315f, -0.5956993045f),
    float2(0.8577286100f, -0.5141027442f), float2(0.9039892931f, -0.4275550934f),
    float2(0.9415440652f, -0.3368898534f), float2(0.9700312532f, -0.2429801799f),
    float2(0.9891765100f, -0.1467304745f), float2(0.9987954562f, -0.0490676743f),
    float2(0.9987954562f,  0.0490676743f), float2(0.9891765100f,  0.1467304745f),
    float2(0.9700312532f,  0.2429801799f), float2(0.9415440652f,  0.3368898534f),
    float2(0.9039892931f,  0.4275550934f), float2(0.8577286100f,  0.5141027442f),
    float2(0.8032075315f,  0.5956993045f), float2(0.7409511254f,  0.6715589548f),
    float2(0.6715589548f,  0.7409511254f), float2(0.5956993045f,  0.8032075315f),
    float2(0.5141027442f,  0.8577286100f), float2(0.4275550934f,  0.9039892931f),
    float2(0.3368898534f,  0.9415440652f), float2(0.2429801799f,  0.9700312532f),
    float2(0.1467304745f,  0.9891765100f), float2(0.0490676743f,  0.9987954562f)
};

// ============================================================================
// Tone Mapping & Deterministic 0.99-LSB IGN sRGB Quantization
// ============================================================================
float3 tonemap_khronos_pbr_neutral(float3 color) {
    color = max(color, 0.0f);
    const float start_compression = 0.76f;
    const float desaturation = 0.15f;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08f ? (x - 6.25f * x * x) : 0.04f;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) return max(color, 0.0f);
    const float d = 1.0f - start_compression;
    float new_peak = 1.0f - (d * d) / (peak + d - start_compression);
    color *= new_peak / max(peak, 1e-6f);
    float g = 1.0f - 1.0f / (desaturation * (peak - new_peak) + 1.0f);
    return lerp(color, new_peak.xxx, g);
}

float3 tonemap_agx(float3 val) {
    val = max(val, 1e-10f);
    const float3x3 agx_mat = float3x3(
        0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f,
        0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f,
        0.0423756549057051f, 0.0784336f, 0.879142973793104f);
    const float3x3 agx_mat_inv = float3x3(
        1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f,
        -0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f,
        -0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f);
    val = mul(agx_mat, val);
    val = clamp((log2(val) + 12.47393f) / 16.5f, 0.0f, 1.0f);
    float3 x2 = val * val;
    float3 x4 = x2 * x2;
    val = 15.5f * x4 * x2 - 40.14f * x4 * val + 31.96f * x4 - 6.868f * x2 * val + 0.4298f * x2 + 0.1191f * val - 0.00232f;
    val = mul(agx_mat_inv, val);
    return saturate(pow(max(val, 0.0f), 2.2f));
}

float linear_to_srgb_ch(float c) {
    c = saturate(c);
    return c <= 0.0031308f ? (c * 12.92f) : (1.055f * pow(c, 1.0f / 2.4f) - 0.055f);
}

float srgb_to_linear_ch(float s) {
    s = saturate(s);
    return s <= 0.04045f ? (s / 12.92f) : pow((s + 0.055f) / 1.055f, 2.4f);
}

float3 quantize_srgb_ign(float3 linear_sdr, uint2 px) {
    float ign = frac(52.9829189f * frac(dot(float2(px), float2(0.06711056f, 0.00583715f))));
    float dither = (ign - 0.5f) * 0.99f;
    float3 srgb255 = float3(linear_to_srgb_ch(linear_sdr.r), linear_to_srgb_ch(linear_sdr.g), linear_to_srgb_ch(linear_sdr.b)) * 255.0f;
    float3 code = clamp(round(srgb255 + dither), 0.0f, 255.0f) * (1.0f / 255.0f);
    return float3(srgb_to_linear_ch(code.r), srgb_to_linear_ch(code.g), srgb_to_linear_ch(code.b));
}

#endif
