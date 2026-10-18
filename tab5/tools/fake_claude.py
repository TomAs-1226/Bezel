#!/usr/bin/env python3
"""A pretend Claude Messages API (and, optionally, a pretend Catalyst Link) for testing the tablet's assistant.

    python tools/fake_claude.py [--port 8787] [--link-port 8765 [--link-delay S]]

POST /v1/messages checks every request the way the tablet must send it — headers, the streaming body
(adaptive thinking with summaries, effort, server-side fallbacks, prompt-caching marks, eager input
streaming on every tool, no sampling knobs, no prefill), tool_use / tool_result pairing with all of a
turn's results in one message, thinking blocks echoed with the signatures this server issued, and the
echo rule after a mid-output fallback — and answers 400 with the reason if anything is off. Otherwise it
streams canned SSE, chosen by a keyword in the technician's latest question:

    (default)       robot_overview and get_alerts in parallel, then an answer built from their results
    "shooter"       set_tunable (a confirmation), then revert_snapshot on the snapshot it reports
    "refuse"        a refusal after partial output and a half-streamed tool call
    "invalid"       a tool call whose streamed input isn't valid JSON, then a good one
    "fallback"      a mid-output server-side fallback block, then a tool call from the serving model
    "all tools"     every read tool in one turn, then select_auto
    "busy"          529 overloaded the first time, then the default turn
    "split"         the default turn, written a few bytes at a time (events split across TCP writes)
    "work order"    create_work_order (with "clip": attaching clips/pit-clip.h264 from the card)
    "patch"         propose_patch (with "stale": an edit whose old text isn't in the file)

Every response is chunked (Transfer-Encoding), one chunk per write. With --link-port it also serves a
small Catalyst Link (docs/link-api.md) with token "test-token": status, inbox, files, patches, code reads,
and /v1/messages forwarded to the fake above. Standard library only.
"""
import argparse
import json
import re
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SIGNATURES = {}          # signature -> thinking text issued
PRE_FALLBACK = set()     # signatures and tool ids from before a fallback boundary: must not come back
BUSY_SEEN = set()        # conversations already given a 529
LOCK = threading.Lock()
LINK_TOKEN = "test-token"
DUMP = []                # [directory, count] with --dump


def log(*a):
    print(*a, flush=True)


class Bad(Exception):
    pass


# ---------------------------------------------------------------- validation

def validate_headers(h, via_link):
    if via_link:
        if h.get("X-Link-Token") != LINK_TOKEN:
            raise Bad("X-Link-Token missing or wrong")
    elif not h.get("x-api-key"):
        raise Bad("x-api-key header missing")
    if not via_link and h.get("anthropic-version") != "2023-06-01":
        raise Bad("anthropic-version must be 2023-06-01")
    if not (h.get("content-type") or "").startswith("application/json"):
        raise Bad("content-type must be application/json")
    betas = [b.strip() for b in (h.get("anthropic-beta") or "").split(",")]
    if "server-side-fallback-2026-07-01" not in betas:
        raise Bad("anthropic-beta must include server-side-fallback-2026-07-01 (fallbacks: \"default\")")


def blocks_of(msg):
    c = msg.get("content")
    if isinstance(c, str):
        return [{"type": "text", "text": c}]
    if not isinstance(c, list) or not c:
        raise Bad("message content must be a non-empty list")
    return c


def validate_body(b):
    for k in ("temperature", "top_p", "top_k"):
        if k in b:
            raise Bad(f"{k} is not supported on this model")
    if not isinstance(b.get("model"), str) or not b["model"]:
        raise Bad("model missing")
    mt = b.get("max_tokens")
    if not isinstance(mt, int) or not 1 <= mt <= 128000:
        raise Bad("max_tokens must be an integer in 1..128000")
    if b.get("stream") is not True:
        raise Bad("stream must be true")
    if b.get("thinking") != {"type": "adaptive", "display": "summarized"}:
        raise Bad("thinking must be {\"type\": \"adaptive\", \"display\": \"summarized\"}")
    if (b.get("output_config") or {}).get("effort") not in ("low", "medium", "high", "xhigh", "max"):
        raise Bad("output_config.effort missing or unknown")
    if b.get("fallbacks") != "default":
        raise Bad("fallbacks must be \"default\"")
    breakpoints = 0
    system = b.get("system")
    if not isinstance(system, list) or not system:
        raise Bad("system must be a list of text blocks")
    if (system[-1].get("cache_control") or {}).get("type") != "ephemeral":
        raise Bad("the system block should carry cache_control")
    breakpoints += sum(1 for s in system if "cache_control" in s)
    tools = b.get("tools")
    if not isinstance(tools, list) or not tools:
        raise Bad("tools missing")
    names = set()
    for t in tools:
        if not t.get("name") or not t.get("description") or (t.get("input_schema") or {}).get("type") != "object":
            raise Bad(f"tool {t.get('name')!r} needs name, description and an object input_schema")
        if t.get("eager_input_streaming") is not True:
            raise Bad(f"tool {t['name']} lacks eager_input_streaming: true")
        names.add(t["name"])
    msgs = b.get("messages")
    if not isinstance(msgs, list) or not msgs:
        raise Bad("messages missing")
    if msgs[0].get("role") != "user":
        raise Bad("the first message must be the user's")
    if msgs[-1].get("role") != "user":
        raise Bad("the last message must be the user's (no assistant prefill on this model)")
    last_blocks = blocks_of(msgs[-1])
    if (last_blocks[-1].get("cache_control") or {}).get("type") != "ephemeral":
        raise Bad("the last block of the last message should carry cache_control")
    for i, m in enumerate(msgs):
        role = m.get("role")
        if role not in ("user", "assistant"):
            raise Bad(f"messages[{i}].role {role!r}")
        if i and role == "assistant" and msgs[i - 1].get("role") == "assistant":
            raise Bad(f"messages[{i}]: two assistant messages in a row")
        bl = blocks_of(m)
        breakpoints += sum(1 for x in bl if "cache_control" in x)
        for j, x in enumerate(bl):
            t = x.get("type")
            if t == "text" and not x.get("text"):
                raise Bad(f"messages[{i}].content[{j}]: text blocks must be non-empty")
            if t in ("thinking", "redacted_thinking") and "cache_control" in x:
                raise Bad(f"messages[{i}].content[{j}]: thinking blocks can't carry cache_control")
            if t == "thinking":
                sig = x.get("signature")
                if role != "assistant" or sig not in SIGNATURES or SIGNATURES[sig] != x.get("thinking"):
                    raise Bad(f"messages[{i}].content[{j}]: thinking block not echoed unchanged (signature {sig!r})")
                if sig in PRE_FALLBACK:
                    raise Bad(f"messages[{i}].content[{j}]: thinking from before a fallback boundary was echoed")
            if t == "fallback":
                pass  # an ignored audit marker; keeping it is allowed
            if t == "tool_use":
                if x.get("id") in PRE_FALLBACK:
                    raise Bad(f"messages[{i}].content[{j}]: a tool_use from before a fallback boundary was echoed")
                if not isinstance(x.get("input"), dict):
                    raise Bad(f"messages[{i}].content[{j}]: tool_use input must be an object")
                if x.get("name") not in names:
                    raise Bad(f"messages[{i}].content[{j}]: tool_use names an undeclared tool")
        if role == "assistant":
            uses = [x["id"] for x in bl if x.get("type") == "tool_use"]
            if uses:
                if i + 1 >= len(msgs) or msgs[i + 1].get("role") != "user":
                    raise Bad(f"messages[{i}]: tool_use without a following user message")
                nxt = blocks_of(msgs[i + 1])
                results = [x.get("tool_use_id") for x in nxt if x.get("type") == "tool_result"]
                if sorted(results) != sorted(uses):
                    raise Bad(f"messages[{i + 1}] must answer every tool_use of messages[{i}] in one message "
                              f"(want {uses}, got {results})")
                lead = [x.get("type") for x in nxt[:len(results)]]
                if any(t != "tool_result" for t in lead):
                    raise Bad(f"messages[{i + 1}]: tool_result blocks must come first")
        else:
            for x in bl:
                if x.get("type") == "tool_result":
                    prev = msgs[i - 1] if i else None
                    ids = [y.get("id") for y in blocks_of(prev)] if prev and prev.get("role") == "assistant" else []
                    if x.get("tool_use_id") not in ids:
                        raise Bad(f"messages[{i}]: tool_result for an unknown tool_use {x.get('tool_use_id')!r}")
                    if not isinstance(x.get("content"), (str, list)):
                        raise Bad(f"messages[{i}]: tool_result content must be a string or blocks")
                    if isinstance(x.get("content"), str) and len(x["content"]) > 8300:
                        raise Bad(f"messages[{i}]: a tool_result of {len(x['content'])} characters (keep them under ~8 KB)")
    if breakpoints > 4:
        raise Bad(f"{breakpoints} cache_control breakpoints (at most 4)")


# ---------------------------------------------------------------- the conversation

def exchange(b):
    """(the technician's latest words, the assistant messages since, the last user message)"""
    msgs = b["messages"]
    q_at = 0
    for i, m in enumerate(msgs):
        if m["role"] == "user" and any(x.get("type") == "text" and not x["text"].startswith("[")
                                       for x in blocks_of(m)):
            q_at = i
    words = " ".join(x["text"] for x in blocks_of(msgs[q_at]) if x.get("type") == "text" and not x["text"].startswith("["))
    step = sum(1 for m in msgs[q_at:] if m["role"] == "assistant")
    return words, step, msgs[-1]


def results(msg):
    out = {}
    for x in blocks_of(msg):
        if x.get("type") == "tool_result":
            c = x.get("content")
            if isinstance(c, list):
                c = "".join(y.get("text", "") for y in c)
            try:
                data = json.loads(c)
            except (TypeError, ValueError):
                data = c
            out[x["tool_use_id"]] = (data, bool(x.get("is_error")))
    return out


def scenario_of(words):
    w = words.lower()
    for key, name in (("refuse", "refusal"), ("invalid", "invalid"), ("fallback", "fallback"), ("busy", "busy"),
                      ("split", "split"), ("all tools", "tools"), ("patch", "patch"), ("work order", "workorder"), ("shooter", "tunable")):
        if key in w:
            return name
    return "overview"


class Stream:
    """Builds a message's SSE events."""

    def __init__(self, model):
        self.events = []
        self.index = 0
        self.ev("message_start", {"type": "message_start", "message": {
            "id": "msg_" + uuid.uuid4().hex[:20], "type": "message", "role": "assistant", "model": model,
            "content": [], "stop_reason": None, "stop_sequence": None,
            "usage": {"input_tokens": 2100, "output_tokens": 1, "cache_read_input_tokens": 1800,
                      "cache_creation_input_tokens": 0}}})

    def ev(self, name, data):
        self.events.append((name, data))

    def thinking(self, text, pre_fallback=False):
        sig = "sig-" + uuid.uuid4().hex
        with LOCK:
            SIGNATURES[sig] = text
            if pre_fallback:
                PRE_FALLBACK.add(sig)
        i = self.index
        self.ev("content_block_start", {"type": "content_block_start", "index": i,
                                        "content_block": {"type": "thinking", "thinking": "", "signature": ""}})
        half = len(text) // 2
        for part in (text[:half], text[half:]):
            self.ev("content_block_delta", {"type": "content_block_delta", "index": i,
                                            "delta": {"type": "thinking_delta", "thinking": part}})
        self.ev("content_block_delta", {"type": "content_block_delta", "index": i,
                                        "delta": {"type": "signature_delta", "signature": sig}})
        self.ev("content_block_stop", {"type": "content_block_stop", "index": i})
        self.index += 1

    def text(self, text):
        i = self.index
        self.ev("content_block_start", {"type": "content_block_start", "index": i,
                                        "content_block": {"type": "text", "text": ""}})
        for part in re.findall(r".{1,24}", text, re.S):
            self.ev("content_block_delta", {"type": "content_block_delta", "index": i,
                                            "delta": {"type": "text_delta", "text": part}})
        self.ev("ping", {"type": "ping"})
        self.ev("content_block_stop", {"type": "content_block_stop", "index": i})
        self.index += 1

    def tool(self, name, fragments, pre_fallback=False, stop=True):
        tid = "toolu_" + uuid.uuid4().hex[:22]
        if pre_fallback:
            with LOCK:
                PRE_FALLBACK.add(tid)
        i = self.index
        self.ev("content_block_start", {"type": "content_block_start", "index": i, "content_block": {
            "type": "tool_use", "id": tid, "name": name, "input": {}}})
        for f in fragments:
            self.ev("content_block_delta", {"type": "content_block_delta", "index": i,
                                            "delta": {"type": "input_json_delta", "partial_json": f}})
        if stop:
            self.ev("content_block_stop", {"type": "content_block_stop", "index": i})
        self.index += 1
        return tid

    def fallback(self, frm, to):
        i = self.index
        self.ev("content_block_start", {"type": "content_block_start", "index": i, "content_block": {
            "type": "fallback", "from": {"model": frm}, "to": {"model": to}}})
        self.ev("content_block_stop", {"type": "content_block_stop", "index": i})
        self.index += 1

    def end(self, stop_reason, details=None, iterations=None):
        delta = {"stop_reason": stop_reason, "stop_sequence": None}
        if details:
            delta["stop_details"] = details
        usage = {"output_tokens": 180}
        if iterations:
            usage["iterations"] = iterations
        self.ev("message_delta", {"type": "message_delta", "delta": delta, "usage": usage})
        self.ev("message_stop", {"type": "message_stop"})

    def sse(self, crlf=False):
        nl = "\r\n" if crlf else "\n"
        out = ": fake_claude" + nl
        for name, data in self.events:
            out += f"event: {name}{nl}data: {json.dumps(data)}{nl}{nl}"
        return out.encode()


def split_json(obj, pieces):
    s = json.dumps(obj)
    n = max(1, len(s) // pieces)
    return [s[i:i + n] for i in range(0, len(s), n)]


def overview_turn(step, last, model):
    s = Stream(model)
    if step == 0:
        s.thinking("A status check: the overview and the alerts together will show battery, mode and faults.")
        s.text("Checking the robot.")
        s.tool("robot_overview", ["", "{", "}"])
        s.tool("get_alerts", ["{}"])
        s.end("tool_use")
        return s
    res = results(last)
    over = next((d for d, e in res.values() if isinstance(d, dict) and "power" in d), {})
    alerts = next((d for d, e in res.values() if isinstance(d, dict) and isinstance(d.get("alerts"), list)), {})
    pw = over.get("power", {})
    mode = over.get("mode", {}).get("mode", "unknown")
    worst = (alerts.get("alerts") or [{}])[0]
    s.thinking("Battery and mode from the overview; the worst alert names the fault.")
    s.text(f"Battery {pw.get('battery_v')} V ({pw.get('band')}), robot {mode}, {alerts.get('errors', '?')} error(s). "
           f"Worst: {worst.get('source', '')} {worst.get('text', 'none')}. Check that motor's CAN wiring and run "
           f"the Find motor op mode in DS Utility.")
    s.end("end_turn")
    return s


READ_TOOLS = [("get_mechanisms", {}), ("get_power", {}), ("get_can", {}), ("get_vision", {}), ("list_tunables", {}),
              ("list_autos", {}), ("list_snapshots", {}), ("list_logs", {}), ("list_topics", {"prefix": "/Catalyst/Shooter/"}),
              ("read_topics", {"names": ["/Catalyst/Shooter/TargetRPS", "/FMSInfo/ControlWord", "/Catalyst/CAN/Devices",
                                         "/Catalyst/Swerve/Pose", "/nope"]}),
              ("run_preflight", {}), ("systemcore_health", {}), ("motor_history", {}), ("code_search", {"query": "kShooter"}),
              ("code_read", {"path": "src/main/java/frc/robot/Constants.java"}), ("list_patches", {}),
              ("list_work_orders", {"status": "all"}), ("summarize_log", {"name": "match-q14.wpilog"}),
              ("summarize_log", {"name": "../etc/passwd"})]


def respond(b, model):
    words, step, last = exchange(b)
    sc = scenario_of(words)
    s = Stream(model)
    if sc in ("overview", "split", "busy"):
        return sc, overview_turn(step, last, model)
    if sc == "tunable":
        if step == 0:
            s.thinking("The shooter target is 62 rps; raising it to 70 needs the technician's approval.")
            s.text("I'll raise the shooter target to 70 rps.")
            s.tool("set_tunable", ['{"key": "/Catalyst/Shooter/TargetRPS"', ', "value": 7', '0, "reason": "spin-up to 62 rps',
                                   ' is too slow for the long shot"}'])
            s.end("tool_use")
        elif step == 1:
            (data, err), = results(last).values()
            if err or not isinstance(data, dict) or "snapshot" not in data:
                s.text(f"Understood, I left it as it was ({data}).")
                s.end("end_turn")
            else:
                s.thinking("The write landed; exercise the revert path with the snapshot it reported.")
                s.text(f"Set: the robot echoes {data.get('robot_echo')}. Putting it back from snapshot #{data['snapshot']}.")
                s.tool("revert_snapshot", split_json({"id": data["snapshot"], "reason": "test: put the shooter back"}, 3))
                s.end("tool_use")
        else:
            (data, err), = results(last).values()
            s.text(f"Revert: {json.dumps(data)}" if not err else f"The revert didn't go through: {data}")
            s.end("end_turn")
        return sc, s
    if sc == "tools":
        if step == 0:
            s.text("Reading everything.")
            for name, inp in READ_TOOLS:
                s.tool(name, split_json(inp, 2))
            s.end("tool_use")
        elif step == 1:
            s.tool("select_auto", split_json({"name": "Taxi", "reason": "test the chooser write"}, 2))
            s.end("tool_use")
        else:
            s.text("done")
            s.end("end_turn")
        return sc, s
    if sc == "refusal":
        s.text("I can help with that. First, ")
        s.tool("set_tunable", ['{"key": "/Catalyst/Shooter/Tar'], stop=False)
        s.end("refusal", {"type": "refusal", "category": "cyber",
                          "explanation": "This request was declined by a safety classifier."})
        return sc, s
    if sc == "invalid":
        if step == 0:
            s.tool("get_power", ['{"verbose": tru', 'e,}'])
            s.end("tool_use")
        elif step == 1:
            (data, err), = results(last).values()
            if not (err and isinstance(data, dict) and "INVALID_JSON" in data):
                raise Bad("expected an is_error tool_result carrying INVALID_JSON for the malformed input")
            s.text("My last call was malformed; trying again.")
            s.tool("get_power", ["{}"])
            s.end("tool_use")
        else:
            (data, err), = results(last).values()
            s.text(f"Battery {data.get('battery_v')} V, {data.get('total_current_a')} A total.")
            s.end("end_turn")
        return sc, s
    if sc == "fallback":
        if step == 0:
            s.thinking("Start with the CAN bus.", pre_fallback=True)
            s.tool("get_can", ["{}"], pre_fallback=True)
            s.text("Looking at the CAN bus first. ")
            s.fallback(model, "claude-opus-4-8")
            s.thinking("Continuing on the fallback model: power is the better first look.")
            s.text("Checking power instead.")
            s.tool("get_power", ["{", "}"])
            s.end("tool_use", iterations=[{"type": "message", "output_tokens": 40},
                                          {"type": "fallback_message", "output_tokens": 140}])
        else:
            res = results(last)
            if len(res) != 1:
                raise Bad(f"expected exactly one tool_result (get_power), got {len(res)}")
            (data, err), = res.values()
            s.text(f"On the fallback model: battery {data.get('battery_v')} V.")
            s.end("end_turn")
        return sc, s
    if sc == "workorder":
        if step == 0:
            s.text("Sending this to the PC's inbox.")
            order = {"title": "Hood motor (can_s2 27) not answering", "kind": "bug", "priority": "high",
                     "body": "Talon FX 27 on can_s2 drops off the bus in the pit. Check wiring and CAN termination; "
                             "confirm the Hood subsystem handles a missing motor."}
            if "clip" in words.lower():
                order["sd_files"] = ["clips/pit-clip.h264"]
            s.tool("create_work_order", split_json(order, 4))
            s.end("tool_use")
        else:
            (data, err), = results(last).values()
            s.text(f"Work order: {json.dumps(data)}")
            s.end("end_turn")
        return sc, s
    if sc == "patch":
        if step == 0:
            s.text("Proposing a patch.")
            s.tool("propose_patch", split_json({
                "title": "Raise shooter target", "summary": "Spin-up is too slow; raise the default target.",
                "edits": [{"path": "src/main/java/frc/robot/Constants.java",
                           "old": "kShooterRPS = 99;" if "stale" in words.lower() else "kShooterRPS = 62;",
                           "new": "kShooterRPS = 70;"}]}, 5))
            s.end("tool_use")
        else:
            (data, err), = results(last).values()
            s.text(f"Patch: {json.dumps(data)}")
            s.end("end_turn")
        return sc, s
    raise Bad("no scenario")


# ---------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    via_link = False

    def log_message(self, fmt, *args):
        pass

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n)

    def send_json(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def chunk(self, data):
        self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
        self.wfile.flush()

    def handle_one_request(self):
        # the tablet hangs up once it has message_stop; the chunked trailer then has nowhere to go
        try:
            super().handle_one_request()
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def messages(self):
        raw = self.body()
        try:
            validate_headers(self.headers, self.via_link)
            b = json.loads(raw)
            validate_body(b)
            words, step, _ = exchange(b)
            sc = scenario_of(words)
            if sc == "busy":
                key = words
                with LOCK:
                    first = key not in BUSY_SEEN
                    BUSY_SEEN.add(key)
                if first:
                    log(f"claude: {sc} step {step} -> 529")
                    return self.send_json(529, {"type": "error", "error": {"type": "overloaded_error",
                                                                          "message": "Overloaded"}})
            sc, s = respond(b, b["model"])
        except Bad as e:
            log(f"claude: 400 {e}")
            return self.send_json(400, {"type": "error", "error": {"type": "invalid_request_error", "message": str(e)}})
        except (ValueError, KeyError, TypeError, IndexError) as e:
            log(f"claude: 400 unreadable request ({e!r})")
            return self.send_json(400, {"type": "error", "error": {"type": "invalid_request_error",
                                                                  "message": f"unreadable request: {e}"}})
        log(f"claude: {sc} step {step} ok ({len(b['messages'])} messages, {len(raw)} bytes"
            f"{', via link' if self.via_link else ''})")
        if DUMP:
            with LOCK:
                DUMP[1] += 1
                n = DUMP[1]
            with open(f"{DUMP[0]}/request-{n:03d}.json", "wb") as f:
                f.write(raw)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        payload = s.sse(crlf=sc == "split")
        if sc == "split":
            # a few bytes per write, so events, lines and CRLFs land in separate TCP segments
            i, k = 0, 0
            while i < len(payload):
                n = 1 + (k * 7919) % 7
                self.chunk(payload[i:i + n])
                i += n
                k += 1
                time.sleep(0.0005)
        else:
            for name, data in s.events:
                self.chunk(f"event: {name}\ndata: {json.dumps(data)}\n\n".encode())
                time.sleep(0.002)
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def do_POST(self):
        if self.path == "/v1/messages":
            return self.messages()
        self.send_json(404, {"error": "not found"})


# ---------------------------------------------------------------- a small Catalyst Link

class Link:
    inbox, patches, files = [], [], []


class LinkHandler(Handler):
    via_link = True

    def authed(self):
        if self.headers.get("X-Link-Token") != LINK_TOKEN:
            self.send_json(401, {"ok": False, "error": "token"})
            return False
        return True

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/link/status":
            if self.headers.get("X-Link-Token") != LINK_TOKEN:
                return self.send_json(200, {"ok": True, "name": "fake-pc", "version": "1.0.0", "auth": False})
            return self.send_json(200, {"ok": True, "name": "fake-pc", "version": "1.0.0", "auth": True,
                                        "repo": "CatalystX1", "branch": "main", "dirty": False, "claude": True,
                                        "inbox_open": len(Link.inbox), "patches": len(Link.patches),
                                        "files": len(Link.files)})
        if not self.authed():
            return
        if path == "/inbox":
            return self.send_json(200, {"ok": True, "items": Link.inbox})
        if path == "/code/patches":
            return self.send_json(200, {"ok": True, "patches": [dict(p, check="none") for p in Link.patches]})
        if path == "/code/tree":
            return self.send_json(200, {"ok": True, "entries": [
                {"path": "src/main/java/frc/robot/Constants.java", "type": "file", "size": 4210}]})
        if path == "/code/read":
            return self.send_json(200, {"ok": True, "path": "src/main/java/frc/robot/Constants.java", "start": 1,
                                        "end": 3, "total": 3, "text": "class Constants {\n  kShooterRPS = 62;\n}\n"})
        if path == "/code/search":
            return self.send_json(200, {"ok": True, "matches": [
                {"path": "src/main/java/frc/robot/Constants.java", "line": 2, "text": "  kShooterRPS = 62;"}]})
        self.send_json(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if self.path == "/v1/messages":
            return self.messages()
        if not self.authed():
            return
        raw = self.body()
        path = self.path.split("?")[0]
        stamp = time.strftime("%Y%m%d-%H%M%S")
        if path == "/inbox":
            b = json.loads(raw)
            wid = f"wo-{stamp}-{len(Link.inbox) + 1}"
            Link.inbox.append({"id": wid, "title": b["title"], "kind": b.get("kind"), "priority": b.get("priority"),
                               "status": "open", "when": time.strftime("%Y-%m-%d %H:%M"), "files": b.get("files")})
            log(f"link: POST /inbox {wid} {b['title']!r} files={b.get('files')} robot={b.get('robot')}")
            return self.send_json(200, {"ok": True, "id": wid, "path": f"inbox/{wid}.md"})
        if path == "/files":
            name = self.path.split("name=", 1)[-1]
            Link.files.append(name)
            log(f"link: POST /files {name} ({len(raw)} bytes)")
            return self.send_json(200, {"ok": True, "name": name, "path": f"files/{name}", "bytes": len(raw)})
        if path == "/code/patch":
            b = json.loads(raw)
            pid = f"p-{stamp}"
            Link.patches.append({"id": pid, "title": b["title"], "branch": f"tab/{stamp}", "status": "proposed",
                                 "when": time.strftime("%Y-%m-%d %H:%M")})
            log(f"link: POST /code/patch {pid} {b['title']!r} ({len(b['edits'])} edits)")
            return self.send_json(200, {"ok": True, "id": pid, "branch": f"tab/{stamp}", "files": [e["path"] for e in b["edits"]],
                                        "diffstat": f"{len(b['edits'])} file changed", "check": {"ran": False}})
        self.send_json(404, {"ok": False, "error": "not found"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--link-port", type=int, default=0)
    ap.add_argument("--link-delay", type=float, default=0,
                    help="start the Link this many seconds late (to watch the tablet's outbox drain)")
    ap.add_argument("--dump", help="write each accepted request body to DIR/request-NNN.json")
    a = ap.parse_args()
    if a.dump:
        DUMP.extend([a.dump, 0])
    if a.link_port:
        def run_link():
            time.sleep(a.link_delay)
            link = ThreadingHTTPServer((a.host, a.link_port), LinkHandler)
            log(f"fake Catalyst Link on http://{a.host}:{a.link_port} (token {LINK_TOKEN})")
            link.serve_forever()
        threading.Thread(target=run_link, daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    log(f"fake Claude Messages API on http://{a.host}:{a.port}/v1/messages")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
