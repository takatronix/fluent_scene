// bench — frames-per-second of both backends on a representative robot HUD
// (camera image + detections + shadowed panel + filtered PiP) at 1080p, then
// the CPU renderer on long thin paths: a voice-meter waveform (one filled
// polygon + two polylines, 120 samples each, as Kepler's stage draws it) at
// 900×800, and a long diagonal polyline — the cases whose cost used to grow
// with bbox pixels × segments.
//
//   ./bench [frames]        (default 60; first frame excluded as warmup)

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

#include <fluent_scene/fluent_scene.hpp>
#ifdef FS_HAVE_VULKAN
#include <fluent_scene/vulkan_renderer.hpp>
#endif

using namespace fluent_scene;

namespace {

std::vector<uint8_t> makeCamera(uint32_t w, uint32_t h, uint32_t t) {
    std::vector<uint8_t> px(w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &px[(y * w + x) * 4];
            p[0] = static_cast<uint8_t>((x + t * 3) & 0xff);
            p[1] = static_cast<uint8_t>(80 + ((y + t) & 0x7f));
            p[2] = static_cast<uint8_t>(((x / 24) + (y / 24)) % 2 ? 190 : 70);
            p[3] = 255;
        }
    }
    return px;
}

double benchOne(const char* name, Renderer& renderer, int frames) {
    Stage stage(1920, 1080);
    auto cam = makeCamera(1280, 720, 0);
    ImageView view{1280, 720, cam.data(), 0};
    auto& video = stage.image(view, Fit::Cover);
    auto& boxes = stage.boxes({{{500, 300, 300, 420}, 0.9f, "asparagus", 0},
                               {{1100, 400, 260, 380}, 0.8f, "asparagus", 0}})
                      .color(Color::Teal)
                      .cornerRadius(8)
                      .smoothing(0.2f);
    stage.polyline({{200, 1000}, {700, 900}, {1300, 940}, {1800, 820}})
        .thickness(8)
        .dash(30)
        .color(Color::Orange);
    auto& hud = stage.group("hud").position(32, 32).opacity(0.92f).shadow();
    hud.rect({0, 0, 420, 120}).cornerRadius(14).color({0, 0, 0, 0.45f});
    hud.text("走行中 SPEED 1.2 m/s", {20, 40}).size(30);
    auto& pip = stage.image(view).frame({1560, 60, 320, 180}).cornerRadius(10).blur(3);

    double total_ms = 0;
    for (int i = 0; i < frames + 1; ++i) {
        cam = makeCamera(1280, 720, static_cast<uint32_t>(i));  // live video
        // ImageView is borrowed: every layer holding one must be repointed
        // when the backing frame changes.
        video.setImage({1280, 720, cam.data(), 0});
        pip.setImage({1280, 720, cam.data(), 0});
        boxes.setBoxes({{{500.0f + i, 300, 300, 420}, 0.9f, "asparagus", 0},
                        {{1100.0f - i, 400, 260, 380}, 0.8f, "asparagus", 0}});
        const auto t0 = std::chrono::steady_clock::now();
        renderer.render(stage, 1.0f / 30.0f);
        const auto t1 = std::chrono::steady_clock::now();
        if (i > 0) {  // frame 0 = warmup (pipelines are prebuilt; caches are not)
            total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
    }
    const double ms = total_ms / frames;
    std::printf("%-8s %7.2f ms/frame  (%5.1f fps)  1920x1080\n", name, ms, 1000.0 / ms);
    return ms;
}

// Times `frames` renders of `stage` (after one warmup) and prints a row.
double timeRenders(const char* name, Renderer& renderer, Stage& stage, int frames,
                   const std::function<void(int)>& update) {
    double total_ms = 0;
    for (int i = 0; i < frames + 1; ++i) {
        update(i);
        const auto t0 = std::chrono::steady_clock::now();
        renderer.render(stage, 1.0f / 30.0f);
        const auto t1 = std::chrono::steady_clock::now();
        if (i > 0) {
            total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
    }
    const double ms = total_ms / frames;
    std::printf("%-14s %7.2f ms/frame  (%6.1f fps)  %.0fx%.0f\n", name, ms, 1000.0 / ms,
                stage.width(), stage.height());
    return ms;
}

// The voice meter of Kepler's stage: 920×100 at the bottom of a 900×800-ish
// view, N = 120 samples, a translucent filled band between the upper and
// lower envelopes plus both edges stroked at 2.5.
void benchWave(Renderer& renderer, int frames) {
    constexpr int kN = 120;
    constexpr float kL = 40, kW = 820, kY = 700, kH = 90;
    Stage stage(900, 800);
    auto& fill = stage.polygon({}).color({0.47f, 0.59f, 1.0f, 0.22f});
    auto& up = stage.polyline({}).color({0.47f, 0.59f, 1.0f, 0.85f}).thickness(2.5f);
    auto& down = stage.polyline({}).color({0.47f, 0.59f, 1.0f, 0.85f}).thickness(2.5f);
    std::vector<Vec2> u(kN + 1), d(kN + 1), band;
    auto update = [&](int t) {
        for (int i = 0; i <= kN; ++i) {
            const float x = kL + kW * i / kN;
            const float a = 0.5f + 0.5f * std::sin(i * 0.37f + t * 0.2f) *
                                       std::sin(i * 0.05f + t * 0.07f);
            u[i] = {x, kY + kH * 0.5f - a * kH * 0.45f};
            d[i] = {x, kY + kH * 0.5f + a * kH * 0.45f};
        }
        band.assign(u.begin(), u.end());
        band.insert(band.end(), d.rbegin(), d.rend());
        fill.setPoints(band);
        up.setPoints(u);
        down.setPoints(d);
    };
    timeRenders("cpu wave", renderer, stage, frames, update);
    up.hidden(true);
    down.hidden(true);
    timeRenders("cpu wave fill", renderer, stage, frames, update);
    fill.hidden(true);
    up.hidden(false);
    down.hidden(false);
    timeRenders("cpu wave lines", renderer, stage, frames, update);
}

// One long diagonal path across 1080p: 240 short segments along a slanted
// sine, so the content bbox is nearly the whole frame.
void benchDiagonal(Renderer& renderer, int frames) {
    Stage stage(1920, 1080);
    auto& path = stage.polyline({}).thickness(3).color(Color::Orange);
    std::vector<Vec2> pts(241);
    timeRenders("cpu diagonal", renderer, stage, frames, [&](int t) {
        for (size_t i = 0; i < pts.size(); ++i) {
            const float k = static_cast<float>(i) / (pts.size() - 1);
            pts[i] = {40 + 1840 * k, 1040 - 1000 * k + 30 * std::sin(k * 40 + t * 0.1f)};
        }
        path.setPoints(pts);
    });
    path.hidden(true);  // the per-frame floor: clear + output conversion
    timeRenders("cpu empty", renderer, stage, frames, [](int) {});
}

}  // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 60;
    CpuRenderer cpu;
    const double cpu_ms = benchOne("cpu", cpu, std::max(frames / 10, 3));
    {
        CpuRenderer paths;
        benchWave(paths, std::max(frames / 4, 5));
        benchDiagonal(paths, std::max(frames / 4, 5));
    }
#ifdef FS_HAVE_VULKAN
    try {
        VulkanRenderer gpu;
        const double gpu_ms = benchOne("vulkan", gpu, frames);
        std::printf("speedup: %.1fx\n", cpu_ms / gpu_ms);
    } catch (const std::exception& e) {
        std::printf("vulkan unavailable: %s\n", e.what());
    }
#endif
    return 0;
}
