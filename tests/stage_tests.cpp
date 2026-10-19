// stage_tests — L0 contract tests: geometry (CALayer semantics), implicit
// animation via Transaction with injected time (§13-6), hit-testing, limits,
// the filter metadata table, and render determinism.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <fluent_scene/fluent_scene.hpp>

using namespace fluent_scene;

namespace fluent_scene::detail {
extern bool g_cpu_polygon_reference;  // cpu_renderer.cpp test seam
}

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

void testGeometry() {
    Stage stage(1000, 800);
    // frame is sugar over bounds+position+anchor (CALayer §4.1).
    auto& panel = stage.rect({0, 0, 200, 100}).frame({100, 50, 200, 100});
    CHECK(near(panel.position().x, 200));  // anchor (0.5,0.5) → center
    CHECK(near(panel.position().y, 100));
    const Rect f = panel.frame();
    CHECK(near(f.x, 100) && near(f.y, 50) && near(f.w, 200) && near(f.h, 100));

    // position places the anchor, not the frame origin.
    panel.anchor(0, 0).position(300, 300);
    CHECK(near(panel.frame().x, 300) && near(panel.frame().y, 300));

    // Content bbox becomes automatic bounds (§15-1).
    auto& c = stage.circle({500, 400}, 50);
    const Rect cb = c.bounds();
    CHECK(near(cb.x, 450) && near(cb.y, 350) && near(cb.w, 100) && near(cb.h, 100));

    // frame() on a rotated layer is diagnosed.
    auto& r = stage.rect({0, 0, 10, 10}).rotation(30);
    r.frame({0, 0, 20, 20});
    bool diagnosed = false;
    for (const std::string& d : stage.drainDiagnostics()) {
        if (d.find("transformed layer") != std::string::npos) {
            diagnosed = true;
        }
    }
    CHECK(diagnosed);
}

void testTransaction() {
    Stage stage(100, 100);
    auto& r = stage.rect({0, 0, 10, 10}).id("r");

    // Outside a transaction: snap.
    r.opacity(0.5f);
    CHECK(near(r.presentedOpacity(), 0.5f));

    // Inside: animate; injected dt drives it deterministically.
    {
        Transaction t(1.0f, Ease::Linear);
        r.opacity(1.0f).position(50, 0);
    }
    CHECK(near(r.presentedOpacity(), 0.5f));  // not advanced yet
    stage.advance(0.5f);
    CHECK(near(r.presentedOpacity(), 0.75f));
    // position animates from its auto value (content center (5,5)) to (50,0).
    CHECK(near(r.presentedPosition().x, 27.5f));
    CHECK(near(r.presentedPosition().y, 2.5f));
    stage.advance(0.5f);
    CHECK(near(r.presentedOpacity(), 1.0f));
    CHECK(near(r.opacity(), 1.0f));  // model value was 1.0 all along

    // Retarget mid-flight continues from the presentation value (no jump).
    {
        Transaction t(1.0f, Ease::Linear);
        r.opacity(0.0f);
    }
    stage.advance(0.5f);
    CHECK(near(r.presentedOpacity(), 0.5f));
    {
        Transaction t(1.0f, Ease::Linear);
        r.opacity(1.0f);
    }
    stage.advance(0.0f);
    CHECK(near(r.presentedOpacity(), 0.5f));  // starts where it was
}

// Colors are animatable (§9 L1): content color, background, and an existing
// border's color retarget from the presentation value; outside a
// Transaction they snap as before.
void testColorTransaction() {
    Stage stage(100, 100);
    auto& r = stage.rect({0, 0, 10, 10}).color(Color{0, 0, 1, 1}).background(Color::Black);
    r.border(2, Color::Black);
    CHECK(near(r.presentedColor().b, 1));
    {
        Transaction t(1.0f, Ease::Linear);
        r.color(Color{1, 0, 0, 1}).background(Color::White).border(4, Color::White);
    }
    CHECK(near(r.colorValue().r, 1) && near(r.presentedColor().r, 0));
    CHECK(near(r.borderValue()->width, 4));  // width snaps
    stage.advance(0.5f);
    CHECK(near(r.presentedColor().r, 0.5f) && near(r.presentedColor().b, 0.5f));
    CHECK(near(r.presentedBackground().g, 0.5f));
    CHECK(near(r.presentedBorder()->color.g, 0.5f));
    stage.advance(0.5f);
    CHECK(near(r.presentedColor().r, 1) && near(r.presentedColor().b, 0));
    // A border added inside a Transaction appears at its color (nothing to
    // animate from).
    auto& s = stage.rect({0, 0, 10, 10});
    {
        Transaction t(1.0f, Ease::Linear);
        s.border(1, Color{1, 0, 0, 1});
    }
    CHECK(near(s.presentedBorder()->color.r, 1));
    // Premultiplied mix: Transparent → opaque orange keeps the hue.
    CHECK(near(animMix(Color::Transparent, Color{1, 0.5f, 0, 1}, 0.5f).g, 0.5f));
}

// removeWhenSettled: the layer animates out, stays rendered but unfindable
// and unhittable, and detaches on the advance that settles it — together
// with a leaving descendant, and without touching an explicit remove().
void testRemoveWhenSettled() {
    Stage stage(100, 100);
    auto& card = stage.group("card").frame({0, 0, 50, 50});
    auto& inner = card.rect({0, 0, 50, 50}).id("inner");
    inner.onPointer([](const PointerEvent&) {});
    CHECK(stage.layerCount() == 3);
    {
        Transaction t(1.0f, Ease::Linear);
        card.opacity(0);
        inner.opacity(0.5f);
    }
    inner.removeWhenSettled();
    card.removeWhenSettled();
    CHECK(card.leaving() && stage.find("card") == nullptr && stage.find("inner") == nullptr);
    CHECK(!stage.pointerDown({10, 10}));  // leaving layers take no pointers
    stage.advance(0.5f);
    CHECK(stage.layerCount() == 3 && near(card.presentedOpacity(), 0.5f));
    stage.advance(0.5f);
    CHECK(stage.layerCount() == 1);  // both gone, parent first, no double free

    // Nothing in flight: gone on the next advance, even a zero-dt one.
    auto& still = stage.rect({0, 0, 5, 5}).id("still");
    still.removeWhenSettled();
    CHECK(stage.layerCount() == 2);
    stage.advance(0);
    CHECK(stage.layerCount() == 1);

    // remove() during the exit wins; the pending entry is forgotten.
    auto& quick = stage.rect({0, 0, 5, 5});
    {
        Transaction t(1.0f);
        quick.opacity(0);
    }
    quick.removeWhenSettled();
    quick.remove();
    stage.advance(2.0f);
    CHECK(stage.layerCount() == 1);
    stage.root().removeWhenSettled();  // diagnosed no-op
    CHECK(!stage.drainDiagnostics().empty() && stage.layerCount() == 1);
}

// The CPU renderer's culled polygon fill (near-edge distance + far-edge
// parity per row chunk) must be bit-identical to the brute-force
// sd_fill(polygonDistance) it replaces — random, self-intersecting and
// degenerate outlines, under scale / mirror / fractional output sizes.
void testPolygonFillExact() {
    uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    struct Case {
        uint32_t out_w, out_h;
        Vec2 scale;
    };
    const Case cases[] = {{300, 200, {1, 1}},     {333, 217, {1, 1}},  {300, 200, {1.37f, 0.8f}},
                          {300, 200, {-1, 1}},    {300, 200, {1, -1.5f}}, {91, 67, {2.5f, 2.5f}}};
    int checked = 0;
    for (int shape = 0; shape < 6; ++shape) {
        std::vector<Vec2> pts;
        if (shape == 0) {  // a waveform band, like the voice meter
            for (int i = 0; i <= 60; ++i) {
                pts.push_back({20 + 4.3f * i, 100 - 40 * rnd()});
            }
            for (int i = 60; i >= 0; --i) {
                pts.push_back({20 + 4.3f * i, 100 + 40 * rnd()});
            }
        } else if (shape == 1) {  // a spiky star
            for (int i = 0; i < 48; ++i) {
                const float a = i * 6.2831853f / 48;
                const float r = (i % 2 ? 30 : 90) + 10 * rnd();
                pts.push_back({150 + r * std::cos(a), 100 + r * std::sin(a)});
            }
        } else if (shape == 2) {  // self-intersecting scribble
            for (int i = 0; i < 40; ++i) {
                pts.push_back({10 + 280 * rnd(), 10 + 180 * rnd()});
            }
        } else if (shape == 3) {  // degenerate: repeats, collinear, horizontal runs
            pts = {{20, 20}, {20, 20}, {120, 20}, {220, 20}, {220, 20.5f}, {220, 180},
                   {120, 180}, {120, 100}, {120, 100}, {20, 180}};
        } else if (shape == 4) {  // sub-pixel sliver
            pts = {{10, 50}, {290, 50.3f}, {290, 50.9f}, {10, 50.4f}};
        } else {  // vertices exactly on pixel centers and edges
            pts = {{10.5f, 10.5f}, {200.5f, 10.5f}, {100, 150}, {150, 150}, {10, 190}};
        }
        for (const Case& c : cases) {
            Stage stage(300, 200);
            stage.group()
                .bounds({0, 0, 300, 200})
                .position(150, 100)
                .scale(c.scale.x, c.scale.y)
                .polygon(pts)
                .color({0.9f, 0.6f, 0.2f, 0.8f});
            CpuRenderer fast;
            CpuRenderer reference;
            const Surface& a = fast.render(stage, c.out_w, c.out_h, 0);
            std::vector<uint8_t> got(a.pixels, a.pixels + a.strideBytes * a.height);
            detail::g_cpu_polygon_reference = true;
            const Surface& b = reference.render(stage, c.out_w, c.out_h, 0);
            detail::g_cpu_polygon_reference = false;
            CHECK(a.strideBytes == b.strideBytes &&
                  std::memcmp(got.data(), b.pixels, got.size()) == 0);
            ++checked;
        }
    }
    CHECK(checked == 36);
}

void testHitTest() {
    Stage stage(1000, 800);
    stage.rect({0, 0, 1000, 800}).id("bg").frame({0, 0, 1000, 800});
    auto& hud = stage.group("hud");
    hud.bounds({0, 0, 340, 96}).position(24 + 170, 24 + 48);  // frame at (24,24)
    hud.rect({0, 0, 340, 96}).id("panel");
    CHECK(stage.hitTest({30, 30}) != nullptr);
    CHECK(stage.hitTest({30, 30})->id() == "panel");
    CHECK(stage.hitTest({500, 500})->id() == "bg");
    hud.hidden(true);
    CHECK(stage.hitTest({30, 30})->id() == "bg");

    // A bare group is a coordinate space: position moves its origin (the
    // §2 example), it forwards hits but is never hit itself.
    auto& g = stage.group("g2").position(100, 100);
    g.rect({0, 0, 50, 50}).id("inner");
    CHECK(stage.hitTest({120, 120}) != nullptr);
    CHECK(stage.hitTest({120, 120})->id() == "inner");
    CHECK(stage.hitTest({99, 99})->id() != "g2");
}

void testLimits() {
    Stage stage(100, 100, StageLimits{4, 2, 8, 8, 16});
    stage.rect({0, 0, 1, 1});
    stage.rect({0, 0, 1, 1});
    stage.rect({0, 0, 1, 1});  // 4 layers total with root
    bool threw = false;
    try {
        stage.rect({0, 0, 1, 1});
    } catch (const std::length_error&) {
        threw = true;
    }
    CHECK(threw);

    // Data limits clamp with a diagnostic instead of throwing.
    Stage s2(100, 100, StageLimits{16, 4, 4, 8, 8});
    auto& p = s2.polyline({{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}});
    CHECK(std::get<PolylineContent>(p.content()).points.size() == 4);
    CHECK(!s2.drainDiagnostics().empty());
}

void testFilterTable() {
    const FilterSpec* spec = findFilterSpec(FS_BILATERAL);
    CHECK(spec != nullptr);
    CHECK(std::string(spec->name) == "bilateral");
    CHECK(spec->params.size() == 2);
    CHECK(std::string(spec->params[0].name) == "radius");
    CHECK(spec->params[0].unit == FilterUnit::Length);
    CHECK(near(spec->params[1].default_value, 0.15f));

    // Typed structs carry defaults and setters from the same single source.
    const Filter f = Bilateral().sigma_color(0.3f);
    CHECK(f.mode == FS_BILATERAL);
    CHECK(near(f.values[0], 4.0f));  // default radius
    CHECK(near(f.values[1], 0.3f));

    // Every filter in the table resolves by mode.
    for (const FilterSpec& s : filterTable()) {
        CHECK(findFilterSpec(s.mode) == &s);
    }
}

void testRenderDeterminism() {
    Stage stage(320, 200);
    stage.rect({20, 20, 120, 80}).cornerRadius(10).color(Color::Teal);
    stage.circle({220, 100}, 40).thickness(6).color(Color::Orange);
    stage.line({10, 180}, {310, 150}).dash(12);
    auto& g = stage.group("g").opacity(0.8f).shadow();
    g.rect({160, 30, 100, 60}).color({1, 1, 1, 0.7f});
    g.rect({160, 30, 100, 60}).cornerRadius(8).blur(3);

    CpuRenderer r1;
    const Surface& a = r1.render(stage, 0.0f);
    CHECK(a.valid() && a.width == 320 && a.height == 200);
    std::vector<uint8_t> first(a.pixels, a.pixels + a.strideBytes * a.height);

    CpuRenderer r2;
    const Surface& b = r2.render(stage, 0.0f);
    CHECK(std::memcmp(first.data(), b.pixels, first.size()) == 0);

    // Something actually rendered.
    bool any = false;
    for (size_t i = 3; i < first.size(); i += 4) {
        if (first[i] != 0) {
            any = true;
            break;
        }
    }
    CHECK(any);

    // Scaled output renders the same scene at double size (SDF, no crash).
    const Surface& big = r1.render(stage, 640, 400, 0.0f);
    CHECK(big.width == 640 && big.height == 400);
}

// Filter slots as live state: Clock slots count dt by themselves (and
// restart from a set value), setFilterParam animates inside a Transaction
// and snaps outside one, and a filter slot mid-animation keeps the layer
// "animating" until it settles.
void testFilterClockAndAnim() {
    Stage stage(64, 64);
    Layer& l = stage.rect({0, 0, 64, 64}).filter(Liquid().time(1.0f).level(0.2f));
    stage.advance(0.25f);
    CHECK(near(l.filters()[0].values[0], 1.25f));  // time: Clock
    CHECK(near(l.filters()[0].values[8], 9.25f));  // splash: Clock (default 9)
    CHECK(near(l.filters()[0].values[1], 0.2f));   // level: Scalar, untouched
    l.setFilterParam(0, 8, 0.0f);                  // a tap: splash age restarts
    stage.advance(0.5f);
    CHECK(near(l.filters()[0].values[8], 0.5f));
    {
        Transaction t(1.0f, Ease::Linear);
        l.setFilterParam(0, 1, 1.0f);
    }
    stage.advance(0.5f);
    CHECK(near(l.filters()[0].values[1], 0.6f, 0.01f));
    stage.advance(0.6f);
    CHECK(near(l.filters()[0].values[1], 1.0f));
    l.setFilterParam(0, 1, 0.3f);  // outside a Transaction: immediate
    CHECK(near(l.filters()[0].values[1], 0.3f));
    // A bad index or slot is a diagnosed no-op, never a crash.
    l.setFilterParam(3, 0, 1.0f);
    l.setFilterParam(0, kFilterSlots, 1.0f);
    CHECK(!stage.drainDiagnostics().empty());
}

// The liquid on the CPU: a figure with its own alpha on a transparent
// ground, and the coarse path (quality < 0.75: one raymarch per block,
// bilinear upsample) still the same picture as full resolution. The mean
// difference also carries what quality changes on purpose (a 28-step march
// instead of 48, a wider antialiased rim), hence 8/255 rather than ~0.
void testLiquidCpu() {
    std::vector<uint8_t> opaque(4 * 4 * 4, 255);
    const ImageView src{4, 4, opaque.data(), 0};
    auto render = [&](float quality) {
        Stage stage(200, 120);
        stage.image(src).frame({0, 0, 200, 120}).filter(
            Liquid().time(2.0f).think(0.5f).quality(quality));
        CpuRenderer r;
        const Surface& s = r.render(stage, 0.0f);
        return std::vector<uint8_t>(s.pixels, s.pixels + s.strideBytes * s.height);
    };
    const std::vector<uint8_t> full = render(1.0f);
    const std::vector<uint8_t> coarse = render(0.3f);
    const size_t stride = 200 * 4;
    auto alpha = [&](const std::vector<uint8_t>& p, int x, int y) {
        return p[static_cast<size_t>(y) * stride + x * 4 + 3];
    };
    CHECK(alpha(full, 100, 60) > 200);   // the body at the center
    CHECK(alpha(full, 2, 2) < 8);        // transparent ground at the corner
    CHECK(alpha(coarse, 100, 60) > 200);
    CHECK(alpha(coarse, 2, 2) < 8);
    double sum = 0;
    for (size_t i = 0; i < full.size(); ++i) {
        sum += std::abs(static_cast<int>(full[i]) - static_cast<int>(coarse[i]));
    }
    const double mean = sum / full.size();
    CHECK(mean < 8.0);
    if (mean >= 8.0) {
        std::fprintf(stderr, "  liquid coarse mean diff %.2f\n", mean);
    }
}

}  // namespace

int main() {
    testGeometry();
    testTransaction();
    testColorTransaction();
    testRemoveWhenSettled();
    testHitTest();
    testLimits();
    testFilterTable();
    testRenderDeterminism();
    testPolygonFillExact();
    testFilterClockAndAnim();
    testLiquidCpu();
    if (g_failures == 0) {
        std::printf("all stage_tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
