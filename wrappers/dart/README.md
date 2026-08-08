# printerdriver

Dart bindings for the [PrinterDriver](https://github.com/rafiki270/PrinterDriver) SDK: a
receipt-printing core in portable C++17 whose whole point is that it never claims a
receipt printed when it only handed bytes to a socket. Jobs are fenced with ordered
completion markers (`GS ( H` process ids, `GS r 1`), and the terminal answer is
tri-state — `done`, `failed` or **`unknown`** — with a confidence level that says what
the claim rests on.

Pure Dart over `dart:ffi`, no runtime dependencies. It works in a Flutter app the same
way it works in a server or CLI program; the application supplies the native library
(see [Shipping the native library](#shipping-the-native-library)).

```dart
import 'package:printerdriver/printerdriver.dart';

final driver = PrinterDriver.open(storageDirectory: '/var/lib/pos/printerdriver');
final kitchen = driver.addTcpPrinter(host: '192.168.1.101', widthDots: 576);

final result = await kitchen.send(
  Payload.document([
    DocumentOp.align(Alignment.center),
    DocumentOp.line('THE CORNER CAFE'),
    DocumentOp.align(Alignment.left),
    DocumentOp.line('1x Flat white              3.40'),
    DocumentOp.feed(3),
  ]),
  key: 'order-7F3A-92C1#kitchen-1',
  onProgress: (event) => ticketUi.update(event.state),
);

switch (result) {
  case JobDone(:final confidence): markTicketPrinted(confidence);
  case JobFailed(:final reason):   showFailure(reason);   // safe to resubmit the key
  case JobUnknown():               askOperator();         // reprint or confirm by hand
}
```

`example/main.dart` is the same thing as a runnable program.

## The three nouns

| | |
|---|---|
| `PrinterDriver` | Owns the native driver, its printers, its jobs and the journal. One per app; `dispose()` frees everything. |
| `Printer` | Accepts jobs. Carries `status` (a snapshot), `refreshStatus()` (a round trip), `events` (live device events), `drain()`, `openCashDrawer()`. |
| `PrintJob` | One submitted job: `events`, `result`, `currentState`, `attempt`. |

## Payload tiers

```dart
Payload.raster(rgba8, width: w, height: h)   // tier 1: whatever your renderer produced
Payload.grayscale(gray, width: w, height: h) // the same, one byte per pixel
Payload.document([...ops])                   // tier 2: lines, alignment, bold, feed
Payload.raw(bytes)                           // tier 3: escape hatch, still fenced and cut
```

The raster tier is the drop-in path for an app that already renders receipts to a
canvas: the core does the greyscale conversion, scaling, binarization, banding and
pacing, in integer arithmetic, so the same pixels always produce the same bytes.

## The tri-state, and why there is no `bool`

`JobResult` is a sealed class with exactly three cases, so a `switch` cannot forget one:

- **`JobDone(confidence)`** — the job reached `DoneSoftware`. `confidence` says what that
  rests on: `cutFaultFree` on a `GS ( H` printer that also answered the post-cut cutter
  read, `transportAccepted` on a write-only one. The SDK never inflates it.
- **`JobFailed(reason)`** — nothing printed, or the failure was confirmed: a preflight
  refusal, an unreachable transport, a cutter fault. Resubmitting the same key is safe.
- **`JobUnknown(reason)`** — bytes went out and nothing came back: a completion timeout,
  a dropped link, a crash. Not a success, not a failure. Surface it to an operator and
  resolve it with `forceReprint` or a manual confirmation.

`JobDone` also declares `grade`, `authority` and `method` (docs/api.md §13). They are
null against the current C ABI, which carries only outcome, confidence and reason in
`pd_job_result` — this wrapper reports that rather than deriving a grade from the
confidence level, because deriving it is a decision, and decisions belong in the core.
`Printer.completion` gives the fence a printer actually answers.

Idempotency: resubmitting a `key` that already has a job does not print. It returns that
job — the identical Dart object, because the ABI returns the identical handle. Printing
the same key again is only possible through `forceReprint`, which prepends the reprint
banner and the attempt counter.

## Events

`Printer.events` is a live broadcast stream of `DeviceEvent`s (`online/offline`,
`coverOpen/Closed`, `paperOut/NearEnd/Ok`, `cutterError`,
`recoverable/unrecoverableError`, `connectionLost/Restored`). It replaces polling a
printer to ask whether it is still there.

`PrintJob.events` is the job's recorded history: every `JobEvent` the core wrote, in
order, ending with a terminal one. **It is delivered when the job settles rather than
transition by transition**, and that is a limitation of the current C ABI rather than a
choice: `pd_subscribe_job` hands its callback a `const pd_job_event*` that points at a
temporary of the emitting worker thread's stack frame, and a Dart
`NativeCallable.listener` — the only callback kind a foreign thread may invoke — runs
after that call has returned, so it cannot read through that pointer. This package
therefore uses the listener purely as a wake-up and takes the event data from the
synchronous replay `pd_subscribe_job` performs on the calling thread once the job is
terminal, where the pointer is alive. Nothing is lost, reordered or invented;
`PrintJob.currentState` is a live read for a progress indicator that needs one. An ABI
that handed out an event outliving its callback — by value, or with storage the driver
owns — would make the stream live, and nothing else here would change.

## Shipping the native library

The application supplies `libprinterdriver_capi.dylib` / `.so` / `.dll`. Build it from
the repository root:

```sh
cmake -S . -B build-dart -DPD_BUILD_SHARED_CAPI=ON
cmake --build build-dart --target printerdriver_capi_shared
```

`loadPrinterDriverLibrary` resolves it in this order:

1. the `libraryPath` passed to `PrinterDriver.open` — an instruction, never a hint: if it
   cannot be opened, the call throws instead of binding some other build;
2. the `PRINTERDRIVER_LIB_PATH` environment variable, naming either the file or a
   directory holding it;
3. the directory of the running executable, and a `lib/` beside it;
4. the plain library name, letting the platform loader search its own paths.

An application whose native code is linked into the executable — an iOS binary, or an
Android app whose `.so` is already loaded — passes an open library instead:
`PrinterDriver.open(library: DynamicLibrary.process())`.

**Flutter:** this is a plain Dart package, so it works in a Flutter app but does not yet
package the native library for you. Bundling it as a Flutter plugin (a podspec for iOS
and macOS, CMake for Android and Linux, so the `.framework`/`.so` ships with the app and
no path resolution is needed) is a later step; for now a Flutter app has to add the
library to its own build and, on Android, load it before `PrinterDriver.open`.

## Testing your own printing paths

Printing code is worth testing without a printer, particularly the branch that has to
handle `JobUnknown`. The library built from the `printerdriver_capi_testing` target adds
the scripted devices of `capi/tests/pd_test_support.h` — and only that target does; the
shipped library exports no test doubles at all, which this package's own suite checks.

```sh
cmake --build build-dart --target printerdriver_capi_testing
```

```dart
final driver = PrinterDriver.open(libraryPath: '.../libprinterdriver_capi_testing.dylib');
final printer = driver.addScriptedPrinterForTesting(script: 'silent');
expect(await printer.send(receipt), isA<JobUnknown>());
```

Scripts: `ok` (healthy `GS ( H`, reaches `cutFaultFree`), `gsr1` (ordered fence only,
caps at `cutProcessed`), `silent` (accepts bytes, never acknowledges, ends unknown),
`paperout` (strict preflight refuses), `refuse` (the connection fails).

## Running this package's tests

```sh
cmake -S . -B build-dart -DPD_BUILD_SHARED_CAPI=ON     # from the repository root
cmake --build build-dart -j
cd wrappers/dart && dart pub get && dart test
```

The suite finds the built libraries under `build-dart/` on its own; set
`PRINTERDRIVER_LIB_PATH` to point it somewhere else. Without them it skips rather than
fails.

## Threading

Callbacks come off the core's own threads and are marshalled onto the isolate that
created the driver, so nothing here has to be called from a particular thread. Two calls
block the calling isolate on purpose, because there is nothing useful to do while they
wait: `Printer.refreshStatus()` (a status round trip queued behind the active job) and
`Printer.drain()`. `PrinterDriver.dispose()` blocks too — it waits for in-flight jobs to
reach a terminal state — so await the jobs you care about first.

## Licence

MIT, the same as the rest of the repository; see the `LICENSE` file here, which mirrors
the one at the repository root.
