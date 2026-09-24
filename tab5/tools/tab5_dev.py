#!/usr/bin/env python3
"""Drive a Tab5 running Catalyst Tab over its USB-C console (components/tab_hal/src/hal_tab5_dev.c).

  python tools/tab5_dev.py COM9 shot out.png
  python tools/tab5_dev.py COM9 tap 640 360
  python tools/tab5_dev.py COM9 swipe 1000 360 200 360 250
  python tools/tab5_dev.py COM9 flip

Several commands can be chained with ';': "tap 640 660; sleep 0.5; shot a.png".
Needs pyserial and Pillow. Opening the port doesn't reset the tablet (DTR/RTS are held low)."""
import struct
import sys
import time

import serial
from PIL import Image


def open_port(name):
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 5
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def reply(s, timeout=20):
    """Lines until OK or ERR; SHOT lines bring their binary with them."""
    end = time.time() + timeout
    while time.time() < end:
        line = s.readline()
        if not line:
            continue
        text = line.decode(errors="replace").strip()
        if text.startswith("SHOT "):
            _, w, h, n = text.split()
            data = b""
            n = int(n)
            while len(data) < n:
                chunk = s.read(n - len(data))
                if not chunk:
                    raise SystemExit("shot: timed out after %d of %d bytes" % (len(data), n))
                data += chunk
            return ("shot", int(w), int(h), data)
        if text == "OK":
            return ("ok",)
        if text.startswith("ERR"):
            raise SystemExit(text)
    print("warning: no reply", file=sys.stderr)
    return ("none",)


def save(w, h, rle, path):
    px = bytearray(w * h * 3)
    o = 0
    for i in range(0, len(rle), 4):
        run, v = struct.unpack_from("<HH", rle, i)
        r = ((v >> 11) & 31) * 255 // 31
        g = ((v >> 5) & 63) * 255 // 63
        b = (v & 31) * 255 // 31
        px[o:o + run * 3] = bytes((r, g, b)) * run
        o += run * 3
    im = Image.frombytes("RGB", (w, h), bytes(px))
    if h > w:
        im = im.rotate(-90, expand=True)  # the panel's portrait buffer, turned the way the tablet is held
    im.save(path)


def main():
    s = open_port(sys.argv[1])
    for cmd in " ".join(sys.argv[2:]).split(";"):
        parts = cmd.split()
        if not parts:
            continue
        if parts[0] == "sleep":
            time.sleep(float(parts[1]))
            continue
        if parts[0] in ("shot", "pshot"):
            s.write((parts[0] + "\n").encode())
            r = reply(s)
            if r[0] != "shot":
                continue
            save(r[1], r[2], r[3], parts[1] if len(parts) > 1 else "shot.png")
            reply(s)
            print("saved", parts[1] if len(parts) > 1 else "shot.png")
            continue
        if parts[0] == "settime":
            # the PC's clock and zone (POSIX TZ; default Pacific, the team's)
            tz = parts[1] if len(parts) > 1 else "PST8PDT,M3.2.0,M11.1.0"
            parts = ["settime", str(int(time.time())), tz]
        s.write((" ".join(parts) + "\n").encode())
        reply(s)
        print("ok", " ".join(parts))


if __name__ == "__main__":
    main()
