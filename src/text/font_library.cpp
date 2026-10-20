#include "text/font_library.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb-ot.h>
#include <hb.h>

#include "fluent_scene/text.hpp"

namespace fluent_scene {
namespace detail {

namespace {

FT_Library ftLib(void* p) { return static_cast<FT_Library>(p); }
FT_Face ftFace(void* p) { return static_cast<FT_Face>(p); }
hb_font_t* hbFont(void* p) { return static_cast<hb_font_t*>(p); }

bool fileReadable(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

FT_Error openFt(FT_Library lib, const FontFace& face, FT_Long index, FT_Face* out) {
    if (!face.bytes.empty()) {
        return FT_New_Memory_Face(lib, face.bytes.data(), static_cast<FT_Long>(face.bytes.size()),
                                  index, out);
    }
    return FT_New_Face(lib, face.source.c_str(), index, out);
}

// Face of a collection to use: Noto CJK collections carry JP/KR/SC/TC/HK
// faces (and their Mono cuts); prefer the Japanese one matching `mono`.
FT_Long pickCollectionFace(FT_Library lib, const FontFace& face, bool mono) {
    FT_Face probe = nullptr;
    if (openFt(lib, face, -1, &probe) != 0) {
        return 0;
    }
    const FT_Long count = probe->num_faces;
    FT_Done_Face(probe);
    if (count <= 1) {
        return 0;
    }
    FT_Long best = 0;
    int best_score = -1;
    for (FT_Long i = 0; i < count; ++i) {
        FT_Face f = nullptr;
        if (openFt(lib, face, i, &f) != 0) {
            continue;
        }
        const std::string name = f->family_name != nullptr ? f->family_name : "";
        FT_Done_Face(f);
        const bool is_mono = name.find("Mono") != std::string::npos;
        int score = 0;
        if (name.find("JP") != std::string::npos) {
            score += 2;
        }
        if (is_mono == mono) {
            score += 1;
        }
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

}  // namespace

FontLibrary& FontLibrary::instance() {
    static FontLibrary* lib = new FontLibrary();  // never destroyed: renderers
    return *lib;                                  // may outlive static dtors
}

FontLibrary::FontLibrary() {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) == 0) {
        ft_library_ = library;
    }
}

FontLibrary::~FontLibrary() {
    for (auto& f : faces_) {
        if (f->hb_font != nullptr) {
            hb_font_destroy(hbFont(f->hb_font));
        }
        if (f->ft_face != nullptr) {
            FT_Done_Face(ftFace(f->ft_face));
        }
    }
    if (ft_library_ != nullptr) {
        FT_Done_FreeType(ftLib(ft_library_));
    }
}

bool FontLibrary::openFace(FontFace& face, std::string& error) {
    if (ft_library_ == nullptr) {
        error = "FreeType initialization failed";
        return false;
    }
    const bool mono = face.family == "mono";
    const FT_Long index = pickCollectionFace(ftLib(ft_library_), face, mono);
    FT_Face ft = nullptr;
    if (openFt(ftLib(ft_library_), face, index, &ft) != 0) {
        error = "cannot parse font " + face.source;
        return false;
    }
    if (!FT_IS_SCALABLE(ft)) {
        FT_Done_Face(ft);
        error = "font is not scalable: " + face.source;
        return false;
    }
    hb_blob_t* blob = face.bytes.empty()
                          ? hb_blob_create_from_file(face.source.c_str())
                          : hb_blob_create(reinterpret_cast<const char*>(face.bytes.data()),
                                           static_cast<unsigned>(face.bytes.size()),
                                           HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* hface = hb_face_create(blob, static_cast<unsigned>(index));
    hb_blob_destroy(blob);
    hb_font_t* hfont = hb_font_create(hface);
    hb_face_destroy(hface);
    const unsigned upem = std::max(1u, hb_face_get_upem(hb_font_get_face(hfont)));
    hb_font_set_scale(hfont, static_cast<int>(upem), static_cast<int>(upem));
    hb_ot_font_set_funcs(hfont);
    hb_font_make_immutable(hfont);

    face.ft_face = ft;
    face.hb_font = hfont;
    face.upem = static_cast<float>(upem);
    // hhea metrics (what FreeType reports as ascender/descender/height for
    // OpenType); OS/2 typo metrics would differ for some fonts, but these
    // are what every previous fluent_scene version laid lines out with.
    face.ascender = static_cast<float>(ft->ascender) / face.upem;
    face.descender = -static_cast<float>(ft->descender) / face.upem;
    face.line_gap = std::max(0.0f, static_cast<float>(ft->height) / face.upem -
                                       face.ascender - face.descender);
    hb_position_t max_digit = 0;
    for (uint32_t cp = '0'; cp <= '9'; ++cp) {
        hb_codepoint_t gid = 0;
        if (hb_font_get_nominal_glyph(hfont, cp, &gid)) {
            max_digit = std::max(max_digit, hb_font_get_glyph_h_advance(hfont, gid));
        }
    }
    face.max_digit_advance = static_cast<float>(max_digit) / face.upem;
    face.replacement_advance = 0.75f;
    face.opened = true;
    return true;
}

bool FontLibrary::ensureOpen(FontFace& face) {
    if (face.opened || face.broken) {
        return face.opened;
    }
    std::string error;
    if (!openFace(face, error)) {
        face.broken = true;
        std::fprintf(stderr, "fluent_scene: %s\n", error.c_str());
        return false;
    }
    return true;
}

bool FontLibrary::add(const std::string& family, std::vector<uint8_t> bytes, int weight,
                      bool italic, std::string& error) {
    if (bytes.empty()) {
        error = "empty font data";
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (faces_.size() >= 0xffff) {
        error = "too many fonts";
        return false;
    }
    auto face = std::make_unique<FontFace>();
    face->id = static_cast<uint16_t>(faces_.size());
    face->family = family.empty() ? "sans" : family;
    face->weight = std::clamp(weight, 1, 1000);
    face->italic = italic;
    face->source = "<memory>";
    face->bytes = std::move(bytes);
    if (!openFace(*face, error)) {
        return false;
    }
    faces_.push_back(std::move(face));
    return true;
}

bool FontLibrary::addFile(const std::string& family, const std::string& path, int weight,
                          bool italic, std::string& error, bool lazy) {
    if (!fileReadable(path)) {
        error = "cannot open font file " + path;
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (faces_.size() >= 0xffff) {
        error = "too many fonts";
        return false;
    }
    auto face = std::make_unique<FontFace>();
    face->id = static_cast<uint16_t>(faces_.size());
    face->family = family.empty() ? "sans" : family;
    face->weight = std::clamp(weight, 1, 1000);
    face->italic = italic;
    face->source = path;
    if (!lazy && !openFace(*face, error)) {
        return false;
    }
    faces_.push_back(std::move(face));
    return true;
}

void FontLibrary::ensureDefaults(const std::string& override_file) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!override_file.empty() && override_file != default_override_) {
        default_override_ = override_file;
        std::string error;
        if (!addFile("sans", override_file, 400, false, error)) {
            std::fprintf(stderr, "fluent_scene: %s — falling back to system fonts\n",
                         error.c_str());
        }
    }
    if (defaults_loaded_) {
        return;
    }
    defaults_loaded_ = true;
    struct Candidate {
        const char* family;
        int weight;
        const char* paths[5];
    };
    // First readable path per (family, weight) wins. The wasm build embeds
    // its subsets at the NotoSansCJKjp-*.otf paths (wasm/build.sh).
    static const Candidate kDefaults[] = {
        {"sans", 400,
         {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
          "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
          "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",
          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", nullptr}},
        {"sans", 700,
         {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
          "/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc",
          "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Bold.otf",
          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", nullptr}},
        {"serif", 400,
         {"/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
          "/usr/share/fonts/truetype/noto/NotoSerifCJK-Regular.ttc",
          "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", nullptr, nullptr}},
        {"serif", 700,
         {"/usr/share/fonts/opentype/noto/NotoSerifCJK-Bold.ttc",
          "/usr/share/fonts/truetype/noto/NotoSerifCJK-Bold.ttc",
          "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf", nullptr, nullptr}},
        {"mono", 400,
         {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
          "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
          "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", nullptr, nullptr}},
        {"mono", 700,
         {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
          "/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc",
          "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", nullptr, nullptr}},
    };
    for (const Candidate& c : kDefaults) {
        if (std::string(c.family) == "sans" && c.weight == 400 && !default_override_.empty() &&
            resolve("sans", 400, false) != nullptr) {
            continue;  // the renderer's font_file already is "sans" Regular
        }
        for (const char* path : c.paths) {
            if (path == nullptr) {
                break;
            }
            std::string error;
            if (addFile(c.family, path, c.weight, false, error, /*lazy=*/true)) {
                break;
            }
        }
    }
    if (resolve("sans", 400, false) == nullptr) {
        std::fprintf(stderr,
                     "fluent_scene: no usable font found — text will not render "
                     "(set Options::font_file or registerFont)\n");
    }
}

const FontFace* FontLibrary::resolve(const std::string& family, int weight, bool italic) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string want = family.empty() ? "sans" : family;
    const FontFace* best = nullptr;
    int best_score = 0;
    // Later registrations win ties (re-registering replaces).
    for (const auto& f : faces_) {
        if (f->family != want || f->broken) {
            continue;
        }
        int score = 100000 - std::abs(f->weight - weight) * 10;
        // CSS-ish: for bold requests prefer heavier, for light lighter.
        if ((weight >= 500 && f->weight < weight) || (weight < 400 && f->weight > weight)) {
            score -= 5;
        }
        if (f->italic != italic) {
            score -= 20000;
        }
        if (best == nullptr || score >= best_score) {
            best = f.get();
            best_score = score;
        }
    }
    if (best != nullptr && !ensureOpen(*const_cast<FontFace*>(best))) {
        return resolve(family, weight, italic);  // marked broken; next best
    }
    if (best == nullptr && want != "sans") {
        return resolve("sans", weight, italic);
    }
    if (best == nullptr) {
        for (const auto& f : faces_) {
            if (ensureOpen(*f)) {
                return f.get();
            }
        }
    }
    return best;
}

const FontFace* FontLibrary::fallback(int weight, bool italic) {
    return resolve("sans", weight, italic);
}

const FontFace* FontLibrary::face(uint16_t id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return id < faces_.size() ? faces_[id].get() : nullptr;
}

uint32_t FontLibrary::glyphIndex(const FontFace& face, uint32_t codepoint) {
    hb_codepoint_t gid = 0;
    return hb_font_get_nominal_glyph(hbFont(face.hb_font), codepoint, &gid) ? gid : 0;
}

bool FontLibrary::glyphExtents(const FontFace& face, uint32_t glyph, float box[4]) {
    hb_glyph_extents_t e{};
    if (!hb_font_get_glyph_extents(hbFont(face.hb_font), glyph, &e) || e.width == 0 ||
        e.height == 0) {
        return false;
    }
    box[0] = static_cast<float>(e.x_bearing) / face.upem;
    box[3] = static_cast<float>(e.y_bearing) / face.upem;
    box[2] = static_cast<float>(e.x_bearing + e.width) / face.upem;
    box[1] = static_cast<float>(e.y_bearing + e.height) / face.upem;  // height < 0
    return true;
}

void FontLibrary::withLock(const std::function<void()>& fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    fn();
}

std::vector<std::string> FontLibrary::families() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::set<std::string> names;
    for (const auto& f : faces_) {
        names.insert(f->family);
    }
    return {names.begin(), names.end()};
}

}  // namespace detail

bool registerFont(const std::string& family, const std::string& file, int weight, bool italic) {
    auto& lib = detail::FontLibrary::instance();
    lib.ensureDefaults();
    std::string error;
    if (!lib.addFile(family, file, weight, italic, error)) {
        std::fprintf(stderr, "fluent_scene: %s\n", error.c_str());
        return false;
    }
    return true;
}

bool registerFontData(const std::string& family, const uint8_t* data, size_t size, int weight,
                      bool italic) {
    auto& lib = detail::FontLibrary::instance();
    lib.ensureDefaults();
    if (data == nullptr || size == 0) {
        return false;
    }
    std::string error;
    if (!lib.add(family, std::vector<uint8_t>(data, data + size), weight, italic, error)) {
        std::fprintf(stderr, "fluent_scene: %s\n", error.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> fontFamilies() {
    auto& lib = detail::FontLibrary::instance();
    lib.ensureDefaults();
    return lib.families();
}

}  // namespace fluent_scene
