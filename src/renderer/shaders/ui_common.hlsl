struct UiInstance {
    float4 bounds;
    float4 uv_bounds;
    float4 tint;
    float4 border_color;
    float4 radius; // top-left, top-right, bottom-right, bottom-left
    float4 image_background;
    float border_width;
    uint shading; // ui_solid..ui_capsule; textured kinds keep their ui_textures slot from bit 2.
    uint2 clip; // Half-open pixels: left | top << 16, right | bottom << 16.
};
static const uint ui_solid = 0, ui_coverage = 1, ui_image = 2, ui_capsule = 3;
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
float rounded_coverage(float2 p, float2 size, float4 r) {
    if (any(size <= 0)) return 0;
    float d = max(max(-p.x, -p.y), max(p.x - size.x, p.y - size.y));
    if (r.x > 0 && p.x < r.x && p.y < r.x)
        d = max(d, length(p - float2(r.x, r.x)) - r.x);
    if (r.y > 0 && p.x > size.x - r.y && p.y < r.y)
        d = max(d, length(p - float2(size.x - r.y, r.y)) - r.y);
    if (r.z > 0 && p.x > size.x - r.z && p.y > size.y - r.z)
        d = max(d, length(p - (size - r.z)) - r.z);
    if (r.w > 0 && p.x < r.w && p.y > size.y - r.w)
        d = max(d, length(p - float2(r.w, size.y - r.w)) - r.w);
    return saturate(.5 - d);
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
