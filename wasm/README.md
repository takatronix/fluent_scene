# fluent_scene / wasm — the browser build (Phase W1)

The same C++ that runs on the robot, compiled to WebAssembly: Scene
documents compile and render **inside the page**, so pointer feedback has
zero network latency.

```bash
source ~/emsdk/emsdk_env.sh
./wasm/build.sh            # DejaVuSans embedded (~2.3 MB module)
./wasm/build.sh --cjk      # NotoSansCJK for Japanese text (~21 MB)
python3 -m http.server -d wasm/dist 8792   # then open /index.html
```

- `api.cpp` — the flat C surface (create/feed/pointer/render), usable from
  any wasm host, not just the bundled page.
- `demo.html` — webcam water demo: capture, refraction ripple, five live
  filter tiles, HUD, all local.
- Parity: a scene rendered in node matches the native CPU renderer to
  within 1 LSB (float rounding across ISAs).

Not yet here: WebGPU backend (W3 — shaders are already single-source
GLSL∩C++), Japanese-capable small font subset, the playground editor.
