// Per-`.source` Archetype MaterialShade Bin CS (invoked via CmdDispatchIndirect, 1 PSO per active archetype)
// Evaluates Arch::ms_shade_footprint using precomputed analytical barycentric derivatives from VisUnpack,
// applies Kaplanyan 2016 Geometric Specular AA + BC5 Toksvig mip compensation, and writes:
//   - OpenPBR 3x8B Canonical Slab (`Slab0_SpecNormal`, `Slab1_AlbedoClosure`, and `TileEmissiveMask`-gated `Slab2_Emissive`), OR
//   - Studio mode (`MatCap / Smooth / Flat` + optional `Cavity` view-space normal + optional `Wireframe::overlay` / selected edges)!

#ifndef ARCH_STUDIO_MODE
#define ARCH_STUDIO_MODE 0
#endif

float shade_edge_dist(float val, float dx, float dy) {
    if (!atelier_finite_f(dx) || !atelier_finite_f(dy))
        return 1e8f;
    float scale = max(abs(dx), abs(dy));
    if (scale == 0.0f) return 1e8f;
    float2 grad = float2(dx, dy) / scale;
    return abs(val / scale) * rsqrt(dot(grad, grad));
}

float3 shade_normal_delta(float3 value, float3 delta) {
    float3 unavail = (float3)asfloat(0x7fc00000u);
    if (!atelier_finite3(value) || !atelier_finite3(delta))
        return unavail;
    float scale = max(abs(value.x), max(abs(value.y), abs(value.z)));
    if (scale == 0.0f)
        return any(delta != 0.0f) ? unavail : 0.0f;
    precise float3 v = value / scale, d = delta / scale;
    precise float mag = length(v);
    precise float3 n = v / mag;
    return (d - n * dot(n, d)) / mag;
}

uint select_openpbr_profile(Arch::ms_Output o, out float lobe_w, out float p0, out float p1, out float3 lobe_col) {
    lobe_w = 0.0f;
    p0 = 0.0f;
    p1 = 1.0f;
    lobe_col = atelier_finite3(o.ms_specular_color) ? saturate(o.ms_specular_color) : 1.0f;

    uint best_id = SLAB_PROFILE_STANDARD_F82;
    float best_w = 1e-3f;

    float w_coat = atelier_finite_f(o.ms_coat_weight) ? saturate(o.ms_coat_weight) : 0.0f;
    if (w_coat > best_w) {
        best_w = w_coat;
        best_id = SLAB_PROFILE_COAT;
        lobe_w = w_coat;
        p0 = atelier_finite_f(o.ms_coat_roughness) ? clamp(o.ms_coat_roughness, 0.02f, 1.0f) : 0.1f;
        p1 = atelier_finite_f(o.ms_coat_darkening) ? saturate(o.ms_coat_darkening) : 1.0f;
        lobe_col = 1.0f;
    }

    float w_tf = atelier_finite_f(o.ms_thin_film_weight) ? saturate(o.ms_thin_film_weight) : 0.0f;
    if (w_tf > best_w) {
        best_w = w_tf;
        best_id = SLAB_PROFILE_IRIDESCENT;
        lobe_w = w_tf;
        float tf_t = atelier_finite_f(o.ms_thin_film_thickness) ? saturate(o.ms_thin_film_thickness) : 0.5f;
        float tf_ior = atelier_finite_f(o.ms_thin_film_ior) ? clamp(o.ms_thin_film_ior, 1.0f, 2.5f) : 1.5f;
        p0 = 100.0f + tf_t * 900.0f;
        p1 = tf_ior;
        lobe_col = 1.0f;
    }

    float w_fol = atelier_finite_f(o.ms_transmission_scatter) ? saturate(o.ms_transmission_scatter) : 0.0f;
    if (w_fol > best_w) {
        best_w = w_fol;
        best_id = SLAB_PROFILE_FOLIAGE;
        lobe_w = w_fol;
        p0 = 0.5f;
        p1 = 1.0f;
        lobe_col = atelier_finite3(o.ms_transmission_color) ? saturate(o.ms_transmission_color) : 1.0f;
    }

    float w_fuzz = atelier_finite_f(o.ms_fuzz_weight) ? saturate(o.ms_fuzz_weight) : 0.0f;
    if (w_fuzz > best_w) {
        best_w = w_fuzz;
        best_id = SLAB_PROFILE_FUZZ;
        lobe_w = w_fuzz;
        p0 = atelier_finite_f(o.ms_fuzz_roughness) ? clamp(o.ms_fuzz_roughness, 0.05f, 1.0f) : 0.5f;
        p1 = 1.0f;
        lobe_col = atelier_finite3(o.ms_fuzz_color) ? saturate(o.ms_fuzz_color) : 1.0f;
    }

    float w_sss = atelier_finite_f(o.ms_subsurface_weight) ? saturate(o.ms_subsurface_weight) : 0.0f;
    if (w_sss > best_w) {
        best_w = w_sss;
        best_id = SLAB_PROFILE_SUBSURFACE;
        lobe_w = w_sss;
        float sss_r = atelier_finite_f(o.ms_subsurface_radius) ? o.ms_subsurface_radius : 0.01f;
        p0 = saturate(sss_r / 0.05f);
        p1 = 1.0f;
        lobe_col = atelier_finite3(o.ms_subsurface_color) ? saturate(o.ms_subsurface_color) : 1.0f;
    }

    return best_id;
}

[numthreads(64, 1, 1)]
void main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint tid = (gid.y * 1024u + gid.x) * 64u + gtid;

    StructuredBuffer<uint> counts = ResourceDescriptorHeap[atelier_safe_desc(g_aux0_srv)];
    StructuredBuffer<uint> offsets = ResourceDescriptorHeap[atelier_safe_desc(g_bin_offset_or_cluster_srv)];
    uint bin_count = counts[g_archetype_index];
    if (tid >= bin_count)
        return;

    StructuredBuffer<uint2> bin_pixels = ResourceDescriptorHeap[atelier_safe_desc(g_aux1_srv)];
    uint2 px_inst = bin_pixels[offsets[g_archetype_index] + tid];
    uint2 px = uint2(px_inst.x & 0xFFFFu, px_inst.x >> 16u);
    uint instance_id = px_inst.y;

    StructuredBuffer<GlobalFrameConstants> fcb_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_srv)];
    GlobalFrameConstants fcb = fcb_buf[0];

    StructuredBuffer<GpuBaryDerivatives> bary_buf = ResourceDescriptorHeap[atelier_safe_desc(g_frame_cb_idx)];
    GpuBaryDerivatives bd = bary_buf[px.y * fcb.render_extent.x + px.x];

    StructuredBuffer<GpuInstanceRecord> instances = ResourceDescriptorHeap[atelier_safe_desc(fcb.instances_srv)];
    StructuredBuffer<GpuMeshDescriptor> meshes = ResourceDescriptorHeap[atelier_safe_desc(fcb.mesh_table_srv)];
    GpuInstanceRecord inst = instances[instance_id];
    GpuMeshDescriptor mesh = meshes[inst.mesh_pool_index];

    StructuredBuffer<uint> blas_idx = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.blas_indices_srv))];
    uint gi0 = blas_idx[bd.global_tri_id * 3u + 0u];
    uint gi1 = blas_idx[bd.global_tri_id * 3u + 1u];
    uint gi2 = blas_idx[bd.global_tri_id * 3u + 2u];

    StructuredBuffer<GpuPackedPosition> pos_stream = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.positions_srv))];
    StructuredBuffer<GpuPackedAttributes> attr_stream = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(mesh.attributes_srv))];

    float3 lp0 = pos_stream[gi0].pos;
    float3 lp1 = pos_stream[gi1].pos;
    float3 lp2 = pos_stream[gi2].pos;
    GpuPackedAttributes a0 = attr_stream[gi0];
    GpuPackedAttributes a1 = attr_stream[gi1];
    GpuPackedAttributes a2 = attr_stream[gi2];

    float3 wp0 = mul(float4(lp0, 1.0f), inst.world).xyz;
    float3 wp1 = mul(float4(lp1, 1.0f), inst.world).xyz;
    float3 wp2 = mul(float4(lp2, 1.0f), inst.world).xyz;

    float3 local_n0 = unpack_oct_snorm16(a0.oct_normal);
    float3 n0 = transform_local_normal(local_n0, inst);
    float3 n1 = transform_local_normal(unpack_oct_snorm16(a1.oct_normal), inst);
    float3 n2 = transform_local_normal(unpack_oct_snorm16(a2.oct_normal), inst);

    float4 t0 = transform_local_tangent(unpack_oct_tangent_sign(a0.oct_tangent_sign), inst);
    float4 t1 = transform_local_tangent(unpack_oct_tangent_sign(a1.oct_tangent_sign), inst);
    float4 t2 = transform_local_tangent(unpack_oct_tangent_sign(a2.oct_tangent_sign), inst);

    uint surf_idx = bd.surface_and_flags & 0x07FFFFFFu;
    bool winding_flipped = (bd.surface_and_flags & (1u << 27u)) != 0u;
    bool backfacing = (bd.surface_and_flags & (1u << 28u)) != 0u;
    uint edge_mask = (bd.surface_and_flags >> 29u) & 7u;

    float3 gn_unflipped = reconstruct_geom_normal(lp0, lp1, lp2, local_n0, winding_flipped, inst);

    float2 bary = bd.bary;
    float w0 = 1.0f - bary.x - bary.y;
    float3 interp_n_raw = n0 * w0 + n1 * bary.x + n2 * bary.y;
    float4 interp_t = t0 * w0 + t1 * bary.x + t2 * bary.y;

    Arch::ms_Context c_in;
    c_in.ms_uv = a0.uv * w0 + a1.uv * bary.x + a2.uv * bary.y;
    c_in.ms_position_world = wp0 + (wp1 - wp0) * bary.x + (wp2 - wp0) * bary.y;
    c_in.ms_normal_world = safe_unit_normal(interp_n_raw);
    c_in.ms_geometric_normal_world = gn_unflipped;
    c_in.ms_tangent_world = float4(interp_t.xyz, interp_t.w < 0.0f ? -1.0f : 1.0f);

    // Negative Mip-LOD bias (`-0.65` mip -> `0.63728x` UV gradient) when 8-phase TAA supersampling is active:
    float uv_mip_scale = ((fcb.flags & (1u << 11u)) != 0u) ? 0.63728f : 1.0f;

    Arch::ms_Context c_dx = (Arch::ms_Context)0;
    c_dx.ms_uv = ((a1.uv - a0.uv) * bd.db_dx.x + (a2.uv - a0.uv) * bd.db_dx.y) * uv_mip_scale;
    c_dx.ms_position_world = (wp1 - wp0) * bd.db_dx.x + (wp2 - wp0) * bd.db_dx.y;
    c_dx.ms_normal_world = shade_normal_delta(interp_n_raw, (n1 - n0) * bd.db_dx.x + (n2 - n0) * bd.db_dx.y);
    c_dx.ms_tangent_world = float4((t1.xyz - t0.xyz) * bd.db_dx.x + (t2.xyz - t0.xyz) * bd.db_dx.y, 0.0f);

    Arch::ms_Context c_dy = (Arch::ms_Context)0;
    c_dy.ms_uv = ((a1.uv - a0.uv) * bd.db_dy.x + (a2.uv - a0.uv) * bd.db_dy.y) * uv_mip_scale;
    c_dy.ms_position_world = (wp1 - wp0) * bd.db_dy.x + (wp2 - wp0) * bd.db_dy.y;
    c_dy.ms_normal_world = shade_normal_delta(interp_n_raw, (n1 - n0) * bd.db_dy.x + (n2 - n0) * bd.db_dy.y);
    c_dy.ms_tangent_world = float4((t1.xyz - t0.xyz) * bd.db_dy.x + (t2.xyz - t0.xyz) * bd.db_dy.y, 0.0f);

    StructuredBuffer<GpuSurfaceRecord> surfaces = ResourceDescriptorHeap[atelier_safe_desc(g_material_surfaces_srv)];
    GpuSurfaceRecord surf = surfaces[surf_idx];

#if ARCH_STUDIO_MODE
    // Studio shading: skip full `.source` texture/graph evaluation in `MatCap` (`shading_mode == 2`)!
    float3 shade_n;
    float3 base_albedo = 0.8f;
    if (fcb.shading_mode == 2u) {
        shade_n = c_in.ms_normal_world;
    } else {
        ByteAddressBuffer material_parameters = ResourceDescriptorHeap[atelier_safe_desc(g_material_params_srv)];
        g_accum_toksvig_var = 0.0f;
        Arch::ms_Output mat_out = Arch::ms_shade_footprint(
            surf.input_base, c_in, c_dx, c_dy, Arch::ms_load_parameters(material_parameters, surf.parameter_base));
        bool valid_sn = atelier_finite3(mat_out.ms_normal_world) &&
                        dot(mat_out.ms_normal_world, mat_out.ms_normal_world) > 1e-20f;
        shade_n = (fcb.shading_mode == 1u) ? gn_unflipped : (valid_sn ? safe_unit_normal(mat_out.ms_normal_world) : c_in.ms_normal_world);
        base_albedo = atelier_finite3(mat_out.ms_base_color) ? saturate(mat_out.ms_base_color) : 0.0f;
    }

    bool tint_bf = (fcb.flags & (1u << 9u)) != 0u;
    if (((surf.flags & 2u) != 0u || tint_bf) && backfacing)
        shade_n = -shade_n;

    float3 eye_n = safe_unit_normal(mul((float3x3)fcb.inv_view, shade_n));
    float3 headlight_dir = normalize(float3(0.4f, 0.8f, -0.6f));

    float3 color = 0.0f;
    if (fcb.shading_mode == 2u) {
        Texture2D<float4> matcap = ResourceDescriptorHeap[atelier_safe_desc(fcb.matcap_srv)];
        SamplerState smp = SamplerDescriptorHeap[0];
        color = matcap.SampleLevel(smp, eye_n.xy * float2(0.5f, -0.5f) + 0.5f, 0.0f).rgb;
    } else {
        float key = 0.3f + 0.7f * saturate(dot(eye_n, headlight_dir));
        color = base_albedo * key;
    }
    if (tint_bf && backfacing) {
        float bf_l = 0.34f + 0.66f * saturate(dot(eye_n, headlight_dir));
        color = lerp(color, float3(0.06f, 0.74f, 0.90f) * bf_l, 0.84f);
    }

    StructuredBuffer<uint> sel_mask = ResourceDescriptorHeap[atelier_safe_desc(fcb.selected_mask_srv)];
    bool is_selected = ((sel_mask[instance_id >> 5u] >> (instance_id & 31u)) & 1u) != 0u;

    if ((g_aux4_uav & 1u) == 0u && (fcb.wireframe_mode == 1u || is_selected)) {
        float3 b3 = float3(w0, bary.x, bary.y);
        float3 dbx3 = float3(-bd.db_dx.x - bd.db_dx.y, bd.db_dx.x, bd.db_dx.y);
        float3 dby3 = float3(-bd.db_dy.x - bd.db_dy.y, bd.db_dy.x, bd.db_dy.y);
        float3 dist = float3((edge_mask & 1u) != 0u ? shade_edge_dist(b3.x, dbx3.x, dby3.x) : 1e8f,
                             (edge_mask & 2u) != 0u ? shade_edge_dist(b3.y, dbx3.y, dby3.y) : 1e8f,
                             (edge_mask & 4u) != 0u ? shade_edge_dist(b3.z, dbx3.z, dby3.z) : 1e8f);
        float width = is_selected ? max(fcb.wire_style.x, 2.0f) : fcb.wire_style.x;
        float edge = 1.0f - smoothstep(max(0.0f, width * 0.5f - 0.5f), width * 0.5f + 0.5f, min(dist.x, min(dist.y, dist.z)));
        float3 ink = is_selected ? float3(1.0f, 0.69f, 0.0f) : float3(0.025f, 0.032f, 0.04f);
        float alpha = edge * (is_selected ? 1.0f : fcb.wire_style.y);
        color = lerp(color, ink, alpha);
    }

    RWTexture2D<float4> out_color = ResourceDescriptorHeap[atelier_safe_desc(g_aux2_uav)];
    out_color[px] = float4(color, 1.0f);

    if ((g_aux4_uav & 1u) != 0u && g_aux3_uav != 0xFFFFFFFFu) {
        // Cavity enabled: write view-space author normal into RGBA16F UAV
        float3 cav_src = (fcb.shading_mode == 1u) ? gn_unflipped : c_in.ms_normal_world;
        float3 cav_n = safe_unit_normal(mul((float3x3)fcb.inv_view, cav_src));
        if (backfacing) cav_n = -cav_n;
        RWTexture2D<float4> out_cav_n = ResourceDescriptorHeap[atelier_safe_desc(g_aux3_uav)];
        out_cav_n[px] = float4(cav_n, 1.0f);
    }
#else
    // Scene Lit: Pack OpenPBR v1.1 24B/px Canonical Slab (3 x 8B)
    ByteAddressBuffer material_parameters = ResourceDescriptorHeap[atelier_safe_desc(g_material_params_srv)];
    g_accum_toksvig_var = 0.0f;
    Arch::ms_Output mat_out = Arch::ms_shade_footprint(
        surf.input_base, c_in, c_dx, c_dy, Arch::ms_load_parameters(material_parameters, surf.parameter_base));

    bool valid_sn = atelier_finite3(mat_out.ms_normal_world) &&
                    dot(mat_out.ms_normal_world, mat_out.ms_normal_world) > 1e-20f;
    float3 shade_n = valid_sn ? safe_unit_normal(mat_out.ms_normal_world) : c_in.ms_normal_world;
    if ((surf.flags & 2u) != 0u && backfacing)
        shade_n = -shade_n;
    float3 gn_oriented = backfacing ? -gn_unflipped : gn_unflipped;
    if (!backfacing || (surf.flags & 2u) != 0u) {
        float ndg = dot(shade_n, gn_oriented);
        if (ndg < 0.02f)
            shade_n = safe_unit_normal(shade_n + gn_oriented * (0.02f - ndg));
    }

    // Kaplanyan 2016 Geometric Specular AA + BC5 Toksvig normal variance compensation:
    float dx_n2 = atelier_finite3(c_dx.ms_normal_world) ? dot(c_dx.ms_normal_world, c_dx.ms_normal_world) : 0.0f;
    float dy_n2 = atelier_finite3(c_dy.ms_normal_world) ? dot(c_dy.ms_normal_world, c_dy.ms_normal_world) : 0.0f;
    float geom_var = min(dx_n2 + dy_n2, 0.18f);
    float base_r = atelier_finite_f(mat_out.ms_roughness) ? saturate(mat_out.ms_roughness) : 0.5f;
    float filtered_r = sqrt(saturate(base_r * base_r + geom_var * 0.25f + g_accum_toksvig_var * 0.25f));

    float ior = atelier_finite_f(mat_out.ms_specular_ior) ? clamp(mat_out.ms_specular_ior, 1.0f, 3.0f) : 1.5f;
    float spec_w = atelier_finite_f(mat_out.ms_specular_weight) ? saturate(mat_out.ms_specular_weight) : 1.0f;
    float f0_ior = ((ior - 1.0f) / (ior + 1.0f));
    f0_ior = f0_ior * f0_ior * spec_w;

    OpenPbrSurface pbr = (OpenPbrSurface)0;
    pbr.shading_normal = shade_n;
    pbr.roughness = filtered_r;
    pbr.metallic = atelier_finite_f(mat_out.ms_metallic) ? saturate(mat_out.ms_metallic) : 0.0f;
    pbr.spec_f0_dielectric = clamp(f0_ior, 0.0f, 0.25f);
    pbr.micro_ao = atelier_finite_f(mat_out.ms_micro_ao) ? saturate(mat_out.ms_micro_ao) : 1.0f;
    pbr.base_color = atelier_finite3(mat_out.ms_base_color) ? saturate(mat_out.ms_base_color) : 0.0f;
    pbr.diffuse_roughness = atelier_finite_f(mat_out.ms_diffuse_roughness) ? saturate(mat_out.ms_diffuse_roughness) : 0.0f;
    pbr.profile_id = select_openpbr_profile(mat_out, pbr.lobe_weight, pbr.lobe_param0, pbr.lobe_param1, pbr.f82_or_lobe_color);
    pbr.flags = 1u | (surf.flags & 6u);

    uint2 s0, s1;
    pack_openpbr_slab(pbr, s0, s1);

    RWTexture2D<uint2> slab0_uav = ResourceDescriptorHeap[atelier_safe_desc(g_aux2_uav)];
    RWTexture2D<uint2> slab1_uav = ResourceDescriptorHeap[atelier_safe_desc(g_aux3_uav)];
    slab0_uav[px] = s0;
    slab1_uav[px] = s1;

    bool active_sss = (pbr.profile_id == SLAB_PROFILE_SUBSURFACE && pbr.lobe_weight > 1e-3f);
    if (WaveActiveAnyTrue(active_sss) && WaveIsFirstLane() && g_viewport_packed0 != 0xFFFFFFFFu) {
        RWByteAddressBuffer sss_args = ResourceDescriptorHeap[atelier_safe_desc(g_viewport_packed0)];
        sss_args.Store3(0u, uint3((fcb.render_extent.x + 15u) / 16u, (fcb.render_extent.y + 15u) / 16u, 1u));
    }

    bool allow_emit = !backfacing || ((surf.flags & 4u) != 0u);
    float3 raw_emit = atelier_finite3(mat_out.ms_emissive) ? mat_out.ms_emissive : 0.0f;
    float3 emit_rgb = allow_emit ? clamp(raw_emit * fcb.exposure_scale, 0.0f, 65504.0f) : 0.0f;
    if (any(emit_rgb >= 6e-8f)) {
        RWTexture2D<float4> slab2_uav = ResourceDescriptorHeap[atelier_safe_desc(g_aux4_uav)];
        slab2_uav[px] = float4(emit_rgb, 1.0f);
        uint tiles_x = (fcb.render_extent.x + 7u) / 8u;
        uint tile_idx = (px.y / 8u) * tiles_x + (px.x / 8u);
        RWStructuredBuffer<uint> tile_em_mask = ResourceDescriptorHeap[atelier_safe_desc(g_aux5_uav)];
        InterlockedOr(tile_em_mask[tile_idx >> 5u], 1u << (tile_idx & 31u));
    }
#endif
}
