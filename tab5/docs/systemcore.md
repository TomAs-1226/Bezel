# Systemcore and Catalyst depth: the contract

What the **systemcore**, **motors**, **states**, **controls** and **recorder** screens read, from where,
and the thresholds they judge it by. Distilled from FrcCatalyst `upgrade/alpha-7` (2.0.0-beta.2:
`agent/`, `system/SystemCoreStatus.java`, `hardware/CANBusHealth.java`, `identity/MotorHistory.java`,
`statemachine/robot/CatalystStateMachineLog.java`, `autonomy/AutonomyBoard.java`), Catalyst Console
(`src/app.js`'s Systemcore page, `src/core-format.js`, `src/can-model.js`, `docs/can-buses.md`,
`src/drivers.js`) and the Catalyst App's Motor History tool. The C side is `components/catalyst/src/cat_sc.c`
(parsers, pure) and `cat_sc_io.c` (workers); the screens are `components/ui/src/ui_apps_sc.c`.

The rule from [catalyst-contract.md](catalyst-contract.md) holds: **nothing here commands the robot.**
catalyst-agent has no endpoint that changes anything (its tests assert `do_GET` is the only handler),
the controls manifest is read and never written, and the recorder only reads NetworkTables.

## catalyst-agent (on the Systemcore, HTTP, port 9010)

An optional package from FrcCatalyst's `agent/` (`build.sh` → `catalyst-agent_2.0.x.ipk`, installed
through the Systemcore web UI's package manager or `opkg install`). Standard-library Python, `Nice=10`,
sampled on request. The tablet asks it the way Console does (`app.js` `pollAgent`):

- **Where**: `http://<the address NetworkTables connected to>:9010` (a `host:port` loses its port).
- **When**: every 3 s, and only while the systemcore screen is open (motors: `/api/motor-history` every
  30 s while that screen is open). On a worker thread, never the UI's; 2.5 s timeout.
- **Absent**: three silent polls in a row mean "not installed" (a robot that just rebooted refuses
  connections for a moment). It keeps asking; the screen falls back to the NetworkTables summary and one
  line on how to install it. Not an error: most robots won't have it.
- The library's `docs/utilities/motor-history.md` says `:4800`; the agent's code (`PORT = 9010`), Console
  and the Catalyst App all use **9010**.

| Route | Body |
|---|---|
| `/api/health` | `{ok, agent: "catalyst-agent", version}` |
| `/api/system` | the snapshot below |
| `/api/robot` | `robotProgram` alone |
| `/api/cameras` | `cameras` alone |
| `/api/motor-history` | `catalyst/motor-history.json` as it is on disk, plus `path`, `fileModifiedAt`; 404 with `{error, path, devices: []}` before the robot program has written it |
| `/api/motor-history.csv` | one row per device, the columns of `MotorHistory/Rows` |

`/api/system` (`snapshot()`), every value possibly `null` — **null is absent, never 0**:

```
identity     {hostname, os, osVersion, kernel, model, uptimeSeconds, agentVersion}
cpu          {cores: [{core, percent, mhz}], loadAverage: [1, 5, 15 min] | null, model,
              throttling: {underVoltageNow, frequencyCappedNow, throttledNow, softTempLimitNow,
                           throttledSinceBoot, underVoltageSinceBoot} | null}
thermal      [{zone, celsius}]
memory       {totalBytes, availableBytes, usedBytes, cachedBytes, swapTotalBytes, swapFreeBytes}
storage      {mounts: [{mount, device, filesystem, totalBytes, usedBytes, freeBytes}],
              directories: [{path, bytes | null}]}          /home/systemcore, /var/log, /U, /V
processes    {count, topByCpu: [{pid, name, cpuPercent, rssBytes}] (8), topByMemory: [...] (8)}
can          [{name, up, state, bitrate, restarts, rxPackets, txPackets, rxErrors, txErrors,
               rxDropped, txDropped}]                       frame counters per interface
network      [{name, up, mac, speedMbps (-1 unknown), addresses: ["a.b.c.d/n"],
               wireless: {linkQuality, signalDbm} | null}]  every interface but lo and can*
robotProgram {unit: "robot.service", state, subState, restarts, runningForSeconds, memoryBytes, pid,
              log: [60 journal lines, short-iso, newest last]}
cameras      {available, reason?, sampledAt, cameras: [{name, host, ip, type, interface, ntConnected,
               ntName, aliasIps, uiUrl, streamUrl, fps, temperatureC, cpuPercent, ramPercent,
               pipelineType, pipelineIndex, statusReachable}]}
motorHistory {present, updatedMs, clockTrusted, devices: [the CSV row, as objects]}
sampledAt    epoch seconds
```

A per-core `percent` is per core; a process's `cpuPercent` is per core too, so one process can read
past 100 % on the four-core CM5. `throttling` is `null` without `vcgencmd` — absent is not "all clear".

## NetworkTables

### `/Catalyst/Systemcore/*` — `SystemCoreStatus.publish()`, mirrored from the OS's system server

Every loop: `BatteryVolts`, `BrownedOut`, `CpuPercent`, `TempCelsius`, `RamFraction`, `Rail3v3Amps`,
`CanUtilization` (double[5], fractions, `can_s0`…`can_s4`), `CanDown`. About once a second:
`BrownoutVolts`, `RecoveryVolts`, `StorageFraction`, `RamUsedBytes`, `RamTotalBytes`, `StorageUsedBytes`,
`StorageTotalBytes`, `EmmcLifeUsed` (0–1, the midpoint of JEDEC's 10 % band; worse of SLC/MLC),
`EmmcPreEol` (1 normal, 2 warning, 3 urgent), `CanDownCount`, `CanUnavailCount` (since boot),
`TeamNumber` (the Systemcore's own), `NetworkInterfaces` (string[], the OS's wording, passed through),
`HardwareSubRev`. The OS publishes utilisation as ten numbers, `[percent, fraction]` per bus; the library
decodes it to five fractions before mirroring.

### `/Catalyst/CAN/Health/<bus>/*` — `CANBusHealth.publish()` (Phoenix's view)

`OK`, `Utilization`, `BusOffCount`, `TxFullCount`, `REC`, `TEC`, for every bus the CAN registry knows
(it reaches CANivores too). Systemcore has **five buses on three SPI hosts**: `can_s0`+`can_s1`,
`can_s2` alone, `can_s3`+`can_s4` (Console `docs/can-buses.md`, `CatalystCANBus.controllerGroup()`).
For a Systemcore bus the OS's `CanUtilization` wins (it covers idle buses); Phoenix's fills in the rest.

### `/Catalyst/MotorHistory/*` — `MotorHistory.publish()`

`Rows` (string[]): `serial|model|kind|bus|id|name|firmware|poweredS|runningS|loadedS|revolutions|peakA|
peakC|hotS|energyJ|boots|firstSeenMs|lastSeenMs|identities|stickyFaults` — the name is a team's string
and may contain `|`: every separator past the format's nineteen belongs to it. Also `Count`, `Devices`,
`File`, `UpdatedMs`, `ClockTrusted` (false while the Systemcore's clock reads 1970), `Discovery`,
`Summary`. The rows are totals only; identities and recent boots are in the agent's file.

The file (`/api/motor-history`): `{format, version, updatedMs, clockTrusted, devices: [{serial, model,
kind (motor|encoder|imu|device), hardwareRev, manufactured, firstSeenMs, lastSeenMs, boots, totals:
{poweredSeconds, runningSeconds, loadedSeconds, revolutions, energyJoules, peakStatorAmps, peakTempC,
hotSeconds, stickyFaults}, identities: [{id, name, bus, firmware, firstSeenMs, lastSeenMs}], sessions
(≤ 40): [{startMs, seconds, runningSeconds, revolutions, peakAmps, peakTempC, hotSeconds}]}]}`. Turning
is `|velocity| ≥ 0.5 rps`, loaded is stator `≥ 5 A`, hot is `≥ hotCelsius` (70 °C by default).

### States

- **Mechanisms**: `CatalystMechanism.setState()` → `/Catalyst/<mechanism>/State` (string).
- **State machines** (`CatalystStateMachineLog`, prefix defaults to the machine's name, e.g.
  `Superstructure`): `State`, `StateOrdinal`, `StateConfirmed`, `Target`, `NextHop`, `Phase`,
  `Transitioning`, `Counters/{Transitions,Rejections,Timeouts,Aborts,Yields}`,
  `Transition/{Seq,From,To,Route,Trigger,Outcome,Reason,Detail,DurationSeconds,Arrivals}`, `Transition/History`
  (newest first, `timestamp|seq|from|to|route|trigger|outcome|reason|duration|detail`), `Rejected/Last`,
  `Faulted`, `FaultReason`, `Graph/*`. A `Phase` beside a `State` is how the tablet tells a machine from
  a mechanism.
- **Autonomy** (`AutonomyBoard`, published only on change): `Autonomy/Tasks/{Running,Held,Explain}`,
  `Chase/{Target,Why,Rejected}`, `Authority/{Scale,Binding,Explain}`, `Power/{Deficit,Shed,Short,
  Explain}`, `Intent/{Guess,HitRate,Samples,Explain}`, `Situation/*`.
- **Mode**: `/FMSInfo/ControlWord` (2027), else `FMSControlData`.

### Controls — `/Catalyst/Controls/.manifest`

A JSON array `[{control, action, controller?, combo?}]`, only `control` and `action` required; a robot
that names no controller has one, the driver's (Console `drivers.js` `readControlBindings`). Published by
the robot program (Catalyst X1 does; the library has no publisher of its own), read and never written —
which button does what is the robot's wiring, and a dashboard that could rebind one would be a
dashboard that drives.

## Thresholds (Console's, so the two never disagree)

| Reading | ok / warn / fault | Source |
|---|---|---|
| CPU (overall and per core), RAM | < 85 / ≥ 85 / ≥ 95 % | `core-format.js` `level()` |
| Controller temperature | < 80 / ≥ 80 / ≥ 90 °C | `paintCore` (below where the CM5 throttles) |
| Storage | < 85 / ≥ 85 / ≥ 93 % | `paintCore` (a full disk stops logging first) |
| eMMC wear | < 70 / ≥ 70 / ≥ 90 % of rated life; pre-EOL 2 warn, 3 fault | `paintCoreWear` |
| CAN bus bar (Systemcore page) | < 70 / ≥ 70 / ≥ 85 % | `paintCoreCan` |
| A shared SPI pair | < 80 / ≥ 80 / ≥ 100 % of one controller | `docs/can-buses.md` |
| REC / TEC | error-passive at 128 (fault); the tablet warns from 96 | `CANBusHealth.hasErrorActivity` |
| Camera temperature | hot at ≥ 80 °C | `paintAgentCameras` |
| Robot program | not `active` fault; any restarts warn | `paintAgentProgram` |
| Motor | peak ≥ 70 °C or any hot seconds: warn; more than one identity: renumbered | Console / App |

## What the screens do with it

- **systemcore** — processor (per core with clock, load, throttling now vs since boot; the NT figure and
  its last two minutes without the agent), temperature by zone, memory used of total, the robot
  program (state, restarts, running for, memory, its last lines; the full tail in its own tile), CAN by
  bus grouped by SPI host with the pair's total, REC/TEC/bus-off from `CAN/Health`, frames and errors
  from the agent, downs since boot, top processes, storage by mount and by directory, eMMC, network
  interfaces (`usb0` is the tether) and how this tablet reaches the robot, Limelights, machine identity.
- **motors** — every device by serial, motors first, sorted by loaded, turning or hot time, powered
  hours or peak temperature; flags for hot time and past identities; a detail pane with the totals, the
  last identities and a bar per recent boot (warn when it had hot time).
- **states** — lanes recorded by the tablet from boot (`cat_sc_init`, 10 Hz on a worker, reading only
  changed values): the robot's mode, every state machine, every mechanism, Autonomy's decisions. Each
  lane shows the current state, time in it and the changes over 1, 5 or 15 min; a lane's detail has the
  time in each state, the machine's own counters and the transition list with wall-clock times.
- **controls** — each controller named in the manifest, its bindings numbered on a pad diagram and
  listed; without a manifest, the shape to publish.
- **recorder** — presets (drivetrain, power, each discovered mechanism, or any announced numeric
  topics), 50 Hz on a worker thread reading nt4 directly into `<sd>/runs/run-YYYYMMDD-HHMMSS.csv`, a
  glass record button and a mark button, live sparklines, the runs on the card, and "send to PC" through
  Catalyst Link (`link_upload`).

### The recorder's CSV

```
t,mark,Swerve/ModuleStates.fl_speed,…,Systemcore/BatteryVolts
0.000,,1.42,0.51,…,12.585
0.020,1,1.44,0.51,…,12.592
```

`t` is seconds since the start (three decimals); `mark` is the mark's number on the row after the tap,
empty otherwise; a value the robot isn't publishing is an empty field, never 0. Columns are fixed at
the start: an array topic (a Pose2d, module states) contributes one column per element it had then.
The tablet subscribes at 20 Hz (`periodic` 0.05 s), so a 50 Hz row may repeat the last value it had;
the rows are on the tablet's clock, evenly spaced.

## Gaps worth closing in the library

- `/Catalyst/Controls/.manifest` has no publisher in Catalyst itself; a `ControlsManifest` helper that
  robot code fills as it binds buttons would give every robot one.
- `CANBusHealth.publish()` isn't called by the X1 capture Console holds (no `/Catalyst/CAN/Health/*`
  there): without it the tablet has utilisation but no REC/TEC.
- State machines keep `Transition/History` with the robot's own timestamps; mechanisms don't, so a
  lane's times are the tablet's (it saw the change within 100 ms of the value arriving).
