# アートフィルタ群 (設計書)

status: 実装と同時進行のリサーチノート + 凍結パラメータ表
date: 2026-08-19

対象: **anime / watercolor / sumie / impressionist**(+ボーナス
**stainedglass / pixelart**)。既存の `toon`(簡易) と `oilpaint`(Kuwahara)
の上に、絵画様式ごとの「本物の見た目の根拠」を持つフィルタを増やす。

## 0. 制約 — この枠の中で設計する

現行フィルタ機構 (filters_shared.h) の契約:

| 制約 | 帰結 |
|---|---|
| 単一パス・無状態 | 中間バッファ不可。多パス手法 (ETF/FDoG, LIC, 流体) は**構造ごと再定式化**するか諦める |
| パラメータ 5 slot | 様式の本質 5 つに絞る。残りは定数で焼く |
| GLSL∩C++ 単一ソース | fract 無し (`x - floor(x)`)、配列無し、value noise 必須 (ハッシュ閾値の CPU/GPU 丸め差で golden が割れるため) |
| golden 3バックエンド一致 | 不連続な分岐 (hash 直接閾値) 禁止。smoothstep / vnoise で連続化 |
| チェーン可 | 前処理 (bilateral) や仕上げ (median, vignette, lut) は**フィルタ合成に外だし**できる |
| コスト感覚 | bilateral=49tap, oilpaint=64tap, notebook≈400tap が既存の許容帯 |

`time` パラメータはホスト駆動 (lsd/notebook 方式)。今回の4本は**静止画で
完結する様式**なので time slot は使わない (紙・キャンバスのゆらぎは座標
ノイズで決定的に出す)。

## 1. 調査 — 様式ごとの手法系譜と採用判断

### 1.1 アニメ / セル画

| 系譜 | 内容 | 採用 |
|---|---|---|
| 既存 `toon` | RGB ポスタライズ + Sobel 閾値 | 残す (安い)。RGB 量子化は**色相が回る**・二値エッジはジャギる、が弱点 |
| Winnemöller 2006 (Real-Time Video Abstraction) | bilateral 平滑 + **輝度のみ soft quantization** (tanh) + DoG エッジ | ✅ 骨格として採用 |
| XDoG (Winnemöller 2011) | DoG の連続閾値化 `1+tanh(φ·(S−ε))` — 線がアンチエイリアスされた「描いた線」になる | ✅ 線抽出に採用 |
| FDoG / ETF (Kang 2007, Coherent Line Drawing) | エッジ接線流に沿った DoG。線が繋がり最も美しい | ❌ ETF 平滑化が多パス。persistent_buffers (P2) 後の v2 へ |

**採用構成 `anime`**: bilateral 平滑 (本体内蔵、mix 可変) → 輝度 soft
quantization (chroma 保存 = 色相が回らない) → 等方 XDoG 線 (太さ・濃さ
独立制御) → 彩度リフト。`toon` の上位互換ではなく別物として共存。

### 1.2 油絵

| 系譜 | 内容 | 判断 |
|---|---|---|
| Kuwahara 1976 | 最小分散象限平均 | ✅ 既存 `oilpaint` (0.10.1)。触らない |
| Papari 2007 / Kyprianidis 2009 (anisotropic Kuwahara) | 8セクタ+構造テンソル異方性。フラット領域の「羽毛状アーティファクト」が消える | 構造テンソルの平滑化が実質多パス。単一パス近似は oilpaint の jitter が既に方向場の代用。**v2 候補として park** |
| Hertzmann 1998 (curved brush strokes) | 大→小の多層曲線ストローク。最高品質だが反復型 | ストローク表現は 1.4 の impressionist に単一パス再定式化で吸収 |

油絵の「筆致・盛り上げ (impasto)」は impressionist 側に実装し、
`oilpaint` は Kuwahara 平滑の顔として残す — 二本立て。

### 1.3 水彩

| 系譜 | 内容 | 採用 |
|---|---|---|
| Curtis 1997 (Computer-Generated Watercolor) | Kubelka-Munk 顔料 + 流体シミュ | ❌ 多パス反復。見た目の分解だけ借りる |
| Bousseau 2006 (Interactive Watercolor Rendering) | **顔料密度モデル** `C' = C·(1−(1−C)·(d−1))` に エッジ暗化・granulation・希釈を全部畳む | ✅ 密度モデルをそのまま採用 (単一式で単一パスに収まる、リアルタイム NPR の定番) |
| 実時間ゲーム系 (wobble+paper) | UV ノイズ歪みで輪郭の手ぶれ、紙テクスチャ乗算 | ✅ vnoise 2オクターブで採用 |

**採用構成 `watercolor`**: wobble (UV 歪み) → ウォッシュ (黄金角スパイラル
ギャザー = にじみ+単純化) → 密度 d を 1 + エッジ暗化 + granulation +
大域ムラで組む → Bousseau 密度式 → 希釈 (紙白へのリフト) → 紙の tooth
(ノイズ照明)。

### 1.4 水墨画

| 系譜 | 内容 | 採用 |
|---|---|---|
| Strassmann 1986 (Hairy Brushes) | 筆圧・穂先モデルのストローク合成 | 掠れ (かすれ) の「穂ごとのインク切れ」概念だけ借りる |
| Zhang 1999 / 二段拡散系 | セルオートマトンによる滲み (にじみ) | ❌ 反復。**リング min ギャザー + ノイズ変調半径**で一発近似 |
| Way 2002 (Chinese ink painting NPR) | 輪郭ストローク + 濃淡ウォッシュの分業 | ✅ 構成として採用 |

**採用構成 `sumie`**: 輝度→墨濃度 (soft 4段ウォッシュ、濃淡) →
グラデーション接線方向への異方性スメア (運筆) → ストローク空間の
異方性ノイズで掠れ → リングギャザーで滲みハロー → XDoG 輪郭 (筆圧
ゆらぎ付き) → 和紙 (生成り色 + 繊維ノイズ) → 淡彩 (chroma で
元の色を薄く戻す)。

### 1.5 印象派

| 系譜 | 内容 | 採用 |
|---|---|---|
| Haeberli 1990 (Paint By Numbers) | 点サンプル→ストローク張り | 概念の祖 |
| Litwinowicz 1997 (Impressionist video) | **短い直線ストローク・向きは勾配接線・エッジでクリップ** | ✅ ストロークモデルとして採用 |
| Hertzmann 1998 | 曲線・多層 | 反復のため❌ (多層は2スケールで近似) |
| 点描/筆触分割 (divisionism) | 隣接ストロークの色を故意に振動させ、網膜上で混色 | ✅ 種ごとの色ジッタとして採用 |

**単一パス再定式化** (この設計の肝): ストロークを「描く」のではなく、
各画素が**近傍のストローク種 (ジッタ格子) を逆引き**し、自分を覆う種の
うち z-hash 最大のものの色を着る。カプセル距離場で足先テーパー+ソフト
縁。粗い背景層 + 細かい前景層の 2 層で被覆保証。ブラシの盛り上げ
(impasto) はストローク座標系の異方性ノイズを疑似法線として照明。

### 1.6 ボーナス

- **stainedglass** — Worley 1996 セルラーノイズ (F1/F2)。F2−F1 が小さい
  ところが鉛線。セル代表点の色を posterize したガラス + セル内グラデ +
  ハイライト。単一パス 9 セル探索で安い。
- **pixelart** — `pixelate` の上位: ブロック化 + **Bayer 4×4 ordered
  dither** + チャンネル量子化 + 彩度そろえ。レトロゲーム機の絵。
- 見送り (将来メモ): crosshatch (notebook が NC な代わりの MIT 版線画は
  anime の XDoG が当面担う) / lowpoly・reaction-diffusion (persistent_buffers
  P2) / LIC 流線画 (Cabral 1993, 多パス) / ニューラル style transfer
  (モデル配布が必要、フィルタ機構の外)。

## 2. パラメータ表 (凍結)

設計方針: **slot 0 = 様式の強さ・大きさの主ダイヤル**、以降は効果の
独立軸。全フィルタ、デフォルト値が filters_tour で「一目でその様式」に
見えることを合格条件とする。

### anime (FS_ANIME=40)

| slot | name | default | unit | 意味 |
|---|---|---|---|---|
| 0 | levels | 5.0 | Scalar | 輝度階調数 (セル影の段数)。2〜8 |
| 1 | lines | 1.0 | Scalar | 線の濃さ (0=線なし〜2) |
| 2 | width | 1.6 | Length | 線の太さ (XDoG σ) |
| 3 | smooth | 0.7 | Scalar | 前平滑の強さ (bilateral mix) |
| 4 | vivid | 0.3 | Scalar | 彩度リフト (0=原色調) |

### watercolor (FS_WATERCOLOR=41)

| slot | name | default | unit | 意味 |
|---|---|---|---|---|
| 0 | wash | 6.0 | Length | にじみ/単純化半径 |
| 1 | edge | 0.9 | Scalar | エッジ暗化 (顔料の縁溜まり) |
| 2 | grain | 0.7 | Scalar | granulation (顔料の粒状沈着) |
| 3 | wobble | 1.0 | Scalar | 輪郭の手ぶれ (UV 歪み) |
| 4 | dilute | 0.35 | Scalar | 希釈 (紙白への透け) |

### sumie (FS_SUMIE=42)

| slot | name | default | unit | 意味 |
|---|---|---|---|---|
| 0 | ink | 1.0 | Scalar | 墨の濃さ (トーンカーブ) |
| 1 | bleed | 4.0 | Length | 滲み半径 |
| 2 | dry | 0.6 | Scalar | 掠れ (ドライブラシ) |
| 3 | outline | 0.8 | Scalar | 輪郭ストロークの濃さ |
| 4 | chroma | 0.0 | Scalar | 淡彩 (0=純墨、0.3で彩色水墨) |

### impressionist (FS_IMPRESSIONIST=43)

| slot | name | default | unit | 意味 |
|---|---|---|---|---|
| 0 | stroke | 7.0 | Length | 筆致の大きさ (種格子ピッチ) |
| 1 | vibrance | 0.8 | Scalar | 筆触分割の色振動 |
| 2 | flow | 0.8 | Scalar | 向きの勾配追従度 (0=ノイズ場) |
| 3 | relief | 0.7 | Scalar | impasto 照明 (絵具の盛り) |
| 4 | canvas | 0.35 | Scalar | キャンバス地の透け |

### stainedglass (FS_STAINEDGLASS=44)

| slot | name | default | unit | 意味 |
|---|---|---|---|---|
| 0 | size | 24.0 | Length | セル寸法 |
| 1 | lead | 0.8 | Scalar | 鉛線の太さ (0=なし) |
| 2 | irregular | 0.85 | Scalar | セル形状の不規則さ |
| 3 | saturate | 0.5 | Scalar | ガラスの彩度・ポスタライズ強さ |
| 4 | light | 0.6 | Scalar | 透過光のグラデーション |

### pixelart (FS_PIXELART=45)

| slot | name | default | unit | 意味 |
|---|---|---|---|---|
| 0 | size | 6.0 | Length | ドット寸法 |
| 1 | colors | 5.0 | Scalar | チャンネル階調数 (≒パレット規模) |
| 2 | dither | 0.5 | Scalar | Bayer ディザ強度 |
| 3 | saturate | 0.25 | Scalar | 彩度そろえ (レトロパレット感) |
| 4 | — | | | 予備 |

## 3. 実装ノート (本体の要点と、初稿で踏んだ落とし穴)

- **soft quantization** (anime/sumie 共通): 帯内位置を
  `clamp((band−0.5)·φ, −0.5, 0.5)` でプラトー+ランプ化 (tanh でなく
  clamp-linear — シムに何も足さず golden も安定)。anime の chroma 保存は
  `c · Lq/max(L,ε)`。
- **XDoG**: 2 半径のリングサンプルガウス近似 (中心+8tap) の差分を
  `smoothstep(0.014/(0.4+0.6s), 0.08, −DoG)·s` の連続形で閾値化。
  strength が濃さと (閾値低下で) 太さの両方に効く。
- **watercolor の密度式**: d は 1 中心 (Bousseau)。`d = clamp(1+Σ, 0.4,
  2.5)`。granulation は中間調ゲート `(0.10+0.90·4L(1−L))` 必須 —
  全面に掛けると紙やすりになる (初稿の教訓)。
- **sumie のトーン**: 指数は**濃度側** `dens = (1−L)^(2.6−0.9·ink)`。
  輝度側に γ を置いた初稿は中間調が灰ベタになり余白が死んだ。仕上げに
  `dens·smoothstep(0.06, 0.16, dens)` でハイライトを紙へスナップ。
- **方向場は粗く安定に**: 運筆/掠れの接線はテクスチャ内で毎ピクセル暴れ、
  ストローク空間ノイズが白噪化する (初稿)。勾配は 3.2u の粗視化で取り、
  平坦部は `coh = smoothstep(0.02, 0.09, gm)` で固定方向にブレンド。
- **ノイズ2点読みは近接で**: 紙 tooth / 剛毛照明の「微分」は 2 サンプルを
  ノイズ空間で近く (0.2〜0.35) 置く。離すと独立サンプル=塩ノイズ (初稿)。
- **impressionist の色振動は輝度保存**: ジッタ後
  `c · L(c)/L(c')` で明度を返す。自由ジッタは空がコンフェッティになる。
  被覆は前景層+背景層 (2.1×ピッチ) で保証し、**canvas はその上の透過**。
  dab 選択は `score = cover·(0.35+0.65·z) + 層バイアス` の argmax。
- **stainedglass の F1/F2**: 3×3 セル探索を 2 距離追跡で。鉛線は
  `smoothstep(lw, 1.9lw, F2−F1)`、`lead=0` は線なし特例。
- **pixelart の Bayer 4×4**: `M2(x,y)=2x+3y−4xy` の再帰構成を算術で —
  配列レス (GLSL∩C++ 制約に配列を持ち込まない)。
- 乱数は全て fs_lsd_hash / fs_lsd_vnoise (連続・fract-free)。**新しい
  intrinsic を glsl_compat.hpp に足していない** (mat2 も不使用、回転は
  cos/sin 手組み)。

## 4. ライセンス

全て**論文アルゴリズムからの独自実装 (MIT)**。shadertoy 等からのコード
借用なし (notebook の CC BY-NC-SA が唯一の例外である状態を維持)。
コメントに系譜 (Kuwahara 1976 方式) を明記する — 家風。

## 5. 検証 (実測済)

1. `filters_tour` — デフォルト値で様式が一目で判ること (人間目視、
   合成写真での目視追い込み込み)
2. `golden_tests` — フィルタ毎に全 slot を振る 5 シーン追加。CPU golden
   生成 → **Vulkan 同一 golden 照合 pass**。anime は通常許容で
   max|Δ|=1。watercolor/sumie は notebook と同じジッタ読み waiver
   (wobble の fp16 ドリフト×フルコントラスト境界)、impressionist は
   dab argmax tie 波及で over_ratio 3%、stainedglass/pixelart は
   ハード選択 waiver — 全て根拠コメント付き
3. 性能 (1080p フルフレーム 1 フィルタ、AGX Thor、読み戻し込み):

   | filter | Vulkan | CPU (参考) |
   |---|---|---|
   | (基準 none) | 10.9 ms | 0.10 s |
   | oilpaint (既存) | 8.5 ms | 1.9 s |
   | notebook (既存) | 38.5 ms | 54 s |
   | anime | 6.9 ms | 3.5 s |
   | watercolor | 6.2 ms | 1.4 s |
   | sumie | 6.8 ms | 2.9 s |
   | impressionist | 9.7 ms | 5.5 s |
   | stainedglass | 4.7 ms | 0.78 s |
   | pixelart | 6.4 ms | 0.40 s |

   全て notebook より軽く、Vulkan では 60fps 圏内。

## 6. 残タスク

- ~~wasm/dist 再ビルド~~ → **済** (emsdk+naga-cli 導入、
  `./wasm/build.sh --webgpu`。naga が新6本の WGSL 翻訳を通過、node
  スモークで fs_filters_json 45種 + anime Scene の CPU レンダ確認)。
  ブラウザ実機 (WebGPU パス目視) のみ残
- 実写・実カメラでの目視 (追い込みは合成写真ベース)
- v2 候補: ETF/FDoG 線画 (persistent_buffers P2 後)、anisotropic
  Kuwahara、流体水彩
