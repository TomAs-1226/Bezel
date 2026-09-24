"""catalyst-link: serve the tablet, and work the inbox from the PC (humans and the PC's agent)."""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any

from . import __version__
from .inbox import Inbox
from .repo import Patches, repo_root, repo_status
from .state import LinkError, State


def _state() -> State:
    return State().ensure()


def _print_json(obj: Any) -> None:
    print(json.dumps(obj, indent=2, ensure_ascii=False))


def _table(rows: list[dict[str, Any]], cols: list[str]) -> None:
    if not rows:
        print("(none)")
        return
    widths = {c: max(len(c), *(len(str(r.get(c, ""))) for r in rows)) for c in cols}
    widths[cols[-1]] = 0  # the last column runs free
    print("  ".join(c.upper().ljust(widths[c]) for c in cols).rstrip())
    for r in rows:
        print("  ".join(str(r.get(c, "")).ljust(widths[c]) for c in cols).rstrip())


# --- serve --------------------------------------------------------------------------------------------

def cmd_serve(args: argparse.Namespace) -> int:
    from . import mdns
    from .server import Config, LinkApp, make_server

    cfg = Config(repo=Path(args.repo).expanduser(), port=args.port, bind=args.bind, check=args.check,
                 check_timeout=args.check_timeout, on_work_order=args.on_work_order,
                 claude=args.claude, claude_model=args.claude_model, claude_cli=args.claude_cli,
                 media=not args.no_media)
    if args.name:
        cfg.name = args.name
    state = _state()
    app = LinkApp(cfg, state)
    server = make_server(app, quiet=args.quiet)
    port = server.server_address[1]
    st = repo_status(app.repo)
    addrs = mdns.local_addresses(cfg.bind) or ["127.0.0.1"]
    print(f"Catalyst Link {__version__} — {cfg.name}")
    print(f"  repo     {app.repo}  (branch {st['branch']}{', dirty' if st['dirty'] else ''}; never written to)")
    for a in addrs:
        print(f"  url      http://{a}:{port}")
    print(f"  token    {state.token()}   (type this into the tablet's settings once)")
    print(f"  claude   {_claude_line(app)}")
    media_line = "on (the PC's now-playing, for the tablet's home mode)" if app.media.available else app.media.reason
    print(f"  media    {media_line}")
    print(f"  check    {cfg.check or 'off'}" + (f"  (timeout {cfg.check_timeout:g} s)" if cfg.check else ""))
    print(f"  on-work-order  {cfg.on_work_order or 'off'}")
    print(f"  state    {state.home}")
    stop_mdns = None
    if not args.no_mdns:
        try:
            stop_mdns = mdns.advertise(cfg.name, port, cfg.bind)
        except Exception as exc:  # mDNS is a convenience; never fatal
            print(f"  mdns     failed: {exc}")
        print(f"  mdns     {'_catalyst-link._tcp' if stop_mdns else 'off (pip install zeroconf)'}")
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if stop_mdns:
            stop_mdns()
    return 0


def _claude_line(app: Any) -> str:
    from .claude_code import ClaudeCodeBackend

    proxy = app.proxy
    if proxy is None:
        return "off (--claude off)"
    if isinstance(proxy, ClaudeCodeBackend):
        auth = proxy.auth() if proxy.installed else None
        if not proxy.installed or not (auth or {}).get("loggedIn"):
            return f"claude-code, NOT READY: {proxy.why_not()}"
        plan = (auth or {}).get("subscriptionType") or (auth or {}).get("authMethod") or "logged in"
        return f"claude-code via your Claude login ({plan}); model {proxy.model or 'Claude Code default'}; cli {proxy.cli}"
    return "api (ANTHROPIC_API_KEY)" if proxy.available else "api, NOT READY: no ANTHROPIC_API_KEY or no anthropic SDK"


# --- Claude Code: checking it, and storing a setup-token -------------------------------------------

def cmd_claude_check(args: argparse.Namespace) -> int:
    """Where the claude-code backend stands, without a model call (or with one, --live)."""
    from .claude_code import TOKEN_ENV, TOKEN_FILE, ClaudeCodeBackend, oauth_token, sdk

    state = _state()
    backend = ClaudeCodeBackend(state, args.claude_model, args.claude_cli)
    print(f"agent sdk  {'claude-agent-sdk ' + getattr(sdk, '__version__', '?') if sdk else 'NOT INSTALLED (pip install claude-agent-sdk)'}")
    print(f"cli        {backend.cli or 'NOT FOUND (pass --claude-cli PATH)'}")
    tok = oauth_token(state.home)
    if not tok:
        source = "none: Claude Code's own login (claude login)"
    else:
        source = f"${TOKEN_ENV}" if os.environ.get(TOKEN_ENV, "").strip() else str(state.home / TOKEN_FILE)
    print(f"token      {source}")
    if not backend.installed:
        return 1
    auth = backend.auth() or {}
    print(f"login      {json.dumps(auth)}")
    if not auth.get("loggedIn"):
        print()
        print("Not logged in. On this PC run once:  claude setup-token   (then: catalyst-link claude-token)")
        print("or:  claude login   (for the same Windows/macOS/Linux user the Link runs as)")
        return 1
    if args.live:
        from .claude_code import live_check
        ok, text = live_check(backend)
        print(f"live       {'ok' if ok else 'FAILED'}: {text}")
        return 0 if ok else 1
    return 0


def cmd_claude_token(args: argparse.Namespace) -> int:
    """Store the token `claude setup-token` printed, read from stdin (so it stays out of shell history)."""
    from .claude_code import TOKEN_FILE

    state = _state()
    path = state.home / TOKEN_FILE
    if args.remove:
        path.unlink(missing_ok=True)
        print(f"removed {path}")
        return 0
    if sys.stdin.isatty():
        import getpass
        tok = getpass.getpass("Paste the token from `claude setup-token` (input hidden): ").strip()
    else:
        tok = sys.stdin.read().strip()
    if not tok or any(c.isspace() for c in tok):
        print("catalyst-link: that doesn't look like a token", file=sys.stderr)
        return 1
    path.write_text(tok + "\n", encoding="utf-8")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    print(f"saved to {path} (readable only by you); restart the Link to use it")
    return 0


# --- inbox --------------------------------------------------------------------------------------------

def cmd_inbox(args: argparse.Namespace) -> int:
    items = Inbox(_state()).list(args.status)
    if args.json:
        _print_json(items)
    else:
        _table(items, ["id", "status", "priority", "kind", "when", "title"])
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    state = _state()
    item = Inbox(state).get(args.id)
    if args.json:
        _print_json(item)
        return 0
    print(Path(item["path"]).read_text(encoding="utf-8"), end="")
    print(f"\n---\nfile: {item['path']}")
    if item.get("patch"):
        rec_path = state.patches_dir / f"{item['patch']}.json"
        if rec_path.exists():
            rec = json.loads(rec_path.read_text(encoding="utf-8"))
            print(f"patch: {rec['branch']} (from {rec['base'][:10]}); review with:")
            print(f"  git log {rec['base'][:10]}..{rec['branch']}")
            print(f"  git diff {rec['base'][:10]}...{rec['branch']}")
            print(f"worktree: {rec.get('worktree')}  (check: {Patches.check_word(rec.get('check', {}))})")
    return 0


def _set(args: argparse.Namespace, status: str) -> int:
    result = Inbox(_state()).set_status(args.id, status, args.note or "")
    print(f"{result['id']}: {status}")
    return 0


def cmd_claim(args: argparse.Namespace) -> int:
    return _set(args, "claimed")


def cmd_done(args: argparse.Namespace) -> int:
    return _set(args, "done")


def cmd_reject(args: argparse.Namespace) -> int:
    return _set(args, "rejected")


def cmd_release(args: argparse.Namespace) -> int:
    return _set(args, "open")


# --- patches and token ----------------------------------------------------------------------------------

def cmd_patches(args: argparse.Namespace) -> int:
    state = _state()
    repo = repo_root(Path(args.repo).expanduser()) if args.repo else Path.cwd()
    patches = Patches(repo, state)
    rows = []
    for rec in patches.records():
        rows.append({"id": rec["id"], "status": patches.status_of(rec), "check": Patches.check_word(rec.get("check", {})),
                     "when": rec.get("when", ""), "branch": rec["branch"], "title": rec["title"],
                     "worktree": rec.get("worktree"), "diff": str(state.patches_dir / f"{rec['id']}.diff")})
    if args.json:
        _print_json(rows)
    else:
        _table(rows, ["id", "status", "check", "when", "branch", "title"])
    return 0


def cmd_token(args: argparse.Namespace) -> int:
    state = _state()
    if args.rotate:
        print(state.rotate_token())
        print("rotated: re-enter it on the tablet; a running Link picks it up on the next request", file=sys.stderr)
    else:
        print(state.token())
    return 0


# --- Claude Code's hooks ---------------------------------------------------------------------------------

HOOK_EVENTS = ("UserPromptSubmit", "PreToolUse", "PostToolUse", "Notification", "Stop", "SubagentStop",
               "SessionStart", "SessionEnd")


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


def hook_command() -> str:
    """This Python running hook.py by its path: works whether or not the package is installed."""
    from . import hook

    py = Path(sys.executable).resolve().as_posix()
    return f'"{py}" "{Path(hook.__file__).resolve().as_posix()}"'


def cmd_hook(args: argparse.Namespace) -> int:
    from . import hook

    return hook.main()


def cmd_hook_settings(args: argparse.Namespace) -> int:
    print(json.dumps(hook_settings(args.command or hook_command()), indent=2))
    return 0


def cmd_claude_sessions(args: argparse.Namespace) -> int:
    """What the running Link knows about Claude Code's sessions (asks it over HTTP, with the token)."""
    import urllib.request

    state = _state()
    req = urllib.request.Request(f"{args.url.rstrip('/')}/v1/claude/sessions", headers={"X-Link-Token": state.token()})
    with urllib.request.urlopen(req, timeout=5) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    if args.json:
        _print_json(data)
        return 0
    rows = []
    for s in data.get("sessions", []):
        eta = s.get("eta")
        rows.append({"state": s["state"], "project": s["project"], "elapsed": f"{s['elapsed_s']:.0f}s",
                     "eta": f"~{eta['remaining_s']:.0f}s" if eta else "-", "step": s["step"], "title": s["title"]})
    _table(rows, ["state", "project", "elapsed", "eta", "step", "title"])
    print(f"({data.get('turns_recorded', 0)} finished turns recorded for estimates)")
    return 0


# --- parser ---------------------------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="catalyst-link", description="Catalyst Link, the PC side of Catalyst Tab.")
    p.add_argument("--version", action="version", version=f"catalyst-link {__version__}")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("serve", help="serve the tablet")
    s.add_argument("--repo", required=True, help="the robot project's git repo")
    s.add_argument("--port", type=int, default=8765)
    s.add_argument("--bind", default="0.0.0.0")
    s.add_argument("--name", help="the name the tablet shows (default: this PC's hostname)")
    s.add_argument("--check", help='compile check run in each patch worktree, e.g. "./gradlew compileJava"')
    s.add_argument("--check-timeout", type=float, default=300.0, metavar="SECONDS")
    s.add_argument("--on-work-order", metavar="COMMAND",
                   help="run this (detached) for each new work order; {path} and {id} are substituted")
    s.add_argument("--claude", choices=("auto", "api", "claude-code", "off"), default="auto",
                   help="how the tablet reaches Claude: api (ANTHROPIC_API_KEY), claude-code (your Claude "
                        "subscription through Claude Code on this PC), off; auto: api when a key is set, else claude-code")
    s.add_argument("--claude-model", help="claude-code: the model (default: Claude Code's own default)")
    s.add_argument("--claude-cli", metavar="PATH", help="claude-code: the Claude Code CLI to run (default: found)")
    s.add_argument("--no-mdns", action="store_true", help="don't advertise over mDNS")
    s.add_argument("--no-media", action="store_true",
                   help="no media remote (the tablet's home mode then can't see or control what the PC plays)")
    s.add_argument("--quiet", action="store_true", help="no per-request log lines")
    s.set_defaults(fn=cmd_serve)

    s = sub.add_parser("inbox", help="list work orders")
    s.add_argument("--status", default="open,claimed", help="open, claimed, done, rejected, a comma list, or all")
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_inbox)

    s = sub.add_parser("show", help="print a work order")
    s.add_argument("id", help="a work-order id (or a unique prefix)")
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_show)

    for name, fn, need_note, text in (("claim", cmd_claim, False, "claim a work order"),
                                      ("done", cmd_done, True, "mark a work order done"),
                                      ("reject", cmd_reject, True, "reject a work order"),
                                      ("release", cmd_release, False, "put a claimed work order back")):
        s = sub.add_parser(name, help=text)
        s.add_argument("id")
        s.add_argument("--note", required=need_note, help="what changed and on which branch, or why not")
        s.set_defaults(fn=fn)

    s = sub.add_parser("patches", help="list patch branches proposed from the tablet")
    s.add_argument("--repo", help="the robot repo (default: the one each patch was made in)")
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_patches)

    s = sub.add_parser("claude-check", help="is the claude-code backend ready? (no model call unless --live)")
    s.add_argument("--claude-model")
    s.add_argument("--claude-cli", metavar="PATH")
    s.add_argument("--live", action="store_true", help="also run one tiny turn through Claude Code")
    s.set_defaults(fn=cmd_claude_check)

    s = sub.add_parser("claude-token", help="store a `claude setup-token` token for the claude-code backend (read from stdin)")
    s.add_argument("--remove", action="store_true", help="delete the stored token")
    s.set_defaults(fn=cmd_claude_token)

    s = sub.add_parser("hook", help="Claude Code hook: send the event on stdin to the Link (prints nothing)")
    s.set_defaults(fn=cmd_hook)

    s = sub.add_parser("hook-settings", help="print the hooks block for Claude Code's settings.json")
    s.add_argument("--command", help='the hook command (default: this Python running hook.py by its path)')
    s.set_defaults(fn=cmd_hook_settings)

    s = sub.add_parser("claude-sessions", help="what the running Link knows about Claude Code's sessions")
    s.add_argument("--url", default="http://127.0.0.1:8765")
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_claude_sessions)

    s = sub.add_parser("token", help="print the pairing token")
    s.add_argument("--rotate", action="store_true", help="make a new one (the tablet must be re-paired)")
    s.set_defaults(fn=cmd_token)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.fn(args) or 0)
    except LinkError as exc:
        print(f"catalyst-link: {exc.message}", file=sys.stderr)
        return 1
