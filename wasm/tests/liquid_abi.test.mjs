// liquid (Kepler の Liquid) の ABI を headless で検証 (node, CPU レンダラ)。
// $params に束縛した filter パラメータ (mood / level / pointer / splash)、
// Clock スロット (time は dt だけで進む)、fs_layer_set_filter_param、
// fs_filters_json の unit "clock"。
// 実行:  node wasm/tests/liquid_abi.test.mjs   (emsdk の node で可)
import createFluentScene from '../dist/fluent_scene.mjs';

const mod = await createFluentScene();
const cw = (n, r, a) => mod.cwrap(n, r, a);
const api = {
  create: cw('fs_create', 'number', ['string']),
  error: cw('fs_error', 'string', []),
  setF32: cw('fs_set_param_f32', 'number', ['number', 'string', 'number']),
  setFilterParam: cw('fs_layer_set_filter_param', 'number',
                     ['number', 'string', 'string', 'string', 'number', 'number']),
  filtersJson: cw('fs_filters_json', 'string', []),
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

const W = 160, H = 100;
const scene = `schema: fluent.scene/v1alpha2
stage: { size: [${W}, ${H}] }
params:
  listen: { type: f32, default: 0, animate: { duration: 1, ease: linear } }
  think: { type: f32, default: 0, animate: { duration: 1, ease: linear } }
  speak: { type: f32, default: 0, animate: { duration: 1, ease: linear } }
  level: { type: f32, default: 0 }
  px: { type: f32, default: 80 }
  py: { type: f32, default: 50 }
  pointer: { type: f32, default: 0 }
  splash: { type: f32, default: 9 }
layers:
  - id: liquid
    frame: [0, 0, ${W}, ${H}]
    background: [0, 0, 0, 1]
    filters:
      - liquid: { listen: $params.listen, think: $params.think, speak: $params.speak,
                  level: $params.level, pointer_x: $params.px, pointer_y: $params.py,
                  pointer: $params.pointer, splash: $params.splash, quality: 0.3 }
`;
const inst = api.create(scene);
if (!inst) { console.error('fs_create failed:', api.error()); process.exit(1); }

let ptr = 0, stride = 0;
const frame = (dt) => { ptr = api.render(inst, dt, W, H); stride = api.stride(inst); };
const px = (x, y) => {
  const o = ptr + y * stride + x * 4;
  return [mod.HEAPU8[o], mod.HEAPU8[o + 1], mod.HEAPU8[o + 2], mod.HEAPU8[o + 3]];
};
const snap = () => mod.HEAPU8.slice(ptr, ptr + stride * H);
const meanDiff = (a, b) => { let s = 0; for (let i = 0; i < a.length; i++) s += Math.abs(a[i] - b[i]); return s / a.length; };
const meanRgb = () => {   // average straight color over the opaque pixels
  const s = [0, 0, 0]; let n = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const p = px(x, y); if (p[3] > 128) { s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n++; }
  }
  return s.map((v) => v / Math.max(n, 1));
};

// ---- a figure on a transparent ground ----
frame(0);
check(px(80, 50)[3] > 200, 'the body is opaque at the center');
check(px(1, 1)[3] < 8, 'the ground stays transparent');
const idle = meanRgb();
check(idle[2] > idle[0], 'idle is blue');

// ---- time is a Clock: dt alone moves it, dt 0 does not ----
const a0 = snap();
frame(0);
check(meanDiff(a0, snap()) < 0.01, 'dt 0 → the same frame (deterministic)');
frame(0.6);
check(meanDiff(a0, snap()) > 0.5, 'dt 0.6 → the liquid moved by itself');

// ---- mood through $params + animate ----
check(api.setF32(inst, 'speak', 1) === 1, 'speak bound to a param');
check(api.setF32(inst, 'level', 0.6) === 1, 'level bound to a param');
frame(0.5);
const half = meanRgb();
frame(0.6);
const speak = meanRgb();
check(speak[0] > speak[2] && speak[0] > idle[0] + 40, 'speaking turns warm');
check(half[0] > idle[0] && half[0] < speak[0], 'the mood eased through animate (half-way warmer)');

// ---- splash: a tap resets the Clock to 0 ----
const before = snap();
check(api.setF32(inst, 'splash', 0) === 1, 'splash bound to a param');
frame(0.25);
check(meanDiff(before, snap()) > 1.0, 'the splash changes the picture');

// ---- pointer: the strand reaches for it ----
check(api.setF32(inst, 'px', 140) === 1 && api.setF32(inst, 'py', 50) === 1, 'pointer xy');
check(api.setF32(inst, 'pointer', 1) === 1, 'pointer presence');
frame(2.0);   // let the splash settle
const withPtr = snap();
api.setF32(inst, 'pointer', 0);
frame(0);
check(meanDiff(withPtr, snap()) > 0.3, 'the pointer leans / reaches the body');

// ---- fs_layer_set_filter_param (any slot by name) ----
check(api.setFilterParam(inst, 'liquid', 'liquid', 'hue', 120, 0) === 1, 'set_filter_param hue');
check(api.setFilterParam(inst, 'liquid', 'liquid', 'nope', 1, 0) === 0, 'unknown param → 0');
check(api.setFilterParam(inst, 'liquid', 'sepia', 'intensity', 1, 0) === 0, 'filter not on the layer → 0');
check(api.setFilterParam(inst, 'nobody', 'liquid', 'hue', 1, 0) === 0, 'unknown layer → 0');

// ---- catalog ----
const cat = JSON.parse(api.filtersJson());
const liq = cat.find((f) => f.name === 'liquid');
check(!!liq && liq.params.length === 13, 'catalog lists liquid with 13 params');
check(liq && liq.params[0].unit === 'clock' && liq.params[8].unit === 'clock', 'time / splash are clock');

api.destroy(inst);
console.log(failures === 0 ? 'PASS' : `FAIL (${failures})`);
process.exit(failures === 0 ? 0 : 1);
