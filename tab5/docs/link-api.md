# Catalyst Link — the HTTP API between the tablet and the PC

Catalyst Link (`tab5/link/`) is a small Python service on the laptop that holds the robot's code.
The tablet talks to it over plain HTTP on the pit LAN (or the Wi-Fi the laptop shares). This page is
the contract between `components/assist/src/link.c` (the tablet) and the `catalyst_link` package in
`link/` (the PC; its README has the safety model, the CLI and how to run it).

## Discovery and auth

- **This tablet's default.** `components/assist/src/link.c` compiles in one PC's host and port
  (`LINK_DEFAULT_HOST`/`LINK_DEFAULT_PORT`) because only one tablet and one PC exist today — `link_init()`
  falls back to it whenever kv has no saved address, so the tablet never needs mDNS or the "pair the pc"
  screen just to reach that PC. Only the host/port are baked in; the token never is (this repo is public).
  A `LINK_TOKEN` (and, to point at a different PC, `LINK_HOST`) in `KEYS.ENV` (`docs/keys.md`) writes into
  the same `link_url`/`link_token` kv keys pairing uses, and — like pairing, like typing an address in
  settings — always wins over the compiled default from then on.
- Default port **8765**. The Link advertises `_catalyst-link._tcp` over mDNS (with the optional
  `zeroconf` package) with TXT `name=<pc>`, `version`, `path=/link/status` and `pair=1|0`.
- Every request carries `X-Link-Token: <token>`. The Link writes its main token to
  `~/.catalyst-link/token` on first start. The tablet gets a token by **pairing** (below): the PC shows a
  six-digit code, the code typed on the tablet brings back a token of the tablet's own. Typing the main
  token by hand (the link app's "token" button) still works. A missing or wrong token gets `401 {"ok":false,"error":"token"}`.
  `GET /link/status` and the two pairing routes answer without a token; without one `/link/status`
  reports only `{"ok":true,"name","version","auth":false,"pairing":true|false}`.
- Bodies are JSON (`Content-Type: application/json`), UTF-8, sent with a `Content-Length` (chunked
  uploads get 411). Errors are `{"ok":false,"error":"…"}` with a 4xx/5xx status — except on
  `/v1/messages`, which answers in the Messages API's own error format (below). Bodies are capped:
  1 MB of JSON, 32 MB for `/v1/messages`, 64 MB for an upload (413 past that).
- Every write request (and every refused token) is appended to `~/.catalyst-link/log.jsonl` with its
  outcome before the answer goes out.

## Pairing

The tablet's "pair pc" app (Settings › home › pair the pc, the link app's "pair", or the app library)
browses mDNS for `_catalyst-link._tcp`, the owner picks a PC (or types its address), and:

1. `POST /link/pair` `{"device":"catalyst tab"}` (no token) →
   `{"ok":true,"pairing":"<id>","digits":6,"expires_in":120,"name":"<pc>"}`. The Link makes a six-digit
   code and shows it **on the PC only**: in the console `catalyst-link serve` runs in, and as a Windows
   notification (`serve --no-pair-toast` turns that off); under the desktop app (`serve --gui`), in the
   app's pairing panel instead of the console. The code never travels over the network.
2. The owner types the code on the tablet: `POST /link/pair/confirm` `{"pairing":"<id>","code":"482913"}`
   (spaces are ignored) → `{"ok":true,"token":"<a token for this tablet>","name":"<pc>"}`. The tablet saves the
   address and token (`link_url`, `link_token`) exactly as if they had been typed, and every later request
   carries the token — the assistant, the inbox and patches, and home mode's media remote
   (`/media/now`, `/media/art`, `/media/control` go through this same paired connection).

Errors: a wrong code is `403 {"ok":false,"error":"code","attempts_left":n}`; an unknown, used or expired
pairing (or the fifth wrong code) is `410 {"ok":false,"error":"expired","why":"expired|tries"}` — start
again for a new code; `429` when pairings come too fast; `403` with `serve --no-pair`.

The guard rails: one pairing waits at a time (a new request replaces it, and its code), a code lives two
minutes and takes five wrong tries, and at most one new pairing every 3 s and ten per 10 minutes. Six
digits and five tries is a 1-in-200,000 chance per code. Whoever reads the PC's screen is the one
pairing; a device elsewhere on the LAN can ask for a code but can't see it. The pairing routes are in
`log.jsonl` (`"pairing":"started"|"paired"`), never with the code or the token.

Each pairing hands out a **token of the tablet's own** (same format as the main token). The Link keeps
only its SHA-256, with the name the tablet gave, the address it paired from and when it was last seen, in
`~/.catalyst-link/devices.json`. Forgetting a tablet (the desktop app's pairing panel,
`POST /admin/devices/<id>/forget`) deletes that entry, and its token gets 401 from the next request on;
other tablets are untouched. The main token keeps working everywhere; `catalyst-link token --rotate`
makes a new one **and forgets every paired tablet**, so rotating still means "every tablet pairs again".
The LAN carries tokens in plain HTTP, as it always has.

## The desktop app's routes (`/admin/*`)

The Catalyst Link desktop app (`link/desktop/`) reads and acts through these. They answer only a
**loopback** client (`127.0.0.1`/`::1`) carrying the Link's **main** token, and never a request with an
`Origin` header (so no web page, even one on this PC, can reach them): a tablet, even a paired one,
gets 403 or 401. The app's Rust side makes the requests; the token never reaches its window.

| route | |
|---|---|
| `GET /admin/overview` | name, version, port, bind, addresses, repo (`repo_repo`, `repo_branch`, `repo_dirty`), state folder, pairing (`enabled`, `toast`, `pending` without the code), `devices`, `by_hand` (other machines using the main token, by address), `media`, `mdns` (`advertising` / `off` / `failed`, with `why`), `claude`, inbox counts, Claude Code sessions by state |
| `GET /admin/pairing` | `{"enabled","pending":{"id","device","ip","code","expires_in","ttl","tries_left"} or null,"last":{"device","ip","at"} or null}` — the only place a code is ever served |
| `POST /admin/pairing/cancel` | drop the waiting pairing |
| `GET /admin/devices` | the paired tablets: `id`, `name`, `ip`, `paired_at`, `last_seen` (never a token or digest) |
| `POST /admin/devices/<id>/forget` | revoke one tablet's token |
| `POST /admin/inbox/<id>/status` | `{"status","note"}` (status `open`, `claimed`, `done` or `rejected`): as the tablet's route, plus putting a claimed item back (`open`), which is the CLI's `release` |
| `GET /admin/claude-hooks` | whether Claude Code's user settings carry the Link's hooks: `installed`, `partial`, `outdated` (another Python, checkout or port), `path`, `command` |
| `POST /admin/claude-hooks` | `{"install":true}` or `{"install":false}`: add or remove only the Link's hook entries (backup kept as `settings.json.catalyst-link.bak`); 409 for a settings file that isn't valid JSON, which is left alone |

Requests from the app (its `User-Agent`, from this PC) and every `/admin` request are left out of the
console's per-request log lines, as Claude Code's hook traffic already is.

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

## Claude Code sessions — what Claude Code on the PC is doing

The tablet's companion (desk mode) shows what Claude Code is doing on the owner's PC: which session
is running, what it's doing, when it will likely finish, and when it has finished or needs input. The
Link learns this from Claude Code's own hooks, not by polling Claude Code — `link/catalyst_link/hook.py`
runs on each hook event and posts a trimmed copy here; `link/catalyst_link/sessions.py` keeps the
state and the finish estimate; the CLI has `hook`, `hook-settings` and `claude-sessions` (the PC-side
install steps are in `link/README.md`).

`POST /v1/claude/events` (token required; this is what `hook.py` calls) — body:

- `event` — the hook's name (`hook_event_name`): `UserPromptSubmit`, `PreToolUse`, `PostToolUse`,
  `PostToolUseFailure`, `Notification`, `Stop`, `StopFailure`, `SubagentStop`, `SessionStart`,
  `SessionEnd`, `PreCompact`. Required.
- `session_id` — required, at most 100 characters.
- `cwd` — the session's working folder.
- `prompt` — `UserPromptSubmit` only: the first non-empty line of what the owner typed, at most 160
  characters; left out when `CATALYST_LINK_HOOK_PROMPTS=0` (the tablet then shows "turn N" instead).
- `tool` and `detail` — `PreToolUse` / `PostToolUse` / `PostToolUseFailure`: the tool's name and a few
  words of what it's doing, e.g. `"editing Shooter.java"`, `"running ./gradlew build"`, `"searching
  for kArmP"`. Tool inputs and outputs never leave the PC — `detail` is `hook.py`'s own summary.
- `message` (+ `notification_type`) — `Notification`: the text Claude Code showed.
- `error` — `StopFailure` (and any event that carries one): what failed.
- `reason` — `SessionEnd`; `source` — `SessionStart`.

→ `{"ok":true,"state":"running","seq":3}`. 400 without `session_id` or `event`.

### The state machine

| event | state |
|---|---|
| `UserPromptSubmit` | `running` — a new turn, the clock starts |
| `PreToolUse` / `PostToolUse` / `PostToolUseFailure` | `running`; `step` becomes what the tool is doing |
| `Notification` | `waiting_for_input` — but only while `running` (a permission prompt, or a question); after a `Stop` it's Claude Code's own idle reminder and changes nothing |
| a tool event while `waiting_for_input` | back to `running` — whatever it was waiting for was answered |
| `Stop` | `done`; the turn's active time is recorded for future estimates |
| `StopFailure`, or any event carrying an `error` | `error` |
| `SubagentStop`, `PreCompact` | still `running`; `step` says so |
| `SessionEnd` while `running` or `waiting_for_input` | `done`, but the turn is **not** recorded (cut off, not finished) |
| `SessionEnd` (any state) | the session is also marked `ended` and leaves the list an hour later |
| `SessionStart` | no state change; `step` becomes "session started" if the session hasn't had a turn yet |

`seq` counts state changes, so a reader can tell "finished again" from "still finished". A session the
Link hasn't seen before starts in state `done` (nothing to look at yet, `seq` 0).

### `GET /v1/claude/sessions`

```json
{"ok":true,"now":"2026-09-24T10:03:12-05:00","turns_recorded":214,"events":5301,
 "sessions":[
   {"id":"sess-1","title":"Tune the arm's feedforward","cwd":"C:/dev/CatalystX1","project":"CatalystX1",
    "state":"running","seq":3,"step":"editing Shooter.java","tools":4,"turns":2,
    "started_at":"2026-09-24T09:41:03-05:00","turn_started_at":"2026-09-24T10:01:40-05:00",
    "last_activity":"2026-09-24T10:03:10-05:00","finished_at":null,
    "elapsed_s":90.4,"quiet_s":2.1,"since_s":12.0,
    "eta":{"remaining_s":140.0,"low_s":70.0,"high_s":260.0,"finish_at":"2026-09-24T10:05:32-05:00",
           "samples":4,"typical_s":230.0,
           "basis":"median of the 4 of your last 12 turns (in CatalystX1) that ran past 1m30s"},
    "eta_basis":"median of the 4 of your last 12 turns (in CatalystX1) that ran past 1m30s",
    "acked":false,"ended":false,"error":null}]}
```

Sessions are sorted by `last_activity`, newest first, at most 16. `done` sessions drop out of the list
24 hours after their last activity, `ended` ones after 1 hour; the list is in memory only (a restarted
Link starts empty), but the turn history behind the estimates persists in
`~/.catalyst-link/claude-turns.jsonl` (the last 500 turns). `eta` is `null` when there's no estimate;
`eta_basis` is the same explanation whether or not `eta` is present (also reachable inside `eta.basis`
when it is). `acked` is true when the session has been acknowledged since its last state change and
it isn't currently `running`.

### The estimate, honestly

The estimate only counts Claude's own active time — time spent waiting on the owner (`Notification`
while `running`, until the next tool call) is excluded, both from a turn's recorded duration and from
how far into the current turn it looks. For a turn `e` seconds into its active time, the estimate is
the median of the owner's recorded turns that ran longer than `e`, minus `e` — among turns that were
still going at this point, when did half of them finish. The low/high are those turns' quartiles.
Turns from the same folder are used when there are at least 5 of the last 40; otherwise all recent
turns, and `basis` says which. With fewer than 3 recorded turns to compare against, or fewer than 2
that ran at least this long, there is no estimate, and `eta_basis` says why (e.g. "no estimate yet: 1
finished turn recorded, 3 needed" or "no estimate: already longer than all of your last 5 turns").
Turns under 1 second aren't recorded (a slash command, a hook test, not a real turn). It is an
estimate from the owner's own history, not a prediction of this turn specifically, and the tablet
labels it as one.

### Acknowledging

`POST /v1/claude/sessions/<id>/ack` and `POST /v1/claude/sessions/ack` (every session) →
`{"ok":true,"acked":n}`. 404 for an unknown id.

These routes are routine traffic: unlike the rest of the API, they are not written to
`~/.catalyst-link/log.jsonl` and not printed per request.

### The tablet side

`components/assist/src/ccwatch.c` polls `GET /v1/claude/sessions` every ~3 seconds while the Link is
reachable (a 404, from an older Link without this endpoint, backs off to once a minute). When a
session changes to `done`, `waiting_for_input` or `error`, the tablet raises attention for it: the
eyes react, a chime plays, the island shows a message, and the reminder repeats — once, or every 1, 2
or 5 minutes (default 2) — until the owner taps the companion's face or that session's row, which
posts the acknowledgement back to the Link. The assistant also has a read-only tool, `claude_sessions`,
that reads this same endpoint.

## Media — what the PC is playing, for the tablet's home mode

The PC's now-playing and its transport controls, from Windows' Global System Media Transport Controls
(the volume flyout's media card: Spotify, YouTube Music in a browser or as an app, anything that reports
to it). `link/catalyst_link/media.py`; the extras are `pip install catalyst-link[media]` (the WinRT
projections, Pillow for album art, pycaw for a volume level). Without them, on another OS, or with
`serve --no-media`, the routes answer `"available": false` with the reason. `GET /link/status` carries
`"media": true|false`. These routes are routine traffic: not written to `log.jsonl`.

- `GET /media/now` →
  ```json
  {"ok":true,"available":true,"volume":0.42,"muted":false,
   "playing":{"title":"Midnight City","artist":"M83","album":"Hurry Up, We're Dreaming",
              "app":"Spotify","app_id":"Spotify.exe","state":"playing",
              "position":31.4,"duration":243.0,
              "can":{"play":false,"pause":true,"next":true,"previous":true},"art":"9f3c0a1b2c3d"}}
  ```
  `playing` is `null` with no media session. `state` is `playing`, `paused`, `stopped`, `changing`,
  `opened` or `closed`. `position` runs on from the session's last report while playing (clamped to
  `duration`); either may be `null`. `volume`/`muted` are `null` without pycaw. `art` is an id that
  changes with the song, `null` when the song has no thumbnail. 502 when Windows' media service fails.
- `GET /media/art?format=jpeg|rgb565&size=16..512` → the current thumbnail. `jpeg` (default): a baseline
  JPEG fitted inside `size` (the raw thumbnail when it's already a JPEG and Pillow is missing).
  `rgb565`: exactly `size`×`size` pixels, cropped to fill, little-endian, rows packed
  (`application/octet-stream`). Add `encoding=base64` for `{"ok":true,"id","size","format","bytes",
  "data":"<base64>"}` instead (the tablet reads JSON bodies only). 404 with no art (or no Pillow).
- `POST /media/control` `{"action":"play|pause|toggle|next|previous|stop|volume_up|volume_down|mute"}`
  → `{"ok":true,"done":bool}` (`done` false: the app refused, e.g. no next track).
  `{"action":"volume","level":0.5}` sets the master volume (501 without pycaw); `volume_up`,
  `volume_down` and `mute` press the keyboard's media keys, so they need nothing extra. 400 for any
  other action, 503 when the remote isn't available.

The tablet side is `components/home/src/home_pc.c`: `/media/now` every second while home mode or the
music app shows it (every few seconds when nothing plays), the art as base64 RGB565 at 160 px when its
id changes, and the controls sent the moment they're tapped.
