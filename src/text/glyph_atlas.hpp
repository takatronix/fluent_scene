#pragma once

// Internal: the dynamic glyph atlas every renderer owns (docs/design/text.md §4).
//
// - No fixed glyph set: glyphs rasterize on first use (CJK needs thousands).
// - Pages of R8 texels (default 1024², ≤ 16 pages = 16 MB), shelf-packed.
// - Glyph kinds share the pages:
//     Bitmap — coverage at the exact pixel size (quarter-pixel size steps),
//              unhinted (BitmapHinted: FreeType light hinting, opt-in via
//              TextContent::hinting), one of 4 horizontal subpixel phases;
//              for small upright text;
//     Sdf    — signed distance field at a size bucket (24…256 px in √2
//              steps) for large, rotated, scaled, outlined or bold-synthetic
//              text; sampled with screen-space antialiasing.
// - Bounded memory: when every page is full, the least-recently-used page
//   that no draw of the current frame touched is wiped and reused. If all
//   pages are in use this frame the request fails (the glyph is skipped and
//   counted) instead of growing past the cap.
// - Each page tracks the row band changed since the backend last uploaded,
//   so a new glyph costs a strip upload, not a page.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fluent_scene {
namespace detail {

struct FontFace;

// Bitmap: unhinted coverage at the exact size; BitmapHinted: the same with
// FreeType light (vertical) hinting; Sdf: a distance field bucket.
enum class GlyphKind : uint8_t { Bitmap = 0, Sdf = 1, BitmapHinted = 2 };

struct GlyphKey {
    uint32_t glyph = 0;
    uint16_t face = 0;
    uint16_t size = 0;    // Bitmap: px × 4; Sdf: bucket px
    uint8_t kind = 0;     // GlyphKind
    uint8_t phase = 0;    // Bitmap: x subpixel phase 0..3
    uint8_t oblique = 0;  // synthetic slant
    uint8_t bold = 0;     // Bitmap synthetic emboldening, 1/64 px per side
    uint64_t packed() const {
        return (static_cast<uint64_t>(glyph) << 32) | (static_cast<uint64_t>(face) << 16) | size;
    }
    uint32_t packed2() const {
        return static_cast<uint32_t>(kind) | (static_cast<uint32_t>(phase) << 8) |
               (static_cast<uint32_t>(oblique) << 16) | (static_cast<uint32_t>(bold) << 24);
    }
    bool operator==(const GlyphKey& o) const {
        return packed() == o.packed() && packed2() == o.packed2();
    }
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const {
        return std::hash<uint64_t>()(k.packed() * 0x9E3779B97F4A7C15ull ^ k.packed2());
    }
};

struct GlyphSlot {
    uint16_t page = 0;
    uint16_t x = 0, y = 0, w = 0, h = 0;  // tile in page texels
    float left = 0;    // tile left edge − glyph origin x, raster px
    float top = 0;     // tile top edge − baseline, raster px, y up
    float raster_px = 0;  // pixel size the tile was rasterized at
    float spread = 0;     // Sdf: distance range (texels) of 0..255
    bool empty = true;    // nothing to draw (space)
};

class GlyphAtlas {
public:
    struct Options {
        uint32_t page_size = 1024;
        uint32_t max_pages = 16;
    };
    struct Page {
        std::vector<uint8_t> px;
        uint32_t size = 0;
        uint64_t revision = 0;      // bumps on every change
        int dirty_y0 = 0, dirty_y1 = 0;  // rows changed since clearDirty
        uint64_t last_used = 0;     // frame stamp of the last lookup
        struct Shelf {
            int y = 0, h = 0, x = 0;
        };
        std::vector<Shelf> shelves;
        int shelf_bottom = 0;
        std::vector<GlyphKey> keys;  // glyphs living here (for eviction)
    };
    struct Stats {
        uint64_t rasterized = 0;
        uint64_t evicted_pages = 0;
        uint64_t failed = 0;
        double raster_ms = 0;
    };

    GlyphAtlas() = default;
    explicit GlyphAtlas(Options options) : options_(options) {}

    // One frame = one LRU tick; pages used since the last beginFrame are
    // never evicted.
    void beginFrame() { ++frame_; }

    // The slot for `key`, rasterizing on a miss; nullptr when the glyph
    // cannot be placed (atlas exhausted this frame).
    const GlyphSlot* get(const FontFace& face, const GlyphKey& key);

    // Marks a page as used this frame (cached draws skip get()).
    void touchPage(size_t i) { pages_[i].last_used = frame_; }
    // Bumps whenever any slot may have moved (a page was wiped).
    uint64_t epoch() const { return stats_.evicted_pages; }

    size_t pageCount() const { return pages_.size(); }
    const Page& page(size_t i) const { return pages_[i]; }
    void clearDirty(size_t i) {
        pages_[i].dirty_y0 = 0;
        pages_[i].dirty_y1 = 0;
    }
    const Stats& stats() const { return stats_; }
    size_t glyphCount() const { return slots_.size(); }
    uint32_t pageSize() const { return options_.page_size; }

    // SDF bucket for a glyph drawn at `px` screen pixels.
    static uint16_t sdfBucket(float px);
    // Distance range (texels) for a bucket: ~0.19 em, so outlines up to
    // ~0.18 em and synthetic bold stay inside the field.
    static int sdfSpread(uint16_t bucket);

private:
    bool rasterize(const FontFace& face, const GlyphKey& key, GlyphSlot& slot,
                   std::vector<uint8_t>& tile);
    bool place(int w, int h, GlyphSlot& slot);
    bool placeIn(size_t page_index, int w, int h, GlyphSlot& slot);
    void wipe(size_t page_index);

    Options options_;
    std::vector<Page> pages_;
    std::unordered_map<GlyphKey, GlyphSlot, GlyphKeyHash> slots_;
    uint64_t frame_ = 1;
    Stats stats_;
};

}  // namespace detail
}  // namespace fluent_scene
