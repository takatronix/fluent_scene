#pragma once

/// \file webgl_renderer.hpp
/// \brief WebGLRenderer — the browser GPU backend for browsers without a
/// WebGPU adapter.
///
/// WebGPURenderer's twin on WebGL2: the same shared plan
/// (src/render_shared.hpp), the same draws in the same order, and the very
/// same shape/filter bodies (shared/*.h) — compiled GLSL → SPIR-V → naga →
/// GLSL ES 3.00 by wasm/build.sh --webgl, never hand-ported. It exists for
/// browsers that expose `navigator.gpu` but hand out no adapter (Chromium on
/// Linux without Vulkan — a car's browser, say) while WebGL2 runs on the
/// GPU. What differs from WebGPURenderer is only WebGL's reality:
///
///  - **Synchronous.** The context comes up in the constructor; status() is
///    Ready or Error right away (Error: no WebGL2, no float render targets).
///  - **Synchronous readback.** readPixels() renders offscreen and returns
///    the pixels at once (glReadPixels) — for tests and self-checks only.
///  - **No storage buffers.** Polygon points ride an RG32F texture.
///
/// Requires EXT_color_buffer_float (half/float render targets); every
/// WebGL2 implementation on a real GPU has it.

#include <cstdint>
#include <memory>
#include <string>

namespace fluent_scene {

class Stage;

class WebGLRenderer {
public:
    struct Options {
        /// CSS selector of the canvas the WebGL2 context lives on (e.g.
        /// "#view"). Required: WebGL has no context without a canvas.
        std::string canvas_selector;
        /// Font file for text content; empty = probe well-known locations.
        std::string font_file;
    };

    enum class Status { Ready, Error };

    /// Creates the WebGL2 context on the canvas; check status() after.
    explicit WebGLRenderer(Options options);
    ~WebGLRenderer();
    WebGLRenderer(const WebGLRenderer&) = delete;
    WebGLRenderer& operator=(const WebGLRenderer&) = delete;

    Status status() const;
    /// Human-readable reason when status() == Error (also: context lost).
    const std::string& error() const;
    /// The GL renderer string (unmasked when the browser allows it) —
    /// tells a hardware GPU from a software rasterizer (SwiftShader).
    const std::string& rendererInfo() const;

    /// Renders one frame into the canvas at out_w x out_h (resizing the
    /// canvas's drawing buffer if needed), advancing animations by dt.
    bool renderToCanvas(Stage& stage, uint32_t out_w, uint32_t out_h, float dt);

    /// Renders offscreen and reads back straight-RGBA8 pixels (tightly
    /// packed, valid until the next call), or nullptr on failure.
    const uint8_t* readPixels(Stage& stage, uint32_t out_w, uint32_t out_h, float dt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fluent_scene
