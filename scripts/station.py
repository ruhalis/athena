#!/usr/bin/env python3
"""The voice station: the audio board's microphone -> speech to text -> the
Burabai Hermes brain -> text to speech -> the audio board's speaker, with the
conversation state on the LED face. Runs on the office Mac mini next to the
delphi gateway. Standard library only; the brain and the speech servers are
what already runs on that machine.

One turn (no wake word: every utterance is a question):
  listen   the board streams s16le 16 kHz mono; an energy gate over 20 ms
           frames, calibrated on the room, cuts one utterance (0.5-15 s)
  stt      the WAV goes to the Hermes gateway's /v1/audio/transcriptions
           (its built-in faster-whisper; --stt hermes, the default) or to
           whisper.cpp on port 8088 (--stt whispercpp)
  brain    hermes_core.ask_hermes() from ~/burabai-station (--brain core,
           the default; the confirmation lock for register/close_violation
           lives there), or the SSE server on port 8643 (--brain http)
  speak    the answer, markdown stripped, one sentence at a time through the
           Hermes TTS server on port 8089 (Chatterbox, OpenAI-compatible),
           resampled to 16 kHz and streamed to the board while the next
           sentence synthesizes. Strict half-duplex: the mic is open only
           while listening; from the end of an utterance until the answer has
           played plus a tail, everything the board hears is dropped (it hears
           its own speaker)
  confirm  a pending register/close_violation is read out and the next
           utterance is the yes/no; voice_loop.interpret_confirmation decides
           and anything unclear is a no
Face: idle with the clock, listen, think, speak, alert while a confirmation
is awaited, error on a failure (falls back to idle on its own).

Where things are (all overridable by flags):
  audio board   --host / $ATHENA_AUDIO_HOST / athena-audio.local:7076
  face board    $ATHENA_MATRIX_HOST (face.py's rules)
  gateway STT   http://127.0.0.1:8642/v1/audio/transcriptions, key from
                $HERMES_API_KEY or API_SERVER_KEY in ~/.hermes/.env
  brain         ~/burabai-station/burabai_station (hermes_core.py); its env
                VLLM_BASE_URL / VLLM_MODEL / VLLM_API_KEY / SITE_BASE_URL is
                filled in here when unset: the tunnel on 30002, the model the
                gateway runs, the key from model.api_key in ~/.hermes/config.yaml
                (the dev's decision: one key for both), the mock site on 9001
  TTS           http://127.0.0.1:8089/v1/audio/speech

Testing without a person in the room:
  station.py --question "какие договоры истекают в ближайшие 60 дней"   # brain + speaker
  station.py --inject question.wav --once                                # STT + brain + speaker
  station.py --question ... --no-speak --no-face                         # brain only, prints
"""

import argparse
import array
import io
import json
import math
import os
import queue
import re
import signal
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
import wave
from collections import deque
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio as audio_mod  # noqa: E402  scripts/audio.py: connect, resample_linear, CHUNK_BYTES ...
import face as face_mod    # noqa: E402  scripts/face.py: FaceLink

SAMPLE_RATE = audio_mod.SAMPLE_RATE
FRAME_BYTES = audio_mod.CHUNK_BYTES          # 20 ms
FRAME_S = FRAME_BYTES / float(audio_mod.BYTES_PER_SEC)
PREROLL_FRAMES = 15                          # 300 ms kept from before the onset
ONSET_FRAMES = 3                             # 60 ms above the gate starts an utterance

DEFAULT_CORE_DIR = "~/burabai-station/burabai_station"
DEFAULT_BRAIN_URL = "http://127.0.0.1:8643/api/v1/assistant/query"
DEFAULT_TTS_URL = "http://127.0.0.1:8089/v1/audio/speech"
STT_URLS = {
    "hermes": "http://127.0.0.1:8642/v1/audio/transcriptions",
    "whispercpp": "http://127.0.0.1:8088/inference",
}
VLLM_DEFAULTS = {
    "VLLM_BASE_URL": "http://127.0.0.1:30002/v1",
    "VLLM_MODEL": "Qwen/Qwen3.8-27B-FP8",     # hermes_core's own default names a model this server does not have
    "SITE_BASE_URL": "http://127.0.0.1:9001",
}

OFFLINE_TEXT = "Не могу связаться с сервером данных, попробуйте позже."
UNCLEAR_TEXT = "Не разобрал ответ, поэтому ничего не фиксирую. Повторите команду, если нужно."
NOTHING_HEARD_TEXT = "Ответа не услышал, поэтому ничего не фиксирую."

# Whisper on silence or noise: whole-transcript artefacts, same family as the
# mini's whisper-local-server filter.
HALLUCINATION_RE = re.compile(
    r"^(подпис(ывайтесь|ывайся|ки).*|субтитр.*|спасибо за внимание\.?|продолжение следует\.?|"
    r"thank you\.?|thanks for watching\.?|\.+)$", re.I)


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


class StationError(Exception):
    pass


# --------------------------------------------------------------------------- config

def read_env_file(path, key):
    try:
        lines = Path(path).expanduser().read_text().splitlines()
    except OSError:
        return None
    for line in lines:
        line = line.strip()
        if line.startswith("export "):
            line = line[7:].lstrip()
        if line.startswith(key + "="):
            val = line.split("=", 1)[1].strip().strip('"').strip("'")
            if val:
                return val
    return None


def hermes_model_key():
    """model.api_key from ~/.hermes/config.yaml, without a YAML parser."""
    try:
        text = (Path.home() / ".hermes" / "config.yaml").read_text()
    except OSError:
        return None
    in_model = False
    for line in text.splitlines():
        if line.strip() and not line.startswith((" ", "\t")):
            in_model = line.startswith("model:")
            continue
        if in_model:
            m = re.match(r"\s+api_key:\s*(\S+)", line)
            if m:
                return m.group(1).strip("\"'")
    return None


def prepare_env(args):
    """Fill in what hermes_core reads at import when the environment lacks it."""
    for key, val in VLLM_DEFAULTS.items():
        os.environ.setdefault(key, val)
    if not os.environ.get("VLLM_API_KEY"):
        key = hermes_model_key()
        if key:
            os.environ["VLLM_API_KEY"] = key
    # voice_loop.py builds OpenAI clients at import for its own (unused here) cloud path.
    os.environ.setdefault("OPENAI_API_KEY", "unused")
    if args.stt == "hermes" and not args.stt_key:
        args.stt_key = os.environ.get("HERMES_API_KEY") or read_env_file("~/.hermes/.env", "API_SERVER_KEY")
        if not args.stt_key:
            raise StationError("no key for the gateway STT: set HERMES_API_KEY or API_SERVER_KEY in ~/.hermes/.env, or use --stt whispercpp")


# ---------------------------------------------------------------------------- audio

def audio_target(args):
    if args.host:
        host, port = audio_mod.parse_hostport(args.host)
    else:
        env = os.environ.get("ATHENA_AUDIO_HOST")
        host, port = audio_mod.parse_hostport(env) if env else (audio_mod.DEFAULT_HOST, None)
    return host, port or audio_mod.DEFAULT_PORT


class Mic:
    """Owns the receive side of the board's socket on a thread. frame() hands
    out 20 ms frames while not muted; the socket drains either way (the board
    drops mic samples for a slow reader, never the connection, but our own
    receive buffer must not fill while we send)."""

    def __init__(self, sock):
        self.sock = sock
        self.muted = True                          # listen() opens it
        self.dead = threading.Event()
        self.error = None
        self.received = 0
        self._q = queue.Queue()
        self._buf = b""
        threading.Thread(target=self._run, name="mic", daemon=True).start()

    def _run(self):
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError as e:
                self.error = str(e)
                self.dead.set()
                return
            if not chunk:
                self.error = "connection closed by the board"
                self.dead.set()
                return
            self.received += len(chunk)
            if self.muted:
                self._buf = b""
                continue
            self._buf += chunk
            while len(self._buf) >= FRAME_BYTES:
                self._q.put(self._buf[:FRAME_BYTES])
                self._buf = self._buf[FRAME_BYTES:]

    def flush(self):
        while True:
            try:
                self._q.get_nowait()
            except queue.Empty:
                return

    def frame(self, timeout=0.5):
        try:
            return self._q.get(timeout=timeout)
        except queue.Empty:
            return None


def frame_dbfs(frame):
    samples = array.array("h")
    samples.frombytes(frame)
    if not samples:
        return -96.0
    rms = math.sqrt(sum(s * s for s in samples) / float(len(samples)))
    return audio_mod.dbfs(rms)


class Gate:
    """Energy gate. The floor is the 10th percentile of the last 3 s of frame
    levels seen outside an utterance; a frame is voice when it sits `vad_db`
    above that floor, and never below `vad_min` dBFS."""

    WINDOW = 150     # frames, 3 s
    WARMUP = 50      # frames, 1 s before the first decision

    def __init__(self, vad_db, vad_min):
        self.vad_db, self.vad_min = vad_db, vad_min
        self.levels = deque(maxlen=self.WINDOW)
        self.floor = None

    def update(self, level):
        self.levels.append(level)
        if len(self.levels) >= self.WARMUP:
            ordered = sorted(self.levels)
            self.floor = ordered[len(ordered) // 10]

    @property
    def threshold(self):
        if self.floor is None:
            return None
        return max(self.floor + self.vad_db, self.vad_min)


def listen(mic, gate, args, deadline=None, what="a question"):
    """Cut one utterance from the mic. -> PCM bytes, or None when `deadline`
    (monotonic) passes first. Raises StationError when the board is gone."""
    pre = deque(maxlen=PREROLL_FRAMES)
    utter = bytearray()
    in_speech = False
    voiced_run = silence_run = speech_frames = 0
    silence_frames = max(1, int(round(args.silence_ms / 1000.0 / FRAME_S)))
    max_bytes = int(args.max_speech * audio_mod.BYTES_PER_SEC)
    announced = False
    # Half-duplex, strictly: the mic is open only inside this function. It is
    # muted again on every way out, so nothing the board hears while the
    # station transcribes, thinks or speaks (its own voice included) can
    # become the next question; the next listen() starts from a clean queue.
    mic.flush()
    mic.muted = False
    while True:
        if mic.dead.is_set():
            mic.muted = True
            raise StationError("audio board: %s" % mic.error)
        frame = mic.frame()
        if frame is None:
            if deadline is not None and time.monotonic() > deadline:
                mic.muted = True
                return None
            continue
        level = frame_dbfs(frame)
        if not in_speech:
            gate.update(level)
            pre.append(frame)
            threshold = gate.threshold
            if threshold is None:
                continue
            if not announced:
                log("gate: room floor %.1f dBFS, voice above %.1f dBFS; waiting for %s" % (gate.floor, threshold, what))
                announced = True
            voiced_run = voiced_run + 1 if level > threshold else 0
            if voiced_run >= ONSET_FRAMES:
                in_speech = True
                utter = bytearray(b"".join(pre))
                speech_frames, silence_run = voiced_run, 0
                log("gate: voice at %.1f dBFS" % level)
            elif deadline is not None and time.monotonic() > deadline:
                mic.muted = True
                return None
            continue
        utter += frame
        if level > gate.threshold:
            silence_run, speech_frames = 0, speech_frames + 1
        else:
            silence_run += 1
        if silence_run >= silence_frames or len(utter) >= max_bytes:
            voiced_s = speech_frames * FRAME_S
            total_s = len(utter) / float(audio_mod.BYTES_PER_SEC)
            if voiced_s < args.min_speech:
                log("gate: %.2f s of voice in %.1f s, too short, ignored" % (voiced_s, total_s))
                in_speech, voiced_run, utter = False, 0, bytearray()
                pre.clear()
                continue
            log("gate: utterance %.1f s (%.1f s of voice); mic closed until the answer has played" % (total_s, voiced_s))
            mic.muted = True
            return bytes(utter)


def play_pcm(sock, samples):
    """Stream s16 samples to the board paced to real time (the board buffers
    0.5 s); returns once the last of it has played."""
    data = samples.tobytes()
    total, sent = len(data), 0
    start = time.monotonic()
    while sent < total:
        end = min(sent + FRAME_BYTES, total)
        try:
            sock.sendall(data[sent:end])
        except OSError as e:
            raise StationError("audio board: %s" % e)
        sent = end
        ahead = sent / float(audio_mod.BYTES_PER_SEC) - (time.monotonic() - start)
        if ahead > 0.3:
            time.sleep(ahead - 0.3)
    remaining = total / float(audio_mod.BYTES_PER_SEC) - (time.monotonic() - start)
    if remaining > 0:
        time.sleep(remaining)


def wav_bytes(pcm):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return buf.getvalue()


def parse_wav(data):
    """Any RIFF WAV (PCM16 or float32, any rate, any channels) -> (s16 mono samples, rate)."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise StationError("not a WAV (%d bytes, starts %r)" % (len(data), data[:12]))
    pos, fmt, payload = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], "little")
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag = int.from_bytes(body[0:2], "little")
            channels = int.from_bytes(body[2:4], "little")
            rate = int.from_bytes(body[4:8], "little")
            bits = int.from_bytes(body[14:16], "little")
            if tag == 0xFFFE and len(body) >= 26:      # WAVE_FORMAT_EXTENSIBLE: the sub-format tag
                tag = int.from_bytes(body[24:26], "little")
            fmt = (tag, channels, rate, bits)
        elif cid == b"data":
            payload = body
        pos += 8 + size + (size & 1)
    if fmt is None or payload is None:
        raise StationError("WAV without fmt/data chunks")
    tag, channels, rate, bits = fmt
    if tag == 3 or bits == 32:
        floats = array.array("f")
        floats.frombytes(payload[:len(payload) // 4 * 4])
        samples = array.array("h", (int(max(-1.0, min(1.0, x)) * 32767) for x in floats))
    elif bits == 16:
        samples = array.array("h")
        samples.frombytes(payload[:len(payload) // 2 * 2])
    else:
        raise StationError("WAV format %d/%d-bit not supported" % (tag, bits))
    if channels > 1:
        samples = audio_mod._downmix(samples, channels)
    return samples, rate


# ------------------------------------------------------------------------------ stt

def multipart(fields, filename, data):
    boundary = "athena" + uuid.uuid4().hex
    body = bytearray()
    for key, val in fields.items():
        body += ("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n" % (boundary, key, val)).encode("utf-8")
    body += ("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
             "Content-Type: audio/wav\r\n\r\n" % (boundary, filename)).encode("utf-8")
    body += data
    body += ("\r\n--%s--\r\n" % boundary).encode("utf-8")
    return "multipart/form-data; boundary=" + boundary, bytes(body)


def http_error_text(e):
    try:
        body = e.read().decode("utf-8", "replace")
    except Exception:
        body = ""
    try:
        obj = json.loads(body)
        body = obj.get("error", {}).get("message") or obj.get("detail") or body
    except Exception:
        pass
    return "HTTP %d %s" % (e.code, str(body)[:200])


class Stt:
    def __init__(self, kind, url, key, language):
        self.kind, self.url, self.key, self.language = kind, url, key, language

    def transcribe(self, pcm):
        if self.kind == "hermes":
            fields = {"model": "whisper-1", "language": self.language}
        else:
            fields = {"response_format": "json", "language": self.language}
        ctype, body = multipart(fields, "speech.wav", wav_bytes(pcm))
        headers = {"Content-Type": ctype}
        if self.key:
            headers["Authorization"] = "Bearer " + self.key
        req = urllib.request.Request(self.url, data=body, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                obj = json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            raise StationError("stt %s: %s" % (self.kind, http_error_text(e)))
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise StationError("stt %s at %s: %s" % (self.kind, self.url, e))
        return clean_transcript(obj.get("text") or "")


def clean_transcript(text):
    """Drop what whisper makes up on noise: bracketed sound tags, the stock
    silence phrases, anything without two letters in a row."""
    t = re.sub(r"[\(\[][^\)\]]*[\)\]]", " ", text)
    t = re.sub(r"\s+", " ", t).strip()
    if HALLUCINATION_RE.match(t.rstrip(" .…!")):
        return ""
    if not re.search(r"[^\W\d_]{2,}", t):
        return ""
    return t


# ---------------------------------------------------------------------------- brain

class CoreBrain:
    """hermes_core.ask_hermes() in-process, the dev's design: the model picks
    a tool, the tool calls SITE_BASE_URL, register/close_violation stop for a
    spoken yes/no (interpret_confirmation from voice_loop.py)."""

    def __init__(self, core_dir, timeout):
        core_dir = str(Path(core_dir).expanduser())
        if not os.path.isfile(os.path.join(core_dir, "hermes_core.py")):
            raise StationError("no hermes_core.py in %s (--core-dir)" % core_dir)
        sys.path.insert(0, core_dir)
        try:
            import hermes_core  # noqa: F401
            import voice_loop   # noqa: F401
        except ImportError as e:
            raise StationError("cannot import the Burabai station code from %s: %s "
                               "(it needs the Python that has openai and httpx, on the mini /opt/homebrew/bin/python3)" % (core_dir, e))
        self.core, self.vl = hermes_core, voice_loop
        # ask_hermes() reads the module-level client at call time; a copy with a
        # bounded timeout keeps a dropped tunnel from hanging a turn for minutes.
        self.core.llm_client = self.core.llm_client.with_options(timeout=timeout, max_retries=1)
        self.pending = self.history = None
        self.where = "hermes_core in %s -> %s (%s)" % (core_dir, os.environ["VLLM_BASE_URL"], os.environ["VLLM_MODEL"])

    def ask(self, question):
        r = self.core.ask_hermes(question)
        self.pending, self.history = r.get("pending_confirmation"), r.get("history")
        return r

    def confirm(self, answer):
        """-> (verdict, result). Anything but an explicit yes cancels."""
        verdict = self.vl.interpret_confirmation(answer) if answer else "unclear"
        r = self.core.confirm_pending(self.pending, self.history, confirmed=(verdict == "yes"))
        self.pending = self.history = None
        return verdict, r


class HttpBrain:
    """The SSE server on port 8643 (assistant_server.py): stateless, no
    confirmation flow, kept as a fallback when the core cannot be imported."""

    def __init__(self, url):
        self.url = url
        self.pending = None
        self.where = url

    def ask(self, question):
        req = urllib.request.Request(self.url, data=json.dumps({"question": question}).encode("utf-8"),
                                     headers={"Content-Type": "application/json", "Accept": "text/event-stream"}, method="POST")
        tokens, calls = [], []
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                for raw in r:
                    line = raw.decode("utf-8", "replace").strip()
                    if not line.startswith("data:"):
                        continue
                    ev = json.loads(line[5:].strip())
                    kind = ev.get("type")
                    if kind == "token":
                        tokens.append(ev.get("text", ""))
                    elif kind == "tool_trace":
                        calls = ev.get("calls", [])
                    elif kind == "error":
                        raise StationError("brain: %s" % ev.get("text"))
                    elif kind == "done":
                        break
        except urllib.error.HTTPError as e:
            raise StationError("brain: %s" % http_error_text(e))
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise StationError("brain at %s: %s" % (self.url, e))
        return {"text": "".join(tokens).strip(), "tool_trace": calls, "pending_confirmation": None}

    def confirm(self, answer):
        raise StationError("the HTTP brain has no confirmation flow")


# ------------------------------------------------------------------------------ tts

def sanitize_for_tts(text):
    """Markdown out, then the character whitelist the mini's Hermes applies
    before this same Chatterbox server (Cyrillic, ASCII, a few symbols):
    other scripts and emoji make it hang or breathe."""
    t = re.sub(r"```.*?```", " ", text, flags=re.S)
    t = re.sub(r"`([^`]*)`", r"\1", t)
    t = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", t)
    t = re.sub(r"^[ \t]{0,3}#{1,6}[ \t]*", "", t, flags=re.M)
    t = re.sub(r"^[ \t]*(?:[-*•]|\d+[.)])[ \t]+", "", t, flags=re.M)
    t = re.sub(r"\*{1,3}", "", t)
    t = re.sub(r"_{1,3}", " ", t)
    safe_extra = {0x00B0, 0x00AB, 0x00BB, 0x2026, 0x2013, 0x2014, 0x0009, 0x000A, 0x000D}
    t = "".join(c for c in t if 0x20 <= ord(c) <= 0x7E or 0x400 <= ord(c) <= 0x4FF or ord(c) in safe_extra)
    t = re.sub(r"([.!?:;,…])[ \t]*\n{2,}", r"\1 ", t)     # a paragraph break after punctuation is just a pause
    t = re.sub(r"\n{2,}", ". ", t)
    t = t.replace("\n", " ")
    t = re.sub(r"\.{2,}", ".", t)
    t = re.sub(r" {2,}", " ", t).strip()
    return t.lstrip(" .,;:")


ABBREVIATIONS = frozenset((
    "кв", "оз", "ул", "г", "д", "с", "п", "т", "им", "тел", "ст", "стр", "корп", "обл", "р", "пос", "уч",
    "га", "руб", "тг", "тыс", "млн", "млрд", "см", "напр", "т.е", "т.д", "т.п", "и.о", "мкр", "пр", "пер", "наб",
))


def _sentence_parts(text):
    """Split at . ! ? … followed by space, except after an abbreviation or an
    initial (кв. 7, оз. Боровое, Д.С.) and before a digit or a lowercase letter."""
    out, start = [], 0
    for m in re.finditer(r"[.!?…]+\s+", text):
        end = m.end()
        before = text[start:m.start()]
        word = re.split(r"[\s(«\"]", before)[-1].lower() if before else ""
        nxt = text[end:end + 1]
        if m.group().startswith("."):
            if word in ABBREVIATIONS or (len(word) <= 1 and word.isalpha()) or re.fullmatch(r"[а-яa-z]\.[а-яa-z]", word):
                continue
            if nxt.isdigit() or (nxt.isalpha() and nxt.islower()):
                continue
        out.append(text[start:end])
        start = end
    out.append(text[start:])
    return out


def split_sentences(text, min_len=25, max_len=110):
    """Chunks for one TTS call each: long sentences cut at punctuation (the
    server drops the tail of long inputs), short ones merged with a neighbour
    (it refuses clips under 2 s)."""
    chunks = []
    for part in _sentence_parts(text):
        s = part.strip()
        while len(s) > max_len:
            cut = max(s.rfind(sep, min_len, max_len) for sep in (", ", "; ", ": ", " – ", " — "))
            if cut < 0:
                cut = s.rfind(" ", min_len, max_len)
            if cut < 0:
                cut = max_len
            chunks.append(s[:cut + 1].strip())
            s = s[cut + 1:].strip()
        if s:
            chunks.append(s)
    out = []
    for c in chunks:
        if out and (len(c) < min_len or len(out[-1]) < min_len) and len(out[-1]) + 1 + len(c) <= max_len:
            out[-1] += " " + c
        else:
            out.append(c)
    return out


class Tts:
    def __init__(self, url, gain_db, model, voice):
        self.url, self.gain_db, self.model, self.voice = url, gain_db, model, voice

    def synth(self, sentence):
        """-> s16 samples at 16 kHz, peak-normalised then `gain_db` (clipped)."""
        payload = json.dumps({"model": self.model, "voice": self.voice, "input": sentence,
                              "response_format": "wav"}, ensure_ascii=False).encode("utf-8")
        for attempt in range(3):
            req = urllib.request.Request(self.url, data=payload, headers={"Content-Type": "application/json"}, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=130) as r:
                    data = r.read()
                break
            except urllib.error.HTTPError as e:
                # 503: the server's own "too short, likely early EOS" check; a retry usually gets a full clip
                if e.code == 503 and attempt < 2:
                    log("tts: %s, retrying" % http_error_text(e))
                    continue
                raise StationError("tts: %s" % http_error_text(e))
            except (urllib.error.URLError, OSError) as e:
                raise StationError("tts at %s: %s" % (self.url, e))
        samples, rate = parse_wav(data)
        samples = audio_mod.resample_linear(samples, rate, SAMPLE_RATE)
        gain = audio_mod.normalize_db(samples) + self.gain_db
        if gain:
            samples, _ = audio_mod.apply_gain(samples, gain)
        return samples


class Speaker:
    def __init__(self, tts, link, face, args):
        self.tts, self.link, self.face, self.args = tts, link, face, args

    def say(self, text):
        sentences = split_sentences(sanitize_for_tts(text))
        if not sentences:
            return
        if self.args.no_speak or self.link is None:
            for s in sentences:
                log("would say: %s" % s)
            return
        q = queue.Queue(maxsize=2)                 # synthesis runs one to two sentences ahead of playback

        def worker():
            for s in sentences:
                try:
                    q.put(("pcm", self.tts.synth(s), s))
                except Exception as e:      # noqa: BLE001  one bad sentence must not silence the rest
                    q.put(("err", e, s))
            q.put(None)

        threading.Thread(target=worker, name="tts", daemon=True).start()
        mic = self.link.mic
        mic.muted = True                           # already muted since listen() returned; belt and braces
        started = time.monotonic()
        spoke = False
        try:
            while True:
                item = q.get()
                if item is None:
                    break
                kind, payload, s = item
                if kind == "err":
                    log("tts: %s (for: %s)" % (payload, s[:60]))
                    continue
                if not spoke:
                    self.face.push("speak")
                    log("speak (first audio after %.1f s): %s" % (time.monotonic() - started, s))
                    spoke = True
                else:
                    log("speak: %s" % s)
                play_pcm(self.link.sock, payload)
                play_pcm(self.link.sock, array.array("h", bytes(2 * int(SAMPLE_RATE * 0.15))))
        finally:
            time.sleep(self.args.tail)             # the room stops ringing before listen() reopens the mic
            mic.flush()


# ---------------------------------------------------------------------------- board

class AudioLink:
    """The one socket to the audio board plus its Mic thread; reconnects on demand."""

    def __init__(self, host, port):
        self.host, self.port = host, port
        self.sock = self.mic = None

    def ensure(self):
        if self.mic is not None and not self.mic.dead.is_set():
            return
        self.close()
        try:
            self.sock = audio_mod.connect(self.host, self.port)
        except audio_mod.AudioError as e:          # mDNS on the mini fails now and then: retried by the loop
            raise StationError(str(e))
        self.mic = Mic(self.sock)
        log("audio: connected to %s:%d" % (self.host, self.port))

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = self.mic = None


# ----------------------------------------------------------------------------- loop

class Station:
    def __init__(self, args):
        self.args = args
        self.stop = threading.Event()
        self.face = face_mod.FaceLink(log=log, host=args.face_host, brightness=args.brightness,
                                      dry_run=args.face_dry_run, enabled=not args.no_face)
        self.stt = Stt(args.stt, args.stt_url or STT_URLS[args.stt], args.stt_key, args.language)
        if args.brain == "core":
            self.brain = CoreBrain(args.core_dir, args.brain_timeout)
        else:
            self.brain = HttpBrain(args.brain_url)
        self.tts = Tts(args.tts_url, args.gain, args.tts_model, args.tts_voice)
        self.needs_board = not (args.no_speak and (args.question or args.inject))
        self.link = AudioLink(*audio_target(args)) if self.needs_board else None
        self.speaker = Speaker(self.tts, self.link, self.face, args)
        self.gate = Gate(args.vad_db, args.vad_min)

    def run(self):
        log("station: stt %s at %s; brain %s; tts %s (gain %+.0f dB after peak normalisation); face %s; audio %s" % (
            self.args.stt, self.stt.url, self.brain.where, self.tts.url, self.args.gain, self.face.where(),
            "%s:%d" % (self.link.host, self.link.port) if self.link else "off"))
        self.face.push("idle")
        used_question = used_inject = False
        try:
            while not self.stop.is_set():
                try:
                    if self.link is not None:
                        self.link.ensure()
                    if self.args.question and not used_question:
                        used_question = True
                        text = self.args.question
                        log("question (from --question): %s" % text)
                    elif self.args.inject and not used_inject:
                        used_inject = True
                        text = self.hear(self.read_inject())
                    else:
                        self.face.push("idle")
                        pcm = listen(self.link.mic, self.gate, self.args)
                        text = self.hear(pcm)
                    if text:
                        self.turn(text)
                    if self.args.once:
                        break
                    if used_question and (used_inject or not self.args.inject):
                        break                       # the scripted inputs are used up
                except StationError as e:
                    log("error: %s" % e)
                    self.face.push("error")
                    if self.link is not None:
                        self.link.close()
                    if self.args.once or self.args.question or self.args.inject:
                        return 1
                    self.stop.wait(5.0)
            return 0
        finally:
            self.face.close("idle")
            if self.link is not None:
                self.link.close()

    def read_inject(self):
        try:
            with open(self.args.inject, "rb") as f:
                samples, rate = parse_wav(f.read())
        except OSError as e:
            raise StationError("cannot read %s: %s" % (self.args.inject, e))
        samples = audio_mod.resample_linear(samples, rate, SAMPLE_RATE)
        log("inject: %s (%d Hz) as one utterance of %.1f s" % (self.args.inject, rate, len(samples) / float(SAMPLE_RATE)))
        return samples.tobytes()

    def hear(self, pcm):
        self.face.push("think")
        t0 = time.monotonic()
        text = self.stt.transcribe(pcm)
        if not text:
            log("stt (%.1f s): nothing usable, back to listening" % (time.monotonic() - t0))
            return None
        log("heard (%.1f s): %s" % (time.monotonic() - t0, text))
        return text

    def turn(self, text):
        self.face.push("think")
        t0 = time.monotonic()
        try:
            r = self.brain.ask(text)
        except StationError:
            raise
        except Exception as e:      # noqa: BLE001  the model, the tunnel or the site API
            log("brain: %s: %s" % (type(e).__name__, e))
            self.face.push("error")
            self.speaker.say(OFFLINE_TEXT)
            return
        for call in r.get("tool_trace") or []:
            log("tool: %s(%s) -> %s" % (call.get("tool"), json.dumps(call.get("args"), ensure_ascii=False),
                                        call.get("result_preview", call.get("found", ""))))
        log("answer (%.1f s): %s" % (time.monotonic() - t0, r["text"].replace("\n", " ")[:300]))
        self.speaker.say(r["text"])
        if getattr(self.brain, "pending", None):
            self.confirmation()

    def confirmation(self):
        pending = self.brain.pending
        log("confirmation wanted for %s(%s); listening %.0f s for yes/no" % (
            pending["tool"], json.dumps(pending["args"], ensure_ascii=False), self.args.confirm_timeout))
        self.face.push("alert")
        answer = ""
        if self.link is not None:
            pcm = listen(self.link.mic, self.gate, self.args, deadline=time.monotonic() + self.args.confirm_timeout, what="yes or no")
            if pcm is not None:
                self.face.push("think")
                answer = self.stt.transcribe(pcm)
                log("heard: %s" % (answer or "(nothing usable)"))
        verdict, r = self.brain.confirm(answer)
        log("confirmation: %s -> %s" % (verdict, r["text"]))
        if verdict == "unclear":
            self.speaker.say(UNCLEAR_TEXT if answer else NOTHING_HEARD_TEXT)
        else:
            self.speaker.say(r["text"])


def build_parser():
    p = argparse.ArgumentParser(prog="station.py", description="Voice station: audio board <-> Hermes STT/TTS <-> the Burabai brain, state on the face.",
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="testing without a person in the room:" + __doc__.split("Testing without a person in the room:")[1])
    p.add_argument("--host", metavar="HOST[:PORT]", help="the audio board (default $ATHENA_AUDIO_HOST or %s:%d)" % (audio_mod.DEFAULT_HOST, audio_mod.DEFAULT_PORT))
    p.add_argument("--face-host", metavar="HOST[:PORT]", help="the face board (default $ATHENA_MATRIX_HOST, face.py's rules)")
    p.add_argument("--brightness", type=int, help="face brightness 0-255 on connect")
    p.add_argument("--no-face", action="store_true", help="run without the face board")
    p.add_argument("--face-dry-run", action="store_true", help="print the face lines instead of sending them")
    p.add_argument("--stt", choices=sorted(STT_URLS), default="hermes", help="hermes: the gateway's built-in faster-whisper on 8642 (default); whispercpp: whisper-server on 8088")
    p.add_argument("--stt-url", help="override the STT endpoint")
    p.add_argument("--stt-key", help="bearer key for the gateway STT (default $HERMES_API_KEY, then API_SERVER_KEY in ~/.hermes/.env)")
    p.add_argument("--language", default="ru", help="speech language for STT (default ru)")
    p.add_argument("--brain", choices=("core", "http"), default="core", help="core: hermes_core.ask_hermes() in-process (default); http: the SSE server on 8643")
    p.add_argument("--core-dir", default=DEFAULT_CORE_DIR, help="where hermes_core.py lives (default %s)" % DEFAULT_CORE_DIR)
    p.add_argument("--brain-url", default=DEFAULT_BRAIN_URL, help="--brain http endpoint (default %s)" % DEFAULT_BRAIN_URL)
    p.add_argument("--brain-timeout", type=float, default=120.0, metavar="S", help="one model call may take this long, one retry (default 120)")
    p.add_argument("--tts-url", default=DEFAULT_TTS_URL, help="OpenAI-compatible speech endpoint (default %s)" % DEFAULT_TTS_URL)
    p.add_argument("--tts-model", default="gpt-4o-mini-tts", help="model field for the TTS request (the local server ignores it)")
    p.add_argument("--tts-voice", default="alloy", help="voice field for the TTS request (the local server ignores it)")
    p.add_argument("--gain", type=float, default=6.0, metavar="DB", help="speaker gain after peak normalisation, clipped at full scale (default 6; louder clips harder and garbles the voice)")
    p.add_argument("--vad-db", type=float, default=10.0, metavar="DB", help="voice must sit this far above the room floor (default 10)")
    p.add_argument("--vad-min", type=float, default=-40.0, metavar="DBFS", help="never count anything quieter than this as voice (default -40)")
    p.add_argument("--silence-ms", type=int, default=700, help="silence that ends an utterance (default 700)")
    p.add_argument("--min-speech", type=float, default=0.5, metavar="S", help="utterances with less voice than this are ignored (default 0.5)")
    p.add_argument("--max-speech", type=float, default=15.0, metavar="S", help="an utterance is cut after this long (default 15)")
    p.add_argument("--tail", type=float, default=0.6, metavar="S", help="mic stays closed this long after playback (default 0.6)")
    p.add_argument("--confirm-timeout", type=float, default=20.0, metavar="S", help="how long to wait for a spoken yes/no (default 20)")
    p.add_argument("--question", metavar="TEXT", help="skip the microphone once: ask TEXT, speak the answer, exit")
    p.add_argument("--inject", metavar="WAV", help="use this WAV as the first utterance instead of the microphone")
    p.add_argument("--once", action="store_true", help="exit after one turn")
    p.add_argument("--no-speak", action="store_true", help="print the sentences instead of synthesizing and playing them")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        prepare_env(args)
        station = Station(args)
    except StationError as e:
        print("station.py: %s" % e, file=sys.stderr)
        return 2
    except audio_mod.AudioError as e:
        print("station.py: %s" % e, file=sys.stderr)
        return e.code

    def stop(signum, frame):
        log("signal %d, stopping" % signum)
        station.stop.set()
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    try:
        return station.run()
    except KeyboardInterrupt:
        station.face.close("idle")
        if station.link is not None:
            station.link.close()
        return 0
    except audio_mod.AudioError as e:
        print("station.py: %s" % e, file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
