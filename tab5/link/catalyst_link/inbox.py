"""Work orders: Markdown files with YAML front matter in ~/.catalyst-link/inbox/, which the PC's own
agent works through (link/AGENT.md). Both the server and the CLI edit them, under a lock file.

The front matter is written one `key: <JSON value>` per line — JSON scalars and flow lists are valid
YAML — and read back tolerantly, so a hand edit like `status: done` still parses."""
from __future__ import annotations

import hashlib
import json
import re
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any

from .state import FileLock, LinkError, State, iso, now, slugify, stamp, when, write_atomic

KINDS = ("bug", "task", "tune", "question")
PRIORITIES = ("low", "normal", "high")
STATUSES = ("open", "claimed", "done", "rejected")
# Over HTTP the tablet sets claimed/done/rejected; `release` (claimed → open) is the CLI's.
TRANSITIONS: dict[str, frozenset[str]] = {
    "open": frozenset({"claimed", "done", "rejected"}),
    "claimed": frozenset({"done", "rejected", "open"}),
    "done": frozenset(),
    "rejected": frozenset(),
}
FRONT_KEYS = ("id", "title", "kind", "priority", "status", "created", "updated", "from", "patch", "branch", "files")
MAX_TITLE = 120
MAX_BODY = 256 * 1024
MAX_NOTE = 8 * 1024
DUPLICATE_WINDOW = timedelta(minutes=10)
ID_RE = re.compile(r"^wo-[a-z0-9-]{1,80}$")

BODY_OPEN, BODY_CLOSE = "<!-- link:body -->", "<!-- /link:body -->"
ROBOT_OPEN, ROBOT_CLOSE = "<!-- link:robot -->", "<!-- /link:robot -->"
NOTES_OPEN = "<!-- link:notes -->"


# --- front matter ----------------------------------------------------------------------------------

def dump_front(meta: dict[str, Any]) -> str:
    keys = [k for k in FRONT_KEYS if k in meta] + [k for k in meta if k not in FRONT_KEYS]
    return "---\n" + "".join(f"{k}: {json.dumps(meta[k], ensure_ascii=False)}\n" for k in keys) + "---\n"


def parse_front(text: str) -> tuple[dict[str, Any], str]:
    """(front matter, the rest). Values are JSON if they parse, else the bare string."""
    if not text.startswith("---\n"):
        return {}, text
    end = text.find("\n---\n", 3)
    if end < 0:
        return {}, text
    meta: dict[str, Any] = {}
    for line in text[4:end].splitlines():
        key, sep, value = line.partition(":")
        if not sep or not key.strip() or line.startswith((" ", "#")):
            continue
        value = value.strip()
        try:
            meta[key.strip()] = json.loads(value) if value else None
        except ValueError:
            meta[key.strip()] = value.strip("'")
    return meta, text[end + 5:]


def _between(text: str, start: str, stop: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    i += len(start)
    j = text.find(stop, i)
    return text[i: j if j >= 0 else len(text)].strip("\n")


# --- the inbox ----------------------------------------------------------------------------------------

class Inbox:
    def __init__(self, state: State, patches_dir: Path | None = None) -> None:
        self.state = state
        self.dir = state.inbox_dir
        self.patches_dir = patches_dir or state.patches_dir

    def _path(self, wid: str) -> Path:
        return self.dir / f"{wid}.md"

    def _lock(self, wid: str) -> FileLock:
        return FileLock(self.dir / f".{wid}.lock")

    def resolve_id(self, ref: str) -> str:
        """An exact id, a file path, or a unique prefix of an id."""
        ref = Path(ref).stem if ref.endswith(".md") else ref
        if ID_RE.match(ref) and self._path(ref).exists():
            return ref
        hits = [p.stem for p in self.dir.glob("wo-*.md") if p.stem.startswith(ref)]
        if len(hits) == 1:
            return hits[0]
        if not hits:
            raise LinkError(404, f"no work order {ref}")
        raise LinkError(409, f"{ref} matches {len(hits)} work orders")

    # --- create ------------------------------------------------------------------------------------

    @staticmethod
    def _choice(req: dict[str, Any], key: str, allowed: tuple[str, ...], default: str) -> str:
        value = req.get(key) or default
        if value not in allowed:
            raise LinkError(400, f"{key} must be one of {', '.join(allowed)}")
        return value

    def create(self, req: dict[str, Any]) -> tuple[dict[str, Any], bool]:
        """Returns (response, created). created is False for a re-sent duplicate."""
        title = req.get("title")
        if not isinstance(title, str) or not title.strip():
            raise LinkError(400, "title is required")
        title = " ".join(title.split())[:MAX_TITLE]
        body = req.get("body") or ""
        if not isinstance(body, str):
            raise LinkError(400, "body must be a string")
        if len(body.encode("utf-8")) > MAX_BODY:
            raise LinkError(413, f"body is larger than {MAX_BODY // 1024} KB")
        kind = self._choice(req, "kind", KINDS, "task")
        priority = self._choice(req, "priority", PRIORITIES, "normal")
        robot = req.get("robot") or {}
        if not isinstance(robot, dict):
            raise LinkError(400, "robot must be an object")
        patch = req.get("patch") or None
        if patch is not None and (not isinstance(patch, str) or not re.fullmatch(r"p-[a-z0-9-]{1,80}", patch)):
            raise LinkError(400, "patch must be a patch id (p-…)")
        files = req.get("files") or []
        if not isinstance(files, list) or not all(isinstance(f, str) for f in files) or len(files) > 50:
            raise LinkError(400, "files must be a list of file names (at most 50)")
        source = req.get("from") or "catalyst-tab"
        if not isinstance(source, str):
            raise LinkError(400, "from must be a string")

        sha = hashlib.sha256(json.dumps([title, body, kind, patch, files]).encode("utf-8")).hexdigest()
        dup = self._recent_duplicate(sha)
        if dup:
            return {"ok": True, "id": dup, "path": f"inbox/{dup}.md", "duplicate": True}, False

        branch = None
        if patch:
            rec_path = self.patches_dir / f"{patch}.json"
            if rec_path.exists():
                try:
                    branch = json.loads(rec_path.read_text(encoding="utf-8")).get("branch")
                except ValueError:
                    pass

        t = now()
        meta = {"id": "", "title": title, "kind": kind, "priority": priority, "status": "open",
                "created": iso(t), "updated": iso(t), "from": source[:64], "patch": patch, "branch": branch,
                "files": [Path(f.replace("\\", "/")).name for f in files], "request_sha": sha}
        base = f"wo-{stamp(t)}-{slugify(title)}"
        for n in range(1, 100):
            wid = base if n == 1 else f"{base}-{n}"
            meta["id"] = wid
            try:
                with open(self._path(wid), "x", encoding="utf-8", newline="\n") as f:  # "x": ids never collide
                    f.write(self._render(meta, title, body, robot, ""))
                return {"ok": True, "id": wid, "path": f"inbox/{wid}.md"}, True
            except FileExistsError:
                continue
        raise LinkError(409, "could not pick a free work-order id")

    def _render(self, meta: dict[str, Any], title: str, body: str, robot: dict[str, Any], notes: str) -> str:
        safe_body = body.replace(BODY_CLOSE, "<!-- /link:body - -->").strip("\n")
        files_dir = self.state.files_dir
        lines = [dump_front(meta), f"# {title}", "", BODY_OPEN, safe_body, BODY_CLOSE, "",
                 "## Robot snapshot", "", ROBOT_OPEN, "```json",
                 json.dumps(robot, indent=2, ensure_ascii=False, sort_keys=True), "```", ROBOT_CLOSE, ""]
        if meta.get("patch"):
            lines += ["## Proposed patch", "",
                      f"- id: `{meta['patch']}`",
                      f"- branch: `{meta.get('branch') or '(unknown)'}`",
                      f"- diff: `{self.patches_dir / (meta['patch'] + '.diff')}`", ""]
        if meta.get("files"):
            lines += ["## Attached files", ""] + [f"- `{files_dir / name}`" for name in meta["files"]] + [""]
        lines += ["## Notes", "", NOTES_OPEN]
        if notes:
            lines.append(notes.strip("\n"))
        return "\n".join(lines) + "\n"

    def _recent_duplicate(self, sha: str) -> str | None:
        """The tablet's outbox may re-send a work order whose answer it never got."""
        cutoff = now() - DUPLICATE_WINDOW
        for meta in self._metas():
            if meta.get("request_sha") == sha:
                try:
                    if datetime.fromisoformat(str(meta.get("created"))) > cutoff:
                        return str(meta["id"])
                except ValueError:
                    continue
        return None

    # --- read --------------------------------------------------------------------------------------

    def _metas(self) -> list[dict[str, Any]]:
        metas = []
        for path in self.dir.glob("wo-*.md"):
            try:
                meta, _ = parse_front(path.read_text(encoding="utf-8"))
            except OSError:
                continue
            meta.setdefault("id", path.stem)
            metas.append(meta)
        metas.sort(key=lambda m: str(m.get("created", "")), reverse=True)
        return metas

    @staticmethod
    def _when(meta: dict[str, Any]) -> str:
        try:
            return when(datetime.fromisoformat(str(meta.get("created"))))
        except ValueError:
            return ""

    def list(self, status: str | None = None) -> list[dict[str, Any]]:
        wanted = {s.strip() for s in status.split(",")} if status and status != "all" else None
        return [{"id": m["id"], "title": m.get("title", ""), "kind": m.get("kind", ""),
                 "priority": m.get("priority", ""), "status": m.get("status", ""), "when": self._when(m)}
                for m in self._metas() if wanted is None or m.get("status") in wanted]

    def counts(self) -> dict[str, int]:
        out = {s: 0 for s in STATUSES}
        for m in self._metas():
            s = m.get("status")
            if s in out:
                out[s] += 1
        return out

    def get(self, ref: str) -> dict[str, Any]:
        wid = self.resolve_id(ref)
        path = self._path(wid)
        text = path.read_text(encoding="utf-8")
        meta, rest = parse_front(text)
        robot_text = _between(rest, ROBOT_OPEN, ROBOT_CLOSE).strip().removeprefix("```json").removesuffix("```")
        try:
            robot = json.loads(robot_text) if robot_text.strip() else {}
        except ValueError:
            robot = {}
        notes = rest.split(NOTES_OPEN, 1)[1].strip("\n") if NOTES_OPEN in rest else ""
        item = {k: v for k, v in meta.items() if k != "request_sha"}
        item.update({"when": self._when(meta), "body": _between(rest, BODY_OPEN, BODY_CLOSE),
                     "robot": robot, "notes": notes, "path": str(path)})
        return item

    # --- status ------------------------------------------------------------------------------------

    def set_status(self, ref: str, status: str, note: str = "", *, allowed: tuple[str, ...] = STATUSES) -> dict[str, Any]:
        if status not in allowed:
            raise LinkError(400, f"status must be one of {', '.join(allowed)}")
        if not isinstance(note, str):
            raise LinkError(400, "note must be a string")
        note = " ".join(note.split())[:MAX_NOTE] if "\n" not in note else note.strip()[:MAX_NOTE]
        wid = self.resolve_id(ref)
        path = self._path(wid)
        with self._lock(wid):
            text = path.read_text(encoding="utf-8")
            meta, rest = parse_front(text)
            current = str(meta.get("status", "open"))
            if status not in TRANSITIONS.get(current, frozenset()):
                raise LinkError(409, f"{wid} is {current}; it can't become {status}")
            t = now()
            meta["status"] = status
            meta["updated"] = iso(t)
            entry = f"- {when(t)} · {status}" + (f" · {note}" if note else "")
            if "\n" in entry:  # a multi-line note stays one list item
                first, *more = entry.split("\n")
                entry = "\n".join([first] + ["  " + line for line in more])
            if NOTES_OPEN not in rest:
                rest = rest.rstrip("\n") + f"\n\n## Notes\n\n{NOTES_OPEN}\n"
            rest = rest.rstrip("\n") + "\n" + entry + "\n"
            write_atomic(path, dump_front(meta) + rest)
        return {"ok": True, "id": wid, "status": status}
