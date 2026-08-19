// filter_state.frag — the state pass of a stateful filter
// (persistent_buffers P1): advances the layer's persistent field one tick.
// Reads the premultiplied source (bindings 0/1), the filter's image
// parameter (2/3) and LAST frame's state (4/5); writes the new state RAW —
// it is data, not color, so there is no alpha shaping here.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"

layout(set = 0, binding = 0) uniform texture2D src_tex_t;
layout(set = 0, binding = 1) uniform sampler src_tex_s;
#define src_tex sampler2D(src_tex_t, src_tex_s)

layout(set = 0, binding = 2) uniform texture2D lut_tex_t;
layout(set = 0, binding = 3) uniform sampler lut_tex_s;
#define FS_SAMPLE_LUT(uv) (textureLod(sampler2D(lut_tex_t, lut_tex_s), uv, 0.0).rgb)

layout(set = 0, binding = 4) uniform texture2D state_tex_t;
layout(set = 0, binding = 5) uniform sampler state_tex_s;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 fs_sample_straight(vec2 uv) {
    ivec2 sz = textureSize(src_tex, 0);
    ivec2 p = clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1);
    vec4 c = texelFetch(src_tex, p, 0);
    return c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
}

#define FS_SAMPLE(uv) fs_sample_straight(uv)
#define FS_TEXEL (vec2(1.0) / vec2(textureSize(src_tex, 0)))

vec4 fs_sample_state_raw(vec2 uv) {
    ivec2 sz = textureSize(sampler2D(state_tex_t, state_tex_s), 0);
    ivec2 p = clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1);
    return texelFetch(sampler2D(state_tex_t, state_tex_s), p, 0);
}
#define FS_SAMPLE_STATE(uv) fs_sample_state_raw(uv)
#include "fluent_scene/shared/filters_shared.h"

void main() {
    o_color = fs_apply_state(u_mode(), v_uv, u.pa.x, u.pa.y, u.pa.z, u.pa.w,
                             u.pb.x);
}
