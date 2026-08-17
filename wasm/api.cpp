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

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fluent_scene/cpu_renderer.hpp>
#include <fluent_scene/effects.hpp>
#include <fluent_scene/fluent_scene.hpp>
#include <fluent_scene/fvs/compiler.hpp>
#include <fluent_scene/fvs/document.hpp>
#ifdef FS_HAVE_WEBGPU
#include <fluent_scene/webgpu_renderer.hpp>
#endif

using namespace fluent_scene;
namespace fvs = fluent_scene::fvs;

namespace {

std::string g_error;

#ifdef FS_HAVE_WEBGPU
std::unique_ptr<WebGPURenderer> g_webgpu;
#endif

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

/// The renderer behind the frame — "cpu", or "webgpu" once fs_webgpu_init
/// succeeded. Shown in HUDs so nobody has to wonder what is drawing.
EMSCRIPTEN_KEEPALIVE
const char* fs_backend() {
#ifdef FS_HAVE_WEBGPU
    if (g_webgpu && g_webgpu->status() == WebGPURenderer::Status::Ready) {
        return "webgpu";
    }
#endif
    return "cpu";
}

#ifdef FS_HAVE_WEBGPU

/// Starts the async WebGPU init presenting into `canvas_selector`
/// (e.g. "#view"). Poll fs_webgpu_status() from rAF until it leaves 0.
EMSCRIPTEN_KEEPALIVE
void fs_webgpu_init(const char* canvas_selector) {
    WebGPURenderer::Options opts;
    opts.canvas_selector = canvas_selector != nullptr ? canvas_selector : "";
    g_webgpu = std::make_unique<WebGPURenderer>(std::move(opts));
}

/// 0 = initializing, 1 = ready, -1 = error (fs_webgpu_error says why).
EMSCRIPTEN_KEEPALIVE
int fs_webgpu_status() {
    if (!g_webgpu) {
        return -1;
    }
    switch (g_webgpu->status()) {
        case WebGPURenderer::Status::Ready: return 1;
        case WebGPURenderer::Status::Error: return -1;
        default: return 0;
    }
}

EMSCRIPTEN_KEEPALIVE
const char* fs_webgpu_error() {
    static std::string out;
    out = g_webgpu ? g_webgpu->error() : "fs_webgpu_init not called";
    return out.c_str();
}

/// Renders one frame straight into the canvas — no readback, no
/// putImageData; this is the W3 hot path.
EMSCRIPTEN_KEEPALIVE
int fs_render_webgpu(FsInstance* inst, float dt, int out_w, int out_h) {
    if (!g_webgpu) {
        return 0;
    }
    if (inst->ripple) {
        inst->ripple->tick(dt);
    }
    return g_webgpu->renderToCanvas(inst->scene->stage(), static_cast<uint32_t>(out_w),
                                    static_cast<uint32_t>(out_h), dt)
               ? 1
               : 0;
}

/// Async pixel readback pair for CPU-vs-GPU comparison: begin renders the
/// frame offscreen and starts the map; pixels returns the tight RGBA8
/// buffer once it lands (nullptr while pending).
EMSCRIPTEN_KEEPALIVE
int fs_webgpu_read_begin(FsInstance* inst, float dt, int out_w, int out_h) {
    if (!g_webgpu) {
        return 0;
    }
    return g_webgpu->readbackBegin(inst->scene->stage(), static_cast<uint32_t>(out_w),
                                   static_cast<uint32_t>(out_h), dt)
               ? 1
               : 0;
}

/// Async readback of the frame fs_render_webgpu last drew — no second
/// scene render. Completion comes home through fs_webgpu_read_pixels,
/// same as fs_webgpu_read_begin.
EMSCRIPTEN_KEEPALIVE
int fs_webgpu_read_last(int out_w, int out_h) {
    if (!g_webgpu) {
        return 0;
    }
    return g_webgpu->readbackLastFrame(static_cast<uint32_t>(out_w),
                                       static_cast<uint32_t>(out_h))
               ? 1
               : 0;
}

EMSCRIPTEN_KEEPALIVE
const uint8_t* fs_webgpu_read_pixels() {
    return g_webgpu ? g_webgpu->readbackPixels() : nullptr;
}

#endif  // FS_HAVE_WEBGPU

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

/// Live filter tweaking by public name: updates the parameter slots of the
/// layer's first filter of that kind, or appends one when the chain has
/// none. All five slots are given in filters_def.h order (pass the defaults
/// for slots the filter does not use); a filter's image parameter and the
/// rest of the chain are left untouched, so a slider driving
/// `beauty.smoothing` composes with a `lut` grade on the same layer.
/// Filter parameters are compile-time literals in Scene YAML — this is the
/// runtime path.
EMSCRIPTEN_KEEPALIVE
int fs_layer_set_filter(FsInstance* inst, const char* layer_id, const char* filter_name,
                        float p0, float p1, float p2, float p3, float p4) {
    Layer* target = inst->scene->stage().find(layer_id);
    if (target == nullptr) {
        return 0;
    }
    for (const FilterSpec& spec : filterTable()) {
        if (spec.name == std::string(filter_name)) {
            const float values[5] = {p0, p1, p2, p3, p4};
            std::vector<Filter> chain = target->filters();
            bool replaced = false;
            for (Filter& f : chain) {
                if (f.mode == spec.mode && !replaced) {
                    std::copy(values, values + 5, f.values);
                    replaced = true;
                }
            }
            if (!replaced) {
                Filter f;
                f.mode = spec.mode;
                std::copy(values, values + 5, f.values);
                chain.push_back(f);
            }
            target->clearFilters();
            for (const Filter& f : chain) {
                target->filter(f);
            }
            return 1;
        }
    }
    return 0;
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

/// Clears the layer's filter chain (fs_layer_set_filter appends/replaces;
/// this is the only way back to "no filter").
EMSCRIPTEN_KEEPALIVE
int fs_layer_clear_filters(FsInstance* inst, const char* layer_id) {
    Layer* target = inst->scene->stage().find(layer_id);
    if (target == nullptr) {
        return 0;
    }
    target->clearFilters();
    return 1;
}

/// The filter catalog as JSON — the same single-source filterTable() that
/// feeds Scene parsing, so a page can build its UI without a hand-kept copy.
/// [{"name":..,"mode":..,"summary":..,"params":[{"name":..,"def":..,
///   "unit":"scalar"|"length"|"coord_x"|"coord_y"}]}]
EMSCRIPTEN_KEEPALIVE
const char* fs_filters_json() {
    static std::string json;
    if (json.empty()) {
        auto esc = [](const char* s) {
            std::string o;
            for (; *s; ++s) {
                if (*s == '"' || *s == '\\') o += '\\';
                o += *s;
            }
            return o;
        };
        json = "[";
        bool first = true;
        for (const FilterSpec& spec : filterTable()) {
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"" + esc(spec.name) + "\",\"mode\":" +
                    std::to_string(spec.mode) + ",\"summary\":\"" +
                    esc(spec.summary) + "\",\"params\":[";
            bool pfirst = true;
            for (const FilterParamSpec& p : spec.params) {
                if (!pfirst) json += ",";
                pfirst = false;
                const char* unit = p.unit == FilterUnit::Length   ? "length"
                                   : p.unit == FilterUnit::CoordX ? "coord_x"
                                   : p.unit == FilterUnit::CoordY ? "coord_y"
                                                                  : "scalar";
                json += "{\"name\":\"" + esc(p.name) + "\",\"def\":" +
                        std::to_string(p.default_value) + ",\"unit\":\"" +
                        unit + "\"}";
            }
            json += "]}";
        }
        json += "]";
    }
    return json.c_str();
}

}  // extern "C"
