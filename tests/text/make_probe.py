#!/usr/bin/env python3
"""Writes the text-quality probe scenes (tests/text/probe_*.fvs).

The same documents render on the CPU and Vulkan backends (fvsc preview)
and on WebGPU/wasm in headless Chromium (tests/text/probe_web.mjs), so a
before/after comparison always looks at identical inputs.
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [10, 14, 20, 26, 40, 72]
LINE = "Hamburgefonstiv 0123456789 あいうえお カタカナ 漢字表示速度"
BIG = "Ag8 漢あ"

def text(t, x, y, size, color, align="left"):
    c = ", ".join(f"{v:g}" for v in color)
    return (f"  - content: {{ text: {{ text: {json.dumps(t, ensure_ascii=False)}, "
            f"position: [{x:g}, {y:g}], size: {size:g}, align: {align}, color: [{c}] }} }}")

def sizes_scene(bg, fg):
    out = ["schema: fluent.scene/v1alpha2", "stage: { size: [2200, 1180] }", "layers:",
           f"  - content: {{ rect: {{ size: [2200, 1180], color: [{', '.join(map(str, bg))}] }} }}"]
    y = 12
    for s in SIZES:
        out.append(text(f"{s}px {LINE}", 12, y, s, fg))
        y += s * 1.5 + 6
    out.append(text("160px " + BIG, 12, y, 160, fg))
    out.append(text("300px " + BIG, 12, y + 230, 300, fg))
    return "\n".join(out) + "\n"

def transform_scene():
    out = ["schema: fluent.scene/v1alpha2", "stage: { size: [1200, 760] }", "layers:",
           "  - content: { rect: { size: [1200, 760], color: [0.07, 0.08, 0.1, 1] } }"]
    # rotated text at several sizes
    for i, (s, deg) in enumerate([(14, 8), (26, -12), (48, 20)]):
        out += [f"  - position: [{40 + i * 360}, {60 + i * 30}]", f"    rotation: {deg}",
                "    sublayers:", "  " + text(f"{s}px 回転 Rotate 42.5", 0, 0, s, [1, 1, 1, 1])]
    # scaled-up small text (layer scale) — the classic blurry case
    for i, sc in enumerate([2, 4, 8]):
        out += [f"  - position: [40, {230 + i * 150}]", f"    scale: [{sc}, {sc}]",
                "    sublayers:", "  " + text("12px×%d 速度 88" % sc, 0, 0, 12, [0.95, 0.9, 0.6, 1])]
    # live-number style (tabular digits case)
    for i, v in enumerate(["111.1", "888.8", "123.4"]):
        out.append(text(v, 1150, 300 + i * 130, 120, [0.4, 0.9, 1, 1], "right"))
    return "\n".join(out) + "\n"

def main():
    scenes = {
        "probe_dark": sizes_scene([0.06, 0.07, 0.09, 1], [0.95, 0.96, 0.98, 1]),
        "probe_light": sizes_scene([0.97, 0.96, 0.94, 1], [0.08, 0.08, 0.1, 1]),
        "probe_xform": transform_scene(),
    }
    for name, doc in scenes.items():
        with open(os.path.join(HERE, name + ".fvs"), "w") as f:
            f.write(doc)
    print(" ".join(scenes))

if __name__ == "__main__":
    main()
