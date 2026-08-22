// fs_set_boxes / fs_set_points の headless 検証 (node)。
// CPU レンダラで画素まで確認する — 「ABI が通った」ではなく「箱が描けた」を見る。
// 実行:  node wasm/tests/boxes_abi.test.mjs   (emsdk の node で可)
import createFluentScene from '../dist/fluent_scene.mjs';

const mod = await createFluentScene();
const api = {
  create: mod.cwrap('fs_create', 'number', ['number']),
  error: mod.cwrap('fs_error', 'string', []),
  setBoxes: mod.cwrap('fs_set_boxes', 'number',
                      ['number', 'string', 'number', 'number', 'string']),
  floatBuffer: mod.cwrap('fs_float_buffer', 'number',
                         ['number', 'string', 'number']),
  setPoints: mod.cwrap('fs_set_points', 'number',
                       ['number', 'string', 'number', 'number']),
  render: mod.cwrap('fs_render', 'number', ['number', 'number', 'number', 'number']),
  renderW: mod.cwrap('fs_render_width', 'number', ['number']),
  stride: mod.cwrap('fs_render_stride', 'number', ['number']),
  destroy: mod.cwrap('fs_destroy', null, ['number']),
};

const scene = `schema: fluent.scene/v1alpha2
stage: { size: [1280, 720] }
inputs:
  dets:    { type: "sequence<Detection2D, 128>", update: per_frame }
  corners: { type: "sequence<vec2, 256>", update: per_frame }
layers:
  - content: { boxes: { source: $inputs.dets,
                        color: [0.1, 0.9, 0.4, 1], smoothing: 0 } }
  - content: { polyline: { source: $inputs.corners,
                           color: [1, 0.86, 0.24, 1], thickness: 2 } }
`;
const sp = mod.stringToNewUTF8(scene);
const inst = api.create(sp);
if (!inst) { console.error('fs_create failed:', api.error()); process.exit(1); }

// 2 boxes with labels
const boxes = new Float32Array([
  100, 100, 200, 300, 0.91, 7,
  700, 200, 150, 400, 0.66, 8,
]);
const bp = api.floatBuffer(inst, 'dets', boxes.length);
mod.HEAPF32.set(boxes, bp / 4);
const ok1 = api.setBoxes(inst, 'dets', bp, 2, 'asparagus\ntag');

const pts = new Float32Array([500, 500, 600, 500, 600, 600, 500, 600, 500, 500]);
const pp = api.floatBuffer(inst, 'corners', pts.length);
mod.HEAPF32.set(pts, pp / 4);
const ok2 = api.setPoints(inst, 'corners', pp, 5);

console.log('setBoxes:', ok1, ' setPoints:', ok2);
if (!ok1 || !ok2) process.exit(1);

const ptr = api.render(inst, 0.016, 1280, 720);
const stride = api.stride(inst);
const px = (x, y) => {
  const o = ptr + y * stride + x * 4;
  return [mod.HEAPU8[o], mod.HEAPU8[o + 1], mod.HEAPU8[o + 2], mod.HEAPU8[o + 3]];
};
// 箱の縁 (x=100 上辺) に緑、ポリラインの縁 (500,500) に黄が乗っているはず
const edge = px(150, 100);
const poly = px(550, 500);
const bg = px(50, 650);
console.log('box edge   @(150,100):', edge);
console.log('polyline   @(550,500):', poly);
console.log('background @(50,650):', bg);
const green = edge[1] > 150 && edge[1] > edge[0];
const yellow = poly[0] > 150 && poly[1] > 120 && poly[2] < 120;
const empty = bg[0] < 40 && bg[1] < 40 && bg[2] < 40;
console.log(green && yellow && empty ? 'PASS' : 'FAIL');
process.exit(green && yellow && empty ? 0 : 1);
