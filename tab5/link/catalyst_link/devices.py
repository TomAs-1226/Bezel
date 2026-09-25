"""The paired tablets: each pairing gets its own token, so one tablet can be forgotten without the others.

A tablet that pairs by code (pairing.py) is handed a token of its own, not the Link's main token. Only
its SHA-256 is kept, in ~/.catalyst-link/devices.json, with the name the tablet gave, the address it
paired from, and when it was last seen. Forgetting a tablet deletes its entry, and its token stops
working on the next request.

The main token (~/.catalyst-link/token) still opens everything: it is what a technician types by hand,
what Claude Code's hook reads, and what the desktop app uses. Rotating it (`catalyst-link token
--rotate`) forgets every paired tablet too, so "rotate" keeps meaning "every tablet pairs again".
"""
from __future__ import annotations

import hashlib
import hmac
import json
import secrets
import threading
import time
from pathlib import Path
from typing import Any, Callable

from .state import iso, new_token, write_atomic

TOUCH_SAVE_S = 60.0   # last-seen is written to disk at most this often per tablet (it's kept in memory)
MAX_DEVICES = 64


def _digest(token: str) -> str:
    return hashlib.sha256(token.strip().encode("utf-8")).hexdigest()


class Devices:
    def __init__(self, path: Path, clock: Callable[[], float] = time.time) -> None:
        self.path = path
        self._clock = clock
        self._lock = threading.Lock()
        self._items: list[dict[str, Any]] = []
        self._mtime: float | None = None      # the file as last read or written by us
        self._saved: dict[str, float] = {}    # id -> when its last-seen was last written
        self._load()

    # --- the file ----------------------------------------------------------------------------------

    def _stat(self) -> float | None:
        try:
            return self.path.stat().st_mtime
        except FileNotFoundError:
            return None

    def _load(self) -> None:
        mtime = self._stat()
        items: list[dict[str, Any]] = []
        if mtime is not None:
            try:
                raw = json.loads(self.path.read_text(encoding="utf-8"))
                items = [d for d in raw.get("devices", []) if isinstance(d, dict) and d.get("id") and d.get("sha256")]
            except (OSError, ValueError, AttributeError):
                items = []
        self._items = items
        self._mtime = mtime

    def _refresh(self) -> None:
        """Pick up a change made by another process (the CLI rotating the token deletes the file)."""
        if self._stat() != self._mtime:
            self._load()

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        write_atomic(self.path, json.dumps({"devices": self._items}, indent=2, ensure_ascii=False) + "\n")
        self._mtime = self._stat()

    # --- what the server calls ---------------------------------------------------------------------

    def add(self, name: str, ip: str) -> str:
        """A new tablet paired: returns the token to hand it (the only time the token exists in full)."""
        token = new_token()
        t = self._clock()
        with self._lock:
            self._refresh()
            dev = {"id": secrets.token_hex(4), "name": (name or "a tablet")[:48], "sha256": _digest(token),
                   "paired_at": iso_from(t), "ip": ip, "last_seen": iso_from(t)}
            self._items.append(dev)
            self._items = self._items[-MAX_DEVICES:]
            self._saved[dev["id"]] = t
            self._save()
        return token

    def match(self, token: str | None) -> str | None:
        """The id of the tablet this token belongs to, or None."""
        if not token:
            return None
        d = _digest(token)
        with self._lock:
            self._refresh()
            found = None
            for dev in self._items:  # compare every one, in constant time each
                if hmac.compare_digest(dev["sha256"], d):
                    found = dev["id"]
            return found

    def touch(self, dev_id: str, ip: str) -> None:
        t = self._clock()
        with self._lock:
            for dev in self._items:
                if dev["id"] != dev_id:
                    continue
                moved = dev.get("ip") != ip
                dev["ip"] = ip
                dev["last_seen"] = iso_from(t)
                if moved or t - self._saved.get(dev_id, 0.0) >= TOUCH_SAVE_S:
                    self._saved[dev_id] = t
                    try:
                        self._save()
                    except OSError:
                        pass  # last-seen is a convenience; never fail a request over it
                return

    def forget(self, dev_id: str) -> bool:
        with self._lock:
            self._refresh()
            before = len(self._items)
            self._items = [d for d in self._items if d["id"] != dev_id]
            if len(self._items) == before:
                return False
            self._save()
            return True

    def list(self) -> list[dict[str, Any]]:
        """Newest first, without the token digests."""
        with self._lock:
            self._refresh()
            return [{k: v for k, v in d.items() if k != "sha256"} for d in reversed(self._items)]


def iso_from(t: float) -> str:
    from datetime import datetime

    return iso(datetime.fromtimestamp(t).astimezone())
