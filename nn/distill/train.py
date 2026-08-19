#!/usr/bin/env python3
"""Distill Informative Drawings (MIT) into a 105k-param line net.
Design doc: docs/design/nn_distill_lines.ja.md — read it first.

  python train.py --data /path/to/images --steps 20000
  python train.py --data ./coco/val2017 --steps 20000 --out runs/v1

Teacher runs via onnxruntime (CUDA if available); student is PyTorch.
"""
import argparse, os, glob, random, time
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image
import onnxruntime as ort

TEACHER_URL = 'https://takatronix.github.io/fluent_scene/models/id_lineart.onnx'


class TinyLines(nn.Module):
    """Frozen architecture — see design doc §3. Change = re-measure speed."""
    def __init__(self):
        super().__init__()
        a = lambda: nn.LeakyReLU(0.2, inplace=True)
        self.e1 = nn.Sequential(nn.Conv2d(3, 24, 3, 2, 1), a())
        self.e2 = nn.Sequential(nn.Conv2d(24, 48, 3, 2, 1), a())
        self.body = nn.Sequential(*[
            nn.Sequential(nn.Conv2d(48, 48, 3, 1, d, dilation=d), a())
            for d in (1, 2, 4, 1)])
        self.d1 = nn.Sequential(nn.Conv2d(48, 24, 3, 1, 1), a())
        self.out = nn.Conv2d(24, 1, 3, 1, 1)

    def forward(self, x, return_logits=False):
        x = self.e2(self.e1(x))
        x = x + self.body(x)
        x = F.interpolate(x, scale_factor=4, mode='bilinear', align_corners=False)
        z = self.out(self.d1(x))
        return z if return_logits else torch.sigmoid(z)


def sobel(x):
    kx = torch.tensor([[1, 0, -1], [2, 0, -2], [1, 0, -1]], dtype=x.dtype,
                      device=x.device).view(1, 1, 3, 3)
    return torch.cat([F.conv2d(x, kx, padding=1),
                      F.conv2d(x, kx.transpose(2, 3), padding=1)], 1)


def load_teacher(path):
    if not os.path.exists(path):
        import urllib.request
        print('downloading teacher…')
        urllib.request.urlretrieve(TEACHER_URL, path)
    providers = ['CUDAExecutionProvider', 'CPUExecutionProvider']
    s = ort.InferenceSession(path, providers=providers)
    print('teacher EP:', s.get_providers()[0])
    return s


def batch(files, n, crop):
    out = np.empty((n, 3, crop, crop), np.float32)
    for i in range(n):
        while True:
            try:
                im = Image.open(random.choice(files)).convert('RGB')
                break
            except Exception:
                pass
        sc = random.uniform(0.6, 1.4)
        w, h = (max(crop, int(im.width * sc)), max(crop, int(im.height * sc)))
        im = im.resize((w, h))
        x0, y0 = random.randint(0, w - crop), random.randint(0, h - crop)
        im = im.crop((x0, y0, x0 + crop, y0 + crop))
        if random.random() < 0.5:
            im = im.transpose(Image.FLIP_LEFT_RIGHT)
        out[i] = np.asarray(im, np.float32).transpose(2, 0, 1) / 255.0
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', required=True, help='directory of images')
    ap.add_argument('--steps', type=int, default=20000)
    ap.add_argument('--batch', type=int, default=16)
    ap.add_argument('--crop', type=int, default=384)
    ap.add_argument('--lr', type=float, default=2e-3)
    ap.add_argument('--loss', choices=['l1', 'bce'], default='bce',
                    help='pixel term; l1 on sigmoid collapses to all-white (see design doc)')
    ap.add_argument('--out', default='runs/v1')
    ap.add_argument('--teacher', default='id_lineart.onnx')
    args = ap.parse_args()

    files = [f for e in ('jpg', 'jpeg', 'png') for f in
             glob.glob(os.path.join(args.data, f'**/*.{e}'), recursive=True)]
    assert files, f'no images under {args.data}'
    print(len(files), 'images')
    os.makedirs(args.out, exist_ok=True)

    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    teacher = load_teacher(args.teacher)
    t_in = teacher.get_inputs()[0].name
    net = TinyLines().to(dev)
    print(sum(p.numel() for p in net.parameters()), 'params')
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.steps)

    val = batch(files, 4, args.crop)
    val_t = teacher.run(None, {t_in: val})[0]

    t0 = time.time()
    for step in range(1, args.steps + 1):
        x = batch(files, args.batch, args.crop)
        with torch.no_grad():
            y_t = torch.from_numpy(teacher.run(None, {t_in: x})[0]).to(dev)
        x = torch.from_numpy(x).to(dev)
        z = net(x, return_logits=True)
        y = torch.sigmoid(z)
        px = (F.binary_cross_entropy_with_logits(z, y_t) if args.loss == 'bce'
              else F.l1_loss(y, y_t))
        loss = px + 0.5 * F.l1_loss(sobel(y), sobel(y_t))
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
        if step % 200 == 0:
            print(f'{step:6d}  loss {loss.item():.4f}  '
                  f'{(time.time() - t0) / step:.2f}s/step', flush=True)
        if step % 5000 == 0 or step == args.steps:
            with torch.no_grad():
                y_v = net(torch.from_numpy(val).to(dev)).cpu().numpy()
            tiles = []
            for i in range(len(val)):
                row = np.concatenate([val[i].transpose(1, 2, 0),
                                      np.repeat(val_t[i].transpose(1, 2, 0), 3, 2),
                                      np.repeat(y_v[i].transpose(1, 2, 0), 3, 2)], 1)
                tiles.append(row)
            img = (np.clip(np.concatenate(tiles, 0), 0, 1) * 255).astype(np.uint8)
            Image.fromarray(img).save(f'{args.out}/val_{step}.png')
            torch.save(net.state_dict(), f'{args.out}/student.pt')
    print('done →', args.out)


if __name__ == '__main__':
    main()
