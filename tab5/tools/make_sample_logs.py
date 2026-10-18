#!/usr/bin/env python3
"""Writes sample Driver Station logs for the simulator's microSD (sim_sd/logs/):

  match-q14.wpilog   a 2027 DS log: battery sagging under load with one brownout dip, packet time, CPU,
                     per-bus CAN utilisation, and a few console messages
  practice.dslog     an NI v4 dslog with CTRE-sized records: trip time, loss, battery, CPU, a brownout

    python tools/make_sample_logs.py [DIR]      (default: build/sim_sd/logs)
"""
import math
import os
import random
import struct
import sys

random.seed(5805)


def record(eid, ts_us, payload):
    return bytes([3 | 3 << 2 | 7 << 4]) + struct.pack("<IIQ", eid, len(payload), ts_us) + payload


def start(eid, name, typ):
    n, t = name.encode(), typ.encode()
    p = b"\x00" + struct.pack("<I", eid) + struct.pack("<I", len(n)) + n + struct.pack("<I", len(t)) + t + struct.pack("<I", 0)
    return record(0, 0, p)


def wpilog(path):
    out = bytearray(b"WPILOG" + struct.pack("<HI", 0x0100, 0))
    out += start(1, "DS:/Dscomm/Status/Battery", "double")
    out += start(2, "DS:/Dscomm/Status/PacketTime", "int64")
    out += start(3, "DS:/Dscomm/Status/CPU", "double")
    out += start(4, "DS:/Dscomm/Status/CanBusUtilizations", "float[]")
    out += start(5, "messages", "string")
    t0 = 1_000_000
    for i in range(150 * 50):  # 150 s at 50 Hz
        t = i / 50
        load = 0.0 if t < 3 else (0.4 + 0.5 * max(0.0, math.sin(t * 0.9)))
        v = 12.7 - 3.4 * load + random.uniform(-0.05, 0.05)
        if 88.0 < t < 88.4:
            v = 6.6 + random.uniform(0, 0.2)  # the brownout
        ts = t0 + i * 20000
        out += record(1, ts, struct.pack("<d", v))
        out += record(2, ts, struct.pack("<q", int(3500 + 1500 * load + random.uniform(0, 900))))
        out += record(3, ts, struct.pack("<d", 0.30 + 0.25 * load))
        out += record(4, ts, struct.pack("<5f", 44 + 20 * load, 0, 31 + 10 * load, 0, 0))
    for t, msg in [(0.5, "Robot program starting"), (3.0, "Enabled: autonomous"), (18.0, "Enabled: teleop"),
                   (88.1, "Brownout detected: 6.62 V"), (88.6, "Warning: CAN frames dropped on can_s2"),
                   (150.0, "Disabled")]:
        out += record(5, t0 + int(t * 1e6), msg.encode())
    open(path, "wb").write(out)


def dslog(path):
    out = bytearray(struct.pack(">i", 4) + struct.pack(">qQ", 3_900_000_000, 0))
    for i in range(120 * 50):
        t = i / 50
        load = 0.3 + 0.5 * max(0.0, math.sin(t * 0.7))
        v = 12.5 - 3.0 * load
        brown = 61.0 < t < 61.3
        if brown:
            v = 6.7
        rec = bytearray(35)
        rec[0] = int(min(255, (4 + 3 * load) * 2))
        rec[1] = 1 if random.random() < 0.1 else 0
        rec[2] = int(v)
        rec[3] = int((v - int(v)) * 256)
        rec[4] = int((30 + 30 * load) * 2)
        rec[5] = 0x7F if brown else 0xFF
        rec[6] = int((40 + 15 * load) * 2)
        out += rec
    open(path, "wb").write(out)


if __name__ == "__main__":
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "sim_sd", "logs")
    os.makedirs(d, exist_ok=True)
    wpilog(os.path.join(d, "match-q14.wpilog"))
    dslog(os.path.join(d, "practice.dslog"))
    print("wrote", d)
