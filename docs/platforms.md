# Platform Support

Addendum (2026-08-08) to [sdk-spec.md](sdk-spec.md) §2/§7 and [api.md](api.md) §5–§9.

## Platform matrix

| Platform | How an app consumes the SDK | Status |
|---|---|---|
| iOS | Swift package (SwiftPM) over the C ABI | first wave |
| **macOS** | **The same Swift package — it MUST declare and test both `.iOS` and `.macOS` platforms.** The core is already built and tested on macOS (all CI-of-record runs happen on a Mac). Distribution: same SwiftPM package, tag-based. | first wave (requirement added 2026-08-08) |
| Android | Kotlin over JNI, Gradle `maven-publish` (Maven Central/GitHub Packages), MIT POM | first wave (scaffold until a JVM host verifies it) |
| Flutter/Dart | pub.dev-ready FFI package | first wave |
| Linux / Raspberry Pi | C++ API or C ABI directly; the pd-agent daemon reuses the same core | works today (POSIX) |
| **Windows** | See below | **written, not built** (2026-08-08) — see "Implementation status" |

## Implementation status (added 2026-08-08)

Milestone M8 landed the two platform edges below and the .NET wrapper. What that means
precisely, because "landed" is doing a lot of work in that sentence:

| Piece | Where | Status |
|---|---|---|
| Winsock2 transport | `core/src/transport_win.cpp` | Written. **Syntax- and type-checked only.** Never compiled by MSVC, never linked against `ws2_32`, never run. |
| `FlushFileBuffers`/`ReplaceFile` journal | `core/src/platform_file_win.cpp` | Same. |
| Platform selection | `CMakeLists.txt` (`if(WIN32)`) | The POSIX and Windows sources are **alternative translation units, never both**, so `core/src/transport.cpp` is byte-for-byte what it always was and the POSIX build is unaffected. |
| Syntax check + negative controls | `scripts/check_windows_syntax.sh`, `core/tests/win32_stub.h` | Green, including three negative controls that must fail with a named diagnostic — one of which proves the platform selector is really selecting the Windows branch. |
| .NET wrapper | `wrappers/dotnet/` | **Built and tested** (35 tests) on macOS against the real native library. NuGet metadata complete; `dotnet pack` clean. Managed-only package: no Windows DLL has been built to put in it. |
| Windows CI | `.github/workflows/windows.yml` | Written, `workflow_dispatch` only, **never run**. |

The honest summary: the Windows source exists and type-checks, the managed wrapper is
genuinely verified, and nothing has yet proved a byte reaches a printer from Windows.
`wrappers/dotnet/README.md` carries the ordered list of what the first real CI run has to
confirm.

## Using the SDK from a Windows app

The core is portable C++17 with an abstract Transport interface — Windows support is a
port of the thin platform edges, not a rewrite:

1. **Winsock transport**: `transport.cpp`'s POSIX socket calls map to Winsock2
   (`WSAStartup`, `SOCKET`, `closesocket`; `TCP_NODELAY` exists unchanged). The
   self-pipe wake mechanism becomes a loopback socketpair or event object.
2. **Job store durability**: POSIX `fsync` → `FlushFileBuffers`/`_commit`; atomic
   rename semantics via `ReplaceFile`.
3. **Serial transport** (later): COM ports via `CreateFile`/`SetCommState`.

Everything else — encoder, parser, state machine, profiles, queue addon — is
platform-neutral standard C++ and compiles as-is (MSVC or clang-cl).

### Consumption paths on Windows

- **`printerdriver.dll` (C ABI)** — any Windows language binds it directly:
  C#/.NET via `[DllImport]`/P-Invoke, C++ natively, Node via N-API/ffi, Python via
  ctypes. The C ABI is the stable boundary; enums and the tri-state result carry over
  unchanged.
- **.NET wrapper** ([`wrappers/dotnet/`](../wrappers/dotnet/), delivered 2026-08-08) —
  idiomatic C# (`async`/`await` over the event callbacks, `enum` mirrors,
  `IAsyncEnumerable<JobEvent>`), shipped as a **NuGet package** with the native DLL under
  `runtimes/win-x64/native/` (and `win-arm64` later), MIT-licensed. The managed side is
  verified; the DLL that would go under `runtimes/` has not been built yet.
- **`pdctl.exe`** — the CLI builds on Windows for scripts, diagnostics, and probes.

### The important Windows-specific point

On Windows the SDK **bypasses the Windows print spooler entirely** — the app talks to
the SDK, which talks raw TCP 9100 (or serial/USB) to the printer. The printer is *not*
installed as a Windows printer for this path. That is deliberate: the research
([brief.md](brief.md), [device-database.md](device-database.md) case E) documents that
`JOB_STATUS_COMPLETE` from the spooler can mean "handed to the port monitor", not
"printed" — the exact false-success this SDK exists to eliminate. A Windows app using
the SDK gets the same `GS ( H`-fenced tri-state truth as every other platform, with
`completionAuthority = physical_printer` instead of `SERVER_COMPLETED`.

## Distribution summary

| Artifact | Channel |
|---|---|
| Swift package (iOS + macOS) | SwiftPM via git tag (Swift Package Index-ready) |
| Kotlin/Android AAR | Maven Central / GitHub Packages (`maven-publish`, MIT POM) |
| Dart/Flutter package | pub.dev |
| .NET wrapper + native DLL | NuGet (`runtimes/` layout) |
| C/C++ | CMake from source; vcpkg candidate later |
