// glyph_mask.frag — one text glyph's coverage from the R8 atlas into the
// mask (MAX blending; glyphs union like every other multi-part content).
// pa = glyph rect in local units (x, y, w, h); pb = atlas uv (u0, v0, u1, v1).

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"

// Separated texture/sampler (WGSL has no combined form; Vulkan is
// fine with either) — the define keeps every call site unchanged.
layout(set = 0, binding = 0) uniform texture2D atlas_tex_t;
layout(set = 0, binding = 1) uniform sampler atlas_tex_s;
#define atlas_tex sampler2D(atlas_tex_t, atlas_tex_s)

layout(location = 0) out float o_cov;

void main() {
    vec2 local = u_local(gl_FragCoord.xy);
    vec2 f = (local - u.pa.xy) / u.pa.zw;
    if (f.x < 0.0 || f.y < 0.0 || f.x >= 1.0 || f.y >= 1.0) {
        o_cov = 0.0;
        return;
    }
    vec2 uv = u.pb.xy + f * (u.pb.zw - u.pb.xy);
    // textureLod, not texture: the early-out above is per-fragment control
    // flow, and WGSL forbids implicit-derivative sampling there. The atlas
    // has one mip, so lod 0 is the same picture on every backend.
    o_cov = textureLod(atlas_tex, uv, 0.0).r;
}
