// present.frag — WebGPU-only: the premultiplied float canvas straight onto
// the browser surface (canvas alphaMode "premultiplied"), clamped the way
// the CPU output loop clamps. The browser compositor then produces exactly
// what putImageData(straight RGBA) over the page background produces:
// clamp(rgb/a)·a = clamp(rgb, 0, a).
//
// Vulkan never uses this pass (it reads back through unpremul.frag); the
// file lives here so every backend's shader goes through the one
// glslc → SPIR-V (→ naga → WGSL) pipeline.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"

layout(set = 0, binding = 0) uniform texture2D src_tex_t;
layout(set = 0, binding = 1) uniform sampler src_tex_s;
#define src_tex sampler2D(src_tex_t, src_tex_s)

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 p = texture(src_tex, v_uv);
    float a = clamp(p.a, 0.0, 1.0);
    o_color = vec4(clamp(p.rgb, vec3(0.0), vec3(a)), a);
}
