# 極小線画NN蒸留 (設計書 draft-1)

status: **approved-direction** — オーナー指示「もっと高速なNNを自分で作れないか」
「4090マシンで続きを」(2026-08-20)。実行は rtx4090 マシン上の Claude が担当。
date: 2026-08-20

## 1. 目的と根拠

anime.html の線画NN (Informative Drawings, 4.3M params) は仕事(局所的な
線抽出)に対して過剰に重い。Thor CPU 512²実測:

| ネット | params | 512² |
|---|---|---|
| 教師: Informative Drawings | 4.3M | 1136 ms |
| **生徒 (本設計・未学習の構造実測)** | **105k** | **116 ms (≈10倍)** |

fp16重み ≈ 200KB。M3 Ultra WebGPU で 10–30ms/frame = 30fps級の見込み。

## 2. ライセンス (最重要制約)

- 教師は **Informative Drawings (MIT)** のみ。蒸留した重みは自作＝**MIT
  で配布可**。商用クリアなNNノード第1号(自家製)になる
- **AnimeGANv3系(非商用)を教師にした蒸留は本線ではやらない** (NC汚染の
  グレーゾーン)。やる場合は個人用と明記し配布しない
- 学習画像は入力としてしか使わない(重みに転写されるのは教師の様式)。
  公開データセットは COCO 等の再配布可のものを使う

## 3. 生徒ネット構造 (凍結)

Thor実測に使った構造そのまま。変更する場合は速度を再実測してから。

```
input  1x3xHxW (RGB 0..1)
e1: Conv 3->24  k3 s2, LeakyReLU(0.2)
e2: Conv 24->48 k3 s2, LeakyReLU(0.2)      # skip保存
b0..b3: Conv 48->48 k3, dilation 1,2,4,1, 各LeakyReLU(0.2)
Add(skip)
Resize x4 (bilinear, asymmetric)
d1: Conv 48->24 k3, LeakyReLU(0.2)
out: Conv 24->1 k3, Sigmoid                # 線強度 0..1 (1=紙, 0=墨)
```

## 4. 学習レシピ

- 教師: `https://takatronix.github.io/fluent_scene/models/id_lineart.onnx`
  (onnxruntime CUDAExecutionProvider で targets をオンザフライ生成。
  入力 NCHW 0..1、出力 1ch 0..1)
- データ: 画像ディレクトリ (COCO val2017 の5000枚で開始。人物・室内・
  屋外が混ざっていれば何でもよい。手持ち写真の追加歓迎)
- 前処理: ランダムクロップ 384²(8の倍数)、ランダムスケール 0.6–1.4、
  左右フリップ。教師も同じクロップに適用
- ロス: `L1(student, teacher) + 0.5 * L1(sobel(student), sobel(teacher))`
  (線のシャープさは勾配項が担う)
- 最適化: AdamW lr 2e-3 → cosine decay、batch 16、20k steps
  (4090 で ~20–30分)。fp32 学習で十分
- 収束確認: 5k steps 毎に固定検証画像(ポートレート+風景 各2枚)の
  side-by-side を保存

## 5. 受け入れ基準

1. 検証画像で教師出力と目視同等 (線の欠落・偽線が気にならないこと)
2. 512² CPU 推論 ≤ 200ms (Thor基準) / ONNX opset17・動的HxW
3. fp16 変換後も目視同等 (**罠: 変換前に全ノードへ一意名を付けること** —
   無名ノードがあると cast テンソル名が衝突し壊れたグラフになる)

## 6. 成果物と納品

- `nn/distill/` の学習スクリプトで `tiny_lines.onnx` / `tiny_lines_fp16.onnx`
  を生成し、`wasm/dist/models/` に置いて push (LICENSE.txt に MIT 自家製と
  追記)。anime.html のスタイルに「tiny pencil (ours, MIT)」を追加
- 学習ログ(最終loss・検証画像)を nn/distill/runs/ に残す(重い中間
  チェックポイントはコミットしない)

## 7. 発展 (本設計の範囲外)

- 同じ生徒構造でインク風(2値太線)を自前後処理 or 自前教師で
- スタイル可変(太さ条件付け): 条件チャネル追加は v2 で
