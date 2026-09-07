# host/ — the face renderer on the Mac

Everything here runs on the Mac, needs no board, and exists so a change to the
LED face is looked at before a firmware build. `cc` (Xcode command line tools),
`node` and `python3` are all it takes; nothing is installed. Outputs go to
`host/out/` (gitignored), the built binary is `host/aura_host` (gitignored).

The face has two renderers that are one design in two languages:
`reference/aurora.js` (the source of every number, what the bench page runs)
and `main/aura.c` (the port that runs on the board). The tools below make the
C one visible, check the two against each other, and rebuild the bench page.

| Tool | What it does |
|---|---|
| `build.sh` | Compiles the real `main/aura.c` with clang against `stubs/` (the ESP-IDF headers) and `hub75_stub.c` (captures the frame instead of bit-banging GPIOs) into `aura_host`. Under a second. |
| `aura_host` | Plays a scripted sequence of states through `aura.c` at 40 fps: `aura_host --out DIR --every 10 --text 14:32 idle:3 think:2`. One CSV line per frame on stdout (`frame,t_s,mode,mad,maxd`), a `.ppm` snapshot every `--every` frames and always the first and last of a segment. `--every 0` writes only those. Built from `harness.c`; when `aura_draw()`'s signature changes, `harness.c` follows. |
| `sheet.py` | The tool to reach for. Builds, runs `aura_host` for a scenario, composes the snapshots into a contact sheet (one row per segment, 5-bit levels with the dither, exactly what the panel shows) and prints a per-segment motion summary. `python3 host/sheet.py states`, `fades`, `session`, `all`, or `idle:2 think:3 --every 5`. |
| `parity.mjs` | Does `aura.c` draw what `aurora.js` draws? Runs both at the same instants with the board's shader shape (copies, grid, layers read from `aura.c`'s `#define`s) and compares in driver levels. `node host/parity.mjs`; exit 1 when out of band. |
| `sheet.mjs` | The same 8-state contact sheet from the JS alone (defaults to the board's shader shape; `--grid 64 --layers 4 --iterations 36` is the shader as LiveKit runs it). The quick look while tuning the `AURA` table before the C port is touched. |
| `png.py` | PPM frames into one PNG, used by `sheet.py`; also a CLI. |
| `bench/make_bench.py` | Builds the live bench page from `bench/bench.template.html` with `aurora.js` inlined (`--out FILE`), or says whether a built page is current (`--check FILE`). The page is then published to the *existing* Aura Bench artifact, never as a new one. |

## The loop

1. Change the `AURA` table (or the shader) in `reference/aurora.js`.
2. `node host/sheet.mjs` and look at `host/out/js-states.png`.
3. Mirror the change into `main/aura.c`, field for field.
4. `python3 host/sheet.py all` and look at `states.png`, `fades.png`, `session.png`; read the summary.
5. `node host/parity.mjs`; it must say `ok`.
6. `idf.py build` (the esp-idf skill), and flash only when asked.
7. `python3 host/bench/make_bench.py --out <file>` and republish the bench.

## Reading the numbers

`sheet.py` prints, per segment: `switch` (how much the segment's first frame
moved from the frame before it), `mean mad` and `max mad` (mean absolute level
change per channel between consecutive frames, averaged over all 64×64×3
samples, and its worst frame), and `max maxd` (the largest single-sample
change). A tween restarts from what is on the panel, so a switch frame should
move no more than the state it came from already moves by itself; when it moves
more, and more than 0.5 levels per channel, the line says `CUT`. That is what
a hard cut, a reset, or a haze that blinks between levels looks like. A high
`max mad` inside a segment is that state's own rhythm: error's beat reaches 2 to
3, alert's flash about 0.6, idle on its own stays under 0.15. The first 0.8 s
of a segment is the tween out of the previous state and carries its rhythm
with it, so idle entered from error peaks near 1.5 before settling. Single
samples moving 20 or more levels are normal wherever a bright edge crosses a
pixel and in error's tremor, so `maxd` is shown, not judged.

`parity.mjs` prints, per state at 0.475 s (mid-tween from dark) and 2.975 s
(settled), the share of samples that match exactly, that differ by 1, by 2,
and by 3 or more, the largest difference, and the mean level of each side.
With the board's shader shape the two renderers agree to within a level or
two: 90 to 99.8 % exact, nothing 3 or more apart, max 2. The band (at most
0.5 % of samples 3 or more apart, max 4) is set just outside that; off-by-one
comes from the C's table sines, fast square root and interpolated gamma. A
whole region off, or a mean level off by more than a few percent, is a real
divergence: a table row that was changed in one file only, a tween or phase
that is integrated differently, a colour converted in a different space.

## What the panel cannot show

The driver has 32 levels per channel, so the preview quantises with a Bayer
dither and the sheets show the banding the LEDs will show. Things learned the
hard way, all visible in these sheets before they reach the board:

- The haze sits at driver level 1 or 2; it can only fade by dithering between
  two whole levels, which is why haze is kept in levels per channel and mixed
  as such. A haze that is computed in linear light and quantised blinks.
- Black beside a lit pixel reads as a hole. Anything that dims (the halo
  around `t`) dims toward the haze, never to zero.
- A colour mix in linear light passes through a near-white halfway; the fade
  dips brightness a little at its midpoint so one colour gives way to the next.
- Sleep is dim enough that only its densest part survives the quantiser; it
  needs more base brightness than its look suggests.
- `heldRandom`'s index 0 hashes to exactly 0 in the C, so the voice went silent
  at every 70 s wrap until the index was offset; both files carry the offset.
