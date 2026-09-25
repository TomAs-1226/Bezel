# Catalyst Tab, refocused: a pocket multitool for one programmer

Scope: v1 for FRC 5805's programmer (author of FrcCatalyst) and for the on-device/PC Claude
assistants. Grounded in `tab5/README.md`, `tab5/docs/{catalyst-contract,systemcore,tab5-hardware,
bezel-port,link-api}.md`, `tab5/components/{ui,catalyst,assist}`, `tab5/link/`, FrcCatalyst
(`main`, `systemcore-alpha6`, `upgrade/alpha-7`), and Catalyst Console
(`C:\Users\yu_th\dev\CatalystConsole\src`).

## 1. Who uses it, and the top 6 jobs

- **Pit, robot on the cart, cable in hand**: the primary scenario. Tether to Systemcore's USB-C,
  no Wi-Fi needed (events forbid it — `ui_apps_tools.c` settings note: "At events Wi-Fi to the
  robot isn't allowed").
- **Bench, robot on blocks**: calibration and characterization runs where something must move.
- **On-field practice**: preflight before a run, black-box recording during it, review after.
- **With Claude on the PC** (via Catalyst Link): patch review, work-order triage, code questions —
  the Tab is the terminal, the PC/Claude Code does the heavy lifting.

Ranked jobs:
1. **Preflight before a run** — GO/NO-GO in seconds (`preflight`, ported from X1's `preflight.py`,
   `components/catalyst/src/cat_preflight.c`).
2. **Pull and read logs off the robot** — `.wpilog`/`.dslog`/`.dsevents` from microSD, and live
   `motors`/`systemcore` telemetry, without opening a laptop.
3. **Diagnose a fault fast** — `alerts`, `devices`, `can tap` (works with robot code dead), `states`.
4. **Run/observe a calibration** — wheel radius, slip current, SysId, watching robot-side progress
   and confirming readiness, then handing the actual button-press to the Driver Station.
5. **Adjust a tunable or auto choice on the spot** — `tune`, `auto` (the only two live writes besides
   a Limelight LED, per `docs/catalyst-contract.md`).
6. **Ask Claude, stage a fix or a work order** — `assist` + `link`, so a diagnosis becomes a patch
   branch or an inbox item the PC's Claude Code picks up later.

Everything else (camera, IMU inclinometer, CAN sniffer, black-box recorder) serves job 2 or 3 from
an angle only a handheld gets: inside a bellypan, off the cart, with the robot code dead.

## 2. Screen list for v1

**Keep, unchanged** — each earns its place against a job above:
`pulse` (job 1/3 — the one-glance overview), `devices` (3), `power` (3), `motion` (3),
`preflight` (1), `alerts` (3), `tune` (5), `auto` (5), `robot` (2), `systemcore` (2/3),
`motors` (2), `states` (3), `recorder` (2, the on-device black box — no Tab5 substitute exists),
`can tap` (3, works with the robot code dead — unique to the tablet), `assist` (6), `link` (6),
`logs` (2, the DS-log reader), `level` (job 4, unique to a handheld with an IMU), `settings`.

**Cut**: none from the README's list are purely decorative — each maps to a job or is unique
hardware capability (camera, IMU, CAN tap). The candidates for demotion, not deletion, are:
- `field` (pose/PathPlanner/vision) — valuable for autonomous debugging but duplicates Console's
  field view exactly; keep it but do not spend v1 polish on it beyond parity.
- `controls` — reads a manifest most robots don't publish yet (`docs/systemcore.md`: "no publisher
  of its own" in the library); keep as a thin screen, not a v1 focus.
- `lens` (camera) — real value for bellypans/gearboxes, but H.264 clip recording is expensive
  (~4.2 MB PSRAM, PPA+encoder load per `docs/tab5-hardware.md`); keep JPEG snapshot in v1, treat
  clip recording as a stretch goal.

**New for v1** — a **calibration** app/section, not currently a distinct screen. Today
`WheelRadiusCalibration` and `SlipCurrentCalibration` results surface only implicitly through
`/Catalyst/Calibration/*` (read by `tune`'s Console analogue, `calibration.js`, and by
`cat_preflight.c`'s wheel-radius check). v1 should give calibration its own guided screen instead
of leaving it scattered — see §4.

## 3. Log pipeline

**Off the robot.** Two paths, both already implemented:
- **Live NT**: `nt4` subscription at `/Catalyst/`, `/FMSInfo/`, etc. (`docs/catalyst-contract.md`),
  20 Hz effective (`periodic 0.05s`). Feeds `pulse`, `motors`, `systemcore`, `states` continuously.
- **Files from microSD**: `.wpilog` (2027 DS), `.dslog`/`.dsevents` (NI DS) parsed by
  `components/catalyst/src/cat_logs.c` per `cat_logs.h`. These are Driver Station logs the tablet
  reads directly off the card that was in the DS laptop or Systemcore — the tablet does not (today)
  pull them over the USB tether from a live robot; it reads a card. FrcCatalyst's own
  `WpilogSink` delegates to WPILib's `DataLogManager` (USB stick under `/U/logs`, else `~/logs`) —
  Catalyst does no rotation/naming of its own (confirmed: no such logic found in
  `frc.lib.catalyst.logging`), so the tablet's reader must tolerate WPILib's own naming, not a
  Catalyst-specific scheme.

**On-device analysis, decimated for the P4.** `cat_log_t` already buckets every series to
`CAT_LOG_POINTS = 240` points (battery keeps each bucket's **minimum**, since dips matter; others
keep the maximum) plus up to `CAT_LOG_EVENTS = 64` first events — this is the right model and
should stay the ceiling for any new log view: **never plot raw per-sample data on-device**; 240
points at 60 Hz redraw is trivial, tens of thousands is not (LVGL's software renderer costs ~26 ns
per pixel, per `docs/bezel-port.md`). The 50 Hz recorder CSV (`docs/systemcore.md`) is the one
place raw-rate data is written, and it goes to microSD, not to a chart — sparklines during
recording, not full-resolution review, on-device.

**What goes to the PC vs. Claude.** Via Catalyst Link's `POST /files` (`docs/link-api.md`): the
recorder's CSV ("send to PC"), a Lens JPEG/clip, or a `.wpilog` copied off the card. The on-device
`assist` tool `summarize_log` and `list_logs` (`components/assist/src/as_tooldefs.c`) already give
Claude the same decimated summary the UI shows — Claude doesn't get raw log bytes on-device, only
the same 240-point/64-event digest, which is the right boundary: full-resolution analysis (fitting
a slip-current curve, correlating two runs) is a PC/Claude-Code job over the uploaded file, not an
on-device one.

## 4. Calibration/testing flows

Ground truth: Catalyst's diagnostic and calibration routines are **Driver Station Utility op-modes
or teleop-bound Commands — never NT command triggers** (`docs/catalyst-contract.md`, confirmed
against `frc.lib.catalyst.util.SystemCheck`, `subsystems/swerve/WheelRadiusCalibration.java`,
`subsystems/swerve/SlipCurrentCalibration.java` [2.x only], `sysid/SysIdRoutine.java` +
`util/CharacterizationHelper.java`). The tablet can watch and confirm; it never starts a routine.

For each routine, the Tab's flow:

- **Wheel radius** (`WheelRadiusCalibration.java`, all branches with swerve). Tab: preflight check
  already reads `/Catalyst/Calibration/WheelRadius/*`; a dedicated calibration screen adds a
  "start on the DS, watch here" state — shows `AccumRotations`/turns live, then the finished
  constant with a **copy-to-clipboard-equivalent** (display large, monospace, for hand-transcription
  — Link has no write path into robot constants except a patch, see below). Safety: display
  "robot must be enabled in Test/the bound mode" and refuse to show a stale in-progress state as
  current (mirrors Console's `calibration.js` quiet-while-running/result-when-done rule).
- **Slip current** (`SlipCurrentCalibration.java`, 2.x-only — **not on `main`**, so this screen
  must detect and say plainly "not available on this robot's Catalyst version" rather than show a
  blank/broken state). Reads `Volts`/`PeakAmps`/`RecommendedAmps`(rounded to 5 A)/`Snippet`. Same
  pattern: watch, don't trigger. Safety: this drives the robot into a wall by design — the screen
  should show a large "ROBOT WILL MOVE" warning before the run starts (from the DS) and while
  `Status` shows running.
- **SysId feedforward** (`sysid/SysIdRoutine.java` + `util/CharacterizationHelper.java`). Results
  are **not** published to `/Catalyst/` — they go through WPILib's own SysId log, read back in the
  SysId analyzer or AdvantageScope on the PC. The Tab's job here is narrower: confirm the robot is
  in the right state (enabled, right mode bound) and forward the resulting `.wpilog` to the PC via
  Link's file upload — it cannot itself analyze a SysId run because it never sees the SysId log
  format's high-rate feedforward data. Say this plainly in the UI ("results open in SysId/AdvantageScope
  on your PC") rather than fake an on-device analysis.
- **System check** (`frc.lib.catalyst.util.SystemCheck`, all branches). Bound as a Command; results
  at `/Catalyst/SystemCheck/<name>/{Ready,Report,<test>}` — already read by `preflight`
  (`cat_preflight.c`). No new screen needed; surfacing per-test PASS/FAIL under `preflight`'s
  existing per-check detail view covers it.
- **Confirmation and safety, uniformly**: every calibration screen shows the robot's current mode
  (enabled/disabled, which DS mode) pulled from `/FMSInfo/ControlWord` at the top, refuses to imply
  the tablet can start/stop anything, and — like the assist's write tools — never issues a command;
  it is read-only by the same contract as everything else (`docs/catalyst-contract.md`'s rule holds
  here without exception).

## 5. UI sizing rules (5", 1280×720, thumb in a noisy pit)

Current state (measured from `components/bezel`): body fonts are 17/20 px (`bz_font_body_17`,
`bz_font_body_20`), mono 13/16 px, at 294 ppi against Bezel's 255 ppi reference — the panel is
**13% physically smaller than the design's own unit**, which the project already flags
(`docs/bezel-port.md`: "Bezel's reference panel is 720×720... every Bezel number is a Tab5 pixel").
That means today's smallest body text (`bz_font_body_17`) reads roughly like 15 px at the reference
density — too small for a gloved thumb glancing down in a loud pit.

Rules for v1:
- **Minimum touch target**: 88 px (already the rule — `docs/bezel-port.md`: "the 48 dp touch floor
  becomes 88 px", via `lv_obj_set_ext_click_area`); apply it to every new interactive element, not
  just existing ones. Never rely on the visual size of a small icon as the tap target.
- **Minimum body text**: raise the floor from 17 px to **20 px** (`bz_font_body_20`) for anything a
  technician reads standing up; reserve 13/16 px mono for dense tabular screens only
  (`systemcore`, `motors`, `can tap`) viewed close/seated, and even there prefer 16 px minimum.
- **Items per screen**: cap primary content at what fits without scrolling at arm's length — the
  existing tile grid (radius 32, padding 24, 14 px gaps per `bezel-port.md`) at 3 columns × 2–3 rows
  is the right density; resist adding a 4th column of tiles to fit more data, add a second screen
  instead.
- **Contrast**: keep Bezel's ink/dim/faint ladder (`#EEF0F2`/`#9AA1A8`/`#5D646B` dark,
  `#15181B`/`#5B636C`/`#A3AAB2` light) but avoid `faint`/`dim` for anything safety-relevant (battery
  floor, e-stop, brownout) — those stay `ink` or a status color, never a de-emphasized tone.
- **Fluid-first rendering**: solid surfaces are the default; glass is reserved for floating chrome
  that is cheap because it's small and mostly static (dock, island, orb, confirmation cards) —
  exactly the existing `bz_comp.c` groups (dock 0, island 3, control center 6, orb 7). Content
  tiles, lists and charts stay **flat surface1/2/3** with no per-pixel refraction; this is already
  the pattern, and it should not be relaxed to add glass to content screens for v1 — the control
  center is already the one place missing the 60 Hz budget (`docs/bezel-port.md`: p95 47.6 ms
  pulling it), so no new glass surface should be added without a frame-cost trace to justify it.

## 6. What Claude can do with the Tab as a tool

**On-device assist** (`components/assist/`, 29 tools in `as_tooldefs.c`): reads everything the UI
reads — the team's event and battery fleet (`get_matches`, `get_batteries`, from the UI's desk), `robot_overview`, `get_alerts`, `get_mechanisms`, `get_power`, `get_can`, `get_vision`,
`run_preflight`, `list_topics`/`read_topics`, `list_tunables`, `list_autos`, `list_snapshots`,
`list_logs`/`summarize_log`, `systemcore_health`, `motor_history`, plus PC-repo tools forwarded
through Link (`code_tree`, `code_read`, `code_search`, `list_patches`, `list_work_orders`). It
writes in exactly three ways, each gated by an on-screen card the technician must approve (auto-
declines after 90 s, per `README.md`): `set_tunable` / `select_auto` (with `revert_snapshot` for
undo), `propose_patch` (lands on a new branch in the PC's own worktree, never the checked-out
branch, never pushed — `docs/link-api.md`), and `create_work_order` (drops a Markdown ticket with
the robot snapshot into the Link's inbox for the PC's Claude Code to pick up per `link/AGENT.md`).

**Claude on the PC via Link**: `/v1/messages` proxies the Messages API so the API key never reaches
the tablet (`docs/link-api.md`); `/code/*` gives read-only repo access (tree/read/search, deny-
listing secrets and build output); `/code/patch` is the only way code changes, always as a new
`tab/<stamp>-<slug>` branch with an optional compile check; `/inbox` is the asynchronous work-order
queue a human or the PC's own Claude Code agent works through. This is deliberately narrow: the Tab
never deploys, never pushes, never merges — the PC (and the human) stay the only place a change
reaches the robot's real branch.

**Division of labor**: the Tab's Claude answers "what's wrong right now" from live telemetry and
decimated log summaries; the PC's Claude Code does anything needing the full repo, full-resolution
log data, or a build (SysId analysis, a multi-file patch, running tests). The Tab hands off by
uploading a file or filing a work order, exactly as designed — v1 should not try to move repo
access or heavy analysis onto the P4.

## 7. This is an OS, not an app

Owner correction mid-spec: Catalyst Tab is firmware for a device, not one app among many — it needs
a real OS layer underneath the 19 screens above. Most of it already exists; it's under-named
because the code (`components/ui/src/ui_shell.c`) calls it "the shell," not "the OS."

**Launcher / home — exists, reframe rather than rebuild.** `ui_shell.c` already runs a 5-page
horizontal pager (`NPAGES 5`: `pulse`, `devices`, `power`, `motion`, `tools`) under a glass dock
whose 5 items jump pages, with a draggable "droplet" that snaps to the nearest (`build_island`/dock
code, `bz_ui.h` motion caches). `tools` is functionally the app drawer: every screen not on the
first four pages (`preflight`, `alerts`, `tune`, `auto`, `field`, `robot`, `systemcore`, `motors`,
`states`, `controls`, `recorder`, `can tap`, `assist`, `link`, `logs`, `level`, `lens`, `settings`)
opens from an icon grid there and grows out of that icon on the release spring
(`docs/bezel-port.md`: "Apps grow out of the icon that opened them... can be caught mid-flight").
This is a real app model already — each is a `ui_app_t` with `build`/`open`/`refresh` (see
`APP_SETTINGS` in `ui_apps_tools.c:1012`) registered once and opened by pointer
(`ui_app_open`/`ui_app_is_open`, `ui_shell.c:809,846`). **New for v1**: nothing structural — only
naming the `tools` page as the launcher/app grid in the UI itself (a label, a search-by-typing
filter once the list exceeds one screen) rather than treating it as just another page.

**Status bar — exists as the island, but it's a toast, not a bar.** `build_island()`
(`ui_shell.c:502`) is a top-anchored glass capsule (group 3) that normally shows robot/mode/battery
per the README's table, and morphs to carry a transient message for 2.4 s (`ui_island_say`,
`island_until`) before reverting. That covers "status bar" for the persistent robot/tether/Wi-Fi/
battery/time readout already. **New for v1**: a persistent clock (not found in the island's fields
today — `island_text`/`island_tail` carry robot state, no time-of-day) and explicit tether-vs-Wi-Fi
iconography (today's connection state is read from `hal_net()`/`hal_tether()` in `settings_refresh`
only, not surfaced continuously in the island).

**Notifications — partial; the gap is real.** The island's `ui_island_say` is a single-slot,
self-expiring toast (2.4 s) — there is no queue, no history, no "3 things happened while you were
in another app." Alerts themselves persist and are queryable (`alerts` screen, AlertManager/Health
NT topics), and the assist's approval cards persist until acted on or a 90 s timeout
(`README.md`) — but nothing unifies "a patch was approved," "the recorder saved a run," "Link
connected," and "a fault fired" into one place to check later. **New for v1**: a small notification
tray (pull down alongside or merge into the control center) backed by a ring buffer of the same
events the island already announces one at a time — no new data sources, just retention.

**Control center — exists.** `ui_cc.c` (273 lines), pulled from the top edge (group 6, per
`docs/bezel-port.md`): robot/Wi-Fi/USB link status, tablet battery, brightness, volume, tone
(dark/light), calm, sleep. This is already the fast-access settings surface the requirement asks
for; no change needed beyond the frame-budget fix `bezel-port.md` already flags (p95 47.6 ms
pulling it, 69.5 ms closing it — the one screen not holding 60 Hz).

**Settings — exists, but as one flat screen, not an app with sections.** `ui_apps_tools.c`
`settings_build` (`ui_apps_tools.c:915-1012`) is a single three-tile layout covering: **team**
(numeric keypad), **robot address** (5 preset chips: by team / `robot.local` / roboRIO USB / Systemcore
USB / simulator), **Wi-Fi** (scan, tap-to-join with an on-screen keyboard sheet), **USB tether**
(status line only, no configuration — mode is auto-detected), **brightness** and **volume** sliders,
**dark/light** toggle, **calm** toggle, an **fps** overlay toggle (`perf_chip`), and an **about**
block (firmware version, panel model, chip, PSRAM/SRAM free, battery, microSD mount). Persisted via
`ui_settings_save()`. That is roughly a third of what a device Settings app needs. Mapping the
requirement's list:

| Section | Status | Where |
|---|---|---|
| Display: brightness, volume→sound | exists | `settings_build`, sliders |
| Display: auto-dim, sleep timeout, text size/UI scale, orientation, reduced motion (as a setting, not just "calm"), render quality lite/glass | **new** | none of these exist; `calm` today conflates reduced-motion *and* solid-glass (`docs/bezel-port.md`: "Calm = reduced motion + solid glass") — v1 should split calm's two effects into independent switches under this section |
| Network: Wi-Fi join/scan | exists | `settings_build`, `st_scan`/`st_ap`/`st_kb_event` |
| Network: team number, robot address override | exists | `settings_build`, keypad + `ADDR_LABEL`/`ADDR_VALUE` chips |
| Network: tether mode (auto/force) | **new** | today read-only status (`ST.usb_state`); no mode selection exists |
| Robot: NT prefixes subscribed, which log sources to pull, auto-pull on connect | **new** | subscription prefixes are hardcoded (`docs/catalyst-contract.md`'s fixed list); no UI exists to change them or to configure log auto-pull |
| PC Link: pairing, token, host | **exists, but lives on its own `link` app screen**, not Settings | `README.md`'s `link` screen ("Pairing with Catalyst Link... first start... types it into the tablet's settings once" — `docs/link-api.md`); v1 should cross-link from Settings rather than duplicate the UI |
| Assistant: enable, model, what needs confirmation | **new** | `assist.c`/`as_tools.c` hardcode the tool list and the 90 s approval timeout; no on/off switch, no model choice surfaced in UI (model is chosen server-side, "Claude Opus 5 with adaptive thinking" per `README.md`) |
| Storage: SD usage, log retention, wipe | **new** | `hal_sd_root()` is read in `about` (mount path only); no usage figure, no retention policy, no wipe action anywhere |
| Sound/haptics | **partial** | volume + tone chime exists (`hal_tone`); no haptics (Tab5 has no vibration motor confirmed in hardware doc — so "haptics" here likely means none, worth stating explicitly rather than silently omitting) |
| Power: battery, charging, sleep, power off | **partial** | battery/charging shown in `about`; sleep exists only in the control center (`ui_cc.c`); no in-Settings sleep-timeout or software power-off (physical button only, per `tab5-hardware.md`'s Buttons row) |
| Date/time | **new** | RTC (`RX8130CE`) exists in hardware and is used to timestamp logs/snapshots (`tab5-hardware.md`), set from the robot's NT clock when connected — but there is no manual date/time UI |
| Diagnostics: fps overlay, boot record, serial log level | **partial** | fps overlay exists (`perf_chip`); the boot record already exists and is more prominent than "diagnostics" implies — a crash shows amber under the Catalyst card and safe mode kicks in automatically (`firmware/README.md`: "LAST START: CRASH AT WI-FI · SAFE MODE", written to `catalyst-boot.txt`); serial log is baud-115200 over USB-C only, no level control and no on-screen viewer — v1 should surface the boot record and log level here rather than only in amber text and a wired terminal |
| About / firmware update (OTA from Link) | **partial → new** | About block exists; **there is no OTA today** — updates are `esptool.py write_flash` over USB-C only (`firmware/README.md`), and reflashing the merged image **erases settings including Link pairing** (flashing the app partition alone preserves them). OTA from Link is new work, not a UI gap — it needs a firmware update channel added to `link-api.md` and an updater in the firmware itself |
| Factory reset | **new** | no in-UI reset exists; the closest today is reflashing the merged image, which is destructive and needs a cable |

**Net for v1**: build a real sectioned Settings app (list of sections → detail, not one flat
three-tile screen) that keeps every existing control (team, address, Wi-Fi, brightness, volume,
dark/light, calm, fps, about) and adds, in priority order: split calm into reduced-motion + render
quality; sleep timeout and auto-dim (cheap, high value for battery and screen life on a cart);
storage usage/retention/wipe (safety — a full SD stops logging, per `docs/systemcore.md`'s storage
threshold table applied to the tablet's own card); a visible boot-record/diagnostics section; and a
link to the existing `link` and `assist` screens rather than reimplementing their controls. OTA and
factory reset are real firmware work, not UI polish — flag them for a follow-on spec rather than
folding them into this pass.
