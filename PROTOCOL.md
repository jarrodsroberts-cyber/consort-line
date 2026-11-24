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
   account, and `calls` is how many calls it takes at once.

Each must come within 10 seconds of the start and hold exactly these keys. Otherwise
consort-line writes `{"event":"failed","why":"secret"}` (or `"settings"`) and exits with 2. The
secret never travels anywhere else: never a file, an argument or an environment variable.

### Events (from consort-line)

| Event | When | Keys |
|---|---|---|
| `hello` | running, the first line it writes | `version`, `baresip`, `sip` (where SIP listens) |
| `incoming` | a call rings; nothing is answered until `accept` | `call`, `from`, `to` |
| `ringing`, `progress` | a call placed hears 180 or 183 (early media); any number of times | `call` |
| `answered` | the call is answered: its listener is bound | `call`, `port`, `nonce` (32 hex) |
| `closed` | the call is over | `call`, `why`, `figures` (the link's counts) |
| `reply` | a command's answer | `id`, `ok`, then `call` or `why` |
| `failed` | the channel broke a rule; consort-line exits | `why` |

`call` is consort-line's own id for a call, 16 lowercase hex digits. It is never baresip's call
id, which for an incoming call is the caller's own SIP Call-ID. Text from the far end (`from`,
`to`, `why`) is printable ASCII, with `?` for anything else, at most 256 characters.

### Commands (to consort-line)

Each holds an `id` (a whole number from 1, echoed in its `reply`) and nothing beyond its keys:

- `{"id": 1, "do": "dial", "to": "sip:..."}` places a call from the line's account.
- `{"id": 2, "do": "accept", "call": "<id>"}` answers a call that rang.
- `{"id": 3, "do": "hangup", "call": "<id>"}`
- `{"id": 4, "do": "quit"}` hangs up every call and exits.

A line that isn't JSON or one of these forms writes `failed` and exits. Standard input
closing hangs up every call and exits with 0.

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
and the call goes on. With no right ID within 2 seconds of `answered`, it hangs the call up.
The listener stays bound until the call's audio ends.

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
| 0xFF | ERROR | consort-line | 1 byte: 1 the far end hung up, 2 a frame it couldn't forward, 4 out of memory |

consort-line, once the agent's program has proved itself:

- sends the caller's AUDIO every 20 ms, silence where baresip has none, and never anything
  from before the proof;
- plays the agent's AUDIO from a buffer of at most a second, a frame each 20 ms, silence when
  dry (silence isn't counted);
- echoes each MARK when the frame before it has played (one with nothing before it at once);
- answers FLUSH at once with FLUSHED, after dropping every frame and MARK queued; the count
  goes back to 0 only when a MARK plays;
- sends one DTMF frame for each key the far end presses, at most ten a second (a 9 always goes);
- sends HANGUP when the far end hangs up, then closes.

A frame from the agent's program of a type it doesn't send (ID again, DTMF, FLUSHED, ERROR,
0x12 or any other), a payload not its type's size (judged from the first three bytes), a MARK
that isn't the next id, a 65th MARK waiting, or AUDIO that finds the buffer full: consort-line
sends ERROR 2, closes the connection and hangs the call up. It doesn't want HANGUP after its
own ERROR. The agent's program closing the connection, sending HANGUP, or not taking
consort-line's frames for 2 seconds also hangs the call up.

## What it guards against, and what it doesn't

- **Other programs on the PC.** No control port: the control channel is a pipe only its
  starter holds, and a call's connection must prove the secret. A program that reads a
  nonce, races to a call's port, or floods it with connections can neither hear nor speak on
  the call. Programs running as the same Windows user at the same integrity level can read
  any process's memory and handles, so they aren't kept out.
- **The far end.** Its text reaches the agent's program as ASCII only; it can't name a call's
  id; and it can't answer a call (`sip_autoanswer=no`) or transfer the line (`call_transfer=no`).
- **Not yet:** RTP from an address other than the far end's is still taken (baresip's own
  behaviour). Until that is closed, don't feed a line's audio to anything that trusts it.
