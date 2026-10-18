# 実行時のレイヤー追加・削除 (設計書 draft-1)

status: **実装済** (2026-10-19、0.17.0)。C++ (`CompiledScene::addLayers` /
`removeLayer` / `layerIds` / `hasLayer`、`Layer::removeWhenSettled`) +
C ABI (`fs_add_layers` / `fs_remove_layer` / `fs_layer_ids` /
`fs_has_layer`)。Kepler の stage (音声アシスタント UI の中央) が初代顧客。
date: 2026-10-19

## 1. 動機

Kepler の stage は1枚の Scene に「いつもの姿 (焦点面の図)」を置き、話題に
応じて写真・グラフ・関係図を出し入れする。今の Scene 層には「実行中の
構造変更」の口が無く (§1.3 の表: Scene 側は「×、文書編集→再コンパイル→
原子的差し替え」)、stage.html は中身が変わるたびに `fs_create` で全体を
作り直していた。作り直しは:

- 走っている全アニメーション (param の animate、transition) を初期値に戻す
- 全 input を再給餌させる (画像バッファ・テキスト)
- 出し入れそのものをアニメーションにできない (前の木と次の木は別物)

全差し替え (§2 の validate → compile → activate) は「画面の版を替える」
操作としては正しいまま残す。足りないのは**同じ画面の中で部品を足し引き
する**操作で、これは Stage (C++) では最初から `layer.group()` /
`remove()` で書ける。Scene 層にその対応物を作る。

## 2. 原則 — §1.3 の契約を崩さない

| 契約 | 守り方 |
|---|---|
| 拒否は実行前に全部 | 足すものは **Scene 断片 (fragment)** として `parseFragment` で検証してから触る。検証 NG・コンパイル NG・予算超過のどれでも**シーンは呼ぶ前と完全に同じ** (コンパイル途中の失敗はロールバック) |
| `doc()` / `digest()` が画面の監査上の正体 | 編集は所有文書 `doc_` にも反映する (断片の木を親の `sublayers` に接ぎ木、削除は部分木ごと抜く)。digest は編集ごとに再計算。`format(doc())` は今の画面を再コンパイルできる完全な文書 |
| YAML と C++ は同じ絵 | 足すレイヤーも compile() と同じ Builder (同じ Layer API 呼び出し) で作る |
| 時間は dt でしか進まない | 出入りのアニメーションも Transaction + `Stage::advance` だけ。退場後の切り離しも advance の中 |

## 3. 断片 (fragment) の形

```yaml
inputs:                                   # 任意
  photo: { type: image.rgba8, update: on_change, fallback: hide }
params:                                   # 任意
  photo_a: { type: f32, default: 1 }
layers:                                   # 必須
  - id: photo
    opacity: $params.photo_a
    content: { image: { source: $inputs.photo } }
```

- `schema` は任意 (書くなら一致必須)。`stage` / `meta` はホスト文書のもので、
  断片に書くと拒否
- 参照 (`$inputs` / `$params`) はホストの宣言と断片自身の宣言の両方に解決
- **宣言はシーン全体のもの**。断片が宣言した input/param はシーンに加わり、
  レイヤーを消しても残る。ホストに既にある名前の再宣言は**同一なら可**
  (読み捨て)、違えば `validate.conflict`
  - 同一再宣言を許す理由: 断片を自己完結に書ける (どのシーンに足しても
    同じ意味) うえ、消して同じ断片をもう一度足す — stage の典型操作 —
    がそのまま通る
  - レイヤーと一緒に宣言を消さない理由: 別の断片が後からその param を
    参照しているかもしれない。宣言を参照カウントで生死させると、
    「消したら他所の束縛が宙に浮く」を検査で防げなくなる。残った未使用
    宣言は束縛ゼロで無害 (setParam しても何も動かないだけ)
- id はホストの全宣言 id と重複不可。id はホスト側の削除・入れ子先指定の
  ハンドルなので、後から触るレイヤーには id を付ける

## 4. どこに足すか — 「枠グループ」の作法

`addLayers(parent_id, yaml)` は宣言済みレイヤー `parent_id` (空 or `root`
ならステージ直下) の**サブレイヤー末尾 (= 最前面)** に足す。挿入位置の
指定は設けない (§6.1 「zIndex は導入しない」— 合成順は木の順だけ)。
重なり順を決めたいときは、ホスト文書に**空の枠グループ**をその深さに
宣言しておき、そこへ足す:

```yaml
layers:
  - id: backdrop          # 一番奥
  - id: media             # ← 写真・グラフはここへ足す
    frame: [360, 40, 1520, 1000]
    transition: { duration: 0.35, ease: ease_out,
                  enter: { opacity: 0, scale: 0.92 }, exit: { opacity: 0 } }
  - id: figure            # 図は常に手前
```

枠は配置 (frame を rect param で動かせる) と出入りの演出を一手に持つ。
中身は枠の座標系で書けばよく、レイアウトが変わっても断片は変わらない。

## 5. 出入りのアニメーション — Core Animation の写像

CA では sublayer の追加/削除そのものにアクション (`kCAOnOrderIn` /
`kCAOnOrderOut`、既定はフェード) が付く。Scene では既存の `transition`
宣言を拡張して表す:

```yaml
transition:
  duration: 0.35
  ease: ease_out
  enter: { opacity: 0, scale: 0.92 }   # 追加時: ここから宣言値へ
  exit:  { opacity: 0 }                # 削除時: ここへ動いてから切り離す
```

- キーは補間できる幾何・合成属性の4つ: opacity / position / scale
  (裸のスカラー可) / rotation。値はリテラルのみ
- `enter` / `exit` 省略時はどちらも `{ opacity: 0 }` (CA の既定と同じフェード)
- **どの transition が効くか**: そのレイヤー自身の `transition`、無ければ
  一番近い祖先の `transition` (= 枠グループの演出が中身に効く)、それも
  無ければ 0.3 s ease_in_out のフェード
- 演出は `animated = true` で呼んだときだけ。false なら即時 (CA で
  `setDisableActions(true)` するのと同じ)。コンパイル時に最初からある
  レイヤーには enter は掛からない (CA でも初回表示は暗黙アクション無し)
- enter は「宣言値 (param 束縛ならその現在値) を目標に、enter 値から
  補間」。position を enter に書くと position が明示値に固定される
  (auto 配置だったレイヤーでも) — 既定のフェードはこの副作用を持たない

### 5.1 退場中のレイヤー — `Layer::removeWhenSettled`

削除アニメーションの間、レイヤーは**描画されるが文書のものではない**:

1. `removeLayer(id, true)` は即座に束縛を外し (param / input / UI
   コントロール / transition)、文書から抜き、digest を更新する。id は
   この瞬間に空く — 退場中に同じ id の新しい中身を足してよい
   (写真の差し替えは「古いのが消えていく上に新しいのが現れる」)
2. exit 値へ Transaction で動かし、Stage 側の `removeWhenSettled()` を呼ぶ
3. `Stage::advance` は step の後、自分の属性に補間中のものが無くなった
   退場レイヤーを `remove()` する

退場中は `find()` から見えず、ヒットテストにも当たらない (捕捉中の
ジェスチャも落とす)。これは C++ 作者にもそのまま使える Stage の語彙
(`{ Transaction t(0.3f); card.opacity(0); } card.removeWhenSettled();`)
で、Scene 層はそれを呼ぶだけ。退場中の祖先が先に外れれば子孫の
保留も一緒に消える (二重解放しない)。

## 6. 足したレイヤーの初期状態

- **param は現在値から**: 束縛先は宣言の既定値ではなく、その時点の
  param 値で作る (`setParam("tint", …)` 済みなら新しいレイヤーもその色)。
  CompiledScene は param ごとの現在値を持つ
- **input は最新データから**: 既に給餌済みの input に束縛されたレイヤーは
  最後に来たデータ (画像・テキスト・点列・箱) で始まり、fallback は即解除。
  画像は借用ビューなので寿命の約束は既存レイヤーと同じ
- 呼び出し側が Transaction の中で呼んでも、構築は即時 (内部で
  `Transaction(0)` を開く)。演出は enter だけが担う

## 7. 予算とロールバック

断片は今のステージの実数 (`layerCount()`) に対して検査する: 層数
(+ 画像 placeholder 用に最大2枚/束縛) と深さ (親の深さ + 断片の深さ)、
sequence 容量。UI コントロールの内部レイヤーは compile() 同様この見積に
入らないが、上限超過で Stage が投げる `std::length_error` は捕まえて
`compile.budget` にし、ロールバックする。コンパイル途中の拒否
(例: mask の source が未宣言 input — パースは通る) も同様に、作りかけの
レイヤー・束縛・接ぎ木した宣言を全部戻す。

## 8. 内部 — nodes_ の再同期

`nodes()` (文書ノード ↔ 実レイヤー、inspector / linter が読む) は
`const LayerDecl*` を持つため、接ぎ木で `sublayers` ベクタが再確保されると
兄弟の宣言ポインタが無効になる。編集のたびに `rezipNodes` で作り直す:
「文書レイヤーの集合」(以前の nodes_ のレイヤー + 新規) を手掛かりに、
文書の木と Stage の木を並べて歩く。宣言済みサブレイヤーは宣言順に
append され編集もその順を崩さないので、各レイヤーの子のうち文書レイヤー
だけを拾えば宣言の子と 1:1 に対応する (placeholder 部品・コントロール
内部・退場中のレイヤーは集合外なので飛ばされる)。

## 9. 退けた案

| 案 | 退けた理由 |
|---|---|
| 編集のたびに全文書を再コンパイルし、旧 Stage から状態を移植 | アニメーションの途中状態・box の平滑化・永続フィルタバッファの移植が網羅できない。差分を取るより足し引きが素直 |
| 断片の宣言をレイヤーと一緒に削除 (参照カウント) | §3 の通り、他の断片の束縛が宙に浮く。検査で防げない状態を作らない |
| 挿入位置 (index) 引数 | §6.1 の「zIndex を入れない」に反する。枠グループで足りる |
| 断片を YAML ではなく C++ で組む API | C++ 作者は Stage を直接触れる (§1.3)。Scene 層の利用者 (wasm / AI) に必要なのは検証つきの宣言的な口 |
| 退場中も id を保持 (完了まで同名追加を拒否) | 差し替えのたびに待たされる。退場中は文書から外す方が CA の「モデル木から外し、プレゼンテーションだけ残る」に近い |
