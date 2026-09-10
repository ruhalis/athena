#!/usr/bin/env python3
"""Stream audio to and from the Athena audio board over TCP.

Protocol: the board listens on TCP 7076 (AUDIO_TCP_PORT in
firmware/athena_audio/main/audio.h) for one client at a time; a second
connection is closed immediately. Both directions share that one socket
as raw PCM: signed 16-bit little-endian, 16000 Hz, mono, no framing. On
connect the board streams its microphone continuously; every byte
written to it plays on the speaker (silence when nothing arrives). The
board never drops the client for a slow mic read (it drops mic samples
instead), but a play-only client should still drain what it receives so
its own socket buffer does not fill.

Target resolution: --host (host or host:port), else $ATHENA_AUDIO_HOST
(same form), else athena-audio.local. --port sets the audio port (default
7076) and overrides any port from --host or the variable. The matrix board
is a different host with no audio, so ATHENA_MATRIX_HOST is not consulted.

Subcommands: meter [SECONDS], record SECONDS OUT.wav, play FILE.wav
(resampled/downmixed to 16 kHz mono as needed; --gain DB scales it before
sending, clipped at full scale, --normalize lifts its peak to 0 dBFS first),
tone [HZ] [SECONDS].
"""

import argparse
import array
import math
import os
import socket
import sys
import threading
import time
import wave

SAMPLE_RATE = 16000
CHUNK_BYTES = 640                      # 20 ms of 16 kHz mono s16le
BYTES_PER_SEC = SAMPLE_RATE * 2

DEFAULT_HOST = "athena-audio.local"    # the firmware's mDNS name (firmware/athena_audio/main/main.c HOSTNAME)
DEFAULT_PORT = 7076                    # AUDIO_TCP_PORT in main/audio.h

class AudioError(Exception):
    def __init__(self, message, code=1):
        super().__init__(message)
        self.code = code

def parse_hostport(spec):
    """'host' or 'host:port' -> (host, port or None)."""
    host, sep, port = spec.rpartition(":")
    if not sep:
        return spec, None
    try:
        return host, int(port)
    except ValueError:
        raise AudioError("bad host spec %r: expected host or host:port" % spec, code=2)

def resolve_target(args):
    port = args.port
    if args.host:
        host, host_port = parse_hostport(args.host)
        if port is None:
            port = host_port
    else:
        env = os.environ.get("ATHENA_AUDIO_HOST")
        host, env_port = parse_hostport(env) if env else (DEFAULT_HOST, None)
        if port is None:
            port = env_port
    return host, port if port is not None else DEFAULT_PORT

def connect(host, port):
    try:
        sock = socket.create_connection((host, port), timeout=5)
    except socket.gaierror as e:
        raise AudioError("cannot resolve %s: %s (set ATHENA_AUDIO_HOST=<ip> to skip mDNS)" % (host, e), code=2)
    except OSError as e:
        raise AudioError("cannot reach %s:%d: %s (set ATHENA_AUDIO_HOST=<ip> to skip mDNS)" % (host, port, e), code=2)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(2.0)
    return sock

def dbfs(x):
    if x <= 0:
        return -96.0
    return 20.0 * math.log10(x / 32768.0)

def rms_peak(samples):
    if not samples:
        return 0.0, 0
    return math.sqrt(sum(s * s for s in samples) / len(samples)), max(abs(s) for s in samples)

def make_bar(db, width=40, lo=-60.0, hi=0.0):
    frac = max(0.0, min(1.0, (db - lo) / (hi - lo)))
    n = int(round(frac * width))
    return "#" * n + " " * (width - n)

NOTHING_RECEIVED = "board accepted the connection but sent nothing: its audio tasks are not running (check the `audio: mic` log lines)"

def receive_for(sock, seconds):
    """Yield (samples, total_bytes_so_far) for each recv() within `seconds`,
    carrying a trailing odd byte to the next chunk."""
    deadline = time.time() + seconds
    leftover = b""
    total = 0
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            yield array.array("h"), total
            continue
        if not chunk:
            raise AudioError("connection closed by the board")
        total += len(chunk)
        data = leftover + chunk
        data, leftover = (data[:-1], data[-1:]) if len(data) % 2 else (data, b"")
        samples = array.array("h")
        samples.frombytes(data)
        yield samples, total

def cmd_meter(sock, seconds):
    start = time.time()
    window = array.array("h")
    window_start = start
    total_sq = total_count = overall_peak = total_bytes = 0
    for samples, total_bytes in receive_for(sock, seconds):
        if samples:
            window.extend(samples)
            total_sq += sum(s * s for s in samples)
            overall_peak = max(overall_peak, max(abs(s) for s in samples))
            total_count += len(samples)
        now = time.time()
        if now - window_start >= 0.25:
            rms, peak = rms_peak(window)
            print("%6.2f s  rms %5.1f dBFS  peak %5.1f  |%s|" % (now - start, dbfs(rms), dbfs(peak), make_bar(dbfs(rms))))
            window = array.array("h")
            window_start = now
    if total_bytes == 0:
        print(NOTHING_RECEIVED, file=sys.stderr)
        return 3
    overall_rms = math.sqrt(total_sq / total_count) if total_count else 0.0
    print("overall: rms %.1f dBFS  peak %.1f dBFS  bytes %d" % (dbfs(overall_rms), dbfs(overall_peak), total_bytes))
    return 0

def cmd_record(sock, seconds, outpath):
    all_samples = array.array("h")
    total_bytes = 0
    for samples, total_bytes in receive_for(sock, seconds):
        all_samples.extend(samples)
    if total_bytes == 0:
        print(NOTHING_RECEIVED, file=sys.stderr)
        return 3
    rms, peak = rms_peak(all_samples)
    with wave.open(outpath, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(all_samples.tobytes())
    print("rms %.1f dBFS  peak %.1f dBFS  bytes %d" % (dbfs(rms), dbfs(peak), total_bytes))
    print("wrote %s" % outpath)
    return 0

def _downmix(samples, channels):
    n = len(samples) // channels
    return array.array("h", (int(sum(samples[i * channels:(i + 1) * channels]) / channels) for i in range(n)))

def resample_linear(samples, src_rate, dst_rate):
    if src_rate == dst_rate or len(samples) < 2:
        return samples
    n_src = len(samples)
    n_dst = int(round(n_src * dst_rate / float(src_rate)))
    out = array.array("h", bytes(2 * n_dst))
    denom = float(max(n_dst - 1, 1))
    for i in range(n_dst):
        pos = i * (n_src - 1) / denom
        i0 = int(pos)
        i1 = min(i0 + 1, n_src - 1)
        frac = pos - i0
        out[i] = int(samples[i0] * (1 - frac) + samples[i1] * frac)
    return out

def read_wav_16k_mono(path):
    """-> (samples at 16 kHz mono, original rate, original channels)."""
    with wave.open(path, "rb") as w:
        channels = w.getnchannels()
        rate = w.getframerate()
        sampwidth = w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if sampwidth != 2:
        raise AudioError("%s is %d-bit, only 16-bit WAV is supported" % (path, sampwidth * 8), code=2)
    samples = array.array("h")
    samples.frombytes(raw)
    if channels > 1:
        samples = _downmix(samples, channels)
    if rate != SAMPLE_RATE:
        samples = resample_linear(samples, rate, SAMPLE_RATE)
    return samples, rate, channels

def send_samples(sock, samples):
    """Stream signed-16 samples to the board, paced to real time; drain the
    mic concurrently so the board does not drop the connection. -> (duration, bytes drained)."""
    received = [0]
    stop = threading.Event()
    def drain():
        while not stop.is_set():
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if not chunk:
                return
            received[0] += len(chunk)
    threading.Thread(target=drain, daemon=True).start()
    data = samples.tobytes()
    total = len(data)
    sent = 0
    start = time.time()
    try:
        while sent < total:
            end = min(sent + CHUNK_BYTES, total)
            sock.sendall(data[sent:end])
            sent = end
            ahead = (sent / float(BYTES_PER_SEC)) - (time.time() - start)
            if ahead > 0.3:
                time.sleep(ahead - 0.3)
    except OSError as e:
        stop.set()
        raise AudioError("board closed the connection after %.1f s (%d of %d bytes): %s"
                         % (sent / float(BYTES_PER_SEC), sent, total, e), code=1)
    duration = total / float(BYTES_PER_SEC)
    remaining = duration - (time.time() - start)
    time.sleep(max(0.0, remaining) + 0.5)
    stop.set()
    return duration, received[0]

def apply_gain(samples, db):
    """Scale by `db` dB with hard clipping at full scale. -> (samples, clipped count)."""
    g = 10.0 ** (db / 20.0)
    out = array.array("h", bytes(2 * len(samples)))
    clipped = 0
    for i, s in enumerate(samples):
        v = int(round(s * g))
        if v > 32767:
            v, clipped = 32767, clipped + 1
        elif v < -32768:
            v, clipped = -32768, clipped + 1
        out[i] = v
    return out, clipped

def normalize_db(samples):
    """The gain in dB that puts the peak at 0 dBFS (0 for silence)."""
    peak = max((abs(s) for s in samples), default=0)
    return 20.0 * math.log10(32767.0 / peak) if peak else 0.0

def cmd_play(sock, path, gain_db=0.0, normalize=False):
    try:
        samples, rate, channels = read_wav_16k_mono(path)
    except (wave.Error, OSError) as e:
        raise AudioError("cannot read %s: %s" % (path, e), code=2)
    print("%s: %d Hz, %d channel(s), peak %.1f dBFS" % (path, rate, channels, -normalize_db(samples)))
    if normalize:
        gain_db += normalize_db(samples)
    if gain_db:
        samples, clipped = apply_gain(samples, gain_db)
        print("gain %+.1f dB%s" % (gain_db, ", %d samples clipped" % clipped if clipped else ""))
    duration, received = send_samples(sock, samples)
    print("sent %.2f s (drained %d bytes from the board)" % (duration, received))
    return 0

def cmd_tone(sock, hz, seconds):
    n = int(SAMPLE_RATE * seconds)
    amp = 0.25 * 32767
    samples = array.array("h", (int(amp * math.sin(2 * math.pi * hz * i / SAMPLE_RATE)) for i in range(n)))
    duration, received = send_samples(sock, samples)
    print("sent %.2f s of a %.1f Hz tone (drained %d bytes from the board)" % (duration, hz, received))
    return 0

def build_parser():
    epilog = """examples:
  audio.py meter
  audio.py record 3 out.wav
  audio.py play out.wav
  audio.py play --gain 6 out.wav
  audio.py play --normalize out.wav
  audio.py tone 440 2
  audio.py --host 192.168.1.50 --port 7076 tone
"""
    p = argparse.ArgumentParser(
        prog="audio.py",
        description="Stream audio to and from the Athena audio board over TCP.",
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", metavar="HOST[:PORT]", help="the audio board over Wi-Fi (default %s, port %d)" % (DEFAULT_HOST, DEFAULT_PORT))
    p.add_argument("--port", type=int, help="audio TCP port, overrides ATHENA_AUDIO_HOST/--host's port")
    sub = p.add_subparsers(dest="command", required=True)
    m = sub.add_parser("meter", help="print running RMS/peak of the board's mic")
    m.add_argument("seconds", nargs="?", type=float, default=5.0)
    r = sub.add_parser("record", help="record the board's mic to a WAV file")
    r.add_argument("seconds", type=float)
    r.add_argument("out", metavar="OUT.wav")
    pl = sub.add_parser("play", help="play a WAV file through the board's speaker")
    pl.add_argument("file", metavar="FILE.wav")
    pl.add_argument("--gain", type=float, default=0.0, metavar="DB",
                    help="digital gain in dB before sending, clipped at full scale (e.g. 6)")
    pl.add_argument("--normalize", action="store_true",
                    help="lift the file's peak to 0 dBFS first; --gain then applies on top")
    tn = sub.add_parser("tone", help="synthesize and play a sine tone")
    tn.add_argument("hz", nargs="?", type=float, default=440.0)
    tn.add_argument("seconds", nargs="?", type=float, default=2.0)
    return p

def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        host, port = resolve_target(args)
        sock = connect(host, port)
        try:
            if args.command == "meter":
                return cmd_meter(sock, args.seconds)
            if args.command == "record":
                return cmd_record(sock, args.seconds, args.out)
            if args.command == "play":
                return cmd_play(sock, args.file, args.gain, args.normalize)
            if args.command == "tone":
                return cmd_tone(sock, args.hz, args.seconds)
        finally:
            sock.close()
    except AudioError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
