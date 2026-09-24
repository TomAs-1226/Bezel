"""Shared fixtures: a throwaway robot repo, and a Link served on 127.0.0.1 on a free port."""
from __future__ import annotations

import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from catalyst_link.server import Config, LinkApp, make_server  # noqa: E402
from catalyst_link.state import State  # noqa: E402

CONSTANTS = """package frc.robot;

public final class Constants {
  public static final double kElevatorP = 0.8;
  public static final double kElevatorI = 0.0;
  public static final double kArmP = 0.8;
}
"""

ROBOT = "package frc.robot;\n\npublic class Robot {\n" + "".join(
    f"  // line {i}: TimedRobot bookkeeping\n" for i in range(4, 500)) + "}\n"


def _can_symlink() -> bool:
    """Windows makes symlinks only with Developer Mode (or as admin); the symlink cases skip without it."""
    with tempfile.TemporaryDirectory() as d:
        try:
            os.symlink(Path(d) / "target", Path(d) / "link")
        except (OSError, NotImplementedError):
            return False
    return True


SYMLINKS = _can_symlink()
# The repo's symlinks that point outside it (or into .git): refused like any escape, when they exist.
SYMLINK_NAMES = ("escape.txt", "gitconfig-link") if SYMLINKS else ()


def git(repo: Path, *args: str) -> str:
    env = {**os.environ, "GIT_AUTHOR_NAME": "Team", "GIT_AUTHOR_EMAIL": "team@example.com",
           "GIT_COMMITTER_NAME": "Team", "GIT_COMMITTER_EMAIL": "team@example.com"}
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True,
                          env=env).stdout.strip()


def make_repo(root: Path) -> Path:
    """A small FRC-shaped repo on `main` with one commit, plus the things the Link must refuse."""
    repo = root / "CatalystX1"
    java = repo / "src/main/java/frc/robot"
    java.mkdir(parents=True)
    (java / "Constants.java").write_text(CONSTANTS, newline="")  # LF on Windows too
    (java / "Robot.java").write_text(ROBOT, newline="")
    (java / "Dupes.java").write_text("int x = 1;\nint x = 1;\n")
    (java / "Crlf.java").write_text("class Crlf {\r\n  int a = 1;\r\n  int b = 2;\r\n}\r\n", newline="")
    (repo / "vendordeps").mkdir()
    (repo / "vendordeps/Phoenix6.json").write_text('{"name": "Phoenix6"}\n')
    (repo / ".env").write_text("TOKEN=hunter2\n")
    (repo / "deploy.key").write_text("-----BEGIN KEY-----\n")
    (repo / "config").mkdir()
    (repo / "config/secrets.json").write_text('{"password": "x"}\n')
    (repo / "build").mkdir()
    (repo / "build/libs.txt").write_text("output\n")
    (repo / "logo.bin.dat").write_bytes(b"\x89PNG\x00\x00binary")
    outside = root / "outside.txt"
    outside.write_text("not the robot's\n")
    if SYMLINKS:
        os.symlink(outside, repo / "escape.txt")
        os.symlink(repo / ".git" / "config", repo / "gitconfig-link")
    git(root, "init", "-q", "-b", "main", str(repo))
    git(repo, "config", "core.autocrlf", "false")  # a global autocrlf=true would rewrite the CRLF fixture
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "initial")
    return repo


class FakeStream:
    """Stands in for the SDK's Stream: iterable events, close() recorded."""

    def __init__(self, events: list[Any], raise_after: BaseException | None = None, delay: float = 0.0) -> None:
        self.events = events
        self.raise_after = raise_after
        self.delay = delay
        self.closed = threading.Event()
        self.yielded = 0

    def __iter__(self):  # type: ignore[no-untyped-def]
        import time
        for e in self.events:
            if self.closed.is_set():
                return
            if self.delay:
                time.sleep(self.delay)
            self.yielded += 1
            yield e
        if self.raise_after:
            raise self.raise_after

    def close(self) -> None:
        self.closed.set()


class FakeMessages:
    """beta.messages with a signature like an SDK that predates `fallbacks` (so it must go via extra_body)."""

    def __init__(self) -> None:
        self.calls: list[dict[str, Any]] = []
        self.next: FakeStream | BaseException | None = None

    def create(self, *, max_tokens: int, messages: list[Any], model: str, stream: bool = False,
               betas: list[str] | None = None, system: Any = None, thinking: Any = None, tools: Any = None,
               output_config: Any = None, extra_headers: Any = None, extra_body: Any = None,
               timeout: Any = None) -> FakeStream:
        self.calls.append({"max_tokens": max_tokens, "messages": messages, "model": model, "stream": stream,
                           "betas": betas, "system": system, "thinking": thinking, "tools": tools,
                           "output_config": output_config, "extra_body": extra_body})
        nxt = self.next
        if isinstance(nxt, BaseException):
            raise nxt
        assert nxt is not None
        return nxt


class FakeClient:
    def __init__(self) -> None:
        self.messages = FakeMessages()
        self.beta = self


class LinkCase(unittest.TestCase):
    """A repo, a state dir and a running Link per test."""

    check: str | None = None
    check_timeout: float = 300.0
    on_work_order: str | None = None
    claude_client: Any = None
    # The API-key proxy unless a test says otherwise: "auto" would pick claude-code on a PC without a key,
    # and the tests must not depend on whether this PC is logged in to Claude Code.
    claude: str = "api"
    agent_factory: Any = None

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="link-test-")).resolve()
        self.repo = make_repo(self.tmp)
        self.state = State(self.tmp / "home").ensure()
        self.token = self.state.token()
        cfg = Config(repo=self.repo, port=0, bind="127.0.0.1", check=self.check,
                     check_timeout=self.check_timeout, on_work_order=self.on_work_order, name="test-pc",
                     claude=self.claude)
        self.app = LinkApp(cfg, self.state, claude_client=self.claude_client, agent_factory=self.agent_factory)
        self.server = make_server(self.app, quiet=True)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, args=(0.05,), daemon=True)
        self.thread.start()

    def tearDown(self) -> None:
        if hasattr(self.app.proxy, "shutdown"):
            self.app.proxy.shutdown()  # the claude-code backend's sessions
        self.server.shutdown()
        self.server.server_close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def request(self, method: str, path: str, body: Any = None, *, token: str | None = "default",
                headers: dict[str, str] | None = None, raw: bool = False) -> tuple[int, Any]:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        hdrs = dict(headers or {})
        if token == "default":
            token = self.token
        if token is not None:
            hdrs["X-Link-Token"] = token
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            hdrs.setdefault("Content-Type", "application/json")
        conn.request(method, path, body=data, headers=hdrs)
        resp = conn.getresponse()
        payload = resp.read()
        conn.close()
        if raw:
            return resp.status, (resp, payload)
        return resp.status, json.loads(payload) if payload else None

    def log_lines(self) -> list[dict[str, Any]]:
        if not self.state.log_path.exists():
            return []
        return [json.loads(line) for line in self.state.log_path.read_text().splitlines()]
