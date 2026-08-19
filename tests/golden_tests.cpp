// golden_tests — image-level contract tests (§11, §13-6).
//
// Each scene renders deterministically (injected dt only) and compares
// against a stored golden image in tests/golden/. The comparison allows a
// small per-channel tolerance so recompiles and minor FP differences don't
// flake, while real regressions (a shifted shape, a broken filter) fail.
//
//   ./golden_tests                     compare CPU output to the goldens
//   ./golden_tests --update            regenerate goldens (CPU reference)
//   ./golden_tests --renderer=vulkan   compare the GPU backend to the SAME
//                                      goldens (looser tolerance: fp16
//                                      targets and GPU bilinear round
//                                      differently) — the §11 cross-backend
//                                      guarantee. Exits 77 (skip) when no
//                                      Vulkan device exists.
//
// On failure the rendered image is written next to the golden as
// <name>.actual.ppm for visual diffing. The text scene depends on the
// system CJK font (goldens are generated on the robot's own image); when no
// font is available that scene is skipped with a notice.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <memory>

#include <fluent_scene/fluent_scene.hpp>
#ifdef FS_HAVE_VULKAN
#include <fluent_scene/vulkan_renderer.hpp>
#endif

using namespace fluent_scene;

namespace {

int g_failures = 0;
bool g_update = false;
std::string g_dir;  // tests/golden, from GOLDEN_DIR
int g_max_diff_limit = 16;
double g_over_ratio_limit = 0.002;
std::string g_actual_suffix = ".actual.ppm";

// ---- ppm io ----------------------------------------------------------------

bool writePpm(const std::string& path, const Surface& s) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", s.width, s.height);
    for (uint32_t y = 0; y < s.height; ++y) {
        const uint8_t* row = s.row(y);
        for (uint32_t x = 0; x < s.width; ++x) {
            // Composite over mid-gray so alpha regressions are visible too.
            const uint8_t* p = &row[x * 4];
            const float a = p[3] / 255.0f;
            const uint8_t rgb[3] = {static_cast<uint8_t>(p[0] * a + 64 * (1 - a)),
                                    static_cast<uint8_t>(p[1] * a + 64 * (1 - a)),
                                    static_cast<uint8_t>(p[2] * a + 64 * (1 - a))};
            std::fwrite(rgb, 1, 3, f);
        }
    }
    std::fclose(f);
    return true;
}

bool readPpm(const std::string& path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char magic[3] = {};
    unsigned int pw = 0, ph = 0, maxval = 0;
    if (std::fscanf(f, "%2s %u %u %u", magic, &pw, &ph, &maxval) != 4 ||
        std::strcmp(magic, "P6") != 0 || maxval != 255) {
        std::fclose(f);
        return false;
    }
    std::fgetc(f);  // single whitespace after header
    rgb.resize(static_cast<size_t>(pw) * ph * 3);
    const bool ok = std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
    std::fclose(f);
    w = pw;
    h = ph;
    return ok;
}

// ---- comparison ------------------------------------------------------------

void checkScene(const std::string& name, const Surface& s) {
    const std::string golden_path = g_dir + "/" + name + ".ppm";
    if (g_update) {
        if (writePpm(golden_path, s)) {
            std::printf("updated %s\n", golden_path.c_str());
        } else {
            std::fprintf(stderr, "FAIL cannot write %s\n", golden_path.c_str());
            ++g_failures;
        }
        return;
    }
    uint32_t gw = 0, gh = 0;
    std::vector<uint8_t> golden;
    if (!readPpm(golden_path, gw, gh, golden)) {
        std::fprintf(stderr, "FAIL %s: missing golden (run --update once)\n", name.c_str());
        ++g_failures;
        return;
    }
    if (gw != s.width || gh != s.height) {
        std::fprintf(stderr, "FAIL %s: size %ux%u != golden %ux%u\n", name.c_str(), s.width,
                     s.height, gw, gh);
        ++g_failures;
        return;
    }
    // Rebuild the composited rgb the same way writePpm does.
    size_t over_threshold = 0;
    int max_diff = 0;
    for (uint32_t y = 0; y < s.height; ++y) {
        const uint8_t* row = s.row(y);
        for (uint32_t x = 0; x < s.width; ++x) {
            const uint8_t* p = &row[x * 4];
            const float a = p[3] / 255.0f;
            const uint8_t rgb[3] = {static_cast<uint8_t>(p[0] * a + 64 * (1 - a)),
                                    static_cast<uint8_t>(p[1] * a + 64 * (1 - a)),
                                    static_cast<uint8_t>(p[2] * a + 64 * (1 - a))};
            const uint8_t* g = &golden[(static_cast<size_t>(y) * s.width + x) * 3];
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>(rgb[c]) - g[c]);
                max_diff = std::max(max_diff, d);
                if (d > 2) {
                    ++over_threshold;
                }
            }
        }
    }
    const size_t total = static_cast<size_t>(s.width) * s.height * 3;
    const double over_ratio = static_cast<double>(over_threshold) / total;
    if (max_diff > g_max_diff_limit || over_ratio > g_over_ratio_limit) {
        std::fprintf(stderr, "FAIL %s: max_diff=%d over_ratio=%.4f%%\n", name.c_str(), max_diff,
                     over_ratio * 100.0);
        writePpm(g_dir + "/" + name + g_actual_suffix, s);
        ++g_failures;
    } else {
        std::printf("ok %s (max_diff=%d)\n", name.c_str(), max_diff);
    }
}

// ---- shared fixtures -------------------------------------------------------

std::vector<uint8_t> makeTestImage(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px(w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &px[(y * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 255 / w);
            p[1] = static_cast<uint8_t>(y * 255 / h);
            p[2] = static_cast<uint8_t>(((x / 16) + (y / 16)) % 2 ? 200 : 60);
            p[3] = 255;
        }
    }
    return px;
}

// ---- scenes ----------------------------------------------------------------

void sceneShapes(Renderer& r) {
    Stage stage(480, 300);
    stage.grid(60).color({1, 1, 1, 0.12f});
    stage.line({20, 30}, {200, 60}).thickness(5).color(Color::Teal);
    stage.line({20, 60}, {200, 90}).thickness(5).dash(14).cap(Cap::Butt).color(Color::Orange);
    stage.polyline({{20, 130}, {80, 100}, {140, 140}, {200, 110}}).thickness(6);
    stage.polygon({{40, 200}, {110, 170}, {180, 210}, {120, 260}}).color(Color::Blue.faded(0.8f));
    stage.polygon({{40, 200}, {110, 170}, {180, 210}, {120, 260}})
        .thickness(2)
        .color(Color::White);
    stage.rect({230, 30, 100, 60}).cornerRadius(14).color(Color::Magenta.faded(0.9f));
    stage.rect({230, 110, 100, 60}).cornerRadius(14).thickness(4).color(Color::Yellow);
    stage.circle({280, 240}, 34).color(Color::Green);
    stage.circle({280, 240}, 34).thickness(3).color(Color::Black.faded(0.6f));
    stage.circles({{370, 40}, {395, 60}, {420, 45}, {440, 70}}, 7).color(Color::Red);
    stage.arc({400, 140}, 40, -60, 200).thickness(9).color(Color::Teal);
    stage.arrow({360, 220}, {450, 260}).thickness(5).color(Color::Orange);
    stage.crosshair({420, 200}, 22).thickness(2).color(Color::White);
    checkScene("shapes", r.render(stage, 0.0f));
}

void sceneAttributes(Renderer& r) {
    Stage stage(480, 300);
    stage.rect({0, 0, 480, 300}).color({0.13f, 0.15f, 0.18f, 1});

    auto& panel = stage.group("panel");
    panel.bounds({0, 0, 180, 100}).position(24 + 90, 24 + 50);
    panel.background({0, 0, 0, 0.5f});
    panel.cornerRadius(16);
    panel.border(2, Color::Teal);
    panel.shadow(0, 6, 12);
    panel.masksToBounds(true);
    panel.circle({0, 50}, 40).color(Color::Magenta);  // clipped left half

    // Group opacity: overlapping children must fade as one.
    auto& g = stage.group("fade").opacity(0.5f);
    g.rect({240, 30, 90, 60}).color(Color::White);
    g.rect({280, 60, 90, 60}).color(Color::White);

    // Blend modes over a gradient bar.
    stage.rect({240, 160, 220, 30}).color({0.3f, 0.5f, 0.9f, 1});
    stage.rect({250, 150, 40, 50}).blend(Blend::Add).color({0.5f, 0.2f, 0.1f, 1});
    stage.rect({310, 150, 40, 50}).blend(Blend::Multiply).color({0.9f, 0.8f, 0.3f, 1});
    stage.rect({370, 150, 40, 50}).blend(Blend::Screen).color({0.2f, 0.6f, 0.4f, 1});

    // Border + background + shadow on a leaf with content.
    stage.rect({60, 200, 120, 70})
        .cornerRadius(10)
        .color({1, 1, 1, 0.25f})
        .shadow(4, 4, 8);
    checkScene("attributes", r.render(stage, 0.0f));
}

void sceneTransforms(Renderer& r) {
    Stage stage(480, 300);
    stage.grid(50).color({1, 1, 1, 0.1f});
    stage.rect({0, 0, 120, 60}).position(110, 90).rotation(25).thickness(3).color(Color::Teal);
    stage.rect({0, 0, 120, 60}).position(110, 90).thickness(1).color({1, 1, 1, 0.3f});
    // Needle anchored at bottom-center.
    stage.rect({0, 0, 8, 90}).anchor(0.5f, 1.0f).position(300, 150).rotation(135).color(
        Color::Orange);
    stage.circle({300, 150}, 6).color(Color::White);
    // Mirrored + scaled group with nested rotation.
    auto& g = stage.group("g");
    g.bounds({0, 0, 100, 80}).position(400, 220).scale(-1.2f, 1.2f).rotation(-15);
    g.rect({10, 10, 80, 25}).cornerRadius(6).color(Color::Yellow);
    g.arrow({15, 55}, {85, 55}).thickness(4).color(Color::Red);
    checkScene("transforms", r.render(stage, 0.0f));
}

void sceneFilters(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130});
    stage.image(view).frame({170, 10, 150, 130}).blur(5);
    stage.image(view).frame({330, 10, 140, 130}).grayscale();
    stage.image(view).frame({10, 160, 150, 130}).pixelate(10);
    stage.image(view).frame({170, 160, 150, 130}).filter(
        ColorTransform().contrast(1.6f).saturation(0.4f));
    // Group filter: applies to the composited subtree.
    auto& g = stage.group("g").invert();
    g.rect({340, 170, 60, 50}).color(Color::Teal);
    g.circle({430, 230}, 30).color(Color::Orange);
    checkScene("filters", r.render(stage, 0.0f));
}

// Beauty is variance-driven (E[x²]−E[x]², 25 sparse taps), so it gets its
// own cross-backend case: the gradient regions smooth hard, the checker
// cells gate off, and whiten's curve stack exercises pow/mix parity.
void sceneBeauty(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130});
    stage.image(view).frame({170, 10, 150, 130}).filter(Beauty());
    stage.image(view).frame({330, 10, 140, 130}).filter(Beauty().smoothing(1.0f).sharpen(0.0f));
    stage.image(view).frame({10, 160, 150, 130}).filter(
        Beauty().smoothing(0.0f).whiten(0.8f));
    stage.image(view).frame({170, 160, 150, 130}).filter(Beauty().whiten(0.4f).radius(4.0f));
    checkScene("beauty", r.render(stage, 0.0f));
}

// LSD warps its sampling coordinates through a value-noise chain, so CPU
// and GPU accumulate ~1e-4 of coordinate drift and a handful of pixels
// land on the far side of a texel boundary — full checker contrast on
// isolated pixels. The per-pixel limit is therefore waived for this scene
// and the over_ratio limit (0.2% CPU / 1% GPU) carries the regression
// guard alone; a real break moves orders of magnitude more pixels.
void sceneLsd(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Lsd());
    stage.image(view).frame({170, 10, 150, 130}).filter(Lsd().time(2.4f));
    stage.image(view).frame({330, 10, 140, 130}).filter(Lsd().time(7.9f).geometry(1.2f));
    stage.image(view).frame({10, 160, 150, 130}).filter(Lsd().trip(0.2f).time(5.0f));
    stage.image(view).frame({170, 160, 150, 130}).filter(
        Lsd().time(11.3f).drift(2.0f).chroma(2.5f).geometry(-1.0f));  // digital
    const int saved_limit = g_max_diff_limit;
    g_max_diff_limit = 255;
    checkScene("lsd", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
}

// Ntsc demodulates a subcarrier (trig per tap) and Crt makes hard mask /
// branch selections — both flip isolated texels under CPU/GPU drift, so
// they take the LSD waiver: over_ratio alone carries the guard. Tiles:
// signal defaults, worst-case artifacts+RF noise, tube grille, tube slot
// mask with heavy curvature, and the full ntsc→crt chain.
void sceneTv(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Ntsc());
    stage.image(view).frame({170, 10, 150, 130})
        .filter(Ntsc().time(0.5f).artifacts(1.0f).fringing(1.0f).noise(0.2f));
    stage.image(view).frame({330, 10, 140, 130}).filter(Crt());
    stage.image(view).frame({10, 160, 150, 130})
        .filter(Crt().mask(2.0f).curvature(0.3f).scan(1.2f).converge(1.5f));
    stage.image(view).frame({170, 160, 150, 130})
        .filter(Ntsc().time(1.0f)).filter(Crt());
    // Non-integer tile scales park mask/scanline floor() boundaries a ULP
    // from pixel centers, so whole stripes flip between backends (~4%
    // measured). 5% still catches a real break (a torn body is >50%).
    const int saved_limit = g_max_diff_limit;
    const double saved_ratio = g_over_ratio_limit;
    g_max_diff_limit = 255;
    g_over_ratio_limit = 0.05;
    checkScene("tv", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
    g_over_ratio_limit = saved_ratio;
}

// Fractal raymarches a chaotic fold — Lyapunov growth turns rounding drift
// into whole-pixel differences along silhouettes, so it takes the LSD
// waiver: over_ratio alone carries the guard. Tiles cover the static frame
// (t=0), flight+morph over time, glow extremes, and the video-blend path.
void sceneFractal(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Fractal());
    stage.image(view).frame({170, 10, 150, 130}).filter(Fractal().time(3.7f));
    stage.image(view).frame({330, 10, 140, 130}).filter(
        Fractal().time(9.2f).morph(1.8f).glow(2.0f));
    stage.image(view).frame({10, 160, 150, 130}).filter(
        Fractal().time(5.5f).flight(2.5f).glow(0.0f));
    stage.image(view).frame({170, 160, 150, 130}).filter(
        Fractal().time(13.1f).blend(1.0f));
    const int saved_limit = g_max_diff_limit;
    const double saved_ratio = g_over_ratio_limit;
    g_max_diff_limit = 255;
    g_over_ratio_limit = 0.05;
    checkScene("fractal", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
    g_over_ratio_limit = saved_ratio;
}

// Oilpaint picks the least-variant of four quadrants — a hard selection, so
// a whisker of CPU/GPU drift near a variance tie flips a whole daub. Same
// waiver as LSD/notebook: over_ratio carries the guard.
void sceneOilpaint(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Oilpaint());
    stage.image(view).frame({170, 10, 150, 130})
        .filter(Oilpaint().radius(8.0f));
    stage.image(view).frame({330, 10, 140, 130})
        .filter(Oilpaint().radius(6.0f).levels(16.0f));
    stage.image(view).frame({10, 160, 150, 130})
        .filter(Oilpaint().jitter(0.0f));
    stage.image(view).frame({170, 160, 150, 130})
        .filter(Oilpaint().radius(7.0f).jitter(1.4f)).filter(Median());
    const int saved_limit = g_max_diff_limit;
    g_max_diff_limit = 255;
    checkScene("oilpaint", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
}

// The art-style family (docs/design/art_filters.ja.md). The scenes exercise
// every parameter slot across its intended range. Anime is continuous
// end-to-end (soft quantization, smoothstep ink) and holds the normal
// tolerance; watercolor and sumie read the source at noise-wobbled
// positions, so they share notebook's waiver — ~1e-4 of CPU/GPU drift in
// the wobble moves a read across a full-contrast test edge at an isolated
// texel. over_ratio carries the guard.
void sceneAnime(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Anime());
    stage.image(view).frame({170, 10, 150, 130}).filter(Anime().levels(3.0f).lines(1.6f));
    stage.image(view).frame({330, 10, 140, 130}).filter(Anime().lines(0.0f).vivid(1.0f));
    stage.image(view).frame({10, 160, 150, 130}).filter(Anime().smooth(0.0f).width(3.0f));
    stage.image(view).frame({170, 160, 150, 130}).filter(Anime().levels(8.0f).smooth(1.0f));
    checkScene("anime", r.render(stage, 0.0f));
}

void sceneWatercolor(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Watercolor());
    stage.image(view).frame({170, 10, 150, 130}).filter(Watercolor().wash(9.0f).wobble(1.6f));
    stage.image(view).frame({330, 10, 140, 130}).filter(Watercolor().grain(1.5f).edge(1.6f));
    stage.image(view).frame({10, 160, 150, 130}).filter(Watercolor().dilute(0.8f).grain(0.0f));
    stage.image(view).frame({170, 160, 150, 130}).filter(Watercolor().edge(0.0f).wobble(0.0f));
    const int saved_limit = g_max_diff_limit;
    const double saved_ratio = g_over_ratio_limit;
    g_max_diff_limit = 255;
    g_over_ratio_limit = 0.02;
    checkScene("watercolor", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
    g_over_ratio_limit = saved_ratio;
}

void sceneSumie(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Sumie());
    stage.image(view).frame({170, 10, 150, 130}).filter(Sumie().ink(1.7f).outline(1.5f));
    stage.image(view).frame({330, 10, 140, 130}).filter(Sumie().bleed(9.0f).dry(1.3f));
    stage.image(view).frame({10, 160, 150, 130})
        .filter(Sumie().chroma(0.4f).paper(view));  // real-paper path (+2 flag)
    stage.image(view).frame({170, 160, 150, 130}).filter(Sumie().ink(0.4f).dry(0.0f).outline(0.0f));
    const int saved_limit = g_max_diff_limit;
    g_max_diff_limit = 255;
    checkScene("sumie", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
}

// The living sumi-e: 30 diffusion ticks of the persistent ink field
// (persistent_buffers P1), then compare — the "N renders, then the frame"
// generalization of byte-reproducibility from the design doc §2.3. The
// recurrence is contractive (diffusion averages), so cross-backend drift
// damps rather than compounds; the waiver covers the bleed's soft ramps.
void sceneSumieLiving(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 300, 280}).filter(Sumie());
    stage.image(view).frame({320, 10, 150, 130}).filter(Sumie().bleed(9.0f).ink(1.5f));
    for (int i = 0; i < 29; ++i) {
        r.render(stage, 0.016f);
    }
    const int saved_limit = g_max_diff_limit;
    g_max_diff_limit = 255;
    checkScene("sumie_living", r.render(stage, 0.016f));
    g_max_diff_limit = saved_limit;
}

// Inkline's ETF field converges over frames (P1 state) — render 4 ticks,
// then compare. Shares the stateful waiver: nearest state reads feed
// streamline walks, so a whisker of drift moves a stroke.
void sceneInkline(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Inkline());
    stage.image(view).frame({170, 10, 150, 130}).filter(Inkline().width(3.0f).ink(1.5f));
    stage.image(view).frame({330, 10, 140, 130})
        .filter(Anime().lines(0.0f)).filter(Inkline());
    stage.image(view).frame({10, 160, 150, 130}).filter(Inkline().matte(1.0f));
    stage.image(view).frame({170, 160, 150, 130})
        .filter(Inkline().detail(0.4f).coherence(0.3f));
    for (int i = 0; i < 3; ++i) {
        r.render(stage, 0.016f);
    }
    const int saved_limit = g_max_diff_limit;
    const double saved_ratio = g_over_ratio_limit;
    g_max_diff_limit = 255;
    g_over_ratio_limit = 0.02;
    checkScene("inkline", r.render(stage, 0.016f));
    g_max_diff_limit = saved_limit;
    g_over_ratio_limit = saved_ratio;
}

// Impressionist wears the covering dab with the best score — a hard argmax
// like oilpaint's quadrant pick, but a flipped tie here repaints a whole
// DAB (hundreds of texels), so the ratio guard sits above the default.
void sceneImpressionist(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Impressionist());
    stage.image(view).frame({170, 10, 150, 130}).filter(Impressionist().stroke(12.0f));
    stage.image(view).frame({330, 10, 140, 130})
        .filter(Impressionist().vibrance(1.6f).relief(1.5f));
    stage.image(view).frame({10, 160, 150, 130})
        .filter(Impressionist().flow(0.0f).time(3.0f));
    stage.image(view).frame({170, 160, 150, 130})
        .filter(Impressionist().vibrance(0.0f).relief(0.0f).time(7.5f));
    const int saved_limit = g_max_diff_limit;
    const double saved_ratio = g_over_ratio_limit;
    g_max_diff_limit = 255;
    g_over_ratio_limit = 0.03;
    checkScene("impressionist", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
    g_over_ratio_limit = saved_ratio;
}

// Stainedglass picks the nearest Worley site (hard selection at pane
// borders), pixelart quantizes hard against the Bayer threshold — both get
// the oilpaint waiver for the same reason.
void sceneStainedglassPixelart(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Stainedglass());
    stage.image(view).frame({170, 10, 150, 130})
        .filter(Stainedglass().size(40.0f).lead(1.4f).light(1.2f));
    stage.image(view).frame({330, 10, 140, 130})
        .filter(Stainedglass().irregular(0.15f).saturate(1.2f));
    stage.image(view).frame({10, 160, 150, 130}).filter(Pixelart());
    stage.image(view).frame({170, 160, 150, 130})
        .filter(Pixelart().size(10.0f).colors(3.0f).dither(1.0f).saturate(0.8f));
    const int saved_limit = g_max_diff_limit;
    g_max_diff_limit = 255;
    checkScene("stainedglass_pixelart", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
}

void sceneBokeh(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Bokeh());
    stage.image(view).frame({170, 10, 150, 130})
        .filter(Bokeh().radius(14.0f).blades(6.0f));
    stage.image(view).frame({330, 10, 140, 130})
        .filter(Bokeh().radius(14.0f).blades(3.0f).rotation(30.0f));
    stage.image(view).frame({10, 160, 150, 130})
        .filter(Bokeh().radius(20.0f).highlight(2.0f));
    stage.image(view).frame({170, 160, 150, 130})
        .filter(Bokeh().radius(6.0f).blades(5.0f).highlight(0.0f));
    checkScene("bokeh", r.render(stage, 0.0f));
}

// Notebook shares LSD's waiver, for the same reason: the stroke convolution
// reads gradients at hash-jittered positions, so ~1e-4 of CPU/GPU drift can
// flip an isolated texel across a stroke edge at full contrast. The
// over_ratio limit alone guards the regression.
void sceneNotebook(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).filter(Notebook());
    stage.image(view).frame({170, 10, 150, 130})
        .filter(Notebook()).filter(Wobble().time(2.4f));   // page shiver chained
    stage.image(view).frame({330, 10, 140, 130})
        .filter(Notebook().chroma(0.0f).grid(0.0f));   // plain graphite
    stage.image(view).frame({10, 160, 150, 130})
        .filter(Notebook().scale(0.25f));              // fine strokes
    stage.image(view).frame({170, 160, 150, 130})
        .filter(Wobble().amount(8.0f).time(1.1f).fps(0.0f));  // wobble alone
    const int saved_limit = g_max_diff_limit;
    const double saved_ratio = g_over_ratio_limit;
    g_max_diff_limit = 255;
    g_over_ratio_limit = 0.02;   // the wobble tiles add noise-warped reads
    checkScene("notebook", r.render(stage, 0.0f));
    g_max_diff_limit = saved_limit;
    g_over_ratio_limit = saved_ratio;
}

// Image-driven layer masks (docs/design/layer_mask_input.ja.md): a radial
// alpha over the layer's bounds, plain / inverted / feathered / with a
// filter first (mask applies to the FINISHED pixels).
void sceneLayerMask(Renderer& r) {
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    std::vector<uint8_t> mimg(64 * 64 * 4, 0);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const float dx = (x - 31.5f) / 32.0f;
            const float dy = (y - 31.5f) / 32.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float a = std::clamp(1.6f - 2.2f * d, 0.0f, 1.0f);
            mimg[(static_cast<size_t>(y) * 64 + x) * 4 + 3] =
                static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
    const ImageView mview{64, 64, mimg.data(), 0};
    stage.image(view).frame({10, 10, 150, 130}).mask(mview);
    stage.image(view).frame({170, 10, 150, 130}).mask(mview).maskInvert(true);
    stage.image(view).frame({330, 10, 140, 130}).mask(mview).maskFeather(9.0f);
    stage.image(view).frame({10, 160, 150, 130}).grayscale().mask(mview);
    stage.image(view).frame({170, 160, 150, 130}).mask(mview).maskInvert(true).maskFeather(14.0f);
    checkScene("layer_mask", r.render(stage, 0.0f));
}

void sceneAnimation(Renderer& r) {
    // §13-6: "the screen at t = 0.15 s" must be reproducible.
    Stage stage(320, 200);
    auto& box = stage.rect({0, 0, 60, 60}).position(50, 100).color(Color::Teal);
    auto& dot = stage.circle({50, 40}, 12).color(Color::Orange);
    {
        Transaction t(0.3f, Ease::InOut);
        box.position(270, 100).rotation(90);
        dot.opacity(0.2f);
    }
    stage.advance(0.05f);
    stage.advance(0.05f);
    stage.advance(0.05f);  // t = 0.15 — mid-flight
    checkScene("animation_t015", r.render(stage, 0.0f));
}

void sceneImagePaste(Renderer& r) {
    // §5-2b: partial copy & paste via sourceRect + frame, plus fit modes.
    Stage stage(480, 300);
    const auto img = makeTestImage(160, 120);
    const ImageView view{160, 120, img.data(), 0};
    stage.image(view, Fit::Contain).frame({10, 10, 140, 130}).background({1, 1, 1, 0.08f});
    stage.image(view, Fit::Cover).frame({160, 10, 140, 130});
    stage.image(view, Fit::Fill).frame({310, 10, 160, 130});
    stage.image(view).sourceRect({40, 30, 60, 40}).frame({60, 160, 180, 120});
    stage.image(view).sourceRect({40, 30, 60, 40}).frame({280, 160, 90, 120}).opacity(0.6f);
    checkScene("image_paste", r.render(stage, 0.0f));
}

void sceneUi(Renderer& r) {
    // §10: controls are prefab subtrees; states are attribute overrides.
    Stage stage(480, 300);
    stage.rect({0, 0, 480, 300}).color({0.10f, 0.12f, 0.15f, 1});
    ui::Button normal(stage.root(), {24, 24, 180, 56}, "収穫開始");
    ui::Button pressed(stage.root(), {24, 100, 180, 56}, "PRESSED");
    ui::Button disabled(stage.root(), {24, 176, 180, 56}, "DISABLED");
    disabled.enabled(false);
    ui::Switch sw_off(stage.root(), {260, 28, 96, 48});
    ui::Switch sw_on(stage.root(), {260, 104, 96, 48});
    sw_on.setOn(true, false);
    stage.pointerDown({110, 128});  // hold the second button pressed
    stage.advance(0.5f);            // settle all state transitions
    checkScene("ui_controls", r.render(stage, 0.0f));
}

void sceneUiCatalog(Renderer& r) {
    // The full §10 control catalog, one state each, dropdown open.
    Stage stage(480, 420);
    stage.rect({0, 0, 480, 420}).color({0.10f, 0.12f, 0.15f, 1});
    ui::Slider slider(stage.root(), {24, 24, 200, 28}, 0.6f);
    ui::Segmented seg(stage.root(), {24, 76, 240, 40}, {"手動", "巡回", "追従"}, 1);
    ui::Gauge gauge(stage.root(), {400, 70}, 46);
    gauge.setValue(0.64f);
    ui::Dropdown dd(stage.root(), {24, 140, 200, 44}, {"標準", "低速", "高速", "点検"}, 2);
    stage.pointerDown({100, 160});
    stage.pointerUp({100, 160});  // open the dropdown
    stage.advance(0.5f);          // settle transitions
    checkScene("ui_catalog", r.render(stage, 0.0f));
}

void sceneRipple(Renderer& r) {
    // The water-refraction wave at t = 0.15 s: the grid beneath must bend.
    Stage stage(480, 300);
    auto& water = stage.group("water");
    water.bounds({0, 0, 480, 300});
    water.rect({0, 0, 480, 300}).color({0.08f, 0.12f, 0.16f, 1});
    water.grid(40).color({1, 1, 1, 0.25f});
    fx::Ripple ripple(water);
    ripple.splash({160, 150});
    ripple.pointerMoved({360, 110});
    for (int i = 0; i < 3; ++i) {
        ripple.tick(0.05f);
        stage.advance(0.05f);
    }
    checkScene("ripple_t015", r.render(stage, 0.0f));
}

void sceneText(Renderer& r) {
    Stage stage(480, 300);
    stage.rect({0, 0, 480, 300}).color({0.1f, 0.12f, 0.15f, 1});
    stage.text("走行中 — Stage L0", {20, 20}).size(30);
    stage.text("centered", {240, 90}).size(22).align(Align::Center).color(Color::Teal);
    stage.text("right", {460, 130}).size(22).align(Align::Right).color(Color::Orange);
    stage.text("影付きテキスト", {20, 180}).size(34).shadow(2, 3, 6);
    stage.text("small 12px 日本語", {20, 240}).size(12).color({1, 1, 1, 0.8f});
    const Surface& s = r.render(stage, 0.0f);
    checkScene("text", s);
}

}  // namespace

int main(int argc, char** argv) {
    std::string renderer_name = "cpu";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--update") == 0) {
            g_update = true;
        } else if (std::strncmp(argv[i], "--renderer=", 11) == 0) {
            renderer_name = argv[i] + 11;
        }
    }
#ifdef GOLDEN_DIR
    g_dir = GOLDEN_DIR;
#else
    g_dir = "tests/golden";
#endif
    std::unique_ptr<Renderer> renderer;
    if (renderer_name == "vulkan") {
        if (g_update) {
            std::fprintf(stderr, "goldens are defined by the CPU reference; refuse --update\n");
            return 1;
        }
#ifdef FS_HAVE_VULKAN
        try {
            renderer = std::make_unique<VulkanRenderer>();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "skip: %s\n", e.what());
            return 77;
        }
        // fp16 targets, GPU bilinear rounding, and sampler quantization
        // shift a few LSBs; regressions are orders of magnitude larger.
        g_max_diff_limit = 48;
        g_over_ratio_limit = 0.01;
        g_actual_suffix = ".vulkan.actual.ppm";
#else
        std::fprintf(stderr, "skip: built without Vulkan\n");
        return 77;
#endif
    } else {
        renderer = std::make_unique<CpuRenderer>();
    }
    Renderer& r = *renderer;
    sceneShapes(r);
    sceneAttributes(r);
    sceneTransforms(r);
    sceneFilters(r);
    sceneBeauty(r);
    sceneLsd(r);
    sceneFractal(r);
    sceneNotebook(r);
    sceneBokeh(r);
    sceneOilpaint(r);
    sceneAnime(r);
    sceneWatercolor(r);
    sceneSumie(r);
    sceneSumieLiving(r);
    sceneInkline(r);
    sceneImpressionist(r);
    sceneStainedglassPixelart(r);
    sceneLayerMask(r);
    sceneTv(r);
    sceneAnimation(r);
    sceneImagePaste(r);
    sceneUi(r);
    sceneUiCatalog(r);
    sceneRipple(r);
    sceneText(r);
    if (g_failures == 0) {
        std::printf("all golden_tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d golden failure(s)\n", g_failures);
    return 1;
}
