"""athena-face: show the agent's lifecycle on the LED face over Wi-Fi or USB serial.

Hook -> face state (the modes of scripts/face.py):

    pre_gateway_dispatch    listen   a Telegram message arrived
    pre_llm_call            think    turn started (on the CLI this is the first signal)
    pre_api_request         think    every model call in the turn
    pre_tool_call           work
    post_tool_call          think    or error when status == "error" / error_type is set
    api_request_error       error
    post_llm_call           speak    or alert when platform == "cron" (a brief waits)
    pre_approval_request    alert    or think when surface == "smart" (aux LLM decides)
    post_approval_response  think
    on_session_end          idle     or error when failed is true

Hooks only enqueue; one daemon thread owns the connection. It coalesces bursts
(100 ms); an `idle` never displaces `alert` (sticky until other activity),
`speak` or `error` (the board's own ttl ends those, and the worker sends the
idle clock when it does); it holds `error` 2 s and `speak` 3 s before the next
state, sends idle/alert with the clock (`t`) and refreshes it every 60 s.
At interpreter exit it flushes the last queued state so a one-shot
`hermes chat -q` does not leave the face in `think`. Any exception on the
worker is logged and the worker carries on.

Environment: ATHENA_FACE=0 disables the plugin; ATHENA_FACE_DRY_RUN=1 prints
each JSON line to stderr instead of opening a connection; ATHENA_MATRIX_HOST
is the board over Wi-Fi (host[:port], e.g. athena-matrix.local) and
ATHENA_MATRIX_PORT the serial device (with neither, scripts/face.py takes a
board on USB, else athena-matrix.local); ATHENA_FACE_BRIGHTNESS is
0..255, sent after every (re)open; ATHENA_FACE_SLEEP is HH:MM-HH:MM (may wrap
midnight), inside it idle shows as sleep.
"""

import atexit
import json
import logging
import os
import queue
import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

log = logging.getLogger("athena-face")

_REPO_ROOT = Path(__file__).resolve().parents[3]
_BOARD_TTL = {"listen": 30, "think": 120, "work": 300, "speak": 8, "error": 10}  # else sticky
_MIN_HOLD = {"error": 2.0, "speak": 3.0}
_IDLE_PROOF = ("alert", "speak", "error")   # an idle in the same burst does not cancel these
_STOP = object()                             # queue sentinel from close()
_CLOCK_MODES = ("idle", "alert")
_RESTING = ("idle", "alert", "sleep")
_COALESCE_S, _CLOCK_REFRESH_S, _RETRY_S = 0.1, 60.0, 30.0
_FIXED = {"pre_gateway_dispatch": "listen", "pre_llm_call": "think", "pre_api_request": "think",
          "pre_tool_call": "work", "api_request_error": "error", "post_approval_response": "think"}
_driver = None


def _import_face():
    scripts = str(_REPO_ROOT / "scripts")
    sys.path.insert(0, scripts)
    try:
        import face  # scripts/face.py: Face, FaceError, now_hhmm
    finally:
        if sys.path and sys.path[0] == scripts:
            sys.path.pop(0)
    return face


def _env(name, parse):
    raw = os.environ.get(name, "").strip()
    try:
        return parse(raw) if raw else None
    except ValueError as exc:
        log.warning("ignoring %s=%r: %s", name, raw, exc)
        return None


def _brightness(raw):
    n = int(raw)
    if not 0 <= n <= 255:
        raise ValueError("must be 0..255")
    return n


def _window(raw):
    m = re.fullmatch(r"(\d{1,2}):(\d{2})-(\d{1,2}):(\d{2})", raw)
    h1, m1, h2, m2 = (int(x) for x in m.groups()) if m else (99, 0, 0, 0)
    if max(h1, h2) > 23 or max(m1, m2) > 59:
        raise ValueError("expected HH:MM-HH:MM")
    return h1 * 60 + m1, h2 * 60 + m2


def _in_window(window, now=None):
    if window is None:
        return False
    now = now or datetime.now()
    minute, (start, end) = now.hour * 60 + now.minute, window
    return start <= minute < end if start <= end else (minute >= start or minute < end)


class _Driver:
    """Owns the connection to the board. Hooks call push(); everything else runs on the worker."""

    def __init__(self, face, dry_run, brightness, window):
        self._mod, self._dry_run, self._brightness, self._window = face, dry_run, brightness, window
        self._face = face.Face(port=os.environ.get("ATHENA_MATRIX_PORT") or None,
                               host=os.environ.get("ATHENA_MATRIX_HOST") or None)
        self._q, self._lock, self._thread = queue.Queue(), threading.Lock(), None
        self._connected, self._warned, self._retry_at = False, False, 0.0

    def push(self, state):  # agent thread: enqueue and return
        if self._thread is None:
            with self._lock:
                if self._thread is None:
                    self._thread = threading.Thread(target=self._run, name="athena-face", daemon=True)
                    self._thread.start()
        self._q.put_nowait(state)

    @staticmethod
    def _fold(wanted, new):
        # idle never displaces alert (sticky), speak or error (the board's ttl ends them)
        return wanted if new == "idle" and wanted in _IDLE_PROOF else new

    def close(self):  # atexit: push the last state out before the daemon thread is abandoned
        t = self._thread
        if t is None or not t.is_alive():
            return
        self._q.put_nowait(_STOP)
        t.join(timeout=2.0)

    @staticmethod
    def _due(wanted, shown_at):
        if wanted in _CLOCK_MODES:
            return shown_at + _CLOCK_REFRESH_S
        return shown_at + _BOARD_TTL[wanted] if wanted in _BOARD_TTL else None

    def _get(self, deadline):  # one queued state, or None at the deadline (None = forever)
        try:
            return self._q.get(timeout=None if deadline is None else max(0.0, deadline - time.monotonic()))
        except queue.Empty:
            return None

    def _run(self):
        wanted = shown = None
        pending = force = stop = False
        shown_at = hold_until = 0.0
        while not stop:
            try:
                state = self._get(hold_until if pending else self._due(wanted, shown_at))
                if state is _STOP:
                    stop = True
                elif state is not None:
                    wanted, pending, end = self._fold(wanted, state), True, time.monotonic() + _COALESCE_S
                    while True:                     # coalesce the rest of the burst
                        state = self._get(end)
                        if state is None:
                            break
                        if state is _STOP:
                            stop = True
                            break
                        wanted = self._fold(wanted, state)
                elif not pending and wanted is not None:  # timer: clock refresh or ttl fallback
                    force, wanted, pending = wanted in _CLOCK_MODES, wanted if wanted in _CLOCK_MODES else "idle", True
                now = time.monotonic()
                if not pending or (now < hold_until and not stop):
                    continue
                pending = False
                mode = "sleep" if wanted == "idle" and _in_window(self._window) else wanted
                ttl = _BOARD_TTL.get(mode, 0)
                if mode == shown and (ttl == 0 or now - shown_at < ttl) and not (force and mode in _CLOCK_MODES):
                    continue  # already on screen
                force = False
                ok = self._send_mode(mode)
                shown, shown_at = (mode if ok else None), now
                hold_until = now + _MIN_HOLD.get(mode, 0.0) if ok else 0.0
            except Exception as exc:                # never let the worker die
                log.warning("athena-face worker: %s", exc)
                self._fail(str(exc))

    def _send_mode(self, mode):
        obj = {"mode": mode}
        if mode in _CLOCK_MODES:
            obj["t"] = self._mod.now_hhmm()
        if mode in _RESTING:
            obj["ttl"] = 0
        if not self._connected and (time.monotonic() < self._retry_at or not self._open()):
            return False
        try:
            self._write(obj)
            return True
        except Exception as exc:
            self._fail("send failed: %s" % exc)
            return False

    def _open(self):
        try:
            if not self._dry_run:
                self._face.open()
            if self._brightness is not None:
                self._write({"brightness": self._brightness})
        except Exception as exc:
            self._fail("cannot open the face board: %s" % exc)
            return False
        self._connected, self._warned = True, False
        log.info("face board connected on %s", "dry run" if self._dry_run else self._face.where)
        return True

    def _write(self, obj):
        line = json.dumps(obj, separators=(",", ":"))
        if self._dry_run:
            print("athena-face -> " + line, file=sys.stderr, flush=True)
        else:
            self._face.send(obj)
        log.debug("sent %s", line)

    def _fail(self, msg):
        try:
            self._face.close()
        except Exception:
            pass
        self._connected, self._retry_at = False, time.monotonic() + _RETRY_S
        (log.debug if self._warned else log.warning)("%s (retrying in %.0f s)", msg, _RETRY_S)
        self._warned = True


def _callbacks(d):
    def fixed(state):
        def cb(**kwargs):
            d.push(state)
        cb.__name__ = "face_" + state
        return cb

    def post_tool_call(status=None, error_type=None, **kwargs):
        d.push("error" if status == "error" or error_type else "think")

    def post_llm_call(platform="", **kwargs):
        d.push("alert" if platform == "cron" else "speak")

    def pre_approval_request(surface="", **kwargs):
        d.push("think" if surface == "smart" else "alert")

    def on_session_end(failed=False, **kwargs):
        d.push("error" if failed else "idle")

    hooks = {name: fixed(state) for name, state in _FIXED.items()}
    hooks.update(post_tool_call=post_tool_call, post_llm_call=post_llm_call,
                 pre_approval_request=pre_approval_request, on_session_end=on_session_end)
    return hooks


def register(ctx):
    global _driver
    if os.environ.get("ATHENA_FACE", "1").strip().lower() in ("0", "false", "no", "off"):
        log.info("athena-face disabled by ATHENA_FACE=%s", os.environ["ATHENA_FACE"])
        return
    try:
        face = _import_face()
    except Exception as exc:
        log.warning("athena-face disabled: cannot import scripts/face.py: %s", exc)
        return
    dry_run = os.environ.get("ATHENA_FACE_DRY_RUN", "").strip().lower() in ("1", "true", "yes")
    _driver = _Driver(face, dry_run, _env("ATHENA_FACE_BRIGHTNESS", _brightness), _env("ATHENA_FACE_SLEEP", _window))
    for name, cb in _callbacks(_driver).items():
        ctx.register_hook(name, cb)
    _driver.push("idle")            # show the clock as soon as Hermes is up, not only after the first turn
    atexit.register(_driver.close)
    log.info("athena-face enabled (%s)", "dry run" if dry_run else "board %s" % (_driver._face.where or "auto"))
