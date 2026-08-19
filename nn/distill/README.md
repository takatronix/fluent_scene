# nn/distill — 極小線画NNの蒸留

設計書: [docs/design/nn_distill_lines.ja.md](../../docs/design/nn_distill_lines.ja.md) を先に読むこと。

教師 = Informative Drawings (**MIT**)。蒸留した重みは自作＝MITで配布可。
非商用モデル (AnimeGANv3系) を教師にしないこと。

## 手順 (4090想定, ~30分)

```bash
pip install torch onnx onnxruntime-gpu onnxconverter-common pillow numpy

# データ: 何でもよい画像5000枚。COCO val2017 の例:
curl -LO http://images.cocodataset.org/zips/val2017.zip && unzip -q val2017.zip

python train.py --data ./val2017 --steps 20000 --out runs/v1
# runs/v1/val_*.png で教師(中列)と生徒(右列)を目視比較

python export_onnx.py --ckpt runs/v1/student.pt --out tiny_lines
# 受け入れ: 教師と目視同等 / onnx動的HxW / fp16 max|Δ|<0.02

cp tiny_lines.onnx tiny_lines_fp16.onnx ../../wasm/dist/models/
# → wasm/dist/models/LICENSE.txt に「tiny_lines = 自家製MIT」を追記し、
#   wasm/anime.html のスタイルに追加 (modelKind='lines' の分岐がそのまま使える。
#   value を models/tiny_lines.onnx にするだけ)、dist へコピーして push
```

runs/ の中間チェックポイントはコミットしない (.gitignore 済)。
