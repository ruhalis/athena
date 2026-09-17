# athena_audio — INMP441/ICS-43434 microphone and MAX98357A amplifier on an ESP32-S3

Pure ESP-IDF firmware for the Athena audio board, the ears and the mouth
designed in `AUDIO-BOARD.md`. What runs today is the **raw bridge** that
brings the hardware up, stages 2–4 of that note with the Mac as the meter:
one I2S port in full duplex carries the microphone in and the amplifier out,
and `main/audio.c` bridges both to TCP port 7076 as raw PCM. Since stage 5
the same microphone also feeds ESP-SR's audio front end in `main/sr.c`: the
wake word **"Hi, ESP"** and a VAD, their detections on the console and
nowhere else yet. No echo cancellation and no hub protocol: the board moves
sound and hears the wake word, the Mac is still the brain. `scripts/audio.py`
is the bench client (`scripts/voice.py` still uses the Mac's own microphone
and speakers). The WebSocket hub protocol is stage 6 of the design note. The
board answers as
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

`partitions.csv` is the table from `AUDIO-BOARD.md`; the 6 MB `model`
partition holds the ESP-SR models. The esp-sr component (a registry
dependency in `main/idf_component.yml`, fetched into `managed_components/`
on the first build) packs the models selected in `sdkconfig.defaults` into
`build/srmodels/srmodels.bin`, and `idf.py flash` writes it to that
partition along with the app. `idf.py app-flash` does not, so a board whose
model partition is still empty needs one full `flash`.

Boot log to expect:

```
I (xxx) wifi: station athena-audio, mdns athena-audio.local, service _athena-audio
I (xxx) audio: listening on tcp port 7076: pcm s16le 16000 Hz mono, mic out and speaker in on one connection
I (xxx) audio: i2s bclk 5 ws 6 dout 15 din 7, sd_mode 16, 16000 Hz, 32-bit slots, mic slot L, free heap NNNNNN
I (xxx) sr: model 0: wn9_hiesp (...)
I (xxx) sr: model 1: vadnet1_medium (...)
I (xxx) sr: [input] -> |...| -> |WakeNet(wn9_hiesp)| -> [output]          (the AFE's own pipeline line)
I (xxx) sr: wake word "Hi,ESP", vad vadnet1_medium (speech >= 128 ms, silence >= 600 ms, floor -60 dBFS), feed 512 samples, free heap NNNNNN
I (xxx) athena_audio: ready: athena-audio.local, mic out and speaker in on tcp port 7076, wake word and vad on this console
I (xxx) wifi: got ip 10.10.20.94 on <your network>, reachable as athena-audio.local      (a few seconds later)
```

`speech front end unavailable: ESP_ERR_NOT_FOUND` in place of the `sr:` lines
means the model partition is empty; the bridge still runs.

## The stream

Port 7076, one TCP connection, raw PCM both ways: signed 16-bit
little-endian, 16 kHz, mono, no framing. From the moment a client connects
the board sends the microphone; every byte the client writes is played,
silence when nothing arrives. One client at a time: a second connection is
closed at once, unless the first has taken nothing for 3 s (a peer that
vanished without closing), which then gives way to it.

The microphone is delivered whole or not at all. A block leaves the board
10 ms after its first sample; when the socket is full, because the link
stalls or the client reads slowly, the blocks wait: half a second in lwIP's
send buffer and two more in a queue in PSRAM, which a recovered link drains
in a moment, so a stall delays the audio and does not cut it. Only beyond
that are the newest blocks dropped, whole, and counted on the 5 s log line;
the client is never dropped for being slow, and a send that lwIP took only
part of resumes where it stopped, so the byte alignment of the samples
holds. The log line also says how deep the queue stood (`queued up to N ms`)
whenever a stall went past 50 ms. `scripts/audio.py check` is the same
question asked from the client's end: how late each packet lands against
the sample clock, and whether audio went missing. A player should still
drain what it receives so its own socket buffer does not fill. Without a
network there is nothing: the board has no serial protocol, only its log.

On the I2S bus the mic delivers 24 bits MSB-aligned in 32-bit slots. The
firmware takes the left slot (`AUDIO_MIC_SLOT`), shifts it right by 12 bits
(`AUDIO_MIC_SHIFT`: 24 dB of gain over the raw top 16 bits) and clamps.
The gain was 12 dB until stage 5 showed WakeNet needs speech near -20 dBFS:
at 12 dB a normal voice at a metre read -25 to -32 dBFS and the phrase only
fired at -19, so the shift went from 14 to 12, and the same 12 dB applies to
the stream on port 7076. Playback puts each int16 into the top of both
32-bit slots.

Every 5 s the log shows both slots, which is how a wrong L/R or a dead wire
shows up:

```
I (xxx) audio: mic L -42 dBFS (peak -30), R -96 dBFS (peak -96), slot L -> 10.10.20.5:51234
```

With nothing wired to GPIO7 both slots read a flat -90 to -96 dBFS (the pin
sits at a fixed level, unlike the WROOM's floating GPIO34), so that reading
alone does not tell a missing mic from an unclocked one. With one mic and
the 24 dB gain expect about -26 dBFS RMS in an open office, speech peaks
around -10; with one mic the right slot reads about the same as the left,
because the data line floats during the slot nobody drives, and that is
harmless, only the left slot is used.

## Wake word and VAD

`main/sr.c` runs ESP-SR's audio front end (AFE) in low-cost mode on the
microphone blocks `audio_rx` produces, the same int16 the client gets:
WakeNet 9 with the `wn9_hiesp` model (the phrase is "Hi, ESP") and VADNet
for the speech boundaries, plus the noise suppression and gain control the
AFE enables on its own. One mic and no reference channel today
(`SR_INPUT_FORMAT "M"`), so no echo cancellation: that is stage 7, with the
TX ring as the reference. Detections go to the console:

```
I (xxx) sr: wake #3: "Hi,ESP" (word 1 of model 1, -19 dBFS)
I (xxx) sr: vad: speech at -31 dBFS, 128 ms cached
I (xxx) sr: vad: silence after 1.8 s of speech
I (xxx) sr: 1 wakes (3 since boot), speech 14% of 156 frames, -47 dBFS, ring 94% free
```

The 5 s summary is the stage 5 checkpoint: leave the TV or a podcast on for
an evening and read `N since boot` in the morning; every one of those is a
false accept. The VAD line's hangover is `SR_VAD_MIN_NOISE_MS` (600 ms, the
design note's `speech_end` delay); `vad: speech` needs `SR_VAD_MIN_SPEECH_MS`
of speech above the AFE's energy floor (-60 dBFS by default, printed at
boot), so a mic that reads quieter than that in the `audio: mic` line never
triggers it. The `N ms cached` on a speech edge is the head of the utterance
the detector's delay cut off; the AFE hands it back so stage 6 can send it
uplink first. Nothing acts on a detection yet: no state machine, no face, no
uplink. The cleaned mono chunk the AFE returns is dropped; port 7076 still
carries the raw microphone.

Another phrase is another `CONFIG_SR_WN_*` line in `sdkconfig.defaults`
(the symbol is the model folder in upper case; the list with the phrases is
`managed_components/espressif__esp-sr/wakeword_list.md`), then `rm sdkconfig`,
`idf.py reconfigure`, `idf.py flash`. "Hey Athena" is not in the list; that
is stage 8, a trained model.

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
| `audio.py`: `connection closed by the board`, exit 1 | another client holds port 7076 (the board logs `refused`), or a newer connection took over a stalled one (`gives way to`) |
| `audio.py`: `connection refused`, exit 2 | the listener is not up: `audio unavailable` in the boot log, or a wrong host or port |
| `audio.py` exits 3 with "board sent nothing" | the board accepted the connection but its microphone task is not producing: look for `audio: mic` lines in the log |
| `queued up to N ms, nothing dropped` in the `audio: mic` line | the link stalled that long and the stream caught up: late, but whole |
| `N samples dropped` in the `audio: mic` line | the network took more than about 2.5 s to accept the stream (the queue plus lwIP's send buffer): a Wi-Fi outage or a client that does not read; `audio.py check` shows it from the other end |
| `audio unavailable` in the boot log | the I2S port, the SD_MODE pin or their GPIOs could not be opened; the line names the step |
| `speech front end unavailable: ESP_ERR_NOT_FOUND` | the `model` partition is empty: `idf.py flash`, not `app-flash`, writes `srmodels.bin` |
| `speech front end unavailable: ESP_ERR_NOT_SUPPORTED` | `SR_INPUT_FORMAT` names more channels than `sr_feed()` carries; `sr.c` says which |
| No `wake` line for the phrase | say it as three letters, "Hi, E-S-P", at 1–2 m; the `vad: speech` line's dBFS should be about -20 or louder (the first detection on the bench needed -19; -25 and quieter missed, which is why the mic gain is 24 dB); a `ring N% free` near 0 or `fetch errors` in the summary means the AFE is starved, look at the mic and the dropped count |
| `vad: speech` never appears | the mic is too quiet for the -60 dBFS floor: `AUDIO_MIC_SHIFT` in `audio.c`, or the mic's GAIN; or the mic slot is the unwired one |
| Wakes with nobody speaking | count them on the `since boot` number; the fix is a stricter mode or threshold in `sr.c` (`set_wakenet_threshold`), or the second mic and AEC of the later stages |

## Design notes

- The I2S port is opened once for both directions with one
  `i2s_std_config_t`, which is what makes ESP-IDF pair TX and RX on one BCLK
  and one WS: 16 kHz, 32-bit slots, stereo frame, as `AUDIO-BOARD.md` requires
  for the echo reference later.
- `audio_rx` and `audio_tx` run on core 1, `audio_send` and `audio_net` on
  core 0 with Wi-Fi and lwIP; that is the core split the design note keeps
  when the AFE arrives.
- `sr.c` sits behind a half-second queue rather than feeding the AFE from
  `audio_rx` directly: `afe->feed` copies into the AFE's own ring and can
  wait when the models fall behind, and the microphone read must never wait.
  The AFE's task and both `sr_*` tasks are on core 1 with the I2S pair, the
  AFE one notch above them so the models keep up with the microphone.
- `sdkconfig.defaults` is the design note's starter, including the ESP-SR
  model keys, QIO flash and the bigger caches (the models are read from
  flash in place); the TCP send and receive buffers are raised to 12 MSS
  because lwIP caps a socket near buffer / round-trip time and the stream
  is 32 kB/s.
