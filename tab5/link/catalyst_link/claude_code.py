"""POST /v1/messages without an API key: Claude Code on the PC, logged in with the owner's Claude plan.

The tablet keeps speaking the Messages API exactly as it does to the key-holding proxy (proxy.py): one
streamed response per request, and when the answer ends in `tool_use` the tablet runs the tools itself
(on-screen confirmation for anything that changes the robot) and sends the whole conversation back with
the `tool_result`s. This backend turns that into one long-lived Claude Code session per conversation:

- The session is driven through the Claude Agent SDK (`claude-agent-sdk`), which runs the official
  Claude Code CLI with the PC's own login (`claude login`, or `CLAUDE_CODE_OAUTH_TOKEN` from
  `claude setup-token`). The Link never sees or sends a credential of its own and never calls an
  Anthropic endpoint itself.
- The tablet's tools become an in-process MCP server (`tab`) in that session, and they are the only
  tools it has: Claude Code's built-ins (shell, files, web) are off, no settings, plugins, hooks or other
  MCP servers are loaded, prompts are delivered verbatim (no `@file` expansion) and it runs in an empty
  folder under the Link's home.
- Claude Code's raw stream events are forwarded as the Messages API SSE the tablet already parses (tool
  names without the `mcp__tab__` prefix). When Claude calls a tool, the MCP handler waits; the tablet's
  response ends with `stop_reason: tool_use`; the tablet's next request carries the results, which
  resolve the waiting handlers, and the same session streams on into that request's response.
- A new question in a conversation the session already holds (recognised by the text of the last answer
  the tablet echoes back) goes to that session as one new user message. Anything the Link doesn't
  recognise (the Link restarted, the tablet trimmed or rolled back its history) starts a fresh session
  that is told the conversation so far as a transcript.

The tablet's `model`, `max_tokens`, `thinking` and `fallbacks` don't apply: the model is Claude Code's
default or `--claude-model`. `output_config.effort` is passed on."""
from __future__ import annotations

import asyncio
import glob
import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, AsyncIterator, Awaitable, Callable, Protocol

from . import __version__
from .proxy import Responder, api_error, sse
from .state import State

try:  # optional: without it this backend reports itself unavailable
    import claude_agent_sdk as sdk
except ImportError:  # pragma: no cover - exercised only where the SDK is missing
    sdk = None  # type: ignore[assignment]

TOOL_SERVER = "tab"
PREFIX = f"mcp__{TOOL_SERVER}__"
TOKEN_ENV = "CLAUDE_CODE_OAUTH_TOKEN"
TOKEN_FILE = "claude-oauth-token"   # in the Link's home, written by the owner (never by the Link)
CLI_ENV = "CATALYST_LINK_CLAUDE_CLI"
EFFORTS = frozenset({"low", "medium", "high", "xhigh", "max"})
# AssistantMessage.error values → the Messages API's error types (the tablet retries overloaded/api_error once)
_ERROR_TYPES = {"authentication_failed": "authentication_error", "billing_error": "billing_error",
                "rate_limit": "rate_limit_error", "invalid_request": "invalid_request_error",
                "server_error": "api_error", "max_output_tokens": "api_error", "unknown": "api_error"}


# --- finding Claude Code and its login ---------------------------------------------------------------

def find_cli(explicit: str | None = None) -> str | None:
    """The Claude Code CLI to run: --claude-cli / $CATALYST_LINK_CLAUDE_CLI, else the one bundled with the
    Agent SDK, else `claude` on PATH, else (Windows) the copy the Claude desktop app keeps."""
    for cand in (explicit, os.environ.get(CLI_ENV)):
        if cand:
            path = shutil.which(cand) or cand
            return str(Path(path).expanduser()) if Path(path).expanduser().is_file() else None
    if sdk is not None:
        bundled = Path(sdk.__file__).parent / "_bundled" / ("claude.exe" if os.name == "nt" else "claude")
        if bundled.is_file():
            return str(bundled)
    for name in ("claude.exe", "claude") if os.name == "nt" else ("claude",):
        hit = shutil.which(name)
        if hit and (os.name != "nt" or hit.lower().endswith(".exe")):
            return hit
    if os.name == "nt" and os.environ.get("APPDATA"):
        found = glob.glob(os.path.join(os.environ["APPDATA"], "Claude", "claude-code", "*", "claude.exe"))

        def version(p: str) -> tuple[int, ...]:
            try:
                return tuple(int(x) for x in Path(p).parent.name.split("."))
            except ValueError:
                return (0,)

        if found:
            return max(found, key=version)
    return None


def oauth_token(home: Path) -> str | None:
    """A long-lived token from `claude setup-token`: $CLAUDE_CODE_OAUTH_TOKEN, else the file
    <link home>/claude-oauth-token. None: Claude Code uses its own login (`claude login`)."""
    tok = os.environ.get(TOKEN_ENV, "").strip()
    if tok:
        return tok
    try:
        tok = (home / TOKEN_FILE).read_text(encoding="utf-8").strip()
    except OSError:
        return None
    return tok or None


def child_env(home: Path, tool_timeout: float) -> dict[str, str]:
    """What the Claude Code process gets on top of the Link's environment."""
    env = {
        # A key in the Link's environment would make Claude Code bill the API instead of the plan.
        "ANTHROPIC_API_KEY": "",
        # The tablet may wait on the technician's confirmation; don't let Claude Code give up first.
        "MCP_TOOL_TIMEOUT": str(int(tool_timeout * 1000) + 30_000),
        "CLAUDE_AGENT_SDK_CLIENT_APP": f"catalyst-link/{__version__}",
    }
    tok = oauth_token(home)
    if tok:
        env[TOKEN_ENV] = tok
    return env


def auth_status(cli: str, env: dict[str, str], timeout: float = 30.0) -> dict[str, Any]:
    """`claude auth status` (no model call): {"loggedIn": bool, "authMethod", "subscriptionType", …} or
    {"loggedIn": False, "error": "…"}. Personal fields (email, org) are dropped."""
    try:
        proc = subprocess.run([cli, "auth", "status"], capture_output=True, text=True, timeout=timeout,
                              env={**os.environ, **env}, stdin=subprocess.DEVNULL)
        data = json.loads(proc.stdout)
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        return {"loggedIn": False, "error": str(exc)}
    if not isinstance(data, dict):
        return {"loggedIn": False, "error": "unexpected output"}
    return {k: data.get(k) for k in ("loggedIn", "authMethod", "apiProvider", "subscriptionType") if k in data}


# --- the agent: one Claude Code session --------------------------------------------------------------

ToolCall = Callable[[str, dict[str, Any]], Awaitable[tuple[Any, bool]]]


@dataclass
class AgentSpec:
    system: str
    tools: list[dict[str, Any]]      # the tablet's definitions: name, description, input_schema
    call_tool: ToolCall              # (name, input) → (tool_result content, is_error)
    model: str | None = None
    effort: str | None = None
    cli: str | None = None
    cwd: Path | None = None
    env: dict[str, str] = field(default_factory=dict)
    max_turns: int = 50


class Agent(Protocol):
    async def start(self) -> None: ...
    def turn(self, content: list[dict[str, Any]]) -> AsyncIterator[tuple[str, Any]]: ...
    async def close(self) -> None: ...


def mcp_content(content: Any) -> list[dict[str, Any]]:
    """A tool_result's content (a string, or text/image blocks) as MCP content."""
    if isinstance(content, str):
        return [{"type": "text", "text": content}]
    out: list[dict[str, Any]] = []
    for block in content if isinstance(content, list) else []:
        if not isinstance(block, dict):
            continue
        if block.get("type") == "text":
            out.append({"type": "text", "text": str(block.get("text", ""))})
        elif block.get("type") == "image" and (block.get("source") or {}).get("type") == "base64":
            src = block["source"]
            out.append({"type": "image", "data": src.get("data", ""), "mimeType": src.get("media_type", "image/png")})
        else:
            out.append({"type": "text", "text": json.dumps(block, ensure_ascii=False)})
    return out or [{"type": "text", "text": ""}]


class SdkAgent:
    """Claude Code through the Agent SDK: the tablet's tools as the only tools, nothing else loaded."""

    def __init__(self, spec: AgentSpec) -> None:
        if sdk is None:
            raise RuntimeError("claude-agent-sdk is not installed")
        self.spec = spec
        self.client: Any = None

    def _options(self) -> Any:
        tools = []
        for t in self.spec.tools:
            name = str(t["name"])

            async def handler(args: dict[str, Any], _name: str = name) -> dict[str, Any]:
                content, is_error = await self.spec.call_tool(_name, args)
                return {"content": mcp_content(content), "is_error": bool(is_error)}

            schema = t.get("input_schema") or {"type": "object", "properties": {}}
            tools.append(sdk.tool(name, str(t.get("description", "")), schema)(handler))
        server = sdk.create_sdk_mcp_server(TOOL_SERVER, version=__version__, tools=tools)
        kwargs: dict[str, Any] = dict(
            system_prompt=self.spec.system,
            mcp_servers={TOOL_SERVER: server},
            strict_mcp_config=True,          # no .mcp.json, user or plugin servers
            tools=[],                        # no built-in tools: no shell, no files, no web
            allowed_tools=[PREFIX + str(t["name"]) for t in self.spec.tools],
            permission_mode="dontAsk",       # anything not listed above is denied, never prompted
            setting_sources=[],              # no settings, hooks, plugins or CLAUDE.md
            include_partial_messages=True,   # raw stream events, forwarded as they come
            verbatim_prompts=True,           # the technician's text is never expanded (@file, /command)
            cwd=str(self.spec.cwd) if self.spec.cwd else None,
            env=self.spec.env,
            max_turns=self.spec.max_turns,
        )
        if self.spec.model:
            kwargs["model"] = self.spec.model
        if self.spec.cli:
            kwargs["cli_path"] = self.spec.cli
        if self.spec.effort:
            kwargs["effort"] = self.spec.effort
        return sdk.ClaudeAgentOptions(**kwargs)

    async def start(self) -> None:
        self.client = sdk.ClaudeSDKClient(self._options())
        await self.client.connect()

    async def turn(self, content: list[dict[str, Any]]) -> AsyncIterator[tuple[str, Any]]:
        async def one() -> AsyncIterator[dict[str, Any]]:
            yield {"type": "user", "message": {"role": "user", "content": content}, "parent_tool_use_id": None}

        await self.client.query(one())
        async for msg in self.client.receive_response():
            if isinstance(msg, sdk.StreamEvent):
                if msg.parent_tool_use_id is None:
                    yield "event", msg.event
            elif isinstance(msg, sdk.AssistantMessage):
                if msg.error and msg.parent_tool_use_id is None:
                    text = " ".join(b.text for b in msg.content if isinstance(b, sdk.TextBlock)).strip()
                    yield "error", (_ERROR_TYPES.get(str(msg.error), "api_error"), text or str(msg.error))
            elif isinstance(msg, sdk.ResultMessage):
                yield "result", {"is_error": msg.is_error, "subtype": msg.subtype, "result": msg.result,
                                 "session_id": msg.session_id, "num_turns": msg.num_turns,
                                 "cost_usd": msg.total_cost_usd, "stop_reason": msg.stop_reason}

    async def close(self) -> None:
        if self.client is not None:
            await self.client.disconnect()


# --- turning the tablet's history into a session ---------------------------------------------------

def blocks(message: dict[str, Any]) -> list[dict[str, Any]]:
    content = message.get("content")
    if isinstance(content, str):
        return [{"type": "text", "text": content}]
    return [b for b in content if isinstance(b, dict)] if isinstance(content, list) else []


def fingerprint(message: dict[str, Any] | None) -> str | None:
    """What identifies an answer when the tablet echoes it back: its text. None when it has none."""
    if not message or message.get("role") != "assistant":
        return None
    text = "\n".join(str(b.get("text", "")) for b in blocks(message) if b.get("type") == "text").strip()
    return hashlib.sha256(text.encode("utf-8")).hexdigest() if text else None


def text_fingerprint(text: str) -> str | None:
    text = text.strip()
    return hashlib.sha256(text.encode("utf-8")).hexdigest() if text else None


def prompt_blocks(message: dict[str, Any]) -> list[dict[str, Any]]:
    """A user message's text and image blocks, as Claude Code takes them (no cache_control)."""
    out = []
    for b in blocks(message):
        if b.get("type") == "text":
            out.append({"type": "text", "text": str(b.get("text", ""))})
        elif b.get("type") == "image":
            out.append({"type": "image", "source": b.get("source")})
    return out


def _result_text(content: Any) -> str:
    if isinstance(content, str):
        return content
    parts = []
    for b in content if isinstance(content, list) else []:
        if isinstance(b, dict):
            parts.append(str(b.get("text", "")) if b.get("type") == "text" else f"[{b.get('type', 'block')}]")
    return "\n".join(parts)


def transcript(messages: list[dict[str, Any]], limit: int = 200_000) -> str:
    """The conversation so far as text, for a session that starts in the middle of it."""
    names: dict[str, str] = {}
    lines: list[str] = []
    for m in messages:
        who = "Technician (with the tablet's context notes)" if m.get("role") == "user" else "You"
        for b in blocks(m):
            kind = b.get("type")
            if kind == "text" and str(b.get("text", "")).strip():
                lines.append(f"{who}: {b['text']}")
            elif kind == "tool_use":
                names[str(b.get("id"))] = str(b.get("name"))
                lines.append(f"You called {b.get('name')} {json.dumps(b.get('input', {}), ensure_ascii=False)}")
            elif kind == "tool_result":
                text = _result_text(b.get("content"))
                if len(text) > 4000:
                    text = text[:4000] + " …(cut)"
                err = " (error)" if b.get("is_error") else ""
                lines.append(f"Result of {names.get(str(b.get('tool_use_id')), 'a tool')}{err}: {text}")
            elif kind == "image":
                lines.append(f"{who}: [an image]")
    body = "\n\n".join(lines)
    if len(body) > limit:
        body = "…(earlier part cut)\n\n" + body[-limit:]
    return ("[Catalyst Link: this conversation began before this session. What was said so far, including "
            "tool calls the tablet already ran (their results are below; don't repeat them unless you need "
            "fresh values):]\n\n" + body + "\n\n[End of the earlier conversation.]")


def tool_results(message: dict[str, Any]) -> dict[str, tuple[Any, bool]]:
    return {str(b.get("tool_use_id")): (b.get("content", ""), bool(b.get("is_error")))
            for b in blocks(message) if b.get("type") == "tool_result"}


def system_text(system: Any) -> str:
    if isinstance(system, str):
        return system
    if isinstance(system, list):
        return "\n\n".join(str(b.get("text", "")) for b in system if isinstance(b, dict) and b.get("type") == "text")
    return ""


def tool_defs(tools: Any) -> list[dict[str, Any]]:
    """The tablet's custom tools (server tools and anything without a schema are not the tablet's to run)."""
    out = []
    for t in tools if isinstance(tools, list) else []:
        if isinstance(t, dict) and isinstance(t.get("name"), str) and isinstance(t.get("input_schema"), dict):
            out.append({"name": t["name"], "description": str(t.get("description", "")), "input_schema": t["input_schema"]})
    return out


# --- sessions ------------------------------------------------------------------------------------------

@dataclass
class _Call:
    id: str
    name: str
    input: Any
    future: asyncio.Future
    claimed: bool = False    # an MCP handler is waiting on it
    delivered: bool = False  # the tablet's result is in


class Session:
    """One conversation's Claude Code session. Runs on the backend's event loop; `out` carries what the
    tablet's current response should stream (("sse", name, data), ("error", payload), ("closed", None))."""

    def __init__(self, backend: "ClaudeCodeBackend", key: str, spec: AgentSpec) -> None:
        self.backend = backend
        self.id = uuid.uuid4().hex[:12]
        self.key = key
        self.spec = spec
        spec.call_tool = self.call_tool
        self.out: queue.Queue[tuple[str, Any, Any]] = queue.Queue()
        self.prompts: asyncio.Queue[list[dict[str, Any]] | None] = asyncio.Queue()
        self.pending: dict[str, _Call] = {}   # tool_use id → call, in the order Claude made them
        self.phase = "starting"               # streaming, tools (waiting on the tablet), idle, closed
        self.fingerprint: str | None = None   # of the last answer, while idle
        self.last_used = time.monotonic()
        self.responding = False               # a tablet response is (about to be) streaming
        self.queued = 0                       # questions handed over for turns that haven't started
        self.task: asyncio.Task | None = None
        self.broken = False
        self._blocks: dict[int, dict[str, Any]] = {}
        self._stop_reason: str | None = None
        self._text: list[str] = []
        self.rec: dict[str, Any] = {}

    # the loop side

    async def main(self, first: list[dict[str, Any]]) -> None:
        agent: Agent | None = None
        try:
            agent = self.backend.agent_factory(self.spec)
            await agent.start()
            content: list[dict[str, Any]] | None = first
            while content is not None and not self.broken:
                with self.backend.lock:
                    self.phase = "streaming"
                async for kind, item in agent.turn(content):
                    self._on(kind, item)
                with self.backend.lock:
                    # the turn ended without a complete answer (unless the open response is for a question
                    # that is still queued for the next turn)
                    if self.responding and not self.queued:
                        self._emit_error("api_error", "Claude Code ended the turn without an answer")
                    self.phase = "idle"
                if self.broken:
                    break
                content = await self.prompts.get()
                with self.backend.lock:
                    self.queued = max(0, self.queued - 1)
        except asyncio.CancelledError:
            pass
        except Exception as exc:
            with self.backend.lock:
                self._emit_error("api_error", f"Claude Code failed: {exc}")
        finally:
            with self.backend.lock:
                self.phase = "closed"
                self.out.put(("closed", None, None))
                for call in self.pending.values():
                    if not call.future.done():
                        call.future.cancel()
                self.pending.clear()
            if agent is not None:
                try:
                    await asyncio.wait_for(agent.close(), 20)
                except BaseException:  # closing is best effort; the SDK kills the process anyway
                    pass
            self.backend.forget(self)

    def _emit_error(self, etype: str, message: str) -> None:
        """Under the lock. Only reaches the tablet while one of its responses is open."""
        self.rec.setdefault("error", {"type": etype, "message": message})
        if self.responding:
            self.out.put(("error", api_error(etype, message), None))
            self.responding = False
        self.broken = True

    def _on(self, kind: str, item: Any) -> None:
        with self.backend.lock:
            if kind == "error":
                etype, message = item
                self._emit_error(etype, message)
            elif kind == "result":
                self.rec.update(claude_session=item.get("session_id"), cost_usd=item.get("cost_usd"))
                if item.get("is_error") and self.responding and not self.queued:
                    self._emit_error("api_error", str(item.get("result") or item.get("subtype") or "Claude Code error"))
            elif kind == "event" and isinstance(item, dict):
                self._event(item)

    def _event(self, e: dict[str, Any]) -> None:
        kind = e.get("type")
        if kind == "message_start":
            self._blocks, self._text, self._stop_reason = {}, [], None
            self.fingerprint = None
            self.phase = "streaming"
            # a call the tablet answered but Claude Code never ran (it refused the input) is done with
            self.pending = {k: c for k, c in self.pending.items() if not c.delivered}
            msg = e.get("message") or {}
            self.rec["model"] = msg.get("model")
            self._usage(msg.get("usage") or {})
        elif kind == "content_block_start":
            block = dict(e.get("content_block") or {})
            if block.get("type") == "tool_use" and str(block.get("name", "")).startswith(PREFIX):
                block["name"] = block["name"][len(PREFIX):]
                e = {**e, "content_block": block}
            self._blocks[int(e.get("index", 0))] = {**block, "_json": ""}
        elif kind == "content_block_delta":
            delta = e.get("delta") or {}
            b = self._blocks.get(int(e.get("index", 0)))
            if b is not None and delta.get("type") == "input_json_delta":
                b["_json"] += str(delta.get("partial_json", ""))
            elif b is not None and delta.get("type") == "text_delta":
                self._text.append(str(delta.get("text", "")))
        elif kind == "content_block_stop":
            b = self._blocks.get(int(e.get("index", 0)))
            if b is not None and b.get("type") == "tool_use":
                try:
                    parsed = json.loads(b["_json"]) if b["_json"].strip() else {}
                except ValueError:
                    parsed = None
                fut = asyncio.get_running_loop().create_future()
                self.pending[str(b.get("id"))] = _Call(str(b.get("id")), str(b.get("name")), parsed, fut)
        elif kind == "message_delta":
            self._stop_reason = (e.get("delta") or {}).get("stop_reason")
            self._usage(e.get("usage") or {})
        if self.responding:
            self.out.put(("sse", str(kind), e))
        if kind == "message_stop":
            self.rec["stop_reason"] = self._stop_reason
            self.responding = False
            if self._stop_reason == "tool_use" and self.waiting():
                self.phase = "tools"
            else:
                self.fingerprint = text_fingerprint("".join(self._text))

    def _usage(self, usage: dict[str, Any]) -> None:
        for key in ("input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"):
            if usage.get(key) is not None:
                self.rec[key] = usage[key]

    async def call_tool(self, name: str, args: dict[str, Any]) -> tuple[Any, bool]:
        """The MCP handler: find the tool_use this call is, and wait for the tablet's result for it."""
        call = None
        deadline = time.monotonic() + 10
        while call is None:
            with self.backend.lock:
                same = [c for c in self.pending.values() if not c.claimed and c.name == name]
                call = next((c for c in same if c.input == args), same[0] if same else None)
                if call is not None:
                    call.claimed = True
            if call is None:
                if time.monotonic() > deadline:
                    return f"Catalyst Link lost track of this {name} call.", True
                await asyncio.sleep(0.05)
        try:
            return await asyncio.wait_for(asyncio.shield(call.future), self.backend.tool_timeout)
        except asyncio.TimeoutError:
            with self.backend.lock:
                self.broken = True  # the tablet went away mid-conversation; this session is done
            return "The tablet never sent this tool's result.", True
        finally:
            with self.backend.lock:
                self.pending.pop(call.id, None)

    # the HTTP side (any thread, under the lock)

    def waiting(self) -> set[str]:
        """The tool_use ids still waiting for the tablet's result."""
        return {tid for tid, c in self.pending.items() if not c.delivered}

    def deliver(self, results: dict[str, tuple[Any, bool]]) -> None:
        for tid, value in results.items():
            call = self.pending.get(tid)
            if call is not None and not call.delivered:
                call.delivered = True
                self.backend.loop.call_soon_threadsafe(_resolve, call.future, value)


def _resolve(fut: asyncio.Future, value: Any) -> None:
    if not fut.done():
        fut.set_result(value)


class ClaudeCodeBackend:
    """The Responder-facing side: same `available` / `serve(body, betas, out)` as ClaudeProxy."""

    name = "claude-code"
    keepalive = 10.0          # seconds of silence before the tablet gets a ping
    tool_timeout = 15 * 60.0  # the tablet has this long to send a tool's result (its confirmations time out in 90 s)
    idle_timeout = 30 * 60.0  # an idle session's Claude Code process is closed after this
    max_sessions = 4

    def __init__(self, state: State, model: str | None = None, cli: str | None = None,
                 agent_factory: Callable[[AgentSpec], Agent] | None = None) -> None:
        self.state = state
        self.model = model
        self.cli = find_cli(cli) if agent_factory is None else cli
        self.agent_factory = agent_factory or SdkAgent
        self.lock = threading.RLock()
        self.sessions: list[Session] = []
        self._loop: asyncio.AbstractEventLoop | None = None
        self._auth: dict[str, Any] | None = None
        self._auth_at = 0.0
        self._auth_busy = False

    # --- availability ----------------------------------------------------------------------------------

    @property
    def installed(self) -> bool:
        return self.agent_factory is not SdkAgent or (sdk is not None and self.cli is not None)

    @property
    def available(self) -> bool:
        """Installed, and not known to be logged out (the login check runs in the background)."""
        if not self.installed:
            return False
        if self.agent_factory is not SdkAgent:
            return True
        auth = self.auth(block=False)
        return auth is None or bool(auth.get("loggedIn"))

    def why_not(self) -> str | None:
        if sdk is None and self.agent_factory is SdkAgent:
            return "claude-agent-sdk is not installed (pip install claude-agent-sdk)"
        if not self.installed:
            return "no Claude Code CLI found (pass --claude-cli)"
        auth = self.auth(block=False)
        if auth is not None and not auth.get("loggedIn"):
            return "Claude Code is not logged in (run `claude setup-token` or `claude login` on this PC)"
        return None

    def auth(self, block: bool = True) -> dict[str, Any] | None:
        """`claude auth status`, cached for 5 minutes; with block=False, None until the first check ends."""
        if self.agent_factory is not SdkAgent or self.cli is None:
            return {"loggedIn": True}
        with self.lock:
            fresh = self._auth is not None and time.monotonic() - self._auth_at < 300
            if fresh or (self._auth_busy and not block):
                return self._auth
            self._auth_busy = True

        def check() -> None:
            result = auth_status(self.cli or "", child_env(self.state.home, self.tool_timeout))
            with self.lock:
                self._auth, self._auth_at, self._auth_busy = result, time.monotonic(), False

        if block:
            check()
        else:
            threading.Thread(target=check, name="claude-auth", daemon=True).start()
        return self._auth

    # --- the event loop ----------------------------------------------------------------------------------

    @property
    def loop(self) -> asyncio.AbstractEventLoop:
        with self.lock:
            if self._loop is None:
                loop = asyncio.new_event_loop()
                threading.Thread(target=loop.run_forever, name="claude-code", daemon=True).start()
                self._loop = loop
            return self._loop

    def forget(self, session: Session) -> None:
        with self.lock:
            if session in self.sessions:
                self.sessions.remove(session)

    def close_session(self, session: Session) -> None:
        with self.lock:
            session.broken = True
            if session in self.sessions:
                self.sessions.remove(session)
        task = session.task
        if task is not None:
            self.loop.call_soon_threadsafe(task.cancel)

    def shutdown(self) -> None:
        for s in list(self.sessions):
            self.close_session(s)

    def _housekeep(self) -> None:
        """Under the lock: close idle sessions past their time, and the oldest beyond max_sessions."""
        now = time.monotonic()
        for s in list(self.sessions):
            if s.phase in ("idle", "tools") and not s.responding and now - s.last_used > (
                    self.idle_timeout if s.phase == "idle" else self.tool_timeout):
                self.close_session(s)
        while len(self.sessions) >= self.max_sessions:
            idle = [s for s in self.sessions if not s.responding] or self.sessions
            self.close_session(min(idle, key=lambda s: s.last_used))

    # --- a request -----------------------------------------------------------------------------------------

    def serve(self, body: dict[str, Any], betas: list[str], out: Responder) -> None:
        started = time.monotonic()
        rec: dict[str, Any] = {"kind": "claude", "backend": self.name, "model_requested": body.get("model"),
                               "ok": False, "disconnected": False}
        try:
            self._serve(body, out, rec)
        finally:
            rec["seconds"] = round(time.monotonic() - started, 2)
            self.state.log(rec)

    def _serve(self, body: dict[str, Any], out: Responder, rec: dict[str, Any]) -> None:
        if not self.available:
            rec["status"] = 503
            out.send_json(503, api_error("api_error", f"Catalyst Link can't reach Claude Code: {self.why_not()}"))
            return
        messages = body.get("messages")
        if not (isinstance(messages, list) and messages and isinstance(messages[-1], dict)
                and messages[-1].get("role") == "user"):
            rec["status"] = 400
            out.send_json(400, api_error("invalid_request_error", "messages must end with a user message"))
            return
        system = system_text(body.get("system"))
        tools = tool_defs(body.get("tools"))
        key = hashlib.sha256(json.dumps([system, tools], sort_keys=True).encode("utf-8")).hexdigest()
        effort = (body.get("output_config") or {}).get("effort") if isinstance(body.get("output_config"), dict) else None
        last = messages[-1]
        results = tool_results(last)

        with self.lock:
            self._housekeep()
            session = self._match(key, messages, results)
            if session is not None:
                session.rec = rec
                session.responding = True
                session.last_used = time.monotonic()
                if results:
                    rec["how"] = "tool_results"
                    session.deliver(results)
                else:
                    rec["how"] = "next_question"
                    session.phase = "streaming"
                    session.queued += 1
                    self.loop.call_soon_threadsafe(session.prompts.put_nowait, prompt_blocks(last))
            else:
                if results or len(messages) > 1:
                    rec["how"] = "replayed"
                    first = [{"type": "text", "text": transcript(messages if results else messages[:-1])}]
                    first += [{"type": "text", "text": "Continue from here."}] if results else prompt_blocks(last)
                else:
                    rec["how"] = "new"
                    first = prompt_blocks(last)
                spec = AgentSpec(system=system, tools=tools, call_tool=_no_call, model=self.model,
                                 effort=effort if effort in EFFORTS else None, cli=self.cli,
                                 cwd=self._cwd(), env=child_env(self.state.home, self.tool_timeout))
                session = Session(self, key, spec)  # (asyncio.Queue binds to the loop on first use)
                session.rec = rec
                session.responding = True
                self.sessions.append(session)
                loop = self.loop
                # Never wait on the loop while holding the lock: the loop's side takes it too.
                loop.call_soon_threadsafe(self._start, session, first)
            rec["session"] = session.id

        rec["status"] = 200
        out.start_stream()
        try:
            while True:
                try:
                    kind, item, data = session.out.get(timeout=self.keepalive)
                except queue.Empty:
                    out.write(sse("ping", {"type": "ping"}))
                    continue
                if kind == "sse":
                    out.write(sse(item, data))
                    if item == "message_stop":
                        rec["ok"] = True
                        break
                elif kind == "error":
                    rec["error"] = item["error"]
                    out.write(sse("error", item))
                    break
                elif kind == "closed":
                    out.write(sse("error", api_error("api_error", "the Claude Code session ended")))
                    break
        except OSError:  # the tablet went away (it stopped the turn): end this session with it
            rec["disconnected"] = True
            self.close_session(session)
        finally:
            with self.lock:
                session.responding = False
                session.last_used = time.monotonic()

    def _match(self, key: str, messages: list[dict[str, Any]], results: dict[str, Any]) -> Session | None:
        for s in self.sessions:
            if s.key != key or s.broken or s.responding:
                continue
            if results:
                waiting = s.waiting()
                if s.phase == "tools" and waiting and waiting <= set(results):
                    return s
            # "streaming" too: the answer is complete (it has a fingerprint) and only Claude Code's result
            # message is still on its way; the question waits in the session's queue for it.
            elif (s.phase in ("idle", "streaming") and s.fingerprint and len(messages) >= 2
                  and s.fingerprint == fingerprint(messages[-2])):
                return s
        return None

    @staticmethod
    def _start(session: Session, first: list[dict[str, Any]]) -> None:
        """On the loop."""
        if not session.broken:
            session.task = asyncio.get_running_loop().create_task(session.main(first))

    def _cwd(self) -> Path:
        """An empty folder: nothing for Claude Code to pick up (no CLAUDE.md, no .mcp.json)."""
        path = self.state.home / "claude-code"
        path.mkdir(parents=True, exist_ok=True)
        return path


async def _no_call(name: str, args: dict[str, Any]) -> tuple[Any, bool]:  # replaced by Session.__init__
    return "no session", True


def live_check(backend: ClaudeCodeBackend, timeout: float = 120.0) -> tuple[bool, str]:
    """One tiny turn through Claude Code with the backend's settings: (ok, the answer or the error)."""
    spec = AgentSpec(system="Answer in one word.", tools=[], call_tool=_no_call, model=backend.model,
                     cli=backend.cli, cwd=backend._cwd(), env=child_env(backend.state.home, backend.tool_timeout),
                     max_turns=1)

    async def run() -> tuple[bool, str]:
        agent = backend.agent_factory(spec)
        await agent.start()
        text: list[str] = []
        error = None
        model = None
        try:
            async for kind, item in agent.turn([{"type": "text", "text": "Reply with exactly: OK"}]):
                if kind == "event" and item.get("type") == "message_start":
                    model = (item.get("message") or {}).get("model")
                elif kind == "event" and (item.get("delta") or {}).get("type") == "text_delta":
                    text.append(item["delta"]["text"])
                elif kind == "error":
                    error = f"{item[0]}: {item[1]}"
                elif kind == "result" and item.get("is_error"):
                    error = error or str(item.get("result") or item.get("subtype"))
        finally:
            await agent.close()
        return (False, error) if error else (True, f"{''.join(text).strip()!r} from {model}")

    try:
        return asyncio.run(asyncio.wait_for(run(), timeout))
    except Exception as exc:
        return False, f"{type(exc).__name__}: {exc}"
