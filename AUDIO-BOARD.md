# Audio Board for Athena (pure-parts build)

Companion to `RGB-MATRIX.md`. The matrix board is Athena's face; this board is
its ears and mouth. Both are ESP32-S3 boards in one enclosure. This one dials the
Mac hub (`athena.py`) over WebSocket; the matrix is wired over USB serial for
now. The Mac does STT, the Hermes turn, and
TTS; this board does wake word, echo cancellation, capture, and playback.

Status: design note. Nothing in `firmware/athena_audio/` exists yet.

## Parts

| Part | Choice | Why |
|---|---|---|
| MCU | ESP32-S3-DevKitC-1 **N16R8** (WROOM-1, 16 MB flash, 8 MB octal PSRAM) | ESP-SR needs PSRAM for AFE buffers and models |
| Mics ×2 | **ICS-43434** I2S breakouts, same batch | 65 dBA SNR meets Espressif's "≥64 dB recommended". INMP441 is 61 dBA, below their 62 dB floor; it works but wake range suffers |
| Amp | MAX98357A I2S class-D breakout | 3.2 W into 4 Ω at 5 V, accepts 16/24/32-bit slots, SD pin gives a mute/enable |
| Speaker | 4 Ω 3 W full-range, 40–50 mm, in a sealed chamber | Voice band only; small sealed box is enough |
| Power | 5 V ≥ 2 A USB-C adapter for this board, or a shared 5 V ≥ 6 A rail with the matrix | Amp peaks near 1 A; matrix can pull 4 A |
| Caps | 470–1000 µF across amp VIN/GND; 100 nF at each mic VDD | Class-D current spikes and mic PSRR |
| Optional | Momentary button for push-to-talk | Lets you test the whole loop before the wake word works |
| Mechanical | Rubber grommets or foam gaskets for the mics, dust mesh, 16-pin cable to the matrix board is unrelated | See acoustic rules below |

## Wiring

One I2S port in **full-duplex**: TX (amp) and RX (mics) share BCLK and WS.
ESP-IDF requires both directions to use the same sample rate and the same
bits per frame, so everything runs at **16 kHz, 32-bit slots, 2 slots**.
The MAX98357A accepts 32-bit slots, and the mics emit 24-bit in a 32-bit
slot, so nothing needs a second clock domain. Shared clocks also make the
echo reference offset a constant, which is what makes software AEC workable.

| Signal | GPIO | Notes |
|---|---|---|
| I2S BCLK → mic SCK ×2, amp BCLK | 5 | |
| I2S WS → mic WS ×2, amp LRC | 6 | |
| I2S DIN ← mic SD (both mics on one line) | 7 | mic A `L/R` → GND (left slot), mic B `L/R` → 3V3 (right slot) |
| I2S DOUT → amp DIN | 15 | |
| Amp SD_MODE | 16 | low = shutdown (no idle hiss), high = on |
| PTT button (optional) | 4 | to GND, internal pull-up |
| On-board RGB LED | 48 on v1.0 boards, 38 on v1.1 | state debug only |
| Mic VDD | 3V3 | |
| Amp VIN | 5V | star ground back to the DevKit GND |

Pins avoided on purpose: GPIO35–37 (octal PSRAM on N16R8), GPIO26–32
(flash), GPIO19/20 (USB), strapping pins GPIO0/3/45/46.

Keep the four I2S mic wires under about 15 cm, twisted, and routed away
from the speaker leads and the matrix cable. Class-D outputs and HUB75
PWM both couple into MEMS mics as hiss.

## Acoustic and mechanical rules (Espressif microphone guidelines)

- Two mics **4–6.5 cm apart** on a horizontal axis, centred on the front face.
- Mic hole diameter > 1 mm, depth-to-diameter < 2:1, so a 1.5 mm hole through a wall of at most 3 mm.
- Seal each mic port to its hole with silicone or foam; the guideline wants ≥ 25 dB leak attenuation. Add a dust mesh.
- Mics far from the speaker, decoupled with rubber; speaker in its own sealed chamber.
- Sound pressure at the mics must stay ≤ 102 dB at maximum volume, so cap the amp gain rather than relying on AEC.
- Use the same mic model from the same manufacturer for both positions.

## Firmware (ESP-IDF 5.x, C)

Toolchain: **pure ESP-IDF** at the tag pinned by the global Claude Code
`esp-idf` skill (`~/.claude/skills/esp-idf/idf-version`), the same as the
matrix board. No Arduino core and no PlatformIO: the Arduino core PlatformIO
ships for the S3 cannot build ESP-SR. Build, flash, and monitor rules are
that skill; Mac setup is `setup-macos.md` next to it; Athena-specific layout
is in `CLAUDE.md`. The project lives at
`firmware/athena_audio/`. Add the SDK with
`idf.py add-dependency "espressif/esp-sr"` (registry component, IDF ≥ 5.0).
The `esp-skainet` wake-word example is the reference for AFE + WakeNet; only
the I2S reader is board-specific and is yours.

sdkconfig essentials: octal PSRAM at 80 MHz, CPU 240 MHz, a custom partition
table with a `model` partition for ESP-SR, WakeNet model `wn9_hiesp` for
the first phrase.

`sdkconfig.defaults` starter, lifted from esp-skainet's
`wake_word_detection/afe` example with the wake word swapped:

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_SR_WN_WN9_HIESP=y
CONFIG_SR_VADN_VADNET1_MEDIUM=y
```

`partitions.csv`: the `model` partition is where esp-sr stores its models,
and `idf.py flash` writes it together with the app. NVS and PHY partitions
are there because Wi-Fi needs them; the esp-skainet example has neither.

```
# Name,     Type, SubType, Offset,  Size
nvs,        data, nvs,     0x9000,  24K
phy_init,   data, phy,     0xf000,  4K
factory,    app,  factory, 0x10000, 3M
model,      data, spiffs,  ,        6M
```

AFE setup: `afe_config_init("MMR", models, AFE_TYPE_SR, AFE_MODE_LOW_COST)`
with AEC, NS, VAD, and WakeNet enabled. ESP-SR's AEC supports at most two
mics and only 16 kHz; `filter_length` starts at the recommended 4 and goes
to 8 only if the room is reverberant. Feed the AFE interleaved int16 frames
in the order mic1, mic2, reference, in chunks of `get_feed_chunksize()`.
Fetch returns cleaned mono 16 kHz plus `wakeup_state` and `vad_state`;
the cleaned mono is what goes uplink, never the raw stereo.

Reference channel: the player task writes every block both to I2S TX and to
a ring buffer. The capture task reads the reference for each feed chunk
from that ring at a fixed lag. Because TX and RX share clocks, the lag is a
constant measured once with a click loopback test. Espressif's ADF guidance
is that the recording should trail the reference by 0–10 ms, so the ring
offset is tuned to land in that window. When idle, TX keeps writing zeros
so the clocks run and the ring stays aligned, and the amp SD pin stays low.

Sample conversion: mic samples arrive as 24-bit left-justified in int32;
shift right by 14 with clipping to get int16 and about 12 dB of gain, or
leave the AFE gain control to do it.

Tasks:

| Task | Core | Job |
|---|---|---|
| `i2s_rx` | 1 | read stereo frames, convert, assemble MMR chunk, `afe->feed` |
| `afe_fetch` | 1 | `afe->fetch`, publish wake / VAD events, push cleaned PCM to uplink queue in LISTEN |
| `player` | 1 | drain downlink PCM ring into I2S TX, copy into reference ring, zeros when idle |
| `net` | 0 | Wi-Fi, `esp_websocket_client` with auto-reconnect, JSON text frames + binary PCM |
| `state` | 0 | IDLE → LISTEN → THINK → SPEAK, timeouts, barge-in |
| `led` | 0 | mirror state on the on-board RGB LED |

Budget: AFE FD_LOW_COST is about 31 KB internal RAM, 90 KB PSRAM, and 20 %
of one core per Espressif's benchmark; WakeNet and Wi-Fi fit alongside on
N16R8 with room to spare.

## Protocol with the hub

Board → hub, text frames:

```json
{"type":"hello","device":"athena-audio","fw":"0.1","rate":16000}
{"type":"wake","phrase":"hiesp"}
{"type":"speech_end"}
{"type":"interrupt"}
{"type":"button"}
```

Board → hub, binary frames: cleaned mono int16 at 16 kHz, one AFE chunk per
frame, sent only in LISTEN.

Hub → board, text frames:

```json
{"type":"state","state":"listen"}
{"type":"state","state":"think"}
{"type":"state","state":"speak"}
{"type":"play_end"}
{"type":"state","state":"idle"}
{"type":"volume","db":-6}
```

Hub → board, binary frames: mono int16 at 16 kHz to play. The hub resamples
TTS output to 16 kHz so one rate exists everywhere.

State machine on the board:

| State | Enter on | Leave on |
|---|---|---|
| IDLE | boot, `play_end`, listen timeout | wake word or button |
| LISTEN | wake / button | VAD end + ~600 ms hangover → `speech_end`, or 8 s without speech |
| THINK | hub state `think` | first downlink PCM |
| SPEAK | downlink PCM | `play_end`; wake word during SPEAK → flush TX ring, send `interrupt`, go LISTEN |

The hub fans every state change out to the matrix board as its existing
`{"mode": ...}` message, so the face reacts without the two boards talking.

## Stages, each with a checkpoint

1. **Board only.** PSRAM reports 8 MB at boot, Wi-Fi joins, WebSocket echo to the Mac works, RGB LED blinks.
2. **Mics.** One mic → RMS meter on serial, clap test. Then both: cover one mic and confirm left/right separation.
3. **Amp.** Sine sweep, then toggle SD_MODE and confirm the hiss disappears. Then full-duplex on one port: play a click, find its peak in the mic stream, record the lag in samples. That number is the reference offset.
4. **Raw dump.** Stream raw stereo to the Mac, save a WAV, inspect the noise floor with the matrix on and off. Fix wiring and power here, before any speech code runs.
5. **ESP-SR.** AFE `MMR` + WakeNet "Hi ESP" + VAD, detections on serial. Leave the TV on for an evening and count false accepts.
6. **Half-duplex with the hub.** Wake → LISTEN → uplink → `speech_end` → THINK → SPEAK → IDLE. Reference channel is zeros and wake detection is ignored during SPEAK.
7. **AEC and barge-in.** Feed the TX ring as the reference. Play TTS at normal volume and say the wake word ten times; aim for at least eight detections. Tune `filter_length` and the ring offset.
8. **Custom wake word.** Train "hey Athena" with the microWakeWord trainer from synthetic speech and port its tflite-micro streaming inference, using ESPHome's component as the reference.

## Mac-side deltas

`athena.py` gains binary frame handling, VAD-free operation (the board sends
`speech_end`), STT and streaming TTS through the OpenAI API, one call to the
Hermes chat completions endpoint with a session id header, and the fan-out
of state to the matrix. Everything else in `RGB-MATRIX.md` stays.

## Sources

- Espressif Microphone Design Guidelines: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/Espressif_Microphone_Design_Guidelines.html
- ESP-SR Audio Front-end: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html
- ESP-SR Acoustic Echo Cancellation: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html
- ESP-ADF algorithm stream (delay guidance): https://docs.espressif.com/projects/esp-adf/en/latest/api-reference/streams/index.html
- ESP-IDF I2S driver (full-duplex rules): https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html
- ESP32-S3-DevKitC-1 v1.1 user guide: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html
- ICS-43434 datasheet: https://www.mouser.com/datasheet/2/400/ds_000069_ics_43434_v1_2-2581173.pdf
- INMP441 datasheet: https://www.digikey.com/htmldatasheets/production/1431884/0/0/1/inmp441-datasheet.html
