#!/usr/bin/env bash
# Builds fluent_scene for the browser (Phase W1): the CPU renderer, the
# Scene document layer, and the flat C API, as an ES module.
#
#   source ~/emsdk/emsdk_env.sh && ./wasm/build.sh [--cjk]
#
# --cjk embeds NotoSansCJK (~19 MB) for Japanese text; the default embeds
# DejaVuSans (~750 KB, Latin) to keep the module small. The font lands on
# the virtual FS at a path the renderer's candidate list already probes.
set -euo pipefail
cd "$(dirname "$0")/.."

FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
if [[ "${1:-}" == "--cjk" ]]; then
  FONT=/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
fi

mkdir -p wasm/dist
em++ -O2 -std=c++17 -I include \
  src/stage.cpp src/text_atlas.cpp src/cpu_renderer.cpp src/ui.cpp src/effects.cpp \
  src/fvs/diagnostics.cpp src/fvs/sha256.cpp src/fvs/yaml_parser.cpp \
  src/fvs/schema.cpp src/fvs/parser.cpp src/fvs/writer.cpp src/fvs/compiler.cpp \
  src/fvs/describe.cpp src/fvs/linter.cpp src/fvs/inspector.cpp \
  wasm/api.cpp \
  --use-port=freetype --use-port=harfbuzz \
  --embed-file "$FONT@$FONT" \
  -sALLOW_MEMORY_GROWTH=1 \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createFluentScene \
  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8,stringToNewUTF8 \
  -sENVIRONMENT=web,node \
  -o wasm/dist/fluent_scene.mjs
ls -la wasm/dist/
