// filters_def.h — the single source of truth for filter METADATA: the public
// name, the typed-struct name, and every parameter's name, default, and unit.
// filters.hpp includes this file several times with different definitions of
// the three macros to derive, from this one list:
//
//   - the typed chainable structs   (Bilateral().radius(4).sigma_color(0.2))
//   - their default values
//   - the runtime spec table        (Scene parsing, docs, capability JSON)
//
// Contract with filters_shared.h (the body source):
//   - MODE is a FS_* id declared there;
//   - parameters appear in slot order — FS_PARAM's `slot` must equal its
//     position in the block (0, 1, 2, …) and must match the p0..p4 order the
//     fs_apply dispatch passes to the body.
//
// Units: `Length` parameters are logical units — the renderer multiplies
// them by its logical→source-pixel factor before the body sees them, so
// `blur(4)` looks identical on a 640×480 and a 4K source (§5-3 of the
// design). `Scalar` parameters pass through untouched (ratios, angles,
// normalized 0–1 positions). `CoordX`/`CoordY` are positions in the
// layer's local logical space; the renderer maps them into the filter
// pass's normalized uv (so bodies get a ready-to-use uv center).
//
// No include guard: this file is included multiple times by design.
//
//   FS_FILTER(TypeName, method, MODE, summary_utf8)
//   FS_PARAM(slot, name, default, unit)
//   FS_END(TypeName)
//
// Filters that read a second image (`lut`) open with FS_FILTER_IMG instead,
// naming the image parameter as it appears in Scene YAML; FS_PARAM/FS_END
// are shared with the plain form. Includers that don't care about the image
// parameter get the plain expansion for free:
//
//   FS_FILTER_IMG(TypeName, method, MODE, image_name, summary_utf8)

#ifndef FS_FILTER_IMG
#define FS_FILTER_IMG(TypeName, method, MODE, image_name, summary) \
    FS_FILTER(TypeName, method, MODE, summary)
#define FS_FILTER_IMG_DEFAULTED
#endif

// Stateful filters (persistent_buffers P1) carry a per-layer ping-pong
// state buffer the renderer advances each frame via fs_apply_state before
// the normal output pass. Includers that don't care see a plain filter.
#ifndef FS_FILTER_STATEFUL
#define FS_FILTER_STATEFUL(TypeName, method, MODE, summary) \
    FS_FILTER(TypeName, method, MODE, summary)
#define FS_FILTER_STATEFUL_DEFAULTED
#endif

// Stateful AND carrying an image parameter (sumie's paper). Degrades to
// FS_FILTER_IMG so includers keep the image setter without knowing about
// state.
#ifndef FS_FILTER_STATEFUL_IMG
#define FS_FILTER_STATEFUL_IMG(TypeName, method, MODE, image_name, summary) \
    FS_FILTER_IMG(TypeName, method, MODE, image_name, summary)
#define FS_FILTER_STATEFUL_IMG_DEFAULTED
#endif

// ---- color and tone --------------------------------------------------------

FS_FILTER(Grayscale, grayscale, FS_GRAYSCALE, "grayscale (luma)")
FS_END(Grayscale)

FS_FILTER(Sepia, sepia, FS_SEPIA, "sepia tone")
FS_PARAM(0, intensity, 1.0f, Scalar)
FS_END(Sepia)

FS_FILTER(Invert, invert, FS_INVERT, "invert (negative)")
FS_END(Invert)

FS_FILTER(Hue, hue, FS_HUE, "hue rotation (degrees)")
FS_PARAM(0, angle, 90.0f, Scalar)
FS_END(Hue)

FS_FILTER(Exposure, exposure, FS_EXPOSURE, "exposure (EV stops)")
FS_PARAM(0, stops, 0.5f, Scalar)
FS_END(Exposure)

FS_FILTER(WhiteBalance, white_balance, FS_WHITE_BALANCE, "white balance: temperature and tint")
FS_PARAM(0, temperature, 0.3f, Scalar)
FS_PARAM(1, tint, 0.0f, Scalar)
FS_END(WhiteBalance)

FS_FILTER(Levels, levels, FS_LEVELS, "levels (input range and gamma)")
FS_PARAM(0, min, 0.0f, Scalar)
FS_PARAM(1, max, 1.0f, Scalar)
FS_PARAM(2, gamma, 1.0f, Scalar)
FS_END(Levels)

FS_FILTER(Threshold, threshold, FS_THRESHOLD, "luma threshold (binarize)")
FS_PARAM(0, threshold, 0.5f, Scalar)
FS_END(Threshold)

FS_FILTER(Posterize, posterize, FS_POSTERIZE, "posterize (reduce tone levels)")
FS_PARAM(0, levels, 6.0f, Scalar)
FS_END(Posterize)

FS_FILTER(Solarize, solarize, FS_SOLARIZE, "solarize")
FS_PARAM(0, threshold, 0.5f, Scalar)
FS_END(Solarize)

FS_FILTER(Rgb, rgb, FS_RGB, "per-channel RGB gain")
FS_PARAM(0, red, 1.0f, Scalar)
FS_PARAM(1, green, 1.0f, Scalar)
FS_PARAM(2, blue, 1.0f, Scalar)
FS_END(Rgb)

FS_FILTER(Haze, haze, FS_HAZE, "haze removal / addition")
FS_PARAM(0, distance, 0.15f, Scalar)
FS_PARAM(1, slope, 0.0f, Scalar)
FS_END(Haze)

// The LUT atlas comes through the layer's filter-image binding
// (`source: $inputs.<name>` in Scene YAML, `Lut().source(view)` in C++);
// the renderer derives the grid geometry from the atlas width.
FS_FILTER_IMG(Lut, lut, FS_LUT, source, "3D LUT color grade from a tiled atlas image")
FS_PARAM(0, amount, 1.0f, Scalar)
FS_PARAM(1, skin, 0.0f, Scalar)
FS_END(Lut)

// The 44×1 control strip comes from a landmark analyzer through the image
// parameter (`points: $inputs.<name>`); see fs_face in filters_shared.h for
// the layout. No strip = pass-through.
FS_FILTER_IMG(Face, face, FS_FACE, points,
              "landmark-driven face warp and makeup (eye, slim, lip, cheek)")
FS_PARAM(0, eye, 0.0f, Scalar)
FS_PARAM(1, slim, 0.0f, Scalar)
FS_PARAM(2, lip, 0.0f, Scalar)
FS_PARAM(3, cheek, 0.0f, Scalar)
FS_END(Face)

FS_FILTER(ColorTransform, color_transform, FS_COLOR_TRANSFORM,
          "brightness, contrast, saturation, gamma")
FS_PARAM(0, brightness, 0.0f, Scalar)
FS_PARAM(1, contrast, 1.0f, Scalar)
FS_PARAM(2, saturation, 1.0f, Scalar)
FS_PARAM(3, gamma, 1.0f, Scalar)
FS_END(ColorTransform)

FS_FILTER(Opacity, opacity, FS_OPACITY, "opacity (prefer the layer attribute)")
FS_PARAM(0, opacity, 0.7f, Scalar)
FS_END(Opacity)

FS_FILTER(Vignette, vignette, FS_VIGNETTE, "vignette")
FS_PARAM(0, start, 0.35f, Scalar)
FS_PARAM(1, end, 0.85f, Scalar)
FS_END(Vignette)

// ---- smoothing and denoise -------------------------------------------------

FS_FILTER(Blur, blur, FS_BLUR, "gaussian blur (separable, two passes)")
FS_PARAM(0, radius, 4.0f, Length)
FS_END(Blur)

FS_FILTER(Bilateral, bilateral, FS_BILATERAL, "bilateral (edge-preserving denoise)")
FS_PARAM(0, radius, 4.0f, Length)
FS_PARAM(1, sigma_color, 0.15f, Scalar)
FS_END(Bilateral)

FS_FILTER(Median, median, FS_MEDIAN, "median 3x3 (salt-and-pepper denoise)")
FS_END(Median)

FS_FILTER(Beauty, beauty, FS_BEAUTY, "beauty (variance-gated skin smoothing + whitening)")
FS_PARAM(0, smoothing, 0.7f, Scalar)
FS_PARAM(1, whiten, 0.0f, Scalar)
FS_PARAM(2, radius, 10.0f, Length)
FS_PARAM(3, sharpen, 0.15f, Scalar)
FS_PARAM(4, denoise, 0.0f, Scalar)
FS_END(Beauty)

// ---- edges and stylize -----------------------------------------------------

FS_FILTER(Sharpen, sharpen, FS_SHARPEN, "sharpen")
FS_PARAM(0, strength, 1.0f, Scalar)
FS_END(Sharpen)

FS_FILTER(Emboss, emboss, FS_EMBOSS, "emboss")
FS_PARAM(0, strength, 1.0f, Scalar)
FS_END(Emboss)

FS_FILTER(EdgeSobel, edge_sobel, FS_EDGE_SOBEL, "Sobel edge detection")
FS_PARAM(0, strength, 1.0f, Scalar)
FS_END(EdgeSobel)

FS_FILTER(Sketch, sketch, FS_SKETCH, "sketch (line drawing)")
FS_PARAM(0, strength, 1.0f, Scalar)
FS_END(Sketch)

FS_FILTER(Toon, toon, FS_TOON, "toon (posterize + edges)")
FS_PARAM(0, levels, 6.0f, Scalar)
FS_PARAM(1, edge, 0.2f, Scalar)
FS_END(Toon)

// ---- resampling ------------------------------------------------------------

FS_FILTER(Pixelate, pixelate, FS_PIXELATE, "pixelate (mosaic)")
FS_PARAM(0, size, 12.0f, Length)
FS_END(Pixelate)

FS_FILTER(Swirl, swirl, FS_SWIRL, "swirl distortion")
FS_PARAM(0, radius, 0.5f, Scalar)
FS_PARAM(1, angle, 1.0f, Scalar)
FS_END(Swirl)

FS_FILTER(MotionBlur, motion_blur, FS_MOTION_BLUR, "motion blur")
FS_PARAM(0, angle, 0.0f, Scalar)
FS_PARAM(1, distance, 12.0f, Length)
FS_END(MotionBlur)

FS_FILTER(ZoomBlur, zoom_blur, FS_ZOOM_BLUR, "zoom blur")
FS_PARAM(0, strength, 0.15f, Scalar)
FS_END(ZoomBlur)

FS_FILTER(Bokeh, bokeh, FS_BOKEH,
          "lens bokeh — aperture-shaped highlight blur (blades: 0 = round "
          "iris, 3+ = polygon)")
FS_PARAM(0, radius, 8.0f, Length)
FS_PARAM(1, blades, 0.0f, Scalar)
FS_PARAM(2, rotation, 0.0f, Scalar)
FS_PARAM(3, highlight, 1.0f, Scalar)
FS_END(Bokeh)

FS_FILTER(Oilpaint, oilpaint, FS_OILPAINT,
          "oil painting (Kuwahara) — edge-preserving brush daubs; chain "
          "`median` after it for a varnished finish")
FS_PARAM(0, radius, 4.0f, Length)
FS_PARAM(1, levels, 0.0f, Scalar)
FS_PARAM(2, jitter, 0.6f, Scalar)
FS_END(Oilpaint)

// ---- painting and drawing styles -------------------------------------------
// Own implementations from the papers — no borrowed shader code, MIT like the
// rest of the catalog. Survey, algorithm choices and parameter rationale:
// docs/design/art_filters.ja.md.

FS_FILTER(Anime, anime, FS_ANIME,
          "anime cel shading — chroma-preserving soft luma quantization + "
          "XDoG ink lines (after Winnemoeller 2006/2011)")
FS_PARAM(0, levels, 5.0f, Scalar)
FS_PARAM(1, lines, 1.0f, Scalar)
FS_PARAM(2, width, 1.6f, Length)
FS_PARAM(3, smooth, 0.7f, Scalar)
FS_PARAM(4, vivid, 0.3f, Scalar)
FS_END(Anime)

FS_FILTER(Watercolor, watercolor, FS_WATERCOLOR,
          "watercolor wash — pigment-density model with edge darkening, "
          "granulation and paper (after Bousseau 2006)")
FS_PARAM(0, wash, 6.0f, Length)
FS_PARAM(1, edge, 0.9f, Scalar)
FS_PARAM(2, grain, 0.7f, Scalar)
FS_PARAM(3, wobble, 1.0f, Scalar)
FS_PARAM(4, dilute, 0.35f, Scalar)
FS_END(Watercolor)

// The optional `paper` image (a real washi scan) replaces the procedural
// paper; the renderer signals its presence by adding +2 to the chroma slot
// (plan::scaleFilterValues) — no sixth parameter slot exists.
FS_FILTER_STATEFUL_IMG(Sumie, sumie, FS_SUMIE, paper,
          "living sumi-e — bold contour ink deposited into a persistent "
          "ink/water field that DIFFUSES through the paper frame to frame "
          "(Chinese ink painting; the bleed genuinely moves). Feed `paper` "
          "a washi scan for the real sheet")
FS_PARAM(0, ink, 1.0f, Scalar)
FS_PARAM(1, bleed, 6.0f, Length)
FS_PARAM(2, dry, 0.6f, Scalar)
FS_PARAM(3, outline, 0.8f, Scalar)
FS_PARAM(4, chroma, 0.0f, Scalar)
FS_END(Sumie)

FS_FILTER(Impressionist, impressionist, FS_IMPRESSIONIST,
          "impressionist oil — long brush strokes riding a swirling curl-"
          "noise field (van Gogh vortices), broken color, impasto (after "
          "Litwinowicz 1997; host drives `time` for a living painting)")
FS_PARAM(0, stroke, 7.0f, Length)
FS_PARAM(1, time, 0.0f, Scalar)
FS_PARAM(2, vibrance, 0.8f, Scalar)
FS_PARAM(3, flow, 0.8f, Scalar)
FS_PARAM(4, relief, 0.7f, Scalar)
FS_END(Impressionist)

FS_FILTER(Stainedglass, stainedglass, FS_STAINEDGLASS,
          "stained glass — cellular panes (Worley F1/F2) with lead came, "
          "per-pane tint and transmitted light")
FS_PARAM(0, size, 24.0f, Length)
FS_PARAM(1, lead, 0.8f, Scalar)
FS_PARAM(2, irregular, 0.85f, Scalar)
FS_PARAM(3, saturate, 0.5f, Scalar)
FS_PARAM(4, light, 0.6f, Scalar)
FS_END(Stainedglass)

FS_FILTER(Pixelart, pixelart, FS_PIXELART,
          "pixel art — block resample, per-channel quantization and Bayer "
          "4x4 ordered dithering")
FS_PARAM(0, size, 6.0f, Length)
FS_PARAM(1, colors, 5.0f, Scalar)
FS_PARAM(2, dither, 0.5f, Scalar)
FS_PARAM(3, saturate, 0.25f, Scalar)
FS_END(Pixelart)

FS_FILTER_STATEFUL(Inkline, inkline, FS_INKLINE,
          "coherent ink lines (ETF/FDoG, Kang 2007) — the edge tangent "
          "flow lives in a persistent field, one smoothing iteration per "
          "frame; lines composite over the source, or set `matte` for "
          "lines-on-transparent to stack as its own layer")
FS_PARAM(0, width, 1.6f, Length)
FS_PARAM(1, ink, 1.0f, Scalar)
FS_PARAM(2, coherence, 0.8f, Scalar)
FS_PARAM(3, detail, 1.0f, Scalar)
FS_PARAM(4, matte, 0.0f, Scalar)
FS_END(Inkline)

// ---- analog television -----------------------------------------------------
// The pair reads: signal first, tube second —
//   layer.filter(Ntsc().time(t)).filter(Crt())
// Own implementations: NTSC after MAME ntsc.fx (BSD-3) & the ntsc-adaptive
// cross-feed formulation; CRT after Timothy Lottes (public domain) and
// Cathode-Retro (MIT). No GPL code.

FS_FILTER(Ntsc, ntsc, FS_NTSC,
          "NTSC composite signal — color bleed, rainbowing, dot crawl "
          "(host drives `time`); chain `crt` after it")
FS_PARAM(0, sharpness, 1.0f, Scalar)
FS_PARAM(1, time, 0.0f, Scalar)
FS_PARAM(2, artifacts, 0.6f, Scalar)
FS_PARAM(3, fringing, 0.4f, Scalar)
FS_PARAM(4, noise, 0.0f, Scalar)
FS_END(Ntsc)

FS_FILTER(Crt, crt, FS_CRT,
          "CRT tube — linear-light scanline beam, phosphor mask (1 = "
          "grille, 2 = slot), glow, deconvergence, curvature")
FS_PARAM(0, curvature, 0.12f, Scalar)
FS_PARAM(1, scan, 0.6f, Scalar)
FS_PARAM(2, mask, 1.0f, Scalar)
FS_PARAM(3, glow, 0.35f, Scalar)
FS_PARAM(4, converge, 0.5f, Scalar)
FS_END(Crt)

FS_FILTER(Halftone, halftone, FS_HALFTONE, "halftone dots")
FS_PARAM(0, spacing, 8.0f, Length)
FS_END(Halftone)

FS_FILTER(RippleWave, ripple, FS_RIPPLE, "water refraction wave (expanding wavefront)")
FS_PARAM(0, center_x, 0.0f, CoordX)
FS_PARAM(1, center_y, 0.0f, CoordY)
FS_PARAM(2, radius, 60.0f, Length)
FS_PARAM(3, amplitude, 6.0f, Length)
FS_PARAM(4, wavelength, 26.0f, Length)
FS_END(RippleWave)

// ---- psychedelia -----------------------------------------------------------

FS_FILTER(Lsd, lsd, FS_LSD,
          "LSD trip (drift, breathing, Klüver form constants; host drives `time`)")
FS_PARAM(0, trip, 0.8f, Scalar)
FS_PARAM(1, time, 0.0f, Scalar)
FS_PARAM(2, drift, 1.0f, Scalar)
FS_PARAM(3, geometry, 0.5f, Scalar)
FS_PARAM(4, chroma, 1.0f, Scalar)
FS_END(Lsd)

// ---- generative ------------------------------------------------------------

FS_FILTER(Fractal, fractal, FS_FRACTAL,
          "eternal fractal flight (self-morphing KIFS raymarch, orbit-trap "
          "iridescence; host drives `time`)")
FS_PARAM(0, flight, 1.0f, Scalar)
FS_PARAM(1, time, 0.0f, Scalar)
FS_PARAM(2, morph, 1.0f, Scalar)
FS_PARAM(3, glow, 1.0f, Scalar)
FS_PARAM(4, blend, 0.0f, Scalar)
FS_END(Fractal)

// ---- hand drawing ----------------------------------------------------------
// After flockaroo's "notebook drawings" (shadertoy XtVGD1), CC BY-NC-SA 3.0
// — NON-COMMERCIAL use only; see fs_notebook in filters_shared.h.

FS_FILTER(Notebook, notebook, FS_NOTEBOOK,
          "pencil sketch on squared paper (after flockaroo XtVGD1, "
          "CC BY-NC-SA — non-commercial); chain `wobble` for page shiver")
FS_PARAM(0, scale, 0.5f, Scalar)
FS_PARAM(1, grid, 1.0f, Scalar)
FS_PARAM(2, chroma, 1.0f, Scalar)
FS_END(Notebook)

FS_FILTER(Wobble, wobble, FS_WOBBLE,
          "hand-drawn wobble — noise-warped reads, redrawn `fps` times a "
          "second for the stop-motion shiver (host drives `time`); chain "
          "before or after any filter")
FS_PARAM(0, amount, 4.0f, Length)
FS_PARAM(1, time, 0.0f, Scalar)
FS_PARAM(2, scale, 1.0f, Scalar)
FS_PARAM(3, fps, 6.0f, Scalar)
FS_END(Wobble)

#ifdef FS_FILTER_IMG_DEFAULTED
#undef FS_FILTER_IMG
#undef FS_FILTER_IMG_DEFAULTED
#endif

#ifdef FS_FILTER_STATEFUL_DEFAULTED
#undef FS_FILTER_STATEFUL
#undef FS_FILTER_STATEFUL_DEFAULTED
#endif

#ifdef FS_FILTER_STATEFUL_IMG_DEFAULTED
#undef FS_FILTER_STATEFUL_IMG
#undef FS_FILTER_STATEFUL_IMG_DEFAULTED
#endif
