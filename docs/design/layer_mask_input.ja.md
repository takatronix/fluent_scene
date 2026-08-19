# レイヤー画像マスク入力 (設計書 draft-1)

status: **M1 実装済** (2026-08-19、オーナー GO「OKですすめて」による。
論点§6は提案どおりで確定: post-filter固定 / RGBA の A / 名前 mask /
feather は M1 に同梱)。C++ API (`layer.mask(v).maskInvert().maskFeather()`)
+ C ABI (`fs_layer_set_mask`) + 3バックエンド (golden `layer_mask`、
Vulkan max|Δ|=2)。**Scene YAML 露出は未実装** (次: `mask: {source:
$inputs.x, invert, feather}` を compiler/binding へ)。
date: 2026-08-19

## 1. 動機

NN 外段 (人物セグメンテーション等) のマスクを合成に受け入れる口が無い。
NN 調査 (nn_filter_research) の結論: セグメンテーション自体は
MediaPipe Selfie Segmenter が 250KB / SD8Gen3 NPU 0.5ms / Apache-2.0 で
「ついでに載る」レベル。**ボトルネックはエンジン側にマスクの語彙が
無いこと**。目標ユースケース:

- 人物カラー残し + 背景だけ水墨/油絵 (マスク2枚のレイヤー構成)
- 人物だけアニメ化 (マスク側にフィルタ)
- 将来: depth マスク・注視マスク等、外段が作る任意の α 場

## 2. 既存の合成語彙の棚卸し (重複実装しないため)

| 既存 | できること | 足りないこと |
|---|---|---|
| `blend` (レイヤー属性) | Normal/Add/Multiply/Screen の合成則 | 画素ごとの表示/非表示 |
| `masksToBounds` + cornerRadius | 矩形/角丸のクリップ | 任意形状 |
| `opacity` | 一様な透過 | 空間変化 |
| filter の image パラメータ (`lut`/`face`) | フィルタが画像を1枚読む機構 (**流用する**) | レイヤー合成там届かない |

## 3. 提案: レイヤー属性 `mask`

CALayer の `mask`(任意レイヤーツリー) の**画像限定版**から始める。

```yaml
layers:
  - id: bg
    content: { image: { source: $inputs.camera, fit: cover } }
    filters: [ { sumie: {} } ]
    mask: { source: $inputs.person, invert: true, feather: 3 }
  - id: fg
    content: { image: { source: $inputs.camera, fit: cover } }
    mask: { source: $inputs.person }
```

C++: `layer.mask(view).maskInvert(true).maskFeather(3)` /
C ABI: `fs_layer_set_mask(inst, "bg", "person", invert, feather)` —
マスク画素の更新は既存の `fs_image_buffer`/`fs_commit_image` と同じ経路
(per_frame 更新、転送は1回/フレームの既存契約に乗る)。

### 3.1 意味論 (v1 で固定する規則)

- マスクは **α 場**: R8 なら R、RGBA なら A を使う。0=消す、255=見せる。
  `invert` で反転
- 解像度非依存: マスクはレイヤーの **content 座標に正規化 UV で
  リサンプル** (セグメンテーションは 256² 級、映像は 720p+ が常態)。
  bilinear + `feather` (論理単位) の追いぼかし
- **適用位置 = フィルタの後、composite の直前** (offscreen α への乗算)。
  理由: 「背景だけ水墨」はフィルタ結果に穴を開ける操作だから。
  「人物だけに絵柄」は fg レイヤー側にフィルタを付ければ表現できる
  (§1 の2レイヤー構成) — つまり pre/post の両方が **2レイヤーで表現
  可能**なので、v1 は post 固定で語彙を増やさない
- マスク付きレイヤーは常に offscreen 経路 (filters/shadow と同じ §4.1-5)

### 3.2 実装スケッチ (3バックエンド)

- CPU: offscreen `buf` 完成後、`buf.α *= sample(mask, uv)` の1ループ
- Vulkan/WebGPU: composite パスは既にテクスチャセットを持つ —
  set の空きバインディング (4/5、stateful filter と同じスロット) に
  マスクを乗せ、composite.frag が α に乗算 (マスク無しはソース
  self-bind で係数1)。**新パス不要**
- golden: マスク付きシーン追加 (3バックエンド照合)

### 3.3 段階

| 段 | 内容 |
|---|---|
| M1 | 上記 v1 (画像マスク・post-filter・invert/feather) |
| M2 | guided filter 精細化 (低解像マスク→高解像エッジ整合。調査 B-5: 商用でも bilinear 素通しが現状=差別化点) |
| M3 | CALayer 完全形 (mask = 任意レイヤーツリー)。必要になったら |

## 4. NN 側との接続 (参考、本設計の外)

MediaPipe Tasks (web) は マスクを WebGLTexture で返せるが、v1 は
CPU 経由 (`fs_image_buffer`) で十分 (256²×1ch/フレームの転送は軽微)。
ゼロコピーは LiteRT.js/emdawnwebgpu の device 共有と合わせて P2。

## 5. スマホで動く/動かない (調査の結論を製品目線で固定)

### シェーダフィルタ46種 (fluent_scene 本体)

| 環境 | 判定 |
|---|---|
| iPhone (iOS 26 Safari, WebGPU) | **全種 GPU 実行可**。ただし ORT 系とは別に、Safari WebGPU は出荷直後 — 長時間安定性は実機確認が要る (調査 A-3 の既知クラッシュは NN 推論側の事例) |
| Android (Chrome 121+, WebGPU) | **全種 GPU 実行可** |
| WebGPU の無い機種 (CPU wasm フォールバック) | **軽量系のみ実用**: color系全部 / pixelate / pixelart / stainedglass / halftone / toon 等。**アート系 (sumie / impressionist / anime / watercolor / oilpaint / notebook) はコマ送り** — UI で「この機種では静止画のみ」等の格下げ表示を推奨 |

### NN フィルタ (予定、権利クリアのみ記載)

| モデル | 用途 | スマホ判定 | 根拠 |
|---|---|---|---|
| MediaPipe Selfie Segmenter (Apache-2.0) | 人物マスク | ◎ どこでも (0.5ms/NPU 実測、250KB) | 調査§8 |
| MicroAST (MIT, 1.37MB) | 任意画風 | ◯ 384²で実用圏、WebGPU ブラウザも可 (op 11種全対応確認) | 調査§結論 |
| DCT-Net (Apache-2.0, 自前学習) | 顔アニメ化 | ◯ (学習 0.5 GPU時間/スタイル) | 調査§7 |
| AnimeGANv3 級 | フルフレームアニメ化 | △ 速い機種なら可・コマ落ち許容 (オーナー判断済)。**ただし公式重みは非商用 → 自前学習が前提** | 調査§2 |
| 拡散系 (SD-Turbo 等) | 任意画風の最高品質 | ✗ スマホ実時間は届かない (実測最高 12.5fps@512×384 は非公開重み)。さらに SD-Turbo は再ライセンスで年商 $1M 上限 | 調査§3 |
| RVM / RMBG / @imgly | マッティング | ✗ 権利 (GPL-3.0 / 非商用 / AGPL) | 調査§8 |

## 6. 論点 (オーナー判断)

1. 適用位置: v1「post-filter 固定 + 2レイヤーで pre 相当を表現」で良いか、
   `stage: pre|post` を最初から語彙に入れるか
2. マスクのチャンネル規則: R8=R / RGBA=A で良いか (それとも luma?)
3. 属性名: `mask` で良いか (`matte` 案もある。CALayer 語彙に合わせるなら mask)
4. M1 に feather を含めるか (含めない最小案 = invert のみ、feather は M2 の
   guided と一緒に)
