// parity.mjs - does main/aura.c draw what reference/aurora.js draws?
//
// For each state, runs the real aura.c (through aura_host, built by build.sh)
// from a reset for t seconds and renders the same instant with aurora.js, then
// compares the two frames in the driver's 5-bit levels, which is what the panel
// shows. The JS is run with the board's shader shape (copies, grid, layers, read
// from aura.c's #defines). After aura_reset() the C side's tween and phases have closed forms
// (from the dark row toward the state, u = t / AURA_TWEEN_S, v = t / AURA_FADE_S,
// anim = t * 0.05 * speed, pulse = t * rate, because the pace does not change
// during a fade-in), so the JS can be put at exactly the same point.
//
//     node host/parity.mjs                      # every state at 0.475 s (mid-tween) and 2.975 s (settled)
//     node host/parity.mjs --at 2.975 --states idle,error --text 09:41
//     node host/parity.mjs --no-build           # keep the current aura_host
//
// Exit 1 when any frame is outside the band: more than MAX_FAR_PCT of the
// samples differ by 3 or more levels, or any sample differs by more than
// MAX_DIFF. Off-by-one is expected here and there (the C uses table sines and
// a fast square root); a whole region off is a real divergence.
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const require = createRequire(import.meta.url);
const HERE = path.dirname(new URL(import.meta.url).pathname);
const A = require(path.join(HERE, '..', 'reference', 'aurora.js'));

const FPS = 40;
// The board's shader shape, read from aura.c so the check follows the firmware.
const auraC = fs.readFileSync(path.join(HERE, '..', 'main', 'aura.c'), 'utf8');
const cdef = name => { const m = auraC.match(new RegExp(`#define\\s+${name}\\s+(\\d+)`)); if (!m) throw new Error(`parity: no #define ${name} in aura.c`); return Number(m[1]); };
const GRID = cdef('AURA_RES'), COPIES = cdef('AURA_COPIES'), LAYERS = cdef('AURA_LAYERS');
const MAX_FAR_PCT = 0.5, MAX_DIFF = 4;  // the band; see the README

const args = process.argv.slice(2);
function opt(name, dflt) { const i = args.indexOf(name); return i >= 0 && i + 1 < args.length ? args[i + 1] : dflt; }
const ats = opt('--at', '0.475,2.975').split(',').map(Number);
const states = opt('--states', A.STATES.join(',')).split(',');
const text = opt('--text', '14:32');
const verbose = args.includes('--verbose');

if (!args.includes('--no-build')) execFileSync('bash', [path.join(HERE, 'build.sh')], { stdio: ['ignore', 'ignore', 'inherit'] });
const host = path.join(HERE, 'aura_host');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'aura-parity-'));

function readPpm(file) {
  const buf = fs.readFileSync(file);
  const header = buf.toString('latin1', 0, 20).match(/^P6\s+(\d+)\s+(\d+)\s+255\s/);
  if (!header) throw new Error(`${file}: not the P6 header aura_host writes`);
  const start = header[0].length;
  return buf.subarray(start, start + 64 * 64 * 3);
}
const level = v => Math.round(Math.pow(v / 255, 2.2) * 31);

function jsFrame(state, t) {
  const withText = state === 'idle' || state === 'alert';
  const to = A.auraParams(state, withText), from = A.auraDark(to);
  const cur = A.auraMix(from, to, A.auraEase(Math.min(1, t / A.AURA_TWEEN_S)), A.auraEase(Math.min(1, t / A.AURA_FADE_S)));
  return A.render(state, t, { params: cur, phases: A.auraPhases(cur, t), text, iterations: COPIES, grid: GRID, layers: LAYERS, bits: 5, dither: true });
}

let failed = false;
console.log(`board shader from aura.c: ${COPIES} copies, ${GRID}x${GRID} grid, ${LAYERS} turbulence layers\nstate     t      exact   ±1     ±2     ≥3     max  mean level (js / c)`);
for (const state of states) {
  if (!A.STATES.includes(state)) { console.error(`parity: unknown state ${state}`); process.exit(2); }
  for (const t of ats) {
    const k = Math.round(t * FPS);                   // the frame index aura_host will write last
    const out = path.join(tmp, `${state}-${k}`);
    execFileSync(host, ['--fps', String(FPS), '--every', '0', '--out', out, '--text', text, `${state}:${(k + 1) / FPS}`], { stdio: ['ignore', 'ignore', 'inherit'] });
    const c = readPpm(path.join(out, `f${String(k).padStart(5, '0')}_${state}.ppm`));
    const js = jsFrame(state, k / FPS);
    let exact = 0, off1 = 0, off2 = 0, far = 0, max = 0, sumJs = 0, sumC = 0, worst = null;
    for (let i = 0; i < 64 * 64 * 3; i++) {
      const a = level(js[i]), b = level(c[i]), d = Math.abs(a - b);
      sumJs += a; sumC += b;
      if (d === 0) exact++; else if (d === 1) off1++; else if (d === 2) off2++; else far++;
      if (d > max) { max = d; worst = { x: (i / 3 | 0) % 64, y: (i / 3 / 64) | 0, ch: 'rgb'[i % 3], js: a, c: b }; }
    }
    const n = 64 * 64 * 3, pct = v => (100 * v / n).toFixed(1).padStart(5) + '%';
    const farPct = 100 * far / n, bad = farPct > MAX_FAR_PCT || max > MAX_DIFF;
    if (bad) failed = true;
    console.log(`${state.padEnd(8)} ${(k / FPS).toFixed(3)}  ${pct(exact)} ${pct(off1)} ${pct(off2)} ${pct(far)}  ${String(max).padStart(3)}  ${(sumJs / n).toFixed(3)} / ${(sumC / n).toFixed(3)}${bad ? '   OUT OF BAND' : ''}`);
    if (verbose && worst) console.log(`         worst at x=${worst.x} y=${worst.y} ${worst.ch}: js ${worst.js} c ${worst.c}`);
  }
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(failed ? `parity: FAIL (band: ≥3-level samples ≤ ${MAX_FAR_PCT}%, max ≤ ${MAX_DIFF})` : `parity: ok (band: ≥3-level samples ≤ ${MAX_FAR_PCT}%, max ≤ ${MAX_DIFF})`);
process.exit(failed ? 1 : 0);
