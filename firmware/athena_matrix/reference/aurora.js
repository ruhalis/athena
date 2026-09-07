/* aurora.js - Athena "Aurora" face renderer for the 64x64 panel.
 *
 * Four formless looks share one pipeline. AURA (the default): a port of the
 * LiveKit / Unicorn Studio aura shader, a circle outline drawn dozens of times
 * through a turbulence warp and averaged (licence note at sceneAura); every
 * state is one row of numbers in that vocabulary and a state change is a
 * tween between two rows. NEBULA:
 * a ring made of cloud over a low haze. RING: three crisp ribbons on black.
 * CLOUD: soft blobs drifting through a warping field. Each state sets colour,
 * size, rhythm and tendency; `t` sits in the centre.
 *
 * Written to be ported to face.c one-to-one: additive light in linear space,
 * per pixel one atan2/sqrt, a handful of sines and exps. Time t is seconds;
 * frame = t * 40. The last step models the hub75 driver: quantise each channel
 * to the panel's bit depth (5 today, 8 with the DMA driver), optionally with
 * ordered dithering, then gamma-encode for a monitor so the preview shows what
 * the LEDs emit, banding included.
 */
(function (root) {
  'use strict';
  var W = 64, H = 64, TAU = Math.PI * 2;
  var STATES = ['idle', 'listen', 'think', 'work', 'speak', 'alert', 'error', 'sleep'];

  function lin(r, g, b) { return [Math.pow(r / 255, 2.2), Math.pow(g / 255, 2.2), Math.pow(b / 255, 2.2)]; }
  function mix(a, b, u) { return [a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u]; }
  function clamp01(v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
  function smooth(e0, e1, x) { var t = clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }
  function frac(v) { return v - Math.floor(v); }
  function hash(n) { var x = Math.sin(n * 12.9898 + 78.233) * 43758.5453; return x - Math.floor(x); }

  /* Palettes: the cloud's outer colour (thin gas) and its core (dense light). */
  var PAL = {
    idle:   { outer: lin(10, 60, 120),  core: lin(190, 240, 255) },
    listen: { outer: lin(0, 90, 60),    core: lin(170, 255, 210) },
    think:  { outer: lin(50, 10, 110),  core: lin(220, 160, 255) },
    work:   { outer: lin(110, 40, 0),   core: lin(255, 220, 150) },
    error:  { outer: lin(110, 0, 10),   core: lin(255, 120, 100) },
    sleep:  { outer: lin(15, 10, 60),   core: lin(90, 80, 200) }
  };
  PAL.speak = PAL.idle;
  PAL.alert = PAL.idle;
  var AMBER = { outer: lin(120, 70, 0), core: lin(255, 230, 160) };

  /* Cloud parameters: blobs (sigma, orbit radius, angular speed, weight),
   * warp amplitude and speed, gain, plus a few per-state switches. */
  var CFG = {
    idle:   { master: 1.0, blobs: [[14, 3, 0.25, 1.0], [9, 6, -0.18, 0.5]],                     warp: 1.8, wspeed: 0.5, gain: 1.0, stars: 0.5, text: true },
    listen: { master: 1.0, blobs: [[15, 2, 0.4, 1.0], [9, 7, -0.5, 0.5]],                       warp: 1.8, wspeed: 0.8, gain: 1.1, stars: 0.0, text: false },
    think:  { master: 1.0, blobs: [[12, 5, 0.6, 0.7], [8, 9, -0.45, 0.5]],                       warp: 3.0, wspeed: 1.0, gain: 0.9, stars: 0.0, text: false },
    work:   { master: 1.0, blobs: [[9, 8, 2.4, 0.8], [9, 8, 2.4, 0.8], [9, 8, 2.4, 0.8], [10, 0, 0, 0.5]], warp: 1.8, wspeed: 1.6, gain: 1.0, stars: 0.0, text: false },
    speak:  { master: 1.0, blobs: [[14, 3, 0.25, 1.0], [9, 6, -0.18, 0.5]],                     warp: 1.8, wspeed: 0.5, gain: 1.0, stars: 0.3, text: false },
    alert:  { master: 1.0, blobs: [[14, 3, 0.25, 1.0], [9, 6, -0.18, 0.5]],                     warp: 1.8, wspeed: 0.5, gain: 1.0, stars: 0.5, text: true },
    error:  { master: 1.0, blobs: [[11, 0, 0, 1.2]],                                            warp: 4.0, wspeed: 3.0, gain: 1.1, stars: 0.0, text: false },
    sleep:  { master: 0.6, blobs: [[9, 3, 0.12, 1.0]],                                          warp: 1.5, wspeed: 0.2, gain: 1.3, stars: 1.2, text: false }
  };

  /* RING look: three ribbon colours per state, centre, radius, band width,
   * wobble amplitude and speed. */
  var RPAL = {
    idle:   [lin(30, 110, 255), lin(0, 210, 255), lin(80, 255, 210)],
    listen: [lin(0, 200, 120), lin(120, 255, 170), lin(0, 255, 220)],
    think:  [lin(140, 60, 255), lin(220, 100, 255), lin(90, 40, 220)],
    work:   [lin(255, 120, 0), lin(255, 170, 40), lin(255, 60, 20)],
    error:  [lin(255, 30, 30), lin(255, 90, 60), lin(200, 0, 30)],
    sleep:  [lin(40, 40, 180), lin(90, 60, 200), lin(30, 20, 120)]
  };
  RPAL.speak = RPAL.idle;
  RPAL.alert = RPAL.idle;
  var RAMBER = [lin(255, 200, 0), lin(255, 235, 120), lin(255, 160, 0)];
  var RCFG = {
    idle:   { master: 1.0, cx: 32, cy: 32, R: 22, sigma: 3.6, wob: 2.2, speed: 1.0, gain: 0.55, stars: 0.4, text: true },
    listen: { master: 1.0, cx: 32, cy: 32, R: 22, sigma: 3.6, wob: 2.2, speed: 1.6, gain: 0.65, stars: 0.0, text: false },
    think:  { master: 1.0, cx: 36, cy: 28, R: 20, sigma: 2.8, wob: 3.0, speed: 1.4, gain: 0.6, stars: 0.0, text: false },
    work:   { master: 1.0, cx: 32, cy: 32, R: 20, sigma: 3.2, wob: 1.5, speed: 1.2, gain: 0.7, stars: 0.0, text: false },
    speak:  { master: 1.0, cx: 32, cy: 32, R: 21, sigma: 3.2, wob: 2.2, speed: 1.0, gain: 0.55, stars: 0.3, text: false },
    alert:  { master: 1.0, cx: 32, cy: 32, R: 22, sigma: 3.6, wob: 2.2, speed: 1.0, gain: 0.55, stars: 0.4, text: true },
    error:  { master: 1.0, cx: 32, cy: 32, R: 20, sigma: 2.8, wob: 4.0, speed: 3.5, gain: 0.7, stars: 0.0, text: false },
    sleep:  { master: 0.6, cx: 32, cy: 40, R: 13, sigma: 2.5, wob: 1.5, speed: 0.3, gain: 0.7, stars: 1.2, text: false }
  };
  var WHITE = lin(255, 255, 255);

  /* Sixteen fixed stars, each with its own twinkle rate and phase. */
  var STARS = [];
  (function () {
    var seed = 7;
    for (var i = 0; i < 16; i++) {
      STARS.push({ x: Math.floor(hash(seed + i * 3) * 64), y: Math.floor(hash(seed + i * 3 + 1) * 64), f: 0.4 + hash(seed + i * 3 + 2) * 1.2, p: hash(i + 99) * TAU });
    }
  })();
  var STAR_COL = lin(150, 170, 255);

  /* 5x7 font, the glyphs `t` shows in a time: digits and the colon (font5x7.c). */
  var GLYPH = {
    '0': [0x3E, 0x51, 0x49, 0x45, 0x3E], '1': [0x00, 0x42, 0x7F, 0x40, 0x00], '2': [0x42, 0x61, 0x51, 0x49, 0x46],
    '3': [0x21, 0x41, 0x45, 0x4B, 0x31], '4': [0x18, 0x14, 0x12, 0x7F, 0x10], '5': [0x27, 0x45, 0x45, 0x45, 0x39],
    '6': [0x3C, 0x4A, 0x49, 0x49, 0x30], '7': [0x01, 0x71, 0x09, 0x05, 0x03], '8': [0x36, 0x49, 0x49, 0x49, 0x36],
    '9': [0x06, 0x49, 0x49, 0x29, 0x1E], ':': [0x00, 0x36, 0x36, 0x00, 0x00], ' ': [0, 0, 0, 0, 0]
  };
  var TEXT_COL = lin(255, 225, 180);
  var textMaskCache = {};
  function textMask(text, TEXT_Y) {
    var key = text + '@' + TEXT_Y;
    if (textMaskCache[key]) return textMaskCache[key];
    var m = new Uint8Array(W * H);
    var w = text.length * 6 - 1, x0 = Math.floor((W - w) / 2);
    for (var i = 0; i < text.length; i++) {
      var g = GLYPH[text[i]] || GLYPH[' '];
      for (var c = 0; c < 5; c++) for (var r = 0; r < 7; r++) {
        if (g[c] & (1 << r)) m[(TEXT_Y + r) * W + x0 + i * 6 + c] = 2;
      }
    }
    for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) {
      if (m[y * W + x]) continue;
      var near = false;
      for (var dy = -1; dy <= 1 && !near; dy++) for (var dx = -1; dx <= 1; dx++) {
        var xx = x + dx, yy = y + dy;
        if (xx >= 0 && xx < W && yy >= 0 && yy < H && m[yy * W + xx] === 2) { near = true; break; }
      }
      if (near) m[y * W + x] = 1;
    }
    textMaskCache[key] = m;
    return m;
  }

  /* Distance from a point to a segment. */
  function segDist(px, py, ax, ay, bx, by) {
    var vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    var u = clamp01((wx * vx + wy * vy) / (vx * vx + vy * vy));
    var dx = px - (ax + vx * u), dy = py - (ay + vy * u);
    return Math.sqrt(dx * dx + dy * dy);
  }

  var BAYER4 = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

  /* CLOUD look. Fills acc with linear light; returns the text row. */
  function sceneCloud(mode, t, frame, opts, acc) {
    var cfg = CFG[mode] || CFG.idle, pal = PAL[mode] || PAL.idle;
    var x, y, i, k;

    var master = cfg.master, gain = cfg.gain, scale = 1.0;
    var outer = pal.outer, core = pal.core;
    var pulse = 0.5 + 0.5 * Math.sin(TAU * t);

    /* Breath and the per-state rhythm. */
    if (mode === 'idle' || mode === 'alert') scale = 1 + 0.12 * Math.sin(TAU * t / 6);
    if (mode === 'sleep') { scale = 1 + 0.1 * Math.sin(TAU * t / 5); master *= 0.8 + 0.2 * Math.sin(TAU * t / 5); }
    if (mode === 'listen') { scale = 1.05 + 0.1 * Math.sin(TAU * t); gain *= 0.9 + 0.2 * pulse; }
    if (mode === 'alert') {
      outer = AMBER.outer; core = AMBER.core;          /* gold, flashing once a second */
      gain *= 0.5 + 0.9 * pulse * pulse;
    }
    var energy = 0;
    if (mode === 'speak') {
      var kk = Math.floor(t / 0.08), u = smooth(0, 1, frac(t / 0.08));
      var e0 = hash(kk * 7919), e1 = hash((kk + 1) * 7919);
      energy = e0 + (e1 - e0) * u;
      scale = 0.85 + 0.35 * energy;
      gain *= 0.6 + 0.7 * energy;
    }
    var shake = 0, shakeY = 0;
    if (mode === 'error') {
      var strobe = Math.sin(TAU * 2 * t);
      gain *= 0.55 + 0.45 * (strobe > 0 ? 1 : 0.2);
      shake = (hash(frame * 3) - 0.5) * 4; shakeY = (hash(frame * 3 + 1) - 0.5) * 4;
    }

    /* Blob centres for this frame. */
    var blobs = [];
    var cxBase = 32, cyBase = mode === 'sleep' ? 44 : 32;
    if (mode === 'think') cxBase = 36;
    for (k = 0; k < cfg.blobs.length; k++) {
      var bcfg = cfg.blobs[k], ph = k * TAU / cfg.blobs.length;
      var ang = t * bcfg[2] + ph, orb = bcfg[1];
      blobs.push({
        x: cxBase + orb * Math.cos(ang) + shake,
        y: cyBase + orb * Math.sin(ang + ph * 1.3) + shakeY,
        s2: 2 * bcfg[0] * scale * bcfg[0] * scale,
        w: bcfg[3]
      });
    }

    /* Think: a chevron ridge drifting through the cloud, its point to the left. */
    var chev = null;
    if (mode === 'think') {
      var vx = 24 + 3 * Math.sin(t * 0.5), vy = 32 + 2 * Math.sin(t * 0.37), arm = 19 + 3 * Math.sin(t * 0.8);
      chev = { vx: vx, vy: vy, ax: vx + arm, ay1: vy - arm * 0.9, ay2: vy + arm * 0.9 };
    }
    var embers = null;
    if (mode === 'think') {
      embers = [];
      for (k = 0; k < 3; k++) {
        var ea = t * 0.9 + k * TAU / 3;
        embers.push({ x: 34 + 16 * Math.cos(ea) * Math.cos(t * 0.3 + k), y: 32 + 14 * Math.sin(ea * 1.3 + k), b: 0.5 + 0.5 * Math.sin(t * 2.1 + k * 2) });
      }
    }
    var EMBER = lin(255, 220, 255), RED = lin(255, 30, 30);

    var tmask = (cfg.text && opts.text) ? textMask(opts.text, 54) : null;
    var ws = cfg.wspeed, wa2 = cfg.warp;

    for (y = 0; y < H; y++) {
      for (x = 0; x < W; x++) {
        var r = 0, g = 0, b = 0, w;

        /* Domain warp: the cloud is sampled through a slowly moving distortion. */
        var wx = x + wa2 * (Math.sin(y * 0.17 + t * 0.5 * ws) + Math.sin((x + y) * 0.12 - t * 0.3 * ws));
        var wy = y + wa2 * (Math.sin(x * 0.15 - t * 0.4 * ws) + Math.cos((x - y) * 0.11 + t * 0.35 * ws));

        /* Density: the blobs, then a mottled texture. */
        var n = 0;
        for (k = 0; k < blobs.length; k++) {
          var bl = blobs[k], dx = wx - bl.x, dy = wy - bl.y;
          n += bl.w * Math.exp(-(dx * dx + dy * dy) / bl.s2);
        }
        if (chev) {
          var dc = Math.min(segDist(wx, wy, chev.vx, chev.vy, chev.ax, chev.ay1), segDist(wx, wy, chev.vx, chev.vy, chev.ax, chev.ay2));
          n += 0.9 * Math.exp(-dc * dc / 24.5);
        }
        n *= gain;

        /* Colour: thin gas in the outer colour, dense light in the core colour. */
        var m = smooth(0.45, 1.7, n), lum = 1 - Math.exp(-n * 1.3);
        r += lum * (outer[0] + (core[0] - outer[0]) * m);
        g += lum * (outer[1] + (core[1] - outer[1]) * m);
        b += lum * (outer[2] + (core[2] - outer[2]) * m);

        /* Stars, in the calm states, only where the cloud is thin. */
        if (cfg.stars > 0 && n < 0.25) {
          for (k = 0; k < STARS.length; k++) {
            var st = STARS[k];
            if (st.x !== x || st.y !== y) continue;
            var tw = Math.max(0, Math.sin(t * st.f + st.p));
            w = cfg.stars * (0.05 + 0.5 * tw * tw * tw * tw * tw * tw) * (1 - n * 4);
            r += w * STAR_COL[0]; g += w * STAR_COL[1]; b += w * STAR_COL[2];
          }
        }

        /* Think embers wandering through the cloud. */
        if (embers) {
          for (k = 0; k < 3; k++) {
            var em = embers[k], qx = x - em.x, qy = y - em.y, qq = qx * qx + qy * qy;
            if (qq > 16) continue;
            w = Math.exp(-qq / 1.4) * em.b;
            r += w * EMBER[0]; g += w * EMBER[1]; b += w * EMBER[2];
          }
        }

        /* Error: a red border pulsing with the cloud. */
        if (mode === 'error') {
          var edge = Math.min(x, y, 63 - x, 63 - y);
          w = (edge === 0 ? 1 : Math.exp(-edge / 2.5) * 0.4) * (0.6 + 0.4 * Math.sin(TAU * 2 * t));
          r += w * RED[0]; g += w * RED[1]; b += w * RED[2];
        }

        /* `t` in warm white with a dark halo so it reads over the glow. */
        if (tmask) {
          var mv = tmask[y * W + x];
          if (mv === 1) { r *= 0.25; g *= 0.25; b *= 0.25; }
          else if (mv === 2) { r = r * 0.2 + TEXT_COL[0] * 0.8; g = g * 0.2 + TEXT_COL[1] * 0.8; b = b * 0.2 + TEXT_COL[2] * 0.8; }
        }

        i = (y * W + x) * 3;
        acc[i] = r * master; acc[i + 1] = g * master; acc[i + 2] = b * master;
      }
    }
    return cfg;
  }

  /* RING look. Fills acc with linear light. */
  function sceneRing(mode, t, frame, opts, acc) {
    var cfg = RCFG[mode] || RCFG.idle, cols = RPAL[mode] || RPAL.idle;
    var x, y, i, k;
    var master = cfg.master, gain = cfg.gain, R = cfg.R, sigma = cfg.sigma, wob = cfg.wob, sp = cfg.speed;
    var cx = cfg.cx, cy = cfg.cy;
    var pulse = 0.5 + 0.5 * Math.sin(TAU * t);
    var spin = [0.15, -0.1, 0.07];                     /* each ribbon slowly turns its own way */

    if (mode === 'idle' || mode === 'alert') R *= 1 + 0.05 * Math.sin(TAU * t / 6);
    if (mode === 'sleep') { R *= 1 + 0.08 * Math.sin(TAU * t / 5); master *= 0.8 + 0.2 * Math.sin(TAU * t / 5); }
    if (mode === 'listen') { R *= 1 + 0.1 * Math.sin(TAU * t); gain *= 0.9 + 0.25 * pulse; }
    if (mode === 'think') { spin = [0.5, 0.35, 0.65]; cx += 2 * Math.sin(t * 0.6); cy += 1.5 * Math.sin(t * 0.45); }
    if (mode === 'alert') {
      cols = RAMBER;                                   /* gold, flashing once a second */
      gain *= 0.5 + 0.9 * pulse * pulse;
    }
    var energy = 0;
    if (mode === 'speak') {
      var kk = Math.floor(t / 0.08), u = smooth(0, 1, frac(t / 0.08));
      var e0 = hash(kk * 7919), e1 = hash((kk + 1) * 7919);
      energy = e0 + (e1 - e0) * u;
      R *= 0.88 + 0.25 * energy; sigma *= 0.7 + 0.7 * energy; gain *= 0.55 + 0.8 * energy;
    }
    if (mode === 'error') {
      gain *= 0.5 + 0.5 * (Math.sin(TAU * 2 * t) > 0 ? 1 : 0.25);
      cx += (hash(frame * 3) - 0.5) * 4; cy += (hash(frame * 3 + 1) - 0.5) * 4;
    }
    var runner = mode === 'work' ? frac(t / 1.2) * TAU : -1;    /* work: an arc racing round the ring */
    var ripple = mode === 'listen' ? (R + 14) * (1 - frac(t)) : -1;   /* listen: a wave drawn in from the edge */
    var s2 = 2 * sigma * sigma;
    var RED = lin(255, 30, 30);

    for (y = 0; y < H; y++) {
      for (x = 0; x < W; x++) {
        var dx = x - cx, dy = y - cy;
        var r = Math.sqrt(dx * dx + dy * dy), th = Math.atan2(dy, dx);
        var cr = 0, cg = 0, cb = 0, total = 0, w, k2;

        for (k = 0; k < 3; k++) {
          var a = th + t * spin[k] * sp + k * 2.1;
          /* The ribbon's radius wanders with three harmonics, its own phase per ribbon. */
          var Rk = R + (k - 1) * 1.6 + wob * (0.7 * Math.sin(2 * a + t * 0.6 * sp - k) + 0.8 * Math.sin(3 * a + t * 0.9 * sp + 0.8 * k) + 0.5 * Math.sin(5 * a - t * 1.3 * sp + k));
          var d = r - Rk;
          w = Math.exp(-d * d / s2);
          /* Silk: brightness varies along the ribbon too. */
          w *= 0.78 + 0.22 * Math.sin((3 + k) * a + t * 0.8 * sp + k * 1.7);
          if (runner >= 0) {
            var da = runner - th; da -= TAU * Math.floor(da / TAU + 0.5);      /* -PI..PI, positive behind the head */
            w *= 0.25 + 1.3 * (da >= 0 ? Math.exp(-da / 1.0) : Math.exp(-da * da / 0.12));
          }
          w *= gain;
          cr += w * cols[k][0]; cg += w * cols[k][1]; cb += w * cols[k][2];
          total += w;
        }
        /* Crossings go white. */
        var hot = smooth(0.9, 1.8, total) * 0.6;
        cr += hot * WHITE[0]; cg += hot * WHITE[1]; cb += hot * WHITE[2];

        if (ripple >= 0) {
          var dr = r - ripple;
          w = Math.exp(-dr * dr / 12) * 0.3 * (1 - ripple / (R + 14)) * smooth(6, 14, ripple) * gain;
          cr += w * cols[1][0]; cg += w * cols[1][1]; cb += w * cols[1][2];
        }

        if (cfg.stars > 0 && total < 0.08) {
          for (k2 = 0; k2 < STARS.length; k2++) {
            var st = STARS[k2];
            if (st.x !== x || st.y !== y) continue;
            var tw = Math.max(0, Math.sin(t * st.f + st.p));
            w = cfg.stars * (0.05 + 0.5 * tw * tw * tw * tw * tw * tw);
            cr += w * STAR_COL[0]; cg += w * STAR_COL[1]; cb += w * STAR_COL[2];
          }
        }

        if (mode === 'error') {
          var edge = Math.min(x, y, 63 - x, 63 - y);
          w = (edge === 0 ? 1 : Math.exp(-edge / 2.5) * 0.4) * (0.6 + 0.4 * Math.sin(TAU * 2 * t));
          cr += w * RED[0]; cg += w * RED[1]; cb += w * RED[2];
        }

        i = (y * W + x) * 3;
        acc[i] = cr * master; acc[i + 1] = cg * master; acc[i + 2] = cb * master;
      }
    }

    /* `t` in the dark centre, warm white with a dark halo. */
    if (cfg.text && opts.text) {
      var tmask = textMask(opts.text, 29);
      for (i = 0; i < W * H; i++) {
        var mv = tmask[i];
        if (!mv) continue;
        var j = i * 3;
        if (mv === 1) { acc[j] *= 0.25; acc[j + 1] *= 0.25; acc[j + 2] *= 0.25; }
        else { acc[j] = acc[j] * 0.2 + TEXT_COL[0] * 0.8 * master; acc[j + 1] = acc[j + 1] * 0.2 + TEXT_COL[1] * 0.8 * master; acc[j + 2] = acc[j + 2] * 0.2 + TEXT_COL[2] * 0.8 * master; }
      }
    }
    return cfg;
  }

  /* NEBULA look: the ring, but made of cloud. Three soft bands at
   * slightly different radii, sampled through a drifting warp so their edges
   * dissolve, a dim glow in the centre, and a low haze of the state's colour
   * over the whole panel so nothing is ever black. Fills acc. */
  function sceneNebula(mode, t, frame, opts, acc) {
    var cfg = RCFG[mode] || RCFG.idle, cols = RPAL[mode] || RPAL.idle;
    var x, y, i, k;
    var master = cfg.master, gain = cfg.gain, R = cfg.R, sigma = cfg.sigma * 1.15, wob = cfg.wob, sp = cfg.speed;
    var cx = cfg.cx, cy = cfg.cy, warp = 1.6;
    var pulse = 0.5 + 0.5 * Math.sin(TAU * t);
    var spin = [0.15, -0.1, 0.07];

    if (mode === 'idle' || mode === 'alert') R *= 1 + 0.05 * Math.sin(TAU * t / 6);
    if (mode === 'sleep') { R *= 1 + 0.08 * Math.sin(TAU * t / 5); master *= 0.8 + 0.2 * Math.sin(TAU * t / 5); }
    if (mode === 'listen') { R *= 1 + 0.1 * Math.sin(TAU * t); gain *= 0.9 + 0.25 * pulse; }
    if (mode === 'think') { spin = [0.5, 0.35, 0.65]; warp = 2.6; cx += 2 * Math.sin(t * 0.6); cy += 1.5 * Math.sin(t * 0.45); }
    if (mode === 'alert') { cols = RAMBER; gain *= 0.5 + 0.9 * pulse * pulse; }
    var energy = 0;
    if (mode === 'speak') {
      var kk = Math.floor(t / 0.08), u = smooth(0, 1, frac(t / 0.08));
      var e0 = hash(kk * 7919), e1 = hash((kk + 1) * 7919);
      energy = e0 + (e1 - e0) * u;
      R *= 0.88 + 0.25 * energy; sigma *= 0.7 + 0.7 * energy; gain *= 0.55 + 0.8 * energy;
    }
    if (mode === 'error') {
      warp = 3.2;
      gain *= 0.5 + 0.5 * (Math.sin(TAU * 2 * t) > 0 ? 1 : 0.25);
      cx += (hash(frame * 3) - 0.5) * 4; cy += (hash(frame * 3 + 1) - 0.5) * 4;
    }
    var runner = mode === 'work' ? frac(t / 1.2) * TAU : -1;
    var ripple = mode === 'listen' ? (R + 14) * (1 - frac(t)) : -1;
    var layers = [[-4, 1.3, 0.3], [0, 1.0, 1.0], [5, 1.3, 0.3]];       /* dR, sigma factor, weight */
    var hazeCol = mix(cols[0], cols[1], 0.5), haze = mode === 'sleep' ? 0.018 : 0.035;
    var RED = lin(255, 30, 30), ws = sp * 0.6;

    for (y = 0; y < H; y++) {
      for (x = 0; x < W; x++) {
        /* The whole scene is sampled through a slow warp, which is what makes it cloud. */
        var wx = x + warp * (Math.sin(y * 0.31 + t * 0.4 * ws) + Math.sin((x + y) * 0.23 - t * 0.25 * ws));
        var wy = y + warp * (Math.sin(x * 0.27 - t * 0.3 * ws) + Math.cos((x - y) * 0.21 + t * 0.28 * ws));
        var dx = wx - cx, dy = wy - cy;
        var r = Math.sqrt(dx * dx + dy * dy), th = Math.atan2(dy, dx);
        var cr = 0, cg = 0, cb = 0, total = 0, w, k2;

        for (k = 0; k < 3; k++) {
          var a = th + t * spin[k] * sp + k * 2.1;
          var Rk = R + layers[k][0] + wob * (0.7 * Math.sin(2 * a + t * 0.6 * sp - k) + 0.8 * Math.sin(3 * a + t * 0.9 * sp + 0.8 * k) + 0.5 * Math.sin(5 * a - t * 1.3 * sp + k));
          var d = r - Rk, sk = sigma * layers[k][1];
          w = Math.exp(-d * d / (2 * sk * sk)) * layers[k][2];
          w *= 0.75 + 0.25 * Math.sin((3 + k) * a + t * 0.8 * sp + k * 1.7);
          /* Cloud: density breaks up along the band. */
          w *= 0.75 + 0.25 * Math.sin(wx * 0.33 + t * 0.5 * ws + k) * Math.sin(wy * 0.29 - t * 0.4 * ws + 2 * k);
          if (runner >= 0) {
            var da = runner - th; da -= TAU * Math.floor(da / TAU + 0.5);
            w *= 0.25 + 1.3 * (da >= 0 ? Math.exp(-da / 1.0) : Math.exp(-da * da / 0.12));
          }
          w *= gain;
          cr += w * cols[k][0]; cg += w * cols[k][1]; cb += w * cols[k][2];
          total += w;
        }
        var hot = smooth(0.9, 1.9, total) * 0.5;
        cr += hot * WHITE[0]; cg += hot * WHITE[1]; cb += hot * WHITE[2];

        /* A dim glow in the centre, and the haze everywhere: brighter near the
         * ring, never gone in the corners, with slow wisps drifting through it. */
        var rr0 = Math.sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        w = 0.06 * Math.exp(-rr0 * rr0 / 162) * gain;
        cr += w * cols[1][0]; cg += w * cols[1][1]; cb += w * cols[1][2];
        var wisp = 0.5 + 0.5 * Math.sin(x * 0.09 + t * 0.15 + Math.sin(y * 0.07 - t * 0.1)) * Math.cos(y * 0.08 - t * 0.12);
        var vig = 0.7 + 0.3 * Math.exp(-rr0 * rr0 / 1800);
        w = haze * (0.8 + 0.4 * wisp) * vig * (0.6 + 0.4 * gain);
        cr += w * hazeCol[0]; cg += w * hazeCol[1]; cb += w * hazeCol[2];

        if (ripple >= 0) {
          var dr = r - ripple;
          w = Math.exp(-dr * dr / 12) * 0.3 * (1 - ripple / (R + 14)) * smooth(6, 14, ripple) * gain;
          cr += w * cols[1][0]; cg += w * cols[1][1]; cb += w * cols[1][2];
        }

        if (cfg.stars > 0 && total < 0.08) {
          for (k2 = 0; k2 < STARS.length; k2++) {
            var st = STARS[k2];
            if (st.x !== x || st.y !== y) continue;
            var tw = Math.max(0, Math.sin(t * st.f + st.p));
            w = cfg.stars * (0.05 + 0.5 * tw * tw * tw * tw * tw * tw);
            cr += w * STAR_COL[0]; cg += w * STAR_COL[1]; cb += w * STAR_COL[2];
          }
        }

        if (mode === 'error') {
          var edge = Math.min(x, y, 63 - x, 63 - y);
          w = (edge === 0 ? 1 : Math.exp(-edge / 2.5) * 0.4) * (0.6 + 0.4 * Math.sin(TAU * 2 * t));
          cr += w * RED[0]; cg += w * RED[1]; cb += w * RED[2];
        }

        i = (y * W + x) * 3;
        acc[i] = cr * master; acc[i + 1] = cg * master; acc[i + 2] = cb * master;
      }
    }

    if (cfg.text && opts.text) {
      var tmask = textMask(opts.text, 29);
      for (i = 0; i < W * H; i++) {
        var mv = tmask[i];
        if (!mv) continue;
        var j = i * 3;
        if (mv === 1) { acc[j] *= 0.25; acc[j + 1] *= 0.25; acc[j + 2] *= 0.25; }
        else { acc[j] = acc[j] * 0.2 + TEXT_COL[0] * 0.8 * master; acc[j + 1] = acc[j + 1] * 0.2 + TEXT_COL[1] * 0.8 * master; acc[j + 2] = acc[j + 2] * 0.2 + TEXT_COL[2] * 0.8 * master; }
      }
    }
    return cfg;
  }

  /* AURA look (the default). A port of the `AgentAudioVisualizerAura` shader
   * from LiveKit's components-js (packages/shadcn), which was developed for
   * Unicorn Studio and is licensed under the Polyform Non-Resale License 1.0.0
   * (https://polyformproject.org/licenses/non-resale/1.0.0/, (c) 2026 UNCRN
   * LLC). Changes here: fixed-function JS instead of GLSL, the panel's linear
   * light pipeline, per-state colours, `t` in the centre, an optional haze,
   * and one parameter vocabulary for every state so states tween.
   *
   * How it works: a circle outline is drawn ITERATIONS times, each time seen
   * through a turbulence warp at a slightly different phase, and the copies
   * are averaged. Where the copies agree the band is solid; where they fan out
   * it dissolves.
   *
   * AURA is the whole vocabulary: the ring's size (scale), the turbulence's
   * pace (speed), amplitude and frequency, a brightness with a pulse on top
   * (depth, rate, sharp: 1 a sine breath, 2 a flash, 3 a beat), and four
   * things that are 0 in idle and fade in where a state uses them: the
   * voice-driven swell and glow (voice), a tremor of the centre, the red
   * frame (border) and `t` (text, set by auraParams). Idle is the reference;
   * every other state is idle with some numbers moved, and listen, think and
   * work keep idle's cyan within a step of hue so motion tells them apart;
   * only alert and error change colour outright. The idle/listen/think/speak
   * geometry is what LiveKit's hook animates to; work, alert, error and sleep
   * are added in the same vocabulary. Every field is a number, so a state
   * change is auraMix() over AURA_TWEEN_S, with the colour (linear RGB, so
   * a fade is a mix of light, not a sweep round the hue wheel) on its own
   * slower clock AURA_FADE_S, and the two phases (turbulence `anim`, pulse)
   * are integrated by the caller from the current pace so a speed change
   * accelerates instead of jumping. aura.c mirrors this table field for
   * field. */
  var AURA_COL = {
    idle: [0x1F, 0xD5, 0xF9], listen: [0x24, 0xF2, 0xBF], think: [0x25, 0x7E, 0xFA], work: [0x5C, 0xE4, 0xFF],
    speak: [0x1F, 0xD5, 0xF9], alert: [0xFF, 0xC8, 0x14], error: [0xFF, 0x3C, 0x3C], sleep: [0x2B, 0x57, 0xD9]
  };
  var AURA_CYAN = [0x1F, 0xD5, 0xF9];
  var AURA = {
    /*        pace      radius       turbulence              brightness   pulse                        extras */
    idle:   { speed: 10, scale: 0.24, amp: 0.9,  freq: 0.4,  bright: 1.0,  depth: 0.0,  rate: 0.15, sharp: 1, voice: 0, tremor: 0, dy: 0,    haze: 1, border: 0 },   /* LiveKit: scale 0.2, amp 1.2; opened up so `t` fits inside */
    listen: { speed: 20, scale: 0.30, amp: 1.0,  freq: 0.7,  bright: 1.5,  depth: 0.5,  rate: 1.43, sharp: 1, voice: 0, tremor: 0, dy: 0,    haze: 1, border: 0 },   /* the hook's 0.7 s pulse as a raised cosine */
    think:  { speed: 30, scale: 0.30, amp: 0.7,  freq: 1.0,  bright: 0.5,  depth: 1.7,  rate: 0.7,  sharp: 1, voice: 0, tremor: 0, dy: 0,    haze: 1, border: 0 },   /* LiveKit: amp 0.5 and off-centre; centred here, a slow deep swell */
    work:   { speed: 40, scale: 0.28, amp: 0.6,  freq: 1.0,  bright: 1.2,  depth: 0.0,  rate: 0.5,  sharp: 1, voice: 0, tremor: 0, dy: 0,    haze: 1, border: 0 },   /* idle's cyan lifted toward white, turning fast, steady */
    speak:  { speed: 25, scale: 0.26, amp: 0.9,  freq: 0.8,  bright: 1.2,  depth: 0.0,  rate: 0.5,  sharp: 1, voice: 1, tremor: 0, dy: 0,    haze: 1, border: 0 },   /* idle's ring a little quicker, swelling and glowing with the voice */
    alert:  { speed: 10, scale: 0.24, amp: 0.9,  freq: 0.4,  bright: 1.0,  depth: 1.5,  rate: 1.0,  sharp: 2, voice: 0, tremor: 0, dy: 0,    haze: 1, border: 0 },   /* idle's ring, gold, flashing once a second */
    error:  { speed: 40, scale: 0.25, amp: 2.0,  freq: 0.8,  bright: 0.6,  depth: 1.6,  rate: 2.0,  sharp: 3, voice: 0, tremor: 1, dy: 0,    haze: 1, border: 1 },   /* torn, beating twice a second, trembling, framed */
    sleep:  { speed: 6,  scale: 0.20, amp: 0.8,  freq: 0.4,  bright: 0.6,  depth: 0.25, rate: 0.2,  sharp: 1, voice: 0, tremor: 0, dy: 0.10, haze: 0, border: 0 }    /* idle's ring smaller, dimmer, slower, settled low, no haze */
  };
  var AURA_FIELDS = ['speed', 'scale', 'amp', 'freq', 'bright', 'depth', 'rate', 'sharp', 'voice', 'tremor', 'dy', 'haze', 'border', 'text', 'r', 'g', 'b', 'hr', 'hg', 'hb'];
  var AURA_BLUR = 0.2, AURA_SPACING = 0.5, AURA_VARIANCE = 0.1, AURA_SMOOTHING = 1.0, AURA_COLOR_SHIFT = 0.05;
  var AURA_HAZE = 0.03, AURA_VOICE = 0.03, AURA_VOICE_GLOW = 0.4, AURA_TREMOR = 0.025, AURA_TWEEN_S = 0.8, AURA_FADE_S = 2.0, AURA_FADE_DIP = 0.25;
  var AURA_SYLLABLE_S = 0.14, AURA_PHRASE_S = 0.7, AURA_WRAP_S = 70;   /* the voice repeats within 70 s, like aura.c's clock */

  function rgb2hsv(r, g, b) {
    var mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn, h = 0;
    if (d > 1e-6) {
      if (mx === r) h = ((g - b) / d) % 6; else if (mx === g) h = (b - r) / d + 2; else h = (r - g) / d + 4;
      h /= 6; if (h < 0) h += 1;
    }
    return [h, mx > 0 ? d / mx : 0, mx];
  }
  function hsv2rgb(h, s, v) {
    var i = Math.floor(h * 6), f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), u = v * (1 - (1 - f) * s);
    switch (i % 6) {
      case 0: return [v, u, p]; case 1: return [q, v, p]; case 2: return [p, v, u];
      case 3: return [p, q, v]; case 4: return [u, p, v]; default: return [v, p, q];
    }
  }

  /* The full parameter set of one state: the AURA row, `text` (1 where the
   * state shows `t`: idle and alert, unless withText says otherwise) and the
   * colour in linear light, so a tween mixes the two colours' light, and the
   * haze as whole driver levels per channel (hr, hg, hb), so a steady state's
   * background is flat and a fade dithers between two levels. */
  function auraParams(mode, withText) {
    var s = AURA[mode] || AURA.idle, c = AURA_COL[mode] || AURA_CYAN;
    var p = {}, k;
    for (k in s) p[k] = s[k];
    p.text = withText == null ? (mode === 'idle' || mode === 'alert' ? 1 : 0) : (withText ? 1 : 0);
    p.r = Math.pow(c[0] / 255, 2.2); p.g = Math.pow(c[1] / 255, 2.2); p.b = Math.pow(c[2] / 255, 2.2);
    p.hr = hazeLevel(p.r); p.hg = hazeLevel(p.g); p.hb = hazeLevel(p.b);
    return p;
  }
  /* The haze one colour channel (linear) makes, as a whole number of the driver's 31 levels. */
  function hazeLevel(lin) { return Math.round(clamp01((AURA_HAZE * lin - 0.006) / 0.994) * 31); }
  /* a + (b - a) * e, field by field, except the colour and the haze, which
   * mix at f (default e): the shape tweens over AURA_TWEEN_S, the colour
   * fades over the slower AURA_FADE_S. The colour dips a little halfway,
   * scaled by how different the two colours are, so a blend of two saturated
   * colours reads as one giving way to the other rather than a flash of
   * white between them, and a change that keeps the colour does not dim. */
  function auraMix(a, b, e, f) {
    var o = {}, i, k;
    if (f == null) f = e;
    var d = Math.min(1, Math.max(Math.abs(b.r - a.r), Math.abs(b.g - a.g), Math.abs(b.b - a.b)));
    var dip = 1 - AURA_FADE_DIP * d * 4 * f * (1 - f);
    for (i = 0; i < AURA_FIELDS.length; i++) {
      k = AURA_FIELDS[i];
      if (k === 'r' || k === 'g' || k === 'b') o[k] = (a[k] + (b[k] - a[k]) * f) * dip;
      else if (k === 'hr' || k === 'hg' || k === 'hb') o[k] = a[k] + (b[k] - a[k]) * f;
      else o[k] = a[k] + (b[k] - a[k]) * e;
    }
    return o;
  }
  function auraEase(u) { u = clamp01(u); return u * u * (3 - 2 * u); }   /* the tween's ease in and out */
  /* p with the light off: what a cold start fades in from. */
  function auraDark(p) { var o = auraMix(p, p, 0); o.bright = 0; o.depth = 0; o.haze = 0; o.border = 0; o.text = 0; return o; }
  /* The phases a fixed state has at time t; a live caller integrates them instead. */
  function auraPhases(p, t) { return { anim: frac(t * 0.05 * p.speed / TAU) * TAU, pulse: frac(t * p.rate) }; }

  /* turb(): the shader's turbulence warp, four layers of rotated sine
   * displacement. pos in the shader's frame (-0.5..0.5); anim is the
   * turbulence phase in radians (animTime upstream). */
  function auraTurb(px, py, anim, it, freq, amp, out) {
    var m00 = 0.6, m10 = -0.25, m01 = 0.25, m11 = 0.9;       /* rotation, column-major like GLSL */
    var frequency = 2 + 13 * freq, amplitude = amp;
    for (var i = 0; i < 4; i++) {
      var rx = px * m00 + py * m10, ry = px * m01 + py * m11;
      var wx = Math.sin(frequency * rx + i * anim + it), wy = Math.sin(frequency * ry + i * anim + it);
      var k = amplitude / frequency;
      px += k * m00 * wx; py += k * m10 * wy;
      /* rotation *= mat2(0.6, -0.8, 0.8, 0.6) */
      var n00 = m00 * 0.6 + m01 * -0.8, n10 = m10 * 0.6 + m11 * -0.8, n01 = m00 * 0.8 + m01 * 0.6, n11 = m10 * 0.8 + m11 * 0.6;
      m00 = n00; m10 = n10; m01 = n01; m11 = n11;
      amplitude *= 1 + (Math.max(wx, wy) - 1) * AURA_VARIANCE;
      frequency *= 1.4;
    }
    out[0] = px; out[1] = py;
  }

  /* One frame from a parameter set p (auraParams or an auraMix of two), the
   * wall time t (voice and tremor run on it) and the phases {anim, pulse}. */
  /* A random level held for `period` seconds and eased into the next one; the
   * sequence repeats every AURA_WRAP_S so a wrapped clock stays continuous. */
  function heldRandom(t, period, salt) {
    var count = Math.round(AURA_WRAP_S / period), k = Math.floor(t / period), u = smooth(0, 1, frac(t / period));
    var e0 = hash((k % count + 1) * salt), e1 = hash(((k + 1) % count + 1) * salt);   /* +1 as in aura.c, whose hash of 0 is 0 */
    return e0 + (e1 - e0) * u;
  }

  /* The voice level, 0..1: syllables under a phrase loudness gated so the
   * quietest stretches are rests. Speech in bursts with pauses, not noise. */
  function auraVoice(t) {
    var syllable = heldRandom(t, AURA_SYLLABLE_S, 7919), phrase = smooth(0.15, 0.85, heldRandom(t, AURA_PHRASE_S, 104729));
    return syllable * phrase;
  }

  function sceneAura(p, t, phases, opts, acc) {
    var N = opts.iterations || 36;
    var x, y, i, k;

    /* The pulse: a raised cosine, sharpened into a flash or a beat by the exponent. */
    var wave = 0.5 - 0.5 * Math.cos(TAU * phases.pulse);
    if (p.sharp !== 1) wave = Math.pow(wave, p.sharp);
    var bright = p.bright + p.depth * wave;

    /* The voice: a level in syllables under a phrase envelope swells the ring and lifts its glow. */
    var scale = p.scale;
    if (p.voice > 0) {
      var level = p.voice * auraVoice(t);
      scale += AURA_VOICE * level;
      bright += AURA_VOICE_GLOW * level;
    }
    /* The tremor: two sines per axis, 7 to 13 Hz, wander the centre. */
    var dx = 0, dy = p.dy;
    if (p.tremor > 0) {
      var tk = p.tremor * AURA_TREMOR;
      dx += tk * (0.6 * Math.sin(TAU * 7 * t) + 0.4 * Math.sin(TAU * 11 * t));
      dy += tk * (0.6 * Math.sin(TAU * 9 * t + 1) + 0.4 * Math.sin(TAU * 13 * t + 2));
    }

    /* The base colour lives in linear light (that is what fades); the copies
     * take it as HSV so the hue can drift a little across them. */
    var hsv = rgb2hsv(Math.pow(p.r, 1 / 2.2), Math.pow(p.g, 1 / 2.2), Math.pow(p.b, 1 / 2.2));
    var cols = [];
    for (i = 1; i <= N; i++) cols.push(hsv2rgb(frac(hsv[0] + (1 - i / N) * AURA_COLOR_SHIFT * 0.3), hsv[1], hsv[2]));
    /* The haze in driver levels; a fraction is dithered between two levels below. */
    var hazeLv = [p.haze * p.hr, p.haze * p.hg, p.haze * p.hb], LEVEL_LIN = 0.994 / 31;
    var borderPulse = p.border * (0.4 + 0.6 * wave);
    var RED = lin(255, 30, 30);
    var spacing = 1 + (TAU - 1) * AURA_SPACING, anim = phases.anim;
    var st = [0, 0], prev = [0, 0];

    for (y = 0; y < H; y++) {
      for (x = 0; x < W; x++) {
        var cx = (x + 0.5) / W - 0.5 + dx, cy = 0.5 - (y + 0.5) / H + dy;
        auraTurb(cx, cy, anim, -1 / N, p.freq, p.amp, prev);
        var ppr = 0, ppg = 0, ppb = 0;
        for (i = 1; i <= N; i++) {
          auraTurb(cx, cy, anim, (i / N) * spacing, p.freq, p.amp, st);
          var d = Math.abs(Math.sqrt(st[0] * st[0] + st[1] * st[1]) - scale);
          var ddx = st[0] - prev[0], ddy = st[1] - prev[1], pd = Math.sqrt(ddx * ddx + ddy * ddy);
          prev[0] = st[0]; prev[1] = st[1];
          var dynamicBlur = Math.pow(2, pd * 2) - 1;
          var edge = AURA_BLUR * 0.05 + Math.max(dynamicBlur * AURA_SMOOTHING, 0.001);
          var ds = smooth(0, edge, d) - 1;                  /* -1 on the outline, 0 away from it */
          var c = cols[i - 1];
          ppr += ds * c[0]; ppg += ds * c[1]; ppb += ds * c[2];
        }
        /* color = Tonemap(-pp * 1.2); Tonemap(x) = 4x / (1 + 4x); then * brightness */
        var r = -ppr / N * 1.2 * 4, g = -ppg / N * 1.2 * 4, b = -ppb / N * 1.2 * 4;
        r = r / (1 + r) * bright; g = g / (1 + g) * bright; b = b / (1 + b) * bright;
        r = clamp01(r); g = clamp01(g); b = clamp01(b);
        /* The shader's output is display colour; the panel wants linear light. */
        var th = (BAYER4[(y & 3) * 4 + (x & 3)] + 0.5) / 16;
        var h0 = Math.floor(hazeLv[0] + 1 - th), h1 = Math.floor(hazeLv[1] + 1 - th), h2 = Math.floor(hazeLv[2] + th);   /* red and green against blue's complement, as aura.c */
        var lr = Math.pow(r, 2.2) + (h0 > 0 ? 0.006 + h0 * LEVEL_LIN : 0);
        var lg = Math.pow(g, 2.2) + (h1 > 0 ? 0.006 + h1 * LEVEL_LIN : 0);
        var lb = Math.pow(b, 2.2) + (h2 > 0 ? 0.006 + h2 * LEVEL_LIN : 0);

        if (borderPulse > 0) {
          var e = Math.min(x, y, 63 - x, 63 - y);
          var bw = (e === 0 ? 1 : Math.exp(-e / 2.5) * 0.4) * borderPulse;
          lr += bw * RED[0]; lg += bw * RED[1]; lb += bw * RED[2];
        }
        k = (y * W + x) * 3;
        acc[k] = lr; acc[k + 1] = lg; acc[k + 2] = lb;
      }
    }

    if (p.text > 0 && opts.text) {
      var tmask = textMask(opts.text, 29), halo = 1 - 0.75 * p.text, keep = 1 - 0.8 * p.text, ink = 0.8 * p.text;
      for (i = 0; i < W * H; i++) {
        var mv = tmask[i];
        if (!mv) continue;
        var j = i * 3;
        if (mv === 1) {
          /* Dim toward the haze floor, not toward black, as aura.c: the halo never goes darker than the background. */
          var hx = i % W, hy = (i - hx) / W, hth = (BAYER4[(hy & 3) * 4 + (hx & 3)] + 0.5) / 16;
          var g0 = Math.floor(hazeLv[0] + 1 - hth), g1 = Math.floor(hazeLv[1] + 1 - hth), g2 = Math.floor(hazeLv[2] + hth);
          var f0 = g0 > 0 ? 0.006 + g0 * LEVEL_LIN : 0, f1 = g1 > 0 ? 0.006 + g1 * LEVEL_LIN : 0, f2 = g2 > 0 ? 0.006 + g2 * LEVEL_LIN : 0;
          acc[j] = f0 + (acc[j] - f0) * halo; acc[j + 1] = f1 + (acc[j + 1] - f1) * halo; acc[j + 2] = f2 + (acc[j + 2] - f2) * halo;
        }
        else { acc[j] = acc[j] * keep + TEXT_COL[0] * ink; acc[j + 1] = acc[j + 1] * keep + TEXT_COL[1] * ink; acc[j + 2] = acc[j + 2] * keep + TEXT_COL[2] * ink; }
      }
    }
    return p;
  }

  /* Render one frame. opts: look ('aura' | 'nebula' | 'ring' | 'cloud'), bits (5), dither
   * (true | 'temporal' | false), text ('14:32'), iterations (36),
   * frame (temporal dither phase), stats ({duty, frames}). For the aura:
   * params (an auraParams() or auraMix() result, default the mode's own) and
   * phases ({anim, pulse}, default the fixed state's at t) let a live caller
   * render a tween between states with integrated phases. */
  function render(mode, t, opts) {
    opts = opts || {};
    var bits = opts.bits || 5, dither = opts.dither !== false;
    var frame = opts.frame != null ? opts.frame : Math.round(t * 40);
    var out = new Uint8Array(W * H * 3);
    var acc = new Float32Array(W * H * 3);
    var x, y, i, k;
    if (opts.look === 'cloud') sceneCloud(mode, t, frame, opts, acc);
    else if (opts.look === 'ring') sceneRing(mode, t, frame, opts, acc);
    else if (opts.look === 'nebula') sceneNebula(mode, t, frame, opts, acc);
    else {
      var params = opts.params || auraParams(mode, opts.withText);
      sceneAura(params, t, opts.phases || auraPhases(params, t), opts, acc);
    }

    /* The driver: quantise linear light to the bit depth, then encode for a monitor. */
    var maxL = (1 << bits) - 1, duty = 0;
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
      var th = 0.5, temporal = dither === 'temporal';
      if (dither) th = (BAYER4[((y + (temporal ? frame & 3 : 0)) & 3) * 4 + ((x + (temporal ? (frame >> 1) & 3 : 0)) & 3)] + 0.5) / 16;
      i = (y * W + x) * 3;
      for (k = 0; k < 3; k++) {
        var v = clamp01((acc[i + k] - 0.006) / 0.994) * maxL;
        var lv = Math.floor(v + 0.5 + (th - 0.5) * smooth(1.0, 2.0, v));
        if (lv > maxL) lv = maxL;
        duty += lv / maxL;
        out[i + k] = Math.round(255 * Math.pow(lv / maxL, 1 / 2.2));
      }
    }
    if (opts.stats) { opts.stats.duty += duty / (W * H * 3); opts.stats.frames += 1; }
    return out;
  }

  var api = { W: W, H: H, STATES: STATES, CFG: CFG, PAL: PAL, render: render,
    AURA: AURA, AURA_COL: AURA_COL, AURA_TWEEN_S: AURA_TWEEN_S, AURA_FADE_S: AURA_FADE_S,
    auraParams: auraParams, auraMix: auraMix, auraEase: auraEase, auraDark: auraDark, auraPhases: auraPhases };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Aurora = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
