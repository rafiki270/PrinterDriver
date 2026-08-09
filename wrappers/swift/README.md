# PrinterDriver for Swift

Receipt printing for iOS and macOS: hand the SDK a payload, get back a job whose event
stream tells the truth about what happened. No sockets, no chunking, no pacing, no ESC/POS
bytes, no status polling.

The Swift package is a **thin wrapper over the C ABI** in
[`capi/include/printerdriver/pd.h`](../../capi/include/printerdriver/pd.h) — enum bridging
and async adapters, nothing else. Every decision about protocol, confidence and completion
lives in the C++ core behind that header, so Swift, Kotlin and Dart apps all get the same
answers.

## Install

Swift Package Manager. The manifest is at the repository root, so the git URL is the
package URL:

```swift
// Package.swift
dependencies: [
  .package(url: "https://github.com/rafiki270/PrinterDriver.git", from: "0.1.0")
],
targets: [
  .target(name: "YourApp", dependencies: [
    .product(name: "PrinterDriver", package: "PrinterDriver")
  ])
]
```

In Xcode: **File → Add Package Dependencies…** and paste
`https://github.com/rafiki270/PrinterDriver.git`.

Until the first release tag is cut, point at the branch instead:
`.package(url: "https://github.com/rafiki270/PrinterDriver.git", branch: "main")`.

There are no external dependencies — the core is standard library, POSIX sockets and one
thread per printer.

## Platform matrix

| Platform | Minimum | Notes |
|---|---|---|
| iOS | 16.0 | device and simulator, arm64 + x86_64 |
| macOS | 13.0 | arm64 + x86_64; this is where `swift test` runs |
| Swift tools | 5.9 | builds with Swift 5.9 and later toolchains |
| C++ | C++17 | `core/src` and `capi/src` compiled by the same package |

## Quick start

```swift
import PrinterDriver

let driver = try PrinterDriver(storageDirectory: journalURL)
let kitchen = try driver.printer(.tcp(host: "192.168.1.101"), width: .dots576)

// Drop-in style: print a rendered receipt image under an order-derived key.
let job = try kitchen.print(.raster(receiptImage),
                            options: .init(key: "order-7F3A-92C1#kitchen-1"))

for await event in job.events {
    updateTicketUI(event.state, event.confidence)
}

switch await job.result {
case .done(let confidence, let authority):
    markTicketPrinted(confidence, authority)     // e.g. .cutFaultFree via .gsParenH
case .failed(let reason, _):
    showFailure(reason)                          // safe to resubmit the same key
case .unknown:
    askOperator(job)                             // forceReprint, or confirm by hand
}
```

Not in an `async` context? The same job, with closures:

```swift
try kitchen.print(.text(["MY RESTAURANT", "Order 7F3A-92C1"]),
                  options: .init(key: "order-7F3A-92C1#kitchen-1"),
                  onProgress: { event in updateTicketUI(event.state, event.confidence) },
                  completion: { result in resolve(result) })   // fires exactly once
```

## Three things worth knowing before you integrate

**1. The result is tri-state, and there is no `isSuccess`.** A job ends `.done`, `.failed`
or `.unknown`. `.unknown` means bytes went out and nothing came back — a completion
timeout, a dropped link, a crash. It is not a success and not a failure, and collapsing it
into either is precisely the bug that prints a second kitchen ticket. The compiler makes
you handle all three.

**2. The idempotency key is your duplicate defence.** Re-submitting a key that already has
a job does not print: `print` hands back that job, the same object, in whatever state it is
in — and it survives an app restart, because the driver reloads its journal. To print a key
again on purpose, call `forceReprint(key:)`, which marks the paper
(`*** REPRINT / POSSIBLE DUPLICATE ***`, `PRINT ATTEMPT: n`).

**3. Confidence is never inflated.** `.done` carries what the claim rests on:
`.cutFaultFree` on a `GS ( H` printer that acknowledged an ordered fence after the cut,
`.transportAccepted` on a write-only printer where the socket is the only evidence there
is. The `authority` on `.done` says which fence was available at all.

## Payload tiers

All three produce identical feedback. Pick per call, mix freely.

```swift
try Payload.raster(cgImage)                              // your renderer, our transport
try Payload.raster(grayscale: bytes, width: 576, height: 800)
Payload.text(["MY RESTAURANT", "Order 7F3A-92C1"])       // document tier, the common case
Payload.document(ops: [.align(.center), .bold(true), .line("TOTAL"), .feed(lines: 3)],
                 codePage: .pc852)
Payload.raw(escposBytes)                                 // escape hatch
```

Scaling to the printer's dot width, dithering, banding for tall images and pacing all
happen in the core. Raw payloads must not embed their own cuts or realtime status tricks:
the core owns job termination and its trailing fence assumes that.

## Threading contract

pd.h invokes its callbacks on the core's own threads. This wrapper never lets one of those
reach you.

* Every `AsyncStream` element, every `onProgress` call and every `completion` call is
  delivered on **one private serial queue per driver**. Ordering is therefore total: two
  observers of the same job see the same sequence, and the terminal notification always
  comes after the last event.
* It is **not** the main queue. Hop yourself for UI work — `await MainActor.run { … }` —
  which is deliberate: forcing every event through the main actor would serialise printing
  behind the interface.
* You may call straight back into the SDK from any handler.
* `job.events` replays what has happened so far before following the live events, so
  attaching late loses nothing. Awaiting `job.result` is safe from any number of tasks and
  at any time, including long after the job finished.
* Three calls block their calling thread and say so in their documentation:
  `Printer.drain()`, `Printer.refreshStatus(timeoutMilliseconds:)` and the driver's own
  teardown, which waits for in-flight jobs to reach a terminal state.

## Enums are closed, and the mirror is tested

`JobState`, `ConfidenceLevel`, `DeviceEvent`, `FailureReason` and the rest are defined once
in the C++ core, re-exported verbatim by `pd.h`, and mirrored here member for member. A
wrapper cannot add, drop or partially implement a value. If the core ever hands over a
value this wrapper does not know, the bridge traps in debug builds and substitutes a
documented, never-inflating fallback in release — it is never silently dropped.

`PrinterDriverTests.EnumBridgeTests` checks the Swift mirror against `pd.h`'s `_COUNT`
constants, against the core's own member lists, and against `pd::to_string`'s spellings.

## Running the tests

From the repository root, on macOS:

```sh
swift build
swift test
```

The tests drive the real C++ engine over the real C ABI and swap only the socket, for an
in-process scripted device supplied by `capi/tests/pd_test_support.cpp`. That file is
compiled into the test target and into nothing else, so no application binary carries a
test double.

## Parity with the other wrappers

`scripts/check_parity.sh` (docs/api.md §17) asserts that this wrapper references every
public `pd_*` function in `capi/include/printerdriver/pd.h`, and that anything it covers
through a higher-level member instead is named in `scripts/parity_allowlist.txt`. **No
wrapper is a subset**: a `pd_` function added to the ABI without a binding here fails CI.

## License

MIT. See [LICENSE](../../LICENSE).
