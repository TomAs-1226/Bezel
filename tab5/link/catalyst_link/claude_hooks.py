"""Claude Code's hooks for the Link (hook.py): the block to add, and adding or removing it for you.

`catalyst-link hook-settings` prints the block; `hook-install` (and `hook-install --remove`, and the
desktop app's button) merge it into Claude Code's user settings, `~/.claude/settings.json` (or `$CLAUDE_CONFIG_DIR/
settings.json`; `$CATALYST_LINK_CLAUDE_SETTINGS` names another file, which the tests use).

The merge is careful with a file that isn't ours:
- only hook entries whose command runs the Link's hook are added or removed; every other hook, and
  every other setting, is left exactly as it was;
- a file that isn't valid JSON is refused rather than overwritten;
- the file as it was is copied to `settings.json.catalyst-link.bak` before each write.
"""
from __future__ import annotations

import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

from .state import LinkError, write_atomic

HOOK_EVENTS = ("UserPromptSubmit", "PreToolUse", "PostToolUse", "Notification", "Stop", "SubagentStop",
               "SessionStart", "SessionEnd")
# How a command is recognised as the Link's hook, however it was written
MARKERS = ("catalyst_link/hook.py", "catalyst_link\\hook.py", "catalyst_link.hook", "catalyst-link hook")


def hook_settings(command: str) -> dict[str, Any]:
    """The "hooks" block for Claude Code's settings.json that runs `command` on every event the Link reads."""
    entry = {"type": "command", "command": command, "timeout": 5}
    hooks: dict[str, Any] = {}
    for ev in HOOK_EVENTS:
        block: dict[str, Any] = {"hooks": [dict(entry)]}
        if ev in ("PreToolUse", "PostToolUse"):
            block = {"matcher": "*", **block}
        hooks[ev] = [block]
    return {"hooks": hooks}


DEFAULT_PORT = 8765


def hook_command(port: int | None = None) -> str:
    """This Python running hook.py by its path: works whether or not the package is installed. A Link on
    another port than the default is named with `--url`, since the hook can't know it otherwise."""
    from . import hook

    py = Path(sys.executable).resolve().as_posix()
    cmd = f'"{py}" "{Path(hook.__file__).resolve().as_posix()}"'
    if port and port != DEFAULT_PORT:
        cmd += f" --url http://127.0.0.1:{int(port)}"
    return cmd


def settings_path() -> Path:
    env = os.environ.get("CATALYST_LINK_CLAUDE_SETTINGS")
    if env:
        return Path(env).expanduser()
    base = os.environ.get("CLAUDE_CONFIG_DIR")
    return (Path(base).expanduser() if base else Path.home() / ".claude") / "settings.json"


def _is_ours(entry: Any) -> bool:
    return isinstance(entry, dict) and any(m in str(entry.get("command", "")) for m in MARKERS)


def _read(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        text = path.read_text(encoding="utf-8-sig")
        data = json.loads(text) if text.strip() else {}
    except (OSError, ValueError) as exc:
        raise LinkError(409, f"{path} isn't valid JSON, so it was left alone: {exc}") from None
    if not isinstance(data, dict):
        raise LinkError(409, f"{path} isn't a JSON object, so it was left alone")
    return data


def _strip(hooks: dict[str, Any]) -> dict[str, Any]:
    """`hooks` without the Link's entries; blocks and events left empty are dropped."""
    out: dict[str, Any] = {}
    for ev, blocks in hooks.items():
        if not isinstance(blocks, list):
            out[ev] = blocks
            continue
        kept = []
        for block in blocks:
            if isinstance(block, dict) and isinstance(block.get("hooks"), list):
                entries = [e for e in block["hooks"] if not _is_ours(e)]
                if not entries:
                    continue
                block = {**block, "hooks": entries}
            kept.append(block)
        if kept:
            out[ev] = kept
    return out


def status(path: Path | None = None, port: int | None = None) -> dict[str, Any]:
    path = path or settings_path()
    events: list[str] = []
    commands: set[str] = set()
    error = None
    try:
        hooks = _read(path).get("hooks") or {}
    except LinkError as exc:
        hooks, error = {}, exc.message
    if isinstance(hooks, dict):
        for ev in HOOK_EVENTS:
            blocks = hooks.get(ev)
            ours = [e for b in (blocks if isinstance(blocks, list) else []) if isinstance(b, dict)
                    for e in (b.get("hooks") or []) if _is_ours(e)]
            if ours:
                events.append(ev)
                commands.update(str(e.get("command", "")) for e in ours)
    installed = len(events) == len(HOOK_EVENTS)
    expected = hook_command(port)
    return {"ok": True, "path": str(path), "exists": path.exists(), "installed": installed,
            "partial": bool(events) and not installed, "events": events, "error": error,
            # installed, but for another Python, another checkout or another port: install again to fix
            "outdated": bool(commands) and commands != {expected},
            "command": expected}


def _write(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        shutil.copyfile(path, path.with_name(path.name + ".catalyst-link.bak"))
    write_atomic(path, json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def install(command: str | None = None, path: Path | None = None, port: int | None = None) -> dict[str, Any]:
    """Add the Link's hooks (replacing an older copy of them), leaving everything else as it was."""
    path = path or settings_path()
    data = _read(path)
    hooks = data.get("hooks") or {}
    if not isinstance(hooks, dict):
        raise LinkError(409, f"\"hooks\" in {path} isn't an object, so it was left alone")
    hooks = _strip(hooks)
    for ev, blocks in hook_settings(command or hook_command(port))["hooks"].items():
        hooks[ev] = list(hooks.get(ev) or []) + blocks
    data["hooks"] = hooks
    _write(path, data)
    return status(path, port)


def uninstall(path: Path | None = None, port: int | None = None) -> dict[str, Any]:
    """Remove the Link's hooks and nothing else."""
    path = path or settings_path()
    if not path.exists():
        return status(path, port)
    data = _read(path)
    hooks = data.get("hooks")
    if not isinstance(hooks, dict):
        return status(path, port)
    stripped = _strip(hooks)
    if stripped == hooks:
        return status(path, port)
    if stripped:
        data["hooks"] = stripped
    else:
        data.pop("hooks", None)
    _write(path, data)
    return status(path, port)
