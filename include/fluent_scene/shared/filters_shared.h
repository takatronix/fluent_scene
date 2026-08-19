// filters_shared.h — the single source of truth for every single-pass filter
// BODY. Each filter is ONE named function with NAMED parameters, written in
// the GLSL∩C++ subset:
//
//   - the Vulkan backend (Phase L1) #includes this (glslc -I) and the GPU
//     runs these exact bodies;
//   - the CPU reference includes it through shared/glsl_compat.hpp and runs
//     the very same bodies.
//
// Its sibling filters_def.h is the single source of the filter *metadata*
// (public names, parameter names/defaults/units); the parameter order
// declared there IS the slot order p0..p4 used by fs_apply below. Adding a
// filter = one function + one dispatch line here + one block in
// filters_def.h. Mode ids never appear anywhere else.
//
// The includer must provide:
//   FS_SAMPLE(uv)  -> vec3   sample the source image at normalized uv
//   FS_TEXEL       -> vec2   1 / source size
// Function bodies are only compiled when FS_SAMPLE is defined, so metadata
// consumers can include this header without a sampler.
//
// Filters that read a second image (the `lut` grade atlas) additionally
// need:
//   FS_SAMPLE_LUT(uv) -> vec3   sample the filter's image parameter
// Their bodies and dispatch lines compile only when that macro is defined,
// so a backend without the extra sampler treats the mode as a pass-through
// until it grows one.

// ---- stable mode ids (shared by GLSL and C++) ------------------------------
const int FS_GRAYSCALE = 1;
const int FS_SEPIA = 2;
const int FS_INVERT = 3;
const int FS_HUE = 4;
const int FS_EXPOSURE = 5;
const int FS_WHITE_BALANCE = 6;
const int FS_LEVELS = 7;
const int FS_THRESHOLD = 8;
const int FS_POSTERIZE = 9;
const int FS_VIGNETTE = 10;
const int FS_PIXELATE = 11;
const int FS_SHARPEN = 12;
const int FS_EMBOSS = 13;
const int FS_EDGE_SOBEL = 14;
const int FS_SKETCH = 15;
const int FS_TOON = 16;
const int FS_SWIRL = 17;
const int FS_SOLARIZE = 18;
const int FS_MOTION_BLUR = 19;
const int FS_ZOOM_BLUR = 20;
const int FS_RGB = 21;
const int FS_OPACITY = 22;
const int FS_HAZE = 23;
const int FS_HALFTONE = 24;
const int FS_COLOR_TRANSFORM = 25;
const int FS_BILATERAL = 26;
const int FS_MEDIAN = 27;
// Separable gaussian blur is multi-pass, so it has a mode id here (the ids
// are the shared vocabulary) but no body in fs_apply — renderers implement
// it as two 1-D passes. Radius is in logical units like every other length.
const int FS_BLUR = 28;
const int FS_RIPPLE = 29;
const int FS_BEAUTY = 30;
const int FS_LSD = 31;
const int FS_LUT = 32;
const int FS_FACE = 33;
const int FS_NOTEBOOK = 34;
const int FS_BOKEH = 35;
const int FS_OILPAINT = 36;
const int FS_NTSC = 37;
const int FS_CRT = 38;
const int FS_FRACTAL = 39;
const int FS_ANIME = 40;
const int FS_WATERCOLOR = 41;
const int FS_SUMIE = 42;
const int FS_IMPRESSIONIST = 43;
const int FS_STAINEDGLASS = 44;
const int FS_PIXELART = 45;
const int FS_WOBBLE = 46;

#ifdef FS_SAMPLE

float fs_luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Soft skin-chroma mask in normalized YCbCr: the skin cluster sits near
// (Cb 0.41, Cr 0.60) across skin tones, while neutral fabric and foliage
// fall well outside — this is what keeps whitening off shirts and walls
// without any face detection. The wide falloff keeps boundaries invisible.
float fs_skin_chroma(vec3 c) {
    float cb = 0.5 - 0.168736 * c.x - 0.331264 * c.y + 0.5 * c.z;
    float cr = 0.5 + 0.5 * c.x - 0.418688 * c.y - 0.081312 * c.z;
    vec2 d = vec2((cb - 0.41) / 0.07, (cr - 0.60) / 0.06);
    return clamp(1.8 - length(d), 0.0, 1.0);
}

float fs_sobel_magnitude(vec2 uv) {
    float tl = fs_luma(FS_SAMPLE(uv + vec2(-FS_TEXEL.x, -FS_TEXEL.y)));
    float tc = fs_luma(FS_SAMPLE(uv + vec2(0.0, -FS_TEXEL.y)));
    float tr = fs_luma(FS_SAMPLE(uv + vec2(FS_TEXEL.x, -FS_TEXEL.y)));
    float ml = fs_luma(FS_SAMPLE(uv + vec2(-FS_TEXEL.x, 0.0)));
    float mr = fs_luma(FS_SAMPLE(uv + vec2(FS_TEXEL.x, 0.0)));
    float bl = fs_luma(FS_SAMPLE(uv + vec2(-FS_TEXEL.x, FS_TEXEL.y)));
    float bc = fs_luma(FS_SAMPLE(uv + vec2(0.0, FS_TEXEL.y)));
    float br = fs_luma(FS_SAMPLE(uv + vec2(FS_TEXEL.x, FS_TEXEL.y)));
    float gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
    float gy = -tl - 2.0 * tc - tr + bl + 2.0 * bc + br;
    return length(vec2(gx, gy));
}

// ---- color filters ---------------------------------------------------------

vec3 fs_grayscale(vec3 c) { return vec3(fs_luma(c)); }

vec3 fs_sepia(vec3 c, float intensity) {
    vec3 sepia = vec3(dot(c, vec3(0.393, 0.769, 0.189)),
                      dot(c, vec3(0.349, 0.686, 0.168)),
                      dot(c, vec3(0.272, 0.534, 0.131)));
    return mix(c, sepia, clamp(intensity, 0.0, 1.0));
}

vec3 fs_invert(vec3 c) { return 1.0 - c; }

vec3 fs_hue_rotate(vec3 c, float angle_degrees) {
    float a = radians(angle_degrees);
    float yy = dot(c, vec3(0.299, 0.587, 0.114));
    float ii = dot(c, vec3(0.596, -0.274, -0.322));
    float qq = dot(c, vec3(0.211, -0.523, 0.312));
    float hue = atan(qq, ii) + a;
    float chroma = length(vec2(ii, qq));
    ii = chroma * cos(hue);
    qq = chroma * sin(hue);
    return vec3(yy + 0.956 * ii + 0.621 * qq,
                yy - 0.272 * ii - 0.647 * qq,
                yy - 1.106 * ii + 1.703 * qq);
}

vec3 fs_exposure(vec3 c, float stops) { return c * exp2(stops); }

vec3 fs_white_balance(vec3 c, float temperature, float tint) {
    return vec3(c.x * (1.0 + 0.25 * temperature),
                c.y * (1.0 + 0.15 * tint),
                c.z * (1.0 - 0.25 * temperature));
}

vec3 fs_levels(vec3 c, float in_min, float in_max, float gamma) {
    c = clamp((c - vec3(in_min)) * (1.0 / max(in_max - in_min, 1e-4)), 0.0, 1.0);
    return pow(c, vec3(1.0 / max(gamma, 0.05)));
}

vec3 fs_threshold(vec3 c, float threshold) { return vec3(step(threshold, fs_luma(c))); }

vec3 fs_posterize(vec3 c, float levels) {
    float n = max(levels, 2.0);
    return floor(c * n + vec3(0.5)) / n;
}

vec3 fs_vignette(vec3 c, vec2 uv, float start, float end) {
    float d = length(uv - vec2(0.5)) * 1.414;
    return c * (1.0 - smoothstep(start, end, d));
}

vec3 fs_solarize(vec3 c, float threshold) {
    return fs_luma(c) < threshold ? c : 1.0 - c;
}

vec3 fs_rgb(vec3 c, float red, float green, float blue) {
    return c * vec3(red, green, blue);
}

vec3 fs_haze(vec3 c, vec2 uv, float distance_, float slope) {
    float d = distance_ + slope * (1.0 - uv.y);
    return clamp((c - vec3(d)) * (1.0 / max(1.0 - d, 0.05)), 0.0, 1.0);
}

vec3 fs_color_transform(vec3 c, float brightness, float contrast, float saturation, float gamma) {
    c = (c - vec3(0.5)) * contrast + vec3(0.5 + brightness);
    c = mix(vec3(fs_luma(c)), c, saturation);
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / max(gamma, 0.05)));
}

// ---- kernel filters --------------------------------------------------------

vec3 fs_sharpen(vec2 uv, float strength) {
    vec3 c = FS_SAMPLE(uv);
    vec3 neighbors = FS_SAMPLE(uv + vec2(FS_TEXEL.x, 0.0)) +
                     FS_SAMPLE(uv - vec2(FS_TEXEL.x, 0.0)) +
                     FS_SAMPLE(uv + vec2(0.0, FS_TEXEL.y)) +
                     FS_SAMPLE(uv - vec2(0.0, FS_TEXEL.y));
    return c * (1.0 + 4.0 * strength) - neighbors * strength;
}

vec3 fs_emboss(vec2 uv, float strength) {
    vec3 d = FS_SAMPLE(uv) - FS_SAMPLE(uv - FS_TEXEL);
    return vec3(0.5 + fs_luma(d) * strength * 4.0);
}

vec3 fs_edge_sobel(vec2 uv, float strength) {
    return vec3(clamp(fs_sobel_magnitude(uv) * strength, 0.0, 1.0));
}

vec3 fs_sketch(vec2 uv, float strength) {
    return vec3(1.0 - clamp(fs_sobel_magnitude(uv) * strength, 0.0, 1.0));
}

vec3 fs_toon(vec2 uv, float levels, float edge_threshold) {
    vec3 c = fs_posterize(FS_SAMPLE(uv), levels);
    return fs_sobel_magnitude(uv) > edge_threshold ? vec3(0.05) : c;
}

// Edge-preserving smoothing: 7x7 sampled grid, weights fall off with both
// spatial distance and color distance. This is the denoiser of choice when
// edges must survive.
vec3 fs_bilateral(vec2 uv, float radius, float sigma_color) {
    vec3 center = FS_SAMPLE(uv);
    float range_norm = 2.0 * max(sigma_color, 0.01) * max(sigma_color, 0.01);
    float grid = max(radius / 3.0, 1.0);
    vec3 accum = vec3(0.0);
    float weight_sum = 0.0;
    for (int j = -3; j <= 3; ++j) {
        for (int i = -3; i <= 3; ++i) {
            vec3 s = FS_SAMPLE(uv + FS_TEXEL * vec2(float(i), float(j)) * grid);
            vec3 d = s - center;
            float w = exp(-float(i * i + j * j) / 4.5) * exp(-dot(d, d) / range_norm);
            accum += s * w;
            weight_sum += w;
        }
    }
    return accum * (1.0 / max(weight_sum, 1e-5));
}

// 3x3 median via a sorting network; kills salt-and-pepper noise.
vec3 fs_median(vec2 uv) {
    vec3 p0 = FS_SAMPLE(uv + FS_TEXEL * vec2(-1.0, -1.0));
    vec3 p1 = FS_SAMPLE(uv + FS_TEXEL * vec2(0.0, -1.0));
    vec3 p2 = FS_SAMPLE(uv + FS_TEXEL * vec2(1.0, -1.0));
    vec3 p3 = FS_SAMPLE(uv + FS_TEXEL * vec2(-1.0, 0.0));
    vec3 p4 = FS_SAMPLE(uv);
    vec3 p5 = FS_SAMPLE(uv + FS_TEXEL * vec2(1.0, 0.0));
    vec3 p6 = FS_SAMPLE(uv + FS_TEXEL * vec2(-1.0, 1.0));
    vec3 p7 = FS_SAMPLE(uv + FS_TEXEL * vec2(0.0, 1.0));
    vec3 p8 = FS_SAMPLE(uv + FS_TEXEL * vec2(1.0, 1.0));
    vec3 t;
#define FS_SORT2(a, b) t = min(a, b); b = max(a, b); a = t;
    FS_SORT2(p1, p2) FS_SORT2(p4, p5) FS_SORT2(p7, p8)
    FS_SORT2(p0, p1) FS_SORT2(p3, p4) FS_SORT2(p6, p7)
    FS_SORT2(p1, p2) FS_SORT2(p4, p5) FS_SORT2(p7, p8)
    FS_SORT2(p0, p3) FS_SORT2(p5, p8) FS_SORT2(p4, p7)
    FS_SORT2(p3, p6) FS_SORT2(p1, p4) FS_SORT2(p2, p5)
    FS_SORT2(p4, p7) FS_SORT2(p4, p2) FS_SORT2(p6, p4)
    FS_SORT2(p4, p2)
#undef FS_SORT2
    return p4;
}

// Beauty ("磨皮"): the gpupixel/FaceBetter smoothing core in a single pass.
// Not a bilateral — a local Wiener filter: blend toward the neighborhood mean
// only where the neighborhood is statistically flat (variance low) AND the
// pixel passes a red-channel skin gate, so pores vanish while edges, hair,
// and eyes survive untouched. No face detection involved. Mean and variance
// come from one sparse 5x5 grid via E[x²]−E[x]²; the 50/0.1 pairing is
// gpupixel's calibration (their delta=7.07 squared).
//
// Where this deliberately goes past gpupixel: `denoise` is a frame-wide
// fine-scale Wiener pass (dense 3×3 at 2-texel pitch, its own variance —
// the wide sparse grid below reads fine grain as structure and barely
// engages on it) that runs FIRST, so the skin gate, sharpen, and whiten
// all work on the cleaned signal instead of re-amplifying grain. `whiten`
// is gated by the skin-chroma mask so brightening stays off clothing and
// background — the original applies it to every pixel. `sharpen` restores
// micro-contrast the blends absorb (4-tap unsharp).
vec3 fs_beauty(vec2 uv, float smoothing, float whiten, float radius, float sharpen_amount,
               float denoise) {
    vec3 c = FS_SAMPLE(uv);
    if (denoise > 0.0) {
        // Small bilateral (9 taps, 2-texel pitch, range σ ≈ 0.1): every tap
        // is weighted by color similarity to the center, so an edge crossing
        // the window is excluded from the average instead of averaged away.
        // (The earlier box-mean + variance gate softened boundaries: right
        // on an edge the gate saw only moderate variance and half-opened.)
        vec3 accum = c;
        float wsum = 1.0;
        for (int j = -1; j <= 1; ++j) {
            for (int i = -1; i <= 1; ++i) {
                if (i == 0 && j == 0) {
                    continue;
                }
                vec3 s = FS_SAMPLE(uv + FS_TEXEL * vec2(float(i), float(j)) * 2.0);
                vec3 d = s - c;
                float w = exp(-dot(d, d) * 50.0);
                accum += s * w;
                wsum += w;
            }
        }
        c = mix(c, accum * (1.0 / wsum), clamp(denoise, 0.0, 1.0));
    }
    float grid = max(radius, 1.0) * 0.5;
    vec3 sum = vec3(0.0);
    vec3 sum_sq = vec3(0.0);
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            vec3 s = FS_SAMPLE(uv + FS_TEXEL * vec2(float(i), float(j)) * grid);
            sum += s;
            sum_sq += s * s;
        }
    }
    vec3 mean = sum * (1.0 / 25.0);
    vec3 variance = max(sum_sq * (1.0 / 25.0) - mean * mean, vec3(0.0));
    float mean_var = 50.0 * (variance.x + variance.y + variance.z) * (1.0 / 3.0);
    float flatness = 0.1 / (mean_var + 0.1);
    // gpupixel's brightness gate × the chroma mask (with a floor, so
    // borderline skin under warm indoor light is not cut off hard): walls,
    // hair, and clothing stop passing as "skin", which is what kept the
    // whole frame from going soft at high strength.
    float skin = clamp((min(c.x, mean.x - 0.1) - 0.2) * 4.0, 0.0, 1.0) *
                 (0.45 + 0.55 * fs_skin_chroma(c));
    float k = flatness * skin * clamp(smoothing, 0.0, 1.0);
    // Max-shift clamp: pore-level corrections (±0.05-ish) pass untouched,
    // but the pull toward the mean can never flatten real shading or eat
    // an eyelid — this is what keeps maximum strength from turning the
    // face into plastic.
    vec3 shift = clamp((mean - c) * clamp(k, 0.0, 1.0), -0.12, 0.12);
    vec3 result = c + shift;
    if (sharpen_amount > 0.0) {
        vec3 neighbors = FS_SAMPLE(uv + vec2(FS_TEXEL.x, 0.0)) +
                         FS_SAMPLE(uv - vec2(FS_TEXEL.x, 0.0)) +
                         FS_SAMPLE(uv + vec2(0.0, FS_TEXEL.y)) +
                         FS_SAMPLE(uv - vec2(0.0, FS_TEXEL.y));
        result += (c - (c + neighbors) * 0.2) * (sharpen_amount * 2.0);
    }
    if (whiten > 0.0) {
        vec3 lifted = clamp((result - vec3(0.0259)) * 1.02657, 0.0, 1.0);
        vec3 bright = pow(lifted, vec3(0.72));
        bright = mix(vec3(fs_luma(bright)), bright, 0.88);
        result = mix(result, bright, clamp(whiten, 0.0, 1.0) * fs_skin_chroma(c));
    }
    return result;
}

#ifdef FS_SAMPLE_LUT
// 3D LUT color grade from a tiled atlas (the GPUImage layout gpupixel and
// this library's baked grades use): an N³ cube stored as a tiles×tiles grid
// of N×N slices, blue picking the slice pair, red/green the in-slice texel.
// `tiles` and `tile_n` are derived by the renderer from the atlas width
// (width = tiles³, so 512 → 8×8 tiles of 64), never authored. Slice blend
// is manual; in-slice r/g interpolation rides on the LUT sampler. `amount`
// mixes the graded color over the source; `skin` scopes it to the
// skin-chroma mask (1 = grade skin only — the whitening use; 0 = the whole
// frame — the film-look use).
vec3 fs_lut(vec2 uv, float amount, float skin, float tiles, float tile_n) {
    vec3 c = clamp(FS_SAMPLE(uv), 0.0, 1.0);
    float atlas = tiles * tile_n;
    float slice = c.z * (tile_n - 1.0);
    float s0 = floor(slice);
    float s1 = min(s0 + 1.0, tile_n - 1.0);
    vec2 inner = vec2(c.x, c.y) * (tile_n - 1.0) + vec2(0.5, 0.5);
    vec2 uv0 = (vec2(s0 - floor(s0 / tiles) * tiles, floor(s0 / tiles)) * tile_n + inner) / atlas;
    vec2 uv1 = (vec2(s1 - floor(s1 / tiles) * tiles, floor(s1 / tiles)) * tile_n + inner) / atlas;
    vec3 graded = mix(FS_SAMPLE_LUT(uv0), FS_SAMPLE_LUT(uv1), slice - s0);
    float gate = mix(1.0, fs_skin_chroma(c), clamp(skin, 0.0, 1.0));
    return mix(c, graded, clamp(amount, 0.0, 1.0) * gate);
}
// Landmark-driven face warp and makeup — gpupixel Phase 2, generalized.
// The image parameter is a 44×1 control strip: each texel packs one value
// as 16-bit fixed point (hi byte in r, lo in g), all coordinates normalized
// to the layer's uv. An analyzer (MediaPipe in the browser, ml-hub on the
// robot) writes the strip per frame; no strip = pass-through, like `lut`.
//
// Strip layout (values, not texels ×4):
//    0-2  right eye: cx, cy, radius      3-5  left eye: cx, cy, radius
//    6-9  mouth: cx, cy, rx, ry         10    reserved
//   11-14 cheeks: Lx, Ly, Rx, Ry        15    cheek radius
//   16-39 six jaw warp pairs: origin.xy, target.xy (slim)
//
// Warping is the classic inverse mapping: eyes magnify by the parabolic
// weight 1-(1-w²)δ (gpupixel's enlargeEye), the jaw pulls toward its
// targets with a linear falloff (curveWarp). Makeup is applied at the
// warped position so it sticks to the face: lips get a multiply rouge in
// a soft ellipse, cheeks a blend toward pink in soft discs. Distances are
// aspect-corrected so circles are circles.
float fs_face_val(float i) {
    vec3 t = FS_SAMPLE_LUT(vec2((i + 0.5) / 44.0, 0.5));
    return (floor(t.x * 255.0 + 0.5) * 256.0 + floor(t.y * 255.0 + 0.5)) / 65535.0;
}
vec2 fs_face_pt(float i) { return vec2(fs_face_val(i), fs_face_val(i + 1.0)); }

vec3 fs_face(vec2 uv, float eye, float slim, float lip, float cheek) {
    float aspect = FS_TEXEL.y / FS_TEXEL.x;   // width / height
    vec2 src = uv;
    if (slim > 0.0) {
        for (int k = 0; k < 6; ++k) {
            float base = 16.0 + float(k) * 4.0;
            vec2 o = fs_face_pt(base);
            vec2 tgt = fs_face_pt(base + 2.0);
            vec2 span = vec2((tgt.x - o.x) * aspect, tgt.y - o.y);
            float reach = length(span) * 3.0;
            if (reach > 1e-4) {
                vec2 rel = vec2((src.x - o.x) * aspect, src.y - o.y);
                float ratio = clamp(1.0 - length(rel) / reach, 0.0, 1.0);
                src = src - (tgt - o) * (slim * 0.6 * ratio);
            }
        }
    }
    if (eye > 0.0) {
        for (int k = 0; k < 2; ++k) {
            vec2 ec = fs_face_pt(float(k) * 3.0);
            float reach = fs_face_val(float(k) * 3.0 + 2.0) * 2.2;
            vec2 rel = vec2((src.x - ec.x) * aspect, src.y - ec.y);
            float d = length(rel);
            if (reach > 1e-4 && d < reach) {
                float w = d / reach;
                src = ec + (src - ec) * (1.0 - (1.0 - w * w) * eye * 0.35);
            }
        }
    }
    vec3 c = FS_SAMPLE(src);
    if (lip > 0.0) {
        vec2 mc = fs_face_pt(6.0);
        float rx = max(fs_face_val(8.0), 1e-4);
        float ry = max(fs_face_val(9.0), 1e-4);
        vec2 rel = vec2((src.x - mc.x) * aspect / rx, (src.y - mc.y) / ry);
        float m = clamp(1.0 - dot(rel, rel), 0.0, 1.0);
        c = mix(c, c * vec3(1.0, 0.5, 0.58), m * m * clamp(lip, 0.0, 1.0) * 0.85);
    }
    if (cheek > 0.0) {
        float r = max(fs_face_val(15.0), 1e-4);
        for (int k = 0; k < 2; ++k) {
            vec2 cc = fs_face_pt(11.0 + float(k) * 2.0);
            vec2 rel = vec2((src.x - cc.x) * aspect, src.y - cc.y);
            float m = clamp(1.0 - length(rel) / r, 0.0, 1.0);
            c = mix(c, c + (vec3(1.0, 0.45, 0.52) - c) * 0.5,
                    m * m * clamp(cheek, 0.0, 1.0) * 0.5);
        }
    }
    return c;
}

#endif  // FS_SAMPLE_LUT

// ---- resampling filters (change where we read, then what we read) ----------

vec3 fs_pixelate(vec2 uv, float block_size) {
    float size = max(block_size, 1.0);
    vec2 px = uv / FS_TEXEL;
    return FS_SAMPLE((floor(px / size) * size + vec2(size * 0.5)) * FS_TEXEL);
}

vec3 fs_swirl(vec2 uv, float radius, float angle) {
    vec2 d = uv - vec2(0.5);
    float r = length(d);
    if (r < radius) {
        float k = (radius - r) / max(radius, 1e-4);
        float theta = k * k * angle;
        float cs = cos(theta);
        float sn = sin(theta);
        d = vec2(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
    }
    return FS_SAMPLE(vec2(0.5) + d);
}

vec3 fs_motion_blur(vec2 uv, float angle_degrees, float distance_px) {
    float a = radians(angle_degrees);
    vec2 stride = vec2(cos(a), sin(a)) * FS_TEXEL * (distance_px / 4.0);
    vec3 accum = vec3(0.0);
    for (int i = -4; i <= 4; ++i) {
        accum += FS_SAMPLE(uv + stride * float(i));
    }
    return accum / 9.0;
}

vec3 fs_zoom_blur(vec2 uv, float strength) {
    vec2 stride = (vec2(0.5) - uv) * (strength / 8.0);
    vec3 accum = vec3(0.0);
    for (int i = 0; i < 9; ++i) {
        accum += FS_SAMPLE(uv + stride * float(i));
    }
    return accum / 9.0;
}

// Lens bokeh: out-of-focus blur whose kernel is the shape of a camera IRIS,
// not a gaussian. Three things make it read as photography instead of blur:
//   - the kernel is a flat (edge-weighted) disc — highlights become discs,
//     they don't melt;
//   - `blades` cuts that disc into the aperture polygon (3 = triangle,
//     6 = hexagon…; below 3 = round iris), `rotation` turns it;
//   - averaging runs on pseudo-HDR values (pow-up, blur, pow-down) so
//     specular points OUTSHINE their surroundings the way real sensor
//     clipping does — without it a white dot averages away to grey mush.
// Classic polar-grid gather (24 directions × 8 rings); no borrowed code.
vec3 fs_bokeh(vec2 uv, float radius_px, float blades, float rotation_degrees,
              float highlight) {
    float h = 1.0 + 3.0 * clamp(highlight, 0.0, 2.0);   // pseudo-HDR exponent
    float n = floor(blades + 0.5);
    float rot = radians(rotation_degrees);
    vec2 clamp_lo = FS_TEXEL * 3.0;
    vec2 clamp_hi = vec2(1.0, 1.0) - FS_TEXEL * 3.0;
    vec3 acc = vec3(0.0, 0.0, 0.0);
    float wsum = 0.0;
    for (int i = 0; i < 24; ++i) {
        for (int j = 1; j <= 8; ++j) {
            // golden-ratio stagger de-phases the rings so the 24 spokes
            // don't line up into gear teeth on the aperture's rim
            float st = float(j) * 0.38197;
            st = st - floor(st);
            float ang = 6.2831853 * ((float(i) + st) / 24.0) + rot;
            // polygon polar radius: how far the iris extends along this
            // spoke. r(θ) = cos(π/n)/cos(θ mod 2π/n − π/n); 1 for a circle
            float apt = 1.0;
            if (n >= 3.0) {
                float seg = 6.2831853 / n;
                float th = ang - rot + 0.5 * seg;   // vertex-up orientation
                th = th - seg * floor(th / seg);
                apt = cos(0.5 * seg) / cos(th - 0.5 * seg);
            }
            vec2 dir = vec2(cos(ang), sin(ang));
            float t = float(j) / 8.0;
            vec2 p = uv + dir * (t * apt * radius_px) * FS_TEXEL;
            vec3 c = FS_SAMPLE(clamp(p, clamp_lo, clamp_hi));
            // r² ring weight = equal AREA per ring — a flat disc, the
            // signature of bokeh (gaussian would weight the center)
            float w = t * t;
            acc += vec3(pow(c.x, h), pow(c.y, h), pow(c.z, h)) * w;
            wsum += w;
        }
    }
    vec3 m = acc / wsum;
    float ih = 1.0 / h;
    return vec3(pow(m.x, ih), pow(m.y, ih), pow(m.z, ih));
}

// A water-surface refraction wave: the sampling position is displaced by a
// damped sinusoid concentrated around an expanding wavefront, so the image
// beneath genuinely bends (this is the real ripple — the classic GL demo,
// not rings drawn on top). The driver animates `radius` outward and decays
// `amplitude`; stacking several instances layers multiple waves.
vec3 fs_ripple(vec2 uv, float center_x, float center_y, float radius, float amplitude,
               float wavelength) {
    vec2 d = (uv - vec2(center_x, center_y)) / FS_TEXEL;  // pixel space
    float r = max(length(d), 1e-3);
    float band = (r - radius) / max(wavelength, 1.0);
    float envelope = exp(-band * band);  // energy lives near the wavefront
    float displacement = sin(band * 6.28318530718) * amplitude * envelope;
    return FS_SAMPLE(uv - (d / r) * displacement * FS_TEXEL);
}

vec3 fs_halftone(vec2 uv, float spacing) {
    float s = max(spacing, 2.0);
    vec2 px = uv / FS_TEXEL;
    vec2 cell = (floor(px / s) + vec2(0.5)) * s;
    float l = fs_luma(FS_SAMPLE(cell * FS_TEXEL));
    float radius = (1.0 - l) * s * 0.6;
    return vec3(step(radius, length(px - cell)));
}

// ---- psychedelia -----------------------------------------------------------

// A tiny value-noise kit for fs_lsd. Fract-free spellings (x - floor(x)) so
// the C++ shim needs nothing new; value noise is continuous everywhere, so
// CPU/GPU rounding drift cannot flip a cell and break golden parity.

float fs_lsd_hash(vec2 p) {
    vec2 q = vec2(p.x * 0.1031, p.y * 0.1030);
    q = q - floor(q);
    float s = dot(q, q + vec2(33.33, 33.33));
    q = q + vec2(s, s);
    float h = q.x * q.y;
    return h - floor(h);
}

float fs_lsd_vnoise(vec2 p) {
    vec2 ip = floor(p);
    vec2 f = p - ip;
    vec2 s = f * f * (vec2(3.0, 3.0) - f * 2.0);
    float a = fs_lsd_hash(ip);
    float b = fs_lsd_hash(ip + vec2(1.0, 0.0));
    float c = fs_lsd_hash(ip + vec2(0.0, 1.0));
    float d = fs_lsd_hash(ip + vec2(1.0, 1.0));
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

// LSD: the serotonergic-psychedelic visual phenomenology, each stage mapped
// to a documented mechanism (the full story with citations lives in the
// LSD report; short version here):
//
//   drifting/breathing — 5-HT2A agonism raises cortical gain and suppresses
//     alpha gating, so texture "breathes": a slow radial wave plus two
//     incommensurate traveling sine fields warp where we read the source.
//   diffraction — dilated pupils fringe bright edges: radial R/B split.
//   acuity + color pop — unfiltered thalamic drive: unsharp + saturation
//     lift + a slow hue "breath".
//   geometry (Klüver 1926; Ermentrout–Cowan 1979; Bressloff 2001) — V1's
//     E/I instability self-organizes patterns, and Varley (2020) measured
//     LSD raising the fractal dimension of cortical activity: structure
//     at every scale at once. Perceptually though, nothing in a trip is a
//     straight line, a right angle, or a perfect circle — so the field
//     here is aperiodic by construction: domain-warped value-noise fBM,
//     advected outward, octave weights breathing, extracted as amorphous
//     multi-scale masses with softly glowing shorelines. Klüver's classes
//     survive as statistics (multi-scale, flowing, foveally biased), not
//     as drawn geometry.
//
// `time` is seconds, driven per frame by the host (fx::Ripple pattern);
// at the default 0 the filter is static and golden-testable. Frame-to-frame
// feedback (tracers/palinopsia/recursive tunneling) cannot live in a
// single-pass body — the demo closes that loop outside (wasm/lsd.html).
vec3 fs_lsd(vec2 uv, float trip, float time_s, float drift, float geometry, float chroma) {
    float k = clamp(trip, 0.0, 1.5);
    float t = time_s;

    // centered coordinates, x in units of image height (isotropic angles);
    // the center is FIXED — the experience always has one, and the eternal
    // expansion radiates from it
    vec2 d0 = uv - vec2(0.5, 0.5);
    vec2 pc = vec2(d0.x * (FS_TEXEL.y / FS_TEXEL.x), d0.y);
    float r = max(length(pc), 1e-4);

    // the hallucination field comes FIRST: it also drives the image warp,
    // so the drift itself is organic — no sinusoidal wallpaper anywhere.
    // ETERNAL-ZOOM FRACTAL — one fBM whose whole octave ladder slides
    // through scale space: rung n sits at scale 2^(n − zf); as zf advances
    // every feature expands, the over-grown coarse rung fades out and a
    // newborn fine rung fades in with fresh content (its integer flight
    // index seeds it). Six rungs span a 64× range of scale, so looking
    // closer keeps revealing structure — and the flight never ends and
    // never repeats. Varley (2020): the trip raises the fractal dimension
    // of cortical activity; this is that, drawn.
    vec2 qw0 = pc * 1.3 + vec2(t * 0.06, 0.0 - t * 0.047);
    float n1 = fs_lsd_vnoise(qw0);
    float n2 = fs_lsd_vnoise(qw0 * 1.9 + vec2(7.7, 3.1));
    vec2 warp = vec2(n1 - 0.5, n2 - 0.5);
    // THE GLOWING BODY — the hallucination geometry is a circle-inversion
    // IFS (the 2D cousin of fs_fractal's folds; design doc §8): each point
    // orbits "invert outside the unit circle, then rotate·scale·shift",
    // and a leaky accumulator of orbit radii is read at three consecutive
    // depths. Folding those three into bands puts R/G/B a breath apart in
    // phase — iridescent fringes along every equal-orbit contour, nothing
    // straight, nothing repeated. The fold angle, scale and shift ride
    // slow incommensurate clocks: same contract, pure function of `time`.
    float amt = abs(geometry);
    float digital = geometry < 0.0 ? 1.0 : 0.0;            // sign picks texture
    vec2 vp = pc * (3.3 + 0.4 * sin(t * 0.011 + 2.7)) + warp * 0.45;
    vp = vp + vec2(0.13 * sin(t * 0.023), 0.13 * cos(t * 0.029));
    float ifsa = 0.185 * (sin(t * 0.021) + sin(t * 0.034 + 2.0) + sin(t * 0.0079 + 4.0));
    float ca2 = 1.34 * cos(ifsa);
    float sa2 = 1.34 * sin(ifsa);
    float zsc = 0.63 + 0.08 * sin(t * 0.013 + 1.0);
    vec2 sh = vec2(0.033 + 0.022 * sin(t * 0.017), 0.14 + 0.022 * cos(t * 0.019));
    float R0 = 0.0;
    float R1 = 0.0;
    float R2 = 0.0;
    for (int i = 0; i < 32; ++i) {
        float rr = dot(vp, vp);
        if (rr > 1.0) { vp = vp / rr; }
        R0 = R0 * 0.99 + rr;
        if (i < 31) { R1 = R1 * 0.99 + rr; }
        if (i < 30) { R2 = R2 * 0.99 + rr; }
        vp = vec2(ca2 * vp.x + sa2 * vp.y, ca2 * vp.y - sa2 * vp.x) * zsc + sh;
    }
    float w0 = R0 * 0.5 - floor(R0 * 0.5);
    float w1 = R1 * 0.5 - floor(R1 * 0.5);
    float w2 = R2 * 0.5 - floor(R2 * 0.5);
    // iridescence is a rim phenomenon, not a paint job: collapse the three
    // depth-bands mostly onto their shared luminance and keep only 35% of
    // the channel separation — pearl with rainbow edges, never solid
    // magenta fields
    vec3 accRaw = vec3(1.0 - abs(2.0 * w2 - 1.0), 1.0 - abs(2.0 * w1 - 1.0),
                       1.0 - abs(2.0 * w0 - 1.0));
    float accL = (accRaw.x + accRaw.y + accRaw.z) * 0.3333;
    vec3 acc = vec3(accL, accL, accL) + (accRaw - vec3(accL, accL, accL)) * 0.35;

    // drifting & breathing: read the source through the noise field's lens.
    // Less than one radial period fits the view and the phase is noised,
    // so the swell never reads as concentric rings
    float breath = 1.0 + 0.045 * k * drift * sin(t * 1.3 - r * 1.2 + n1 * 2.0);
    vec2 flow = vec2(n1 - 0.5, n2 - 0.5) * 2.0;
    // displacement form: zero at the (wandering) center, so the frame sways
    // and breathes without translating wholesale
    vec2 uvd = uv + d0 * (breath - 1.0) + flow * (0.012 * k * drift);
    // the warp must not leave the CONTENT — the offscreen carries a 2-pixel
    // transparent pad (render_shared rectPad), and sampling it composites as
    // a black fringe; 3 texels keeps the unsharp taps clear of it too
    vec2 clamp_lo = FS_TEXEL * 3.0;
    vec2 clamp_hi = vec2(1.0, 1.0) - FS_TEXEL * 3.0;
    uvd = clamp(uvd, clamp_lo, clamp_hi);

    // diffraction: radial chromatic fringing, growing toward the periphery
    // (the split samples are clamped too, or the frame edge fringes blue)
    vec2 dir = d0 / max(length(d0), 1e-4);
    vec2 ca = dir * (0.0045 * k * chroma * (0.25 + r));
    vec2 uvr = clamp(uvd + ca, clamp_lo, clamp_hi);
    vec2 uvb = clamp(uvd - ca, clamp_lo, clamp_hi);
    vec3 col = vec3(FS_SAMPLE(uvr).x, FS_SAMPLE(uvd).y, FS_SAMPLE(uvb).z);

    // acuity: 4-tap unsharp, then color enhancement + slow hue breathing
    vec3 nb = FS_SAMPLE(uvd + vec2(FS_TEXEL.x, 0.0)) + FS_SAMPLE(uvd - vec2(FS_TEXEL.x, 0.0)) +
              FS_SAMPLE(uvd + vec2(0.0, FS_TEXEL.y)) + FS_SAMPLE(uvd - vec2(0.0, FS_TEXEL.y));
    col += (col - (col + nb) * 0.2) * (0.5 * k);
    col = fs_color_transform(col, 0.0, 1.0 + 0.15 * k, 1.0 + 0.85 * k, 1.0);
    col = fs_hue_rotate(col, 28.0 * k * sin(t * 0.31));

    // organic <-> digital (the SIGN of `geometry`): digital quantizes the
    // band into terraces and hardens every edge; organic leaves it smooth
    float fL = accL;
    if (digital > 0.5) {
        fL = floor(fL * 7.0 + 0.5) * (1.0 / 7.0);
    }
    // ignition happens on the shared band's crest only — sparse filigree.
    // The body of the lace is pearl; the three depth-bands survive as a
    // faint iridescent blur on its rim, never as solid colour fields.
    float band = smoothstep(0.38 - 0.04 * digital, 0.66 - 0.07 * digital, fL);
    vec3 gm = clamp(vec3(0.82, 0.80, 0.85) + (accRaw - vec3(accL, accL, accL)) * 1.3,
                    0.0, 1.0) * band;
    // rides on weak retinal drive, densest at the fixed center
    float gate = clamp(amt, 0.0, 1.5) * k * (0.25 + 0.75 * (1.0 - fs_luma(col)));
    float focus = 0.65 + 0.45 * (1.0 - smoothstep(0.05, 0.95, r));
    gm = gm * (gate * focus);
    float cover = max(gm.x, max(gm.y, gm.z));
    col = col * (1.0 - 0.45 * cover) + gm * 0.95;
    return col;
}

// Oil painting: the Kuwahara filter (Kuwahara et al., 1976 — a classic,
// no borrowed shader code). Around each pixel four overlapping quadrant
// windows are measured; the pixel takes the MEAN of the LEAST-VARIANT
// quadrant. Averaging never crosses an edge (the variant quadrants lose),
// so regions flatten into brush daubs while contours stay crisp — the
// painterly look. Two strokes of style on top:
//   - `jitter` rotates each pixel's quadrant frame by a smooth noise angle,
//     breaking the axis-aligned boxiness into wandering brush direction;
//   - `levels` posterizes the result per channel — dabs of mixed paint
//     rather than continuous gradients (0 = off).
// Like fs_bilateral, the window is a fixed 4×4 per quadrant and `radius`
// stretches its sample spacing.
vec3 fs_oilpaint(vec2 uv, float radius_px, float levels, float jitter) {
    float spacing = max(radius_px, 1.0) / 3.0;
    // brush-direction field: one smooth angle per neighborhood
    vec2 px = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);
    float ang = (fs_lsd_vnoise(px * (0.35 / max(radius_px, 1.0))) - 0.5)
              * (2.4 * clamp(jitter, 0.0, 1.5));
    float ca = cos(ang);
    float sa = sin(ang);
    vec3 best = FS_SAMPLE(uv);
    float best_v = 1e9;
    for (int q = 0; q < 4; ++q) {
        vec2 qdir = vec2(q == 0 || q == 2 ? -1.0 : 1.0,
                         q == 0 || q == 1 ? -1.0 : 1.0);
        vec3 sum = vec3(0.0, 0.0, 0.0);
        float sum_l = 0.0;
        float sum_l2 = 0.0;
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                vec2 o = vec2(float(i), float(j)) * qdir * spacing;
                vec2 ro = vec2(o.x * ca - o.y * sa, o.x * sa + o.y * ca);
                vec3 s = FS_SAMPLE(uv + FS_TEXEL * ro);
                float l = fs_luma(s);
                sum += s;
                sum_l += l;
                sum_l2 += l * l;
            }
        }
        float v = sum_l2 - sum_l * sum_l / 16.0;   // luma variance ×16
        if (v < best_v) {
            best_v = v;
            best = sum / 16.0;
        }
    }
    if (levels >= 2.0) {
        float n = floor(levels + 0.5);
        best = floor(best * n + vec3(0.5)) * (1.0 / n);
    }
    return best;
}

// ---- painting and drawing styles -------------------------------------------
// The art-style family: anime, watercolor, sumie, impressionist,
// stainedglass, pixelart. Survey, algorithm choices and parameter rationale:
// docs/design/art_filters.ja.md. All are original implementations from the
// cited papers (MIT) — same golden discipline as the LSD/notebook kits:
// value noise only (continuous everywhere), fract-free spellings.

// Two decorrelated value-noise channels as a vector.
vec2 fs_art_vnoise2(vec2 p) {
    return vec2(fs_lsd_vnoise(p), fs_lsd_vnoise(p + vec2(19.19, 7.33)));
}

// Ring-sampled gaussian-ish luma blur: center + 8 taps at radius r_px.
// Absolute blur quality is irrelevant here — only DoG *differences* of two
// radii are consumed, and those stay smooth under this estimate.
float fs_art_luma_ring(vec2 uv, float r_px) {
    float acc = fs_luma(FS_SAMPLE(uv)) * 0.25;
    for (int i = 0; i < 8; ++i) {
        float a = 0.785398163 * float(i) + 0.35;
        vec2 o = vec2(cos(a), sin(a)) * r_px;
        acc += fs_luma(FS_SAMPLE(uv + FS_TEXEL * o)) * 0.09375;
    }
    return acc;
}

// XDoG-style ink line mass at uv (Winnemoeller 2011's soft thresholding in
// a smoothstep spelling — steadier across backends than tanh at a cliff).
// The difference of two ring blurs goes negative on the dark side of an
// edge; that is where the pen puts ink. Higher strength both darkens and
// (by lowering the threshold) thickens the line, like pressing the pen.
float fs_art_ink_line(vec2 uv, float width_px, float strength) {
    float w = max(width_px, 0.6);
    float dog = fs_art_luma_ring(uv, w) - fs_art_luma_ring(uv, w * 1.8);
    float s = clamp(strength, 0.0, 2.0);
    return clamp(smoothstep(0.014 / (0.4 + 0.6 * s), 0.08, -dog) * s, 0.0, 1.0);
}

// Anime cel shading — Winnemoeller 2006's real-time abstraction folded into
// one pass: edge-preserving pre-smoothing (fs_bilateral, mixable), LUMA-only
// soft quantization (chroma untouched, so hues never rotate the way the RGB
// posterize in `toon` does — the cel-shadow steps land on the same paint),
// XDoG ink lines, and a saturation lift. `toon` stays as the cheap look;
// this is the drawn-by-hand one.
vec3 fs_anime(vec2 uv, float levels, float lines, float width_px,
              float smooth_amt, float vivid) {
    float res_y = 1.0 / FS_TEXEL.y;
    float u = max(res_y / 400.0, 0.25);
    vec3 c = FS_SAMPLE(uv);
    float sm = clamp(smooth_amt, 0.0, 1.0);
    if (sm > 0.001) {
        c = mix(c, fs_bilateral(uv, 5.0 * u, 0.14), sm);
    }
    // luma soft quantization: a plateau plus a soft ramp inside each band
    float l = fs_luma(c);
    float n = max(levels, 2.0);
    float band = l * n - floor(l * n);
    float soft = clamp((band - 0.5) * 5.0, -0.5, 0.5);
    float lq = clamp((floor(l * n) + 0.5 + soft) / n, 0.0, 1.0);
    c = c * (lq / max(l, 1e-3));
    // cel colors are flat and confident
    float lm = fs_luma(c);
    c = mix(vec3(lm), c, 1.0 + clamp(vivid, 0.0, 1.5));
    // ink rides on top, slightly blue-black like animation ink
    float ink = fs_art_ink_line(uv, width_px, lines);
    return mix(clamp(c, 0.0, 1.0), vec3(0.05, 0.045, 0.08), ink);
}

// Watercolor — Bousseau 2006's pigment-density model carries the whole look
// in one expression: C' = C·(1 − (1−C)·(d−1)), where density d>1 deposits
// pigment (a second pass of the brush) and d<1 thins it. The density field
// is built from three watercolor phenomena: pigment pooling at wet-edge
// boundaries (gradient magnitude), granulation into the paper's tooth (fine
// value noise, strongest in mid-tone washes), and slow wash unevenness. On
// top: hand wobble (domain-warped reads), dilution toward paper white in
// the highlights (watercolor has no white paint), and the paper's own tint
// and tooth relief.
vec3 fs_watercolor(vec2 uv, float wash_px, float edge, float grain,
                   float wobble, float dilute) {
    float res_y = 1.0 / FS_TEXEL.y;
    float u = max(res_y / 400.0, 0.25);
    vec2 px = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);
    // hand wobble: two octaves of vector noise displace where we read
    vec2 wn = fs_art_vnoise2(px * (0.045 / u)) - vec2(0.5, 0.5)
            + (fs_art_vnoise2(px * (0.11 / u) + vec2(31.7, 13.3)) - vec2(0.5, 0.5)) * 0.5;
    vec2 wuv = uv + FS_TEXEL * (wn * (7.0 * u * clamp(wobble, 0.0, 2.0)));
    // wash: golden-angle spiral mean — pigment spreading in water softens
    // detail without the greasy look a box blur gives
    float r = max(wash_px, 0.75);
    vec3 mean = FS_SAMPLE(wuv);
    for (int i = 0; i < 12; ++i) {
        float ang = 2.39996323 * float(i) + 0.9;
        float rad = r * sqrt((float(i) + 0.5) / 12.0);
        mean += FS_SAMPLE(wuv + FS_TEXEL * (vec2(cos(ang), sin(ang)) * rad));
    }
    mean = mean / 13.0;
    // edge darkening: pigment pools where a wash meets a boundary
    float eps = max(r * 0.75, 1.0);
    float gx = fs_luma(FS_SAMPLE(wuv + FS_TEXEL * vec2(eps, 0.0)))
             - fs_luma(FS_SAMPLE(wuv - FS_TEXEL * vec2(eps, 0.0)));
    float gy = fs_luma(FS_SAMPLE(wuv + FS_TEXEL * vec2(0.0, eps)))
             - fs_luma(FS_SAMPLE(wuv - FS_TEXEL * vec2(0.0, eps)));
    float pool = clamp(length(vec2(gx, gy)) * 2.2, 0.0, 1.0) * clamp(edge, 0.0, 2.0);
    // granulation: pigment settles into the tooth, mostly in mid washes
    float lmn = fs_luma(mean);
    float tooth = fs_lsd_vnoise(px * (0.55 / u))
                + fs_lsd_vnoise(px * (1.1 / u) + vec2(53.1, 97.7)) * 0.5;
    float mid = 4.0 * lmn * (1.0 - lmn);
    float gran = (tooth * 0.6667 - 0.5) * clamp(grain, 0.0, 2.0) * (0.10 + 0.90 * mid);
    // a big soft brush is never perfectly even
    float uneven = (fs_lsd_vnoise(px * (0.012 / u) + vec2(7.7, 71.3)) - 0.5) * 0.5;
    float d = clamp(1.0 + 0.9 * pool + 0.38 * gran + uneven, 0.4, 2.5);
    vec3 c = mean * mean * (d - 1.0) + mean * (2.0 - d);
    // dilution: highlights thin out to paper
    vec3 paper = vec3(0.99, 0.975, 0.94);
    c = mix(c, paper, clamp(dilute, 0.0, 1.0) * smoothstep(0.55, 0.97, fs_luma(c)));
    // paper tint + tooth relief lit from the top-left (the two noise reads
    // sit close together — a directional derivative, not independent salt)
    float t1 = fs_lsd_vnoise(px * (0.5 / u) + vec2(211.0, 17.0));
    float t2 = fs_lsd_vnoise(px * (0.5 / u) + vec2(211.35, 17.35));
    c = c * paper * (1.0 + (t1 - t2) * (0.08 + 0.18 * clamp(grain, 0.0, 2.0)));
    return clamp(c, 0.0, 1.0);
}

// Sumi-e ink wash. Tone is the story: luma is bent through an ink curve and
// softly quantized into a few washes (the classical graded strokes), then
// smeared along the local edge tangent — the brush travels along contours,
// not across them (Way 2002's wash/contour split; the stroke-space dry
// brush after Strassmann 1986's bristle idea; the bleed is a one-shot ring
// approximation of the cellular ink-diffusion models). XDoG contours with
// pressure wobble draw the bones. Warm washi paper with fiber noise carries
// it; `chroma` washes a little of the source color back in (tansai).
vec3 fs_sumie(vec2 uv, float ink, float bleed_px, float dry, float outline,
              float chroma) {
    float res_y = 1.0 / FS_TEXEL.y;
    float u = max(res_y / 400.0, 0.25);
    vec2 px = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);
    // slight hand wobble keeps ruled edges out of an ink painting
    vec2 wn = fs_art_vnoise2(px * (0.06 / u)) - vec2(0.5, 0.5);
    vec2 wuv = uv + FS_TEXEL * (wn * (3.0 * u));
    // local flow: the brush runs along the edge tangent. The gradient is
    // taken COARSE and blended toward one master stroke direction where the
    // picture is flat — a per-pixel tangent in texture turns every noise
    // below into per-pixel speckle (ask the first draft).
    float fe = 3.2 * u;
    float ggx = fs_luma(FS_SAMPLE(wuv + FS_TEXEL * vec2(fe, 0.0)))
              - fs_luma(FS_SAMPLE(wuv - FS_TEXEL * vec2(fe, 0.0)));
    float ggy = fs_luma(FS_SAMPLE(wuv + FS_TEXEL * vec2(0.0, fe)))
              - fs_luma(FS_SAMPLE(wuv - FS_TEXEL * vec2(0.0, fe)));
    float gm = length(vec2(ggx, ggy));
    float coh = smoothstep(0.02, 0.09, gm);
    vec2 tang = gm > 1e-4 ? vec2(ggy, -ggx) / gm : vec2(0.94, 0.34);
    tang = tang * coh + vec2(0.94, 0.34) * (1.0 - coh);
    tang = tang / max(length(tang), 1e-4);
    // smear the ink along the tangent: anisotropic 5-tap gather
    vec3 mean = FS_SAMPLE(wuv) * 0.28;
    for (int i = 0; i < 4; ++i) {
        float o = (float(i) - 1.5) * (2.9 * u);
        mean += FS_SAMPLE(wuv + FS_TEXEL * (tang * o)) * 0.18;
    }
    // ink tone curve: the exponent sits on DENSITY, so mid-tones thin out
    // toward reserved paper (sumi-e lives on its whites) while true darks
    // keep their ink; `ink` lowers the exponent and the whole picture
    // wets. Then soft-quantized into four washes.
    float dexp = 2.6 - 0.9 * clamp(ink, 0.0, 2.0);
    float dens0 = pow(clamp(1.0 - fs_luma(mean), 0.0, 1.0), dexp);
    float band = dens0 * 4.0 - floor(dens0 * 4.0);
    float soft = clamp((band - 0.5) * 2.6, -0.5, 0.5);
    float dq = clamp((floor(dens0 * 4.0) + 0.5 + soft) * 0.25, 0.0, 1.0);
    float dens = mix(dens0, dq, 0.75);   // washes, not a poster
    // kasure: the brush runs dry in mid-density sweeps — stroke-space
    // noise, long along the travel, fine across it
    float s_along = dot(px, tang);
    float s_cross = dot(px, vec2(tang.y, -tang.x));
    float kn = fs_lsd_vnoise(vec2(s_along * (0.10 / u), s_cross * (0.55 / u)));
    float midmask = 4.0 * dens * (1.0 - dens);
    dens = dens * (1.0 - clamp(dry, 0.0, 1.5) * 0.42 * midmask
                         * smoothstep(0.5, 0.95, kn) * smoothstep(0.15, 0.35, dens));
    // nijimi: dark cores bleed a soft halo into the paper. AVERAGED over
    // the ring — a max here reads as speckle wherever one tap lands dark
    float rb = max(bleed_px, 0.5);
    float halo = 0.0;
    for (int i = 0; i < 8; ++i) {
        float a = 0.785398163 * float(i) + 0.6;
        float rr = rb * (0.85 + 0.3 * fs_lsd_vnoise(px * (0.09 / u)
                                                    + vec2(float(i) * 13.7, 5.1)));
        float ln = fs_luma(FS_SAMPLE(wuv + FS_TEXEL * (vec2(cos(a), sin(a)) * rr)));
        halo += clamp(pow(clamp(1.0 - ln, 0.0, 1.0), dexp) - 0.45, 0.0, 1.0);
    }
    dens = max(dens, halo * 0.125 * 0.9);
    // highlights snap clean to paper — a wash never leaves a gray film,
    // and sumi-e lives on its reserved whites
    dens = dens * smoothstep(0.06, 0.16, dens);
    // contours: pressure-wobbled ink lines
    float press = 0.55 + 0.7 * fs_lsd_vnoise(px * (0.07 / u) + vec2(99.1, 3.3));
    float bones = fs_art_ink_line(wuv, 1.9 * u, clamp(outline, 0.0, 2.0) * press);
    dens = max(dens, bones * 0.9);
    // washi with fibers; sumi is a warm blue-black, never pure black
    float fib = fs_lsd_vnoise(vec2(px.x * (0.10 / u), px.y * (0.5 / u))
                              + vec2(17.0, 231.0));
    vec3 paper = vec3(0.965, 0.945, 0.895) * (0.965 + 0.07 * (fib - 0.5));
    vec3 c = mix(paper, vec3(0.09, 0.088, 0.10), clamp(dens, 0.0, 1.0));
    // tansai: a light color wash over the ink drawing
    float ch = clamp(chroma, 0.0, 1.0);
    if (ch > 0.001) {
        vec3 tinted = mean * (1.0 - dens * 0.75) * paper;
        c = mix(c, tinted, ch);
    }
    return clamp(c, 0.0, 1.0);
}

// Impressionist brush dabs, single pass. Litwinowicz 1997 lays short
// strokes along the local edge tangent and clips them at strong edges;
// Hertzmann 1998 layers big background strokes under small ones. Both are
// stroke-serial algorithms, so this reformulates them for a gather: every
// pixel searches the 3×3 neighborhood of a jittered stroke-seed grid (two
// layers, coarse under fine), rebuilds each nearby dab — position, tangent
// orientation, capsule footprint, per-seed broken color (the divisionist
// color vibration) — and wears the covering dab with the best score
// (coverage × z-hash, fine layer biased). Between dabs the canvas shows.
// Impasto: bristle noise in stroke space tilts the shading, plus a rim
// shadow at each dab's soft edge.
vec3 fs_impressionist(vec2 uv, float stroke_px, float time_s, float vibrance,
                      float flow, float relief) {
    float res_y = 1.0 / FS_TEXEL.y;
    float u = max(res_y / 400.0, 0.25);
    vec2 px = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);
    float p_fine = max(stroke_px, 2.0);
    float vib = clamp(vibrance, 0.0, 2.0);
    float fl = clamp(flow, 0.0, 1.0);
    float rlf = clamp(relief, 0.0, 2.0);
    float t = time_s;
    vec3 best = vec3(0.0, 0.0, 0.0);
    float best_score = -1.0;
    float cov_any = 0.0;
    for (int layer = 0; layer < 2; ++layer) {
        float pitch = layer == 0 ? p_fine * 2.1 : p_fine;
        float zbias = layer == 0 ? 0.0 : 0.35;
        float wid = layer == 0 ? pitch * 0.55 : pitch * 0.42;
        vec2 lofs = layer == 0 ? vec2(0.0, 0.0) : vec2(37.7, 17.3);
        vec2 cell = floor(px / pitch);
        for (int j = -1; j <= 1; ++j) {
            for (int i = -1; i <= 1; ++i) {
                vec2 id = cell + vec2(float(i), float(j)) + lofs;
                float h1 = fs_lsd_hash(id * 0.731 + vec2(0.17, 0.37));
                float h2 = fs_lsd_hash(id * 0.593 + vec2(7.13, 3.71));
                float hz = fs_lsd_hash(id * 0.419 + vec2(1.91, 8.23));
                vec2 seed = (id - lofs + vec2(0.5, 0.5)
                             + (vec2(h1, h2) - vec2(0.5, 0.5)) * 0.9) * pitch;
                vec2 rel = px - seed;
                float lmax = pitch * 1.95;   // conservative cull, no taps yet
                if (dot(rel, rel) > lmax * lmax) continue;
                vec2 suv = clamp(vec2(seed.x * FS_TEXEL.x, seed.y * FS_TEXEL.y),
                                 vec2(0.0, 0.0), vec2(1.0, 1.0));
                float eps = max(pitch * 0.5, 1.0);
                float gx = fs_luma(FS_SAMPLE(suv + FS_TEXEL * vec2(eps, 0.0)))
                         - fs_luma(FS_SAMPLE(suv - FS_TEXEL * vec2(eps, 0.0)));
                float gy = fs_luma(FS_SAMPLE(suv + FS_TEXEL * vec2(0.0, eps)))
                         - fs_luma(FS_SAMPLE(suv - FS_TEXEL * vec2(0.0, eps)));
                float gm = length(vec2(gx, gy));
                // orientation: where the picture is flat, strokes ride the
                // CURL of a slowly drifting noise potential — a curl field
                // is divergence-free, so stroke lanes close into the
                // vortices van Gogh painted instead of running off. Strong
                // edges still steer the stroke along the contour (flow).
                vec2 fp = seed * (0.011 / u) + vec2(0.13 * t, -0.09 * t);
                float fe2 = 0.35;
                float dpx = fs_lsd_vnoise(fp + vec2(fe2, 0.0))
                          - fs_lsd_vnoise(fp - vec2(fe2, 0.0));
                float dpy = fs_lsd_vnoise(fp + vec2(0.0, fe2))
                          - fs_lsd_vnoise(fp - vec2(0.0, fe2));
                vec2 dirf = vec2(dpy, -dpx);
                float dl = length(dirf);
                vec2 dir = dl > 1e-5 ? dirf / dl : vec2(0.94, 0.34);
                // each dab breathes around its lane — the painting lives
                float wob = (h1 - 0.5) * 0.6
                          + 0.22 * sin(t * (0.5 + 0.9 * h2) + h1 * 6.28318531);
                float cw = cos(wob);
                float sw = sin(wob);
                dir = vec2(dir.x * cw - dir.y * sw, dir.x * sw + dir.y * cw);
                if (gm > 1e-4) {
                    vec2 tg = vec2(gy, -gx) / gm;
                    if (dot(tg, dir) < 0.0) { tg = vec2(-tg.x, -tg.y); }
                    float wf = fl * smoothstep(0.02, 0.12, gm);
                    vec2 dd = dir + (tg - dir) * wf;
                    dir = dd / max(length(dd), 1e-4);
                }
                // long thin capsule = a loaded brush pulled through the
                // paint; strokes shorten only where edges are strong
                // (Litwinowicz's clipping, reduced to a statistic)
                float len = pitch * (1.7 - 1.0 * smoothstep(0.05, 0.30, gm));
                float s = dot(rel, dir);
                float q = dot(rel, vec2(dir.y, -dir.x));
                float edge_d = abs(q) / wid + (s * s) / max(len * len, 1e-4);
                if (edge_d > 1.15) continue;
                float cover = 1.0 - smoothstep(0.75, 1.05, edge_d);
                if (cover < 0.03) continue;
                cov_any = max(cov_any, cover);
                float score = cover * (0.35 + 0.65 * hz) + zbias;
                if (score > best_score) {
                    best_score = score;
                    vec3 c = FS_SAMPLE(suv);
                    // broken color: each dab mixes its pigment a bit wrong.
                    // LUMA-PRESERVING — divisionism vibrates hue while the
                    // values keep the large forms readable (a free jitter
                    // turns a sky into confetti); the vibration shimmers
                    // slowly with time
                    float vib_t = vib * (0.85 + 0.15 * sin(t * 0.7 + hz * 6.28318531));
                    vec3 cj = c * (vec3(1.0, 1.0, 1.0)
                                   + (vec3(h1, h2, hz) - vec3(0.5, 0.5, 0.5))
                                     * (vib_t * 0.6));
                    c = cj * (fs_luma(c) / max(fs_luma(cj), 1e-3));
                    // impasto: bristle streaks along the dab, lit top-left
                    // (two CLOSE noise reads = a directional derivative);
                    // the phase drifts with time, so paint creeps along
                    // the stroke like wet oil
                    vec2 bp = vec2(s * (0.16 / u) - t * (0.5 + 0.5 * h2),
                                   q * (0.5 / u))
                            + vec2(hz * 61.0, h1 * 47.0);
                    float b1 = fs_lsd_vnoise(bp);
                    float b2 = fs_lsd_vnoise(bp + vec2(0.2, 0.28));
                    float shade = (b1 - b2) * 0.9 * (0.35 + 0.65 * fs_luma(c))
                                + (0.5 - edge_d) * 0.18;
                    best = c * (1.0 + rlf * shade);
                }
            }
        }
    }
    // the canvas ground shows between dabs, and its weave sits faintly in
    // the paint everywhere
    float wx = fs_lsd_vnoise(vec2(px.x * (0.5 / u), px.y * (0.09 / u)));
    float wy = fs_lsd_vnoise(vec2(px.x * (0.09 / u) + 43.0, px.y * (0.5 / u)));
    float weave = (wx + wy) * 0.5;
    vec3 ground = vec3(0.92, 0.895, 0.85) * (0.93 + 0.14 * weave);
    vec3 c = mix(ground, best, smoothstep(0.10, 0.5, cov_any));
    c = c * (1.0 + 0.10 * (weave - 0.5));
    return clamp(c, 0.0, 1.0);
}

// Stained glass — Worley 1996 cellular noise: each pixel finds the nearest
// (F1) and second-nearest (F2) jittered cell sites; F2−F1 measures the
// distance to the pane border, so the lead came is a smoothstep on it.
// Panes are tinted from the source at the site (posterized, saturated, and
// nudged in hue per pane so a flat sky still breaks into distinct glass),
// shaded by a light gradient across the pane and a faint ripple in the
// glass itself.
vec3 fs_stainedglass(vec2 uv, float size_px, float lead, float irregular,
                     float sat_amt, float light_amt) {
    float pitch = max(size_px, 4.0);
    float res_y = 1.0 / FS_TEXEL.y;
    float u = max(res_y / 400.0, 0.25);
    vec2 px = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);
    vec2 cell = floor(px / pitch);
    float f1 = 1e9;
    float f2 = 1e9;
    vec2 site1 = px;
    vec2 id1 = cell;
    float jit = clamp(irregular, 0.0, 1.0) * 0.95;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 id = cell + vec2(float(i), float(j));
            float h1 = fs_lsd_hash(id * 0.677 + vec2(0.31, 0.71));
            float h2 = fs_lsd_hash(id * 0.531 + vec2(5.17, 2.93));
            vec2 site = (id + vec2(0.5, 0.5)
                         + (vec2(h1, h2) - vec2(0.5, 0.5)) * jit) * pitch;
            float d = length(px - site);
            if (d < f1) {
                f2 = f1;
                f1 = d;
                site1 = site;
                id1 = id;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    vec2 suv = clamp(vec2(site1.x * FS_TEXEL.x, site1.y * FS_TEXEL.y),
                     vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec3 c = FS_SAMPLE(suv);
    float sat = clamp(sat_amt, 0.0, 1.5);
    c = fs_posterize(c, 7.0 - 3.0 * sat);
    c = mix(vec3(fs_luma(c)), c, 1.0 + 0.9 * sat);
    float hn = fs_lsd_hash(id1 * 0.913 + vec2(9.71, 4.13)) - 0.5;
    c = fs_hue_rotate(c, hn * (14.0 + 30.0 * sat));
    // transmitted light: a soft gradient across each pane toward the sun
    float lt = clamp(light_amt, 0.0, 1.5);
    vec2 rel = (px - site1) / pitch;
    float grad = dot(rel, vec2(-0.55, -0.75));
    float ripple = fs_lsd_vnoise(px * (0.16 / u) + id1 * 3.7) - 0.5;
    c = c * (1.0 + lt * (0.30 * grad + 0.18 * ripple) + lt * 0.10);
    // lead came: dark, slightly warm metal, thin bevel light near the glass
    float lw = pitch * 0.055 * clamp(lead, 0.0, 2.0) + 0.6;
    float border = f2 - f1;
    float m = lead < 0.01 ? 1.0 : smoothstep(lw, lw * 1.9, border);
    float bevel = smoothstep(lw * 0.4, lw * 1.6, border);
    vec3 came = vec3(0.13, 0.125, 0.12) * (0.75 + 0.5 * bevel);
    return clamp(mix(came, c, m), 0.0, 1.0);
}

// Pixel art — block-average resample (2×2 inside each block), a gentle
// saturation-and-contrast conditioning toward confident retro palettes,
// Bayer 4×4 ordered dithering, then per-channel quantization: n³ effective
// colors. The threshold matrix comes from the classic recursive
// construction, computed arithmetically — no lookup table:
//   M2(x,y) = 2x + 3y − 4xy,  M4(x,y) = 4·M2(x mod 2, y mod 2)
//                                       + M2(⌊x/2⌋, ⌊y/2⌋).
vec3 fs_pixelart(vec2 uv, float size_px, float colors, float dither,
                 float sat_amt) {
    float bs = max(size_px, 2.0);
    vec2 px = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);
    vec2 block = floor(px / bs);
    vec2 base = (block + vec2(0.5, 0.5)) * bs;
    vec3 c = vec3(0.0, 0.0, 0.0);
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            vec2 o = vec2(float(i) - 0.5, float(j) - 0.5) * (bs * 0.35);
            vec2 tuv = clamp(vec2((base.x + o.x) * FS_TEXEL.x,
                                  (base.y + o.y) * FS_TEXEL.y),
                             vec2(0.0, 0.0), vec2(1.0, 1.0));
            c += FS_SAMPLE(tuv);
        }
    }
    c = c / 4.0;
    float sat = clamp(sat_amt, 0.0, 1.5);
    c = mix(vec3(fs_luma(c)), c, 1.0 + sat);
    c = clamp((c - vec3(0.5)) * (1.0 + 0.25 * sat) + vec3(0.5), 0.0, 1.0);
    float bx = block.x - 4.0 * floor(block.x / 4.0);
    float by = block.y - 4.0 * floor(block.y / 4.0);
    float x1 = bx - 2.0 * floor(bx / 2.0);
    float y1 = by - 2.0 * floor(by / 2.0);
    float x2 = floor(bx / 2.0);
    float y2 = floor(by / 2.0);
    float m4 = 4.0 * (2.0 * x1 + 3.0 * y1 - 4.0 * x1 * y1)
             + (2.0 * x2 + 3.0 * y2 - 4.0 * x2 * y2);
    float t = (m4 + 0.5) / 16.0 - 0.5;
    float n = max(floor(colors + 0.5), 2.0) - 1.0;
    c = c + vec3(t * clamp(dither, 0.0, 1.5) / n);
    return clamp(floor(c * n + vec3(0.5)) / n, 0.0, 1.0);
}

// Hand-drawn wobble — the page-shiver that used to live inside notebook,
// as its own chainable filter: reads are displaced by two octaves of vector
// noise, and the noise field is re-rolled `fps` times a second (quantized
// time + a hash phase per redraw), which is exactly the stop-motion "each
// frame is a fresh drawing" effect. fps = 0 drifts continuously instead.
vec3 fs_wobble(vec2 uv, float amount_px, float time_s, float scale_,
               float fps) {
    float res_y = 1.0 / FS_TEXEL.y;
    float u = max(res_y / 400.0, 0.25);
    vec2 px = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);
    float tq = fps > 0.01 ? floor(time_s * fps) / max(fps, 0.01) : time_s;
    vec2 ph = vec2(fs_lsd_hash(vec2(tq * 1.13, 3.7)),
                   fs_lsd_hash(vec2(tq * 0.87, 9.1))) * 73.0;
    float freq = 0.05 / (u * max(scale_, 0.1));
    vec2 wn = fs_art_vnoise2(px * freq + ph) - vec2(0.5, 0.5)
            + (fs_art_vnoise2(px * (freq * 2.3) + ph + vec2(31.7, 13.3))
               - vec2(0.5, 0.5)) * 0.5;
    return FS_SAMPLE(uv + FS_TEXEL * (wn * (2.0 * max(amount_px, 0.0))));
}

// ---- generative ------------------------------------------------------------
// fs_fractal — eternal flight through a self-morphing kaleidoscopic IFS.
// Design: docs/design/fractal_filter.ja.md. Original composition; the
// technique families (KIFS folding per Knighty/fractalforums 2010, sphere
// tracing per Hart 1996, orbit-trap coloring, quasi-periodic parameter
// drive) are public knowledge — no Shadertoy code is ported.
//
// Everything is a pure function of `time` (host-driven, fs_lsd contract):
// no randomness, no frame state. Every time-varying knob rides a bank of
// mutually incommensurate sines, so the parameter orbit is dense on a
// torus — strictly aperiodic. Chaos in the folds amplifies that drift into
// endless structural novelty: the mood persists, the frame never repeats.
// The camera advances along z, which the 2-unit space tiling makes exactly
// periodic — wrapping z each tile keeps float32 sharp forever.

// Three incommensurate sines (golden-ratio frequency spread). Range ±3.
float fs_fr_osc(float t, float f, float ph) {
    return sin(t * f + ph) + sin(t * f * 1.6180339887 + ph * 2.0) +
           sin(t * f * 0.3819660113 + ph * 4.0);
}

vec3 fs_fr_pal(float x, vec3 phase, float amp) {
    return vec3(0.5, 0.5, 0.5) + amp * cos(vec3(6.2831853 * x, 6.2831853 * x, 6.2831853 * x) + phase);
}

// Distance estimate + orbit traps. Returns (d, trap@7, trap@6, trap@5):
// the same min-|z| trap cut at three consecutive fold depths — their gaps
// become the R/G/B phase offsets of the iridescent fringes (a 2D trick
// from the inversion-IFS family, recomposed here in 3D).
// The change itself is fractal: every fold DEPTH carries its own clock,
// and the deeper (finer) the level, the faster it runs — so the fine
// filigree seethes on a seconds scale while the great halls drift on a
// minutes scale. One global deformation would read as a single slow
// morph; this reads as a world alive at every magnification.
vec4 fs_fr_de(vec3 p, float a1, float a2, float sc, vec3 off, float t, float m, float wInv) {
    // infinite tiling: fold all of space into one 2-unit mirror cell
    vec3 w = p * 0.5;
    w = w - floor(w);
    vec3 z = abs(1.0 - w * 2.0);
    float ks = 1.0;
    float d = 1000.0;
    float t0 = 1000.0;
    float t1 = 1000.0;
    float t2 = 1000.0;
    for (int n = 0; n < 7; ++n) {
        float fn = float(n);
        float aa = a1 + 0.11 * m * sin(t * (0.20 + 0.17 * fn) + fn * 2.4);
        float bb = a2 + 0.08 * m * sin(t * (0.16 + 0.21 * fn) + fn * 1.7);
        float c1 = cos(aa);
        float s1 = sin(aa);
        float c2 = cos(bb);
        float s2 = sin(bb);
        float rx = c1 * z.x + s1 * z.y;
        float ry = c1 * z.y - s1 * z.x;
        z.x = rx;
        z.y = ry;
        z = abs(z);
        float tmp = 0.0;
        if (z.x < z.y) { tmp = z.x; z.x = z.y; z.y = tmp; }
        if (z.x < z.z) { tmp = z.x; z.x = z.z; z.z = tmp; }
        if (z.y < z.z) { tmp = z.y; z.y = z.z; z.z = tmp; }
        float ry2 = c2 * z.y + s2 * z.z;
        float rz = c2 * z.z - s2 * z.y;
        z.y = ry2;
        z.z = rz;
        // family morph: a blendable sphere-inversion fold. At 0 the fold
        // stack is pure Menger — rectilinear, architectural. As it rises
        // the space curves and the grid melts into bubbled, lace-like
        // Apollonian forms. Continuously driveable, so the architecture
        // itself dissolves and re-crystallizes over time.
        float r2i = dot(z, z);
        float fi = 1.0 + wInv * (1.35 / max(r2i, 0.35) - 1.0);
        fi = clamp(fi, 0.6, 3.2);
        z = z * fi;
        ks = ks * fi;
        z = z * sc - off * (sc - 1.0);
        float zf = off.z * (sc - 1.0);
        if (z.z < -0.5 * zf) { z.z = z.z + zf; }
        ks = ks * sc;
        float r = length(z) / ks;
        d = min(d, r);
        if (n < 5) { t2 = min(t2, r); }
        if (n < 6) { t1 = min(t1, r); }
        t0 = min(t0, r);
    }
    return vec4(d - 0.0012, t0, t1, t2);
}

vec3 fs_fractal(vec2 uv, float flight, float time_s, float morph, float glow, float blend) {
    vec3 src = FS_SAMPLE(uv);
    float t = time_s;
    float m = clamp(morph, 0.0, 2.0);

    // the drive bank — every knob on its own incommensurate clock. The
    // clocks are FAST enough that the geometry visibly reorganizes within
    // seconds (the owner's brief: the SHAPE must never stop becoming);
    // the fold angles, the scale, and the offset vector — the very family
    // of the fractal — are all in flight at once.
    float a1b = 3.55 + 0.85 * m * fs_fr_osc(t, 0.061, 0.7);
    float a2b = 0.50 * m * fs_fr_osc(t, 0.047, 2.3);
    float sc = 3.02 + 0.20 * m * fs_fr_osc(t, 0.033, 4.1);
    sc = clamp(sc, 2.5, 3.5);
    vec3 off = vec3(0.96 + 0.055 * m * fs_fr_osc(t, 0.043, 1.1),
                    0.90 + 0.055 * m * fs_fr_osc(t, 0.037, 3.9),
                    0.34 + 0.030 * m * fs_fr_osc(t, 0.029, 5.5));
    float wInv = clamp(0.30 + 0.30 * fs_fr_osc(t, 0.0165, 5.9), 0.0, 0.9) * clamp(m, 0.0, 1.5);
    float nlp = 0.11 * m * (1.0 + 0.45 * fs_fr_osc(t, 0.011, 1.9));
    float hue = 0.075 * t + 0.30 * fs_fr_osc(t, 0.007, 3.3);
    float roll = 0.22 * fs_fr_osc(t, 0.009, 0.2);

    // the weather layer — mood clocks slower than the geometry, so the
    // piece passes through seasons: vividness swells and drains, the
    // channel spread collapses to near-monochrome and blooms back to full
    // rainbow, the two lights trade warm for cold, the fog thickens and
    // clears, the glow ebbs. A time teleport jumps the weather with it.
    float mA = 0.50 + 0.093 * fs_fr_osc(t, 0.0043, 2.9);
    float spr = 0.68 + 0.24 * fs_fr_osc(t, 0.0037, 4.7);
    float hb = hue * 6.2831853;
    vec3 mph = vec3(hb, hb + 2.1 * spr, hb + 4.2 * spr);
    // fast shimmer: the hues themselves flicker on a seconds scale
    mph = mph + vec3(0.11 * sin(t * 0.31 + 1.0), 0.11 * sin(t * 0.41 + 3.0),
                     0.11 * sin(t * 0.55 + 5.0));
    float fogd = clamp(0.16 + 0.05 * fs_fr_osc(t, 0.0047, 3.8), 0.06, 0.30);
    float gw = 0.10 + 0.02 * fs_fr_osc(t, 0.0059, 1.2);
    vec3 keyC = mix(vec3(1.0, 0.93, 0.82), fs_fr_pal(0.05, mph, 0.5), 0.45);
    vec3 filC = mix(vec3(0.22, 0.32, 0.52), fs_fr_pal(0.55, mph, 0.5), 0.45);

    // matter: what the world is MADE of wanders too — stone (matte mass),
    // glass (reflection, rim light, hard sparkle), pure light (the surface
    // dissolves and only the glow remains). ~2 min per lap, and a time
    // teleport lands mid-substance: the same geometry keeps returning as a
    // different material.
    float pm = t * 0.021 + 0.13 * fs_fr_osc(t, 0.005, 2.2);
    float mu = pm - floor(pm / 3.0) * 3.0;
    float dSt = min(abs(mu), 3.0 - abs(mu));
    float dGl = min(abs(mu - 1.0), 3.0 - abs(mu - 1.0));
    float dLi = min(abs(mu - 2.0), 3.0 - abs(mu - 2.0));
    float wSt = 1.0 - smoothstep(0.45, 1.15, dSt);
    float wGl = 1.0 - smoothstep(0.45, 1.15, dGl);
    float wLi = 1.0 - smoothstep(0.45, 1.15, dLi);
    float wS = wSt + wGl + wLi + 0.0001;
    wSt = wSt / wS;
    wGl = wGl / wS;
    wLi = wLi / wS;
    // light-matter runs vivid: a pale palette there washes the frame white
    mA = clamp(mA + 0.25 * wLi, 0.2, 0.85);

    // camera: forward flight, z wrapped on the tile so it can fly forever
    float zraw = 1.30 * clamp(flight, -4.0, 4.0) * t;
    float z0 = 2.0 * (zraw - floor(zraw));
    vec3 ro = vec3(0.09 * fs_fr_osc(t, 0.023, 5.1), 0.09 * fs_fr_osc(t, 0.019, 1.3), z0);
    // No in-flight obstacle steering here, by owner decree: the corrective
    // push (both the hard 0.12 clamp and the soft band that replaced it)
    // read as shivering and worse. The flight PATH itself is the structural
    // fix — a pre-computed clear-corridor spline is the planned successor;
    // until then the page's probe rescue is the safety net.
    vec2 d0 = uv - vec2(0.5, 0.5);
    vec2 pc = vec2(d0.x * (FS_TEXEL.y / FS_TEXEL.x), d0.y);
    float cr = cos(roll);
    float sr = sin(roll);
    vec2 pr = vec2(cr * pc.x - sr * pc.y, sr * pc.x + cr * pc.y);
    vec3 rd = normalize(vec3(pr.x, pr.y, 0.95));

    // march: sphere tracing with a glow integral riding along. The glow
    // kernel d/(d² + ε²) peaks a skin's depth off every surface and dies
    // both on contact and at range — the wet halo, without a break.
    float tt = 0.02 + 0.006 * fs_lsd_hash(uv * 913.7);
    float run = 1.0;
    float hitT = -1.0;
    float used = 0.0;
    vec3 pos = ro;
    vec3 dir = rd;
    vec4 h = vec4(1000.0, 1000.0, 1000.0, 1000.0);
    vec3 gacc = vec3(0.0, 0.0, 0.0);
    for (int i = 0; i < 46; ++i) {
        if (run > 0.5) {
            // non-linear perspective: the ray bends with distance traveled
            float ba = tt * nlp;
            float cb = cos(ba);
            float sb = sin(ba);
            dir = vec3(rd.x, cb * rd.y - sb * rd.z, sb * rd.y + cb * rd.z);
            pos = ro + dir * tt;
            h = fs_fr_de(pos, a1b, a2b, sc, off, t, m, wInv);
            float d = h.x;
            // line integral of a Lorentzian shell density around surfaces:
            // step length × ε/(d²+ε²) — converges, halo stays a halo
            float ds = max(d, 0.0) * 0.72;
            // light-matter narrows the halo: broad wash becomes filament
            float ge = 0.006 - 0.0051 * wLi;
            gacc += fs_fr_pal(0.35 * tt + 2.2 * h.y, mph, mA) *
                    (ds * 0.075 / (d * d + ge));
            float prec = 0.0012 * tt;
            tt += ds;
            used += 1.0;
            if (d < prec) { hitT = tt; run = 0.0; }
            if (tt > 22.0) { run = 0.0; }
        }
    }

    vec3 col = vec3(0.004, 0.005, 0.010);
    if (hitT > 0.0) {
        // tetrahedron normal (4 taps)
        float ep = 0.0009 * max(hitT, 0.08);
        vec3 e1 = vec3(1.0, -1.0, -1.0);
        vec3 e2 = vec3(-1.0, -1.0, 1.0);
        vec3 e3 = vec3(-1.0, 1.0, -1.0);
        vec3 e4 = vec3(1.0, 1.0, 1.0);
        vec3 n = e1 * fs_fr_de(pos + e1 * ep, a1b, a2b, sc, off, t, m, wInv).x +
                 e2 * fs_fr_de(pos + e2 * ep, a1b, a2b, sc, off, t, m, wInv).x +
                 e3 * fs_fr_de(pos + e3 * ep, a1b, a2b, sc, off, t, m, wInv).x +
                 e4 * fs_fr_de(pos + e4 * ep, a1b, a2b, sc, off, t, m, wInv).x;
        n = normalize(n);
        // warm key + cool fill, orbit-trap iridescence, trap + step AO
        vec3 l1 = normalize(vec3(0.55, 0.72, -0.42));
        vec3 l2 = normalize(vec3(-0.48, -0.30, 0.62));
        float kd = clamp(dot(n, l1), 0.0, 1.0);
        float fd = clamp(0.2 + 0.8 * dot(n, l2), 0.0, 1.0);
        vec3 irid = vec3(0.5, 0.5, 0.5) +
                    0.5 * cos(vec3(12.0 * h.y, 12.4 * h.z, 12.8 * h.w) + mph);
        vec3 alb = mix(fs_fr_pal(1.7 * h.y + 0.13, mph, mA), irid, 0.55);
        float aoT = pow(clamp(h.y * 2.6, 0.0, 1.0), 1.25);
        float aoS = clamp(1.15 - used / 46.0, 0.0, 1.0);
        vec3 ref = dir - 2.0 * dot(n, dir) * n;
        vec3 env = mix(vec3(0.10, 0.06, 0.16), vec3(0.55, 0.65, 0.90), 0.5 + 0.5 * ref.y);
        float spe = pow(clamp(dot(ref, l1), 0.0, 1.0), 28.0);
        float rim = pow(clamp(1.0 + dot(n, dir), 0.0, 1.0), 3.0);
        float surfW = wSt + 0.45 * wGl + 0.30 * wLi;
        float envW = 0.10 * wSt + 0.50 * wGl + 0.06 * wLi;
        float speW = 1.1 * wSt + 2.8 * wGl + 0.4 * wLi;
        float spe2 = mix(spe, spe * spe, wGl);
        vec3 surf = alb * (vec3(0.04, 0.04, 0.06) + keyC * (1.05 * kd) + filC * fd);
        surf = surf * (surfW * aoT * aoS * aoS) + env * (envW * aoT) +
               fs_fr_pal(0.3, mph, 0.5) * (rim * (0.85 * wGl + 0.25 * wLi)) +
               vec3(1.0, 1.0, 1.0) * (spe2 * speW * aoS);
        col = surf * exp(-fogd * hitT);
    }
    float gwm = wSt + 1.7 * wGl + 1.4 * wLi;
    col = col + gacc * (gw * gwm * clamp(glow, 0.0, 3.0));
    col = mix(col, col * (vec3(0.30, 0.30, 0.30) + src * 1.7), clamp(blend, 0.0, 1.0));
    return vec3(tanh(col.x * 1.25), tanh(col.y * 1.25), tanh(col.z * 1.25));
}

// ---- hand drawing ----------------------------------------------------------
// fs_notebook — a pencil sketch on squared notebook paper.
//
// After "notebook drawings" by Florian Berger (flockaroo), shadertoy.com/
// view/XtVGD1, CC BY-NC-SA 3.0 — NON-COMMERCIAL; this filter inherits that
// license and must not ship in a commercial build. The core idea is his:
// convolve arc-shaped strokes along a few fixed directions, inking a pixel
// where the luminance gradient runs PARALLEL to the stroke — edges become
// pencil lines, and the quadratic cross-term bends each line into the arc a
// wrist actually draws. Deviations from the original, on purpose:
//   - the noise texture is the hash kit above (self-contained build);
//   - the baked-in vignette and the source-specific green desaturation are
//     dropped — compose `vignette` / grading filters instead;
//   - page wobble, the squared-paper grid and the crayon layer are
//     parameters (`wobble`, `grid`, `chroma`), and the host drives `time`
//     like fs_lsd (a still frame is a still drawing).

// Grain noise. VALUE noise, not raw hash, for the same reason the LSD kit
// spells it out: it is continuous everywhere, so CPU/GPU rounding drift
// cannot flip a speckle across the screening threshold and break golden
// parity. The original sampled a noise texture bilinearly — also continuous.
vec3 fs_nb_rand3(vec2 p) {
    return vec3(fs_lsd_vnoise(p),
                fs_lsd_vnoise(p + vec2(17.13, 3.71)),
                fs_lsd_vnoise(p + vec2(41.7, 29.3)));
}

// Source color at a POINT IN PIXELS that may run beyond the image: outside
// it fades to white over ~5% of the frame, so strokes trail off the page
// instead of smearing the clamped edge texel. min() keeps paper from ever
// reading as pure white next to the karo lines.
vec3 fs_nb_col(vec2 px) {
    vec2 uv = vec2(px.x * FS_TEXEL.x, px.y * FS_TEXEL.y);
    vec2 cuv = clamp(uv, FS_TEXEL * 3.0, vec2(1.0, 1.0) - FS_TEXEL * 3.0);
    vec3 c = FS_SAMPLE(cuv);
    float e = smoothstep(-0.05, 0.0, uv.x) * smoothstep(-0.05, 0.0, uv.y)
            * smoothstep(-0.05, 0.0, 1.0 - uv.x)
            * smoothstep(-0.05, 0.0, 1.0 - uv.y);
    c = mix(vec3(1.0, 1.0, 1.0), c, e);
    return min(c, vec3(0.7, 0.7, 0.7));
}

float fs_nb_val(vec2 px) { return fs_luma(fs_nb_col(px)); }

vec2 fs_nb_grad(vec2 px, float eps) {
    return vec2(fs_nb_val(px + vec2(eps, 0.0)) - fs_nb_val(px - vec2(eps, 0.0)),
                fs_nb_val(px + vec2(0.0, eps)) - fs_nb_val(px - vec2(0.0, eps)))
           * (0.5 / eps);
}

// Crayon layer: stochastic screening — color quantized against white noise
// reads as the uneven pressure of a colored pencil. The noise lives in
// STROKE space (sc·px) so its speckle scales with the strokes, not the image.
vec3 fs_nb_colht(vec2 px, float sc) {
    vec3 c = fs_nb_col(px) * 0.8 + vec3(0.2, 0.2, 0.2)
           + fs_nb_rand3(px * (0.7 * sc));
    return vec3(smoothstep(0.95, 1.05, c.x),
                smoothstep(0.95, 1.05, c.y),
                smoothstep(0.95, 1.05, c.z));
}

vec3 fs_notebook(vec2 uv, float scale, float grid, float chroma) {
    float res_y = 1.0 / FS_TEXEL.y;
    float u = res_y / 400.0;               // logical unit: 1 at 400px height
    // The original's `zoom` both magnified the view AND the strokes; a
    // filter must leave the view alone, so `pos` stays in true image
    // pixels and only the stroke GEOMETRY (step, curvature, probe, grain,
    // grid) is magnified by 1/scale. Smaller scale = bolder pencil.
    // (The original's page shiver moved out to the standalone `wobble`
    // filter — chain it when the stop-motion look is wanted.)
    float sc = clamp(scale, 0.05, 1.0);
    float mag = 1.0 / sc;
    vec2 pos = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);

    float ink = 0.0;                       // accumulated graphite
    vec3 tint = vec3(0.0, 0.0, 0.0);       // crayon layer numerator
    float wsum = 0.0;
    for (int i = 0; i < 3; ++i) {          // 3 stroke directions, axis-free
        float ang = 6.2831853 / 3.0 * (float(i) + 0.8);
        vec2 v = vec2(cos(ang), sin(ang)); // stroke NORMAL (gradient to ink)
        vec2 w = vec2(v.y, -v.x);          // stroke direction
        for (int j = 0; j < 16; ++j) {
            // linear term walks along the stroke; the j² term drifts it
            // sideways — that curvature is the whole hand-drawn look
            vec2 dpos = w * (float(j) * u * mag);
            vec2 dpos2 = v * (float(j * j) / 16.0 * 0.5 * u * mag);
            for (int k = 0; k < 2; ++k) {  // both ends of the stroke
                float sgn = float(k) * 2.0 - 1.0;
                vec2 o = dpos * sgn + dpos2;
                vec2 g = fs_nb_grad(pos + o, 0.4 * mag);
                float gv = dot(g, v);
                float gw = dot(g, w);
                // parallel edges ink, skew edges are penalized, and the
                // clamp is the ink limit of a single pass of the pencil
                float fact = clamp(gv - 0.5 * abs(gw), 0.0, 0.05);
                ink += fact * (1.0 - float(j) / 16.0);   // strokes taper
                // crayon weight rides the PERPENDICULAR component, sampled
                // at the mirrored offset — color hatches across the lines.
                // The epsilon is flockaroo's, and it is LOAD-BEARING: where
                // the gradient vanishes (flat paper) it aims the unit vector
                // at +x, so |w.x| still banks paper-colored samples there —
                // without it the crayon layer divides noise by noise
                vec2 gp = g + vec2(1e-4, 0.0);
                float f2 = abs(dot(gp, w)) / length(gp);
                tint += fs_nb_colht(pos + vec2(o.y, -o.x) * 2.0, sc) * f2;
                wsum += f2;
            }
        }
    }
    ink = ink * (sqrt(res_y) / 36.0);      // = /(3·16 · 0.75/√resY)
    tint = tint / max(wsum, 1e-4);

    float grain = 0.6 + 0.8 * fs_nb_rand3(pos * (0.7 * sc)).x;  // paper tooth
    float tone = 1.0 - ink * grain;
    tone = tone * tone * tone;             // light strokes vanish into paper

    // karo (squared paper): thin gaussian lines robbed mostly of red,
    // spaced in stroke space so the grid scales with the pencil
    float kf = 0.1 * sc / sqrt(u);
    vec2 s2 = vec2(sin(pos.x * kf), sin(pos.y * kf));
    float lines = exp(-s2.x * s2.x * 80.0) + exp(-s2.y * s2.y * 80.0);
    vec3 karo = vec3(1.0, 1.0, 1.0)
              - vec3(0.25, 0.1, 0.1) * (0.5 * lines * clamp(grid, 0.0, 1.0));

    vec3 shade = mix(vec3(1.0, 1.0, 1.0), tint, clamp(chroma, 0.0, 1.0));
    return vec3(tone, tone, tone) * shade * karo;
}

// ---- analog television -----------------------------------------------------
// fs_ntsc — the composite video signal, actually modulated and demodulated.
//
// This is what separates a real "old TV" from scanline cosmetics: NTSC
// crams color into the luma wire by amplitude-modulating I/Q onto a
// 3.58 MHz subcarrier, and the receiver can never fully un-mix them.
// Everything people remember about the look IS that failure:
//   - color bleed    — chroma gets ~0.6 MHz where luma gets 4.2: I/Q come
//                      back through a much narrower low-pass than Y;
//   - rainbowing     — fine luma detail lands in the chroma band and
//                      demodulates as phantom color (`artifacts`);
//   - fringing       — chroma left inside Y rings edges with subcarrier
//                      checker (`fringing`);
//   - dot crawl      — the subcarrier phase advances per scanline and per
//                      frame, so the checker CRAWLS (host drives `time`).
// Implementation: per output pixel a 25-tap horizontal FIR (windowed-sinc,
// MAME ntsc.fx's single-pass structure — BSD-3; the artifacts/fringing
// cross-feed matrix follows the ntsc-adaptive formulation, reimplemented).
// Signal-domain noise rides the composite before demodulation (`noise`),
// so static tints and tears the picture the way RF interference does,
// instead of sprinkling RGB grain on top.
// One emulated scanline = one SOURCE pixel row; retro content should be
// fed at its native resolution.

vec3 fs_ntsc_yiq(vec3 c) {
    return vec3(dot(c, vec3(0.299, 0.587, 0.114)),
                dot(c, vec3(0.5959, -0.2746, -0.3213)),
                dot(c, vec3(0.2115, -0.5227, 0.3112)));
}

// windowed sinc: 2fc·sinc(2πfc·k)·Hamming(k/K) — the workhorse low-pass
float fs_ntsc_lp(float k, float fc) {
    float w = 0.54 + 0.46 * cos(3.14159265 * k / 12.0);
    float x = 6.2831853 * fc * k;
    float s = k == 0.0 ? 1.0 : sin(x) / x;
    return 2.0 * fc * s * w;
}

vec3 fs_ntsc(vec2 uv, float sharpness, float time_s, float artifacts,
             float fringing, float noise) {
    vec2 px = vec2(uv.x / FS_TEXEL.x, uv.y / FS_TEXEL.y);
    float line = floor(px.y);
    // subcarrier: 1/4 cycle per source pixel; phase flips π per scanline
    // and steps a third of a cycle per frame — the dot-crawl clock
    float fp = floor(time_s * 59.94);
    // phase bookkeeping in TURNS, wrapped to [0,1) before the trig — large
    // sin/cos arguments are where fast GPU trig and the CPU part company
    float line_turns = 0.5 * (line - 2.0 * floor(line / 2.0))
                     + (fp - 3.0 * floor(fp / 3.0)) / 3.0;
    float fc_y = 0.21 * clamp(sharpness, 0.3, 2.0);
    float fc_i = 0.065 * clamp(sharpness, 0.3, 2.0);
    float fc_q = 0.045 * clamp(sharpness, 0.3, 2.0);
    vec2 clamp_lo = FS_TEXEL * 3.0;
    vec2 clamp_hi = vec2(1.0, 1.0) - FS_TEXEL * 3.0;

    float acc_y = 0.0;
    float acc_i = 0.0;
    float acc_q = 0.0;
    float norm_y = 0.0;
    for (int k = -12; k <= 12; ++k) {
        float fk = float(k);
        vec2 p = vec2(uv.x + fk * FS_TEXEL.x, uv.y);
        vec3 yiq = fs_ntsc_yiq(FS_SAMPLE(clamp(p, clamp_lo, clamp_hi)));
        float turns = 0.25 * (px.x + fk) + line_turns;
        float ph = 6.2831853 * (turns - floor(turns));
        float cs = cos(ph);
        float sn = sin(ph);
        // the two wires: luma, and chroma riding the subcarrier
        float luma = yiq.x;
        float chroma = yiq.y * cs + yiq.z * sn;
        // RF static lives in the SIGNAL: it lands on both wires and the
        // demodulator will paint it as luma sparkle AND phantom color
        float rf = (fs_lsd_vnoise(vec2((px.x + fk) * 1.9,
                                       line * 3.7 + fp * 17.1)) - 0.5)
                 * (2.0 * clamp(noise, 0.0, 1.0));
        luma += rf;
        chroma += rf;
        float wy = fs_ntsc_lp(fk, fc_y);
        float wi = fs_ntsc_lp(fk, fc_i);
        float wq = fs_ntsc_lp(fk, fc_q);
        // cross-feed: fringing = chroma the luma notch fails to reject;
        // artifacts = luma detail the chroma demodulator mistakes for color
        acc_y += (luma + chroma * clamp(fringing, 0.0, 1.0)) * wy;
        acc_i += (chroma + luma * clamp(artifacts, 0.0, 1.0)) * 2.0 * cs * wi;
        acc_q += (chroma + luma * clamp(artifacts, 0.0, 1.0)) * 2.0 * sn * wq;
        norm_y += wy;
    }
    acc_y = acc_y / max(norm_y, 1e-4);
    // chroma keeps its true filter gain (dividing by the luma norm would
    // undo the bandwidth limit that makes color bleed)
    vec3 c = vec3(acc_y + 0.956 * acc_i + 0.619 * acc_q,
                  acc_y - 0.272 * acc_i - 0.647 * acc_q,
                  acc_y - 1.106 * acc_i + 1.703 * acc_q);
    return clamp(c, 0.0, 1.0);
}

// fs_crt — the tube itself, downstream of fs_ntsc (chain them: the signal
// degrades in the cable, THEN the phosphor draws it). Public-domain lineage:
// beam/mask/warp math after Timothy Lottes' CRT (PD), structure informed by
// Cathode-Retro (MIT). All light math runs in LINEAR space — royale, guest
// and Megatron all shout this: gamma-space scanlines read as thin gray
// lines, linear ones glow.
//   - scanline: each SOURCE row is one beam sweep with a gaussian cross
//     section whose width follows brightness (bright = fat beam — the
//     tube's own "bloom");
//   - deconvergence: R and B guns land a touch off-center;
//   - mask: 1 = aperture grille (Trinitron verticals), 2 = slot mask —
//     drawn at 3 stripes per source pixel (feed native-res content);
//   - glow: wide low bloom lifted from the 3×7 neighborhood already read;
//   - curvature: barrel warp + rounded-corner falloff + vignette.
vec3 fs_crt(vec2 uv, float curvature, float scan, float mask, float glow,
            float converge) {
    // barrel: displace along the radius, stronger on x (a 4:3 tube's glass)
    vec2 d = uv - vec2(0.5, 0.5);
    float r2 = dot(d, d);
    float k = clamp(curvature, 0.0, 0.4);
    vec2 wuv = vec2(0.5, 0.5) + d * (1.0 + k * r2 * vec2(1.0, 1.25));
    // rounded-corner gate, smooth so the bezel edge doesn't alias
    vec2 edge = vec2(0.5, 0.5) - abs(wuv - vec2(0.5, 0.5));
    float gate = smoothstep(0.0, 0.01, edge.x) * smoothstep(0.0, 0.01, edge.y);

    // Beam lattice in PRE-warp space, same anti-moiré reasoning as the mask
    // below; the row CONTENT is still fetched through the warp, so the
    // picture bends while the sweep pitch stays regular on screen.
    float py = uv.y / FS_TEXEL.y;
    float row0 = floor(py - 0.5) + 0.5;     // nearest beam center below
    float hard = 4.0 + 10.0 * clamp(scan, 0.0, 1.5);
    float cx = clamp(converge, 0.0, 3.0) * FS_TEXEL.x * 0.5;
    vec2 clamp_lo = FS_TEXEL * 3.0;
    vec2 clamp_hi = vec2(1.0, 1.0) - FS_TEXEL * 3.0;

    vec3 beam = vec3(0.0, 0.0, 0.0);
    vec3 halo = vec3(0.0, 0.0, 0.0);
    float halo_n = 0.0;
    for (int j = 0; j < 3; ++j) {
        // content row: the beam-lattice offset re-anchored on the warped y
        float ry = wuv.y + (row0 + float(j) - 1.0 - py) * FS_TEXEL.y;
        float dy = py - (row0 + float(j) - 1.0);
        for (int i = -3; i <= 3; ++i) {
            float rx = wuv.x + float(i) * FS_TEXEL.x;
            // deconvergence: three guns, three horizontal aims
            vec2 pr = clamp(vec2(rx + cx, ry), clamp_lo, clamp_hi);
            vec2 pg = clamp(vec2(rx, ry), clamp_lo, clamp_hi);
            vec2 pb = clamp(vec2(rx - cx, ry), clamp_lo, clamp_hi);
            vec3 s = vec3(FS_SAMPLE(pr).x, FS_SAMPLE(pg).y, FS_SAMPLE(pb).z);
            vec3 lin = s * s;               // cheap linearize (γ≈2)
            float wx = exp(-float(i * i) * 0.55);
            // luma-fattened beam: bright rows widen, dim rows pinch
            float width = 1.0 + 1.2 * fs_luma(lin);
            float wy = exp(-dy * dy * hard / width);
            beam += lin * (wx * wy);
            halo += lin * wx;
            halo_n += wx;
        }
    }
    beam = beam / 2.39;                     // Σ exp(-i²·0.55), i=-3..3
    halo = halo / max(halo_n, 1e-4);
    vec3 lit = beam + halo * (0.5 * clamp(glow, 0.0, 2.0));

    // Phosphor mask. One stripe per SOURCE pixel (a triad spans three) —
    // finer would be truer to glass, but a mask below the sampling rate
    // beats against the output grid as rainbow moiré (the trap crt-royale
    // spends two whole passes resampling around). At 1:1 this stays clean;
    // upscaled retro content gets visibly chunky triads, which is the point.
    // The mask lives in PRE-warp coordinates: warping its lattice makes it
    // beat against the output grid as concentric rainbow moiré (the glass
    // bends the picture; the viewer-facing stripe pitch stays regular).
    float m = floor(mask + 0.5);
    if (m >= 1.0) {
        float mx = uv.x / FS_TEXEL.x;
        if (m >= 2.0) {                     // slot: half-period stagger
            float col = floor(mx / 3.0);
            float phase = col - 2.0 * floor(col / 2.0);
            float my = uv.y / FS_TEXEL.y + 0.5 * phase;
            float slot = 0.8 + 0.2 * cos(3.14159265 * my);
            lit = lit * slot;
        }
        float stripe = mx - 3.0 * floor(mx / 3.0);
        vec3 grille = vec3(0.5, 0.5, 0.5);
        if (stripe < 1.0)      grille = vec3(1.0, 0.5, 0.5);
        else if (stripe < 2.0) grille = vec3(0.5, 1.0, 0.5);
        else                   grille = vec3(0.5, 0.5, 1.0);
        // mask eats light; win most of it back so the picture stays bright
        // (the Megatron idea, minus the HDR display)
        lit = lit * grille * 1.45;
    }

    float vign = 1.0 - 0.9 * k * r2;
    lit = lit * (gate * max(vign, 0.0));
    // back to display gamma (per channel — the C++ shim has no vec3 sqrt)
    return vec3(sqrt(max(lit.x, 0.0)), sqrt(max(lit.y, 0.0)),
                sqrt(max(lit.z, 0.0)));
}

// ---- dispatch --------------------------------------------------------------
// The only place slot values (p0..p4, in table order) meet parameter names.
// Returns rgba; alpha is 1 except for filters that define it.

vec4 fs_apply(int mode, vec2 uv, float p0, float p1, float p2, float p3, float p4) {
    vec3 c = FS_SAMPLE(uv);
    float alpha = 1.0;
    if (mode == FS_GRAYSCALE)            c = fs_grayscale(c);
    else if (mode == FS_SEPIA)           c = fs_sepia(c, p0);
    else if (mode == FS_INVERT)          c = fs_invert(c);
    else if (mode == FS_HUE)             c = fs_hue_rotate(c, p0);
    else if (mode == FS_EXPOSURE)        c = fs_exposure(c, p0);
    else if (mode == FS_WHITE_BALANCE)   c = fs_white_balance(c, p0, p1);
    else if (mode == FS_LEVELS)          c = fs_levels(c, p0, p1, p2);
    else if (mode == FS_THRESHOLD)       c = fs_threshold(c, p0);
    else if (mode == FS_POSTERIZE)       c = fs_posterize(c, p0);
    else if (mode == FS_VIGNETTE)        c = fs_vignette(c, uv, p0, p1);
    else if (mode == FS_PIXELATE)        c = fs_pixelate(uv, p0);
    else if (mode == FS_SHARPEN)         c = fs_sharpen(uv, p0);
    else if (mode == FS_EMBOSS)          c = fs_emboss(uv, p0);
    else if (mode == FS_EDGE_SOBEL)      c = fs_edge_sobel(uv, p0);
    else if (mode == FS_SKETCH)          c = fs_sketch(uv, p0);
    else if (mode == FS_TOON)            c = fs_toon(uv, p0, p1);
    else if (mode == FS_SWIRL)           c = fs_swirl(uv, p0, p1);
    else if (mode == FS_SOLARIZE)        c = fs_solarize(c, p0);
    else if (mode == FS_MOTION_BLUR)     c = fs_motion_blur(uv, p0, p1);
    else if (mode == FS_ZOOM_BLUR)       c = fs_zoom_blur(uv, p0);
    else if (mode == FS_RGB)             c = fs_rgb(c, p0, p1, p2);
    else if (mode == FS_OPACITY)         alpha = clamp(p0, 0.0, 1.0);
    else if (mode == FS_HAZE)            c = fs_haze(c, uv, p0, p1);
    else if (mode == FS_HALFTONE)        c = fs_halftone(uv, p0);
    else if (mode == FS_COLOR_TRANSFORM) c = fs_color_transform(c, p0, p1, p2, p3);
    else if (mode == FS_BILATERAL)       c = fs_bilateral(uv, p0, p1);
    else if (mode == FS_MEDIAN)          c = fs_median(uv);
    else if (mode == FS_BEAUTY)          c = fs_beauty(uv, p0, p1, p2, p3, p4);
#ifdef FS_SAMPLE_LUT
    else if (mode == FS_LUT)             c = fs_lut(uv, p0, p1, p2, p3);
    else if (mode == FS_FACE)            c = fs_face(uv, p0, p1, p2, p3);
#endif
    else if (mode == FS_RIPPLE)          c = fs_ripple(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_LSD)             c = fs_lsd(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_NOTEBOOK)        c = fs_notebook(uv, p0, p1, p2);
    else if (mode == FS_WOBBLE)          c = fs_wobble(uv, p0, p1, p2, p3);
    else if (mode == FS_FRACTAL)         c = fs_fractal(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_BOKEH)           c = fs_bokeh(uv, p0, p1, p2, p3);
    else if (mode == FS_OILPAINT)        c = fs_oilpaint(uv, p0, p1, p2);
    else if (mode == FS_NTSC)            c = fs_ntsc(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_CRT)             c = fs_crt(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_ANIME)           c = fs_anime(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_WATERCOLOR)      c = fs_watercolor(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_SUMIE)           c = fs_sumie(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_IMPRESSIONIST)   c = fs_impressionist(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_STAINEDGLASS)    c = fs_stainedglass(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_PIXELART)        c = fs_pixelart(uv, p0, p1, p2, p3);
    return vec4(clamp(c, 0.0, 1.0), alpha);
}

#endif  // FS_SAMPLE

