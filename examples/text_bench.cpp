// text_bench — frame time with 2,000 glyphs on screen (docs/design/text.md
// §8). 40 text layers × 50 characters (Japanese + Latin + digits) on a
// 1920×1080 stage, plus 10 live numbers whose text changes every frame.
//   ./text_bench [--renderer=vulkan] [--frames=N]
// Prints cold (first) frame, steady frames, and the live-number frames.
// Uses only the long-standing API so it builds against older versions too.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fluent_scene/fluent_scene.hpp>
#ifdef FS_HAVE_VULKAN
#include <fluent_scene/vulkan_renderer.hpp>
#endif

using namespace fluent_scene;

int main(int argc, char** argv) {
    bool vulkan = false;
    int frames = 100;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--renderer=vulkan") == 0) {
            vulkan = true;
        } else if (std::strncmp(argv[i], "--frames=", 9) == 0) {
            frames = std::atoi(argv[i] + 9);
        }
    }
    std::unique_ptr<Renderer> r;
#ifdef FS_HAVE_VULKAN
    if (vulkan) {
        r = std::make_unique<VulkanRenderer>();
    }
#endif
    if (!r) {
        vulkan = false;
        r = std::make_unique<CpuRenderer>();
    }
    Stage stage(1920, 1080);
    stage.rect({0, 0, 1920, 1080}).color({0.06f, 0.07f, 0.09f, 1});
    const int kLayers = 40;
    const char* kLine = "車速と回転数の表示 Speed 0123456789 漢字かなカナ ABCxyz!?";  // 50 chars
    for (int i = 0; i < kLayers; ++i) {
        stage.text(kLine, {20.0f + (i % 2) * 960, 20.0f + (i / 2) * 52}).size(20 + (i % 4) * 4);
    }
    std::vector<Layer*> live;
    for (int i = 0; i < 10; ++i) {
        live.push_back(&stage.text("0", {1700, 60.0f + i * 100}).size(64).align(Align::Right));
    }
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    // Baseline: the same frame without text (readback, clear, background).
    Stage empty(1920, 1080);
    empty.rect({0, 0, 1920, 1080}).color({0.06f, 0.07f, 0.09f, 1});
    r->render(empty, 0);
    double base = 0;
    for (int f = 0; f < frames; ++f) {
        const auto a = clock::now();
        r->render(empty, 1.0f / 60);
        base += ms(a, clock::now());
    }
    base /= frames;
    const auto t0 = clock::now();
    r->render(stage, 0);
    const double cold = ms(t0, clock::now());
    double steady = 0;
    for (int f = 0; f < frames; ++f) {
        const auto a = clock::now();
        r->render(stage, 1.0f / 60);
        steady += ms(a, clock::now());
    }
    double changing = 0;
    for (int f = 0; f < frames; ++f) {
        char buf[16];
        for (size_t i = 0; i < live.size(); ++i) {
            std::snprintf(buf, sizeof buf, "%.1f", (f * 7 + i * 13) % 2000 / 10.0);
            live[i]->setText(buf);
        }
        const auto a = clock::now();
        r->render(stage, 1.0f / 60);
        changing += ms(a, clock::now());
    }
    std::printf("%s: 2,000 glyphs + 10 live numbers @1920x1080 — empty frame %.2f ms; with text: "
                "cold %.1f ms, steady %.2f ms (text +%.2f), numbers changing %.2f ms (text +%.2f)\n",
                vulkan ? "vulkan" : "cpu", base, cold, steady / frames, steady / frames - base,
                changing / frames, changing / frames - base);
    return 0;
}
