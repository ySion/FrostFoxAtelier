struct UiInstance {
    float4 bounds;
    float4 uv_bounds;
    float4 tint;
    float4 border_color;
    float4 radius; // top-left, top-right, bottom-right, bottom-left
    float4 image_background;
    float border_width;
    uint shading; // ui_solid..ui_arc_sector (low 3 bits); textured kinds keep their ui_textures slot from bit 3.
    uint2 clip; // Half-open pixels: left | top << 16, right | bottom << 16.
};
static const uint ui_solid = 0, ui_coverage = 1, ui_image = 2, ui_capsule = 3, ui_node_shape = 4, ui_arc_sector = 5;
StructuredBuffer<UiInstance> ui_instances;
float2 target_size;
uint first_instance;
struct UiVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint instance : TEXCOORD1;
};
float4 fit_radius(float4 r, float2 size) {
    float factor = 1;
    if (r.x + r.y > 0) factor = min(factor, size.x / (r.x + r.y));
    if (r.w + r.z > 0) factor = min(factor, size.x / (r.w + r.z));
    if (r.x + r.w > 0) factor = min(factor, size.y / (r.x + r.w));
    if (r.y + r.z > 0) factor = min(factor, size.y / (r.y + r.z));
    return r * factor;
}
float sd_rounded_box_fitted(float2 p, float2 size, float4 fr) {
    float d = max(max(-p.x, -p.y), max(p.x - size.x, p.y - size.y));
    if (fr.x > 0 && p.x < fr.x && p.y < fr.x)
        d = max(d, length(p - float2(fr.x, fr.x)) - fr.x);
    if (fr.y > 0 && p.x > size.x - fr.y && p.y < fr.y)
        d = max(d, length(p - float2(size.x - fr.y, fr.y)) - fr.y);
    if (fr.z > 0 && p.x > size.x - fr.z && p.y > size.y - fr.z)
        d = max(d, length(p - (size - fr.z)) - fr.z);
    if (fr.w > 0 && p.x < fr.w && p.y > size.y - fr.w)
        d = max(d, length(p - float2(fr.w, size.y - fr.w)) - fr.w);
    return d;
}
float rounded_coverage(float2 p, float2 size, float4 r) {
    if (any(size <= 0)) return 0;
    return saturate(.5 - sd_rounded_box_fitted(p, size, r));
}
float4 shade_box(UiInstance item, float2 pixel, float4 fill) {
    float2 local = pixel - item.bounds.xy;
    float outer = all(item.radius == 0) ? 1 : rounded_coverage(local, item.bounds.zw, item.radius);
    float stroke = 0;
    if (item.border_width > 0) {
        float2 inner_size = max(0, item.bounds.zw - 2 * item.border_width);
        float4 inner_radius = fit_radius(max(0, item.radius - item.border_width), inner_size);
        stroke = max(0, outer - rounded_coverage(local - item.border_width, inner_size, inner_radius));
    }
    return fill * (outer - stroke * item.border_color.a)
        + float4(item.border_color.rgb * item.border_color.a, item.border_color.a) * stroke;
}

float sd_rounded_box(float2 p, float2 size, float4 r) {
    return sd_rounded_box_fitted(p, size, fit_radius(r, size));
}

// Exact 2D signed distance to the 8 SOP node silhouettes in local [0, size.x] x [0, size.y].
float sd_node_silhouette(float2 p, float2 size, uint shape) {
    float2 half_s = size * 0.5;
    float2 q = p - half_s;
    if (shape == 0 || shape == 7) {
        float r = min(half_s.x, half_s.y);
        return sd_rounded_box(p, size, float4(r, r, r, r));
    }
    if (shape == 1) {
        float r = min(size.y * 0.17, min(half_s.x, half_s.y));
        return sd_rounded_box(p, size, float4(r, r, r, r));
    }
    if (shape == 2) {
        float rt = min(size.y * 0.52, half_s.x);
        float rb = min(size.y * 0.13, half_s.x);
        return sd_rounded_box(p, size, float4(rt, rt, rb, rb));
    }
    if (shape == 3) {
        // Trapezoid: wide top (r2), tapered bottom (r1).
        float cr = min(size.y * 0.06, 2.5);
        float hh = max(1.0, half_s.y - cr);
        float r2 = max(1.0, half_s.x - cr);
        float inset = min(size.y * 0.34, half_s.x * 0.38);
        float r1 = max(1.0, r2 - inset);
        float2 p2 = float2(abs(q.x), -q.y);
        float2 k1 = float2(r2, hh);
        float2 k2 = float2(r2 - r1, 2.0 * hh);
        float2 ca = float2(p2.x - min(p2.x, p2.y < 0.0 ? r1 : r2), abs(p2.y) - hh);
        float2 cb = p2 - k1 + k2 * saturate(dot(k1 - p2, k2) / max(1e-5, dot(k2, k2)));
        float s = (cb.x < 0.0 && ca.y < 0.0) ? -1.0 : 1.0;
        return s * sqrt(min(dot(ca, ca), dot(cb, cb))) - cr;
    }
    if (shape == 4 || shape == 6) {
        // Pointed Hexagon (4) & Milestone Diamond (6).
        float cr = min(size.y * 0.04, 1.5);
        float hh = max(1.0, half_s.y - cr);
        float hw = max(1.0, half_s.x - cr);
        float inset = shape == 6 ? min(size.y * 0.66, hw * 0.58) : min(size.y * 0.42, hw * 0.42);
        float2 a = abs(q);
        float2 vA = float2(hw - inset, hh);
        float2 vB = float2(hw, 0.0);
        float2 e = vB - vA;
        float2 ca = float2(a.x - min(a.x, vA.x), a.y - hh);
        float2 cb = a - vA - e * saturate(dot(a - vA, e) / max(1e-5, dot(e, e)));
        float s = (a.y < hh && (a.x - hw) * hh + a.y * inset < 0.0) ? -1.0 : 1.0;
        return s * sqrt(min(dot(ca, ca), dot(cb, cb))) - cr;
    }
    // shape == 5: Notched Swallowtail Ribbon (>====<).
    float hh = max(1.0, half_s.y);
    float hw = max(1.0, half_s.x);
    float inset = min(size.y * 0.36, hw * 0.36);
    float2 a = abs(q);
    float2 vB = float2(hw - inset, 0.0);
    float2 vA = float2(hw, hh);
    float2 e = vA - vB;
    float2 ca = float2(a.x - min(a.x, hw), a.y - hh);
    float2 cb = a - vB - e * saturate(dot(a - vB, e) / max(1e-5, dot(e, e)));
    float s = (a.y < hh && (a.x - vB.x) * hh - a.y * inset < 0.0) ? -1.0 : 1.0;
    return s * sqrt(min(dot(ca, ca), dot(cb, cb)));
}

// Signed perpendicular distance from p to the contour-matched internal divider at `offset` from left/right side.
// Negative = closer to the node's outer edge than the divider; Positive = towards the node's center.
float sd_node_divider(float2 p, float2 size, uint shape, bool right_side, float offset) {
    float ty = saturate(p.y / max(1.0, size.y));
    if (shape == 7) {
        float r = min(size.x, size.y) * 0.5;
        float2 q = p - size * 0.5;
        float cx = right_side ? (size.x * 0.5 - r - offset) : -(size.x * 0.5 - r - offset);
        return r - length(q - float2(cx, 0.0));
    }
    if (shape == 0 || shape == 1) {
        // Classic Houdini subtle forward-slash '/' slant across both left and right flag strips.
        float slant = size.y * 0.16;
        float inv_n = rsqrt(1.0 + 0.16 * 0.16);
        if (!right_side) {
            float x_div = offset + (0.5 - ty) * slant;
            return (p.x - x_div) * inv_n;
        } else {
            float x_div = (size.x - offset) + (0.5 - ty) * slant;
            return (x_div - p.x) * inv_n;
        }
    }
    float x_left = 0.0;
    float slope = 0.0;
    if (shape == 2) {
        float flare = size.y * 0.15;
        x_left = (1.0 - ty) * flare;
        slope = flare / max(1.0, size.y);
    } else if (shape == 3) {
        float inset = min(size.y * 0.34, size.x * 0.19);
        x_left = ty * inset;
        slope = inset / max(1.0, size.y);
    } else if (shape == 4 || shape == 6) {
        float inset = shape == 6 ? min(size.y * 0.66, size.x * 0.29) : min(size.y * 0.42, size.x * 0.21);
        x_left = abs(ty - 0.5) * 2.0 * inset;
        slope = (2.0 * inset) / max(1.0, size.y);
    } else {
        float inset = min(size.y * 0.36, size.x * 0.18);
        x_left = (1.0 - abs(ty - 0.5) * 2.0) * inset;
        slope = (2.0 * inset) / max(1.0, size.y);
    }
    float inv_n = rsqrt(1.0 + slope * slope);
    if (!right_side)
        return (p.x - (x_left + offset)) * inv_n;
    return ((size.x - x_left - offset) - p.x) * inv_n;
}

float4 shade_node_shape(UiInstance item, float2 pixel, float4 fill) {
    float2 size = item.bounds.zw;
    if (any(size <= 0)) return 0;
    float2 local = pixel - item.bounds.xy;
    uint shape = (uint)round(item.uv_bounds.x);
    uint slot = (uint)round(item.uv_bounds.y);
    float s0 = item.uv_bounds.z;
    float s1 = item.uv_bounds.w;
    float d_body = sd_node_silhouette(local, size, shape);
    if (slot == 0) {
        float outer = saturate(0.5 - d_body);
        float stroke = 0;
        if (item.border_width > 0)
            stroke = max(0.0, outer - saturate(0.5 - (d_body + item.border_width)));
        return fill * (outer - stroke * item.border_color.a)
            + float4(item.border_color.rgb * item.border_color.a, item.border_color.a) * stroke;
    }
    bool right_side = (slot == 2 || slot == 4);
    if (slot == 1 || slot == 2) {
        // Contour-parallel flag fill slice [s0, s1], inset inside the node border.
        float d_in_body = d_body + item.border_width;
        float d_a = s0 > 0.0 ? -sd_node_divider(local, size, shape, right_side, s0) : -1e5;
        float d_b = sd_node_divider(local, size, shape, right_side, s1);
        float d = max(d_in_body, max(d_a, d_b));
        return fill * saturate(0.5 - d);
    }
    // slot == 3 or 4: crisp 1px contour-parallel divider seam clipped inside the node body.
    float d_seam = abs(sd_node_divider(local, size, shape, right_side, s0)) - item.border_width * 0.5;
    float cov = saturate(0.5 - max(d_seam, d_body + 1.0));
    float4 seam = item.border_color.a > 0
        ? float4(item.border_color.rgb * item.border_color.a, item.border_color.a)
        : fill;
    return seam * cov;
}

float4 shade_arc_sector(UiInstance item, float2 pixel, float4 fill) {
    float2 q = pixel - item.uv_bounds.xy;
    float r_in = item.uv_bounds.z;
    float r_out = item.uv_bounds.w;
    if (r_out <= r_in) return 0;
    float r = length(q);
    float d_rad = max(r - r_out, r_in - r);
    float d = d_rad;
    float cos_h = item.radius.z;
    if (cos_h > -1.0) {
        float2 dir = item.radius.xy;
        float2 sc = float2(cos_h, item.radius.w);
        float2 lq = float2(dot(q, dir), abs(q.x * dir.y - q.y * dir.x));
        float cross_wedge = lq.y * sc.x - lq.x * sc.y;
        float d_seg = length(lq - sc * clamp(dot(lq, sc), r_in, r_out));
        d = cross_wedge > 0.0 ? d_seg : max(d_rad, -d_seg);
    }
    float outer = saturate(0.5 - d);
    float stroke = 0;
    if (item.border_width > 0)
        stroke = max(0.0, outer - saturate(0.5 - (d + item.border_width)));
    return fill * (outer - stroke * item.border_color.a)
        + float4(item.border_color.rgb * item.border_color.a, item.border_color.a) * stroke;
}
