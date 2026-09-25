"""The Claude Code hook: tells Catalyst Link what Claude Code is doing, so the tablet can show it.

Claude Code runs this for each hook event (UserPromptSubmit, PreToolUse, PostToolUse, Notification, Stop,
SubagentStop, SessionStart, SessionEnd) with the event as JSON on stdin. It sends a trimmed copy to the
Link on this PC (POST /v1/claude/events) and exits 0 without printing anything, whatever happens: a
hook's output would reach Claude's context, and a failing hook must never get in Claude's way.

Standard library only, and no imports from the package, so it runs as a module
(`python -m catalyst_link.hook`) or straight from its file (`python path/to/catalyst_link/hook.py`).

Environment:
  CATALYST_LINK_URL           the Link (default http://127.0.0.1:8765; `--url URL` on the command line wins)
  CATALYST_LINK_HOME          the Link's state directory, for its token (default ~/.catalyst-link)
  CATALYST_LINK_HOOK_PROMPTS  0: don't send the prompt's first line (the tablet then shows "turn N")
"""
from __future__ import annotations

import json
import os
import sys
import urllib.request
from pathlib import Path
from typing import Any

TIMEOUT_S = 1.5
PROMPT_CHARS = 160
DETAIL_CHARS = 90
EVENTS = ("SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PostToolUseFailure", "Notification",
          "Stop", "StopFailure", "SubagentStop", "SessionEnd", "PreCompact")


def _short(text: Any, limit: int) -> str:
    s = " ".join(str(text or "").split())
    return s if len(s) <= limit else s[: limit - 3].rstrip() + "..."


def _name(path: Any) -> str:
    p = str(path or "").replace("\\", "/").rstrip("/")
    return p.rsplit("/", 1)[-1] if p else ""


def describe_tool(tool: str, tool_input: Any) -> str:
    """What a tool call is doing, in a few words: "editing Shooter.java", "running ./gradlew build"."""
    inp = tool_input if isinstance(tool_input, dict) else {}
    t = tool or "a tool"
    if t in ("Read", "NotebookRead"):
        return f"reading {_name(inp.get('file_path') or inp.get('notebook_path'))}".strip()
    if t in ("Edit", "MultiEdit", "Write", "NotebookEdit"):
        return f"{'writing' if t == 'Write' else 'editing'} {_name(inp.get('file_path') or inp.get('notebook_path'))}".strip()
    if t in ("Bash", "PowerShell"):
        what = inp.get("description") or inp.get("command")
        return _short(f"running {what}" if what else "running a command", DETAIL_CHARS)
    if t in ("Grep", "Glob"):
        return _short(f"searching for {inp.get('pattern') or ''}".strip(), DETAIL_CHARS)
    if t == "WebFetch":
        url = str(inp.get("url") or "")
        host = url.split("//", 1)[-1].split("/", 1)[0]
        return f"reading {host}" if host else "reading a web page"
    if t == "WebSearch":
        return _short(f"searching the web for {inp.get('query') or ''}".strip(), DETAIL_CHARS)
    if t in ("Task", "Agent"):
        return _short(f"delegating: {inp.get('description') or inp.get('subagent_type') or 'a subagent'}", DETAIL_CHARS)
    if t == "TodoWrite":
        return "updating its plan"
    if t.startswith("mcp__"):
        parts = t.split("__")
        return _short(f"using {parts[-1]} ({parts[1] if len(parts) > 2 else 'mcp'})", DETAIL_CHARS)
    return _short(f"using {t}", DETAIL_CHARS)


def trim(event: dict[str, Any], send_prompts: bool = True) -> dict[str, Any]:
    """The part of a hook event the Link needs. Tool inputs and outputs stay on the PC: only a few words."""
    name = str(event.get("hook_event_name") or "")
    out: dict[str, Any] = {"event": name, "session_id": str(event.get("session_id") or "")[:100],
                           "cwd": str(event.get("cwd") or "")[:300]}
    if name == "UserPromptSubmit" and send_prompts:
        first = next((ln for ln in str(event.get("prompt") or "").splitlines() if ln.strip()), "")
        out["prompt"] = _short(first, PROMPT_CHARS)
    if name in ("PreToolUse", "PostToolUse", "PostToolUseFailure"):
        out["tool"] = str(event.get("tool_name") or "")[:80]
        out["detail"] = describe_tool(out["tool"], event.get("tool_input"))
    if name == "Notification":
        out["message"] = _short(event.get("message"), 200)
        if event.get("notification_type"):
            out["notification_type"] = str(event.get("notification_type"))[:40]
    if name in ("StopFailure", "PostToolUseFailure") or event.get("error"):
        out["error"] = _short(event.get("error") or event.get("message") or "failed", 200)
    if name == "SessionEnd" and event.get("reason"):
        out["reason"] = str(event.get("reason"))[:40]
    if name == "SessionStart" and event.get("source"):
        out["source"] = str(event.get("source"))[:40]
    return out


def _token() -> str | None:
    home = os.environ.get("CATALYST_LINK_HOME")
    path = (Path(home).expanduser() if home else Path.home() / ".catalyst-link") / "token"
    try:
        return path.read_text(encoding="utf-8").strip() or None
    except OSError:
        return None


def send(payload: dict[str, Any], url: str | None = None, token: str | None = None) -> bool:
    base = (url or os.environ.get("CATALYST_LINK_URL") or "http://127.0.0.1:8765").rstrip("/")
    tok = token or _token()
    if not tok:
        return False
    req = urllib.request.Request(f"{base}/v1/claude/events", data=json.dumps(payload).encode("utf-8"), method="POST",
                                 headers={"Content-Type": "application/json", "X-Link-Token": tok})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as resp:
            return 200 <= resp.status < 300
    except Exception:
        return False


def _arg_url(argv: list[str]) -> str | None:
    """`--url URL` on the hook's command line: a Link on another port (the desktop app writes it)."""
    for i, a in enumerate(argv):
        if a == "--url" and i + 1 < len(argv):
            return argv[i + 1]
        if a.startswith("--url="):
            return a[len("--url="):]
    return None


def main() -> int:
    try:
        raw = sys.stdin.buffer.read()
        event = json.loads(raw.decode("utf-8", "replace")) if raw else {}
        if isinstance(event, dict) and event.get("session_id"):
            send(trim(event, os.environ.get("CATALYST_LINK_HOOK_PROMPTS", "1") != "0"), url=_arg_url(sys.argv[1:]))
    except Exception:
        pass  # never in Claude's way
    return 0


if __name__ == "__main__":
    sys.exit(main())
