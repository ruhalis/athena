# RGB Matrix Display for Athena

Waveshare **RGB-Matrix-P3 64×64** (HUB75) driven by an **ESP32-S3** over **Wi-Fi**. The Mac publishes high-level state; the MCU renders. USB serial is not used.

## Architecture

```
athena.py  →  WebSocket JSON  →  ESP32-S3  →  HUB75
```

MCU: ESP32-S3 + HUB75 adapter. Dedicated **5 V / 4 A** PSU. mDNS name `athena-matrix.local`.

Firmware: `firmware/athena_matrix/`, **pure ESP-IDF** at the tag pinned by the global Claude Code `esp-idf` skill (`~/.claude/skills/esp-idf/idf-version`). No Arduino, no PlatformIO. How to build, flash, and watch it is that skill; Mac toolchain setup is `setup-macos.md` next to it; Athena-specific layout is in `CLAUDE.md`. Wi-Fi and hub credentials come from `firmware/components/athena_common/include/athena_secrets.h` (copy the `.example`; gitignored; one file serves both boards).

Status: design note. Nothing in `firmware/athena_matrix/` exists yet.

## Firmware (ESP-IDF 5.x)

MCU: ESP32-S3-DevKitC-1 with the same N16R8 module as the audio board, so both projects share one sdkconfig baseline. PSRAM is not required here, since the HUB75 DMA buffers must live in internal RAM anyway; it only widens the heap.

HUB75 driver: `ESP32-HUB75-MatrixPanel-I2S-DMA` (mrcodetastic) as an IDF component. It builds under plain IDF with no Arduino when `CONFIG_ESP32_HUB75_USE_GFX` is off: the library then depends only on `esp_lcd` and `driver` and drives the panel from the S3's LCD_CAM peripheral over GDMA. Its `examples/esp-idf/without-gfx` is the reference. Add it as a git submodule at `firmware/athena_matrix/components/ESP32-HUB75-MatrixPanel-I2S-DMA`; that directory name is what its CMake expects. Cost of no GFX: no text or shape primitives, so the clock digits and the think bar are drawn from a small bitmap font straight into the panel buffer. A few dozen lines, not a library. The driver API is a C++ class: keep it behind one `.cpp` wrapper exposing `extern "C"` functions so the rest of the firmware stays C.

Pins: a 64×64 panel is 1/32 scan and needs the **E** line. The library's S3 defaults leave E unassigned and put C on GPIO3 (a strapping pin), so always pass an explicit pin map matching the adapter board. Avoid GPIO0/3/45/46 (strapping), 19/20 (USB), 26–32 (flash), and 35–37 (octal PSRAM on N16R8).

Tasks: `net` on core 0 (Wi-Fi, mDNS `athena-matrix` via the `mdns` component, `esp_websocket_client`, parses `{"mode": ...}`); `render` on core 1 (fixed frame rate, draws the current mode into the DMA buffer, applies `brightness_idle` / `brightness_active`). The two meet through one queue.

`sdkconfig.defaults` starter:

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
# CONFIG_ESP32_HUB75_USE_GFX is not set
```

## Protocol

One JSON object per WebSocket message:

```json
{"mode": "think"}
{"mode": "clock", "t": "14:32"}
```

| State | Animation |
|---|---|
| IDLE | Clock |
| PROCESSING | Think bar |

No speak EQ bars until TTS exists.

## Config

```yaml
display:
  enabled: true
  transport: wifi
  url: ws://athena-matrix.local/ws
  brightness_idle: 40
  brightness_active: 180
```

If the socket cannot connect, `athena.py` logs the error and continues the turn. It does not open a serial port.
