# PrinterDriver

A cross-platform SDK for receipt printers (ESC/POS and friends) with real
print-completion feedback. One portable C++17 core does all the work — encoding,
transports, status parsing, job tracking — with thin native wrappers for Swift, Kotlin,
Dart and .NET. Zero third-party dependencies.

Instead of "the bytes were sent", a print job returns `done`, `failed`, or `unknown`,
together with the evidence behind the answer (e.g. the printer's own completion
acknowledgement). Duplicate prevention, receipt verification codes, cash-drawer
control, printer auto-detection and a print-server daemon are built in.

## Installation

**Swift (iOS 16+ / macOS 13+)** — the repo is the SwiftPM package:

```swift
.package(url: "https://github.com/rafiki270/PrinterDriver", branch: "main")
```

**Dart/Flutter** — [wrappers/dart](wrappers/dart) (pub-ready package `printerdriver`).

**.NET 8** — [wrappers/dotnet](wrappers/dotnet) (`dotnet pack` produces the NuGet).

**Android** — [wrappers/android](wrappers/android) (Gradle AAR module, `com.printerdriver:printerdriver`).

**C / C++** — build with CMake; link `printerdriver_core` or the C ABI
(`capi/include/printerdriver/pd.h`):

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build        # run the test suite
```

## Quick start

```swift
import PrinterDriver

let driver  = PrinterDriver(storage: .default)
let printer = driver.printer(.tcp(host: "192.168.1.101"), width: .dots576,
                             profile: "xprinter_s_series")

let job = try printer.print(.text(["Order 7F3A", "2x Pilsner"]),
                            options: .init(key: "order-7F3A#kitchen-1"))

switch try await job.result {
case .done(let confidence, _): print("printed — \(confidence)")
case .failed(let reason, _):   print("did not print: \(reason)")
case .unknown:                 print("sent, unconfirmed — ask the operator")
}
```

Same API shape in Dart, C#, Kotlin and C — see each wrapper's README. Receipts can
also be built from JSON templates with bound data models
([docs/receipt-dsl.md](docs/receipt-dsl.md)), sent as rendered raster images, or as raw
ESC/POS bytes. Resubmitting the same `key` never prints twice; deliberate reprints are
marked on the paper.

**Find and check a printer from the terminal:**

```sh
build/pdctl autodetect              # scan the network, identify, classify
build/pdctl self-test 192.168.1.101 # print a diagnostic ticket via the full pipeline
build/pdctl print 192.168.1.101 --text "Hello" --key demo-1
build/pdctl verify <V-code>         # look up the job behind a printed receipt
```

**Run it as a print server** (one owner per printer, shared by many tills):

```sh
build/pd-agent --store /var/lib/pd --port 8880
curl -X POST localhost:8880/jobs -d '{"printerId":"...","payload":{"text":"..."},"key":"order-1"}'
```

## Supported printers

| Family | Notes |
|---|---|
| **Xprinter XP-S260M** | fully verified on real hardware (probe + 100-receipt soak) |
| Epson TM-T20/T70/T82/T88 IV–VII, m-series, P20II/P80II, U220 | completion commands manufacturer-documented; ePOS JobID transport for spooler models |
| Star TSP100/TSP650/mC-Print | prints via Star Line Mode with session fences (ESC GS ETX / ETB) |
| Bixolon SRP series + SPP-R mobiles | per-model profiles |
| Citizen CT-S/CT-E desktop, CT-S4500 wide, CMP portables | documented command set |
| Rongta RP80/RP3xx/RP58 | supported; completion capability confirmed by probe |
| Partner RP-110 / Sewoo SLK-TS200 | supported; probe recommended |
| Generic ESC/POS (80/58 mm) | conservative defaults, auto-upgraded after a probe |
| Zebra ZQ, Brother RJ | not ESC/POS — detected and reported, never driven blind |

Unknown printers are probed automatically: profiles supply defaults, a one-time
capability probe measures what the device actually supports.

## Supported features

- Print-completion feedback with evidence grades A+–E (see below)
- Text, styled documents, JSON templates with data binding, raster images, raw bytes
- Idempotency keys, duplicate prevention, marked reprints
- Printed verification codes (`V:XXXX`) — look up any physical receipt's job record
- Printer identification and capability probing (`GS I` cross-checked, results cached)
- Network discovery and one-call auto-detection; printed self-test ticket
- Transports: TCP 9100, serial RS-232, Bluetooth (custom-transport API + Android
  RFCOMM / iOS bridges), Epson ePOS, Star CloudPRNT
- Cash drawers: kick + sensor-verified opening, electrical classification, calibration
- Print queue add-on: hold while offline, TTL expiry, priorities
- `pd-agent` HTTP print server; `pdctl` CLI for everything above
- Cutter control, margins, code pages (CP852 for Czech/Hungarian/Polish), QR, Code128,
  EAN-13/8, paper/cover/cutter status events

## How completion feedback works

The core places an ordered marker behind each receipt; the printer echoes it only after
the print mechanism has finished, so completion is confirmed by the device, not
inferred from a successful socket write. Each result carries a grade naming the
strength of that evidence:

| Grade | Backed by |
|---|---|
| A+ | durable printer-side job id (Epson ePOS) |
| A | device completion echo (`GS ( H`, Star fences) |
| B | ordered status response (`GS r 1`) |
| C | device status around transmission (DLE EOT, ASB) |
| D | a spooler/print server said completed |
| E | transport write succeeded — nothing more |

`unknown` results (sent but unconfirmed — crash, timeout, dead link) are reported as
such and are never retried automatically; a resend is an explicit operation.

## Documentation

- [docs/api.md](docs/api.md) — object model, payload tiers, options
- [docs/receipt-dsl.md](docs/receipt-dsl.md) — receipt documents, templates, formatters
- [docs/techspec.md](docs/techspec.md) — the ESC/POS protocol detail behind the fences
- [docs/compatibility-brief.md](docs/compatibility-brief.md) — per-model command research
- [docs/device-database.md](docs/device-database.md) — printers, media, print servers
- [docs/wire-protocols.md](docs/wire-protocols.md) — ePOS, Star, CloudPRNT, BLE details
- [docs/cash-drawer.md](docs/cash-drawer.md) — drawer electrics, commands, verification
- [docs/platforms.md](docs/platforms.md) — platform matrix and the Windows port
- [docs/sdk-spec.md](docs/sdk-spec.md), [docs/brief.md](docs/brief.md) — design and
  original research

Example app: [examples/ios/ReceiptStudio](examples/ios/ReceiptStudio) — SwiftUI printer
scanner + receipt designer.

## Development

C++17, CMake 3.16+, no dependencies. `ctest` runs the C++ suites; `swift test`,
`dart test` and `dotnet test` cover the wrappers (see each wrapper's README for the
one-line setup). The Android module and the Windows build are CI-ready but need their
respective toolchains.

## License

MIT — see [LICENSE](LICENSE).

---

**Ondrej Rafaj** · [rafiki270](https://github.com/rafiki270)
