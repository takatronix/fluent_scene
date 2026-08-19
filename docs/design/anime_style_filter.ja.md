# アニメ調AIフィルタ (設計書 draft-1)

status: **approved** — 2026-08-19 オーナー承認、Phase A 実装済 (anime.html)。
date: 2026-08-19

## 1. 動機と位置づけ

カメラ映像をアニメ調に変換する。GAN (数百万パラメータのCNN) は
filters_shared.h の「単一パス・GLSL∩C++」の枠に入らないため、
**推論はコア外**とする — 認識アナライザ設計 (analyzer design, 8/15) と
同じ原則。コアに推論エンジン (onnxruntime / TensorRT) は絶対に入れない。

エンジンから見ると、アニメ変換は「外部アナライザが作る full-frame 画像」
であり、既存の入力機構だけで受けられる。**コア改修は原則ゼロ**。

## 2. モデル

AnimeGANv3 (TachibanaYoshino) の公式 ONNX を採用。

| 項目 | 値 |
|---|---|
| モデル | AnimeGANv3_Hayao_36 / AnimeGANv3_Shinkai_37 (公式リリース v1.1.0) |
| サイズ | 各 4.2 MB |
| 入出力 | NHWC float32 RGB、x/127.5−1、H/W は8の倍数 (min 256)、動的形状。出力 [−1,1] |
| 実測 | Thor CPU 512²で ~900 ms (onnxruntime 1.29 CPU EP)。ブラウザ WebGPU / TensorRT で大幅短縮見込み |

### 2.1 ライセンス (要注意)

AnimeGANv3 は**非商用ライセンス** (研究・教育は無償、商用はメールで
許諾レター取得)。学習済み重みも同ライセンスなので MIT 移植を使っても
回避できない。README のライセンス表に **notebook (CC BY-NC-SA) と同じ
NC枠**として記載する。商用展開時の選択肢は (a) 許諾レター (b) 自前学習。
White-box Cartoonization 等の代替も NC で、状況は同じ。

**重みは wasm/dist/models/ に同梱する** (draft-1 の「同梱しない」から変更)。
実測で GitHub リリース資産は CORS ヘッダを返さず、ブラウザからの
実行時ダウンロードが不可能だった。HF に公式バイナリの完全ミラーも無い。
同一オリジン配置が唯一の配布経路であり、notebook フィルタ (CC BY-NC-SA
同梱) の前例に従い models/LICENSE.txt + THIRD_PARTY.md で NC を明記。
エンジンにはリンクしない (デモ資産のみ)。

## 3. フェーズ

### Phase A — ブラウザデモ (**実装済み・2026-08-19 = wasm/anime.html**)

gaze.html と同型のアナライザプロトタイプ。scratchpad で実証後、
wasm/anime.html として出荷。最終形は GAN 単体に加え、MediaPipe 自撮り
セグメンテーション (Apache-2.0) との JS 合成で「人物だけ / 背景だけ
アニメ」スコープを持つ — Phase C のマスク合成 mix(src, anime, mask) の
ブラウザ内先行実装。マスクは GAN と**同一キャプチャフレーム**に対して
取り、合成もそのフレームで閉じる (時刻一致ペアの原則)。

- onnxruntime-web 1.22 (CDN)、EP は webgpu → wasm フォールバック
  (?ep= で強制可)。COOP/COEP ヘッダで wasm マルチスレッド有効化。
- カメラ → 8の倍数に整形 (256/384/512 ラダー) → NHWC 変換 → 推論 →
  [−1,1]→RGBA → canvas。推論は busy フラグで直列化 (キュー深さ1、
  遅い EP では fps が落ちるだけで映像は止まらない)。
- 比較 UI: ドラッグで 原画|アニメ のスプリット。
- iOS: playsinline + ユーザージェスチャ起動 (46a29ad の教訓)。

wasm/ へ昇格する場合は anime.html とし、dist カード追加 → Pages 公開
(公開の掟に従う)。**モデルは jsDelivr / GitHub リリース直リンクで取得**
(dist に NC バイナリを置かない)。

### Phase B — エンジン合成 (コア改修ゼロ)

アニメ出力をシーンの **image 入力 (update: per_frame)** として注入する。

```yaml
inputs:
  camera: { type: image.rgba8, update: per_frame }
  anime:  { type: image.rgba8, update: per_frame }   # アナライザ出力
layers:
  - { content: { image: { source: $inputs.camera, fit: cover } } }
  - id: anime_overlay
    opacity: $params.anime_mix        # 0..1 でディゾルブ
    content: { image: { source: $inputs.anime, fit: cover } }
```

- 推論は非同期 (1〜2フレーム遅れ)。全画面スタイルは置換合成なので遅延は
  「絵が少し前の時刻」になるだけで破綻しない。カメラと厳密に重ねる
  クロスフェード時は、アナライザ側が変換元フレームを保持して
  anime と同時に camera へも流す (時刻一致ペア注入) のが正解。
- 鮮度: アナライザ停止時に最終フレームが凍り付くのを防ぐため、
  analyzer design の max_age をそのまま適用 (期限切れで anime_mix を
  0 へアニメート)。

### Phase C — マスク合成 (「人物だけアニメ」)

analyzer design Phase B のマスク基盤 (mix(src, filtered, mask)) が入れば、
anime 入力 × 人物セグマスクで部分アニメ化が組める。本設計からの追加要求
は無し — マスク基盤の仕様に従うだけ。

### Phase D — ネイティブ推論 (ロボット実機)

ml-hub 側ノードで TensorRT (Thor に libnvinfer 10.16 導入済) 推論、
ROS 2 image topic → fluent_scene ROS アダプターの image 入力へ。
コアから見ると Phase B と完全に同一。

## 4. 決定性 / テスト

推論はコア外なので golden テストの対象外。コアが使うのは既存の
image 入力経路のみで、新規テストは不要。Phase A デモは push 前
ヘッドレススモーク (fake camera + wasm EP) の運用に従う。

## 5. 非採用案

- **fs_apply への組み込み**: 単一ソース原則 (GLSL∩C++) と単一パス構造に
  GAN は載らない。却下。
- **永続バッファ機構 (persistent_buffers draft) への相乗り**: あれは
  シェーダーパス列の話で、外部推論とは別物。混ぜない。
- **コアへの onnxruntime リンク**: 依存爆発 + 全バックエンド移植不能。
  「推論はコア外」原則で却下。

## 6. 追加AIフィルタのロードマップ (ライセンス精査済み)

anime.html と同じ骨格 (アナライザ = ort-web / MediaPipe、合成 = per_frame
image / マスク) にそのまま載る候補。**優先はライセンスが商用クリーンな順**:

| 候補 | モデル | ライセンス | 合成形 |
|---|---|---|---|
| 画風変換 (mosaic/candy/udnie 等) | fast-neural-style (ONNX Model Zoo, ~6.5MB) | **BSD-3 (商用可)** | anime と同一 (image 置換) |
| 人物マット / 背景置換 | MediaPipe Selfie Segmentation | **Apache-2.0** | anime.html で導入済み |
| 顔スタイライズ (sketch/oil/cartoon) | MediaPipe FaceStylizer | **Apache-2.0** | 顔 ROI のみ image 合成 |
| 深度→本物のボケ | Depth Anything V2 Small (~25MB) | **Apache-2.0** | 深度マップ→既存 bokeh の強度マスク |
| 高精細マット | MODNet | **Apache-2.0** | セグの上位互換 (髪の毛レベル) |
| アニメ調 (本設計) | AnimeGANv3 | 非商用 | 実装済み |

注意: RVM (Robust Video Matting) は GPL-3 のため**コード・重みとも不使用**
(ライセンス規範)。新規候補は必ずこの表に追記してから着手する。
