#!/usr/bin/env python3
"""Talk to the Athena LED "face" board over USB serial.

Protocol: one JSON object per line, LF-terminated, at most 256 bytes, at
115200 8N1. The board answers every line with exactly `ok` or `err <reason>`
on the same port, interleaved with its own ESP_LOG output; lines that are
neither `ok` nor start with `err` are board log output and are skipped.
Keys: mode (one of MODES), t (str, max 8 chars, shown by idle/alert), ttl
(int seconds, 0 = sticky), brightness (int 0..255, persists across modes).
An empty object `{}` is a ping and answers `ok`.

Port resolution order: explicit argument/--port, then $ATHENA_MATRIX_PORT,
then the `## Boards` section of ../CLAUDE.md (first existing `matrix*`
device), then a lone /dev/cu.usbserial-*/usbmodem* match.
"""

import argparse
import fcntl
import glob
import json
import os
import re
import select
import struct
import sys
import termios
import time
from datetime import datetime

MODES = ("idle", "listen", "think", "work", "speak", "alert", "error", "sleep", "test", "off")

MODE_HELP = {
    "idle": "nothing happening",
    "listen": "the user is talking or typing; wake word heard",
    "think": "LLM request in flight",
    "work": "a tool is running (shell, MCP, browser)",
    "speak": "the reply is being delivered (later: TTS playing)",
    "alert": "needs the user: a plan waiting for approval, a brief delivered, a question",
    "error": "something failed (tool error, API error, disconnect)",
    "sleep": "night / do not disturb",
    "test": "wiring check",
    "off": "blank",
}

TIME_MODES = ("idle", "alert")

CLAUDE_MD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "CLAUDE.md")

class FaceError(Exception):
    def __init__(self, message, code=1):
        super().__init__(message)
        self.code = code

def now_hhmm():
    return datetime.now().strftime("%H:%M")

def _board_candidates():
    """Yield device paths for `matrix*` entries in CLAUDE.md's Boards section, in file order."""
    try:
        with open(CLAUDE_MD, "r") as f:
            text = f.read()
    except OSError:
        return
    m = re.search(r"^## Boards\b(.*)", text, re.S | re.M)
    if not m:
        return
    for line in m.group(1).splitlines():
        entry = re.match(r"^([a-zA-Z0-9_-]+):\s*(/dev/cu\.\S+)", line.strip())
        if entry and entry.group(1).startswith("matrix"):
            yield entry.group(2)

def find_port(explicit=None):
    if explicit:
        return explicit
    env = os.environ.get("ATHENA_MATRIX_PORT")
    if env:
        return env
    for dev in _board_candidates():
        if os.path.exists(dev):
            return dev
    candidates = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.usbmodem*"))
    if len(candidates) == 1:
        return candidates[0]
    if len(candidates) > 1:
        raise FaceError("multiple USB serial ports found: " + ", ".join(candidates), code=2)
    raise FaceError("no board on USB (plug the DevKit's UART/USB connector in, or set ATHENA_MATRIX_PORT)", code=2)

class Face:
    def __init__(self, port=None, baud=115200, timeout=2.0):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.fd = None
        self.last_log = []

    def open(self):
        port = self.port or find_port()
        self.port = port
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as e:
            if e.errno == 16 or "Resource busy" in str(e):  # EBUSY
                raise FaceError("port is held by another program (a monitor, screen, or IDE serial view)", code=2)
            raise FaceError("cannot open %s: %s" % (port, e), code=2)
        try:
            iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
            iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
                       termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
            oflag &= ~termios.OPOST
            lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
            cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | getattr(termios, "CRTSCTS", 0))
            cflag |= termios.CS8 | termios.CLOCAL | termios.CREAD
            cc[termios.VMIN] = 0
            cc[termios.VTIME] = 0
            ispeed = ospeed = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
            try:
                fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack('i', termios.TIOCM_DTR | termios.TIOCM_RTS))
            except OSError:
                pass    # a pty or a bridge without modem lines; both lines then stay as the driver left them
            fl = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)
            termios.tcflush(fd, termios.TCIOFLUSH)   # drop log lines and stale replies from before we opened
        except Exception as e:
            os.close(fd)
            raise FaceError("cannot configure %s: %s (unplug and replug the board if this persists)" % (port, e), code=2)
        self.fd = fd
        return self

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()

    def send(self, obj):
        if self.fd is None:
            self.open()
        try:
            os.write(self.fd, (json.dumps(obj, separators=(",", ":")) + "\n").encode("ascii"))
            return self._read_reply()
        except OSError as e:
            self.close()
            raise FaceError("serial error on %s: %s" % (self.port, e))

    def _read_reply(self):
        buf = b""
        deadline = time.time() + self.timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise FaceError("no reply from board (is the face firmware flashed?)")
            r, _, _ = select.select([self.fd], [], [], remaining)
            if not r:
                raise FaceError("no reply from board (is the face firmware flashed?)")
            chunk = os.read(self.fd, 4096)
            if not chunk:
                raise FaceError("no reply from board (is the face firmware flashed?)")
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.rstrip(b"\r").decode(errors="replace")
                if text == "ok":
                    return "ok"
                if text.startswith("err"):
                    raise FaceError(text)
                self.last_log.append(text)

    def set(self, mode=None, t=None, ttl=None, brightness=None):
        obj = {}
        if mode is not None:
            obj["mode"] = mode
        if t is not None:
            obj["t"] = t
        if ttl is not None:
            obj["ttl"] = ttl
        if brightness is not None:
            obj["brightness"] = brightness
        return self.send(obj)

    def ping(self):
        return self.send({})

    def brightness(self, n):
        return self.send({"brightness": n})

DEMO_SEQUENCE = [
    ("idle", "nothing happening, the Mac sends the time once a minute"),
    ("listen", "a message arrived / the user is talking"),
    ("think", "the LLM request is in flight"),
    ("work", "a tool is running: shell, Linear, browser"),
    ("speak", "the reply is being delivered"),
    ("alert", "waiting for the user: a plan needs approval"),
    ("error", "something failed, auto-reverts after 10 s"),
    ("sleep", "night mode"),
    ("idle", "nothing happening, the Mac sends the time once a minute"),
]

def build_parser():
    epilog = """examples:
  face.py idle
  face.py alert --t "HI"
  face.py work --ttl 600
  face.py --brightness 60
  face.py --ping
  face.py --demo
  face.py --demo --dry-run --pause 0
  face.py --dry-run work --ttl 600
"""
    p = argparse.ArgumentParser(
        prog="face.py",
        description="Drive the Athena LED face board over USB serial.",
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("mode", nargs="?", choices=MODES, help="one of: " + ", ".join(MODES))
    p.add_argument("--t", dest="t", help="text for idle/alert (max 8 chars)")
    p.add_argument("--no-time", action="store_true", help="do not auto-fill --t with the current time")
    p.add_argument("--ttl", type=int, help="seconds before falling back to idle (0 = sticky)")
    p.add_argument("--brightness", type=int, metavar="0-255", help="0..255, persists across modes")
    p.add_argument("--port", help="serial device, overrides ATHENA_MATRIX_PORT and auto-detect")
    p.add_argument("--ping", action="store_true", help="send {} and expect ok")
    p.add_argument("--demo", action="store_true", help="walk through all agent states")
    p.add_argument("--pause", type=float, default=4.0, help="seconds between --demo steps (default 4)")
    p.add_argument("--dry-run", action="store_true", help="print what would be sent, never opens the port")
    return p

def _send_or_print(face, obj, dry_run):
    if dry_run:
        print("-> " + json.dumps(obj, separators=(",", ":")))
        return "ok (dry run)"
    return face.send(obj)

def main(argv=None):
    args = build_parser().parse_args(argv)

    if args.brightness is not None and not (0 <= args.brightness <= 255):
        print("error: --brightness must be 0..255", file=sys.stderr)
        return 2

    if not args.demo and not args.ping and args.mode is None and args.brightness is None:
        print("error: give a mode, --ping, --demo, or --brightness", file=sys.stderr)
        return 2

    face = None if args.dry_run else Face(port=args.port)

    try:
        if args.demo:
            for mode, narration in DEMO_SEQUENCE:
                print("[%s] %s" % (mode, narration))
                obj = {"mode": mode}
                if mode in TIME_MODES and not args.no_time:
                    obj["t"] = now_hhmm()
                print(_send_or_print(face, obj, args.dry_run))
                if args.pause > 0:
                    time.sleep(args.pause)
            return 0

        if args.ping:
            print(_send_or_print(face, {}, args.dry_run))
            return 0

        if args.mode is None:
            print(_send_or_print(face, {"brightness": args.brightness}, args.dry_run))
            return 0

        obj = {"mode": args.mode}
        if args.t is not None:
            obj["t"] = args.t
        elif args.mode in TIME_MODES and not args.no_time:
            obj["t"] = now_hhmm()
        if args.ttl is not None:
            obj["ttl"] = args.ttl
        if args.brightness is not None:
            obj["brightness"] = args.brightness
        print(_send_or_print(face, obj, args.dry_run))
        return 0
    except FaceError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code
    finally:
        if face is not None:
            face.close()

if __name__ == "__main__":
    sys.exit(main())
