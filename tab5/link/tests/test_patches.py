"""POST /code/patch and GET /code/patches: branches in their own worktrees, never the checked-out one."""
from __future__ import annotations

import json
import subprocess
import sys
import unittest

from helpers import LinkCase, git

CONST = "src/main/java/frc/robot/Constants.java"
PATCH = {
    "title": "Raise elevator kP",
    "summary": "Elevator lags its goal by 4 cm at the top.",
    "edits": [{"path": CONST, "old": "kElevatorP = 0.8;", "new": "kElevatorP = 1.1;"}],
    "robot": {"team": 5805, "battery": 12.4, "mode": "disabled"},
    "from": "catalyst-tab",
}


class PatchCase(LinkCase):
    def patch(self, body: dict | None = None, **changes: object) -> tuple[int, dict]:
        return self.request("POST", "/code/patch", {**(body or PATCH), **changes})

    def branches(self) -> list[str]:
        out = git(self.repo, "for-each-ref", "--format=%(refname:short)", "refs/heads/")
        return out.split()


class PatchSuccessTest(PatchCase):
    def test_patch_lands_on_a_new_branch_only(self) -> None:
        head_before = git(self.repo, "rev-parse", "HEAD")
        status, body = self.patch()
        self.assertEqual(status, 200, body)
        self.assertTrue(body["ok"])
        self.assertRegex(body["id"], r"^p-\d{8}-\d{6}-raise-elevator-kp$")
        self.assertEqual(body["branch"], "tab/" + body["id"][2:])
        self.assertEqual(body["files"], [CONST])
        self.assertEqual(body["diffstat"], "1 file changed, 1 insertion(+), 1 deletion(-)")
        self.assertIn("-  public static final double kElevatorP = 0.8;", body["diff"])
        self.assertIn("+  public static final double kElevatorP = 1.1;", body["diff"])
        self.assertEqual(body["check"], {"ran": False})

        # The branch exists, one commit on top of HEAD, authored by the tablet.
        self.assertIn(body["branch"], self.branches())
        self.assertEqual(git(self.repo, "rev-parse", f"{body['branch']}^"), head_before)
        self.assertEqual(git(self.repo, "log", "-1", "--format=%an <%ae>|%cn <%ce>", body["branch"]),
                         "Catalyst Tab <tab@catalyst.local>|Catalyst Tab <tab@catalyst.local>")
        message = git(self.repo, "log", "-1", "--format=%B", body["branch"])
        self.assertTrue(message.startswith("Raise elevator kP\n\nElevator lags its goal by 4 cm at the top."))
        self.assertIn('"team": 5805', message)
        self.assertIn('"battery": 12.4', message)
        self.assertIn(f"Link-Patch: {body['id']}", message)
        self.assertIn("kElevatorP = 1.1;", git(self.repo, "show", f"{body['branch']}:{CONST}"))

        # The checked-out branch and the working tree are untouched.
        self.assertEqual(git(self.repo, "symbolic-ref", "--short", "HEAD"), "main")
        self.assertEqual(git(self.repo, "rev-parse", "HEAD"), head_before)
        self.assertEqual(git(self.repo, "status", "--porcelain"), "")
        self.assertIn("kElevatorP = 0.8;", (self.repo / CONST).read_text())

        # Its worktree is under the Link's home, and its record and diff are written.
        wt = self.state.worktrees_dir / body["id"]
        self.assertIn("kElevatorP = 1.1;", (wt / CONST).read_text())
        rec = json.loads((self.state.patches_dir / f"{body['id']}.json").read_text())
        self.assertEqual((rec["branch"], rec["base"], rec["robot"]["team"]), (body["branch"], head_before, 5805))
        self.assertEqual((self.state.patches_dir / f"{body['id']}.diff").read_text(), body["diff"])

        # Listed, counted and audited.
        _, listing = self.request("GET", "/code/patches")
        self.assertEqual(listing["patches"][0]["id"], body["id"])
        self.assertEqual(listing["patches"][0]["status"], "proposed")
        self.assertEqual(listing["patches"][0]["check"], "none")
        self.assertEqual(self.request("GET", "/link/status")[1]["patches"], 1)
        audit = [r for r in self.log_lines() if r.get("path") == "/code/patch"]
        self.assertEqual((audit[-1]["status"], audit[-1]["id"]), (200, body["id"]))

    def test_new_file_and_multiple_edits(self) -> None:
        new = "src/main/java/frc/robot/subsystems/Intake.java"
        status, body = self.patch(title="Add intake", edits=[
            {"path": new, "old": "", "new": "package frc.robot.subsystems;\n\npublic class Intake {}\n"},
            {"path": CONST, "old": "kArmP = 0.8;", "new": "kArmP = 0.9;"},
            {"path": CONST, "old": "kElevatorI = 0.0;", "new": "kElevatorI = 0.01;"},
            {"path": new, "old": "Intake {}", "new": "Intake {\n}"},
        ])
        self.assertEqual(status, 200, body)
        self.assertEqual(sorted(body["files"]), sorted([CONST, new]))
        self.assertIn("public class Intake {\n}", git(self.repo, "show", f"{body['branch']}:{new}"))
        const = git(self.repo, "show", f"{body['branch']}:{CONST}")
        self.assertIn("kArmP = 0.9;", const)
        self.assertIn("kElevatorI = 0.01;", const)
        self.assertFalse((self.repo / new).exists())

    def test_crlf_file_keeps_its_line_endings(self) -> None:
        path = "src/main/java/frc/robot/Crlf.java"
        status, body = self.patch(title="crlf", edits=[{"path": path, "old": "  int a = 1;\n  int b = 2;",
                                                        "new": "  int a = 10;\n  int b = 20;"}])
        self.assertEqual(status, 200, body)
        blob = subprocess.run(["git", "-C", str(self.repo), "show", f"{body['branch']}:{path}"],
                              capture_output=True, check=True).stdout
        self.assertEqual(blob, b"class Crlf {\r\n  int a = 10;\r\n  int b = 20;\r\n}\r\n")

    def test_patches_start_from_head_not_the_dirty_tree(self) -> None:
        (self.repo / CONST).write_text((self.repo / CONST).read_text().replace("0.8;", "0.9;", 1))
        status, body = self.patch(edits=[{"path": CONST, "old": "kElevatorP = 0.9;", "new": "kElevatorP = 1.0;"}])
        self.assertEqual(status, 409)
        self.assertIn("uncommitted", body["error"])
        status, body = self.patch()
        self.assertEqual(status, 200)
        self.assertIn("kElevatorP = 0.9;", (self.repo / CONST).read_text())  # the team's edit survives

    def test_resent_patch_is_answered_once(self) -> None:
        _, first = self.patch()
        status, again = self.patch()
        self.assertEqual(status, 200)
        self.assertEqual(again["id"], first["id"])
        self.assertTrue(again["duplicate"])
        self.assertEqual(len([b for b in self.branches() if b.startswith("tab/")]), 1)

    def test_request_cannot_choose_a_command(self) -> None:
        marker = self.tmp / "pwned"
        status, body = self.patch(check=f"touch {marker}", on_work_order=f"touch {marker}")
        self.assertEqual(status, 200)
        self.assertEqual(body["check"], {"ran": False})
        self.assertFalse(marker.exists())


class PatchRejectionTest(PatchCase):
    def assertRejected(self, status: int, edits: list[dict], why: str | None = None, **extra: object) -> dict:
        got, body = self.patch(edits=edits, **extra)
        self.assertEqual(got, status, body)
        self.assertFalse(body["ok"])
        if why:
            self.assertEqual(body["why"], why)
        return body

    def test_rejections_leave_nothing_behind(self) -> None:
        body = self.assertRejected(409, [{"path": "src/main/java/frc/robot/Dupes.java", "old": "int x = 1;", "new": "y"}],
                                   "not_unique")
        self.assertEqual(body["edit"], 0)
        self.assertIn("2 times", body["error"])
        body = self.assertRejected(409, [PATCH["edits"][0], {"path": CONST, "old": "kNope = 1;", "new": "x"}], "not_found")
        self.assertEqual(body["edit"], 1)
        self.assertRejected(409, [{"path": "src/Nope.java", "old": "a", "new": "b"}], "missing_file")
        self.assertRejected(409, [{"path": CONST, "old": "", "new": "whole file"}], "exists")
        self.assertRejected(409, [{"path": CONST, "old": "0.8;", "new": "0.8;"}, ], "not_unique")
        self.assertRejected(409, [{"path": CONST, "old": "kArmP = 0.8;", "new": "kArmP = 0.8;"}], "no_change")
        self.assertRejected(413, [{"path": CONST, "old": "kArmP", "new": "kArmP"}] * 21)
        self.assertRejected(413, [{"path": CONST, "old": "kArmP = 0.8;", "new": "x" * 70000}])
        for denied in (".env", "config/secrets.json", "deploy.key", "build/libs.txt", "build/new.txt",
                       ".gradle/x", "bin/x", ".git/config", ".git/hooks/pre-commit", "../outside.txt",
                       "/etc/passwd", "escape.txt", "gitconfig-link"):
            self.assertRejected(403, [{"path": denied, "old": "", "new": "x"}], "path")
        self.assertRejected(400, [{"path": CONST, "old": "a"}])
        self.assertEqual(self.patch(title="")[0], 400)
        self.assertEqual(self.request("POST", "/code/patch", b"not json")[0], 400)

        self.assertEqual([b for b in self.branches() if b.startswith("tab/")], [])
        self.assertEqual(list(self.state.worktrees_dir.iterdir()), [])
        self.assertEqual(list(self.state.patches_dir.iterdir()), [])
        self.assertEqual(git(self.repo, "status", "--porcelain"), "")
        self.assertFalse((self.repo / "build/new.txt").exists())


class PatchStatusTest(PatchCase):
    def test_merged_and_dropped(self) -> None:
        _, merged = self.patch()
        _, dropped = self.patch(title="Something else",
                                edits=[{"path": CONST, "old": "kArmP = 0.8;", "new": "kArmP = 1.2;"}])
        git(self.repo, "merge", "-q", "--no-ff", "-m", "take the tablet's kP", merged["branch"])
        git(self.repo, "worktree", "remove", "--force", str(self.state.worktrees_dir / dropped["id"]))
        git(self.repo, "branch", "-D", dropped["branch"])
        _, listing = self.request("GET", "/code/patches")
        status = {p["id"]: p["status"] for p in listing["patches"]}
        self.assertEqual(status, {merged["id"]: "merged", dropped["id"]: "dropped"})
        self.assertEqual(self.request("GET", "/link/status")[1]["patches"], 0)


class CheckPassTest(PatchCase):
    # Runs in the patch's worktree, so it sees the patched file.
    check = (f"{sys.executable} -c \"import pathlib,sys; t=pathlib.Path('{CONST}').read_text(); "
             "print('compiling'); print('BUILD SUCCESSFUL'); sys.exit(0 if '1.1' in t else 1)\"")

    def test_check_passes(self) -> None:
        status, body = self.patch()
        self.assertEqual(status, 200, body)
        check = body["check"]
        self.assertTrue(check["ran"] and check["ok"])
        self.assertEqual(check["exit"], 0)
        self.assertTrue(check["tail"].endswith("BUILD SUCCESSFUL"))
        self.assertEqual(self.request("GET", "/code/patches")[1]["patches"][0]["check"], "passed")


class CheckFailTest(PatchCase):
    check = "echo compiling; echo 'Constants.java:4: error: bad'; exit 3"

    def test_failed_check_keeps_the_branch(self) -> None:
        status, body = self.patch()
        self.assertEqual(status, 200, body)
        self.assertEqual((body["check"]["ran"], body["check"]["ok"], body["check"]["exit"]), (True, False, 3))
        self.assertIn("error: bad", body["check"]["tail"])
        self.assertIn(body["branch"], self.branches())
        self.assertEqual(self.request("GET", "/code/patches")[1]["patches"][0]["check"], "failed")


class CheckTimeoutTest(PatchCase):
    check = "echo starting; sleep 30"
    check_timeout = 1.0

    def test_check_times_out(self) -> None:
        status, body = self.patch()
        self.assertEqual(status, 200, body)
        check = body["check"]
        self.assertTrue(check["timed_out"])
        self.assertFalse(check["ok"])
        self.assertLess(check["seconds"], 10)
        self.assertIn("timed out", check["tail"])
        self.assertEqual(self.request("GET", "/code/patches")[1]["patches"][0]["check"], "timeout")


if __name__ == "__main__":
    unittest.main()
