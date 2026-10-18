# What Catalyst Tab reads and writes on a robot

Catalyst Tab is a pocket Catalyst Console: it speaks NetworkTables 4 the way Console's
`src-tauri/src/nt4.rs` does and reads the topics Catalyst publishes. This page is the contract,
distilled from FrcCatalyst `main` (1.12.0) and `upgrade/alpha-7` (2.0.0-beta.2), Catalyst Console 2.0.0
and Catalyst X1's `tools/preflight.py`. The C side of it is `components/catalyst/src/cat_model.c`.

## The rule it inherits from Console

**It never controls the robot.** Catalyst exposes no command triggers over NetworkTables — the
diagnostic routines (System check, Zero wheels, Find motor, wheel-radius calibration, SysId) are Driver
Station *Utility* op modes. Catalyst Tab shows their results and says which op mode to run. The only
writes it makes are the ones Console makes:

1. Tunables the robot declares in `/Catalyst/Tunables/.manifest` (JSON: `[{key, name?, group?, min?,
   max?, step?, unit?}]`), and 1.x `TunableNumber`s under `/Catalyst/Tuning/`.
2. The auto choice: `…/Auto Selector/selected` (`/Auto Selector` on 2.x, `/SmartDashboard/Auto
   Selector` on 1.x).
3. A Limelight's own `ledMode` (blink to identify a camera).

Nothing under `/FMSInfo` or `/.schema` is ever written.

## Connection

- `ws://<addr>:5810/nt/catalyst-tab`, offering `v4.1.networktables.first.wpi.edu` then
  `networktables.first.wpi.edu`; WebSocket pings are answered.
- Addresses tried in turn (Console's `main.rs:46-73`): `robot.local`, `172.26.0.1` (Systemcore USB),
  `172.30.0.1` (Systemcore Wi-Fi), `10.TE.AM.2`, `roborio-TEAM-frc.local`, `172.22.11.2` (roboRIO USB).
- One subscription, `periodic` 0.05 s, on the prefixes a pit tool needs rather than `/`: `/Catalyst/`,
  `/FMSInfo/`, `/Auto Selector/`, `/SmartDashboard/`, `/limelight`, `/PathPlanner/`. A 2.x robot
  announces 400–500 topics; this keeps the announce burst small.
- Timestamp handshake every second, `[-1, 0, int, now]`: keepalive, round trip, server clock offset.
- Structs decode as runs of little-endian doubles by width (Pose2d 24, SwerveModuleState 16, …);
  `struct:ControlWord` (2027) maps onto the 2026 FMSControlData bits (enabled 1, auto 2, test 4,
  e-stop 8, FMS 16, DS 32).
- A value the robot isn't publishing shows as "—", never 0. On disconnect everything goes stale.

## Topics, by what the tablet shows

| Shows | Topics |
|---|---|
| **Identity** | `/Catalyst/Robot/Identity/{Name,TeamNumber,Season,Controller}`, `/Catalyst/Robot/Software/{CatalystVersion,CatalystGitSha,RobotCodeVersion,WPILibVersion}` |
| **Mode** | `/FMSInfo/ControlWord` (2027) else `/FMSInfo/FMSControlData`; `/FMSInfo/IsRedAlliance`, `StationNumber`, `OpMode` |
| **Battery** | first present of `/Catalyst/Status/BatteryVolts`, `/Catalyst/Brownout/MeasuredVoltage`, `/Catalyst/Systemcore/BatteryVolts`; `/Catalyst/Brownout/{TotalCurrent,PredictedVoltage,AtRisk}`; `/Catalyst/Systemcore/BrownedOut`; floor `/Catalyst/Robot/Power/BrownoutVolts` |
| **Loop** | `/Catalyst/Loop/Robot/{LastMs,AverageMs,MaxMs,OverBudget}` against 20 ms |
| **CAN** | `/Catalyst/CAN/Devices` (`"bus\|id\|type\|name"`, names may contain `\|`), `/Catalyst/Systemcore/{CanUtilization,CanDown}`, `/Catalyst/CAN/Health/<bus>/{OK,Utilization,BusOffCount,REC,TEC}` |
| **Devices** | 2.x `/Catalyst/Devices/{Motors,Cameras}/{Expected,Connected,Rows}`, `/Catalyst/Devices/Controller/{Kind,Connected}` |
| **Power** | `/Catalyst/Robot/Power/{Module,Channels,ChannelsInUse}` (`"channel\|what"`); live channels only if the team publishes the PDH (`/SmartDashboard/PDH/Chan0…`, `Voltage`, `TotalCurrent`) |
| **Mechanisms** | `/Catalyst/<Mech>/{State,AngleDegrees,SetpointDegrees,VelocityRPS,SetpointRPS,CurrentAmps,StatorCurrentAmps,TemperatureC,AtSetpoint,AtSpeed}` |
| **Alerts** | `/Catalyst/Alerts/{Errors,Warnings,Info}` (`"[Subsystem] message"`), `ErrorCount`, `WarningCount`; WPILib groups `/SmartDashboard/<group>/{errors,warnings,infos}` |
| **Health** | `/Catalyst/Health/<subsystem>/<id>/{description,severity,firing,detail}` |
| **Pose** | `/Catalyst/Physics/PoseArray` else `/Catalyst/Swerve/Pose`; `/Catalyst/Swerve/{ModuleStates,ModuleTargets,HeadingDeg}`; `/PathPlanner/activePath` |
| **Vision** | 2.x `/Catalyst/Vision/Health/{Level,Summary,Rows}` (`"name\|STATE\|detail\|fps\|tempC\|connected"`); `/limelight-<name>/{tv,tid,tx,ty}` |
| **Systemcore** | `/Catalyst/Systemcore/{CpuPercent,TempCelsius,RamFraction,StorageFraction,EmmcPreEol,TeamNumber}` |
| **Routines' results** | `/Catalyst/SystemCheck/<name>/{Ready,Report,<test>}`, 2.x `/Catalyst/Preflight/{Ready,Summary,Findings}`, `/Catalyst/Calibration/WheelRadius/*`, `/Catalyst/Auto/StartCheck/*` |
| **Motor history** | `/Catalyst/MotorHistory/Rows` (2.x) |

## Thresholds (Console's, so the two never disagree)

- Battery: ≥ 12.5 V charged, ≥ 12.2 V "swap before a match", below that low; preflight FAIL < 11.8,
  WARN < 12.4.
- Loop: warn above 0.75 × budget, fault above the budget.
- CAN bus: warn above 0.70, fault above 0.85 (a bus above 0.90 is critical in Console's CAN model;
  `can_s0+s1` and `can_s3+s4` share an SPI host, so a pair above 1.0 is critical).
- Systemcore percentages: warn 85, fault 95. Temperature ≥ 80 °C warns.
- Status colours mean status and nothing else, and every status carries a shape: ● ok, ◆ check,
  ■ fault, ○ stale.

## Preflight

Ported from X1's `tools/preflight.py`, generalised (X1-only checks dropped), plus the library's own
`/Catalyst/Preflight/Findings` and `/Catalyst/SystemCheck/*/Ready`. See `cat_preflight.c` for the
table: link, program running, e-stop, DS attached, enabled (don't deploy), alliance, battery, brownout,
CAN down, CAN load, alerts, health checks, team number, pose seeded, gyro alive, cameras, tunables,
wheel radius, deployed build.

## Driver Station logs (read from microSD)

- **`.wpilog`** (2027 DS): `"WPILOG"`, u16 version, u32 extra-header length; records with a bitfield
  byte giving the widths of entry id, payload size and timestamp. Entries read:
  `DS:/Dscomm/Status/{Battery,PacketTime,CPU,CanBusUtilizations}`.
- **`.dslog`** (NI, v3/v4): 20-byte header, 50 Hz records: trip (b0 × 0.5 ms), packet loss, battery
  (b2 + b3/256 V), CPU (b4 × 0.5 %), status byte (inverted; brownout bit 7), CAN (b6 × 0.5 %).
  Record length depends on the PD type — the parser branches on it rather than assuming 35 bytes.
- **`.dsevents`**: 20-byte header, then 16-byte LabVIEW timestamp + u32 length + text records.
