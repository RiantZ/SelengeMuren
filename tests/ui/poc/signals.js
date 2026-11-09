// Synthetic counters modelled on the old Telemetry viewer, shared by the page and server_sim.js.
// period/slot/pulse are in ns at rate x1; f(t seconds, sample index, seed) gives a sample value.
const TAU = 6.283185307179586;
function hash32(x) {
  x |= 0;
  x ^= x >>> 16; x = Math.imul(x, 0x7feb352d);
  x ^= x >>> 15; x = Math.imul(x, 0x846ca68b);
  x ^= x >>> 16;
  return x >>> 0;
}
// uniform [0, 1) for a non-negative integer index below 2^53
function h01(k, seed) {
  const hi = Math.floor(k / 4294967296);
  return hash32((k - hi * 4294967296) ^ hash32(hi + seed * 40503)) / 4294967296;
}
function vnoise(x, seed) {
  const i = Math.floor(x), f = x - i, a = h01(i, seed), b = h01(i + 1, seed);
  return a + (b - a) * f * f * (3 - 2 * f);
}
function ramp(t) { return t < 0.15 ? Math.max(0, t / 0.15) : 1; }
function dip(t) { if (t < 2.5) return 0; return (t - 2.5) % 40 < 3 ? 1 : 0; }

const DEFS = [
  { name: 'Playback/Audio/Hardware buffer fullness (ms)', rgb: [0.18, 0.80, 0.22], lo: 0, hi: 50, kind: 'range', period: 20e6,
    f: (t, k, s) => 27 + 8 * Math.sin(TAU * 0.9 * t + s) + 10 * (h01(k, s + 1) - 0.5) + 6 * (vnoise(t * 0.8, s + 2) - 0.5) },
  { name: 'Playback/Audio/Rendering cyclogramm', rgb: [0.90, 0.10, 0.90], lo: 0, hi: 1, kind: 'cyclo', period: 21.33e6, duty: 0.4 },
  { name: 'Playback/Audio/Software buffer fullness (ms)', rgb: [0.05, 0.05, 0.05], lo: 0, hi: 200, kind: 'range', period: 40e6,
    f: (t, k, s) => 118 + 7 * Math.sin(TAU * 0.31 * t + s) + 6 * (h01(k, s + 3) - 0.5) },
  { name: 'Playback/Video/Decoding cyclogramm', rgb: [0.62, 0.18, 0.16], lo: 0, hi: 1, kind: 'events', slot: 97e6, pulse: 1.4e6, prob: 0.62 },
  { name: 'Playback/Video/Raw buffers', rgb: [0.92, 0.12, 0.12], lo: 0, hi: 16, kind: 'range', period: 40e6,
    f: (t, k, s) => {
      const p = 0.21 + 0.04 * Math.sin(s), ph = ((t + s * 0.13) / p) % 1;
      return 3.2 + 6.4 * (1 - ph) + 0.5 * (h01(k, s + 4) - 0.5);
    } },
  { name: 'Playback/Video/Rendering cyclogramm', rgb: [0.05, 0.50, 0.10], lo: 0, hi: 1, kind: 'cyclo', period: 33.33e6, duty: 0.55 },
  { name: 'Process/CPU usage', rgb: [0.08, 0.10, 0.52], lo: 0, hi: 100, kind: 'range', period: 70e6,
    f: (t, k, s) => ramp(t) * (67 + 2.5 * Math.sin(TAU * t / 7.3 + s) + 1.5 * (h01(k, s + 5) - 0.5) - 40 * dip(t + s * 5.3)) },
  { name: 'Process/Memory in mb', rgb: [0.88, 0.10, 0.10], lo: 0, hi: 1024, kind: 'range', period: 70e6,
    f: (t, k, s) => ramp(t) * (505 + 25 * Math.sin(TAU * t / 61 + s) + 5 * (h01(k, s + 6) - 0.5) - 190 * dip(t + s * 5.3)) },
  { name: 'Process/Threads', rgb: [0.08, 0.10, 0.52], lo: 0, hi: 32, kind: 'range', period: 70e6,
    f: (t, k, s) => Math.round(ramp(t) * (30 + 1.2 * Math.sin(TAU * t / 23 + s) - 13 * dip(t + s * 5.3))) },
];
