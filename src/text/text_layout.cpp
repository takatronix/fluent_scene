#include "text/text_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include <hb.h>

#include "text/font_library.hpp"

namespace fluent_scene {
namespace detail {

namespace {

hb_font_t* hbFont(void* p) { return static_cast<hb_font_t*>(p); }

// ---- UTF-8 -----------------------------------------------------------------

void decodeUtf8(const std::string& s, std::vector<uint32_t>& out) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        uint32_t cp = p[i];
        int extra = 0;
        if (cp < 0x80) {
            extra = 0;
        } else if ((cp & 0xE0) == 0xC0) {
            cp &= 0x1F;
            extra = 1;
        } else if ((cp & 0xF0) == 0xE0) {
            cp &= 0x0F;
            extra = 2;
        } else if ((cp & 0xF8) == 0xF0) {
            cp &= 0x07;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        bool bad = false;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= n || (p[i + k] & 0xC0) != 0x80) {
                bad = true;
                break;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (bad) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += static_cast<size_t>(extra) + 1;
    }
}

// ---- character classes (line breaking) --------------------------------------

bool isSpace(uint32_t c) { return c == ' ' || c == '\t' || c == 0x3000 || c == 0xA0; }

bool isCjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0x20000 && c <= 0x3FFFF) || (c >= 0x3000 && c <= 0x303F);
}

// Joins the previous character's font and unit (marks, selectors, joiners).
bool isJoiner(uint32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0xFE00 && c <= 0xFE0F) || c == 0x200D ||
           c == 0x3099 || c == 0x309A || (c >= 0xE0100 && c <= 0xE01EF) ||
           (c >= 0x1F3FB && c <= 0x1F3FF);
}

// 行頭禁則: may not start a line.
bool noLineStart(uint32_t c) {
    static const char32_t kSet[] =
        U"、。，．・：；？！゛゜ヽヾゝゞ々〻ー‐゠–—―…‥」』）〕］｝〉》】〙〗〟’”｠»"
        U"ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶㇰㇱㇲㇳㇴㇵㇶㇷㇸㇹㇺㇻㇼㇽㇾㇿ"
        U"ｧｨｩｪｫｬｭｮｯｰ･｡｣､!),.:;?]}°%‰′″℃¢！？％";
    for (const char32_t* p = kSet; *p != 0; ++p) {
        if (static_cast<uint32_t>(*p) == c) {
            return true;
        }
    }
    return false;
}

// 行末禁則: may not end a line.
bool noLineEnd(uint32_t c) {
    static const char32_t kSet[] = U"「『（〔［｛〈《【〘〖〝‘“｟«([{｢$¥£€＄￥＃#";
    for (const char32_t* p = kSet; *p != 0; ++p) {
        if (static_cast<uint32_t>(*p) == c) {
            return true;
        }
    }
    return false;
}

bool isWordChar(uint32_t c) { return !isSpace(c) && !isCjk(c) && c != '\n'; }

// Break allowed between chars a and b (a before b)?
bool canBreakBetween(uint32_t a, uint32_t b) {
    if (b == '\n' || a == '\n' || isJoiner(b)) {
        return false;
    }
    if (noLineStart(b) || noLineEnd(a)) {
        return false;
    }
    if (isSpace(a)) {
        return !isSpace(b);
    }
    if (isSpace(b)) {
        return false;  // spaces hang at the end of the line
    }
    if (isCjk(a) || isCjk(b)) {
        return true;
    }
    if (a == '-' && isWordChar(b) && b != '-') {
        return true;
    }
    return false;
}

// ---- shaped units ----------------------------------------------------------

struct Style {
    const FontFace* face = nullptr;  // primary face
    float size = 26;
    int weight = 400;
    bool italic = false;
    uint16_t paint = 0;
};

struct GlyphRec {
    uint32_t glyph;
    uint16_t face;
    float adv;     // em × style size (local units at scale 1)
    float x_off;
    float y_off;   // y up
};

struct Unit {
    uint32_t cp = 0;
    uint16_t style = 0;
    const FontFace* face = nullptr;
    float width = 0;     // advance incl. letter spacing, at scale 1
    uint32_t g0 = 0, g1 = 0;  // glyph range
    bool break_after = false;
    bool newline = false;
    bool space = false;
};

struct Shaped {
    std::vector<Style> styles;
    std::vector<Unit> units;
    std::vector<GlyphRec> glyphs;
    uint32_t missing = 0;
};

const FontFace* faceFor(FontLibrary& lib, const Style& st, uint32_t cp, uint32_t& missing) {
    if (cp < 0x20 || isSpace(cp)) {
        return st.face;
    }
    if (lib.glyphIndex(*st.face, cp) != 0) {
        return st.face;
    }
    if (const FontFace* fb = lib.fallback(st.weight, st.italic)) {
        if (fb != st.face && lib.glyphIndex(*fb, cp) != 0) {
            return fb;
        }
    }
    if (const FontFace* fb = lib.fallback(400, false)) {
        if (fb != st.face && lib.glyphIndex(*fb, cp) != 0) {
            return fb;
        }
    }
    ++missing;
    return st.face;
}

void shapeItem(const std::vector<uint32_t>& cps, size_t begin, size_t end, const FontFace& face,
               const Style& st, bool tabular, Shaped& out) {
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_codepoints(buf, cps.data(), static_cast<int>(cps.size()),
                             static_cast<unsigned>(begin), static_cast<int>(end - begin));
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_language(buf, hb_language_from_string("ja", -1));
    hb_buffer_guess_segment_properties(buf);
    hb_feature_t features[1];
    unsigned nfeat = 0;
    if (tabular) {
        hb_feature_from_string("tnum", -1, &features[nfeat++]);
    }
    hb_shape(hbFont(face.hb_font), buf, features, nfeat);
    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    const float k = st.size / face.upem;
    for (unsigned i = 0; i < n; ++i) {
        const size_t ci = info[i].cluster;  // index into cps (cluster = char index)
        GlyphRec g{info[i].codepoint, face.id, pos[i].x_advance * k, pos[i].x_offset * k,
                   pos[i].y_offset * k};
        if (g.glyph == 0 && g.adv <= 0) {
            g.adv = face.replacement_advance * st.size;
        }
        const uint32_t cp = ci < cps.size() ? cps[ci] : 0;
        if (tabular && cp >= '0' && cp <= '9' && face.max_digit_advance > 0) {
            const float cell = face.max_digit_advance * st.size;
            g.x_off += (cell - g.adv) * 0.5f;
            g.adv = cell;
        }
        Unit& u = out.units[ci];
        if (u.g1 == u.g0) {
            u.g0 = static_cast<uint32_t>(out.glyphs.size());
        }
        out.glyphs.push_back(g);
        u.g1 = static_cast<uint32_t>(out.glyphs.size());
        u.width += g.adv;
    }
    hb_buffer_destroy(buf);
}

Shaped shapeContent(const TextContent& c, FontLibrary& lib, std::vector<LaidPaint>& paints) {
    Shaped sh;
    std::vector<uint32_t> cps;
    auto addStyle = [&](const std::string& font, float size, int weight, bool italic,
                        bool has_color, Color color) -> uint16_t {
        Style st;
        st.face = lib.resolve(font.empty() ? c.font : font, weight, italic);
        st.size = std::max(0.0f, size);
        st.weight = weight;
        st.italic = italic;
        LaidPaint p{has_color, color};
        st.paint = static_cast<uint16_t>(paints.size());
        paints.push_back(p);
        sh.styles.push_back(st);
        return static_cast<uint16_t>(sh.styles.size() - 1);
    };
    std::vector<uint16_t> char_style;
    if (c.spans.empty()) {
        addStyle(c.font, c.size, c.weight, c.italic, false, Color{});
        decodeUtf8(c.utf8, cps);
        char_style.assign(cps.size(), 0);
    } else {
        for (const TextSpan& s : c.spans) {
            const uint16_t si =
                addStyle(s.font.empty() ? c.font : s.font, s.size > 0 ? s.size : c.size,
                         s.weight > 0 ? s.weight : c.weight,
                         s.italic < 0 ? c.italic : s.italic != 0, s.has_color, s.color);
            const size_t before = cps.size();
            decodeUtf8(s.utf8, cps);
            char_style.resize(cps.size(), si);
            (void)before;
        }
        if (sh.styles.empty()) {
            addStyle(c.font, c.size, c.weight, c.italic, false, Color{});
        }
    }
    if (sh.styles.front().face == nullptr) {
        return sh;
    }
    // CR LF / CR → LF.
    {
        std::vector<uint32_t> norm;
        std::vector<uint16_t> norm_style;
        norm.reserve(cps.size());
        for (size_t i = 0; i < cps.size(); ++i) {
            if (cps[i] == '\r') {
                if (i + 1 < cps.size() && cps[i + 1] == '\n') {
                    continue;
                }
                norm.push_back('\n');
            } else {
                norm.push_back(cps[i]);
            }
            norm_style.push_back(char_style[i]);
        }
        cps.swap(norm);
        char_style.swap(norm_style);
    }
    sh.units.resize(cps.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        Unit& u = sh.units[i];
        u.cp = cps[i];
        u.style = char_style[i];
        u.newline = cps[i] == '\n';
        u.space = isSpace(cps[i]);
        const Style& st = sh.styles[u.style];
        if (i > 0 && isJoiner(cps[i]) && sh.units[i - 1].style == u.style) {
            u.face = sh.units[i - 1].face;
        } else {
            u.face = faceFor(lib, st, cps[i], sh.missing);
        }
    }
    // Itemize: runs of the same (style, face), newlines excluded.
    size_t i = 0;
    while (i < cps.size()) {
        if (sh.units[i].newline) {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < cps.size() && !sh.units[j].newline && sh.units[j].style == sh.units[i].style &&
               sh.units[j].face == sh.units[i].face) {
            ++j;
        }
        shapeItem(cps, i, j, *sh.units[i].face, sh.styles[sh.units[i].style], c.tabular, sh);
        i = j;
    }
    // Letter spacing after each visible char; break opportunities.
    for (size_t k = 0; k < cps.size(); ++k) {
        Unit& u = sh.units[k];
        if (!u.newline && !isJoiner(u.cp) && c.letter_spacing != 0) {
            u.width += c.letter_spacing;
        }
        if (k + 1 < cps.size()) {
            u.break_after = canBreakBetween(cps[k], cps[k + 1]);
        }
    }
    return sh;
}

// ---- lines -----------------------------------------------------------------

struct Line {
    size_t begin = 0, end = 0;  // unit range (end exclusive, excl. newline)
    float width = 0;            // excl. trailing spaces, at scale k
    bool ellipsis = false;
    float ascent = 0, descent = 0, height = 0;
};

float unitsWidth(const Shaped& sh, size_t b, size_t e, float k) {
    while (e > b && sh.units[e - 1].space) {
        --e;
    }
    float w = 0;
    for (size_t i = b; i < e; ++i) {
        w += sh.units[i].width;
    }
    return w * k;
}

struct Breaking {
    std::vector<Line> lines;
    bool truncated = false;
};

Breaking breakLines(const Shaped& sh, const TextContent& c, float k) {
    Breaking out;
    const float max_w = c.max_width;
    const bool wrap = c.wrap && max_w > 0;
    size_t start = 0;
    const size_t n = sh.units.size();
    while (start <= n) {
        size_t i = start;
        float w = 0;
        size_t last_break = SIZE_MAX;
        bool hard = false;
        size_t end = n;
        for (; i < n; ++i) {
            const Unit& u = sh.units[i];
            if (u.newline) {
                end = i;
                hard = true;
                break;
            }
            const float uw = u.width * k;
            if (wrap && !u.space && w + uw > max_w + 1e-3f && i > start) {
                if (last_break != SIZE_MAX) {
                    end = last_break + 1;
                } else {
                    end = i;  // emergency: break inside a word
                }
                break;
            }
            w += uw;
            if (u.break_after) {
                last_break = i;
            }
        }
        if (i >= n && !hard) {
            end = n;
        }
        Line line;
        line.begin = start;
        line.end = end;
        line.width = unitsWidth(sh, start, end, k);
        out.lines.push_back(line);
        if (hard) {
            start = end + 1;
            if (start > n) {
                break;
            }
            if (start == n) {  // trailing newline: one empty line
                Line empty;
                empty.begin = empty.end = n;
                out.lines.push_back(empty);
                break;
            }
        } else if (end >= n) {
            break;
        } else {
            start = end;
        }
    }
    return out;
}

void lineMetrics(const Shaped& sh, const TextContent& c, float k, Line& line, uint16_t fallback_style) {
    float asc = 0, desc = 0, natural = 0, max_size = 0;
    bool any = false;
    auto take = [&](const FontFace* f, float size) {
        if (f == nullptr) {
            return;
        }
        asc = std::max(asc, f->ascender * size);
        desc = std::max(desc, f->descender * size);
        natural = std::max(natural, (f->ascender + f->descender + f->line_gap) * size);
        max_size = std::max(max_size, size);
        any = true;
    };
    for (size_t i = line.begin; i < line.end; ++i) {
        const Unit& u = sh.units[i];
        take(u.face, sh.styles[u.style].size * k);
    }
    if (!any) {
        const Style& st = sh.styles[fallback_style];
        take(st.face, st.size * k);
    }
    line.ascent = asc;
    line.descent = desc;
    line.height = c.line_height > 0 ? c.line_height * max_size : natural;
}

struct Fit {
    Breaking br;
    float k = 1;
    float max_line = 0;
    float height = 0;
    bool fits = true;
};

Fit fitAt(const Shaped& sh, const TextContent& c, float k) {
    Fit f;
    f.k = k;
    f.br = breakLines(sh, c, k);
    // max_lines / max_height truncation.
    std::vector<Line>& lines = f.br.lines;
    for (Line& l : lines) {
        const size_t n = sh.units.size();
        const uint16_t fs = l.begin < n ? sh.units[l.begin].style
                                        : (n > 0 ? sh.units[n - 1].style : uint16_t{0});
        lineMetrics(sh, c, k, l, fs);
    }
    size_t keep = lines.size();
    if (c.max_lines > 0 && keep > static_cast<size_t>(c.max_lines)) {
        keep = static_cast<size_t>(c.max_lines);
    }
    if (c.max_height > 0) {
        float h = 0;
        size_t fit_n = 0;
        for (size_t i = 0; i < keep; ++i) {
            if (h + lines[i].height > c.max_height + 1e-3f && i > 0) {
                break;
            }
            h += lines[i].height;
            fit_n = i + 1;
        }
        keep = std::max<size_t>(1, fit_n);
    }
    if (keep < lines.size()) {
        lines.resize(keep);
        f.br.truncated = true;
    }
    for (const Line& l : lines) {
        f.max_line = std::max(f.max_line, l.width);
        f.height += l.height;
    }
    const bool over_w = c.max_width > 0 && f.max_line > c.max_width + 0.5f;
    const bool over_h = c.max_height > 0 && f.height > c.max_height + 0.5f;
    f.fits = !f.br.truncated && !over_w && !over_h;
    if (!c.wrap && c.max_width > 0 && over_w) {
        f.br.truncated = c.ellipsis;  // one-line overflow → ellipsis when asked
    }
    return f;
}

}  // namespace

std::string layoutKey(const TextContent& c) {
    std::string key;
    key.reserve(64 + c.utf8.size());
    auto put = [&](const void* p, size_t n) { key.append(static_cast<const char*>(p), n); };
    auto putf = [&](float v) { put(&v, sizeof v); };
    auto puti = [&](int v) { put(&v, sizeof v); };
    auto puts = [&](const std::string& s) {
        puti(static_cast<int>(s.size()));
        key += s;
    };
    auto putc = [&](const Color& col) {
        putf(col.r);
        putf(col.g);
        putf(col.b);
        putf(col.a);
    };
    puts(c.utf8);
    putf(c.position.x);
    putf(c.position.y);
    putf(c.size);
    puti(static_cast<int>(c.align));
    puti(static_cast<int>(c.valign));
    puts(c.font);
    puti(c.weight);
    puti(c.italic);
    putf(c.letter_spacing);
    putf(c.line_height);
    puti(c.tabular);
    puti(c.hinting);
    putf(c.max_width);
    putf(c.max_height);
    puti(c.wrap);
    puti(c.max_lines);
    puti(c.ellipsis);
    putf(c.min_size);
    puti(static_cast<int>(c.spans.size()));
    for (const TextSpan& s : c.spans) {
        puts(s.utf8);
        puts(s.font);
        putf(s.size);
        puti(s.weight);
        puti(s.italic);
        puti(s.has_color);
        putc(s.color);
    }
    return key;
}

LaidText layoutText(const TextContent& c) {
    LaidText out;
    FontLibrary& lib = FontLibrary::instance();
    lib.ensureDefaults();
    std::lock_guard<std::recursive_mutex> lock(lib.mutex());
    Shaped sh = shapeContent(c, lib, out.paints);
    if (sh.styles.empty() || sh.styles.front().face == nullptr) {
        return out;
    }
    out.ok = true;
    out.missing = sh.missing;

    // Auto-fit: the largest scale in [min_size/size, 1] that fits.
    Fit fit = fitAt(sh, c, 1.0f);
    const bool can_fit = c.min_size > 0 && c.min_size < c.size &&
                         (c.max_width > 0 || c.max_height > 0 || c.max_lines > 0);
    if (!fit.fits && can_fit) {
        float lo = c.min_size / c.size, hi = 1.0f;
        Fit best = fitAt(sh, c, lo);
        if (best.fits) {
            for (int it = 0; it < 12 && hi - lo > 0.002f; ++it) {
                const float mid = 0.5f * (lo + hi);
                Fit f = fitAt(sh, c, mid);
                if (f.fits) {
                    lo = mid;
                    best = std::move(f);
                } else {
                    hi = mid;
                }
            }
        }
        fit = std::move(best);
    }
    const float k = fit.k;
    std::vector<Line>& lines = fit.br.lines;

    // Ellipsis on the last kept line.
    struct Extra {
        GlyphRec g;
        const FontFace* face;
        uint16_t style;
    };
    std::vector<std::vector<Extra>> extras(lines.size());
    if (fit.br.truncated && c.ellipsis && !lines.empty()) {
        Line& last = lines.back();
        uint16_t style = 0;
        const FontFace* face = sh.styles[0].face;
        if (last.end > last.begin) {
            style = sh.units[last.end - 1].style;
            face = sh.units[last.end - 1].face;
        }
        const Style& st = sh.styles[style];
        uint32_t gid = lib.glyphIndex(*face, 0x2026);
        if (gid == 0) {
            if (const FontFace* fb = lib.fallback(st.weight, st.italic)) {
                if (lib.glyphIndex(*fb, 0x2026) != 0) {
                    face = fb;
                    gid = lib.glyphIndex(*fb, 0x2026);
                }
            }
        }
        const float adv =
            gid != 0 ? hb_font_get_glyph_h_advance(hbFont(face->hb_font), gid) / face->upem *
                           st.size
                     : 0.0f;
        const float ew = adv * k;
        if (c.max_width > 0) {
            while (last.end > last.begin &&
                   unitsWidth(sh, last.begin, last.end, k) + ew > c.max_width + 1e-3f) {
                --last.end;
            }
        }
        while (last.end > last.begin && sh.units[last.end - 1].space) {
            --last.end;
        }
        last.width = unitsWidth(sh, last.begin, last.end, k) + ew;
        last.ellipsis = true;
        if (gid != 0) {
            extras.back().push_back({GlyphRec{gid, face->id, adv, 0, 0}, face, style});
        }
    }

    // Block geometry.
    float block_w = 0, block_h = 0;
    for (const Line& l : lines) {
        block_w = std::max(block_w, l.width);
        block_h += l.height;
    }
    if (c.max_width > 0) {
        block_w = c.max_width;
    }
    float left = c.position.x;
    if (c.align == Align::Center) {
        left -= block_w * 0.5f;
    } else if (c.align == Align::Right) {
        left -= block_w;
    }
    const Line& first = lines.front();
    const float first_baseline_off = (first.height - first.ascent - first.descent) * 0.5f +
                                     first.ascent;
    float top = c.position.y;
    switch (c.valign) {
        case VAlign::Top: break;
        case VAlign::Middle: top -= block_h * 0.5f; break;
        case VAlign::Bottom: top -= block_h; break;
        case VAlign::Baseline: top -= first_baseline_off; break;
    }

    // Glyph origins + ink.
    TextMetrics& m = out.metrics;
    float ink_x0 = 1e30f, ink_y0 = 1e30f, ink_x1 = -1e30f, ink_y1 = -1e30f;
    float y = top;
    for (size_t li = 0; li < lines.size(); ++li) {
        const Line& l = lines[li];
        const float baseline = y + (l.height - l.ascent - l.descent) * 0.5f + l.ascent;
        float x = left;
        if (c.align == Align::Center) {
            x += (block_w - l.width) * 0.5f;
        } else if (c.align == Align::Right) {
            x += block_w - l.width;
        }
        auto emit = [&](const GlyphRec& g, const FontFace* face, uint16_t style) {
            const Style& st = sh.styles[style];
            LaidGlyph lg;
            lg.glyph = g.glyph;
            lg.face = g.face;
            lg.paint = st.paint;
            lg.size = st.size * k;
            lg.x = x + g.x_off * k;
            lg.y = baseline - g.y_off * k;
            const FontFace* gf = lib.face(g.face);
            if (gf == nullptr) {
                gf = face;
            }
            lg.oblique = st.italic && !gf->italic;
            if (st.weight - gf->weight >= 150) {
                lg.bold = 0.02f * std::min(1.0f, (st.weight - gf->weight) / 300.0f);
            }
            float box[4];
            if (g.glyph != 0 && lib.glyphExtents(*gf, g.glyph, box)) {
                const float slant = lg.oblique ? 0.2126f * box[3] * lg.size : 0.0f;
                const float grow = lg.bold * lg.size;
                ink_x0 = std::min(ink_x0, lg.x + box[0] * lg.size - grow);
                ink_x1 = std::max(ink_x1, lg.x + box[2] * lg.size + grow + slant);
                ink_y0 = std::min(ink_y0, lg.y - box[3] * lg.size - grow);
                ink_y1 = std::max(ink_y1, lg.y - box[1] * lg.size + grow);
            } else if (g.glyph == 0) {
                ink_x0 = std::min(ink_x0, lg.x);
                ink_x1 = std::max(ink_x1, lg.x + 0.75f * lg.size);
                ink_y0 = std::min(ink_y0, lg.y - 0.72f * lg.size);
                ink_y1 = std::max(ink_y1, lg.y);
            }
            out.max_size = std::max(out.max_size, lg.size);
            out.glyphs.push_back(lg);
            x += g.adv * k;
        };
        for (size_t ui = l.begin; ui < l.end; ++ui) {
            const Unit& u = sh.units[ui];
            for (uint32_t gi = u.g0; gi < u.g1; ++gi) {
                const GlyphRec& g = sh.glyphs[gi];
                if (u.space && g.glyph != 0) {
                    x += g.adv * k;  // spaces advance but draw nothing
                    continue;
                }
                emit(g, u.face, u.style);
            }
            if (!u.newline && !isJoiner(u.cp)) {
                x += c.letter_spacing * k;
            }
        }
        for (const Extra& e : extras[li]) {
            emit(e.g, e.face, e.style);
        }
        m.line_widths.push_back(l.width);
        y += l.height;
    }

    m.box = {left, top, block_w, block_h};
    if (ink_x1 >= ink_x0) {
        m.ink = {ink_x0, ink_y0, ink_x1 - ink_x0, ink_y1 - ink_y0};
    } else {
        m.ink = {left, top, 0, 0};
    }
    m.baseline = top + first_baseline_off;
    m.ascent = first.ascent;
    m.descent = first.descent;
    m.line_height = first.height;
    m.size = c.size * k;
    m.lines = static_cast<int>(lines.size());
    m.truncated = fit.br.truncated;
    return out;
}

}  // namespace detail
}  // namespace fluent_scene
