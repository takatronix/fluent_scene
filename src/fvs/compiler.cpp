// compiler.cpp — SceneDoc → Stage, through the public Layer API only.
//
// Every layer is created by the same drawing call a C++ author writes
// (stage.rect(...), stage.image(...)), and only explicitly-declared fields
// are applied afterwards — so every default lives once, in the C++ API, and
// the §2 "same picture from YAML and C++" contract holds by construction.

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

#include "fluent_scene/fvs/compiler.hpp"
#include "fluent_scene/transaction.hpp"

namespace fluent_scene::fvs {

namespace {

Color placeholderInk(float alpha) { return {1, 1, 1, alpha}; }

}  // namespace

/// Grants compile() access to CompiledScene's private members without
/// widening its public API.
struct CompileAccess {
    static std::unique_ptr<CompiledScene> create() {
        return std::unique_ptr<CompiledScene>(new CompiledScene());
    }
    static SceneDoc& doc(CompiledScene& s) { return s.doc_; }
    static std::string& digestRef(CompiledScene& s) { return s.digest_; }
    static std::unique_ptr<Stage>& stage(CompiledScene& s) { return s.stage_; }
    static auto& nodes(CompiledScene& s) { return s.nodes_; }
    static auto& paramValues(CompiledScene& s) { return s.param_values_; }
    static auto& paramBindings(CompiledScene& s) { return s.param_bindings_; }
    static auto& inputBindings(CompiledScene& s) { return s.input_bindings_; }
    static auto& filterImageBindings(CompiledScene& s) { return s.filter_image_bindings_; }
    static auto& maskBindings(CompiledScene& s) { return s.mask_bindings_; }
    static auto& controlBindings(CompiledScene& s) { return s.control_bindings_; }
    static auto& layerTransitions(CompiledScene& s) { return s.layer_transitions_; }
    static auto& inputState(CompiledScene& s) { return s.input_state_; }
    static auto& buttons(CompiledScene& s) { return s.buttons_; }
    static auto& switches(CompiledScene& s) { return s.switches_; }
    static auto& sliders(CompiledScene& s) { return s.sliders_; }
    static auto& gauges(CompiledScene& s) { return s.gauges_; }
    static void fire(CompiledScene& s, const UiEvent& event) {
        if (s.ui_handler_) {
            s.ui_handler_(event);
        }
    }
};

namespace {

/// The value an attribute starts at when bound to `$params.<name>`: the
/// param's declared default, typed like the param (applyAttr adapts an f32
/// to a uniform scale).
Value paramInitialValue(const ParamDecl& p) {
    Value v;
    v.kind = paramValueKind(p.type);
    v.num[0] = p.def[0];
    v.num[1] = p.def[1];
    v.num[2] = p.def[2];
    v.num[3] = p.def[3];
    v.flag = p.def_flag;
    return v;
}

/// Applies one attribute value through the Layer setters. Shared by the
/// build pass and setParam so both spell the mapping exactly once.
void applyAttr(Layer& layer, AttrId id, const Value& v) {
    switch (id) {
        case AttrId::Bounds: layer.bounds(v.rect()); break;
        case AttrId::Position: layer.position(v.vec2()); break;
        case AttrId::Anchor: layer.anchor(v.num[0], v.num[1]); break;
        case AttrId::Frame: layer.frame(v.rect()); break;
        case AttrId::Rotation: layer.rotation(v.num[0]); break;
        case AttrId::Scale:
            // An f32 param bound to scale is the [s, s] shorthand.
            if (v.kind == ValueKind::F32) {
                layer.scale(v.num[0]);
            } else {
                layer.scale(v.num[0], v.num[1]);
            }
            break;
        case AttrId::Transform:
            layer.transform(Mat23{v.mat[0], v.mat[1], v.mat[2], v.mat[3], v.mat[4], v.mat[5]});
            break;
        case AttrId::Opacity: layer.opacity(v.num[0]); break;
        case AttrId::Hidden: layer.hidden(v.flag); break;
        case AttrId::MasksToBounds: layer.masksToBounds(v.flag); break;
        case AttrId::CornerRadius: layer.cornerRadius(v.num[0]); break;
        case AttrId::Background: layer.background(v.color()); break;
        case AttrId::Blend: layer.blend(static_cast<Blend>(v.enum_value)); break;
        case AttrId::Shadow:
        case AttrId::Border: break;  // map attributes, applied by applyMapAttr
    }
}

void setShadowField(Shadow& s, const FieldSpec& field, const Value& v) {
    const std::string name = field.name;
    if (name == "offset") {
        s.offset = v.vec2();
    } else if (name == "radius") {
        s.radius = v.num[0];
    } else if (name == "color") {
        s.color = v.color();
    } else if (name == "opacity") {
        s.opacity = v.num[0];
    }
}

void setBorderField(Border& b, const FieldSpec& field, const Value& v) {
    const std::string name = field.name;
    if (name == "width") {
        b.width = v.num[0];
    } else if (name == "color") {
        b.color = v.color();
    }
}

/// Applies a shadow/border map; `value` resolves each field (a literal, or
/// a bound param's current value).
template <typename Resolve>
void applyMapAttr(Layer& layer, const AttrDecl& attr, Resolve value) {
    if (attr.spec->id == AttrId::Shadow) {
        Shadow s{};
        for (const MapField& f : attr.map) {
            setShadowField(s, *f.spec, value(f.value));
        }
        layer.shadow(s);
    } else if (attr.spec->id == AttrId::Border) {
        Border b{};
        for (const MapField& f : attr.map) {
            setBorderField(b, *f.spec, value(f.value));
        }
        layer.border(b);
    }
}

/// Applies a shared style field (color/thickness/…) via its Layer setter.
void applyStyle(Layer& layer, const FieldSpec& field, const Value& v) {
    const std::string name = field.name;
    if (name == "color") {
        layer.color(v.color());
    } else if (name == "thickness") {
        layer.thickness(v.num[0]);
    } else if (name == "dash") {
        layer.dash(v.num[0]);
    } else if (name == "cap") {
        layer.cap(static_cast<Cap>(v.enum_value));
    } else if (name == "corner_radius") {
        layer.cornerRadius(v.num[0]);
    } else if (name == "smoothing") {
        layer.smoothing(v.num[0]);
    } else if (name == "show_label") {
        layer.showLabel(v.flag);
    }
}

/// Writes one param value through one binding (build pass and setParam).
void applyBinding(const CompiledScene::ParamBinding& b, const Value& v) {
    using Target = CompiledScene::ParamBinding::Target;
    switch (b.target) {
        case Target::Attr: applyAttr(*b.layer, b.attr, v); break;
        case Target::Style: applyStyle(*b.layer, *b.field, v); break;
        case Target::ShadowField: {
            Shadow s = b.layer->shadowValue().value_or(Shadow{});
            setShadowField(s, *b.field, v);
            b.layer->shadow(s);
            break;
        }
        case Target::BorderField: {
            Border border = b.layer->borderValue().value_or(Border{});
            setBorderField(border, *b.field, v);
            b.layer->border(border);
            break;
        }
        case Target::FilterParam:
            b.layer->setFilterParam(b.filter_index, b.filter_slot, v.num[0]);
            break;
    }
}

using ParamValues = std::map<std::string, Value>;

/// A declared Rect attribute's value now: the literal, or the bound param's
/// current value.
Rect declaredRect(const ParamValues& params, const AttrDecl& attr) {
    if (!attr.value.param.empty()) {
        if (auto it = params.find(attr.value.param); it != params.end()) {
            return it->second.rect();
        }
    }
    return attr.value.rect();
}

/// The statically-resolvable size of a declared layer, for placeholder
/// layout: declared frame/bounds win; image and grid content fill the
/// parent; everything else does not matter for placeholders.
Vec2 staticSize(const ParamValues& params, const LayerDecl& decl, Vec2 parent_size) {
    if (const AttrDecl* a = decl.find(AttrId::Bounds)) {
        return declaredRect(params, *a).size();
    }
    if (const AttrDecl* a = decl.find(AttrId::Frame)) {
        return declaredRect(params, *a).size();
    }
    return parent_size;
}

class Builder {
public:
    Builder(CompiledScene& scene, const SceneDoc& doc, const CompileOptions& options,
            DiagnosticList& diags)
        : scene_(scene), doc_(doc), options_(options), diags_(diags) {}

    bool build() {
        // Bound resource check, up front (§15-4: never throw mid-build).
        // Each unfed image input may add up to 2 placeholder layers.
        uint32_t placeholder_budget = 0;
        for (const InputDecl& in : doc_.inputs) {
            if (in.type == InputType::ImageRgba8 &&
                in.fallback == InputFallback::Placeholder) {
                placeholder_budget += 2;
            }
        }
        if (doc_.layer_count + placeholder_budget + 1 > options_.limits.max_layers) {
            error("compile.budget",
                  "the document declares " + std::to_string(doc_.layer_count) +
                      " layers (+" + std::to_string(placeholder_budget) +
                      " placeholders), over the limit of " +
                      std::to_string(options_.limits.max_layers));
            return false;
        }
        if (doc_.max_depth + 2 > options_.limits.max_depth) {
            error("compile.budget",
                  "the document nests " + std::to_string(doc_.max_depth) +
                      " layers deep, over the limit of " +
                      std::to_string(options_.limits.max_depth));
            return false;
        }
        for (const InputDecl& in : doc_.inputs) {
            const uint32_t cap = in.type == InputType::SeqVec2 ? options_.limits.max_points
                                 : in.type == InputType::SeqDetection2D
                                     ? options_.limits.max_boxes
                                     : 0;
            if (cap != 0 && in.capacity > cap) {
                error("compile.budget",
                      "input '" + in.name + "' declares capacity " +
                          std::to_string(in.capacity) + ", over the limit of " +
                          std::to_string(cap));
                return false;
            }
        }

        // Every param starts at its declared default.
        for (const ParamDecl& p : doc_.params) {
            CompileAccess::paramValues(scene_)[p.name] = paramInitialValue(p);
        }

        auto stage = std::make_unique<Stage>(doc_.width, doc_.height, options_.limits);
        stage->fit(doc_.fit);
        CompileAccess::stage(scene_) = std::move(stage);
        for (const LayerDecl& decl : doc_.layers) {
            buildLayer(CompileAccess::stage(scene_)->root(), decl,
                       {doc_.width, doc_.height});
        }
        return !diags_.hasErrors();
    }

    /// Builds one grafted fragment layer (CompiledScene::addLayers) under
    /// `parent`, placeholder-sized against its declared parent chain.
    Layer* buildGrafted(Layer& parent, const LayerDecl& decl) {
        return buildLayer(parent, decl, staticParentSizeOf(decl));
    }

    /// Placeholders a fragment may add: up to two per layer showing an
    /// image input with the placeholder fallback (scene or fragment input).
    uint32_t placeholderBudget(const std::vector<LayerDecl>& layers,
                               const SceneDoc& frag) const {
        uint32_t n = 0;
        for (const LayerDecl& l : layers) {
            if (l.content) {
                if (const MapField* src = l.content->find("source")) {
                    const InputDecl* in = frag.findInput(src->value.input);
                    if (in == nullptr) {
                        in = doc_.findInput(src->value.input);
                    }
                    if (in != nullptr && in->type == InputType::ImageRgba8 &&
                        in->fallback == InputFallback::Placeholder) {
                        n += 2;
                    }
                }
            }
            n += placeholderBudget(l.sublayers, frag);
        }
        return n;
    }

private:
    void error(std::string code, std::string message) {
        diags_.add(std::move(code), Severity::kError, Phase::kCompile, Span{},
                   std::move(message));
    }

    const ParamValues& params() const { return CompileAccess::paramValues(scene_); }

    /// A declared value as it applies now: the literal, or the bound
    /// param's current value.
    Value resolve(const Value& v) const {
        if (v.param.empty()) {
            return v;
        }
        return params().at(v.param);
    }

    /// Registers `binding` when `v` is a `$params` reference.
    void bind(const Value& v, const CompiledScene::ParamBinding& binding) {
        if (!v.param.empty()) {
            CompileAccess::paramBindings(scene_)[v.param].push_back(binding);
        }
    }

    Layer* buildLayer(Layer& parent, const LayerDecl& decl, Vec2 parent_size) {
        Layer* layer = createLayer(parent, decl);
        if (layer == nullptr) {
            return nullptr;
        }
        CompileAccess::nodes(scene_).emplace_back(&decl, layer);
        if (!decl.id.empty()) {
            layer->id(decl.id);
        }

        // Content style fields (explicit only — creation defaults stand;
        // $params refs start at the param's current value).
        using Target = CompiledScene::ParamBinding::Target;
        if (decl.content) {
            for (const MapField& f : decl.content->fields) {
                if (f.spec->is_style) {
                    applyStyle(*layer, *f.spec, resolve(f.value));
                    bind(f.value, {layer, AttrId::Opacity, Target::Style, f.spec});
                }
            }
        }

        // Attributes (table order).
        for (const AttrDecl& attr : decl.attrs) {
            const ValueKind kind = attr.spec->kind;
            if (kind == ValueKind::MapShadow || kind == ValueKind::MapBorder) {
                applyMapAttr(*layer, attr, [&](const Value& v) { return resolve(v); });
                const Target target =
                    kind == ValueKind::MapShadow ? Target::ShadowField : Target::BorderField;
                for (const MapField& f : attr.map) {
                    bind(f.value, {layer, attr.spec->id, target, f.spec});
                }
            } else {
                applyAttr(*layer, attr.spec->id, resolve(attr.value));
                bind(attr.value, {layer, attr.spec->id});
            }
        }

        for (size_t i = 0; i < decl.filters.size(); ++i) {
            const FilterDecl& f = decl.filters[i];
            Filter value = f.value;
            for (const auto& [slot, param] : f.param_refs) {
                value.values[slot] = params().at(param).num[0];
            }
            layer->filter(value);
            for (const auto& [slot, param] : f.param_refs) {
                CompiledScene::ParamBinding b{layer, AttrId::Opacity, Target::FilterParam};
                b.filter_index = i;
                b.filter_slot = slot;
                CompileAccess::paramBindings(scene_)[param].push_back(b);
            }
            if (!f.image_input.empty()) {
                const InputDecl* input = doc_.findInput(f.image_input);
                if (input == nullptr || input->type != InputType::ImageRgba8) {
                    error("compile.reference",
                          std::string(f.spec->name) + " " + f.spec->image_param +
                              ": " +
                              (input == nullptr
                                   ? "undeclared input '" + f.image_input + "'"
                                   : "input '" + f.image_input + "' is not image.rgba8"));
                    continue;
                }
                CompileAccess::filterImageBindings(scene_)[f.image_input].push_back(
                    {layer, i});
            }
        }
        if (decl.mask) {
            const InputDecl* input = doc_.findInput(decl.mask->input);
            if (input == nullptr || input->type != InputType::ImageRgba8) {
                error("compile.reference",
                      std::string("mask.source: ") +
                          (input == nullptr
                               ? "undeclared input '" + decl.mask->input + "'"
                               : "input '" + decl.mask->input + "' is not image.rgba8"));
            } else {
                layer->maskInvert(decl.mask->invert);
                layer->maskFeather(decl.mask->feather);
                CompileAccess::maskBindings(scene_)[decl.mask->input].push_back({layer});
            }
        }
        if (decl.transition) {
            CompileAccess::layerTransitions(scene_)[layer] = *decl.transition;
        }

        const Vec2 own_size = staticSize(params(), decl, parent_size);
        for (const LayerDecl& sub : decl.sublayers) {
            buildLayer(*layer, sub, own_size);
        }
        return layer;
    }

    /// The text styling fields, through the same Layer setters C++ uses.
    static void applyTextFields(Layer& layer, const ContentDecl& c) {
        const auto has = [&](const char* name) { return c.find(name) != nullptr; };
        const auto num = [&](const char* name) { return c.find(name)->value.num[0]; };
        const auto flag = [&](const char* name) { return c.find(name)->value.flag; };
        const auto color = [&](const char* name) { return c.find(name)->value.color(); };
        if (const MapField* f = c.find("vertical_align")) {
            layer.verticalAlign(static_cast<VAlign>(f->value.enum_value));
        }
        if (const MapField* f = c.find("font")) {
            layer.font(f->value.str);
        }
        if (has("weight")) {
            layer.fontWeight(static_cast<int>(std::lround(num("weight"))));
        }
        if (has("italic")) {
            layer.italic(flag("italic"));
        }
        if (has("letter_spacing")) {
            layer.letterSpacing(num("letter_spacing"));
        }
        if (has("line_height")) {
            layer.lineHeight(num("line_height"));
        }
        if (has("tabular")) {
            layer.tabularNumbers(flag("tabular"));
        }
        if (has("hinting")) {
            layer.textHinting(flag("hinting"));
        }
        if (has("max_width")) {
            layer.maxWidth(num("max_width"));
        }
        if (has("max_height")) {
            layer.maxHeight(num("max_height"));
        }
        if (has("wrap")) {
            layer.wrap(flag("wrap"));
        }
        if (has("max_lines")) {
            layer.maxLines(static_cast<int>(std::lround(num("max_lines"))));
        }
        if (has("ellipsis")) {
            layer.ellipsis(flag("ellipsis"));
        }
        if (has("min_size")) {
            layer.autoFit(num("min_size"));
        }
        if (has("outline_width") || has("outline_color")) {
            const MapField* pos = c.find("outline_position");
            layer.textOutline(has("outline_width") ? num("outline_width") : 2.0f,
                              has("outline_color") ? color("outline_color") : Color::Black,
                              pos != nullptr ? static_cast<OutlinePosition>(pos->value.enum_value)
                                             : OutlinePosition::Outside);
        }
        if (has("gradient_from") || has("gradient_to")) {
            const Color from = has("gradient_from") ? color("gradient_from") : Color::White;
            layer.textGradient(from, has("gradient_to") ? color("gradient_to") : from,
                               has("gradient_angle") ? num("gradient_angle") : 90.0f);
        }
        if (has("glow_radius") || has("glow_color")) {
            layer.glow(has("glow_radius") ? num("glow_radius") : 8.0f,
                       has("glow_color") ? color("glow_color") : Color::White);
        }
        if (const MapField* f = c.find("spans")) {
            layer.spans(f->value.spans);
        }
    }

    /// Creates the layer through the same drawing call a C++ author uses.
    Layer* createLayer(Layer& parent, const LayerDecl& decl) {
        if (!decl.content) {
            return &parent.group();
        }
        const ContentDecl& c = *decl.content;
        const std::string name = c.spec->name;
        auto num = [&](const char* field, float fallback) {
            const MapField* f = c.find(field);
            return f != nullptr ? f->value.num[0] : fallback;
        };
        auto vec = [&](const char* field) {
            const MapField* f = c.find(field);
            return f != nullptr ? f->value.vec2() : Vec2{};
        };

        // UI controls (§10, L4): the compiler instantiates the same ui::
        // prefabs a C++ author uses. The layer's declared frame places them;
        // gestures leave through CompiledScene::onUiEvent.
        if (name == "button" || name == "switch" || name == "slider" || name == "gauge") {
            return createControl(parent, decl, c);
        }

        Layer* layer = nullptr;
        if (name == "image") {
            layer = &parent.image(ImageView{});
            if (const MapField* f = c.find("fit")) {
                layer->fit(static_cast<Fit>(f->value.enum_value));
            }
            if (const MapField* f = c.find("source_rect")) {
                layer->sourceRect(f->value.rect());
            }
        } else if (name == "text") {
            const MapField* literal = c.find("text");
            layer = &parent.text(literal != nullptr ? literal->value.str : std::string{},
                                 vec("position"));
            if (const MapField* f = c.find("size")) {
                layer->size(f->value.num[0]);
            }
            if (const MapField* f = c.find("size_pt")) {
                layer->sizePt(f->value.num[0]);
            }
            if (const MapField* f = c.find("align")) {
                layer->align(static_cast<Align>(f->value.enum_value));
            }
            applyTextFields(*layer, c);
        } else if (name == "line") {
            layer = &parent.line(vec("from"), vec("to"));
        } else if (name == "polyline" || name == "polygon" || name == "circles") {
            const MapField* pts = c.find("points");
            static const std::vector<Vec2> kEmpty;
            const std::vector<Vec2>& points = pts != nullptr ? pts->value.points : kEmpty;
            if (name == "polyline") {
                layer = &parent.polyline(points);
            } else if (name == "polygon") {
                layer = &parent.polygon(points);
            } else if (const MapField* r = c.find("radius")) {
                layer = &parent.circles(points, r->value.num[0]);
            } else {
                layer = &parent.circles(points);
            }
        } else if (name == "rect") {
            Rect r{};
            if (const MapField* f = c.find("rect")) {
                r = f->value.rect();
            } else if (const MapField* f2 = c.find("size")) {
                r = {0, 0, f2->value.num[0], f2->value.num[1]};
            }
            layer = &parent.rect(r);
        } else if (name == "circle") {
            layer = &parent.circle(vec("center"), num("radius", 0));
        } else if (name == "arc") {
            layer = &parent.arc(vec("center"), num("radius", 0), num("start_deg", 0),
                                num("end_deg", 0));
        } else if (name == "arrow") {
            layer = &parent.arrow(vec("from"), vec("to"));
            if (const MapField* f = c.find("head_size")) {
                layer->headSize(f->value.num[0]);
            }
        } else if (name == "crosshair") {
            if (const MapField* f = c.find("size")) {
                layer = &parent.crosshair(vec("center"), f->value.num[0]);
            } else {
                layer = &parent.crosshair(vec("center"));
            }
        } else if (name == "grid") {
            if (const MapField* f = c.find("spacing")) {
                layer = &parent.grid(f->value.num[0]);
            } else {
                layer = &parent.grid();
            }
        } else if (name == "boxes") {
            layer = &parent.boxes({});
        } else {
            error("compile.internal", std::string("unhandled content type '") + name + "'");
            return nullptr;
        }

        // Input binding + fallback presentation.
        if (const MapField* src = c.find("source")) {
            registerInput(*layer, decl, src->value.input);
        }
        return layer;
    }

    /// Builds one UI prefab. Bindable fields (`on`, `value`) may carry a
    /// $params reference: the control starts at the param's default and
    /// setParam drives it from then on (programmatic changes fire no event,
    /// same as the C++ setters).
    Layer* createControl(Layer& parent, const LayerDecl& decl, const ContentDecl& c) {
        CompiledScene* scene = &scene_;
        const std::string name = c.spec->name;
        const AttrDecl* frame_attr = decl.find(AttrId::Frame);
        const Rect frame = frame_attr != nullptr ? declaredRect(params(), *frame_attr) : Rect{};

        // The event id: the declared id, or the auto id assigned later —
        // resolved lazily at fire time from the control's layer.
        if (name == "button") {
            const MapField* label = c.find("label");
            auto& control = CompileAccess::buttons(scene_).emplace_back(
                std::make_unique<ui::Button>(parent, frame,
                                             label != nullptr ? label->value.str : ""));
            ui::Button* raw = control.get();
            raw->onTap([scene, raw] {
                CompileAccess::fire(*scene, {raw->layer().id(), "button", 1, true});
            });
            return &raw->layer();
        }
        if (name == "switch") {
            auto& control = CompileAccess::switches(scene_).emplace_back(
                std::make_unique<ui::Switch>(parent, frame));
            ui::Switch* raw = control.get();
            if (const MapField* on = c.find("on")) {
                if (!on->value.param.empty()) {
                    const ParamDecl* p = doc_.findParam(on->value.param);
                    raw->setOn(resolve(on->value).flag, false);
                    CompileAccess::controlBindings(scene_)[p->name].push_back(
                        {CompiledScene::ControlBinding::Kind::SwitchOn, raw});
                } else {
                    raw->setOn(on->value.flag, false);
                }
            }
            raw->onChange([scene, raw](bool on) {
                CompileAccess::fire(*scene,
                                    {raw->layer().id(), "switch", on ? 1.0f : 0.0f, on});
            });
            return &raw->layer();
        }
        if (name == "slider") {
            float initial = 0;
            const ParamDecl* bound = nullptr;
            if (const MapField* value = c.find("value")) {
                if (!value->value.param.empty()) {
                    bound = doc_.findParam(value->value.param);
                    initial = resolve(value->value).num[0];
                } else {
                    initial = value->value.num[0];
                }
            }
            auto& control = CompileAccess::sliders(scene_).emplace_back(
                std::make_unique<ui::Slider>(parent, frame, initial));
            ui::Slider* raw = control.get();
            if (bound != nullptr) {
                CompileAccess::controlBindings(scene_)[bound->name].push_back(
                    {CompiledScene::ControlBinding::Kind::SliderValue, raw});
            }
            raw->onChange([scene, raw](float v) {
                CompileAccess::fire(*scene, {raw->layer().id(), "slider", v, false});
            });
            return &raw->layer();
        }
        // gauge — display only, no events.
        auto num_of = [&](const char* field, float fallback) {
            const MapField* f = c.find(field);
            return f != nullptr && f->value.param.empty() ? f->value.num[0] : fallback;
        };
        const MapField* center = c.find("center");
        auto& control = CompileAccess::gauges(scene_).emplace_back(std::make_unique<ui::Gauge>(
            parent, center != nullptr ? center->value.vec2() : Vec2{},
            num_of("radius", 0)));
        ui::Gauge* raw = control.get();
        if (const MapField* value = c.find("value")) {
            if (!value->value.param.empty()) {
                const ParamDecl* p = doc_.findParam(value->value.param);
                raw->setValue(resolve(value->value).num[0]);
                CompileAccess::controlBindings(scene_)[p->name].push_back(
                    {CompiledScene::ControlBinding::Kind::GaugeValue, raw});
            } else {
                raw->setValue(value->value.num[0]);
            }
        }
        return &raw->layer();
    }

    void registerInput(Layer& layer, const LayerDecl& decl, const std::string& input_name) {
        const InputDecl* input = doc_.findInput(input_name);
        CompiledScene::InputBinding binding;
        binding.layer = &layer;
        binding.fallback = input->fallback;

        if (input->fallback == InputFallback::Hide) {
            layer.hidden(true);
        } else if (input->fallback == InputFallback::Placeholder &&
                   input->type == InputType::ImageRgba8) {
            // A deterministic "no signal" panel: dark plate, faint grid,
            // and the input's name — so a missing feed diagnoses itself.
            const Vec2 size = staticSizeOf(decl);
            layer.background({0.10f, 0.11f, 0.13f, 1.0f});
            binding.background_was_placeholder = true;
            Layer& grid = layer.grid(48).color(placeholderInk(0.07f)).thickness(1);
            Layer& label = layer.text("$inputs." + input_name, {size.x * 0.5f, size.y * 0.5f})
                               .size(24)
                               .align(Align::Center)
                               .color(placeholderInk(0.45f));
            binding.placeholder_parts = {&grid, &label};
        }
        CompileAccess::inputBindings(scene_)[input_name].push_back(std::move(binding));
    }

    /// Static size of a declared layer against its statically-known parent
    /// chain (placeholder layout only).
    Vec2 staticSizeOf(const LayerDecl& target) {
        Vec2 size = {doc_.width, doc_.height};
        findStaticSize(doc_.layers, target, size, &size);
        return size;
    }

    /// The static size `target`'s parent hands down (the stage for roots).
    Vec2 staticParentSizeOf(const LayerDecl& target) {
        Vec2 size = {doc_.width, doc_.height};
        findParentSize(doc_.layers, target, size, &size);
        return size;
    }

    bool findParentSize(const std::vector<LayerDecl>& layers, const LayerDecl& target,
                        Vec2 parent_size, Vec2* out) const {
        for (const LayerDecl& decl : layers) {
            if (&decl == &target) {
                *out = parent_size;
                return true;
            }
            if (findParentSize(decl.sublayers, target, staticSize(params(), decl, parent_size),
                               out)) {
                return true;
            }
        }
        return false;
    }

    bool findStaticSize(const std::vector<LayerDecl>& layers, const LayerDecl& target,
                        Vec2 parent_size, Vec2* out) const {
        for (const LayerDecl& decl : layers) {
            const Vec2 own = staticSize(params(), decl, parent_size);
            if (&decl == &target) {
                *out = own;
                return true;
            }
            if (findStaticSize(decl.sublayers, target, own, out)) {
                return true;
            }
        }
        return false;
    }

    CompiledScene& scene_;
    const SceneDoc& doc_;
    const CompileOptions& options_;
    DiagnosticList& diags_;
};

}  // namespace

// ---------------------------------------------------------------------------
// CompiledScene runtime surface
// ---------------------------------------------------------------------------

void CompiledScene::diagnose(std::string message) {
    if (diagnostics_.size() < 256) {
        diagnostics_.push_back(std::move(message));
    }
}

std::vector<std::string> CompiledScene::drainDiagnostics() {
    std::vector<std::string> out = std::move(diagnostics_);
    diagnostics_.clear();
    for (std::string& s : stage_->drainDiagnostics()) {
        out.push_back(std::move(s));
    }
    return out;
}

Layer* CompiledScene::layerFor(const LayerDecl* decl) const {
    for (const auto& [d, layer] : nodes_) {
        if (d == decl) {
            return layer;
        }
    }
    return nullptr;
}

namespace {

/// Clears a binding's fallback presentation on first data.
void clearFallback(CompiledScene::InputBinding& binding) {
    if (binding.fed) {
        return;
    }
    binding.fed = true;
    if (binding.fallback == InputFallback::Hide) {
        binding.layer->hidden(false);
    }
    for (Layer* part : binding.placeholder_parts) {
        part->remove();
    }
    binding.placeholder_parts.clear();
    if (binding.background_was_placeholder) {
        binding.layer->background(Color::Transparent);
    }
}

}  // namespace

bool CompiledScene::setImage(const std::string& input, const ImageView& view) {
    const InputDecl* decl = doc_.findInput(input);
    if (decl == nullptr || decl->type != InputType::ImageRgba8) {
        diagnose("setImage('" + input + "'): " +
                 (decl == nullptr ? "undeclared input" : "input is not image.rgba8"));
        return false;
    }
    InputState& state = input_state_[input];
    state.fed = true;
    state.image = view;
    for (InputBinding& b : input_bindings_[input]) {
        clearFallback(b);
        b.layer->setImage(view);
    }
    for (const FilterImageBinding& b : filter_image_bindings_[input]) {
        b.layer->setFilterImage(b.filter_index, view);
    }
    for (const MaskBinding& b : mask_bindings_[input]) {
        b.layer->mask(view);
    }
    return true;
}

bool CompiledScene::setText(const std::string& input, const std::string& utf8) {
    const InputDecl* decl = doc_.findInput(input);
    if (decl == nullptr || decl->type != InputType::TextUtf8) {
        diagnose("setText('" + input + "'): " +
                 (decl == nullptr ? "undeclared input" : "input is not text.utf8"));
        return false;
    }
    InputState& state = input_state_[input];
    state.fed = true;
    state.text = utf8;
    for (InputBinding& b : input_bindings_[input]) {
        clearFallback(b);
        b.layer->setText(utf8);
    }
    return true;
}

bool CompiledScene::setPoints(const std::string& input, const std::vector<Vec2>& points) {
    const InputDecl* decl = doc_.findInput(input);
    if (decl == nullptr || decl->type != InputType::SeqVec2) {
        diagnose("setPoints('" + input + "'): " +
                 (decl == nullptr ? "undeclared input" : "input is not sequence<vec2, N>"));
        return false;
    }
    const std::vector<Vec2>* data = &points;
    std::vector<Vec2> clamped;
    if (points.size() > decl->capacity) {
        diagnose("setPoints('" + input + "'): " + std::to_string(points.size()) +
                 " points clamped to declared capacity " + std::to_string(decl->capacity));
        clamped.assign(points.begin(), points.begin() + decl->capacity);
        data = &clamped;
    }
    InputState& state = input_state_[input];
    state.fed = true;
    state.points = *data;
    for (InputBinding& b : input_bindings_[input]) {
        clearFallback(b);
        b.layer->setPoints(*data);
    }
    return true;
}

bool CompiledScene::setBoxes(const std::string& input, const std::vector<Box>& boxes) {
    const InputDecl* decl = doc_.findInput(input);
    if (decl == nullptr || decl->type != InputType::SeqDetection2D) {
        diagnose("setBoxes('" + input + "'): " +
                 (decl == nullptr ? "undeclared input"
                                  : "input is not sequence<detection2d, N>"));
        return false;
    }
    const std::vector<Box>* data = &boxes;
    std::vector<Box> clamped;
    if (boxes.size() > decl->capacity) {
        diagnose("setBoxes('" + input + "'): " + std::to_string(boxes.size()) +
                 " boxes clamped to declared capacity " + std::to_string(decl->capacity));
        clamped.assign(boxes.begin(), boxes.begin() + decl->capacity);
        data = &clamped;
    }
    InputState& state = input_state_[input];
    state.fed = true;
    state.boxes = *data;
    for (InputBinding& b : input_bindings_[input]) {
        clearFallback(b);
        b.layer->setBoxes(*data);
    }
    return true;
}

void CompiledScene::applyParam(const ParamDecl& decl, const Value& value) {
    // Bound UI controls first: their setters carry their own animation and
    // never fire user-gesture events.
    for (const ControlBinding& b : control_bindings_[decl.name]) {
        switch (b.kind) {
            case ControlBinding::Kind::SwitchOn:
                static_cast<ui::Switch*>(b.control)->setOn(value.flag);
                break;
            case ControlBinding::Kind::SliderValue:
                static_cast<ui::Slider*>(b.control)->setValue(value.num[0]);
                break;
            case ControlBinding::Kind::GaugeValue:
                static_cast<ui::Gauge*>(b.control)->setValue(value.num[0]);
                break;
        }
    }
    param_values_[decl.name] = value;
    for (const ParamBinding& b : param_bindings_[decl.name]) {
        // Animation priority (§9): the param's animate declaration, else the
        // layer's transition, else snap.
        if (decl.has_animate) {
            Transaction t(decl.animate_duration, decl.animate_ease);
            applyBinding(b, value);
        } else if (auto it = layer_transitions_.find(b.layer);
                   it != layer_transitions_.end()) {
            Transaction t(it->second.duration, it->second.ease);
            applyBinding(b, value);
        } else {
            applyBinding(b, value);
        }
    }
}

namespace {

bool paramGate(const SceneDoc& doc, const std::string& name, ParamType expected,
               const ParamDecl*& out, std::string& message) {
    out = doc.findParam(name);
    if (out == nullptr) {
        message = "undeclared param";
        return false;
    }
    if (!out->runtime_mutable) {
        message = "param is not runtime_mutable";
        return false;
    }
    if (out->type != expected) {
        message = std::string("param has type ") + paramTypeName(out->type);
        return false;
    }
    return true;
}

}  // namespace

bool CompiledScene::setParam(const std::string& name, float value) {
    const ParamDecl* decl = nullptr;
    std::string why;
    if (!paramGate(doc_, name, ParamType::F32, decl, why)) {
        diagnose("setParam('" + name + "'): " + why);
        return false;
    }
    Value v;
    v.kind = ValueKind::F32;
    v.num[0] = value;
    applyParam(*decl, v);
    return true;
}

bool CompiledScene::setParam(const std::string& name, Vec2 value) {
    const ParamDecl* decl = nullptr;
    std::string why;
    if (!paramGate(doc_, name, ParamType::Vec2, decl, why)) {
        diagnose("setParam('" + name + "'): " + why);
        return false;
    }
    Value v;
    v.kind = ValueKind::Vec2;
    v.num[0] = value.x;
    v.num[1] = value.y;
    applyParam(*decl, v);
    return true;
}

bool CompiledScene::setParam(const std::string& name, Rect value) {
    const ParamDecl* decl = nullptr;
    std::string why;
    if (!paramGate(doc_, name, ParamType::Rect, decl, why)) {
        diagnose("setParam('" + name + "'): " + why);
        return false;
    }
    Value v;
    v.kind = ValueKind::Rect;
    v.num[0] = value.x;
    v.num[1] = value.y;
    v.num[2] = value.w;
    v.num[3] = value.h;
    applyParam(*decl, v);
    return true;
}

bool CompiledScene::setParam(const std::string& name, Color value) {
    const ParamDecl* decl = nullptr;
    std::string why;
    if (!paramGate(doc_, name, ParamType::Color, decl, why)) {
        diagnose("setParam('" + name + "'): " + why);
        return false;
    }
    Value v;
    v.kind = ValueKind::Color;
    v.num[0] = value.r;
    v.num[1] = value.g;
    v.num[2] = value.b;
    v.num[3] = value.a;
    applyParam(*decl, v);
    return true;
}

bool CompiledScene::setParam(const std::string& name, bool value) {
    const ParamDecl* decl = nullptr;
    std::string why;
    if (!paramGate(doc_, name, ParamType::Bool, decl, why)) {
        diagnose("setParam('" + name + "'): " + why);
        return false;
    }
    Value v;
    v.kind = ValueKind::Bool;
    v.flag = value;
    applyParam(*decl, v);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime tree edits (docs/design/runtime_layers.ja.md)
// ---------------------------------------------------------------------------

namespace {

/// Every live layer of a subtree (placeholder parts and control internals
/// included), root first.
void collectSubtree(Layer& layer, std::vector<Layer*>& out) {
    out.push_back(&layer);
    for (const auto& child : layer.sublayers()) {
        collectSubtree(*child, out);
    }
}

uint32_t countDecls(const std::vector<LayerDecl>& layers) {
    uint32_t n = 0;
    for (const LayerDecl& l : layers) {
        n += 1 + countDecls(l.sublayers);
    }
    return n;
}

uint32_t maxDepth(const std::vector<LayerDecl>& layers, uint32_t depth) {
    uint32_t deepest = 0;
    for (const LayerDecl& l : layers) {
        deepest = std::max({deepest, depth, maxDepth(l.sublayers, depth + 1)});
    }
    return deepest;
}

/// The declared layer with this explicit id, and the vector that owns it.
LayerDecl* findDecl(std::vector<LayerDecl>& layers, const std::string& id,
                    std::vector<LayerDecl>** owner) {
    for (LayerDecl& l : layers) {
        if (!id.empty() && l.id == id) {
            *owner = &layers;
            return &l;
        }
        if (LayerDecl* hit = findDecl(l.sublayers, id, owner)) {
            return hit;
        }
    }
    return nullptr;
}

/// The model value of an edge-animatable attribute (transition enter/exit).
Value modelValue(const Layer& layer, AttrId id) {
    Value v;
    switch (id) {
        case AttrId::Opacity:
            v.kind = ValueKind::F32;
            v.num[0] = layer.opacity();
            break;
        case AttrId::Rotation:
            v.kind = ValueKind::F32;
            v.num[0] = layer.rotation();
            break;
        case AttrId::Position:
            v.kind = ValueKind::Vec2;
            v.num[0] = layer.position().x;
            v.num[1] = layer.position().y;
            break;
        default:  // Scale
            v.kind = ValueKind::Vec2;
            v.num[0] = layer.scale().x;
            v.num[1] = layer.scale().y;
            break;
    }
    return v;
}

/// transition.enter / exit, or the default fade.
std::vector<AttrDecl> edgeState(const std::optional<std::vector<AttrDecl>>& declared) {
    if (declared) {
        return *declared;
    }
    AttrDecl fade;
    fade.spec = findAttrSpec("opacity");
    fade.value.kind = ValueKind::F32;
    fade.value.num[0] = 0;
    return {fade};
}

}  // namespace

TransitionDecl CompiledScene::edgeTransition(const Layer& layer) const {
    for (const Layer* l = &layer; l != nullptr; l = l->parent()) {
        if (auto it = layer_transitions_.find(l); it != layer_transitions_.end()) {
            return it->second;
        }
    }
    return TransitionDecl{};  // 0.3 s ease_in_out fade
}

void CompiledScene::rezipNodes(const std::vector<Layer*>& extra) {
    // The live document layers: the ones nodes_ knew (minus whatever an
    // edit dropped from it beforehand) plus `extra`. Decl pointers in nodes_
    // may dangle after a graft — only the Layer side is read here.
    std::unordered_set<const Layer*> live(extra.begin(), extra.end());
    for (const auto& node : nodes_) {
        live.insert(node.second);
    }
    nodes_.clear();
    // Declared sublayers are appended in declaration order, and edits keep
    // that order, so the document layers among a layer's sublayers zip 1:1
    // with its declared sublayers (placeholder parts, control internals and
    // layers animating out are not in `live`).
    struct Zip {
        static void walk(std::vector<std::pair<const LayerDecl*, Layer*>>& out,
                         const std::unordered_set<const Layer*>& live,
                         const std::vector<LayerDecl>& decls, const Layer& parent) {
            size_t i = 0;
            for (const auto& child : parent.sublayers()) {
                if (i >= decls.size()) {
                    break;
                }
                if (!live.count(child.get())) {
                    continue;
                }
                out.emplace_back(&decls[i], child.get());
                walk(out, live, decls[i].sublayers, *child);
                ++i;
            }
        }
    };
    Zip::walk(nodes_, live, doc_.layers, stage_->root());
}

void CompiledScene::replayInputs(const std::vector<Layer*>& fresh) {
    const std::unordered_set<const Layer*> is_fresh(fresh.begin(), fresh.end());
    for (const auto& [name, state] : input_state_) {
        const InputDecl* decl = doc_.findInput(name);
        if (!state.fed || decl == nullptr) {
            continue;
        }
        for (InputBinding& b : input_bindings_[name]) {
            if (!is_fresh.count(b.layer)) {
                continue;
            }
            clearFallback(b);
            switch (decl->type) {
                case InputType::ImageRgba8: b.layer->setImage(state.image); break;
                case InputType::TextUtf8: b.layer->setText(state.text); break;
                case InputType::SeqVec2: b.layer->setPoints(state.points); break;
                case InputType::SeqDetection2D: b.layer->setBoxes(state.boxes); break;
            }
        }
        for (const FilterImageBinding& b : filter_image_bindings_[name]) {
            if (is_fresh.count(b.layer)) {
                b.layer->setFilterImage(b.filter_index, state.image);
            }
        }
        for (const MaskBinding& b : mask_bindings_[name]) {
            if (is_fresh.count(b.layer)) {
                b.layer->mask(state.image);
            }
        }
    }
}

void CompiledScene::unbind(const std::vector<Layer*>& doomed) {
    const std::unordered_set<const Layer*> gone(doomed.begin(), doomed.end());
    auto drop = [&](auto& by_name) {
        for (auto& [name, bindings] : by_name) {
            bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                          [&](const auto& b) { return gone.count(b.layer); }),
                           bindings.end());
        }
    };
    drop(param_bindings_);
    drop(input_bindings_);
    drop(filter_image_bindings_);
    drop(mask_bindings_);
    // UI controls living in the subtree go now: their destructors detach
    // the pointer handlers from layers that are still alive.
    auto dropControls = [&](auto& controls) {
        for (auto it = controls.begin(); it != controls.end();) {
            if (!gone.count(&(*it)->layer())) {
                ++it;
                continue;
            }
            const void* raw = it->get();
            for (auto& [name, bindings] : control_bindings_) {
                bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                              [&](const ControlBinding& b) {
                                                  return b.control == raw;
                                              }),
                               bindings.end());
            }
            it = controls.erase(it);
        }
    };
    dropControls(buttons_);
    dropControls(switches_);
    dropControls(sliders_);
    dropControls(gauges_);
    for (auto it = layer_transitions_.begin(); it != layer_transitions_.end();) {
        it = gone.count(it->first) ? layer_transitions_.erase(it) : std::next(it);
    }
}

EditResult CompiledScene::addLayers(const std::string& parent_id,
                                    const std::string& fragment_yaml, bool animated) {
    EditResult result;
    auto reject = [&](const char* code, std::string message) {
        result.diagnostics.add(code, Severity::kError, Phase::kCompile, Span{},
                               std::move(message));
    };

    // Where the layers go: a declared layer, or the root.
    Layer* parent = &stage_->root();
    std::vector<LayerDecl>* siblings = &doc_.layers;
    uint32_t parent_depth = 0;
    if (!parent_id.empty() && parent_id != "root") {
        std::vector<LayerDecl>* owner = nullptr;
        LayerDecl* parent_decl = findDecl(doc_.layers, parent_id, &owner);
        parent = parent_decl != nullptr ? layerFor(parent_decl) : nullptr;
        if (parent == nullptr) {
            reject("edit.reference", "no declared layer '" + parent_id + "' to add under");
            return result;
        }
        siblings = &parent_decl->sublayers;
        for (const Layer* up = parent; up->parent() != nullptr; up = up->parent()) {
            ++parent_depth;
        }
    }

    // Validate the fragment against the scene — rejection changes nothing.
    ParseResult parsed = parseFragment(fragment_yaml, doc_);
    result.diagnostics = parsed.diagnostics;
    if (!parsed.ok()) {
        return result;
    }
    SceneDoc& frag = parsed.doc;
    if (frag.layers.empty()) {
        return result;  // nothing to add (declarations alone are not an edit)
    }

    // Resource bounds, against what the stage holds now (§15-4).
    const StageLimits& limits = stage_->limits();
    CompileOptions options;
    options.limits = limits;
    {
        DiagnosticList scratch;
        Builder probe(*this, doc_, options, scratch);
        const uint32_t placeholders = probe.placeholderBudget(frag.layers, frag);
        if (stage_->layerCount() + frag.layer_count + placeholders > limits.max_layers) {
            reject("compile.budget",
                   "adding " + std::to_string(frag.layer_count) + " layers (+" +
                       std::to_string(placeholders) + " placeholders) to the " +
                       std::to_string(stage_->layerCount()) +
                       " on stage would exceed the limit of " +
                       std::to_string(limits.max_layers));
            return result;
        }
    }
    if (parent_depth + frag.max_depth + 2 > limits.max_depth) {
        reject("compile.budget", "the fragment would nest " +
                                     std::to_string(parent_depth + frag.max_depth) +
                                     " layers deep, over the limit of " +
                                     std::to_string(limits.max_depth));
        return result;
    }
    for (const InputDecl& in : frag.inputs) {
        const uint32_t cap = in.type == InputType::SeqVec2          ? limits.max_points
                             : in.type == InputType::SeqDetection2D ? limits.max_boxes
                                                                    : 0;
        if (cap != 0 && in.capacity > cap) {
            reject("compile.budget", "input '" + in.name + "' declares capacity " +
                                         std::to_string(in.capacity) +
                                         ", over the limit of " + std::to_string(cap));
            return result;
        }
    }

    // Commit point. Declarations join the scene, the layers join the
    // document, then the Stage — through the same Builder as compile().
    const std::vector<InputDecl> inputs_before = doc_.inputs;
    const std::vector<ParamDecl> params_before = doc_.params;
    const uint32_t count_before = doc_.layer_count;
    const uint32_t depth_before = doc_.max_depth;
    auto byName = [](const auto& a, const auto& b) { return a.name < b.name; };
    for (const InputDecl& in : frag.inputs) {
        doc_.inputs.push_back(in);
    }
    std::sort(doc_.inputs.begin(), doc_.inputs.end(), byName);
    for (const ParamDecl& p : frag.params) {
        doc_.params.push_back(p);
        param_values_[p.name] = paramInitialValue(p);
    }
    std::sort(doc_.params.begin(), doc_.params.end(), byName);

    const size_t first = siblings->size();
    for (LayerDecl& l : frag.layers) {
        siblings->push_back(std::move(l));
    }
    doc_.layer_count += frag.layer_count;
    doc_.max_depth = maxDepth(doc_.layers, 1);

    const size_t known = nodes_.size();
    const size_t under_before = parent->sublayers().size();
    std::vector<Layer*> tops;
    {
        // Built layers start where they are declared — never mid-animation,
        // even when the host calls this inside its own Transaction.
        Transaction snap(0.0f);
        Builder builder(*this, doc_, options, result.diagnostics);
        try {
            for (size_t i = first; i < siblings->size(); ++i) {
                tops.push_back(builder.buildGrafted(*parent, (*siblings)[i]));
            }
        } catch (const std::length_error& e) {
            reject("compile.budget", e.what());
        }
    }
    // Everything the edit created — all of it lands under `parent`, after
    // what was there (a partial build included).
    std::vector<Layer*> created;
    for (size_t i = under_before; i < parent->sublayers().size(); ++i) {
        created.push_back(parent->sublayers()[i].get());
    }
    std::vector<Layer*> fresh;
    for (Layer* top : created) {
        collectSubtree(*top, fresh);
    }
    std::vector<Layer*> new_nodes;
    for (size_t i = known; i < nodes_.size(); ++i) {
        new_nodes.push_back(nodes_[i].second);
    }

    if (result.diagnostics.hasErrors()) {
        // Roll back: the scene must be exactly as before the call.
        unbind(fresh);
        nodes_.resize(known);
        for (Layer* top : created) {
            top->remove();
        }
        siblings->erase(siblings->begin() + static_cast<std::ptrdiff_t>(first), siblings->end());
        for (const ParamDecl& p : frag.params) {
            param_values_.erase(p.name);
        }
        doc_.inputs = inputs_before;
        doc_.params = params_before;
        doc_.layer_count = count_before;
        doc_.max_depth = depth_before;
        rezipNodes({});
        return result;
    }

    rezipNodes(new_nodes);
    replayInputs(fresh);
    digest_ = fvs::digest(doc_);

    for (size_t i = 0; i < tops.size(); ++i) {
        Layer& layer = *tops[i];
        const LayerDecl& decl = (*siblings)[first + i];
        if (!decl.id.empty()) {
            result.ids.push_back(decl.id);
        }
        if (!animated) {
            continue;
        }
        // Enter (§9): start at transition.enter, animate to what was built.
        const TransitionDecl t = edgeTransition(layer);
        for (const AttrDecl& edge : edgeState(t.enter)) {
            const Value target = modelValue(layer, edge.spec->id);
            {
                Transaction snap(0.0f);
                applyAttr(layer, edge.spec->id, edge.value);
            }
            Transaction animate(t.duration, t.ease);
            applyAttr(layer, edge.spec->id, target);
        }
    }
    return result;
}

bool CompiledScene::removeLayer(const std::string& id, bool animated) {
    std::vector<LayerDecl>* owner = nullptr;
    LayerDecl* decl = id.empty() ? nullptr : findDecl(doc_.layers, id, &owner);
    Layer* layer = decl != nullptr ? layerFor(decl) : nullptr;
    if (layer == nullptr) {
        diagnose("removeLayer('" + id + "'): no declared layer with this id");
        return false;
    }
    const TransitionDecl t = edgeTransition(*layer);  // before its bindings go

    std::vector<Layer*> doomed;
    collectSubtree(*layer, doomed);
    unbind(doomed);
    const std::unordered_set<const Layer*> gone(doomed.begin(), doomed.end());
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                [&](const auto& node) { return gone.count(node.second); }),
                 nodes_.end());
    doc_.layer_count -= 1 + countDecls(decl->sublayers);
    owner->erase(owner->begin() + (decl - owner->data()));
    doc_.max_depth = maxDepth(doc_.layers, 1);
    rezipNodes({});
    digest_ = fvs::digest(doc_);

    if (!animated) {
        layer->remove();
        return true;
    }
    // Exit (§9): animate to transition.exit, detach once it settles. The
    // layer keeps rendering meanwhile but is no longer the document's.
    {
        Transaction animate(t.duration, t.ease);
        for (const AttrDecl& edge : edgeState(t.exit)) {
            applyAttr(*layer, edge.spec->id, edge.value);
        }
    }
    layer->removeWhenSettled();
    return true;
}

std::vector<std::string> CompiledScene::layerIds() const {
    std::vector<std::string> ids;
    for (const auto& [decl, layer] : nodes_) {
        if (!decl->id.empty()) {
            ids.push_back(decl->id);
        }
    }
    return ids;
}

bool CompiledScene::hasLayer(const std::string& id) const {
    for (const auto& [decl, layer] : nodes_) {
        if (!id.empty() && decl->id == id) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// compile()
// ---------------------------------------------------------------------------

CompileResult compile(const SceneDoc& doc, const CompileOptions& options) {
    CompileResult result;
    if (doc.schema != kSchemaId) {
        result.diagnostics.add("compile.rejected", Severity::kError, Phase::kCompile, Span{},
                               "document was not validated (schema is empty or unsupported); "
                               "compile only parseScene results with ok()");
        return result;
    }
    auto scene = CompileAccess::create();
    CompileAccess::doc(*scene) = doc;  // own copy: nodes_ points into it
    CompileAccess::digestRef(*scene) = digest(doc);
    Builder builder(*scene, CompileAccess::doc(*scene), options, result.diagnostics);
    if (!builder.build()) {
        return result;
    }
    result.scene = std::move(scene);
    return result;
}

}  // namespace fluent_scene::fvs
