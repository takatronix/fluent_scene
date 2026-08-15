# 永続バッファ付きフィルタ (設計書 draft-1)

status: **draft** — 設計凍結までコード禁止 (品質規範)。オーナーレビュー待ち。
date: 2026-08-16

## 1. 動機 — 今のフィルタ機構では書けないもの

現行のフィルタは「単一パス・無状態・チェーン可」(filters_shared.h の
fs_apply)。FS_BLUR だけが各レンダラー内蔵の2パス特例。この枠に入らない
要望が既に4つ溜まっている:

| 要望 | 必要な能力 |
|---|---|
| lowpoly (JFA Delaunay, shadertoy ldV3Wc) | 中間バッファ4枚 + パス列 (seed→JFA×n→三角形抽出→塗り) + **前フレーム保持** |
| reaction-diffusion (Gray-Scott, Shane/Flexi系) | **ピンポン・フィードバック** (前フレームの自分を読む) + ブラー中間 |
| CRT残光 (フォスフォー減衰, guest-advanced の afterglow) | 前フレーム出力の減衰合成 1枚 |
| VHSトラッキング歪み蓄積 | 同上 |

共通項は2つだけ: **(A) フレームをまたいで生きる名前付きオフスクリーン**、
**(B) 1フィルタ=複数パスの宣言**。

## 2. 設計方針

単一ソース原則 (GLSL∩C++、機械翻訳WGSL) は崩さない。各パスの本体は今まで
どおり filters_shared.h の1関数。増えるのは**宣言と配線**だけ。

### 2.1 宣言 (filters_def.h の拡張)

```
FS_FILTER_STATEFUL(ReactionDiffusion, reaction_diffusion, FS_RD,
                   "Gray-Scott reaction-diffusion")
FS_STATE(0, field, 1.0f)          // 永続バッファ: 名前と解像度スケール
FS_PASS(0, fs_rd_step,  field)    // 本体関数と書き込み先 (fieldへ)
FS_PASS(1, fs_rd_shade, OUTPUT)   // 最終パスはレイヤ出力へ
FS_PARAM(0, feed, 0.037f, Scalar)
...
FS_END(ReactionDiffusion)
```

- `FS_STATE(slot, name, scale)` — レイヤ毎にレンダラーが確保・保持する
  RGBA16F オフスクリーン。scale は元解像度比 (JFAは1.0、ブラー用は0.5等)。
- `FS_PASS(order, body_fn, target)` — 実行順と書き込み先。target が state
  名ならピンポン (読み=前回分/書き=今回分をレンダラーが自動スワップ)。
- 本体側は `FS_SAMPLE_STATE(slot, uv)` マクロを追加で使える。未定義の
  バックエンドではパススルー (FS_SAMPLE_LUT と同じ compile-out 方式)。

### 2.2 レンダラー側

- **所有権**: 永続バッファは Layer に紐付け (filters_ 変更・clearFilters で
  破棄)。CPU/Vulkan/WebGPU 各レンダラーが `layer id → state表` を持つ。
- **ピンポン**: state への書き込みパスがあるフレームは read/write の2面を
  スワップ。初回フレームは0クリア(JFA/RDはシード注入パスが自分で埋める)。
- **リサイズ**: 元レイヤの解像度変更で破棄→再確保→0クリア。

### 2.3 決定性 (golden との整合)

- 状態は `render(stage, dt)` の呼び出し回数だけで進む (実時間不使用)。
  golden テストは「N回renderして最終フレーム比較」形式を追加する
  (`checkSceneAfter(name, frames)`)。既存の byte-reproducible 原則は
  「同じ呼び出し列なら同じ画」に一般化される。
- 乱数はこれまで通り座標ハッシュ/vnoiseのみ (Date/rand禁止)。

### 2.4 WGSL/naga 経路

各パスは従来と同じ単一パス fragment なので機械翻訳経路は無変更。
増えるのはパイプライン配線 (webgpu_renderer のパス実行ループ) のみ。

## 3. 実装フェーズ

1. **Phase P1 — ピンポン1枚** (最小・価値最大): FS_STATE 1枚 + FS_PASS 2本
   まで。reaction-diffusion と CRT残光 (crt の afterglow パラメータ追加) を
   これで出す。
2. **Phase P2 — 多バッファ+可変パス数**: JFA lowpoly (状態2枚+パス5本、
   JFA反復はパス内ループで畳める: 8ステップ固定)。
3. **Phase P3 — ジェネレータの一般化**: 入力レイヤ無しの手続きシーン
   (kali系はfractal=39で実装済のため必要になったら)。

## 4. ライセンスメモ

- JFA: Rong & Tan 2006 の公知アルゴリズム。実装は自前 (ldV3Wc のコードは
  CC BY-NC-SA なので参照のみ)。
- Gray-Scott: 公知の反応拡散方程式。Flexi/Shane のコードは参照のみ。
- 残光: 指数減衰合成のみ (自明)。

## 5. 未決事項 (レビューで決めたい)

- FS_STATE の画素形式を RGBA16F 固定にするか (JFAは座標格納に精度必須。
  CPU参照はfloat配列なので常に32F、GPUだけ16F だと golden がズレる →
  **32F固定を提案**、メモリは1080pで8MB/枚)。
- Scene YAML への露出 (`filter: {type: reaction_diffusion, ...}` は自動で
  出るが、状態リセットの明示API `layer.resetFilterState()` が要るか)。
- stage_web の30フィルタ同時プレビューとの整合 (状態持ちフィルタは
  タイル毎に独立状態で問題ないか → 問題ない想定だがメモリ×タイル数)。
