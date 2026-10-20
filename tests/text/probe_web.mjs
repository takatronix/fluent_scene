// Renders tests/text/probe_*.fvs with a wasm build in headless Chromium
// (WebGPU readback + wasm CPU) and writes PNGs:
//   NODE_PATH=<dir with playwright-core> node tests/text/probe_web.mjs \
//     <dist-dir relative to repo> <out-dir> <tag> [chrome-path]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { chromium } = require('playwright-core');

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const [dist = 'wasm/dist', outDir = '/tmp/fstext', tag = 'web', chrome] = process.argv.slice(2);
const types = { '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html' };
const server = http.createServer((req, res) => {
  const p = path.join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!p.startsWith(root) || !fs.existsSync(p) || fs.statSync(p).isDirectory()) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'content-type': types[path.extname(p)] || 'application/octet-stream' });
  fs.createReadStream(p).pipe(res);
}).listen(0, '127.0.0.1');
await new Promise((r) => server.once('listening', r));
const base = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch({
  executablePath: chrome || process.env.CHROME || `${process.env.HOME}/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome`,
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--ignore-gpu-blocklist'],
});
fs.mkdirSync(outDir, { recursive: true });
for (const scene of ['probe_dark', 'probe_light', 'probe_xform']) {
  const page = await browser.newPage();
  page.on('console', (m) => { if (m.type() === 'error') console.error('page:', m.text()); });
  await page.goto(`${base}/tests/text/probe.html?dist=/${dist}&scene=/tests/text/${scene}.fvs`);
  const r = await page.evaluate(() => window.probe());
  if (r.error) throw new Error(r.error);
  for (const k of ['cpu', 'gpu']) {
    if (!r[k]) continue;
    const f = path.join(outDir, `${tag}-${scene}-wasm${k === 'gpu' ? 'webgpu' : 'cpu'}.png`);
    fs.writeFileSync(f, Buffer.from(r[k].split(',')[1], 'base64'));
  }
  console.log(scene, `${r.w}x${r.h}`, 'wasm-cpu', r.cpu_ms.toFixed(0) + 'ms', r.gpu ? 'webgpu ok' : 'webgpu: ' + r.gpu_error);
  await page.close();
}
await browser.close();
server.close();
