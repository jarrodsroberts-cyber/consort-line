# consort-line's protocol

consort-line carries phone calls between a SIP carrier and one program on the same PC, which
this page calls the agent's program. There are two channels:

- **The control channel**: consort-line's standard input and output. Only the program that
  started consort-line holds them.
- **A call's connection**: one TCP connection on 127.0.0.1 for each answered call, carrying
  the call's audio as frames.

## The control channel

UTF-8 JSON, one object a line, at most 4,096 bytes a line, ASCII only in what consort-line
writes. Nothing else ever appears on standard output: baresip's log goes to standard error.

### The first two lines (to consort-line)

1. The secret, 32 bytes from the system's random source, as 64 hex digits:
   `{"line": 1, "secret": "<64 hex>"}`
2. The settings: `{"listen": "127.0.0.1:<port>", "account": "sip:<user>@127.0.0.1", "calls": <1 to 16>}`.
   `listen` is where SIP listens (port 0 lets Windows pick one), `account` is the line's one
   account (`<user>` letters, digits, `.`, `_` and `-`; the whole account 64 bytes at most),
   and `calls` is how many calls it takes at once.

Both must have come within 10 seconds of the start. Each is one JSON object with exactly these
keys, each once, and nothing after it, in the form the commands below take. Otherwise
consort-line writes
`{"event":"failed","why":"secret"}` (or `"settings"`) and exits with 2. The secret never
travels anywhere else: never a file, an argument or an environment variable.

### Events (from consort-line)

| Event | When | Keys |
|---|---|---|
| `hello` | running, the first line it writes | `version`, `baresip`, `sip` (where SIP listens) |
| `incoming` | a call rings; nothing is answered until `accept` | `call`, `from`, `to` |
| `ringing`, `progress` | a call placed hears 180 or 183 (early media); any number of times | `call` |
| `answered` | the call is answered: its listener is bound | `call`, `port`, `nonce` (32 hex) |
| `closed` | the call is over | `call`, `why`, `figures` (the link's counts) |
| `reply` | a command's answer | `id`, `ok`; `why` when not ok; `call` when the command found or placed a call |
| `failed` | the channel broke a rule, or the line couldn't start (`why`: "the line couldn't start"); consort-line exits | `why` |

`call` is consort-line's own id for a call, 16 lowercase hex digits. It is never baresip's call
id, which for an incoming call is the caller's own SIP Call-ID. Text from the far end (`from`,
`to`, `why`) is printable ASCII, with `?` for anything else, at most 256 characters.

### Commands (to consort-line)

Each is one JSON object on its line, with an `id` (a whole number from 1 to 2^53 - 1, that is
9,007,199,254,740,991, echoed in its `reply`) and its other keys, each once, and nothing
beyond them:

- `{"id": 1, "do": "dial", "to": "sip:..."}` places a call from the line's account. `to` is
  `sip:` and then printable ASCII, with no spaces and none of `<`, `>`, `"`, `;` or `?` (so no
  URI parameters or headers: baresip would send a `?Replaces=` as a Replaces header), 512
  bytes at most. Any other `to` string is refused in the reply (`why`: "an address the line
  doesn't take"), and the line goes on. A dial whose `to` is missing or isn't a string isn't
  this form, and nor is one with an escaped NUL or surrogate in it (below).
- `{"id": 2, "do": "accept", "call": "<id>"}` answers a call that rang.
- `{"id": 3, "do": "hangup", "call": "<id>"}`
- `{"id": 4, "do": "quit"}` hangs up every call and exits.

A line that isn't one of these forms writes `failed`, hangs up every call and exits with 1.
Each member must be a name, a colon and a value (a string, or a word such as a number), with
no value an object or an array. So these count as not: a key twice, anything after the
object, a name with no value, an empty member or a trailing comma, and an escaped NUL or
surrogate (`\u0000`, `\ud800` to `\udfff`). Standard input closing hangs up every call and
exits with 0.

### Exit codes

- 0: after `quit`, when standard input closes, or when standard output can't be written
  (the agent's program closed its end).
- 2: the first two lines weren't right.
- 1: a later line broke a rule, or anything else went wrong.

## A call's connection

Once a call is answered (on a call that rang, once `accept` was sent; on a call placed, at the
far end's 200 OK, never during early media), consort-line:

1. listens on 127.0.0.1 alone, on a port Windows picks, bound exclusively
   (`SO_EXCLUSIVEADDRUSE`, never `SO_REUSEADDR`);
2. makes a fresh 16-byte nonce;
3. writes `answered` with the port and the nonce.

The agent's program connects to 127.0.0.1 (written out, never a name) at that port, and its
first frame is its ID. consort-line takes the one connection whose ID proves the agent's
program, compared in constant time, and then sends its own ID. It closes at once any
connection with a wrong ID, with none within a second, or that comes after the right one,
and the call goes on. At most 16 connections wait to prove themselves: to make room for
another, the oldest is read one last time and, unless that proves it, closed. So the agent's
program sends its ID as soon as it connects. With no right ID within 2 seconds of `answered`,
it hangs the call up. The listener stays bound until the call's audio ends.

### The proofs

Each proof is HMAC-SHA256 keyed with the secret's 32 bytes, over the ASCII bytes `consort`
(the agent's program's proof) or `line` (consort-line's), then the nonce's 16 bytes, then the
port as 2 bytes, big-endian: the digest's first 16 bytes.

Check value: with the secret's bytes 0x00 to 0x1f, the nonce's bytes 0x00 to 0x0f and port
40000, the agent's program's proof is `2167cf0b7445558125097303dee3f210` and consort-line's is
`f1d51df5fa017fd659d37b2b62c483d9`, so the agent's program's ID frame is
`0100102167cf0b7445558125097303dee3f210`.

### The frames

In AudioSocket's form: one byte of type, two bytes of length (big-endian), then the payload.

| Type | Name | Sent by | Payload |
|---|---|---|---|
| 0x00 | HANGUP | either | none |
| 0x01 | ID | the agent's program, then consort-line | 16 bytes: the sender's proof |
| 0x03 | DTMF | consort-line | 1 byte: `0`-`9`, `*`, `#` or `A`-`D` |
| 0x10 | AUDIO | either | 320 bytes: 20 ms, 160 samples, 16-bit signed little-endian, mono, 8 kHz |
| 0x20 | FLUSH | the agent's program | none |
| 0x21 | FLUSHED | consort-line | 8 bytes: the last MARK played (u32, big-endian; 0 for none), then the samples played since it (u32, big-endian) |
| 0x22 | MARK | either | 4 bytes: an id (u32, big-endian, from 1) |
| 0xFF | ERROR | consort-line | 1 byte: 2 a frame it couldn't forward, 4 out of memory (AudioSocket's 1, the far end hung up, never comes: that is HANGUP) |

AUDIO is 20 ms both ways, whatever packet time (`a=ptime`) the far end asks for: baresip
sends it packets of that length.

consort-line, once the agent's program has proved itself:

- sends the caller's AUDIO every 20 ms, silence where baresip has none, and never anything
  from before the proof;
- plays the agent's AUDIO from a buffer of at most a second, a frame each 20 ms, silence when
  dry (silence isn't counted). When consort-line falls behind, the frames wait and play late
  rather than in a rush, so the agent's program paces its audio by the MARKs that come back,
  not by the clock;
- echoes each MARK when the frame before it has played (one with nothing before it at once);
- answers FLUSH at once with FLUSHED, after dropping every frame and MARK queued; the count
  goes back to 0 only when a MARK plays;
- sends one DTMF frame for each key the far end presses, at most ten a second (a 9 always goes);
- sends HANGUP when the call ends at the far end (it hangs up, or the call drops), after
  every frame still waiting for the agent's program, then closes. When the agent's program
  ends the call (`hangup`, `quit`, standard input or output closing, or a line that broke a
  rule), the connection just closes, with no HANGUP. Either way, `closed` on the control
  channel is the record of how the call ended.

A frame from the agent's program of a type it doesn't send (ID again, DTMF, FLUSHED, ERROR,
0x12 or any other), a payload not its type's size (judged from the first three bytes), a MARK
that isn't the next id, a 65th MARK waiting, or AUDIO that finds the buffer full: consort-line
sends ERROR 2 (after every frame still waiting), closes the connection and hangs the call up.
It doesn't want HANGUP after its own ERROR.

After HANGUP or ERROR, consort-line shuts its end of the connection and reads and drops
whatever the agent's program still sends, until the agent's program closes its end or 2
seconds pass, and only then closes the socket. On Windows a socket closed with bytes unread,
or that bytes reach once it is closed, ends in a reset that drops everything not yet read, so
an agent's program still speaking would lose the last frames. The agent's program closes its
end once it has read HANGUP, ERROR or the end of the connection. When consort-line exits it
waits for connections still closing this way (2 seconds at most) before it goes.

The agent's program closing the connection, sending HANGUP, or not taking
consort-line's frames for 2 seconds also hangs the call up. The 2 seconds count from when the
connection holds no more frames: consort-line sets its own send buffer to 2,048 bytes, and
the agent's program sets its socket's receive buffer to 4,096 bytes (`SO_RCVBUF`, before it
connects). The connection then holds about half a second of frames on Windows 11 (26
measured, steady over a call), against every frame of 3 seconds and more with Windows' own
buffer sizes. Windows Server 2022 lets its share grow over a call (7 to 97 frames measured).

## What it guards against, and what it doesn't

- **Other programs on the PC.** No control port: the control channel is a pipe only its
  starter holds, and a call's connection must prove the secret. A program that reads a
  nonce, races to a call's port, or floods it with connections can neither hear nor speak on
  the call. Programs running as the same Windows user at the same integrity level can read
  any process's memory and handles, so they aren't kept out.
- **The far end.** Its text reaches the agent's program as ASCII only; it can't name a call's
  id; and it can't answer a call (`sip_autoanswer=no`) or transfer the line (`call_transfer=no`).
- **SIP from other programs.** In this version SIP on 127.0.0.1 takes an INVITE from any
  program on the PC, so an `incoming` call may not be the carrier's. A version with a carrier
  takes INVITEs only from the carrier.
- **Not yet:** RTP from an address other than the far end's is still taken (baresip's own
  behaviour). Until that is closed, don't feed a line's audio to anything that trusts it.
