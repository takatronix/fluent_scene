#pragma once

/// \file webgpu_renderer.hpp
/// \brief WebGPURenderer — the browser GPU backend (Phase W3).
///
/// The same picture as every other backend: it walks the tree with the
/// shared plan (src/render_shared.hpp) and runs the very same shape/filter
/// bodies (shared/*.h) — compiled GLSL → SPIR-V → naga → WGSL by
/// wasm/build.sh --webgpu, never hand-ported. What differs from
/// VulkanRenderer is only the browser's reality:
///
///  - **Async device.** `navigator.gpu` hands out adapter and device via
///    promises, so construction starts the request and returns; poll
///    `status()` from the page's rAF loop until Ready (or Error).
///  - **No blocking readback.** The primary path presents straight into a
///    canvas (`renderToCanvas`) — zero copies, zero read-backs, the whole
///    reason this backend exists. For pixel comparison there is an
///    explicit async pair: `readbackBegin` renders offscreen and starts
///    the map, `readbackPixels` returns the pixels once the map lands.
///
/// Push constants don't exist in browser WGSL; the shared 128-byte block
/// becomes a dynamic-offset uniform buffer (see push_common.glsl).

#include <cstdint>
#include <memory>
#include <string>

namespace fluent_scene {

class Stage;

class WebGPURenderer {
public:
    struct Options {
        /// CSS selector of the canvas to present into (e.g. "#view").
        /// Empty = no primary canvas: offscreen readback still works and
        /// addCanvas can attach canvases later.
        std::string canvas_selector;
        /// Font file for text content; empty = probe well-known locations.
        std::string font_file;
        uint32_t font_pixel_size = 26;
    };

    enum class Status { Initializing, Ready, Error };

    /// Starts the async adapter/device request and returns immediately.
    explicit WebGPURenderer(Options options);
    ~WebGPURenderer();
    WebGPURenderer(const WebGPURenderer&) = delete;
    WebGPURenderer& operator=(const WebGPURenderer&) = delete;

    Status status() const;
    /// Human-readable reason when status() == Error.
    const std::string& error() const;

    /// Renders one frame into a canvas at out_w x out_h (advancing
    /// animations by dt). Requires status() == Ready. `target` 0 is the
    /// Options::canvas_selector canvas; any other value is an id from
    /// addCanvas. Returns false when that canvas cannot present (stale
    /// id, detached or zero-sized canvas) — the device and the other
    /// targets are unaffected, so status() stays Ready.
    bool renderToCanvas(Stage& stage, uint32_t out_w, uint32_t out_h, float dt,
                        int target = 0);

    /// Adds another canvas to present into — one device, many views. May
    /// be called as soon as the renderer exists (before Ready). Returns
    /// the target id for renderToCanvas, or 0 when the canvas has no
    /// WebGPU context (already claimed by 2d/WebGL, or not in the DOM).
    int addCanvas(const std::string& canvas_selector);
    /// Releases a canvas from addCanvas; target 0 is not removable.
    void removeCanvas(int target);

    /// Renders offscreen and starts the async pixel readback. One
    /// readback may be in flight at a time.
    bool readbackBegin(Stage& stage, uint32_t out_w, uint32_t out_h, float dt);
    /// Starts the async readback of the frame renderToCanvas last
    /// recorded, rescaled to out_w x out_h — no second scene render, so
    /// per-frame feedback loops don't double the GPU load. Requires at
    /// least one renderToCanvas this session.
    bool readbackLastFrame(uint32_t out_w, uint32_t out_h);
    /// The finished readback's straight-RGBA8 pixels (tightly packed,
    /// valid until the next readbackBegin), or nullptr while pending.
    const uint8_t* readbackPixels() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fluent_scene
