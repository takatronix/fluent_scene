// fullscreen.vert — single-triangle fullscreen pass (filters, blur,
// unpremultiply). Outputs normalized uv over the target.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 c = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = c;
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
#ifdef FS_FLIP_Y
    // WebGL's default framebuffer has its origin bottom-left; every
    // offscreen target keeps the top-left row order of the other backends.
    gl_Position.y = -gl_Position.y;
#endif
}
