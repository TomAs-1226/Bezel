"""The PC's media remote: what is playing on this PC, and play/pause/next/previous/volume, for the tablet's
home mode (docs/link-api.md, "Media").

Windows reports whatever is playing — Spotify, YouTube Music in a browser or as an app, a video in
Edge — through the Global System Media Transport Controls (GSMTC), the same source as the volume
flyout's media card. The Link reads it through the WinRT projections: the `winrt-Windows.Media.Control`
packages (pywinrt 2.x) or the older all-in-one `winsdk`. Neither is required: without them, or on
another OS, the endpoints answer `"available": false` with the reason, and nothing else changes.

Volume is the PC's master volume: set to a level with `pycaw` when it is installed, and stepped or
muted with the keyboard's media keys otherwise (no dependency).

Album art is the session's thumbnail. The tablet asks for it as RGB565 pixels at an exact square size
(it has no PNG decoder and its JPEG decoder is fussy), which needs Pillow; plain `format=jpeg` works
without Pillow when the thumbnail already is a JPEG.

Everything that touches the OS sits behind `Platform`, so the tests run anywhere with a fake one.
"""
from __future__ import annotations

import asyncio
import base64
import hashlib
import io
import sys
import threading
import time
from datetime import datetime, timedelta
from typing import Any, Protocol

from .state import LinkError

# GSMTC's PlaybackStatus enum
STATUS = {0: "closed", 1: "opened", 2: "changing", 3: "stopped", 4: "playing", 5: "paused"}
ACTIONS = ("play", "pause", "toggle", "next", "previous", "stop", "volume_up", "volume_down", "mute", "volume")
ART_MAX = 512           # px: the largest square the tablet may ask for
ART_DEFAULT = 160

# App user model ids → what a person calls them. Browsers' PWAs carry their extension id.
APP_NAMES = {
    "spotify": "Spotify",
    "cinhimbnkkaeohfgghhklpknlkffjgod": "YouTube Music",   # the YouTube Music PWA (Chrome and Edge)
    "music.youtube": "YouTube Music",
    "msedge": "Edge",
    "chrome": "Chrome",
    "firefox": "Firefox",
    "308046b0af4a39cb": "Firefox",
    "brave": "Brave",
    "opera": "Opera",
    "zunemusic": "Media Player",
    "microsoft.media": "Media Player",
    "applemusic": "Apple Music",
    "itunes": "iTunes",
    "vlc": "VLC",
    "foobar2000": "foobar2000",
    "tidal": "TIDAL",
    "deezer": "Deezer",
    "amazon music": "Amazon Music",
}


def app_name(aumid: str) -> str:
    low = (aumid or "").lower()
    for key, name in APP_NAMES.items():
        if key in low:
            return name
    base = (aumid or "").split("!")[-1].split("\\")[-1]
    if base.lower().endswith(".exe"):
        base = base[:-4]
    return base[:32] or "an app"


class Platform(Protocol):
    """What the OS must answer. Implemented by WindowsPlatform; tests pass a fake."""

    def session(self) -> dict[str, Any] | None:
        """The current media session: title, artist, album, app (the AUMID), status (GSMTC int),
        position and duration (seconds, None when unknown), updated (epoch seconds of the position),
        can (dict of play/pause/next/previous booleans), thumbnail (bytes or None). None: nothing."""
        ...

    def control(self, action: str) -> bool:
        """play, pause, toggle, next, previous, stop on the current session. True if it was accepted."""
        ...

    def volume(self) -> tuple[float | None, bool | None]:
        """Master volume 0..1 and mute, or None where they can't be read."""
        ...

    def set_volume(self, level: float) -> bool:
        ...

    def volume_key(self, which: str) -> bool:
        """"up", "down" or "mute", as the keyboard's media keys."""
        ...


class Unavailable(Exception):
    """The platform can't serve media at all; the message says why."""


class WindowsPlatform:
    """GSMTC through pywinrt (or winsdk), on a thread of its own with its own event loop."""

    def __init__(self) -> None:
        if sys.platform != "win32":
            raise Unavailable("the media remote needs Windows")
        self.mc, self.streams = self._import()
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run, name="link-media", daemon=True)
        self._thread.start()
        self._manager: Any = None
        self._thumb_key: Any = None
        self._thumb: bytes | None = None
        self._pycaw: Any = None
        try:
            self._pycaw = self._volume_endpoint()
        except Exception:
            self._pycaw = None

    @staticmethod
    def _import() -> tuple[Any, Any]:
        import importlib
        for mc_name, st_name in (("winrt.windows.media.control", "winrt.windows.storage.streams"),
                                 ("winsdk.windows.media.control", "winsdk.windows.storage.streams")):
            try:
                return importlib.import_module(mc_name), importlib.import_module(st_name)
            except Exception:  # ImportError, or a projection that fails to load
                continue
        raise Unavailable("install the WinRT media packages: pip install winrt-Windows.Media.Control "
                          "winrt-Windows.Storage.Streams winrt-Windows.Foundation")

    def _run(self) -> None:
        try:  # pywinrt wants the thread in an apartment; winsdk initializes itself
            import winrt  # type: ignore
            init = getattr(winrt, "init_apartment", None)
            if init:
                init()
        except Exception:
            pass
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    def _call(self, coro: Any, timeout: float = 5.0) -> Any:
        return asyncio.run_coroutine_threadsafe(coro, self._loop).result(timeout)

    async def _session(self) -> Any:
        if self._manager is None:
            self._manager = await self.mc.GlobalSystemMediaTransportControlsSessionManager.request_async()
        return self._manager.get_current_session()

    @staticmethod
    def _seconds(v: Any) -> float | None:
        if v is None:
            return None
        if isinstance(v, timedelta):
            return v.total_seconds()
        dur = getattr(v, "duration", None)  # a TimeSpan struct in 100 ns ticks
        if isinstance(dur, int):
            return dur / 1e7
        try:
            return float(v)
        except (TypeError, ValueError):
            return None

    async def _read_thumb(self, ref: Any) -> bytes | None:
        stream = await ref.open_read_async()
        size = int(stream.size)
        if size <= 0 or size > 8 * 1024 * 1024:
            return None
        reader = self.streams.DataReader(stream.get_input_stream_at(0))
        await reader.load_async(size)
        try:
            return bytes(reader.read_buffer(size))       # pywinrt 2: IBuffer has the buffer protocol
        except Exception:
            data = bytearray(size)
            reader.read_bytes(data)                      # winsdk: fills a writable buffer
            return bytes(data)

    async def _snapshot(self) -> dict[str, Any] | None:
        s = await self._session()
        if s is None:
            return None
        props = await s.try_get_media_properties_async()
        info = s.get_playback_info()
        tl = s.get_timeline_properties()
        controls = getattr(info, "controls", None)

        def can(name: str) -> bool:
            return bool(getattr(controls, name, False)) if controls is not None else False

        start = self._seconds(getattr(tl, "start_time", None)) or 0.0
        end = self._seconds(getattr(tl, "end_time", None))
        pos = self._seconds(getattr(tl, "position", None))
        updated = getattr(tl, "last_updated_time", None)
        updated_s = updated.timestamp() if isinstance(updated, datetime) else None
        key = (s.source_app_user_model_id, props.title, props.artist, props.album_title)
        if key != self._thumb_key:
            self._thumb_key = key
            self._thumb = None
            if props.thumbnail is not None:
                try:
                    self._thumb = await self._read_thumb(props.thumbnail)
                except Exception:
                    self._thumb = None
        status = getattr(info, "playback_status", None)
        return {
            "title": props.title or "", "artist": props.artist or "", "album": props.album_title or "",
            "app": s.source_app_user_model_id or "",
            "status": int(status) if status is not None else 0,
            "position": None if pos is None else max(0.0, pos - start),
            "duration": None if end is None or end <= start else end - start,
            "updated": updated_s,
            "can": {"play": can("is_play_enabled"), "pause": can("is_pause_enabled"),
                    "next": can("is_next_enabled"), "previous": can("is_previous_enabled")},
            "thumbnail": self._thumb,
        }

    def session(self) -> dict[str, Any] | None:
        return self._call(self._snapshot())

    async def _control(self, action: str) -> bool:
        s = await self._session()
        if s is None:
            return False
        method = {"play": "try_play_async", "pause": "try_pause_async", "toggle": "try_toggle_play_pause_async",
                  "next": "try_skip_next_async", "previous": "try_skip_previous_async",
                  "stop": "try_stop_async"}[action]
        return bool(await getattr(s, method)())

    def control(self, action: str) -> bool:
        return self._call(self._control(action))

    # --- volume -----------------------------------------------------------------------------------------

    @staticmethod
    def _volume_endpoint() -> Any:
        from ctypes import POINTER, cast

        from comtypes import CLSCTX_ALL  # type: ignore
        from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume  # type: ignore
        dev = AudioUtilities.GetSpeakers()
        # pycaw 2025+ wraps the device (AudioDevice) and hands out the endpoint itself; older ones
        # return the raw IMMDevice, which has to be activated
        endpoint = getattr(dev, "EndpointVolume", None)
        if endpoint is not None:
            return endpoint
        iface = dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
        return cast(iface, POINTER(IAudioEndpointVolume))

    def volume(self) -> tuple[float | None, bool | None]:
        if self._pycaw is None:
            return None, None
        try:
            return float(self._pycaw.GetMasterVolumeLevelScalar()), bool(self._pycaw.GetMute())
        except Exception:
            return None, None

    def set_volume(self, level: float) -> bool:
        if self._pycaw is None:
            return False
        try:
            self._pycaw.SetMasterVolumeLevelScalar(max(0.0, min(1.0, level)), None)
            return True
        except Exception:
            return False

    def volume_key(self, which: str) -> bool:
        import ctypes
        vk = {"up": 0xAF, "down": 0xAE, "mute": 0xAD}[which]
        user32 = ctypes.windll.user32  # type: ignore[attr-defined]
        user32.keybd_event(vk, 0, 0, 0)
        user32.keybd_event(vk, 0, 2, 0)  # KEYEVENTF_KEYUP
        return True


def _to_rgb565(data: bytes, size: int) -> bytes:
    """Any image Pillow reads → size×size, cropped to fill (centre), RGB565 little-endian, rows packed."""
    from PIL import Image  # optional: raises ImportError without Pillow
    im = Image.open(io.BytesIO(data)).convert("RGB")
    w, h = im.size
    side = min(w, h)
    im = im.crop(((w - side) // 2, (h - side) // 2, (w - side) // 2 + side, (h - side) // 2 + side))
    im = im.resize((size, size), Image.LANCZOS)
    raw = im.tobytes()
    out = bytearray(size * size * 2)
    j = 0
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[j] = v & 0xFF
        out[j + 1] = v >> 8
        j += 2
    return bytes(out)


def _to_jpeg(data: bytes, size: int) -> bytes:
    try:
        from PIL import Image
    except ImportError:
        if data[:2] == b"\xff\xd8":
            return data  # already a JPEG; the size is whatever the app gave
        raise
    im = Image.open(io.BytesIO(data)).convert("RGB")
    im.thumbnail((size, size), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=85, optimize=False, progressive=False)  # baseline: the P4's decoder
    return buf.getvalue()


class Media:
    """What the HTTP handlers call. `platform` None: find the real one (or record why there is none)."""

    def __init__(self, platform: Platform | None = None, enabled: bool = True) -> None:
        self.enabled = enabled
        self.reason = ""
        self.platform: Platform | None = None
        self._lock = threading.Lock()
        self._art_cache: tuple[str, int, str, bytes] | None = None
        if not enabled:
            self.reason = "the media remote is off on this Link (--no-media)"
        elif platform is not None:
            self.platform = platform
        else:
            try:
                self.platform = WindowsPlatform()
            except Unavailable as exc:
                self.reason = str(exc)
            except Exception as exc:  # a broken projection must never stop the Link
                self.reason = f"the media remote couldn't start: {exc}"

    @property
    def available(self) -> bool:
        return self.platform is not None

    def _session(self) -> dict[str, Any] | None:
        assert self.platform is not None
        try:
            with self._lock:
                return self.platform.session()
        except Exception as exc:
            raise LinkError(502, f"couldn't read the PC's media session: {exc}") from None

    @staticmethod
    def art_id(s: dict[str, Any]) -> str | None:
        if not s.get("thumbnail"):
            return None
        h = hashlib.sha1()
        for k in ("app", "title", "artist", "album"):
            h.update(str(s.get(k, "")).encode("utf-8", "replace") + b"\0")
        h.update(hashlib.sha1(s["thumbnail"]).digest())
        return h.hexdigest()[:12]

    def now(self) -> dict[str, Any]:
        if not self.available:
            return {"ok": True, "available": False, "reason": self.reason, "playing": None}
        s = self._session()
        vol, muted = (None, None)
        try:
            vol, muted = self.platform.volume()  # type: ignore[union-attr]
        except Exception:
            pass
        out: dict[str, Any] = {"ok": True, "available": True, "playing": None,
                               "volume": None if vol is None else round(vol, 3), "muted": muted}
        if s is None:
            return out
        state = STATUS.get(int(s.get("status") or 0), "closed")
        pos, dur = s.get("position"), s.get("duration")
        if pos is not None and state == "playing" and s.get("updated"):
            pos += max(0.0, time.time() - float(s["updated"]))  # the session reports where it was when it last said
        if pos is not None and dur:
            pos = min(pos, dur)
        out["playing"] = {
            "title": s.get("title") or "", "artist": s.get("artist") or "", "album": s.get("album") or "",
            "app": app_name(s.get("app") or ""), "app_id": s.get("app") or "",
            "state": state,
            "position": None if pos is None else round(pos, 1),
            "duration": None if not dur else round(dur, 1),
            "can": s.get("can") or {},
            "art": self.art_id(s),
        }
        return out

    def art(self, q: dict[str, str]) -> tuple[bytes, str, dict[str, Any]]:
        """(body, content type, meta). JPEG by default; format=rgb565 gives size×size pixels."""
        if not self.available:
            raise LinkError(404, self.reason)
        fmt = (q.get("format") or "jpeg").lower()
        if fmt not in ("jpeg", "rgb565"):
            raise LinkError(400, "format is jpeg or rgb565")
        try:
            size = int(q.get("size") or ART_DEFAULT)
        except ValueError:
            raise LinkError(400, "size must be a number") from None
        if not 16 <= size <= ART_MAX:
            raise LinkError(400, f"size must be 16..{ART_MAX}")
        s = self._session()
        aid = self.art_id(s) if s else None
        if not s or not aid:
            raise LinkError(404, "no album art")
        if self._art_cache and self._art_cache[:3] == (aid, size, fmt):
            return self._art_cache[3], "image/jpeg" if fmt == "jpeg" else "application/octet-stream", \
                {"id": aid, "size": size, "format": fmt}
        try:
            data = _to_rgb565(s["thumbnail"], size) if fmt == "rgb565" else _to_jpeg(s["thumbnail"], size)
        except ImportError:
            raise LinkError(404, "album art for the tablet needs Pillow on the PC: pip install pillow") from None
        except Exception as exc:
            raise LinkError(404, f"the album art couldn't be read: {exc}") from None
        self._art_cache = (aid, size, fmt, data)
        return data, "image/jpeg" if fmt == "jpeg" else "application/octet-stream", \
            {"id": aid, "size": size, "format": fmt}

    def art_json(self, q: dict[str, str]) -> dict[str, Any]:
        data, _, meta = self.art(q)
        return {"ok": True, **meta, "bytes": len(data), "data": base64.b64encode(data).decode("ascii")}

    def control(self, body: dict[str, Any]) -> dict[str, Any]:
        if not self.available:
            raise LinkError(503, self.reason)
        action = body.get("action")
        if action not in ACTIONS:
            raise LinkError(400, f"action is one of {', '.join(ACTIONS)}")
        p = self.platform
        assert p is not None
        try:
            with self._lock:
                if action == "volume":
                    level = body.get("level")
                    if not isinstance(level, (int, float)) or isinstance(level, bool) or not 0 <= level <= 1:
                        raise LinkError(400, "level is a number 0..1")
                    done = p.set_volume(float(level))
                    if not done:
                        raise LinkError(501, "setting a volume level needs pycaw on the PC (pip install pycaw); "
                                             "volume_up, volume_down and mute work without it")
                elif action in ("volume_up", "volume_down", "mute"):
                    done = p.volume_key(action.split("_")[-1] if action != "mute" else "mute")
                else:
                    done = p.control(action)
        except LinkError:
            raise
        except Exception as exc:
            raise LinkError(502, f"the PC didn't take it: {exc}") from None
        return {"ok": True, "done": bool(done)}
