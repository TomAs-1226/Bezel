"""The presence beacon: the PC says over BLE that it is switched on and the Link is running.

This is a doorbell, not a wire. The beacon carries four bytes that identify this Link and nothing else
— no name, no address, no data — and the tablet never connects to it. When the tablet hears it, it talks
to the Link over HTTP on the LAN exactly as it does the rest of the time (docs/link-api.md,
"Presence"). The whole point is that the tablet can tell a PC that is asleep from a PC whose last packet
went missing, and can start polling the moment the PC wakes instead of a few seconds later.

The id comes out of the Link's own state directory next to the token, and `/link/status` hands it out
only to a request whose token was accepted, so learning the id is behind the same gate as everything
else. Rotating the token throws it away too, which keeps "rotate" meaning "every tablet pairs again"
for BLE as well as HTTP.

**Advertising, not scanning — which is the hard direction in Python.** Almost every Python BLE library
is a central: `bleak`, by far the most used, scans and connects and explicitly cannot advertise. So the
usual answer does not apply here. On Windows the way through is WinRT's
`BluetoothLEAdvertisementPublisher`, reached through the same pywinrt projections the media remote
already uses (media.py), which is why the dependency is `winrt-Windows.Devices.Bluetooth` rather than
anything new in kind. Manufacturer-specific data is the payload container because it is the advertising
section Windows lets an application fill; Windows manages the flags and service-UUID sections itself.

On macOS and Linux this module reports itself unavailable. `bless` (a peripheral-side companion to
bleak, CoreBluetooth and BlueZ) is the library to reach for if that is ever wanted; it was not worth a
dependency for a PC that is a Windows machine.

Unavailable is never fatal, and there is no second mechanism to fall back to on purpose: without the
beacon the tablet polls exactly as it did before any of this existed. If the wake-the-poller half is
ever wanted without Bluetooth at all, a UDP broadcast heartbeat on the LAN does that much and needs no
radio — but it cannot do the part that matters, because a PC that is asleep or off the network sends no
UDP either, which is precisely the case BLE is here to detect.
"""
from __future__ import annotations

import secrets
import sys
import threading
from pathlib import Path
from typing import Any

# The SIG's Assigned Numbers reserve company id 0xFFFF for internal and interoperability testing, which
# is the correct choice for a project with no company id of its own; borrowing a real vendor's would be
# worse than using the one meant for this. "CL" and a format byte follow, so a later beacon shape is
# distinguishable rather than silently misread, and the tablet matches the whole prefix — a stranger's
# 0xFFFF beacon is not mistaken for a Link.
COMPANY_ID = 0xFFFF
MAGIC = b"CL"
FORMAT = 1
ID_BYTES = 4


class Unavailable(Exception):
    """No beacon on this PC, with the reason to report; never fatal."""


def payload(beacon_id: bytes) -> bytes:
    """What goes in the manufacturer-data section, after the company id Windows writes for us."""
    return MAGIC + bytes([FORMAT]) + beacon_id


class WindowsBeacon:
    """BluetoothLEAdvertisementPublisher through pywinrt (or the older all-in-one winsdk)."""

    def __init__(self, beacon_id: bytes) -> None:
        if sys.platform != "win32":
            raise Unavailable("the presence beacon needs Windows (see presence.py on bless)")
        adv, streams = self._import()
        writer = streams.DataWriter()
        writer.write_bytes(payload(beacon_id))
        data = adv.BluetoothLEManufacturerData()
        data.company_id = COMPANY_ID
        data.data = writer.detach_buffer()
        self._adv = adv
        self._pub = adv.BluetoothLEAdvertisementPublisher()
        self._pub.advertisement.manufacturer_data.append(data)
        try:
            self._pub.start()
        except OSError as exc:
            # The usual causes: Bluetooth is switched off, or the adapter's driver has no peripheral
            # role. Both are the owner's to fix, and neither is the Link's business to work around.
            raise Unavailable(f"Windows wouldn't start the advertisement: {exc}") from None

    @staticmethod
    def _import() -> tuple[Any, Any]:
        import importlib

        for adv_name, st_name in (("winrt.windows.devices.bluetooth.advertisement", "winrt.windows.storage.streams"),
                                  ("winsdk.windows.devices.bluetooth.advertisement", "winsdk.windows.storage.streams")):
            try:
                return importlib.import_module(adv_name), importlib.import_module(st_name)
            except Exception:  # ImportError, or a projection that fails to load
                continue
        raise Unavailable("install the WinRT Bluetooth package: pip install winrt-Windows.Devices.Bluetooth")

    def state(self) -> str:
        """The publisher's own opinion, which is the only honest source: it can abort after a good start
        (the radio switched off, another app taking the adapter) and say so here rather than anywhere
        else. The names come from BluetoothLEAdvertisementPublisherStatus."""
        names = {0: "created", 1: "waiting", 2: "started", 3: "stopping", 4: "stopped", 5: "aborted"}
        try:
            status = self._pub.status
            return names.get(int(status), str(status))
        except Exception as exc:
            return f"unknown ({exc})"

    def stop(self) -> None:
        try:
            self._pub.stop()
        except Exception:
            pass


class Presence:
    """What the server holds. `beacon` None: make the real one, or record why there is none."""

    def __init__(self, state: Any, enabled: bool = True, beacon: Any | None = None) -> None:
        self.enabled = enabled
        self.reason = ""
        self.beacon: Any | None = None
        self._lock = threading.Lock()
        self.id = beacon_id(state.home) if enabled else b""
        if not enabled:
            self.reason = "the presence beacon is off on this Link (--no-presence)"
        elif beacon is not None:
            self.beacon = beacon
        else:
            try:
                self.beacon = WindowsBeacon(self.id)
            except Unavailable as exc:
                self.reason = str(exc)
            except Exception as exc:  # a broken projection must never stop the Link
                self.reason = f"the presence beacon couldn't start: {exc}"

    @property
    def available(self) -> bool:
        return self.beacon is not None

    @property
    def hex(self) -> str:
        """What /link/status hands an authenticated tablet. "" when there is no beacon to look for: a
        tablet given nothing simply never starts its radio, which is what every tablet does today."""
        return self.id.hex() if self.available else ""

    def report(self) -> dict[str, Any]:
        """For the console line and the desktop app's panel."""
        with self._lock:
            if not self.available:
                return {"available": False, "reason": self.reason}
            return {"available": True, "id": self.id.hex(), "state": self.beacon.state(),
                    "company_id": f"0x{COMPANY_ID:04x}"}

    def stop(self) -> None:
        with self._lock:
            if self.beacon is not None:
                self.beacon.stop()
                self.beacon = None


def beacon_id(home: Path) -> bytes:
    """Four random bytes, kept in ~/.catalyst-link/presence-id, made on first use.

    Random rather than derived from the token, so that reading the beacon off the air says nothing at
    all about the token; and stored rather than made each start, so a tablet that learned it does not
    have to learn it again after the Link restarts. `catalyst-link token --rotate` deletes it (state.py)
    and the next start makes a new one.

    It is a stable public identifier: anyone in the pit with a BLE scanner can see it and tell that this
    PC is the same PC as yesterday. For a mains-powered machine sitting on a table that is a fair trade
    for the tablet knowing whether it is awake, but it is a real property and not a hidden one.
    """
    path = home / "presence-id"
    try:
        raw = bytes.fromhex(path.read_text(encoding="utf-8").strip())
        if len(raw) == ID_BYTES:
            return raw
    except (OSError, ValueError):
        pass
    raw = secrets.token_bytes(ID_BYTES)
    from .state import write_atomic

    home.mkdir(parents=True, exist_ok=True)
    write_atomic(path, raw.hex() + "\n")
    return raw
