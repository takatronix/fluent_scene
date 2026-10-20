#include "text/glyph_atlas.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "text/font_library.hpp"
#include "text/sdf.hpp"

namespace fluent_scene {
namespace detail {

namespace {

constexpr uint16_t kBuckets[] = {24, 32, 48, 64, 96, 128, 192, 256};

// The replacement glyph for codepoints no font covers: a hollow box, the
// same in every build (the wasm subsets' .notdef has no outline).
void boxOutline(FT_Library lib, float em, FT_Outline& o) {
    FT_Outline_New(lib, 8, 2, &o);
    const float x0 = 0.08f * em, x1 = 0.67f * em, y0 = 0.0f, y1 = 0.72f * em;
    const float t = std::max(1.0f, 0.07f * em);
    const float pts[8][2] = {{x0, y0},     {x0, y1},     {x1, y1},     {x1, y0},
                             {x0 + t, y0 + t}, {x1 - t, y0 + t}, {x1 - t, y1 - t}, {x0 + t, y1 - t}};
    for (int i = 0; i < 8; ++i) {
        o.points[i].x = static_cast<FT_Pos>(std::lround(pts[i][0] * 64));
        o.points[i].y = static_cast<FT_Pos>(std::lround(pts[i][1] * 64));
        o.tags[i] = FT_CURVE_TAG_ON;
    }
    o.contours[0] = 3;
    o.contours[1] = 7;
    o.flags = FT_OUTLINE_NONE;
}

// Rasterizes `o` (26.6, y up) into a tight coverage tile.
bool coverageTile(FT_Library lib, FT_Outline& o, GlyphSlot& slot, std::vector<uint8_t>& tile) {
    if (o.n_points == 0) {
        return false;
    }
    FT_BBox cbox;
    FT_Outline_Get_CBox(&o, &cbox);
    const int x0 = static_cast<int>(std::floor(cbox.xMin / 64.0));
    const int y0 = static_cast<int>(std::floor(cbox.yMin / 64.0));
    const int x1 = static_cast<int>(std::ceil(cbox.xMax / 64.0));
    const int y1 = static_cast<int>(std::ceil(cbox.yMax / 64.0));
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        return false;
    }
    tile.assign(static_cast<size_t>(w) * h, 0);
    FT_Bitmap bm{};
    bm.rows = static_cast<unsigned>(h);
    bm.width = static_cast<unsigned>(w);
    bm.pitch = w;
    bm.buffer = tile.data();
    bm.num_grays = 256;
    bm.pixel_mode = FT_PIXEL_MODE_GRAY;
    FT_Outline_Translate(&o, -x0 * 64, -y0 * 64);
    FT_Outline_Get_Bitmap(lib, &o, &bm);
    FT_Outline_Translate(&o, x0 * 64, y0 * 64);
    slot.w = static_cast<uint16_t>(w);
    slot.h = static_cast<uint16_t>(h);
    slot.left = static_cast<float>(x0);
    slot.top = static_cast<float>(y1);
    return true;
}

}  // namespace

uint16_t GlyphAtlas::sdfBucket(float px) {
    for (uint16_t b : kBuckets) {
        if (b >= px * 0.85f) {
            return b;
        }
    }
    return kBuckets[sizeof kBuckets / sizeof kBuckets[0] - 1];
}

int GlyphAtlas::sdfSpread(uint16_t bucket) {
    return std::max(4, static_cast<int>(std::lround(bucket * 3.0 / 16.0)));
}

bool GlyphAtlas::rasterize(const FontFace& face, const GlyphKey& key, GlyphSlot& slot,
                           std::vector<uint8_t>& tile) {
    auto& lib = FontLibrary::instance();
    std::lock_guard<std::recursive_mutex> lock(lib.mutex());
    FT_Library ftlib = static_cast<FT_Library>(lib.ftLibrary());
    FT_Face ft = static_cast<FT_Face>(face.ft_face);
    const bool sdf = key.kind == static_cast<uint8_t>(GlyphKind::Sdf);
    const float px = sdf ? static_cast<float>(key.size) : key.size / 4.0f;
    slot.raster_px = px;
    if (FT_Set_Char_Size(ft, 0, static_cast<FT_F26Dot6>(std::lround(px * 64)), 72, 72) != 0) {
        return false;
    }
    FT_Outline box{};
    FT_Outline* o = nullptr;
    bool own_box = false;
    if (key.glyph != 0) {
        const FT_Int32 flags =
            FT_LOAD_NO_BITMAP |
            (key.kind == static_cast<uint8_t>(GlyphKind::BitmapHinted) ? FT_LOAD_TARGET_LIGHT
                                                                       : FT_LOAD_NO_HINTING);
        if (FT_Load_Glyph(ft, key.glyph, flags) != 0 ||
            ft->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
            return false;
        }
        o = &ft->glyph->outline;
    } else {
        boxOutline(ftlib, px, box);
        o = &box;
        own_box = true;
    }
    if (key.oblique != 0) {
        // 12° slant about the baseline, like FT_GlyphSlot_Oblique.
        FT_Matrix shear{0x10000, 0x0366A, 0, 0x10000};
        FT_Outline_Transform(o, &shear);
    }
    bool ok = false;
    if (sdf) {
        SdfTile t;
        const int spread = sdfSpread(key.size);
        ok = makeSdf(ftlib, o, spread, t);
        if (ok) {
            tile = std::move(t.px);
            slot.w = static_cast<uint16_t>(t.w);
            slot.h = static_cast<uint16_t>(t.h);
            slot.left = static_cast<float>(t.left);
            slot.top = static_cast<float>(t.top);
            slot.spread = static_cast<float>(spread);
        }
    } else {
        if (key.bold != 0) {
            FT_Outline_EmboldenXY(o, key.bold * 2, key.bold * 2);  // symmetric
        }
        if (key.phase != 0) {
            FT_Outline_Translate(o, key.phase * 16, 0);
        }
        ok = coverageTile(ftlib, *o, slot, tile);
    }
    if (own_box) {
        FT_Outline_Done(ftlib, &box);
    }
    return ok;
}

bool GlyphAtlas::placeIn(size_t page_index, int w, int h, GlyphSlot& slot) {
    Page& p = pages_[page_index];
    const int size = static_cast<int>(p.size);
    const int aw = w + 1, ah = h + 1;  // 1-texel gap right/bottom
    if (aw > size || ah > size) {
        return false;
    }
    Page::Shelf* best = nullptr;
    for (Page::Shelf& s : p.shelves) {
        if (s.h >= ah && s.h <= ah + ah / 3 + 4 && s.x + aw <= size &&
            (best == nullptr || s.h < best->h)) {
            best = &s;
        }
    }
    if (best == nullptr) {
        const int sh = (ah + 3) & ~3;
        if (p.shelf_bottom + sh > size) {
            return false;
        }
        p.shelves.push_back({p.shelf_bottom, sh, 0});
        p.shelf_bottom += sh;
        best = &p.shelves.back();
    }
    slot.page = static_cast<uint16_t>(page_index);
    slot.x = static_cast<uint16_t>(best->x);
    slot.y = static_cast<uint16_t>(best->y);
    best->x += aw;
    return true;
}

void GlyphAtlas::wipe(size_t page_index) {
    Page& p = pages_[page_index];
    for (const GlyphKey& k : p.keys) {
        slots_.erase(k);
    }
    p.keys.clear();
    p.shelves.clear();
    p.shelf_bottom = 0;
    std::fill(p.px.begin(), p.px.end(), 0);
    p.dirty_y0 = 0;
    p.dirty_y1 = static_cast<int>(p.size);
    ++p.revision;
    ++stats_.evicted_pages;
}

bool GlyphAtlas::place(int w, int h, GlyphSlot& slot) {
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (placeIn(i, w, h, slot)) {
            return true;
        }
    }
    if (pages_.size() < std::max<uint32_t>(1, options_.max_pages)) {
        Page p;
        p.size = std::max<uint32_t>(256, options_.page_size);
        p.px.assign(static_cast<size_t>(p.size) * p.size, 0);
        p.revision = 1;
        p.dirty_y0 = 0;
        p.dirty_y1 = static_cast<int>(p.size);
        pages_.push_back(std::move(p));
        return placeIn(pages_.size() - 1, w, h, slot);
    }
    // Evict the least recently used page no draw of this frame touched.
    size_t victim = pages_.size();
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].last_used < frame_ &&
            (victim == pages_.size() || pages_[i].last_used < pages_[victim].last_used)) {
            victim = i;
        }
    }
    if (victim == pages_.size()) {
        return false;
    }
    wipe(victim);
    return placeIn(victim, w, h, slot);
}

const GlyphSlot* GlyphAtlas::get(const FontFace& face, const GlyphKey& key) {
    auto it = slots_.find(key);
    if (it != slots_.end()) {
        if (!it->second.empty) {
            pages_[it->second.page].last_used = frame_;
        }
        return &it->second;
    }
    const auto t0 = std::chrono::steady_clock::now();
    GlyphSlot slot;
    std::vector<uint8_t> tile;
    const bool drawn = rasterize(face, key, slot, tile);
    ++stats_.rasterized;
    if (!drawn) {
        slot.empty = true;
        stats_.raster_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        return &slots_.emplace(key, slot).first->second;
    }
    if (!place(slot.w, slot.h, slot)) {
        ++stats_.failed;
        return nullptr;
    }
    slot.empty = false;
    Page& p = pages_[slot.page];
    for (int r = 0; r < slot.h; ++r) {
        std::memcpy(p.px.data() + static_cast<size_t>(slot.y + r) * p.size + slot.x,
                    tile.data() + static_cast<size_t>(r) * slot.w, slot.w);
    }
    if (p.dirty_y1 <= p.dirty_y0) {
        p.dirty_y0 = slot.y;
        p.dirty_y1 = slot.y + slot.h;
    } else {
        p.dirty_y0 = std::min(p.dirty_y0, static_cast<int>(slot.y));
        p.dirty_y1 = std::max(p.dirty_y1, static_cast<int>(slot.y + slot.h));
    }
    ++p.revision;
    p.last_used = frame_;
    p.keys.push_back(key);
    stats_.raster_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return &slots_.emplace(key, slot).first->second;
}

}  // namespace detail
}  // namespace fluent_scene
