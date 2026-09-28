/* The tool definitions: one schema per tool, sent to the API and used to validate what comes back. */
#include "as_tools.h"

#include <stdio.h>
#include <string.h>

#include "as_mem.h"

#define NOARGS "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"

#define KINDS "[\"fact\",\"preference\",\"decision\",\"fix\",\"fault\",\"procedure\",\"event\",\"person\",\"plan\"]"

const as_tooldef_t AS_TOOLS[] = {
    /* ---- the robot, read ---- */
    { "triage",
      "The robot's health in one call, worst first: every anomaly the tablet can see, across the link and the "
      "robot program, power (battery, brownouts), CAN (a bus down, devices not answering, utilisation, "
      "Systemcore's shared controllers), the loop, the robot's own preflight and checks, alerts and health checks, "
      "mechanisms (temperature, stalls), the drivetrain (modules against their commands, while it drives), vision "
      "and Systemcore. Each finding has a severity (fault, warn, info), what it is with its numbers, and the next "
      "check. Start a diagnosis here, then narrow with the specific tools.",
      NOARGS, AS_T_READ },
    { "robot_overview",
      "The whole robot at a glance, as facts rather than findings: identity and versions, mode, battery and "
      "brownout, loop time, CAN load, device roster, alert counts, Systemcore, the NetworkTables link and the "
      "Catalyst Link to the PC. triage says what is wrong; this says what it is.",
      NOARGS, AS_T_READ },
    { "swerve_check",
      "Whether each swerve module follows its command: commanded against actual speed and steering angle, per "
      "module, judged over many samples, with a verdict. One module that doesn't follow while the others do is "
      "hardware (a jammed drive, a loose wheel or pulley, that motor); all of them under-following alike is global "
      "(battery sag, current limits, feedforward, wheel radius). Live by default: samples the robot for `seconds` "
      "(4 if not given) and needs it driving. Or give `recording`, a name from list_recordings made with the "
      "recorder's drivetrain preset. The first check on any bad drivetrain run.",
      "{\"type\":\"object\",\"properties\":{\"recording\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":48},"
      "\"seconds\":{\"type\":\"number\",\"minimum\":1,\"maximum\":10}},\"additionalProperties\":false}",
      AS_T_READ },
    { "get_alerts",
      "Every active alert (AlertManager, WPILib alert groups, firing HealthMonitor checks), worst first, "
      "plus the robot's own preflight findings.",
      NOARGS, AS_T_READ },
    { "get_mechanisms",
      "Each mechanism Catalyst publishes (/Catalyst/<name>/…): state, position against goal, velocity, "
      "current, temperature, at-goal; and the swerve modules, actual against commanded, at this one instant "
      "(swerve_check judges them over time).",
      NOARGS, AS_T_READ },
    { "get_power",
      "Battery voltage, brownout floor and prediction, total current, and the power distribution channels "
      "with what each feeds and its live current if the robot publishes the PDH.",
      NOARGS, AS_T_READ },
    { "get_can",
      "CAN devices grouped by bus with their connected state, each bus's utilization, bus-down flags and "
      "the device roster's expected/connected counts.",
      NOARGS, AS_T_READ },
    { "get_vision",
      "Vision health: level, each camera's state, frame rate and temperature, and the tag in view.",
      NOARGS, AS_T_READ },
    { "run_preflight",
      "Runs the tablet's preflight: listens to the robot for about 3 seconds, then returns GO or NO-GO "
      "with every failure and warning. Changes nothing.",
      NOARGS, AS_T_READ },
    { "list_topics",
      "Lists NetworkTables topic names and types under a prefix (e.g. \"/Catalyst/Elevator/\"). At most "
      "200 names.",
      "{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":120}},"
      "\"required\":[\"prefix\"],\"additionalProperties\":false}",
      AS_T_READ },
    { "read_topics",
      "Reads the current values of up to 40 NetworkTables topics by full name.",
      "{\"type\":\"object\",\"properties\":{\"names\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":40,"
      "\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":160}}},"
      "\"required\":[\"names\"],\"additionalProperties\":false}",
      AS_T_READ },
    { "list_tunables",
      "The tunables the robot declares (/Catalyst/Tunables/.manifest): key, name, group, unit, range, step "
      "and current value. Only these can be changed.",
      NOARGS, AS_T_READ },
    { "list_autos", "The auto chooser's options, the selected one and the active one.", NOARGS, AS_T_READ },
    /* ---- the team, read (the tablet's own data, no robot needed) ---- */
    { "get_matches",
      "The team's event from The Blue Alliance: rank and record, every match of ours with its time, alliance, "
      "partners, opponents and result, and when the queue and match reminders ring. Use it for \"when do we "
      "play\", \"who are we with\", \"how are we doing\".",
      NOARGS, AS_T_READ },
    { "get_batteries",
      "The team's battery fleet from the tablet's battery manager: each battery's status (good, watch, bad, "
      "retired), internal resistance and its trend, charge, recent uses with the lowest voltage and brownouts "
      "per match, and the ranking of which to put in next with why.",
      NOARGS, AS_T_READ },
    { "list_snapshots",
      "Snapshots of every tunable taken before each change the tablet made, newest first, with what "
      "differs from the robot now.",
      NOARGS, AS_T_READ },
    { "list_logs", "Driver Station and robot logs on the tablet's microSD card (.wpilog, .dslog, .dsevents).",
      NOARGS, AS_T_READ },
    { "summarize_log",
      "Summarizes one log from list_logs: duration, lowest battery, brownouts and voltage dips, trip time, "
      "packet loss, CAN and CPU peaks, and the first events.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":120}},"
      "\"required\":[\"name\"],\"additionalProperties\":false}",
      AS_T_READ },
    { "tba_query",
      "Reads The Blue Alliance directly, for anything beyond our own event (get_matches is ours). "
      "`path` is one of: /team/frc<N>/simple, /team/frc<N>/events/<year>/simple, "
      "/team/frc<N>/event/<eventKey>/status, /team/frc<N>/event/<eventKey>/matches/simple, "
      "/team/frc<N>/awards, /event/<eventKey>/simple, /event/<eventKey>/rankings, "
      "/event/<eventKey>/oprs, /event/<eventKey>/matches/simple, /event/<eventKey>/teams/simple, "
      "/events/<year>/simple, /match/<matchKey>/simple. An event key looks like 2026casj; a match "
      "key like 2026casj_qm34. Nothing else is fetched. A long answer comes back as whole rows with "
      "the count of what was left out.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":2,"
      "\"maxLength\":120}},\"required\":[\"path\"],\"additionalProperties\":false}",
      AS_T_READ },
    { "list_recordings",
      "The run recorder's own recordings on the tablet's card: name, size, length and channel count, "
      "newest first. These are 50 Hz CSVs the technician recorded, not robot logs -- list_logs is those.",
      NOARGS, AS_T_READ },
    { "analyze_recording",
      "Analyses one recording ON THE TABLET and returns the numbers, not the samples: per channel its "
      "count, minimum and maximum with the times they happened, mean, last value and a sliced trend, plus "
      "where the marks are. Use this rather than asking for raw data -- a five-minute run is hundreds of "
      "thousands of samples.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,"
      "\"maxLength\":48}},\"required\":[\"name\"],\"additionalProperties\":false}",
      AS_T_READ },
    { "get_states",
      "The state timelines the tablet has recorded since it booted: for each mechanism and state machine, "
      "the state it is in now, how long it spent in each state, how many times it changed, the last few "
      "transitions, and for a state machine its rejection, timeout, abort and yield counters. Answers "
      "\"what has it been doing\" and \"why did it leave that state\".",
      "{\"type\":\"object\",\"properties\":{\"window_s\":{\"type\":\"number\",\"minimum\":1,"
      "\"maximum\":86400}},\"additionalProperties\":false}",
      AS_T_READ },
    /* ---- the tablet and the house: changes, but never to the robot ---- */
    { "open_app",
      "Brings one of the tablet's apps on screen, by the name on its icon: e.g. weather, music, photos, clock, "
      "timer, calendar, notes, documents, checklist, flashlight, batteries, blue alliance, recorder, logs, alerts, "
      "tune, systemcore, assist, companion, memory (what the two AIs remember), settings. For settings, `section` "
      "opens one of its sections. Say which one you opened.",
      "{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":32},"
      "\"section\":{\"type\":\"string\",\"enum\":[\"display\",\"look\",\"sound\",\"robot\",\"network\",\"pc link\","
      "\"assistant\",\"home\",\"date and time\",\"storage\",\"power\",\"about\"]}},"
      "\"required\":[\"app\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "set_tablet",
      "Changes the tablet's own settings; give only what was asked for. brightness and volume 0-100; tone dark or "
      "light; accent, the highlight colour; calm true for less motion; flashlight true for a full-white screen, "
      "false to put it away; screen \"sleep\" turns the screen off now (a tap wakes it), \"lock\" shows the lock "
      "screen; home_mode true brings up the desk surface (clock, weather, music, the companion), false goes back "
      "to the robot pages.",
      "{\"type\":\"object\",\"properties\":{"
      "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
      "\"volume\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
      "\"tone\":{\"type\":\"string\",\"enum\":[\"dark\",\"light\"]},"
      "\"accent\":{\"type\":\"string\",\"enum\":[\"ice\",\"orange\",\"leaf\",\"violet\",\"rose\",\"amber\",\"teal\","
      "\"white\"]},"
      "\"calm\":{\"type\":\"boolean\"},\"flashlight\":{\"type\":\"boolean\"},"
      "\"screen\":{\"type\":\"string\",\"enum\":[\"sleep\",\"lock\"]},\"home_mode\":{\"type\":\"boolean\"}},"
      "\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "tablet_status",
      "The tablet itself right now: battery and charging, the Wi-Fi network and signal, the date and time, how "
      "long it has been up, free memory, brightness, volume, tone and accent, whether home mode is on, what is "
      "open, unread notifications, a running timer, the next alarm and pending reminders.",
      NOARGS, AS_T_DEVICE },
    { "timer",
      "The tablet's timer, which keeps running and rings when done whatever is on screen. countdown starts a "
      "countdown of `seconds` (1 s to 24 h); match starts the FRC match clock (autonomous, then teleop; `seconds` "
      "15 or 20 sets autonomous's length); stopwatch starts the stopwatch; pause, resume, reset; status says what "
      "it is doing and the time left. There is one timer: starting one replaces whatever was running (say so). "
      "For a reminder with words, use reminder.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"countdown\",\"match\","
      "\"stopwatch\",\"pause\",\"resume\",\"reset\",\"status\"]},"
      "\"seconds\":{\"type\":\"number\",\"minimum\":1,\"maximum\":86400}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "alarm",
      "The clock app's alarms (at most 5), which ring at a time of day even with the app closed. list shows them "
      "numbered; add sets one at `time` (\"HH:MM\", 24-hour) on `days`: once, daily, weekdays, weekends, or days "
      "like \"mo,we,fr\" (once if not given); on, off and remove act on alarm number `index` from list.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"add\",\"on\","
      "\"off\",\"remove\"]},\"time\":{\"type\":\"string\",\"minLength\":3,\"maxLength\":5},"
      "\"days\":{\"type\":\"string\",\"maxLength\":27},\"index\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "reminder",
      "A reminder with words: when it is due the tablet chimes, wakes the screen and shows the text (it is kept "
      "in the notifications). add needs `text` and either `minutes` from now (0.5 to 1440) or `at` \"HH:MM\" "
      "(today, or tomorrow if that time has passed); list shows the pending ones with their ids; cancel takes an "
      "`id`. At most 8; they survive a restart.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"add\",\"list\",\"cancel\"]},"
      "\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":90},"
      "\"minutes\":{\"type\":\"number\",\"minimum\":0.5,\"maximum\":1440},"
      "\"at\":{\"type\":\"string\",\"minLength\":3,\"maxLength\":5},"
      "\"id\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "notes",
      "The notes app's pit notebook: one running text on the tablet (copied to the card as CATOS/DOCS/NOTES.TXT). "
      "read returns it; append adds `text` as a new line with the time in front; replace overwrites the whole "
      "notebook with `text` (only when asked to rewrite it).",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"read\",\"append\","
      "\"replace\"]},\"text\":{\"type\":\"string\",\"maxLength\":3000}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "documents",
      "The documents on the tablet's card (CATOS/DOCS, .md and .txt), which the documents app shows and renders. "
      "list names them, newest first; read returns one by `name`; write saves `markdown` as `name` (.MD is added; "
      "an existing document of that name is replaced). For anything longer than a note: a procedure, a match "
      "report, a checklist of your own.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"read\",\"write\"]},"
      "\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":40},"
      "\"markdown\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":12000}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "checklist",
      "The pre-match checklist app. status lists its items, numbered, each checked or not; check and uncheck "
      "act on one `item` (its number, or words from its text); reset clears them all for the next match.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"status\",\"check\","
      "\"uncheck\",\"reset\"]},\"item\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":60}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "calendar",
      "The calendar app's events, on the card. list returns the events from `date` for `days` days (today and 1 "
      "day if not given; at most 31); add puts an event on `date` at `time` (\"HH:MM\") with `text`. date is "
      "\"YYYY-MM-DD\", \"today\" or \"tomorrow\".",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"add\"]},"
      "\"date\":{\"type\":\"string\",\"minLength\":5,\"maxLength\":10},"
      "\"days\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":31},"
      "\"time\":{\"type\":\"string\",\"minLength\":3,\"maxLength\":5},"
      "\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":70}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "weather",
      "The weather where the tablet is set to (home mode's place): now, today's high and low, the next hours and "
      "the week, with the chance of rain. Says so when no place is set.",
      NOARGS, AS_T_DEVICE },
    { "notifications",
      "The tablet's notifications: every message the island showed, newest first. read returns the newest "
      "`limit` (10 if not given); clear empties the list.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"read\",\"clear\"]},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":30}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "music",
      "The music player: the tablet's own, over the tracks on its card, or with `where` \"pc\" the PC's media "
      "through Catalyst Link (there play and pause both toggle). play, pause, next, previous, stop, or status: "
      "what is playing now.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":"
      "[\"play\",\"pause\",\"next\",\"previous\",\"stop\",\"status\"]},"
      "\"where\":{\"type\":\"string\",\"enum\":[\"tablet\",\"pc\"]}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "smart_home",
      "The Home Assistant entities chosen in home mode's settings. \"list\" first: it gives each "
      "entity's id, name, kind and state, and nothing outside that list can be touched. \"toggle\" "
      "does the natural thing for the kind; \"brightness\" takes 0-100 for a light (0 turns it off); "
      "\"temperature\" takes a set point for a thermostat.",
      "{\"type\":\"object\",\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"toggle\",\"brightness\",\"temperature\"]},"
      "\"entity\":{\"type\":\"string\",\"maxLength\":80},"
      "\"value\":{\"type\":\"number\"}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "systemcore_health",
      "Systemcore's own view from catalyst-agent (port 9010): OS and uptime, per-core CPU and throttling, "
      "temperatures, memory, storage, top processes, CAN interface error counters, network, and the robot "
      "program's service state, restarts and log tail. Systemcore only.",
      NOARGS, AS_T_READ },
    { "motor_history",
      "The robot's motor history (FrcCatalyst MotorHistory via catalyst-agent): the most worn motors by "
      "running time, heat and faults. Systemcore only.",
      NOARGS, AS_T_READ },

    /* ---- memory: the calling conversation's own pool, never the other's ---- */
    { "remember",
      "Saves one durable fact to YOUR long-term memory, which lasts across conversations and restarts and is "
      "yours alone (the other AI on this tablet can't read it). One fact per call, as a self-contained sentence "
      "that will make sense months from now without this conversation: who, what, when; for a fault the symptom, "
      "the cause and the fix. importance: 3 would cost something if forgotten, 2 useful, 1 minor. To correct a "
      "memory, give its id in `replaces`. A memory that says nearly what one already says updates that one. "
      "Passwords, keys and tokens are refused. Returns the memory's id.",
      "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"minLength\":4,\"maxLength\":280},"
      "\"kind\":{\"type\":\"string\",\"enum\":" KINDS "},"
      "\"importance\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3},"
      "\"replaces\":{\"type\":\"integer\",\"minimum\":1}},"
      "\"required\":[\"text\",\"kind\",\"importance\"],\"additionalProperties\":false}",
      AS_T_MEMORY },
    { "forget",
      "Deletes one memory from YOUR long-term memory by its id (from recall, or the ids shown at the start of "
      "the conversation). Use it when asked to forget something, or for a memory that is wrong and can't be "
      "corrected.",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"minimum\":1}},"
      "\"required\":[\"id\"],\"additionalProperties\":false}",
      AS_T_MEMORY },
    { "recall",
      "Searches YOUR long-term memory: by `query` words (memories sharing them, most relevant first), by "
      "`kind`, or with neither for the most important. Returns each memory's id, kind, importance, date and "
      "text. The start of each conversation already shows the most important; use this for the rest, and to find "
      "one before correcting or forgetting it.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"maxLength\":120},"
      "\"kind\":{\"type\":\"string\",\"enum\":" KINDS "},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":30}},\"additionalProperties\":false}",
      AS_T_MEMORY },
    { "update_notebook",
      "Rewrites one section of your season notebook, the picture of this team you carry into every conversation: "
      "robot (what it is, its quirks and history), team (procedures, preferences, who does what), season (events, "
      "matches and results: the campaign so far), watchlist (recurring faults and open problems you are keeping "
      "an eye on). The text REPLACES the section: merge what it already says with what changed, and keep it dense. "
      "\"\" empties it.",
      "{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\",\"enum\":[\"robot\",\"team\","
      "\"season\",\"watchlist\"]},\"text\":{\"type\":\"string\",\"maxLength\":600}},"
      "\"required\":[\"section\",\"text\"],\"additionalProperties\":false}",
      AS_T_SELF_TECH },
    { "update_persona",
      "Rewrites one field of your persona, the self you carry into every conversation: name (what the owner calls "
      "you, once they give you a name), personality (your traits and how you talk), relationship (how you and the "
      "owner get on, your history together in brief), running_jokes (inside jokes, nicknames, recurring bits). "
      "The text REPLACES the field: merge what it already says with what changed. \"\" empties it.",
      "{\"type\":\"object\",\"properties\":{\"field\":{\"type\":\"string\",\"enum\":[\"name\",\"personality\","
      "\"relationship\",\"running_jokes\"]},\"text\":{\"type\":\"string\",\"maxLength\":400}},"
      "\"required\":[\"field\",\"text\"],\"additionalProperties\":false}",
      AS_T_SELF_COMP },

    /* ---- the code, through Catalyst Link on the PC (read-only) ---- */
    { "code_tree",
      "Lists files in the robot project on the PC, relative to the repo root.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"maxLength\":200},"
      "\"depth\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4}},\"additionalProperties\":false}",
      AS_T_READ },
    { "code_read",
      "Reads lines of a file in the robot project (at most 400 lines per call).",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200},"
      "\"start\":{\"type\":\"integer\",\"minimum\":1},\"end\":{\"type\":\"integer\",\"minimum\":1}},"
      "\"required\":[\"path\"],\"additionalProperties\":false}",
      AS_T_READ },
    { "code_search",
      "Searches the robot project for a plain substring (case-insensitive).",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":120},"
      "\"path\":{\"type\":\"string\",\"maxLength\":200}},\"required\":[\"query\"],\"additionalProperties\":false}",
      AS_T_READ },
    { "list_patches", "Patch branches proposed from the tablet and their status (proposed, merged, dropped).",
      NOARGS, AS_T_READ },
    { "list_work_orders",
      "Work orders in the PC's inbox.",
      "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\","
      "\"enum\":[\"open\",\"claimed\",\"done\",\"rejected\",\"all\"]}},\"additionalProperties\":false}",
      AS_T_READ },
    { "claude_sessions",
      "What Claude Code is doing on the owner's PC, from Catalyst Link's hooks: each session's folder, state "
      "(running, waiting_for_input, done, error), current step, elapsed time and an estimate of when it will "
      "finish with how that estimate was made. Estimates come from the owner's past turns; say they are estimates.",
      NOARGS, AS_T_READ },

    /* ---- changes: each one waits for the technician ---- */
    { "set_tunable",
      "Changes one declared tunable. The technician must approve it on screen; every tunable is snapshotted "
      "first so it can be reverted. Returns the value the robot echoes back.",
      "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":96,"
      "\"description\":\"the tunable's full NT key from list_tunables\"},"
      "\"value\":{\"type\":[\"number\",\"boolean\"]},"
      "\"reason\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":240}},"
      "\"required\":[\"key\",\"value\",\"reason\"],\"additionalProperties\":false}",
      AS_T_CHANGE },
    { "select_auto",
      "Selects an autonomous routine in the chooser (one of list_autos' options). Needs the technician's approval.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":40},"
      "\"reason\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":240}},"
      "\"required\":[\"name\",\"reason\"],\"additionalProperties\":false}",
      AS_T_CHANGE },
    { "revert_snapshot",
      "Writes a snapshot's tunable values back to the robot. Needs the technician's approval; the current "
      "values are snapshotted first.",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"minimum\":1},"
      "\"reason\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":240}},"
      "\"required\":[\"id\",\"reason\"],\"additionalProperties\":false}",
      AS_T_CHANGE },
    { "propose_patch",
      "Proposes a change to the robot's code. After the technician approves, Catalyst Link commits it on a "
      "new branch in its own worktree on the PC: never the checked-out branch, never pushed, never deployed. "
      "Each edit replaces `old`, which must occur exactly once in the file, with `new`; old \"\" with a new "
      "path creates a file. Returns the branch, diffstat and compile check.",
      "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":80},"
      "\"summary\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":2000},"
      "\"edits\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":20,\"items\":{\"type\":\"object\","
      "\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200},"
      "\"old\":{\"type\":\"string\",\"maxLength\":20000},\"new\":{\"type\":\"string\",\"maxLength\":20000}},"
      "\"required\":[\"path\",\"old\",\"new\"],\"additionalProperties\":false}}},"
      "\"required\":[\"title\",\"summary\",\"edits\"],\"additionalProperties\":false}",
      AS_T_CHANGE },
    { "create_work_order",
      "Drops a work order in Catalyst Link's inbox for the PC's own agent to work on later. Attach a proposed "
      "patch's id and files from the tablet's microSD (recordings, clips, logs; paths relative to the card). "
      "Queued on the tablet if the PC is unreachable.",
      "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":96},"
      "\"body\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":8000},"
      "\"kind\":{\"type\":\"string\",\"enum\":[\"bug\",\"task\",\"tune\",\"question\"]},"
      "\"priority\":{\"type\":\"string\",\"enum\":[\"low\",\"normal\",\"high\"]},"
      "\"patch\":{\"type\":\"string\",\"maxLength\":80},"
      "\"sd_files\":{\"type\":\"array\",\"maxItems\":8,\"items\":{\"type\":\"string\",\"minLength\":1,"
      "\"maxLength\":160}}},"
      "\"required\":[\"title\",\"body\",\"kind\",\"priority\"],\"additionalProperties\":false}",
      AS_T_SEND },
};

const int AS_NTOOLS = (int)(sizeof AS_TOOLS / sizeof AS_TOOLS[0]);

const as_tooldef_t *as_tool_find(const char *name)
{
    for (int i = 0; name && i < AS_NTOOLS; i++) if (!strcmp(AS_TOOLS[i].name, name)) return &AS_TOOLS[i];
    return NULL;
}

bool as_tool_for(const as_tooldef_t *t, int pool)
{
    if (!t) return false;
    bool tech = pool == AS_MEM_ASSIST;
    switch (t->kind) {
    case AS_T_CHANGE:
    case AS_T_SEND:      /* they need a card on the Assist screen */
    case AS_T_SELF_TECH: return tech;
    case AS_T_SELF_COMP: return !tech;
    default: return true;
    }
}

void as_tools_json(ab_t *out)
{
    ab_puts(out, "[");
    bool first = true;
    for (int i = 0; i < AS_NTOOLS; i++) {
        if (!as_tool_for(&AS_TOOLS[i], AS_MEM_ASSIST)) continue;
        if (!first) ab_puts(out, ",");
        first = false;
        ab_puts(out, "{\"name\":");
        ab_str(out, AS_TOOLS[i].name);
        ab_puts(out, ",\"description\":");
        ab_str(out, AS_TOOLS[i].description);
        ab_puts(out, ",\"input_schema\":");
        ab_puts(out, AS_TOOLS[i].schema);
        /* inputs stream as generated; the tablet validates them itself (as_tool_check) */
        ab_puts(out, ",\"eager_input_streaming\":true}");
    }
    ab_puts(out, "]");
}

bool as_tool_check(const as_tooldef_t *t, const aj_t *input, char *err, size_t n)
{
    char perr[64];
    aj_t *schema = aj_parse(t->schema, strlen(t->schema), perr, sizeof perr);
    if (!schema) {
        snprintf(err, n, "the tablet's schema for %s is broken (%s)", t->name, perr);
        return false;
    }
    bool ok = aj_validate(schema, input, err, n);
    aj_free(schema);
    return ok;
}
