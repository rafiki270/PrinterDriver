# printerdriver-react-native

React Native / Expo bindings for the [PrinterDriver](../../README.md) receipt-printing SDK.

Instead of "the bytes were sent", a print job returns `done`, `failed`, or `unknown`,
together with the evidence behind the answer. One portable C++17 core does all the work —
encoding, transports, status parsing, job tracking, the ordered completion fences — and
this package is a thin JSI layer over the same `pd.h` C ABI the Swift, Kotlin, Dart and
.NET wrappers bind. No printing logic lives in JavaScript.

> **Read [Verification status](#verification-status) before you rely on this.** The
> TypeScript layer is tested and the C++ module is type-checked against the real ABI, but
> no React Native app has ever loaded this package. Nothing here has printed a receipt.

---

## Requirements

- **React Native 0.71+ with the New Architecture enabled.** The module is a C++
  TurboModule; the old architecture cannot load it, and its JSON bridge would copy every
  raster payload twice anyway.
- **iOS 16+ / Android minSdk 24.**
- **Expo: a development build.** Expo Go cannot load this package — it contains native
  code. `npx expo prebuild` + `npx expo run:ios` / `run:android`, or EAS Build. That is
  the normal path for any native module.

## Install

```sh
npm install printerdriver-react-native
```

**iOS** — autolinking picks up the podspec:

```sh
cd ios && pod install
```

**Android** — nothing to do; autolinking adds the Gradle module. Rebuild the app (the
native core is compiled from source, so a Metro reload is not enough).

**Expo** — add the config plugin, then make a development build:

```json
{
  "expo": {
    "plugins": [
      ["printerdriver-react-native", {
        "localNetworkUsageDescription": "Connects to your receipt printers."
      }]
    ]
  }
}
```

The plugin sets `NSLocalNetworkUsageDescription` (iOS 14+ refuses local-network
connections without it, and a port-9100 printer is on the local network) and enables the
New Architecture on both platforms. It deliberately does **not** request Bluetooth
permissions: this package does not own a Bluetooth stack — you supply one through a
[JavaScript transport](#bluetooth-and-other-app-owned-links) — so those strings belong to
whichever BLE library you chose.

## Quick start

```ts
import { PrinterDriver, Payloads } from 'printerdriver-react-native';

const driver = PrinterDriver.create({ storageDirectory: `${DocumentDirectoryPath}/pd` });
const printer = driver.addPrinter({
  host: '192.168.1.101',
  widthDots: 576,
  profileId: 'xprinter_s_series',
});

const job = printer.print(Payloads.text(['Order 7F3A', '2x Pilsner']), {
  key: 'order-7F3A#kitchen-1',
});

const result = await job.result;
switch (result.outcome) {
  case 'done':    markPrinted(result.confidence, result.grade); break;
  case 'failed':  showFailure(result.reason); break;         // safe to resubmit same key
  case 'unknown': askOperator(job); break;                   // NOT success, NOT failure
}
```

There is no `success` boolean anywhere in this package, and a test enforces that. A
completion-wait timeout ends in `unknown`, not `failed`: bytes were sent and the receipt
may well be printing. Collapsing that into either bucket is exactly the bug that produces
duplicate kitchen tickets ([docs/api.md §4](../../docs/api.md)). Under `strict` the
compiler makes the `switch` exhaustive for you.

**Pass a `storageDirectory`.** Without one the driver is in-memory: no journal, no crash
recovery, no persisted verification tokens, and `driver.findJob(key)` after a relaunch
finds nothing.

### Watching a job

```ts
job.events.on(event => setTicketState(event.state, event.confidence));

// or, equivalently, over the same buffer:
for await (const event of job.events) { … }   // ends with a terminal event, always
```

### Devices, not polling

```ts
printer.events.on(event => {          // 'paperOut' | 'coverOpen' | 'offline' | …
  if (event === 'paperOut') alertStaff();
});
const snapshot = printer.status();    // never a live query, so it cannot block a print
snapshot.paperNearEnd;                // 'yes' | 'no' | 'unknown' — never a silent false
```

## API surface

Everything the C ABI exposes is reachable from this package. `test/parity.test.ts` checks
that mechanically, at three levels: every `pd_*` function in `pd.h` has a TurboModule
method, the spec really declares it, and `cpp/PrinterDriverModule.cpp` really calls it
([docs/api.md §17](../../docs/api.md)).

| Area | Surface |
|---|---|
| Driver | `PrinterDriver.create(config)`, `lastError`, `profileIds()`, `instanceNonce`, `localSubnet`, `destroy()` |
| Printers | `addPrinter(tcpConfig)`, `printer(host, width, profile)`, `addCustomPrinter(transport)`, `id`, `widthDots`, `completion`, `completionProvenance`, `language`, `drain()` |
| Printing | `printer.print(payload, options)`, `printer.send(payload, {onProgress})`, `printer.forceReprint(key, {banner})` |
| Payload tiers | `Payloads.text([...])`, `Payloads.document(new Receipt()…)`, `Payloads.raster({pixels, width, height})`, `Payloads.raw(bytes)` |
| Jobs | `id`, `key`, `printToken`, `cutToken`, `attempt`, `state`, `confidence`, `isTerminal`, `events`, `result`, `awaitResult(timeoutMs)` |
| Job lookup | `driver.findJob(key)`, `driver.jobByToken('K73F')` — paper → job |
| Status | `printer.status()`, `printer.refreshStatus(timeoutMs)`, `printer.events` |
| Cash drawer | `drawerCapabilities()`, `openDrawer({channel, pulseMs})`, `readDrawerSensor()`, `calibrateDrawerPolarity(highMeansOpen)`, `drawerPolarityCalibrated`, `drawerHighMeansOpen`, `openCashDrawer()` |
| Queue | `driver.createQueue(policy)`, `enqueue`, `pause`, `resume`, `isPaused`, `isBlocked`, `unblock`, `pending`, `expiredCount`, `overflowCount`, `drainedCount`, `tick`, `destroy` |
| Detection | `printer.selfTest(options)`, `driver.autoDetect(options)`, `driver.discover(options)`, `detectedAt`, `discoveredAt` |
| Extension | `registerCompletionMethod`, `registerProbeStep`, `registerBlockHandler`, `registerFormatter`, `registerDrawerKick` |
| Transports | `CustomTransport` (`connect`/`write`/`close`), `printer.feedBytes(bytes)`, `printer.reportLinkDropped(message)` |
| Enums | 26 closed enums as `const` objects plus string-literal unions, mirrored from `pd.h` and checked by a test |

### Job options

```ts
printer.print(payload, {
  key: 'order-7F3A#kitchen-1',   // idempotency key; resubmitting it does NOT print again
  cut: 'partial',                // 'profile' (default) | 'partial' | 'full' | 'none'
  openDrawer: false,
  preflight: 'strict',           // refuses on cover-open / paper-out
  timeoutMs: 0,                  // 0 → the profile's own completion budget
  topFeedDots: 0,
  bottomFeedDots: 96,            // TOTAL clearance before the cut; can only add whitespace
  printVerificationId: true,     // the `V:` trailer line and QR (docs/api.md §14)
});
```

Re-submitting an existing `key` returns the existing job rather than printing. A
deliberate duplicate goes through `forceReprint`, which prints the
`*** REPRINT / POSSIBLE DUPLICATE ***` banner unless you pass `banner: false` — a
per-call, deliberate act for a customer copy. A kitchen ticket should never disable it.

### Verification identifiers

Every job on a `GS ( H` printer carries a four-character token that the printer echoes at
physical print completion, printed on the ticket as `V:`:

```ts
const job = driver.jobByToken('K73F');   // the receipt in an operator's hand → the job
job?.printToken;                         // 'K73F'
```

### Self-test and auto-detection

```ts
const { candidates, done } = driver.autoDetect();   // nothing prints, nothing fires
candidates.on(found => addToSettingsList(found));
const all = await done;

const test = await printer.selfTest();              // prints exactly ONE diagnostic ticket
test.result.outcome;                                // 'done' at grade A ⇒ the stack works
test.ticketText;                                    // the ticket exactly as it was laid out
```

`discover()` is the raw sweep underneath: the only bytes it writes are `DLE EOT 1`, every
one of them below `0x20`, so none of it can print on any device.

### Bluetooth and other app-owned links

The platform owns the socket; the core owns the protocol. Supply three operations and push
received bytes in, and the fence, the correlation token, preflight, the journal and the
confidence grading all behave exactly as they do over TCP:

```ts
const printer = driver.addCustomPrinter({
  description: 'bt-spp:00:11:22:33:44:55',
  connect: async () => myBle.connect(deviceId),
  write:   async (data) => myBle.write(deviceId, data),   // return the count ACTUALLY sent
  close:   async () => myBle.disconnect(deviceId),
});

myBle.onData(deviceId, bytes => printer.feedBytes(bytes));
myBle.onDisconnect(deviceId, () => printer.reportLinkDropped('out of range'));
```

Report short writes honestly. Zero bytes out is a known failure and one byte out is
`unknown`, and that difference decides whether an operator should reprint.

## Threading contract

Core callbacks arrive on printer worker threads and transport reader threads. The native
module hops every one of them onto the JS thread through the TurboModule `CallInvoker`
before it reaches your code, so **every listener, transport callback and registration
callback in this package runs on the JS thread** and may safely touch React state.

Callbacks that the core needs an **answer** from — a transport's `connect`/`write`/`close`,
a registered completion method's fence and matcher, a probe step's `classify`, a block
handler, a formatter, a vendor drawer kick — block a core thread until your handler
settles. They may be `async`. If one throws, rejects, or never settles, it is answered
after a deadline with **that registration's own documented failure** — a short write, a
`notMine` verdict, a declined formatter, a degraded block — never with an invented success,
and never by hanging the printer.

That arrangement is only safe because of one invariant, which is why so much of this API
returns a `Promise`:

> **No blocking `pd_*` call ever runs on the JS thread.** `destroy`, `drain`,
> `refreshStatus`, `awaitResult`, `openDrawer`, `readDrawerSensor`, `selfTest`,
> `autoDetect`, `discover` and `queue.destroy` all run on a worker thread inside the native
> module. If any of them ran inline, a JS thread parked inside the ABI could be exactly the
> thread a core worker is waiting on, and the two would deadlock.

Raster payloads take the zero-copy path: an `ArrayBuffer` crosses to `pd_print` by pointer,
with no base64 and no copy on the JavaScript side. A typed-array **view** is copied first,
because a view may be a window onto a much larger buffer.

## Layout

```
wrappers/react-native/
├── src/                        the TypeScript API
│   ├── index.ts                the only module that imports 'react-native'
│   ├── NativePrinterDriver.ts  the codegen spec: one method per pd_* function
│   ├── methodNames.ts          those method names, importable without React Native
│   ├── native.ts               module accessor + the sink router
│   ├── enums.ts                26 closed enums mirrored from pd.h
│   ├── generated/              abi.generated.ts, extracted from pd.h by a script
│   ├── marshal.ts              pure JS ⇄ ABI translation (all of it directly testable)
│   ├── types.ts                the value types an app sees
│   ├── payload.ts              the three payload tiers + the Receipt builder
│   ├── events.ts               emitter + async iterator over one buffer
│   ├── PrinterDriver.ts / Printer.ts / PrintJob.ts / PrintQueue.ts
│   ├── transports.ts           JS-owned links (Bluetooth, MFi, USB, test doubles)
│   └── registrations.ts        the five runtime extension points
├── cpp/
│   ├── PrinterDriverModule.h   the threading contract, in full
│   ├── PrinterDriverModule.cpp the whole module: one file, both platforms
│   └── __tests__/rn_stub.h     host-side stand-in for the RN headers (never shipped)
├── ios/PrinterDriverPackage.h  manual-registration escape hatch for autolinking
├── android/                    build.gradle + CMakeLists.txt
├── scripts/
│   ├── generate-abi-mirror.mjs regenerates src/generated/abi.generated.ts from pd.h
│   ├── check_rn_cpp_syntax.sh  the C++ check, with negative controls
│   └── stage-native-sources.mjs stages core/capi/queue/dsl into native/ for `npm pack`
└── test/                       135 tests, `node --test` on the TypeScript sources
```

The build files compile `core/`, `capi/`, `queue/` and `dsl/` **in place** — the same files
`CMakeLists.txt`, `Package.swift` and `wrappers/android` compile. There is no private copy
of the engine in this package. The one exception is the npm tarball, which cannot reference
files above its own directory: `npm pack` runs `scripts/stage-native-sources.mjs` first and
copies them into `native/`, which is git-ignored precisely so it can never drift.

## Development

```sh
npm install          # pulls React Native in as a peer, for real type definitions
npm run verify       # abi:check + typecheck + test + cpp:syntax
```

| Command | What it does |
|---|---|
| `npm run abi:check` | fails if `src/generated/abi.generated.ts` is stale against `pd.h` |
| `npm run abi:generate` | regenerates it |
| `npm run typecheck` | `tsc --noEmit` over `src/` and `test/` |
| `npm test` | `node --test` on the TypeScript sources (Node 22+ strips the types) |
| `npm run cpp:syntax` | `clang++ -fsyntax-only` on the module against the real `pd.h`, plus three negative controls |
| `npm run build` | emits `lib/` (types + CommonJS) |

Adding a value to a `pd.h` enum without mirroring it here fails `abi:check` (the generated
artifact is stale) or `npm test` (the artifact and the hand-written mirror disagree). It
never becomes a silent "not implemented on platform X".

---

## Verification status

**Written on a machine with Node, npm and clang, and with no React Native application, no
Xcode project, no Android SDK/NDK/JVM and no printer.** What follows is exactly what was
checked and exactly what was not.

### Verified here

| What | How | Result |
|---|---|---|
| The TypeScript API type-checks | `tsc --noEmit`, `strict` + `noUncheckedIndexedAccess` + `erasableSyntaxOnly`, against **React Native 0.86.2's own `.d.ts` files** (installed as a peer, not a hand-written stub) | clean |
| 26 enum mirrors match `pd.h` member-for-member and value-for-value | `test/enums.test.ts` against `src/generated/abi.generated.ts`, extracted from the header by `scripts/generate-abi-mirror.mjs` | 59 tests |
| The tri-state result has three outcomes, no `success` boolean, and an exhaustive `switch` | `test/tristate.test.ts`, including a source scan of all of `src/` | 5 tests |
| Payload encoding and option marshalling, including both inverted flags | `test/marshal.test.ts` | 18 tests |
| Every public `pd_*` function is reachable: method name, spec declaration, **and a real call in the C++ module** | `test/parity.test.ts` over all 86 functions | 8 tests |
| The event stream, the sink router, and every "handler missing / throws / rejects / unknown kind" path | `test/events.test.ts`, `test/bridge.test.ts` | 21 tests |
| The public API against a recording module double | `test/api.test.ts` | 24 tests |
| **135 TypeScript tests total** | `npm test` | 135 pass, 0 fail |
| The C++ module's syntax and types against the **real `pd.h`** | `clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic` with `cpp/__tests__/rn_stub.h` standing in for the RN headers | clean |
| That the C++ check can go red | three negative controls, each required to fail *for its stated reason*: a `pd_driver*` passed to `pd_printer_id`; a `jsi::Value` copied (the stub models JSI's move-only ownership); and the module compiled **without** the stub, which must fail with `'ReactCommon/CallInvoker.h' file not found` — proving the React Native code path is really being compiled and not preprocessed away | all three fail correctly |
| The npm package is well-formed and self-contained | `npm pack --dry-run`, including the staged `native/` sources and the exclusion of the test stub | clean: the staged `native/` sources are in the tarball, `cpp/__tests__/rn_stub.h` is not |

### NOT verified — what a real app and CI must confirm, in this order

1. **It builds on iOS.** `pod install` has never evaluated
   `printerdriver-react-native.podspec`, and Xcode has never compiled the core through it.
2. **It builds on Android.** `android/CMakeLists.txt` has never been configured by CMake
   and `android/build.gradle` has never been run by Gradle. No NDK exists here.
3. **Codegen accepts the spec.** `src/NativePrinterDriver.ts` has never been through
   `@react-native/codegen`. In particular, this package exports the module accessor as a
   *function* (`getNativePrinterDriver()`) rather than calling `getEnforcing` at module
   scope, so that importing the package cannot throw outside an app — codegen finds the
   module name from the same call, but that has not been demonstrated.
4. **Autolinking finds the module.** `react-native.config.js`'s `cxxModule*` keys have
   never been exercised. `ios/PrinterDriverPackage.h` documents manual registration if they
   are not enough.
5. **The Expo config plugin runs.** `app.plugin.js` has never been through
   `expo prebuild`. Its `@expo/config-plugins` API usage is written from the documented
   surface, not from an observed run.
6. **The threading contract holds under load.** The CallInvoker hop, the blocking-question
   path with its deadline, and the "no blocking `pd_*` on the JS thread" invariant are
   implemented and documented, and have never executed. The deadlock this design exists to
   prevent has therefore also never been *observed* not to happen.
7. **A byte reaches a printer.** Nothing in this package has printed anything.
8. **The ArrayBuffer zero-copy path is genuinely zero-copy** on a real JSI runtime, and the
   pixel plane stays valid for the duration of the `pd_print` call.

The C++ syntax check proves that this module talks to `pd.h` correctly. It proves nothing
about JSI behaviour, because `cpp/__tests__/rn_stub.h` is a hand-written model of the parts
of JSI the module uses — faithful about ownership, silent about everything else — and was
not compared byte for byte with React Native's real headers.

## Licence

MIT — see [LICENSE](LICENSE), which points at the repository's single
[LICENSE](../../LICENSE).
