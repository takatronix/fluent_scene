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

#ifdef FS_SAMPLE

float fs_luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

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
// gpupixel's calibration (their delta=7.07 squared). `whiten` approximates
// the original's baked LUT chain with a levels lift + gamma curve;
// `sharpen` restores micro-contrast the blend absorbs (4-tap unsharp).
vec3 fs_beauty(vec2 uv, float smoothing, float whiten, float radius, float sharpen_amount) {
    vec3 c = FS_SAMPLE(uv);
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
    float skin = clamp((min(c.x, mean.x - 0.1) - 0.2) * 4.0, 0.0, 1.0);
    float k = (0.1 / (mean_var + 0.1)) * skin * clamp(smoothing, 0.0, 1.0);
    vec3 result = mix(c, mean, clamp(k, 0.0, 1.0));
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
        result = mix(result, bright, clamp(whiten, 0.0, 1.0));
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
// mixes the graded color over the source.
vec3 fs_lut(vec2 uv, float amount, float tiles, float tile_n) {
    vec3 c = clamp(FS_SAMPLE(uv), 0.0, 1.0);
    float atlas = tiles * tile_n;
    float slice = c.z * (tile_n - 1.0);
    float s0 = floor(slice);
    float s1 = min(s0 + 1.0, tile_n - 1.0);
    vec2 inner = vec2(c.x, c.y) * (tile_n - 1.0) + vec2(0.5, 0.5);
    vec2 uv0 = (vec2(s0 - floor(s0 / tiles) * tiles, floor(s0 / tiles)) * tile_n + inner) / atlas;
    vec2 uv1 = (vec2(s1 - floor(s1 / tiles) * tiles, floor(s1 / tiles)) * tile_n + inner) / atlas;
    vec3 graded = mix(FS_SAMPLE_LUT(uv0), FS_SAMPLE_LUT(uv1), slice - s0);
    return mix(c, graded, clamp(amount, 0.0, 1.0));
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
    // the singularity wanders slowly — hallucinated tunnels aren't pinned
    // to the fixation point
    vec2 d0 = uv - vec2(0.5 + 0.05 * k * sin(t * 0.13),
                        0.5 + 0.05 * k * sin(t * 0.17 + 1.0));
    vec2 pc = vec2(d0.x * (FS_TEXEL.y / FS_TEXEL.x), d0.y);
    float r = max(length(pc), 1e-4);

    // the hallucination field comes FIRST: it also drives the image warp,
    // so the drift itself is organic — no sinusoidal wallpaper anywhere
    // ETERNAL ZOOM — the defining 3D sensation: flying forward while the
    // whole field expands, forever. Three log2-spaced scale layers
    // crossfade under a hann window on the depth exponent; a layer that
    // grows past the window fades out while a fresh far layer fades in,
    // decorrelated by its integer flight index — so the expansion never
    // ends and never repeats. A low-frequency warp bends every layer:
    // still no lines, no right angles, no circles.
    vec2 qw0 = pc * 1.3 + vec2(t * 0.021, 0.0 - t * 0.017);
    float n1 = fs_lsd_vnoise(qw0);
    float n2 = fs_lsd_vnoise(qw0 * 1.9 + vec2(7.7, 3.1));
    vec2 warp = vec2(n1 - 0.5, n2 - 0.5);
    float zp = t * 0.3;                                    // flight speed
    float zf = zp - floor(zp);
    float fieldv = 0.0;                                    // hue carrier
    float wsum = 0.0;
    float mass = 0.0;                                      // depth-layered masses
    float mass_ph = 0.0;                                   // hue of nearest mass
    for (int i = 0; i < 3; ++i) {
        float e = float(i) - zf;                           // depth exponent
        float sc = exp2(e);                                // shrinks → expands
        float m = float(i) + floor(zp);                    // stable layer id
        vec2 off = vec2(fs_lsd_hash(vec2(m, 1.7)) * 61.3,
                        fs_lsd_hash(vec2(m, 9.2)) * 47.7);
        float w = 0.5 - 0.5 * cos(2.0943951 * (e + 1.0));  // hann on [-1, 2]
        vec2 pl = pc * (3.0 * sc) + warp * (2.2 * sc) + off;
        float nv = fs_lsd_vnoise(pl) * 0.5 +
                   fs_lsd_vnoise(vec2(pl.x * 2.1 + 31.7, pl.y * 2.1 - 17.3)) * 0.3 +
                   fs_lsd_vnoise(vec2(pl.x * 4.3 - 13.1, pl.y * 4.7 + 23.9)) * 0.2;
        fieldv += w * nv;
        wsum += w;
        // each depth shell contributes its own masses, thresholded BEFORE
        // blending (contrast survives), nearer shells brighter, max-combined
        // so near masses read as occluding far ones — this is the 3D. Each
        // shell also owns a hue (golden-angle spaced), so depth separates
        // in color the way parallax separates in motion
        float near_f = smoothstep(2.0, -1.0, e);
        float clv = smoothstep(0.55, 0.75, nv) * w * (0.5 + 0.5 * near_f);
        float phm = m * 2.39996 + nv * 2.0 + t * 0.15;
        mass_ph = clv > mass ? phm : mass_ph;
        mass = max(mass, clv);
    }
    fieldv /= max(wsum, 0.1);

    // drifting & breathing: read the source through the noise field's lens.
    // Less than one radial period fits the view and the phase is noised,
    // so the swell never reads as concentric rings
    float breath = 1.0 + 0.045 * k * drift * sin(t * 0.9 - r * 1.2 + n1 * 2.0);
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
    col = fs_hue_rotate(col, 28.0 * k * sin(t * 0.19));

    // fractal masses from the field computed above: amorphous blobs at
    // every scale, streaming outward, warped by their own values — no
    // straight line, no right angle, no circle survives anywhere.
    float chunk = mass;                                    // amorphous masses
    // the shoreline hugs each mass edge — an organic contour, not a line,
    // and never a wash over empty regions
    float shore = smoothstep(0.06, 0.2, chunk) * (1.0 - smoothstep(0.2, 0.5, chunk));
    // hallucinated geometry rides on weak retinal drive (shadow, flat wall)
    float gate = clamp(geometry, 0.0, 1.5) * k * (0.2 + 0.8 * (1.0 - fs_luma(col)));
    // hue: the nearest mass's own color; faint fieldv hue where nothing is
    float ph = chunk > 0.05 ? mass_ph : fieldv * 6.3 + t * 0.4;
    vec3 pcol = vec3(0.5 + 0.5 * cos(ph), 0.5 + 0.5 * cos(ph - 2.094),
                     0.5 + 0.5 * cos(ph + 2.094));
    col = col * (1.0 - 0.35 * gate * chunk) +
          pcol * (gate * (0.5 * chunk + 0.45 * shore));
    return col;
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
    else if (mode == FS_BEAUTY)          c = fs_beauty(uv, p0, p1, p2, p3);
#ifdef FS_SAMPLE_LUT
    else if (mode == FS_LUT)             c = fs_lut(uv, p0, p1, p2);
#endif
    else if (mode == FS_RIPPLE)          c = fs_ripple(uv, p0, p1, p2, p3, p4);
    else if (mode == FS_LSD)             c = fs_lsd(uv, p0, p1, p2, p3, p4);
    return vec4(clamp(c, 0.0, 1.0), alpha);
}

#endif  // FS_SAMPLE

