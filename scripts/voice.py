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

Half-duplex by default: while the reply plays (plus a short tail) microphone
audio is not sent, because the MacBook's microphone hears its own speakers and
the server's voice detection would take the reply for you interrupting it.
--barge-in sends the microphone all the time; use it with headphones.

Runs under `uv run --script` (see the shebang): the audio and WebSocket
libraries live in an ephemeral environment, nothing is installed into the
system Python. scripts/face.py (standard library) is imported for the board,
which is found the way face.py finds it (--face-host, ATHENA_MATRIX_HOST, a
board on USB, else athena-matrix.local). OPENAI_API_KEY comes from the
environment, else from <repo>/.env, else from ~/.hermes/.env. The board serves
several connections over Wi-Fi, so this runs alongside a Hermes gateway.

    scripts/voice.py                      # talk; Ctrl-C to stop
    scripts/voice.py --voice cedar --vad semantic
    scripts/voice.py --list-devices
    scripts/voice.py --no-face            # without the board
    scripts/voice.py --stop               # stop a copy started in the background
"""

import argparse
import asyncio
import base64
import json
import os
import queue
import signal
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
import face as face_mod  # noqa: E402  scripts/face.py: Face, FaceError, find_target, now_hhmm

RATE = 24000                     # the Realtime API's PCM rate, both directions
BLOCK_MS = 100                   # one microphone chunk / one playback block
DEFAULT_MODEL = "gpt-realtime-2.1"
DEFAULT_VOICE = "marin"
VOICES = ("alloy", "ash", "ballad", "coral", "echo", "sage", "shimmer", "verse", "marin", "cedar")
DEFAULT_TRANSCRIBE = "gpt-4o-mini-transcribe"
PIDFILE = Path.home() / ".hermes" / "athena-voice.pid"
DEFAULT_INSTRUCTIONS = (
    "You are Athena, a voice assistant that lives on a desk with a small LED face, "
    "talking through a MacBook's microphone and speakers. Keep replies short and "
    "conversational: one to three sentences unless the user asks for detail. Answer in "
    "the language the user speaks. If you did not catch something, say so briefly."
)
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


# ----------------------------------------------------------------------------- face

class FaceLink:
    """Owns the connection to the board on its own thread; callers push(mode) and return.

    Bursts within 50 ms collapse to the last mode. idle/alert are re-sent every minute
    with the clock; speak is sent with a 30 s ttl and re-sent every 20 s, so a bridge
    that dies mid-sentence never leaves the board stuck in speak; error falls back to
    idle after 10 s (the board does the same on its own); a lost board is retried
    every 10 s with the last wanted mode."""

    CLOCK = ("idle", "alert")
    STICKY = ("idle", "alert", "sleep")
    TTL = {"speak": 30}
    REFRESH = {"idle": 60.0, "alert": 60.0, "speak": 20.0}
    FALLBACK = {"error": 10.0}
    RETRY, COALESCE = 10.0, 0.05

    def __init__(self, host=None, port=None, brightness=None, dry_run=False, enabled=True):
        self.enabled, self.dry_run, self.brightness = enabled, dry_run, brightness
        self._face = face_mod.Face(host=host, port=port)
        self._q = queue.Queue()
        self._connected = self._warned = False
        self._t = threading.Thread(target=self._run, name="face", daemon=True)
        if enabled:
            self._t.start()

    def where(self):
        if not self.enabled:
            return "off (--no-face)"
        if self.dry_run:
            return "dry run"
        kind, target = face_mod.find_target(port=self._face.port, host=self._face.host)
        return "%s:%d over Wi-Fi" % target if kind == "tcp" else target + " over USB"

    def push(self, mode):
        if self.enabled:
            self._q.put(mode)

    def close(self, final="idle"):
        if not self.enabled or not self._t.is_alive():
            return
        self._q.put(("stop", final))
        self._t.join(timeout=4)

    def _run(self):
        wanted = shown = None
        shown_at = retry_at = 0.0
        while True:
            now = time.monotonic()
            if wanted is not None and not self._connected and not self.dry_run:
                deadline = retry_at
            elif shown in self.REFRESH:
                deadline = shown_at + self.REFRESH[shown]
            elif shown in self.FALLBACK:
                deadline = shown_at + self.FALLBACK[shown]
            else:
                deadline = None
            try:
                item = self._q.get(timeout=None if deadline is None else max(0.0, deadline - now))
            except queue.Empty:
                item = None
            if item is not None:
                if isinstance(item, tuple):
                    self._send(item[1])
                    self._face.close()
                    return
                wanted = item
                end = time.monotonic() + self.COALESCE
                while True:                                  # collapse the rest of the burst
                    try:
                        nxt = self._q.get(timeout=max(0.0, end - time.monotonic()))
                    except queue.Empty:
                        break
                    if isinstance(nxt, tuple):
                        self._send(nxt[1])
                        self._face.close()
                        return
                    wanted = nxt
            now = time.monotonic()
            if item is None and shown in self.FALLBACK and now - shown_at >= self.FALLBACK[shown]:
                wanted = "idle"
            refresh = (shown is not None and shown == wanted and shown in self.REFRESH
                       and now - shown_at >= self.REFRESH[shown])
            if wanted is None or (wanted == shown and not refresh):
                continue
            if not self._connected and not self.dry_run and now < retry_at:
                continue
            if self._send(wanted):
                shown, shown_at = wanted, now
            else:
                shown, retry_at = None, now + self.RETRY

    def _send(self, mode):
        obj = {"mode": mode}
        if mode in self.CLOCK:
            obj["t"] = face_mod.now_hhmm()
        if mode in self.STICKY:
            obj["ttl"] = 0
        elif mode in self.TTL:
            obj["ttl"] = self.TTL[mode]
        if self.dry_run:
            log("face -> " + json.dumps(obj, separators=(",", ":")))
            return True
        try:
            if not self._connected:
                self._face.open()
                if self.brightness is not None:
                    self._face.send({"brightness": self.brightness})
                self._connected, self._warned = True, False
                log("face: connected to %s" % self._face.where)
            self._face.send(obj)
            return True
        except face_mod.FaceError as e:
            self._face.close()
            self._connected = False
            if not self._warned:
                log("face: %s (retrying every %.0f s)" % (e, self.RETRY))
                self._warned = True
            return False


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

    def name(self, kind):
        stream = self.in_stream if kind == "in" else self.out_stream
        return sd.query_devices(stream.device)["name"]

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
                    "noise_reduction": {"type": "near_field"},
                    "transcription": {"model": args.transcribe},
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
                log("WARNING: the microphone has been silent for 5 s. If macOS never asked, allow "
                    "the microphone for this terminal app in System Settings > Privacy & Security "
                    "> Microphone, then start again.")
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
    audio = Audio(args.input, args.output)
    audio.start(loop, aq)
    log("mic: %s @ %d Hz   speakers: %s @ %d Hz" % (audio.name("in"), audio.in_rate, audio.name("out"), audio.out_rate))
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
    p = argparse.ArgumentParser(prog="voice.py", description="Talk to Athena through the Mac's microphone and speakers "
                                "with the OpenAI Realtime API, and show the conversation state on the LED face.",
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="stop a background copy with --stop; the pid is in %s" % PIDFILE)
    p.add_argument("--model", default=DEFAULT_MODEL, help="Realtime model (default %(default)s; gpt-realtime-2, gpt-realtime-2.1-mini)")
    p.add_argument("--voice", default=DEFAULT_VOICE, choices=VOICES, help="output voice (default %(default)s)")
    p.add_argument("--speed", type=float, default=1.0, help="speech speed 0.25..1.5 (default 1.0)")
    p.add_argument("--vad", choices=("server", "semantic"), default="server", help="turn detection (default server)")
    p.add_argument("--silence-ms", type=int, default=700, help="server VAD: silence that ends your turn (default 700)")
    p.add_argument("--transcribe", default=DEFAULT_TRANSCRIBE, help="model that transcribes your speech for the log (default %(default)s)")
    p.add_argument("--instructions", default=DEFAULT_INSTRUCTIONS, help="system prompt for the voice")
    p.add_argument("--instructions-file", help="read the system prompt from a file instead")
    p.add_argument("--no-greet", dest="greet", action="store_false", help="do not say hello when connected")
    p.add_argument("--greeting", default=DEFAULT_GREETING, help="what the greeting should do")
    p.add_argument("--barge-in", action="store_true", help="send the microphone while the reply plays (headphones)")
    p.add_argument("--tail", type=float, default=0.3, help="half-duplex: seconds the mic stays gated after playback (default 0.3)")
    p.add_argument("--input", type=device_arg, help="microphone device index or name (default: system input)")
    p.add_argument("--output", type=device_arg, help="speaker device index or name (default: system output)")
    p.add_argument("--list-devices", action="store_true", help="print the audio devices and exit")
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
    if args.instructions_file:
        args.instructions = Path(args.instructions_file).read_text().strip()
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

    face = FaceLink(host=args.face_host, port=args.face_port, brightness=args.brightness,
                    dry_run=args.face_dry_run, enabled=args.face)
    PIDFILE.parent.mkdir(parents=True, exist_ok=True)
    PIDFILE.write_text(str(os.getpid()))
    log("athena voice: model %s, voice %s, %s vad, %s; face %s" % (
        args.model, args.voice, args.vad, "barge-in on" if args.barge_in else "half-duplex", face.where()))
    log("stop with Ctrl-C or `scripts/voice.py --stop`")
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
