// push_common.glsl — the one push-constant layout every fluent_scene
// pipeline shares (128 bytes, the spec minimum). The C++ recorder fills the
// same struct (VulkanRenderer::Push); member meaning varies per pipeline
// and is documented at each fill site.
//
// FS_WEBGPU: browser WGSL has no push constants, so the same block becomes
// a uniform buffer at group 1 (group 0 stays texture/sampler/storage, same
// as the Vulkan descriptor set). One draw = one dynamic-offset slice.

#ifdef FS_WEBGPU
#define FS_PUSH_DECL layout(set = 1, binding = 0) uniform
#else
#define FS_PUSH_DECL layout(push_constant) uniform
#endif

FS_PUSH_DECL PushBlock {
    vec4 rect;   // quad in target pixels (x, y, w, h)
    vec4 meta;   // target w, target h, aa (local units per pixel), mode/flags
    vec4 inv0;   // inverse target→local transform: a, c, tx
    vec4 inv1;   //                                 b, d, ty
    vec4 pa;     // pipeline-specific
    vec4 pb;     // pipeline-specific
    vec4 px2;    // pipeline-specific
    vec4 color;  // straight rgba
} u;

// Fragment position (pixel center) mapped into the draw's local space.
vec2 u_local(vec2 px) {
    return vec2(u.inv0.x * px.x + u.inv0.y * px.y + u.inv0.z,
                u.inv1.x * px.x + u.inv1.y * px.y + u.inv1.z);
}

int u_mode() { return int(u.meta.w + 0.5); }
