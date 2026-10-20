#pragma once

// Internal: the text front-end every renderer owns (docs/design/text.md §6).
// It turns a TextContent + the layer's local→target transform into glyph
// quads on atlas pages, grouped by paint; the backend only draws them:
//
//   for each group: open an RG mask → one quad per glyph (glyph_mask) →
//                   composite with the group's paint (text_composite).
//
// Choice per glyph: an exact-size bitmap when the transform is an upright
// uniform scale, the glyph is at most kBitmapMaxPx on screen and no outline
// is asked for; otherwise the SDF bucket for its on-screen size. Bitmap
// glyphs snap to the pixel grid vertically and to quarter pixels
// horizontally, so small text is as sharp as the font allows.
//
// Layouts are cached per content (exact key) and swept when unused, so a
// number that changes every frame re-shapes a few characters and hits the
// atlas — no rasterization after the first sighting of each digit.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "fluent_scene/content.hpp"
#include "fluent_scene/geometry.hpp"
#include "text/glyph_atlas.hpp"
#include "text/text_layout.hpp"

namespace fluent_scene {
namespace detail {

class TextEngine {
public:
    static constexpr float kBitmapMaxPx = 32.0f;

    struct Quad {
        uint16_t page = 0;
        Rect local{};          // glyph tile in layer-local units
        float uv[4] = {0, 0, 0, 0};
        float mode = 0;        // 0 bitmap coverage, 1 SDF
        float px_range = 0;    // SDF: output px spanned by 0..1
        float dilate_fill = 0;   // output px
        float dilate_outer = 0;  // output px
    };
    struct Group {
        Color fill{1, 1, 1, 1};  // straight; the gradient's `from` when on
        bool gradient = false;   // fill runs fill → Draw::gradient_to
        std::vector<Quad> quads;
    };
    struct Draw {
        std::vector<Group> groups;
        Color outline{0, 0, 0, 0};  // a = 0: no outline
        bool gradient = false;
        Color gradient_to{1, 1, 1, 1};
        Vec2 gradient_origin{};   // local units
        Vec2 gradient_axis{};     // direction / length²
        Rect bounds{};            // local bbox of every quad
    };

    explicit TextEngine(GlyphAtlas::Options options = {}) : atlas_(options) {}

    // Font defaults (+ the renderer's Options::font_file as "sans").
    void init(const std::string& font_file);
    bool ready();

    // Call once per rendered frame (atlas LRU tick, layout cache sweep).
    void beginFrame();

    const LaidText& layout(const TextContent& content);
    TextMetrics metrics(const TextContent& content);
    // Everything the content may paint (box ∪ ink, outline-padded), for
    // offscreen extents and Stage::measureText.
    Rect extent(const TextContent& content);

    // The draw for `content` under `m` (local → target px) with the layer
    // color `color` (fill unless a span overrides it). Cached with the
    // layout: an unchanged text under an unchanged transform reuses last
    // frame's quads. Valid until the next call into the engine.
    const Draw& build(const TextContent& content, const Mat23& m, Color color);

    GlyphAtlas& atlas() { return atlas_; }
    uint64_t layoutsBuilt() const { return layouts_built_; }

private:
    struct CacheEntry {
        LaidText laid;
        uint64_t frame = 0;
        bool has_draw = false;
        Mat23 draw_m{};
        Color draw_color{};
        uint64_t draw_epoch = 0;
        std::vector<uint16_t> draw_pages;
        Draw draw;
    };
    CacheEntry& entry(const TextContent& content);
    void buildDraw(const TextContent& content, const LaidText& laid, const Mat23& m, Color color,
                   Draw& out);
    GlyphAtlas atlas_;
    std::unordered_map<std::string, CacheEntry> cache_;
    uint64_t frame_ = 1;
    uint64_t layouts_built_ = 0;
    std::string font_file_;
    bool inited_ = false;
};

}  // namespace detail
}  // namespace fluent_scene
