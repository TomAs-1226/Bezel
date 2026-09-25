# CatalystOS

CatalystOS is what the Tab5 firmware is becoming: a small operating system for a pocket tablet, robotics
first but general purpose. This page is its architecture: what each layer is, why it is built that way,
the numbers it rests on, and what is done and what is next.

## What "OS" means here, and what it doesn't

The goal is the fluidity of a phone-class OS on a microcontroller (Xiaomi's Vela is the reference the
owner named). Vela is built on NuttX, an existing RTOS kernel; what makes it fluid is not the kernel
but the graphics pipeline above it. The same is true here, so:

- **The kernel stays FreeRTOS (SMP, both cores), as shipped in ESP-IDF.** Its scheduler switches tasks in
  microseconds and nothing measured on this device has ever been limited by it. Writing a new kernel
  would cost months and buy nothing a user can see.
- **Everything above the kernel is CatalystOS's own**: the task and core plan, the display compositor,
  the frame scheduler, the services (power, notifications, storage, network, time, the assistant), the
  shell (lock screen, control center, app library) and the app model.
- **Language: C, with the hot loops in the P4's own SIMD (PIE) where it pays.** C is the fastest
  language the toolchain supports on this chip; Rust support for the ESP32-P4 is not mature enough to
  bet a firmware on. The speed that matters comes from not touching pixels, not from the language.

## The hardware budget

Measured on the tablet, not taken from datasheets:

| Resource | Measured | What it means |
|---|---|---|
| Panel | 720 × 1280 portrait, MIPI-DSI, 60 Hz | the UI is landscape: every pixel that changes must be turned 90° |
| PPA rotate (scale-rotate-mirror engine) | 15–20 Mpx/s at any angle | a full frame takes ~50 ms: ~20 fps if every pixel changes. Confirmed as the engine's practical ceiling (docs/research) |
| DMA2D block copy (no rotation) | ~67 Mpx/s | a full frame in ~14 ms: moving pictures that are already turned is cheap |
| CPU copy PSRAM → PSRAM | ~40 MB/s each way | never copy frames with the CPU |
| Internal SRAM | 768 KB, ~12–25 KB free | the scarce resource: everything big lives in the 32 MB PSRAM |
| PSRAM | 32 MB at 200 MHz | frame buffers, pictures of screens, app data |

At 60 fps a frame is 16.7 ms, so a frame may **rotate about 300k pixels** (a quarter of the screen) or
**move about 1 M already-turned pixels**. The whole design follows from those two numbers.

## Layers

### 1. Kernel and task plan

| Core | Tasks |
|---|---|
| 1 | the UI task (LVGL drawing, the shell, the compositor's frame), the boot card |
| 0 | NetworkTables client, Wi-Fi co-processor link (esp-hosted), lwIP, USB tether, camera, CAN, tone, workers (`hal_thread`) |

Any unpinned task above priority 9 is forced onto core 0 at link time (`__wrap_xTaskCreatePinnedToCore`)
so network bursts never steal a frame. Start-up is recorded stage by stage (`hal_boot_*`); after failed
starts the next one comes up in safe mode.

### 2. The display pipeline: the Catalyst compositor

Two paths, chosen per motion:

- **Redraw path** (things that change): LVGL draws only what changed, in landscape; each changed area is
  turned onto the panel by the PPA, or by the CPU when it is a sliver (a rotation a few pixels thin hangs
  the PPA; see `rotate_area`). Presents are paced by vsync and double-buffered; the next frame is drawn
  while the panel scans the last one.
- **Composition path** (things that move): a surface that moves as a whole (a page, an app, the control
  center, the lock screen) is a picture already in the panel's orientation, and each frame is a few DMA2D
  rectangle copies. Nothing is drawn or rotated per frame. A landscape row is a portrait column, so any
  horizontal slide or any vertical sheet is a handful of rectangles.

  | Surface | Motion | Status |
  |---|---|---|
  | pages | horizontal slide under the fixed dock and status bar | done (~58 fps) |
  | control center | top sheet, follows the finger | done |
  | lock screen | top sheet, pushed up | done |
  | apps | bottom sheet, up to open, down to close | done |
  | banners, keyboard, app switcher | sheets / slides | next |

  The picture a moving surface reveals is drawn offscreen a band at a time, only as it comes into view,
  so starting a motion costs a few milliseconds, not a full redraw. At rest the real surface takes over
  without being sent again: the panel's second buffer is synced by DMA.

### 3. Services

| Service | Where | Notes |
|---|---|---|
| power | `ui_shell.c` (SLP), `ui_lock.c` | dim, sleep, wake (the waking tap presses nothing), lock screen |
| notifications | `ui_lock.c`, `ui_shell.c` (the island) | every island message kept; listed in the control center and on the lock screen; the island's orb shows a pip until the control center is opened |
| settings | `hal_kv_*` (NVS) | one key per setting |
| storage | `ui_storage.c` | microSD layout `/sdcard/CATOS/{DOCS,PHOTOS,AUDIO,DATA,LOGS}` |
| network | `hal_tab5_net.c` | Wi-Fi (C6 over SDIO), USB tether, mDNS, HTTPS streaming |
| time | SNTP, RTC | alarms ring with the app closed (`ui_os_boot`) |
| robot | `components/catalyst` (NT4 model) | see docs/catalyst-integration.md for the library contract |
| assistant | `components/assist`, Catalyst Link | Claude via the PC or a key; GPT with a key (branch tab5-companion) |
| audio | `hal_tab5_audio.c` | one speaker task mixing tones and streamed speech (48 kHz); microphones at 16 kHz, on only while the companion is on screen; ESP-SR WakeNet "Hi, ESP" from the `model` partition |
| voice | `components/assist/src/voice.c` | the companion's own conversation: wake word or tap, energy VAD, OpenAI transcription, a JSON reply with a feeling, OpenAI speech streamed to the speaker |
| match alerts | `ui_match.c`, `components/home/src/home_tba.c` | alarms before the team's matches from The Blue Alliance; schedule changes as notifications (below) |
| log analysis | `components/assist/src/analyze.c`, `components/catalyst/src/cat_logs.c` | a log's digest read by a GPT pit engineer (below) |
| battery fleet | `ui_batt.c`, `components/catalyst/src/cat_batt.c` | the team's batteries: which went in when, what the logs measured, which goes in next (below) |
| home mode | `ui_home_mode.c`, `components/home` | the desk surface (time, weather, the PC's music, Home Assistant, the companion) and its launcher: music, smart home, weather, calendar, timer, alarms, photos (and a photos screensaver); comes up by hand, at boot, on the stand, or from an NFC tag on a Unit RFID 2 in Port A |

#### Match alerts

An alarm before each of the team's matches at its current event (team from settings, key `TBA_API_KEY` in
`CATOS/KEYS.ENV`), whatever is on screen.

- **Data.** `tba.h` on the home worker, as the blue alliance app uses it; `ui_match.c` only calls `tba_want()`
  once per background poll (every 2 min on an event day while a match of ours is to come, every 30 min otherwise,
  every 2 min while there is no data or TBA is unreachable; never without a key and a team) and reads our
  upcoming matches back with `tba_upcoming()` when `tba_gen()` moves. No thread of its own: the home worker
  starts for the poll and ends itself ~15 s later. If-Modified-Since as always.
- **Reminders.** At the queue lead (default 25 min: time for the checklist) and the match lead (default 5 min)
  before a match's predicted time (else its scheduled time). A reminder learned of late still rings, never once
  the match's time has passed; when both are due the match one rings alone.
- **The alarm.** A full-screen screen on the glass layer's top, over the pages, apps, home mode and the lock
  screen (kept on top while up; the lock's push and the control center's pull are ignored under it): the match
  and "in N min", our alliance's colour down the edge, the partners and the opponents, the predicted or scheduled
  time, a big **open checklist** (queue reminder: lifts the lock and opens `APP_CHECK`) and **dismiss**. It wakes
  and lights the screen (`bz_ui_wake`; `bz_ui_swallow_cancel` so the first tap presses its buttons). The sound is
  a burst every 2 s (below), escalating in three steps, from 55 % to full level over a minute, with the
  speaker raised to at least 70 % while it rings (restored after); it stops after 2 min (a notification says so),
  and the screen stays until dismissed or 10 min after the match's time. Sound can be turned off.
- **The sound.** Synthesized, not tones: bell-like notes (fundamental, octave, twelfth and a faint 4.2x shimmer,
  the upper partials decaying faster; a 6 ms attack, an exponential decay, a soft limiter), rendered per burst
  into a PSRAM buffer (a few ms of CPU) and played on the speaker's 24 kHz PCM stream (`hal_play_*`; a music or
  speech stream gives way, and if none can be had the old `hal_tone` bursts ring). The **queue reminder** is a
  rising E-major arpeggio (E5 G#5 B5 E6); after 5 bursts a double tap on the top note is added, after 15 a second
  arpeggio a fifth higher, faster and brighter. The **match reminder** is an urgent A5/E6 two-tone: four notes,
  then six faster, then eight ending on a brighter D6/A6. A **schedule change** is a soft G5-D6 chime, once, at
  the tablet's volume (at least 30 %); it doesn't cut a stream that is talking (tones mix over it instead).
- **Schedule changes.** A tracked match whose time moves by 3 min or more from the time last announced, or a new
  match of ours (a playoff), is an island message and a notification with a chime ("Q34 moved to 14:52 (+8 min)");
  its reminders ring again for the new time. One line per poll ("· 9 more of ours changed"); the first schedule
  seen, or a whole schedule appearing, is not news.
- **Lifetime.** Alarms outlive app switches; after a reboot they are derived again from the card cache, then the
  network.
- **Settings.** The blue alliance app's **alerts** chip: on/off, sound, queue lead (off, 15-40 min), match lead
  (off, 3-10 min), the next alarm, and a test alarm. kv `matchalert` = `on,queue,match,sound`.
- **Home mode** shows "next: Q34 · 14:52 · red with 1234, 5678" under the date.
- **Testing.** `python tools/tab5_dev.py COM9 alarm test` rings a made-up Q34 queue alarm in 5 s
  (`alarm test 30`: in 30 s, time to switch apps or let the screen sleep); `alarm test 5 match` the match
  reminder, `alarm test 5 chime` a made-up schedule change (message and chime). The alerts view has a button.

#### Log analysis

The logs app (the card's root, `logs/`, and the recorder's `runs/`) and the recorder's run list have an
**analyze** action: a bounded digest of the log, never the file, goes to OpenAI with a pit engineer's brief.

- **The digest** (`cat_log_digest`, `cat_csv_digest`, at most 14 KB): for a DS log or `.wpilog`, the length,
  battery lowest and mean, brownouts (flag and dips under 6.8 V), trip time, packet loss, CAN and CPU peaks, each
  series in 24 slices, and counters (loop overruns and the worst loop, CAN faults, mode changes, state changes,
  health checks fired), then the kept events. The `.wpilog` reader now also reads Catalyst's own topics when
  NetworkTables was logged: `/Catalyst/Alerts/{Errors,Warnings}` (each new alert an event),
  `/Catalyst/Health/.../firing`, mechanisms' and state machines' `State`, `/Catalyst/Loop/...`, `CanDown`, and
  WPILib's `DS:` mode flags; once the 64 event slots are full a warning or error replaces an info event. For a
  recorder run: per column its samples, range with the times of the extremes, mean, last value and a 12-slice
  trend, and the marks.
- **The agent** (`analyze.c`): a Catalyst pit engineer. Its brief says what the digest is and isn't (an absence is
  never a zero), what Catalyst publishes (mechanisms, health checks, alerts, power, CAN on Systemcore's paired
  buses, the loop monitor, tunables, swerve), the usual causal chains, and a fixed plain-text answer: a verdict,
  ranked causes with confidence and evidence, next checks, what to look at (tunables by path, limits, wiring), and
  what the log can't tell. It says so plainly when the log doesn't support a conclusion.
- **Where it runs.** On the assistant's worker (`assist_post_job`), after any conversation turn in flight: no new
  thread. Key and model are the assistant's OpenAI settings (kv `oai_key`, `oai_model`; default gpt-4o-mini), not
  streamed, no token cap.
- **The answer** shows in the logs app (scrollable), goes to the island and the notifications ("analysis of
  <file>: <verdict>"), and **save** writes `CATOS/DOCS/MMDDHHMM.MD` with the answer and the digest that was sent.

#### Battery fleet

The robot can't know which of the team's batteries is in it (`RobotIdentity.battery("MK ES17-12")` is the model,
`/Catalyst/Robot/Power/Battery`, shown in the app's footer), so the tablet is the source of truth for that.

- **The roster** (the **batteries** app, robot group; 12 by default, "1".."12", renamable, add and remove, 24 at
  most): each battery's status (good, watch, bad, retired), notes, year bought, "off the charger now", and its last 30
  uses. A tile shows its resistance now (the median of its last three measured uses), its charge state and uses.
- **The checklist** has a battery row at the top: a tap opens a picker of big tiles (the recommended one in ice, bad
  and retired ones greyed), charge chips (fresh off charger, rested, not charged), and **mark bad**. A pick records the
  battery, the time, the next match from TBA (its key, `2026casj_qm34`, and label) and the charge. A second pick
  within 20 min for the same match, with nothing measured yet, replaces the first (a correction).
- **The logs.** `.wpilog` and `.dslog` in the card's root and `logs/` are read on the assistant's worker
  (`assist_post_job`, 6 a job, each once: a hash of name, size and time is kept), 40 s after start-up, when the
  batteries or logs app opens, and on **read logs**. A log goes to the pick of its qualification match (FMS
  `MatchNumber`/`MatchType`, or the `FRC_<date>_<time>_<event>_Q34` file name), else to the newest pick at most 4 h
  before it started (or 10 min after). Its numbers: resting voltage before the load (before `DS:enabled`, or 8 A),
  lowest voltage, brownouts (the controller's flag, else dips under 6.8 V), energy (Wh), mean and peak current, and
  the internal resistance: V = V0 - I·R fitted by least squares in 8 s windows with at least 15 A of spread, the
  median of the windows (a steady load gives none, as in Catalyst's `BatteryResistanceIdentifier`). Voltage from
  `/Catalyst/Brownout/MeasuredVoltage`, `Status/BatteryVolts` or `Systemcore/BatteryVolts` first, then
  `…BatteryVoltage`, then the DS's; current from `…/TotalCurrent`. A `.dslog` has no total current: voltage and
  brownouts only.
- **Live.** While a pick is under 4 h old and has no log yet, the robot's battery voltage and
  `/Catalyst/Brownout/TotalCurrent` (else a PDH's `TotalCurrent`) are folded in at 10 Hz; a session ends 15 s after
  the robot is disabled. A log's numbers replace live ones.
- **The recommendation** ranks every battery with its reasons in words ("#7: lowest resistance (18 mohm), rested 2 h,
  charged"): charged since its last use (+), rested 30 min off the charger (+), resistance (lower better, rising 15 %
  over its baseline −), brownouts in its last three uses (−), on watch (−), and uses today against the fleet's mean.
  Bad, retired, or in within the last 2 h and not marked charged since: out. A battery whose resistance is over
  25 mΩ (two measured uses) or 30 % over its baseline (the median of its first three) goes on watch by itself, with a
  notification. The match alarm's screen says "battery: #7 · …" (or "battery in: #7" once picked).
- **ask gpt** sends a summary (`cat_batt_summary`: each battery, its last six uses, the tablet's ranking) to the
  analysis path with a battery lead's brief (`analyze_fleet_start`); the answer shows in the app and the island.
- **Data.** `CATOS/DATA/batteries.json`: `{version, updated, next_uid, seen: [hashes], batteries: [{uid, label, status,
  auto_watch, year, notes, charged, base_mohm, uses_total, uses: [{t, match, label, charge, charged, src ("p" pick,
  "l" log, "n" live), v_rest, v_min, mohm, wh, amps, peak_a, dur_s, brownouts, log}]}]}`; times are unix seconds; a
  number not measured is absent, never 0. Written from the UI thread after each change, retried every 2 s for a
  minute (the card's EIO after start-up). Without a card, the roster alone goes to kv `batteries`.
- **Testing.** `python tools/tab5_dev.py COM9 bms` prints the ranking and the alarm line; `bms demo` fills made-up
  history on the first six, `bms reset` restores the default 12, `bms pick 7`, `bms scan`, `bms gpt`, `bms json`.
  Unit tests: `test/test_batt.c`.

### 4. The shell

Home, robot, devices, power, motion and the app library as pages; the status bar and the island; the control
center (a pull from the top edge anywhere); the lock screen; the assistant's orb (bottom right, over the pages
only).

**The island** (`ui_shell.c`) never rests over content. At rest it is a 48 px orb in the top-right corner, over
the pages, apps and home mode alike: the robot's state is its colour and mark (● connected, ◆ a warning, ■ a fault
or e-stop, ○ looking for the team), and a small amber pip means notifications not yet seen. A tap opens the
control center, which lists them (and the link's detail). A message (`ui_island_say`, kept as a notification)
pulses the orb amber three times while a capsule grows leftwards out of it on the `release` spring, its words
fading in once it is 55 % grown; it holds 3 s and shrinks back into the orb on `smooth`. A message arriving
while one shows reshapes the capsule and restarts the hold. The link coming up ("robot · teleop · 12.41 v") or
going ("lost the robot") shows the same way, once it has held a moment, but isn't kept. Cost: the capsule's own
strip redraws while it moves, the orb's 48 px disc while it pulses, nothing at rest; nothing moves while a page
or sheet picture slides.

Layout contract: whatever sits at the band's right ends `ORB_CLEAR` (64 px) short of the page padding: the status
cluster (link, battery, clock, centred on the orb) and every app head's buttons are aligned at `HEAD_RIGHT_X`
(`ui_internal.h`). A page's head context can now run to the status cluster (`ui_head_width`).

**Look** (settings > look). The tone (dark, light); the **accent**, one of eight curated colours (ice, orange,
leaf, violet, rose, amber, teal, white; `BZ_ACCENTS` in `bz_tokens.c`, each with a dark-tone fill and a light-tone
container and their on-colours), which is Bezel's `ice` role: selected chips and modes, levels, meters, lit
tiles, the companion's eyes. `signal` (orange) stays the one signal colour for the primary action and the team
itself. Changing it (`bz_ui_set_accent`) rebuilds the palette, rewrites the shared styles and restyles every
object (`lv_obj_report_style_change`), then redraws the screen once (~40 ms); every screen, built or not, takes
it, because nothing stores a raw accent colour. Home mode's face: which cards show (music, companion, smart home,
weather, next match; the middle row closes up and the last card takes the rest of the width), the clock (big or
the smaller display face, seconds beside it or not: one small label a second) and the ground (plain, or a solid
tint of the accent). kv: `accent` (the accent's name), `hm_cards` (bits: music 1, companion 2, smart home 4,
weather 8, next match 16), `hm_clock` (1 small), `hm_secs`, `hm_bg` (1 tinted).

Dev console (`tools/tab5_dev.py COM9 "..."`): `say <text>` sends a message through the island, `accent <name|n>`
sets the accent, `settings <section>` opens settings on a section (`settings look`).

### 5. Apps

Today every app is built in: a `ui_app_t` with build/open/close/refresh/frame, 34 of them in four groups
(robot, diagnose, everyday, this tablet).

Next, apps installed from the microSD card: native code loaded with Espressif's `elf_loader` component
(supports the P4 and running from PSRAM), calling a stable C API (`catos.h`: the LVGL subset, the shell's
widgets, the services above) through an exported symbol table. Native code rather than WebAssembly
because WASM interpreters and even AOT runtimes are markedly slower, and the UI is the hot path. There is
no memory protection between apps on this chip: an installed app is trusted code.

## Roadmap

| Phase | What | Status |
|---|---|---|
| 1 | Boot, recovery, lean renderer, dirty-area presents, page slides in DMA2D | done |
| 2 | Compositor sheets: control center, lock, apps; system edge gestures | done |
| 3 | Notifications, lock screen, sleep/wake, everyday apps, card layout | done |
| 4 | Pipelined present: the PPA/DMA2D present on core 0 while core 1 draws the next frame | next |
| 5 | PIE-accelerated fills, blends and copies in the redraw path (~1.4× on PSRAM copies, per Espressif's numbers) | next |
| 6 | More compositor surfaces: banners, keyboard, an app switcher of live cards | next |
| 7 | Apps from the microSD card (`elf_loader`, `catos.h`, a manifest per app) | planned |
| 8 | Assets on the card: wallpapers, extra fonts and icon sets, loaded at start | planned |
| 9 | Audio playback (WAV/MP3 from the card) through the ES8388 codec | planned |

Each phase is measured on the tablet before it counts as done: frame rate from the heartbeat, frame
cost by stage, and screenshots of the panel.
