# RGB Matrix Display for Athena

Waveshare **RGB-Matrix-P3 64×64** (HUB75E) driven by an **ESP32-S3-DevKitC-1** over a **USB cable** from the Mac. The Mac writes one JSON line per state change to the board's serial port; the board renders. No Wi-Fi, no WebSocket, no credentials: plug in two cables and it works. Wi-Fi is a later step (see the end of this file).

Status: design for the S3 build. `firmware/athena_matrix/` currently holds a **bit-banged prototype** with its own `hub75` component; it builds for the WROOM-32 with its own pin map and for the ESP32-S3 with the pin map below (see `firmware/athena_matrix/README.md`). The driver submodule is declared in `.gitmodules` but not checked out. The serial protocol below is implemented in that prototype (`main/serial.c`, `main/face.c`, `main/protocol.h`) and runs on both targets.

## Architecture

```
athena-face plugin / scripts/face.py  →  USB (UART bridge, 115200, one JSON per line)  →  ESP32  →  16-pin HUB75E ribbon  →  panel
                                                                                  5 V / 4 A PSU     →  panel
```

Two cables reach the DevKit (USB to the Mac, ribbon to the panel) and one reaches the panel (its own 5 V supply). The DevKit's USB cable is both its power and the command channel.

## Parts

| Part | Choice | Notes |
|---|---|---|
| Panel | Waveshare RGB-Matrix-P3-64x64 | 192 × 192 mm, 4096 LEDs, 3 mm pitch, 1/32 scan, HUB75E in and out headers, ≤ 20 W |
| MCU | ESP32-S3-DevKitC-1 **N16R8** | same module as the audio board; PSRAM is unused here, the DMA buffers live in internal RAM |
| Ribbon | 16-pin 2×8 IDC ribbon | ships with the panel; keep it under 30 cm |
| Breakout | 2×8 IDC breakout or 15 Dupont jumper wires | an adapter board wired to the pin map below saves the jumpers but is not required |
| Panel PSU | 5 V, **≥ 4 A**, with the VH4 power lead | the lead ships with the panel; never power the panel from the DevKit |
| USB | USB-C data cable to the DevKit's **UART** connector | the connector labelled UART, not USB |
| Optional | 74HCT245 level shifter | only if a 3.3 V ribbon shows ghosting, see Troubleshooting |

## The panel

The back has two 2×8 headers, **IN** and **OUT**, marked by arrows; the ribbon goes on **IN**. The 4-pin VH4 header is the power input: 5 V only, Waveshare warns that any other voltage burns the panel. OUT is only for chaining a second panel.

HUB75E pinout, looking at the IN header with the key notch at the top (pin 1 is marked on the silkscreen; odd pins are one row, even pins the other):

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | R1 (red, top half) | 2 | G1 (green, top half) |
| 3 | B1 (blue, top half) | 4 | GND |
| 5 | R2 (red, bottom half) | 6 | G2 (green, bottom half) |
| 7 | B2 (blue, bottom half) | 8 | **E** (row select bit 4) |
| 9 | A (row select bit 0) | 10 | B (row select bit 1) |
| 11 | C (row select bit 2) | 12 | D (row select bit 3) |
| 13 | CLK | 14 | LAT / STB |
| 15 | OE (output enable, active low) | 16 | GND |

Waveshare's own figures number the ribbon the other way, 16 down to 1 with R1 as wire 16 and the last GND as wire 1: their wire N is pin 17 − N here (their 13 is GND, pin 4 above; pin 13 CLK above is their wire 4). The layout is the same, so wire by signal name. The rainbow ribbon repeats its colours between the two halves; count from the marked edge wire.

A 64×64 panel is 1/32 scan: A–E select one of 32 row pairs and the R/G/B lines carry the top half (rows 0–31) and the bottom half (rows 32–63) at once. **E is not optional.** Without it only the top half addresses correctly; on a 1/16 panel pin 8 is ground.

## Power

- The panel draws up to 4 A at full white. Feed it from its own 5 V ≥ 4 A supply through the VH4 lead. The DevKit's 5V pin and the Mac's USB port cannot source that, and Waveshare notes the panel's inputs misbehave when the supply sags.
- The DevKit is powered by the Mac over the UART connector. Nothing else needs to feed it.
- **One ground.** Panel GND, PSU GND, and a DevKit ground pin must be tied together, or the data lines have no reference and the panel shows noise. Ribbon pins 4 and 16 do this if the DevKit ground goes to the breakout.
- Order: ribbon on, ground tied, panel PSU on, then the DevKit's USB. Never plug or unplug the ribbon with the panel powered.
- Start at brightness 40 of 255 while wiring. Full white at 255 is where the 4 A goes.

## Wiring the ribbon to the DevKit

Every HUB75 signal lands on the DevKit's **J1** header, in header order, so a breakout wires up in one pass. J1 pin numbers count from the 3V3 end. The ESP32-S3 routes the LCD_CAM peripheral through the GPIO matrix, so any output-capable pin works; these were picked to skip the pins that do not.

| HUB75 pin | Signal | GPIO | J1 pin |
|---|---|---|---|
| 1 | R1 | 4 | 4 |
| 2 | G1 | 5 | 5 |
| 3 | B1 | 6 | 6 |
| 5 | R2 | 7 | 7 |
| 6 | G2 | 15 | 8 |
| 7 | B2 | 16 | 9 |
| 9 | A | 17 | 10 |
| 10 | B | 18 | 11 |
| 11 | C | 8 | 12 |
| 12 | D | 9 | 15 |
| 8 | E | 10 | 16 |
| 13 | CLK | 11 | 17 |
| 14 | LAT | 12 | 18 |
| 15 | OE | 13 | 19 |
| 4, 16 | GND | GND | 22 (G) |

Skipped on purpose: GPIO3 and GPIO46 (strapping, they sit between 8 and 9 on J1), GPIO0 and GPIO45 (strapping), GPIO19/20 (USB), GPIO26–32 (flash), GPIO35–37 (octal PSRAM on N16R8), GPIO43/44 (the UART console that carries the commands). GPIO14 stays free next to the block.

The driver library's ESP32-S3 defaults do not fit a 64×64 panel: they leave E unassigned and put C on strapping pin GPIO3. Waveshare's ESP32-S3 wiring diagram (the ESP-IDF page for this panel) is exactly those defaults plus E on GPIO9 (A 18, B 8, C 3, D 42, E 9, CLK 41, LAT 40, OE 2). It agrees with the table above only on the six colour lines, and its D, CLK and LAT sit above GPIO31 where the interim `hub75` driver cannot reach. Do not wire from it. Always pass this pin map explicitly.

The panel's input buffers are 5 V parts, and the ESP32 drives 3.3 V. That works over a short ribbon; a long or noisy one shows ghosting or flicker. Shorten first; add a 74HCT245 between the DevKit and the ribbon only if that is not enough.

## Firmware (ESP-IDF, `firmware/athena_matrix/`)

Toolchain: **pure ESP-IDF** at the tag pinned by the global Claude Code `esp-idf` skill (`~/.claude/skills/esp-idf/idf-version`). No Arduino, no PlatformIO. How to build, flash, and watch it is that skill; Mac toolchain setup is `setup-macos.md` next to it; Athena-specific layout is in `CLAUDE.md`. This board needs **no secrets header**: nothing to join, nothing to authenticate.

HUB75 driver: `ESP32-HUB75-MatrixPanel-I2S-DMA` (mrcodetastic) as a git submodule at `firmware/athena_matrix/components/ESP32-HUB75-MatrixPanel-I2S-DMA`; that directory name is what its CMake expects. With `CONFIG_ESP32_HUB75_USE_GFX=n` it needs only `esp_lcd` and `driver` and drives the panel from the S3's LCD_CAM peripheral over GDMA; `examples/esp-idf/without-gfx` inside the submodule is the reference (its `main/CMakeLists.txt` lists the component under `REQUIRES`). Leave the PSRAM framebuffer option off. The API is a C++ class, so keep it behind one `.cpp` wrapper that exposes `extern "C"` functions; the rest of the firmware is C.

The wrapper's setup, with the pin map above:

```cpp
HUB75_I2S_CFG cfg(64, 64, 1);           // width, height, chain length
cfg.gpio.r1 = 4;  cfg.gpio.g1 = 5;  cfg.gpio.b1 = 6;
cfg.gpio.r2 = 7;  cfg.gpio.g2 = 15; cfg.gpio.b2 = 16;
cfg.gpio.a = 17;  cfg.gpio.b = 18;  cfg.gpio.c = 8;  cfg.gpio.d = 9;  cfg.gpio.e = 10;
cfg.gpio.clk = 11; cfg.gpio.lat = 12; cfg.gpio.oe = 13;

static MatrixPanel_I2S_DMA panel(cfg);
panel.begin();
panel.setBrightness8(40);
panel.clearScreen();
```

Drawing without GFX: `drawPixelRGB888(x, y, r, g, b)`, `fillRect(x, y, w, h, r, g, b)`, `fillScreenRGB888(r, g, b)`, `clearScreen()`. There are no text primitives, so the clock digits come from a small bitmap font drawn pixel by pixel. A few dozen lines, not a library.

Serial input: the DevKit's UART connector is a CP2102N bridge on **UART0** (GPIO43/44), the same UART the log console uses, so no extra pins. Install the UART driver on UART0 at 115200 8N1, read it line by line, parse each line with the bundled `cJSON`. Log lines and command replies share the port; that is fine, the Mac side filters.

Tasks: `serial` (reads lines, validates, posts a command struct) and `render` (40 fps, draws the current mode, applies brightness and the ttl fallback). One queue between them, nothing else shared. With the DMA driver `render` can sit on core 1; with the interim bit-banged driver both stay on core 0 because its refresh loop owns core 1 and never blocks.

`sdkconfig.defaults` starter:

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP32_HUB75_USE_GFX=n
```

## Protocol

One JSON object per line, terminated by LF (a CR before it is ignored), at most 256 bytes. The board answers every line with exactly `ok` or `err <reason>` on the same port, interleaved with its normal log output; the Mac side skips any line that is neither. Unknown keys are ignored; `{}` is a ping and answers `ok`. An unknown mode answers `err bad mode` and the display keeps its state; malformed JSON answers `err bad json`; an over-long line is dropped with `err too long`.

| Key | Type | Effect |
|---|---|---|
| `mode` | string | one of the modes below, applied at the next frame |
| `t` | string, ≤ 8 chars | text shown by `idle` and `alert` (the Mac's clock, `14:32`); remembered until replaced, `""` clears it. The board has no clock of its own. |
| `ttl` | integer seconds | how long the mode stays before the board falls back to `idle`; `0` = until told otherwise. Overrides the mode's default. |
| `brightness` | integer 0–255 | applies immediately, persists across modes; may come alone or with a mode |

Eight agent states and two maintenance modes. Each state must be tellable from the others at a glance across a room, which is why there are not more of them; every state is tied to an event the host can actually observe (see `.hermes/plugins/athena-face/`), not to a mood the model would have to invent.

| Mode | Agent state | Default ttl | Look |
|---|---|---|---|
| `idle` | nothing happening | sticky | cyan aura: a soft ring breathing slowly through the turbulence; `t` in its centre if set |
| `listen` | a message arrived, the user is talking | 30 s | mint aura, idle's hue a step toward green, a little larger and quicker, a quick shallow pulse on a 0.7 s beat |
| `think` | LLM request in flight | 120 s | azure aura, idle's hue a step toward blue, swirling faster, swelling slowly from dim to bright |
| `work` | a tool is running (shell, MCP, browser) | 300 s | ice aura, idle's cyan lifted toward white, turning fast at a steady bright level |
| `speak` | the reply is being delivered (later: TTS) | 8 s | cyan aura, idle's ring a little quicker, swelling and glowing with a voice level that comes in syllables and phrases |
| `alert` | needs the user: plan awaiting approval, brief delivered, question | sticky | gold aura flashing once a second; `t` in its centre if set |
| `error` | something failed: tool, API, disconnect | 10 s | red aura torn by heavy turbulence, beating twice a second, trembling, in a pulsing red frame |
| `sleep` | night, do not disturb | sticky | dim deep-blue aura, smaller and low in the panel, drifting slowly, no haze |
| `test` | wiring check | sticky | red top-left, green top-right, blue bottom-left, white bottom-right, white border |
| `off` | blank | sticky | output disabled; any other mode re-enables it |

Every state is the idle picture with some numbers moved (size, pace, turbulence, brightness and pulse, colour). The palette stays close: `listen`, `think` and `work` keep idle's cyan within a step of hue, so motion is what tells them apart, and only `alert` and `error` change colour outright. A change of state is a tween: the board eases from whatever is on the panel to the new state over 0.8 s, the colour fades more slowly over 2 s as a mix of the two colours' light rather than a sweep through the hues between, dimming a little halfway, the haze behind the ring blends along with it, `t` fades with it, and the turbulence keeps its phase, so nothing ever cuts. Out of `test` or `off` the next state fades in from dark.

The board boots into `test` and stays there until the first command, so a panel can be checked with nothing but power and USB. When a ttl runs out the board returns to `idle` and keeps `t`.

```
{"mode":"idle","t":"14:32"}   -> ok
{"mode":"work","ttl":600}     -> ok
{"brightness":80}             -> ok
{"mode":"dance"}              -> err bad mode
```

## Using it from the Mac

1. **Connect.** Panel PSU on, then the DevKit's UART connector to the Mac. Find the port with `ls /dev/cu.usbserial-*` (never `/dev/tty.*`). Record it as `matrix: /dev/cu.usbserial-XXXXXXXX` under `## Boards` in `CLAUDE.md`; the `esp-idf` skill reads it from there.
2. **Build and flash** from the project directory, following the `esp-idf` skill:

   ```bash
   . ~/esp/esp-idf/export.sh >/dev/null
   cd firmware/athena_matrix
   idf.py set-target esp32s3        # first time only
   idf.py build
   idf.py -p /dev/cu.usbserial-XXXXXXXX flash
   ```

3. **Talk to it by hand.** Either of these opens the port as a terminal; type a line, press Enter, watch the panel and the reply:

   ```bash
   idf.py -p /dev/cu.usbserial-XXXXXXXX monitor    # leave with Ctrl+]
   screen /dev/cu.usbserial-XXXXXXXX 115200        # leave with Ctrl+A then K
   ```

   Send `{"mode":"test"}` first. Four coloured quadrants with a clean border means the ribbon, the scan lines, and E are right. Then `{"mode":"idle","t":"14:32"}` and `{"mode":"think"}`.
4. **Talk to it from a script.** `scripts/face.py` in the repo root is the Mac side: standard library only, no pyserial. It resolves the port from `--port`, then `ATHENA_MATRIX_PORT`, then the `## Boards` section of `CLAUDE.md`, then a lone `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*`. It clears DTR and RTS in one step after opening, so the bridge does not reset the board.

   ```bash
   scripts/face.py test                       # wiring pattern
   scripts/face.py idle                       # face with the current time
   scripts/face.py work --ttl 600
   scripts/face.py --brightness 60
   scripts/face.py --demo                     # walks through every state, 4 s each
   scripts/face.py --demo --dry-run --pause 0 # prints the lines it would send, no board needed
   ```

   Only one process can hold the port. Close the monitor before flashing or scripting, or macOS answers `Resource busy`. The `athena-face` plugin holds the port whenever a Hermes session that loaded it is alive.

### Troubleshooting

| Symptom | Look at |
|---|---|
| Nothing lights | panel PSU; ribbon on IN not OUT; OE wire; ribbon plugged one pin off |
| Top half right, bottom half garbage or mirrored | E wire (HUB75 pin 8 → GPIO10) |
| Colours swapped | R/G/B order on the ribbon, top half is pins 1–3, bottom half 5–7 |
| Rows scrambled | A–D wires, one per row-select bit |
| Image shifted one column, or a stuck first column | set `cfg.clkphase = false` |
| Flicker, ghosting, faint doubles | ribbon too long for 3.3 V logic: shorten, lower `cfg.i2sspeed`, then a 74HCT245 |
| Panel stays dark with correct wiring | driver chip needs init: try `cfg.driver = HUB75_I2S_CFG::FM6126A` |
| Board resets when the panel goes bright | supply sag or missing common ground |
| Board reboots each time a script opens the port | DTR/RTS toggled on open; deassert both before `open()` |
| `Resource busy` on the port | another monitor or terminal holds it; close it, never kill it blind |

## Config

The Hermes side is the project plugin `.hermes/plugins/athena-face/` (enable once with `hermes plugins enable athena-face`, with `HERMES_ENABLE_PROJECT_PLUGINS=true` in `~/.hermes/.env`). It hooks the agent loop (`pre_gateway_dispatch` → listen, `pre_llm_call` → think, `pre_tool_call` → work, `post_tool_call` → think or error, `post_llm_call` → speak, or alert for a cron run, `pre_approval_request` → alert, `api_request_error` → error) and pushes each state to a background thread that owns the serial port through `scripts/face.py`. Hooks never block the agent: if the port is missing or busy the plugin logs one warning and tries again on the next state, no more often than every 30 s. Settings are environment variables in `~/.hermes/.env`:

```
ATHENA_MATRIX_PORT=/dev/cu.usbserial-XXXXXXXX   # optional, else face.py's port resolution
ATHENA_FACE_BRIGHTNESS=40                        # optional, sent when the port opens
ATHENA_FACE_SLEEP=23:00-07:00                    # optional, idle shows as sleep in this window
ATHENA_FACE=0                                    # disable the plugin
ATHENA_FACE_DRY_RUN=1                            # print the lines instead of opening a port
```

The board falls back to `idle` on its own when a `think` or `work` outlives its ttl, so a crashed session never leaves the face stuck.

## Later: Wi-Fi

The earlier design put the same JSON on a WebSocket (`ws://athena-matrix.local/ws`, mDNS via the `mdns` component, `esp_websocket_client`, credentials from `firmware/components/athena_common/include/athena_secrets.h`). It is deferred, not dropped. The line format and the render task stay as they are; only the `serial` task gets a sibling that feeds the same queue from a socket, and the secrets header becomes required on that day. Until then the USB cable is the transport.
