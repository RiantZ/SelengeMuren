// The client: WebGL2 rendering, input and HUD. It asks server_sim.js for a window of data and only draws it.
// DEFS comes from signals.js, loaded before this file.
(() => {
'use strict';
const $ = (id) => document.getElementById(id);
const glc = $('gl'), stc = $('txt-static'), dyc = $('txt-dyn');
const hud = $('hud'), errBox = $('err'), countersBox = $('counters'), btnLive = $('btn-live');
const sctx = stc.getContext('2d'), dctx = dyc.getContext('2d');

function fail(msg) { errBox.textContent = msg; throw new Error(msg); }

const gl = glc.getContext('webgl2', { antialias: false, alpha: false, depth: false, stencil: false, powerPreference: 'high-performance' });
if (!gl) fail('WebGL2 недоступен в этом браузере.');

const worker = new Worker('server_sim.js');
worker.onerror = (e) => { errBox.textContent = 'Ошибка воркера: ' + e.message; };

// ---------------------------------------------------------------- time
const HISTORY_MS = 3600e3;                               // the synthetic recording started an hour ago
const EPOCH_MS = performance.timeOrigin + performance.now() - HISTORY_MS;
const REC_START_WALL_MS = Date.now() - HISTORY_MS;
const clockNs = () => (performance.timeOrigin + performance.now() - EPOCH_MS) * 1e6;
const INIT_WINDOW = 2.73e9, MIN_WINDOW = 20e3, LIVE_DELAY = 100e6, LIVE_BATCH = 50e6;

// ---------------------------------------------------------------- state
const st = { n: 9, mult: 1, rtt: 0, marks: 'auto', smooth: true };
const view = { left: 0, window: INIT_WINDOW };
const zoom = { target: INIT_WINDOW, fx: 0.5 };
let live = false;
const cursor = { x: 0, in: false };
let drag = null;

// ---------------------------------------------------------------- GL helpers
function compile(type, src) {
  const sh = gl.createShader(type);
  gl.shaderSource(sh, src);
  gl.compileShader(sh);
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(sh));
  return sh;
}
function link(vs, fs) {
  const p = gl.createProgram();
  gl.attachShader(p, compile(gl.VERTEX_SHADER, vs));
  gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  return p;
}
function uniforms(p, names) {
  const u = {};
  for (const n of names) u[n] = gl.getUniformLocation(p, n);
  return u;
}

const MAP_GLSL = `
uniform vec2 u_canvas;
uniform vec4 u_panel;   // x, y, w, h in device px, y down
uniform vec2 u_x;       // px from panel left = x * u_x.x + u_x.y
uniform vec2 u_v;       // value range of the panel
uniform float u_pad;
vec2 to_px(vec2 q) {
  float x = u_panel.x + q.x * u_x.x + u_x.y;
  float y = u_panel.y + u_pad + (u_v.y - q.y) / (u_v.y - u_v.x) * (u_panel.w - 2.0 * u_pad);
  return vec2(x, y);
}
vec4 to_clip(vec2 p) { return vec4(p.x / u_canvas.x * 2.0 - 1.0, 1.0 - p.y / u_canvas.y * 2.0, 0.0, 1.0); }
`;

// one instance = one segment between neighbour points, drawn as a quad
const VS_LINE = `#version 300 es
precision highp float;
layout(location = 0) in vec2 a_p0;   // x: ns from the block start, y: value
layout(location = 1) in vec2 a_p1;
uniform float u_half;
${MAP_GLSL}
void main() {
  int vid = gl_VertexID;
  vec2 s0 = to_px(a_p0), s1 = to_px(a_p1);
  vec2 d = s1 - s0;
  float len = length(d);
  vec2 dir = len > 1e-4 ? d / len : vec2(1.0, 0.0);
  vec2 nrm = vec2(-dir.y, dir.x);
  float along = (vid == 1 || vid == 4 || vid == 5) ? 1.0 : 0.0;
  float side = (vid == 2 || vid == 3 || vid == 5) ? 1.0 : -1.0;
  vec2 p = mix(s0 - dir * u_half, s1 + dir * u_half, along) + nrm * side * u_half;
  gl_Position = to_clip(p);
}`;
const FS_COLOR = `#version 300 es
precision mediump float;
uniform vec4 u_color;
out vec4 o_color;
void main() { o_color = u_color; }`;

// one instance = one hollow square marker on a raw point, snapped to the pixel grid
const VS_MARK = `#version 300 es
precision highp float;
layout(location = 0) in vec2 a_p;
uniform float u_size;
out vec2 v_local;
${MAP_GLSL}
void main() {
  int vid = gl_VertexID;
  vec2 c = vec2((vid == 1 || vid == 4 || vid == 5) ? 1.0 : -1.0, (vid == 2 || vid == 3 || vid == 5) ? 1.0 : -1.0);
  vec2 s = floor(to_px(a_p)) + 0.5;
  v_local = c;
  gl_Position = to_clip(s + c * u_size);
}`;
const FS_MARK = `#version 300 es
precision mediump float;
uniform vec4 u_color;
uniform float u_inner;
in vec2 v_local;
out vec4 o_color;
void main() { o_color = max(abs(v_local.x), abs(v_local.y)) > u_inner ? u_color : vec4(1.0); }`;

// one instance = one rectangle: panel backgrounds, separators, cursor
const VS_RECT = `#version 300 es
precision highp float;
layout(location = 0) in vec4 a_rect;
layout(location = 1) in vec4 a_color;
uniform vec2 u_canvas;
out vec4 v_color;
void main() {
  int vid = gl_VertexID;
  vec2 c = vec2((vid == 1 || vid == 4 || vid == 5) ? 1.0 : 0.0, (vid == 2 || vid == 3 || vid == 5) ? 1.0 : 0.0);
  vec2 p = a_rect.xy + c * a_rect.zw;
  gl_Position = vec4(p.x / u_canvas.x * 2.0 - 1.0, 1.0 - p.y / u_canvas.y * 2.0, 0.0, 1.0);
  v_color = a_color;
}`;
const FS_RECT = `#version 300 es
precision mediump float;
in vec4 v_color;
out vec4 o_color;
void main() { o_color = v_color; }`;

let pLine, pMark, pRect;
try {
  pLine = link(VS_LINE, FS_COLOR);
  pMark = link(VS_MARK, FS_MARK);
  pRect = link(VS_RECT, FS_RECT);
} catch (e) { fail('Ошибка шейдера:\n' + e.message); }
const MAP_U = ['u_canvas', 'u_panel', 'u_x', 'u_v', 'u_pad', 'u_color'];
const UL = uniforms(pLine, [...MAP_U, 'u_half']);
const UM = uniforms(pMark, [...MAP_U, 'u_size', 'u_inner']);
const UR = uniforms(pRect, ['u_canvas']);

const vaoLine = gl.createVertexArray();
gl.bindVertexArray(vaoLine);
gl.enableVertexAttribArray(0); gl.vertexAttribDivisor(0, 1);
gl.enableVertexAttribArray(1); gl.vertexAttribDivisor(1, 1);
const vaoMark = gl.createVertexArray();
gl.bindVertexArray(vaoMark);
gl.enableVertexAttribArray(0); gl.vertexAttribDivisor(0, 1);
gl.bindVertexArray(null);

const RECT_CAP = 1024;
const rects = { vao: gl.createVertexArray(), buf: gl.createBuffer(), data: new Float32Array(RECT_CAP * 8), n: 0 };
gl.bindVertexArray(rects.vao);
gl.bindBuffer(gl.ARRAY_BUFFER, rects.buf);
gl.bufferData(gl.ARRAY_BUFFER, rects.data.byteLength, gl.DYNAMIC_DRAW);
gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 32, 0); gl.vertexAttribDivisor(0, 1);
gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 32, 16); gl.vertexAttribDivisor(1, 1);
gl.bindVertexArray(null);

function rect(x, y, w, h, r, g, b, a) {
  if (rects.n >= RECT_CAP) return;
  const o = rects.n++ * 8, d = rects.data;
  d[o] = x; d[o + 1] = y; d[o + 2] = w; d[o + 3] = h; d[o + 4] = r; d[o + 5] = g; d[o + 6] = b; d[o + 7] = a;
}
function flushRects() {
  if (!rects.n) return;
  gl.useProgram(pRect);
  gl.uniform2f(UR.u_canvas, glc.width, glc.height);
  gl.bindVertexArray(rects.vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, rects.buf);
  gl.bufferSubData(gl.ARRAY_BUFFER, 0, rects.data, 0, rects.n * 8);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, rects.n);
  gl.disable(gl.BLEND);
  stats.draws++;
  rects.n = 0;
}

// optional GPU timer (often missing in Safari and Firefox)
const timerExt = gl.getExtension('EXT_disjoint_timer_query_webgl2');
const queries = [];
let curQuery = null;
function beginGpu() {
  if (!timerExt || queries.length > 6) return;
  curQuery = gl.createQuery();
  gl.beginQuery(timerExt.TIME_ELAPSED_EXT, curQuery);
}
function endGpu() {
  if (!curQuery) return;
  gl.endQuery(timerExt.TIME_ELAPSED_EXT);
  queries.push(curQuery);
  curQuery = null;
}
function pollGpu() {
  while (queries.length) {
    const q = queries[0];
    if (!gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE)) break;
    const ns = gl.getQueryParameter(q, gl.QUERY_RESULT);
    if (!gl.getParameter(timerExt.GPU_DISJOINT_EXT)) gpuMs.push(ns / 1e6);
    gl.deleteQuery(q);
    queries.shift();
  }
}

// ---------------------------------------------------------------- stats
class Ring {
  constructor(n) { this.a = new Float64Array(n); this.n = 0; this.i = 0; }
  push(v) { this.a[this.i] = v; this.i = (this.i + 1) % this.a.length; if (this.n < this.a.length) this.n++; }
  view() { return this.n === this.a.length ? this.a : this.a.subarray(0, this.n); }
  clear() { this.n = 0; this.i = 0; }
}
const intervals = new Ring(600), jsMs = new Ring(240), gpuMs = new Ring(240), uploadMs = new Ring(240), gaps = new Ring(600);
const stats = { draws: 0, segs: 0, marks: 0, reqs: 0, bytes: 0, lastBlock: null };
const sorted = (r) => Float64Array.from(r.view()).sort();
const pct = (s, p) => (s.length ? s[Math.min(s.length - 1, Math.floor(p * s.length))] : 0);
const avg = (r) => { const v = r.view(); let s = 0; for (let i = 0; i < v.length; i++) s += v[i]; return v.length ? s / v.length : 0; };
const maxOf = (r) => { const v = r.view(); let m = 0; for (let i = 0; i < v.length; i++) if (v[i] > m) m = v[i]; return m; };

// ---------------------------------------------------------------- data blocks from the "server"
let cfgGen = 1, reqSeq = 0, inflight = 0;
const blocks = [];                                 // newest last, at most 3
const queue = [];
let rateT0 = performance.now(), rateReqs = 0, rateBytes = 0, reqPerSec = 0, bytesPerSec = 0;

worker.onmessage = (e) => queue.push(e.data);

function addBlock(m) {
  if (m.id === inflight) inflight = 0;
  if (m.gen !== cfgGen) return;
  const vbo = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(m.buf), gl.STATIC_DRAW);
  blocks.push({ lo: m.lo, hi: m.hi, genNow: m.genNow, nsPerPx: m.nsPerPx, vbo,
                offsets: m.offsets, counts: m.counts, raw: m.raw, bytes: m.buf.byteLength });
  while (blocks.length > 3) gl.deleteBuffer(blocks.shift().vbo);
  stats.lastBlock = { bytes: m.buf.byteLength, genMs: m.genMs, span: m.hi - m.lo, points: m.buf.byteLength / 8 };
  rateBytes += m.buf.byteLength;
}

function dropBlocks() {
  for (const b of blocks) gl.deleteBuffer(b.vbo);
  blocks.length = 0;
  inflight = 0;
  cfgGen++;
}

// the block that shows the most of the visible window, newest wins a tie
function pickBlock(vl, vr, now) {
  const need0 = Math.max(vl, 0), need1 = Math.min(vr, now);
  if (need1 <= need0) return { block: blocks[blocks.length - 1] || null, cov: 1 };
  let best = null, cov = -1;
  for (let i = blocks.length - 1; i >= 0; i--) {
    const b = blocks[i];
    const c = Math.max(0, Math.min(b.hi, need1) - Math.max(b.lo, need0)) / (need1 - need0);
    if (c > cov + 1e-9) { best = b; cov = c; }
  }
  return { block: best, cov };
}

function maybeRequest(now, nsPerPx) {
  if (inflight) return;                            // one request in flight, the latest view wins
  const vl = view.left, vr = view.left + view.window;
  const pk = pickBlock(vl, vr, now);
  let need = !pk.block || pk.cov < 0.999;
  if (!need) {
    const b = pk.block, r = b.nsPerPx / nsPerPx, m = 0.2 * view.window;
    if (r > 1.5 || r < 0.5) need = true;           // zoom changed the resolution
    else if (live) need = now - b.genNow > LIVE_BATCH;
    else need = (vl - b.lo < m && b.lo > 0) || (b.hi - vr < m && now - b.hi > m);
  }
  if (!need) return;
  const span = view.window;
  inflight = ++reqSeq;
  worker.postMessage({
    id: inflight, gen: cfgGen, epochMs: EPOCH_MS, nsPerPx, n: st.n, mult: st.mult, rtt: st.rtt,
    lo: Math.max(0, vl - 0.5 * span), hi: live ? now + 1e9 : Math.min(now, vr + 0.5 * span),
  });
  rateReqs++;
}

// ---------------------------------------------------------------- layout
let dpr = window.devicePixelRatio || 1, cssW = 1, cssH = 1, layoutDirty = true;
const G = {};
new ResizeObserver((en) => { const r = en[0].contentRect; cssW = r.width; cssH = r.height; }).observe($('plot'));

function layout() {
  dpr = window.devicePixelRatio || 1;
  const W = Math.max(1, Math.round(cssW * dpr)), H = Math.max(1, Math.round(cssH * dpr));
  if (glc.width !== W || glc.height !== H) {
    for (const c of [glc, stc, dyc]) { c.width = W; c.height = H; }
    layoutDirty = true;
  }
  G.W = W; G.H = H;
  G.hdr = Math.round(18 * dpr);
  G.sts = Math.round(18 * dpr);
  G.gut = Math.round(48 * dpr);
  G.plotX = G.gut;
  G.plotW = Math.max(1, W - G.gut);
  G.rowsY = G.hdr;
  G.rowsH = Math.max(1, H - G.hdr - G.sts);
  G.rowH = Math.max(1, Math.floor(G.rowsH / st.n));
}

function counterInfo(c) {
  const d = DEFS[c % DEFS.length], seed = Math.floor(c / DEFS.length);
  return { d, seed, name: d.name + (seed ? ' #' + (seed + 1) : '') };
}

// ---------------------------------------------------------------- text
const p2 = (v) => String(v).padStart(2, '0'), p3 = (v) => String(v).padStart(3, '0');
function fmtAbs(ns) {
  const ms = Math.floor(ns / 1e6), sub = ns - ms * 1e6, d = new Date(REC_START_WALL_MS + ms);
  return `${p2(d.getDate())}.${p2(d.getMonth() + 1)}.${d.getFullYear()} ${p2(d.getHours())}:${p2(d.getMinutes())}:` +
         `${p2(d.getSeconds())}.${p3(d.getMilliseconds())}'${p3(Math.floor(sub / 1000))}"${Math.floor((sub % 1000) / 100)}`;
}
function fmtSpan(ns) {
  const ms = Math.floor(ns / 1e6), sub = ns - ms * 1e6;
  return `${p2(Math.floor(ms / 3600000))}:${p2(Math.floor(ms / 60000) % 60)}:${p2(Math.floor(ms / 1000) % 60)}.` +
         `${p3(ms % 1000)}'${p3(Math.floor(sub / 1000))}"${Math.floor((sub % 1000) / 100)}`;
}
function fmtAxis(t, step) {
  if (Math.abs(t) < 0.5) return '0.0';
  if (t < 1e3) return t.toFixed(0) + ' ns';
  if (t < 1e6) return (t / 1e3).toFixed(step < 100 ? 3 : step < 1e3 ? 2 : 1) + ' µs';
  if (t < 1e9) return (t / 1e6).toFixed(step < 1e5 ? 3 : step < 1e6 ? 2 : 1) + ' ms';
  const dec = step >= 1e9 ? 0 : step >= 1e8 ? 1 : step >= 1e7 ? 2 : step >= 1e6 ? 3 : 6;
  if (t < 60e9) return (t / 1e9).toFixed(Math.max(1, dec)) + ' s';
  const sec = t / 1e9, h = Math.floor(sec / 3600), m = Math.floor(sec / 60) % 60;
  let s = (sec % 60).toFixed(dec);
  if (parseFloat(s) < 10) s = '0' + s;
  return (h ? h + ':' + p2(m) : String(m)) + ':' + s;
}
function fmtNs(ns) {
  if (ns >= 1e9) return (ns / 1e9).toFixed(3) + ' с';
  if (ns >= 1e6) return (ns / 1e6).toFixed(3) + ' мс';
  if (ns >= 1e3) return (ns / 1e3).toFixed(3) + ' мкс';
  return ns.toFixed(1) + ' нс';
}
const fmtInt = (v) => Math.round(v).toLocaleString('ru-RU');
const fmtBytes = (b) => (b >= 1e6 ? (b / 1e6).toFixed(2) + ' МБ' : (b / 1e3).toFixed(1) + ' КБ');
function fmtRate(hz) {
  if (hz >= 1e6) return (hz / 1e6).toFixed(1) + ' МГц';
  if (hz >= 1e3) return (hz / 1e3).toFixed(1) + ' кГц';
  return hz.toFixed(1) + ' Гц';
}
const fmtY = (v) => (v >= 1000 ? (v / 1000).toFixed(1) + 'k' : v.toFixed(1));
const css = (rgb) => `rgb(${rgb.map((x) => Math.round(x * 255)).join(',')})`;

const STEPS = [];
for (let e = 0; e <= 11; e++) for (const m of [1, 2, 5]) STEPS.push(m * 10 ** e);
STEPS.push(600e9, 1200e9, 1800e9, 3600e9);
function pickStep(nsPerCssPx) {
  const want = 72 * nsPerCssPx;
  for (const s of STEPS) if (s >= want) return s;
  return 7200e9;
}

function drawStaticText() {
  sctx.clearRect(0, 0, stc.width, stc.height);
  const big = G.rowH >= 22 * dpr, small = G.rowH >= 12 * dpr;
  sctx.textBaseline = 'middle';
  for (let c = 0; c < st.n; c++) {
    const { d, name } = counterInfo(c);
    const y = G.rowsY + c * G.rowH;
    if (big) {
      sctx.font = `${Math.round(10.5 * dpr)}px system-ui, -apple-system, "Segoe UI", Ubuntu, Arial, sans-serif`;
      sctx.fillStyle = '#333';
      sctx.textAlign = 'right';
      const x = G.gut - 4 * dpr, pad = 2 * dpr;
      sctx.fillText(fmtY(d.hi), x, y + pad + 5 * dpr);
      sctx.fillText(fmtY((d.lo + d.hi) / 2), x, y + G.rowH / 2);
      sctx.fillText(fmtY(d.lo), x, y + G.rowH - pad - 5 * dpr);
    }
    if (small) {
      sctx.textAlign = 'left';
      sctx.font = `${Math.round(11.5 * dpr)}px system-ui, -apple-system, "Segoe UI", Ubuntu, Arial, sans-serif`;
      const cy = y + Math.min(9 * dpr, G.rowH / 2);
      sctx.fillStyle = css(d.rgb);
      sctx.beginPath(); sctx.arc(G.gut + 10 * dpr, cy, 4 * dpr, 0, TAU_JS); sctx.fill();
      sctx.fillStyle = '#111';
      sctx.fillText(name, G.gut + 18 * dpr, cy);
    }
  }
}
const TAU_JS = Math.PI * 2;

const dyn = { left: NaN, window: NaN, W: 0 };
function drawDynamicText() {
  if (dyn.left === view.left && dyn.window === view.window && dyn.W === G.W && !layoutDirty) return;
  dyn.left = view.left; dyn.window = view.window; dyn.W = G.W;
  const W = G.W, H = G.H;
  dctx.clearRect(0, 0, W, G.hdr);
  dctx.clearRect(0, H - G.sts, W, G.sts);
  const nsPerPx = view.window / G.plotW, step = pickStep(nsPerPx * dpr);
  dctx.font = `${Math.round(11 * dpr)}px system-ui, -apple-system, "Segoe UI", Ubuntu, Arial, sans-serif`;
  dctx.textBaseline = 'middle';
  dctx.textAlign = 'left';
  dctx.fillStyle = '#222';
  dctx.strokeStyle = '#9a9a9a';
  dctx.lineWidth = Math.max(1, Math.round(dpr));
  dctx.beginPath();
  const t0 = Math.ceil(view.left / step) * step, vr = view.left + view.window;
  for (let t = t0; t < vr; t += step) {
    const x = Math.round(G.plotX + (t - view.left) / nsPerPx) + 0.5;
    dctx.moveTo(x, 0); dctx.lineTo(x, G.hdr);
    if (t >= 0) dctx.fillText(fmtAxis(t, step), x + 3 * dpr, G.hdr / 2 + dpr);
  }
  dctx.stroke();
  const y = H - G.sts / 2;
  dctx.fillStyle = '#1d3fbf';
  dctx.fillText(fmtAbs(view.left), 4 * dpr, y);
  dctx.textAlign = 'right';
  dctx.fillText(fmtAbs(vr), W - 4 * dpr, y);
  dctx.textAlign = 'center';
  dctx.fillStyle = '#111';
  dctx.fillText(fmtSpan(view.window), W / 2, y);
}

// ---------------------------------------------------------------- frame
function drawFrame(now, nsPerPx) {
  const W = G.W, H = G.H;
  gl.viewport(0, 0, W, H);
  gl.disable(gl.SCISSOR_TEST);
  gl.clearColor(1, 1, 1, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);

  rect(0, 0, W, G.hdr, 0.957, 0.957, 0.957, 1);
  rect(0, H - G.sts, W, G.sts, 0.957, 0.957, 0.957, 1);
  rect(0, G.hdr, G.gut, G.rowsH, 0.94, 0.94, 0.94, 1);
  rect(G.plotX, G.rowsY, G.plotW, G.rowH, 0.91, 0.91, 0.975, 1);       // the first counter is "selected", like the old UI
  rect(0, G.rowsY, G.gut, G.rowH, 0.80, 0.80, 0.91, 1);
  for (let c = 1; c <= st.n; c++) rect(0, G.rowsY + c * G.rowH - 1, W, 1, 0.80, 0.80, 0.80, 1);
  rect(0, G.hdr - 1, W, 1, 0.70, 0.70, 0.70, 1);
  rect(0, H - G.sts, W, 1, 0.70, 0.70, 0.70, 1);
  flushRects();

  const pk = pickBlock(view.left, view.left + view.window, now);
  gaps.push(pk.cov < 0.999 ? 1 : 0);
  if (pk.block) drawBlock(pk.block, nsPerPx);

  if (cursor.in && !drag) {
    const x = Math.round(cursor.x * dpr);
    if (x >= G.plotX) rect(x, G.rowsY, Math.max(1, Math.round(dpr)), G.rowsH, 0, 0, 0, 0.35);
  }
  flushRects();
}

function drawBlock(b, nsPerPx) {
  const H = G.H, sx = 1 / nsPerPx, ox = (b.lo - view.left) / nsPerPx;
  const pad = 3 * dpr, spanPx = (Math.min(b.hi, b.genNow) - b.lo) / nsPerPx;
  gl.enable(gl.SCISSOR_TEST);
  gl.bindBuffer(gl.ARRAY_BUFFER, b.vbo);

  gl.useProgram(pLine);
  gl.uniform2f(UL.u_canvas, G.W, H);
  gl.uniform1f(UL.u_pad, pad);
  gl.uniform1f(UL.u_half, 0.5 * Math.max(1, dpr));
  gl.uniform2f(UL.u_x, sx, ox);
  gl.bindVertexArray(vaoLine);
  for (let c = 0; c < st.n; c++) {
    const cnt = b.counts[c];
    if (cnt < 2) continue;
    const d = DEFS[c % DEFS.length], y = G.rowsY + c * G.rowH, h = G.rowH - 1;
    if (h < 1) continue;
    gl.scissor(G.plotX, H - y - h, G.plotW, h);
    const off = b.offsets[c] * 8;
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, off);
    gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 8, off + 8);
    gl.uniform4f(UL.u_panel, G.plotX, y, G.plotW, h);
    gl.uniform2f(UL.u_v, d.lo, d.hi);
    gl.uniform4f(UL.u_color, d.rgb[0], d.rgb[1], d.rgb[2], 1);
    gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, cnt - 1);
    stats.draws++;
    stats.segs += cnt - 1;
  }

  if (st.marks !== 'off') {
    const size = 2 * Math.max(1, Math.round(dpr));
    gl.useProgram(pMark);
    gl.uniform2f(UM.u_canvas, G.W, H);
    gl.uniform1f(UM.u_pad, pad);
    gl.uniform1f(UM.u_size, size);
    gl.uniform1f(UM.u_inner, 1 - Math.max(1, Math.round(dpr)) / size);
    gl.uniform2f(UM.u_x, sx, ox);
    gl.bindVertexArray(vaoMark);
    for (let c = 0; c < st.n; c++) {
      const cnt = b.counts[c];
      if (!b.raw[c] || cnt < 1 || spanPx / cnt < 5 * dpr) continue;   // markers only where raw points stand apart
      const d = DEFS[c % DEFS.length], y = G.rowsY + c * G.rowH, h = G.rowH - 1;
      if (h < 8 * dpr) continue;
      gl.scissor(G.plotX, H - y - h, G.plotW, h);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, b.offsets[c] * 8);
      gl.uniform4f(UM.u_panel, G.plotX, y, G.plotW, h);
      gl.uniform2f(UM.u_v, d.lo, d.hi);
      gl.uniform4f(UM.u_color, d.rgb[0], d.rgb[1], d.rgb[2], 1);
      gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, cnt);
      stats.draws++;
      stats.marks += cnt;
    }
  }
  gl.bindVertexArray(null);
  gl.disable(gl.SCISSOR_TEST);
}

function clampView(now) {
  zoom.target = Math.min(Math.max(zoom.target, MIN_WINDOW), now * 1.1);
  if (live) return;
  const minL = -0.05 * view.window, maxL = Math.max(minL, now + 0.05 * view.window - view.window);
  view.left = Math.min(Math.max(view.left, minL), maxL);
}

let lastTs = 0, lastHud = 0, frameNo = 0;
function frame(ts) {
  requestAnimationFrame(frame);
  const tStart = performance.now();
  const dt = lastTs ? ts - lastTs : 16.7;
  if (lastTs && dt < 1000) intervals.push(dt);
  lastTs = ts;
  if (timerExt) pollGpu();

  const tU = performance.now();
  for (let i = 0; i < queue.length; i++) addBlock(queue[i]);
  queue.length = 0;
  uploadMs.push(performance.now() - tU);

  layout();
  const now = clockNs();
  if (view.window !== zoom.target) {             // zoom keeps the time under the anchor point in place
    const fx = live ? 1 : zoom.fx, anchor = view.left + fx * view.window;
    const a = st.smooth ? 1 - Math.exp(-dt / 45) : 1;
    let nw = view.window * Math.pow(zoom.target / view.window, a);
    if (Math.abs(Math.log(zoom.target / nw)) < 1e-3) nw = zoom.target;
    view.window = nw;
    view.left = anchor - fx * nw;
  }
  if (live) view.left = now - LIVE_DELAY - view.window;
  clampView(now);
  const nsPerPx = view.window / G.plotW;
  maybeRequest(now, nsPerPx);

  stats.draws = 0; stats.segs = 0; stats.marks = 0;
  if (timerExt) beginGpu();
  drawFrame(now, nsPerPx);
  if (timerExt) endGpu();

  if (layoutDirty) drawStaticText();
  drawDynamicText();
  layoutDirty = false;

  jsMs.push(performance.now() - tStart);
  frameNo++;
  if (ts - lastHud > 250) { lastHud = ts; updateHud(now, nsPerPx); }
}

// ---------------------------------------------------------------- HUD
function browserName() {
  const ua = navigator.userAgent;
  let m;
  if ((m = ua.match(/Edg\/(\d+)/))) return 'Edge ' + m[1];
  if ((m = ua.match(/Firefox\/(\d+)/))) return 'Firefox ' + m[1];
  if ((m = ua.match(/Chrome\/(\d+)/))) return 'Chrome ' + m[1];
  if ((m = ua.match(/Version\/([\d.]+).*Safari/))) return 'Safari ' + m[1];
  return ua.slice(0, 40);
}
function gpuName() {
  let r = gl.getParameter(gl.RENDERER) || '';
  if (!r || /^webkit webgl$/i.test(r)) {
    const ext = gl.getExtension('WEBGL_debug_renderer_info');
    if (ext) r = gl.getParameter(ext.UNMASKED_RENDERER_WEBGL);
  }
  return r;
}
const BROWSER = browserName(), GPU = gpuName();

function counterRate(d) {
  if (d.kind === 'events') return 1.3 * d.prob * 1e9 / (d.slot / st.mult);
  return 1e9 / (d.period / st.mult);
}
function windowPoints(d) {
  if (d.kind === 'range') return view.window / (d.period / st.mult);
  if (d.kind === 'cyclo') return 4 * view.window / (d.period / st.mult);
  return 5.2 * d.prob * view.window / (d.slot / st.mult);
}

function updateHud(now, nsPerPx) {
  const tNow = performance.now(), el = (tNow - rateT0) / 1000;
  if (el >= 1) { reqPerSec = rateReqs / el; bytesPerSec = rateBytes / el; rateReqs = 0; rateBytes = 0; rateT0 = tNow; }
  const iv = sorted(intervals), p50 = pct(iv, 0.5), p99 = pct(iv, 0.99), mx = iv.length ? iv[iv.length - 1] : 0;
  let janks = 0;
  for (let i = 0; i < iv.length; i++) if (iv[i] > 1.5 * p50) janks++;
  let gapFrames = 0;
  const gv = gaps.view();
  for (let i = 0; i < gv.length; i++) gapFrames += gv[i];
  const lb = stats.lastBlock;
  const cur = cursor.in ? fmtAxis(view.left + (cursor.x * dpr - G.plotX) * nsPerPx, nsPerPx / 100) : '—';
  hud.textContent = [
    `браузер   ${BROWSER}`,
    `GPU       ${GPU}`,
    `холст     ${G.W}×${G.H} px · DPR ${dpr}`,
    `── плавность`,
    `кадры     ${iv.length ? (1000 / avg(intervals)).toFixed(1) : '—'} к/с · дисплей ≈ ${p50 ? (1000 / p50).toFixed(0) : '—'} Гц`,
    `интервал  p50 ${p50.toFixed(2)} · p99 ${p99.toFixed(2)} · max ${mx.toFixed(2)} мс`,
    `рывки     ${janks} из ${iv.length} кадров (интервал > 1,5×p50)`,
    `JS/кадр   ср ${avg(jsMs).toFixed(2)} · max ${maxOf(jsMs).toFixed(2)} мс`,
    `GPU/кадр  ${timerExt ? (gpuMs.n ? avg(gpuMs).toFixed(2) + ' мс' : '…') : 'таймер браузером не дан'}`,
    `отрисовка ${stats.draws} draw · ${fmtInt(stats.segs)} сегм · ${fmtInt(stats.marks)} маркеров`,
    `── данные («сервер» в Web Worker)`,
    `окно      ${fmtNs(view.window)} · ${fmtNs(nsPerPx)} на пиксель`,
    `ответ     ${lb ? fmtBytes(lb.bytes) + ' · ' + fmtInt(lb.points) + ' точек · генерация ' + lb.genMs.toFixed(1) + ' мс' : '—'}`,
    `запросы   ${reqPerSec.toFixed(1)}/с · ${fmtBytes(bytesPerSec)}/с · RTT ${st.rtt} мс`,
    `без данных у края: ${gapFrames} из ${gv.length} кадров`,
    `курсор    ${cur}`,
  ].join('\n');

  const pk = pickBlock(view.left, view.left + view.window, now), b = pk.block;
  const spanPx = b ? (Math.min(b.hi, b.genNow) - b.lo) / nsPerPx : 0;
  const rows = [];
  for (let c = 0; c < Math.min(st.n, 40); c++) {
    const { d, name } = counterInfo(c);
    const cnt = b ? b.counts[c] : 0, raw = b ? b.raw[c] : 0;
    const marks = raw && st.marks !== 'off' && cnt && spanPx / cnt >= 5 * dpr && G.rowH - 1 >= 8 * dpr;
    rows.push(`<tr><td><span class="dot" style="background:${css(d.rgb)}"></span>${name}</td>` +
      `<td class="num">${fmtRate(counterRate(d))}</td><td class="num">${fmtInt(windowPoints(d))}</td>` +
      `<td class="num">${fmtInt(cnt)}</td><td>${b ? (raw ? 'сырые' + (marks ? ' + маркеры' : '') : 'M4') : '—'}</td></tr>`);
  }
  if (st.n > 40) rows.push(`<tr><td colspan="5">… ещё ${st.n - 40}</td></tr>`);
  countersBox.innerHTML = '<table><tr><th>Счётчик</th><th>частота</th><th>точек в окне</th>' +
    '<th>в ответе сервера</th><th>режим</th></tr>' + rows.join('') + '</table>';
}

// ---------------------------------------------------------------- input
function setLive(on) {
  live = on;
  btnLive.classList.toggle('on', on);
  btnLive.textContent = on ? '⏸ live' : '▶ live';
}
function zoomBy(f, fx) {
  zoom.target *= f;
  zoom.fx = fx;
}
const clamp01 = (v) => Math.min(1, Math.max(0, v));

glc.addEventListener('pointerdown', (e) => {
  if (e.button !== 0) return;
  drag = { x: e.clientX };
  glc.setPointerCapture(e.pointerId);
  glc.classList.add('drag');
  if (live) setLive(false);
  zoom.target = view.window;                       // grabbing stops a running zoom
});
glc.addEventListener('pointermove', (e) => {
  cursor.x = e.offsetX;
  cursor.in = true;
  if (!drag) return;
  const dx = e.clientX - drag.x;
  drag.x = e.clientX;
  view.left -= dx * dpr * (view.window / G.plotW);  // 1:1 with the mouse
});
const endDrag = () => { drag = null; glc.classList.remove('drag'); };
glc.addEventListener('pointerup', endDrag);
glc.addEventListener('pointercancel', endDrag);
glc.addEventListener('pointerleave', () => { cursor.in = false; });
glc.addEventListener('wheel', (e) => {
  e.preventDefault();
  let dx = e.deltaX, dy = e.deltaY;
  if (e.deltaMode === 1) { dx *= 16; dy *= 16; } else if (e.deltaMode === 2) { dx *= 400; dy *= 400; }
  if (Math.abs(dx) > Math.abs(dy) && !e.ctrlKey) {  // horizontal trackpad swipe pans
    if (live) setLive(false);
    view.left += dx * dpr * (view.window / G.plotW);
    return;
  }
  zoomBy(Math.exp(dy * (e.ctrlKey ? 0.01 : 0.0015)), live ? 1 : clamp01((e.offsetX * dpr - G.plotX) / G.plotW));
}, { passive: false });

window.addEventListener('keydown', (e) => {
  if (e.target instanceof HTMLSelectElement || e.metaKey || e.ctrlKey || e.altKey) return;
  const now = clockNs();
  switch (e.code) {                                // event.code: works in any keyboard layout
    case 'Space': setLive(!live); break;
    case 'Home': setLive(false); view.left = 0; view.window = zoom.target = INIT_WINDOW; break;
    case 'End': setLive(false); view.left = now - view.window; break;
    case 'KeyF': setLive(false); view.window = zoom.target = now * 1.02; view.left = -0.01 * now; break;
    case 'Equal': case 'NumpadAdd': zoomBy(1 / 1.6, live ? 1 : 0.5); break;
    case 'Minus': case 'NumpadSubtract': zoomBy(1.6, live ? 1 : 0.5); break;
    case 'ArrowLeft': case 'ArrowRight':
      setLive(false);
      view.left += (e.code === 'ArrowLeft' ? -1 : 1) * view.window * (e.shiftKey ? 0.5 : 0.1);
      break;
    default: return;
  }
  e.preventDefault();
});

btnLive.addEventListener('click', () => { setLive(!live); btnLive.blur(); });
function onSelect(id, fn) {
  const el = $(id);
  el.addEventListener('change', () => { fn(el.value); el.blur(); });
}
onSelect('sel-n', (v) => { st.n = +v; dropBlocks(); layoutDirty = true; });
onSelect('sel-mult', (v) => { st.mult = +v; dropBlocks(); });
onSelect('sel-rtt', (v) => { st.rtt = +v; });
onSelect('sel-marks', (v) => { st.marks = v; });
onSelect('sel-smooth', (v) => { st.smooth = v === 'on'; });
document.addEventListener('visibilitychange', () => { intervals.clear(); gaps.clear(); lastTs = 0; });
glc.addEventListener('webglcontextlost', (e) => { e.preventDefault(); errBox.textContent = 'WebGL-контекст потерян, перезагрузите страницу.'; });

requestAnimationFrame(frame);
})();
