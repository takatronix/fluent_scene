#pragma once

// Internal: the process-wide font registry (docs/design/text.md §2).
//
// Families map to faces (weight × italic). Each face keeps its bytes in
// memory once and opens them twice: a FreeType face for outlines and a
// HarfBuzz font (hb-ot, font units, unhinted) for shaping — so advances are
// size-independent and the layout shapes a string once for every size.
//
// Thread-safety: every entry point takes the library mutex; FT faces are
// only touched under it (rasterization happens through withFace()).

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>  // std::recursive_mutex
#include <string>
#include <vector>

namespace fluent_scene {
namespace detail {

struct FontFace {
    uint16_t id = 0;
    std::string family;
    int weight = 400;
    bool italic = false;
    std::string source;          // file path or "<memory>"
    std::vector<uint8_t> bytes;  // in-memory fonts only; files are mapped
    bool opened = false;         // defaults open lazily, on first resolve
    bool broken = false;
    void* ft_face = nullptr;     // FT_Face
    void* hb_font = nullptr;     // hb_font_t*, scale = units_per_em
    float upem = 1000;
    // Metrics in em (font units / upem), y up.
    float ascender = 0.8f;
    float descender = 0.2f;      // positive, below the baseline
    float line_gap = 0;
    float max_digit_advance = 0;  // em; tabular fallback cell
    float replacement_advance = 0.5f;
};

class FontLibrary {
public:
    static FontLibrary& instance();

    // Registers a face; false when it cannot be parsed. A re-registration
    // of the same family/weight/italic replaces the previous face for new
    // lookups (old ids stay valid).
    bool add(const std::string& family, std::vector<uint8_t> bytes, int weight, bool italic,
             std::string& error);
    // `lazy` registers without opening (the file must exist); the defaults
    // use it so a process that never asks for "serif" never maps it.
    bool addFile(const std::string& family, const std::string& path, int weight, bool italic,
                 std::string& error, bool lazy = false);

    // Loads the defaults once: "sans" (Noto Sans CJK JP Regular / Bold from
    // the system or the wasm virtual FS), "serif", "mono" when present.
    // `override_file` (renderer Options::font_file) becomes "sans" 400.
    void ensureDefaults(const std::string& override_file = std::string());

    // Closest face for a request; nullptr when no font at all is available.
    const FontFace* resolve(const std::string& family, int weight, bool italic);
    // Default-family face nearest to `weight`/`italic` (fallback chain).
    const FontFace* fallback(int weight, bool italic);
    const FontFace* face(uint16_t id) const;

    // Glyph id of `codepoint` in `face` (0 when absent).
    uint32_t glyphIndex(const FontFace& face, uint32_t codepoint);

    // Glyph ink box in em (x0, y0 bottom, x1, y1 top; y up). False if empty.
    bool glyphExtents(const FontFace& face, uint32_t glyph, float box[4]);

    // Runs `fn` with the library mutex held (FreeType is not thread-safe).
    void withLock(const std::function<void()>& fn);
    std::recursive_mutex& mutex() { return mutex_; }
    void* ftLibrary() const { return ft_library_; }  // FT_Library

    std::vector<std::string> families() const;
    bool hasAnyFace() const { return !faces_.empty(); }

private:
    FontLibrary();
    ~FontLibrary();
    bool openFace(FontFace& face, std::string& error);
    bool ensureOpen(FontFace& face);

    mutable std::recursive_mutex mutex_;
    void* ft_library_ = nullptr;
    std::vector<std::unique_ptr<FontFace>> faces_;  // index = id
    bool defaults_loaded_ = false;
    std::string default_override_;
};

}  // namespace detail
}  // namespace fluent_scene
