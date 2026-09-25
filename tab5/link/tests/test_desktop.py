"""What the desktop app needs: paired tablets with their own tokens, the /admin routes, installing
Claude Code's hooks, and `serve --gui` keeping secrets off the console."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from helpers import LinkCase, make_repo

from catalyst_link import claude_hooks
from catalyst_link.devices import Devices
from catalyst_link.state import LinkError

LINK_DIR = Path(__file__).resolve().parents[1]


class DesktopCase(LinkCase):
    def setUp(self) -> None:
        self.shown: list[tuple[str, str]] = []
        self.pair_notify = [lambda code, device: self.shown.append((code, device))]
        super().setUp()
        self.now = 1000.0
        self.app.pairing._clock = lambda: self.now

    def pair_tablet(self, device: str = "pit tab") -> str:
        _, body = self.request("POST", "/link/pair", {"device": device}, token=None)
        code = self.shown[-1][0]
        status, done = self.request("POST", "/link/pair/confirm", {"pairing": body["pairing"], "code": code},
                                    token=None)
        self.assertEqual(status, 200)
        self.now += 5  # past the gap between pairings
        return done["token"]


class DevicesTest(DesktopCase):
    def test_each_tablet_gets_its_own_token_and_forgetting_one_revokes_only_it(self) -> None:
        a = self.pair_tablet("tab a")
        b = self.pair_tablet("tab b")
        self.assertNotEqual(a, b)
        self.assertEqual(self.request("GET", "/inbox", token=a)[0], 200)
        self.assertEqual(self.request("GET", "/inbox", token=b)[0], 200)
        _, listing = self.request("GET", "/admin/devices")
        names = {d["name"]: d for d in listing["devices"]}
        self.assertEqual(set(names), {"tab a", "tab b"})
        self.assertEqual(names["tab a"]["ip"], "127.0.0.1")
        for d in listing["devices"]:
            self.assertNotIn("sha256", d)
            self.assertNotIn("token", d)
        status, out = self.request("POST", f"/admin/devices/{names['tab a']['id']}/forget", {})
        self.assertEqual((status, out["forgot"]), (200, names["tab a"]["id"]))
        self.assertEqual(self.request("GET", "/inbox", token=a)[0], 401)
        self.assertEqual(self.request("GET", "/inbox", token=b)[0], 200)
        # the main token is untouched
        self.assertEqual(self.request("GET", "/inbox")[0], 200)
        self.assertEqual(self.request("POST", "/admin/devices/nope/forget", {})[0], 404)

    def test_only_a_digest_is_stored(self) -> None:
        tok = self.pair_tablet()
        text = self.state.devices_path.read_text(encoding="utf-8")
        self.assertNotIn(tok, text)
        self.assertIn("sha256", text)

    def test_rotating_the_main_token_forgets_every_tablet(self) -> None:
        tok = self.pair_tablet()
        self.state.rotate_token()
        self.token = self.state.token()
        self.assertEqual(self.request("GET", "/inbox", token=tok)[0], 401)
        _, listing = self.request("GET", "/admin/devices")
        self.assertEqual(listing["devices"], [])

    def test_last_seen_moves(self) -> None:
        t = [1_790_000_000.0]
        d = Devices(self.tmp / "dev.json", clock=lambda: t[0])
        tok = d.add("tab", "10.0.0.5")
        dev_id = d.match(tok)
        self.assertIsNotNone(dev_id)
        t[0] += 3600
        d.touch(dev_id, "10.0.0.9")  # a new address is written at once
        again = Devices(self.tmp / "dev.json")
        self.assertEqual(again.list()[0]["ip"], "10.0.0.9")
        self.assertIsNone(d.match("aaaa-bbbb-cccc-dddd"))
        self.assertIsNone(d.match(None))


class AdminTest(DesktopCase):
    def test_a_tablet_token_cant_use_the_admin_routes(self) -> None:
        tok = self.pair_tablet()
        self.assertEqual(self.request("GET", "/admin/overview", token=tok)[0], 401)
        self.assertEqual(self.request("GET", "/admin/overview", token=None)[0], 401)
        self.assertEqual(self.request("GET", "/admin/overview")[0], 200)

    def test_a_web_page_cant_either(self) -> None:
        status, _ = self.request("GET", "/admin/pairing", headers={"Origin": "http://evil.example"})
        self.assertEqual(status, 403)
        status, _ = self.request("POST", "/admin/claude-hooks", {"install": True}, headers={"Origin": "null"})
        self.assertEqual(status, 403)

    def test_unknown_and_wrong_method(self) -> None:
        self.assertEqual(self.request("GET", "/admin/nothing")[0], 404)
        self.assertEqual(self.request("POST", "/admin/overview", {})[0], 405)

    def test_the_code_is_only_in_the_pairing_panel(self) -> None:
        _, body = self.request("POST", "/link/pair", {"device": "pit tab"}, token=None)
        code = self.shown[-1][0]
        _, ov = self.request("GET", "/admin/overview")
        self.assertEqual(ov["pairing"]["pending"]["device"], "pit tab")
        self.assertNotIn(code, json.dumps(ov))
        self.assertNotIn(self.token, json.dumps(ov))
        _, panel = self.request("GET", "/admin/pairing")
        self.assertEqual(panel["pending"]["code"], code)
        self.assertEqual(panel["pending"]["tries_left"], 5)
        self.assertGreater(panel["pending"]["expires_in"], 100)
        # neither the code nor any token ends up in the audit log
        self.request("POST", "/link/pair/confirm", {"pairing": body["pairing"], "code": code}, token=None)
        _, panel = self.request("GET", "/admin/pairing")
        self.assertIsNone(panel["pending"])
        self.assertEqual(panel["last"]["device"], "pit tab")
        log = self.state.log_path.read_text(encoding="utf-8")
        self.assertNotIn(f"\"{code}\"", log)
        self.assertNotIn(self.token, log)

    def test_cancel(self) -> None:
        _, body = self.request("POST", "/link/pair", {"device": "x"}, token=None)
        code = self.shown[-1][0]
        _, out = self.request("POST", "/admin/pairing/cancel", {})
        self.assertTrue(out["cancelled"])
        status, _ = self.request("POST", "/link/pair/confirm", {"pairing": body["pairing"], "code": code}, token=None)
        self.assertEqual(status, 410)

    def test_overview(self) -> None:
        _, ov = self.request("GET", "/admin/overview")
        self.assertEqual(ov["name"], "test-pc")
        self.assertIn("version", ov)
        self.assertEqual(ov["repo_branch"], "main")
        self.assertEqual(ov["mdns"]["state"], "off")
        self.assertIn("available", ov["media"])
        self.assertEqual(ov["inbox"]["open"], 0)
        self.assertEqual(ov["devices"], [])

    def test_the_pc_can_release_a_claimed_work_order(self) -> None:
        _, made = self.request("POST", "/inbox", {"title": "check the elevator", "body": "it sags"})
        wid = made["id"]
        # the tablet may not put an item back...
        self.request("POST", f"/inbox/{wid}/status", {"status": "claimed"})
        self.assertEqual(self.request("POST", f"/inbox/{wid}/status", {"status": "open"})[0], 400)
        # ...the PC may
        status, out = self.request("POST", f"/admin/inbox/{wid}/status", {"status": "open"})
        self.assertEqual((status, out["status"]), (200, "open"))
        self.assertEqual(self.request("POST", f"/admin/inbox/{wid}/status", {"status": "claimed"})[0], 200)
        self.assertEqual(self.request("POST", f"/admin/inbox/{wid}/status", {"status": "bogus"})[0], 400)


class HooksTest(DesktopCase):
    def setUp(self) -> None:
        super().setUp()
        self.settings = self.tmp / "claude" / "settings.json"
        self._old = os.environ.get("CATALYST_LINK_CLAUDE_SETTINGS")
        os.environ["CATALYST_LINK_CLAUDE_SETTINGS"] = str(self.settings)

    def tearDown(self) -> None:
        if self._old is None:
            os.environ.pop("CATALYST_LINK_CLAUDE_SETTINGS", None)
        else:
            os.environ["CATALYST_LINK_CLAUDE_SETTINGS"] = self._old
        super().tearDown()

    def test_install_keeps_everything_else_and_uninstall_removes_only_ours(self) -> None:
        self.settings.parent.mkdir(parents=True)
        mine = {"type": "command", "command": "echo mine"}
        original = {"model": "opus", "hooks": {"Stop": [{"hooks": [mine]}],
                                               "PreCompact": [{"hooks": [mine]}]}}
        self.settings.write_text(json.dumps(original), encoding="utf-8")
        _, st = self.request("GET", "/admin/claude-hooks")
        self.assertFalse(st["installed"])
        status, st = self.request("POST", "/admin/claude-hooks", {"install": True})
        self.assertEqual(status, 200)
        self.assertTrue(st["installed"])
        data = json.loads(self.settings.read_text(encoding="utf-8"))
        self.assertEqual(data["model"], "opus")
        self.assertEqual(data["hooks"]["PreCompact"], [{"hooks": [mine]}])
        self.assertEqual(data["hooks"]["Stop"][0], {"hooks": [mine]})
        self.assertEqual(len(data["hooks"]["Stop"]), 2)
        self.assertIn("hook.py", data["hooks"]["Stop"][1]["hooks"][0]["command"])
        self.assertTrue(self.settings.with_name("settings.json.catalyst-link.bak").exists())
        # installing twice doesn't duplicate
        self.request("POST", "/admin/claude-hooks", {"install": True})
        data = json.loads(self.settings.read_text(encoding="utf-8"))
        self.assertEqual(len(data["hooks"]["Stop"]), 2)
        status, st = self.request("POST", "/admin/claude-hooks", {"install": False})
        self.assertFalse(st["installed"])
        self.assertEqual(json.loads(self.settings.read_text(encoding="utf-8")), original)

    def test_a_fresh_file(self) -> None:
        st = claude_hooks.install("python x/catalyst_link/hook.py", self.settings)
        self.assertTrue(st["installed"])
        st = claude_hooks.uninstall(self.settings)
        self.assertFalse(st["installed"])
        self.assertEqual(json.loads(self.settings.read_text(encoding="utf-8")), {})

    def test_a_broken_file_is_left_alone(self) -> None:
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text("{ not json", encoding="utf-8")
        status, err = self.request("POST", "/admin/claude-hooks", {"install": True})
        self.assertEqual(status, 409)
        self.assertEqual(self.settings.read_text(encoding="utf-8"), "{ not json")
        with self.assertRaises(LinkError):
            claude_hooks.install(None, self.settings)

    def test_a_link_on_another_port_is_named_in_the_command(self) -> None:
        st = claude_hooks.install(None, self.settings, port=8799)
        self.assertIn("--url http://127.0.0.1:8799", st["command"])
        self.assertFalse(st["outdated"])
        data = json.loads(self.settings.read_text(encoding="utf-8"))
        self.assertIn("--url http://127.0.0.1:8799", data["hooks"]["Stop"][0]["hooks"][0]["command"])
        # the Link moved back to the default port: the installed hooks point at the old one
        self.assertTrue(claude_hooks.status(self.settings, port=8765)["outdated"])
        self.assertNotIn("--url", claude_hooks.hook_command(8765))
        from catalyst_link import hook
        self.assertEqual(hook._arg_url(["--url", "http://127.0.0.1:8799"]), "http://127.0.0.1:8799")
        self.assertEqual(hook._arg_url(["hook", "--url=http://h:1"]), "http://h:1")
        self.assertIsNone(hook._arg_url(["hook"]))

    def test_bad_body(self) -> None:
        self.assertEqual(self.request("POST", "/admin/claude-hooks", {"install": "yes"})[0], 400)


class OnePortOneLinkTest(DesktopCase):
    def test_a_second_link_cant_take_the_same_port(self) -> None:
        # On Windows SO_REUSEADDR would let both listen and split the tablet's requests between them.
        from catalyst_link.server import Config, LinkApp, make_server

        cfg = Config(repo=self.repo, port=self.port, bind="127.0.0.1", claude="off", media=False)
        with self.assertRaises(OSError):
            make_server(LinkApp(cfg, self.state, pair_notify=[]), quiet=True)


class GuiServeTest(unittest.TestCase):
    """`serve --gui` prints neither the token nor a pairing code, and still pairs."""

    def test_no_secrets_on_the_console(self) -> None:
        import http.client
        import socket

        tmp = Path(tempfile.mkdtemp(prefix="link-gui-")).resolve()
        repo = make_repo(tmp)
        home = tmp / "home"
        # no token yet: a first start under the app must make one, as the CLI does
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        env = {**os.environ, "CATALYST_LINK_HOME": str(home), "PYTHONPATH": str(LINK_DIR), "PYTHONIOENCODING": "utf-8"}
        proc = subprocess.Popen([sys.executable, "-u", "-m", "catalyst_link", "serve", "--repo", str(repo),
                                 "--port", str(port), "--bind", "127.0.0.1", "--gui", "--no-mdns", "--no-media",
                                 "--claude", "off"],
                                cwd=LINK_DIR, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            deadline = time.time() + 20
            while time.time() < deadline:
                try:
                    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                    conn.request("POST", "/link/pair", body=json.dumps({"device": "tab"}),
                                 headers={"Content-Type": "application/json"})
                    resp = conn.getresponse()
                    resp.read()
                    conn.close()
                    break
                except OSError:
                    time.sleep(0.2)
            else:
                self.fail("the Link didn't start")
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            token = (home / "token").read_text(encoding="utf-8").strip()
            conn.request("GET", "/admin/pairing", headers={"X-Link-Token": token})
            code = json.loads(conn.getresponse().read())["pending"]["code"]
            conn.close()
            time.sleep(0.5)
        finally:
            proc.terminate()
            out = proc.communicate(timeout=10)[0].decode("utf-8", "replace")
        self.assertIn("Catalyst Link", out)
        self.assertNotIn(token, out)
        self.assertNotIn(code, out)
        self.assertNotIn(f"{code[:3]} {code[3:]}", out)


if __name__ == "__main__":
    unittest.main()
