"""Work orders (HTTP, the file format, the CLI, the hook) and file uploads."""
from __future__ import annotations

import contextlib
import io
import json
import os
import re
import sys
import time
import unittest
from unittest import mock

from helpers import LinkCase

from catalyst_link import cli
from catalyst_link.files import sanitize
from catalyst_link.inbox import parse_front

ORDER = {
    "title": "Elevator overshoots at L4",
    "body": "Overshoots by ~6 cm, then settles.\n\n- happens with a full battery\n- `kElevatorP` is 0.8",
    "kind": "bug", "priority": "high",
    "robot": {"team": 5805, "battery": 12.6, "mode": "disabled"},
    "files": ["run-20260923-141003.csv"], "from": "catalyst-tab",
}


def run_cli(*args: str) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main(list(args))
    return code, out.getvalue(), err.getvalue()


class InboxTest(LinkCase):
    def setUp(self) -> None:
        super().setUp()
        env = mock.patch.dict(os.environ, {"CATALYST_LINK_HOME": str(self.state.home)})
        env.start()
        self.addCleanup(env.stop)

    def create(self, **changes: object) -> dict:
        status, body = self.request("POST", "/inbox", {**ORDER, **changes})
        self.assertEqual(status, 200, body)
        return body

    def test_create_writes_markdown_with_front_matter(self) -> None:
        body = self.create()
        self.assertRegex(body["id"], r"^wo-\d{8}-\d{6}-elevator-overshoots-at-l4$")
        self.assertEqual(body["path"], f"inbox/{body['id']}.md")
        text = (self.state.home / body["path"]).read_text()
        meta, rest = parse_front(text)
        self.assertEqual(meta["id"], body["id"])
        self.assertEqual((meta["title"], meta["kind"], meta["priority"], meta["status"]),
                         ("Elevator overshoots at L4", "bug", "high", "open"))
        self.assertEqual(meta["files"], ["run-20260923-141003.csv"])
        self.assertIsNone(meta["patch"])
        self.assertTrue(meta["created"])
        self.assertIn("# Elevator overshoots at L4", rest)
        self.assertIn("- `kElevatorP` is 0.8", rest)
        self.assertIn('"battery": 12.6', rest)
        self.assertIn(str(self.state.files_dir / "run-20260923-141003.csv"), rest)

    def test_list_show_and_transitions(self) -> None:
        a = self.create()["id"]
        b = self.create(title="Tune the arm", kind="tune", priority="low", body="b")["id"]
        _, listing = self.request("GET", "/inbox?status=open")
        self.assertEqual({i["id"] for i in listing["items"]}, {a, b})
        item = listing["items"][0]
        self.assertEqual(set(item), {"id", "title", "kind", "priority", "status", "when"})
        self.assertRegex(item["when"], r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$")
        self.assertEqual(self.request("GET", "/link/status")[1]["inbox_open"], 2)

        status, got = self.request("GET", f"/inbox/{a}")
        self.assertEqual(status, 200)
        self.assertEqual(got["item"]["body"], ORDER["body"])
        self.assertEqual(got["item"]["robot"], ORDER["robot"])
        self.assertEqual(got["item"]["notes"], "")

        self.assertEqual(self.request("POST", f"/inbox/{a}/status", {"status": "claimed", "note": "on it"})[0], 200)
        self.assertEqual(self.request("POST", f"/inbox/{a}/status", {"status": "claimed"})[0], 409)  # no double claims
        self.assertEqual(self.request("POST", f"/inbox/{a}/status", {"status": "open"})[0], 400)  # the CLI's job
        self.assertEqual(self.request("POST", f"/inbox/{a}/status", {"status": "done", "note": "kP 0.8 → 1.0 on tab/x"})[0], 200)
        self.assertEqual(self.request("POST", f"/inbox/{a}/status", {"status": "rejected"})[0], 409)  # done is final
        _, got = self.request("GET", f"/inbox/{a}")
        self.assertEqual(got["item"]["status"], "done")
        self.assertIn("· claimed · on it", got["item"]["notes"])
        self.assertIn("· done · kP 0.8 → 1.0 on tab/x", got["item"]["notes"])
        self.assertEqual(got["item"]["body"], ORDER["body"])  # notes never leak into the body

        _, listing = self.request("GET", "/inbox?status=open")
        self.assertEqual([i["id"] for i in listing["items"]], [b])
        _, listing = self.request("GET", "/inbox")
        self.assertEqual(len(listing["items"]), 2)
        self.assertEqual(self.request("GET", "/link/status")[1]["inbox_open"], 1)
        self.assertEqual(self.request("GET", "/inbox/wo-nope")[0], 404)

    def test_validation_and_duplicates(self) -> None:
        self.assertEqual(self.request("POST", "/inbox", {**ORDER, "kind": "wish"})[0], 400)
        self.assertEqual(self.request("POST", "/inbox", {**ORDER, "priority": "urgent"})[0], 400)
        self.assertEqual(self.request("POST", "/inbox", {**ORDER, "title": " "})[0], 400)
        self.assertEqual(self.request("POST", "/inbox", {**ORDER, "patch": "../../etc"})[0], 400)
        first = self.create()
        again = self.create()
        self.assertEqual(again["id"], first["id"])
        self.assertTrue(again["duplicate"])
        self.assertEqual(len(list(self.state.inbox_dir.glob("*.md"))), 1)

    def test_linked_patch_names_its_branch(self) -> None:
        _, patch = self.request("POST", "/code/patch", {
            "title": "Raise elevator kP", "summary": "s",
            "edits": [{"path": "src/main/java/frc/robot/Constants.java", "old": "kElevatorP = 0.8;",
                       "new": "kElevatorP = 1.1;"}]})
        wid = self.create(patch=patch["id"])["id"]
        item = self.request("GET", f"/inbox/{wid}")[1]["item"]
        self.assertEqual((item["patch"], item["branch"]), (patch["id"], patch["branch"]))
        code, out, _ = run_cli("show", wid)
        self.assertEqual(code, 0)
        self.assertRegex(out, rf"git diff [0-9a-f]{{10}}\.\.\.{re.escape(patch['branch'])}")

    def test_cli_flow(self) -> None:
        wid = self.create()["id"]
        code, out, _ = run_cli("inbox")
        self.assertEqual(code, 0)
        self.assertIn(wid, out)
        code, out, _ = run_cli("inbox", "--json")
        self.assertEqual(json.loads(out)[0]["id"], wid)
        code, out, _ = run_cli("show", wid[:12] + wid[12:20])  # a unique prefix will do
        self.assertIn("Elevator overshoots at L4", out)
        self.assertEqual(run_cli("claim", wid)[0], 0)
        self.assertEqual(run_cli("claim", wid)[0], 1)
        self.assertEqual(run_cli("release", wid)[0], 0)
        self.assertEqual(run_cli("claim", wid, "--note", "agent-1")[0], 0)
        with self.assertRaises(SystemExit):  # done needs a note
            run_cli("done", wid)
        self.assertEqual(run_cli("done", wid, "--note", "raised kP on robot/elevator-kp; tests pass")[0], 0)
        meta, _ = parse_front((self.state.inbox_dir / f"{wid}.md").read_text())
        self.assertEqual(meta["status"], "done")
        code, out, _ = run_cli("inbox", "--status", "done", "--json")
        self.assertEqual(json.loads(out)[0]["status"], "done")
        self.assertEqual(run_cli("reject", "wo-missing", "--note", "x")[0], 1)

    def test_hand_edited_front_matter_still_parses(self) -> None:
        wid = self.create()["id"]
        path = self.state.inbox_dir / f"{wid}.md"
        path.write_text(path.read_text().replace('status: "open"', "status: claimed"))
        self.assertEqual(self.request("GET", f"/inbox/{wid}")[1]["item"]["status"], "claimed")

    def test_cli_token_and_patches(self) -> None:
        code, out, _ = run_cli("token")
        self.assertEqual(out.strip(), self.token)
        code, out, _ = run_cli("token", "--rotate")
        self.assertNotEqual(out.strip(), self.token)
        self.assertRegex(out.strip(), r"^[a-z2-9]{4}(-[a-z2-9]{4}){3}$")
        code, out, _ = run_cli("patches", "--json")
        self.assertEqual(json.loads(out), [])


class HookTest(LinkCase):
    # The PC's own command; the Link substitutes only the new work order's (quoted) path.
    on_work_order = f"{sys.executable} -c \"import sys,shutil; shutil.copy(sys.argv[1], sys.argv[1] + '.seen')\" {{path}}"

    def test_hook_runs_detached_with_the_path(self) -> None:
        _, body = self.request("POST", "/inbox", ORDER)
        seen = self.state.home / (body["path"] + ".seen")
        deadline = time.monotonic() + 10
        while not seen.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(seen.exists())
        self.assertTrue(any(r.get("kind") == "hook" and r.get("id") == body["id"] for r in self.log_lines()))


class FilesTest(LinkCase):
    def upload(self, name: str, data: bytes) -> tuple[int, dict]:
        return self.request("POST", f"/files?name={name}", data, headers={"Content-Type": "application/octet-stream"})

    def test_sanitize(self) -> None:
        self.assertEqual(sanitize("run-20260923-141003.csv"), "run-20260923-141003.csv")
        self.assertEqual(sanitize("../../etc/passwd"), "passwd")
        self.assertEqual(sanitize("..\\..\\boot.ini"), "boot.ini")
        self.assertEqual(sanitize(".bashrc"), "bashrc")
        self.assertEqual(sanitize("clip 1 (final).h264"), "clip_1_final_.h264")
        self.assertEqual(sanitize("żółw.log"), "_w.log")
        self.assertEqual(sanitize(""), "file")
        self.assertEqual(sanitize(".."), "file")
        self.assertLessEqual(len(sanitize("a" * 500 + ".csv")), 120)

    def test_upload_list_and_dedupe(self) -> None:
        status, body = self.upload("run-20260923-141003.csv", b"t,v\n0,12.4\n")
        self.assertEqual(status, 200, body)
        self.assertEqual((body["name"], body["path"], body["bytes"]),
                         ("run-20260923-141003.csv", "files/run-20260923-141003.csv", 11))
        self.assertEqual((self.state.files_dir / "run-20260923-141003.csv").read_bytes(), b"t,v\n0,12.4\n")

        _, again = self.upload("run-20260923-141003.csv", b"t,v\n0,12.4\n")  # a re-send: same bytes
        self.assertEqual(again["name"], "run-20260923-141003.csv")
        self.assertTrue(again["duplicate"])
        _, other = self.upload("run-20260923-141003.csv", b"t,v\n0,11.9\n")  # a different file, same name
        self.assertEqual(other["name"], "run-20260923-141003-2.csv")
        _, third = self.upload("run-20260923-141003.csv", b"t,v\n0,11.1\n")
        self.assertEqual(third["name"], "run-20260923-141003-3.csv")

        _, evil = self.upload("..%2F..%2Fescape.sh", b"#!/bin/sh\n")
        self.assertEqual(evil["name"], "escape.sh")
        self.assertTrue((self.state.files_dir / "escape.sh").exists())
        self.assertFalse((self.tmp / "escape.sh").exists())

        _, listing = self.request("GET", "/files")
        self.assertEqual({f["name"] for f in listing["files"]},
                         {"run-20260923-141003.csv", "run-20260923-141003-2.csv", "run-20260923-141003-3.csv", "escape.sh"})
        self.assertEqual(self.request("GET", "/link/status")[1]["files"], 4)
        self.assertTrue(any(r.get("path") == "/files" and r.get("name") == "escape.sh" for r in self.log_lines()))

    def test_binary_and_limits(self) -> None:
        clip = bytes(range(256)) * 4096  # 1 MB of H.264-ish bytes
        status, body = self.upload("clip.h264", clip)
        self.assertEqual((status, body["bytes"]), (200, len(clip)))
        self.assertEqual((self.state.files_dir / "clip.h264").read_bytes(), clip)
        status, body = self.request("POST", "/files?name=big.bin", b"",
                                    headers={"Content-Length": str(65 * 1024 * 1024)}, raw=True)
        self.assertEqual(status, 413)


if __name__ == "__main__":
    unittest.main()
