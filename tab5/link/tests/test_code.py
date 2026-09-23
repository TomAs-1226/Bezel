"""Auth, /link/status, path safety and the read-only code endpoints."""
from __future__ import annotations

import unittest
from urllib.parse import quote

from helpers import LinkCase

from catalyst_link import pathsafe
from catalyst_link.state import LinkError


class AuthTest(LinkCase):
    def test_missing_token_is_401(self) -> None:
        status, body = self.request("GET", "/code/tree", token=None)
        self.assertEqual(status, 401)
        self.assertEqual(body["error"], "token")

    def test_wrong_token_is_401_and_logged(self) -> None:
        status, body = self.request("GET", "/inbox", token="nope-nope-nope-nope")
        self.assertEqual(status, 401)
        self.assertEqual(body, {"ok": False, "error": "token"})
        self.assertTrue(any(r.get("error") == "token" for r in self.log_lines()))

    def test_post_without_token_writes_nothing(self) -> None:
        status, _ = self.request("POST", "/inbox", {"title": "x"}, token=None)
        self.assertEqual(status, 401)
        self.assertEqual(list(self.state.inbox_dir.glob("*.md")), [])

    def test_status_without_token_says_little(self) -> None:
        status, body = self.request("GET", "/link/status", token=None)
        self.assertEqual(status, 200)
        self.assertEqual(set(body), {"ok", "name", "version", "auth"})
        self.assertFalse(body["auth"])
        self.assertEqual(body["name"], "test-pc")

    def test_status_with_token(self) -> None:
        status, body = self.request("GET", "/link/status")
        self.assertEqual(status, 200)
        self.assertTrue(body["auth"])
        self.assertEqual(body["repo"], "CatalystX1")
        self.assertEqual(body["branch"], "main")
        self.assertFalse(body["dirty"])
        self.assertFalse(body["claude"])  # no client injected, no key in the test environment
        self.assertEqual((body["inbox_open"], body["patches"], body["files"]), (0, 0, 0))

    def test_token_rotation_takes_effect(self) -> None:
        new = self.state.rotate_token()
        self.assertEqual(self.request("GET", "/inbox")[0], 401)
        self.assertEqual(self.request("GET", "/inbox", token=new)[0], 200)

    def test_unknown_route_and_method(self) -> None:
        self.assertEqual(self.request("GET", "/nope")[0], 404)
        self.assertEqual(self.request("GET", "/code/patch")[0], 405)


class PathSafetyTest(unittest.TestCase):
    def test_clean(self) -> None:
        self.assertEqual(pathsafe.clean("./src//main/"), "src/main")
        self.assertEqual(pathsafe.clean("src\\main"), "src/main")
        for bad in ("../x", "src/../../x", "/etc/passwd", "C:/Windows", "a/\x00b"):
            with self.assertRaises(LinkError, msg=bad):
                pathsafe.clean(bad)

    def test_deny_list(self) -> None:
        for bad in (".env", ".env.local", "prod.env", "config/secrets.json", "deploy.key", "cert.pem",
                    "build/x.class", "sub/build/x", ".gradle/caches", "bin/Main.class", ".git/config",
                    "a/.GIT/HEAD", "My_Secret.txt"):
            self.assertIsNotNone(pathsafe.denied(tuple(bad.split("/"))), bad)
        for ok in ("src/main/java/frc/robot/Robot.java", "build.gradle", "vendordeps/Phoenix6.json", ".gitignore"):
            self.assertIsNone(pathsafe.denied(tuple(ok.split("/"))), ok)


class CodeEndpointsTest(LinkCase):
    def read(self, path: str, **q: int) -> tuple[int, dict]:
        extra = "".join(f"&{k}={v}" for k, v in q.items())
        return self.request("GET", f"/code/read?path={quote(path)}{extra}")

    def test_refusals(self) -> None:
        cases = {
            "../outside.txt": 403, "/etc/passwd": 403, "escape.txt": 403, "gitconfig-link": 403,
            ".git/config": 403, ".env": 403, "deploy.key": 403, "config/secrets.json": 403,
            "build/libs.txt": 403, "src/../../outside.txt": 403, "missing.java": 404,
        }
        for path, want in cases.items():
            status, body = self.read(path)
            self.assertEqual(status, want, path)
            self.assertFalse(body["ok"])

    def test_tree(self) -> None:
        status, body = self.request("GET", "/code/tree?depth=10")
        self.assertEqual(status, 200)
        paths = {e["path"] for e in body["entries"]}
        self.assertIn("src/main/java/frc/robot/Constants.java", paths)
        self.assertIn("src", paths)
        for hidden in (".git", ".env", "deploy.key", "config/secrets.json", "build", "escape.txt", "gitconfig-link"):
            self.assertNotIn(hidden, paths)
        entry = next(e for e in body["entries"] if e["path"].endswith("Constants.java"))
        self.assertEqual(entry["type"], "file")
        self.assertGreater(entry["size"], 0)
        self.assertFalse(body["truncated"])

    def test_tree_depth_and_subpath(self) -> None:
        _, shallow = self.request("GET", "/code/tree?depth=1")
        self.assertTrue(all("/" not in e["path"] for e in shallow["entries"]))
        _, sub = self.request("GET", "/code/tree?path=src/main/java&depth=2")
        self.assertEqual({e["path"] for e in sub["entries"]}, {"src/main/java/frc", "src/main/java/frc/robot"})

    def test_tree_truncates_at_400(self) -> None:
        many = self.repo / "many"
        many.mkdir()
        for i in range(450):
            (many / f"f{i:03}.txt").write_text("x")
        _, body = self.request("GET", "/code/tree?path=many")
        self.assertEqual(len(body["entries"]), 400)
        self.assertTrue(body["truncated"])

    def test_read_window_and_limits(self) -> None:
        status, body = self.read("src/main/java/frc/robot/Constants.java")
        self.assertEqual(status, 200)
        self.assertEqual(body["total"], 7)
        self.assertEqual((body["start"], body["end"]), (1, 7))
        self.assertTrue(body["text"].startswith("package frc.robot;\n"))

        _, body = self.read("src/main/java/frc/robot/Robot.java", start=10, end=12)
        self.assertEqual((body["start"], body["end"]), (10, 12))
        self.assertEqual(body["text"].count("\n"), 3)
        self.assertIn("line 10:", body["text"])

        _, body = self.read("src/main/java/frc/robot/Robot.java", start=1, end=100000)
        self.assertEqual(body["end"], 400)  # ≤ 400 lines per call
        self.assertEqual(body["total"], 500)

        _, body = self.read("src/main/java/frc/robot/Robot.java", start=450, end=900)
        self.assertEqual(body["end"], 500)  # end clamps to the file

        _, body = self.read("src/main/java/frc/robot/Robot.java", start=600)
        self.assertEqual((body["text"], body["end"]), ("", 500))

    def test_read_byte_limit(self) -> None:
        (self.repo / "wide.txt").write_text(("x" * 199 + "\n") * 300)  # 60 KB
        _, body = self.read("wide.txt", start=1, end=300)
        self.assertLessEqual(len(body["text"].encode()), 24 * 1024)
        self.assertEqual(body["end"], 24 * 1024 // 200)

    def test_read_binary_refused(self) -> None:
        self.assertEqual(self.read("logo.bin.dat")[0], 415)

    def test_search(self) -> None:
        status, body = self.request("GET", "/code/search?q=kelevatorp")
        self.assertEqual(status, 200)
        self.assertEqual(len(body["matches"]), 1)
        m = body["matches"][0]
        self.assertEqual((m["path"], m["line"]), ("src/main/java/frc/robot/Constants.java", 4))
        self.assertIn("kElevatorP = 0.8;", m["text"])
        _, body = self.request("GET", "/code/search?q=kelevatorp&case=1")
        self.assertEqual(body["matches"], [])

    def test_search_never_looks_in_denied_places(self) -> None:
        for q in ("hunter2", "password", "BEGIN KEY", "output", "repositoryformatversion", "the robot's"):
            _, body = self.request("GET", f"/code/search?q={quote(q)}")
            self.assertEqual(body["matches"], [], q)

    def test_search_limits(self) -> None:
        _, body = self.request("GET", "/code/search?q=TimedRobot&max=5")
        self.assertEqual(len(body["matches"]), 5)
        self.assertTrue(body["truncated"])
        _, body = self.request("GET", "/code/search?q=TimedRobot&max=100000")
        self.assertEqual(len(body["matches"]), 200)
        _, body = self.request("GET", "/code/search?q=kArmP&path=vendordeps")
        self.assertEqual(body["matches"], [])
        self.assertEqual(self.request("GET", "/code/search?q=")[0], 400)
        self.assertEqual(self.request("GET", f"/code/search?q={'x' * 201}")[0], 400)


if __name__ == "__main__":
    unittest.main()
