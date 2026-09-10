# athena_matrix — Waveshare RGB-Matrix-P3-64x64 on an ESP32-WROOM-32 or ESP32-S3

Pure ESP-IDF firmware for the Waveshare 64×64 P3 HUB75E panel, written from
scratch: no Arduino, no third-party matrix library. The classic ESP32 has no
LCD_CAM peripheral, so the panel is refreshed by a tight GPIO loop on core 1
(binary code modulation, 5 bit planes per colour, roughly 100–150 Hz). The same
loop also runs on the ESP32-S3-DevKitC-1 as the bring-up path until the DMA
driver from `RGB-MATRIX.md` is wired in. `main/` is the Athena face: `serial.c`
reads one JSON line per state from UART0 (the USB bridge) and `net.c` the same
lines from TCP port 7075 over Wi-Fi (the board answers as `athena-matrix.local`;
the network's name and password come from `../components/athena_common/include/athena_secrets.h`,
see Build), `command.c` validates them, `face.c` draws the current state at
40 fps, `protocol.h` names the states and their fallback times. The protocol
is in `RGB-MATRIX.md`; the Mac side is `scripts/face.py` and the `athena-face`
Hermes plugin.

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
| Micro-USB cable to the WROOM DevKit, or USB-C to the S3's **UART** connector | powers the board, carries the boot log, flashes it, and is the fallback command channel; with Wi-Fi up, commands normally arrive over the network instead |
| A 2.4 GHz Wi-Fi network | its name and password go in `firmware/components/athena_common/include/athena_secrets.h` (see Build); with the wrong ones the face still works over USB |

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
cp ../components/athena_common/include/athena_secrets.h.example ../components/athena_common/include/athena_secrets.h   # first time only; then fill in the Wi-Fi name and password
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
I (xxx) wifi: station athena-matrix, mdns athena-matrix.local, service _athena-face
I (xxx) net: listening on tcp port 7075, up to 4 clients
I (xxx) audio: listening on tcp port 7076: pcm s16le 16000 Hz mono, mic out and speaker in on one connection
I (xxx) audio: i2s bclk 32 ws 33 dout 15 din 34, 16000 Hz, 32-bit slots, mic slot L, free heap 96540
I (xxx) athena_matrix: ready: boot mode test, modes: idle listen think work speak alert error sleep test off
I (xxx) wifi: got ip 10.10.20.93 on <your network>, reachable as athena-matrix.local      (a few seconds later)
```

From then on every line you send is answered with `ok` or `err <reason>`, and
an applied state is logged as `I (xxx) face: mode think ttl 120`. From the
monitor, type `{"mode":"think"}` and Enter; from the Mac, `scripts/face.py think`,
which takes the board on USB if one is plugged in and `athena-matrix.local`
otherwise (`--host athena-matrix.local` to insist on Wi-Fi; `nc athena-matrix.local 7075`
to type lines by hand).

## What the panel shows

The board boots into the wiring test and stays there until the first command,
so a fresh panel can be checked with nothing but power and USB. The eight agent
states are one picture, the aura (`main/aura.c`): a circle outline seen through
a turbulence warp, drawn as a soft ring in the state's colour over a faint haze
of the same colour, moving at the state's pace. The palette stays close: listen,
think and work keep idle's cyan within a step of hue, so motion is what tells
them apart, and only alert and error change colour outright. A state change
never cuts: the ring eases from one state's numbers to the next over 0.8 s,
the colour and the haze behind the ring fade more slowly over 2 s, and `t` fades with it.

| State | Correct result |
|---|---|
| test (boot) | red top-left, green top-right, blue bottom-left, white bottom-right, thin white border. A swapped colour line shows as the wrong colour in a quadrant; a missing E as a wrong bottom half; a wrong A..D as scrambled rows |
| idle | a cyan ring breathing slowly, the time in its centre if the Mac sent one |
| listen | a mint ring, idle's hue a step toward green, a little larger and quicker, a quick shallow pulse on a 0.7 s beat |
| think | an azure ring, idle's hue a step toward blue, swirling faster, swelling slowly from dim to bright |
| work | an ice ring, idle's cyan lifted toward white, turning fast at a steady bright level |
| speak | a cyan ring, idle's a little quicker, swelling and glowing with a voice level that comes in syllables and phrases |
| alert | a gold ring flashing once a second, the time in its centre if the Mac sent one |
| error | a red ring torn by heavy turbulence, beating twice a second, trembling, in a pulsing red frame |
| sleep | a small dim deep-blue ring, low in the panel, drifting slowly, no haze |

`scripts/face.py --demo` walks through all of them, 4 s each.

## Microphone and speaker

The same board is also the ears and the mouth: one I2S port in full duplex
carries an INMP441 or ICS-43434 microphone in and a MAX98357A amplifier out,
and `main/audio.c` bridges both to TCP port 7076 as raw PCM. No wake word and
no echo cancellation run here (the classic ESP32 has neither PSRAM nor a free
core for ESP-SR): the Mac does that, this board only moves the sound.
`AUDIO-BOARD.md` is the separate S3 design that does more on the board.

### Wiring

The pins are the four the panel map above leaves free, so the same wiring
works with or without the matrix attached.

| ESP32 pin | MAX98357A | INMP441 / ICS-43434 | Note |
|---|---|---|---|
| GPIO32 | BCLK | SCK | one shared bit clock |
| GPIO33 | LRC | WS | one shared word select |
| GPIO15 | DIN | | audio out to the amp |
| GPIO34 | | SD | audio in from the mic; an input-only pin is fine here |
| 3V3 | SD | VDD | amp SD high = on, left channel; the mic is 3.3 V only, 5 V kills it |
| 5V / VIN | VIN | | amp power from the 5 V rail, never from the 3V3 regulator |
| GND | GND | GND and L/R | one common ground; L/R to GND puts the mic in the left slot |

On the S3 build the four I2S pins are GPIO1 (BCLK), GPIO2 (WS), GPIO14 (DIN
to the amp) and GPIO21 (SD from the mic), untested.

- **Speaker**: both wires on the amp's screw terminal and nowhere else. The output is bridge-tied, so grounding either wire shorts the amp. 4 Ω or 8 Ω, 3 W.
- **Amp SD pin** tied to 3V3 means always on, left channel, full level for a mono stream. Left floating, the breakout's own pull-up selects the stereo average and you get half the level (the firmware puts the same sample in both slots, so it still plays). No GPIO is free on the WROOM once the panel is wired, and the refresh loop would clobber a software-driven output in bank 0 anyway, so mute is done by streaming silence.
- **GAIN** unconnected is 9 dB; to GND 12 dB; to VIN 6 dB; through 100 kΩ to GND 15 dB, to VIN 3 dB.
- **1000 µF 16 V** across the amp's VIN and GND, stripe leg on GND, within a few centimetres of the amp: the class-D bursts otherwise dip the 5 V rail and reset the ESP32.
- **Second mic**: same SCK, WS and SD wires, its L/R to 3V3; both mics tri-state SD outside their own slot. `AUDIO_MIC_SLOT` in `main/audio.c` picks the one that is streamed.
- Keep the mic wires under 15 cm, twisted, away from the speaker leads and the panel ribbon. Wire with USB unplugged and check the mic's VDD is on 3V3 twice.

### The stream

Port 7076, one TCP connection, raw PCM both ways: signed 16-bit
little-endian, 16 kHz, mono, no framing. From the moment a client connects
the board sends the microphone; every byte the client writes is played,
silence when nothing arrives. One client at a time: a second connection is
closed at once. The microphone never blocks or drops the client: whatever
the client cannot take right now, because the link stalls or it is busy
sending playback, is lost and counted on the 5 s log line rather than
closing the connection. A player should still drain what it receives so its
own socket buffer does not fill. The face protocol on 7075 is untouched and both ports work at the same time.
Audio starts only once Wi-Fi is up; without a network there is no audio and
the face still works over the cable.

On the I2S bus the mic delivers 24 bits MSB-aligned in 32-bit slots. The
firmware takes the left slot (`AUDIO_MIC_SLOT`), shifts it right by 14 bits
(`AUDIO_MIC_SHIFT`: 12 dB of gain over the raw top 16 bits) and clamps.
Playback puts each int16 into the top of both 32-bit slots.

Every 5 s the log shows both slots, which is how a wrong L/R or a dead wire
shows up:

```
I (xxx) audio: mic L -42 dBFS (peak -30), R -96 dBFS (peak -96), slot L -> 10.10.20.5:51234
```

With nothing on GPIO34 the left slot reads floating-pin noise near full
scale. With a mic in a quiet room expect about -50 dBFS, speech -30 to -20.

### From the Mac

`scripts/audio.py` (standard library only, like `face.py`) is the bench
client. `--host` (`host` or `host:port`) and `ATHENA_MATRIX_HOST` (its host
part only, a `:7075` there is the face port) pick the board as for `face.py`,
`--port` the audio port:

```bash
scripts/audio.py meter 5             # live mic level bar for 5 s
scripts/audio.py record 5 take.wav   # 5 s of the mic into a 16 kHz mono wav
scripts/audio.py tone 440 2          # 2 s of 440 Hz through the speaker
scripts/audio.py play take.wav       # any 16-bit wav; stereo is averaged, other rates resampled
```

### If the sound does not work

| Symptom | Look at |
|---|---|
| Log shows -96 dBFS in both slots | the mic has no clock or no power: SCK, WS, VDD wires |
| Mic near full scale with nothing said | SD floating: GPIO34 to the mic's SD; or the streamed slot is the unwired one, flip L/R or `AUDIO_MIC_SLOT` |
| Level in the log on R, the stream silent | the mic sits in the right slot: L/R is on 3V3, or the breakout labels it the other way; move L/R to GND or set `AUDIO_MIC_SLOT` to 1 |
| Board resets when sound plays | the 5 V supply sags: the 1000 µF cap, GAIN to VIN, the amp on the 5 V bus instead of the laptop's USB |
| Hiss that follows the aura | supply or coupling from the panel: shorter mic wires, away from the ribbon, 100 nF at the mic's VDD |
| `audio.py`: `connection closed by the board`, exit 1 | another client holds port 7076 (the board logs `refused`); `connection refused`, exit 2, is a firmware without audio or a wrong port |
| `audio.py` exits 3 with "board sent nothing" | the board accepted the connection but its microphone task is not producing: look for `audio: mic` lines in the log |
| `N samples dropped` in the `audio: mic` line | the network took longer than half a second to accept the stream: Wi-Fi jitter or a client that reads too slowly |
| `audio unavailable` in the boot log | the I2S port or its pins could not be opened; the line names the step, the face is unaffected |

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
| `Resource busy` on the port | another monitor holds it; close it, or talk to the board over Wi-Fi |
| No `wifi: got ip` line, `disconnected (reason …)` repeats | the reason's hint in the log: no AP with that name in range (2.4 GHz only), or a wrong password; fix `athena_secrets.h`, rebuild, flash |
| `got ip`, but `athena-matrix.local` does not resolve on the Mac | the network blocks mDNS queries: use the address from that line, `ATHENA_MATRIX_HOST=10.x.x.x` |
| `got ip`, but no reply over Wi-Fi at all | the Mac is on another network, or the access point isolates clients: `ping` the board's address; if that fails, the cable is the transport |
| Boot loops right after `got ip` | a panic in the Wi-Fi path: watch the serial log for `Guru Meditation` and decode the backtrace per the `esp-idf` skill |
| `err busy` over Wi-Fi | four connections are already open; close one (a vanished peer frees its slot after about a minute) |

## Preview on the Mac

`host/` compiles the real `main/aura.c` with clang against stubs for the ESP-IDF headers and the hub75 driver, so a change to the renderer is looked at before any firmware build and without a board. `python3 host/sheet.py all` writes contact sheets of what the panel would show (5-bit levels, dithered) for every state, the colour fades and a whole session into `host/out/`, and prints how much each frame moved. `node host/parity.mjs` checks that `aura.c` and `reference/aurora.js` draw the same frames. `node host/sheet.mjs` is the same sheet from the JS alone. `python3 host/bench/make_bench.py --out <file>` rebuilds the live bench page from `aurora.js`. `host/README.md` has the details and what the numbers mean.

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
