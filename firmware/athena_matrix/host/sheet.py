#!/usr/bin/env python3
"""sheet.py - look at the C renderer (main/aura.c) on the Mac.

Builds aura_host (build.sh), plays a scripted sequence of states through the
real aura.c at 40 fps, and writes contact sheets of what the panel would show:
the driver's 5-bit levels with the dither, one 64x64 tile per snapshot, one row
per segment. Also prints, per segment, how much the picture moved between
consecutive frames in driver levels (mean and worst), so a jump that the eye
would catch on the panel shows up as a number.

    python3 host/sheet.py states            # each state held 4 s, a tile per second
    python3 host/sheet.py fades             # idle <-> alert / error / sleep, a tile per 0.25 s
    python3 host/sheet.py session           # the bench's session script, a tile per 0.5 s
    python3 host/sheet.py all               # the three above
    python3 host/sheet.py idle:2 think:3 --every 5    # any sequence, a tile every 5 frames

Options: --out DIR (default host/out), --scale N (tile upscale, default 4),
--text 14:32 (what idle and alert show), --every N (custom sequences),
--no-build (use the existing aura_host). Output: <scenario>.png, <scenario>.csv
(frame,t_s,mode,mad,maxd) and the raw ppm/<scenario>/ frames in DIR.

Look at the PNG with the Read tool; the first tile of every row is the switch
from the row above, still mid-tween. mad is the mean absolute level change per
channel between one frame and the previous (over all 64x64x3 samples), maxd
the largest single change. "switch" is the mad of a segment's first frame: a
tween restarts from what is on the panel, so that frame should move no more
than the state it came from already moves on its own. It is flagged CUT when
it is above CUT_MIN and above the previous segment's largest mad, which is
what a hard cut, a reset, or a haze that blinks looks like. A high max mad
inside a segment is that state's own rhythm (error's beat, alert's flash);
single pixels moving 20+ levels are normal at any bright edge, so maxd is
shown, not judged.
"""
import argparse
import csv
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from png import read_ppm, write_png  # noqa: E402

STATES = ['idle', 'listen', 'think', 'work', 'speak', 'alert', 'error', 'sleep']
# The bench page's "Play a session": the states one Hermes turn produces, then
# an alert, an error and sleep.
SESSION = [('idle', 2.5), ('listen', 2), ('think', 3), ('work', 4), ('think', 2), ('speak', 4), ('idle', 3),
           ('alert', 4), ('idle', 2), ('error', 3), ('idle', 3), ('sleep', 5)]
SCENARIOS = {
    'states': dict(every=40, segs=[(m, 4.0) for m in STATES],
                   note='each state held 4 s, a tile every second; row order idle, listen, think, work, speak, alert, error, sleep'),
    'fades': dict(every=10, segs=[('idle', 3), ('alert', 3), ('idle', 3), ('error', 3), ('idle', 3), ('sleep', 3), ('idle', 3)],
                  note='the colour changes: idle, alert, idle, error, idle, sleep, idle; a tile every 0.25 s'),
    'session': dict(every=20, segs=SESSION,
                    note='the bench session: idle, listen, think, work, think, speak, idle, alert, idle, error, idle, sleep; a tile every 0.5 s'),
}
FPS = 40
CUT_MIN = 0.5     # a switch frame moving more than this (levels per channel) and more than the state before it is a cut
GAP = 8           # pixels between tiles in the sheet
BG = (0x18, 0x18, 0x18)


def build():
    subprocess.run(['bash', os.path.join(HERE, 'build.sh')], check=True)


def run_host(segs, every, out_dir, text):
    args = [os.path.join(HERE, 'aura_host'), '--fps', str(FPS), '--every', str(every), '--out', out_dir, '--text', text]
    args += [f'{m}:{s:g}' for m, s in segs]
    res = subprocess.run(args, check=True, capture_output=True, text=True)
    rows = []
    for line in res.stdout.splitlines():
        k, t, mode, mad, maxd = line.split(',')
        rows.append((int(k), float(t), mode, float(mad), int(maxd)))
    return rows


def segments(segs):
    """(mode, first frame, frame count) per segment, as aura_host counts them."""
    out, k = [], 0
    for mode, seconds in segs:
        n = int(round(seconds * FPS))
        out.append((mode, k, n))
        k += n
    return out


def compose_rows(rows, scale):
    """rows: list of lists of (w, h, pixels). One sheet row per list."""
    w, h, _ = rows[0][0]
    tw, th = w * scale, h * scale
    cols = max(len(r) for r in rows)
    W = GAP + cols * (tw + GAP)
    H = GAP + len(rows) * (th + GAP)
    buf = bytearray(bytes(BG) * (W * H))
    for r, row in enumerate(rows):
        for c, (fw, fh, px) in enumerate(row):
            ox, oy = GAP + c * (tw + GAP), GAP + r * (th + GAP)
            for sy in range(fh):
                src = sy * fw * 3
                line = bytearray()
                for sx in range(fw):
                    line += px[src + sx * 3:src + sx * 3 + 3] * scale
                for dy in range(scale):
                    o = ((oy + sy * scale + dy) * W + ox) * 3
                    buf[o:o + tw * 3] = line
    return W, H, bytes(buf)


def sheet(name, segs, every, out_dir, scale, text, build_first=True):
    ppm_dir = os.path.join(out_dir, 'ppm', name)
    os.makedirs(ppm_dir, exist_ok=True)
    for f in os.listdir(ppm_dir):
        os.remove(os.path.join(ppm_dir, f))
    if build_first:
        build()
    rows_csv = run_host(segs, every, ppm_dir, text)
    with open(os.path.join(out_dir, f'{name}.csv'), 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['frame', 't_s', 'mode', 'mad', 'maxd'])
        w.writerows(rows_csv)

    tiles = []
    for mode, k0, n in segments(segs):
        ks = [k for k in range(k0, k0 + n) if k == k0 or k % every == 0]
        tiles.append([read_ppm(os.path.join(ppm_dir, f'f{k:05d}_{mode}.ppm')) for k in ks])
    W, H, rgb = compose_rows(tiles, scale)
    png_path = os.path.join(out_dir, f'{name}.png')
    write_png(png_path, W, H, rgb)

    print(f'{name}: {png_path} ({W}x{H}, {sum(len(r) for r in tiles)} tiles)')
    print(f'  {SCENARIOS[name]["note"] if name in SCENARIOS else "custom sequence"}')
    print(f'  {"segment":<8} {"seconds":>7} {"switch":>7} {"mean mad":>9} {"max mad":>8} {"at":>8} {"max maxd":>9}')
    cuts, prev_max = [], None
    for mode, k0, n in segments(segs):
        seg = rows_csv[k0:k0 + n]
        switch = seg[0][3]
        mad = sum(r[3] for r in seg) / len(seg)
        top = max(seg, key=lambda r: r[3])
        maxd = max(r[4] for r in seg)
        cut = prev_max is not None and switch > CUT_MIN and switch > prev_max
        if cut:
            cuts.append((mode, seg[0][1], switch, prev_max))
        print(f'  {mode:<8} {n / FPS:>7.2f} {switch:>7.3f} {mad:>9.3f} {top[3]:>8.3f} {top[1]:>8.3f} {maxd:>9d}{"   CUT" if cut else ""}')
        prev_max = max(r[3] for r in seg[1:]) if len(seg) > 1 else switch
    if cuts:
        print('  CUT: ' + '; '.join(f'{m} at t={t:.3f} moved {v:.2f} levels/channel on its first frame, the state before it never more than {pm:.2f}' for m, t, v, pm in cuts)
              + ' (a tween should not do that; look at that tile)')
    else:
        print(f'  no cuts: no switch frame moved both more than {CUT_MIN} levels/channel and more than the state before it')

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('what', nargs='+', help='states | fades | session | all | mode:seconds ...')
    ap.add_argument('--out', default=os.path.join(HERE, 'out'))
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--text', default='14:32')
    ap.add_argument('--every', type=int, default=10, help='frames between tiles for a custom sequence')
    ap.add_argument('--no-build', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    names = a.what
    if names == ['all']:
        names = list(SCENARIOS)
    custom = [w for w in names if ':' in w]
    if custom:
        segs = []
        for w in custom:
            m, s = w.split(':', 1)
            if m not in STATES:
                sys.exit(f'sheet.py: unknown state {m!r}; one of {", ".join(STATES)}')
            segs.append((m, float(s)))
        sheet('custom', segs, a.every, a.out, a.scale, a.text, not a.no_build)
        return
    built = a.no_build
    for name in names:
        if name not in SCENARIOS:
            sys.exit(f'sheet.py: unknown scenario {name!r}; one of {", ".join(SCENARIOS)}, all, or mode:seconds')
        sc = SCENARIOS[name]
        sheet(name, sc['segs'], sc['every'], a.out, a.scale, a.text, not built)
        built = True


if __name__ == '__main__':
    main()
