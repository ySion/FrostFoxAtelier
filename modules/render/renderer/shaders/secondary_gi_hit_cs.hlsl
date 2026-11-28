// Secondary GI Hit Shading CS (per-`.source` Archetype, invoked via CmdDispatchIndirect on compacted GI RayHitHeaders)
// Uses `ArchGi::ms_shade_gi_hit` (with normal_world/normal_texture/TBN pruned by SliceProjector and OpenPBR §3.10
// directional-hemispherical effective albedo + ThinFoliage transmission folded), evaluates pre-exposed NEE,
// queries & updates NVIDIA SHaRC v1.8.3 + AMD Capsaicin GI-1.2 multi-bounce surface radiance cache, and writes ScreenProbe radiance!

float3 sharc_cosine_hemisphere_dir(float2 u, float3 N) {
    float r = sqrt(saturate(u.x));
    float phi = 2.0f * ATELIER_PI * u.y;
    float lz = sqrt(max(0.0f, 1.0f - u.x));
    float3 up_ref = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T = normalize(cross(up_ref, N));
    float3 B = cross(N, T);
    return normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * lz);
}

struct GpuGiRayHit {
    uint probe_and_ray; // bits 0..23: screen_probe_idx, bits 24..31: local_ray_idx
    uint instance_id;
    uint global_tri_id;
    uint packed_bary;   // f16 b1 | (f16 b2 << 16)
    float3 ray_origin;
    float hit_t;
    float3 ray_dir;
    uint surf_idx;
};

[numthreads(64, 1, 1)]
void main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint tid = (gid.y * 1024u + gid.x) * 64u + gtid;
    StructuredBuffer<uint> counts = ResourceDescriptorHeap[atelier_safe_desc(g_aux0_srv)];
    StructuredBuffer<uint> offsets = ResourceDescriptorHeap[atelier_safe_desc(g_bin_offset_or_cluster_srv)];
    uint count = counts[g_archetype_index];
    if (tid >= count)
        return;

    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    StructuredBuffer<uint> hit_indices = ResourceDescriptorHeap[atelier_safe_desc(g_aux1_srv)];
    StructuredBuffer<GpuGiRayHit> hits = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_idx)];
    uint hit_id = hit_indices[offsets[g_archetype_index] + tid];
    GpuGiRayHit hit = hits[hit_id];

    StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
    StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
    GpuInstanceRecord inst = instances[hit.instance_id];
    GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];

    StructuredBuffer<uint> blas_idx = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.blas_indices_srv))];
    StructuredBuffer<GpuPackedPosition> pos_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.positions_srv))];
    StructuredBuffer<GpuPackedAttributes> attr_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.attributes_srv))];
    StructuredBuffer<uint> fmeta_s = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.face_meta_srv))];

    uint i0 = blas_idx[hit.global_tri_id * 3u + 0u];
    uint i1 = blas_idx[hit.global_tri_id * 3u + 1u];
    uint i2 = blas_idx[hit.global_tri_id * 3u + 2u];
    uint fmeta = fmeta_s[hit.global_tri_id];
    bool winding_flipped = (fmeta >> 31u) != 0u;
    bool manifold_closed = (fmeta & (1u << 30u)) != 0u;

    float b1 = f16tof32(hit.packed_bary & 0xFFFFu);
    float b2 = f16tof32(hit.packed_bary >> 16u);
    float w0 = 1.0f - b1 - b2;

    float3 lp0 = pos_s[i0].pos, lp1 = pos_s[i1].pos, lp2 = pos_s[i2].pos;
    GpuPackedAttributes a0 = attr_s[i0], a1 = attr_s[i1], a2 = attr_s[i2];

    float3 local_n0 = unpack_oct_snorm16(a0.oct_normal);
    float3 gn_auth = reconstruct_geom_normal(lp0, lp1, lp2, local_n0, winding_flipped, inst);
    float3 wp = mul(float4(lp0 * w0 + lp1 * b1 + lp2 * b2, 1.0f), inst.world).xyz;
    float3 nw0 = transform_local_normal(local_n0, inst);
    float3 nw1 = transform_local_normal(unpack_oct_snorm16(a1.oct_normal), inst);
    float3 nw2 = transform_local_normal(unpack_oct_snorm16(a2.oct_normal), inst);
    float3 nw = safe_unit_normal(nw0 * w0 + nw1 * b1 + nw2 * b2);
    float4 tw0 = transform_local_tangent(unpack_oct_tangent_sign(a0.oct_tangent_sign), inst);
    float4 tw1 = transform_local_tangent(unpack_oct_tangent_sign(a1.oct_tangent_sign), inst);
    float4 tw2 = transform_local_tangent(unpack_oct_tangent_sign(a2.oct_tangent_sign), inst);
    float4 tw = tw0 * w0 + tw1 * b1 + tw2 * b2;

    StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(g_material_surfaces_srv)];
    ByteAddressBuffer material_parameters = ResourceDescriptorHeap[atelier_safe_desc(g_material_params_srv)];
    GpuSurfaceRecord surf = surfaces[hit.surf_idx];

    bool hit_backface = dot(gn_auth, hit.ray_dir) > 0.0f;
    bool double_sided = (surf.flags & 2u) != 0u;
    bool two_sided_emit = (surf.flags & 4u) != 0u;

    RWStructuredBuffer<uint4> probe_rays_out = ResourceDescriptorHeap[atelier_safe_desc(g_aux2_uav)];
    // Closed 2-manifold back-face gate: interior rays hitting backfaces of closed single-sided solids contribute 0!
    if (hit_backface && !double_sided && !two_sided_emit && manifold_closed) {
        probe_rays_out[hit.probe_and_ray] = asuint(float4(0.0f, 0.0f, 0.0f, clamp(hit.hit_t, 1e-3f, 500.0f)));
        return;
    }

    ArchGi::ms_Context ctx;
    ctx.ms_uv = a0.uv * w0 + a1.uv * b1 + a2.uv * b2;
    ctx.ms_position_world = wp;
    ctx.ms_normal_world = nw;
    ctx.ms_geometric_normal_world = gn_auth;
    ctx.ms_tangent_world = float4(tw.xyz, tw.w < 0.0f ? -1.0f : 1.0f);

    ArchGi::ms_GiHit gi_mat = ArchGi::ms_shade_gi_hit(
        surf.input_base, ctx, ArchGi::ms_load_parameters(material_parameters, surf.parameter_base));

    float3 oriented_n = hit_backface ? -gn_auth : gn_auth;
    float3 radiance = (!hit_backface || two_sided_emit)
                          ? (atelier_finite3(gi_mat.ms_emissive) ? max(gi_mat.ms_emissive, 0.0f) : 0.0f)
                          : 0.0f;

    float3 albedo = (!hit_backface || double_sided) ? saturate(gi_mat.ms_effective_albedo) : 0.0f;
    float3 trans = saturate(gi_mat.ms_transmission);

    // Next-Event Estimation (NEE) over all directional lights + bounded local-light candidates (`direct_candidates`):
    uint nee_shadow_rays = 0u;
    if ((any(albedo > 0.0f) || any(trans > 0.0f)) && fcb.light_count > 0u && fcb.lights_srv != 0xFFFFFFFFu) {
        StructuredBuffer<GpuLightRecord> lights = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
        bool shadows_on = (fcb.flags & 1u) != 0u && fcb.tlas_srv != 0xFFFFFFFFu;
        uint max_cands = clamp(fcb.primary_sun_and_gi_cands >> 16u, 1u, 32u);

        // 1. Evaluate all Directional lights [0 .. fcb.dir_light_count):
        [loop] for (uint di = 0u; di < fcb.dir_light_count; ++di) {
            GpuLightRecord l = lights[di];
            float3 L = safe_unit_normal(-l.direction);
            float nl_front = max(0.0f, dot(oriented_n, L));
            float nl_back = max(0.0f, dot(-oriented_n, L));
            float3 contrib = (albedo * nl_front + trans * nl_back) * ATELIER_INV_PI * l.color_intensity;
            if (all(contrib <= 0.0f))
                continue;
            float vis = 1.0f;
            if (shadows_on && (l.kind_and_flags & 8u) != 0u) {
                RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
                float3 off_n = (nl_front > 0.0f) ? oriented_n : -oriented_n;
                RayDesc sray;
                sray.Origin = wp + off_n * 3e-3f;
                sray.Direction = L;
                sray.TMin = 1e-4f;
                sray.TMax = 1e4f;
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
                sq.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, sray);
                sq.Proceed();
                vis = (sq.CommittedStatus() == COMMITTED_NOTHING) ? 1.0f : 0.0f;
                ++nee_shadow_rays;
                if (vis > 0.0f && fcb.instance_count > 256u) {
                    float vsm_vis = sample_vsm_sun_visibility(l, wp + off_n * 0.02f, 0.02f, fcb);
                    if (vsm_vis >= 0.0f)
                        vis *= vsm_vis;
                }
            }
            if (vis > 0.0f)
                radiance += contrib * vis;
        }

        // 2. Evaluate all overlapping local lights (exact deterministic up to `max_cands`; WRS tail beyond `max_cands`):
        uint direct_done = 0u;
        uint tail_cnt = 0u;
        float tail_w_sum = 0.0f;
        float tail_w_pick = 1.0f;
        float3 tail_contrib = 0.0f;
        float3 tail_origin = wp;
        float3 tail_dir = float3(0.0f, 1.0f, 0.0f);
        float tail_tmax = 0.0f;
        bool tail_casts = false;
        uint wrs_t = ((fcb.flags & (1u << 7u)) != 0u) ? fcb.frame_index : 0u;
        uint rng_state = hit.probe_and_ray * 747796405u + wrs_t * 2891336453u + 277803737u;

        [loop] for (uint li = fcb.dir_light_count; li < fcb.light_count; ++li) {
            GpuLightRecord l = lights[li];
            uint kind = l.kind_and_flags & 3u;
            float ext = (kind == 1u) ? (0.5f * (l.width + l.height)) : l.source_radius;
            float3 to_l = l.position - wp;
            float d2 = dot(to_l, to_l);
            float max_d = l.range + ext;
            if (d2 >= max_d * max_d)
                continue;
            float d = sqrt(max(d2, 1e-6f));
            float3 L = to_l / d;

            float nl_front = max(0.0f, dot(oriented_n, L));
            float nl_back = max(0.0f, dot(-oriented_n, L));
            if (nl_front <= 0.0f && (nl_back <= 0.0f || all(trans <= 0.0f)))
                continue;

            float wt = saturate(1.0f - pow(max(d - ext, 0.0f) / max(l.range, 1e-3f), 4.0f));
            float3 li_rad = 0.0f;
            if (kind == 1u) {
                float cos_l = dot(-L, l.direction);
                cos_l = ((l.kind_and_flags & 4u) != 0u) ? abs(cos_l) : max(cos_l, 0.0f);
                li_rad = l.color_intensity * (cos_l * l.width * l.height * wt * wt / max(d2, 0.04f));
            } else {
                float spot_atten = 1.0f;
                if (kind == 3u) {
                    float spot_t = saturate((dot(-L, l.direction) - l.width) * l.height);
                    if (spot_t <= 0.0f)
                        continue;
                    spot_atten = spot_t * spot_t;
                }
                float cone_r = min(0.35f, 0.12f * hit.hit_t);
                float min_d2 = max(0.04f, max(l.source_radius * l.source_radius, cone_r * cone_r));
                li_rad = l.color_intensity * (spot_atten * wt * wt / max(d2, min_d2));
            }
            float3 contrib = (albedo * nl_front + trans * nl_back) * ATELIER_INV_PI * li_rad;
            float w = dot(contrib, float3(0.2126f, 0.7152f, 0.0722f));
            if (w <= 1e-7f)
                continue;

            float3 off_n = (nl_front > 0.0f) ? oriented_n : -oriented_n;
            float3 b_orig = wp + off_n * 3e-3f;
            float3 b_to_l = l.position - b_orig;
            float bd = length(b_to_l);
            float trim = ((kind == 0u || kind == 3u) && l.source_radius > 0.0f && bd > 1.5f * l.source_radius)
                             ? min(bd * 0.45f, max(3e-3f, l.source_radius * 0.90f))
                             : 3e-3f;
            float tmax = max(bd - trim, 0.0f);
            float3 sdir = (bd > 1e-5f) ? (b_to_l / bd) : L;
            bool casts = (l.kind_and_flags & 8u) != 0u;

            if (direct_done + 1u < max_cands) {
                bool vis = true;
                if (shadows_on && casts && tmax > 1e-4f) {
                    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
                    RayDesc sray;
                    sray.Origin = b_orig;
                    sray.Direction = sdir;
                    sray.TMin = 1e-4f;
                    sray.TMax = tmax;
                    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
                    sq.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, sray);
                    sq.Proceed();
                    vis = (sq.CommittedStatus() == COMMITTED_NOTHING);
                    ++nee_shadow_rays;
                }
                if (vis)
                    radiance += contrib;
                ++direct_done;
            } else {
                ++tail_cnt;
                tail_w_sum += w;
                rng_state = rng_state * 1664525u + 1013904223u;
                float u_r = float(rng_state >> 8u) * (1.0f / 16777216.0f);
                if (tail_cnt == 1u || u_r * tail_w_sum < w) {
                    tail_w_pick = w;
                    tail_contrib = contrib;
                    tail_origin = b_orig;
                    tail_dir = sdir;
                    tail_tmax = tmax;
                    tail_casts = casts;
                }
            }
        }

        if (tail_cnt > 0u) {
            bool vis = true;
            if (shadows_on && tail_casts && tail_tmax > 1e-4f) {
                RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
                RayDesc sray;
                sray.Origin = tail_origin;
                sray.Direction = tail_dir;
                sray.TMin = 1e-4f;
                sray.TMax = tail_tmax;
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
                sq.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, sray);
                sq.Proceed();
                vis = (sq.CommittedStatus() == COMMITTED_NOTHING);
                ++nee_shadow_rays;
            }
            if (vis) {
                float scale = (tail_cnt == 1u) ? 1.0f : (tail_w_sum / max(tail_w_pick, 1e-7f));
                radiance += tail_contrib * scale;
            }
        }
    }

    uint wave_nee_rays = WaveActiveSum(nee_shadow_rays);
    if (WaveIsFirstLane() && wave_nee_rays > 0u && g_viewport_packed1 != 0xFFFFFFFFu) {
        RWStructuredBuffer<uint> gi_counters = ResourceDescriptorHeap[atelier_safe_desc(g_viewport_packed1)];
        InterlockedAdd(gi_counters[3u], wave_nee_rays);
    }

    // Pre-expose direct hit radiance (`emissive + NEE`) before SHaRC & ScreenProbe accumulation:
    radiance = clamp(radiance * fcb.exposure_scale, 0.0f, 65504.0f);

    // ========================================================================
    // NVIDIA SHaRC v1.8.3 + AMD Capsaicin GI-1.2 Multi-Bounce Surface Cache
    //   Slot[0] = Total Outgoing Diffuse Radiance L_o(x) (queried at secondary x_2 & off-screen reflections)
    //   Slot[1] = Demodulated Multi-Bounce Indirect Irradiance E_ind(x)/pi (queried at primary x_1 -> zero self-feedback!)
    // ========================================================================
    uint temporal_idx = ((fcb.flags & (1u << 7u)) != 0u) ? fcb.frame_index : 0u;
    float2 q_jit = atelier_blue_jitter2(uint2(hit.probe_and_ray, hit.global_tri_id), temporal_idx, 0x3C6EF372u);

    float vsz_x1 = 0.20f;
    uint2 key_x1 = sharc_compute_key(wp, oriented_n, fcb.camera_eye, vsz_x1);

    // 1. Query previous frame's resolved multi-bounce indirect irradiance `Slot[1]` at x_1 (with tangential seam dither):
    float3 ind_irrad_x1 = 0.0f;
    bool has_hist = ((fcb.flags & (1u << 7u)) != 0u) && (g_viewport_packed0 != 0xFFFFFFFFu) &&
                    (fcb.wrc_hash_keys_uav != 0xFFFFFFFFu);
    RWStructuredBuffer<uint> hash_keys_uav = ResourceDescriptorHeap[atelier_safe_desc(fcb.wrc_hash_keys_uav)];
    RWStructuredBuffer<uint2> voxel_keys_uav = ResourceDescriptorHeap[atelier_safe_desc(g_aux4_uav)];
    if (has_hist && any(albedo > 0.0f)) {
        StructuredBuffer<uint4> wrc_prev = ResourceDescriptorHeap[atelier_safe_desc(g_viewport_packed0)];
        float3 j_wp = sharc_jitter_query_pos(wp, oriented_n, vsz_x1, hit.hit_t, q_jit);
        float vsz_j;
        uint2 j_key = sharc_compute_key(j_wp, oriented_n, fcb.camera_eye, vsz_j);
        uint q_slot = sharc_find_slot_uav(hash_keys_uav, j_key);
        if (q_slot == SHARC_INVALID_SLOT)
            q_slot = sharc_find_slot_uav(hash_keys_uav, key_x1);
        if (q_slot != SHARC_INVALID_SLOT) {
            float3 cached_ind;
            float s_ind;
            uint af_i, sf_i, fm_i;
            sharc_unpack_resolved(wrc_prev[q_slot * 2u + 1u], cached_ind, s_ind, af_i, sf_i, fm_i);
            if (s_ind > 0.0f)
                ind_irrad_x1 = cached_ind * fcb.exposure_ratio;
        }
    }

    float3 total_radiance = clamp(radiance + albedo * ind_irrad_x1, 0.0f, 1e4f);
    uint resolved_slot = SHARC_INVALID_SLOT;

    // 2. Insert/find x_1 in SHaRC hash table and accumulate `Slot[0]` (L_o(x_1)) + stratified 1/4 multi-bounce `Slot[1]`:
    if (fcb.wrc_hash_keys_uav != 0xFFFFFFFFu && g_aux4_uav != 0xFFFFFFFFu) {
        uint slot_x1 = sharc_find_or_insert_slot(hash_keys_uav, voxel_keys_uav, key_x1);
        if (slot_x1 != SHARC_INVALID_SLOT) {
            RWStructuredBuffer<uint4> wrc_accum = ResourceDescriptorHeap[atelier_safe_desc(g_aux3_uav)];
            // Avoid poisoning cache cells from sub-voxel corner grazes; bypass SHaRC readback within `< 1.25 * vsz_x1`
            // (`Capsaicin GI-1.2` `bypass_cache`) so thin walls/curtains never leak light across corners!
            if (hit.hit_t >= vsz_x1 * 0.25f) {
                uint3 q_lo = uint3(round(min(total_radiance, 32.0f) * SHARC_RADIANCE_SCALE));
                if (q_lo.x != 0u) InterlockedAdd(wrc_accum[slot_x1 * 2u + 0u].x, q_lo.x);
                if (q_lo.y != 0u) InterlockedAdd(wrc_accum[slot_x1 * 2u + 0u].y, q_lo.y);
                if (q_lo.z != 0u) InterlockedAdd(wrc_accum[slot_x1 * 2u + 0u].z, q_lo.z);
                InterlockedAdd(wrc_accum[slot_x1 * 2u + 0u].w, 1u);
                if (hit.hit_t >= vsz_x1 * 1.25f)
                    resolved_slot = slot_x1;
            }

            // Capsaicin GI-1.2 / SHaRC sparse secondary bounce (`1/4` stratified paths trace x_1 -> x_2):
            bool trace_mb = (((hit.probe_and_ray + temporal_idx) & 3u) == 0u) && (fcb.tlas_srv != 0xFFFFFFFFu);
            if (trace_mb) {
                float2 u_mb = atelier_blue_jitter2(uint2(hit.probe_and_ray, hit.instance_id), temporal_idx >> 2u, 0x5A2C019Du);
                float3 mb_dir = sharc_cosine_hemisphere_dir(u_mb, oriented_n);

                RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[atelier_safe_desc(fcb.tlas_srv)];
                RayDesc mb_ray;
                mb_ray.Origin = wp + oriented_n * 3e-3f;
                mb_ray.Direction = mb_dir;
                mb_ray.TMin = 2e-3f;
                mb_ray.TMax = 100.0f;
                RayQuery<RAY_FLAG_FORCE_OPAQUE> mq;
                mq.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE, 0xFFu, mb_ray);
                mq.Proceed();

                float3 mb_in = 0.0f;
                bool mb_valid = false;
                if (mq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                    uint inst2_id = mq.CommittedInstanceID();
                    StructuredBuffer<uint> part_bases = ResourceDescriptorHeap[atelier_safe_desc(fcb.part_tri_base_srv)];
                    StructuredBuffer<uint> slot_map = ResourceDescriptorHeap[atelier_safe_desc(fcb.material_slot_map_srv)];
                    GpuInstanceRecord inst2 = instances[inst2_id];
                    GpuMeshDescriptor mesh2 = meshes[inst2.mesh_pool_index];
                    uint gtri2 = part_bases[inst2.part_table_base + mq.CommittedGeometryIndex()] + mq.CommittedPrimitiveIndex();

                    StructuredBuffer<uint> bidx2 = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh2.blas_indices_srv))];
                    StructuredBuffer<GpuPackedPosition> pos2 = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh2.positions_srv))];
                    StructuredBuffer<GpuPackedAttributes> attr2 = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh2.attributes_srv))];
                    StructuredBuffer<uint> fm2 = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh2.face_meta_srv))];

                    uint fmeta2 = fm2[gtri2];
                    bool wf2 = (fmeta2 >> 31u) != 0u;
                    bool closed2 = (fmeta2 & (1u << 30u)) != 0u;
                    uint sidx2 = slot_map[inst2.material_slot_offset + (fmeta2 & 0x07FFFFFFu)];
                    uint sflags2 = surfaces[sidx2].flags;

                    uint vi0 = bidx2[gtri2 * 3u + 0u];
                    uint vi1 = bidx2[gtri2 * 3u + 1u];
                    uint vi2 = bidx2[gtri2 * 3u + 2u];
                    float3 p20 = pos2[vi0].pos, p21 = pos2[vi1].pos, p22 = pos2[vi2].pos;
                    float3 gn2 = reconstruct_geom_normal(p20, p21, p22, unpack_oct_snorm16(attr2[vi0].oct_normal), wf2, inst2);
                    bool bf2 = dot(gn2, mb_dir) > 0.0f;
                    if (bf2 && (sflags2 & 6u) == 0u && closed2) {
                        mb_valid = true;
                        mb_in = 0.0f;
                    } else {
                        float2 b_2 = mq.CommittedTriangleBarycentrics();
                        float w20 = 1.0f - b_2.x - b_2.y;
                        float3 wp2 = mul(float4(p20 * w20 + p21 * b_2.x + p22 * b_2.y, 1.0f), inst2.world).xyz;
                        float3 n2 = bf2 ? -gn2 : gn2;
                        if (has_hist) {
                            float vsz_x2;
                            uint2 key_x2 = sharc_compute_key(wp2, n2, fcb.camera_eye, vsz_x2);
                            uint slot_x2 = sharc_find_slot_uav(hash_keys_uav, key_x2);
                            if (slot_x2 == SHARC_INVALID_SLOT) {
                                uint lvl_coarse = min(sharc_get_level(wp2 + 1e-4f, fcb.camera_eye) + 1u, SHARC_LEVEL_MASK);
                                float vsz_c = sharc_get_voxel_size(lvl_coarse);
                                uint2 key_c = sharc_pack_grid_key(int3(floor((wp2 + 1e-4f) / vsz_c)), lvl_coarse, sharc_encode_normal_bits(n2));
                                slot_x2 = sharc_find_slot_uav(hash_keys_uav, key_c);
                            }
                            if (slot_x2 != SHARC_INVALID_SLOT) {
                                StructuredBuffer<uint4> wrc_prev = ResourceDescriptorHeap[atelier_safe_desc(g_viewport_packed0)];
                                float3 lo_x2;
                                float s_x2;
                                uint af2, sf2, fm_x2;
                                sharc_unpack_resolved(wrc_prev[slot_x2 * 2u + 0u], lo_x2, s_x2, af2, sf2, fm_x2);
                                if (s_x2 > 0.0f) {
                                    mb_in = lo_x2 * fcb.exposure_ratio;
                                    mb_valid = true;
                                }
                            }
                        }

                        // Off-screen / unseeded secondary surface NEE fallback (matches Capsaicin GI-1.2 `PopulateMultibounceCells`
                        // so sunlit upper atrium walls bounce daylight down even when off-screen and not yet in `wrc_prev`!):
                        uint mb_nee_rays = 0u;
                        if (!mb_valid && fcb.dir_light_count > 0u && fcb.lights_srv != 0xFFFFFFFFu) {
                            StructuredBuffer<GpuLightRecord> lights2 = ResourceDescriptorHeap[atelier_safe_desc(fcb.lights_srv)];
                            uint sun_i2 = min(fcb.primary_sun_and_gi_cands & 0xFFFFu, fcb.dir_light_count - 1u);
                            GpuLightRecord sun2 = lights2[sun_i2];
                            float3 L2 = safe_unit_normal(-sun2.direction);
                            float nl2 = max(0.0f, dot(n2, L2));
                            mb_valid = true;
                            if (nl2 > 0.0f) {
                                float vis2 = 1.0f;
                                if ((fcb.flags & 1u) != 0u && (sun2.kind_and_flags & 8u) != 0u) {
                                    float vsm2 = sample_vsm_sun_visibility(sun2, wp2 + n2 * 0.02f, 0.02f, fcb);
                                    if (vsm2 >= 0.0f) {
                                        vis2 = vsm2;
                                    } else {
                                        RayDesc sr2;
                                        sr2.Origin = wp2 + n2 * 4e-3f;
                                        sr2.Direction = L2;
                                        sr2.TMin = 1e-3f;
                                        sr2.TMax = 1e4f;
                                        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq2;
                                        sq2.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, sr2);
                                        sq2.Proceed();
                                        vis2 = (sq2.CommittedStatus() == COMMITTED_NOTHING) ? 1.0f : 0.0f;
                                        ++mb_nee_rays;
                                    }
                                }
                                if (vis2 > 0.0f)
                                    mb_in = (0.20f * ATELIER_INV_PI * nl2 * vis2) * sun2.color_intensity * fcb.exposure_scale;
                            }
                        }
                        uint wave_mb_nee = WaveActiveSum(mb_nee_rays);
                        if (WaveIsFirstLane() && wave_mb_nee > 0u && g_viewport_packed1 != 0xFFFFFFFFu) {
                            RWStructuredBuffer<uint> gi_counters = ResourceDescriptorHeap[atelier_safe_desc(g_viewport_packed1)];
                            InterlockedAdd(gi_counters[3u], wave_mb_nee);
                        }
                    }
                } else {
                    mb_in = eval_atelier_sky(mb_dir, 2.0f, fcb);
                    mb_valid = true;
                }

                if (mb_valid) {
                    uint3 q_mb = uint3(round(min(max(mb_in, 0.0f), 32.0f) * SHARC_RADIANCE_SCALE));
                    if (q_mb.x != 0u) InterlockedAdd(wrc_accum[slot_x1 * 2u + 1u].x, q_mb.x);
                    if (q_mb.y != 0u) InterlockedAdd(wrc_accum[slot_x1 * 2u + 1u].y, q_mb.y);
                    if (q_mb.z != 0u) InterlockedAdd(wrc_accum[slot_x1 * 2u + 1u].z, q_mb.z);
                    InterlockedAdd(wrc_accum[slot_x1 * 2u + 1u].w, 1u);
                }
            }
        }
    }

    float3 safe_rad = clamp(total_radiance, 0.0f, 32.0f);
    float safe_t = clamp(hit.hit_t, 1e-3f, 500.0f);
    if (resolved_slot != SHARC_INVALID_SLOT) {
        // Negative `.w = -safe_t` tags a SHaRC-cached hit for `Mode 7` (`ResolveCells`) after Async Compute `LitGiWrcResolve`,
        // stored as `uint4` so integer `resolved_slot` and `fp16x2` bits are immune to hardware FP32 subnormal FTZ/DAZ!
        probe_rays_out[hit.probe_and_ray] = uint4(resolved_slot,
                                                  sharc_pack_fp16x2(safe_rad.r, safe_rad.g),
                                                  sharc_pack_fp16x2(safe_rad.b, 0.0f),
                                                  asuint(-safe_t));
    } else {
        probe_rays_out[hit.probe_and_ray] = asuint(float4(safe_rad, safe_t));
    }
}
