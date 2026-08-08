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

Built and tested: the full C++ core (encoder, interleaved response parser, durable job
store, TCP transport, fenced print engine), printer identification with compositional
capability profiles and probe-then-promote, the device database from
[docs/device-database.md](docs/device-database.md), the C ABI (`pd.h`), the print-queue
addon, and three wrappers — Swift (iOS 16+/macOS 13+, verified), Dart (pub-ready,
verified), Kotlin/Android (scaffold, CI-pending). Every job result carries a confidence
grade, a completion authority and the method that produced it.

Identity is untrusted by default. `GS I` is a string the firmware chooses, and at least
one printer family ships answering as somebody else's model
([docs/capability-profiles.md](docs/capability-profiles.md)), so identification combines
MAC OUI, the reported strings and observed command behaviour.

**Hardware-verified**, not just unit-tested: the reference Xprinter XP-S260M was probed
(`GS ( H` confirmed), then soak-tested with 100 real receipts — during which the paper
ran out mid-run and the engine classified every outcome honestly (86 confirmed, one
`unknown`, two preflight refusals with zero bytes sent, circuit-breaker stop, zero
duplicates after resume). Two real defects found by that hardware testing — the cutter
slicing trailing content, and journal-reloaded jobs losing their evidence grade — are
fixed with regression tests.

Not built yet: Bluetooth/USB/serial transports, the ePOS and StarPRNT transports (their
profiles are data only — Star printers refuse honestly instead of printing unfenced),
network discovery in the core, the receipt-DSL renderer
([docs/receipt-dsl.md](docs/receipt-dsl.md) is specified), and the Windows port
([docs/platforms.md](docs/platforms.md)).

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

```swift
import PrinterDriver

let driver  = PrinterDriver(storage: .default)
let kitchen = driver.printer(.tcp(host: "192.168.1.101"), width: .dots576,
                             profile: "xprinter_s_series")
let job = try kitchen.print(.text(["Order 7F3A", "2x Pilsner"]),
                            options: .init(key: "order-7F3A#kitchen-1"))

switch try await job.result {                 // tri-state — there is no isSuccess
case .done(let confidence, let authority):    // e.g. .cutFaultFree from the printer itself
  markPrinted(confidence, authority)
case .failed(let reason, _):                  // known failure, zero or safe bytes sent
  showFailure(reason)
case .unknown:                                // sent, unacknowledged — operator decides
  askOperator(job)
}
```

## Dart package

[wrappers/dart/](wrappers/dart/) is a pub-ready pure-Dart package (`printerdriver`) with
hand-written FFI bindings over the same C ABI, a sealed tri-state `JobResult`, and strict
separation between the shipped native library and the test-only scripted-device build.

```sh
cmake -S . -B build-dart -DPD_BUILD_SHARED_CAPI=ON
cmake --build build-dart -j --target printerdriver_capi_testing
cd wrappers/dart && dart test
```

## Android package

[wrappers/android/](wrappers/android/) is a Maven-publishable AAR scaffold
(`com.printerdriver:printerdriver`) — Gradle + externalNativeBuild over the same core,
JNI glue syntax-checked against `pd.h`, honest README stating exactly what a CI run on a
JVM host must still confirm.

## Example app

[examples/ios/ReceiptStudio](examples/ios/ReceiptStudio) (branch `example-ios`) is a
SwiftUI app that scans the network for port-9100 candidates, remembers printers, runs
`Identify`, offers a block-based receipt designer that serializes to the
[receipt-dsl](docs/receipt-dsl.md) JSON, and prints through the full fenced engine with
the honest tri-state status sheet.

## Evidence, not success

Every result names what backs it ([docs/device-database.md](docs/device-database.md)):

| Grade | Meaning | Example |
|---|---|---|
| A | job-level confirmation from the mechanism | `GS ( H` echo, ePOS JobID |
| B | ordered device response, weaker semantics | `GS r 1` |
| C | device status around transmission | DLE EOT, ASB |
| D | a spooler said completed | CUPS, Windows spooler |
| E | transport only | TCP write succeeded |

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
capi/include/printerdriver/   pd.h — the C ABI every wrapper binds, plus its modulemap
capi/src/                     the ABI implementation and its enum static_asserts
capi/tests/                   the C ABI test, the scripted-device factory, the enum bridge
queue/                        the print-queue addon (separate library on purpose)
Package.swift                 SwiftPM manifest for the whole repository
wrappers/swift/               the Swift wrapper: Sources, Tests, README
wrappers/dart/                the Dart FFI package (pub-ready)
wrappers/android/             the Kotlin/Android AAR scaffold (CI-pending)
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
- [docs/receipt-dsl.md](docs/receipt-dsl.md) — the serializable receipt document model:
  blocks, styles, templates with bound parameter models, formatters, cut control.
- [docs/platforms.md](docs/platforms.md) — platform matrix, the Windows path, and why the
  SDK bypasses OS print spoolers on purpose.

## License

MIT — see [LICENSE](LICENSE).
