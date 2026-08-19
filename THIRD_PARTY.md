# Third-party notices & attribution / 出典・ライセンス一覧

fluent_scene 本体は **MIT** (© 2026 takatronix)。ただし以下の例外・系譜・
同梱物があります。**商用利用の可否はこの表が正**です。

## ⚠️ 非商用の例外 (この2件のみ)

| 対象 | 由来 | ライセンス | 備考 |
|---|---|---|---|
| `notebook` フィルタ (`FS_NOTEBOOK`) | flockaroo "notebook drawings" ([shadertoy XtVGD1](https://www.shadertoy.com/view/XtVGD1)) の移植 | **CC BY-NC-SA 3.0 — 非商用限定** | 商用ビルドに含めないこと。他の全フィルタは MIT |
| AnimeGANv3 学習済み重み (`wasm/dist/models/*.onnx`: JP_face / PortraitSketch + fp16版) | [TachibanaYoshino/AnimeGANv3](https://github.com/TachibanaYoshino/AnimeGANv3) (HFミラー経由)。重み無改変 (fp16版は丸めのみ)・グラフのみ機械変換 (opset9→17、数値照合済) | **非商用限定** (商用は作者の許諾レター要) | anime.html デモ専用アセット。エンジンにはリンクしない。詳細 `wasm/dist/models/LICENSE.txt` |

## 独自実装だが系譜を明記するもの (すべて MIT)

| フィルタ | 技術系譜 (コード借用なし・論文/公知技術からの独自実装) |
|---|---|
| `ntsc` | MAME ntsc.fx (BSD-3) の単一パス構造 + ntsc-adaptive のクロスフィード定式 |
| `crt` | Timothy Lottes (Public Domain)、Cathode-Retro (MIT) の系譜 |
| `oilpaint` | Kuwahara et al. 1976 |
| `anime` | Winnemöller 2006 (Real-Time Video Abstraction) / XDoG (Winnemöller 2011) |
| `watercolor` | Bousseau et al. 2006 (顔料密度モデル) |
| `sumie` | Zhang 1999 系のインク拡散を1ギャザー化 (オーナー旧作方式準拠) |
| `impressionist` | Litwinowicz 1997 / Hertzmann 1998 のギャザー型再定式化 |
| `inkline` | Kang et al. 2007 (Coherent Line Drawing, ETF/FDoG) |
| `stainedglass` | Worley 1996 (cellular noise) |
| `pixelart` | Bayer ordered dithering (公知) |
| `fractal` | KIFS folding (Knighty/fractalforums 2010系), sphere tracing (Hart 1996) |
| `lsd` | Klüver 1926 / Ermentrout–Cowan 1979 / Bressloff 2001 (機序は lsd_report 参照) |
| `beauty` | 分散ゲート Wiener 平滑 (公知) |

## 同梱アセット

| ファイル | 由来 | ライセンス |
|---|---|---|
| `wasm/washi.jpg` (和紙テクスチャ) | [ambientCG Paper006](https://ambientcg.com/view?id=Paper006) (CC0) の微細粒を下地に、楮繊維・地合いムラを独自合成 | CC0 ベース+独自 → 実質制約なし |
| `wasm/dist/models/id_lineart*.onnx` (線画NN) | [Informative Drawings](https://github.com/carolineec/informative-drawings) (Chan+ 2022) の生成器、ONNX化はHF rocca経由・重み無改変 | **MIT — 商用可**。NNノードのライセンスクリア第1号 |
| wasm 埋め込みフォント DejaVuSans | DejaVu fonts | Bitstream Vera / DejaVu ライセンス (再配布可) |

## 同梱 LUT アトラス (`wasm/dist/luts/grade_*.png`)

512×512 GPUImage 配置の 3D LUT。**再配布物**と**自家製**を区別して記載:

| ファイル | 由来 | ライセンス |
|---|---|---|
| `grade_cine_teal` (Amatorka) / `grade_pastel_pop` (MissEtikate) / `grade_soft_sepia` (SoftElegance1) / `grade_soft_glow` (SoftElegance2) | [BradLarson/GPUImage](https://github.com/BradLarson/GPUImage) `framework/Resources/lookup_*.png` 無改変 (改名のみ) | **BSD-3-Clause** © 2012 Brad Larson |
| `grade_natural_cine` (custom) / `grade_pink_light` (light) | [pixpark/gpupixel](https://github.com/pixpark/gpupixel) `src/res/lookup_*.png` 無改変 (改名のみ) | **Apache-2.0** |
| `grade_teal_orange` / `grade_kodak_warm` / `grade_cool_blue` / `grade_bleach_bypass` / `grade_faded_film` / `grade_mono_noir` | 自家製 (数式生成、2026-08-20) | MIT (本体と同じ) |
| `whiten_base_64` / `whiten_full_64` | gpupixel 美白チェーンの焼き込み (既存) | Apache-2.0 |

## ライブラリ依存

| 依存 | 用途 | ライセンス |
|---|---|---|
| FreeType | テキスト描画 | FTL (BSD風) |
| HarfBuzz | シェーピング | MIT (旧ライセンス) |
| naga (wgpu) | ビルド時 SPIR-V→WGSL 機械翻訳 (生成物のみ同梱) | MIT/Apache-2.0 |
| Emscripten | wasm ビルド | MIT |

## デモページが実行時に読み込む外部物 (リポジトリ非同梱)

| ページ | 依存 | ライセンス | 備考 |
|---|---|---|---|
| `portrait.html` | [@mediapipe/tasks-vision](https://www.npmjs.com/package/@mediapipe/tasks-vision) (CDN) | Apache-2.0 | |
| `portrait.html` | Selfie Segmenter モデル (Google 配布) | Apache-2.0 ([model card](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Selfie%20Segmentation.pdf)) | 実行時に CDN からダウンロード |
| `gaze.html` | MediaPipe Face Landmarker | Apache-2.0 | 同上 |
| `anime.html` | [onnxruntime-web](https://www.npmjs.com/package/onnxruntime-web) (CDN) | MIT | GAN 推論ランタイム |
| `anime.html` | MediaPipe Selfie Segmenter | Apache-2.0 | 人物/背景スコープ用。実行時に CDN からダウンロード |

## 今後の NN フィルタ実験の方針 (オーナー決定 2026-08-19)

- 商用可 (Apache-2.0/MIT 等) と**非商用 (研究/デモ限定)** の両方を扱うが、
  **非商用のものは本表とデモページ上に必ず明記**する
- 例: DCT-Net (Apache-2.0, 商用可) / AnimeGANv3 公式重み (非商用) —
  詳細は各実験のレポートに記載
