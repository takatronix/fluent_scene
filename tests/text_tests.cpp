// text_tests — the text engine's contracts (docs/design/text.md):
//   - glyph atlas: on-demand CJK, bounded pages, page-LRU eviction, no
//     eviction of pages the current frame uses;
//   - SDF accuracy: coverage reconstructed from the field vs FreeType's own
//     exact raster of the same outline (PSNR floor);
//   - layout: kinsoku, wrapping, ellipsis, auto-fit, tabular digits,
//     vertical alignment, newlines, spans, missing glyphs;
//   - public API: Stage::textMetrics before any renderer, Layer setters.
// Prints the atlas build time for 500 new CJK glyphs (the perf number the
// design doc quotes).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <fluent_scene/fluent_scene.hpp>

#include "text/font_library.hpp"
#include "text/glyph_atlas.hpp"
#include "text/text_layout.hpp"

using namespace fluent_scene;
using namespace fluent_scene::detail;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

bool near(float a, float b, float eps = 1e-2f) { return std::fabs(a - b) < eps; }

std::string utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

// The first `n` CJK ideographs the default face covers, from U+4E00.
std::vector<uint32_t> kanjiGlyphs(const FontFace& face, size_t n) {
    std::vector<uint32_t> out;
    for (uint32_t cp = 0x4E00; cp < 0x9FFF && out.size() < n; ++cp) {
        const uint32_t g = FontLibrary::instance().glyphIndex(face, cp);
        if (g != 0) {
            out.push_back(g);
        }
    }
    return out;
}

void testAtlasOnDemandAndPerf(const FontFace& face) {
    GlyphAtlas atlas;  // defaults: 1024² pages, 16 max
    const std::vector<uint32_t> glyphs = kanjiGlyphs(face, 500);
    CHECK(glyphs.size() == 500);
    atlas.beginFrame();
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t g : glyphs) {
        GlyphKey k;
        k.glyph = g;
        k.face = face.id;
        k.kind = static_cast<uint8_t>(GlyphKind::Sdf);
        k.size = GlyphAtlas::sdfBucket(40);
        CHECK(atlas.get(face, k) != nullptr);
    }
    const double sdf_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(atlas.stats().rasterized == 500);
    // Second request: cache hits, no rasterization.
    for (uint32_t g : glyphs) {
        GlyphKey k;
        k.glyph = g;
        k.face = face.id;
        k.kind = static_cast<uint8_t>(GlyphKind::Sdf);
        k.size = GlyphAtlas::sdfBucket(40);
        atlas.get(face, k);
    }
    CHECK(atlas.stats().rasterized == 500);
    // Bitmap path at 16 px.
    const auto t1 = std::chrono::steady_clock::now();
    for (uint32_t g : glyphs) {
        GlyphKey k;
        k.glyph = g;
        k.face = face.id;
        k.kind = static_cast<uint8_t>(GlyphKind::Bitmap);
        k.size = 16 * 4;
        CHECK(atlas.get(face, k) != nullptr);
    }
    const double bmp_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("perf: 500 new CJK glyphs — SDF (48 px bucket) %.1f ms, bitmap 16 px %.1f ms; "
                "%zu page(s)\n",
                sdf_ms, bmp_ms, atlas.pageCount());
}

void testAtlasEviction(const FontFace& face) {
    GlyphAtlas::Options o;
    o.page_size = 256;
    o.max_pages = 2;
    GlyphAtlas atlas(o);
    const std::vector<uint32_t> glyphs = kanjiGlyphs(face, 400);
    auto key = [&](uint32_t g) {
        GlyphKey k;
        k.glyph = g;
        k.face = face.id;
        k.kind = static_cast<uint8_t>(GlyphKind::Sdf);
        k.size = 48;  // ~68 px tiles: ~9 per 256² page
        return k;
    };
    // One glyph per frame: the atlas must keep working by evicting.
    for (size_t i = 0; i < 120; ++i) {
        atlas.beginFrame();
        CHECK(atlas.get(face, key(glyphs[i])) != nullptr);
    }
    CHECK(atlas.pageCount() == 2);  // never past the cap
    CHECK(atlas.stats().evicted_pages > 0);
    CHECK(atlas.stats().failed == 0);
    // A glyph evicted long ago re-rasterizes transparently.
    atlas.beginFrame();
    const uint64_t before = atlas.stats().rasterized;
    CHECK(atlas.get(face, key(glyphs[0])) != nullptr);
    CHECK(atlas.stats().rasterized == before + 1);
    // Within ONE frame, pages in use are never evicted: asking for more
    // glyphs than two pages hold fails some requests instead.
    atlas.beginFrame();
    size_t ok = 0;
    std::vector<const GlyphSlot*> slots;
    for (size_t i = 200; i < 260; ++i) {
        const GlyphSlot* s = atlas.get(face, key(glyphs[i]));
        if (s != nullptr) {
            ++ok;
            slots.push_back(s);
        }
    }
    CHECK(ok > 0 && ok < 60);
    CHECK(atlas.stats().failed > 0);
    CHECK(atlas.pageCount() == 2);
    // Everything handed out this frame is still where it was put.
    for (const GlyphSlot* s : slots) {
        CHECK(!s->empty && s->page < 2);
    }
}

// Coverage rebuilt from the SDF tile vs FreeType's exact raster at the
// bucket size: the field must reproduce the glyph, not a blurred guess.
void testSdfAccuracy(const FontFace& face) {
    GlyphAtlas atlas;
    atlas.beginFrame();
    FT_Library lib = static_cast<FT_Library>(FontLibrary::instance().ftLibrary());
    (void)lib;
    FT_Face ft = static_cast<FT_Face>(face.ft_face);
    double se = 0;
    size_t n = 0;
    const char32_t kText[] = U"漢字表示速度鬱薔薇あいうえおカタカナAg8@";
    for (const char32_t* p = kText; *p != 0; ++p) {
        const uint32_t gid = FontLibrary::instance().glyphIndex(face, *p);
        GlyphKey k;
        k.glyph = gid;
        k.face = face.id;
        k.kind = static_cast<uint8_t>(GlyphKind::Sdf);
        k.size = 64;
        const GlyphSlot* s = atlas.get(face, k);
        CHECK(s != nullptr && !s->empty);
        if (s == nullptr) {
            continue;
        }
        const auto& page = atlas.page(s->page);
        FT_Set_Char_Size(ft, 0, 64 * 64, 72, 72);
        FT_Load_Glyph(ft, gid, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_RENDER);
        const FT_Bitmap& bm = ft->glyph->bitmap;
        const int bl = ft->glyph->bitmap_left, bt = ft->glyph->bitmap_top;
        for (int y = 0; y < s->h; ++y) {
            for (int x = 0; x < s->w; ++x) {
                const float v = page.px[(s->y + y) * page.size + s->x + x] / 255.0f;
                const float d = (0.5f - v) * 2.0f * s->spread;
                const float cov = std::fmin(1.0f, std::fmax(0.0f, 0.5f - d));
                const int rx = static_cast<int>(s->left) + x - bl;
                const int ry = bt - (static_cast<int>(s->top) - y);
                float ref = 0;
                if (rx >= 0 && ry >= 0 && rx < static_cast<int>(bm.width) &&
                    ry < static_cast<int>(bm.rows)) {
                    ref = bm.buffer[ry * bm.pitch + rx] / 255.0f;
                }
                se += (cov - ref) * (cov - ref);
                ++n;
            }
        }
    }
    const double psnr = 10 * std::log10(1.0 / std::fmax(se / n, 1e-12));
    std::printf("sdf accuracy: coverage from field vs exact raster, PSNR %.1f dB\n", psnr);
    CHECK(psnr > 30.0);
}

void testLayout() {
    Stage stage(800, 600);  // no renderer: Stage lays out itself
    TextContent c;
    c.size = 20;

    // Kinsoku: no line starts with 。 or 」, none ends with 「.
    c.utf8 = "あいうえおかきくけ。こさしすせそたちつ「てとなにぬねの」はひふへほ";
    for (float w = 60; w <= 200; w += 10) {
        c.max_width = w;
        const LaidText laid = layoutText(c);
        CHECK(laid.metrics.lines >= 2);
        for (float lw : laid.metrics.line_widths) {
            CHECK(lw <= w + 20.01f);  // at most one pushed-in char over
        }
    }
    {
        // Explicit check at a width where 。 would land first on line 2.
        TextContent k;
        k.size = 20;
        k.utf8 = "あいう。えお";
        k.max_width = 60.5f;  // 3 chars fit
        const TextMetrics m = stage.textMetrics(k);
        CHECK(m.lines == 3);  // あい / う。え / お
        CHECK(m.line_widths[0] < 50);  // う moved down with 。
    }

    // Latin wraps at spaces, trailing spaces don't count.
    c = TextContent{};
    c.size = 20;
    c.utf8 = "hello world wrapping";
    c.max_width = 90;
    TextMetrics m = stage.textMetrics(c);
    CHECK(m.lines == 3);  // hello / world / wrapping
    c.max_width = 60;     // "wrapping" alone is wider: broken inside
    m = stage.textMetrics(c);
    CHECK(m.lines == 4);

    // Newlines.
    c = TextContent{};
    c.utf8 = "a\nb\n\nc";
    m = stage.textMetrics(c);
    CHECK(m.lines == 4);

    // max_lines + ellipsis.
    c = TextContent{};
    c.size = 20;
    c.utf8 = "あいうえおかきくけこさしすせそたちつてと";
    c.max_width = 100;
    c.max_lines = 2;
    c.ellipsis = true;
    m = stage.textMetrics(c);
    CHECK(m.lines == 2 && m.truncated);
    CHECK(m.line_widths[1] <= 100.01f);

    // Auto-fit: shrinks until one line fits.
    c = TextContent{};
    c.size = 40;
    c.utf8 = "Auto-fit text that is long";
    c.max_width = 200;
    c.wrap = false;
    c.min_size = 6;
    m = stage.textMetrics(c);
    CHECK(m.size < 40 && m.size >= 6);
    CHECK(m.line_widths[0] <= 200.5f && m.line_widths[0] > 180);

    // Tabular: same width for any digits.
    c = TextContent{};
    c.size = 50;
    c.tabular = true;
    c.utf8 = "1111.1";
    const float w1 = stage.textMetrics(c).box.w;
    c.utf8 = "8888.8";
    CHECK(near(w1, stage.textMetrics(c).box.w));

    // Vertical alignment.
    c = TextContent{};
    c.size = 30;
    c.utf8 = "Ag";
    c.position = {0, 100};
    c.valign = VAlign::Baseline;
    CHECK(near(stage.textMetrics(c).baseline, 100));
    c.valign = VAlign::Middle;
    m = stage.textMetrics(c);
    CHECK(near(m.box.y + m.box.h * 0.5f, 100));
    c.valign = VAlign::Bottom;
    m = stage.textMetrics(c);
    CHECK(near(m.box.y + m.box.h, 100));
    c.valign = VAlign::Top;
    m = stage.textMetrics(c);
    CHECK(near(m.box.y, 100));
    CHECK(m.ink.y > m.box.y && m.ink.y + m.ink.h <= m.box.y + m.box.h + 0.5f);

    // Align right: the box ends at the position.
    c.align = Align::Right;
    c.position = {300, 0};
    m = stage.textMetrics(c);
    CHECK(near(m.box.x + m.box.w, 300));

    // Spans: a bigger span raises the line's ascent.
    c = TextContent{};
    c.size = 20;
    TextSpan a;
    a.utf8 = "速度 ";
    TextSpan b;
    b.utf8 = "88";
    b.size = 60;
    c.spans = {a, b};
    m = stage.textMetrics(c);
    CHECK(m.ascent > 60);
    CHECK(m.lines == 1);

    // Old single-line rule: top-left position, box height = line height.
    c = TextContent{};
    c.utf8 = "走行中";
    c.size = 26;
    c.position = {10, 20};
    m = stage.textMetrics(c);
    CHECK(near(m.box.x, 10) && near(m.box.y, 20));
    CHECK(m.box.h > 26 && m.box.h < 26 * 1.6f);
    CHECK(m.baseline > 20 + 26 * 0.8f && m.baseline < 20 + 26 * 1.3f);
}

void testLayerApi() {
    Stage stage(400, 200);
    Layer& t = stage.text("88.5", {10, 10})
                   .size(48)
                   .bold()
                   .italic()
                   .tabularNumbers()
                   .textOutline(3, Color::Black)
                   .textGradient(Color::White, Color::Teal)
                   .maxWidth(300)
                   .verticalAlign(VAlign::Middle)
                   .letterSpacing(1);
    const auto& c = std::get<TextContent>(t.content());
    CHECK(c.weight == 700 && c.italic && c.tabular && c.outline.width == 3 &&
          c.gradient.enabled && c.max_width == 300 && c.valign == VAlign::Middle);
    t.sizePt(12);
    CHECK(near(std::get<TextContent>(t.content()).size, 16));
    CHECK(stage.textMetrics(t).lines == 1);
    // Glow is the shadow with no offset.
    t.glow(10, Color::Magenta);
    CHECK(t.shadowValue() && t.shadowValue()->offset.x == 0 && t.shadowValue()->radius == 10);
    // Setters on non-text layers warn, not crash.
    Layer& r = stage.rect({0, 0, 10, 10});
    r.bold();
    CHECK(!stage.drainDiagnostics().empty());

    // Rendering: outline + gradient + spans draw something on the CPU.
    CpuRenderer cpu;
    const Surface& s = cpu.render(stage, 0);
    size_t lit = 0;
    for (uint32_t y = 0; y < s.height; ++y) {
        for (uint32_t x = 0; x < s.width; ++x) {
            lit += s.row(y)[x * 4 + 3] > 0;
        }
    }
    CHECK(lit > 500);
}

void testMissingGlyph() {
    TextContent c;
    c.utf8 = "\xF0\x9F\x80\x84";  // U+1F004 mahjong tile: not in Noto CJK
    c.size = 30;
    const LaidText laid = layoutText(c);
    CHECK(laid.missing == 1);
    CHECK(laid.glyphs.size() == 1 && laid.glyphs[0].glyph == 0);
    CHECK(laid.metrics.ink.w > 0);  // the replacement box has ink
}

}  // namespace

int main() {
    FontLibrary::instance().ensureDefaults();
    const FontFace* face = FontLibrary::instance().resolve("sans", 400, false);
    if (face == nullptr || FontLibrary::instance().glyphIndex(*face, 0x6F22) == 0) {
        std::fprintf(stderr, "skip: no CJK font\n");
        return 77;
    }
    testAtlasOnDemandAndPerf(*face);
    testAtlasEviction(*face);
    testSdfAccuracy(*face);
    testLayout();
    testLayerApi();
    testMissingGlyph();
    std::printf(g_failures == 0 ? "PASS\n" : "FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
