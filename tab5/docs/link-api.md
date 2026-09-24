# Catalyst Link — the HTTP API between the tablet and the PC

Catalyst Link (`tab5/link/`) is a small Python service on the laptop that holds the robot's code.
The tablet talks to it over plain HTTP on the pit LAN (or the Wi-Fi the laptop shares). This page is
the contract between `components/assist/src/link.c` (the tablet) and the `catalyst_link` package in
`link/` (the PC; its README has the safety model, the CLI and how to run it).

## Discovery and auth

- Default port **8765**. The Link advertises `_catalyst-link._tcp` over mDNS with TXT `name=<pc>`.
- Every request carries `X-Link-Token: <token>`. The Link prints its token (and writes it to
  `~/.catalyst-link/token`) on first start; the technician types it into the tablet's settings once.
  A missing or wrong token gets `401 {"ok":false,"error":"token"}`. `GET /link/status` answers
  without a token (or with a wrong one) but then reports only `{"ok":true,"name","version","auth":false}`.
- Bodies are JSON (`Content-Type: application/json`), UTF-8, sent with a `Content-Length` (chunked
  uploads get 411). Errors are `{"ok":false,"error":"…"}` with a 4xx/5xx status — except on
  `/v1/messages`, which answers in the Messages API's own error format (below). Bodies are capped:
  1 MB of JSON, 32 MB for `/v1/messages`, 64 MB for an upload (413 past that).
- Every write request (and every refused token) is appended to `~/.catalyst-link/log.jsonl` with its
  outcome before the answer goes out.

## Status

`GET /link/status` →
```json
{"ok":true,"name":"thomas-laptop","version":"1.0.0","auth":true,
 "repo":"CatalystX1","branch":"main","dirty":false,
 "claude":true, "claude_via":"claude-code", "inbox_open":2, "patches":1, "files":4}
```
`claude` is true when the Link can serve `/v1/messages`; `claude_via` says how: `"api"` (it has
`ANTHROPIC_API_KEY`), `"claude-code"` (Claude Code on the PC, logged in with the owner's Claude
subscription; `claude` is false while that login is missing), or `null` (`--claude off`).
`inbox_open` counts work orders not yet finished (`open` + `claimed`); `patches` counts patches still
`proposed`; `dirty` means tracked files have uncommitted changes (patches start from `HEAD`, not from
them).

## Claude, through the Link

`POST /v1/messages` — one contract, two backends, chosen on the PC (`catalyst-link serve --claude
auto|api|claude-code|off`; `auto`, the default, is `api` when `ANTHROPIC_API_KEY` is set and
`claude-code` otherwise). The tablet sends and parses the same thing either way.

### `api`: the Messages API with the PC's key

The body is a Messages API request exactly as the tablet would send it to
`api.anthropic.com` (it always sets `"stream": true`). Request headers the tablet sends and the Link
honours: `anthropic-beta` (comma-separated; passed through as the SDK's `betas`). The Link adds the
key and calls the official Python SDK; the response is `text/event-stream` in the Messages API's own
SSE wire format (`event: message_start` / `content_block_start` / `content_block_delta` /
`content_block_stop` / `message_delta` / `message_stop`, and `event: error` on failure), so the
tablet parses one format whichever route it takes. The key never reaches the tablet.

- Body fields the installed SDK declares go to it as arguments; any it doesn't (a newer parameter)
  go through its `extra_body`, so the body reaches the API unchanged. `"stream"` is always true.
- An error before the stream starts answers with the API's HTTP status and its error object,
  `{"type":"error","error":{"type":"rate_limit_error","message":"…"}}` (the Link itself uses 503
  `api_error` when it has no key, 400 `invalid_request_error` for a body without `model`,
  `max_tokens` and `messages`, 502/504 when it can't reach the API). After the stream has started, a
  failure is one `event: error` with that same object, and the stream ends.
- Events are re-emitted as `event: <type>\ndata: <json>\n\n`, flushed one by one, with
  `Content-Type: text/event-stream`, no `Content-Length` and `Connection: close`. The SDK swallows
  the API's `ping`s, so the Link sends its own `event: ping` / `{"type":"ping"}` after every 10 s of
  upstream silence (ignore it, as on the direct route). If the tablet disconnects, the Link closes the
  upstream stream.

### `claude-code`: the owner's Claude subscription, no API key

The Link runs a Claude Code session through the Claude Agent SDK (`claude-agent-sdk`, which runs the
official `claude` CLI logged in with `claude login` or a `claude setup-token` token). The Link holds
no credential and never calls an Anthropic endpoint itself.

- **Tools stay on the tablet.** The request's `tools` become an MCP server inside the session and are
  its only tools (Claude Code's own shell, file and web tools are off; no settings, plugins, hooks,
  `CLAUDE.md` or other MCP servers load; prompts are delivered verbatim, so `@file` or `/command` in
  the technician's text does nothing; it runs in the empty folder `~/.catalyst-link/claude-code/`).
  When Claude calls one, the response streams its `tool_use` block (the name as the tablet declared it)
  and ends with `stop_reason: "tool_use"`, exactly as the API does; the tablet runs the tool — with the
  on-screen confirmation for anything that changes the robot — and posts the conversation with the
  `tool_result`s, and the same session carries on in that response. Parallel calls work the same way.
- **The stream** is Claude Code's own Messages API events, re-emitted in the format above
  (`message_start` … `message_stop`, `event: ping` every 10 s of silence). `message.model` is the model
  Claude Code used, which is Claude Code's default or `--claude-model`, not the body's `model`. An error
  is one `event: error` (`rate_limit_error` when the plan's limit is hit, `authentication_error` when
  the login lapsed), after which the session is discarded.
- **Sessions.** One per conversation. A request is matched to its session by the `tool_result` ids it
  carries, or, for a new question, by the text of the answer the tablet echoes back as the
  second-to-last message; only the new user message is sent. A conversation the Link doesn't hold (it
  restarted, the tablet trimmed or rolled back its history) starts a new session primed with a text
  transcript of the history. A dropped connection ends the session. Idle sessions close after 30 min,
  a session waiting on tool results after 15 min, and at most 4 are kept.
- **What doesn't apply:** `model`, `max_tokens`, `thinking`, `fallbacks`, `cache_control` and the
  `anthropic-beta` header. `output_config.effort` is passed to Claude Code.
- **Errors before the stream:** 503 `api_error` when the SDK or CLI is missing or Claude Code isn't
  logged in (the message says which); 400 `invalid_request_error` when `messages` doesn't end with a
  user message. The audit log gets one line per request with `"backend":"claude-code"`, how it was
  matched (`new`, `tool_results`, `next_question`, `replayed`), the model and token usage.

## Code (read-only)

All paths are relative to the repo root; anything resolving outside it (symlinks are followed), into
`.git`, or matching the Link's deny list is refused with 403 — and never listed or searched. The deny
list matches any path component: `*.env`, `.env*`, `*secret*`, `*.pem`, `*.key` (and other key and
keystore files), `build/`, `.gradle/`, `bin/`. Reads see the working tree as it is on disk.

- `GET /code/tree?path=src/main/java&depth=2` →
  `{"ok":true,"entries":[{"path":"src/main/java/frc/robot/Robot.java","type":"file","size":4210}]}`
  (≤ 400 entries, breadth first; `"truncated":true` past that. Folders are listed too, as
  `{"path","type":"dir"}`; `depth` defaults to 2, at most 10.)
- `GET /code/read?path=…&start=1&end=200` →
  `{"ok":true,"path":"…","start":1,"end":200,"total":431,"text":"…"}` (≤ 400 lines and ≤ 24 KB per
  call; `end` clamps, and says where the text actually stops; lines are returned without numbers and
  with their own line endings; past the end `text` is `""` and `end < start`; binary files are 415)
- `GET /code/search?q=kElevatorP&path=src&max=40` →
  `{"ok":true,"matches":[{"path":"…","line":37,"text":"  public static final double kElevatorP = 0.8;"}]}`
  (plain substring, case-insensitive unless `case=1`; `max` defaults to 40, at most 200;
  `"truncated":true` when it stopped at `max`; `q` ≤ 200 characters)

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
- The patch lands on a **new** branch `tab/<stamp>-<slug>` (the id without its `p-`) created from the
  current `HEAD` — not from the working tree's uncommitted changes — in its own git
  worktree under `~/.catalyst-link/worktrees/`. The checked-out branch and working tree are never
  touched; nothing is pushed; nothing is deployed.
- Each edit's `old` must occur **exactly once** in the file (else 409 with which edit and why:
  `{"ok":false,"error":"edit 2 (…/Constants.java): old text occurs 3 times…","edit":1,"why":"not_unique"}`
  — `edit` is 0-based; `why` is `not_found`, `not_unique`, `missing_file`, `exists` or `no_change`).
  Edits apply in order, so later ones see earlier ones' results. `old: ""` with a path that doesn't
  exist creates a file; `old: ""` on an existing file is refused. In a CRLF file, an `old` written with
  `\n` matches the file's `\r\n`. No deletions of files; no edits outside the repo, in `.git`, or on
  deny-listed paths (403, `"why":"path"`); ≤ 20 edits and ≤ 64 KB of `old` + `new` per patch (413).
- It commits as `Catalyst Tab <tab@catalyst.local>` with the title, the summary and the robot
  snapshot in the message, and writes `patches/<id>.json` + `.diff`.
- If a compile check is configured (`--check "./gradlew compileJava"`), it runs in the worktree with a
  timeout and its result is reported as `{"ran":true,"ok","seconds","exit","timed_out","tail"}`
  (`tail`: the last 20 lines); unconfigured, `check` is `{"ran":false}`. A failing check still keeps
  the branch (marked `check: failed`). The command comes only from the PC's command line.
- `diff` is capped at 64 KB (`"diff_truncated":true`; the whole diff is in `patches/<id>.diff`).
- A patch identical to one proposed in the last ten minutes (the outbox re-sending after a lost
  answer) returns that patch's answer with `"duplicate":true` instead of a second branch.

`GET /code/patches` → `{"ok":true,"patches":[{"id","title","branch","status","when","check"}]}` where
`status` is `proposed`, `merged` (the branch is now an ancestor of HEAD) or `dropped` (deleted), and
`check` is `none`, `passed`, `failed` or `timeout`. Newest first.

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

`kind` defaults to `task` and `priority` to `normal` (anything else outside the lists is 400);
`title` is required. A work order identical to one filed in the last ten minutes returns the first
one's id with `"duplicate":true`.

- `GET /inbox?status=open` → `{"ok":true,"items":[{"id","title","kind","priority","status","when"}]}`
  (newest first; `status` may be a comma list; without it, every item)
- `GET /inbox/<id>` → `{"ok":true,"item":{…front matter…,"when","body":"…","robot":{…},"notes":"…","path"}}`
  (`branch` is filled in when `patch` names a known patch; `notes` is one `- <when> · <status> · <note>`
  line per change)
- `POST /inbox/<id>/status` `{"status":"claimed|done|rejected","note":"…"}` → `{"ok":true}`.
  `open` → `claimed`/`done`/`rejected`, `claimed` → `done`/`rejected`; anything else is 409 (so an
  item can't be claimed twice, and `done`/`rejected` are final). Putting a claimed item back is the
  PC's (`catalyst-link release`).

## Files

`POST /files?name=run-20260923-141003.csv` with the raw bytes as the body (any content type, ≤ 64 MB)
→ `{"ok":true,"name":"run-20260923-141003.csv","path":"files/run-….csv","bytes":48213}`. Names are
sanitised to `[A-Za-z0-9._-]` (directories dropped, other characters → `_`, no leading dots); an
existing name gets a `-2` suffix (then `-3`…), unless the existing file has the very same bytes — a
re-sent upload — which returns that file with `"duplicate":true`. The answer also carries `sha256`.
`GET /files` → `{"ok":true,"files":[{"name","bytes","when"}]}`, newest first.
