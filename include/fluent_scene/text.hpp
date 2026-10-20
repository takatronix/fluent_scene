#pragma once

/// \file text.hpp
/// \brief Text vocabulary — fonts, styling, layout options and metrics
///        (docs/design/text.md, docs/api/text.ja.md).
///
/// Text is one content type (TextContent, content.hpp); everything here is
/// a field of it or a value it returns. Sizes and distances are logical
/// units like every other public API; `pt()` converts typographic points
/// (1 pt = 4/3 logical units, the CSS px/pt ratio).
///
/// ```cpp
/// stage.text("速度 88.5", {40, 40}).size(64).bold().tabularNumbers()
///      .textOutline(3, Color::Black);
/// TextMetrics m = stage.textMetrics(layer);   // box, baseline, lines, ...
/// ```

#include <cstdint>
#include <string>
#include <vector>

#include "fluent_scene/geometry.hpp"

namespace fluent_scene {

/// Points → logical units (1 pt = 4/3 units, as CSS px:pt).
constexpr float pt(float points) { return points * (4.0f / 3.0f); }

/// Common OpenType weights (any 1–1000 value works).
namespace FontWeight {
constexpr int Thin = 100;
constexpr int Light = 300;
constexpr int Regular = 400;
constexpr int Medium = 500;
constexpr int Bold = 700;
constexpr int Black = 900;
}  // namespace FontWeight

/// Which edge of the text block `TextContent::position` refers to
/// vertically (Align is the horizontal counterpart).
enum class VAlign {
    Top,       ///< Top of the first line box (default; the historic rule).
    Middle,    ///< Vertical center of the whole block.
    Baseline,  ///< The first line's baseline.
    Bottom,    ///< Bottom of the last line box.
};

/// Where an outline sits relative to the glyph edge.
enum class OutlinePosition {
    Outside,  ///< Grows outward; the fill keeps its shape (default).
    Inside,   ///< Eats into the fill; the silhouette keeps its size.
    Center,   ///< Straddles the edge.
};

/// A stroke around the glyphs (width 0 = none).
struct TextOutline {
    float width = 0;                          ///< Logical units.
    Color color{0, 0, 0, 1};
    OutlinePosition position = OutlinePosition::Outside;
};

/// A linear two-stop fill gradient across the text block (replaces the
/// layer color for the fill; outline keeps its own color). `angle_deg` 90
/// runs top → bottom, 0 left → right (clockwise on screen, like arcs).
struct TextGradient {
    bool enabled = false;
    Color from{1, 1, 1, 1};
    Color to{1, 1, 1, 1};
    float angle_deg = 90;
};

/// Per-span overrides inside a rich text. Zero / empty fields inherit the
/// content's own value.
struct TextSpan {
    std::string utf8;          ///< The span's text.
    std::string font;          ///< Font family; empty = inherit.
    float size = 0;            ///< Logical units; 0 = inherit.
    int weight = 0;            ///< 1–1000; 0 = inherit.
    int italic = -1;           ///< -1 inherit, 0 upright, 1 italic.
    bool has_color = false;    ///< When true, `color` overrides the fill.
    Color color{1, 1, 1, 1};
};

/// Measured layout of one text content, in the layer's local units.
struct TextMetrics {
    Rect box{};          ///< Layout box: line boxes stacked, aligned.
    Rect ink{};          ///< Tight bounds of the glyph outlines drawn.
    float baseline = 0;  ///< y of the first line's baseline.
    float ascent = 0;    ///< First line: baseline − top of its glyph box.
    float descent = 0;   ///< First line: bottom − baseline (positive).
    float line_height = 0;  ///< Distance between consecutive baselines.
    float size = 0;      ///< Font size actually used (after auto-fit).
    int lines = 0;       ///< Line count after wrapping / max_lines.
    bool truncated = false;  ///< Text was cut (max_lines / max_height / ellipsis).
    std::vector<float> line_widths;  ///< Advance width of each line.
};

/// Registers a font file (TTF / OTF / TTC) under a family name, for a weight
/// and slant. The first face of a .ttc whose family contains "JP" is used
/// (Noto CJK collections). Text asks for a family + weight + italic; the
/// closest registered face is used, and missing glyphs fall back to the
/// default family. Returns false (and logs once) when the file cannot be
/// read. Process-wide; safe to call before or after renderers exist.
bool registerFont(const std::string& family, const std::string& file, int weight = 400,
                  bool italic = false);

/// Same, from bytes already in memory (the wasm build fetches fonts this
/// way). The bytes are copied.
bool registerFontData(const std::string& family, const uint8_t* data, size_t size,
                      int weight = 400, bool italic = false);

/// The families currently known (default ones included: "sans", and
/// "serif" / "mono" when the system has them).
std::vector<std::string> fontFamilies();

}  // namespace fluent_scene
