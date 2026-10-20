// image.frag — image content: inverse-map the fragment into layer space,
// antialias the destination rect's edges, sample the (premultiplied) source
// window bilinearly with the CPU reference's window clamp.
// pa = dest rect (x,y,w,h) in local units; pb = source window in pixels
// (x,y,w,h); px2 = (tex_w, tex_h, fold_alpha, 0).

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"
#include "fluent_scene/shared/shapes_shared.h"

// Separated texture/sampler (WGSL has no combined form; Vulkan is
// fine with either) — the define keeps every call site unchanged.
layout(set = 0, binding = 0) uniform texture2D image_tex_t;
layout(set = 0, binding = 1) uniform sampler image_tex_s;
#define image_tex sampler2D(image_tex_t, image_tex_s)

layout(location = 0) out vec4 o_color;

// One bilinear tap with the CPU reference's integer window clamp.
vec4 tap(int sx, int sy) {
    sx = clamp(sx, int(u.pb.x), int(u.pb.x + u.pb.z) - 1);
    sy = clamp(sy, int(u.pb.y), int(u.pb.y + u.pb.w) - 1);
    return texelFetch(image_tex, ivec2(sx, sy), 0);
}

void main() {
    vec2 local = u_local(gl_FragCoord.xy);
    // Antialiased dest edges (meta.z = aa, shapes_shared.h); pixel-aligned
    // edges equal a hard clip.
    float edge = sd_rounded_rect_cov(local, u.pa.xy + u.pa.zw * 0.5, u.pa.zw * 0.5, 0.0, 0.0,
                                     u.meta.z, 0.5 * vec2(u.inv0.x, u.inv1.x),
                                     0.5 * vec2(u.inv0.y, u.inv1.y));
    if (edge <= 0.0) {
        o_color = vec4(0.0);
        return;
    }
    // Manual bilinear, mirroring the CPU reference tap for tap: hardware
    // filtering quantizes the fraction and clamps differently at the
    // source-window edges.
    vec2 srcf = u.pb.xy + (local - u.pa.xy) * (u.pb.zw / u.pa.zw) - 0.5;
    vec2 base = floor(srcf);
    vec2 t = srcf - base;
    int sx0 = int(base.x);
    int sy0 = int(base.y);
    vec4 smp = (tap(sx0, sy0) * (1.0 - t.x) + tap(sx0 + 1, sy0) * t.x) * (1.0 - t.y) +
               (tap(sx0, sy0 + 1) * (1.0 - t.x) + tap(sx0 + 1, sy0 + 1) * t.x) * t.y;
    o_color = smp * (u.px2.z * edge);
}
