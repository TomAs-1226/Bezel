"""The media remote (/media/*): the PC's now-playing and its transport controls, with the OS layer faked."""
from __future__ import annotations

import base64
import io
import time
import unittest
from typing import Any
from unittest import mock

from helpers import LinkCase

from catalyst_link import media
from catalyst_link.media import Media, app_name

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:  # the rgb565 art needs Pillow
    HAVE_PIL = False


def png(color: tuple[int, int, int], w: int = 40, h: int = 20) -> bytes:
    buf = io.BytesIO()
    Image.new("RGB", (w, h), color).save(buf, "PNG")
    return buf.getvalue()


class FakePlatform:
    def __init__(self) -> None:
        self.current: dict[str, Any] | None = None
        self.calls: list[tuple[str, Any]] = []
        self.level: float | None = 0.4
        self.muted: bool | None = False
        self.accept = True
        self.fail: BaseException | None = None

    def session(self) -> dict[str, Any] | None:
        if self.fail:
            raise self.fail
        return self.current

    def control(self, action: str) -> bool:
        self.calls.append(("control", action))
        return self.accept

    def volume(self) -> tuple[float | None, bool | None]:
        return self.level, self.muted

    def set_volume(self, level: float) -> bool:
        self.calls.append(("set_volume", level))
        if self.level is None:
            return False
        self.level = level
        return True

    def volume_key(self, which: str) -> bool:
        self.calls.append(("key", which))
        return True


def song(**kw: Any) -> dict[str, Any]:
    s = {"title": "Midnight City", "artist": "M83", "album": "Hurry Up, We're Dreaming",
         "app": "Spotify.exe", "status": 4, "position": 30.0, "duration": 243.0, "updated": time.time(),
         "can": {"play": False, "pause": True, "next": True, "previous": True}, "thumbnail": None}
    s.update(kw)
    return s


class MediaCase(LinkCase):
    def setUp(self) -> None:
        self.fake = FakePlatform()
        self.media_platform = self.fake
        super().setUp()


class NowPlayingTest(MediaCase):
    def test_nothing_playing(self) -> None:
        status, body = self.request("GET", "/media/now")
        self.assertEqual(status, 200)
        self.assertTrue(body["available"])
        self.assertIsNone(body["playing"])
        self.assertEqual(body["volume"], 0.4)
        self.assertFalse(body["muted"])

    def test_a_song(self) -> None:
        self.fake.current = song()
        status, body = self.request("GET", "/media/now")
        self.assertEqual(status, 200)
        p = body["playing"]
        self.assertEqual((p["title"], p["artist"], p["app"], p["state"]), ("Midnight City", "M83", "Spotify", "playing"))
        self.assertEqual(p["duration"], 243.0)
        self.assertGreaterEqual(p["position"], 30.0)
        self.assertLess(p["position"], 33.0)
        self.assertTrue(p["can"]["next"])
        self.assertIsNone(p["art"])  # no thumbnail

    def test_position_runs_on_while_playing_but_not_paused(self) -> None:
        self.fake.current = song(updated=time.time() - 10)
        _, body = self.request("GET", "/media/now")
        self.assertAlmostEqual(body["playing"]["position"], 40.0, delta=1.5)
        self.fake.current = song(status=5, updated=time.time() - 10)
        _, body = self.request("GET", "/media/now")
        self.assertEqual(body["playing"]["state"], "paused")
        self.assertEqual(body["playing"]["position"], 30.0)

    def test_position_never_passes_the_end(self) -> None:
        self.fake.current = song(position=240.0, updated=time.time() - 60)
        _, body = self.request("GET", "/media/now")
        self.assertEqual(body["playing"]["position"], 243.0)

    def test_unknown_timeline(self) -> None:
        self.fake.current = song(position=None, duration=None)
        _, body = self.request("GET", "/media/now")
        self.assertIsNone(body["playing"]["position"])
        self.assertIsNone(body["playing"]["duration"])

    def test_needs_the_token(self) -> None:
        status, body = self.request("GET", "/media/now", token="wrong")
        self.assertEqual(status, 401)
        status, _ = self.request("POST", "/media/control", {"action": "toggle"}, token=None)
        self.assertEqual(status, 401)
        self.assertEqual(self.fake.calls, [])

    def test_status_reports_media(self) -> None:
        _, body = self.request("GET", "/link/status")
        self.assertTrue(body["media"])

    def test_platform_error_is_a_502(self) -> None:
        self.fake.fail = OSError("RPC server unavailable")
        status, body = self.request("GET", "/media/now")
        self.assertEqual(status, 502)
        self.assertIn("RPC server unavailable", body["error"])


class ControlTest(MediaCase):
    def test_transport(self) -> None:
        for action in ("play", "pause", "toggle", "next", "previous", "stop"):
            status, body = self.request("POST", "/media/control", {"action": action})
            self.assertEqual(status, 200, action)
            self.assertEqual(body, {"ok": True, "done": True})
        self.assertEqual([a for _, a in self.fake.calls], ["play", "pause", "toggle", "next", "previous", "stop"])

    def test_refused_by_the_app(self) -> None:
        self.fake.accept = False
        status, body = self.request("POST", "/media/control", {"action": "next"})
        self.assertEqual(status, 200)
        self.assertFalse(body["done"])

    def test_volume_keys(self) -> None:
        for action in ("volume_up", "volume_down", "mute"):
            self.assertEqual(self.request("POST", "/media/control", {"action": action})[0], 200)
        self.assertEqual(self.fake.calls, [("key", "up"), ("key", "down"), ("key", "mute")])

    def test_volume_level(self) -> None:
        status, _ = self.request("POST", "/media/control", {"action": "volume", "level": 0.75})
        self.assertEqual(status, 200)
        self.assertEqual(self.fake.level, 0.75)
        for bad in (1.5, -0.1, "loud", True, None):
            status, _ = self.request("POST", "/media/control", {"action": "volume", "level": bad})
            self.assertEqual(status, 400, bad)

    def test_volume_level_without_pycaw(self) -> None:
        self.fake.level = None
        status, body = self.request("POST", "/media/control", {"action": "volume", "level": 0.5})
        self.assertEqual(status, 501)
        self.assertIn("pycaw", body["error"])

    def test_bad_action(self) -> None:
        for body in ({"action": "rm -rf"}, {}, {"action": 3}):
            status, reply = self.request("POST", "/media/control", body)
            self.assertEqual(status, 400)
            self.assertFalse(reply["ok"])
        self.assertEqual(self.fake.calls, [])

    def test_not_a_write_in_the_audit_log(self) -> None:
        self.request("POST", "/media/control", {"action": "toggle"})
        self.assertFalse([line for line in self.log_lines() if line.get("path", "").startswith("/media/")])

    def test_get_is_not_allowed(self) -> None:
        self.assertEqual(self.request("GET", "/media/control")[0], 405)


@unittest.skipUnless(HAVE_PIL, "Pillow makes the art")
class ArtTest(MediaCase):
    def test_no_art(self) -> None:
        self.fake.current = song()
        status, body = self.request("GET", "/media/art")
        self.assertEqual(status, 404)
        self.fake.current = None
        self.assertEqual(self.request("GET", "/media/art")[0], 404)

    def test_rgb565_is_exact_and_cropped_to_fill(self) -> None:
        self.fake.current = song(thumbnail=png((255, 0, 0)))
        status, (resp, payload) = self.request("GET", "/media/art?format=rgb565&size=32", raw=True)
        self.assertEqual(status, 200)
        self.assertEqual(resp.getheader("Content-Type"), "application/octet-stream")
        self.assertEqual(len(payload), 32 * 32 * 2)
        self.assertEqual(payload[:2], bytes([0x00, 0xF8]))  # pure red, RGB565 little-endian

    def test_base64_json_for_the_tablet(self) -> None:
        self.fake.current = song(thumbnail=png((0, 0, 255)))
        _, now = self.request("GET", "/media/now")
        aid = now["playing"]["art"]
        self.assertTrue(aid)
        status, body = self.request("GET", "/media/art?format=rgb565&size=16&encoding=base64")
        self.assertEqual(status, 200)
        self.assertEqual((body["id"], body["size"], body["format"], body["bytes"]), (aid, 16, "rgb565", 512))
        px = base64.b64decode(body["data"])
        self.assertEqual(px[:2], bytes([0x1F, 0x00]))  # pure blue

    def test_jpeg(self) -> None:
        self.fake.current = song(thumbnail=png((0, 255, 0), 300, 300))
        status, (resp, payload) = self.request("GET", "/media/art?size=100", raw=True)
        self.assertEqual(status, 200)
        self.assertEqual(resp.getheader("Content-Type"), "image/jpeg")
        im = Image.open(io.BytesIO(payload))
        self.assertEqual((im.format, im.size), ("JPEG", (100, 100)))
        self.assertFalse(im.info.get("progressive"))

    def test_art_id_follows_the_song(self) -> None:
        self.fake.current = song(thumbnail=png((10, 10, 10)))
        a = self.request("GET", "/media/now")[1]["playing"]["art"]
        self.fake.current = song(title="Wait", thumbnail=png((10, 10, 10)))
        b = self.request("GET", "/media/now")[1]["playing"]["art"]
        self.assertNotEqual(a, b)

    def test_bad_requests(self) -> None:
        self.fake.current = song(thumbnail=png((1, 2, 3)))
        for q in ("format=gif", "size=2", "size=9999", "size=big"):
            self.assertEqual(self.request("GET", f"/media/art?{q}")[0], 400, q)

    def test_unreadable_thumbnail(self) -> None:
        self.fake.current = song(thumbnail=b"not an image")
        status, body = self.request("GET", "/media/art?format=rgb565")
        self.assertEqual(status, 404)


class UnavailableTest(LinkCase):
    """No platform: the endpoints say why, and nothing else breaks."""

    def setUp(self) -> None:
        patcher = mock.patch.object(media, "WindowsPlatform", side_effect=media.Unavailable("the media remote needs Windows"))
        patcher.start()
        self.addCleanup(patcher.stop)
        super().setUp()

    def test_now(self) -> None:
        status, body = self.request("GET", "/media/now")
        self.assertEqual(status, 200)
        self.assertFalse(body["available"])
        self.assertIn("Windows", body["reason"])
        self.assertIsNone(body["playing"])

    def test_control_and_art(self) -> None:
        self.assertEqual(self.request("POST", "/media/control", {"action": "toggle"})[0], 503)
        self.assertEqual(self.request("GET", "/media/art")[0], 404)
        _, st = self.request("GET", "/link/status")
        self.assertFalse(st["media"])

    def test_the_rest_of_the_link_still_works(self) -> None:
        status, body = self.request("GET", "/code/tree")
        self.assertEqual(status, 200)


class OffTest(unittest.TestCase):
    def test_no_media_flag(self) -> None:
        m = Media(FakePlatform(), enabled=False)
        self.assertFalse(m.available)
        self.assertIn("--no-media", m.now()["reason"])

    def test_a_broken_projection_never_stops_the_link(self) -> None:
        with mock.patch.object(media, "WindowsPlatform", side_effect=RuntimeError("bad dll")):
            m = Media()
        self.assertFalse(m.available)
        self.assertIn("bad dll", m.reason)


class AppNameTest(unittest.TestCase):
    def test_names(self) -> None:
        self.assertEqual(app_name("Spotify.exe"), "Spotify")
        self.assertEqual(app_name("Chrome._crx_cinhimbnkkaeohfgghhklpknlkffjgod"), "YouTube Music")
        self.assertEqual(app_name("MSEdge"), "Edge")
        self.assertEqual(app_name("308046B0AF4A39CB"), "Firefox")
        self.assertEqual(app_name("C:\\Tools\\mpv.exe"), "mpv")
        self.assertEqual(app_name(""), "an app")


if __name__ == "__main__":
    unittest.main()
