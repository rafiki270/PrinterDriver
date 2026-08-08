# Brief: Getting Real Feedback from Thermal Printers on Finished Jobs

## Project goal

The operational symptom behind all of this: **kitchen receipts sometimes don't get
printed, staff resend them, and the original then prints later** — it was buffered
somewhere in the stack the whole time — producing duplicate tickets and duplicate food.

The fix this project builds: a **native printing SDK for iOS and Android with a shared
C++ core**, so all printing logic exists exactly once — one point of possible failure —
with thin wrappers for any other system later. Design goals:

- **ease of use** — minimal API, sensible defaults;
- **as much feedback as possible through a single point**, using closed enums defined in
  the core, so no wrapper can ever have a status that is "not implemented" — unknown and
  unsupported are explicit enum values, never missing callbacks;
- **a printing interface as standard as possible** — standard receipt semantics over a
  conservative standard ESC/POS subset, vendor quirks hidden behind capability profiles;
- **works with the most standard printers** — the printers already implemented in our
  existing POS apps, plus the Xprinter XP-S260M studied here.

The SDK serves **multiple in-house apps** — the existing web-based POS monorepo and our
native POS suite alike — so nothing app-specific may live inside it: payloads are opaque,
document-to-printer routing stays app-side.

Full SDK design: [docs/sdk-spec.md](sdk-spec.md). The research below is the protocol
foundation the core is built on.

## Problem

We can send receipts to our thermal printers (Xprinter XP-S260M), but we currently have
almost no way to know whether a job actually finished. The signals we get today only tell
us that bytes were handed off to the next layer in the stack — not that the printer did
anything with them.

- A successful TCP write only means the OS accepted the bytes into a socket buffer.
- A TCP acknowledgement only means the network delivered the bytes somewhere — not that
  the printer's print engine consumed them.
- CUPS defines backend "success" as *the file being successfully transmitted to the
  device or remote server* — nothing about the paper actually printing.
- Windows print spooling explicitly documents that `JOB_STATUS_COMPLETE` can be set
  before the job has actually printed, and that port monitors without "TrueEndOfJob"
  support mark jobs as printed immediately after submission.

So "the print call didn't error" has essentially no correlation with "a receipt came out
of the printer." That's the core issue driving this investigation.

## Is it actually fixable?

**Yes — this is not a dead end.** ESC/POS (the command language most receipt printers,
including ours, speak) has two under-used mechanisms that give real, ordered evidence
that print data was processed by the engine, not just accepted by a buffer:

1. **`GS ( H` Function 48** — a "process ID" marker. You send your receipt, then append
   a 4-byte marker (e.g. `P123`). Because the printer processes commands strictly in
   order, when it prints the receipt and reaches the marker, it sends that same marker
   back to us. Receiving it proves everything before it in the stream has finished
   printing.
2. **`GS r 1`** — a simpler fallback. Placed right after the receipt data, its response
   is only sent once the preceding print operation has completed. No job ID, and only
   safe with one job in flight at a time, but Epson explicitly documents it as usable for
   recognizing print completion.

The catch: cheap ESC/POS-*compatible* printers (which is what "ESC/POS compatible" often
really means) implement an inconsistent subset of the spec. We do **not** know yet which
of these our XP-S260M actually supports over which interface (LAN / serial / USB). That
has to be tested directly — it's a 5-minute test, not a research question.

## What we can honestly claim, at each level

| Level | Evidence | What we can honestly claim |
|---|---|---|
| Transport accepted | `send()`, TCP ACK, USB write | Bytes reached a buffer somewhere |
| Printer currently healthy | `DLE EOT`, ASB | Online, paper present, cover closed, no current cutter error |
| Ordered receipt completion | `GS r 1` or `GS ( H` response | Preceding print data was processed by the print engine |
| Cut command processed | Ordered marker after cut | Firmware processed the cut command |
| Cut reported fault-free | Ordered marker + cutter status | No cutter error was reported after processing |
| Paper physically emerged | Exit sensor (hardware) | Paper moved through the exit |
| Receipt contains readable content | QR/optical verification (hardware) | Physical paper exists with identifiable printed content |
| Receipt was taken | Presenter/taken sensor (hardware) | Someone removed the receipt |

A pure software fix gets us to: *"all preceding receipt data completed, the cut command
was processed, and the printer reports no hardware fault."* It cannot prove a dirty
printhead didn't leave a white stripe, that paper was loaded thermal-side-out, or that the
receipt separated cleanly. Those require a physical sensor or camera — see
[Physical verification options](techspec.md#physical-verification-options) in the tech
spec if we ever need that level for critical (e.g. kitchen) tickets.

## Feasibility summary

| Feedback | Can we detect it? |
|---|---|
| Data sent to printer | ✅ |
| Printer online | ✅ |
| Paper present | ✅ |
| Cover open | ✅ |
| Printer error | ✅ |
| Receipt finished printing | ✅ **Confirmed** — `GS ( H` ack received in probe (2026-08-08) |
| Cutter error | ✅ |
| Cut command processed | ✅ Very likely — `GS ( H` confirmed working; cut sequence itself not yet exercised |
| Blade physically cut the paper | ⚠️ Not absolutely — needs cutter-status check as a proxy |
| Text visibly printed correctly | ❌ Not without a sensor/camera |

## What changes architecturally

The fundamental shift is: **talk to the printer bidirectionally**, instead of firing
bytes at port 9100 (or through CUPS) and forgetting about them. That means:

- A small local **printer agent** (e.g. on the Raspberry Pi already near the printers)
  owns each printer connection exclusively — no other process writes to it directly.
- Every job gets a durable state machine (`QUEUED` → ... → `PRINT_CONFIRMED` →
  `CUT_COMMAND_PROCESSED` → `DONE_SOFTWARE`), not a boolean `printed`.
- Crucially, an explicit **`UNKNOWN`** state exists for the case where we sent bytes but
  lost the acknowledgement (crash, network drop) before recording it. This case is
  fundamentally unavoidable with any ephemeral raw ESC/POS acknowledgement — see
  [Exactly-once printing is impossible](techspec.md#exactly-once-printing-is-still-impossible-with-an-ephemeral-raw-acknowledgement)
  — so `UNKNOWN` jobs must go to a human/operator, never an automatic retry, especially
  for kitchen tickets where a duplicate is as bad as a missing ticket.

Full design: [docs/techspec.md](techspec.md).

## Immediate next step — ✅ done (2026-08-08)

The capability probe was run against our XP-S260M at `192.168.1.101` over LAN, with no
CUPS queues configured and exclusive access. Result: **best case** — `GS ( H` process-ID
completion acknowledgement is supported, and `DLE EOT` status and `GS r 1` both respond
on the same socket. Full output and interpretation:
[docs/testing-plan.md](testing-plan.md#-result-our-xp-s260m-unit--tested-2026-08-08).

Originally this step required stopping CUPS and any other client first (a shared
connection makes responses ambiguous), plus the printer's LAN IP — kept here for reruns
on other units/firmware.

Next: the fault-injection matrix and cut-sequence test from the testing plan, and the SDK
build itself per [docs/sdk-spec.md](sdk-spec.md).

## Recommended order of attack

1. ✅ **Done 2026-08-08** — Run the probe against the XP-S260M's LAN port with CUPS and
   all POS clients stopped.
2. ✅ **Confirmed — this is the chosen architecture.** `GS ( H` works → build the printing
   core around process-ID acknowledgements.
3. ~~If only `GS r 1` works~~ (not needed — but this unit supports `GS r 1` too, so it
   remains a validated fallback: one job at a time, continuous ASB, cutting treated as a
   separate, weaker-confidence step.)
4. If LAN is write-only (no backchannel) → repeat the same test through RS-232, which the
   XP-S260M also supports and which is a more reliable full-duplex channel than some LAN
   print-server implementations.
5. Contact Xprinter for the exact "one ticket one control" (`ESC v` / `ESC x`) protocol
   spec, or inspect their SDK/OPOS-JPOS service object — this may be a better-supported
   idle/cut signal than Epson's extensions on this specific hardware.
6. Introduce the persistent `UNKNOWN` state before enabling any retry logic.
7. For mission-critical kitchen tickets, add a visible job ID to every ticket immediately,
   and consider QR-code verification at the paper exit if duplicate/missing tickets are
   costly enough to justify hardware.

The most likely outcome is either strong raw completion via `GS ( H`, or reasonable print
completion via `GS r 1` plus Xprinter's idle/cutter status. The probe will tell us which
architecture is justified rather than us guessing from socket behavior.

**Outcome (2026-08-08): the probe confirmed the strong path — `GS ( H` raw completion
works on our XP-S260M over LAN, and `GS r 1` works as fallback.**
