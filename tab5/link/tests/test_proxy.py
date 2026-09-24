"""POST /v1/messages: SSE framing, beta and `fallbacks` passthrough, error mapping, disconnects.

Two layers: an injected fake SDK client (fast, exact), and the real `anthropic` SDK pointed at a fake
Messages API upstream (so what actually goes over the wire is checked too)."""
from __future__ import annotations

import json
import os
import socket
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from unittest import mock

from helpers import FakeClient, FakeStream, LinkCase

try:
    import anthropic
    import httpx2
except ImportError:  # pragma: no cover
    anthropic = None  # type: ignore[assignment]

MODEL = "claude-opus-5"
BODY: dict[str, Any] = {
    "model": MODEL, "max_tokens": 16000, "stream": True, "fallbacks": "default",
    "thinking": {"type": "adaptive", "display": "summarized"},
    "output_config": {"effort": "medium"},
    "system": [{"type": "text", "text": "You are the pit technician.", "cache_control": {"type": "ephemeral"}}],
    "tools": [{"name": "read_code", "description": "Read a file", "eager_input_streaming": True,
               "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}}],
    "messages": [{"role": "user", "content": "Why is the elevator hot?"}],
}
BETAS = "server-side-fallback-2026-07-01, mid-conversation-tool-changes-2026-07-01"

EVENTS: list[dict[str, Any]] = [
    {"type": "message_start", "message": {"id": "msg_1", "type": "message", "role": "assistant", "model": MODEL,
                                          "content": [], "stop_reason": None, "stop_sequence": None,
                                          "usage": {"input_tokens": 812, "output_tokens": 1,
                                                    "cache_read_input_tokens": 700}}},
    {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
    {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "The elevator motor is at 71 °C."}},
    {"type": "content_block_stop", "index": 0},
    {"type": "message_delta", "delta": {"stop_reason": "end_turn", "stop_sequence": None}, "usage": {"output_tokens": 42}},
    {"type": "message_stop"},
]


def frames(raw: bytes) -> list[tuple[str, dict[str, Any]]]:
    out = []
    for block in raw.decode().split("\n\n"):
        if not block.strip():
            continue
        lines = dict(line.split(": ", 1) for line in block.split("\n"))
        out.append((lines["event"], json.loads(lines["data"])))
    return out


def expected_sse(events: list[dict[str, Any]]) -> bytes:
    return b"".join(f"event: {e['type']}\ndata: {json.dumps(e, separators=(',', ':'), ensure_ascii=False)}\n\n".encode()
                    for e in events)


class FakeClientCase(LinkCase):
    def setUp(self) -> None:
        self.claude_client = FakeClient()
        super().setUp()
        self.fake = self.claude_client.messages

    def post(self, body: dict[str, Any] | None = None, betas: str | None = BETAS) -> tuple[int, Any]:
        headers = {"anthropic-beta": betas} if betas else {}
        return self.request("POST", "/v1/messages", body or BODY, headers=headers, raw=True)

    def claude_logs(self, count: int = 1) -> list[dict[str, Any]]:
        """The proxy's log records; it writes them just after the response ends, so wait a moment."""
        deadline = time.monotonic() + 5
        while True:
            logs = [r for r in self.log_lines() if r.get("kind") == "claude"]
            if len(logs) >= count or time.monotonic() > deadline:
                return logs
            time.sleep(0.02)


class ProxyStreamTest(FakeClientCase):
    def test_sse_framing_and_passthrough(self) -> None:
        self.fake.next = FakeStream(EVENTS)
        status, (resp, raw) = self.post()
        self.assertEqual(status, 200)
        self.assertEqual(resp.getheader("Content-Type"), "text/event-stream; charset=utf-8")
        self.assertIsNone(resp.getheader("Content-Length"))
        self.assertEqual(resp.getheader("Connection"), "close")
        self.assertEqual(raw, expected_sse(EVENTS))  # byte for byte the API's own wire format
        self.assertEqual([e for e, _ in frames(raw)], [e["type"] for e in EVENTS])

        call = self.fake.calls[0]
        self.assertTrue(call["stream"])
        self.assertEqual(call["betas"], ["server-side-fallback-2026-07-01", "mid-conversation-tool-changes-2026-07-01"])
        # This SDK's create() doesn't declare `fallbacks`, so it rides in extra_body; the rest are kwargs.
        self.assertEqual(call["extra_body"], {"fallbacks": "default"})
        self.assertEqual(call["model"], MODEL)
        self.assertEqual(call["thinking"], BODY["thinking"])
        self.assertEqual(call["output_config"], {"effort": "medium"})
        self.assertTrue(call["tools"][0]["eager_input_streaming"])
        self.assertEqual(call["system"][0]["cache_control"], {"type": "ephemeral"})

        log = self.claude_logs()[-1]
        self.assertEqual((log["model"], log["input_tokens"], log["output_tokens"], log["cache_read_input_tokens"]),
                         (MODEL, 812, 42, 700))
        self.assertEqual((log["stop_reason"], log["ok"], log["fell_back"], log["disconnected"]),
                         ("end_turn", True, False, False))

    def test_keepalive_pings_fill_silences(self) -> None:
        self.app.proxy.keepalive = 0.05
        self.fake.next = FakeStream(EVENTS, delay=0.25)  # the model thinking, with nothing to show
        status, (_, raw) = self.post()
        self.assertEqual(status, 200)
        got = frames(raw)
        self.assertIn(("ping", {"type": "ping"}), got)
        self.assertEqual([d for e, d in got if e != "ping"], EVENTS)

    def test_no_betas_means_no_betas_argument(self) -> None:
        self.fake.next = FakeStream(EVENTS)
        self.post(betas=None)
        self.assertIsNone(self.fake.calls[0]["betas"])

    def test_server_side_fallback_is_logged(self) -> None:
        fallback = {"type": "content_block_start", "index": 0,
                    "content_block": {"type": "fallback", "from": {"model": MODEL}, "to": {"model": "claude-opus-4-8"}}}
        events = [EVENTS[0], fallback, {"type": "content_block_stop", "index": 0}, *EVENTS[1:]]
        self.fake.next = FakeStream(events)
        status, (_, raw) = self.post()
        self.assertEqual(status, 200)
        self.assertEqual(frames(raw)[1][1]["content_block"]["type"], "fallback")  # passed through untouched
        log = self.claude_logs()[-1]
        self.assertTrue(log["fell_back"])
        self.assertEqual(log["model"], "claude-opus-4-8")

    def test_sdk_model_objects_are_serialised_as_the_wire_json(self) -> None:
        if anthropic is None:
            self.skipTest("anthropic not installed")
        from anthropic.types.beta import BetaRawMessageStreamEvent
        from pydantic import TypeAdapter

        adapter = TypeAdapter(BetaRawMessageStreamEvent)
        self.fake.next = FakeStream([adapter.validate_python(e) for e in EVENTS])
        _, (_, raw) = self.post()
        got = [d for _, d in frames(raw)]
        self.assertEqual(got[2], EVENTS[2])
        self.assertEqual(got[0]["message"]["usage"]["input_tokens"], 812)
        self.assertEqual([d["type"] for d in got], [e["type"] for e in EVENTS])


@unittest.skipIf(anthropic is None, "anthropic not installed")
class ProxyErrorTest(FakeClientCase):
    REQUEST = None

    @staticmethod
    def status_error(cls: type, status: int, etype: str, message: str) -> Exception:
        req = httpx2.Request("POST", "https://api.anthropic.com/v1/messages")
        body = {"type": "error", "error": {"type": etype, "message": message}, "request_id": "req_1"}
        return cls(message, response=httpx2.Response(status, request=req), body=body)

    def test_error_before_the_stream_keeps_its_status(self) -> None:
        self.fake.next = self.status_error(anthropic.RateLimitError, 429, "rate_limit_error", "slow down")
        status, (resp, raw) = self.post()
        self.assertEqual(status, 429)
        self.assertEqual(json.loads(raw), {"type": "error", "error": {"type": "rate_limit_error", "message": "slow down"},
                                           "request_id": "req_1"})
        self.assertEqual(self.claude_logs()[-1]["status"], 429)

    def test_connection_error_is_502(self) -> None:
        self.fake.next = anthropic.APIConnectionError(request=httpx2.Request("POST", "https://api.anthropic.com"))
        status, (_, raw) = self.post()
        self.assertEqual(status, 502)
        self.assertEqual(json.loads(raw)["error"]["type"], "api_error")

    def test_unknown_error_without_body(self) -> None:
        self.fake.next = RuntimeError("boom")
        status, (_, raw) = self.post()
        self.assertEqual(status, 500)
        self.assertEqual(json.loads(raw), {"type": "error", "error": {"type": "api_error", "message": "boom"}})

    def test_error_mid_stream_is_an_error_event(self) -> None:
        overloaded = self.status_error(anthropic.APIStatusError, 200, "overloaded_error", "Overloaded")
        self.fake.next = FakeStream(EVENTS[:3], raise_after=overloaded)
        status, (_, raw) = self.post()
        self.assertEqual(status, 200)
        got = frames(raw)
        self.assertEqual([e for e, _ in got], ["message_start", "content_block_start", "content_block_delta", "error"])
        self.assertEqual(got[-1][1]["type"], "error")
        self.assertEqual(got[-1][1]["error"], {"type": "overloaded_error", "message": "Overloaded"})
        log = self.claude_logs()[-1]
        self.assertFalse(log["ok"])
        self.assertEqual(log["error"]["type"], "overloaded_error")
        self.assertTrue(self.fake.next.closed.is_set())


class ProxyEdgeTest(FakeClientCase):
    def test_tablet_disconnect_closes_the_sdk_stream(self) -> None:
        many = [EVENTS[0]] + [{"type": "content_block_delta", "index": 0,
                               "delta": {"type": "text_delta", "text": "x" * 200}}] * 2000
        stream = FakeStream(many, delay=0.002)
        self.fake.next = stream
        body = json.dumps(BODY).encode()
        sock = socket.create_connection(("127.0.0.1", self.port))
        sock.sendall(b"POST /v1/messages HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                     + f"X-Link-Token: {self.token}\r\nContent-Length: {len(body)}\r\n\r\n".encode() + body)
        got = b""
        while b"event: message_start" not in got:
            got += sock.recv(4096)
        sock.close()  # the technician walked away mid-answer
        self.assertTrue(stream.closed.wait(10), "the SDK stream was not closed")
        self.assertLess(stream.yielded, len(many))
        self.assertTrue(self.claude_logs()[-1]["disconnected"])

    def test_bad_requests_answer_in_the_api_format(self) -> None:
        status, (_, raw) = self.request("POST", "/v1/messages", {"model": MODEL}, raw=True)
        self.assertEqual(status, 400)
        self.assertEqual(json.loads(raw)["error"]["type"], "invalid_request_error")
        status, (_, raw) = self.request("POST", "/v1/messages", b"{nope", raw=True)
        self.assertEqual(status, 400)
        self.assertEqual(json.loads(raw)["type"], "error")
        self.assertEqual(self.fake.calls, [])

    def test_needs_the_token(self) -> None:
        status, body = self.request("POST", "/v1/messages", BODY, token=None)
        self.assertEqual((status, body), (401, {"ok": False, "error": "token"}))
        self.assertEqual(self.fake.calls, [])

    def test_status_reports_claude(self) -> None:
        self.assertTrue(self.request("GET", "/link/status")[1]["claude"])


class NoKeyTest(LinkCase):
    def test_without_a_key_it_says_so(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("ANTHROPIC_API_KEY", None)
            self.assertFalse(self.request("GET", "/link/status")[1]["claude"])
            status, (_, raw) = self.request("POST", "/v1/messages", BODY, raw=True)
        self.assertEqual(status, 503)
        self.assertEqual(json.loads(raw)["error"]["type"], "api_error")


# --- the real SDK against a fake upstream -------------------------------------------------------------

class Upstream:
    """A pretend api.anthropic.com: records requests, answers with canned SSE or an error."""

    def __init__(self) -> None:
        self.requests: list[dict[str, Any]] = []
        self.mode = "ok"
        upstream = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a: Any) -> None:
                pass

            def do_POST(self) -> None:
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                upstream.requests.append({"path": self.path, "headers": dict(self.headers.items()), "body": body})
                if upstream.mode == "400":
                    data = json.dumps({"type": "error", "error": {"type": "invalid_request_error",
                                                                  "message": "max_tokens: too large"}}).encode()
                    self.send_response(400)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Connection", "close")
                self.end_headers()
                events = EVENTS if upstream.mode == "ok" else EVENTS[:2]
                for e in events[:1]:
                    self.wfile.write(expected_sse([e]))
                self.wfile.write(b'event: ping\ndata: {"type": "ping"}\n\n')  # the SDK drops pings
                self.wfile.write(expected_sse(events[1:]))
                if upstream.mode == "midstream":
                    self.wfile.write(b'event: error\ndata: {"type":"error","error":{"type":"overloaded_error",'
                                     b'"message":"Overloaded"}}\n\n')
                self.wfile.flush()
                self.close_connection = True

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, args=(0.05,), daemon=True).start()

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()


@unittest.skipIf(anthropic is None, "anthropic not installed")
class RealSdkTest(LinkCase):
    def setUp(self) -> None:
        self.upstream = Upstream()
        env = mock.patch.dict(os.environ, {"ANTHROPIC_API_KEY": "sk-test-key", "ANTHROPIC_BASE_URL": self.upstream.url})
        env.start()
        self.addCleanup(env.stop)
        self.addCleanup(self.upstream.stop)
        super().setUp()

    def post(self) -> tuple[int, Any]:
        return self.request("POST", "/v1/messages", BODY, headers={"anthropic-beta": BETAS}, raw=True)

    def test_stream_through_the_real_sdk(self) -> None:
        status, (resp, raw) = self.post()
        self.assertEqual(status, 200)
        self.assertEqual(frames(raw), [(e["type"], e) for e in EVENTS])  # the ping is the SDK's to drop
        self.assertTrue(raw.startswith(b'event: message_start\ndata: {"type":"message_start",'))
        sent = self.upstream.requests[0]
        self.assertTrue(sent["path"].startswith("/v1/messages"))
        headers = {k.lower(): v for k, v in sent["headers"].items()}
        self.assertEqual(headers["x-api-key"], "sk-test-key")  # the key is added on the PC
        self.assertEqual({b.strip() for b in headers["anthropic-beta"].split(",")},
                         {"server-side-fallback-2026-07-01", "mid-conversation-tool-changes-2026-07-01"})
        body = sent["body"]
        self.assertEqual(body["fallbacks"], "default")
        self.assertIs(body["stream"], True)
        self.assertEqual(body["thinking"], BODY["thinking"])
        self.assertEqual(body["output_config"], BODY["output_config"])
        self.assertIs(body["tools"][0]["eager_input_streaming"], True)
        self.assertEqual(body["messages"], BODY["messages"])
        self.assertNotIn("betas", body)

    def test_upstream_400_keeps_status_and_body(self) -> None:
        self.upstream.mode = "400"
        status, (_, raw) = self.post()
        self.assertEqual(status, 400)
        self.assertEqual(json.loads(raw)["error"], {"type": "invalid_request_error", "message": "max_tokens: too large"})

    def test_upstream_error_event_mid_stream(self) -> None:
        self.upstream.mode = "midstream"
        status, (_, raw) = self.post()
        self.assertEqual(status, 200)
        got = frames(raw)
        self.assertEqual([e for e, _ in got], ["message_start", "content_block_start", "error"])
        self.assertEqual(got[-1][1]["error"]["type"], "overloaded_error")


if __name__ == "__main__":
    unittest.main()
