// text_composite.frag — a finished text mask (r = fill, g = fill ∪ outline)
// into color: fill color over the fill, outline color over the ring, both
// through the luminance contrast curve (text_shared.h), premultiplied and
// blended with the layer's blend mode (fixed-function factors).
// meta.z = fold alpha; meta.w = flags (1 gradient, 2 outline);
// inv = target px → local (gradient); pa = gradient origin.xy, axis.zw;
// pb = gradient `to` color; px2 = outline color; color = fill color.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"
#include "fluent_scene/shared/text_shared.h"

layout(set = 0, binding = 0) uniform texture2D mask_tex_t;
layout(set = 0, binding = 1) uniform sampler mask_tex_s;
#define mask_tex sampler2D(mask_tex_t, mask_tex_s)

layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = (gl_FragCoord.xy - u.rect.xy) / u.rect.zw;
    vec2 m = texture(mask_tex, uv).rg;
    int flags = u_mode();
    vec4 fc = u.color;
    if ((flags & 1) != 0) {
        float t = text_gradient_t(u_local(gl_FragCoord.xy), u.pa.xy, u.pa.zw);
        fc = mix(u.color, u.pb, t);
    }
    float fcov = text_contrast(m.r, text_luma(fc.rgb));
    float fa = fc.a * fcov * u.meta.z;
    vec3 rgb = fc.rgb * fa;
    float a = fa;
    if ((flags & 2) != 0) {
        float ocov = max(fcov, text_contrast(max(m.g, m.r), text_luma(u.px2.rgb)));
        float oa = u.px2.a * (ocov - fcov) * u.meta.z;
        rgb += u.px2.rgb * oa;
        a += oa;
    }
    o_color = vec4(rgb, a);
}
