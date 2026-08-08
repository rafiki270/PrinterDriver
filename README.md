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

Milestone 1 — the foundations only: the closed public enums, the ESC/POS byte encoder, and
the incremental response parser, with golden-byte tests including the exact frames captured
from this project's Xprinter XP-S260M. The job state machine, transports, engine, job store
and CLI are milestone 2 and are not in this repository yet.

## Build and test

Requires CMake 3.16+ and a C++17 compiler. There are no third-party dependencies and
nothing is downloaded during configuration.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The build produces the static library `printerdriver_core`; public headers live in
`core/include/printerdriver/`. Tests are plain executables using a small built-in assert
harness (`core/tests/test_harness.hpp`) and are registered individually with CTest.

## Layout

```
core/include/printerdriver/   public headers (types, escpos_encoder, response_parser)
core/src/                     implementation
core/tests/                   test harness and per-area test binaries
docs/                         specifications — these are authoritative, not the code
scripts/printer_probe.py      hardware capability probe
```

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
