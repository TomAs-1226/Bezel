"""Read-only code access: /code/tree, /code/read, /code/search. Reads the working tree as it is on disk."""
from __future__ import annotations

import os
from collections import deque
from pathlib import Path
from typing import Any

from . import pathsafe
from .state import LinkError

MAX_TREE_ENTRIES = 400
MAX_TREE_DEPTH = 10
MAX_READ_LINES = 400
MAX_READ_BYTES = 24 * 1024
MAX_READ_FILE = 8 * 1024 * 1024
MAX_MATCHES = 200
MAX_QUERY = 200
MAX_SEARCH_FILE = 2 * 1024 * 1024
MAX_SEARCH_FILES = 20000
MAX_MATCH_TEXT = 300


def _int(value: str | None, default: int, lo: int, hi: int) -> int:
    if value in (None, ""):
        return default
    try:
        n = int(value)  # type: ignore[arg-type]
    except ValueError:
        raise LinkError(400, f"not a number: {value}") from None
    return max(lo, min(hi, n))


def _is_binary(head: bytes) -> bool:
    return b"\x00" in head


def _entry(root: Path, path: Path, rel: str) -> dict[str, Any] | None:
    """A tree entry for `path`, or None if it's a symlink out of bounds or unreadable."""
    try:
        if path.is_symlink() and not pathsafe.target_ok(root, path):
            return None
        st = path.stat()
    except OSError:
        return None
    if path.is_dir():
        return {"path": rel, "type": "dir"}
    return {"path": rel, "type": "file", "size": st.st_size}


def tree(root: Path, rel: str | None, depth: str | None) -> dict[str, Any]:
    rel, real = pathsafe.resolve(root, rel or "", must_exist=True)
    levels = _int(depth, 2, 1, MAX_TREE_DEPTH)
    start = root / rel if rel else root
    if not start.is_dir():
        entry = _entry(root, start, rel)
        return {"ok": True, "entries": [entry] if entry else [], "truncated": False}

    entries: list[dict[str, Any]] = []
    truncated = False
    # Breadth first, so a cut at the limit drops the deepest entries, not a whole sibling folder.
    queue: deque[tuple[Path, str, int]] = deque([(start, rel, 1)])
    while queue and not truncated:
        folder, folder_rel, level = queue.popleft()
        try:
            children = sorted(os.scandir(folder), key=lambda e: e.name)
        except OSError:
            continue
        for child in children:
            child_rel = f"{folder_rel}/{child.name}" if folder_rel else child.name
            if pathsafe.denied(tuple(child_rel.split("/"))):
                continue
            entry = _entry(root, Path(child.path), child_rel)
            if entry is None:
                continue
            if len(entries) >= MAX_TREE_ENTRIES:
                truncated = True
                break
            entries.append(entry)
            # Never descend through a symlink: it could loop, and its target is listed where it lives.
            if entry["type"] == "dir" and level < levels and not child.is_symlink():
                queue.append((Path(child.path), child_rel, level + 1))
    entries.sort(key=lambda e: e["path"])
    return {"ok": True, "entries": entries, "truncated": truncated}


def read(root: Path, rel: str | None, start: str | None, end: str | None) -> dict[str, Any]:
    if not rel:
        raise LinkError(400, "path is required")
    rel, real = pathsafe.resolve(root, rel, must_exist=True)
    if not real.is_file():
        raise LinkError(400, f"{rel}: not a file")
    if real.stat().st_size > MAX_READ_FILE:
        raise LinkError(413, f"{rel}: larger than {MAX_READ_FILE // (1024 * 1024)} MB")
    data = real.read_bytes()
    if _is_binary(data[:8192]):
        raise LinkError(415, f"{rel}: binary file")
    lines = data.decode("utf-8", errors="replace").splitlines(keepends=True)
    total = len(lines)
    first = _int(start, 1, 1, 1 << 30)
    last = _int(end, first + MAX_READ_LINES - 1, 1, 1 << 30)
    last = min(last, first + MAX_READ_LINES - 1, total)

    out: list[str] = []
    size = 0
    returned_end = min(first - 1, total)  # past the end: empty text, end < start
    for line_no in range(first, last + 1):
        line = lines[line_no - 1]
        n = len(line.encode("utf-8"))
        if size + n > MAX_READ_BYTES:
            if not out:  # one enormous line: return its head so the call still makes progress
                out.append(line.encode("utf-8")[:MAX_READ_BYTES].decode("utf-8", errors="ignore"))
                returned_end = line_no
            break
        out.append(line)
        size += n
        returned_end = line_no
    return {"ok": True, "path": rel, "start": first, "end": returned_end, "total": total, "text": "".join(out)}


def search(root: Path, q: str | None, rel: str | None, max_: str | None, case: str | None) -> dict[str, Any]:
    if not q:
        raise LinkError(400, "q is required")
    if len(q) > MAX_QUERY:
        raise LinkError(400, f"q is longer than {MAX_QUERY} characters")
    rel, real = pathsafe.resolve(root, rel or "", must_exist=True)
    limit = _int(max_, 40, 1, MAX_MATCHES)
    sensitive = case == "1"
    needle = q if sensitive else q.lower()

    matches: list[dict[str, Any]] = []
    scanned = 0

    def scan(path: Path, path_rel: str) -> bool:
        """Scan one file; True once the match limit is reached."""
        nonlocal scanned
        scanned += 1
        try:
            if path.is_symlink() and not pathsafe.target_ok(root, path):
                return False
            if path.stat().st_size > MAX_SEARCH_FILE:
                return False
            data = path.read_bytes()
        except OSError:
            return False
        if _is_binary(data[:8192]):
            return False
        text = data.decode("utf-8", errors="replace")
        if needle not in (text if sensitive else text.lower()):
            return False
        for i, line in enumerate(text.splitlines(), start=1):
            if needle in (line if sensitive else line.lower()):
                matches.append({"path": path_rel, "line": i, "text": line[:MAX_MATCH_TEXT]})
                if len(matches) >= limit:
                    return True
        return False

    base = root / rel if rel else root
    full = False
    if base.is_file():
        full = scan(base, rel)
    else:
        for folder, dirs, files in os.walk(base):
            folder_rel = Path(folder).relative_to(root).as_posix()
            folder_rel = "" if folder_rel == "." else folder_rel
            dirs[:] = sorted(
                d for d in dirs
                if not pathsafe.denied(tuple(f"{folder_rel}/{d}".strip("/").split("/")))
                and not os.path.islink(os.path.join(folder, d))
            )
            for name in sorted(files):
                file_rel = f"{folder_rel}/{name}" if folder_rel else name
                if pathsafe.denied(tuple(file_rel.split("/"))):
                    continue
                if scan(Path(folder) / name, file_rel) or scanned >= MAX_SEARCH_FILES:
                    full = True
                    break
            if full:
                break
    return {"ok": True, "matches": matches, "truncated": full}
