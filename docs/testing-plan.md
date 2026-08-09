# Testing Plan: XP-S260M Capability Probe

Companion to [docs/techspec.md](techspec.md). This is the concrete, runnable test to find
out — for the actual XP-S260M unit and firmware we own — which completion-acknowledgement
mechanisms are supported, over which interface, before any of the architecture in the tech
spec gets built.

**This is a 5-minute test, not a research question.** Run it before writing any agent code.

## ✅ Result: our XP-S260M unit — tested 2026-08-08

Probe run over LAN (`192.168.1.101:9100`, MAC `00:61:17:5b:b4:65`), no CUPS queues
configured, exclusive access:

```
DLE EOT response: 16
GS(H): SUPPORTED 37 22 50 30 30 31 00
GS r response: 00
```

This is the **best-case row** of the interpretation table below:

- **Bidirectional LAN backchannel: YES.** `DLE EOT 1` returned `0x16` — online, no error
  bits set. The Ethernet module forwards status bytes back on the same 9100 socket.
- **`GS ( H` Function 48: SUPPORTED.** The printer physically printed the test line and
  then echoed the exact process-ID frame (`37 22 50 30 30 31 00` for token `P001`).
  Strong per-receipt print-completion acknowledgement is available on this hardware.
- **`GS r 1`: RESPONDS.** Returned `0x00` (paper present) after the second test line —
  the queued fallback mechanism also works.

**Consequence:** build the SDK core's primary completion path on `GS ( H` process-ID
acknowledgements ([techspec.md §3.1](techspec.md#31-gs--h-function-48--process-id-preferred-mechanism),
[§5.2](techspec.md#52-suggested-strong-sequence-when-gs--h-is-supported)), with `GS r 1`
as the fallback — which this unit also supports.

Still untested on this unit: the fault-injection matrix below (paper-out mid-job, cutter
jam, power loss, ack lost after print), the post-cut fence sequence (`P…`/`C…` markers
around a real cut), the Xprinter `ESC v` / `ESC x` vendor queries, and the serial
interface comparison.

## Before running

- Stop CUPS and every other client that might be talking to the printer. The probe needs
  exclusive use of the connection so responses can be attributed unambiguously.
- The script physically prints two short test sections but **does not cut**.
- Have the printer's LAN IP address ready (e.g. `192.168.1.50`).

## What the probe checks

1. **Can it talk back at all?** Sends `DLE EOT 1` (`10 04 01`). If we get a byte back, the
   interface is bidirectional.
2. **Does print-completion acknowledgement work?** Prints a short test line, then appends
   a `GS ( H` marker (`P001`). If the printer physically prints the line and then sends the
   marker back, we have proper completion detection.
3. **Does the `GS r 1` fallback work?** Sends the paper-status query queued after print
   data and checks for a response.

## Probe script

Save as `scripts/printer_probe.py` and run with `python3 scripts/printer_probe.py`
(edit `HOST`/`PORT` first, or adapt to take them as CLI args).

```python
#!/usr/bin/env python3
from __future__ import annotations

import socket
import time

HOST = "192.168.1.50"
PORT = 9100

ESC_INITIALIZE = b"\x1b@"
ASB_OFF = b"\x1d\x61\x00"
DLE_EOT_PRINTER = b"\x10\x04\x01"
GS_R_PAPER = b"\x1d\x72\x01"


def process_id_command(token: bytes) -> bytes:
    if len(token) != 4:
        raise ValueError("Process ID must be exactly four bytes")
    if any(value < 0x20 or value > 0x7E for value in token):
        raise ValueError("Process ID must contain printable ASCII only")
    return b"\x1d\x28\x48\x06\x00\x30\x30" + token


def receive_until_idle(
    sock: socket.socket,
    total_timeout: float,
    idle_timeout: float = 0.3,
) -> bytes:
    deadline = time.monotonic() + total_timeout
    last_data_at: float | None = None
    result = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            result.extend(chunk)
            last_data_at = time.monotonic()
        except socket.timeout:
            if (
                last_data_at is not None
                and time.monotonic() - last_data_at >= idle_timeout
            ):
                break
    return bytes(result)


with socket.create_connection((HOST, PORT), timeout=3.0) as sock:
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(0.2)

    # Reset and disable unsolicited ASB so the probe output is simpler.
    sock.sendall(ESC_INITIALIZE + ASB_OFF)
    receive_until_idle(sock, total_timeout=0.5)

    # Does the LAN interface return any real-time status?
    sock.sendall(DLE_EOT_PRINTER)
    response = receive_until_idle(sock, total_timeout=2.0)
    print(
        "DLE EOT response:",
        response.hex(" ") if response else "NO RESPONSE",
    )

    # Test Epson GS ( H Function 48.
    token = b"P001"
    expected = b"\x37\x22" + token + b"\x00"
    sock.sendall(
        b"GS(H) completion probe P001\n\n"
        + process_id_command(token)
    )
    response = receive_until_idle(sock, total_timeout=10.0)
    print(
        "GS(H):",
        "SUPPORTED" if expected in response else "NO MATCH",
        response.hex(" ") if response else "NO RESPONSE",
    )

    # Test queued GS r 1 completion response.
    sock.sendall(
        b"GS r completion probe\n\n"
        + GS_R_PAPER
    )
    response = receive_until_idle(sock, total_timeout=10.0)
    print(
        "GS r response:",
        response.hex(" ") if response else "NO RESPONSE",
    )
```

## Example output

```
Connecting to 192.168.1.123:9100...
✓ Connected
Testing bidirectional status...
✓ Printer replied: ONLINE
Testing print completion...
Printing TEST RECEIPT...
Waiting...
✓ PRINT COMPLETION ACK RECEIVED
  Time: 1.34 seconds
Testing cutter...
✓ Cut command processed
✓ No cutter error

RESULT
─────────────────────────────
Bidirectional comms       YES
Paper status              YES
Printer errors            YES
Print completion          YES
Cut processing            YES
─────────────────────────────
```

Or, importantly, a fallback-required result:

```
Print completion          NOT SUPPORTED
GS r completion           YES
```

## Interpretation table

| Result | Meaning |
|---|---|
| `DLE EOT` responds and `GS(H)` returns exact token | Excellent: strong per-receipt software completion is available |
| `DLE EOT` responds, `GS(H)` silent, `GS r` responds | Good fallback: one in-flight receipt with queued completion |
| All three silent over LAN | LAN adapter probably lacks a usable backchannel; repeat over serial or direct USB |
| Status works over serial but not LAN | Run the local printer agent over serial |
| Only Xprinter SDK/OPOS returns completion | Use vendor service object and validate its semantics |
| Nothing exposes ordered completion | Use status plus physical sensor, or replace the printer for critical jobs |

## Fault-injection tests (run before trusting any result in production)

| Test | Required outcome |
|---|---|
| Normal receipt | Completion response arrives once |
| Cover open before send | No receipt bytes sent; `FAILED_KNOWN` |
| Paper removed midway | No false completion; ASB/offline error recorded |
| Cutter deliberately jammed | Print may confirm, but cut must not be reported successful |
| LAN unplugged before transmission | Known transport failure |
| LAN unplugged after paper emerges but before acknowledgement | `UNKNOWN`, never automatic success or retry |
| Power removed midway | No false completion |
| Agent killed just after `sendall()` | On restart, job becomes `UNKNOWN` |
| Two clients attempt to print | Second client must queue behind the agent |
| Printer firmware changed | Capability suite reruns before production use |

**Acceptance rule:** there must be **no** test in which the system reports `DONE` while the
receipt demonstrably did not finish.

## Recommended order of attack

1. Run the probe against the XP-S260M's LAN port with CUPS and all POS clients stopped.
2. If `GS ( H` works, build the local printer agent around process-ID acknowledgements
   (see [techspec.md §5.2](techspec.md#52-suggested-strong-sequence-when-gs--h-is-supported)).
3. If only `GS r 1` works, use one job at a time, continuous ASB, and treat cutting
   separately (see [techspec.md §5.3](techspec.md#53-fallback-sequence-when-gs--h-is-unsupported)).
4. If LAN is write-only, repeat the same test through RS-232.
5. Obtain the exact Xprinter `ESC v`/`ESC x` "one ticket one control" protocol, or inspect
   their SDK/service object (see [techspec.md §3.5](techspec.md#35-xprinter-specific-one-ticket-one-control)).
6. Introduce a persistent `UNKNOWN` state before enabling any retry logic.
7. For mission-critical kitchen tickets, add a visible job ID immediately and consider QR
   verification at the paper exit (see [techspec.md §8](techspec.md#8-physical-verification-options)).

## What we need to run this

- The XP-S260M's LAN IP address on the local network (`192.168.x.x` or `10.x.x.x` is fine).
- Which machine will run the test: Mac, Linux/Raspberry Pi, or Windows.

## ✅ Hardware finding: the XP-S260M impersonates an Epson — 2026-08-09

`pdctl autodetect 192.168.1.0/24` against the reference unit:

```
IP                  VENDOR    MODEL       TRUSTED  PROFILE      COMPLETION  CEILING
192.168.1.101:9100  Unknown   TM-T88III   NO       generic_80   GsParenH    A
  identity untrusted (35%)
```

**The unit answers `GS I` with "TM-T88III"** — an Epson model it is not. This is the
impersonation case [capability-profiles.md](capability-profiles.md) documents for clone
firmware (Rongta's manual shows "EPOSN"/"TM-T88V"), now observed first-hand on our own
hardware rather than inferred from a vendor manual.

The identification logic behaved correctly: identity marked **untrusted** at 35%, the
Epson profile **not** loaded on the strength of a self-reported string, `generic_80`
selected instead, and the reason stated on the report. Had `GS I` been trusted, this
printer would have been driven with Epson-specific assumptions — including Epson's
drawer pinout and command set — on hardware that merely claims the name.

Also confirmed in the same run: `autodetect` promotes the completion *flag* but not its
*provenance* from a printless probe (`GS ( H` shown as `Unverified` even though the
mechanism answered), which is the documented honesty rule — a fence asked out of an
empty buffer proves the command exists, not that its answer waits for paper. The
`Probed` promotion in the earlier self-test came from a run that actually printed.

`pdctl drawer-probe` on the same unit reports the documented 24 V / 1 A `Epson24V6P6C`
port with sensor pin 3, kick method `EpsonEscP`, and `Unverified` command provenance —
a pulse would honestly end at `KickSentUnverified` until `pdctl drawer test` establishes
otherwise against a physically attached drawer.
