// cpu_renderer.cpp — the reference compositor (§11, Phase L0).
//
// Correctness first, speed second: this backend defines what every frame
// must look like, and the Vulkan backend (L1) is validated against it.
//
// Pipeline per layer:
//   direct path   — plain leaves evaluate their SDF straight into the
//                   target through the full local→output transform
//                   (rotation stays vector-crisp);
//   offscreen path — layers with filters, shadows, masking, group opacity,
//                   or non-normal group blending render their subtree into
//                   a transient premultiplied-float buffer at output
//                   density, filter it, derive the shadow from its alpha,
//                   and composite with bilinear sampling.
//
// Internal pixel format: RGBA float32, premultiplied alpha, sRGB-encoded
// (compositing in encoded space, like the platform compositors this
// library's look is calibrated against). Surface output converts to
// straight-alpha RGBA8.

#include "fluent_scene/cpu_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "fluent_scene/shared/glsl_compat.hpp"

namespace fluent_scene {
namespace shapes {
using namespace glsl;
#include "fluent_scene/shared/shapes_shared.h"
}  // namespace shapes
namespace textfx {
using namespace glsl;
#include "fluent_scene/shared/text_shared.h"
}  // namespace textfx
}  // namespace fluent_scene

#include "fluent_scene/stage.hpp"
#include "render_shared.hpp"
#include "text/text_engine.hpp"

namespace fluent_scene {

namespace {

using glsl::vec2;
using glsl::vec3;
using glsl::vec4;
using plan::dashSegments;
using plan::IRect;
using plan::pathSegments;
using plan::rectIntersect;
using plan::rectPad;
using plan::rectUnion;
using plan::scaleOf;
using plan::Seg;
using plan::segBounds;
using plan::targetBBox;

vec2 gv(Vec2 v) { return {v.x, v.y}; }

constexpr float kPi = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Buf — premultiplied RGBA float32 pixels
// ---------------------------------------------------------------------------

struct Buf {
    int w = 0;
    int h = 0;
    std::vector<float> px;  // w * h * 4

    void reset(int width, int height) {
        w = width;
        h = height;
        px.assign(static_cast<size_t>(w) * h * 4, 0.0f);
    }
    float* at(int x, int y) { return px.data() + (static_cast<size_t>(y) * w + x) * 4; }
    const float* at(int x, int y) const {
        return px.data() + (static_cast<size_t>(y) * w + x) * 4;
    }
};

// Bilinear sample of a premultiplied buffer; outside is transparent.
void sampleBilinear(const Buf& b, float x, float y, float out[4]) {
    const float fx = x - 0.5f;
    const float fy = y - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            const int xi = x0 + dx;
            const int yi = y0 + dy;
            if (xi < 0 || yi < 0 || xi >= b.w || yi >= b.h) {
                continue;
            }
            const float wgt = (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty);
            const float* p = b.at(xi, yi);
            for (int c = 0; c < 4; ++c) {
                out[c] += p[c] * wgt;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Blending — THE definition of the four modes, shared with the GPU.
//
// All buffers are premultiplied. Each mode is a fixed-function blend
// equation (cs/cd premultiplied source/dest, sa source alpha), so the
// Vulkan backend realizes the identical math as blend factors:
//
//   Normal:   co = cs           + cd*(1-sa)     (ONE, ONE_MINUS_SRC_ALPHA)
//   Add:      co = cs           + cd            (ONE, ONE)
//   Multiply: co = cs*cd        + cd*(1-sa)     (DST_COLOR, ONE_MINUS_SRC_ALPHA)
//   Screen:   co = cs*(1-cd)    + cd            (ONE_MINUS_DST_COLOR, ONE)
//   ao = sa + da*(1-sa)                          (all modes)
//
// Values may exceed 1 mid-composite (Add); the final surface conversion
// clamps, exactly like a float16 render target read back through unpremul.
// ---------------------------------------------------------------------------

// Composites a premultiplied source sample over dst.
void blendPremul(float* dst, const float src[4], Blend mode) {
    const float sa = src[3];
    if (sa <= 0) {
        return;
    }
    for (int c = 0; c < 3; ++c) {
        switch (mode) {
            case Blend::Normal:   dst[c] = src[c] + dst[c] * (1 - sa); break;
            case Blend::Add:      dst[c] = src[c] + dst[c]; break;
            case Blend::Multiply: dst[c] = src[c] * dst[c] + dst[c] * (1 - sa); break;
            case Blend::Screen:   dst[c] = src[c] * (1 - dst[c]) + dst[c]; break;
        }
    }
    dst[3] = sa + dst[3] * (1 - sa);
}

// Composites one straight-alpha source sample (color + coverage) over dst.
void blendPixel(float* dst, Color src, float alpha, Blend mode) {
    const float sa = src.a * alpha;
    if (sa <= 0) {
        return;
    }
    const float premul[4] = {src.r * sa, src.g * sa, src.b * sa, sa};
    blendPremul(dst, premul, mode);
}

// ---------------------------------------------------------------------------
// Coverage mask — correct antialiased union of overlapping strokes
// ---------------------------------------------------------------------------

// Multi-part content (polyline joints, dashes, markers, grids) must not
// double-blend where parts overlap, so parts accumulate max-coverage into a
// mask over the content's target bbox, and the mask composites once.
//
// Only what parts actually touch is ever initialized or composited: each row
// keeps the span its parts wrote, so a long thin path whose bbox is most of
// the frame costs its own pixels, not the bbox's.
struct Mask {
    IRect box;
    std::unique_ptr<float[]> cov;  // row-major over box; valid inside spans only
    std::vector<int> span_x0, span_x1;  // per row, half-open; empty when x0 >= x1

    void reset(IRect b) {
        box = b;
        const int w = std::max(0, b.x1 - b.x0);
        const int h = std::max(0, b.y1 - b.y0);
        cov.reset(new float[static_cast<size_t>(w) * static_cast<size_t>(h)]);
        span_x0.assign(static_cast<size_t>(h), 0);
        span_x1.assign(static_cast<size_t>(h), 0);
    }
    float* at(int x, int y) {
        return cov.get() + static_cast<size_t>(y - box.y0) * (box.x1 - box.x0) + (x - box.x0);
    }
    // Makes [x0, x1) of row y writable: zero-fills what the row's span did
    // not cover yet and grows the span (callers stay inside box).
    void touch(int y, int x0, int x1) {
        if (x0 >= x1) {
            return;
        }
        const size_t row = static_cast<size_t>(y - box.y0);
        int& s0 = span_x0[row];
        int& s1 = span_x1[row];
        if (s0 >= s1) {
            std::fill(at(x0, y), at(x0, y) + (x1 - x0), 0.0f);
            s0 = x0;
            s1 = x1;
            return;
        }
        if (x0 < s0) {
            std::fill(at(x0, y), at(s0, y), 0.0f);
            s0 = x0;
        }
        if (x1 > s1) {
            std::fill(at(s1, y), at(x1, y), 0.0f);
            s1 = x1;
        }
    }
};

// The part of `local_bbox` (+pad px) a part may write, clipped to the mask.
IRect partBox(const Mask& mask, const Mat23& fwd, Rect local_bbox, float pad, int w, int h) {
    IRect r = targetBBox(fwd, local_bbox, pad, w, h);
    r.x0 = std::max(r.x0, mask.box.x0);
    r.y0 = std::max(r.y0, mask.box.y0);
    r.x1 = std::min(r.x1, mask.box.x1);
    r.y1 = std::min(r.y1, mask.box.y1);
    return r;
}

// Rasterizes one part: evaluates `coverage(local)` over the target bbox of
// `local_bbox` and max-accumulates into the mask.
template <typename CoverageFn>
void rasterizePart(Mask& mask, const Mat23& inv, const Mat23& fwd, Rect local_bbox, float pad,
                   int w, int h, CoverageFn coverage) {
    const IRect r = partBox(mask, fwd, local_bbox, pad, w, h);
    if (r.empty()) {
        return;
    }
    for (int y = r.y0; y < r.y1; ++y) {
        mask.touch(y, r.x0, r.x1);
        for (int x = r.x0; x < r.x1; ++x) {
            const Vec2 local = inv.apply({x + 0.5f, y + 0.5f});
            const float c = coverage(gv(local));
            if (c > 0) {
                float* m = mask.at(x, y);
                *m = std::max(*m, c);
            }
        }
    }
}

// Coverage of a shape field (shapes_shared.h: fill-form signed distance)
// through `m`: one sd_fill when m is conformal, otherwise the screen-space
// gradient from half-pixel steps (the same steps as shape_mask.frag).
struct AaFrame {
    float aa;  // plan::aaParam: > 0 conformal, < 0 anisotropic
    vec2 hx, hy;
    AaFrame(const Mat23& m, const Mat23& inv)
        : aa(plan::aaParam(m)), hx{inv.a * 0.5f, inv.b * 0.5f}, hy{inv.c * 0.5f, inv.d * 0.5f} {}
    template <typename FieldFn>
    float cover(vec2 p, FieldFn field) const {
        const float f = field(p);
        if (aa > 0) {
            return shapes::sd_fill(f, aa);
        }
        const float early = shapes::sd_aniso_early(f, -aa);
        if (early >= 0) {
            return early;
        }
        const float g = shapes::sd_aniso_aa(f, field(p + hx), field(p - hx), field(p + hy),
                                            field(p - hy));
        return shapes::sd_fill_aniso(f, g, -aa);
    }
};

void compositeMask(Buf& target, const Mask& mask, Color color, float alpha, Blend mode) {
    const int stride = mask.box.x1 - mask.box.x0;
    for (int y = mask.box.y0; y < mask.box.y1; ++y) {
        const size_t row = static_cast<size_t>(y - mask.box.y0);
        const float* cov = mask.cov.get() + row * stride - mask.box.x0;
        for (int x = mask.span_x0[row]; x < mask.span_x1[row]; ++x) {
            const float c = cov[x];
            if (c > 0) {
                blendPixel(target.at(x, y), color, alpha * c, mode);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Filters — the shared bodies, compiled for the CPU
// ---------------------------------------------------------------------------

thread_local const Buf* g_filter_src = nullptr;

vec3 filterSample(vec2 uv) {
    const Buf& b = *g_filter_src;
    int x = static_cast<int>(uv.x * b.w);
    int y = static_cast<int>(uv.y * b.h);
    x = std::clamp(x, 0, b.w - 1);
    y = std::clamp(y, 0, b.h - 1);
    const float* p = b.at(x, y);
    // Bodies see straight rgb, like a GPU sampling a resolved texture.
    const float a = p[3];
    if (a <= 0) {
        return {0, 0, 0};
    }
    return {p[0] / a, p[1] / a, p[2] / a};
}

vec2 filterTexel() { return {1.0f / g_filter_src->w, 1.0f / g_filter_src->h}; }

// The filter-image parameter (`lut` grade atlas): borrowed RGBA8 straight
// from the ImageView, sampled bilinearly — the LUT body aligns its reads to
// texel centers and rides the sampler for in-slice interpolation, exactly
// like the GPU's linear sampler will.
thread_local const ImageView* g_filter_image = nullptr;

vec3 filterImageSample(vec2 uv) {
    const ImageView& v = *g_filter_image;
    const float fx = std::clamp(uv.x, 0.0f, 1.0f) * v.width - 0.5f;
    const float fy = std::clamp(uv.y, 0.0f, 1.0f) * v.height - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0,
                              static_cast<int>(v.width) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0,
                              static_cast<int>(v.height) - 1);
    const int x1 = std::min(x0 + 1, static_cast<int>(v.width) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(v.height) - 1);
    const float tx = std::clamp(fx - x0, 0.0f, 1.0f);
    const float ty = std::clamp(fy - y0, 0.0f, 1.0f);
    auto texel = [&v](int x, int y) {
        const uint8_t* p = v.pixels + static_cast<size_t>(y) * v.stride() + x * 4;
        return vec3{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
    };
    const vec3 top = texel(x0, y0) * (1 - tx) + texel(x1, y0) * tx;
    const vec3 bottom = texel(x0, y1) * (1 - tx) + texel(x1, y1) * tx;
    return top * (1 - ty) + bottom * ty;
}

// Persistent filter state (persistent_buffers P1): the previous frame's
// state field. Read NEAREST and raw — it is data, not color, and nearest
// keeps the three backends bit-honest with no filtering to disagree on.
thread_local const Buf* g_filter_state = nullptr;

vec4 filterStateSample(vec2 uv) {
    const Buf& b = *g_filter_state;
    int x = static_cast<int>(uv.x * b.w);
    int y = static_cast<int>(uv.y * b.h);
    x = std::clamp(x, 0, b.w - 1);
    y = std::clamp(y, 0, b.h - 1);
    const float* p = b.at(x, y);
    return {p[0], p[1], p[2], p[3]};
}

namespace filter_bodies {
using namespace glsl;
#define FS_SAMPLE(uv) filterSample(uv)
#define FS_TEXEL filterTexel()
#define FS_SAMPLE_LUT(uv) filterImageSample(uv)
#define FS_SAMPLE_STATE(uv) filterStateSample(uv)
#include "fluent_scene/shared/filters_shared.h"
#undef FS_SAMPLE
#undef FS_TEXEL
#undef FS_SAMPLE_LUT
#undef FS_SAMPLE_STATE
}  // namespace filter_bodies

// Image-driven layer mask (docs/design/layer_mask_input.ja.md): the mask's
// ALPHA, mapped over the layer's bounds, multiplies the finished offscreen.
// Bilinear with the GPU sampler's half-texel convention; the 7-tap feather
// disc mirrors layer_mask.frag exactly.

float maskAlphaBilinear(const ImageView& v, float ux, float uy) {
    ux = std::clamp(ux, 0.0f, 1.0f);
    uy = std::clamp(uy, 0.0f, 1.0f);
    const float fx = ux * v.width - 0.5f;
    const float fy = uy * v.height - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0,
                              static_cast<int>(v.width) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0,
                              static_cast<int>(v.height) - 1);
    const int x1 = std::min(x0 + 1, static_cast<int>(v.width) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(v.height) - 1);
    const float tx = std::clamp(fx - x0, 0.0f, 1.0f);
    const float ty = std::clamp(fy - y0, 0.0f, 1.0f);
    auto a = [&v](int x, int y) {
        return v.pixels[static_cast<size_t>(y) * v.stride() + x * 4 + 3] / 255.0f;
    };
    const float top = a(x0, y0) * (1 - tx) + a(x1, y0) * tx;
    const float bottom = a(x0, y1) * (1 - tx) + a(x1, y1) * tx;
    return top * (1 - ty) + bottom * ty;
}

void applyLayerMask(Buf& buf, const Layer& layer, float scale, Rect ext, Rect bounds) {
    const ImageView& mv = layer.maskValue();
    const float ax = ext.w / std::max(bounds.w, 1e-4f);
    const float bx = (ext.x - bounds.x) / std::max(bounds.w, 1e-4f);
    const float ay = ext.h / std::max(bounds.h, 1e-4f);
    const float by = (ext.y - bounds.y) / std::max(bounds.h, 1e-4f);
    const float feather_px = layer.maskFeatherValue() * scale;
    const float tx = 1.0f / buf.w;
    const float ty = 1.0f / buf.h;
    auto mask_at = [&](float uu, float vv) {
        return maskAlphaBilinear(mv, uu * ax + bx, vv * ay + by);
    };
    for (int y = 0; y < buf.h; ++y) {
        for (int x = 0; x < buf.w; ++x) {
            const float uu = (x + 0.5f) * tx;
            const float vv = (y + 0.5f) * ty;
            float m;
            if (feather_px > 0.01f) {
                m = mask_at(uu, vv) * 0.4f;
                for (int i = 0; i < 6; ++i) {
                    const float a = 1.0471976f * i + 0.2618f;
                    m += mask_at(uu + std::cos(a) * feather_px * tx,
                                 vv + std::sin(a) * feather_px * ty) *
                         0.1f;
                }
            } else {
                m = mask_at(uu, vv);
            }
            m = std::clamp(m, 0.0f, 1.0f);
            if (layer.maskInvertValue()) {
                m = 1.0f - m;
            }
            float* d = buf.at(x, y);
            d[0] *= m;
            d[1] *= m;
            d[2] *= m;
            d[3] *= m;
        }
    }
}

// Separable gaussian blur on the premultiplied buffer (radius in buffer px).
void gaussianBlur(Buf& buf, float radius_px) {
    if (radius_px <= 0.01f || buf.w == 0 || buf.h == 0) {
        return;
    }
    const float sigma = radius_px * 0.5f;
    const int half = std::max(1, static_cast<int>(std::ceil(sigma * 2.5f)));
    std::vector<float> weights(half + 1);
    float sum = 0;
    for (int i = 0; i <= half; ++i) {
        weights[i] = std::exp(-0.5f * (i / sigma) * (i / sigma));
        sum += i == 0 ? weights[i] : 2 * weights[i];
    }
    for (float& w : weights) {
        w /= sum;
    }
    Buf tmp;
    tmp.reset(buf.w, buf.h);
    for (int y = 0; y < buf.h; ++y) {  // horizontal
        for (int x = 0; x < buf.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = -half; i <= half; ++i) {
                const int xi = std::clamp(x + i, 0, buf.w - 1);
                const float* p = buf.at(xi, y);
                const float w = weights[std::abs(i)];
                for (int c = 0; c < 4; ++c) {
                    acc[c] += p[c] * w;
                }
            }
            std::memcpy(tmp.at(x, y), acc, sizeof acc);
        }
    }
    for (int y = 0; y < buf.h; ++y) {  // vertical
        for (int x = 0; x < buf.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = -half; i <= half; ++i) {
                const int yi = std::clamp(y + i, 0, buf.h - 1);
                const float* p = tmp.at(x, yi);
                const float w = weights[std::abs(i)];
                for (int c = 0; c < 4; ++c) {
                    acc[c] += p[c] * w;
                }
            }
            std::memcpy(buf.at(x, y), acc, sizeof acc);
        }
    }
}

// A stateful filter's per-layer ping-pong field (persistent_buffers P1).
// ping is the read side (last frame); the state pass writes pong, then the
// pair swaps. Sized to the layer's offscreen buffer; a resize resets to
// zero (the documented cold start).
struct FilterStatePair {
    Buf ping;
    Buf pong;
    uint64_t stamp = 0;
};

// The CPU's low-resolution path for the generative liquid (its `quality`
// slot): the raymarch runs once per stride×stride block corner and smooth
// blocks are bilinearly upsampled; blocks whose corners disagree (the
// silhouette, a droplet's edge — a few percent of the area) are marched
// per pixel, so the edge never stairs at block size. GPUs never take it
// (they spend quality on the step budget instead); goldens pin quality 1
// where both agree.
int liquidCpuStride(float quality) {
    return quality >= 0.75f ? 1 : quality >= 0.4f ? 2 : quality >= 0.25f ? 3 : 4;
}

void applyFilterCoarse(Buf& buf, const Buf& src, int mode, vec4 pa, vec4 pb, vec4 pc,
                       vec4 pd, int stride) {
    const int cw = (buf.w + stride - 1) / stride + 1;
    const int ch = (buf.h + stride - 1) / stride + 1;
    std::vector<vec4> coarse(static_cast<size_t>(cw) * ch);
    auto premul = [&](int x, int y) {
        const vec2 uv{(x + 0.5f) / buf.w, (y + 0.5f) / buf.h};
        const vec4 o = filter_bodies::fs_apply(mode, uv, pa, pb, pc, pd);
        return vec4{o.x * o.w, o.y * o.w, o.z * o.w, o.w};
    };
    for (int cy = 0; cy < ch; ++cy) {
        for (int cx = 0; cx < cw; ++cx) {
            // the block's corner pixel center (clamped to the buffer)
            coarse[static_cast<size_t>(cy) * cw + cx] =
                premul(std::min(cx * stride, buf.w - 1), std::min(cy * stride, buf.h - 1));
        }
    }
    const float inv = 1.0f / stride;
    for (int by = 0; by * stride < buf.h; ++by) {
        for (int bx = 0; bx * stride < buf.w; ++bx) {
            const int bx1 = std::min(bx + 1, cw - 1);
            const int by1 = std::min(by + 1, ch - 1);
            const vec4& a = coarse[static_cast<size_t>(by) * cw + bx];
            const vec4& b = coarse[static_cast<size_t>(by) * cw + bx1];
            const vec4& c = coarse[static_cast<size_t>(by1) * cw + bx];
            const vec4& d = coarse[static_cast<size_t>(by1) * cw + bx1];
            auto spread = [](float va, float vb, float vc, float vd) {
                return std::max(std::max(va, vb), std::max(vc, vd)) -
                       std::min(std::min(va, vb), std::min(vc, vd));
            };
            const bool edge = spread(a.w, b.w, c.w, d.w) > 0.12f ||
                              spread(a.x, b.x, c.x, d.x) > 0.25f ||
                              spread(a.y, b.y, c.y, d.y) > 0.25f ||
                              spread(a.z, b.z, c.z, d.z) > 0.25f;
            for (int y = by * stride; y < std::min((by + 1) * stride, buf.h); ++y) {
                const float ty = (y - by * stride) * inv;
                for (int x = bx * stride; x < std::min((bx + 1) * stride, buf.w); ++x) {
                    const float tx = (x - bx * stride) * inv;
                    vec4 v;
                    if (edge) {
                        v = (x == bx * stride && y == by * stride) ? a : premul(x, y);
                    } else {
                        auto lerp2 = [&](float va, float vb, float vc, float vd) {
                            return (va * (1 - tx) + vb * tx) * (1 - ty) +
                                   (vc * (1 - tx) + vd * tx) * ty;
                        };
                        v = {lerp2(a.x, b.x, c.x, d.x), lerp2(a.y, b.y, c.y, d.y),
                             lerp2(a.z, b.z, c.z, d.z), lerp2(a.w, b.w, c.w, d.w)};
                    }
                    const float src_a = src.at(x, y)[3];
                    float* o = buf.at(x, y);
                    o[0] = v.x * src_a;
                    o[1] = v.y * src_a;
                    o[2] = v.z * src_a;
                    o[3] = v.w * src_a;
                }
            }
        }
    }
}

// Applies one filter to the buffer. `scale` converts logical units to
// buffer pixels; `ext` is the local-space rect the buffer covers (§5-3).
// `state` is non-null exactly when the filter's spec is stateful.
void applyFilter(Buf& buf, const Filter& f, float scale, Rect ext,
                 FilterStatePair* state) {
    float values[kFilterSlots];
    plan::scaleFilterValues(f, scale, ext, static_cast<float>(buf.w),
                            static_cast<float>(buf.h), values);
    if (f.mode == FS_BLUR) {
        gaussianBlur(buf, values[0]);
        return;
    }
    if (f.mode == FS_LUT) {
        // An unfed or malformed atlas leaves the layer ungraded — the
        // documented pass-through, not an error.
        if (!lutAtlasGrid(f.image, &values[2], &values[3])) {
            return;
        }
        g_filter_image = &f.image;
    } else if (f.mode == FS_FACE || f.mode == FS_TEXTURE) {
        // No landmark strip = no face on screen; an unfed texture plate
        // likewise: pass through quietly.
        if (!f.image.valid()) {
            return;
        }
        g_filter_image = &f.image;
    } else if (f.image.valid()) {
        // Generic image parameter (sumie's paper): bind, never gate.
        g_filter_image = &f.image;
    }
    Buf src = buf;  // filters read the unmodified source
    g_filter_src = &src;
    if (state != nullptr) {
        // State pass: advance the field one tick (reads last frame's ping
        // + the source, writes pong RAW), then swap so the output pass
        // below sees the fresh field.
        if (state->ping.w != buf.w || state->ping.h != buf.h) {
            state->ping.reset(buf.w, buf.h);
            state->pong.reset(buf.w, buf.h);
        }
        g_filter_state = &state->ping;
        for (int y = 0; y < buf.h; ++y) {
            for (int x = 0; x < buf.w; ++x) {
                const vec2 uv{(x + 0.5f) / buf.w, (y + 0.5f) / buf.h};
                const vec4 out = filter_bodies::fs_apply_state(
                    f.mode, uv, values[0], values[1], values[2], values[3], values[4]);
                float* d = state->pong.at(x, y);
                d[0] = out.x;
                d[1] = out.y;
                d[2] = out.z;
                d[3] = out.w;
            }
        }
        std::swap(state->ping, state->pong);
        g_filter_state = &state->ping;
    }
    const vec4 pa{values[0], values[1], values[2], values[3]};
    const vec4 pb{values[4], values[5], values[6], values[7]};
    const vec4 pc{values[8], values[9], values[10], values[11]};
    const vec4 pd{values[12], values[13], values[14], values[15]};
    if (f.mode == FS_LIQUID) {
        const int stride = liquidCpuStride(values[11]);
        if (stride > 1) {
            applyFilterCoarse(buf, src, f.mode, pa, pb, pc, pd, stride);
            g_filter_src = nullptr;
            g_filter_image = nullptr;
            g_filter_state = nullptr;
            return;
        }
    }
    for (int y = 0; y < buf.h; ++y) {
        for (int x = 0; x < buf.w; ++x) {
            const float src_a = src.at(x, y)[3];
            const vec2 uv{(x + 0.5f) / buf.w, (y + 0.5f) / buf.h};
            const vec4 out = filter_bodies::fs_apply(f.mode, uv, pa, pb, pc, pd);
            float* d = buf.at(x, y);
            // Filters keep the source's alpha shape (resampling filters
            // approximate; exact per-filter alpha is refined with L1 golden
            // work). FS_OPACITY scales it.
            const float a = filter_bodies::fs_shapes_alpha(f.mode) ? src_a * out.w : src_a;
            d[0] = out.x * a;
            d[1] = out.y * a;
            d[2] = out.z * a;
            d[3] = a;
        }
    }
    g_filter_src = nullptr;
    g_filter_image = nullptr;
    g_filter_state = nullptr;
}

// One polygon edge's contribution to polygonDistance: the squared distance
// to the edge, and whether the point's +x ray crosses it (sign toggle).
// Shared by the brute-force and the culled fill so both compute bit-equal
// distances and signs.
inline void polygonEdge(vec2 vi, vec2 vj, vec2 p, float& d2, bool& toggle) {
    const vec2 e = vj - vi;
    const vec2 w = p - vi;
    const float t = glsl::clamp(glsl::dot(w, e) / std::max(glsl::dot(e, e), 1e-12f), 0.0f, 1.0f);
    const vec2 b = w - e * t;
    d2 = std::min(d2, glsl::dot(b, b));
    const bool c0 = p.y >= vi.y;
    const bool c1 = p.y < vj.y;
    const bool c2 = e.x * w.y > e.y * w.x;
    toggle = (c0 && c1 && c2) || (!c0 && !c1 && !c2);
}

// Signed distance to a polygon (negative inside, any winding; per-edge math
// mirrors sd_segment — the loop is renderer structure, per shapes_shared.h).
float polygonDistance(const std::vector<Vec2>& pts, vec2 p) {
    float d2 = 1e30f;
    float sign = 1.0f;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        bool toggle = false;
        polygonEdge(gv(pts[i]), gv(pts[j]), p, d2, toggle);
        if (toggle) {
            sign = -sign;
        }
    }
    return sign * std::sqrt(d2);
}

}  // namespace

namespace detail {
// Test seam (not API): forces the brute-force polygon fill so stage_tests
// can prove rasterizePolygonFill bit-identical to it.
bool g_cpu_polygon_reference = false;
}  // namespace detail

namespace {

// Filled-polygon coverage, sd_fill(polygonDistance(p), aa) at every pixel of
// the part box — but costing ~pixels × nearby edges instead of pixels × all
// edges (a 240-edge waveform band went from ~35 ms to ~1 ms at 900×800).
//
// Needs an axis-aligned `inv` (no rotation/skew), so each target row maps to
// one local y; callers fall back to rasterizePart otherwise. Per row, then
// per chunk of kChunk pixels:
//   - distance: only edges whose bbox, padded by `near` (local units, ≥ aa),
//     overlaps the chunk can be closer than `near`; farther edges cannot
//     change sd_fill, which saturates beyond aa/2. The minimum over the near
//     edges is therefore the exact minimum wherever it matters.
//   - sign: an edge toggles only when it straddles the row (c0/c1 depend on
//     y alone). A straddling edge that is not near lies wholly left or right
//     of the chunk, so it toggles exactly when it lies to the right — counted
//     once per chunk. Near edges are evaluated per pixel with the same
//     predicate as polygonDistance.
// Every evaluated value is the brute-force one: output is bit-identical.
void rasterizePolygonFill(Mask& mask, const Mat23& inv, const Mat23& fwd,
                          const std::vector<Vec2>& pts, Rect local_bbox, float pad_px, float aa,
                          int w, int h) {
    const IRect r = partBox(mask, fwd, local_bbox, pad_px, w, h);
    if (r.empty()) {
        return;
    }
    constexpr int kChunk = 16;
    const float near = 2.0f * aa;
    struct Edge {
        vec2 vi, vj;
        float x0, x1, y0, y1;  // bbox padded by `near`
    };
    std::vector<Edge> edges;
    edges.reserve(pts.size());
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        Edge e{gv(pts[i]), gv(pts[j]), 0, 0, 0, 0};
        e.x0 = std::min(e.vi.x, e.vj.x) - near;
        e.x1 = std::max(e.vi.x, e.vj.x) + near;
        e.y0 = std::min(e.vi.y, e.vj.y) - near;
        e.y1 = std::max(e.vi.y, e.vj.y) + near;
        edges.push_back(e);
    }
    std::vector<const Edge*> row_near, row_cross, chunk_near;
    for (int y = r.y0; y < r.y1; ++y) {
        const float ly = inv.apply({r.x0 + 0.5f, y + 0.5f}).y;  // the row's local y
        row_near.clear();
        row_cross.clear();
        for (const Edge& e : edges) {
            if (ly < e.y0 || ly > e.y1) {
                continue;
            }
            row_near.push_back(&e);
            const bool c0 = ly >= e.vi.y;
            const bool c1 = ly < e.vj.y;
            if (c0 == c1) {
                row_cross.push_back(&e);  // straddles: may toggle the sign
            }
        }
        if (row_cross.empty() && row_near.empty()) {
            continue;  // outside: coverage 0, nothing to write
        }
        for (int cx = r.x0; cx < r.x1; cx += kChunk) {
            const int cx1 = std::min(cx + kChunk, r.x1);
            const float lxa = inv.apply({cx + 0.5f, y + 0.5f}).x;
            const float lxb = inv.apply({cx1 - 0.5f, y + 0.5f}).x;
            const float lx0 = std::min(lxa, lxb);
            const float lx1 = std::max(lxa, lxb);
            chunk_near.clear();
            for (const Edge* e : row_near) {
                if (e->x1 >= lx0 && e->x0 <= lx1) {
                    chunk_near.push_back(e);
                }
            }
            bool far_odd = false;  // far straddling edges to the right
            for (const Edge* e : row_cross) {
                if (e->x0 > lx1) {
                    far_odd = !far_odd;
                }
            }
            if (chunk_near.empty() && !far_odd) {
                continue;  // wholly outside this chunk
            }
            mask.touch(y, cx, cx1);
            for (int x = cx; x < cx1; ++x) {
                const vec2 p = gv(inv.apply({x + 0.5f, y + 0.5f}));
                float d2 = 1e30f;
                bool odd = far_odd;
                for (const Edge* e : chunk_near) {
                    bool toggle = false;
                    polygonEdge(e->vi, e->vj, p, d2, toggle);
                    odd ^= toggle;
                }
                const float c = shapes::sd_fill((odd ? -1.0f : 1.0f) * std::sqrt(d2), aa);
                if (c > 0) {
                    float* m = mask.at(x, y);
                    *m = std::max(*m, c);
                }
            }
        }
    }
}

}  // namespace

// ===========================================================================
// CpuRenderer::Impl
// ===========================================================================

struct CpuRenderer::Impl {
    Options options;
    detail::TextEngine text;
    bool text_inited = false;
    Buf frame;
    std::vector<uint8_t> out_rgba;
    Surface surface;
    // Stateful-filter fields, keyed by (layer identity, filter mode) and
    // swept when their layer stops rendering (persistent_buffers P1).
    std::map<std::pair<const void*, int>, FilterStatePair> filter_states;
    uint64_t frame_stamp = 0;

    // ---- fonts ------------------------------------------------------------

    void ensureText() {
        if (!text_inited) {
            text_inited = true;
            text.init(options.font_file);
        }
    }

    // Layout box in logical units (Stage measurement hook).
    Rect measureText(const TextContent& c) {
        ensureText();
        return text.metrics(c).box;
    }

    // Everything a text content may paint (offscreen extents).
    Rect textExtent(const TextContent& c) {
        ensureText();
        return text.extent(c);
    }

    // ---- extent of a subtree in its own local space -----------------------

    plan::TextMeasure measurer() {
        return [this](const TextContent& c) { return textExtent(c); };
    }

    // ---- content rasterization --------------------------------------------

    // Draws the layer's content into `target` through `m` (content-local →
    // target), with `alpha` folded in and the layer's blend mode.
    void drawContent(Buf& target, const Layer& layer, Rect bounds, const Mat23& m, float alpha) {
        const Content& content = layer.content();
        if (std::holds_alternative<std::monostate>(content)) {
            return;
        }
        const Mat23 inv = m.inverse();
        const float scale = scaleOf(m);
        if (scale <= 0) {
            return;
        }
        const float aa = 1.0f / scale;
        const AaFrame af(m, inv);
        const float th = layer.thickness();
        const Color color = layer.presentedColor();
        const Blend mode = layer.blendMode();
        const float pad_px = (th * 0.5f + 2.0f) * plan::maxScaleOf(m);

        // Shared plumbing: build a mask over the whole content, rasterize
        // parts into it, composite once.
        Mask mask;
        const auto begin = [&](Rect local_bbox, float extra_pad_logical) {
            mask.reset(targetBBox(m, local_bbox, pad_px + extra_pad_logical * scale,
                                  target.w, target.h));
            return !mask.box.empty();
        };
        // `field` returns the fill-form signed distance (negative inside).
        const auto part = [&](Rect local_bbox, float extra_pad_logical, auto field) {
            rasterizePart(mask, inv, m, local_bbox, pad_px + extra_pad_logical * scale,
                          target.w, target.h, [&](vec2 p) { return af.cover(p, field); });
        };
        const auto finish = [&] { compositeMask(target, mask, color, alpha, mode); };

        const auto strokeSegs = [&](const std::vector<Seg>& segs, Cap capstyle) {
            for (const Seg& s : segs) {
                const vec2 a = gv(s.a);
                const vec2 b = gv(s.b);
                if (capstyle == Cap::Butt) {
                    part(segBounds(s), 0, [&, a, b](vec2 p) {
                        return shapes::sd_segment_butt(p, a, b, th * 0.5f);
                    });
                } else {
                    part(segBounds(s), 0, [&, a, b](vec2 p) {
                        return shapes::sd_stroke_field(shapes::sd_segment(p, a, b), th);
                    });
                }
            }
        };

        if (const auto* c = std::get_if<ImageContent>(&content)) {
            drawImage(target, *c, bounds, m, inv, alpha, mode);
        } else if (const auto* c = std::get_if<TextContent>(&content)) {
            drawText(target, *c, m, inv, color, alpha, mode, scale);
        } else if (const auto* c = std::get_if<LineContent>(&content)) {
            if (begin(segBounds({c->from, c->to}), 0)) {
                strokeSegs(dashSegments({{c->from, c->to}}, layer.dashValue()), layer.capValue());
                finish();
            }
        } else if (const auto* c = std::get_if<PolylineContent>(&content)) {
            if (c->points.size() >= 2 && begin(layer.contentBounds(), 0)) {
                strokeSegs(dashSegments(pathSegments(c->points, false), layer.dashValue()),
                           layer.capValue());
                finish();
            }
        } else if (const auto* c = std::get_if<PolygonContent>(&content)) {
            if (c->points.size() >= 3 && begin(layer.contentBounds(), 0)) {
                if (th > 0) {
                    strokeSegs(dashSegments(pathSegments(c->points, true), layer.dashValue()),
                               layer.capValue());
                } else if (inv.b == 0 && inv.c == 0 && af.aa > 0 &&
                           !detail::g_cpu_polygon_reference) {
                    rasterizePolygonFill(mask, inv, m, c->points, layer.contentBounds(), pad_px,
                                         aa, target.w, target.h);
                } else {
                    const auto& pts = c->points;
                    part(layer.contentBounds(), 0, [&](vec2 p) {
                        return polygonDistance(pts, p);
                    });
                }
                finish();
            }
        } else if (const auto* c = std::get_if<RectContent>(&content)) {
            if (begin(c->rect, 0)) {
                const vec2 center = gv(c->rect.center());
                const vec2 half{c->rect.w * 0.5f, c->rect.h * 0.5f};
                const float cr = std::min(layer.cornerRadius(),
                                          std::min(half.x, half.y));
                part(c->rect, 0, [&, center, half, cr](vec2 p) {
                    const float d = shapes::sd_rounded_rect(p, center, half, cr);
                    return th > 0 ? shapes::sd_stroke_field(d, th) : d;
                });
                finish();
            }
        } else if (const auto* c = std::get_if<CircleContent>(&content)) {
            if (begin(layer.contentBounds(), 0)) {
                part(layer.contentBounds(), 0, [&](vec2 p) {
                    const float d = shapes::sd_circle(p, gv(c->center), c->radius);
                    return th > 0 ? shapes::sd_stroke_field(d, th) : d;
                });
                finish();
            }
        } else if (const auto* c = std::get_if<CirclesContent>(&content)) {
            if (!c->centers.empty() && begin(layer.contentBounds(), 0)) {
                for (const Vec2& center : c->centers) {
                    const Rect b{center.x - c->radius, center.y - c->radius, 2 * c->radius,
                                 2 * c->radius};
                    part(b, 0, [&, center](vec2 p) {
                        const float d = shapes::sd_circle(p, gv(center), c->radius);
                        return th > 0 ? shapes::sd_stroke_field(d, th) : d;
                    });
                }
                finish();
            }
        } else if (const auto* c = std::get_if<ArcContent>(&content)) {
            if (begin(layer.contentBounds(), 0)) {
                float a0 = c->start_deg * kPi / 180.0f;
                float a1 = c->end_deg * kPi / 180.0f;
                if (a1 < a0) {
                    std::swap(a0, a1);
                }
                a1 = std::min(a1, a0 + 2 * kPi);
                part(layer.contentBounds(), 0, [&, a0, a1](vec2 p) {
                    const float d = shapes::sd_arc(p, gv(c->center), c->radius, a0, a1);
                    return d - th * 0.5f;
                });
                finish();
            }
        } else if (const auto* c = std::get_if<ArrowContent>(&content)) {
            const float len = std::hypot(c->to.x - c->from.x, c->to.y - c->from.y);
            if (len > 1e-4f) {
                const float head =
                    c->head_size > 0 ? c->head_size : std::max(3.0f * th, 9.0f);
                if (begin(rectPad(layer.contentBounds(), head), 0)) {
                    const Vec2 dir{(c->to.x - c->from.x) / len, (c->to.y - c->from.y) / len};
                    const Vec2 neck{c->to.x - dir.x * head, c->to.y - dir.y * head};
                    const Vec2 side{-dir.y * head * 0.5f, dir.x * head * 0.5f};
                    const vec2 tip = gv(c->to);
                    const vec2 w0 = gv({neck.x + side.x, neck.y + side.y});
                    const vec2 w1 = gv({neck.x - side.x, neck.y - side.y});
                    strokeSegs({{c->from, neck}}, layer.capValue());
                    part(rectPad(segBounds({neck, c->to}), head), 0, [&, tip, w0, w1](vec2 p) {
                        return shapes::sd_triangle(p, tip, w0, w1);
                    });
                    finish();
                }
            }
        } else if (const auto* c = std::get_if<CrosshairContent>(&content)) {
            if (begin(layer.contentBounds(), 0)) {
                const float gap = c->gap > 0 ? c->gap : c->size * 0.25f;
                const Vec2 o = c->center;
                const Seg ticks[4] = {{{o.x + gap, o.y}, {o.x + c->size, o.y}},
                                      {{o.x - gap, o.y}, {o.x - c->size, o.y}},
                                      {{o.x, o.y + gap}, {o.x, o.y + c->size}},
                                      {{o.x, o.y - gap}, {o.x, o.y - c->size}}};
                strokeSegs({ticks[0], ticks[1], ticks[2], ticks[3]}, layer.capValue());
                finish();
            }
        } else if (const auto* c = std::get_if<GridContent>(&content)) {
            if (begin(bounds, 0)) {
                const vec2 center = gv(bounds.center());
                const vec2 half{bounds.w * 0.5f, bounds.h * 0.5f};
                const float spacing = c->spacing;
                rasterizePart(mask, inv, m, bounds, pad_px, target.w, target.h,
                              [&, center, half, spacing](vec2 p) {
                    const float lines = af.cover(p, [&](vec2 q) {
                        return shapes::sd_stroke_field(shapes::sd_grid(q, spacing), th);
                    });
                    const float inside = af.cover(p, [&](vec2 q) {
                        return shapes::sd_rounded_rect(q, center, half, 0.0f);
                    });
                    return lines * inside;
                });
                finish();
            }
        } else if (const auto* c = std::get_if<BoxesContent>(&content)) {
            if (!c->smoothed.empty() && begin(rectPad(layer.contentBounds(), 20.0f), 0)) {
                for (const Box& box : c->smoothed) {
                    const vec2 center = gv(box.rect.center());
                    const vec2 half{box.rect.w * 0.5f, box.rect.h * 0.5f};
                    const float cr = std::min(layer.cornerRadius(),
                                              std::min(half.x, half.y));
                    part(box.rect, 0, [&, center, half, cr](vec2 p) {
                        const float d = shapes::sd_rounded_rect(p, center, half, cr);
                        return th > 0 ? shapes::sd_stroke_field(d, th) : d;
                    });
                }
                finish();
                if (c->show_label) {
                    for (const Box& box : c->smoothed) {
                        std::string label = box.label;
                        if (box.score > 0) {
                            char score[16];
                            std::snprintf(score, sizeof score, "%.2f", box.score);
                            label += (label.empty() ? "" : " ") + std::string(score);
                        }
                        if (label.empty()) {
                            continue;
                        }
                        TextContent t;
                        t.utf8 = label;
                        t.size = 14;
                        t.position = {box.rect.x + 2, std::max(box.rect.y - 18.0f, 0.0f)};
                        drawText(target, t, m, inv, color, alpha, mode, scale);
                    }
                }
            }
        }
    }

    void drawImage(Buf& target, const ImageContent& c, Rect bounds, const Mat23& m,
                   const Mat23& inv, float alpha, Blend mode) {
        if (!c.view.valid() || bounds.w <= 0 || bounds.h <= 0) {
            return;
        }
        // Source window (sourceRect crop, clamped to the image).
        Rect src{0, 0, static_cast<float>(c.view.width), static_cast<float>(c.view.height)};
        if (c.source_rect.w > 0 && c.source_rect.h > 0) {
            src = rectIntersect(c.source_rect, src);
        }
        if (src.w <= 0 || src.h <= 0) {
            return;
        }
        // Fit: dest rect inside bounds, and the matching source window.
        Rect dest = bounds;
        if (c.fit == Fit::Contain) {
            const float s = std::min(bounds.w / src.w, bounds.h / src.h);
            dest = {bounds.x + (bounds.w - src.w * s) * 0.5f,
                    bounds.y + (bounds.h - src.h * s) * 0.5f, src.w * s, src.h * s};
        } else if (c.fit == Fit::Cover) {
            const float s = std::max(bounds.w / src.w, bounds.h / src.h);
            const float vis_w = bounds.w / s;
            const float vis_h = bounds.h / s;
            src = {src.x + (src.w - vis_w) * 0.5f, src.y + (src.h - vis_h) * 0.5f, vis_w, vis_h};
        }
        const IRect box = targetBBox(m, dest, 1, target.w, target.h);
        if (box.empty()) {
            return;
        }
        const uint32_t stride = c.view.stride();
        const auto texel = [&](int sx, int sy, float out[4]) {
            sx = std::clamp(sx, static_cast<int>(src.x),
                            static_cast<int>(src.x + src.w) - 1);
            sy = std::clamp(sy, static_cast<int>(src.y),
                            static_cast<int>(src.y + src.h) - 1);
            const uint8_t* p = c.view.pixels + static_cast<size_t>(sy) * stride + sx * 4;
            const float a = p[3] / 255.0f;
            out[0] = p[0] / 255.0f * a;
            out[1] = p[1] / 255.0f * a;
            out[2] = p[2] / 255.0f * a;
            out[3] = a;
        };
        // The dest rect's edges are antialiased like any shape (a rotated
        // or sub-pixel-placed image has no stair-steps); pixel-aligned
        // edges come out exactly as a hard clip would.
        const AaFrame af(m, inv);
        const vec2 dest_center = gv(dest.center());
        const vec2 dest_half{dest.w * 0.5f, dest.h * 0.5f};
        for (int y = box.y0; y < box.y1; ++y) {
            for (int x = box.x0; x < box.x1; ++x) {
                const Vec2 local = inv.apply({x + 0.5f, y + 0.5f});
                const float edge = shapes::sd_rounded_rect_cov(gv(local), dest_center, dest_half,
                                                               0.0f, 0.0f, af.aa, af.hx, af.hy);
                if (edge <= 0) {
                    continue;
                }
                const float sxf = src.x + (local.x - dest.x) * (src.w / dest.w) - 0.5f;
                const float syf = src.y + (local.y - dest.y) * (src.h / dest.h) - 0.5f;
                const int sx0 = static_cast<int>(std::floor(sxf));
                const int sy0 = static_cast<int>(std::floor(syf));
                const float tx = sxf - sx0;
                const float ty = syf - sy0;
                float acc[4] = {0, 0, 0, 0};
                float t00[4], t10[4], t01[4], t11[4];
                texel(sx0, sy0, t00);
                texel(sx0 + 1, sy0, t10);
                texel(sx0, sy0 + 1, t01);
                texel(sx0 + 1, sy0 + 1, t11);
                for (int ch = 0; ch < 4; ++ch) {
                    acc[ch] = (t00[ch] * (1 - tx) + t10[ch] * tx) * (1 - ty) +
                              (t01[ch] * (1 - tx) + t11[ch] * tx) * ty;
                    acc[ch] *= alpha * edge;
                }
                blendPremul(target.at(x, y), acc, mode);
            }
        }
    }

    // Text: per paint group, glyph coverage (fill, outer) into two masks
    // with MAX union, then one premultiplied composite — the same two
    // steps as glyph_mask.frag + text_composite.frag (text_shared.h).
    void drawText(Buf& target, const TextContent& c, const Mat23& m, const Mat23& inv,
                  Color color, float alpha, Blend mode, float scale) {
        (void)scale;
        ensureText();
        const detail::TextEngine::Draw& draw = text.build(c, m, color);
        if (draw.groups.empty()) {
            return;
        }
        detail::GlyphAtlas& atlas = text.atlas();
        const bool outlined = draw.outline.a > 0;
        const float outline_luma =
            textfx::text_luma(glsl::vec3(draw.outline.r, draw.outline.g, draw.outline.b));
        for (const detail::TextEngine::Group& group : draw.groups) {
            Rect gb{};
            for (const auto& q : group.quads) {
                gb = gb.w > 0 ? plan::rectUnion(gb, q.local) : q.local;
            }
            Mask fill_mask, outer_mask;
            fill_mask.reset(targetBBox(m, gb, 1, target.w, target.h));
            if (fill_mask.box.empty()) {
                continue;
            }
            if (outlined) {
                outer_mask.reset(fill_mask.box);
            }
            for (const auto& q : group.quads) {
                IRect box = targetBBox(m, q.local, 1, target.w, target.h);
                box.x0 = std::max(box.x0, fill_mask.box.x0);
                box.y0 = std::max(box.y0, fill_mask.box.y0);
                box.x1 = std::min(box.x1, fill_mask.box.x1);
                box.y1 = std::min(box.y1, fill_mask.box.y1);
                if (box.empty()) {
                    continue;
                }
                const auto& page = atlas.page(q.page);
                const int ps = static_cast<int>(page.size);
                const uint8_t* px = page.px.data();
                const auto tex = [&](int xx, int yy) {
                    return px[static_cast<size_t>(yy) * ps + xx] / 255.0f;
                };
                // Bilinear R8 page sample (clamp to edge) at a local point,
                // clamped into the glyph's tile.
                const auto sample = [&](Vec2 lp) {
                    const float fx = std::clamp((lp.x - q.local.x) / q.local.w, 0.0f, 1.0f);
                    const float fy = std::clamp((lp.y - q.local.y) / q.local.h, 0.0f, 1.0f);
                    const float ax = (q.uv[0] + fx * (q.uv[2] - q.uv[0])) * ps - 0.5f;
                    const float ay = (q.uv[1] + fy * (q.uv[3] - q.uv[1])) * ps - 0.5f;
                    const float fx0 = std::floor(ax), fy0 = std::floor(ay);
                    const float tx = ax - fx0, ty = ay - fy0;
                    const int ax0 = std::clamp(static_cast<int>(fx0), 0, ps - 1);
                    const int ay0 = std::clamp(static_cast<int>(fy0), 0, ps - 1);
                    const int ax1 = std::clamp(static_cast<int>(fx0) + 1, 0, ps - 1);
                    const int ay1 = std::clamp(static_cast<int>(fy0) + 1, 0, ps - 1);
                    return (tex(ax0, ay0) * (1 - tx) + tex(ax1, ay0) * tx) * (1 - ty) +
                           (tex(ax0, ay1) * (1 - tx) + tex(ax1, ay1) * tx) * ty;
                };
                const Vec2 hx{inv.a * 0.5f, inv.b * 0.5f}, hy{inv.c * 0.5f, inv.d * 0.5f};
                const float max_dilate = std::max(std::fabs(q.dilate_fill), std::fabs(q.dilate_outer));
                for (int y = box.y0; y < box.y1; ++y) {
                    fill_mask.touch(y, box.x0, box.x1);
                    if (outlined) {
                        outer_mask.touch(y, box.x0, box.x1);
                    }
                    for (int x = box.x0; x < box.x1; ++x) {
                        const Vec2 local = inv.apply({x + 0.5f, y + 0.5f});
                        const float fx = (local.x - q.local.x) / q.local.w;
                        const float fy = (local.y - q.local.y) / q.local.h;
                        if (fx < 0 || fy < 0 || fx >= 1 || fy >= 1) {
                            continue;
                        }
                        const float v = sample(local);
                        float range = q.px_range;
                        if (range < 0) {  // anisotropic (text_shared.h)
                            range = -range;
                            if (!textfx::text_aniso_far(v, range, max_dilate)) {
                                range = textfx::text_aniso_range(
                                    v, sample({local.x + hx.x, local.y + hx.y}),
                                    sample({local.x - hx.x, local.y - hx.y}),
                                    sample({local.x + hy.x, local.y + hy.y}),
                                    sample({local.x - hy.x, local.y - hy.y}), range);
                            }
                        }
                        const float f =
                            textfx::text_coverage(v, q.mode, range, q.dilate_fill);
                        if (f > 0) {
                            float* mp = fill_mask.at(x, y);
                            *mp = std::max(*mp, f);
                        }
                        if (outlined) {
                            const float o =
                                textfx::text_coverage(v, q.mode, range, q.dilate_outer);
                            if (o > 0) {
                                float* op = outer_mask.at(x, y);
                                *op = std::max(*op, o);
                            }
                        }
                    }
                }
            }
            compositeText(target, fill_mask, outlined ? &outer_mask : nullptr, group, draw,
                          outline_luma, inv, alpha, mode);
        }
    }

    void compositeText(Buf& target, const Mask& fill_mask, const Mask* outer_mask,
                       const detail::TextEngine::Group& group,
                       const detail::TextEngine::Draw& draw, float outline_luma, const Mat23& inv,
                       float alpha, Blend mode) {
        const Color fill = group.fill;
        const int stride = fill_mask.box.x1 - fill_mask.box.x0;
        const Color outline = draw.outline;
        for (int y = fill_mask.box.y0; y < fill_mask.box.y1; ++y) {
            const size_t row = static_cast<size_t>(y - fill_mask.box.y0);
            const int x0 = outer_mask != nullptr
                               ? std::min(fill_mask.span_x0[row], outer_mask->span_x0[row])
                               : fill_mask.span_x0[row];
            const int x1 = outer_mask != nullptr
                               ? std::max(fill_mask.span_x1[row], outer_mask->span_x1[row])
                               : fill_mask.span_x1[row];
            for (int x = x0; x < x1; ++x) {
                const bool in_fill = x >= fill_mask.span_x0[row] && x < fill_mask.span_x1[row];
                float f = in_fill ? fill_mask.cov[row * stride + (x - fill_mask.box.x0)] : 0.0f;
                float o = f;
                if (outer_mask != nullptr && x >= outer_mask->span_x0[row] &&
                    x < outer_mask->span_x1[row]) {
                    o = std::max(f, outer_mask->cov[row * stride + (x - fill_mask.box.x0)]);
                }
                if (o <= 0) {
                    continue;
                }
                Color fc = fill;
                if (group.gradient) {
                    const Vec2 lp = inv.apply({x + 0.5f, y + 0.5f});
                    const float t = textfx::text_gradient_t(
                        glsl::vec2(lp.x, lp.y),
                        glsl::vec2(draw.gradient_origin.x, draw.gradient_origin.y),
                        glsl::vec2(draw.gradient_axis.x, draw.gradient_axis.y));
                    const Color& to = draw.gradient_to;
                    fc = {fill.r + (to.r - fill.r) * t, fill.g + (to.g - fill.g) * t,
                          fill.b + (to.b - fill.b) * t, fill.a + (to.a - fill.a) * t};
                }
                const float fl = textfx::text_luma(glsl::vec3(fc.r, fc.g, fc.b));
                const float fcov = textfx::text_contrast(f, fl);
                float premul[4];
                if (outer_mask != nullptr) {
                    const float ocov = std::max(fcov, textfx::text_contrast(o, outline_luma));
                    const float fa = fc.a * fcov * alpha;
                    const float oa = outline.a * (ocov - fcov) * alpha;
                    premul[0] = fc.r * fa + outline.r * oa;
                    premul[1] = fc.g * fa + outline.g * oa;
                    premul[2] = fc.b * fa + outline.b * oa;
                    premul[3] = fa + oa;
                } else {
                    const float fa = fc.a * fcov * alpha;
                    premul[0] = fc.r * fa;
                    premul[1] = fc.g * fa;
                    premul[2] = fc.b * fa;
                    premul[3] = fa;
                }
                blendPremul(target.at(x, y), premul, mode);
            }
        }
    }

    // ---- background / border ---------------------------------------------

    void drawRoundedRect(Buf& target, Rect r, float corner, float stroke_w, const Mat23& m,
                         Color color, float alpha, Blend mode) {
        if (r.w <= 0 || r.h <= 0 || color.a <= 0 || alpha <= 0) {
            return;
        }
        const Mat23 inv = m.inverse();
        const float scale = scaleOf(m);
        if (scale <= 0) {
            return;
        }
        const AaFrame af(m, inv);
        const vec2 center = gv(r.center());
        const vec2 half{r.w * 0.5f, r.h * 0.5f};
        const float cr = std::min(corner, std::min(half.x, half.y));
        const IRect box =
            targetBBox(m, r, (stroke_w + 2) * plan::maxScaleOf(m), target.w, target.h);
        for (int y = box.y0; y < box.y1; ++y) {
            for (int x = box.x0; x < box.x1; ++x) {
                const Vec2 lp = inv.apply({x + 0.5f, y + 0.5f});
                const float cov = shapes::sd_rounded_rect_cov(gv(lp), center, half, cr, stroke_w,
                                                              af.aa, af.hx, af.hy);
                if (cov > 0) {
                    blendPixel(target.at(x, y), color, alpha * cov, mode);
                }
            }
        }
    }

    // ---- layer recursion ---------------------------------------------------

    void renderLayer(Buf& target, const Layer& layer, const Mat23& parent_m, Vec2 parent_size) {
        if (layer.hidden()) {
            return;
        }
        const float opacity = layer.presentedOpacity();
        if (opacity <= 0) {
            return;
        }
        const Layer::Resolved r = layer.resolve(parent_size);
        const Mat23 m = parent_m * r.to_parent;

        const bool needs_offscreen = plan::needsOffscreen(layer, opacity);

        if (!needs_offscreen) {
            const Blend mode = layer.blendMode();
            drawRoundedRect(target, r.bounds, layer.cornerRadius(), 0, m,
                            layer.presentedBackground(), opacity, mode);
            drawContent(target, layer, r.bounds, m, opacity);
            for (const auto& child : layer.sublayers()) {
                renderLayer(target, *child, m, r.bounds.size());
            }
            if (const std::optional<Border> border = layer.presentedBorder()) {
                drawRoundedRect(target, r.bounds, layer.cornerRadius(),
                                border->width, m, border->color,
                                opacity, mode);
            }
            return;
        }

        // ---- offscreen path ----
        const float scale = scaleOf(m);
        Rect ext = plan::subtreeExtent(layer, parent_size, measurer());
        if (ext.w <= 0 || ext.h <= 0 || scale <= 0) {
            return;
        }
        ext = rectPad(ext, 2.0f / scale);
        const int bw = std::max(1, static_cast<int>(std::ceil(ext.w * scale)));
        const int bh = std::max(1, static_cast<int>(std::ceil(ext.h * scale)));
        if (static_cast<int64_t>(bw) * bh > 64LL * 1024 * 1024) {
            return;  // degenerate transform; refuse absurd allocations
        }
        Buf buf;
        buf.reset(bw, bh);
        // Local space → buffer: axis-aligned, at target density. The outer
        // rotation/position happens at composite time (bilinear), which is
        // exactly the §4.1-5 contract for filtered/shadowed subtrees.
        const Mat23 mb =
            Mat23::scaling({scale, scale}) * Mat23::translation({-ext.x, -ext.y});

        drawRoundedRect(buf, r.bounds, layer.cornerRadius(), 0, mb, layer.presentedBackground(),
                        1.0f, Blend::Normal);
        drawContent(buf, layer, r.bounds, mb, 1.0f);
        for (const auto& child : layer.sublayers()) {
            renderLayer(buf, *child, mb, r.bounds.size());
        }
        if (const std::optional<Border> border = layer.presentedBorder()) {
            drawRoundedRect(buf, r.bounds, layer.cornerRadius(), border->width,
                            mb, border->color, 1.0f, Blend::Normal);
        }

        for (const Filter& f : layer.filters()) {
            FilterStatePair* st = nullptr;
            const FilterSpec* fspec = findFilterSpec(f.mode);
            if (fspec != nullptr && fspec->stateful) {
                FilterStatePair& entry =
                    filter_states[{static_cast<const void*>(&layer), f.mode}];
                entry.stamp = frame_stamp;
                st = &entry;
            }
            applyFilter(buf, f, scale, ext, st);
        }
        if (layer.maskValue().valid()) {
            applyLayerMask(buf, layer, scale, ext, r.bounds);
        }

        const Mat23 inv = m.inverse();
        const vec2 mask_center = gv(r.bounds.center());
        const vec2 mask_half{r.bounds.w * 0.5f, r.bounds.h * 0.5f};
        const float mask_cr =
            std::min(layer.cornerRadius(), std::min(mask_half.x, mask_half.y));
        const AaFrame af(m, inv);

        if (const auto& sh = layer.shadowValue()) {
            // Silhouette → blur → tint, composited under the body.
            Buf shadow_buf;
            shadow_buf.reset(bw, bh);
            for (size_t i = 3; i < buf.px.size(); i += 4) {
                shadow_buf.px[i] = buf.px[i];
            }
            gaussianBlur(shadow_buf, sh->radius * scale);
            const IRect box = targetBBox(
                m, rectPad({ext.x + sh->offset.x, ext.y + sh->offset.y, ext.w, ext.h},
                           sh->radius * 2),
                2, target.w, target.h);
            // blendPixel multiplies in sh->color.a itself.
            const float shadow_alpha = sh->opacity * opacity;
            for (int y = box.y0; y < box.y1; ++y) {
                for (int x = box.x0; x < box.x1; ++x) {
                    const Vec2 local = inv.apply({x + 0.5f, y + 0.5f});
                    float smp[4];
                    sampleBilinear(shadow_buf, (local.x - sh->offset.x - ext.x) * scale,
                                   (local.y - sh->offset.y - ext.y) * scale, smp);
                    if (smp[3] > 0) {
                        blendPixel(target.at(x, y), sh->color, shadow_alpha * smp[3],
                                   Blend::Normal);
                    }
                }
            }
        }

        const IRect box = targetBBox(m, ext, 2, target.w, target.h);
        for (int y = box.y0; y < box.y1; ++y) {
            for (int x = box.x0; x < box.x1; ++x) {
                const Vec2 local = inv.apply({x + 0.5f, y + 0.5f});
                float smp[4];
                sampleBilinear(buf, (local.x - ext.x) * scale, (local.y - ext.y) * scale, smp);
                if (smp[3] <= 0) {
                    continue;
                }
                float a = opacity;
                if (layer.masksToBounds()) {
                    a *= shapes::sd_rounded_rect_cov(gv(local), mask_center, mask_half, mask_cr,
                                                     0.0f, af.aa, af.hx, af.hy);
                    if (a <= 0) {
                        continue;
                    }
                }
                for (int ch = 0; ch < 4; ++ch) {
                    smp[ch] *= a;
                }
                blendPremul(target.at(x, y), smp, layer.blendMode());
            }
        }
    }

    // ---- frame ------------------------------------------------------------

    const Surface& renderFrame(Stage& stage, uint32_t out_w, uint32_t out_h, float dt) {
        stage.setTextMeasurer([this](const TextContent& c) { return measureText(c); });
        stage.advance(dt);

        ++frame_stamp;
        text.beginFrame();
        frame.reset(static_cast<int>(out_w), static_cast<int>(out_h));

        // Logical canvas → output mapping (§4: Stage fit policy).
        const float W = stage.width();
        const float H = stage.height();
        float sx = out_w / W;
        float sy = out_h / H;
        if (stage.fit() == Fit::Contain) {
            sx = sy = std::min(sx, sy);
        } else if (stage.fit() == Fit::Cover) {
            sx = sy = std::max(sx, sy);
        }
        const Mat23 m0 = Mat23::translation({(out_w - W * sx) * 0.5f, (out_h - H * sy) * 0.5f}) *
                         Mat23::scaling({sx, sy});

        renderLayer(frame, stage.root(), m0, {W, H});

        // Premultiplied float → straight RGBA8.
        out_rgba.resize(static_cast<size_t>(out_w) * out_h * 4);
        for (size_t i = 0; i < out_rgba.size(); i += 4) {
            const float* p = frame.px.data() + i;
            const float a = std::clamp(p[3], 0.0f, 1.0f);
            const float inv_a = a > 0 ? 1.0f / a : 0.0f;
            out_rgba[i + 0] = static_cast<uint8_t>(std::clamp(p[0] * inv_a, 0.0f, 1.0f) * 255.0f + 0.5f);
            out_rgba[i + 1] = static_cast<uint8_t>(std::clamp(p[1] * inv_a, 0.0f, 1.0f) * 255.0f + 0.5f);
            out_rgba[i + 2] = static_cast<uint8_t>(std::clamp(p[2] * inv_a, 0.0f, 1.0f) * 255.0f + 0.5f);
            out_rgba[i + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
        surface.width = out_w;
        surface.height = out_h;
        surface.strideBytes = static_cast<size_t>(out_w) * 4;
        surface.pixels = out_rgba.data();
        // Drop state for layers that no longer render (removed, or their
        // stateful filter cleared) — the doc's ownership rule.
        for (auto it = filter_states.begin(); it != filter_states.end();) {
            it = it->second.stamp == frame_stamp ? std::next(it)
                                                 : filter_states.erase(it);
        }
        return surface;
    }
};

// ===========================================================================
// CpuRenderer
// ===========================================================================

CpuRenderer::CpuRenderer() : CpuRenderer(Options{}) {}

CpuRenderer::CpuRenderer(Options options) : impl_(new Impl) { impl_->options = std::move(options); }

CpuRenderer::~CpuRenderer() = default;

const Surface& CpuRenderer::render(Stage& stage, float dt) {
    return impl_->renderFrame(stage,
                              static_cast<uint32_t>(std::lround(stage.width())),
                              static_cast<uint32_t>(std::lround(stage.height())), dt);
}

const Surface& CpuRenderer::render(Stage& stage, uint32_t out_width, uint32_t out_height,
                                   float dt) {
    return impl_->renderFrame(stage, out_width, out_height, dt);
}

}  // namespace fluent_scene
