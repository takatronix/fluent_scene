// テキスト API の headless 検証 (node): fs_measure_text / fs_layer_text_metrics /
// fs_layer_set_text_style / fs_register_font / fs_font_families、Scene の
// 新しい text フィールド (weight, outline, spans, max_width, ellipsis, ...)。
// 実行:  node wasm/tests/text_abi.test.mjs   (emsdk の node で可)
import fs from 'node:fs';
import createFluentScene from '../dist/fluent_scene.mjs';

const mod = await createFluentScene();
const cw = (n, r, a) => mod.cwrap(n, r, a);
const api = {
  create: cw('fs_create', 'number', ['string']),
  error: cw('fs_error', 'string', []),
  measure: cw('fs_measure_text', 'string', ['number', 'string']),
  metrics: cw('fs_layer_text_metrics', 'string', ['number', 'string']),
  style: cw('fs_layer_set_text_style', 'number', ['number', 'string', 'string']),
  setText: cw('fs_layer_set_text', 'number', ['number', 'string', 'string']),
  families: cw('fs_font_families', 'string', []),
  register: cw('fs_register_font', 'number', ['string', 'number', 'number', 'number', 'number']),
  render: cw('fs_render', 'number', ['number', 'number', 'number', 'number']),
  destroy: cw('fs_destroy', null, ['number']),
};
let failures = 0;
const check = (ok, what) => {
  if (!ok) { failures++; console.error('FAIL', what); } else console.log('ok', what);
};

const inst = api.create(`schema: fluent.scene/v1alpha2
stage: { size: [400, 200] }
layers:
  - id: speed
    content: { text: { text: "88.5", position: [380, 100], size: 64, weight: 700,
                       align: right, vertical_align: middle, tabular: true,
                       outline_width: 3, outline_color: [0, 0, 0, 1] } }
  - id: rich
    content: { text: { spans: [ { text: "速度 " }, { text: "120", size: 40, color: [0.4, 0.9, 1, 1] } ],
                       position: [10, 10], size: 20 } }
`);
check(inst !== 0, 'scene with new text fields compiles: ' + api.error());

const m = JSON.parse(api.metrics(inst, 'speed'));
check(Math.abs(m.box[0] + m.box[2] - 380) < 0.01, 'align right: box ends at x=380');
check(Math.abs(m.box[1] + m.box[3] / 2 - 100) < 0.01, 'vertical_align middle: box centered at y=100');
check(m.lines === 1 && m.size === 64, 'one line at size 64');

// tabular: every 4-digit number has the same width
const w = (t) => JSON.parse(api.measure(inst, `{ text: "${t}", size: 50, tabular: true }`)).box[2];
check(Math.abs(w('1111') - w('8888')) < 0.01, 'tabular digits: equal widths');

// wrapping + kinsoku: "。" never starts a line
const wrap = JSON.parse(api.measure(inst, '{ text: "あいうえおかきくけこ。さしすせそ", size: 20, max_width: 100 }'));
check(wrap.lines >= 2 && wrap.line_widths.every((x) => x <= 100.01), 'wraps at max_width');
const ell = JSON.parse(api.measure(inst, '{ text: "とても長いタイトルです", size: 20, max_width: 80, wrap: false, ellipsis: true }'));
check(ell.truncated && ell.lines === 1 && ell.line_widths[0] <= 80.01, 'ellipsis truncates to width');
const fit = JSON.parse(api.measure(inst, '{ text: "auto fit text here", size: 40, max_width: 120, wrap: false, min_size: 8 }'));
check(fit.size < 40 && fit.size >= 8 && fit.line_widths[0] <= 120.5, 'auto-fit shrinks: size ' + fit.size);

// cheap live updates: set text / restyle keep the other fields
check(api.setText(inst, 'speed', '123.4') === 1, 'set text');
check(api.style(inst, 'speed', '{ size: 32, color: [1, 0.5, 0, 1] }') === 1, 'restyle: ' + api.error());
const m2 = JSON.parse(api.metrics(inst, 'speed'));
check(m2.size === 32 && Math.abs(m2.box[0] + m2.box[2] - 380) < 0.01, 'restyle keeps align, changes size');
check(api.style(inst, 'nope', '{ size: 10 }') === 0, 'unknown id → 0');
check(api.style(inst, 'speed', '{ size: "big" }') === 0 && api.error().length > 0, 'bad map → 0 + error');

check(api.families().split('\n').includes('sans'), 'families include sans');
// register the embedded Bold under a new family name
const bold = fs.readFileSync(new URL('../fonts/NotoSansJP-Bold-subset.otf', import.meta.url));
const p = mod._malloc(bold.length);
mod.HEAPU8.set(bold, p);
check(api.register('heading', 700, 0, p, bold.length) === 1, 'register font from bytes');
mod._free(p);
check(api.families().split('\n').includes('heading'), 'registered family listed');
const hb = JSON.parse(api.measure(inst, '{ text: "見出し", size: 30, font: heading, weight: 700 }'));
check(hb.box[2] > 80, 'registered family measures');

const px = api.render(inst, 0, 400, 200);
let lit = 0;
for (let i = 0; i < 400 * 200; i++) if (mod.HEAPU8[px + i * 4 + 3] > 0) lit++;
check(lit > 2000, 'renders text pixels: ' + lit);

api.destroy(inst);
console.log(failures === 0 ? 'PASS' : `FAIL (${failures})`);
process.exit(failures === 0 ? 0 : 1);
