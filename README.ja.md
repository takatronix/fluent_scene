# fluent_scene

**[English](README.md)** | 日本語

作者: **[@takatronix](https://x.com/takatronix)** · MITライセンス

![hud_basic — 実際のレンダリング出力](docs/images/hud_basic.png)

**画面を一度書けば、ロボットでも、ブラウザでも、アプリでも同じ絵。**

fluent_scene は CALayer 型の保持レイヤーツリーと SDF レンダリングによる
2D 合成エンジンです。カメラ映像・検出結果・HUD・UI を論理座標系で宣言し、
CPU / Vulkan / WebGPU の3バックエンドが**ピクセル一致**で描きます
（golden テストが契約。実測 max|Δ|=1）。

```
Scene(宣言 .fvs/YAML) → Stage(実行時ツリー/C++) → Surface(ピクセル)
```

## ライブデモ — ブラウザで今すぐ

すべて [GitHub Pages](https://takatronix.github.io/fluent_scene/) 上で動く
wasm ビルドです。サーバー処理はなく、映像はタブの外に出ません。

| デモ | 内容 |
|---|---|
| [filter studio](https://takatronix.github.io/fluent_scene/edit.html) | ノードグラフの映像合成エディタ — 複数入力を回転・ブレンドで重ね、人物抽出(MediaPipe→レイヤーマスク)で背景だけ別フィルタ、47フィルタを自由に配線。「昔のカメラみたいに」「背景だけ水墨画に」とAIに頼むと自動で配線される（内蔵オフラインAI、または自分のClaude / OpenAI互換キー。キーはlocalStorageのみ）。共有リンク・Scene YAML書き出し付き |
| [filter lab](https://takatronix.github.io/fluent_scene/filters.html) | 全47フィルタをWebカメラでワンタップ切替 — 単一ソースGLSL∩C++のカタログ実演 |
| [プレイグラウンド](https://takatronix.github.io/fluent_scene/) | ライブラリ一式が手元のタブで動く（CPU wasm・ポートレート演出付き） |
| [beauty](https://takatronix.github.io/fluent_scene/beauty.html) | 美顔フィルタ — 磨皮(分散ゲートWiener)+肌限定美白+3D LUT、Webカメラでワイプ比較 |
| [lsd](https://takatronix.github.io/fluent_scene/lsd.html) | LSD フィルタ — 時間駆動のサイケデリア、閉眼幻視対応（[解説レポート](https://takatronix.github.io/fluent_scene/lsd_report.html)） |
| [gaze](https://takatronix.github.io/fluent_scene/gaze.html) | 視線フォーカス — MediaPipe landmarker が Scene パラメータを駆動 |
| [webgpu](https://takatronix.github.io/fluent_scene/webgpu.html) | バックエンド検証 — 同一シーンを CPU と WebGPU で描いて画素比較 |

## 3つの書き方、同じ絵

**C++（Stage API）** — ロボット・デスクトップ・組み込み:

```cpp
Stage stage(1920, 1080);
stage.image(camera);
stage.boxes(detections).color(Color::Teal).smoothing(0.2f);
stage.group("hud").position(24, 24).shadow()
     .rect({0, 0, 340, 96}).cornerRadius(12).color({0, 0, 0, 0.45f});
renderer->render(stage, dt);   // CPU でも Vulkan でも同じ絵
```

**YAML（Scene 文書）** — 人にも AI にも編集可能。型検査・参照解決・digest が
実行前に全部走るので、**AI が実行中の画面を安全に生成・編集**できます:

```yaml
layers:
  - content: { image: { source: $inputs.camera } }
  - content: { boxes: { source: $inputs.detections, smoothing: 0.2 } }
  - id: hud
    position: [24, 24]
    shadow: {}
    sublayers:
      - content: { rect: { size: [340, 96], corner_radius: 12,
                           color: [0, 0, 0, 0.45] } }
```

**JavaScript（wasm）** — Webアプリへは ES module 1つ:

```js
import createFluentScene from './fluent_scene.mjs';
const mod = await createFluentScene();
const inst = mod.cwrap('fs_create', 'number', ['number'])(
    mod.stringToNewUTF8(sceneYaml));
// 毎フレーム: カメラを fs_commit_image → fs_render(CPU) か
// fs_render_webgpu(canvas 直描画・読み戻しゼロ) — API は数値と文字列だけ
```

## どこで動くか

| プラットフォーム | 入口 | 状態 |
|---|---|---|
| **ロボット (ROS 2)** | `scene_node`（.fvs+.fvb → トピック購読 → 描画 → Image 配信）、`scene_web` ライブ編集、`fvsc` CLI | 実機運用中 |
| **Webアプリ** | `fluent_scene.mjs` + `.wasm`（ES module、自己完結）。CPU または WebGPU | 公開中（上のデモ） |
| **デスクトップ** | C++17 ライブラリ + `stage_web` / `scene_web` / `fvsc` | 運用中 |
| **モバイル** | 同じポータブル C++17 コア + フラット C ABI（wasm/api.cpp と同じ面）。iOS / Android バインディングはロードマップ（[計画書](docs/design/mobile_support.ja.md)） | 計画 |

コアの依存は freetype + harfbuzz だけ。GPU が無ければ CPU リファレンスが
そのまま本番品質で動きます。

## バックエンドと「同一の絵」契約

| バックエンド | 用途 | 実測 |
|---|---|---|
| `CpuRenderer` | リファレンス。どこでも動く（wasm 含む） | golden の定義元 |
| `VulkanRenderer` | ロボット・デスクトップ本番。実行時シェーダーコンパイルゼロ | 1080p 5.6ms/frame（CPU比 ~17倍、読み戻し込み） |
| `WebGPURenderer` | ブラウザ GPU。canvas 直描画（読み戻しゼロ） | CPU比 max|Δ|=1・許容超過0画素 |

同一性は願望ではなく仕組みです: 描画プランは共有層
([render_shared.hpp](src/render_shared.hpp)) を3者が同じ順で歩き、
シェイプとフィルタの数式は **GLSL∩C++ の単一ソース**
([filters_shared.h](include/fluent_scene/shared/filters_shared.h)) を
CPU は C++ として、Vulkan は GLSL→SPIR-V として、WebGPU は
SPIR-V→WGSL 機械変換（naga）としてコンパイルします。手書き移植はゼロ。
golden テストが3バックエンドを同じ基準画像に対して検証します。

## なにができるか

- **content 13種** — image（部分切り出し） / text（日本語・HarfBuzz） /
  line / polyline / polygon / rect / circle / circles / arc / arrow /
  crosshair / grid / boxes（ラベル+時間平滑化）。全て SDF・AA 付きで、
  どの出力解像度でもベクタ品質
- **CALayer 準拠の属性** — frame / position / anchor / rotation / scale /
  opacity / shadow / border / background / cornerRadius / masksToBounds /
  blend。左上原点・+y 下の**1座標系のみ**（反転スイッチは存在しない）
- **フィルタ 54種** — blur / bilateral / color_transform / toon / halftone /
  ripple / beauty（磨皮+美白+NR） / lut（3D LUT） / lsd /
  bokeh（多角形絞り） / oilpaint（Kuwahara油絵） / ntsc + crt（コンポジット
  信号の実変調→ブラウン管。MAME ntsc.fx BSD-3 / Lottes PD / Cathode-Retro
  MIT 系譜） / fractal（無限KIFS飛行） /
  notebook（鉛筆スケッチ、[flockaroo XtVGD1](https://www.shadertoy.com/view/XtVGD1)
  移植・CC BY-NC-SA につき**この1本のみ非商用限定**、他は MIT のまま）/
  アート様式ファミリー: anime（セル影+XDoG線画）/ watercolor（Bousseau
  顔料密度水彩）/ sumie（水墨・掠れと滲み）/ impressionist（勾配追従の
  筆致点描）/ stainedglass（Worleyステンドグラス）/ pixelart（Bayerディザ
  ドット絵）— 調査とパラメータ設計は
  [docs/design/art_filters.ja.md](docs/design/art_filters.ja.md) /
  texture（テクスチャ合成 — 画像パラメータをタイルして multiply / screen /
  overlay / softlight / add、紙・キャンバス・粒状・リークライトのプレート同梱）…
  任意のレイヤーにもグループ（合成後の1枚）にも掛かる

  ![フィルタカタログ（filters_tour の出力）](docs/images/filters_tour.png)

- **implicit animation** — 属性を変えるだけで滑らかに動く
  （`Transaction t(0.3f, Ease::InOut)`）。時間は `render(stage, dt)` の dt
  でしか進まないため、**t=0.15s の画面をバイト単位で再現**できる
- **保持型** — 毎フレームは変更データの差し替えだけ
  （`setImage / setText / setBoxes / opacity()`）
- **UI コントロール** — `ui::Button` / `ui::Switch` / `ui::Slider` /
  `ui::Segmented` / `ui::Gauge` / `ui::Dropdown`。入力は
  `stage.pointerDown/Move/Up` への注入だけ — Web のクリックもタッチも
  VR レイも同じ3呼び出しに正規化される

  ![UIコントロールカタログ](docs/images/ui_catalog.png)

## AI と作る前提の設計

Scene 文書は AI が書き手になるための契約を持ちます: 実行前に全て拒否する
型検査、並べ替え不変 digest、GPU 予算ゲート、`describe --json` による能力の
自己記述、コントラスト比などを警告するデザインリンター。壊れた編集は
フレーム境界で拒否され、壊れたフレームが表に出る経路はありません。
詳細は[設計書 §13](docs/design/fluent_scene.ja.md)。

## 5分で始める

```bash
sudo apt install libfreetype-dev libharfbuzz-dev
cmake -S . -B build && cmake --build build -j
ctest --test-dir build          # 単体 + golden 画像 + 全 example
./build/hud_basic               # 冒頭の画像を実際に描く
./build/scene_web examples/scenes/webcam_water.fvs   # ライブ編集サーバー
```

wasm を自分でビルドする場合（生成物は `wasm/dist/`、そのまま Pages に載る
ものと同一）:

```bash
source ~/emsdk/emsdk_env.sh
./wasm/build.sh --webgpu        # GLSL→SPIR-V→naga→WGSL も自動生成
```

## フェーズ

| Phase | 範囲 | 状態 |
|---|---|---|
| L0 | Stage API + CPU リファレンス + Transaction + golden | **完了** |
| L1 | Vulkan バックエンド（CPU と同一出力） | **完了** |
| L2 | Scene v1alpha2（YAML→Stage）+ describe --json + リンター | **完了** |
| L3 | ROS binding + scene_node + インスペクター | **完了**（実機 E2E） |
| L4 | UI コントロール（ポインタ注入 + 6コントロール） | **完了** |
| W1 | wasm（ブラウザで CPU レンダラ、native と 1LSB 一致） | **完了** |
| W3 | WebGPU バックエンド（ブラウザ GPU、canvas 直描画） | **完了** |
| 次 | Python binding / C ABI 公開 / モバイルバインディング | 計画 |

既知の制限は [cookbook 末尾](docs/cookbook.ja.md#既知の-l0-制限正直リスト)に
明記しています。

## ドキュメント

- [getting-started.ja.md](docs/getting-started.ja.md) — 5分チュートリアル
- [cookbook.ja.md](docs/cookbook.ja.md) — やりたいこと別レシピ集
- [docs/api/README.ja.md](docs/api/README.ja.md) — API リファレンス
  （ヘッダが一次ソース、全公開 API に Doxygen コメント）
- [設計書（なぜこうなっているか）](docs/design/fluent_scene.ja.md)
- [CHANGELOG.md](CHANGELOG.md)

## 作者

**takatronix** — デモや進捗は X で:
**[@takatronix](https://x.com/takatronix)**

## ライセンス

[MIT](LICENSE) © 2026 takatronix — 例外 (notebook の非商用) と
全出典は [THIRD_PARTY.md](THIRD_PARTY.md) に一覧。
