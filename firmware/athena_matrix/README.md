# athena_matrix — Waveshare RGB-Matrix-P3-64x64 on an ESP32-WROOM-32 or ESP32-S3

Pure ESP-IDF firmware for the Waveshare 64×64 P3 HUB75E panel, written from
scratch: no Arduino, no third-party matrix library. The classic ESP32 has no
LCD_CAM peripheral, so the panel is refreshed by a tight GPIO loop on core 1
(binary code modulation, 5 bit planes per colour, roughly 100–150 Hz). The same
loop also runs on the ESP32-S3-DevKitC-1 as the bring-up path until the DMA
driver from `RGB-MATRIX.md` is wired in. `main/` is the Athena face: `serial.c`
reads one JSON line per state from UART0 (the USB bridge), `face.c` draws the
current state at 40 fps, `protocol.h` names the states and their fallback
times. The protocol is in `RGB-MATRIX.md`; the Mac side is `scripts/face.py`
and the `athena-face` Hermes plugin.

Everything about *how* to build, flash and watch the board lives in the global
`esp-idf` Claude Code skill. This file holds what is specific to this project:
the wiring and what the panel should show.

## What you need

| Part | Notes |
|---|---|
| Waveshare RGB-Matrix-P3-64x64 | HUB75E, 1/32 scan, 5 V, up to 4 A at full white |
| ESP32-WROOM-32 dev board | 38-pin DevKitC or 30-pin "DevKit V1". Not a WROVER (GPIO16/17 are its PSRAM) |
| or ESP32-S3-DevKitC-1 (N16R8) | the board `RGB-MATRIX.md` is designed for; wire it per that file's J1 table, not the WROOM map below |
| 16-pin ribbon (ships with the panel) plus 15 female-to-male Dupont wires, or a 2×8 IDC breakout | the ribbon goes on the panel's **IN** header |
| 5 V supply, **4 A or more**, on the panel's 4-pin VH power lead | the ESP32's 5V pin can feed it only for a short wiring check at brightness 12, see Power and order |
| Micro-USB cable to the WROOM DevKit, or USB-C to the S3's **UART** connector | powers the board and carries the log |

## Wiring

The panel's back has two 2×8 headers marked with arrows. Plug the ribbon into
**IN** (arrow pointing into the panel). Looking at the IN header as drawn in the
Waveshare figure, the 16 positions are:

```
   R1  o o  G1
   B1  o o  GND
   R2  o o  G2
   B2  o o  E
   A   o o  B
   C   o o  D
   CLK o o  LAT
   OE  o o  GND
```

Waveshare numbers the ribbon wires 16 → 1 from R1 down to the last GND; the
usual HUB75 numbering runs 1 → 16 the other way. Both are in the table so you
can follow either. Colours are for the rainbow ribbon in the Waveshare picture;
they repeat between the two halves (blue, green, yellow, orange, red and brown
each appear twice), so count from the marked edge wire rather than matching a
colour. If your ribbon differs, count only.

| HUB75 pin | Waveshare wire | Ribbon colour | Signal | ESP32 GPIO | Where on the 38-pin DevKitC |
|---|---|---|---|---|---|
| 1 | 16 | brown | R1 | **23** | right header, 2nd pin |
| 2 | 15 | red | G1 | **22** | right header, 3rd pin |
| 3 | 14 | orange | B1 | **21** | right header, 6th pin (after TX, RX) |
| 4 | 13 | yellow | GND | **GND** | right header, 7th pin |
| 5 | 12 | green | R2 | **19** | right header, 8th pin |
| 6 | 11 | blue | G2 | **18** | right header, 9th pin |
| 7 | 10 | purple | B2 | **5** | right header, 10th pin |
| 8 | 9 | grey | E | **13** | left header, 15th pin (after 12 and a GND) |
| 9 | 8 | white | A | **25** | left header, 9th pin |
| 10 | 7 | black | B | **26** | left header, 10th pin |
| 11 | 6 | brown | C | **27** | left header, 11th pin |
| 12 | 5 | red | D | **14** | left header, 12th pin |
| 13 | 4 | orange | CLK | **17** | right header, 11th pin |
| 14 | 3 | yellow | LAT | **16** | right header, 12th pin |
| 15 | 2 | green | OE | **4** | right header, 13th pin |
| 16 | 1 | blue | GND | **GND** | any GND |

So the whole data side sits on the right header in one run
(`23 22 · · 21 GND 19 18 5 17 16 4`) and the five address lines sit on the left
header (`25 26 27 14 · · 13`: skip GPIO12 **and the GND after it**, E goes on
13). Pin positions count from the top of each header on the 38-pin DevKitC V4
with the USB connector at the bottom; trust the silkscreen label over the
count. On the 30-pin DevKit V1 the pins are labelled `D23`, `D22`, … with the
same GPIO numbers. The map is
`main/board_pins.h`; change it there if you wire differently. All 14 pins must
stay in GPIO 0..31.

Waveshare's ESP32-S3 wiring diagram on the panel's ESP-IDF page does not apply
to this board. It is drawn for the S3 (`RGB-MATRIX.md` covers why it is not our
S3 map either), and on a classic ESP32 the GPIO 6, 7 and 8 it uses are the SPI
flash lines: wire the panel there and the chip does not boot.

On the ESP32-S3 the map is different and `board_pins.h` picks it by target:
`R1=4 G1=5 B1=6 R2=7 G2=15 B2=16 A=17 B=18 C=8 D=9 E=10 CLK=11 LAT=12 OE=13`,
the J1 header layout from `RGB-MATRIX.md` (that file has the J1 pin numbers and
the reasons each other pin was skipped). It is the same map the DMA driver will
use, so the wiring does not change when the driver does.

**E is not optional.** A 64×64 panel is 1/32 scan; without E only half the rows
address correctly.

### Power and order

1. Ribbon on the panel's IN header, the other end to the ESP32 as above.
2. **One ground.** Panel GND (either ribbon GND), power-supply GND and an ESP32
   GND pin must be joined, or the data lines have no reference and the panel
   shows noise.
3. Panel 5 V supply on.
4. Then the ESP32's USB.
5. Never plug or unplug the ribbon with the panel powered.

The firmware boots at brightness 12 of 255. That is low enough to run the
wiring check with the panel fed from the DevKit's 5V pin over USB (about
0.1 A for the test pattern on top of the panel's own logic; the USB port gives
0.5 A and the DevKit's diode about 1 A, and the ESP32 browns out and reboots
when the rail sags). Full white at 255 is where the 4 A goes; raise it
(`scripts/face.py --brightness N`, or `ATHENA_FACE_BRIGHTNESS` for the plugin)
only on a supply that can deliver that. A tell-tale of the rail sagging on a
USB-fed panel: after a reset the bridge re-enumerates and the Mac cannot
configure the port any more (`stty: tcsetattr: Invalid argument`, pyserial and
`face.py` fail the same way). Unplug the USB cable for a few seconds and plug
it back; the firmware already on the board is unaffected.

The ESP32 drives 3.3 V into the panel's 5 V logic. That works over a short
ribbon (under 30 cm). If you see ghosting or flicker, shorten the ribbon first;
a 74HCT245 between the ESP32 and the ribbon is the fix if that is not enough.
Before the firmware runs, OE floats and the panel may flash garbage for a
moment; a 10 kΩ pull-up from GPIO4 to 3.3 V keeps it dark during boot.

## Build, flash, watch

```bash
. ~/esp/esp-idf/export.sh >/dev/null
cd firmware/athena_matrix
idf.py set-target esp32              # first time only; esp32s3 for the S3 DevKitC-1
idf.py build
idf.py -p /dev/cu.usbserial-XXXXXXXX flash
idf.py -p /dev/cu.usbserial-XXXXXXXX monitor    # leave with Ctrl+]
```

Boot log to expect:

```
I (xxx) hub75: 64x64 1/32 scan, 5 bit planes, brightness 12, refresh on core 1
I (xxx) hub75: R1=23 G1=22 B1=21 R2=19 G2=18 B2=5 A=25 B=26 C=27 D=14 E=13 CLK=17 LAT=16 OE=4   (the S3 map on an S3)
I (xxx) serial: UART0 115200 8N1, lines up to 256 bytes
I (xxx) athena_matrix: ready: boot mode test, modes: idle listen think work speak alert error sleep test off
```

From then on every line you send is answered with `ok` or `err <reason>`, and
an applied state is logged as `I (xxx) face: mode think ttl 120`. From the
monitor, type `{"mode":"think"}` and Enter; from the Mac, `scripts/face.py think`.

## What the panel shows

The board boots into the wiring test and stays there until the first command,
so a fresh panel can be checked with nothing but power and USB. The eight agent
states are one picture, the aura (`main/aura.c`): a circle outline seen through
a turbulence warp, drawn as a soft ring in the state's colour over a faint haze
of the same colour, moving at the state's pace.

| State | Correct result |
|---|---|
| test (boot) | red top-left, green top-right, blue bottom-left, white bottom-right, thin white border. A swapped colour line shows as the wrong colour in a quadrant; a missing E as a wrong bottom half; a wrong A..D as scrambled rows |
| idle | a cyan ring breathing slowly, the time in its centre if the Mac sent one |
| listen | a green ring, a little larger and quicker, pulsing brighter on a 0.7 s beat |
| think | a violet ring shifted up and right, swirling faster, pulsing from dim to bright |
| work | an amber ring turning fast at a steady bright level |
| speak | a cyan ring whose size jumps with a voice level every 80 ms |
| alert | a gold ring flashing once a second, the time in its centre if the Mac sent one |
| error | a red ring strobing twice a second, shaking and torn by heavy turbulence, inside a red border; gone after 10 s |
| sleep | a dim indigo ring, small and low in the panel, drifting slowly, no haze |

`scripts/face.py --demo` walks through all of them, 4 s each.

## Troubleshooting

| Symptom | Look at |
|---|---|
| Nothing lights | panel supply; ribbon on IN not OUT; OE wire (GPIO4); ribbon plugged one pin off |
| Top half right, bottom half wrong or mirrored | E wire (HUB75 pin 8 → GPIO13) |
| Quadrant has the wrong colour | that colour line; top half is R1 G1 B1, bottom half R2 G2 B2 |
| Rows scrambled or repeated | A–D wires, one per address bit |
| Panel dark although wiring is right | FM6126A driver chip: set `cfg.driver = HUB75_DRIVER_FM6126A` in `main.c` |
| Faint ghost of the row above | ribbon too long; try a 74HCT245 |
| Flicker | check the log: refresh under 100 Hz means the loop is starved; drop `color_depth` to 4 |
| ESP32 resets when the panel goes bright | supply sag or a missing common ground |
| `Resource busy` on the port | another monitor holds it; close it |

## Design notes

- `components/hub75/` is the driver. `hub75_init()` validates the pins, builds
  the colour and address lookup tables, allocates the buffers and starts the
  refresh task. `hub75_present()` packs the RGB888 back buffer into bit planes
  and blocks until the refresh loop has swapped to it.
- The refresh loop writes the whole `GPIO_OUT` register for each pixel clock
  (data with CLK low, then CLK high). That is why every pin must be in bank 0
  and why no other code may drive a GPIO in 0..31 while the panel runs.
- Plane *p* is shown for `64 << p` clocks, re-shifting the same 64 columns;
  the shift register is one row wide so only the last pass matters, and the
  weights come out as exact powers of two with no timer.
- Brightness is the fraction of each window with OE low, so it does not cost
  colour depth.
- The refresh task never blocks, so `sdkconfig.defaults` turns off the idle-task
  watchdog check for core 1.
- The ESP32-S3 build uses the driver unchanged with a pin map that keeps every
  pin below 32; `sdkconfig.defaults.esp32s3` adds the 16 MB flash and octal
  PSRAM keys. The DMA driver in `RGB-MATRIX.md` is still the plan for the S3;
  this is the bring-up path.
