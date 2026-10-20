#pragma once

// Internal: signed distance fields for glyph outlines (docs/design/text.md §3).
//
// Hybrid generator, ~100x faster than FreeType's own "sdf" renderer at the
// same accuracy where it matters:
//   - near the edge (|d| < 2.5 texels — everything antialiasing, thin
//     outlines and synthetic bold read) the distance is EXACT: the outline
//     is flattened to line segments and each segment writes the distance to
//     the texels around it;
//   - farther out (thick outlines, glow-free padding) a Felzenszwalb
//     squared-EDT seeded from the coverage bitmap fills in (the TinySDF
//     approximation, accurate to a fraction of a texel there);
//   - the sign comes from the coverage the smooth rasterizer produces for
//     the same outline, so overlapping contours union correctly.
//
// Encoding: one byte per texel, 255 * clamp(0.5 - d / (2 * spread)), with
// d in texels, positive outside. 127.5 is the edge; ±spread spans 0..255.

#include <cstdint>
#include <vector>

namespace fluent_scene {
namespace detail {

struct SdfTile {
    int w = 0, h = 0;
    int left = 0;  // texel x of the tile's left edge relative to the origin
    int top = 0;   // texel y of the tile's top edge, y up from the baseline
    std::vector<uint8_t> px;
};

// `ft_library`: FT_Library; `ft_outline`: FT_Outline* in 26.6 texel units
// (y up). The outline is translated in place and restored. Returns false
// for an empty outline (spaces).
bool makeSdf(void* ft_library, void* ft_outline, int spread, SdfTile& out);

}  // namespace detail
}  // namespace fluent_scene
