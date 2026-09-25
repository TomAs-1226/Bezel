"""Pairing: the tablet gets the Link's token without anyone typing it.

The tablet asks to pair (POST /link/pair); the Link makes a six-digit code and shows it on the PC (the
console the Link runs in, and a Windows notification when it can); the owner types that code on the
tablet (POST /link/pair/confirm), and the Link answers with its token. Whoever can read the PC's screen
is the one pairing, so a device elsewhere on the LAN can't pair itself.

Guess-proofing: one pairing at a time (a new request replaces the one waiting), a code lives two
minutes and takes five wrong tries, and new pairings are rate-limited. Six digits and five tries is a
1-in-200,000 chance per code, and at most PAIR_BURST codes are made in PAIR_WINDOW_S.
"""
from __future__ import annotations

import hmac
import os
import secrets
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable

from .state import LinkError

CODE_TTL_S = 120.0
CODE_TRIES = 5
PAIR_GAP_S = 3.0          # between two new pairings
PAIR_BURST = 10           # new pairings ...
PAIR_WINDOW_S = 600.0     # ... per this window

Notify = Callable[[str, str], None]  # (code, device) -> shows the code on the PC


@dataclass
class Pending:
    id: str
    code: str
    device: str
    ip: str
    expires: float
    tries: int = CODE_TRIES


def format_code(code: str) -> str:
    """"482913" -> "482 913", as the PC shows it."""
    return f"{code[:3]} {code[3:]}"


def console_notify(code: str, device: str) -> None:
    line = "=" * 46
    print(f"\n{line}\n  pairing {device}: enter  {format_code(code)}  on the tablet\n"
          f"  (the code works for {CODE_TTL_S / 60:g} minutes)\n{line}\n", flush=True)


def toast_notify(code: str, device: str) -> None:
    """A Windows notification with the code; best effort, never waits, never raises."""
    if os.name != "nt":
        return
    # The code is digits and the device name is reduced to [A-Za-z0-9 -]: nothing here needs escaping.
    who = "".join(c for c in device if c.isalnum() or c in " -")[:40] or "the tablet"
    script = (
        "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType=WindowsRuntime] > $null;"
        "$x = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent("
        "[Windows.UI.Notifications.ToastTemplateType]::ToastText02);"
        "$t = $x.GetElementsByTagName('text');"
        f"$t.Item(0).AppendChild($x.CreateTextNode('Catalyst Link: {format_code(code)}')) > $null;"
        f"$t.Item(1).AppendChild($x.CreateTextNode('Enter this code on {who} to pair it with this PC.')) > $null;"
        "$n = [Windows.UI.Notifications.ToastNotification]::new($x);"
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("
        "'{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe').Show($n)"
    )
    try:
        subprocess.Popen(["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", script],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except OSError:
        pass


class Pairing:
    def __init__(self, token: Callable[[], str], notify: list[Notify] | None = None,
                 enabled: bool = True, clock: Callable[[], float] = time.monotonic) -> None:
        self._token = token
        self.notify = notify if notify is not None else [console_notify]
        self.enabled = enabled
        self._clock = clock
        self._lock = threading.Lock()
        self._pending: Pending | None = None
        self._starts: list[float] = []

    def start(self, body: dict[str, Any], ip: str, name: str) -> dict[str, Any]:
        if not self.enabled:
            raise LinkError(403, "pairing is off on this Link (serve --no-pair): type the token instead")
        device = str(body.get("device") or "a tablet")[:48]
        now = self._clock()
        with self._lock:
            self._starts = [t for t in self._starts if now - t < PAIR_WINDOW_S]
            if self._starts and now - self._starts[-1] < PAIR_GAP_S:
                raise LinkError(429, "a pairing just started: wait a moment", retry_after=PAIR_GAP_S)
            if len(self._starts) >= PAIR_BURST:
                raise LinkError(429, "too many pairings: try again in a few minutes", retry_after=PAIR_WINDOW_S)
            self._starts.append(now)
            code = f"{secrets.randbelow(10 ** 6):06d}"
            self._pending = Pending(secrets.token_hex(8), code, device, ip, now + CODE_TTL_S)
            pid = self._pending.id
        for fn in self.notify:
            try:
                fn(code, device)
            except Exception as exc:  # a notifier is a convenience, never the reason pairing fails
                print(f"[link] pairing notification failed: {exc}", file=sys.stderr)
        return {"ok": True, "pairing": pid, "digits": 6, "expires_in": int(CODE_TTL_S), "name": name}

    def confirm(self, body: dict[str, Any], name: str) -> dict[str, Any]:
        pid = str(body.get("pairing") or "")
        code = "".join(c for c in str(body.get("code") or "") if c.isdigit())
        now = self._clock()
        with self._lock:
            p = self._pending
            if p is None or not hmac.compare_digest(p.id.encode(), pid.encode()) or now > p.expires:
                if p is not None and now > p.expires:
                    self._pending = None
                raise LinkError(410, "expired", why="expired")
            if not hmac.compare_digest(p.code.encode(), code.encode()):
                p.tries -= 1
                if p.tries <= 0:
                    self._pending = None
                    raise LinkError(410, "expired", why="tries")
                raise LinkError(403, "code", attempts_left=p.tries)
            self._pending = None
        return {"ok": True, "token": self._token(), "name": name}

    @property
    def pending(self) -> Pending | None:
        with self._lock:
            return self._pending
