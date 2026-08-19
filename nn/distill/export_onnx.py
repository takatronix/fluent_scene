#!/usr/bin/env python3
"""Export the trained student to ONNX (dynamic HxW) + fp16, verify both.

  python export_onnx.py --ckpt runs/v1/student.pt --out tiny_lines
"""
import argparse
import numpy as np
import torch
import onnx
import onnxruntime as ort
from train import TinyLines

ap = argparse.ArgumentParser()
ap.add_argument('--ckpt', required=True)
ap.add_argument('--out', default='tiny_lines')
ap.add_argument('--harden', type=float, default=0.0,
                help='contrast gain baked after the sigmoid, for the near-binary '
                     'ink styles. The student\'s greys are its uncertainty about '
                     'which side of a hard edge a pixel falls on; re-deciding '
                     'that with clip((v-0.5)*k+0.5) is reading the distilled '
                     'answer out, not cosmetics. Measured on the ink model: '
                     'mid-greys 27%% -> 8%%, against a target of 8.3%%. Leave 0 '
                     'for the pencil styles, whose greys are real tone.')
args = ap.parse_args()

net = TinyLines()
net.load_state_dict(torch.load(args.ckpt, map_location='cpu'))
net.eval()

if args.harden:
    class Hardened(torch.nn.Module):
        def __init__(self, net, k):
            super().__init__()
            self.net, self.k = net, k

        def forward(self, x):
            return ((self.net(x) - 0.5) * self.k + 0.5).clamp(0, 1)

    net = Hardened(net, args.harden).eval()

x = torch.rand(1, 3, 384, 512)
torch.onnx.export(net, x, f'{args.out}.onnx', opset_version=17,
                  dynamo=False,  # new exporter ignores opset 17 (emits 18) + splits weights to .data
                  input_names=['input'], output_names=['lines'],
                  dynamic_axes={'input': {2: 'h', 3: 'w'},
                                'lines': {2: 'h', 3: 'w'}})

# newer torch exporters may split weights into <out>.onnx.data — wasm needs
# a single self-contained file, so inline them back
import os
if os.path.exists(f'{args.out}.onnx.data'):
    m = onnx.load(f'{args.out}.onnx')          # pulls external data into memory
    onnx.save(m, f'{args.out}.onnx')           # re-save inline
    os.remove(f'{args.out}.onnx.data')

# verify fp32 onnx vs torch at two sizes
for hw in [(384, 512), (256, 456)]:
    xin = np.random.rand(1, 3, *hw).astype(np.float32)
    with torch.no_grad():
        y_t = net(torch.from_numpy(xin)).numpy()
    s = ort.InferenceSession(f'{args.out}.onnx', providers=['CPUExecutionProvider'])
    y_o = s.run(None, {'input': xin})[0]
    print(hw, 'torch vs onnx max|Δ|', float(np.abs(y_t - y_o).max()))

# fp16 — TRAP: name every node BEFORE converting, or cast tensor names collide
from onnxconverter_common import float16
m = onnx.load(f'{args.out}.onnx')
for i, n in enumerate(m.graph.node):
    if not n.name:
        n.name = f'{n.op_type}_anon_{i}'
m16 = float16.convert_float_to_float16(m, keep_io_types=True)
onnx.save(m16, f'{args.out}_fp16.onnx')
s16 = ort.InferenceSession(f'{args.out}_fp16.onnx', providers=['CPUExecutionProvider'])
xin = np.random.rand(1, 3, 384, 512).astype(np.float32)
a = ort.InferenceSession(f'{args.out}.onnx', providers=['CPUExecutionProvider']).run(None, {'input': xin})[0]
b = s16.run(None, {'input': xin})[0]
print('fp16 max|Δ|', float(np.abs(a - b).max()), '(expect < 0.02)')
print('wrote', f'{args.out}.onnx', f'{args.out}_fp16.onnx')
