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
import traceback
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable
from urllib.parse import parse_qs, unquote, urlsplit

from . import __version__, codeview
from .files import Files
from .inbox import Inbox
from .proxy import ClaudeProxy, api_error
from .repo import Patches, repo_root, repo_status
from .state import LinkError, State

MAX_JSON = 1024 * 1024            # patches, work orders, status changes
MAX_MESSAGES = 32 * 1024 * 1024   # a Messages API request can carry images


@dataclass
class Config:
    repo: Path
    port: int = 8765
    bind: str = "0.0.0.0"
    check: str | None = None          # from the PC's CLI only, never from a request
    check_timeout: float = 300.0
    on_work_order: str | None = None  # likewise
    name: str = field(default_factory=socket.gethostname)


class LinkApp:
    def __init__(self, cfg: Config, state: State, claude_client: Any | None = None) -> None:
        self.cfg = cfg
        self.state = state.ensure()
        self.repo = repo_root(cfg.repo)
        self.patches = Patches(self.repo, self.state, cfg.check, cfg.check_timeout)
        self.inbox = Inbox(self.state)
        self.files = Files(self.state.files_dir)
        self.proxy = ClaudeProxy(self.state, claude_client)

    def check_token(self, given: str | None) -> bool:
        expected = self.state.token()
        return given is not None and hmac.compare_digest(given.strip().encode(), expected.encode())

    def status(self, authed: bool) -> dict[str, Any]:
        base: dict[str, Any] = {"ok": True, "name": self.cfg.name, "version": __version__, "auth": authed}
        if not authed:
            return base
        counts = self.inbox.counts()
        proposed = sum(1 for p in self.patches.list() if p["status"] == "proposed")
        return {**base, **repo_status(self.repo), "claude": self.proxy.available,
                "inbox_open": counts["open"] + counts["claimed"], "patches": proposed, "files": self.files.count()}

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
        if not self.quiet:
            sys.stderr.write(f"[link] {self.client_address[0]} {format % args}\n")

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

    def _write_audit(self) -> None:
        """Every write request (and every refused token) goes to log.jsonl once, with its outcome."""
        a = self._audit
        if self._audited:
            return
        if (a["method"] == "POST" and a["path"] != "/v1/messages") or a.get("error") == "token":
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
        authed = self.app.check_token(self.headers.get("X-Link-Token"))
        try:
            if path == "/link/status" and method == "GET":
                return self._json(200, self.app.status(authed))
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
        except (BrokenPipeError, ConnectionResetError):
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
        self.app.proxy.serve(body, betas, self)


def make_server(app: LinkApp, quiet: bool = False) -> ThreadingHTTPServer:
    handler = type("LinkHandler", (Handler,), {"app": app, "quiet": quiet})
    server = ThreadingHTTPServer((app.cfg.bind, app.cfg.port), handler)
    server.daemon_threads = True
    return server
