"""Claude Code's sessions: the hook, the tracker's states and estimates, and the HTTP endpoints."""
from __future__ import annotations

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from helpers import LinkCase

from catalyst_link import cli, hook
from catalyst_link.sessions import SessionTracker


class Clock:
    def __init__(self, t: float = 1_800_000_000.0) -> None:
        self.t = t

    def __call__(self) -> float:
        return self.t

    def tick(self, s: float) -> None:
        self.t += s


def ev(tr: SessionTracker, name: str, sid: str = "s1", cwd: str = "C:/dev/CatalystX1", **kw: object) -> dict:
    return tr.event({"event": name, "session_id": sid, "cwd": cwd, **kw})


def row(tr: SessionTracker, sid: str = "s1") -> dict:
    return next(s for s in tr.listing()["sessions"] if s["id"] == sid)


def turn(tr: SessionTracker, clock: Clock, seconds: float, sid: str = "s1", cwd: str = "C:/dev/CatalystX1",
         tools: int = 2) -> None:
    ev(tr, "UserPromptSubmit", sid, cwd, prompt="do a thing")
    for _ in range(tools):
        ev(tr, "PreToolUse", sid, cwd, tool="Read", detail="reading Robot.java")
    clock.tick(seconds)
    ev(tr, "Stop", sid, cwd)


class TrackerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp(prefix="cc-test-"))
        self.clock = Clock()
        self.tr = SessionTracker(self.dir / "claude-turns.jsonl", clock=self.clock)

    def test_a_turn_runs_waits_and_finishes(self) -> None:
        ev(self.tr, "SessionStart", source="startup")
        r = row(self.tr)
        self.assertEqual((r["state"], r["seq"], r["acked"]), ("done", 0, True))  # nothing to look at yet
        self.assertEqual(r["step"], "session started")

        ev(self.tr, "UserPromptSubmit", prompt="Fix the shooter's\nsecond line")
        r = row(self.tr)
        self.assertEqual((r["state"], r["title"], r["step"], r["seq"]), ("running", "Fix the shooter's second line", "thinking", 1))

        self.clock.tick(10)
        ev(self.tr, "PreToolUse", tool="Edit", tool_input={"file_path": "C:\\dev\\x\\Shooter.java"})
        r = row(self.tr)
        self.assertEqual((r["step"], r["tools"]), ("editing Shooter.java", 1))

        ev(self.tr, "Notification", message="Claude needs your permission to use Bash")
        r = row(self.tr)
        self.assertEqual((r["state"], r["seq"]), ("waiting_for_input", 2))
        self.assertIsNone(r["eta"])
        self.assertIn("waiting on you", r["eta_basis"])

        self.clock.tick(100)  # the owner was away: not counted as Claude's time
        ev(self.tr, "PreToolUse", tool="Bash", tool_input={"command": "./gradlew build"})
        self.assertEqual(row(self.tr)["state"], "running")
        self.clock.tick(20)
        ev(self.tr, "Stop")
        r = row(self.tr)
        self.assertEqual((r["state"], r["step"], r["seq"]), ("done", "finished", 4))
        self.assertAlmostEqual(r["elapsed_s"], 30, delta=0.01)
        self.assertEqual(len(self.tr.history), 1)
        self.assertAlmostEqual(self.tr.history[0].active_s, 30, delta=0.01)
        self.assertEqual(self.tr.history[0].project, "CatalystX1")

        # Claude Code's idle reminder after a Stop changes nothing
        ev(self.tr, "Notification", message="Claude is waiting for your input")
        r = row(self.tr)
        self.assertEqual((r["state"], r["seq"], r["step"]), ("done", 4, "finished"))

    def test_history_survives_a_restart(self) -> None:
        turn(self.tr, self.clock, 40)
        turn(self.tr, self.clock, 0.2)  # too short to be a turn
        again = SessionTracker(self.dir / "claude-turns.jsonl", clock=self.clock)
        self.assertEqual([round(h.active_s) for h in again.history], [40])

    def test_no_estimate_without_history(self) -> None:
        turn(self.tr, self.clock, 30)
        ev(self.tr, "UserPromptSubmit", prompt="again")
        r = row(self.tr)
        self.assertIsNone(r["eta"])
        self.assertIn("1 finished turn recorded, 3 needed", r["eta_basis"])

    def test_estimate_is_the_median_of_turns_that_ran_this_long(self) -> None:
        for d in (20, 60, 100, 200, 300):
            turn(self.tr, self.clock, d)
        ev(self.tr, "UserPromptSubmit", prompt="next")
        self.clock.tick(80)  # past 20 and 60: the survivors are 100, 200, 300
        r = row(self.tr)
        eta = r["eta"]
        self.assertIsNotNone(eta)
        self.assertAlmostEqual(eta["remaining_s"], 200 - 80, delta=0.01)
        self.assertAlmostEqual(eta["low_s"], 150 - 80, delta=0.01)
        self.assertAlmostEqual(eta["high_s"], 250 - 80, delta=0.01)
        self.assertEqual(eta["samples"], 3)
        self.assertEqual(eta["typical_s"], 100)
        self.assertIn("median of the 3 of your last 5 turns (in CatalystX1) that ran past 1m20s", eta["basis"])
        self.assertTrue(eta["finish_at"])

        self.clock.tick(300)  # longer than all of them
        r = row(self.tr)
        self.assertIsNone(r["eta"])
        self.assertIn("already longer than all of your last 5 turns", r["eta_basis"])

    def test_estimate_prefers_the_same_folder(self) -> None:
        for _ in range(5):
            turn(self.tr, self.clock, 1000, sid="other", cwd="C:/dev/Elsewhere")
        for d in (10, 20, 30, 40, 50):
            turn(self.tr, self.clock, d)
        ev(self.tr, "UserPromptSubmit", prompt="next")
        self.clock.tick(5)
        eta = row(self.tr)["eta"]
        self.assertAlmostEqual(eta["remaining_s"], 30 - 5, delta=0.01)
        self.assertIn("(in CatalystX1)", eta["basis"])
        ev(self.tr, "UserPromptSubmit", sid="new", cwd="C:/dev/Brand", prompt="x")
        self.clock.tick(5)
        self.assertIn("across folders", row(self.tr, "new")["eta"]["basis"])

    def test_errors_ends_and_acks(self) -> None:
        ev(self.tr, "UserPromptSubmit", prompt="x")
        ev(self.tr, "StopFailure", error="API Error: overloaded")
        r = row(self.tr)
        self.assertEqual((r["state"], r["error"]), ("error", "API Error: overloaded"))
        self.assertFalse(r["acked"])
        self.assertEqual(self.tr.ack("s1"), 1)
        self.assertTrue(row(self.tr)["acked"])
        ev(self.tr, "UserPromptSubmit", prompt="y")
        self.assertFalse(row(self.tr)["acked"])  # running is never "acked"
        ev(self.tr, "SessionEnd", reason="exit")
        r = row(self.tr)
        self.assertEqual((r["state"], r["ended"], r["step"]), ("done", True, "session closed"))
        self.assertEqual(len(self.tr.history), 0)  # a turn cut off by closing isn't a finished turn
        self.tr.ack(None)
        self.assertTrue(row(self.tr)["acked"])

    def test_bad_events_are_refused(self) -> None:
        from catalyst_link.state import LinkError
        with self.assertRaises(LinkError):
            self.tr.event({"event": "Stop"})
        with self.assertRaises(LinkError):
            self.tr.event({"session_id": "x"})
        with self.assertRaises(LinkError):
            self.tr.ack("nope")

    def test_old_sessions_leave_the_list(self) -> None:
        turn(self.tr, self.clock, 10)
        self.clock.tick(25 * 3600)
        self.assertEqual(self.tr.listing()["sessions"], [])


class HookTest(unittest.TestCase):
    def test_trim_keeps_a_few_words(self) -> None:
        big = {"command": "x" * 5000}
        t = hook.trim({"hook_event_name": "PreToolUse", "session_id": "abc", "cwd": "/r", "tool_name": "Bash",
                       "tool_input": big, "tool_response": "y" * 10000})
        self.assertEqual(set(t), {"event", "session_id", "cwd", "tool", "detail"})
        self.assertLessEqual(len(t["detail"]), 90)
        p = hook.trim({"hook_event_name": "UserPromptSubmit", "session_id": "abc", "prompt": "\n\n  first line  \nsecret"})
        self.assertEqual(p["prompt"], "first line")
        self.assertNotIn("prompt", hook.trim({"hook_event_name": "UserPromptSubmit", "session_id": "a", "prompt": "p"}, False))

    def test_describe(self) -> None:
        self.assertEqual(hook.describe_tool("Read", {"file_path": "/a/b/Robot.java"}), "reading Robot.java")
        self.assertEqual(hook.describe_tool("Write", {"file_path": "C:\\a\\New.java"}), "writing New.java")
        self.assertEqual(hook.describe_tool("Bash", {"command": "ls", "description": "List files"}), "running List files")
        self.assertEqual(hook.describe_tool("WebFetch", {"url": "https://docs.wpilib.org/en/x"}), "reading docs.wpilib.org")
        self.assertEqual(hook.describe_tool("mcp__catalyst__catalyst_docs_search", {}), "using catalyst_docs_search (catalyst)")
        self.assertEqual(hook.describe_tool("TodoWrite", None), "updating its plan")

    def test_main_is_silent_and_never_fails(self) -> None:
        out = io.StringIO()
        stdin = io.TextIOWrapper(io.BytesIO(b"{not json"))
        with mock.patch.object(hook.sys, "stdin", stdin), contextlib.redirect_stdout(out):
            self.assertEqual(hook.main(), 0)
        stdin = io.TextIOWrapper(io.BytesIO(json.dumps({"hook_event_name": "Stop", "session_id": "s"}).encode()))
        env = {"CATALYST_LINK_URL": "http://127.0.0.1:9", "CATALYST_LINK_HOME": tempfile.mkdtemp()}
        with mock.patch.object(hook.sys, "stdin", stdin), mock.patch.dict(os.environ, env), contextlib.redirect_stdout(out):
            self.assertEqual(hook.main(), 0)
        self.assertEqual(out.getvalue(), "")

    def test_settings_snippet(self) -> None:
        s = cli.hook_settings("python -m catalyst_link.hook")
        self.assertEqual(set(s["hooks"]), set(cli.HOOK_EVENTS))
        self.assertEqual(s["hooks"]["PreToolUse"][0]["matcher"], "*")
        self.assertNotIn("matcher", s["hooks"]["Stop"][0])
        self.assertEqual(s["hooks"]["Stop"][0]["hooks"][0], {"type": "command", "command": "python -m catalyst_link.hook",
                                                            "timeout": 5})
        self.assertIn("hook.py", cli.hook_command())


class EndpointTest(LinkCase):
    def post_event(self, name: str, **kw: object) -> tuple[int, dict]:
        return self.request("POST", "/v1/claude/events", {"event": name, "session_id": "sess-1", "cwd": "/w/CatalystX1", **kw})

    def test_events_need_the_token(self) -> None:
        status, body = self.request("POST", "/v1/claude/events", {"event": "Stop", "session_id": "x"}, token="wrong")
        self.assertEqual((status, body["error"]), (401, "token"))
        status, _ = self.request("GET", "/v1/claude/sessions", token=None)
        self.assertEqual(status, 401)

    def test_hook_to_listing_to_ack(self) -> None:
        self.assertEqual(self.request("GET", "/v1/claude/sessions")[1]["sessions"], [])
        # the hook itself, pointed at this Link
        ok = hook.send(hook.trim({"hook_event_name": "UserPromptSubmit", "session_id": "sess-1", "cwd": "/w/CatalystX1",
                                  "prompt": "Tune the arm"}), url=f"http://127.0.0.1:{self.port}", token=self.token)
        self.assertTrue(ok)
        self.assertEqual(self.post_event("PreToolUse", tool="Grep", detail="searching for kArmP")[0], 200)
        status, data = self.request("GET", "/v1/claude/sessions")
        self.assertEqual(status, 200)
        s = data["sessions"][0]
        self.assertEqual((s["id"], s["state"], s["title"], s["project"], s["step"], s["tools"]),
                         ("sess-1", "running", "Tune the arm", "CatalystX1", "searching for kArmP", 1))
        self.assertIsNone(s["eta"])
        self.assertIn("needed", s["eta_basis"])

        self.post_event("Stop")
        s = self.request("GET", "/v1/claude/sessions")[1]["sessions"][0]
        self.assertEqual((s["state"], s["acked"]), ("done", False))
        self.assertEqual(self.request("POST", "/v1/claude/sessions/sess-1/ack", {})[1]["acked"], 1)
        self.assertTrue(self.request("GET", "/v1/claude/sessions")[1]["sessions"][0]["acked"])
        self.assertEqual(self.request("POST", "/v1/claude/sessions/nope/ack", {})[0], 404)
        self.assertEqual(self.request("POST", "/v1/claude/sessions/ack", {})[0], 200)
        self.assertEqual(self.request("POST", "/v1/claude/events", {"event": "Stop"})[0], 400)
        # routine traffic stays out of the audit log
        self.assertFalse([ln for ln in self.log_lines() if ln.get("path", "").startswith("/v1/claude/")])


if __name__ == "__main__":
    unittest.main()
