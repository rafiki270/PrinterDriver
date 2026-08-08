# SDK Spec: PrinterDriver — Cross-Platform Receipt Printing SDK

Companion to [docs/brief.md](brief.md) (problem research summary),
[docs/techspec.md](techspec.md) (protocol-level detail),
[docs/testing-plan.md](testing-plan.md) (hardware capability probe), and
[docs/api.md](api.md) (the public interface). This document defines what we are
actually building.

---

## 1. The operational problem driving this

Kitchen receipts sometimes don't get printed. Staff can't tell (the software said
"success"), so they send the ticket again — and then the original prints later, because it
was sitting in a buffer somewhere in the stack the whole time. Result: **duplicate kitchen
tickets, duplicate food production.**

The research in [brief.md](brief.md)/[techspec.md](techspec.md) explains why: every layer
between the POS app and the paper (socket write, TCP ACK, CUPS, Windows spooler, the
printer's own 128 KB input buffer) reports "success" when it has merely handed bytes to
the next layer. Nothing in the current stack confirms the print engine actually ran.

On top of that, today printing is implemented separately per platform/client, so the same
failure gets handled (or mishandled) differently in several places, and diagnosis is
guesswork.

## 2. What we are building

A **native printing SDK for iOS and Android with a shared C++ core**.

- **All printing logic lives in the C++ core** — transports, protocol encoding, status
  parsing, job state machine, retry/duplicate policy. One implementation means **one
  point of possible failure**: one place bugs can exist, one place to fix them, identical
  behavior on every platform.
- **Platform wrappers are thin bindings, not implementations.** iOS (Swift) and Android
  (Kotlin) first; wrappers for any other system later (React Native, Flutter, .NET,
  Node, plain C ABI). A wrapper that contains logic is a bug.
- The same core should be reusable for a Linux/Raspberry Pi printer agent
  (see [techspec.md §5](techspec.md#5-recommended-architecture)) — that's the payoff of
  putting everything in portable C++.

## 3. Design goals

1. **Ease of use.** Minimal API surface: connect, print, observe. Sensible defaults.
   A simple receipt should be a few lines of Swift/Kotlin.
2. **As much feedback as possible, through a single point, enum-based.** One status/event
   channel per job and per printer. All states, events, and errors are **closed enums
   defined in the core** and re-exported by every wrapper — so there is never a situation
   where a platform "didn't implement" a status. When something genuinely can't be known
   (printer has no backchannel, feature unsupported), that is itself an explicit enum
   value (`UNKNOWN`, `UNSUPPORTED`), never a missing callback or a silent success.
3. **Printing interface as standard as possible.** The public API models standard receipt
   semantics (text, styling, alignment, feed, cut, raster images, barcodes/QR, cash
   drawer). Raster deserves equal billing with text: our existing POS renders receipts
   to canvas and prints images today (§10). The wire default is a **conservative
   standard ESC/POS subset** — the dialect
   the widest range of cheap "ESC/POS compatible" printers actually implements. Vendor
   quirks and extensions live behind capability profiles inside the core, never in the
   public API.
4. **Works with the most standard printers.** Support target = the printers already
   deployed/implemented in our existing POS apps (see §9) plus the Xprinter XP-S260M
   studied in the research docs. Cheap ESC/POS-compatible hardware is the norm, not the
   exception — the SDK must degrade honestly (see confidence levels, §5) rather than
   require premium hardware.

## 4. Architecture

```
┌────────────────────────────────────────────────────────┐
│                    Platform wrappers                   │
│  iOS (Swift)   Android (Kotlin/JNI)   later: RN,       │
│                                       Flutter, .NET,   │
│                                       Node, C ABI      │
│  — thin bindings only, zero printing logic —           │
├────────────────────────────────────────────────────────┤
│                      C++ core                          │
│  ├─ Public API (jobs, printers, events)                │
│  ├─ Job state machine + persistent job store           │
│  ├─ Queue: one active job per printer                  │
│  ├─ ESC/POS encoder (standard subset + profile quirks) │
│  ├─ Response parser (interleaved stream: DLE EOT,      │
│  │   GS r, GS ( H frames, ASB frames, vendor bytes)    │
│  ├─ Capability profiles (per model/firmware)           │
│  └─ Transports: TCP 9100 │ Bluetooth │ USB │ Serial    │
└────────────────────────────────────────────────────────┘
```

Core responsibilities worth calling out:

- **Full-duplex connection ownership.** The core owns each printer connection exclusively
  and runs a continuous reader, exactly as required by
  [techspec.md §5](techspec.md#5-recommended-architecture). The parser must handle the
  interleaved incoming stream: single-byte real-time statuses, single-byte `GS r`
  responses, ASB status frames, `GS ( H` frames (`37 22 … 00`), and unknown vendor bytes.
- **One active job per printer**, with completion fences between jobs
  ([techspec.md §3](techspec.md#3-escpos-command-reference-used-in-this-design)).
- **Persistent job store** so crash recovery can classify in-flight jobs as `UNKNOWN`
  instead of losing or double-printing them.

## 5. The single feedback point

Per-job event stream plus a per-printer device event stream, both fed by the same core
enums. Draft shape (names illustrative):

```cpp
// Job lifecycle — mirrors techspec §5.1 exactly.
enum class JobState {
  Queued,
  PreflightOk,
  SendStarted,
  BytesSent,
  PrintConfirmed,
  CutCommandProcessed,
  DoneSoftware,
  PhysicallyVerified,   // reserved for future hardware verification (techspec §8)
  FailedKnown,
  Unknown,              // sent, no acknowledgement — requires operator decision
};

// What evidence backs the current claim — the evidence ladder from brief.md.
enum class ConfidenceLevel {
  TransportAccepted,    // bytes reached a buffer somewhere
  PrinterHealthy,       // DLE EOT / ASB say online, paper, cover closed
  PrintConfirmed,       // ordered completion fence (GS ( H or GS r 1) came back
  CutProcessed,         // ordered marker after cut acknowledged
  CutFaultFree,         // cutter status clear after ordered fence
  PhysicallyVerified,   // future: exit sensor / QR scan
};

// Device events — from ASB + status queries.
enum class DeviceEvent {
  Online, Offline,
  CoverOpen, CoverClosed,
  PaperOut, PaperNearEnd, PaperOk,
  CutterError, RecoverableError, UnrecoverableError,
  ConnectionLost, ConnectionRestored,
};

enum class FailureReason {
  None,
  TransportUnreachable,
  PreflightCoverOpen, PreflightPaperOut, PreflightHardwareError,
  TimeoutAwaitingCompletion,
  CutterFault,
  Unsupported,          // requested feature not available on this printer
  Unknown,
};
```

Rules:

- Every wrapper exposes **all** enum members. Adding a member is a core change that flows
  to all platforms at once.
- A job's terminal state always carries its `ConfidenceLevel`, so callers can distinguish
  "done because the printer confirmed it" from "done because bytes were sent and this
  printer has no backchannel." The SDK never upgrades confidence on its own.
- `Unknown` is a first-class outcome, surfaced to the app for an operator decision — the
  SDK never auto-retries out of `Unknown`
  ([techspec.md §7](techspec.md#7-exactly-once-printing-is-still-impossible-with-an-ephemeral-raw-acknowledgement)).

## 6. Duplicate prevention (the actual kitchen problem)

Straight from the research, now enforced by the core:

- **Idempotency keys.** Every print call takes a caller-supplied key (order/ticket UUID).
  Re-submitting the same key returns the existing job and its state — it does not print
  again. This alone kills the "resend because it looked stuck" duplicate.
- **Explicit `forceReprint`.** Printing the same key again is a distinct, deliberate
  operation, and the core marks the ticket visibly:
  `*** REPRINT / POSSIBLE DUPLICATE ***`, `PRINT ATTEMPT: n`.
- **Visible stable ID on every ticket** (and optionally a QR of it), so staff can
  reconcile paper against orders.
- **No automatic retry from `Unknown`** — for kitchen tickets a duplicate is as bad as a
  missing ticket.

## 7. Transports

| Transport | Platforms | Notes |
|---|---|---|
| TCP 9100 (LAN/Wi-Fi) | all | Primary. Backchannel must be probed per printer — full-duplex at the network level does not guarantee the printer's LAN module forwards status bytes ([techspec.md §4](techspec.md#4-which-interface-to-use)). Keep socket open after send. |
| Bluetooth | iOS (BLE/MFi), Android (Classic SPP + BLE) | Very common on cheap mobile receipt printers. Feedback semantics must be probed the same way as LAN. |
| USB | Android (USB host API) | iOS generally N/A. Prefer direct endpoint access over OS print paths. |
| Serial RS-232 | agent/desktop scenarios | Most reliable full-duplex channel on the XP-S260M class; not applicable on phones, kept in core for the Linux agent reuse. |

Never print on one interface and poll status on another — separate buffers, no ordering
guarantee ([techspec.md §4](techspec.md#4-which-interface-to-use)).

## 8. Capability profiles

Per model/firmware, determined by the probe from
[testing-plan.md](testing-plan.md) and shipped as data with the SDK:

- which completion mechanism works: `GS ( H` / `GS r 1` / vendor (e.g. Xprinter
  `ESC v` / `ESC x`) / none;
- cut command variant and whether a post-cut fence is trustworthy;
- ASB support;
- buffer size, timing characteristics;
- quirk flags for dialect deviations.

The profile decides which `ConfidenceLevel` a job on that printer can ever reach — and
the SDK reports exactly that, honestly, instead of pretending.

## 9. Printer support matrix

Support target: every printer currently implemented/used in our existing POS apps, plus
the XP-S260M. Filled from a scan of the existing POS monorepo run 2026-08-08:

| Printer | Protocol today | Notes for the SDK |
|---|---|---|
| **Epson TM-T20III** | ESC/POS over TCP 9100 | The monorepo's primary reference model — default 576 dots/line (`const/defaultPrinterDotsPerLine.ts`). |
| **Epson TM-T88VI and newer TM/TM-i** | ePOS-Print (SOAP/XML over HTTP) | Job-level XML responses (`success="true"`); WebConfig auto-configuration exists but has unfinished digest-auth and model-detection TODOs. |
| **Rongta ESC/POS models** | ESC/POS over TCP 9100 | Known quirk: cuts too early — today compensated by hardcoding 6 newlines before the cut (`canvasToEscposCommands.ts`); CP852 code page. Exactly what capability profiles (§8) exist for. |
| **Generic ESC/POS thermal printers** | ESC/POS over TCP 9100 | The DB `PrinterType` enum is just `escpos` \| `epos`, with `escpos` the default — the fleet is generic-ESC/POS-shaped. |
| **Xprinter XP-S260M** | ESC/POS over TCP 9100 (also serial/USB) | Partial cutter, 128 KB buffer. Research subject of [techspec.md](techspec.md). **Capability probe run 2026-08-08 (our unit, `192.168.1.101`, LAN): `GS ( H` process-ID completion SUPPORTED; `DLE EOT` and `GS r 1` also respond — primary completion path is `GS ( H`** ([testing-plan.md](testing-plan.md#-result-our-xp-s260m-unit--tested-2026-08-08)). |
| **Partner Tech RP-110** (Sewoo-designed) | ESC/POS (USB/serial/LAN variants) | Deployed with our native POS suite (photo-identified unit, 24 V thermal receipt printer, Partner Tech Europe import). ESC/POS-compatible per vendor; completion-mechanism probe pending — run `scripts/printer_probe.py` against it. |

Paper widths in use today: 384 / 504 / 576 dots per line (58 mm and 80 mm papers at
203 dpi / 8 dots-per-mm).

## 10. Current state in the existing POS stack (what the SDK replaces)

From the 2026-08-08 scan. Main code: `organization/web/pos/src/utilities/device/printer/`
(web POS printer stack) and `organization/native/pos-shell/modules/tcpModule/` (native
TCP transport). Printer entity/routing:
`organization/contember/api/model/entities/commerce/pointOfSale/Printer.ts`.

**Send path today:** React components render the receipt → canvas (`renderToCanvas()`) →
dithered raster → ESC/POS image commands (`canvasToEscposCommands.ts`; tall images split
at 1024 px because Epson chokes on very tall rasters) → per-printer-host serial queue
(`enqueue-task`) → transport:

- **ESC/POS:** raw TCP to `<ip>:9100` via the native TcpModule, sent in 1 KB chunks with
  empirically tuned 9–21 ms inter-chunk delays ("some printers may need a little time
  between commands");
- **ePOS:** HTTP POST of SOAP XML to `http://<ip>/cgi-bin/epos/service.cgi?devid=local_printer`.

**"Feedback" today — and the likely duplicate-ticket mechanism:** after sending, the
ESC/POS path issues a real-time status query (`0x10 0x04`, i.e. `DLE EOT`) and waits up
to 5 s. Per
[techspec.md §3.3](techspec.md#33-dle-eot--useful-but-not-as-a-completion-acknowledgement),
`DLE EOT` is answered immediately, out of order with buffered print data — so this check
can report success while the receipt is still sitting in the printer's buffer, and can
"fail" by timeout while the receipt goes on to print anyway. With no retry logic and
kitchen tickets buffered behind a 128 KB input buffer, that is precisely the observed
symptom: ticket looks failed → staff resend → the buffered original prints later →
duplicate. The ePOS path parses `success="true"` from the XML; no retry on parse failure.

**Pain points found in code, mapped to what the SDK fixes:**

| Today (evidence in monorepo) | SDK answer |
|---|---|
| Android TCP module is an unimplemented stub — printing is effectively iOS-only native (`tcpModule/android/.../tcp_module.kt` TODO) | The C++ core's transport runs identically on both platforms — Android gets real printing for free |
| `DLE EOT` misused as a completion check, 5 s timeout, `@TODO: handle errors of print functions` (`EscposPrinter.tsx`, `Printer.ts`) | Ordered `GS ( H` completion fences + `JobState`/`ConfidenceLevel` + first-class `Unknown` |
| No print-job status logging (`printPendingTicketsAndCancellations.ts` `@TODO`) | Persistent job store with full state history |
| Rongta early-cut hardcoded 6-newline workaround | Capability-profile quirk flag |
| Empirical 9–21 ms chunk pacing scattered in app code | Flow control owned by the core, tuned per profile |
| Network discovery is a stub returning fake devices (`shared/std/.../scanNetwork.ts`) | SDK discovery (open question §11.3) |
| Broken IPv6 heuristic in ePOS URL building (`EposPrinter.tsx`) | Core owns addressing and transports |
| CP852 hardcoded; emoji/non-Latin filtered | Encoding per capability profile |
| Duplicate warnings only on manual reprint of some doc types (`const/reprintDuplicateWarningPrintableTypes.ts`) | Idempotency keys, explicit `forceReprint`, attempt counter on every ticket (§6) |

**Requirements this adds to the SDK:**

- **Raster is a first-class job type.** The existing POS prints receipts as rendered images, not
  ESC/POS text — so raster jobs (dithering, width fitting to 384/504/576 dots, banding
  for tall images) matter as much as text primitives, and the existing
  one-job-at-a-time-per-printer queue semantics must be preserved (the core already
  requires this for completion fencing).
- **Job payloads stay opaque; routing stays app-side.** The POS distinguishes 15
  printable types (receipt, guestCheck, ticket, customerTicket, cancellationTicket,
  seatChange, customerTabsMerge, closingSummary, customerPager, payment, paidAndClosed,
  startPreparation, internalMessage, image, plus a card-terminal receipt type) routed to
  per-role printers
  (receiptPrinter / ticketPrinters / customerPagerPrinter / paymentAndClosurePrinter).
  The SDK prints jobs; which printer gets which document type remains the app's decision.

## 11. Open questions

1. **iOS binding mechanism** — direct Swift/C++ interop (Xcode 15+) vs. an Objective-C++
   shim. Interop is cleaner if our minimum toolchain allows it.
2. **Android binding** — hand-rolled JNI vs. generated bindings (djinni-style). Decide
   after the API surface stabilizes.
3. **Discovery** (mDNS/network scan/BT scan) — in core or per-platform? Leaning core for
   LAN discovery, platform-assisted for Bluetooth.
4. **Persistence backend** for the job store on mobile (SQLite in core vs. host-provided
   storage callback).
5. **Whether the Linux/RPi agent ships from this repo** using the same core (probable
   yes — it is the strongest argument for the C++ core).
6. **ePOS-Print protocol support** — the existing POS stack drives Epson TM-T88VI+ via ePOS
   (SOAP/HTTP) today. Does v1 include an ePOS transport in the core, or are those
   printers driven through their ESC/POS mode instead (most Epson TM models speak both)?

## 12. Non-goals (for v1)

- Physical print verification hardware (exit sensors, QR scanners) — designed for in the
  state machine (`PhysicallyVerified`), not built now
  ([techspec.md §8](techspec.md#8-physical-verification-options)).
- Non-receipt printing (label/ZPL, A4/CUPS).
- Cloud print routing — the SDK talks to printers directly; fleet/cloud orchestration can
  sit on top later.
