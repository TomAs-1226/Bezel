"""The robot repo: git helpers, and patches — the only way the tablet changes code.

A patch never touches the checked-out branch or the main working tree. It becomes a new branch
`tab/<stamp>-<slug>` created from HEAD in its own worktree under ~/.catalyst-link/worktrees/, committed
as Catalyst Tab, optionally compile-checked there with the PC's own command, and recorded in
patches/<id>.json + .diff. Nothing is pushed or deployed."""
from __future__ import annotations

import hashlib
import json
import os
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any

from . import pathsafe
from .state import LinkError, State, iso, now, slugify, stamp, when, write_atomic

MAX_EDITS = 20
MAX_PATCH_BYTES = 64 * 1024
MAX_TITLE = 120
MAX_SUMMARY = 8 * 1024
MAX_RESPONSE_DIFF = 64 * 1024
AUTHOR_NAME = "Catalyst Tab"
AUTHOR_EMAIL = "tab@catalyst.local"
DUPLICATE_WINDOW = timedelta(minutes=10)


class GitError(Exception):
    pass


def git(repo: Path, *args: str, check: bool = True, stdin: bytes | None = None,
        env: dict[str, str] | None = None, timeout: float = 120) -> subprocess.CompletedProcess[bytes]:
    """Run git in `repo` with no prompts and no pager; raise GitError on failure when `check`."""
    full_env = {**os.environ, "GIT_TERMINAL_PROMPT": "0", "GIT_PAGER": "cat", "LC_ALL": "C", **(env or {})}
    proc = subprocess.run(
        ["git", "-c", "core.quotepath=off", "-C", str(repo), *args],
        input=stdin, capture_output=True, env=full_env, timeout=timeout,
    )
    if check and proc.returncode != 0:
        raise GitError(f"git {' '.join(args)}: {proc.stderr.decode('utf-8', 'replace').strip()}")
    return proc


def git_text(repo: Path, *args: str) -> str:
    return git(repo, *args).stdout.decode("utf-8", "replace").strip()


def repo_root(path: Path) -> Path:
    """The top of the git work tree containing `path`."""
    try:
        top = git_text(path.expanduser(), "rev-parse", "--show-toplevel")
    except (GitError, FileNotFoundError, NotADirectoryError) as exc:
        raise SystemExit(f"catalyst-link: {path} is not a git repository ({exc})") from None
    return Path(top).resolve()


def repo_status(repo: Path) -> dict[str, Any]:
    branch = git(repo, "symbolic-ref", "--short", "-q", "HEAD", check=False).stdout.decode().strip()
    dirty = bool(git(repo, "status", "--porcelain", "--untracked-files=no", check=False).stdout.strip())
    return {"repo": repo.name, "branch": branch or "(detached)", "dirty": dirty}


def branch_exists(repo: Path, branch: str) -> bool:
    return git(repo, "show-ref", "--verify", "--quiet", f"refs/heads/{branch}", check=False).returncode == 0


def tab_branches(repo: Path) -> dict[str, str]:
    """{"tab/…": tip sha} for every patch branch that still exists."""
    out = git(repo, "for-each-ref", "--format=%(refname:short) %(objectname)", "refs/heads/tab/", check=False)
    pairs = (line.split(" ", 1) for line in out.stdout.decode("utf-8", "replace").splitlines() if " " in line)
    return {name: sha for name, sha in pairs}


# --- the compile check --------------------------------------------------------------------------------

def run_check(command: str, cwd: Path, timeout: float, log_path: Path) -> dict[str, Any]:
    """Run the PC's check command (from its CLI flags, never from a request) in a patch's worktree."""
    started = time.monotonic()
    timed_out = False
    with open(log_path, "wb") as log:
        kwargs: dict[str, Any] = {}
        if os.name == "posix":
            kwargs["start_new_session"] = True  # so a timeout kills the whole tree (gradle, javac…)
        else:
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP  # type: ignore[attr-defined]
        proc = subprocess.Popen(command, shell=True, cwd=cwd, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT, **kwargs)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            _kill_tree(proc)
            proc.wait()
    seconds = round(time.monotonic() - started, 1)
    text = log_path.read_bytes().decode("utf-8", "replace")
    tail = "\n".join(text.rstrip().splitlines()[-20:])[-2000:]
    if timed_out:
        tail = (tail + f"\n[catalyst-link] check timed out after {timeout:g} s").strip()
    return {"ran": True, "ok": proc.returncode == 0 and not timed_out, "seconds": seconds,
            "exit": proc.returncode, "timed_out": timed_out, "tail": tail}


def _kill_tree(proc: subprocess.Popen[bytes]) -> None:
    try:
        if os.name == "posix":
            os.killpg(proc.pid, signal.SIGKILL)
        else:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
    except (ProcessLookupError, PermissionError, OSError):
        proc.kill()


# --- patches ----------------------------------------------------------------------------------------

@dataclass(frozen=True)
class Edit:
    path: str
    old: str
    new: str


def _edit_error(index: int, path: str, why: str, message: str) -> LinkError:
    return LinkError(409, f"edit {index + 1} ({path}): {message}", edit=index, why=why)


class Patches:
    def __init__(self, repo: Path, state: State, check: str | None = None, check_timeout: float = 300) -> None:
        self.repo = repo
        self.state = state
        self.check = check
        self.check_timeout = check_timeout
        self._lock = threading.Lock()
        self._merged: dict[tuple[str, str, str], bool] = {}

    # --- validation ------------------------------------------------------------------------------

    @staticmethod
    def _parse(req: dict[str, Any]) -> tuple[str, str, list[Edit], dict[str, Any], str]:
        title = req.get("title")
        if not isinstance(title, str) or not title.strip():
            raise LinkError(400, "title is required")
        title = " ".join(title.split())[:MAX_TITLE]
        summary = req.get("summary") or ""
        if not isinstance(summary, str):
            raise LinkError(400, "summary must be a string")
        summary = summary[:MAX_SUMMARY]
        robot = req.get("robot") or {}
        if not isinstance(robot, dict):
            raise LinkError(400, "robot must be an object")
        source = req.get("from") or "catalyst-tab"
        if not isinstance(source, str):
            raise LinkError(400, "from must be a string")
        raw_edits = req.get("edits")
        if not isinstance(raw_edits, list) or not raw_edits:
            raise LinkError(400, "edits must be a non-empty list")
        if len(raw_edits) > MAX_EDITS:
            raise LinkError(413, f"{len(raw_edits)} edits; at most {MAX_EDITS} per patch")
        edits: list[Edit] = []
        size = 0
        for i, e in enumerate(raw_edits):
            if not isinstance(e, dict) or not all(isinstance(e.get(k), str) for k in ("path", "old", "new")):
                raise LinkError(400, f"edit {i + 1}: needs string path, old and new", edit=i)
            size += len(e["old"].encode("utf-8")) + len(e["new"].encode("utf-8"))
            edits.append(Edit(e["path"], e["old"], e["new"]))
        if size > MAX_PATCH_BYTES:
            raise LinkError(413, f"patch is {size} bytes; at most {MAX_PATCH_BYTES}")
        return title, summary, edits, robot, source[:64]

    def _head_file(self, rel: str) -> str | None:
        """The file's text at HEAD, or None if HEAD has no such path."""
        out = git(self.repo, "ls-tree", "-z", "HEAD", "--", rel).stdout.decode("utf-8", "replace")
        if not out:
            return None
        meta, _, _ = out.partition("\t")
        mode, kind, obj = meta.split()
        if kind != "blob":
            raise LinkError(409, f"{rel}: is a directory or submodule at HEAD")
        if mode == "120000":
            raise LinkError(403, f"{rel}: is a symlink at HEAD; symlinks are not edited")
        data = git(self.repo, "cat-file", "blob", obj).stdout
        try:
            return data.decode("utf-8")
        except UnicodeDecodeError:
            raise LinkError(409, f"{rel}: not a UTF-8 text file") from None

    def _apply(self, edits: list[Edit]) -> tuple[dict[str, str], dict[str, str | None]]:
        """Apply the edits in order to HEAD's files, in memory. Returns (new contents, originals)."""
        current: dict[str, str | None] = {}
        original: dict[str, str | None] = {}
        for i, e in enumerate(edits):
            try:
                rel, _ = pathsafe.resolve(self.repo, e.path)
            except LinkError as exc:
                raise LinkError(exc.status, f"edit {i + 1}: {exc.message}", edit=i, why="path") from None
            if not rel:
                raise _edit_error(i, e.path, "path", "path is the repo root")
            if rel not in current:
                current[rel] = original[rel] = self._head_file(rel)
            content = current[rel]
            if content is None:
                if e.old != "":
                    raise _edit_error(i, rel, "missing_file", "no such file at HEAD (use old:\"\" to create one)")
                current[rel] = e.new
                continue
            if e.old == "":
                raise _edit_error(i, rel, "exists", "old is empty but the file exists; files are never replaced whole")
            old, new = e.old, e.new
            count = content.count(old)
            if count == 0 and "\r\n" in content and "\n" in old and "\r\n" not in old:
                # The tablet may have normalised a CRLF file's line endings; match it the file's way.
                crlf_old = old.replace("\n", "\r\n")
                if content.count(crlf_old):
                    old, new = crlf_old, new.replace("\r\n", "\n").replace("\n", "\r\n")
                    count = content.count(old)
            if count == 0:
                hint = ""
                disk = self.repo / rel
                try:
                    if disk.is_file() and e.old in disk.read_text(encoding="utf-8", errors="replace"):
                        hint = " (it is in the working tree's uncommitted copy; patches start from HEAD)"
                except OSError:
                    pass
                raise _edit_error(i, rel, "not_found", f"old text not found{hint}")
            if count > 1:
                raise _edit_error(i, rel, "not_unique", f"old text occurs {count} times; it must occur exactly once")
            current[rel] = content.replace(old, new, 1)
        changed = {p: c for p, c in current.items() if c is not None and c != original[p]}
        if not changed:
            raise LinkError(409, "the edits change nothing", why="no_change")
        return changed, original

    # --- creating ------------------------------------------------------------------------------------

    @staticmethod
    def _request_sha(title: str, summary: str, edits: list[Edit]) -> str:
        canon = json.dumps([title, summary, [[e.path, e.old, e.new] for e in edits]], ensure_ascii=False)
        return hashlib.sha256(canon.encode("utf-8")).hexdigest()

    def _recent_duplicate(self, sha: str) -> dict[str, Any] | None:
        """The tablet's outbox may re-send a patch whose answer it never got; answer with the first one."""
        cutoff = now() - DUPLICATE_WINDOW
        for rec in self.records():
            if rec.get("request_sha") == sha and datetime.fromisoformat(rec["created"]) > cutoff:
                if self.status_of(rec) == "proposed":
                    return rec
        return None

    def _unique_id(self, title: str) -> tuple[str, str]:
        base = f"{stamp()}-{slugify(title)}"
        for n in range(1, 100):
            core = base if n == 1 else f"{base}-{n}"
            pid, branch = f"p-{core}", f"tab/{core}"
            if not ((self.state.patches_dir / f"{pid}.json").exists()
                    or (self.state.worktrees_dir / pid).exists() or branch_exists(self.repo, branch)):
                return pid, branch
        raise LinkError(409, "could not pick a free patch id")

    def _message(self, pid: str, title: str, summary: str, robot: dict[str, Any], source: str) -> str:
        snapshot = json.dumps(robot, indent=2, ensure_ascii=False, sort_keys=True)[:4000] if robot else "(none)"
        parts = [title, "", summary.strip() or "(no summary)", "", "Robot snapshot:", snapshot, "",
                 f"Proposed from {source} through Catalyst Link. Not reviewed, not pushed, not deployed.",
                 f"Link-Patch: {pid}"]
        return "\n".join(parts) + "\n"

    def create(self, req: dict[str, Any]) -> dict[str, Any]:
        title, summary, edits, robot, source = self._parse(req)
        sha = self._request_sha(title, summary, edits)
        dup = self._recent_duplicate(sha)
        if dup:
            return {**self.response(dup), "duplicate": True}

        with self._lock:
            if git(self.repo, "rev-parse", "--verify", "-q", "HEAD^{commit}", check=False).returncode != 0:
                raise LinkError(409, "the repo has no commits yet")
            head = git_text(self.repo, "rev-parse", "HEAD")
            changed, original = self._apply(edits)
            pid, branch = self._unique_id(title)
            wt = self.state.worktrees_dir / pid
            git(self.repo, "worktree", "add", "-q", "-b", branch, str(wt), head)
            try:
                wt_root = wt.resolve()
                for rel, text in changed.items():
                    target = wt_root / rel
                    # HEAD's tree (checked out here) may differ from the main working tree that
                    # pathsafe looked at: check again for symlinks out, before and after mkdir.
                    if not pathsafe.inside(wt_root, target.parent) or target.is_symlink():
                        raise LinkError(403, f"{rel}: resolves outside the patch worktree")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if not pathsafe.inside(wt_root, target.parent):
                        raise LinkError(403, f"{rel}: resolves outside the patch worktree")
                    target.write_bytes(text.encode("utf-8"))
                git(wt, "add", "--", *changed)
                ident = {"GIT_AUTHOR_NAME": AUTHOR_NAME, "GIT_AUTHOR_EMAIL": AUTHOR_EMAIL,
                         "GIT_COMMITTER_NAME": AUTHOR_NAME, "GIT_COMMITTER_EMAIL": AUTHOR_EMAIL}
                # --no-verify: the team's hooks run when a human merges; the proposal itself must land.
                git(wt, "-c", "commit.gpgsign=false", "commit", "-q", "--no-verify", "-F", "-",
                    stdin=self._message(pid, title, summary, robot, source).encode("utf-8"), env=ident)
                commit = git_text(wt, "rev-parse", "HEAD")
                diff = git(wt, "diff", "--no-color", "--no-ext-diff", head, commit).stdout.decode("utf-8", "replace")
                diffstat = git_text(wt, "diff", "--shortstat", head, commit)
            except BaseException:
                git(self.repo, "worktree", "remove", "--force", str(wt), check=False)
                git(self.repo, "branch", "-D", branch, check=False)
                raise
            write_atomic(self.state.patches_dir / f"{pid}.diff", diff)

        check: dict[str, Any] = {"ran": False}
        if self.check:
            check = run_check(self.check, wt, self.check_timeout, self.state.patches_dir / f"{pid}.check.log")

        rec = {
            "id": pid, "title": title, "summary": summary, "branch": branch, "worktree": str(wt),
            "repo": str(self.repo), "base": head, "commit": commit, "files": sorted(changed),
            "created_files": sorted(p for p in changed if original[p] is None),
            "diffstat": diffstat, "check": check, "robot": robot, "from": source,
            "created": iso(), "when": when(), "request_sha": sha,
        }
        write_atomic(self.state.patches_dir / f"{pid}.json", json.dumps(rec, indent=2, ensure_ascii=False))
        return self.response(rec)

    def response(self, rec: dict[str, Any]) -> dict[str, Any]:
        diff_path = self.state.patches_dir / f"{rec['id']}.diff"
        diff = diff_path.read_text(encoding="utf-8") if diff_path.exists() else ""
        out = {"ok": True, "id": rec["id"], "branch": rec["branch"], "files": rec["files"],
               "diffstat": rec["diffstat"], "diff": diff[:MAX_RESPONSE_DIFF], "check": rec["check"]}
        if len(diff) > MAX_RESPONSE_DIFF:
            out["diff_truncated"] = True
        return out

    # --- listing ---------------------------------------------------------------------------------------

    def records(self) -> list[dict[str, Any]]:
        recs = []
        for path in self.state.patches_dir.glob("p-*.json"):
            try:
                recs.append(json.loads(path.read_text(encoding="utf-8")))
            except (OSError, ValueError):
                continue
        recs.sort(key=lambda r: r.get("created", ""), reverse=True)
        return recs

    def status_of(self, rec: dict[str, Any], heads: dict[str, str] | None = None) -> str:
        """proposed, merged (the branch is an ancestor of HEAD) or dropped (the branch is gone)."""
        repo = Path(rec.get("repo") or self.repo)
        heads = heads if heads is not None else tab_branches(repo)
        branch = rec["branch"]
        tip = heads.get(branch)
        if tip is None:
            return "dropped"
        head = git(repo, "rev-parse", "-q", "--verify", "HEAD", check=False).stdout.decode().strip()
        key = (str(repo), head, tip)
        if key not in self._merged:  # /link/status is polled; ask git once per (HEAD, tip)
            self._merged[key] = git(repo, "merge-base", "--is-ancestor", tip, "HEAD", check=False).returncode == 0
        return "merged" if self._merged[key] else "proposed"

    @staticmethod
    def check_word(check: dict[str, Any]) -> str:
        if not check.get("ran"):
            return "none"
        if check.get("timed_out"):
            return "timeout"
        return "passed" if check.get("ok") else "failed"

    def list(self) -> list[dict[str, Any]]:
        heads = tab_branches(self.repo)
        return [{"id": r["id"], "title": r["title"], "branch": r["branch"],
                 "status": self.status_of(r, heads if Path(r.get("repo") or self.repo) == self.repo else None),
                 "when": r.get("when", ""), "check": self.check_word(r.get("check", {}))}
                for r in self.records()]
