# PrinterDriver

A cross-platform receipt-printing SDK whose entire implementation — ESC/POS encoding,
printer response parsing, job state machine, transports and retry policy — lives in one
portable C++17 core, with platform wrappers reduced to thin bindings. It exists because
every layer between a POS app and the paper reports "success" when it has merely handed
bytes to the next layer, so kitchen tickets silently fail to print, staff resend them, and
the buffered original prints afterwards. The core replaces that with ordered completion
fences (`GS ( H` process-ID markers, `GS r 1`) and an honest tri-state job outcome in which
*unknown* is a first-class result rather than something collapsed into success or failure.

## Status

Milestone 7. On top of the durable job store, TCP transport and print engine there is now
printer identification, a compositional capability profile, the device database from
[docs/device-database.md](docs/device-database.md), and probe-then-promote: a device is
interrogated non-destructively once, the findings override the shipped profile defaults,
and they are cached by identity so nothing is re-probed on every boot. Every job result
carries a confidence grade, a completion authority and the method that produced it.

Identity is untrusted by default. `GS I` is a string the firmware chooses, and at least
one printer family ships answering as somebody else's model
([docs/capability-profiles.md](docs/capability-profiles.md)), so identification combines
MAC OUI, the reported strings and observed command behaviour.

Not built yet: the C ABI and platform wrappers, Bluetooth/USB/serial transports, the ePOS
and StarPRNT transports (their profiles are data only), network discovery, and the
print-queue addon.

## Build and test

Requires CMake 3.16+ and a C++17 compiler. There are no third-party dependencies and
nothing is downloaded during configuration.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The build produces the static library `printerdriver_core` and the `pdctl` diagnostic CLI;
public headers live in `core/include/printerdriver/`. Tests are plain executables using a
small built-in assert harness (`core/tests/test_harness.hpp`) and are registered
individually with CTest.

## pdctl

```sh
build/pdctl status   <host>                   # DLE EOT 1-4 decoded plus raw bytes
build/pdctl probe    <host> [--mac <address>] # full printer discovery report
build/pdctl identify <host> [--mac <address>] # fingerprint only, prints nothing
build/pdctl print    <host> --text "..." --key order-7F3A
build/pdctl print list                        # the device database
```

`print` runs through the whole engine — preflight, ordered fence, cut fence, cutter
status, job store — and exits 0 on `done`, 1 on `failed`, 2 on `unknown`. The core owns a
printer connection exclusively, so stop CUPS and every other client first.

`probe` sends only non-destructive queries: DLE EOT 1-4, `GS I`, one `GS ( H` marker,
`GS r 1` and ASB on/off. It never sends `DLE ENQ`, a power-off or a buffer clear — those
resume, discard or interrupt a ticket that may be half printed, so they live behind
separate operator commands that print a warning banner naming what they are about to do:

```sh
build/pdctl recover    <host> --resume|--clear   # DLE ENQ 1 / DLE ENQ 2
build/pdctl counters   <host>                    # GS g 2 maintenance counters
build/pdctl test-print <host>                    # GS ( A, consumes paper
build/pdctl settings   <host>                    # GS ( E fn 4 / fn 6 readback
```

## Layout

```
core/include/printerdriver/   public headers
  types.hpp                   closed enums, JobResult, JobEvent
  escpos_encoder.hpp          byte builder and the fence/status primitives
  response_parser.hpp         incremental parser for the interleaved return stream
  capability_profile.hpp      compositional profile: identity, transport, completion,
                              status, recovery, quirks, media
  device_profiles.hpp         the device database, one entry per printer family
  identity.hpp                GS I parsing, MAC OUI table, multi-signal identify()
  capability_probe.hpp        non-destructive interrogation, findings, promotion,
                              and the findings cache
  job_store.hpp               append-only durable job journal
  transport.hpp               Transport interface and the TCP implementation
  driver.hpp                  PrinterDriver / Printer / PrintJob — the public API
core/src/                     implementation
core/tests/                   test harness, scriptable fake printer, test binaries
tools/pdctl.cpp               command-line diagnostics
docs/                         specifications — these are authoritative, not the code
scripts/printer_probe.py      standalone hardware capability probe
```

## Threads

One worker thread per printer runs jobs strictly FIFO, one at a time. Each open
connection has its own reader thread pumping received bytes into the response parser.
Job-event and device-event callbacks run on those threads and must not block or call
back into the driver.

## Documentation

- [docs/brief.md](docs/brief.md) — the operational problem and the research behind it.
- [docs/sdk-spec.md](docs/sdk-spec.md) — what is being built; §5 defines the closed enums.
- [docs/api.md](docs/api.md) — the public object model and payload tiers.
- [docs/techspec.md](docs/techspec.md) — protocol detail; §3 is the ESC/POS command
  reference this encoder implements, including the real-time vs queued distinction that
  makes completion fencing possible.
- [docs/testing-plan.md](docs/testing-plan.md) — the capability probe and its result for
  our hardware, plus the fault-injection matrix that must be run before any capability
  profile is trusted in production.
- [docs/capability-profiles.md](docs/capability-profiles.md) — per-model command research,
  the compositional profile hierarchy, and why `GS I` cannot be trusted on its own.
- [docs/device-database.md](docs/device-database.md) — the printer/media/transport matrix,
  print-server semantics, and the confidence grades every result carries.
