# athena_audio — INMP441/ICS-43434 microphone and MAX98357A amplifier on an ESP32-S3

Pure ESP-IDF firmware for the Athena audio board, the ears and the mouth
designed in `AUDIO-BOARD.md`. What runs today is the **raw bridge** that
brings the hardware up, stages 2–4 of that note with the Mac as the meter:
one I2S port in full duplex carries the microphone in and the amplifier out,
and `main/audio.c` bridges both to TCP port 7076 as raw PCM. No wake word, no
echo cancellation and no hub protocol yet: the board moves sound and the Mac
is the brain. `scripts/audio.py` is the bench client (`scripts/voice.py` still
uses the Mac's own microphone and speakers). ESP-SR and the WebSocket hub
protocol are the later stages of the design note. The board answers as
`athena-audio.local`; the network's name and password come from
`../components/athena_common/include/athena_secrets.h`, shared with the
matrix (see Build).

Everything about *how* to build, flash and watch the board lives in the global
`esp-idf` Claude Code skill. This file holds what is specific to this project:
the wiring, the stream and what to look at when there is no sound.

## Wiring

The pins are the table in `AUDIO-BOARD.md`, set in `main/board_pins.h`.

| ESP32-S3 pin | MAX98357A | INMP441 / ICS-43434 | Note |
|---|---|---|---|
| GPIO5 | BCLK | SCK | one shared bit clock |
| GPIO6 | LRC | WS | one shared word select |
| GPIO7 | | SD | audio in from the mic |
| GPIO15 | DIN | | audio out to the amp |
| GPIO16 | SD | | amp SD_MODE, driven high at boot: on, left slot |
| 3V3 | | VDD | the mic is 3.3 V only, 5 V kills it |
| 5V / VIN | VIN | | amp power from the 5 V rail, never from the 3V3 regulator |
| GND | GND | GND and L/R | one common ground; L/R to GND puts the mic in the left slot |

- **Speaker**: both wires on the amp's screw terminal and nowhere else. The output is bridge-tied, so grounding either wire shorts the amp. 4 Ω or 8 Ω, 3 W.
- **Amp SD pin** on GPIO16 is driven high once at start: amp on, left slot, full level for a mono stream. The firmware puts the same sample in both slots, so a floating pin (the breakout's own divider selects the stereo average, half the level) still plays. Muting per utterance, which is what kills the idle hiss, comes with the later stages; today mute is streaming silence.
- **GAIN** unconnected is 9 dB; to GND 12 dB; to VIN 6 dB; through 100 kΩ to GND 15 dB, to VIN 3 dB.
- **1000 µF 16 V** across the amp's VIN and GND, stripe leg on GND, within a few centimetres of the amp: the class-D bursts otherwise dip the 5 V rail and reset the board.
- **Second mic**: same SCK, WS and SD wires, its L/R to 3V3; both mics tri-state SD outside their own slot. `AUDIO_MIC_SLOT` in `main/audio.c` picks the one that is streamed.
- Keep the mic wires under 15 cm, twisted, away from the speaker leads and the matrix ribbon. Wire with USB unplugged and check the mic's VDD is on 3V3 twice.

## Build, flash, watch

```bash
. ~/esp/esp-idf/export.sh >/dev/null
cd firmware/athena_audio
cp ../components/athena_common/include/athena_secrets.h.example ../components/athena_common/include/athena_secrets.h   # first time only, shared with the matrix; fill in the Wi-Fi name and password
idf.py set-target esp32s3            # first time only
idf.py build
idf.py -p /dev/cu.usbmodemXXXXXXXX flash       # the native USB connector; /dev/cu.usbserial-* once a cable is on the UART connector
idf.py -p /dev/cu.usbmodemXXXXXXXX monitor     # leave with Ctrl+]
```

`partitions.csv` is the table from `AUDIO-BOARD.md`, with the 6 MB `model`
partition ESP-SR will use, so a board flashed today is not re-partitioned
later.

Boot log to expect:

```
I (xxx) wifi: station athena-audio, mdns athena-audio.local, service _athena-audio
I (xxx) audio: listening on tcp port 7076: pcm s16le 16000 Hz mono, mic out and speaker in on one connection
I (xxx) audio: i2s bclk 5 ws 6 dout 15 din 7, sd_mode 16, 16000 Hz, 32-bit slots, mic slot L, free heap NNNNNN
I (xxx) athena_audio: ready: athena-audio.local, mic out and speaker in on tcp port 7076
I (xxx) wifi: got ip 10.10.20.94 on <your network>, reachable as athena-audio.local      (a few seconds later)
```

## The stream

Port 7076, one TCP connection, raw PCM both ways: signed 16-bit
little-endian, 16 kHz, mono, no framing. From the moment a client connects
the board sends the microphone; every byte the client writes is played,
silence when nothing arrives. One client at a time: a second connection is
closed at once. The microphone never blocks or drops the client: whatever
the client cannot take right now, because the link stalls or it is busy
sending playback, is lost and counted on the 5 s log line rather than
closing the connection. A player should still drain what it receives so its
own socket buffer does not fill. Without a network there is nothing: the
board has no serial protocol, only its log.

On the I2S bus the mic delivers 24 bits MSB-aligned in 32-bit slots. The
firmware takes the left slot (`AUDIO_MIC_SLOT`), shifts it right by 14 bits
(`AUDIO_MIC_SHIFT`: 12 dB of gain over the raw top 16 bits) and clamps.
Playback puts each int16 into the top of both 32-bit slots.

Every 5 s the log shows both slots, which is how a wrong L/R or a dead wire
shows up:

```
I (xxx) audio: mic L -42 dBFS (peak -30), R -96 dBFS (peak -96), slot L -> 10.10.20.5:51234
```

With nothing wired to GPIO7 both slots read a flat -90 to -96 dBFS (the pin
sits at a fixed level, unlike the WROOM's floating GPIO34), so that reading
alone does not tell a missing mic from an unclocked one. With a mic in a quiet room
expect about -50 dBFS, speech -30 to -20.

## From the Mac

`scripts/audio.py` (standard library only, like `face.py`) is the bench
client. `--host` (`host` or `host:port`), else `ATHENA_AUDIO_HOST` (same
form), else `athena-audio.local` pick the board; `--port` sets the audio
port. `ATHENA_MATRIX_HOST` is the other board and is not consulted.

```bash
scripts/audio.py meter 5             # live mic level bar for 5 s
scripts/audio.py record 5 take.wav   # 5 s of the mic into a 16 kHz mono wav
scripts/audio.py tone 440 2          # 2 s of 440 Hz through the speaker
scripts/audio.py play take.wav       # any 16-bit wav; stereo is averaged, other rates resampled
scripts/audio.py play --gain 6 say.wav      # +6 dB before sending, clipped at full scale (speech peaks near 0 dBFS but averages far lower)
scripts/audio.py play --normalize say.wav   # peak to 0 dBFS without clipping; --gain adds on top
scripts/audio.py play --gain 20 say.wav     # a brick-wall limiter in effect (~40 % of samples clip): loud, still intelligible speech; avoid for long tones, the speaker is 3 W
```

## If the sound does not work

| Symptom | Look at |
|---|---|
| No `got ip` line | the network's name or password in `athena_secrets.h`; the matrix `README.md` has the Wi-Fi troubleshooting, the module is the same |
| Log shows -96 dBFS in both slots | the mic has no clock or no power: SCK, WS, VDD wires |
| Mic near full scale with nothing said | SD floating: GPIO7 to the mic's SD; or the streamed slot is the unwired one, flip L/R or `AUDIO_MIC_SLOT` |
| Level in the log on R, the stream silent | the mic sits in the right slot: L/R is on 3V3, or the breakout labels it the other way; move L/R to GND or set `AUDIO_MIC_SLOT` to 1 |
| Tone plays at half level | the amp's SD pin is not on GPIO16 (floating selects the stereo average); check the wire and the `sd_mode 16` in the boot log |
| Board resets when sound plays | the 5 V supply sags: the 1000 µF cap, GAIN to VIN, the amp on the 5 V bus instead of the laptop's USB |
| Hiss that follows the face | coupling from the matrix: shorter mic wires, away from the ribbon and the speaker leads, 100 nF at the mic's VDD |
| `audio.py`: `connection closed by the board`, exit 1 | another client holds port 7076 (the board logs `refused`) |
| `audio.py`: `connection refused`, exit 2 | the listener is not up: `audio unavailable` in the boot log, or a wrong host or port |
| `audio.py` exits 3 with "board sent nothing" | the board accepted the connection but its microphone task is not producing: look for `audio: mic` lines in the log |
| `N samples dropped` in the `audio: mic` line | the network took longer than half a second to accept the stream: Wi-Fi jitter or a client that reads too slowly |
| `audio unavailable` in the boot log | the I2S port, the SD_MODE pin or their GPIOs could not be opened; the line names the step |

## Design notes

- The I2S port is opened once for both directions with one
  `i2s_std_config_t`, which is what makes ESP-IDF pair TX and RX on one BCLK
  and one WS: 16 kHz, 32-bit slots, stereo frame, as `AUDIO-BOARD.md` requires
  for the echo reference later.
- `audio_rx` and `audio_tx` run on core 1, `audio_send` and `audio_net` on
  core 0 with Wi-Fi and lwIP; that is the core split the design note keeps
  when the AFE arrives.
- `sdkconfig.defaults` is the design note's starter without the ESP-SR keys;
  the TCP send and receive buffers are raised to 12 MSS because lwIP caps a
  socket near buffer / round-trip time and the stream is 32 kB/s.
