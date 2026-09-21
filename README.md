# Project 1 - Simple Mail Client

- Name: Davis Garman
- Email: davisgarman@u.boisestate.edu
- Class: CS425-001

## Overview

A C mail client that implements the P1 SMTP dialogue directly over a TCP socket,
without a mail library. It checks the greeting before sending anything, then
sends HELO, MAIL FROM, RCPT TO, DATA, the message, and QUIT. The required replies
are 220, 250, 250, 250, 354, 250, and 221, respectively. A failed step stops the
session and reports the stage and the server's actual reply.

## Building and Running

Requires a POSIX/Linux environment, a C compiler, and GNU Make. Coverage reporting
also requires gcovr. The optional CLI integration tests use Python 3's standard
library. Unity is included in the repository.

```sh
make release
./build/release/myapp
```

Running with no arguments prints usage and exits successfully.

```text
Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port]
          [-H helo-host] <server>
```

| Argument | Meaning | Default |
| --- | --- | --- |
| `-f` | Envelope sender and From header | Required |
| `-t` | Envelope recipient and To header | Required |
| `-s` | Subject header | Empty |
| `-b` | Message body | Read stdin through EOF |
| `-p` | TCP port or service name | `25` |
| `-H` | Name sent with HELO | `localhost` |
| `<server>` | Server hostname, IPv4 address, or IPv6 address | Required |

```sh
printf 'This is the message body.\n.A line starting with a dot.\n' | \
  ./build/release/myapp -f me@boisestate.edu -t you@example.com \
  -s 'hello' -H onyx.boisestate.edu -p 2525 \
  <server>
```

## Design

`src/main.c` uses `getopt`, applies defaults, reads stdin when needed, connects
the socket transport to the session, and closes/frees resources on completion.
CR or LF in addresses, subjects, or the HELO name is rejected before connecting.
The body may contain mixed newline styles; NUL bytes from stdin are rejected
instead of silently truncating the message. All session allocations share a
cleanup path, including rejected commands and disconnects.

This separation makes the client-server protocol testable without a live mail
server. The application initiates the connection, sends a command, and waits for
the server's reply before advancing. Tests replace the server with scripted
bytes and capture exactly what the client sends.

## Testing and Coverage

Run the Task 3–5 checks:

```sh
make clean
make all
make check
make report
make leak
make leak-test
```

The 13 Unity tests in `tests/lab-test.c` exercise every function declared in
`src/lab.h`, including:

- Exact command and DATA formatting, empty messages, mixed newlines, leading
  periods, and CR/LF injection rejection.
- Single-line and continuation replies, fragmented reads, coalesced replies,
  replies spanning the transport buffer, malformed replies, and line-size limits.
- A successful complete session, every wrong status code, every truncated prefix
  of a session's replies, read errors, partial writes, and failed/zero-byte writes.
- Socket callbacks through a local socket pair, and connection success, refusal,
  and resolution errors using an ephemeral loopback listener.

Protocol tests use only the in-memory transport. Socket tests require permission
to create local sockets, but do not contact an external server.

Local verification achieved 100% line coverage (203/203)
`make leak` and `make leak-test` run AddressSanitizer with leak detection enabled.
Both passed locally, including the session's error and cleanup paths.

Additional end-to-end checks run the executable against a local scripted TCP
server, checking exact wire data, stdin, defaults, optional flags, invalid
arguments, all rejected reply stages, and exit codes:

```sh
make integration
```

## Known Bugs or Issues

No known functional failures

## Experience

The main design challenge is that TCP supplies a byte stream rather than SMTP
reply lines. Keeping unread bytes in the session and using a scripted transport
makes fragmentation and command sequencing reproducible in tests. Another
important boundary is the distinction between a literal body line containing a
period and the end-of-DATA marker: newline normalization and dot stuffing happen
before the final terminator is appended. Testing each rejection and truncated
reply also checks that the same cleanup path works throughout the dialogue.
