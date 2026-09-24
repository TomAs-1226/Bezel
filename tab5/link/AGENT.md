# Working the Catalyst Link inbox

You are the PC's coding agent. Catalyst Tab — the pit technician's tablet — drops **work orders** here
while the robot is on the cart: bugs it diagnosed, tuning it wants checked, tasks it can't do from the
pit. Some come with a **proposed patch**: a branch `tab/<stamp>-<slug>` that Catalyst Link made from
the tablet's edits. Your job is to turn a work order into a reviewed change on your own branch.

The inbox lives in `~/.catalyst-link/inbox/` (or `$CATALYST_LINK_HOME/inbox/`), one Markdown file per
work order. Use the `catalyst-link` CLI (or `python -m catalyst_link` from `tab5/link/`) rather than
editing the front matter by hand.

## 1. Pick one

```sh
catalyst-link inbox                 # open and claimed items, newest first
catalyst-link inbox --status open --json
catalyst-link claim <id>            # fails if someone else already claimed it: pick another
```

Claim before you start, so two agents never work the same order. If you have to stop without
finishing, `catalyst-link release <id>` puts it back.

## 2. Read everything it carries

```sh
catalyst-link show <id>
```

- **Body**: what the technician and the tablet's assistant saw and want.
- **Robot snapshot**: battery, mode, alerts, the tunables' values when it was filed. Evidence, not
  ground truth — the robot has moved on since.
- **Attached files**: run recordings (`.csv`), clips (`.h264`), logs — absolute paths under
  `~/.catalyst-link/files/`. Read them when the order refers to them.
- **Proposed patch** (if any): `show` prints its branch, base commit and review commands:

  ```sh
  cd <robot repo>
  git log  <base>..tab/<stamp>-<slug>
  git diff <base>...tab/<stamp>-<slug>
  ```

  Its worktree (`~/.catalyst-link/worktrees/p-…`) already has it checked out, and `check:` says
  whether the Link's compile check passed there.

## 3. Do the work on your own branch

- Branch from the team's main line in the robot repo: `git switch -c robot/<short-name> main`
  (or use your own worktree). Never commit on a `tab/…` branch and never on `main` directly.
- **The tablet's patch is a proposal.** Review it like a stranger's pull request: is the diagnosis
  right, is the change the smallest correct one, does it break anything else? Cherry-pick it, adapt
  it, or write something better — never merge it blindly.
- Keep to what the order asks. Robot code runs a 50 kg machine: no drive-by refactors.
- Build and run the tests (`./gradlew build`, or whatever the project uses). A change that doesn't
  build isn't done.
- **Never deploy to a robot** (`./gradlew deploy`, anything that talks to the roboRIO/Systemcore) and
  **never push** unless the humans have told you to in this session. People on the field decide when
  code goes on the robot.

## 4. Close it

```sh
catalyst-link done <id> --note "What changed, on which branch, build/test result, anything to watch"
catalyst-link reject <id> --note "Why not (can't reproduce / needs a human / wrong diagnosis: …)"
```

A good note: `raised kElevatorP 0.8 → 1.0 and added kD 0.02 on robot/elevator-l4 (from tab/…,
adapted); ./gradlew build passes; verify on the cart with the L4 routine before a match.`
The note shows up on the tablet, so write it for the technician.

## Rules

- Read-only on the Link's own state except through the CLI (`claim`, `release`, `done`, `reject`).
- Don't delete `tab/…` branches or worktrees; the humans prune them (`git worktree remove`,
  `git branch -D`) — the Link then shows the patch as `dropped`.
- Secrets (`.env`, keys) are outside what the tablet can see; keep it that way — don't copy them
  into work-order notes or commit messages.
