#pragma once

// Internal: text layout (docs/design/text.md §5) — a pure function of a
// TextContent and the registered fonts, shared by every renderer and by
// Stage::textMetrics. No pixels here: the output is positioned glyphs in
// the layer's local units plus the metrics the public API returns.
//
//   spans → per-char font choice (fallback to "sans" for missing glyphs)
//         → HarfBuzz shaping per (span, face) item, in font units, once
//         → units (one per char) with break opportunities (spaces, CJK
//           boundaries; kinsoku: no line start on 。、」ー small kana …,
//           no line end on 「（ …)
//         → greedy lines at max_width, max_lines / max_height, ellipsis
//         → auto-fit: the largest scale ≥ min_size/size that fits
//         → alignment (Align × VAlign) and glyph origins.

#include <cstdint>
#include <string>
#include <vector>

#include "fluent_scene/content.hpp"

namespace fluent_scene {
namespace detail {

struct LaidGlyph {
    uint32_t glyph = 0;
    uint16_t face = 0;
    uint16_t paint = 0;   // index into LaidText::paints
    float x = 0, y = 0;   // origin on the baseline, local units
    float size = 0;       // font size, local units
    float bold = 0;       // synthetic emboldening, em per side (0 = none)
    bool oblique = false; // synthetic slant
};

struct LaidPaint {
    bool has_color = false;  // false = the layer color
    Color color{1, 1, 1, 1};
};

struct LaidText {
    std::vector<LaidGlyph> glyphs;
    std::vector<LaidPaint> paints;
    TextMetrics metrics;
    float max_size = 0;     // largest glyph size (local units)
    uint32_t missing = 0;   // codepoints no font covers
    bool ok = false;        // false when no font is available
};

LaidText layoutText(const TextContent& content);

// A compact, exact cache key for a content (everything layout reads).
std::string layoutKey(const TextContent& content);

}  // namespace detail
}  // namespace fluent_scene
