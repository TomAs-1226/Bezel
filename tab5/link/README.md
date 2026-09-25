# Catalyst Link

The PC-side companion of [Catalyst Tab](../README.md). It runs on the team laptop next to the robot
project's git repo, and gives the tablet in the pit five things:

- **read-only code**: browse, read and search the robot project;
- **patches**: the tablet's proposed edits become a new branch in its own worktree — for a human or
  the PC's agent to review, never merged or deployed by the Link;
- **an inbox of work orders** that the PC's own coding agent (Claude Code, typically) works through
  asynchronously, following [AGENT.md](AGENT.md);
- **file drop**: run recordings, H.264 clips and logs from the tablet's microSD;
- **Claude, through the PC**: either through Claude Code logged in with your Claude subscription (no
  API key anywhere), or the Messages API forwarded with the PC's key. Either way no key lives on the
  tablet.

The wire contract is [docs/link-api.md](../docs/link-api.md); the tablet's side is
`components/assist/include/link.h`.

It runs headless from a terminal (`catalyst-link serve`), or inside **the desktop app**
([below](#the-desktop-app)): a window and tray icon that runs the same Link and shows its status, pairing,
now playing, Claude Code's sessions, the inbox and the log.

## The safety model

What the Link guarantees, whatever the tablet sends:

- **Every request needs the token** (`X-Link-Token`, compared in constant time). Only
  `GET /link/status` answers without it, and then only with name, version and `"auth": false`.
- **Code is read-only.** Paths are resolved (symlinks followed) and must stay inside the repo; `.git`
  and the deny list — `*.env`, `.env*`, `*secret*`, `*.pem`, `*.key` (and `*.p12`, `*.pfx`, `*.jks`,
  `*.keystore`, `id_rsa*`…), `build/`, `.gradle/`, `bin/` at any depth — are refused with 403 and
  never listed or searched.
- **Patches never touch your branch or your working tree.** Each one is a new branch `tab/<id>` made
  from `HEAD` in `~/.catalyst-link/worktrees/`, committed as `Catalyst Tab <tab@catalyst.local>` with
  the title, summary and robot snapshot in the message. Edits are exact, unique string replacements
  (≤ 20 edits, ≤ 64 KB); a file can be created but never deleted or replaced whole.
- **Nothing is pushed, merged or deployed.** The Link never runs `git push`, never merges, never
  runs a deploy.
- **The tablet never chooses a command.** The compile check (`--check`) and the work-order hook
  (`--on-work-order`) come only from the PC's command line; the only thing substituted into the hook
  is the Link-generated path of the new work order.
- **Credentials stay on the PC.** An `ANTHROPIC_API_KEY` in the Link's environment, or Claude Code's
  own login. In `claude-code` mode Claude gets the tablet's tools and nothing else: no shell, no files,
  no web, no settings, plugins or other MCP servers, and every tool still runs on the tablet, behind
  the tablet's confirmation for anything that changes the robot.
- **Everything that writes is audited**: `~/.catalyst-link/log.jsonl` has one line per write request
  (patch, work order, status change, upload) and per refused token, with its outcome, plus one line
  per Claude request with the model that served it and its token usage.
- Sizes are capped: JSON bodies 1 MB, Messages requests 32 MB, uploads 64 MB; reads 400 lines /
  24 KB, trees 400 entries, searches 200 matches.

## Install

Python 3.10 or newer, and git.

```sh
cd tab5/link
python -m pip install .            # the catalyst-link command, with the anthropic SDK and claude-agent-sdk
python -m pip install zeroconf     # optional: lets the tablet find the Link by mDNS
python -m pip install ".[media]"   # optional, Windows: the media remote for the tablet's home mode
```

The media remote shows the tablet what this PC is playing (Spotify, YouTube Music, anything in Windows'
media controls) with its album art, and lets it play, pause, skip and change the volume
([docs/link-api.md](../docs/link-api.md#media--what-the-pc-is-playing-for-the-tablets-home-mode)).

Or without installing: `pip install -r requirements.txt` and run `python -m catalyst_link …` from
`tab5/link/`.

## Run it

```sh
catalyst-link serve --repo ~/robot \
    --check "./gradlew compileJava" --check-timeout 300
```

With no `ANTHROPIC_API_KEY` set, Claude goes through Claude Code and your Claude subscription (see
[below](#claude-with-your-claude-subscription-no-api-key)); set the key to use the API instead.

It prints its address and its token. **Pair the tablet** (settings → home → pair the pc, or the "pair
pc" app): the tablet lists the Links it finds on the network (mDNS: `pip install zeroconf`), you tap
this PC, and the Link prints a six-digit code here (and shows it as a Windows notification); type it
on the tablet and they're paired. Each paired tablet gets a token of its own, so one can be forgotten
(in the desktop app) without re-pairing the others. Typing the main token by hand in the link app still
works (`serve --no-pair` turns pairing off; `--no-pair-toast` keeps the code in this console).

| flag | |
|---|---|
| `--repo PATH` | the robot project's git repo (required) |
| `--port 8765` / `--bind 0.0.0.0` | where to listen |
| `--check "CMD"` | compile check run (via the shell) in each new patch worktree; a failing or timed-out check keeps the branch and says so |
| `--check-timeout 300` | seconds before the check's whole process tree is killed |
| `--on-work-order "CMD {path}"` | run a command, detached, for each new work order; off by default |
| `--name NAME` | what the tablet shows (default: the hostname) |
| `--claude MODE` | how the tablet reaches Claude: `claude-code`, `api`, `off`, or `auto` (default): `api` when `ANTHROPIC_API_KEY` is set, else `claude-code` |
| `--claude-model MODEL` | `claude-code` only: the model (default: Claude Code's own default) |
| `--claude-cli PATH` | `claude-code` only: the Claude Code CLI to run (also `$CATALYST_LINK_CLAUDE_CLI`) |
| `--no-media` | no media remote: the tablet's home mode then can't see or control what this PC plays |
| `--no-pair` | no pairing by code: the tablet needs the token typed in by hand |
| `--no-pair-toast` | show the pairing code only in this console, not as a Windows notification |
| `--no-mdns`, `--quiet` | |
| `--gui` | how the desktop app runs it: neither the token nor a pairing code is ever printed (the app shows the code) |

State lives in `~/.catalyst-link/` (override with `CATALYST_LINK_HOME`): `token`, `devices.json` (the
paired tablets: name, address, last seen and a SHA-256 of each one's token, never the token), `inbox/`,
`patches/` (`<id>.json`, `.diff`, `.check.log`), `files/`, `worktrees/`, `log.jsonl`, `hooks.log`,
and for `claude-code`: `claude-oauth-token` (only if you store one) and the empty working folder
`claude-code/`.

### Claude with your Claude subscription (no API key)

The `claude-code` backend runs [Claude Code](https://docs.claude.com/en/docs/claude-code) on this PC
through the official Claude Agent SDK, signed in as you, so the tablet's assistant runs on your Claude
Pro/Max plan and its usage limits. Nothing changes on the tablet: it keeps using the Link route. How it
works is in [docs/link-api.md](../docs/link-api.md#claude-code-the-owners-claude-subscription-no-api-key).

1. `pip install .` brings `claude-agent-sdk`, which includes the Claude Code CLI (a `claude` on PATH, or
   `--claude-cli PATH`, works too).
2. **Sign in once**, as the same OS user the Link runs as. Either of:
   - `claude setup-token`: prints a long-lived token for your subscription. Store it for the Link with
     `catalyst-link claude-token` (paste it; the input is hidden and it is written to
     `~/.catalyst-link/claude-oauth-token`, readable only by you), or put it in the Link's environment
     as `CLAUDE_CODE_OAUTH_TOKEN`. Best for a Link that runs as a service or at login.
   - `claude login` (or `/login` inside `claude`): Claude Code's own login in `~/.claude/`. Enough when
     you start the Link yourself.

   The token is a credential: never commit it or put it in a work order.
3. Check it: `catalyst-link claude-check` (no model call: finds the SDK and the CLI, and reports the
   login and plan), then `catalyst-link claude-check --live` (one tiny turn through Claude Code).
4. Start the Link as usual. Its banner reads `claude   claude-code via your Claude login (max)`, and
   `/link/status` reports `"claude": true, "claude_via": "claude-code"`.

If `ANTHROPIC_API_KEY` is also set, `--claude auto` prefers the API; pass `--claude claude-code` to use
the subscription anyway (the key is then withheld from Claude Code, so it can't bill the API).

The tablet can also skip the Link and this PC entirely, and use OpenAI directly over Wi-Fi with an
OpenAI API key — settings → assistant, or the Link app, under "openai key"; the model is a setting
too, default `gpt-4o-mini`. That route doesn't go through the Link at all, so nothing above applies to it.

### Letting the PC's agent pick up work orders by itself

`--on-work-order` is off unless you turn it on. With Claude Code installed, this starts a headless
session for each new work order (an example to opt into — adjust the permissions to taste):

```sh
catalyst-link serve --repo ~/robot --check "./gradlew compileJava" \
  --on-work-order 'claude -p "Work the Catalyst Link inbox item at {path}; follow tab5/link/AGENT.md"'
```

The command runs in the robot repo, detached, with its output in `~/.catalyst-link/hooks.log`.
`{path}` is the work order's file (quoted) and `{id}` its id. Or leave the hook off and ask your agent
now and then: *"Work the open Catalyst Link inbox items; follow tab5/link/AGENT.md."*

## Claude Code on the tablet

The companion's desk mode can show the tablet what Claude Code is doing on this PC — which session is
running, what it's doing, and roughly when it'll finish — by way of Claude Code's own hooks. The wire
contract is in [docs/link-api.md](../docs/link-api.md#claude-code-sessions--what-claude-code-on-the-pc-is-doing).

The quick way: the desktop app's **claude code** panel has an **install the hooks** button, and
`catalyst-link hook-install [--port N]` does the same from a terminal (`--remove` takes them out). Both
add only the Link's entries to `~/.claude/settings.json` (or `$CLAUDE_CONFIG_DIR/settings.json`), keep
every other hook and setting, save the old file as `settings.json.catalyst-link.bak`, and refuse to
touch a file that isn't valid JSON. The hook command points at this checkout's `hook.py`: install
again after moving the Link (the app shows the hooks as "pointing somewhere else"). By hand:

1. Run the Link as usual. The hook posts to it on `127.0.0.1:8765` (a Link on another port is named in
   the command as `--url http://127.0.0.1:PORT`) with the token from `~/.catalyst-link/token`, so it
   must run as the same user account as Claude Code.
2. `catalyst-link hook-settings` prints the `hooks` block for Claude Code's `settings.json`, wired to
   this PC's Python running `hook.py` by its path (works whether or not the package is pip-installed):

   ```json
   {
     "hooks": {
       "UserPromptSubmit": [{"hooks": [{"type": "command",
         "command": "\"C:/Users/you/AppData/Local/Programs/Python/Python312/python.exe\" \"C:/.../tab5/link/catalyst_link/hook.py\"",
         "timeout": 5}]}],
       "PreToolUse": [{"matcher": "*", "hooks": [{"type": "command",
         "command": "\"C:/Users/you/AppData/Local/Programs/Python/Python312/python.exe\" \"C:/.../tab5/link/catalyst_link/hook.py\"",
         "timeout": 5}]}],
       "PostToolUse": [{"matcher": "*", "hooks": [{"type": "command", "command": "...", "timeout": 5}]}],
       "Notification": [{"hooks": [{"type": "command", "command": "...", "timeout": 5}]}],
       "Stop": [{"hooks": [{"type": "command", "command": "...", "timeout": 5}]}],
       "SubagentStop": [{"hooks": [{"type": "command", "command": "...", "timeout": 5}]}],
       "SessionStart": [{"hooks": [{"type": "command", "command": "...", "timeout": 5}]}],
       "SessionEnd": [{"hooks": [{"type": "command", "command": "...", "timeout": 5}]}]
     }
   }
   ```

3. Merge that into `~/.claude/settings.json` (every project) or a project's `.claude/settings.json`
   (that project only). If a `hooks` key is already there, add these events' entries alongside the
   existing ones rather than replacing them — each event is a list of blocks. Restart your Claude
   Code sessions (or check `/hooks`) so they pick it up.
4. Check it: start a Claude Code session, send it a prompt, then run `catalyst-link claude-sessions` —
   it should show the session `running`. On the tablet, open the companion and tap **claude** for the
   sessions panel.

The hook (`catalyst_link/hook.py`) never prints anything and always exits 0, so it can never block
Claude Code or feed anything into its context; each call has a 1.5 s timeout. Environment variables:
`CATALYST_LINK_URL` (default `http://127.0.0.1:8765`), `CATALYST_LINK_HOME` (default
`~/.catalyst-link`, for the token), and `CATALYST_LINK_HOOK_PROMPTS=0` to stop sending the prompt's
first line (the tablet then shows "turn N" instead). What leaves the PC: the session id, the working
folder, the first line of each prompt, a few words describing each tool call, and notification text.
What never does: tool inputs and outputs, file contents, or transcripts. Finish estimates need a few
completed turns in the same folder (or recently, elsewhere) before they show up.

## The CLI

```sh
catalyst-link inbox [--status open|claimed|done|rejected|all] [--json]
catalyst-link show <id>                  # the work order, plus its patch branch and review commands
catalyst-link claim <id> [--note …]
catalyst-link done <id> --note "what changed, which branch"
catalyst-link reject <id> --note "why not"
catalyst-link release <id>               # put a claimed item back
catalyst-link patches [--json]           # proposed / merged / dropped, and the check result
catalyst-link token [--rotate]           # print it, or make a new one (re-pair the tablet)
catalyst-link claude-check [--live]      # is the claude-code backend ready? (--live: one tiny turn)
catalyst-link claude-token [--remove]    # store (from stdin) or delete a `claude setup-token` token
catalyst-link hook                       # Claude Code hook: reads stdin, posts to the Link, prints nothing
catalyst-link hook-settings [--command …] # print the "hooks" block for Claude Code's settings.json
catalyst-link hook-install [--port N] [--remove]  # add (or take out) only the Link's hooks in ~/.claude/settings.json
catalyst-link claude-sessions [--json]   # what the running Link knows about Claude Code's sessions
```

Ids accept a unique prefix. A patch shows as **merged** once its branch is an ancestor of `HEAD`, and
**dropped** once the branch is deleted; clean up with
`git worktree remove ~/.catalyst-link/worktrees/<id> && git branch -D tab/<…>`.

## The desktop app

`desktop/` is Catalyst Link as a Windows app: a window and a tray icon around the same Link. It is built
like Catalyst Console — Tauri 2, plain HTML/CSS/JS, no bundler — in the Catalyst identity
(`identity.css` and `motion.js`, copied from FrcCatalyst's `docs/assets` and checked for drift), with
Bezel's calm monochrome, lowercase copy and spring motion.

**How it works.** The app starts the Link as a child process (`python -m catalyst_link serve --gui …`),
restarts it if it falls over after running a while, and stops it when you quit; a Windows job object
makes sure a crashed or killed app never leaves a Link holding the port. If a Link is already answering
on the port (started from a terminal or at login), the app attaches to it instead of starting a second.
Every panel works through the Link's HTTP API on `127.0.0.1`: the app's Rust side reads the main token
from `~/.catalyst-link/token` and makes the requests, so the token never reaches the window. The new
`/admin/*` routes it uses answer only this PC, only the main token, and never a web page
([docs/link-api.md](../docs/link-api.md#the-desktop-apps-routes-admin)). Closing the window hides it
to the tray; the tablet keeps its Link until **quit** in the tray menu.

| panel | |
|---|---|
| **status** | running / starting / stopped / attached, the address the tablet uses, version, port, pid, robot project and branch, pairing, mDNS, media, Claude and inbox at a glance; restart, stop, start |
| **pairing** | **pair a tablet** walks you through it; when a tablet asks, the window comes forward (from the tray too) with the six-digit code large, a two-minute countdown and tries left; cancel it, or see "paired". Below: the paired tablets (name, address, last seen) with **forget**, and machines using the main token by hand |
| **now playing** | what `media.py` sees (title, artist, app, album art, progress) with previous / play-pause / next, mute, volume keys and a volume slider: the same calls the tablet makes |
| **claude code** | the sessions the hooks report (running, waiting on you, done, error; for how long; the finish estimate) and **install / update / remove the hooks** |
| **inbox** | the work orders by status; open one to read it (body, robot snapshot, notes) and **claim**, **release**, **done** or **reject** (the last two with a note), or open its file |
| **log** | the Link's output, filtered, followed and copyable. Tokens and pairing codes never appear: `--gui` keeps them off the console, and every line is masked again before it is stored |
| **settings** | robot project, port, name; pairing, pairing notifications, media remote, mDNS; **start with Windows** (a per-user `HKCU\…\Run` value, off by default; Task Manager's startup switch is honoured) and **start minimized**; which Python runs the Link |

### Install and run

Needs the Link's own requirements (above), Node.js and Rust (`rustup`, the MSVC toolchain) to build,
and the WebView2 runtime (part of Windows 11).

```sh
cd tab5/link
python -m pip install ".[media]" zeroconf   # the Link, its media remote and mDNS, into the Python the app runs
cd desktop
npm install
npm run dev          # run it from source (a debug build)
npm run build        # release build + installer: src-tauri/target/release/bundle/nsis/Catalyst Link_<v>_x64-setup.exe
npm test             # the identity check and the frontend's unit tests
cd src-tauri && cargo test   # the Rust side's tests (log masking, settings, the HTTP client)
```

On first start, choose the robot project (status → **choose the robot project…**, or settings); the
Link starts as soon as one is set. Settings live in `%APPDATA%\com.frccatalyst.link\settings.json`.

Where the Link comes from: the app runs `catalyst_link` from the source tree it was built in
(`tab5/link`) when that folder still exists, else from the pip-installed package; settings → python →
**link folder** overrides both, and **python** picks the interpreter (default `python` on `PATH`). An
installed copy on another PC therefore needs `python -m pip install .` in `tab5/link` there.

The identity files are copies: `npm test` and `npm run build` (and `tauri build`, through
`beforeBuildCommand`) fail when `src/styles/identity.css` or `src/motion.js` differ from FrcCatalyst's
`docs/assets` (found under `~/dev`, or `CATALYST_IDENTITY_DIR`); `npm run identity` copies them over.
`scripts/make-icons.py` redraws the icons from the identity's colours.

For testing without touching your real setup: `CATALYST_LINK_HOME` (the Link's state),
`CATALYST_LINK_DESKTOP_SETTINGS` (the app's settings file) and `CATALYST_LINK_CLAUDE_SETTINGS` (the
Claude Code settings the hooks button edits) all point elsewhere, and the child Link inherits them.

## Running it at login

The desktop app's **start with Windows** is the easy way on Windows. Without the app:

**Linux (systemd user unit)** — `~/.config/systemd/user/catalyst-link.service`:

```ini
[Unit]
Description=Catalyst Link
After=network-online.target

[Service]
ExecStart=%h/.local/bin/catalyst-link serve --repo %h/robot --check "./gradlew compileJava" --quiet
EnvironmentFile=-%h/.config/catalyst-link.env
Restart=on-failure

[Install]
WantedBy=default.target
```

Put `ANTHROPIC_API_KEY=…` (or, for your subscription, `CLAUDE_CODE_OAUTH_TOKEN=…` from
`claude setup-token`) in `~/.config/catalyst-link.env` (mode 600), then
`systemctl --user enable --now catalyst-link` (and `loginctl enable-linger $USER` to run it without
a login session). `journalctl --user -u catalyst-link` shows the token line.

**macOS** — a LaunchAgent plist running the same command, with `ANTHROPIC_API_KEY` in its
`EnvironmentVariables`.

**Windows** — Task Scheduler: a task triggered *At log on*, action
`catalyst-link.exe serve --repo C:\Users\you\robot --check "gradlew.bat compileJava"`, running as
you (so it finds your `claude-oauth-token` or Claude Code login), or with `ANTHROPIC_API_KEY` set as a
user environment variable. Or as a service with NSSM:
`nssm install CatalystLink "C:\…\Scripts\catalyst-link.exe" serve --repo C:\Users\you\robot`, then
`nssm set CatalystLink AppEnvironmentExtra ANTHROPIC_API_KEY=sk-ant-…`. Allow port 8765 through the
Windows firewall on the pit network's profile.

## How the tablet finds it

With `zeroconf` installed the Link advertises `_catalyst-link._tcp` with TXT `name=<hostname>` (and
`version`, `path=/link/status`); the tablet browses for it when its Link address is empty. Otherwise
type the address shown at startup. The tablet polls `GET /link/status` every few seconds; anything it
sends while the Link is away waits in its microSD outbox, and the Link recognises a re-sent patch,
work order or upload (same content within ten minutes, or the same bytes under the same name) and
answers with the original instead of making a copy.

## Tests

```sh
python -m unittest discover -s tab5/link/tests
```

They build a throwaway git repo per test and run the server on a free port; the proxy is tested both
with a fake SDK client and with the real `anthropic` SDK pointed at a fake Messages API upstream, and
the `claude-code` backend with a scripted fake Claude Code session. On a PC where Claude Code is
logged in, `CATALYST_LINK_LIVE=1` adds one live test: a fake tablet drives real Claude Code through a
tool call, its result and a follow-up question (it uses a little of your plan's quota). On Windows
without Developer Mode the symlink-escape cases are skipped (making symlinks needs it).
