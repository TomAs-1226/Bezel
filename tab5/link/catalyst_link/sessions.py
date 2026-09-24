"""Claude Code's sessions on this PC, fed by its hooks (hook.py), for the tablet's companion.

Each session is a small state machine driven by hook events:

    UserPromptSubmit          -> running (a new turn: the clock starts)
    PreToolUse / PostToolUse  -> running; the step is what the tool is doing
    Notification              -> waiting_for_input (a permission prompt, or Claude idle and asking)
    Stop                      -> done (the turn's active time goes into the history)
    StopFailure, error        -> error
    SubagentStop, PreCompact  -> still running; the step says so
    SessionEnd                -> done if it was still going; the session is closed

`seq` counts state changes, so a reader can tell "finished again" from "still finished".

The finish estimate is deliberately plain. A turn's active time (time spent waiting on the owner left
out) is recorded when it stops. For a running turn that has been going for e seconds, the estimate is
the median of the recorded turns that ran longer than e, minus e: among the owner's own recent turns
that were still going at this point, when did half of them finish. The spread is their quartiles.
Turns from the same folder are used when there are at least five, else all recent turns. With fewer
than three to go on, or when this turn has outlasted every one of them, there is no estimate, and the
basis says why. The tablet always labels it an estimate.

Sessions live in memory (a restarted Link starts empty); the turn history is kept in
~/.catalyst-link/claude-turns.jsonl.
"""
from __future__ import annotations

import json
import statistics
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

from .hook import describe_tool
from .state import LinkError

HISTORY_KEEP = 500        # turns kept in the file (read back at start)
RECENT = 40               # turns an estimate looks at
SAME_FOLDER_MIN = 5       # this many from the same folder, or use all folders
MIN_SAMPLES = 3
MAX_SESSIONS = 50
LIST_MAX = 16
DONE_TTL_S = 24 * 3600    # finished sessions leave the list after a day
ENDED_TTL_S = 3600        # closed ones after an hour
MIN_TURN_S = 1.0          # a Stop this soon after the prompt (a slash command, a hook test) isn't a turn

RUNNING, WAITING, DONE, ERROR = "running", "waiting_for_input", "done", "error"


def _iso(t: float | None) -> str | None:
    return datetime.fromtimestamp(t).astimezone().isoformat(timespec="seconds") if t else None


def _project(cwd: str) -> str:
    p = cwd.replace("\\", "/").rstrip("/")
    return p.rsplit("/", 1)[-1] if p else ""


def _dur(s: float) -> str:
    s = int(round(s))
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s // 3600}h{(s % 3600) // 60:02d}m"


def _quantile(xs: list[float], q: float) -> float:
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    pos = q * (len(xs) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (pos - lo)


@dataclass
class Session:
    id: str
    cwd: str = ""
    title: str = ""
    state: str = DONE
    seq: int = 0
    step: str = ""
    started_at: float = 0.0
    turn_started_at: float | None = None
    last_activity: float = 0.0
    state_since: float = 0.0
    finished_at: float | None = None
    waited_s: float = 0.0            # this turn's time in waiting_for_input, closed spans
    waiting_since: float | None = None
    tools: int = 0
    turns: int = 0
    acked_seq: int = 0              # nothing to look at until the first change
    ended: bool = False
    error: str = ""

    def active_s(self, now: float) -> float:
        if self.turn_started_at is None:
            return 0.0
        end = self.finished_at if self.state in (DONE, ERROR) and self.finished_at else now
        waited = self.waited_s + ((end - self.waiting_since) if self.waiting_since is not None else 0.0)
        return max(0.0, end - self.turn_started_at - waited)


@dataclass
class Turn:
    t: float
    project: str
    active_s: float
    tools: int


class SessionTracker:
    def __init__(self, history_path: Path | None, clock: Callable[[], float] = time.time) -> None:
        self.path = history_path
        self.clock = clock
        self.lock = threading.Lock()
        self.sessions: dict[str, Session] = {}
        self.history: list[Turn] = self._load()
        self.appended = 0  # lines added since the file was last written whole
        self.events = 0

    # --- history --------------------------------------------------------------------------------------

    def _load(self) -> list[Turn]:
        if not self.path or not self.path.exists():
            return []
        turns = []
        for line in self.path.read_text(encoding="utf-8").splitlines()[-HISTORY_KEEP:]:
            try:
                d = json.loads(line)
                turns.append(Turn(float(d["t"]), str(d.get("project", "")), float(d["active_s"]), int(d.get("tools", 0))))
            except (ValueError, KeyError, TypeError):
                continue
        return turns

    def _record(self, turn: Turn) -> None:
        self.history.append(turn)
        if len(self.history) > HISTORY_KEEP:
            self.history = self.history[-HISTORY_KEEP:]
        if not self.path:
            return
        try:
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(json.dumps({"t": round(turn.t, 1), "project": turn.project, "active_s": round(turn.active_s, 1),
                                    "tools": turn.tools}) + "\n")
            # written whole again now and then, so the file doesn't grow without end
            self.appended += 1
            if self.appended >= HISTORY_KEEP:
                self.appended = 0
                tmp = self.path.with_suffix(".tmp")
                tmp.write_text("".join(json.dumps({"t": h.t, "project": h.project, "active_s": h.active_s,
                                                   "tools": h.tools}) + "\n" for h in self.history), encoding="utf-8")
                tmp.replace(self.path)
        except OSError:
            pass

    def estimate(self, project: str, elapsed: float, tools: int = 0) -> tuple[dict[str, Any] | None, str]:
        """(eta, basis): the estimate for a turn `elapsed` seconds in, or None and why not."""
        recent = self.history[-RECENT * 4:]
        same = [h for h in recent if h.project == project][-RECENT:]
        if len(same) >= SAME_FOLDER_MIN:
            pool, scope = same, f"in {project}" if project else "in this folder"
        else:
            pool, scope = recent[-RECENT:], "across folders"
        if len(pool) < MIN_SAMPLES:
            n = len(pool)
            return None, f"no estimate yet: {n} finished turn{'s' if n != 1 else ''} recorded, {MIN_SAMPLES} needed"
        durations = [h.active_s for h in pool]
        survivors = [h for h in pool if h.active_s > elapsed]
        if len(survivors) < 2:
            which = "all" if not survivors else "all but one"
            return None, f"no estimate: already longer than {which} of your last {len(pool)} turns ({scope})"
        ds = [h.active_s for h in survivors]
        med = statistics.median(ds)
        lo, hi = _quantile(ds, 0.25), _quantile(ds, 0.75)
        remaining = max(1.0, med - elapsed)
        basis = (f"median of the {len(survivors)} of your last {len(pool)} turns ({scope}) that ran past "
                 f"{_dur(elapsed)}")
        tool_counts = [h.tools for h in survivors if h.tools]
        if tool_counts and tools:
            basis += f"; {tools} tool calls so far, those turns made {int(statistics.median(tool_counts))}"
        eta = {"remaining_s": round(remaining, 1), "low_s": round(max(1.0, lo - elapsed), 1),
               "high_s": round(max(1.0, hi - elapsed), 1), "finish_at": _iso(self.clock() + remaining),
               "samples": len(survivors), "typical_s": round(statistics.median(durations), 1), "basis": basis}
        return eta, basis

    # --- events ---------------------------------------------------------------------------------------

    def _set_state(self, s: Session, state: str, now: float) -> None:
        if state != s.state:
            if s.state == WAITING and s.waiting_since is not None:
                s.waited_s += now - s.waiting_since
                s.waiting_since = None
            if state == WAITING:
                s.waiting_since = now
            s.state = state
            s.state_since = now
            s.seq += 1

    def event(self, body: dict[str, Any]) -> dict[str, Any]:
        sid = str(body.get("session_id") or "").strip()
        name = str(body.get("event") or body.get("hook_event_name") or "").strip()
        if not sid or len(sid) > 100:
            raise LinkError(400, "session_id is required (at most 100 characters)")
        if not name:
            raise LinkError(400, "event is required")
        now = self.clock()
        with self.lock:
            self.events += 1
            s = self.sessions.get(sid)
            if s is None:
                self._prune(now)
                s = Session(id=sid, started_at=now, state_since=now, last_activity=now)
                self.sessions[sid] = s
            cwd = str(body.get("cwd") or "")[:300]
            if cwd:
                s.cwd = cwd
            s.last_activity = now
            s.ended = False if name != "SessionEnd" else True

            if name == "UserPromptSubmit":
                prompt = " ".join(str(body.get("prompt") or "").split())[:160]
                s.turns += 1
                s.title = prompt or f"turn {s.turns}"
                s.turn_started_at = now
                s.finished_at = None
                s.waited_s = 0.0
                s.waiting_since = None
                s.tools = 0
                s.error = ""
                s.state = DONE if s.state == RUNNING else s.state  # a new turn is a change even from running
                self._set_state(s, RUNNING, now)
                s.step = "thinking"
            elif name in ("PreToolUse", "PostToolUse", "PostToolUseFailure"):
                self._resume(s, now)
                detail = str(body.get("detail") or "") or describe_tool(str(body.get("tool") or body.get("tool_name") or ""),
                                                                         body.get("tool_input"))
                if name == "PreToolUse":
                    s.tools += 1
                    s.step = detail[:120]
                elif name == "PostToolUseFailure":
                    s.step = f"{detail[:100]} (failed)"
            elif name == "Notification":
                msg = " ".join(str(body.get("message") or "").split())[:160]
                if s.state == RUNNING:  # mid-turn: a permission prompt or a question
                    self._set_state(s, WAITING, now)
                    s.step = msg or "waiting for you"
                elif s.state == WAITING:
                    s.step = msg or s.step
                # after a Stop it's Claude Code's idle reminder: the turn is still done
            elif name == "Stop":
                self._finish(s, now, DONE)
                s.step = "finished"
            elif name == "StopFailure" or (body.get("error") and name not in ("PostToolUseFailure",)):
                s.error = " ".join(str(body.get("error") or "failed").split())[:160]
                self._finish(s, now, ERROR)
                s.step = s.error
            elif name == "SubagentStop":
                self._resume(s, now)
                s.step = "a subagent finished"
            elif name == "PreCompact":
                self._resume(s, now)
                s.step = "compacting the conversation"
            elif name == "SessionEnd":
                if s.state in (RUNNING, WAITING):
                    self._finish(s, now, DONE, record=False)
                s.step = "session closed"
            elif name == "SessionStart":
                if s.turn_started_at is None:
                    s.step = "session started"
            # unknown events only mark activity
            return {"ok": True, "state": s.state, "seq": s.seq}

    def _resume(self, s: Session, now: float) -> None:
        """A tool ran: whatever it was waiting for was answered."""
        if s.turn_started_at is None:  # hooks installed mid-turn: start counting now
            s.turn_started_at = now
            s.turns += 1
            s.title = s.title or f"turn {s.turns}"
        if s.state != RUNNING:
            self._set_state(s, RUNNING, now)

    def _finish(self, s: Session, now: float, state: str, record: bool = True) -> None:
        was_turn = s.turn_started_at is not None and s.state in (RUNNING, WAITING)
        self._set_state(s, state, now)
        s.finished_at = now
        if record and was_turn and state == DONE:
            active = s.active_s(now)
            if active >= MIN_TURN_S:
                self._record(Turn(now, _project(s.cwd), active, s.tools))

    def _prune(self, now: float) -> None:
        for sid, s in list(self.sessions.items()):
            idle = now - s.last_activity
            if (s.ended and idle > ENDED_TTL_S) or (s.state in (DONE, ERROR) and idle > DONE_TTL_S):
                del self.sessions[sid]
        if len(self.sessions) > MAX_SESSIONS:
            for sid, _ in sorted(self.sessions.items(), key=lambda kv: kv[1].last_activity)[: len(self.sessions) - MAX_SESSIONS]:
                del self.sessions[sid]

    def ack(self, sid: str | None) -> int:
        with self.lock:
            if sid and sid not in self.sessions:
                raise LinkError(404, "no such session")
            targets = [self.sessions[sid]] if sid else list(self.sessions.values())
            for s in targets:
                s.acked_seq = s.seq
            return len(targets)

    # --- the listing ----------------------------------------------------------------------------------

    def listing(self) -> dict[str, Any]:
        now = self.clock()
        with self.lock:
            self._prune(now)
            rows = sorted(self.sessions.values(), key=lambda s: s.last_activity, reverse=True)[:LIST_MAX]
            out = []
            for s in rows:
                project = _project(s.cwd)
                elapsed = s.active_s(now)
                eta, basis = (None, "")
                if s.state == RUNNING:
                    eta, basis = self.estimate(project, elapsed, s.tools)
                elif s.state == WAITING:
                    basis = "waiting on you: no estimate while it waits"
                out.append({
                    "id": s.id, "title": s.title or project or s.id[:8], "cwd": s.cwd, "project": project,
                    "state": s.state, "seq": s.seq, "step": s.step, "tools": s.tools, "turns": s.turns,
                    "started_at": _iso(s.started_at), "turn_started_at": _iso(s.turn_started_at),
                    "last_activity": _iso(s.last_activity), "finished_at": _iso(s.finished_at),
                    "elapsed_s": round(elapsed, 1), "quiet_s": round(now - s.last_activity, 1),
                    "since_s": round(now - s.state_since, 1), "eta": eta, "eta_basis": basis,
                    "acked": s.acked_seq == s.seq and s.state != RUNNING, "ended": s.ended,
                    "error": s.error or None,
                })
            return {"ok": True, "now": _iso(now), "sessions": out, "turns_recorded": len(self.history),
                    "events": self.events}
