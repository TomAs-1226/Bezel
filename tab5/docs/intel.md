# The intelligence core

The tablet watches everything it already knows (the robot and its last run, the event's schedule and live
queue, the battery fleet, the weather, the tablet itself) and says, briefly and at the right moment, what
needs doing. It is a service, not an app: its words show on the island, in the notifications, in the
home page's and home mode's greeting line, in the control center's ask tile, and in the insights app, and
both AIs can read it. Code: `components/intel` (the sources, the rules, the store, the intents, the
briefing: portable C, the pure half host-tested in `test/test_intel.c`) and `components/ui/src/ui_intel.c`
(the pass on the UI thread, the island, the app, the tile).

## The sources

| Source | What | Key | Where | How often |
|---|---|---|---|---|
| The Blue Alliance | the event, our matches, rank (as before: `home_tba.c`) | `TBA_API_KEY` | home worker | 2 min on an event day with a match to come, 30 min otherwise |
| **Nexus for FRC** | the live queue: what is queuing now, each match's estimated queue / on-deck / on-field / start time, announcements, parts requests | **`NEXUS_API_KEY`** | `intel_nexus.c`, home worker | 30 s while a match of ours is within the hour on an event day, 2 min otherwise on that day, never on other days |
| Statbotics v3 | EPA, world and state rank, the event's EPA and record, each of our matches' win chance and predicted score | none | `intel_sb.c`, home worker | season hourly; event and matches 5 min on an event day, hourly otherwise |
| Systemcore's vision aggregator | every Limelight, its address, whether it holds an NT session | none (robot LAN) | `intel_rn.c` | 20 s while a robot is connected; 2 min once absent (a roboRIO has none) |
| Each Limelight's REST | `:5807/status`: fps, temperature, cpu, pipeline | none | `intel_rn.c` | 20 s each |
| CTRE Phoenix diagnostics | `:1250/?action=getdevices`: every device's model, id, firmware | none | `intel_rn.c` | a minute; 5 min once absent |
| catalyst-agent | throttling, temperatures, storage (already polled by the Systemcore screens) | none | `cat_sc_io.c` | one snapshot every 30 s while a Systemcore robot is connected |

No thread of its own. The requests run on the home services' worker through `tba_ext_register` (`tba.h`):
one request per worker pass, so the PC's media and Home Assistant keep their turns, and the worker is
started (`tba_ext_want`) only when a request is due and ends ~15 s after, giving its internal-RAM stack
back. Bodies are read into PSRAM, bounded per source (384 KB Nexus, 256 KB Statbotics, 96 KB robot). The
internet sources ask only while Wi-Fi has an address. A failure keeps the last answer, marked stale.

Nexus's fields could not be checked against a live answer (no key when this was written), so its parser
reads defensively: times in milliseconds or seconds, a few other spellings of each field, teams as numbers,
`"254"` or `"frc254"`, a label like `Qualification 34`, `Playoff 5`, `Match 5 (R2)` or `Final 2` mapped to
TBA's `Q34`, `SF5`, `F2`. Without a key the core says "add NEXUS_API_KEY to KEYS.ENV" and the alarms use
TBA's times.

## The match alarms take Nexus's times first

TBA is the past, Nexus is the next ten minutes. `ui_match.c` tracks each of our matches by TBA's identity
and times it with `mt_times` (`intel.h`, host-tested):

- the match's time is Nexus's estimated start when Nexus has the match, else TBA's predicted, else the
  schedule's;
- the **queue reminder** rings at that time less the queue lead (default 25 min), or at Nexus's estimated
  queue time less a 5-minute walk when that comes first, or **at once when Nexus says the match is
  queuing**;
- the **match reminder** rings at the time less the match lead, or at once when Nexus says it's on deck;
  nothing rings once Nexus has it on the field, and nothing past the match's time unless Nexus says it is
  still queuing (the field running late);
- a Nexus status older than 5 minutes isn't taken as "now";
- the alarm screen counts to Nexus's queue time and says where its times came from ("queue ~16:13, start
  ~16:28 (nexus)", or "tba predicted" / "scheduled"), and so do the island line and the schedule-change
  notes ("Q34 now ~14:52 (+8 min) · nexus"; Nexus's estimates drift, so a move is news at 5 min, TBA's at 3);
- a match of ours only Nexus has (a playoff TBA hasn't posted yet) is tracked too;
- for the last 15 minutes before Nexus's queue time the queue is a live activity on the island ("Q34
  queues", the time left folded into the orb).

## Insights

A pass every 2 s on the UI thread's 10 Hz refresh (never per frame, and held while a finger moves something
or a page slides; 0.3-1 ms measured on the tablet, 3 ms the worst seen): a
snapshot of everything above (`intel_in_t`), the rules (`intel_rules.c`), the store's merge, at most one
island message. Each enabled run is followed at 10 Hz for its brownouts, lowest voltage and every swerve
module's following (actual velocity along the commanded one, so a module reporting its 180-degree twin
still follows).

| Insight | Priority | Says |
|---|---|---|
| match prep | 2 from 45 min before the queue when there is a battery to pick or a fault on the robot; 3 while queuing only with a fault; else 1 | "Q34 queues ~14:32 (38 min) · put in #7 · FL lagged 30% last run · 62% to win" |
| field late | 1 | "the field runs ~12 min behind: Q34 now ~14:52" |
| a low battery in the robot, a match within the hour | 2, 3 within 25 min | "the robot's battery reads 12.10 V: swap it before Q34" |
| brownouts last run | 2 | "2 brownouts last run, lowest 6.4 V: check the battery and the main breaker" |
| one swerve module short | 2 | "FL module followed 70% of its command last run: check it before queuing" |
| every module short alike | 1 | "every module under-followed last run (~72%)" |
| CAN | 3 bus down, 2 devices missing | "3 of 22 CAN devices aren't answering" |
| robot errors, loop overruns, uncommitted code | 2, 1, 1 (2 on an event day) | |
| Systemcore throttling, hot, storage low | 2 | |
| a camera without NT or not answering; hot | 2; 1 | "limelight-left: no NT session" |
| one device model on two firmwares (Phoenix) | 1 (2 on an event day) | "Talon FX on two firmwares: 26.1.0.0 x2, 26.70.0.0 x1" |
| a Nexus announcement in the last 45 min | 2 | "event: lunch until 13:00" |
| another team's parts request on Nexus, last 30 min | 1 | "team 1234 needs a 1/2 in hex bearing" |
| EPA and rank | 1 | "EPA 31.2, #312 of 3700 (top 9%) · rank 45 of 60 here" |
| the calendar within the hour; rain | 2 within 20 min, 1; 1 | |
| the tablet low | 2 | "this tablet is at 18%: charge it" |
| Nexus not set up on an event day; refused | 1; 2 | |

**Never nag** (`intel_store.c`, host-tested): only priority 2 and 3 are said; each insight at most once (its
key is remembered for an hour after it disappears, so a flapping rule isn't said twice; a rise to priority 3
is said again); a priority-2 message at most every 10 minutes; at most six messages in any hour whatever
their priority; nothing in the quiet hours (default 22-7) or of a category turned off; a dismissed one stays
dismissed. Everything is listed in the insights app regardless.

## The briefing

The day, the event (rank, EPA), the next matches with Nexus's times and Statbotics' odds, the queue, the
battery, the robot and its last run, vision, firmware, the calendar, alarms and the tablet, one line each,
then what the tablet noticed (`intel_briefing`, deterministic). It is in the insights app beside the list,
comes by itself at the time set in settings, and on an event day's morning. **summarize** (or the daily
briefing with "ai summary" on) asks OpenAI for three sentences (`assist_complete` on the assistant's
worker, the assistant's key and model), at most eight a day.

## The ask bar

"Ask or do anything": the insights app's top bar, the control center's ask tile, a long press on the
assistant's orb, or `open_app` "insights". Typed words are matched first to what the tablet does at once
(`intel_intent`, host-tested), done through the same tools the AIs use (`ui_actions_do`): a timer ("timer 5
min", "5 min timer tea", "90s"), the stopwatch, brightness and volume ("brightness 40", "brighter"), mute,
dark or light, home mode, sleep or lock, "next match" (Nexus's times), "battery for q34",
"queue", "epa", "brief me", "what's up", "weather", "note ...", "remind me in 10 min to ...", "alarm 6:30",
"open <app>" or an app's name. Anything else is a question: it goes to the technician or the companion
(chips beside the bar; kv `intel_ai`), who have the tools to answer it.

## The AIs' tools

| Tool | Returns |
|---|---|
| `insights` | what the tablet noticed, worst first, with where each came from; today's briefing; which sources are connected |
| `event_live` | Nexus (now queuing, each of our matches' status and times, how many away, announcements, parts requests) and Statbotics (EPA, ranks, event record, each match's win chance); asks again when older than 90 s and waits a few seconds |
| `robot_network` | the aggregator's cameras, each Limelight's status, Phoenix's devices and firmware |

Every question's context also ends with the most important insight ("noticed: ...", `AS_DESK_INTEL`).

## Settings

The insights app's **settings** view (kv `intel` = `cats,quiet_from,quiet_to,brief_hhmm,ai`): what the island
may say (matches, the robot, power, the event, the day, sources), quiet hours (none, 22-7, 23-7, 21-8), a
morning briefing (off, 6:30 ... 9:00), the AI summary, clear the insights, and each source's state with the
key it lacks.

## Testing

- Host: `test/test_intel.c` (the parsers against made-up documents, the alarm timing, the rules on made-up
  days, the store's limits, the intents, the briefing). `tab5-shots/intel/host_tests.bat`.
- Dev console (`tools/tab5_dev.py COM9 "..."`, or `tab5-shots/intel/tabcmd.py` for the printed lines):
  - `assist intel` prints the sources, the pass's cost and every insight; `assist intel brief` the
    briefing; `assist intel summary` asks for the model's; `assist intel ask <words>` runs the ask bar;
    `assist intel open`, `assist intel view 0|1|2|3`; `assist intel run 70 2` a made-up last run (FL at 70 %,
    two brownouts); `assist intel clear`.
  - `assist nexus` prints Nexus's state; `assist nexus fake <queue in min> [status]` feeds a made-up
    document (a Q99 of ours) through the parser as if it came from Nexus; `assist nexus base <url>|off`
    points the client at a fake server (a key isn't needed then); `assist nexus event <key>|off` follows
    another event as if it were today's; `assist nexus refresh`.
  - `tab5-shots/intel/intel_fake.py` serves a made-up Nexus event, an aggregator, a Limelight and a Phoenix
    server from the PC; with `tools/fake_robot.py` and `assist robot <pc>` the robot-network path runs end
    to end.
