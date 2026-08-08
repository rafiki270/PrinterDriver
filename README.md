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

Milestone 2. On top of milestone 1's closed enums, ESC/POS encoder and response parser
there is now a durable job store, a TCP transport with a live backchannel, capability
profiles, and the print engine implementing the ordered completion sequences from
[docs/techspec.md §5.2](docs/techspec.md) and [§5.3](docs/techspec.md) behind the public
API in [docs/api.md](docs/api.md). `pdctl` drives all of it from the command line.

Not built yet: the C ABI and platform wrappers, Bluetooth/USB/serial transports,
discovery, and the print-queue addon.

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

## Swift package

`Package.swift` sits at the repository root so this repository is consumable directly by
SwiftPM — the git URL is the package URL. It compiles the same `core/src` and `capi/src`
files CMake does, and adds the Swift wrapper over the C ABI.

```sh
swift build      # iOS 16+ / macOS 13+
swift test       # runs on macOS
```

See [wrappers/swift/README.md](wrappers/swift/README.md) for the API, the threading
contract and the platform matrix.

## pdctl

```sh
build/pdctl status <host>                     # DLE EOT 1-4 decoded plus raw bytes
build/pdctl probe  <host>                     # which completion fence this unit supports
build/pdctl print  <host> --text "..." --key order-7F3A
```

`print` runs through the whole engine — preflight, ordered fence, cut fence, cutter
status, job store — and exits 0 on `done`, 1 on `failed`, 2 on `unknown`. The core owns a
printer connection exclusively, so stop CUPS and every other client first.

## Layout

```
core/include/printerdriver/   public headers
  types.hpp                   closed enums, JobResult, JobEvent
  escpos_encoder.hpp          byte builder and the fence/status primitives
  response_parser.hpp         incremental parser for the interleaved return stream
  capability_profile.hpp      per-model data; decides the reachable ConfidenceLevel
  job_store.hpp               append-only durable job journal
  transport.hpp               Transport interface and the TCP implementation
  driver.hpp                  PrinterDriver / Printer / PrintJob — the public API
core/src/                     implementation
core/tests/                   test harness, scriptable fake printer, test binaries
capi/include/printerdriver/   pd.h — the C ABI every wrapper binds, plus its modulemap
capi/src/                     the ABI implementation and its enum static_asserts
capi/tests/                   the C ABI test, the scripted-device factory, the enum bridge
queue/                        the print-queue addon (separate library on purpose)
Package.swift                 SwiftPM manifest for the whole repository
wrappers/swift/               the Swift wrapper: Sources, Tests, README
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
