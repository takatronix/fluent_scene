# 文字（テキスト）API

テキストは content の 1 種（`TextContent`）。C++ は `Layer` のセッター、
Scene YAML は `content: { text: { ... } }` のフィールド、wasm/JS は Scene YAML ＋
いくつかの関数で、**同じ語彙**を使います。単位はすべて論理単位（px 相当）。
仕組みは [docs/design/text.md](../design/text.md)。

どの大きさでも鮮明です: 小さい正立文字は画面のピクセルサイズちょうどで
ラスタライズ、大きい・回転・拡大・縁取りの文字は距離場（SDF）で描きます。
CJK の字は使われた時にその場で作られ、メモリは上限付きです。

## 早見表

| 目的 | C++ (`Layer&`) | Scene YAML (`text:` のキー) |
|---|---|---|
| 文字列 | `stage.text("…", pos)` / `.setText()` | `text` / `source: $inputs.x` |
| 大きさ | `.size(px)` / `.sizePt(pt)` | `size` / `size_pt` |
| 横の基準・行揃え | `.align(Align::Left/Center/Right)` | `align: left/center/right` |
| 縦の基準 | `.verticalAlign(VAlign::Top/Middle/Baseline/Bottom)` | `vertical_align: top/middle/baseline/bottom` |
| フォント | `.font("sans"/"serif"/"mono"/登録名)` | `font` |
| 太さ | `.fontWeight(100..900)` / `.bold()` | `weight: 700` |
| 斜体 | `.italic()` | `italic: true` |
| 字間 | `.letterSpacing(px)` | `letter_spacing` |
| 行の高さ | `.lineHeight(倍率)`（0 = フォント既定 ≈1.45） | `line_height` |
| 等幅数字 | `.tabularNumbers()` | `tabular: true` |
| ヒンティング（小さい字の縦線を画素に揃える。既定は無し＝ブラウザと同じ柔らかい AA） | `.textHinting()` | `hinting: true` |
| 折返し幅 | `.maxWidth(px)` / `.wrap(false)` | `max_width` / `wrap` |
| 行数・高さの上限 | `.maxLines(n)` / `.maxHeight(px)` | `max_lines` / `max_height` |
| 省略記号 | `.ellipsis()` | `ellipsis: true` |
| 自動縮小 | `.autoFit(最小サイズ)` | `min_size` |
| 色 | `.color(c)` | `color` |
| 縁取り | `.textOutline(幅, 色, OutlinePosition::Outside/Inside/Center)` | `outline_width` / `outline_color` / `outline_position` |
| グラデーション | `.textGradient(from, to, 角度)` | `gradient_from` / `gradient_to` / `gradient_angle` |
| 影 | `.shadow(dx, dy, ぼかし)` | レイヤーの `shadow: { offset, radius, color, opacity }` |
| 発光 | `.glow(半径, 色)` | `glow_radius` / `glow_color` |
| リッチテキスト | `.spans({...})` / `.setSpans()` | `spans: [{ text, color, size, weight, italic, font }]` |
| 計測 | `stage.textMetrics(layer)` | （JS）`fs_measure_text` / `fs_layer_text_metrics` |

既定値はすべて「旧来の 1 行テキスト」と同じ見た目・位置になります
（`position` = 1 行目の行ボックスの左上）。

## C++

```cpp
#include <fluent_scene/fluent_scene.hpp>
using namespace fluent_scene;

Stage stage(1280, 720);

// 大きな速度表示: 太字・等幅数字・縁取り、右端と縦中央を (1200, 360) に
Layer& speed = stage.text("88.5", {1200, 360})
                   .size(160).bold().tabularNumbers()
                   .align(Align::Right).verticalAlign(VAlign::Middle)
                   .textOutline(6, {0, 0, 0, 0.8f});
speed.setText("102.3");   // 毎フレーム変えても安い（字形はアトラスに残る）

// 日本語の段落: 幅 480 で折り返し（禁則処理つき）、3 行で「…」
stage.text("長い説明文……", {40, 40}).size(22)
     .maxWidth(480).lineHeight(1.6f).maxLines(3).ellipsis();

// 1 行に収まるまで縮める（最小 12）
stage.text("とても長いタイトル", {640, 40}).size(48)
     .maxWidth(600).wrap(false).autoFit(12).align(Align::Center);

// リッチテキスト
TextSpan label{"速度 "}; label.has_color = true; label.color = Color::Gray;
TextSpan value{"88"};    value.size = 72; value.weight = 700;
TextSpan unit{" km/h"};  unit.has_color = true; unit.color = Color::Gray;
stage.text("", {40, 600}).size(28).verticalAlign(VAlign::Baseline)
     .spans({label, value, unit});

// 装飾
stage.text("NEON", {40, 200}).size(64).bold().color(Color::White)
     .glow(14, {1, 0.2f, 0.8f, 1});
stage.text("グラデーション", {40, 300}).size(56).bold()
     .textGradient({0.3f, 0.9f, 1, 1}, {0.8f, 0.3f, 1, 1});

// 計測（レンダラ不要）: レイアウトエンジンはこれで配置を決める
TextMetrics m = stage.textMetrics(speed);
// m.box（行ボックス）, m.ink（字形の外接）, m.baseline, m.ascent, m.descent,
// m.line_height, m.lines, m.line_widths, m.size（自動縮小後）, m.truncated

// フォントの追加（プロセス共通）
registerFont("rounded", "/usr/share/fonts/truetype/xxx/Rounded-Bold.ttf", 700);
stage.text("丸ゴ", {40, 400}).font("rounded").bold();
```

`pt(12)` は 16 論理単位（1 pt = 4/3 px）。`FontWeight::Bold` などの定数あり。

## Scene YAML

```yaml
schema: fluent.scene/v1alpha2
stage: { size: [1280, 720] }
layers:
  - id: speed
    content: { text: { text: "88.5", position: [1200, 360], size: 160, weight: 700,
                       tabular: true, align: right, vertical_align: middle,
                       outline_width: 6, outline_color: [0, 0, 0, 0.8] } }
  - content: { text: { text: "長い説明文……", position: [40, 40], size: 22,
                       max_width: 480, line_height: 1.6, max_lines: 3, ellipsis: true } }
  - content: { text: { text: "NEON", position: [40, 200], size: 64, weight: 700,
                       glow_radius: 14, glow_color: [1, 0.2, 0.8, 1] } }
  - content: { text: { spans: [ { text: "速度 ", color: [0.6, 0.6, 0.6, 1] },
                                { text: "88", size: 72, weight: 700 },
                                { text: " km/h", color: [0.6, 0.6, 0.6, 1] } ],
                       position: [40, 600], size: 28, vertical_align: baseline } }
```

`text` / `source` / `spans` のどれか 1 つが必須。`spans` の要素は文字列だけでも可。
全フィールドは `fvsc describe --json` にも出ます。

## wasm / JS

Scene YAML（`fs_create` / `fs_add_layers`）がそのまま使えます。加えて:

| 関数 | 説明 |
|---|---|
| `fs_layer_set_text(inst, id, utf8)` | 文字列だけ差し替え（従来通り） |
| `fs_layer_set_text_style(inst, id, map)` | YAML のフロー写像で書いたフィールドだけ変更: `'{ size: 32, color: [1, 0.5, 0, 1], weight: 700 }'` |
| `fs_measure_text(inst, map) → JSON` | レイヤー無しで計測: `'{ text: "88.5", size: 64, tabular: true }'` |
| `fs_layer_text_metrics(inst, id) → JSON` | 既存レイヤーの計測 |
| `fs_register_font(family, weight, italic, ptr, len)` | フェッチしたフォントを登録（`_malloc` した領域に置いて渡す。コピーされる） |
| `fs_font_families() → string` | 既知のファミリ（改行区切り） |

JSON: `{ box: [x, y, w, h], ink: [...], baseline, ascent, descent, line_height,
size, lines, truncated, line_widths: [...] }`。

```js
const measure = mod.cwrap('fs_measure_text', 'string', ['number', 'string']);
const m = JSON.parse(measure(inst, '{ text: "123.4", size: 96, weight: 700, tabular: true }'));
// 右寄せの位置を自分で決める等に使う: m.box[2] が幅

const buf = new Uint8Array(await (await fetch('/fonts/MyFont.otf')).arrayBuffer());
const p = mod._malloc(buf.length); mod.HEAPU8.set(buf, p);
mod.cwrap('fs_register_font', 'number', ['string', 'number', 'number', 'number', 'number'])
   ('myfont', 400, 0, p, buf.length);
mod._free(p);
```

wasm には Noto Sans JP の Regular と Bold のサブセット（かな・JIS 第 1 水準 +
常用/人名用漢字・記号）が入っています（`wasm/build.sh --no-bold` で Bold を外せる）。

## 注意

- 縁取りの幅は最大約 0.17 × size（それ以上は切り詰め）。太い縁取りは
  レイヤーを重ねる（同じ文字を太字＋大きめの縁取りで下に置く）か影で。
- グローはレイヤーの影そのもの（1 レイヤーに 1 つ）。影とグローを同時に
  使うときはグループで入れ子にする。
- `fs_layer_set_text_style` はキー名の出現で「指定あり」を判定します
  （文字列の中に `size:` のような並びを書くと誤判定しうる）。
