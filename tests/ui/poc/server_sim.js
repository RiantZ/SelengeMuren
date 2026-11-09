// The "server", runs in a Web Worker: answers a window request (time, scale, duration) with raw points,
// or with M4 points (first/min/max/last per pixel column) when raw ones are denser than 2 per pixel.
importScripts('signals.js');

const K = 8;                      // samples read per M4 bucket (a real server would read all)
const pulseBuf = new Float64Array(2);

// pulse starts of slot q for an 'events' counter, returns their number
function pulses(q, seed, Q, L, prob) {
  if (h01(q, seed + 11) >= prob) return 0;
  pulseBuf[0] = q * Q + h01(q, seed + 12) * (Q - 6 * L);
  if (h01(q, seed + 13) < 0.3) { pulseBuf[1] = pulseBuf[0] + 3 * L; return 2; }
  return 1;
}

function rangeRaw(d, seed, lo, hi, mult, now, pts) {
  const P = d.period / mult;
  const k0 = Math.max(0, Math.floor(lo / P) - 1), k1 = Math.min(Math.ceil(hi / P) + 1, Math.floor(now / P));
  for (let k = k0; k <= k1; k++) {
    const t = k === 0 ? 0 : k * P + (h01(k, seed + 77) - 0.5) * 0.3 * P;
    if (t > now) break;
    pts.push(t - lo, d.f(t * 1e-9, k, seed));
  }
}

function rangeM4(d, seed, lo, hi, w, mult, now, pts) {
  const P = d.period / mult;
  const b0 = Math.floor(lo / w), b1 = Math.ceil(Math.min(hi, now) / w);
  for (let b = b0; b < b1; b++) {
    const t0 = b * w, t1 = Math.min(t0 + w, now);
    const ka = Math.ceil(t0 / P), kb = Math.ceil(t1 / P) - 1;
    if (kb < ka) continue;
    const cnt = kb - ka + 1, steps = Math.min(K, cnt);
    let ft = 0, fv = 0, lt = 0, lv = 0, nt = 0, nv = Infinity, xt = 0, xv = -Infinity;
    for (let i = 0; i < steps; i++) {
      const k = steps === 1 ? ka : ka + Math.round(i * (cnt - 1) / (steps - 1));
      const t = k * P, v = d.f(t * 1e-9, k, seed);
      if (i === 0) { ft = t; fv = v; }
      lt = t; lv = v;
      if (v < nv) { nv = v; nt = t; }
      if (v > xv) { xv = v; xt = t; }
    }
    pts.push(ft - lo, fv);
    if (nt <= xt) pts.push(nt - lo, nv, xt - lo, xv); else pts.push(xt - lo, xv, nt - lo, nv);
    pts.push(lt - lo, lv);
  }
}

function cycloRaw(d, lo, hi, mult, now, pts) {
  const P = d.period / mult, D = P * d.duty;
  const k0 = Math.max(0, Math.floor(lo / P) - 1), k1 = Math.min(Math.ceil(hi / P) + 1, Math.floor(now / P));
  for (let k = k0; k <= k1; k++) {
    const ts = k * P, te = ts + D;
    pts.push(ts - lo, 0, ts - lo, 1);
    if (te > now) { pts.push(now - lo, 1); break; }
    pts.push(te - lo, 1, te - lo, 0);
  }
}

function cycloM4(d, lo, hi, w, mult, now, pts) {
  const P = d.period / mult, D = P * d.duty;
  const state = (t) => (t - Math.floor(t / P) * P < D ? 1 : 0);
  const b0 = Math.floor(lo / w), b1 = Math.ceil(Math.min(hi, now) / w);
  for (let b = b0; b < b1; b++) {
    const t0 = b * w, t1 = Math.min(t0 + w, now);
    const s0 = state(t0), s1 = state(Math.max(t0, t1 - 1e-3)), kk = Math.floor(t0 / P);
    const hasOn = s0 === 1 || (kk + 1) * P < t1, hasOff = s0 === 0 || kk * P + D < t1;
    pts.push(t0 - lo, s0);
    if (hasOn && hasOff) pts.push(t0 + 0.33 * (t1 - t0) - lo, 1 - s0, t0 + 0.66 * (t1 - t0) - lo, s0);
    pts.push(t1 - lo, s1);
  }
}

function eventsRaw(d, seed, lo, hi, mult, now, pts) {
  const Q = d.slot / mult, L = d.pulse / mult;
  const q0 = Math.max(0, Math.floor(lo / Q) - 1), q1 = Math.ceil(hi / Q) + 1;
  pts.push(q0 * Q - lo, 0);
  for (let q = q0; q <= q1; q++) {
    const n = pulses(q, seed, Q, L, d.prob);
    for (let i = 0; i < n; i++) {
      const ts = pulseBuf[i], te = ts + L;
      if (te > now) return;
      pts.push(ts - lo, 0, ts - lo, 1, te - lo, 1, te - lo, 0);
    }
  }
  pts.push(Math.min((q1 + 1) * Q, now) - lo, 0);
}

function eventsM4(d, seed, lo, hi, w, mult, now, pts) {
  const Q = d.slot / mult, L = d.pulse / mult;
  const b0 = Math.floor(lo / w), b1 = Math.ceil(Math.min(hi, now) / w);
  let runOpen = false, runEnd = 0;
  for (let b = b0; b < b1; b++) {
    const t0 = b * w, t1 = Math.min(t0 + w, now);
    let any = false;
    for (let q = Math.max(0, Math.floor((t0 - L) / Q)); q <= Math.floor(t1 / Q) && !any; q++) {
      const n = pulses(q, seed, Q, L, d.prob);
      for (let i = 0; i < n; i++) if (pulseBuf[i] < t1 && pulseBuf[i] + L > t0) { any = true; break; }
    }
    if (!any) {                                  // flat zero: keep only the ends of the run
      if (!runOpen) { pts.push(t0 - lo, 0); runOpen = true; }
      runEnd = t1;
      continue;
    }
    if (runOpen) { pts.push(t0 - lo, 0); runOpen = false; }
    pts.push(t0 + 0.5 * (t1 - t0) - lo, 1, t1 - lo, 0);
  }
  if (runOpen) pts.push(runEnd - lo, 0);
}

function estimate(d, span, mult) {
  if (d.kind === 'range') return span / (d.period / mult);
  if (d.kind === 'cyclo') return 4 * span / (d.period / mult);
  return 5.2 * d.prob * span / (d.slot / mult);
}

function build(lo, hi, nsPerPx, n, mult, now) {
  const pts = [];
  const offsets = new Uint32Array(n), counts = new Uint32Array(n), raw = new Uint8Array(n);
  const spanPx = Math.max(1, (hi - lo) / nsPerPx);
  const w = 2 ** Math.floor(Math.log2(nsPerPx));   // M4 bucket: a power of two, at most one pixel
  for (let c = 0; c < n; c++) {
    const d = DEFS[c % DEFS.length], seed = Math.floor(c / DEFS.length);
    const start = pts.length / 2;
    const isRaw = estimate(d, hi - lo, mult) <= 2 * spanPx;
    if (d.kind === 'range') isRaw ? rangeRaw(d, seed, lo, hi, mult, now, pts) : rangeM4(d, seed, lo, hi, w, mult, now, pts);
    else if (d.kind === 'cyclo') isRaw ? cycloRaw(d, lo, hi, mult, now, pts) : cycloM4(d, lo, hi, w, mult, now, pts);
    else isRaw ? eventsRaw(d, seed, lo, hi, mult, now, pts) : eventsM4(d, seed, lo, hi, w, mult, now, pts);
    offsets[c] = start;
    counts[c] = pts.length / 2 - start;
    raw[c] = isRaw ? 1 : 0;
  }
  return { offsets, counts, raw, buf: new Float32Array(pts) };
}

self.onmessage = (e) => {
  const m = e.data;
  const t0 = performance.now();
  const now = (performance.timeOrigin + performance.now() - m.epochMs) * 1e6;
  const hi = Math.min(m.hi, now);
  const out = build(m.lo, hi, m.nsPerPx, m.n, m.mult, now);
  const genMs = performance.now() - t0;
  const reply = () => self.postMessage({
    id: m.id, gen: m.gen, lo: m.lo, hi, genNow: now, nsPerPx: m.nsPerPx,
    offsets: out.offsets, counts: out.counts, raw: out.raw, buf: out.buf.buffer, genMs,
  }, [out.buf.buffer]);
  if (m.rtt > 0) setTimeout(reply, m.rtt); else reply();
};
