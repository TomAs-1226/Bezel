# The tablet's two AIs: memory, tools, prompts, the island

Catalyst Tab has two AIs, and they are kept apart on purpose.

| | The technician (Assist) | The companion |
|---|---|---|
| Where | the Assist app, the orb | her face (desk mode), home mode's wake word |
| Talks | text only: it never speaks | a young woman's voice (OpenAI speech), and text on screen |
| Model | Claude (through the PC's Catalyst Link, or a key) or OpenAI (`ai_route`, `oai_model`, default gpt-5) | OpenAI Chat Completions (`v_chat`, else the assistant's OpenAI model) |
| Code | `components/assist/src/assist.c` | `components/assist/src/voice.c` |
| Prompt | `as_prompts.c` `AS_PROMPT_TECH` | `as_prompts.c` `AS_PROMPT_COMPANION` |
| Memory | its own pool | her own pool |
| Changes the robot | only through an approval card (tunables, the auto, a patch) | never |

## Memory

Each AI keeps a long-term memory that lasts across conversations and restarts. Neither can read the
other's: which pool a memory tool touches is decided by the conversation that called it
(`as_env_t.pool`), never by anything the model writes, and `as_tool_run` refuses a tool that is not the
conversation's.

A pool (`as_mem.h`, pure and host-tested) is:

- **Self sections** the model rewrites whole, up to 600 characters each. The technician's is its *season
  notebook*: `robot` (what it is, its quirks and history), `team` (procedures, preferences, who does
  what), `season` (events, matches, results: the campaign so far), `watchlist` (recurring faults and open
  problems). The companion's is her *persona*: `name` (once the owner gives her one, 40 characters),
  `personality`, `relationship`, `running_jokes`. Her persona shapes how she speaks, and her name is how
  the island names her.
- **Memories**, at most 120: one durable fact each (280 characters), a kind (`fact`, `preference`,
  `decision`, `fix`, `fault`, `procedure`, `event`, `person`, `plan`), an importance (1 minor, 2 useful,
  3 important), when it was made and changed, and how often it was recalled.

Rules the store enforces, whatever the model does:

- A memory that says nearly what one already says (three-quarters of the same words, stems counted,
  digits never) updates that one instead of sitting beside it.
- Full: the least valuable memory of no greater importance makes room, and the result says which; if every
  memory matters more, it refuses.
- Anything that looks like a secret is refused: a known key's prefix with a long tail (`sk-`, `ghp_`,
  `AKIA`...), "password is ...", "api key: ...", a long run of mixed letters and digits.
- Typography the tablet's faces don't carry (curly quotes, the en dash, odd spaces) is folded on the way in.

### What the model is shown

At the start of a conversation, once: its self sections, then as many memories as fit a budget (3200
characters for the technician, 2000 for the companion), the most valuable first, where value is
importance, then how recently it changed, then how often it was recalled, with a boost for memories
sharing words with the first question. Then how many more there are ("recall finds them"). The
technician's goes into the first message's context note, which survives history trimming and keeps the
cached prefix stable; the companion's is a second system message fixed for the conversation (a new
conversation starts after 10 minutes of quiet, or "new"). `memo digest a|c [question]` prints exactly
what the model would see.

### The tools

| Tool | Who | Does |
|---|---|---|
| `remember` | both | one fact, kind, importance; `replaces` an id to correct one |
| `forget` | both | a memory by id |
| `recall` | both | by words, by kind, or the most important; ids, dates, text |
| `update_notebook` | technician | rewrites a notebook section |
| `update_persona` | companion | rewrites a persona field |

The prompts say when to remember (a fault found or fixed, a part replaced, a procedure or preference
stated, a decision, a result that will matter later; for her, what a good friend would remember) and when
not to (live readings, anything a tool can read again, guesses, small talk, and never a secret).

### Where it is kept

`<sd>/CATOS/DATA/MEM-ASSIST.JSON` and `MEM-COMPANION.JSON` (`as_memio.c`):

```
{"version":1, "pool":"technician", "next_id":13, "updated":<unix>,
 "self":{"robot":{"text":"...","at":<unix>}, ...},
 "memories":[{"id":1,"kind":"fix","imp":3,"text":"...","created":<unix>,"updated":<unix>,"uses":0}, ...]}
```

- A pool loads the first time something asks for it, long after the card's EIO spell at start-up; a load
  that fails is tried again 2 s later, and **a pool that failed to load is never written**, so a card
  that hiccups can't have its memories replaced by an empty pool. A file that doesn't parse is kept aside
  as `MEM-*.BAD`.
- Writes go through `MEM-*.TMP` and a rename, from the thread that made the change; a failed write is
  retried from the UI's 10 Hz tick (every 2 s for a minute, then every 30 s). A `.TMP` found without its
  file (a write cut short) is recovered.
- Without a card a pool lives in kv (`mem_tech`, `mem_comp`): the self sections and as many of the most
  valuable memories as fit in 3.8 KB, written only from the UI thread (NVS must not be written from a
  thread whose stack may be in PSRAM). When a card appears, what kv kept moves onto it.

### Seeing and deleting

The **memory** screen (`APP_MEMORY`, `ui_app_assist.c`): chips for whose pool, the self sections with an
× each, the memories with an × each (tap twice: the second tap forgets), and **forget everything** (twice).
It is reached from settings → assistant ("memory"), the Assist prompt bar's memory button, the companion's
controls, `open_app` "memory", and `memo open a|c`.

## System tools

Both AIs can do things to the tablet, never to the robot, without asking first (`as_tooldefs.c`,
`AS_T_DEVICE`, carried onto the UI thread by `ui_actions.c`). Each goes through the app that owns the data
(`ui_sys.h` hooks), whether or not that app has ever been opened:

| Tool | Does | Where the data lives |
|---|---|---|
| `open_app` | an app by its name, a settings section, or "memory" | |
| `set_tablet` | brightness, volume, tone, accent, calm, screen sleep or lock, home mode | the settings (`S`, saved) |
| `tablet_status` | battery, Wi-Fi, time, uptime, memory, card, look, timer, alarms, reminders | |
| `timer` | countdown (with a label), match clock, stopwatch, pause, resume, reset, status | the timer app |
| `alarm` | list, add at HH:MM on days, on, off, remove | the clock app (kv `alarms`) |
| `reminder` | a few words at a time: in N minutes or at HH:MM; list; cancel | kv `reminders` (8 at most) |
| `notes` | read, append (with the time), replace | the notes app (kv `notes`, `CATOS/DOCS/NOTES.TXT`) |
| `documents` | list, read, write markdown | `CATOS/DOCS` (`ui_docs_write`) |
| `checklist` | status, check or uncheck an item (by number or words), reset | the checklist app (kv `checklist`) |
| `calendar` | list from a date for N days, add an event | the calendar (`CATOS/DATA/EVENTS.TXT`) |
| `weather` | now, today, the hours, the week | home's weather (Open-Meteo) |
| `notifications` | read the newest, clear | the island's notifications |
| `music`, `smart_home` | the tablet's or the PC's music; Home Assistant's picked entities | home |

Card reads, document writes and the weather run on the calling AI's worker; the rest is parked for the UI
thread and answered within 4 s (a request the worker stopped waiting for is finished and freed by the UI,
never answered into the next one). Anything that would take the screen from the companion (another app,
sleep, the lock screen, home mode) waits until she has finished speaking.

## The pet (the companion's alone)

The companion's context line also carries the desk pet's state and its last few diary entries (`home_pet_brief`,
handed in with `assist_set_pet_context` because `home` requires `assist`), and she has four tools the technician is
never offered (`AS_T_PET`, `as_tool_for`): `pet_status`, `pet_feed {food}`, `pet_play`, `pet_rename {name}`. They run
on her worker, thread-safe, and the pet page or idle face reacts when one lands. A hungry pet may be mentioned once
every three hours, and the prompt says to do it lightly and only when it fits. See pet.md.

## The intelligence core's tools

Both AIs read what the tablet's intelligence core knows (docs/intel.md), served through the ui's handler on
the calling AI's worker:

| Tool | Returns |
|---|---|
| `insights` | what the tablet noticed, worst first, each with where it came from; today's briefing; the sources' state |
| `event_live` | Nexus's live queue (now queuing, our matches' status and estimated times, announcements, parts requests) and Statbotics (EPA, ranks, each match's win chance); asked again when stale |
| `robot_network` | Systemcore's vision aggregator, each Limelight's REST status, CTRE Phoenix's devices and firmware |

Every question's context line also ends with the core's most important insight ("noticed: ...").

## Diagnostics

- `triage`: every anomaly the tablet can see, ranked worst first in the triage order (link and program,
  power, CAN, loop, the robot's own checks, alerts, mechanisms, the drivetrain, vision, Systemcore), each
  with its numbers and the next check. Thresholds are Console's (catalyst-contract.md, systemcore.md),
  including Systemcore's shared CAN controllers (`can_s0`+`can_s1`, `can_s3`+`can_s4`).
- `swerve_check`: each module's commanded against actual speed and steering, compared as velocity vectors
  (a module reporting its 180° twin still follows), over 1-10 s live or a recorder CSV with the drivetrain
  preset. One module that doesn't follow is named as hardware (a jammed drive, a loose wheel or pulley,
  that motor); all of them under-following alike is global (battery, current limits, feedforward, wheel
  radius); a steering error names the CANcoder and azimuth. This is the first check on a bad drivetrain
  run: a jammed drive or a loose wheel otherwise passes for a tuning problem.

Both are pure (`as_diag.c`) and host-tested against made-up robots.

## The island

What the AIs do shows on the island's live activities (`ui_activity_*`), through hooks the ui hands the
assistant (`assist_set_activity_hooks`):

- **A turn**: "assist · reading the alerts", "Ivy · checking the weather". Only while that conversation's
  own screen is not showing: leave mid-answer and it appears. The technician's answer, finished out of
  sight, is said when its turn ends ("assist: ..."); the companion's replies are said out of the island
  whenever her face is closed.
- **A write**: "editing pit-plan", "patch: ...", "work order: ...", from the moment it starts, wherever
  the owner is. The technician's begins while the model is still streaming the tool's input (it takes the
  document's name when it arrives, even after the body). When it is done it leaves a word: "saved the
  notes", "patch on <branch>", "work order sent to the PC" (a document is announced by the documents app
  itself).
- **A timer** either AI starts: "tea" with its time left, folding into the orb between its moments; at the
  end it rings three pairs of notes at least at half volume, over any app, home mode or the lock screen,
  and leaves "timer done · tea".

Closing the companion stops her listening and speaking, never a question she is answering: its tools run
and its writes finish. Only a stop abandons a turn. Closing Assist never stopped anything.

## The companion's voice

`VO_VOICE_DEFAULT` is `marin` (OpenAI's newest female voice, on `gpt-4o-mini-tts`). A voice a model or key
refuses falls back to `coral` for as long as the setting stays; `tts-1`, which has neither marin nor
cedar, gets `nova` (`as_tts_voice`). The speech `instructions` describe a warm, bright young woman talking
across the desk, plus the feeling of the line. The settings' picker lists the female voices first. Each
speech request is logged: `voice: POST /v1/audio/speech gpt-4o-mini-tts/marin -> 200 in 1.9 s`.

Her conversation history keeps each turn's tool calls and results (cut to 600 characters), not only the
words around them: a history of "okay, the timer's started" with no timer call in it taught the model to
say it and not do it.

## Testing

- Host: `test/test_assist.c` (the memory store, triage, swerve tracking, the prompts' tool names, which
  conversation gets which tool, the voices, the island's labels).
- The dev console (`tools/tab5_dev.py COM9 "..."`; for the console's own output use a serial reader such
  as `tab5-shots/brain/tabcmd.py`, since tab5_dev prints only its replies):
  - `ask <text>` the companion; `assist ask <text>`, `assist new`, `assist dump` the technician;
  - `assist act <tool> <json>` runs one tablet tool as a model's call would (`assist act timer
    {"action":"status"}`), no model needed; the notes take `{"action":"clear"}` here (never from a model);
  - `memo`, `memo a|c`, `memo digest a|c [question]`, `memo forget a|c <id>`, `memo clear a|c`,
    `memo open a|c`;
  - `assist robot <address>` points the tablet at a robot (`tools/fake_robot.py --host 0.0.0.0
    --scenario jam` has the back-left module following at half its command); `assist robot` alone goes
    back to the team's.
- The console logs `island: activity N begins|now|ends`, `timer: timer done · <label>, ringing`, and the
  voice's requests.
