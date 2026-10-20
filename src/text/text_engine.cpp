#include "text/text_engine.hpp"

#include <algorithm>
#include <cmath>

#include "text/font_library.hpp"

namespace fluent_scene {
namespace detail {

namespace {

constexpr uint64_t kCacheKeepFrames = 120;  // layouts unused this long go
constexpr size_t kCacheMax = 4096;

Rect unite(Rect a, Rect b) {
    if (a.w <= 0 && a.h <= 0) {
        return b;
    }
    if (b.w <= 0 && b.h <= 0) {
        return a;
    }
    const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const float x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

// Outline width actually drawable for a glyph of `size`: the SDF spread is
// 3/16 em, minus room for one antialiasing pixel.
float clampOutline(float width, float size) { return std::min(width, size * 0.17f); }

}  // namespace

void TextEngine::init(const std::string& font_file) {
    if (inited_ && font_file == font_file_) {
        return;
    }
    inited_ = true;
    font_file_ = font_file;
    FontLibrary::instance().ensureDefaults(font_file);
    cache_.clear();
}

bool TextEngine::ready() {
    if (!inited_) {
        init(font_file_);
    }
    return FontLibrary::instance().resolve("sans", 400, false) != nullptr;
}

void TextEngine::beginFrame() {
    ++frame_;
    atlas_.beginFrame();
    if (cache_.size() > 64 && (frame_ % 30) == 0) {
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (frame_ - it->second.frame > kCacheKeepFrames) {
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

const LaidText& TextEngine::layout(const TextContent& content) {
    if (!inited_) {
        init(font_file_);
    }
    std::string key = layoutKey(content);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        if (cache_.size() >= kCacheMax) {
            cache_.clear();
        }
        CacheEntry e;
        e.laid = layoutText(content);
        ++layouts_built_;
        it = cache_.emplace(std::move(key), std::move(e)).first;
    }
    it->second.frame = frame_;
    return it->second.laid;
}

TextMetrics TextEngine::metrics(const TextContent& content) { return layout(content).metrics; }

Rect TextEngine::extent(const TextContent& content) {
    const LaidText& laid = layout(content);
    if (!laid.ok) {
        return {};
    }
    Rect r = unite(laid.metrics.box, laid.metrics.ink);
    const float pad = clampOutline(content.outline.width, laid.max_size) + 1.0f;
    return {r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad};
}

void TextEngine::build(const TextContent& content, const Mat23& m, Color color, Draw& out) {
    out.groups.clear();
    out.bounds = {};
    const LaidText& laid = layout(content);
    if (!laid.ok || laid.glyphs.empty()) {
        return;
    }
    FontLibrary& lib = FontLibrary::instance();
    const float det = std::fabs(m.a * m.d - m.b * m.c);
    const float s = det > 0 ? std::sqrt(det) : 0.0f;  // output px per local unit
    if (s <= 0) {
        return;
    }
    const bool upright = std::fabs(m.b) < 1e-6f && std::fabs(m.c) < 1e-6f && m.a > 0 &&
                         m.d > 0 && std::fabs(m.a - m.d) <= 1e-4f * m.a;
    const bool outlined = content.outline.width > 0 && content.outline.color.a > 0;

    out.outline = outlined ? content.outline.color : Color{0, 0, 0, 0};
    out.gradient = content.gradient.enabled;
    if (out.gradient) {
        // Gradient across the layout box along angle_deg (clockwise).
        const Rect b = laid.metrics.box.w > 0 || laid.metrics.box.h > 0 ? laid.metrics.box
                                                                         : laid.metrics.ink;
        const float rad = content.gradient.angle_deg * 0.01745329252f;
        const Vec2 dir{std::cos(rad), std::sin(rad)};
        const Vec2 c{b.x + b.w * 0.5f, b.y + b.h * 0.5f};
        const float half = 0.5f * (std::fabs(dir.x) * b.w + std::fabs(dir.y) * b.h);
        const float len = std::max(1e-3f, 2 * half);
        out.gradient_origin = {c.x - dir.x * half, c.y - dir.y * half};
        out.gradient_axis = {dir.x / len, dir.y / len};
        out.gradient_to = content.gradient.to;
    }

    // One group per paint actually used, in first-use order.
    std::vector<int> group_of(laid.paints.size(), -1);
    const Mat23 inv = m.inverse();
    for (const LaidGlyph& g : laid.glyphs) {
        const FontFace* face = lib.face(g.face);
        if (face == nullptr) {
            continue;
        }
        const float px = g.size * s;
        if (px < 0.5f) {
            continue;
        }
        Quad q;
        const GlyphSlot* slot = nullptr;
        GlyphKey key;
        key.glyph = g.glyph;
        key.face = g.face;
        key.oblique = g.oblique ? 1 : 0;
        if (upright && !outlined && px <= kBitmapMaxPx) {
            // Exact-size bitmap, snapped: integer baseline, quarter-px x.
            const Vec2 o = m.apply({g.x, g.y});
            // Ties sit a hair off .5 so float noise between backends (CPU
            // vs offscreen GPU transforms) never flips a baseline by 1 px.
            const float oy = std::floor(o.y + 0.5f + 1.0f / 64.0f);
            const float xq = std::floor(o.x * 4.0f + 0.5f + 1.0f / 64.0f) / 4.0f;
            const float xi = std::floor(xq);
            key.kind = static_cast<uint8_t>(GlyphKind::Bitmap);
            key.size = static_cast<uint16_t>(std::lround(px * 4.0f));
            key.phase = static_cast<uint8_t>(std::lround((xq - xi) * 4.0f) & 3);
            key.bold = static_cast<uint8_t>(
                std::min(255L, std::lround(g.bold * px * 64.0f)));
            slot = atlas_.get(*face, key);
            if (slot == nullptr || slot->empty) {
                continue;
            }
            const float x0 = xi + slot->left, y0 = oy - slot->top;
            const Vec2 p0 = inv.apply({x0, y0});
            const Vec2 p1 = inv.apply({x0 + slot->w, y0 + slot->h});
            q.local = {p0.x, p0.y, p1.x - p0.x, p1.y - p0.y};
            q.mode = 0;
        } else {
            key.kind = static_cast<uint8_t>(GlyphKind::Sdf);
            key.size = GlyphAtlas::sdfBucket(px);
            slot = atlas_.get(*face, key);
            if (slot == nullptr || slot->empty) {
                continue;
            }
            const float sc = g.size / slot->raster_px;  // local units per texel
            q.local = {g.x + slot->left * sc, g.y - slot->top * sc, slot->w * sc,
                       slot->h * sc};
            const float px_per_texel = sc * s;
            q.mode = 1;
            q.px_range = 2.0f * slot->spread * px_per_texel;
            const float bold_px = g.bold * px;
            const float w_px = outlined ? clampOutline(content.outline.width, g.size) * s : 0.0f;
            switch (content.outline.position) {
                case OutlinePosition::Outside:
                    q.dilate_fill = bold_px;
                    q.dilate_outer = bold_px + w_px;
                    break;
                case OutlinePosition::Inside:
                    q.dilate_fill = bold_px - w_px;
                    q.dilate_outer = bold_px;
                    break;
                case OutlinePosition::Center:
                    q.dilate_fill = bold_px - w_px * 0.5f;
                    q.dilate_outer = bold_px + w_px * 0.5f;
                    break;
            }
            // Keep the grown edge inside the field (spread − 1 texel).
            const float max_dilate = (slot->spread - 1.0f) * px_per_texel;
            q.dilate_outer = std::min(q.dilate_outer, max_dilate);
            q.dilate_fill = std::min(q.dilate_fill, max_dilate);
        }
        const float ps = static_cast<float>(atlas_.page(slot->page).size);
        q.page = slot->page;
        q.uv[0] = slot->x / ps;
        q.uv[1] = slot->y / ps;
        q.uv[2] = (slot->x + slot->w) / ps;
        q.uv[3] = (slot->y + slot->h) / ps;
        int& gi = group_of[g.paint];
        if (gi < 0) {
            gi = static_cast<int>(out.groups.size());
            Group grp;
            const LaidPaint& p = laid.paints[g.paint];
            grp.gradient = out.gradient && !p.has_color;
            grp.fill = grp.gradient ? content.gradient.from : (p.has_color ? p.color : color);
            out.groups.push_back(std::move(grp));
        }
        out.groups[static_cast<size_t>(gi)].quads.push_back(q);
        out.bounds = unite(out.bounds, q.local);
    }
}

}  // namespace detail
}  // namespace fluent_scene
