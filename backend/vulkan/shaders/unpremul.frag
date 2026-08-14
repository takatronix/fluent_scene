// unpremul.frag — final conversion: premultiplied float target → straight
// alpha for the RGBA8 readback surface (the same math as the CPU
// reference's output loop; UNORM store rounds).

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"

// Separated texture/sampler (WGSL has no combined form; Vulkan is
// fine with either) — the define keeps every call site unchanged.
layout(set = 0, binding = 0) uniform texture2D src_tex_t;
layout(set = 0, binding = 1) uniform sampler src_tex_s;
#define src_tex sampler2D(src_tex_t, src_tex_s)

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 p = texture(src_tex, v_uv);
    float a = clamp(p.a, 0.0, 1.0);
    vec3 rgb = a > 0.0 ? clamp(p.rgb / a, vec3(0.0), vec3(1.0)) : vec3(0.0);
    o_color = vec4(rgb, a);
}
