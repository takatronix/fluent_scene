# Changelog

## 0.14.1 — 2026-08-19 (水彩の絵画化 / 印象派の長筆致・原色)

- **watercolor**: 実カメラで「滲んだ写真」にしかならない指摘 (オーナー)
  への構造対応 — 目標吸光度を絵描き寄りに: 紙の白の予約 (吸光度フロア)、
  **無彩色は描かない** (彩度ゲート、暗部は除く)、鮮やかな顔料 (彩度1.45
  リフト)、**5段のソフトウォッシュ量子化** (境界に乾き縁が乗る)。
  単純化半径は `wash` 連動
- **impressionist** (オーナー指示): 筆致を大幅に長く (len 1.7→2.5P、
  カル半径2.75P)、幅を絞って本物のストローク比に。**チューブから出した
  ままの原色** (dab色を彩度1.55リフトしてからジッタ)
- golden 再生成 (watercolor/watercolor_living/impressionist)、Vulkan 照合 pass

## 0.14.0 — 2026-08-19 (watercolor v2: 生きた水彩)

「一番がっかり」評だった v1 を P1 機構で全面再実装:
- **顔料/水分の実フィールド** (rgb=吸光度, w=湿潤): 濡れている間だけ
  滲み、水が顔料より先に走り、目標吸光度への符号付き緩和でライブ映像を
  描き直す
- **コーヒーリング**: 乾燥前線 (粗視化した湿潤勾配×乾燥帯) で風上の
  濡れた側から顔料を飲む — 描いた縁でなく乾き際に自然に濃い縁が出る。
  初稿の per-neighbor 差分転写は正帰還で格子不安定 (網戸模様) を起こした
  ため、粗スケール一方向移流+ゲートの安定形に (§実装ノート追記)
- **Beer-Lambert 透過**: `paper·exp(−吸光度)` — washes が濁らず発光する
  (v1 密度式との決定的な差)
- granulation は中間ウォッシュ帯ゲート+微振幅 (全面スティップルは
  顔料でなくノイズ、の教訓を維持)
- `paper` 画像パラメータ (dilute+2 フラグ) — 同梱 `wasm/wcpaper.jpg`
  (冷圧紙、Paper006 CC0 下地+独自バンプ合成)。デモ2ページが自動バインド
- API/パラメータ外形は不変 (wash/edge/grain/wobble/dilute)。
  golden 再生成 + watercolor_living (30tick、Vulkan max|Δ|=46)

## 0.13.3 — 2026-08-19 (filter studio: ノードグラフ化 — 多入力合成 + 人物抽出)

- **filter studio をノードグラフに全面改装** (オーナー要望「FVエディタ
  みたいな自由な構造」「複数入力を混ぜたい」「人物だけ抽出して背景だけ
  別フィルタ」): 入力 / 人物抽出 / フィルタ / 変形合成 / 出力 の5種ノードを
  ポートで自由に配線。グラフは**レイヤーツリーへコンパイル** —
  出力への各経路が1レイヤー (フィルタ列=チェーン、人物ポート=M1マスク、
  変形=position/rotation/scale/opacity/blend)
- 構造変更は fs_create でシーン再コンパイル (数ms)、ドラッグ移動/回転/
  不透明度は runtime_mutable params (implicit animation でぬるっと動く)。
  scale は YAML リテラル制約のためスロットル付き再コンパイル
- **人物抽出ノード**: portrait.html と同じ MediaPipe Selfie Segmenter
  (要CDN・追加時に動的import) → `fs_layer_set_mask`。人物/背景の2出力
  ポートで枝分かれ。+ボタン一発で既存チェーンを背景側に引き継ぐ自動配線。
  人物側マスク枝は常に手前に合成 (縦位置ルールの例外)
- **複数入力**: 入力ノードは複数可 (カメラ共有・画像/動画・サンプル)。
  変形合成ノードを挟んだ枝は角丸+影の小窓になり、プレビュー上ドラッグ
  移動・ホイール拡縮。blend 4種 (normal/add/screen/multiply)
- AI生成は「選択中ノードの枝」を書き換え。内蔵コンポーザは
  「背景だけ〜」で人物分割まで自動配線。グラフ全体をURLハッシュに保存
  (旧チェーン形式ハッシュも読める)。YAML書き出しはコンパイル結果の
  Scene 文書そのまま
- 検証: headless CDP で 背景sumie+PIP+人物クリーンの3枝合成・ハッシュ
  復元・セグメンタ読込まで実映像確認 (実人物での目視は未)

## 0.13.2 — 2026-08-19 (filter studio: GUIフィルタエディタ + AI生成)

- **wasm/edit.html — filter studio**: フィルタチェーンをノード列として
  GUI編集するデモページ。追加パレット (カテゴリ色 + 日本語ラベル + 検索)、
  ドラッグ並べ替え、ノード単位の on/off、選択ノードのスライダは
  `fs_filters_json()` から自動生成 (カタログと恒久同期)。
  チェーンは `fs_layer_clear_filters` + `fs_layer_set_filter` を順に積む
  ランタイム経路のみ — エンジン変更なし
- **AI生成**: 自然言語→チェーン。3プロバイダ切替 — 内蔵オフライン
  コンポーザ (キーワードレシピ25種 + 強弱修飾 + tone→art→finish 整列)、
  Claude API 直叩き、OpenAI互換 (Ollama / LM Studio)。APIキーは
  **localStorage のみ**に保存と明記。生成チェーンはノードが1個ずつ
  ポップインしながらリアルタイムに繋がる
- 入力: Webカメラ (既定・許可待ちの間は手続き生成サンプルが先に回る) /
  画像・動画ファイル (D&D対応) / サンプル。ワイプ比較・WebGPU→CPU
  フォールバック・解像度ラダーは filter lab と同一機構
- 共有リンク (チェーンをURLハッシュに直列化)・Scene YAML コピー・
  PNGスナップ。index.html にカード追加、README 両言語のデモ表更新

- **inkline** (`FS_INKLINE=47`): Kang 2007 の Coherent Line Drawing。
  エッジ接線流 (ETF) を P1 永続フィールドに持ち**1フレーム=1平滑反復**
  (数フレームで収束、ライブ映像では追従)。線は流れに直交する DoG を
  ストリームライン沿いに積分 — v1 anime の等方 XDoG (「結構下手」評) と
  違い、線が繋がった一本のストロークになる
- `matte` パラメータ: 0=ソースに線を合成 (チェーン向き) / 1=**透明地に
  線だけ** (オーナー指示の「線を別レイヤーに」— レイヤーとして重ねる)。
  filter が α を書く2例目 (opacity に続く)
- params: width / ink / coherence / detail / matte。
  デモに「🖋 アニメ・線v2」プリセット (anime 線なし → inkline チェーン)
- golden `inkline` (4tick 収束後 5タイル)、Vulkan 照合 **max|Δ|=1**

## 0.13.0 — 2026-08-19 (レイヤー画像マスク M1)

- **Layer.mask** — 画像駆動アルファマスク (設計書
  docs/design/layer_mask_input.ja.md): マスク画像の **A** がレイヤーの
  bounds に正規化UVでマップされ、**フィルタ後**の完成画素の α に乗算。
  `maskInvert` / `maskFeather` (論理単位、7-tap ディスク)。
  人物セグメンテーション等の NN 外段マスクの受け皿
- C++ `layer.mask(view)...` / C ABI `fs_layer_set_mask(inst, layer,
  input, w, h, invert, feather)`。Scene YAML 露出は次版
- 実装: CPU は bounds マップの bilinear α 乗算、GPU 2系は
  layer_mask.frag のフルスクリーンパス (composite 前)。golden
  `layer_mask` 5タイル、Vulkan 照合 max|Δ|=2
- 「人物だけ残して背景だけ水墨」は 2 レイヤー構成 (bg=filter+mask
  invert / fg=mask) で表現 — セグメンテーション PoC は次弾

## 0.12.1 — 2026-08-19 (sumie: 実物和紙 + フィルタ画像パラメータの一般化)

- **sumie `paper` 画像パラメータ**: 実物の和紙スキャンを紙として使える
  (FS_FILTER_STATEFUL_IMG 新設)。空きslotが無いので、紙の有無は
  plan::scaleFilterValues が chroma slot に +2 でエンコード (3バックエンド
  共通の1箇所)。論理空間でタイリング (1枚 ≈ ステージ高2.2枚分)
- **フィルタ画像バインディングの一般化**: lut/face 専用だった image
  パラメータ配線を全フィルタに開放 (CPU/Vulkan/WebGPU)
- **C ABI**: `fs_layer_set_filter_image(inst, layer, filter, input, w, h)`
- **同梱和紙** `wasm/washi.jpg`: ambientCG Paper006 (CC0) の微細粒を下地に
  楮繊維・地合いムラを合成したシームレス1024² (183KB)。filters.html が
  sumie 選択時に自動バインド
- 手続き和紙は無給紙時のフォールバックに降格し大幅減音 (2連続で
  「クロスハッチにしか見えない」評をもらった教訓)

## 0.12.0 — 2026-08-19 (persistent_buffers P1 + 生きた水墨)

- **persistent_buffers P1 実装** (設計書 status 参照): FS_FILTER_STATEFUL
  宣言でフィルタがレイヤー毎の永続 ping-pong フィールド (32F固定・
  nearest契約) を持てる。CPU/Vulkan/WebGPU 3バックエンド配線済み。
  golden は「N回render→比較」形式を追加 (`sumie_living`, 30 tick,
  Vulkan max|Δ|=31)
- **sumie v2 — 生きた水墨** (オーナー指示: 中華風・動的滲み):
  ノイズ滲みを廃止し、墨/水/定着の実フィールドを毎フレーム拡散。
  太い粗スケール輪郭+潑墨マッスから**目標濃度への差分注入**、
  水が先に走り墨は濡れた紙だけを歩き、乾燥で定着、再湿潤チャーンで
  滲みが呼吸し続ける。ライブ映像ではシーン変化に追従して描き直る。
  パラメータ外形は不変 (ink/bleed/dry/outline/chroma)
- filters_tour 等の1回レンダでは1tick分 (骨描+薄いウォッシュ) しか
  見えない — 様式の本体は連続レンダで立ち上がる (仕様)

## 0.11.1 — 2026-08-19 (印象派 v2 / wobble 分離 / v2 リサーチ)

オーナーフィードバック起点の改修 (評定と方針: docs/design/art_filters.ja.md §7)。

- **impressionist v2**: 向きの基底場をカールノイズ (ノイズポテンシャルの
  回転 = 発散ゼロ場) に変更 — 筆致の列が**ゴッホの渦に閉じる**。筆致を
  細長く (len 1.7P/wid 0.42P)。`time` パラメータ追加 (slot 1 = デモ自動
  アニメ規約): 渦がゆっくり流れ・dab が呼吸し・絵具がストロークに沿って
  這い・色振動が明滅する「生きた絵」。canvas は定数化して枠を確保。
  **API 変更**: params は stroke/time/vibrance/flow/relief に
- **wobble** (`FS_WOBBLE=46`) 新設: notebook に内蔵だったページ振動を
  独立フィルタ化 (オーナー指示)。2 オクターブのベクタノイズ歪みを
  `fps` 回/秒で引き直すストップモーション式 (fps=0 で連続ドリフト)。
  任意のフィルタとチェーン可
- **notebook**: wobble と time を除去し静止画化。params は
  scale/grid/chroma に (**API 変更**)。振動が欲しいときは
  `.filter(Notebook()).filter(Wobble().time(t))`
- §7 リサーチ追記: 様式別 SOTA (Flair 水彩 / Paint Transformer 系油絵 /
  White-box Cartoonization・AnimeGANv3 / StreamDiffusion) と野心の階段
  (A 単一パス / B persistent_buffers / C ニューラル外段)。次段の本線は B

## 0.11.0 — 2026-08-19 (アートフィルタ 6種: 40〜45)

絵画様式ファミリー。全て論文アルゴリズムからの独自実装 (MIT、shadertoy
借用なし)。調査サーベイ・採用判断・パラメータ設計の凍結表は
`docs/design/art_filters.ja.md`。

- **anime** (`FS_ANIME=40`): セル画。Winnemöller 2006 の実時間抽象化を
  単一パスに畳む — bilateral 前平滑 (mix可変) + **輝度のみ** soft
  quantization (chroma 保存なので `toon` と違い色相が回らない) +
  XDoG (2011) インク線 (smoothstep 連続閾値) + 彩度リフト。
  levels / lines / width / smooth / vivid
- **watercolor** (`FS_WATERCOLOR=41`): 水彩。Bousseau 2006 の顔料密度式
  `C' = C·(1−(1−C)·(d−1))` に全現象を畳む — エッジ顔料溜まり・
  granulation (中間調ゲート) ・ウォッシュむら・手ぶれ UV 歪み・
  ハイライト希釈・紙 tooth 照明。wash / edge / grain / wobble / dilute
- **sumie** (`FS_SUMIE=42`): 水墨画。濃度側指数のトーンカーブ (中間調が
  紙へ抜ける=余白が生きる) + soft 4段ウォッシュ + 粗視化接線に沿う運筆
  スメア + ストローク空間ノイズの掠れ + リング平均の滲みハロー +
  筆圧ゆらぎ付き輪郭 + 和紙。ink / bleed / dry / outline / chroma (淡彩)
- **impressionist** (`FS_IMPRESSIONIST=43`): 印象派筆致。Litwinowicz 1997
  (勾配接線ストローク+エッジクリップ) と Hertzmann 1998 (粗→細の多層) を
  ギャザー型に再定式化 — ジッタ種格子 3×3 逆引き×2層、カプセル距離場の
  dab、**輝度保存**の筆触分割色振動、ストローク空間剛毛ノイズの impasto
  照明、キャンバス地。stroke / vibrance / flow / relief / canvas
- **stainedglass** (`FS_STAINEDGLASS=44`): ステンドグラス。Worley 1996
  F1/F2 セルラー — F2−F1 の smoothstep が鉛線。パネルは posterize+彩度+
  パネル毎色相ネジ、透過光グラデ+ガラスむら。
  size / lead / irregular / saturate / light
- **pixelart** (`FS_PIXELART=45`): ドット絵。ブロック平均 + Bayer 4×4
  ordered dither (再帰構成を算術で、テーブル無し) + チャンネル量子化 n³
  色。size / colors / dither / saturate
- golden: 5シーン追加 (anime は通常許容で CPU/Vulkan max|Δ|=1 を実測。
  watercolor/sumie は notebook と同じジッタ読み waiver、impressionist/
  stainedglass/pixelart はハード選択 waiver — 根拠コメント付き)
- filters_tour のカタログは自動で 6 タイル増える (テーブル駆動)
- wasm/dist 再ビルド済 (`./wasm/build.sh --webgpu`、naga の WGSL 機械翻訳が
  新6本を通過)。node スモークで fs_filters_json 45種 + anime 適用 Scene の
  CPU レンダを確認。filters.html にプリセット6個追加

## 0.10.1 — 2026-08-16 (フィルタ大量追加: 34〜39)

- **bokeh** (`FS_BOKEH=35`): レンズボケ。絞り形状指定 (blades: 0=円形,
  3+=多角形)・回転・擬似HDRハイライト。黄金比スタガの極座標ギャザー、
  独自実装 (MIT)
- **oilpaint** (`FS_OILPAINT=36`): Kuwahara (1976) の油絵調。最小分散
  象限平均+筆方向ジッタ+ポスタライズ。仕上げは `median` をチェーン。
  独自実装 (MIT)
- **ntsc** (`FS_NTSC=37`): NTSCコンポジット信号の変調→復調を実物理で。
  色にじみ・レインボー (artifacts)・フリンジ (fringing)・ドットクロール
  (time駆動)・RFノイズ (信号領域に注入)。MAME ntsc.fx (BSD-3) の
  単一パス構造 + ntsc-adaptive のクロスフィード定式を独自実装
- **crt** (`FS_CRT=38`): ブラウン管。リニア光の輝度依存ビーム走査線・
  フォスフォーマスク (グリル/スロット、歪み前空間配置でモアレ根治)・
  グロー・デコンバージェンス・曲率+角丸+ビネット。Lottes (PD) +
  Cathode-Retro (MIT) 系譜、GPLコード不使用。
  `filter(Ntsc().time(t)).filter(Crt())` で「超絶リアル」チェーン
- **fractal** (`FS_FRACTAL=39`): 永遠に反復しない自己変形KIFSフラクタル
  飛行ジェネレータ (flight/time/morph/glow/blend, host駆動time,
  WebGPUデモ wasm/fractal.html)
- **notebook** (`FS_NOTEBOOK=34`): 方眼ノートに鉛筆スケッチ。
  flockaroo の "notebook drawings" (shadertoy XtVGD1) 移植 —
  **CC BY-NC-SA 3.0 のため非商用限定**(このフィルタのみ。他は MIT のまま)。
  弧状ストローク畳み込み+色鉛筆の確率スクリーニング+方眼紙。
  パラメータ: scale / time(ホスト駆動) / wobble / grid / chroma。
  原作からの意図的変更: ノイズテクスチャ→value noise(CPU/GPU golden 一致の
  規範)、ビネット・緑退色は削除(既存フィルタで合成)、zoom の視点移動を
  廃してストローク寸法のみ拡大
- 設計書 draft: `docs/design/persistent_buffers.ja.md` — フレーム間永続
  バッファ+複数パス宣言 (lowpoly JFA / reaction-diffusion / CRT残光の
  共通基盤)。設計凍結までコード無し

## 0.10.0 — 2026-08-14 (Phase L4: Scene で UI が動く)

- **UI コントロールの Scene 語彙**: `button`(label) / `switch`(on) /
  `slider`(value) / `gauge`(center, radius, value)。コンパイラは C++ 著者と
  同じ ui:: プレハブを生成する(§10-1: 新しい描画機構ゼロ)。button/switch/
  slider はレイヤーの `frame` 必須(validator が要求)
- **$params 束縛**(§10-2): `switch: { on: $params.light }` — setParam が
  コントロールを駆動する(プログラム変更はイベントを発火しない、C++ の
  setter と同じ)。gauge は params でライブ表示
- **イベントの出口**(§10-4): `CompiledScene::onUiEvent` に全コントロールの
  ユーザー操作が `{id, control, value, flag}` で届く
- **scene_web がインタラクティブに**: ページの pointer イベントを論理座標に
  正規化して GET /pointer → フレーム境界で `stage.pointerDown/Move/Up` に
  注入(§10-3)。UI イベントは /status にミラー、`--events /topic` で
  std_msgs/String JSON としても publish — **§14 L4 完了条件
  (ポインタ注入で動き、イベントが topic/コールバックに出る)を充足**
- E2E: .fvs 宣言のボタン/スイッチ/スライダーをブラウザ経由の HTTP ポインタで
  操作し、`{"id": "start", "control": "button", ...}` が ROS トピックに
  届くことを実機で確認


## 0.9.0 — 2026-08-14 (Phase L3: インスペクター + ROS binding + scene_node)

- **シーンインスペクター**（§13-3、`scene/inspector.hpp`）: 全レイヤーを
  ステージ空間に固定した配置表（resolved bounds・local→stage 変換・実効
  opacity・描画順）。`visibleAt` が「座標 (x,y) に見えているものは何か」を
  最前面から返し、`inspectJson` / `atJson` が JSON で答える。リンターは
  この同じ配置計算の上の検査になった（幾何の定義は一箇所）
- scene_web に `/inspect` と `/at?x=&y=` を追加（配置スナップショットは
  値のみ共有 — HTTP スレッドはライブシーンに触らない）
- **binding 文書**（`fluent.binding/v1alpha1`、`scene/binding.hpp`）:
  「何が要るか」（Scene の inputs）と「この機体でどこから来るか」（topic /
  message_type / qos / converter）の分離。converter カタログは単一表、
  message_type の適合・adapter・QoS 語彙を activation 前に全て検証。
  `validateBindingAgainstScene` が未宣言入力(エラー)・型不適合(エラー)・
  未バインド入力(情報: fallback 提示)を判定
- **scene_node**（本番 ROS 2 アダプター）: scene.fvs + binding.fvb →
  購読(Image / CompressedImage / Detection2DArray / String / Polygon)、
  変換はレンダースレッドで latest-wins、描画(Vulkan、無ければ CPU)、
  出力 Surface を Image / CompressedImage で publish。scene は
  ホットリロード(検証→フレーム境界 swap)、binding は寿命固定(配線変更 =
  再起動、設計どおり)
- 実機 E2E: d405 CompressedImage 30fps + String トピック → 合成 20fps を
  `/fluent_scene/composite` に配信、ライブ文字列と実映像を確認
- **`aspa_json_to_detection2d` converter**: aspa 認識スタックの独自 JSON
  (`class` / `conf` / `box_xyxy` 画像ピクセル座標、`\uXXXX` エスケープの
  日本語ラベル含む)を boxes へ。image_size でステージ論理座標に変換。
  実機 d405 + 検出枠 HUD(角丸・ラベル・スコア・平滑化)を描画確認

## 0.8.1 — 2026-08-14 (scene_web: 編集→保存→原子的差し替え)

- `tools/scene_web` — Scene 文書(.fvs)のライブプレビューサーバー。ファイルを
  保存すると validate → compile → lint を通し、**フレーム境界で原子的に
  差し替え**（§2 の activate）。壊れた編集は旧画面を保持したまま赤バナーで
  エラー行を表示 — 壊れたフレームが表に出る経路は存在しない
- `--image input=topic` で ROS 2 CompressedImage を宣言済み `$inputs.<名前>`
  へ接続（ROS 環境を source してビルド）。未接続の image 入力は
  プレースホルダーパネルのまま = 入力契約がそのままデモになる
- GET /status が digest・リロード回数・エラー・lint 警告を JSON で返す
  （ページの状態表示はこれを映すだけ）

## 0.8.0 — 2026-08-14 (Phase L2: Scene 宣言層)

- **Scene v1alpha2**（`fluent_scene::fvs`）: レイヤーツリー YAML（.fvs）
  → Stage 構築。§1.3 の契約を実装 — 型検査・未知キー拒否・`$inputs` /
  `$params` 参照解決・必須/排他フィールド検査を**実行前に全て**行う
  （`parseScene` が ok なら compile は資源上限以外で失敗しない）
- **同一出力の契約**: コンパイラは C++ 著者と同じ Layer API で木を組み、
  明示されたフィールドだけを適用する（既定値の定義は C++ 側に一度だけ）。
  §2 の canonical 例を YAML と C++ の両方で描き**ピクセル一致**を
  scene_tests の golden で検証
- **並べ替え不変 digest + 正準フォーマッタ**（§13-8）: 正準形は一つ
  （表順のキー・明示フィールドのみ・安定数値表記）。digest はその
  SHA-256。マッピング順・コメント・空白の変更では変わらない
- **単一定義のメタデータ表**: `shared/contents_def.h`（content 13種の
  フィールド/型/既定値）と `shared/attributes_def.h`（§6.2 属性15種)を
  新設 — filters_def.h 方式。validation・describe・fmt の語彙は全て
  ここから導出、既定値は C++ 構造体とテストで突き合わせ
- **`fvsc` CLI**: `validate`（型検査+リンター+digest）/ `preview`
  （PPM レンダリング。未接続の image 入力は入力名入りプレースホルダー）/
  `fmt` / `digest` / `describe --json`
- **能力の自己記述**（§13-1）: `describe --json` が content/属性/
  フィルタ30種/入力・パラメータ型/上限を機械可読カタログで返す
- **デザインリンター**（§13-2）: コントラスト比 4.5:1（実レンダリング
  ピクセルに対して測定）・テキストはみ出し・完全遮蔽（protected の
  遮蔽はエラー = §13-4）・画面外配置
- **ランタイム表面**: `CompiledScene` — `setImage/setText/setPoints/
  setBoxes`（宣言型チェック+容量クランプ）と `setParam`（§9 の animate
  宣言 → Transaction、layer transition フォールバック）。image 入力の
  fallback は placeholder / hide / hold
- YAML パーサ（依存ゼロの有界サブセット・fluent_scene から移植）に
  複数行フローコレクション対応を追加（§2 の canonical レイアウト）
- 設計書 §2 の YAML 例を修正: テキスト位置は content の `position`
  フィールドへ（レイヤー属性 position は anchor 配置なので同じ絵に
  ならない — golden が機械検証）

## 0.7.0 — 2026-08-14 (本物の水面屈折波紋)

- `fs_ripple` フィルタ（filters_shared.h、30種目）: 拡大する波面の周りの
  減衰正弦波でサンプリング位置を変位させ、**下の映像を実際に歪ませる**。
  CPU/GPU 同一ソース（GPU parity max_diff=0）
- FilterUnit に `CoordX`/`CoordY` を追加 — フィルタパラメータとして
  レイヤーローカルの論理座標を渡すと、レンダラーがバッファ uv へ変換
  （§5-3 の座標版）
- `fx::Ripple` を屈折駆動に全面書き換え: リングレイヤーを廃止し、対象
  レイヤー（=水面）のフィルタチェーンに波を積む方式へ。既存フィルタは
  base として保持、破棄時に原状復帰。`max_waves`（既定8）で有界
- stage_web: カメラ映像が水面に（クリック=スプラッシュ、ホバー=航跡）。
  デモ映像を畝のある圃場パターンに刷新

## 0.6.0 — 2026-08-14 (stage_web: ブラウザで触れる)

- `tools/stage_web` — Stage の Surface を MJPEG で配信し、ブラウザの
  mouse/touch/pointer イベントを論理座標に正規化して
  `stage.pointerDown/Move/Up` へ逆流させる自己完結 Web アプリ
  （POSIX ソケット + libjpeg のみ。ページ側 UI ロジックはゼロ）
- デモ HUD: 収穫開始/停止・ライトスイッチ+インジケーター・速度スライダー・
  モード segmented・プロファイル dropdown・バッテリーゲージ・検出枠・
  ホバー波紋。Vulkan バックエンドで描画（無ければ CPU にフォールバック）
- E2E 実証: HTTP 経由のポインタ注入だけで全コントロールが反応

## 0.5.0 — 2026-08-14 (Phase L1: Vulkan バックエンド)

- `VulkanRenderer` — CPU リファレンスと同一出力の GPU 本番バックエンド。
  全 golden シーンを GPU で通過（ほぼ全て max_diff 1〜2、閾値クリフを持つ
  フィルタのみ許容差内の残差）。1080p の代表 HUD で 5.6ms/frame
  （CPU 94.7ms、~17倍。毎フレームの CPU 読み戻し込み）
- 単一ソースの完結: shapes_shared.h / filters_shared.h が GLSL として
  SPIR-V にコンパイルされ GPU で実行される（ビルド時 glslc、実行時の
  シェーダーコンパイルはゼロ）
- CPU/GPU 共通のプラン層 src/render_shared.hpp（offscreen 判定・extent・
  dash 分割・フィルタ論理単位スケール）— 両バックエンドが同じ判断で
  ツリーを歩く
- ブレンド4種を premultiplied 固定機能式に統一（CPU 実装 = GPU ブレンド
  ファクタと恒等。Add/Multiply/Screen の意味論を両バックエンドで一致）
- golden_tests --renderer=vulkan（ctest: golden_tests_vulkan、GPU が無い
  環境ではスキップ）と examples/bench（CPU vs GPU 実測）
- 画像/フィルタのサンプリングを CPU とビット同型の texelFetch 実装に
  （pixelate のブロックずれ・sourceRect 境界の1texel差を根治）

## 0.4.0 — 2026-08-14 (波紋エフェクト + Dropdownスクロール)

- `fx::Ripple`（effects.hpp 新設）: ポインタの軌跡に広がって消える
  リング波紋（トレイル + タップの二重スプラッシュ）。円レイヤー +
  Transaction のみ、`max_rings` で有界、dt 駆動で決定的
- Dropdown: `max_visible` 超のリストをドラッグスクロール（6単位で
  タップ/ドラッグ弁別、開いた時に選択行へ自動スクロール）
- テスト: ripple 生成/間引き/上限/消滅、dropdown スクロール+選択。
  golden `ripple_t015`、example `ripple_demo`

## 0.3.0 — 2026-08-14 (UIカタログ完成)

- `ui::Slider`（0..1。ドラッグはポインタ直結・onChange連続、setValueは
  アニメ。fillはクリップ式で角丸を歪めない）
- `ui::Segmented`（2〜5択の排他選択。ピルがスライド、ラベル色が状態表示）
- `ui::Gauge`（表示専用ラジアル。`Layer::setArc` データ更新APIを追加）
- `ui::Dropdown`（root末尾ポップアップ+透明スクリムの外側タップ閉じ+
  上下自動開き+シェブロン回転。初版は max_visible で切りスクロールなし）
- ハンドラ実行中の自己remove（ポップアップが自分を閉じる）を安全化
  （配送時にハンドラをコピーして呼び出す）
- テスト: ui_tests に4コントロール分を追加、golden `ui_catalog`、
  example `ui_catalog`

## 0.2.0 — 2026-08-14 (Phase L4 先行: UIコントロール)

- ポインタ注入（§10-3）: `Stage::pointerDown/Move/Up/Cancel`。Web ビューア
  のクリック・タッチ・VR レイを論理座標の3呼び出しに正規化する統一入力口。
  キャプチャ追跡（UIControl 型）、`Layer::onPointer`、interactive hit-test
  （handler の無いサブツリーはポインタに対して透明 = disabled 素通し）
- `ui::Button`（momentary）と `ui::Switch`（トグル）: プレハブサブツリー +
  状態=属性上書き + Transaction 遷移（§10-1/2）。スタイルは
  ButtonStyle / SwitchStyle で差し替え
- テスト: ui_tests（タップ・スライドオフ・キャンセル・disabled 素通し・
  トグル中間フレーム・ジェスチャ中の remove 安全性）+ golden
  `ui_controls` + example `ui_demo`

## 0.1.0 — 2026-08-14 (Phase L0)

初版。Stage API + CPU リファレンスバックエンド。

- CALayer 準拠のレイヤーツリー（bounds / anchor / position / frame 糖衣、
  左上原点・+y下の単一座標系、zPosition なし）
- content 13種（image / text / line / polyline / polygon / rect / circle /
  circles / arc / arrow / crosshair / grid / boxes）を SDF 描画
- 属性: opacity / hidden / masksToBounds / cornerRadius / shadow / border /
  background / blend(normal, add, multiply, screen) / rotation / scale /
  transform
- フィルタ 29種（GLSL∩C++ 単一ソース + X-macro メタデータ。長さ系
  パラメータは論理単位）
- Transaction による implicit animation（dt 注入・決定的、進行中の
  再変更は現在値から継続）
- 検出ボックスの時間平滑化（id / 最近傍対応、時定数指定）
- テキスト: FreeType + HarfBuzz（日本語、欠落グリフは決定的代替）
- hit-test（CALayer 規則）、StageLimits（構造は throw、データは
  切詰め+診断）
- テスト: stage_tests（幾何・アニメ・limits・フィルタ表・決定性）+
  golden 7シーン + 全 example の CI 実行

既知の制限（cookbook 末尾に記載）: arc の dash / Cap::Butt 未対応、
テキスト1行のみ、sRGB 空間でのフィルタ計算。
