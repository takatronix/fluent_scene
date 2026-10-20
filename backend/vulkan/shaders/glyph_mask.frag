// glyph_mask.frag — one glyph's (fill, outer) coverage from an atlas page
// into the text mask (MAX blending; glyphs union like every other
// multi-part content). Math: fluent_scene/shared/text_shared.h.
// pa = glyph tile in local units (x, y, w, h); pb = page uv (u0, v0, u1, v1);
// px2 = (mode 0 bitmap / 1 SDF, px_range, dilate_fill px, dilate_outer px).

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"
#include "fluent_scene/shared/text_shared.h"

// Separated texture/sampler (WGSL has no combined form; Vulkan is
// fine with either) — the define keeps every call site unchanged.
layout(set = 0, binding = 0) uniform texture2D atlas_tex_t;
layout(set = 0, binding = 1) uniform sampler atlas_tex_s;
#define atlas_tex sampler2D(atlas_tex_t, atlas_tex_s)

layout(location = 0) out vec2 o_cov;

// Field value at a local point (clamped into the glyph's tile).
float glyph_v(vec2 local) {
    vec2 f = clamp((local - u.pa.xy) / u.pa.zw, vec2(0.0), vec2(1.0));
    vec2 uv = u.pb.xy + f * (u.pb.zw - u.pb.xy);
    return textureLod(atlas_tex, uv, 0.0).r;
}

void main() {
    vec2 local = u_local(gl_FragCoord.xy);
    vec2 f = (local - u.pa.xy) / u.pa.zw;
    if (f.x < 0.0 || f.y < 0.0 || f.x >= 1.0 || f.y >= 1.0) {
        o_cov = vec2(0.0);
        return;
    }
    // textureLod, not texture: the early-out above is per-fragment control
    // flow, and WGSL forbids implicit-derivative sampling there. Pages have
    // one mip; the SDF antialiasing width comes from px2.y instead of
    // derivatives, so it is identical on every backend.
    float v = glyph_v(local);
    float range = u.px2.y;
    if (range < 0.0) {  // anisotropic: per-pixel range (text_shared.h)
        range = -range;
        if (!text_aniso_far(v, range, max(abs(u.px2.z), abs(u.px2.w)))) {
            vec2 hx = 0.5 * vec2(u.inv0.x, u.inv1.x);
            vec2 hy = 0.5 * vec2(u.inv0.y, u.inv1.y);
            range = text_aniso_range(v, glyph_v(local + hx), glyph_v(local - hx),
                                     glyph_v(local + hy), glyph_v(local - hy), range);
        }
    }
    o_cov = vec2(text_coverage(v, u.px2.x, range, u.px2.z),
                 text_coverage(v, u.px2.x, range, u.px2.w));
}
