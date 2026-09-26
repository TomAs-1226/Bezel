/* The tool definitions: one schema per tool, sent to the API and used to validate what comes back. */
#include "as_tools.h"

#include <stdio.h>
#include <string.h>

#define NOARGS "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"

const as_tooldef_t AS_TOOLS[] = {
    /* ---- the robot, read ---- */
    { "robot_overview",
      "The whole robot at a glance: identity and versions, mode, battery and brownout, loop time, CAN load, "
      "device roster, alert counts, Systemcore, the NetworkTables link and the Catalyst Link to the PC. "
      "Start here.",
      NOARGS, AS_T_READ },
    { "get_alerts",
      "Every active alert (AlertManager, WPILib alert groups, firing HealthMonitor checks), worst first, "
      "plus the robot's own preflight findings.",
      NOARGS, AS_T_READ },
    { "get_mechanisms",
      "Each mechanism Catalyst publishes (/Catalyst/<name>/…): state, position against goal, velocity, "
      "current, temperature, at-goal; and the swerve modules, actual against commanded.",
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
      "Brings one of the tablet's apps on screen, by the name on its icon: weather, music, photos, "
      "clock, timer, notes, documents, lens, field, power, alerts, tune, logs, settings and the rest. "
      "Say which one you opened.",
      "{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\",\"minLength\":1,"
      "\"maxLength\":32}},\"required\":[\"app\"],\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "set_tablet",
      "Sets the screen brightness and the speaker volume, each 0 to 100. Give only the one asked for.",
      "{\"type\":\"object\",\"properties\":{"
      "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
      "\"volume\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
      "\"additionalProperties\":false}",
      AS_T_DEVICE },
    { "music",
      "The tablet's music player, over the tracks on its card: play, pause, next, previous, stop, or "
      "what is playing now.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":"
      "[\"play\",\"pause\",\"next\",\"previous\",\"stop\",\"status\"]}},"
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

void as_tools_json(ab_t *out)
{
    ab_puts(out, "[");
    for (int i = 0; i < AS_NTOOLS; i++) {
        if (i) ab_puts(out, ",");
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
