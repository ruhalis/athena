#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["sounddevice>=0.5", "websockets>=14", "numpy>=1.26"]
# ///
"""Talk to Athena by voice from this Mac and mirror the conversation on the LED face.

Mac microphone -> OpenAI Realtime API (gpt-realtime-2.x over WebSocket, PCM16 at
24 kHz both ways) -> Mac speakers, while each turn of the conversation is sent
to the matrix board as one of scripts/face.py's states:

    connected, nothing happening         idle    (clock in `t`, refreshed every minute)
    you start talking                    listen
    you stop talking, reply in flight    think
    first audio of the reply             speak   (kept alive until playback drains)
    reply played to the end              idle
    API error, dropped connection        error   (board returns to idle after 10 s; we reconnect)
    Ctrl-C, SIGTERM, --stop              idle

She waits for you to speak first (--greet makes her say hello once connected)
and hears and speaks one language, Russian unless --language says otherwise
(`auto` follows the speaker): the code goes to the transcriber and a sentence
onto the end of the system prompt.

Half-duplex by default: while the reply plays (plus a short tail) microphone
audio is not sent, because the MacBook's microphone hears its own speakers and
the server's voice detection would take the reply for you interrupting it.
--barge-in sends the microphone all the time; use it with headphones.

--audio board takes the microphone and the speaker of the audio board instead
(firmware/athena_audio over Wi-Fi: one TCP connection, raw s16le 16 kHz mono
both ways, resampled here to the API's 24 kHz and back). It is found the way
scripts/audio.py finds it (--audio-host, ATHENA_AUDIO_HOST, else
athena-audio.local) and serves one client, so the voice station on the mini or
an audio.py run must not hold it. The board has no echo cancellation yet, its
microphone sits next to its speaker, so it stays half-duplex with a longer
tail; --gain sets how loud its speaker is.

Runs under `uv run --script` (see the shebang): the audio and WebSocket
libraries live in an ephemeral environment, nothing is installed into the
system Python. scripts/face.py (standard library) is imported for the board,
which is found the way face.py finds it (--face-host, ATHENA_MATRIX_HOST, a
board on USB, else athena-matrix.local). OPENAI_API_KEY comes from the
environment, else from <repo>/.env, else from ~/.hermes/.env. The board serves
several connections over Wi-Fi, so this runs alongside a Hermes gateway.

    scripts/voice.py                      # talk; Ctrl-C to stop
    scripts/voice.py --voice cedar --vad semantic
    scripts/voice.py --voice cedar --audio board   # the audio board's mic and speaker over Wi-Fi
    scripts/voice.py --list-devices
    scripts/voice.py --no-face            # without the board
    scripts/voice.py --stop               # stop a copy started in the background
"""

import argparse
import asyncio
import base64
import json
import os
import signal
import socket
import sys
import threading
import time
from pathlib import Path

import numpy as np
import sounddevice as sd
import websockets

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import face as face_mod  # noqa: E402  scripts/face.py: FaceLink (the board on its own thread)
import audio as audio_mod  # noqa: E402  scripts/audio.py: the audio board's address and socket

RATE = 24000                     # the Realtime API's PCM rate, both directions
BLOCK_MS = 100                   # one microphone chunk / one playback block
BOARD_LEAD_S = 0.3               # how far ahead of its speaker the audio board is fed (it buffers 0.5 s)
BOARD_OUT_LATENCY_S = 0.1        # the link plus the board's 60 ms of I2S DMA: the speaker ends this long after the clock says
BOARD_SILENT_S = 6.0             # the board streams its mic without pause; this long without a byte is a lost board
BOARD_RETRY_S = 3.0
BOARD_TAIL_S = 0.5               # default --tail for the board: its mic is next to its speaker and arrives over Wi-Fi
DEFAULT_BOARD_GAIN_DB = 6.0      # the station's figure for the same speaker; more clips harder
DEFAULT_MODEL = "gpt-realtime-2.1"
DEFAULT_VOICE = "marin"
VOICES = ("alloy", "ash", "ballad", "coral", "echo", "sage", "shimmer", "verse", "marin", "cedar")
DEFAULT_TRANSCRIBE = "gpt-4o-mini-transcribe"
PIDFILE = Path.home() / ".hermes" / "athena-voice.pid"
DEFAULT_INSTRUCTIONS = (
    "You are Athena, a voice assistant that lives on a desk with a small LED face, "
    "talking through %s. Keep replies short and "
    "conversational: one to three sentences unless the user asks for detail. "
    "If you did not catch something, say so briefly."
)
TALKING_THROUGH = {"mac": "a MacBook's microphone and speakers",
                   "board": "the microphone and the small speaker of your own box"}
DEFAULT_LANGUAGE = "ru"          # the one language of the conversation; `auto` follows the speaker
LANGUAGES = {"ru": "Russian", "en": "English", "kk": "Kazakh"}
ONE_LANGUAGE = ("Speak only %(name)s. The user speaks %(name)s: take everything you hear as %(name)s, even a "
                "word that sounds like another language, and always answer in %(name)s.")
ANY_LANGUAGE = "Answer in the language the user speaks."
DEFAULT_GREETING = "Greet the user in one short sentence: say you are Athena and that you are listening."


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def load_api_key():
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if key:
        return key
    for path in (REPO / ".env", Path.home() / ".hermes" / ".env"):
        try:
            lines = path.read_text().splitlines()
        except OSError:
            continue
        for line in lines:
            line = line.strip()
            if line.startswith("export "):
                line = line[7:].lstrip()
            if line.startswith("OPENAI_API_KEY="):
                val = line.split("=", 1)[1].strip().strip('"').strip("'")
                if val:
                    return val
    return None


# ---------------------------------------------------------------------------- audio

class Audio:
    """Microphone in, speakers out, PCM16 mono at RATE. A device that will not open at
    RATE is opened at 48 kHz and bridged by averaging pairs (in) or repeating samples
    (out). The input callback hands chunks to an asyncio queue; the output callback
    drains a byte buffer and pads with silence."""

    def __init__(self, in_dev=None, out_dev=None):
        self.in_dev, self.out_dev = in_dev, out_dev
        self.loop = self.aq = None
        self.in_rate = self.out_rate = RATE
        self.in_stream = self.out_stream = None
        self.in_peak = 0                      # loudest sample seen, for the silent-mic warning
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._had_audio = False
        self.drained_at = 0.0                 # monotonic time the playback buffer last ran dry

    def start(self, loop, aq):
        self.loop, self.aq = loop, aq
        self.in_stream = self._open(sd.RawInputStream, "in")
        self.out_stream = self._open(sd.RawOutputStream, "out")
        self.in_stream.start()
        self.out_stream.start()

    def _open(self, cls, kind):
        dev = self.in_dev if kind == "in" else self.out_dev
        cb = self._in_cb if kind == "in" else self._out_cb
        last = None
        for rate in (RATE, 48000):
            try:
                stream = cls(samplerate=rate, blocksize=rate * BLOCK_MS // 1000, device=dev,
                             channels=1, dtype="int16", callback=cb)
            except Exception as e:                       # PortAudioError or a bad device spec
                last = e
                continue
            if kind == "in":
                self.in_rate = rate
            else:
                self.out_rate = rate
            return stream
        raise SystemExit("cannot open the %sput device: %s (try --list-devices)" % (kind, last))

    silent_hint = ("If macOS never asked, allow the microphone for this terminal app in System "
                   "Settings > Privacy & Security > Microphone, then start again.")

    def name(self, kind):
        stream = self.in_stream if kind == "in" else self.out_stream
        return sd.query_devices(stream.device)["name"]

    def describe(self):
        return "mic: %s @ %d Hz   speakers: %s @ %d Hz" % (self.name("in"), self.in_rate, self.name("out"), self.out_rate)

    def _in_cb(self, indata, frames, t, status):
        data = bytes(indata)
        if self.in_rate != RATE:
            f = self.in_rate // RATE
            a = np.frombuffer(data, np.int16)
            data = a[: len(a) // f * f].reshape(-1, f).mean(axis=1).astype(np.int16).tobytes()
        if data:
            peak = int(np.abs(np.frombuffer(data, np.int16)).max())
            if peak > self.in_peak:
                self.in_peak = peak
        if self.loop is not None:
            self.loop.call_soon_threadsafe(self._put, data)

    def _put(self, data):
        try:
            self.aq.put_nowait(data)
        except asyncio.QueueFull:
            pass                                          # the socket is behind; drop rather than lag

    def _out_cb(self, outdata, frames, t, status):
        f = self.out_rate // RATE
        need = (frames // f) * 2
        with self._lock:
            chunk = bytes(self._buf[:need])
            del self._buf[:need]
            drained = self._had_audio and not self._buf
            if drained:
                self._had_audio = False
        if drained:
            self.drained_at = time.monotonic()
        if len(chunk) < need:
            chunk += b"\0" * (need - len(chunk))
        if f > 1:
            chunk = np.repeat(np.frombuffer(chunk, np.int16), f).tobytes()
        want = frames * 2
        if len(chunk) < want:
            chunk += b"\0" * (want - len(chunk))
        outdata[:] = chunk[:want]

    def play(self, pcm):
        with self._lock:
            self._buf += pcm
            self._had_audio = True

    def clear(self):
        with self._lock:
            self._buf.clear()

    def playing(self):
        with self._lock:
            return bool(self._buf)

    def stop(self):
        for s in (self.in_stream, self.out_stream):
            if s is not None:
                try:
                    s.stop()
                    s.close()
                except Exception:
                    pass


class Resampler:
    """Streaming rational resampler for PCM16 mono: zero-stuff by `up`, a windowed-sinc
    low-pass just under the lower of the two Nyquists, keep every `down`th sample. The
    filter history and the decimation phase carry across chunks, so a seam is inaudible."""

    def __init__(self, up, down, zeros=24):
        self.up, self.down = up, down
        widest = max(up, down)
        n = 2 * zeros * widest + 1
        k = np.arange(n) - (n - 1) / 2.0
        h = np.sinc(0.9 / widest * k) * np.hamming(n)        # cutoff in Nyquists of the stuffed rate
        self.h = (h * up / h.sum()).astype(np.float32)
        self.hist = np.zeros(n - 1, np.float32)
        self.phase = 0

    def process(self, pcm, gain=1.0):
        """Whole int16 samples in -> whole int16 samples out, `gain` applied, clipped at full scale."""
        x = np.frombuffer(pcm, np.int16)
        if not len(x):
            return b""
        z = np.zeros(len(x) * self.up, np.float32)
        z[::self.up] = x
        buf = np.concatenate((self.hist, z))
        y = np.convolve(buf, self.h, mode="valid")           # one output per stuffed sample
        self.hist = buf[-(len(self.h) - 1):]
        out = y[self.phase::self.down]
        self.phase = (self.phase - len(y)) % self.down
        return np.clip(np.rint(out * gain), -32768, 32767).astype(np.int16).tobytes()


class BoardAudio:
    """The audio board's microphone and speaker in place of the Mac's, with Audio's surface.
    One TCP connection (scripts/audio.py has the protocol): the board streams its microphone
    at 16 kHz all the time and plays every byte written to it. `board-mic` owns the socket:
    it reads without pause, so the board never queues for us, hands BLOCK_MS blocks at RATE
    to the asyncio queue, and reconnects a lost board. `board-speaker` feeds the reply paced
    to real time, BOARD_LEAD_S ahead: the board cannot take audio back, so that lead is what
    still plays after clear(), and the playback clock it keeps (_play_end) is what playing()
    and drained_at answer from, since the Mac cannot hear the board's speaker run dry."""

    silent_hint = "Look at the board's `audio: mic` log line and at `scripts/audio.py meter`."

    def __init__(self, host, port, gain_db=0.0, on_lost=None):
        self.host, self.port = host, port
        self.gain_db, self.gain = gain_db, 10.0 ** (gain_db / 20.0)
        self.on_lost = on_lost
        self.loop = self.aq = None
        self.in_peak = 0
        self._sock = None
        self._buf = bytearray()                   # the reply at 16 kHz, not yet sent
        self._odd = b""                           # half a sample from a delta that ended mid-sample
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._closing = threading.Event()
        self._play_end = 0.0                      # monotonic time the board's speaker finishes what was sent
        self._to_api = Resampler(RATE // 8000, audio_mod.SAMPLE_RATE // 8000)
        self._to_board = Resampler(audio_mod.SAMPLE_RATE // 8000, RATE // 8000)

    def start(self, loop, aq):
        self.loop, self.aq = loop, aq
        try:
            sock, first = self._connect()
        except audio_mod.AudioError as e:
            raise SystemExit("error: audio board: %s" % e)
        self._sock = sock
        threading.Thread(target=self._rx, args=(sock, first), name="board-mic", daemon=True).start()
        threading.Thread(target=self._tx, name="board-speaker", daemon=True).start()

    def describe(self):
        return "mic and speaker: the audio board at %s:%d, %d Hz <-> %d Hz, speaker gain %+.0f dB" % (
            self.host, self.port, audio_mod.SAMPLE_RATE, RATE, self.gain_db)

    def _connect(self):
        """-> (socket, its first microphone bytes). The board accepts a second client only to
        close it at once, so the first bytes are the proof that this connection is the one."""
        sock = audio_mod.connect(self.host, self.port)
        try:
            first = sock.recv(4096)
        except OSError as e:
            sock.close()
            raise audio_mod.AudioError("%s:%d accepted the connection but sent no microphone: %s" % (self.host, self.port, e))
        if not first:
            sock.close()
            raise audio_mod.AudioError("%s:%d closed the connection at once: another client already streams (the voice "
                                       "station on the mini, a scripts/audio.py run) and the board serves one" % (self.host, self.port))
        return sock, first

    def _rx(self, sock, data):
        block = audio_mod.BYTES_PER_SEC * BLOCK_MS // 1000
        pending = bytearray(data)
        heard = time.monotonic()
        problem = None                            # the last failure logged, so a board that stays away logs once
        while not self._closing.is_set():
            try:
                if sock is None:
                    sock, data = self._connect()
                    self._sock = sock
                    pending = bytearray(data)
                    heard = time.monotonic()
                    log("audio board: connected again")
                    problem = None
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    if time.monotonic() - heard > BOARD_SILENT_S:
                        raise OSError("no microphone for %.0f s" % BOARD_SILENT_S)
                    continue
                if not data:
                    raise OSError("connection closed by the board")
                heard = time.monotonic()
                pending += data
                while len(pending) >= block:
                    pcm = bytes(pending[:block])
                    del pending[:block]
                    peak = int(np.abs(np.frombuffer(pcm, np.int16).astype(np.int32)).max())
                    if peak > self.in_peak:
                        self.in_peak = peak
                    self.loop.call_soon_threadsafe(self._put, self._to_api.process(pcm))
            except RuntimeError:
                return                                    # the event loop closed under us: we are stopping
            except (OSError, audio_mod.AudioError) as e:
                if sock is not None:
                    self._sock = None
                    sock.close()
                    sock = None
                if self._closing.is_set():
                    return
                if problem is None and self.on_lost:
                    self.on_lost()
                if str(e) != problem:
                    problem = str(e)
                    log("audio board lost: %s; trying again every %.0f s" % (problem, BOARD_RETRY_S))
                self._closing.wait(BOARD_RETRY_S)

    def _put(self, data):
        try:
            self.aq.put_nowait(data)
        except asyncio.QueueFull:
            pass                                          # the socket is behind; drop rather than lag

    def _tx(self):
        while not self._closing.is_set():
            sock = self._sock
            with self._lock:
                if sock is None:
                    self._buf.clear()                     # no board to say it to: a reply never waits for one
                chunk = bytes(self._buf[:audio_mod.CHUNK_BYTES])
                del self._buf[:audio_mod.CHUNK_BYTES]
                if chunk:
                    # A speaker that ran dry starts this run now; otherwise the chunk queues behind the last.
                    self._play_end = max(self._play_end, time.monotonic()) + len(chunk) / float(audio_mod.BYTES_PER_SEC)
            if not chunk:
                self._wake.wait(0.05)
                self._wake.clear()
                continue
            try:
                sock.sendall(chunk)
            except OSError:
                try:
                    sock.shutdown(socket.SHUT_RDWR)       # wakes board-mic, which reconnects
                except OSError:
                    pass
                continue
            ahead = self._play_end - time.monotonic()
            if ahead > BOARD_LEAD_S:
                self._closing.wait(ahead - BOARD_LEAD_S)

    def play(self, pcm):
        pcm = self._odd + pcm
        pcm, self._odd = (pcm[:-1], pcm[-1:]) if len(pcm) % 2 else (pcm, b"")
        out = self._to_board.process(pcm, self.gain)
        with self._lock:
            self._buf += out
        self._wake.set()

    def clear(self):
        with self._lock:
            self._buf.clear()
        self._odd = b""

    def playing(self):
        with self._lock:
            return bool(self._buf) or time.monotonic() < self._play_end + BOARD_OUT_LATENCY_S

    @property
    def drained_at(self):
        return self._play_end + BOARD_OUT_LATENCY_S

    def stop(self):
        self._closing.set()
        self._wake.set()
        sock = self._sock
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


# -------------------------------------------------------------------------- realtime

class State:
    def __init__(self):
        self.ready = self.greeted = self.speaking = self.await_drain = False
        self.transcript = []


def session_update(args):
    if args.vad == "semantic":
        turn = {"type": "semantic_vad", "eagerness": "auto", "create_response": True, "interrupt_response": True}
    else:
        turn = {"type": "server_vad", "threshold": 0.5, "prefix_padding_ms": 300,
                "silence_duration_ms": args.silence_ms, "create_response": True, "interrupt_response": True}
    return {
        "type": "session.update",
        "session": {
            "type": "realtime",
            "output_modalities": ["audio"],
            "instructions": args.instructions,
            "audio": {
                "input": {
                    "format": {"type": "audio/pcm", "rate": RATE},
                    # The board's bare microphone hears you across the desk, the MacBook's from the keyboard.
                    "noise_reduction": {"type": "far_field" if args.audio == "board" else "near_field"},
                    "transcription": dict({"model": args.transcribe}, **({"language": args.language} if args.language else {})),
                    "turn_detection": turn,
                },
                "output": {
                    "format": {"type": "audio/pcm", "rate": RATE},
                    "voice": args.voice,
                    "speed": args.speed,
                },
            },
        },
    }


async def pump_mic(ws, audio, aq, st, args):
    """Microphone chunks -> input_audio_buffer.append, gated while the reply plays."""
    started, warned = time.monotonic(), False
    while True:
        data = await aq.get()
        now = time.monotonic()
        if not warned and now - started > 5.0:
            warned = True
            if audio.in_peak < 30:
                log("WARNING: the microphone has been silent for 5 s. " + audio.silent_hint)
        if not args.barge_in and (audio.playing() or now - audio.drained_at < args.tail):
            continue                                      # half-duplex: do not feed our own voice back
        await ws.send(json.dumps({"type": "input_audio_buffer.append",
                                  "audio": base64.b64encode(data).decode("ascii")}))


async def watch_playback(audio, st, face):
    """After response.done, wait for the speakers to drain, then the face goes idle."""
    while True:
        await asyncio.sleep(0.1)
        if st.await_drain and not audio.playing():
            st.await_drain = st.speaking = False
            face.push("idle")


async def handle(ev, ws, audio, st, face, args):
    t = ev.get("type", "")
    if t == "session.created":
        log("session %s" % ev.get("session", {}).get("id", ""))
    elif t == "session.updated":
        if not st.ready:
            st.ready = True
            face.push("idle")
            log("ready: listening on the microphone")
            if args.greet and not st.greeted:
                st.greeted = True
                await ws.send(json.dumps({"type": "response.create",
                                          "response": {"instructions": args.greeting}}))
    elif t == "input_audio_buffer.speech_started":
        # The server cancels the reply in flight (interrupt_response); drop what we have not
        # played yet. Half-duplex only lets speech through before playback starts, so this
        # fires there too, on the mic audio that was still in flight when the reply began.
        if audio.playing() or st.speaking:
            audio.clear()
            st.await_drain = st.speaking = False
        face.push("listen")
    elif t == "input_audio_buffer.speech_stopped":
        face.push("think")
    elif t == "response.created":
        st.speaking = st.await_drain = False
        st.transcript = []
        face.push("think")
    elif t in ("response.output_audio.delta", "response.audio.delta"):
        audio.play(base64.b64decode(ev["delta"]))
        if not st.speaking:
            st.speaking = True
            face.push("speak")
    elif t in ("response.output_audio_transcript.delta", "response.audio_transcript.delta"):
        st.transcript.append(ev.get("delta", ""))
    elif t in ("response.output_audio_transcript.done", "response.audio_transcript.done"):
        log("athena: " + (ev.get("transcript") or "".join(st.transcript)).strip())
    elif t == "conversation.item.input_audio_transcription.completed":
        log("you: " + (ev.get("transcript") or "").strip())
    elif t == "response.done":
        r = ev.get("response", {})
        status = r.get("status")
        if status in ("failed", "incomplete", "cancelled"):
            det = r.get("status_details") or {}
            reason = (det.get("error") or {}).get("message") or det.get("reason") or status
            log("response %s: %s" % (status, reason))
            if status == "failed":
                face.push("error")                        # FaceLink returns it to idle after 10 s
                return
            if status == "cancelled":
                audio.clear()                             # never finish a reply the server cut
        st.await_drain = True                             # idle once the speakers are quiet
    elif t == "error":
        e = ev.get("error", {})
        log("API error: %s (%s)" % (e.get("message"), e.get("code") or e.get("type")))
        face.push("error")


async def reader(ws, audio, st, face, args):
    async for raw in ws:
        try:
            ev = json.loads(raw)
        except ValueError:
            continue
        await handle(ev, ws, audio, st, face, args)


async def run(args, key, face):
    loop = asyncio.get_running_loop()
    aq = asyncio.Queue(maxsize=50)
    if args.audio == "board":
        audio = BoardAudio(*args.audio_target, gain_db=args.gain, on_lost=lambda: face.push("error"))
    else:
        audio = Audio(args.input, args.output)
    audio.start(loop, aq)
    log(audio.describe())
    st = State()
    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    url = "wss://api.openai.com/v1/realtime?model=" + args.model
    headers = {"Authorization": "Bearer " + key}
    attempt = 0
    try:
        while not stop.is_set():
            try:
                log("connecting to %s ..." % args.model)
                async with websockets.connect(url, additional_headers=headers, max_size=None,
                                              ping_interval=20, ping_timeout=20, open_timeout=20) as ws:
                    attempt = 0
                    await ws.send(json.dumps(session_update(args)))
                    tasks = {
                        "reader": asyncio.create_task(reader(ws, audio, st, face, args)),
                        "mic": asyncio.create_task(pump_mic(ws, audio, aq, st, args)),
                        "watch": asyncio.create_task(watch_playback(audio, st, face)),
                        "stop": asyncio.create_task(stop.wait()),
                    }
                    done, pending = await asyncio.wait(tasks.values(), return_when=asyncio.FIRST_COMPLETED)
                    for p in pending:
                        p.cancel()
                    for d in done:
                        d.result()                        # re-raise a reader/mic failure
                    if tasks["reader"] in done and not stop.is_set():
                        raise ConnectionError("the server closed the session")
            except websockets.InvalidStatus as e:
                log("connection refused: HTTP %s (bad API key, model name, or no Realtime access?)"
                    % e.response.status_code)
                face.push("error")
                break
            except (websockets.ConnectionClosed, websockets.InvalidHandshake, OSError, asyncio.TimeoutError) as e:
                if stop.is_set():
                    break
                attempt += 1
                audio.clear()
                st.ready = st.speaking = st.await_drain = False
                face.push("error")
                if attempt > args.max_retries:
                    log("giving up after %d reconnects: %r" % (attempt - 1, e))
                    break
                delay = min(30, 2 ** attempt)
                log("connection lost (%s); reconnecting in %d s" % (e, delay))
                try:
                    await asyncio.wait_for(stop.wait(), delay)
                except asyncio.TimeoutError:
                    pass
    finally:
        audio.stop()


# ------------------------------------------------------------------------------ cli

def audio_board_target(spec):
    """--audio-host, else ATHENA_AUDIO_HOST, else the board's mDNS name: the way scripts/audio.py finds it."""
    spec = spec or os.environ.get("ATHENA_AUDIO_HOST")
    host, port = audio_mod.parse_hostport(spec) if spec else (audio_mod.DEFAULT_HOST, None)
    return host, port or audio_mod.DEFAULT_PORT


def device_arg(value):
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return value                                      # sounddevice matches names by substring


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def read_pid():
    try:
        return int(PIDFILE.read_text().strip())
    except (OSError, ValueError):
        return None


def stop_running():
    pid = read_pid()
    if pid is None or not pid_alive(pid):
        print("no voice bridge is running (no live pid in %s)" % PIDFILE)
        PIDFILE.unlink(missing_ok=True)
        return 1
    os.kill(pid, signal.SIGTERM)
    for _ in range(50):
        if not pid_alive(pid):
            print("stopped voice bridge (pid %d); the face is back to idle" % pid)
            return 0
        time.sleep(0.1)
    print("pid %d did not exit within 5 s; kill it by hand" % pid)
    return 1


def build_parser():
    p = argparse.ArgumentParser(prog="voice.py", description="Talk to Athena through the Mac's microphone and speakers, "
                                "or the audio board's (--audio board), with the OpenAI Realtime API, and show the "
                                "conversation state on the LED face.",
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="stop a background copy with --stop; the pid is in %s" % PIDFILE)
    p.add_argument("--model", default=DEFAULT_MODEL, help="Realtime model (default %(default)s; gpt-realtime-2, gpt-realtime-2.1-mini)")
    p.add_argument("--voice", default=DEFAULT_VOICE, choices=VOICES, help="output voice (default %(default)s)")
    p.add_argument("--speed", type=float, default=1.0, help="speech speed 0.25..1.5 (default 1.0)")
    p.add_argument("--vad", choices=("server", "semantic"), default="server", help="turn detection (default server)")
    p.add_argument("--silence-ms", type=int, default=700, help="server VAD: silence that ends your turn (default 700)")
    p.add_argument("--transcribe", default=DEFAULT_TRANSCRIBE, help="model that transcribes your speech for the log (default %(default)s)")
    p.add_argument("--language", default=DEFAULT_LANGUAGE, metavar="CODE", help="the one language she hears and speaks, "
                   "ISO 639-1 (default %(default)s): it goes to the transcriber and onto the end of the system prompt; "
                   "`auto` follows the speaker")
    p.add_argument("--instructions", help="system prompt for the voice")
    p.add_argument("--instructions-file", help="read the system prompt from a file instead")
    p.add_argument("--greet", action="store_true", help="say hello when connected (default: she waits for you to speak first)")
    p.add_argument("--no-greet", dest="greet", action="store_false", help=argparse.SUPPRESS)    # the default now; old command lines
    p.set_defaults(greet=False)
    p.add_argument("--greeting", default=DEFAULT_GREETING, help="what the greeting should do")
    p.add_argument("--barge-in", action="store_true", help="send the microphone while the reply plays (headphones)")
    p.add_argument("--tail", type=float, help="half-duplex: seconds the mic stays gated after playback "
                   "(default 0.3, with the audio board %.1f)" % BOARD_TAIL_S)
    p.add_argument("--audio", choices=("mac", "board"), default="mac", help="whose microphone and speaker: this Mac's "
                   "(default) or the audio board's over Wi-Fi (firmware/athena_audio)")
    p.add_argument("--audio-host", metavar="HOST[:PORT]", help="the audio board, implies --audio board (default: "
                   "ATHENA_AUDIO_HOST, else %s:%d)" % (audio_mod.DEFAULT_HOST, audio_mod.DEFAULT_PORT))
    p.add_argument("--gain", type=float, default=DEFAULT_BOARD_GAIN_DB, metavar="DB", help="audio board: speaker gain in dB, "
                   "clipped at full scale (default %(default)s; the board has no volume knob)")
    p.add_argument("--input", type=device_arg, help="Mac microphone device index or name (default: system input)")
    p.add_argument("--output", type=device_arg, help="Mac speaker device index or name (default: system output)")
    p.add_argument("--list-devices", action="store_true", help="print the Mac's audio devices and exit")
    p.add_argument("--face-host", metavar="HOST[:PORT]", help="the board over Wi-Fi (default: how face.py finds it)")
    p.add_argument("--face-port", metavar="/dev/cu.…", help="the board over USB serial")
    p.add_argument("--brightness", type=int, metavar="0-255", help="panel brightness, sent once connected")
    p.add_argument("--no-face", dest="face", action="store_false", help="run without the board")
    p.add_argument("--face-dry-run", action="store_true", help="print the face lines instead of sending them")
    p.add_argument("--max-retries", type=int, default=20, help="reconnect attempts before giving up (default 20)")
    p.add_argument("--stop", action="store_true", help="stop the running bridge (SIGTERM to the pid file) and exit")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.list_devices:
        print(sd.query_devices())
        return 0
    if args.stop:
        return stop_running()
    if args.audio_host:
        args.audio = "board"
    if args.audio == "board":
        try:
            args.audio_target = audio_board_target(args.audio_host)
        except audio_mod.AudioError as e:
            print("error: %s" % e, file=sys.stderr)
            return 2
    if args.tail is None:
        args.tail = BOARD_TAIL_S if args.audio == "board" else 0.3
    if args.instructions_file:
        args.instructions = Path(args.instructions_file).read_text().strip()
    elif args.instructions is None:
        args.instructions = DEFAULT_INSTRUCTIONS % TALKING_THROUGH[args.audio]
    args.language = "" if args.language.lower() == "auto" else args.language.lower()
    language = ONE_LANGUAGE % {"name": LANGUAGES.get(args.language, "the language with the ISO 639-1 code " + args.language)} \
        if args.language else ANY_LANGUAGE
    args.instructions += " " + language
    args.greeting += " " + language                      # a response's own instructions replace the session's
    if args.brightness is not None and not 0 <= args.brightness <= 255:
        print("error: --brightness must be 0..255", file=sys.stderr)
        return 2
    pid = read_pid()
    if pid is not None and pid_alive(pid) and pid != os.getpid():
        print("error: a voice bridge is already running (pid %d); stop it with --stop" % pid, file=sys.stderr)
        return 2
    key = load_api_key()
    if not key:
        print("error: OPENAI_API_KEY is not set and not in %s or ~/.hermes/.env" % (REPO / ".env"), file=sys.stderr)
        return 2

    face = face_mod.FaceLink(log=log, host=args.face_host, port=args.face_port, brightness=args.brightness,
                    dry_run=args.face_dry_run, enabled=args.face)
    PIDFILE.parent.mkdir(parents=True, exist_ok=True)
    PIDFILE.write_text(str(os.getpid()))
    log("athena voice: model %s, voice %s, language %s, %s vad, %s; face %s" % (
        args.model, args.voice, args.language or "auto", args.vad, "barge-in on" if args.barge_in else "half-duplex", face.where()))
    log("stop with Ctrl-C or `scripts/voice.py --stop`")
    if args.audio == "board" and args.barge_in:
        log("WARNING: --barge-in with the audio board: it has no echo cancellation, its microphone hears "
            "the reply and the reply will interrupt itself")
    try:
        asyncio.run(run(args, key, face))
    finally:
        face.close("idle")
        if read_pid() == os.getpid():
            PIDFILE.unlink(missing_ok=True)
        log("stopped; the face is idle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
