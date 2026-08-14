// wasm/api.cpp — the browser face of fluent_scene (Phase W1).
//
// The same library that runs on the robot compiles with emscripten and
// renders into a canvas: a Scene document goes in as YAML text, camera
// frames go in as RGBA buffers, pointers go in as three calls, and each
// render returns a borrowed RGBA view the page paints with putImageData.
// No server, no round trip — touch feedback is local.
//
// The C surface is deliberately flat (numbers, strings, buffers) so any
// JS wrapper — or any other wasm host — can drive it without bindings
// machinery. One FsInstance owns one CompiledScene + CpuRenderer.

#include <emscripten.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fluent_scene/cpu_renderer.hpp>
#include <fluent_scene/effects.hpp>
#include <fluent_scene/fluent_scene.hpp>
#include <fluent_scene/fvs/compiler.hpp>
#include <fluent_scene/fvs/document.hpp>

using namespace fluent_scene;
namespace fvs = fluent_scene::fvs;

namespace {

std::string g_error;

struct FsInstance {
    std::unique_ptr<fvs::CompiledScene> scene;
    CpuRenderer renderer;
    // Borrowed-view contract: the pixels behind setImage must outlive the
    // render, so each input keeps its own persistent buffer.
    std::map<std::string, std::vector<uint8_t>> images;
    std::unique_ptr<fx::Ripple> ripple;
    std::string events_json;  // UI events since the last drain
    int last_w = 0, last_h = 0, last_stride = 0;
};

}  // namespace

extern "C" {

/// Compiles a Scene document. Returns nullptr on rejection (fs_error()
/// carries every diagnostic, one per line).
EMSCRIPTEN_KEEPALIVE
FsInstance* fs_create(const char* yaml_utf8) {
    g_error.clear();
    fvs::ParseResult parsed = fvs::parseScene(yaml_utf8 != nullptr ? yaml_utf8 : "");
    if (!parsed.ok()) {
        for (const auto& d : parsed.diagnostics.items()) {
            g_error += "line " + std::to_string(d.span.begin_line) + ": [" + d.code + "] " +
                       d.message + "\n";
        }
        return nullptr;
    }
    fvs::CompileResult compiled = fvs::compile(parsed.doc);
    if (!compiled.ok()) {
        for (const auto& d : compiled.diagnostics.items()) {
            g_error += "[" + d.code + "] " + d.message + "\n";
        }
        return nullptr;
    }
    auto* inst = new FsInstance();
    inst->scene = std::move(compiled.scene);
    FsInstance* raw = inst;
    inst->scene->onUiEvent([raw](const fvs::UiEvent& e) {
        raw->events_json += std::string("{\"id\": \"") + e.id + "\", \"control\": \"" +
                            e.control + "\", \"value\": " + std::to_string(e.value) + "}\n";
    });
    return inst;
}

/// The last rejection's diagnostics (empty when fs_create succeeded).
EMSCRIPTEN_KEEPALIVE
const char* fs_error() { return g_error.c_str(); }

/// The renderer behind fs_render — "cpu" today; "webgpu" when W3 lands.
/// Shown in HUDs so nobody has to wonder what is drawing.
EMSCRIPTEN_KEEPALIVE
const char* fs_backend() { return "cpu"; }

/// The document's canonical digest.
EMSCRIPTEN_KEEPALIVE
const char* fs_digest(FsInstance* inst) { return inst->scene->digest().c_str(); }

/// Logical canvas size.
EMSCRIPTEN_KEEPALIVE
float fs_stage_width(FsInstance* inst) { return inst->scene->stage().width(); }
EMSCRIPTEN_KEEPALIVE
float fs_stage_height(FsInstance* inst) { return inst->scene->stage().height(); }

/// Allocates (or resizes) the RGBA buffer for a declared image input and
/// returns it; JS writes pixels there, then calls fs_commit_image.
EMSCRIPTEN_KEEPALIVE
uint8_t* fs_image_buffer(FsInstance* inst, const char* input, int w, int h) {
    auto& buf = inst->images[input];
    buf.resize(static_cast<size_t>(w) * h * 4);
    return buf.data();
}

/// Feeds the buffer from fs_image_buffer into the scene.
EMSCRIPTEN_KEEPALIVE
int fs_commit_image(FsInstance* inst, const char* input, int w, int h) {
    auto it = inst->images.find(input);
    if (it == inst->images.end()) {
        return 0;
    }
    return inst->scene->setImage(input, {static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                         it->second.data(), 0})
               ? 1
               : 0;
}

/// Pointer injection in logical stage coordinates (phase 0 down, 1 move,
/// 2 up). Feeds an attached ripple too.
EMSCRIPTEN_KEEPALIVE
void fs_pointer(FsInstance* inst, int phase, float x, float y) {
    Stage& stage = inst->scene->stage();
    if (phase == 0) {
        stage.pointerDown({x, y});
        if (inst->ripple) {
            inst->ripple->splash({x, y});
        }
    } else if (phase == 1) {
        stage.pointerMove({x, y});
        if (inst->ripple) {
            inst->ripple->pointerMoved({x, y});
        }
    } else {
        stage.pointerUp({x, y});
    }
}

/// Attaches the refraction ripple to a layer (hover = wake, tap = splash).
EMSCRIPTEN_KEEPALIVE
int fs_attach_ripple(FsInstance* inst, const char* layer_id, int max_waves) {
    Layer* target = inst->scene->stage().find(layer_id);
    if (target == nullptr) {
        return 0;
    }
    fx::RippleStyle style;
    if (max_waves > 0) {
        // Each wave is one full-surface filter pass — on the CPU renderer
        // that is the frame budget, so hosts cap it to their machine.
        style.max_waves = static_cast<uint32_t>(max_waves);
    }
    inst->ripple = std::make_unique<fx::Ripple>(*target, style);
    return 1;
}

/// Runtime params (declared in the document).
EMSCRIPTEN_KEEPALIVE
int fs_set_param_f32(FsInstance* inst, const char* name, float v) {
    return inst->scene->setParam(name, v) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE
int fs_set_param_bool(FsInstance* inst, const char* name, int v) {
    return inst->scene->setParam(name, v != 0) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE
int fs_set_param_vec2(FsInstance* inst, const char* name, float x, float y) {
    return inst->scene->setParam(name, Vec2{x, y}) ? 1 : 0;
}

/// Renders one frame at out_w x out_h (0 = logical size) after advancing
/// animations by dt seconds. Returns the RGBA pixels (borrowed until the
/// next render).
EMSCRIPTEN_KEEPALIVE
const uint8_t* fs_render(FsInstance* inst, float dt, int out_w, int out_h) {
    if (inst->ripple) {
        inst->ripple->tick(dt);
    }
    const Surface& s = (out_w > 0 && out_h > 0)
                           ? inst->renderer.render(inst->scene->stage(),
                                                   static_cast<uint32_t>(out_w),
                                                   static_cast<uint32_t>(out_h), dt)
                           : inst->renderer.render(inst->scene->stage(), dt);
    inst->last_w = static_cast<int>(s.width);
    inst->last_h = static_cast<int>(s.height);
    inst->last_stride = static_cast<int>(s.strideBytes);
    return s.pixels;
}

/// The rendered surface geometry of the last fs_render.
EMSCRIPTEN_KEEPALIVE
int fs_render_width(FsInstance* inst) { return inst->last_w; }
EMSCRIPTEN_KEEPALIVE
int fs_render_height(FsInstance* inst) { return inst->last_h; }
EMSCRIPTEN_KEEPALIVE
int fs_render_stride(FsInstance* inst) { return inst->last_stride; }

/// UI events since the last call (one JSON object per line), then clears.
EMSCRIPTEN_KEEPALIVE
const char* fs_drain_events(FsInstance* inst) {
    static std::string out;
    out = std::move(inst->events_json);
    inst->events_json.clear();
    return out.c_str();
}

EMSCRIPTEN_KEEPALIVE
void fs_destroy(FsInstance* inst) { delete inst; }

}  // extern "C"
