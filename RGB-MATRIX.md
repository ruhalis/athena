# RGB Matrix Display for Jarvis

Waveshare **RGB-Matrix-P3 64×64** (HUB75) driven by an **ESP32-S3** over **Wi-Fi**. The Mac publishes high-level state; the MCU renders. USB serial is not used.

## Architecture

```
Redis state_change  →  jarvis_display.py  →  WebSocket JSON  →  ESP32-S3  →  HUB75
```

MCU: ESP32-S3 + HUB75 adapter. Dedicated **5 V / 4 A** PSU. mDNS name `jarvis-matrix.local`.

Firmware: `jarvis/services/firmware/jarvis_matrix/` (PlatformIO). Copy `include/secrets.h.example` to `include/secrets.h`.

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
  url: ws://jarvis-matrix.local/ws
  brightness_idle: 40
  brightness_active: 180
```

If the socket cannot connect, `jarvis_display.py` logs the error and does not open a serial port.
