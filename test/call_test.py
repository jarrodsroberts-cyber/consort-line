"""Usage: python -I test/call_test.py LINE_EXE CALLER_EXE

Whole calls on 127.0.0.1, with this script as Consort:

- the line (LINE_EXE, consort-line): this script holds its standard input and output (the
  secret, the settings, then commands and events) and each answered call's connection.
- a caller (CALLER_EXE, the tests' far end, never released): plays a tone (aufile), records
  what it hears, and is driven over ctrl_tcp bound to 127.0.0.1.
- an echo (the same far end with echo and aubridge): answers and sends its audio back, so a
  round trip through the line is timed on one clock.

Standard library only. Exits 0 when every check passes, 1 when one fails."""

from __future__ import annotations

import array
import hashlib
import hmac
import json
import math
import queue
import secrets
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

RATE, SAMPLES = 8000, 160
HANGUP, ID, DTMF, AUDIO = 0x00, 0x01, 0x03, 0x10
FLUSH, FLUSHED, MARK, ERROR = 0x20, 0x21, 0x22, 0xFF
SETTINGS = {"listen": "127.0.0.1:0", "account": "sip:line@127.0.0.1", "calls": 4}
FAILED: list[str] = []


def check(ok: bool, what: str) -> bool:
    print(("  ok    " if ok else "  FAIL  ") + what, flush=True)
    if not ok:
        FAILED.append(what)
    return ok


# -- audio ----------------------------------------------------------------------------------

def tone(seconds: float, hz: float, dbfs_level: float) -> array.array:
    peak = 32767 * 10 ** (dbfs_level / 20) * math.sqrt(2)
    return array.array("h", (round(peak * math.sin(2 * math.pi * hz * i / RATE))
                             for i in range(round(seconds * RATE))))


def write_wav(path: Path, samples: array.array) -> None:
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(samples.tobytes())


def power_at(samples, hz: float) -> float:
    """Goertzel: the share of a block's energy at one frequency (0 to about 1)."""
    k = 2 * math.cos(2 * math.pi * hz / RATE)
    s1 = s2 = 0.0
    for x in samples:
        s1, s2 = x + k * s1 - s2, s1
    energy = sum(x * x for x in samples) or 1.0
    return (s1 * s1 + s2 * s2 - k * s1 * s2) * 2 / (len(samples) * energy)


def dbfs(samples) -> float:
    if not len(samples):
        return -120.0
    rms = math.sqrt(sum(x * x for x in samples) / len(samples))
    return 20 * math.log10(rms / 32767) if rms else -120.0


# -- processes -------------------------------------------------------------------------------

def free_port(kind: int) -> int:
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class FarEnd:
    """The tests' far end, from a config file and an account (params: more of its account's
    parameters, such as ";ptime=30")."""

    def __init__(self, exe: str, folder: Path, sip_port: int, account: str, lines: list[str],
                 params: str = ""):
        folder.mkdir(parents=True)
        config = folder / "config"
        config.write_text("\n".join([
            f"sip_listen\t\t127.0.0.1:{sip_port}",
            "sip_transports\t\tudp",
            "net_interface\t\t127.0.0.1",
            "ausrc_srate\t\t8000", "auplay_srate\t\t8000",
            "ausrc_channels\t\t1", "auplay_channels\t\t1",
            # With no menu loaded, baresip makes no call of an INVITE unless told to
            "call_accept\t\tyes",
            "module\t\t\tg711.so",
            *lines]) + "\n", encoding="utf-8")
        self.log = open(folder / "stderr.txt", "wb")
        self.proc = subprocess.Popen(
            [exe, str(config),
             f"<{account}>;regint=0;answermode=manual;audio_codecs=PCMU{params}"],
            cwd=folder, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=self.log)

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(10)
        self.log.close()


class Line:
    """consort-line, as Consort holds it: its standard input and output are the channel.
    close_after: Consort's end of standard output closes once that event has come (its
    standard input stays open)."""

    def __init__(self, exe: str, folder: Path, first: list[bytes] | None = None,
                 close_after: str | None = None):
        folder.mkdir(parents=True, exist_ok=True)
        self.secret = secrets.token_bytes(32)
        self.events: queue.Queue = queue.Queue()
        self.raw: list[bytes] = []
        self.ids = 0
        self.close_after = close_after
        self.log = open(folder / "stderr.txt", "wb")
        self.proc = subprocess.Popen([exe], cwd=folder, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.log)
        if first is None:
            first = [json.dumps({"line": 1, "secret": self.secret.hex()}).encode(),
                     json.dumps(SETTINGS).encode()]
        for line in first:
            self.proc.stdin.write(line + b"\n")
        self.proc.stdin.flush()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for raw in self.proc.stdout:
            text = raw.rstrip(b"\r\n")
            self.raw.append(text)
            try:
                event = json.loads(text)
            except ValueError:
                event = {"event": "not json", "line": text.decode("utf-8", "replace")}
            self.events.put(event)
            if self.close_after and event.get("event") == self.close_after:
                self.proc.stdout.close()     # on this thread, the only one reading it
                break
        self.events.put({"event": "eof"})

    def wait(self, kinds: set[str], within: float = 10.0) -> dict:
        ends = time.monotonic() + within
        while True:
            left = ends - time.monotonic()
            if left <= 0:
                return {"event": "timeout", "wanted": sorted(kinds)}
            try:
                event = self.events.get(timeout=left)
            except queue.Empty:
                continue
            if event.get("event") in kinds | {"eof", "failed", "not json"}:
                return event

    def send(self, **command) -> dict:
        self.ids += 1
        self.proc.stdin.write(json.dumps({"id": self.ids, **command}).encode() + b"\n")
        self.proc.stdin.flush()
        while True:
            event = self.wait({"reply"})
            if event.get("event") != "reply" or event.get("id") == self.ids:
                return event

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(10)
        self.log.close()

    def exit_code(self, within: float = 15.0) -> int | None:
        try:
            return self.proc.wait(within)
        except subprocess.TimeoutExpired:
            return None


def proof(secret: bytes, nonce: bytes, side: bytes, port: int) -> bytes:
    return hmac.new(secret, side + nonce + port.to_bytes(2, "big"), hashlib.sha256).digest()[:16]


def frame(kind: int, payload: bytes = b"") -> bytes:
    return bytes([kind]) + len(payload).to_bytes(2, "big") + payload


class Conn:
    """A call's connection, as Consort makes it: proves itself, checks the line's proof, and
    reads every frame on a thread of its own, with the moment it came."""

    def __init__(self, secret: bytes, answered: dict):
        port, nonce = answered["port"], bytes.fromhex(answered["nonce"])
        # As PROTOCOL.md asks: a receive buffer of 4,096 bytes, set before connecting
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        self.sock.settimeout(5)
        self.sock.connect(("127.0.0.1", port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.sendall(frame(ID, proof(secret, nonce, b"consort", port)))
        back = self._exactly(19)
        self.proved = back == frame(ID, proof(secret, nonce, b"line", port))
        self.frames: queue.Queue = queue.Queue()
        self.sock.settimeout(None)
        threading.Thread(target=self._read, daemon=True).start()

    def _exactly(self, n: int) -> bytes:
        data = b""
        while len(data) < n:
            part = self.sock.recv(n - len(data))
            if not part:
                break
            data += part
        return data

    def _read(self) -> None:
        try:
            while True:
                header = self._exactly(3)
                if len(header) < 3:
                    break
                payload = self._exactly(int.from_bytes(header[1:], "big"))
                self.frames.put((time.perf_counter(), header[0], payload))
        except OSError:
            pass
        self.frames.put((time.perf_counter(), None, b""))

    def next(self, kinds: set[int], within: float = 5.0):
        """The next frame of these kinds (AUDIO skipped unless asked for), or the close."""
        ends = time.monotonic() + within
        while True:
            left = ends - time.monotonic()
            if left <= 0:
                return None
            try:
                at, kind, payload = self.frames.get(timeout=left)
            except queue.Empty:
                return None
            if kind in kinds or kind is None:
                return at, kind, payload

    def send(self, kind: int, payload: bytes = b"") -> float:
        self.sock.sendall(frame(kind, payload))
        return time.perf_counter()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class Ctrl:
    """The far end's ctrl_tcp, as netstrings of JSON."""

    def __init__(self, port: int):
        ends = time.monotonic() + 10
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.monotonic() > ends:
                    raise
                time.sleep(0.1)

    def do(self, command: str, params: str = "") -> None:
        body = json.dumps({"command": command, "params": params, "token": command}).encode()
        self.sock.sendall(str(len(body)).encode() + b":" + body + b",")


def hello_of(line: Line) -> dict:
    hello = line.wait({"hello"})
    check(hello.get("event") == "hello", f"the line says hello ({hello})")
    check(bool(line.raw) and line.raw[0].startswith(b'{"event":"hello"'),
          "hello is the first byte on standard output")
    return hello


# -- the tests -------------------------------------------------------------------------------

def a_call_that_rings_here(line_exe: str, caller_exe: str, root: Path, ptime: int = 20) -> None:
    print(f"A call that rings here (the caller's packets {ptime} ms), answered by Consort, "
          "audio both ways, a key, a hang-up")
    caller_wav, heard_wav = root / "caller.wav", root / "heard.wav"
    write_wav(caller_wav, tone(4.0, 440, -20) + array.array("h", bytes(2 * RATE * 6)))
    caller_sip, ctrl_port = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)

    line = Line(line_exe, root / "line")
    caller = None
    try:
        hello = hello_of(line)
        check(hello.get("version") == "0.1.0" and hello.get("baresip") == "4.13.0",
              "hello names the add-on's and baresip's versions")
        check(hello.get("sip", "").startswith("127.0.0.1:"), "SIP is on 127.0.0.1 alone")
        line_sip = int(hello["sip"].rsplit(":", 1)[1])

        # Its offer asks for a=ptime:<ptime>, which the line's sender then keeps to
        caller = FarEnd(caller_exe, root / "caller", caller_sip, "sip:caller@127.0.0.1", [
            f"audio_source\t\taufile,{caller_wav}", f"audio_player\t\taufile,{heard_wav}",
            "module\t\t\taufile.so", "module_app\t\tmenu.so", "module_app\t\tctrl_tcp.so",
            f"ctrl_tcp_listen\t\t127.0.0.1:{ctrl_port}"], params=f";ptime={ptime}")
        ctrl = Ctrl(ctrl_port)
        ctrl.do("dial", f"sip:line@127.0.0.1:{line_sip}")

        incoming = line.wait({"incoming"})
        check(incoming.get("event") == "incoming", f"the call rings ({incoming.get('event')})")
        call = incoming.get("call", "")
        check(len(call) == 16 and all(c in "0123456789abcdef" for c in call),
              "the call's id is the add-on's own: 16 hex digits")
        check("caller@127.0.0.1" in incoming.get("from", ""), "incoming names who calls")
        check(line.wait({"answered"}, within=1.0).get("event") == "timeout",
              "nothing is answered until Consort says")

        reply = line.send(do="accept", call=call)
        check(reply.get("ok") is True, f"Consort accepts it ({reply})")
        answered = line.wait({"answered"})
        check(answered.get("event") == "answered" and answered.get("call") == call,
              "answered names the call, its port and nonce")
        check(len(answered.get("nonce", "")) == 32, "the nonce is 16 bytes")

        conn = Conn(line.secret, answered)
        check(conn.proved, "both ends prove themselves")

        # The caller's audio: 440 Hz, a frame every 20 ms
        audio: list = []
        start = time.perf_counter()
        while time.perf_counter() - start < 2.0:
            got = conn.next({AUDIO}, within=0.5)
            if not got or got[1] is None:
                break
            audio.append((got[0], got[2]))
        check(90 <= len(audio) <= 110, f"a frame every 20 ms: {len(audio)} in 2 s")
        check(all(len(payload) == 320 for _, payload in audio), "every AUDIO is 320 bytes")
        heard = [array.array("h", p) for _, p in audio[-50:]]
        tonal = sum(1 for s in heard if dbfs(s) > -30 and power_at(s, 440) > 0.5)
        check(tonal >= 40, f"the caller's 440 Hz comes through ({tonal} of 50 frames)")

        # Consort's audio: 1 kHz for a second, paced, then a MARK; then a FLUSH mid-sentence
        voice = tone(1.0, 1000, -16)
        lead = time.perf_counter()
        for i in range(50):
            while time.perf_counter() < lead + i * 0.02 - 0.1:
                time.sleep(0.002)
            conn.send(AUDIO, voice[i * SAMPLES:(i + 1) * SAMPLES].tobytes())
        conn.send(MARK, (1).to_bytes(4, "big"))
        marked = conn.next({MARK}, within=3.0)
        check(bool(marked) and marked[2] == (1).to_bytes(4, "big"),
              "MARK 1 comes back once its sentence has played")
        # The MARK says the line handed the tone to baresip: give it time to reach the caller
        time.sleep(1.5)
        for _ in range(25):
            conn.send(AUDIO, voice[:SAMPLES].tobytes())
        flushed_at = conn.send(FLUSH)
        flushed = conn.next({FLUSHED}, within=2.0)
        check(bool(flushed) and len(flushed[2]) == 8, "FLUSH is answered with FLUSHED")
        if flushed and len(flushed[2]) == 8:
            mark, count = struct.unpack(">II", flushed[2])
            check(mark == 1 and count % SAMPLES == 0 and count <= 25 * SAMPLES,
                  f"FLUSHED: MARK 1, {count} samples since (whole frames, no more than sent)")
            check(flushed[0] - flushed_at < 0.5, "FLUSHED within half a second")

        # A key from the caller
        ctrl.do("sndcode", "5")
        key = conn.next({DTMF}, within=5.0)
        check(bool(key) and key[2] == b"5", "the caller's key comes as one DTMF frame")

        # The caller hangs up
        ctrl.do("hangup")
        gone = conn.next({HANGUP}, within=5.0)
        check(bool(gone) and gone[1] == HANGUP, "HANGUP when the far end hangs up")
        check(conn.next(set(), within=3.0) is not None, "then the connection closes")
        closed = line.wait({"closed"})
        check(closed.get("call") == call and isinstance(closed.get("figures"), dict),
              f"closed, with the link's figures ({closed.get('figures')})")
        conn.close()
    finally:
        line.stop()
        if caller:
            caller.stop()

    if heard_wav.exists():
        samples = wav_data(heard_wav)
        blocks = [samples[i:i + SAMPLES] for i in range(0, len(samples) - SAMPLES, SAMPLES)]
        tonal = sum(1 for b in blocks if dbfs(b) > -30 and power_at(b, 1000) > 0.5)
        loudest = max((dbfs(b) for b in blocks), default=-120.0)
        print(f"  the caller's recording: {len(samples) / RATE:.1f} s, "
              f"loudest 20 ms at {loudest:.0f} dBFS")
        check(tonal >= 40, f"the caller heard Consort's 1 kHz ({tonal} frames)")
    else:
        check(False, "the caller's recording was written")


def a_call_at_30_ms(line_exe: str, caller_exe: str, root: Path) -> None:
    """A far end that asks for 30 ms packets still hears the line (its source takes any
    ptime), and the line's frames to Consort stay 20 ms."""
    a_call_that_rings_here(line_exe, caller_exe, root, ptime=30)


def wav_data(path: Path) -> array.array:
    """A WAV's samples from its data chunk, whatever its header says of the length (a far end
    that is killed may leave the header unfinished)."""
    raw = path.read_bytes()
    at = raw.find(b"data")
    if at < 0:
        return array.array("h")
    body = raw[at + 8:]
    return array.array("h", body[:len(body) // 2 * 2])


def a_call_placed_to_an_echo(line_exe: str, caller_exe: str, root: Path) -> None:
    print("A call Consort places, to an echo: the round trip through the line")
    echo_sip = free_port(socket.SOCK_DGRAM)
    echo = FarEnd(caller_exe, root / "echo", echo_sip, "sip:echo@127.0.0.1", [
        "audio_source\t\taubridge,echo", "audio_player\t\taubridge,echo",
        "module\t\t\taubridge.so", "module_app\t\techo.so"])
    line = Line(line_exe, root / "line")
    try:
        hello_of(line)
        time.sleep(0.5)
        reply = line.send(do="dial", to=f"sip:echo@127.0.0.1:{echo_sip}")
        check(reply.get("ok") is True and len(reply.get("call", "")) == 16,
              f"Consort places the call ({reply})")
        answered = line.wait({"answered"})
        check(answered.get("event") == "answered" and answered.get("call") == reply.get("call"),
              f"answered at the far end's 200 OK ({answered.get('event')})")
        if answered.get("event") != "answered":
            return
        conn = Conn(line.secret, answered)
        check(conn.proved, "both ends prove themselves")

        burst = tone(0.06, 1000, -10)
        trips = []
        for _ in range(12):
            time.sleep(0.4)
            while not conn.frames.empty():
                conn.frames.get_nowait()
            sent_at = conn.send(AUDIO, burst[:SAMPLES].tobytes())
            conn.send(AUDIO, burst[SAMPLES:2 * SAMPLES].tobytes())
            conn.send(AUDIO, burst[2 * SAMPLES:].tobytes())
            ends = time.monotonic() + 2.0
            while time.monotonic() < ends:
                got = conn.next({AUDIO}, within=0.5)
                if not got or got[1] is None:
                    break
                if dbfs(array.array("h", got[2])) > -30:
                    trips.append(got[0] - sent_at)
                    break
        check(len(trips) >= 10, f"the burst came back {len(trips)} times of 12")
        if trips:
            trips.sort()
            print(f"  round trip through the line and an echo: median "
                  f"{statistics.median(trips) * 1000:.0f} ms, "
                  f"{trips[0] * 1000:.0f} to {trips[-1] * 1000:.0f} ms")

        # Consort hangs up over the control channel
        reply = line.send(do="hangup", call=answered["call"])
        check(reply.get("ok") is True, "Consort hangs up")
        check(line.wait({"closed"}).get("event") == "closed", "closed")
        gone = conn.next({HANGUP}, within=5.0)
        check(bool(gone) and gone[1] is None,
              f"and the call's connection closes with no HANGUP: Consort hung up "
              f"({gone and gone[1]})")
        conn.close()
    finally:
        line.stop()
        echo.stop()


def bursts(sender: Conn, hearer: Conn, times: int = 10) -> list[float]:
    """Seconds from a 60 ms burst sent on one call's connection to its first loud frame on the
    other's: one way through the line's send path, RTP on 127.0.0.1, and its receive path."""
    burst = tone(0.06, 1000, -10)
    trips = []
    for _ in range(times):
        time.sleep(0.4)
        while not hearer.frames.empty():
            hearer.frames.get_nowait()
        sent_at = sender.send(AUDIO, burst[:SAMPLES].tobytes())
        sender.send(AUDIO, burst[SAMPLES:2 * SAMPLES].tobytes())
        sender.send(AUDIO, burst[2 * SAMPLES:].tobytes())
        ends = time.monotonic() + 2.0
        while time.monotonic() < ends:
            got = hearer.next({AUDIO}, within=0.5)
            if not got or got[1] is None:
                break
            if dbfs(array.array("h", got[2])) > -30:
                trips.append(got[0] - sent_at)
                break
    return sorted(trips)


def two_lines(line_exe: str, caller_exe: str, root: Path) -> None:
    print("Two lines, one calling the other: what the line adds, each way, on one clock")
    a = Line(line_exe, root / "a")
    b = Line(line_exe, root / "b")
    try:
        hello_of(a)
        hello_b = hello_of(b)
        b_sip = int(hello_b["sip"].rsplit(":", 1)[1])
        placed = a.send(do="dial", to=f"sip:line@127.0.0.1:{b_sip}")
        check(placed.get("ok") is True, f"line A places the call ({placed})")
        incoming = b.wait({"incoming"})
        check(incoming.get("event") == "incoming", "it rings on line B")
        check(b.send(do="accept", call=incoming.get("call", "")).get("ok") is True,
              "B's Consort accepts it")
        answered_b, answered_a = b.wait({"answered"}), a.wait({"answered"})
        check(answered_a.get("event") == answered_b.get("event") == "answered",
              "answered on both lines")
        if answered_a.get("event") != "answered" or answered_b.get("event") != "answered":
            return
        conn_a, conn_b = Conn(a.secret, answered_a), Conn(b.secret, answered_b)
        check(conn_a.proved and conn_b.proved, "both calls' ends prove themselves")

        for name, trips in (("A to B", bursts(conn_a, conn_b)), ("B to A", bursts(conn_b, conn_a))):
            check(len(trips) >= 8, f"{name}: the burst came through {len(trips)} times of 10")
            if trips:
                print(f"  {name}: median {statistics.median(trips) * 1000:.0f} ms, "
                      f"{trips[0] * 1000:.0f} to {trips[-1] * 1000:.0f} ms")

        check(a.send(do="hangup", call=answered_a["call"]).get("ok") is True, "A hangs up")
        check(a.wait({"closed"}).get("event") == "closed", "closed on A")
        check(b.wait({"closed"}).get("event") == "closed", "and on B, which the far end left")
        gone_b = conn_b.next({HANGUP}, within=5.0)
        check(bool(gone_b) and gone_b[1] == HANGUP,
              f"B's Consort hears HANGUP: its far end hung up ({gone_b and gone_b[1]})")
        gone_a = conn_a.next({HANGUP}, within=5.0)
        check(bool(gone_a) and gone_a[1] is None,
              f"A's connection closes with no HANGUP: A's Consort hung up "
              f"({gone_a and gone_a[1]})")
        conn_a.close()
        conn_b.close()
    finally:
        a.stop()
        b.stop()


def standard_output_going(line_exe: str, caller_exe: str, root: Path) -> None:
    print("Consort's end of standard output goes while a call is up, then the call ends")
    silence = root / "silence.wav"
    write_wav(silence, array.array("h", bytes(2 * RATE * 10)))
    endings = ("Consort's HANGUP frame", "Consort's hangup command", "the far end's hang-up")
    for n, how in enumerate(endings):
        far_sip, ctrl_port = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
        if n < 2:
            far = FarEnd(caller_exe, root / f"echo{n}", far_sip, "sip:echo@127.0.0.1", [
                "audio_source\t\taubridge,echo", "audio_player\t\taubridge,echo",
                "module\t\t\taubridge.so", "module_app\t\techo.so"])
        else:
            far = FarEnd(caller_exe, root / f"caller{n}", far_sip, "sip:caller@127.0.0.1", [
                f"audio_source\t\taufile,{silence}",
                f"audio_player\t\taufile,{root / 'heard.wav'}",
                "module\t\t\taufile.so", "module_app\t\tmenu.so", "module_app\t\tctrl_tcp.so",
                f"ctrl_tcp_listen\t\t127.0.0.1:{ctrl_port}"])
        line = Line(line_exe, root / f"line{n}", close_after="answered")
        try:
            hello = hello_of(line)
            if n < 2:
                time.sleep(0.5)
                line.send(do="dial", to=f"sip:echo@127.0.0.1:{far_sip}")
            else:
                ctrl = Ctrl(ctrl_port)
                ctrl.do("dial", f"sip:line@127.0.0.1:{hello['sip'].rsplit(':', 1)[1]}")
                incoming = line.wait({"incoming"})
                line.send(do="accept", call=incoming.get("call", ""))
            answered = line.wait({"answered"})
            if not check(answered.get("event") == "answered",
                         f"{how}: the call is answered ({answered.get('event')})"):
                continue
            check(line.wait({"eof"}).get("event") == "eof",
                  "  Consort's end of standard output closes; standard input stays open")
            conn = Conn(line.secret, answered)
            check(conn.proved, "  both ends prove themselves")
            if n == 0:
                conn.send(HANGUP)
            elif n == 1:
                line.proc.stdin.write(json.dumps(
                    {"id": line.ids + 1, "do": "hangup", "call": answered["call"]}).encode()
                    + b"\n")
                line.proc.stdin.flush()
            else:
                ctrl.do("hangup")
            # The closed event can't be written: the line hangs the call up once, then quits
            code = line.exit_code()
            check(code == 0, f"  the call is hung up once, and the line exits 0 ({code})")
            conn.close()
        finally:
            line.stop()
            far.stop()


def the_control_channel(line_exe: str, caller_exe: str, root: Path) -> None:
    print("The control channel")
    secret_line = json.dumps({"line": 1, "secret": "11" * 32}).encode()
    settings = json.dumps(SETTINGS).encode()
    cases = [
        ("a secret that isn't 32 bytes", [b'{"line": 1, "secret": "00"}', b"{}"], "secret"),
        ("a secret line with more in it",
         [json.dumps({"line": 1, "secret": "11" * 32, "x": 1}).encode()], "secret"),
        ("a secret line with more after it", [secret_line + b' {"x": 1}', settings], "secret"),
        ("a secret line with a key twice",
         [b'{"line": 1, "secret": "' + b"11" * 32 + b'", "secret": "' + b"22" * 32 + b'"}',
          settings], "secret"),
        ("a secret line with a NUL in it", [secret_line + b"\x00 junk", settings], "secret"),
        ("a secret with a \\u0000 in it",
         [b'{"line": 1, "secret": "' + b"11" * 32 + b'\\u0000"}', settings], "secret"),
        ("settings that listen beyond 127.0.0.1",
         [secret_line, json.dumps({**SETTINGS, "listen": "0.0.0.0:5060"}).encode()], "settings"),
        ("an account that isn't on 127.0.0.1",
         [secret_line, json.dumps({**SETTINGS, "account": "sip:x@example.com"}).encode()],
         "settings"),
        ("settings with more in them",
         [secret_line, json.dumps({**SETTINGS, "module": "ctrl_tcp"}).encode()], "settings"),
        ("settings with more after them", [secret_line, settings + b" x"], "settings"),
    ]
    for n, (what, first, why) in enumerate(cases):
        line = Line(line_exe, root / f"first{n}", first=first)
        try:
            failed = line.wait({"failed"})
            check(failed.get("event") == "failed" and failed.get("why") == why,
                  f"{what}: failed ({failed.get('why')})")
            code = line.exit_code()
            check(code == 2, f"  and it exits 2 ({code})")
        finally:
            line.stop()

    # Both first lines are due 10 s from the start, not 10 s each
    line = Line(line_exe, root / "late", first=[])
    try:
        time.sleep(6)
        line.proc.stdin.write(secret_line + b"\n")
        line.proc.stdin.flush()
        time.sleep(6)                       # 12 s from the start, 6 s after the secret
        try:
            line.proc.stdin.write(settings + b"\n")
            line.proc.stdin.flush()
        except OSError:
            pass                            # it has gone already, as it should
        got = line.wait({"hello"})
        check(got.get("event") == "failed" and got.get("why") == "settings",
              f"settings 12 s after the start: failed ({got})")
        code = line.exit_code()
        check(code == 2, f"  and it exits 2 ({code})")
    finally:
        line.stop()

    # A line after the first two that breaks a rule: failed, and it exits 1
    broken = [
        ("a command with a key it doesn't take",
         b'{"id": 1, "do": "dial", "to": "sip:x@127.0.0.1", "via": "y"}'),
        ("a command with a key twice",
         b'{"id": 1, "do": "hangup", "call": "0123456789abcdef", "call": "fedcba9876543210"}'),
        ("two commands on one line",
         b'{"id": 1, "do": "hangup", "call": "0123456789abcdef"} {"id": 2, "do": "quit"}'),
        ("a command with a NUL in it", b'{"id": 1, "do": "quit"}\x00 junk'),
        ("a command with a \\u0000 in it", b'{"id": 1, "do": "quit\\u0000x"}'),
        ("a line past 4,096 bytes", b"x" * 5000),
    ]
    for n, (what, data) in enumerate(broken):
        line = Line(line_exe, root / f"broken{n}")
        try:
            hello_of(line)
            line.proc.stdin.write(data + b"\n")
            line.proc.stdin.flush()
            got = line.wait({"failed", "reply"})
            check(got.get("event") == "failed", f"{what}: failed ({got})")
            code = line.exit_code()
            check(code == 1, f"  and it exits 1 ({code})")
        finally:
            line.stop()

    # hello is the first line even when a command comes with the first two
    line = Line(line_exe, root / "eager",
                first=[secret_line, settings, b'{"id": 1, "do": "quit"}'])
    try:
        end = line.wait(set(), within=15)
        check(end.get("event") == "eof", f"a quit sent with the first lines: it quits ({end})")
        check(len(line.raw) == 2 and line.raw[0].startswith(b'{"event":"hello"')
              and line.raw[1].startswith(b'{"event":"reply","id":1,"ok":true'),
              f"  hello first, then the quit's reply ({line.raw})")
        code = line.exit_code()
        check(code == 0, f"  and it exits 0 ({code})")
    finally:
        line.stop()

    line = Line(line_exe, root / "quit")
    try:
        hello_of(line)
        reply = line.send(do="quit")
        check(reply.get("event") == "reply" and reply.get("ok") is True,
              f"Consort says quit: it replies ({reply})")
        code = line.exit_code()
        check(code == 0, f"  and it exits 0 ({code})")
    finally:
        line.stop()

    line = Line(line_exe, root / "eof")
    try:
        hello_of(line)
        line.proc.stdin.close()
        code = line.exit_code()
        check(code == 0, f"Consort's end closing: it exits 0 ({code})")
    finally:
        line.stop()

    # Consort's end of standard output closed before hello: hello can't be written, and
    # that is Consort closing its end, so 0
    folder = root / "no-output"
    folder.mkdir()
    with open(folder / "stderr.txt", "wb") as log:
        proc = subprocess.Popen([line_exe], cwd=folder, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=log)
        try:
            proc.stdout.close()
            proc.stdin.write(secret_line + b"\n" + settings + b"\n")
            proc.stdin.flush()
            code = proc.wait(15)
        except (OSError, subprocess.TimeoutExpired) as error:
            code = repr(error)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(10)
            try:
                proc.stdin.close()
            except OSError:
                pass
    check(code == 0,
          f"Consort's end of standard output closed before hello: it exits 0 ({code})")


def main(argv: list[str]) -> int:
    if len(argv) != 2 or not all(Path(p).is_file() for p in argv):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    line_exe, caller_exe = (str(Path(p).resolve()) for p in argv)
    with tempfile.TemporaryDirectory(prefix="consort-line-") as made:
        root = Path(made)
        for test in (a_call_that_rings_here, a_call_at_30_ms, a_call_placed_to_an_echo,
                     two_lines, standard_output_going, the_control_channel):
            folder = root / test.__name__
            folder.mkdir()
            before = len(FAILED)
            try:
                test(line_exe, caller_exe, folder)
            except Exception as error:       # a test that breaks is a failure, said
                check(False, f"{test.__name__} raised {error!r}")
            if len(FAILED) > before:
                for log in sorted(folder.rglob("stderr.txt")):
                    print(f"--- {log.relative_to(root)} (last 40 lines)")
                    lines = log.read_text("utf-8", "replace").splitlines()
                    print("\n".join(lines[-40:]))
    print(f"{len(FAILED)} failed" if FAILED else "every check passed")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
