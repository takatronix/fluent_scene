// scene_tests — the L2 contract: parse/validate rejection, reorder-invariant
// digest, canonical fmt idempotence, metadata-table honesty against the C++
// structs, the §2 YAML ↔ C++ pixel-identity golden, $params animation, and
// the design linter (§13-2).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fluent_scene/cpu_renderer.hpp>
#include <fluent_scene/fluent_scene.hpp>
#include <fluent_scene/fvs/binding.hpp>
#include <fluent_scene/fvs/compiler.hpp>
#include <fluent_scene/fvs/document.hpp>
#include <fluent_scene/fvs/inspector.hpp>
#include <fluent_scene/fvs/linter.hpp>

using namespace fluent_scene;
namespace fs = fluent_scene::fvs;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// The §2 canonical document, exactly as the design writes it.
const char* kCanonicalScene = R"(schema: fluent.scene/v1alpha2
inputs:
  camera:     { type: image.rgba8, update: per_frame }
  detections: { type: "sequence<Detection2D, 128>", update: per_frame }
layers:
  - content: { image: { source: $inputs.camera } }
  - content: { boxes: { source: $inputs.detections,
                        color: [0.1, 0.9, 0.7, 1], smoothing: 0.2 } }
  - id: hud
    position: [24, 24]
    opacity: 0.9
    shadow: {}
    sublayers:
      - content: { rect: { size: [340, 96], corner_radius: 12,
                           color: [0, 0, 0, 0.45] } }
      - content: { text: { text: "走行中", position: [16, 12], size: 28 } }
)";

bool hasCode(const fs::DiagnosticList& diags, const std::string& code) {
    for (const fs::Diagnostic& d : diags.items()) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

void testCanonicalSceneParses() {
    const fs::ParseResult r = fs::parseScene(kCanonicalScene);
    for (const auto& d : r.diagnostics.items()) {
        std::fprintf(stderr, "  diag: [%s] %s\n", d.code.c_str(), d.message.c_str());
    }
    CHECK(r.ok());
    CHECK(r.doc.layer_count == 5);
    CHECK(r.doc.inputs.size() == 2);
    CHECK(r.doc.layers.size() == 3);
    CHECK(r.doc.layers[2].id == "hud");
}

void testRejections() {
    auto errorsOf = [](const std::string& body) {
        std::string text = "schema: fluent.scene/v1alpha2\n" + body;
        return fs::parseScene(text).diagnostics;
    };
    // Unknown keys, at every level.
    CHECK(hasCode(errorsOf("layers:\n  - contnet: { rect: {} }\n"), "validate.unknown_key"));
    CHECK(hasCode(errorsOf("layers:\n  - content: { rects: {} }\n"), "validate.unknown_key"));
    CHECK(hasCode(errorsOf("layers:\n  - content: { rect: { radius: 4 } }\n"),
                  "validate.unknown_key"));
    // Type errors.
    CHECK(hasCode(errorsOf("layers:\n  - opacity: nope\n"), "validate.type"));
    CHECK(hasCode(errorsOf("layers:\n  - blend: darken\n"), "validate.type"));
    // Undeclared references are rejected before execution.
    CHECK(hasCode(errorsOf("layers:\n  - content: { image: { source: $inputs.cam } }\n"),
                  "validate.reference"));
    CHECK(hasCode(errorsOf("layers:\n  - opacity: $params.alpha\n"), "validate.reference"));
    // Input type must match the content that consumes it.
    CHECK(hasCode(errorsOf("inputs:\n  t: { type: text.utf8 }\nlayers:\n"
                           "  - content: { image: { source: $inputs.t } }\n"),
                  "validate.type"));
    // frame is sugar; spelling both is ambiguous.
    CHECK(hasCode(errorsOf("layers:\n  - frame: [0, 0, 10, 10]\n    bounds: [0, 0, 10, 10]\n"),
                  "validate.conflict"));
    // Required and exactly-one-of fields.
    CHECK(hasCode(errorsOf("layers:\n  - content: { circle: { radius: 5 } }\n"),
                  "validate.required"));
    CHECK(hasCode(errorsOf("layers:\n  - content: { text: {} }\n"), "validate.required"));
    // states is L4.
    CHECK(hasCode(errorsOf("layers:\n  - states: {}\n"), "validate.phase"));
    // Duplicate ids.
    CHECK(hasCode(errorsOf("layers:\n  - id: a\n  - id: a\n"), "validate.duplicate_id"));
    // A wrong schema is refused outright.
    CHECK(hasCode(fs::parseScene("schema: fluent.scene/v9\nlayers: []\n").diagnostics,
                  "validate.schema"));
}

void testDigestAndFmt() {
    const fs::ParseResult a = fs::parseScene(kCanonicalScene);
    CHECK(a.ok());

    // Reordering mappings cannot change the digest.
    const char* reordered = R"(layers:
  - content: { image: { source: $inputs.camera } }
  - content: { boxes: { smoothing: 0.2, color: [0.1, 0.9, 0.7, 1], source: $inputs.detections } }
  - shadow: {}
    opacity: 0.9
    position: [24, 24]
    id: hud
    sublayers:
      - content: { rect: { color: [0, 0, 0, 0.45], corner_radius: 12, size: [340, 96] } }
      - content: { text: { size: 28, position: [16, 12], text: "走行中" } }
inputs:
  detections: { type: "sequence<Detection2D, 128>", update: per_frame }
  camera: { type: image.rgba8, update: per_frame }
schema: fluent.scene/v1alpha2
)";
    const fs::ParseResult b = fs::parseScene(reordered);
    CHECK(b.ok());
    CHECK(fs::digest(a.doc) == fs::digest(b.doc));

    // A semantic change breaks it.
    std::string changed = kCanonicalScene;
    const size_t pos = changed.find("opacity: 0.9");
    changed.replace(pos, 12, "opacity: 0.8");
    const fs::ParseResult c = fs::parseScene(changed);
    CHECK(c.ok());
    CHECK(fs::digest(a.doc) != fs::digest(c.doc));

    // fmt is canonical and idempotent, and preserves the digest.
    const std::string once = fs::format(a.doc);
    const fs::ParseResult re = fs::parseScene(once);
    CHECK(re.ok());
    CHECK(fs::format(re.doc) == once);
    CHECK(fs::digest(re.doc) == fs::digest(a.doc));
}

// The metadata tables document defaults; the C++ structs own them. Keep the
// two honest against each other (single definition, enforced).
void testTableDefaultsMatchCpp() {
    auto fieldDefault = [](const char* content, const char* field) {
        const fs::FieldSpec* spec = fs::findContentSpec(content)->find(field);
        CHECK(spec != nullptr && spec->def.has_value);
        return spec->def;
    };
    const TextContent text{};
    CHECK(fieldDefault("text", "size").num[0] == text.size);
    CHECK(std::string(fieldDefault("text", "align").str) == "left");
    const CirclesContent circles{};
    CHECK(fieldDefault("circles", "radius").num[0] == circles.radius);
    const CrosshairContent crosshair{};
    CHECK(fieldDefault("crosshair", "size").num[0] == crosshair.size);
    const GridContent grid{};
    CHECK(fieldDefault("grid", "spacing").num[0] == grid.spacing);
    const ArrowContent arrow{};
    CHECK(fieldDefault("arrow", "head_size").num[0] == arrow.head_size);
    const ImageContent image{};
    CHECK(image.fit == Fit::Contain &&
          std::string(fieldDefault("image", "fit").str) == "contain");

    // Shadow/border table defaults are read from the structs directly; spot
    // check the wiring.
    const Shadow shadow{};
    CHECK(fs::shadowFields()[1].def.num[0] == shadow.radius);
    const Border border{};
    CHECK(fs::borderFields()[0].def.num[0] == border.width);
}

// -- the §2 golden: YAML and C++ must produce the identical picture ---------

std::vector<uint8_t> makeCameraPixels(uint32_t w, uint32_t h) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &pixels[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 255 / w);
            p[1] = static_cast<uint8_t>(y * 255 / h);
            p[2] = ((x / 16 + y / 16) % 2) ? 200 : 60;
            p[3] = 255;
        }
    }
    return pixels;
}

std::vector<Box> makeDetections() {
    return {{{400, 300, 280, 220}, 0.92f, "asparagus", 7},
            {{900, 500, 200, 260}, 0.71f, "asparagus", 8}};
}

void testYamlCppParity() {
    const auto cam_pixels = makeCameraPixels(320, 240);
    const ImageView cam{320, 240, cam_pixels.data(), 0};
    const std::vector<Box> dets = makeDetections();

    // YAML side.
    const fs::ParseResult parsed = fs::parseScene(kCanonicalScene);
    CHECK(parsed.ok());
    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;
    CHECK(scene.setImage("camera", cam));
    CHECK(scene.setBoxes("detections", dets));

    // C++ side — §2 of the design, verbatim.
    Stage stage(1920, 1080);
    stage.image(cam);
    stage.boxes(dets).color(Color::Teal).smoothing(0.2f);
    auto& hud = stage.group("hud").position(24, 24).opacity(0.9f).shadow();
    hud.rect({0, 0, 340, 96}).cornerRadius(12).color({0, 0, 0, 0.45f});
    hud.text("走行中", {16, 12}).size(28);

    CpuRenderer ra, rb;
    const Surface& fa = ra.render(scene.stage(), 480, 270, 0.0f);
    const Surface& fb = rb.render(stage, 480, 270, 0.0f);
    CHECK(fa.width == fb.width && fa.height == fb.height);
    size_t diff = 0;
    for (uint32_t y = 0; y < fa.height; ++y) {
        if (std::memcmp(fa.row(y), fb.row(y), static_cast<size_t>(fa.width) * 4) != 0) {
            const uint8_t* pa = fa.row(y);
            const uint8_t* pb = fb.row(y);
            for (uint32_t i = 0; i < fa.width * 4; ++i) {
                diff += pa[i] != pb[i];
            }
        }
    }
    if (diff != 0) {
        std::fprintf(stderr, "  parity: %zu differing bytes\n", diff);
    }
    CHECK(diff == 0);

    // Feeding data replaced the placeholder (no leftover synthetic layers
    // beyond the declared 5 + root).
    CHECK(scene.stage().layerCount() == 6);
}

void testParamsAnimateAndInputs() {
    const char* text = R"(schema: fluent.scene/v1alpha2
params:
  hud_alpha: { type: f32, default: 1.0, runtime_mutable: true,
               animate: { duration: 0.4, ease: linear } }
  frozen: { type: f32, default: 3, runtime_mutable: false }
layers:
  - id: hud
    opacity: $params.hud_alpha
    content: { rect: { size: [100, 50] } }
)";
    const fs::ParseResult parsed = fs::parseScene(text);
    CHECK(parsed.ok());
    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;
    Layer* hud = scene.stage().find("hud");
    CHECK(hud != nullptr);
    CHECK(near(hud->opacity(), 1.0f));  // starts at the param default

    // setParam with an animate declaration interpolates (§9).
    CHECK(scene.setParam("hud_alpha", 0.0f));
    scene.stage().advance(0.2f);
    CHECK(near(hud->presentedOpacity(), 0.5f, 0.02f));
    scene.stage().advance(0.3f);
    CHECK(near(hud->presentedOpacity(), 0.0f));

    // Type and mutability gates hold.
    CHECK(!scene.setParam("hud_alpha", true));
    CHECK(!scene.setParam("frozen", 1.0f));
    CHECK(!scene.setParam("nope", 1.0f));
    CHECK(!scene.drainDiagnostics().empty());
}

// Geometry bound to params (scale / frame / bounds): the shorthand keeps
// working, an f32 param scales uniformly, a vec2 param per axis, a rect param
// drives frame — and every one of them animates through transition/animate.
void testParamBoundGeometry() {
    const char* text = R"(schema: fluent.scene/v1alpha2
params:
  zoom:   { type: f32, default: 0.5 }
  stretch: { type: vec2, default: [2, 3] }
  place:  { type: rect, default: [10, 20, 100, 50],
            animate: { duration: 1, ease: linear } }
layers:
  - id: a
    scale: $params.zoom
    transition: { duration: 1, ease: linear }
    content: { rect: { size: [10, 10] } }
  - id: b
    scale: $params.stretch
    content: { rect: { size: [10, 10] } }
  - id: c
    scale: 1.5
    content: { rect: { size: [10, 10] } }
  - id: d
    frame: $params.place
    background: [1, 0, 0, 1]
)";
    const fs::ParseResult parsed = fs::parseScene(text);
    CHECK(parsed.ok());
    for (const auto& d : parsed.diagnostics.items()) {
        std::fprintf(stderr, "  %s %s\n", d.code.c_str(), d.message.c_str());
    }
    // Canonical form keeps the references; it round-trips to the same digest.
    const std::string once = fs::format(parsed.doc);
    CHECK(once.find("scale: $params.zoom") != std::string::npos);
    CHECK(once.find("frame: $params.place") != std::string::npos);
    CHECK(once.find("type: rect, default: [10, 20, 100, 50]") != std::string::npos);
    const fs::ParseResult re = fs::parseScene(once);
    CHECK(re.ok());
    CHECK(fs::digest(re.doc) == fs::digest(parsed.doc));

    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;
    Stage& stage = scene.stage();
    Layer* a = stage.find("a");
    Layer* b = stage.find("b");
    Layer* c = stage.find("c");
    Layer* d = stage.find("d");
    CHECK(a && b && c && d);
    CHECK(near(a->scale().x, 0.5f) && near(a->scale().y, 0.5f));
    CHECK(near(b->scale().x, 2.0f) && near(b->scale().y, 3.0f));
    CHECK(near(c->scale().x, 1.5f) && near(c->scale().y, 1.5f));
    CHECK(near(d->frame().x, 10) && near(d->frame().w, 100) && near(d->frame().h, 50));

    // f32 scale animates through the layer's transition, uniformly.
    CHECK(scene.setParam("zoom", 1.5f));
    stage.advance(0.5f);
    CHECK(near(a->presentedScale().x, 1.0f, 0.01f) && near(a->presentedScale().y, 1.0f, 0.01f));
    stage.advance(0.6f);
    CHECK(near(a->presentedScale().x, 1.5f));

    // vec2 scale without transition snaps.
    CHECK(scene.setParam("stretch", Vec2{1, 1}));
    CHECK(near(b->presentedScale().x, 1.0f) && near(b->presentedScale().y, 1.0f));

    // rect frame animates through the param's animate declaration.
    CHECK(scene.setParam("place", Rect{110, 20, 200, 50}));
    stage.advance(0.5f);
    CHECK(near(d->presentedBounds().w, 150, 0.5f));
    stage.advance(0.6f);
    CHECK(near(d->presentedBounds().w, 200) && near(d->frame().x, 110));
    CHECK(!scene.setParam("place", Vec2{0, 0}));  // type gate
    scene.drainDiagnostics();

    // Rejections: wrong param types for scale and frame.
    auto errorsOf = [](const std::string& body) {
        return fs::parseScene("schema: fluent.scene/v1alpha2\nparams:\n"
                              "  on: { type: bool }\n  p: { type: vec2 }\n"
                              "layers:\n" + body)
            .diagnostics;
    };
    CHECK(hasCode(errorsOf("  - scale: $params.on\n"), "validate.type"));
    CHECK(hasCode(errorsOf("  - frame: $params.p\n"), "validate.type"));
    CHECK(hasCode(errorsOf("  - scale: $params.missing\n"), "validate.reference"));
    CHECK(hasCode(errorsOf("  - scale: big\n"), "validate.type"));
}

// Color params (§9 L1): content style keys and shadow/border fields bind to
// $params; colors interpolate premultiplied through transition/animate.
void testParamStyleBindings() {
    const char* text = R"(schema: fluent.scene/v1alpha2
stage: { size: [200, 100] }
params:
  tint: { type: color, default: [0, 0, 1, 1] }
  edge: { type: color, default: [0, 0, 0, 0] }
  width: { type: f32, default: 4 }
layers:
  - id: wave
    transition: { duration: 1, ease: linear }
    content: { polyline: { points: [[10, 50], [190, 50]], color: $params.tint,
                           thickness: $params.width } }
  - id: plate
    frame: [0, 0, 40, 40]
    background: $params.edge
    border: { width: 2, color: $params.tint }
)";
    const fs::ParseResult parsed = fs::parseScene(text);
    CHECK(parsed.ok());
    for (const auto& d : parsed.diagnostics.items()) {
        std::fprintf(stderr, "  %s %s\n", d.code.c_str(), d.message.c_str());
    }
    const std::string once = fs::format(parsed.doc);
    CHECK(once.find("color: $params.tint") != std::string::npos);
    CHECK(fs::digest(fs::parseScene(once).doc) == fs::digest(parsed.doc));

    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;
    Stage& stage = scene.stage();
    Layer* wave = stage.find("wave");
    Layer* plate = stage.find("plate");
    CHECK(wave && plate);
    CHECK(near(wave->colorValue().b, 1) && near(wave->thickness(), 4));
    CHECK(plate->borderValue() && near(plate->borderValue()->color.b, 1));

    // The polyline's transition interpolates its color; the plate (no
    // transition) snaps its border color.
    CHECK(scene.setParam("tint", Color{1, 0, 0, 1}));
    CHECK(near(wave->colorValue().r, 1));               // model value: the target
    CHECK(near(wave->presentedColor().b, 1));           // presentation: not yet
    CHECK(near(plate->presentedBorder()->color.r, 1));  // snapped
    stage.advance(0.5f);
    CHECK(near(wave->presentedColor().r, 0.5f, 0.01f) &&
          near(wave->presentedColor().b, 0.5f, 0.01f));
    stage.advance(0.6f);
    CHECK(near(wave->presentedColor().r, 1) && near(wave->presentedColor().b, 0));
    CHECK(scene.setParam("width", 8.0f));
    CHECK(near(wave->thickness(), 8));

    // Premultiplied mixing: fading in from transparent keeps the hue.
    {
        Transaction t(1.0f, Ease::Linear);
        plate->background(Color{1, 0.5f, 0, 1});
    }
    stage.advance(0.5f);
    const Color mid = plate->presentedBackground();
    CHECK(near(mid.a, 0.5f, 0.01f) && near(mid.r, 1, 0.01f) && near(mid.g, 0.5f, 0.01f));
    stage.advance(0.6f);

    // The renderer draws the presented color: the line is red now.
    CpuRenderer renderer;
    const Surface& frame = renderer.render(stage, 200, 100, 0);
    const uint8_t* px = frame.pixels + 50 * frame.strideBytes + 100 * 4;
    CHECK(px[0] > 200 && px[2] < 40);

    // Style keys reject the wrong param type; content geometry stays literal.
    auto errorsOf = [](const std::string& body) {
        return fs::parseScene("schema: fluent.scene/v1alpha2\nparams:\n"
                              "  c: { type: color }\n  f: { type: f32 }\nlayers:\n" + body)
            .diagnostics;
    };
    CHECK(hasCode(errorsOf("  - content: { circle: { radius: 4, color: $params.f } }\n"),
                  "validate.type"));
    CHECK(hasCode(errorsOf("  - content: { circle: { radius: $params.f } }\n"),
                  "validate.reference"));
    CHECK(hasCode(errorsOf("  - border: { width: $params.c }\n"), "validate.type"));
}

// Runtime tree edits (docs/design/runtime_layers.ja.md): fragments graft
// under a declared parent with their own declarations, enter through the
// governing transition, start at current param values and fed input data;
// removal unbinds at once and can animate out first. Rejections change
// nothing, and animations elsewhere keep running.
void testRuntimeLayers() {
    const char* host = R"(schema: fluent.scene/v1alpha2
stage: { size: [400, 200] }
inputs:
  label: { type: text.utf8, update: on_change, fallback: hide }
params:
  tint: { type: color, default: [0, 0, 1, 1] }
  fig_x: { type: f32, default: 0, animate: { duration: 1, ease: linear } }
layers:
  - id: figure
    frame: [0, 0, 40, 40]
    background: [0, 1, 0, 1]
    rotation: $params.fig_x
  - id: media
    frame: [100, 0, 300, 200]
    transition: { duration: 1, ease: linear,
                  enter: { opacity: 0, scale: 0.5 }, exit: { opacity: 0 } }
)";
    const char* photo = R"(inputs:
  photo: { type: image.rgba8, update: on_change, fallback: hide }
params:
  photo_a: { type: f32, default: 1 }
layers:
  - id: photo
    opacity: $params.photo_a
    content: { rect: { rect: [0, 0, 100, 100], color: $params.tint } }
  - id: caption
    content: { text: { source: $inputs.label, size: 20 } }
)";
    const fs::ParseResult parsed = fs::parseScene(host);
    CHECK(parsed.ok());
    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;
    Stage& stage = scene.stage();
    const std::string digest0 = scene.digest();
    const uint32_t count0 = stage.layerCount();
    CHECK(scene.layerIds() == (std::vector<std::string>{"figure", "media"}));

    // Something already animating elsewhere, and state the new layers must
    // pick up: a fed input and a param moved off its default.
    CHECK(scene.setParam("fig_x", 90.0f));
    stage.advance(0.25f);
    CHECK(near(stage.find("figure")->presentedRotation(), 22.5f, 0.01f));
    CHECK(scene.setText("label", "こんにちは"));
    CHECK(scene.setParam("tint", Color{1, 0, 0, 1}));

    fs::EditResult added = scene.addLayers("media", photo, true);
    CHECK(added.ok());
    for (const auto& d : added.diagnostics.items()) {
        std::fprintf(stderr, "  %s %s\n", d.code.c_str(), d.message.c_str());
    }
    CHECK(added.ids == (std::vector<std::string>{"photo", "caption"}));
    CHECK(scene.layerIds() ==
          (std::vector<std::string>{"figure", "media", "photo", "caption"}));
    CHECK(scene.hasLayer("photo") && !scene.hasLayer("nope"));
    CHECK(scene.digest() != digest0 && scene.digest() == fs::digest(scene.doc()));
    CHECK(scene.doc().findParam("photo_a") != nullptr);
    CHECK(scene.doc().findInput("photo") != nullptr);
    CHECK(scene.doc().layer_count == 4);
    CHECK(stage.layerCount() == count0 + 2);
    // The document follows the edit: the canonical form parses back and
    // compiles to the same tree.
    const fs::ParseResult round = fs::parseScene(fs::format(scene.doc()));
    CHECK(round.ok() && fs::digest(round.doc) == scene.digest());
    CHECK(scene.nodes().size() == 4 && scene.nodes()[2].first->id == "photo" &&
          scene.nodes()[2].second == stage.find("photo"));
    // Inspector and linter walk the edited tree like a compiled one.
    {
        const auto placed = fs::placeLayers(scene);
        CHECK(std::count_if(placed.begin(), placed.end(),
                            [](const fs::PlacedLayer& p) { return p.decl != nullptr; }) == 4);
        CHECK(fs::inspectJson(scene, placed).find("\"photo\"") != std::string::npos);
        CpuRenderer lint_renderer;
        (void)fs::lint(scene, lint_renderer);
    }

    Layer* p = stage.find("photo");
    Layer* caption = stage.find("caption");
    CHECK(p != nullptr && caption != nullptr && p->parent() == stage.find("media"));
    CHECK(near(p->colorValue().r, 1) && near(p->colorValue().b, 0));  // current tint
    const auto* text = std::get_if<TextContent>(&caption->content());
    CHECK(text != nullptr && text->utf8 == "こんにちは");  // replayed input
    CHECK(!caption->hidden());                             // fallback cleared
    // Enter through media's transition: from opacity 0 / scale 0.5.
    CHECK(near(p->presentedOpacity(), 0) && near(p->presentedScale().x, 0.5f));
    CHECK(near(p->opacity(), 1) && near(p->scale().x, 1));
    stage.advance(0.5f);
    CHECK(near(p->presentedOpacity(), 0.5f, 0.01f) && near(p->presentedScale().x, 0.75f, 0.01f));
    // …while the figure's param animation kept its own clock.
    CHECK(near(stage.find("figure")->presentedRotation(), 67.5f, 0.01f));
    stage.advance(0.6f);
    CHECK(near(p->presentedOpacity(), 1) && near(p->presentedScale().x, 1));

    // Bindings of the new layers are live.
    CHECK(scene.setParam("photo_a", 0.25f));
    CHECK(near(p->opacity(), 0.25f));
    CHECK(scene.setParam("tint", Color{0, 1, 0, 1}));
    CHECK(near(p->colorValue().g, 1));
    CpuRenderer renderer;
    {
        const Surface& f = renderer.render(stage, 400, 200, 0);
        const uint8_t* px = f.pixels + 50 * f.strideBytes + 150 * 4;
        CHECK(px[1] > 20 && px[0] < 10);  // the photo rect, green at α .25
    }

    // Rejections leave the scene untouched.
    const std::string digest1 = scene.digest();
    const uint32_t count1 = stage.layerCount();
    auto rejected = [&](const char* parent, const std::string& yaml, const char* code) {
        const fs::EditResult r = scene.addLayers(parent, yaml);
        CHECK(!r.ok() && hasCode(r.diagnostics, code));
        CHECK(scene.digest() == digest1 && stage.layerCount() == count1);
    };
    rejected("nope", "layers: [ { id: x } ]\n", "edit.reference");
    rejected("media", "layers: [ { id: photo } ]\n", "validate.duplicate_id");
    rejected("media", "params: { tint: { type: f32 } }\nlayers: [ {} ]\n",
             "validate.conflict");
    rejected("media", "stage: { size: [1, 1] }\nlayers: [ {} ]\n", "validate.unknown_key");
    rejected("", "layers: [ { opacity: $params.missing } ]\n", "validate.reference");
    rejected("", "layers:\n  - id: m\n    content: { rect: {} }\n    sublayers: [ { id: n } ]\n"
                 "    mask: { source: $inputs.nope }\n",
             "compile.reference");  // fails mid-build: rolled back
    CHECK(!scene.hasLayer("m") && !scene.hasLayer("n"));
    CHECK(scene.layerIds().size() == 4);

    // Plain removal: gone at once, bindings with it; declarations stay.
    CHECK(scene.removeLayer("photo"));
    CHECK(!scene.hasLayer("photo") && stage.find("photo") == nullptr);
    CHECK(stage.layerCount() == count1 - 1);
    CHECK(scene.setParam("photo_a", 1.0f));        // still declared, binds nothing
    CHECK(!scene.removeLayer("photo") && !scene.removeLayer("root"));
    scene.drainDiagnostics();

    // The same fragment adds again (identical redeclaration is fine) — and
    // its id is free even while the old layer is still animating out.
    CHECK(scene.removeLayer("caption", true));
    CHECK(!scene.hasLayer("caption") && stage.find("caption") == nullptr);
    CHECK(caption->leaving() && near(caption->opacity(), 0));
    const uint32_t leaving_count = stage.layerCount();
    fs::EditResult again = scene.addLayers("media", photo);
    CHECK(again.ok());
    CHECK(stage.find("caption") != nullptr && stage.find("caption") != caption);
    CHECK(near(stage.find("photo")->presentedOpacity(), 1));  // not animated
    stage.advance(0.5f);
    CHECK(stage.layerCount() == leaving_count + 2);  // old caption still fading
    stage.advance(0.6f);
    CHECK(stage.layerCount() == leaving_count + 1);  // …and detached once settled
    CHECK(scene.layerIds() ==
          (std::vector<std::string>{"figure", "media", "photo", "caption"}));

    // Removing a parent takes the added subtree with it.
    CHECK(scene.removeLayer("media"));
    CHECK(scene.layerIds() == (std::vector<std::string>{"figure"}));
    CHECK(stage.layerCount() == 2 && scene.doc().layer_count == 1);
    CHECK(scene.setParam("tint", Color{1, 1, 1, 1}));
    stage.advance(0.1f);

    // Root grafts, UI controls, and a gesture in flight on a removed control.
    int taps = 0;
    scene.onUiEvent([&](const fs::UiEvent&) { ++taps; });
    CHECK(scene.addLayers("", "layers:\n  - id: go\n    frame: [200, 100, 120, 48]\n"
                              "    content: { button: { label: OK } }\n")
              .ok());
    CHECK(stage.pointerDown({260, 124}));
    CHECK(scene.removeLayer("go", true));
    CHECK(!stage.pointerUp({260, 124}) && taps == 0);  // capture dropped, no tap
    stage.advance(1.0f);
    CHECK(stage.layerCount() == 2);

    // transition.enter / exit round-trip through the canonical form.
    const fs::ParseResult t = fs::parseScene(
        "schema: fluent.scene/v1alpha2\nlayers:\n  - transition: { duration: 0.5, "
        "exit: { scale: 0.8, opacity: 0 }, enter: { position: [1, 2] } }\n");
    CHECK(t.ok());
    const std::string tf = fs::format(t.doc);
    CHECK(tf.find("enter: { position: [1, 2] }, exit: { scale: [0.8, 0.8], opacity: 0 }") !=
          std::string::npos);
    CHECK(fs::digest(fs::parseScene(tf).doc) == fs::digest(t.doc));
    CHECK(hasCode(fs::parseScene("schema: fluent.scene/v1alpha2\nlayers:\n"
                                 "  - transition: { duration: 1, enter: { blend: add } }\n")
                      .diagnostics,
                  "validate.unknown_key"));
}

void testLinter() {
    const char* text = R"(schema: fluent.scene/v1alpha2
stage: { size: [640, 360] }
layers:
  - content: { rect: { rect: [0, 0, 640, 360], color: [0.82, 0.82, 0.82, 1] } }
  - id: faint
    content: { text: { text: "low contrast", position: [40, 40], size: 30 } }
  - id: gone
    content: { circle: { center: [2000, 2000], radius: 20 } }
  - id: estop
    protected: true
    content: { rect: { rect: [500, 20, 100, 60], color: [0.9, 0.1, 0.1, 1] } }
  - content: { rect: { rect: [480, 0, 160, 120], color: [0, 0, 0, 1] } }
)";
    const fs::ParseResult parsed = fs::parseScene(text);
    CHECK(parsed.ok());
    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    CpuRenderer renderer;
    const fs::DiagnosticList lints = fs::lint(*compiled.scene, renderer);
    // White text on a light plate: unreadable, flagged.
    CHECK(hasCode(lints, "lint.contrast"));
    // The circle at (2000, 2000) is outside a 640×360 canvas.
    CHECK(hasCode(lints, "lint.offscreen"));
    // The protected E-STOP rect is buried under an opaque plate — an error.
    CHECK(hasCode(lints, "lint.protected_occluded"));
    CHECK(lints.hasErrors());
}

void testInspector() {
    const fs::ParseResult parsed = fs::parseScene(kCanonicalScene);
    CHECK(parsed.ok());
    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;
    CpuRenderer renderer;
    renderer.render(scene.stage(), 0.0f);  // settle text bounds

    const auto placed = fs::placeLayers(scene);
    CHECK(placed.size() >= 6);  // root + 5 declared (placeholders extra)

    // The HUD panel rect wins over the image at a point inside the panel.
    const auto hits = fs::visibleAt(placed, {40, 40});
    CHECK(!hits.empty());
    bool saw_rect = false, saw_image = false;
    int rect_at = -1, image_at = -1;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (std::holds_alternative<RectContent>(hits[i]->layer->content())) {
            saw_rect = true;
            rect_at = static_cast<int>(i);
        }
        if (std::holds_alternative<ImageContent>(hits[i]->layer->content())) {
            saw_image = true;
            image_at = static_cast<int>(i);
        }
    }
    CHECK(saw_rect && saw_image);
    CHECK(rect_at < image_at);  // topmost first

    // Effective opacity multiplies down the chain (hud declares 0.9).
    for (const auto* h : hits) {
        if (std::holds_alternative<RectContent>(h->layer->content())) {
            CHECK(near(h->eff_opacity, 0.9f));
        }
    }

    // A point outside everything hits only parent-filling layers.
    const auto far_hits = fs::visibleAt(placed, {1900, 1060});
    for (const auto* h : far_hits) {
        CHECK(!std::holds_alternative<RectContent>(h->layer->content()));
    }

    const std::string json = fs::inspectJson(scene, placed);
    CHECK(json.find("\"digest\"") != std::string::npos);
    CHECK(json.find("\"hud\"") != std::string::npos);
    CHECK(fs::atJson(placed, {40, 40}).find("\"rect\"") != std::string::npos);
}

void testSceneUiControls() {
    const char* text = R"(schema: fluent.scene/v1alpha2
stage: { size: [640, 480] }
params:
  light: { type: bool, default: false }
  speed: { type: f32, default: 0.4 }
layers:
  - id: start
    frame: [40, 400, 200, 56]
    content: { button: { label: "収穫開始" } }
  - id: lamp
    frame: [260, 404, 84, 44]
    content: { switch: { on: $params.light } }
  - id: vel
    frame: [360, 410, 200, 32]
    content: { slider: { value: $params.speed } }
  - id: meter
    content: { gauge: { center: [560, 120], radius: 48, value: $params.speed } }
)";
    const fs::ParseResult parsed = fs::parseScene(text);
    for (const auto& d : parsed.diagnostics.items()) {
        std::fprintf(stderr, "  ui diag: [%s] %s\n", d.code.c_str(), d.message.c_str());
    }
    CHECK(parsed.ok());
    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;
    Stage& stage = scene.stage();

    std::vector<fs::UiEvent> events;
    scene.onUiEvent([&](const fs::UiEvent& e) { events.push_back(e); });

    // A tap on the button fires an event carrying the declared id.
    CHECK(stage.pointerDown({140, 428}));
    stage.pointerUp({140, 428});
    CHECK(events.size() == 1);
    CHECK(events[0].id == "start");
    CHECK(std::string(events[0].control) == "button");

    // A tap on the switch toggles it (param default was false).
    CHECK(stage.pointerDown({302, 426}));
    stage.pointerUp({302, 426});
    CHECK(events.size() == 2);
    CHECK(events[1].id == "lamp");
    CHECK(events[1].flag == true);

    // setParam drives bound controls and fires no user event.
    const size_t before = events.size();
    CHECK(scene.setParam("light", false));
    CHECK(scene.setParam("speed", 0.9f));
    CHECK(events.size() == before);

    // A slider drag emits continuous values in 0..1.
    CHECK(stage.pointerDown({460, 426}));
    stage.pointerMove({560, 426});
    stage.pointerUp({560, 426});
    CHECK(events.size() > before);
    CHECK(events.back().id == "vel");
    CHECK(events.back().value >= 0.0f && events.back().value <= 1.0f);

    // Rejections: a button without a frame, a param-type mismatch.
    auto errorsOf = [](const std::string& body) {
        return fs::parseScene("schema: fluent.scene/v1alpha2\n" + body).diagnostics;
    };
    CHECK(hasCode(errorsOf("layers:\n  - content: { button: { label: x } }\n"),
                  "validate.required"));
    CHECK(hasCode(errorsOf("params:\n  s: { type: f32, default: 1 }\nlayers:\n"
                           "  - frame: [0, 0, 80, 40]\n"
                           "    content: { switch: { on: $params.s } }\n"),
                  "validate.type"));
}

void testBindingDoc() {
    const char* kBinding = R"(schema: fluent.binding/v1alpha1
kind: Binding
metadata: { name: hud_ros2 }
scene: { name: camera_detection_hud }
bindings:
  camera:
    source: { adapter: ros2, topic: /camera/color/image_raw,
              message_type: sensor_msgs/msg/Image, qos: sensor_data }
    converter: ros_image_to_rgba8
  detections:
    source: { adapter: ros2, topic: /perception/detections,
              message_type: vision_msgs/msg/Detection2DArray, qos: sensor_data }
    converter: ros_detections_to_detection2d
outputs:
  frame:
    sink: { adapter: ros2, topic: /visualization/composite,
            message_type: sensor_msgs/msg/Image, qos: sensor_data }
    converter: rgba8_to_ros_image
)";
    const fs::BindingParseResult b = fs::parseBinding(kBinding);
    for (const auto& d : b.diagnostics.items()) {
        std::fprintf(stderr, "  bind diag: [%s] %s\n", d.code.c_str(), d.message.c_str());
    }
    CHECK(b.ok());
    CHECK(b.doc.bindings.size() == 2);
    CHECK(b.doc.outputs.size() == 1);
    CHECK(b.doc.findBinding("camera") != nullptr);

    // Cross-validation against the §2 scene: clean.
    const fs::ParseResult scene = fs::parseScene(kCanonicalScene);
    CHECK(scene.ok());
    fs::DiagnosticList cross;
    fs::validateBindingAgainstScene(b.doc, scene.doc, cross);
    CHECK(!cross.hasErrors());

    // A binding naming an undeclared input is an error; a converter of the
    // wrong type is an error.
    const char* kBad = R"(schema: fluent.binding/v1alpha1
bindings:
  nonexistent:
    source: { adapter: ros2, topic: /t, message_type: std_msgs/msg/String }
    converter: ros_string_to_utf8
  camera:
    source: { adapter: ros2, topic: /t2, message_type: std_msgs/msg/String }
    converter: ros_string_to_utf8
)";
    const fs::BindingParseResult bad = fs::parseBinding(kBad);
    CHECK(bad.ok());
    fs::DiagnosticList cross2;
    fs::validateBindingAgainstScene(bad.doc, scene.doc, cross2);
    CHECK(hasCode(cross2, "bind.unknown_input"));
    CHECK(hasCode(cross2, "bind.type_mismatch"));
    CHECK(hasCode(cross2, "bind.unbound_input"));  // detections went unbound

    // Shape rejections: adapter, qos, message type vs converter, relative
    // topic names, unknown converters.
    auto errorsOf = [](const std::string& body) {
        return fs::parseBinding("schema: fluent.binding/v1alpha1\n" + body).diagnostics;
    };
    CHECK(hasCode(errorsOf("bindings:\n  a:\n    source: { adapter: dds, topic: /t }\n"
                           "    converter: ros_string_to_utf8\n"),
                  "bind.adapter"));
    CHECK(hasCode(errorsOf("bindings:\n  a:\n    source: { adapter: ros2, topic: t }\n"
                           "    converter: ros_string_to_utf8\n"),
                  "bind.topic"));
    CHECK(hasCode(errorsOf("bindings:\n  a:\n    source: { adapter: ros2, topic: /t, "
                           "qos: best }\n    converter: ros_string_to_utf8\n"),
                  "bind.qos"));
    CHECK(hasCode(errorsOf("bindings:\n  a:\n    source: { adapter: ros2, topic: /t, "
                           "message_type: sensor_msgs/msg/Image }\n"
                           "    converter: ros_string_to_utf8\n"),
                  "bind.message_type"));
    CHECK(hasCode(errorsOf("bindings:\n  a:\n    source: { adapter: ros2, topic: /t }\n"
                           "    converter: no_such_converter\n"),
                  "bind.converter"));
}

// -- lut: the image-parameter filter ----------------------------------------

// A channel-swap grade (out = bgr) as a tiled atlas: `tiles`² slices of
// N=tiles² levels — the smallest legal atlas is tiles=2 (8×8, 4³).
std::vector<uint8_t> makeSwapLutAtlas(uint32_t tiles) {
    const uint32_t n = tiles * tiles;
    const uint32_t w = tiles * n;
    std::vector<uint8_t> px(static_cast<size_t>(w) * w * 4);
    for (uint32_t s = 0; s < n; ++s) {
        const uint32_t ox = (s % tiles) * n;
        const uint32_t oy = (s / tiles) * n;
        for (uint32_t gy = 0; gy < n; ++gy) {
            for (uint32_t gx = 0; gx < n; ++gx) {
                uint8_t* p = &px[(static_cast<size_t>(oy + gy) * w + ox + gx) * 4];
                const auto level = [n](uint32_t k) {
                    return static_cast<uint8_t>(k * 255 / (n - 1));
                };
                p[0] = level(s);   // out.r = in.b
                p[1] = level(gy);  // out.g = in.g
                p[2] = level(gx);  // out.b = in.r
                p[3] = 255;
            }
        }
    }
    return px;
}

void testLutFilter() {
    const char* text = R"(schema: fluent.scene/v1alpha2
stage: { size: [8, 8] }
inputs:
  grade: { type: image.rgba8 }
layers:
  - content: { rect: { rect: [0, 0, 8, 8], color: [0.8, 0.2, 0.4] } }
    filters: [ { lut: { source: $inputs.grade, amount: 1 } } ]
)";
    const fs::ParseResult parsed = fs::parseScene(text);
    CHECK(parsed.ok());
    CHECK(parsed.doc.layers[0].filters[0].image_input == "grade");
    // The canonical spelling round-trips (digest covers the source ref).
    const std::string once = fs::format(parsed.doc);
    CHECK(once.find("source: $inputs.grade") != std::string::npos);
    const fs::ParseResult re = fs::parseScene(once);
    CHECK(re.ok());
    CHECK(fs::digest(re.doc) == fs::digest(parsed.doc));

    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;

    // Unfed grade: the filter is a pass-through, not garbage.
    CpuRenderer r;
    const Surface& plain = r.render(scene.stage(), 8, 8, 0.0f);
    const uint8_t* center = plain.row(4) + 4 * 4;
    CHECK(std::abs(center[0] - 204) <= 2 && std::abs(center[1] - 51) <= 2 &&
          std::abs(center[2] - 102) <= 2);

    // Fed with the channel-swap atlas: red and blue trade places. Trilinear
    // reconstruction of a linear map is exact up to 8-bit quantization.
    const auto atlas = makeSwapLutAtlas(2);
    CHECK(scene.setImage("grade", ImageView{8, 8, atlas.data(), 0}));
    const Surface& graded = r.render(scene.stage(), 8, 8, 0.0f);
    center = graded.row(4) + 4 * 4;
    CHECK(std::abs(center[0] - 102) <= 3 && std::abs(center[1] - 51) <= 3 &&
          std::abs(center[2] - 204) <= 3);

    // Rejections: a non-reference source, and an undeclared input.
    CHECK(hasCode(fs::parseScene("schema: fluent.scene/v1alpha2\nlayers:\n"
                                 "  - content: { rect: {} }\n"
                                 "    filters: [ { lut: { source: 5 } } ]\n")
                      .diagnostics,
                  "validate.reference"));
    const fs::ParseResult bad =
        fs::parseScene("schema: fluent.scene/v1alpha2\nlayers:\n"
                       "  - content: { rect: {} }\n"
                       "    filters: [ { lut: { source: $inputs.nope } } ]\n");
    CHECK(bad.ok());
    CHECK(hasCode(fs::compile(bad.doc).diagnostics, "compile.reference"));
}

// The layer mask through the document (Layer::mask's declarative face):
// an image input whose alpha multiplies the layer after its filters.
void testLayerMask() {
    const char* text = R"(schema: fluent.scene/v1alpha2
stage: { size: [8, 8] }
inputs:
  cut: { type: image.rgba8 }
layers:
  - content: { rect: { rect: [0, 0, 8, 8], color: [0.8, 0.2, 0.4] } }
    mask: { source: $inputs.cut }
)";
    const fs::ParseResult parsed = fs::parseScene(text);
    CHECK(parsed.ok());
    CHECK(parsed.doc.layers[0].mask.has_value());
    CHECK(parsed.doc.layers[0].mask->input == "cut");
    CHECK(!parsed.doc.layers[0].mask->invert);
    // Round-trips canonically; the defaults stay off the page.
    const std::string once = fs::format(parsed.doc);
    CHECK(once.find("mask: { source: $inputs.cut }") != std::string::npos);
    const fs::ParseResult re = fs::parseScene(once);
    CHECK(re.ok());
    CHECK(fs::digest(re.doc) == fs::digest(parsed.doc));

    fs::CompileResult compiled = fs::compile(parsed.doc);
    CHECK(compiled.ok());
    fs::CompiledScene& scene = *compiled.scene;

    // Unfed mask: the layer draws whole, like an unfed lut passes through.
    CpuRenderer r;
    const Surface& plain = r.render(scene.stage(), 8, 8, 0.0f);
    CHECK(plain.row(4)[6 * 4 + 3] > 250);

    // Left half opaque, right half transparent — the alpha channel is the
    // mask, so the rect survives on the left and vanishes on the right.
    std::vector<uint8_t> cut(8 * 8 * 4, 0);
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            cut[(y * 8 + x) * 4 + 3] = x < 4 ? 255 : 0;
        }
    }
    CHECK(scene.setImage("cut", ImageView{8, 8, cut.data(), 0}));
    const Surface& masked = r.render(scene.stage(), 8, 8, 0.0f);
    CHECK(masked.row(4)[1 * 4 + 3] > 250);   // kept
    CHECK(masked.row(4)[6 * 4 + 3] < 5);     // cut away

    // invert flips which side survives; feather round-trips as written.
    const fs::ParseResult inv = fs::parseScene(
        "schema: fluent.scene/v1alpha2\nstage: { size: [8, 8] }\ninputs:\n"
        "  cut: { type: image.rgba8 }\nlayers:\n"
        "  - content: { rect: { rect: [0, 0, 8, 8], color: [0.8, 0.2, 0.4] } }\n"
        "    mask: { source: $inputs.cut, invert: true, feather: 2 }\n");
    CHECK(inv.ok());
    CHECK(inv.doc.layers[0].mask->invert);
    CHECK(fs::format(inv.doc).find("invert: true, feather: 2") != std::string::npos);
    fs::CompileResult ic = fs::compile(inv.doc);
    CHECK(ic.ok());
    CHECK(ic.scene->setImage("cut", ImageView{8, 8, cut.data(), 0}));
    const Surface& flipped = r.render(ic.scene->stage(), 8, 8, 0.0f);
    CHECK(flipped.row(4)[1 * 4 + 3] < 5);    // now cut away
    CHECK(flipped.row(4)[6 * 4 + 3] > 250);  // now kept

    // Rejections: missing source, unknown key, non-reference, undeclared.
    CHECK(hasCode(fs::parseScene("schema: fluent.scene/v1alpha2\nlayers:\n"
                                 "  - content: { rect: {} }\n"
                                 "    mask: { feather: 2 }\n")
                      .diagnostics,
                  "validate.required"));
    CHECK(hasCode(fs::parseScene("schema: fluent.scene/v1alpha2\nlayers:\n"
                                 "  - content: { rect: {} }\n"
                                 "    mask: { source: $inputs.cut, soft: 2 }\n")
                      .diagnostics,
                  "validate.unknown_key"));
    CHECK(hasCode(fs::parseScene("schema: fluent.scene/v1alpha2\nlayers:\n"
                                 "  - content: { rect: {} }\n"
                                 "    mask: { source: mask.png }\n")
                      .diagnostics,
                  "validate.reference"));
    const fs::ParseResult bad =
        fs::parseScene("schema: fluent.scene/v1alpha2\nlayers:\n"
                       "  - content: { rect: {} }\n"
                       "    mask: { source: $inputs.nope }\n");
    CHECK(bad.ok());
    CHECK(hasCode(fs::compile(bad.doc).diagnostics, "compile.reference"));
}

void testDescribe() {
    const std::string json = fs::describeJson();
    CHECK(json.find("\"contents\"") != std::string::npos);
    CHECK(json.find("\"boxes\"") != std::string::npos);
    CHECK(json.find("\"attributes\"") != std::string::npos);
    CHECK(json.find("\"corner_radius\"") != std::string::npos);
    CHECK(json.find("\"filters\"") != std::string::npos);
    // Every filter in the table appears.
    for (const FilterSpec& f : filterTable()) {
        CHECK(json.find("\"" + std::string(f.name) + "\"") != std::string::npos);
    }
}

}  // namespace

int main() {
    testCanonicalSceneParses();
    testRejections();
    testDigestAndFmt();
    testTableDefaultsMatchCpp();
    testYamlCppParity();
    testParamsAnimateAndInputs();
    testParamBoundGeometry();
    testParamStyleBindings();
    testRuntimeLayers();
    testLinter();
    testInspector();
    testSceneUiControls();
    testBindingDoc();
    testLutFilter();
    testLayerMask();
    testDescribe();
    if (g_failures == 0) {
        std::printf("scene_tests: all passed\n");
        return 0;
    }
    std::fprintf(stderr, "scene_tests: %d failure(s)\n", g_failures);
    return 1;
}
