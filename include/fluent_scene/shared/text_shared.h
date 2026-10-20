// text_shared.h — glyph coverage and text paint math, written once in the
// GLSL∩C++ subset (like shapes_shared.h) and compiled into the CPU
// reference, glyph_mask.frag and text_composite.frag.
//
// A text draw is two steps:
//   1. every glyph writes (fill, outer) coverage into an RG mask with MAX
//      blending — glyphs union, overlaps never double-blend;
//   2. the mask composites: fill color over `fill`, outline color over the
//      ring `outer − fill`, through a luminance-dependent contrast curve.
//
// Glyph texels are either coverage (mode 0: an exact-size bitmap) or a
// signed distance field (mode 1: 0.5 = edge, inside > 0.5) whose full
// 0..1 range spans `px_range` output pixels — so antialiasing is exactly
// one output pixel wide at any scale, rotation or bucket.

// Coverage of a glyph texel value `v`, grown outward by `dilate` output
// pixels (synthetic bold, outlines; negative erodes). Bitmaps ignore
// `dilate` (bold is baked into their rasterization; outlines force SDF).
float text_coverage(float v, float mode, float px_range, float dilate) {
    if (mode < 0.5) {
        return v;
    }
    float d = (0.5 - v) * px_range;  // output px, positive outside
    return clamp(0.5 - d + dilate, 0.0, 1.0);
}

// Rec. 709 luma of a straight color.
float text_luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Coverage contrast: blending happens on sRGB-encoded values, which makes
// light-on-dark text look thin and dark-on-light text heavy. A mild,
// luminance-driven exponent (Skia/DirectWrite style) evens the stroke
// weight out without the halo of full linear-light blending.
float text_contrast(float cov, float luma) {
    return pow(clamp(cov, 0.0, 1.0), mix(1.10, 0.80, luma));
}

// Gradient parameter of local point `p`: `origin` is the gradient start,
// `axis` its direction divided by its length² (zero axis = no gradient).
float text_gradient_t(vec2 p, vec2 origin, vec2 axis) {
    return clamp(dot(p - origin, axis), 0.0, 1.0);
}
