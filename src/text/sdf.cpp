#include "text/sdf.hpp"

#include <algorithm>
#include <cmath>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

namespace fluent_scene {
namespace detail {

namespace {

constexpr float kInf = 1e20f;
constexpr float kBand = 2.5f;  // texels of exact distance around the edge

struct Seg {
    float ax, ay, bx, by;
};

struct Flattener {
    std::vector<Seg> segs;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    bool open = false;

    void line(float x, float y) {
        if (x != cx || y != cy) {
            segs.push_back({cx, cy, x, y});
        }
        cx = x;
        cy = y;
    }
    void close() {
        if (open) {
            line(sx, sy);
        }
    }
};

// Subdivision count from the control polygon length: chords stay within
// ~0.02 texel of the curve for any glyph-sized radius.
int pieces(float control_length) {
    return std::clamp(static_cast<int>(std::ceil(control_length * 0.5f)), 2, 64);
}

int moveTo(const FT_Vector* to, void* user) {
    auto& f = *static_cast<Flattener*>(user);
    f.close();
    f.cx = f.sx = to->x / 64.0f;
    f.cy = f.sy = to->y / 64.0f;
    f.open = true;
    return 0;
}

int lineTo(const FT_Vector* to, void* user) {
    static_cast<Flattener*>(user)->line(to->x / 64.0f, to->y / 64.0f);
    return 0;
}

int conicTo(const FT_Vector* c, const FT_Vector* to, void* user) {
    auto& f = *static_cast<Flattener*>(user);
    const float x0 = f.cx, y0 = f.cy, x1 = c->x / 64.0f, y1 = c->y / 64.0f;
    const float x2 = to->x / 64.0f, y2 = to->y / 64.0f;
    const int n = pieces(std::hypot(x1 - x0, y1 - y0) + std::hypot(x2 - x1, y2 - y1));
    for (int i = 1; i <= n; ++i) {
        const float t = static_cast<float>(i) / n, mt = 1 - t;
        f.line(mt * mt * x0 + 2 * mt * t * x1 + t * t * x2,
               mt * mt * y0 + 2 * mt * t * y1 + t * t * y2);
    }
    return 0;
}

int cubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
    auto& f = *static_cast<Flattener*>(user);
    const float x0 = f.cx, y0 = f.cy, x1 = c1->x / 64.0f, y1 = c1->y / 64.0f;
    const float x2 = c2->x / 64.0f, y2 = c2->y / 64.0f, x3 = to->x / 64.0f, y3 = to->y / 64.0f;
    const int n = pieces(std::hypot(x1 - x0, y1 - y0) + std::hypot(x2 - x1, y2 - y1) +
                         std::hypot(x3 - x2, y3 - y2));
    for (int i = 1; i <= n; ++i) {
        const float t = static_cast<float>(i) / n, mt = 1 - t;
        const float a = mt * mt * mt, b = 3 * mt * mt * t, cc = 3 * mt * t * t, d = t * t * t;
        f.line(a * x0 + b * x1 + cc * x2 + d * x3, a * y0 + b * y1 + cc * y2 + d * y3);
    }
    return 0;
}

// 1D squared distance transform (Felzenszwalb & Huttenlocher).
void edt1d(float* grid, int offset, int stride, int length, float* f, int* v, float* z) {
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    f[0] = grid[offset];
    for (int q = 1, k = 0; q < length; ++q) {
        f[q] = grid[offset + q * stride];
        const float q2 = static_cast<float>(q) * q;
        float s = 0;
        do {
            const int r = v[k];
            s = (f[q] - f[r] + q2 - static_cast<float>(r) * r) / static_cast<float>(q - r) / 2;
        } while (s <= z[k] && --k > -1);
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    for (int q = 0, k = 0; q < length; ++q) {
        while (z[k + 1] < q) {
            ++k;
        }
        const int r = v[k];
        const float qr = static_cast<float>(q - r);
        grid[offset + q * stride] = f[r] + qr * qr;
    }
}

void edt2d(float* data, int w, int h, std::vector<float>& f, std::vector<int>& v,
           std::vector<float>& z) {
    for (int x = 0; x < w; ++x) {
        edt1d(data, x, w, h, f.data(), v.data(), z.data());
    }
    for (int y = 0; y < h; ++y) {
        edt1d(data, y * w, 1, w, f.data(), v.data(), z.data());
    }
}

}  // namespace

bool makeSdf(void* ft_library, void* ft_outline, int spread, SdfTile& out) {
    auto* outline = static_cast<FT_Outline*>(ft_outline);
    if (outline->n_points == 0) {
        return false;
    }
    FT_BBox cbox;
    FT_Outline_Get_CBox(outline, &cbox);
    const int x0 = static_cast<int>(std::floor(cbox.xMin / 64.0)) - spread;
    const int y0 = static_cast<int>(std::floor(cbox.yMin / 64.0)) - spread;
    const int x1 = static_cast<int>(std::ceil(cbox.xMax / 64.0)) + spread;
    const int y1 = static_cast<int>(std::ceil(cbox.yMax / 64.0)) + spread;
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        return false;
    }
    const size_t len = static_cast<size_t>(w) * h;

    // Coverage (bottom-up rows flipped to top-down by a negative pitch).
    std::vector<uint8_t> cov(len, 0);
    FT_Bitmap bm{};
    bm.rows = static_cast<unsigned>(h);
    bm.width = static_cast<unsigned>(w);
    bm.pitch = w;
    bm.buffer = cov.data();
    bm.num_grays = 256;
    bm.pixel_mode = FT_PIXEL_MODE_GRAY;
    FT_Outline_Translate(outline, -x0 * 64, -y0 * 64);
    FT_Outline_Get_Bitmap(static_cast<FT_Library>(ft_library), outline, &bm);
    Flattener fl;
    FT_Outline_Funcs funcs{moveTo, lineTo, conicTo, cubicTo, 0, 0};
    FT_Outline_Decompose(outline, &funcs, &fl);
    fl.close();
    FT_Outline_Translate(outline, x0 * 64, y0 * 64);
    // `cov` rows run top-down (FreeType's positive pitch); segment
    // coordinates are relative to (x0, y0), y up: texel row r has its
    // center at y = h - r - 0.5.

    // Far field: TinySDF-style seeds from coverage, squared EDT.
    std::vector<float> outer(len, kInf), inner(len, 0);
    for (size_t i = 0; i < len; ++i) {
        const float a = cov[i] / 255.0f;
        if (a <= 0) {
            continue;
        }
        if (a >= 1) {
            outer[i] = 0;
            inner[i] = kInf;
        } else {
            const float d = 0.5f - a;
            outer[i] = d > 0 ? d * d : 0;
            inner[i] = d < 0 ? d * d : 0;
        }
    }
    const int m = std::max(w, h);
    std::vector<float> f(static_cast<size_t>(m)), z(static_cast<size_t>(m) + 1);
    std::vector<int> v(static_cast<size_t>(m));
    edt2d(outer.data(), w, h, f, v, z);
    edt2d(inner.data(), w, h, f, v, z);

    // Near band: exact distance to the flattened outline.
    std::vector<float> band(len, kInf);
    for (const Seg& s : fl.segs) {
        const int bx0 = std::max(0, static_cast<int>(std::floor(std::min(s.ax, s.bx) - kBand)));
        const int bx1 = std::min(w - 1, static_cast<int>(std::ceil(std::max(s.ax, s.bx) + kBand)));
        const int by0 = std::max(0, static_cast<int>(std::floor(h - std::max(s.ay, s.by) - kBand)));
        const int by1 = std::min(h - 1, static_cast<int>(std::ceil(h - std::min(s.ay, s.by) + kBand)));
        const float dx = s.bx - s.ax, dy = s.by - s.ay;
        const float ll = dx * dx + dy * dy;
        const float inv_ll = ll > 0 ? 1.0f / ll : 0.0f;
        for (int r = by0; r <= by1; ++r) {
            const float py = static_cast<float>(h - r) - 0.5f;
            float* row = band.data() + static_cast<size_t>(r) * w;
            for (int c = bx0; c <= bx1; ++c) {
                const float px = static_cast<float>(c) + 0.5f;
                const float t =
                    std::clamp(((px - s.ax) * dx + (py - s.ay) * dy) * inv_ll, 0.0f, 1.0f);
                const float ex = s.ax + t * dx - px, ey = s.ay + t * dy - py;
                const float d2 = ex * ex + ey * ey;
                if (d2 < row[c]) {
                    row[c] = d2;
                }
            }
        }
    }

    out.w = w;
    out.h = h;
    out.left = x0;
    out.top = y1;
    out.px.resize(len);
    const float scale = 255.0f / (2.0f * static_cast<float>(spread));
    for (size_t i = 0; i < len; ++i) {
        float d = std::sqrt(outer[i]) - std::sqrt(inner[i]);  // + outside
        if (band[i] < kBand * kBand) {
            const float e = std::sqrt(band[i]);
            d = cov[i] >= 128 ? -e : e;
        }
        const float val = 127.5f - d * scale;
        out.px[i] = static_cast<uint8_t>(std::clamp(val + 0.5f, 0.0f, 255.0f));
    }
    return true;
}

}  // namespace detail
}  // namespace fluent_scene
