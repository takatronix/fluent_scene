// shape_mask.frag — evaluates one shape part's SDF coverage into the R16F
// mask (MAX blending unions overlapping parts, exactly like the CPU
// reference's coverage mask). The shape math is the shared single source.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_common.glsl"
#include "fluent_scene/shared/shapes_shared.h"
#include "shape_modes.h"

layout(std430, set = 0, binding = 0) readonly buffer Points { vec2 pts[]; };

layout(location = 0) out float o_cov;

// Signed distance to a polygon (negative inside, any winding) — the loop is
// backend structure, the per-edge math mirrors sd_segment (see
// shapes_shared.h's note on variable-length shapes).
float polygon_distance(int offset, int count, vec2 p) {
    float d2 = 1e30;
    float sgn = 1.0;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        vec2 vi = pts[offset + i];
        vec2 vj = pts[offset + j];
        vec2 e = vj - vi;
        vec2 w = p - vi;
        float t = clamp(dot(w, e) / max(dot(e, e), 1e-12), 0.0, 1.0);
        vec2 b = w - e * t;
        d2 = min(d2, dot(b, b));
        bool c0 = p.y >= vi.y;
        bool c1 = p.y < vj.y;
        bool c2 = e.x * w.y > e.y * w.x;
        if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) {
            sgn = -sgn;
        }
    }
    return sgn * sqrt(d2);
}

// The part's field (shapes_shared.h fill form: negative inside). `part`
// picks the grid's second field (its clip rect).
float shape_field(vec2 local, int mode, int part) {
    if (mode == SM_ROUNDED_RECT) {
        float d = sd_rounded_rect(local, u.pa.xy, u.pa.zw, u.pb.x);
        return u.pb.y > 0.0 ? sd_stroke_field(d, u.pb.y) : d;
    } else if (mode == SM_CIRCLE) {
        float d = sd_circle(local, u.pa.xy, u.pa.z);
        return u.pa.w > 0.0 ? sd_stroke_field(d, u.pa.w) : d;
    } else if (mode == SM_SEGMENT_ROUND) {
        return sd_stroke_field(sd_segment(local, u.pa.xy, u.pa.zw), u.pb.x);
    } else if (mode == SM_SEGMENT_BUTT) {
        return sd_segment_butt(local, u.pa.xy, u.pa.zw, u.pb.x);
    } else if (mode == SM_ARC) {
        return sd_arc(local, u.pa.xy, u.pa.z, u.pa.w, u.pb.x) - u.pb.y * 0.5;
    } else if (mode == SM_TRIANGLE) {
        return sd_triangle(local, u.pa.xy, u.pa.zw, u.pb.xy);
    } else if (mode == SM_GRID) {
        if (part == 0) {
            return sd_stroke_field(sd_grid(local, u.pa.x), u.pa.y);
        }
        return sd_rounded_rect(local, u.pb.xy, u.pb.zw, 0.0);
    } else if (mode == SM_POLYGON) {
        return polygon_distance(int(u.pa.x + 0.5), int(u.pa.y + 0.5), local);
    }
    return 1e6;
}

// Coverage of one field: sd_fill with the uniform `aa`, or — anisotropic
// transforms, aa < 0 — with the screen-space footprint from half-pixel
// steps (shapes_shared.h; the CPU's AaFrame runs the same steps).
float field_coverage(vec2 local, int mode, int part) {
    float f = shape_field(local, mode, part);
    float aa = u.meta.z;
    if (aa > 0.0) {
        return sd_fill(f, aa);
    }
    float early = sd_aniso_early(f, -aa);
    if (early >= 0.0) {
        return early;
    }
    vec2 hx = 0.5 * vec2(u.inv0.x, u.inv1.x);
    vec2 hy = 0.5 * vec2(u.inv0.y, u.inv1.y);
    float g = sd_aniso_aa(f, shape_field(local + hx, mode, part), shape_field(local - hx, mode, part),
                          shape_field(local + hy, mode, part), shape_field(local - hy, mode, part));
    return sd_fill_aniso(f, g, -aa);
}

void main() {
    vec2 local = u_local(gl_FragCoord.xy);
    int mode = u_mode();
    float cov = field_coverage(local, mode, 0);
    if (mode == SM_GRID) {
        cov *= field_coverage(local, mode, 1);
    }
    o_cov = cov;
}
