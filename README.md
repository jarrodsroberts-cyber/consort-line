# consort-line

A phone line for one program on a Windows PC: [baresip](https://github.com/baresip/baresip)
(SIP, RTP, the jitter buffer, G.711) and a small add-on that carries each call's audio to that
program over one 127.0.0.1 connection a call. The frames are in AudioSocket's form, with marks,
flushes and HMAC proofs both ways. It is built for Consort, a desktop assistant, and works for
any program that speaks [PROTOCOL.md](PROTOCOL.md).

## How it runs

One program, `consort-line.exe`, with no arguments and no files:

- Its standard input and output are the control channel. The first line is a 32-byte secret,
  the second the settings, then commands one way and events the other.
- It loads only two of baresip's modules: G.711 and the add-on.
- Each answered call gets its own port on 127.0.0.1 and a fresh nonce. The program connects
  there and proves it holds the secret, and the line proves itself back.
- It exits with 0 after `quit` or once the program closes its end of the channel, 2 when the
  first two lines weren't right, and 1 when a later line breaks a rule or anything else fails.

It doesn't use baresip's `ctrl_tcp`, which gives the line to whichever program connects to
it last. [PROTOCOL.md](PROTOCOL.md) has the whole contract, and what it guards against.

## Building

Only on CI ([.github/workflows/build.yml](.github/workflows/build.yml)): Windows, MSVC, CMake
and Ninja. re and baresip come from their v4.13.0 commits, and OpenSSL is baresip's own
prebuilt zip for Windows, checked by its SHA-256. The add-on is built inside baresip's build
as an app module, so `consort-line.exe` links libbaresip statically.

- `consort/`: the add-on and the program.
  - `proto.c`: the frames and the player.
  - `proof.c`: the proofs and random bytes, from Windows' BCrypt.
  - `link.c`: one call's listener, frames, deadlines and 20 ms tick.
  - `wake.c`: waking baresip's main thread from another thread.
  - `consort.c`: calls, the control channel, and the audio source and player.
  - `main.c`: the program.
- `test/unit.c`: the add-on without baresip, including the link over real sockets.
- `test/call_test.py`: whole calls between the line and a far end (`test/caller.c`, never
  released) on 127.0.0.1, including a round trip through an echo, and the control channel's
  rules and exit codes.

## Releases

A tag `v*` makes a release from that run's build: `consort-line-<version>-windows-x64.zip`
(the program, this README, PROTOCOL.md and the licences of the add-on, baresip, re and OpenSSL)
and `SHA256SUMS`. A program that installs it should pin the zip's SHA-256.

## Licence

BSD 3-Clause, as baresip and re. See [LICENSE](LICENSE).
