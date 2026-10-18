# fluent_scene / wasm — the browser build (Phase W1)

The same C++ that runs on the robot, compiled to WebAssembly: Scene
documents compile and render **inside the page**, so pointer feedback has
zero network latency.

```bash
source ~/emsdk/emsdk_env.sh
./wasm/build.sh --webgpu   # what Pages serves: Japanese font + WebGPU (~3.8 MB, ~1.5 MB gzipped)
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

## Fonts

The renderer shapes text with one face: the first file it finds in
`kFontSearch` (`src/text_atlas.cpp`). `build.sh` embeds one font at one of
those paths:

| flag | font | covers |
|---|---|---|
| `--jp` (default) | `wasm/fonts/NotoSansJP-subset.otf` (1.0 MB, committed) | 4,297 characters: kana (half-width too), the 2,965 JIS X 0208 level-1 kanji plus every jōyō and jinmeiyō kanji (3,298 kanji in all), CJK and full-width punctuation, JIS symbols (①, →, ℃ …), Greek, Cyrillic, ASCII, Latin-1 |
| `--latin` | the host's `DejaVuSans.ttf` (0.75 MB) | Latin, Greek, Cyrillic; Japanese draws hollow boxes |
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
