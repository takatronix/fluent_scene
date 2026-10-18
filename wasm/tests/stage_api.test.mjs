// Kepler stage 向け ABI の headless 検証 (node)。
// fs_add_layers / fs_remove_layer / fs_layer_ids / fs_has_layer、
// fs_set_param_color / fs_set_param_rect、scale の $params、
// fs_set_text / fs_layer_set_text、fs_diagnostics。
// boxes_abi と同じく CPU レンダラの画素まで見る。
// 実行:  node wasm/tests/stage_api.test.mjs   (emsdk の node で可)
import createFluentScene from '../dist/fluent_scene.mjs';

const mod = await createFluentScene();
const cw = (n, r, a) => mod.cwrap(n, r, a);
const api = {
  create: cw('fs_create', 'number', ['string']),
  error: cw('fs_error', 'string', []),
  digest: cw('fs_digest', 'string', ['number']),
  addLayers: cw('fs_add_layers', 'number', ['number', 'string', 'string', 'number']),
  removeLayer: cw('fs_remove_layer', 'number', ['number', 'string', 'number']),
  layerIds: cw('fs_layer_ids', 'string', ['number']),
  hasLayer: cw('fs_has_layer', 'number', ['number', 'string']),
  setColor: cw('fs_set_param_color', 'number',
               ['number', 'string', 'number', 'number', 'number', 'number']),
  setRect: cw('fs_set_param_rect', 'number',
              ['number', 'string', 'number', 'number', 'number', 'number']),
  setF32: cw('fs_set_param_f32', 'number', ['number', 'string', 'number']),
  setText: cw('fs_set_text', 'number', ['number', 'string', 'string']),
  layerSetText: cw('fs_layer_set_text', 'number', ['number', 'string', 'string']),
  diagnostics: cw('fs_diagnostics', 'string', ['number']),
  render: cw('fs_render', 'number', ['number', 'number', 'number', 'number']),
  stride: cw('fs_render_stride', 'number', ['number']),
  destroy: cw('fs_destroy', null, ['number']),
};

let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`);
  if (!ok) failures++;
};

const scene = `schema: fluent.scene/v1alpha2
stage: { size: [400, 200] }
inputs:
  label: { type: text.utf8, update: on_change, fallback: hide }
params:
  tint: { type: color, default: [0, 0, 1, 1] }
  zoom: { type: f32, default: 1 }
  place: { type: rect, default: [200, 0, 200, 200] }
layers:
  - id: figure
    frame: [0, 0, 100, 100]
    scale: $params.zoom
    background: $params.tint
  - id: caption
    content: { text: { source: $inputs.label, position: [10, 160], size: 24 } }
  - id: title
    content: { text: { text: "before", position: [10, 120], size: 24 } }
  - id: media
    frame: $params.place
    transition: { duration: 1, ease: linear, enter: { opacity: 0 } }
`;
const inst = api.create(scene);
if (!inst) { console.error('fs_create failed:', api.error()); process.exit(1); }

let ptr = 0, stride = 0;
const frame = (dt) => { ptr = api.render(inst, dt, 400, 200); stride = api.stride(inst); };
const px = (x, y) => {
  const o = ptr + y * stride + x * 4;
  return [mod.HEAPU8[o], mod.HEAPU8[o + 1], mod.HEAPU8[o + 2], mod.HEAPU8[o + 3]];
};
const inkIn = (x0, y0, x1, y1) => {   // any bright pixel in the box (text ink)
  for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) if (px(x, y)[0] > 128) return true;
  return false;
};

// ---- color / rect / scale params ----
frame(0);
check(px(50, 50)[2] > 200 && px(50, 50)[0] < 30, 'figure starts blue (color param default)');
check(api.setColor(inst, 'tint', 1, 0, 0, 1) === 1, 'fs_set_param_color → 1');
check(api.setF32(inst, 'zoom', 0.5) === 1, 'scale bound to an f32 param');
frame(0);
check(px(50, 50)[0] > 200 && px(50, 50)[2] < 30, 'figure is red now');
check(px(10, 10)[3] === 0, 'zoom 0.5 shrank the figure about its center');
check(api.setColor(inst, 'zoom', 1, 1, 1, 1) === 0, 'color setter on an f32 param → 0');
check(api.diagnostics(inst).includes("param has type f32"), 'fs_diagnostics says why');

// ---- text ----
check(!inkIn(10, 160, 120, 190), 'caption hidden before its input is fed');
check(api.setText(inst, 'label', 'こんにちは') === 1, 'fs_set_text → 1');
frame(0);
check(inkIn(10, 160, 140, 190), 'caption drawn after fs_set_text');
check(api.setText(inst, 'nope', 'x') === 0, 'fs_set_text on an undeclared input → 0');
check(api.layerSetText(inst, 'title', 'after') === 1, 'fs_layer_set_text → 1');
check(api.layerSetText(inst, 'figure', 'x') === 0, 'fs_layer_set_text on a non-text layer → 0');

// ---- add / remove ----
const digest0 = api.digest(inst);
check(api.layerIds(inst) === 'figure\ncaption\ntitle\nmedia\n', 'fs_layer_ids lists declared ids');
const card = `params:
  card_tint: { type: color, default: [0, 1, 0, 1] }
layers:
  - id: card
    frame: [20, 20, 160, 160]
    background: $params.card_tint
`;
check(api.addLayers(inst, 'media', card, 1) === 1, 'fs_add_layers(media, animated) → 1');
check(api.hasLayer(inst, 'card') === 1, 'fs_has_layer(card)');
check(api.layerIds(inst) === 'figure\ncaption\ntitle\nmedia\ncard\n', 'card listed under media');
check(api.digest(inst) !== digest0, 'digest follows the edit');
frame(0);
check(px(300, 100)[3] < 10, 'card enters from opacity 0 (media transition)');
frame(0.5);
const mid = px(300, 100);   // straight alpha: the fade shows in α
check(mid[3] > 100 && mid[3] < 160 && mid[1] > 240, `card half faded in (α=${mid[3]})`);
frame(0.6);
check(px(300, 100)[3] > 250, 'card fully in');
check(api.setColor(inst, 'card_tint', 1, 1, 0, 1) === 1, 'fragment param is live');
check(api.setRect(inst, 'place', 0, 0, 200, 200) === 1, 'fs_set_param_rect moves media');
frame(0.5);                 // media's transition animates its frame too
check(px(200, 100)[3] > 250 && px(30, 100)[3] < 10, 'media half way (x 100→)');
frame(0.6);
check(px(100, 100)[0] > 240 && px(100, 100)[1] > 240, 'media + card moved left, card yellow');

check(api.addLayers(inst, 'media', 'layers: [ { id: card } ]', 0) === 0, 'duplicate id rejected');
check(api.error().includes('validate.duplicate_id'), 'fs_error carries the diagnostic');
check(api.addLayers(inst, 'nope', 'layers: [ {} ]', 0) === 0, 'unknown parent rejected');
check(api.addLayers(inst, '', 'layers: [ { opacity: $params.zzz } ]', 0) === 0,
      'undeclared param rejected');

check(api.removeLayer(inst, 'card', 1) === 1, 'fs_remove_layer(card, animated) → 1');
check(api.hasLayer(inst, 'card') === 0, 'card no longer declared');
frame(0.5);
check(px(100, 100)[3] > 60 && px(100, 100)[3] < 200, 'card fading out, still drawn');
check(api.addLayers(inst, 'media', card, 0) === 1, 'same fragment adds again while the old one leaves');
check(api.removeLayer(inst, 'card', 0) === 1, 'plain removal');
frame(0.6);
check(px(100, 100)[3] === 0, 'all gone');
check(api.removeLayer(inst, 'card', 0) === 0, 'removing twice → 0');
check(api.layerIds(inst) === 'figure\ncaption\ntitle\nmedia\n', 'back to the host ids');

api.destroy(inst);
console.log(failures === 0 ? 'PASS' : `FAIL (${failures})`);
process.exit(failures === 0 ? 0 : 1);
