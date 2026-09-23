# Catalyst Link — the HTTP API between the tablet and the PC

Catalyst Link (`tab5/link/`) is a small Python service on the laptop that holds the robot's code.
The tablet talks to it over plain HTTP on the pit LAN (or the Wi-Fi the laptop shares). This page is
the contract between `components/assist/src/link.c` (the tablet) and `link/catalyst_link.py` (the PC).

## Discovery and auth

- Default port **8765**. The Link advertises `_catalyst-link._tcp` over mDNS with TXT `name=<pc>`.
- Every request carries `X-Link-Token: <token>`. The Link prints its token (and writes it to
  `~/.catalyst-link/token`) on first start; the technician types it into the tablet's settings once.
  A missing or wrong token gets `401 {"error":"token"}`. `GET /link/status` answers without a token
  but then reports only `{"name","version","auth":false}`.
- Bodies are JSON (`Content-Type: application/json`), UTF-8. Errors are `{"ok":false,"error":"…"}`
  with a 4xx/5xx status.

## Status

`GET /link/status` →
```json
{"ok":true,"name":"thomas-laptop","version":"1.0.0","auth":true,
 "repo":"CatalystX1","branch":"main","dirty":false,
 "claude":true, "inbox_open":2, "patches":1, "files":4}
```
`claude` is true when the Link can forward the Messages API (it has `ANTHROPIC_API_KEY`).

## Claude, through the Link

`POST /v1/messages` — the body is a Messages API request exactly as the tablet would send it to
`api.anthropic.com` (it always sets `"stream": true`). Request headers the tablet sends and the Link
honours: `anthropic-beta` (comma-separated; passed through as the SDK's `betas`). The Link adds the
key and calls the official Python SDK; the response is `text/event-stream` in the Messages API's own
SSE wire format (`event: message_start` / `content_block_start` / `content_block_delta` /
`content_block_stop` / `message_delta` / `message_stop`, and `event: error` on failure), so the
tablet parses one format whichever route it takes. The key never reaches the tablet.

## Code (read-only)

All paths are relative to the repo root; anything resolving outside it, into `.git`, or matching the
Link's deny list (`*.env`, keys, `build/`, `.gradle/`) is refused with 403.

- `GET /code/tree?path=src/main/java&depth=2` →
  `{"ok":true,"entries":[{"path":"src/main/java/frc/robot/Robot.java","type":"file","size":4210}]}`
  (≤ 400 entries; `"truncated":true` past that)
- `GET /code/read?path=…&start=1&end=200` →
  `{"ok":true,"path":"…","start":1,"end":200,"total":431,"text":"…"}` (≤ 400 lines and ≤ 24 KB per
  call; `end` clamps; lines are returned without numbers)
- `GET /code/search?q=kElevatorP&path=src&max=40` →
  `{"ok":true,"matches":[{"path":"…","line":37,"text":"  public static final double kElevatorP = 0.8;"}]}`
  (plain substring, case-insensitive unless `case=1`; ≤ 200 matches)

## Patches — the only way code changes, and never on the branch you're on

`POST /code/patch`
```json
{"title":"Raise elevator kP","summary":"Elevator lags its goal by 4 cm at the top; …",
 "edits":[{"path":"src/main/java/frc/robot/Constants.java",
           "old":"kElevatorP = 0.8;","new":"kElevatorP = 1.1;"}],
 "robot":{"team":5805,"battery":12.4,"mode":"disabled"}, "from":"catalyst-tab"}
```
→ `{"ok":true,"id":"p-20260923-141205-raise-elevator-kp","branch":"tab/20260923-141205-raise-elevator-kp",
"files":["…/Constants.java"],"diffstat":"1 file changed, 1 insertion(+), 1 deletion(-)",
"diff":"--- a/…\n+++ b/…\n@@ …","check":{"ran":true,"ok":true,"seconds":38.2,"tail":"BUILD SUCCESSFUL"}}`

Rules the Link enforces, whatever the tablet sends:
- The patch lands on a **new** branch `tab/<id>` created from the current `HEAD`, in its own git
  worktree under `~/.catalyst-link/worktrees/`. The checked-out branch and working tree are never
  touched; nothing is pushed; nothing is deployed.
- Each edit's `old` must occur **exactly once** in the file (else 409 with which edit and why).
  `old: ""` with a path that doesn't exist creates a file. No deletions of files; no edits outside the
  repo, in `.git`, or on deny-listed paths; ≤ 20 edits and ≤ 64 KB per patch.
- It commits as `Catalyst Tab <tab@catalyst.local>` with the title, the summary and the robot
  snapshot in the message, and writes `patches/<id>.json` + `.diff`.
- If a compile check is configured (`--check "./gradlew compileJava"`), it runs in the worktree with a
  timeout and its result is reported; a failing check still keeps the branch (marked `check: failed`).

`GET /code/patches` → `{"ok":true,"patches":[{"id","title","branch","status","when","check"}]}` where
`status` is `proposed`, `merged` (the branch is now an ancestor of HEAD) or `dropped` (deleted).

## Work orders — the inbox the PC's own agent works through

`POST /inbox`
```json
{"title":"Elevator overshoots at L4","body":"markdown…","kind":"bug|task|tune|question",
 "priority":"low|normal|high","robot":{…snapshot…},"patch":"p-…","files":["run-20260923-141003.csv"],
 "from":"catalyst-tab"}
```
→ `{"ok":true,"id":"wo-20260923-141300-elevator-overshoots","path":"inbox/wo-….md"}`

Each work order is a Markdown file with YAML front matter (`id, title, kind, priority, status: open,
created, patch, files`) and the body plus the robot snapshot, in `~/.catalyst-link/inbox/`. The PC's
agent (Claude Code, or anything) reads `link/AGENT.md` for how to work the inbox: claim one by setting
`status: claimed`, do the work on its own branch, set `status: done` with a note. The Link can also
run a configured command on each new work order (`--on-work-order "claude -p …"`), off by default.

- `GET /inbox?status=open` → `{"ok":true,"items":[{"id","title","kind","priority","status","when"}]}`
- `GET /inbox/<id>` → `{"ok":true,"item":{…,"body":"…","notes":"…"}}`
- `POST /inbox/<id>/status` `{"status":"claimed|done|rejected","note":"…"}` → `{"ok":true}`

## Files

`POST /files?name=run-20260923-141003.csv` with the raw bytes as the body (any content type, ≤ 64 MB)
→ `{"ok":true,"name":"run-20260923-141003.csv","path":"files/run-….csv","bytes":48213}`. Names are
sanitised to `[A-Za-z0-9._-]`; an existing name gets a `-2` suffix. `GET /files` lists them.
