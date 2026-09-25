"""The Link's own state directory (~/.catalyst-link/, or $CATALYST_LINK_HOME), its token and audit log."""
from __future__ import annotations

import json
import os
import re
import secrets
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Any

# No l/i/o/0/1: the technician types this on a touch keyboard, once.
_TOKEN_ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"


class LinkError(Exception):
    """An error with an HTTP status, reported to the tablet as {"ok":false,"error":message,...extra}."""

    def __init__(self, status: int, message: str, **extra: Any) -> None:
        super().__init__(message)
        self.status = status
        self.message = message
        self.extra = extra

    def payload(self) -> dict[str, Any]:
        return {"ok": False, "error": self.message, **self.extra}


def new_token() -> str:
    """Four groups of four, about 79 bits: "k7mq-3xtr-9pwd-h2fc"."""
    return "-".join("".join(secrets.choice(_TOKEN_ALPHABET) for _ in range(4)) for _ in range(4))


def link_home() -> Path:
    env = os.environ.get("CATALYST_LINK_HOME")
    return Path(env).expanduser() if env else Path.home() / ".catalyst-link"


def now() -> datetime:
    return datetime.now().astimezone()


def stamp(t: datetime | None = None) -> str:
    """The time part of ids: 20260923-141205."""
    return (t or now()).strftime("%Y%m%d-%H%M%S")


def when(t: datetime | None = None) -> str:
    """What the tablet shows in lists: "2026-09-23 14:12"."""
    return (t or now()).strftime("%Y-%m-%d %H:%M")


def iso(t: datetime | None = None) -> str:
    return (t or now()).isoformat(timespec="seconds")


def slugify(text: str, limit: int = 40) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    slug = slug[:limit].rstrip("-")
    return slug or "untitled"


def write_atomic(path: Path, data: bytes | str) -> None:
    raw = data.encode("utf-8") if isinstance(data, str) else data
    tmp = path.with_name(f".{path.name}.{secrets.token_hex(4)}.tmp")
    with open(tmp, "wb") as f:
        f.write(raw)
    os.replace(tmp, path)


class FileLock:
    """A cross-process lock (the server and the CLI both edit the inbox): an O_EXCL lock file."""

    def __init__(self, path: Path, timeout: float = 5.0, stale: float = 30.0) -> None:
        self.path = path
        self.timeout = timeout
        self.stale = stale

    def __enter__(self) -> "FileLock":
        deadline = time.monotonic() + self.timeout
        while True:
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                os.close(fd)
                return self
            except FileExistsError:
                try:
                    if time.time() - self.path.stat().st_mtime > self.stale:
                        self.path.unlink(missing_ok=True)  # a crashed holder
                        continue
                except FileNotFoundError:
                    continue
                if time.monotonic() > deadline:
                    raise LinkError(503, f"busy: {self.path.name} is locked")
                time.sleep(0.02)

    def __exit__(self, *exc: object) -> None:
        self.path.unlink(missing_ok=True)


class State:
    def __init__(self, home: Path | None = None) -> None:
        self.home = (home or link_home()).expanduser().resolve()
        self.inbox_dir = self.home / "inbox"
        self.patches_dir = self.home / "patches"
        self.files_dir = self.home / "files"
        self.worktrees_dir = self.home / "worktrees"
        self.token_path = self.home / "token"
        self.log_path = self.home / "log.jsonl"
        self.hooks_log_path = self.home / "hooks.log"
        self.devices_path = self.home / "devices.json"   # the paired tablets (devices.py)
        self._log_lock = threading.Lock()
        self._token_cache: tuple[float, str] | None = None

    def ensure(self) -> "State":
        self.home.mkdir(parents=True, exist_ok=True)
        try:
            os.chmod(self.home, 0o700)
        except OSError:
            pass
        for d in (self.inbox_dir, self.patches_dir, self.files_dir, self.worktrees_dir):
            d.mkdir(exist_ok=True)
        return self

    # --- token -------------------------------------------------------------------------------------

    def token(self) -> str:
        """The pairing token, created on first use. Re-read when the file changes (`token --rotate`)."""
        try:
            mtime = self.token_path.stat().st_mtime
        except FileNotFoundError:
            return self.rotate_token()
        if self._token_cache and self._token_cache[0] == mtime:
            return self._token_cache[1]
        value = self.token_path.read_text(encoding="utf-8").strip()
        if not value:
            return self.rotate_token()
        self._token_cache = (mtime, value)
        return value

    def rotate_token(self) -> str:
        """A new main token. Every paired tablet is forgotten with it: rotating means "pair again"."""
        self.home.mkdir(parents=True, exist_ok=True)
        value = new_token()
        write_atomic(self.token_path, value + "\n")
        self.devices_path.unlink(missing_ok=True)
        try:
            os.chmod(self.token_path, 0o600)
        except OSError:
            pass
        self._token_cache = None
        return value

    # --- audit log ---------------------------------------------------------------------------------

    def log(self, record: dict[str, Any]) -> None:
        line = json.dumps({"t": iso(), **record}, ensure_ascii=False, default=str)
        with self._log_lock, open(self.log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
