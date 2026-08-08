# PrinterDriver -- .NET wrapper

C# bindings for the PrinterDriver receipt-printing SDK core, over the C ABI in
[`capi/include/printerdriver/pd.h`](../../capi/include/printerdriver/pd.h). Generated-thin
by design (docs/api.md §1.3, §6): enum bridging plus async adapters, no logic -- anything
that needs a decision lives in the C++ core, not here.

> **Verification status in one line:** the managed side is fully built and tested, on
> macOS, against the real native library (35 tests, all green). The **native Windows build
> has never been run** -- the Windows sources are syntax-checked only, and
> `.github/workflows/windows.yml` is manual-dispatch and has not been dispatched.

## Verification status

What was actually checked, and with what. Read this before trusting anything below it.

| Layer | Status |
|---|---|
| Managed wrapper (`PrinterDriver/`) | **Built and tested.** `dotnet build` clean, zero warnings, `net8.0`. |
| Managed tests (`PrinterDriver.Tests/`) | **35 tests, all passing on macOS**, against the real `libprinterdriver_capi_testing.dylib` built from this repository -- the same C ABI plus the scripted device of `capi/tests/pd_test_support.h`. Not mocks: every end-to-end test drives real C++ through real P/Invoke. |
| `dotnet pack` | **Runs clean**, producing a managed-only `PrinterDriver.0.1.0.nupkg` plus its `.snupkg`. |
| Native library, macOS (`libprinterdriver_capi_testing.dylib`) | **Built and exercised** by the tests above, and by the C++ suite's twelve `ctest` targets. |
| Native library, **Windows** (`printerdriver_capi.dll`) | **Never built.** No MSVC, no clang-cl, no Windows machine in this loop. |
| Windows core sources (`core/src/transport_win.cpp`, `core/src/platform_file_win.cpp`, and the `PD_PLATFORM_WINDOWS` branches) | **Syntax- and type-checked only** -- see below. Never compiled by MSVC, never linked against `ws2_32`, never run. |
| `.github/workflows/windows.yml` | **Written, never run.** `workflow_dispatch` only, on purpose (see "Continuous integration"). |
| NuGet `runtimes/win-x64/native/` payload | **Not present.** The package as built here is managed-only; staging the DLL is a step a Windows release build performs. |

### Syntax confidence without Windows

This machine is a Mac. What is available is a host C++ compiler, so the Windows edge got a
real, adversarial check rather than a review:

```sh
scripts/check_windows_syntax.sh
```

which runs, for `core/src/transport_win.cpp`, `core/src/platform_file_win.cpp`,
`core/src/job_store.cpp`, `core/src/capability_probe.cpp` and two test translation units
that pull in `core/tests/fake_printer.hpp`:

```sh
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic \
  -DPD_FORCE_WINDOWS_PLATFORM -DPD_WINDOWS_SYNTAX_CHECK \
  -include core/tests/win32_stub.h \
  -I core/include -I core/src -I core/tests -I capi/include -I queue/include \
  <source>
```

`core/tests/win32_stub.h` is a minimal, test-only stand-in for `<winsock2.h>`,
`<ws2tcpip.h>` and `<windows.h>`, with the signatures that matter kept faithful: `SOCKET`
is `UINT_PTR` (so storing one in an `int` is a type error, exactly as on Windows), `HANDLE`
is `void*`, `setsockopt`/`getsockopt` take `char*` buffers, `send`/`recv` take and return
`int`, and the `addrlen` parameters are `int` rather than POSIX's `socklen_t`. It is never
compiled into anything shipped; a real Windows build never defines
`PD_WINDOWS_SYNTAX_CHECK`.

**The check has teeth, and that is itself checked.** A green run that could not go red
proves nothing, so the script also runs three negative controls and requires each to fail
*with the expected diagnostic*, not merely to fail:

- **A** -- a Winsock `SOCKET` narrowed into an `int`, the canonical Windows port bug. Must
  be rejected with `cannot be narrowed from type 'pd::net::Socket'`.
- **B** -- `FlushFileBuffers` called on a `std::string`. Must be rejected with a conversion
  error to `HANDLE`.
- **C** -- the whole check re-run *without* `-DPD_FORCE_WINDOWS_PLATFORM`. Must fail with
  `use of undeclared identifier 'socket_'`, because the POSIX branch of `transport.hpp`
  declares different members. This is what proves every "ok" above was checking the Windows
  branch and not quietly re-checking the POSIX one.

**What this does not establish:** anything about behaviour, about MSVC or clang-cl, about
linking, or about the real Windows SDK headers. It proves the code parses and type-checks.
Nothing more.

### Why `PD_FORCE_WINDOWS_PLATFORM` and not `-D_WIN32`

Defining `_WIN32` on clang against libc++ reconfigures the standard library itself
(`<thread>`, `<mutex>`, path handling) and produces failures that have nothing to do with
this SDK. `core/include/printerdriver/platform.hpp` therefore defines its own selector:

```c++
#if defined(_WIN32) || defined(PD_FORCE_WINDOWS_PLATFORM)
#define PD_PLATFORM_WINDOWS 1
#endif
```

A real Windows build defines `_WIN32` and never defines `PD_FORCE_WINDOWS_PLATFORM`, so
the two cannot disagree there.

## What a real Windows CI run must confirm

In priority order. This is the actual risk list, not a formality:

1. **`printerdriver_core` compiles under MSVC.** The syntax check used clang with a
   hand-written header stub; MSVC's own `<winsock2.h>`/`<windows.h>` will disagree
   somewhere, most likely over macro pollution (`min`/`max`, `ERROR`) that
   `NOMINMAX`/`WIN32_LEAN_AND_MEAN` normally suppress and that this build has never had to
   suppress.
2. **`ws2_32` links.** `CMakeLists.txt` adds it for `WIN32`; nothing has verified the
   target name against a real MSVC link step.
3. **The loopback wake pair actually works** (`core/src/transport_win.cpp`,
   `makeWakePair`). Windows has no `socketpair`, so `TcpTransport::close()` wakes its
   reader through a connected loopback TCP pair. The peer-verification branch in particular
   has never executed.
4. **`select()` reports a failed non-blocking connect through `exceptfds`.** The Windows
   `poll()` shim is written on `select` rather than `WSAPoll` precisely because WSAPoll does
   not report that, but the mapping from `exceptfds` to `kPollError` has never run.
5. **`FILE_APPEND_DATA` without `FILE_WRITE_DATA` gives true append semantics** for the
   journal, and **`ReplaceFileA` + the `MoveFileExA` first-compaction fallback** behave as
   the durability rule requires (`core/src/platform_file_win.cpp`). `test_store` is the
   suite that would catch a mistake here.
6. **Directory durability is genuinely narrower on Windows.** `syncDirectory` is a
   documented no-op there -- Win32 exposes no flushable handle to a directory. The
   ordering the journal needs is obtained from `MOVEFILE_WRITE_THROUGH`/`ReplaceFile`
   instead. Whether that is sufficient under power loss is unverified.
7. **`TempDir`'s `FindFirstFileA` sweep** cleans up after `test_store`, so the suite does
   not leave megabytes of journals in `%TEMP%`.
8. **The .NET wrapper binds `printerdriver_capi.dll`** through
   `NativeLibraryResolver`, with `RuntimeInformation.RuntimeIdentifier` reporting
   `win-x64` and the `runtimes/win-x64/native/` lookup finding a staged DLL.

## Layout

```
wrappers/dotnet/
├── README.md                    <- this file; also packed as the NuGet readme
├── PrinterDriver/
│   ├── PrinterDriver.csproj     <- net8.0, LangVersion latest, PackageId PrinterDriver
│   ├── Enums.cs                 <- the thirteen mirrored enums
│   ├── JobResult.cs             <- the closed Done/Failed/Unknown hierarchy
│   ├── Model.cs                 <- JobEvent, DeviceStatus, the config records
│   ├── Payload.cs               <- the three payload tiers and their marshalling
│   ├── PrinterDriver.cs         <- the driver: handles, interning, disposal order
│   ├── Printer.cs               <- print, force-reprint, status, device events
│   ├── PrintJob.cs              <- state, events, the tri-state await
│   ├── CallbackRoots.cs         <- GCHandle lifetime for native callbacks
│   └── Interop/
│       ├── NativeMethods.cs         <- P/Invoke over pd.h, and the struct layouts
│       └── NativeLibraryResolver.cs <- PRINTERDRIVER_LIB_PATH, then runtimes/<rid>/native
└── PrinterDriver.Tests/
    ├── NativeFixture.cs         <- finds the testing library; test-only P/Invokes
    ├── PrintJobTests.cs         <- end to end against the scripted devices
    ├── PayloadTierTests.cs      <- document and raster tiers
    ├── JobResultTests.cs        <- the tri-state contract, asserted structurally
    ├── EnumBridgeTests.cs       <- enums and struct layouts vs the core
    └── CallbackLifetimeTests.cs <- forced GCs across live subscriptions
```

## Finding the native library

`NativeLibraryResolver` installs a `DllImportResolver` for the assembly. Resolution order,
first hit wins:

1. **`PRINTERDRIVER_LIB_PATH`** -- a file, or a directory containing the library. An
   explicit path is an instruction, not a hint: when it is set and cannot be opened,
   resolution throws rather than silently binding a different build of the SDK. That
   distinction matters more than it looks; binding yesterday's build is how a fixed bug
   comes back.
2. **`runtimes/<rid>/native/`** beside the managed assembly -- the NuGet layout, and where
   a hand-staged deployment normally puts it.
3. **The platform loader's own search**, which is what resolves the library when NuGet's
   runtime asset handling has already staged it.

`PrinterDriver.NativeLibraryPath` reports what was actually opened, for diagnosing a
deployment that bound the wrong file.

## NuGet packaging

`dotnet pack wrappers/dotnet/PrinterDriver` produces `PrinterDriver.0.1.0.nupkg`, MIT,
repository `https://github.com/rafiki270/PrinterDriver`, with this README as the package
readme.

**The package built from this repository is managed-only.** A Windows release build stages
the native DLL first:

```
wrappers/dotnet/PrinterDriver/
└── runtimes/
    ├── win-x64/native/printerdriver_capi.dll
    └── win-arm64/native/printerdriver_capi.dll     <- later
```

and the csproj's glob packs whatever is there, structure intact. The split is deliberate:
the managed assembly is verified on every machine, the native DLL only on a Windows one,
and a package that claimed to contain a verified Windows binary today would be lying.

## Quick start

```csharp
using PrinterDriver;

using var driver = PrinterDriver.Create(new PrinterDriverConfig(
    StorageDirectory: Path.Combine(Environment.GetFolderPath(
        Environment.SpecialFolder.LocalApplicationData), "printerdriver")));

var kitchen = driver.AddPrinterTcp(new TcpPrinterConfig(Host: "192.168.1.101"));

var job = kitchen.Print(
    Payload.FromText("2x MARGHERITA\nTABLE 14\n"),
    new JobOptions(Key: "order-7F3A-92C1#kitchen-1"));

await foreach (var step in job.EventsAsync())
{
    Console.WriteLine($"{step.State} ({step.Confidence})");
}

switch (await job.GetResultAsync())
{
    case JobResult.Done done:
        MarkTicketPrinted(done.Confidence);
        break;
    case JobResult.Failed failed:
        ShowFailure(failed.Reason);       // safe to resubmit the same key
        break;
    case JobResult.Unknown unknown:
        AskOperator(unknown.Confidence);  // ForceReprint, or confirm by hand
        break;
}
```

No `IsSuccess` anywhere: `JobResult` is `Done`/`Failed`/`Unknown`, the hierarchy is closed
(the base constructor is private), and `JobResultTests` fails the build if a boolean is
ever added to it. Collapsing `Unknown` into either bucket is the bug that prints a second
kitchen ticket, or drops one silently.

The document and raster tiers are there too:

```csharp
kitchen.Print(new Payload.Document([
    DocumentOp.Align(Alignment.Center),
    DocumentOp.Bold(true),
    DocumentOp.Line("KITCHEN TICKET"),
    DocumentOp.Bold(false),
    DocumentOp.Align(Alignment.Left),
    DocumentOp.Line("2x MARGHERITA"),
    DocumentOp.Feed(2),
], CodePage.PC437));

kitchen.Print(new Payload.Raster(rgba8Pixels, width, height));
```

## Threading contract

Straight from `pd.h`, unchanged by this wrapper:

- **Job event callbacks** run on the owning printer's worker thread, except for the replay
  of already-recorded events, which runs on the thread calling `pd_subscribe_job` before
  that call returns. `PrintJob.EventsAsync` subscribes **eagerly** -- it is not an
  `async IAsyncEnumerable` method, precisely so that no event can be decoded between "the
  caller asked for events" and "the channel exists to receive them".
- **Device event callbacks** run on whichever thread decoded the status: the transport's
  reader thread, or a worker thread when a status query answers inside a job.
  `Printer.DeviceEventReceived` handlers run there too, and an exception from one is
  swallowed rather than being allowed to take a native worker thread down.
- **A callback must not block, and must not call back into any `pd_*` function on the same
  driver.** Nothing in this wrapper's trampolines does: they write to a `Channel` and
  return.
- **`PrintJob.GetResultAsync`** polls `pd_job_await` with a 100 ms timeout in a loop rather
  than waiting indefinitely in one native call, so a cancelled token is noticed within one
  poll interval. `pd_job_await` has no cancellation hook, and a thread parked inside it
  cannot be interrupted. `WaitForResult` is the blocking form for callers that want it.
- **There is no `pd_unsubscribe_*` in the C ABI.** Every delegate and context object handed
  to the core is rooted with a `GCHandle` for the life of the driver
  (`CallbackRoots`), and released only *after* `pd_destroy` has returned -- which is the
  moment after which no worker thread exists to call it. `CallbackLifetimeTests` forces
  full compacting collections across live subscriptions to prove it.
- **`PrinterDriver.Dispose`** calls `pd_destroy`, which blocks until every in-flight job
  reaches a terminal state, then frees the roots and completes any open event stream.

## Running the tests

The suite drives `printerdriver_capi_testing`, not the library that ships: the same C ABI
plus the scripted device and enum bridge of `capi/tests/pd_test_support.h`. Build it from
the repository root:

```sh
cmake -S . -B build-dotnet -DPD_BUILD_SHARED_CAPI=ON
cmake --build build-dotnet --target printerdriver_capi_testing
dotnet test wrappers/dotnet/PrinterDriver.Tests
```

`NativeFixture` finds that build automatically, or honours `PRINTERDRIVER_LIB_PATH`. **There
is no silent skip when the library is missing** -- a suite that passes because it tested
nothing is worse than one that fails, so it fails with the two commands above in the
message.

The test project targets `net8.0`, the same framework the library ships, so the suite
exercises the exact assembly that ships. `<RollForward>Major</RollForward>` is what lets
those assemblies run on a machine that only has a newer shared runtime installed.

## Continuous integration

[`.github/workflows/windows.yml`](../../.github/workflows/windows.yml) builds the core with
MSVC, runs `ctest`, then builds, tests and packs the .NET wrapper on `windows-latest`.

It is **`workflow_dispatch` only** -- no `push`, no `pull_request` trigger. That is
deliberate and not laziness: the Windows path has never been compiled, so the run is
expected to need iteration, and a workflow that fails on every push would be a red badge
that says nothing anyone can act on. Run it from the Actions tab when someone is available
to read the output; once it is green, adding `push`/`pull_request` triggers is a one-line
change.

## License

MIT -- see [`LICENSE`](../../LICENSE) at the repository root.
