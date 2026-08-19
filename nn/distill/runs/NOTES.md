# 蒸留実行記録 (2026-08-20, rtx4090マシン)

設計書: docs/design/nn_distill_lines.ja.md / 実行: Claude (Opus 5)

## 結果: 受け入れ基準 3/3 合格

- 成果物: `tiny_lines.onnx` (fp32, 423KB) / `tiny_lines_fp16.onnx` (215KB ≈設計予測200KB)
- opset17・動的HxW (384×512 / 256×456 で確認)・単一ファイル・全ノード命名済
- torch vs onnx max|Δ| 6e-7 / fp32 vs fp16 max|Δ| 0.0011 (<0.02、実画像4枚)
- CPU 512² (このマシン): fp32 8.2ms (マルチ) / 84ms (1スレッド) — 基準200ms余裕
  (Thorは未再測。構造はThor実測116msのものと同一なので据え置き有効)
- 目視: runs/v2/val_20000.png (入力|教師|生徒) と
  runs/v2/acceptance_fp32_fp16.png (入力|教師|fp32|fp16) を参照。
  主要輪郭欠落なし・偽線なし。教師よりタッチが軽い「薄めの鉛筆」様式

## 重要な発見: 設計書レシピそのままでは白紙崩壊する

v1 (L1+sobel, lr2e-3, 設計書どおり) は step200 で既に「全画素=紙」に崩壊し
20kまで一度も脱出しなかった (生徒出力 min 1.000)。線は画素の~3%しかなく
「常に白」が強い局所解 + sigmoid飽和で勾配消失が真因。

1500step×3案のベイクオフ:

| 案 | pixel項 | lr | 教師インク画素での生徒平均 | 判定 |
|---|---|---|---|---|
| A | L1 (設計書) | 5e-4 | 1.000 (完全死) | lr減では救えない |
| B | BCE(ロジット) | 2e-3 | 0.787 | ✓ 採用 |
| C | BCE(ロジット) | 1e-3 | 0.760 | ✓ |

対処 = pixel項を L1 → BCE(ロジット直結、飽和しても勾配 p−t で生きる) に
変更 (train.py --loss bce が新既定)。sobel項・構造・optimizer・cosineは設計どおり。
ONNXグラフは不変 (sigmoidは推論グラフに残る)。

## v2 本番 (採用モデル)

- 20k steps, batch16, crop384, BCE+0.5*sobelL1, AdamW 2e-3 cosine, COCO val2017 5000枚
- 48分 (0.15s/step, データローダ律速でGPU util ~70%)
- 最終 train loss 0.1937 / 固定val4枚: 教師との L1 0.0430
  (推移 5k:0.0468 → 10k:0.0445 → 15k:0.0427 → 20k:0.0430)
- チェックポイント: runs/v2/student.pt (425KB)

## エクスポータの罠 (export_onnx.py に修正済)

- torch≥2.9 の新(dynamo)エクスポータは opset_version=17 を無視して 18 を出し、
  重みを .onnx.data に分離する → `dynamo=False` で旧経路に固定
- 旧経路+onnxconverter_common の fp16 変換は設計書どおり無名ノード命名が必要
