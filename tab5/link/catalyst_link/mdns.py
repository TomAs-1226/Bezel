"""Advertise the Link as _catalyst-link._tcp over mDNS, when the optional `zeroconf` package is installed."""
from __future__ import annotations

import socket
from typing import Callable

from . import __version__

SERVICE = "_catalyst-link._tcp.local."


def local_addresses(bind: str) -> list[str]:
    """IPv4 addresses the tablet can reach us on (non-loopback), or just the bound one."""
    if bind not in ("", "0.0.0.0"):
        return [bind]
    addrs: set[str] = set()
    try:
        import ifaddr  # comes with zeroconf

        for adapter in ifaddr.get_adapters():
            for ip in adapter.ips:
                if isinstance(ip.ip, str) and not ip.ip.startswith(("127.", "169.254.")):
                    addrs.add(ip.ip)
    except ImportError:
        pass
    if not addrs:
        try:  # the address of the default route; connect() on UDP sends nothing
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect(("10.255.255.255", 1))
                addrs.add(s.getsockname()[0])
        except OSError:
            pass
    return sorted(addrs)


def advertise(name: str, port: int, bind: str) -> Callable[[], None] | None:
    """Register the service; returns a function that unregisters it, or None without zeroconf."""
    try:
        from zeroconf import ServiceInfo, Zeroconf
    except ImportError:
        return None
    addrs = local_addresses(bind)
    if not addrs:
        return None
    label = "".join(c if c.isalnum() or c == "-" else "-" for c in name)[:60] or "catalyst-link"
    info = ServiceInfo(
        SERVICE,
        f"{label}.{SERVICE}",
        addresses=[socket.inet_aton(a) for a in addrs],
        port=port,
        properties={"name": name, "version": __version__, "path": "/link/status"},
        server=f"{label}.local.",
    )
    zc = Zeroconf()
    zc.register_service(info, allow_name_change=True)

    def stop() -> None:
        try:
            zc.unregister_service(info)
        finally:
            zc.close()

    return stop
