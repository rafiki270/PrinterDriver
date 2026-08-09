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
addon, and four wrappers — Swift (iOS 16+/macOS 13+, verified), Dart (pub-ready,
verified), .NET (NuGet-ready, verified on macOS against the real native library),
Kotlin/Android (scaffold, CI-pending). Every job result carries a confidence grade, a
completion authority and the method that produced it.

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

The **receipt DSL** ([docs/receipt-dsl.md](docs/receipt-dsl.md)) is built as its own
library, `printerdriver_dsl`: the document model with a hand-written strict-JSON subset,
named styles with inheritance, template binding (`{{path}}`, `each`, `if`/`unless`,
locale-aware formatters), and the hardware-path renderer that lays columns out against
the printer's characters-per-line. Every departure from what a document asked for — an
italic the hardware cannot do, a barcode this milestone does not draw, a model path that
is not there — comes back in a render report rather than disappearing. `pdctl render`
prints that report and a character approximation of the paper without touching a printer.
Code 128 (with automatic B/C subset optimisation) and EAN-13/EAN-8 render as real `GS k`
symbols with HRI positioning; symbologies beyond those keep the declared degradation.
Every cut the renderer emits — mid-document as well as trailing — feeds the profile's
blade clearance first, so a multi-ticket document cannot clip its own content.

**`pd-agent`** ([agent/README.md](agent/README.md)) is the same core compiled as the
local printer agent of [docs/techspec.md §5](docs/techspec.md): a small HTTP/1.1
submission API in front of **one** `PrinterDriver`, which is what makes the same order
key from two tills print once. Every response is an evidence document — state, fence,
cutter and paper status, grade, authority, method and the verification token on the
paper — never a bare success flag.

**LAN discovery** lives in the core (`discovery.hpp`, `pdctl discover`). The only bytes
it ever writes are `DLE EOT 1` (`10 04 01`): a port-9100 device prints what it receives,
so a scan that sent anything printable would cost a venue a roll of paper per run.

Not built yet: Bluetooth/USB/serial transports, the ePOS and StarPRNT transports (their
profiles are data only — Star printers refuse honestly instead of printing unfenced),
Bluetooth discovery, and the DSL's raster path (wrapper-side text rendering, plus the
2-D and remaining linear symbologies).
**Bluetooth** goes through a custom-transport ABI rather than into the core
([docs/compatibility-brief.md](docs/compatibility-brief.md) §25): the platform owns the
socket — CoreBluetooth or ExternalAccessory on Apple, `BluetoothSocket` RFCOMM on
Android, BlueZ on Linux — and the core owns the protocol, so a Bluetooth job gets the
same fence, the same correlated token and the same grade a TCP job gets, and a wrapper
cannot weaken a completion guarantee because a wrapper never makes one. The Apple and
Android implementations are written and unit-tested against scripted transports; neither
has been run against a paired printer, and the BlueZ transport is syntax-checked only
(`scripts/check_linux_bluetooth_syntax.sh`).

Not built yet: USB and serial transports, the ePOS and StarPRNT transports (their
profiles are data only — Star, Zebra and Brother printers refuse honestly instead of
printing unfenced), network discovery in the core, and the DSL's raster path
(wrapper-side text rendering and barcode symbologies).

The **Windows port** ([docs/platforms.md](docs/platforms.md)) is written but not built:
the Winsock2 transport and the `FlushFileBuffers`/`ReplaceFile` journal path are
syntax- and type-checked only (`scripts/check_windows_syntax.sh`, which runs negative
controls proving the check can fail), and `.github/workflows/windows.yml` is
manual-dispatch and has not been dispatched. The POSIX build is untouched by it — the
two are alternative translation units, never both.

## Build and test

Requires CMake 3.16+ and a C++17 compiler. There are no third-party dependencies and
nothing is downloaded during configuration.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The build produces the static libraries `printerdriver_core`, `printerdriver_queue`,
`printerdriver_dsl` and `printerdriver_agent`, plus the `pdctl` diagnostic CLI and the
`pd-agent` daemon; public headers live in `core/include/printerdriver/`,
`queue/include/printerdriver/`, `dsl/include/printerdriver/` and
`agent/include/printerdriver/`. Tests are plain executables using a small built-in assert
harness (`core/tests/test_harness.hpp`) and are registered individually with CTest.

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

## .NET package

[wrappers/dotnet/](wrappers/dotnet/) is a NuGet-ready C# wrapper (`PrinterDriver`,
net8.0, MIT) over the same C ABI: P/Invoke, the twenty-one enums mirrored and asserted
against the core's own member counts, a closed `Done`/`Failed`/`Unknown` record with no
boolean anywhere in it, `IAsyncEnumerable` event streams and `GCHandle`-rooted native
callbacks.

```sh
cmake -S . -B build-dotnet -DPD_BUILD_SHARED_CAPI=ON
cmake --build build-dotnet --target printerdriver_capi_testing
dotnet test wrappers/dotnet/PrinterDriver.Tests
```

The managed side is verified on macOS against the real native library;
[wrappers/dotnet/README.md](wrappers/dotnet/README.md) states exactly what is verified,
what is syntax-checked, and what a Windows CI run must still confirm.

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
| A+ | a durable, queryable printer-side job | ePOS JobID with a retrievable result |
| A | job-level confirmation from the mechanism | `GS ( H` echo, Star checked block |
| B | ordered device response, weaker semantics | `GS r 1` |
| C | device status around transmission | DLE EOT, ASB |
| D | a spooler said completed | CUPS, Windows spooler |
| E | transport only | TCP write succeeded |

Nothing produces A+ yet — it needs the ePOS transport, which does not exist here. The
grade is defined because these enums are closed and mirrored by four wrappers, so adding
a member later would renumber every mirror a second time.

Every capability in the device database also carries its **provenance**
([docs/compatibility-brief.md](docs/compatibility-brief.md) §28): `Documented` where the
manufacturer's own command documentation lists it, `Probed` where this driver asked the
hardware and it answered, `Unverified` where neither. Epson is the only family whose
shipped defaults claim Documented — it publishes a model-by-model ESC/POS applicability
database naming `GS ( H` fn 48. "ESC/POS compatible" on anybody's datasheet is
Unverified, and `pdctl probe` prints the documentation and the probe as two separate
columns because either can say yes while the other says no.

## pdctl

```sh
build/pdctl discover   [cidr]                 # sweep a subnet (default: the local /24)
build/pdctl autodetect [cidr]                 # sweep + identify + printless probe
build/pdctl status   <host>                   # DLE EOT 1-4 decoded plus raw bytes
build/pdctl probe    <host> [--mac <address>] # full printer discovery report
build/pdctl identify <host> [--mac <address>] # fingerprint only, prints nothing
build/pdctl print    <host> --text "..." --key order-7F3A
build/pdctl print list                        # the device database
```

`discover` writes exactly three bytes per address — `DLE EOT 1` — and reports each open
port together with whatever came back. Silence is a finding, not a failure: it is the LAN
module that does not forward status bytes, and it is why the table separates "open" from
"answers".

`autodetect` composes `discover`, `identify` and the **printless** subset of the
capability probe into one table — ip, vendor guess, model, trusted, profile, completion,
grade ceiling, provenance — and still writes nothing printable. That restriction has a
price it states on every run: an ordered fence only means anything when there is print
data ahead of it, so a fence found here proves the command *exists* and not that its
answer waits for paper. `pdctl probe`, which prints, or a real job is what promotes it.

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
build/pdctl self-test  <host> [--profile <name>] # one diagnostic ticket, consumes paper
build/pdctl settings   <host>                    # GS ( E fn 4 / fn 6 readback
build/pdctl drawer test <host> [--channel 1|2] [--pulse 200]  # opens the cash drawer
```

The cash drawer splits the same way, for a reason
[docs/cash-drawer.md](docs/cash-drawer.md) prints in giant letters: **RJ11/RJ12-looking
drawer connectors are not a universal electrical standard.** Star's identical-looking
6P6C socket carries +24 V on pin 3 and the sense line on pin 6, exactly where Epson puts
sense and signal ground, and 12 V outputs exist alongside the common 24 V ones. So
`drawer-probe` is read-only and safe on hardware nobody has classified, and `drawer test`
refuses outright on an unclassified port:

```sh
build/pdctl drawer-probe <host> [--profile <name>]   # never fires an output
```

`self-test` is the other half of `test-print`, and the two are deliberately different
documents: `test-print` asks the firmware to print its own status sheet, and `self-test`
prints the SDK's view of the unit through the ordinary fenced engine — identity, profile
and how it was selected, media, completion mechanism with its grade ceiling and
provenance, the drawer classification, a Czech/Hungarian/Polish charset line, a Code 128
sample and the job's own `V:` token in the trailer QR. Anything the profile cannot draw is
printed as a declared degradation instead of being dropped. The ticket is the report and
the terminal result is the proof: a `Done` at grade A is the statement that the whole
stack works end to end on that unit, over that interface path.

It prints the documented port (standard, voltage, current, channels, sense pin) and the
software provenance as two separate columns, then runs a non-destructive switch test —
close the drawer, read; open it by hand, read — and persists which level means OPEN,
because Star documents that the meaning of that signal depends on the drawer that is
plugged in. Until that calibration exists, a sensor reading reports a level and no
interpretation.

## Layout

```
core/include/printerdriver/   public headers
  types.hpp                   closed enums, JobResult, JobEvent
  escpos_encoder.hpp          byte builder and the fence/status primitives
  response_parser.hpp         incremental parser for the interleaved return stream
  capability_profile.hpp      compositional profile: identity, transport, completion,
                              status, recovery, quirks, media
  device_profiles.hpp         the device database: 83 entries across Epson, Star,
                              Bixolon, Citizen, Rongta, Xprinter, Partner, Zebra and
                              Brother, per family and per model
  identity.hpp                GS I parsing, MAC OUI table, multi-signal identify()
  capability_probe.hpp        non-destructive interrogation, findings, promotion,
                              and the findings cache
  job_store.hpp               append-only durable job journal
  transport.hpp               Transport interface and the TCP implementation
  discovery.hpp               LAN sweep: DLE EOT 1 only, never a printable byte
  transport.hpp               Transport interface, the TCP implementation, and the
                              embedder-owned custom transport Bluetooth arrives through
  transport_bluez.hpp         Linux BlueZ RFCOMM (syntax-checked only)
  cash_drawer.hpp             the drawer as a separate peripheral: electrical
                              classification, kick and status methods, the drawer state
                              machine, and the persisted polarity calibration
  driver.hpp                  PrinterDriver / Printer / PrintJob — the public API
core/src/                     implementation
core/tests/                   test harness, scriptable fake printer, test binaries
capi/include/printerdriver/   pd.h — the C ABI every wrapper binds, plus its modulemap
capi/src/                     the ABI implementation and its enum static_asserts
capi/tests/                   the C ABI test, the scripted-device factory, the enum bridge
queue/                        the print-queue addon (separate library on purpose)
dsl/                          the receipt DSL: document model, binding, renderer,
                              barcode symbologies
agent/                        pd-agent: the local printer agent and its HTTP/1.1 front
                              end, with its own README
Package.swift                 SwiftPM manifest for the whole repository
wrappers/swift/               the Swift wrapper: Sources, Tests, README
wrappers/dart/                the Dart FFI package (pub-ready)
wrappers/dotnet/              the .NET wrapper: library, tests, README (NuGet-ready)
wrappers/android/             the Kotlin/Android AAR scaffold (CI-pending)
tools/pdctl.cpp               command-line diagnostics
docs/                         specifications — these are authoritative, not the code
scripts/printer_probe.py      standalone hardware capability probe
```

## Threads

One worker thread per printer runs jobs strictly FIFO, one at a time. Each open
connection has its own reader thread pumping received bytes into the response parser.
Job-event and device-event callbacks run on those threads and must not block or call
back into the driver. `pd-agent` adds one accept thread plus a small fixed pool of
connection workers, so a request waiting on a completion fence never stops the agent
answering another printer's.

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

## Supported functionality

✅ shipped & tested · 🔄 in build · 📋 specced (docs are normative)

- ✅ Fenced print completion (`GS ( H` / `GS r 1`) with tri-state results, evidence
  grades A+–E, and completion authority — never a bare success boolean
- ✅ Idempotency keys, explicit force-reprint with toggleable banner, duplicate
  prevention; durable journal with crash recovery to honest `Unknown`
- ✅ Receipt verification identifiers: `V:` token printed + QR'd, `jobByToken`,
  `pdctl verify` (paper → full evidence record)
- ✅ Compositional capability profiles, multi-signal identification (GS I untrusted),
  non-destructive probe-then-promote with provenance (documented/probed/unverified)
- ✅ Print-queue addon (hold/drain, TTL, priority, lane-blocking on Unknown)
- ✅ Receipt DSL: JSON documents, templates + bound models, formatters, columns,
  margins, cut control, declared degradation; `pdctl render` preview
- ✅ Wrappers: Swift (iOS+macOS), Dart (pub-ready), .NET (NuGet-packed); Kotlin/Android
  scaffold (CI-pending); iOS example app (ReceiptStudio)
- ✅ Cash drawer as a separate peripheral capability: electrical classification
  (Epson/Star/12 V/unknown 6P6C), per-family kick method and cooldown, the verified
  opening sequence (`GS r 2` → `ESC p` → watch the switch) reporting `OPEN_VERIFIED` /
  `FAILED_TO_OPEN` / `KICK_SENT_UNVERIFIED`, persisted polarity calibration, and a
  refusal — zero bytes — on any port nobody has classified
- ✅ Self-test and auto-detection: one fenced diagnostic ticket that IS the detection
  report, and a non-printing sweep (discovery → identify → printless probe) that
  classifies every candidate as answered / silent / unverified / unreachable and refuses
  to promote a fence it could only ask out of an empty buffer
- ✅ LAN discovery and auto-detection in every wrapper — Swift `AsyncStream`, Dart
  `Stream`, .NET `IAsyncEnumerable`, Kotlin `Flow` — not only in the CLI
- ✅ pdctl: status · probe · identify · print · verify · render · counters · settings ·
  test-print · self-test · autodetect · recover · drawer-probe · drawer test
- 🔄 Provenance-in-code, A+ grade, Bluetooth custom-transport ABI, expanded catalogue
  (M12) · pd-agent daemon, LAN discovery, DSL barcodes (M13a)
- 📋 ePOS A+ transport, Star raw (ETB / ESC GS ETX / CloudPRNT), serial, probe-path,
  BLE heuristics (M13b) · Windows native CI

## Supported printers

| Family | Completion path | Status |
|---|---|---|
| **Xprinter XP-S260M** | `GS ( H` | ⭐ **hardware-verified** (probe + 100-receipt soak) |
| Epson TM-T20/T70/T82/T88 IV–VII, m-series, P20II/P80II, U220 | `GS ( H` (manufacturer-documented); ePOS A+ on spooler models | documented |
| Star TSP100/TSP650/mC-Print, SM portables | ETB / ESC GS ETX / CloudPRNT (specced), StarPRNT | queued (M13b); refuses honestly today |
| Bixolon SRP-330/350/380/Q/F310, SPP-R mobiles | per-model manuals; `GS ( H` never assumed | profile per model |
| Citizen CT-S/CT-E desktop, CT-S4500 wide, CMP portables | `ESC p`/`GS r` documented; probe fences | documented |
| Rongta RP80/RP3xx/RP58 | `GS ( H` documented-provisional (mirror manuals) | probe-first |
| Partner RP-110 / Sewoo SLK-TS200 | probe all | probe-first |
| Generic ESC/POS 80/58 mm | `GS r 1` conservative → probe-promoted | supported |
| Zebra ZQ (ZPL/CPCL), Brother RJ (raster/ESC-P) | non-ESC/POS | refuse honestly, zero bytes |
