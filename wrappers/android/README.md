# PrinterDriver -- Android wrapper

> **STATUS: scaffold -- not yet built on a JVM host; CI workflow provided.**
> Every file here was written and, where a host tool made it possible, syntax-checked
> (see "Verification status" below) — but nothing has been resolved by Gradle, compiled
> by the Android/NDK toolchain, or run. Treat this as a structurally complete starting
> point for a first real build, not as a build that has already succeeded once.

Kotlin/Android bindings for the PrinterDriver receipt-printing SDK core, over the C ABI
in [`capi/include/printerdriver/pd.h`](../../capi/include/printerdriver/pd.h). Generated-thin
by design (docs/api.md §1.3, §6): enum bridging and coroutine adapters, no logic --
anything that needs a decision lives in the C++ core, not here.

## Verification status

What was actually checked, and with what:

| Layer | Status |
|---|---|
| Kotlin sources (`src/main/kotlin`) | Written, reviewed by hand. **Not compiled** -- no `kotlinc`/Gradle/Android SDK on the authoring machine. |
| Bluetooth SPP transport (`src/main/kotlin/com/printerdriver/Bluetooth.kt`) | Written. **Never compiled and never run** -- not on a device, not on an emulator. No paired printer has ever been in the room. See that file's own header comment for the itemised list of what is unproven. **Hardware-pending.** |
| JNI glue (`src/main/cpp/printerdriver_jni.cpp`) | **Syntax- and type-checked** with a real compiler -- see below. Not compiled by the NDK, not linked, not run. |
| CMake build (`CMakeLists.txt`) | Written against the root project's own `CMakeLists.txt` as a reference. **Never configured or run**, on Android or otherwise -- no Android SDK/NDK available. |
| Gradle build (`build.gradle.kts`, `settings.gradle.kts`) | Written. **Never resolved** -- no Gradle/JVM available. Plugin/dependency versions are unpinned-against-reality: believed current and mutually compatible at authoring time, not verified against an actual repository. |
| JVM unit tests (`src/test/kotlin`) | Written, pure-Kotlin logic only (no native calls). **Never executed.** |
| GitHub Actions workflow (`.github-ci-example.yml`) | Written, inert (lives outside `.github/workflows/`). **Never run.** |

### Syntax confidence without a JVM

This machine has no JVM, Gradle, or Android SDK/NDK -- there is no `javac`, `kotlinc`,
`gradle`, or `adb` here, and no way to install them in this environment. What *is*
available is a host C++ compiler, so the one piece of this scaffold that got a real,
adversarial check is the JNI glue's C++ syntax and types:

```sh
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic \
  -DPD_JNI_TEST_STUB \
  -include wrappers/android/src/test/cpp/jni_stub.h \
  -I core/include -I capi/include -I capi/src \
  wrappers/android/src/main/cpp/printerdriver_jni.cpp
```

Run it from the repository root. It passes cleanly. `jni_stub.h`
(`src/test/cpp/jni_stub.h`) is a minimal, test-only stand-in for the two real NDK
headers the glue includes (`<jni.h>`, `<android/log.h>`) -- declared-but-undefined
methods matching the real JNI API's shapes closely enough to catch real signature
mistakes, force-included in place of those headers only when `PD_JNI_TEST_STUB` is
defined (which the real Android/Gradle/CMake build never does). Keep the stub in sync
with the JNI subset the glue actually calls; it grew `NewByteArray`,
`SetByteArrayRegion`, `CallBooleanMethod` and `CallIntMethod` when the custom-transport
vtable landed. **This is a syntax and type check only** -- it proves the glue parses,
that every `pd_*`/`PD_*` symbol it uses matches the real `pd.h`, and that JNI calls are
shaped correctly against a faithful-enough stub. It does **not** prove the code links,
runs, or is correct against the *real* NDK's `<jni.h>` (ABI-compatible in the parts that
matter, but not verified byte-for-byte against it), and it does not exercise the
CMake/Gradle/NDK build path at all -- see the table above.

Two things it structurally cannot catch, worth knowing before trusting a green run:

- **The JNI signature strings.** `"()Z"`, `"([B)I"`, `"(IIZIJ)V"` and friends are string
  literals handed to `GetMethodID`; no compiler checks them against the Kotlin
  declarations they are supposed to describe. They are verified by `javap`, or by the
  code failing at runtime, and neither has happened. This is item 4 below.
- **Anything about the Kotlin side.** `Bluetooth.kt` has never been near a compiler.

A green check is worthless if the check cannot go red, so the same technique run against
a deliberately wrong translation unit must fail, and does. Two controls specific to the
custom-transport ABI, each built by prepending the mistake to the real glue file and
compiling the result with the command above:

| Control | Expected rejection |
|---|---|
| `pd_transport_vtable.write` assigned a function returning `int32_t` -- a wrapper that reports a short write as a 32-bit count | `incompatible function pointer types assigning to 'pd_transport_write_fn'` |
| `pd_transport_feed_bytes(printer, size, data)` -- the buffer/length transposition | `no matching function for call to 'pd_transport_feed_bytes'` |

## What a real CI run must confirm

In priority order -- this is the actual risk list, not a formality:

1. **Gradle resolves.** `com.android.library` 8.5.2 + Kotlin 1.9.24 + the
   `kotlinx-coroutines-*` versions pinned in `build.gradle.kts` are believed mutually
   compatible but were never resolved against real Maven Central metadata.
2. **CMake configures and the NDK builds `libprinterdriver_jni.so`.** The relative
   paths in `CMakeLists.txt` (`../../core`, `../../capi` from `wrappers/android/`) were
   verified by inspection against this repository's actual directory layout, not by
   running CMake.
3. **The JNI glue compiles under the real NDK `<jni.h>`**, not just the host stub --
   in particular `__android_log_print`'s variadic signature and every
   `Get*ArrayElements`/`Release*ArrayElements` call.
4. **`GetMethodID` actually finds `onEvent`/`onMessage`/`connect`/`write`/`close`** on the
   Kotlin classes the `callbackFlow` builders, `Printer.send` and
   `BluetoothSppTransport` produce at runtime -- the JNI signature strings
   (`"(IIZIJ)V"`, `"(I)V"`, `"(Ljava/lang/String;)V"`, `"()Z"`, `"([B)I"`, `"()V"`) were
   derived by hand from the Kotlin declarations in `NativeCallbacks.kt` and never
   confirmed against `javap` output.
5. **The `internal`-without-per-member-`internal` visibility pattern in
   `NativeBridge.kt`/`NativeCallbacks.kt` actually avoids Kotlin's internal-member name
   mangling** the way its code comments assert. This determines whether the
   `Java_com_printerdriver_internal_NativeBridge_*` symbol names match what Kotlin
   actually emits; it was not checked against real `kotlinc` bytecode output.
6. **`kotlinx-coroutines-android`'s `Dispatchers.Main` resolves at runtime** (needed by
   `Printer.send`'s closure sugar and `PrinterDriver`'s internal scope).
7. **Every enum's `Bitmap.Config.ARGB_8888` byte-order assumption** in
   `Payload.Raster.of` (R,G,B,A per pixel on a little-endian device) holds on a real
   device/emulator -- believed correct and well-documented Android behavior, not
   independently re-verified here.
8. **R8/consumer-rules.pro** actually keeps what it claims to on a real minified build
   (`NativeBridge`'s native methods, the four callback interfaces and their lambda
   implementers).
9. **An RFCOMM connection to a real printer succeeds at all.** Everything in
   `Bluetooth.kt` -- the SPP UUID, `cancelDiscovery()` before `connect()`, the reader
   thread, the API 31+ permission set -- comes from documentation rather than from an
   observed connection. In particular, the handle-publication latch exists because
   `pd_add_printer_custom` starts the printer's worker thread (and queues a capability
   probe on it) before returning, so `connect()` can run before Kotlin has been told the
   printer handle: sound on paper, never observed racing.

## Repository layout

```
wrappers/android/            <- this Gradle project's root IS the `printerdriver` module
├── settings.gradle.kts
├── build.gradle.kts          <- com.android.library + kotlin-android + maven-publish
├── gradle.properties
├── CMakeLists.txt            <- compiles core/src + capi/src + the JNI glue
├── consumer-rules.pro
├── .github-ci-example.yml    <- template only; NOT under .github/ (see below)
└── src/
    ├── main/
    │   ├── AndroidManifest.xml
    │   ├── cpp/printerdriver_jni.cpp
    │   └── kotlin/com/printerdriver/
    │       ├── Bluetooth.kt   <- RFCOMM/SPP transport over pd_add_printer_custom
    │       └── ...
    └── test/
        ├── cpp/jni_stub.h     <- host-only syntax-check fixture, see above
        └── kotlin/com/printerdriver/...
```

Single-module and flat on purpose: there is no nested `printerdriver/` module
directory. `CMakeLists.txt` reaches the SDK's C++ sources with `../../core` and
`../../capi` -- two levels up from `wrappers/android/` to the repository root -- and a
nested module folder would push those out to `../../../core` instead. `core/` and
`capi/` are compiled in place from this module; nothing is copied.

**No `gradlew`/`gradlew.bat`/`gradle-wrapper.jar` are committed.** The wrapper *scripts*
are boilerplate this scaffold could have hand-authored, but `gradle-wrapper.jar` is a
compiled binary, and generating one by hand on a host with no Gradle installed is not
something that can be done honestly. `gradle/wrapper/gradle-wrapper.properties` (plain
text, pinning Gradle 8.7) is committed as a declaration of intent; run
`gradle wrapper --gradle-version 8.7 --distribution-type bin` once on a machine with
Gradle installed to generate the rest, then commit it. Until then, use a locally
installed `gradle`, or the `gradle/actions/setup-gradle` approach
`.github-ci-example.yml` uses.

## Integration

Not yet published (see "Signing" in `build.gradle.kts` -- Maven Central publishing is a
documented TODO). Once a release is published to GitHub Packages:

```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories {
        maven {
            url = uri("https://maven.pkg.github.com/rafiki270/PrinterDriver")
            credentials {
                username = providers.gradleProperty("gpr.user").getOrElse("")
                password = providers.gradleProperty("gpr.token").getOrElse("")
            }
        }
    }
}
```

```kotlin
// build.gradle.kts
dependencies {
    implementation("com.printerdriver:printerdriver:0.1.0")
}
```

`minSdk` 26. Pulls in `kotlinx-coroutines-core` and `kotlinx-coroutines-android`
transitively.

Permissions this library declares, and what they are for:

| Permission | Why |
|---|---|
| `INTERNET` | `addPrinterTcp` is a raw TCP socket. |
| `BLUETOOTH`, `BLUETOOTH_ADMIN` (`maxSdkVersion="30"`) | The pre-Android-12 pair, for `addPrinterBluetooth`. |
| `BLUETOOTH_CONNECT` | API 31+: talking to an already-paired device. **Runtime permission** -- the host app must request it. |
| `BLUETOOTH_SCAN` (`neverForLocation`) | API 31+: what `cancelDiscovery()` falls under. `neverForLocation` is asserted because this library never scans and never derives location; without it every consuming app inherits a location requirement. |

This library cannot request the runtime permissions itself -- a prompt needs an Activity,
and a library that pops one has taken a decision belonging to the host app. Pairing is
likewise out of scope: it is an operator action in the system's Bluetooth settings, and a
printing SDK that can pair can also pair with the wrong printer.

## Quick start

```kotlin
import com.printerdriver.*

val driver = PrinterDriver.create(PrinterDriverConfig(storageDirectory = context.filesDir.path))
val kitchen = driver.addPrinterTcp(TcpPrinterConfig(host = "192.168.1.101"))

// Or over Bluetooth Classic SPP, to a device the operator has already paired
// (docs/compatibility-brief.md §25). NEVER RUN AGAINST HARDWARE -- see "Verification
// status". Registration does not connect: an out-of-range printer is reported through
// the job's own JobResult, not by this call.
val counter = driver.addPrinterBluetooth(
    context,
    BluetoothPrinterConfig(address = "00:11:22:33:44:55", widthDots = 384)
)
counter.bluetoothTransportKind   // CLASSIC_SPP -- never a bare `bluetooth = true` (§25)

// Coroutine style
val job = kitchen.print(
    Payload.Raster.of(receiptBitmap),
    JobOptions(key = "order-7F3A-92C1#kitchen-1")
)
scope.launch { job.events.collect { updateTicketUI(it.state, it.confidence) } }
when (val result = job.result()) {
    is JobResult.Done -> markTicketPrinted(result.confidence)
    is JobResult.Failed -> showFailure(result.reason)      // safe to resubmit the same key
    is JobResult.Unknown -> askOperator(job)                // forceReprint or manual confirm
}

// Closure style (docs/api.md §12) -- for call sites that would rather not use coroutines
kitchen.send(
    Payload.Raster.of(receiptBitmap),
    JobOptions(key = "order-7F3A-92C1#kitchen-1"),
    onProgress = { event -> ticketUi.update(event.state) }
) { result ->
    when (result) {
        is JobResult.Done -> markTicketPrinted(result.confidence)
        is JobResult.Failed -> showFailure(result.reason)
        is JobResult.Unknown -> askOperator()
    }
}

// ... application shutdown:
driver.close()
```

No `isSuccess` boolean anywhere: `JobResult` is `Done`/`Failed`/`Unknown`, and Kotlin's
exhaustive `when` is what makes an app handle all three (docs/api.md §1.4, §4).

## Threading contract

Straight from `pd.h`, unchanged by this wrapper:

- **`Printer.print` / `Printer.forceReprint` / `Printer.refreshStatus` /
  `Printer.drain`** are `suspend` and hop to `Dispatchers.IO` -- the underlying calls
  are bounded but can touch the durable job store (a journal write, fsync'd unless
  `PrinterDriverConfig.fsyncDisabled`) or block briefly on a printer round trip.
- **`PrintJob.result()`** polls `pd_job_await` with a 250ms timeout in a loop rather
  than waiting indefinitely in one native call, specifically so a cancelled coroutine
  is noticed within one poll interval -- `pd_job_await` itself has no cancellation hook.
- **`PrintJob.events` / `Printer.events`** (`callbackFlow`) receive events on whatever
  thread the core's callback contract puts them on: the synchronous replay half of
  `pd_subscribe_job` on the calling thread, everything after that on a printer's worker
  thread. The JNI layer (`printerdriver_jni.cpp`) attaches/detaches that worker thread
  to the JVM per callback -- see that file's header comment for the full contract and
  why it does not call back into any `pd_*` function from inside a callback (`pd.h`
  forbids it).
- **There is no `pd_unsubscribe_*` in the C ABI.** A `Flow` collector stopping (its
  coroutine cancelled, `awaitClose` invoked) does not tell the native layer to stop
  calling the underlying callback -- it just stops reading from an now-abandoned
  channel. The `GlobalRef` each subscription holds is released when the owning
  `PrinterDriver.close()` runs, not before.
- **`Printer.send`'s closure sugar** (`onProgress`/`onResult`) runs on
  `Dispatchers.Main.immediate` by default -- the conventional choice for callback-style
  Android APIs. `onResult` fires exactly once, always, because it is sugar over
  `PrintJob.result()`'s suspend contract, not a separate mechanism.
- **A custom transport's `connect`/`write`/`close`** (`BluetoothSppTransport`) are called
  **by the core**, on the owning printer's worker thread, one at a time and never
  concurrently with each other. None of them may call `pd_transport_feed_bytes` -- the
  thread they run on is the thread that would have to service the delivery. Received
  bytes go the other way from the transport's own reader thread, which may run
  concurrently with a `write` in flight; that is the normal case, a status answer
  arriving while the next chunk goes out. The registration outlives an individual
  connection: after a link drop the core calls `connect` again on the same object.
- **`PrinterDriver.close()`** shuts every registered Bluetooth transport down *before*
  `pd_destroy`, because `pd_destroy` frees every `pd_printer` handle and a reader thread
  outliving it would feed bytes through a dangling pointer. The consequence is that an
  in-flight job loses its backchannel at that moment and settles as Failed or Unknown
  rather than waiting for a fence that can no longer arrive -- await outstanding results
  before closing if that matters. The native side independently refuses post-`pd_destroy`
  feeds (`printerdriver_jni.cpp`'s lifecycle mutex), so a straggler is safe rather than
  merely unlikely.
- **`PrinterDriver.close()`** cancels that internal scope and then calls `pd_destroy`,
  which itself blocks until every in-flight job reaches a terminal state. A
  `Printer.send` callback already inside its native wait when `close()` runs may still
  fire once, briefly, after `close()` starts returning -- see `PrinterDriver.close`'s
  KDoc.

## Testing

`src/test/kotlin` is a plain JVM unit test layer: pure-Kotlin logic (enum `fromRaw`
mapping, `JobResult.fromRaw`, `DeviceStatus` tri-state decoding), nothing that calls
`System.loadLibrary`. Run with `gradle testDebugUnitTest` (see
`.github-ci-example.yml`'s `test` job). There is no `src/androidTest` in this
scaffold -- instrumented tests against a real/emulated device are out of scope here.

## Continuous integration

`.github-ci-example.yml` lives at `wrappers/android/`, not `.github/workflows/`, so it
does not run automatically. To activate it: review the action and SDK/NDK/CMake version
pins (see "Verification status" -- none of them were checked against a live registry),
then copy or symlink it to `.github/workflows/android-wrapper-ci.yml` at the repository
root, and uncomment its `push`/`pull_request` triggers.

## License

MIT -- see [`LICENSE`](../../LICENSE) at the repository root. `groupId`
`com.printerdriver`, `artifactId` `printerdriver`, initial `version` `0.1.0`
(`build.gradle.kts`).
