# liquid — 生きている液体 (Kepler の Liquid を filter に) 設計書

- 状態: **実装済** (2026-10-19、ブランチ `feat/liquid`、main 未マージ)
- モードID: `FS_LIQUID = 55`、公開名 `liquid` / 型名 `Liquid`
- デモ: `wasm/liquid.html`
- 原典: Kepler の顔 `public/faces/liquid.html` (raw WebGL2、4パス)

## 1. 目的

Kepler の "Liquid" はメタボールの液体です。虹彩のある濡れた金属で、内側に
光の流れがあり、蒸気を吐き、状態 (待機/聞く/考える/話す) で色と動きが
変わり、ポインタに寄り、タップで弾けます。これを **fluent_scene を通して**
描けるようにし、デモとして出すのが目的です (オーナー 2026-10-19
「fluent sceneのデモでつかいたい」)。

条件は「CPU / Vulkan / WebGPU の3バックエンドで同じ絵になること (golden
で検証)」と「Scene の $params だけで全部を駆動できること」の2つです。

## 2. どこに置くか — 生成系 filter + エンジン側の3つの拡張

fractal と同じ**生成系 filter** にしました。content 型にしなかった理由は
2つあります。

- filter なら本体を `filters_shared.h` に1回書くだけで、Vulkan (GLSL)・
  WebGPU (naga で WGSL に機械翻訳)・CPU (glsl_compat) の全部で同じ本体が
  走ります。新しいパイプラインもシェーダも要りません
- 合成も他の filter と同じ規則で決まります。出力 α は液体自身のもので
  (`fs_shapes_alpha`: opacity / inkline と同じ扱い)、背景は透明です。
  ソースは読みません (α の形だけ使います)。不透明な `background` を持つ
  レイヤーに掛ければ、その矩形の中で液体が下の絵の上に浮きます

Kepler 版はホスト (JS) で状態ごとの値を積分していました。fluent_scene に
載せるため、エンジン側に一般機能を3つ足しました。

| 拡張 | 内容 | liquid での用途 |
|---|---|---|
| **スロット 5 → 12 → 16** (`kFilterSlots`) | GPU は push block の `pa`/`pb`/`px2`/`color` の4つの vec4 (`color` は filter パスでは使っていなかった)。`fs_apply(mode, uv, pa, pb, pc, pd)` | 13個の param (下表) |
| **`FilterUnit::Clock`** | `Layer::step` が毎 advance で dt を足す自走スロット。値を設定すると、その値から数え直す | `time` (ホストのループが不要)、`splash` (タップ = 0 を設定) |
| **filter パラメータの `$params` 束縛** | `filters: [ { liquid: { level: $params.level } } ]`。`Layer::setFilterParam` は Transaction の中なら補間する | 気分の重みとポインタは param の `animate` がそのまま補間する |

既存 filter は全部5スロット以内なので、本体も dispatch 行も変わりません
(golden は全一致のまま)。stateful filter の state パスは5スロットのままです。

## 3. パラメータ (スロット順 = filters_def.h)

| slot | 名前 | 単位 | 既定 | 意味 |
|---|---|---|---|---|
| 0 | time | Clock | 0 | 秒。dt で自走する |
| 1 | level | Scalar | 0 | 声の大きさ 0–1 (マイク・TTS) |
| 2 | listen | Scalar | 0 | 気分の重み。listen + think + speak の残りが idle |
| 3 | think | Scalar | 0 | 〃 (合計が 1 を超えたら正規化する) |
| 4 | speak | Scalar | 0 | 〃 |
| 5 | pointer_x | CoordX | 0 | ポインタ (レイヤーのローカル座標) |
| 6 | pointer_y | CoordY | 0 | 〃 |
| 7 | pointer | Scalar | 0 | ポインタの存在 0–1 (`animate` で出入りさせる) |
| 8 | splash | Clock | 9 | タップからの経過秒。0 を設定するとタップになる |
| 9 | hue | Scalar | 0 | パレットの色相回転 (度) |
| 10 | size | Scalar | 1 | 図形の大きさ (短辺の半分 = 1) |
| 11 | quality | Scalar | 1 | GPU は step 数 (20 + 28q)、CPU は粗い格子 (§6) |
| 12 | pearl | Scalar | 0 | 材質。0 = 虹彩の濡れた金属、1 = 真珠のガラス (§7.1) |

気分を4状態の **重み** にしたのは、`animate` を付けた3つの param を同時に
setParam すれば、どの状態からどの状態へも線形に混ざるからです。状態を
1本の軸に並べると、idle→speaking の途中で listen と think を経由して
しまいます。

## 4. 「時間の純関数」にする — 2本の時計と結果の混合

Kepler は状態ごとに位相の速度が違い (例: 葉の移動 idle 0.22 / thinking
0.75)、それをホストで積分していました。filter は time の純関数なので
積分はできません。かといって `phase = t × rate(state)` にすると、速度が
変わった瞬間に t = 10⁴ s なら位相が数千ラジアン跳びます。

そこで **静 (calm) と動 (busy) の固定時計を2本** 回し、位相ではなく
**結果** を気分・声・splash から決めた重みで混ぜます。

- 葉 (lobe) 4つの Lissajous 軌道: t·0.22 と t·0.75 の位置を mix
- 表面波: 位相 t·1.0 と t·2.4 の変位を mix (大きな折り目は静の時計だけ)
- 内側の光の流れ (gyroid シート): 位相/回転の静と動を mix。動の重みが
  0 のピクセルでは動を計算しない
- 蒸気: ループする flow map を2位相 (半周期ずらし) でクロスフェード。
  速度が変わってもループが伸び縮みするだけで跳ばない。各位相は重み 0 の
  瞬間にシードを替えるので、同じ模様は戻らない

**飛沫** は粒子リストを持たず、閉形式の減衰ばね (k = 2, c = 1.4) で
`r(t) = e^{-0.7t}(r₀ cos ωt + (v₀ + 0.7 r₀)/ω · sin ωt)` を使います。
タップは5粒で、向きはタップの時刻 (`time − splash`) から決まるシードで
変わります。話している間は3粒が時間差で順番に飛び、大きさは level で
決まります。

## 5. 1パスにした描画 (Kepler の4パスとの対応)

| Kepler | liquid (1パス) |
|---|---|
| 1. raymarch の液体 (環境反射・薄膜・2つのハイライト・屈折した内側の光・縁の SSS) | 同じ式のまま。SDF に「首 + 先端」の触手 (ポインタへ伸びる) と飛沫を足した。外れたレイでも最接近距離が1–2画素以内なら縁の色を少し載せる (AA。quality が低いほど幅を広げるので、CPU の粗い格子でも縁が階段にならない) |
| 2. 蒸気: フィードバックテクスチャで curl-noise 移流 | §4 の flow map (2D value noise の二重 domain warp + ridge の筋)。シルエットまでの距離は march の最接近距離を使い、外側の球の推定へ滑らかに渡す |
| 3. bloom (1/2・1/4 解像度のガウス) | 解析的な代用: シルエットに沿う halo + 液体の明部の自己発光 |
| 4. 合成・トーンマップ・grain | 同じ。grain とジッターのハッシュには**整数ピクセル座標**を使う (補間 uv を入れると入力の誤差が 10³ 倍に増幅され、CPU/GPU が合わない) |

状態バッファを使わないので、`persistent_buffers` も前フレームも不要です。
golden は t の任意の時刻を直接描けます。

## 6. CPU の経路と許容誤差

- CPU は同じ本体を1ピクセルずつ実行します。1280×720 で ~1.2 s/frame
  (ネイティブ) かかるので、実時間で動かすのは GPU です
- **`quality < 0.75` のときだけ CPU は粗い格子を使います**。stride 2/3/4
  (q ≥ 0.4 / ≥ 0.25 / それ未満) のブロックの隅で raymarch し、乗算済みで
  双線形補間します。隅どうしで α か色が大きく食い違うブロック (縁と飛沫。
  面積の数%) だけは1画素ずつ raymarch するので、縁がブロック単位の階段に
  なりません。384×240 で quality 1 = 0.14 s、0.5 = 0.04 s (ネイティブ)
  (stage_tests が全解像度との平均差 < 6/255 を確認)。GPU はこの経路を
  通らず、quality を step 数に使います
- golden `liquid` は 6 タイルです (t=0 の idle、各気分 + level、ポインタの
  触手、splash 0.35 s + hue、quality 0.76 + size 0.8)。**CPU は golden と
  ビット一致し、Vulkan (RTX 5090) は max_diff 48 / over_ratio 0.75% です**。
  ずれるのはシルエット上の1画素だけで、「かすめて当たった (α 0.3)」と
  「AA の縁で外れた」が丸め誤差で入れ替わります。そのため liquid だけ
  max_diff を 96 に緩め、over_ratio (±2 を超える画素の割合) は通常の
  ガード (CPU 0.2% / GPU 1%) のまま残します。lsd/fractal のように
  max_diff 255 の全面 waiver にはしていません
- WebGPU は headless Chromium (SwiftShader) で `?snap=1` の読み戻しを使い、
  CPU と並べて目視で確認しました (`webgpu.html` の自己検証も PASS)

## 7. デモ (wasm/liquid.html)

シーンはレイヤー1枚 + filter 1つ + $params 11個です。ページがするのは
「何が起きたか」を setParam することだけです。

- 状態ボタン + 自動巡回 (7/6/6/8 秒)、1–4 キー
- ポインタ: px/py は `animate` 0.35 s で追従し、出入りは ptr を 0.6 s で
  補間する。タップ/スペースで `splash = 0` + level のキック
- 🎤: getUserMedia → RMS (dB) → level (立ち上がり 20/s、減衰 6/s)。
  マイクが無いときは、聞く/話すが合成の音節エンベロープで揺れる
- WebGPU が使えなければ CPU (ステージ 0.3 倍 + quality 0.5)。バッジに
  WebGPU/CPU・fps・解像度を出す。fps が落ちたら quality → 解像度の順で
  下げる
- `?cpu=1` `?res=WxH` `?state=thinking&auto=0` `?snap=1` (ループ無しで
  `lqSnapshot(dt)` が1枚返す。headless の WebGPU canvas はスクショが空に
  なるため)

## 7.1 pearl — Fluent の真珠ガラス (0.19.0)

Kepler の FLUENT の顔を liquid で作り直すとき、LIQUID の顔 (虹彩の
金属 + 青緑の蒸気) と見分けがつく材質が要りました。hue だけでは材質が
変わらないので、`pearl` を足しました (0〜1 で連続に混ざる)。

- 体: 深い c0 の半透明。キーライトを wrap で受ける
- 光: ガラスが薄くなる所 (`(1 − n·v)^1.7`) に色が溜まり、体の周りの角度で
  c1 / c2 / c0 が回る (Windows の Bloom のような縁の光)
- 内側の光の流れ: gyroid シートを広く (sharp 6)、明るく見せる。色の輪の
  軸 (spin 軸) の近くでは角度が速く回って継ぎ目に見えるので、軸から
  0.35 以内は色を3色の平均に寄せる (`fs_lq_currents` の `pole`。
  pearl 0 では 0 なので LIQUID の絵は変わらない)
- 反射はフレネルの分だけ (ガラス)。真珠の光沢は縁の近くだけ。広い
  ハイライトと小さく鋭いハイライト
- 蒸気は筋 (ridge) を 8 割減らして濃度を柔らかくし、注入を 45% 下げる。
  bloom の外側の halo を広げる (アクリルの霞)
- パレットは Fluent の青 → 紫。話すときは LIQUID の橙ではなくローズ/珊瑚
- 表面の波は 3 割おだやか (金属の細かいさざ波より、ゆっくりしたガラス)

## 8. 既知の差と残課題

- 蒸気は移流シミュではなく解析的な近似です。Kepler の「ポインタが蒸気を
  かき混ぜる」は再現していません (速度の履歴が要るため)。filter の
  state バッファで本物の移流にする余地はあります (§5 の表の 2 を
  persistent_buffers P1 で)
- ポインタのばね (Kepler は k = 26, c = 7 の慣性) は `animate`
  ease_out 0.35 s で代用しています。オーバーシュートしません
- 話すときの飛沫は、その時点の level で大きさが決まります (Kepler は
  音節の立ち上がりで発射していた)
- `fs_webgpu_read_last` は、ページが canvas に present した後だと
  headless (SwiftShader) で mapAsync に失敗します (既存の挙動で liquid とは
  無関係)。デモの検証では read_begin を使っています
