"""A stand-in for the tablet: connects to a Link's /audio/stream, acks like the tablet does, and writes a WAV.

    python tools/audio_receiver.py http://127.0.0.1:8765 TOKEN out.wav [seconds]

It plays no sound. It reports packets, samples, the longest gap between packets, and the round trip the Link
measured from its acks. Used to test the PC half without a tablet."""
import socket, struct, sys, time, wave
from urllib.parse import urlsplit

PACKET = struct.Struct("<BBHII")
ACK = struct.Struct("<BBHIII")


def main() -> None:
    url, token, out = sys.argv[1], sys.argv[2], sys.argv[3]
    secs = float(sys.argv[4]) if len(sys.argv) > 4 else 10
    u = urlsplit(url)
    s = socket.create_connection((u.hostname, u.port or 80), timeout=5)
    s.sendall(f"GET /audio/stream HTTP/1.1\r\nHost: x\r\nX-Link-Token: {token}\r\n\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    head, buf = buf.split(b"\r\n\r\n", 1)
    print(head.decode().splitlines()[0])
    if b" 200 " not in head.split(b"\r\n")[0]:
        print(buf[:300].decode(errors="replace"))
        return
    pcm = bytearray()
    t0 = last = time.monotonic()
    gaps, packets, idle, lastseq = [], 0, 0, 0
    s.settimeout(1.0)
    while time.monotonic() - t0 < secs:
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d:
            print("closed by the link")
            break
        buf += d
        while len(buf) >= PACKET.size:
            t, flags, n, seq, tms = PACKET.unpack(buf[: PACKET.size])
            if len(buf) < PACKET.size + n * 2:
                break
            pcm += buf[PACKET.size : PACKET.size + n * 2]
            buf = buf[PACKET.size + n * 2 :]
            now = time.monotonic()
            gaps.append(now - last)
            last = now
            packets += 1
            idle += flags & 1
            lastseq = seq
            s.sendall(ACK.pack(3, 0, 120, seq, 0, int(now * 1000) & 0xFFFFFFFF))
    with wave.open(out, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000); w.writeframes(bytes(pcm))
    print(f"packets {packets} (idle {idle}), samples {len(pcm)//2} = {len(pcm)/96000:.2f} s, longest gap {max(gaps)*1000:.0f} ms")


main()
