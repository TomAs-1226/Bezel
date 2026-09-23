#!/usr/bin/env python3
"""A pretend Catalyst 2.x robot: an NT4 server publishing what FrcCatalyst publishes.

Run it, then point the simulator (or the tablet, with --host 0.0.0.0) at it:

    python tools/fake_robot.py [--host 127.0.0.1] [--port 5810] [--scenario pit|match|sick]

It speaks NetworkTables 4 the way a robot does — announce, msgpack value frames, the timestamp
handshake, client publishes — and publishes the topic surface in docs/catalyst-contract.md: identity,
the 2027 ControlWord struct, battery and brownout, loop timing, CAN, the device roster (with one motor
missing), alerts and a firing health check, mechanisms, swerve structs, vision, Systemcore, a PDH,
a tunables manifest, the auto selector and the robot's own preflight findings. Writes to declared
tunables and to the auto selector are taken and echoed back, like a robot would.

Needs: pip install websockets msgpack
"""
import argparse
import asyncio
import json
import math
import random
import struct
import time

import msgpack
import websockets
from websockets.asyncio.server import serve

TYPE_IDS = {"boolean": 0, "double": 1, "int": 2, "float": 3, "string": 4, "json": 4, "raw": 5,
            "boolean[]": 16, "double[]": 17, "int[]": 18, "float[]": 19, "string[]": 20}


def type_id(t):
    return TYPE_IDS.get(t, 5)  # struct:* and other binary types go as raw


def control_word(enabled=False, mode=2, estop=False, fms=False, ds=True):
    w = (mode & 3) << 56
    w |= int(enabled) << 58 | int(estop) << 59 | int(fms) << 60 | int(ds) << 61
    return struct.pack("<Q", w)


def pose2d(x, y, r):
    return struct.pack("<3d", x, y, r)


def modules(pairs):
    return b"".join(struct.pack("<2d", s, a) for s, a in pairs)


class Robot:
    def __init__(self, scenario):
        self.scenario = scenario
        self.t0 = time.monotonic()
        self.topics = {}   # name -> [type, value, id]
        self.next_id = 1
        self.enabled = scenario == "match"
        self.heading = 37.0
        self.build()

    def put(self, name, typ, value):
        if name not in self.topics:
            self.topics[name] = [typ, value, self.next_id]
            self.next_id += 1
        else:
            self.topics[name][1] = value

    def build(self):
        p = self.put
        R = "/Catalyst/Robot/"
        p(R + "Identity/Name", "string", "Ratchet")
        p(R + "Identity/TeamNumber", "int", 5805)
        p(R + "Identity/Season", "int", 2027)
        p(R + "Identity/Controller", "string", "Systemcore")
        p(R + "Software/CatalystVersion", "string", "2.0.0-beta.2")
        p(R + "Software/CatalystGitSha", "string", "03003fb9c1e4")
        p(R + "Software/RobotCodeVersion", "string", "x1-0.9.4")
        p(R + "Software/WPILibVersion", "string", "2027.0.0-alpha-7")
        p(R + "Power/BrownoutVolts", "double", 6.75)
        p(R + "Power/Module", "string", "REV PDH")
        p(R + "Power/Channels", "int", 24)
        chans = ["FL drive", "FL steer", "FR drive", "FR steer", "BL drive", "BL steer", "BR drive", "BR steer",
                 "Shooter L", "Shooter R", "Hood", "Intake", "Arm", "Elevator", "Limelight", "Radio"]
        p(R + "Power/ChannelsInUse", "string[]", [f"{i}|{n}" for i, n in enumerate(chans)])

        p("/FMSInfo/ControlWord", "struct:ControlWord", control_word(self.enabled))
        p("/FMSInfo/IsRedAlliance", "boolean", False)
        p("/FMSInfo/StationNumber", "int", 2)
        p("/FMSInfo/OpMode", "string", "Teleop")

        devs = [("can_s0", 1, "TalonFX", "FL drive"), ("can_s0", 2, "TalonFX", "FL steer"),
                ("can_s0", 3, "TalonFX", "FR drive"), ("can_s0", 4, "TalonFX", "FR steer"),
                ("can_s0", 5, "TalonFX", "BL drive"), ("can_s0", 6, "TalonFX", "BL steer"),
                ("can_s0", 7, "TalonFX", "BR drive"), ("can_s0", 8, "TalonFX", "BR steer"),
                ("can_s0", 20, "Pigeon2", "Gyro"), ("can_s0", 21, "CANcoder", "FL azimuth"),
                ("can_s0", 22, "CANcoder", "FR azimuth"), ("can_s0", 23, "CANcoder", "BL azimuth"),
                ("can_s0", 24, "CANcoder", "BR azimuth"), ("can_s2", 25, "TalonFX", "Shooter L"),
                ("can_s2", 26, "TalonFX", "Shooter R"), ("can_s2", 27, "TalonFX", "Hood"),
                ("can_s2", 28, "TalonFX", "Intake"), ("can_s2", 29, "TalonFX", "Arm"),
                ("can_s2", 30, "TalonFX", "Elevator")]
        p("/Catalyst/CAN/Devices", "string[]", [f"{b}|{i}|{t}|{n}" for b, i, t, n in devs])
        motors = [d for d in devs if d[2] == "TalonFX"]
        missing = {27} if self.scenario != "pit-ok" else set()
        p("/Catalyst/Devices/Motors/Expected", "int", len(motors))
        p("/Catalyst/Devices/Motors/Connected", "int", len(motors) - len(missing))
        p("/Catalyst/Devices/Motors/Rows", "string[]",
          [f"{n}|{b}|{i}|{'false' if i in missing else 'true'}" for b, i, t, n in motors])
        p("/Catalyst/Devices/Cameras/Expected", "int", 1)
        p("/Catalyst/Devices/Cameras/Connected", "int", 1)
        p("/Catalyst/Devices/Cameras/Rows", "string[]", ["limelight-ground|true|31 fps"])
        p("/Catalyst/Devices/Controller/Kind", "string", "Xbox")
        p("/Catalyst/Devices/Controller/Connected", "boolean", True)

        p("/Catalyst/Alerts/Errors", "string[]", ["[Hood] Talon FX 27 isn't answering on can_s2"])
        p("/Catalyst/Alerts/Warnings", "string[]", ["[Vision] limelight-ground dropped to 18 fps",
                                                    "[Drive] FR steer drew 38 A at stall"])
        p("/Catalyst/Alerts/Info", "string[]", ["[Auto] 4-note center selected"])
        p("/Catalyst/Alerts/ErrorCount", "int", 1)
        p("/Catalyst/Alerts/WarningCount", "int", 2)
        H = "/Catalyst/Health/Shooter/HighTemp/"
        p(H + "description", "string", "Shooter L above warn temperature")
        p(H + "severity", "string", "WARN")
        p(H + "firing", "boolean", True)
        p(H + "detail", "string", "71 °C, warn at 70")

        for s in ("Arm", "Elevator", "Shooter", "Intake"):
            p(f"/Catalyst/{s}/State", "string", {"Arm": "STOW", "Elevator": "HOLD", "Shooter": "SPINUP", "Intake": "IDLE"}[s])

        p("/Catalyst/Vision/Health/Level", "int", 1)
        p("/Catalyst/Vision/Health/LevelName", "string", "DEGRADED")
        p("/Catalyst/Vision/Health/Summary", "string", "1 camera, low frame rate")
        p("/limelight-ground/tv", "double", 1.0)
        p("/limelight-ground/tid", "double", 7.0)

        p("/Catalyst/Systemcore/EmmcPreEol", "int", 1)
        p("/Catalyst/Systemcore/TeamNumber", "int", 5805)
        p("/Catalyst/Systemcore/BrownedOut", "boolean", False)
        p("/Catalyst/Systemcore/CanDown", "boolean", False)

        manifest = [
            {"key": "/Catalyst/X1/Driver/MaxSpeed", "name": "max speed", "group": "driver", "min": 0, "max": 5.2, "step": 0.1, "unit": "m/s"},
            {"key": "/Catalyst/X1/Driver/SlowMode", "name": "slow mode", "group": "driver", "min": 0.1, "max": 1, "step": 0.05},
            {"key": "/Catalyst/X1/Align/kP", "name": "align kP", "group": "align", "min": 0, "max": 12, "step": 0.1},
            {"key": "/Catalyst/X1/Align/ToleranceDeg", "name": "tolerance", "group": "align", "min": 0.2, "max": 5, "step": 0.1, "unit": "°"},
            {"key": "/Catalyst/X1/Turret/UseV8", "name": "sotf v8", "group": "turret"},
            {"key": "/Catalyst/Shooter/TargetRPS", "name": "shooter target", "group": "shooter", "min": 0, "max": 100, "step": 1, "unit": "rps"},
        ]
        p("/Catalyst/Tunables/.manifest", "json", json.dumps(manifest))
        p("/Catalyst/X1/Driver/MaxSpeed", "double", 4.6)
        p("/Catalyst/X1/Driver/SlowMode", "double", 0.35)
        p("/Catalyst/X1/Align/kP", "double", 6.5)
        p("/Catalyst/X1/Align/ToleranceDeg", "double", 1.5)
        p("/Catalyst/X1/Turret/UseV8", "boolean", True)
        p("/Catalyst/Shooter/TargetRPS", "double", 62.0)

        A = "/Auto Selector/"
        p(A + "options", "string[]", ["Do nothing", "2-note amp", "4-note center", "5-note source", "Taxi"])
        p(A + "selected", "string", "4-note center")
        p(A + "active", "string", "4-note center")

        p("/Catalyst/Preflight/Ready", "boolean", False)
        p("/Catalyst/Preflight/Summary", "string", "1 blocker, 1 warning")
        p("/Catalyst/Preflight/Findings", "string[]", [
            "[BLOCKER] motors — Hood (can_s2 27) not answering",
            "[warn]    vision — limelight-ground at 18 fps",
            "[ok]      battery",
            "[ok]      gyro"])
        p("/Catalyst/SystemCheck/X1/Ready", "boolean", True)
        p("/Catalyst/SystemCheck/X1/Report", "string", "12 of 12 passed at 14:02")
        W = "/Catalyst/Calibration/WheelRadius/"
        p(W + "Status", "string", "done")
        p(W + "CorrectedRadiusInches", "double", 1.962)
        p(W + "PercentChange", "double", -1.9)
        p("/Catalyst/Auto/StartCheck/Ready", "boolean", False)
        p("/Catalyst/Auto/StartCheck/DistanceMeters", "double", 0.31)
        p("/Catalyst/Auto/StartCheck/HeadingErrorDeg", "double", 4.0)
        p("/PathPlanner/activePath", "struct:Pose2d[]",
          b"".join(pose2d(1.4 + 0.25 * i, 5.5 - 0.12 * i * i / 3, -0.2 * i) for i in range(12)))
        self.tick()

    def tick(self):
        """Advance the robot's physics a little; values that change are re-sent."""
        t = time.monotonic() - self.t0
        p = self.put
        load = 45 + 35 * max(0.0, math.sin(t * 0.6)) if self.enabled else 3.0
        batt = 12.62 - load * 0.012 + 0.03 * math.sin(t * 5)
        if self.scenario == "sick":
            batt -= 0.9
        p("/FMSInfo/ControlWord", "struct:ControlWord", control_word(self.enabled))
        p("/Catalyst/Systemcore/BatteryVolts", "double", round(batt, 3))
        p("/Catalyst/Brownout/MeasuredVoltage", "double", round(batt, 3))
        p("/Catalyst/Brownout/TotalCurrent", "double", round(load, 1))
        p("/Catalyst/Brownout/PredictedVoltage", "double", round(batt - load * 0.015, 2))
        p("/Catalyst/Brownout/AtRisk", "boolean", batt - load * 0.015 < 8.0)
        p("/Catalyst/Loop/Robot/AverageMs", "double", round(11.8 + 1.5 * math.sin(t * 0.3), 2))
        p("/Catalyst/Loop/Robot/LastMs", "double", round(11 + random.random() * 4, 2))
        p("/Catalyst/Loop/Robot/MaxMs", "double", 19.4)
        p("/Catalyst/Loop/Robot/OverBudget", "boolean", False)
        p("/Catalyst/Systemcore/CanUtilization", "double[]",
          [round(0.46 + 0.05 * math.sin(t), 3), 0.0, round(0.31 + 0.03 * math.sin(t * 1.3), 3), 0.0, 0.0])
        p("/Catalyst/Systemcore/CpuPercent", "double", round(38 + 6 * math.sin(t * 0.8), 1))
        p("/Catalyst/Systemcore/TempCelsius", "double", round(54 + 2 * math.sin(t * 0.1), 1))
        p("/Catalyst/Systemcore/RamFraction", "double", 0.41)
        p("/Catalyst/Systemcore/StorageFraction", "double", 0.62)
        for ch in range(24):
            amps = 0.0
            if ch < 8:
                amps = (9 if ch % 2 == 0 else 2.5) * (load / 50) + random.random() * 0.4
            elif ch in (8, 9):
                amps = 18 + 6 * math.sin(t * 0.7) if self.enabled else 0.3
            elif ch == 14:
                amps = 1.1
            elif ch == 15:
                amps = 0.7
            p(f"/SmartDashboard/PDH/Chan{ch}", "double", round(amps, 2))
        p("/SmartDashboard/PDH/Voltage", "double", round(batt, 2))
        p("/SmartDashboard/PDH/TotalCurrent", "double", round(load, 1))

        arm = 12 + 60 * (0.5 + 0.5 * math.sin(t * 0.4))
        p("/Catalyst/Arm/AngleDegrees", "double", round(arm, 2))
        p("/Catalyst/Arm/SetpointDegrees", "double", 72.0 if math.sin(t * 0.4) > 0 else 12.0)
        p("/Catalyst/Arm/CurrentAmps", "double", round(4 + 3 * abs(math.cos(t * 0.4)), 2))
        p("/Catalyst/Arm/TemperatureC", "double", 38.5)
        p("/Catalyst/Arm/AtSetpoint", "boolean", abs(math.cos(t * 0.4)) < 0.1)
        p("/Catalyst/Elevator/PositionMeters", "double", round(0.42 + 0.02 * math.sin(t * 2), 3))
        p("/Catalyst/Elevator/SetpointMeters", "double", 0.42)
        p("/Catalyst/Elevator/CurrentAmps", "double", 6.1)
        p("/Catalyst/Elevator/TemperatureC", "double", 33.0)
        p("/Catalyst/Elevator/AtSetpoint", "boolean", True)
        target = self.topics["/Catalyst/Shooter/TargetRPS"][1]
        spd = target * (1 - math.exp(-((t % 12) / 1.5)))
        p("/Catalyst/Shooter/VelocityRPS", "double", round(spd, 2))
        p("/Catalyst/Shooter/SetpointRPS", "double", target)
        p("/Catalyst/Shooter/StatorCurrentAmps", "double", round(12 + 40 * math.exp(-((t % 12) / 1.5)), 1))
        p("/Catalyst/Shooter/TemperatureC", "double", 71.0)
        p("/Catalyst/Shooter/AtSpeed", "boolean", spd > target * 0.97)

        self.heading += random.uniform(-0.05, 0.05) + (0.8 if self.enabled else 0)
        x = 2.9 + 1.2 * math.cos(t * 0.25)
        y = 4.1 + 0.9 * math.sin(t * 0.25)
        rad = math.radians(self.heading)
        p("/Catalyst/Swerve/Pose", "struct:Pose2d", pose2d(x, y, rad))
        p("/Catalyst/Physics/PoseArray", "double[]", [round(x, 3), round(y, 3), round(rad, 4)])
        p("/Catalyst/Swerve/HeadingDeg", "double", round(self.heading, 3))
        base = 1.4 if self.enabled else 0.0
        ang = 0.6 * math.sin(t * 0.5)
        p("/Catalyst/Swerve/ModuleStates", "struct:SwerveModuleState[]",
          modules([(base * (1 + 0.05 * i), ang + 0.03 * i) for i in range(4)]))
        p("/Catalyst/Swerve/ModuleTargets", "struct:SwerveModuleState[]",
          modules([(base * 1.05, ang) for _ in range(4)]))
        p("/Catalyst/Vision/Health/Rows", "string[]",
          [f"limelight-ground|LOW_FPS|2 tags|{18 + int(3 * math.sin(t))}|52.0|true"])
        p("/Catalyst/Match/TimeLeft", "double", round(max(0.0, 135 - (t % 150)), 1) if self.enabled else -1.0)


class Session:
    def __init__(self, robot, ws):
        self.robot, self.ws = robot, ws
        self.subs = []          # prefixes
        self.announced = set()
        self.pubs = {}          # pubuid -> name
        self.sent = {}          # name -> last value sent

    def matches(self, name):
        return any(name.startswith(p) for p in self.subs)

    async def announce_new(self):
        msgs = []
        for name, (typ, _, tid) in self.robot.topics.items():
            if name in self.announced or not self.matches(name):
                continue
            self.announced.add(name)
            msgs.append({"method": "announce", "params": {"name": name, "id": tid, "type": typ, "properties": {}}})
        if msgs:
            await self.ws.send(json.dumps(msgs))

    async def send_values(self, force=False):
        now = int((time.monotonic() - self.robot.t0) * 1e6)
        out = b""
        for name in self.announced:
            typ, value, tid = self.robot.topics[name]
            if not force and self.sent.get(name) == value:
                continue
            self.sent[name] = value
            out += msgpack.packb([tid, now, type_id(typ), value], use_bin_type=True)
        if out:
            await self.ws.send(out)

    async def on_text(self, text):
        for m in json.loads(text):
            method, params = m.get("method"), m.get("params", {})
            if method == "subscribe":
                self.subs += params.get("topics", [])
                await self.announce_new()
                await self.send_values(force=True)
            elif method == "publish":
                name, typ = params["name"], params["type"]
                self.pubs[params["pubuid"]] = name
                if name not in self.robot.topics:
                    self.robot.put(name, typ, None)
                typ0, _, tid = self.robot.topics[name]
                await self.ws.send(json.dumps([{"method": "announce", "params": {
                    "name": name, "id": tid, "type": typ0, "pubuid": params["pubuid"], "properties": {}}}]))

    async def on_binary(self, data):
        unpacker = msgpack.Unpacker(raw=False)
        unpacker.feed(data)
        for uid, ts, typ, value in unpacker:
            if uid == -1:
                now = int((time.monotonic() - self.robot.t0) * 1e6)
                await self.ws.send(msgpack.packb([-1, now, 2, value]))
                continue
            name = self.pubs.get(uid)
            if not name:
                continue
            print(f"robot: {name} <- {value!r}")
            self.robot.put(name, self.robot.topics[name][0], value)
            if name == "/Auto Selector/selected":
                self.robot.put("/Auto Selector/active", "string", value)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5810)
    ap.add_argument("--scenario", default="pit", choices=["pit", "pit-ok", "match", "sick"])
    a = ap.parse_args()
    robot = Robot(a.scenario)
    sessions = set()

    async def handler(ws):
        s = Session(robot, ws)
        sessions.add(s)
        print("client connected:", ws.request.path)
        try:
            async for msg in ws:
                if isinstance(msg, str):
                    await s.on_text(msg)
                else:
                    await s.on_binary(msg)
        except websockets.ConnectionClosed:
            pass
        finally:
            sessions.discard(s)
            print("client left")

    async def physics():
        while True:
            await asyncio.sleep(0.05)
            robot.tick()
            for s in list(sessions):
                try:
                    await s.announce_new()
                    await s.send_values()
                except websockets.ConnectionClosed:
                    pass

    async with serve(handler, a.host, a.port,
                     subprotocols=["v4.1.networktables.first.wpi.edu", "networktables.first.wpi.edu"]):
        print(f"fake Catalyst robot on ws://{a.host}:{a.port}/nt/ ({a.scenario})")
        await physics()


if __name__ == "__main__":
    asyncio.run(main())
