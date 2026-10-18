#!/usr/bin/env python3
"""A pretend catalyst-agent: what FrcCatalyst's agent serves on a Systemcore, for the simulator.

    python tools/fake_agent.py [--host 0.0.0.0] [--port 9010]

The routes and JSON shapes are exactly catalyst_agent.py's (FrcCatalyst upgrade/alpha-7,
agent/overlay/usr/local/bin/catalyst-agent/catalyst_agent.py, agent 2.0.3): /api/system (the whole
snapshot), /api/health, /api/robot, /api/cameras, /api/motor-history and /api/motor-history.csv.
Every route is a GET, as on the robot. The machine it describes has a few things wrong with it, so
the tablet's systemcore and motors screens have something to say:

  - core 2 is pinned by the robot program, and the SoC throttled earlier this boot;
  - can_s2 is collecting transmit errors (the robot's /Catalyst/CAN/Health/can_s2/TEC climbs too);
  - the robot program has restarted three times, and its log holds the stack trace;
  - one Limelight is hot and one is not answering;
  - Shooter L has a lot of hot time, and the intake roller has been renumbered twice.

The agent binds every interface on the robot, and so does this by default: the tablet reaches the
agent at whatever address NetworkTables connected to (127.0.0.1 or 127.0.0.2 in the simulator).
Standard library only.
"""
import argparse
import http.server
import json
import math
import random
import socketserver
import time

T0 = time.time()
BOOT = T0 - 5400.0                      # up an hour and a half
PROGRAM_START = T0 - 212.0              # and the robot program came back 212 s ago


def now():
    return time.time() - T0


# ----------------------------------------------------------------------------------- identity

def identity():
    return {
        "hostname": "robot",
        "os": "Systemcore OS 2027.0.0-beta14",
        "osVersion": "2027.0.0",
        "kernel": "6.12.77-rt",
        "model": "Raspberry Pi Compute Module 5 Rev 1.0",
        "uptimeSeconds": round(time.time() - BOOT, 2),
        "agentVersion": "2.0.2",
    }


# ----------------------------------------------------------------------------------- cpu

def cpu():
    t = now()
    load = 0.5 + 0.5 * abs(math.sin(t * 0.3))
    cores = [
        {"core": 0, "percent": round(31 + 18 * load + random.uniform(-3, 3), 1), "mhz": 2400},
        {"core": 1, "percent": round(12 + 9 * load + random.uniform(-2, 2), 1), "mhz": 2400},
        # the robot program's main thread, stuck in a busy loop on one core
        {"core": 2, "percent": round(min(100.0, 95 + 4 * load + random.uniform(-1, 1)), 1), "mhz": 2400},
        {"core": 3, "percent": round(7 + 5 * load + random.uniform(-1, 1), 1), "mhz": 1500},
    ]
    return {
        "cores": cores,
        "loadAverage": [round(2.1 + 0.3 * load, 2), 1.84, 1.52],
        "model": "Cortex-A76",
        "throttling": {
            "underVoltageNow": False, "frequencyCappedNow": False, "throttledNow": False,
            "softTempLimitNow": False, "throttledSinceBoot": True, "underVoltageSinceBoot": False,
        },
    }


def thermal():
    t = now()
    return [
        {"zone": "cpu-thermal", "celsius": round(76.5 + 2.5 * math.sin(t * 0.05), 1)},
        {"zone": "rp1_adc", "celsius": 48.2},
    ]


# ----------------------------------------------------------------------------------- memory, storage

def memory():
    total = 4246257664
    available = 3280000000 - int(4e6 * math.sin(now() * 0.1))
    return {
        "totalBytes": total,
        "availableBytes": available,
        "usedBytes": total - available,
        "cachedBytes": 1650000000,
        "swapTotalBytes": 0,
        "swapFreeBytes": 0,
    }


def storage():
    return {
        "mounts": [
            {"mount": "/", "device": "/dev/mmcblk0p2", "filesystem": "ext4",
             "totalBytes": 7326429184, "usedBytes": 6405000000, "freeBytes": 921429184},
            {"mount": "/boot/firmware", "device": "/dev/mmcblk0p1", "filesystem": "vfat",
             "totalBytes": 535805952, "usedBytes": 71000000, "freeBytes": 464805952},
            {"mount": "/U", "device": "/dev/sda1", "filesystem": "vfat",
             "totalBytes": 31914983424, "usedBytes": 12800000000, "freeBytes": 19114983424},
        ],
        "directories": [
            {"path": "/home/systemcore", "bytes": 4120000000},
            {"path": "/var/log", "bytes": 1380000000},
            {"path": "/U", "bytes": 12800000000},
        ],
    }


# ----------------------------------------------------------------------------------- processes

def processes():
    t = now()
    rows = [
        {"pid": 1873, "name": "java", "cpuPercent": round(97 + random.uniform(-2, 2), 1), "rssBytes": 612000000},
        {"pid": 431, "name": "MrcCommDaemon", "cpuPercent": round(6.2 + random.uniform(-1, 1), 1), "rssBytes": 48000000},
        {"pid": 502, "name": "vision-aggregator", "cpuPercent": 4.1, "rssBytes": 96000000},
        {"pid": 377, "name": "systemcore-web", "cpuPercent": 2.3, "rssBytes": 71000000},
        {"pid": 612, "name": "ntcore-server", "cpuPercent": round(1.8 + 0.4 * math.sin(t), 1), "rssBytes": 39000000},
        {"pid": 1990, "name": "python3", "cpuPercent": 0.6, "rssBytes": 21000000},
        {"pid": 244, "name": "systemd-journal", "cpuPercent": 0.4, "rssBytes": 18000000},
        {"pid": 1, "name": "systemd", "cpuPercent": 0.1, "rssBytes": 12000000},
    ]
    return {
        "count": 162,
        "topByCpu": sorted(rows, key=lambda r: r["cpuPercent"], reverse=True),
        "topByMemory": sorted(rows, key=lambda r: r["rssBytes"], reverse=True),
    }


# ----------------------------------------------------------------------------------- can

def can_interfaces():
    t = now()
    out = []
    for i, (rate, errs) in enumerate([(2900, 0), (0, 0), (1850, 1), (0, 0), (0, 0)]):
        frames = int(rate * (time.time() - BOOT))
        tx_err = int(3 * t) + 14 if errs else 0          # can_s2: a marginal connector, errors climbing
        out.append({
            "name": f"can_s{i}",
            "up": True,
            "state": "ERROR-WARNING" if errs and tx_err > 96 else "ERROR-ACTIVE",
            "bitrate": 1000000,
            "restarts": 1 if errs else 0,
            "rxPackets": frames * 2 // 3,
            "txPackets": frames // 3,
            "rxErrors": 0,
            "txErrors": tx_err,
            "rxDropped": 0,
            "txDropped": 2 if errs else 0,
        })
    return out


# ----------------------------------------------------------------------------------- network

def network():
    return [
        {"name": "end0", "up": True, "mac": "2c:cf:67:5a:10:02", "speedMbps": 1000,
         "addresses": ["10.58.5.2/24"], "wireless": None},
        {"name": "usb0", "up": True, "mac": "02:5c:0e:00:00:02", "speedMbps": None,
         "addresses": ["172.26.0.1/24"], "wireless": None},
        {"name": "wlan0", "up": True, "mac": "2c:cf:67:5a:10:03", "speedMbps": None,
         "addresses": ["172.30.0.1/24"], "wireless": {"linkQuality": 58.0, "signalDbm": -61.0}},
        {"name": "eth1", "up": False, "mac": "2c:cf:67:5a:10:04", "speedMbps": -1,
         "addresses": [], "wireless": None},
    ]


# ----------------------------------------------------------------------------------- robot program

LOG = [
    "2027-03-14T10:19:41+0000 robot[1622]: Exception in thread \"main\" java.lang.NullPointerException",
    "2027-03-14T10:19:41+0000 robot[1622]:   at frc.robot.subsystems.Shooter.periodic(Shooter.java:212)",
    "2027-03-14T10:19:41+0000 robot[1622]:   at org.wpilib.command3.Scheduler.run(Scheduler.java:410)",
    "2027-03-14T10:19:41+0000 systemd[1]: robot.service: Main process exited, code=exited, status=1/FAILURE",
    "2027-03-14T10:19:42+0000 systemd[1]: robot.service: Scheduled restart job, restart counter is at 3.",
    "2027-03-14T10:19:43+0000 robot[1873]: ********** Robot program starting **********",
    "2027-03-14T10:19:44+0000 robot[1873]: Catalyst 2.0.0-beta.2 (systemcore)",
    "2027-03-14T10:19:44+0000 robot[1873]: CANRegistry: 13 devices on can_s0, 6 on can_s2",
    "2027-03-14T10:19:45+0000 robot[1873]: MotorHistory: 12 devices on record, 10 motors",
    "2027-03-14T10:19:45+0000 robot[1873]: Warning: Loop time of 0.02s overrun",
    "2027-03-14T10:19:46+0000 robot[1873]: Robot code ready",
]


def robot_program():
    return {
        "unit": "robot.service",
        "state": "active",
        "subState": "running",
        "restarts": 3,
        "runningForSeconds": round(time.time() - PROGRAM_START, 1),
        "memoryBytes": 612000000,
        "pid": 1873,
        "log": LOG,
    }


# ----------------------------------------------------------------------------------- cameras

def cameras():
    t = now()
    return {
        "available": True,
        "cameras": [
            {"name": "limelight-ground", "host": "limelight-ground", "ip": "10.58.5.11", "type": "limelight4",
             "interface": "eth", "ntConnected": True, "ntName": "limelight-ground",
             "aliasIps": ["172.30.0.220", "172.26.0.220"], "uiUrl": "http://172.30.0.220:5801/",
             "streamUrl": "http://172.30.0.220:5800/", "fps": round(56.6 + math.sin(t), 1),
             "temperatureC": round(81.4 + math.sin(t * 0.07), 1), "cpuPercent": 75.0, "ramPercent": 63.2,
             "pipelineType": "pipe_fiducial", "pipelineIndex": 0, "statusReachable": True},
            {"name": "limelight-rear", "host": "limelight-rear", "ip": "10.58.5.12", "type": None,
             "interface": "eth", "ntConnected": False, "ntName": None, "aliasIps": ["172.26.0.221"],
             "uiUrl": None, "streamUrl": None, "fps": None, "temperatureC": None, "cpuPercent": None,
             "ramPercent": None, "pipelineType": None, "pipelineIndex": None, "statusReachable": False},
        ],
        "sampledAt": time.time(),
    }


# ----------------------------------------------------------------------------------- motor history

DAY = 86400e3


def _motor(serial, model, kind, idents, powered_h, running_h, loaded_h, revs, peak_a, peak_c, hot_s,
           boots, sticky=0, sessions=12, hot_sessions=False):
    ms = time.time() * 1000
    first = ms - DAY * 60
    ids = []
    span = (ms - first) / max(1, len(idents))
    for k, (i, name, bus, fw) in enumerate(idents):
        ids.append({"id": i, "name": name, "bus": bus, "firmware": fw,
                    "firstSeenMs": int(first + k * span), "lastSeenMs": int(first + (k + 1) * span - DAY)})
    ids[-1]["lastSeenMs"] = int(ms)
    rng = random.Random(serial)
    sess = []
    for s in range(min(sessions, boots)):
        start = ms - (min(sessions, boots) - s) * DAY * 0.8
        secs = rng.uniform(600, 3600)
        run = secs * rng.uniform(0.15, 0.4) if kind == "motor" else 0
        hot = rng.uniform(20, 240) if hot_sessions and s % 3 != 1 else 0
        sess.append({"startMs": int(start), "seconds": round(secs, 1), "runningSeconds": round(run, 1),
                     "revolutions": round(run * rng.uniform(20, 80), 1),
                     "peakAmps": round(rng.uniform(20, peak_a), 1) if kind == "motor" else 0,
                     "peakTempC": round(rng.uniform(35, peak_c), 1) if kind == "motor" else 0,
                     "hotSeconds": round(hot, 1)})
    return {
        "serial": serial, "model": model, "kind": kind, "hardwareRev": "1.4", "manufactured": "2025-11",
        "firstSeenMs": int(first), "lastSeenMs": int(ms), "boots": boots,
        "totals": {"poweredSeconds": round(powered_h * 3600, 1), "runningSeconds": round(running_h * 3600, 1),
                   "loadedSeconds": round(loaded_h * 3600, 1), "revolutions": round(revs, 1),
                   "energyJoules": round(running_h * 3600 * 180, 1), "peakStatorAmps": peak_a,
                   "peakTempC": peak_c, "hotSeconds": hot_s, "stickyFaults": sticky},
        "identities": ids, "sessions": sess,
    }


def motor_history_doc():
    fw = "26.1.1.1"
    s = "000E0B500C776800000A00011A00"
    devices = []
    swerve = [("FL drive", 1, 14.2, 3.9, 2.1, 612000), ("FL steer", 2, 14.2, 2.2, 0.6, 88000),
              ("FR drive", 3, 14.0, 3.8, 2.2, 598000), ("FR steer", 4, 14.0, 2.1, 0.7, 91000),
              ("BL drive", 5, 13.1, 3.6, 1.9, 571000), ("BL steer", 6, 13.1, 2.0, 0.5, 84000),
              ("BR drive", 7, 13.1, 3.7, 2.0, 580000), ("BR steer", 8, 13.1, 2.3, 0.8, 97000)]
    for k, (name, cid, ph, rh, lh, revs) in enumerate(swerve):
        devices.append(_motor(s + "%04X" % (0xE0 + k), "Talon FX", "motor", [(cid, name, "can_s0", fw)],
                              ph, rh, lh, revs, 96.0 + 7 * (k % 2 == 0), 52.0 + 2 * k, 0, 64 + k))
    # the shooter flywheel: working hard in a closed box, and it shows
    devices.append(_motor(s + "00F1", "Kraken X60", "motor", [(25, "Shooter L", "can_s2", "26.1.0.0"),
                                                               (25, "Shooter L", "can_s2", fw)],
                          9.8, 4.1, 3.6, 1410000, 118.0, 88.4, 2710.0, 51, sticky=0x40, sessions=24,
                          hot_sessions=True))
    devices.append(_motor(s + "00F2", "Kraken X60", "motor", [(26, "Shooter R", "can_s2", fw)],
                          9.8, 4.1, 3.5, 1402000, 112.0, 71.2, 95.0, 51, sessions=24))
    # a motor that has lived three lives: a back drive, a spare, and now the intake roller
    devices.append(_motor(s + "00F3", "Talon FX", "motor", [(45, "BL drive", "can_s0", "25.3.0.0"),
                                                             (45, "Intake Roller", "can_s2", "26.1.0.0"),
                                                             (28, "Intake", "can_s2", fw)],
                          21.5, 6.2, 2.8, 1054000, 131.0, 64.0, 0.0, 118, sessions=30))
    devices.append(_motor(s + "00F4", "Talon FX", "motor", [(29, "Arm", "can_s2", fw)],
                          9.1, 0.9, 2.4, 3100, 74.0, 58.5, 0.0, 49))
    devices.append({"serial": "0A1B2C3D", "model": "CANcoder", "kind": "encoder", "hardwareRev": "", "manufactured": "",
                    "firstSeenMs": int(time.time() * 1000 - DAY * 60), "lastSeenMs": int(time.time() * 1000),
                    "boots": 64, "totals": {"poweredSeconds": 51000.0},
                    "identities": [{"id": 21, "name": "FL azimuth", "bus": "can_s0", "firmware": "26.1.0.0",
                                    "firstSeenMs": 0, "lastSeenMs": 0}], "sessions": []})
    devices.append({"serial": "0A1B2C40", "model": "Pigeon 2", "kind": "imu", "hardwareRev": "", "manufactured": "",
                    "firstSeenMs": int(time.time() * 1000 - DAY * 60), "lastSeenMs": int(time.time() * 1000),
                    "boots": 64, "totals": {"poweredSeconds": 51000.0},
                    "identities": [{"id": 20, "name": "Gyro", "bus": "can_s0", "firmware": "26.1.0.0",
                                    "firstSeenMs": 0, "lastSeenMs": 0}], "sessions": []})
    return {"format": "catalyst-motor-history", "version": 1, "updatedMs": int(time.time() * 1000) - 14000,
            "clockTrusted": True, "devices": devices,
            "path": "/home/systemcore/catalyst/motor-history.json", "fileModifiedAt": time.time() - 14}


COLUMNS = ("serial", "model", "kind", "bus", "id", "name", "firmware", "poweredSeconds", "runningSeconds",
           "loadedSeconds", "revolutions", "peakStatorAmps", "peakTempC", "hotSeconds", "energyJoules", "boots",
           "firstSeenMs", "lastSeenMs", "identities", "stickyFaults")


def device_row(d):
    identities = d.get("identities") or []
    latest = identities[-1] if identities else {}
    totals = d.get("totals") or {}
    return {
        "serial": d.get("serial", ""), "model": d.get("model", ""), "kind": d.get("kind", ""),
        "bus": latest.get("bus", ""), "id": latest.get("id", ""), "name": latest.get("name", ""),
        "firmware": latest.get("firmware", ""),
        "poweredSeconds": totals.get("poweredSeconds", 0), "runningSeconds": totals.get("runningSeconds", 0),
        "loadedSeconds": totals.get("loadedSeconds", 0), "revolutions": totals.get("revolutions", 0),
        "peakStatorAmps": totals.get("peakStatorAmps", 0), "peakTempC": totals.get("peakTempC", 0),
        "hotSeconds": totals.get("hotSeconds", 0), "energyJoules": totals.get("energyJoules", 0),
        "boots": d.get("boots", 0), "firstSeenMs": d.get("firstSeenMs", 0), "lastSeenMs": d.get("lastSeenMs", 0),
        "identities": len(identities), "stickyFaults": totals.get("stickyFaults", 0),
    }


def motor_history_csv(doc):
    def cell(v):
        s = "" if v is None else str(v)
        return '"' + s.replace('"', '""') + '"' if any(c in s for c in ',"\n') else s
    lines = [",".join(COLUMNS)]
    for d in doc.get("devices") or []:
        row = device_row(d)
        lines.append(",".join(cell(row[c]) for c in COLUMNS))
    return "\n".join(lines) + "\n"


def motor_history_summary():
    doc = motor_history_doc()
    return {"present": True, "updatedMs": doc["updatedMs"], "clockTrusted": doc["clockTrusted"],
            "devices": [device_row(d) for d in doc["devices"]]}


def snapshot():
    time.sleep(0.1)                      # the real agent's two /proc/stat reads, 100 ms apart
    return {
        "identity": identity(),
        "cpu": cpu(),
        "thermal": thermal(),
        "memory": memory(),
        "storage": storage(),
        "processes": processes(),
        "can": can_interfaces(),
        "network": network(),
        "robotProgram": robot_program(),
        "cameras": cameras(),
        "motorHistory": motor_history_summary(),
        "sampledAt": time.time(),
    }


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send_text(self, text, content_type, status=200, filename=None):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        if filename:
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % filename)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _send(self, payload, status=200):
        self._send_text(json.dumps(payload), "application/json", status)

    def do_GET(self):
        route = self.path.split("?")[0].rstrip("/") or "/"
        if route in ("/", "/api", "/api/system"):
            self._send(snapshot())
        elif route == "/api/health":
            self._send({"ok": True, "agent": "catalyst-agent", "version": "2.0.3"})
        elif route == "/api/robot":
            self._send(robot_program())
        elif route == "/api/cameras":
            self._send(cameras())
        elif route == "/api/motor-history":
            self._send(motor_history_doc())
        elif route == "/api/motor-history.csv":
            self._send_text(motor_history_csv(motor_history_doc()), "text/csv; charset=utf-8",
                            filename="motor-history.csv")
        else:
            self._send({"error": "not found", "path": route}, status=404)

    def log_message(self, fmt, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9010)
    a = ap.parse_args()
    server = Server((a.host, a.port), Handler)
    print(f"fake catalyst-agent on http://{a.host}:{a.port}/api/system")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
