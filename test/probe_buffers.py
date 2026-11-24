"""A probe, not a test: what Windows' loopback takes from a non-blocking sender once the
receiver stops reading, and after the sender's buffer is raised. Prints figures only."""
import socket
import sys
import time

FRAME = bytes([0x10, 0x01, 0x40]) + bytes(320)


def pair(rcvbuf: int, sndbuf: int):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    c = socket.socket()
    if rcvbuf:
        c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    c.connect(srv.getsockname())
    s, _ = srv.accept()
    srv.close()
    if sndbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, sndbuf)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.setblocking(False)
    return s, c


def push(s, queue: bytearray) -> int:
    taken = 0
    while queue:
        try:
            n = s.send(queue)
        except BlockingIOError:
            break
        taken += n
        del queue[:n]
    return taken


def drain(c) -> str:
    got = 0
    c.settimeout(3)
    try:
        while True:
            part = c.recv(65536)
            if not part:
                return f"{got}"
            got += len(part)
    except OSError as error:
        return f"{got} then {type(error).__name__}"


def growth():
    s, c = pair(4096, 2048)
    queue, taken, t0 = bytearray(), 0, time.monotonic()
    marks = []
    for i in range(1, 151):
        queue += FRAME
        taken += push(s, queue)
        if i in (25, 50, 75, 100, 125, 150):
            marks.append(f"{i * 0.02:.1f}s {taken / 323:.0f}")
        time.sleep(0.02)
    print("share over time (4 KB / 2 KB):", ", ".join(marks), f"(sndbuf reads "
          f"{s.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)})")
    s.close()
    c.close()


def raised(name, rcvbuf, sndbuf, stall, room_of, wait_after_raise=0.0, then=0):
    s, c = pair(rcvbuf, sndbuf)
    queue, share = bytearray(), 0
    for _ in range(stall):
        queue += FRAME
        share += push(s, queue)
        time.sleep(0.02)
    waiting = len(queue)
    room = room_of(waiting)
    if room:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, room)
    if wait_after_raise:
        time.sleep(wait_after_raise)
    more = push(s, queue)
    queue += b"\x00\x00\x00"
    last = push(s, queue)
    for _ in range(then):              # more tries, 10 ms apart
        if not queue:
            break
        time.sleep(0.01)
        last += push(s, queue)
    left = len(queue)
    reads = s.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
    s.shutdown(socket.SHUT_WR)
    s.close()
    got = drain(c)
    c.close()
    print(f"{name:<34} share {share / 323:5.1f}  waiting {waiting / 323:5.1f}  raised to "
          f"{room or '-':>7} (reads {reads:>7})  took {more:>6}+{last:<4} left {left:>6}  "
          f"got {got} of {share + more + last + left}")


print(sys.version.split()[0], sys.getwindowsversion())
growth()
for stall in (80, 110):
    raised(f"{stall} frames, no raise", 4096, 2048, stall, lambda q: 0)
    raised(f"{stall} frames, queue + 2 KB", 4096, 2048, stall, lambda q: q + 2048)
    raised(f"{stall} frames, 1 MB", 4096, 2048, stall, lambda q: 1 << 20)
    raised(f"{stall} frames, 1 MB, 50 ms later", 4096, 2048, stall, lambda q: 1 << 20, 0.05)
    raised(f"{stall} frames, 1 MB, 10 tries", 4096, 2048, stall, lambda q: 1 << 20, 0, 10)
    raised(f"{stall} frames, default sndbuf, 1 MB", 4096, 0, stall, lambda q: 1 << 20)
