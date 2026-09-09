# Assembling Athena: one enclosure, one 5 V supply

Companion to `RGB-MATRIX.md` (the face) and `AUDIO-BOARD.md` (the ears and
mouth). Those two notes describe each board on its own supply. This one puts
the panel, the matrix ESP32-S3, the audio ESP32-S3, the amp, the two mics and
the speaker into one enclosure fed by **one 5 V 8 A adapter through one
cable**. Nothing inside converts voltage; the adapter's 5 V is the only rail.

Status: design note, nothing assembled yet. Prices and Almaty listings for
every part are in `Athena_audio_board_parts_Almaty.xlsx` (sheets "Shopping
list" and "One-cable power"). Where this note and a board note disagree on
power, this note wins for the boxed build; the board notes still describe the
bench setup with separate supplies.

## What goes in the box

| Item | Detail | Defined in |
|---|---|---|
| Panel | Waveshare RGB-Matrix-P3 64×64, HUB75E, VH4 power lead | `RGB-MATRIX.md` |
| Matrix MCU | ESP32-S3-DevKitC-1 N16R8, ribbon on J1 per the pin map | `RGB-MATRIX.md` |
| Audio MCU | ESP32-S3-DevKitC-1 N16R8, I2S on GPIO 5/6/7/15, amp SD on 16 | `AUDIO-BOARD.md` |
| Mics | 2× ICS-43434 (ozon order) or 2× INMP441 (Almaty stock) | `AUDIO-BOARD.md` |
| Amp + speaker | MAX98357A, 4 Ω 3 W driver in a sealed chamber | `AUDIO-BOARD.md` |
| Supply | 5 V 8 A wall adapter, 5.5×2.1 barrel, centre positive | this note |
| Power entry | DC-022 panel-mount jack, two WAGO 221-415 as the 5 V bus, bulk caps | this note |

The two boards do not talk to each other. The matrix listens on
`athena-matrix.local:7075`; the audio board dials the hub. Sharing a supply
changes nothing in firmware or scripts.

## The short version

Adapter into the jack, jack into port 1 of each clamp, one wire pair per
consumer into ports 2–5. That is the entire power distribution; there is no
distribution board, nothing is soldered, and the clamps are the "board"
people imagine they need. The signals are separate wiring and do not change:
the ribbon from the matrix DevKit to the panel, the four I2S wires to the
mics and the four wires to the amp are exactly as the two board notes say.

Five things still have to be right before "everything works":

1. **Polarity, once, with a meter.** The adapter's centre pin is + and goes to
   the + clamp. A reversed barrel kills the panel.
2. **Adapter first, then USB.** Never let a DevKit on USB drive the ribbon
   into an unpowered panel.
3. **One measurement.** Run `--demo` and a white `test` frame at brightness
   255 with an inline ammeter on the barrel, then set the brightness cap so
   the total stays under 8 A. Until then keep brightness at 40.
4. **One check per clone.** With the bus off and only USB in, the 5V pin
   reads about 4.7 V. A full 5.0 V means no diode: unplug the adapter while
   flashing that board.
5. **Firmware.** The face works today. `firmware/athena_audio/` does not
   exist yet, so the audio DevKit powers up and does nothing until stage 1 of
   `AUDIO-BOARD.md` is written.

## Power budget

| Consumer | Worst case | Typical | Note |
|---|---|---|---|
| Panel | 4.0 A | 1–1.5 A | full white at brightness 255 is the 4 A; the aura never draws it |
| Matrix DevKit | 0.5 A | 0.25 A | Wi-Fi TX bursts |
| Audio DevKit | 0.5 A | 0.25 A | |
| MAX98357A + speaker | 1.0 A | 0.2 A | 3.2 W into 4 Ω at 5 V; voice averages far less |
| 2× mics | 0.01 A | | |
| **Total** | **≈ 6 A** | **≈ 2 A** | |

So the adapter is **5 V 8 A (40 W)**. A 6 A adapter also works if the
firmware caps panel brightness around 150 of 255; anything smaller browns out
the panel on a white burst, and Waveshare notes its inputs misbehave when the
supply sags. Measure the real draw once (see Bring-up) and set the cap from
the measurement, not from this table.

## Power tree

```
230 V  →  5 V 8 A adapter (5.5×2.1, centre +)  →  DC-022 jack on the back wall  →  5 V bus: two WAGO 221-415 lever connectors, one for +, one for −
                                                                                    ├─ panel VH4 lead, 18 AWG, ≤ 15 cm       (+ 1000 µF only if white bursts flicker)
                                                                                    ├─ matrix DevKit 5V + GND pins, 22 AWG
                                                                                    ├─ audio DevKit 5V + GND pins, 22 AWG
                                                                                    └─ amp VIN + GND, 22 AWG, twisted         (+ 1000 µF at the amp)
```

The bus is two five-way lever connectors (WAGO 221-415, 32 A, 0.2–4 mm²,
stranded wire accepted): one for +5 V, one for GND. Port 1 takes the wire
from the jack, ports 2–5 the panel, the matrix DevKit, the audio DevKit and
the amp. Alternatives: one SPL-82 push-in block (two poles, one in and four
out each) with tinned or ferruled ends, or a 12-way 20 A screw barrier strip
with five poles bridged for each rail. Not PCB screw terminals (KF301,
about 10 A) for the panel branch, and not a CCTV barrel splitter cable,
whose wires are too thin for the panel.

Where the two ESP32s connect, each on its own pair from the clamps:

| From the bus | To the DevKitC-1 | Where the pin is |
|---|---|---|
| + clamp, one port | pin marked **5V** (VIN on some clones) | bottom of the J1 header, the column that starts with 3V3 |
| − clamp, one port | pin marked **GND** | the last pin of J1, right under 5V |

The 5V pin feeds the board's own regulator, which makes the 3.3 V for the
chip. The matrix DevKit then feeds nothing but ribbon signals; the panel's
power is the bus. The audio DevKit feeds only the two mics from its 3V3 pin;
the amp's VIN is the bus. The USB-C connectors stay empty in normal use.

Rules that make this work:

- **Star, not chain.** Every consumer gets its own wire pair from the bus.
  Never route the panel's current through a DevKit, and never take the amp's
  5 V from the DevKit's 5V pin in the boxed build (the DevKit's diode would
  drop it and the pin's trace is not sized for it).
- **One ground.** Panel GND, both DevKit GNDs, amp GND and the jack sleeve all
  meet at the bus. The ribbon's GND pins still tie the matrix DevKit to the
  panel, as `RGB-MATRIX.md` requires; that is fine, it is the same ground.
- **Caps where the current spikes.** 1000 µF 16 V across the amp's VIN/GND,
  within a few centimetres of the amp. A second 1000 µF across the panel's
  VH4 lead only if the aura flickers on bright frames. 100 nF at each mic VDD
  if the mic wires exceed 15 cm (the INMP441 breakouts already carry one).
- **Wire gauge.** 18 AWG for the panel lead (it ships with the panel; shorten
  it rather than extend it), 22 AWG for the boards and the amp.
- **Optional fuse.** A 6.3 A polyfuse or an 8 A blade fuse in the + lead
  between the jack and the bus. The adapter has its own protection; the fuse
  is for a shorted wire inside the box.

## The jack and the barrel

A 5.5×2.1 barrel plug plus a DC-022 socket carries about 5 A continuously,
which covers the 2 A typical and short 6 A peaks. If that margin bothers you,
cut the plug off the adapter, pass the cable through a grommet in the back
wall and land the two wires straight on the bus terminals; the adapter is
then captive, but nothing limits the current. The DC-022 has three lugs:
centre pin, sleeve, and a switch contact that opens when a plug is inserted.
Use centre for +, sleeve for −, leave the third lug unconnected.

Check polarity with a meter before anything is wired: centre pin **+5.0 to
5.2 V** against the sleeve. Waveshare's warning that any voltage other than
5 V burns the panel applies to reversed polarity too.

## USB and the 5V pin

Both DevKits keep their UART USB-C connector for flashing and the boot
console. The ESP32-S3-DevKitC-1 has a Schottky diode between USB VBUS and the
5V pin, so the Mac's port and the bus never fight: with the bus on, the diode
is reverse-biased and the Mac sees no load; with the bus off, USB powers the
DevKit alone.

Verify the diode once on each clone: with only USB plugged in and the bus
off, the 5V pin reads about 4.7 V (one diode drop below VBUS). If it reads a
full 5.0 V, assume there is no diode and unplug the adapter while flashing.

With the bus off and USB in, the matrix DevKit drives 3.3 V signals into an
unpowered panel. `RGB-MATRIX.md` orders it panel PSU first, then USB; keep
that order here: **adapter first, then USB**, or unplug the ribbon.

Never put 5 V on a 3V3 pin. The mics live on 3V3 from the DevKit's own LDO.

## Wiring, step by step

1. Mount the DC-022 in the back wall. Wire centre → port 1 of the + WAGO,
   sleeve → port 1 of the − WAGO, with the fuse in the + lead if you use
   one. Stick both connectors to the wall next to the jack.
2. Meter the adapter: centre + 5.0–5.2 V. Plug it into the jack, meter the
   bus: same reading, − to sleeve is 0 Ω. Unplug.
3. Panel: shorten the VH4 lead to reach the bus, red to +, black to −. The
   lead has two reds and two blacks; twist each pair together and put it in
   one WAGO port (two 18 AWG strands are 1.6 mm², inside the 4 mm² limit),
   so each rail uses exactly five ports: jack, panel, matrix, audio, amp.
4. Matrix DevKit: 22 AWG pair from the bus to the **5V** and **GND** pins at
   the bottom of the J1 header (the header that starts with 3V3, as in
   `RGB-MATRIX.md`). Ribbon on the panel's IN header and on J1 per the pin
   map.
5. Audio DevKit: the same pair to its 5V and GND pins.
6. Amp: 22 AWG twisted pair from the bus to VIN and GND, 1000 µF across the
   amp's VIN/GND with the stripe on GND. Speaker on the screw terminal. Keep
   this pair and the speaker leads on the far side of the box from the I2S
   wires.
7. Mics and I2S per `AUDIO-BOARD.md`: BCLK 5, WS 6, DIN 7, DOUT 15, SD 16;
   four mic wires under 15 cm, twisted, routed away from the ribbon and the
   speaker leads. HUB75 PWM and the class-D output both couple into MEMS mics
   as hiss.
8. Ground check with a meter: continuity between panel GND (either ribbon GND
   pin), both DevKit GNDs, amp GND and the jack sleeve. All must read 0 Ω.
9. Short check: resistance from bus + to bus − is not 0 Ω (the panel and the
   boards read a few hundred ohms or more with everything off).

Everything is plugged and screwed, nothing soldered except the jack, the
speaker leads and the mic wires if the breakouts have no headers.

## Mechanical layout

- **Panel** on the front face, 192×192 mm, on its own standoffs. Ribbon
  ≤ 30 cm, shorter is better; 15 cm is comfortable with the boards mounted
  directly behind the panel.
- **Mics** on the front face below the panel, 4–6.5 cm apart on a horizontal
  axis, centred. Each port through a 1.5 mm hole in a wall of at most 3 mm,
  sealed to the breakout with a foam gasket, dust mesh outside. Rubber
  standoffs, not screws straight into the wall.
- **Speaker** in its own sealed chamber, as far from the mics as the box
  allows (bottom or a side wall), driver decoupled with rubber, chamber
  airtight so the back wave never reaches the mic ports. A 40 mm driver is
  happy in 100–200 cm³.
- **Boards** behind the panel on standoffs, USB-C connectors facing the back
  wall so both can be reached for flashing without opening the box. Amp next
  to the speaker chamber, bus terminals next to the jack.
- **Back wall**: DC jack, two USB-C cut-outs, ventilation slots near the top.
  The panel dissipates up to 20 W at full white; the adapter stays outside
  and adds no heat.
- Sound pressure at the mics must stay ≤ 102 dB at maximum volume; cap amp
  volume in software rather than moving the mics.

## Bring-up

1. Bus wired, ribbon on, nothing on USB. Adapter in. The matrix boots into
   `test`; the audio board shows its RGB LED.
2. From the Mac: `./scripts/face.py --host athena-matrix.local --ping`
   answers `ok`. Then `--brightness 40`, then `--demo`.
3. Measure the draw with an inline meter on the barrel (a USB-C meter does
   not fit here; a cheap DC inline ammeter or a bench supply with a readout
   does): at `idle`, at `--demo`, and at `test` with `--brightness 255`.
   Record all three in this file. Set the firmware brightness cap so the
   panel plus 2 A for the rest stays under the adapter's rating.
4. Plug USB into the matrix DevKit with the bus on; confirm the 5V pin still
   reads bus voltage and nothing warms up. Repeat for the audio DevKit.
5. Audio stages 1–4 of `AUDIO-BOARD.md` with the panel running the demo.
   Stage 4 (raw dump, noise floor with the matrix on and off) is where a
   shared supply shows its problems: hiss that follows the aura means the
   panel's current spikes reach the mics through the rail or through
   coupling. Fixes, in order: the 1000 µF on the panel lead, shorter mic
   wires, moving the I2S wires away from the ribbon, a 100 nF at each mic.
6. Only then the speaker at full volume with the panel on white: the bus must
   not sag below 4.8 V at the amp.

## Alternative: a USB-C socket on the box

A plain 5 V USB-C source without Power Delivery negotiation supplies at most
3 A, enough for the audio board alone, not for the panel. The Raspberry Pi 5
supply's 5 A mode exists only after PD negotiation, which neither ESP32 does.
So a USB-C socket only works with a converter inside:

```
65 W USB-C PD charger  →  PD trigger cable set to 12 V  →  DC-DC buck 9 A set to 5.0 V  →  the same bus
```

Parts (Alash Electronics): PD trigger cable 12 V (500 ₸, under order), buck
5–40 V → 1.25–35 V, 300 W, 9 A (1 950 ₸). Set the buck to 5.00–5.10 V with
nothing connected, then wire the bus; the panel dies above about 5.5 V and
the buck has no reverse-polarity protection. This costs about twice the
barrel adapter and adds a trim pot that can be mis-set. Choose it only if a
USB-C socket and an interchangeable laptop charger matter.

## Do not

- Feed the panel anything but 5 V; never reverse the barrel.
- Plug or unplug the HUB75 ribbon with the adapter on.
- Chain the panel's or the amp's 5 V through a DevKit.
- Put 5 V on 3V3.
- Run a DevKit on USB with the bus off while the ribbon is on the panel.
- Size the supply below 6 A while the amp is in the box.

## What this changes in the other notes

- `RGB-MATRIX.md` "Power": the DevKit is no longer powered by the Mac over
  UART and the panel no longer has its own supply; both take 5 V from the
  bus, USB is data only, and the power-up order becomes adapter first, then
  USB.
- `AUDIO-BOARD.md` "Parts", Power row: the shared 5 V rail option is this
  note; the USB-C adapter option remains the bench setup.
- Scripts and firmware: unchanged.
