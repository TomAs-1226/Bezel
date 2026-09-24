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
```

Or without installing: `pip install -r requirements.txt` and run `python -m catalyst_link …` from
`tab5/link/`.

## Run it

```sh
catalyst-link serve --repo ~/robot \
    --check "./gradlew compileJava" --check-timeout 300
```

With no `ANTHROPIC_API_KEY` set, Claude goes through Claude Code and your Claude subscription (see
[below](#claude-with-your-claude-subscription-no-api-key)); set the key to use the API instead.

It prints its address and the **pairing token**. On the tablet, open settings → Link and type the
token once (leave the address empty to find the Link by mDNS, or type `http://<pc>:8765`).

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
| `--no-mdns`, `--quiet` | |

State lives in `~/.catalyst-link/` (override with `CATALYST_LINK_HOME`): `token`, `inbox/`,
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
```

Ids accept a unique prefix. A patch shows as **merged** once its branch is an ancestor of `HEAD`, and
**dropped** once the branch is deleted; clean up with
`git worktree remove ~/.catalyst-link/worktrees/<id> && git branch -D tab/<…>`.

## Running it at login

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
