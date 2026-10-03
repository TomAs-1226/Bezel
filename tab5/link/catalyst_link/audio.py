"""The PC's sound, for the tablet's speaker: WASAPI loopback of the default output, streamed as raw PCM.

Windows lets a program capture what an output device is playing ("loopback"), which is every app at once.
`pyaudiowpatch` is PyAudio with that added; it installs from a wheel and needs nothing else. The capture runs
only while at least one tablet is listening, and follows the default output device when it changes.

What goes over the wire (GET /audio/stream, the same port and token as the rest of the API):

    HTTP 200, then forever: packets of  <BBHII> type, flags, n, seq, t_ms  followed by n samples
    - 48 kHz, mono, signed 16-bit little-endian (the Tab5's speaker is one speaker; stereo is mixed down here)
    - type 1 is audio. flags bit 0 means "idle": nothing is playing on the PC, n is 0, and this is only a
      keepalive (every 250 ms), so a quiet PC costs the tablet's Wi-Fi nothing and the tablet knows the
      silence is not a dropout.
    - t_ms is this PC's monotonic clock in milliseconds when the packet was sent.
    The tablet answers on the same connection with 16-byte frames  <BBHIII> type 3, 0, fill_ms, seq,
    underruns, tablet_ms: how much it has queued, the last packet it has, how often it ran dry. The Link
    times the round trip from them (a packet's send time against its ack), which is how the latency is measured.

Never fatal: with no audio device, or without pyaudiowpatch, `status()` says why and /audio/stream answers 503.
"""
from __future__ import annotations

import json
import socket
import struct
import sys
import threading
import time
from collections import deque
from pathlib import Path
from typing import Any, Callable

try:  # IMA ADPCM at C speed; gone from Python 3.13, and then the stream is raw PCM as before
    import audioop  # type: ignore[import-not-found]
except Exception:  # pragma: no cover
    audioop = None  # type: ignore[assignment]

RATE = 48000
CHANNELS = 1
PACKET = struct.Struct("<BBHII")
ACK = struct.Struct("<BBHIII")
IDLE_AFTER_S = 0.25        # no audio for this long: keepalives instead of silence
KEEPALIVE_S = 0.25
TICK_S = 0.020
MAX_BACKLOG_S = 0.30       # a client that can't keep up loses the oldest audio rather than gaining latency

VBCABLE_HELP = (
    "This PC has no active output device, so there is nothing to capture. Install a virtual audio cable "
    "(VB-CABLE, from vb-audio.com) and set 'CABLE Input' as the default playback device, then restart "
    "Catalyst Link: see 'Sound to the tablet' in tab5/link/README.md."
)


class Unavailable(Exception):
    pass


def _import() -> Any:
    try:
        import numpy  # noqa: F401
        import pyaudiowpatch  # type: ignore[import-not-found]
    except ImportError as exc:
        raise Unavailable(
            "install it with: python -m pip install \".[audio]\" (in tab5/link), then restart the link"
        ) from exc
    return pyaudiowpatch


class Subscription:
    """One listener's share of the capture: mono 48 kHz s16le bytes, newest last, bounded."""

    def __init__(self) -> None:
        self.chunks: deque[bytes] = deque()
        self.bytes = 0
        self.cond = threading.Condition()
        self.last_audio = 0.0

    def push(self, data: bytes) -> None:
        with self.cond:
            self.chunks.append(data)
            self.bytes += len(data)
            limit = int(RATE * 2 * MAX_BACKLOG_S)
            while self.bytes > limit and len(self.chunks) > 1:
                self.bytes -= len(self.chunks.popleft())
            self.last_audio = time.monotonic()
            self.cond.notify()

    def take(self, limit_bytes: int = 1 << 20) -> bytes:
        with self.cond:
            if not self.chunks:
                return b""
            data = b"".join(self.chunks)
            self.chunks.clear()
            self.bytes = 0
        return data[-limit_bytes:] if len(data) > limit_bytes else data


def to_mono48(raw: bytes, channels: int, rate: int, state: dict[str, Any]) -> bytes:
    """Interleaved s16le at `rate` → mono s16le at 48 kHz. Stereo is averaged; a rate other than 48 kHz is
    taken there by linear interpolation that carries its position across chunks (`state`)."""
    import numpy as np

    x = np.frombuffer(raw, dtype="<i2")
    if channels > 1:
        x = x[: len(x) // channels * channels].reshape(-1, channels).astype(np.float32).mean(axis=1)
    else:
        x = x.astype(np.float32)
    if rate != RATE and len(x):
        step = rate / RATE
        prev = state.get("last")
        if prev is not None:
            x = np.concatenate(([prev], x))
        pos = state.get("pos", 0.0) if prev is not None else 0.0
        n_out = int(max(0, (len(x) - 1 - pos) / step) + 1) if len(x) > 1 else 0
        idx = pos + np.arange(n_out) * step
        idx = idx[idx <= len(x) - 1]
        y = np.interp(idx, np.arange(len(x)), x)
        state["pos"] = (idx[-1] + step - (len(x) - 1)) if len(idx) else pos - (len(x) - 1)
        state["last"] = x[-1]
        x = y
    return np.clip(np.rint(x), -32768, 32767).astype("<i2").tobytes()


class Capture:
    """The shared loopback capture. start() on the first listener, stop() on the last."""

    def __init__(self, pa_factory: Callable[[], Any] | None = None) -> None:
        self._factory = pa_factory
        self.lock = threading.Lock()
        self.subs: set[Subscription] = set()
        self.thread: threading.Thread | None = None
        self.stop_flag = threading.Event()
        self.device = ""
        self.reason = ""
        self.frames = 0
        self.errors = 0
        self.last_data = 0.0

    # -- discovery -------------------------------------------------------------------------------------

    def _pa(self) -> Any:
        if self._factory:
            return self._factory()
        return _import().PyAudio()

    def probe(self) -> dict[str, Any]:
        """What Windows exposes right now, without capturing anything."""
        try:
            pa = self._pa()
        except Unavailable as exc:
            return {"ok": False, "reason": str(exc), "devices": []}
        try:
            devices = [d["name"] for d in pa.get_loopback_device_info_generator()]
            try:
                default = pa.get_default_wasapi_loopback()["name"]
            except Exception:  # noqa: BLE001 - no default render device at all
                default = ""
            if default:
                return {"ok": True, "default": default, "devices": devices}
            cable = [n for n in devices if "cable" in n.lower()]
            return {"ok": False, "reason": VBCABLE_HELP, "devices": devices, "cable": bool(cable)}
        finally:
            pa.terminate()

    # -- running ---------------------------------------------------------------------------------------

    def subscribe(self) -> Subscription:
        sub = Subscription()
        with self.lock:
            self.subs.add(sub)
            if self.thread is None or not self.thread.is_alive():
                self.stop_flag.clear()
                self.thread = threading.Thread(target=self._run, name="audio-capture", daemon=True)
                self.thread.start()
        return sub

    def unsubscribe(self, sub: Subscription) -> None:
        with self.lock:
            self.subs.discard(sub)
            if not self.subs:
                self.stop_flag.set()

    def _fan_out(self, data: bytes) -> None:
        self.last_data = time.monotonic()
        for sub in list(self.subs):
            sub.push(data)

    def _open(self) -> tuple[Any, Any, dict[str, Any]]:
        pa = self._pa()
        try:
            dev = pa.get_default_wasapi_loopback()
        except Exception:  # noqa: BLE001
            pa.terminate()
            raise Unavailable(VBCABLE_HELP) from None
        rate, ch = int(dev["defaultSampleRate"]), int(dev["maxInputChannels"])
        state: dict[str, Any] = {}

        def callback(in_data: bytes, frame_count: int, ti: Any, status: Any) -> tuple[None, int]:
            try:
                self.frames += frame_count
                self._fan_out(to_mono48(in_data, ch, rate, state))
            except Exception:  # noqa: BLE001 - never let the audio callback raise
                self.errors += 1
            return (None, _import().paContinue)

        stream = pa.open(format=_import().paInt16, channels=ch, rate=rate, input=True,
                         input_device_index=dev["index"], frames_per_buffer=rate // 100,
                         stream_callback=callback)
        self.device = dev["name"].replace(" [Loopback]", "")
        return pa, stream, dev

    def _run(self) -> None:
        pa = stream = None
        current = None
        try:
            while not self.stop_flag.is_set():
                if stream is None:
                    try:
                        pa, stream, dev = self._open()
                        current = dev["index"]
                        self.reason = ""
                    except Unavailable as exc:
                        self.reason = str(exc)
                        self.device = ""
                        self.stop_flag.wait(2.0)
                        continue
                    except Exception as exc:  # noqa: BLE001 - a device that vanished between list and open
                        self.reason = f"could not open the output device: {exc}"
                        self.errors += 1
                        self.stop_flag.wait(2.0)
                        continue
                self.stop_flag.wait(2.0)
                # the default output changed (headphones plugged in, a Bluetooth device): follow it
                try:
                    probe = self._pa()
                    try:
                        now = probe.get_default_wasapi_loopback()["index"]
                    except Exception:  # noqa: BLE001
                        now = None
                    finally:
                        probe.terminate()
                except Unavailable:
                    now = current
                if now != current or not stream.is_active():
                    stream.close()
                    pa.terminate()
                    pa = stream = None
        finally:
            if stream is not None:
                try:
                    stream.stop_stream()
                    stream.close()
                except Exception:  # noqa: BLE001
                    pass
            if pa is not None:
                pa.terminate()
            self.device = ""


class Stats:
    def __init__(self) -> None:
        self.rtt_ms = 0.0
        self.rtt_min_ms = 0.0
        self.rtt_max_ms = 0.0
        self.fill_ms = 0
        self.underruns = 0
        self.acks = 0
        self.rtts: deque[float] = deque(maxlen=200)


class AudioService:
    """Owns the capture, the on/off switch (kept in the Link's state directory) and what the tablets report."""

    def __init__(self, state_dir: Path | None = None, capture: Capture | None = None, enabled: bool = True) -> None:
        self.capture = capture or Capture()
        self.path = state_dir / "audio.json" if state_dir else None
        self.enabled = enabled and self._load(True)
        self.lock = threading.Lock()
        self.clients: dict[int, dict[str, Any]] = {}
        self._next = 1

    def _load(self, default: bool) -> bool:
        if self.path and self.path.exists():
            try:
                return bool(json.loads(self.path.read_text("utf-8")).get("enabled", default))
            except (OSError, ValueError):
                return default
        return default

    def set_enabled(self, on: bool) -> None:
        self.enabled = bool(on)
        if self.path:
            try:
                self.path.write_text(json.dumps({"enabled": self.enabled}), "utf-8")
            except OSError:
                pass

    def status(self) -> dict[str, Any]:
        with self.lock:
            clients = [{"ip": c["ip"], "since": round(time.time() - c["since"], 1), "packets": c["packets"],
                        "rtt_ms": round(c["stats"].rtt_ms, 1), "fill_ms": c["stats"].fill_ms,
                        "underruns": c["stats"].underruns} for c in self.clients.values()]
        out: dict[str, Any] = {"ok": True, "enabled": self.enabled, "rate": RATE, "channels": CHANNELS,
                               "listening": len(clients), "clients": clients,
                               "device": self.capture.device, "reason": self.capture.reason}
        if sys.platform != "win32":
            out["reason"] = "sound to the tablet is Windows only (WASAPI loopback)"
        return out

    # -- one tablet's connection ---------------------------------------------------------------------------

    def serve(self, wfile: Any, rfile: Any, sock: Any, ip: str, stop: threading.Event | None = None) -> None:
        """Stream until the tablet goes away or the switch turns off. Runs on the HTTP handler's thread."""
        sub = self.capture.subscribe()
        stats = Stats()
        info = {"ip": ip, "since": time.time(), "packets": 0, "stats": stats}
        with self.lock:
            cid = self._next
            self._next += 1
            self.clients[cid] = info
        sent: dict[int, float] = {}
        done = threading.Event()
        last_ack = [time.monotonic()]
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16384)  # a stall must not queue seconds of audio
            sock.settimeout(3.0)
        except OSError:
            pass

        def reader() -> None:
            buf = b""
            try:
                while not done.is_set():
                    try:
                        chunk = sock.recv(256)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    buf += chunk
                    while len(buf) >= ACK.size:
                        t, _f, fill, seq, under, _tab = ACK.unpack(buf[: ACK.size])
                        buf = buf[ACK.size:]
                        if t != 3:
                            continue
                        stats.fill_ms, stats.underruns = fill, under
                        last_ack[0] = time.monotonic()
                        t0 = sent.pop(seq, None)
                        if t0 is not None:
                            rtt = (time.monotonic() - t0) * 1000
                            stats.rtts.append(rtt)
                            stats.rtt_ms = rtt
                            stats.acks += 1
                        for k in [k for k in sent if k < seq]:
                            sent.pop(k, None)
            except OSError:
                pass
            finally:
                done.set()

        threading.Thread(target=reader, name="audio-acks", daemon=True).start()
        seq = 0
        adpcm_state: Any = None  # audioop's (valprev, index), carried from packet to packet
        next_tick = time.monotonic()
        last_ka = 0.0
        t_start = time.monotonic()
        try:
            while not done.is_set() and self.enabled and not (stop and stop.is_set()):
                if time.monotonic() - last_ack[0] > 8.0:
                    break  # the tablet stopped answering: Wi-Fi gone, or it rebooted
                next_tick += TICK_S
                delay = next_tick - time.monotonic()
                if delay > 0:
                    done.wait(delay)
                else:
                    next_tick = time.monotonic()
                data = sub.take()
                now = time.monotonic()
                if data:
                    n = len(data) // 2
                    flags = 0
                elif now - sub.last_audio >= IDLE_AFTER_S and now - last_ka >= KEEPALIVE_S:
                    n, flags, last_ka = 0, 1, now
                else:
                    continue
                seq += 1
                t_ms = int((now - t_start) * 1000) & 0xFFFFFFFF
                # a long gap is cut into packets the tablet's small frames can take (n is 16 bits)
                if n > 0:
                    while data:
                        part, data = data[:4800], data[4800:]
                        if audioop is not None:
                            # four bits a sample: 24 KB/s where raw PCM is 96, which the tablet's Wi-Fi chip
                            # cannot carry for long. The coder's state before this packet goes with it, so a
                            # lost packet costs only itself.
                            vp, ix = adpcm_state if adpcm_state else (0, 0)
                            body, adpcm_state = audioop.lin2adpcm(part, 2, adpcm_state)
                            wfile.write(PACKET.pack(1, 2, len(part) // 2, seq & 0xFFFFFFFF, t_ms)
                                        + struct.pack("<hBB", vp, ix, 0) + body)
                        else:
                            wfile.write(PACKET.pack(1, 0, len(part) // 2, seq & 0xFFFFFFFF, t_ms) + part)
                        sent[seq] = now
                        info["packets"] += 1
                        seq += 1
                else:
                    wfile.write(PACKET.pack(1, flags, 0, seq & 0xFFFFFFFF, t_ms))
                    sent[seq] = now
                wfile.flush()
        except (OSError, ValueError):
            pass
        finally:
            done.set()
            self.capture.unsubscribe(sub)
            with self.lock:
                self.clients.pop(cid, None)
