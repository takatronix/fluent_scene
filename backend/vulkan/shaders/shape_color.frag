// shape_color.frag — direct single-part rounded rect (layer backgrounds
// and borders): SDF → premultiplied color in one pass, like the CPU
// reference's drawRoundedRect. pa=(cx,cy,hx,hy) pb=(corner, stroke_w);
// px2.x = fold alpha.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"
#include "fluent_scene/shared/shapes_shared.h"

layout(location = 0) out vec4 o_color;

void main() {
    vec2 local = u_local(gl_FragCoord.xy);
    float cov = sd_rounded_rect_cov(local, u.pa.xy, u.pa.zw, u.pb.x, u.pb.y, u.meta.z,
                                    0.5 * vec2(u.inv0.x, u.inv1.x),
                                    0.5 * vec2(u.inv0.y, u.inv1.y));
    float a = u.color.a * u.px2.x * cov;
    o_color = vec4(u.color.rgb * a, a);
}
