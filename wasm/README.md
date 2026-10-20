# fluent_scene / wasm — the browser build (Phase W1)

The same C++ that runs on the robot, compiled to WebAssembly: Scene
documents compile and render **inside the page**, so pointer feedback has
zero network latency.

```bash
source ~/emsdk/emsdk_env.sh
./wasm/build.sh --webgpu --webgl   # what Pages serves: Japanese font + WebGPU + WebGL2 (~7.2 MB, ~2.2 MB brotli)
./wasm/build.sh --webgpu   # Japanese font + WebGPU only (~5.6 MB)
./wasm/build.sh            # CPU renderer only (~2.7 MB)
./wasm/build.sh --latin    # DejaVuSans instead of the Japanese font (~2.4 MB, Latin only)
./wasm/build.sh --cjk      # the whole NotoSansCJK collection (~21 MB)
python3 -m http.server -d wasm/dist 8792   # then open /index.html
```

- `api.cpp` — the flat C surface (create/feed/pointer/render), usable from
  any wasm host, not just the bundled page.
- `demo.html` — webcam water demo: capture, refraction ripple, five live
  filter tiles, HUD, all local.
- Parity: a scene rendered in node matches the native CPU renderer to
  within 1 LSB (float rounding across ISAs).

## WebGL2 — the GPU where WebGPU hands out no adapter

Some browsers expose `navigator.gpu` but `requestAdapter()` resolves to
null (Chromium on Linux without Vulkan — the Tesla's browser, for one) while
WebGL2 runs on the GPU. `--webgl` compiles `src/webgl_renderer.cpp` in: the
WebGPU backend's draws, recorded and replayed on WebGL2, its shaders the same
GLSL through naga as GLSL ES 3.00. Synchronous, so no polling:

```js
const ok = cw('fs_webgl_init', 'number', ['string'])('#view') === 1;   // a canvas without another context
cw('fs_webgl_info', 'string', [])();   // GL renderer — "SwiftShader" / "llvmpipe" = software: stay on the CPU
const px = cw('fs_webgl_read', 'number', ['number', 'number', 'number', 'number'])(inst, 0, w, h);  // self-test
cw('fs_render_webgl', 'number', ['number', 'number', 'number', 'number'])(inst, dt, w, h);         // per frame
cw('fs_webgl_status', 'number', [])();  // -1 after a context loss
```

Needs `EXT_color_buffer_float` (every WebGL2 on a real GPU). Programs
compile on first use (the filter program is big; a scene without filters
never compiles it). Check: `webgl.html` (CPU vs WebGL2 per pixel).

## Driving a scene from JS — params, text, layers

Everything is a flat C call; wrap what you use with `cwrap`. Strings are
UTF-8, returns are `1` (applied) or `0` (rejected — `fs_error()` for
`fs_create` / `fs_add_layers`, `fs_diagnostics(inst)` for the setters).

```js
const cw = (n, r, a) => mod.cwrap(n, r, a);
const fs = {
  create:       cw('fs_create', 'number', ['string']),
  error:        cw('fs_error', 'string', []),
  diagnostics:  cw('fs_diagnostics', 'string', ['number']),
  setF32:       cw('fs_set_param_f32', 'number', ['number', 'string', 'number']),
  setVec2:      cw('fs_set_param_vec2', 'number', ['number', 'string', 'number', 'number']),
  setColor:     cw('fs_set_param_color', 'number', ['number', 'string', 'number', 'number', 'number', 'number']),
  setRect:      cw('fs_set_param_rect', 'number', ['number', 'string', 'number', 'number', 'number', 'number']),
  setText:      cw('fs_set_text', 'number', ['number', 'string', 'string']),
  layerSetText: cw('fs_layer_set_text', 'number', ['number', 'string', 'string']),
  addLayers:    cw('fs_add_layers', 'number', ['number', 'string', 'string', 'number']),
  removeLayer:  cw('fs_remove_layer', 'number', ['number', 'string', 'number']),
  layerIds:     cw('fs_layer_ids', 'string', ['number']),
  hasLayer:     cw('fs_has_layer', 'number', ['number', 'string']),
};
```

| call | does |
|---|---|
| `fs_set_param_f32 / _vec2 / _bool (inst, name, …)` | write a declared param; bound attributes follow, through its `animate` or the layer's `transition` |
| `fs_set_param_color(inst, name, r, g, b, a)` | a `color` param (0-1, straight α). Binds `background`, content `color`, `border.color`, … — colors interpolate |
| `fs_set_param_rect(inst, name, x, y, w, h)` | a `rect` param, for `frame` / `bounds` |
| `fs_set_text(inst, input, utf8)` | feed a `text.utf8` input (every text layer with `source: $inputs.<input>`) |
| `fs_layer_set_text(inst, layer_id, utf8)` | replace the text of one literal text layer by id |
| `fs_layer_set_text_style(inst, layer_id, map)` | restyle a text layer from a flow-YAML map of text fields (`'{ size: 32, weight: 700, outline_width: 3 }'`); fields not in the map keep their value |
| `fs_measure_text(inst, map)` / `fs_layer_text_metrics(inst, id)` | text metrics as JSON (`box`, `ink`, `baseline`, `ascent`, `descent`, `line_height`, `size`, `lines`, `truncated`, `line_widths`) — see [docs/api/text.ja.md](../docs/api/text.ja.md) |
| `fs_register_font(family, weight, italic, ptr, len)` / `fs_font_families()` | add a font from fetched bytes (`_malloc`ed, copied) / list families |
| `fs_add_layers(inst, parent_id, yaml, animated)` | graft a Scene fragment (`layers:` + optional `inputs:` / `params:`) under a declared layer (`""` = root); `animated` enters through `transition.enter` (default a fade) |
| `fs_remove_layer(inst, id, animated)` | remove a declared layer and its subtree; `animated` leaves through `transition.exit` first. The id is free at once |
| `fs_layer_ids(inst)` / `fs_has_layer(inst, id)` | declared ids, one per line in tree order / one check |
| `fs_diagnostics(inst)` | why the last setters returned 0, one per line (drains) |

`scale` binds an `f32` param (uniform) or a `vec2` param, so layout moves
are a param write, not a new document:

```yaml
params:
  zoom:  { type: f32, default: 1 }
  tint:  { type: color, default: [0.47, 0.59, 1, 1], animate: { duration: 0.5, ease: ease_in_out } }
  place: { type: rect, default: [360, 40, 1520, 1000] }
inputs:
  state: { type: text.utf8, update: on_change, fallback: hide }
layers:
  - id: figure
    position: [500, 500]
    scale: $params.zoom
    transition: { duration: 0.4, ease: ease_in_out }
    border: { width: 2, color: $params.tint }
  - id: media                                  # slot for content that comes and goes
    frame: $params.place
    transition: { duration: 0.35, ease: ease_out,
                  enter: { opacity: 0, scale: 0.92 }, exit: { opacity: 0 } }
  - content: { text: { source: $inputs.state, position: [24, 24], size: 28 } }
```

```js
fs.setF32(inst, 'zoom', 0.3);                       // the figure shrinks (0.4 s)
fs.setColor(inst, 'tint', 1, 0.7, 0.3, 1);          // …and turns amber (0.5 s)
fs.setText(inst, 'state', '考え中…');
fs.addLayers(inst, 'media', `
inputs: { photo: { type: image.rgba8, update: on_change, fallback: hide } }
layers:
  - id: photo
    content: { image: { source: $inputs.photo, fit: contain } }`, 1) || console.error(fs.error());
// then fs_image_buffer / fs_commit_image(inst, 'photo', w, h) as usual
fs.removeLayer(inst, 'photo', 1);                   // fades out, detaches when done
```

Design notes: [docs/design/runtime_layers.ja.md](../docs/design/runtime_layers.ja.md).
Headless checks: `node wasm/tests/stage_api.test.mjs`, `node wasm/tests/boxes_abi.test.mjs`,
`node wasm/tests/text_abi.test.mjs`.

## Fonts

The default family "sans" is the first file found per weight in the list
in `src/text/font_library.cpp` (`ensureDefaults`); `build.sh` embeds its
fonts at those paths. Bold text uses the Bold face when embedded and is
synthesized otherwise; more fonts load at runtime with `fs_register_font`.

| flag | font | covers |
|---|---|---|
| `--jp` (default) | `wasm/fonts/NotoSansJP-subset.otf` + `NotoSansJP-Bold-subset.otf` (1.0 MB each, committed; `--no-bold` drops the Bold) | 4,297 characters: kana (half-width too), the 2,965 JIS X 0208 level-1 kanji plus every jōyō and jinmeiyō kanji (3,298 kanji in all), CJK and full-width punctuation, JIS symbols (①, →, ℃ …), Greek, Cyrillic, ASCII, Latin-1 |
| `--latin` | the host's `DejaVuSans.ttf` + `-Bold` (1.4 MB) | Latin, Greek, Cyrillic; Japanese draws hollow boxes |
| `--cjk` | the host's `NotoSansCJK-Regular.ttc` (19 MB) | every CJK glyph |

The subset is cut from Noto Sans CJK JP Regular (SIL OFL 1.1). Its license
lives in `wasm/fonts/OFL.txt`, and every Noto build copies it to
`wasm/dist/OFL.txt`. Characters outside the subset, such as the other
~3,000 rare level-2 kanji or emoji, draw the renderer's hollow box. To
regenerate the subset (same inputs give a byte-identical file):

```bash
uv tool install fonttools                  # provides pyftsubset
python3 wasm/fonts/make_jp_subset.py       # reads the system NotoSansCJK, or --font PATH
```

Not yet here: WebGPU backend (W3 — shaders are already single-source
GLSL∩C++), the playground editor.
