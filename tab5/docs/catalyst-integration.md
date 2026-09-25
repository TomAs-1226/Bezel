# Catalyst Tab ↔ FrcCatalyst — the integration contract

Audience: whoever changes the FrcCatalyst library next. This page is the exhaustive cross-reference
between every NetworkTables topic and HTTP call the tablet makes and what the library actually
publishes, with `file:line` on both sides. `docs/catalyst-contract.md`, `docs/systemcore.md` and
`docs/link-api.md` describe the contract at the level a technician needs; this page is for changing
the library side of it and does not repeat their prose.

**See also `docs/api-connections.md`** for the tablet's complete API surface, including what this page
does not cover: NT4 topics the tablet *writes* (not just reads), the Catalyst Link HTTP API, the cloud
APIs called directly by the firmware (The Blue Alliance, OpenAI, Anthropic, Open-Meteo, Home Assistant),
and the tablet's local interfaces (SD card layout, dev console). This page remains the authoritative
library cross-reference for NT4/HTTP-vs-FrcCatalyst specifically.

**Checkouts read for this page:**

- Tablet: `tab5/` at `C:\Users\yu_th\dev\_worktrees\Bezel-tab5\tab5`, branch `tab5-pr1`.
- Library, 2.x beta (Systemcore/alpha-7 line): `C:\Users\yu_th\dev\_worktrees\FrcCatalyst-alpha7`,
  branch `upgrade/alpha-7`, version string `2.0.0-beta.2`.
- Library, 2.x alpha-6 line: `C:\Users\yu_th\dev\_worktrees\FrcCatalyst-alpha6`, branch
  `systemcore-alpha6`.
- Library, 1.x (roboRIO): `C:\Users\yu_th\dev\FrcCatalyst-v1.1.0`.

Every library claim below cites a file and line in the alpha-7 checkout unless marked otherwise. Where
alpha-6 or 1.x differ, that is called out; where they were not checked, it says **unverified**.

**Classification key**

- **PUBLISHED** — the library publishes exactly what the tablet reads (path, type, and shape agree).
- **MISMATCH** — the library publishes something under a different name, type, or shape than the
  tablet expects, or than the tablet's own doc (`catalyst-contract.md`/`systemcore.md`) claims.
- **MISSING** — the tablet subscribes to (or would use) something the library never publishes at all.
- **APP-LEVEL** — published only by example/robot code, not by any Catalyst library class. The tablet
  reads it, but no `import frc.lib.catalyst...` class is responsible for it existing.

---

## 1. Identity / Software (screens: pulse, robot)

Tablet reader: `components/catalyst/src/cat_model.c` `read_identity()`, lines 113–121.

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Robot/Identity/Name` | string | cat_model.c:113 | `RobotIdentity` (`declare`/`named`), `docs/advanced/robot-identity.md:193` | PUBLISHED |
| `/Catalyst/Robot/Identity/TeamNumber` | double | cat_model.c:114 | `robot-identity.md:194`, from `RobotController.getTeamNumber()` | PUBLISHED |
| `/Catalyst/Robot/Identity/Controller` | string | cat_model.c:121 | `robot-identity.md:196` (`Systemcore`/`Simulation`) | PUBLISHED |
| `/Catalyst/Robot/Software/CatalystVersion` | string | cat_model.c:115 | `robot-identity.md:204`, `CatalystVersion` | PUBLISHED |
| `/Catalyst/Robot/Software/RobotCodeVersion` | string | cat_model.c:116 | `robot-identity.md:208` — **declared by the team**; absent unless `RobotIdentity.named(...).robotCodeVersion(...)` is called | PUBLISHED, opt-in |
| `/Catalyst/Robot/Software/WPILibVersion` | string | cat_model.c:117 | `robot-identity.md:210` | PUBLISHED |
| `/Catalyst/Robot/Software/CatalystGitSha` | string | cat_model.c:118-120 | `robot-identity.md:205` | PUBLISHED |

No mismatches here. `RobotCodeVersion`/`RobotCodeBuild` are opt-in by design (only the team's own build
knows its commit) — the tablet's robot screen already treats these as "may be blank," matching
`RobotIdentity.java:50-64`.

## 2. Mode / FMS (screens: pulse, robot, preflight)

Tablet reader: `cat_model.c` `read_mode()`, lines 129–150.

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/FMSInfo/ControlWord` | double (bitfield) | cat_model.c:129 | WPILib 2027's own `FMSInfo`, not Catalyst — Catalyst never touches this table | PUBLISHED (not library's) |
| `/FMSInfo/FMSControlData` | double (bitfield) | cat_model.c:132 | WPILib 2026's own `FMSInfo` | PUBLISHED (not library's) |
| `/FMSInfo/IsRedAlliance`, `StationNumber`, `OpMode` | bool/double/string | cat_model.c:145-148 | WPILib's own | PUBLISHED (not library's) |
| `/Catalyst/Match/TimeLeft` | double | cat_model.c:150 | **No publisher found anywhere in alpha-7, alpha-6, or v1.1.0.** | MISSING |
| `/SmartDashboard/MatchTime` | double | cat_model.c:150 | WPILib convention, not Catalyst | PUBLISHED (not library's) |

`/Catalyst/Match/TimeLeft` is dead code on the tablet: it is tried first of three match-clock sources
and nothing ever publishes it. Low priority (P2) — `/FMSInfo/MatchTime` and `/SmartDashboard/MatchTime`
already cover every real robot — but worth either wiring up or deleting so it stops looking like a
supported path.

## 3. Battery / brownout / power (screens: pulse, power, robot)

Tablet reader: `cat_model.c` `read_power()`, lines 162–210.

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Status/BatteryVolts` | double | cat_model.c:162 (1st choice) | **Not a library topic.** Only appears in `example/src/main/java/frc/robot/RobotContainer.java:820`: `CatalystLog.log("Status/BatteryVolts", RobotController.getBatteryVoltage())` — the example app's own convention, no Catalyst class publishes it | **APP-LEVEL** |
| `/Catalyst/Brownout/MeasuredVoltage` | double | cat_model.c:162 (2nd choice) | `BrownoutMonitor.update()`, `BrownoutMonitor.java:119` — **opt-in**: only published if the robot constructs a `BrownoutMonitor` and calls `update()` every loop (`BrownoutMonitor.java:41-57`) | PUBLISHED, opt-in |
| `/Catalyst/Systemcore/BatteryVolts` | double | cat_model.c:163 (3rd choice) | `SystemCoreStatus.publish()`, `systemcore.md:71` — Systemcore only, needs `HealthMonitor.systemCoreChecks()` (`docs/index.md:254`) | PUBLISHED, Systemcore-only |
| `/Catalyst/Brownout/{TotalCurrent,PredictedVoltage,AtRisk}` | double/double/bool | cat_model.c:174-176 | `BrownoutMonitor.java:120-123` | PUBLISHED, opt-in |
| `/Catalyst/Systemcore/BrownedOut` | boolean | cat_model.c:177 | `systemcore.md:71-76`, `SystemCoreStatus` | PUBLISHED, Systemcore-only |
| `/Catalyst/Robot/Power/BrownoutVolts` | double | cat_model.c:173 | `robot-identity.md:264`, `RobotController.getBrownoutVoltage()` | PUBLISHED |
| `/Catalyst/Robot/Power/{Module,ChannelsInUse}` | string/string[] | cat_model.c:179,181-193 | `robot-identity.md:265-268` — `ChannelsInUse` is declared, not measured | PUBLISHED, opt-in |
| `/SmartDashboard/<PDH>/{TotalCurrent,Voltage,Chan0..23}` | double | cat_model.c:197-210 | Not Catalyst — this is REV/CTRE's own `PowerDistribution` `Sendable`, published only if the team calls `SmartDashboard.putData("PDH", pdh)` | **APP-LEVEL** |

### This is the most important finding in this document

**There is no zero-config, always-on NetworkTables battery voltage in FrcCatalyst 2.x.** The tablet's
own contract doc (`catalyst-contract.md:44`) lists three fallbacks as if they were interchangeable
library outputs; only one of the three (`Systemcore/BatteryVolts`) is actually a library topic, and it
only exists on a Systemcore that has called `HealthMonitor.systemCoreChecks()`. A roboRIO-era 1.x
robot, or a 2.x robot that skips `BrownoutMonitor` and `HealthMonitor.systemCoreChecks()`, publishes
**no battery voltage at all** — the single most important number on the pulse screen shows "not
published" (`ui_pages.c:64`) for the whole match. This is classified MISSING (see §8, item 1).

## 4. Loop time (screens: pulse, robot)

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Loop/Robot/{AverageMs,LastMs,MaxMs,OverBudget}` | double×3, bool | cat_model.c:219-222 | `LoopMonitor`, `docs/advanced/logging.md:202`, `LoopMonitor.java:46` | PUBLISHED, opt-in (team must instantiate `LoopMonitor` in `robotPeriodic()`) |

No mismatch. Opt-in by design — this is a measurement tool, not something Catalyst can derive on its
own.

## 5. CAN / device roster (screens: pulse, devices, systemcore, preflight)

Tablet reader: `cat_model.c` `read_can()`, lines 251-293; CAN Health detail in
`components/ui/src/ui_apps_sc.c` `sc_can()`, lines 771-826.

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Systemcore/CanUtilization` | double[5] | cat_model.c:251 | `systemcore.md:71-78`, `SystemCoreStatus` (decoded from the OS's ten numbers to five fractions) | PUBLISHED, Systemcore-only |
| `/Catalyst/Status/CanUtilization` (single double, scaled fallback) | double | cat_model.c:258-260 | **No publisher found.** Neither `CANBusHealth`, `SystemCoreStatus`, nor anything else in alpha-7/alpha-6/v1.1.0 writes this key. | MISSING |
| `/Catalyst/Systemcore/CanDown` | boolean | cat_model.c:263 | `systemcore.md:71`, `SystemCoreStatus` | PUBLISHED, Systemcore-only |
| `/Catalyst/CAN/Devices` | string[] `"bus\|id\|type\|name"` | cat_model.c:268 | `CANRegistry.publish()`, `CANRegistry.java:309` (`CatalystLog.log("CAN/Devices", out)`) — auto-populated, every `CatalystMotor` self-registers (`CANRegistry.java:37-38`) | PUBLISHED |
| `/Catalyst/Devices/Motors/{Expected,Connected,Rows}` | double,double,string[] | cat_model.c:278-284 | `DeviceRoster`, `DeviceRoster.java:41-47` — 2.x only, driven by `HealthMonitor.update()` at 4 Hz (`CHANGELOG.md:527-529`) | PUBLISHED, 2.x only |
| `/Catalyst/Devices/Cameras/{Expected,Connected}` | double | cat_model.c:282-283 | `DeviceRoster.java:41-42` | PUBLISHED, 2.x only |
| `/Catalyst/Devices/Controller/{Kind,Connected}` | string, bool | cat_model.c:292-293 | `DeviceRoster.java:45-46`, `DeviceRoster.registerController()` at `DeviceRoster.java:116-118` | PUBLISHED, 2.x only |
| `/Catalyst/CAN/Health/<bus>/{Utilization,REC,TEC,BusOffCount}` for `bus ∈ {can_s0..can_s4}` | double | `ui_apps_sc.c:771,781` | `CANBusHealth`, `CANBusHealth.java:36`; bus names `can_s0`..`can_s4` confirmed against `CatalystCANBus`/`docs/advanced/systemcore.md:227-231` — the tablet's bus name literals match the library's exactly | PUBLISHED |
| `/Catalyst/Systemcore/{CanDownCount,CanUnavailCount}` | double | `ui_apps_sc.c:817` | `systemcore.md:73-76` | PUBLISHED, Systemcore-only |

**1.x note:** `/Catalyst/CAN/Devices` exists on 1.x too (`v1.1.0` has `CANRegistry`), but
`DeviceRoster`/`/Catalyst/Devices/*` and `CANBusHealth`'s five-bus model are 2.x-only (v1.1.0 has no
`DeviceRoster.java`, confirmed by search). The tablet's `ui_pages2.c:100-102` empty-state text already
says `/Catalyst/CAN/Devices` is "from 1.12" — **unverified**: this page's checkouts don't include
1.12.0 specifically to confirm that exact version number, only that v1.1.0 (older) lacks it and alpha-7
(current) has it.

`/Catalyst/Status/CanUtilization` (MISSING, see §8 item 2) is dead code: a fallback for a topic nobody
publishes.

## 6. Alerts / Health (screens: pulse, alerts, preflight)

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Alerts/{Errors,Warnings,Info}` | string[] | cat_model.c:319-330 | `AlertManager`, `docs/utilities/index.md:61-63` | PUBLISHED |
| `/SmartDashboard/<group>/{errors,warnings,infos}` | string[] | cat_model.c:337-345 | WPILib's own `Alert`/`AlertGroup`, not Catalyst | PUBLISHED (not library's) |
| `/Catalyst/Health/<subsystem>/<id>/{firing,severity,description,detail}` | bool/string×3 | cat_model.c:347-369 | `HealthMonitor`, `HealthMonitor.java:24` | PUBLISHED |

No mismatches. Note the tablet does **not** read `/Catalyst/Health/{ErrorCount,WarnCount,InfoCount,Healthy}`
or `/Catalyst/Health/History` even though `HealthMonitor.java:24` and `docs/advanced/health.md:72-74,177`
publish them — see §7 Proposed new integration.

## 7. Mechanisms (screen: motion)

Tablet reader: `cat_model.c` mechanism discovery, lines 97-101 (`RESERVED[]`) and 373-431.

| Topic pattern | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/<name>/State` | string | cat_model.c:407 | `CatalystMechanism.setState()`, `systemcore.md:104` | PUBLISHED |
| `/Catalyst/<name>/AngleDegrees`, `SetpointDegrees` | double | cat_model.c:410-411 | `RotationalMechanism`'s auto-telemetry, `docs/mechanisms/index.md:26` | PUBLISHED |
| `/Catalyst/<name>/PositionMeters`, `SetpointMeters`, `VelocityMPS` | double | cat_model.c:416-418 | `LinearMechanism` auto-telemetry | PUBLISHED |
| `/Catalyst/<name>/VelocityRPS`, `SetpointRPS` | double | cat_model.c:422-423 | `FlywheelMechanism` auto-telemetry | PUBLISHED |
| `/Catalyst/<name>/{StatorCurrentAmps,CurrentAmps,TemperatureC}` | double | cat_model.c:428-429 | every built-in mechanism, per `docs/mechanisms/index.md:26-28` | PUBLISHED |
| `/Catalyst/<name>/{AtSetpoint,AtSpeed}` | boolean | cat_model.c:431 | built-in mechanisms | PUBLISHED |

**Internal note, not a library issue:** the tablet's mechanism-*discovery* keys
(`AngleDegrees, PositionMeters, VelocityRPS, VelocityMPS` — `cat_model.c:97-101`) and the recorder's
mechanism *column* list (`MECH_KEYS` in `ui_apps_sc.c` ~line 2200) disagree: `VelocityMPS` triggers
discovery but is missing from `MECH_KEYS`, so a `LinearMechanism`'s velocity channel is silently absent
from a recorded run. This is a tablet bug, not a library gap — flagged here only because it was found
while cross-referencing telemetry key names against the library's `LinearMechanism` doc.

**MISMATCH — reserved name collision risk:** the tablet's mechanism-discovery skips any top-level
`/Catalyst/<name>` whose name is in `RESERVED[]` (`Robot, Alerts, Health, Physics, Swerve, Vision, Loop,
Safety, Calibration, SystemCheck, Preflight, Auto, Systemcore, CAN, Devices, MotorHistory, Tunables,
Tuning, Controls, Brownout, Status, Match, Drive, Superstructure` — `cat_model.c:97-101`). `Drive` and
`Superstructure` are exactly the names teams are most likely to give a drivetrain or top-level state
machine (the library's own examples and `docs/index.md:353` use `Superstructure` as the canonical state
machine name). A team's `Superstructure` mechanism/state-machine matching that reserved word is
**invisible to the motion screen's discovery** even though the library publishes it correctly. This
isn't a library defect to fix in FrcCatalyst, but it is worth naming in the library's own
`CatalystMechanism`/`StateMachineCore` docs as a dashboard-compatibility note, since Catalyst's own
`docs/index.md:353-354` recommends `Superstructure` as the idiomatic name.

## 8. Pose / swerve / vision (screens: motion, field)

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Physics/PoseArray` | double[3] | cat_model.c:440 | `PhysicsCore.java:556` | PUBLISHED, opt-in (`PhysicsCore` must be built) |
| `/Catalyst/Swerve/Pose` | double[3] fallback | cat_model.c:441 | **MISMATCH** — the library's `SwerveSubsystem` publishes `Physics/Pose` as a `Pose2d` **struct** (`PhysicsCore.java:555`, `docs/advanced/physics.md:99-101`), not a plain `double[3]` at `/Catalyst/Swerve/Pose`. The only plain-array pose the library ever writes is `Physics/PoseArray`. `docs/advanced/simulation.md:325` further only ever names `/Catalyst/Swerve/Pose` as an AdvantageScope struct add, and its exact shape/type was not independently confirmed in this pass. | **MISMATCH — unverified exact type**, flag for follow-up |
| `/Catalyst/Swerve/ModuleStates` | double[8] | cat_model.c:449 | `SwerveSubsystem`, `CHANGELOG.md:1762`, `docs/advanced/logging.md:83` — published as `SwerveModuleState[]` (a WPILib struct array), not confirmed as a flat `double[8]` | **MISMATCH — unverified exact type** |
| `/Catalyst/Swerve/ModuleTargets` | double[8] | cat_model.c:455 | same source as above | **MISMATCH — unverified exact type** |
| `/Catalyst/Swerve/HeadingDeg` | double | cat_model.c:461 | **No publisher found** in alpha-7 — `SwerveSubsystem` publishes heading only inside `ModuleStates`/`Pose`, never a standalone `HeadingDeg` scalar | MISSING |
| `/PathPlanner/activePath` | double[64×3] | cat_model.c:462 | PathPlanner's own telemetry, not Catalyst | PUBLISHED (not library's) |
| `/Catalyst/Vision/Health/{Level,Summary,Rows}` | double/string/string[] | cat_model.c:469-475 | `VisionHealth.java:57-64` | PUBLISHED, 2.x only |
| `/limelight<name>/{tv,tid}` | double | cat_model.c:490,497-500 | Limelight's own NT interface, not Catalyst | PUBLISHED (not library's) |

**Struct vs. flat-array is the real issue here.** `nt4.c`'s client decodes structs as runs of
little-endian doubles by width (`catalyst-contract.md:33`: "Pose2d 24, SwerveModuleState 16"), and
`cat_model.c` reads `/Catalyst/Swerve/{Pose,ModuleStates,ModuleTargets}` with `num_array`-style getters
(cat_model.c:441,449,455) that appear to expect **plain `double[]`** topics, going by their call
signatures matching the same helper used for genuinely flat `double[]` topics elsewhere (e.g.
`AutoStartCheck`'s `Expected`/`Current`, `AutoStartCheck.java:41-42`, which really are `double[]`). If
`SwerveSubsystem` in fact publishes `Pose`/`ModuleStates`/`ModuleTargets` as WPILib struct topics
(`Pose2d.struct`, `SwerveModuleState.struct[]`), the tablet's struct-decoder in `nt4.c` may already
handle this correctly by byte-width (matching `catalyst-contract.md:33`'s own claim) — in which case
this is **not** a real mismatch, only a naming coincidence with `PoseArray`. This needs one thing this
pass could not do: reading `SwerveSubsystem.java`'s exact `CatalystLog.log(...)` calls for `Pose`,
`ModuleStates`, and `ModuleTargets` side by side with `nt4_get_numbers`'s call sites in `cat_model.c`
around lines 440-461, to confirm struct-vs-array agreement. Flagged here as the single highest-value
follow-up read before trusting the motion/field screens' pose and swerve-arrow rendering.

`/Catalyst/Swerve/HeadingDeg` (MISSING) is the one clean, unambiguous gap in this section — see §11
item 3.

## 9. Systemcore, via catalyst-agent HTTP and NT fallback (screen: systemcore)

Already exhaustively covered by `docs/systemcore.md`, which cites `SystemCoreStatus.java`,
`CANBusHealth.java`, `MotorHistory.java` with file:line. This page adds only what that page didn't
already nail down:

- catalyst-agent's HTTP routes are confirmed against the tablet's own call sites:
  `GET /api/system` at `components/catalyst/src/cat_sc_io.c:160` and `components/assist/src/as_tools.c:901,941`;
  `GET /api/motor-history` at `cat_sc_io.c:203` and `as_tools.c:1087`. Port 9010
  (`CAT_AGENT_PORT`, `components/catalyst/include/cat_sc.h:48`) matches the agent's own
  `agent/overlay/usr/local/bin/catalyst-agent/catalyst_agent.py:48` (`PORT = 9010`) exactly — no
  mismatch, despite `docs/utilities/motor-history.md` saying `:4800` (a **library-side documentation
  bug**, already called out in `catalyst-contract.md:28`).
- The tablet gives up on the agent after 3 silent polls (`CAT_AGENT_GIVE_UP`, `cat_sc.h:49-51`) and
  polls `/api/system` every 3.0 s (`CAT_AGENT_POLL_S`), `/api/motor-history` every 30 s — matching
  `systemcore.md:22-24` exactly.
- `/Catalyst/Systemcore/NetworkInterfaces` (string[]) is read at `ui_apps_sc.c:921` and is PUBLISHED
  (`systemcore.md:76`), but when absent the tablet's own empty-state text
  (`ui_apps_sc.c:941`) says so explicitly — useful confirmation this path is exercised in practice.

## 10. Tunables (screen: tune) — the second major finding

Tablet reader: `cat_model.c` `read_tunables()`, lines 524-573.

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Tunables/.manifest` | string (JSON array `[{key,name,group,unit,min,max,step}]`) | cat_model.c:528 | **No publisher exists anywhere.** Confirmed absent by full-text search of alpha-7, alpha-6, and v1.1.0 for `.manifest` and `Tunables` (only match: `org.wpilib.tunables`, the *WPILib* auto-chooser package, unrelated). | **MISSING** |
| `/Catalyst/Tuning/<Name>/{kP,kI,kD,kS,kV,kA,kG}` | double | cat_model.c:554-565 (fallback path) | `TunableGains`, `TunableGains.java:42-54`, backed by `TunableNumber`, `TunableNumber.java:34,48-56` | PUBLISHED |
| `/Catalyst/Tuning/<Name>/MM/{CruiseVelocity,Acceleration,Jerk}` | double | cat_model.c:554-565 | `TunableGains.java:52-54` | PUBLISHED |
| `/Catalyst/Tuning/<Name>/Diff/{kP,...,kA}` | double | cat_model.c:554-565 | `DifferentialWristMechanism.java:96-98` | PUBLISHED |

The tablet already has a graceful fallback for this (`cat_model.c:551-565`, comment: *"1.x
TunableNumbers: no manifest, no ranges. Console can't tune these; the tablet can."*) — it discovers
every topic under `/Catalyst/Tuning/` by prefix and infers a display name and group from the NT path
(cat_model.c:559-562), with `min = max = step = NaN`. So the tune screen is not empty, it is
**degraded**: every gain shows as an unbounded numeric field instead of a bounded Bezel level with
correct units and step size, and gains cannot be grouped the way a manifest's `group` field would
allow. This affects every 2.x robot today, not just 1.x as the tablet's own comment implies — alpha-7's
`TunableGains`/`TunableNumber` have exactly the same shape as 1.x's. **Fix in §11 item 4.**

## 11. Auto chooser (screen: auto)

Tablet reader: `cat_model.c` `read_autos()`, lines 575-594.

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Auto Selector/options` | string[] | cat_model.c:577,585 | `AutoSelector`, backed by `org.wpilib.tunable.Selectable` via `Tunables.publish(dashboardKey, chooser)` (`AutoSelector.java:80`); dashboard key defaults to `"Auto Selector"` (`AutoSelector.java:57-59`), landing at the **absolute** path `/Auto Selector` per alpha-7's tunables backend (`AutoSelector.java:73-75`) | PUBLISHED |
| `/Auto Selector/selected` | string, tablet **writes** this (`cat_select_auto()`, `cat_model.c:705-711`) | cat_model.c:590 | same `Selectable` | PUBLISHED |
| `/Auto Selector/active` | string | cat_model.c:592 | same `Selectable` | PUBLISHED, **unverified exact field name** — `Selectable`'s NT wire shape comes from `org.wpilib.tunables`, an external WPILib artifact with no source in this repo checkout; `active` vs `selected` semantics (which one reflects what the robot is actually running vs. what the operator picked) was not confirmed against `Selectable`'s own javadoc |
| `/SmartDashboard/Auto Selector/{options,selected,active}` | same | cat_model.c:577,581 | 1.x's `SendableChooser`-backed `AutoSelector`, confirmed present in `v1.1.0` (no `org.wpilib.tunable` there) | PUBLISHED, 1.x |

No mismatch to fix. Flag: `AutoStartCheck` (`AutoStartCheck.java:36-44`, §12) is wired to
`AutoSelector.selectedStartingPose()` and is genuinely useful for the tablet's auto screen (comparing
the chosen auto's start pose against the live pose before the match) but the tablet does not appear to
read `/Catalyst/Auto/StartCheck/*` from the **auto** screen — only from **preflight**
(`cat_model.c:630-633`). Worth a UI-side suggestion, not a library one.

## 12. Preflight / SystemCheck / Calibration (screen: preflight)

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Preflight/{Ready,Summary,Findings}` | bool/string/string[] | cat_model.c:600-606 | `Preflight.java:127-131` — exact match, `Preflight/Ready`, `Preflight/Summary`, `Preflight/Findings` | PUBLISHED |
| `/Catalyst/SystemCheck/<name>/{Ready,Report}` | bool/string | cat_model.c:617-624 | `SystemCheck.java:33-35` | PUBLISHED |
| `/Catalyst/Calibration/WheelRadius/{Status,CorrectedRadiusInches,PercentChange}` | string/double/double | cat_model.c:626-628 | `WheelRadiusCalibration.java:32`, `docs/subsystems/index.md:166` | PUBLISHED |
| `/Catalyst/Auto/StartCheck/{Ready,DistanceMeters,HeadingErrorDeg}` | bool/double/double | cat_model.c:630-633 | `AutoStartCheck.java:38-40` | PUBLISHED |

Fully matched section, no gaps. `/Catalyst/Calibration/SlipCurrent/*` (`SlipCurrentCalibration.java:28`)
is published by the library but **not read by the tablet at all** — see §14.

## 13. Controls manifest (screen: controls) — the third major finding

| Topic | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/Controls/.manifest` | string, JSON `[{control, action, controller?, combo?}]` | `CAT_CTL_TOPIC`, `components/catalyst/include/cat_sc.h:223`, read at `ui_apps_sc.c:2005` | **No publisher anywhere in the library.** Confirmed by search of all three checkouts. `systemcore.md:117-123` already documents this as a known gap: "Published by the robot program (Catalyst X1 does; the library has no publisher of its own)." | **MISSING (already known)** |

The tablet's controls screen has a well-written empty state that prints the exact topic and a sample
JSON schema when nothing is published (`ui_apps_sc.c:2028-2035`) — so this degrades gracefully, but the
**controls** screen is functionally empty on every stock Catalyst robot until the team hand-rolls a
publisher (as Catalyst X1's own robot code does). See §14 item 1 for the concrete fix.

## 14. States / state machines / autonomy (screen: states)

Tablet reader: `components/catalyst/src/cat_sc_io.c` lines 355-430; UI in `ui_apps_sc.c` 1613-1695.

| Topic pattern | Type | Tablet line | Library source | Status |
|---|---|---|---|---|
| `/Catalyst/<...>/State` (any depth, discovered) | string | cat_sc_io.c:365-410 | `CatalystStateMachineLog`, `CatalystStateMachineLog.java:40`, `systemcore.md:104-108` | PUBLISHED |
| `/Catalyst/<...>/Phase` | string | cat_sc_io.c:404-408 | `CatalystStateMachineLog`, `systemcore.md:106-111` | PUBLISHED |
| `<base>/Counters/{Transitions,Rejections,Timeouts}` | double | `ui_apps_sc.c:1613-1619` | `CatalystStateMachineLog` publishes `Counters/{Transitions,Rejections,Timeouts,Aborts,Yields}` (`systemcore.md:107`) | PUBLISHED, tablet reads a **subset** (misses `Aborts`, `Yields` — a tablet-side gap, not a library one) |
| `/Catalyst/Autonomy/Tasks/Running` | string | cat_sc_io.c:356 | `AutonomyBoard`, `systemcore.md:112-114`: `Autonomy/Tasks/{Running,Held,Explain}` | **MISMATCH — tablet reads a fixed 4-topic subset** |
| `/Catalyst/Autonomy/Chase/Target` | string | cat_sc_io.c:357 | `AutonomyBoard` publishes `Chase/{Target,Why,Rejected}` | same |
| `/Catalyst/Autonomy/Intent/Guess` | string | cat_sc_io.c:358 | `AutonomyBoard` publishes `Intent/{Guess,HitRate,Samples,Explain}` | same |
| `/Catalyst/Autonomy/Authority/Binding` | string | cat_sc_io.c:359 | `AutonomyBoard` publishes `Authority/{Scale,Binding,Explain}` | same |
| — (not read at all) | — | — | `AutonomyBoard` also publishes `Power/{Deficit,Shed,Short,Explain}` and `Situation/*` (`systemcore.md:113-114`, `AutonomyBoard.java:37`) | **entirely unread by the tablet** |

This is the mirror image of the tunables/controls findings: **the library already publishes more than
the tablet reads.** `AutonomyBoard`'s fixed schema (`CHANGELOG.md:280-281`: "publishes all of the above
under `/Catalyst/Autonomy/` on a fixed schema, only when values change") includes the *reasoning*
strings (`Why`, `Explain`, `Rejected`) that would make the states screen's Autonomy lane genuinely
useful in a pit ("why did it choose this target and not that one"), plus power-budgeting and situation
data the states screen never surfaces. **This is a tablet-firmware fix, not a library change** — noted
here only because the cross-reference makes it obvious, and because it changes the priority
calculation for any library work on `AutonomyBoard`: don't add topics, wire up the ones that exist.

## 15. Motor history (screen: motors)

Already covered exhaustively by `docs/systemcore.md` §"MotorHistory" with citations to
`MotorHistory.java`. Confirmed against tablet source: `/Catalyst/MotorHistory/Rows` parsed at
`components/catalyst/src/cat_sc.c:264` against the schema in `cat_sc.h:204-206`; used only as a
fallback when catalyst-agent's `/api/motor-history` is unavailable (`cat_sc_io.c:326`, "totals only,
never identities/boots" — matches `systemcore.md:87-93` exactly). No mismatch.

## 16. Recorder (screen: recorder)

The recorder's fixed column sets (`ui_apps_sc.c` ~2170-2205) all reference topics already covered
above (Swerve, Physics, Brownout, Loop, Systemcore, per-mechanism keys) plus a fully general "any
announced numeric topic under `/Catalyst/`" custom mode (`ui_apps_sc.c:2301`). No library gap — this
screen is designed to degrade to whatever the robot actually publishes, by design
(`docs/catalyst-contract.md`'s own "never zero, only absent" rule, mirrored at `cat_sc.h:17-18`).

---

## Library changes to make (MISMATCH / MISSING items)

### 1. Publish a real battery voltage, unconditionally (P0)

**Problem:** no FrcCatalyst 2.x class publishes a battery voltage topic that exists on every robot
with zero setup. `Systemcore/BatteryVolts` needs a Systemcore plus one line
(`HealthMonitor.systemCoreChecks()`); `Brownout/MeasuredVoltage` needs a `BrownoutMonitor` built and
called every loop; `Status/BatteryVolts` is not a library topic at all, only an example convention. A
1.x team, or a 2.x team that skips both opt-ins, gets a blank battery reading on every dashboard,
including Catalyst Console — not just the tablet.

**Fix:** add a battery-voltage line to `RobotIdentity`'s per-loop responsibilities, or a new
one-line-setup class, e.g.:

```java
// frc.lib.catalyst.util.BatteryMonitor — new class
public final class BatteryMonitor {
    private BatteryMonitor() {}
    /** Call once per loop (e.g. from robotPeriodic). Zero-config, always available. */
    public static void update() {
        CatalystLog.log("Status/BatteryVolts", RobotController.getBatteryVoltage());
    }
}
```

Path/type/rate: `/Catalyst/Status/BatteryVolts`, double, every loop — this promotes the example's own
convention (`RobotContainer.java:820`) into the library, which also resolves the APP-LEVEL
classification above without changing any dashboard's expected path. Simplest version: fold this one
line into `RobotIdentity.declare()`'s existing per-loop hook if one exists, or document it as the first
line of `robotPeriodic()` in the quickstart. Priority **P0** — pulse's battery band is empty without
it on any robot that hasn't opted into `BrownoutMonitor` or `HealthMonitor.systemCoreChecks()`.

### 2. Remove or wire up `/Catalyst/Status/CanUtilization` (P2)

**Problem:** `cat_model.c:258-260` falls back to a single scaled double at this path when
`Systemcore/CanUtilization` (double[5]) is absent, but nothing in the library ever publishes it. Dead
fallback on both sides.

**Fix:** either (a) have `CANBusHealth` publish a single robot-wide utilization scalar at this path for
non-Systemcore (1.x, CANivore-only) robots where the five-bus model doesn't apply, or (b) do nothing —
this is a P2 cleanup, not a functional gap, since `CAN/Health/<bus>/Utilization` already covers every
robot with a `CatalystCANBus` registered. Recommend (b) unless a 1.x team specifically wants a single
CAN-load number without the five-bus machinery.

### 3. Publish `/Catalyst/Swerve/HeadingDeg` (P1)

**Problem:** the tablet's motion screen reads a standalone heading scalar that nothing publishes.
Heading is derivable from `ModuleStates`/`Pose` but the tablet apparently expects it pre-computed.

**Fix:** in `SwerveSubsystem`'s existing telemetry publish (same call site as `ModuleStates`/
`ModuleTargets`, per `CHANGELOG.md:1762` and `docs/advanced/logging.md:83`):

```java
CatalystLog.log("Swerve/HeadingDeg", getHeading().getDegrees());
```

Path/type/rate: `/Catalyst/Swerve/HeadingDeg`, double, same cadence as `ModuleStates` (every loop).
Priority **P1** — the motion screen's swerve-arrow view can currently only get heading from the pose
array, which is opt-in (`PhysicsCore`); a robot with `SwerveSubsystem` but no `PhysicsCore` has modules
drawn with no heading reference.

### 4. Confirm (or fix) `Pose`/`ModuleStates`/`ModuleTargets` struct vs. array shape (P0, verification)

**Problem:** §8 above found the tablet's own read calls for `/Catalyst/Swerve/{Pose,ModuleStates,
ModuleTargets}` pattern-match the flat-`double[]` getters used elsewhere in `cat_model.c`, but the
library's own docs (`docs/advanced/physics.md:99-101`, `docs/advanced/simulation.md:325`) describe
`Physics/Pose` as a `Pose2d` **struct**, and `CHANGELOG.md:1762` describes `Swerve/ModuleStates` as a
`SwerveModuleState[]` **struct array** — not confirmed as flat doubles. This could be a real bug (the
tablet renders garbage or nothing for these three topics on a real robot) or a non-issue (the tablet's
NT4 client decodes structs the same way, per `catalyst-contract.md:33`).

**Fix:** not a library code change — a verification task. Read `SwerveSubsystem`'s exact
`CatalystLog.log(...)` calls for these three keys next to `cat_model.c:440-461`'s exact `nt4_get_*`
call signatures, or capture real NT4 traffic from a `SwerveSubsystem` and diff against what the tablet
parses. **Priority P0** because it blocks trusting the motion and field screens' most visually
prominent feature (the swerve module arrows) on real hardware, but it is a read-and-confirm task, not
a code change, until proven broken.

### 5. Add a Tunables manifest publisher (P1)

**Problem:** §10. No FrcCatalyst class publishes `/Catalyst/Tunables/.manifest`. Every gain on a 2.x
robot — including the six mechanism types that get `TunableGains` for free — shows on the tablet with
no declared range, unit, or group, because `TunableNumber` (`TunableNumber.java:32-111`) carries only a
key and a default value.

**Fix:** extend `TunableGains` (and, ideally, `TunableNumber` itself) to register metadata into a
process-wide manifest that gets flushed once per boot (mirroring `RobotIdentity`'s "publish once, NT4
retains it" pattern, `RobotIdentity.java:80-87`):

```java
// frc.lib.catalyst.util.TunablesManifest — new class, analogous to CANRegistry
public final class TunablesManifest {
    public record Entry(String key, String name, String group, String unit,
                         double min, double max, double step) {}
    private static final List<Entry> entries = new ArrayList<>();

    public static synchronized void register(String key, String name, String group,
                                              String unit, double min, double max, double step) {
        entries.add(new Entry(key, name, group, unit, min, max, step));
        publish();
    }
    private static void publish() {
        // serialize `entries` to the JSON array shape cat_model.c:526-548 already parses:
        // [{key,name,group,min,max,step,unit}, ...]
        CatalystLog.log("Tunables/.manifest", toJson(entries));
    }
}
```

`TunableGains`'s constructor (`TunableGains.java:31-60`) is the natural place to call
`TunablesManifest.register(...)` once per gain, with sensible defaults it already has context for: a PID
gain's `unit` is dimensionless, Motion Magic's `CruiseVelocity`/`Acceleration`/`Jerk` have known units
from the mechanism's own config. Path/type/rate: `/Catalyst/Tunables/.manifest`, string (JSON), once at
boot plus once per new tunable registered. Priority **P1** — the tune screen works today via the
fallback path, but every slider is unbounded and ungrouped; this is the single highest-value UX fix
available on the library side for the tune screen.

### 6. Add a Controls manifest publisher (P1, already tracked)

**Problem:** §13, already documented as a known gap in `systemcore.md:178-179`. No FrcCatalyst class
publishes `/Catalyst/Controls/.manifest`; only hand-rolled robot code (Catalyst X1) does.

**Fix:** a `ControlsManifest` helper robot code calls as it binds buttons, e.g.:

```java
// frc.lib.catalyst.driver.ControlsManifest — new class
public final class ControlsManifest {
    public static void bind(String control, String action) { bind(control, action, null, null); }
    public static void bind(String control, String action, String controller, String combo) {
        entries.add(new Entry(control, action, controller, combo));
        publish(); // -> /Catalyst/Controls/.manifest, same JSON shape cat_sc.h:6,223 already expects
    }
}
```

The idiomatic call site would be inside `CommandXboxController`-style binding helpers in
`configureBindings()`, one line per `.onTrue(...)`/`.whileTrue(...)` binding. Path/type/rate:
`/Catalyst/Controls/.manifest`, string (JSON `[{control,action,controller?,combo?}]`), republished on
each `bind()` call (rare, boot-time only in practice). Priority **P1** — the controls screen is
functionally empty on every stock robot without this.

---

## Proposed new integration

Ideas the tablet's own placeholder strings and empty states already gesture at (`ui_apps_sc.c:1043-1045,
826, 874, 1313-1316`), things the library could add cheaply, and one thing that should stay off the
table:

1. **A single `/Catalyst/Tablet/Summary` topic.** Every screen currently reconstructs "is the robot
   healthy" from a dozen scattered topics (battery, loop, CAN, alerts, preflight). A one-line JSON
   summary — `{ok, worstAlert, battery, canWorstBus, preflightReady}` — published by, say, `Preflight`
   or a new tiny `RobotSummary` class alongside its existing `Ready`/`Summary`/`Findings` triplet, would
   let the pulse screen (and Catalyst Console) render its headline state from one subscription instead
   of the ~15 the tablet currently reads across `read_mode`, `read_power`, `read_loop`, `read_can`,
   `read_alerts`, and `read_checks`. Cheap: it is a reduction of data the library already computes.
2. **Alert acknowledgement, read-only-safe.** The tablet's alerts screen and the assist's `get_alerts`
   tool are read-only, correctly. A safe extension that doesn't violate "never commands the robot":
   `AlertManager` could accept an acknowledgement write to a `/Catalyst/Alerts/Ack/<hash>` boolean the
   robot code itself decides whether to honor (e.g. silencing a repeat chime, never suppressing the
   underlying condition). This keeps the robot in control of what acknowledgement means, while letting
   a technician say "seen it" from the pit without touching robot logic. Optional, and only worth doing
   if a real workflow needs it — don't build it speculatively.
3. **A read-only health-snapshot HTTP endpoint on catalyst-agent, beyond `/api/system`.** The assist's
   `systemcore_health` and `motor_history` tools already reconstruct a diagnosis from raw `/api/system`
   and `/api/motor-history` (`as_tools.c:939-1141`). A `/api/health` route that folds in
   `/Catalyst/Health/*`, `/Catalyst/Preflight/*`, and `/Catalyst/Alerts/*` (read over NT by the agent
   itself, which already runs on the same box) into one JSON blob would save the assist several tool
   calls per diagnosis and give any HTTP client — not just the tablet — one place to ask "is this robot
   OK." This is agent-side Python, not FrcCatalyst Java, but belongs in the same `agent/` source tree.
4. **A log index for pulling logs remotely.** The tablet's **logs** screen reads `.wpilog`/`.dslog`/
   `.dsevents` straight off microSD (`docs/catalyst-contract.md:77-85`) — it never asks the robot for
   them. catalyst-agent already indexes `robotProgram.log` (60 journal lines) in `/api/system`
   (`systemcore.md:56`); a `/api/logs` route listing available `.wpilog` files by match/timestamp (the
   Systemcore already writes these somewhere for DS replay) would let the tablet pull a log from a
   robot whose DS laptop isn't on the cart, instead of requiring the microSD to be pulled by hand.
   Worth scoping only if teams actually hit this — flagged because the tablet's own logs screen
   currently has no path to the robot at all, only to a card.
5. **Do not add remote command triggers.** Every gap found above is a missing *read* or *metadata*
   topic. Nothing here should turn into "let the tablet start test mode" or "let the tablet trigger
   SysId" — that would break the one invariant every doc in this repo repeats
   (`catalyst-contract.md:10-13`, `README.md:50-52`): diagnostics stay Driver Station Utility op modes,
   and the tablet only ever shows their results.

---

## Checklist

| Item | Library file to change | Priority | Effort |
|---|---|---|---|
| Zero-config battery voltage (`Status/BatteryVolts`) | new `frc.lib.catalyst.util.BatteryMonitor`, or fold into `RobotIdentity` | P0 | S |
| Confirm `Swerve/{Pose,ModuleStates,ModuleTargets}` struct vs. array shape | verification only — `SwerveSubsystem`, read against `cat_model.c:440-461` | P0 | S (a read, not a code change) |
| Publish `Swerve/HeadingDeg` | `SwerveSubsystem`'s telemetry publish (same site as `ModuleStates`) | P1 | S |
| Tunables manifest (`Tunables/.manifest`) | new `frc.lib.catalyst.util.TunablesManifest`, called from `TunableGains` | P1 | M |
| Controls manifest (`Controls/.manifest`) | new `frc.lib.catalyst.driver.ControlsManifest` | P1 | M |
| Remove or wire up dead `/Catalyst/Match/TimeLeft` fallback | none (tablet-side) or a match-clock publisher if wanted | P2 | S |
| Remove or wire up dead `/Catalyst/Status/CanUtilization` fallback | none, or `CANBusHealth` scalar for non-Systemcore robots | P2 | S |
| Wire up `AutonomyBoard`'s unread fields (`Why`, `Explain`, `Rejected`, `Power/*`, `Situation/*`) | tablet-side (`cat_sc_io.c:355-360`), not library | — (tablet fix) | M |
| Recorder's `MECH_KEYS` missing `VelocityMPS` | tablet-side (`ui_apps_sc.c` ~2200), not library | — (tablet fix) | S |
| `docs/utilities/motor-history.md` says agent port `:4800`, agent is really `:9010` | library docs fix, `docs/utilities/motor-history.md` | P2 (docs only) | S |
| Proposed: `/Catalyst/Tablet/Summary` rollup topic | new small class, e.g. beside `Preflight` | P2 (nice to have) | M |
| Proposed: `/api/health` on catalyst-agent | `agent/overlay/.../catalyst_agent.py` | P2 (nice to have) | M |
| Proposed: `/api/logs` index on catalyst-agent | `agent/overlay/.../catalyst_agent.py` | P2 (nice to have) | L |
