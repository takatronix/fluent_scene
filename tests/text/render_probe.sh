#!/usr/bin/env bash
# Renders the text probe scenes on the CPU and Vulkan backends into PNGs:
#   tests/text/render_probe.sh <build-dir> <out-dir> <tag>
# (WebGPU/wasm: tests/text/probe_web.mjs.) Needs python3 + Pillow.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=$1 out=$2 tag=$3
mkdir -p "$out"
python3 "$here/make_probe.py" >/dev/null
for scene in probe_dark probe_light probe_xform; do
  for r in cpu vulkan; do
    "$build/fvsc" preview "$here/$scene.fvs" -o "$out/$scene.$r.ppm" --renderer=$r >/dev/null
    python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" \
      "$out/$scene.$r.ppm" "$out/$tag-$scene-$r.png"
    rm "$out/$scene.$r.ppm"
  done
done
ls "$out" | grep "^$tag-"
