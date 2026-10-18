"""Uploaded files (run recordings, H.264 clips, logs) in ~/.catalyst-link/files/."""
from __future__ import annotations

import hashlib
import os
import re
import secrets
import threading
from datetime import datetime
from pathlib import Path
from typing import Any, Protocol

from .state import LinkError, when

MAX_FILE = 64 * 1024 * 1024
MAX_NAME = 120
CHUNK = 1024 * 1024


class Readable(Protocol):
    def read(self, n: int, /) -> bytes: ...


def sanitize(name: str | None) -> str:
    """Only [A-Za-z0-9._-], no directories, no leading dots (so no '..' and no hidden files)."""
    base = (name or "").replace("\\", "/").rsplit("/", 1)[-1]
    base = re.sub(r"[^A-Za-z0-9._-]", "_", base).lstrip(".")
    base = re.sub(r"_+", "_", base)
    if len(base) > MAX_NAME:
        stem, ext = os.path.splitext(base)
        base = stem[: MAX_NAME - len(ext)] + ext
    return base or "file"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


class Files:
    def __init__(self, directory: Path) -> None:
        self.dir = directory
        self._lock = threading.Lock()

    def save(self, name: str | None, stream: Readable, length: int) -> dict[str, Any]:
        if length > MAX_FILE:
            raise LinkError(413, f"larger than {MAX_FILE // (1024 * 1024)} MB")
        clean = sanitize(name)
        tmp = self.dir / f".upload-{secrets.token_hex(6)}"
        h = hashlib.sha256()
        got = 0
        try:
            with open(tmp, "wb") as f:
                while got < length:
                    chunk = stream.read(min(CHUNK, length - got))
                    if not chunk:
                        break
                    f.write(chunk)
                    h.update(chunk)
                    got += len(chunk)
            if got != length:
                raise LinkError(400, f"upload ended after {got} of {length} bytes")
            digest = h.hexdigest()
            with self._lock:
                stem, ext = os.path.splitext(clean)
                for n in range(1, 1000):
                    candidate = self.dir / (clean if n == 1 else f"{stem}-{n}{ext}")
                    if not candidate.exists():
                        os.replace(tmp, candidate)
                        return self._answer(candidate, got, digest)
                    # The same bytes under the same name: a re-sent upload from the tablet's outbox.
                    if candidate.stat().st_size == got and _sha256(candidate) == digest:
                        return {**self._answer(candidate, got, digest), "duplicate": True}
            raise LinkError(409, f"too many files named {clean}")
        finally:
            tmp.unlink(missing_ok=True)

    @staticmethod
    def _answer(path: Path, size: int, digest: str) -> dict[str, Any]:
        return {"ok": True, "name": path.name, "path": f"files/{path.name}", "bytes": size, "sha256": digest}

    def list(self) -> list[dict[str, Any]]:
        found = []
        for path in self.dir.iterdir():
            if path.name.startswith(".") or not path.is_file():
                continue
            st = path.stat()
            found.append((st.st_mtime, {"name": path.name, "bytes": st.st_size,
                                        "when": when(datetime.fromtimestamp(st.st_mtime).astimezone())}))
        found.sort(key=lambda f: f[0], reverse=True)
        return [entry for _, entry in found]

    def count(self) -> int:
        return sum(1 for p in self.dir.iterdir() if p.is_file() and not p.name.startswith("."))
