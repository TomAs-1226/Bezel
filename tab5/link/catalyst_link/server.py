"""The HTTP server: every endpoint in tab5/docs/link-api.md, on the stdlib ThreadingHTTPServer."""
from __future__ import annotations

import hmac
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import threading
import time
import traceback
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable
from urllib.parse import parse_qs, unquote, urlsplit

from . import __version__, claude_hooks, codeview
from .devices import Devices, iso_from
from .files import Files
from .inbox import Inbox
from .media import Media
from .pairing import Pairing, console_notify, toast_notify
from .claude_code import ClaudeCodeBackend
from .proxy import ClaudeProxy, api_error
from .repo import Patches, repo_root, repo_status
from .sessions import SessionTracker
from .state import LinkError, State

MAX_JSON = 1024 * 1024            # patches, work orders, status changes
MAX_MESSAGES = 32 * 1024 * 1024   # a Messages API request can carry images
LOOPBACK = ("127.0.0.1", "::1", "::ffff:127.0.0.1")
MAIN = "main"                     # identify(): the Link's main token, as opposed to a paired tablet's


@dataclass
class Config:
    repo: Path
    port: int = 8765
    bind: str = "0.0.0.0"
    check: str | None = None          # from the PC's CLI only, never from a request
    check_timeout: float = 300.0
    on_work_order: str | None = None  # likewise
    name: str = field(default_factory=socket.gethostname)
    # How /v1/messages reaches Claude: "api" (ANTHROPIC_API_KEY, proxy.py), "claude-code" (the owner's
    # Claude subscription through Claude Code, claude_code.py), "off", or "auto": api when a key is set,
    # else claude-code.
    claude: str = "auto"
    claude_model: str | None = None   # claude-code only; None → Claude Code's own default
    claude_cli: str | None = None     # claude-code only; None → found (see claude_code.find_cli)
    media: bool = True                # the media remote (media.py): the PC's now-playing for the tablet
    pair: bool = True                 # pairing by a code shown on the PC (pairing.py); off: type the token
    pair_toast: bool = False          # also show the code as a Windows notification (the CLI turns it on)


def pick_backend(cfg: Config, claude_client: Any | None) -> str:
    if cfg.claude != "auto":
        return cfg.claude
    return "api" if claude_client is not None or os.environ.get("ANTHROPIC_API_KEY") else "claude-code"


class LinkApp:
    def __init__(self, cfg: Config, state: State, claude_client: Any | None = None,
                 agent_factory: Any | None = None, media_platform: Any | None = None,
                 pair_notify: list[Any] | None = None) -> None:
        self.cfg = cfg
        self.state = state.ensure()
        self.repo = repo_root(cfg.repo)
        self.patches = Patches(self.repo, self.state, cfg.check, cfg.check_timeout)
        self.inbox = Inbox(self.state)
        self.files = Files(self.state.files_dir)
        # Claude Code on this PC, as its hooks report it (hook.py → POST /v1/claude/events)
        self.claude_sessions = SessionTracker(self.state.home / "claude-turns.jsonl")
        self.claude_backend = pick_backend(cfg, claude_client)
        # the PC's media session for the tablet's home mode; never fatal (media.py says why when it's absent)
        self.media = Media(media_platform, enabled=cfg.media)
        # the tablet pairs by a code shown here; the console always shows it, a notification when asked
        if pair_notify is None:
            pair_notify = [console_notify] + ([toast_notify] if cfg.pair_toast else [])
        # each tablet that pairs gets a token of its own (devices.py), so it can be forgotten alone
        self.devices = Devices(self.state.devices_path)
        self.pairing = Pairing(self.state.token, pair_notify, enabled=cfg.pair, issue=self.devices.add)
        # set by `serve` once it has tried: {"state": "advertising"|"off"|"failed", ...}
        self.mdns: dict[str, Any] = {"state": "off", "why": "not advertised by this process"}
        self.started_at = time.time()
        # clients using the main token from another machine (typed by hand): ip -> last seen
        self.by_hand: dict[str, float] = {}
        self.proxy: ClaudeProxy | ClaudeCodeBackend | None
        if self.claude_backend == "api":
            self.proxy = ClaudeProxy(self.state, claude_client)
        elif self.claude_backend == "claude-code":
            self.proxy = ClaudeCodeBackend(self.state, cfg.claude_model, cfg.claude_cli, agent_factory)
        else:
            self.proxy = None

    @property
    def claude_available(self) -> bool:
        return self.proxy is not None and self.proxy.available

    def check_token(self, given: str | None) -> bool:
        return self.identify(given) is not None

    def identify(self, given: str | None) -> str | None:
        """MAIN for the Link's own token, a paired tablet's id for its token, None for anything else."""
        if given is None:
            return None
        expected = self.state.token()
        if hmac.compare_digest(given.strip().encode(), expected.encode()):
            return MAIN
        return self.devices.match(given)

    def seen(self, who: str, ip: str) -> None:
        if who == MAIN:
            if ip not in LOOPBACK:
                self.by_hand[ip] = time.time()
        else:
            self.devices.touch(who, ip)

    # --- what the desktop app shows (the /admin routes: this PC only, main token only) ---------------

    def overview(self) -> dict[str, Any]:
        from . import mdns

        counts = self.inbox.counts()
        sessions = self.claude_sessions.listing()["sessions"]
        by_state: dict[str, int] = {}
        for sess in sessions:
            by_state[sess["state"]] = by_state.get(sess["state"], 0) + 1
        pending = self.pairing.info(with_code=False)
        proxy = self.proxy
        return {
            "ok": True, "name": self.cfg.name, "version": __version__, "port": self.cfg.port, "bind": self.cfg.bind,
            "addresses": mdns.local_addresses(self.cfg.bind), "started_at": iso_from(self.started_at),
            "repo": str(self.repo), **{f"repo_{k}": v for k, v in repo_status(self.repo).items()},
            "state_home": str(self.state.home),
            "pairing": {"enabled": self.cfg.pair, "toast": self.cfg.pair_toast, "pending": pending},
            "devices": self.devices.list(),
            "by_hand": [{"ip": ip, "last_seen": iso_from(t)} for ip, t in
                        sorted(self.by_hand.items(), key=lambda kv: kv[1], reverse=True)],
            "media": {"enabled": self.cfg.media, "available": self.media.available, "reason": self.media.reason},
            "mdns": self.mdns,
            "claude": {"via": self.claude_backend if proxy is not None else None, "available": self.claude_available},
            "inbox": counts, "sessions": by_state,
        }

    def pairing_panel(self) -> dict[str, Any]:
        last = self.pairing.last
        return {"ok": True, "enabled": self.cfg.pair, "pending": self.pairing.info(with_code=True),
                "last": ({**last, "at": iso_from(last["at"])} if last else None)}

    def status(self, authed: bool) -> dict[str, Any]:
        base: dict[str, Any] = {"ok": True, "name": self.cfg.name, "version": __version__, "auth": authed,
                                "pairing": self.cfg.pair}
        if not authed:
            return base
        counts = self.inbox.counts()
        proposed = sum(1 for p in self.patches.list() if p["status"] == "proposed")
        return {**base, **repo_status(self.repo), "claude": self.claude_available,
                "claude_via": self.claude_backend if self.proxy is not None else None,
                "inbox_open": counts["open"] + counts["claimed"], "patches": proposed, "files": self.files.count(),
                "media": self.media.available}

    def run_hook(self, wid: str, path: Path) -> None:
        """--on-work-order: the PC's own command, with the new work order's path, detached."""
        template = self.cfg.on_work_order
        if not template:
            return

        def quote(s: str) -> str:
            return shlex.quote(s) if os.name == "posix" else subprocess.list2cmdline([s])

        # The id is Link-made ([a-z0-9-]) and the path is under the Link's home; quoted all the same.
        command = template.replace("{path}", quote(str(path))).replace("{id}", quote(wid))
        kwargs: dict[str, Any] = {}
        if os.name == "posix":
            kwargs["start_new_session"] = True
        else:
            kwargs["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP  # type: ignore[attr-defined]
        log = open(self.state.hooks_log_path, "ab")
        log.write(f"\n=== {wid}: {command}\n".encode("utf-8"))
        log.flush()
        proc = subprocess.Popen(command, shell=True, cwd=self.repo, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT, **kwargs)
        log.close()
        self.state.log({"kind": "hook", "id": wid, "command": command, "pid": proc.pid})

        def reap() -> None:
            code = proc.wait()
            self.state.log({"kind": "hook", "id": wid, "exit": code})

        threading.Thread(target=reap, name=f"hook-{wid}", daemon=True).start()


class Handler(BaseHTTPRequestHandler):
    server_version = f"CatalystLink/{__version__}"
    protocol_version = "HTTP/1.1"
    timeout = 120  # per socket read: a stalled upload doesn't hold a thread forever
    app: LinkApp
    quiet = False
    _audit: dict[str, Any]
    _audited = False

    # --- plumbing ----------------------------------------------------------------------------------

    def log_message(self, format: str, *args: Any) -> None:
        if self.quiet or self._routine():
            return
        sys.stderr.write(f"[link] {self.client_address[0]} {format % args}\n")

    def _routine(self) -> bool:
        """Traffic not worth a log line each: Claude Code's hooks post on every tool call and the tablet
        polls the sessions; the desktop app on this PC polls what it shows (its /admin routes, and
        anything it asks with its own User-Agent)."""
        path = getattr(self, "path", "") or ""
        if path.startswith(("/v1/claude/", "/admin/")):
            return True
        headers = getattr(self, "headers", None)
        agent = (headers.get("User-Agent") or "") if headers is not None else ""
        return agent.startswith("CatalystLinkDesktop/") and self.client_address[0] in LOOPBACK

    def _json(self, status: int, obj: dict[str, Any]) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if self.close_connection:
            self.send_header("Connection", "close")
        self.end_headers()
        self._audit["status"] = status
        self._write_audit()  # on disk before the tablet hears the outcome
        self.wfile.write(body)

    def _bytes(self, status: int, body: bytes, ctype: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self._audit["status"] = status
        self.wfile.write(body)

    def _write_audit(self) -> None:
        """Every write request (and every refused token) goes to log.jsonl once, with its outcome."""
        a = self._audit
        if self._audited:
            return
        # traffic, not writes: Claude, Claude Code's hooks, and the media remote's play/pause
        routine = a["path"] == "/v1/messages" or a["path"].startswith(("/v1/claude/", "/media/"))
        if (a["method"] == "POST" and not routine) or a.get("error") == "token":
            self._audited = True
            self.app.state.log(a)

    def _length(self, limit: int) -> int:
        raw = self.headers.get("Content-Length")
        if raw is None:
            raise LinkError(411, "Content-Length is required")
        try:
            length = int(raw)
        except ValueError:
            raise LinkError(400, "bad Content-Length") from None
        if length < 0:
            raise LinkError(400, "bad Content-Length")
        if length > limit:
            raise LinkError(413, f"body larger than {limit // (1024 * 1024)} MB")
        return length

    def _body(self, limit: int = MAX_JSON) -> dict[str, Any]:
        raw = self.rfile.read(self._length(limit))
        try:
            obj = json.loads(raw.decode("utf-8")) if raw else {}
        except (UnicodeDecodeError, ValueError):
            raise LinkError(400, "body is not JSON") from None
        if not isinstance(obj, dict):
            raise LinkError(400, "body must be a JSON object")
        return obj

    # --- the proxy's Responder ------------------------------------------------------------------------

    def send_json(self, status: int, obj: dict[str, Any]) -> None:
        self._json(status, obj)

    def start_stream(self) -> None:
        self.close_connection = True  # no Content-Length: the stream ends when we close
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.flush()
        self._audit["status"] = 200

    def write(self, data: bytes) -> None:
        self.wfile.write(data)
        self.wfile.flush()

    # --- dispatch ------------------------------------------------------------------------------------

    def do_GET(self) -> None:
        self._handle("GET")

    def do_POST(self) -> None:
        self._handle("POST")

    def _handle(self, method: str) -> None:
        url = urlsplit(self.path)
        path = unquote(url.path).rstrip("/") or "/"
        query = {k: v[-1] for k, v in parse_qs(url.query, keep_blank_values=True).items()}
        self._audit = {"kind": "request", "method": method, "path": path, "ip": self.client_address[0]}
        self._audited = False
        who = self.app.identify(self.headers.get("X-Link-Token"))
        authed = who is not None
        if authed:
            self.app.seen(who, self.client_address[0])
        try:
            if path == "/admin" or path.startswith("/admin/"):
                return self._admin(method, path, who)
            if path == "/link/status" and method == "GET":
                return self._json(200, self.app.status(authed))
            if path in self.PAIR_ROUTES:  # the way to get a token: no token needed
                if method != "POST":
                    raise LinkError(405, "method not allowed")
                return self._pair(path)
            route = self._route(method, path)
            if route is None:
                if self._known(path):
                    raise LinkError(405, "method not allowed")
                raise LinkError(404, "not found")
            if not authed:
                self._audit["error"] = "token"
                self.close_connection = True  # the body stays unread
                return self._json(401, {"ok": False, "error": "token"})
            route(query)
        except LinkError as exc:
            if method == "POST":
                self.close_connection = True  # the body may be unread
            self._audit.update(error=exc.message)
            self._json(exc.status, exc.payload())
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            self._audit["error"] = "client disconnected"
        except Exception as exc:  # a bug: answer, and keep the traceback on the PC
            traceback.print_exc()
            self.close_connection = True
            self._audit["error"] = f"internal: {exc}"
            try:
                self._json(500, {"ok": False, "error": f"internal error: {exc}"})
            except OSError:
                pass
        finally:
            self._write_audit()  # a request that ended without an answer (the tablet went away)

    PAIR_ROUTES = ("/link/pair", "/link/pair/confirm")

    # --- the desktop app's routes -------------------------------------------------------------------------
    # This PC only (a loopback client), the main token only (not a tablet's), and never from a web page
    # (a browser always sends Origin; the desktop app's requests come from its Rust side, which doesn't).

    ADMIN_ROUTES: list[tuple[str, str, str]] = [
        ("GET", r"/admin/overview", "adm_overview"),
        ("GET", r"/admin/pairing", "adm_pairing"),
        ("POST", r"/admin/pairing/cancel", "adm_pairing_cancel"),
        ("GET", r"/admin/devices", "adm_devices"),
        ("POST", r"/admin/devices/(?P<id>[^/]+)/forget", "adm_forget"),
        ("POST", r"/admin/inbox/(?P<id>[^/]+)/status", "adm_inbox_status"),
        ("GET", r"/admin/claude-hooks", "adm_hooks"),
        ("POST", r"/admin/claude-hooks", "adm_hooks_set"),
    ]

    def _admin(self, method: str, path: str, who: str | None) -> None:
        if self.client_address[0] not in LOOPBACK or self.headers.get("Origin") is not None:
            self.close_connection = True
            self._audit["error"] = "admin: not this PC"
            return self._json(403, {"ok": False, "error": "the desktop app's routes answer only on this PC"})
        if who != MAIN:
            self.close_connection = True
            self._audit["error"] = "token"
            return self._json(401, {"ok": False, "error": "token"})
        for m, pattern, name in self.ADMIN_ROUTES:
            match = re.fullmatch(pattern, path)
            if match and m == method:
                self._params = match.groupdict()
                return getattr(self, name)()
        if any(re.fullmatch(pattern, path) for _, pattern, _ in self.ADMIN_ROUTES):
            raise LinkError(405, "method not allowed")
        raise LinkError(404, "not found")

    def adm_overview(self) -> None:
        self._json(200, self.app.overview())

    def adm_pairing(self) -> None:
        self._json(200, self.app.pairing_panel())

    def adm_pairing_cancel(self) -> None:
        self._body()
        self._json(200, {"ok": True, "cancelled": self.app.pairing.cancel()})

    def adm_devices(self) -> None:
        self._json(200, {"ok": True, "devices": self.app.devices.list()})

    def adm_forget(self) -> None:
        self._body()
        dev_id = self._params["id"]
        if not self.app.devices.forget(dev_id):
            raise LinkError(404, f"no paired tablet {dev_id}")
        self._audit.update(forgot=dev_id)
        self._json(200, {"ok": True, "forgot": dev_id})

    def adm_inbox_status(self) -> None:
        body = self._body()
        status = str(body.get("status"))
        # the PC may also put a claimed item back ("open"), as the CLI's `release` does
        result = self.app.inbox.set_status(self._params["id"], status, body.get("note") or "")
        self._audit.update(id=result["id"], status_to=status, via="desktop")
        self._json(200, result)

    def adm_hooks(self) -> None:
        self._json(200, claude_hooks.status(port=self.app.cfg.port))

    def adm_hooks_set(self) -> None:
        body = self._body()
        install = body.get("install")
        if not isinstance(install, bool):
            raise LinkError(400, "install is true or false")
        port = self.app.cfg.port
        result = claude_hooks.install(port=port) if install else claude_hooks.uninstall(port=port)
        self._audit.update(claude_hooks="installed" if install else "removed")
        self._json(200, result)

    def _pair(self, path: str) -> None:
        body = self._body(4096)
        if path == "/link/pair":
            result = self.app.pairing.start(body, self.client_address[0], self.app.cfg.name)
            self._audit.update(pairing="started", device=str(body.get("device") or "")[:48])
        else:
            result = self.app.pairing.confirm(body, self.app.cfg.name)
            self._audit["pairing"] = "paired"  # the token is in the answer, never in the log
        self._json(200, result)

    ROUTES: list[tuple[str, str, str]] = [
        ("GET", r"/code/tree", "code_tree"),
        ("GET", r"/code/read", "code_read"),
        ("GET", r"/code/search", "code_search"),
        ("GET", r"/code/patches", "code_patches"),
        ("POST", r"/code/patch", "code_patch"),
        ("GET", r"/inbox", "inbox_list"),
        ("POST", r"/inbox", "inbox_create"),
        ("GET", r"/inbox/(?P<id>[^/]+)", "inbox_get"),
        ("POST", r"/inbox/(?P<id>[^/]+)/status", "inbox_status"),
        ("GET", r"/files", "files_list"),
        ("POST", r"/files", "files_upload"),
        ("POST", r"/v1/messages", "messages"),
        ("GET", r"/v1/claude/sessions", "cc_sessions"),
        ("POST", r"/v1/claude/events", "cc_event"),
        ("POST", r"/v1/claude/sessions/ack", "cc_ack_all"),
        ("POST", r"/v1/claude/sessions/(?P<id>[^/]+)/ack", "cc_ack"),
        ("GET", r"/media/now", "media_now"),
        ("GET", r"/media/art", "media_art"),
        ("POST", r"/media/control", "media_control"),
    ]

    def _known(self, path: str) -> str | None:
        return next((name for _, pattern, name in self.ROUTES if re.fullmatch(pattern, path)), None)

    def _route(self, method: str, path: str) -> Callable[[dict[str, str]], None] | None:
        for m, pattern, name in self.ROUTES:
            match = re.fullmatch(pattern, path)
            if m == method and match:
                self._params = match.groupdict()
                return getattr(self, name)
        return None

    # --- code ----------------------------------------------------------------------------------------

    def code_tree(self, q: dict[str, str]) -> None:
        self._json(200, codeview.tree(self.app.repo, q.get("path"), q.get("depth")))

    def code_read(self, q: dict[str, str]) -> None:
        self._json(200, codeview.read(self.app.repo, q.get("path"), q.get("start"), q.get("end")))

    def code_search(self, q: dict[str, str]) -> None:
        self._json(200, codeview.search(self.app.repo, q.get("q"), q.get("path"), q.get("max"), q.get("case")))

    def code_patches(self, q: dict[str, str]) -> None:
        self._json(200, {"ok": True, "patches": self.app.patches.list()})

    def code_patch(self, q: dict[str, str]) -> None:
        result = self.app.patches.create(self._body())
        self._audit.update(id=result["id"], branch=result["branch"], files=result["files"],
                           check=Patches.check_word(result["check"]), duplicate=result.get("duplicate", False))
        self._json(200, result)

    # --- inbox ---------------------------------------------------------------------------------------

    def inbox_list(self, q: dict[str, str]) -> None:
        self._json(200, {"ok": True, "items": self.app.inbox.list(q.get("status"))})

    def inbox_create(self, q: dict[str, str]) -> None:
        result, created = self.app.inbox.create(self._body())
        self._audit.update(id=result["id"], duplicate=not created)
        self._json(200, result)
        if created:
            try:
                self.app.run_hook(result["id"], self.app.state.home / result["path"])
            except OSError as exc:
                self.app.state.log({"kind": "hook", "id": result["id"], "error": str(exc)})

    def inbox_get(self, q: dict[str, str]) -> None:
        self._json(200, {"ok": True, "item": self.app.inbox.get(self._params["id"])})

    def inbox_status(self, q: dict[str, str]) -> None:
        body = self._body()
        status = body.get("status")
        # Over the wire the tablet may claim, finish or reject; putting an item back is the CLI's.
        result = self.app.inbox.set_status(self._params["id"], str(status), body.get("note") or "",
                                           allowed=("claimed", "done", "rejected"))
        self._audit.update(id=result["id"], status_to=status)
        self._json(200, {"ok": True})

    # --- files ---------------------------------------------------------------------------------------

    def files_list(self, q: dict[str, str]) -> None:
        self._json(200, {"ok": True, "files": self.app.files.list()})

    def files_upload(self, q: dict[str, str]) -> None:
        length = self._length(64 * 1024 * 1024)
        result = self.app.files.save(q.get("name"), self.rfile, length)
        self._audit.update(name=result["name"], bytes=result["bytes"], duplicate=result.get("duplicate", False))
        self._json(200, result)

    # --- Claude ------------------------------------------------------------------------------------------

    def messages(self, q: dict[str, str]) -> None:
        try:
            body = self._body(MAX_MESSAGES)
        except LinkError as exc:  # this route answers in the Messages API's error format
            self.close_connection = True
            etype = "request_too_large" if exc.status == 413 else "invalid_request_error"
            return self._json(exc.status, api_error(etype, exc.message))
        betas = [b.strip() for h in self.headers.get_all("anthropic-beta") or [] for b in h.split(",") if b.strip()]
        if self.app.proxy is None:
            self.close_connection = True
            return self._json(503, api_error("api_error", "Claude is turned off on this Catalyst Link (--claude off)"))
        self.app.proxy.serve(body, betas, self)


    # --- Claude Code on this PC ---------------------------------------------------------------------------

    def cc_sessions(self, q: dict[str, str]) -> None:
        self._json(200, self.app.claude_sessions.listing())

    def cc_event(self, q: dict[str, str]) -> None:
        self._json(200, self.app.claude_sessions.event(self._body()))

    def cc_ack_all(self, q: dict[str, str]) -> None:
        self._body()
        self._json(200, {"ok": True, "acked": self.app.claude_sessions.ack(None)})

    def cc_ack(self, q: dict[str, str]) -> None:
        self._body()
        self._json(200, {"ok": True, "acked": self.app.claude_sessions.ack(self._params["id"])})

    # --- the media remote (media.py) ----------------------------------------------------------------------

    def media_now(self, q: dict[str, str]) -> None:
        self._json(200, self.app.media.now())

    def media_art(self, q: dict[str, str]) -> None:
        if q.get("encoding") == "base64":  # for a client that can only read JSON bodies
            return self._json(200, self.app.media.art_json(q))
        body, ctype, _ = self.app.media.art(q)
        self._bytes(200, body, ctype)

    def media_control(self, q: dict[str, str]) -> None:
        body = self._body()
        self._audit["action"] = body.get("action")
        self._json(200, self.app.media.control(body))


class LinkServer(ThreadingHTTPServer):
    daemon_threads = True
    if os.name == "nt":
        # On Windows SO_REUSEADDR (http.server's default) lets a second Link bind the same port while the
        # first still listens, and the two then split the tablet's requests between them. Exclusive use
        # makes the second one fail to start instead, as it would on Linux or macOS.
        allow_reuse_address = False

        def server_bind(self) -> None:
            self.socket.setsockopt(socket.SOL_SOCKET, getattr(socket, "SO_EXCLUSIVEADDRUSE", -5), 1)
            super().server_bind()


def make_server(app: LinkApp, quiet: bool = False) -> ThreadingHTTPServer:
    handler = type("LinkHandler", (Handler,), {"app": app, "quiet": quiet})
    return LinkServer((app.cfg.bind, app.cfg.port), handler)
