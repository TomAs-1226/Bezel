"""Pairing by a code shown on the PC: /link/pair and /link/pair/confirm."""
from __future__ import annotations

import unittest

from helpers import LinkCase

from catalyst_link import pairing
from catalyst_link.pairing import CODE_TRIES, Pairing
from catalyst_link.state import LinkError


class PairingCase(LinkCase):
    def setUp(self) -> None:
        self.shown: list[tuple[str, str]] = []
        self.pair_notify = [lambda code, device: self.shown.append((code, device))]
        super().setUp()
        self.now = 1000.0
        self.app.pairing._clock = lambda: self.now

    def start(self, device: str = "catalyst-tab") -> tuple[int, dict]:
        return self.request("POST", "/link/pair", {"device": device}, token=None)

    def confirm(self, pid: str, code: str) -> tuple[int, dict]:
        return self.request("POST", "/link/pair/confirm", {"pairing": pid, "code": code}, token=None)

    def wrong(self, code: str) -> str:
        return "000000" if code != "000000" else "111111"


class PairTest(PairingCase):
    def test_the_code_shows_on_the_pc_and_the_right_one_gets_the_token(self) -> None:
        status, body = self.start("kitchen tab")
        self.assertEqual(status, 200)
        self.assertEqual(body["digits"], 6)
        self.assertEqual(body["name"], "test-pc")
        self.assertNotIn("code", body)          # the code never travels to the tablet
        self.assertNotIn("token", body)
        self.assertEqual(len(self.shown), 1)
        code, device = self.shown[0]
        self.assertRegex(code, r"^\d{6}$")
        self.assertEqual(device, "kitchen tab")
        status, body2 = self.confirm(body["pairing"], code)
        self.assertEqual(status, 200)
        # the tablet gets a token of its own (devices.py), so it can be forgotten without the others
        self.assertNotEqual(body2["token"], self.token)
        self.assertRegex(body2["token"], r"^[a-z2-9]{4}(-[a-z2-9]{4}){3}$")
        # the token it handed over is one every other route takes
        status, _ = self.request("GET", "/inbox", token=body2["token"])
        self.assertEqual(status, 200)
        status, st = self.request("GET", "/link/status", token=body2["token"])
        self.assertTrue(st["auth"])

    def test_a_spaced_code_is_fine(self) -> None:
        _, body = self.start()
        code = self.shown[0][0]
        status, _ = self.confirm(body["pairing"], f"{code[:3]} {code[3:]}")
        self.assertEqual(status, 200)

    def test_a_code_is_used_once(self) -> None:
        _, body = self.start()
        code = self.shown[0][0]
        self.assertEqual(self.confirm(body["pairing"], code)[0], 200)
        status, err = self.confirm(body["pairing"], code)
        self.assertEqual(status, 410)
        self.assertEqual(err["error"], "expired")

    def test_wrong_codes_count_down_then_expire(self) -> None:
        _, body = self.start()
        code = self.shown[0][0]
        for left in range(CODE_TRIES - 1, 0, -1):
            status, err = self.confirm(body["pairing"], self.wrong(code))
            self.assertEqual(status, 403)
            self.assertEqual(err["error"], "code")
            self.assertEqual(err["attempts_left"], left)
        status, err = self.confirm(body["pairing"], self.wrong(code))
        self.assertEqual((status, err["why"]), (410, "tries"))
        # even the right code is no good now
        self.assertEqual(self.confirm(body["pairing"], code)[0], 410)

    def test_the_code_expires(self) -> None:
        _, body = self.start()
        self.now += pairing.CODE_TTL_S + 1
        status, err = self.confirm(body["pairing"], self.shown[0][0])
        self.assertEqual((status, err["why"]), (410, "expired"))

    def test_a_new_pairing_replaces_the_one_waiting(self) -> None:
        _, first = self.start()
        self.now += pairing.PAIR_GAP_S + 0.1
        _, second = self.start()
        old_code, new_code = self.shown[0][0], self.shown[1][0]
        self.assertEqual(self.confirm(first["pairing"], old_code)[0], 410)
        self.assertEqual(self.confirm(second["pairing"], new_code)[0], 200)

    def test_an_unknown_pairing_id_is_refused(self) -> None:
        self.start()
        self.assertEqual(self.confirm("0123456789abcdef", self.shown[0][0])[0], 410)

    def test_pairings_are_rate_limited(self) -> None:
        self.assertEqual(self.start()[0], 200)
        status, err = self.start()
        self.assertEqual(status, 429)
        for _ in range(pairing.PAIR_BURST - 1):
            self.now += pairing.PAIR_GAP_S + 0.1
            self.assertEqual(self.start()[0], 200)
        self.now += pairing.PAIR_GAP_S + 0.1
        self.assertEqual(self.start()[0], 429)
        self.now += pairing.PAIR_WINDOW_S
        self.assertEqual(self.start()[0], 200)

    def test_only_post(self) -> None:
        self.assertEqual(self.request("GET", "/link/pair", token=None)[0], 405)

    def test_neither_code_nor_token_reaches_the_log(self) -> None:
        _, body = self.start()
        code = self.shown[0][0]
        self.confirm(body["pairing"], self.wrong(code))
        self.confirm(body["pairing"], code)
        text = self.state.log_path.read_text(encoding="utf-8")
        self.assertNotIn(self.token, text)
        self.assertNotIn(f"\"{code}\"", text)
        kinds = [r.get("pairing") for r in self.log_lines() if r.get("path", "").startswith("/link/pair")]
        self.assertIn("started", kinds)
        self.assertIn("paired", kinds)

    def test_status_says_pairing_is_on(self) -> None:
        _, st = self.request("GET", "/link/status", token=None)
        self.assertFalse(st["auth"])
        self.assertTrue(st["pairing"])


class PairOffTest(LinkCase):
    pair = False

    def test_off_means_forbidden(self) -> None:
        status, err = self.request("POST", "/link/pair", {"device": "x"}, token=None)
        self.assertEqual(status, 403)
        self.assertIn("--no-pair", err["error"])
        _, st = self.request("GET", "/link/status", token=None)
        self.assertFalse(st["pairing"])


class NotifierTest(unittest.TestCase):
    def test_a_failing_notifier_doesnt_stop_pairing(self) -> None:
        def boom(code: str, device: str) -> None:
            raise RuntimeError("no toast here")
        seen: list[str] = []
        p = Pairing(lambda: "tok", [boom, lambda c, d: seen.append(c)])
        out = p.start({"device": "tab"}, "10.0.0.2", "pc")
        self.assertTrue(out["ok"])
        self.assertEqual(len(seen), 1)
        self.assertEqual(p.confirm({"pairing": out["pairing"], "code": seen[0]}, "pc")["token"], "tok")

    def test_disabled(self) -> None:
        with self.assertRaises(LinkError) as cm:
            Pairing(lambda: "tok", [], enabled=False).start({}, "ip", "pc")
        self.assertEqual(cm.exception.status, 403)

    def test_format(self) -> None:
        self.assertEqual(pairing.format_code("482913"), "482 913")


class MdnsTest(unittest.TestCase):
    def test_txt_says_pairing(self) -> None:
        import sys
        import types
        from catalyst_link import mdns

        captured: dict = {}

        class Info:
            def __init__(self, *a, **kw) -> None:
                captured.update(kw)

        class Zc:
            def register_service(self, info, allow_name_change=False) -> None:
                pass

            def unregister_service(self, info) -> None:
                pass

            def close(self) -> None:
                pass

        fake = types.ModuleType("zeroconf")
        fake.ServiceInfo = Info  # type: ignore[attr-defined]
        fake.Zeroconf = Zc  # type: ignore[attr-defined]
        old = sys.modules.get("zeroconf")
        sys.modules["zeroconf"] = fake
        try:
            stop = mdns.advertise("desk pc", 8765, "127.0.0.1", pair=True)
            self.assertIsNotNone(stop)
            self.assertEqual(captured["properties"]["pair"], "1")
            self.assertEqual(captured["port"], 8765)
            stop()
        finally:
            if old is None:
                del sys.modules["zeroconf"]
            else:
                sys.modules["zeroconf"] = old


if __name__ == "__main__":
    unittest.main()
