"""POST /v1/messages through Claude Code (the owner's Claude subscription), with the tablet's tools.

Two layers: a scripted fake Claude Code session (fast, exact: the SSE the tablet sees, the tool round
trip, session reuse, replay, disconnects, timeouts), and — only with CATALYST_LINK_LIVE=1 on a PC where
Claude Code is logged in — the real Agent SDK and CLI, driven by a fake tablet over HTTP."""
from __future__ import annotations

import asyncio
import json
import os
import socket
import time
import unittest
from pathlib import Path
from typing import Any, AsyncIterator
from unittest import mock

from helpers import LinkCase

from catalyst_link import claude_code
from catalyst_link.claude_code import PREFIX, AgentSpec, fingerprint, transcript
from catalyst_link.server import Config, pick_backend

SYSTEM = [{"type": "text", "text": "You are the pit technician.", "cache_control": {"type": "ephemeral"}}]
TOOLS = [{"name": "get_battery", "description": "The robot's battery", "eager_input_streaming": True,
          "input_schema": {"type": "object", "properties": {"which": {"type": "string"}}, "required": ["which"]}},
         {"name": "set_tunable", "description": "Change a tunable (confirmed on the tablet)",
          "input_schema": {"type": "object", "properties": {"key": {"type": "string"}, "value": {"type": "number"}}}}]


def body(messages: list[dict[str, Any]], **extra: Any) -> dict[str, Any]:
    return {"model": "claude-opus-5", "max_tokens": 32000, "stream": True, "fallbacks": "default",
            "thinking": {"type": "adaptive", "display": "summarized"}, "output_config": {"effort": "medium"},
            "system": SYSTEM, "tools": TOOLS, "messages": messages, **extra}


def user(text: str) -> dict[str, Any]:
    return {"role": "user", "content": [{"type": "text", "text": text}]}


def frames(raw: bytes) -> list[tuple[str, dict[str, Any]]]:
    out = []
    for block in raw.decode().split("\n\n"):
        if block.strip():
            lines = dict(line.split(": ", 1) for line in block.split("\n"))
            out.append((lines["event"], json.loads(lines["data"])))
    return out


def message(blocks: list[dict[str, Any]], stop: str, model: str = "claude-opus-5-5") -> list[dict[str, Any]]:
    """The raw stream events Claude Code emits for one API message (tool names still prefixed)."""
    ev: list[dict[str, Any]] = [{"type": "message_start", "message": {
        "id": "msg_x", "type": "message", "role": "assistant", "model": model, "content": [],
        "stop_reason": None, "usage": {"input_tokens": 10, "output_tokens": 1}}}]
    for i, b in enumerate(blocks):
        if b["type"] == "text":
            ev.append({"type": "content_block_start", "index": i, "content_block": {"type": "text", "text": ""}})
            ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "text_delta", "text": b["text"]}})
        else:
            ev.append({"type": "content_block_start", "index": i, "content_block": {
                "type": "tool_use", "id": b["id"], "name": PREFIX + b["name"], "input": {}, "caller": {"type": "direct"}}})
            raw = json.dumps(b["input"])
            for part in (raw[:5], raw[5:]):
                ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "input_json_delta", "partial_json": part}})
        ev.append({"type": "content_block_stop", "index": i})
    ev.append({"type": "message_delta", "delta": {"stop_reason": stop, "stop_sequence": None}, "usage": {"output_tokens": 20}})
    ev.append({"type": "message_stop"})
    return ev


def text_of(content: list[dict[str, Any]]) -> str:
    return "\n".join(b.get("text", "") for b in content if b.get("type") == "text")


class FakeAgent:
    """A scripted Claude Code session. Tool calls go through spec.call_tool exactly as the SDK's MCP
    handler would, after the tool_use message has streamed."""

    def __init__(self, spec: AgentSpec, owner: "ClaudeCodeCase") -> None:
        self.spec = spec
        self.owner = owner
        self.turns: list[list[dict[str, Any]]] = []
        self.results: list[tuple[Any, bool]] = []
        self.closed = False
        owner.agents.append(self)

    async def start(self) -> None:
        pass

    async def turn(self, content: list[dict[str, Any]]) -> AsyncIterator[tuple[str, Any]]:
        self.turns.append(content)
        text = text_of(content)
        if "hang" in text:
            for e in message([], "end_turn")[:1]:
                yield "event", e
            await asyncio.sleep(60)
        if "battery" in text:
            calls = [{"type": "tool_use", "id": "toolu_1", "name": "get_battery", "input": {"which": "main"}},
                     {"type": "tool_use", "id": "toolu_2", "name": "get_battery", "input": {"which": "aux"}}]
            for e in message([{"type": "text", "text": "Checking."}, *calls], "tool_use"):
                yield "event", e
            # Claude Code runs the two calls; the aux one first, to show matching is by input, not order.
            aux = await self.spec.call_tool("get_battery", {"which": "aux"})
            main = await self.spec.call_tool("get_battery", {"which": "main"})
            self.results += [main, aux]
            answer = f"main {main[0]}, aux {aux[0]}{' (aux failed)' if aux[1] else ''}"
            for e in message([{"type": "text", "text": answer}], "end_turn"):
                yield "event", e
        elif "fail" in text:
            yield "error", ("rate_limit_error", "You've hit your limit")
        else:
            for e in message([{"type": "text", "text": f"Answer {len(self.turns)}: {text[-30:]}"}], "end_turn"):
                yield "event", e
        yield "result", {"is_error": False, "session_id": "sess-1", "cost_usd": 0.0}

    async def close(self) -> None:
        self.closed = True


class ClaudeCodeCase(LinkCase):
    claude = "claude-code"

    def setUp(self) -> None:
        self.agents: list[FakeAgent] = []
        self.agent_factory = lambda spec: FakeAgent(spec, self)
        super().setUp()
        self.backend = self.app.proxy
        self.backend.keepalive = 0.2

    def post(self, messages: list[dict[str, Any]], **extra: Any) -> tuple[int, list[tuple[str, dict[str, Any]]]]:
        status, (_, raw) = self.request("POST", "/v1/messages", body(messages, **extra), raw=True)
        return status, frames(raw) if status == 200 else json.loads(raw)

    def claude_logs(self, count: int) -> list[dict[str, Any]]:
        deadline = time.monotonic() + 5
        while True:
            logs = [r for r in self.log_lines() if r.get("kind") == "claude"]
            if len(logs) >= count or time.monotonic() > deadline:
                return logs
            time.sleep(0.02)

    def wait(self, cond: Any, what: str) -> None:
        deadline = time.monotonic() + 5
        while not cond():
            if time.monotonic() > deadline:
                self.fail(f"timed out waiting for {what}")
            time.sleep(0.02)


class StatusAndAnswers(ClaudeCodeCase):
    def test_status_says_claude_through_claude_code(self) -> None:
        st = self.request("GET", "/link/status")[1]
        self.assertTrue(st["claude"])
        self.assertEqual(st["claude_via"], "claude-code")

    def test_a_question_streams_as_the_messages_api(self) -> None:
        status, got = self.post([user("Why is the elevator hot?")])
        self.assertEqual(status, 200)
        self.assertEqual([e for e, _ in got], ["message_start", "content_block_start", "content_block_delta",
                                                "content_block_stop", "message_delta", "message_stop"])
        self.assertEqual(got[0][1]["message"]["model"], "claude-opus-5-5")
        self.assertIn("elevator hot", got[2][1]["delta"]["text"])
        spec = self.agents[0].spec
        self.assertEqual(spec.system, "You are the pit technician.")
        self.assertEqual([t["name"] for t in spec.tools], ["get_battery", "set_tunable"])
        self.assertEqual(spec.effort, "medium")
        self.assertIsNone(spec.model)  # the tablet's "claude-opus-5" doesn't apply; Claude Code's default does
        self.assertEqual(spec.env["ANTHROPIC_API_KEY"], "")  # never the API, even if the PC has a key
        self.assertEqual(self.agents[0].turns, [[{"type": "text", "text": "Why is the elevator hot?"}]])
        log = self.claude_logs(1)[0]
        self.assertEqual((log["backend"], log["how"], log["ok"], log["model"]), ("claude-code", "new", True, "claude-opus-5-5"))

    def test_tool_round_trip_through_the_tablet(self) -> None:
        q = user("What does the battery read?")
        status, got = self.post([q])
        self.assertEqual(status, 200)
        uses = [d["content_block"] for e, d in got if e == "content_block_start" and d["content_block"]["type"] == "tool_use"]
        self.assertEqual([(u["id"], u["name"]) for u in uses], [("toolu_1", "get_battery"), ("toolu_2", "get_battery")])
        self.assertEqual(got[-2][1]["delta"]["stop_reason"], "tool_use")
        self.assertEqual(got[-1][0], "message_stop")

        # The tablet ran them (and would have asked the technician first, for a write) and sends the results.
        echo = {"role": "assistant", "content": [{"type": "text", "text": "Checking."},
                                                 {**uses[0], "input": {"which": "main"}}, {**uses[1], "input": {"which": "aux"}}]}
        results = {"role": "user", "content": [
            {"type": "tool_result", "tool_use_id": "toolu_1", "content": '{"volts":12.4}'},
            {"type": "tool_result", "tool_use_id": "toolu_2", "content": "no aux battery", "is_error": True}]}
        status, got = self.post([q, echo, results])
        self.assertEqual(status, 200)
        text = "".join(d["delta"].get("text", "") for e, d in got if e == "content_block_delta")
        self.assertEqual(text, 'main {"volts":12.4}, aux no aux battery (aux failed)')
        self.assertEqual(len(self.agents), 1)  # the same Claude Code session went on
        self.assertEqual(self.agents[0].results, [('{"volts":12.4}', False), ("no aux battery", True)])
        self.assertEqual([r["how"] for r in self.claude_logs(2)], ["new", "tool_results"])

        # A follow-up question goes to the same session as just the new words.
        answer = {"role": "assistant", "content": [{"type": "thinking", "thinking": "…", "signature": ""},
                                                   {"type": "text", "text": text}]}
        status, got = self.post([q, echo, results, answer, user("And the aux?")])
        self.assertEqual(status, 200)
        self.assertEqual(len(self.agents), 1)
        self.assertEqual(self.agents[0].turns[-1], [{"type": "text", "text": "And the aux?"}])
        self.assertEqual(self.claude_logs(3)[-1]["how"], "next_question")

    def test_an_unknown_conversation_is_replayed(self) -> None:
        history = [user("First question"), {"role": "assistant", "content": [{"type": "text", "text": "An earlier answer"}]},
                   user("Second question")]
        status, got = self.post(history)
        self.assertEqual(status, 200)
        first = self.agents[0].turns[0]
        self.assertIn("An earlier answer", first[0]["text"])
        self.assertIn("First question", first[0]["text"])
        self.assertEqual(first[-1], {"type": "text", "text": "Second question"})
        self.assertEqual(self.claude_logs(1)[0]["how"], "replayed")

    def test_orphaned_tool_results_are_replayed_too(self) -> None:
        history = [user("battery?"), {"role": "assistant", "content": [
            {"type": "tool_use", "id": "toolu_9", "name": "get_battery", "input": {"which": "main"}}]},
            {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_9", "content": "12.1 V"}]}]
        status, _ = self.post(history)
        self.assertEqual(status, 200)
        replay = self.agents[0].turns[0][0]["text"]
        self.assertIn('You called get_battery {"which": "main"}', replay)
        self.assertIn("Result of get_battery: 12.1 V", replay)

    def test_an_error_reaches_the_tablet_as_an_error_event(self) -> None:
        status, got = self.post([user("this will fail")])
        self.assertEqual(status, 200)
        self.assertEqual(got, [("error", {"type": "error", "error": {"type": "rate_limit_error",
                                                                      "message": "You've hit your limit"}})])
        self.wait(lambda: self.agents[0].closed, "the broken session to close")

    def test_the_tablet_hanging_up_ends_the_session(self) -> None:
        conn = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        data = json.dumps(body([user("hang please")])).encode()
        conn.sendall(b"POST /v1/messages HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                     + f"X-Link-Token: {self.token}\r\nContent-Length: {len(data)}\r\n\r\n".encode() + data)
        self.assertIn(b"message_start", conn.recv(4096) + conn.recv(4096))
        conn.close()
        self.wait(lambda: self.agents and self.agents[0].closed, "the session to close")
        self.wait(lambda: any(r.get("disconnected") for r in self.claude_logs(1)), "the log line")

    def test_a_tablet_that_never_answers_a_tool(self) -> None:
        self.backend.tool_timeout = 0.3
        status, got = self.post([user("battery?")])
        self.assertEqual(got[-2][1]["delta"]["stop_reason"], "tool_use")
        self.wait(lambda: self.agents[0].closed, "the session to give up")
        self.assertEqual(self.agents[0].results[0], ("The tablet never sent this tool's result.", True))

    def test_bad_bodies(self) -> None:
        status, err = self.post([{"role": "assistant", "content": "hi"}])
        self.assertEqual((status, err["error"]["type"]), (400, "invalid_request_error"))
        self.assertEqual(self.agents, [])

    def test_needs_the_token(self) -> None:
        status, _ = self.request("POST", "/v1/messages", body([user("hi")]), token=None)
        self.assertEqual(status, 401)
        self.assertEqual(self.agents, [])


class Helpers(unittest.TestCase):
    def test_fingerprint_is_the_text(self) -> None:
        a = {"role": "assistant", "content": [{"type": "thinking", "thinking": "x", "signature": "s"},
                                              {"type": "text", "text": "Hello"}]}
        self.assertEqual(fingerprint(a), claude_code.text_fingerprint("Hello"))
        self.assertIsNone(fingerprint({"role": "assistant", "content": []}))
        self.assertIsNone(fingerprint(user("Hello")))

    def test_transcript_caps_long_results(self) -> None:
        t = transcript([{"role": "user", "content": [{"type": "tool_result", "tool_use_id": "a", "content": "x" * 9000}]}])
        self.assertIn("…(cut)", t)
        self.assertLess(len(t), 5000)

    def test_mcp_content(self) -> None:
        self.assertEqual(claude_code.mcp_content("hi"), [{"type": "text", "text": "hi"}])
        img = {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": "AAA"}}
        self.assertEqual(claude_code.mcp_content([img]), [{"type": "image", "data": "AAA", "mimeType": "image/jpeg"}])

    def test_auto_picks_api_with_a_key_else_claude_code(self) -> None:
        cfg = Config(repo=Path("."))
        with mock.patch.dict(os.environ, {"ANTHROPIC_API_KEY": "sk-x"}):
            self.assertEqual(pick_backend(cfg, None), "api")
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("ANTHROPIC_API_KEY", None)
            self.assertEqual(pick_backend(cfg, None), "claude-code")
            cfg.claude = "api"
            self.assertEqual(pick_backend(cfg, None), "api")

    def test_token_from_env_or_file(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("CLAUDE_CODE_OAUTH_TOKEN", None)
            home = Path(d)
            self.assertNotIn("CLAUDE_CODE_OAUTH_TOKEN", claude_code.child_env(home, 60))
            (home / "claude-oauth-token").write_text("tok-from-file\n")
            self.assertEqual(claude_code.child_env(home, 60)["CLAUDE_CODE_OAUTH_TOKEN"], "tok-from-file")
            os.environ["CLAUDE_CODE_OAUTH_TOKEN"] = "tok-from-env"
            self.assertEqual(claude_code.oauth_token(home), "tok-from-env")


class OffAndMissing(LinkCase):
    claude = "off"

    def test_off(self) -> None:
        self.assertFalse(self.request("GET", "/link/status")[1]["claude"])
        status, (_, raw) = self.request("POST", "/v1/messages", body([user("hi")]), raw=True)
        self.assertEqual(status, 503)


class NoCli(LinkCase):
    claude = "claude-code"

    def setUp(self) -> None:
        patch = mock.patch.object(claude_code, "find_cli", return_value=None)
        patch.start()
        self.addCleanup(patch.stop)
        super().setUp()

    def test_without_claude_code_it_says_so(self) -> None:
        self.assertFalse(self.request("GET", "/link/status")[1]["claude"])
        status, (_, raw) = self.request("POST", "/v1/messages", body([user("hi")]), raw=True)
        self.assertEqual(status, 503)
        self.assertIn("Claude Code", json.loads(raw)["error"]["message"])


# --- the real thing: Claude Code logged in on this PC --------------------------------------------------

@unittest.skipUnless(os.environ.get("CATALYST_LINK_LIVE") == "1" and claude_code.sdk is not None,
                     "set CATALYST_LINK_LIVE=1 on a PC where Claude Code is logged in")
class LiveClaudeCode(LinkCase):
    """A fake tablet drives the real Claude Code through the Link: a tool call, its result, a follow-up."""

    claude = "claude-code"

    def post(self, messages: list[dict[str, Any]]) -> list[tuple[str, dict[str, Any]]]:
        b = body(messages)
        b["system"] = [{"type": "text", "text": "You are a test harness. Always use the tools you are given to "
                                                "answer; after a tool result, answer with the number only."}]
        status, (_, raw) = self.request("POST", "/v1/messages", b, raw=True)
        self.assertEqual(status, 200, raw)
        got = frames(raw)
        self.assertNotIn("error", [e for e, _ in got], got)
        return got

    def test_tool_round_trip(self) -> None:
        q = user("What is the main battery voltage right now?")
        got = self.post([q])
        uses = [d["content_block"] for e, d in got if e == "content_block_start" and d["content_block"]["type"] == "tool_use"]
        self.assertTrue(uses, got)
        self.assertEqual(uses[0]["name"], "get_battery")
        self.assertEqual(got[-2][1]["delta"]["stop_reason"], "tool_use")
        echo = {"role": "assistant", "content": uses}
        results = {"role": "user", "content": [{"type": "tool_result", "tool_use_id": u["id"],
                                                "content": '{"volts": 12.37}'} for u in uses]}
        got = self.post([q, echo, results])
        text = "".join(d["delta"].get("text", "") for e, d in got if e == "content_block_delta")
        self.assertIn("12.37", text)
        answer = {"role": "assistant", "content": [{"type": "text", "text": text}]}
        got = self.post([q, echo, results, answer, user("Repeat the number you just told me, nothing else.")])
        text2 = "".join(d["delta"].get("text", "") for e, d in got if e == "content_block_delta")
        self.assertIn("12.37", text2)
        deadline = time.monotonic() + 5
        while True:  # the last request's log line is written just after its response ends
            logs = [r for r in self.log_lines() if r.get("kind") == "claude"]
            if len(logs) >= 3 or time.monotonic() > deadline:
                break
            time.sleep(0.05)
        self.assertEqual([r["how"] for r in logs], ["new", "tool_results", "next_question"])
        print(f"\n  live: served by {logs[0].get('model')}; answers {text!r}, {text2!r}")


if __name__ == "__main__":
    unittest.main()
