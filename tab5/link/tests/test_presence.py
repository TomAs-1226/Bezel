"""The BLE presence beacon: the id, who is allowed to learn it, and what rotating the token does to it.

The radio itself is not testable here and is not the interesting part. What is interesting is that the
beacon id is behind the same gate as the rest of the API, that it survives a restart and does not survive
a rotate, and that a PC which cannot advertise says so instead of breaking anything.
"""
from __future__ import annotations

import unittest
from pathlib import Path
from typing import Any

from catalyst_link import presence
from catalyst_link.state import State
from helpers import LinkCase


class FakeBeacon:
    """Stands in for WindowsBeacon: it is handed the id and asked how it is getting on."""

    def __init__(self, state: str = "started") -> None:
        self._state = state
        self.stopped = False

    def state(self) -> str:
        return self._state

    def stop(self) -> None:
        self.stopped = True


class PresenceIdTest(unittest.TestCase):
    def setUp(self) -> None:
        import tempfile

        self.tmp = Path(tempfile.mkdtemp(prefix="link-presence-")).resolve()
        self.state = State(self.tmp / "home").ensure()

    def tearDown(self) -> None:
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_id_is_four_bytes_and_survives_a_restart(self) -> None:
        first = presence.beacon_id(self.state.home)
        self.assertEqual(len(first), presence.ID_BYTES)
        # A tablet that learned the id must not have to learn it again because the Link was restarted.
        self.assertEqual(presence.beacon_id(self.state.home), first)

    def test_rotating_the_token_forgets_the_beacon(self) -> None:
        first = presence.beacon_id(self.state.home)
        self.state.rotate_token()
        self.assertFalse(self.state.presence_path.exists())
        # A forgotten tablet keeps its old id; it must no longer match this PC.
        self.assertNotEqual(presence.beacon_id(self.state.home), first)

    def test_a_corrupt_id_file_is_replaced_rather_than_fatal(self) -> None:
        self.state.presence_path.write_text("not hex at all\n", encoding="utf-8")
        self.assertEqual(len(presence.beacon_id(self.state.home)), presence.ID_BYTES)

    def test_the_id_is_not_derived_from_the_token(self) -> None:
        """Reading the beacon off the air must say nothing about the token, so the two are unrelated."""
        token = self.state.token()
        raw = presence.beacon_id(self.state.home)
        self.assertNotIn(raw.hex(), token.replace("-", ""))

    def test_payload_carries_the_magic_the_tablet_matches(self) -> None:
        raw = bytes([1, 2, 3, 4])
        self.assertEqual(presence.payload(raw), b"CL" + bytes([presence.FORMAT]) + raw)

    def test_no_beacon_on_this_pc_is_a_reason_not_an_error(self) -> None:
        def explode(_: bytes) -> Any:
            raise presence.Unavailable("no radio here")

        old = presence.WindowsBeacon
        presence.WindowsBeacon = explode  # type: ignore[assignment,misc]
        try:
            p = presence.Presence(self.state)
        finally:
            presence.WindowsBeacon = old  # type: ignore[assignment,misc]
        self.assertFalse(p.available)
        self.assertEqual(p.reason, "no radio here")
        # "" rather than the id: a tablet told nothing never starts its radio, which is today's behaviour.
        self.assertEqual(p.hex, "")
        self.assertEqual(p.report(), {"available": False, "reason": "no radio here"})


class PresenceStatusTest(LinkCase):
    presence = True
    presence_beacon = None  # set in setUp, which needs the instance

    def setUp(self) -> None:
        self.presence_beacon = FakeBeacon()
        super().setUp()

    def test_status_gives_the_id_only_with_a_good_token(self) -> None:
        code, body = self.request("GET", "/link/status")
        self.assertEqual(code, 200)
        self.assertEqual(body["presence"], self.app.presence.id.hex())

        # Without a token /link/status still answers, but only its name: presence is not in that answer,
        # so an unpaired tablet (or anyone on the LAN) cannot learn which beacon belongs to this PC.
        code, body = self.request("GET", "/link/status", token=None)
        self.assertEqual(code, 200)
        self.assertNotIn("presence", body)

    def test_the_admin_overview_reports_the_publishers_own_state(self) -> None:
        code, body = self.request("GET", "/admin/overview")
        self.assertEqual(code, 200)
        self.assertEqual(body["presence"]["state"], "started")
        self.assertTrue(body["presence"]["available"])
        self.assertEqual(body["presence"]["company_id"], "0xffff")

    def test_stopping_stops_advertising_and_hides_the_id(self) -> None:
        self.app.presence.stop()
        self.assertTrue(self.presence_beacon.stopped)
        code, body = self.request("GET", "/link/status")
        self.assertEqual(body["presence"], "")


class PresenceOffTest(LinkCase):
    presence = False

    def test_off_says_so_and_hands_out_nothing(self) -> None:
        code, body = self.request("GET", "/link/status")
        self.assertEqual(body["presence"], "")
        self.assertFalse(self.app.presence.available)
        self.assertIn("--no-presence", self.app.presence.reason)
        # No id is even generated when the beacon is off: nothing to leak and nothing to clean up.
        self.assertFalse(self.state.presence_path.exists())


if __name__ == "__main__":
    unittest.main()
