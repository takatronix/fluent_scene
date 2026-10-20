#pragma once

/// \file content.hpp
/// \brief Content — what a layer draws (§7 of the design).
///
/// A layer holds **zero or one** content; a layer with no content and
/// sublayers is a group. Content stores only *what* to draw, in the layer's
/// own bounds space; *where and how it composites* (position, rotation,
/// opacity, shadow, …) are layer attributes, never content fields (principle
/// S-3). Shared styling — color, thickness, dash, cap, corner radius — also
/// lives on the layer so every content type spells it identically.
///
/// All shapes render as signed distance fields (shared/shapes_shared.h):
/// antialiased, and re-evaluated (not upscaled) at any output resolution.

#include <string>
#include <variant>
#include <vector>

#include "fluent_scene/geometry.hpp"
#include "fluent_scene/text.hpp"
#include "fluent_scene/types.hpp"

namespace fluent_scene {

/// A borrowed image mapped into the layer bounds with a Fit policy.
/// `source_rect` (CALayer `contentsRect` in spirit, but in **source pixels**,
/// never 0–1) cuts a sub-rectangle out of the source first — partial
/// copy-and-paste is this plus `frame()` (§5-2b). A zero-size source_rect
/// means the whole image.
struct ImageContent {
    ImageView view;             ///< Borrowed pixels; valid until render.
    Fit fit = Fit::Contain;     ///< Mapping into the layer bounds.
    Rect source_rect{};         ///< Source crop in pixels; zero size = all.
};

/// UTF-8 text (Japanese included; shaped by HarfBuzz, drawn from a dynamic
/// glyph atlas — exact-size bitmaps for small upright text, signed distance
/// fields for large / transformed / outlined text; docs/design/text.md).
/// `position` is the reference point in layer space: by default the
/// top-left of the first line box (Align / VAlign move the reference edge).
/// Missing glyphs draw a deterministic replacement box.
///
/// Every field past `align` is optional styling; the defaults reproduce
/// plain single-line text exactly as before.
struct TextContent {
    std::string utf8;           ///< The text ('\n' starts a new line).
    Vec2 position{};            ///< Reference point in layer space.
    float size = 26;            ///< Font size in logical units (pt() converts).
    Align align = Align::Left;  ///< Horizontal: which edge `position` refers
                                ///< to; also aligns lines inside the block.
    VAlign valign = VAlign::Top;  ///< Vertical reference edge.

    // ---- font ----
    std::string font;           ///< Family ("sans", "serif", "mono", or a
                                ///< registerFont name); empty = "sans".
    int weight = 400;           ///< 100–900; ≥ 600 picks a Bold face when one
                                ///< is registered, otherwise emboldens.
    bool italic = false;        ///< Italic face, or a synthetic 12° slant.
    float letter_spacing = 0;   ///< Extra advance after each character.
    float line_height = 0;      ///< Baseline pitch as a multiple of size;
                                ///< 0 = the font's own (≈1.45 for Noto CJK).
    bool tabular = false;       ///< Fixed-width digits (OpenType tnum, plus
                                ///< equalized advances) — numbers that
                                ///< change every frame do not jitter.

    // ---- layout box ----
    float max_width = 0;        ///< Wrap / truncate width; 0 = unbounded.
    float max_height = 0;       ///< Lines that would exceed it are dropped.
    bool wrap = true;           ///< Wrap at max_width (Japanese kinsoku
                                ///< rules); false keeps one line per '\n'.
    int max_lines = 0;          ///< 0 = unlimited.
    bool ellipsis = false;      ///< Mark truncated text with "…".
    float min_size = 0;         ///< Auto-fit: when > 0, shrink `size` (not
                                ///< below this) until the text fits
                                ///< max_width / max_height / max_lines.

    // ---- paint ----
    TextOutline outline;        ///< Stroke around the glyphs.
    TextGradient gradient;      ///< Fill gradient (else the layer color).

    /// Rich text: when non-empty, these spans replace `utf8` and may
    /// override font / size / weight / italic / color per run.
    std::vector<TextSpan> spans;
};

/// A single straight line from `from` to `to`.
struct LineContent {
    Vec2 from{};
    Vec2 to{};
};

/// An open path through `points` (round joints; capsule SDF strokes).
struct PolylineContent {
    std::vector<Vec2> points;
};

/// A closed path through `points`: filled when the layer's thickness is 0,
/// outlined otherwise.
struct PolygonContent {
    std::vector<Vec2> points;
};

/// A rectangle (the layer's cornerRadius rounds it; thickness 0 fills).
struct RectContent {
    Rect rect{};
};

/// A circle (thickness 0 fills; otherwise a ring).
struct CircleContent {
    Vec2 center{};
    float radius = 0;
};

/// Markers at each point — the point-cloud content. Thickness 0 draws
/// filled dots; a positive thickness draws rings.
struct CirclesContent {
    std::vector<Vec2> centers;
    float radius = 6;           ///< Marker radius in logical units.
};

/// A circular arc from `start_deg` to `end_deg` (degrees; 0° = +x,
/// increasing clockwise on screen). Gauges and angle indicators.
/// L0 draws round caps; Cap::Butt on arcs is a documented L1 item.
struct ArcContent {
    Vec2 center{};
    float radius = 0;
    float start_deg = 0;
    float end_deg = 0;
};

/// An arrow from `from` to `to` with a filled triangular head.
struct ArrowContent {
    Vec2 from{};
    Vec2 to{};
    float head_size = 0;        ///< Head length in logical units; 0 = auto
                                ///< (scales with the layer's thickness).
};

/// A crosshair: four ticks around a center with a central gap.
struct CrosshairContent {
    Vec2 center{};
    float size = 24;            ///< Overall half-extent from the center.
    float gap = 0;              ///< Central gap radius; 0 = auto (size / 4).
};

/// An axis-aligned debug/calibration grid across the whole layer,
/// lines every `spacing` logical units (through the layer origin).
struct GridContent {
    float spacing = 100;
};

/// Data-driven detection boxes with optional temporal smoothing.
/// `targets` is what the caller set; `smoothed` is the presentation state
/// the smoothing filter advances each `Stage::advance(dt)` — deterministic
/// given the same inputs and dt sequence.
struct BoxesContent {
    std::vector<Box> targets;   ///< Latest detections (model value).
    std::vector<Box> smoothed;  ///< Presentation boxes actually drawn.
    float smoothing = 0;        ///< Time constant in seconds (~63% of the
                                ///< gap closes per constant); 0 disables.
    bool show_label = true;     ///< Draw label + score above each box.
};

/// The content sum type. `std::monostate` = no content (a group).
using Content = std::variant<std::monostate, ImageContent, TextContent, LineContent,
                             PolylineContent, PolygonContent, RectContent, CircleContent,
                             CirclesContent, ArcContent, ArrowContent, CrosshairContent,
                             GridContent, BoxesContent>;

}  // namespace fluent_scene
