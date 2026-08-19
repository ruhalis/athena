# Jarvis matrix firmware (ESP32-S3)

PlatformIO project. Wi-Fi WebSocket JSON → HUB75 64×64.

1. Copy `include/secrets.h.example` to `include/secrets.h` and set SSID/password.
2. `pio run -t upload`
3. Mac: `display.url: ws://<esp-ip>/ws` in `jarvis/config.yaml`
