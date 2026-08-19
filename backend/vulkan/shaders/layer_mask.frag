// layer_mask.frag — image-driven alpha mask over a layer's finished pixels
// (post-filter; docs/design/layer_mask_input.ja.md). Multiplies the
// premultiplied offscreen by the mask image's ALPHA, mapped over the
// layer's bounds. pa = the v_uv→mask-uv affine (ax, bx, ay, by);
// pb.x = invert, pb.y = feather radius in buffer px (0 = hard sample).

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"

layout(set = 0, binding = 0) uniform texture2D src_tex_t;
layout(set = 0, binding = 1) uniform sampler src_tex_s;
#define src_tex sampler2D(src_tex_t, src_tex_s)
layout(set = 0, binding = 2) uniform texture2D mask_tex_t;
layout(set = 0, binding = 3) uniform sampler mask_tex_s;
#define mask_tex sampler2D(mask_tex_t, mask_tex_s)

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float mask_at(vec2 uv) {
    vec2 muv = vec2(uv.x * u.pa.x + u.pa.y, uv.y * u.pa.z + u.pa.w);
    return textureLod(mask_tex, clamp(muv, vec2(0.0), vec2(1.0)), 0.0).a;
}

void main() {
    vec2 texel = vec2(1.0) / vec2(textureSize(src_tex, 0));
    float m;
    if (u.pb.y > 0.01) {
        // 7-tap disc feather; the CPU reference mirrors these exact taps
        m = mask_at(v_uv) * 0.4;
        for (int i = 0; i < 6; ++i) {
            float a = 1.0471976 * float(i) + 0.2618;
            m += mask_at(v_uv + vec2(cos(a), sin(a)) * (u.pb.y * texel)) * 0.1;
        }
    } else {
        m = mask_at(v_uv);
    }
    m = clamp(m, 0.0, 1.0);
    if (u.pb.x > 0.5) {
        m = 1.0 - m;
    }
    o_color = texture(src_tex, v_uv) * m;
}
