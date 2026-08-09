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
order, ending with a terminal one. It is **live** — each transition arrives as the core
records it, not as a batch once the job settles — because `pd_subscribe_job` hands its
callback the event *by value*. That is what a Dart `NativeCallable.listener`, the only
callback kind a foreign thread may invoke, needs: the listener runs after the native
call has already returned, so anything reached through a pointer into the emitting
worker's stack frame would be gone by then, and a copy has no such lifetime.

Subscribing late loses nothing: a new subscription replays what has happened so far
before following the rest, and the stream closes after the terminal event. Ordering is
the core's guarantee rather than this wrapper's.

## Bluetooth, and other links you own

Bluetooth is not in the core and should not be: on Apple it is CoreBluetooth or MFi, on
Android a `BluetoothSocket` over RFCOMM, on Linux a BlueZ socket — three platform
frameworks with their own permissions models, pairing UI and threading. So the split is
**the platform owns the socket, the core owns the protocol**: you supply three
operations, and the ordered fence, preflight, journal and confidence grading stay where
they are. A printer reached this way reports the same grade, authority and method a TCP
printer reports; `test/custom_transport_test.dart` asserts exactly that.

```dart
final transport = CustomTransport.fromLibrary(
  myPlugin,                       // your own .so/.framework
  connect: 'my_bt_connect',
  write: 'my_bt_write',
  close: 'my_bt_close',
  ctx: session,
  description: 'bt-spp:00:11:22:33:44:55',   // the printer id derives from this
);
final printer = driver.addCustomPrinter(transport, profileId: 'epson_tm_p20ii');

// The receive direction is ordinary Dart, from any isolate, at any time:
socket.listen(printer.feedBytes, onDone: () => printer.reportLinkDropped('closed'));
```

`connect` and `write` are **native function pointers, not Dart closures**, and that is
forced rather than preferred. The core calls them on its own worker thread and needs an
answer there and then — did the link open, how many bytes actually went out — and no
`dart:ffi` callback can do that: `NativeCallable.isolateLocal` aborts the process when
invoked off its isolate's thread, `NativeCallable.listener` is the one form a foreign
thread may invoke but supports `void` returns only (behind `write` the core would read
an uninitialised register as the byte count), and `NativeCallable.isolateGroupBound` is
experimental and runs with no isolate entered, so the callback crashes the moment it
touches Dart state. A wrapper that guessed at "did the bytes go out" would be the one
lie this SDK exists to remove, so it does not guess. `CustomTransport.withDartClose`
does take a Dart function, because close returns nothing and the core waits for no
answer.

The one rule to respect: pd.h forbids feeding bytes from inside `connect`/`write`/
`close`, since those run on the thread that would have to deliver them. Through this API
that is unreachable — no Dart code runs inside `connect` or `write` at all.

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

## Parity with the other wrappers

`scripts/check_parity.sh` (docs/api.md §17) asserts that this wrapper references every
public `pd_*` function in `capi/include/printerdriver/pd.h`, and that anything it covers
through a higher-level member instead is named in `scripts/parity_allowlist.txt`. **No
wrapper is a subset**: a `pd_` function added to the ABI without a binding here fails CI.

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
