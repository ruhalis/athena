// sheet.mjs - a contact sheet straight from reference/aurora.js, no C involved:
// the eight states as rows, six instants each, rendered the way the panel
// shows them (the board's shader shape: 12 copies on a 32 grid with 3
// turbulence layers, then 5-bit levels with the Bayer dither). The quick look
// while the AURA table is being tuned; sheet.py is the same picture from the C port.
//
//     node host/sheet.mjs                          # writes host/out/js-states.png
//     node host/sheet.mjs --out /tmp/x.png --scale 4 --text 14:32
//     node host/sheet.mjs --iterations 36 --grid 64 --layers 4   # the shader as LiveKit runs it
//     TIMES='{"speak":[0,0.1,0.2,0.3,0.4,0.5]}' node host/sheet.mjs   # override a row's instants
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const require = createRequire(import.meta.url);
const HERE = path.dirname(new URL(import.meta.url).pathname);
const A = require(path.join(HERE, '..', 'reference', 'aurora.js'));

const args = process.argv.slice(2);
function opt(name, dflt) { const i = args.indexOf(name); return i >= 0 && i + 1 < args.length ? args[i + 1] : dflt; }
const out = opt('--out', path.join(HERE, 'out', 'js-states.png'));
const S = Number(opt('--scale', 4)), text = opt('--text', '14:32');
const iterations = Number(opt('--iterations', 12)), grid = Number(opt('--grid', 32)), layers = Number(opt('--layers', 3));
const GAP = 8, BG = 0x18;
const TIMES = Object.assign({
  idle: [0.0, 1.2, 2.4, 3.1, 4.6, 6.0], listen: [0.0, 0.25, 0.5, 0.75, 1.0, 1.5], think: [0.0, 0.6, 1.2, 1.8, 2.4, 3.0],
  work: [0.0, 0.5, 1.0, 1.5, 2.0, 2.5], speak: [0.0, 0.16, 0.32, 0.48, 0.64, 0.8], alert: [0.0, 0.25, 0.5, 0.75, 1.0, 3.1],
  error: [0.0, 0.125, 0.25, 0.375, 0.5, 0.75], sleep: [0.0, 1.5, 3.0, 4.5, 6.0, 8.0],
}, JSON.parse(process.env.TIMES || '{}'));

function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
  return Buffer.concat([len, td, crc]);
}
function png(W, H, rgb) {
  const raw = Buffer.alloc((W * 3 + 1) * H);
  for (let y = 0; y < H; y++) { raw[y * (W * 3 + 1)] = 0; raw.set(rgb.subarray(y * W * 3, (y + 1) * W * 3), y * (W * 3 + 1) + 1); }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 6 })), chunk('IEND', Buffer.alloc(0))]);
}

const cols = Math.max(...A.STATES.map(m => TIMES[m].length));
const W = GAP + cols * (64 * S + GAP), H = GAP + A.STATES.length * (64 * S + GAP);
const buf = Buffer.alloc(W * H * 3, BG);
A.STATES.forEach((m, r) => TIMES[m].forEach((t, c) => {
  const withText = m === 'idle' || m === 'alert';
  const rgb = A.render(m, t, { withText, text, iterations, grid, layers, bits: 5, dither: true });
  const ox = GAP + c * (64 * S + GAP), oy = GAP + r * (64 * S + GAP);
  for (let y = 0; y < 64 * S; y++) for (let x = 0; x < 64 * S; x++) {
    const si = ((y / S | 0) * 64 + (x / S | 0)) * 3, di = ((oy + y) * W + ox + x) * 3;
    buf[di] = rgb[si]; buf[di + 1] = rgb[si + 1]; buf[di + 2] = rgb[si + 2];
  }
}));
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, png(W, H, buf));
console.log(`${out} (${W}x${H}; rows ${A.STATES.join(', ')}; ${iterations} copies, ${grid} grid, ${layers} layers)`);
